/*
 * power_stage.c - Buck-Boost power stage control
 *
 * Inner loop: peak-current-mode via COMP1/DAC3 CH1 → HRTIM EEV4 (cycle-by-cycle)
 * Outer loop: PI voltage regulation via TIM6 ISR adjusting the peak current reference
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "power_stage.h"

#include "main.h"
#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_adc.h"
#include "stm32g4xx_hal_hrtim.h"
#include "stm32g4xx_ll_tim.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_exti.h"
#include "stm32g4xx_ll_system.h"
#include "src1m1_conf.h" // TCPP0203 board definitions (pins, sense ratios)

/* ---- External HAL handles from main.c ---- */
extern HRTIM_HandleTypeDef hhrtim1;
extern DAC_HandleTypeDef hdac3;
extern COMP_HandleTypeDef hcomp1;
extern ADC_HandleTypeDef hadc2;     // VIN (PA4) is on ADC2, not ADC1
extern UART_HandleTypeDef hlpuart1; // UART for debug prints

/* ---- Static state ---- */

// Unified control configuration (PI and limits)
static PS_ControlCfg s_ctrl = {
    .vout_ref_V = 5.0f,
    .iout_ref_A = 3.0f,
    .il_peak_A_min = 0.1f,
    .il_peak_A_max = 20.0f,
    .slope_A_per_s = 6000.0f,
    .kp_v = 0.05f,
    .ki_v = 200.0f};
static bool s_enabled = false;
static volatile PS_Mode s_mode = PS_MODE_OFF;

// ADC scaling factors — initialized in PS_Init() (macros IL_SHUNT_OHM etc. defined below)
static PS_Scaling s_scaling = {0};

// Latest measurement snapshot (updated in PID ISR, read by PS_GetMeas)
static volatile PS_Meas s_meas = {0};

// Slope compensation and bootstrap refresh control
static float s_slope_comp_A_per_s = 0.0f;
static uint16_t s_bootstrap_refresh_every_n = 1;
static uint16_t s_bootstrap_refresh_counter = 0;
static float s_il_pk_ref_A = 1.0f; // Current peak reference (updated each cycle by PID)

// Low-speed timer (TIM7) used to IRQ-step the slope compensation during the PWM on-time
static bool s_tim7_inited = false;
static volatile uint16_t s_slope_steps_remaining = 0;
static float s_slope_step_us = 1.0f;

// Track which HRTIM timer is PWM vs refresh, and current period/refresh ticks
static uint32_t s_pwm_timer_idx = HRTIM_TIMERINDEX_TIMER_A;
static uint32_t s_refresh_timer_idx = HRTIM_TIMERINDEX_TIMER_B;
static uint16_t s_hrtim_period = 0;
static uint16_t s_refresh_ticks_cfg = 1;
static uint32_t s_hrtim_ticks_per_us = 0;
static volatile bool s_refresh_pulse_next = true;

// Local ADC helper (polling, no DMA) for simple bring-up
static ADC_HandleTypeDef hadc1;
static bool s_adc_inited = false;

// PID state for voltage loop (runs in TIM6 ISR)
static bool s_tim6_inited = false;
static volatile bool s_pid_running = false;
static volatile float s_pid_vout_ref = 5.0f;
static volatile float s_pid_iout_limit = 3.0f;
static volatile float s_pid_integrator = 0.0f;
static volatile float s_pid_error_prev = 0.0f;
static volatile float s_pid_softstart_ref = 0.0f; // ramps from 0 to target
static volatile bool s_pid_softstart_done = false;
#define PID_SOFTSTART_RATE_V_PER_S 2000.0f // ramp rate during soft-start

// Debug print throttling (every N cycles to avoid overwhelming serial)
#define DEBUG_PRINT_INTERVAL 1000 // Print every 1000 PID cycles (~50ms at 20kHz)
static uint16_t s_debug_print_counter = 0;

// Thresholds
#define VBUS_ON_THRESHOLD_MV 4000u
#define MODE_BUCK_MARGIN 1.10f  // buck if Vin > Vout * 1.10
#define MODE_BOOST_MARGIN 0.90f // boost if Vin < Vout * 0.90

// Inductor current trip conversion defaults
#ifndef IL_SHUNT_OHM
#define IL_SHUNT_OHM (0.005f) // 5 mΩ default
#endif
#ifndef IL_AMP_GAIN
#define IL_AMP_GAIN (50.0f) // INA281A2 gain
#endif

#define IOUT_SHUNT_OHM (0.012f)
#define IIN_SHUNT_OHM (0.005f)

/* ---- DAC conversion helper ---- */

// Convert inductor current in Amps to DAC12 code on 3.3 V reference
static uint32_t PS_IL_A_to_DAC12(float A)
{
  float v = A * IL_SHUNT_OHM * IL_AMP_GAIN;
  if (v < 0.0f)
    v = 0.0f;
  if (v > 3.3f)
    v = 3.3f;
  uint32_t code = (uint32_t)((v / 3.3f) * 4095.0f);
  if (code > 4095u)
    code = 4095u;
  return code;
}

/* ---- ADC helpers ---- */

static void PS_ADC_Init(void)
{
  if (s_adc_inited)
    return;

  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  // PA0 (VOUT), PA1 (IL_INSTANT), PA4 (VIN)
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // PB0 (IIN_AVG)
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  // PC1 (IOUT_AVG)
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.GainCompensation = 0;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  HAL_ADC_Init(&hadc1);
  HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);

  s_adc_inited = true;
}

