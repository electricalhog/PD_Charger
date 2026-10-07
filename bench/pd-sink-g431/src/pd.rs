//! USB-PD sink: UCPD driver glue and the device policy manager.
//!
//! The UCPD glue (driver, attach/detach detection) follows elagil/usbpd's
//! examples/embassy-stm32-g431cb-epr.  The policy is ours: request exactly
//! the voltage and current the host set with `req`, enter or leave EPR mode
//! only when told to, and report every step on the UART.
use embassy_futures::select::{Either, select};
use embassy_stm32::gpio::Output;
use embassy_stm32::ucpd::{self, CcPhy, CcPull, CcSel, CcVState, PdPhy, Ucpd};
use embassy_stm32::{Peri, bind_interrupts, dma, peripherals};
use embassy_time::{Duration, Timer, with_timeout};
use uom::si::electric_current::milliampere;
use uom::si::electric_potential::millivolt;
use uom::si::power::{milliwatt, watt};
use usbpd::protocol_layer::message::data::epr_mode::DataEnterFailed;
use usbpd::protocol_layer::message::data::request::{
    EprRequestDataObject, FixedVariableSupply, PowerSource,
};
use usbpd::protocol_layer::message::data::source_capabilities::{Augmented, PowerDataObject, SourceCapabilities};
use usbpd::sink::device_policy_manager::{DevicePolicyManager, Event};
use usbpd::sink::policy_engine::Sink;
use usbpd::timers::Timer as SinkTimer;
use usbpd::units::Power;
use usbpd_traits::Driver as SinkDriver;

use crate::emit;
use crate::shared::{CMD, Cmd, Contract, with_state};

bind_interrupts!(struct Irqs {
    UCPD1 => ucpd::InterruptHandler<peripherals::UCPD1>;
    DMA1_CHANNEL1 => dma::InterruptHandler<peripherals::DMA1_CH1>;
    DMA1_CHANNEL2 => dma::InterruptHandler<peripherals::DMA1_CH2>;
});

pub struct UcpdResources {
    pub ucpd: Peri<'static, peripherals::UCPD1>,
    pub pin_cc1: Peri<'static, peripherals::PB6>,
    pub pin_cc2: Peri<'static, peripherals::PB4>,
    pub rx_dma: Peri<'static, peripherals::DMA1_CH1>,
    pub tx_dma: Peri<'static, peripherals::DMA1_CH2>,
}

struct UcpdSinkDriver<'d> {
    pd_phy: PdPhy<'d, peripherals::UCPD1>,
}

impl SinkDriver for UcpdSinkDriver<'_> {
    async fn wait_for_vbus(&mut self) {
        // The policy engine only runs while attached, so VBUS is present.
    }

    async fn receive(&mut self, buffer: &mut [u8]) -> Result<usize, usbpd_traits::DriverRxError> {
        self.pd_phy.receive(buffer).await.map_err(|err| match err {
            ucpd::RxError::Crc | ucpd::RxError::Overrun => usbpd_traits::DriverRxError::Discarded,
            ucpd::RxError::HardReset => usbpd_traits::DriverRxError::HardReset,
        })
    }

    async fn transmit(&mut self, data: &[u8]) -> Result<(), usbpd_traits::DriverTxError> {
        self.pd_phy.transmit(data).await.map_err(|err| match err {
            ucpd::TxError::Discarded => usbpd_traits::DriverTxError::Discarded,
            ucpd::TxError::HardReset => usbpd_traits::DriverTxError::HardReset,
        })
    }

    async fn transmit_hard_reset(&mut self) -> Result<(), usbpd_traits::DriverTxError> {
        self.pd_phy.transmit_hardreset().await.map_err(|err| match err {
            ucpd::TxError::Discarded => usbpd_traits::DriverTxError::Discarded,
            ucpd::TxError::HardReset => usbpd_traits::DriverTxError::HardReset,
        })
    }
}

struct EmbassySinkTimer;

impl SinkTimer for EmbassySinkTimer {
    async fn after_millis(milliseconds: u64) {
        Timer::after_millis(milliseconds).await
    }
}

enum Orientation {
    Cc1,
    Cc2,
    DebugAccessory,
}

