# 2026-11-09: bench check after the setup was power-cycled

Firmware as flashed on 2026-11-06 (PPS, CCM handover, ID_MON calibration). Load 25.3 Ω, QT Py off.

## 1. Found: SW_UVP latched at power-up

- `bu regulator status`: FAULT, SW_UVP, fault at 136 ms after boot (`regulator_fault_tick_ms`).
- Fault snapshot (`regulator_fault_snapshot`): V_out 17 mV, **V_in 33 mV**, setpoint 5000 mV.
- So the sink attached and the regulator started for the 5 V contract before the 24 V input was up, and V_out
  could not rise (inference from the snapshot: the source boots from its ST-LINK USB before the bench supply).
- Since then, 254 attaches each ended in VBUS_ON_FAILED → Hard Reset, with the fault latched and no switching.
- VS_GOOD and IS_GOOD were 1 when checked, so input was present by then.

**Recovery:** one `bu regulator clear-fault` → RUNNING, V_in 24.42 V, V_out 4.97 V, 5 V contract.

**Gap (firmware):** a start with V_in at about 0 latches SW_UVP instead of waiting for the input. A source that
boots before its input supply has to be power-cycled or cleared by hand. Fixed in section 4.

## 2. Functional check after recovery

| Request | State | Mode | V_out (mV) | I_out, CCM's current (mA) | V/R (mA, inferred) | V_in (mV) | IS_MON (mA) |
|---|---|---|---|---|---|---|---|
| fixed 9 V | RUNNING | BUCK DCM | 9020 | 354 | 357 | 24382 | 196 |
| fixed 15 V | RUNNING | BUCK DCM | 14936 | 594 | 590 | 24382 | 444 |
| fixed 20 V | RUNNING | BUCK DCM | 19851 | 790 | 785 | 24365 | 747 |
| PPS 12 V, 1.5 A | RUNNING | BUCK DCM | 12038 | 486 | 476 | 24382 | 319 |
| PPS 5 V, 1.5 A | RUNNING | BUCK DCM | 4949 | 193 | 196 | 24399 | 87 |
| fixed 15 V | RUNNING | BUCK DCM | 15143 | 595 | 599 | 24365 | 441 |

Single status reads. All requests matched, no new Hard Reset (count 254 before and after), no fault.

At 15 V, mean of 20 reads: V_out 14954 mV, V_in 24373 mV, IS_MON 447.5 mA (σ 4.2), ID_MON corrected 595 mA.

## 3. IS_MON against Dan's ammeter (24 V input, 10 mA display steps)

Fixed contracts into 25.3 Ω, buck DCM, 200 kHz. IS_MON is the mean of 20 status reads (σ 3–4 mA). P_out is V²/R
(inferred).

| V_out (mV) | V_in (mV) | Ammeter (mA) | IS_MON (mA) | IS_MON − meter | P_out / P_in (meter) |
|---|---|---|---|---|---|
| 4990 | 24408 | 100 | 85.5 | −14.5 | 0.40 |
| 15028 | 24372 | 400 | 446.2 | +46.2 | 0.92 |
| 20052 | 24367 | 710 | 748.9 | +38.9 | 0.92 |

- **Not one line.** A least-squares line gives IS_MON = 1.087 × meter − 11.5 mA, with residuals of −12, +23 and
  −11 mA. IS_MON reads low at 5 V and high at 15–20 V.
- **Not separated yet.** The meter may include housekeeping current that IS_MON does not see, and buck DCM's input
  current is a short pulse (about 0.45 µs at 5 V). Either could bend the curve (inference).

**Zero point (Type-C unplugged, regulator IDLE, INPUT_EN off):** ammeter about 10 mA (housekeeping upstream of the
input switch). ADC3 DR (IS_MON) read directly at 0x50000440: 10–11 counts, so **IS_MON's zero is +32 mA**.

With the 10 mA housekeeping removed from the meter and the 32 mA zero removed from IS_MON (assuming the idle
housekeeping also holds while switching, which is not checked):

| V_out | Stage current, meter − 10 (mA) | IS_MON − 32 (mA) | Ratio |
|---|---|---|---|
| 5 V | 90 | 53.5 | 0.59 |
| 15 V | 390 | 414.2 | 1.06 |
| 20 V | 700 | 716.9 | 1.02 |

- At 15–20 V, IS_MON is a 32 mA offset plus a gain within 2–6 %.
- At 5 V it reads 40 % low. That fits the short DCM input pulse (about 0.45 µs) not being followed by the amplifier,
  but this is not checked (inference). A probe on IS_MON, or more points between 5 and 15 V, would check it.

## 4. Fix: a start without input stays IDLE (no SW_UVP latch)

`regulator_start()` now reads V_in (VS_MON) after the input path comes up. If it is below `VIN_START_MIN_MV`
(9000 mV, `regulator_config.h`), it drops the input path and returns IDLE with no fault. The next VBUSOn (the next
attach or Hard Reset) tries again. Refusals are counted in `regulator_start_low_vin_count`; the V_in of the latest
one is in `regulator_start_low_vin_mv`. `bu regulator status` shows both.

- **Why VS_GOOD did not catch it:** not established. VS_GOOD passed the start's wait, yet V_in read 33 mV.
- **Normal start (flashed, 9000 mV):** RUNNING, buck, V_in 24433 mV, V_out 4915 mV, 5 V contract, 0 refusals, 0 Hard
  Resets.
