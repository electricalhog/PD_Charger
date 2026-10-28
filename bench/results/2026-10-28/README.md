# Bench log 2026-10-28: CCM entry integrator from the fit; the 5 V forced-CCM start still latches SW_OVP (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-28, about 19:12 EDT |
| Operator | Claude Code, remotely through `bu`. Dan: "I'm ready to finish implementing ccm" (flashing for CCM approved earlier) |
| Firmware | 2690662 plus the entry change below, ELF sha256 `2c97f97873aebd1e…` |
| Load | 25.3 Ω on VBUS; the QT Py idle |
| Status | **Open. The integrator now starts where the bench needs it, and the 5 V start still overshot to SW_OVP. The integrator is not the cause.** |

## 1. The change

**Before.** After a start, CCM's integrator began at the formula value I R + 2 V_sd dt / T, about +0.04 V. The bench
needs −0.18 V at 5 V and −0.42 V at 20 V (400 kHz, 25.3 Ω).

**Now.**
- The integrator CCM last ran with is kept per setpoint band (below 7 V, 7–12 V, 12–17.5 V, above 17.5 V) across starts.
  It is scaled by V_ref within a band and tagged with the period it ran at.
- The first entry in a band starts from −(16 ‰ V_ref + 100 mV), the fit to the measured values (`CCM_U0_*`).
- **Host model** (new `test_entry`: 55 µF, the plant offset that gives the measured integrator, L1's entry offset):

  | Setpoint | Excursion, fit start | Excursion, formula start |
  |---|---|---|
  | 5 V | +0.13 / −0.10 V | +0.44 V |
  | 9 V | +0.14 / −0.12 V | +0.54 V |
  | 15 V | +0.03 / −0.01 V | +0.72 V |
  | 20 V | +0.22 / −0.23 V | +0.93 V |

  - **L1's entry offset:** the first CCM period starts from DCM's zero current, not from the ripple's value there.
    That is +0.58 A at 5 V and −1.01 A at 20 V.

## 2. The 5 V forced-CCM start at 400 kHz (19:12)

- **Sequence:** stop, `regulator_ccm_enable` 2, trace armed, then `sink req 9000`. The sink Hard Reset and the source
  restarted at 5 V.
- **SW_OVP 200 µs after CCM entered**, the same as 2026-10-23.

| t (ms) | V_out (mV) | Current column (mA) | On-time (ns) | |
|---|---|---|---|---|
| 25.60 | 4898 | 347 | 304 | DCM, 400 kHz |
| 25.65 | 4932 | 220 | 467 | CCM entry |
| 25.70 | 4949 | 557 | 467 | |
| 25.75 | 5105 | 535 | 466 | |
| 25.80 | 5208 | 528 | 463 | |
| 25.85 | 5381 | 515 | 456 | then SW_OVP (5.5 V) |

**What the trace shows:**
- **The on-time was right.** It started at 467 ns. The 2026-10-22 steady state at 5 V was 476 ns (integrator −0.21 V),
  so the duty started at or under the steady-state value. The integrator read −0.42 V at the fault.
- **The current and V_out rose together and did not ring back.** CCM's current stayed at 0.52–0.56 A against 0.2 A
  into the load. V_out rose at about 3 V/ms, which is 0.33 A into about 110 µF, for 200 µs. An LC ring from L1's entry
  offset would turn round within about 40 µs.
- **The same start passed on 2026-10-22 with the +0.04 V formula start** (section 5: synchronized scans, 1 nF caps).
  The failures began on 2026-10-23, after the small divider caps and the double samples. So a wrong starting value is
  not what overshoots here.

**Not yet known:** what holds the extra 0.3 A for 200 µs. Candidates:
- the real gate timing in the first CCM periods: Timer B's sources switch at the roll-over, and Q3's bootstrap was
  idle in DCM;
- the V_out reading in CCM with the small caps;
- a slower node than the 55 µF fit.

The scope (CH1 IL_MON, CH2 Q1 gate, CH4 VBUS) on one more triggered start would separate them.

**End state:** FAULT (SW_OVP), outputs off, `regulator_ccm_enable` set back to 0. Not cleared.

Data: `data/trace_ccm400_5v_entry_u0fit_ovp.csv`.
