/**
 * @file    pd_vbus.c
 * @brief   BSP_USBPD_PWR_* on this board's regulator and output switch.
 *
 * See pd_vbus.h.  Compiled to nothing unless PD_OWNS_VBUS; the CMake option
 * PD_VBUS_PATH_CHARGER also drops the X-NUCLEO-SRC1M1 BSP from the link, so
 * these strong definitions replace the __weak stubs in usbpd_pwr_user.c.
 *
 * Contexts: VBUSOn/Off run in the CAD and PE tasks, SetVoltage_* and
 * pd_vbus_wait_in_range in the PE task, pd_vbus_poll in the default task.
 * regulator_start() blocks for up to INPUT_PGOOD_TIMEOUT_MS + 2 ms
 * (HAL_Delay), as it already did when DPM_SetupNewPower called it.
 */

#include "pd_vbus.h"

#if PD_OWNS_VBUS

#include "pd_bench_config.h"
#include "pd_policy.h"
#include "regulator.h"
#include "regulator_config.h"
#include "adc_monitor.h"
#include "main.h"
#include "cmsis_os.h"
#include "usbpd_core.h"
#include "usbpd_dpm_core.h"
#include "usbpd_dpm_user.h"
#include "usbpd_pwr_user.h"

/* HAL tick at which the output switch was last seen opening. */
static volatile uint32_t vbus_off_tick;
static bool              vbus_seen_on;
static bool              fault_reset_sent;

static bool output_switch_closed(void)
{
    return (PIN_OUTPUT_EN_PORT->ODR & PIN_OUTPUT_EN_PIN) != 0u;
}

uint32_t pd_vbus_get_mv(void)
{
    if (output_switch_closed())
    {
        return adc_measurements.v_out_mv;
    }
    /* Switch open: OUTPUT_DIS discharges the VBUS side, which no ADC sees. */
    return ((HAL_GetTick() - vbus_off_tick) >= PD_VBUS_DISCHARGE_MS)
               ? 0u : adc_measurements.v_out_mv;
}

uint32_t pd_vbus_wait_in_range(uint32_t target_mv, uint32_t timeout_ms)
{
    const uint32_t tol_mv = target_mv * PD_VBUS_TOLERANCE_PCT / 100u;
    const uint32_t start  = HAL_GetTick();
    uint32_t in_range = 0u;

    for (;;)
    {
        uint32_t elapsed = HAL_GetTick() - start;
        if (regulator_get_state() != REGULATOR_STATE_RUNNING)
        {
            return UINT32_MAX;
        }
        uint32_t v = adc_measurements.v_out_mv;
        bool ok = output_switch_closed() &&
                  v + tol_mv >= target_mv && v <= target_mv + tol_mv;
        in_range = ok ? in_range + 1u : 0u;
        if (in_range >= 3u)              /* 3 consecutive 2 ms samples */
        {
            return elapsed;
        }
        if (elapsed >= timeout_ms)
        {
            return UINT32_MAX;
        }
        osDelay(2);
    }
}

void pd_vbus_poll(void)
{
    bool closed = output_switch_closed();
    if (vbus_seen_on && !closed)
    {
        vbus_off_tick = HAL_GetTick();
    }
    vbus_seen_on = closed;

    RegulatorState state = regulator_get_state();
    if (state != REGULATOR_STATE_FAULT)
    {
        fault_reset_sent = false;
    }
    else if (!fault_reset_sent &&
             DPM_Params[USBPD_PORT_0].PE_Power == USBPD_POWER_EXPLICITCONTRACT)
    {
        /* The FAULT stays latched (only `bu regulator clear-fault` releases
         * it), so the Hard Reset ends with VBUS off rather than a retry.  */
        fault_reset_sent = true;
        pd_status.fault_hard_resets++;
        pd_status_event(PD_EV_FAULT_HARDRESET);
        (void)USBPD_DPM_RequestHardReset(USBPD_PORT_0);
    }
}

