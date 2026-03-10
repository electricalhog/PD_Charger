/*
 * power_stage.c - Buck-Boost power stage control
 *
 * Inner loop: peak-current-mode via COMP1/DAC3 CH1 → HRTIM EEV4 (cycle-by-cycle)
 * Outer loop: PI voltage regulation via TIM6 ISR adjusting the peak current reference
 *
 * ADC measurements: DMA-backed continuous scanning — ISR reads directly from buffer,
 *                   no blocking HAL calls in the control path.
 *   ADC1 (DMA1 CH1): ranks VOUT, IL_INSTANT, IIN_AVG, IOUT_AVG  (4 channels)
 *   ADC2 (DMA1 CH6): VIN only                                    (1 channel)
 *
 * UART telemetry: non-blocking circular ring buffer drained by DMA (DMA1 CH3,
 *                 already wired to LPUART1 TX in main.c).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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
extern ADC_HandleTypeDef hadc1;     // ADC1: VOUT, IL_INSTANT, IIN_AVG, IOUT_AVG
extern ADC_HandleTypeDef hadc2;     // ADC2: VIN (PA4, ADC2_IN17)
extern UART_HandleTypeDef hlpuart1; // LPUART1 for telemetry

/* ====================================================================
 * ADC DMA scanning buffers (updated continuously by DMA, ISR reads only)
 * ==================================================================== */

#define ADC1_DMA_CH_COUNT 4u
#define ADC1_IDX_VOUT 0u /* PA0 → ADC1_IN1  */
#define ADC1_IDX_IL 1u   /* PA1 → ADC1_IN2  */
#define ADC1_IDX_IIN 2u  /* PB0 → ADC1_IN15 */
#define ADC1_IDX_IOUT 3u /* PC1 → ADC1_IN17 */

static volatile uint16_t s_adc1_dma_buf[ADC1_DMA_CH_COUNT];
static volatile uint16_t s_adc2_dma_buf[1]; /* [0] = VIN */

static DMA_HandleTypeDef s_hdma_adc1;
static DMA_HandleTypeDef s_hdma_adc2;

/* ====================================================================
 * Non-blocking UART TX ring buffer
 *
 * PS_UART_TxEnqueue() - safe to call from ISR or task context; copies
 *   bytes into the ring buffer and fires DMA if idle.
 * HAL_UART_TxCpltCallback() - called when DMA segment completes;
 *   advances the tail pointer and kicks the next segment.
 *
 * Buffer size must be a power of 2.
 * ==================================================================== */

#define UART_TX_BUF_SIZE 512u
_Static_assert((UART_TX_BUF_SIZE & (UART_TX_BUF_SIZE - 1u)) == 0u,
               "UART_TX_BUF_SIZE must be a power of 2");

static uint8_t s_uart_txbuf[UART_TX_BUF_SIZE];
static volatile uint16_t s_uart_tx_head = 0u; /* write index */
static volatile uint16_t s_uart_tx_tail = 0u; /* DMA read index */
static volatile bool s_uart_dma_busy = false;
static volatile uint16_t s_uart_dma_len = 0u;

static void PS_UART_TxKick(void);

void PS_UART_TxEnqueue(const char *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++)
  {
    uint16_t next = (s_uart_tx_head + 1u) & (UART_TX_BUF_SIZE - 1u);
    if (next == s_uart_tx_tail)
      break; /* buffer full – drop remaining bytes rather than block */
    s_uart_txbuf[s_uart_tx_head] = (uint8_t)data[i];
    s_uart_tx_head = next;
  }
  PS_UART_TxKick();
}

/* Start DMA for the next contiguous segment of the ring buffer. */
static void PS_UART_TxKick(void)
{
  if (s_uart_dma_busy)
    return;
  uint16_t head = s_uart_tx_head;
  uint16_t tail = s_uart_tx_tail;
  if (head == tail)
    return; /* empty */
  /* Transmit up to the wrap boundary in one DMA shot. */
  uint16_t len = (head > tail) ? (head - tail)
                               : (UART_TX_BUF_SIZE - tail);
  s_uart_dma_len = len;
  s_uart_dma_busy = true;
  HAL_UART_Transmit_DMA(&hlpuart1, &s_uart_txbuf[tail], len);
}

