# 2026-10-16: Phase 0 power and temperature at 5, 15 and 28 V

This is the record of the evening's bench work, in the order it happened. The plan behind it
is Phase 0 of `plans/240w-roadmap.md`.

## Setup

- **Source supply.** 1500 W server PSUs at 24.56 V, through 10" of 8 AWG and an XT60. V_in is
  taken as 24.56 V at every point.
- **Load.** The 25 Ω / 25 W on VBUS. From the 15 V reading it works out to 25.3 Ω.
- **QT Py buck.** Left on VBUS, not armed. Its draw:

  | VBUS | Draw | Power | How |
  |---|---|---|---|
  | 5.17 V | about 73 mA | 0.38 W | from the total minus V/25 |
  | 15.17 V | 80 mA | 1.21 W | measured |
  | 28 V | about 90 mA | 2.5 W | measured |

- **Meters.**
  - Input current from the supply display.
  - VBUS and the total output current (25 Ω plus QT Py) on the user's meters.
  - Temperatures by IR. L1 was read bare at first, then on tape.
- **Scope.** CH2 SW1, CH3 SW2, CH4 VBUS. CH1 not used.
- **Firmware.** `3ec2a3b` with the bench options (SPR 5–24 V, EPR 28/36/48 V). From 22:2x it
  also has the ADC fix below.
- **Logging.** `scripts/phase0_log.py` logged each step: 20 board reads averaged, with the bench
  readings typed in.

## What happened

**1. Start (about 21:55).**
- The regulator was latched in SW_UVP with V_in at 0.06 V. A 5 V contract had started with the
  supply off.
- While the regulator was latched, the sink re-attached about 376 times.
- With the supply on, the fault was cleared. The sink attached at 5 V / 500 mA and the regulator
  ran.

**2. 5 V, rectifier off (22:05).**
- 50 mA in (display) at 24.56 V, and 280 mA out at 5.17 V.
- That is 1.23 W in for 1.45 W out. The display is too coarse at this power.
- Board: V_out 5.01 V and V_in 24.17 V, against 5.17 V and 24.56 V on the meters.

**3. Sink USB.**
- The sink's ST-LINK was first plugged through a VIA Labs USB 2.0 hub. It enumerated with
  `error -71` and then `Cannot enable. Maybe the USB cable is bad?`, every 4 s from 21:45:43
  to 21:48:51.
- Plugged straight into the laptop, it came up.

**4. 15 V, rectifier off (22:18).**
- 560 mA in, 680 mA out at 15.17 V (80 mA of it the QT Py).
- That is 13.75 W in, 10.32 W out: 75.1 %, 3.4 W lost (board housekeeping included).
- Command 777 mA.

**5. IS_MON and ID_MON read 0.**
- The user's meter read 0.128 V on PB0 (Arduino A3), which is 512 mA at 250 mV/A.
- ADC1's injected sequence was the cause:
  - JSQR was `0x400`: one conversion, IL_MON only. JDR2 and JDR3 were 0.
  - The HAL programs injected rank 1 only when `ScanConvMode` is `ADC_SCAN_DISABLE`
    (`stm32g4xx_hal_adc_ex.c`).
- **Fix:** `adc_monitor.c` writes JSQR for the three ranks after the HAL calls. Built and flashed
  on approval (22:2x). JSQR reads `0x1e38402` after.
- **After the fix the currents convert, but read at one phase of the period.** Each is one
  sample per PID period. At 15 V the board read 828 mA in (512 mA on the pin) and 1496 mA out
  (680 mA metered). At 28 V the output read about 0.

**6. EPR.**
- At 15 V the first `epr 100` got `epr_enter_failed reason=CableNotEprCapable`. The source log
  showed two attaches 0.4 s apart when the sink's USB was moved.
- After the flash's fresh attach, EPR entered and 28 V was granted.

**7. 28 V, rectifier off (22:25).**
- 1.74 A in, 1.19 A out at 27.87 V.
- That is 42.73 W in, 33.17 W out: 77.6 %, 9.56 W lost.
- Command 1473 mA against a 1598 mA ceiling. Modeled peak 9.6 A.

**8. The 25 Ω unplugged at 28 V (about 22:27).**
- The user pulled the 25 Ω (about 1.1 A down to about 0.09 A). The regulator latched
  **HW_FLT1** (VS_GOOD low with the input path on).
- The last loop snapshot before the trip: V_out 27.98 V, command 1455 mA, V_in 24.0 V.
- IS_GOOD stayed high. The source sent one Hard Reset, then the sink re-attached about 95 times
  while the regulator was latched.