/* =========================================================================
 * BSP_USBPD_PWR_* (prototypes in usbpd_pwr_user.h)
 * =========================================================================*/

int32_t BSP_USBPD_PWR_Init(uint32_t Instance)
{
    return (Instance < USBPD_PWR_INSTANCES_NBR) ? BSP_ERROR_NONE : BSP_ERROR_WRONG_PARAM;
}

int32_t BSP_USBPD_PWR_Deinit(uint32_t Instance)
{
    return BSP_USBPD_PWR_Init(Instance);
}

int32_t BSP_USBPD_PWR_SetRole(uint32_t Instance, USBPD_PWR_PowerRoleTypeDef Role)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }
    return (Role == POWER_ROLE_SOURCE) ? BSP_ERROR_NONE : BSP_ERROR_FEATURE_NOT_SUPPORTED;
}

int32_t BSP_USBPD_PWR_SetPowerMode(uint32_t Instance, USBPD_PWR_PowerModeTypeDef PwrMode)
{
    (void)PwrMode;
    return BSP_USBPD_PWR_Init(Instance);
}

int32_t BSP_USBPD_PWR_GetPowerMode(uint32_t Instance, USBPD_PWR_PowerModeTypeDef *PwrMode)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR || PwrMode == NULL) { return BSP_ERROR_WRONG_PARAM; }
    *PwrMode = USBPD_PWR_MODE_NORMAL;
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VBUSInit(uint32_t Instance)
{
    return BSP_USBPD_PWR_Init(Instance);
}

int32_t BSP_USBPD_PWR_VBUSDeInit(uint32_t Instance)
{
    return BSP_USBPD_PWR_Init(Instance);
}

int32_t BSP_USBPD_PWR_VBUSOn(uint32_t Instance)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }

    /* Attach or the end of a Hard Reset: vSafe5V. */
    pd_interface_notify_voltage_contract(5000u);
    if (regulator_get_state() == REGULATOR_STATE_IDLE)
    {
        regulator_start();
    }
    if (regulator_get_state() != REGULATOR_STATE_RUNNING)
    {
        pd_status.vbus_on_failures++;
        pd_status_event(PD_EV_VBUS_ON_FAILED);
        return BSP_ERROR_COMPONENT_FAILURE;
    }
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VBUSOff(uint32_t Instance)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }

    pd_interface_notify_disconnect();   /* target 0, regulator_stop(): switch open, OUTPUT_DIS on */
    vbus_off_tick = HAL_GetTick();
    vbus_seen_on  = false;
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VBUSIsOn(uint32_t Instance, uint8_t *pState)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR || pState == NULL) { return BSP_ERROR_WRONG_PARAM; }
    *pState = output_switch_closed() ? 1u : 0u;
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VBUSSetVoltage_Fixed(uint32_t Instance,
                                           uint32_t VbusTargetInmv,
                                           uint32_t OperatingCurrent,
                                           uint32_t MaxOperatingCurrent)
{
    (void)OperatingCurrent;
    (void)MaxOperatingCurrent;
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }

    /* Never set anything the PDO table does not offer. */
    if (VbusTargetInmv < 5000u || VbusTargetInmv > pd_policy_max_offered_mv())
    {
        return BSP_ERROR_WRONG_PARAM;
    }
    pd_interface_notify_voltage_contract(VbusTargetInmv);
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VBUSGetVoltage(uint32_t Instance, uint32_t *pVoltage)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR || pVoltage == NULL) { return BSP_ERROR_WRONG_PARAM; }
    *pVoltage = pd_vbus_get_mv();
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VBUSGetCurrent(uint32_t Instance, int32_t *pCurrent)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR || pCurrent == NULL) { return BSP_ERROR_WRONG_PARAM; }
    *pCurrent = output_switch_closed() ? (int32_t)adc_measurements.i_out_ma : 0;
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_SetVBUSDisconnectionThreshold(uint32_t Instance, uint32_t VoltageThreshold)
{
    (void)VoltageThreshold;
    return BSP_USBPD_PWR_Init(Instance);
}