static uint16_t PS_ADC_Read_Channel(uint32_t channel)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  sConfig.Channel = channel;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_12CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  HAL_ADC_ConfigChannel(&hadc1, &sConfig);

  HAL_ADC_Start(&hadc1);
  HAL_ADC_PollForConversion(&hadc1, 2);
  uint16_t val = (uint16_t)HAL_ADC_GetValue(&hadc1);
  HAL_ADC_Stop(&hadc1);
  return val;
}

// Unified ADC sampling with scaling (returns float in Volts or Amps)
// Note: VIN (PS_CH_VIN) is on ADC2_IN17, all others on ADC1
static float PS_ReadAndScale(uint32_t adc_channel, float scale_factor)
{
  if (!s_adc_inited)
    PS_ADC_Init();

  uint16_t raw = 0;

  // VIN is on ADC2, all others on ADC1
  if (adc_channel == PS_CH_VIN)
  {
    // Read from ADC2 (already configured in main.c)
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel = adc_channel;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_12CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    HAL_ADC_ConfigChannel(&hadc2, &sConfig);

    HAL_ADC_Start(&hadc2);
    HAL_ADC_PollForConversion(&hadc2, 2);
    raw = (uint16_t)HAL_ADC_GetValue(&hadc2);
    HAL_ADC_Stop(&hadc2);
  }
  else
  {
    // Read from ADC1 (all other channels)
    raw = PS_ADC_Read_Channel(adc_channel);
  }

  return (float)raw * scale_factor;
}

/* ---- TCPP0203 GPIO helpers ---- */

static void PS_TCPP_EnablePinInit(void)
{
  TCPP0203_PORT0_ENABLE_GPIO_CLK_ENABLE();
  LL_GPIO_SetPinMode(TCPP0203_PORT0_ENABLE_GPIO_PORT, TCPP0203_PORT0_ENABLE_GPIO_PIN, TCPP0203_PORT0_ENABLE_GPIO_MODE);
  LL_GPIO_SetPinOutputType(TCPP0203_PORT0_ENABLE_GPIO_PORT, TCPP0203_PORT0_ENABLE_GPIO_PIN, TCPP0203_PORT0_ENABLE_GPIO_OUTPUT);
  LL_GPIO_SetPinPull(TCPP0203_PORT0_ENABLE_GPIO_PORT, TCPP0203_PORT0_ENABLE_GPIO_PIN, TCPP0203_PORT0_ENABLE_GPIO_PUPD);
  TCPP0203_PORT0_ENABLE_GPIO_DEFVALUE();
}

static void PS_TCPP_FLG_Init(void)
{
  TCPP0203_PORT0_FLG_GPIO_CLK_ENABLE();
  LL_GPIO_SetPinMode(TCPP0203_PORT0_FLG_GPIO_PORT, TCPP0203_PORT0_FLG_GPIO_PIN, TCPP0203_PORT0_FLG_GPIO_MODE);
  LL_GPIO_SetPinPull(TCPP0203_PORT0_FLG_GPIO_PORT, TCPP0203_PORT0_FLG_GPIO_PIN, TCPP0203_PORT0_FLG_GPIO_PUPD);
  TCPP0203_PORT0_FLG_SET_EXTI();
  TCPP0203_PORT0_FLG_TRIG_ENABLE();
  TCPP0203_PORT0_FLG_EXTI_ENABLE();
  NVIC_SetPriority(TCPP0203_PORT0_FLG_EXTI_IRQN, TCPP0203_PORT0_FLG_IT_PRIORITY);
  NVIC_EnableIRQ(TCPP0203_PORT0_FLG_EXTI_IRQN);
}

/* ---- Public API: Init / Enable ---- */

void PS_Init(void)
{
  // Initialize ADC scaling factors (requires macros from src1m1_conf.h and local defines)
  s_scaling.vin_gain = 3.3f / 4095.0f * (float)(PS_VSENSE_RA + PS_VSENSE_RB) / (float)PS_VSENSE_RB;
  s_scaling.vout_gain = 3.3f / 4095.0f * (float)(PS_VSENSE_RA + PS_VSENSE_RB) / (float)PS_VSENSE_RB;
  s_scaling.iin_gain = 3.3f / 4095.0f / (IIN_SHUNT_OHM * IL_AMP_GAIN);
  s_scaling.iout_gain = 3.3f / 4095.0f / (IOUT_SHUNT_OHM * IL_AMP_GAIN);
  s_scaling.il_gain = 3.3f / 4095.0f / (IL_SHUNT_OHM * IL_AMP_GAIN);
  s_scaling.adc_fullscale = 4095;

  PS_TCPP_EnablePinInit();
  PS_TCPP_FLG_Init();
  PS_ADC_Init();

  // Start fast analog inner loop blocks (DAC3 CH1 → COMP1- ; COMP1 output → HRTIM EEV4)
  if (HAL_DAC_Start(&hdac3, DAC_CHANNEL_1) == HAL_OK)
  {
    HAL_DAC_SetValue(&hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R, PS_IL_A_to_DAC12(1.0f));
  }
  HAL_COMP_Start(&hcomp1);

  s_enabled = false;
  s_mode = PS_MODE_OFF;

  // Precompute HRTIM ticks/us for slope timing
  uint32_t sysclk = HAL_RCC_GetSysClockFreq();
  if (sysclk > 0u)
  {
    s_hrtim_ticks_per_us = (sysclk * 32u) / 1000000u;
    if (s_hrtim_ticks_per_us == 0u)
      s_hrtim_ticks_per_us = 1u;
  }
}

