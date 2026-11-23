# Roadmap to 240 W (plan, 2026-10-16)

Goal: every PDO of a 240 W EPR source, up to 48 V / 5 A, on this board. The board is 122 × 47 mm,
4-layer 1 oz FR4, GaN in 3 × 5 mm QFN, with no heat sink and no airflow. It has EPC2308 (150 V)
fitted now; EPC2302 (100 V, the part the board was drawn for) goes in once the CCM firmware is
proven. The output has ceramics only: the 82 µF electrolytics (C14, C15) are not fitted.

Numbers marked *model* come from `plans/scripts/loss_budget.py` (its docstring lists every source
value). The rest come from the 2026-10-15 reads (`bench/results/2026-10-15/metrics/steps.json`),
the code, the KiCad sheets and the datasheets cited.

## 1. Where it stands

- **What has run.** The highest load so far:
  - 31 W continuous (28 V into the 25 Ω);
  - 52 W for 2 s (36 V).

  All of it was DCM.
- **The DCM ceiling.** The present modulator is DCM only. Its ceiling comes from the period
  window (4450 ns after the refresh, its dead time and the margin) and the peak limits
  (`regulator.c` step 5). *Model*, at V_in 24.6 V with the rectifier on:

  | Contract | Mode | Ceiling | 25 Ω load | PDO at full current |
  |---|---|---|---|---|
  | 5 V | buck | 8.4 W | 1 W | 15 W |
  | 9 V | buck | 21.6 W | 3.2 W | 27 W |
  | 15 V | buck | 37 W | 9 W | 45 W |
  | 20 V | buck | 31.5 W | 16 W | 100 W |
  | 28 V | buck-boost | 47 W | 31 W | 140 W |
  | 36 V | boost | 81 W | 52 W | 180 W |
  | 48 V | boost | 96 W | 92 W | 240 W |

  Every PDO at full current is beyond DCM. CCM is required, not an optimization.
- **The model overstates delivery.** The command needed 1.23× the load current at 28 V and 1.48×
  at 36 V (run E, rectifier on). Input power was not logged, so the gap is not yet split into
  loss and model error.
- **48 V into 25 Ω cannot hold with this modulator.** In model units, 92 W needs 113–136 W
  against a 96 W ceiling (10 A boost limit), or 124 W with a 12 A limit. That is the same
  saturated corner the 36 V burst sat in.

## 2. The 36 V oscillation: what it probably is

**The frequency matches the loop delay.**
- 3.92 kHz ≈ 1/(4 × 63 µs). The loop's plant is C dV/dt = I_cmd − I_load, an integrator, plus
  about 63 µs of delay (measured 2026-10-12, `regulator_config.h` LOOP_*). Its phase reaches
  −180° at 1/(4τ) = 3.97 kHz.
- The same 3.9 kHz limit cycle appeared on 2026-10-12 at 20 V, no load, with fc = 300 Hz.

**What sets the gain there.** The loop gain at that frequency is about
(fc / f) × (C_model / C_seen) × g, where g is the modulator's incremental gain.
- The gains are computed for C_int + C_ext = 300 µF, but on 2026-10-12 the loop saw about 23 µF
  at 3.9 kHz.
  - At fc 300 Hz that gives 0.98: the limit cycle.
  - At fc 100 Hz it needs C_seen / g ≈ 8 µF to oscillate.
- **Ceramics under DC bias (2026-10-17).**
  - The electrolytics are not fitted. The output capacitance is 5 × CL31B106KBHNNNE (10 µF,
    50 V, X7R, 1206).
  - Samsung's bias curve takes each to about 3.2 µF at 20 V, 2.0 µF at 28 V, 1.5 µF at 36 V
    and 1.2 µF at 48 V.
  - With fc 100 Hz and gains for 300 µF, the loop gain at 3.97 kHz is:

    | V_out | 5 ceramics | Loop gain |
    |---|---|---|
    | 20 V | 16 µF | 0.47 |
    | 28 V | 10 µF | 0.76 |
    | 36 V | 7.5 µF | 1.01 |
    | 48 V | 6 µF | 1.26 |

  - 36 V sits at 1, where it oscillated. 48 V under load should oscillate too. At fc 300 Hz and
    20 V the gain is 1.4, the 2026-10-12 limit cycle.
  - The bench VBUS side adds a little, and the modulator gain g is not 1, so these are
    first-order.
