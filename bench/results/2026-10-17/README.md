# Bench log 2026-10-17 and 2026-10-18: VBUS at 28 V, 20 → 28 V ramp, 5 V connect, Q5/Q6 drop and reapply

| Field | Entry |
|---|---|
| Dates | 2026-10-17 11:06–17:14 EDT (steps 1–9); 2026-10-18 09:06–09:16 EDT (step 10). EDT = UTC−4 |
| Operator | Dan: wiring, loads, scope probes, approvals. Claude Code: commands through `tools/bringup` (`bu`) and `scripts/` |
| DUT | PD Charger, STM32G474 four-switch buck-boost, HRTIM 200 kHz, DCM current-command modulator, PID at 20 kHz |
| Planned | Output impedance of the converter node from electronic-load and VBUS-contract transients, for comparison with `plans/scripts/loop_model.py` |
| Executed | Steps 1–10. The load step could not be applied: the QT Py buck does not convert (step 3). Replaced by drop and reapply of the load with Q5/Q6 (steps 8–10) |
| Hardware changes | None |
| Analysis | `report/TEST_REPORT.md`, cited as R§: definitions, results, scorecard, uncertainty, limitations. Requirement register: `plans/stability-requirements.md` |
| Data | Committed: `scripts/`, `metrics/`, `figures/`, `report/`. Git-ignored, on the bench machine only: `data/` (84 MB) |
| Status | Complete. Statements withdrawn from the first version of this log: §4 |

## Terms

Full definitions with resolution: R§2.2. Uses of the word "dip" in this repository: R§2.3.

| Term | Meaning in this log |
|---|---|
| TP2 | Converter output node `/DC-DC/VOUTa` (five 10 µF X7R 1206), ahead of R6, VD and Q5/Q6. Scope point in steps 2–6 and 10 |
| V_out | Regulator measurement at VD (VD_MON, after R6, ahead of Q5/Q6); trace column `v_out_mv` |
| VBUS | Q5/Q6 output to J3, where the 25.3 Ω and the QT Py sit. Not probed in this log |
| OUTPUT_EN | GPIO PC8, gate drive of Q5/Q6. Low = switch commanded open |
| Drop and reapply | OUTPUT_EN low for T_cmd, then high. The load leaves V_out T_release after the falling edge and returns at the rising edge |
| T_cmd | OUTPUT_EN low time. Runs 4–43 and F: TIM4 request + 1.4 µs. Runs 1–3: whole 50 µs PID cycles, ±50 µs |
| T_release | OUTPUT_EN falling until the load leaves V_out: 1.35 / 4.15 / 3.90 / 4.20 ms at 5 V / 20 V / 28 V SR off / 28 V SR on, ±0.15 ms (R§6.1) |
| T_unl | T_cmd − T_release, the time the load was off. Sub-release pulse: T_unl ≤ 0.25 ms (load never left). Event: T_unl > 0.25 ms |
| V_0, σ_pre | Mean and population sd of the 147 pre-trigger trace samples (7.35 ms) |
| ΔV_os | Max V_out from the trigger sample to the reconnect sample, minus V_0: the rise while the load is off |
| ΔV_us ("dip") | V_0 − min V_out from the reconnect sample to the end of the 95 ms record: the undershoot when the load is reapplied, at the converter node. Not a calibrated load step, not a PD receptacle measurement, not an instability measure (R§2.4) |
| t_min, t_rec(2 %) | Reconnect sample to the minimum; reconnect sample to the last sample outside V_0 ± 2 %, plus one sample ("—": band never left) |
| E_bg | Largest ΔV_us among sub-release pulses at that voltage: 0.09 / 0.24 / 0.39 V at 5 / 20 / 28 V SR off; none at 28 V SR on |
| Railed | I_cmd ≥ I_max − 2 mA: the loop command at its ceiling |

## 1. Configuration

### 1.1 Hardware

| Item | Configuration |
|---|---|
| Source | 1500 W server supplies, 24.56 V at the operator's meter, 10 in of 8 AWG, XT60. Regulator V_in reads 24.04–24.15 V |
| Output capacitance at TP2 | 5 × CL31B106KBHNNNE (10 µF 50 V X7R 1206); C14/C15 (82 µF) not fitted. DC-bias value (calc): ≈ 44 / 16 / 10 µF at 5 / 20 / 28 V |
| Sense and switch | TP2 → R6 (12 mΩ) → VD (VD_MON, ADC1 PA0, LSB ≈ 17.2 mV) → Q5/Q6 (SiR5607DP, back-to-back P-FET; R13 51 kΩ, R27 10 kΩ) → VBUS/J3 |
| Q5/Q6 gate (schematic calc, not probed) | ≈ −4.2 V at 5 V; −13 V (13 V zener clamp) from 20 V up |
| Load, after Q5/Q6 | 25.3 Ω (derived from the 15 V reading, 2026-10-16) and the QT Py buck input (≈ 100–120 µF ceramic) |
| QT Py buck input current (buck not converting) | Measured by Dan at the buck input; the QT Py logic can run from USB, so this is the buck converter's input current, not the QT Py's. 73 mA at 5.17 V (total minus V/25 Ω); 80 mA at 15.17 V and ≈ 90 mA at 28 V (2.5 W) measured; ≈ 85 mA at 20 V interpolated. 28 V SR off totals 2026-10-16: 1.74 A in, 1.19 A out at 27.87 V |
| QT Py buck state | Not converting since 2026-10-07 (last switched power, 15 V). IC8 12 V gate rail is a 38 V part; 42.5 V and 47 V seen 2026-10-12; 36 V burst 2026-10-15 |
| PD sink | Sink board, `bu sink` (/dev/ttyACM2). Its `hard_resets` counter is the sink-side count; the G474 source-side count is `bu pd status` `hard_reset_count` |

### 1.2 Instruments

| Instrument | Use and settings |
|---|---|
| Oscilloscope Rigol DS1104Z (8-bit, 100 MHz) | CH4 on TP2, probe setting 10×; probe model and ground lead not recorded. Per-record settings: §6 |
| Scope baseline (read back 11:08:29) | CH4 DC, 5 V/div, offset −20 V, bandwidth limit off; 1 µs/div, offset 0; trigger CH2 rising 5.4 V, auto, HF reject; high resolution, memory depth 300,000 |
| Scope limits found 2026-10-18 | Below 5 V/div the offset stops at −20 V; at 1 V/div the trigger level clamps at 25 V |
| Regulator trace | 20 kHz, 2048 samples (147 pre-trigger, 1900 post). V_out = trimmed mean of the last 9 VD_MON samples (≈ 45 µs of each 50 µs point). I_cmd truncated to 1 mA |
| Pulse timer (runs 4–43, F) | TIM4 one-pulse, 1 µs ticks, interrupt priority 1. Pulse measured with the DWT cycle counter (170 MHz): request + 1.4 µs |
| QT Py | `bu load`, /dev/ttyACM1 |

### 1.3 Firmware

