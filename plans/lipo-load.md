# LiPo as a power sink (started on branch `claude/lipo-load`, 2026-11-09)

**Why.** Dan: a LiPo on VBUS as a load that can sink a lot of power, on its own branch with no USB at all. Charging is
managed so the pack is never full; this is not a charger (no termination, no balancing).

**Base.** `claude/ccm-hf-buck-50w` at d427689 (PPS, the shared CCM/DCM current limiter, the ID_MON CCM calibration, the
start that waits for V_in).

## 1. Build

- `cmake --preset Battery` (`build/Battery`); `bu` points there by default (`tools/bringup/bringup.toml`). The PD
  builds are `cmake --preset Debug` and `cmake --preset EprPps` (`build/Debug`, `build/EprPps`).
- `BATTERY_LOAD` is a CMake option with no default (2026-11-15, branch `claude/regulator-epr-pps`): a configure
  without it stops, so a build directory from before it was an option (`build/BatteryNext`) cannot quietly turn into a
  PD build with OUTPUT_DIS live. Reconfigure such a directory with `-DBATTERY_LOAD=ON`. With it ON, any PD option ON
  stops the configure, and `PD_VBUS_PATH_CHARGER` defaults to OFF.
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

## 7. CCM entry limit slew (2026-11-13, bench README section 10)

Ceramics only (Dan, 2026-11-13: no bulk C; if the G474 loop cannot cope, the next step is the STM32G5). At a CCM
entry with the limit engaged, the limit starts at the measured current and rises at
`regulator_ccm_ilim_slew_ma_ms` (50 A/s in the battery build, 0 in PD so a PD load step meets the full limit), and
the voltage integrator runs while it climbs. Bench: the 5 A forced entry that tripped SW_OCP on 2026-11-12 held 5 A
for 20 s (ammeter 5003 mA), trace peak 5043 mA.

The entry itself: readings taken across the mode change moved (VD_MON about +0.3 V within 0.1 ms) and the closed
loop chased them, so the current dipped to ~0 A for 8 ms and rang to 3.3 A for ~25 ms before the slew. Rather than
correct for that reading (an observation, not a property of the hardware as built), the entry now flies blind for
a short window (Dan, 2026-11-13): for `regulator_ccm_entry_blind` PID cycles (20 = 1 ms in the battery build, 0 in
PD until it is run there) the duty is the entry's guess, V_out at the entry plus u0, with only the V_in feedforward
live, and u and v_lim hold. The load cannot move much in a millisecond. At the end the limit is re-seated on the
V_out read in CCM, the slew starts from the current read in CCM, and u takes up the difference so the first
closed-loop duty equals the window's (bumpless). ID_MON over the limit ends it early; SW_OCP and the hardware trips
stay armed throughout. Host model (`test_boost_entry_blind`): for a reading jump of −0.3 to +0.3 V and a guess
0.1 V off either way, within 0.05 A over the limit and 0.26 A under the entry current, against 2.2 A over and 4.1 A
under without the window. 20 cycles covers ADC2's first whole ID_MON scan in CCM (one every 0.56 ms).

Mid-run limit changes (same build): every limit increase in CCM now climbs at the same slew, not only the entry;
decreases act at once (`BUCK_CCM_SLEW_DOWN` 0). A climb that starts while the limit holds V_out below the reference
re-seats v_lim on V_out and hands the difference to u (bumpless), and u runs during the climb, so the hand-back to
the voltage loop at the end of it starts from a current u (the stale-u overshoot of the host model: +0.62 A at a
0.17 Ω path). Host model (`test_boost_limit_steps`, pack plant): up steps within 0.05 A over the new limit, down
steps without undershoot; an entry at 0.6 A into a 0.3 A limit peaks 1.35 A (the entry guess, ended by the guard).
New trace trigger `mode` (5): fires on the first regulator mode change after the arm.

## 8. Toward a laptop on 48 V / 5 A (2026-11-13, host model and bench suites)

Host model (`test_laptop_48v`, not run on the bench): boost CCM from 24.4 V to 48 V, board ceramics 4 / 9 / 15 µF at
bias, laptop input C 10 / 47 / 100 µF, cable 0.05 Ω with 0.3 / 1 µH, standby 0.2 A, PD settings (limit 5.25 A, no
slew, no window), 0.1 µs steps.

| Load edge 0.2 → 5 A | Laptop V | Release to 0.2 A, max | Unplug at 5 A, board max |
|---|---|---|---|
| 1 A/ms | 47.58–48.02 V | 48.15 V | 60.31 V |
| 10 A/ms | 46.48–48.02 V | 49.32 V | 60.28 V |
| step | 41.43–48.75 V, 1.1 V p-p ringing | 54.97 V | 60.28 V |

