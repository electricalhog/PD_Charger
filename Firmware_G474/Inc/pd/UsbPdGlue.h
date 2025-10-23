#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called by STM32 USB-PD (UCPD) middleware when a Source contract is established
void PD_ContractEstablished(uint8_t port, uint32_t mv, uint32_t ma);

// Called when Source power should be disabled (detach, error, etc.)
void PD_SourceDisabled(uint8_t port);

#ifdef __cplusplus
}
#endif
