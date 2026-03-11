/**
 * @file    power_stage.c
 * @brief   Buck-boost power stage control implementation.
 *
 * Implements the regulator state machine, peak-current-mode control via
 * HRTIM + COMP1 + DAC3, slope compensation (TIM6 staircase), and the PID
 * outer voltage loop (TIM7).
 *
 * All ISR entry points are implemented as HAL callback overrides
 * (HAL_TIM_PeriodElapsedCallback, HAL_HRTIM_RepetitionEventCallback,
 * HAL_HRTIM_Compare1EventCallback, HAL_HRTIM_FaultNotification), which
 * survive CubeMX code regeneration without any modification to
 * stm32g4xx_it.c.
 *
 * NLSpec: buck-boost-nlspec-v0_1_2d, §4–§12
 *
 * Peripheral handles required (declared in CubeMX-generated main.c):
 *   hhrtim1  — HRTIM1 (Timer A and Timer B, EEV4, FLT1/FLT2)
 *   hdac3    — DAC3 Channel 1 (slope-compensated peak current reference)
 *   hadc1    — ADC1 (VD_MON / IL_MON)
 *   hadc2    — ADC2 (VS_MON)
 *   hcomp1   — COMP1 (IL_MON vs DAC3_CH1; output → EEV4)
 *   htim6    — TIM6 (slope compensation, NVIC priority 0)
 *   htim7    — TIM7 (PID voltage loop, NVIC priority 2)
 */

#include "power_stage.h"
#include "regulator_config.h"
#include "main.h"
#include "stm32g4xx_hal.h"
#include <string.h>

/* =========================================================================
 * External HAL handles (defined in CubeMX-generated main.c)
 * ========================================================================= */
extern HRTIM_HandleTypeDef  hhrtim1;
extern DAC_HandleTypeDef    hdac3;
extern ADC_HandleTypeDef    hadc1;
extern ADC_HandleTypeDef    hadc2;
extern COMP_HandleTypeDef   hcomp1;
extern TIM_HandleTypeDef    htim6; /* slope compensation */
extern TIM_HandleTypeDef    htim7; /* PID voltage loop   */

/* =========================================================================
 * SHARED STATE — written here, read by PD stack  (NLSpec §9.1)
 * ========================================================================= */
volatile uint32_t target_voltage_mv = 0u;
volatile bool     regulator_ready   = false;
volatile bool     regulator_fault   = false;

/* =========================================================================
 * PRIVATE STATE
 * ========================================================================= */

/** Current top-level state machine state. */
static volatile PS_State_t s_state = PS_STATE_INIT;

/** Current operating mode (buck / boost). */
static volatile PS_Mode_t  s_mode  = PS_MODE_BUCK;

/* --- PID state (NLSpec §8.4, §8.7) --- */
static volatile float      s_pid_integrator   = 0.0f;
static volatile float      s_pid_error_prev   = 0.0f;
static volatile uint32_t   s_pid_output_dac   = 0u; /* DAC counts, Y-intercept */

/** Runtime-mutable PID coefficients (NLSpec §8.7). */
static float s_pid_kp = 1.0f;
static float s_pid_ki = 100.0f;
static float s_pid_kd = 0.0f;

/** Runtime-mutable safety thresholds (NLSpec §10.2). */
static uint8_t  s_ovp_pct      = OVP_RELATIVE_PCT_DEFAULT;
static uint8_t  s_uvp_pct      = UVP_RELATIVE_PCT_DEFAULT;
static uint8_t  s_max_backstop = MAX_CONSECUTIVE_BACKSTOPS_DEFAULT;
static uint8_t  s_integrator_reset_pct = PID_INTEGRATOR_RESET_PCT_DEFAULT;

/* --- ADC telemetry (updated by PID ISR and background ADC conversions) --- */
static volatile uint32_t s_vout_mv = 0u;
static volatile uint32_t s_vin_mv  = 0u;
static volatile uint32_t s_il_ma   = 0u;

/* --- Slope compensation --- */
/**
 * Precomputed DAC decrement per TIM6 tick.
 * Units: DAC counts per SLOPE_COMP_RATE_HZ tick.
 * Derivation: slope_a_per_s × DAC_COUNTS_PER_AMP / SLOPE_COMP_RATE_HZ
 */
static volatile uint32_t s_slope_step_dac = 0u;

/** Runtime-mutable slope value (NLSpec §7.3). */
static uint32_t s_slope_a_per_s = SLOPE_A_PER_S_DEFAULT;

