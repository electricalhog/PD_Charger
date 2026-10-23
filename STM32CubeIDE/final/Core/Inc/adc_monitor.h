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
 * Both ADCs scan once per switching period on HRTIM ADC trigger 1 into
 * circular DMA rings (see "Timer-synchronized scans" below).  IL_MON is
 * not converted.
 *
 * All scaling constants are derived from the hardware parameters in
 * regulator_config.h (§3.3, §3.4, §14.2).
 *
 * NLSpec conformance: v0.1.2j §14
 */

#ifndef ADC_MONITOR_H
#define ADC_MONITOR_H

#include <stdbool.h>
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

/** Timer-synchronized scans (2026-10-22, plans/ccm-buck-50w.md section 9).
 *  HRTIM ADC trigger 1 (Timer C CMP4, once per switching period, placed by
 *  regulator.c in the quiet part of the period) starts both ADCs together:
 *    ADC1 regular [VD_MON (V_out), IS_MON (I_in)]
 *    ADC2 regular [VS_MON (V_in),  ID_MON (I_out)]
 *  one conversion per rank (no hardware oversampling), each scan into a
 *  circular DMA ring.  The ADC1 ring (64 periods) doubles as a per-period
 *  V_out trace (bu mem read adc1_dma_buffer).
 *  Why VD_MON was oversampled until now (2026-09-28): single samples read
 *  about 1.6 V sd that the scope did not see.  Those were 2.5-cycle samples
 *  from the 5.5 kohm divider at a random phase, right after other channels
 *  in a 4-channel scan: unsettled charge, not divider noise.  24.5 cycles
 *  at a fixed quiet phase is the test of that. */
#define ADC_SYNC_SCAN_LENGTH            2u
#define ADC_SYNC_V_SAMPLING             LL_ADC_SAMPLINGTIME_24CYCLES_5   /**< VD_MON, VS_MON: 576 ns */
#define ADC_SYNC_I_SAMPLING             LL_ADC_SAMPLINGTIME_6CYCLES_5    /**< IS_MON, ID_MON (amplifier outputs) */
/** Scan timing at 42.5 MHz (PCLK/4): trigger to the start of rank 1's
 *  sampling, rank 1's sampling window, and the whole scan (37 + 19 cycles). */
#define ADC_SYNC_LATENCY_NS             50u
#define ADC_SYNC_V_SAMPLE_NS            576u
#define ADC_SYNC_SCAN_NS                1318u
/** Scans averaged for V_in, I_out and I_in (adc_monitor_update_slow). */
#define ADC_SYNC_AVG_SCANS              8u

#define ADC1_SCAN_LENGTH                ADC_SYNC_SCAN_LENGTH
#define ADC1_DMA_SCANS                  64u
#define ADC1_DMA_BUFFER_LENGTH          (ADC1_SCAN_LENGTH * ADC1_DMA_SCANS)
#define ADC2_DMA_SCANS                  32u
#define ADC2_DMA_BUFFER_LENGTH          (ADC_SYNC_SCAN_LENGTH * ADC2_DMA_SCANS)
/** VD_MON samples the trimmed mean may use. */
#define ADC1_AVG_SCANS                  20u

/** DMA ring index of VD_MON within a scan */
#define ADC1_DMA_INDEX_VD_MON           0u  /**< Regular rank 1: V_out */

/** The analog watchdog compares the 12-bit result directly (no
 *  oversampling).  With 8x oversampling until 2026-10-22 it saw data bits
 *  [15:4] of the unshifted sum (LL_ADC_ConfigAnalogWDThresholds note). */
#define ADC1_AWD_THRESHOLD_SHIFT        0u  /**< 12-bit value -> TR1 field */

/* =========================================================================
 * Measurement result structure
 * =========================================================================*/

/**
 * AdcMeasurements — latest scaled measurements from all five channels.
 *
 * Updated by adc_monitor_update / adc_monitor_update_slow from the
 * synchronized DMA rings (the PID ISR, and regulator_start).
 *
 * All values are in millivolts or milliamps (as labelled).  A value of 0
 * indicates the channel has not yet been sampled (e.g., at startup).
 */
typedef struct
{
    uint32_t v_out_mv;       /**< V_out measured via VD_MON divider [mV] */
    uint32_t v_in_mv;        /**< V_in measured via VS_MON divider [mV]  */
    uint32_t i_inductor_ma;  /**< 0: IL_MON is not converted since 2026-10-22 [mA]   */
    uint32_t i_out_ma;       /**< ID_MON, mean of ADC_SYNC_AVG_SCANS synchronized [mA] */
    uint32_t i_in_ma;        /**< IS_MON, the same [mA]                               */
} AdcMeasurements;

/* =========================================================================
 * Shared measurement state
 * =========================================================================*/

/**
 * adc_measurements — latest scaled measurement results.
 *
 * Written from HAL_ADC_ConvCpltCallback (ADC1, DMA completion, priority 2)
 * and from the TIM7 PID ISR (ADC2 poll and trigger, priority 2).
 * Read from any context.
 *
 * All fields are uint32_t; 32-bit aligned writes on Cortex-M4 are atomic.
 * No mutex is required for single-writer/multiple-reader access (§9.1).
 */
extern volatile AdcMeasurements adc_measurements;

/** Raw DMA rings (12-bit counts, ADC_SYNC_SCAN_LENGTH per scan). */
extern volatile uint16_t adc2_dma_buffer[ADC2_DMA_BUFFER_LENGTH];
/** Index of the newest complete scan in adc2_dma_buffer. */
uint32_t adc_monitor_latest_scan2(void);
/** V_in (trimmed mean), I_out and I_in from the last ADC_SYNC_AVG_SCANS
 *  synchronized scans into adc_measurements (adc_monitor_update calls it). */
void adc_monitor_update_slow(void);
/** ADC1's ring. */
extern volatile uint16_t adc1_dma_buffer[ADC1_DMA_BUFFER_LENGTH];

/** Index of the newest complete scan in adc1_dma_buffer (from the DMA's
 *  remaining count). */
uint32_t adc_monitor_latest_scan(void);

/** Median of the last n (<= 15) VD_MON samples, raw counts (the V_out
 *  estimate since 2026-10-22, adc_monitor_update).  One sample per
 *  switching period, so a single bad sample never reaches the result.    */
uint16_t adc_monitor_vd_median_raw(uint32_t n);

/** For the PID ISR: updates adc_measurements (V_out newest sample, V_in and
 *  the currents from the rings) and returns the median of the last n (<= 15)
 *  VD_MON samples, in raw counts.                                         */
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
    return adc1_dma_buffer[scan * ADC1_SCAN_LENGTH + ADC1_DMA_INDEX_VD_MON];
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
