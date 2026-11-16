# Bench log 2026-10-19: 28 V switch-node waveforms at 0, 3 and 25 W extra load; SW_OVP after the buck repair

| Field | Entry |
|---|---|
| Date | 2026-10-19, 21:19–21:36 EDT (UTC−4), bench actions |
| Operator | Dan: repaired the QT Py buck's phase and DISABLE wires, approvals. Claude Code: commands through `tools/bringup` (`bu`) and `scripts/` |
| DUT | PD Charger, STM32G474 four-switch buck-boost, 200 kHz, DCM current-command modulator; EPC2308 GaN; L1 IHLP6767GZER4R7M11 (4.7 µH) |
| Requested | Load-step test at 28 V after the buck repair; waveform data for power-loss analysis and conducted emissions; then a 5 s run at 25 W on the QT Py buck (≈ 56 W total) with raised current limits |
| Executed | Steps 1–6. The QT Py cannot apply a step (§3, step 2), so the data are steady-state switch-node records at four operating points |
| Hardware changes | QT Py buck phase and DISABLE wires repaired by Dan before the session |
| Firmware changes | Step 5 only, reverted in step 6 (§1.3) |
| Data | Committed: `scripts/`, `metrics/`, `figures/`. Git-ignored on the bench machine: `data/` |
| Status | Complete. Not a conducted-emission measurement and not a measured efficiency (§5) |

Terms (bench log 2026-10-17, `../2026-10-17/README.md`): SR = synchronous rectifier (`regulator_sr_enable`); I_cmd = PID output, the commanded charge per period as a current; I_max = its ceiling; railed = I_cmd ≥ I_max − 2 mA. Here: SW1 = TP3, the Q1/Q2 node (input leg); SW2 = TP4, the Q3/Q4 node (output leg); t1, t2 and ring as defined under "Per-period structure" (§4).

## 1. Configuration

### 1.1 Hardware and load

| Item | Configuration |
|---|---|
| Source | 1500 W server supplies, regulator V_in 23.7–24.0 V |
| Load on VBUS (after Q5/Q6) | 25.3 Ω and the QT Py buck input; QT Py buck output into its 10 Ω / 10 W ballast |
| PD | Sink board (`bu sink`), 28 V EPR contract, 3 A |

### 1.2 Instruments

| Instrument | Settings |
|---|---|
| Rigol DS1104Z, 8-bit, 100 MHz | CH2 = SW1, CH3 = SW2: DC, 10 V/div, offset −20 V, probe setting 10×, bandwidth limit off. CH1, CH4 off. 100 µs/div, normal acquisition, memory depth auto: 600,000 points at 500 MSa/s (2 ns, 1.2 ms). Trigger CH2 rising 12 V, single, DC coupling. Probe models and ground leads not recorded |
| Scope state | Saved before step 3 (`data/scope_user_state.json`), restored after step 6, no field differs |
| Regulator status | `bu regulator status`: single PID-cycle values |
| Regulator trace | 20 kHz, 2048 samples (102 ms), `trace-arm --trigger now --post 2047` |
| QT Py telemetry | `bu load run` frames every 50 ms |

### 1.3 Firmware

| Steps | G474 | QT Py buck (`bench/buck-load-qtpy` 0.2.0, power stage) |
|---|---|---|
| 1–4 | da5a649 build flashed 2026-10-17 17:07:52 (ELF sha256 `eb49b8f6…`) | `Params::BENCH_10R_10W`: ballast 7 W max, output trip 1.5 A, setpoint rise 5 mV/ms (as found) |
| 5 | da5a649 + `DCM_MAX_PEAK_MA_BB` 10000 → 12300 (ELF sha256 `cec033da…`). The comparator backstop is unchanged at its 12.5 A cap (150 % of 12.3 A is capped by `PEAK_PROTECT_MAX_MA`) | `Params::BENCH_10R_OVERLOAD_25W`: 25 W, trip 2.0 A, rise 20 mV/ms (2.5 × the ballast rating; approved by Dan for 5 s) |
| 6 → end | Rebuilt from da5a649, flashed: ELF sha256 `eb49b8f6…`, identical to before | Back to `BENCH_10R_10W`. The 25 W set stays defined in `bench/load-control/src/lib.rs`, unused |

