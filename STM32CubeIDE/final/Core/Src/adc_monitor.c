/**
 * @file    adc_monitor.c
 * @brief   ADC measurement subsystem implementation (NLSpec §14).
 *
 * ADC1: 4-channel circular DMA scan (VD_MON, IL_MON, ID_MON, IS_MON).
 * ADC2: Single software-triggered conversion (VS_MON / V_in), triggered
 *       from the TIM7 PID ISR.
 *
 * Scaling formulas (§14.2, Appendix D), voltages with the per-channel
 * bench-calibrated full scales in regulator_config.h:
 *   V_mV  = ADC_raw × VD_MON_FULL_SCALE_MV (or VS_MON_...) / 4096
 *   I_mA (IL, IS) = ADC_raw × 13200 / 4096  ≈ 3.22 mA/count
 *   I_mA (ID)     = ADC_raw × 5500  / 4096  ≈ 1.34 mA/count
 */

#include "adc_monitor.h"
#include "main.h"           /* hadc1, hadc2, hdma_adc1 handles */
#include "stm32g4xx_hal.h"

/* External peripheral handles declared in main.c */
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;

/* =========================================================================
 * Module-level state
 * =========================================================================*/

volatile AdcMeasurements adc_measurements = {0u, 0u, 0u, 0u, 0u};

volatile uint16_t adc1_dma_buffer[ADC1_DMA_BUFFER_LENGTH] = {0u};

/* =========================================================================
 * Internal scaling helpers
 * =========================================================================*/

/**
 * scale_voltage_mv — Scale a 12-bit ADC count to millivolts.
 *
 * @param  raw            ADC count [0, 4095]
 * @param  full_scale_mv  the channel's calibrated full scale
 *                        (VD_MON_FULL_SCALE_MV or VS_MON_FULL_SCALE_MV)
 * @return voltage in millivolts
 *
 * Derive: V_mV = raw × full_scale_mv / 4096  (§14.2, Appendix D)
 */
static inline uint32_t scale_voltage_mv(uint16_t raw, uint32_t full_scale_mv)
{
    return ((uint32_t)raw * full_scale_mv) / ADC_FULL_SCALE_COUNTS;
}

/**
 * scale_current_il_ma — Scale IL_MON or IS_MON count to milliamps.
 *
 * @param  raw  ADC count [0, 4095]
 * @return current in milliamps
 *
 * Derive: I_mA = raw × 13200 / 4096  (§14.2, Appendix D)
 *   5 mΩ shunt, 50 V/V amp → 250 mV/A sensitivity → max 13.2 A at 3.3 V.
 */
static inline uint32_t scale_current_il_ma(uint16_t raw)
{
    return ((uint32_t)raw * ADC_IL_FULL_SCALE_MA) / ADC_FULL_SCALE_COUNTS;
}

/**
 * scale_current_id_ma — Scale ID_MON count to milliamps.
 *
 * @param  raw  ADC count [0, 4095]
 * @return current in milliamps
 *
 * Derive: I_mA = raw × 5500 / 4096  (§14.2, Appendix D)
 *   12 mΩ shunt, 50 V/V amp → 600 mV/A sensitivity → max 5.5 A at 3.3 V.
 */
static inline uint32_t scale_current_id_ma(uint16_t raw)
{
    return ((uint32_t)raw * ADC_ID_FULL_SCALE_MA) / ADC_FULL_SCALE_COUNTS;
}

/* =========================================================================
 * Public API implementations
 * =========================================================================*/

void adc_monitor_init(void)
{
    /* Run factory offset calibration on ADC1 (single-ended mode).
     * Must be called after HAL_ADC_Init() and before HAL_ADC_Start().     */
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
    {
        /* TODO(hardware): decide whether to enter Error_Handler or continue
         * with un-calibrated ADC during bring-up.                           */
    }

    /* Injected group: the current monitors (see adc_monitor.h), software
     * start, 8x oversampled like the regular group (the ratio is shared). */
    {
        static const uint32_t ch[3] = { ADC_CHANNEL_2, ADC_CHANNEL_7, ADC_CHANNEL_15 };
        static const uint32_t rk[3] = { ADC_INJECTED_RANK_1, ADC_INJECTED_RANK_2, ADC_INJECTED_RANK_3 };
        ADC_InjectionConfTypeDef j = {0};
        j.InjectedSamplingTime          = ADC_SAMPLETIME_12CYCLES_5;
        j.InjectedSingleDiff            = ADC_SINGLE_ENDED;
        j.InjectedOffsetNumber          = ADC_OFFSET_NONE;
        j.InjectedNbrOfConversion       = 3u;
        j.InjectedDiscontinuousConvMode = DISABLE;
        j.AutoInjectedConv              = DISABLE;
        j.QueueInjectedContext          = DISABLE;
        j.ExternalTrigInjecConv         = ADC_INJECTED_SOFTWARE_START;
        j.ExternalTrigInjecConvEdge     = ADC_EXTERNALTRIGINJECCONV_EDGE_NONE;
        j.InjecOversamplingMode         = DISABLE;   /* 8x took 14 us and cost 2 regular triggers per PID period */
        j.InjecOversampling.Ratio         = ADC_OVERSAMPLING_RATIO_8;
        j.InjecOversampling.RightBitShift = ADC_RIGHTBITSHIFT_NONE;   /* shared with regular */
        for (uint32_t k = 0u; k < 3u; k++)
        {
            j.InjectedChannel = ch[k];
            j.InjectedRank    = rk[k];
            (void)HAL_ADCEx_InjectedConfigChannel(&hadc1, &j);
        }
    }

    /* Run factory offset calibration on ADC2 */
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK)
    {
        /* TODO(hardware): same as above */
    }
}