/* HAL weak override – drain ring buffer after each DMA TX completion. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart != &hlpuart1)
    return;
  s_uart_tx_tail = (s_uart_tx_tail + s_uart_dma_len) & (UART_TX_BUF_SIZE - 1u);
  s_uart_dma_busy = false;
  PS_UART_TxKick(); /* send any remaining bytes */
}

/* ---- Static state ---- */

// Unified control configuration (PI and limits)
static PS_ControlCfg s_ctrl = {
    .vout_ref_V = 8.0f,
    .iout_ref_A = 3.0f,
    .il_peak_A_min = 0.1f,
    .il_peak_A_max = 20.0f,
    .slope_A_per_s = 6000.0f,
    .kp_v = 0.05f,
    .ki_v = 200.0f};
static bool s_enabled = false;
static volatile PS_Mode s_mode = PS_MODE_OFF;

// ADC scaling factors — initialized in PS_Init()
static PS_Scaling s_scaling = {0};

// Latest measurement snapshot (updated in PID ISR, read by PS_GetMeas)
static volatile PS_Meas s_meas = {0};

// Slope compensation and bootstrap refresh control
static float s_slope_comp_A_per_s = 10000;
static uint16_t s_bootstrap_refresh_every_n = 2;
static uint16_t s_bootstrap_refresh_counter = 0;
static float s_il_pk_ref_A = 1.0f;

// Low-speed timer (TIM7) used to IRQ-step the slope compensation
static bool s_tim7_inited = false;
static volatile uint16_t s_slope_steps_remaining = 0;
static float s_slope_step_us = 10.0f;

// Track which HRTIM timer is PWM vs refresh, and current period/refresh ticks
static uint32_t s_pwm_timer_idx = HRTIM_TIMERINDEX_TIMER_A;
static uint32_t s_refresh_timer_idx = HRTIM_TIMERINDEX_TIMER_B;
static uint16_t s_hrtim_period = 0;
static uint16_t s_refresh_ticks_cfg = 1;
static uint32_t s_hrtim_ticks_per_us = 0;
static volatile bool s_refresh_pulse_next = true;

// PID state for voltage loop (runs in TIM6 ISR)
static bool s_tim6_inited = false;
static volatile bool s_pid_running = false;
static volatile float s_pid_vout_ref = 5.0f;
static volatile float s_pid_iout_limit = 3.0f;
static volatile float s_pid_integrator = 0.0f;
static volatile float s_pid_error_prev = 0.0f;
static volatile float s_pid_softstart_ref = 0.0f;
static volatile bool s_pid_softstart_done = false;
#define PID_SOFTSTART_RATE_V_PER_S 2000.0f

// Periodic telemetry throttle (every N PID cycles)
#define MEAS_PRINT_INTERVAL 100000u
static uint16_t s_meas_print_counter = 0;

// Thresholds
#define VBUS_ON_THRESHOLD_MV 4000u
#define MODE_BUCK_MARGIN 1.10f
#define MODE_BOOST_MARGIN 0.90f

// Inductor current trip conversion defaults
#ifndef IL_SHUNT_OHM
#define IL_SHUNT_OHM (0.005f)
#endif
#ifndef IL_AMP_GAIN
#define IL_AMP_GAIN (50.0f)
#endif

#define IOUT_SHUNT_OHM (0.012f)
#define IIN_SHUNT_OHM (0.005f)

/* ---- DAC conversion helper ---- */

static uint32_t PS_IL_A_to_DAC12(float A)
{
  float v = A * IL_SHUNT_OHM * IL_AMP_GAIN;
  if (v < 0.0f)
    v = 0.0f;
  if (v > 3.3f)
    v = 3.3f;
  uint32_t code = (uint32_t)((v / 3.3f) * 4095.0f);
  return (code > 4095u) ? 4095u : code;
}

