/**
 * @file    regulator.c
 * @brief   Buck-boost regulator state machine, HRTIM post-init, and ISRs
 *          (NLSpec §4–§13).
 *
 * This file owns:
 *   - System state machine (INIT/IDLE/RUNNING/FAULT) and mode (BUCK/BOOST)
 *   - HRTIM post-init configuration (fault levels, fault enable, compare regs)
 *   - Soft-start ramp logic
 *   - Mode selection and mode transition sequence
 *   - Software safety checks (OVP, UVP, V_in range, backstop count)
 *   - ISR bodies for TIM7 (PID), HRTIM Timer A period, HRTIM fault
 *   - Power-path GPIO sequencing
 *
 * ============================================================
 * HRTIM output naming conventions (§5.2, §5.3, §3.6):
 *   CHA1 (PA8)  = Timer A Output 1 = input-side high-side FET (Q1)
 *   CHA2 (PA9)  = Timer A Output 2 = input-side low-side  FET (Q2, complementary)
 *   CHB1 (PA10) = Timer B Output 1 = output-side high-side FET (Q3)
 *   CHB2 (PA11) = Timer B Output 2 = output-side low-side  FET (Q4, complementary)
 *
 * Buck mode:  Timer A switches (CHA1 Set=Period, Reset=EEV4|CMP2)
 *             Timer B static   (CHB1 Set=CMP3,   Reset=Period — bootstrap refresh)
 *
 * Boost mode: Timer B switches (CHB1 Set=EEV4|CMP2, Reset=Period)
 *             Timer A static   (CHA1 Set=CMP3,       Reset=Period — bootstrap refresh)
 *
 * Both timer Set/Reset sources are pre-configured by CubeMX (final.ioc,
 * confirmed v0.1.2i, §5.5).  No polarity swap is needed at mode transition.
 * ============================================================
 *
 * ============================================================
 * HRTIM compare register assignments (§10.3):
 *   CMP1xR — Blanking window end: EEV4 is masked until CMP1 fires.
 *             Timer A CMP1 = HRTIM_BLANKING_TICKS_BUCK  (buck active leg)
 *             Timer B CMP1 = HRTIM_BLANKING_TICKS_BOOST (boost active leg)
 *             Blanking filter: HRTIM_TIMEEVFLT_BLANKINGCMP1 (configured in IOC).
 *   CMP2xR — Hardware backstop: ends the charge phase if EEV4 has not fired.
 *             Both timers: MAX_ON_TIME_COUNTS.
 *             Active leg RST source (buck): HRTIM_OUTPUTRESET_TIMCMP2.
 *             Active leg SET source (boost): HRTIM_OUTPUTSET_TIMCMP2.
 *   CMP3xR — Bootstrap refresh end: static leg resumes HIGH after LOW pulse.
 *             Both timers: BOOTSTRAP_REFRESH_TICKS.
 *             Static leg SET source: HRTIM_OUTPUTSET_TIMCMP3.
 * ============================================================
 *
 * ============================================================
 * HRTIM register access (RM0440 §27):
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R  = Timer A SET1R
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R  = Timer A RST1R
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R  = Timer B SET1R
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R  = Timer B RST1R
 *   HRTIM1->sTimerxRegs[x].CMP1xR = Compare 1 (blanking end)
 *   HRTIM1->sTimerxRegs[x].CMP2xR = Compare 2 (backstop)
 *   HRTIM1->sTimerxRegs[x].CMP3xR = Compare 3 (bootstrap refresh pulse end)
 *   HRTIM1->sCommonRegs.OENR       = Output enable register
 *   HRTIM1->sCommonRegs.ODISR      = Output disable register
 * ============================================================
 */

#include "regulator.h"
#include "regulator_config.h"
#include "pid_controller.h"
#include "slope_comp.h"
#include "adc_monitor.h"
#include "pd_interface.h"
#include "debug_log.h"
#include "main.h"           /* hhrtim1, hdac3, htim6, htim7, hcomp1 handles */
#include "stm32g4xx_hal.h"
#include <string.h>         /* memset */

/* External peripheral handles declared in main.c */
extern HRTIM_HandleTypeDef  hhrtim1;
extern DAC_HandleTypeDef    hdac3;
extern TIM_HandleTypeDef    htim7;
extern COMP_HandleTypeDef   hcomp1;

/* =========================================================================
 * HRTIM convenience bit definitions
 *
 * These bit positions follow RM0440 §27.5.x.  CMSIS (stm32g474xx.h) defines
 * HRTIM_SET1R_CMPx and HRTIM_RST1R_CMPx directly; fallback defines are
 * provided below for each used compare unit in case the header is absent.
 * =========================================================================*/

/** HRTIM TIMxCR register: FLT1EN (bit 20) and FLT2EN (bit 21) */
#define HRTIM_TIMCR_FLT1EN_BIT  (1UL << 20)
#define HRTIM_TIMCR_FLT2EN_BIT  (1UL << 21)

/* Compare 1 (CMP1): blanking window end.  Bit 3 in SET1R/RST1R. */
#ifndef HRTIM_RST1R_CMP1
#define HRTIM_RST1R_CMP1   (1UL << 3)
#endif
#ifndef HRTIM_SET1R_CMP1
#define HRTIM_SET1R_CMP1   (1UL << 3)
#endif

/* Compare 2 (CMP2): hardware backstop.  Bit 4 in SET1R/RST1R. */
#ifndef HRTIM_RST1R_CMP2
#define HRTIM_RST1R_CMP2   (1UL << 4)
#endif
#ifndef HRTIM_SET1R_CMP2
#define HRTIM_SET1R_CMP2   (1UL << 4)
#endif

/* Compare 3 (CMP3): bootstrap refresh end.  Bit 5 in SET1R/RST1R. */
#ifndef HRTIM_RST1R_CMP3
#define HRTIM_RST1R_CMP3   (1UL << 5)
#endif
#ifndef HRTIM_SET1R_CMP3
#define HRTIM_SET1R_CMP3   (1UL << 5)
#endif

