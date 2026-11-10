# LiPo as a power sink (branch `claude/lipo-load`, 2026-11-09)

**Why.** Dan: a LiPo on VBUS as a load that can sink a lot of power, on its own branch with no USB at all. Charging is
managed so the pack is never full; this is not a charger (no termination, no balancing).

**Base.** `claude/ccm-hf-buck-50w` at d427689 (PPS, the shared CCM/DCM current limiter, the ID_MON CCM calibration, the
start that waits for V_in).

## 1. Build

- `cmake --preset Battery` (`build/Battery`); `bu` on this branch points there (`tools/bringup/bringup.toml`). The PD
  branches' `build/Debug` cache is never touched from here.
- `BATTERY_LOAD` is set unconditionally in `CMakeLists.txt`. Any PD option ON stops the configure.
- **Left out of the build:** the USB-PD stack, UCPD, TCPP0203, the tracer/GUI, the SRC1M1 BSP, `pd_policy.c` and
  `pd_vbus.c`.
- `Core/Src/battery_no_usb.c` stubs the five entry points the generated code still calls. The UCPD peripheral is never
  enabled, so the CC pins show no Rp. Flash: 69 KB (the PD build is 160 KB).

## 2. What the firmware does differently

| Item | Battery build | Why |
|---|---|---|
| OUTPUT_DIS (VBUS discharge, after Q5/Q6) | never asserted (`OUTPUT_DIS_WRITE`) | It would short the pack. The PD build asserts it at init, in IDLE, on every stop and on every fault. |
| OUTPUT_EN | the regulator drives it (`OUTPUT_EN_SHARED_WITH_TCPP` 0) | The non-PD build left it to the SRC1M1 BSP. |
| Boot | no auto-start | The non-PD build started at 28 V. |
| START (mailbox, arg = ceiling mV) | Reads the pack with Q5/Q6 closed and every FET off (30 ms), then opens the switch. Starts at max(V_pack, 5 V). Step 4c closes the switch once V_out is within 1 V, then the target goes to the ceiling. | No mismatch at the connect. The current limit is in force from the first cycle. |
| Current | `regulator_ccm_ilim_ma` (boot default 500 mA) through the shared limiter (step 5d), in DCM and CCM | V_out held at V_pack + I·R. This is the PPS limit, proven within ±20 mA into 25.3 Ω (bench 2026-11-06). |
| Ceiling | `regulator_battery_ceiling_mv`, 5–25 V (`BATTERY_CEILING_MAX_MV`). SET_VOLTAGE changes it. | Set it below the pack's full voltage. |
| CCM no-reverse floor | Both CCM laws hold the duty at or above the one that balances the pack as read at START. | Without it, a reference below the pack drives L1's average current negative, back into V_in. Host test: −25.6 A buck / −12.8 A boost without, 0.00 / −0.01 A with (averaged model). The first build floored at V_out filtered over 0.8 ms, which held a CCM entry overshoot until SW_OCP (2026-11-09). In the model, L1 from 5 A at a 2 A limit stays at 5.00 A with that floor and settles to 2.00 A with the pack floor. |
| Rectifier windows in DCM | forced off (`regulator_sr_enable` 0) | DCM through the body diodes cannot reverse. |
| SW_VIN_RISE fault | V_in more than 2 V over its start value for 3 PID cycles | The bench supply cannot sink. This is the backstop for any reverse path left. |
| Keepalive | RUNNING stops if `regulator_battery_keepalive` is unchanged for 2 s | There is no PD stack to time out. |
| switch-pulse | refused | Reclosing onto the pack after V_out moved is a surge of ΔV / R_series. |
| UVP while limiting | floor 3 V (`BATTERY_CL_UVP_MV`) | A resistor-only run folds to I·R. |

A fault or stop opens Q5/Q6 and leaves OUTPUT_DIS low. Lowering the ceiling below the pack ends in SW_OVP: DCM cannot
pull V_out down and the pack holds it up. That is a safe stop; to stop, use stop.

**Tool:** `bu battery status`, and `bu battery run --ceiling-mv C --ilim-ma I --seconds S`.
- `run` writes the current, starts, sends the keepalive and logs every 0.2 s.
- It always stops at the end, and also on Ctrl-C or SIGTERM.
- Log: `bringup_out/*_battery_run.json`.

## 3. Hardware before the first pack (Dan)

1. **OUTPUT_DIS:** the gate has a pull (Dan, 2026-11-09), so the discharge FET stays off between reset and
   `MX_GPIO_Init`. The firmware never drives it.
2. **The pack read:** with every FET off, check on the schematic that no body diode conducts from VOUTa back into the
   stage. Q4's should block VOUTa → SW2 (inference from the topology, not checked).
3. **Q5/Q6:** the body diodes point toward each other (Dan), so the switch blocks both ways when off. It carries the
   full pack current when on.
4. **Per pack:** a series resistor to start, a 15 A fuse later. The QT Py comes off VBUS.
   - Installed 2026-11-09: 4S at 15.2 V, 220 mΩ series, 15 A fuse.
   - With 0.22 Ω, a 1 V mismatch at the connect is about 4.5 A. It flows only into C_int (50 µF, τ about 11 µs), since
     C_ext is on the pack side.
   - Loop (hand estimate from the gains in `regulator.c`, not measured): the limiter's crossover is
     kl·kp/(1 + kp·R), about 72 rad/s (11 Hz) at 0.22 Ω, against about 16 rad/s at 25.3 Ω (L0). The voltage
     integrator's corner is ki·R, about 6.5 rad/s, so the phase margin is about 85°.

## 4. Ladder (each step on Dan's OK)

| Step | Load | Converter at 24.4 V in | Open before it |
|---|---|---|---|
| L0 | 25.3 Ω, no pack | buck | Flash. Check: pack reads ≈ 0, start to a 9 V ceiling at 200 mA folds to ≈ 5 V, killing `run` stops within 2 s, OUTPUT_DIS stays 0 |
| L1 | 3S (≤ 12.6 V) + series R | buck | §3 items 1–4 (skipped: Dan went to 4S) |
| L2 | 4S (≤ 16.8 V) + 220 mΩ + 15 A fuse | buck | §3 items 1, 3, 4 answered |
| L3 | 6S (≤ 25.2 V) + series R | band / boost | ID_MON reads about 19 % low in boost CCM (2026-11-04), so the real current would be over the limit. Calibrate the band and boost CCM current first. |

Raise the current per pack only with temperatures logged: the free-air budget measured on 2026-10-16 is 2–3.5 W of
board loss for a 60 K rise.

## 5. Not tested

L0 ran on 2026-11-09 (`bench/results/2026-11-09/README.md` section 5):
- Boot, the connect sequence, the limit, OUTPUT_DIS held low, the keepalive stop and the Ctrl-C/SIGTERM stop all
  behaved as above.
- From L0: the limit's input read 43 mA under V/R at 200 mA in DCM. Resolved on 2026-11-09 against an ammeter: ID_MON is now 991 / +4, within 1.2 mA from 0.25 to 1 A (README sections 7-8).

L2 at 200 mA ran clean on 2026-11-09 (same README, section 6). The pack read was 15505 mV against Dan's 15.2 V; the
current is still unverified.

The floor is checked only in the averaged host model. SW_VIN_RISE is untested.
