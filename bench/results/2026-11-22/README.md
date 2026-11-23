# Bench 2026-11-22 — AD8410A on IL_MON, resistor only, DCM 5 → 33 V

- U7 (IL_MON amplifier, was INA281A2 50 V/V) replaced by an AD8410A on an adapter (`KiCad/ad8410a_adapter`):
  REF1 tied to VS, REF2 to GND, so IL_MON sits at VS/2 at 0 A and swings both ways.
- The board's 3V3 rail was down at first (IL_MON idled at 0.53 V, about VS/2 for VS ≈ 1.06 V; inferred). Dan fed the
  board's 3V3 straight from the Nucleo. After that, IL_MON at idle: **1636 mV** (scope VAVG, 0.5 V/div, 20 MHz BWL).
- Load: 25.3 Ω after the VBUS FETs. No PD sink, no Type-C cable, no pack, QT Py not connected. V_in 24.6 V.
- Scope: CH1 IL_MON, CH2 SW1, CH3 SW2, CH4 V_out ahead of the VBUS FETs (TP2).
- Flashed `build/Battery` (non-PD, `regulator start` / `bu battery run` work without a sink), with the IL_MON scaling
  below. `bringup.local.toml` points at `build/Battery`.

## Firmware: IL_MON gain and offset (COMP1 thresholds)

`IL_MON_GAIN_VV` (20) and `IL_MON_ZERO_MV` (1636) in `regulator_config.h`; `DAC_COUNTS_PER_AMP` is now computed
(124 counts/A, 310 for the old part) and `IL_MA_TO_DAC_COUNTS()` adds the zero level. Used by the PEAK_PROTECT
backstops and the CCM peak cap in `regulator.c`; the (shelved) slope ramp floors at the zero level. Buck backstop
9 A → DAC 3147, buck-boost/boost 12.5 A → 3581. A DAC code under the zero level trips COMP1 at any current, so the
0 written at stop stays safe. The 20 V/V gain is the AD8410A's as remembered: the datasheet fetch timed out.

Without this change the old 310 counts/A from 0 V would have put the buck backstop near 6.1 A and buck-boost near
14.9 A at the 1636 mV zero, or 17 A and 26 A at the 0.53 V zero of the dead 3V3 rail (from the scaling, not run).

## Runs (`bu battery run`, 2 A limit as the 11-13 resistor suite)

| Run | Result | V_out (fw) | ID_MON (fw) | Expected into 25.3 Ω | IS (fw, raw) |
|---|---|---|---|---|---|
| `regulator start 5000` | RUNNING, stopped after ~1 s: keepalive (`keepalive_stops` 1), not a fault | 4967 mV | 26 mA | 196 mA | — |
| 5 V, 6 s (`run_5V.json`) | ran to the end, stopped, no fault | 4991 mV | 24 mA | 197 mA | 37 / 3 mA |
| 12 V, 6 s (`run_12V.json`) | ran to the end, stopped, no fault | 12034 mV | 54 mA | 476 mA | 66 / 493 mA |

Scope at 12 V (`scope_12V_running.png`, triggered on SW1): SW1 200 kHz DCM pulses to about V_in, V_out (TP2) VAVG
11.81 V, IL_MON one triangle per cycle from the 1.63 V baseline to VMAX 1.9 V, i.e. about 2.6 A peak at 20 V/V.

**ID_MON reads about 8–9× low.** On 11-16 it read 565–567 mA at 15 V into the same resistor, on 11-13 758 mA near
20 V. Today the load does take its current, by two measures that do not use ID_MON (inferences, not a current probe):
- 12 V: a DCM triangle of 2.6 A peak at 200 kHz from 24.6 V into 4.7 µH carries about 0.52 A.
- 5 V: the firmware's 376 ns on-time gives a 1.57 A peak and about 0.29 A (and its own `i_pk_ma` was 1602).

Cause (Dan, 07:2x): only the AD8410A got the Nucleo's 3V3; the board's 3V3 rail is still down, so U8 (ID_MON) and U6
(IS_MON) are unpowered. Their readings below mean nothing. **CCM, its current limit and the CCM OCP all use ID_MON**,
so the sweep below ran DCM only (`regulator_ccm_enable` 0, written at IDLE; a reset puts it back to 1).

