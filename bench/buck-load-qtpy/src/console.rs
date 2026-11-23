//! USB CDC console on the QT Py's own USB port: the load's command link.
//!
//! One command per line, one reply line each:
//! - `cmd seq=N arm=0|1 clear=0|1 p_mw=N limit_ma=N contract_mv=N vin_min_mv=N`:
//!   the host's load command (load_link::Command; missing keys are 0, which
//!   never arms).  Reply: `TEL ...`, the telemetry of the last control period.
//!   The host repeats it at about 20 Hz; after 250 ms without one a running
//!   load faults (load_control watchdog).
//! - `status` / `?`: `STATUS ...` with telemetry, raw ADC counts per GPIO and
//!   the command counters;
//! - `bootsel`: reboot into the RP2040 ROM's USB bootloader (RPI-RP2 drive),
//!   only if `bootsel_allowed` (main.rs);
//! - `version`, `help`.
//!
//! Setting the line coding to 1200 baud also asks for BOOTSEL, the pico-sdk
//! convention, so one host flow reflashes both this firmware and the port
//! firmware it replaced.

use core::fmt::Write as _;

use embassy_executor::Spawner;
use embassy_futures::select::{Either, select};
use embassy_rp::peripherals::USB;
use embassy_rp::usb::Driver;
use embassy_time::Timer;
use embassy_usb::class::cdc_acm::{CdcAcmClass, ControlChanged, Receiver, Sender, State};
use embassy_usb::{Builder, Config, UsbDevice};
use static_cell::StaticCell;

use load_link::Command;

use crate::SHARED;

type UsbDriver = Driver<'static, USB>;

pub fn start(spawner: Spawner, driver: UsbDriver) {
    // Raspberry Pi's VID with the pico-sdk CDC PID, so the bench udev rules
    // and `bu` treat it like the port firmware; the product string tells
    // them apart.
    let mut config = Config::new(0x2e8a, 0x000a);
    config.manufacturer = Some("PD_Charger bench");
    config.product = Some("buck-load-qtpy");
    config.serial_number = Some("buck-load-qtpy");
    config.max_power = 100;
    config.max_packet_size_0 = 64;

    static CONFIG_DESCRIPTOR: StaticCell<[u8; 256]> = StaticCell::new();
    static BOS_DESCRIPTOR: StaticCell<[u8; 256]> = StaticCell::new();
    static CONTROL_BUF: StaticCell<[u8; 64]> = StaticCell::new();
    static STATE: StaticCell<State> = StaticCell::new();
    let mut builder = Builder::new(
        driver,
        config,
        CONFIG_DESCRIPTOR.init([0; 256]),
        BOS_DESCRIPTOR.init([0; 256]),
        &mut [],
        CONTROL_BUF.init([0; 64]),
    );
    let class = CdcAcmClass::new(&mut builder, STATE.init(State::new()), 64);
    let usb = builder.build();
    let (tx, rx, ctl) = class.split_with_control();
    spawner.spawn(usb_task(usb).unwrap());
    spawner.spawn(console_task(tx, rx, ctl).unwrap());
}

#[embassy_executor::task]
async fn usb_task(mut usb: UsbDevice<'static, UsbDriver>) -> ! {
    usb.run().await
}

#[embassy_executor::task]
async fn console_task(
    mut tx: Sender<'static, UsbDriver>,
    mut rx: Receiver<'static, UsbDriver>,
    ctl: ControlChanged<'static>,
) -> ! {
    let mut line = [0u8; 128];
    let mut n = 0usize;
    let mut packet = [0u8; 64];
    loop {
        rx.wait_connection().await;
        loop {
            match select(rx.read_packet(&mut packet), ctl.control_changed()).await {
                Either::First(Ok(len)) => {
                    for &b in &packet[..len] {
                        if b == b'\n' || b == b'\r' {
                            if n > 0 {
                                handle(&mut tx, &line[..n]).await;
                                n = 0;
                            }
                        } else if n < line.len() {
                            line[n] = b;
                            n += 1;
                        }
                    }
                }
                Either::First(Err(_)) => break,
                Either::Second(()) => {
                    if tx.line_coding().data_rate() == 1200 {
                        bootsel(&mut tx).await;
                    }
                }
            }
        }
    }
}

async fn handle(tx: &mut Sender<'static, UsbDriver>, line: &[u8]) {
    let mut out = Line::new();
    let line = line.trim_ascii();
    let (word, args) = match line.iter().position(|b| *b == b' ') {
        Some(i) => (&line[..i], &line[i + 1..]),
        None => (line, &line[line.len()..]),
    };
    match word {
        b"cmd" => match parse_command(args) {
            Some(c) => {
                crate::accept_command(c);
                telemetry(&mut out);
            }
            None => {
                SHARED.lock(|s| {
                    let mut s = s.borrow_mut();
                    s.diag.bad_frames = s.diag.bad_frames.wrapping_add(1);
                });
                let _ = write!(out, "ERR usage: cmd seq=N arm=0|1 clear=0|1 p_mw=N limit_ma=N contract_mv=N vin_min_mv=N");
            }
        },
        b"status" | b"?" => status(&mut out),
        b"bootsel" => return bootsel(tx).await,
        b"version" => {
            let _ = write!(
                out,
                "VERSION buck-load-qtpy {} power_stage={}",
                env!("CARGO_PKG_VERSION"),
                crate::power::PRESENT as u8
            );
        }
        b"help" => {
            let _ = write!(out, "HELP cmd ... | status | bootsel | version (1200 baud = bootsel)");
        }
        _ => {
            let _ = write!(out, "ERR unknown command");
        }
    }
    send(tx, &out).await;
}