/* --- Backstop violation counter (NLSpec §10.4) --- */
static volatile uint8_t  s_consecutive_backstops = 0u;
static volatile bool     s_comp1_fired_this_period = false;

/* --- Soft-start (NLSpec §11.2) --- */
static volatile bool     s_softstart_active  = false;
static volatile float    s_softstart_setpoint_mv = 0.0f;
static volatile float    s_softstart_ramp_mv_per_pid = 0.0f;

/* =========================================================================
 * PRIVATE HELPER PROTOTYPES
 * ========================================================================= */
static void ps_configure_hrtim_buck(void);
static void ps_configure_hrtim_boost(void);
static void ps_configure_static_leg_buck(void);
static void ps_configure_static_leg_boost(void);
static void ps_disable_all_outputs(void);
static void ps_zero_dac(void);
static void ps_precompute_slope_step(void);
static void ps_enter_fault(void);
static PS_Mode_t ps_select_mode(uint32_t vin_mv, uint32_t vset_mv);

/* =========================================================================
 * SHARED STATE — PUBLIC GETTERS
 * ========================================================================= */

PS_State_t PS_GetState(void)  { return s_state; }
PS_Mode_t  PS_GetMode(void)   { return s_mode;  }
uint32_t   PS_GetVout_mV(void){ return s_vout_mv; }
uint32_t   PS_GetVin_mV(void) { return s_vin_mv;  }
uint32_t   PS_GetIL_mA(void)  { return s_il_ma;   }

/* =========================================================================
 * PD ↔ REGULATOR INTERFACE  (NLSpec §9.2)
 * ========================================================================= */

void regulator_set_target_voltage(uint32_t voltage_mv)
{
    if (voltage_mv == 0u) {
        /* 0 mV setpoint while RUNNING → transition to IDLE (NLSpec §9.2) */
        if (s_state == PS_STATE_RUNNING) {
            PS_Stop();
        }
        target_voltage_mv = 0u;
        return;
    }

    if (voltage_mv > VSETPOINT_MAX_MV) {
        voltage_mv = VSETPOINT_MAX_MV;
    }

    /* Check whether the step is large enough to warrant an integrator reset */
    if ((s_state == PS_STATE_RUNNING) && (target_voltage_mv > 0u)) {
        uint32_t prev = target_voltage_mv;
        uint32_t delta = (voltage_mv > prev) ? (voltage_mv - prev) : (prev - voltage_mv);
        /* delta > threshold_pct % of prev → reset integrator (NLSpec §8.8) */
        if ((delta * 100u) > ((uint32_t)s_integrator_reset_pct * prev)) {
            s_pid_integrator = 0.0f;
            s_pid_error_prev = 0.0f;
        }
    }

    target_voltage_mv = voltage_mv; /* single aligned 32-bit write — atomic on Cortex-M4 */
}

void regulator_stop(void)         { PS_Stop(); }
void regulator_clear_fault(void)  { PS_ClearFault(); }

/* =========================================================================
 * PS_Init — post-CubeMX initialisation  (NLSpec §4.1)
 * ========================================================================= */

