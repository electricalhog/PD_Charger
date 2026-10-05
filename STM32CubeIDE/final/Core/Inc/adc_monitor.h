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
#include "stm32g4xx.h"   /* ADC1 registers for adc_monitor_awd_window */

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

/** ADC1 regular group: VD_MON alone, 8x hardware oversampled (6.5-cycle
 *  samples, 3.6 us of the period averaged per result), once per switching
 *  period on HRTIM ADC trigger 1 (master compare 1, see regulator.c), into
 *  a circular DMA ring of ADC1_DMA_SCANS results: 64 periods, 320 us, which
 *  doubles as a per-period trace (bu mem read adc1_dma_buffer).
 *  Why oversampling (2026-09-28 evening): single VD_MON samples read about
 *  1.6 V (sd) of noise that the scope does not see on V_out (TP2 has nothing
 *  above 20 mV past 220 kHz) and that a 1 nF on the divider did not change,
 *  so it enters after the divider; it beat at 40 kHz against the 200 kHz
 *  sampling.
 *  ADC1 injected group: IL_MON, ID_MON, IS_MON, not oversampled, started by
 *  software from the PID ISR (adc_monitor_update), configured in
 *  adc_monitor_init (not in final.ioc).                                  */
#define ADC1_SCAN_LENGTH                1u
#define ADC1_DMA_SCANS                  64u
#define ADC1_DMA_BUFFER_LENGTH          (ADC1_SCAN_LENGTH * ADC1_DMA_SCANS)
/** VD_MON samples the trimmed mean may use (one PID period at 20 kHz). */
#define ADC1_AVG_SCANS                  10u

/** DMA ring index of VD_MON within a scan (the only regular channel) */
#define ADC1_DMA_INDEX_VD_MON           0u  /**< Regular rank 1: V_out */

/** Results (DMA ring, JDR1..3) are the sum of 8 conversions, no shift: with
 *  oversampling the analog watchdog compares data bits [15:4] (the LL driver
 *  note on LL_ADC_ConfigAnalogWDThresholds), so a 12-bit shifted result made
 *  it see V_out / 16 and it never fired (bench, 2026-09-28).  Unshifted, it
 *  sees the 12-bit value / 2: a 2-count (38 mV) threshold step.           */
#define ADC1_OVS_SUM_SHIFT              3u  /**< sum -> 12-bit value */
#define ADC1_AWD_THRESHOLD_SHIFT        1u  /**< 12-bit value -> TR1 field */

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

/** Raw ADC1 DMA ring (12-bit counts, ADC1_SCAN_LENGTH per scan). */
extern volatile uint16_t adc1_dma_buffer[ADC1_DMA_BUFFER_LENGTH];

/** Index of the newest complete scan in adc1_dma_buffer (from the DMA's
 *  remaining count). */
uint32_t adc_monitor_latest_scan(void);

/** Median of the last n (<= 15) VD_MON samples, raw counts.  One sample per
 *  switching period, so a single bad sample never reaches the result.    */
uint16_t adc_monitor_vd_median_raw(uint32_t n);

/** One pass over the ring for the PID ISR: updates adc_measurements' ADC1
 *  fields (as adc_monitor_scale_adc1_buffer) and returns the mean of the
 *  last n (3 to ADC1_AVG_SCANS) VD_MON samples less the highest and lowest,
 *  in raw counts.                                                         */
uint16_t adc_monitor_update(uint32_t n);

/** ADC1 analog watchdog 1 on VD_MON (single channel), unfiltered.  The
 *  hardware filter (ADC_TR1.AWDFILT) never fired with VD_MON in a 4-channel
 *  scan (bench, 2026-09-28: flag set at once with AWDFILT 0, never with 1):
 *  it appears to count consecutive conversions of the sequence, not of the
 *  watched channel.  Confirm in the ISR with adc_monitor_vd_recent instead.
 *  Call before adc_monitor_start_adc1_dma (CFGR is written with ADSTART=0). */
void adc_monitor_awd_init(void);

/** The newest n (<= 8) VD_MON samples, newest first, including the scan in
 *  progress (VD_MON is its first conversion).                            */
void adc_monitor_vd_recent(uint16_t *out, uint32_t n);

/** Set the watchdog window [lt, ht] in raw counts; allowed while converting
 *  (checked on the bench, 2026-09-28). */
static inline void adc_monitor_awd_window(uint32_t lt, uint32_t ht)
{
    lt >>= ADC1_AWD_THRESHOLD_SHIFT;
    ht >>= ADC1_AWD_THRESHOLD_SHIFT;
    ADC1->TR1 = (lt & 0xFFFu) | (ADC1->TR1 & ADC_TR1_AWDFILT) | ((ht & 0xFFFu) << 16);
}

/** 12-bit VD_MON value of ring slot `scan`. */
static inline uint16_t adc_monitor_vd_at(uint32_t scan)
{
    return (uint16_t)(adc1_dma_buffer[scan * ADC1_SCAN_LENGTH + ADC1_DMA_INDEX_VD_MON] >> ADC1_OVS_SUM_SHIFT);
}

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
