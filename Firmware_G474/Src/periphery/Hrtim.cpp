#include "periphery/Hrtim.h"

HRTIM_HandleTypeDef HrtimDriver::hhrtim1_{};
uint16_t HrtimDriver::period_ = 0;

static uint16_t ns_to_dt_ticks(uint32_t ns, uint32_t hrtim_clk_hz) {
    // HRTIM deadtime clock is typically fHR / 8 or fHR depending on DTG prescaler
    // Use a simple conversion assuming DTG prescaler yields 1 tick = 1/hrt_clk ns equivalent
    // This will be refined via CubeMX; placeholder approximate conversion:
    const float tick_ns = 1e9f / static_cast<float>(hrtim_clk_hz);
    return static_cast<uint16_t>(ns / tick_ns);
}

void HrtimDriver::Init(uint32_t fsw_hz, uint32_t deadtime_ns) {
    __HAL_RCC_HRTIM1_CLK_ENABLE();

    hhrtim1_.Instance = HRTIM1;
    if (HAL_HRTIM_Init(&hhrtim1_) != HAL_OK) {
        // error handler hook
    }

    // Get HRTIM base clock (depends on kernel clock); assume 170 MHz for G474
    uint32_t hrtim_clk_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_HRTIM1);
    if (hrtim_clk_hz == 0) { hrtim_clk_hz = 170000000; }

    // Up-counting period in ticks for desired Fsw, center-aligned by using CMP update
    period_ = static_cast<uint16_t>(hrtim_clk_hz / fsw_hz);

    HRTIM_TimeBaseCfgTypeDef tb = {};
    tb.Period = period_;
    tb.RepetitionCounter = 0;
    tb.PrescalerRatio = HRTIM_PRESCALERRATIO_MUL32; // placeholder, tuned by CubeMX
    tb.Mode = HRTIM_MODE_CONTINUOUS;

    HRTIM_TimerCfgTypeDef tcfg = {};
    tcfg.InterleavedMode = HRTIM_INTERLEAVED_MODE_DISABLED;
    tcfg.DMARequests = HRTIM_TIMER_DMA_NONE;
    tcfg.HalfModeEnable = HRTIM_HALFMODE_DISABLED;
    tcfg.StartOnSync = HRTIM_SYNCSTART_DISABLED;
    tcfg.ResetOnSync = HRTIM_SYNCRESET_DISABLED;
    tcfg.DACSynchro = HRTIM_DACSYNC_NONE;
    tcfg.PreloadEnable = HRTIM_PRELOAD_ENABLED;
    tcfg.UpdateGating = HRTIM_UPDATEGATING_INDEPENDENT;
    tcfg.BurstMode = HRTIM_TIMER_BURSTMODE_MAINTAINCLOCK;
    tcfg.RepetitionUpdate = HRTIM_UPDATEONREPETITION_DISABLED;

    HAL_HRTIM_TimeBaseConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, &tb);
    HAL_HRTIM_WaveformTimerConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, &tcfg);
    HAL_HRTIM_TimeBaseConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, &tb);
    HAL_HRTIM_WaveformTimerConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, &tcfg);

    // Compare units default to 50% duty
    HRTIM_CompareCfgTypeDef ccfg = {};
    ccfg.CompareValue = period_ / 2;
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, HRTIM_COMPAREUNIT_1, &ccfg);
    HAL_HRTIM_WaveformCompareConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, HRTIM_COMPAREUNIT_1, &ccfg);

    // Deadtime config
    HRTIM_DeadTimeCfgTypeDef dt = {};
    dt.Prescaler = HRTIM_DT_PRESCALER_DIV1;
    dt.RisingValue = ns_to_dt_ticks(deadtime_ns, hrtim_clk_hz);
    dt.FallingValue = ns_to_dt_ticks(deadtime_ns, hrtim_clk_hz);
    dt.RisingSign = HRTIM_TIMDEADTIME_RISING_POSITIVE;
    dt.FallingSign = HRTIM_TIMDEADTIME_FALLING_POSITIVE;
    HAL_HRTIM_DeadTimeConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, &dt);
    HAL_HRTIM_DeadTimeConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, &dt);

    // Output polarity and set/reset sources (complementary)
    HRTIM_OutputCfgTypeDef oc = {};
    oc.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    oc.SetSource = HRTIM_OUTPUTSET_TIMCMP1;
    oc.ResetSource = HRTIM_OUTPUTRESET_TIMPER;
    oc.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    oc.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    oc.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    oc.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    oc.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

    // Timer A outputs (TA1/TA2)
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, HRTIM_OUTPUT_TA1, &oc);
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, HRTIM_OUTPUT_TA2, &oc);
    // Timer B outputs (TB1/TB2)
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, HRTIM_OUTPUT_TB1, &oc);
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, HRTIM_OUTPUT_TB2, &oc);

    // Start counters and outputs
    HAL_HRTIM_WaveformCounterStart(&hhrtim1_, HRTIM_TIMERID_TIMER_A);
    HAL_HRTIM_WaveformCounterStart(&hhrtim1_, HRTIM_TIMERID_TIMER_B);
    HAL_HRTIM_WaveformOutputStart(&hhrtim1_, HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
}

void HrtimDriver::SetDuty(BoardCfg::Leg leg, uint16_t duty) {
    if (duty > period_) duty = period_;
    HRTIM_COMPAREUNIT_TypeDef cmp = HRTIM_COMPAREUNIT_1;
    if (leg == BoardCfg::Leg::Boost) {
        HRTIM_CompareCfgTypeDef ccfg = {};
        ccfg.CompareValue = duty;
        HAL_HRTIM_WaveformCompareConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_A, cmp, &ccfg);
    } else {
        HRTIM_CompareCfgTypeDef ccfg = {};
        ccfg.CompareValue = duty;
        HAL_HRTIM_WaveformCompareConfig(&hhrtim1_, HRTIM_TIMERINDEX_TIMER_B, cmp, &ccfg);
    }
}

uint16_t HrtimDriver::PeriodTicks() { return period_; }

HRTIM_HandleTypeDef* HrtimDriver::Handle() { return &hhrtim1_; }
