/*
 * power_stage.c - Buck-Boost power stage HAL control (skeleton)
 */

#include <stdint.h>
#include <stdbool.h>
#include "power_stage.h"

#include "main.h"
#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_adc.h"
#include "stm32g4xx_hal_hrtim.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_exti.h"
#include "stm32g4xx_ll_system.h"
#include "src1m1_conf.h"  // TCPP0203 board definitions (pins, sense ratios)

// Use existing HRTIM handle from main.c
extern HRTIM_HandleTypeDef hhrtim1;

static PS_Config_t s_cfg = { .vbus_target_mv = 5000, .iout_limit_ma = 3000, .enabled = false };

// Local ADC helper (polling, no DMA) for simple bring-up
static ADC_HandleTypeDef hadc1;
static bool s_adc_inited = false;

// Simple threshold for "VBUS is on" decision
#define VBUS_ON_THRESHOLD_MV  4000u

static void PS_ADC_Init(void)
{
  if (s_adc_inited) return;

  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  // PA0 (ADC1_IN1) and PA4 (ADC1_IN9) already configured as analog in board config macros,
  // but ensure analog mode for safety.
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

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
  // Fields not available on all HAL versions intentionally left default
  HAL_ADC_Init(&hadc1);

  // Calibrate once
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

// Basic TCPP0203 EN pin init
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
  // Configure FLG pin as input with EXTI falling edge
  TCPP0203_PORT0_FLG_GPIO_CLK_ENABLE();
  LL_GPIO_SetPinMode(TCPP0203_PORT0_FLG_GPIO_PORT, TCPP0203_PORT0_FLG_GPIO_PIN, TCPP0203_PORT0_FLG_GPIO_MODE);
  LL_GPIO_SetPinPull(TCPP0203_PORT0_FLG_GPIO_PORT, TCPP0203_PORT0_FLG_GPIO_PIN, TCPP0203_PORT0_FLG_GPIO_PUPD);
  TCPP0203_PORT0_FLG_SET_EXTI();
  TCPP0203_PORT0_FLG_TRIG_ENABLE();
  TCPP0203_PORT0_FLG_EXTI_ENABLE();
  NVIC_SetPriority(TCPP0203_PORT0_FLG_EXTI_IRQN, TCPP0203_PORT0_FLG_IT_PRIORITY);
  NVIC_EnableIRQ(TCPP0203_PORT0_FLG_EXTI_IRQN);
}

void PS_Init(void)
{
  // Initialize TCPP EN control
  PS_TCPP_EnablePinInit();
  // Initialize TCPP FLG (fault) line
  PS_TCPP_FLG_Init();

  // Initialize ADC for VBUS/IOUT measurements
  PS_ADC_Init();

  // Leave PWM/HRTIM stopped here; detailed waveform setup can be added later
  s_cfg.enabled = false;
}

void PS_Enable(bool en)
{
  if (en)
  {
    TCPP0203_PORT0_ENABLE_GPIO_SET();
    s_cfg.enabled = true;
  }
  else
  {
    TCPP0203_PORT0_ENABLE_GPIO_RESET();
    s_cfg.enabled = false;
  }
}

void PS_SetTargets_mV_mA(uint32_t vbus_mv, uint32_t iout_ma)
{
  s_cfg.vbus_target_mv = vbus_mv;
  s_cfg.iout_limit_ma  = iout_ma;
  // TODO: implement control loop and HRTIM PWM update
}

