/**
 * @file    pd_policy.c
 * @brief   Source PDO table, request evaluation, EPR decisions, telemetry.
 *
 * See pd_policy.h.  The limits come from pd_bench_config.h.
 */

#include "pd_policy.h"
#include "pd_bench_config.h"
#include "regulator.h"
#include "main.h"
#include "usbpd_def.h"
#include <string.h>

volatile PdStatus pd_status;

static uint32_t spr_pdo[PD_POLICY_MAX_SPR_PDO];
static uint32_t spr_count;
static uint32_t epr_pdo[PD_POLICY_MAX_EPR_PDO];
static uint32_t epr_count;

/* PDO 1 bits that only exist from PD 3.0 on (cleared for a PD 2.0 partner). */
#if defined(USBPDCORE_EPR)
#define PDO1_PD3_ONLY_BITS (USBPD_PDO_SRC_FIXED_UNCHUNK_SUPPORT_Msk | USBPD_PDO_SRC_FIXED_EPR_SUPPORT_Msk)
#else
#define PDO1_PD3_ONLY_BITS (USBPD_PDO_SRC_FIXED_UNCHUNK_SUPPORT_Msk)
#endif

/* RDO fields (USB PD R3.2 Table 6.22, Fixed and Variable RDO) */
#define RDO_POSITION(rdo)           (((rdo) >> 28) & 0xFu)
#define RDO_CAPABILITY_MISMATCH(rdo) (((rdo) >> 26) & 0x1u)
#define RDO_OPERATING_10MA(rdo)     (((rdo) >> 10) & 0x3FFu)
#define RDO_MAX_OPERATING_10MA(rdo) ((rdo) & 0x3FFu)

static uint32_t fixed_pdo(uint32_t mv, uint32_t ma)
{
    return USBPD_PDO_TYPE_FIXED
         | (((mv / 50u) << USBPD_PDO_SRC_FIXED_VOLTAGE_Pos) & USBPD_PDO_SRC_FIXED_VOLTAGE_Msk)
         | ((ma / 10u) & USBPD_PDO_SRC_FIXED_MAX_CURRENT_Msk);
}

static uint32_t pdo_mv(uint32_t pdo)
{
    return ((pdo & USBPD_PDO_SRC_FIXED_VOLTAGE_Msk) >> USBPD_PDO_SRC_FIXED_VOLTAGE_Pos) * 50u;
}

static uint32_t pdo_max_10ma(uint32_t pdo)
{
    return pdo & USBPD_PDO_SRC_FIXED_MAX_CURRENT_Msk;
}

void pd_policy_init(void)
{
    spr_count = 0u;
    epr_count = 0u;

    /* PDO 1: vSafe5V.  The capability bits live only here.  Dual-Role Data
     * is not advertised: DPM_USER_Settings has PE_DataSwap = FALSE, so a
     * DR_Swap would be answered Not_Supported anyway.                     */
    uint32_t pdo1 = fixed_pdo(5000u, PD_SRC_MAX_CURRENT_MA)
                  | USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL
                  | USBPD_PDO_SRC_FIXED_UNCHUNK_NOT_SUPPORTED
                  | USBPD_PDO_SRC_FIXED_DRD_NOT_SUPPORTED
                  | USBPD_PDO_SRC_FIXED_USBCOMM_NOT_SUPPORTED
                  | USBPD_PDO_SRC_FIXED_EXT_POWER_NOT_AVAILABLE
                  | USBPD_PDO_SRC_FIXED_USBSUSPEND_NOT_SUPPORTED
                  | USBPD_PDO_SRC_FIXED_DRP_NOT_SUPPORTED;
#if defined(USBPDCORE_EPR)
    if (PD_EPR_ENABLE)
    {
        pdo1 |= USBPD_PDO_SRC_FIXED_EPR_SUPPORTED;
    }
#endif
    spr_pdo[spr_count++] = pdo1;

    if (PD_SRC_PDO_9V_ENABLE)  { spr_pdo[spr_count++] = fixed_pdo( 9000u, PD_SRC_MAX_CURRENT_MA); }
    if (PD_SRC_PDO_15V_ENABLE) { spr_pdo[spr_count++] = fixed_pdo(15000u, PD_SRC_MAX_CURRENT_MA); }
    if (PD_SRC_PDO_20V_ENABLE) { spr_pdo[spr_count++] = fixed_pdo(20000u, PD_SRC_MAX_CURRENT_MA); }

#if defined(USBPDCORE_EPR)
    if (PD_EPR_ENABLE && PD_EPR_PDO_28V_ENABLE)
    {
        epr_pdo[epr_count++] = fixed_pdo(28000u, PD_EPR_28V_CURRENT_MA);
    }
#endif

    memset((void *)&pd_status, 0, sizeof(pd_status));
    pd_status.magic         = PD_STATUS_MAGIC;
    pd_status.version       = PD_STATUS_VERSION;
#if defined(USBPDCORE_EPR)
    pd_status.lib_epr       = 1u;
    pd_status.epr_offered   = PD_EPR_ENABLE ? 1u : 0u;
#endif
    pd_status.spr_pdo_count = spr_count;
    pd_status.epr_pdo_count = epr_count;
    for (uint32_t i = 0u; i < spr_count; i++) { pd_status.spr_pdo[i] = spr_pdo[i]; }
    for (uint32_t i = 0u; i < epr_count; i++) { pd_status.epr_pdo[i] = epr_pdo[i]; }
}