| Steps, runs | Image | Identity |
|---|---|---|
| Steps 2–7 | Build of 0f0b165, flashed 2026-10-16 | Commit 0f0b165 |
| Step 8, runs 1–3 | Uncommitted build: `SWITCH_PULSE` in whole PID cycles. Built 13:06:19, flashed 13:06:38 | `data/firmware/final_runs1-3_flashed_20261017-130638.bin`, 150,632 B, sha256 `a1753b1e…f91692`. A comment-only rebuild at 13:10:53 reproduces it byte for byte |
| Steps 9–10, runs 4–43, F | `final.elf` built 17:07:35, flashed 17:07:52 | Source = commit da5a649 (committed 2026-10-18 16:52:48, after the tests; `git status STM32CubeIDE` clean). ELF 3,050,640 B, sha256 `eb49b8f6…891885` |

da5a649 against 5ccd2c5: 9 files, +471/−14: regulator trace, TIM4-timed `SWITCH_PULSE`, VBUS read guard. No control-law, gain or limit change.

| Controller constant | Value |
|---|---|
| Gains, switch flag on (C = 300 µF) | kp 0.1885 mA/mV, ki 29.61 mA/mV/s |
| Gains, switch flag off (C = 50 µF) | kp 0.0314 mA/mV, ki 4.93 mA/mV/s |
| Crossover setting | 100 Hz (`LOOP_CROSSOVER_HZ`); gains recomputed every PID cycle from `output_switch_on` |
| PID rate | 19,973 cycles/s (counted 11:17:13 from `debug_log.total_count`, 3 s apart) |
| Setpoint slew | 4 mV per cycle = 79.9 V/s |
| Skip band | max(200 mV, 15 ‰ of setpoint): +0.20 / +0.30 / +0.42 V at 5 / 20 / 28 V |
| Protection | OVP 110 %; UVP 50 %; Hard Reset when VBUS is > 8 % under setpoint for 200 ms |

### 1.4 Synchronous rectifier (SR) state

| From (EDT) | To | `regulator_sr_enable` | Steps |
|---|---|---|---|
| 2026-10-16 22:29:33 | 2026-10-17 11:42 (G474 reset) | 1 | 2–5 and the pre-trigger window of the step 6 reset capture |
| 2026-10-17 11:42 | 2026-10-18 09:07:11 | 0 (boot default) | 6 (connect), 7, 8, 9 |
| 2026-10-18 09:07:11 | 2026-10-18 09:15:57 | 1 | 10 (series E and F) |
| 2026-10-18 09:15:57 | — | 0 | End state |

Two commands combining a 28 V request with `bu mem write regulator_sr_enable 1` were blocked by the session's permission classifier on 2026-10-17 at 18:08:45 and 18:09:49 EDT; SR stayed off. The write at 2026-10-18 09:07:11 followed Dan's approval.

## 2. Step index

| Step | Date, time (EDT) | Activity | Outcome | Data |
|---|---|---|---|---|
| 1 | 10-17 11:06 | Locate TP2 | TP2 = `/DC-DC/VOUTa`, converter side of R6 and Q5/Q6 | — |
| 2 | 10-17 11:08–11:11 | 28 V, SR on: AC-coupled records at TP2 | Records clipped and point-sampled; their statistics withdrawn (§4). 12 ms record usable with boxcar averaging | `data/cap/vbus28_ac_*`; `metrics/vbus.json` |
| 3 | 10-17 11:12 | 3 W on the QT Py at 28 V | QT Py buck did not convert; no load step applied | `data/load_run_3w.json`; `data/cap/vbus28_ac_2s4_loadrun.csv` |
| 4 | 10-17 11:14–11:17 | 20 → 28 V contract change at TP2 | Ramp 83.4–90.0 V/s against 79.9 V/s set | `data/cap/vbus_20to28_ramp.csv`; `metrics/vbus.json` |
| 5 | 10-17 11:17–11:18 | Scope restored, 5 V contract, files copied | — | — |
| 6 | 10-17 11:41–11:47 | G474 reset from 5 V; 5 V connect at TP2; connect fits | Fall to 2.5 V; connect low 3.15 V, back to 5.0 V after 15.18 ms; fits do not identify the bench side | `data/cap/reset_from5v_raw.csv`, `connect5_raw.csv`; `metrics/connect_fit*.json` |
| 7 | 10-17 11:44 | State record | 5 V, RUNNING, OUTPUT_EN 1 | — |
| 8 | 10-17 13:05–13:12 | `SWITCH_PULSE` in PID cycles; runs 1–3 at 5 V | 5 V events: ΔV_us 1.22–1.24 V (24.4–24.8 %) | `data/steps/` |
| 9 | 10-17 17:07–17:14 | TIM4-timed `SWITCH_PULSE`; series B, C1–C3, D (runs 4–35) | ΔV_us rises with T_unl at 1.33 / 1.65 V/ms (20 V / 28 V SR off) | `data/steps_tim4/` |
| 10 | 10-18 09:06–09:16 | 28 V SR on: series E (runs 36–43); TP2 scope capture F | ΔV_us 0.57–2.82 V; at TP2 ΔV_us 2.50–3.01 V depending on averaging window | `data/steps_tim4_sr/`, `data/scope_28v/`; `metrics/tp2_28v_6ms.json` |

## 3. Steps

### Step 1. TP2 location (2026-10-17 11:06)

| Item | Entry |
|---|---|
| Method | Net query of `KiCad/PD Charger.kicad_pcb` for TP1–TP4, J3, R6, Q5, Q6, J2, C16–C19, C49, U8 |
| Result | TP2 = `/DC-DC/VOUTa`: converter output capacitors, ahead of R6, VD and Q5/Q6 |
| Correction | An earlier reading of the bench notes put the 25.3 Ω and the QT Py on TP2. Dan: both are after Q5/Q6. `bench/README.md` corrected 11:46:44 |

### Step 2. TP2 at 28 V, SR on, AC-coupled (11:08–11:11)

Objective: ripple and low-frequency content at TP2 with the 25.3 Ω and the unarmed QT Py connected, as the reference for step 3.

```text
11:08:12  bu sink status                     -> attached CC1, epr_mode 1, contract 5000 mV, hard_resets 369
11:08:34  bu sink req 28000 3000 --until contract   -> matched
11:08:39  bu regulator status                -> RUNNING, BUCK_BOOST, target 28000, sr_enable 1,
                                                i_cmd 1395.46 mA, i_max 1680.07 mA, v_out 27940 mV
11:08:44  scope chan 4 --coupling AC --scale 0.2 --offset 0 --bwl 20M
          scope timebase --scale 0.001 --offset 0.004
          scope acquire --type HRES --mdepth 1.2M          (300,000 points delivered)
          scope trigger --source CHAN4 --slope POS --level 0.15 --sweep SING --coupling HFR
11:09:16  scope single                       -> STOP (fired at once)
11:09:49  scope capture CH4 --raw            -> 12 ms record
11:10:22  scope timebase --scale 0.2 --offset 0; trigger CHAN4 POS 0.9 V AUTO HFR;
          scope run; 3 s; scope stop; capture -> 2.4 s record (unarmed)
11:11:52  bu mem read regulator_skip_events, 2 s apart -> 802610, 802760
```

