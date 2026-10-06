//! I2C1 (PB8 SCL, PB9 SDA) and the analog side of the SRC1M1 shield:
//!
//! * TCPP02 at 0x34: put in Normal mode with the provider gate driver open
//!   (this board sinks; VBUS bypasses the shield's power path) and the VBUS
//!   discharge off, then poll its flags.  ENABLE = PC8, FLGn = PC5.
//! * VBUS on PA0 through the shield's 200k/40k divider (x6, 19.8 V full scale).
//! * The buck load (QT Py RP2040) at load_link::ADDRESS on the same bus: one
//!   command frame and one telemetry read every COMMAND_PERIOD_MS.
//!
//! Interlock: the load is armed only with an explicit contract, no
//! renegotiation in progress (`hold`), the TCPP02 healthy, and a host target.
use embassy_stm32::adc::{Adc, SampleTime};
use embassy_stm32::gpio::{Input, Output};
use embassy_stm32::i2c::I2c;
use embassy_stm32::i2c::Master;
use embassy_stm32::mode::Async;
use embassy_stm32::peripherals::{ADC1, PA0};
use embassy_stm32::Peri;
use embassy_time::{Duration, Ticker, Timer, with_timeout};
use load_link::{Command, Fault, State as LoadState, Telemetry};

use crate::emit;
use crate::shared::with_state;

pub const TCPP02_ADDRESS: u8 = 0x34;
const TCPP_REG_CTRL: u8 = 0x00;
const TCPP_REG_ACK: u8 = 0x01;
const TCPP_REG_FLAGS: u8 = 0x02;
/// Normal mode (bits 5:4 = 01), provider gate driver open (bit 2 = 0),
/// consumer gate driver open (bit 3 = 1), VBUS/VCONN discharge off, VCONN
/// switches open.  Bit values from Drivers/BSP/Components/tcpp0203/tcpp0203.h.
const TCPP_CTRL_SINK: u8 = 0x10 | 0x08;
/// Ack register: Normal mode reads back as bits 5:4 = 10.
const TCPP_ACK_MODE_MASK: u8 = 0x30;
const TCPP_ACK_MODE_NORMAL: u8 = 0x20;
/// Flag register bits: OCP VCONN, OCP VBUS, OVP VBUS, OVP CC, OTP.
const TCPP_FLAG_FAULTS: u8 = 0x1F;
const TCPP_FLAG_VBUS_OK: u8 = 0x20;

/// VBUS divider on the shield: (200 + 40) / 40.
const VBUS_DIVIDER: u32 = 6;
const ADC_FULL_SCALE: u16 = 4095;
const VDDA_MV: u32 = 3300;

/// Consecutive failed exchanges before the load is reported offline.
const LINK_FAIL_LIMIT: u32 = 5;

/// Async (DMA) I2C: a blocking transfer would stall the executor that also
/// runs the PD policy engine, which sends GoodCRC in software within
/// tReceive (~1 ms).
pub type Bus = I2c<'static, Async, Master>;
const I2C_TIMEOUT: Duration = Duration::from_millis(10);

async fn write(i2c: &mut Bus, addr: u8, data: &[u8]) -> bool {
    matches!(with_timeout(I2C_TIMEOUT, i2c.write(addr, data)).await, Ok(Ok(())))
}

async fn write_read(i2c: &mut Bus, addr: u8, data: &[u8], read: &mut [u8]) -> bool {
    matches!(with_timeout(I2C_TIMEOUT, i2c.write_read(addr, data, read)).await, Ok(Ok(())))
}

pub struct LinkResources {
    pub i2c: Bus,
    pub tcpp_enable: Output<'static>,
    pub tcpp_flg: Input<'static>,
    pub adc: Adc<'static, ADC1>,
    pub vbus_pin: Peri<'static, PA0>,
}

async fn tcpp_init(i2c: &mut Bus) -> Result<u8, ()> {
    if !write(i2c, TCPP02_ADDRESS, &[TCPP_REG_CTRL, TCPP_CTRL_SINK]).await {
        return Err(());
    }
    let mut ack = [0u8];
    if !write_read(i2c, TCPP02_ADDRESS, &[TCPP_REG_ACK], &mut ack).await {
        return Err(());
    }
    Ok(ack[0])
}

fn read_vbus_mv(adc: &mut Adc<'static, ADC1>, pin: &mut Peri<'static, PA0>) -> (u32, bool) {
    // 240 kohm source: use the longest sample time.
    let raw = adc.blocking_read(pin, SampleTime::CYCLES640_5);
    let mv = raw as u32 * VDDA_MV / ADC_FULL_SCALE as u32 * VBUS_DIVIDER;
    (mv, raw >= ADC_FULL_SCALE - 8)
}

