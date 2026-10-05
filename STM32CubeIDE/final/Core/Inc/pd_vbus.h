/**
 * @file    pd_vbus.h
 * @brief   VBUS through this board's regulator and output switch, for the
 *          USB-PD stack (PD_OWNS_VBUS builds).
 *
 * pd_vbus.c implements the BSP_USBPD_PWR_* functions the ST stack calls
 * (prototypes in usbpd_pwr_user.h) on top of the regulator, replacing the
 * X-NUCLEO-SRC1M1 / TCPP03 BSP:
 *
 *   VBUSOn          regulator at 5 V; OUTPUT_EN closes once V_out regulates
 *   VBUSOff         regulator_stop(): output switch open, OUTPUT_DIS on
 *   SetVoltage_*    new regulator target (slewed by the PID ISR)
 *   VBUSGetVoltage  VD_MON while the output switch is closed; after it opens,
 *                   0 once PD_VBUS_DISCHARGE_MS has passed (no VBUS-side ADC)
 *   VCONN*          GPIO switches when PD_VCONN_ENABLE, else not supported
 *
 * The SRC1M1 BSP is not linked in these builds: it drives PC8 (the TCPP03
 * ENABLE on that shield, OUTPUT_EN on this board) and reprograms ADC1 + DMA
 * in VBUSInit, which the regulator's VD_MON scan owns.
 */

#ifndef PD_VBUS_H
#define PD_VBUS_H

#include <stdint.h>
#include <stdbool.h>
#include "pd_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PD_OWNS_VBUS

/** VBUS as the PD stack sees it [mV] (see the file comment). */
uint32_t pd_vbus_get_mv(void);

/**
 * pd_vbus_wait_in_range — Block (osDelay) until VBUS is within
 * PD_VBUS_TOLERANCE_PCT of target_mv with the output switch closed, or
 * timeout_ms passes.  Task context only (PE task, from SetupNewPower).
 * @return elapsed ms on success, or UINT32_MAX on timeout / regulator fault.
 */
uint32_t pd_vbus_wait_in_range(uint32_t target_mv, uint32_t timeout_ms);

/**
 * pd_vbus_poll — Default task, every 10 ms.  Tracks when the output switch
 * opens (for the vSafe0V estimate) and sends one Hard Reset when the
 * regulator latches a FAULT during an explicit contract, so the sink sees
 * a clean power removal instead of a contract with no VBUS.
 */
void pd_vbus_poll(void);

#endif /* PD_OWNS_VBUS */

#ifdef __cplusplus
}
#endif

#endif /* PD_VBUS_H */