/* ---- ADC DMA buffer read (ISR-safe, zero-latency) ---- */

/*
 * Returns the latest scaled reading for the given channel by indexing
 * directly into the DMA buffers.  Never starts a conversion, never blocks.
 *
 * Note: PS_CH_VIN (ADC2_IN17) and PS_CH_IOUT_AVG (ADC1_IN17) share the
 * channel-number constant ADC_CHANNEL_17.  VIN is always on ADC2, so callers
 * that need VIN should read s_adc2_dma_buf[0] directly (as PS_StartClosedLoop
 * does).  PS_ReadAndScale maps channel 17 to ADC1_IDX_IOUT (IOUT_AVG).
 */
static float PS_ReadAndScale(uint32_t adc_channel, float scale_factor)
{
  uint16_t raw;
  if (adc_channel == PS_CH_VOUT)
    raw = s_adc1_dma_buf[ADC1_IDX_VOUT];
  else if (adc_channel == PS_CH_IL_INSTANT)
    raw = s_adc1_dma_buf[ADC1_IDX_IL];
  else if (adc_channel == PS_CH_IIN_AVG)
    raw = s_adc1_dma_buf[ADC1_IDX_IIN];
  else
    raw = s_adc1_dma_buf[ADC1_IDX_IOUT]; /* PS_CH_IOUT_AVG */
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

/* ====================================================================
 * ADC DMA initialization helpers
 *
 * Called from PS_Init() after main.c's MX_ADC1_Init() / MX_ADC2_Init()
 * have already run.  We reconfigure both ADCs for continuous scanning
 * with DMA-circular mode so the buffers are always up to date.
 *
 * DMA channel assignments (not used by main.c):
 *   DMA1 CH5 → ADC1   (DMAMUX request = DMA_REQUEST_ADC1)
 *   DMA1 CH6 → ADC2   (DMAMUX request = DMA_REQUEST_ADC2)
 * ==================================================================== */

static void PS_ADC1_DMA_Init(void)
{
  /* Stop any conversion that MX_ADC1_Init may have started */
  HAL_ADC_Stop(&hadc1);

  /* Reconfigure ADC1 for 4-channel scan + continuous + DMA */
  hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.NbrOfConversion = ADC1_DMA_CH_COUNT;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
    return;

  /* Channel ranks */
  ADC_ChannelConfTypeDef ch = {0};
  ch.SamplingTime = ADC_SAMPLETIME_12CYCLES_5;
  ch.SingleDiff = ADC_SINGLE_ENDED;
  ch.OffsetNumber = ADC_OFFSET_NONE;
  ch.Offset = 0;

  ch.Channel = PS_CH_VOUT;
  ch.Rank = ADC_REGULAR_RANK_1;
  HAL_ADC_ConfigChannel(&hadc1, &ch);
  ch.Channel = PS_CH_IL_INSTANT;
  ch.Rank = ADC_REGULAR_RANK_2;
  HAL_ADC_ConfigChannel(&hadc1, &ch);
  ch.Channel = PS_CH_IIN_AVG;
  ch.Rank = ADC_REGULAR_RANK_3;
  HAL_ADC_ConfigChannel(&hadc1, &ch);
  ch.Channel = PS_CH_IOUT_AVG;
  ch.Rank = ADC_REGULAR_RANK_4;
  HAL_ADC_ConfigChannel(&hadc1, &ch);

  /* DMA1 Channel 5 for ADC1 – circular, half-word, periph→mem */
  s_hdma_adc1.Instance = DMA1_Channel5;
  s_hdma_adc1.Init.Request = DMA_REQUEST_ADC1;
  s_hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
  s_hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
  s_hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
  s_hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  s_hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  s_hdma_adc1.Init.Mode = DMA_CIRCULAR;
  s_hdma_adc1.Init.Priority = DMA_PRIORITY_LOW;
  HAL_DMA_Init(&s_hdma_adc1);
  __HAL_LINKDMA(&hadc1, DMA_Handle, s_hdma_adc1);

  /* No NVIC for ADC DMA — we only read the buffer, never need the callback */

  HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_adc1_dma_buf, ADC1_DMA_CH_COUNT);
}

