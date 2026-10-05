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
    REGULATOR_MODE_BUCK_BOOST = 2 /**< V_in ≈ V_out: Q1 and Q4 pulse together (BUCK_BOOST_ENABLED) */
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

/**
 * RegulatorClearResult — outcome of regulator_clear_fault().
 */
typedef enum
{
    REGULATOR_CLEAR_OK                = 0u, /**< FAULT released; now IDLE          */
    REGULATOR_CLEAR_NOT_IN_FAULT      = 1u, /**< Nothing to clear                  */
    REGULATOR_CLEAR_INPUT_NOT_GOOD    = 2u, /**< VS_GOOD never rose: no/low VIN,
                                                 VIN over-voltage, or VS shorted   */
    REGULATOR_CLEAR_INPUT_OVERCURRENT = 3u  /**< IS_GOOD low after re-enable:
                                                 ADM1270 tripped again             */
} RegulatorClearResult;

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
 * regulator_fault_tick_ms — HAL tick at the most recent enter_fault().
 * Used to enforce the ADM1270 cool-down before re-enabling the input.
 */
extern volatile uint32_t regulator_fault_tick_ms;

/**
 * regulator_pid_output_dac_counts — latest PID output in DAC counts.
 * Telemetry: updated by TIM7 ISR.
 */
extern volatile uint16_t regulator_pid_output_dac_counts;
extern volatile uint16_t regulator_on_time_ns;

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
 * regulator_stop — Controlled shutdown: RUNNING → IDLE.
 *
 * Actions (§11.3):
 *   1. Disarm the HRTIM fault IRQ (dropping INPUT_EN makes VS_GOOD fall).
 *   2. Disable all HRTIM switching outputs (all FETs off).
 *   3. Set DAC3 CH1 to 0.
 *   4. Stop TIM6 (slope compensation) and TIM7 (PID).
 *   5. Disable the input/output power path.
 *   6. Transition to IDLE — unless FAULT is latched, which only
 *      regulator_clear_fault() releases.
 *
 * Safe to call from any context.  Non-blocking.
 */
void regulator_stop(void);

/**
 * regulator_clear_fault — Re-arm the ADM1270 and release the FAULT latch.
 *
 * The input protection (ADM1270, latch-off mode) only re-arms after its
 * TIMER_OFF cool-down AND an ENABLE high→low→high toggle, and VS_GOOD can
 * only be verified with the input path on.  Sequence:
 *   1. Hold INPUT_EN low until ADM1270_COOLDOWN_MS has elapsed since the
 *      fault (enter_fault already dropped it).
 *   2. Assert INPUT_EN (outputs still off, OUTPUT_EN off) and wait up to
 *      INPUT_PGOOD_TIMEOUT_MS for VS_GOOD; check IS_GOOD.
 *   3. Drop INPUT_EN again (IDLE keeps the input path off).
 *   4. On success: clear HRTIM fault flags, reset PID, FAULT → IDLE.
 *      On failure: stay in FAULT with the input path off.
 *
 * Does NOT start the regulator — call regulator_start() afterwards.
 *
 * Task context only (blocks for up to ~200 ms).
 */
RegulatorClearResult regulator_clear_fault(void);

/**
 * RegulatorDebugMailbox — command channel written by a debugger over SWD
 * (tools/bringup `bu regulator ...`) and serviced in task context by
 * regulator_debug_poll().
 *
 * Protocol: wait for request == 0, write request; the firmware executes it,
 * stores result, increments done_count, then zeroes request.
 */
typedef enum
{
    REGULATOR_DEBUG_CMD_NONE        = 0u,
    REGULATOR_DEBUG_CMD_CLEAR_FAULT = 1u, /**< result = RegulatorClearResult */
    REGULATOR_DEBUG_CMD_STOP        = 2u, /**< result = 0                    */
    REGULATOR_DEBUG_CMD_BENCH_PWM_ON  = 3u, /**< result = RegulatorBenchResult */
    REGULATOR_DEBUG_CMD_BENCH_PWM_OFF = 4u, /**< result = RegulatorBenchResult */
    REGULATOR_DEBUG_CMD_SET_VOLTAGE = 5u, /**< arg = mV; result = RegulatorSetResult */
    REGULATOR_DEBUG_CMD_START       = 6u, /**< arg = mV; result = RegulatorSetResult */
    REGULATOR_DEBUG_CMD_SNAPSHOT    = 7u  /**< freeze the VD_MON ring into regulator_snapshot_trace and
                                               hold pulses off for 2 PID periods as a scope marker;
                                               result = 0, or 1 if not RUNNING */
} RegulatorDebugCmd;

