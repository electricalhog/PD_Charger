/**
 * @file    regulator_config.h
 * @brief   Centralized hardware constants, pin mappings, and compile-time
 *          configurable parameters for the buck-boost regulator firmware.
 *
 * This file is the single point of truth for all hardware-derived constants
 * in the firmware (NLSpec §2.1).  Every pin mapping and every numeric constant
 * that comes from the schematic, the datasheet, or a deliberate design choice
 * lives here — NOT scattered across source files.
 *
 * Pin-mapping entry format (§2.1):
 *   - Pin name (schematic net label)
 *   - MCU port/pin identifier
 *   - Net label from schematic
 *   - Physical connection
 *   - Signal direction
 *
 * Constant entry format (§2.1):
 *   - Parameter name
 *   - Units
 *   - Valid range (where applicable)
 *   - Derivation reference
 *
 * Sources of truth (priority order, per spec §Meta):
 *   manual corrections > KiCad schematics > final.ioc > spec answers
 *
 * NLSpec conformance: v0.1.2j
 */

#ifndef REGULATOR_CONFIG_H
#define REGULATOR_CONFIG_H

#include "stm32g4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * SECTION 1: ANALOG INPUT PIN MAPPINGS (Voltage and Current Sense)
 * =========================================================================*/

/**
 * VD_MON — Output voltage sense (V_out)
 * MCU pin : PA0 / ADC1_IN1
 * Net     : VD_MON (output.kicad_sch)
 * Physical: 100 kΩ / 5.82 kΩ resistive divider from V_out rail
 * Dir     : Analog input
 * NOTE    : IOC labels PA0 as VS_MON — this is an IOC labeling error.
 *           The schematic (higher priority) confirms PA0 = VD_MON (§2.2).
 */
#define PIN_VD_MON_PORT GPIOA
#define PIN_VD_MON_PIN GPIO_PIN_0
#define ADC_CHANNEL_VD_MON ADC_CHANNEL_1

/**
 * IL_MON — Inductor current sense
 * MCU pin : PA1 / ADC1_IN2 (shared with COMP1_INP)
 * Net     : IL_MON (dc-dc.kicad_sch)
 * Physical: INA281A2 (U7) output; 5 mΩ shunt (R5), 50 V/V gain
 * Dir     : Analog input (also COMP1 non-inverting input)
 */
#define PIN_IL_MON_PORT GPIOA
#define PIN_IL_MON_PIN GPIO_PIN_1
#define ADC_CHANNEL_IL_MON ADC_CHANNEL_2

/**
 * VS_MON_A4 — Input voltage sense (V_in)
 * MCU pin : PA4 / ADC2_IN17
 * Net     : VS_MON (input.kicad_sch)
 * Physical: 100 kΩ / 5.82 kΩ resistive divider from V_in rail
 * Dir     : Analog input
 * NOTE    : V_in is on ADC2 because PA4 is not available on ADC1 for
 *           this package (§8.5).
 */
#define PIN_VS_MON_PORT GPIOA
#define PIN_VS_MON_PIN GPIO_PIN_4
#define ADC_CHANNEL_VS_MON ADC_CHANNEL_17

/**
 * ID_MON — Output current sense (I_out, delivery current)
 * MCU pin : PC1 / ADC1_IN7
 * Net     : ID_MON (output.kicad_sch)
 * Physical: INA281A2 (U8) output; 12 mΩ shunt (R6), 50 V/V gain
 * Dir     : Analog input
 * Resolved: Round 3 Q2, confirmed from final.ioc pin labels (§3.3).
 */
#define PIN_ID_MON_PORT GPIOC
#define PIN_ID_MON_PIN GPIO_PIN_1
#define ADC_CHANNEL_ID_MON ADC_CHANNEL_7

/**
 * IS_MON — Input current sense (I_in, source current)
 * MCU pin : PB0 / ADC1_IN15
 * Net     : IS_MON (input.kicad_sch)
 * Physical: INA293A2 (U6) output; 5 mΩ shunt (R25, confirmed physical
 *           inspection), 50 V/V gain
 * Dir     : Analog input
 * Resolved: Round 3 Q2 + Q3 — R25 = 5 mΩ (schematic shows 2 mΩ, stale).
 */
#define PIN_IS_MON_PORT GPIOB
#define PIN_IS_MON_PIN GPIO_PIN_0
#define ADC_CHANNEL_IS_MON ADC_CHANNEL_15

/* =========================================================================
 * SECTION 2: COMPARATOR OUTPUT PIN
 * =========================================================================*/

/**
 * COMP1_OUT — Comparator 1 output (routes to HRTIM EEV4)
 * MCU pin : PA6
 * Net     : COMP1_OUT (dc-dc.kicad_sch)
 * Physical: COMP1 output pin; connects HRTIM external event 4 (EEV4)
 * Dir     : Digital output (from COMP1 peripheral)
 */
#define PIN_COMP1_OUT_PORT GPIOA
#define PIN_COMP1_OUT_PIN GPIO_PIN_6

/* =========================================================================
 * SECTION 3: HRTIM OUTPUT PIN MAPPINGS (Gate Drive Signals)
 * =========================================================================*/

/**
 * PHASE_1_P (CHA1) — Timer A Output 1, input-side high-side FET drive
 * MCU pin : PA8 / HRTIM1_CHA1
 * Net     : PHASE_1_P (dc-dc.kicad_sch)
 * Physical: uP1966E (U1) high-side input; drives Q1 (EPC2306/EPC2302)
 * Dir     : Digital output (HRTIM-controlled)
 * Buck    : Switches (Set=Period, Reset=EEV4)
 * Boost   : Static HIGH (forced)
 */
#define PIN_CHA1_PORT GPIOA
#define PIN_CHA1_PIN GPIO_PIN_8

/**
 * PHASE_1_N (CHA2) — Timer A Output 2, input-side low-side FET drive
 * MCU pin : PA9 / HRTIM1_CHA2
 * Net     : PHASE_1_N (dc-dc.kicad_sch)
 * Physical: uP1966E (U1) low-side input; drives Q2 (EPC2306/EPC2302)
 * Dir     : Digital output (HRTIM-controlled, complementary to CHA1)
 */
#define PIN_CHA2_PORT GPIOA
#define PIN_CHA2_PIN GPIO_PIN_9

/**
 * PHASE_2_P (CHB1) — Timer B Output 1, output-side high-side FET drive
 * MCU pin : PA10 / HRTIM1_CHB1
 * Net     : PHASE_2_P (dc-dc.kicad_sch)
 * Physical: uP1966E (U3) high-side input; drives Q3 (EPC2306/EPC2302)
 * Dir     : Digital output (HRTIM-controlled)
 * Buck    : Static HIGH (forced)
 * Boost   : Switches (Set=EEV4, Reset=Period)
 */
#define PIN_CHB1_PORT GPIOA
#define PIN_CHB1_PIN GPIO_PIN_10

/**
 * PHASE_2_N (CHB2) — Timer B Output 2, output-side low-side FET drive
 * MCU pin : PA11 / HRTIM1_CHB2
 * Net     : PHASE_2_N (dc-dc.kicad_sch)
 * Physical: uP1966E (U3) low-side input; drives Q4 (EPC2306/EPC2302)
 * Dir     : Digital output (HRTIM-controlled, complementary to CHB1)
 */
#define PIN_CHB2_PORT GPIOA
#define PIN_CHB2_PIN GPIO_PIN_11

/* =========================================================================
 * SECTION 4: HRTIM FAULT INPUT PIN MAPPINGS
 * =========================================================================*/

