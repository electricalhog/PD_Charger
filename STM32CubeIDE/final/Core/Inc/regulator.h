/**
 * @file    regulator.h
 * @brief   Buck-boost voltage regulator state machine and public API (NLSpec §4–§12).
 *
 * Owns the regulator state machine (INIT → IDLE → RUNNING → FAULT), the
 * HRTIM post-init configuration, the PID execution ISR (TIM7), the HRTIM
 * period ISR (for DAC Y-intercept reload), and the HRTIM fault ISR.
 *
 * Architecture overview (§1):
 *   - The USB PD stack runs under FreeRTOS (handled by X-CUBE-TCPP).
 *   - The regulator control loop runs outside FreeRTOS, driven by hardware
 *     timer ISRs at priorities 0–2 (below FreeRTOS mask of 5).
 *   - Communication between the two sides is through shared volatile state
 *     in pd_interface.h (§9.1) and the function API below (§9.2).
 *
 * Public API (§9.2, Appendix G):
 *   regulator_init()                — one-time hardware configuration
 *   regulator_start()               — IDLE → RUNNING (with soft-start)
 *   regulator_stop()                — RUNNING → IDLE
 *   regulator_clear_fault()         — FAULT → IDLE (after HW fault deasserts)
 *   regulator_set_target_voltage()  — update PID setpoint (wraps pd_interface)
 *   regulator_get_state()           — read current FSM state
 *   regulator_get_mode()            — read current operating mode
 *
 * ISR ownership (Appendix G):
 *   TIM6_DAC_IRQHandler            — slope compensation (priority 0)
 *   HRTIM1_TIMA_IRQHandler         — DAC Y-intercept reload + backstop count (priority 1)
 *   HRTIM1_FLT_IRQHandler          — fault handling (priority 1)
 *   TIM7_DAC_IRQHandler            — PID computation (priority 2)
 *
 * NLSpec conformance: v0.1.2j §4–§12
 */

#ifndef REGULATOR_H
#define REGULATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "regulator_config.h"
#include "pid_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Enumerations
 * =========================================================================*/

/**
 * RegulatorState — top-level state machine states (§4).
 *
 * Transition diagram:
 *   INIT → IDLE → RUNNING → FAULT
 *                 ↑         |
 *                 └─────────┘  (fault-clear → IDLE)
 */
typedef enum
{
    REGULATOR_STATE_INIT    = 0, /**< Power-on: peripheral init in progress */
    REGULATOR_STATE_IDLE    = 1, /**< Ready; all FETs off; PID not running   */
    REGULATOR_STATE_RUNNING = 2, /**< Actively switching; PID + slope comp active */
    REGULATOR_STATE_FAULT   = 3  /**< Latched fault; all FETs off            */
} RegulatorState;

/**
 * RegulatorMode — operating sub-mode when in RUNNING state (§5).
 */
typedef enum
{
    REGULATOR_MODE_BUCK      = 0, /**< V_in > V_out: Timer A switches, Timer B static */
    REGULATOR_MODE_BOOST     = 1, /**< V_in < V_out: Timer B switches, Timer A static */
    REGULATOR_MODE_BUCK_BOOST = 2 /**< V_in ≈ V_out: four-switch interleaved (future) */
} RegulatorMode;

/**
 * RegulatorFaultSource — bitmask for the source of the last fault (§10.1).
 * Multiple bits may be set simultaneously.
 */
typedef enum
{
    REGULATOR_FAULT_NONE            = 0x00u,
    REGULATOR_FAULT_HW_FLT1        = 0x01u, /**< Hardware fault FLT1 (VS_GOOD) */
    REGULATOR_FAULT_HW_FLT2        = 0x02u, /**< Hardware fault FLT2 (IS_GOOD) */
    REGULATOR_FAULT_SW_OVP         = 0x04u, /**< Software overvoltage protection */
    REGULATOR_FAULT_SW_UVP         = 0x08u, /**< Software undervoltage protection */
    REGULATOR_FAULT_SW_VIN_RANGE   = 0x10u, /**< V_in out of range for mode */
    REGULATOR_FAULT_SW_BACKSTOP    = 0x20u  /**< Too many consecutive backstop events */
} RegulatorFaultSource;