bool PS_GetMeasurements_mV_mA(uint16_t* vbus_mv, int16_t* iout_ma)
{
  if (!s_adc_inited) PS_ADC_Init();

  // Read VBUS sense on ADC1 Channel 1 (PA0)
  uint16_t raw_vsense = PS_ADC_Read_Channel(ADC_CHANNEL_1);
  // Read ISENSE on ADC1 Channel 7 (per src1m1_conf.h)
  uint16_t raw_isense = PS_ADC_Read_Channel(ADC_CHANNEL_7);

  // Convert raw to mV (3.3V reference, 12-bit)
  uint32_t vsense_mv = (uint32_t)raw_vsense * 3300u / 4095u;
  uint32_t isense_mv = (uint32_t)raw_isense * 3300u / 4095u;

  // VBUS divider: VBUS = Vsense * (RA+RB)/RB
  uint32_t vbus = (vsense_mv * (SRC1M1_VSENSE_RA + SRC1M1_VSENSE_RB)) / SRC1M1_VSENSE_RB;
  if (vbus_mv) *vbus_mv = (uint16_t)(vbus > 0xFFFFu ? 0xFFFFu : vbus);

  // Current: Vsense = I * RS * GA  => I(mA) = Vsense(mV) / (RS(mOhm) * GA) * 1000
  // Here RS in milliohm, GA unitless, both provided in src1m1_conf.h
  int32_t iout = 0;
  if (SRC1M1_ISENSE_GA > 0u && SRC1M1_ISENSE_RS > 0u)
  {
    iout = (int32_t)((isense_mv * 1000u) / (SRC1M1_ISENSE_RS * SRC1M1_ISENSE_GA));
  }
  if (iout_ma) *iout_ma = (int16_t)iout;

  return true;
}

bool PS_IsOn(void)
{
  uint16_t vbus_mv = 0;
  int16_t iout_ma = 0;
  PS_GetMeasurements_mV_mA(&vbus_mv, &iout_ma);
  return s_cfg.enabled && (vbus_mv >= VBUS_ON_THRESHOLD_MV);
}

// --- HRTIM test helpers ----------------------------------------------------

static void PS_ConfigOutputForPWM(uint32_t timerIdx, uint16_t period, uint16_t cmp)
{
  // Configure compare 1 for duty
  HRTIM_CompareCfgTypeDef cmpCfg = {0};
  cmpCfg.CompareValue = cmp;
  HAL_HRTIM_WaveformCompareConfig(&hhrtim1, timerIdx, HRTIM_COMPAREUNIT_1, &cmpCfg);

  // Configure outputs: set at period, reset at compare 1
  HRTIM_OutputCfgTypeDef out = {0};
  out.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
  out.SetSource = HRTIM_OUTPUTSET_TIMPER;
  out.ResetSource = HRTIM_OUTPUTRESET_TIMCMP1;
  out.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
  out.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
  out.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
  out.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
  out.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

  // Channel 1 (main)
  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1,
                                 &out);
  // Channel 2 (complementary)
  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2,
                                 &out);
}

void PS_HRTIM_TestStart(uint16_t dutyA_permille, uint16_t dutyB_permille, uint16_t deadtime_ticks)
{
  // Read the current period configured by CubeMX for timers A/B (shared with master init)
  // For simplicity, reuse period from Master (same as A/B in this project)
  uint16_t period = __HAL_HRTIM_GETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER);
  if (period == 0) period = 0xFFDF; // fallback to Cube default

  // Clamp duty to [0, period]
  uint16_t cmpA = (dutyA_permille > 1000) ? period : (uint16_t)((uint32_t)period * dutyA_permille / 1000u);
  uint16_t cmpB = (dutyB_permille > 1000) ? period : (uint16_t)((uint32_t)period * dutyB_permille / 1000u);

  // Update deadtime for both timers
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

  // Configure outputs to act as PWM
  PS_ConfigOutputForPWM(HRTIM_TIMERINDEX_TIMER_A, period, cmpA);
  PS_ConfigOutputForPWM(HRTIM_TIMERINDEX_TIMER_B, period, cmpB);

  // Start outputs and timers
  HAL_HRTIM_WaveformOutputStart(&hhrtim1,
      HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
  HAL_HRTIM_WaveformCountStart(&hhrtim1,
      HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
}

void PS_HRTIM_TestStop(void)
{
  HAL_HRTIM_WaveformOutputStop(&hhrtim1,
      HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
  HAL_HRTIM_WaveformCountStop(&hhrtim1,
      HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
}