- **Refusal path (temporary build with the threshold at 30000 mV, above the 24.4 V input), about 15 s:**
  - state IDLE with no fault source, INPUT_EN 0, OUTPUT_EN 0, OUTPUT_DIS 1;
  - 50 refusals at V_in 24535–24552 mV;
  - 26 attaches, 26 Hard Resets, 51 VBUS_ON_FAILED, 0 fault Hard Resets: one retry about every 0.6 s, with no latch.
- **Restored to 9000 mV and flashed:** RUNNING, V_in 24450 mV, V_out 4967 mV, 5 V contract at the first attach.
- **Not tested:** the supply itself coming up after boot. The test above kept the input on and raised the threshold.
  A supply that drops out while RUNNING still latches a fault (SW_VIN_RANGE in buck, SW_UVP in buck-boost).
- The sink's TCPP02 read `tcpp_fault` 1, `tcpp_ok` 0 (ack 0x18). It read the same earlier today, before this
  flash. Not investigated.

## 5. Battery-load build, step L0 (branch `claude/lipo-load`, plans/lipo-load.md)

No-USB build (`build/Battery`, ELF sha1 `61c2de6f…`), flashed at 21:11. Load: 25.3 Ω, no pack, QT Py off. V_in 24.5 V.

- **Boot:** IDLE, no auto-start, OUTPUT_EN 0, **OUTPUT_DIS 0**, `regulator_ccm_ilim_ma` 500 (default).
- **`bu battery run --ceiling-mv 9000 --ilim-ma 200 --seconds 10`:** 34 samples, no fault, stopped by `run` at the
  end.
  - The pack read 0 mV, as expected with no pack.
  - By the first sample (0.84 s) the switch was closed and the target was at the 9 V ceiling.
  - Buck DCM, limiting in 100 % of samples, never CCM.
  - The limit's input was 200.0 mA (σ 0.3). V_out (single VD_MON samples) was 6141 mV mean (σ 63), so V/R = 243 mA
    (inferred). **The current read 43 mA under V/R** (−18 %). The 2026-11-06 DCM run read 250 against 230 mA, before
    the ID_MON recalibration (gain 1059, offset −54). Not resolved: it needs an ammeter in series. If it holds, a pack
    would take about 20 % over the set current at 200 mA.
  - OUTPUT_DIS 0 in every sample.
- **Keepalive:** `bu regulator start 9000` with no keepalive writes. RUNNING until 1.58 s, IDLE at 1.93 s,
  `keepalive_stops` 1, switch open, OUTPUT_DIS 0, no reset.
- **SIGTERM during `run` (5 s):** STOP serviced through the mailbox, no reset. The first try lost the log and the
  summary (the process ended after the stop). `battery.py` now writes the log first; the re-run kept 14 samples.
- **SIGKILL during `run` (5 s): the G474 reset** (uptime 7.7 s afterwards, mailbox counters 0, `keepalive_stops` 0).
  The RCC flags (SFTRSTF, BORRSTF, PINRSTF) are never cleared, so they don't identify the reset. Inference: killing
  STM32_Programmer_CLI mid-transfer, then reconnecting, reset the target. Don't SIGKILL `bu` during a run; use Ctrl-C.
- **Not tested:** SW_VIN_RISE and the CCM no-reverse floor (neither can be provoked with a resistor), the pack read
  with a pack.
- The G431 sink's ST-LINK VCP was not enumerated when checked after the flash (`bu sink status`: no VCP with its
  serial). Not investigated; the battery build has no USB-PD.

## 6. Battery-load step L2: 4S pack (run by Dan at 21:41)

Load: 4S LiPo (Dan: 15.2 V), 220 mΩ in series, 15 A fuse. Same build as section 5. V_in 24.5 V. The board had rebooted
since L0 (counters 0, `regulator_ccm_ilim_ma` back at 500).

`bu battery run --ceiling-mv 16000 --ilim-ma 200 --seconds 10` (`bringup_out/20261109-214121_battery_run.json`):
- 31 samples, no fault, stopped by `run` at the end. IDLE afterwards, switch open, OUTPUT_DIS 0.
- **The pack read 15505 mV** on VD_MON (switch closed, every FET off). Dan's figure is 15.2 V: 305 mV (2 %) apart.
  Which one is off is not known.
- By the first sample (0.88 s) the switch was closed and the target was at the 16 V ceiling.
- Buck DCM, limiting in 100 % of samples, never CCM. OUTPUT_DIS 0 in every sample.
- The limit's input was 200 mA (σ 2.0, 195–204). V_out was 15580 mV mean (σ 77, 15436–15815).
- V_out − pack is 75 mV. 200 mA × 0.22 Ω is 44 mV, under the VD_MON sample noise, so the current can't be checked from
  I·R here.
- IS_MON read 189 mA (σ 3) at 24.50 V, so P_in = 4.63 W.
  - At 200 mA, P_out = 3.12 W (67 %).
  - If the current is 18 % low as in L0, P_out = 3.79 W (82 %). Neither figure is resolved.

## 7. ID_MON in DCM against Dan's ammeter (4S pack)

Per-step logs: the `log` field in each `ilim_sweep_*.jsonl` names a `bringup_out/` file; copies are in `runs/`.

The ammeter (10 A range) replaced the 220 mΩ resistor. Script: `scripts/ilim_sweep.sh`, results in `ilim_sweep_100-250.jsonl`.
Each step was 20 s at a 16 V ceiling, 8 s idle between steps, V_in 24.5 V, buck DCM.