uint32_t adc_monitor_latest_scan(void)
{
    /* Next element the DMA writes; the scan holding it is incomplete unless
     * it is the first element of a scan, so the newest complete scan is the
     * one before in either case.                                           */
    uint32_t next = ADC1_DMA_BUFFER_LENGTH - hadc1.DMA_Handle->Instance->CNDTR;
    if (next >= ADC1_DMA_BUFFER_LENGTH) { next = 0u; }
    return (next / ADC1_SCAN_LENGTH + ADC1_DMA_SCANS - 1u) % ADC1_DMA_SCANS;
}

uint16_t adc_monitor_vd_median_raw(uint32_t n)
{
    uint16_t v[15];
    if (n > 15u) { n = 15u; }
    if (n == 0u) { n = 1u; }
    uint32_t scan = adc_monitor_latest_scan();
    for (uint32_t k = 0u; k < n; k++)
    {
        uint16_t x = adc_monitor_vd_at(scan);
        uint32_t j = k;
        while (j > 0u && v[j - 1u] > x) { v[j] = v[j - 1u]; j--; }   /* insertion */
        v[j] = x;
        scan = (scan + ADC1_DMA_SCANS - 1u) % ADC1_DMA_SCANS;
    }
    return v[n / 2u];
}

_Static_assert((ADC1_DMA_SCANS & (ADC1_DMA_SCANS - 1u)) == 0u, "ADC1_DMA_SCANS must be a power of two");

uint16_t adc_monitor_update(uint32_t n)
{
    /* Trimmed mean of the last n VD_MON samples: sum less the highest and
     * lowest.  One pass; a sorted median cost 3.6 % of the CPU here
     * (bu profile, 2026-09-28).                                            */
    if (n > ADC1_AVG_SCANS) { n = ADC1_AVG_SCANS; }
    if (n < 3u) { n = 3u; }
    uint32_t scan = adc_monitor_latest_scan();
    const uint16_t newest = adc_monitor_vd_at(scan);
    uint32_t vsum = 0u, vmin = 0xFFFFu, vmax = 0u;
    for (uint32_t k = 0u; k < n; k++)
    {
        uint32_t x = adc1_dma_buffer[scan * ADC1_SCAN_LENGTH + ADC1_DMA_INDEX_VD_MON];   /* sum of 8 */
        vsum += x;
        if (x < vmin) { vmin = x; }
        if (x > vmax) { vmax = x; }
        scan = (scan - 1u) & (ADC1_DMA_SCANS - 1u);
    }
    adc_measurements.v_out_mv = scale_voltage_mv(newest, VD_MON_FULL_SCALE_MV);

    /* Currents: the injected sequence started one PID period ago (3
     * conversions, about 1.8 us), then start the next.                   */
    if (ADC1->ISR & ADC_ISR_JEOS)
    {
        adc_measurements.i_inductor_ma = scale_current_il_ma((uint16_t)ADC1->JDR1);
        adc_measurements.i_out_ma      = scale_current_id_ma((uint16_t)ADC1->JDR2);
        adc_measurements.i_in_ma       = scale_current_il_ma((uint16_t)ADC1->JDR3);
        ADC1->ISR = ADC_ISR_JEOS | ADC_ISR_JEOC;
    }
#ifndef ADC_INJECTED_CURRENTS
#define ADC_INJECTED_CURRENTS 1
#endif
#if ADC_INJECTED_CURRENTS
    if ((ADC1->CR & ADC_CR_JADSTART) == 0u)
    {
        ADC1->CR |= ADC_CR_JADSTART;
    }
#endif
    return (uint16_t)((vsum - vmin - vmax) / ((n - 2u) << ADC1_OVS_SUM_SHIFT));
}

void adc_monitor_awd_init(void)
{
    /* AWD1 on channel 1 (VD_MON) only, window wide open until regulator
     * code sets one; interrupt left to the caller.                        */
    ADC1->CFGR = (ADC1->CFGR & ~ADC_CFGR_AWD1CH) | ADC_CFGR_AWD1SGL | ADC_CFGR_AWD1EN |
                 (1u << ADC_CFGR_AWD1CH_Pos);
    ADC1->TR1  = 0u | (0xFFFu << 16);   /* AWDFILT 0: see adc_monitor.h */
    ADC1->ISR  = ADC_ISR_AWD1;
}

