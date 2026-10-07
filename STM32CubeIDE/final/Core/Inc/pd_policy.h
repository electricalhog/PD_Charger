/**
 * @file    pd_policy.h
 * @brief   Source PDO table, request evaluation, EPR decisions, and the
 *          pd_status telemetry block read over SWD by `bu pd status`.
 *
 * The PDO table is built at start-up from pd_bench_config.h, so the offered
 * capabilities do not depend on the CubeMX-generated usbpd_pdo_defs.h.
 * Object positions follow USB PD R3.2 §6.4.1: 1..7 are SPR PDOs, 8..13 are
 * EPR PDOs and only valid in an EPR_Request while in EPR mode.
 */

#ifndef PD_POLICY_H
#define PD_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PD_POLICY_MAX_SPR_PDO   7u
#define PD_POLICY_MAX_EPR_PDO   6u
#define PD_POLICY_FIRST_EPR_POS 8u

/** Outcome of pd_policy_evaluate_rdo(); also kept in pd_status. */
typedef enum
{
    PD_RDO_ACCEPT              = 0u,
    PD_RDO_BAD_POSITION        = 1u, /**< no PDO at that object position        */
    PD_RDO_EPR_OUTSIDE_EPR     = 2u, /**< position >= 8 while not in EPR mode    */
    PD_RDO_NOT_FIXED           = 3u, /**< only fixed PDOs are offered            */
    PD_RDO_OVER_CURRENT        = 4u  /**< operating/max current above the PDO    */
} PdRdoResult;

/** Contract derived from an accepted RDO. */
typedef struct
{
    uint32_t pdo;          /**< the PDO the RDO points to          */
    uint32_t position;     /**< object position, 1..13             */
    uint32_t voltage_mv;
    uint32_t operating_ma;
    uint32_t max_ma;
} PdContract;

#define PD_STATUS_MAGIC      0x54534450u   /* "PDST" little-endian */
#define PD_STATUS_VERSION    1u
#define PD_STATUS_EVENTS     32u

/**
 * pd_status — telemetry for `bu pd status`.  Every field is a 32-bit word
 * so the host decodes it without the ELF's struct layout; the order is part
 * of PD_STATUS_VERSION.  Written from the PD tasks and the default task,
 * read only by the debugger.
 */
typedef struct
{
    uint32_t magic;                /**< PD_STATUS_MAGIC                                  */
    uint32_t version;              /**< PD_STATUS_VERSION                                */
    uint32_t lib_epr;              /**< 1 if built against a USB-PD core with EPR        */
    uint32_t epr_offered;          /**< 1 if PDO 1 carries EPR Mode Capable              */
    uint32_t spr_pdo_count;
    uint32_t epr_pdo_count;
    uint32_t spr_pdo[PD_POLICY_MAX_SPR_PDO];
    uint32_t epr_pdo[PD_POLICY_MAX_EPR_PDO];
    uint32_t attach_count;
    uint32_t detach_count;
    uint32_t hard_reset_count;
    uint32_t contract_count;       /**< accepted requests that reached PS_RDY            */
    uint32_t last_rdo;
    uint32_t last_rdo_result;      /**< PdRdoResult of the last evaluated RDO            */
    uint32_t contract_mv;          /**< 0 = no explicit contract                         */
    uint32_t contract_ma;          /**< operating current of the contract                */
    uint32_t contract_position;
    uint32_t transition_ms;        /**< time the last transition took to come in range   */
    uint32_t transition_timeouts;  /**< transitions that never came in range             */
    uint32_t vbus_on_failures;     /**< attach with the regulator unable to start        */
    uint32_t fault_hard_resets;    /**< Hard Resets sent because the regulator faulted   */
    uint32_t epr_mode;             /**< 1 while in EPR mode                              */
    uint32_t epr_enter_requests;   /**< EPR_Mode (Enter) received                        */
    uint32_t epr_enter_succeeded;
    uint32_t epr_enter_failed;
    uint32_t epr_exits;
    uint32_t epr_enter_mode_replies; /**< DPM asked to accept an EPR entry          */
    uint32_t ev_count;             /**< total events; ring index = ev_count % EVENTS     */
    uint32_t ev_tick[PD_STATUS_EVENTS];  /**< HAL tick (ms) of each event                */
    uint32_t ev_code[PD_STATUS_EVENTS];  /**< USBPD_NotifyEventValue_TypeDef, or
                                              0x100 | CAD event for attach/detach        */
} PdStatus;

extern volatile PdStatus pd_status;

/** Event codes logged in addition to USBPD notifications. */
#define PD_EV_CAD_BASE        0x100u  /* 0x100 | USBPD_CAD_EVENT */
#define PD_EV_FAULT_HARDRESET 0x200u
#define PD_EV_TRANSITION_TIMEOUT 0x201u
#define PD_EV_VBUS_ON_FAILED  0x202u
#define PD_EV_SETUP_POWER     0x203u  /* PE called USBPD_DPM_SetupNewPower */
#define PD_EV_SETUP_POWER_ERR 0x204u  /* ... and it returned an error */
#define PD_EV_POWER_NOT_READY 0x205u  /* IsPowerReady answered DISABLE */

/** Build the PDO tables; call once before the PD stack runs. */
void pd_policy_init(void);

/** Copy the SPR PDOs (bytes) for USBPD_CORE_DATATYPE_SRC_PDO.  PD 2.0
 *  partners get PDO 1 without the PD 3.x-only bits. */
void pd_policy_get_spr_pdos(uint8_t *dst, uint32_t *size_bytes, bool pd2_partner);

/** Copy the EPR-only PDOs (positions 8..) for USBPD_CORE_DATATYPE_SRC_PDO_EPR. */
void pd_policy_get_epr_pdos(uint8_t *dst, uint32_t *size_bytes);

/** PDO at an object position (1..13), false if none. */
bool pd_policy_lookup(uint32_t position, uint32_t *pdo);

/** Evaluate a received RDO; on PD_RDO_ACCEPT fills *contract. */
PdRdoResult pd_policy_evaluate_rdo(uint32_t rdo, bool epr_mode, PdContract *contract);

/** True if the stage may enter EPR now (EPR enabled, regulator not faulted). */
bool pd_policy_epr_entry_allowed(void);

/** Highest voltage any PDO offers [mV]. */
uint32_t pd_policy_max_offered_mv(void);

/** Append one event to the pd_status ring. */
void pd_status_event(uint32_t code);

#ifdef __cplusplus
}
#endif

#endif /* PD_POLICY_H */
