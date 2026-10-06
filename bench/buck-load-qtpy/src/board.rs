//! How the QT Py RP2040 connects to the 20 V buck converter.
//!
//! **Assumed, not verified.** The buck firmware in electricalhog/_20v_Buck_Converter
//! names its analog inputs CURRENT_TAP = A0, BATTERY_TAP (V_in) = A1,
//! OUTPUT_TAP = A2; this file assumes those labels landed on the QT Py's
//! A0/A1/A2, with the NTC on A3.  The seven gate/fan signals have no
//! obvious mapping onto the QT Py's seven digital pins, so the ones below
//! are placeholders, and they are only driven in builds with the
//! `power-stage` feature.  Check every line against your wiring first.
//!
//! | Signal       | QT Py pin | RP2040 | Notes                              |
//! |--------------|-----------|--------|------------------------------------|
//! | I_SENSE      | A0        | GPIO29 | ADC3                               |
//! | V_IN sense   | A1        | GPIO28 | ADC2                               |
//! | V_OUT sense  | A2        | GPIO27 | ADC1                               |
//! | NTC          | A3        | GPIO26 | ADC0                               |
//! | PWM phase 1  | SDA       | GPIO24 | PWM slice 4 A (power-stage only)   |
//! | DISABLE 1    | SCL       | GPIO25 | high = driver off (power-stage)    |
//! | PWM phase 2  | TX        | GPIO20 | held low (power-stage)             |
//! | DISABLE 2    | RX        | GPIO5  | held high (power-stage)            |
//! | PWM phase 3  | SCK       | GPIO6  | held low (power-stage)             |
//! | DISABLE 3    | MISO      | GPIO4  | held high (power-stage)            |
//! | FAN          | MOSI      | GPIO3  | on while running (power-stage)     |
//! | Link SDA     | STEMMA QT | GPIO22 | I2C1 target, address 0x55          |
//! | Link SCL     | STEMMA QT | GPIO23 |                                    |
//!
//! One phase only: the 10 Ω / 10 W ballast never needs more than ~0.85 A.

/// ADC reference (QT Py RP2040 ADC_AVDD = 3.3 V) [mV].
pub const ADC_REF_MV: f32 = 3300.0;
pub const ADC_FULL_SCALE: f32 = 4096.0;

/// V_in / V_out divider ratio.  From the buck sketch: 31.7 counts per volt
/// on a 10-bit, 3.3 V ADC -> 1023 / 3.3 / 31.7 = 9.78.
pub const V_DIVIDER: f32 = 9.78;

/// Current sense: 9.77 counts per amp on 10 bits = 31.5 mV/A at the pin,
/// with the sketch's 2.2 A offset = 69.3 mV.
pub const I_SENSE_MV_PER_A: f32 = 31.5;
pub const I_SENSE_OFFSET_MV: f32 = 69.3;

/// 125 MHz / (TOP + 1) = 250 kHz, the sketch's FREQUENCY.
#[cfg_attr(not(feature = "power-stage"), allow(dead_code))]
pub const PWM_TOP: u16 = 499;

pub fn pin_mv(raw: u16) -> f32 {
    raw as f32 * ADC_REF_MV / ADC_FULL_SCALE
}

pub fn volts_mv(raw: u16) -> u32 {
    (pin_mv(raw) * V_DIVIDER) as u32
}

pub fn amps_ma(raw: u16) -> i32 {
    ((pin_mv(raw) - I_SENSE_OFFSET_MV) / I_SENSE_MV_PER_A * 1000.0) as i32
}

/// Duty [0.1 %] -> PWM compare value.
#[cfg_attr(not(feature = "power-stage"), allow(dead_code))]
pub fn compare(duty_permille: u16) -> u16 {
    ((duty_permille as u32 * (PWM_TOP as u32 + 1)) / 1000) as u16
}