void PS_Init(void)
{
    /* 1. Configure HRTIM Timer A and Timer B Set/Reset sources for buck mode
     *    (default operating mode). See NLSpec §5.2, §5.5. */
    ps_configure_hrtim_buck();

    /* 2. Configure the static leg (Timer B) for buck mode:
     *    CHB1 high throughout period with end-of-period bootstrap refresh. */
    ps_configure_static_leg_buck();

    /* 3. Set backstop Compare 1 on both Timer A and Timer B. See NLSpec §10.3. */
    __HAL_HRTIM_SETCOMPARE(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                            HRTIM_COMPAREUNIT_1, MAX_ON_TIME_COUNTS);
    __HAL_HRTIM_SETCOMPARE(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                            HRTIM_COMPAREUNIT_1, MAX_ON_TIME_COUNTS);

    /* 4. Set bootstrap refresh Compare 2 on both timers. See NLSpec §5.4. */
    __HAL_HRTIM_SETCOMPARE(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                            HRTIM_COMPAREUNIT_2, BOOTSTRAP_CMP2_COUNTS);
    __HAL_HRTIM_SETCOMPARE(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                            HRTIM_COMPAREUNIT_2, BOOTSTRAP_CMP2_COUNTS);

    /* 5. Zero DAC3 CH1 — comparator threshold = 0, no switching. */
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);
    ps_zero_dac();

    /* 6. Start COMP1. See NLSpec §4.1. */
    HAL_COMP_Start(&hcomp1);

    /* 7. Start ADC1 (VD_MON, IL_MON) and ADC2 (VS_MON) in continuous mode. */
    HAL_ADC_Start(&hadc1);
    HAL_ADC_Start(&hadc2);

    /* 8. Precompute slope compensation step size. See NLSpec §7.5. */
    ps_precompute_slope_step();

    /* 9. Configure NVIC priorities for regulator ISRs. See NLSpec §13.2.
     *    These must be below configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (3)
     *    to be above FreeRTOS masking. */
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn,    NVIC_PRIO_SLOPE_COMP, 0u);
    HAL_NVIC_SetPriority(TIM7_IRQn,        NVIC_PRIO_PID,        0u);
    HAL_NVIC_SetPriority(HRTIM1_TIMA_IRQn, NVIC_PRIO_HRTIM,      0u);
    HAL_NVIC_SetPriority(HRTIM1_TIMB_IRQn, NVIC_PRIO_HRTIM,      0u);
    HAL_NVIC_SetPriority(HRTIM1_FLT_IRQn,  NVIC_PRIO_HRTIM,      0u);

    /* 10. Enable HRTIM fault inputs FLT1 and FLT2. See NLSpec §10.1.
     *     The fault polarity and output state (all FETs off = INACTIVE) must
     *     be configured in the CubeMX project. This call arms the fault
     *     detection and enables the fault interrupt. */
    HAL_HRTIM_EnableFault(&hhrtim1, HRTIM_FAULT_1);
    HAL_HRTIM_EnableFault(&hhrtim1, HRTIM_FAULT_2);

    /* 11. Verify no active fault before declaring IDLE. */
    uint32_t fault_status = HRTIM1->sCommonRegs.ISR;
    if (fault_status & (HRTIM_ISR_FLT1 | HRTIM_ISR_FLT2)) {
        ps_enter_fault();
        return;
    }

    /* 12. All outputs remain inactive (IDLE). Do not start switching yet. */
    s_state = PS_STATE_IDLE;
    regulator_fault = false;
    regulator_ready = false;
}

/* =========================================================================
 * PS_Start — IDLE → RUNNING  (NLSpec §4.2→4.3, §11.1)
 * ========================================================================= */

void PS_Start(uint32_t voltage_mv, uint32_t current_ma)
{
    (void)current_ma; /* Reserved for future current-limiting wrapper. */

    if (s_state != PS_STATE_IDLE) {
        return;
    }

    if ((voltage_mv < VSETPOINT_MIN_MV) || (voltage_mv > VSETPOINT_MAX_MV)) {
        return;
    }

    /* Store target. */
    target_voltage_mv = voltage_mv;

    /* Perform a V_in ADC read to determine operating mode. */
    uint32_t vin  = s_vin_mv;
    PS_Mode_t mode = ps_select_mode(vin, voltage_mv);

    /* Reconfigure HRTIM if mode has changed. */
    if (mode != s_mode) {
        if (mode == PS_MODE_BUCK) {
            ps_configure_hrtim_buck();
            ps_configure_static_leg_buck();
        } else {
            ps_configure_hrtim_boost();
            ps_configure_static_leg_boost();
        }
        s_mode = mode;
    }

    /* Reset PID state before enabling (NLSpec §8.8). */
    s_pid_integrator = 0.0f;
    s_pid_error_prev = 0.0f;
    s_pid_output_dac = 0u;

    /* Reset backstop counter. */
    s_consecutive_backstops  = 0u;
    s_comp1_fired_this_period = false;

    /* Initialise soft-start ramp (NLSpec §11.2).
     * Ramp from 0 to target over SOFTSTART_RAMP_TIME_MS × PID_RATE_HZ / 1000
     * PID ticks. During soft-start the integrator is held at zero and only
     * the proportional ramp drives the setpoint. */
    uint32_t pid_ticks_for_ramp = (SOFTSTART_RAMP_TIME_MS * PID_RATE_HZ) / 1000u;
    if (pid_ticks_for_ramp == 0u) {
        pid_ticks_for_ramp = 1u;
    }
    s_softstart_setpoint_mv    = 0.0f;
    s_softstart_ramp_mv_per_pid = (float)voltage_mv / (float)pid_ticks_for_ramp;
    s_softstart_active         = true;

    /* Load initial DAC value = 0 (safe: no current until PID runs). */
    ps_zero_dac();

    /* Start slope compensation timer (TIM6, priority 0). */
    HAL_TIM_Base_Start_IT(&htim6);

    /* Start HRTIM outputs. */
    if (s_mode == PS_MODE_BUCK) {
        /* Enable Timer A (switching) and Timer B (static). */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1,
            HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
            HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
    } else {
        /* Enable Timer B (switching) and Timer A (static). */
        HAL_HRTIM_WaveformOutputStart(&hhrtim1,
            HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2 |
            HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2);
    }

    /* Enable HRTIM period (repetition) interrupt for DAC Y-intercept reload.
     * RepetitionCounter must be 0 in CubeMX config to fire every period. */
    if (s_mode == PS_MODE_BUCK) {
        __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_TIM_IT_REP);
    } else {
        __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                    HRTIM_TIM_IT_REP);
    }

    /* Enable HRTIM Compare 1 interrupt for backstop counting (NLSpec §10.4). */
    if (s_mode == PS_MODE_BUCK) {
        __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_TIM_IT_CMP1);
    } else {
        __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                    HRTIM_TIM_IT_CMP1);
    }

    /* Start PID timer (TIM7, priority 2). */
    HAL_TIM_Base_Start_IT(&htim7);

    s_state = PS_STATE_RUNNING;
    regulator_ready = false;
    regulator_fault = false;
}