/**
 * VS_GOOD — Input power-good (HRTIM FLT1)
 * MCU pin : PA12 / HRTIM1_FLT1  (power board J1.16)
 * Net     : VS_GOOD = ADM1270 (U5) PWRGD, open-drain, R31 100k to +3V3
 * Physical: High when the switched input rail VS is above the FB_PG
 *           threshold: VS > 4.60 V rising / < 4.46 V falling
 *           (R28 100k + R29 4.3k over R32 29k, 1.0 V ref, 30 mV hyst).
 *           LOW whenever INPUT_EN is low (VS off), on VIN over-voltage
 *           (> 57.7 V, OV divider R34/R35), or after an ADM1270 current trip.
 * Dir     : Digital input, active-low (fault asserts when input goes LOW)
 * Effect  : Any assertion forces ALL HRTIM outputs to safe state (all FETs off)
 * Note    : Only meaningful while the input path is enabled, so the FLT
 *           interrupts are armed only in RUNNING (see regulator.c).
 */
#define PIN_VS_GOOD_PORT GPIOA
#define PIN_VS_GOOD_PIN GPIO_PIN_12

/**
 * IS_GOOD — Input over-current latch (HRTIM FLT2)
 * MCU pin : PA15 / HRTIM1_FLT2  (power board J1.17)
 * Net     : IS_GOOD = ADM1270 (U5) ~FAULT, open-drain, R30 100k to +3V3
 * Physical: Pulled LOW when the ADM1270 shuts its FET off after an SOA
 *           over-current: > 24.5 A (49 mV across R25 2 mΩ, ISET=VCAP) for
 *           2 ms (C_TIMER 20 nF), or > 50 A instantly (severe OC, ~2 µs).
 *           Latch-off mode (~FAULT not tied to ENABLE): stays low until
 *           TIMER_OFF has recharged (ADM1270_COOLDOWN_MS) AND INPUT_EN is
 *           toggled low → high.
 * Dir     : Digital input, active-low (fault asserts when input goes LOW)
 */
#define PIN_IS_GOOD_PORT GPIOA
#define PIN_IS_GOOD_PIN GPIO_PIN_15

/* =========================================================================
 * SECTION 5: POWER PATH CONTROL GPIO
 * =========================================================================*/

/**
 * INPUT_EN — Enables input power path
 * MCU pin : PC7
 * Net     : INPUT_EN (input.kicad_sch)
 * Physical: ADM1270 gate control enable signal, active HIGH (confirmed)
 * Dir     : GPIO output
 * Usage   : Assert HIGH before enabling switching; deassert on fault/shutdown.
 */
#define PIN_INPUT_EN_PORT GPIOC
#define PIN_INPUT_EN_PIN GPIO_PIN_7

/**
 * ADM1270_COOLDOWN_MS — Minimum INPUT_EN low time after an ADM1270 current
 * trip before it can be re-enabled.  TIMER_OFF off-time
 * t = V_TMROFFH × C_TIMER_OFF / I_TMROFF = 2.0 V × 50 nF / 1 µA = 100 ms
 * typical; 2.04 V × 55 nF / 0.85 µA ≈ 132 ms worst case (datasheet Rev. A
 * p.4 limits, ±10 % cap).  Rounded up for margin.
 */
#define ADM1270_COOLDOWN_MS 150u

/**
 * INPUT_PGOOD_TIMEOUT_MS — Maximum wait for VS_GOOD after asserting INPUT_EN.
 * The ADM1270 soft-starts VS under its (folded-back) current limit; the
 * FET gate slews at 25 µA, so a few ms is typical.  A timeout means no/low
 * VIN, VIN over-voltage, or the ADM1270 tripped charging VS (short on VS).
 */
#define INPUT_PGOOD_TIMEOUT_MS 50u

/**
 * OUTPUT_EN — Enables output power path
 * MCU pin : PC8
 * Net     : OUTPUT_EN
 * Physical: Output path enable signal, active HIGH (confirmed)
 * Dir     : GPIO output
 * Usage   : Assert HIGH before switching; deassert on fault/shutdown.
 */
#define PIN_OUTPUT_EN_PORT GPIOC
#define PIN_OUTPUT_EN_PIN GPIO_PIN_8

/**
 * OUTPUT_DIS — Discharges output path
 * MCU pin : PC9
 * Net     : OUTPUT_DIS
 * Physical: Output path discharge signal, active HIGH (confirmed)
 * Dir     : GPIO output
 * Usage   : Assert (HIGH) during FAULT and IDLE states to bleed VBUS.
 */
#define PIN_OUTPUT_DIS_PORT GPIOC
#define PIN_OUTPUT_DIS_PIN GPIO_PIN_9

/* =========================================================================
 * SECTION 6: HRTIM TIMING CONSTANTS
 * =========================================================================*/

/**
 * SYSCLK_HZ — System clock frequency in Hz.
 * Units  : Hz
 * Value  : 170 MHz (STM32G474, confirmed final.ioc PLL configuration).
 * Purpose: Single source of truth for all time-to-tick conversions.
 */
#define SYSCLK_HZ  170000000UL

/**
 * HRTIM_PRESCALER_MUL — HRTIM internal clock multiplier (DLL factor).
 * Units  : dimensionless
 * Value  : 32 (CKPSC = 0 = MUL32 in STM32CubeMX, confirmed final.ioc).
 * Purpose: HRTIM counter resolution = 1 / (SYSCLK_HZ × HRTIM_PRESCALER_MUL)
 *          = 1 / (170 MHz × 32) = 183.82 ps/tick.
 */
#define HRTIM_PRESCALER_MUL  32UL

/**
 * HRTIM_SWITCHING_FREQ_HZ — Target switching frequency.
 * Units  : Hz
 * Value  : 200 kHz (5.0 µs period).  Target upgrade: 500 kHz → 10 880 counts.
 */
#define HRTIM_SWITCHING_FREQ_HZ  200000UL

/**
 * HRTIM_NS_TO_TICKS — Convert a nanosecond duration to HRTIM timer counts.
 * Formula : ticks = ns × SYSCLK_HZ × HRTIM_PRESCALER_MUL / 1 000 000 000
 *                 = ns × (SYSCLK_HZ / 1 000 000) × HRTIM_PRESCALER_MUL / 1 000
 * Note    : Uses uint64_t intermediary to prevent 32-bit overflow for inputs
 *           up to ~780 ns.  Evaluated entirely at compile time for constant ns.
 * Examples: 100 ns → 544 ticks, 200 ns → 1088 ticks, 500 ns → 2720 ticks.
 */
#define HRTIM_NS_TO_TICKS(ns) \
    ((uint32_t)((uint64_t)(ns) * (SYSCLK_HZ / 1000000UL) * HRTIM_PRESCALER_MUL / 1000UL))

/** The same for a runtime value in 32-bit arithmetic (a 64-bit divide in the
 *  PID ISR costs ~1 % of the CPU); exact for ns below 789 000.            */
#define HRTIM_NS_TO_TICKS_RT(ns) \
    (((uint32_t)(ns) * (uint32_t)((SYSCLK_HZ / 1000000UL) * HRTIM_PRESCALER_MUL)) / 1000u)

/**
 * HRTIM_PERIOD_COUNTS — HRTIM period register value; sets switching frequency.
 * Units  : HRTIM timer counts (183.82 ps/count at MUL32 prescaler)
 * Derive : SYSCLK_HZ × HRTIM_PRESCALER_MUL / HRTIM_SWITCHING_FREQ_HZ
 *          = 170e6 × 32 / 200e3 = 27 200 (exact). (§5.5)
 * Range  : [5440, 54400]   (100 kHz to 1 MHz)
 */
#define HRTIM_PERIOD_COUNTS \
    ((uint32_t)(HRTIM_PRESCALER_MUL * (SYSCLK_HZ / HRTIM_SWITCHING_FREQ_HZ)))

