/**
 * @file    pd_bench_config.h
 * @brief   USB-PD source policy for bench bring-up: which PDOs are offered,
 *          at what current, and whether EPR and VCONN are enabled.
 *
 * Every limit that decides what a connected sink (dummy load, EPR tester,
 * laptop) may draw lives here, so a review of this file is a review of
 * what the port can do.  The defaults are deliberately conservative:
 *
 *   - 5 V and 9 V fixed PDOs at PD_SRC_MAX_CURRENT_MA (500 mA); 15 V too
 *     with CMake -DPD_BENCH_15V=ON, and 20 V with -DPD_BENCH_20V=ON (see the
 *     stepped-transition checks).
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
 * Value  : 3000 mA (2026-10-15; 500 mA until then).  The bench now has
 *          25 ohm fixed on VBUS (1.9 A at 48 V), so every PDO must cover
 *          that.  The power stage is still DCM-only (SYNC_RECT_ENABLED 0)
 *          with the raised peak limits; whether it holds 5 % at these
 *          currents is what the 2026-10-15 runs measure.  A sink may draw
 *          up to the RDO operating current it requests, capped by this.
 * Raise  : only after a dummy-load run at the highest offered voltage
 *          holds V_out within 5 % at this current.
 */
#define PD_SRC_MAX_CURRENT_MA       3000u   /* 25 ohm on VBUS since 2026-10-15: 0.8 A at 20 V */

/** PD_SRC_PDO_9V_ENABLE / _15V_ / _20V_ — offer that fixed SPR PDO (1/0). */
#define PD_SRC_PDO_9V_ENABLE        1u
#if defined(PD_BENCH_15V) && (PD_BENCH_15V)
/* CMake -DPD_BENCH_15V=ON: the buck load's gate drive needs 12-15 V on its
 * input.  Reached in steps; see the checks at the end of this file. */
#define PD_SRC_PDO_15V_ENABLE       1u
#else
#define PD_SRC_PDO_15V_ENABLE       0u
#endif
#if defined(PD_BENCH_20V) && (PD_BENCH_20V)
/* CMake -DPD_BENCH_20V=ON (needs PD_BENCH_15V): reached from 15 V. */
#define PD_SRC_PDO_20V_ENABLE       1u
#else
#define PD_SRC_PDO_20V_ENABLE       0u
#endif
#if defined(PD_BENCH_24V) && (PD_BENCH_24V)
/* CMake -DPD_BENCH_24V=ON: a 24 V fixed PDO after the others in the SPR list.
 * Bench only: the spec stops SPR fixed PDOs at 20 V.  At about 24.5 V in it
 * sits in the middle of the buck-boost band, for steady buck-boost tests
 * (2026-10-12). */
#define PD_SRC_PDO_24V_ENABLE       1u
#else
#define PD_SRC_PDO_24V_ENABLE       0u
#endif

/* =========================================================================
 * SECTION 2: EPR
 * =========================================================================*/

/**
 * PD_EPR_ENABLE — Advertise EPR Mode Capable (PDO 1 bit 23) and enable the
 * EPR source state machine (DPM_Settings.PE_PD3_Support.Is_EPR_Supported_SRC).
 * Has no effect unless the USB-PD core library defines USBPDCORE_EPR
 * (STM32 USB-PD core >= v5.0.0; this tree has V5.3, PD3_FULL, which does).
 */
#define PD_EPR_ENABLE               1u