/* =========================================================================
 * PS_Stop — RUNNING → IDLE  (NLSpec §11.3)
 * ========================================================================= */

void PS_Stop(void)
{
    /* Disable PID and slope comp timers first to prevent further DAC writes. */
    HAL_TIM_Base_Stop_IT(&htim7);
    HAL_TIM_Base_Stop_IT(&htim6);

    /* Disable HRTIM interrupts on both timers. */
    __HAL_HRTIM_TIMER_DISABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                  HRTIM_TIM_IT_REP | HRTIM_TIM_IT_CMP1);
    __HAL_HRTIM_TIMER_DISABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                  HRTIM_TIM_IT_REP | HRTIM_TIM_IT_CMP1);

    /* Disable all HRTIM outputs. */
    ps_disable_all_outputs();

    /* Zero DAC — comparator threshold = 0. */
    ps_zero_dac();

    s_softstart_active = false;
    regulator_ready    = false;

    if (s_state != PS_STATE_FAULT) {
        s_state = PS_STATE_IDLE;
    }
}

/* =========================================================================
 * PS_ClearFault  (NLSpec §4.4)
 * ========================================================================= */

void PS_ClearFault(void)
{
    if (s_state != PS_STATE_FAULT) {
        return;
    }

    /* Check whether the hardware fault source is still asserted. */
    uint32_t fault_status = HRTIM1->sCommonRegs.ISR;
    if (fault_status & (HRTIM_ISR_FLT1 | HRTIM_ISR_FLT2)) {
        /* Hardware fault still active — cannot clear yet. */
        return;
    }

    /* Clear HRTIM fault flags. */
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C;

    /* Reset PID and shared state. */
    s_pid_integrator = 0.0f;
    s_pid_error_prev = 0.0f;
    s_pid_output_dac = 0u;
    s_consecutive_backstops = 0u;

    regulator_fault = false;
    regulator_ready = false;
    s_state = PS_STATE_IDLE;
}

/* =========================================================================
 * PRIVATE: HRTIM OUTPUT CONFIGURATION
 * ========================================================================= */

/**
 * Configure Timer A as the switching leg for BUCK mode (NLSpec §5.2, §5.5).
 * CHA1: Set = Period (HIGH at period start); Reset = EEV4 | CMP1 (backstop).
 * Polarity: ACTIVE = HIGH.
 */
static void ps_configure_hrtim_buck(void)
{
    HRTIM_OutputCfgTypeDef ocfg = {0};

    /* Timer A Output 1 — switching leg (high-side, Q1). */
    ocfg.Polarity             = HRTIM_OUTPUTPOLARITY_HIGH;
    ocfg.SetSource            = HRTIM_OUTPUTSET_TIMPER;
    ocfg.ResetSource          = HRTIM_OUTPUTRESET_EEV_4 |
                                HRTIM_OUTPUTRESET_TIMCMP1;
    ocfg.IdleMode             = HRTIM_OUTPUTIDLEMODE_NONE;
    ocfg.IdleLevel            = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    ocfg.FaultLevel           = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    ocfg.ChopperModeEnable    = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    ocfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                   HRTIM_OUTPUT_TA1, &ocfg);

    /* Timer A Output 2 — complementary low-side (Q2); polarity governed by
     * the dead-time insertion configured in CubeMX. No explicit Set/Reset
     * needed here — it follows the complement of TA1. */
    ocfg.SetSource   = 0U;
    ocfg.ResetSource = 0U;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                   HRTIM_OUTPUT_TA2, &ocfg);
}

