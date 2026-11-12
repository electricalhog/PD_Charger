# Bench 2026-11-11: LiPo-load branch (`claude/lipo-load`)

Board, 24.4 V bench supply and tools as on 2026-11-09 (`../2026-11-09/README.md`).

## 1. IS_MON recalibration on 25.3 Ω (in progress)

Dan found a ground loop in parallel with his input ammeter on 2026-11-09, so it read low. The IS_MON fit of
2026-11-09 section 10 (855 / +10, `IS_GAIN_PERMILLE_DEFAULT`, `IS_OFFSET_MA_DEFAULT`) rests on those readings.
That is a likely cause (inference) of the CCM power balance over 100 % in section 17 there.

- **Load:** the 25.3 Ω resistor on VBUS, no pack. Dan's ammeter on the input.
- **Run:** `scripts/is_cal_sweep.sh`. Buck DCM in voltage mode at 5, 8, 11, 14, 17 and 20 V ceilings, 20 s each, 8 s
  idle before and between. The 1500 mA limit never acts.
- **Logging:** the run log now records `is_raw_ma`, IS_MON before its calibration (`bu battery run`), so the fit
  can start from the raw reading.
- `is_cal_2-2.jsonl`: a run with a 2 mV ceiling, which the firmware refused (OUT_OF_RANGE). Nothing switched.

**Results** (`is_cal_5000-20000.jsonl`). Every step ran without a fault, in DCM, with the limit never acting.
Dan's meter read 6.7 mA with the regulator idle.

| Ceiling | V_out | I_out (ID_MON) | IS_MON raw, mean ± σ | IS_MON at 855/+10 | Meter | Fit residual | P_in (meter) | P_out | Efficiency |
|---|---|---|---|---|---|---|---|---|---|
| 5 V | 5018 mV | 198.5 mA | 80.5 ± 2.4 mA | 55.0 mA | 84.8 mA | +0.4 mA | 2.08 W | 1.00 W | 47.9 % |
| 8 V | 7968 mV | 315.5 mA | 157.1 ± 3.7 mA | 121.0 mA | 161.9 mA | +0.2 mA | 3.96 W | 2.51 W | 63.5 % |
| 11 V | 11012 mV | 432.2 mA | 260.0 ± 3.1 mA | 219.9 mA | 265.4 mA | −0.3 mA | 6.49 W | 4.76 W | 73.3 % |
| 14 V | 14004 mV | 552.7 mA | 392.8 ± 4.1 mA | 328.7 mA | 399.2 mA | −0.5 mA | 9.75 W | 7.74 W | 79.4 % |
| 17 V | 17005 mV | 664.6 mA | 537.4 ± 4.2 mA | 451.0 mA | 545.7 mA | −0.1 mA | 13.32 W | 11.30 W | 84.8 % |
| 20 V | 20006 mV | 784.4 mA | 713.9 ± 2.3 mA | 599.8 mA | 724.4 mA | +0.4 mA | 17.69 W | 15.69 W | 88.7 % |

- **Fit:** meter = 1.0098 × IS_MON + 3.1 mA. It is now `IS_GAIN_PERMILLE_DEFAULT` 1010 and `IS_OFFSET_MA_DEFAULT` 3
  (pending ELF, section 2). The old fit read 13–17 % low.
- P_in is V_in (VS_MON) × the meter. P_out is V_out (VD_MON, before Q5/Q6) × ID_MON.

**The 2026-11-09 section 17 runs re-read with this fit.** Raw IS_MON was recovered from the 855/+10 value and the
output current is Dan's output meter:

| Step | I_in (new fit) | P_in | P_out | Efficiency | Loss |
|---|---|---|---|---|---|
| DCM 250 mA | 215 mA | 5.25 W | 3.91 W | 74.4 % | 1.35 W |
| DCM 500 mA | 398 mA | 9.73 W | 7.76 W | 79.7 % | 1.97 W |
| DCM 1000 mA | 789 mA | 19.29 W | 15.75 W | 81.7 % | 3.54 W |
| DCM 1500 mA | 1176 mA | 28.75 W | 23.66 W | 82.3 % | 5.09 W |
| DCM 1984 mA (not limiting) | 1531 mA | 37.42 W | 31.38 W | 83.9 % | 6.04 W |
| CCM 2000 mA | 1352 mA | 33.02 W | 31.49 W | 95.4 % | 1.52 W |
| CCM 2500 mA | 1683 mA | 41.03 W | 39.76 W | 96.9 % | 1.27 W |
| CCM 3000 mA | 2019 mA | 49.25 W | 48.25 W | 98.0 % | 1.01 W |
| CCM 3500 mA | 2356 mA | 57.49 W | 56.37 W | 98.1 % | 1.11 W |