| Validity check | Result | Consequence |
|---|---|---|
| Clipping | Samples at the lowest / highest code: 5.42 % / 0.83 % (12 ms), 5.36 % / 0.81 % (2.4 s). In 2400 blocks of 5 µs, block mean against fraction at the lowest code r = −0.64 | Minima, p-p and rms are bounded by the screen window (−1.01 to +1.88 V), not by the signal |
| Sampling, 2.4 s records | 8 µs point samples against a 5 µs switching period. High-resolution mode did not average: record σ 0.569 V, at ripple level (5 µs boxcar of the 12 ms record: 57 mV rms) | Switching ripple aliases into the band; rms, p-p, minima counts and spectral lines from these records are withdrawn (§4) |
| Alias test on the 12 ms record | Decimated to 8 µs at 40 phases, 12-sample average: rms 172 mV (63–289), p-p 1.0 V, 281 minima/s. True 12-sample (96 µs) boxcar: 49 mV rms | The withdrawn 198 mV rms and 485 minima are consistent with aliasing |
| Delivered depth | 1.2M points requested, 300,000 delivered | Interval 40 ns (12 ms) and 8 µs (2.4 s) |

Usable result, 12 ms record (`metrics/vbus.json` `dense_12ms`):

| Boxcar | rms about mean (mV) | p-p (V) |
|---|---|---|
| 5 µs (one switching period) | 57 | 0.729 |
| 100 µs | 48 | 0.516 |
| 500 µs | 23 | 0.118 |
| 1 ms | 16 | 0.062 |

| Excursion below the 5 µs-boxcar median (0.007 V) by > 0.25 V | Duration | Deepest | Samples at the lowest code in the event |
|---|---|---|---|
| 2.767–2.871 ms | 104 µs | −0.607 V (lower bound, clipped) | 14.9 % (record p50 5.6 %, p95 6.4 %) |
| 9.268–9.357 ms | 90 µs | −0.527 V (lower bound, clipped) | 12.1 % |

Skip counter: 150 counts in 2 s = 75 transitions/s at 11:11:52 (28 V, SR on; status: pulses_skipped 0, i_cmd 1382.0 mA, i_max 1679.05 mA, on-time 1772 ns). One reading. The regulator traces of steps 9–10 show 0 SKIPPING-flag transitions in their windows; unresolved (R§10.1 U11).

### Step 3. 3 W step on the QT Py at 28 V (11:12)

Objective: apply a 3 W load step after Q5/Q6 and record TP2.

```text
11:12:08  scope run
          bu load run --p 3000 --seconds 4 --limit-ma 3000 --contract-mv 28000 --vin-min-mv 24000
          scope stop; scope capture CH4 --raw      (record = last 2.4 s before the stop)
```

| QT Py telemetry (`data/load_run_3w.json`) | Value |
|---|---|
| Frames | 83 over t_s 0.051 to 4.152 s: 80 `running` (enable 1), 3 `off`; fault `none` in all |
| V_in | 27075 to 27892 mV (mean 27,486 mV) |
| V_out | 56 to 141 mV |
| I_out | −93 to 82 mA |
| P_out | 0 to 1 mW |
| Duty | 0 to 302 ‰ (0 at the first frame; 158 / 210 / 267 / 298 ‰ at 0.35 / 0.65 / 0.95 / 1.25 s; 296–302 ‰ from 1.5 s) |
| Temperature channel | 1751 to 1821 mV |

| Result | Value |
|---|---|
| Stimulus | None: the QT Py buck reached ≈ 300 ‰ duty with ≤ 141 mV and ≤ 1 mW out. |
| TP2, 20 ms block means (120 blocks per 2.4 s record) | Load run −15.5 to 70.8 mV; unarmed −12.1 to 70.7 mV. No step distinguishable from the unarmed record |
| Consequence | No load step is available on this hardware. Plan changed to drop and reapply of the existing load with Q5/Q6 (steps 8–10; R header, plan-change row) |

### Step 4. 20 → 28 V contract change at TP2 (11:14–11:17)

Objective: the converter-node trajectory during a VBUS contract change.

| Attempt | Time | Scope (CH4 DC, 2 V/div, offset −24 V, 20 MHz limit) | Action | Result |
|---|---|---|---|---|
| 1 | 11:14:17 | 0.2 s/div, running | 20 V request; 0.6 s later 28 V request; stop | 5 ms means 28.08–28.27 V over the record; both transitions missed. Not kept |
| 2 | 11:15:48 | 20 ms/div, offset 80 ms; single, CH4 falling 26 V, DC trigger coupling | Armed; 20 V, then 28 V | Triggered on switching spikes before the request. Not kept |
| 3 | 11:16:20 | As 2; trigger CH4 rising 24 V, HF reject | 20 V contract; armed (WAIT); 28 V request | Triggered (STOP); record holds the ramp: `data/cap/vbus_20to28_ramp.csv` |

Source-side PD state 11:15:38 (`bu pd status`): contract 28000 mV / 3000 mA, position 8, last_transition_ms 110, contract_count 9, hard_reset_count 123, transition_timeouts 122.

Ramp slope by fit window (`metrics/vbus.json` `ramp_20_to_28v`):

| Fit window (V) | Slope (V/s) | Residual rms (mV) |
|---|---|---|
| 22 → 27.18 | 89.4 | 134 |
| 21 → 27 | 83.4 | 348 |
| 22 → 27 | 89.8 | 134 |
| 23 → 26 | 85.1 | 78 |
| 21 → 27.5 | 83.7 | 339 |
| 20.5 → 27.5 | 85.5 | 427 |
| 22 → 26 | 90.0 | 146 |
| 24 → 27 | 86.0 | 84 |

| Result | Value |
|---|---|
| Slope range | 83.4–90.0 V/s over the windows above; 20.5 → 27.5 V in 80.2 ms = 87.3 V/s |
| Firmware slew | 4 mV per cycle × 19,973 cycles/s = 79.9 V/s. The scope slope is 4–13 % higher; cause not established |
| End level | 28.18 V. 1 ms means within −0.138 to +0.167 V of it for the next 80 ms; last 80 ms: sd 70 mV, p-p 274 mV (1 ms means) |

### Step 5. Scope restore and 5 V (11:17–11:18)

```text
11:17:46  scope chan 4 --coupling DC --scale 5 --offset -20 --bwl OFF
          scope timebase --scale 1e-6 --offset 0
          scope trigger --source CHAN2 --slope POS --level 5.4 --sweep AUTO --coupling HFR
          scope acquire --type HRES; scope run
11:18:06  read-back: DC, 5 V/div, offset -20 V, BWL off, 1 us/div, trigger CHAN2 5.4 V AUTO   (= baseline)
          bu sink req 5000 3000 --until contract   -> matched; RUNNING, BUCK, target 5000
11:18:14  records copied to data/cap/; git check-ignore -> data/ ignored
```

### Step 6. G474 reset from 5 V and 5 V connect at TP2 (11:41–11:47)

