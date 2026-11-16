//! I2C1 (PB8 SCL, PB9 SDA) and the analog side of the SRC1M1 shield:
//!
//! * TCPP02 at 0x34: put in Normal mode with the provider gate driver open
//!   (this board sinks; VBUS bypasses the shield's power path) and the VBUS
//!   discharge off, then poll its flags.  ENABLE = PC8, FLGn = PC5.
//! * VBUS on PA0 through the shield's 200k/40k divider (x6, 19.8 V full scale).
//!
//! The buck load is no longer on this bus: since 2026-10-06 the host commands
//! it over the QT Py's own USB port (`bu load`), and the host owns the
//! interlock (disarm before every renegotiation).
use embassy_stm32::adc::{Adc, SampleTime};
use embassy_stm32::gpio::{Input, Output};
use embassy_stm32::i2c::I2c;
use embassy_stm32::i2c::Master;
use embassy_stm32::mode::Async;
use embassy_stm32::peripherals::{ADC1, PA0};
use embassy_stm32::Peri;
use embassy_time::{Duration, Ticker, Timer, with_timeout};

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
/// Ack register power mode.  On the bench TCPP02 the ack echoes the control
/// bits (control 0x10/0x20/0x18 read back as 0x10/0x20/0x18, 2026-10-05), so
/// Normal reads back as bits 5:4 = 01.  tcpp0203.h defines the ack fields
/// bit-reversed (Normal = 10), which this part does not do.
const TCPP_ACK_MODE_MASK: u8 = 0x30;
const TCPP_ACK_MODE_NORMAL: u8 = 0x10;
/// How long the ack may take to report Normal mode after the write [ms].
const TCPP_MODE_TIMEOUT_MS: u32 = 50;
/// Flag register bits: OCP VCONN, OCP VBUS, OVP VBUS, OVP CC, OTP.
const TCPP_FLAG_FAULTS: u8 = 0x1F;
const TCPP_FLAG_VBUS_OK: u8 = 0x20;

/// VBUS divider on the shield: (200 + 40) / 40.
const VBUS_DIVIDER: u32 = 6;
const ADC_FULL_SCALE: u16 = 4095;
const VDDA_MV: u32 = 3300;

/// Link task period [ms]: VBUS every cycle, TCPP02 every 5th.
const PERIOD_MS: u64 = 20;

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

/// I2C1 sees the bus busy: a transfer in flight, or a target holding SCL or
/// SDA low.  Checked before each cycle because the driver spins on it.
fn bus_busy() -> bool {
    embassy_stm32::pac::I2C1.isr().read().busy()
}

pub struct LinkResources {
    pub i2c: Bus,
    pub tcpp_enable: Output<'static>,
    pub tcpp_flg: Input<'static>,
    pub adc: Adc<'static, ADC1>,
    pub vbus_pin: Peri<'static, PA0>,
}

/// Writes the control register, then polls the ack register (up to
/// TCPP_MODE_TIMEOUT_MS) until it reports Normal mode.  Returns the last ack
/// and the ms waited.
async fn tcpp_init(i2c: &mut Bus) -> Result<(u8, u32), ()> {
    if !write(i2c, TCPP02_ADDRESS, &[TCPP_REG_CTRL, TCPP_CTRL_SINK]).await {
        return Err(());
    }
    let mut ack = [0u8];
    for waited_ms in 0..=TCPP_MODE_TIMEOUT_MS {
        if !write_read(i2c, TCPP02_ADDRESS, &[TCPP_REG_ACK], &mut ack).await {
            return Err(());
        }
        if ack[0] & TCPP_ACK_MODE_MASK == TCPP_ACK_MODE_NORMAL {
            return Ok((ack[0], waited_ms));
        }
        Timer::after_millis(1).await;
    }
    Ok((ack[0], TCPP_MODE_TIMEOUT_MS))
}

