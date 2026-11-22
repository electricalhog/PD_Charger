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

## End state

`build/Battery` flashed, IDLE, input and output paths off, no fault, `regulator_ccm_enable` 0 (DCM only) until reset.
Board 3V3 fixed, ID_MON / IS_MON reading. Scope on the layout above.