void pd_policy_get_spr_pdos(uint8_t *dst, uint32_t *size_bytes, bool pd2_partner)
{
    for (uint32_t i = 0u; i < spr_count; i++)
    {
        uint32_t pdo = spr_pdo[i];
        if (i == 0u && pd2_partner)
        {
            pdo &= ~PDO1_PD3_ONLY_BITS;
        }
        memcpy(dst + 4u * i, &pdo, 4u);
    }
    *size_bytes = 4u * spr_count;
}

void pd_policy_get_epr_pdos(uint8_t *dst, uint32_t *size_bytes)
{
    memcpy(dst, epr_pdo, 4u * epr_count);
    *size_bytes = 4u * epr_count;
}

bool pd_policy_lookup(uint32_t position, uint32_t *pdo)
{
    if (position >= 1u && position <= spr_count)
    {
        *pdo = spr_pdo[position - 1u];
        return true;
    }
    if (position >= PD_POLICY_FIRST_EPR_POS &&
        position < PD_POLICY_FIRST_EPR_POS + epr_count)
    {
        *pdo = epr_pdo[position - PD_POLICY_FIRST_EPR_POS];
        return true;
    }
    return false;
}

PdRdoResult pd_policy_evaluate_rdo(uint32_t rdo, bool epr_mode, PdContract *contract)
{
    uint32_t position = RDO_POSITION(rdo);
    uint32_t pdo;
    PdRdoResult result;

    if (!pd_policy_lookup(position, &pdo))
    {
        result = PD_RDO_BAD_POSITION;
    }
    else if (position >= PD_POLICY_FIRST_EPR_POS && !epr_mode)
    {
        result = PD_RDO_EPR_OUTSIDE_EPR;
    }
    else if ((pdo & USBPD_PDO_TYPE_Msk) != USBPD_PDO_TYPE_FIXED)
    {
        result = PD_RDO_NOT_FIXED;
    }
    else if (RDO_OPERATING_10MA(rdo) > pdo_max_10ma(pdo) ||
             (RDO_MAX_OPERATING_10MA(rdo) > pdo_max_10ma(pdo) && !RDO_CAPABILITY_MISMATCH(rdo)))
    {
        result = PD_RDO_OVER_CURRENT;
    }
    else
    {
        contract->pdo          = pdo;
        contract->position     = position;
        contract->voltage_mv   = pdo_mv(pdo);
        contract->operating_ma = RDO_OPERATING_10MA(rdo) * 10u;
        contract->max_ma       = RDO_MAX_OPERATING_10MA(rdo) * 10u;
        result = PD_RDO_ACCEPT;
    }

    pd_status.last_rdo        = rdo;
    pd_status.last_rdo_result = (uint32_t)result;
    return result;
}

bool pd_policy_epr_entry_allowed(void)
{
    return PD_EPR_ENABLE && regulator_get_state() != REGULATOR_STATE_FAULT;
}

uint32_t pd_policy_max_offered_mv(void)
{
    uint32_t max_mv = 0u;
    for (uint32_t i = 0u; i < spr_count; i++)
    {
        if (pdo_mv(spr_pdo[i]) > max_mv) { max_mv = pdo_mv(spr_pdo[i]); }
    }
    for (uint32_t i = 0u; i < epr_count; i++)
    {
        if (pdo_mv(epr_pdo[i]) > max_mv) { max_mv = pdo_mv(epr_pdo[i]); }
    }
    return max_mv;
}

void pd_status_event(uint32_t code)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t slot = pd_status.ev_count % PD_STATUS_EVENTS;
    pd_status.ev_tick[slot] = HAL_GetTick();
    pd_status.ev_code[slot] = code;
    pd_status.ev_count++;
    __set_PRIMASK(primask);
}
