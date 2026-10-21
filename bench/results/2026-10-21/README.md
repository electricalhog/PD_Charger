# Bench log 2026-10-21: period-mean currents and the CCM current limit (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-21, about 05:56 – 06:08 EDT (UTC−4) |
| Operator | Claude Code, remotely through `tools/bringup` (`bu`). Nobody was at the bench. Dan approved the flash and steps 0–3 below ("all ok", relayed by the parent session), including the live comparator write in step 3 |
| DUT | PD Charger. STM32G474, four-switch buck-boost, EPC2308 GaN, L1 4.7 µH |
| Branch | `claude/ccm-hf-buck-50w`. Plan: `plans/ccm-buck-50w.md` section 8 |
| Load | The 25.3 Ω on VBUS. The QT Py buck sat idle on VBUS throughout (`bu load status`: off) |
| Source | 24 V bench supply (V_in 23.96–24.18 V by VS_MON) |
| Sink | The G431 sink (`bu sink`): 5, 9, 15 and 20 V SPR contracts |
| Scope | DS1104Z. CH4 = VBUS, DC, 20 MHz limit, 1–5 V/div per step. CH2/CH3 (SW1/SW2) off for the long records |
| Status | **The period-mean output current (ID_MON) is usable in CCM and the limit regulates it: 0.5 A at a 20 V contract holds VBUS at 12.65 V into 25.3 Ω with no fault.** IL_MON is not usable below ~1 A. A fold of more than 5 % ends in a PD detach. The switch-pulse 20 ms reconnect is not fixed: with the per-period L1 bound it trips SW_UVP instead of SW_OVP |

## 1. Build

| # | Firmware | ELF sha256 | Notes |
|---|---|---|---|
| 1 | 131b8a3 (CMake Debug, `PD_VCONN`, `PD_BENCH_FORCE_VCONN`, `PD_BENCH_15V/20V/24V`, `PD_BENCH_EPR_28V`) | `927d881b24a4059d…` | Flashed 05:56, verified. All runs below |

The commit after this log changes only defaults, to the runtime values these runs used: the limit's current source is
ID_MON (`regulator_ccm_ilim_src` 1) and `regulator_id_gain_permille` is 952. SW_OCP and the light-load exit then follow
the same current. That build was compiled and host-tested but **not flashed**: the board runs build 1.

The runtime writes, all approved, were:
- `regulator_buck_fsw_khz` 400 and `regulator_ccm_enable` 2, while IDLE;
- `regulator_id_gain_permille` 952 and `regulator_ccm_ilim_src` 1, after section 3;
- `regulator_ccm_ilim_ma` 750 / 500 / 3000;
- `regulator_ccm_peak_ma` 3500, then 0.

## 2. Default path (CCM off): unchanged

| Contract | Mode | I_cmd | Trace σ (102 ms) | 2026-10-20 |
|---|---|---|---|---|
| 5 V | buck DCM 200 kHz | 367 mA | 12.7 mV | 369 mA, 11.6 mV |
| 20 V | buck DCM 200 kHz | 994 mA | 29.5 mV | 997 mA, 27.2 mV |

- VD_MON samples per PID cycle: 9, as `regulator_adc_per_pid` shows. ADC1's injected IS_MON (8 × 25 cycles) now costs
  one regular trigger per cycle. VOUT_MEDIAN_SAMPLES is 9, so the V_out mean still holds only fresh samples.
- `adc2_aborts` stayed 0 throughout, so the ADC2 sequence (VS_MON, IL_MON, ID_MON, 32.8 µs) always finished inside the
  50 µs cycle.
- With CCM off, the currents read as follows in DCM at 20 V: ID_MON 752 mA, IS_MON 804 mA (P_in 19.3 W), IL_MON
  405 mA. VBUS was 20.14 V by the scope, so V/R is 796 mA.

## 3. Calibration in forced CCM, 400 kHz

Each point is the mean of 8 status reads plus a 102 ms trace. The reference is VBUS from the scope (CH4 VAVG) / 25.3 Ω.
Data: `data/cal_ccm400.jsonl`, `data/trace_ccm400_*.csv`.

| Contract | VBUS (scope) | V/R | ID_MON | IL_MON raw | IL expected (clipped mean) | IS_MON | P_out / P_in | Trace σ |
|---|---|---|---|---|---|---|---|---|
| 5 V | 4.918 V | 194 mA | 231 mA | 122 mA | 349 mA | 53 mA | 0.75 | 38.3 mV |
| 9 V | 8.927 V | 353 mA | 396 mA | 316 mA | 550 mA | 159 mA | 0.82 | 36.9 mV |
| 15 V | 14.890 V | 589 mA | 633 mA | 537 mA | 710 mA | 420 mA | 0.87 | 20.3 mV |
| 20 V | 19.973 V | 789 mA | 859 mA | 664 mA | 791 mA | 709 mA | 0.93 | 24.1 mV |

