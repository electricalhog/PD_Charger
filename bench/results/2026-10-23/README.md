# Bench log 2026-10-23: divider caps removed, double-sampled scans, sample-phase sweep in CCM (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-23, 17:13 – 17:40 EDT |
| Operator | Claude Code, remotely through `bu`. Dan: "I dropped off both divider caps … Experiment with double samples too" |
| Hardware | The 1 nF caps on both dividers (VD_MON, VS_MON) removed. IL_MON coax on CH1. 25.3 Ω on VBUS, QT Py idle |
| Firmware | A: synchronized single-V scans with the V_out median (as 2026-10-22 section 5). B: the scans [V, V, I], first V discarded (`ADC_SYNC_DOUBLE_V`), plus runtime sample placement (`regulator_adc_phase_sel`, `_permille`); B2 is B with the trigger wrapped into the previous period instead of clamped |
| Status | **Bare dividers: no noise penalty, but in CCM the reading moved about 3 % with the sample position, and a sample on a switching edge read volts off (two SW_OVP trips). With smaller caps refitted (section 6, value assumed ~150 pF as suggested) and double samples, the spread away from edges is about 0.2 % (20 mV at 9 V, 40 mV at 20 V), and forced CCM 5 / 9 / 20 V runs with no exits** |

## 1. Caps off, single-V scans (firmware A)

Each point: 8 ring reads (512 VD samples), a 102 ms trace, and the scope's VBUS (CH4 VAVG). In every case the firmware
holds its own V_out reading at the setpoint, so the scope column shows where VBUS actually sat.

| Point | VD outliers (> 12 counts) / sample σ | VS outliers | Trace σ | CCM exits | VBUS (scope) |
|---|---|---|---|---|---|
| 5 V DCM | 4 / 3.6 counts | 4 | 23.6 mV | — | 4.998 V |
| 5 V CCM 400 kHz | 6 / 4.1 | 2 | 29.1 mV | 0 | 4.826 V |
| 9 V CCM | 9 / 4.3 | 0 | 28.5 mV | 0 | 8.746 V |
| 20 V CCM | 2 / 5.0 | 33 | 35.9 mV (44.5 with the caps) | 0 | 19.40 V |

## 2. Double samples (firmware B)

| Point | VD outliers / σ | VS outliers | Trace σ | CCM exits | VBUS |
|---|---|---|---|---|---|
| 5 V DCM | 0 / 3.4 | 1 | 21.5 mV | — | 4.997 V |
| 5 V CCM | 0 / 4.0 | 1 | 27.4 mV | 0 | 4.886 V |
| 9 V CCM | 3 / 4.1 | 5 | 26.6 mV | 0 | 8.844 V |

- **Charge sharing is small:** in DCM the discarded and kept V samples differ by 1–2 counts, so the ADC's sample
  capacitor hardly disturbs the bare divider.
- **SW_OVP on the 9 → 20 V ramp, at about 12 V.**
  - The VD_MON reading stepped from about 700 to about 820 counts (12 → 14 V) between two periods and stayed there.
    VS_MON read 26 V at the same time. The output capacitance cannot move V_out 1.7 V in 2.5 µs, so this was a step in
    the readings.
  - Cause: around duty 0.5 the target sat early in Q1's interval. The double scan needs its trigger 1.2 µs earlier, so
    the trigger was clamped to the period start, and the kept sample then straddled Q1's turn-off.
  - Fix (B2): the trigger wraps into the previous period.
  - The DMA rings stayed aligned (64/64 and 32/32 scans checked).

## 3. Sample-phase sweep at 9 V forced CCM (firmware B2)

The kept V sample was centered at a fraction of Q2's or Q1's interval. At 9 V, Q1's interval is 260–1110 ns and Q2's
runs from 1130 ns to the 2.5 µs period end. Abort guard: VBUS outside 7.7–10.3 V.