static void PS_ADC2_DMA_Init(void)
{
  HAL_ADC_Stop(&hadc2);

  /* ADC2: single channel (VIN), continuous, DMA circular */
  hadc2.Init.ContinuousConvMode = ENABLE;
  hadc2.Init.DMAContinuousRequests = ENABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
    return;

  /* Channel 17 = VIN on ADC2 — already set by MX_ADC2_Init, no change needed */

  /* DMA1 Channel 6 for ADC2 */
  s_hdma_adc2.Instance = DMA1_Channel6;
  s_hdma_adc2.Init.Request = DMA_REQUEST_ADC2;
  s_hdma_adc2.Init.Direction = DMA_PERIPH_TO_MEMORY;
  s_hdma_adc2.Init.PeriphInc = DMA_PINC_DISABLE;
  s_hdma_adc2.Init.MemInc = DMA_MINC_ENABLE;
  s_hdma_adc2.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  s_hdma_adc2.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  s_hdma_adc2.Init.Mode = DMA_CIRCULAR;
  s_hdma_adc2.Init.Priority = DMA_PRIORITY_LOW;
  HAL_DMA_Init(&s_hdma_adc2);
  __HAL_LINKDMA(&hadc2, DMA_Handle, s_hdma_adc2);

  HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED);
  HAL_ADC_Start_DMA(&hadc2, (uint32_t *)s_adc2_dma_buf, 1);
}

/* ---- Public API: Init / Enable ---- */

void PS_Init(void)
{
  /* Initialize ADC scaling factors */
  s_scaling.vin_gain = 3.3f / 4095.0f * (float)(PS_VSENSE_RA + PS_VSENSE_RB) / (float)PS_VSENSE_RB;
  s_scaling.vout_gain = 3.3f / 4095.0f * (float)(PS_VSENSE_RA + PS_VSENSE_RB) / (float)PS_VSENSE_RB;
  s_scaling.iin_gain = 3.3f / 4095.0f / (IIN_SHUNT_OHM * IL_AMP_GAIN);
  s_scaling.iout_gain = 3.3f / 4095.0f / (IOUT_SHUNT_OHM * IL_AMP_GAIN);
  s_scaling.il_gain = 3.3f / 4095.0f / (IL_SHUNT_OHM * IL_AMP_GAIN);
  s_scaling.adc_fullscale = 4095;

  PS_TCPP_EnablePinInit();
  PS_TCPP_FLG_Init();

  /* Start DMA-backed ADC scanning (hadc1/hadc2 already init'd by main.c) */
  PS_ADC1_DMA_Init();
  PS_ADC2_DMA_Init();

  /* Start fast analog inner loop: DAC3 CH1 → COMP1 inverting; COMP1 out → HRTIM EEV4 */
  if (HAL_DAC_Start(&hdac3, DAC_CHANNEL_1) == HAL_OK)
    HAL_DAC_SetValue(&hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R, PS_IL_A_to_DAC12(1.0f));
  HAL_COMP_Start(&hcomp1);

  s_enabled = false;
  s_mode = PS_MODE_OFF;

  /* Precompute HRTIM ticks/us for slope timing */
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
  s_pid_vout_ref = s_ctrl.vout_ref_V;
  s_pid_iout_limit = s_ctrl.iout_ref_A;
  if (s_ctrl.slope_A_per_s >= 0.0f)
    s_slope_comp_A_per_s = s_ctrl.slope_A_per_s;
}