/// `key=value` words into a Command.  Unknown keys or bad numbers reject the
/// whole line; missing keys stay 0 (arm=0 never arms, limit_ma=0 never arms).
fn parse_command(args: &[u8]) -> Option<Command> {
    let mut c = Command { seq: 0, arm: false, clear_fault: false, p_mw: 0, limit_ma: 0, contract_mv: 0, vin_min_mv: 0 };
    for word in args.split(|b| *b == b' ').filter(|w| !w.is_empty()) {
        let eq = word.iter().position(|b| *b == b'=')?;
        let (key, val) = (&word[..eq], &word[eq + 1..]);
        let n: u32 = core::str::from_utf8(val).ok()?.parse().ok()?;
        let n16 = || u16::try_from(n).ok();
        match key {
            b"seq" => c.seq = u8::try_from(n).ok()?,
            b"arm" => c.arm = n != 0,
            b"clear" => c.clear_fault = n != 0,
            b"p_mw" => c.p_mw = n16()?,
            b"limit_ma" => c.limit_ma = n16()?,
            b"contract_mv" => c.contract_mv = n16()?,
            b"vin_min_mv" => c.vin_min_mv = n16()?,
            _ => return None,
        }
    }
    Some(c)
}

fn telemetry(out: &mut Line) {
    let (t, d) = SHARED.lock(|s| {
        let s = s.borrow();
        (s.telemetry, s.diag)
    });
    let _ = write!(
        out,
        "TEL seq={} state={} fault={} enable={} vin_mv={} vout_mv={} iout_ma={} pout_mw={} duty_pm={} temp_mv={}",
        t.seq,
        t.state.name(),
        t.fault.name(),
        d.enable as u8,
        t.vin_mv,
        t.vout_mv,
        t.iout_ma,
        t.pout_mw,
        t.duty_permille,
        t.temp_mv
    );
}

fn status(out: &mut Line) {
    let (t, d, age_ms) = SHARED.lock(|s| {
        let s = s.borrow();
        let age = s.cmd.map(|_| embassy_time::Instant::now().saturating_duration_since(s.cmd_at).as_millis());
        (s.telemetry, s.diag, age)
    });
    let _ = write!(
        out,
        "STATUS state={} fault={} enable={} vin_mv={} vout_mv={} iout_ma={} duty_pm={} pout_mw={} temp_mv={} \
         raw26={:.1} raw27={:.1} raw28={:.1} raw29={:.1} izero={:.1} cmds={} bad_cmds={} seq={} cmd_age_ms=",
        t.state.name(),
        t.fault.name(),
        d.enable as u8,
        t.vin_mv,
        t.vout_mv,
        t.iout_ma,
        t.duty_permille,
        t.pout_mw,
        t.temp_mv,
        d.raw26,
        d.raw27,
        d.raw28,
        d.raw29,
        d.izero,
        d.frames,
        d.bad_frames,
        t.seq,
    );
    let _ = match age_ms {
        Some(ms) => write!(out, "{}", ms),
        None => write!(out, "none"),
    };
}

async fn bootsel(tx: &mut Sender<'static, UsbDriver>) {
    let verdict = SHARED.lock(|s| crate::bootsel_allowed(&s.borrow()));
    let mut out = Line::new();
    match verdict {
        Ok(()) => {
            let _ = write!(out, "OK bootsel");
            send(tx, &out).await;
            Timer::after_millis(50).await;
            embassy_rp::rom_data::reset_to_usb_boot(0, 0);
            loop {
                cortex_m::asm::nop();
            }
        }
        Err(why) => {
            let _ = write!(out, "ERR bootsel refused: {}", why);
            send(tx, &out).await;
        }
    }
}

/// Sends `line` plus CRLF in 64-byte packets, closing the transfer with a
/// short (or empty) packet.
async fn send(tx: &mut Sender<'static, UsbDriver>, line: &Line) {
    let mut buf = Line::new();
    let _ = write!(buf, "{}\r\n", line.as_str());
    let bytes = buf.as_bytes();
    for chunk in bytes.chunks(64) {
        if tx.write_packet(chunk).await.is_err() {
            return;
        }
    }
    if bytes.len() % 64 == 0 {
        let _ = tx.write_packet(&[]).await;
    }
}

/// Fixed-size line buffer; output past the end is dropped.
struct Line {
    buf: [u8; 384],
    len: usize,
}

impl Line {
    fn new() -> Self {
        Self { buf: [0; 384], len: 0 }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }

    fn as_str(&self) -> &str {
        core::str::from_utf8(self.as_bytes()).unwrap_or("")
    }
}

impl core::fmt::Write for Line {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}
