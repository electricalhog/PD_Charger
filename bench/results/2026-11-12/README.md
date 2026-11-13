# 2026-11-12: preparation for an 8S pack (boost), `claude/lipo-load`

No run on the board: the firmware was built, host-tested and flashed, and the script self-tested. No START was sent.

## 1. Firmware (flashed 17:10, regulator IDLE, output switch open)

`build/Battery/final.elf`, sha1 `f60eff98…`, 70.0 KB flash, no warnings: the image is byte-identical to
`build/BatteryNext` `85a95b16…` (objcopy and cmp; only the debug paths differ). Flash verified by the programmer.
After the reset: IDLE, no fault, OUTPUT_EN 0, OUTPUT_DIS 0, limit 500 mA, CCM enable 1, ID_MON 991 / +4, IS_MON 1010.

| Change | From | To | Why |
|---|---|---|---|
| `BATTERY_CEILING_MAX_MV` | 25000 | 34000 | An 8S pack is 24.0–33.6 V. A compile-time check keeps the relative OVP above the ceiling under 40 V (the output ceramics are 50 V parts). |
| `CCM_BOOST_U0_MV` | +100 | −440 | See section 2. |

## 2. Boost CCM entry into a pack: the integrator's preset

- **Measured (2026-11-03, `sweep_*.jsonl`).** In boost CCM at 28 V from 24.3 V into 25.3 Ω, the integrator u settled
  at −389 to −493 mV in 13 records. So the board makes about 0.44 V more than the boost law expects.
- **The preset was +100 mV,** from the loss model. Into 25.3 Ω the 0.54 V error is 21 mA. Into a pack it is the error
  over the path resistance.
- **Host model (`test_boost_pack`, new).** L1 4.7 µH, 9 µF of ceramics (their value at 30 V), the limit on ADC2's
  newest scan at kl 100, a forced-CCM entry from 0.5 A in DCM, ceiling 0.8 V over the pack:

  | Preset | Board over the model | 30 V pack, 0.13 Ω, limit 1 A |
  |---|---|---|
  | +0.10 V (old) | +0.44 V | peak 3.77 A, then 1.00 A |
  | −0.44 V | +0.44 V | peak 1.03 A, lowest 0.50 A |
  | −0.44 V | +0.54 V | peak 1.13 A (1.27 A at a 0.5 A limit) |
  | −0.44 V | +0.34 V | lowest −0.09 A (−0.27 A at a 0.5 A limit) |

  The spike lasts about a millisecond (1 / ki = 0.32 ms), shorter than the limit's 0.56 ms scan, so the limit does
  not see it. Each 0.1 V of preset error is 0.77 A at 0.13 Ω, either way.
- **Steady state in the model:** the limit holds 0.5–3 A at 26.4–33 V and 0.13–0.30 Ω with under 0.08 A p-p, and a
  ceiling too low for the limit holds (ceiling − pack) / R instead.
- **A low ID_MON reading in the model:** at 0.7× (what boost CCM read into 25.3 Ω on 2026-11-03) a 2 A limit holds
  2.86 A in the pack. With the ceiling 0.25 V over the pack it cannot pass 1.9 A whatever the reading.
- **Limits of this:** one voltage (28 V) and one load (1.1 A) were measured. The model is averaged over the period,
  so the 200 kHz ripple is not in it. The −440 mV preset is untested on the board.

## 3. `scripts/boost_cal.py`

Without `--go` it prints its plan and touches nothing. `--selftest` runs both phases against a simulated board
(22 checks pass).

**`resistor` (no pack, 25.3 Ω on VBUS).**
- A START at 5 V first: with a pack on VBUS the firmware refuses it and the phase stops.
- Then one 12 s run per ceiling (26, 28, 30, 32, 33 V: 1.03–1.30 A, 27–43 W in the resistor), limit 2 A as a backstop.
- It gives ID_MON against V/R in boost DCM or CCM, and in CCM the u the board settles at, at the pack's voltages.
  The firmware learns that u for the band, so pack runs after it (without a reset) start from the measured value.

**`pack` (8S on VBUS, ammeter in series). Each run needs Dan's go.**
- A pack read per step, refused outside 3.0–4.2 V a cell.
- Ceiling = the read + 1.6 × I × 170 mΩ + 200 mV (300–1500 mV), refused over 4.2 V a cell. The ceiling, not ID_MON,
  bounds the current if the reading is low.