## 2. Step index

| Step | Time (EDT) | Activity | Outcome |
|---|---|---|---|
| 1 | 21:19–21:20 | Initial state; SW_OVP investigation; clear-fault | Fault from a VD_MON disturbance, not an output over-voltage; cleared, 5 V stable |
| 2 | 21:21 | QT Py buck check, 0.5 W at 28 V | Buck converts after the repair |
| 3 | 21:22–21:29 | SW1/SW2 records at 28 V: SR on 0 W, SR on 3 W, SR off 0 W | `data/sw_28v_sr1_0w.csv`, `_sr1_3w.csv`, `_sr0_0w.csv` |
| 4 | 21:29–21:33 | Firmware limits for 25 W: edits, tests, builds (G474 21:32:00), flashes | Ceiling ×1.48 |
| 5 | 21:33 | 25 W on the QT Py for 6 s at 28 V, SR on | ≈ 56 W held without fault; I_cmd at 95 % of the raised ceiling |
| 6 | 21:34–21:36 | Both firmwares reverted (G474 build 21:34:11); scope restored; 5 V, SR off | End state: RUNNING 5 V, QT Py off |

## 3. Steps

### Step 1. Initial state and SW_OVP (21:19–21:20)

| Item | Reading |
|---|---|
| Regulator | FAULT, `SW_OVP`, 148 s old; G474 uptime 665 s, fault at 517 s (`regulator_fault_tick_ms` 517057) |
| Fault snapshot | V_out 11262 mV (filtered), V_in 24145 mV, VD_MON 596 counts, estimate 653 counts, setpoint 5000 mV, skip events 1977 |
| Fault trace (last 64 VD_MON scans, one per 5 µs period) | 46 scans at 275–302 counts (4.7–5.2 V), then 18 scans jumping between 252 and 966 counts (4.3–16.7 V) from one scan to the next |
| PD source since boot | 144 attaches, 143 Hard Resets, 2 contracts, 284 VBUS-on failures (the latched fault blocks VBUS, ≈ 1 retry/s since the trip) |
| Sink | First `status` reply "unknown command" (line garbled in the event stream); retry: attached CC1, hard_resets 895, `tcpp_fault 1`, `tcpp_ok 0` |
| QT Py | off, no fault; VIN tap 2.6 V, OUT 0.12 V |

Assessment: a 12 V rise within one 5 µs scan on ≥ 44 µF needs about 100 A; the converter supplied about 0.4 A. The excursion is a disturbance on the VD_MON sense or its ground, not an output over-voltage. It occurred after reconnection (Dan: everything was unplugged during the repair); the event that caused it is not identified. The OVP acted as designed on the filtered value.

Action: `bu regulator clear-fault` (result OK). 20 s at 5 V: RUNNING, V_out 4.93–5.02 V, I_cmd 364–367 mA (2026-10-17: 368 mA), so the repaired buck adds no idle draw. `tcpp_fault 1` on the sink has no earlier reading to compare (§6).

### Step 2. QT Py buck check, 0.5 W (21:21)

`bu load run --p 500 --seconds 3 --limit-ma 3000 --contract-mv 28000 --vin-min-mv 24000`, 28 V, SR on. Running frames from 0.5 s (52): buck V_out 2.10–2.33 V (target 2.24 V), 441–541 mW, duty 86–90 ‰; no fault on either board.

The QT Py sets power through its output voltage with the setpoint rising at 5 mV/ms (`load-control` `slew_up_mv_per_ms`): 3 W (5.5 V) takes ≈ 1.1 s. It cannot apply a load step; only disarm (DISABLE high) removes its load at once. The step-response test was replaced by steady-state switch-node records.

### Step 3. Switch-node records at 28 V (21:22–21:29)

