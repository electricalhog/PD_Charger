/**
 * @file    pd_bench_config.h
 * @brief   USB-PD source policy for bench bring-up: which PDOs are offered,
 *          at what current, and whether EPR and VCONN are enabled.
 *
 * Every limit that decides what a connected sink (dummy load, EPR tester,
 * laptop) may draw lives here, so a review of this file is a review of
 * what the port can do.  The defaults are deliberately conservative:
 *
 *   - 5 V and 9 V fixed PDOs at PD_SRC_MAX_CURRENT_MA (500 mA).
 *   - EPR Mode Capable advertised (only when the linked USB-PD core library
 *     implements EPR, i.e. USBPDCORE_EPR is defined), but no EPR PDO above
 *     20 V: a sink can enter EPR mode and exchange EPR_Source_Capabilities,
 *     EPR_Request and EPR_KeepAlive while VBUS stays at SPR voltages.
 *   - VCONN off (no VCONN switch on the bench rig yet), so EPR entry ends
 *     in EPR_Mode (Enter Failed) unless PD_VCONN_ENABLE is set.
 *
 * Raising any limit is a bench decision; the _Static_asserts below hold
 * the ones the firmware can check.
 */

#ifndef PD_BENCH_CONFIG_H
#define PD_BENCH_CONFIG_H

#include "regulator_config.h"

/* =========================================================================
 * SECTION 1: SPR fixed PDOs
 * =========================================================================*/

/**
 * PD_SRC_MAX_CURRENT_MA — Current offered in every PDO.
 * Units  : mA (PDO resolution 10 mA)
 * Value  : 500 mA.  The power stage runs DCM-only (SYNC_RECT_ENABLED 0,
 *          DCM_MAX_PEAK_MA 2500) and its output current capability has not
 *          been measured; bench runs so far loaded it with 330 ohm (85 mA
 *          at 28 V).  A sink may draw up to the RDO operating current it
 *          requests, which is capped by this value.
 * Raise  : only after a dummy-load run at the highest offered voltage
 *          holds V_out within 5 % at this current.
 */
#define PD_SRC_MAX_CURRENT_MA       500u

/** PD_SRC_PDO_9V_ENABLE / _15V_ / _20V_ — offer that fixed SPR PDO (1/0). */
#define PD_SRC_PDO_9V_ENABLE        1u
#define PD_SRC_PDO_15V_ENABLE       0u
#define PD_SRC_PDO_20V_ENABLE       0u

/* =========================================================================
 * SECTION 2: EPR
 * =========================================================================*/

/**
 * PD_EPR_ENABLE — Advertise EPR Mode Capable (PDO 1 bit 23) and enable the
 * EPR source state machine (DPM_Settings.PE_PD3_Support.Is_EPR_Supported_SRC).
 * Has no effect unless the USB-PD core library defines USBPDCORE_EPR
 * (STM32 USB-PD core >= v5.0.0; the library in this tree predates it).
 */
#define PD_EPR_ENABLE               1u

/**
 * PD_EPR_PDO_28V_ENABLE — Offer a 28 V fixed EPR PDO (object position 8).
 * Value  : 0.  With 0, EPR_Source_Capabilities carries only the SPR PDOs
 *          and VBUS never exceeds the highest SPR PDO.
 * Before setting 1, all of these must hold:
 *   - the regulator has held 28 V into a dummy load at PD_EPR_28V_CURRENT_MA
 *     without tripping SW_OVP (boost starts rang to 36-38 V in runs 32/33),
 *   - every part on VBUS is rated for >= 34 V (28 V x 1.2): output switch
 *     FETs, output capacitors, the Type-C receptacle, and the sink,
 *   - the cable is a 5 A EPR cable with an e-marker, and VCONN works
 *     (PD_VCONN_ENABLE), otherwise the library refuses EPR entry anyway,
 *   - the transition-time check below passes.
 */
#define PD_EPR_PDO_28V_ENABLE       0u
#define PD_EPR_28V_CURRENT_MA       500u

/* =========================================================================
 * SECTION 3: VCONN
 *
 * The STM32G4 UCPD cannot source VCONN; it needs a 5 V switch per CC line.
 * The bench rig wires the receptacle CC pins straight to the Nucleo UCPD
 * pins (PB6 = CC1, PB4 = CC2) and adds one NPN -> PNP high-side switch per
 * line from +5V.  Enabled by the CMake option PD_VCONN (adds _VCONN_SUPPORT
 * and PD_VCONN_ENABLE=1).
 *
 * NUCLEO-G474RE header positions (Arduino names from the stm32duino
 * NUCLEO_G474RE variant; Morpho numbering is the Nucleo-64 layout):
 *   CC1   PB6  Arduino D10 = CN5-3,  Morpho CN10-17
 *   CC2   PB4  Arduino D5  = CN9-6,  Morpho CN10-27
 *   VCONN on CC1 enable  PA7  Arduino D11 = CN5-4,  Morpho CN10-15
 *   VCONN on CC2 enable  PB5  Arduino D4  = CN9-5,  Morpho CN10-29
 * Each enable sits on the Morpho pin next to the CC pin it switches.
 * Neither is used in final.ioc (PB0/PB1, the old placeholders, are not:
 * PB0 is IS_MON).
 * =========================================================================*/
