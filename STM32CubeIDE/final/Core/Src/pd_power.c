/**
 * @file    pd_power.c
 * @brief   USB PD source power policy (see pd_power.h).
 *
 * Hardware notes (2026-09-29, X-NUCLEO-SRC1M1 stacked on the NUCLEO):
 *   - PC8 is the TCPP0203 ENABLE pin and also the power board's OUTPUT_EN.
 *     The TCPP must stay enabled for attach detection, so the PD stack owns
 *     PC8 and holds it high (the regulator leaves it alone when
 *     OUTPUT_EN_SHARED_WITH_TCPP is set in regulator_config.h).
 *   - The ST BSP would reprogram ADC1 as a continuous two-channel scan on
 *     VBUSInit, which kills the regulator's HRTIM-timed V_out sampling.  The
 *     build defines USBPD_CONFIG_MX, which removes that code (and the BSP's
 *     GPIO/EXTI setup, done here instead).  VSENSE (PA0, ADC12_IN1) and
 *     ISENSE (PC1, ADC12_IN7) are sampled on ADC2 as injected conversions,
 *     one pair per ms from the HAL tick, into the BSP's usbpd_pwr_adcx_buff so
 *     BSP_USBPD_PWR_VBUSGetVoltage/GetCurrent keep working.  ADC2's regular
 *     channel (V_in, started from the PID ISR) is unaffected: an injected
 *     conversion only delays it.
 */

#include "pd_power.h"

#include "main.h"
#include "cmsis_os.h"
#include "task.h"
#include "usbpd_core.h"
#include "usbpd_dpm_conf.h"
#include "usbpd_dpm_user.h"
#include "usbpd_dpm_core.h"
#include "usbpd_pdo_defs.h"
#include "src1m1_conf.h"
#include "src1m1_usbpd_pwr.h"
#include "regulator.h"
#include "regulator_config.h"
#include "pd_interface.h"
#include "adc_monitor.h"

extern ADC_HandleTypeDef hadc2;
extern uint16_t usbpd_pwr_adcx_buff[];
extern volatile uint32_t regulator_commanded_mv;

/* =========================================================================
 * State
 * =========================================================================*/

volatile uint32_t pd_vbus_source      = PD_VBUS_SOURCE_DEFAULT;
volatile uint32_t pd_profile          = PD_PROFILE_DEFAULT;
volatile uint32_t pd_vbus_path_max_mv = PD_VBUS_PATH_MAX_MV_DEFAULT;

volatile PdPowerStatus pd_power_status;

static uint32_t applied_source  = 0xFFFFFFFFu;
static uint32_t applied_profile = 0xFFFFFFFFu;
static uint32_t applied_path_mv = 0xFFFFFFFFu;
static volatile bool vsense_running = false;

static const uint32_t spr_candidates_mv[] = { 5000u, 9000u, 15000u, 20000u };
static const uint32_t epr_candidates_mv[] = { 28000u, 36000u, 48000u };

/* ADC2 sample time code 6 = 247.5 cycles (5.8 us at 42.5 MHz): the VSENSE
 * divider is 200k/40k (33 kohm source). */
#define VSENSE_SMP_CODE     6u

/* =========================================================================
 * PDO construction
 * =========================================================================*/

static uint32_t fixed_pdo(uint32_t mv, uint32_t ma)
{
    /* Fixed supply PDO: B31..30 = 00, B19..10 voltage in 50 mV, B9..0 current in 10 mA. */
    return ((mv / 50u) << 10) | (ma / 10u);
}

static uint32_t source_max_mv(void)
{
    return (pd_vbus_source == PD_VBUS_SOURCE_REGULATOR) ? SETPOINT_MAX_MV : 5000u;
}

static uint32_t offer_limit_mv(void)
{
    uint32_t lim = source_max_mv();
    if (pd_vbus_path_max_mv < lim)
    {
        lim = pd_vbus_path_max_mv;
    }
    return lim;
}