/* =========================================================================
 * Runtime-mutable parameters (§15.2)
 *
 * These variables are declared here so that the debug/diagnostic interface
 * can read and write them directly.  The PID ISR reads them on every cycle.
 * =========================================================================*/

/**
 * pid_kp, pid_ki, pid_kd — PID tuning coefficients.
 * Defaults: Kp=1.0, Ki=100.0, Kd=0.0 (§8.7).
 * Hot-swappable: yes (new values take effect on next PID cycle).
 */
extern float pid_kp;
extern float pid_ki;
extern float pid_kd;

/**
 * max_consecutive_backstops — Consecutive backstop events before fault.
 * Default: 3 (§10.4).  Range: [1, 10].
 */
extern uint8_t max_consecutive_backstops;

/**
 * pid_integrator_reset_threshold_pct — Setpoint-change threshold for
 * PID integrator reset.
 * Default: 20 % (§8.8).  Range: [5, 100].
 */
extern uint8_t pid_integrator_reset_threshold_pct;

/**
 * regulator_integrator_reset_requested — Flag set by pd_interface.c when
 * a large setpoint change is detected.  The TIM7 ISR clears it after reset.
 * Volatile: written from FreeRTOS task, read from TIM7 ISR.
 */
extern volatile bool regulator_integrator_reset_requested;

/* =========================================================================
 * Status / telemetry (§15.1)
 * =========================================================================*/

/**
 * regulator_last_fault_source — bitmask of the most recent fault cause.
 * Read from diagnostic interface.  Cleared on regulator_clear_fault().
 */
extern RegulatorFaultSource regulator_last_fault_source;

/**
 * regulator_pid_output_dac_counts — latest PID output in DAC counts.
 * Telemetry: updated by TIM7 ISR.
 */
extern volatile uint16_t regulator_pid_output_dac_counts;

/**
 * regulator_pid_error_mv — latest PID error (setpoint − V_out) in mV.
 * Telemetry: updated by TIM7 ISR.
 */
extern volatile int32_t regulator_pid_error_mv;

/* =========================================================================
 * Public API
 * =========================================================================*/

/**
 * regulator_init — One-time hardware post-init configuration.
 *
 * Performs all tasks listed in §4.1 (INIT state):
 *   1. Reconfigure HRTIM output fault levels to INACTIVE (all FETs off on
 *      fault).  CubeMX sets FaultLevel = NONE; this corrects it (§5.5).
 *   2. Enable FLT1 and FLT2 fault response on Timer A and Timer B.
 *   3. Configure HRTIM compare registers on Timer A and Timer B (§10.3):
 *        CMP1 = blanking window end (HRTIM_TIMEEVFLT_BLANKINGCMP1)
 *        CMP2 = hardware backstop max on-time (ends charge phase if EEV4 late)
 *        CMP3 = bootstrap refresh pulse end (static leg resumes HIGH after LOW)
 *   4. Set DAC3 CH1 to 0 (zero current setpoint).
 *   5. Start COMP1 (§6.1).
 *   6. Configure slope compensation timer TIM6 (slope_comp_init()).
 *   7. Configure PID timer TIM7 (NVIC priority, period counts).
 *   8. Initialise PID state and coefficients (§8.7).
 *   9. Enable HRTIM period and fault interrupts.
 *  10. Apply forced-HIGH to the static leg for default mode (buck: CHB1 HIGH).
 *  11. Initialise ADC subsystem (adc_monitor_init() + start DMA).
 *  12. Initialise PD interface shared state (pd_interface_init()).
 *  13. Enable HRTIM fault inputs (already done by MX_HRTIM1_Init).
 *  14. Check for pre-existing fault; transition to IDLE if none.
 *
 * Must be called AFTER all MX_*_Init() functions and BEFORE osKernelStart().
 * Must NOT be called from an ISR.
 */