async fn wait_detached<T: ucpd::Instance>(cc_phy: &mut CcPhy<'_, T>) {
    loop {
        let (cc1, cc2) = cc_phy.vstate();
        if cc1 == CcVState::LOWEST && cc2 == CcVState::LOWEST {
            // tPDDebounce (10..20 ms): the source's BMC traffic pulls CC low
            // for microseconds.  Without this the sink declared a detach on
            // every Source_Capabilities burst once the source resent them
            // every 150 ms (2026-10-06).
            if with_timeout(Duration::from_millis(15), cc_phy.wait_for_vstate_change())
                .await
                .is_err()
            {
                return;
            }
            continue;
        }
        cc_phy.wait_for_vstate_change().await;
    }
}

async fn wait_attached<T: ucpd::Instance>(cc_phy: &mut CcPhy<'_, T>) -> Orientation {
    loop {
        let (cc1, cc2) = cc_phy.vstate();
        if cc1 == CcVState::LOWEST && cc2 == CcVState::LOWEST {
            cc_phy.wait_for_vstate_change().await;
            continue;
        }
        // tCCDebounce (100..200 ms): restart if the CC state moves.
        if with_timeout(Duration::from_millis(100), cc_phy.wait_for_vstate_change())
            .await
            .is_ok()
        {
            continue;
        }
        return match (cc1, cc2) {
            (_, CcVState::LOWEST) => Orientation::Cc1,
            (CcVState::LOWEST, _) => Orientation::Cc2,
            _ => Orientation::DebugAccessory,
        };
    }
}

/// One `caps` / `EVT pdo` line per PDO.
pub fn emit_pdo(tag: &str, position: usize, pdo: &PowerDataObject) {
    match pdo {
        PowerDataObject::FixedSupply(f) if f.0 == 0 => {}
        PowerDataObject::FixedSupply(f) => emit!(
            "{} pos={} type=fixed mv={} ma={}{}",
            tag,
            position,
            f.voltage().get::<millivolt>(),
            f.max_current().get::<milliampere>(),
            if position == 1 && f.epr_mode_capable() { " epr_capable=1" } else { "" }
        ),
        PowerDataObject::Augmented(Augmented::Spr(p)) => emit!(
            "{} pos={} type=pps min_mv={} max_mv={} ma={}",
            tag,
            position,
            p.min_voltage().get::<millivolt>(),
            p.max_voltage().get::<millivolt>(),
            p.max_current().get::<milliampere>()
        ),
        PowerDataObject::Augmented(Augmented::Epr(a)) => emit!(
            "{} pos={} type=avs min_mv={} max_mv={} mw={}",
            tag,
            position,
            a.min_voltage().get::<millivolt>(),
            a.max_voltage().get::<millivolt>(),
            a.pd_power().get::<milliwatt>()
        ),
        _ => emit!("{} pos={} type=other", tag, position),
    }
}

/// The RDO for the host's target, or None if no fixed PDO has that voltage.
/// In EPR mode every request is an EPR_Request (RDO + PDO copy), SPR
/// positions included (USB PD R3.2 §6.4.9).
fn build_request(
    caps: &SourceCapabilities,
    target_mv: u32,
    target_ma: u32,
    epr_mode: bool,
) -> Option<(PowerSource, Contract)> {
    let source_epr_capable = matches!(
        caps.pdos().first(),
        Some(PowerDataObject::FixedSupply(f)) if f.epr_mode_capable()
    );
    for (i, pdo) in caps.pdos().iter().enumerate() {
        let position = (i + 1) as u8;
        if pdo.is_zero_padding() || (position >= 8 && !epr_mode) {
            continue;
        }
        let PowerDataObject::FixedSupply(f) = pdo else { continue };
        if f.voltage().get::<millivolt>() != target_mv {
            continue;
        }
        let max_ma = f.max_current().get::<milliampere>();
        let ma = target_ma.min(max_ma);
        let raw = (ma / 10) as u16;
        let rdo = FixedVariableSupply(0)
            .with_object_position(position)
            .with_no_usb_suspend(true)
            .with_epr_mode_capable(source_epr_capable)
            .with_capability_mismatch(target_ma > max_ma)
            .with_raw_operating_current(raw)
            .with_raw_max_operating_current(raw);
        let contract = Contract { position, mv: target_mv, ma, epr_request: epr_mode };
        let source = if epr_mode {
            PowerSource::EprRequest(EprRequestDataObject { rdo: rdo.0, pdo: *pdo })
        } else {
            PowerSource::FixedVariableSupply(rdo)
        };
        return Some((source, contract));
    }
    None
}

