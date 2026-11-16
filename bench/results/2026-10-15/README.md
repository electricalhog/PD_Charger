# 2026-10-15: 25 Ω on VBUS, interim DCM limits, first predictive-rectifier runs

This is the record of the day's bench work in the order it happened. It covers what was flashed,
what each step did, and what was seen. The plan behind it is `plans/ccm-predictive.md`; its
conclusions are not repeated here.

## Setup at the start

- **Source.** G474 with the power board. 24 V bench supply (more than 10 A).
  - Firmware: as committed in `c5151d9`, plus the 36/48 V EPR gates (built with them on), the
    VD_MON recalibration and the 4 A boost peak limit from 2026-10-12.
  - D3 (the VD_MON clamp) disconnected on 2026-10-12.
- **New today: a 25 Ω / 25 W resistor fixed on VBUS,** on a large heat sink. It is not switched,
  so every contract loads it: V²/25 is 1 W at 5 V, 16 W at 20 V, 31 W at 28 V, 52 W at 36 V and
  92 W at 48 V. Its voltage is on scope CH1.
  - The user's limit: up to 5× rated power in short steps, averaging no more than 25 W over
    10 minutes. 31 W at 28 V is fine continuously.
- **QT Py buck load.** Back on its 11 Ω / 10 W ballast and not armed all day. All load here is
  the 25 Ω.
- **Scope.** CH2 SW1 (Q1/Q2 node), CH3 SW2 (Q3/Q4 node), CH4 VBUS at TP2.
- **Sink.** NUCLEO-G431 over the Apple 240 W cable.

## What happened

The regulator readings below are `bu regulator status` reads transcribed into
`metrics/steps.json`; each table cell is the mean of 2–6 reads. The bench was left at the 28 V
EPR contract from the previous session.

**1. The resistor went on.** Afterwards the regulator was found latched in SW_UVP, with VBUS at
0 V and no contract. It had not held 28 V against the new load. The fault was left latched while
the next build was prepared.

**2. Run A (build 17:12).**
- Build: DCM peak limits raised from 2.5 / 4 / 4 A (buck / buck-boost / boost) to 6 / 8 / 10 A,
  with the comparator at 150 % capped at 12.5 A. Every PDO at 3 A. A firmware cap Hard Resets a
  contract above 28 V after 2 s.
- Steps: 5 → 9 → 15 → 20 V, then 24 V.
  - 5–20 V held.
  - At 24 V the regulator was in buck-boost with the current command at its ceiling (1252 mA)
    and VBUS at 22.7 V. The sink's transition timer ran out; it sent three Hard Resets, then
    detached and re-requested.
  - Returned to 20 V.

**3. Run B (build 17:15).**
- Build: buck-boost peak limit 10 A.
- 24 V held: 24.1 V, command 1352 of 1840 mA, peaks about 8.6 A.
- 28 V, the first EPR request with the resistor:
  - The contract was accepted and PS_RDY was sent.
  - VBUS then sat at 22.0 V in boost, with a 276 mA ceiling and 125 ns pulses.
  - The mode log: buck-boost → boost at setpoint 27.49 V with V_out 27.34 V, and boost →
    buck-boost only when 20 V was requested afterwards.

**4. Run C (builds 17:19).**
- Build:
  - Buck-boost used up to V_in + 7 V (was + 3 V).
  - Boost hands back to buck-boost when V_out sags 1.5 V under that edge.
  - PS_RDY waits until the ramp has finished.
  - A Hard Reset when VBUS stays more than 8 % under the setpoint for 200 ms.
- 28 V held in buck-boost: 27.96 V, command 1520 of 1598 mA, peaks about 9.8 A.
- Parked at 20 V. The user reported Q1–Q4 hot. Parked at 5 V.

**5. Run D (builds 17:52/17:54).**
- Build: the rectifier build, with `regulator_sr_enable` = 0.
  - Timers C and D give the rectifier edges, Q4 has its own set/reset (no dead-time generator),
    and Timers A–D are preloaded and updated together.
  - With the rectifier off this is meant to switch as before.
- 5 → 28 V all held. The commands came within 5 % of runs A–C, except 20 V (1058 mA against
  960 and 908).
- The three 15 V reads were 14.94, 15.01 and 14.30 V.
- Capture `sr0_bb28` at 28 V (18:51), top of `figures/sw_28v_bb_off_on.png`:
  - After each charge pulse, SW2 sits at about 33 V and SW1 at about −3 V for 1.3 µs.
  - Q3 and Q2 conduct through their body diodes at peaks near 10 A.

**6. Rectifier on, first attempt (19:14).**
- At 5 V, capture `sr0_buck5`, then `regulator_sr_enable` written 1, with no fade-in in that
  build.
- Within a second the regulator latched SW_OVP. The fault snapshot: V_out 5519 mV against a
  5000 mV setpoint, and 1121 pulse-skip transitions.
- Capture `sr1_buck5` (19:14:40) was taken after the fault, with the outputs off. It shows no
  switching.
- Written back to 0.