Objective: the TP2 trajectory when the converter stops (reset) and when it restarts and Q5/Q6 close onto the load (connect). The switch closes once per start (regulator.c step 4c), so the reset is the only repeatable close on this build. Reset = `bu probe reset` (G474 via ST-LINK); SR returns to off after it.

```text
11:42:29  scope chan 4 --coupling DC --scale 0.5 --offset -4.5 --bwl 20M
          scope timebase --scale 0.002 --offset 0.008
          scope trigger --source CHAN4 --slope NEG --level 4.6 --sweep SING --coupling HFR
          scope single  (WAIT); timeout 40 bu probe reset -> reset True; 4 s; STOP
          bu regulator status -> RUNNING, no fault, target 5000, OUTPUT_EN 1, v_out 5036 mV
11:42:48  scope capture CH4 --raw -> reset_from5v_raw.csv
11:43:32  scope chan 4 --coupling DC --scale 1 --offset -3 --bwl 20M
          scope timebase --scale 0.002 --offset 0.010
          scope trigger --source CHAN4 --slope POS --level 4.0 --sweep SING --coupling HFR
          scope single  (WAIT); bu probe reset; 4 s; STOP; capture -> connect5_raw.csv
          bu regulator status -> RUNNING, target 5000, OUTPUT_EN 1
11:45:18  first connect_fit.py run; 11:45:46 V_d0 held at 2.65 V, --free added
11:47:14  scope restored to the baseline and read back
```

Reset capture (`metrics/vbus.json` `reset_from5v`; pre-trigger window at 5 V, SR on):

| Quantity | Value |
|---|---|
| Steady window | 3.7 ms ending 0.3 ms before the trigger (46,250 samples). Windows that reach the trigger include the start of the collapse |
| Mean; raw range; 2 µs-mean range | 5.16 V; 1.95–7.732 V; 4.42–5.88 V |
| 50 µs means | p-p 0.049 V, sd 11 mV |
| Lines, Hann window | 99,730 Hz 275.4 mV pk; 200 kHz 22.2 mV; 299,730 Hz 283.4 mV; 499,730 Hz 269.2 mV. 20–90 kHz max 3.9 mV (88,649 Hz); 0.3–3 kHz max 14.1 mV (541 Hz) |
| Collapse, 20 µs means at −500 / −300 / −200 / −100 / −60 / −20 / 0 µs | 5.158 / 5.147 / 5.138 / 4.939 / 4.793 / 4.668 / 4.603 V |
| Fall below 4 / 3 / 2.5 V | 216.6 / 698.2 / 1185.6 µs after the trigger |
| End | Minimum 20 µs mean 2.5 V; last 1 ms mean 2.55 V |

The largest lines sit at half the 200 kHz switching frequency and its odd multiples, with the 200 kHz line at 22 mV: the signature of period doubling. One record, probe ground not controlled; requirement D5 is Fail at 5 V on this record, Not tested at 20 and 28 V (R§7).

Connect capture (`metrics/vbus.json` `connect5`). Time origin 1.505 ms = steepest descent of the 20 µs moving average (1 µs resampling).

| Quantity | Value |
|---|---|
| Start, first 0.5 ms mean | 2.441 V |
| Peak before the close (20 µs mean) | 5.114 V at 1.31 ms |
| Low (20 µs mean; 50 µs mean) | 3.154 V; 3.167 V, 399 µs after the origin |
| 90 → 10 % fall | 413 µs |
| Last 1 ms mean | 5.073 V |

| Slope near the origin | Definition | Value (V/µs) | i = C·dV/dt at C = 44 µF (arithmetic only) |
|---|---|---|---|
| Steepest | 20 µs-spaced difference of the 20 µs mean, at 1.441 ms | −0.009 | 0.40 A |
| At origin, ±1 µs | Central difference of the 20 µs moving average | −0.026 | 1.14 A |
| At origin, ±10 µs | 20 µs span | −0.0047 | 0.21 A |

The three estimates differ by a factor 5.5 and depend on the span; none is a measured current.

| Time after fit origin (ms) | V_out, 20 µs mean (V) |
|---|---|
| 0.3 | 3.342 |
| 0.5 | 3.220 |
| 1 | 3.469 |
| 2 | 3.802 |
| 5 | 4.377 |
| 10 | 4.771 |
| 15 | 4.972 |
| 20 | 5.064 |

| Level (V) | Time after the low (ms), 50 µs mean |
|---|---|
| 4.0 | 2.27 |
| 4.5 | 5.54 |
| 4.8 | 10.07 |
| 5.0 | 15.18 |

Connect fits (`scripts/connect_fit.py`). Fixed: board C 44 µF (5 × 10 µF × 0.88), kp 0.1885 A/V, ki 29.61 A/V/s, delay 63 µs, 25.3 Ω, QT Py 73 mA above 2.5 V. Fitted: bench-side C and initial voltage V_d0, switch-conductance ramp, converter-current gain g (bounds 0.3–3.0), initial integrator I_int0, close time t_c. Window: −0.3 to +20 ms about the origin. Measured: low 3.15 V, 4.38 V at 5 ms.

| Fit | C_bench (µF) | V_d0 (V) | Switch ramp (µs) | g | I_int0 (A) | t_c (µs) | rms error (mV) | Model low (V) | Model at 5 ms (V) |
|---|---|---|---|---|---|---|---|---|---|
| `connect_fit.json`: Bench side held at V_d0 = 2.65 V, ramp ≤ 0.8 ms | 36.5 | 2.66 | 569 | 2.483 | −0.181 | −95 | 153.5 | 3.50 | 4.29 |
| `connect_fit_free.json`: `--free`: C 5–800 µF, V_d0 0–4.5 V, ramp ≤ 2 ms | 271.0 | 3.13 | 2000 | 0.300 | 0.686 | −84 | 64.6 | 3.36 | 4.35 |
| `connect_fit_qtpy.json`: `--qtpy`: C held 100–120 µF, V_d0 0–4.5 V, ramp ≤ 5 ms | 120.0 | 3.38 | 5000 | 3.000 | −0.193 | −109 | 137.4 | 3.47 | 4.30 |

| Fit | Parameters at a bound |
|---|---|
| Held | None (g 2.483 inside 0.3–3.0) |
| Free | Ramp 2000 µs (upper), g 0.300 (lower) |
| QT Py C | C 120 µF, ramp 5000 µs, g 3.000 (all upper) |

Result: bench-side C from 36.5 to 271 µF depending on the bounds, rms error 65–154 mV, model low 3.36–3.50 V against 3.15 V measured. The fits do not identify the bench side. An earlier "413 µF" result is not reproducible (§4).

### Step 7. State record (11:44)

5 V contract, regulator RUNNING, OUTPUT_EN 1 (status at the end of the connect capture). No hard-reset counter was read.

### Step 8. `SWITCH_PULSE` in PID cycles; runs 1–3 at 5 V (13:05–13:12)

Objective: open Q5/Q6 for a set time with the load connected and record V_out and I_cmd with the regulator trace.