- **CCM no longer comes out over 100 %.** But its loss falls as the current rises, 1.5 W at 2 A to 1.1 W at 3.5 A,
  and that is not physical.
- **DCM at 2 A comes out at a 6 W loss.** Dan had the board only a few degrees over room temperature after
  20 s steps there (2026-11-09 section 10). That makes 6 W doubtful too (inference: a 20 s step need not reach
  steady state).
- **The fit only covers DCM, up to 0.72 A, into 25.3 Ω** at 5–20 V out. The pack runs are at 15.5 V, up to 2.4 A in,
  and in CCM. The likely explanation is that IS_MON is mode-dependent or nonlinear outside that range (inference).
  Input-meter readings on the pack, in DCM and CCM, would settle it.

## 2. CCM entry with a current limit: the reference from V_out (host only; ELF built, not flashed)

**The problem (2026-11-09 section 17):** every CCM step entered from DCM at its ceiling, about 2 A, not limiting.
- The limit (step 5d) runs before the entry decision. So at the entry it was not limiting, and the CCM law started
  on V_ref (pack + 800 mV).
- The voltage law drove the whole gap at once, and the limit acted only once an ID_MON scan read over the setting.
- The first scan over the setting read 3.3, 4.8, 5.2 and 5.3 A at the 2, 2.5, 3 and 3.5 A settings, near ID_MON's
  5.5 A full scale.

**Change (`buck_ccm_limit_from`, called at the CCM entry in `regulator.c` when a limit is set):**
- If V_out is below V_ref, the limit engages with v_lim = V_out. v_lim then rises at kl (I_lim − I) until the
  current reaches the limit, or V_out reaches V_ref and the limit hands back.
- u0 is preset against that reference, so the first duty is the same as before.
- **Entry ramp (`s->ramp`):** the voltage integrator u keeps running until the current first reaches the limit, or
  the limit hands back. Otherwise u is held while limiting, as before.
  - Why: with u held at a wrong preset, V_out sits off v_lim. v_lim can then reach V_ref under the limit, hand back,
    and u winds up at once.
- `regulator_ccm_entry_ramp` (RAM, `CCM_ENTRY_RAMP_DEFAULT` 1). 0 gives the old entry.

**Host model correction:** the lag models of 2026-11-09 sections 15–16 reused the floor sims' plant. That plant has
L = 50 × 4.7 µH, a stability device for checks of the current's sign only. Into 0.13 Ω it adds a 1.8 ms pole that
is not on the board.
- `test_ilim_lag` and `test_ilim_entry` now use `pack_plant_us`: L1 4.7 µH into C_int 50 µF (V_out), R to the pack,
  stepped at 0.1 µs.
- **The section 15 diagnosis stands:** the 8-scan mean at kl 400, 0.17 Ω still oscillates (3.0 A p-p).

| Newest scan, limit 2 A | 0.05 Ω | 0.08 Ω | 0.10 Ω | 0.13 Ω | 0.17 Ω |
|---|---|---|---|---|---|
| kl 100 (p-p) | 0.15 A | 0.06 A | 0.05 A | 0.04 A | 0.03 A |
| kl 400 (p-p) | 71 A (unstable) | 0.28 A | 0.10 A | 0.08 A | 0.03 A |

- Section 16's "400 oscillates at 0.10 Ω" was the 50× plant. On the real one, 400 holds down to 0.08 Ω. CCM kl 100
  stays, with about 4× more margin.

**Entry model** (`test_ilim_entry`): from DCM at 2 A, ceiling pack + 0.8 V, plant 0.5 V over the model, u0 preset
as in `regulator.c` (−0.627 V base, about 0.13 V short of the plant), kl 100, newest scan.