| Setting | Limit input (ID_MON, 1059 / −54) | Ammeter | IS_MON | V_out | Efficiency |
|---|---|---|---|---|---|
| 100 mA | 100.1 mA | 148 mA | 120.5 mA | 15445 mV | 77.4 % |
| 150 mA | 150.1 mA | 195 mA | 155.9 mA | 15504 mV | 79.2 % |
| 200 mA | 200.3 mA | 242 mA | 193.3 mA | 15468 mV | 79.0 % |
| 250 mA | 249.7 mA | 288 mA | 225.3 mA | 15483 mV | 80.8 % |

- Every step: no fault, limiting in 100 % of samples, never CCM, OUTPUT_DIS 0, IDLE at the end. The pack read
  15436–15505 mV.
- One V_out sample read 11486 mV (200 mA step, 19.54 s) while the current read 200 mA. Inference: a bad single
  reading, since the switch was closed onto the pack.
- **Fit:** the ammeter reading is 0.9359 × the limit's input + 54.4 mA, with residuals within 0.1 mA. In raw ID_MON
  terms that is 0.9911 × raw + 3.9 mA, so the gain/offset would be **991 / +4**.
  - The default 1059 / −54 came from DCM into 25.3 Ω on 2026-11-06, with V/R as the reference.
  - L0 (section 5) read 200 mA at V/R 243 mA. The fit gives 241 mA, so it agrees.
  - Why the 2026-11-06 V/R fit was off is not known. Inference: a VBUS load not counted in V/R.
- Covers 148–288 mA only, at 15.5 V out, in DCM. Untested: higher current, CCM, and other V_out.
- Efficiency uses IS_MON, as calibrated in section 3.
- The 300 mA Dan saw at the 200 mA setting in section 6, on the earlier meter, compares with 242 mA here.

## 8. ID_MON 991 / +4: check from 250 mA to 1 A

`regulator_id_gain_permille` 991 and `regulator_id_offset_ma` +4 were written to RAM (section 7 fit). Then
`scripts/ilim_sweep.sh 250 500 750 1000` ran, with results in `ilim_sweep_250-1000.jsonl`. Ammeter on the 10 A range;
it read 0.2 mA with no output.

| Setting | Limit input | Ammeter | Error | V_in | P_in (IS_MON) | P_out (V_out × ammeter) | Efficiency | Loss |
|---|---|---|---|---|---|---|---|---|
| 250 mA | 250.0 mA | 250.6 mA | +0.6 mA | 24476 mV | 4.88 W | 3.89 W | 79.6 % | 1.0 W |
| 500 mA | 500.1 mA | 499.5 mA | −0.6 mA | 24463 mV | 9.44 W | 7.73 W | 81.9 % | 1.7 W |
| 750 mA | 750.2 mA | 749.0 mA | −1.2 mA | 24449 mV | 14.08 W | 11.72 W | 83.2 % | 2.4 W |
| 1000 mA | 1000.3 mA | 999.8 mA | −0.5 mA | 24454 mV | 18.70 W | 15.67 W | 83.8 % | 3.0 W |

- Every step: no fault, limiting in 100 % of samples, buck DCM, **never CCM**, OUTPUT_DIS 0, IDLE at the end.
- V_out is the median of single VD_MON samples (15470–15677 mV). V_out is read before Q5/Q6, so P_out includes the
  switch, fuse and ammeter.
- The 1 A loss (3.0 W) is at the top of the 2–3.5 W free-air budget (2026-10-16). Temperatures were not recorded.
- 991 / +4 is now the default (`ID_GAIN_PERMILLE_DEFAULT`, `ID_OFFSET_MA_DEFAULT`) on `claude/lipo-load`. That build
  is not flashed yet; the board runs it from RAM until a reset.
- Still untested: CCM (the `ID_CCM_ADD_MA` table is added to the raw reading before the gain), and V_out other than
  15.5 V.

## 9. Toward CCM: SW_OCP at the 2000 mA setting

Dan reconnected 24 V before this. The G474 did not reset (uptime 1769 s) and 991 / +4 was still in RAM. Dan's ammeter
moved to the **input**. Run: `scripts/ilim_sweep.sh 1500 2000 2500 3000`, with results in
`ilim_sweep_1500-3000.jsonl`.

**1500 mA setting:** 65 samples, buck DCM, never CCM, no fault.
- Limit input 1499.8 mA, V_out 15850 mV median, V_in 24642 mV.
- **Input:** the ammeter read 1.015 A (7.8 mA with the regulator idle). IS_MON read 1176 mA, **+161 mA (+16 %)**.
- With the meter: P_in 25.0 W, P_out 23.8 W (V_out × limit input), **95.0 %, loss 1.2 W**. The section 8 efficiencies
  use IS_MON, so their losses are too high by this margin. Dan: the board is barely above room temperature.

**2000 mA setting: SW_OCP within 0.9 s of the start**, the first sample. Not cleared. No trace was armed.

At the fault the latched values were:
- ID_MON 5491 mA, its 5.5 A full scale, so the real peak is not known. `ccm_i_avg_ma` 5275 against `ccm_ocp_ma` 5000
  (for 1 ms).
- V_out 16798 mV, the pack 15505 mV.
- IS_MON 4089 mA, V_in 25044 mV (start 24484).
- `ccm_entries` 2, `ccm_exits` 2 (STOP). `ccm_limiting` 1, with `ccm_v_lim_mv` 14738, below the pack. `ccm_u_mv` 754.
- DCM's `i_max_ma` was 2378, so CCM entry is at 85 % of it, about 2.0 A.

**Inference (not confirmed without a trace):** the CCM no-reverse floor (`v_floor_v` = V_out filtered over 0.8 ms) held
the current up.
- The floor puts D·V_in at least at V_out. That holds L1 at whatever current it carries, not at 0 A.
- Into a pack, V_out = V_pack + I·R, so the floor follows the overshoot. The limiter had pulled its reference to
  14.7 V, below the pack, but the duty could not drop below the floor.
