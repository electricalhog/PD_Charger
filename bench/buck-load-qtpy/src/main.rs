//! Buck converter as a power-target load (QT Py RP2040).
//!
//! The host (`bu load`, `bu bench`) commands the load over the QT Py's USB
//! CDC console (src/console.rs): every `cmd` line carries a load_link::Command
//! (arm, power target, contract limits) and gets one `TEL` telemetry line
//! back.  The load_control law runs at 1 kHz and keeps its own limits; with
//! no command for 250 ms it faults with every driver off.  (The I2C link to
//! the PD sink was dropped 2026-10-06: this QT Py's STEMMA pads cannot drive
//! low, so it could never ACK.)  Pin map, scales and the gate-driver hazard:
//! src/board.rs.
#![no_std]
#![no_main]

mod board;
mod console;

use core::cell::RefCell;

use embassy_executor::Spawner;
use embassy_rp::adc::{self, Adc, Channel};
use embassy_rp::gpio::{Level, Output, Pull};
use embassy_rp::peripherals::USB;
use embassy_rp::{bind_interrupts, usb};
use embassy_sync::blocking_mutex::Mutex;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_time::{Duration, Instant, Ticker};
use load_control::{Controller, Measurement, Params};
use load_link::{Command, Telemetry};

bind_interrupts!(struct Irqs {
    ADC_IRQ_FIFO => adc::InterruptHandler;
    USBCTRL_IRQ => usb::InterruptHandler<USB>;
});

/// Raw readings and link counters for the USB console and the BOOTSEL gate.
#[derive(Clone, Copy, Default)]
pub struct Diag {
    /// At least one control period has run (telemetry is real).
    pub valid: bool,
    /// ADC averages [counts]: GPIO26, GPIO27, GPIO28, GPIO29.
    pub raw26: f32,
    pub raw27: f32,
    pub raw28: f32,
    pub raw29: f32,
    /// Current-sense zero [counts], tracked while the drivers are disabled.
    pub izero: f32,
    /// The active phase's driver is enabled.
    pub enable: bool,
    /// Host commands accepted / rejected (unparsable).
    pub frames: u32,
    pub bad_frames: u32,
}

pub struct Shared {
    pub cmd: Option<Command>,
    pub cmd_at: Instant,
    pub telemetry: Telemetry,
    pub diag: Diag,
}

pub static SHARED: Mutex<CriticalSectionRawMutex, RefCell<Shared>> = Mutex::new(RefCell::new(Shared {
    cmd: None,
    cmd_at: Instant::from_ticks(0),
    telemetry: Telemetry {
        seq: 0,
        state: load_link::State::Off,
        fault: load_link::Fault::None,
        vin_mv: 0,
        vout_mv: 0,
        iout_ma: 0,
        pout_mw: 0,
        duty_permille: 0,
        temp_mv: 0,
    },
    diag: Diag {
        valid: false,
        raw26: 0.0,
        raw27: 0.0,
        raw28: 0.0,
        raw29: 0.0,
        izero: 0.0,
        enable: false,
        frames: 0,
        bad_frames: 0,
    },
}));

/// The RP2040 may reset (BOOTSEL, reflash) only while the low-side FETs it
/// would turn on can do no harm: driver off, the higher tap under 4.5 V (no
/// gate rail) and the lower one under 0.3 V (+OUT discharged).  Taken as
/// higher/lower so it holds whichever tap is VIN (board.rs).
pub fn bootsel_allowed(s: &Shared) -> Result<(), &'static str> {
    let (a, b) = (s.telemetry.vin_mv, s.telemetry.vout_mv);
    let (lo, hi) = if a < b { (a, b) } else { (b, a) };
    if !s.diag.valid {
        Err("no measurement yet")
    } else if s.diag.enable {
        Err("load running")
    } else if hi > 4500 {
        Err("a tap is above 4.5 V: the gate rail may be live")
    } else if lo > 300 {
        Err("both taps above 0.3 V: +OUT may be charged")
    } else {
        Ok(())
    }
}

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let p = embassy_rp::init(Default::default());

    // Every driver off before anything else: a floating DISABLE enables the
    // driver, and a low PWM then turns its low side on (board.rs).
    let disable2 = Output::new(p.PIN_3, Level::High);
    let disable13 = Output::new(p.PIN_5, Level::High);
    let pwm13 = Output::new(p.PIN_4, Level::Low);
    let fan = Output::new(p.PIN_20, Level::Low);
    // Phases 1 and 3 (one pin pair, board.rs) stay parked for the life of
    // the program.
    core::mem::forget((disable13, pwm13));

    #[cfg(feature = "power-stage")]
    let stage = power::Stage::new(p.PWM_SLICE3, p.PIN_6, disable2, fan);
    #[cfg(not(feature = "power-stage"))]
    let stage = power::Stage::new(Output::new(p.PIN_6, Level::Low), disable2, fan);

    let adc = Adc::new(p.ADC, Irqs, adc::Config::default());
    let sense = Sense {
        adc,
        vin: Channel::new_pin(p.PIN_26, Pull::None),
        vout: Channel::new_pin(p.PIN_27, Pull::None),
        iout: Channel::new_pin(p.PIN_28, Pull::None),
        temp: Channel::new_pin(p.PIN_29, Pull::None),
    };
    spawner.spawn(control_task(sense, stage).unwrap());

    console::start(spawner, usb::Driver::new(p.USB, Irqs));
}