/// True for EPR_Source_Capabilities.  Those always carry the 7 SPR slots,
/// zero-filled when unused, then the EPR PDOs (USB PD R3.2 §6.5.15.1);
/// Source_Capabilities has at most 7 PDOs and no zero entry.  Only a full
/// set of exactly 7 is ambiguous, and `epr_hint` (entry pending or already
/// in EPR mode) decides it.
fn caps_are_epr(caps: &SourceCapabilities, epr_hint: bool) -> bool {
    let pdos = caps.pdos();
    pdos.len() > 7 || (pdos.len() == 7 && (epr_hint || pdos.iter().any(|p| p.is_zero_padding())))
}

#[derive(Default)]
struct Device {
    /// EnterEprMode sent, waiting for EPR capabilities or a failure.
    epr_pending: bool,
    /// The request last handed to the policy engine, committed on transition.
    pending: Option<Contract>,
}

impl Device {
    /// Record new capabilities and whether they put us in EPR mode.  The PE
    /// calls inform() only after Get_Source_Cap; after attach and after EPR
    /// entry it goes straight to request() (usbpd 72008c4), so both call
    /// this.  Deciding from the caps, not from epr_pending alone, keeps the
    /// SPR caps that follow a Soft_Reset during entry from being taken as
    /// EPR ones.
    fn note_caps(&mut self, caps: &SourceCapabilities) -> bool {
        let hint = self.epr_pending || with_state(|s| s.epr_mode);
        self.epr_pending = false;
        let epr = caps_are_epr(caps, hint);
        with_state(|s| {
            s.epr_mode = epr;
            s.caps = Some(caps.clone());
        });
        emit!("EVT caps epr_mode={} n={}", epr as u8, caps.pdos().len());
        for (i, pdo) in caps.pdos().iter().enumerate() {
            emit_pdo("EVT pdo", i + 1, pdo);
        }
        epr
    }
}

impl DevicePolicyManager for Device {
    async fn inform(&mut self, caps: &SourceCapabilities) {
        self.note_caps(caps);
    }

    async fn request(&mut self, caps: &SourceCapabilities) -> PowerSource {
        // New capabilities (attach, source update, EPR entry): the load is
        // held off until the contract that follows.  No waiting here: the
        // Request must go out within tSenderResponse.
        let epr = self.note_caps(caps);
        let (target_mv, target_ma) = with_state(|s| {
            s.hold = true;
            (s.target_mv, s.target_ma)
        });
        let chosen = build_request(caps, target_mv, target_ma, epr).or_else(|| {
            emit!("EVT warn target_mv={} not offered, requesting 5000", target_mv);
            build_request(caps, 5000, target_ma, epr)
        });
        match chosen {
            Some((source, contract)) => {
                emit!(
                    "EVT request pos={} mv={} ma={} epr_request={}",
                    contract.position,
                    contract.mv,
                    contract.ma,
                    contract.epr_request as u8
                );
                self.pending = Some(contract);
                source
            }
            None => {
                // No usable vSafe5V PDO: malformed capabilities.  The PE
                // needs some answer; this mirrors the library default.
                emit!("EVT error no 5000 mV PDO in capabilities");
                self.pending = None;
                PowerSource::new_fixed(
                    usbpd::protocol_layer::message::data::request::CurrentRequest::Highest,
                    usbpd::protocol_layer::message::data::request::VoltageRequest::Safe5V,
                    caps,
                )
                .unwrap()
            }
        }
    }

    async fn transition_power(&mut self, accepted: &PowerSource) {
        let contract = self.pending.unwrap_or(Contract {
            position: accepted.object_position(),
            mv: 0,
            ma: 0,
            epr_request: false,
        });
        with_state(|s| {
            s.contract = Some(contract);
            s.contracts += 1;
            s.hold = false;
        });
        emit!(
            "EVT contract pos={} mv={} ma={} epr_mode={}",
            contract.position,
            contract.mv,
            contract.ma,
            with_state(|s| s.epr_mode) as u8
        );
    }

    async fn hard_reset(&mut self) {
        self.epr_pending = false;
        with_state(|s| {
            s.hard_resets += 1;
            s.contract = None;
            s.epr_mode = false;
            s.hold = true;
        });
        emit!("EVT hard_reset");
    }