bool PS_GetMeas(PS_Meas *out)
{
  if (!out)
    return false;
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
  s_pid_vout_ref = s_ctrl.vout_ref_V;
  s_pid_iout_limit = s_ctrl.iout_ref_A;
}

bool PS_GetMeasurements_mV_mA(uint16_t *vbus_mv, int16_t *iout_ma)
{
  float vout_V = PS_ReadAndScale(PS_CH_VOUT, s_scaling.vout_gain);
  if (vbus_mv)
  {
    uint32_t v = (uint32_t)(vout_V * 1000.0f);
    *vbus_mv = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
  }

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

/**
 * This applies a single step of slope compensation while making sure the value written to the DAC stays clamped within safe paramaters when duty cycle nears 100%
 * This takes reads a value from the DAC and subtracts off ramp_A to clamp the peak amp compensation for duty cycle nearing 100% duty
 *
 *
 *
 */
static void PS_ApplySlopeCompensation(float on_time_us)
{
  if (s_slope_comp_A_per_s == 0.0f)
    return;

  float ramp_A = s_slope_comp_A_per_s * (on_time_us / 1e6f);
  float compensated = s_il_pk_ref_A - ramp_A;
  if (compensated < s_ctrl.il_peak_A_min)
    compensated = s_ctrl.il_peak_A_min;
  if (compensated > s_ctrl.il_peak_A_max)
    compensated = s_ctrl.il_peak_A_max;

  uint32_t code = PS_IL_A_to_DAC12(compensated);
  if (HAL_DAC_GetState(&hdac3) == HAL_DAC_STATE_RESET)
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);
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
  LL_TIM_SetAutoReload(TIM7, 100);
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
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1, &out);
  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2, &out);
}

static void PS_ConfigOutputForBoostPWM(uint32_t timerIdx, uint16_t period, uint16_t cmp, bool include_eev)
{
  HRTIM_CompareCfgTypeDef cmpCfg = {0};
  cmpCfg.CompareValue = cmp;
  HAL_HRTIM_WaveformCompareConfig(&hhrtim1, timerIdx, HRTIM_COMPAREUNIT_1, &cmpCfg);

  HRTIM_OutputCfgTypeDef out = {0};
  out.Polarity = HRTIM_OUTPUTPOLARITY_HIGH;
  out.SetSource = HRTIM_OUTPUTSET_TIMCMP1 | (include_eev ? HRTIM_OUTPUTSET_EEV_4 : 0);
  out.ResetSource = HRTIM_OUTPUTRESET_TIMPER;
  out.IdleMode = HRTIM_OUTPUTIDLEMODE_NONE;
  out.IdleLevel = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
  out.FaultLevel = HRTIM_OUTPUTFAULTLEVEL_NONE;
  out.ChopperModeEnable = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
  out.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1, &out);
  HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                 (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2, &out);
}

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
                                   (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA1 : HRTIM_OUTPUT_TB1, &out);
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, timerIdx,
                                   (timerIdx == HRTIM_TIMERINDEX_TIMER_A) ? HRTIM_OUTPUT_TA2 : HRTIM_OUTPUT_TB2, &out);
  }
}

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

static void PS_EnableHrtimIRQs(void)
{
  __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A, HRTIM_TIM_IT_RST);
  __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B, HRTIM_TIM_IT_RST);

  NVIC_SetPriority(HRTIM1_TIMA_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0));
  NVIC_EnableIRQ(HRTIM1_TIMA_IRQn);

  NVIC_SetPriority(HRTIM1_TIMB_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0));
  NVIC_EnableIRQ(HRTIM1_TIMB_IRQn);
}