/**
 * RegulatorBenchResult — outcome of regulator_bench_pwm().
 */
typedef enum
{
    REGULATOR_BENCH_OK              = 0u,
    REGULATOR_BENCH_NOT_IDLE        = 1u, /**< only allowed from IDLE              */
    REGULATOR_BENCH_INPUT_PATH_ON   = 2u, /**< INPUT_EN must be low (no power)     */
    REGULATOR_BENCH_FAULT_LINE_LOW  = 3u  /**< VS_GOOD/IS_GOOD low: HRTIM would
                                               hold the outputs off               */
} RegulatorBenchResult;

/**
 * regulator_bench_pwm — Open-loop gate-signal test for bench validation
 * without a power stage (Nucleo + scope/analyzer).
 *
 * On: from IDLE with INPUT_EN low and both fault lines high, removes the
 * comparator event (EEV4) from the Timer A/B set/reset sources and enables
 * TA1/TA2/TB1/TB2 in the buck-mode configuration: TA1 is set at the period
 * and reset by the CMP2 max-duty backstop, with the configured dead-time;
 * TB1 is the static leg (CMP3 bootstrap refresh).  The HRTIM fault IRQ is
 * armed, so a fault line dropping latches FAULT as in RUNNING.
 * Off (or stop/fault): outputs off, set/reset sources restored.
 *
 * The PID and power path are never touched.  regulator_start() refuses
 * while bench PWM is active.
 */
RegulatorBenchResult regulator_bench_pwm(bool on);

/** True while bench PWM is running (read by tools/bringup). */
extern volatile bool regulator_bench_pwm_active;

#define REGULATOR_DEBUG_RESULT_UNKNOWN_CMD 0xFFFFFFFFu

/**
 * RegulatorSetResult — outcome of the SET_VOLTAGE and START mailbox commands.
 * While running, a new target is followed at SETPOINT_SLEW_MV_PER_CYCLE.
 */
typedef enum
{
    REGULATOR_SET_OK           = 0u,
    REGULATOR_SET_OUT_OF_RANGE = 1u, /**< outside SETPOINT_MIN_MV..SETPOINT_MAX_MV */
    REGULATOR_SET_NOT_IDLE     = 2u, /**< START only from IDLE                     */
    REGULATOR_SET_START_FAILED = 3u, /**< START left the regulator not RUNNING    */
    REGULATOR_SET_PD_OWNS_VBUS = 4u  /**< PD_VBUS_PATH_CHARGER build: only a USB-PD
                                          contract sets the output voltage        */
} RegulatorSetResult;

typedef struct
{
    volatile uint32_t request;     /**< RegulatorDebugCmd; 0 = idle       */
    volatile uint32_t result;      /**< result of the last command        */
    volatile uint32_t done_count;  /**< incremented after each command    */
    volatile uint32_t last_cmd;    /**< last command executed             */
    volatile uint32_t arg;         /**< argument, written before request  */
} RegulatorDebugMailbox;

extern RegulatorDebugMailbox regulator_debug;

/**
 * regulator_debug_poll — Service regulator_debug.  Call periodically from a
 * task (not an ISR); commands may block for up to ~200 ms.
 */
void regulator_debug_poll(void);

/**
 * regulator_set_target_voltage — Update the PID voltage setpoint.
 *
 * Thin wrapper around pd_interface_notify_voltage_contract() (§9.2).
 * Safe to call from any context.  Non-blocking.
 *
 * @param voltage_mv  Target output voltage in millivolts.
 */
void regulator_set_target_voltage(uint32_t voltage_mv);

/** ADC1 analog watchdog 1 ISR body (pulse skipping), from ADC1_2_IRQHandler. */
void regulator_adc1_awd_isr(void);

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
