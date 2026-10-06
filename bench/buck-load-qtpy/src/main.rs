//! Buck converter as a power-target load (QT Py RP2040).
//!
//! The bench PD sink (NUCLEO-G431RB) is the I2C controller: every 20 ms it
//! writes a load_link::Command (arm, power target, contract limits) and reads
//! a load_link::Telemetry.  This firmware is the I2C target on the STEMMA QT
//! port (I2C1, GPIO22 SDA / GPIO23 SCL, address 0x55) and runs the
//! load_control law at 1 kHz.  Pin map and scales: src/board.rs.
#![no_std]
#![no_main]

mod board;

use core::cell::RefCell;

use embassy_executor::Spawner;
use embassy_rp::adc::{self, Adc, Channel};
use embassy_rp::gpio::Pull;
use embassy_rp::i2c_slave::{self, I2cSlave};
use embassy_rp::peripherals::I2C1;
use embassy_rp::{bind_interrupts, i2c};
use embassy_sync::blocking_mutex::Mutex;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_time::{Duration, Instant, Ticker};
use load_control::{Controller, Measurement, Params};
use load_link::{Command, Telemetry};
use panic_halt as _;

bind_interrupts!(struct Irqs {
    I2C1_IRQ => i2c::InterruptHandler<I2C1>;
    ADC_IRQ_FIFO => adc::InterruptHandler;
});

struct Shared {
    cmd: Option<Command>,
    cmd_at: Instant,
    telemetry: Telemetry,
}

static SHARED: Mutex<CriticalSectionRawMutex, RefCell<Shared>> = Mutex::new(RefCell::new(Shared {
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
}));

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let p = embassy_rp::init(Default::default());

    let mut link_config = i2c_slave::Config::default();
    link_config.addr = load_link::ADDRESS as u16;
    link_config.sda_pullup = true;
    link_config.scl_pullup = true;
    let link = I2cSlave::new(p.I2C1, p.PIN_23, p.PIN_22, Irqs, link_config);
    spawner.spawn(link_task(link).unwrap());

    let adc = Adc::new(p.ADC, Irqs, adc::Config::default());
    let sense = Sense {
        adc,
        iout: Channel::new_pin(p.PIN_29, Pull::None),
        vin: Channel::new_pin(p.PIN_28, Pull::None),
        vout: Channel::new_pin(p.PIN_27, Pull::None),
        ntc: Channel::new_pin(p.PIN_26, Pull::None),
    };

    #[cfg(feature = "power-stage")]
    let stage = power::Stage::new(
        p.PWM_SLICE4, p.PIN_24, p.PIN_25, p.PIN_20, p.PIN_5, p.PIN_6, p.PIN_4, p.PIN_3,
    );
    #[cfg(not(feature = "power-stage"))]
    let stage = power::Stage;

    spawner.spawn(control_task(sense, stage).unwrap());
}

/// I2C target: store each valid command, serve the latest telemetry.
#[embassy_executor::task]
async fn link_task(mut dev: I2cSlave<'static, I2C1>) -> ! {
    let mut buf = [0u8; 32];
    loop {
        match dev.listen(&mut buf).await {
            Ok(i2c_slave::Command::Write(len)) => {
                if len == 1 + Command::LEN && buf[0] == load_link::REG_COMMAND {
                    if let Ok(cmd) = Command::decode(&buf[1..len]) {
                        SHARED.lock(|s| {
                            let mut s = s.borrow_mut();
                            s.cmd = Some(cmd);
                            s.cmd_at = Instant::now();
                        });
                    }
                }
            }
            Ok(i2c_slave::Command::WriteRead(_)) | Ok(i2c_slave::Command::Read) => {
                // Only one readable register (REG_TELEMETRY).
                let frame = SHARED.lock(|s| s.borrow().telemetry.encode());
                let _ = dev.respond_and_fill(&frame, 0xFF).await;
            }
            Ok(i2c_slave::Command::GeneralCall(_)) | Err(_) => {}
        }
    }
}

struct Sense {
    adc: Adc<'static, adc::Async>,
    iout: Channel<'static>,
    vin: Channel<'static>,
    vout: Channel<'static>,
    ntc: Channel<'static>,
}