- **The ceiling in the fast path may add to it.** It is recomputed every PID cycle from the measured
    V_out and V_in. At 36 V it scales about 1:1 with V_out. It was active: the on-time read was
    1383 ns against a window limit of about 1360–1450 ns for the V_in and V_out read, and one
    read had the command above the ceiling read beside it.

**Tests.** Each needs a runtime write or a trace (ask first). Test 2 is the direct check of the
capacitance explanation.

**Revised (2026-10-17, later).** The QT Py's input is about 100–120 µF (user), on the bench side
of the closed switch.
- If those are ceramics like the board's, the linear model gives a loop gain at −180° of 0.30
  at 36 V and 0.44 at 20 V / 300 Hz (`loop_model.py --load-caps 1.1`). That predicts neither
  oscillation.
- So the capacitance explanation holds only if that capacitance mostly disappears at 20–36 V
  (low-voltage parts), or if the 2026-10-12 limit cycle ran with the switch open.
- Otherwise the 36 V oscillation is the nonlinear one: the command pinned at the ceiling, which
  moves with V_out every PID cycle. Test 1 (trace i_cmd against i_max) decides it.

**Model (2026-10-17, `plans/scripts/loop_model.py`).**
- **The model:** DCM as a current source into the VBUS network; the PI as coded (kp, ki from
  fc 100 Hz and 300 µF); 63 µs of delay; the board's five ceramics derated for DC bias.
- **It fits:**
  - 20 V at fc 300 Hz oscillates at 3.93 kHz;
  - 20 V and 28 V at fc 100 Hz hold (28 V with a 35° margin);
  - 36 V sits at a 13° margin, a loop gain of 0.89 at −180°.

  The fit requires the QT Py's input ceramics to add at most about 6 µF.
- **It also says the regulator is soft.**
  - kp sized for 300 µF is 0.19 A/V. Against the real 10 µF and a 25 Ω load, that gives about
    4–5 Ω of closed-loop output impedance from 25 Hz to 2 kHz, peaking at 14 Ω near 3.9 kHz
    at 28 V.
  - A 1.1 A step should dip VBUS about 7.9 V at 28 V. A PD-size step (5 A) is far outside what
    this loop can hold. Phases 1 and 2 have to fix this.
- **Untested.** The 3.9 kHz peak has not been measured. On 2026-10-17 the QT Py no longer
  converted, so the load steps came from the output switch closing after a start, at 5 V only
  (`bench/results/2026-10-17/`).
- **What can make one without hardware:** a firmware step or sine added to the PI's output (the
  delivered current), logged at the PID rate with a triggered freeze and averaged over
  repetitions. From the node's side, a current step there is a load step.
1. Trace i_cmd, i_max and v_out at the PID rate through a 36 V burst. If the command rides the
   ceiling at 3.9 kHz, the ceiling is in the loop.
2. At 36 V, set `regulator_c_ext_uf` 250 → 0 (kp ÷ 6). Prediction: the 3.9 kHz goes away.
3. Measure C from an up-ramp (the method in the `regulator_config.h` LOOP_* note), and repeat 36 V
   with the rectifier off.

## 3. Loss per switching period

At 200 kHz, 1 µJ per period is 0.2 W. A 5 W board budget is 25 µJ per period.

*Model*, forced CCM, 24 V in, EPC2308, 15 ns dead time, static leg refreshed every 8th period.
Core loss, copper and capacitor ESR are left out.