| Record | Condition | Timing |
|---|---|---|
| `sw_28v_sr1_0w.csv` | SR on, QT Py off | 21:22:36 |
| `sw_28v_sr1_3w.csv` | SR on, QT Py 3 W (run B, 21:23:35) | Acquisition armed 2.9 s into the run, frozen before 5.7 s; buck 5.28–5.74 V, 2.79–3.29 W in that window (57 frames) |
| `sw_28v_sr1_3w_late.csv` | Run A (21:22:57) | Acquisition time not established; not used |
| `sw_28v_sr0_0w.csv` | SR off, QT Py off | 21:28:47 |

Status during each condition (10 reads; run A for 3 W):

<!-- BEGIN gen:status -->
| Condition | Reads | I_cmd min / mean / max (mA) | I_max (mA) | I_cmd / I_max (mean) | V_out mean (mV) | V_in mean (mV) |
|---|---|---|---|---|---|---|
| SR on, 25.3 Ω only | 10 | 1353 / 1384 / 1418 | 1657–1700 | 82 % | 27840 | 23939 |
| SR on, + buck 3 W (run A) | 10 | 1474 / 1487 / 1496 | 1677–1681 | 89 % | 27921 | 24012 |
| SR off, 25.3 Ω only | 10 | 1474 / 1481 / 1487 | 1595–1599 | 93 % | 27920 | 24004 |
<!-- END gen:status -->

### Step 4. Limits for 25 W

| Limit | Value before | Effect at 28 V | Change |
|---|---|---|---|
| G474 `DCM_MAX_PEAK_MA_BB` | 10 A | Binding: the code takes the smaller of this cap and the window limit (≈ 12.2 A); I_max ≈ 1680 mA with SR on | 12.3 A, so the timing window binds: (5000 − 250 − 300) ns × 28/52 = 2.40 µs → 12.2 A from 24 V |
| G474 peak comparator (COMP1 backstop) | 12.5 A (`PEAK_PROTECT_MAX_MA`; IL_MON full scale 13.2 A) | — | None |
| QT Py ballast power | 7 W | Clamps P* | 25 W |
| QT Py output trip | 1.5 A | 25 W into 10 Ω is 1.58 A | 2.0 A |
| QT Py setpoint rise | 5 mV/ms | 15.8 V in 3.2 s | 20 mV/ms (0.8 s) |

Checks: `load-control` unit tests 9 passed; G474 `bu build` 0 errors, 0 warnings. QT Py reflashed with the regulator stopped (VIN tap 3.26 V, OUT 0.13 V, under the BOOTSEL limits 4.5 V / 0.3 V). After the G474 flash at 28 V: I_max 2421 mA (SR off), 2491–2496 mA (SR on), ×1.49 as predicted by (12.2/10)².

### Step 5. 25 W on the QT Py for 6 s, 28 V, SR on (21:33:11)

```text
bu load run --p 25000 --seconds 6 --limit-ma 3000 --contract-mv 28000 --vin-min-mv 24000
  ~2.2 s: bu regulator trace-arm --trigger now --post 2047; scope single (SW1/SW2)
```

| Quantity | Value |
|---|---|
| QT Py, 1.0 to 6.15 s (102 frames) | Output 15.2–16.5 V, 23.2–27.4 W, duty 594–638 ‰, VIN tap 25.1–26.9 V; no fault |
| Total load | 25.3 Ω ≈ 31 W + QT Py input ≈ 25 W / buck efficiency: ≈ 56 W out of the converter (QT Py input not measured) |
| Regulator trace (102 ms in the hold) | V_out mean 27.98 V, sd 249 mV, 27.13–28.41 V; I_cmd mean 2389 mA (2309–2509) against I_max 2454–2522 mA (95 %); 82 of 2048 samples railed; 28 skip samples; on-time 2307–2397 ns |
| Status reads | Six reads; only the first fell inside the hold (I_cmd 2072, I_max 2510 mA); the others followed the end of the run |
| Faults | None (regulator RUNNING throughout, QT Py `none`) |
| Scope record | `sw_28v_sr1_25w.csv`, acquired inside the hold |

With the 25.3 Ω only, σ_pre was 61–80 mV (2026-10-18, 28 V, SR on); at 56 W the sd is 249 mV with 4 % of samples railed: the loop is at its ceiling for part of the time.

