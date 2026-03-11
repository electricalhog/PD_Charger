/**
 * @file    slope_comp.c
 * @brief   TIM6-based slope compensation for peak current mode control (NLSpec §7).
 *
 * Implements the "Timer ISR staircase" ramp mechanism (§7.5 option 1):
 *   - TIM6 fires at 2 MHz.
 *   - Each ISR reads DAC3 CH1, subtracts the precomputed step, writes back.
 *   - The step value is precomputed in slope_comp_update_step() (called from
 *     the lower-priority TIM7 PID ISR) and stored in a volatile variable.
 *   - slope_comp_step_counts is NEVER recomputed in the ISR (§7.2 requirement).
 *
 * DAC reload at period start:
 *   slope_comp_reload_dac_peak() is called from the HRTIM Timer A period ISR
 *   to reset DAC3 CH1 to the current PID output peak before the blanking
 *   window expires (§7.6).
 */

#include "slope_comp.h"
#include "regulator_config.h"
#include "main.h"       /* hhrtim1, hdac3, htim6 handles */
#include "stm32g4xx_hal.h"

/* External peripheral handles declared in main.c */
extern DAC_HandleTypeDef  hdac3;
extern TIM_HandleTypeDef  htim6;

/* =========================================================================
 * Module-level shared state
 * =========================================================================*/

/**
 * slope_comp_step_counts — DAC decrement per TIM6 tick.
 * Written by slope_comp_update_step() (TIM7 context, priority 2).
 * Read   by slope_comp_tim6_isr()     (TIM6 context, priority 0).
 * 32-bit aligned on Cortex-M4: write is atomic (§8.6).
 */
volatile uint32_t slope_comp_step_counts = 0u;

/**
 * slope_comp_peak_dac_counts — PID output peak (DAC Y-intercept).
 * Written by slope_comp_set_peak()     (TIM7 context, priority 2).
 * Read   by slope_comp_reload_dac_peak() (HRTIM context, priority 1).
 * 32-bit aligned on Cortex-M4: write is atomic.
 */
volatile uint32_t slope_comp_peak_dac_counts = 0u;

/* =========================================================================
 * Internal helpers
 * =========================================================================*/

/**
 * dac3_ch1_write — Write a value directly to DAC3 channel 1 data register.
 *
 * Uses the 12-bit right-aligned holding register (DHR12R1).  This is the
 * lowest-latency path (no HAL overhead) for use inside ISRs.
 *
 * @param counts  DAC value in [0, 4095].
 */
static inline void dac3_ch1_write(uint32_t counts)
{
    /* Clamp to 12-bit range before writing */
    if (counts > 4095u)
    {
        counts = 4095u;
    }
    DAC3->DHR12R1 = counts;
}

/* =========================================================================
 * Public API implementations
 * =========================================================================*/

void slope_comp_init(void)
{
    /* Reconfigure TIM6 for 2 MHz slope compensation rate (§7.3).
     * MX_TIM6_Init() sets Period = 65535 (placeholder); we overwrite here.
     * PSC = 0, ARR = 84 → 170 MHz / (0+1) / (84+1) = 2 000 000 Hz ✓      */
    TIM6->PSC = TIM6_PRESCALER;
    TIM6->ARR = TIM6_PERIOD_COUNTS;
    TIM6->EGR = TIM_EGR_UG;   /* force update event to load new PSC/ARR */

    /* Configure TIM6 interrupt at the highest regulator priority (0).
     * TIM6 shares the interrupt vector with DAC underrun (TIM6_DAC_IRQn).  */
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, NVIC_PRIORITY_SLOPE_COMP_TIM6, 0u);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);

    /* Initialise shared state */
    slope_comp_step_counts    = 0u;
    slope_comp_peak_dac_counts = 0u;

    /* DAC3 CH1 starts at 0 — comparator threshold = 0 → safe state */
    dac3_ch1_write(0u);
}

