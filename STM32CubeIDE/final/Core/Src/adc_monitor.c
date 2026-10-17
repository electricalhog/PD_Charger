/**
 * @file    adc_monitor.c
 * @brief   ADC measurement subsystem implementation (NLSpec §14).
 *
 * ADC1: 4-channel circular DMA scan (VD_MON, IL_MON, ID_MON, IS_MON).
 * ADC2: One software-triggered conversion (VS_MON / V_in), 16x
 *       oversampled, started by the TIM7 PID ISR and read the next cycle.
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
        /* The HAL sets the channels' sampling times above but writes JSQR
         * for rank 1 only while ScanConvMode is ADC_SCAN_DISABLE (the regular
         * group is VD_MON alone; stm32g4xx_hal_adc_ex.c), so JSQR held IL_MON
         * alone and JDR2/JDR3 (ID_MON, IS_MON) read 0 until 2026-10-16.  The
         * three ranks, software start (JEXTEN 0), written while JADSTART = 0. */
        ADC1->JSQR = ((3u - 1u) << ADC_JSQR_JL_Pos)
                   | (2u  << ADC_JSQR_JSQ1_Pos)
                   | (7u  << ADC_JSQR_JSQ2_Pos)
                   | (15u << ADC_JSQR_JSQ3_Pos);
    }

    /* ADC2 (VS_MON, V_in): 16 conversions of 24.5 + 12.5 ADC cycles
     * (0.87 us each at 42.5 MHz) averaged in hardware per trigger, 13.9 us.
     * 0.87 us is 0.174 of the 5 us switching period, so the 16 samples fall
     * at 16 different points of it over 2.8 periods.  Until 2026-10-12 V_in
     * was one 2.5-cycle (59 ns) sample (MX_ADC2_Init) wherever in the period
     * the PID ISR started: VS_MON is a 100k/5.82k divider with no capacitor,
     * wired beside PHASE_2_P/N to the MCU, and at 28 V boost with 3 W 0.2 %
     * of cycles read 1 to 5 V off (19.1 to 27.6 V around 24.4 V).  Written
     * here, with ADC2 not yet enabled.                                     */
    LL_ADC_SetChannelSamplingTime(ADC2, LL_ADC_CHANNEL_17, LL_ADC_SAMPLINGTIME_24CYCLES_5);
    LL_ADC_ConfigOverSamplingRatioShift(ADC2, LL_ADC_OVS_RATIO_16, LL_ADC_OVS_SHIFT_RIGHT_4);
    LL_ADC_SetOverSamplingDiscont(ADC2, LL_ADC_OVS_REG_CONT);
    LL_ADC_SetOverSamplingScope(ADC2, LL_ADC_OVS_GRP_REGULAR_CONTINUED);

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
    /* Start one software-triggered ADC2 conversion (16 oversampled, 13.9 us,
     * see adc_monitor_init).  Non-blocking: read the result with
     * adc_monitor_poll_vin_result() or adc_monitor_read_vin_result().
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

bool adc_monitor_poll_vin_result(void)
{
    if ((ADC2->ISR & ADC_ISR_EOC) == 0u)
    {
        return false;
    }
    uint16_t raw = (uint16_t)ADC2->DR;   /* clears EOC */
    adc_measurements.v_in_mv = scale_voltage_mv(raw, VS_MON_FULL_SCALE_MV);
    return true;
}

void adc_monitor_read_vin_result(void)
{
    /* A bounded spin (the conversion takes 13.9 us), not
     * HAL_ADC_PollForConversion: its 1 ms timeout reads HAL_GetTick(), which
     * cannot advance with interrupts masked.  A missed conversion keeps the
     * last V_in.                                                          */
    for (uint32_t spin = 0u; spin < 20000u; spin++)
    {
        if (adc_monitor_poll_vin_result())
        {
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