static void build_pdos(void)
{
    uint32_t spr[PD_MAX_SPR_PDO] = { 0u };
    uint32_t epr[PD_MAX_EPR_PDO] = { 0u };
    uint32_t n_spr = 0u, n_epr = 0u;
    uint32_t lim = offer_limit_mv();
    bool bench = (pd_profile == PD_PROFILE_BENCH_5V) || (pd_vbus_source == PD_VBUS_SOURCE_BENCH_5V);

    if (bench)
    {
        spr[n_spr++] = fixed_pdo(5000u, PD_BENCH_5V_CURRENT_MA);
    }
    else
    {
        for (uint32_t i = 0u; i < sizeof spr_candidates_mv / sizeof spr_candidates_mv[0]; i++)
        {
            if (spr_candidates_mv[i] <= lim)
            {
                spr[n_spr++] = fixed_pdo(spr_candidates_mv[i], PD_SPR_MAX_CURRENT_MA);
            }
        }
        if (pd_profile == PD_PROFILE_EPR)
        {
            for (uint32_t i = 0u; i < sizeof epr_candidates_mv / sizeof epr_candidates_mv[0]; i++)
            {
                if (epr_candidates_mv[i] <= lim)
                {
                    epr[n_epr++] = fixed_pdo(epr_candidates_mv[i], PD_EPR_MAX_CURRENT_MA);
                }
            }
        }
    }

    /* Common bits live in PDO 1 only.  EPR Mode Capable only when there is
     * something to offer in EPR mode. */
    spr[0] |= USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL
            | USBPD_PDO_SRC_FIXED_UNCHUNK_NOT_SUPPORTED
            | USBPD_PDO_SRC_FIXED_DRD_NOT_SUPPORTED
            | USBPD_PDO_SRC_FIXED_USBCOMM_NOT_SUPPORTED
            | USBPD_PDO_SRC_FIXED_EXT_POWER_NOT_AVAILABLE
            | USBPD_PDO_SRC_FIXED_USBSUSPEND_NOT_SUPPORTED
            | USBPD_PDO_SRC_FIXED_DRP_NOT_SUPPORTED
            | ((n_epr > 0u) ? USBPD_PDO_SRC_FIXED_EPR_SUPPORTED : USBPD_PDO_SRC_FIXED_EPR_NOT_SUPPORTED);

    /* The PE task reads these lists; swap them in one critical section
     * (none needed before the scheduler runs, and a FreeRTOS critical
     * section taken then would leave BASEPRI raised until it starts). */
    bool rtos = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    if (rtos)
    {
        taskENTER_CRITICAL();
    }
    for (uint32_t i = 0u; i < PD_MAX_SPR_PDO; i++)
    {
        pd_power_status.spr_pdo[i] = spr[i];
    }
    for (uint32_t i = 0u; i < PD_MAX_EPR_PDO; i++)
    {
        pd_power_status.epr_pdo[i] = epr[i];
    }
    pd_power_status.n_spr_pdo = n_spr;
    pd_power_status.n_epr_pdo = n_epr;
    /* Mirror into the generated PWR_IF storage, which the GUI interface
     * (STM32CubeMonitor-UCPD) reads to show the port's capabilities. */
    for (uint32_t i = 0u; i < USBPD_MAX_NB_PDO; i++)
    {
        PORT0_PDO_ListSRC[i] = (i < PD_MAX_SPR_PDO) ? spr[i] : 0u;
    }
    USBPD_NbPDO[1] = (uint8_t)n_spr;
    if (rtos)
    {
        taskEXIT_CRITICAL();
    }
}

uint32_t pd_power_get_spr_pdos(uint32_t *dst, uint32_t max)
{
    uint32_t n = pd_power_status.n_spr_pdo;
    if (n > max)
    {
        n = max;
    }
    for (uint32_t i = 0u; i < n; i++)
    {
        dst[i] = pd_power_status.spr_pdo[i];
    }
    return n;
}

uint32_t pd_power_get_epr_pdos(uint32_t *dst, uint32_t max)
{
    uint32_t n = pd_power_status.n_epr_pdo;
    if (n > max)
    {
        n = max;
    }
    for (uint32_t i = 0u; i < n; i++)
    {
        dst[i] = pd_power_status.epr_pdo[i];
    }
    return n;
}

