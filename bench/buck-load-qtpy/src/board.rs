//! How the QT Py RP2040 connects to the 20 V buck converter.
//!
//! Pin numbers are the QT Py port of the buck firmware, the code that last
//! ran this hardware: electricalhog/_20v_Buck_Converter, branch
//! `electronic_load`, `_20v_Buck_Converter.h` (commit 2f670d1 "Adapt pins
//! for QTPy").  The QT Py is wired straight to the buck's controller
//! footprint.  The Datalore-IP-rp2040 adapter in electricalhog/20v-buck-converter
//! was never built, so its GPIO0..6 map does not apply.  Buck nets are from
//! that repo's `20v_Buck_Converter.sch`.
//!
//! | Signal       | QT Py     | RP2040    | Buck net                                   |
//! |--------------|-----------|-----------|--------------------------------------------|
//! | PWM phases 1+3 | MI      | GPIO4     | PWM_1 and PWM_3 paralleled (slice 2 A), parked low |
//! | DISABLE 1+3  | RX        | GPIO5     | DISABLE_1 and DISABLE_3 paralleled, parked high |
//! | PWM phase 2  | SCK       | GPIO6     | PWM_2 (slice 3 A), the phase this load runs|
//! | DISABLE 2    | MO        | GPIO3     | DISABLE_2                                  |
//! | FAN          | TX        | GPIO20    | FAN_ENABLE (Q8 -> Q7, fan on the 12 V rail)|
//! | V tap        | A3        | GPIO26    | VIN (HV side), port INPUT_TAP, ADC0        |
//! | V tap        | A2        | GPIO27    | +OUT (LV side), port OUTPUT_TAP, ADC1      |
//! | I_SENSE      | A1        | GPIO28    | port: CURRENT_TAP, ADC2                    |
//! | Temperature  | A0        | GPIO29    | port: TEMP_TAP, ADC3                       |
//! | Link SDA/SCL | STEMMA QT | GPIO22/23 | I2C1 (was the port's rotary encoder)       |
//! | (none)       | SDA, SCL  | GPIO24/25 | header pins unpopulated                    |
//!
//! Phase 2 alone is what the port ran below 5 A (`output_enable(0b010)`).
//! The port's table still lists phase 1 on GPIO24/25.  A PWM output on this
//! QT Py died, and phase 1 was moved onto phase 3's pins (user, 2026-10-06):
//! MI drives both PWM inputs and RX both DISABLE inputs, so they park and
//! enable together.  The taps were checked on the bench (VIN back-fed to
//! 2.65 V read on GPIO26, +OUT at 0.1 V on GPIO27).
//!
//! **Gate drivers.** Each phase has a TI dual driver: channel A (high side,
//! referenced to the phase node) takes PWM, channel B (low side) takes PWM
//! through an inverter.  PWM high = high side on, PWM low = low side on.
//! DISABLE high turns both outputs off; DISABLE is pulled low inside the
//! driver, so a floating or low DISABLE *enables* it.  The RP2040 resets with
//! every pad pulled down: whenever this firmware is not driving the pins
//! (reset, BOOTSEL, flashing) and the 12 V gate rail is up, all three low-side
//! FETs are on and +OUT is shorted through 1 uH per phase.  The gate rail
//! comes from VIN (IC8, L6983, about 11.9 V), so the QT Py may only reset with
//! VIN low and +OUT discharged (see `bootsel_allowed` in main.rs).
//!
//! **Supplies.** The QT Py's 3V3 drives the buck's 3V3 rail (driver inputs,
//! inverter, current amplifier) and back-feeds VIN to about 2.7 V through
//! IC7's high-side body diode, so VIN never reads 0 while the QT Py is on USB.

/// The DISABLE wiring of both phases driven from MI (GPIO4) is known: RX
/// (GPIO5), parked high with them (user, 2026-10-06).  Set false if the
/// wiring changes again; the power-stage build then refuses to compile.
#[cfg_attr(not(feature = "power-stage"), allow(dead_code))]
pub const SHARED_PHASE_DISABLE_CONFIRMED: bool = true;

#[cfg(feature = "power-stage")]
const _: () = assert!(
    SHARED_PHASE_DISABLE_CONFIRMED,
    "power-stage: confirm the DISABLE wiring of the phases sharing MI (GPIO4) first (board.rs)"
);

/// ADC reference (QT Py RP2040 ADC_AVDD = 3.3 V) [mV], 12-bit ADC.
pub const ADC_REF_MV: f32 = 3300.0;
pub const ADC_FULL_SCALE: f32 = 4096.0;

/// Both voltage taps: 118.5 counts per volt on the 12-bit ADC (the port's
/// VOLTAGE_SCALE, tuned on this board; the 3.3k/330 dividers are 11.0 nominal,
/// 124 counts/V).
pub const V_COUNTS_PER_V: f32 = 118.5;

/// Current sense: 36.5 counts per amp (the port's CURRENT_SCALE).  The zero
/// is measured at run time while every driver is disabled.
pub const I_COUNTS_PER_A: f32 = 36.5;

/// PWM period TOP + 1 = 285 counts at 125 MHz = 438.6 kHz, the port's
/// MAX_DUTY (120000 / FREQUENCY 420).
#[cfg_attr(not(feature = "power-stage"), allow(dead_code))]
pub const PWM_TOP: u16 = 284;

/// DISABLE lines (GPIO3 phase 2, GPIO5 phases 1+3) for the panic handler's
/// raw SIO writes.
pub const DISABLE_MASK: u32 = (1 << 3) | (1 << 5);
pub const DISABLE_GPIOS: [usize; 2] = [3, 5];

pub fn pin_mv(raw: f32) -> f32 {
    raw * ADC_REF_MV / ADC_FULL_SCALE
}

pub fn volts_mv(raw: f32) -> u32 {
    (raw * 1000.0 / V_COUNTS_PER_V) as u32
}

pub fn amps_ma(raw: f32, zero: f32) -> i32 {
    ((raw - zero) * 1000.0 / I_COUNTS_PER_A) as i32
}

/// Duty [0.1 %] -> PWM compare value.
#[cfg_attr(not(feature = "power-stage"), allow(dead_code))]
pub fn compare(duty_permille: u16) -> u16 {
    ((duty_permille as u32 * (PWM_TOP as u32 + 1)) / 1000) as u16
}
