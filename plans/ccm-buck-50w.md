# Buck forced CCM at 400 kHz, 50 W peak: the first step past 100 W (plan, 2026-10-20)

Branch `claude/ccm-hf-buck-50w`, from `claude/usb-c-epr-testing-9bt5lo` at 30bf2b5.

**Goal.** The buck runs forced CCM at 400 kHz and holds 20 V / 2.5 A (50 W) as a peak load,
efficiently and stably, from the 24 V bench supply. This is roadmap Phase 3 (`240w-roadmap.md`
section 6) moved ahead of Phase 2, because buck CCM needs no new hardware and its current sense
is quiet (SW2 static). The same modulator, at 5 A, is the 20 V / 100 W PDO.

**Status (2026-10-20 late, `bench/results/2026-10-20/README.md`).**
- **Run on hardware**, remotely, at the 25.3 Ω only: forced CCM at 200 and 400 kHz holds 5–20 V, and
  goes through contract sequences both ways, including the buck-boost handover, with no faults.
- **Reconnect steps:** the dip is half of DCM's.
- **Fixed after the first runs:**
  - Q3's bootstrap: the refresh is now 200 ns every 5 µs; 150 ns every 10 µs dropped Q3 out;
  - a pre-existing bug: the HRTIM hardware fault response was never enabled;
  - transitions: down-ramps run in DCM, CCM leaves before mode changes, entry is bumpless.
- **Open after 2026-10-20:**
  - no current sensing usable in CCM (§4.5 of the log);
  - large capacitive dumps overshoot (SW_OVP);
  - so CCM-by-load (mode 1) and SW_OCP are not yet meaningful, and a LiPo on VBUS is not safe.
- **2026-10-21 (section 8, `bench/results/2026-10-21/README.md`):**
  - **Current sensing:** the currents are means over the switching period. ID_MON reads 1.05× in CCM and is now CCM's
    current. IL_MON is not usable below ~1 A.
  - **Current limit:** a limit in the duty law holds 0.5 A into 25.3 Ω at a 20 V contract (VBUS 12.65 V, no fault).
    The PD stack detaches any fold beyond 5 %.
  - **Large capacitive dumps (switch-pulse 20 ms):** still fault. A per-period L1 bound trades the SW_OVP for an SW_UVP.
  - **LiPo:** still not safe on VBUS. The current limit and the measurement exist now; what remains is the PD-contract
    decision and the reverse-blocking hardware (section 8.4).
- **With `regulator_ccm_enable` = 0 (the default), the build behaves as the testing branch,**
  apart from these (verified at 5, 20 and 28 V):
  - the HRTIM counters are re-synchronized at a start and a mode change;
  - the hardware fault enable is now correct;
  - FLT1/FLT2 have a 0.75 µs filter.

Numbers marked *model* come from `plans/scripts/ccm_buck.py` (sources in its docstring). The rest
come from the code, the datasheets cited there, and the bench logs named.

## 1. Why now, and what limits the buck today

**The DCM ceiling.** At 200 kHz (`loss_budget.py`, *model*, 24.6 V in, rectifier on), the buck
tops out at:

| Output | Ceiling |
|---|---|
| 5 V | 8.4 W |
| 9 V | 21.6 W |
| 15 V | 37 W |
| 20 V | 31.5 W |

None of these reaches 50 W, and every buck PDO at full current is beyond DCM.

**At 400 kHz the DCM ceiling halves** (energy per pulse ∝ 1/f). The 20 V figure is 0.54 A, under
the 25 Ω's 0.8 A. So 400 kHz is only usable together with CCM. The firmware ties the two
together: the buck runs at 400 kHz only while CCM is enabled.

**The CCM boundary** is ripple / 2. Peak and valley currents at the target points:

| Point | 200 kHz ripple | 200 kHz peak / valley | 400 kHz ripple | 400 kHz peak / valley |
|---|---|---|---|---|
| 20 V / 2.5 A | 3.6 A | 4.3 / 0.7 A | 1.8 A | 3.4 / 1.6 A |
| 15 V / 3 A | 6.0 A | 6.0 / 0.0 A | 3.0 A | 4.5 / 1.5 A |
| 9 V / 3 A | 6.1 A | 6.1 / −0.05 A | 3.1 A | 4.5 / 1.5 A |

