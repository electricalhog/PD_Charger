# Bench 2026-11-22 — AD8410A on IL_MON, resistor only; ID_MON reads ~8× low

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

What changed today is the 3V3 supply (rail down, now from the Nucleo). Cause not looked into. **CCM, its current
limit and the CCM OCP all use ID_MON**, so no run above 12 V and no CCM until ID_MON reads right again.

The IL_MON mean (scope VAVG 1.629 V at 12 V vs 1.636 V at idle) is below the 8-bit screen resolution at 0.5 V/div
(20 mV ≈ 0.2 A); it is not a gain check. The gain is only checked by the triangle above, roughly.

## End state

`build/Battery` flashed, IDLE, input and output paths off, no fault. Scope on the layout above.
