# Bench log 2026-11-02 (evening): CCM current limit with a resistor; handback rate at 200 kHz

| Field | Entry |
|---|---|
| Time | 21:4x–22:02 EST, remote (Claude Code through `bu`), on Dan's "test current regulation so I can attach a battery load" |
| Firmware | `claude/ccm-hf-buck-50w` at 2b9fafd as flashed by the previous session (refresh 300 ns every 5 µs; limit on ID_MON, kl 400 V/(A s), 3 A default) |
| Load | 25.3 Ω on VBUS, QT Py idle. Not re-checked today; the 5 V reading (247 mA on the limit's input) is consistent with it |
| Start and end state | RUNNING, 5 V, buck forced CCM at 400 kHz, limit 3000 mA, no fault |
| Scope | CH4 on TP2 per the 2026-11-02 log; it read 19.73 V DC at the 20 V contract (VD_MON 19.87 V) |
| Script | `scripts/cc_test.py`; raw output `cc_test_400k.jsonl` |

Every number below is a tool reading (status fields, the 20 kHz regulator trace, the scope) unless it is under
"Inference".

## 1. Limit set 3 % under the measured current, 400 kHz forced CCM (`cc_test.py`)

| Contract | Baseline V / I (limit's input) | Limit set | Held 1.5 s later: V / I / flag | PD after |
|---|---|---|---|---|
| 9 V | 8971 mV / 445 ± 24 mA | 432 mA | 8433 mV / 445 ± 22 mA / limiting | no new detach |
| 15 V | 15071 mV / 466 ± 33 mA | 452 mA | 15042 mV / 444 ± 27 mA / limiting | no new detach |
| 20 V | 19917 mV / 658 ± 21 mA | 638 mA | 18422 mV / 629 ± 24 mA / limiting | no new detach |

- I is the mean of 6 status reads, ± their σ. V is the newest raw VD_MON sample, averaged over the same reads.
- The engage and release traces do not contain the events. Each `bu` write lands later than the 102 ms trace window
  armed just before it, so those rows show the state before the change. The script's "engage"/"release" trace
  summaries are therefore not data about the transitions.
- After each release: limiting 0, V back at 8988 / 15054 / 19906 mV.

## 2. Hot-plug with a 1 A limit (`switch-pulse 20` ms, output switch open, then reconnected)

| Contract | Dip | Peak | Samples in the limit (30 ms) | Time below −5 % | Fault |
|---|---|---|---|---|---|
| 9 V | −1136 mV | +123 mV | 71 | 2.25 ms | none |
| 15 V | −2686 mV | +125 mV | 192 | 6.9 ms | none |
| 20 V | −5409 mV | +507 mV | 98 | 0.2 ms | none |

From the regulator trace (VD_MON at the board node, 20 kHz).

## 3. A deeper fold at 20 V: limit 600 mA (baseline 653 mA)

- **Scope (CH4, 20 ms/div, single, falling through 19.4 V):** triggered. The capture starts at 16.59 V and runs below
  the screen's 15 V floor, so its minimum and end are clipped and not valid.
- **Status 1 s later:** RUNNING, V 16557 mV, limit input 631 mA, limiting 1, v_lim 16731 mV.
- **PD counters:** detach 2 → 31, hard reset 2 → 31, transition_timeouts 29, attach 32. During this fold the PD stack
  detached and hard-reset about 29 times.
- **Release (limit 3000 mA):** the scope triggered, but its whole 240 ms record is flat at about 19.74 V, so the
  release itself was not captured. Status after: RUNNING, 19989 mV, limiting 0, limit input 609 mA.

## 4. Handbacks at 20 V, 200 kHz forced CCM (re-entered through 24 V)

- **Exit counter:** 160 → 194 in about 75 s (21:59:3x–22:00:47), all OVER_BAND. That is about 0.4 per second.
  The 2026-11-02 log has about 108 in 30 minutes at 400 kHz (≈ 0.06/s).
- **Error-triggered trace** (threshold 600 mV), fired within about 10 s: min 19299 mV, max 20489 mV.
- **102 ms "now" trace:** mean 20000 mV, σ 42.5 mV, min 19627 mV, max 20179 mV, CCM on every sample.
- **Integrator:** −360 to −378 mV.

## Inference (not measured)

- **ID_MON may not be linear in current over these points.** The readings were 653 mA at 19.9 V, 631 mA at 16.6 V
  and 609 mA at 20.0 V, against V/25.3 Ω of 787, 654 and 790 mA. Those ratios differ by more than the reading's σ.
  Either ID_MON's gain or offset moves with the operating point, or the load is not a fixed 25.3 Ω (the QT Py's input
  draw is not separated). A meter on the load current is needed to tell.
- **The 3 % steps in section 1 are inside ID_MON's scatter** (σ 21–33 mA = 3–7 %), so section 1 shows that the limit
  engages, holds and releases without a fault, but not its accuracy.
- **Why the handbacks are more frequent at 200 kHz:** most likely the larger ripple at 200 kHz spreads VD_MON's
  readings, so the 300 mV band check (2 cycles) is crossed more often. Not separated from other causes.
- **The PD detach loop in section 3** is the fixed-contract ±5 % check the 2026-10-21 log describes; this run did not
  record which check fired.

## What this changes

- **Before a battery:**
  - calibrate the limit's current input against a meter at several currents and voltages, including while V moves;
  - capture the engage and release in the firmware: a trace trigger on the limit flag. `bu` timing cannot do it.
- **PPS is required** to hold a limit more than about 5 % under the contract without PD tearing the contract down
  (section 3).
- **The handback rule needs widening or filtering** before CCM-by-load is trusted at 200 kHz.
  `plans/regulation-strategy.md` section 3.4.