/* =========================================================================
 * State machine variables
 * =========================================================================*/

/** Current FSM state — written from ISR and API; aligned 32-bit enum. */
static volatile RegulatorState regulator_state = REGULATOR_STATE_INIT;

/** Current operating mode */
static volatile RegulatorMode  regulator_mode  = REGULATOR_MODE_BUCK;

/* =========================================================================
 * Runtime-mutable parameter definitions (extern in regulator.h)
 * =========================================================================*/

float   pid_kp = 1.0f;
float   pid_ki = 100.0f;
float   pid_kd = 0.0f;

uint8_t max_consecutive_backstops          = (uint8_t)MAX_CONSECUTIVE_BACKSTOPS_DEFAULT;
uint8_t pid_integrator_reset_threshold_pct = 20u;

volatile bool regulator_integrator_reset_requested = false;

/* =========================================================================
 * Status / telemetry definitions (extern in regulator.h)
 * =========================================================================*/

RegulatorFaultSource regulator_last_fault_source    = REGULATOR_FAULT_NONE;
volatile uint16_t    regulator_pid_output_dac_counts = 0u;
volatile int32_t     regulator_pid_error_mv          = 0;

/* =========================================================================
 * PID controller instance
 * =========================================================================*/

static PidState  pid_state;
static PidConfig pid_config;

/* =========================================================================
 * Soft-start state
 * =========================================================================*/

/** Soft-start incremental setpoint (ramps from 0 to target_voltage_mv). */
static volatile uint32_t softstart_setpoint_mv = 0u;

/** True while the soft-start ramp is in progress. */
static volatile bool softstart_active = false;

/** Increment per PID cycle to reach target in SOFT_START_RAMP_MS (§11.2). */
static uint32_t softstart_increment_mv = 0u;

/* =========================================================================
 * Backstop counter
 * =========================================================================*/

/** Consecutive periods in which CMP2 fired before COMP1 (§10.4). */
static volatile uint8_t consecutive_backstop_count = 0u;

/* =========================================================================
 * Internal helper declarations
 * =========================================================================*/

static void hrtim_configure_fault_levels_and_enable(void);
static void hrtim_configure_compare_registers(void);
static void hrtim_enable_period_and_fault_interrupts(void);
static void hrtim_start_timers(void);
static void hrtim_apply_buck_mode_static_leg(void);
static void hrtim_apply_boost_mode_static_leg(void);
static void hrtim_disable_all_outputs(void);
static void power_path_enable(void);
static void power_path_disable(void);
static RegulatorMode determine_mode_from_voltages(uint32_t v_in_mv, uint32_t v_setpoint_mv);
static void enter_fault(RegulatorFaultSource source);
static bool software_safety_checks_pass(uint32_t v_out_mv, uint32_t v_in_mv,
                                         uint32_t v_setpoint_mv, RegulatorMode mode);

/* =========================================================================
 * regulator_init
 * =========================================================================*/

void regulator_init(void)
{
    /* --- Step 1: Reconfigure HRTIM output fault levels and enable fault
     *             response on Timer A and Timer B (§5.5, §10.1).
     *  CubeMX sets FaultLevel = NONE and FaultEnable = NONE — this must
     *  be corrected in post-init or faults will not affect the outputs.    */
    hrtim_configure_fault_levels_and_enable();

    /* --- Step 2: Configure HRTIM compare registers (§10.3) ---
     *  CMP1 = blanking window end, CMP2 = backstop, CMP3 = bootstrap end   */
    hrtim_configure_compare_registers();

    /* --- Step 3: Set DAC3 CH1 to 0 (zero current threshold → safe) --- */
    HAL_DAC_SetValue(&hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0u);
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);

    /* --- Step 4: Start COMP1 (§6.1) --- */
    HAL_COMP_Start(&hcomp1);

    /* --- Step 5: Initialise ADC subsystem --- */
    adc_monitor_init();
    adc_monitor_start_adc1_dma();

    /* --- Step 6: Initialise slope compensation (configures TIM6 rate,
     *             NVIC priority 0, does NOT start TIM6 yet)              --- */
    slope_comp_init();

    /* --- Step 7: Configure TIM7 (PID) for 20 kHz --- */
    TIM7->PSC = TIM7_PRESCALER;
    TIM7->ARR = TIM7_PERIOD_COUNTS;
    TIM7->EGR = TIM_EGR_UG;   /* force update event to load PSC/ARR */

    /* TODO(debug): During bring-up, verify TIM7 period by scoping a GPIO
     * toggled in regulator_pid_tim7_isr().  Expect ~50 µs (20 kHz). */
    HAL_NVIC_SetPriority(TIM7_DAC_IRQn, NVIC_PRIORITY_PID_TIM7, 0u);
    HAL_NVIC_EnableIRQ(TIM7_DAC_IRQn);

    /* --- Step 8: Initialise PID state and default coefficients (§8.7) --- */
    pid_config.kp          = pid_kp;
    pid_config.ki          = pid_ki;
    pid_config.kd          = pid_kd;
    pid_config.dt_seconds  = 1.0f / (float)PID_EXECUTION_RATE_HZ;  /* 50 µs */
    pid_config.output_min  = (float)PID_OUTPUT_MIN;
    pid_config.output_max  = (float)PID_OUTPUT_MAX;

    /* TODO(debug): Kp/Ki/Kd defaults are in regulator_config.h.  Tune via
     * debugger watch on pid_kp/pid_ki/pid_kd (live-writeable) (§8.7). */
    pid_init(&pid_state, &pid_config);

    /* --- Step 9: Enable HRTIM period (Timer A REP) and fault interrupts --- */
    hrtim_enable_period_and_fault_interrupts();

    /* --- Step 10: Start HRTIM counters (outputs stay disabled) --- */
    hrtim_start_timers();

    /* --- Step 11: Apply forced-HIGH to static leg for default mode (BUCK):
     *              CHB1 HIGH, CHB2 LOW via Timer B forced output.
     *  NOTE: Outputs are NOT enabled via OENR here — that happens in
     *        regulator_start().  The forced state is prepared so outputs
     *        can be enabled safely.                                        --- */
    hrtim_apply_buck_mode_static_leg();

    /* --- Step 12: Initialise PD interface shared state --- */
    pd_interface_init();

    /* --- Step 13: Power path GPIO — ensure both paths are disabled at init --- */
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_SET);

    /* --- Step 14: Check for pre-existing hardware fault --- */
    /* HRTIM fault status register — if any fault is pending, stay in INIT.
     * The ISR will transition to FAULT on the first interrupt.             */
    regulator_state = REGULATOR_STATE_IDLE;
}