- The floor should be the pack's open-circuit voltage, not the loaded V_out. At V_pack, L1's current decays to 0 A and
  cannot reverse.
- What started the overshoot at the CCM entry is not known. Candidates: the entry preset (`u0`), and the limiter's
  step at the handover.

## 10. DCM-only sweep for IS_MON; a start after clear-fault ran without the PID ISR

SW_OCP (section 9) was cleared once. `regulator_ccm_enable` was set to 0 in RAM. Run:
`scripts/ilim_sweep.sh 250 500 1000 1500 2000`, with results in `ilim_sweep_250-2000.jsonl`. Dan's ammeter is on the
input.

**250 mA step: not run.** All 86 samples were identical for 20 s:
- RUNNING, V_out 15384 mV, the limit input 5605 mA (left from the fault), IS_MON 12 mA, switch open.
- **Cause (from the code and HAL source):** `enter_fault()` stops TIM6 and TIM7 by clearing CEN, which leaves their
  HAL handles BUSY. `regulator_clear_fault()` did not restore them.
- `HAL_TIM_Base_Start_IT(&htim7)` returns HAL_ERROR unless the handle is READY. `regulator_start()` ignored the
  return and went RUNNING with no PID ISR, so there was no control, no keepalive stop and no safety checks.
- `regulator_stop()` (via HAL) made the handles READY again, so the later steps ran normally.
- Nothing switched (IS_MON 12 mA).
- This affects every build, not just the battery one.

**Input current (DCM, never CCM):**

| Setting | Limit input | IS_MON (raw) | Ammeter | V_out | V_in | P_in (meter) | P_out | Efficiency | Loss |
|---|---|---|---|---|---|---|---|---|---|
| 500 mA | 498.9 mA | 392.9 mA | 346.4 mA | 15557 mV | 24546 mV | 8.50 W | 7.76 W | 91.3 % | 0.74 W |
| 1000 mA | 1000.0 mA | 782.6 mA | 678.7 mA | 15764 mV | 24586 mV | 16.69 W | 15.76 W | 94.5 % | 0.92 W |
| 1500 mA | 1500.7 mA | 1174.6 mA | 1015.0 mA | 15850 mV | 24639 mV | 25.01 W | 23.79 W | 95.1 % | 1.22 W |
| 2000 mA | 2000.2 mA | 1565.4 mA | 1349.0 mA | 15971 mV | 24687 mV | 33.30 W | 31.94 W | 95.9 % | 1.36 W |

- **Fit:** meter = 0.8553 × IS_MON + 10.0 mA, with residuals within 0.7 mA. The 7.8 mA idle draw is in the offset.
- P_out is V_out (VD_MON, before Q5/Q6) × the limit input. VD_MON read the pack 2 % above Dan's 15.2 V. If VD_MON reads
  high, these efficiencies are about 2 points high (inference).
- Dan: the board was barely above room temperature.

**Code (built, not flashed; ELF sha1 `e6f0eadc…`, kept as `build/Battery/final_next.elf`):**
- `regulator_clear_fault()` stops TIM6 and TIM7 through HAL (`slope_comp_stop()`, `HAL_TIM_Base_Stop_IT`).
- `regulator_start()` checks `HAL_TIM_Base_Start_IT(&htim7)`. On failure it stops and counts
  `regulator_start_tim_fail_count`.
- The CCM no-reverse floor is now `regulator_battery_pack_mv` (section 9). The V_out filter is removed. The host test
  adds the forward overshoot: L1 from 5 A at a 2 A limit ends at 2.00 A with the pack floor and 5.00 A with the old
  V_out floor (buck and boost).
- IS_MON calibration: `regulator_is_gain_permille` 855 and `regulator_is_offset_ma` +10, applied in step 5a (filtered
  I_in, P_in). `bu battery` rows now show `i_in_filt_ma`.
- ID_MON defaults 991 / +4 (section 8).

## 11. CCM on the 4S pack with the floor at the pack: the floor set the current

Flashed `fb42be8` (ELF `e0127868…`). Boot: IDLE, no fault, ID 991/+4, IS 855/+10, CCM on. Trace armed with
`--trigger ccm --post 1024`. Run: `bu battery run --ceiling-mv 16000 --ilim-ma 2000 --seconds 10`. Logs:
`runs/20261109-221506_battery_run.json` and the trace `runs/20261109-221507_regulator_trace.csv`.

- **No fault.** CCM in all 33 status samples, limiting in all, IDLE at the end.
- **The board read 2864 mA mean** at the 2000 mA setting. V_out was 16022 mV median, with the pack at 15522 mV.
  - IS_MON (calibrated in DCM) read 1569 mA at 24683 mV, so P_in = 38.7 W. P_out from the board's reading would be
    45.9 W, which is impossible.
  - So ID_MON reads high in CCM, or IS_MON reads low in CCM, or both. Dan's input meter reading for this run would
    tell which.
  - From V_out − pack = 0.50 V and the DCM path resistance (0.24 Ω, section 10), the real current is about 2.1 A
    (inference).
- **Trace:** 1023 samples before the first CCM cycle, 1025 after.
  - Before (DCM): the measured current was 1.9–2.06 A, t_on 2550–2680 ns.
  - At the entry, t_on went to 2958 ns and stayed at 2949–2963 ns for the whole 51 ms after. Over 3 ms the measured
    current rose from 1.93 A to 2.95 A, and held there.
  - 2955 ns is the floor duty: V_pack × r1 / V_in × T. **The floor bound from the first CCM cycle**, and the limit
    could not lower the duty.