void PS_Enable(bool en)
{
  if (en)
  {
    TCPP0203_PORT0_ENABLE_GPIO_SET();
    s_enabled = true;
  }
  else
  {
    TCPP0203_PORT0_ENABLE_GPIO_RESET();
    s_enabled = false;
  }
}

PS_Mode PS_GetMode(void)
{
  return s_mode;
}

void PS_SetScaling(const PS_Scaling *s)
{
  if (s)
    s_scaling = *s;
}

void PS_SetControlCfg(const PS_ControlCfg *c)
{
  if (!c)
    return;
  s_ctrl = *c;
  // Propagate live references to PID if running
  s_pid_vout_ref = s_ctrl.vout_ref_V;
  s_pid_iout_limit = s_ctrl.iout_ref_A;
  if (s_ctrl.slope_A_per_s >= 0.0f)
    s_slope_comp_A_per_s = s_ctrl.slope_A_per_s;
}

bool PS_GetMeas(PS_Meas *out)
{
  if (!out)
    return false;
  // Atomic-ish copy of the volatile snapshot (single-core Cortex-M4, word-aligned reads)
  out->vin = s_meas.vin;
  out->vout = s_meas.vout;
  out->iin_avg = s_meas.iin_avg;
  out->iout_avg = s_meas.iout_avg;
  out->il_inst = s_meas.il_inst;
  return (s_mode != PS_MODE_OFF);
}

void PS_SetTargets_mV_mA(uint32_t vbus_mv, uint32_t iout_ma)
{
  s_ctrl.vout_ref_V = (float)vbus_mv / 1000.0f;
  s_ctrl.iout_ref_A = (float)iout_ma / 1000.0f;
  // If PID is running, update its references live
  s_pid_vout_ref = s_ctrl.vout_ref_V;
  s_pid_iout_limit = s_ctrl.iout_ref_A;
}

bool PS_GetMeasurements_mV_mA(uint16_t *vbus_mv, int16_t *iout_ma)
{
  // Read VOUT in Volts, convert to mV
  float vout_V = PS_ReadAndScale(PS_CH_VOUT, s_scaling.vout_gain);
  if (vbus_mv)
  {
    uint32_t vbus_mv_val = (uint32_t)(vout_V * 1000.0f);
    *vbus_mv = (uint16_t)(vbus_mv_val > 0xFFFFu ? 0xFFFFu : vbus_mv_val);
  }

  // Read IOUT in Amps, convert to mA
  float iout_A = PS_ReadAndScale(PS_CH_IOUT_AVG, s_scaling.iout_gain);
  if (iout_ma)
    *iout_ma = (int16_t)(iout_A * 1000.0f);

  return true;
}

bool PS_IsOn(void)
{
  uint16_t vbus_mv = 0;
  int16_t iout_ma = 0;
  PS_GetMeasurements_mV_mA(&vbus_mv, &iout_ma);
  return s_enabled && (vbus_mv >= VBUS_ON_THRESHOLD_MV);
}

/* ---- HRTIM output configuration helpers ---- */

// Internal helper: check if bootstrap refresh should occur this cycle
static bool PS_ShouldRefreshBootstrap(void)
{
  s_bootstrap_refresh_counter++;
  if (s_bootstrap_refresh_counter >= s_bootstrap_refresh_every_n)
  {
    s_bootstrap_refresh_counter = 0;
    return true;
  }
  return false;
}

// Slope compensation ramp applied to DAC threshold during PWM on-time
static void PS_ApplySlopeCompensation(float on_time_us)
{
  if (s_slope_comp_A_per_s == 0.0f)
    return;

  float ramp_A = s_slope_comp_A_per_s * (on_time_us / 1e6f);
  float compensated_peak = s_il_pk_ref_A + ramp_A;
  if (compensated_peak < s_ctrl.il_peak_A_min)
    compensated_peak = s_ctrl.il_peak_A_min;
  if (compensated_peak > s_ctrl.il_peak_A_max)
    compensated_peak = s_ctrl.il_peak_A_max;

  uint32_t code = PS_IL_A_to_DAC12(compensated_peak);
  if (HAL_DAC_GetState(&hdac3) == HAL_DAC_STATE_RESET)
  {
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);
  }
  HAL_DAC_SetValue(&hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R, code);
}

/* ---- TIM7: slope compensation step timer ---- */

static void PS_TIM7_Init(void)
{
  if (s_tim7_inited)
    return;

  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM7);

  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  uint32_t presc = (pclk1 / 1000000u);
  if (presc == 0u)
    presc = 1u;
  LL_TIM_DisableCounter(TIM7);
  LL_TIM_SetPrescaler(TIM7, (uint16_t)(presc - 1u));
  LL_TIM_SetAutoReload(TIM7, 0);
  LL_TIM_SetCounterMode(TIM7, LL_TIM_COUNTERMODE_UP);
  LL_TIM_SetUpdateSource(TIM7, LL_TIM_UPDATESOURCE_REGULAR);
  LL_TIM_EnableARRPreload(TIM7);

  NVIC_SetPriority(TIM7_DAC_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 2, 0));
  NVIC_EnableIRQ(TIM7_DAC_IRQn);

  s_tim7_inited = true;
}