#[embassy_executor::task]
pub async fn link_task(mut res: LinkResources) {
    // TCPP02: ENABLE high, then Normal mode over I2C.
    res.tcpp_enable.set_high();
    Timer::after_millis(5).await;
    match tcpp_init(&mut res.i2c).await {
        Ok(ack) if ack & TCPP_ACK_MODE_MASK == TCPP_ACK_MODE_NORMAL => {
            with_state(|s| s.tcpp_ok = true);
            emit!("EVT tcpp02 ok ack=0x{:02x}", ack);
        }
        Ok(ack) => emit!("EVT tcpp02 error unexpected ack=0x{:02x} (CC may not pass)", ack),
        Err(()) => emit!("EVT tcpp02 error no answer at 0x34 (I2C wiring, pull-ups, ENABLE)"),
    }

    let mut ticker = Ticker::every(Duration::from_millis(load_link::COMMAND_PERIOD_MS));
    let mut seq: u8 = 0;
    let mut cycle: u32 = 0;
    let mut link_fails: u32 = 0;
    let mut last_state = LoadState::Off;
    let mut last_fault = Fault::None;
    let mut saturated_reported = false;
    loop {
        ticker.next().await;
        cycle = cycle.wrapping_add(1);

        // VBUS every cycle.
        let (vbus_mv, saturated) = read_vbus_mv(&mut res.adc, &mut res.vbus_pin);
        with_state(|s| {
            s.vbus_mv = vbus_mv;
            s.vbus_saturated = saturated;
        });
        if saturated && !saturated_reported {
            emit!("EVT warn vbus over the shield divider's 19.8 V range (PA0 at VDDA)");
        }
        saturated_reported = saturated;

        // TCPP02 flags and FLGn every 100 ms.
        if cycle % 5 == 0 {
            let mut flags = [0u8];
            let ok = write_read(&mut res.i2c, TCPP02_ADDRESS, &[TCPP_REG_FLAGS], &mut flags).await;
            let flg_low = res.tcpp_flg.is_low();
            let (was_fault, now_fault) = with_state(|s| {
                let was = s.tcpp_fault;
                s.tcpp_flags = flags[0];
                s.tcpp_fault = !ok || flags[0] & TCPP_FLAG_FAULTS != 0 || flg_low;
                (was, s.tcpp_fault)
            });
            if now_fault && !was_fault {
                emit!("EVT tcpp02 fault flags=0x{:02x} flg={} i2c={}", flags[0], !flg_low as u8, ok as u8);
            } else if was_fault && !now_fault {
                emit!("EVT tcpp02 clear flags=0x{:02x} vbus_ok={}", flags[0], (flags[0] & TCPP_FLAG_VBUS_OK != 0) as u8);
            }
        }

        // Command to the load.
        seq = seq.wrapping_add(1);
        let cmd = with_state(|s| {
            let armed = s.load_enable
                && s.load_p_mw > 0
                && !s.hold
                && !s.tcpp_fault
                && s.tcpp_ok
                && s.contract.is_some();
            let contract = s.contract;
            let clear = core::mem::take(&mut s.load_clear);
            Command {
                seq,
                arm: armed,
                clear_fault: clear && !armed,
                p_mw: if armed { s.load_p_mw } else { 0 },
                limit_ma: contract.map_or(0, |c| c.ma.min(u16::MAX as u32) as u16),
                contract_mv: contract.map_or(0, |c| c.mv.min(u16::MAX as u32) as u16),
                vin_min_mv: contract.map_or(0, |c| (c.mv * 9 / 10).min(u16::MAX as u32) as u16),
            }
        });
        let mut frame = [0u8; 1 + Command::LEN];
        frame[0] = load_link::REG_COMMAND;
        frame[1..].copy_from_slice(&cmd.encode());
        let wrote = write(&mut res.i2c, load_link::ADDRESS, &frame).await;

        let mut buf = [0u8; Telemetry::LEN];
        let tel = if wrote
            && write_read(&mut res.i2c, load_link::ADDRESS, &[load_link::REG_TELEMETRY], &mut buf).await
        {
            Telemetry::decode(&buf).ok()
        } else {
            None
        };

        match tel {
            Some(t) => {
                if link_fails >= LINK_FAIL_LIMIT {
                    emit!("EVT load online");
                }
                link_fails = 0;
                with_state(|s| {
                    s.load_online = true;
                    s.load = Some(t);
                    s.load_armed_cmd = cmd.arm;
                });
                if t.state != last_state || t.fault != last_fault {
                    emit!(
                        "EVT load state={} fault={} vin_mv={} vout_mv={} pout_mw={}",
                        t.state.name(),
                        t.fault.name(),
                        t.vin_mv,
                        t.vout_mv,
                        t.pout_mw
                    );
                    if t.state == LoadState::Fault {
                        // A latched load fault drops the host target: no
                        // automatic re-arm after `load clear`.
                        with_state(|s| s.load_enable = false);
                    }
                    last_state = t.state;
                    last_fault = t.fault;
                }
            }
            None => {
                link_fails += 1;
                if link_fails == LINK_FAIL_LIMIT {
                    with_state(|s| {
                        s.load_online = false;
                        s.load = None;
                    });
                    emit!("EVT load offline (no valid telemetry from 0x{:02x})", load_link::ADDRESS);
                }
            }
        }
    }
}

/// Before renegotiating: hold the load and wait (<= 500 ms) until it reports
/// it is not running.  False if it still runs, so the caller aborts.
pub async fn quiesce_load() -> bool {
    with_state(|s| s.hold = true);
    for _ in 0..25 {
        let running = with_state(|s| {
            s.load_online && s.load.is_some_and(|t| t.state == LoadState::Running)
        });
        if !running {
            return true;
        }
        Timer::after_millis(20).await;
    }
    false
}