| Kept sample at | VBUS (scope) | VD sample σ |
|---|---|---|
| auto (Q2's interval, 50 %) | 8.852 V | 4.2 |
| Q2 10 % | 8.623 V | 6.5 |
| Q2 30 % | 8.719 V | 4.4 |
| Q2 50 % | 8.849 V | 5.0 |
| Q2 70 % | 8.857 V | 4.5 |
| Q2 90 % (near the period end and the refresh) | 8.898 V | 13.9 |
| Q1 10 % | 8.678 V | 5.5 |
| Q1 30 % | 8.731 V | 4.5 |
| Q1 50 % | 8.753 V | 3.9 |
| Q1 70 % (window across Q1's turn-off) | **SW_OVP** | 29 |

- **About 0.28 V (3 %) across the usable positions** while the firmware held 9.00 V. The point sample sees whatever
  the node, or the ground between the divider and the ADC, does at that instant. The scope's absolute DC accuracy is a
  few percent, so the differences between rows are what count.
  - On 2026-10-21 (the 3.6 µs window, caps fitted) VBUS matched within 30 mV at 20 V.
  - Today's 20 V point (section 1) sat 0.6 V low with the sample in Q1's interval.
- **Best positions:** 50–70 % of Q2's interval, highest VBUS and steady. Worst: the start of either interval, right
  after an edge.
- **At Q1 70 %** the 576 ns window reached Q1's turn-off. The readings scattered and then rose about 1.6 V; ID_MON read
  0.82 A, so the output itself probably rose too. SW_OVP at 10.14 V, cleared after the diagnosis. Fault trace in
  `regulator_fault_trace` (saved below).
- **Placement at 20 V:** Q2's interval is about 0.4 µs, too short for the sample, so it has to sit in Q1's interval,
  where the error is larger.

## 4. State at 17:40

- Firmware B2, boot defaults (double scans, automatic placement), RUNNING at 5 V in buck DCM, CCM off, no fault.

## 5. What the bare-divider runs meant for the filter question

- **Timer triggering does remove the noise.** It fixed the CCM handbacks, and taking the caps off cost nothing in
  noise.
- **A point sample in CCM is not the period average.** It is offset by up to about 3 % depending on where in the period
  it lands, and corrupted near edges. The 1 nF caps (5.5 µs) were averaging that away, at the cost of delay.
- **Options:**
  1. **About 100–220 pF back on each divider** (time constant 0.55–1.2 µs). This smooths the edge transients and the
     ground shift within one 400 kHz period, and adds about 1 µs of delay. With double samples the charge-sharing error
     that made medium caps a bad idea no longer applies.
  2. **Keep the bare dividers.** Sample at 50–70 % of Q2's interval, or Q1's at high duty, and calibrate the offset per
     operating point. That is fragile.
  3. **Spread the sample phase over several periods** (equivalent-time sampling): this needs a per-period interrupt,
     or more HRTIM compares in the trigger.

## 6. Caps refitted (17:50 – 17:54), firmware B2

Dan: "Caps fitted". The value was not stated; this log assumes the ~150 pF suggested (time constant about 0.8 µs with
the 5.5 kΩ divider). Scripts: `scripts/pass.py`, `scripts/phase.py`, `scripts/snap.py`.

| Point | VD outliers / σ | VS outliers | Trace σ | CCM exits | VBUS (scope) | Bare, double (section 2) | Bare, single (section 1) |
|---|---|---|---|---|---|---|---|
| 5 V DCM | 3 / 3.8 | 6 | 22.3 mV | — | 4.985 V | 4.997 V | 4.998 V |
| 5 V CCM 400 kHz | 14 / 4.5 | 3 | 24.5 mV | 0 | 4.926 V | 4.886 V | 4.826 V |
| 9 V CCM | 9 / 4.2 | 7 | 27.9 mV | 0 | 8.923 V | 8.844 V | 8.746 V |
| 20 V CCM | 26 / 5.6 | 11 | 40.8 mV | 0 | 19.735 V | — | 19.400 V |

**The sample-phase sweep.** VBUS while the firmware held its reading at the setpoint:

| Kept sample at | 9 V, bare | 9 V, caps | 20 V, caps |
|---|---|---|---|
| auto (50 % of the longer interval) | 8.852 | 8.929 | 19.716 |
| Q2 10 / 30 % | 8.623 / 8.719 | 8.816 / 8.837 | — |
| Q2 50 / 70 / 90 % | 8.849 / 8.857 / 8.898 | 8.926 / 8.914 / 8.906 | — |
| Q1 10 / 30 / 50 % | 8.678 / 8.731 / 8.753 | 8.740 / 8.733 / 8.882 | — / 19.730 / 19.709 |
| Q1 70 % | SW_OVP (window across Q1's turn-off) | — | 19.750 |

- **Away from the first ~30 % of an interval** the reading moves 20 mV at 9 V and 40 mV at 20 V, about 0.2 %. On the
  bare dividers it moved about 3 %.
- **Within 0.3–0.6 µs after an edge** it still reads 0.1–0.2 V high: the cap spreads the edge's transient over its
  ~0.8 µs time constant.
- **Automatic placement:** the center of the longer interval is in the good zone at every point here, so it stays the
  default.
- **Absolute level:** the remaining 0.08 V (9 V) and 0.27 V (20 V) between the setpoint and the scope's VBUS is within
  the DS1104Z's DC accuracy (a few percent of 8 divisions) plus the drop through R6 and Q5/Q6 (about 40 mV at 0.8 A).
  A DMM at TP2 is needed to calibrate it.

Data: `data/caps_on.jsonl`, `data/phase_sweep_caps.jsonl`.

## 7. End state

- Firmware B2 (double scans, automatic placement) at boot defaults. RUNNING at 5 V in buck DCM, CCM off, no fault,
  trace σ 25.5 mV.
- The scope's CH4 is at 1 V/div; its offset was last set by the sweep.

## 8. Hardware against the model: switch-pulse at 20 V, forced CCM 400 kHz (18:42)

Dan: "Can you check where the hardware compares by testing?"
- **Setup:** firmware B2 with the caps fitted, 25.3 Ω, limit 3 A on ID_MON.
- **Pulses:** `switch-pulse` 5, 10 and 20 ms.
- **Records:** the regulator trace, and a raw scope record (CH1 IL_MON 1×, CH3 SW2, CH4 VBUS) triggered on IL_MON
  rising.
- **Scripts:** `scripts/pulses.py` (bench), `scripts/fit_replay.py` and `scripts/plot_hw_vs_model.py`, and the host
  test's `-DSIM_REPLAY` mode.

| Pulse | VBUS before reconnect | IL_MON peak | Board min (SW2, 2.5 µs median / trace, 50 µs) | Max after | Limit | Fault |
|---|---|---|---|---|---|---|
| 5 ms | 19.5 V (the path barely opened: release lag 4.15 ms) | 2.6 A | 18.8 / 19.78 V | 20.14 V | no | none |
| 10 ms | 3.2 V | 8.2 A | 14.8 / 15.44 V | 20.28 V | yes | none |
| 20 ms | 3.2 V | 7.8 A | 14.5 / 15.02 V | 20.16 V | yes | none |

- **VBUS floor:** VBUS drains to about 3.2 V, not to 0. Something on VBUS holds it there.
- **The inductor current is flat-topped:** 8.0–8.2 A from +25 to +75 µs, then about 4.5 A. The likely cause is the
  COMP1 per-cycle backstop (nominally 9 A) ending Q1's pulses. A bandwidth-limited IL_MON reads somewhat low, which
  fits a 9 A threshold.

**The 2026-10-22 model did not match this.** With 55 µF and no per-cycle current bound, reaching the bench's 15 V dip
needed 15–23 A in L1 and overshot to 21–25 V. Three changes make it match:
1. **L1 capped at 7.5 A average** (the comparator).
2. **The 20 kHz loop measuring as the firmware now does:** V_out over the last 9 periods, I_out over the last 8, read
   in the cycle that uses them.
3. **A fit of VBUS-side capacitance and Q5/Q6 turn-on.** Best fit: 85 µF, 2 Ω decaying with a 70 µs time constant.

| 20 ms record | Bench | Fitted model |
|---|---|---|
| VBUS 4 → 14 V | 115 µs | 122 µs |
| Board node min | 14.8 V | 15.1 V |
| Max after | 20.16 V | 20.02 V |
| RMS over 0–300 µs (board / VBUS) | — | 0.41 / 0.34 V |
| Board V at +1 / +2 / +3 / +4 ms | 16.38 / 17.35 / 18.35 / 19.27 V | 16.39 / 17.29 / 18.18 / 19.05 V |
| Back above 19.5 V | 4.30 ms | 4.53 ms |

- **The 10 ms record recovers faster:** back above 19.5 V at 2.85 ms, board 17.09 V at +0.15 ms. The fitted model
  reproduces that when the reconnect lands 30 µs into the 50 µs control cycle: 17.25 V and 3.04 ms. So the spread
  between records is when the limit's 20 µs current window catches the inrush.
- **Where they differ:** on the bench the inductor current drops to about 4.5 A at about 55 µs. In the phase-0 model it
  stays at the cap until about 110 µs: the firmware limit acted one control cycle earlier than the model did.
- **What this tells us about the hardware:**
  - The VBUS-side capacitance is about 85 µF at 3–15 V (the 55 µF of 2026-10-23 section 6 left the comparator out).
  - COMP1 bounds L1 at about 8 A during an inrush. The backstop works, with no fault, which the 2026-09-28 notes had
    left unverified above about 0.6 A.
  - The CCM plant and loop behave as modelled once the comparator is in.

Data: `data/hw_vs_model/` (local): bench records `hw_pulse{5,10,20}_*`, model runs `model_fit_*.csv`, figure
`hw_vs_model.png`.

## 9. End state

- Firmware B2 at boot defaults. RUNNING at 5 V in buck DCM, CCM off, no fault.
- The scope has CH2 off and CH1/CH3/CH4 at the pulse settings.

## 10. Late session (23:2x – 23:5x): Q1 gate, current readings, ADC trigger placement

**Q1's gate in DCM** (20 V, 200 kHz; CH2 on the gate, ground-referenced):
- Q1's on-time measured 2916 ns, against 2935 ns programmed.
- The gate plateau is about 28.0 V while Q1 conducts (switch node 24.4 V plus the gate drive). Vgs is therefore about
  3.6 V, ±1 V at 5 V/div, so a differential probe is needed to confirm the drive level.
- Off, the gate follows the switch node: about −0.3 V, ringing to −7 V after the DCM discharge.

**Current readings after the probing.** At 20 V DCM the board read IS_MON 1.1–1.2 A and ID_MON about 2.0 A. The bench
supply read 0.74 A in and Dan's meter 0.8 A out, so the hardware is fine and the readings were wrong.
- At zero current the raw readings are 8–18 mA, so there is no offset.
- **Cause:** the DCM sample placement. The scan sat at the end of the period, which at 20 V / 200 kHz is the end of the
  discharge and the ring after it.

**Trigger placement fix** (this commit, flashed). Each PID cycle the scan goes in the roomier of two quiet intervals:
- in DCM, the pulse after its blanking, or the discharge's end plus 1.2 µs to the period's end;
- in CCM, Q1's or Q2's interval.

The kept V sample is centered and the current sample kept inside the interval. When nothing fits, the V sample has
priority.

**Results with the fix:**
- At 20 V DCM the samples now fall in Q1's pulse. Voltages are fine (V_out 19.9–20.1 V). The currents read low instead:
  ID_MON 0 mA and IS_MON 0.38 A, against 0.8 / 0.74 A real.
  - In DCM at high duty no single phase gives the average currents: they read low during the pulse and high in the ring.
  - The DCM currents are telemetry only. The CCM limit's ID_MON read correctly at 400 kHz (sections 6 and 8).
  - **Open.**
- **The 5 V forced-CCM start at 400 kHz still latched SW_OVP**, three times tonight, before and after the fix.
  - The fault trace shows a real, smooth rise: 5.05 → 5.74 V over about 160 µs after CCM entered, then over-band,
    then SW_OVP.
  - Likely cause: the entry integrator. After a start it is the formula value (about +0.04 V); the loop needs about
    −0.18 V at 5 V / 400 kHz / 0.2 A, and the integrator corrects at only about 1.6 V/s. It passed several times
    earlier in the day: a marginal case.
  - **Open:** a better starting value (for example the measured −2 % of V_ref at light load).
- **A 24 V request** (the buck-boost route into CCM) timed out and Hard Reset. Not investigated.
- **CCM gate check: not done** tonight.

**End state:** this firmware (placement fix), RUNNING at 5 V in buck DCM at 200 kHz, CCM off, no fault.

