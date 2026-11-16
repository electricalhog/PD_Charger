# Bench log 2026-10-20: buck forced CCM at 200/400 kHz, first hardware runs (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-20, about 22:25 – 23:55 EDT (UTC−4) |
| Operator | Claude Code, remotely through `tools/bringup` (`bu`), on Dan's instruction ("test it", "make sure the pd board is stable using the hardware available"). Nobody was at the bench |
| DUT | PD Charger. STM32G474, four-switch buck-boost, EPC2308 GaN, L1 4.7 µH |
| Branch | `claude/ccm-hf-buck-50w`. Plan: `plans/ccm-buck-50w.md` |
| Load | The 25.3 Ω on VBUS. The QT Py buck sat idle on VBUS throughout |
| Source | 24 V bench supply (V_in 23.8–24.2 V by VS_MON) |
| Sink | The G431 sink (`bu sink`): 5, 9, 15, 20 and 24 V SPR contracts, plus the 28 V EPR contract once |
| Scope | DS1104Z. CH2 = SW1 (TP3), CH3 = SW2 (TP4), both 10 V/div. CH4 = VBUS at 1 V/div DC, 20 MHz bandwidth limit. 100 µs/div, 1.2 ms records at 4 ns |
| Status | **Buck CCM is stable in holds and contract transitions at 200 and 400 kHz, 5–20 V.** Large capacitive dumps still overshoot (§4.6). Current sensing is not usable in CCM (§4.5). Not ready for a LiPo on VBUS (§6) |

Scripts (`scripts/`, run from anywhere; each calls `tools/bringup/bu`):

| Script | Use |
|---|---|
| `hold.py LABEL [n]` | n status reads plus one 102 ms trace at the present contract |
| `seq.py mv,mv,... [settle_s]` | contract sequence through the sink; per step state, CCM entries and exits, exit log, trace σ; stops on a fault |
| `sw2check.py LABEL` | one scope capture (CH2 SW1, CH3 SW2, CH4 VBUS); SW1 frequency and duty, Q3 dropouts (long SW2 lows, body-diode highs), VBUS low-pass σ and its 20 kHz line |
| `swan.py CSV` | the same analysis offline, plus the refresh dips |
| `steps.py MS N SETPOINT_MV` | N `switch-pulse` reconnects with the trace on the switch trigger: dip, peak, time outside ±5 % |

## 1. Builds

All are CMake Debug with `-DPD_VCONN=ON -DPD_BENCH_FORCE_VCONN=ON -DPD_BENCH_15V=ON -DPD_BENCH_20V=ON -DPD_BENCH_24V=ON -DPD_BENCH_EPR_28V=ON`.

**The first flash went out without these options.** My earlier `bu build --reconfigure` had reset the cache, so that build offered 5/9 V only. It was rebuilt with the options and reflashed at once.

| # | Firmware | What changed |
|---|---|---|
| 1 | 1652de3 | The CCM branch as committed, CCM off by default |
| 2 | +1 | The HRTIM fault enable in FLTxR instead of TIMxCR; runtime refresh length and interval; telemetry race fix |
| 3 | +2 | FLT1/FLT2 digital filter; leave CCM before a mode change; duty guard; HRTIM-triggered injected ADC (reverted in 5) |
| 4 | +3 | Down-ramps in DCM; bumpless entry; CCM exit log |
| 5 | +4 | Injected ADC back to software start. Refresh default 200 ns every 5 µs. **One flash carried the build-4 image** because a static-assert failure did not stop the flash step. It ran with CCM off. Fixed and reflashed |
| 6–8 | +5 | One-sided entry and a switch settle time; saturation-based duty guard; integrator memory and entry grace |
| 9 | +8 | Clamp-tracking anti-windup in `buck_ccm_update`. End state: ELF sha256 `79938803825dedeb…` |

## 2. Default build (CCM off): unchanged on hardware

| Contract | Mode | I_cmd | Trace σ | 2026-10-17/19 baseline |
|---|---|---|---|---|
| 5 V | buck | 369 mA | 11.6 mV | 364–367 mA |
| 20 V | buck | 997 mA | 27.2 mV | — |
| 28 V EPR | buck-boost | 1491 mA | 19.7 mV | 1481 mA (SR off) |

There were no faults.

## 3. What went wrong, and the fixes

**1. Q3's bootstrap could not hold a 150 ns refresh every 10 µs.**
- At 9 V in forced CCM (200 kHz), Q3 dropped out once per PID cycle for about 10 µs. It turned off at Q1's turn-off and came back about 7 µs after the next refresh, with no TB1 edge. That looks like driver UVLO.
- Effects:
  - SW2 sat at body-diode level, then below ground;
  - SW2 averaged 0.47 V under VBUS, which pumped the output (the apparent "plant gain > 1", u −0.56 V);
  - VBUS carried a 186 mV line at 20 kHz;
  - the 15 → 20 V ramp tripped SW_OVP.
