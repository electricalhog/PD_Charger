# Peak current mode on the G474 comparators, DCM and CCM, 5–48 V (plan, 2026-11-22)

Branch `claude/regulator-epr-pps`. Hardware as built, no changes: AD8410A on IL_MON (100 mV/A, 0 A at 1.64 V,
+16.6 A span), 4.7 µH, 24.6 V bench input. Bench state: `bench/results/2026-11-22/README.md`.

Sources:
- **[RM]** RM0440 Rev 9 (`STM32CubeIDE/rm0440-…pdf`): §22.4.11 DAC sawtooth, Table 191 HFSEL, §28.3.21 DAC
  triggers, Table 223 external events, Table 228 fault inputs, Table 229 DAC triggers, TIMxOUTR.DTEN, TIMxCR.DELCMP2.
- **[HAL]** `Drivers/STM32G4xx_HAL_Driver` and the CMSIS device header.
- **[DB]** the CubeMX pin database, `STM32G474R(B-C-E)Tx.xml`.
- **[ST]** ST's G474 peak-current-mode buck, `STM32Cube_FW_G4_V1.6.1/Projects/B-G474E-DPOW1/Applications/
  Digital_Power/Buck_CurrentMode_SW`.
- **[model]** `plans/scripts/` arithmetic, below.
- **[assumed]** anything P0 has to measure.

## 0. Bench status (2026-11-22, after the first runs): blocked on IL_MON

Built and verified (`bench/results/2026-11-22/README.md`, "Peak current mode, first bench"):
- Timer E in lockstep with Timer A, DCDE sawtooth triggers, CPT1 = TA1 reset, CPT2 = EEV4; DAC3 CH1 in sawtooth mode
  from `HRTIM_RST_TRG5` / `HRTIM_STEP_TRG5`, 425-tick steps. Checked at idle against COMP1 (reset value, 12.4 step,
  trigger mapping all as configured).
- The DCM comparator mode (§1 below, `regulator_pcmc_dcm`, `PCMC_DELAY_NS` 200).

**Measured: IL_MON does not follow L1's current while Q1 is on.** At 12 V, averaged over ~200 pulses:
- In DCM it holds at 0 A (±3 mV) for the first ~1 µs, then jumps with an overshoot near turn-off.
- In forced CCM at 200 kHz it holds at the valley, steps once, and only reaches the peak in the last ~300 ns of a
  2.25 µs on-time.
- Through the discharge (SW1 low, SW2 steady) it tracks: 2.47 A/µs against 2.68 A/µs from the model.

So COMP1 cannot end a pulse at a peak, and **peak current mode on IL_MON is not possible with the adapter as it
reads today**. `PCMC_DCM_DEFAULT` is 0. §§1–6 stand as the design if IL_MON is made to track. ID_MON and IS_MON can't
replace it: the output caps sit between Q3 and R6, and U6 reads the ADM1270's filtered sense. Both are DC.

**Cause found (later the same day, README "IL_MON blind spot"): the AD8410A's ~1 µs common-mode deglitch, triggered
by SW2 steps.** With the output-leg refresh every 2nd or 3rd period in forced CCM, the pulses with no refresh before
them track the on-time ramp ~200 ns after Q1 turns on (2.6 A/µs against 2.7 from the model), at 200 and 400
kHz. So: CCM can run peak current mode with the refresh every Nth period (the refreshed pulse on the backstop). DCM
needs Q3 to hold SW2 at V_out ≥ 1.7 µs before Q1 turns on, because SW2 floats to ~0 V between pulses today.

**Buck CCM peak current mode runs (later still, README "Buck CCM peak current mode", `regulator_pcmc_ccm`,
default off).** 12 V into 25.3 Ω at 200 and 400 kHz, the two periods' pulses within 15 ns of each other. The design
as built, which differs from the bullet above:
- The refresh period's pulse ends on Timer B CMP1 (TIMEVNT1) at the tracked on-time, not on the backstop. COMP1 is
  masked for it by Timer A's EEV4 filter TIMFLTR1 (Timer B reset → TB CMP1). The held valley does not protect it:
  IL_MON jumps to the true current when the hold releases, and COMP1 tripped there.
- The tracker reads Timer B CPT1 (TA1 reset) through DMA. It subtracts the 67-tick capture latency and moves a
  quarter of the way to each sample, either way. The threshold's ripple term uses T·V_out/V_in.
- CCM uses its own threshold lead, `regulator_pcmc_ccm_delay_ns` 0: 200 ns collapsed V_out at the takeover.
- 5 and 20 V at 200 and 400 kHz also regulate (10 s each, no fault). The pulse pairs are within 10 ns of each other.
- Open:
  - One unexplained OVER_BAND exit, then SW_OVP 13.9 V, in about two minutes of running.
  - The takeover from the duty law dips V_out by 1.0–1.5 V. That is 31 % at 5 V, because i_cmd's offset from I_out
    (+0.3 A at 5 V, −0.5 A at 20 V) lands as a current step.
  - Load steps and a battery aren't tried yet.

**Peak current mode as the buck's only mode (later, README "Peak current mode as the buck's only mode",
`regulator_pcmc_ccm` = 2, opt-in).** Forced CCM and the comparator from the first PID cycle, no duty law, no
takeover. A negative current command (to −1.5 A) takes the place of DCM. Into 25.3 Ω, 5/12/20 V at 200 and 400 kHz
with the refresh every 2nd/3rd/4th period, and 5 ↔ 20 V triangle sweeps at 10 and 50 V/s: no fault, never left the
mode. What it needed: COMP1's zero measured at every start (2072–2083 vs the assumed 2031), slope compensation
≥ 0.8·m2 (sub-harmonic dips), the ramp aligned to the tracked on-time, a DAC3 full-scale cap on the slope, and a
300 Hz loop only while the output switch is open. Mode 1 keeps its old constants. Open:
- The no-load start trips SW_OVP. Below ~0.5 µs on-time IL_MON overshoots (6.3 against 4.8 A/µs), so the
  comparator loses authority. This needs a low-duty modulator before mode 2 can start into an open output or a battery.
- 20 V / 400 kHz still dips (2–20 per 100 ms).
- The command runs 0.1–0.85 A under I_out.

The firmware-only alternative is **valley current mode**:
- The comparator acts during the discharge, the interval where IL_MON is good, in buck, boost and buck-boost alike.
- It is not designed here yet.

## 1. What changes

Today the on-time ends every pulse: DCM's on-time is predicted from the peak command, and CCM's duty law is voltage
mode. COMP1 only backs that up, at 9–12.5 A. In this plan:

- **COMP1 ends every charge pulse** in DCM and CCM, in all three kinds (buck, band, boost). It compares IL_MON with
  DAC3_CH1, and EEV4 resets the charge.
- **DAC3_CH1 is a hardware sawtooth** for slope compensation. The HRTIM resets and steps it, so no CPU runs per step.
- **The voltage loop's output stays `i_cmd`**, the average output current it already computes. Each mode and kind
  turns it into a peak, so one command drives DCM and CCM and the handover is bumpless in average current.
- **Every CCM kind runs both legs complementary through the HRTIM dead-time units.** The firmware programs only TA1
  and TB1.
- **A current limit is a clamp on the peak command:** it holds within one period, with no ID_MON in that path.
- **COMP3 becomes a hardware V_out OVP** and **COMP4 a per-cycle input-current limit.**

Why the 2026-10 "ruled out" no longer holds: `ccm-buck-50w.md` §8.3 needed "a faster current-sense amplifier". The
AD8410A is that change, and it is bidirectional. Its range works out at 400 kHz in CCM (§3). P0 measures its speed.

## 2. Comparators, triggers and interrupts

**Pins [DB]:**
- PA1 (IL_MON): COMP1_INP.
- PA0 (V_out divider, VD_MON): COMP3_INP.
- PC1 (ID_MON): also COMP3_INP.
- PB0 (IS_MON): COMP4_INP.
- PA4 (V_in): no comparator input.
- COMP2, COMP5, COMP6 and COMP7 have no board signal on their inputs, so they stay unused.

| Comparator | + input | − input (threshold) | HRTIM routing | CPU interrupt | Role |
|---|---|---|---|---|---|
| COMP1 | PA1 IL_MON | DAC3_CH1, sawtooth [HAL: DAC3_CH1 for COMP1/3] | **EEV4** (src2, fast, no digital filter) [RM T223]: buck TA1 reset, boost/band TB1 set, both OR CMP2 (backstop). Blanked to CMP1 [existing] | none: it trips every period. EXTI21 stays masked. COMP1_OUT on PA6 for the scope [existing] | ends every charge pulse |
| COMP3 | PA0 V_out | DAC1_CH1, **internal only, buffer off**, so PA4 stays V_in's ADC input [HAL: DAC1_CH1 for COMP1/3/4] | **FLT5, internal source = COMP3** (FLTxSRC = 01) [RM T228], digital filter on | HRTIM1_FLT_IRQn: the existing `regulator_hrtim_fault_isr` gains FLT5 → `HW_OVP` | hard V_out OVP, ns response |
| COMP4 | PB0 IS_MON | DAC3_CH2, enabled in the .ioc, unused in code [HAL: DAC3_CH2 for COMP2/4] | **EEV2** (src2, fast) [RM T223] added to the charge reset (TA1 reset / TB1 set) | **EXTI30 → COMP4_5_6_IRQn** (rising) [HAL]: counts trips; the handler masks EXTI30 and the PID ISR re-arms it, so ≤ 20 k/s; trips in 20 consecutive PID cycles (1 ms) → FAULT `IN_OCP` | per-cycle input-current limit |

Routes I'm not taking, and why:
- **COMP1 → FLT4 [RM T228].** It would fault at the PCMC threshold every period.
- **COMP4 → a fault.** COMP4 can only reach FLT2 [RM T228], and FLT2 is the ADM1270's IS_GOOD (PA15).
- **COMP3 for ID_MON.** It's the same comparator as V_out. V_out OVP is the one the logs need (SW_OVP on 11-15 and
  others). ID_MON stays an ADC reading, and the peak clamp replaces its job in CCM.

NVIC: COMP4_5_6_IRQn at priority 1 (as HRTIM, `NVIC_PRIORITY_HRTIM`). The TIM6 slope ISR (priority 0) is deleted.
Thresholds:
- **COMP3:** `ovp_ref_mv` (which already holds through down-ramps) + max(1.5 V, 8 %), written to DAC1_CH1 each PID
  cycle. V_out scale is 70.6 V full scale (`VD_MON_FULL_SCALE_MV`), 58 counts/V. The software SW_OVP stays below it.
- **COMP4:** 12.0 A input (IS_MON 250 mV/A → 3.0 V, 3723 counts). 240 W from 24.6 V at η 0.93 is 10.5 A [model].
  This is under the ADM1270's 24.5 A / 2 ms.

**DAC3_CH1 sawtooth, driven by HRTIM Timer E** (Timers E and F are unused; A–D are taken) [RM §28.3.21, ST example]:
- **Triggers:**
  - Timer E: `DualChannelDacEnable = DCDE_ENABLED`, `DCDR_COUNTER` (reset at Timer E's period start), `DCDS_CMP2`.
  - With DCDE = 1 and DCDS = 0, CMP2 auto-repeats: one step every CMP2 ticks.
  - DAC3_CH1: reset trigger `DAC_TRIGGER_HRTIM_RST_TRG5`, step trigger `DAC_TRIGGER_HRTIM_STEP_TRG5` (TRG n = timer n,
    as [ST] uses TRG3 with Timer C).
- **Why a spare timer:** Timer A's and B's CMP2 are the backstop and the CCM off-edge. RM0440 forbids other uses of
  CMP2 while DCDE = 1 with DCDS = 0.
- **Lock to the switching period:** Timer E's period equals the active leg's switching period. It starts and
  re-synchronizes in the same MCR write as A–D, where the firmware already re-syncs at a start and a mode change.
  Same clock and same period, so no drift.
- **Step rate:** CMP2 = period / 64 at 200 kHz (78 ns) and period / 32 at 400 kHz. Limits: DAC_DOR updates every 7 AHB
  cycles (41 ns) at HFSEL = 10 [RM T191], and DAC3 is 15 Msps internal [RM §22]. [ST] uses 75 steps at 200 kHz.
- **Data:** `HAL_DACEx_SawtoothWaveGenerate(…, DAC_SAWTOOTH_POLARITY_DECREMENT, reset, step)`.
  - Reset is 12-bit. Step is 16-bit **12.4 fixed point** ([ST] scales by 4096·16/3.3).
  - The counter saturates at 0 [RM §22.4.11]. A ramp under IL_MON's zero code trips COMP1 at once, so a floor of 0
    is safe.
  - The PID ISR writes STRSTDATA/STINCDATA. They take effect at the next reset trigger.

## 3. Operating envelope and the choices it forces [model, `plans/scripts/pcmc_envelope.py`]

η = 0.93, τ = 200 ns trip delay [assumed]. Reset level = the DAC value at the ramp start, in amps above IL_MON's zero.

| V_out / I_out | Kind | f | D | I_pk | Slope m_a (A/µs) | Reset level | Span 16.6 A |
|---|---|---|---|---|---|---|---|
| 5 V / 3 A | buck | 400 k | 0.21 | 4.1 | 0 | 3.3 | ok |
| 9 V / 3 A | buck | 400 k | 0.38 | 4.6 | 0 | 3.9 | ok |
| 15 V / 3 A | buck | 400 k | 0.63 | 4.6 | 0.72 | 5.3 | ok |
| 20 V / 5 A | buck | 200 k | 0.84 | 7.1 | 2.05 | **15.5** | 1.1 A margin |
| 20 V / 5 A | buck | 400 k | 0.84 | 6.0 | 2.05 | 10.2 | ok |
| 24 V / 5 A | band | 400 k | 0.26 | 7.0 | 0 | 5.9 | ok |
| 28 V / 5 A | boost | 400 k | 0.15 | 7.1 | 0 | 6.1 | ok |
| 36 V / 5 A | boost | 400 k | 0.34 | 10.1 | 0 | 9.1 | ok |
| 48 V / 5 A | boost | 200 k | 0.51 | **17.1** | — | — | **over** |
| 48 V / 5 A | boost | 400 k | 0.51 | 13.8 | margin only | 12.8 | ok |

**Decisions:**
1. **CCM runs at 400 kHz in every kind.** 200 kHz overruns the span at 48 V and leaves 1.1 A at 20 V. Buck CCM at
   400 kHz already ran on this board (`ccm-buck-50w.md`).
2. **DCM stays at 200 kHz,** as now.
3. **Slope**, per PID cycle from the ADC voltages:
   - m_a = 1.25 · max(0, (m2 − m1)/2), plus 0.1·m2 when D > 0.45.
   - buck: m1 = (V_in − V_out)/L, m2 = V_out/L.
   - boost and band: m1 = V_in/L, m2 = (V_out − V_in)/L.
   - Zero in DCM and at D < 0.45. That's the subharmonic bound with 25 % margin. [ST] uses a fixed 0.5 V/period.
4. **Peak clamp: 14.0 A** commanded. 1.6 A of the span is left for the trip-delay overshoot until P0 measures τ.
5. **Reset value** = I_pk − m1·τ + m_a·(t_on,pred + t_q1) in DAC counts, with t_q1 = Q1's turn-on after the period
   start (250 ns in CCM, after the refresh in DCM). It is clamped to `IL_MA_TO_DAC_COUNTS(14000 + ramp)` ≤ 4095.

**i_cmd → I_pk** (one function per mode and kind, host-tested):
- **DCM, buck:** I_out = ½·I_pk²·L·f·(1/(V_in − V_out) + 1/V_out). Boost and band are the same with their slopes.
  This is the relation the DCM code already uses to set its on-time.
- **CCM, buck:** I_pk = i_cmd + ΔI/2 + m1·τ.
- **CCM, boost and band:** I_pk = i_cmd·V_out/(V_in·η) + ΔI/2. ΔI is from the voltages and 400 kHz.

**DCM ↔ CCM, with no ID_MON in the decision:**
- I_bound = the output current where CCM's valley is 0 at 400 kHz: 1.0–1.6 A across 5–48 V [model].
- Enter CCM at i_cmd > 1.3·I_bound and leave under 1.0·I_bound. CCM therefore never needs a negative average current,
  and the bidirectional IL_MON covers the valley dipping under 0 near the exit.
- The voltage-based kind selection (buck / band / boost entry and exit permille) stays as it is.

**Minimum comparator-controlled pulse:** the blanking plus τ.
- Buck: 100 ns + 200 ns → 1.25 A at 5 V, 0.29 A at 20 V [model].
- Boost: 500 ns + 200 ns → 3.7 A.
- Below 1.2× that, DCM keeps today's predicted on-time with the DAC at the clamp. Skipping covers lighter loads, as now.

## 4. Leg timing per mode

**CCM, all kinds: hardware dead time on Timer A and Timer B** (TIMxOUTR.DTEN, rising and falling = `regulator_ccm_
deadtime_ns`, 20 ns = 27 ticks of 0.735 ns):

| Kind | TA1 (Q1), TA2 = NOT TA1 + DT | TB1 (Q3), TB2 = NOT TB1 + DT |
|---|---|---|
| buck | set CMP4 (250 ns), **reset EEV4 \| EEV2 \| CMP2** (CMP2 = max on-time backstop) | held on; reset CMP2 and set CMP4 around Q4's refresh every 10 µs (Timer B period, as now) |
| band | Q2 notch: reset at the period start, set after `ccm_band_q2_ns` (640 ns) | reset at the notch end (Q4 on), **set EEV4 \| EEV2 \| CMP2** |
| boost | held on, Q2 refresh as now | reset at the period start / refresh end, **set EEV4 \| EEV2 \| CMP2** |

- With DT on, Q2 and Q4 follow the comparator's actual edge with guaranteed dead time. Timer C's "Q2 one dead time
  after Q1" event (`buck_ccm.h`) is no longer needed in CCM.
- **DTEN can't change while the timer runs [RM TIMxOUTR].** A DCM → CCM entry or exit therefore stops Timers A, B and
  E right after a period event, writes DTEN, set/reset sources, CMP values and the DAC step, then restarts and
  re-syncs them in one MCR write. That loses one period (2.5–5 µs), at a moment when L1 is near zero in DCM anyway.
- I'm not using CMP2 auto-delay (TIMxCR.DELCMP2 off a capture on EEV4) or Timer C reset on EEV4. Both put the
  dead-time guarantee in software timing, and an unblanked EEV4 would set Q2 while Q1 is on.
- Blanking: CMP1 = Q1 (or Q4) turn-on + t_blank, as CCM already does (`regulator.c:805`).

**DCM: DTEN = 0, as now.**
- The charge pulse is ended by EEV4 at a constant threshold (step 0) = I_pk − m1·τ.
- CMP2 becomes a backstop at 1.25 × the predicted on-time + 100 ns. If COMP1 fails, the peak is bounded at 1.25 I_pk.
- **Blanking: no change needed** (corrected 2026-11-22). With `SYNC_RECT_ENABLED` 0, DCM's CMP1 is already
  `DCM_PULSE_START_TICKS` + blanking (`hrtim_configure_compare_registers`). At idle Timer E captured TA1's
  EEV4 reset at 2016 ticks, 0.37 µs into the period: the pulse start plus 100 ns.
- The predictive SR stays off in DCM (`SR_ENABLE_DEFAULT 0`). If it is turned on, its Q2 window must start after the
  CMP2 backstop + DT. Q2 must never be on while Q1 might be.

## 5. Phases (bench gates; resistor only until P3; gate changes scoped unpowered first, skill rules)

**P0: Measure IL_MON and COMP1.** Firmware: a `bu` command for a fixed DAC threshold (the blanking needs no fix, §1).
1. DCM buck at 5 / 12 / 20 V and boost at 28 V, raw scope capture at 500 MSa/s, CH1 with no BWL. Check IL_MON's
   rising and falling slopes against the model (gain at two slopes), the delay, and the settling after the SW1/SW2
   edges.
2. Probe PA6 (COMP1_OUT) and set fixed thresholds of 1 / 2 / 4 / 8 A with the on-time held long. COMP1 ends the pulse.
   Read the trip current from IL_MON at the PA6 edge.
   - **Gate:** within ±10 % or ±0.3 A.
   - **Gate:** τ measured. Use it for τ in §3.
   - **Gate:** no trips on edge ringing after the blanking.
3. **Decision rules:**
   - If ringing trips COMP1 after the blanking, move COMP1 to **EEV6** (src3 = COMP1, digital filter) [RM T223] and
     fold the filter latency into τ.
   - If τ > 300 ns, keep CCM at 400 kHz and lower the clamp by m1·(τ − 200 ns).

**P1: Sawtooth with the power stage off.**
- Timer E, DAC3_CH1 sawtooth, all in `regulator.c` / USER CODE. Not the .ioc: the known regeneration drift.
- With no power, IL_MON sits at 1.64 V. The ramp crosses it at t = (reset − zero)/slope, and PA6 flips there.
- **Gate:** the crossing is within one step of the model at three slopes, and stays locked over 1 s of periods.
- **Gate:** `bu profile` shows no added CPU load.

**P2: Comparator DCM, all kinds, into the resistor.**
- 5 → 33 V, as today's sweep.
- **Gate:** V_out and ripple at least as good as the on-time DCM on 2026-11-22.
- **Gate:** the pulses end at PA6 at the commanded peak.

**P3: CCM, with the dead-time units, at 400 kHz.**
- Buck 5–20 V first, then band 24 V, then boost 28–33 V. A new `regulator_ccm_enable` value 3 (PCMC) keeps the
  duty-law CCM selectable.
- Fixed I_pk first, then the voltage loop.
- **Gate:** no period-doubling at D > 0.5 (12–20 V buck).
- **Gate:** I_out(i_cmd) follows §3.
- **Gate:** DCM ↔ CCM handovers both ways, with no SW_OVP/UVP.
- **Gate:** `switch-pulse` 20 ms at 20 V (the `ccm-buck-50w.md` §8.3 SW_UVP case) passes.
- Then the PD walk with the G431 sink.

**P4: COMP3 / COMP4.**
- Lower each threshold until it acts. COMP3 must fault in < 1 µs on the scope. COMP4 must cut pulses, and its EXTI
  count must reach FAULT after 1 ms.
- Then normal thresholds, and a regression of P2/P3.

**P5: EPR (36 / 48 V) into the resistor, then a LiPo with Dan's go for that run.**

## 6. Code touchpoints

- `Core/Src/slope_comp.c/.h`: becomes the DAC3 sawtooth and Timer E. `slope_comp_set_peak()` writes STRSTDATA and
  `slope_comp_update_step()` writes STINCDATA. The TIM6 ISR and its floor logic are deleted.
- `Core/Src/regulator.c`:
  - the PID step 6 peak command and the clamp;
  - the DCM CMP2 backstop;
  - EEV2 in the charge reset;
  - FLT5 enable and FLT5 in `regulator_hrtim_fault_isr`;
  - the COMP3 / COMP4 / DAC1_CH1 / DAC3_CH2 init;
  - the COMP4 EXTI handler;
  - Timer E in the counter re-sync.
- `Core/Src/buck_ccm.c`: a PCMC kind for buck, band and boost. It does i_cmd → I_pk, slope and reset value, and
  programs TA1/TB1 with the DT units. It is host-tested like the duty law (`tests/buck_ccm`).
- `Core/Inc/regulator_config.h`: the clamp, τ, the slope margin, the COMP3 / COMP4 thresholds, the step counts, and
  CCM at 400 kHz.
- `plans/scripts/pcmc_cycle.py`: a cycle model with slope, τ and the DAC staircase, for the subharmonic boundary and
  I_out(i_cmd).
- `tools/bringup`: `bu regulator il-threshold <mA>` (P0), and PCMC fields in `regulator status`.
