#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal TCPC abstraction (e.g., STUSB1602)
// Real implementation should use ST's USB-PD middleware or TCPC driver.

typedef void (*pd_contract_cb_t)(float v_V, float i_A);

void USBPD_Port_Init(pd_contract_cb_t cb);
void USBPD_Port_Task(void); // call in main loop or 1kHz tick
void USBPD_Port_RequestPDOs(const uint32_t *src_pdos, uint8_t count);

#ifdef __cplusplus
}
#endif