The IL_MON mean (scope VAVG 1.629 V at 12 V vs 1.636 V at idle) is below the 8-bit screen resolution at 0.5 V/div
(20 mV ≈ 0.2 A); it is not a gain check. The gain is only checked by the triangle above, roughly.

## DCM sweep, 15 → 33 V (`run_<V>V.json`, `scope_<V>V_running.png`; 24 V has no screenshot)

`bu battery run`, 5 s each, 2 A limit (not effective: ID_MON is unpowered), scope read 3.5 s in.

| Ceiling | Result | V_out (fw) | TP2 VAVG (scope) | IL_MON VMAX → peak at 20 V/V | SW1 / SW2 VMAX |
|---|---|---|---|---|---|
| 15 V | ran, stopped, no fault | 14960 mV | 14.64 V | 1.98 V → 3.4 A | 31.2 / 17.2 V |
| 20 V | ran, stopped, no fault | 20020 mV | 19.42 V | 1.88 V → 2.4 A | 28.0 / 22.4 V |
| 24 V | ran, stopped, no fault | 24008 mV | 23.44 V | 2.68 V → 10.4 A | 28.4 / 34.4 V |
| 28 V | ran, stopped, no fault | 27937 mV | 27.29 V | 2.78 V → 11.4 A | 28.4 / 39.2 V |
| 33 V | ran, stopped, no fault; **V_out 30.4 V, not 33** | 30420 mV | 29.74 V | 2.84 V → 12.0 A | 28.4 / 43.2 V |

- IL_MON VMAX includes edge spikes (20 MHz BWL). The 28 V screenshot shows the boost-DCM shape: a fast rise to a
  near-flat top at about 2.56 V (≈ 9.3 A; the discharge slope (V_out − V_in)/L is small at 28 V from 24.5 V), then a
  step down at the SW2 edge. That is under the 12.5 A COMP1 backstop (2.886 V). VMAX at 33 V (2.84 V) is close to
  it: whether COMP1 cut pulses there is not checked (no trace taken).
- 33 V short at 30.4 V: as 2026-11-12 (a DCM resistor run above ~30 V at 24.4 V in does not reach the ceiling).
- TP2 (scope) reads 0.3–0.7 V under the firmware's V_out; not calibrated today.

## 3V3 fixed (Dan, later): current monitors back (`run3v3_<V>V.json`)

IL_MON at idle 1640 mV (3 reads, 1639–1640; firmware keeps 1636, inside the ±20 mV of 0.5 V/div). DCM only, 5 s each.

| Ceiling | V_out (fw) | ID_MON | V_out / 25.3 Ω | ID / expected | I_in (IS_MON) | P_out / P_in |
|---|---|---|---|---|---|---|
| 5 V | 5018 mV | 195 mA | 198 mA | 0.983 | 55 mA | 1.00 / 1.35 W (0.74) |
| 12 V | 12476 mV | 468 mA | 493 mA | 0.949 | 288 mA | 6.15 / 7.08 W (0.87) |
| 20 V | 19986 mV | 780 mA | 790 mA | 0.987 | 695 mA | 15.79 / 17.06 W (0.93) |

All three ran to the end and stopped, no fault. ID_MON is back in line with 11-13 / 11-16 (P_out uses the nominal
25.3 Ω, which drifts warm). The 12 V tail mean sits 476 mV over the ceiling (12034 mV in the 07:17 run); not looked into.

## Peak current mode, first bench (`pcmc/`, plan `plans/peak-current-mode.md`)

Same setup: 25.3 Ω, V_in 24.6 V, probes as above, `build/Battery`.

### Firmware

- Timer E runs in lockstep with Timer A: same prescaler and period, reset together, no outputs.
  - DCDE = 1, DCDR = DCDS = 0. CMP2 = 425 ticks (78 ns) is the sawtooth step: 64 steps per 200 kHz period, 32 per
    400 kHz.
  - CPT1 = TA1 reset, CPT2 = EEV4 (COMP1, unblanked).