- Defaults: DCM 250, 500, 750, 1000 mA; CCM 500, 1000, 1500, 2000 mA; 20 s each, 8 s idle between.
- In CCM, SW_OCP is lowered for the step (2 × the limit + 500 mA) and restored.

**Both.** The run stops when ID_MON, or V_in × I_in / V_out, stays over the step's stop level for 3 samples. The
second needs no ID_MON. After each step it asks for the ammeter reading and fits gain and offset in the firmware's
terms. It writes neither.

**Tested** on the board with `--scope` in the DCM resistor phase (section 5). Since that run each sample also logs
`skip` (pulse skipping on) and `skip_events` (`regulator_skip_events`), and the tail has `skip_frac` / `skip_events`.

## 4. Order on the bench

1. Flash: done.
2. `resistor --ccm dcm`: done (section 5). `resistor --ccm forced`: no pack. Check u against −440 mV.
3. Pack on, ammeter in series: `pack --ccm dcm`, then `pack --ccm forced`, each on Dan's go.

## 5. Resistor phase, DCM (17:11–17:16, Dan ran it with `--scope`)

`boost_cal_resistor_dcm_26000-33000.json`, captures in `captures/resistor_dcm_*.csv.gz` (CH2 V_in, CH4 V_out,
30k points, 240 µs, taken 5.0 s into each step). 25.3 Ω on VBUS, no pack, V_in 24.45–24.51 V (VS_MON; scope
24.42–24.51 V). START at 5 V: OK, pack 0 mV. No fault, no abort, no ammeter readings entered. All five steps ran
in **BUCK_BOOST** mode, DCM (no CCM, no limiting).

| ceiling | V_out VD_MON (tail mean) | V_out scope median | on_ns (tail) | ID_MON raw | V/R scope | ratio | I_in (IS_MON) | P_in → P_out |
|---|---|---|---|---|---|---|---|---|
| 26 V | 26.02 V | 25.56 V | 1673 | 1001 mA | 1010 mA | 0.991 | 1407 mA | 34.5 W → 25.8 W (75 %) |
| 28 V | 27.74 V (hunting, see below) | 27.49 V (at 5.0 s) | 1892 | 1079 mA | 1087 mA | 0.993 | 1578 mA | 38.7 → 29.9 W (77 %) |
| 30 V | 29.99 V | 29.41 V | 1893 | 1157 mA | 1162 mA | 0.996 | 1833 mA | 44.9 → 34.2 W (76 %) |
| 32 V | 30.52 V — not reached | 29.87 V | 1921 | 1178 mA | 1181 mA | 0.997 | 1887 mA | 46.2 → 35.3 W (76 %) |
| 33 V | 30.53 V — not reached | 29.91 V | 1922 | 1177 mA | 1182 mA | 0.996 | 1878 mA | 45.9 → 35.4 W (77 %) |

P_out is V_scope² / 25.3 Ω; P_in is VS_MON × IS_MON (IS_MON calibrated in buck on 2026-11-12, section 1).

**ID_MON in buck-boost DCM reads V/R from the scope within 1 %** with the flashed 991 / +4 mA: fit gain 0.975,
offset +29 mA, residual ≤ 3.6 mA, ratio mean 1.001. Nothing to change for DCM. Against VD_MON the fit is 1.032
because **VD_MON reads 0.46–0.65 V above the scope median** here (26 V: +0.46; 30 V: +0.58; 32/33 V: +0.62–0.65 V),
as it did at 15 V on 2026-11-11. The ID_MON 0.7× of 2026-11-03 was boost CCM; it is not seen in DCM.

**32 and 33 V were not reached: the buck-boost DCM pulse is at its cap.** on_ns 1919–1922 ns is the 10 A peak
(`DCM_MAX_PEAK_MA_BB`: 10 A × 4.7 µH / 24.5 V = 1.92 µs), so this load tops out at 29.9 V (scope), 1.18 A, 35 W out
with 46 W in. The mode stays BUCK_BOOST: the band's top is V_in + 7 V = 31.5 V and BOOST needs the setpoint over
32.0 V **and V_out over 31.5 V** (`determine_mode_from_voltages`), which the capped pulse cannot deliver into
25.3 Ω, so the converter sits at the cap. With a pack on VBUS, V_out is the pack, so an 8S above 31.5 V does
reach BOOST; a resistor above ~30 V at 24.4 V in does not. For the forced-CCM resistor run this means 32/33 V
only count if CCM (no peak cap) gets there; expect the BUCK_BOOST-mode boost kind, as on 2026-11-03 at 28 V.