**7. Run E (build 19:16).**
- Build:
  - The rectifier fades in over 100 ms: its window, the model's diode term, and Q3's start in
    buck.
  - In buck, pulse skipping also turns off Q4's refresh and Q3.
- Flashed. At 5 V the command was 374 mA with the rectifier off. Then `regulator_sr_enable` = 1:
  - Fully faded in, no fault, VBUS 4.93–5.12 V, the command 227–230 mA.
  - Capture `sr1_buck5b` (19:17), bottom of `figures/sw_5v_buck_off_on.png`:
    - During the freewheel SW1 sits near −0.5 V (−3 V with it off), and SW2 at VBUS (7.5 V
      with it off).
    - About 150 ns before conduction ends, SW1 steps to −2 V and SW2 to 7.5 V: the body
      diodes take the tail.
- Stepped up with the rectifier on: 9, 15, 20 and 24 V, then EPR 28 V. All held, with no fault
  (table below).
- Capture `sr1_bb28` at 28 V (19:18), bottom of `figures/sw_28v_bb_off_on.png`:
  - During conduction SW1 is near −0.7 V and SW2 about 2 V lower than in `sr0_bb28`.
  - At about 1.25 µs both nodes step to the diode levels for about 0.15 µs.
  - Afterwards the nodes ring between about 0 and 26 V until the next pulse. The cycle is
    420 ns (median of 111 cycles in this capture, measured 2026-10-16); "about 2 µs per cycle"
    was written here at first.
- The user reported the FETs cooler than in run C.

**8. 36 V burst (19:22).**
- Requested from 28 V with the rectifier on, scope armed on VBUS (`burst36_sr1`,
  `figures/burst36.png`).
- The regulator was in boost:
  - V_out 35.9–36.5 V by the ADC.
  - The command 2080–2163 mA against a ceiling of 2050–2280 mA, about 96 % of it.
  - Peaks 6.4–7.2 A, below the 10 A limit.
- The user heard a screech.
- VBUS was above 33 V for 2.09 s, averaging 34.9 V on the scope. It had 5.3 V p-p (0.88 V rms)
  in a 20 kHz band, in a sawtooth with its strongest line at 3.92 kHz and harmonics near 8 and
  12 kHz.
- The 2 s cap then sent a Hard Reset (`pd_bench_high_v_resets` = 1). VBUS fell to about 2.6 V,
  drifted down to 1.5 V, and came back at 5 V SPR about 1.1 s later.
- In the second before the request, at 28 V, the same capture shows VBUS swinging about
  ±0.5 V at a few Hz.

## Readings

The current command needed for the same 25 Ω load (`figures/icmd_by_run.png`, `metrics/steps.json`):

| Contract | Load (V/25) | Run A | Run B | Run C | Run D (rectifier off) | Run E (rectifier on) |
|---|---|---|---|---|---|---|
| 5 V | 200 mA | 357 | | | 370 | 228 |
| 9 V | 360 mA | 531 | | | 554 | 456 |
| 15 V | 600 mA | 755 | | | 775 | 629 |
| 20 V | 800 mA | 964 | 907 | 1002 | 1058 | 845 |
| 24 V | 960 mA | not held | 1352 | | 1382 | 1187 |
| 28 V | 1120 mA | | not held | 1520 | 1553 | 1373 |
| 36 V | 1440 mA | | | | | 2127, about 96 % of its ceiling |

Command and load are in mA. Reading the cells:
- Run D's 20 V cell is the 1052–1062 mA reads. Runs A–C read 893–1003 mA at 20 V.
- The QT Py load sat on VBUS all day, not armed.

The rectifier window ran from the pulse end + 50 ns to 85 % of the predicted discharge
time − 40 ns:

| Contract | Charge pulse | Rectifier window (ns from the period start) |
|---|---|---|
| 5 V | 343 ns | 642–1672 |
| 9 V | 728 ns | 1028–1974 |
| 15 V | 1434 ns | 1731–2370 |
| 20 V | 2886 ns | 3167–3564 |
| 24 V | 1522 ns | 1822–3024 |
| 28 V | 1791 ns | 2083–3282 |
| 36 V | 1383 ns | 1669–4044 |

## Figures

- `figures/icmd_by_run.png`: the current command against contract for each run. Crosses mark the
  contracts that did not hold (run A at 24 V, run B at 28 V).
- `figures/sw_5v_buck_off_on.png`: SW1, SW2 and VBUS at 5 V, rectifier off (19:14) and on
  (19:17), one period from Q1 turn-on.
- `figures/sw_28v_bb_off_on.png`: the same at 28 V buck-boost (18:51 off, 19:18 on), from the end
  of the charge pulse.
- `figures/burst36.png`: the 36 V burst. On the left the whole record, including the second at
  28 V before and the return to 5 V. In the middle 5 ms of the hold, on the right its spectrum.

## Files

- **`metrics/`:**
  - `steps.json`: every regulator read used above, per run and contract, with the build and the
    notes.
  - `burst36.json`: the burst figures.
- **`scripts/figures.py`:** makes the figures and `burst36.json` from those.
- **`data/cap/`:** the six captures. Kept out of git; on the bench machine.
