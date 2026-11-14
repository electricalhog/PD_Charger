# 2026-11-13 — transition suites (battery-load build)

Firmware: `build/BatteryNext` with the CCM entry window (`regulator_ccm_entry_blind` 20 cycles), limit slew on
every increase (`regulator_ccm_ilim_slew_ma_ms` 50), and the `mode` trace trigger (plans/lipo-load.md sections 7–8).
Flashed 21:1x 2026-11-13 (verified; ID_MON gain 991 / offset 4 read back).

`scripts/transitions.py` — one START per scenario, the 20 kHz trace (102 ms) armed just before the transition
the scenario is for, a summary per scenario from the trace (V_out range, per-cycle ID_MON in CCM against the limit,
mode and CCM edges), stop at the first fault / abort / refusal, never clears a fault. No ammeter prompts. Without
`--go` it only prints the plan.

| Suite | Load | Scenarios | Time |
|---|---|---|---|
| `pack` | 8S pack on VBUS, resistor off | forced entries 1–5 A; auto ramps 0.15 → 5 → 0.15 A at 5 / 20 / 50 A/s; limit steps 1 → 5, 5 → 1, 0.5 → 5, 5 → 0.3 A; 2 ↔ 4 A square ×5 at 300 ms; CCM exit and re-entry; CC → CV and CV → CC | 16, ~2.8 min, ~59 s at 4–5 A |
| `resistor` | 25.3 Ω, no pack (5 V START checks VBUS free) | starts 5 → 12 / 20 / 24 / 26 / 30 / 33 V (auto; 33 V also forced and DCM); ceiling 20 → 28 and 28 → 20 V across V_in; 33 → 12 → 33 V; 4 s dwells at 24.0 / 24.4 / 24.8 V | 14, ~1.5 min, ~1.5 kJ |

## Resistor suite, 21:14–21:17 (`transitions_resistor_20261113_211448.json`, traces in `captures/`)

Dan at the bench, 24.4 V supply on, 25.3 Ω on VBUS, no pack. All 14 scenarios ran to the end: no fault, no abort,
every START accepted; the 5 V START read 0 mV on VBUS.

Held steady:
- 12 and 20 V: buck, no CCM entry, no mode change in 3 s (the `mode` trace never fired).
- 24.0 / 24.4 / 24.8 V dwells, from the trace ring (the last 102 ms before the stop, 2048 samples): buck-boost, CCM
  band kind throughout, no mode change; V_out mean on the setpoint, sd 38–43 mV, range 0.31–0.36 V; ID_MON
  within 10–14 mA.
- 24–33 V auto, 33 V forced: the logged V_out ends within ±0.2 V of the ceiling.

Not held:
- 33 V DCM only: V_out stayed at 30.2–30.5 V for the whole 3 s (about 1.2 A, 37 W).
- Every start through V_in (and `up_20to28V`) leaves CCM twice. At about 22 V the mode changes buck to buck-boost;
  at about 24.2–24.3 V the CCM kind changes band to boost. Each time CCM exits, about 1 ms of DCM follows, then a
  CCM entry with the 1 ms open-loop window. Measured in the traces (80 V/s reference ramp):

| Edge | DCM | V_out low in DCM | V_out in the window |
|---|---|---|---|
| ~22 V, auto (5 starts) | 1.15 ms | 20.2–20.3 V, 1.7–1.9 V under the reference | up to 22.34–22.39 V, 0.6–0.7 V over the latched start |
| ~22 V, forced (33 V start) | 1.15 ms | 19.56 V, 3.6 V under | 22.35 V |
| ~24.3 V, all | 1.00 ms | 22.89–23.16 V, 1.2–1.3 V under | 25.21–25.34 V, 1.5–1.6 V over the latched start and about 0.5 V over the ramp reference |

After each window the reference is re-seated on V_out and the ramp continues from there; the next 2 ms stay within
0.2 V. Inference: the dip is the DCM law taking over mid-ramp, not the window. A direct CCM-to-CCM handover across
the mode and kind change (no detour through DCM) is the candidate fix, not built.

## 2026-11-14 10:28 — CCM to CCM across the mode and kind change (`transitions_resistor_20261114_102812.json`)