**The 28 V step: two regimes** (Dan: "very noisy"). Rows every 0.23 s:
- 0–5.1 s: V_out 27.84–28.15 V (sd ≈ 70 mV), on_ns 1773–1793, I_in 1599–1618 mA. Quiet.
- 5.3–12.2 s: V_out 26.47–28.68 V (sd 0.5 V), on_ns hunting 1831–1922 (the cap), I_in 1571–1587 mA. **More
  commanded on-time, less input current**: a 1917 ns pulse took 1575 mA here and 1875–1892 mA at 32/33 V, so
  about 15 % of the pulse energy was missing in this regime. The loop was alternating between the cap and
  something that drops the delivered energy. Candidates, none confirmed: pulse skipping (the skip band is
  setpoint + 200 mV; a limit cycle between skipping on an overshoot and the cap on the sag would look like this);
  the 12.5 A peak comparator (`PEAK_PROTECT_MAX_MA`) cutting pulses — but the same pulse at 32/33 V drew the
  expected current. Not inductor saturation (L1 saturates at 21 A).
- The change came 0.3 s after the script's scope capture (5.0 s into every step). The other four steps had the
  same capture and did not change, so this is a coincidence until a run without `--scope` says otherwise.
- The 240 µs capture at 5.0 s sits at the transition: V_out median 27.49 V, period-mean range 0.09 V, 200 kHz
  line 0.21 V — no slow swing inside 240 µs. The swing is slower than 1 kHz and faster than the 4 Hz sampling.
- 28 V / 25.3 Ω is the point `regulator_config.h` records as marginal in DCM (2026-10-15: collapsed in BOOST,
  moved to buck-boost at 10 A peaks). It needs 87 % of the buck-boost DCM cap here; 30 V needed 97 % and was steady.

**V_out at 200 kHz (scope, 10× probe, converter side of Q5/Q6):** 1–99 % spread 1.0–1.2 V, raw p-p 4.7–5.4 V
(spikes; lines at 18–22 MHz, 0.04–0.07 V). Period-mean range 0.01–0.09 V inside each capture.

**To resolve the 28 V regime (next resistor run, no pack):** the 20 kHz regulator trace, armed by the script 1 s
into the run so the start ramp cannot fire it:

    uv run --quiet --project tools/bringup python bench/results/2026-11-12/scripts/boost_cal.py resistor 28000 --ccm dcm --trace error:800 --go

`--trace error:800` fires when |setpoint − V_out| passes 0.8 V (regime 1 stayed within ±0.16 V, regime 2 reached
1.5 V) and keeps 25 ms before and 77 ms after at 50 µs per sample: V_out, I_cmd, its ceiling, on-time and the
skipping / at-limit flags, saved to `captures/trace_resistor_dcm_28000.csv`. Not `--trigger ceiling`: that flag is
the DCM *window* limit (`t_on >= t_lim_ns`, about 2.4 µs at 28 V), and the 10 A peak cap acts through `i_max_ma`
at 1.92 µs, under it, so a ceiling arm stays silent through this step (Dan armed one at 19:2x; the script's arm
resets it). The script also logs `skip` and `skip_events` per sample. No `--scope`, which separates the capture
from the transition; `--seconds 20` if 12 s does not reach the second regime.

## 6. The 28 V regime at 20 kHz (19:36:58, `resistor 28000 --ccm dcm --trace error:800`)

`boost_cal_resistor_dcm_28000-28000.json`, trace `captures/trace_resistor_dcm_28000.csv` (1538 samples, 50 µs each:
trigger at sample 1, so the error was already over 0.8 V when the trace was armed 1 s into the run). No scope.
Ammeter 1019 mA (entered as 1.019 at the prompt; the file is corrected). No fault, no abort.

**This run was in the second regime from the first sample** (0.2 s: on_ns 1761; from 0.5 s: 1830–1915, VD_MON
25.9–28.5 V). The first run's 5 s of quiet were not repeated.