| R | Peak, from V_ref (old), any limit | Peak, from V_out, 2 / 2.5 / 3 / 3.5 A | Lowest after the entry, from V_out |
|---|---|---|---|
| 0.10 Ω | 7.15 A | 2.05 / 2.59 / 3.04 / 3.53 A | 0.61–0.92 A |
| 0.13 Ω | 5.50 A | 2.03 / 2.53 / 3.02 / 3.52 A | 0.97–1.15 A |
| 0.17 Ω | 4.25 A | 2.02 / 2.52 / 3.01 / 3.51 A | 1.20–1.28 A |

- **The dip after the entry** is the first duty's error, u0 against the plant, over R. It is gone in about 1 ms as u
  runs.
  - On the board, u0's base was within about 18 mV at 2 A (inference from section 16's trace: u −234 mV held,
    V_out − v_cmd +144 mV). That would be about 0.15 A at 0.13 Ω.
- **u0 sensitivity** (scratch runs, 3.5 A limit; u held in the ramp / u running):

| u0 base | 0.10 Ω | 0.13 Ω | 0.17 Ω |
|---|---|---|---|
| −0.3 V (0.2 V high) | 4.74 / 3.68 A | 4.13 / 3.52 A | 3.62 / 3.51 A |
| −0.9 V (0.4 V low) | 3.53 / 3.78 A | 5.50 / 3.52 A | 3.84 / 3.51 A |

  - With u0 far off, either way the first period's error still shows. At −0.9 V the current dips negative for a
    moment: −0.2 to −1.4 A with u running.

**Build:** `build/BatteryNext/final.elf` (sha1 `185dc8cd…`), 69.8 KB flash, no warnings. It was built outside
`build/Battery` so that `bu` keeps resolving the flashed ELF's (`3b787a7e…`) symbols during the IS_MON sweep. Not
flashed.

**To check on the bench:** CCM steps at 2, 3 and 3.5 A with the trace armed, `regulator_ccm_entry_ramp` 1. A step at
0 for comparison only if wanted.

## 3. Entry fix on the board, and IS_MON on the pack in DCM and CCM

**Flash:** `bu build` and `bu flash` gave `build/Battery/final.elf`, sha1 `ea98d87d…`. Its flash image is
byte-identical to `build/BatteryNext` `5b2e3fbd…` (objcopy and cmp; only the debug paths differ). Read back:
- IDLE, `is_gain_permille` 1010, `is_offset_ma` 3, `regulator_ccm_entry_ramp` 1.
- `ccm_ilim_kl_ccm` 100, `ccm_ilim_fast` 1.

**Run:** `scripts/ilim_mode_sweep.sh dcm 1000`, then `ccm 2000 3000 3500` (this folder's copy; `ilim_dcm_1000-1000.jsonl`,
`ilim_ccm_2000-3500.jsonl`, logs and traces in `runs/`).
- The 4S pack is back, with Dan's ammeter on the **input**.
- 20 s steps, ceiling pack + 800 mV, as in 2026-11-09 section 17.
- No fault on any step. Every CCM step was CCM and limiting in every sample, from the entry on.

**Input current: IS_MON against the meter** (board values are the mean of the last 60 samples):

| Step | Pack read | V_in | V_out | I_out (ID_MON) | IS_MON (1010/+3) | Meter | IS − meter |
|---|---|---|---|---|---|---|---|
| DCM 1000 mA | 15367 mV | 24402 mV | 15437 mV | 1001 mA | 771 mA | 762.5 mA | +1.1 % |
| CCM 2000 mA | 15350 mV | 24352 mV | 15571 mV | 2002 mA | 1323 mA | 1.323 A | 0.0 % |
| CCM 3000 mA | 15401 mV | 24222 mV | 15793 mV | 2996 mA | 1985 mA | 1.977–1.984 A | +0.2 % |
| CCM 3500 mA | 15384 mV | 24064 mV | 15785 mV | 3504 mA | 2325 mA | 2.304–2.335 A | +0.2 % |

The 25.3 Ω fit (section 1) holds on the pack, in DCM and in CCM, up to 2.3 A in.

**Power balance with both currents checked against the meter** (the output current on 2026-11-09):