static void PS_StartHrtimOutputs(void)
{
  HAL_HRTIM_WaveformOutputStart(&hhrtim1,
                                HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
  HAL_HRTIM_WaveformCountStart(&hhrtim1,
                               HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
}

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
  uint16_t period = __HAL_HRTIM_GETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER);
  if (period == 0)
    period = 0xFFDF;

  uint16_t cmpA = (pwm_permille > 1000) ? period
                                        : (uint16_t)((uint32_t)period * pwm_permille / 1000u);
  uint16_t refresh_ticks = (refresh_permille > 1000) ? 1
                                                     : (uint16_t)((uint32_t)period * refresh_permille / 1000u);

  PS_ConfigDeadtime(deadtime_ticks);
  PS_ConfigOutputForBuckPWM(HRTIM_TIMERINDEX_TIMER_A, period, cmpA, true);
  PS_ConfigOutputForRefresh(HRTIM_TIMERINDEX_TIMER_B, period, refresh_ticks, false);
  PS_StartHrtimOutputs();

  s_pwm_timer_idx = HRTIM_TIMERINDEX_TIMER_A;
  s_refresh_timer_idx = HRTIM_TIMERINDEX_TIMER_B;
  s_hrtim_period = period;
  s_refresh_ticks_cfg = refresh_ticks;
  s_bootstrap_refresh_counter = (uint16_t)((s_bootstrap_refresh_every_n > 0)
                                               ? (s_bootstrap_refresh_every_n - 1)
                                               : 0);
  s_refresh_pulse_next = true;
  s_mode = PS_MODE_BUCK;

  PS_EnableHrtimIRQs();
}

/* ---- Public API: Boost mode ---- */

void PS_StartBoostMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks)
{
  uint16_t period = __HAL_HRTIM_GETPERIOD(&hhrtim1, HRTIM_TIMERINDEX_MASTER);
  if (period == 0)
    period = 0xFFDF;

  uint16_t cmpB = (pwm_permille > 1000) ? period
                                        : (uint16_t)((uint32_t)period * pwm_permille / 1000u);
  uint16_t refresh_ticks = (refresh_permille > 1000) ? 1
                                                     : (uint16_t)((uint32_t)period * refresh_permille / 1000u);

  PS_ConfigDeadtime(deadtime_ticks);
  PS_ConfigOutputForRefresh(HRTIM_TIMERINDEX_TIMER_A, period, refresh_ticks, false);
  PS_ConfigOutputForBoostPWM(HRTIM_TIMERINDEX_TIMER_B, period, cmpB, true);
  PS_StartHrtimOutputs();

  s_pwm_timer_idx = HRTIM_TIMERINDEX_TIMER_B;
  s_refresh_timer_idx = HRTIM_TIMERINDEX_TIMER_A;
  s_hrtim_period = period;
  s_refresh_ticks_cfg = refresh_ticks;
  s_bootstrap_refresh_counter = (uint16_t)((s_bootstrap_refresh_every_n > 0)
                                               ? (s_bootstrap_refresh_every_n - 1)
                                               : 0);
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
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);
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
  PS_Stop();

  s_ctrl.vout_ref_V = vout_ref_V;
  s_ctrl.iout_ref_A = iout_limit_A;
  s_pid_vout_ref = vout_ref_V;
  s_pid_iout_limit = iout_limit_A;

  /* Read VIN from ADC2 DMA buffer (populated by PS_ADC2_DMA_Init in PS_Init) */
  float vin_V = (float)s_adc2_dma_buf[0] * s_scaling.vin_gain;

  uint32_t vin_mv = (uint32_t)(vin_V * 1000.0f);
  uint32_t vout_target_mv = (uint32_t)(vout_ref_V * 1000.0f);

  PS_SetCurrentTrip_A(s_ctrl.il_peak_A_min);
  PS_SetSlopeCompensation(s_ctrl.slope_A_per_s);
  PS_SetBootstrapRefreshPeriod(2);

  uint16_t dt_ticks = 20;

  if (vin_mv > (uint32_t)(vout_target_mv * MODE_BUCK_MARGIN))
  {
    PS_StartBuckMode(500, 50, dt_ticks);
  }
  else if (vin_mv < (uint32_t)(vout_target_mv * MODE_BOOST_MARGIN))
  {
    PS_StartBoostMode(500, 50, dt_ticks);
  }
  else
  {
    /* Vin ≈ Vout: buck near pass-through until PID settles */
    PS_StartBuckMode(990, 50, dt_ticks);
  }

  PS_Enable(true);
  PS_PID_Start();
}