static void PS_SlopeTimer_Start(float step_us, uint16_t steps)
{
  if (steps == 0)
    return;
  if (!s_tim7_inited)
    PS_TIM7_Init();
  s_slope_step_us = (step_us <= 0.0f) ? 1.0f : step_us;
  s_slope_steps_remaining = steps;

  uint32_t arr = (uint32_t)(s_slope_step_us);
  if (arr == 0u)
    arr = 1u;
  if (arr > 0xFFFFu)
    arr = 0xFFFFu;
  LL_TIM_SetAutoReload(TIM7, (uint16_t)(arr - 1u));
  LL_TIM_SetCounter(TIM7, 0);
  LL_TIM_ClearFlag_UPDATE(TIM7);
  LL_TIM_EnableIT_UPDATE(TIM7);
  LL_TIM_EnableCounter(TIM7);
}

static void PS_SlopeTimer_Stop(void)
{
  if (!s_tim7_inited)
    return;
  LL_TIM_DisableIT_UPDATE(TIM7);
  LL_TIM_DisableCounter(TIM7);
  s_slope_steps_remaining = 0;
}

/* ---- HRTIM output configuration ---- */

// Configure output for BUCK-mode PWM: HIGH at period, LOW at compare/EEV
// This is the standard configuration for the buck (high-side) leg.
static void PS_ConfigOutputForBuckPWM(uint32_t timerIdx, uint16_t period, uint16_t cmp, bool include_eev)
{
  HRTIM_CompareCfgTypeDef cmpCfg = {0};
  cmpCfg.CompareValue = cmp;
  HAL_HRTIM_WaveformCompareConfig(&hhrtim1, timerIdx, HRTIM_COMPAREUNIT_1, &cmpCfg);

  HRTIM_OutputCfgTypeDef out = {0};
  out.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
  out.SetSource = HRTIM_OUTPUTSET_TIMPER;
  out.ResetSource = HRTIM_OUTPUTRESET_TIMCMP1 | (include_eev ? HRTIM_OUTPUTRESET_EEV_4 : 0);
  out.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
  out.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
  out.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
  out.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
  out.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1,
                                 &out);
  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2,
                                 &out);
}

// Configure output for BOOST-mode PWM: LOW at period, HIGH at compare/EEV
// In boost, the energy-storage phase (low-side Q4 ON) is the LOW phase.
// The current trip (EEV4) should TERMINATE energy storage by setting the output HIGH
// (turning on high-side Q3 to deliver energy to output).
static void PS_ConfigOutputForBoostPWM(uint32_t timerIdx, uint16_t period, uint16_t cmp, bool include_eev)
{
  HRTIM_CompareCfgTypeDef cmpCfg = {0};
  cmpCfg.CompareValue = cmp;
  HAL_HRTIM_WaveformCompareConfig(&hhrtim1, timerIdx, HRTIM_COMPAREUNIT_1, &cmpCfg);

  HRTIM_OutputCfgTypeDef out = {0};
  out.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
  // SWAPPED vs buck: Set on compare/EEV, Reset on period
  out.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | (include_eev ? HRTIM_OUTPUTSET_EEV_4 : 0);
  out.ResetSource = HRTIM_OUTPUTRESET_TIMPER;
  out.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
  out.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
  out.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
  out.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
  out.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1,
                                 &out);
  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2,
                                 &out);
}

// Configure a leg to be mostly-on with a small refresh window (bootstrap recharge)
static void PS_ConfigOutputForRefresh(uint32_t timerIdx, uint16_t period, uint16_t refresh_ticks, bool enable_refresh)
{
  if (enable_refresh)
  {
    if (refresh_ticks < 1)
      refresh_ticks = 1;
    if (refresh_ticks >= period)
      refresh_ticks = period - 1;
    uint16_t cmp = (uint16_t)(period - refresh_ticks);
    PS_ConfigOutputForBuckPWM(timerIdx, period, cmp, false);
  }
  else
  {
    HRTIM_OutputCfgTypeDef out = {0};
    out.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
    out.SetSource = HRTIM_OUTPUTSET_TIMPER;
    out.ResetSource = HRTIM_OUTPUTRESET_NONE;
    out.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
    out.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    out.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
    out.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    out.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                   (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1,
                                   &out);
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                   (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2,
                                   &out);
  }
}

// Shared deadtime config helper
static void PS_ConfigDeadtime(uint16_t deadtime_ticks)
{
  HRTIM_DeadTimeCfgTypeDef dt = {0};
  dt.Prescaler = HRTIM_TIMDEADTIME_PRESCALERRATIO_MUL8;
  dt.RisingValue = deadtime_ticks;
  dt.RisingSign = HRTIM_TIMDEADTIME_RISINGSIGN_POSITIVE;
  dt.RisingLock = HRTIM_TIMDEADTIME_RISINGLOCK_WRITE;
  dt.RisingSignLock = HRTIM_TIMDEADTIME_RISINGSIGNLOCK_WRITE;
  dt.FallingValue = deadtime_ticks;
  dt.FallingSign = HRTIM_TIMDEADTIME_FALLINGSIGN_POSITIVE;
  dt.FallingLock = HRTIM_TIMDEADTIME_FALLINGLOCK_WRITE;
  dt.FallingSignLock = HRTIM_TIMDEADTIME_FALLINGSIGNLOCK_WRITE;
  HAL_HRTIM_DeadTimeConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, &dt);
  HAL_HRTIM_DeadTimeConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, &dt);
}

