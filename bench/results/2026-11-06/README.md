# 2026-11-06: PPS Current Limit in DCM (CCM auto), below-spec limits

Firmware: as on 2026-11-05 (`PD_BENCH_15V/20V/24V/EPR_28V/FORCE_VCONN/PPS`) with two changes:
- **No forced CCM in PPS (Dan):** a sudden drop in V_out may need DCM. `pps_enter` now leaves
  `regulator_ccm_enable` as it is (auto, 1).
- **One current limit for CCM and DCM** (regulator.c step 5d). `buck_ccm_limit` runs every PID cycle on CCM's
  current (ID_MON corrected) and lowers the reference of whichever loop runs:
  - the DCM PID tracks min(V_ref, v_lim);
  - CCM's law takes v_cmd/limiting from it;
  - CCM entry is bumpless against v_cmd.

V_in 24.4 V. Load: 25.3 Ω only; the QT Py is off. The limit floor `pd_pps_cl_min_ma` was set to 100 mA over SWD
(non-compliant, Dan's OK) and put back to 1000 at the end. Script: `scripts/pps_cl.py`. Data:
`data/pps_cl.jsonl` (run 2) and `data/pps_cl_run1.jsonl` (run 1, Hard Reset). Regulator status was sampled every
~0.19 s. "Tail" is the mean of the last 5 samples; "V/R" is inferred (V_out / 25.3 Ω).

## 1. Run 1: Hard Reset on release (fixed)

`pps 9000 250` folded the output to 5.89 V; then `pps 9000 1500` (release) → `TRANSITION_TIMEOUT` 22 ms after
SETUP_POWER, then Hard Reset.
- **Cause:** SetProfile sized the step from the old setpoint (9000 → 9000 = 0 mV), so it allowed the 22 ms
  small budget while V_out had to climb from 5.9 V.
- **Fix:** the step is now the larger of the steps from the setpoint and from V_out (`usbpd_pwr_if.c`).

## 2. Run 2: all steps, 0 Hard Resets

| Request | PS_RDY wait (ms) | Limiting | CCM | V_out tail (mV) | I limit input, tail (mA) | V/R (mA) |
|---|---|---|---|---|---|---|
| 9000 mV, 1500 mA (CV) | 8 | 0 % | never | 9023 | 369 | 357 |
| 9000, **250** | 8 | 100 % | never | 5812 | 250 | 230 |
| 9000, 1500 (release) | 8 | 0 % | never | 9009 | 369 | 356 |
| 21000, 1500 (CV) | 160 | 0 % | never | 21027 | 834 | 831 |
| 21000, **500**, held 60 s | 8 | 100 % (320 samples) | never | 12507 | 501 | 494 |
| 21000, **300** (12.5 → 7.2 V) | 8 | 100 % | never | 7209 | 300 | 285 |
| 21000, 1500 (release) | 8 | 0 % | never | 21014 | 837 | 831 |
| 9000, 1500 | 160 | 0 % | never | 8992 | 370 | 355 |

- **Limit holds in DCM.** The limit's own input sits on the limit (248–251, 496–504 and 299–302 mA after 1 s).
  Against V/R: −20, −6 and −15 mA, inside iPpsCLNew ±150 mA. ID_MON in DCM reads 4–8 % above V/R (inferred).
- **Mode.** CCM was never entered, at any step: auto mode stays in DCM up to 834 mA here. **The limit in CCM (the
  shared limiter with CCM's law, entry while limiting) is not exercised by this run.**
- **Hold.** 60 s at 21 V / 500 mA with the sink's 5 s re-requests: 0 Hard Resets, RUNNING throughout, no pulse
  skipping. `pd_status` vPpsShutdown resets: 0.
- **V_out outliers.** Two single samples of 316 in the 60 s hold: 11.0 V at 25.5 s and 19.4 V at 49.0 s. The
  current (503 mA) and on-time (1118–1123 ns) were unchanged at those samples, so they look like VD_MON
  single-sample outliers, not output moves (inference).
- **Not resolved.** Fold and release settling are within the first sample (≤ 0.33 s); the sampling cannot check
  tPpsCLProgramSettle / tPpsCLCVTransient (250 / 275 ms). PS_RDY on a release goes at 8 ms while the limiter is
  still lowering the reference (counted as "in CL").

## 3. Run 3: forced CCM (`regulator_ccm_enable` = 2 over SWD, Dan), 0 Hard Resets

`pps_cl.py 100 ccm`: the same requests with CCM forced, then one fold started in DCM (auto) with CCM forced
during it. Last 14 lines of `data/pps_cl.jsonl`. `regulator_ccm_enable` went back to 1 at the end.

| Step | Limiting | CCM | V_out tail (mV) | I limit input, tail (mA) | V/R (mA) | Error vs V/R |
|---|---|---|---|---|---|---|
| CCM forced at 9000, 1500 (CV) | 0 % | entered | 8930 | 341 | 353 | −12 |
| 9000, **250** | 100 % | yes | 5919 | 249 | 234 | +15 |
| 9000, 1500 (release) | 0 % | yes | 9023 | 342 | 357 | −15 |
| 21000, 1500 (CV) | 0 % | yes | 20979 | 706 | 829 | −123 |
| 21000, **500**, held 30 s | 100 % | yes | 17785 | 500 | **703** | **−203** |
| 21000, **300** (17.8 → 7.6 V) | 100 % | yes | 7599 | 300 | 300 | 0 |
| 21000, 1500 (release) | 0 % | yes | 20976 | 686 | 829 | −143 |
| auto: 21000, 500 | 100 % | exited (LIGHT_LOAD) | 12559 | 500 | 496 | +4 |
| then CCM forced during the fold | 100 % | entered | 17630 | 502 | **697** | **−195** |
| auto: 9000, 1500 | 0 % | exited (RAMP_DOWN) | 8972 | 370 | 355 | +15 |

- **The limiter works in CCM.** It entered limiting from CCM, held for 30 s, took the 17.8 → 7.6 V drop without a
  CCM exit or fault, entered CCM while limiting, and stayed limiting through LIGHT_LOAD and RAMP_DOWN exits. In
  each case its input sat on the limit.
- **But in CCM at 18–21 V its input (ID_MON corrected) reads 15–29 % below V/R.** At a 500 mA limit the resistor
  took about 700 mA (inferred from V/R), 200 mA over iPpsCLNew's ±150 mA. At 6–9 V in CCM, and in DCM at every
  point, the error was within ±20 mA. The cause is not established. The same 21 V / 829 mA point reads 834 mA in
  DCM (run 2) and 686–706 mA in CCM.
- **CCM entry while limiting.** V_out moved from 12.56 V to 17.1 V by the first sample (0.39 s): the
  reading's offset at the new mode, not a jump in the limit (the input stayed at 493–503 mA).
- **Fold speed.** 17.8 → 7.6 V in CCM: 9.64 V at the first sample (0.31 s) and 7.83 V at 0.50 s. That may miss
  tPpsCLProgramSettle (250 ms); the sampling cannot show it.

## 4. ID_MON in buck CCM vs DCM (sweep)

`scripts/ccm_idmon.py`, data `data/ccm_idmon.jsonl`. PPS CV at 5–21 V (1500 mA, no limit reached) into 25.3 Ω, each
point read in auto (DCM) and then with `regulator_ccm_enable` = 2, buck at 200 kHz both ways. Each value is the mean
of 10 status reads. V_out and R are the same in both modes, so the CCM/DCM ratio is the reading's change.

| V_out (V) | V/R (mA) | DCM: ID_MON raw | ratio to V/R | CCM: ID_MON raw | ratio to V/R | CCM − DCM (mA) | IS_MON CCM/DCM |
|---|---|---|---|---|---|---|---|
| 5 | 198 | 235 | 1.18 | 235 | 1.20 | 0 | 0.78 |
| 7 | 278 | 309 | 1.11 | 302 | 1.09 | −7 | 0.87 |
| 9 | 355 | 388 | 1.09 | 361 | 1.02 | −26 | 0.93 |
| 11 | 434 | 469 | 1.08 | 398 | 0.91 | −71 | 0.95 |
| 13 | 513 | 544 | 1.06 | 427 | 0.83 | −117 | 0.95 |
| 15 | 593 | 610 | 1.03 | 463 | 0.78 | −147 | 0.97 |
| 17 | 671 | 685 | 1.02 | 522 | 0.78 | −164 | 0.95 |
| 18 | 712 | 715 | 1.00 | 557 | 0.78 | −158 | 0.94 |
| 20 | 791 | 811 | 1.03 | 664 | 0.84 | −147 | 0.93 |
| 21 | 828 | (auto stayed in CCM) | | 740 | 0.89 | (−137 against run 2's DCM 877) | |

- **The CCM reading drops by up to 164 mA (22–24 % at 15–18 V).** The drop grows from 0 at 5 V to about 150 mA at
  14 V and stays near 150 mA up to 20 V. DCM reads V/R + 17–37 mA throughout.
- **IS_MON (input) does not drop with it.** It reads 3–7 % lower in CCM at 9–20 V, while ID_MON reads 7–24 % lower.
  The same V_out into the same R is the same output, so the CCM error is in ID_MON. Output voltage is not the
  issue either: the 2026-11-05 CV run in forced CCM had TP2 within 0.2–0.4 V of VD_MON.
- **Not ripple or clipping.** In forced CCM at these loads the inductor ripple is about 3–6 A p-p (4.7 µH, 200 kHz), so
  the valley is negative. A clipped negative part, or Q3 being off while Q4 refreshes at the valley, would both make the
  reading high. The deficit also does not follow the ripple: 9 V and 15 V have the same 6 A p-p, and the deficits are
  26 and 147 mA.
- **Frequency.** The calibration that fitted (ID_MON = 1.05 × V/R + 24 mA, 2026-10-21) was forced CCM at 400 kHz,
  where Q4 refreshes every second period. At 200 kHz it refreshes every period. Not yet compared at the same points.

## 5. Run 4: forced-CCM PPS limit with the new ID_MON calibration

After the CCM handover work (`ccm_transitions/README.md`): ID_MON 1.059 × raw − 54 mA, plus the CCM duty correction
at 200 kHz. Same ladder as run 3 (`pps_cl.py 100 ccm`), last 14 lines of `data/pps_cl.jsonl`. 0 Hard Resets, RUNNING
and no fault throughout; `regulator_ccm_enable` back to 1 at the end.

| Step | CCM | V_out tail (mV) | I limit input, tail (mA) | V/R (mA) | Error vs V/R (run 3) |
|---|---|---|---|---|---|
| 9000, **250** | yes | 6329 | 251 | 250 | +1 (+15) |
| 21000, 1500 (CV) | yes | 20993 | 836 | 830 | +6 (−123) |
| 21000, **500**, held 30 s | yes | 13035 | 500 | 515 | −15 (**−203**) |
| 21000, **300** (13.0 → 7.7 V) | yes | 7692 | 300 | 304 | −4 (0) |
| auto: 21000, **500** | exited (LIGHT_LOAD) | 12449 | 500 | 492 | +8 (+4) |
| then CCM forced during the fold | entered | 13021 | 500 | 515 | −15 (**−195**) |

- **Inside iPpsCLNew (±150 mA) at every point:** −15 to +8 mA against V/R (inferred), from −203 mA in run 3.
- **CCM entry while limiting:** V_out moved 12.45 → 13.02 V, true current 492 → 515 mA (inferred). Before the
  calibration the same step moved it to 17.6 V.
- **Fold speed:** 13.0 → 7.7 V was at 7.97 V by the first sample (0.31 s); run 3's larger 17.8 → 7.6 V fold was at
  9.64 V there.

## 6. Not done

- ID_MON in CCM at high V_out: why it reads low. A correction is in since `ccm_transitions/` (CCM and DCM within
  ±13 mA of V/R at 5–21 V, 200 kHz); the PPS limit in CCM re-run with it is run 4 (section 5).
- vPpsShutdown Hard Reset (fold below 4.5 V, e.g. 5 V / 150 mA): not run.
- tPPSTimeout (P3) and Get_PPS_Status: as on 2026-11-05.
