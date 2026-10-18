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
 * pid_kp, pid_ki, pid_kd — Voltage-loop gains in use (telemetry since
 * 2026-10-12): kp [mA/mV], ki [mA/(mV s)], recomputed every PID cycle from
 * the loop crossover and the output capacitance.  Tune these instead:
 *   regulator_loop_fc_hz  crossover [Hz]      (LOOP_CROSSOVER_HZ)
 *   regulator_c_int_uf    C before the output switch [uF] (LOOP_C_INT_UF)
 *   regulator_c_ext_uf    C after it, counted once it is on (LOOP_C_EXT_UF)
 */
extern float pid_kp;
extern float pid_ki;
extern float pid_kd;
extern volatile float regulator_loop_fc_hz;
extern volatile float regulator_c_int_uf;
extern volatile float regulator_c_ext_uf;

/** The setpoint the loop regulates to now: target_voltage_mv followed at the
 *  slew rate (PID ISR step 4a).  Equal to the target once a ramp is done. */
extern volatile uint32_t regulator_commanded_mv;

/** Predictive synchronous rectification (regulator_config.h, SR_*): runtime
 *  switch (0/1, SR_ENABLE_DEFAULT at boot) and the window last programmed,
 *  ns from the period start (0 when not switched). */
extern volatile uint8_t  regulator_sr_enable;
extern volatile uint16_t regulator_sr_on_ns;
extern volatile uint16_t regulator_sr_off_ns;

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
 * regulator_pid_output_dac_counts — peak-current comparator threshold in DAC
 * counts (a protection level since 2026-10-12; the on-time ends pulses).
 * regulator_on_time_ns — charge-pulse on-time this PID period.
 * regulator_i_cmd_ma / regulator_i_pk_ma / regulator_i_max_ma — the loop's
 * average output-current command, the peak current it asks of the mode,
 * and the most the mode can deliver now.  Telemetry, updated by the TIM7 ISR.
 */
extern volatile uint16_t regulator_pid_output_dac_counts;
extern volatile uint16_t regulator_on_time_ns;
extern volatile float regulator_i_cmd_ma;
extern volatile float regulator_i_pk_ma;
extern volatile float regulator_i_max_ma;

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
    REGULATOR_DEBUG_CMD_SNAPSHOT    = 7u, /**< freeze the VD_MON ring into regulator_snapshot_trace and
                                               hold pulses off for 2 PID periods as a scope marker;
                                               result = 0, or 1 if not RUNNING */
    REGULATOR_DEBUG_CMD_TRACE_ARM   = 8u, /**< arg = trigger | post << 4 | threshold_10mv << 16
                                               (regulator_trace); result = RegulatorTraceResult */
    REGULATOR_DEBUG_CMD_SWITCH_PULSE = 9u /**< arg = us (1..50000) the output switch stays open:
                                               the PID ISR opens it, TIM4 (one-pulse, 1 us)
                                               reconnects it; result = RegulatorTraceResult */
} RegulatorDebugCmd;

/**
 * Regulator trace (2026-10-17): one sample per PID cycle into a ring, armed
 * from the debug mailbox (TRACE_ARM), frozen a set number of samples after a
 * trigger, and read over SWD (`bu regulator trace`).  It records only while
 * RUNNING (the PID ISR returns early otherwise).
 *
 * Triggers: NOW at the first sample; SWITCH when SWITCH_PULSE opens the output
 * switch; CEILING when the on-time reaches its window limit (step 6); ERROR
 * when |setpoint - V_out| exceeds the threshold.
 */
#define REGULATOR_TRACE_LEN 2048u

typedef enum
{
    REGULATOR_TRACE_TRIG_NOW     = 0u,
    REGULATOR_TRACE_TRIG_SWITCH  = 1u,
    REGULATOR_TRACE_TRIG_CEILING = 2u,
    REGULATOR_TRACE_TRIG_ERROR   = 3u
} RegulatorTraceTrigger;