#ifndef PD_VCONN_ENABLE
#define PD_VCONN_ENABLE             0u
#endif

#if PD_VCONN_ENABLE
/* Active-high enables of the 5 V VCONN switches (GPIO high -> NPN on ->
 * PNP on -> +5V on that CC line).  pd_vbus.c drives both low before
 * configuring them as outputs. */
#define PD_VCONN_CC1_PORT           GPIOA
#define PD_VCONN_CC1_PIN            GPIO_PIN_7
#define PD_VCONN_CC2_PORT           GPIOB
#define PD_VCONN_CC2_PIN            GPIO_PIN_5
#endif

/* =========================================================================
 * SECTION 4: VBUS path timing
 * =========================================================================*/

/**
 * PD_VBUS_DISCHARGE_MS — Time OUTPUT_DIS is held after the output switch
 * opens before VBUS is reported at vSafe0V.  VBUS after the output switch
 * has no ADC channel on this board (VD_MON is before Q5/Q6), so vSafe0V is
 * a timed estimate, not a measurement.
 * Units  : ms.  Spec: tSafe0V max 650 ms.
 */
#define PD_VBUS_DISCHARGE_MS        100u

/**
 * PD_VBUS_TOLERANCE_PCT — |V_out - contract| window for "VBUS in range"
 * before PS_RDY is sent.  Spec vSrcNew: +/-5 %.
 */
#define PD_VBUS_TOLERANCE_PCT       5u

/**
 * PD_SPR_TRANSITION_BUDGET_MS / PD_EPR_TRANSITION_BUDGET_MS — How long the
 * source may take to reach a new voltage before PS_RDY.  The sink's
 * tPSTransition is 450 ms min (SPR) and 830 ms min (EPR); the PE has
 * already spent up to tSrcTransition (35 ms) before SetupNewPower.
 */
#define PD_SPR_TRANSITION_BUDGET_MS 400u
#define PD_EPR_TRANSITION_BUDGET_MS 750u

/** Up-slew of the regulator setpoint, mV per ms (regulator.c step 4a). */
#define PD_SLEW_MV_PER_MS \
    ((SETPOINT_SLEW_MV_PER_CYCLE) * (PID_EXECUTION_RATE_HZ) / 1000u)

/** Worst-case time to slew from vSafe5V to V (a request can follow a Hard
 *  Reset, which always restarts at 5 V). */
#define PD_RAMP_FROM_5V_MS(mv)      (((mv) - 5000u) / PD_SLEW_MV_PER_MS)

/* =========================================================================
 * Derived values and checks
 * =========================================================================*/

#define PD_SPR_MAX_MV                                                    \
    (PD_SRC_PDO_20V_ENABLE ? 20000u :                                    \
     PD_SRC_PDO_15V_ENABLE ? 15000u :                                    \
     PD_SRC_PDO_9V_ENABLE  ?  9000u : 5000u)

_Static_assert(PD_SRC_MAX_CURRENT_MA >= 100u && PD_SRC_MAX_CURRENT_MA <= 3000u,
               "PD_SRC_MAX_CURRENT_MA: 100..3000 mA (above 3 A needs a 5 A e-marked cable)");
_Static_assert(PD_EPR_28V_CURRENT_MA >= 100u && PD_EPR_28V_CURRENT_MA <= 5000u,
               "PD_EPR_28V_CURRENT_MA: 100..5000 mA");
_Static_assert(PD_RAMP_FROM_5V_MS(PD_SPR_MAX_MV) <= PD_SPR_TRANSITION_BUDGET_MS,
               "highest SPR PDO cannot be reached from 5 V within tPSTransition at "
               "SETPOINT_SLEW_MV_PER_CYCLE; the sink would Hard Reset. Raise the "
               "regulator up-slew (bench-validate overshoot first) or drop the PDO");
_Static_assert(!PD_EPR_PDO_28V_ENABLE ||
               PD_RAMP_FROM_5V_MS(28000u) <= PD_EPR_TRANSITION_BUDGET_MS,
               "28 V cannot be reached from 5 V within EPR tPSTransition at "
               "SETPOINT_SLEW_MV_PER_CYCLE");
_Static_assert(!PD_EPR_PDO_28V_ENABLE || 28000u * 110u / 100u < OVP_ABSOLUTE_MV,
               "28 V EPR PDO with its relative OVP must stay under OVP_ABSOLUTE_MV");
_Static_assert(PD_SPR_MAX_MV <= SETPOINT_MAX_MV, "PDO above SETPOINT_MAX_MV");

#endif /* PD_BENCH_CONFIG_H */