### Step 6. Reversal (21:34–21:36)

SR 0; 5 V contract; regulator stop; QT Py reflashed with `BENCH_10R_10W`; G474 rebuilt from da5a649 and flashed (ELF identical, `eb49b8f6…`). End state: RUNNING 5 V, SR off, QT Py off without fault; scope restored.

## 4. Results

### Per-period structure (`scripts/sw_periods.py`, `metrics/sw_periods.json`)

Each 5 µs period in buck-boost: t1 charge (Q1 and Q4 on: SW1 ≈ V_in, SW2 low; L charges from V_in), t2 transfer (Q2 and Q3, or their reverse conduction with SR off: SW1 low, SW2 ≈ V_out; L discharges into V_out), then DCM: both nodes ring on L and the node capacitance until the next period starts with a hard turn-on of Q1 and Q4.

<!-- BEGIN gen:periods -->
| Condition | Periods | t1 charge (µs) | t2 transfer (µs) | Ring (µs) | I_pk at 4.7 µH (A) | I_L rms (A) | ½·L·I_pk²·f (W) | SW1 before Q1 on (V) | SW2 plateau (V) | SW2 peak at Q4 off (V) | Ring f (MHz) | C_eff (pF) | Ring amplitude, first half-cycle (V) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SR on, 25.3 Ω only | 231 | 1.79 | 1.53 | 1.68 | 9.15 | 4.30 | 39.3 | 0.8 | 31.4 | 39.9 | 2.43 | 914.8 | 26.2 |
| SR on, + buck 3 W | 232 | 1.94 | 1.64 | 1.42 | 9.92 | 4.85 | 46.3 | 1.8 | 31.4 | 41.1 | 2.38 | 950.7 | 25.7 |
| SR on, + buck 25 W, BB peak limit 12.3 A | 232 | 2.35 | 1.97 | 0.67 | 11.95 | 6.42 | 67.1 | 1.2 | 31.4 | 42.4 | — (ring < 2 periods) | — | — |
| SR off, 25.3 Ω only | 239 | 1.88 | 1.32 | 1.80 | 9.59 | 4.43 | 43.2 | 2.0 | 33.4 | 41.1 | 2.59 | 803.0 | 5.8 |

Medians over the record (1.2 ms). V_in 24.0 V (23.85 V at 25 W, regulator ADC) for I_pk = V_in·t1/L. Scope voltages uncorrected (§5, U2).
<!-- END gen:periods -->

### Edges (`scripts/sw_nodes.py`, `metrics/sw_nodes.json`)

<!-- BEGIN gen:edges -->
| Condition | Node | Hard rises / falls | Rise 10–90 % (ns) | Rise peak dV/dt (V/ns) | Overshoot above p95 level (V) | Fall 10–90 % (ns) | Fall peak dV/dt (V/ns) | Undershoot below p5 level (V) | Min / max (V) |
|---|---|---|---|---|---|---|---|---|---|
| SR on, 25.3 Ω only | SW1 | 233 / 440 | 39.3 | 6.3 | 3.7 | 5.3 | 5.7 | 7.7 | −9.759 / 29.278 |
| SR on, 25.3 Ω only | SW2 | 232 / 2 | 5.4 | 6.3 | 7.7 | 21.3 | 1.6 | 4.9 | −8.963 / 40.742 |
| SR on, + buck 3 W | SW1 | 235 / 435 | 44.6 | 4.9 | 2.8 | 4.9 | 5.3 | 7.7 | −9.353 / 29.278 |
| SR on, + buck 3 W | SW2 | 233 / 1 | 5.4 | 6.9 | 9.0 | 22.1 | 1.6 | 4.5 | −9.371 / 41.964 |
| SR on, + buck 25 W, BB peak limit 12.3 A | SW1 | 233 / 438 | 45.0 | 4.9 | 2.4 | 4.8 | 6.1 | 7.7 | −10.166 / 29.278 |
| SR on, + buck 25 W, BB peak limit 12.3 A | SW2 | 233 / 0 | 5.3 | 7.5 | 9.8 | — | — | — | −9.371 / 43.187 |
| SR off, 25.3 Ω only | SW1 | 479 / 480 | 53.0 | 3.3 | 2.0 | 4.7 | 6.3 | 3.9 | −8.54 / 29.685 |
| SR off, 25.3 Ω only | SW2 | 240 / 206 | 5.3 | 6.9 | 7.7 | 109.6 | 1.6 | 4.1 | −6.519 / 42.372 |

