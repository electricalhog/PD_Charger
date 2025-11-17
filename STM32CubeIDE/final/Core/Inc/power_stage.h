/*
 * power_stage.h - Buck-Boost power stage HAL control
 */
#ifndef POWER_STAGE_H
#define POWER_STAGE_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

// Switching frequency used to size HRTIM period (main.c uses this)
#ifndef SWITCHING_FREQUENCY_HZ
#define SWITCHING_FREQUENCY_HZ 200000u // 200 kHz default
#endif

// ADC channel mapping (adjust to your board)
#define PS_ADCx                 ADC1
#define PS_ADC_CLK_ENABLE()     __HAL_RCC_ADC12_CLK_ENABLE()

// TODO: set these to the actual ADC channels
#define PS_CH_VIN               ADC_CHANNEL_1
#define PS_CH_VOUT              ADC_CHANNEL_2
#define PS_CH_IIN_AVG           ADC_CHANNEL_3
#define PS_CH_IOUT_AVG          ADC_CHANNEL_4
#define PS_CH_IL_INSTANT        ADC_CHANNEL_5   // inductor current (fast)

// Scaling (adjust to your dividers/shunts)
typedef struct {
  float vin_gain;    // V/LSB
  float vout_gain;   // V/LSB
  float iin_gain;    // A/LSB
  float iout_gain;   // A/LSB
  float il_gain;     // A/LSB (instantaneous)
  uint16_t adc_fullscale; // e.g., 4095 for 12-bit
} PS_Scaling;

typedef struct {
  float vin, vout, iin_avg, iout_avg, il_inst;
} PS_Meas;

typedef struct {
  float vout_ref_V;     // output voltage target
  float iout_ref_A;     // output current limit (CC fallback)
  float il_peak_A_min;  // minimum allowed IL peak
  float il_peak_A_max;  // maximum allowed IL peak
  float slope_A_per_s;  // digital slope compensation (approx)
  // PI gains for voltage loop
  float kp_v, ki_v;
} PS_ControlCfg;

#ifdef __cplusplus
extern "C" {
#endif

void PS_Init(void);
void PS_SetScaling(const PS_Scaling* s);
void PS_SetControlCfg(const PS_ControlCfg* c);

// Start closed-loop current-mode test
void PS_StartClosedLoop(float vout_ref_V, float iout_ref_A);

// Optional: manual PWM test already present
void PS_HRTIM_TestStart(uint16_t ta_cmp, uint16_t tb_cmp, uint16_t period);

// Latest measurements (thread-safe snapshot accessor)
bool PS_GetMeas(PS_Meas* out);

// To be called from IRQs (wired internally, exposed for clarity)
void PS_OnHrtimPeriod(void);       // per-cycle update
void PS_OnIlAnalogWatchdog(void);  // cycle-by-cycle trip

#ifdef __cplusplus
}
#endif

#endif // POWER_STAGE_H