| Time | Action | Result |
|---|---|---|
| 13:05:47–13:06:10 | `SWITCH_PULSE` argument in 50 µs PID cycles (1–1000); `vbus_seen_on` volatile; `bu regulator switch-pulse` takes ms (0.05–50) | — |
| 13:06:17 | `pytest` tools/bringup; `bu build` | 56 passed (0.50 s); 0 errors, 0 warnings. RAM 62,696 B, flash 150,632 B |
| 13:06:32 | Status, old build | pulses_skipped 164, sr_enable 0, i_cmd 366.57 mA, i_max 1854.15 mA |
| 13:06:38 | `bu flash` | ok, verified, reset |
| 13:06:54 | Status 4 s later | RUNNING, BUCK, target 5000, sr_enable 0, i_cmd 364.26 mA, i_max 1843.82 mA, v_in 24145 mV, v_out 4984 mV. Sink hard_resets 372 |
| 13:07:34 | `bu mem read regulator_cyc_pid --count 3` | Last 2053, max 2362, mean 2060 cycles (12.1 µs at 170 MHz) |
| 13:07:55 | Free-running trace (`trace-arm --trigger now`) | V_out 4915–5070 mV, I_cmd 354–384 mA, no skipping, at_limit 0. PID max with trace on: 2575 cycles (15.1 µs) |
| 13:08:25 | `switch_step.py --contract-mv 5000 --open-ms 1 --repeat 1` | Run 1 |
| 13:09:08 | Same, `--open-ms 3` and `5`, 2 s apart | Runs 2, 3 |
| 13:10:52 | Image saved with objcopy | `data/firmware/final_runs1-3_…bin` |
| 13:11:12 | Status; `bu sink status` | RUNNING; hard_resets 372 |

Runs 1–3 (series A) and runs 4–5 (series B, step 9), 5 V, SR off:

| Run | Image | T_cmd (ms) | T_unl (ms) | V_0 (V) | σ_pre (mV) | ΔV_os (V) | ΔV_us (V) | % of V_0 | t_min (ms) | t_rec(2 %) (ms) | Skip samples open / after | V_min (V) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | earlier build (whole PID cycles) | 0.950 | −0.40 | 5.000 | 17.9 | 0.036 | 0.085 | 1.7 | — | — | 0 / 0 | 4.915 |
| 2 | earlier build (whole PID cycles) | 2.950 | 1.60 | 4.999 | 17.8 | 0.347 | 1.222 | 24.4 | 0.40 | 10.65 | 18 / 2 | 3.777 |
| 3 | earlier build (whole PID cycles) | 4.950 | 3.60 | 5.000 | 16.5 | 0.363 | 1.241 | 24.8 | 0.70 | 9.85 | 60 / 5 | 3.759 |
| 4 | `da5a649`, TIM4 | 1.438 | 0.09 | 5.001 | 19.2 | 0.069 | 0.069 | 1.4 | — | — | 0 / 0 | 4.932 |
| 5 | `da5a649`, TIM4 | 3.001 | 1.65 | 5.001 | 16.0 | 0.328 | 1.224 | 24.5 | 0.45 | 8.05 | 22 / 0 | 3.777 |

| Run | I_cmd before (mA) | I_cmd 0.25 ms before reconnect (mA) | I_cmd peak after (mA) | V_out slope while open (mV/ms) | Time below vSrcNew (ms) | Time below vSrcValid (ms) | Railed samples |
|---|---|---|---|---|---|---|---|
| 1 | 367.5 | 366.6 | 370 | — | 0.00 | 0.00 | 0 |
| 2 | 368.1 | 359.2 | 604 | 98.0 | 1.40 | 0.60 | 0 |
| 3 | 368.1 | 357.8 | 562 | 14.3 | 1.60 | 0.70 | 0 |
| 4 | 367.9 | 367.0 | 374 | — | 0.00 | 0.00 | 0 |
| 5 | 368.1 | 353.2 | 606 | 89.8 | 1.45 | 0.60 | 0 |

| Observation | Value |
|---|---|
| T_release at 5 V | First sample > V_0 + 75 mV at 1.5 / 1.25 / 1.45 ms, first skip sample 1.7 / 1.65 / 1.65 ms (runs 2 / 3 / 5); ensemble 1.35 ms (R§6.1) |
| ΔV_us plateau | 1.222–1.241 V for T_unl 1.60–3.60 ms: independent of the off time |
| Recovery | Within 2 %: 8.05–10.65 ms; within 1 %: 74.25 / 84.9 / 89.1 ms (runs 2 / 3 / 5) |
| Command | I_cmd peak 562–606 mA, 1.53–1.65 × the value before; never railed (I_max ≈ 1.85 A) |
| PD comparison | V_min 3.76–3.78 V is below vSrcValid (4.25 V) for 0.6–0.7 ms (R§6.7) |

### Step 9. TIM4-timed `SWITCH_PULSE`; series B, C1–C3, D (17:07–17:14)

Objective: ΔV_us and recovery against T_unl at 5, 20 and 28 V (SR off), with the pulse timed by TIM4 instead of whole PID cycles.

```text
17:07:27  switch_step.py rewritten for the TIM4 pulse
17:07:52  bu flash; 5 s; bu regulator status -> RUNNING, 5000 mV, v_out 4967 mV
          TIM4 interrupt enabled (NVIC ISER0 bit 30), priority 1 (read at 0xE000E41E)
17:08:15  B : switch_step.py --contract-mv 5000 --open-ms 1.437,3 --out data/steps_tim4
17:08:37  bu sink req 20000 3000 --until contract
17:08:47  C1: --contract-mv 20000 --open-ms 1.0,1.2,1.3,1.4,1.5,1.6,1.8,2.0,2.5 --max-dip-pct 20
17:10:03  C2: --contract-mv 20000 --open-ms 2.5,2.75,3.0,3.5,4.0,5.0 --max-dip-pct 20
17:10:45  C3: --contract-mv 20000 --open-ms 4.25,4.5,4.75,5.0,5.5,6.0,7.0 --max-dip-pct 20
17:11:52  bu sink epr 100 --wait 3; bu sink req 28000 3000 --until contract --timeout 3
17:12:17  D : --contract-mv 28000 --open-ms 3.5,4.0,4.25,4.5,4.75,5.0,5.5,6.0 --max-dip-pct 15
17:13:54  bu sink req 5000 3000 --until contract --timeout 3; bu sink status -> hard_resets 372
```

`--max-dip-pct` stops a series when ΔV_us exceeds that percentage; it fired only on the last item of series B ("3 ms: dip -24.5 % is past --max-dip-pct 15").

| `switch_step.py` change | Time | Runs affected |
|---|---|---|
| Release detection band max(4 σ_pre, 30 mV) | 17:09:59 | Run-time summaries of C1 used the earlier band |
| Band must hold for three samples; `railed_samples` key | 17:14:14 | Run-time summaries of B, C, D used the earlier rule |
| Never overwrite an earlier run of the same open time | 17:14:32 | Traces of runs 14 and 20 had been overwritten by runs 15 and 24; their summary values are kept |

