//! I2C link between the bench PD sink (NUCLEO-G431RB, controller) and the
//! buck load (QT Py RP2040, target at [`ADDRESS`]), over the QT Py's STEMMA QT
//! port.  The bus also carries the SRC1M1 shield's TCPP02 at 0x34.
//!
//! Two transactions, both ending in a CRC-8 (poly 0x07, init 0, SMBus PEC
//! without the address byte):
//!
//! * write `[REG_COMMAND, Command::encode()]`: arm/disarm, power target and
//!   the contract limits.  The sink sends one every [`COMMAND_PERIOD_MS`];
//!   the load drops to OFF (and latches `Watchdog` if it was running) when
//!   none arrives for [`WATCHDOG_MS`].
//! * write `[REG_TELEMETRY]`, repeated-start read [`Telemetry::LEN`] bytes.
//!
//! All multi-byte fields are little-endian.
#![no_std]

/// 7-bit I2C address of the load.
pub const ADDRESS: u8 = 0x55;
pub const VERSION: u8 = 1;
pub const REG_COMMAND: u8 = 0x10;
pub const REG_TELEMETRY: u8 = 0x20;
pub const COMMAND_PERIOD_MS: u64 = 20;
pub const WATCHDOG_MS: u64 = 250;

pub fn crc8(data: &[u8]) -> u8 {
    let mut crc = 0u8;
    for &b in data {
        crc ^= b;
        for _ in 0..8 {
            crc = if crc & 0x80 != 0 { (crc << 1) ^ 0x07 } else { crc << 1 };
        }
    }
    crc
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameError {
    Length,
    Version,
    Crc,
}

/// Sink -> load.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Command {
    /// Incremented by the sink on every write; the load echoes the last one.
    pub seq: u8,
    /// Run toward `p_mw`.  False = gate drivers disabled, target 0.
    pub arm: bool,
    /// Clear a latched fault (honored only while `arm` is false).
    pub clear_fault: bool,
    /// Power target into the ballast [mW].
    pub p_mw: u16,
    /// Contract operating current the input must stay under [mA].
    pub limit_ma: u16,
    /// Contract voltage [mV].
    pub contract_mv: u16,
    /// Input voltage below which the load faults (source sagging) [mV].
    pub vin_min_mv: u16,
}

impl Command {
    pub const LEN: usize = 12;

    pub fn encode(&self) -> [u8; Self::LEN] {
        let mut b = [0u8; Self::LEN];
        b[0] = VERSION;
        b[1] = self.seq;
        b[2] = (self.arm as u8) | ((self.clear_fault as u8) << 1);
        b[3..5].copy_from_slice(&self.p_mw.to_le_bytes());
        b[5..7].copy_from_slice(&self.limit_ma.to_le_bytes());
        b[7..9].copy_from_slice(&self.contract_mv.to_le_bytes());
        b[9..11].copy_from_slice(&self.vin_min_mv.to_le_bytes());
        b[11] = crc8(&b[..11]);
        b
    }

    pub fn decode(b: &[u8]) -> Result<Self, FrameError> {
        if b.len() != Self::LEN {
            return Err(FrameError::Length);
        }
        if crc8(&b[..11]) != b[11] {
            return Err(FrameError::Crc);
        }
        if b[0] != VERSION {
            return Err(FrameError::Version);
        }
        let u16_at = |i: usize| u16::from_le_bytes([b[i], b[i + 1]]);
        Ok(Self {
            seq: b[1],
            arm: b[2] & 1 != 0,
            clear_fault: b[2] & 2 != 0,
            p_mw: u16_at(3),
            limit_ma: u16_at(5),
            contract_mv: u16_at(7),
            vin_min_mv: u16_at(9),
        })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    /// Gate drivers disabled, waiting to be armed.
    #[default]
    Off = 0,
    /// Armed and switching toward the target.
    Running = 1,
    /// Latched; needs `clear_fault` with `arm` false.
    Fault = 2,
    /// Built without the `power-stage` feature: telemetry only.
    NoPowerStage = 3,
}

impl State {
    pub fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Running,
            2 => Self::Fault,
            3 => Self::NoPowerStage,
            _ => Self::Off,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Running => "running",
            Self::Fault => "fault",
            Self::NoPowerStage => "no_power_stage",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum Fault {
    #[default]
    None = 0,
    /// No valid command for WATCHDOG_MS while running.
    Watchdog = 1,
    /// V_in fell under vin_min_mv: the source is current-limiting or faulting.
    VinSag = 2,
    /// V_in under 4 V while armed: Hard Reset or detach.
    VbusLost = 3,
    /// Estimated input current over the contract limit.
    InputOverCurrent = 4,
    /// Measured output current over the ballast limit.
    OutputOverCurrent = 5,
    /// V_out over the ballast voltage limit.
    OutputOverVoltage = 6,
}

impl Fault {
    pub fn from_u8(v: u8) -> Self {
        match v {
            1 => Self::Watchdog,
            2 => Self::VinSag,
            3 => Self::VbusLost,
            4 => Self::InputOverCurrent,
            5 => Self::OutputOverCurrent,
            6 => Self::OutputOverVoltage,
            _ => Self::None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Watchdog => "watchdog",
            Self::VinSag => "vin_sag",
            Self::VbusLost => "vbus_lost",
            Self::InputOverCurrent => "input_over_current",
            Self::OutputOverCurrent => "output_over_current",
            Self::OutputOverVoltage => "output_over_voltage",
        }
    }
}

/// Load -> sink.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Telemetry {
    /// Last accepted Command::seq.
    pub seq: u8,
    pub state: State,
    pub fault: Fault,
    pub vin_mv: u16,
    pub vout_mv: u16,
    /// Output current from the buck's current sense; coarse below ~1 A.
    pub iout_ma: i16,
    /// Output power from V_out^2 / R_ballast [mW].
    pub pout_mw: u16,
    /// Duty cycle [0.1 %].
    pub duty_permille: u16,
    /// NTC divider voltage [mV] (no conversion to temperature).
    pub temp_mv: u16,
}

impl Telemetry {
    pub const LEN: usize = 18;

