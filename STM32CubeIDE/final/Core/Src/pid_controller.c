/**
 * @file    pid_controller.c
 * @brief   Discrete-time PID controller implementation (NLSpec §8.4).
 *
 * Standard proportional-integral-derivative controller with back-calculation
 * anti-windup.  Designed to run from the TIM7 ISR at 20 kHz.
 *
 * Anti-windup strategy: when the raw (unsaturated) output would exceed
 * [output_min, output_max], the excess is subtracted from the integrator
 * before it is stored.  This is the "back-calculation" / "conditional
 * integration" variant — it prevents the integrator from accumulating
 * beyond the point where its contribution could ever be expressed in the
 * output (§8.4).
 */

#include "pid_controller.h"

/* =========================================================================
 * Public API implementations
 * =========================================================================*/

void pid_init(PidState *state, const PidConfig *config)
{
    (void)config;   /* config is validated at the call site; no action here */
    state->integrator     = 0.0f;
    state->previous_error = 0.0f;
}

float pid_update(PidState *state, const PidConfig *config,
                 float setpoint, float measurement)
{
    /* --- Compute error signal --- */
    float error = setpoint - measurement;

    /* --- Proportional term --- */
    float proportional = config->kp * error;

    /* --- Integral term (Euler forward integration) --- */
    float integral_candidate = state->integrator + (config->ki * error * config->dt_seconds);

    /* --- Derivative term (backward difference on error signal) --- */
    float derivative = config->kd * (error - state->previous_error) / config->dt_seconds;

    /* --- Unsaturated output --- */
    float raw_output = proportional + integral_candidate + derivative;

    /* --- Clamp output and apply back-calculation anti-windup ---
     *
     * If the raw output exceeds the limits, remove the excess from the
     * integrator so that the integrator state reflects only what was
     * actually applied.  This prevents runaway accumulation during
     * saturation (e.g., large PD voltage steps).
     */
    float clamped_output = raw_output;

    if (raw_output > config->output_max)
    {
        clamped_output        = config->output_max;
        integral_candidate   -= (raw_output - config->output_max);
    }
    else if (raw_output < config->output_min)
    {
        clamped_output        = config->output_min;
        integral_candidate   -= (raw_output - config->output_min);
    }

    /* --- Commit state updates --- */
    state->integrator     = integral_candidate;
    state->previous_error = error;

    return clamped_output;
}

void pid_reset_integrator(PidState *state)
{
    state->integrator     = 0.0f;
    state->previous_error = 0.0f;
}