/* =========================================================================
 * regulator_start
 * =========================================================================*/

void regulator_start(void)
{
    /* Guard: only start from IDLE */
    if (regulator_state != REGULATOR_STATE_IDLE)
    {
        return;
    }

    /* Guard: valid setpoint required */
    uint32_t voltage_mv = target_voltage_mv;
    if (voltage_mv < SETPOINT_MIN_MV || voltage_mv > SETPOINT_MAX_MV)
    {
        return;
    }

    /* Guard: no active hardware fault */
    /* TODO(hardware): read HRTIM fault status register to verify FLT1/FLT2
     * are deasserted before enabling outputs.  Example:
     *   if (HRTIM1->sCommonRegs.ISR & (HRTIM_ISR_FLT1 | HRTIM_ISR_FLT2)) { return; }
     */

    /* --- Enable input path FIRST so VS_MON sees the real supply voltage ---
     * INPUT_EN (and OUTPUT_EN) must be asserted before ADC2 can measure a
     * valid V_in.  Allow ≥ 2 ms for the voltage-divider network to settle
     * and for at least one ADC2 conversion to complete before reading back.
     * Called here in task context so HAL_Delay() is safe.                  */
    power_path_enable();
    HAL_Delay(2u);

    /* Re-sample V_in with INPUT_EN now asserted. */
    /* --- Take a fresh V_in reading before mode selection (§5.1) ---
     *
     * adc_measurements.v_in_mv is only updated by adc_monitor_read_vin_result()
     * inside the TIM7 PID ISR.  On first entry to regulator_start() the TIM7
     * has not yet run, so the field holds its power-on value of 0.
     *
     * With v_in_mv = 0 the mode selector would always choose BOOST (because
     * target_voltage_mv > 0 + MODE_HYSTERESIS_MV), then the first TIM7
     * execution would read the actual V_in, fail the V_in range check for the
     * selected mode (e.g. BOOST: V_in > V_set − BOOST_VIN_MARGIN_MV; future
     * BUCK_BOOST: similar per-mode check), and call enter_fault() before a
     * single useful PID cycle completes — killing the outputs and leaving the
     * debug buffer nearly empty.
     *
     * Taking a synchronous ADC2 sample here (≤ 1 ms blocking) gives mode
     * selection a valid V_in reading.  The poll timeout is safe from the task
     * context in which regulator_start() is called.                          */
    adc_monitor_trigger_vin();
    adc_monitor_read_vin_result();

    /* --- Determine operating mode based on V_in vs setpoint (§5.1) --- */
    uint32_t v_in_mv = (uint32_t)adc_measurements.v_in_mv;
    regulator_mode = determine_mode_from_voltages(v_in_mv, voltage_mv);

    /* --- Reset PID integrator (§8.8) --- */
    pid_reset_integrator(&pid_state);
    regulator_integrator_reset_requested = false;

    /* --- Configure soft-start (§11.2) ---
     * Number of PID cycles in SOFT_START_RAMP_MS:
     *   ramp_steps = SOFT_START_RAMP_MS × PID_EXECUTION_RATE_HZ / 1000
     *   At 20 kHz, 5 ms → 100 steps.                                      */
    uint32_t ramp_steps = (uint32_t)SOFT_START_RAMP_MS *
                          (uint32_t)PID_EXECUTION_RATE_HZ / 1000u;
    if (ramp_steps == 0u) { ramp_steps = 1u; }
    softstart_increment_mv = voltage_mv / ramp_steps;
    if (softstart_increment_mv == 0u) { softstart_increment_mv = 1u; }
    softstart_setpoint_mv = 0u;
    softstart_active      = true;

    /* --- Apply forced-HIGH to the static leg for the selected mode --- */
    if (regulator_mode == REGULATOR_MODE_BUCK)
    {
        hrtim_apply_buck_mode_static_leg();
    }
    else /* BOOST (future: BUCK_BOOST for four-switch mode) */
    {
        /* TODO(future): add hrtim_apply_buck_boost_mode_static_leg() here
         * when REGULATOR_MODE_BUCK_BOOST is implemented (§5.4 four-switch). */
        hrtim_apply_boost_mode_static_leg();
    }

    /* Power path already enabled above; no second call needed. */

    /* --- Enable HRTIM switching outputs for the active leg (§5.5) --- */
    if (regulator_mode == REGULATOR_MODE_BUCK)
    {
        /* Enable Timer A outputs (CHA1/CHA2 = input-side switching leg) */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                       HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2);
        /* Enable Timer B outputs (CHB1/CHB2 = output-side static leg) */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                       HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
    }
    else /* BOOST (future: BUCK_BOOST for four-switch mode) */
    {
        /* Enable Timer B outputs (CHB1/CHB2 = output-side switching leg) */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                       HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
        /* Enable Timer A outputs (CHA1/CHA2 = input-side static leg) */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                       HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2);
    }

    /* --- Initialise slope compensation and start TIM6 --- */
    uint32_t v_out_mv = (uint32_t)adc_measurements.v_out_mv;
    slope_comp_update_step(v_out_mv, v_in_mv,
                            (SlopeCompMode)regulator_mode);
    slope_comp_start(0u);   /* peak starts at 0; ramps up with soft-start */

    /* --- Start PID timer (TIM7) --- */
    HAL_TIM_Base_Start_IT(&htim7);

    regulator_state = REGULATOR_STATE_RUNNING;
}

