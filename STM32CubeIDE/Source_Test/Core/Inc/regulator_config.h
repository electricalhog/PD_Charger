/**
 * @file    regulator_config.h
 * @brief   Centralized hardware pin mapping and compile-time configuration
 *          for the buck-boost regulator.
 *
 * This is the single point of truth for all hardware-derived constants,
 * pin assignments, and compile-time configurable parameters as required
 * by NLSpec §2.1.
 *
 * All pin mapping entries include a doc-comment defining:
 *   - Pin name, MCU port/pin identifier, net label from schematic
 *   - What it connects to physically, and signal direction
 *
 * All compile-time configurable constants include a doc-comment stating:
 *   - Parameter name, expected units, valid range, derivation reference
 *
 * NLSpec: buck-boost-nlspec-v0_1_2d, §2.1
 * Target MCU: STM32G474RETx (170 MHz Cortex-M4F)
 * Board: PD Regulator Prototype Rev 0
 */

#ifndef REGULATOR_CONFIG_H
#define REGULATOR_CONFIG_H

#include "stm32g4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * PIN MAPPING — Analog Inputs
 * ========================================================================= */

/**
 * VD_MON — Output voltage sense (V_out).
 * MCU pin: PA0 / ADC1_IN1
 * Net: VD_MON on output.kicad_sch
 * Connected to: 100 kΩ / 5.82 kΩ resistive divider from V_out rail
 * Direction: Analog input
 */
#define PIN_VD_MON_PORT         GPIOA
#define PIN_VD_MON_PIN          GPIO_PIN_0
#define ADC_CH_VD_MON           ADC_CHANNEL_1

/**
 * IL_MON — Inductor current sense (shared with COMP1_INP).
 * MCU pin: PA1 / ADC1_IN2 / COMP1_INP
 * Net: IL_MON on dc-dc.kicad_sch
 * Connected to: INA281A2 (U7) output, 50 V/V gain across R5 (5 mΩ shunt)
 * Direction: Analog input; also feeds COMP1 non-inverting input directly
 */
#define PIN_IL_MON_PORT         GPIOA
#define PIN_IL_MON_PIN          GPIO_PIN_1
#define ADC_CH_IL_MON           ADC_CHANNEL_2

/**
 * VS_MON — Input voltage sense (V_in).
 * MCU pin: PA4 / ADC2_IN17
 * Net: VS_MON on input.kicad_sch
 * Connected to: 100 kΩ / 5.82 kΩ resistive divider from V_in rail
 * Direction: Analog input
 * Note: PA4 is only available on ADC2 on this package (not ADC1).
 */
#define PIN_VS_MON_PORT         GPIOA
#define PIN_VS_MON_PIN          GPIO_PIN_4
#define ADC_CH_VS_MON           ADC_CHANNEL_17

/* =========================================================================
 * PIN MAPPING — HRTIM PWM Outputs
 * ========================================================================= */

/**
 * CHA1 — HRTIM Timer A Output 1, drives Q1 (input-side high-side GaN FET).
 * MCU pin: PA8 / HRTIM_CHA1
 * Net: CHA1 on dc-dc.kicad_sch
 * Direction: PWM output (active switching leg in buck mode)
 */
#define PIN_CHA1_PORT           GPIOA
#define PIN_CHA1_PIN            GPIO_PIN_8

/**
 * CHA2 — HRTIM Timer A Output 2, drives Q2 (input-side low-side GaN FET).
 * MCU pin: PA9 / HRTIM_CHA2
 * Net: CHA2 on dc-dc.kicad_sch
 * Direction: PWM output (complementary to CHA1, dead-time inserted)
 */
#define PIN_CHA2_PORT           GPIOA
#define PIN_CHA2_PIN            GPIO_PIN_9

/**
 * CHB1 — HRTIM Timer B Output 1, drives Q3 (output-side high-side GaN FET).
 * MCU pin: PA10 / HRTIM_CHB1
 * Net: CHB1 on dc-dc.kicad_sch
 * Direction: PWM output (active switching leg in boost mode;
 *            static high-side hold in buck mode)
 */