| Step | P_in (V_in × meter) | P_out (V_out × I_out) | Efficiency | Loss |
|---|---|---|---|---|
| DCM 1000 mA | 18.61 W | 15.45 W | 83.0 % | 3.16 W |
| CCM 2000 mA | 32.22 W | 31.17 W | 96.8 % | 1.05 W |
| CCM 3000 mA | 47.97 W | 47.32 W | 98.6 % | 0.65 W |
| CCM 3500 mA | 55.82 W | 55.31 W | 99.1 % | 0.51 W |

- **The CCM loss still falls as the current rises.** Both currents are now checked against a meter, so the error is
  in the voltages (inference): VD_MON (V_out, before Q5/Q6) reading high in CCM, or VS_MON (V_in) reading low.
- V_in sagged to 24.06 V at 3.5 A.
- Not checked: a DMM on V_in and V_out during a CCM step.

**Entry traces, against 2026-11-09 section 17** (same ceiling and pack):

| Limit | Highest scan, old entry | Highest scan, new | Within 10 % from | Steady (10–75 ms) | u at entry → steady |
|---|---|---|---|---|---|
| 2000 mA | 3344 mA | 2458 mA (0.7 ms) | 2.95 ms | 1999 ± 32 mA | −358 → −363 mV |
| 3000 mA | 5204 mA | 3333 mA (3.95 ms) | 5.6 ms | 2997 ± 33 mA | −359 → −374 mV |
| 3500 mA | 5322 mA | 4892 mA (3.65 ms) | 7.0 ms | 3490 ± 63 mA | −358 → −131 mV |

- The overshoot at 2 and 3 A is about 0.46 and 0.33 A, from 1.3 and 2.1 A before. At 3.5 A it is barely better.
- **What the 3.5 A trace shows:**
  - At the entry, 1.75 A read, v_cmd started at 15660 mV.
  - In the first 0.5 ms the current read 3045 mA while v_cmd rose only 48 mV. The first duty itself drove the
    current up.
  - u ran from −358 to −131 mV in 2.5 ms, until a scan read over 3.5 A (3825 mA). The ramp ended there with u at
    −131 mV, the current still rising.
  - It peaked at 4892 mA at 3.65 ms. v_cmd then came down to 15336 mV by 8 ms.
- **Inference:** the first CCM duty (u0 from V_out at the entry, the CCM_U0 base) is high for the current L1 carries.
  - At the entry, v_cmd + u = 15302 mV at 1.75 A. Steady at 3.5 A it was 15317 mV, and at 2 A 15124 mV.
  - So the entry duty already nearly carries the 3.5 A one. The ramp's u integration then adds to it, and the
    measured current, 0.34 ms behind, ends the ramp late.
- In the 2 A and 3 A traces the same first jump shows: 1745 → 2892 mA by 1.0 ms at 3 A.

## 4. V_in and V_out against the scope (CH2 V_in, CH4 V_out)

**Setup:** CH2 is on V_in and CH4 on V_out, both 10× probes (Dan). `scripts/scope_v_check.sh`, results in
`scope_v_check.jsonl` and `scope_v_check.log`.
- Scope settings: DC coupling, 2 V/div, 20 MHz bandwidth limit, 100 µs/div. The CH2 offset is −20 V, the most the
  scope allows (it clamps −24 V to −20 V). The CH4 offset is −15.5 V.
- CH4 had no average at idle, with the output switch open, so it is on the converter side of Q5/Q6, the node VD_MON
  reads.
- Steps on the 4S pack: DCM 1000 mA and CCM 3500 mA, ceiling pack + 800 mV, 20 s each.
- **Timing:** each `bu scope measure` took about 3–4 s, so only readings 1–3 of each step fell inside the run.
  Readings 4–5 are after it (V_out back at idle) and are dropped. The board mean is over 6–18 s of the run.

| Step | V_in scope | V_in board | Board − scope | V_out scope | V_out board | Board − scope | I_out | IS_MON |
|---|---|---|---|---|---|---|---|---|
| idle | 24.57 V | — | — | — | — | — | — | — |
| DCM 1000 mA | 24.511 V | 24.420 V | −0.09 V | 15.185 V | 15.594 V | **+0.41 V** | 1.000 A | 0.775 A |
| CCM 3500 mA | 24.307 V | 24.275 V | −0.03 V | 15.315 V | 15.830 V | **+0.52 V** | 3.501 A | 2.309 A |