/**
 * Configure Timer B as the switching leg for BOOST mode (NLSpec §5.3, §5.5).
 * CHB1: Set = Period (→ LOW due to inverted polarity, charge phase begins);
 *       Reset = EEV4 | CMP1 (→ HIGH, discharge phase begins).
 * Polarity: ACTIVE = LOW (inverted relative to buck).
 */
static void ps_configure_hrtim_boost(void)
{
    HRTIM_OutputCfgTypeDef ocfg = {0};

    /* Timer B Output 1 — switching leg (high-side, Q3), inverted polarity. */
    ocfg.Polarity             = HRTIM_OUTPUTPOLARITY_LOW;
    ocfg.SetSource            = HRTIM_OUTPUTSET_TIMPER;
    ocfg.ResetSource          = HRTIM_OUTPUTRESET_EEV_4 |
                                HRTIM_OUTPUTRESET_TIMCMP1;
    ocfg.IdleMode             = HRTIM_OUTPUTIDLEMODE_NONE;
    ocfg.IdleLevel            = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    ocfg.FaultLevel           = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    ocfg.ChopperModeEnable    = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    ocfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                   HRTIM_OUTPUT_TB1, &ocfg);

    /* Timer B Output 2 — complementary low-side (Q4). */
    ocfg.Polarity    = HRTIM_OUTPUTPOLARITY_LOW;
    ocfg.SetSource   = 0U;
    ocfg.ResetSource = 0U;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                   HRTIM_OUTPUT_TB2, &ocfg);
}

/**
 * Configure Timer B as the STATIC leg for BUCK mode (NLSpec §5.2, §5.4).
 * CHB1 held HIGH for most of period; brief LOW at end-of-period for bootstrap
 * refresh (CMP2-triggered, duration = BOOTSTRAP_REFRESH_COUNTS).
 */
static void ps_configure_static_leg_buck(void)
{
    HRTIM_OutputCfgTypeDef ocfg = {0};

    ocfg.Polarity             = HRTIM_OUTPUTPOLARITY_HIGH;
    ocfg.SetSource            = HRTIM_OUTPUTSET_TIMPER;     /* HIGH at period start */
    ocfg.ResetSource          = HRTIM_OUTPUTRESET_TIMCMP2;  /* LOW at CMP2 (bootstrap) */
    ocfg.IdleMode             = HRTIM_OUTPUTIDLEMODE_NONE;
    ocfg.IdleLevel            = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    ocfg.FaultLevel           = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    ocfg.ChopperModeEnable    = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    ocfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                   HRTIM_OUTPUT_TB1, &ocfg);

    /* TB2 (low-side, Q4) is complementary and managed by dead-time insertion. */
    ocfg.SetSource   = 0U;
    ocfg.ResetSource = 0U;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                   HRTIM_OUTPUT_TB2, &ocfg);
}

/**
 * Configure Timer A as the STATIC leg for BOOST mode (NLSpec §5.3, §5.4).
 * CHA1 held HIGH for most of period; brief LOW at end-of-period for bootstrap
 * refresh (CMP2-triggered).
 */
static void ps_configure_static_leg_boost(void)
{
    HRTIM_OutputCfgTypeDef ocfg = {0};

    ocfg.Polarity             = HRTIM_OUTPUTPOLARITY_HIGH;
    ocfg.SetSource            = HRTIM_OUTPUTSET_TIMPER;     /* HIGH at period start */
    ocfg.ResetSource          = HRTIM_OUTPUTRESET_TIMCMP2;  /* LOW at CMP2 (bootstrap) */
    ocfg.IdleMode             = HRTIM_OUTPUTIDLEMODE_NONE;
    ocfg.IdleLevel            = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    ocfg.FaultLevel           = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    ocfg.ChopperModeEnable    = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    ocfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                   HRTIM_OUTPUT_TA1, &ocfg);

    /* TA2 (low-side, Q2) follows complement via dead-time insertion. */
    ocfg.SetSource   = 0U;
    ocfg.ResetSource = 0U;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                   HRTIM_OUTPUT_TA2, &ocfg);
}