#define PIN_CHB1_PORT           GPIOA
#define PIN_CHB1_PIN            GPIO_PIN_10

/**
 * CHB2 — HRTIM Timer B Output 2, drives Q4 (output-side low-side GaN FET).
 * MCU pin: PA11 / HRTIM_CHB2
 * Net: CHB2 on dc-dc.kicad_sch
 * Direction: PWM output (complementary to CHB1, dead-time inserted)
 */
#define PIN_CHB2_PORT           GPIOA
#define PIN_CHB2_PIN            GPIO_PIN_11

/* =========================================================================
 * PIN MAPPING — HRTIM Fault Inputs
 * ========================================================================= */

/**
 * FLT1 — HRTIM Fault Input 1.
 * MCU pin: PA12 / HRTIM_FLT1
 * Net: FLT1 on input.kicad_sch
 * Connected to: ADM1270ACPZ ~FAULT output (active-low)
 * Direction: Digital input, active-low
 */
#define PIN_FLT1_PORT           GPIOA
#define PIN_FLT1_PIN            GPIO_PIN_12

/**
 * FLT2 — HRTIM Fault Input 2.
 * MCU pin: PA15 / HRTIM_FLT2
 * Net: FLT2 (OPEN:Q5 — physical source unconfirmed)
 * Direction: Digital input, active-low
 */
#define PIN_FLT2_PORT           GPIOA
#define PIN_FLT2_PIN            GPIO_PIN_15

/* =========================================================================
 * HRTIM SWITCHING FREQUENCY
 * ========================================================================= */

/**
 * HRTIM_PERIOD_COUNTS — HRTIM period register value; sets the switching frequency.
 * Units: HRTIM timer counts (183.82 ps/count at MUL32 prescaler, 5.44 GHz tick)
 * Derivation: 170 MHz × 32 / f_sw. See NLSpec §5.5.
 *   27200 counts → 200 kHz
 *   10880 counts → 500 kHz
 * Valid range: [5440, 54400] corresponding to [100 kHz, 1 MHz]
 */
#define HRTIM_PERIOD_COUNTS         27200u

_Static_assert(HRTIM_PERIOD_COUNTS >= 5440u && HRTIM_PERIOD_COUNTS <= 54400u,
               "HRTIM_PERIOD_COUNTS out of valid range [5440, 54400]");

/* =========================================================================
 * MAXIMUM DUTY CYCLE / BACKSTOP
 * ========================================================================= */

/**
 * MAX_DUTY_CYCLE_PCT — Maximum charge-phase duty cycle as integer percent.
 * Units: percent (0–100)
 * Rationale: 85 % leaves 750 ns before period end at 200 kHz for the
 *            discharge phase and bootstrap refresh window (≥200 ns).
 *            Constraint: backstop ≤ period − (refresh_ns / tick_ps).
 *            At 200 kHz: 85 % × 27200 = 23120 counts. See NLSpec §10.3.
 * Valid range: [50, 95]
 */
#define MAX_DUTY_CYCLE_PCT          85u

_Static_assert(MAX_DUTY_CYCLE_PCT >= 50u && MAX_DUTY_CYCLE_PCT <= 95u,
               "MAX_DUTY_CYCLE_PCT out of valid range [50, 95]");

/**
 * MAX_ON_TIME_COUNTS — Hardware backstop Compare 1 register value.
 * Units: HRTIM timer counts
 * Derivation: HRTIM_PERIOD_COUNTS × MAX_DUTY_CYCLE_PCT / 100
 *   At 200 kHz: 27200 × 85 / 100 = 23120 counts ≈ 4.25 µs on-time
 */
#define MAX_ON_TIME_COUNTS          (HRTIM_PERIOD_COUNTS * MAX_DUTY_CYCLE_PCT / 100u)

/* =========================================================================
 * COMPARATOR BLANKING WINDOW
 * ========================================================================= */

/**
 * BLANKING_DURATION_NS — COMP1 blanking window after switching transition.
 * Units: nanoseconds
 * Rationale: Suppresses IL_MON ringing from parasitic L/C after the FET
 *            switches. Prevents false COMP1 trips. See NLSpec §5.5.
 *            Adjust empirically in range 100–500 ns.
 * Valid range: [100, 500]
 */