- DAC3 CH1, COMP1's threshold, runs as a sawtooth: reset on `HRTIM_RST_TRG5`, step on `HRTIM_STEP_TRG5`, downward.
  - `slope_comp_set_ramp(reset, step_12p4)` writes DAC_STR1, which takes effect at the next Timer E roll-over (the
    same boundary as the Timer A/B compares).
  - The PID writes the threshold with step 0, as before; stop writes 0.
- DCM comparator mode: `regulator_pcmc_dcm`, step 6a.
  - Threshold = I_pk − m1·`regulator_pcmc_delay_ns` (200 ns, assumed).
  - CMP2 becomes a backstop at 1.25·t_on + 100 ns.
  - Used only when the threshold falls after the blanking with 20 % margin, and with SR off.
  - Telemetry: `regulator_pcmc_*` (Timer E captures in ns after the pulse start).
- `bu battery run --watch SYM[:TYPE]` logs extra symbols in the same probe session as the keepalive. A separate
  `bu mem read` during a run collided with it on the ST-LINK and ended the first 12 V run (DEV_CONNECT_ERR; the
  regulator stopped on the keepalive, no fault).

### At idle (switching off): Timer E and the sawtooth

| Register | Read | Meaning |
|---|---|---|
| MCR | 0x3F0008 | master and Timers A–E on |
| TE CR / CR2 | 0x08040008 / 0x1 | CONT, PREEN, TRSTU / DCDE |
| TE PER / CMP2 | 27200 / 425 | |
| DAC3 CR | 0xF7 | EN1, TEN1, TSEL1 = 13, WAVE1 = sawtooth |
| DAC3 STMODR | 0xD0D | reset and step triggers = 13 (HRTIM trigger 5) |
| TE CPT1 | 2016 ticks | TA1's EEV4 reset, 0.37 µs into the period = the pulse start + 100 ns blanking (DAC at 0 trips COMP1) |

STR1 was written by hand and the period time of COMP1's last rise read from CPT2:
- Reset 2600 at 16 counts/step tripped in step 34.
- Reset 2300 at 16 counts/step tripped in step 16.
- Both are DAC ≈ 2044–2060.

At 1 count/step (12 reads), COMP1's last rise came at DAC 2056–2084. That is 1647–1680 mV against the 1640 mV scope
mean, so IL_MON's noise at idle reaches ~40 mV (0.4 A) above its mean with no hysteresis. The reset value, the 12.4
step, the roll-over reset and the trigger mapping all behave as configured.

### 12 V DCM with the comparator mode on: COMP1 never ends the pulse

- `run_dcm_12V.json` ran to the end with no fault: 12.01 V, 469 mA.
- Q1's turn-off (CPT1) came at 1079–1220 ns, at the CMP2 backstop (1065–1162 ns).
- COMP1's rise (CPT2) came at 1144–1567 ns, against ~620 ns from the model at the 1.6–1.7 A threshold.

The scope says why (`scope_dcm_12V_200ns.png`; raw 250 MSa/s, 191 pulses, `aligned_dcm_12V.txt` from
`align_pulses.py`):
- SW2 is at 0 V for the Q4 bootstrap refresh and steps to ~14 V at +50…150 ns.
- **IL_MON stays at its 0 A level (±3 mV) from 0 to ~1000 ns while Q1 is on.**
- It then rises to 0.36 V (3.6 A) at 1250 ns, after SW1 falls at ~1100 ns, and decays to 0 at ~1950 ns, where the
  freewheel ends (SW1 leaves −2.5 V).
- From volt-seconds (t_on 1.1 µs at 24.6 − 14.1 V, freewheel 0.78 µs at 14.9 V, 4.7 µH; inference) the real peak
  is ~2.5 A, so the 3.6 A on IL_MON is an overshoot.

### 12 V forced CCM, 200 kHz, comparator mode off (`run_ccm_12V.json`, `scope_ccm_12V_1us.png`)

Ran to the end with no fault: 11.96 V, on-time 2216 ns. SW2 sits at V_out apart from a refresh dip to 0 V just before
each Q1 turn-on. Averaged over 239 periods (`aligned_ccm_12V.txt`):