/** Disable all HRTIM outputs (safe state). */
static void ps_disable_all_outputs(void)
{
    HAL_HRTIM_WaveformOutputStop(&hhrtim1,
        HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
        HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
}

/** Write 0 to DAC3 CH1 (direct register access for speed). */
static void ps_zero_dac(void)
{
    /* Direct register write — 32-bit aligned, atomic. */
    DAC3->DHR12R1 = 0u;
}

/** Precompute slope compensation step (DAC counts per TIM6 tick). */
static void ps_precompute_slope_step(void)
{
    /* slope_step_dac = slope_a_per_s × DAC_COUNTS_PER_AMP / SLOPE_COMP_RATE_HZ
     * Use 64-bit intermediate to avoid overflow. */
    uint64_t num = (uint64_t)s_slope_a_per_s * (uint64_t)DAC_COUNTS_PER_AMP;
    s_slope_step_dac = (uint32_t)(num / (uint64_t)SLOPE_COMP_RATE_HZ);
    if (s_slope_step_dac == 0u) {
        s_slope_step_dac = 1u; /* minimum 1 count per tick */
    }
}

/** Select buck or boost based on V_in vs. V_setpoint with hysteresis. */
static PS_Mode_t ps_select_mode(uint32_t vin_mv, uint32_t vset_mv)
{
    if (vin_mv > (vset_mv + MODE_HYSTERESIS_MV)) {
        return PS_MODE_BUCK;
    }
    if (vin_mv < (vset_mv > MODE_HYSTERESIS_MV ?
                  vset_mv - MODE_HYSTERESIS_MV : 0u)) {
        return PS_MODE_BOOST;
    }
    /* Within hysteresis band — keep current mode. */
    return s_mode;
}

/** Enter FAULT state. Called from any ISR or task context. */
static void ps_enter_fault(void)
{
    /* Disable timers and outputs immediately. */
    HAL_TIM_Base_Stop_IT(&htim7);
    HAL_TIM_Base_Stop_IT(&htim6);
    __HAL_HRTIM_TIMER_DISABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                  HRTIM_TIM_IT_REP | HRTIM_TIM_IT_CMP1);
    __HAL_HRTIM_TIMER_DISABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                  HRTIM_TIM_IT_REP | HRTIM_TIM_IT_CMP1);
    ps_disable_all_outputs();
    ps_zero_dac();

    /* Reset PID integrator. */
    s_pid_integrator = 0.0f;
    s_pid_error_prev = 0.0f;
    s_pid_output_dac = 0u;
    s_softstart_active = false;

    s_state         = PS_STATE_FAULT;
    regulator_ready = false;
    regulator_fault = true;
}

/* =========================================================================
 * HAL CALLBACK OVERRIDES — ISR HANDLERS
 *
 * These override the __weak HAL callbacks and are safe with CubeMX
 * regeneration: they do not require changes to stm32g4xx_it.c.
 * The CubeMX-generated IRQ handlers call HAL_TIM_IRQHandler /
 * HAL_HRTIM_IRQHandler, which dispatch to these callbacks.
 * ========================================================================= */