/**
 * PD_EPR_PDO_28V_ENABLE — Offer a 28 V fixed EPR PDO (object position 8).
 * Value  : 0, or 1 with CMake -DPD_BENCH_EPR_28V=ON (bench only).  With 0,
 *          EPR_Source_Capabilities carries only the SPR PDOs and VBUS never
 *          exceeds the highest SPR PDO.
 * Before it becomes a default, all of these must hold (status 2026-10-11):
 *   - the regulator holds 28 V into a dummy load at PD_EPR_28V_CURRENT_MA
 *     without tripping SW_OVP (boost starts rang to 36-38 V in runs 32/33).
 *     This is what the bench option is for (plans/epr-bringup.md, task 1).
 *   - every part on VBUS is rated for >= 34 V (28 V x 1.2).  The power board
 *     was designed with parts of at least 60 V (part numbers not verified).
 *   - the cable is a 5 A EPR cable with an e-marker, and VCONN works
 *     (PD_VCONN_ENABLE), otherwise the library refuses EPR entry anyway.
 *     The Apple 240 W cable passes on both CC lines (2026-10-07 CC2,
 *     2026-10-11 CC1).
 *   - the transition-time checks below pass.
 */
#if defined(PD_BENCH_EPR_28V) && (PD_BENCH_EPR_28V)
#define PD_EPR_PDO_28V_ENABLE       1u
#else
#define PD_EPR_PDO_28V_ENABLE       0u
#endif
#define PD_EPR_28V_CURRENT_MA       3000u

/**
 * PD_EPR_PDO_36V_ENABLE / PD_EPR_PDO_48V_ENABLE — 36 and 48 V fixed EPR PDOs
 * (the next object positions after 28 V), each behind its own bench option
 * (CMake -DPD_BENCH_EPR_36V=ON, -DPD_BENCH_EPR_48V=ON), enabled in order
 * (plans/epr-bringup.md).  On top of the 28 V conditions:
 *   - 48 V needs every part on VBUS rated >= 58 V; the power board's are
 *     60 V by design (unverified), R27 dissipates 123 mW at 48 V.
 *   - the QT Py load's VIN tap (9.78:1) passes 3.3 V at 32 V and IC8
 *     (L6983) is a 38 V part: risk accepted for the bench (2026-10-11).
 *   - boost delivers at most L I_pk^2 V_out / (2 (V_out - V_in)) f at the
 *     DCM_MAX_PEAK_MA limit: 5.9 W at 48 V from 24 V, before losses.
 *   - BLOCKED (2026-10-12): VD_MON under-reads above about 20 V.  With the
 *     ADC at 36.0 V, the scope read 42.3 V at TP2 and Q3's diode plateau on
 *     SW2 rose 15.4 V from the 28 V point (ADC +8.1 V).  48 V would be about
 *     60 V, and OVP uses the same sense.  Fix the sense (D3's 3.4 V clamp
 *     leakage is the suspect) and meter-check before enabling either.
 */
#if defined(PD_BENCH_EPR_36V) && (PD_BENCH_EPR_36V)
#define PD_EPR_PDO_36V_ENABLE       1u
#else
#define PD_EPR_PDO_36V_ENABLE       0u
#endif
#if defined(PD_BENCH_EPR_48V) && (PD_BENCH_EPR_48V)
#define PD_EPR_PDO_48V_ENABLE       1u
#else
#define PD_EPR_PDO_48V_ENABLE       0u
#endif
#define PD_EPR_36V_CURRENT_MA       3000u
#define PD_EPR_48V_CURRENT_MA       3000u   /* 25 ohm draws 1.92 A */

/**
 * PD_BENCH_HIGH_V_MV / PD_BENCH_HIGH_V_MAX_MS — Time cap for contracts above
 * PD_BENCH_HIGH_V_MV (2026-10-15).  The bench has 25 ohm / 25 W fixed on
 * VBUS: 31 W at 28 V is fine continuously, 52 W at 36 V and 92 W at 48 V
 * only in bursts.  A contract above the threshold that lasts longer than
 * PD_BENCH_HIGH_V_MAX_MS ends in a Hard Reset (back to vSafe5V; the bench
 * sink does not re-enter EPR on its own).  The host script keeps the
 * 10-minute average; this is the backstop.  0 ms turns it off.
 */