// Enable HRTIM period-reset IRQs for both timers
static void PS_EnableHrtimIRQs(void)
{
  char n1[] = "[DEBUG] EnableHrtimIRQs: entry\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n1, sizeof(n1) - 1, 10);

  __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, HRTIM_TIM_IT_RST);

  char n2[] = "[DEBUG] After Timer A enable IT\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n2, sizeof(n2) - 1, 10);

  __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, HRTIM_TIM_IT_RST);

  char n3[] = "[DEBUG] After Timer B enable IT\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n3, sizeof(n3) - 1, 10);

  NVIC_SetPriority(HRTIM1_TIMA_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0));

  char n4[] = "[DEBUG] After Timer A SetPriority\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n4, sizeof(n4) - 1, 10);

  NVIC_EnableIRQ(HRTIM1_TIMA_IRQn);

  char n5[] = "[DEBUG] After Timer A EnableIRQ\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n5, sizeof(n5) - 1, 10);

  NVIC_SetPriority(HRTIM1_TIMB_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0));

  char n6[] = "[DEBUG] After Timer B SetPriority\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n6, sizeof(n6) - 1, 10);

  NVIC_EnableIRQ(HRTIM1_TIMB_IRQn);

  char n7[] = "[DEBUG] After Timer B EnableIRQ\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)n7, sizeof(n7) - 1, 10);
}

// Start both HRTIM outputs and timers
static void PS_StartHrtimOutputs(void)
{
  char m1[] = "[DEBUG] B4WaveOut\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)m1, sizeof(m1) - 1, 10);

  HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);

  char m2[] = "[DEBUG] AfterWaveOut\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)m2, sizeof(m2) - 1, 10);

  char m2b[] = "[DEBUG] B4WaveCount\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)m2b, sizeof(m2b) - 1, 10);

  HAL_HRTIM_WaveformCountStart(&hhrtim1,
                               HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);

  char m3[] = "[DEBUG] AfterWaveCount\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)m3, sizeof(m3) - 1, 10);
}

// Stop both HRTIM outputs and timers
static void PS_StopHrtimOutputs(void)
{
  HAL_HRTIM_WaveformOutputStop(&hhrtim1,
                               HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
  HAL_HRTIM_WaveformCountStop(&hhrtim1,
                              HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
}

/* ---- Public API: manual test modes ---- */

void PS_HRTIM_TestStart(uint16_t dutyA_permille, uint16_t dutyB_permille, uint16_t deadtime_ticks)
{
  uint16_t period = __HAL_HRTIM_GETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER);
  if (period == 0)
    period = 0xFFDF;

  uint16_t cmpA = (dutyA_permille > 1000) ? period : (uint16_t)((uint32_t)period * dutyA_permille / 1000u);
  uint16_t cmpB = (dutyB_permille > 1000) ? period : (uint16_t)((uint32_t)period * dutyB_permille / 1000u);

  PS_ConfigDeadtime(deadtime_ticks);
  PS_ConfigOutputForBuckPWM(HRTIM_TIMERINDEX_TIMER_A, period, cmpA, false);
  PS_ConfigOutputForBuckPWM(HRTIM_TIMERINDEX_TIMER_B, period, cmpB, false);
  PS_StartHrtimOutputs();
}

void PS_HRTIM_TestStop(void)
{
  PS_StopHrtimOutputs();
}

/* ---- Public API: Buck mode ---- */

void PS_StartBuckMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks)
{
  char msg[] = "[DEBUG] BUCK: before GetPeriod\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, sizeof(msg) - 1, 10);

  uint16_t period = __HAL_HRTIM_GETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER);

  char msg2[] = "[DEBUG] BUCK: after GetPeriod\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg2, sizeof(msg2) - 1, 10);

  if (period == 0)
    period = 0xFFDF;

  uint16_t cmpA = (pwm_permille > 1000) ? period : (uint16_t)((uint32_t)period * pwm_permille / 1000u);
  uint16_t refresh_ticks = (refresh_permille > 1000) ? 1 : (uint16_t)((uint32_t)period * refresh_permille / 1000u);

  // Buck: Timer A is PWM leg (high at period, low at compare/EEV4)
  // Timer B is pass-through (held high with periodic bootstrap refresh)

  char msg_dt[] = "[DEBUG] Before ConfigDeadtime\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg_dt, sizeof(msg_dt) - 1, 10);
  PS_ConfigDeadtime(deadtime_ticks);

  char msg_bp[] = "[DEBUG] Before ConfigOutputForBuckPWM\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg_bp, sizeof(msg_bp) - 1, 10);
  PS_ConfigOutputForBuckPWM(HRTIM_TIMERINDEX_TIMER_A, period, cmpA, true);

  char msg_rf[] = "[DEBUG] Before ConfigOutputForRefresh\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg_rf, sizeof(msg_rf) - 1, 10);
  PS_ConfigOutputForRefresh(HRTIM_TIMERINDEX_TIMER_B, period, refresh_ticks, false);

  char msg_sh[] = "[DEBUG] Before StartHrtimOutputs\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg_sh, sizeof(msg_sh) - 1, 10);
  PS_StartHrtimOutputs();

  s_pwm_timer_idx = HRTIM_TIMERINDEX_TIMER_A;
  s_refresh_timer_idx = HRTIM_TIMERINDEX_TIMER_B;
  s_hrtim_period = period;
  s_refresh_ticks_cfg = refresh_ticks;
  s_bootstrap_refresh_counter = (uint16_t)((s_bootstrap_refresh_every_n > 0) ? (s_bootstrap_refresh_every_n - 1) : 0);
  s_refresh_pulse_next = true;
  s_mode = PS_MODE_BUCK;

  PS_EnableHrtimIRQs();

  char msg3[] = "[DEBUG] BUCK: after EnableHrtimIRQs\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg3, sizeof(msg3) - 1, 10);
}

/* ---- Public API: Boost mode (FIXED: inverted PWM phase) ---- */