- The fault was cleared and the board restarted at 5 V with no repeat. It was then taken to
  28 V with the 25 Ω still off, where the QT Py read about 90 mA.
- The 25 Ω was plugged back in at 28 V, and regulation held.

**9. 28 V, rectifier on (22:30).**
- `regulator_sr_enable` = 1 at 28 V. CubeProgrammer reported "Failed to download data", but the
  value took effect.
- Fade-in complete, window 2126–3392 ns, command 1516 → 1360 mA.
- 1.48 A in, VBUS 28.05 V. The output current was not measured (estimated at 1.199 A), which
  gives 92.5 % and 2.7 W.
- Coil whine was audible.
- VBUS on the scope:
  - at 500 ms/div, a swing of about 1 V p-p at about 5 Hz;
  - at 500 µs/div, 1.6–2.2 V p-p, mostly switching noise, with no 3.9 kHz.

**10. Switch nodes at 28 V, rectifier on (22:45).**
- Scope memory, 4 ns, 1.2 ms (`data/cap/sw_28v_sr1_raw.csv`, `scripts/sw_ring.py`,
  `metrics/sw_ring.json`). 52 periods were classified.
- **Charge** (SW1 at 24.6 V, SW2 at −1.7 V): mean 1786 ns. That gives about 9.3 A at the peak.
- **Discharge** (SW1 at −0.8 V, SW2 at 31.4 V): mean 1564 ns. That is where the current reaches
  zero for that peak (1565 ns).
- **Clamp:** at that point both nodes swing. SW1 goes about 2 V over V_in and SW2 below ground,
  on Q1's and Q4's body diodes, for about 240 ns.
- **Free ring:** then both nodes ring in antiphase until the next pulse, period 420 ns (IQR
  378–436), 2.38 MHz. With L1 that is 0.95 nF.
- **Spikes beyond the clamp levels, near edges:**
  - SW1 below −4 V for about 20 ns per period, within 12–24 ns of an edge (minimum −10.5 V);
  - SW2 above 35 V for about 26 ns per period, 36–48 ns from an edge (maximum 41.3 V);
  - VBUS 3.2 V p-p bursts at 12.5 MHz after each SW1 rise.

  The probe ground leads were not checked.

**11. Thermal at 28 V, rectifier on.** Ambient 21.4 °C.

| Time | Q3/Q4 hot spot | Q1/Q2 hot spot | L1 | Command |
|---|---|---|---|---|
| 22:33 | 64.0 °C | 55.0 °C | | 1372 mA |
| 22:40 | 65.0 °C | 55.0 °C | 63 °C (bare, reads low) | 1369 mA |
| 23:17 | 68.9 °C | 65.2 °C | 66.6 °C (tape) | 1392 mA |
| 23:20 | | | | 1357 mA |

- At 23:20: 1.48 A in, 1.23 A out at 28.14 V (measured). That is 36.35 W in, 34.61 W out:
  95.2 %, 1.74 W lost.
- The user noted that L1, between the two legs, heats the FET spots.
- The user called 23:17 steady state. Its readings came 36 min after the 22:40 ones. No
  cool-down was taken.

**12. Down to 5 V (about 23:25).** 5 V granted with the rectifier on: buck, command 232 mA, no
fault. Left there.

## Readings

All rows are at V_in 24.56 V. Loss includes the board's housekeeping, which was not measured.

| Step | P_in | P_out | η | Loss | Command / output current |
|---|---|---|---|---|---|
| 5 V, rectifier off | 1.23 W | 1.45 W | (display too coarse) | | 1.28 |
| 15 V, rectifier off | 13.75 W | 10.32 W | 75.1 % | 3.43 W | 1.14 |
| 28 V, rectifier off | 42.73 W | 33.17 W | 77.6 % | 9.56 W | 1.24 |
| 28 V, rectifier on (I_out estimated) | 36.35 W | 33.63 W | 92.5 % | 2.72 W | 1.14 |
| 28 V, rectifier on, hot (I_out measured) | 36.35 W | 34.61 W | 95.2 % | 1.74 W | 1.10 |

- The two rectifier-on rows differ mainly in how the output current was taken: estimated
  1.199 A, then measured 1.23 A.
- About 1 % on either current moves the loss by about 0.4 W.

## Files

- `scripts/phase0_log.py`: the step logger.
- `scripts/sw_ring.py`: the switch-node analysis.
- `metrics/phase0.json`: every logged step, with the bench readings and the board means.
- `metrics/sw_ring.json`: the switch-node figures.
- `data/` (kept out of git; on the bench machine):
  - `phase0.jsonl`, the raw log;
  - `cap/`, the scope memory and screen captures.