_Static_assert(HRTIM_PERIOD_COUNTS >= 5440u && HRTIM_PERIOD_COUNTS <= 54400u,
               "HRTIM_PERIOD_COUNTS out of valid range [5440, 54400]");

/**
 * HRTIM_BLANKING_NS_BUCK / HRTIM_BLANKING_NS_BOOST / BOOTSTRAP_REFRESH_NS
 * Source time-domain constants for blanking window and bootstrap pulse.
 * Adjust empirically using an oscilloscope on IL_MON and the switching nodes.
 */
/* 100 ns until run 4 (2026-09-27, 24 V in, no load).  At about 4.7 V out the
 * modulator fell into minimum pulses (258 ns, i.e. blanking plus the trip
 * delay) while the commanded threshold was 190 mV: the comparator tripped
 * at the end of blanking, forced CCM drove the inductor current negative,
 * and the output collapsed to -1.7 V in 40 us.  SW2 rings +-3 V for a few
 * hundred ns at each SW1 edge, so the INA281 output is not trusted before
 * that ring settles.  Run 5 with 300 ns was worse (the longer minimum pulse
 * put more energy into each cycle the modulator could not end early), and
 * the 100 us bounce it showed is the L1 / C_out resonance at no load, not
 * a comparator glitch, so 100 ns is restored.                              */
#define HRTIM_BLANKING_NS_BUCK   100u  /**< Buck blanking:  100 ns after switching edge */
#define HRTIM_BLANKING_NS_BOOST  500u  /**< Boost blanking: 500 ns covers bootstrap + ring */
#define BOOTSTRAP_REFRESH_NS     200u  /**< Bootstrap LOW pulse: 200 ns per gate driver spec */

/**
 * HRTIM_BLANKING_TICKS_BUCK — Comparator blanking window (CMP1) in buck mode.
 * Units  : HRTIM ticks; derived from HRTIM_BLANKING_NS_BUCK via HRTIM_NS_TO_TICKS.
 * Purpose: EEV4 (IL_MON comparator) is masked from period reset until CMP1 fires
 *          (HRTIM_TIMEEVFLT_BLANKINGCMP1).  Prevents false trips on switching
 *          ringing during the dead-time and turn-on transient. (§5.5, §7.3)
 * Compare: CMP1xR on Timer A (active leg in buck mode).
 */
#define HRTIM_BLANKING_TICKS_BUCK   HRTIM_NS_TO_TICKS(HRTIM_BLANKING_NS_BUCK)

/**
 * HRTIM_BLANKING_TICKS_BOOST — Comparator blanking window (CMP1) in boost mode.
 * Units  : HRTIM ticks; derived from HRTIM_BLANKING_NS_BOOST via HRTIM_NS_TO_TICKS.
 * Purpose: Must cover the bootstrap refresh pulse (BOOTSTRAP_REFRESH_TICKS) plus
 *          switching transient ringing.  Larger than buck because the boost static
 *          leg (CHA1) refresh pulse occupies the start of the period. (§5.5, §12.2)
 * Compare: CMP1xR on Timer B (active leg in boost mode).
 */
#define HRTIM_BLANKING_TICKS_BOOST  HRTIM_NS_TO_TICKS(HRTIM_BLANKING_NS_BOOST)

_Static_assert(HRTIM_BLANKING_TICKS_BUCK >= 544u &&
                   HRTIM_BLANKING_TICKS_BUCK <= 2720u,
               "HRTIM_BLANKING_TICKS_BUCK out of valid range [544, 2720]");
_Static_assert(HRTIM_BLANKING_TICKS_BOOST >= 1088u &&
                   HRTIM_BLANKING_TICKS_BOOST <= 5440u,
               "HRTIM_BLANKING_TICKS_BOOST out of valid range [1088, 5440]");

/**
 * BOOTSTRAP_REFRESH_TICKS — Duration of the bootstrap refresh LOW pulse (CMP3).
 * Units  : HRTIM ticks; derived from BOOTSTRAP_REFRESH_NS via HRTIM_NS_TO_TICKS.
 * Purpose: CMP3xR on the static leg timer is programmed to this value.  At each
 *          period reset the static leg output goes LOW (bootstrap cap charges
 *          through the gate driver bootstrap diode).  CMP3 fires after this
 *          interval and drives the output back HIGH. (§5.4)
 * Compare: CMP3xR on Timer B (buck static) or Timer A (boost static).
 * Constraint: Must be ≤ HRTIM_BLANKING_TICKS_BOOST so the refresh pulse on
 *             the boost static leg (CHA1) completes before COMP1 is unmasked.
 *             Also must be < HRTIM_PERIOD_COUNTS − MAX_ON_TIME_COUNTS to avoid
 *             overlap with the active switching phase.
 * NOTE: During the boost-mode refresh, both input (Q2) and output (Q4)
 *       low-side FETs are simultaneously ON for ~200 ns.  Inductor voltage is
 *       clamped to ~0 V; current change is negligible (ΔI ≈ 0 over 200 ns at
 *       4.7 µH).  Verify safe operation during hardware bring-up.
 */
#define BOOTSTRAP_REFRESH_TICKS  HRTIM_NS_TO_TICKS(BOOTSTRAP_REFRESH_NS)

_Static_assert(BOOTSTRAP_REFRESH_TICKS <= HRTIM_BLANKING_TICKS_BOOST,
               "BOOTSTRAP_REFRESH_TICKS must fit within boost blanking window");

/**
 * SYNC_RECT_ENABLED — Drive the low-side switch of the active leg (Q2 in
 * buck) as the synchronous rectifier.  Added 2026-09-27 after runs 1 to 5.
 *
 * With 1 the leg runs in forced continuous conduction: whenever the high
 * side is off the low side is on, so at no load the inductor current goes
 * negative every cycle (V_out / L for the rest of the period: about 5 A at
 * 5 V out), the output collapses within one PID period, and because the
 * INA281 current sense is unidirectional the comparator never sees the
 * negative current and the next charge phase runs to the backstop (runs 1,
 * 4 and 5: SW_OVP from 10 to 13 V overshoots).
 *
 * With 0 the inductor current freewheels through the low-side GaN FET's
 * reverse conduction and stops at zero (discontinuous conduction), and
 * regulator.c skips the charge pulses while V_out is above the setpoint.
 * The low side is still switched, but only for the bootstrap refresh: Q2 is
 * on for BOOTSTRAP_REFRESH_NS at the start of every period and the charge
 * pulse starts DCM_REFRESH_DEADTIME_NS after it ends.  Runs 6 to 18
 * (2026-09-27) left Q2 off entirely; the high-side bootstrap then only
 * charged while SW1 sat below the 5 V rail, Q1's gate drive starved, the
 * "pulses" on TP3 were 20 ns needles ringing at 25 MHz, and Q1 conducted
 * as a resistor of a few hundred ohms (about 60 mA into the 33 ohm bench
 * load whatever the DAC asked for, and it charged the idle output to 8 V
 * from Vin with every output disabled).  The cost of 0 is the
 * reverse-conduction drop (about 2 V for an EPC2302, dissipative at load).
 * Set to 1 once the converter regulates under a load that keeps the
 * inductor current positive, or once diode emulation exists.
 */
#define SYNC_RECT_ENABLED 0u

/**
 * OUTPUT_SWITCH_ENABLED — Whether regulator_start() asserts OUTPUT_EN.
 * Units  : boolean (0u or 1u)
 * Value  : 1u (bench, 2026-09-28 evening: boost at 28 V into a 330 ohm
 *          load, 85 mA / 2.4 W; Dan)
 * Purpose: With 1u the power path drives OUTPUT_EN high with INPUT_EN, so
 *          the Q5/Q6 output switch (through the R27/Q9 buffer Dan added
 *          2026-09-28) connects VBUS, and with it the 2 W 33 ohm bench load.
 *          With 0u OUTPUT_EN stays low for the whole run: the regulator runs
 *          into VOUTa alone (the scope is before the output FETs, Dan
 *          2026-09-27) and the load is out of circuit.  Set to 0u for the
 *          boost runs, where 28 V into 33 ohm would be 24 W.  OUTPUT_DIS is
 *          driven the same in either case.
 * Adjust : 1u for buck runs that want the bench load; back to 1u for real
 *          operation once the output switch is characterised.
 */
