/*
 * power_stage.h - Buck-Boost power stage HAL control
 */
#ifndef POWER_STAGE_H
#define POWER_STAGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t vbus_target_mv;   // requested VBUS in mV
  uint32_t iout_limit_ma;    // requested current limit in mA
  bool     enabled;          // converter enabled state
} PS_Config_t;

// Initialize GPIO/ADC/HRTIM glue needed by the power stage. Does not enable power.
void PS_Init(void);

// Enable/Disable the power stage output path (Type-C power switch + PWM).
void PS_Enable(bool en);

// Update targets (voltage/current). No ramp implemented yet.
void PS_SetTargets_mV_mA(uint32_t vbus_mv, uint32_t iout_ma);

// Read instantaneous measurements (VBUS in mV, Iout in mA). Returns false on failure.
bool PS_GetMeasurements_mV_mA(uint16_t* vbus_mv, int16_t* iout_ma);

// Query whether output is considered ON (based on enable and measured VBUS > threshold)
bool PS_IsOn(void);

// Test helpers for HRTIM switching waveforms (no control loop)
// duty in permille (0..1000), deadtime in timer ticks
void PS_HRTIM_TestStart(uint16_t dutyA_permille, uint16_t dutyB_permille, uint16_t deadtime_ticks);
void PS_HRTIM_TestStop(void);

#ifdef __cplusplus
}
#endif

#endif // POWER_STAGE_H