uint32_t pd_power_pdo_at(uint32_t pos)
{
    if (pos >= 1u && pos <= pd_power_status.n_spr_pdo)
    {
        return pd_power_status.spr_pdo[pos - 1u];
    }
    if (pos >= 8u && (pos - 8u) < pd_power_status.n_epr_pdo)
    {
        return pd_power_status.epr_pdo[pos - 8u];
    }
    return 0u;
}

static uint32_t pdo_mw(uint32_t pdo)
{
    return ((pdo >> 10) & 0x3FFu) * 50u * (pdo & 0x3FFu) * 10u / 1000u;
}

uint8_t pd_power_spr_pdp_w(void)
{
    uint32_t w = 0u;
    for (uint32_t i = 0u; i < pd_power_status.n_spr_pdo; i++)
    {
        uint32_t p = pdo_mw(pd_power_status.spr_pdo[i]) / 1000u;
        w = (p > w) ? p : w;
    }
    return (uint8_t)w;
}

uint8_t pd_power_epr_pdp_w(void)
{
    uint32_t w = pd_power_spr_pdp_w();
    for (uint32_t i = 0u; i < pd_power_status.n_epr_pdo; i++)
    {
        uint32_t p = pdo_mw(pd_power_status.epr_pdo[i]) / 1000u;
        w = (p > w) ? p : w;
    }
    return (pd_power_status.n_epr_pdo > 0u) ? (uint8_t)w : 0u;
}

/* =========================================================================
 * TCPP GPIO / EXTI (what the BSP does without USBPD_CONFIG_MX)
 * =========================================================================*/

static void tcpp_gpio_init(void)
{
    /* ENABLE (PC8): already a push-pull output from MX_GPIO_Init (OUTPUT_EN). */
    TCPP0203_PORT0_ENABLE_GPIO_SET();

    /* FLGn (PC5): input with pull-up, falling-edge EXTI -> BSP_USBPD_PWR_EventCallback. */
    TCPP0203_PORT0_FLG_GPIO_CLK_ENABLE();
    LL_GPIO_SetPinMode(TCPP0203_PORT0_FLG_GPIO_PORT, TCPP0203_PORT0_FLG_GPIO_PIN, TCPP0203_PORT0_FLG_GPIO_MODE);
    LL_GPIO_SetPinPull(TCPP0203_PORT0_FLG_GPIO_PORT, TCPP0203_PORT0_FLG_GPIO_PIN, TCPP0203_PORT0_FLG_GPIO_PUPD);
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    TCPP0203_PORT0_FLG_SET_EXTI();
    TCPP0203_PORT0_FLG_EXTI_ENABLE();
    TCPP0203_PORT0_FLG_TRIG_ENABLE();
    NVIC_SetPriority(TCPP0203_PORT0_FLG_EXTI_IRQN, TCPP0203_PORT0_FLG_IT_PRIORITY);
    NVIC_EnableIRQ(TCPP0203_PORT0_FLG_EXTI_IRQN);
}

/* =========================================================================
 * VBUS / IBUS sensing on ADC2 injected channels
 * =========================================================================*/

static void vsense_init(void)
{
    if ((ADC2->CR & ADC_CR_ADEN) == 0u)
    {
        HAL_ADC_Start(&hadc2);   /* enables ADC2 (and runs one V_in conversion) */
    }
    /* SMPR and JSQR are writable only with ADSTART = JADSTART = 0. */
    for (uint32_t spin = 0u; spin < 10000u && (ADC2->CR & (ADC_CR_ADSTART | ADC_CR_JADSTART)); spin++)
    {
    }
    ADC2->SMPR1 = (ADC2->SMPR1 & ~((7u << ADC_SMPR1_SMP1_Pos) | (7u << ADC_SMPR1_SMP7_Pos)))
                | (VSENSE_SMP_CODE << ADC_SMPR1_SMP1_Pos) | (VSENSE_SMP_CODE << ADC_SMPR1_SMP7_Pos);
    /* Two injected conversions, software trigger: JSQ1 = IN1 (VSENSE), JSQ2 = IN7 (ISENSE). */
    ADC2->JSQR = (1u << ADC_JSQR_JL_Pos) | (1u << ADC_JSQR_JSQ1_Pos) | (7u << ADC_JSQR_JSQ2_Pos);
    ADC2->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS;
    vsense_running = true;
}

