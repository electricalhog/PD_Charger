/**
 * @file    pd_interface.h
 * @brief   USB PD ↔ Regulator integration boundary (NLSpec §9).
 *
 * Bridges the USB PD stack (X-CUBE-TCPP, FreeRTOS-managed) and the
 * regulator control firmware.  Provides:
 *
 *   1. Shared volatile state — single-writer/single-reader variables that
 *      the PD stack reads and the regulator ISRs write (or vice versa).
 *      No mutex required for 32-bit aligned, single-writer access on
 *      Cortex-M4 (§9.1).
 *
 *   2. Function API — non-blocking calls safe from any context (ISR or
 *      FreeRTOS task).  The PD stack calls these when a new power contract
 *      is accepted or when the sink disconnects (§9.2).
 *
 * This module does NOT implement USB PD negotiation logic — that is handled
 * entirely by X-CUBE-TCPP middleware.  It implements only the translation
 * layer between the negotiated contract and the regulator setpoint.
 *
 * NLSpec conformance: v0.1.2j §9
 */

#ifndef PD_INTERFACE_H
#define PD_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Shared state (§9.1)
 *
 * Written by one side, read by the other.  All are volatile, 32-bit aligned.
 * =========================================================================*/

/**
 * target_voltage_mv — Negotiated output voltage setpoint.
 *
 * Direction : PD stack → Regulator
 * Type      : uint32_t (millivolts)
 * Valid range: 5000–48000 mV (USB PD SPR + EPR), or 0 for "no contract".
 * Written by: pd_interface_notify_voltage_contract() (FreeRTOS task context)
 * Read by   : regulator PID ISR (TIM7, priority 2)
 */
extern volatile uint32_t target_voltage_mv;

/**
 * regulator_ready — Output is within the regulation window.
 *
 * Direction : Regulator → PD stack
 * Type      : bool
 * Written by: regulator PID ISR (TIM7, priority 2)
 * Read by   : FreeRTOS PD task (for VBUS-ready signalling, §13.2)
 */
extern volatile bool regulator_ready;

/**
 * regulator_fault — Regulator is in FAULT state.
 *
 * Direction : Regulator → PD stack
 * Type      : bool
 * Written by: regulator HRTIM fault ISR or PID ISR
 * Read by   : FreeRTOS PD task (for fault notification)
 */
extern volatile bool regulator_fault;

/* =========================================================================
 * API (§9.2)
 * =========================================================================*/

/**
 * pd_interface_init — Initialise the PD interface shared state.
 *
 * Zeroes all shared variables.  Must be called from regulator_init(),
 * which runs before the FreeRTOS scheduler starts.
 */
void pd_interface_init(void);

/**
 * pd_interface_notify_voltage_contract — Update the voltage setpoint from
 * a newly accepted USB PD power contract.
 *
 * Safe to call from any context (FreeRTOS task or ISR).  Non-blocking.
 *
 * Behaviour (§9.2):
 *   - Updates target_voltage_mv.
 *   - If the regulator is in IDLE and a valid setpoint arrives, it does NOT
 *     auto-start — an explicit regulator_start() is still required.
 *   - If the regulator is RUNNING, the PID loop picks up the new setpoint
 *     on its next execution cycle.
 *   - If the setpoint is 0 mV, this is treated as "no contract" → the
 *     regulator transitions to IDLE if currently RUNNING.
 *   - If the new setpoint differs from the current by > 20 %, the PID
 *     integrator reset flag is raised for the TIM7 ISR to act on (§8.8).
 *
 * @param voltage_mv  Negotiated voltage in millivolts.
 */
void pd_interface_notify_voltage_contract(uint32_t voltage_mv);

/**
 * pd_interface_notify_disconnect — Signal that the sink has disconnected.
 *
 * Called from the PD stack when the sink cable is unplugged or PD
 * negotiation fails.  Triggers a controlled regulator shutdown (IDLE state).
 *
 * Non-blocking; safe from any context.
 */
void pd_interface_notify_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* PD_INTERFACE_H */