#define OUTPUT_SWITCH_ENABLED 1u

/**
 * OUTPUT_EN_SHARED_WITH_TCPP — PC8 also drives the TCPP0203 ENABLE pin.
 * Units  : boolean (0u or 1u)
 * Value  : 1u (2026-09-29: X-NUCLEO-SRC1M1 stacked on the NUCLEO for the
 *          USB PD stack)
 * Purpose: With 1u the regulator never writes OUTPUT_EN; pd_power.c holds
 *          PC8 high so the TCPP stays enabled (attach detection needs it).
 *          The power board's output switch then stays on while the board is
 *          powered, so its output must feed a switched path (the SRC1M1
 *          VIN, whose VBUS gate the PD stack closes), never a bare load.
 *          Back to 0u when the shield is off and OUTPUT_EN is the output
 *          switch again (step 4c gating, OUTPUT_SWITCH_ENABLED).
 */
#define OUTPUT_EN_SHARED_WITH_TCPP 1u

/**
 * OUTPUT_CONNECT_MARGIN_MV — How close V_out must be to the target before
 *          the PID ISR asserts OUTPUT_EN (step 4c in regulator.c).
 * Units  : mV
 * Value  : 1000u (2026-09-28)
 * Purpose: With OUTPUT_EN asserted at start, boost 28 V into 330 ohm
 *          faulted SW_UVP after 5 ms: the buck precharge sat at the 600 ns
 *          DCM_MAX_ON_TIME_NS cap and V_out stalled at 13.6 V, because
 *          charge per pulse falls as V_out nears V_in and the load took it
 *          all.  Connecting the load only once the final mode is regulating
 *          keeps the start at no load (runs 30 to 38) and makes the load a
 *          step the loop has to hold.
 */
#define OUTPUT_CONNECT_MARGIN_MV 1000u

/**
 * DCM_MAX_PEAK_MA — Peak inductor current the on-time law may command
 *          (SYNC_RECT_ENABLED 0).  The PID output is clamped to it too, so the
 *          integrator cannot wind past it.
 * Units  : mA
 * Value  : 2500u (2026-09-28 evening)
 * Purpose: DCM_MAX_ON_TIME_NS alone capped the pulse at 600 ns whatever the
 *          operating point.  That is 2.4 A at 5 V out from 24 V, but only
 *          1.2 A at 13.6 V, where charge per pulse was down to the 330 ohm
 *          load's need and buck stalled (boost start into the load, SW_UVP).
 *          Near V_in a fixed peak current delivers more charge per pulse, so
 *          the peak is the limit that keeps authority up there; 2.5 A keeps
 *          the low-V_out behaviour of runs 21 to 29.
 */
#define DCM_MAX_PEAK_MA 2500u

/**
 * DCM_MAX_PEAK_MA_BB — The same limit in buck-boost.  A buck-boost pulse
 *          delivers only L * I_pk^2 / 2 (2.9 W at 2.5 A and 200 kHz, before
 *          two diode drops): at 2.5 A the output held 18.3 V against a 24 to
 *          26 V target with the PID railed (2026-09-28 evening, 330 ohm).
 *          4 A is 7.5 W ideal, well under L1's 21 A saturation.
 * Units  : mA
 */
#define DCM_MAX_PEAK_MA_BB 4000u

/**
 * DCM_ON_TIME_CEIL_NS — Longest computed on-time (PID ISR step 6b).  The
 *          CMP2 value written at start and on a mode change is still
 *          DCM_MAX_ON_TIME_NS until the first PID cycle.
 * DCM_WINDOW_MARGIN_NS — Slack left at the end of the period so the inductor
 *          current reaches zero before the next pulse (discontinuous mode).
 * Units  : ns
 */
#define DCM_ON_TIME_CEIL_NS  4000u
#define DCM_WINDOW_MARGIN_NS 300u

/** DCM_DIODE_DROP_MV — body-diode drop in the freewheel path, for the DCM
 *  on-time bound (PID ISR step 6b).  Units: mV. */
#define DCM_DIODE_DROP_MV 700u

/**
 * SETPOINT_SLEW_MV_PER_CYCLE — How fast the regulated setpoint follows a new
 *          target while running (PID ISR step 4a).
 * Units  : mV per PID cycle (20 kHz): 1 is 20 V/s
 * Purpose: A step down (28 V to 20 V) put V_out over the relative OVP of the
 *          new target at once: the output only falls as fast as the load
 *          discharges it.  OVP/UVP are checked against the slewed value.
 *          At 20 V/s a 330 ohm load keeps up down to about 1 V at 25 uF.
 */
#define SETPOINT_SLEW_MV_PER_CYCLE 1u

/**
 * BUCK_BOOST_ENABLED — Use REGULATOR_MODE_BUCK_BOOST for setpoints near V_in.
 *          Q1 (TA1) and Q4 (TB2) pulse together for t_on = I_pk * L / V_in,
 *          then L1 freewheels from ground through Q2's and into the output
 *          through Q3's body diodes, so each pulse delivers L * I_pk^2 / 2
 *          at any V_out/V_in (discontinuous, non-synchronous).
 * BB_BELOW_VIN_MV / BB_ABOVE_VIN_MV — Band around V_in (filtered) where it is
 *          selected; BB_HYSTERESIS_MV on each edge.
 * Units  : boolean; mV
 */
#define BUCK_BOOST_ENABLED 1u
#define BB_BELOW_VIN_MV   2000u
#define BB_ABOVE_VIN_MV   3000u
#define BB_HYSTERESIS_MV   500u

/**
 * VIN_FILTER_SHIFT — V_in low-pass for mode selection: filtered += (sample -
 *          filtered) >> shift per PID cycle.  5 is a 1.6 ms time constant;
 *          single V_in samples read up to 4 V off (run 36).
 */
#define VIN_FILTER_SHIFT 5u

/**
 * ADC_VD_SAMPLE_END_NS — Where in the switching period VD_MON's sample ends
 *          (ADC1 is triggered once per period by HRTIM master compare 1).
 *          4.85 us: after the pulse and inductor discharge at every point
 *          swept (they end by about 2.5 us), before the next period's Q2
 *          refresh edge at 0.
 * ADC_VD_SAMPLING_NS — The window VD_MON's result averages: 8 conversions
 *          of 6.5 + 12.5 ADC cycles at 42.5 MHz (PCLK/4), 3.58 us, so the
 *          result is the mean over 1.27 to 4.85 us of the period.
 * Units  : ns
 */
#define ADC_VD_SAMPLE_END_NS 4850u
#define ADC_VD_SAMPLING_NS   3576u
#define ADC_TRIGGER_TICKS    HRTIM_NS_TO_TICKS(ADC_VD_SAMPLE_END_NS - ADC_VD_SAMPLING_NS)

/**
 * AWD_FILTER_SAMPLES — Consecutive VD_MON samples (one per period) outside
 *          the skip window before skipping toggles (1 to 8), checked in the
 *          watchdog ISR against the DMA ring (the hardware AWDFILT does not
 *          work with a multi-channel scan, adc_monitor.h).  2 rejects a
 *          single bad sample at 5 us of lag.
 * VOUT_MEDIAN_SAMPLES — Samples in the trimmed mean V_out the PID uses
 *          (highest and lowest dropped).
 */