At 200 kHz the 9 and 15 V points sit on the boundary, valley ±0. At 400 kHz they are well inside
CCM. All of these peaks are under the buck comparator backstop (9 A).

## 2. Design decisions

### 2.1 Switching frequency: 400 kHz in buck, 200 kHz elsewhere

**Loss, *model*** (`ccm_buck.py --loss`; EPC2308, 20 ns dead time, Q4 refresh every 10 µs, L1
core loss calibrated to 1 W at the 2026-10-19 DCM point):

| Point | 200 kHz | 300 kHz | 400 kHz | 500 kHz |
|---|---|---|---|---|
| 20 V 2.5 A | 0.79 W, 98.4 % | 0.85 W | 0.95 W, 98.1 % | 1.05 W |
| 15 V 3 A | 1.21 W, 97.4 % | 1.17 W | 1.22 W, 97.4 % | 1.29 W |
| 9 V 3 A | 1.20 W, 95.8 % | 1.15 W | 1.19 W, 95.8 % | 1.27 W |
| 5 V 3 A | 1.02 W, 93.6 % | 1.05 W | 1.13 W, 93.0 % | 1.22 W |

- **On loss alone 400 kHz is not a gain.**
  - In CCM, Q1's hard turn-on (Q_oss·V_in, about 0.6 µJ) dominates the switching terms.
  - Core loss is small at these ripples: 0.07 W at 400 kHz against 0.14 W at 200 kHz. That
    holds over a 0.5–2 W calibration range (`--core-ref-w`); the calibration is an assumption
    stated in the script.
- **What 400 kHz buys:**
  - margin from the CCM boundary at 9–15 V / 3 A (the valley stays positive, so the turn-on
    edges and IL_MON readings stay in one regime);
  - 25 % lower peaks at 15 V;
  - half the output ripple.
- **Why not 500 kHz.** The 20 V off-time is then 320 ns, against about 330 ns needed (refresh
  slot plus two dead times plus margin).