/* ---- IRQ handlers ---- */

void HRTIM1_TIMA_IRQHandler(void)
{
  HAL_HRTIM_IRQHandler(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A);
}

void HRTIM1_TIMB_IRQHandler(void)
{
  HAL_HRTIM_IRQHandler(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B);
}

void HAL_HRTIM_CounterResetCallback(HRTIM_HandleTypeDef *h, uint32_t TimerIdx)
{
  if (h != &hhrtim1)
    return;
  if (s_hrtim_period == 0)
    return;

  if (TimerIdx == s_pwm_timer_idx)
  {
    bool do_refresh = PS_ShouldRefreshBootstrap();
    s_refresh_pulse_next = do_refresh;
  }
}

/* ---- TIM6 ISR: PI voltage loop (20 kHz) ---- */

void TIM6_DAC_IRQHandler(void)
{
  if (!LL_TIM_IsActiveFlag_UPDATE(TIM6))
    return;
  LL_TIM_ClearFlag_UPDATE(TIM6);

  if (!s_pid_running)
    return;

  /* ---- Sample measurements from DMA buffers (zero latency, no blocking) ---- */
  s_meas.vout = (float)s_adc1_dma_buf[ADC1_IDX_VOUT] * s_scaling.vout_gain;
  s_meas.vin = (float)s_adc2_dma_buf[0] * s_scaling.vin_gain;
  s_meas.il_inst = (float)s_adc1_dma_buf[ADC1_IDX_IL] * s_scaling.il_gain;
  s_meas.iin_avg = (float)s_adc1_dma_buf[ADC1_IDX_IIN] * s_scaling.iin_gain;
  s_meas.iout_avg = (float)s_adc1_dma_buf[ADC1_IDX_IOUT] * s_scaling.iout_gain;

  /* ---- Periodic telemetry (non-blocking ring buffer) ---- */
  if (++s_meas_print_counter >= MEAS_PRINT_INTERVAL)
  {
    s_meas_print_counter = 0;
    char buf[72];
    int len = snprintf(buf, sizeof(buf),
                       "[MEAS] Vout=%umV Vin=%umV Iout=%umA\r\n",
                       (uint32_t)(s_meas.vout * 1000.0f),
                       (uint32_t)(s_meas.vin * 1000.0f),
                       (uint32_t)(s_meas.iout_avg * 1000.0f));
    if (len > 0)
      PS_UART_TxEnqueue(buf, (uint16_t)len);
  }

  /* ---- Soft-start ramp ---- */
  const float Ts = 1.0f / (float)PS_PID_LOOP_HZ;
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

  /* ---- PI voltage loop ---- */
  float error = target - s_meas.vout;

  s_pid_integrator += s_ctrl.ki_v * Ts * error;
  if (s_pid_integrator > s_ctrl.il_peak_A_max)
    s_pid_integrator = s_ctrl.il_peak_A_max;
  if (s_pid_integrator < 0.0f)
    s_pid_integrator = 0.0f;

  float il_ref = s_ctrl.kp_v * error + s_pid_integrator;

  if (il_ref < s_ctrl.il_peak_A_min)
    il_ref = s_ctrl.il_peak_A_min;
  if (il_ref > s_ctrl.il_peak_A_max)
    il_ref = s_ctrl.il_peak_A_max;

  /* Output current limit (CC mode): TODO — sample IOUT and reduce il_ref if needed */
  (void)s_pid_iout_limit;

  /* Update fast inner-loop DAC threshold */
  PS_SetCurrentTrip_A(il_ref);

  s_pid_error_prev = error;
}

/* ---- TIM7 ISR: slope compensation step timer ---- */

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
        PS_SlopeTimer_Stop();
    }
  }
}
