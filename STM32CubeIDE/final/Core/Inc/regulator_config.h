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
 * VS_GOOD — Input voltage good signal (HRTIM FLT1)
 * MCU pin : PA12 / HRTIM1_FLT1
 * Net     : VS_GOOD (input.kicad_sch)
 * Physical: ADM1270ACPZ FAULT output or voltage supervisor on V_in rail
 * Dir     : Digital input, active-low (fault asserts when input goes LOW)
 * Effect  : Any assertion forces ALL HRTIM outputs to safe state (all FETs off)
 */
#define PIN_VS_GOOD_PORT GPIOA
#define PIN_VS_GOOD_PIN GPIO_PIN_12

/**
 * IS_GOOD — Input current good signal (HRTIM FLT2)
 * MCU pin : PA15 / HRTIM1_FLT2
 * Net     : IS_GOOD (input.kicad_sch, final.ioc label)
 * Physical: INA293A2 (U6) or ADM1270 overcurrent indicator
 * Dir     : Digital input, active-low (fault asserts when input goes LOW)
 * Resolved: Round 3 Q5 (§3.7).
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
 * MAX_DUTY_CYCLE_PCT — Maximum allowed charge-phase duty cycle.
 * Units  : percent of switching period
 * Derive : 85 % leaves ~750 ns at 200 kHz for the discharge phase and
 *          bootstrap refresh (≥200 ns).  Constraint: backstop_counts ≤
 *          period − refresh_ticks. 85 % × 27200 = 23120; 27200 − 1088 =
 *          26112 → 23120 < 26112 ✓  (§10.3)
 * Range  : [50, 96]
 */
#define MAX_DUTY_CYCLE_PCT 85u

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
#define MAX_ON_TIME_COUNTS ((HRTIM_PERIOD_COUNTS) * (MAX_DUTY_CYCLE_PCT) / 100u)

_Static_assert(BOOTSTRAP_REFRESH_TICKS <
               (HRTIM_PERIOD_COUNTS - MAX_ON_TIME_COUNTS),
               "BOOTSTRAP_REFRESH_TICKS must not overlap active switching phase");

/* =========================================================================
 * SECTION 7: SLOPE COMPENSATION TIMER (TIM6) CONSTANTS
 * =========================================================================*/

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
 * ADC_VOLTAGE_FULL_SCALE_MV — Full-scale physical voltage for VD_MON / VS_MON.
 * Units  : millivolts
 * Derive : Divider ratio = R_low / (R_high + R_low)
 *          = 5820 / (100000 + 5820) = 5820 / 105820 = 0.05500.
 *          ADC full-scale = 3.3 V → full-scale physical voltage =
 *          3.3 V / 0.05500 = 60.0 V.  (§3.4, Appendix D)
 *          Scaling: V_mV = ADC_raw × 60000 / 4096 ≈ 14.65 mV/count.
 */
#define ADC_VOLTAGE_FULL_SCALE_MV 60000u

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
