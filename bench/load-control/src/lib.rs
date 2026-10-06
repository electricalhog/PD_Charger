//! Power-target control law for the buck load, independent of hardware so it
//! runs under `cargo test` on the host.
//!
//! The load is a synchronous buck into a ballast resistor R, so output power
//! is set through the output voltage: V* = sqrt(P* · R).  The current sense
//! on the buck is scaled for tens of amps and is only used as an over-current
//! trip; power is computed from V_out.
//!
//! Every call to [`Controller::step`] gets fresh measurements, the latest
//! command from the sink and its age, and returns the duty cycle and whether
//! the gate drivers may switch.  Anything that is not `Running` returns
//! `enable = false`, which the board maps to all DISABLE lines high (both
//! FETs off): stopping by "0 % duty" would leave the low side switching and
//! pull the output capacitor's energy back into VBUS.
#![no_std]

use load_link::{Command, Fault, State};

#[derive(Clone, Copy, Debug)]
pub struct Params {
    /// Ballast resistance [mΩ].
    pub r_ballast_mohm: u32,
    /// Power the ballast may take [mW] (derated from its rating).
    pub p_ballast_max_mw: u32,
    /// Output over-voltage trip above the ballast's voltage limit [mV].
    pub vout_trip_margin_mv: u32,
    /// Output over-current trip [mA].
    pub iout_trip_ma: i32,
    /// Efficiency assumed when converting output power to input current.
    pub eta: f32,
    /// Maximum duty cycle [0.1 %].
    pub duty_max_permille: u32,
    /// Setpoint slew up / down [mV per ms].  Down must stay slower than the
    /// R·C_out discharge so the buck never runs backwards into VBUS.
    pub slew_up_mv_per_ms: f32,
    pub slew_down_mv_per_ms: f32,
    /// Integral gain [0.1 % duty per (mV · ms)].
    pub ki: f32,
    /// Integral clamp [0.1 % duty].
    pub integ_limit_permille: f32,
    /// V_in below this while armed means VBUS went away (Hard Reset, detach).
    pub vbus_lost_mv: u32,
    /// How long V_in may sit under Command::vin_min_mv before faulting [ms].
    pub vin_sag_ms: u32,
    /// How long the input-current estimate may exceed the limit by 15 % [ms].
    pub input_oc_ms: u32,
    /// Command age after which a running load faults [ms].
    pub watchdog_ms: u32,
}

impl Params {
    /// 10 Ω / 10 W ballast, derated to 7 W.
    pub const BENCH_10R_10W: Params = Params {
        r_ballast_mohm: 10_000,
        p_ballast_max_mw: 7_000,
        vout_trip_margin_mv: 1_500,
        iout_trip_ma: 1_500,
        eta: 0.80,
        duty_max_permille: 900,
        slew_up_mv_per_ms: 5.0,
        slew_down_mv_per_ms: 2.0,
        ki: 0.0005,
        integ_limit_permille: 100.0,
        vbus_lost_mv: 4_000,
        vin_sag_ms: 5,
        input_oc_ms: 20,
        watchdog_ms: load_link::WATCHDOG_MS as u32,
    };