| PDO | Mode | I_L avg | Ripple p-p | I_pk | µJ/period | W | η |
|---|---|---|---|---|---|---|---|
| 5 V 3 A | buck | 3.0 A | 4.2 A | 5.1 A | 3.9 | 0.78 | 95.0 % |
| 9 V 3 A | buck | 3.0 A | 6.0 A | 6.0 A | 3.9 | 0.77 | 97.2 % |
| 15 V 3 A | buck | 3.0 A | 6.0 A | 6.0 A | 3.9 | 0.78 | 98.3 % |
| 20 V 5 A | buck | 5.0 A | 3.5 A | 6.8 A | 9.3 | 1.87 | 98.2 % |
| 28 V 5 A | boost | 5.9 A | 3.6 A | 7.8 A | 12.5 | 2.51 | 98.2 % |
| 36 V 5 A | boost | 7.7 A | 8.5 A | 11.9 A | 18.6 | 3.73 | 98.0 % |
| 48 V 5 A | boost | 10.3 A | 12.8 A | 16.6 A | 30.5 | 6.09 | 97.5 % |

**One period at 48 V / 5 A** (30.5 µJ):

| Term | µJ | W |
|---|---|---|
| Input path: Q7 and Q8 (SiR5607DP) and R25 (5 mΩ as built), at 10.3 A | 10.3 | 2.06 |
| GaN conduction: Q1 static, Q4 and Q3 switching | 7.5 | 1.50 |
| L1 DCR + R5 | 5.9 | 1.18 |
| Output path: Q5, Q6 and R6 | 3.3 | 0.66 |
| Q4's hard turn-on at the valley (Q_OSS·V plus overlap) | 2.0 | 0.40 |
| Dead time, turn-off, refresh, gate drive | 1.5 | 0.29 |

- **Today's settings cost about 1 W more.** With the present 50 ns dead time and a refresh every
  period, the same point is 35.5 µJ (7.1 W).
- **EPC2302 (the planned part) in place of EPC2308:**
  - about 0.5 W better at 48 V / 5 A (27.9 µJ, 5.6 W);
  - about 0.1 W worse at the 3 A PDOs, from its larger Q_OSS (85 against 37 nC at 50 V);
  - R_θJB 1.5 against 2.8 °C/W.

  It is a 100 V part (120 V repetitive transient). At 48 V out, SW2's overshoot sets the margin.
  On 2026-10-16 SW2 read up to 41 V at 28 V out, with an unchecked probe ground.
- **Not modeled:**
  - L1 core loss, which is largest at 48 V with 12.8 A p-p ripple (Vishay's IHLP calculator, or
    measure it);
  - 1 oz copper: each mΩ at 10.3 A is 0.11 W;
  - capacitor ESR. Boost output-capacitor ripple is 5.9 A rms at 48 V / 5 A and 4.3 A rms at
    36 V: about 1.2 A per ceramic. Samsung's ripple curve gives a few °C of self-heating for
    that, so heat is not the limit; capacitance is (section 4).

**Free-air budget.**
- **Measured (2026-10-16, `bench/results/2026-10-16/`).** At 28 V, rectifier on, DCM, still air
  at 21.4 °C, the power stage soaked to 65–69 °C:
  - Q3/Q4 hot spot +47.5 K, L1 +45 K, Q1/Q2 +44 K;
  - board loss 1.7–2.7 W, housekeeping included (the two readings differ by the output-current
    method).

  That is **about 17–28 K/W at the hot spot, so a 60 K rise is about 2–3.5 W.**
- **Caveats:**
  - In this DCM point, L1 is a large share of the heat: its copper loss is about 0.1 W, so its
    heat is mostly core loss.
  - CCM spreads the loss differently.
  - Settling has a slow part (tens of minutes). No cool-down was taken.
- **The estimate it replaces** was 5–7 W: convection plus radiation over 115 cm², about 15 W/m²K,
  derated for spreading.
- **One old data point:** the 1 MHz open-loop test (`Media/Buck-Boost Converter Efficiency.xlsx`)
  lost 14.5 W at 123 W out (duration not recorded).

**What follows.**
- **From 24 V, 240 W is not a continuous rating in still air.** The model is about 6 W before core
  loss and copper, so 7–8 W or more, against the 2–3.5 W measured above for a 60 K rise.
  - Even 140 W (28 V, 2.5 W modeled plus core loss) is marginal.
  - 240 W is a burst rating on this board.
  - Continuous high power needs the input-voltage lever, much lower series resistance, or
    airflow.
