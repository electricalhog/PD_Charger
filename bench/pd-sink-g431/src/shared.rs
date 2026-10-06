//! State shared between the PD task and the UART command tasks.
use core::cell::RefCell;
use core::fmt::Write;

use embassy_sync::blocking_mutex::Mutex;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use load_link::Telemetry;
use usbpd::protocol_layer::message::data::source_capabilities::SourceCapabilities;

/// One line of output to the host (without the line ending).
pub type Line = heapless::String<224>;

/// Commands from the host that need the policy engine.
#[derive(Clone, Copy)]
pub enum Cmd {
    /// Renegotiate to the current target (`req`).
    Request,
    /// Enter EPR mode with this operational PDP in watts (`epr`).
    EnterEpr(u32),
    /// Leave EPR mode (`eprexit`).
    ExitEpr,
    /// Ask the source for its capabilities again (`getcaps`).
    GetCaps,
}

/// A contract accepted by the source.
#[derive(Clone, Copy)]
pub struct Contract {
    pub position: u8,
    pub mv: u32,
    pub ma: u32,
    pub epr_request: bool,
}

pub struct State {
    /// "CC1", "CC2", or None when detached.
    pub attached: Option<&'static str>,
    pub epr_mode: bool,
    pub contract: Option<Contract>,
    pub caps: Option<SourceCapabilities>,
    /// What `req` asked for; applied to every negotiation, kept across attaches.
    pub target_mv: u32,
    pub target_ma: u32,
    pub contracts: u32,
    pub hard_resets: u32,
    pub epr_failures: u32,

    /// VBUS from the shield divider (PA0) [mV]; saturated above ~19.8 V.
    pub vbus_mv: u32,
    pub vbus_saturated: bool,
    /// TCPP02 answered and is in Normal mode.
    pub tcpp_ok: bool,
    /// TCPP02 flag register fault bits, FLGn low, or I2C failure.
    pub tcpp_fault: bool,
    pub tcpp_flags: u8,

    /// Renegotiation in progress: the load is held off until the next contract.
    pub hold: bool,
    /// Host wants the load running (`load p`); cleared by `load off`, a load
    /// fault, and detach.
    pub load_enable: bool,
    pub load_p_mw: u16,
    /// One-shot request to clear a latched load fault.
    pub load_clear: bool,
    pub load_online: bool,
    pub load: Option<Telemetry>,
    /// What the last command frame asked for.
    pub load_armed_cmd: bool,
}

pub const DEFAULT_TARGET_MV: u32 = 5000;
pub const DEFAULT_TARGET_MA: u32 = 500;
pub const MAX_TARGET_MV: u32 = 48_000;
pub const MAX_TARGET_MA: u32 = 5_000;

pub static STATE: Mutex<CriticalSectionRawMutex, RefCell<State>> = Mutex::new(RefCell::new(State {
    attached: None,
    epr_mode: false,
    contract: None,
    caps: None,
    target_mv: DEFAULT_TARGET_MV,
    target_ma: DEFAULT_TARGET_MA,
    contracts: 0,
    hard_resets: 0,
    epr_failures: 0,
    vbus_mv: 0,
    vbus_saturated: false,
    tcpp_ok: false,
    tcpp_fault: false,
    tcpp_flags: 0,
    hold: false,
    load_enable: false,
    load_p_mw: 0,
    load_clear: false,
    load_online: false,
    load: None,
    load_armed_cmd: false,
}));

pub static CMD: Channel<CriticalSectionRawMutex, Cmd, 4> = Channel::new();
pub static OUT: Channel<CriticalSectionRawMutex, Line, 24> = Channel::new();

pub fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> R {
    STATE.lock(|s| f(&mut s.borrow_mut()))
}

/// Queue a line for the host.  Drops it if the queue is full (the PD task
/// must never block on the UART).
pub fn emit_args(args: core::fmt::Arguments) {
    let mut line = Line::new();
    if line.write_fmt(args).is_err() {
        // Truncated: still send what fits.
    }
    let _ = OUT.try_send(line);
}

#[macro_export]
macro_rules! emit {
    ($($arg:tt)*) => { $crate::shared::emit_args(format_args!($($arg)*)) };
}