#define BLANKING_DURATION_NS        100u

_Static_assert(BLANKING_DURATION_NS >= 100u && BLANKING_DURATION_NS <= 500u,
               "BLANKING_DURATION_NS out of valid range [100, 500]");

/**
 * BLANKING_DURATION_COUNTS — Blanking window in HRTIM counts.
 * Units: HRTIM timer counts
 * Derivation: BLANKING_DURATION_NS × 5.44 counts/ns (at 5.44 GHz tick)
 *   100 ns × 5.44 = 544 counts
 */
#define BLANKING_DURATION_COUNTS    ((uint32_t)(BLANKING_DURATION_NS * 544u / 100u))

/* =========================================================================
 * BOOTSTRAP CAPACITOR REFRESH
 * ========================================================================= */

/**
 * BOOTSTRAP_REFRESH_NS — Bootstrap capacitor refresh pulse minimum duration.
 * Units: nanoseconds
 * Rationale: uP1966E gate driver requires switch node at GND for ≥200 ns to
 *            recharge the bootstrap capacitor. Maximum is 500 ns to limit
 *            inductor energy reversal in boost mode. See NLSpec §5.4.
 * Valid range: [200, 500]
 */
#define BOOTSTRAP_REFRESH_NS        200u

_Static_assert(BOOTSTRAP_REFRESH_NS >= 200u && BOOTSTRAP_REFRESH_NS <= 500u,
               "BOOTSTRAP_REFRESH_NS out of valid range [200, 500]");

/**
 * BOOTSTRAP_REFRESH_COUNTS — Bootstrap refresh pulse duration in HRTIM counts.
 * Units: HRTIM timer counts
 * Derivation: BOOTSTRAP_REFRESH_NS × 5.44 counts/ns
 *   200 ns × 5.44 = 1088 counts
 */
#define BOOTSTRAP_REFRESH_COUNTS    ((uint32_t)(BOOTSTRAP_REFRESH_NS * 544u / 100u))

/**
 * BOOTSTRAP_CMP2_COUNTS — HRTIM Compare 2 value for bootstrap refresh trigger.
 * Units: HRTIM timer counts
 * Derivation: Period − refresh_counts. Output goes low at this count for the
 *            static (high-side-held) leg, and returns high at next period reset.
 *   At 200 kHz: 27200 − 1088 = 26112 counts
 */
#define BOOTSTRAP_CMP2_COUNTS       (HRTIM_PERIOD_COUNTS - BOOTSTRAP_REFRESH_COUNTS)

/* =========================================================================
 * MODE HYSTERESIS
 * ========================================================================= */

/**
 * MODE_HYSTERESIS_MV — Hysteresis band for buck/boost mode selection.
 * Units: millivolts
 * Derivation: Buck if V_in > V_set + hysteresis; boost if V_in < V_set − hysteresis.
 *             See NLSpec §5.1.
 * Valid range: [100, 5000]
 */
#define MODE_HYSTERESIS_MV          1000u

_Static_assert(MODE_HYSTERESIS_MV >= 100u && MODE_HYSTERESIS_MV <= 5000u,
               "MODE_HYSTERESIS_MV out of valid range [100, 5000]");

/* =========================================================================
 * DAC AND CURRENT SCALING  (NLSpec §3.5, §6.2)
 * ========================================================================= */

/**
 * DAC_VREF_MV — DAC reference voltage (V_DDA = V_REF+).
 * Units: millivolts
 * Value: 3300 mV
 */
#define DAC_VREF_MV                 3300u

/**
 * DAC_RESOLUTION_COUNTS — DAC full-scale count (12-bit DAC3).
 * Units: DAC counts
 * Value: 4095
 */
#define DAC_RESOLUTION_COUNTS       4095u

/**
 * IL_MON_SENS_MV_PER_A — IL_MON signal sensitivity.
 * Units: millivolts per ampere
 * Derivation: R5 (5 mΩ) × INA281A2 gain (50 V/V) = 0.250 V/A = 250 mV/A.
 *             See NLSpec §3.3.
 */
