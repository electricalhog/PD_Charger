# 2026-11-14 — voltage sweep 12 → 33 → 12 V: efficiency, loss, switch nodes

Bench: 24.6 V supply, the 25.3 ohm on VBUS, no pack. Scope (2026-11-12 assignment): CH1 SW1, CH2 V_in, CH3 SW2,
CH4 V_out. Firmware: the BatteryNext build with CCM downward (`regulator_ccm_down` 1, `ccm_kind_switch` 1,
`ccm_entry_blind` 20; plans/lipo-load.md sections 9 and 10), flashed 10:48.

`scripts/sweep.py --go` (10:53–11:02, `sweep_20261114_105322.json`): one START at 12 V, auto CCM, limit 2 A as a
backstop, then the ceiling stepped through 23 points to 33 V and 22 back to 12 V without stopping; at each point
2 s settle and one raw 4-channel capture (30k points, 240 us, 125 MSa/s; `captures/sweep_20261114_105322_*.csv.gz`).
No fault, no abort. `scripts/sweep_analysis.py` → `sweep_points_20261114_105322.json` and `figures/`.

Heat: the CCM points took about 14 s each, not 5 (the scope's re-centre took about 10.5 s whenever CCM was on,
0.8 s in DCM; not understood), so the resistor took about 14.1 kJ in 9.1 minutes, under the 15 kJ / 10 min budget
but 2.7x the 5.3 kJ the plan stated.

## The converter's path (board log, `exit_reason` and `kind_last` on every row)

| Stepping up | Stepping down |
|---|---|
| DCM 12–21 V | boost 32–25.6 V |
| buck CCM entry at the 22 V point (t 38.8 s) | boost → band at the 25.2 V point (0x23) |
| buck → band at 23.5 V (0x13) | band → buck at the 22 V point (0x31) |
| band → boost at 25.6 V (0x32) | buck CCM 22–21 V, then LIGHT_LOAD exit at the 20 V point |

One CCM entry and one exit in the whole run, 4 kind switches. The kind hysteresis shows: band to 25.2 V up, boost
from 25.6 V down; buck to 23.0 V up, band down to 22.5 V.

## Efficiency and loss (`figures/sweep_efficiency.png`)

Method of 2026-11-11: V_in, V_out the scope medians; I_out ID_MON, I_in IS_MON, board means over the 1.5 s before
each capture. Measured, stepping up (stepping down in brackets):

| State | V_out | Efficiency | Loss |
|---|---|---|---|
| DCM | 11.8–20.6 V (5–17 W) | 71.9–86.2 % (76.1–90.7 %) | 2.1–2.6 W (1.6–2.5 W) |
| buck CCM | 21.6–22.6 V | 93.0–93.6 % (94.4–95.2 %) | 1.34–1.38 W (0.97–1.03 W) |
| band CCM | 23.1–24.7 V | 93.8–94.4 % (95.5–96.5 %) | 1.36–1.40 W (0.89–0.96 W) |
| boost CCM | 25.1–32.2 V (25–41 W) | 93.7–94.9 % (96.0–96.8 %) | 1.36–2.75 W (0.84–1.64 W) |

The up / down gap is mostly ID_MON (`figures/sweep_sensor_check.png`): ID_MON / (V_out / 25.3 ohm) creeps 0.960 →
0.990 over the up pass, then steps to 1.020 at the first down point, at the same operating point (boost, on-time
1331 ns both sides), while V_in·I_in / (V_out² / 25.3 ohm), which does not use ID_MON, is continuous across the turn
(1.057 → 1.062). With I_out = V_out / 25.3 ohm instead, up and down agree where both were measured in one state
(20 V DCM: 87.2 % up, 87.2 % down; boost 29.3 V: 95.7 % / 94.3 %, a 1.4-point gap still in IS_MON's or the
resistor's drift). The resistor warms through the run and its temperature coefficient is not known, so that
estimate's absolute level is uncertain too. The cause of ID_MON's step is not known.

Read with that: CCM is 93–97 % from 21.6 to 32.2 V at 18–41 W, with no visible dip at the buck / band / boost
changes in either direction; DCM at these loads is 72–91 %, 1.6–2.6 W loss (up: DCM at 20.6 V lost 2.64 W, buck CCM at
21.6 V 1.36 W).

## Switch nodes (`figures/sweep_switch_nodes.png`)

One capture per state, stepping up: DCM 15.7 V (both nodes ring once the inductor current reaches zero, the
rectifier FETs off), buck CCM 22.1 V (SW2 held at V_out, SW1 switching at 200 kHz, on-time 4214 ns), band 23.9 V
(both legs switching, 555 ns), boost 29.4 V (SW1 held high with spikes to about 31 V at each SW2 edge, SW2
switching, 1089 ns). Overshoots are scope-probe measurements with ground leads, no bandwidth limit.
