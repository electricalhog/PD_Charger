/*
 * power_stage.h - Buck-Boost power stage HAL control
 *
 * ADC pin assignments come from main.h (CubeMX-generated):
 *   VOUT_SENSE_Pin  / VOUT_SENSE_GPIO_Port  = PA0  → ADC1_IN1   (hadc1, rank 1)
 *   IL_SENSE_Pin    / IL_SENSE_GPIO_Port    = PA1  → ADC1_IN2   (hadc1, rank 2)
 *   IIN_SENSE_Pin   / IIN_SENSE_GPIO_Port   = PB0  → ADC1_IN15  (hadc1, rank 3)
 *   IOUT_SENSE_Pin  / IOUT_SENSE_GPIO_Port  = PC1  → ADC1_IN7   (hadc1, rank 4)
 *   VIN_SENSE_Pin   / VIN_SENSE_GPIO_Port   = PA4  → ADC2_IN17  (hadc2, rank 1)
 */
#ifndef POWER_STAGE_H
#define POWER_STAGE_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

#define PS_CH_VOUT ADC_CHANNEL_1       // PA0 → ADC1_IN1
#define PS_CH_IL_INSTANT ADC_CHANNEL_2 // PA1 → ADC1_IN2
#define PS_CH_IOUT_AVG ADC_CHANNEL_7   // PC1 → ADC1_IN7
#define PS_CH_IIN_AVG ADC_CHANNEL_15   // PB0 → ADC1_IN15
#define PS_CH_VIN ADC_CHANNEL_17       // PA4 → ADC2_IN17 (on ADC2, not ADC1 - needs separate handling)

/* Switching frequency used to size HRTIM period (referenced by main.c USER CODE). */
#ifndef SWITCHING_FREQUENCY_HZ
#define SWITCHING_FREQUENCY_HZ 200000u /* 200 kHz default */
#endif

/* PID voltage loop update rate (Hz) — TIM6 ISR runs at this frequency. */
#ifndef PS_PID_LOOP_HZ
#define PS_PID_LOOP_HZ 20000u /* 20 kHz */
#endif

/* Voltage-sense resistor divider (Rtop / Rbottom in ohms). */
#define PS_VSENSE_RA 100000
#define PS_VSENSE_RB 5820

/* Converter operating mode */
typedef enum
{
  PS_MODE_OFF = 0,
  PS_MODE_BUCK,
  PS_MODE_BOOST,
} PS_Mode;

/* ADC scaling gains — populated by PS_Init(), overridable via PS_SetScaling(). */
typedef struct
{
  float vin_gain;         /* V / LSB  */
  float vout_gain;        /* V / LSB  */
  float iin_gain;         /* A / LSB  */
  float iout_gain;        /* A / LSB  */
  float il_gain;          /* A / LSB  (instantaneous inductor current) */
  uint16_t adc_fullscale; /* 4095 for 12-bit */
} PS_Scaling;

typedef struct
{
  float vin, vout, iin_avg, iout_avg, il_inst;
} PS_Meas;

typedef struct
{
  float vout_ref_V;    /* output voltage target              */
  float iout_ref_A;    /* output current limit (CC fallback) */
  float il_peak_A_min; /* minimum allowed IL peak            */
  float il_peak_A_max; /* maximum allowed IL peak            */
  float slope_A_per_s; /* digital slope compensation         */
  float kp_v, ki_v;    /* PI gains for voltage loop          */
} PS_ControlCfg;

#ifdef __cplusplus
extern "C"
{
#endif

  /* Initialisation / enable */
  void PS_Init(void);
  void PS_Enable(bool en);
  void PS_SetScaling(const PS_Scaling *s);
  void PS_SetControlCfg(const PS_ControlCfg *c);

  /* Mode control */
  PS_Mode PS_GetMode(void);
  void PS_Stop(void);

  /* Closed-loop regulation (auto-selects buck / boost from Vin vs Vout) */
  void PS_StartClosedLoop(float vout_ref_V, float iout_limit_A);

  /* Manual PWM test */
  void PS_HRTIM_TestStart(uint16_t dutyA_permille, uint16_t dutyB_permille, uint16_t deadtime_ticks);
  void PS_HRTIM_TestStop(void);

  /* Individual mode entry */
  void PS_StartBuckMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks);
  void PS_StartBoostMode(uint16_t pwm_permille, uint16_t refresh_permille, uint16_t deadtime_ticks);

  /* Inner-loop current trip (programs DAC3 CH1 → COMP1 inverting input) */
  void PS_SetCurrentTrip_A(float il_peak_A);

  /* Slope compensation and bootstrap refresh tuning */
  void PS_SetSlopeCompensation(float slope_A_per_s);
  void PS_SetBootstrapRefreshPeriod(uint16_t refresh_every_n);

  /* Target setters used by the USB-PD glue layer */
  void PS_SetTargets_mV_mA(uint32_t vbus_mv, uint32_t iout_ma);

  /* Measurement accessors */
  bool PS_GetMeas(PS_Meas *out);
  bool PS_GetMeasurements_mV_mA(uint16_t *vbus_mv, int16_t *iout_ma);
  bool PS_IsOn(void);

  /* Non-blocking UART telemetry — safe to call from ISR or task context */
  void PS_UART_TxEnqueue(const char *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* POWER_STAGE_H */