- **V_out p-p at the probe:** about 1 V in DCM (14.64–15.6 V), about 4 V in CCM (12.9–17.2 V).
- **Scope accuracy:** these are the scope's own VAVG readings, which Dan does not trust (2026-11-11: the scope is
  accurate when the median is computed from the CSV trace). Sections 5 and 6 use trace medians and supersede the
  numbers in this section. The datasheet-based caution written here first no longer applies to those.
  - It agrees with 2026-11-09: the board read the pack at 15505 mV against Dan's 15.2 V (+0.3 V).
  - Between DCM and CCM, on the same channel and settings, the board's excess grows by 0.11 V.
- **Efficiency with the scope's V_out and V_in** (I_out ID_MON, I_in IS_MON, both checked against the meter):

| Step | With the board's voltages | With the scope's |
|---|---|---|
| DCM 1000 mA | 82.4 % | 79.9 % (3.81 W loss) |
| CCM 3500 mA | 98.9 % | 95.5 % (2.50 W loss) |

- **To settle VD_MON:** a DMM on the pack terminals while the board takes its pack read (switch closed, every FET
  off). It is a static node, so a DMM is exact there, and VD_MON reads it through the closed Q5/Q6.

**Entry trace (section 3, CCM 3500 mA) against this:** from the entry to 2.4 ms, V_out (VD_MON) stayed within about
20 mV of v_cmd: 15660 → 15746 mV. Over that time the current read 1.75 → 3.8 A and u rose 0.23 V.
- Through about 0.12 Ω, 2 A more should raise V_out by about 0.25 V. The reading rose 0.09 V.
- **Inference:** VD_MON's reading does not follow V_out one for one through the entry, so a small error held u
  integrating up. That fits VD_MON's bias changing between DCM and CCM, as measured above.

## 5. Power ramp, 1.5 to 69 W, with scope captures at every point

**Run:** `scripts/power_ramp.py`, then `scripts/ramp_table.py` and `scripts/ramp_figures.py`. Data in `ramp/`
(`ramp.json`, `points.json`, `board_dcm.json`, `board_ccm.json`, the capture CSVs, 68 MB). Figures in `figures/`.
- **Two battery runs on the 4S pack, ceiling pack + 800 mV.** The current limit was stepped in RAM inside each run.
  - DCM (`regulator_ccm_enable` 0): 100 to 2000 mA.
  - CCM: entered once at 2500 mA, walked down to 1300 mA (just above the light-load exit, about 1.19 A), then up to
    4500 mA.
- **No fault.** Every point was limiting, and the CCM run stayed in CCM at every point.
- **Per point:** 2.5 s settle, then one raw capture of CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out (30k points, 240 µs,
  125 MSa/s). At five points a second capture of CH4 AC-coupled (300k points, 2.4 ms) for the spectrum.
- **Efficiency:** I_out is ID_MON and I_in is IS_MON, both checked against the ammeter (within 0.3 % and 1.1 %).
  V_in and V_out are the medians of the scope's trace samples over the 240 µs capture (30k samples), not the
  board's readings and not the scope's own "measured" values.
  - Dan, 2026-11-11: the scope is accurate when the median is computed from the CSV trace. The table was first made
    with the trace means; the medians moved every efficiency by 0.17 points or less (V_in by ≤ 26 mV, V_out by
    ≤ 13 mV).
  - The median is interpolated inside the 92 mV code step (`med()` in `scripts/ramp_table.py`), so it is not
    rounded to a code. `points.json` keeps the plain median and the mean beside it.
- **Rectification:** `regulator_sr_enable` and `sr_blend` read 0 at every point. This build forces the rectifier
  windows off in DCM, so DCM runs through the body diodes. CCM drives both legs synchronously.