    pub fn encode(&self) -> [u8; Self::LEN] {
        let mut b = [0u8; Self::LEN];
        b[0] = VERSION;
        b[1] = self.seq;
        b[2] = self.state as u8;
        b[3] = self.fault as u8;
        b[4..6].copy_from_slice(&self.vin_mv.to_le_bytes());
        b[6..8].copy_from_slice(&self.vout_mv.to_le_bytes());
        b[8..10].copy_from_slice(&self.iout_ma.to_le_bytes());
        b[10..12].copy_from_slice(&self.pout_mw.to_le_bytes());
        b[12..14].copy_from_slice(&self.duty_permille.to_le_bytes());
        b[14..16].copy_from_slice(&self.temp_mv.to_le_bytes());
        b[16] = 0;
        b[17] = crc8(&b[..17]);
        b
    }

    pub fn decode(b: &[u8]) -> Result<Self, FrameError> {
        if b.len() != Self::LEN {
            return Err(FrameError::Length);
        }
        if crc8(&b[..17]) != b[17] {
            return Err(FrameError::Crc);
        }
        if b[0] != VERSION {
            return Err(FrameError::Version);
        }
        let u16_at = |i: usize| u16::from_le_bytes([b[i], b[i + 1]]);
        Ok(Self {
            seq: b[1],
            state: State::from_u8(b[2]),
            fault: Fault::from_u8(b[3]),
            vin_mv: u16_at(4),
            vout_mv: u16_at(6),
            iout_ma: i16::from_le_bytes([b[8], b[9]]),
            pout_mw: u16_at(10),
            duty_permille: u16_at(12),
            temp_mv: u16_at(14),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc8_smbus_check_value() {
        // CRC-8/SMBUS check value for "123456789".
        assert_eq!(crc8(b"123456789"), 0xF4);
    }

    #[test]
    fn command_round_trip() {
        let c = Command { seq: 7, arm: true, clear_fault: false, p_mw: 2500, limit_ma: 500,
                          contract_mv: 9000, vin_min_mv: 8100 };
        assert_eq!(Command::decode(&c.encode()), Ok(c));
    }

    #[test]
    fn telemetry_round_trip() {
        let t = Telemetry { seq: 9, state: State::Running, fault: Fault::None, vin_mv: 8950,
                            vout_mv: 4470, iout_ma: -12, pout_mw: 1998, duty_permille: 503, temp_mv: 1650 };
        assert_eq!(Telemetry::decode(&t.encode()), Ok(t));
    }

    #[test]
    fn corrupt_frames_are_rejected() {
        let mut b = Command::default().encode();
        b[4] ^= 1;
        assert_eq!(Command::decode(&b), Err(FrameError::Crc));
        assert_eq!(Telemetry::decode(&[0u8; 5]), Err(FrameError::Length));
        let mut t = Telemetry::default().encode();
        t[0] = 2;
        t[17] = crc8(&t[..17]);
        assert_eq!(Telemetry::decode(&t), Err(FrameError::Version));
    }
}
