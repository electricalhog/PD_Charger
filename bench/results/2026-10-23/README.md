# Bench log 2026-10-23: divider caps removed, double-sampled scans, sample-phase sweep in CCM (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-23, 17:13 – 17:40 EDT |
| Operator | Claude Code, remotely through `bu`. Dan: "I dropped off both divider caps … Experiment with double samples too" |
| Hardware | The 1 nF caps on both dividers (VD_MON, VS_MON) removed. IL_MON coax on CH1. 25.3 Ω on VBUS, QT Py idle |
| Firmware | A: synchronized single-V scans with the V_out median (as 2026-10-22 section 5). B: the scans [V, V, I], first V discarded (`ADC_SYNC_DOUBLE_V`), plus runtime sample placement (`regulator_adc_phase_sel`, `_permille`); B2 is B with the trigger wrapped into the previous period instead of clamped |
| Status | **No noise penalty from removing the caps; double samples a little cleaner. But in CCM the reading depends on where in the period it is taken (about 3 % at 9 V), and a sample at a switching edge reads volts off: two SW_OVP trips. Not ready to run CCM unattended on point samples** |

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

## 4. End state

- Firmware B2, boot defaults (double scans, automatic placement), RUNNING at 5 V in buck DCM, CCM off, no fault.
- The scope's CH4 is at 1 V/div with offset −9 V.

## 5. What it means for the filter question

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
