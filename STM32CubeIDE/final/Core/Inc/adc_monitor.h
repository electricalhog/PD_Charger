/**
 * @file    adc_monitor.h
 * @brief   ADC measurement subsystem — scaling and DMA buffer management (NLSpec §14).
 *
 * Provides scaled physical-unit measurements from the five analog channels:
 *   - VD_MON (PA0 / ADC1_IN1)  — V_out [mV]
 *   - IL_MON (PA1 / ADC1_IN2)  — I_inductor [mA]
 *   - ID_MON (PC1 / ADC1_IN7)  — I_out [mA]
 *   - IS_MON (PB0 / ADC1_IN15) — I_in [mA]
 *   - VS_MON (PA4 / ADC2_IN17) — V_in [mV]
 *
 * ADC1 channels are read via DMA into a 4-element circular buffer.
 * ADC2 (VS_MON) is triggered by the PID ISR as a single software-triggered
 * conversion.
 *
 * All scaling constants are derived from the hardware parameters in
 * regulator_config.h (§3.3, §3.4, §14.2).
 *
 * NLSpec conformance: v0.1.2j §14
 */

#ifndef ADC_MONITOR_H
#define ADC_MONITOR_H

#include <stdint.h>
#include "regulator_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * ADC DMA buffer layout for ADC1 (4-channel scan, circular DMA)
 *
 * ADC1 regular sequence (4 channels, configured in MX_ADC1_Init):
 *   Rank 1: ADC1_IN1  (PA0, VD_MON)
 *   Rank 2: ADC1_IN2  (PA1, IL_MON)
 *   Rank 3: ADC1_IN7  (PC1, ID_MON)
 *   Rank 4: ADC1_IN15 (PB0, IS_MON)
 * =========================================================================*/

/** Number of ADC1 channels in the regular scan sequence. */
#define ADC1_DMA_BUFFER_LENGTH          4u

/** DMA buffer index for each ADC1 channel */
#define ADC1_DMA_INDEX_VD_MON           0u  /**< Rank 1: V_out */
#define ADC1_DMA_INDEX_IL_MON           1u  /**< Rank 2: I_inductor */
#define ADC1_DMA_INDEX_ID_MON           2u  /**< Rank 3: I_out */
#define ADC1_DMA_INDEX_IS_MON           3u  /**< Rank 4: I_in */

/* =========================================================================
 * Measurement result structure
 * =========================================================================*/

/**
 * AdcMeasurements — latest scaled measurements from all five channels.
 *
 * Updated atomically from the DMA complete callback (ADC1) and from
 * adc_monitor_trigger_vin_and_wait() (ADC2).
 *
 * All values are in millivolts or milliamps (as labelled).  A value of 0
 * indicates the channel has not yet been sampled (e.g., at startup).
 */
typedef struct
{
    uint32_t v_out_mv;       /**< V_out measured via VD_MON divider [mV] */
    uint32_t v_in_mv;        /**< V_in measured via VS_MON divider [mV]  */
    uint32_t i_inductor_ma;  /**< Inductor peak current via IL_MON [mA]  */
    uint32_t i_out_ma;       /**< Output current via ID_MON [mA]         */
    uint32_t i_in_ma;        /**< Input current via IS_MON [mA]          */
} AdcMeasurements;

/* =========================================================================
 * Shared measurement state
 * =========================================================================*/

/**
 * adc_measurements — latest scaled measurement results.
 *
 * Written from HAL_ADC_ConvCpltCallback (ADC1, DMA completion, priority 2)
 * and from the TIM7 PID ISR (ADC2 trigger+read, priority 2).
 * Read from any context.
 *
 * All fields are uint32_t; 32-bit aligned writes on Cortex-M4 are atomic.
 * No mutex is required for single-writer/multiple-reader access (§9.1).
 */
extern volatile AdcMeasurements adc_measurements;

/** Raw ADC1 DMA buffer (12-bit unsigned counts for each channel). */
extern volatile uint16_t adc1_dma_buffer[ADC1_DMA_BUFFER_LENGTH];

/* =========================================================================
 * API
 * =========================================================================*/

/**
 * adc_monitor_init — Calibrate ADCs and prepare DMA buffer.
 *
 * Runs HAL offset calibration on ADC1 and ADC2.  Must be called from
 * regulator_init() after MX_ADC1_Init() and MX_ADC2_Init().
 *
 * Calibration takes a few microseconds; must not be called from an ISR.
 */
void adc_monitor_init(void);

/**
 * adc_monitor_start_adc1_dma — Start ADC1 circular DMA conversion.
 *
 * Initiates a continuous DMA-driven scan of all four ADC1 channels.
 * On each scan completion, HAL_ADC_ConvCpltCallback fires, scales the
 * raw counts, and updates adc_measurements.
 *
 * Must be called from regulator_init() after adc_monitor_init().
 */
void adc_monitor_start_adc1_dma(void);

/**
 * adc_monitor_trigger_vin — Trigger a single ADC2 conversion for V_in.
 *
 * Non-blocking: starts the software-triggered ADC2 conversion.  The result
 * is read by adc_monitor_read_vin_result() once the conversion completes.
 *
 * Called from the TIM7 PID ISR at the start of each PID cycle.
 *
 * TODO(hardware): For better accuracy, consider HRTIM-triggered injected
 *                 conversion synchronised to the switching period midpoint
 *                 (§8.5 recommendation).
 */
void adc_monitor_trigger_vin(void);

/**
 * adc_monitor_read_vin_result — Read the ADC2 result and update v_in_mv.
 *
 * Reads the ADC2 data register directly (no HAL overhead) and scales the
 * raw count to millivolts.  Must be called after adc_monitor_trigger_vin()
 * and after the conversion complete flag is set.
 *
 * In practice, at 170 MHz SYSCLK with ADC clocked at PCLK/4 = 42.5 MHz and
 * 2.5-cycle sampling time, the total conversion time ≈ 330 ns.  The PID ISR
 * may call trigger at the start of its execution and read at the end (~3 µs
 * later) — conversion will be complete by then.
 */
void adc_monitor_read_vin_result(void);

/**
 * adc_monitor_scale_adc1_buffer — Scale the raw DMA buffer to physical units.
 *
 * Called from HAL_ADC_ConvCpltCallback.  Reads adc1_dma_buffer and updates
 * adc_measurements.v_out_mv, i_inductor_ma, i_out_ma, i_in_ma.
 *
 * May also be called directly from the PID ISR for a synchronous read.
 */
void adc_monitor_scale_adc1_buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_MONITOR_H */