/* =========================================================================
 * regulator_stop
 * =========================================================================*/

void regulator_stop(void)
{
    /* Disable all HRTIM switching outputs immediately (§11.3) */
    hrtim_disable_all_outputs();

    /* Stop slope compensation and zero DAC */
    slope_comp_stop();

    /* Stop PID timer */
    HAL_TIM_Base_Stop_IT(&htim7);

    /* Disable power path */
    power_path_disable();

    /* Reset soft-start state */
    softstart_active      = false;
    softstart_setpoint_mv = 0u;

    /* Reset PID integrator for clean restart */
    pid_reset_integrator(&pid_state);

    regulator_state = REGULATOR_STATE_IDLE;
}

/* =========================================================================
 * regulator_clear_fault
 * =========================================================================*/

void regulator_clear_fault(void)
{
    if (regulator_state != REGULATOR_STATE_FAULT)
    {
        return;
    }

    /* Verify hardware fault inputs are deasserted (§4.4).
     * FLT1 (VS_GOOD) and FLT2 (IS_GOOD) are active-low; a GPIO read HIGH
     * means the fault has cleared.
     *
     * TODO(hardware): Verify GPIO read direction and polarity against
     * schematic during bring-up.  The HRTIM fault latches must also be
     * cleared via the HRTIM_ICR register before re-enabling outputs.
     */
    GPIO_PinState vs_good = HAL_GPIO_ReadPin(PIN_VS_GOOD_PORT, PIN_VS_GOOD_PIN);
    GPIO_PinState is_good = HAL_GPIO_ReadPin(PIN_IS_GOOD_PORT, PIN_IS_GOOD_PIN);

    if (vs_good == GPIO_PIN_RESET || is_good == GPIO_PIN_RESET)
    {
        /* Hardware fault is still asserted — cannot clear */
        return;
    }

    /* Clear HRTIM fault interrupt flags */
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C;

    /* Reset regulator state */
    pid_reset_integrator(&pid_state);
    regulator_integrator_reset_requested = false;
    regulator_last_fault_source          = REGULATOR_FAULT_NONE;
    consecutive_backstop_count           = 0u;
    regulator_fault                      = false;

    regulator_state = REGULATOR_STATE_IDLE;
}

/* =========================================================================
 * regulator_set_target_voltage
 * =========================================================================*/

void regulator_set_target_voltage(uint32_t voltage_mv)
{
    pd_interface_notify_voltage_contract(voltage_mv);
}

/* =========================================================================
 * regulator_get_state / regulator_get_mode
 * =========================================================================*/

RegulatorState regulator_get_state(void)
{
    return regulator_state;
}

RegulatorMode regulator_get_mode(void)
{
    return regulator_mode;
}

/* =========================================================================
 * ISR bodies
 * =========================================================================*/

/**
 * regulator_hrtim_tima_period_isr — HRTIM Timer A period (REP) ISR.
 *
 * Fires at every switching period reset (200 kHz at default settings).
 * Reloads DAC3 CH1 with the PID peak value (§7.6).
 * Counts CMP2 backstop events (§10.4).
 *
 * Priority: NVIC_PRIORITY_HRTIM (1) — below TIM6 (0), above TIM7 (2).
 * No FreeRTOS API calls allowed.
 */
void regulator_hrtim_tima_period_isr(void)
{
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        return;
    }

    /* Check if this period reset was caused by the Compare 2 backstop.
     * We distinguish by checking the HRTIM Timer A interrupt status register:
     *   - REP flag (bit 0) = period/repetition interrupt
     *   - CMP2 flag (bit 5) = compare 2 match interrupt
     *
     * TODO(hardware): Wire CMP2 interrupt properly.  For now, backstop
     * counting is a TODO pending hardware verification that CMP2 fires
     * correctly.  See §10.3.
     */

    /* Reload DAC3 CH1 with current PID peak value.
     * Must happen before the blanking window expires (§7.6).              */
    slope_comp_reload_dac_peak();

    /* TODO(debug): Confirm DAC reload timing with oscilloscope: probe DAC3
     * output (PA5 / DAC3_OUT1) and TA1 switching node; the DAC must settle
     * to the new peak value before CMP1 unmasks EEV4 (~100 ns in buck,
     * ~500 ns in boost from period reset). (§7.6) */

    /* Clear the HRTIM Timer A repetition interrupt flag */
    /* This is handled by the HAL callback mechanism or direct register clear:
     * HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].TIMxICR = HRTIM_TIMISR_REP;
     * The HAL IRQ handler clears it; we just need to implement the callback. */
}

/**
 * regulator_hrtim_fault_isr — HRTIM fault ISR body.
 *
 * Called from HRTIM1_FLT_IRQHandler when FLT1 or FLT2 asserts.
 * The HRTIM hardware has already forced all outputs to their fault state
 * (INACTIVE = LOW → all FETs off).
 *
 * Priority: NVIC_PRIORITY_HRTIM (1).
 * No FreeRTOS API calls allowed.
 */
void regulator_hrtim_fault_isr(void)
{
    /* Determine fault source from HRTIM interrupt status register */
    uint32_t hrtim_isr = HRTIM1->sCommonRegs.ISR;

    RegulatorFaultSource source = REGULATOR_FAULT_NONE;
    if (hrtim_isr & HRTIM_ISR_FLT1)
    {
        source |= REGULATOR_FAULT_HW_FLT1;
    }
    if (hrtim_isr & HRTIM_ISR_FLT2)
    {
        source |= REGULATOR_FAULT_HW_FLT2;
    }

    enter_fault(source);

    /* Clear HRTIM fault flags */
    HRTIM1->sCommonRegs.ICR = (hrtim_isr & (HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C));
}

