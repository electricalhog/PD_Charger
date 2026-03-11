/**
 * @file    slope_comp.h
 * @brief   Slope compensation module for peak current mode control (NLSpec §7).
 *
 * Implements a TIM6-based staircase ramp on DAC3 CH1 that decreases the
 * comparator threshold linearly within each switching period.  This prevents
 * subharmonic oscillation at duty cycles above 50% (§7.1).
 *
 * Signal chain (§7.4):
 *   PID output (DAC counts) → DAC3 CH1 peak value (Y-intercept)
 *   TIM6 ISR fires every 500 ns → subtracts step_counts from DAC3 CH1
 *   HRTIM period reset → DAC3 CH1 reloaded with current PID peak value
 *
 * Ramp mechanism: "Timer ISR staircase" (§7.5 option 1).
 *   - TIM6 fires at 2 MHz (10× per switching period at 200 kHz).
 *   - Each ISR decrements DAC3 CH1 by a precomputed integer step.
 *   - Step is recomputed from V_out / L (buck) or (V_out−V_in) / L (boost)
 *     on each PID cycle; the ISR only reads the precomputed value (§7.2).
 *
 * ISR ownership and priority (§13.2, Appendix G):
 *   TIM6_DAC_IRQHandler — priority 0 (most latency-sensitive).
 *
 * NLSpec conformance: v0.1.2j §7
 */

#ifndef SLOPE_COMP_H
#define SLOPE_COMP_H

#include <stdint.h>
#include "regulator_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Operating mode type (also used by regulator.h; duplicated here to avoid
 * a circular dependency — slope_comp.h does not include regulator.h).
 * =========================================================================*/

/**
 * SlopeCompMode — operating mode selector for slope step calculation.
 * Matches RegulatorMode in regulator.h but does not depend on it.
 */
typedef enum
{
    SLOPE_COMP_MODE_BUCK  = 0,  /**< Buck: downslope = V_out / L */
    SLOPE_COMP_MODE_BOOST = 1   /**< Boost: downslope = (V_out − V_in) / L */
} SlopeCompMode;

/* =========================================================================
 * Shared state (written by PID ISR, read by TIM6 ISR)
 * =========================================================================*/

/**
 * slope_comp_step_counts — precomputed DAC decrement per TIM6 tick.
 *
 * Volatile because it is written from TIM7 ISR (PID context) and read
 * from TIM6 ISR (slope comp context).  A 32-bit aligned write on Cortex-M4
 * is atomic; no mutex is required (§8.6).
 *
 * A value of 0 means slope compensation is not yet active (regulator not
 * running or mode not configured).
 */
extern volatile uint32_t slope_comp_step_counts;

/**
 * slope_comp_peak_dac_counts — current PID output (DAC Y-intercept).
 *
 * Written by the TIM7 PID ISR after each PID computation.
 * Read by the HRTIM period ISR to reload DAC3 CH1 at each period start.
 * Volatile for the same reason as slope_comp_step_counts.
 */
extern volatile uint32_t slope_comp_peak_dac_counts;

/* =========================================================================
 * API
 * =========================================================================*/

/**
 * slope_comp_init — Configure TIM6 for 2 MHz slope compensation.
 *
 * Writes TIM6 ARR and PSC registers to achieve 2 MHz interrupt rate (§7.3),
 * configures NVIC priority to NVIC_PRIORITY_SLOPE_COMP_TIM6 (0), and
 * enables the TIM6 interrupt.  Does NOT start TIM6 — call slope_comp_start()
 * when the regulator transitions to RUNNING.
 *
 * Must be called from regulator_init() after all MX_*_Init() functions.
 */
void slope_comp_init(void);

/**
 * slope_comp_start — Load the DAC peak value and start TIM6.
 *
 * Writes slope_comp_peak_dac_counts to DAC3 CH1 immediately (so the
 * comparator has a valid threshold from the first switching period), then
 * starts TIM6.
 *
 * @param initial_peak_counts  Initial DAC peak value (PID output in DAC counts).
 */
void slope_comp_start(uint32_t initial_peak_counts);

/**
 * slope_comp_stop — Stop TIM6 and hold DAC3 CH1 at zero.
 *
 * Called on FAULT or when the regulator transitions to IDLE.  Setting DAC3
 * CH1 to 0 means COMP1 threshold = 0 V → any inductor current will trip
 * the comparator → safe state for the switching network.
 */
void slope_comp_stop(void);

/**
 * slope_comp_reload_dac_peak — Reload DAC3 CH1 with the current PID peak.
 *
 * Called from the HRTIM period ISR at the start of each switching period
 * (§7.6).  Must execute before the blanking window expires.
 *
 * Reads slope_comp_peak_dac_counts (written by TIM7) and writes it directly
 * to the DAC3 CH1 data holding register.
 */
void slope_comp_reload_dac_peak(void);

/**
 * slope_comp_update_step — Recompute the DAC decrement step for the current
 * operating point.
 *
 * Called from the TIM7 PID ISR after each PID cycle.  Uses floating-point
 * arithmetic to compute the slope (§7.2) and stores the integer floor in
 * slope_comp_step_counts for the TIM6 ISR.
 *
 * The formula (§7.2):
 *   Buck:  S = V_out_V / L_H × DAC_COUNTS_PER_AMP / TIM6_RATE_HZ
 *   Boost: S = (V_out_V − V_in_V) / L_H × DAC_COUNTS_PER_AMP / TIM6_RATE_HZ
 *
 * A minimum step of 1 is enforced so the ramp always moves.
 *
 * @param v_out_mv  Measured output voltage in millivolts.
 * @param v_in_mv   Measured input voltage in millivolts.
 * @param mode      Current operating mode (buck or boost).
 */
void slope_comp_update_step(uint32_t v_out_mv, uint32_t v_in_mv, SlopeCompMode mode);

/**
 * slope_comp_set_peak — Update the DAC Y-intercept for the next period.
 *
 * Called by the TIM7 PID ISR after computing the new PID output.  Stores
 * the new peak value so the HRTIM period ISR can reload it.
 *
 * @param peak_dac_counts  New PID output in DAC counts [0, PID_OUTPUT_MAX].
 */
void slope_comp_set_peak(uint32_t peak_dac_counts);

/* =========================================================================
 * ISR handler declaration (defined in slope_comp.c; referenced from
 * stm32g4xx_it.c which calls it from TIM6_DAC_IRQHandler).
 * =========================================================================*/

/**
 * slope_comp_tim6_isr — TIM6 period-elapsed ISR body.
 *
 * Decrements DAC3 CH1 by slope_comp_step_counts, clamping at 0.
 * Must not recompute the step — that is done in slope_comp_update_step()
 * from the lower-priority TIM7 ISR (§7.5).
 *
 * Called from TIM6_DAC_IRQHandler in stm32g4xx_it.c.
 */
void slope_comp_tim6_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* SLOPE_COMP_H */
