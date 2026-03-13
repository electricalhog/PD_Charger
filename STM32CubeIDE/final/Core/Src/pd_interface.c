/**
 * @file    pd_interface.c
 * @brief   USB PD ↔ Regulator bridge implementation (NLSpec §9).
 *
 * Translates USB PD power contract events into regulator setpoint updates.
 * All functions are non-blocking and safe to call from any context.
 *
 * The PID integrator reset on large setpoint changes (§8.8) is handled here
 * by setting a flag that the TIM7 ISR checks on its next execution cycle.
 */

#include "pd_interface.h"
#include "regulator.h"
#include "regulator_config.h"

/* =========================================================================
 * Shared state definitions
 * =========================================================================*/

volatile uint32_t target_voltage_mv = 0u;
volatile bool     regulator_ready   = false;
volatile bool     regulator_fault   = false;

/* =========================================================================
 * Module-private state
 * =========================================================================*/

/**
 * pd_previous_voltage_mv — Last committed setpoint for integrator-reset
 * threshold comparison (§8.8).  Updated whenever the setpoint is accepted.
 */
static uint32_t pd_previous_voltage_mv = 0u;

/* =========================================================================
 * Public API implementations
 * =========================================================================*/

void pd_interface_init(void)
{
    target_voltage_mv   = 0u;
    regulator_ready     = false;
    regulator_fault     = false;
    pd_previous_voltage_mv = 0u;
}

void pd_interface_notify_voltage_contract(uint32_t voltage_mv)
{
    /* --- Validate setpoint range (§9.2) --- */
    if (voltage_mv != 0u &&
        (voltage_mv < SETPOINT_MIN_MV || voltage_mv > SETPOINT_MAX_MV))
    {
        /* Out-of-range setpoint: reject silently.
         * TODO(hw): add a trace/log message here during bring-up to flag
         * unexpected PD negotiation results.                                 */
        return;
    }

    /* --- Check for large setpoint change → PID integrator reset (§8.8) ---
     *
     * If the new setpoint differs from the previous by more than
     * pid_integrator_reset_threshold_pct %, request an integrator reset.
     * The regulator.c TIM7 ISR checks regulator_integrator_reset_requested
     * and clears it after acting on it.
     */
    if (pd_previous_voltage_mv != 0u && voltage_mv != 0u)
    {
        uint32_t delta = (voltage_mv > pd_previous_voltage_mv)
                       ? (voltage_mv - pd_previous_voltage_mv)
                       : (pd_previous_voltage_mv - voltage_mv);

        /* Compute threshold: default 20 % of previous setpoint (§8.8) */
        uint32_t threshold = (pd_previous_voltage_mv *
                              (uint32_t)pid_integrator_reset_threshold_pct) / 100u;

        if (delta > threshold)
        {
            regulator_integrator_reset_requested = true;
        }
    }

    /* --- Update shared setpoint (atomic 32-bit write, §8.6) --- */
    target_voltage_mv = voltage_mv;

    /* --- Handle 0 mV = "no contract" → stop regulator if running (§9.2) --- */
    if (voltage_mv == 0u)
    {
        regulator_stop();
    }

    pd_previous_voltage_mv = voltage_mv;
}

void pd_interface_notify_disconnect(void)
{
    /* Clear setpoint and stop the regulator (§9.2) */
    target_voltage_mv = 0u;
    pd_previous_voltage_mv = 0u;
    regulator_stop();
}