Medians per edge. Hard edge: peak dV/dt ≥ 1.5 V/ns (2 ns samples); slower crossings are the L-C ring and are excluded. 10–90 % is between the record's p5 and p95 levels, so SW1 rises that start from the ring or end in a slow approach read 40–53 ns despite 3–6 V/ns peaks. Edges near 5 ns are at the 100 MHz scope bandwidth (rise ≈ 3.5 ns); overshoot and dV/dt are lower bounds. 8-bit at 10 V/div: 0.31 V per code.
<!-- END gen:edges -->

### Spectra (`figures/sw_28v_spectrum.png`)

<!-- BEGIN gen:spectra -->
| Condition | Node | 0.15-0.5 MHz peak (dBµV) | 0.5-5 MHz peak (dBµV) | 5-30 MHz peak (dBµV) | 30-100 MHz peak (dBµV) |
|---|---|---|---|---|---|
| SR on, 25.3 Ω only | SW1 | 139.8 at 0.20 MHz | 131.6 at 2.40 MHz | 116.7 at 5.40 MHz | 97.4 at 36.80 MHz |
| SR on, 25.3 Ω only | SW2 | 143.4 at 0.20 MHz | 131.1 at 2.40 MHz | 112.1 at 5.60 MHz | 92.6 at 44.60 MHz |
| SR on, + buck 3 W | SW1 | 140.7 at 0.20 MHz | 131.4 at 2.20 MHz | 113.9 at 5.40 MHz | 92.3 at 33.40 MHz |
| SR on, + buck 3 W | SW2 | 144.0 at 0.20 MHz | 132.7 at 2.20 MHz | 112.2 at 5.20 MHz | 91.9 at 30.99 MHz |
| SR on, + buck 25 W, BB peak limit 12.3 A | SW1 | 142.9 at 0.20 MHz | 130.1 at 1.40 MHz | 114.4 at 5.80 MHz | 94.8 at 37.20 MHz |
| SR on, + buck 25 W, BB peak limit 12.3 A | SW2 | 145.4 at 0.20 MHz | 132.2 at 0.60 MHz | 111.8 at 6.00 MHz | 92.1 at 31.60 MHz |
| SR off, 25.3 Ω only | SW1 | 140.5 at 0.40 MHz | 132.9 at 1.00 MHz | 120.5 at 5.60 MHz | 103.8 at 32.80 MHz |
| SR off, 25.3 Ω only | SW2 | 145.5 at 0.20 MHz | 133.6 at 0.60 MHz | 114.6 at 5.20 MHz | 98.6 at 32.20 MHz |

Hann window over the whole 1.2 ms record, peak amplitude per 833.3 Hz bin, 1 µV reference. Switch-node voltage: the noise source, not a conducted-emission measurement (no LISN).
<!-- END gen:spectra -->

Figures: `figures/sw_28v_periods.png` (three periods per condition), `figures/sw_28v_spectrum.png` (SW1 and SW2, 0 W against 25 W, SR on).

### Loss mechanisms visible in the waveforms (arithmetic, not measured power)

| Mechanism | Basis | SR on, 0 W | SR on, 25 W | SR off, 0 W |
|---|---|---|---|---|
| Inductor-path conduction | I_L rms² × 22.6 mΩ (DCR hot 5.0, R5 5, two EPC2308 at 6.3 mΩ hot; `plans/scripts/loss_budget.py`) | 0.42 W | 0.93 W | 0.44 W |
| Energy through L per period | ½·L·I_pk²·f at 4.7 µH (table above) | 39.3 W | 67.1 W | 43.2 W |
| SR off − SR on, same load | Difference of the line above | — | — | +3.9 W (+3.5 W at an effective 5.2 µH, U1) |
| Reverse conduction in t2 with SR off | Q2 and Q3 at 2 V + 25 mV/A, triangle from I_pk over t2 | — | — | 5.5 W (model) |
| Q1 hard turn-on | SW1 0.8–2.0 V before turn-on (no ZVS): Q_oss(24 V) × V_in ≈ 23 nC × 24 V | 0.11 W | 0.11 W | 0.11 W |
| DCM ring | ½·C_eff·A² per period (A = first half-cycle) | 0.06 W | — (ring < 2 periods) | < 0.01 W |