/// Stores a command from the host (console `cmd`); the control task picks it
/// up on its next 1 ms period and judges its age against the watchdog.
pub fn accept_command(cmd: Command) {
    SHARED.lock(|s| {
        let mut s = s.borrow_mut();
        s.cmd = Some(cmd);
        s.cmd_at = Instant::now();
        s.diag.frames = s.diag.frames.wrapping_add(1);
    });
}

struct Sense {
    adc: Adc<'static, adc::Async>,
    vin: Channel<'static>,
    vout: Channel<'static>,
    iout: Channel<'static>,
    temp: Channel<'static>,
}

/// Average of `n` conversions (~2 us each at the 48 MHz ADC clock) [counts].
async fn avg(adc: &mut Adc<'static, adc::Async>, ch: &mut Channel<'static>, n: u32) -> f32 {
    let mut sum = 0u32;
    for _ in 0..n {
        sum += adc.read(ch).await.unwrap_or(0) as u32;
    }
    sum as f32 / n as f32
}

#[embassy_executor::task]
async fn control_task(mut sense: Sense, mut stage: power::Stage) -> ! {
    let mut ctrl = Controller::new(Params::BENCH_10R_10W, power::PRESENT);
    let mut ticker = Ticker::every(Duration::from_millis(1));
    let mut temp_raw = 0.0f32;
    let mut izero: Option<f32> = None;
    let mut off_ms: u32 = 0;
    let mut tick: u32 = 0;
    loop {
        ticker.next().await;
        tick = tick.wrapping_add(1);
        let vin = avg(&mut sense.adc, &mut sense.vin, 4).await;
        let vout = avg(&mut sense.adc, &mut sense.vout, 4).await;
        // 16 samples: one phase of 1 uH at 438 kHz ripples by amps, and the
        // samples are not synchronised to the PWM.
        let iout = avg(&mut sense.adc, &mut sense.iout, 16).await;
        if tick % 100 == 0 || tick == 1 {
            temp_raw = avg(&mut sense.adc, &mut sense.temp, 4).await;
        }

        // Current zero: with the driver disabled no current can flow, so
        // track the amplifier's offset then and hold it while running.
        off_ms = if stage.enabled() { 0 } else { off_ms.saturating_add(1) };
        let zero = match izero {
            None => iout,
            Some(z) if off_ms > 50 => z + (iout - z) * 0.01,
            Some(z) => z,
        };
        izero = Some(zero);

        let m = Measurement {
            vin_mv: board::volts_mv(vin),
            vout_mv: board::volts_mv(vout),
            iout_ma: board::amps_ma(iout, zero),
        };

        let (cmd, age_ms) = SHARED.lock(|s| {
            let s = s.borrow();
            (s.cmd, Instant::now().saturating_duration_since(s.cmd_at).as_millis() as u32)
        });
        let out = ctrl.step(m, cmd.map(|c| (c, age_ms)), 1);
        stage.apply(out.enable, out.duty_permille);

        SHARED.lock(|s| {
            let mut s = s.borrow_mut();
            s.telemetry = Telemetry {
                seq: cmd.map_or(0, |c| c.seq),
                state: out.state,
                fault: out.fault,
                vin_mv: m.vin_mv.min(u16::MAX as u32) as u16,
                vout_mv: m.vout_mv.min(u16::MAX as u32) as u16,
                iout_ma: m.iout_ma.clamp(i16::MIN as i32, i16::MAX as i32) as i16,
                pout_mw: out.pout_mw.min(u16::MAX as u32) as u16,
                duty_permille: out.duty_permille,
                temp_mv: board::pin_mv(temp_raw) as u16,
            };
            let d = &mut s.diag;
            d.valid = true;
            d.raw26 = vin;
            d.raw27 = vout;
            d.raw28 = iout;
            d.raw29 = temp_raw;
            d.izero = zero;
            d.enable = stage.enabled();
        });
    }
}

