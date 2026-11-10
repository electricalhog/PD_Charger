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