#define IL_MON_SENS_MV_PER_A        250u

/**
 * DAC_COUNTS_PER_AMP — DAC counts per ampere of peak inductor current.
 * Units: counts / A
 * Derivation: (DAC_VREF_MV / DAC_RESOLUTION_COUNTS) / IL_MON_SENS_MV_PER_A × 1000
 *             = (3300 / 4095) / 250 × 1000 ≈ 3.22 mA/count → 310 counts/A
 *             Integer approximation: 310 counts/A. See NLSpec §6.2.
 */
#define DAC_COUNTS_PER_AMP          310u

/* =========================================================================
 * VOLTAGE MEASUREMENT SCALING  (NLSpec §3.4, §14.2)
 * ========================================================================= */

/**
 * VOLTAGE_SCALE_NUM / VOLTAGE_SCALE_DEN — V_out / V_in ADC channel scaling.
 * Units: millivolts per ADC count (as a fraction)
 * Derivation: Divider ratio = 5820 / (100000 + 5820) = 0.05500
 *             V_mV = ADC_raw × (V_ADC_FS_mV / ADC_FS) / ratio
 *                  = ADC_raw × (3300 / 4096) / 0.05500
 *                  ≈ ADC_raw × 60000 / 4096  (≈ 14.65 mV/count)
 *             Integer formula: V_mV = (uint32_t)ADC_raw × 60000u / 4096u
 */
#define VOLTAGE_SCALE_NUM           60000u
#define VOLTAGE_SCALE_DEN           4096u

/**
 * IL_MON_SCALE_NUM / IL_MON_SCALE_DEN — Inductor current ADC channel scaling.
 * Units: milliamps per ADC count (as a fraction)
 * Derivation: I_mA = ADC_raw × V_ADC_FS_mV / (ADC_FS × sensitivity_V_per_A)
 *                  = ADC_raw × 3300 / (4096 × 0.250)
 *                  = ADC_raw × 13200 / 4096  (≈ 3.22 mA/count)
 *             See NLSpec §14.2.
 */
#define IL_MON_SCALE_NUM            13200u
#define IL_MON_SCALE_DEN            4096u

/**
 * ID_MON_SCALE_NUM / ID_MON_SCALE_DEN — Output current (ID_MON) ADC scaling.
 * Units: milliamps per ADC count (as a fraction)
 * Derivation: R6 (12 mΩ) × INA281A2 (50 V/V) = 600 mV/A
 *             I_mA = ADC_raw × 3300 / (4096 × 0.600)
 *                  = ADC_raw × 5500 / 4096  (≈ 1.34 mA/count)
 *             See NLSpec §14.2. OPEN:Q2 — pin assignment unconfirmed.
 */
#define ID_MON_SCALE_NUM            5500u
#define ID_MON_SCALE_DEN            4096u

/**
 * IS_MON_SCALE_NUM / IS_MON_SCALE_DEN — Input current (IS_MON) ADC scaling.
 * Units: milliamps per ADC count (as a fraction)
 * Derivation: R25 (5 mΩ, confirmed by physical inspection) × gain (50 V/V) = 250 mV/A
 *             Identical to IL_MON sensitivity.
 *             I_mA = ADC_raw × 13200 / 4096  (≈ 3.22 mA/count)
 *             See NLSpec §3.3, §14.2.
 */
#define IS_MON_SCALE_NUM            13200u
#define IS_MON_SCALE_DEN            4096u

/* =========================================================================
 * PID OUTPUT LIMITS  (NLSpec §8.6)
 * ========================================================================= */

/**
 * PID_OUTPUT_MIN — Minimum PID output value (DAC counts).
 * Units: DAC counts (0–4095)
 * Rationale: Zero current setpoint — comparator never trips, regulator idle.
 */
#define PID_OUTPUT_MIN              0u

/**
 * PID_OUTPUT_MAX — Maximum PID output value (DAC counts).
 * Units: DAC counts (0–4095)
 * Derivation: USB PD EPR maximum = 5 A; 5 A × 310 counts/A ≈ 1550 counts.
 *             OPEN:Q7 — verify inductor/FET ratings exceed 5 A with margin.
 *             See NLSpec §8.6.
 * Valid range: [1, 4095]
 */