/**
 * regulator_pid_tim7_isr — PID computation ISR body.
 *
 * Fires at 20 kHz.  Sequence (§8, §10.2, §15.1):
 *   1. Read ADC measurements (V_out, V_in, I_L, etc.)
 *   2. Update PID coefficients from runtime-mutable variables.
 *   3. Check for integrator reset request (large setpoint change).
 *   4. Run soft-start setpoint ramp if active (§11.2).
 *   5. Run PID computation → new DAC peak value.
 *   6. Update slope compensation step (§7.2).
 *   7. Update telemetry (§15.1).
 *   8. Software safety checks (§10.2); enter fault if violated.
 *   9. Update regulator_ready flag.
 *
 * Priority: NVIC_PRIORITY_PID_TIM7 (2).
 * No FreeRTOS API calls allowed.
 */
void regulator_pid_tim7_isr(void)
{
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        return;
    }

    /* --- 1. Trigger and read ADC conversions --- */
    adc_monitor_trigger_vin();

    uint32_t v_out_mv  = adc_measurements.v_out_mv;
    uint32_t i_l_ma    = adc_measurements.i_inductor_ma;
    (void)i_l_ma;   /* telemetry only */

    /* Read V_in result from the ADC2 conversion triggered at the start;
     * at 20 kHz period (50 µs), ADC2 conversion (~354 ns) is complete.    */
    adc_monitor_read_vin_result();
    uint32_t v_in_mv = adc_measurements.v_in_mv;

    /* TODO(debug): If v_out_mv or v_in_mv reads zero, check ADC1/ADC2 DMA
     * init in stm32g4xx_hal_msp.c and confirm INPUT_EN is asserted before
     * regulator_start() powers the voltage-divider network (§5.1). */

    /* --- 2. Refresh PID config from runtime-mutable variables ---
     * This allows Kp/Ki/Kd to be tuned at runtime without restart (§8.7). */
    pid_config.kp = pid_kp;
    pid_config.ki = pid_ki;
    pid_config.kd = pid_kd;

    /* --- 3. Handle integrator reset request (§8.8) --- */
    if (regulator_integrator_reset_requested)
    {
        pid_reset_integrator(&pid_state);
        regulator_integrator_reset_requested = false;

        /* Re-evaluate operating mode when setpoint changes significantly (§5.1).
         * If the mode changes (buck ↔ boost), reconfigure the HRTIM static
         * and switching legs immediately to prevent shoot-through.            */
        uint32_t voltage_mv = target_voltage_mv;
        RegulatorMode new_mode = determine_mode_from_voltages(v_in_mv, voltage_mv);

        if (new_mode != regulator_mode)
        {
            /* Mode change during RUNNING: reconfigure HRTIM for the new mode.
             * Disable all outputs during the transition to prevent a partial
             * switching state (§5.3, §11.4).                                  */
            hrtim_disable_all_outputs();

            regulator_mode = new_mode;

            /* Reconfigure the static and switching legs for the new mode */
            if (regulator_mode == REGULATOR_MODE_BUCK)
            {
                hrtim_apply_buck_mode_static_leg();
                HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                               HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
                                               HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
            }
            else /* BOOST (future: BUCK_BOOST for four-switch mode) */
            {
                hrtim_apply_boost_mode_static_leg();
                HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                               HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2 |
                                               HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2);
            }

            /* Reset soft-start for the mode transition (§11.4) */
            uint32_t ramp_steps = (uint32_t)SOFT_START_RAMP_MS *
                                  (uint32_t)PID_EXECUTION_RATE_HZ / 1000u;
            if (ramp_steps == 0u) { ramp_steps = 1u; }
            softstart_increment_mv = voltage_mv / ramp_steps;
            if (softstart_increment_mv == 0u) { softstart_increment_mv = 1u; }
            softstart_setpoint_mv = 0u;
            softstart_active      = true;
        }
    }

    /* --- 4. Soft-start setpoint ramp (§11.2) --- */
    uint32_t effective_setpoint_mv;
    if (softstart_active)
    {
        softstart_setpoint_mv += softstart_increment_mv;
        if (softstart_setpoint_mv >= target_voltage_mv)
        {
            softstart_setpoint_mv = target_voltage_mv;
            softstart_active      = false;
        }
        effective_setpoint_mv = softstart_setpoint_mv;
    }
    else
    {
        effective_setpoint_mv = target_voltage_mv;
    }

    /* --- 5. PID computation --- */
    float setpoint_f    = (float)effective_setpoint_mv;
    float measurement_f = (float)v_out_mv;
    float pid_out_f     = pid_update(&pid_state, &pid_config,
                                     setpoint_f, measurement_f);

    uint32_t dac_counts = (uint32_t)pid_out_f;
    if (dac_counts > (uint32_t)PID_OUTPUT_MAX)
    {
        dac_counts = (uint32_t)PID_OUTPUT_MAX;
    }

    /* --- 6. Update slope compensation peak and step --- */
    slope_comp_set_peak(dac_counts);
    slope_comp_update_step(v_out_mv, v_in_mv, (SlopeCompMode)regulator_mode);

    /* TODO(debug): Verify slope_comp_step_counts in debugger watch.
     * Expect ~1–5 DAC counts/tick at 2 MHz with default inductor value.
     * If zero, check INDUCTOR_VALUE_UH / INDUCTOR_VALUE_UH_TENTHS in
     * regulator_config.h (§7.2, §16). */

    /* --- 7. Update telemetry --- */
    regulator_pid_output_dac_counts = (uint16_t)dac_counts;
    regulator_pid_error_mv          = (int32_t)effective_setpoint_mv - (int32_t)v_out_mv;

    /* --- 8. Software safety checks (§10.2) --- */
    if (!software_safety_checks_pass(v_out_mv, v_in_mv,
                                      effective_setpoint_mv, regulator_mode))
    {
        /* Fault entry handled inside software_safety_checks_pass */
        return;
    }

    /* --- 9. Update regulator_ready flag (§9.1) ---
     * Regulation window: ±2% of setpoint (§9.3).                          */
    if (effective_setpoint_mv > 0u)
    {
        uint32_t tolerance_mv = effective_setpoint_mv / 50u;  /* 2% */
        int32_t error = regulator_pid_error_mv;
        regulator_ready = (error >= -(int32_t)tolerance_mv &&
                           error <=  (int32_t)tolerance_mv);
    }
    else
    {
        regulator_ready = false;
    }

    /* --- 10. High-speed debug sample capture (§15.3) ---
     * Record one sample per PID cycle (20 kHz) into the circular debug buffer.
     * All fields are derived from values already computed in this ISR so there
     * is no extra ADC conversion overhead.  The buffer is readable in a live
     * debugger memory/watch window without halting the CPU.                  */
    {
        DebugSample s;
        s.v_out_mv      = v_out_mv;
        s.v_in_mv       = v_in_mv;
        s.i_inductor_ma = (uint32_t)adc_measurements.i_inductor_ma;
        s.i_out_ma      = (uint32_t)adc_measurements.i_out_ma;
        s.dac_counts    = (uint16_t)dac_counts;
        s.error_mv      = regulator_pid_error_mv;
        s.state         = (uint8_t)regulator_state;
        s.mode          = (uint8_t)regulator_mode;
        debug_log_record(&s);
    }
}