All tabulated metrics are recomputed from the saved traces by `scripts/pulse_metrics.py collect` (41 traces, 43 pulses), not taken from the run-time summaries.

Release lag (R§6.1):

<!-- BEGIN gen:release -->
| Configuration | Traces in ensemble | T_release, ensemble at +75 mV (ms) | Ensemble at +40 / +100 mV (ms) | Single traces at +75 mV (ms) | T_release used (ms) |
|---|---|---|---|---|---|
| 5 V, SR off | 3 | 1.35 | 1.15 / 1.45 | 1.35 (n = 3) | 1.35 |
| 20 V, SR off | 5 | 4.15 | 4.10 / 4.25 | 3.90 to 4.25 (n = 4); 1 crossing at 1.05 ms ignored (background excursion) | 4.15 |
| 28 V, SR off | 4 | 3.90 | 3.75 / 4.00 | 3.80 to 3.95 (n = 4) | 3.90 |
| 28 V, SR on | 8 | — | 3.40 / none | unresolved (σ_pre 61–80 mV) | 4.20 (TP2 scope) |
<!-- END gen:release -->

ΔV_us by T_unl (T_unl: ΔV_us, V):

<!-- BEGIN gen:matrix -->
| Nominal T_cmd (ms) | 20 V, SR off: T_unl (ms): ΔV_us (V) | 28 V, SR off: T_unl: ΔV_us | 28 V, SR on: T_unl: ΔV_us (each pulse) |
|---|---|---|---|
| 4.25 | — | 0.35: 0.34 | — |
| 4.50 | 0.35: 0.18 | 0.60: 0.37 | — |
| 4.75 | 0.60: 0.50 | 0.85: 0.56 | 0.55: 0.57, 0.88 |
| 5.00 | 0.85: 0.86, 0.77 | 1.10: 0.68 | 0.80: 0.91, 1.12 |
| 5.50 | 1.35: 1.49 | 1.60: 2.06 | 1.30: 1.68, 1.96 |
| 6.00 | 1.85: 1.92 | 2.10: 2.63 | 1.80: 2.82, 2.56 |
| 7.00 | 2.85: 3.55 | — | — |
<!-- END gen:matrix -->

| Series | Runs | Contract, SR | T_cmd (ms) | Pulses (sub-release + events) | ΔV_os (V) | ΔV_us (V) | ΔV_us (% of V_0) | Railed samples per trace |
|---|---|---|---|---|---|---|---|---|
| A | 1–3 | 5 V, SR off | 0.950 to 4.950 | 3 (1 + 2) | 0.04 to 0.36 | 0.09 to 1.24 | 1.7 to 24.8 | 0 to 0 |
| B | 4–5 | 5 V, SR off | 1.438 to 3.001 | 2 (1 + 1) | 0.07 to 0.33 | 0.07 to 1.22 | 1.4 to 24.5 | 0 to 0 |
| C1 | 6–14 | 20 V, SR off | 1.001 to 2.501 | 9 (9 + 0) | 0.00 to 0.16 | 0.07 to 0.24 | 0.4 to 1.2 | 0 to 0 |
| C2 | 15–20 | 20 V, SR off | 2.501 to 5.001 | 6 (5 + 1) | 0.03 to 0.26 | 0.08 to 0.86 | 0.4 to 4.3 | 0 to 0 |
| C3 | 21–27 | 20 V, SR off | 4.251 to 7.001 | 7 (1 + 6) | 0.06 to 0.29 | 0.09 to 3.55 | 0.5 to 17.7 | 0 to 0 |
| D | 28–35 | 28 V, SR off | 3.501 to 6.001 | 8 (2 + 6) | 0.08 to 0.35 | 0.11 to 2.63 | 0.4 to 9.4 | 0 to 9 |
| E | 36–43 | 28 V, SR on | 4.751 to 6.001 | 8 (0 + 8) | 0.27 to 0.48 | 0.57 to 2.82 | 2.0 to 10.1 | 0 to 1 |

Run 35 (28 V SR off, T_cmd 6.001 ms), largest railed count:

| t (ms) | 5.95 | 6.00 (reconnect) | 6.05 | 6.10 | 6.15 | 6.20 | 6.25 | 6.30 | 6.35 | 6.40 | 6.45 | 6.50 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| V_out (mV) | 28199 | 28147 | 26336 | 25370 | 25836 | 26146 | 26319 | 26629 | 26940 | 27181 | 27388 | 27612 |
| I_cmd (mA) | 1523 | 1502 | 1694 | 1755 | 1725 | 1706 | 1695 | 1676 | 1658 | 1644 | 1632 | 1603 |
| I_max (mA) | 1587 | 1590 | 1694 | 1755 | 1725 | 1706 | 1695 | 1676 | 1658 | 1644 | 1632 | 1620 |

Railed 6.05–6.45 ms (9 samples). V_out 27681 / 27923 / 27975 mV at 6.6 / 6.7 / 6.8 ms.

| Observation | Value |
|---|---|
| 20 V headroom | I_max rises as V_out falls in buck mode. Run 27 (T_cmd 7 ms): peak I_cmd 1708 mA with I_max 2222 mA at that sample; run 26 (6 ms): 1400 against 1928 mA. 0 railed samples at 20 V |
| 28 V SR off headroom | I_cmd 1530–1541 mA against I_max 1598 mA before the pulses (96 %); railed 0–9 samples per event trace |
| At-limit flag | Never set in any trace; railed is counted from I_cmd against I_max |
| VBUS read during a pulse | `pd_vbus_get_mv` returns V_out while the switch is open and `pd_vbus_poll` (10 ms) has not yet seen it; before da5a649 a query in that gap read 0 V |
| Stop or fault during a pulse | Cancels the pulse |
| After the event | rms(V_out − V_0)/σ_pre 0.8–1.3 from 10 ms on; I_cmd 10 ms on minus before −2 to +4 mA (R§6.5) |
| Faults | None in series A–D |

### Step 10. 28 V with SR on; series E and TP2 capture F (2026-10-18 09:06–09:16)

Objective: repeat series D with SR on (Dan approved) and capture one reconnect at TP2 with the scope to check the 50 µs trace minimum.

```text
09:07:02  bu regulator status; bu sink status -> hard_resets 372, epr_failures 1, contracts 23
09:07:06  bu sink req 28000 3000 --until contract --timeout 3; 1.5 s; status
            -> SR off: i_cmd 1554.78 mA, i_max 1597.23 mA, v_out 27957 mV, v_in 24043 mV
09:07:11  bu mem write regulator_sr_enable 1 --type u8; 2 s; three status reads 0.5 s apart
            -> i_cmd 1383 / 1421 / 1395 mA, i_max 1683 / 1679 / 1675 mA, v_out 27923 / 27871 / 27854 mV
09:07:20  E : switch_step.py --contract-mv 28000 --open-ms 4.75,5,5.5,6 --repeat 2 --max-dip-pct 15
              --out data/steps_tim4_sr                                  (all eight ended 'running')
09:08:10  scope state saved -> data/scope_28v/scope_user_state.json
09:08:17  scope_step.py --contract-mv 28000 --open-ms 6     (scope did not trigger; trace kept)
09:09:45  scope limit tests (offset and trigger-level clamps, §1.2)
09:13:37  scope_step.py defaults: 2.5 V/div, 1 ms/div, trigger 7.5 div from the left, CH2/CH3 hidden
09:13:41  F : scope_step.py --contract-mv 28000 --open-ms 6 --out data/scope_28v
09:14:34  scope_step.py --restore data/scope_28v/scope_user_state.json; compared field by field
09:15:57  bu mem write regulator_sr_enable 0 --type u8; bu sink req 5000 3000; status -> sr 0
```