- **Current is the problem, not switching.** At 48 V / 5 A, everything besides conduction and
  the one hard turn-on (2 µJ) is under 1.5 µJ of a 30 µJ period.
- **The levers, in order:**
  1. **Input voltage.** At 56 V in, every PDO is a buck and 48 V / 5 A models 2.4 W (99 %)
     (`--vin 56`).
     - The ADM1270 OV is set at 57 V.
     - The input ceramics are 50 V parts, so above about 40 V they need 100 V parts.
     - The 100 V EPC2302 leaves less margin there than at 24 V.
  2. **Series resistance** in the 10 A path (section 4).
  3. **Dead time and refresh rate** (about 1 W at 48 V).

## 4. Hardware gates

Each change goes in before the power level that needs it.

**IL_MON range.**
- Now: 13.2 A full scale; COMP1 is capped at 12.5 A.
- Binds once the peaks pass about 11 A: about 105 W at 48 V (I_pk is 16.6 A at 5 A) and about
  155 W at 36 V.
- Change: R5 5 → 2 mΩ. That gives 33 A full scale and saves 0.36 W at 48 V / 5 A.

**ADM1270 current limit.**
- Now: 50 mV / R25 = 10 A (9.4–10.6 A) with a 2 ms timer (`input.kicad_sch`). ISET is tied to
  VCAP, so the internal 2 V reference applies.
- Binds at I_in ≈ 10.3 A, which is 240 W from 24 V.
- Change: R25 5 → 2 mΩ, the schematic value (25 A).

**Input PFETs Q7 and Q8.**
- Now: 7.3 mΩ each when hot, 1.5 W at 10.3 A.
- Binds on the heat budget: 1.5 W of the 2–3.5 W measured for a 60 K rise.
- Change: lower-R PFETs, or an N-channel hot-swap controller in a respin.

**Output shunt R6.**
- Now: 12 mΩ, ID_MON 5.5 A full scale.
- Binds at 5 A: little headroom for OCP or feedforward.
- Change: about 6 mΩ (11 A full scale, half the loss).

**Output and input capacitors.**
- **Now:** ceramics only, CL31B106KBHNNNE (10 µF, 50 V, X7R, 1206); C14 and C15 (82 µF) are not
  fitted. The output has five: about 16 µF at 20 V, 10 µF at 28 V, 7.5 µF at 36 V and 6 µF at
  48 V.
- **Binds at 36 and 48 V:**
  - **Ripple:** forced CCM at 48 V / 5 A gives I_out·D / (f·C) ≈ 2.1 V p-p; 36 V / 5 A gives
    about 1.1 V p-p.
  - **Rating:** 48 V is 96 % of the parts' 50 V rating, before ripple and edge spikes.
  - **Loop:** the gain in section 2.
  - **Load steps:** about 6 µF cannot carry a sink's step while the loop responds.
- **Change:**
  - 100 V-rated output ceramics, and/or a 63 V polymer in C15's 10 × 7.9 mm footprint.
  - Aim for 30 µF or more effective at 48 V: 0.4 V p-p at 5 A.
  - Check the total against the PD spec's source bulk-capacitance limits.

**R27 (Q5/Q6 gate).**
- Now: an 0603 at 123 mW (48 V).
- Binds at sustained 48 V.
- Change: a 1206 (already noted).

**Static-leg bootstrap.**
- Now: about 10 µs hold, assumed (nlspec Round 2a).
- Binds in CCM. A refresh every period costs 0.3–0.7 W at 48 V / 5 A (15–50 ns dead time).
- Change: measure the droop and set N. The LM5175 refreshes only below 4.45 V BOOT–SW.

## 5. Control architecture for CCM

### Industry practice

- **TI LM5175 (SNVSA37A §8.3):**
  - fixed-frequency valley current mode in buck and peak current mode in boost, from one sense
    resistor in series with the low-side FETs;
  - adaptive slope compensation;
  - the static high side held on (HDRV2 in buck, HDRV1 in boost), with a refresh pulse when
    BOOT–SW drops below 4.45 V (electrical characteristics);
  - forced CCM or DCM with diode emulation and pulse skipping, selected by the MODE pin;
  - an optional average current loop.