| t from Q1 on | IL_MON (rel.) | L1's current must be |
|---|---|---|
| 100–900 ns | −0.12 V, flat | rising at ~2.7 A/µs (V_L ≈ 12.9 V, constant) |
| 950–1850 ns | +0.02 V, flat | rising |
| 1900–2200 ns | rises to +0.58 V (5.8 A) | peak at turn-off (~2250 ns) |
| 2500–4500 ns, SW1 low | falls 0.247 V/µs (2.47 A/µs) | 2.68 A/µs (model, 12.6 V / 4.7 µH) |

**IL_MON does not follow L1's current while Q1 is on, in DCM or CCM. It does through the discharge** (0.92× the
model slope). R5 is in series with L1 (L1 → SW2a → R5 → SW2, from the PCB), so the shunt is in the right place; the
fault is in what U7's output does during the on-time. Whether that is the AD8410A's common-mode recovery or the
adapter's input wiring is not known: it needs a probe on the adapter, which is a hardware look.

Consequences:
- **Peak current mode on IL_MON is blocked.** `PCMC_DCM_DEFAULT` is now 0 (the code stays, off).
- **ID_MON and IS_MON can't stand in.** The output ceramics (C15–C19, C49) sit on VOUTa between Q3 and R6, so ID_MON
  is DC. U6 reads the ADM1270's filtered sense.
- **PEAK_PROTECT (COMP1) is not a cycle-by-cycle limit in the on-time either.** In a 28 V run (`regress_dcm_28V_b.json`)
  COMP1 rose at 1855–1897 ns, right after every pulse ended at 1773–1792 ns, against the 12.5 A level (DAC 3581).
  The model peak there is ~9.3 A (24.6 V × 1.78 µs / 4.7 µH; inference), so that rise is IL_MON's overshoot.

### Regression, comparator mode off (`regress_dcm_<V>V.json`, DCM only)

| Ceiling | V_out | I_out (ID_MON) | Earlier today |
|---|---|---|---|
| 5 V | 4980 mV | 196 mA | 5018 / 195 |
| 12 V | 11924 mV | 470 mA | 12476 / 468 (12034 in the 07:17 run) |
| 20 V | 20013 mV | 781 mA | 19986 / 780 |
| 28 V | 27461 mV; repeat 27961 mV | 1079 / 1100 mA | 27937 (ID_MON off then) |

All ran to the end, stopped, no fault. The first 28 V tail mean includes one 26.49 V sample; the repeat's samples
were 27.7–28.15 V.

## IL_MON blind spot: the AD8410A's common-mode deglitch, triggered by SW2 (`refresh/`)

The AD8410A datasheet (Rev D, Theory of Operation; Figures 10 and 11) says the output is held at its last value
for about 1 µs after a large input common-mode transient, then settles. R5's common mode is SW2. The test moved the
output-leg bootstrap refresh (Q4 on, SW2 → 0 V) away from some Q1 turn-ons, with no firmware change:
`regulator_ccm_refresh_interval_ns` (Timer B's period in forced CCM) set to 2 or 3 switching periods. Forced CCM
(`regulator_ccm_enable` 2), 12 V ceiling, 25.3 Ω, comparator mode off. Pulses are grouped by the time from SW2's
return to V_out after its last refresh dip to Q1's turn-on (`align_by_refresh.py`, `aligned_ccm_12V_*.txt`).

| Run | V_out / I_out | Refresh just before Q1 (0 µs) | No refresh since the previous period (2.5 or 5 µs) |
|---|---|---|---|
| 200 kHz, every 10 µs | 11959 mV / 466 mA | flat from the dip to +1650 ns, then a jump | tracks from ~200 ns after Q1 on: 2.63 A/µs (400–2200 ns) |
| 400 kHz, every 5 µs | 11959 mV / 465 mA | flat to +900 ns, a jump at ~1000 ns | tracks from ~200 ns: 2.6 A/µs (400–1000 ns) |
| 400 kHz, every 7.5 µs | 11952 mV / 467 mA | flat to +900 ns | 2.5 µs and 5.0 µs groups both track: 2.6 A/µs (400–1000 ns) |

Model rising slope: (24.6 − 12.0) V / 4.7 µH = 2.7 A/µs. All three runs ended RUNNING → IDLE with no fault, CCM active
throughout. SW2 stays at 11.7–12.1 V in the periods without a refresh; Q3's gate voltage was not probed (at 10 µs the
interval is twice the 5 µs default).