Firmware: `regulator_ccm_kind_switch` 1 (plans/lipo-load.md section 9): buck CCM into buck-boost switches straight
to band CCM (an entry's u0 guess and the 1 ms window), band to boost keeps u; no CCM exit, no DCM. Same bench, same
suite. All 14 ran, no fault, no abort. Each 24–33 V start now counts 1 CCM entry, 1 exit (the stop) and 2 kind
switches, against 3 entries and 3 exits on 2026-11-13.

| Start (auto unless noted) | buck → buck-boost | buck-boost → boost |
|---|---|---|
| 24 V | 21.96 V, no CCM gap, V_out 21.97–22.47 V in 3 ms | – |
| 26 V | 21.97 V, no gap, 21.97–22.37 V | 24.39 V, peak 25.34 V (+0.95), 0.90 V over the reference, within 0.2 V after 4.8 ms |
| 30 V | 22.11 V, no gap, 22.02–22.40 V | 24.42 V, peak 25.28 V (+0.87), 0.77 V over, 6.0 ms |
| 33 V | 21.99 V, no gap, 21.99–22.44 V | 24.44 V, peak 25.41 V (+0.97), 0.79 V over, 5.2 ms |
| 33 V forced | 22.97 V, no gap, 22.78–23.23 V | (after the trace) |
| 20 → 28 V | 22.00 V, no gap, 22.04–22.46 V | 24.41 V, peak 25.25 V (+0.84), 0.79 V over, 6.0 ms |

Against 2026-11-13 (table above): the DCM dips (1.2–1.9 V, 3.6 V forced) are gone. Left: at buck-boost → boost the
boost on-time sits at its 360 ns minimum and V_out steps 0.8–1.0 V over the reference. Inference: the switch is
decided on the ramp's setpoint, and the law's reference (the limit's, re-seated at an entry) runs about 1 V behind
it (auto changed mode at 22.0 V, forced at 22.97 V, threshold about 23 V), so boost was chosen for a reference it
can only meet at its shortest pulse.

Also: one logged V_out of 24.23 V at 1.43 s in the 20 V run (buck, DCM), with ID_MON (758 mA) and the on-time
(2628–2635 ns) unchanged and no SW_OVP; inference: a one-sample reading, not the output.

The 2026-11-13 trace CSVs were overwritten by this run (the script named captures by scenario only; it now adds the
time). The 2026-11-13 numbers above were read from those traces before that.

## 2026-11-14 10:35 — the upward switch waits for the law's reference (`transitions_resistor_20261114_103518.json`)

Follow-up build (plans/lipo-load.md section 9, second paragraph). Same bench and suite: all 14 ran, no fault, no
abort; each 24–33 V start 1 entry, 1 exit, 2 kind switches; the dwells unchanged (no mode change).

| Start (auto unless noted) | buck → buck-boost | buck-boost → boost | Whole trace (100 ms) |
|---|---|---|---|
| 24 V | 23.01 V (ref 23.03), step +0.07 V | – | −0.27..+0.15 V from the reference |
| 26 V | 23.00 V, step +0.27 V | 25.58 V, boost on-time 437 ns, 0.13 V over the reference at most | −0.13..+0.25 V |
| 30 V | 23.07 V, step +0.02 V | 25.58 V, 437 ns, 0.13 V over | −0.30..+0.13 V |
| 33 V | 22.98 V, step +0.11 V | 25.55 V, 425 ns, 0.09 V over | −0.23..+0.13 V |
| 33 V forced | 23.03 V, step +0.24 V | 25.58 V, 433 ns, 0.12 V over | −0.20..+0.15 V |
| 20 → 28 V | in CCM throughout | | −0.24..+0.17 V |

No CCM gap in any of them. The change now happens at the thresholds (about 23.0 V and 25.6 V), boost starts above
its 360 ns minimum, and V_out stays within 0.3 V of the reference through both. The lowest point (−0.24 to −0.30
V) is inside the 1 ms window after buck → band, the open-loop guess.

Downward is unchanged (CCM leaves on any falling setpoint, RAMP_DOWN): `down_28to20V` falls to 0.91 V under the
reference at the buck-boost → buck change, in DCM; `swing_33to12to33V` drops 2.72 V under the reference 0.65 ms
after the ceiling falls from 33 V, in DCM, which the DCM-only start shows cannot hold more than about 30.4 V at
this load.

Script notes: SWD status reads limit the logger to about 3.3 rows/s, so the script's abort needs about 1 s
(SW_OCP and SW_OVP in firmware are the fast protections), and the timed events fired 0.85–0.9 s late.

## 2026-11-14 10:49 — CCM downward: boost → band → buck without leaving CCM (`transitions_resistor_20261114_104951.json`)

Build with `regulator_ccm_down` = 1 (plans/lipo-load.md section 10), flashed 10:48. Same bench (24.6 V in, 25.3 ohm,
no pack). The suite now has three downward scenarios (17 in all): all ran, no fault, no abort, each 1 CCM entry and
1 exit.

| Scenario | band → buck at the reference | trace (100 ms from the change) |
|---|---|---|
| `down_28to20V` | 22.26 V; band at its 150 ns floor for 0.75 ms before; buck on-time 4178 ns after | CCM in every sample down to 20 V; −0.13..+0.16 V from the reference |
| `down_24to21V` | 22.29 V; 0.35 ms at 150 ns; 4177 ns | CCM throughout; −0.13..+0.17 V |
| `down_33to12V_forced` | 22.24 V; 0.6 ms at 150 ns; 4171 ns | CCM throughout (to 14.2 V in the window); −0.13..+0.15 V |

The 0.91 V and 2.72 V DCM dips of the 10:35 run are gone. The boost → band change (inside buck-boost mode, about
25.2 V) is not in any trace: `down_27to24V` triggered on a mode change and 27 → 24 V never changes mode (both are
buck-boost), so its trace never fired. The logged rows (3.3/s) show 3 kind switches and no CCM gap in it; a
transient there is not measured.

Auto still leaves CCM at the bottom: in `down_28to20V` and the swing, CCM ends in buck near 20 V. The sweep
(2026-11-14 README) logs the reason: LIGHT_LOAD at the 20 V point (0.81 A against the DCM ceiling), the designed
auto exit; auto never enters at 12 or 20 V into this resistor either (`start_12V_auto`, `start_20V_auto`: 0
entries).
