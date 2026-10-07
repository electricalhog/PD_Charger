//! Scriptable USB-PD sink for the PD_Charger bench (NUCLEO-G431RB).
//!
//! CC1 = PB6, CC2 = PB4 (UCPD1, through the SRC1M1 shield's TCPP02),
//! commands and events on LPUART1 (PA2/PA3, the ST-LINK virtual COM port) at
//! 115200 8N1.  I2C1 (PB8/PB9) carries the TCPP02 and the buck load; VBUS is
//! sensed on PA0.  See README.md for the protocol and wiring.  LD2 (PA5) is on
//! while a contract is active.
#![no_std]
#![no_main]

mod link;
mod pd;
mod shared;

use embassy_executor::Spawner;
use embassy_stm32::adc::Adc;
use embassy_stm32::gpio::{Input, Level, Output, Pull, Speed};
use embassy_stm32::i2c::{self, I2c};
use embassy_stm32::time::Hertz;
use embassy_stm32::usart::{self, BufferedUart, BufferedUartRx, BufferedUartTx};
use embassy_stm32::{bind_interrupts, peripherals};
use embedded_io_async::{Read, Write};
use panic_halt as _;
use static_cell::StaticCell;

use crate::shared::{CMD, Cmd, MAX_TARGET_MA, MAX_TARGET_MV, OUT, with_state};

bind_interrupts!(struct UartIrqs {
    LPUART1 => usart::BufferedInterruptHandler<peripherals::LPUART1>;
});

bind_interrupts!(struct I2cIrqs {
    I2C1_EV => i2c::EventInterruptHandler<peripherals::I2C1>;
    I2C1_ER => i2c::ErrorInterruptHandler<peripherals::I2C1>;
    DMA1_CHANNEL3 => embassy_stm32::dma::InterruptHandler<peripherals::DMA1_CH3>;
    DMA1_CHANNEL4 => embassy_stm32::dma::InterruptHandler<peripherals::DMA1_CH4>;
});

const VERSION: &str = env!("CARGO_PKG_VERSION");

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let mut config = embassy_stm32::Config::default();
    config.rcc.hsi = true; // UCPD needs HSI16
    config.rcc.mux.adc12sel = embassy_stm32::rcc::mux::Adcsel::SYS;
    let p = embassy_stm32::init(config);

    static TX_BUF: StaticCell<[u8; 1024]> = StaticCell::new();
    static RX_BUF: StaticCell<[u8; 128]> = StaticCell::new();
    let mut uart_config = usart::Config::default();
    uart_config.baudrate = 115_200;
    let uart = BufferedUart::new(
        p.LPUART1,
        p.PA3,
        p.PA2,
        TX_BUF.init([0; 1024]),
        RX_BUF.init([0; 128]),
        UartIrqs,
        uart_config,
    )
    .unwrap();
    let (tx, rx) = uart.split();

    let led = Output::new(p.PA5, Level::Low, Speed::Low);
    let ucpd = pd::UcpdResources {
        ucpd: p.UCPD1,
        pin_cc1: p.PB6,
        pin_cc2: p.PB4,
        rx_dma: p.DMA1_CH1,
        tx_dma: p.DMA1_CH2,
    };

    // I2C1 at 100 kHz: TCPP02 + load over a few tens of cm of wire.  The
    // internal pull-ups (~40 kohm) only back up the external ones.
    let mut i2c_config = i2c::Config::default();
    i2c_config.frequency = Hertz::khz(100);
    i2c_config.scl_pullup = true;
    i2c_config.sda_pullup = true;
    // The driver busy-waits (without yielding) for a free bus before each
    // transfer, up to this timeout.  The 1 s default froze the executor, PD
    // included, while a target held SCL low (2026-10-05); link.rs also skips
    // transfers while the bus reads BUSY.
    i2c_config.timeout = embassy_time::Duration::from_millis(2);
    let link = link::LinkResources {
        i2c: I2c::new(p.I2C1, p.PB8, p.PB9, p.DMA1_CH3, p.DMA1_CH4, I2cIrqs, i2c_config),
        tcpp_enable: Output::new(p.PC8, Level::Low, Speed::Low),
        tcpp_flg: Input::new(p.PC5, Pull::Up),
        adc: Adc::new(p.ADC1, Default::default()),
        vbus_pin: p.PA0,
    };

    spawner.spawn(uart_tx_task(tx).unwrap());
    spawner.spawn(uart_rx_task(rx).unwrap());
    emit!("EVT boot pd-sink-g431 {}", VERSION);
    spawner.spawn(link::link_task(link).unwrap());
    spawner.spawn(pd::ucpd_task(ucpd, led).unwrap());
}

#[embassy_executor::task]
async fn uart_tx_task(mut tx: BufferedUartTx<'static>) {
    loop {
        let line = OUT.receive().await;
        let _ = tx.write_all(line.as_bytes()).await;
        let _ = tx.write_all(b"\r\n").await;
    }
}