**It is a limit cycle at the loop's own rate, not noise.** In the trace, consecutive 50 µs samples go
26.7 → 27.5 → 28.3 → 26.7 → … : period 3 PID cycles (spectrum peak 7.0–7.3 kHz, mean upward-crossing period
0.14 ms), V_out estimate 25.8–28.8 V, sample-to-sample |ΔV| median 0.95 V, 95 % 1.8 V, max 3.0 V. In step with it:
- `i_cmd_ma` 1364–1730 and the on-time 1790–1917: at the low samples (26.4–27.3 V) `i_cmd` equals `i_max_ma`
  (the DCM ceiling, 41 % of samples) and the on-time is at the 10 A cap; at the high samples (28.3–28.8 V)
  `i_cmd` is 1405–1470 and the on-time 1810–1840 ns.
- Skipping is on in 11.5 % of the PID samples, and `regulator_skip_events` rose about 17,800 per second
  (about 8,900 skip episodes a second, one every 22 switching periods, i.e. every cycle of the oscillation).
  The 200 kHz skip decision (AWD on raw VD_MON, band setpoint + 200 mV) sits between the PID's samples, which is
  why the PID-rate flag catches only part of it.
- I_in 1574 mA against 1612 mA in the quiet regime of run 1: the skipped periods are the missing energy.

**Why it can swing 2 V in 50 µs.** The output capacitance is 5 × 10 µF X7R, about 10 µF at 28 V (`regulator_config.h`,
LOOP_C_INT_UF comment; C14/C15 not fitted). Ten 10 A buck-boost pulses deliver about 2.3 mJ per PID cycle and the
load takes 1.5 mJ: +0.8 mJ is +3 V on 10 µF at 27.5 V, and a skipped cycle is −5 V. The loop gains are computed for
C_int + C_ext = 300 µF with the switch on (kp 0.19 mA/mV, ki 29.6 /s, fc 100 Hz), 30× the capacitance present with a
resistor on VBUS. This is the mechanism of plans/240w-roadmap.md section 2 (the 3.9 kHz cycle at 36 V and at 20 V
no load): the loop gain at the delay's −180° is (fc / f)(C_model / C_seen) g, and here the skip band and the 10 A
cap close the cycle. Near the cap the modulator cannot follow the P term downward past the skip band nor upward
past the cap, so once perturbed into it the cycle holds (run 1: 5 s quiet at 1785 ns, then the cycle for 7 s;
run 2: the cycle throughout). 26 V (on-time 1673, 87 % → 69 % of cap) and 30 V (97 %, V_out under the band) stayed
out of it in run 1; 32/33 V cannot reach their skip band at all. Inference beyond the trace: the ceramics' value.

**What it means.**
- For the 8S pack: nothing. The pack is the output capacitor; VD_MON cannot swing like this on it.
- For ID_MON: the first run's calibration stands (section 5). Against the ammeter this run reads 1074 mA
  (calibrated) for 1019 mA, ratio 0.949, while the first run's scope V/R gave 0.99–1.00 at every step. One meter
  reading of a 7 kHz 2 V swing into 25.3 Ω is not a calibration point; the resistor fit uses the scope.
- For a real device at 28 V / 1 A (the 0.57 V p-p boost ripple at 28 V EPR, bench/results/2026-11-03/README.md): this is
  that item with a device whose bulk capacitance is small. Candidates, not tried: a lower skip band when the DCM
  command is near its ceiling; `regulator_c_ext_uf` set from a measured C (the up-ramp method in the config
  comment); the electrolytics. Not for this branch.

**To see it on the scope** (optional): `--scope --scope-tdiv 100e-6` captures 1.2 ms at 40 ns (8 cycles of the
swing, 125 points per switching period); the record's `v_out_period_mean_pp_v` is the swing with the 200 kHz ripple
averaged out. Applied to the run-1 captures: 0.04 V (26 V), 0.10 V (28 V, at the transition), 0.09 V (30 V),
0.01 V (32, 33 V).

**Order from here:** `resistor --ccm forced` at 26, 28, 30 V (the band's boost kind; 32/33 V only if CCM gets
there). CCM has no skip band and no 10 A cap, and the 28 V point is the one to watch for its own cycle (the
CCM current limit cycle of 2026-11-09 section 12 was into the pack, at the limit).

## 7. Pack runs, 8S (19:55–20:26, Dan ran them) and the ~1.1 A stall