void PS_StartBoostMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks)
{
  uint16_t period = __HAL_HRTIM_GETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER);
  if (period == 0)
    period = 0xFFDF;

  // In boost mode, pwm_permille represents the energy-storage duty (Q4 ON time).
  // With the inverted output, compare value sets where the output goes HIGH (Q3 ON).
  // So cmpB = period * (1 - pwm_permille/1000) would be the compare point,
  // but since the output resets at period (goes LOW) and sets at compare (goes HIGH),
  // the LOW time = compare ticks, HIGH time = period - compare ticks.
  // For duty D of Q4 (low-side): LOW time / period = D, so compare = period * D.
  uint16_t cmpB = (pwm_permille > 1000) ? period : (uint16_t)((uint32_t)period * pwm_permille / 1000u);
  uint16_t refresh_ticks = (refresh_permille > 1000) ? 1 : (uint16_t)((uint32_t)period * refresh_permille / 1000u);

  PS_ConfigDeadtime(deadtime_ticks);

  // Boost: Timer A is pass-through (held high with periodic bootstrap refresh)
  // Timer B is PWM leg with INVERTED phase (low at period = Q4 ON, high at compare/EEV4 = Q3 ON)
  PS_ConfigOutputForRefresh(HRTIM_TIMERINDEX_TIMER_A, period, refresh_ticks, false);
  PS_ConfigOutputForBoostPWM(HRTIM_TIMERINDEX_TIMER_B, period, cmpB, true);

  PS_StartHrtimOutputs();

  s_pwm_timer_idx = HRTIM_TIMERINDEX_TIMER_B;
  s_refresh_timer_idx = HRTIM_TIMERINDEX_TIMER_A;
  s_hrtim_period = period;
  s_refresh_ticks_cfg = refresh_ticks;
  s_bootstrap_refresh_counter = (uint16_t)((s_bootstrap_refresh_every_n > 0) ? (s_bootstrap_refresh_every_n - 1) : 0);
  s_refresh_pulse_next = true;
  s_mode = PS_MODE_BOOST;

  PS_EnableHrtimIRQs();
}

/* ---- Public API: Stop converter ---- */

void PS_Stop(void)
{
  s_pid_running = false;
  PS_SlopeTimer_Stop();
  PS_StopHrtimOutputs();

  // Stop TIM6 PID loop if running
  if (s_tim6_inited)
  {
    LL_TIM_DisableIT_UPDATE(TIM6);
    LL_TIM_DisableCounter(TIM6);
  }

  s_mode = PS_MODE_OFF;
  s_pid_integrator = 0.0f;
  s_pid_error_prev = 0.0f;
  s_pid_softstart_done = false;
  s_pid_softstart_ref = 0.0f;
}

/* ---- Public API: Current trip and slope ---- */

void PS_SetCurrentTrip_A(float il_peak_A)
{
  if (il_peak_A < 0.0f)
    il_peak_A = 0.0f;
  s_il_pk_ref_A = il_peak_A;
  uint32_t code = PS_IL_A_to_DAC12(il_peak_A);
  if (HAL_DAC_GetState(&hdac3) == HAL_DAC_STATE_RESET)
  {
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);
  }
  HAL_DAC_SetValue(&hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R, code);
}

void PS_SetSlopeCompensation(float slope_A_per_s)
{
  if (slope_A_per_s < 0.0f)
    slope_A_per_s = 0.0f;
  s_slope_comp_A_per_s = slope_A_per_s;
}

void PS_SetBootstrapRefreshPeriod(uint16_t refresh_every_n)
{
  if (refresh_every_n < 1)
    refresh_every_n = 1;
  s_bootstrap_refresh_every_n = refresh_every_n;
  s_bootstrap_refresh_counter = (uint16_t)(refresh_every_n - 1);
}

/* ---- TIM6: PID voltage loop timer ---- */

static void PS_TIM6_Init(void)
{
  if (s_tim6_inited)
    return;

  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);

  uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  // Prescaler to get 1 MHz timer clock, then ARR for desired PID rate
  uint32_t presc = (pclk1 / 1000000u);
  if (presc == 0u)
    presc = 1u;
  uint32_t arr = (1000000u / PS_PID_LOOP_HZ);
  if (arr == 0u)
    arr = 1u;
  if (arr > 0xFFFFu)
    arr = 0xFFFFu;

  LL_TIM_DisableCounter(TIM6);
  LL_TIM_SetPrescaler(TIM6, (uint16_t)(presc - 1u));
  LL_TIM_SetAutoReload(TIM6, (uint16_t)(arr - 1u));
  LL_TIM_SetCounterMode(TIM6, LL_TIM_COUNTERMODE_UP);
  LL_TIM_SetUpdateSource(TIM6, LL_TIM_UPDATESOURCE_REGULAR);
  LL_TIM_EnableARRPreload(TIM6);

  NVIC_SetPriority(TIM6_DAC_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 3, 0));
  NVIC_EnableIRQ(TIM6_DAC_IRQn);

  s_tim6_inited = true;
}

static void PS_PID_Start(void)
{
  if (!s_tim6_inited)
    PS_TIM6_Init();

  s_pid_integrator = 0.0f;
  s_pid_error_prev = 0.0f;
  s_pid_softstart_ref = 0.0f;
  s_pid_softstart_done = false;
  s_pid_running = true;

  LL_TIM_SetCounter(TIM6, 0);
  LL_TIM_ClearFlag_UPDATE(TIM6);
  LL_TIM_EnableIT_UPDATE(TIM6);
  LL_TIM_EnableCounter(TIM6);
}