- **Cause:** at a given duty the board makes more than the model's D·V_in. The CCM entry already corrects for this
  with u0 = −(16 ‰ V_ref + 100 mV), about −0.35 V at 15.5 V (`CCM_U0_*`, fitted 2026-10-21/23). Here, the floor duty
  gave V_pack + 0.5 V.
  - The floor clamped the duty, so it skipped that correction. Into a stiff pack, it forced the current up to whatever
    0.5 V drives.
- **Fix (built, not flashed; ELF `82f04fd9…`):** the floor now clamps the law's reference (v_cmd ≥ V_pack) in both the
  buck and boost laws. The integrator's offset then applies, and the limit can lower V_out to the pack.
  - Host test, L1 from 5 A at a 1 A limit: with the plant 0.5 V over the model and u0 −0.35 V, I settles at 1.00 A and
    never drops below 0.76 A.
  - With no plant offset and the same u0 (u0 0.35 V on the unsafe side), I still settles at 1.00 A, but dips to
    −0.12 A on the way down (averaged model, L = 50 × L1).
  - Letting the integrator run at the floor did not change that dip, so it was reverted.

## 12. Floor on the reference (ELF `82f04fd9…`): no fault, but the CCM limit cycles

Flashed `82f04fd9…`. Boot: IDLE, no fault. Trace armed `--trigger ccm --post 1024` for each run. Dan did not catch the
input meter.

**Ceiling 16.0 V, 2000 mA, 10 s** (`runs/20261109-222148_*`): no fault, IDLE at the end, but not settled.
- CCM in and out: 24 entries and 24 exits, all LIGHT_LOAD (under 50 % of the DCM ceiling, about 1.2 A).
- Limiting in 39 % of samples. The status samples read 0.9–2.7 A (limit input) and 0.36–1.55 A (IS_MON).
- **Trace:** after the CCM entry, a growing oscillation with a period of about 9 ms. The current went 1.3 → 0.9 A at the
  bottom and 2.1 → 2.3 A at the top. The limit engaged and released each half-cycle. t_on 2929–2984 ns, so the floor
  no longer pins the duty.
- Single VD_MON samples read as low as 15263 mV, under the pack (15522 mV), while the current read positive. Single
  V_out samples in CCM are not trusted here.

**Ceiling 16.4 V, 2000 mA, 10 s** (`runs/20261109-222240_*`), so that V_out at 2 A (about 16.0 V) sits under the
ceiling:
- No fault. CCM in every status sample, limiting in every sample. 30 entries over the run; the exits logged are
  LIGHT_LOAD, then STOP.
- Limit input 2316 mA mean (σ 826, 1076–3699). IS_MON 1235 mA mean (σ 508).
- **Trace:** the same cycle, about 8 ms, 0.8–2.5 A, with the limit releasing each half-cycle. Trace peak 3512 mA, under
  SW_OCP's 5 A.
- **Energy:** P_in (IS_MON) 30.5 W, P_out from the limit input 36.6 W. In both CCM runs the limit input is about 1.25 ×
  what P_in × 0.95 / V_out allows. So ID_MON reads high in CCM, or IS_MON reads low in CCM (needs the input meter).

**Not found:** why the limit releases. It releases when v_lim reaches V_ref. The CCM voltage integrator holds while
limiting and runs while released. The averaged host model (400 kHz) holds 2 A into 0.2 Ω without a cycle (`battery
200 mOhm` in the current-limit test), so it does not reproduce this.
- Differences from the model: 200 kHz, the board's duty offset, the CCM current reading's error, and the LIGHT_LOAD
  exits.
- The trace does not record the CCM integrator (u) or v_lim, which would show the mechanism.

The floor change (sections 11–12) is not committed.

## 13. CCM in voltage mode just above the pack: CCM holds

Run: `bu battery run --ceiling-mv 16100 --ilim-ma 3500 --seconds 10`, so the pack and path resistance set the current
and the limit is only a backstop. Logs: `runs/20261109-222627_battery_run.json`, `runs/20261109-222628_regulator_trace.csv`.
The pack read 15522 mV.

- No fault, IDLE at the end. **CCM in all 37 samples, limiting in none, one CCM entry** for the run.
- V_out 16109 mV (σ 133, single samples). Limit input (ID_MON) 3094 mA (σ 59). IS_MON 1696 mA (σ 31) at V_in
  24722 mV.
- **Dan's input meter: 1.72 A, falling to 1.69 A as the pack charged.** IS_MON agrees within about 1–2 % in CCM, so
  the input calibration (section 10) holds in CCM.
- **Trace:** DCM before the entry at 1.9 A (t_on σ 67 ns). After it, the current rose to about 3.35 A (as read) within
  10 ms, then held: σ 25–44 mA, t_on 2953–2974 ns (σ 2–3 ns), V_out 16.10 V mean.
- **(Withdrawn in section 14:** the 0.24 Ω below is not a resistor, and the 2.46 A estimate is not valid.)
- **ID_MON reads high in CCM.** P_in on the meter is 24.72 V × 1.70 A = 42.0 W. At 95 % (the DCM figure at 2 A,
  section 10) P_out is 39.9 W, so I = 2.48 A. V_out − pack over the 0.24 Ω path gives 2.46 A. The board read
  3.09 A, **about 25 % high** (inference: it assumes the DCM efficiency and path resistance).
  - At this duty (0.65) the `ID_CCM_ADD_MA` table adds 160 mA (raw), so the reading would still be about 18 % high
    without it.
  - The table was fitted on 2026-11-06 into 25.3 Ω against V/R, where CCM read low.
  - The CCM limit regulates this reading. That is consistent with the "2000 mA" CCM runs in sections 11–12 putting
    less than 2 A into the pack.