Pack read 30.98–31.25 V (VD_MON, switch closed, nothing switching). V_in 24.4–24.6 V. Ammeter in series.

| run | step (mA) | result | ID_MON tail | ammeter |
|---|---|---|---|---|
| `pack_dcm_250-1000` | 250 / 500 / 750 / 1000 | DCM buck-boost, limiting | 256 / 509 / 749 / 996 mA | 247 / 496 / 745 / 995 mA |
| `pack_forced_500-1000` | 500 | **SW_OCP** 0.2 s in (SW_OCP 1500; ID_MON 1867 mA, I_in 2.5 A in the one sample) | — | — |
| `pack_forced_1000-1000` | 1000 | CCM boost kind in BUCK_BOOST mode, u −255 mV | 998 | 997 |
| `pack_forced_1000-5000` | 1000 / 2000 / **3000** | 1000, 2000 good (u −345, +25 mV); **3000 stalled at ~1.1 A**, Dan stopped it | 1000 / 1998 / 1102 | — / 1997 / — |
| `pack_forced_2000-3000` | 2000 / 2500 / 3000 | all good; BOOST mode from 2500 | 1998 / 2503 / 3005 | — / 2496 / 2999 |
| `pack_forced_3000-5000` | 3000 | Dan stopped it at once | — | — |
| `pack_forced_2500-5000` | 2500 / 3000 / 4000 / **5000** | 2500 started at 2.5 A then ceiling-bound at 1.31 A (CCM, not limiting, u −481 mV); 3000 → 2505; 4000 good; **5000: SW_OCP** at the BOOST-mode CCM entry (ID_MON 5194 mA, SW_OCP 5000) | 1312 / 2505 / 3997 | — / — / 3997 |
| `pack_forced_4000-5000` | **4000 / 5000** | **both stalled at ~1.1 A**, Dan stopped it | 1083 / 1100 | — |

**ID_MON against the ammeter:** DCM fit gain 1.004, offset −11 mA, residual ≤ 4.8 mA; CCM ratios 0.990–0.994
(1–4 A). The flashed 991 / +4 holds in boost DCM and boost CCM on the pack. The 0.7× of 2026-11-03 is not seen.
**Boost u, CCM not limiting:** −481 mV (one step); limiting −255 to +250 mV. The −440 mV preset is near.

**The stall: a mode-selection limit cycle, measured.** After the 4000-5000 run (read over SWD, nothing actuated):
`regulator_ccm_entries` = `exits` = 24715; the last 8 exits were `MODE` at 1 ms intervals (ticks …097 to …104),
then `STOP`. The mode log has no buck-boost → boost change in that run (its last entry is 38 s before the stop).
The stalled steps all have ceilings above V_in + 7 V + 0.5 V (≈ 32.0 V: 32095, 32471, 32700 mV) with V_out (VD_MON)
around 31.0–31.9 V, under the band's top edge 31.5 V; the good steps at those limits reached BOOST mode (mode log:
2 → 1 at VD_MON 31.44–31.60 V). The sequence, from the code with those numbers:
1. BUCK_BOOST mode, boost-kind CCM enters (forced), the current rises to the limit, V_out = pack + I × R rises
   over 31.5 V (0.17 Ω × 3 A ≈ 0.5 V).
2. `determine_mode_from_voltages` returns BOOST, so 3b requests `CCM_EXIT_MODE` (regulator.c "Out of CCM first").
3. DCM buck-boost at the 10 A cap carries about 1.1 A, V_out falls back under 31.5 V within `CCM_MODE_SETTLE`
   (4 cycles), so no mode change happens.
4. After `CCM_REENTRY_HOLD` (1 ms) CCM enters again: back to 1.

**Scope (Dan stopped it on one stalled run; memory read back, `captures/pack_stuck_sw1_vin_sw2_vout.csv.gz`,
CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out, 1.2 ms at 40 ns):** 540 µs of DCM buck-boost, 197 µs of CCM, DCM to the end.
- DCM buck-boost periods: Q1 + Q4 on 1.9 µs (SW1 at V_in, SW2 at −0.6 V: the 10 A cap); both off, L1 discharging
  through the body diodes for 1.2 µs (SW1 −3.4 V, SW2 33.6 V with V_out 30.7 V); then SW1 and SW2 ring in
  antiphase between about 2 and 25 V until the next period (L1 with the switch-node capacitance, both legs off).