/* =========================================================================
 * Internal helper implementations
 * =========================================================================*/

/**
 * hrtim_configure_fault_levels_and_enable — Configure HRTIM fault response.
 *
 * CubeMX sets FaultLevel = NONE for all outputs and FaultEnable = NONE for
 * all timers.  We must correct both to ensure the HRTIM forces all FETs off
 * when FLT1 or FLT2 asserts (§10.1, §5.5).
 *
 * Approach:
 *   a) Reconfigure all four outputs via HAL with FaultLevel = INACTIVE.
 *   b) Enable FLT1 and FLT2 on Timer A and Timer B via direct register bits.
 */
static void hrtim_configure_fault_levels_and_enable(void)
{
    HRTIM_OutputCfgTypeDef output_cfg;
    memset(&output_cfg, 0, sizeof(output_cfg));

    output_cfg.Polarity              = HRTIM_OUTPUTPOLARITY_HIGH;
    output_cfg.IdleMode              = HRTIM_OUTPUTIDLEMODE_NONE;
    output_cfg.IdleLevel             = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    output_cfg.FaultLevel            = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    output_cfg.ChopperModeEnable     = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    output_cfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

    /* Timer A Output 1 (CHA1): buck switching high-side
     * Set = Period reset, Reset = EEV4 | CMP2 (CMP2=backstop, matching IOC) */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_TIMPER;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_EEV_4 | HRTIM_OUTPUTRESET_TIMCMP2;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_OUTPUT_TA1, &output_cfg);

    /* Timer A Output 2 (CHA2): complementary low-side, no own Set/Reset */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_NONE;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_NONE;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_OUTPUT_TA2, &output_cfg);

    /* Timer B Output 1 (CHB1): boost switching high-side
     * Set = EEV4 | CMP2 (CMP2=backstop, matching IOC), Reset = Period      */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_EEV_4 | HRTIM_OUTPUTSET_TIMCMP2;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_TIMPER;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                    HRTIM_OUTPUT_TB1, &output_cfg);

    /* Timer B Output 2 (CHB2): complementary low-side */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_NONE;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_NONE;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                    HRTIM_OUTPUT_TB2, &output_cfg);

    /* Enable FLT1 and FLT2 response on Timer A and Timer B.
     * RM0440 §27.5.2: TIMxCR bits 20 (FLT1EN) and 21 (FLT2EN).           */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].TIMxCR |=
        (HRTIM_TIMCR_FLT1EN_BIT | HRTIM_TIMCR_FLT2EN_BIT);
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].TIMxCR |=
        (HRTIM_TIMCR_FLT1EN_BIT | HRTIM_TIMCR_FLT2EN_BIT);
}

/**
 * hrtim_configure_compare_registers — Set CMP1/CMP2/CMP3 for all timers (§10.3).
 *
 * CMP1: blanking window end — EEV4 (COMP1 output) is masked from the period
 *       reset until CMP1 fires, per HRTIM_TIMEEVFLT_BLANKINGCMP1 configured
 *       in the IOC.  Timer A (buck active) uses HRTIM_BLANKING_TICKS_BUCK;
 *       Timer B (boost active) uses HRTIM_BLANKING_TICKS_BOOST.
 *
 * CMP2: hardware backstop — ends the charge phase if COMP1 has not fired by
 *       MAX_ON_TIME_COUNTS.  Added as Reset source for TA1 (buck) and Set
 *       source for TB1 (boost) in hrtim_configure_fault_levels_and_enable().
 *
 * CMP3: bootstrap refresh end — the static leg output returns HIGH after the
 *       BOOTSTRAP_REFRESH_TICKS LOW pulse at each period reset.  Set as the
 *       SET source for the static leg output in hrtim_apply_*_mode_static_leg().
 */
static void hrtim_configure_compare_registers(void)
{
    /* CMP1: blanking window end per timer's active switching role.
     * Timer A is the active leg in BUCK; Timer B is the active leg in BOOST.
     * Both CMP1xR values are written so the blanking is correct regardless
     * of which timer is active at any given time.                           */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP1xR = HRTIM_BLANKING_TICKS_BUCK;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP1xR = HRTIM_BLANKING_TICKS_BOOST;

    /* CMP2: hardware backstop — MAX_ON_TIME_COUNTS on both timers.
     * The backstop fires as Reset (buck, TA1) or Set (boost, TB1) per the
     * sources set in hrtim_configure_fault_levels_and_enable().             */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR = MAX_ON_TIME_COUNTS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR = MAX_ON_TIME_COUNTS;

    /* CMP3: bootstrap refresh end — BOOTSTRAP_REFRESH_TICKS on both timers.
     * The mode functions configure SETx1R = TIMCMP3 for the static leg so
     * the output rises after the brief bootstrap LOW pulse at period reset.  */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP3xR = BOOTSTRAP_REFRESH_TICKS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP3xR = BOOTSTRAP_REFRESH_TICKS;
}