static void PS_PID_Stop(void)
{
  s_pid_running = false;
  if (s_tim6_inited)
  {
    LL_TIM_DisableIT_UPDATE(TIM6);
    LL_TIM_DisableCounter(TIM6);
  }
}

/* ---- Public API: Closed-loop regulation ---- */

void PS_StartClosedLoop(float vout_ref_V, float iout_limit_A)
{
  // DIAGNOSTIC: Print entry point
  char diag[50];
  int len = snprintf(diag, sizeof(diag), "[DEBUG] StartClosedLoop entry\r\n");
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);

  // Stop any existing operation
  PS_Stop();

  // Store targets
  s_ctrl.vout_ref_V = vout_ref_V;
  s_ctrl.iout_ref_A = iout_limit_A;
  s_pid_vout_ref = vout_ref_V;
  s_pid_iout_limit = iout_limit_A;

  // DIAGNOSTIC: About to read VIN from ADC2
  char msg_vin[] = "[DEBUG] Before VIN read\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg_vin, sizeof(msg_vin) - 1, 10);

  // TEMPORARY FIX: Use fixed VIN value to test if HRTIM works
  // Real ADC2 read: float vin_V = PS_ReadAndScale(PS_CH_VIN, s_scaling.vin_gain);
  float vin_V = 12.0f; // Fixed 12V for testing

  // DIAGNOSTIC: VIN read completed
  char msg_vin2[] = "[DEBUG] VIN fixed to 12V\r\n";
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg_vin2, sizeof(msg_vin2) - 1, 10);

  uint32_t vin_mv = (uint32_t)(vin_V * 1000.0f);
  uint32_t vout_target_mv = (uint32_t)(vout_ref_V * 1000.0f);

  // Set a conservative initial peak current
  PS_SetCurrentTrip_A(s_ctrl.il_peak_A_min);

  // Enable slope compensation
  PS_SetSlopeCompensation(s_ctrl.slope_A_per_s);

  // Bootstrap refresh every 2 cycles
  PS_SetBootstrapRefreshPeriod(2);

  // Default deadtime ticks (adjust based on GaN switching characteristics)
  uint16_t dt_ticks = 20;

  // DIAGNOSTIC: About to start mode
  len = snprintf(diag, sizeof(diag), "[DEBUG] Before mode start\r\n");
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);

  // Select mode based on Vin vs Vout
  if (vin_mv > (uint32_t)(vout_target_mv * MODE_BUCK_MARGIN))
  {
    // Buck mode: Vin significantly higher than Vout
    // Initial duty ~50% as starting point; PID will adjust via peak current ref
    len = snprintf(diag, sizeof(diag), "[DEBUG] Starting BUCK mode\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
    PS_StartBuckMode(500, 50, dt_ticks);
    len = snprintf(diag, sizeof(diag), "[DEBUG] BUCK mode started\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
  }
  else if (vin_mv < (uint32_t)(vout_target_mv * MODE_BOOST_MARGIN))
  {
    // Boost mode: Vin significantly lower than Vout
    len = snprintf(diag, sizeof(diag), "[DEBUG] Starting BOOST mode\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
    PS_StartBoostMode(500, 50, dt_ticks);
    len = snprintf(diag, sizeof(diag), "[DEBUG] BOOST mode started\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
  }
  else
  {
    // Vin ≈ Vout: default to buck with low duty (near pass-through)
    // TODO: implement true buck-boost mode for this region
    len = snprintf(diag, sizeof(diag), "[DEBUG] Starting BUCK mode (pass-through)\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
    PS_StartBuckMode(990, 50, dt_ticks);
    len = snprintf(diag, sizeof(diag), "[DEBUG] BUCK mode started (pass-through)\r\n");
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
  }

  // DIAGNOSTIC: Mode started, about to enable TCPP
  len = snprintf(diag, sizeof(diag), "[DEBUG] Before TCPP enable\r\n");
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);

  // Enable TCPP gate
  PS_Enable(true);

  // DIAGNOSTIC: TCPP enabled, about to start PID
  len = snprintf(diag, sizeof(diag), "[DEBUG] Before PID start\r\n");
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);

  // Start the PID voltage loop
  PS_PID_Start();

  // DIAGNOSTIC: PID started
  len = snprintf(diag, sizeof(diag), "[DEBUG] PID started\r\n");
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)diag, len, 10);
}

/* ---- IRQ handlers ---- */

// HRTIM Timer A/B IRQ forwarders
void HRTIM1_TIMA_IRQHandler(void)
{
  HAL_HRTIM_IRQHandler(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A);
}
void HRTIM1_TIMB_IRQHandler(void)
{
  HAL_HRTIM_IRQHandler(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B);
}

// Called when timer counter resets (start of new period)
// SIMPLIFIED: Only set flags, don't call blocking HAL functions from ISR
void HAL_HRTIM_CounterResetCallback(HRTIM_HandleTypeDef *h, uint32_t TimerIdx)
{
  if (h != &hhrtim1)
    return;
  if (s_hrtim_period == 0)
    return;

  // Bootstrap refresh scheduling: just set flags, don't call HAL functions in ISR
  if (TimerIdx == s_pwm_timer_idx)
  {
    bool do_refresh = PS_ShouldRefreshBootstrap();
    s_refresh_pulse_next = do_refresh;
    // Note: HAL_HRTIM_WaveformCompareConfig calls are deferred - they happen in the next cycle
  }

  // Slope compensation: simplified to not call blocking functions from ISR
  // TODO: Defer complex HAL operations to a non-ISR context if needed
}