/* -----------------------------------------------------------------------
 * TIM6 ISR — Slope Compensation  (NLSpec §7.5)
 * NVIC priority 0 (highest — above FreeRTOS and all other regulator ISRs).
 * Fires at SLOPE_COMP_RATE_HZ (2 MHz default) while RUNNING.
 * Decrements DAC3 CH1 by s_slope_step_dac counts each tick.
 * The step size is precomputed at init; never recomputed here (NLSpec §7.5).
 * ----------------------------------------------------------------------- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6)
    {
        /* Slope compensation — decrement DAC3 CH1 each TIM6 tick. */
        if (s_state == PS_STATE_RUNNING) {
            uint32_t cur = DAC3->DHR12R1;
            if (cur >= s_slope_step_dac) {
                DAC3->DHR12R1 = cur - s_slope_step_dac;
            } else {
                DAC3->DHR12R1 = 0u;
            }
        }
    }
    else if (htim->Instance == TIM7)
    {
        /* PID voltage loop (NLSpec §8). */

        if (s_state != PS_STATE_RUNNING) {
            return;
        }

        /* --- Sample ADC values ---
         * Note: In the final CubeMX project, ADC1 should be configured with
         * VD_MON (IN1) as rank 1 in injected or single-channel continuous mode
         * so that HAL_ADC_GetValue returns the V_out measurement.
         * ADC2 uses VS_MON (IN17) for V_in. See NLSpec §8.5. */
        uint32_t vout_raw = HAL_ADC_GetValue(&hadc1);
        uint32_t vin_raw  = HAL_ADC_GetValue(&hadc2);

        /* Convert to millivolts (NLSpec §14.2). */
        uint32_t vout_mv = (uint32_t)(vout_raw * VOLTAGE_SCALE_NUM / VOLTAGE_SCALE_DEN);
        uint32_t vin_mv  = (uint32_t)(vin_raw  * VOLTAGE_SCALE_NUM / VOLTAGE_SCALE_DEN);

        /* Update telemetry (single 32-bit store — atomic). */
        s_vout_mv = vout_mv;
        s_vin_mv  = vin_mv;

        /* --- Software safety checks (NLSpec §10.2) --- */
        uint32_t vset = target_voltage_mv;

        /* Absolute OVP (compile-time constant — must not be tuned). */
        if (vout_mv > OVP_ABSOLUTE_MV) {
            ps_enter_fault();
            return;
        }

        if (vset > 0u) {
            /* Relative OVP: V_out > setpoint × OVP_pct / 100. */
            if (vout_mv > ((vset * (uint32_t)s_ovp_pct) / 100u)) {
                ps_enter_fault();
                return;
            }
            /* UVP: V_out < setpoint × UVP_pct / 100 (only after soft-start). */
            if (!s_softstart_active &&
                (vout_mv < ((vset * (uint32_t)s_uvp_pct) / 100u))) {
                ps_enter_fault();
                return;
            }
            /* V_in validation (NLSpec §10.2). */
            if (s_mode == PS_MODE_BUCK) {
                if (vin_mv < (vset + MODE_HYSTERESIS_MV / 2u)) {
                    ps_enter_fault();
                    return;
                }
            } else {
                if (vin_mv > (vset > MODE_HYSTERESIS_MV / 2u ?
                              vset - MODE_HYSTERESIS_MV / 2u : 0u)) {
                    ps_enter_fault();
                    return;
                }
            }
        }

        /* --- Backstop violation count check (NLSpec §10.4) --- */
        if (s_consecutive_backstops >= s_max_backstop) {
            ps_enter_fault();
            return;
        }

        /* --- Mode re-selection (NLSpec §5.1) --- */
        if (vset > 0u) {
            PS_Mode_t new_mode = ps_select_mode(vin_mv, vset);
            if (new_mode != s_mode) {
                /* Controlled mode transition sequence (NLSpec §5.1). */
                PS_Stop();
                s_mode = new_mode;
                PS_Start(vset, 0u);
                return;
            }
        }

        /* --- Soft-start ramp (NLSpec §11.2) --- */
        float effective_setpoint_mv;
        if (s_softstart_active) {
            s_softstart_setpoint_mv += s_softstart_ramp_mv_per_pid;
            if (s_softstart_setpoint_mv >= (float)vset) {
                s_softstart_setpoint_mv = (float)vset;
                s_softstart_active = false;
            }
            effective_setpoint_mv = s_softstart_setpoint_mv;
        } else {
            effective_setpoint_mv = (float)vset;
        }

        /* --- PID computation (NLSpec §8.4) --- */
        float dt       = 1.0f / (float)PID_RATE_HZ;
        float error    = effective_setpoint_mv - (float)vout_mv;

        float p_term   = s_pid_kp * error;
        float i_term   = s_pid_integrator + s_pid_ki * error * dt;
        float d_term   = s_pid_kd * (error - s_pid_error_prev) / dt;
        float output   = p_term + i_term + d_term;

        /* Anti-windup: clamp integrator (back-calculation, NLSpec §8.4). */
        if (output > (float)PID_OUTPUT_MAX) {
            i_term -= (output - (float)PID_OUTPUT_MAX);
            output  = (float)PID_OUTPUT_MAX;
        } else if (output < (float)PID_OUTPUT_MIN) {
            i_term -= (output - (float)PID_OUTPUT_MIN);
            output  = (float)PID_OUTPUT_MIN;
        }

        s_pid_integrator = i_term;
        s_pid_error_prev = error;

        /* Clamp to DAC range [PID_OUTPUT_MIN, PID_OUTPUT_MAX]. */
        uint32_t dac_counts = (uint32_t)output;
        if (dac_counts > PID_OUTPUT_MAX) {
            dac_counts = PID_OUTPUT_MAX;
        }

        /* Publish new Y-intercept for slope comp to use at next period reset. */
        s_pid_output_dac = dac_counts; /* single 32-bit store — atomic */

        /* --- Update regulator_ready status --- */
        if (vset > 0u) {
            uint32_t diff = (vout_mv > vset) ? (vout_mv - vset) : (vset - vout_mv);
            regulator_ready = (diff <= VREGULATION_WINDOW_MV);
        } else {
            regulator_ready = false;
        }
    }
}