impl Sense {
    /// Average of 4 conversions (~2 us each at 48 MHz ADC clock).
    async fn read(adc: &mut Adc<'static, adc::Async>, ch: &mut Channel<'static>) -> u16 {
        let mut sum = 0u32;
        for _ in 0..4 {
            sum += adc.read(ch).await.unwrap_or(0) as u32;
        }
        (sum / 4) as u16
    }
}

#[embassy_executor::task]
async fn control_task(mut sense: Sense, mut stage: power::Stage) -> ! {
    let mut ctrl = Controller::new(Params::BENCH_10R_10W, power::PRESENT);
    let mut ticker = Ticker::every(Duration::from_millis(1));
    let mut ntc_raw = 0u16;
    let mut tick: u32 = 0;
    loop {
        ticker.next().await;
        tick = tick.wrapping_add(1);
        let vin = Sense::read(&mut sense.adc, &mut sense.vin).await;
        let vout = Sense::read(&mut sense.adc, &mut sense.vout).await;
        let iout = Sense::read(&mut sense.adc, &mut sense.iout).await;
        if tick % 100 == 0 {
            ntc_raw = Sense::read(&mut sense.adc, &mut sense.ntc).await;
        }
        let m = Measurement {
            vin_mv: board::volts_mv(vin),
            vout_mv: board::volts_mv(vout),
            iout_ma: board::amps_ma(iout),
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
                temp_mv: board::pin_mv(ntc_raw) as u16,
            };
        });
    }
}

#[cfg(not(feature = "power-stage"))]
mod power {
    /// No gate-driver pins are touched in this build.
    pub const PRESENT: bool = false;
    pub struct Stage;
    impl Stage {
        pub fn apply(&mut self, _enable: bool, _duty_permille: u16) {}
    }
}

#[cfg(feature = "power-stage")]
mod power {
    use embassy_rp::Peri;
    use embassy_rp::gpio::{Level, Output};
    use embassy_rp::peripherals::{PIN_3, PIN_4, PIN_5, PIN_6, PIN_20, PIN_24, PIN_25, PWM_SLICE4};
    use embassy_rp::pwm::{Config, Pwm};

    use crate::board;

    pub const PRESENT: bool = true;

    pub struct Stage {
        pwm: Pwm<'static>,
        cfg: Config,
        disable1: Output<'static>,
        fan: Output<'static>,
        // Phases 2 and 3 are parked: PWM low, driver disabled.
        _pwm2: Output<'static>,
        _disable2: Output<'static>,
        _pwm3: Output<'static>,
        _disable3: Output<'static>,
    }

    impl Stage {
        #[allow(clippy::too_many_arguments)]
        pub fn new(
            slice: Peri<'static, PWM_SLICE4>,
            pwm1: Peri<'static, PIN_24>,
            disable1: Peri<'static, PIN_25>,
            pwm2: Peri<'static, PIN_20>,
            disable2: Peri<'static, PIN_5>,
            pwm3: Peri<'static, PIN_6>,
            disable3: Peri<'static, PIN_4>,
            fan: Peri<'static, PIN_3>,
        ) -> Self {
            // Drivers disabled before the PWM pin is claimed.
            let disable1 = Output::new(disable1, Level::High);
            let _disable2 = Output::new(disable2, Level::High);
            let _disable3 = Output::new(disable3, Level::High);
            let _pwm2 = Output::new(pwm2, Level::Low);
            let _pwm3 = Output::new(pwm3, Level::Low);
            let mut cfg = Config::default();
            cfg.top = board::PWM_TOP;
            cfg.compare_a = 0;
            let pwm = Pwm::new_output_a(slice, pwm1, cfg.clone());
            Self { pwm, cfg, disable1, fan: Output::new(fan, Level::Low), _pwm2, _disable2, _pwm3, _disable3 }
        }

        pub fn apply(&mut self, enable: bool, duty_permille: u16) {
            if enable {
                self.cfg.compare_a = board::compare(duty_permille);
                self.pwm.set_config(&self.cfg);
                self.disable1.set_low();
                self.fan.set_high();
            } else {
                // Both FETs off first, then park the PWM.
                self.disable1.set_high();
                self.cfg.compare_a = 0;
                self.pwm.set_config(&self.cfg);
                self.fan.set_low();
            }
        }
    }
}