void regulator_init(void);

/**
 * regulator_start — Transition IDLE → RUNNING with soft-start.
 *
 * Preconditions (§4.2):
 *   - System is in IDLE state.
 *   - target_voltage_mv is in [SETPOINT_MIN_MV, SETPOINT_MAX_MV].
 *   - No active hardware fault.
 *
 * Actions:
 *   1. Determine operating mode from V_in vs target_voltage_mv (§5.1).
 *   2. Enable HRTIM switching outputs for the active leg.
 *   3. Apply forced-HIGH to the static leg.
 *   4. Start TIM6 (slope compensation) and TIM7 (PID).
 *   5. Begin soft-start ramp (§11.2).
 *
 * Returns without action if preconditions are not met.
 */
void regulator_start(void);

/**
 * regulator_stop — Controlled shutdown: RUNNING/FAULT → IDLE.
 *
 * Actions (§11.3):
 *   1. Disable all HRTIM switching outputs (all FETs off).
 *   2. Set DAC3 CH1 to 0.
 *   3. Stop TIM6 (slope compensation).
 *   4. Stop TIM7 (PID).
 *   5. Transition to IDLE.
 *
 * Safe to call from any context.  Non-blocking.
 */
void regulator_stop(void);

/**
 * regulator_clear_fault — Release the FAULT latch and return to IDLE.
 *
 * Preconditions (§4.4):
 *   - No active hardware fault input (FLT1 and FLT2 must be deasserted).
 *
 * Actions:
 *   1. Verify hardware fault inputs are not asserted.
 *   2. Reset PID integrator.
 *   3. Clear fault status.
 *   4. Transition to IDLE.
 *
 * Does NOT automatically start the regulator — an explicit regulator_start()
 * is required after fault clearance.
 *
 * Safe to call from any context.  Non-blocking.
 */
void regulator_clear_fault(void);

/**
 * regulator_set_target_voltage — Update the PID voltage setpoint.
 *
 * Thin wrapper around pd_interface_notify_voltage_contract() (§9.2).
 * Safe to call from any context.  Non-blocking.
 *
 * @param voltage_mv  Target output voltage in millivolts.
 */
void regulator_set_target_voltage(uint32_t voltage_mv);

/**
 * regulator_get_state — Read the current FSM state.
 *
 * Thread-safe: reads a single 32-bit enum variable.
 */
RegulatorState regulator_get_state(void);

/**
 * regulator_get_mode — Read the current operating mode.
 *
 * Meaningful only when state == REGULATOR_STATE_RUNNING.
 */
RegulatorMode regulator_get_mode(void);

/* =========================================================================
 * ISR handler declarations
 * (Defined in regulator.c; called from stm32g4xx_it.c)
 * =========================================================================*/

/**
 * regulator_hrtim_tima_period_isr — HRTIM Timer A period ISR body.
 *
 * Called from HRTIM1_TIMA_IRQHandler.  Reloads DAC3 CH1 with the current
 * PID peak value (§7.6) and counts CMP2 backstop events (§10.4).
 */
void regulator_hrtim_tima_period_isr(void);

/**
 * regulator_hrtim_fault_isr — HRTIM fault ISR body.
 *
 * Called from HRTIM1_FLT_IRQHandler.  Determines fault source, enters
 * FAULT state, disables timers, zeros DAC (§10.1).
 */
void regulator_hrtim_fault_isr(void);

/**
 * regulator_pid_tim7_isr — PID computation ISR body.
 *
 * Called from TIM7_DAC_IRQHandler.  Reads ADC results, runs PID, updates DAC
 * peak, updates slope step, performs software safety checks (§8, §10.2).
 */
void regulator_pid_tim7_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* REGULATOR_H */