- Sweep (9 V, 200 kHz), long SW2 lows per 1.2 ms:

  | Refresh | Long SW2 lows / 1.2 ms | 20 kHz line on VBUS |
  |---|---|---|
  | 150 ns / 10 µs | 48 | 186 mV |
  | 150 ns / 5 µs | 0 | — |
  | 200 ns / 5 µs | 0 | 0.3 mV |
  | 300 ns / 5 µs | 0 | — |
  | 300 ns / 10 µs | 0 | — |

- **Default now 200 ns every 5 µs** (runtime `regulator_ccm_refresh_ns` and `_interval_ns`). Q1's turn-on moves to refresh + 2 dt + 20 ns, which is 260 ns.

**2. The HRTIM hardware fault response was never enabled (pre-existing).**
- `hrtim_configure_fault_levels_and_enable()` ORed "FLT1EN/FLT2EN" into TIMxCR bits 20 and 21. Those bits are TBU and TCU, so each timer also reloaded its registers on Timer B / C updates.
- FLTAR and FLTBR read 0. Faults reached the outputs only through the interrupt, in software.
- **Now:** FLTxR = FLT1EN | FLT2EN on Timers A and B (read back 0x3), and TIMxCR is clean (0x08040008).

**3. HW_FLT1 at the buck (CCM) → buck-boost handover (22.5 V).**
- The fault came in the same millisecond as the mode change, which cut all four gates with about 1 A in L1. VS_GOOD read high again afterwards.
- **Now:**
  - CCM leaves first, then 4 PID cycles in DCM before the mode change;
  - FLT1/FLT2 have a 0.75 µs digital filter (FLTINR1 0x6161);
  - CCM leaves on duty saturation (1 ms at the longest pulse) before the handover.

**4. SW_OVP on a forced-CCM down-ramp, 20 → 15 V.**
- There were 16 entries and exits in one ramp. A re-entry with V_out at 18.8 V and a 15 V setpoint stepped the duty, and VBUS rang 14.6 → 17.6 V.
- **Now:** down-ramps run in DCM (exit RAMP_DOWN), and entry is bumpless. The integrator starts from its last CCM value, shifted to the measured V_out.

**5. 400 kHz: 20 V never entered CCM, and SW_OVP at 5 V.**
- A 92 % / 96 % duty guard put 20 V out of reach: it needs duty 0.82 against a 0.864 maximum. DCM at 400 kHz carries only 0.54 A at 20 V, so the contract failed.
- On the restart at 5 V, VBUS rose 5.0 → 5.9 V just after the output switch closed.
- **Now:**
  - entry is one-sided (not above the setpoint + half the skip band), 20 ms after the switch closes;
  - the duty guard is saturation-based;
  - V_out held over the skip band for 100 µs hands back to DCM (skipping clamps);
  - the entry grace doubles that band for 5 ms.

**6. Re-entry limit cycle.** After the down-ramps to 15 V and 9 V at 400 kHz there were about 7700 handovers before it settled. The formula-based entry missed the light-load offset (u −0.3 to −0.46 V). Fixed by the integrator memory in item 4.

## 4. Results on the final build

### 4.1 Forced CCM, contract sequences: no faults

- **200 kHz:** 24 → 20 → 15 → 9 → 5 → 9 → 15 → 20 → 24 → 20 → 5 V (11 changes).
  - One CCM exit per change: RAMP_DOWN, MODE or DUTY.
  - CCM on 100 % of trace samples at every buck hold.
  - Trace σ 19.6–27.8 mV.
- **400 kHz:** 24 → 20 → 15 → 9 → 5 → 9 → 15 → 20 → 15 → 5 → 20 → 24 → 20 V (13 changes).
  - Same, plus 1–2 single OVER_BAND handbacks on some up-ramps.
  - Settled σ 19.5 mV at 15 V.
  - Readings taken 2.5 s after a change were up to 89 mV while the integrator settled.

### 4.2 Switch nodes and VBUS in CCM (scope)

| Point | SW1 | Q3 dropouts | VBUS below 500 kHz | 20 kHz line |
|---|---|---|---|---|
| 9 V, 200 kHz | 200.0 kHz, duty 0.351 | 0 | 8 mV rms (0.5–5 kHz), 3 mV (5–50 kHz) | 0.3 mV |
| 15 V, 400 kHz | 400 kHz, duty 0.586 | 0 | 18 mV σ | 0.5 mV |

- At 9 V the remaining VBUS content is switching ripple (107 mV at 200 kHz).
- Q4's refresh shows as ~196 ns SW2 dips at the period start (every 10 µs before the change, every 5 µs after), 252 ns ahead of Q1 turning on.

