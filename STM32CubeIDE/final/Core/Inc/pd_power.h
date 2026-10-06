/**
 * @file    pd_power.h
 * @brief   USB PD source power policy: which VBUS source feeds the port, which
 *          PDOs (SPR and EPR) are offered, and how a contract reaches the
 *          regulator.
 *
 * The ST stack (USBPD core V5.3, EPR capable) runs the protocol: EPR mode
 * entry, cable discovery over SOP', EPR_Source_Capabilities, keep-alive.
 * This module decides what is offered and whether a request can be met.
 *
 * VBUS path on the bench (2026-09-29): X-NUCLEO-SRC1M1 (TCPP0203) stacked on
 * the NUCLEO; its VIN is either the NUCLEO 5 V rail (PD_VBUS_SOURCE_BENCH_5V)
 * or the buck-boost output (PD_VBUS_SOURCE_REGULATOR).  The TCPP gate driver
 * closes the VBUS switch; the regulator sets the voltage behind it.
 *
 * Runtime knobs (volatile, written over SWD by `bu pd set`, applied by
 * pd_power_poll() which re-sends Source_Capabilities while attached):
 *   pd_vbus_source       where VBUS comes from
 *   pd_profile           BENCH_5V / SPR / EPR
 *   pd_vbus_path_max_mv  highest voltage the VBUS path may carry
 * The offered PDOs are the profile's list clipped to what the source can
 * produce and the path can carry, so a knob can only ever take PDOs away.
 */

#ifndef PD_POWER_H
#define PD_POWER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Configuration
 * =========================================================================*/

/** VBUS sources. */
#define PD_VBUS_SOURCE_BENCH_5V     0u  /**< SRC1M1 VIN from the NUCLEO 5 V rail: vSafe5V only   */
#define PD_VBUS_SOURCE_REGULATOR    1u  /**< SRC1M1 VIN from the buck-boost output               */

/** Offer profiles. */
#define PD_PROFILE_BENCH_5V         0u  /**< 5 V at PD_BENCH_5V_CURRENT_MA only                  */
#define PD_PROFILE_SPR              1u  /**< 5/9/15/20 V fixed                                    */
#define PD_PROFILE_EPR              2u  /**< SPR plus EPR 28/36/48 V fixed, EPR Mode Capable      */

/** Defaults at boot: the NUCLEO 5 V rail feeds the shield (bench, 2026-09-29). */
#define PD_VBUS_SOURCE_DEFAULT      PD_VBUS_SOURCE_BENCH_5V
#define PD_PROFILE_DEFAULT          PD_PROFILE_EPR

/**
 * PD_VBUS_PATH_MAX_MV_DEFAULT — highest VBUS the port hardware may carry.
 * 20 V while the X-NUCLEO-SRC1M1 carries VBUS: it is an SPR board (its VSENSE
 * divider, 200k/40k, saturates at 19.8 V and its protection is sized for
 * SPR).  Raise it only when EPR-rated protection carries VBUS.
 */
#define PD_VBUS_PATH_MAX_MV_DEFAULT 20000u

/**
 * Currents.  The spec (v0.1.2 §9.4) limits the advertised current to
 * 1.5 A; the NUCLEO 5 V rail comes from the ST-LINK USB port, so the bench
 * source offers 0.5 A.
 */
#define PD_SPR_MAX_CURRENT_MA       1500u
#define PD_EPR_MAX_CURRENT_MA       1500u
#define PD_BENCH_5V_CURRENT_MA      500u

/** Regulator settle wait before PS_RDY: tPSTransition is 450-550 ms (SPR),
 *  830-1020 ms (EPR); the stack waits for SetupNewPower to return. */
#define PD_SETTLE_TIMEOUT_SPR_MS    450u
#define PD_SETTLE_TIMEOUT_EPR_MS    900u

/** Maximum PDO counts (USBPD_MAX_NB_PDO / USBPD_MAX_NB_EPRPDO). */
#define PD_MAX_SPR_PDO              7u
#define PD_MAX_EPR_PDO              6u

/* =========================================================================
 * Runtime state
 * =========================================================================*/

extern volatile uint32_t pd_vbus_source;
extern volatile uint32_t pd_profile;
extern volatile uint32_t pd_vbus_path_max_mv;