typedef enum
{
    REGULATOR_TRACE_IDLE      = 0u,
    REGULATOR_TRACE_ARMED     = 1u,   /**< recording, waiting for the trigger */
    REGULATOR_TRACE_TRIGGERED = 2u,   /**< recording the post-trigger samples */
    REGULATOR_TRACE_DONE      = 3u    /**< frozen until the next TRACE_ARM    */
} RegulatorTraceState;

typedef enum
{
    REGULATOR_TRACE_OK          = 0u,
    REGULATOR_TRACE_NOT_RUNNING = 1u,  /**< SWITCH_PULSE needs RUNNING                  */
    REGULATOR_TRACE_SWITCH_BUSY = 2u,  /**< switch not closed yet, or already pulsing   */
    REGULATOR_TRACE_BAD_ARG     = 3u,  /**< trigger > 3, or pulse outside 1..50000 us   */
    REGULATOR_TRACE_NO_SWITCH   = 4u   /**< built without OUTPUT_SWITCH_ENABLED         */
} RegulatorTraceResult;

/** Trace flag bits (RegulatorTraceSample.flags). */
#define REGULATOR_TRACE_FLAG_MODE_MASK   0x0003u  /**< RegulatorMode                         */
#define REGULATOR_TRACE_FLAG_SKIPPING    0x0004u  /**< pulses skipped this cycle             */
#define REGULATOR_TRACE_FLAG_SWITCH_ON   0x0008u  /**< output switch closed                  */
#define REGULATOR_TRACE_FLAG_RECTIFIER   0x0010u  /**< rectifier blend above 0               */
#define REGULATOR_TRACE_FLAG_AT_LIMIT    0x0020u  /**< on-time clamped at its window limit   */
#define REGULATOR_TRACE_FLAG_PULSE_OPEN  0x0040u  /**< SWITCH_PULSE holding the switch open  */
#define REGULATOR_TRACE_FLAG_TRIGGER     0x0080u  /**< the trigger sample                    */

typedef struct
{
    uint16_t v_out_mv;   /**< V_out this cycle (VD_MON trimmed mean) [mV]      */
    uint16_t i_cmd_ma;   /**< PI output, the average-current command [mA]       */
    uint16_t i_max_ma;   /**< its ceiling this cycle [mA]                       */
    uint16_t t_on_ns;    /**< charge pulse programmed [ns]                      */
    uint16_t flags;      /**< REGULATOR_TRACE_FLAG_*                            */
} RegulatorTraceSample;  /* 10 bytes */

typedef struct
{
    uint32_t state;          /**< RegulatorTraceState                          */
    uint32_t trigger;        /**< RegulatorTraceTrigger                        */
    uint32_t post;           /**< samples kept after the trigger sample        */
    uint32_t threshold_mv;   /**< ERROR trigger threshold                      */
    uint32_t write_index;    /**< next slot (mod REGULATOR_TRACE_LEN)          */
    uint32_t trigger_index;  /**< slot of the trigger sample                   */
    uint32_t count;          /**< samples written since the arm                */
    uint32_t remaining;      /**< post-trigger samples still to write          */
    uint32_t pulse_us;       /**< last SWITCH_PULSE: open time requested [us]  */
    uint32_t pulse_cycles;   /**< its OUTPUT_EN low-to-high time, DWT cycles
                                  (170 MHz); 0 until TIM4 reconnects           */
} RegulatorTraceCtl;

extern volatile RegulatorTraceSample regulator_trace[REGULATOR_TRACE_LEN];
extern volatile RegulatorTraceCtl    regulator_trace_ctl;

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
 * While running, a new target is followed at SETPOINT_SLEW_UP_MV_PER_CYCLE
 * or SETPOINT_SLEW_DOWN_MV_PER_CYCLE.
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

/**
 * regulator_switch_pulse_tim4_isr — SWITCH_PULSE reconnect (TIM4 update,
 * one-pulse, priority NVIC_PRIORITY_SWITCH_PULSE_TIM4).
 */
void regulator_switch_pulse_tim4_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* REGULATOR_H */