- **Digital controllers.** Average current mode with the current sampled mid-interval, and the
  duty updated every switching period. The total delay is 1–2 periods (Corradini, Maksimović,
  Mattavelli, Zane, *Digital Control of High-Frequency Switched-Mode Power Converters*, Wiley/IEEE
  2015). Predictive (deadbeat) current control is a published variant: Chen, Prodić, Erickson and
  Maksimović, IEEE TPEL 18(1), 2003. `ccm-predictive.md` is in this family.
- **ZVS by negative valley current** (Waffler and Kolar, IEEE TPEL 24(6), 2009,
  doi:10.1109/TPEL.2009.2015881). It removes the hard turn-on at the cost of RMS current. Here
  that turn-on is 2 µJ of 30 µJ at 240 W, so ZVS is a light- and mid-load tool on this board.
- **G474.**
  - The HRTIM has DAC triggers for slope compensation (RM0440 §28, Figure 283) and COMP-based
    resets.
  - The DAC generates the sawtooth itself (§22.4.11).
  - Any compare can trigger an ADC conversion.

### Proposed for this board

- **Light load: DCM, as now.** The predictive rectifier and pulse skipping stay.
- **Heavy load: forced CCM.**
  - The active leg switches complementary, with 10–20 ns dead time per edge. At 16.6 A the
    turn-off swing takes about 5 ns.
  - The static leg is held on (buck: Q3; boost: Q1) and refreshed every N periods.
  - In boost, put the refresh inside Q4's on-time, so L1 sees 0 V during it.
- **Inner loop: average current, every period (or every second period).**
  - IL_MON is converted from an HRTIM trigger at the middle of the longer interval, where SW2 is
    quiet:
    - boost: Q4-on at 36–48 V, Q3-on at 28 V;
    - buck: SW2 is static anyway.
  - Duty is feedforward (V_out/V_in in buck, 1 − V_in/V_out in boost) plus the current loop's
    correction.
  - COMP1 stays as the cycle-by-cycle backstop.
- **Not peak current mode** (the LM5175 way). Confirmed 2026-10-21: Dan's scope capture showed IL_MON (INA281)
  bandwidth-limited against the modeled inductor current (`ccm-buck-50w.md` 8.3). The original three reasons:
  - IL_MON sits at SW2 and rings on every SW2 edge (500 ns blanking in boost today);
  - it is unidirectional;
  - at 48 V / 5 A the peak (16.6 A) is beyond its range, while the average (10.3 A) is inside.
- **Outer loop: as now.** It commands average output current, plus feedforward of I_out from
  ID_MON. Target crossover is 2–5 kHz once the delay is short.
- **DCM ↔ CCM handover** on I_cmd, with hysteresis. Both modulators realize the same I_cmd, so
  the handover is bumpless.
- **No regeneration into the bench supply.**
  - It cannot sink current. 0.4 J from 350 µF at 48 V would lift about 100 µF of input
    capacitance to about 90 V, past the 60 V input parts (Q7, Q8, ADM1270). The bench supply's
    own output capacitance is unknown.
  - So: I_cmd ≥ 0 in CCM, and down-slews in DCM with a VS_MON guard.
- **The buck-boost band** (V_out within a few V of V_in) is not a fixed PDO at 24 V in. It stays
  in DCM (47 W) until later.

### Stability constraints

Each one is checked on the bench.
1. **Delay.** The phase crossover is at 1/(4τ): 3.97 kHz today, with τ ≈ 63 µs. The target is
   τ ≤ 15 µs (per-period ADC and compute), which moves it to about 17 kHz.
2. **The current loop must sit above the L–C resonance.**
   - The resonance is (1 − D)/(2π√(LC)).
   - With the ceramics only (bias-derated): about 15 kHz at 48 V (6 µF), 18 kHz at 36 V
     (7.5 µF), 18 kHz in buck at 20 V (16 µF). With 30 µF at 48 V it is about 7 kHz.
   - So the current loop needs 20–40 kHz of bandwidth, which means per-period updates. More
     output capacitance relaxes this.
