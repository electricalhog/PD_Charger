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

// PID voltage loop update rate (Hz). TIM6 ISR runs at this frequency.
#ifndef PS_PID_LOOP_HZ
#define PS_PID_LOOP_HZ 20000u // 20 kHz slow loop
#endif

// Converter operating mode
typedef enum
{
  PS_MODE_OFF = 0,
  PS_MODE_BUCK,
  PS_MODE_BOOST,
  // PS_MODE_BUCKBOOST,  // future: both legs PWM simultaneously
} PS_Mode;

// ADC channel mapping (STM32G474 board: PA0=VOUT, PA1=IL_INSTANT, PA4=VIN, PB0=IIN_AVG, PC1=IOUT_AVG)
#define PS_ADCx ADC1
#define PS_ADC_CLK_ENABLE() __HAL_RCC_ADC12_CLK_ENABLE()

#define PS_CH_VOUT ADC_CHANNEL_1       // PA0 → ADC1_IN1
#define PS_CH_IL_INSTANT ADC_CHANNEL_2 // PA1 → ADC1_IN2
#define PS_CH_VIN ADC_CHANNEL_17       // PA4 → ADC2_IN17 (on ADC2, not ADC1 - needs separate handling)
#define PS_CH_IIN_AVG ADC_CHANNEL_15   // PB0 → ADC1_IN15
#define PS_CH_IOUT_AVG ADC_CHANNEL_17  // PC1 → ADC1_IN17

#define PS_VSENSE_RA 100000
#define PS_VSENSE_RB 5820

// Scaling (adjust to your dividers/shunts)
typedef struct
{
  float vin_gain;         // V/LSB
  float vout_gain;        // V/LSB
  float iin_gain;         // A/LSB
  float iout_gain;        // A/LSB
  float il_gain;          // A/LSB (instantaneous)
  uint16_t adc_fullscale; // e.g., 4095 for 12-bit
} PS_Scaling;

typedef struct
{
  float vin, vout, iin_avg, iout_avg, il_inst;
} PS_Meas;

typedef struct
{
  float vout_ref_V;    // output voltage target
  float iout_ref_A;    // output current limit (CC fallback)
  float il_peak_A_min; // minimum allowed IL peak
  float il_peak_A_max; // maximum allowed IL peak
  float slope_A_per_s; // digital slope compensation (approx)
  // PI gains for voltage loop
  float kp_v, ki_v;
} PS_ControlCfg;

#ifdef __cplusplus
extern "C"
{
#endif

  void PS_Init(void);
  void PS_Enable(bool en);
  void PS_SetScaling(const PS_Scaling *s);
  void PS_SetControlCfg(const PS_ControlCfg *c);

  // Stop converter: disable HRTIM outputs and PID loop, safe shutdown
  void PS_Stop(void);

  // Get current operating mode
  PS_Mode PS_GetMode(void);

  // Start closed-loop voltage regulation with peak-current-mode inner loop.
  // Automatically selects buck or boost based on Vin vs Vout.
  void PS_StartClosedLoop(float vout_ref_V, float iout_limit_A);

  // Legacy/simple target setter used by USB-PD glue
  void PS_SetTargets_mV_mA(uint32_t vbus_mv, uint32_t iout_ma);

  // Optional: manual PWM test already present
  void PS_HRTIM_TestStart(uint16_t ta_cmp, uint16_t tb_cmp, uint16_t period);

  // Start buck mode: PWM on Timer A, Timer B held high with a short refresh window each cycle
  // pwm_permille: duty for the PWM leg (0..1000), refresh_permille: small off-time percentage for the pass-through leg (e.g., 10 = 1%)
  void PS_StartBuckMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks);

  // Start boost mode: PWM on Timer B, Timer A held high with a short refresh window each cycle
  void PS_StartBoostMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks);

  // Slope compensation for current mode stability (CCM, typically D > 0.5)
  // slope_A_per_s: ramp slope (amps per second) added during PWM on-time to DAC reference
  // Typical range: 0 (disabled) to ~10000 A/s depending on converter design
  void PS_SetSlopeCompensation(float slope_A_per_s);

  // Bootstrap refresh control: refresh pass-through leg every N PWM cycles (not every cycle)
  // refresh_every_n: number of PWM cycles between refresh pulses (1 = every cycle, 4 = every 4th, etc.)
  void PS_SetBootstrapRefreshPeriod(uint16_t refresh_every_n);

  // Latest measurements (thread-safe snapshot accessor)
  bool PS_GetMeas(PS_Meas *out);

  // To be called from IRQs (wired internally, exposed for clarity)
  void PS_OnHrtimPeriod(void);      // per-cycle update
  void PS_OnIlAnalogWatchdog(void); // cycle-by-cycle trip

  // Set instantaneous inductor current trip (A) for fast inner hardware loop
  // This programs DAC3 CH1 which is wired to COMP1 inverting input in this project.
  void PS_SetCurrentTrip_A(float il_peak_A);

  // Legacy/simple measurement helpers implemented in power_stage.c
  bool PS_GetMeasurements_mV_mA(uint16_t *vbus_mv, int16_t *iout_ma);
  bool PS_IsOn(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_STAGE_H