#define AWD_FILTER_SAMPLES  2u
#define VOUT_MEDIAN_SAMPLES 9u

/**
 * DCM_REFRESH_DEADTIME_NS — Gap between the low-side refresh pulse and the
 * high-side charge pulse on the same leg while SYNC_RECT_ENABLED is 0.
 * Units  : ns
 * Purpose: With dead-time insertion off on Timer A (its two outputs are
 *          programmed separately, see hrtim_configure_compare_registers)
 *          this is the only thing keeping Q1 and Q2 from overlapping.
 *          Both edges are HRTIM compare events, so the gap is exact to a
 *          tick plus driver skew; 50 ns is several times the EPC2302
 *          switching times.
 */
#define DCM_REFRESH_DEADTIME_NS    50u
#define DCM_REFRESH_DEADTIME_TICKS HRTIM_NS_TO_TICKS(DCM_REFRESH_DEADTIME_NS)

/**
 * DCM_PULSE_START_TICKS — Timer A CMP4: where the buck charge pulse starts
 * (the end of the refresh pulse plus the gap).  Timer A CMP1 (blanking end)
 * and CMP2 (backstop) are offset by the same amount in regulator.c.
 */
#define DCM_PULSE_START_TICKS (BOOTSTRAP_REFRESH_TICKS + DCM_REFRESH_DEADTIME_TICKS)

/**
 * DCM_MAX_ON_TIME_NS — Charge-pulse length cap while SYNC_RECT_ENABLED is 0.
 * Units  : ns
 * Purpose: The CMP2 backstop is the only on-time limit that has been seen
 *          to act; the peak-current comparator has not yet been observed
 *          ending a pulse (IL_MON has not been on a scope channel).  It
 *          therefore sets the converter's authority.  Run 21 (2026-09-28,
 *          24 V in, 33 ohm load, 5 V target) ran 406 ns pulses every
 *          period with 8 percent skipped, the PID output saturated at
 *          PID_OUTPUT_MAX, and V_out held at 4.79 V on the scope: 400 ns
 *          delivers about 0.8 uC per pulse into the output (145 mA at
 *          200 kHz), not the 1.25 uC the ideal ramp predicts, and could not
 *          reach 5 V.  Charge per pulse scales about as the square of the
 *          on-time, so 600 ns is about 1.8 uC (360 mA at 5 V) at about
 *          2.5 A peak in L1 (IHLP-6767 4.7 uH) and Q1.  Run 6 (2026-09-27,
 *          no load) had seen 1.1 to 1.5 us pulses at 6 A peak overshoot
 *          the setpoint; those ran before the Q1 bootstrap starvation
 *          (see SYNC_RECT_ENABLED) was understood, so they say nothing
 *          about the comparator.
 */
#define DCM_MAX_ON_TIME_NS 600u

/**
 * DCM_MIN_ON_TIME_NS — Shortest charge pulse the firmware will command while
 *                      SYNC_RECT_ENABLED is 0.
 * Units  : ns
 * Purpose: The PID ISR sets Timer A CMP2 (the TA1 reset) each PID period
 *          from its peak-current command through t_on = I_pk * L /
 *          (V_in - V_out) (regulator.c, "Predicted on-time").  Below the
 *          blanking window (HRTIM_BLANKING_NS_BUCK) the comparator could not
 *          act anyway, and a shorter pulse than the gate drivers resolve
 *          is not a smaller pulse, so the command floors here and pulse
 *          skipping handles anything lighter.  Why the on-time is computed
 *          at all: runs 22 and 26 (2026-09-28, 24 V in, 33 ohm load) ran
 *          every pulse to the CMP2 backstop with DAC thresholds of 3 V and
 *          280 mV alike, while run 25 with a 21 mV threshold trimmed pulses
 *          to almost nothing.  The comparator path therefore works only for
 *          thresholds below roughly 150 mV (0.6 A); IL_MON has not been on a
 *          scope channel, so whether the INA281 output is slow, filtered or
 *          clipped before PA1 is open.
 */
#define DCM_MIN_ON_TIME_NS 100u

/**
 * PULSE_SKIP_ABOVE_MV — How far above the setpoint pulse skipping engages
 *                       while SYNC_RECT_ENABLED is 0.
 * Units  : mV above the effective (soft-start) setpoint
 * Purpose: In DCM the smallest pulse the modulator can make still moves
 *          V_out, so at no load the only way down is to make no pulse; the
 *          Timer A period ISR skips pulses while the V_out estimate is above
 *          setpoint + this (SKIP_HYSTERESIS_RAW below it to
 *          resume).  Skipping is the guard for that case only.  The PID owns
 *          the setpoint through the peak-current DAC.  With the skip band
 *          at the setpoint itself (runs 7 to 22) the estimate hovered just
 *          under the setpoint, the PID saw a small positive error for ever
 *          and railed at PID_OUTPUT_MAX, every pulse ran to the CMP2
 *          backstop, and the converter was a burst-mode hysteretic loop
 *          (run 22, 2026-09-28, 33 ohm load: 0.6 V ripple at 5 V, 57
 *          percent of periods skipped).  200 mV is about two full buck
 *          pulses at 5 V.  It was 4 percent until run 37 (2026-09-28, boost
 *          24 V to 28 V, no load): there 4 percent is 1.12 V, a boost pulse
 *          moves V_out only a few mV, and V_out sat at the skip threshold,
 *          29.1 V, with the PID at its floor.  The band is set by what one
 *          pulse does, which does not scale with the setpoint, so it is in
 *          mV.  Must stay under the relative OVP margin at SETPOINT_MIN_MV
 *          (asserted after OVP_RELATIVE_PCT).
 */
#define PULSE_SKIP_ABOVE_MV 200u

/** SKIP_ABOVE_PERMILLE_DEFAULT — The skip band's proportional part, per mille
 *  of the setpoint (regulator_skip_above_permille, runtime); the band is the
 *  larger of this and PULSE_SKIP_ABOVE_MV.  Must stay under OVP_RELATIVE_PCT. */
#define SKIP_ABOVE_PERMILLE_DEFAULT 15u

/**
 * MAX_DUTY_CYCLE_PCT — Maximum allowed charge-phase duty cycle.
 * Units  : percent of switching period
 * Derive : 85 % leaves ~750 ns at 200 kHz for the discharge phase and
 *          bootstrap refresh (≥200 ns).  Constraint: backstop_counts ≤
 *          period − refresh_ticks. 85 % × 27200 = 23120; 27200 − 1088 =
 *          26112 → 23120 < 26112 ✓  (§10.3)
 * Range  : [50, 96]
 */
/* 85 until run 4 (2026-09-27): the current sense is unidirectional, so
 * once the inductor current is negative the comparator never trips and the
 * charge phase runs to this backstop; 4.25 us at 4 A/us put 10 V on the
 * output before the software OVP saw it.  50 (the lowest the assert below
 * allows) halves that.  Buck at 24 V in needs 21 % for 5 V out.        */
#define MAX_DUTY_CYCLE_PCT 50u

_Static_assert(MAX_DUTY_CYCLE_PCT >= 50u && MAX_DUTY_CYCLE_PCT <= 96u,
               "MAX_DUTY_CYCLE_PCT out of valid range [50, 96]");

/**
 * MAX_ON_TIME_COUNTS — HRTIM Compare 2 value for hardware backstop.
 * Units  : HRTIM timer counts
 * Derive : HRTIM_PERIOD_COUNTS × MAX_DUTY_CYCLE_PCT / 100. (§10.3)
 *          27200 × 85 / 100 = 23120 counts at 200 kHz.
 * Purpose: Hardware Compare 2 match ends the charge phase if COMP1 (EEV4)
 *          has not fired by this point in the period.  Prevents unbounded
 *          inductor current ramp when COMP1 is inactive.
 * Compare: CMP2xR on both Timer A (buck active) and Timer B (boost active).
 */