3. **Boost RHP zero.** It is at (1 − D)²R/(2πL): 81 kHz at 48 V / 5 A, 108 kHz at 36 V, 140 kHz
   at 28 V. The small L1 keeps it far above a 5 kHz voltage loop.
4. **Use the capacitance the loop actually sees.**
   - Compute kp from C measured at kHz. C_ext sits behind the output switch and cable.
   - The on-board C depends on V_out: about 44 µF at 5 V down to 6 µF at 48 V with the
     present ceramics. So kp should follow V_out.
5. **Keep limits out of the fast path.** Compute ceilings from filtered or commanded values,
   with anti-windup.
6. **Input filter (Middlebrook).**
   - At 240 W from 24 V the converter is about −2.4 Ω.
   - The supply and leads measured about 30 mΩ (`Media/` sheet).
   - With an assumed 1 µH and about 100 µF, the filter peak is about 0.3 Ω: margin about 7×.
     Check V_in ripple at each step.
7. **Quantization.**
   - The HRTIM step (184 ps) moves V_out about 3.5 mV at 48 V, under VD_MON's 17 mV LSB.
   - With integral action there is no DPWM limit cycle (Peterchev and Sanders, IEEE TPEL 2003).

## 6. Phases

Each phase is flashed only after review. Power-stage changes are flashed on approval.

**Phase 0: measure.** No firmware change, except telemetry if needed.
- Efficiency at 20, 24 and 28 V steady and in the 36 V burst, rectifier off and on:
  - P_in from the bench supply (or IS_MON × VS_MON);
  - P_out from the 25 Ω (V²/25).

  This splits the 1.23× / 1.48× gap into loss and model error.
- Board temperatures at 28 V / 31 W steady state, by thermocouple or IR: Q1–Q4, L1, Q7/Q8,
  R5/R25. This gives a measured °C/W in place of the 5–7 W estimate.
- The 36 V tests in section 2.
- Bootstrap droop on a held high side, which sets N.
- The capacitor part numbers. (Done 2026-10-17: ceramics only, CL31B106KBHNNNE.)
- Exit: efficiency per contract, board °C/W, and the 36 V mechanism confirmed or ruled out.

*Status (2026-10-16 evening, `bench/results/2026-10-16/`).*
- **Done:**
  - 5, 15 and 28 V rectifier off, and 28 V rectifier on (9.6 W lost off, 1.7–2.7 W on);
  - the 28 V thermal soak;
  - the QT Py's draw at 5, 15 and 28 V;
  - IS_MON and ID_MON now convert (`adc_monitor.c` JSQR fix).
- **Still open:**
  - the idle housekeeping on a DMM;
  - input and output current read together, to get the loss within 0.2 W;
  - the 25 Ω measured hot;
  - 20 and 24 V; the rectifier-on sweep;
  - the cool-down;
  - the 36 V tests;
  - bootstrap droop.
- **New items:**
  - Pulling the 25 Ω at 28 V latched HW_FLT1 (VS_GOOD), not OVP.
  - The board's current reads are single samples at one phase of the period, so they are not
    averages. Oversample them across the period, as V_in is.
  - Check the switch-node edge spikes with a ground spring, to tell probe pickup from real
    overshoot.

*Procedure (2026-10-16).* The logger is `bench/results/2026-10-16/scripts/phase0_log.py`. It
averages 20 board reads per step and stores them with the bench readings in `data/phase0.jsonl`.
- **Setup:**
  - a meter at the board input (J2) for V_in; the supply display (or a series meter) for I_in;
  - a meter at the 25 Ω terminals for V_out;
  - the 25 Ω's resistance, measured cold and again right after the thermal run;
  - the QT Py buck taken off VBUS, since its standby draw would count as loss. If it stays,
    measure its VIN current once.
  - Temperatures by thermocouple under Kapton, or IR on a Kapton or matte dot (bare metal reads
    low). Spots: Q1–Q4, L1, Q7/Q8, R25, R5, and ambient.
- **Baseline:** supply on, no contract (regulator idle). V_in × I_in is the board's housekeeping
  power (`--idle-w`).