| Mode | Setting | I_out | I_in | V_in (scope) | V_out (scope) | P_out | Efficiency | Loss | VD_MON − scope | VS_MON − scope |
|---|---|---|---|---|---|---|---|---|---|---|
| DCM | 100 mA | 0.101 A | 0.082 A | 24.61 V | 15.09 V | 1.5 W | 75.5 % | 0.49 W | +0.23 V | -0.10 V |
| DCM | 250 mA | 0.251 A | 0.205 A | 24.64 V | 15.12 V | 3.8 W | 75.1 % | 1.26 W | +0.37 V | -0.15 V |
| DCM | 500 mA | 0.491 A | 0.381 A | 24.60 V | 15.13 V | 7.4 W | 79.3 % | 1.94 W | +0.25 V | -0.14 V |
| DCM | 750 mA | 0.743 A | 0.567 A | 24.59 V | 15.15 V | 11.3 W | 80.7 % | 2.69 W | +0.45 V | -0.13 V |
| DCM | 1000 mA | 1.017 A | 0.774 A | 24.58 V | 15.16 V | 15.4 W | 81.1 % | 3.60 W | +0.45 V | -0.14 V |
| DCM | 1250 mA | 1.261 A | 0.961 A | 24.56 V | 15.18 V | 19.1 W | 81.1 % | 4.46 W | +0.37 V | -0.14 V |
| DCM | 1500 mA | 1.511 A | 1.156 A | 24.54 V | 15.19 V | 23.0 W | 80.9 % | 5.41 W | +0.39 V | -0.14 V |
| DCM | 1750 mA | 1.756 A | 1.341 A | 24.53 V | 15.21 V | 26.7 W | 81.2 % | 6.19 W | +0.40 V | -0.16 V |
| DCM | 2000 mA | 2.003 A | 1.532 A | 24.51 V | 15.23 V | 30.5 W | 81.2 % | 7.05 W | +0.40 V | -0.15 V |
| CCM | 2500 mA | 2.502 A | 1.644 A | 24.50 V | 15.25 V | 38.1 W | 94.7 % | 2.14 W | +0.41 V | -0.13 V |
| CCM | 2000 mA | 2.007 A | 1.327 A | 24.52 V | 15.23 V | 30.6 W | 94.0 % | 1.97 W | +0.30 V | -0.12 V |
| CCM | 1750 mA | 1.750 A | 1.164 A | 24.53 V | 15.23 V | 26.6 W | 93.3 % | 1.92 W | +0.34 V | -0.10 V |
| CCM | 1500 mA | 1.504 A | 1.006 A | 24.54 V | 15.22 V | 22.9 W | 92.7 % | 1.81 W | +0.31 V | -0.09 V |
| CCM | 1300 mA | 1.303 A | 0.880 A | 24.56 V | 15.21 V | 19.8 W | 91.7 % | 1.79 W | +0.27 V | -0.13 V |
| CCM | 3000 mA | 2.998 A | 1.979 A | 24.48 V | 15.29 V | 45.8 W | 94.6 % | 2.61 W | +0.69 V | -0.14 V |
| CCM | 3500 mA | 3.495 A | 2.302 A | 24.44 V | 15.32 V | 53.6 W | 95.2 % | 2.71 W | +0.54 V | -0.13 V |
| CCM | 4000 mA | 4.002 A | 2.634 A | 24.41 V | 15.36 V | 61.5 W | 95.6 % | 2.84 W | +0.55 V | -0.14 V |
| CCM | 4500 mA | 4.496 A | 2.970 A | 24.40 V | 15.39 V | 69.2 W | 95.5 % | 3.28 W | +0.48 V | -0.14 V |

- **69 W is the top of this setup, not 100 W.** At 15.4 V out, 100 W is 6.5 A. ID_MON's full scale is 5.5 A and
  SW_OCP is at 5 A, so 4.5 A was the highest step. 100 W needs a higher output voltage (a 6S pack at 22–25 V, or
  20 V 5 A on the PD build).
- **At the same 23–30 W, CCM loses 1.8–2.0 W where DCM loses 5.4–7.1 W.** DCM's loss is close to proportional to
  the current (about 3.5 W/A), which fits body-diode conduction (inference).
- **VD_MON against the scope:** +0.23 to +0.45 V in DCM, +0.27 to +0.69 V in CCM, largest at 3 A and above. The
  board's V_out is a median of single samples, about 9 samples a point, so each value is noisy. VS_MON is
  0.09–0.16 V under the scope throughout. Section 6 turns these into calibrations.
- **Board temperature was not recorded.** The CCM run spent about 60 s at 3 A and above.

**Figures:**
- `figures/efficiency.png`: efficiency and loss against output power, both modes, with the range where both can run
  (18–31 W).
