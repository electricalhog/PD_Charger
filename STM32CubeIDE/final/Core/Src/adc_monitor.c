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

    /* Run factory offset calibration on ADC2 */
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK)
    {
        /* TODO(hardware): same as above */
    }
}

void adc_monitor_start_adc1_dma(void)
{
    /* Start ADC1 once, free-running: ContinuousConvMode + DMAContinuous
     * Requests + circular DMA (set in the IOC) keep adc1_dma_buffer holding
     * the latest scan with no CPU involvement.
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
        adc_measurements.v_in_mv = scale_voltage_mv(raw, VS_MON_FULL_SCALE_MV);
    }
}

void adc_monitor_scale_adc1_buffer(void)
{
    adc_measurements.v_out_mv      = scale_voltage_mv(adc1_dma_buffer[ADC1_DMA_INDEX_VD_MON],
                                                      VD_MON_FULL_SCALE_MV);
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
