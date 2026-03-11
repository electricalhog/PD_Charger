/**
 * @file    adc_monitor.c
 * @brief   ADC measurement subsystem implementation (NLSpec §14).
 *
 * ADC1: 4-channel circular DMA scan (VD_MON, IL_MON, ID_MON, IS_MON).
 * ADC2: Single software-triggered conversion (VS_MON / V_in), triggered
 *       from the TIM7 PID ISR.
 *
 * Scaling formulas (§14.2, Appendix D):
 *   V_mV  = ADC_raw × 60000 / 4096  ≈ 14.65 mV/count
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
 * @param  raw  ADC count [0, 4095]
 * @return voltage in millivolts
 *
 * Derive: V_mV = raw × 60000 / 4096  (§14.2, Appendix D)
 *   Full-scale: 3.3 V ADC, 100k/5.82k divider → 60 V physical.
 */
static inline uint32_t scale_voltage_mv(uint16_t raw)
{
    return ((uint32_t)raw * ADC_VOLTAGE_FULL_SCALE_MV) / ADC_FULL_SCALE_COUNTS;
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

    /* Run factory offset calibration on ADC2 */
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK)
    {
        /* TODO(hardware): same as above */
    }
}

void adc_monitor_start_adc1_dma(void)
{
    /* Start ADC1 in DMA circular mode.
     * The MX_ADC1_Init() configures 4 channels; ContinuousConvMode =
     * DISABLE and DMAContinuousRequests = DISABLE.  We restart the DMA on
     * each conversion complete in HAL_ADC_ConvCpltCallback.
     *
     * TODO(hardware): For production, consider setting ContinuousConvMode
     * and DMAContinuousRequests = ENABLE in a post-init re-call of
     * HAL_ADC_Init() to avoid re-triggering overhead in the callback.
     */
    HAL_ADC_Start_DMA(&hadc1,
                      (uint32_t *)(void *)adc1_dma_buffer,
                      ADC1_DMA_BUFFER_LENGTH);
}

void adc_monitor_trigger_vin(void)
{
    /* Start a single software-triggered ADC2 conversion.
     * Non-blocking: conversion runs asynchronously.  Read the result with
     * adc_monitor_read_vin_result() after the estimated conversion time.   */
    HAL_ADC_Start(&hadc2);
}

void adc_monitor_read_vin_result(void)
{
    /* Poll for conversion complete (with a short timeout).
     * At 42.5 MHz ADC clock, 2.5 + 12.5 = 15 cycles ≈ 354 ns.
     * We wait up to 1 ms; in practice the conversion is done in < 1 µs.    */
    if (HAL_ADC_PollForConversion(&hadc2, 1u) == HAL_OK)
    {
        uint16_t raw = (uint16_t)HAL_ADC_GetValue(&hadc2);
        adc_measurements.v_in_mv = scale_voltage_mv(raw);
    }
}

void adc_monitor_scale_adc1_buffer(void)
{
    adc_measurements.v_out_mv      = scale_voltage_mv(adc1_dma_buffer[ADC1_DMA_INDEX_VD_MON]);
    adc_measurements.i_inductor_ma = scale_current_il_ma(adc1_dma_buffer[ADC1_DMA_INDEX_IL_MON]);
    adc_measurements.i_out_ma      = scale_current_id_ma(adc1_dma_buffer[ADC1_DMA_INDEX_ID_MON]);
    adc_measurements.i_in_ma       = scale_current_il_ma(adc1_dma_buffer[ADC1_DMA_INDEX_IS_MON]);
}

/* =========================================================================
 * HAL ADC callback — called from DMA ISR when ADC1 scan completes
 * =========================================================================*/

/**
 * HAL_ADC_ConvCpltCallback — DMA transfer-complete callback for ADC1.
 *
 * Called by HAL from DMA1_Channel5_IRQHandler when the ADC1 scan DMA
 * buffer is fully written.  Scales the raw counts and restarts DMA.
 *
 * NOTE: This overrides the weak HAL definition.  It must not call any
 * FreeRTOS API (ISR priority 2 is below FreeRTOS mask 5).
 * Actually, DMA1_Channel5 is configured at priority 5 in MX_DMA_Init
 * (RTOS-managed).  Therefore FromISR variants could be used here if needed,
 * but we deliberately avoid any FreeRTOS dependency in the regulator path.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        adc_monitor_scale_adc1_buffer();

        /* Restart DMA for the next ADC1 scan cycle */
        HAL_ADC_Start_DMA(&hadc1,
                          (uint32_t *)(void *)adc1_dma_buffer,
                          ADC1_DMA_BUFFER_LENGTH);
    }
}