### 4.3 Integrator and the feedforward

At the 25.3 Ω, 200 kHz, refresh 200 / 5000:

| Output | Integrator u |
|---|---|
| 5 V | −0.02 V |
| 9 V | −0.21 to −0.23 V |
| 15 V | −0.32 to −0.34 V |
| 20 V | −0.39 to −0.46 V |

The plant gives a little more than D·V_in at light load. The likely causes are dead-time conduction with a negative valley current and the gate-driver delay (+25 ns of measured Q1 width at 5 V). It is well inside the −1 / +2 V range.

### 4.4 Reconnect steps at 20 V (`switch-pulse 5` ms, 0 → 0.8 A, n = 3 each)

| Mode | Dip | Peak | Outside ±5 % |
|---|---|---|---|
| DCM, 200 kHz | −0.77 … −1.08 V | +0.08 … +0.21 V | ≤ 0.1 ms |
| CCM, 200 kHz | −0.39 … −0.43 V | +0.33 … +0.35 V | 0 |
| CCM, 400 kHz | −0.39 … −0.56 V | +0.28 … +0.35 V | 0 |

- CCM halves the dip, as `ccm_buck.py --steps` predicted (−0.98 V for 0.8 → 2.5 A).
- The overshoot is larger than DCM's but inside +5 %.

### 4.5 Current sensing in CCM: not usable

- **ID_MON** reads about 30 mA at 0.8 A in CCM (20 V).
- **IL_MON** with an HRTIM-triggered mid-interval sample: 0–150 counts mid-pulse in DCM, where L1 carries about 1.5 A (≈480 counts).
- **IS_MON** with software start: in DCM it implies η 0.47–0.88 against V²/R; in CCM it reads far lower (implied η 1.2–5). It is a fixed-phase sample of a pulsed current.
- So the 10-16 "IS_MON reads 1.45× the meter" was probably the same phase bias, not a gain.
- **Consequence:**
  - CCM's light-load exit (mode 1) and SW_OCP have no valid input; forced mode (2) was used with `regulator_ccm_ocp_ma` at 5500, so OCP was effectively off.
  - The hardware backstops stay: the 9 A comparator on IL_MON (untested above ~0.6 A per the 2026-09-28 notes), the ADM1270 at 10 A for 2 ms, and OVP.

### 4.6 Large capacitive dump: SW_OVP (open)

- `switch-pulse 20` ms at 20 V / 400 kHz: the 25 Ω drains the bench-side capacitors during the 20 ms. The reconnect dipped VBUS −5.2 V, then rang to +2.9 V (22.9 V) within about 0.3 ms: SW_OVP.
- This happened before and after clamp-tracking anti-windup. That is the L–C response of a voltage-mode converter: while V_out is volts low, L1's current rises past the load, because nothing bounds it short of the comparator.
- DCM, a bounded current source, does not ring. CCM needs either an inductor-current bound or more output capacitance (roadmap section 4) before it can take steps like this.

## 5. End state

- The final build is flashed (§1 row 9).
- RUNNING, 5 V contract, buck DCM at 200 kHz.
- `regulator_ccm_enable` = 0 and `regulator_buck_fsw_khz` = 200 (the boot defaults).
- No latched fault.
- The scope was left in auto trigger. CH4 settings were changed (1 V/div, DC, 20 MHz limit).

## 6. LiPo on VBUS: not yet

These are the reasons, in order:

1. **No current regulation.**
   - CCM is voltage mode. A battery below the setpoint is an unbounded load, and §4.6 is the small version of that event.
   - The only current bounds today are the 9 A comparator (unverified) and the ADM1270 input trip (10 A for 2 ms).
2. **No usable current measurement in CCM** (§4.5), so a current limit cannot be closed in firmware yet.
3. **The PD sequence starts VBUS at 5 V.** A charged battery on VBUS back-feeds the receptacle and the converter node as soon as the output switch closes: 5 V target, 17–25 V battery.
   - In forced CCM that pulls current backwards into the bench supply, which cannot sink.
   - In DCM it is stopped only by the OVP trip.
4. **OUTPUT_DIS** bleeds VBUS in IDLE and FAULT, which would drain the battery through the bleeder.

**What would make it safe:**
- **A series element that blocks reverse current and limits charge current:** an ideal diode plus a resistor, or a CC electronic load / charger stage between VBUS and the pack.
- **Contract voltage at or below the pack's full voltage** (4.2 V × cells) for CV.
- **Firmware:**
  - period-averaged current sampling: oversample across whole periods as V_in already does, calibrated against a meter;
  - an input- or output-current limit loop in CCM;
  - a connect step that waits for V_out ≈ V_pack.
- **PDO current:** >100 W at 20 V also needs a 5 A PDO (`PD_SRC_MAX_CURRENT_MA` is 3000) and the cable to match.