**So the on-time blind spot is the deglitch, triggered by the SW2 step.** SW1's 0 → 25 V edge at Q1 turn-on does
not trigger it: with SW2 steady, IL_MON follows the ramp ~200 ns after the edge (the amplifier's response, Figures
20–23). A pulse one full period after a refresh is visible at both 200 and 400 kHz. The hold measured 1.0 µs
(400 kHz) and 1.7 µs (200 kHz) after SW2 came back up; why they differ is not known.

What this means (inference, not bench-tested yet):
- **CCM:** refresh every Nth period (N ≥ 2) and leave the refresh right before Q1 turns on. The held value is then
  the valley, under any peak threshold, so COMP1 can't false-trip; the refreshed period's pulse ends on the CMP2
  backstop, which must then be the voltage-mode on-time. Moving the refresh to just after Q1 turns off instead
  would hold the peak, so COMP1 would trip at the next pulse's blanking end.
- **DCM (rectifier off):** Q3 and Q4 are both off, SW2 floats, and the Q2 refresh drags it to ~0 V through L1, so
  SW2 still steps 0 → V_out + V_f at every Q1 turn-on (the 0 → 14 V seen in the DCM capture). No refresh move fixes
  that. Q3 would have to hold SW2 at V_out for ≥ 1.7 µs before Q1 turns on, which needs a new Q3/Q4 schedule.

## Buck CCM peak current mode (`ccm_pcmc/`)

Forced CCM (`regulator_ccm_enable` 2), `regulator_pcmc_ccm` 1, 12 V ceiling, 2 A limit, 25.3 Ω, V_in 24.6 V.
Firmware (`pcmc_ccm_step`, regulator.c; `pcmc_ccm_*`, buck_ccm.c):
- Q4 refreshes every second period (Timer B period 2T).
- In the period without a refresh, COMP1 ends Q1 at a DAC3 sawtooth set from the voltage loop's average-current
  command: i_thr = i_cmd + m1·t/2 − m1·τ, with slope compensation above D 0.45.
- In the refresh period, Timer B CMP1 (TIMEVNT1) ends Q1 at the tracked on-time.
- The CMP2 backstop is set past both. Q2 turns on a dead time after it, so Q1 and Q2 can't overlap.

It took five firmware fixes to get there. Each was found on the bench, in this order:

| # | Measured | Cause | Fix |
|---|---|---|---|
| 1 | SW_UVP 0.3 ms after the takeover at 6.4 V: V_out 6.3 → 3.1 V (`trace_12V_200k_tau200.csv`). Scope: pulses ended with IL_MON at 1.78–1.83 V, the DAC level (1.82 V); first pulse 960 ns from a −2.15 A valley, the duty law's 1240 ns (`periods_12V_200k_tau200.txt`) | τ = 200 ns (`regulator_pcmc_delay_ns`) took m1·τ = 0.78 A off the threshold; the comparator's measured lead is ~0 | CCM's own `regulator_pcmc_ccm_delay_ns`, default 0 (DCM's stays 200) |
| 2 | 8 tracker samples in 0.68 s; another run's on-time pinned at 2451 ns while i_cmd wound 725 → 1015 mA, then OVER_BAND exits ×3 and SW_OVP 14.1 V (`trace_12V_200k_tau0_phaselock.csv`, `logs/`) | the PID read Timer E's capture at one fixed phase: TIM7's 50 µs is 10 periods of the same clock, so each run saw one kind of period only | Timer B CPT1 (TA1 reset) copied by DMA2 ch1 into a 2-slot ring; Timer B's count says which period |
| 3 | DMA tracker at ~1 sample/cycle, then SW_OVP: on-time 2350 → 2810 ns while i_cmd fell 450 → 212 mA, V_out 10.6 → 11.95 V in 0.3 ms (`trace_12V_200k_dma_ratchet.csv`) | tracker up at once, down ¼: the copied refresh pulse makes the comparator pulse settle at 2·t_ss − track (volt-seconds, inferred), a gain of −1 that ratcheted up; the ripple term used the same on-time | tracker ¼ either way; ripple term from T·V_out/V_in; backstop track + max(150 ns, track/8) |
| 4 | Stable, but a 2-period pattern: refresh pulses 1923 ± 29 ns, others 2771 ± 381 ns; valleys 1.52 / 1.23 V (`periods_12V_200k_symtrack.txt`) | COMP1 cut the refresh pulse ~2.2 µs into the period, when the deglitch hold released and IL_MON jumped from the held valley to the true current, already over the threshold. This disproves the "valley is held, so no false trip" point above | Timer A's EEV4 filter = TIMFLTR1 (blanking from Timer B's reset to TB CMP1, RM0440 Table 242) in this mode: COMP1 is masked for the whole refresh pulse |
| 5 | 400 kHz: tracker stopped ~3 s in, i_cmd at its 8 A ceiling, threshold 9.6 A, V_out 11.2–11.8 V; each pulse ended at the backstop (8029) and was captured at 8096 | a TA1-reset capture lands 67 ticks (12.3 ns) after the compare, past the 64-tick guard, so every backstop-ended pulse was rejected and the backstop could not move out | subtract the 67-tick latency before classifying |