#if SYNC_RECT_ENABLED
#define MAX_ON_TIME_COUNTS ((HRTIM_PERIOD_COUNTS) * (MAX_DUTY_CYCLE_PCT) / 100u)
#else
#define MAX_ON_TIME_COUNTS HRTIM_NS_TO_TICKS(DCM_MAX_ON_TIME_NS)
#endif

_Static_assert(BOOTSTRAP_REFRESH_TICKS <
               (HRTIM_PERIOD_COUNTS - MAX_ON_TIME_COUNTS),
               "BOOTSTRAP_REFRESH_TICKS must not overlap active switching phase");

_Static_assert(DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS + DCM_REFRESH_DEADTIME_TICKS <
               HRTIM_PERIOD_COUNTS,
               "DCM charge pulse must end before the next refresh pulse");

/* =========================================================================
 * SECTION 7: SLOPE COMPENSATION TIMER (TIM6) CONSTANTS
 * =========================================================================*/

/**
 * SLOPE_COMP_ENABLED — Run the TIM6 staircase ramp (§7.5 option 1).
 *
 * SHELVED (2026-09-27, bench bring-up): the 2 MHz TIM6 ISR at NVIC priority 0
 * leaves ~85 CPU cycles per interrupt at 170 MHz, and ISR entry/exit plus
 * the body consume essentially all of them.  Once regulator_start() started
 * TIM6 the CPU never returned to thread level: regulator_start() did not
 * complete (outputs enabled, state stuck at IDLE, TIM7 PID never started)
 * and FreeRTOS tasks stopped running.  The §7.5 estimate of 12–36 % CPU did
 * not hold.
 *
 * With 0: TIM6 is never started; DAC3 CH1 holds the PID peak for the whole
 * period (reloaded each period by the HRTIM Timer A ISR), i.e. plain peak
 * current mode with NO slope compensation.  Subharmonic oscillation is
 * expected above ~50 % duty (§7.1) — keep duty < 50 % until a CPU-free ramp
 * (DAC3 hardware sawtooth reset by the HRTIM period, §7.5 option 3) or a
 * DMA-driven ramp (option 2) replaces this.
 */
#define SLOPE_COMP_ENABLED 0u

/**
 * TIM6_RATE_HZ — Slope compensation timer interrupt rate.
 * Units  : Hz
 * Value  : 2 MHz (10 ticks per switching period at 200 kHz; 4 at 500 kHz)
 * Purpose: Drives DAC3 CH1 staircase ramp.  Rate >> f_sw ensures smooth
 *          ramp approximation (§7.5 CPU budget: ~10–30 cycles/ISR).
 */
#define TIM6_RATE_HZ 2000000u

/**
 * TIM6_PRESCALER — TIM6 prescaler register value (PSC).
 * Units  : register value (0 = ÷1)
 * Derive : APBCLK / (PSC+1) / (ARR+1) = TIM6 rate.  PSC=0 → no division.
 *          (§7.3)
 */
#define TIM6_PRESCALER 0u

/**
 * TIM6_PERIOD_COUNTS — TIM6 auto-reload register value (ARR).
 * Units  : timer counts
 * Derive : ARR = SYSCLK_HZ / TIM6_RATE_HZ − 1 = 170e6 / 2e6 − 1 = 84. (§7.3)
 *          Period = (PSC+1)(ARR+1)/SYSCLK_HZ = 1×85/170e6 = 500 ns = 2 MHz ✓
 */
#define TIM6_PERIOD_COUNTS ((SYSCLK_HZ / TIM6_RATE_HZ) - 1u)

/* =========================================================================
 * SECTION 8: PID TIMER (TIM7) CONSTANTS
 * =========================================================================*/

/**
 * PID_EXECUTION_RATE_HZ — Default PID outer-loop execution rate.
 * Units  : Hz
 * Range  : [1000, 500000]
 * Value  : 20 kHz (every 10th switching period at 200 kHz). (§8.2)
 */
#define PID_EXECUTION_RATE_HZ 20000u

_Static_assert(PID_EXECUTION_RATE_HZ >= 1000u &&
                   PID_EXECUTION_RATE_HZ <= 500000u,
               "PID_EXECUTION_RATE_HZ out of valid range [1000, 500000]");

/**
 * TIM7_PRESCALER — TIM7 prescaler register value (PSC).
 * Units  : register value (0 = ÷1)
 */
#define TIM7_PRESCALER 0u

/**
 * TIM7_PERIOD_COUNTS — TIM7 auto-reload register value (ARR).
 * Units  : timer counts
 * Derive : ARR = SYSCLK_HZ / PID_EXECUTION_RATE_HZ − 1
 *          = 170e6 / 20e3 − 1 = 8499. (§8.2)
 *          Period = (0+1)(8499+1)/SYSCLK_HZ = 8500/170e6 = 50 µs = 20 kHz ✓
 */
#define TIM7_PERIOD_COUNTS ((SYSCLK_HZ / PID_EXECUTION_RATE_HZ) - 1u)

/* =========================================================================
 * SECTION 9: NVIC PRIORITY ASSIGNMENTS
 * =========================================================================*/

/**
 * NVIC priority assignments (§13.2).
 * FreeRTOS configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5 (confirmed IOC).
 * Regulator ISRs must be numerically less than 5 (higher urgency).
 * FreeRTOS API calls are prohibited from these ISRs.
 */

/** TIM6 (slope compensation) — most latency-sensitive; fires every 500 ns. */
#define NVIC_PRIORITY_SLOPE_COMP_TIM6 0u

/** HRTIM period + fault — must reload DAC before blanking window expires. */
#define NVIC_PRIORITY_HRTIM 1u

/** TIM7 (PID) and ADC completion — paired; must be below HRTIM. */
#define NVIC_PRIORITY_PID_TIM7 2u

/** UCPD1 / PD stack — FreeRTOS-managed (confirmed IOC: priority 5). */
#define NVIC_PRIORITY_UCPD1 5u

/* =========================================================================
 * SECTION 10: PID CONTROLLER PARAMETERS
 * =========================================================================*/

/**
 * PID_OUTPUT_MIN — Minimum PID output (DAC counts).
 * Units  : DAC counts (0–4095)
 * Value  : 0  (zero current threshold → regulator effectively idle)
 */
#define PID_OUTPUT_MIN 0

/**
 * PEAK_FLOOR_NEG_MARGIN_MA — How far below zero the average inductor current
 * may be commanded by the per-cycle peak-current floor (added 2026-09-27).
 * Units  : mA
 * Purpose: In forced continuous conduction the low-side switch conducts
 *          whenever the high side is off, so a peak-current setpoint of zero
 *          drives a large negative average inductor current.  At no load the
 *          output then collapses within one PID period (bench, run 1: V_out
 *          366, 0, 1127, 0, 3720, 0 mV on successive cycles).  regulator.c
 *          floors the PID output each cycle at ripple/2 − this margin, so
 *          the average inductor current cannot be commanded below −margin.
 *          0 would forbid any negative current, leaving no way to bleed an
 *          overshoot at no load; a few hundred mA is a controllable way down
 *          (300 mA into 40 µF is 0.4 V per PID period).
 */
#define PEAK_FLOOR_NEG_MARGIN_MA 300u

