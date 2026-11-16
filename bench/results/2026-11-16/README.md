# Bench 2026-11-16 — CCM-exit seed fix, regression on the resistor

- G474 source flashed with `build/EprPps` at commit `001f42e` (ELF sha256 `7d4fdac72546726c…`), 24.7 V in.
- VBUS: the 25.3 Ω only (QT Py not connected). NUCLEO-G431 sink on the Type-C cable. **No pack.**
- Scope: CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out (TP2, ahead of the output switch).
- Script: `scripts/pd_seedfix.py`, a copy of 2026-11-15's `pd_final.py` with a `pulse` phase (resistor only) and
  the contract check on the median of five status reads.

## The change (commit 001f42e)

At a CCM exit the DCM integrator is now seeded from the lower of CCM's filtered current and the newest per-cycle
reading (`buck_ccm_exit_seed_ma`). After a drop (newest under half the filtered), DCM's offset is the per-voltage
table's, not the one learned at the higher load at the CCM entry. This is for the 2026-11-15 21:41 SW_OVP at 15 V:
1586 mA into 13 mA, +1.97 V in one PID cycle.

Host test (`tests/buck_ccm`, `test_exit_seed`, single-PID-cycle bound at 40 µF, no load): with the trace's numbers
the seed is 182 / 163 / 96 mA at 5 / 9 / 15 V, i.e. +0.21 / +0.19 / +0.10 V in one cycle (the bench seed: +1.98 V).
The loop's correction in the cycles after is not modelled.

## Regression (17:00–17:03)

| Phase | Result |
|---|---|
| base | mailbox START and SET_VOLTAGE refused `PD_OWNS_VBUS` |
| walk, first run (16:59) | aborted at the second 5→15 V step on one status read of 16747 mV (below) |
| walk (17:00) | 2 × 5→9→15→9→5→15→5 V, 12 of 12 contracts, 0 Hard Resets; PS_RDY 60 / 84 / 134 ms as on 11-15 |
| hold (17:02) | 15 V for 30 s, 166 reads, RUNNING throughout; V_out mean 14967 mV, ID_MON 565–567 mA |
| pulse (17:02) | `switch-pulse` 5 ms at 15 V / 0.59 A (DCM): traced V_out 14539–15212 mV, RUNNING after, no fault |
| detach (17:02) | Rd off at 15 V: TP2 14.8 V → 0 V (scope, 50 ms means); Rd back: 15 V again, but with one Hard Reset (below) |

- **The 16747 mV read.** One status read (one ADC sample) 0.5 s after the 5→15 V contract, in DCM, skip count 0,
  no fault (SW_OVP needs 3 PID cycles over 16.5 V); the read 3 s later was 15695 mV and the steady reads after it
  were in range. The scope record of that step (−100 to +140 ms) rises monotonically to 15.26 V at most, but does
  not cover the read. Inference: the single-read noise seen on 11-15 (14.3–15.6 V at 15 V), with a larger outlier.
  The walk now checks the median of five reads; the rerun passed.
- **Detach.** The script's "a read under 800 mV" check failed as on 11-15 (status ADC fields freeze at IDLE, here at
  4570 mV); TP2 on the scope is the evidence. 127 ms after the detach the source attached again for 13 ms (TP2
  0.7 V for one 50 ms bucket), as on 11-15.
- **Hard Reset on the re-attach (new tonight, not on 11-15).** Source events: CAD_ATTEMC, Discover Identity,
  SRCCAP_SENT, **HARDRESET_TX 28 ms later**, POWER_NOT_READY, detach, attach, contract. TP2: 5 V for about 50 ms,
  back to 0, then 15 V from about 1.1 s later. Counters after: 4 attaches, 3 detaches, 1 Hard Reset; the sink's
  Hard Reset count 31 → 32. Cause not looked into. No CCM ran, so the seed change is not on this path.

## Not tested

The new seed itself: the 25.3 Ω (0.59 A at 15 V) stays in DCM, so no auto CCM entry or exit ran (`ccm_exits` 0).
The case it is for needs a load over the CCM entry that drops fast (the power bank).

## End state

RUNNING at the 5 V contract, no fault, `ccm_exits` 0. QT Py not connected.