Result with all five (raw captures at 4 ns/sample, `per_period.py`):

| Run | V_out (ADC samples) | I_out | i_cmd | Refresh pulse | Other pulse | Valleys (IL_MON) |
|---|---|---|---|---|---|---|
| 200 kHz, 6 s (`run_12V_200k_fltr.json`) | 11.95–12.21 V, then SW_OVP at 5.5 s (below) | 460–474 mA | 513–770 mA | 2365 ± 11 ns | 2351 ± 51 ns | 1.369 / 1.338 V |
| 200 kHz, 12 s and 20 s (`run_12V_200k_excursion.json`, `_20s.json`) | 11.52–12.42 V, means 11.97 / 12.02 | 463–477 mA | 507–689 mA | – | – | – |
| 400 kHz, 10 s (`run_12V_400k_lat.json`) | 11.00–12.16 V, mean 11.84 | 441–475 mA | 539–851 mA | 1133 ± 11 ns | 1141 ± 50 ns | 1.538 / 1.512 V |
| 200 kHz, defaults, 10 s (`run_12V_200k_final.json`) | 11.71–12.25 V, mean 12.00 | 432–477 mA | 511–585 mA | – | – | – |

- The tracked on-time was 2.35–2.37 µs at 200 kHz (T·V_out/V_in = 2.44 µs) and 1.12 µs at 400 kHz.
- The backstop ended 0.07–0.19 % of sampled pulses at 200 kHz and 1.3 % at 400 kHz. The tracker got ~19,800 samples/s (PID 20 kHz).
- The takeover from the duty law (`periods_takeover_symtrack.txt`) kept pulse widths at ~1.2 µs at 6.4 V. V_out still
  dipped: the trace (`trace_12V_200k_symtrack_entry.csv`) has it 1.19 V under V_cmd 1.9 ms in (the capture ended at
  1.3 ms). See the 5 and 20 V section.
- i_cmd runs ~50–150 mA above I_out in steady state. The PID absorbs that.

**Open: one excursion.** In the 6 s run above, CCM exited OVER_BAND at about 4.35 s, re-entered, and SW_OVP latched
at 13.9 V about 1.15 s later. The trace wasn't armed for it, and it did not recur in the next 42 s (12 + 20 + 10 s
runs). I checked one hypothesis and ruled it out: the ADC V window sits ~1 µs before Q1's actual turn-off at D 0.49.
The cause is not known.

Regression, peak mode off, 200 kHz, 5 s each: duty-law forced CCM 11966 mV / 467 mA (`regress_dutylaw_200k.json`);
DCM 11986 mV / 468 mA (`regress_dcm_200k.json`). Both ended RUNNING with no fault. DCM at 400 kHz was not rerun.