- **ID_MON (R6, the output shunt) is linear:** ID = 1.050 × V/R + 24 mA over the four points.
  - The offset includes the QT Py's idle draw, which flows through R6 but not through the 25.3 Ω.
  - Gain 952 ‰ is now the default.
  - On 2026-10-20 ID_MON read 31 mA at 0.8 A. That was the fixed-phase sampling, not the sensor.
- **IS_MON is plausible:** P_out / P_in (V²/R against V_in × IS_MON) rises 0.75 → 0.93 from 5 to 20 V. P_in includes
  the board's housekeeping. That makes IS_MON a usable input-power trend.
- **IL_MON (R5, between L1 and SW2) is not usable below ~1 A:**
  - The "expected" column is the mean of a triangle of the computed ripple (1.7–2.8 A p-p), clipped at 0 by the
    unidirectional INA281.
  - IL_MON reads 0.35× to 0.84× of that. It is not a fixed gain: the fit is 1.22 × expected − 0.33 A.
  - In DCM at 20 V it read 0.51×.
  - A likely cause is the INA281 recovering from SW2's common-mode steps: the Q4 refresh every 5 µs in CCM, and the
    DCM ring. Not verified: no probe was on IL_MON.
  - The IL_MON comparator backstop (COMP1, nominally 9 A) may not trip where its DAC value says.
- **CCM holds:**
  - CCM was on in 100 % of trace samples at every point.
  - The integrator was −0.21 to −0.40 V, as on 2026-10-20.
  - There was one RAMP_DOWN exit, on the 9 → 5 V step.

## 4. Current limit

The limit acts on ID_MON with gain 952 ‰.

**750 mA at 20 V** (V/R 0.79 A; a −5 % fold). Data: `data/trace_ilim750_fold.csv`, `data/trace_ilim750_hold.csv`.
- The limit took over at once (`ccm_limiting` 1). V_out fell smoothly from 19.99 V to 18.5 V within the 100 ms trace,
  with no oscillation.
- **PD detach.** At about 6.5 % low, the PD stack's power-ready check reported "not ready" twice, detached, re-attached
  and renegotiated 20 V (`bu pd status`: POWER_NOT_READY ×2, CAD_DETACHED).
- After the restart it settled at 19.65 V (trace mean; σ 28.6 mV).
  - The limited current read 749 mA (σ 63 mA per cycle).
  - Before the detach V_out had reached 18.5 V. The two levels differ by about 40 mA of load current, which the QT Py
    may have drawn differently. Not resolved.