int32_t BSP_USBPD_PWR_RegisterVBUSDetectCallback(uint32_t Instance,
                                                 USBPD_PWR_VBUSDetectCallbackFunc *pfnVBUSDetectCallback)
{
    (void)pfnVBUSDetectCallback;        /* source only: no VBUS detection */
    return BSP_USBPD_PWR_Init(Instance);
}

void BSP_USBPD_PWR_EventCallback(uint32_t Instance)
{
    (void)Instance;                     /* no TCPP03 FLGn line */
}

/* -------------------------------------------------------------------------
 * VCONN
 * -------------------------------------------------------------------------*/

#if PD_VCONN_ENABLE
static bool vconn_gpio_ready;

static void vconn_gpio_init(void)
{
    GPIO_InitTypeDef init = {0};
    HAL_GPIO_WritePin(PD_VCONN_CC1_PORT, PD_VCONN_CC1_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PD_VCONN_CC2_PORT, PD_VCONN_CC2_PIN, GPIO_PIN_RESET);
    init.Mode  = GPIO_MODE_OUTPUT_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Pin   = PD_VCONN_CC1_PIN;
    HAL_GPIO_Init(PD_VCONN_CC1_PORT, &init);
    init.Pin   = PD_VCONN_CC2_PIN;
    HAL_GPIO_Init(PD_VCONN_CC2_PORT, &init);
    vconn_gpio_ready = true;
}

static int32_t vconn_set(uint32_t CCPinId, GPIO_PinState level)
{
    if (!vconn_gpio_ready) { vconn_gpio_init(); }
    if (CCPinId == USBPD_PWR_TYPE_C_CC1)
    {
        HAL_GPIO_WritePin(PD_VCONN_CC1_PORT, PD_VCONN_CC1_PIN, level);
    }
    else if (CCPinId == USBPD_PWR_TYPE_C_CC2)
    {
        HAL_GPIO_WritePin(PD_VCONN_CC2_PORT, PD_VCONN_CC2_PIN, level);
    }
    else
    {
        return BSP_ERROR_WRONG_PARAM;
    }
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VCONNInit(uint32_t Instance, uint32_t CCPinId)
{
    (void)CCPinId;
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }
    if (!vconn_gpio_ready) { vconn_gpio_init(); }
    return BSP_ERROR_NONE;
}

int32_t BSP_USBPD_PWR_VCONNDeInit(uint32_t Instance, uint32_t CCPinId)
{
    return BSP_USBPD_PWR_VCONNOff(Instance, CCPinId);
}

int32_t BSP_USBPD_PWR_VCONNOn(uint32_t Instance, uint32_t CCPinId)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }
    return vconn_set(CCPinId, GPIO_PIN_SET);
}

int32_t BSP_USBPD_PWR_VCONNOff(uint32_t Instance, uint32_t CCPinId)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR) { return BSP_ERROR_WRONG_PARAM; }
    return vconn_set(CCPinId, GPIO_PIN_RESET);
}

int32_t BSP_USBPD_PWR_VCONNIsOn(uint32_t Instance, uint32_t CCPinId, uint8_t *pState)
{
    if (Instance >= USBPD_PWR_INSTANCES_NBR || pState == NULL) { return BSP_ERROR_WRONG_PARAM; }
    if (CCPinId == USBPD_PWR_TYPE_C_CC1)
    {
        *pState = (PD_VCONN_CC1_PORT->ODR & PD_VCONN_CC1_PIN) ? 1u : 0u;
    }
    else if (CCPinId == USBPD_PWR_TYPE_C_CC2)
    {
        *pState = (PD_VCONN_CC2_PORT->ODR & PD_VCONN_CC2_PIN) ? 1u : 0u;
    }
    else
    {
        return BSP_ERROR_WRONG_PARAM;
    }
    return BSP_ERROR_NONE;
}
#endif /* PD_VCONN_ENABLE */

#endif /* PD_OWNS_VBUS */