#[embassy_executor::task]
async fn uart_rx_task(mut rx: BufferedUartRx<'static>) {
    let mut line: heapless::Vec<u8, 64> = heapless::Vec::new();
    let mut byte = [0u8; 1];
    loop {
        if rx.read(&mut byte).await.is_err() {
            line.clear();
            continue;
        }
        match byte[0] {
            b'\r' | b'\n' => {
                if !line.is_empty() {
                    if let Ok(text) = core::str::from_utf8(&line) {
                        handle(text.trim());
                    } else {
                        emit!("ERR not utf-8");
                    }
                    line.clear();
                }
            }
            b => {
                if line.push(b).is_err() {
                    line.clear();
                    emit!("ERR line too long");
                }
            }
        }
    }
}

fn parse(arg: Option<&str>) -> Option<u32> {
    arg.and_then(|a| a.parse::<u32>().ok())
}

fn send(cmd: Cmd) {
    if with_state(|s| s.attached.is_none()) {
        emit!("ERR not attached");
    } else if CMD.try_send(cmd).is_err() {
        emit!("ERR busy");
    } else {
        emit!("OK");
    }
}

/// One command line -> one `OK`/`ERR` reply, or data lines closed by `END`.
fn handle(text: &str) {
    let mut words = text.split_ascii_whitespace();
    match words.next() {
        Some("status") | Some("?") => with_state(|s| {
            emit!(
                "STATUS attached={} epr_mode={} target_mv={} target_ma={} contract_pos={} contract_mv={} contract_ma={} contracts={} hard_resets={} epr_failures={} vbus_mv={} vbus_sat={} tcpp_ok={} tcpp_fault={} tcpp_flags=0x{:02x} tcpp_ack=0x{:02x} hold={}",
                s.attached.unwrap_or("none"),
                s.epr_mode as u8,
                s.target_mv,
                s.target_ma,
                s.contract.map_or(0, |c| c.position),
                s.contract.map_or(0, |c| c.mv),
                s.contract.map_or(0, |c| c.ma),
                s.contracts,
                s.hard_resets,
                s.epr_failures,
                s.vbus_mv,
                s.vbus_saturated as u8,
                s.tcpp_ok as u8,
                s.tcpp_fault as u8,
                s.tcpp_flags,
                s.tcpp_ack,
                s.hold as u8
            )
        }),
        Some("caps") => {
            with_state(|s| match &s.caps {
                Some(caps) => {
                    for (i, pdo) in caps.pdos().iter().enumerate() {
                        pd::emit_pdo("PDO", i + 1, pdo);
                    }
                }
                None => emit!("ERR no capabilities yet"),
            });
            emit!("END");
        }
        Some("req") => {
            let Some(mv) = parse(words.next()) else {
                emit!("ERR usage: req <mV> [mA]");
                return;
            };
            let ma = parse(words.next()).unwrap_or_else(|| with_state(|s| s.target_ma));
            if !(5000..=MAX_TARGET_MV).contains(&mv) || ma > MAX_TARGET_MA {
                emit!("ERR range: 5000..={} mV, 0..={} mA", MAX_TARGET_MV, MAX_TARGET_MA);
                return;
            }
            let attached = with_state(|s| {
                s.target_mv = mv;
                s.target_ma = ma;
                s.attached.is_some()
            });
            if attached { send(Cmd::Request) } else { emit!("OK target stored for the next attach") }
        }
        Some("epr") => match parse(words.next()) {
            Some(w) if (1..=240).contains(&w) => send(Cmd::EnterEpr(w)),
            _ => emit!("ERR usage: epr <operational PDP W, 1..240>"),
        },
        Some("eprexit") => send(Cmd::ExitEpr),
        Some("getcaps") => send(Cmd::GetCaps),
        Some("tcpp") => match words.next() {
            None => with_state(|s| emit!("TCPP ack=0x{:02x} flags=0x{:02x} ok={}", s.tcpp_ack, s.tcpp_flags, s.tcpp_ok as u8)),
            Some(hex) => match u8::from_str_radix(hex.trim_start_matches("0x"), 16) {
                Ok(ctrl) => {
                    with_state(|s| s.tcpp_poke = Some(ctrl));
                    emit!("OK result follows as EVT tcpp02 poke");
                }
                Err(_) => emit!("ERR usage: tcpp [<control register, hex>]"),
            },
        },
        Some("rd") => {
            // Bench debug: which CC pins carry this sink's Rd (UCPD CCENABLE).
            // The PD task's next UCPD re-init restores both.
            use embassy_stm32::pac::ucpd::vals::Ccenable;
            let en = match words.next() {
                Some("off") => Some(Ccenable::DISABLED),
                Some("cc1") => Some(Ccenable::CC1),
                Some("cc2") => Some(Ccenable::CC2),
                Some("both") => Some(Ccenable::BOTH),
                _ => None,
            };
            match en {
                Some(en) => {
                    embassy_stm32::pac::UCPD1.cr().modify(|w| w.set_ccenable(en));
                    emit!("OK");
                }
                None => emit!("ERR usage: rd off|cc1|cc2|both"),
            }
        }
        Some("version") => emit!("VERSION pd-sink-g431 {}", VERSION),
        Some("help") => emit!("OK commands: status caps req <mV> [mA] epr <W> eprexit getcaps tcpp [<ctrl hex>] rd off|cc1|cc2|both version"),
        _ => emit!("ERR unknown command (help)"),
    }
}

