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

    /* --- Integral term (Euler forward integration), bounded ---
     * The integrator itself stays inside the output limits, so it can never
     * hold more than the output can express.                              */
    float integral_candidate = state->integrator + (config->ki * error * config->dt_seconds);
    if (integral_candidate > config->output_max) { integral_candidate = config->output_max; }
    if (integral_candidate < config->output_min) { integral_candidate = config->output_min; }

    /* --- Derivative term (backward difference on error signal) --- */
    float derivative = config->kd * (error - state->previous_error) / config->dt_seconds;

    /* --- Unsaturated output --- */
    float raw_output = proportional + integral_candidate + derivative;

    /* --- Clamp output; conditional integration as anti-windup ---
     *
     * When the output is saturated and the error would push it further into
     * saturation, the integrator keeps its previous value.
     *
     * The earlier form here was back-calculation with unity gain: the whole
     * clamped excess was moved into the integrator.  With kp large against
     * ki that turns one saturated cycle into a stored opposite-sign command:
     * on the bench (2026-09-27, run 1) a raw output of -316 clamped to 0
     * left +316 in the integrator, which was applied in full on the next
     * cycle (DAC 0, 416, 0, 1178, 0, 3771 over six cycles, then OVP).
     */
    float clamped_output = raw_output;

    if (raw_output > config->output_max)
    {
        clamped_output = config->output_max;
        if (error > 0.0f) { integral_candidate = state->integrator; }
    }
    else if (raw_output < config->output_min)
    {
        clamped_output = config->output_min;
        if (error < 0.0f) { integral_candidate = state->integrator; }
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