SR on against SR off at 28 V (pairs at equal T_cmd):

<!-- BEGIN gen:rectifier -->
| Nominal T_cmd (ms) | ΔV_us, SR off (V) | ΔV_us, SR on, mean of 2 (V) | On − off (V) | SR-on repeat difference (V) | Railed samples, SR off | Railed samples, SR on (each pulse) |
|---|---|---|---|---|---|---|
| 4.75 | 0.56 | 0.72 | +0.16 | 0.32 | 1 | 0 / 0 |
| 5.00 | 0.68 | 1.02 | +0.34 | 0.21 | 3 | 0 / 0 |
| 5.50 | 2.06 | 1.82 | −0.24 | 0.28 | 7 | 0 / 0 |
| 6.00 | 2.63 | 2.69 | +0.06 | 0.26 | 9 | 1 / 1 |

Pooled sd of the SR-on repeats 0.19 V (four pairs). σ_pre: SR off 31 mV (n = 8, 29–35); SR on 72 mV (n = 8, 61–80); ratio 2.3.
<!-- END gen:rectifier -->

TP2 capture F (`metrics/tp2_28v_6ms.json`, `figures/tp2_28v_6ms.png`). Record: 1,200,000 points at 20 ns, −13.5 to +10.5 ms about the trigger, 85 codes, quantum 0.103 V, 22.542–31.394 V. Baseline (−12 to −7 ms): 28.054 V, raw sd 0.628 V, raw p-p 6.485 V; trace V_0 27.993 V.

| Averaging window (µs) | Minimum (V) | At (µs after trigger) | ΔV_us (V) | % of V_0 |
|---|---|---|---|---|
| 1 | 25.041 | 26.1 | 3.014 | 10.7 |
| 2.5 | 25.154 | 26.0 | 2.901 | 10.3 |
| 5 | 25.382 | 19.5 | 2.672 | 9.5 |
| 10 | 25.391 | 21.8 | 2.664 | 9.5 |
| 20 | 25.425 | 26.7 | 2.629 | 9.4 |
| 50 | 25.553 | 32.6 | 2.502 | 8.9 |

| Quantity | Value |
|---|---|
| Recovery, 5 µs mean | Within ±2 % (0.561 V) 0.211 ms, within ±1 % (0.281 V) 0.261 ms after the trigger |
| Release seen at TP2 | p-p of the 5 µs mean in 0.1 ms windows: quiet (−13 to −3 ms) median 0.065 V, p90 0.119 V, max 0.551 V (n = 100). Rise starts 1.75 ± 0.1 ms before the trigger, i.e. T_release ≈ 4.2 ms of the 6.0 ms pulse |
| Regulator trace of the same pulse | V_0 27993 mV, σ_pre 66.4 mV; pulse 6001.38 µs; reconnect sample 6.0 ms; ΔV_os 0.378 V; ΔV_us 2.502 V (8.9 %); 1 railed sample; I_cmd max 1843 mA; skip sample at 5.9 ms |
| Trace V_out 5.95–6.30 ms | 27837, 28251, 26509, 25491, 26371, 26923, 27423, 27681 mV |
| Trace against scope | Trace ΔV_us 2.502 V = 50 µs boxcar of the scope record (2.502 V). The 1 µs boxcar reads 3.014 V: the trace under-reads the fast minimum by 0.51 V (17 %) |
| First attempt, 09:08:17 (scope untriggered) | `step_28v_6ms_0.csv`: V_0 28005 mV, σ_pre 60.3 mV, ΔV_os 0.315 V, ΔV_us 2.548 V (9.1 %) at 0.1 ms, within 2 % at 0.3 ms, 1 railed sample |
| Faults | None in series E or F |

## 4. Statements withdrawn or corrected from the first version of this log

| First version | Status | Reason | Now |
|---|---|---|---|
| Setup: 25.3 Ω and QT Py "on TP2" | Corrected | Step 1 | After Q5/Q6 |
| Step 2: ≈ 0.9 V p-p ripple; 1.55 V p-p, 198 mV rms; 485 dips below −0.25 V; band table with 321 Hz 168 mV pk | Withdrawn | Clipped records; 8 µs point samples alias the 5 µs ripple (step 2 validity table) | Step 2 boxcar table; ADC-trace excursions and spectra, R§6.2 (no line above the local median at 24.6 or 321 Hz) |
| Step 3: "the same spectrum as in step 2 (321 Hz at 163 mV pk)" | Withdrawn | Same | 20 ms block means, step 3 |
| Step 3: QT Py I_out −0.09 to +0.07 A | Corrected | Recomputed from `load_run_3w.json` | −93 to +82 mA |
| Step 2: 75 skip transitions/s stated as a rate | Restated | One counter reading; traces show none | Step 2 skip-counter line; R§10.1 U11 |
| Step 4: ramp 88 V/s, 97 mV rms about the line | Withdrawn | One ad hoc window | 83.4–90.0 V/s by fit window |
| Step 6 reset: 99.6 kHz 238 mV pk; 0.34 V p-p over 50 µs; 13 mV at 507 Hz | Withdrawn | Window reached into the collapse | 99,730 Hz 275.4 mV pk; 50 µs p-p 0.049 V; 14.1 mV at 541 Hz |
| Step 6 connect: fall up to 0.14 V/µs, "about 6 A out of the board's 44 µF" | Withdrawn | Not reproduced; slope depends on span | 0.0047–0.026 V/µs by definition; arithmetic 0.21–1.14 A |
| Step 6 fits: 413 µF from 3.2 V with g = 0.72 | Withdrawn | Not reproducible | Fits table, step 6 |
| Step 8: back within 2 % in 10.7 / 9.9 ms | Corrected | Rounding | 10.65 / 9.85 ms |
| Step 8: 110 µF ‖ 25 Ω drain argument | Withdrawn | Reasoning on an unmeasured load side | R§10 hypotheses H1–H3 and tests T1–T3 |
| Step 8: within 1 % took 74–85 ms | Completed | Run 5 missing | 74.25 / 84.9 / 89.1 ms (runs 2 / 3 / 5) |
| Step 9 header 13:20–13:35 | Corrected | Wrong times | 17:07–17:14 |
| Step 9: "the sink's hard-reset count stayed 372" | Withdrawn | Not read at 11:44 nor after series E/F | §5 item 7 |
| Step 9: Q5/Q6 let go 4.1–4.4 ms at 20 and 28 V | Replaced | Single-trace readings | Ensemble T_release 4.15 / 3.90 / 4.20 ms (R§6.1) |
| Step 9: 20 V command 1708 mA "against about 1.9 A" | Corrected | I_max rises as V_out falls | 1708 against 2222 mA at that sample |
| Step 9: dip "reads as the 25 Ω and the QT Py input coming back onto the output capacitance" | Withdrawn as a conclusion | Load side not probed | R§10 hypotheses |
| Step 10 header 09:00–09:15 | Corrected | Times | 09:06–09:16 |