void pd_power_tick_1ms(void)
{
    if (!vsense_running || (ADC2->CR & ADC_CR_ADEN) == 0u)
    {
        return;
    }
    if (ADC2->ISR & ADC_ISR_JEOS)
    {
        uint16_t v = (uint16_t)ADC2->JDR1;
        uint16_t i = (uint16_t)ADC2->JDR2;
        ADC2->ISR = ADC_ISR_JEOC | ADC_ISR_JEOS;
        usbpd_pwr_adcx_buff[ADCBUF_VSENSE] = v;
        usbpd_pwr_adcx_buff[ADCBUF_ISENSE] = i;
        pd_power_status.vbus_mv = (uint32_t)v * 3300u / 4095u
                                * (SRC1M1_VSENSE_RA + SRC1M1_VSENSE_RB) / SRC1M1_VSENSE_RB;
        pd_power_status.tick_ms = HAL_GetTick();
    }
    if ((ADC2->CR & ADC_CR_JADSTART) == 0u)
    {
        /* ADC_CR set-only bits ignore a written 0: write JADSTART alone
         * (plus the read/write bits unchanged) so an ADSTART that the PID
         * ISR set, or that just cleared, is not written back. */
        ADC2->CR = (ADC2->CR & (ADC_CR_ADVREGEN | ADC_CR_DEEPPWD | ADC_CR_ADCALDIF)) | ADC_CR_JADSTART;
    }
}

/* =========================================================================
 * Init / poll
 * =========================================================================*/

void pd_power_init(void)
{
    /* Stack settings the .ioc cannot express for this CubeMX/G4 pack:
     * SOP'/SOP'' for EPR cable discovery, EPR as a source. */
    DPM_Settings[USBPD_PORT_0].PE_SupportedSOP = USBPD_SUPPORTED_SOP_SOP
                                              | USBPD_SUPPORTED_SOP_SOP1
                                              | USBPD_SUPPORTED_SOP_SOP2;
    DPM_Settings[USBPD_PORT_0].PE_PD3_Support.d.Is_EPR_Supported_SRC   = USBPD_TRUE;
    DPM_Settings[USBPD_PORT_0].PE_PD3_Support.d.Is_SrcCapaExt_Supported = USBPD_TRUE;
    DPM_USER_Settings[USBPD_PORT_0].PE_DataSwap       = USBPD_FALSE;
    DPM_USER_Settings[USBPD_PORT_0].PE_DR_Swap_To_DFP = USBPD_FALSE;
    DPM_USER_Settings[USBPD_PORT_0].PE_DR_Swap_To_UFP = USBPD_FALSE;
    DPM_USER_Settings[USBPD_PORT_0].PE_VconnSwap      = USBPD_FALSE;

    tcpp_gpio_init();
    vsense_init();

    build_pdos();
    applied_source  = pd_vbus_source;
    applied_profile = pd_profile;
    applied_path_mv = pd_vbus_path_max_mv;
}

void pd_power_poll(void)
{
    int32_t ma;
    if (BSP_USBPD_PWR_VBUSGetCurrent(USBPD_PWR_TYPE_C_PORT_1, &ma) == BSP_ERROR_NONE)
    {
        pd_power_status.ibus_ma = ma;
    }

    if (pd_vbus_source != applied_source || pd_profile != applied_profile
        || pd_vbus_path_max_mv != applied_path_mv)
    {
        applied_source  = pd_vbus_source;
        applied_profile = pd_profile;
        applied_path_mv = pd_vbus_path_max_mv;
        build_pdos();
        if (pd_power_status.attached && pd_power_status.contract_mv != 0u)
        {
            /* New offer: the sink re-requests.  In EPR mode the stack sends
             * EPR_Source_Capabilities. */
            (void)USBPD_DPM_RequestSourceCapability(USBPD_PORT_0);
        }
    }
}

/* =========================================================================
 * Contracts
 * =========================================================================*/