- **The period must divide the master period.**
  - The ADC1 trigger (master timer, VD_MON's sample window) stays at 200 kHz.
  - So the buck period has to be a submultiple: 200 or 400 kHz.
  - The 8x VD_MON oversampling (3.58 µs) and its calibration are unchanged.
- **Runtime choice.** `regulator_buck_fsw_khz` (200/400) makes the frequency an A/B on the bench
  (target E2): the model's 0.15 W at 20 V is within its own uncertainty.

### 2.2 Control law: voltage mode with V_in feedforward, integral trim at the PID rate

Once per PID cycle (20 kHz):

`t_on = T · (V_ref · (1 − r) + u) / V_in`, with `u += ki · e · dt`

- r = 150 ns / 10 µs is the Q4-refresh share: L1 sees 0 V there instead of −V_out.
- ki = 2π·500 /s; integral only.

**Why not the DCM current-command loop.** In DCM the converter is a current source and the
output capacitor carries every load step until the loop answers. With 63 µs of loop delay and
16 µF of ceramics at 20 V, that is the roadmap's 7.9 V dip for 1.1 A at 28 V.

**Why voltage mode.** In CCM with the duty set by feedforward, the inductor is driven to the
right average voltage at once. A step only rings the L1–C network: the dip is about
ΔI·√(L/C), and the integrator only removes the IR drop.

**Simulation, *model*** (`ccm_buck.py --steps`, averaged plant, the PID ISR's sampling and
delay, node after the output switch):

| Step | Board ceramics only | Plus the QT Py's input ceramics | PD window |
|---|---|---|---|
| 20 V, 0.8 → 2.5 A | −0.98 / +0.63 V | −0.62 / +0.28 V | inside vSrcValid |
| 20 V, 2.5 → 0.8 A | −0.83 / +0.90 V | | inside vSrcValid |
| 20 V, 0.05 → 2.5 A (full reconnect) | −1.37 V | | inside vSrcValid (±1.5 V) |
| 15 V, 0 → 3 A (full) | −1.41 V | inside | **outside** vSrcValid |
| 5 V, 0 → 3 A (full) | −0.92 V | inside | **outside** vSrcValid |

- Full-range steps at 5 and 15 V are the worst case. PD's test steps 25 % at 150 mA/µs.
  Instantaneous 25 % steps (0 to 3 A in quarters) pass at every voltage. The largest
  deviations are:

  | Output | Largest deviation | vSrcValid limit |
  |---|---|---|
  | 5 V | 0.31 V | 0.75 V |
  | 9 V | 0.36 V | 0.95 V |
  | 15 V | 0.43 V | 1.25 V |
  | 20 V | 0.49 V | 1.50 V |

  At 5 V the dip passes vSrcNew for under 0.2 ms, inside the 5 ms the spec allows.
- The lever for full steps is output capacitance (roadmap section 4: 30 µF or more effective),
  or per-period active damping later.

**Loop margins, *model*** (`--loop`): integral-only at 500 Hz gives PM 78°, min|1+T| 0.83, at
every point. A proportional term of 0.2 crosses again near 7 kHz once the bench-side ceramics
are in the loop, so kp = 0.

**Why not peak current mode** (COMP1 + the DAC3 sawtooth, RM0440 §28.3.21). Since 2026-10-21 there is a decisive
reason: IL_MON is bandwidth-limited (§8.3). The original reasons were:
- It turns the inductor into a current source again, so it has the same load-step problem as DCM
  unless the outer loop runs per period.
- The comparator has never been seen ending a pulse on this board (`regulator_config.h`
  DCM_MAX_ON_TIME_NS note).
- It stays available as a cycle-by-cycle backstop: EEV4 still resets Q1, at 9 A in buck.

### 2.3 Legs and timing (`Core/Src/buck_ccm.c`, host-tested)

| FET | On | Off |
|---|---|---|
| Q1 (TA1) | 250 ns (Timer A CMP4) | 250 ns + t_on (CMP2) |
| Q2 (TA2) | one dead time after Q1 (Timer C CMP2), through the period boundary | one dead time before Q1 (Timer A CMP3) |
| Q3 (TB1) | one dead time after the refresh | one dead time before the refresh |
| Q4 (TB2) | Timer B roll-over (the refresh) | 150 ns later |

- **Q4's refresh lands once every 10 µs,** because Timer B's period is 10 µs: every 4th period at
  400 kHz, every 2nd at 200 kHz.
  - It falls inside Q2's on-time, so both low sides are on and L1 sees 0 V.
  - The roadmap assumed a 10 µs bootstrap hold (Q3 droop is still unmeasured).
- **Dead time** is `regulator_ccm_deadtime_ns` (default 20, clamped to 10–40 ns). The upper
  bound keeps the refresh and two dead times inside the 250 ns before Q1.
- **Longest Q1 pulse:** T − 250 − dt − 60 ns, which is 2170 ns at 400 kHz (D 0.87). 20 V from
  24 V needs D ≈ 0.84.
- **Entering and leaving CCM** only rewrites preloaded registers. Timer B's roll-over is always
  also a Timer A roll-over, so the legs never drift apart.
- **Host test** (`STM32CubeIDE/final/tests/buck_ccm/`, `make -C …`):
  - It plays the register program tick by tick at 200 and 400 kHz, dead times 10–40 ns, every
    on-time.
  - Checks: no Q1/Q2 or Q3/Q4 overlap; every gap at least the dead time; the refresh inside Q2
    with Q1 off; Q3 on whenever Q1 is on; the duty law's feedforward, integral and anti-windup.
  - Mutation-checked: removing a dead time gives 2504 failures.

### 2.4 Entering and leaving CCM (PID ISR step 5b)

- **Enter** when the DCM loop's command reaches 85 % of the DCM ceiling, with the output switch
  closed, soft-start done and pulses not skipped.
  - The integrator then starts at I·40 mΩ plus the dead-time drop, so the entry is bumpless.
  - `regulator_ccm_enable = 2` forces CCM whenever the output is connected (first light at a
    few watts).
- **Leave** for any of:
  - the filtered ID_MON load drops under 50 % of the DCM ceiling (after 20 ms in CCM; not when
    forced);
  - V_out is held more than max(5 %, 0.5 V) over the setpoint for 2 ms. A release overshoot,
    +0.9 V at 20 V in the model, decays within 1 ms, so it does not trigger this;
  - V_in rises 2 V over its value at entry (regeneration: the bench supply cannot sink);
  - soft-start, the output switch opening (but not a `switch-pulse`, whose reconnect is the
    load step), a mode change, the enable being cleared, or stop/fault.
- **After leaving:**
  - the DCM loop's integrator is preset to the load current;
  - re-entry is held off for 1 ms only, because the 400 kHz DCM ceiling is below the load that
    caused the entry.
- **No pulse skipping in CCM.** The ADC watchdog window is held open and its ISR returns: Q1 off
  with Q2 and Q3 on would pull the current negative. `snapshot` is refused in CCM for the same
  reason.

### 2.5 Protection added

- **SW_OCP (0x40).** The filtered ID_MON stays over `regulator_ccm_ocp_ma` (3.3 A) for 1 ms in
  CCM. The PDOs are 3 A. *Since 2026-10-21 (section 8):* CCM's period-mean current (ID_MON by default) over 5 A, as the
  backstop behind the current limit.
- **Unchanged:** the 9 A cycle-by-cycle comparator, the relative and absolute OVP, UVP, the
  ADM1270, and the HRTIM fault inputs.

## 3. What changed

### 3.1 Firmware (`STM32CubeIDE/final`)

- **`Core/Src/buck_ccm.c`, `Core/Inc/buck_ccm.h`:** the duty law and edge timing, with no HAL
  dependency.
- **`regulator_config.h` section 6b:** every CCM constant, with the reasoning; plus
  POWER_FILTER_SHIFT.
- **`regulator.c`:**
  - The switching period per mode. It is set with the outputs off, and all counters are then
    reset together (`hrtim_restart_counters`).
  - Steps 5a (current and power filters), 5b (enter/leave), 5c (SW_OCP) and 6c (CCM edges).
  - The leg functions now restore everything CCM changes: Timer A CMP3, Timer B PER, CMP3, CMP4.
  - `regulator_bench_ccm_pwm` (mailbox command 10): the CCM edge program at a fixed on-time,
    unpowered.
- **`regulator.h`:**
  - the runtime switches and telemetry (`regulator_ccm_*`, `regulator_fsw_khz`,
    `regulator_p_in_mw`, `regulator_p_out_mw`, `regulator_i_out_filt_ma`);
  - RegulatorCcmExit;
  - trace flag 0x100 (CCM). In CCM samples the trace's `i_cmd_ma` holds the filtered output
    current.

### 3.2 Tools

- **`bu regulator status`:** adds the CCM fields, the power and current telemetry,
  `efficiency_board_sensors` and SW_OCP.
- **`bu regulator trace`:** adds a `ccm` CSV column.
- **`bu regulator bench-ccm-pwm --t-on-ns NS [--fsw 200|400]`:** new; stop it with
  `bench-pwm off`.
- **Tests:** 57 pass (`tools/bringup/tests`).

### 3.3 Models

- **`plans/scripts/ccm_buck.py`:** loss against frequency, dead time and refresh rate, including
  L1 core loss in Vishay's IHLP form (doc 34252); loop margins; and load steps with the ISR's
  sampling.

## 4. Targets

### Efficiency

Measured as in Phase 0 (`240w-roadmap.md`): a meter at J2 and the supply current for P_in, a
meter at the load for P_out, and housekeeping subtracted from an idle reading.
`efficiency_board_sensors` is a trend only (IS_MON and ID_MON are uncalibrated).

| ID | Target | Model |
|---|---|---|
| E1 | 20 V / 2.5 A, CCM, 400 kHz: board η ≥ 95 %, converter-only ≥ 97 % | 98.1 % converter-only |
| E2 | 200 against 400 kHz at 20 V / 2.5 A and 15 V / 3 A: measured ΔP_loss within 0.2 W of the model; the default frequency follows the data | 400 kHz +0.15 W at 20 V, ±0.05 W at 15 V |
| E3 | Same point in DCM and CCM (20 V / 0.8 A at 200 kHz, rectifier on): CCM loss ≤ DCM loss | — |
| E4 | Dead time 10 / 20 / 30 / 40 ns at 20 V / 2.5 A: the loss minimum, and SW1 with no hard-commutation ringing past the 2026-10-19 levels | — |
| E5 | 50 W for 60 s, then to steady state, temperatures as on 2026-10-16: hot spot ≤ +60 K | ≈ 1 W converter loss + housekeeping, under the measured 2–3.5 W budget |

### Stability

These map onto `plans/stability-requirements.md`; new rows in its section G.

| ID | Target | Model |
|---|---|---|
| S1 | 20 V CCM holds 10 s at 0.8 A and at 2.5 A: no limit cycle; V_out σ ≤ 50 mV in the trace; no line near 3.9 kHz or fsw/2 | — |
| S2 | Reconnect (`switch-pulse`) 0 → 2.5 A at 20 V: inside vSrcValid, ≤ 5 ms outside vSrcNew | −1.37 V |
| S3 | Release 2.5 A → 0 at 20 V: under +5 %, no fault, no OVER_BAND exit | +1.36 V with board ceramics alone: **over** +5 %; +0.80 V with the bench-side ceramics |
| S4 | DCM ↔ CCM: one entry per load rise and at most one exit per load fall (`ccm_entries`/`ccm_exits`), V_out within 2 % across each | — |
| S5 | No VIN_RISE exit and V_in ≤ 26 V through a 20 → 5 V down-ramp with 25 Ω | — |
| S6 | The trace's ring after S2 matches the L1–C model (frequency within 20 %), as the check that the plant is what the model says | 10–18 kHz, depending on the bench-side capacitance |

## 5. Load for 50 W at 20 V

**Total needed:** 8 Ω, or 2.5 A.

**Proposed:**
- the 25.3 Ω on VBUS (15.8 W);
- a second 25 Ω in parallel (another 15.8 W): roadmap open decision 2;
- the QT Py buck for the rest, about 18 W into its 10 Ω ballast. It held 25 W for 6 s on
  2026-10-19 under `BENCH_10R_OVERLOAD_25W`.

The two resistors alone (1.6 A, 32 W) are the intermediate step.

**The step stimulus** is the output-switch reconnect (`bu regulator switch-pulse`). Q5/Q6 turn on
slowly (`bench/README.md`), so the measured step is softer than the model's.

## 6. Bench ladder

Each step needs Dan's OK before it runs. **Stop and report on any surprise** (hw-bringup rule).

**0. Flash the default build** (CCM off) and repeat a 2026-10-17 hold at 5, 20 and 28 V with
25 Ω. Compare V_out σ, I_cmd and `ccm_entries` = 0. This proves the default path unchanged on
hardware.

**1. Gate check, power stage unpowered.**
- Command: `bu regulator bench-ccm-pwm --t-on-ns 2100 --fsw 400`, then `--t-on-ns 300`, then
  `--fsw 200`.
- Dead times: `regulator_ccm_deadtime_ns` 20, then 10.
- Scope PHASE_1_P/N and PHASE_2_P/N:
  - the dead time on all four edges;
  - Q4 high for 150 ns once every 10 µs, inside Q2's high time;
  - Q3 low only around it.
- Then `bench-pwm off`.

**2. First light.**
- Settings, while IDLE: `regulator_ccm_enable = 2` (forced), `regulator_buck_fsw_khz = 400`.
- Contract: 5 V with the 25.3 Ω (1 W).
- Check: `status` (ccm_active, fsw_khz 400, on-time ≈ 0.21 × 2.5 µs), and SW1 and SW2 on the
  scope.
- Then 9, 15 and 20 V: 3.2, 9 and 16 W.

**3. CCM by load (`enable = 1`).**
- At 20 V, add the second 25 Ω (32 W), then the QT Py in steps to 50 W.
- Per step: P_in, P_out, temperatures right after, a trace at `--trigger now`, and SW1/SW2.
- Covers E1 and S1.

**4. Steps.** `switch-pulse` at 20 V at 0.8, 1.6 and 2.5 A, n ≥ 5 each, with `trace-arm
--trigger switch`. Covers S2, S3, S4 and S6.

**5. A/B and tuning:** frequency 200/400 (E2), dead time (E4), DCM against CCM (E3).

**6. Thermal at 50 W** (E5).

**7. Down-ramp** 20 → 5 V with load, VS_MON logged (S5).

## 7. Risks and open items

- **ID_MON and IS_MON are uncalibrated.** CCM's exit threshold and SW_OCP use ID_MON. Check it
  against a meter at 0.8 and 1.6 A before step 3. If it reads low, OCP is late; if high, CCM
  exits early (it then re-enters after 1 ms, which shows in `ccm_exits`).
- **The Q3 bootstrap hold is assumed, not measured** (10 µs here, 4× the period).
- **The full-range release overshoot** at 20 V (S3) and the full steps at 5 and 15 V exceed the
  PD window with the board ceramics alone. More output capacitance is the fix; it is already in
  roadmap Phase 4.
- **Changing `regulator_ccm_enable` or `regulator_buck_fsw_khz` while running.** The frequency
  only follows at the next start or mode change. Clearing the enable at 400 kHz therefore leaves
  the buck in DCM at 400 kHz, where 20 V cannot carry 0.8 A. Change both while IDLE.
- **Next, toward >100 W:**
  - 20 V / 5 A in buck uses this modulator unchanged (peak 5.9 A at 400 kHz, model 97.8 %), with
    a third 25 Ω or a bigger load;
  - boost CCM (roadmap Phase 2) needs the same leg structure mirrored: Q1 held, Q2 refresh
    inside Q4's on-time.

## 8. Current sensing and the current limit (2026-10-21)

Bench: `bench/results/2026-10-21/README.md`. Host test: `STM32CubeIDE/final/tests/buck_ccm/` (`make -C …`).

### 8.1 Period-mean currents (`adc_monitor.c`)

- **ADC2:** IL_MON (PA1, ADC12_IN2) and ID_MON (PC1, ADC12_IN7) moved here, auto-injected (JAUTO) after VS_MON on the
  same software start.
  - Each is 16 conversions of 25 ADC cycles (9.4 µs).
  - The sequence ends 32.8 µs into the 50 µs PID cycle, and the next cycle reads it.
- **ADC1:** IS_MON is alone on the injected group, 8× (4.7 µs).
  - It now costs one VD_MON trigger per PID cycle: 9 samples instead of 10, and VOUT_MEDIAN_SAMPLES is 9.
  - The HRTIM-triggered injected start of 2026-10-20 is gone.
- **Why 25-cycle conversions:**
  - No power-of-two ratio of the available conversion times makes a window that is a whole number of both the 200 and
    400 kHz periods (that needs 425 ADC cycles).
  - 25-cycle spacing spreads the samples over the period best of what fits. The ripple fundamental is attenuated to
    0.063–0.067 of its amplitude: ≤ 0.1 A at 200 kHz, ≤ 0.05 A at 400 kHz, as a fixed bias.
- **Bench, forced CCM at 400 kHz, 5–20 V into 25.3 Ω:**
  - ID_MON = 1.050 × V/R + 24 mA. The offset includes the QT Py's idle draw.
  - IS_MON gives P_out / P_in 0.75–0.93, including housekeeping.
  - IL_MON reads 0.35–0.84 × the clipped-triangle mean, and 0.51× in DCM. It needs a probe before it is trusted, and
    so do the COMP1 thresholds in amps.
- **CCM's current** (limit, SW_OCP, light-load exit, DCM restart) is `regulator_ccm_ilim_src`. The default is 1:
  ID_MON × `regulator_id_gain_permille` (952).
  - IL_MON stays available. In CCM it is corrected for the INA281 clipping its negative valley
    (`buck_ccm_unclip_a`).
  - ID_MON sees the output, after the board ceramics, and reads to 5.5 A.

### 8.2 The limit (`buck_ccm.c`)

- **Law:** the duty is built on min(V_ref, v_lim).
  - v_lim starts at the measured V_out once I > I_lim.
  - It moves as v_lim += kl (I_lim − I) dt.
  - It hands back when it reaches V_ref.
  - The voltage integrator holds while v_lim is the lower (anti-windup).
- **Gain:** kl = 400 V/(A s), runtime `regulator_ccm_ilim_kl`.
  - At the PID rate the CCM buck is a voltage source, so the loop gain is kl / the load's incremental resistance.
  - kl is set for a battery behind the output switch (about 45 mΩ: 0.45 per PID cycle). 800 V/(A s) rang there in the
    host test.
  - Into 25.3 Ω the fold is slow by design: model 63 ms time constant, bench about 50 ms.
- **Rejected:** a current PI referenced to the measured V_out. The delay of the V_out mean made it the same voltage
  source, and its second integrator rang with 25 Ω (ζ 0.07, host test).
- **Duty resolution:** the CCM on-time is in HRTIM ticks (184 ps). 1 ns steps are 9.6 mV at 24 V, which is a 0.2 A
  dither into a battery.
- **Defaults:**
  - `regulator_ccm_ilim_ma` 3000, the PDO current.
  - SW_OCP 5000 mA on the filtered CCM current: the backstop behind the limit. A 2 → 4 A step into a 3 A limit takes
    25 ms to settle.
- **Host test:** an averaged plant with the ISR's sampling. It reproduces the 2026-10-20 switch-pulse: 14.8 / 23.2 V
  against −5.2 / +2.9 V on the bench. It checks:
  - the 25.3 Ω fold and its release;
  - batteries of 10–200 mΩ, both under a contract change and hot-plugged;
  - a step through the limit;
  - no action under the limit;
  - the integrator freeze;
  - the clipping inversion.

### 8.3 Per-period L1 bound (`regulator_ccm_peak_ma`, off by default)

**Not usable on this hardware (Dan, 2026-10-21).** A scope capture of IL_MON against the modeled inductor current
showed the INA281's bandwidth limiting the signal. That explains the readings: 0.35–0.84× and nonlinear in CCM, and
0–150 counts mid-pulse in DCM. It means:
- peak current mode is ruled out on this board;
- so is any per-period bound through COMP1;
- COMP1's thresholds in amps, including the 9 A backstop, are not a reliable protection. The ADM1270 input trip and
  the software limits are what bound current.

A faster current-sense amplifier would be a hardware change. The capture itself is Dan's and is not in the repo.

- **What it does:** COMP1's threshold in CCM, so Q1 ends early when L1 reaches it. Q2 still waits for its programmed
  edge; until then Q2's reverse conduction carries L1.
- **Model:** it is the only mechanism fast enough for the L1–C ring of a capacitive dump. At 2–3 A average the dump's
  peak drops from 23.2 V to 20.8–21.5 V.
- **Bench, 3500 mA, switch-pulse 20 ms at 20 V:** SW_UVP. V_out fell 20 → 10 V in 150 µs while recharging the drained
  VBUS side.
  - That capacitance is far above the model's 8 µF.
  - L1 seemed to deliver only about 1 A, so the comparator may act below its nominal level.
- Left off. The fix needs the VBUS-side capacitance measured (register F6), and then one of:
  - a UVP that waits while L1 is bounded;
  - a higher bound;
  - more output capacitance.

### 8.4 Before a LiPo on VBUS

1. **The contract against the limit.** The PD stack detaches when VBUS leaves ±5 % (`USBPD_DPM_IsPowerReady`), and
   `pd_vbus.c` adds a Hard Reset at 8 % for 200 ms.
   - **Decision (Dan, 2026-10-21): PPS.** Current limiting is part of a PPS contract: the sink requests an operating
     current and the source folds the voltage in current-limit mode, so the CCM limit of 8.2 becomes the PPS
     current limit and the ±5 % check does not apply while limiting.
   - Not chosen: a fixed contract at the pack voltage; a bench-only exemption.
2. **Hardware** (log 2026-10-20 section 6):
   - reverse blocking, because OUTPUT_DIS bleeds VBUS in IDLE/FAULT and the 5 V start back-feeds;
   - a connect step that waits for V_out ≈ V_pack.
3. **Exits to DCM drop the limit.** DCM's command is capped only by its ceiling. Capping it at the limit as well is
   untested.
4. **ID_MON range:** 5.5 A full scale.

