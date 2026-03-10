/**
 * @file    power_stage.h
 * @brief   Buck-boost power stage control API.
 *
 * This module owns the regulator state machine (INIT → IDLE → RUNNING → FAULT),
 * the PID outer voltage loop, peak-current-mode inner loop via HRTIM + COMP1,
 * slope compensation, and the PD↔Regulator interface (NLSpec §9).
 *
 * All regulator ISRs (slope comp TIM6, HRTIM period/fault, PID TIM7) are
 * implemented as __weak HAL callback overrides within power_stage.c and
 * therefore survive CubeMX code regeneration without modification to
 * stm32g4xx_it.c.
 *
 * NLSpec: buck-boost-nlspec-v0_1_2d, §4–§12
 */

#ifndef POWER_STAGE_H
#define POWER_STAGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * STATE AND MODE TYPES  (NLSpec §4, §5)
 * ========================================================================= */

/**
 * @brief  Top-level regulator system states (NLSpec §4).
 */
typedef enum {
    PS_STATE_INIT    = 0, /**< Power-on; post-init configuration in progress. */
    PS_STATE_IDLE    = 1, /**< Outputs off; awaiting enable command. */
    PS_STATE_RUNNING = 2, /**< Actively switching; PID and slope comp running. */
    PS_STATE_FAULT   = 3, /**< Latched fault; outputs forced off. */
} PS_State_t;

/**
 * @brief  Operating mode selection (NLSpec §5).
 */
typedef enum {
    PS_MODE_BUCK  = 0, /**< V_in > V_out: Timer A switching, Timer B static. */
    PS_MODE_BOOST = 1, /**< V_in < V_out: Timer B switching, Timer A static. */
} PS_Mode_t;

/* =========================================================================
 * SHARED STATE — PD ↔ Regulator (NLSpec §9.1)
 * ========================================================================= */

/**
 * target_voltage_mv — Negotiated output voltage setpoint.
 * Direction: PD stack → Regulator
 * Written by: regulator_set_target_voltage(); single aligned 32-bit write (atomic).
 * Read by: PID ISR on every execution.
 * Units: millivolts. Valid range: 5000–48000. 0 = disable.
 */
extern volatile uint32_t target_voltage_mv;

/**
 * regulator_ready — True when V_out is within the regulation window.
 * Direction: Regulator → PD stack
 * Written by: PID ISR. Read by: PD stack / application.
 */
extern volatile bool regulator_ready;

/**
 * regulator_fault — True when the regulator is in FAULT state.
 * Direction: Regulator → PD stack
 * Written by: any ISR that detects a fault condition.
 * Read by: PD stack / application.
 */
extern volatile bool regulator_fault;

/* =========================================================================
 * INITIALISATION AND STATE CONTROL
 * ========================================================================= */

/**
 * @brief  Initialise the power stage and transition from INIT to IDLE.
 *
 * Must be called once, after all CubeMX-generated MX_*_Init() functions have
 * completed (including MX_HRTIM1_Init, MX_DAC3_Init, MX_COMP1_Init,
 * MX_ADC1_Init, MX_ADC2_Init, MX_TIM6_Init, MX_TIM7_Init).
 *
 * Performs (NLSpec §4.1):
 *  - Configure HRTIM Timer A/B Set/Reset sources and backstop CMP1.
 *  - Set DAC3 CH1 to 0 (zero current setpoint).
 *  - Start COMP1.
 *  - Start ADC1 and ADC2 in continuous mode.
 *  - Precompute slope compensation step size.
 *  - Configure static (non-switching) leg for default (buck) mode.
 *  - Enable HRTIM fault inputs FLT1 and FLT2.
 *  - Configure NVIC priorities for regulator ISRs.
 *  - Verify no active hardware fault.
 *  - Transition to IDLE.
 */
void PS_Init(void);

/**
 * @brief  Enable the power stage and transition IDLE → RUNNING.
 *
 * Determines operating mode from measured V_in vs. voltage_mv, configures
 * the correct switching/static HRTIM legs, resets the PID integrator, and
 * begins soft-start (NLSpec §11.2).
 *
 * Has no effect if the current state is not IDLE.
 *
 * @param  voltage_mv  Target output voltage [5000, 48000] mV.
 * @param  current_ma  Output current limit [0, 5000] mA (reserved for future
 *                     current-limiting wrapper; pass 0 to use hardware max).
 */
void PS_Start(uint32_t voltage_mv, uint32_t current_ma);

/**
 * @brief  Disable the power stage and transition RUNNING → IDLE (NLSpec §11.3).
 *
 * Disables all HRTIM switching outputs, zeroes DAC3 CH1, stops slope
 * compensation and PID timers. Safe to call from any state.
 */
void PS_Stop(void);

/**
 * @brief  Clear a latched fault condition (NLSpec §4.4).
 *
 * Transitions FAULT → IDLE only if the hardware fault input (FLT1/FLT2)
 * is no longer asserted. Has no effect if not in FAULT state or if the
 * hardware fault is still active.
 */
void PS_ClearFault(void);

/* =========================================================================
 * STATUS QUERIES
 * ========================================================================= */

/**
 * @brief  Return the current system state.
 * @retval PS_State_t value.
 */
PS_State_t PS_GetState(void);

/**
 * @brief  Return the current operating mode (buck or boost).
 * @retval PS_Mode_t value.
 */
PS_Mode_t PS_GetMode(void);

/**
 * @brief  Return the latest V_out measurement in millivolts.
 *         Source: ADC1_IN1 (VD_MON).
 * @retval V_out in mV.
 */
uint32_t PS_GetVout_mV(void);

/**
 * @brief  Return the latest V_in measurement in millivolts.
 *         Source: ADC2_IN17 (VS_MON).
 * @retval V_in in mV.
 */
uint32_t PS_GetVin_mV(void);

/**
 * @brief  Return the latest inductor current measurement in milliamps.
 *         Source: ADC1_IN2 (IL_MON).
 * @retval I_L in mA.
 */
uint32_t PS_GetIL_mA(void);

/* =========================================================================
 * PD ↔ REGULATOR INTERFACE  (NLSpec §9.2)
 * ========================================================================= */

/**
 * @brief  Set the target output voltage. Safe to call from any context.
 *
 * - If voltage_mv is 0 and state is RUNNING: transitions to IDLE.
 * - If voltage_mv is non-zero and state is RUNNING: PID picks up the new
 *   setpoint on its next execution. If the change exceeds
 *   PID_INTEGRATOR_RESET_PCT_DEFAULT, the integrator is reset.
 * - If state is IDLE: stores the setpoint but does not start switching.
 *   An explicit PS_Start() call is required.
 *
 * @param  voltage_mv  [0, 48000] mV. 0 means "no voltage requested."
 */
void regulator_set_target_voltage(uint32_t voltage_mv);

/**
 * @brief  Stop the regulator. Equivalent to PS_Stop(). Safe from any context.
 */
void regulator_stop(void);

/**
 * @brief  Clear a latched fault. Equivalent to PS_ClearFault().
 */
void regulator_clear_fault(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_STAGE_H */