- Edges up to 10 A/ms stay within ±5 % with no ringing (the test checks these).
- A hard step droops the laptop to ~41 V on ceramics: at 20 kHz the loop answers one PID period later, and ~9 µF
  at 5 A falls ~2.8 V per 5 µs switching period before then. Raising kp to answer faster destabilizes the 20 kHz
  loop in the model. This is charge on the capacitor before any controller can act, so it is not only a loop-rate
  (G5) question.
- Unplug at 5 A overshoots the board to ~60 V. Output OVP in CCM is software only (3 PID cycles at 110 % or 52.8 V)
  and the VD_MON AWD skip is open in CCM, so nothing cuts the switching cycle by cycle. Candidate: the G474's
  comparators with DAC thresholds as HRTIM fault / external events for a cycle-by-cycle output OVP (not built).

What the battery bench can and cannot show toward that case:

- Can: the DCM → CCM entry and exit, limit steps and ramps up to 5 A into a stiff sink (`transitions.py pack`), and
  voltage slews across V_in through buck / buck-boost / boost with a resistive load (`transitions.py resistor`).
- Cannot: 48 V (battery-build ceiling 34 V), 240 W (the 25.3 Ω resistor draws 1.9 A / 92 W at 48 V; the pack sits at
  ~31 V), a fast load step or unplug (switch pulse refused in the battery build, e-load removed), PD / EPR
  negotiation (PD build), thermals at 160–240 W, and ID_MON headroom at 5 A (full scale ~5.3–5.5 A).

## 9. CCM to CCM across the mode and kind change (2026-11-14, bench README 2026-11-13, resistor suite)

`regulator_ccm_kind_switch` (1 battery, 0 PD): on the way up a running CCM goes straight to the next kind instead
of exit → ~1 ms DCM → re-entry. Buck CCM reaching buck-boost: the mode changes in place (no output off, no counter
restart; the band / boost edges rewrite every Timer A / B source the legs use, one preloaded set) and band or boost
starts from an entry's u0 guess under the 1 ms open-loop window. Band → boost: the same law with n in output
volts, so u and the limit carry over and only the edges change. Downward changes, a different switching period,
or a buck Timer B longer than one period exit as before. Bench: the DCM dips (1.2–1.9 V, 3.6 V forced) are gone.

The switch then waits for the law's own reference (the limit's v_cmd), not the ramp setpoint: after an entry's
re-seat the reference ran about 1 V behind an 80 V/s ramp, and boost chosen on the setpoint sat at its shortest
pulse, V_out 0.8–1.0 V over the reference for 5–6 ms. Until the reference gets there the running kind holds.

## 10. CCM downward: boost → band → buck (2026-11-14, bench README 2026-11-13 10:49 and 2026-11-14)

`regulator_ccm_down` (1 in both builds since 2026-11-15; PD not yet run with a sink): a falling setpoint no longer exits CCM (RAMP_DOWN only when it is 0). The
exit dated from 2026-10-20, before entries were bumpless; at the converter's load currents C·dV/dt at 80 V/s
(~4 mA on 50 uF) is small, so CCM can follow the ramp down. A running boost goes to band when the law's reference
(min(v_cmd, setpoint)) says band or nothing, or when its on-time sits at the shortest pulse for CCM_FLOOR_CYCLES (3)
with V_out over the reference: boost and band are one law, so u carries over (mode changes in place when the mode
leaves BOOST). Band goes to buck when the reference is under buck's reach (V_in × max on / period, CCM_DUTY_ENTER_PCT)
and band is at its floor, says nothing, or the mode is leaving buck-boost: buck starts from an entry's u0 guess
under the 1 ms open-loop window, and the buck leg's static Timer B and CMP1 are rewritten. Bench (resistor): band →
buck at about 22.26 V with no CCM gap and within −0.13/+0.17 V of the reference; the 0.9–2.7 V DCM dips are gone.
Auto still leaves CCM in buck at light load (LIGHT_LOAD near 20 V into 25.3 ohm), as designed.

## 11. One branch for both builds (2026-11-15, branch `claude/regulator-epr-pps`)

- `BATTERY_LOAD` is a CMake option with no default (section 1). Presets: `Battery` (no USB, mailbox START only),
  `Debug` / `Release` (USB-PD), `EprPps` (USB-PD with VCONN, 15/20 V, SPR PPS and EPR 28/36/48 V).
- The PD build now takes the battery-validated CCM defaults: `regulator_ccm_down` 1, `regulator_ccm_kind_switch` 1,
  `regulator_ccm_entry_blind` 20 (1 ms). None of them has run with a PD sink yet.
- `regulator_ccm_ilim_slew_ma_ms` stays 50 A/s in the Battery build and 0 in PD (Dan, 2026-11-15): a laptop load
  step at CCM entry would otherwise meet a limit that starts at the entry current and climbs for up to ~100 ms.
- The Battery preset's flash image is byte-identical to `build/BatteryNext` (the image on the board on 2026-11-14).