#define PID_OUTPUT_MAX              1550u

_Static_assert(PID_OUTPUT_MAX >= 1u && PID_OUTPUT_MAX <= 4095u,
               "PID_OUTPUT_MAX out of valid range [1, 4095]");

/* =========================================================================
 * SAFETY THRESHOLDS  (NLSpec §10.2)
 * ========================================================================= */

/**
 * OVP_ABSOLUTE_MV — Absolute output overvoltage protection hard cap.
 * Units: millivolts
 * Derivation: 110 % × 48000 mV (USB PD EPR max) = 52800 mV. See NLSpec §10.2.
 * THIS IS A COMPILE-TIME CONSTANT — must not be changed at runtime.
 * The hardware ceiling is the ADM1270 OVP at ~60 V.
 */
#define OVP_ABSOLUTE_MV             52800u

/**
 * OVP_RELATIVE_PCT_DEFAULT — Default relative OVP threshold (% of setpoint).
 * Units: percent
 * Rationale: Output / load protection. See NLSpec §10.2.
 * Valid range: [105, 150]. Runtime-mutable tuning parameter.
 */
#define OVP_RELATIVE_PCT_DEFAULT    110u

/**
 * UVP_RELATIVE_PCT_DEFAULT — Default relative UVP threshold (% of setpoint).
 * Units: percent
 * Rationale: Loss-of-regulation detection. See NLSpec §10.2.
 * Valid range: [20, 90]. Runtime-mutable tuning parameter.
 */
#define UVP_RELATIVE_PCT_DEFAULT    50u

/**
 * MAX_CONSECUTIVE_BACKSTOPS_DEFAULT — Max consecutive backstop events before fault.
 * Units: count
 * Rationale: 3 consecutive periods hitting backstop without COMP1 firing
 *            indicates a persistent overcurrent / no-trip condition. NLSpec §10.4.
 * Valid range: [1, 255]. Runtime-mutable tuning parameter.
 */
#define MAX_CONSECUTIVE_BACKSTOPS_DEFAULT  3u

/* =========================================================================
 * SOFT-START  (NLSpec §11.2)
 * ========================================================================= */

/**
 * SOFTSTART_RAMP_TIME_MS — Soft-start ramp duration from 0 to target voltage.
 * Units: milliseconds
 * Rationale: Limits inrush current and output voltage overshoot on enable.
 *            OPEN:Q12 — Dan to confirm desired ramp time.
 * Valid range: [1, 100]
 */
#define SOFTSTART_RAMP_TIME_MS      10u

_Static_assert(SOFTSTART_RAMP_TIME_MS >= 1u && SOFTSTART_RAMP_TIME_MS <= 100u,
               "SOFTSTART_RAMP_TIME_MS out of valid range [1, 100]");

/* =========================================================================
 * PID EXECUTION RATE  (NLSpec §8.2)
 * ========================================================================= */

/**
 * PID_RATE_HZ — PID outer voltage loop execution rate.
 * Units: Hz
 * Derivation: 20 kHz = every 10th switching period at 200 kHz. See NLSpec §8.2.
 *             Range: [1000, f_sw]. Default: 20000 Hz.
 * Valid range: [1000, 500000]
 */
#define PID_RATE_HZ                 20000u

_Static_assert(PID_RATE_HZ >= 1000u && PID_RATE_HZ <= 500000u,
               "PID_RATE_HZ out of valid range [1000, 500000]");

/**
 * PID_TIMER_PERIOD_COUNTS — TIM7 auto-reload register value for the PID rate.
 * Units: timer counts (170 MHz, prescaler = 0)
 * Derivation: 170,000,000 / PID_RATE_HZ − 1
 *   At 20 kHz: 170,000,000 / 20,000 − 1 = 8499
 */
#define PID_TIMER_PERIOD_COUNTS     (170000000u / PID_RATE_HZ - 1u)

/* =========================================================================
 * SLOPE COMPENSATION TIMER  (NLSpec §7.5)
 * ========================================================================= */