- CCM periods: SW1 held at V_in except the 300 ns Q2 refresh; Q4 on 1.0–1.2 µs, Q3 the rest.
- In both, SW1 sits within 0.5 V of the CH2 V_in trace while Q1 is on (the probes' accuracy). This record does not
  show Q1's gate failing. What looks like a collapse is the end of the discharge and the ringing with both legs off,
  and the "maxed duty" is the 1.9 µs pulse at the peak cap. Inference: Q1's heat is the buck-boost DCM (hard
  turn-off at about 10 A every period, ~85 % of the time in this cycle), not a gate fault. Board temperature was
  not logged.

**Fix (flashed 2026-11-12, `build/BatteryNext` sha1 `7a024ccf…`, verified; not yet run on the pack):** in 3b, a buck-boost → boost change no longer
takes boost-kind CCM out: those legs are the same in both modes (Q1 held on with its refresh, Q4/Q3 switching), and
the DCM legs are applied from `regulator_mode` at the eventual exit. The change runs when CCM ends for another
reason, after `CCM_MODE_SETTLE` as before. Boost → buck-boost and every other exit are unchanged. Host tests pass;
this path is not host-modelled.

**Script fixes (`boost_cal.py`):** SW_OCP floor 1500 → 2500 mA (the forced 500 mA start); CCM pack limits capped at
4000 mA (SW_OCP 5000 needs 1000 mA over the limit: 5000 / 5000 tripped at the entry, 4500 / 5000 in the re-test below).

**Re-test with the fix (23:09–23:11, `pack 3000 4000 4500 --ccm forced --trace error:800`, Dan ran it,
`boost_cal_pack_forced_3000-4500.json`):**

| step (mA) | pack read (mV) | ceiling (mV) | result | V_out tail (mV) | ID_MON tail (mA) | ammeter (mA) | u (mV) |
|---|---|---|---|---|---|---|---|
| 3000 | 31148 | 32164 | CCM boost kind, BUCK_BOOST mode, limiting the whole run | 31852 | 3001 | 2998 | −53 |
| 4000 | 31165 | 32453 | same | 32100 | 4007 | 3994 | +106 |
| 4500 | 31217 | 32641 | **SW_OCP** at the first sample, 0.22 s (filtered 5017 mA, SW_OCP 5000) | — | — | — | — |

- Both good steps sat with V_out over V_in + 7 V (31.5 V), where the old code cycled. Read over SWD after the run:
  `regulator_ccm_entries` = `exits` = 3 (one per step), all three exits `STOP`; the mode log is empty (no mode change
  since the flash). The trace saw CCM and the limit in all 2048 samples of the 3000 and 4000 steps, no skipping, no
  0.8 V excursion. The stall is not seen.
- 4500 / 5000: the same trip as 5000 / 5000. Inference: the CCM entry overshoots about 500 mA over the limit (one
  sample here; the trace arms at 1 s, so it saw nothing). Fault left latched for Dan.
- Q1 temperature not logged.


**Open:**
- The forced 500 mA SW_OCP at 0.2 s: one sample (ID_MON 1867 mA with the limit at 500, CCM not active at that
  sample). Not explained.
- The 2500 mA step of `pack_forced_2500-5000` fell from 2.5 A to 1.31 A at a fixed ceiling, CCM, not limiting. With
  the ceiling fixed from the pack read, a rise in the pack's voltage under charge (polarization) takes the margin.
  Inference; the margin (1.6 × I × 170 mΩ + 200 mV) may need more at ≥ 2.5 A.
- With the fix, more pack runs stay in BUCK_BOOST mode with boost-kind CCM. In the 2500 mA case above that was
  the mode in which the step fell to ceiling-bound.

## 8. 5 A and the DCM-to-CCM ramp: the build (flashed 23:32, verified; results in section 9)

SW_OCP at ID_MON's full scale (Dan): ID_MON's ADC full scale is 5.5 A, so the BatteryNext build caps SW_OCP's level
at about 5.3 A (5.35 A raw after 991 / +4; 150 mA raw of headroom for the INA281's swing), and that is the battery
build's default. Also a firmware limit ramp (`regulator_ccm_ilim_ramp_ma_s`). Built in `build/BatteryNext` (sha1
`db08875a…`, on top of the mode-cycle fix). Design: plans/lipo-load.md section 6. Host tests, tool tests and the
script self-test pass; neither piece is in the host model.