- **Sweep, rectifier off** (boot default): 5, 9, 15, 20, 24 and 28 V. At each step, settle 30 s,
  read the meters, then log.
- **Sweep, rectifier on:** back to 5 V, `regulator_sr_enable` = 1, then step up again.
- **Thermal run at 28 V, rectifier on** (31 W out, inside the continuous budget):
  - Read temperatures every 2 min until they change by less than 1 °C in 4 min. Read the supply
    at the start and the end.
  - Then go to 5 V and read every minute for 10 min. The cool-down gives the time constant.
  - Stop if any spot passes 90 °C. If all of run E's 1.23× gap is loss, this point dissipates
    about 7 W, near the estimated free-air limit.
- **Derived:**
  - η = P_out / P_in;
  - converter loss = P_in − P_out − housekeeping;
  - °C/W per spot = (T − T_amb) / loss;
  - τ from the cool-down.

**Phase 1: control timing.**
- An HRTIM-triggered ADC every period, for a total delay of 15 µs or less.
- kp from the measured C; the ceiling from filtered values; anti-windup.
- Exit:
  - 36 V into 25 Ω for 2 s with no 3.9 kHz, rectifier on and off;
  - the 28 V loop-margin measurement shows 45° or more.

**Phase 2: boost forced CCM on the present hardware (first >100 W pulses).**
- Boost only (36 and 48 V): complementary Q4/Q3, 20 ns dead time to start, Q1 refreshed every
  N periods, the average-current inner loop, and DCM below a threshold.
- Limits on this hardware: I_pk ≤ about 11 A (COMP1 12.5 A, IL_MON 13.2 A) and I_in ≤ about 8 A
  (ADM1270). That allows about 105 W at 48 V and 155 W at 36 V.
- Pulse ladder, each ≤ 2 s (the firmware cap):

  | Step | Load | Out | I_in | I_pk |
  |---|---|---|---|---|
  | 1 | 36 V into 25 Ω | 52 W | | |
  | 2 | 48 V into 25 Ω | 92 W | about 4 A | about 10.4 A |
  | 3 | 36 V into 12.5 Ω (a second 25 Ω in parallel) | 104 W | 4.5 A | 8.8 A |
  | 4 | 36 V into 8.3 Ω (three) | 156 W | | about 11 A, at the edge |

- Record per step: P_in, P_out, temperatures right after the pulse, and SW2 and VBUS on the
  scope.
- Exit: 104 W for 2 s with efficiency within about 2 points of the model and no oscillation.

**Phase 3: buck forced CCM.**
- Full current at 5, 9, 15 and 20 V (15, 27, 45 and 100 W).
- Q3 static and Q4 refreshed, as the LM5175 does.
- *Started 2026-10-20 ahead of Phase 2* (branch `claude/ccm-hf-buck-50w`, `plans/ccm-buck-50w.md`):
  - 400 kHz buck; voltage mode with V_in feedforward and an integral trim at the PID rate
    (not the average-current inner loop of section 5, which needs per-period updates);
  - Q4 refreshing every 10 µs;
  - a 50 W peak at 20 V as the first target.

  Built, not run.

**Phase 4: hardware for more than about 150 W.**
- R5 and R25 to 2 mΩ; R6 to about 6 mΩ; R27 to a 1206.
- Output capacitance: 30 µF or more effective at 48 V, rated 63–100 V (section 4).
- Then EPC2302, after Phases 2–3 run clean and SW2's overshoot is measured with a ground spring.

**Phase 5: 48 V / 5 A.**
- Pulses of 0.5, 2, 10 and 60 s, logging temperature.
- The continuous rating follows from the measured °C/W. From 24 V, expect a time limit.

**Phase 6: PD-level behavior.**
- Load steps with I_out feedforward.
- Transitions in CCM, with no regeneration.
- EPR AVS and conformance.

## Open decisions

1. **The input voltage.** Is 24 V the input, or can it be 48–56 V? This decides whether 240 W is
   continuous on this board.
2. **The load for >100 W out.** A second (and third) 25 Ω in parallel on VBUS, under the same
   heat budget.
3. **Order.** Phase 0 before the CCM build (recommended), or straight to Phase 2.