#define PD_BENCH_HIGH_V_MV          28000u
#if PD_EPR_PDO_36V_ENABLE
#define PD_BENCH_HIGH_V_MAX_MS      2000u
#else
#define PD_BENCH_HIGH_V_MAX_MS      0u
#endif

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
 * PD_VBUS_LOST_PCT / PD_VBUS_LOST_MS — Regulation lost after PS_RDY
 * (2026-10-15).  When V_out stays more than PD_VBUS_LOST_PCT under the
 * regulator's commanded setpoint (which follows the slew, so ramps do not
 * count) for PD_VBUS_LOST_MS while a contract is in place, the source sends
 * a Hard Reset rather than keep claiming a voltage it cannot hold: 28 V into
 * 25 ohm collapsed to 22 V in boost after PS_RDY.
 */
#define PD_VBUS_LOST_PCT            8u
#define PD_VBUS_LOST_MS             200u

/**
 * PD_SPR_TRANSITION_BUDGET_MS / PD_EPR_TRANSITION_BUDGET_MS — How long the
 * source may take to reach a new voltage before PS_RDY.  The sink's
 * tPSTransition is 450 ms min (SPR) and 830 ms min (EPR); the PE has
 * already spent up to tSrcTransition (35 ms) before SetupNewPower.  The EPR
 * budget applies to every request made in EPR mode, SPR positions included
 * (usbpd_pwr_if.c).
 */
#define PD_SPR_TRANSITION_BUDGET_MS 400u
#define PD_EPR_TRANSITION_BUDGET_MS 750u

/** Setpoint slew of the regulator, up and down, mV per ms (regulator.c
 *  step 4a). */
#define PD_SLEW_UP_MV_PER_MS \
    ((SETPOINT_SLEW_UP_MV_PER_CYCLE) * (PID_EXECUTION_RATE_HZ) / 1000u)
#define PD_SLEW_DOWN_MV_PER_MS \
    ((SETPOINT_SLEW_DOWN_MV_PER_CYCLE) * (PID_EXECUTION_RATE_HZ) / 1000u)

/** Worst-case time to slew from vSafe5V to V (a request can follow a Hard
 *  Reset, which always restarts at 5 V). */
#define PD_RAMP_FROM_5V_MS(mv)      (((mv) - 5000u) / PD_SLEW_UP_MV_PER_MS)

/** Time for the setpoint to come down from V to vSafe5V (EPR exit, or a
 *  sink dropping to 5 V).  VBUS follows only as fast as the load drains it
 *  until reverse pulses exist (plans/epr-bringup.md), so this bounds the
 *  setpoint, not the output. */
#define PD_RAMP_TO_5V_MS(mv)        (((mv) - 5000u) / PD_SLEW_DOWN_MV_PER_MS)

/* =========================================================================
 * Derived values and checks
 * =========================================================================*/

#define PD_SPR_MAX_MV                                                    \
    (PD_SRC_PDO_24V_ENABLE ? 24000u :                                    \
     PD_SRC_PDO_20V_ENABLE ? 20000u :                                    \
     PD_SRC_PDO_15V_ENABLE ? 15000u :                                    \
     PD_SRC_PDO_9V_ENABLE  ?  9000u : 5000u)

_Static_assert(PD_SRC_MAX_CURRENT_MA >= 100u && PD_SRC_MAX_CURRENT_MA <= 3000u,
               "PD_SRC_MAX_CURRENT_MA: 100..3000 mA (above 3 A needs a 5 A e-marked cable)");
_Static_assert(PD_EPR_28V_CURRENT_MA >= 100u && PD_EPR_28V_CURRENT_MA <= 5000u,
               "PD_EPR_28V_CURRENT_MA: 100..5000 mA");
/* PD_BENCH_15V / PD_BENCH_20V (bench options).  At the old 20 V/s up-slew
 * 15 and 20 V had to be reached in steps (5 -> 9 -> 15 -> 20 V); at 80 V/s
 * (2026-10-11) 5 -> 20 V takes 188 ms, so every SPR PDO is reachable
 * straight from 5 V and the general checks below cover them. */