/**
 * PEAK_TRIP_DELAY_NS — Delay from the inductor current reaching the DAC
 * threshold to the high-side gate actually turning off (added 2026-09-27).
 * Units  : ns
 * Purpose: INA281 (1.3 MHz), COMP1, HRTIM and the gate driver act this long
 *          after the threshold is crossed, and the current keeps rising at
 *          (V_in − V_out)/L meanwhile, so the real peak is the threshold plus
 *          di/dt × t_d: about 0.8 A at 24 V in, 5 V out, 4.7 µH.  regulator.c
 *          lowers the DAC by that amount so the PID output is the peak that
 *          actually happens.  Bench estimate from run 2 (V_out rose 1.0 V and
 *          1.5 V per PID period on commands that allowed no net current);
 *          measure it with a probe on IL_MON and the switch node, and set it.
 */
#define PEAK_TRIP_DELAY_NS 200u

/**
 * PID_OUTPUT_MAX — Maximum PID output (DAC counts).
 * Units  : DAC counts (0–4095)
 * Derive : 4000 counts × 3.3 V / 4096 / 0.250 V·A⁻¹ ≈ 12.9 A peak
 *          inductor current.  Rationale: (§8.6)
 *            - DAC ceiling: 4095 → 13.2 A; PID_OUTPUT_MAX = 4000 leaves
 *              small margin below the DAC rail.
 *            - Inductor I_sat = 21 A (IHLP6767GZER4R7M11) → 4000 counts
 *              is 61 % of I_sat → adequate margin.
 *            - FET peak: EPC2306 = 200 A, EPC2302 = 400 A (Dan v0.1.2j)
 *              → FET peak far exceeds this ceiling; inductor saturation
 *              is the operative hardware constraint.
 * NOTE   : Peak *inductor* current ≠ output current.  In boost mode
 *          I_L_avg = I_out / (1−D); a 5 A output at D=0.75 requires
 *          I_L ≈ 20 A.  A 5 A inductor limit would prevent boost-mode
 *          operation entirely.
 */
#define PID_OUTPUT_MAX 4000

_Static_assert(PID_OUTPUT_MAX > PID_OUTPUT_MIN && PID_OUTPUT_MAX <= 4095,
               "PID_OUTPUT_MAX must be in (PID_OUTPUT_MIN, 4095]");

/* =========================================================================
 * SECTION 11: VOLTAGE SETPOINT LIMITS
 * =========================================================================*/

/**
 * SETPOINT_MIN_MV — Minimum valid voltage setpoint.
 * Units  : millivolts
 * Value  : 5000 mV (USB PD SPR minimum, 5 V). (§9.2)
 */
#define SETPOINT_MIN_MV 5000u

/**
 * SETPOINT_MAX_MV — Maximum valid voltage setpoint.
 * Units  : millivolts
 * Value  : 48000 mV (USB PD EPR maximum, 48 V). (§9.2)
 */
#define SETPOINT_MAX_MV 48000u

/* =========================================================================
 * SECTION 12: SOFTWARE SAFETY THRESHOLDS
 * =========================================================================*/

/**
 * OVP_ABSOLUTE_MV — Absolute output overvoltage protection threshold.
 * Units  : millivolts
 * Derive : 110 % × 48 V EPR maximum = 52.8 V. (§10.2)
 *          Hardware ceiling is ADM1270 OVP at 60 V.
 * NOTE   : This is a compile-time constant — NOT runtime-tunable.
 *          It represents a hard safety limit below the hardware OVP.
 */
#define OVP_ABSOLUTE_MV 52800u

/**
 * OVP_RELATIVE_PCT — Relative output overvoltage threshold (% of setpoint).
 * Units  : percent
 * Value  : 110  (10 % overshoot above setpoint triggers fault). (§10.2)
 * Range  : [105, 150]
 */
#define OVP_RELATIVE_PCT 110u
_Static_assert(PULSE_SKIP_ABOVE_MV * 100u < SETPOINT_MIN_MV * (OVP_RELATIVE_PCT - 100u),
               "pulse skipping must engage below the relative OVP");

/**
 * UVP_RELATIVE_PCT — Relative output undervoltage threshold (% of setpoint).
 * Units  : percent
 * Value  : 50  (50 % below setpoint indicates loss of regulation). (§10.2)
 * Range  : [20, 90]
 */
#define UVP_RELATIVE_PCT 50u

/**
 * MODE_HYSTERESIS_MV — Hysteresis band around V_in ≈ V_out for mode selection.
 * Units  : millivolts
 * Value  : 1000 mV (1.0 V). (§5.1, Round 3 Q8)
 * Purpose: Prevents mode chattering when V_in ≈ V_setpoint.
 * Adjust : Empirically if chattering occurs near the buck/boost boundary.
 */
#define MODE_HYSTERESIS_MV 1000u

/**
 * BUCK_VIN_MARGIN_MV — Additional V_in margin required above V_out in buck
 * mode. Units  : millivolts Value  : 500 mV (0.5 V guard band). Purpose:
 * Ensures the converter can maintain regulation in buck mode (§10.2).
 *          V_in_min(buck) = V_out_target + MODE_HYSTERESIS_MV (used for mode
 *          selection); BUCK_VIN_MARGIN_MV is the fault-trigger margin.
 * TODO(hardware): Verify this margin is sufficient at the actual operating
 *                 point during bring-up.
 */
#define BUCK_VIN_MARGIN_MV 500u

/**
 * BOOST_VIN_MARGIN_MV — Additional V_out margin required above V_in in boost
 * mode. Units  : millivolts Value  : 500 mV (0.5 V guard band). Purpose:
 * Ensures the converter can maintain regulation in boost mode (§10.2).
 * TODO(hardware): Verify during bring-up.
 */
#define BOOST_VIN_MARGIN_MV 500u

/**
 * BOOST_PRECHARGE_BELOW_VIN_MV — How far under V_in the buck leg precharges
 * the output before a boost start hands over to boost (regulator.c,
 * boost_precharge).
 * Units  : millivolts
 * Value  : 2000 mV (2026-09-28, after runs 32 and 33 rang to 36 to 38 V at a
 *          28 V target from an empty output)
 * Purpose: Q1 turning fully on steps V_in minus V_out across L1 and C_out;
 *          the undamped ring reaches up to twice that step above V_out and
 *          Q3's reverse conduction holds the peak.  2 V under 24 V in is at
 *          most about 26 V, under a 28 V target.
 */
#define BOOST_PRECHARGE_BELOW_VIN_MV 2000u

/**
 * BOOST_PRECHARGE_DONE_MARGIN_MV — How close V_out must be to the precharge
 * cap before the hand-over to boost.
 * Units  : millivolts
 * Value  : 500 mV
 */
#define BOOST_PRECHARGE_DONE_MARGIN_MV 500u

/**
 * MAX_CONSECUTIVE_BACKSTOPS_DEFAULT — Default consecutive-backstop fault
 * threshold. Units  : count Value  : 3 (runtime-mutable, see §10.4) Range  :
 * [1, 10]
 */
#define MAX_CONSECUTIVE_BACKSTOPS_DEFAULT 3u

/* =========================================================================
 * SECTION 13: SOFT-START PARAMETER
 * =========================================================================*/

/**
 * SOFT_START_RAMP_MS — Soft-start ramp duration.
 * Units  : milliseconds
 * Value  : 5 ms (advisory, not safety-critical; see §11.2)
 * Derive : Sets setpoint ramp from 0 to V_target over this interval.
 *          At 20 kHz PID rate = 100 ramp steps.
 */
#define SOFT_START_RAMP_MS 5u

/* =========================================================================
 * SECTION 14: ADC SCALING CONSTANTS
 * =========================================================================*/

/**
 * ADC_FULL_SCALE_COUNTS — 12-bit ADC full-scale value.
 * Units  : ADC counts
 * Value  : 4096 (2^12)
 */
#define ADC_FULL_SCALE_COUNTS 4096u

/**
 * VREF_MV — ADC voltage reference.
 * Units  : millivolts
 * Value  : 3300 mV (V_DDA = 3.3 V)
 */