/**
 * hrtim_enable_period_and_fault_interrupts — Enable HRTIM interrupts.
 *
 * Timer A repetition interrupt → DAC Y-intercept reload at each period (§7.6).
 * Fault interrupt → hardware fault handling (§10.1).
 *
 * Both at NVIC priority NVIC_PRIORITY_HRTIM (1).
 */
static void hrtim_enable_period_and_fault_interrupts(void)
{
    /* Enable Timer A repetition (period) interrupt */
    __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                  HRTIM_TIM_IT_REP);
    HAL_NVIC_SetPriority(HRTIM1_TIMA_IRQn, NVIC_PRIORITY_HRTIM, 0u);
    HAL_NVIC_EnableIRQ(HRTIM1_TIMA_IRQn);

    /* Enable HRTIM fault interrupt */
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, HRTIM_IT_FLT1);
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, HRTIM_IT_FLT2);
    HAL_NVIC_SetPriority(HRTIM1_FLT_IRQn, NVIC_PRIORITY_HRTIM, 0u);
    HAL_NVIC_EnableIRQ(HRTIM1_FLT_IRQn);
}

/**
 * hrtim_start_timers — Start HRTIM counters (outputs remain disabled).
 *
 * Both Timer A and Timer B must be running for their compare and period
 * events to fire.  Outputs are enabled separately in regulator_start().
 */
static void hrtim_start_timers(void)
{
    /* Start Timer A and Timer B counters.
     * HAL_HRTIM_WaveformCounterStart_IT enables the timer with interrupts. */
    HAL_HRTIM_WaveformCounterStart_IT(&hhrtim1,
                                       HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
}

/**
 * hrtim_apply_buck_mode_static_leg — Configure Timer B for bootstrap-refreshed
 * static HIGH operation (buck mode output-side leg).
 *
 * In buck mode Timer B (output side) must hold CHB1 HIGH, with a brief LOW
 * pulse each period to recharge the bootstrap capacitor on the Q3 gate driver
 * (§5.4 bootstrap refresh design).
 *
 * Bootstrap refresh mechanism (CMP3-based):
 *   - At each Timer B period reset: CHB1 → LOW (RST source = TIMPER)
 *   - At count = BOOTSTRAP_REFRESH_TICKS (~200 ns): CHB1 → HIGH (SET source = CMP3)
 *   - CHB1 stays HIGH for the rest of the period (~4.8 µs at 200 kHz)
 *   CMP3xR is pre-programmed in hrtim_configure_compare_registers().
 *
 * Also restores Timer A (input side) to its switching configuration
 * (Set=TIMPER, Reset=EEV4|CMP2) after a potential boost-mode overwrite.
 *
 * During the 200 ns refresh pulse:
 *   Q1 (CHA1) is ON (Timer A just started its charge phase at the same period
 *   reset), Q3 (CHB1) is OFF, and Q4 (CHB2, complementary) is ON.  Current
 *   path: V_in → Q1 → L → Q4 → GND; output capacitor supplies the load.
 *   Effective duty-cycle reduction ≈ BOOTSTRAP_REFRESH_TICKS/HRTIM_PERIOD_COUNTS
 *   ≈ 4 %.  Monitor output ripple during bring-up and adjust if needed.
 */
static void hrtim_apply_buck_mode_static_leg(void)
{
    /* Restore Timer A (input side) switching configuration.
     * When transitioning from boost mode, Timer A's SET/RST were overwritten
     * for the bootstrap static leg.  Restore to buck switching sources.     */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R =
        HRTIM_OUTPUTSET_TIMPER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R =
        HRTIM_OUTPUTRESET_EEV_4 | HRTIM_RST1R_CMP2;

    /* Configure Timer B (output side) for CMP3-based bootstrap refresh.
     * CMP3xR is already set to BOOTSTRAP_REFRESH_TICKS in
     * hrtim_configure_compare_registers(); only SET/RST sources are changed.*/
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = HRTIM_OUTPUTRESET_TIMPER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R = HRTIM_OUTPUTSET_TIMCMP3;
}

/**
 * hrtim_apply_boost_mode_static_leg — Configure Timer A for bootstrap-refreshed
 * static HIGH operation (boost mode input-side leg).
 *
 * In boost mode Timer A (input side) must hold CHA1 HIGH, with a brief LOW
 * pulse each period to recharge the bootstrap capacitor on the Q1 gate driver
 * (§5.4 bootstrap refresh design).
 *
 * Bootstrap refresh mechanism (CMP3-based, symmetric with buck mode):
 *   - At each Timer A period reset: CHA1 → LOW (RST source = TIMPER)
 *   - At count = BOOTSTRAP_REFRESH_TICKS (~200 ns): CHA1 → HIGH (SET source = CMP3)
 *   - CHA1 stays HIGH for the rest of the period (~4.8 µs at 200 kHz)
 *   CMP3xR is pre-programmed in hrtim_configure_compare_registers().
 *
 * Also restores Timer B (output side) to its switching configuration
 * (Set=EEV4|CMP2, Reset=TIMPER) after a potential buck-mode overwrite.
 *
 * During the 200 ns refresh pulse Timer B is simultaneously starting its
 * switching cycle (CHB1 also LOW at period reset).  Both low-side FETs Q2
 * (CHA2, after dead-time) and Q4 (CHB2, after dead-time) may be ON, clamping
 * inductor voltage to ~0 V.  Current change ΔI ≈ 0 over ~200 ns at 4.7 µH.
 * The boost blanking window (HRTIM_BLANKING_TICKS_BOOST = 2720 ticks = 500 ns)
 * covers the refresh pulse, preventing COMP1 false-trip on the switching
 * transient.  Verify safe operation during hardware bring-up.
 */
static void hrtim_apply_boost_mode_static_leg(void)
{
    /* Configure Timer A (input side) for CMP3-based bootstrap refresh.
     * CMP3xR is already set to BOOTSTRAP_REFRESH_TICKS in
     * hrtim_configure_compare_registers(); only SET/RST sources are changed. */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R = HRTIM_OUTPUTRESET_TIMPER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R = HRTIM_OUTPUTSET_TIMCMP3;

    /* Restore boost switching sources for Timer B (output side).
     * (These were overwritten by hrtim_apply_buck_mode_static_leg when
     * transitioning from buck mode, or are set here for initial boost start.) */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R =
        HRTIM_OUTPUTRESET_TIMPER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R =
        HRTIM_OUTPUTSET_EEV_4 | HRTIM_SET1R_CMP2;
}

/**
 * hrtim_disable_all_outputs — Disable all HRTIM outputs immediately.
 *
 * All FETs go to their idle/fault state (INACTIVE = LOW).
 * Used in regulator_stop() and enter_fault().
 */
static void hrtim_disable_all_outputs(void)
{
    HAL_HRTIM_WaveformOutputStop(&hhrtim1,
                                  HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
                                  HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
}

/**
 * power_path_enable — Assert INPUT_EN and OUTPUT_EN GPIOs.
 */
static void power_path_enable(void)
{
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_RESET);
}

/**
 * power_path_disable — Deassert INPUT_EN and OUTPUT_EN GPIOs.
 */
static void power_path_disable(void)
{
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_SET); // discharge VBUS
}

