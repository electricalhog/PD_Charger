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
| Ceiling | `regulator_battery_ceiling_mv`, 5–34 V (`BATTERY_CEILING_MAX_MV`; 25 V until 2026-11-12). SET_VOLTAGE changes it. | Set it below the pack's full voltage. |
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
| L4 | 8S (24.0–33.6 V) | boost; band near empty | Prepared 2026-11-12, not run (`bench/results/2026-11-12/README.md`): the ceiling to 34 V, boost's integrator preset −440 mV, `boost_cal.py` (the resistor phase first, with no pack, then the pack with an ammeter). |

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

## 6. 5 A and a continuous DCM-to-CCM current ramp (2026-11-12, flashed and run: bench README section 9)

**SW_OCP at ID_MON's full scale (Dan, 2026-11-12).** SW_OCP watches ID_MON, whose ADC full scale is 5.5 A
(12 mΩ × 50 V/V into 3.3 V), so a level over about 5.45 A never trips. The BatteryNext build caps the level at
`regulator_ccm_ocp_fs_ma` = (5500 − 150 mA raw) after 991 / +4 ≈ 5.3 A, and the battery build's default is that
level alone (`CCM_OCP_MA_FULL_SCALE`). A current past what ID_MON reads still trips, since the reading sits pinned
over the level. The 150 mA (90 mV) is headroom for the INA281's output swing under its supply, which is likely the
same 3.3 V the ADC reads against (inference from the schematic, not measured). `boost_cal.py` no longer lowers
SW_OCP per step: every CCM run is at full scale, refused on an older build where 0xFFFF would turn SW_OCP off. The
script's own stop level (2 × the limit, at most 7500 mA, on ID_MON or the host-side V_in × I_in / V_out) is the
backstop at lower currents.

**The ramp, firmware side (step 5d0).** `regulator_ccm_ilim_ramp_ma_s` (0 off) moves `regulator_ccm_ilim_ma`
toward `regulator_ccm_ilim_target_ma` every PID cycle, so the limit is continuous at 20 kHz and the host writes
nothing while it runs. Choices:
- The ramp moves the limit, not the voltage ceiling. The ceiling stays at the pack read plus the margin for the top
  current. Everything downstream (the limit law in DCM and CCM, the CCM entry ramp, telemetry) is unchanged.
- Integer state in mA × cycles/s, so slow rates do not stall in float rounding. It never writes 0, which means no
  limit.
- Stop and fault clear the rate, so a ramp is armed per run and never carries into the next START.
- PPS writes `regulator_ccm_ilim_ma` directly; with the rate at 0 (the default) nothing changes for PPS.

**The ramp, host side (`boost_cal.py ramp FROM TO --rate --hold`).** One pack read, one START at FROM, then three
SWD writes: arm (target TO, rate), turn (target FROM) after the top hold, and nothing else. Auto CCM (the default)
makes the run cross the handover by itself: boost-kind CCM over buck-boost DCM enters at a DCM command of 400 mA and
leaves under 200 mA (CCM_BOOST_ENTER_MA / _EXIT_MA), so FROM defaults to 150 mA to see the exit on the way down.
The regulator trace (trigger `ccm`) is armed 0.5 s before the ramp starts and records the entry at 20 kHz: in CCM its
current column is ID_MON per cycle. The script's stop level follows the limit of the moment (max(2 ×, + 1000 mA),
at most 7500 mA), and after the run it reports the entry and exit points, ID_MON minus the limit per segment, mode
changes, CCM entry / exit counts and the exit log.

**What it should show.** One entry and one exit per run (the 1 kHz MODE cycle is fixed: 3 entries for 3 steps on
the re-test). The entry near 400 mA, where the entry overshoot is small, then CCM all the way to TO. That avoids
the ~500 mA overshoot of a cold CCM entry at 4.5–5 A (the two SW_OCP trips of 2026-11-12). Tracking error under
the limit's law with the ramp's lag at 100 mA/s.

**Risks, not measured.**
- Heat: at 5 A into 32 V the converter carries about 160 W. The 4S ramp measured 92–95.6 % in CCM up to 69 W, so the
  loss at 160 W is likely 5–10 W, over the 2–3.5 W free-air budget (§4). The default ramp spends about 25 s over
  4 A. Board temperatures are not logged by the script.
- The forced 500 mA SW_OCP of 2026-11-12 (1.87 A at a 500 mA limit, 0.2 s in) is unexplained; an auto entry near
  400 mA is close to that case. The ccm trace captures it if it recurs; SW_OCP at full scale will not stop it, the
  script's stop level (1.4 A at that limit, 3 samples) will if it lasts.
- At 5 A the limit sits about 300 mA under SW_OCP. A cold CCM entry at 4.5–5 A overshot about 500 mA tonight, so the
  forced 5 A step may trip; the ramp enters near 400 mA instead.