fn read_vbus_mv(adc: &mut Adc<'static, ADC1>, pin: &mut Peri<'static, PA0>) -> (u32, bool) {
    // 240 kohm source: use the longest sample time.
    let raw = adc.blocking_read(pin, SampleTime::CYCLES640_5);
    let mv = raw as u32 * VDDA_MV / ADC_FULL_SCALE as u32 * VBUS_DIVIDER;
    (mv, raw >= ADC_FULL_SCALE - 8)
}

#[embassy_executor::task]
pub async fn link_task(mut res: LinkResources) {
    // TCPP02: ENABLE high, then Normal mode over I2C (retried every second
    // below until it takes).
    res.tcpp_enable.set_high();
    Timer::after_millis(5).await;
    let mut tcpp_configured = false;
    let mut tcpp_reported = false;
    let mut busy_cycles: u32 = 0;

    let mut ticker = Ticker::every(Duration::from_millis(PERIOD_MS));
    let mut cycle: u32 = 0;
    let mut saturated_reported = false;
    loop {
        ticker.next().await;
        cycle = cycle.wrapping_add(1);

        // A target holding the bus low would make every transfer spin in the
        // driver: skip the I2C work until it lets go.
        if bus_busy() {
            busy_cycles += 1;
            if busy_cycles == 5 {
                emit!("EVT i2c stuck busy (a target holds SCL or SDA low); TCPP02 polling skipped");
                with_state(|s| s.tcpp_ok = false);
            }
            continue;
        }
        if busy_cycles >= 5 {
            emit!("EVT i2c free after {} ms", busy_cycles * PERIOD_MS as u32);
        }
        busy_cycles = 0;

        if !tcpp_configured && cycle % 50 == 1 {
            match tcpp_init(&mut res.i2c).await {
                Ok((ack, waited_ms)) if ack & TCPP_ACK_MODE_MASK == TCPP_ACK_MODE_NORMAL => {
                    tcpp_configured = true;
                    with_state(|s| s.tcpp_ok = true);
                    emit!("EVT tcpp02 ok ack=0x{:02x} after_ms={}", ack, waited_ms);
                }
                Ok((ack, _)) if !tcpp_reported => {
                    tcpp_reported = true;
                    emit!("EVT tcpp02 error unexpected ack=0x{:02x} (CC may not pass)", ack)
                }
                Err(()) if !tcpp_reported => {
                    tcpp_reported = true;
                    emit!("EVT tcpp02 error no answer at 0x34 (I2C wiring, pull-ups, ENABLE)")
                }
                _ => {}
            }
        }

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

        // Bench debug: one control-register write from `tcpp <hex>`.
        if let Some(ctrl) = with_state(|s| s.tcpp_poke.take()) {
            let wrote = write(&mut res.i2c, TCPP02_ADDRESS, &[TCPP_REG_CTRL, ctrl]).await;
            Timer::after_millis(5).await;
            let (mut ack, mut flags) = ([0u8], [0u8]);
            let read = write_read(&mut res.i2c, TCPP02_ADDRESS, &[TCPP_REG_ACK], &mut ack).await
                && write_read(&mut res.i2c, TCPP02_ADDRESS, &[TCPP_REG_FLAGS], &mut flags).await;
            emit!(
                "EVT tcpp02 poke ctrl=0x{:02x} wrote={} ack=0x{:02x} flags=0x{:02x} read={}",
                ctrl, wrote as u8, ack[0], flags[0], read as u8
            );
        }

        // TCPP02 ack, flags and FLGn every 100 ms.
        if cycle % 5 == 0 {
            let mut ack = [0u8];
            if write_read(&mut res.i2c, TCPP02_ADDRESS, &[TCPP_REG_ACK], &mut ack).await {
                with_state(|s| {
                    s.tcpp_ack = ack[0];
                    s.tcpp_ok = ack[0] & TCPP_ACK_MODE_MASK == TCPP_ACK_MODE_NORMAL;
                });
            }
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
    }
}