/** Telemetry for `bu pd status` (read over SWD; written by the PD tasks). */
typedef struct
{
    uint32_t attached;          /**< 1 while a sink is attached                     */
    uint32_t contract_mv;       /**< Voltage of the accepted request, 0 = none      */
    uint32_t contract_ma;       /**< Operating current of the accepted request      */
    uint32_t contract_pos;      /**< Object position of the accepted request (1..)  */
    uint32_t epr_mode;          /**< 1 while in EPR mode                            */
    uint32_t vbus_mv;           /**< VBUS at the connector (SRC1M1 VSENSE)          */
    int32_t  ibus_ma;           /**< VBUS current (SRC1M1 ISENSE)                   */
    uint32_t n_spr_pdo;         /**< PDOs offered in Source_Capabilities            */
    uint32_t n_epr_pdo;         /**< EPR PDOs offered (positions 8..)               */
    uint32_t spr_pdo[PD_MAX_SPR_PDO];
    uint32_t epr_pdo[PD_MAX_EPR_PDO];
    uint32_t last_notify;       /**< Last USBPD_NotifyEventValue_TypeDef            */
    uint32_t n_notify;          /**< Notifications seen                             */
    uint32_t n_requests;        /**< Requests evaluated                             */
    uint32_t n_rejects;         /**< Requests rejected                              */
    uint32_t n_hard_resets;     /**< Hard resets seen (either direction)            */
    uint32_t n_epr_entries;     /**< EPR mode entries that succeeded                */
    uint32_t n_epr_fails;       /**< EPR mode entries that failed                   */
    uint32_t epr_last_action;   /**< Last EPR_Mode action/data (action << 8 | data) */
    uint32_t last_rdo;          /**< Last received RDO                              */
    uint32_t last_error;        /**< PD_ERR_* of the last refused setup             */
    uint32_t vconn_on;          /**< 1 while VCONN is sourced                       */
    uint32_t tick_ms;           /**< HAL tick of the last VBUS sample               */
    uint32_t tcpp_type;         /**< TCPP0203 type/version register                 */
    uint32_t tcpp_ack;          /**< ACK register: VCONN switch, GDP/GDC, power mode */
    uint32_t tcpp_flags;        /**< FLAG register: OCP/OVP/OTP, VBUS ok            */
    uint32_t tcpp_reads;        /**< Snapshots taken (read errors: bit 31 set)      */
} PdPowerStatus;

extern volatile PdPowerStatus pd_power_status;

#define PD_ERR_NONE             0u
#define PD_ERR_SOURCE_CANT      1u  /**< The VBUS source cannot produce the voltage */
#define PD_ERR_PATH_LIMIT       2u  /**< Above pd_vbus_path_max_mv                  */
#define PD_ERR_REGULATOR        3u  /**< Regulator did not start or is faulted      */
#define PD_ERR_SETTLE_TIMEOUT   4u  /**< V_out did not reach the target in time     */

/* =========================================================================
 * API
 * =========================================================================*/

/** Before MX_USBPD_Init(): stack settings (SOP', EPR), TCPP GPIO/EXTI and the
 *  ENABLE pin, ADC2 injected channels for VBUS sensing, PDO lists. */
void pd_power_init(void);

/** HAL 1 ms tick (TIM2 callback): VBUS/IBUS sample, one ADC2 injected pair. */
void pd_power_tick_1ms(void);

/** Default task, every 10 ms: apply knob changes (re-advertise). */
void pd_power_poll(void);

/** PDO lists as offered right now.  Copies up to max words, returns count. */
uint32_t pd_power_get_spr_pdos(uint32_t *dst, uint32_t max);
uint32_t pd_power_get_epr_pdos(uint32_t *dst, uint32_t max);

/** PDO at object position pos (1..7 SPR, 8.. EPR), 0 if none. */
uint32_t pd_power_pdo_at(uint32_t pos);

/** Source PDP ratings in W for Source_Capabilities_Extended. */
uint8_t pd_power_spr_pdp_w(void);
uint8_t pd_power_epr_pdp_w(void);

/** Bring VBUS behind the switch to vSafe5V (attach, hard reset recovery).
 *  Blocks up to PD_SETTLE_TIMEOUT_SPR_MS. Returns true when ready. */
bool pd_power_vbus_prepare(void);

/** Contract ended (detach, hard reset): regulator off. */
void pd_power_vbus_release(void);

/** New explicit contract: set the regulator and wait for it to settle.
 *  Called from SetupNewPower (PE task).  Returns true when VBUS is there. */
bool pd_power_set_contract(uint32_t pos, uint32_t mv, uint32_t ma, bool epr);

/** Event hooks from the DPM callbacks (telemetry only). */
void pd_power_on_attach(bool attached);
void pd_power_on_notify(uint32_t event);
void pd_power_on_epr_mode(uint32_t action, uint32_t data);
void pd_power_on_vconn(bool on);

#ifdef __cplusplus
}
#endif

#endif /* PD_POWER_H */