- So CCM is stable in voltage mode into the stiff pack. The cycle in section 12 comes with the current limit in CCM.

## 14. CCM current reading against the output ammeter; the CCM correction table reads high into the pack

**Correction (Dan):** since section 7, no resistor has been in series. The ammeter took the 220 mΩ resistor's place.
The path is 12 AWG wire, the 15 A fuse, the XT60 and the pack (and the ammeter when it is on the output).
- So the "0.24 Ω path" in sections 10–13 is an apparent value: V_out − pack read over the current, covering the
  pack's internal resistance, Q5/Q6 and any VD_MON bias while switching.
- The 2.46 A estimate in section 13 used it and is withdrawn. Measured here, V_out − pack is about 0.17 Ω × I.

**Fixed ceilings, ammeter on the output** (`scripts/ccm_ceiling_sweep.sh 16100 16250 16400`, limit 4500 mA,
`ccm_ceiling_16100-16400.jsonl`):
- **16.1 V:** the pack read 15401 mV, 120 mV under section 13's.
  - The limit input read 4268 mA mean (σ 647, 3.3–5.3 A). Limiting in 59 % of samples, so this was the CCM limit cycle
    of section 12.
  - **Ammeter 4.22 → 4.17 A.**
- **16.25 V: SW_OCP** at the first sample (0.68 s). ID_MON was at full scale 5493 mA, `ccm_i_avg_ma` 5270, `v_lim`
  15549 mV, the pack read 15557 mV. The real peak is not known. Dan checked the hardware: fine. Cleared once.

**Ceiling relative to the pack:** `bu battery run --above-pack-mv N` first STARTs at 5 V. That reads the pack, stores it
and refuses (PACK_AT_CEILING). Then it STARTs at pack + N. The pack read moved 15505–15608 mV between starts.

**+300 / +400 / +500 mV, limit 3000 mA** (`ccm_above_300-500.jsonl`): no fault, CCM in every sample.

| Step | Ceiling | Limiting | Limit input (σ) | Ammeter (start → end) | Board − meter | `ID_CCM_ADD_MA` at the duty |
|---|---|---|---|---|---|---|
| +300 | 15805 mV | 0 % | 2788 mA (30) | 2.678 → 2.584 A | +157 mA | +158 mA (D 0.645) |
| +400 | 16008 mV | 100 % | 3018 mA (156) | 2.878 → 2.935 A | +111 mA | +159 mA (D 0.650) |
| +500 | 16039 mV | 100 % | 3044 mA (144) | 2.885 → 2.920 A | +142 mA | +159 mA (D 0.649) |

**+300 mV with the CCM correction off** (`regulator_id_ccm_corr` 0 in RAM, `ccm_above_300-300.jsonl`):
- Pack 15539 mV, ceiling 15839 mV. No fault, CCM in every sample, limiting in none.
- The limit input fell from 2694 mA (first 5 samples) to 2618 mA (last 5), mean 2647 mA (σ 28). **The ammeter read
  2.695 → 2.605 A.** So the board was within 1 mA at the start and 13 mA at the end.
- **So into the pack, ID_MON in buck CCM at 200 kHz reads right without the table** (one duty, 0.65). The table, fitted
  into 25.3 Ω on 2026-11-06 where CCM read low, adds about 158 mA here.
  - The first 25 % reading in section 13 was mostly this table plus the wrong path-resistance inference.