/* -----------------------------------------------------------------------
 * HRTIM RepetitionEvent Callback — DAC Y-intercept reload  (NLSpec §7.6)
 * NVIC priority 1.  Fires at start of each switching period (RepetitionCounter
 * must be configured as 0 in the CubeMX project for Timer A and Timer B).
 * Reloads DAC3 CH1 with s_pid_output_dac BEFORE the blanking window expires.
 * Also resets the COMP1-fired flag and checks backstop counter.
 * ----------------------------------------------------------------------- */
void HAL_HRTIM_RepetitionEventCallback(HRTIM_HandleTypeDef *hhrtim,
                                        uint32_t TimerIdx)
{
    (void)hhrtim;

    /* Only process the active switching timer. */
    bool is_active_timer = (s_mode == PS_MODE_BUCK)
                            ? (TimerIdx == HRTIM_TIMERINDEX_TIMER_A)
                            : (TimerIdx == HRTIM_TIMERINDEX_TIMER_B);

    if (!is_active_timer || (s_state != PS_STATE_RUNNING)) {
        return;
    }

    /* --- Backstop counting (NLSpec §10.4) --- */
    if (s_comp1_fired_this_period) {
        /* COMP1 fired naturally — reset counter. */
        s_consecutive_backstops = 0u;
    } else {
        /* COMP1 did not fire — backstop may have fired; increment counter.
         * The actual fault check happens in the PID ISR to keep this ISR short. */
        if (s_consecutive_backstops < 255u) {
            s_consecutive_backstops++;
        }
    }
    s_comp1_fired_this_period = false;

    /* --- Reload DAC Y-intercept for slope compensation (NLSpec §7.6) ---
     * Direct register write is necessary to ensure the DAC is updated before
     * the blanking window expires (~544 counts = 100 ns after period reset).
     * s_pid_output_dac is written by TIM7 ISR and read here; both are 32-bit
     * aligned volatile accesses — atomic on Cortex-M4. */
    DAC3->DHR12R1 = s_pid_output_dac;
}

/* -----------------------------------------------------------------------
 * HRTIM Compare1 Callback — Backstop event tracking  (NLSpec §10.3, §10.4)
 * NVIC priority 1.  Fires when CMP1 (MAX_ON_TIME_COUNTS) is reached without
 * COMP1 tripping.  Clears the comp1_fired flag so the RepetitionEvent
 * callback counts it as a backstop event.
 * Note: COMP1 firing via EEV4 resets the output without generating an ISR.
 * We infer COMP1 fired if the CMP1 interrupt does NOT fire in a period.
 * ----------------------------------------------------------------------- */
void HAL_HRTIM_Compare1EventCallback(HRTIM_HandleTypeDef *hhrtim,
                                      uint32_t TimerIdx)
{
    (void)hhrtim;
    (void)TimerIdx;
    /* CMP1 fired (backstop). COMP1 did not trip naturally this period. */
    /* s_comp1_fired_this_period remains false — counted at next period start. */
}

/* -----------------------------------------------------------------------
 * HRTIM Fault Notification — Hardware fault handling  (NLSpec §10.1)
 * NVIC priority 1.  Fires when FLT1 or FLT2 asserts.  The hardware has
 * already forced all outputs to INACTIVE.  Software updates state.
 * ----------------------------------------------------------------------- */
void HAL_HRTIM_FaultNotification(HRTIM_HandleTypeDef *hhrtim, uint32_t Fault)
{
    (void)hhrtim;
    (void)Fault;
    ps_enter_fault();
}

/* =========================================================================
 * MODE TRANSITION HELPER
 *
 * Called from PID ISR when a mode change is needed.  Sequence per NLSpec §5.1:
 *  1. Disable outputs.  2. Reconfigure.  3. Reset PID.  4. Soft-start.
 * PS_Stop() and PS_Start() implement the full sequence.
 * ========================================================================= */
/* (Implemented inline within the TIM7 PID block above.) */