#define VREF_MV 3300u

/**
 * VD_MON_FULL_SCALE_MV / VS_MON_FULL_SCALE_MV — Full-scale physical voltage
 * for the two voltage channels, one constant each, calibrated on the bench.
 * Units  : millivolts
 * Derive : Nominal, from the divider: R_low / (R_high + R_low)
 *          = 5820 / (100000 + 5820) = 0.05500; 3.3 V / 0.05500 = 60.0 V, so
 *          V_mV = raw × 60000 / 4096 ≈ 14.65 mV/count (§3.4, Appendix D).
 *          Measured 2026-09-28 07:2x against the DS1104Z (10x probes) with
 *          the regulator running buck at maximum pulse into 33 ohm: VD_MON
 *          averaged 3794 mV over 512 PID samples while CH4 on TP2 averaged
 *          4.955 V (ratio 1.306; the same ratio, 1.27 to 1.31, at 5.0 V in
 *          run 14 and at 7.2 V idle), and VS_MON averaged 25278 mV while CH1
 *          on TP1 averaged 23.95 V (ratio 0.947).  Dan, 2026-09-28 07:36:
 *          the oscilloscope is right, the ADC needs calibration.  Gain only;
 *          no offset was resolvable from those points.  The cause of the
 *          30 percent VD_MON error is not known.
 */
/* 2026-09-28 evening: 80600 (x1.028) for the HRTIM-triggered, 8x oversampled
 * VD_MON (6.5-cycle samples, 1.6 M conversions/s): the scope on TP2 read
 * 1.7, 2.7 and 3.0 percent above the ADC at 12, 20 and 28 V.  The likely
 * cause is the sample capacitor's charge draw through the ~5.5 kOhm divider
 * at that rate (about 10 uA), a gain error that did not show at 12.5 kHz.
 * Recalibrate if the ADC1 sampling time, ratio or trigger rate changes. */
#define VD_MON_FULL_SCALE_MV 80600u   /* was 78400 (60000 x 1.306) */
#define VS_MON_FULL_SCALE_MV 56800u   /* 60000 × 0.947 */

/**
 * ADC_IL_FULL_SCALE_MA — Full-scale physical current for IL_MON / IS_MON.
 * Units  : milliamps
 * Derive : Sensitivity = R_shunt × Gain = 5 mΩ × 50 = 250 mV/A.
 *          ADC full-scale = 3.3 V → full-scale current = 3.3 / 0.250 = 13.2 A.
 *          Scaling: I_mA = ADC_raw × 13200 / 4096 ≈ 3.22 mA/count.
 *          (§3.3, §6.2, Appendix D)
 */
#define ADC_IL_FULL_SCALE_MA 13200u

/**
 * ADC_ID_FULL_SCALE_MA — Full-scale physical current for ID_MON.
 * Units  : milliamps
 * Derive : Sensitivity = 12 mΩ × 50 = 600 mV/A.
 *          Full-scale = 3.3 / 0.600 = 5.5 A.
 *          Scaling: I_mA = ADC_raw × 5500 / 4096 ≈ 1.34 mA/count.
 *          (§3.3, Appendix D)
 */
#define ADC_ID_FULL_SCALE_MA 5500u

/* =========================================================================
 * SECTION 15: DAC3 SCALING CONSTANTS (Slope Compensation / Current Setpoint)
 * =========================================================================*/

/**
 * DAC_FULL_SCALE_COUNTS — 12-bit DAC full-scale value.
 * Units  : DAC counts
 * Value  : 4096 (2^12); maximum usable count = 4095.
 */
#define DAC_FULL_SCALE_COUNTS 4096u

/**
 * DAC_COUNTS_PER_AMP — DAC counts per ampere of peak inductor current.
 * Units  : counts / A
 * Derive : I_peak × Sensitivity / (V_REF / DAC_counts)
 *          = I_peak × 0.250 V/A / (3.3 V / 4096)
 *          = I_peak × 0.250 / 0.000806
 *          ≈ 310.2 counts/A  →  use 310 counts/A for integer math. (§6.2)
 * NOTE   : Direction is: more DAC counts → higher comparator threshold →
 *          higher peak current before COMP1 trips.
 */
#define DAC_COUNTS_PER_AMP 310u

/**
 * SLOPE_STEP_NUMERATOR — Numerator for slope-step integer computation.
 * SLOPE_STEP_DENOMINATOR — Denominator for slope-step integer computation.
 *
 * Purpose: At each TIM6 tick, DAC3 CH1 decreases by step_counts, where:
 *   step_counts = V_out_V / L_H × DAC_COUNTS_PER_AMP / TIM6_RATE_HZ   (buck)
 *              = V_out_mV × DAC_COUNTS_PER_AMP / (L_uH × TIM6_RATE_HZ × 1000)
 *
 * For integer arithmetic:
 *   step_counts = V_out_mV × SLOPE_STEP_NUMERATOR / SLOPE_STEP_DENOMINATOR
 *
 * Derive: L = 4.7 µH = 4700 nH.
 *   step = V_out_mV × 310 / (4700 × 2000000 / 1000)
 *        = V_out_mV × 310 / 9400000
 *        = V_out_mV × 31 / 940000
 *
 * At V_out = 20 V = 20000 mV:  step = 20000 × 31 / 940000 = 0.66 → rounds to 1.
 * NOTE: This gives approximately 1 count/tick at 20 V. The spec says the
 * exact slope formula uses floating-point for pre-computation (§7.2); the
 * ISR itself only subtracts the precomputed integer step value.
 *
 * Cross-check at 20 V: step_float = (20/4.7e-6) × 310.2 / 2e6 = 0.660.
 * Integer approximation: 20000 × 31 / 940000 = 0.66 → truncates to 0!
 *
 * TODO: For a more accurate integer formula, scale the numerator/denominator.
 * Recommend: step_counts = (uint32_t)(V_out_mv * 33) / 1000000;
 * Cross-check: 20000 × 33 / 1000000 = 0.66 → still 0 with integer division.
 * The correct approach is floating-point pre-computation in pid_isr (§7.2):
 *   slope_comp_update_step() computes step_counts as a float, then stores
 *   the integer floor. Values < 1 should map to 1 to ensure the ramp moves.
 *
 * (§7.2, §7.3, Appendix E)
 */
#define INDUCTOR_VALUE_UH                                                      \
  4u /* 4.7 µH, integer part; use float in slope_comp.c */
#define INDUCTOR_VALUE_UH_TENTHS 7u /* fractional tenth (4.7 µH) */

/* =========================================================================
 * SECTION 16: POWER STAGE HARDWARE PARAMETERS
 * =========================================================================*/

/**
 * These are reference constants for documentation/assertion purposes.
 * They are not used in computation directly but anchor the scaling constants
 * in §14–§15 to their physical basis.
 */

/** Inductor value (IHLP6767GZER4R7M11) in nanohenries. */
#define INDUCTOR_VALUE_NH 4700u

/** Inductor saturation current in milliamps (Vishay datasheet; Round 3 Q7). */
#define INDUCTOR_ISAT_MA 21000u

/** Input shunt R25 in milliohms (physical inspection, [Jonah]; §3.3). */
#define INPUT_SHUNT_R25_MOHM 5u

/** Inductor shunt R5 in milliohms (schematic; §3.3). */
#define INDUCTOR_SHUNT_R5_MOHM 5u

/** Output shunt R6 in milliohms (schematic; §3.3). */
#define OUTPUT_SHUNT_R6_MOHM 12u

/** Current-sense amplifier gain for all three shunts (INA281A2, INA293A2). */
#define CURRENT_SENSE_AMP_GAIN_VV 50u

#ifdef __cplusplus
}
#endif

#endif /* REGULATOR_CONFIG_H */