#[cfg(not(feature = "power-stage"))]
mod power {
    use embassy_rp::gpio::Output;

    /// Telemetry-only build: the active phase stays parked too.
    pub const PRESENT: bool = false;

    pub struct Stage {
        _pwm: Output<'static>,
        _disable: Output<'static>,
        _fan: Output<'static>,
    }

    impl Stage {
        pub fn new(pwm: Output<'static>, disable: Output<'static>, fan: Output<'static>) -> Self {
            Self { _pwm: pwm, _disable: disable, _fan: fan }
        }

        pub fn apply(&mut self, _enable: bool, _duty_permille: u16) {}

        pub fn enabled(&self) -> bool {
            false
        }
    }
}

#[cfg(feature = "power-stage")]
mod power {
    use embassy_rp::Peri;
    use embassy_rp::gpio::Output;
    use embassy_rp::peripherals::{PIN_6, PWM_SLICE3};
    use embassy_rp::pwm::{Config, Pwm};

    use crate::board;

    pub const PRESENT: bool = true;

    /// Phase 2 only: PWM on GPIO6 (slice 3 A), DISABLE on GPIO3.
    pub struct Stage {
        pwm: Pwm<'static>,
        cfg: Config,
        disable: Output<'static>,
        fan: Output<'static>,
        on: bool,
    }

    impl Stage {
        /// `disable` must already be high (main parks it first).
        pub fn new(
            slice: Peri<'static, PWM_SLICE3>,
            pin: Peri<'static, PIN_6>,
            disable: Output<'static>,
            fan: Output<'static>,
        ) -> Self {
            let mut cfg = Config::default();
            cfg.top = board::PWM_TOP;
            cfg.compare_a = 0;
            let pwm = Pwm::new_output_a(slice, pin, cfg.clone());
            Self { pwm, cfg, disable, fan, on: false }
        }

        pub fn apply(&mut self, enable: bool, duty_permille: u16) {
            if enable {
                self.cfg.compare_a = board::compare(duty_permille);
                self.pwm.set_config(&self.cfg);
                self.disable.set_low();
                self.fan.set_high();
            } else {
                // Both FETs off first, then park the PWM.
                self.disable.set_high();
                self.cfg.compare_a = 0;
                self.pwm.set_config(&self.cfg);
                self.fan.set_low();
            }
            self.on = enable;
        }

        pub fn enabled(&self) -> bool {
            self.on
        }
    }
}

/// Drivers off without trusting any HAL state: the three DISABLE pins to SIO,
/// driven high, then halt.  A hung loop with the PWM running would otherwise
/// keep switching open loop.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    cortex_m::interrupt::disable();
    const IO_BANK0_GPIO0_CTRL: usize = 0x4001_4004;
    const SIO_GPIO_OUT_SET: usize = 0xd000_0014;
    const SIO_GPIO_OE_SET: usize = 0xd000_0024;
    const FUNCSEL_SIO: u32 = 5;
    // SAFETY: fixed RP2040 register addresses; nothing else runs after this.
    unsafe {
        core::ptr::write_volatile(SIO_GPIO_OUT_SET as *mut u32, board::DISABLE_MASK);
        core::ptr::write_volatile(SIO_GPIO_OE_SET as *mut u32, board::DISABLE_MASK);
        for gpio in board::DISABLE_GPIOS {
            core::ptr::write_volatile((IO_BANK0_GPIO0_CTRL + 8 * gpio) as *mut u32, FUNCSEL_SIO);
        }
    }
    loop {
        cortex_m::asm::nop();
    }
}