Not done: load steps, and a measurement of Q2's body-diode time after an early comparator
turn-off. The comparator periods lose Timer A's 100 ns leading-edge blanking (TIMFLTR1 replaces CMP1). In the two
captures with it, the shortest comparator pulses were 2236 ns at 200 kHz (119 periods, mean 2351) and 968 ns at
400 kHz (240 periods, mean 1141). Whether a turn-on glitch causes the 400 kHz minimum is not known.

### 5 V and 20 V (`run_5V_*`, `run_20V_*`, `trace_*`, `periods_*`; `pcmc_run.sh`)

Same setup and firmware (e8ba4a1), defaults (`regulator_pcmc_ccm_delay_ns` 0), 10 s each, captures 5 s in.

| Run | V_out (20 ADC samples) | I_out | i_cmd | Tracked on-time | Refresh pulse | Comparator pulse [min, max] |
|---|---|---|---|---|---|---|
| 5 V, 200 kHz | 4.62–5.09 V, mean 4.93 | 187–198 mA | 477–509 mA | 975–979 ns | 995 ± 6 ns | 993 ± 21 [952, 1052] ns |
| 5 V, 400 kHz | 4.81–5.19 V, mean 5.00 | 196–199 mA | 538–595 mA | 472–474 ns | 484 ± 4 ns | 485 ± 13 [432, 532] ns |
| 20 V, 200 kHz | 19.64–20.23 V, mean 19.96 | 771–786 mA | 215–276 mA | 3926–3927 ns | 3929 ± 8 ns | 3938 ± 36 [3856, 4040] ns |
| 20 V, 400 kHz | 18.83–20.27 V, mean 19.91 | 774–785 mA | 189–277 mA | 1875–1892 ns | 1893 ± 9 ns | 1892 ± 23 [1828, 1948] ns |

- All four ended RUNNING with no fault, in CCM and peak mode at every sample. The only CCM exit in each was the
  STOP at the end.
- The 18.83 V reading at 20 V / 400 kHz was one sample (7.7 s). The samples either side read 19.7–20.0 V, and i_cmd
  didn't move.
- At 20 V, CCM was entered at 6.35 V on the soft-start, so peak mode carried the 80 V/s ramp to 20 V. After the
  takeover dip (below), from 2.7 ms to the trace's end at 90 ms, V_out − V_cmd stayed between −1.15 and +0.28 V
  (200 kHz) and between −1.95 and +0.43 V (400 kHz).
- The refresh period's "valley" column isn't comparable: there IL_MON still shows the value held at the refresh (the
  pulse ends before the hold releases at 5 V), so it is left out of the table.

**i_cmd versus I_out depends on the operating point.** It is +290 to +360 mA at 5 V, +50 to +150 mA at 12 V, and
−0.5 A at 20 V. The PID absorbs it in steady state. Inferred causes:
- At 20 V the slope compensation is large (m_a ≈ 2.5 A/µs at D 0.81). The comparator ends Q1 ~140 ns before T·V_out/V_in,
  where the ramp is still ~0.35 A above i_thr.
- At 5 V the offset is not explained.

**The takeover is not bumpless.** The integrator is preset to the measured current, so the offset above becomes a
current step at the takeover. V_out − V_cmd worst within 15 ms of CCM entry (entry at 4.9 V for the 5 V runs, 6.35 V
for 12 and 20 V):

| 5 V 200k | 5 V 400k | 12 V 200k | 20 V 200k | 20 V 400k |
|---|---|---|---|---|
| −1548 mV (2.65 ms) | −1042 mV (4.55 ms) | −1194 mV (1.9 ms) | −1253 mV (2.55 ms) | −1538 mV (1.70 ms) |

At 5 V / 200 kHz that is 31 % under V_cmd, and recovery took about 8 ms. UVP (50 % relative) didn't trip.

## End state

`build/Battery` flashed (CCM peak mode built, `PCMC_CCM_DEFAULT` 0), reset to defaults: IDLE, no fault,
`regulator_pcmc_ccm` 0, `regulator_ccm_enable` 1, `regulator_buck_fsw_khz` 200, `regulator_pcmc_ccm_delay_ns` 0.
Scope back to CH1 0.5 V/div (20 MHz), 2 µs/div, offset 0, trigger CH2 8 V NORM; CH4 10 V/div, no BW limit.