static bool regulator_at(uint32_t mv, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    for (;;)
    {
        if (regulator_get_state() == REGULATOR_STATE_FAULT)
        {
            pd_power_status.last_error = PD_ERR_REGULATOR;
            return false;
        }
        uint32_t v = adc_measurements.v_out_mv;
        uint32_t tol = mv / 20u;   /* 5 % */
        if (regulator_commanded_mv == mv && regulator_ready && v + tol >= mv && v <= mv + tol)
        {
            return true;
        }
        if ((HAL_GetTick() - t0) > timeout_ms)
        {
            pd_power_status.last_error = PD_ERR_SETTLE_TIMEOUT;
            return false;
        }
        osDelay(2);
    }
}

static bool drive_vbus(uint32_t mv, uint32_t timeout_ms)
{
    if (mv > pd_vbus_path_max_mv)
    {
        pd_power_status.last_error = PD_ERR_PATH_LIMIT;
        return false;
    }
    if (pd_vbus_source == PD_VBUS_SOURCE_BENCH_5V)
    {
        if (mv != 5000u)
        {
            pd_power_status.last_error = PD_ERR_SOURCE_CANT;
            return false;
        }
        return true;   /* the NUCLEO 5 V rail is always there */
    }

    regulator_set_target_voltage(mv);
    if (regulator_get_state() == REGULATOR_STATE_IDLE)
    {
        regulator_start();
    }
    if (regulator_get_state() != REGULATOR_STATE_RUNNING)
    {
        pd_power_status.last_error = PD_ERR_REGULATOR;
        return false;
    }
    return regulator_at(mv, timeout_ms);
}

bool pd_power_vbus_prepare(void)
{
    pd_power_status.last_error = PD_ERR_NONE;
    return drive_vbus(5000u, PD_SETTLE_TIMEOUT_SPR_MS);
}

void pd_power_vbus_release(void)
{
    pd_power_status.contract_mv  = 0u;
    pd_power_status.contract_ma  = 0u;
    pd_power_status.contract_pos = 0u;
    if (pd_vbus_source == PD_VBUS_SOURCE_REGULATOR)
    {
        pd_interface_notify_disconnect();
    }
}

bool pd_power_set_contract(uint32_t pos, uint32_t mv, uint32_t ma, bool epr)
{
    pd_power_status.last_error = PD_ERR_NONE;
    bool ok = drive_vbus(mv, epr ? PD_SETTLE_TIMEOUT_EPR_MS : PD_SETTLE_TIMEOUT_SPR_MS);
    if (ok)
    {
        pd_power_status.contract_mv  = mv;
        pd_power_status.contract_ma  = ma;
        pd_power_status.contract_pos = pos;
    }
    return ok;
}

/* =========================================================================
 * Event hooks
 * =========================================================================*/

void pd_power_on_attach(bool attached)
{
    pd_power_status.attached = attached ? 1u : 0u;
    if (!attached)
    {
        pd_power_status.epr_mode = 0u;
        pd_power_status.vconn_on = 0u;
    }
}

void pd_power_on_notify(uint32_t event)
{
    pd_power_status.last_notify = event;
    pd_power_status.n_notify++;
    switch (event)
    {
    case USBPD_NOTIFY_HARDRESET_RX:
    case USBPD_NOTIFY_HARDRESET_TX:
        pd_power_status.n_hard_resets++;
        pd_power_status.epr_mode = 0u;
        break;
    case USBPD_NOTIFY_EPRMODE_SUCCEEDED:
        pd_power_status.epr_mode = 1u;
        pd_power_status.n_epr_entries++;
        break;
    case USBPD_NOTIFY_EPRMODE_FAILED:
    case USBPD_NOTIFY_EPRMODE_INVALID:
        pd_power_status.n_epr_fails++;
        break;
    case USBPD_NOTIFY_EPRMODE_EXIT:
        pd_power_status.epr_mode = 0u;
        break;
    default:
        break;
    }
}

void pd_power_on_epr_mode(uint32_t action, uint32_t data)
{
    pd_power_status.epr_last_action = (action << 8) | (data & 0xFFu);
}

void pd_power_on_vconn(bool on)
{
    pd_power_status.vconn_on = on ? 1u : 0u;
}