void adc_monitor_vd_recent(uint16_t *out, uint32_t n)
{
    uint32_t next = ADC1_DMA_BUFFER_LENGTH - hadc1.DMA_Handle->Instance->CNDTR;
    if (next >= ADC1_DMA_BUFFER_LENGTH) { next = 0u; }
    /* the scan holding the element before `next` has its VD_MON written */
    uint32_t scan = ((next + ADC1_DMA_BUFFER_LENGTH - 1u) % ADC1_DMA_BUFFER_LENGTH) / ADC1_SCAN_LENGTH;
    for (uint32_t k = 0u; k < n && k < 8u; k++)
    {
        out[k] = adc_monitor_vd_at(scan);
        scan = (scan + ADC1_DMA_SCANS - 1u) % ADC1_DMA_SCANS;
    }
}

void adc_monitor_start_adc1_dma(void)
{
    /* Start ADC1 once: each HRTIM ADC trigger 1 (once per switching period,
     * set up in regulator.c) converts one scan, and DMAContinuousRequests +
     * circular DMA (set in the IOC) fill the adc1_dma_buffer ring with no
     * CPU involvement.
     *
     * HAL_ADC_Start_DMA enables the DMA half/full-transfer and ADC overrun
     * interrupts; at this conversion rate they would fire back to back and
     * starve FreeRTOS (DMA1_Channel5 is above SysTick), so turn them off.
     * Consumers call adc_monitor_scale_adc1_buffer() when they need values. */
    HAL_ADC_Start_DMA(&hadc1,
                      (uint32_t *)(void *)adc1_dma_buffer,
                      ADC1_DMA_BUFFER_LENGTH);
    __HAL_DMA_DISABLE_IT(hadc1.DMA_Handle, DMA_IT_TC | DMA_IT_HT);
    __HAL_ADC_DISABLE_IT(&hadc1, ADC_IT_OVR);
}

void adc_monitor_trigger_vin(void)
{
    /* Start a single software-triggered ADC2 conversion.
     * Non-blocking: conversion runs asynchronously.  Read the result with
     * adc_monitor_read_vin_result() after the estimated conversion time.
     * Register-level once ADC2 is enabled: HAL_ADC_Start at -O0 cost
     * several microseconds of the PID ISR (2026-09-28 evening).           */
    if ((ADC2->CR & ADC_CR_ADEN) == 0u)
    {
        HAL_ADC_Start(&hadc2);   /* first call enables the ADC */
        return;
    }
    ADC2->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;   /* write-1-to-clear */
    ADC2->CR |= ADC_CR_ADSTART;
}

void adc_monitor_read_vin_result(void)
{
    /* Poll for conversion complete (with a short timeout).
     * At 42.5 MHz ADC clock, 2.5 + 12.5 = 15 cycles ≈ 354 ns.
     * We wait up to 1 ms; in practice the conversion is done in < 1 µs.    */
    /* A bounded spin, not HAL_ADC_PollForConversion: its 1 ms timeout reads
     * HAL_GetTick(), which cannot advance inside this ISR (the HAL timebase
     * interrupt is lower priority).  A missed conversion keeps the last
     * V_in.                                                               */
    for (uint32_t spin = 0u; spin < 2000u; spin++)
    {
        if (ADC2->ISR & ADC_ISR_EOC)
        {
            uint16_t raw = (uint16_t)ADC2->DR;   /* clears EOC */
            adc_measurements.v_in_mv = scale_voltage_mv(raw, VS_MON_FULL_SCALE_MV);
            break;
        }
    }
}

void adc_monitor_scale_adc1_buffer(void)
{
    /* V_out: the newest regular result (telemetry; the regulator uses the
     * trimmed mean from adc_monitor_update).  Currents: the last injected
     * results, which the PID ISR refreshes (adc_monitor_update).          */
    uint32_t scan = adc_monitor_latest_scan();
    adc_measurements.v_out_mv = scale_voltage_mv(adc_monitor_vd_at(scan), VD_MON_FULL_SCALE_MV);
    adc_measurements.i_inductor_ma = scale_current_il_ma((uint16_t)ADC1->JDR1);
    adc_measurements.i_out_ma      = scale_current_id_ma((uint16_t)ADC1->JDR2);
    adc_measurements.i_in_ma       = scale_current_il_ma((uint16_t)ADC1->JDR3);
}

/* =========================================================================
 * HAL ADC callback — called from DMA ISR when ADC1 scan completes
 * =========================================================================*/

/**
 * HAL_ADC_ConvCpltCallback — DMA transfer-complete callback for ADC1.
 *
 * Normally not called: adc_monitor_start_adc1_dma() disables the DMA
 * interrupts (ADC1 free-runs into a circular buffer).  Kept so that, if the
 * interrupt is ever re-enabled, it only scales and never restarts DMA.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        adc_monitor_scale_adc1_buffer();
    }
}