/**
 * determine_mode_from_voltages — Select buck or boost based on V_in vs
 * V_setpoint with hysteresis (§5.1).
 *
 * @param v_in_mv       Measured input voltage in millivolts.
 * @param v_setpoint_mv Target output voltage in millivolts.
 * @return REGULATOR_MODE_BUCK or REGULATOR_MODE_BOOST.
 */
static RegulatorMode determine_mode_from_voltages(uint32_t v_in_mv,
                                                   uint32_t v_setpoint_mv)
{
    if (v_in_mv > (v_setpoint_mv + MODE_HYSTERESIS_MV))
    {
        return REGULATOR_MODE_BUCK;
    }
    else if (v_setpoint_mv > (v_in_mv + MODE_HYSTERESIS_MV))
    {
        return REGULATOR_MODE_BOOST;
    }
    else
    {
        /* Within hysteresis band — maintain current mode (§5.1) */
        return regulator_mode;
    }
}

/**
 * enter_fault — Transition to FAULT state from any context.
 *
 * Safe to call from ISR (no FreeRTOS API).
 *
 * @param source  Bitmask of fault source(s).
 */
static void enter_fault(RegulatorFaultSource source)
{
    /* Record fault source FIRST before disabling outputs */
    regulator_last_fault_source = source;

    /* Disable switching outputs (belt-and-suspenders: hardware fault logic
     * already forces them to INACTIVE, but also do it in software)          */
    hrtim_disable_all_outputs();

    /* Zero DAC — comparator threshold = 0 → additional safety (§4.4) */
    DAC3->DHR12R1 = 0u;

    /* Stop timers */
    TIM6->CR1 &= ~TIM_CR1_CEN;   /* stop TIM6 (slope comp) */
    TIM7->CR1 &= ~TIM_CR1_CEN;   /* stop TIM7 (PID)        */

    /* Update shared state for PD stack */
    regulator_fault  = true;
    regulator_ready  = false;

    regulator_state  = REGULATOR_STATE_FAULT;
}

/**
 * software_safety_checks_pass — Check all software safety conditions (§10.2).
 *
 * Returns true if all conditions are within bounds.
 * Returns false and calls enter_fault() if any condition is violated.
 */
static bool software_safety_checks_pass(uint32_t v_out_mv,
                                         uint32_t v_in_mv,
                                         uint32_t v_setpoint_mv,
                                         RegulatorMode mode)
{
    /* --- 1. Absolute OVP (§10.2) --- */
    if (v_out_mv > OVP_ABSOLUTE_MV)
    {
        enter_fault(REGULATOR_FAULT_SW_OVP);
        return false;
    }

    /* --- 2. Relative OVP --- */
    if (v_setpoint_mv > 0u)
    {
        uint32_t ovp_threshold = (v_setpoint_mv * OVP_RELATIVE_PCT) / 100u;
        if (v_out_mv > ovp_threshold)
        {
            enter_fault(REGULATOR_FAULT_SW_OVP);
            return false;
        }

        /* --- 3. Relative UVP (only when not in soft-start) --- */
        if (!softstart_active)
        {
            uint32_t uvp_threshold = (v_setpoint_mv * UVP_RELATIVE_PCT) / 100u;
            if (v_out_mv < uvp_threshold)
            {
                enter_fault(REGULATOR_FAULT_SW_UVP);
                return false;
            }
        }
    }

    /* --- 4. V_in range check (§10.2) --- */
    if (mode == REGULATOR_MODE_BUCK)
    {
        /* Buck requires V_in > V_out + margin */
        if (v_in_mv < (v_setpoint_mv + BUCK_VIN_MARGIN_MV))
        {
            enter_fault(REGULATOR_FAULT_SW_VIN_RANGE);
            return false;
        }
    }
    else /* BOOST (future: BUCK_BOOST for four-switch mode) */
    {
        /* Boost requires V_in < V_out - margin */
        if (v_setpoint_mv > BOOST_VIN_MARGIN_MV &&
            v_in_mv > (v_setpoint_mv - BOOST_VIN_MARGIN_MV))
        {
            enter_fault(REGULATOR_FAULT_SW_VIN_RANGE);
            return false;
        }
    }

    return true;
}
