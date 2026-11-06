/**
 * @file    adc_monitor.c
 * @brief   ADC measurement subsystem implementation (NLSpec §14).
 *
 * Timer-synchronized (2026-10-22, plans/ccm-buck-50w.md section 9): once
 * per switching period HRTIM ADC trigger 1 (Timer C CMP4, placed by
 * regulator.c in the quiet part of the period) starts both ADCs together:
 *   ADC1 regular [VD_MON (V_out), IS_MON (I_in)]
 *   ADC2 regular [VS_MON (V_in),  ID_MON (I_out)]
 * one conversion per rank, into circular DMA rings.  Averaging is in
 * software, over successive periods at the same phase.
 *
 * Scaling formulas (§14.2, Appendix D), voltages with the per-channel
 * bench-calibrated full scales in regulator_config.h:
 *   V_mV  = ADC_raw × VD_MON_FULL_SCALE_MV (or VS_MON_...) / 4096
 *   I_mA (IL, IS) = ADC_raw × 13200 / 4096  ≈ 3.22 mA/count
 *   I_mA (ID)     = ADC_raw × 5500  / 4096  ≈ 1.34 mA/count
 */

#include "adc_monitor.h"
#include "main.h"           /* hadc1, hadc2, DMA handles */
#include "stm32g4xx_hal.h"

/* External peripheral handles declared in main.c */
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;

/* =========================================================================
 * Module-level state
 * =========================================================================*/

volatile AdcMeasurements adc_measurements = {0u, 0u, 0u, 0u, 0u};