**Energy in CCM does not balance:** in this run P_in (IS_MON) = 24.43 V × 1.499 A = 36.6 W. P_out = ammeter × V_out =
2.65 A × 15.85 V = 42.0 W (41.2 W at the pack read).
- In DCM (section 10) it balanced at 96 %, and IS_MON matched Dan's input meter in CCM at 1.70 A (section 13).
- One of V_in, IS_MON, V_out or the output current is off in CCM by about 13 %. That needs the input and output read
  at the same time (the bench supply's display for the input).

## 15. CCM current limit: v_cmd and u in the trace, the floor held u's wind-up, then the limit loop oscillated (SW_VIN_RISE)

**Code:** the CCM correction table (`ID_CCM_ADD_*`, `regulator_id_ccm_corr`) is removed. The trace adds `v_cmd_mv`
(the reference both loops track, the limit's v_lim while it acts) and `u_mv` (the CCM integrator), 14 bytes a sample.

**ELF `63e71253…`, `--above-pack-mv 500 --ilim-ma 2000`** (`runs/20261109-224858_*`, pack 15574, ceiling 16074 mV):
no fault, limiting in every sample.
- The board read 2771 mA (tail). **Dan's meter: 2.9 → 2.8 A.**
- **Trace:** at the CCM entry u was −627 mV (preset). Under the limit u integrated up to −50 mV in 1.45 ms. Then the
  limit acted, which holds u.
- The limit pulled v_cmd from 16074 to **15560 mV (the floor)** and it stayed there for the 75 ms traced. The current
  read 2.95–3.05 A.
- At v_cmd = pack with u −50 mV, the board makes about 0.5 V over the pack. The floor kept the limit from going lower,
  and u was held.

**Fix tried:** the floor applies only while the limit is not acting (both laws). The host test adds the board's case
(from 0 A, ceiling 0.5 V over the pack, plant +0.5 V, u0 −0.627 V, limit 2 A): 2.09 / 2.27 A (buck / boost) before,
2.00 / 2.00 A after.
- In the 5 A → 1 A cases the limit's undershoot is no longer caught: L1 dips to −0.07 … −0.21 A in the averaged
  model. That check is now bounded at −0.25 A.

**ELF `8194f367…`, same run** (`runs/20261109-225119_*`, pack 15557, ceiling 16057 mV): **SW_VIN_RISE** (V_in 26486 mV,
start 24484 mV). Not cleared.
- **Trace:** the limit loop oscillated with a period of about 10 ms, growing.
  - v_cmd: 16057 → 14385 (5.85 ms) → 16026 → 13351 mV (15.85 ms).
  - Measured current: 3.24 → 0.36 (8.25 ms) → 4.13 → 0.05 A (18.25 ms).
  - CCM exited (LIGHT_LOAD) at 20.2 ms. DCM was still under the limit at 1.2–1.5 A when the fault came later.
- **The duty follows v_cmd at once.** t_on's minimum (2747 ns at 6.25 ms) sits at v_cmd's. **The measured current lags
  by about 2.4 ms**, about 86° at 10 ms.
- At 5.85 ms V_out read 15350 mV, under the pack (15574 mV), so the real current was negative while the reading still
  said +2.0 A. Power went back into V_in until SW_VIN_RISE.
- **Likely source of the lag (from the ADC timing constants, not measured):** ADC2 free-runs [VS_MON, VS_MON, ID_MON]
  with 256× oversampling, about 0.56 ms a scan. `i_out_ma` is the mean of 8 scans (`ADC_SYNC_AVG_SCANS`), a boxcar of
  about 4.5 ms, so about 2.2 ms of delay.
  - The limit's loop gain into the pack in CCM is about kl / 0.2 Ω = 2000 rad/s. Against 2.2 ms that leaves no phase
    margin. In DCM (about 72 rad/s, section 3 of the plan) it does not matter.
  - The duty floor of sections 11–12 had been hiding this.

## 16. CCM current limit on ID_MON's newest scan, kl 100 in CCM: holds 2 A into the pack

**Code:**
- **Fast input:** `adc_measurements.i_out_lim_ma` is ID_MON from ADC2's newest scan alone (`ADC_ID_LIMIT_SCANS` 1).
  The limit reads it when `regulator_ccm_ilim_fast` is 1 (the default; 0 goes back to the 8-scan mean).
  - Scan: 256 × (37 + 37 + 19) clocks at 42.5 MHz, 560 µs, of which ID_MON is the last 114 µs.
  - Lag: about 0.34 ms, against about 2.3 ms for the 8-scan mean (from the timing constants, not measured).
  - `i_out_ma` (the 8-scan mean) is unchanged for telemetry. The gain and offset (991 / +4) apply to both: a mean of
    period means.
- **Gain:** `regulator_ccm_ilim_kl_ccm` (`CCM_ILIM_KL_CCM_DEFAULT` 100 V/(A·s)) applies while CCM runs.
  `regulator_ccm_ilim_kl` (400) stays DCM's.
- **Tool:** `bu regulator status` shows `ccm_ilim_fast`, `ccm_ilim_kl_ccm` and `adc.i_out_lim_ma`.

**Host model** (`test_ilim_lag`: buck, the ADC2 pipeline as above, the section 15 start, limit 2 A, 200 ms):

(Corrected on 2026-11-11, `../2026-11-11/README.md` section 2: this model used L = 50 × L1, which adds a 1.8 ms
pole the board does not have. On the real L1 and C_int, kl 400 holds down to 0.08 Ω, and the 8-scan mean still
oscillates at 0.17 Ω. The table below is the 50× plant.)

| kl | ID_MON | R | I_L (last 20 ms) | p-p | lowest |
|---|---|---|---|---|---|
| 400 | 8-scan mean | 0.17 Ω | 1.46 A | 6.84 A | −2.01 A |
| 400 | newest scan | 0.10 Ω | 1.40 A | 19.07 A | — |
| 100 | newest scan | 0.05 Ω | 2.00 A | 0.03 A | −4.99 A |
| 100 | newest scan | 0.10 Ω | 2.00 A | 0.01 A | −0.75 A |
| 100 | newest scan | 0.13 Ω | 2.00 A | 0.02 A | 0.00 A |
| 100 | newest scan | 0.17 Ω | 2.00 A | 0.01 A | 0.00 A |

- The 8-scan row reproduces section 15's oscillation and reverse current.
- kl 400 on the newest scan holds at 0.17 Ω but oscillates at 0.10 Ω. The bench path is about 0.13 Ω incremental
  (+300 mV gave 2.69 A, +500 mV about 4.2 A, Dan's meter). That is why kl is 100 in CCM.
- At 0.10 Ω and below, the start still dips L1 negative once (the "lowest" column), by about the same for any kl from
  50 to 150. This is the entry transient (ceiling 0.5 V over the pack with the plant 0.5 V high), not the limit loop.
  Not addressed.

**ELF `3b787a7e…` (sha1), `--above-pack-mv 500 --ilim-ma 2000 --seconds 10`, CCM trace armed**
(`runs/20261109-225941_battery_run.json`, `runs/20261109-225951_regulator_trace.csv`): pack 15522, ceiling 16022 mV.
**No fault. CCM and limiting in every sample.**
- **Board, 0.2 s samples (limit's input, newest scan):** 1939–2070 mA. Tail mean 1981 mA, V_out 15715 mV.
  V_in 24382–24416 mV (start 24399).
- **Trace, CCM entry at 0 ms** (from DCM at about 1.9 A, not limiting):
  - u went from −572 to −234 mV in 0.6 ms, then the limit acted and held it.
  - The current read 3599 mA for one scan (0.6–1.1 ms), then 2.7 A, and fell to 2.0 A by 5 ms. No undershoot: the
    lowest read after 0.6 ms is 1927 mA.
  - v_cmd: 16022 → 15562 mV by 5 ms, then steady.
- **Steady (10–75 ms):** current 2002 mA mean, σ 28 mA, 1930–2123 mA. t_on 2948.8 ns, σ 1.3 ns. v_cmd 15554.6 mV,
  σ 2.9 mV.
- **Not checked yet:** Dan's meter during this run, and lower or higher limits.

## 17. Current-limit sweeps, DCM and CCM, on ELF `3b787a7e…`: within 0.3 % of the meter in both

**Run:** `scripts/ilim_mode_sweep.sh dcm 250 500 1000 1500 2000`, then `scripts/ilim_mode_sweep.sh ccm 2000 2500 3000
3500` (`ilim_dcm_250-2000.jsonl`, `ilim_ccm_2000-3500.jsonl`, logs and traces in `runs/`).
- Each step: 20 s, ceiling pack + 800 mV, 8 s idle between.
- DCM steps ran with `regulator_ccm_enable` 0 (restored to 1 after). CCM steps had the CCM trace armed.
- Dan's ammeter on the output. The limit's input is ID_MON's newest scan (section 16).
- No fault on any step.

| Mode | Setting | CCM share | Limiting | Board (limit input), mean ± σ of 0.2 s samples | Meter | Board − meter | V_out (median) |
|---|---|---|---|---|---|---|---|
| DCM | 250 mA | 0 | 1.00 | 250.8 ± 3.2 mA | 250.4 mA | +0.4 mA | 15591 mV |
| DCM | 500 mA | 0 | 1.00 | 500.2 ± 3.0 mA | 499.0 mA | +1.2 mA | 15539 mV |
| DCM | 1000 mA | 0 | 1.00 | 1000.3 ± 2.6 mA | 999.9 mA | +0.4 mA | 15746 mV |
| DCM | 1500 mA | 0 | 1.00 | 1500.2 ± 3.9 mA | 1.499 A | +1.2 mA | 15781 mV |
| DCM | 2000 mA | 0 | **0.00** | 1983.7 ± 5.6 mA | 1.984 A | −0.3 mA | 15815 mV |
| CCM | 2000 mA | 1.00 | 1.00 | 1999.3 ± 22.4 mA | 2.000 A | −0.7 mA | 15764 mV |
| CCM | 2500 mA | 1.00 | 1.00 | 2501.6 ± 31.0 mA | 2.503 A | −1.4 mA | 15867 mV |
| CCM | 3000 mA | 1.00 | 1.00 | 3000.6 ± 41.3 mA | 3.008 A | −7.4 mA | 16040 mV |
| CCM | 3500 mA | 1.00 | 1.00 | 3513.1 ± 40.8 mA | 3.509 A | +4.1 mA | 16040 mV |

- **Meter against the setting:** DCM −0.1 … −0.2 % at 250–1500 mA. CCM 0.0 … +0.3 %.
- **DCM 2000 mA did not limit:** DCM's ceiling held it at 1.98 A, so the limit never acted (CCM was off).
- **Sample noise:** σ is 3–6 mA in DCM and 22–41 mA in CCM, on the newest scan alone.
- **Temperature (Dan, after the sweep):** ambient 23.3 °C near the board, board maximum 34.8 °C. That is not steady
  state (20 s steps).

**CCM traces: the entry overshoot grows with the limit.**
- Every CCM step entered from DCM at its ceiling, about 2.0 A, not limiting. CCM's voltage loop then heads for the
  ceiling, pack + 800 mV. The limit acts only once a scan reads over the setting.

| Setting | Limit acts at | First scan over the setting | Lowest after | Steady (10–75 ms) | t_on σ |
|---|---|---|---|---|---|
| 2000 mA | 0.25 ms | 3344 mA | 1789 mA | 2000 ± 27 mA | 1.3 ns |
| 2500 mA | 0.55 ms | 4848 mA | 2168 mA | 2499 ± 58 mA | 2.2 ns |
| 3000 mA | 0.70 ms | 5204 mA | 2913 mA | 3000 ± 40 mA | 1.8 ns |
| 3500 mA | 0.65 ms | 5322 mA | 3402 mA | 3498 ± 42 mA | 1.8 ns |

- At 3000 and 3500 mA the peak scan is close to ID_MON's 5.5 A full scale and to SW_OCP (5000 mA filtered, for
  1 ms). The real peak is not known.
- The current is within about 10 % of the setting by about 8 ms. At 2500 mA it undershot to 2.3 A at 9.5 ms and came
  back by about 15 ms.
- In the 0.6 ms before the limit acts, t_on rose from about 2684 ns (DCM) to 3134 ns. u integrated up from its preset
  toward the ceiling.

**Input power does not balance in CCM.** IS_MON's calibration is a DCM one (section 10).

| Step | P_in (V_in × calibrated IS_MON) | P_out (V_out × meter) | Ratio |
|---|---|---|---|
| DCM 1000 mA | 16.5 W | 15.7 W | 95.4 % |
| CCM 2000 mA | 28.1 W | 31.5 W | 112 % |
| CCM 3500 mA | 48.8 W | 56.3 W | 115 % |

An efficiency over 100 % is impossible. So in CCM, IS_MON reads low or VD_MON reads high (inference). An input meter
reading in CCM would tell which.