/**
 * SLOPE_COMP_RATE_HZ — Slope compensation staircase timer rate.
 * Units: Hz
 * Rationale: Must be >> f_sw. At 2 MHz gives 10 steps/period at 200 kHz.
 *            CPU budget: 10–30 cycles per ISR ≈ 12–36 % load at 2 MHz / 170 MHz.
 *            See NLSpec §7.5.
 * Valid range: [1000000, 10000000]
 */
#define SLOPE_COMP_RATE_HZ          2000000u

_Static_assert(SLOPE_COMP_RATE_HZ >= 1000000u && SLOPE_COMP_RATE_HZ <= 10000000u,
               "SLOPE_COMP_RATE_HZ out of valid range [1 MHz, 10 MHz]");

/**
 * SLOPE_COMP_TIMER_PERIOD_COUNTS — TIM6 auto-reload register value.
 * Units: timer counts (170 MHz, prescaler = 0)
 * Derivation: 170,000,000 / SLOPE_COMP_RATE_HZ − 1
 *   At 2 MHz: 170,000,000 / 2,000,000 − 1 = 84
 */
#define SLOPE_COMP_TIMER_PERIOD_COUNTS  (170000000u / SLOPE_COMP_RATE_HZ - 1u)

/**
 * SLOPE_A_PER_S_DEFAULT — Default slope compensation rate.
 * Units: A/s (amperes per second)
 * Derivation: Conservative value exceeding 50 % of worst-case downslope.
 *   Buck worst case: V_out = 20 V, L = 4.7 µH → downslope = 4.26 A/µs = 4.26e6 A/s.
 *   50 % of that = 2.13e6 A/s. Default 4.0e6 A/s provides 1.88× margin.
 *   OPEN:Q10 — Dan to confirm intended slope value. See NLSpec §7.3.
 * Runtime-mutable tuning parameter.
 */
#define SLOPE_A_PER_S_DEFAULT       4000000u

/* =========================================================================
 * USB PD VOLTAGE LIMITS  (NLSpec §1.1, §9.2)
 * ========================================================================= */

/**
 * VSETPOINT_MIN_MV — Minimum valid non-zero voltage setpoint.
 * Units: millivolts
 * Derivation: USB PD SPR minimum advertised voltage = 5 V. See NLSpec §9.2.
 */
#define VSETPOINT_MIN_MV            5000u

/**
 * VSETPOINT_MAX_MV — Maximum valid voltage setpoint.
 * Units: millivolts
 * Derivation: USB PD EPR maximum = 48 V. See NLSpec §9.2.
 */
#define VSETPOINT_MAX_MV            48000u

/**
 * VREGULATION_WINDOW_MV — V_out tolerance for regulator_ready assertion.
 * Units: millivolts (±)
 * Rationale: V_out within ±2 % of setpoint is "in regulation". At 48 V
 *            that is ±960 mV. 1000 mV is a conservative, integer-friendly bound.
 */
#define VREGULATION_WINDOW_MV       1000u

/**
 * PID_INTEGRATOR_RESET_PCT_DEFAULT — Large-step threshold for integrator reset.
 * Units: percent of previous setpoint
 * Derivation: Reset integrator if setpoint change > 20 %. See NLSpec §8.8.
 * Valid range: [5, 100]. Runtime-mutable tuning parameter.
 */
#define PID_INTEGRATOR_RESET_PCT_DEFAULT  20u

/* =========================================================================
 * NVIC PRIORITIES  (NLSpec §13.2)
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 3 (from FreeRTOSConfig.h)
 * Regulator ISRs must be priorities 0-2 (above FreeRTOS critical sections).
 * ========================================================================= */

/** Slope compensation timer (TIM6) — most latency-sensitive. */
#define NVIC_PRIO_SLOPE_COMP        0u

/** HRTIM period / fault interrupts — must never be delayed by RTOS. */
#define NVIC_PRIO_HRTIM             1u

/** PID timer (TIM7) and paired ADC completion — below HRTIM. */
#define NVIC_PRIO_PID               2u

#ifdef __cplusplus
}
#endif

#endif /* REGULATOR_CONFIG_H */