// TIM6 ISR: PID voltage loop (slow loop, ~20 kHz)
void TIM6_DAC_IRQHandler(void)
{
  if (LL_TIM_IsActiveFlag_UPDATE(TIM6))
  {
    LL_TIM_ClearFlag_UPDATE(TIM6);

    if (!s_pid_running)
    {
      // Debug: PID not running - print once per second
      static uint16_t pid_off_counter = 0;
      if (++pid_off_counter >= 20000)
      {
        pid_off_counter = 0;
        char msg[] = "[PID_OFF] Not running\r\n";
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, sizeof(msg) - 1, 10);
      }
      return;
    }

    // Sample VOUT and VIN using scaling factors from PS_Scaling
    // Note: In production, use DMA-driven ADC for non-blocking reads.
    // For bring-up, a fast polling read is acceptable at 20 kHz with 12.5-cycle sampling.

    // Get raw ADC readings for debugging
    if (!s_adc_inited)
      PS_ADC_Init();
    uint16_t raw_vout = PS_ADC_Read_Channel(PS_CH_VOUT);
    uint16_t raw_vin = PS_ADC_Read_Channel(PS_CH_VIN);

    float vout_V = (float)raw_vout * s_scaling.vout_gain;
    float vin_V = (float)raw_vin * s_scaling.vin_gain;

    // Update measurement snapshot for PS_GetMeas() consumers
    s_meas.vout = vout_V;
    s_meas.vin = vin_V;
    s_meas.iout_avg = PS_ReadAndScale(PS_CH_IOUT_AVG, s_scaling.iout_gain);
    s_meas.iin_avg = PS_ReadAndScale(PS_CH_IIN_AVG, s_scaling.iin_gain);
    s_meas.il_inst = PS_ReadAndScale(PS_CH_IL_INSTANT, s_scaling.il_gain);

    // Throttled debug: Print measured values (every DEBUG_PRINT_INTERVAL cycles)
    s_debug_print_counter++;
    if (s_debug_print_counter >= DEBUG_PRINT_INTERVAL)
    {
      s_debug_print_counter = 0;
      char debug_buf[100];
      uint32_t vout_mv = (uint32_t)(s_meas.vout * 1000.0f);
      uint32_t vin_mv = (uint32_t)(s_meas.vin * 1000.0f);
      uint32_t iout_ma = (uint32_t)(s_meas.iout_avg * 1000.0f);
      int len = snprintf(debug_buf, sizeof(debug_buf),
                         "[MEAS] V_out=%umV V_in=%umV I_out=%umA\r\n",
                         vout_mv, vin_mv, iout_ma);
      HAL_UART_Transmit(&hlpuart1, (uint8_t *)debug_buf, len, 10);
    }

    // Soft-start ramp
    float Ts = 1.0f / (float)PS_PID_LOOP_HZ;
    float target = s_pid_vout_ref;

    if (!s_pid_softstart_done)
    {
      s_pid_softstart_ref += PID_SOFTSTART_RATE_V_PER_S * Ts;
      if (s_pid_softstart_ref >= target)
      {
        s_pid_softstart_ref = target;
        s_pid_softstart_done = true;
      }
      target = s_pid_softstart_ref;
    }

    // PI voltage loop
    float error = target - vout_V;

    // Integrator with anti-windup clamping
    s_pid_integrator += s_ctrl.ki_v * Ts * error;
    if (s_pid_integrator > s_ctrl.il_peak_A_max)
      s_pid_integrator = s_ctrl.il_peak_A_max;
    if (s_pid_integrator < 0.0f)
      s_pid_integrator = 0.0f;

    // PI output = peak current reference
    float il_ref = s_ctrl.kp_v * error + s_pid_integrator;

    // Clamp to configured limits
    if (il_ref < s_ctrl.il_peak_A_min)
      il_ref = s_ctrl.il_peak_A_min;
    if (il_ref > s_ctrl.il_peak_A_max)
      il_ref = s_ctrl.il_peak_A_max;

    // Output current limit (CC mode): read IOUT and reduce peak if over limit
    // TODO: sample ID_MON ADC channel for output current and apply CC fallback
    // For now, just apply the voltage-loop reference
    (void)s_pid_iout_limit;

    // Throttled debug: Print PID variables (same throttling as measurements)
    if (s_debug_print_counter == 0) // Only print on the same cycle as measurements
    {
      char debug_buf[80];
      uint32_t target_mv = (uint32_t)(target * 1000.0f);
      uint32_t ilref_ma = (uint32_t)(il_ref * 1000.0f);
      int len = snprintf(debug_buf, sizeof(debug_buf),
                         "[PID] Tgt=%umV IlRef=%umA\r\n",
                         target_mv, ilref_ma);
      HAL_UART_Transmit(&hlpuart1, (uint8_t *)debug_buf, len, 10);
    }

    // Update the fast inner-loop DAC threshold
    PS_SetCurrentTrip_A(il_ref);

    s_pid_error_prev = error;
  }
}

// TIM7 shared with DAC2/4 underrun; handle only TIM7 update
void TIM7_DAC_IRQHandler(void)
{
  if (LL_TIM_IsActiveFlag_UPDATE(TIM7))
  {
    LL_TIM_ClearFlag_UPDATE(TIM7);
    if (s_slope_steps_remaining > 0)
    {
      PS_ApplySlopeCompensation(s_slope_step_us);
      s_slope_steps_remaining--;
      if (s_slope_steps_remaining == 0)
      {
        PS_SlopeTimer_Stop();
      }
    }
  }
}