**500 mA at 20 V** (the plan's test). Data: `data/trace_ilim500_fold.csv`, `data/scope_vbus_ilim500_1ms.csv`.
- **The fold:**
  - In the trace it ran 19.96 → 14.4 V in 95 ms, monotonic, limiting on every sample after the first 7 ms.
  - On the scope (1 ms means), VBUS reached 12.65 V about 200 ms after the fold began and held 12.652 V. The expected
    value is 0.5 A × 25.3 Ω = 12.65 V.
  - The time constant was about 50 ms. The host test predicted 63 ms, settling in 90 ms.
- **PD reaction:**
  - About 100 ms at 12.65 V later, VBUS went off (PD detach).
  - The board restarted at 5 V, renegotiated 20 V, folded again to 12.66 V, and was detached again. This repeated
    until the limit was set back to 3000 mA.
  - SW_UVP did not trip: 12.65 V is 63 % of the setpoint, and the trip is at 50 %.
- **No fault** was latched in any of these. After the release (3000 mA), status showed 20 V, CCM and no limiting. The
  release itself was not traced (the trace was spent on the fold), so its overshoot is unmeasured.

**What the PD layer does with a fold.** The ST stack checks VBUS against the contract (`USBPD_DPM_IsPowerReady`,
±5 %) and detaches when it is out. `pd_vbus.c` would also send a Hard Reset after 200 ms more than 8 % low. A fixed PDO
cannot be held in current limit below vSrcNew without breaking the PD contract. That decision is open (section 7).

## 5. Switch-pulse 20 ms at 20 V / 400 kHz with a per-period L1 bound

Settings: `regulator_ccm_peak_ma` 3500, so COMP1's DAC was at 1085 counts, 3.5 A at 250 mV/A. Steady state at
20 V / 0.79 A was unchanged with it (on-time 1960 ns, V_out 19.94 V), so the comparator did not trip at a 1.65 A peak.
Data: `data/trace_switch_pulse20_peak3500.csv`.

| t after reconnect | V_out (board) | Output current (ID_MON) | Q1 on-time |
|---|---|---|---|
| −0.05 ms | 19.85 V | 5 mA (switch open) | 1933 ns |
| +0.05 ms | 14.68 V | 2.70 A | 2022 ns |
| +0.10 ms | 11.18 V | 2.33 A | 2160 ns (longest) |
| +0.15 ms | 10.02 V | 2.14 A | 2160 ns |

- **SW_UVP latched** at the next cycle: V_out fell under 50 % of 20 V. On 2026-10-20, with no bound, the same pulse
  dipped −5.2 V and rang to +2.9 V: SW_OVP.
- With L1 bounded, the drained VBUS side recharges at a bounded current. The VBUS-side capacitance is evidently far
  above the 8 µF the host model assumes: the board node lost 10 V in 150 µs while delivering 2–2.7 A.
- The board ceramics (16 µF at 20 V) fell about 0.1 V/µs, so they supplied about 1.6 A. That leaves only about 1 A
  from L1, under the ~2.6 A average a 3.5 A peak allows. The comparator may therefore be ending pulses earlier than its
  nominal level, which fits the IL_MON readings in section 3. Not verified.
- The scope missed the reconnect. It was set to trigger at 8 V rising, and the VBUS side evidently never fell below
  8 V during the 20 ms.
- **Stopped here** (hw-bringup rule: a surprise ends the series).
  - The planned A/B with no bound was not run. The model and 2026-10-20 both say it trips SW_OVP.
  - The fault was cleared after the diagnosis above (`clear-fault`: OK). `regulator_ccm_peak_ma` was set back to 0
    and `regulator_ccm_enable` to 0, then the MCU was reset.

## 6. End state

- The board runs build 1 (131b8a3) at boot defaults, and the scope settings are restored.

| Item | State |
|---|---|
| Regulator | RUNNING, no latched fault |
| Contract | 5 V |
| Mode | buck DCM, 200 kHz |
| Trace σ | 16.4 mV |
| CCM settings | `regulator_ccm_enable` 0, `regulator_ccm_peak_ma` 0 |
| Limit | 3000 mA on IL_MON: build 1's defaults, before the change to ID_MON |

## 7. Findings and open items

1. **Use ID_MON for CCM's current.** It is linear (1.05×) in CCM. The defaults now follow, but are not yet flashed.
   - ID_MON measures after the board ceramics, so it is the output current, not L1's.
   - It reads to 5.5 A, which is enough for 100 W at 20 V and no more.
2. **IL_MON needs a probe.**
   - Scope it at the MCU pin against SW2 in CCM and DCM. The question is whether its low, nonlinear mean is
     common-mode recovery or a gain or shunt error.
   - Until then the COMP1 thresholds (9 A backstop, `regulator_ccm_peak_ma`) are uncalibrated in amps.
3. **Current limit vs the PD contract.** The limit works: 0.5 A into 25.3 Ω held 12.65 V. But any fold beyond 5 %
   detaches. For a LiPo on VBUS, Dan has to choose one of:
   - (a) a contract at or just above the pack voltage, so CC charging stays within 5 %;
   - (b) a PPS/AVS-style mode, where current limiting is part of the contract;
   - (c) a bench build that keeps the contract while `regulator_ccm_limiting` is set. That needs both
     `USBPD_PWR_IF_SupplyReady` and the PD_VBUS_LOST check in `pd_vbus.c` to accept a deliberate fold. It is out of PD
     spec.
4. **Switch-pulse 20 ms is still open.**
   - Unbounded it trips OVP (+2.9 V); bounded at 3.5 A it trips UVP (−10 V).
   - Both are the bench's large drained VBUS capacitance meeting a converter with no current-limited start.
   - Options: a UVP with persistence while L1 is bounded; a higher bound; or more board capacitance (roadmap Phase 4).
   - First measure the VBUS-side capacitance (register F6).
5. **The limit's speed into a resistor.** About 50 ms time constant into 25.3 Ω, by design: kl is set for a battery's
   ~45 mΩ (`regulator_config.h` CCM_ILIM_KL_DEFAULT). The battery case is host-tested only.
