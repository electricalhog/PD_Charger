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

## End state

`build/Battery` with `PCMC_DCM_DEFAULT` 0 flashed, IDLE, input and output paths off, no fault,
`regulator_ccm_enable` 0 (DCM only) until reset. Scope back to CH1 0.5 V/div, 2 µs/div, trigger CH2 8 V.