The SR saves ≈ 3.5–3.9 W at this operating point (≈ 34 W out). Conduction, Q1 turn-on and the ring account for ≈ 0.6 W with SR on; the 2026-10-16 meter readings (SR off: 42.7 W in, 33.2 W out, 9.5 W loss) leave most of the loss unattributed. Inductor core loss at 9–12 A peak ripple, the input path (ADM1270, R25), Q5/Q6 and the controller supply are not estimated.

## 5. Uncertainty and limitations

| ID | Item | Effect |
|---|---|---|
| U1 | L tolerance ±20 % and its drop at 9–12 A | I_pk, I_L rms and ½·L·I_pk²·f scale with 1/L or 1/√L. Charge to the output from the timing gives 1.32 A at 0 W against ≈ 1.19 A expected (27.84 V / 25.3 Ω + 0.09 A QT Py), consistent with an effective L ≈ 5.2 µH or a lower switch voltage |
| U2 | Probe calibration not checked | SW1 reads 25.2 V (p95) with Q1 on against V_in 23.9–24.0 V (+1.2 V). SW2 plateau with SR on reads 31.4 V against V_out 27.8–27.9 V (+3.5 V): either Q3 still conducts in reverse with SR on or the probes differ; open |
| U3 | 100 MHz bandwidth, 8-bit at 10 V/div | Edges near 5 ns are at the bandwidth limit; 0.31 V per code |
| U4 | Regulator ADC currents | Single PID-cycle samples: I_in 2.52 A at 0 W gives 60 W in against ≈ 33 W out. Not usable for power |
| U5 | Not a conducted-emission measurement | Switch-node voltage spectra only; no LISN, no input-current probe |
| U6 | One 1.2 ms record per condition | Medians over 231–239 periods; no repeat across time or temperature |

## 6. Open items

| # | Item | Next |
|---|---|---|
| 1 | No measured efficiency | DC meters or current probes on the 24 V input and on VBUS at 0, 3, 25 W, SR on and off |
| 2 | Conducted emissions | LISN on the 24 V input and a spectrum analyzer (or a current probe on the input cable) |
| 3 | SW2 plateau 3.5 V above V_out with SR on (U2) | Both probes on one DC node first; then SW2 against V_out with SR on |
| 4 | Peak margin at 56 W | I_pk 11.9 A (nominal L) against the 12.5 A backstop; read IL_MON peaks before a longer run |
| 5 | SW_OVP from a 90 µs VD_MON disturbance | Source of the disturbance at reconnection; whether the relative OVP should need consecutive samples is a design decision |
| 6 | Sink `tcpp_fault 1` | Read the TCPP02 flags with nothing changing; compare with the sink's start-up |
| 7 | Load-step response | The QT Py disarm is a step (25 W off at once); trigger the trace on its V_out overshoot at the end of a run |

## 7. Files

| Path | Content |
|---|---|
| `scripts/sw_periods.py` | Per-period timing, I_pk, ring from a two-channel record |
| `scripts/sw_nodes.py` | Levels, hard-edge statistics, spectra |
| `scripts/plot_sw.py`, `plot_spectrum.py` | Figures |
| `scripts/make_tables.py` | Fills the generated tables in this file from `metrics/` and `data/` |
| `metrics/sw_periods.json`, `metrics/sw_nodes.json` | Results |
| `data/sw_28v_*.csv` | Scope records (t_s, CH2, CH3) |
| `data/status_28v_*.jsonl`, `trace_28v_sr1_25w.csv`, `load_*_frames.json` | Regulator status, trace, QT Py telemetry |