void slope_comp_start(uint32_t initial_peak_counts)
{
    slope_comp_peak_dac_counts = initial_peak_counts;
    dac3_ch1_write(initial_peak_counts);

    /* Start TIM6 with interrupt enabled */
    HAL_TIM_Base_Start_IT(&htim6);
}

void slope_comp_stop(void)
{
    HAL_TIM_Base_Stop_IT(&htim6);

    /* Zero the comparator threshold — safe state */
    slope_comp_peak_dac_counts = 0u;
    slope_comp_step_counts     = 0u;
    dac3_ch1_write(0u);
}

void slope_comp_reload_dac_peak(void)
{
    /* Read the PID output written by TIM7 ISR and reload DAC3 CH1.
     * This must execute before the blanking window expires (§7.6).
     * slope_comp_peak_dac_counts is 32-bit aligned — read is atomic.      */
    dac3_ch1_write(slope_comp_peak_dac_counts);
}

void slope_comp_update_step(uint32_t v_out_mv, uint32_t v_in_mv, SlopeCompMode mode)
{
    /* Compute the slope (§7.2) using floating-point.
     *
     * Buck:  S_A_per_s = V_out_V / L_H
     * Boost: S_A_per_s = (V_out_V − V_in_V) / L_H   (only if V_out > V_in)
     *
     * step_counts = S_A_per_s × DAC_COUNTS_PER_AMP / TIM6_RATE_HZ
     *
     * L = 4.7 µH = 4.7e-6 H
     * DAC_COUNTS_PER_AMP = 310 counts/A
     * TIM6_RATE_HZ       = 2 000 000 Hz
     */
    const float l_henries       = 4.7e-6f;
    const float dac_counts_per_amp = (float)DAC_COUNTS_PER_AMP;
    const float tim6_rate       = (float)TIM6_RATE_HZ;

    float v_out_volts = (float)v_out_mv * 1e-3f;
    float slope_a_per_s;

    if (mode == SLOPE_COMP_MODE_BUCK)
    {
        slope_a_per_s = v_out_volts / l_henries;
    }
    else /* SLOPE_COMP_MODE_BOOST */
    {
        float v_in_volts = (float)v_in_mv * 1e-3f;
        float v_diff = v_out_volts - v_in_volts;
        /* In boost mode the inductor downslope magnitude = (V_out - V_in) / L.
         * If V_out ≤ V_in (should not happen in boost mode) clamp to 0.      */
        slope_a_per_s = (v_diff > 0.0f) ? (v_diff / l_henries) : 0.0f;
    }

    float step_float = slope_a_per_s * dac_counts_per_amp / tim6_rate;

    /* Clamp to at least 1 so the ramp always decreases (§7.3 note) */
    uint32_t step = (uint32_t)step_float;
    if (step < 1u)
    {
        step = 1u;
    }

    /* Atomic 32-bit write; TIM6 ISR reads this value (§8.6) */
    slope_comp_step_counts = step;
}

void slope_comp_set_peak(uint32_t peak_dac_counts)
{
    /* Clamp to DAC range */
    if (peak_dac_counts > 4095u)
    {
        peak_dac_counts = 4095u;
    }
    /* Atomic 32-bit write */
    slope_comp_peak_dac_counts = peak_dac_counts;
}

/* =========================================================================
 * TIM6 ISR body
 * =========================================================================*/

void slope_comp_tim6_isr(void)
{
    /* Read current DAC3 CH1 value from the output register.
     * NOTE: DAC3->DOR1 is the actual output (after trigger); for a DAC with
     * no trigger (DAC_TRIGGER_NONE, as configured by CubeMX), DHR12R1 is
     * transferred to DOR1 immediately.  We track the logical value in a
     * local shadow rather than reading DOR1 (which may lag by one cycle on
     * some STM32G4 revisions).                                               */

    /* Read current value from the holding register and decrement */
    uint32_t current = DAC3->DHR12R1;

    if (current >= slope_comp_step_counts)
    {
        current -= slope_comp_step_counts;
    }
    else
    {
        current = 0u;   /* floor at 0 — do not wrap */
    }

    DAC3->DHR12R1 = current;
}