## 5. Deviations and open items

| # | Item | Effect | Next |
|---|---|---|---|
| 1 | No load step: QT Py buck not converting | No calibrated ΔI; output impedance not measured | Firmware current-injection step (requirement F2) |
| 2 | Load side, Q5/Q6 gates and load current not probed | Cause of ΔV_us not separated (R§10 H1–H3) | F1 |
| 3 | All voltages at TP2 / VD, ahead of R6 and Q5/Q6 | PD comparisons are indicative; PD asserts at the receptacle | F3 |
| 4 | Runs 1–3 on an uncommitted build | Identity rests on the saved binary | F5: log HEAD, dirty flag, ELF hash per run |
| 5 | Traces of runs 14 and 20 overwritten | Their rows carry run-time summary values | Fixed 17:14:32 |
| 6 | `switch_step.py` edited between series | Run-time summaries differ in rule; tabulated values recomputed from traces | — |
| 7 | Sink hard_resets 369 (11:08:12) → 372 (13:06:54); not read at 11:44 nor after series E/F | Three Hard Resets in that interval, cause not established | Read the sink and source counters before and after every reset or pulse series |
| 8 | Scope ramp slope 4–13 % above the 79.9 V/s slew | Unresolved | Log the setpoint with the trace during a ramp |
| 9 | Skip counter 75/s against 0 flag transitions in traces | Unresolved (R§10.1 U11) | Trace the skip counter |
| 10 | Half-switching-frequency lines at 5 V, one record | D5 Fail at 5 V, not tested at 20/28 V | Repeat with a ground-spring tip at 5/20/28 V, SR on and off |
| 11 | V_in not logged per event | Source term in ΔV_us estimated only (R§10.2) | F4 |
| 12 | Probe model and ground lead not recorded | Ripple amplitudes carry an unknown probe term | Record with every capture |

## 6. Scope records

| File in `data/cap/` | Step | Scope settings | Points | Interval | Span | Codes used / quantum | Lowest to highest (V) | Samples at lowest / highest code |
|---|---|---|---|---|---|---|---|---|
| `vbus28_ac_12ms.csv` | 2 | AC, 0.2 V/div, 20 MHz limit, 1 ms/div, offset 4 ms; single, CH4 rising 0.15 V, HF reject | 300,000 | 0.04 µs | 12 ms | 256 / 11.34 mV | −1.0096 to 1.8831 | 16,245 / 2,497 (5.42 % / 0.83 %) |
| `vbus28_ac_2s4_unarmed.csv` | 2 | AC, 0.2 V/div, 20 MHz limit, 0.2 s/div; auto, CH4 rising 0.9 V, HF reject | 300,000 | 8 µs | 2399.99 ms | 256 / 11.34 mV | −1.0096 to 1.8831 | 16,092 / 2,420 (5.36 % / 0.81 %) |
| `vbus28_ac_2s4_loadrun.csv` | 3 | as the previous record; run during and after `bu load run` | 300,000 | 8 µs | 2399.99 ms | 256 / 11.34 mV | −1.0096 to 1.8831 | 15,990 / 2,379 (5.33 % / 0.79 %) |
| `vbus_20to28_ramp.csv` | 4 | DC, 2 V/div at offset −24 V, 20 MHz limit, 20 ms/div at offset 80 ms; single, CH4 rising 24 V, HF reject | 300,000 | 0.8 µs | 240 ms | 181 / 92 mV | 14.069 to 31.172 | 1 / 1 (0 % / 0 %) |
| `reset_from5v_raw.csv` | 6 | DC, 0.5 V/div at offset −4.5 V, 20 MHz limit, 2 ms/div at offset 8 ms; single, CH4 falling 4.6 V, HF reject | 300,000 | 0.08 µs | 24 ms | 256 / 22.7 mV | 1.9501 to 7.7324 | 96 / 269 (0.03 % / 0.09 %) |
| `connect5_raw.csv` | 6 | DC, 1 V/div at offset −3 V, 20 MHz limit, 2 ms/div at offset 10 ms; single, CH4 rising 4.0 V, HF reject | 300,000 | 0.08 µs | 24 ms | 182 / 46 mV | −0.2299 to 8.0919 | 3 / 2 (0 % / 0 %) |
| `tp2_28v_6ms.csv` (in `data/scope_28v/`) | 10 | DC, 2.5 V/div, 20 MHz limit, high resolution, 50 MSa/s, 1 ms/div, trigger 7.5 div from the left; single, CH4 falling 26.8 V, HF reject | 1,200,000 | 0.02 µs | 24 ms | 85 / 0.103 V | 22.542 to 31.394 | — |

## 7. Files

| Path | Content |
|---|---|
| `report/TEST_REPORT.md` | Test report: definitions, configuration, results, scorecard, uncertainty, limitations |
| `report/pulse_metrics.csv` | One row per pulse (43 × 34 columns), from `pulse_metrics.py collect` |
| `report/undershoot_summary.png` | ΔV_us against T_unl, all series |
| `metrics/vbus.json` | Steps 2, 4, 6: record checks, boxcar statistics, ramp, reset, connect; withdrawn step 2 statistics kept under `first_analysis_retracted` |
| `metrics/connect_fit.json`, `connect_fit_free.json`, `connect_fit_qtpy.json` | Step 6 fits |
| `metrics/tp2_28v_6ms.json` | Step 10 TP2 capture |
| `figures/connect5_fit*.png`, `figures/tp2_28v_6ms.png` | Fit overlays; TP2 capture with the trace |
| `scripts/switch_step.py` | Pulse runner: contract, trace arm, `SWITCH_PULSE`, trace read, per-run summary |
| `scripts/scope_step.py` | Same with a scope capture on TP2; saves and restores the scope state |
| `scripts/pulse_metrics.py` | `collect` (CSV from traces), `tables`, `update FILE`, `check FILE` for the generated tables in this log and the report |
| `scripts/vbus.py`, `tp2_metrics.py`, `connect_fit.py` | Builders of the `metrics/` files |
| `scripts/plot_tp2.py`, `plot_undershoot.py` | Figures |
| `data/cap/` | Scope records of steps 2–6 (§6) and screenshots |
| `data/steps/`, `data/steps_tim4/`, `data/steps_tim4_sr/`, `data/scope_28v/` | Traces (`step_*.csv`) and run summaries (`summary_*.json`) of series A, B–D, E, F |
| `data/load_run_3w.json` | Step 3 QT Py telemetry |
| `data/firmware/` | Runs 1–3 image |
