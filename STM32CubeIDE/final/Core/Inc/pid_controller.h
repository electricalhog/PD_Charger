/**
 * @file    pid_controller.h
 * @brief   Discrete-time PID controller with integrator anti-windup.
 *
 * Implements standard discrete PID (§8.4):
 *   P = Kp × error[n]
 *   I = I_prev + Ki × error[n] × dt
 *   D = Kd × (error[n] − error[n−1]) / dt
 *   output = P + I + D  (clamped to [output_min, output_max])
 *
 * Anti-windup: integrator is back-calculated when output saturates.
 *
 * Thread-safety: PID state must only be accessed from a single ISR context
 * (the TIM7 PID ISR).  No locking is provided.
 *
 * NLSpec conformance: v0.1.2j §8
 */

#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Type definitions
 * =========================================================================*/

/**
 * PidState — mutable runtime state for a single PID instance.
 *
 * Kept separate from PidConfig so the state (integrator, previous error)
 * can be reset without losing the tuning coefficients.
 */
typedef struct
{
    float integrator;    /**< Accumulated integral term (I_prev in §8.4). */
    float previous_error;/**< Error from the previous execution cycle. */
} PidState;

/**
 * PidConfig — tuning coefficients and execution parameters.
 *
 * These values are runtime-mutable (§8.7): they may be written from the
 * debug interface without stopping the regulator.  The PID ISR reads them
 * on every execution cycle.
 */
typedef struct
{
    float kp;           /**< Proportional gain. Default 1.0 (§8.7). */
    float ki;           /**< Integral gain.     Default 100.0 (§8.7). */
    float kd;           /**< Derivative gain.   Default 0.0 (§8.7). */
    float dt_seconds;   /**< Execution period in seconds. Default 50 µs. */
    float output_min;   /**< Minimum clamped output (PID_OUTPUT_MIN). */
    float output_max;   /**< Maximum clamped output (PID_OUTPUT_MAX). */
} PidConfig;

/* =========================================================================
 * API
 * =========================================================================*/

/**
 * pid_init — Initialise the PID controller with the given configuration.
 *
 * Zeroes the PID state (integrator, previous error).  Must be called once
 * before the first pid_update() call, and again after any state reset.
 *
 * @param state  Pointer to the mutable PID state.
 * @param config Pointer to the PID configuration (coefficients, dt, limits).
 */
void pid_init(PidState *state, const PidConfig *config);

/**
 * pid_update — Execute one PID computation cycle.
 *
 * Computes P + I + D, applies back-calculation anti-windup when the
 * output saturates, updates the integrator, and returns the clamped output.
 *
 * Suitable for calling from a timer ISR (TIM7).  Uses floating-point
 * arithmetic; Cortex-M4F FPU is enabled by default in this project.
 *
 * @param state       Mutable PID state (modified in place).
 * @param config      PID tuning configuration (read-only during ISR).
 * @param setpoint    Target value (same units as measurement).
 * @param measurement Current measured value.
 * @return Clamped PID output in [output_min, output_max].
 */
float pid_update(PidState *state, const PidConfig *config,
                 float setpoint, float measurement);

/**
 * pid_reset_integrator — Zero the integrator term and previous error.
 *
 * Must be called on:
 *  - IDLE → RUNNING transition
 *  - FAULT → IDLE transition
 *  - Operating mode change (buck ↔ boost)
 *  - Setpoint change exceeding the configured threshold (§8.8)
 *
 * @param state Mutable PID state.
 */
void pid_reset_integrator(PidState *state);

#ifdef __cplusplus
}
#endif

#endif /* PID_CONTROLLER_H */