    /// Highest V_out the ballast allows: sqrt(P_max · R) [mV].
    pub fn vout_max_mv(&self) -> f32 {
        libm::sqrtf(self.p_ballast_max_mw as f32 * self.r_ballast_mohm as f32)
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Measurement {
    pub vin_mv: u32,
    pub vout_mv: u32,
    pub iout_ma: i32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Output {
    pub state: State,
    pub fault: Fault,
    /// Gate drivers may switch (DISABLE low on the active phase).
    pub enable: bool,
    pub duty_permille: u16,
    /// V_out^2 / R [mW].
    pub pout_mw: u32,
}

pub struct Controller {
    params: Params,
    state: State,
    fault: Fault,
    v_cmd_mv: f32,
    integ: f32,
    sag_ms: u32,
    oc_ms: u32,
}

impl Controller {
    /// `power_stage` false: telemetry only, never arms (NoPowerStage).
    pub fn new(params: Params, power_stage: bool) -> Self {
        Self {
            params,
            state: if power_stage { State::Off } else { State::NoPowerStage },
            fault: Fault::None,
            v_cmd_mv: 0.0,
            integ: 0.0,
            sag_ms: 0,
            oc_ms: 0,
        }
    }

    pub fn params(&self) -> &Params {
        &self.params
    }

    fn trip(&mut self, fault: Fault) {
        self.state = State::Fault;
        self.fault = fault;
    }

    /// One control period.  `cmd` is the latest valid command and its age in
    /// ms (None if none ever arrived).
    pub fn step(&mut self, m: Measurement, cmd: Option<(Command, u32)>, dt_ms: u32) -> Output {
        let p = self.params;
        let pout_mw = (m.vout_mv as u64 * m.vout_mv as u64 / p.r_ballast_mohm as u64) as u32;
        let fresh = cmd.filter(|(_, age)| *age <= p.watchdog_ms).map(|(c, _)| c);

        match self.state {
            State::NoPowerStage => {}
            State::Fault => {
                if let Some(c) = fresh
                    && c.clear_fault
                    && !c.arm
                {
                    self.state = State::Off;
                    self.fault = Fault::None;
                }
            }
            State::Off => {
                if let Some(c) = fresh
                    && c.arm
                    && c.limit_ma > 0
                    && m.vin_mv >= p.vbus_lost_mv
                    && m.vin_mv >= c.vin_min_mv as u32
                {
                    // Bumpless: start from whatever the output capacitor holds.
                    self.v_cmd_mv = m.vout_mv as f32;
                    self.integ = 0.0;
                    self.sag_ms = 0;
                    self.oc_ms = 0;
                    self.state = State::Running;
                }
            }
            State::Running => match fresh {
                None => self.trip(Fault::Watchdog),
                Some(c) if !c.arm => self.state = State::Off,
                Some(c) => self.run(m, c, dt_ms, pout_mw),
            },
        }

        if self.state != State::Running {
            self.integ = 0.0;
            return Output { state: self.state, fault: self.fault, enable: false, duty_permille: 0, pout_mw };
        }
        let duty = if m.vin_mv == 0 {
            0.0
        } else {
            self.v_cmd_mv * 1000.0 / m.vin_mv as f32 + self.integ
        };
        let duty = duty.clamp(0.0, p.duty_max_permille as f32);
        Output {
            state: self.state,
            fault: self.fault,
            enable: true,
            duty_permille: duty as u16,
            pout_mw,
        }
    }

    fn run(&mut self, m: Measurement, c: Command, dt_ms: u32, pout_mw: u32) {
        let p = self.params;
        if m.vin_mv < p.vbus_lost_mv {
            return self.trip(Fault::VbusLost);
        }
        self.sag_ms = if m.vin_mv < c.vin_min_mv as u32 { self.sag_ms + dt_ms } else { 0 };
        if self.sag_ms > p.vin_sag_ms {
            return self.trip(Fault::VinSag);
        }
        if m.iout_ma > p.iout_trip_ma {
            return self.trip(Fault::OutputOverCurrent);
        }
        if m.vout_mv as f32 > p.vout_max_mv() + p.vout_trip_margin_mv as f32 {
            return self.trip(Fault::OutputOverVoltage);
        }
        // Input current estimated from output power.
        let iin_est_ma = pout_mw as f32 / (p.eta * m.vin_mv as f32) * 1000.0;
        self.oc_ms = if iin_est_ma > c.limit_ma as f32 * 1.15 { self.oc_ms + dt_ms } else { 0 };
        if self.oc_ms > p.input_oc_ms {
            return self.trip(Fault::InputOverCurrent);
        }

        // Power the target may use: host target, ballast rating, contract.
        let p_contract_mw = p.eta * m.vin_mv as f32 * c.limit_ma as f32 / 1000.0;
        let p_allowed = (c.p_mw as f32).min(p.p_ballast_max_mw as f32).min(p_contract_mw).max(0.0);
        let v_target = libm::sqrtf(p_allowed * p.r_ballast_mohm as f32)
            .min(m.vin_mv as f32 * p.duty_max_permille as f32 / 1000.0)
            .min(p.vout_max_mv());

        let dt = dt_ms as f32;
        if v_target > self.v_cmd_mv {
            self.v_cmd_mv = (self.v_cmd_mv + p.slew_up_mv_per_ms * dt).min(v_target);
        } else {
            self.v_cmd_mv = (self.v_cmd_mv - p.slew_down_mv_per_ms * dt).max(v_target);
        }
        self.integ = (self.integ + p.ki * (self.v_cmd_mv - m.vout_mv as f32) * dt)
            .clamp(-p.integ_limit_permille, p.integ_limit_permille);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const P: Params = Params::BENCH_10R_10W;

    fn cmd(arm: bool, p_mw: u16) -> Command {
        Command { seq: 1, arm, clear_fault: false, p_mw, limit_ma: 500, contract_mv: 9000, vin_min_mv: 8100 }
    }

    /// Ideal buck into R with a first-order output: V_out follows duty·V_in.
    fn sim(ctrl: &mut Controller, c: Command, vin: u32, ms: u32, vout: &mut f32) -> Output {
        let mut out = Output::default();
        for _ in 0..ms {
            let m = Measurement { vin_mv: vin, vout_mv: *vout as u32, iout_ma: (*vout / 10.0) as i32 };
            out = ctrl.step(m, Some((c, 0)), 1);
            let target = if out.enable { out.duty_permille as f32 / 1000.0 * vin as f32 } else { 0.0 };
            *vout += (target - *vout) * 0.05; // ~20 ms time constant
        }
        out
    }

    #[test]
    fn telemetry_only_build_never_arms() {
        let mut c = Controller::new(P, false);
        let o = c.step(Measurement { vin_mv: 9000, ..Default::default() }, Some((cmd(true, 2000), 0)), 1);
        assert_eq!(o.state, State::NoPowerStage);
        assert!(!o.enable);
    }

    #[test]
    fn reaches_power_target_through_vout() {
        let mut c = Controller::new(P, true);
        let mut vout = 0.0;
        let o = sim(&mut c, cmd(true, 2500), 9000, 3000, &mut vout);
        assert_eq!(o.state, State::Running);
        // sqrt(2.5 W · 10 Ω) = 5.0 V
        assert!((vout - 5000.0).abs() < 100.0, "vout {vout}");
        assert!((o.pout_mw as i32 - 2500).abs() < 120, "pout {}", o.pout_mw);
    }

    #[test]
    fn contract_current_caps_power() {
        // 9 V · 0.5 A · 0.8 = 3.6 W allowed although 6 W was asked.
        let mut c = Controller::new(P, true);
        let mut vout = 0.0;
        let o = sim(&mut c, cmd(true, 6000), 9000, 3000, &mut vout);
        assert!(o.pout_mw <= 3700 && o.pout_mw >= 3400, "pout {}", o.pout_mw);
    }

    #[test]
    fn ballast_rating_caps_power() {
        let mut c = Controller::new(P, true);
        let mut vout = 0.0;
        let mut k = cmd(true, 20_000);
        k.limit_ma = 3000;
        k.contract_mv = 20_000;
        k.vin_min_mv = 18_000;
        let o = sim(&mut c, k, 20_000, 4000, &mut vout);
        assert!(o.pout_mw <= 7_100, "pout {}", o.pout_mw);
    }

    #[test]
    fn disarm_disables_immediately_and_watchdog_latches() {
        let mut c = Controller::new(P, true);
        let mut vout = 0.0;
        sim(&mut c, cmd(true, 1000), 9000, 200, &mut vout);
        let m = Measurement { vin_mv: 9000, vout_mv: vout as u32, iout_ma: 300 };
        let o = c.step(m, Some((cmd(false, 0), 0)), 1);
        assert_eq!((o.state, o.enable), (State::Off, false));

        sim(&mut c, cmd(true, 1000), 9000, 200, &mut vout);
        let o = c.step(m, Some((cmd(true, 1000), P.watchdog_ms + 1)), 1);
        assert_eq!((o.state, o.fault, o.enable), (State::Fault, Fault::Watchdog, false));
        // Clearing needs clear_fault with arm low.
        let mut clr = cmd(false, 0);
        clr.clear_fault = true;
        assert_eq!(c.step(m, Some((cmd(true, 1000), 0)), 1).state, State::Fault);
        assert_eq!(c.step(m, Some((clr, 0)), 1).state, State::Off);
    }

    #[test]
    fn vin_sag_and_vbus_loss_trip() {
        let mut c = Controller::new(P, true);
        let mut vout = 0.0;
        sim(&mut c, cmd(true, 1000), 9000, 200, &mut vout);
        let o = sim(&mut c, cmd(true, 1000), 7000, 10, &mut vout); // under 8.1 V for 10 ms
        assert_eq!(o.fault, Fault::VinSag);

        let mut c = Controller::new(P, true);
        sim(&mut c, cmd(true, 1000), 9000, 200, &mut vout);
        let o = c.step(Measurement { vin_mv: 1000, vout_mv: 3000, iout_ma: 300 }, Some((cmd(true, 1000), 0)), 1);
        assert_eq!(o.fault, Fault::VbusLost);
    }

    #[test]
    fn does_not_arm_without_contract_or_with_low_vin() {
        let mut c = Controller::new(P, true);
        let mut k = cmd(true, 1000);
        k.limit_ma = 0;
        assert_eq!(c.step(Measurement { vin_mv: 9000, ..Default::default() }, Some((k, 0)), 1).state, State::Off);
        assert_eq!(c.step(Measurement { vin_mv: 7000, ..Default::default() }, Some((cmd(true, 1000), 0)), 1).state, State::Off);
    }

    #[test]
    fn step_down_is_slew_limited() {
        let mut c = Controller::new(P, true);
        let mut vout = 0.0;
        sim(&mut c, cmd(true, 3600), 9000, 3000, &mut vout); // ~6 V
        let before = c.v_cmd_mv;
        let m = Measurement { vin_mv: 9000, vout_mv: vout as u32, iout_ma: 600 };
        c.step(m, Some((cmd(true, 100), 0)), 1);
        assert!((before - c.v_cmd_mv - P.slew_down_mv_per_ms).abs() < 0.01);
    }

    #[test]
    fn vout_limit_for_10w_10ohm_is_about_8_4_v() {
        assert!((P.vout_max_mv() - 8366.6).abs() < 1.0);
    }
}