This build adds RAM variables, so `bu` and the scripts must read symbols from it: `tools/bringup/bringup.local.toml`
(not tracked) now sets `elf_cmake = "build/BatteryNext/final.elf"`. Read against `build/Battery` right after the
flash, status showed garbage (gain 1010, limit 65535). The 7a024ccf image of the re-test (section 7) changed only
code in step 3b and its reads were consistent (991 / +4, SW_OCP 5000, 3 entries), so those were read at the same
addresses; the layout itself was not compared.

`boost_cal.py`: every CCM run at full-scale SW_OCP (refused before START on an older build); stop level capped at
7500 mA; new `ramp` phase. Plans, each on Dan's go:

    uv run --quiet --project tools/bringup python bench/results/2026-11-12/scripts/boost_cal.py pack 5000 --ccm forced --trace error:800 --go
    uv run --quiet --project tools/bringup python bench/results/2026-11-12/scripts/boost_cal.py ramp 150 5000 --rate 100 --go

## 9. 5 A step and the DCM-to-CCM ramp (23:34–23:36, Dan ran both, firmware `db08875a…`)

**`pack 5000 --ccm forced` (`boost_cal_pack_forced_5000-5000.json`): SW_OCP at the first sample (0.22 s).** Pack
31200 mV, ceiling 32700 mV, full-scale level 5305 mA. That sample: ID_MON filtered 5360 mA, newest scan 5441 mA
(at full scale), I_in 8696 mA, V_in × I_in / V_out 6516 mA. The input side says the cold CCM entry at 5 A overshot
to about 6.5 A (an upper bound; ID_MON was pinned), more than the ~0.5 A seen at 4.5 A. Full-scale SW_OCP did trip
over full scale, as designed.

**`ramp 150 5000 --rate 100` (`boost_cal_ramp_auto_150-5000_100.json`, trace `captures/trace_ramp_auto_150-5000_100.csv`):
ran to the end, no fault.** Pack 31131 mV, ceiling 32631 mV.

| segment | limit (mA) | ID_MON − limit, mean (min..max) | CCM share | mode |
|---|---|---|---|---|
| hold at 150 | 150 | −1 mA | 0 | BUCK_BOOST |
| up | 175 → 4990 | −4 (−87..+84) mA | 0.97 | BUCK_BOOST |
| hold at 5000 | 5000 | +20 (−82..+80) mA | 1.0 | BUCK_BOOST |
| down | 4975 → 156 | 0 (−83..+96) mA | 0.98 | BUCK_BOOST |
| hold at 150 | 150 | +14 (−5..+19) mA | 0 | BUCK_BOOST, BOOST in the last sample |

- One CCM entry and one exit (counters and exit log: `LIGHT_LOAD`). Entry at a limit of 294 mA (ID_MON 296 mA): the
  DCM command, which reads about 1.2 × ID_MON here, reached 400 mA (trace: 406 mA). Exit at a limit of 217 mA (ID_MON
  mean 204 mA, the 200 mA threshold).
- The handover in the trace (20 kHz, 25 ms before, 77 ms after): per-cycle ID_MON 248–328 mA in the first 5 ms of
  CCM against a limit near 300 mA, 199–374 mA after; V_out 31.17–31.49 V. No overshoot at the entry.
- Top hold: ammeter 4993 mA, ID_MON 5020 mA, V_out 32571 mV (VD_MON) against a ceiling of 32631 mV, so the
  limit held with 60 mV of margin before the ceiling would bind. V_in 24.44 V, I_in 6.67 A.
- V_in × I_in / V_out over ID_MON in CCM: 1.031 near 1 A, 1.015 near 2 A, 1.005 near 3 A, 1.003 near 4 A, 0.998 near
  5 A. That is under 1.0 at 5 A, which an efficiency below 100 % cannot give, so the sensors (VD_MON reads high) are
  within a few percent here, no better. No efficiency is claimed.
- The mode changed BUCK_BOOST → BOOST in the last sample (t 111.98 s, 150 mA, DCM, V_out 31338 mV with V_in
  24616 mV). The documented rule needs V_out over V_in + 7 V; the samples are 0.3 s apart, so an excursion between
  them is possible. Not explained.
- Board temperature not logged.