#if defined(PD_BENCH_20V) && (PD_BENCH_20V) && !(defined(PD_BENCH_15V) && (PD_BENCH_15V))
#error "PD_BENCH_20V needs PD_BENCH_15V"
#endif
_Static_assert(PD_RAMP_FROM_5V_MS(PD_SPR_MAX_MV) <= PD_SPR_TRANSITION_BUDGET_MS,
               "highest SPR PDO cannot be reached from 5 V within tPSTransition at "
               "SETPOINT_SLEW_UP_MV_PER_CYCLE; the sink would Hard Reset. Raise the "
               "regulator up-slew (bench-validate overshoot first) or drop the PDO");
_Static_assert(PD_RAMP_TO_5V_MS(PD_SPR_MAX_MV) <= PD_SPR_TRANSITION_BUDGET_MS,
               "the setpoint cannot come down from the highest SPR PDO to 5 V within "
               "tPSTransition at SETPOINT_SLEW_DOWN_MV_PER_CYCLE");
_Static_assert(!PD_EPR_PDO_28V_ENABLE ||
               PD_RAMP_FROM_5V_MS(28000u) <= PD_EPR_TRANSITION_BUDGET_MS,
               "28 V cannot be reached from 5 V within EPR tPSTransition at "
               "SETPOINT_SLEW_UP_MV_PER_CYCLE");
_Static_assert(!PD_EPR_PDO_28V_ENABLE ||
               PD_RAMP_TO_5V_MS(28000u) <= PD_EPR_TRANSITION_BUDGET_MS,
               "the setpoint cannot come down from 28 V to 5 V within EPR "
               "tPSTransition at SETPOINT_SLEW_DOWN_MV_PER_CYCLE");
_Static_assert(!PD_EPR_PDO_28V_ENABLE || 28000u * 110u / 100u < OVP_ABSOLUTE_MV,
               "28 V EPR PDO with its relative OVP must stay under OVP_ABSOLUTE_MV");
#if (defined(PD_BENCH_EPR_36V) && (PD_BENCH_EPR_36V)) && !(defined(PD_BENCH_EPR_28V) && (PD_BENCH_EPR_28V))
#error "PD_BENCH_EPR_36V needs PD_BENCH_EPR_28V"
#endif
#if (defined(PD_BENCH_EPR_48V) && (PD_BENCH_EPR_48V)) && !(defined(PD_BENCH_EPR_36V) && (PD_BENCH_EPR_36V))
#error "PD_BENCH_EPR_48V needs PD_BENCH_EPR_36V"
#endif
#define PD_EPR_MAX_MV                                                    \
    (PD_EPR_PDO_48V_ENABLE ? 48000u :                                    \
     PD_EPR_PDO_36V_ENABLE ? 36000u :                                    \
     PD_EPR_PDO_28V_ENABLE ? 28000u : PD_SPR_MAX_MV)
_Static_assert(PD_RAMP_FROM_5V_MS(PD_EPR_MAX_MV) <= PD_EPR_TRANSITION_BUDGET_MS,
               "highest EPR PDO cannot be reached from 5 V within EPR tPSTransition");
_Static_assert(PD_RAMP_TO_5V_MS(PD_EPR_MAX_MV) <= PD_EPR_TRANSITION_BUDGET_MS,
               "the setpoint cannot come down from the highest EPR PDO to 5 V within "
               "EPR tPSTransition");
_Static_assert(PD_EPR_MAX_MV * OVP_RELATIVE_PCT / 100u <= OVP_ABSOLUTE_MV,
               "highest EPR PDO with its relative OVP must stay at or under OVP_ABSOLUTE_MV");
_Static_assert(PD_EPR_MAX_MV <= SETPOINT_MAX_MV, "EPR PDO above SETPOINT_MAX_MV");
_Static_assert(PD_SPR_MAX_MV <= SETPOINT_MAX_MV, "PDO above SETPOINT_MAX_MV");

#endif /* PD_BENCH_CONFIG_H */