    async fn epr_mode_entry_failed(&mut self, reason: DataEnterFailed) {
        self.epr_pending = false;
        with_state(|s| {
            s.epr_failures += 1;
            s.epr_mode = false;
        });
        emit!("EVT epr_enter_failed reason={:?}", reason);
    }

    async fn get_event(&mut self, caps: &SourceCapabilities) -> Event {
        // The PE only asks for events in Ready: any negotiation is over,
        // including a Reject/Wait that never reaches transition_power, so an
        // existing contract is valid again.
        with_state(|s| {
            if s.contract.is_some() {
                s.hold = false;
            }
        });
        loop {
            // Channel::receive is cancel safe, as get_event must be.
            let cmd = CMD.receive().await;
            // Every command here changes or re-requests the contract; the
            // host disarms the load before sending it.
            with_state(|s| s.hold = true);
            match cmd {
                Cmd::Request => {
                    let (mv, ma, epr) = with_state(|s| (s.target_mv, s.target_ma, s.epr_mode));
                    match build_request(caps, mv, ma, epr) {
                        Some((source, contract)) => {
                            emit!("EVT request pos={} mv={} ma={} epr_request={}",
                                  contract.position, contract.mv, contract.ma, contract.epr_request as u8);
                            self.pending = Some(contract);
                            return Event::RequestPower(source);
                        }
                        None => {
                            with_state(|s| s.hold = false);
                            emit!("EVT error target_mv={} not in current capabilities", mv);
                        }
                    }
                }
                Cmd::EnterEpr(watts) => {
                    self.epr_pending = true;
                    emit!("EVT epr_enter pdp_w={}", watts);
                    return Event::EnterEprMode(Power::new::<watt>(watts));
                }
                Cmd::ExitEpr => {
                    with_state(|s| s.epr_mode = false);
                    emit!("EVT epr_exit");
                    return Event::ExitEprMode;
                }
                Cmd::GetCaps => {
                    return if with_state(|s| s.epr_mode) {
                        Event::RequestEprSourceCapabilities
                    } else {
                        Event::RequestSprSourceCapabilities
                    };
                }
            }
        }
    }
}

fn clear_session() {
    with_state(|s| {
        s.attached = None;
        s.contract = None;
        s.epr_mode = false;
        s.caps = None;
        s.hold = false;
    });
    while CMD.try_receive().is_ok() {}
}

/// Attach -> run the policy engine until detach, forever.  `led` (LD2) is on
/// while a contract is active.
#[embassy_executor::task]
pub async fn ucpd_task(mut res: UcpdResources, mut led: Output<'static>) {
    loop {
        let mut ucpd = Ucpd::new(
            res.ucpd.reborrow(),
            Irqs {},
            res.pin_cc1.reborrow(),
            res.pin_cc2.reborrow(),
            Default::default(),
        );
        ucpd.cc_phy().set_pull(CcPull::Sink);

        let cc_sel = match wait_attached(ucpd.cc_phy()).await {
            Orientation::Cc1 => CcSel::CC1,
            Orientation::Cc2 => CcSel::CC2,
            Orientation::DebugAccessory => {
                emit!("EVT attach debug_accessory (no PD)");
                wait_detached(ucpd.cc_phy()).await;
                emit!("EVT detach");
                continue;
            }
        };
        let name = if matches!(cc_sel, CcSel::CC1) { "CC1" } else { "CC2" };
        clear_session();
        with_state(|s| s.attached = Some(name));
        emit!("EVT attach cc={}", name);

        let (mut cc_phy, pd_phy) = ucpd.split_pd_phy(res.rx_dma.reborrow(), res.tx_dma.reborrow(), Irqs, cc_sel);
        let mut sink: Sink<UcpdSinkDriver<'_>, EmbassySinkTimer, _> =
            Sink::new(UcpdSinkDriver { pd_phy }, Device::default());

        let led_watch = async {
            // LD2 mirrors "contract active" without touching the PE.
            loop {
                if with_state(|s| s.contract.is_some()) { led.set_high() } else { led.set_low() }
                Timer::after_millis(50).await;
            }
        };
        match select(select(sink.run(), wait_detached(&mut cc_phy)), led_watch).await {
            Either::First(Either::First(result)) => emit!("EVT pe_stopped result={:?}", result),
            Either::First(Either::Second(_)) => emit!("EVT detach"),
            Either::Second(_) => {}
        }
        led.set_low();
        clear_session();
    }
}