volatile uint16_t adc1_dma_buffer[ADC1_DMA_BUFFER_LENGTH] = {0u};
volatile uint16_t adc2_dma_buffer[ADC2_DMA_BUFFER_LENGTH] = {0u};

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
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK)
    {
        /* TODO(hardware): same as above */
    }

    /* Both ADCs, one scan per HRTIM ADC trigger 1 (adc_monitor.h).  Written
     * here with ADSTART = 0 and the ADCs disabled (final.ioc has ADC1 as one
     * 8x-oversampled VD_MON conversion and ADC2 as one software-started
     * VS_MON conversion).
     * History: until 2026-10-22 VD_MON was 8 conversions of 6.5 cycles over
     * 3.58 us from master compare 1 (a 200 kHz phase, so at 400 kHz the
     * window crossed edges); VS_MON 16 x 0.87 us from a software start
     * (spread over 2.8 periods, edges included); the currents injected
     * groups, which delay any regular trigger arriving during them
     * (RM0440 21.4.10) and so would move a timed voltage sample.  IL_MON is
     * no longer converted (its amplifier is bandwidth-limited, Dan
     * 2026-10-22).  The 1 nF caps on the dividers are to come off: 24.5
     * cycles (576 ns) settle the 100k/5.82k (5.5 kohm) source. */
    ADC_TypeDef *const adcs[2] = { ADC1, ADC2 };
    const uint32_t v_ch[2] = { LL_ADC_CHANNEL_1, LL_ADC_CHANNEL_17 };
    const uint32_t i_ch[2] = { LL_ADC_CHANNEL_15, LL_ADC_CHANNEL_7 };
    for (uint32_t k = 0u; k < 2u; k++)
    {
        ADC_TypeDef *a = adcs[k];
        LL_ADC_SetOverSamplingScope(a, LL_ADC_OVS_DISABLE);
        a->CFGR &= ~(ADC_CFGR_JAUTO | ADC_CFGR_CONT | ADC_CFGR_DISCEN);
        LL_ADC_REG_SetTriggerSource(a, LL_ADC_REG_TRIG_EXT_HRTIM_TRG1);
        LL_ADC_REG_SetTriggerEdge(a, LL_ADC_REG_TRIG_EXT_RISING);
        LL_ADC_REG_SetDMATransfer(a, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
        LL_ADC_REG_SetOverrun(a, LL_ADC_REG_OVR_DATA_OVERWRITTEN);
#if ADC_SYNC_DOUBLE_V
        LL_ADC_REG_SetSequencerLength(a, LL_ADC_REG_SEQ_SCAN_ENABLE_3RANKS);
        LL_ADC_REG_SetSequencerRanks(a, LL_ADC_REG_RANK_1, v_ch[k]);   /* discarded */
        LL_ADC_REG_SetSequencerRanks(a, LL_ADC_REG_RANK_2, v_ch[k]);
        LL_ADC_REG_SetSequencerRanks(a, LL_ADC_REG_RANK_3, i_ch[k]);
#else
        LL_ADC_REG_SetSequencerLength(a, LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);
        LL_ADC_REG_SetSequencerRanks(a, LL_ADC_REG_RANK_1, v_ch[k]);
        LL_ADC_REG_SetSequencerRanks(a, LL_ADC_REG_RANK_2, i_ch[k]);
#endif
        LL_ADC_SetChannelSamplingTime(a, v_ch[k], ADC_SYNC_V_SAMPLING);
        LL_ADC_SetChannelSamplingTime(a, i_ch[k], ADC_SYNC_I_SAMPLING);
        a->JSQR = 0u;
    }
#if ADC2_FREE_RUN_MEAN
    /* ADC2 free-running with 256x oversampling (2026-11-03): ID_MON is a
     * pulsed current outside buck CCM (Q3's), and its amplifier rings after
     * every edge, so one sample per period at a fixed phase read 0 A in
     * buck-boost DCM and 2 A in boost CCM for 1.1 A out (bench 2026-11-03).
     * A conversion every 19 ADC clocks (447 ns) is not a divisor of the
     * switching period (5000 / 447.06 = 11.18, 425 phases before the
     * pattern repeats), so each result averages 256 phases spread over the
     * period: the period mean of a linear amplifier's output is the mean of
     * its input.  VS_MON (V_in) is averaged the same way (223 us). */
    LL_ADC_REG_SetTriggerSource(ADC2, LL_ADC_REG_TRIG_SOFTWARE);
    LL_ADC_REG_SetContinuousMode(ADC2, LL_ADC_REG_CONV_CONTINUOUS);
    LL_ADC_SetOverSamplingScope(ADC2, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
    LL_ADC_ConfigOverSamplingRatioShift(ADC2, LL_ADC_OVS_RATIO_256, LL_ADC_OVS_SHIFT_RIGHT_8);
#endif
#if IS_MON_ADC3_MEAN
    /* ADC3 on IS_MON (ADC3_IN12, the same pin as ADC1_IN15), not in
     * final.ioc: clock, regulator, calibration and start written here.
     * Same clock and conversion as ADC2 (PCLK/4, 6.5 + 12.5 cycles), so each
     * oversampled result spreads 256 phases over the period (adc_monitor.h).
     * Read without DMA: DR holds the newest 256-sample mean. */
    __HAL_RCC_ADC345_CLK_ENABLE();
    LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(ADC3), LL_ADC_CLOCK_SYNC_PCLK_DIV4);
    LL_ADC_DisableDeepPowerDown(ADC3);
    LL_ADC_EnableInternalRegulator(ADC3);
    for (volatile uint32_t d = 0u; d < 2000u; d++) { }   /* t_ADCVREG_STUP 20 us */
    LL_ADC_StartCalibration(ADC3, LL_ADC_SINGLE_ENDED);
    while (LL_ADC_IsCalibrationOnGoing(ADC3) != 0u) { }
    for (volatile uint32_t d = 0u; d < 100u; d++) { }    /* 4 ADC clocks after ADCAL */
    LL_ADC_SetResolution(ADC3, LL_ADC_RESOLUTION_12B);
    LL_ADC_REG_SetTriggerSource(ADC3, LL_ADC_REG_TRIG_SOFTWARE);
    LL_ADC_REG_SetContinuousMode(ADC3, LL_ADC_REG_CONV_CONTINUOUS);
    LL_ADC_REG_SetOverrun(ADC3, LL_ADC_REG_OVR_DATA_OVERWRITTEN);
    LL_ADC_REG_SetSequencerLength(ADC3, LL_ADC_REG_SEQ_SCAN_DISABLE);
    LL_ADC_REG_SetSequencerRanks(ADC3, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_12);
    LL_ADC_SetChannelSamplingTime(ADC3, LL_ADC_CHANNEL_12, ADC_SYNC_I_SAMPLING);
    LL_ADC_SetChannelSingleDiff(ADC3, LL_ADC_CHANNEL_12, LL_ADC_SINGLE_ENDED);
    LL_ADC_SetOverSamplingScope(ADC3, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
    LL_ADC_ConfigOverSamplingRatioShift(ADC3, LL_ADC_OVS_RATIO_256, LL_ADC_OVS_SHIFT_RIGHT_8);
    LL_ADC_ClearFlag_ADRDY(ADC3);
    LL_ADC_Enable(ADC3);
    while (LL_ADC_IsActiveFlag_ADRDY(ADC3) == 0u) { }
    LL_ADC_REG_StartConversion(ADC3);
#endif
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

uint32_t adc_monitor_latest_scan2(void)
{
    uint32_t next = ADC2_DMA_BUFFER_LENGTH - hadc2.DMA_Handle->Instance->CNDTR;
    if (next >= ADC2_DMA_BUFFER_LENGTH) { next = 0u; }
    return (next / ADC_SYNC_SCAN_LENGTH + ADC2_DMA_SCANS - 1u) % ADC2_DMA_SCANS;
}

/** Mean of the last n entries of one rank of a ring, from scan `scan` back. */
static uint32_t ring_mean(const volatile uint16_t *ring, uint32_t scans, uint32_t scan, uint32_t rank, uint32_t n)
{
    uint32_t sum = 0u;
    for (uint32_t k = 0u; k < n; k++)
    {
        sum += ring[scan * ADC_SYNC_SCAN_LENGTH + rank];
        scan = (scan + scans - 1u) % scans;
    }
    return sum / n;
}

/** Mean of the last n (>= 3) entries less the highest and lowest. */
static uint32_t ring_trimmed_mean(const volatile uint16_t *ring, uint32_t scans, uint32_t scan, uint32_t rank,
                                  uint32_t n)
{
    uint32_t sum = 0u, lo = 0xFFFFu, hi = 0u;
    for (uint32_t k = 0u; k < n; k++)
    {
        const uint32_t x = ring[scan * ADC_SYNC_SCAN_LENGTH + rank];
        sum += x;
        if (x < lo) { lo = x; }
        if (x > hi) { hi = x; }
        scan = (scan + scans - 1u) % scans;
    }
    return (sum - lo - hi) / (n - 2u);
}

void adc_monitor_update_slow(void)
{
    /* V_in (trimmed), I_out and I_in from the last ADC_SYNC_AVG_SCANS scans. */
    const uint32_t s1 = adc_monitor_latest_scan();
    const uint32_t s2 = adc_monitor_latest_scan2();
    adc_measurements.v_in_mv = scale_voltage_mv((uint16_t)ring_trimmed_mean(adc2_dma_buffer, ADC2_DMA_SCANS, s2, ADC_SYNC_RANK_V,
                                                                            ADC_SYNC_AVG_SCANS), VS_MON_FULL_SCALE_MV);
    adc_measurements.i_out_ma = scale_current_id_ma((uint16_t)ring_mean(adc2_dma_buffer, ADC2_DMA_SCANS, s2, ADC_SYNC_RANK_I,
                                                                        ADC_SYNC_AVG_SCANS));
#if IS_MON_ADC3_MEAN
    (void)s1;
    adc_measurements.i_in_ma  = scale_current_il_ma((uint16_t)LL_ADC_REG_ReadConversionData12(ADC3));
#else
    adc_measurements.i_in_ma  = scale_current_il_ma((uint16_t)ring_mean(adc1_dma_buffer, ADC1_DMA_SCANS, s1, ADC_SYNC_RANK_I,
                                                                        ADC_SYNC_AVG_SCANS));
#endif
    adc_measurements.i_inductor_ma = 0u;   /* IL_MON is not converted (adc_monitor_init) */
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
    /* Median since the synchronized scans (2026-10-22): single VD_MON
     * samples carry outliers of up to +-4.8 V, about one in three at 5 V
     * DCM, with the divider's 1 nF still fitted (so not divider noise) and
     * VS_MON on ADC2 clean at the same instants.  A trimmed mean of 9
     * dropped one of each and read 5.59 V: SW_OVP at the first start. */
    uint32_t scan = adc_monitor_latest_scan();
    const uint16_t newest = adc_monitor_vd_at(scan);
    adc_measurements.v_out_mv = scale_voltage_mv(newest, VD_MON_FULL_SCALE_MV);
    adc_monitor_update_slow();
    return adc_monitor_vd_median_raw(n);
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
    /* the newest scan whose kept VD_MON sample is written: its element is
     * before `next` */
    uint32_t scan = ((next + ADC1_DMA_BUFFER_LENGTH - 1u - ADC1_DMA_INDEX_VD_MON) % ADC1_DMA_BUFFER_LENGTH)
                    / ADC1_SCAN_LENGTH;
    for (uint32_t k = 0u; k < n && k < 8u; k++)
    {
        out[k] = adc_monitor_vd_at(scan);
        scan = (scan + ADC1_DMA_SCANS - 1u) % ADC1_DMA_SCANS;
    }
}

void adc_monitor_start_adc1_dma(void)
{
    /* ADC2's ring first (ADC2_DMA_BUFFER_LENGTH): circular, no interrupts.
     * final.ioc sets its DMA to normal mode. */
    {
        extern DMA_HandleTypeDef hdma_adc2;
        hdma_adc2.Init.Mode = DMA_CIRCULAR;
        (void)HAL_DMA_Init(&hdma_adc2);
        HAL_ADC_Start_DMA(&hadc2, (uint32_t *)(void *)adc2_dma_buffer, ADC2_DMA_BUFFER_LENGTH);
        __HAL_DMA_DISABLE_IT(hadc2.DMA_Handle, DMA_IT_TC | DMA_IT_HT);
        __HAL_ADC_DISABLE_IT(&hadc2, ADC_IT_OVR);
    }
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

void adc_monitor_scale_adc1_buffer(void)
{
    /* V_out: the newest regular result (telemetry; the regulator uses the
     * trimmed mean from adc_monitor_update).  V_in and the currents: the
     * synchronized rings (adc_monitor_update_slow).                      */
    uint32_t scan = adc_monitor_latest_scan();
    adc_measurements.v_out_mv = scale_voltage_mv(adc_monitor_vd_at(scan), VD_MON_FULL_SCALE_MV);
    adc_monitor_update_slow();
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