- `figures/switch_nodes.png`: SW1 and SW2 over 12.5 µs at DCM 0.5 A, DCM 1.5 A, CCM 1.5 A and CCM 4.5 A.
- `figures/vout_noise.png`: V_out ripple (25 µs) and spectrum (2 kHz–20 MHz) at five load points.

**V_out noise** (CH4 AC-coupled, 20 MHz limit, 10× probe with a ground lead on the converter side of Q5/Q6):

| Mode | I_out | P_out | Ripple rms | p-p | 200 kHz line (rms) |
|---|---|---|---|---|---|
| DCM | 0.5 A | 7 W | 63 mV | 1.4 V | 39 mV |
| DCM | 1.5 A | 23 W | 104 mV | 1.5 V | 83 mV |
| CCM | 1.5 A | 23 W | 220 mV | 4.5 V | 105 mV |
| CCM | 3.0 A | 46 W | 235 mV | 4.4 V | 116 mV |
| CCM | 4.5 A | 69 W | 233 mV | 4.8 V | 126 mV |

- In CCM the p-p is edge spikes of about ±2 V, twice a period. They sit at the scope's ±2.5 V range at
  500 mV/div (no samples at the rail). With a ground lead on the probe, part of them is pickup (inference; a
  spring-tip measurement at the output capacitor would tell).
- Between the edges, the ripple is about 0.4 V p-p in CCM and 0.2–0.3 V in DCM.
- The spectra rise toward 20 MHz in every case, most in CCM: the edge ringing.

## 6. Voltage calibration against the scope's trace medians

**Basis:** Dan, 2026-11-11: the scope is accurate as long as its reported "measured" values are not used and a
median is computed from the CSV trace. So the 18 ramp points (section 5) are a voltage reference: CH2 on V_in, CH4
on V_out (converter side of Q5/Q6), 30k samples each.

**VS_MON (V_in): calibrated in the source, not flashed.**
- The board read 0.09–0.16 V under the scope at 24.40–24.64 V at every point: ratio 0.99462, standard deviation
  0.0007, the same in DCM (0.9943) and CCM (0.9949).
- `VS_MON_FULL_SCALE_MV` 69500 → 69876 (`regulator_config.h`). Gain only: one voltage, so no offset is resolvable.
- Built in `build/BatteryNext` (ELF sha1 `e415edc0…`). The board still runs `ea98d87d…` with 69500.

**VD_MON (V_out): measured, not changed.**

| Condition | VD_MON − scope at 15.1–15.4 V |
|---|---|
| Pack read (switch closed, every FET off) | +0.32 V (see below) |
| DCM, 0.1–2.0 A | +0.23 to +0.45 V, mean +0.37 V |
| CCM, 1.3–2.5 A | +0.27 to +0.41 V |
| CCM, 3.0–4.5 A | +0.48 to +0.69 V |

- **Pack read:** the board read the pack at 15401 mV before the DCM run. The scope's V_out at the first point
  (0.101 A) was 15.092 V, which is 15.08 V at the pack after 0.1 A through about 0.13 Ω. That is +0.32 V, and it
  matches Dan's DMM on 2026-11-09 (15505 mV against 15.2 V, +0.3 V). The two readings are a few seconds apart, not
  simultaneous.
- **The error depends on the operating point,** so one gain does not remove it. A gain fitted to the pack read
  (×0.979) would leave about +0.05 V in DCM and up to +0.37 V in CCM at 3 A.
- **Effect on the pack:** the board over-reads, so the real ceiling is 0.3–0.7 V below the commanded one. That is the
  safe direction for charging.
- **Not established:** whether the static +0.32 V is a gain or an offset (one voltage), and why the reading rises
  further while switching. The pack read and the running loop sample VD_MON differently (a software read at idle,
  HRTIM-triggered samples while running).

**Static check prepared, not run:** `scripts/v_static_check.py` captures the board's 30 ms pack read on CH4 and
V_in on CH2, with nothing switching, and compares trace medians with the board's values.
- Tried at about 21:10: the pack read returned 0 mV and START was refused with START_FAILED. The board then showed
  IDLE, no fault, V_in 16 mV and V_out 51 mV, so the pack and the 24 V supply were off by then. Nothing switched.
- The scope's trigger and timebase were put back afterwards (CH1, AUTO, 20 µs/div).

