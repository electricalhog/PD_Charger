/**
 * @file    regulator.c
 * @brief   Buck-boost regulator state machine, HRTIM post-init, and ISRs
 *          (NLSpec §4–§13).
 *
 * This file owns:
 *   - System state machine (INIT/IDLE/RUNNING/FAULT) and mode (BUCK/BOOST)
 *   - HRTIM post-init configuration (fault levels, fault enable, compare regs)
 *   - Soft-start ramp logic
 *   - Mode selection and mode transition sequence
 *   - Software safety checks (OVP, UVP, V_in range, backstop count)
 *   - ISR bodies for TIM7 (PID), HRTIM Timer A period, HRTIM fault
 *   - Power-path GPIO sequencing
 *
 * ============================================================
 * HRTIM output naming conventions (§5.2, §5.3, §3.6):
 *   CHA1 (PA8)  = Timer A Output 1 = input-side high-side FET (Q1)
 *   CHA2 (PA9)  = Timer A Output 2 = input-side low-side  FET (Q2, complementary)
 *   CHB1 (PA10) = Timer B Output 1 = output-side high-side FET (Q3)
 *   CHB2 (PA11) = Timer B Output 2 = output-side low-side  FET (Q4, complementary)
 *
 * Buck mode:  Timer A switches (CHA1 Set=Period, Reset=EEV4|CMP2)
 *             Timer B static   (CHB1 Set=CMP3,   Reset=Period — bootstrap refresh)
 *
 * Boost mode: Timer B switches (CHB1 Set=EEV4|CMP2, Reset=Period)
 *             Timer A static   (CHA1 Set=CMP3,       Reset=Period — bootstrap refresh)
 *
 * Both timer Set/Reset sources are pre-configured by CubeMX (final.ioc,
 * confirmed v0.1.2i, §5.5).  No polarity swap is needed at mode transition.
 * ============================================================
 *
 * ============================================================
 * HRTIM compare register assignments (§10.3):
 *   CMP1xR — Blanking window end: EEV4 is masked until CMP1 fires.
 *             Timer A CMP1 = HRTIM_BLANKING_TICKS_BUCK  (buck active leg)
 *             Timer B CMP1 = HRTIM_BLANKING_TICKS_BOOST (boost active leg)
 *             Blanking filter: HRTIM_TIMEEVFLT_BLANKINGCMP1 (configured in IOC).
 *   CMP2xR — Hardware backstop: ends the charge phase if EEV4 has not fired.
 *             Both timers: MAX_ON_TIME_COUNTS.
 *             Active leg RST source (buck): HRTIM_OUTPUTRESET_TIMCMP2.
 *             Active leg SET source (boost): HRTIM_OUTPUTSET_TIMCMP2.
 *   CMP3xR — Bootstrap refresh end: static leg resumes HIGH after LOW pulse.
 *             Both timers: BOOTSTRAP_REFRESH_TICKS.
 *             Static leg SET source: HRTIM_OUTPUTSET_TIMCMP3.
 * ============================================================
 *
 * ============================================================
 * HRTIM register access (RM0440 §27):
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R  = Timer A SET1R
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R  = Timer A RST1R
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R  = Timer B SET1R
 *   HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R  = Timer B RST1R
 *   HRTIM1->sTimerxRegs[x].CMP1xR = Compare 1 (blanking end)
 *   HRTIM1->sTimerxRegs[x].CMP2xR = Compare 2 (backstop)
 *   HRTIM1->sTimerxRegs[x].CMP3xR = Compare 3 (bootstrap refresh pulse end)
 *   HRTIM1->sCommonRegs.OENR       = Output enable register
 *   HRTIM1->sCommonRegs.ODISR      = Output disable register
 * ============================================================
 */

#include "regulator.h"
#include "regulator_config.h"
#include "pid_controller.h"
#include "slope_comp.h"
#include "adc_monitor.h"
#include "pd_interface.h"
#include "debug_log.h"
#include "main.h"           /* hhrtim1, hdac3, htim6, htim7, hcomp1 handles */
#include "stm32g4xx_hal.h"
#include <string.h>         /* memset */

/* Outputs of the buck switching leg (Timer A).  Both outputs are always
 * enabled: with SYNC_RECT_ENABLED 0 the low side (TA2, Q2) carries only the
 * bootstrap refresh pulse at the start of each period, never the
 * synchronous-rectifier conduction: see regulator_config.h.               */
#define HRTIM_BUCK_SWITCHING_OUTPUTS (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2)
#if SYNC_RECT_ENABLED
#define HRTIM_BOOST_SWITCHING_OUTPUTS (HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2)
#else
/* Diode emulation on the output leg (2026-09-28, run 31).  Timer B drives
 * TB2 (Q4, the boost charge switch) as the dead-time complement of TB1, so
 * with TB1 (Q3) enabled the leg is forced CCM: at no load the inductor
 * current reverses through Q3 every period and V_out is pinned at
 * V_in / (1 - D) by the backstop (run 30: 26.7 V at 600 ns, the loop railed
 * at 28 V).  With TB1 never enabled Q3 conducts only in reverse, the leg is
 * DCM, and the same pulse skipping and on-time control as buck apply.  The
 * dead-time generator keeps producing TB2 from TB1's internal waveform
 * while the TB1 pin is disabled; that is checked on the scope (SW2 low for
 * t_on each period), not assumed.  Q3's bootstrap refreshes while Q4 is on. */
#define HRTIM_BOOST_SWITCHING_OUTPUTS (HRTIM_OUTPUT_TB2)
#endif

/** 1 while the buck charge pulses are held off (V_out above the setpoint);
 *  telemetry, written every switching period by the Timer A period ISR.  */
volatile uint8_t regulator_pulses_skipped = 0u;

/** V_out level above which the next charge pulse is skipped, in raw ADC1
 *  counts of VD_MON (so the period ISR compares without scaling).  Written
 *  by the PID ISR from the effective (soft-start) setpoint.               */
volatile uint16_t regulator_skip_raw_threshold = 0xFFFFu;

/** Glitch-free V_out in raw ADC counts: the VD_MON sample read once per
 *  switching period, bounded by what the power stage can physically do.
 *
 *  Why (2026-09-27, runs 10 to 13, period trace): ADC1 free-runs its
 *  4-channel scan, so its VD_MON sampling window slides through the 5 us
 *  switching period, and samples land anywhere on the ringing the switch
 *  node carries (TP3 rings at about 1.6 MHz, several volts, for the whole
 *  period at no load) and couples into the sense divider; a bad sample
 *  reads 1 to 3 V off while V_out is steady.  Run 10 showed single bad
 *  samples every 9 to 24 periods with 2.5-cycle sampling (also 25 percent
 *  low: the sample capacitor never settled through the 5.5 kOhm divider
 *  after the near-zero IS_MON channel); with 92.5-cycle sampling (run 12,
 *  final.ioc and MX_ADC1_Init) the scale is right and the bad samples come
 *  in pairs.  A median of three (run 11) passed pairs.  A symmetric slew
 *  limiter with a 12-count acceptance band (run 13) held 5.0 V for 19.7 s
 *  and then let a staircase of bad samples (365, 377 counts, each inside
 *  the band of the last) walk the estimate to 5.52 V and trip OVP while
 *  every pulse was being skipped.
 *
 *  The rule here is the physics instead of a band.  Charge only enters the
 *  output through a charge pulse, so the estimate may rise only by what the
 *  pulses issued so far can have delivered: every period whose pulse is
 *  enabled adds VOUT_RISE_PER_PULSE_RAW to a rise budget, capped at
 *  VOUT_RISE_BUDGET_MAX_RAW, and the estimate rises by at most the budget,
 *  consuming it.  One raw count is 19.1 mV (VD_MON_FULL_SCALE_MV,
 *  calibrated 2026-09-28).  A 600 ns pulse (DCM_MAX_ON_TIME_NS) at 24 V in
 *  and 5 V out measured 2 to 2.7 uC on the scope (runs 21 and 22,
 *  2026-09-28), 90 to 140 mV on the 19 to 28 uF the output behaves as, 5 to
 *  7 counts; at the 400 ns the on-time control settles to under load the
 *  scope shows about 20 mV per pulse (run 28, 100 mV ripple over bursts of
 *  5).  The credit is 4 counts and at most 8 are outstanding.  The ADC
 *  glitches this has to reject (run 20 period trace, with 0.1 nF across R16
 *  and R18) are 60 to 100 counts in either direction, last one ADC scan
 *  (two switching periods) and come in adjacent pairs, so a glitch right
 *  after a burst moves the estimate by at most the outstanding budget,
 *  150 mV, against an OVP margin of 500 mV at 5 V; with a credit of 8 and
 *  16 outstanding (run 28) the estimate reached 5.49 V on glitches while
 *  the scope showed 5.0 to 5.1 V.  The
 *  per-period form used in runs 14 to 27 (rise 6 counts in each of the 3
 *  periods after a pulse) let two glitch pairs walk the estimate 18 counts
 *  through 5.5 V in run 27 while the raw samples around them read 5.0 V
 *  (period trace 20260928-082227).  It may fall by VOUT_FALL_RAW per period
 *  (4 counts, 76 mV; the bench load is 230 mA on the effective 19 to 28 uF,
 *  about 3 counts per period); a heavier load needs that bound raised or
 *  the load current fed in.  The PID ISR uses the same value, so PID,
 *  soft-start, OVP, UVP, pulse skipping and the debug log all see it; the
 *  raw sample stays in adc_measurements.v_out_mv.  The proper fix is analog
 *  (a capacitor on the VD_MON node) or a hardware-timed sample once some
 *  phase of the period is quiet; this is the bring-up workaround.        */
#define VOUT_RISE_PER_PULSE_RAW   4u   /* 76 mV of rise credit per pulse    */
#define VOUT_RISE_BUDGET_MAX_RAW  8u   /* two pulses' worth outstanding     */
#define VOUT_FALL_RAW             4u   /* 76 mV per period                  */
/* A sample above the estimate for this many consecutive periods is a real
 * rise whatever the pulses say, and the estimate snaps to the lowest sample
 * of that run.  Glitches last one ADC scan, two periods, so the lowest of
 * four is never a glitch; snapping to the latest sample instead (runs 33
 * and 34) let noise one or two counts above the estimate followed by a
 * glitch pair count as four and lift the estimate to the glitch: 1527,
 * 1527, 1673, 1673 raw at 29 V set it to 32 V and the OVP tripped (run 34,
 * trace 20260928-085639; run 35 fault snapshot, estimate 1631 against a raw
 * 1514 while it decayed).  Run 31 (2026-09-28, boost, no load):
 * with every pulse skipped V_out climbed from 29 to 32 V (period trace
 * 20260928-084508, raw 1677 for 2.5 ms) while the estimate, allowed to
 * rise only on pulse credit, stayed at 29.1 V and the OVP never saw it;
 * the static leg's bootstrap refresh switching pumps the output through
 * Q3's reverse conduction (this seat's reading of the trace, not
 * scoped).  Four periods is 20 us of lag on a real rise.               */
/* Six since 2026-09-28 evening (boost 28 V into 330 ohm): two adjacent glitch
 * pairs, 34.1 34.1 32.3 32.3 V between 28.3 V samples, made four and set the
 * estimate to 32.3 V: SW_OVP (period trace 20260928-1716).  Six is three ADC
 * scans, 30 us of lag on a real rise.                                     */
#define VOUT_RISE_PERSIST_PERIODS 6u
/* ...and only if the run's samples agree within this many raw counts (15 is
 * 290 mV).  Glitch runs jump by volts (31.0 41.1 41.1 31.5 V between 27.6
 * and 26.9 V samples beat six periods the same evening); the real rises the
 * pulse credit misses are slow (run 31's refresh pumping, about 6 mV per
 * period).  A wider run restarts from its latest sample.                 */
#define VOUT_RISE_PERSIST_SPREAD_RAW 15u
volatile uint16_t regulator_vout_est_raw = 0u;
/** Raw counts the estimate may still rise by, earned by enabled pulses. */
static volatile uint8_t vout_rise_budget_raw = 0u;
/** Consecutive periods the raw sample has been above the estimate, and the
 *  lowest sample in that run. */
static volatile uint8_t  vout_above_periods = 0u;
static volatile uint16_t vout_above_min_raw = 0u;
static volatile uint16_t vout_above_max_raw = 0u;

/* Boost precharge (2026-09-28, runs 32 and 33).  Starting boost directly
 * turns Q1, the input leg's static switch, fully on into an empty output:
 * V_in steps across L1 and C_out, the tank rings toward 2 * V_in, and Q3's
 * reverse conduction (diode emulation, SYNC_RECT_ENABLED 0) holds the
 * peak.  Run 33's period trace read 0, 1.0, 8.6 and then 38 V within
 * 25 us of the start (20260928-085222), over the 30.8 V relative OVP of a
 * 28 V target.  So a boost start runs the buck leg first, ramping to
 * BOOST_PRECHARGE_BELOW_VIN_MV under V_in, and the PID ISR (step 4b) hands
 * over to boost once the output is there; the step Q1 then applies is
 * that margin, and the ring on it at most twice that.                    */
static volatile bool boost_precharge = false;

/** PID cycles in a row with V_in outside the mode's range (see the V_in
 *  range check in software_safety_checks_pass). */
#define VIN_RANGE_PERSIST_CYCLES 20u   /* 1 ms at 20 kHz */
static uint16_t vin_range_cycles = 0u;

/* What the software safety check saw when it last latched a fault, for the
 * bench (2026-09-28, run 34: SW_OVP with the estimate at 31.2 V while the
 * period trace and debug log up to the fault read 29.1 V).  u32 each:
 * v_out_mv (the estimate), v_in_mv, the raw VD_MON sample, the estimate in
 * raw counts, the setpoint passed, the PID ISR count at the fault.       */
volatile uint32_t regulator_fault_snapshot[6] = {0u};

/** Hysteresis on the skip decision, raw counts (3 counts is 57 mV).      */
#define SKIP_HYSTERESIS_RAW 3u

/** Per-period trace of what the skip decision saw: bits 0-11 the raw
 *  VD_MON count read in the Timer A period ISR, bit 15 set when that period
 *  was skipped.  512 entries is 2.56 ms; the ISR stops writing on a fault,
 *  so the ring holds the last 2.56 ms before it.  Bring-up instrumentation
 *  (2026-09-27, run 9), read with `bu mem read regulator_period_trace`.  */
#define PERIOD_TRACE_LENGTH 512u
volatile uint16_t regulator_period_trace[PERIOD_TRACE_LENGTH];
volatile uint16_t regulator_period_trace_index = 0u;

/* External peripheral handles declared in main.c */
extern HRTIM_HandleTypeDef  hhrtim1;
extern DAC_HandleTypeDef    hdac3;
extern TIM_HandleTypeDef    htim7;
extern COMP_HandleTypeDef   hcomp1;

/* =========================================================================
 * HRTIM convenience bit definitions
 *
 * These bit positions follow RM0440 §27.5.x.  CMSIS (stm32g474xx.h) defines
 * HRTIM_SET1R_CMPx and HRTIM_RST1R_CMPx directly; fallback defines are
 * provided below for each used compare unit in case the header is absent.
 * =========================================================================*/

/** HRTIM TIMxCR register: FLT1EN (bit 20) and FLT2EN (bit 21) */
#define HRTIM_TIMCR_FLT1EN_BIT  (1UL << 20)
#define HRTIM_TIMCR_FLT2EN_BIT  (1UL << 21)

/* Compare 1 (CMP1): blanking window end.  Bit 3 in SET1R/RST1R. */
#ifndef HRTIM_RST1R_CMP1
#define HRTIM_RST1R_CMP1   (1UL << 3)
#endif
#ifndef HRTIM_SET1R_CMP1
#define HRTIM_SET1R_CMP1   (1UL << 3)
#endif

/* Compare 2 (CMP2): hardware backstop.  Bit 4 in SET1R/RST1R. */
#ifndef HRTIM_RST1R_CMP2
#define HRTIM_RST1R_CMP2   (1UL << 4)
#endif
#ifndef HRTIM_SET1R_CMP2
#define HRTIM_SET1R_CMP2   (1UL << 4)
#endif

/* Compare 3 (CMP3): bootstrap refresh end.  Bit 5 in SET1R/RST1R. */
#ifndef HRTIM_RST1R_CMP3
#define HRTIM_RST1R_CMP3   (1UL << 5)
#endif
#ifndef HRTIM_SET1R_CMP3
#define HRTIM_SET1R_CMP3   (1UL << 5)
#endif

/* =========================================================================
 * State machine variables
 * =========================================================================*/

/** Current FSM state — written from ISR and API; aligned 32-bit enum. */
static volatile RegulatorState regulator_state = REGULATOR_STATE_INIT;

/** Current operating mode */
static volatile RegulatorMode  regulator_mode  = REGULATOR_MODE_BUCK;

/* =========================================================================
 * Runtime-mutable parameter definitions (extern in regulator.h)
 * =========================================================================*/

/* kp is DAC counts per mV of error.  1.0 (3.2 A of peak current per volt)
 * was about 16 times too much for the plant: with 40 uF of output
 * capacitance one PID period of 1 A average moves V_out by 1.25 V, so the
 * loop gain per step was about 4 and every correction overshot (bench,
 * 2026-09-27, run 1).  0.05 gives a loop gain per step near 0.2 with the
 * nominal capacitance and 0.4 if the ceramics derate to half.           */
/* Run 3 (2026-09-27, 24 V in, no load): with no load the output settles
 * within one PID period, so the plant is nearly memoryless with about
 * 12 mV of V_out per DAC count.  kp 0.05 and ki 400 gave a loop that
 * alternated every sample and grew.  kp 0.01 and ki 300 put the discrete
 * poles near 0.84 and -0.14 for that gain.                                */
/* Runs 3 to 18 were tuned against a plant that did not exist: Q1 was
 * conducting as a resistor (regulator_config.h, SYNC_RECT_ENABLED), so the
 * DAC had no effect and the integrator did all the work.  Runs 19 to 22 had
 * the PID railed at PID_OUTPUT_MAX because the skip band sat at the
 * setpoint (regulator_config.h, PULSE_SKIP_ABOVE_MV).  Run 23 (2026-09-28,
 * 24 V in, 33 ohm load, 5 V) is the first with the DAC in charge: kp 0.2,
 * ki 300 alternated every PID period, DAC 30 to 160 counts against V_out
 * 4.7 to 5.3 V, so the plant near the minimum pulse is about 4.6 mV of
 * V_out per DAC count per PID period (the 1 mV estimated from the ideal
 * ramp was low by the trip-delay overshoot and the real decay time), and
 * kp 0.2 was a loop gain per step near 1.  Runs 25 and 26 then showed that
 * the DAC only reached the pulses below about 150 mV of threshold, so that
 * plant was the skip band, not the DAC.  With the on-time computed from the
 * peak command (PID ISR step 6b) the plant is dQ/dI_pk = I_pk * L *
 * (1/(V_in - V_out) + 1/(V_out + V_sd)) per pulse, ten pulses per PID
 * period, on about 19 uF (measured from the no-pulse decay at 232 mA, run
 * 26): at the 1.6 A a 232 mA load needs at 5 V that is 2.5 mV of V_out per
 * DAC count per PID period.  kp 0.1 is a loop gain per step of 0.25; ki
 * 300 (0.015 counts per mV per step) is a 1.3 ms integrator time constant
 * against it.                                                             */
float   pid_kp = 0.1f;
float   pid_ki = 300.0f;
float   pid_kd = 0.0f;

/** Peak-current floor applied on the last PID cycle (DAC counts); telemetry. */
volatile uint16_t regulator_peak_floor_counts = 0u;

/**
 * peak_current_floor_counts — Lowest peak-current command that keeps the
 * average inductor current above −PEAK_FLOOR_NEG_MARGIN_MA.
 *
 * In peak-current mode the average inductor current is I_peak − ΔI/2, with
 * ΔI the ripple set by the voltages, the period and the inductor:
 *   buck : ΔI = (V_in − V_out) · (V_out / V_in) · T / L
 *   boost: ΔI = V_in · (1 − V_in / V_out) · T / L
 * mV × ns / nH gives mA directly.  Returns DAC counts clamped to
 * [PID_OUTPUT_MIN, PID_OUTPUT_MAX / 2].
 */
static uint32_t peak_current_floor_counts(uint32_t v_in_mv, uint32_t v_out_mv,
                                          RegulatorMode mode)
{
    const uint64_t period_ns = 1000000000ull / (uint64_t)HRTIM_SWITCHING_FREQ_HZ;
    uint64_t ripple_ma = 0u;

    if (mode == REGULATOR_MODE_BUCK)
    {
        if (v_in_mv > v_out_mv && v_in_mv > 0u)
        {
            ripple_ma = ((uint64_t)(v_in_mv - v_out_mv) * v_out_mv / v_in_mv)
                        * period_ns / (uint64_t)INDUCTOR_VALUE_NH;
        }
    }
    else
    {
        if (v_out_mv > v_in_mv && v_out_mv > 0u)
        {
            ripple_ma = ((uint64_t)v_in_mv * (v_out_mv - v_in_mv) / v_out_mv)
                        * period_ns / (uint64_t)INDUCTOR_VALUE_NH;
        }
    }

    int64_t floor_ma     = (int64_t)(ripple_ma / 2u) - (int64_t)PEAK_FLOOR_NEG_MARGIN_MA;
    int64_t floor_counts = floor_ma * (int64_t)DAC_COUNTS_PER_AMP / 1000;
    if (floor_counts < (int64_t)PID_OUTPUT_MIN)     { floor_counts = (int64_t)PID_OUTPUT_MIN; }
    if (floor_counts > (int64_t)PID_OUTPUT_MAX / 2) { floor_counts = (int64_t)PID_OUTPUT_MAX / 2; }
    return (uint32_t)floor_counts;
}

/** Trip-delay compensation applied on the last PID cycle (DAC counts); telemetry. */
volatile uint16_t regulator_trip_delay_comp_counts = 0u;
/** Charge-pulse on-time commanded this PID period, ns (buck, DCM). */
volatile uint16_t regulator_on_time_ns = 0u;

/**
 * trip_delay_comp_counts — How far the real peak overshoots the DAC threshold
 * because of PEAK_TRIP_DELAY_NS, in DAC counts (regulator_config.h).
 * di/dt during the on-time is (V_in − V_out)/L in buck and V_in/L in boost;
 * mV / nH is mA per ns.
 */
static uint32_t trip_delay_comp_counts(uint32_t v_in_mv, uint32_t v_out_mv,
                                       RegulatorMode mode)
{
    uint64_t dv_mv;
    if (mode == REGULATOR_MODE_BUCK)
    {
        dv_mv = (v_in_mv > v_out_mv) ? (uint64_t)(v_in_mv - v_out_mv) : 0u;
    }
    else
    {
        dv_mv = (uint64_t)v_in_mv;
    }
    uint64_t overshoot_ma = dv_mv * (uint64_t)PEAK_TRIP_DELAY_NS / (uint64_t)INDUCTOR_VALUE_NH;
    return (uint32_t)(overshoot_ma * (uint64_t)DAC_COUNTS_PER_AMP / 1000u);
}

uint8_t max_consecutive_backstops          = (uint8_t)MAX_CONSECUTIVE_BACKSTOPS_DEFAULT;
uint8_t pid_integrator_reset_threshold_pct = 20u;

volatile bool regulator_integrator_reset_requested = false;

/* =========================================================================
 * Status / telemetry definitions (extern in regulator.h)
 * =========================================================================*/

RegulatorFaultSource regulator_last_fault_source    = REGULATOR_FAULT_NONE;
volatile uint32_t    regulator_fault_tick_ms         = 0u;
RegulatorDebugMailbox regulator_debug                = {0};
volatile bool        regulator_bench_pwm_active      = false;

/* Timer A/B set/reset sources saved while bench PWM strips EEV4 from them. */
static uint32_t bench_saved_set_rst[4];
volatile uint16_t    regulator_pid_output_dac_counts = 0u;
volatile int32_t     regulator_pid_error_mv          = 0;

/* =========================================================================
 * PID controller instance
 * =========================================================================*/

static PidState  pid_state;
static PidConfig pid_config;

/* =========================================================================
 * Soft-start state
 * =========================================================================*/

/** Soft-start incremental setpoint (ramps from 0 to target_voltage_mv). */
static volatile uint32_t softstart_setpoint_mv = 0u;

/** True while the soft-start ramp is in progress. */
static volatile bool softstart_active = false;

/* OUTPUT_EN is asserted by the PID ISR (step 4c) once V_out is regulating,
 * not by power_path_enable(); see OUTPUT_CONNECT_MARGIN_MV.              */
static volatile bool output_switch_on = false;

_Static_assert(DCM_PULSE_START_TICKS + HRTIM_NS_TO_TICKS(DCM_ON_TIME_CEIL_NS) +
               DCM_REFRESH_DEADTIME_TICKS < HRTIM_PERIOD_COUNTS,
               "DCM_ON_TIME_CEIL_NS does not fit in the period");

/** The target the loop regulates to: target_voltage_mv followed at
 *  SETPOINT_SLEW_MV_PER_CYCLE (PID ISR step 4a).  Telemetry for bu.      */
volatile uint32_t regulator_commanded_mv = 0u;

/** V_in low-passed for mode selection (VIN_FILTER_SHIFT).                */
volatile uint32_t regulator_vin_filt_mv = 0u;

/** Peak-current clamp for the PID output, DAC counts (DCM_MAX_PEAK_MA).  */
#define PEAK_CMD_MAX_COUNTS ((DCM_MAX_PEAK_MA * DAC_COUNTS_PER_AMP) / 1000u)
#define PEAK_CMD_MAX_COUNTS_BB ((DCM_MAX_PEAK_MA_BB * DAC_COUNTS_PER_AMP) / 1000u)
_Static_assert(PEAK_CMD_MAX_COUNTS_BB <= PID_OUTPUT_MAX, "DCM_MAX_PEAK_MA_BB over PID_OUTPUT_MAX");

/** ISR cost in CPU cycles (DWT CYCCNT, 170 MHz), bring-up instrumentation
 *  (2026-09-28 evening: uwTick ran at 0.02 of real time in boost, the PID
 *  ISR active in every NVIC sample).  [0] last, [1] max, [2] running mean
 *  x16.  pid includes time preempted by the Timer A ISR.  Zero the max with
 *  a debugger write.                                                      */
volatile uint32_t regulator_cyc_pid[3];
volatile uint32_t regulator_cyc_tima[3];
volatile uint32_t regulator_cyc_vin_poll[3];
static inline void cyc_record(volatile uint32_t *c, uint32_t n)
{
    c[0] = n;
    if (n > c[1]) { c[1] = n; }
    c[2] = c[2] - (c[2] >> 4) + n;   /* mean x16 */
}

/** Last 8 mode changes, for the bench: uwTick, from | to << 8, commanded
 *  mV, filtered V_in mV, V_out mV (estimate).  Index of the next entry.    */
volatile uint32_t regulator_mode_log[8][5];
volatile uint32_t regulator_mode_log_index = 0u;

/** Increment per PID cycle to reach target in SOFT_START_RAMP_MS (§11.2). */
static uint32_t softstart_increment_mv = 0u;

/* =========================================================================
 * Backstop counter
 * =========================================================================*/

/** Consecutive periods in which CMP2 fired before COMP1 (§10.4). */
static volatile uint8_t consecutive_backstop_count = 0u;

/* =========================================================================
 * Internal helper declarations
 * =========================================================================*/

static void hrtim_configure_fault_levels_and_enable(void);
static void hrtim_configure_compare_registers(void);
static void hrtim_enable_period_and_fault_interrupts(void);
static void hrtim_fault_irq_disarm(void);
static void hrtim_fault_irq_rearm(void);
static bool vs_good(void);
static bool is_good(void);
static bool input_path_power_up(RegulatorFaultSource *failure);
static void bench_pwm_restore(void);
static void hrtim_start_timers(void);
static void hrtim_apply_buck_mode_static_leg(void);
static void hrtim_apply_boost_mode_static_leg(void);
static void hrtim_apply_buck_boost_legs(void);
static uint32_t mode_switching_outputs(RegulatorMode mode);
static void hrtim_disable_all_outputs(void);
static void change_mode_while_running(RegulatorMode new_mode, uint32_t v_out_mv,
                                      uint32_t voltage_mv);
static void power_path_enable(void);
static void power_path_disable(void);
static RegulatorMode determine_mode_from_voltages(uint32_t v_in_mv, uint32_t v_setpoint_mv);
static void enter_fault(RegulatorFaultSource source);
static bool software_safety_checks_pass(uint32_t v_out_mv, uint32_t v_in_mv,
                                         uint32_t v_setpoint_mv, RegulatorMode mode);

/* =========================================================================
 * regulator_init
 * =========================================================================*/

void regulator_init(void)
{
    /* --- Step 1: Reconfigure HRTIM output fault levels and enable fault
     *             response on Timer A and Timer B (§5.5, §10.1).
     *  CubeMX sets FaultLevel = NONE and FaultEnable = NONE — this must
     *  be corrected in post-init or faults will not affect the outputs.    */
    hrtim_configure_fault_levels_and_enable();

    /* --- Step 2: Configure HRTIM compare registers (§10.3) ---
     *  CMP1 = blanking window end, CMP2 = backstop, CMP3 = bootstrap end   */
    hrtim_configure_compare_registers();

    /* --- Step 3: Set DAC3 CH1 to 0 (zero current threshold → safe) --- */
    HAL_DAC_SetValue(&hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0u);
    HAL_DAC_Start(&hdac3, DAC_CHANNEL_1);

    /* --- Step 4: Start COMP1 (§6.1) --- */
    HAL_COMP_Start(&hcomp1);

    /* DWT cycle counter for regulator_cyc_* */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* --- Step 5: Initialise ADC subsystem --- */
    adc_monitor_init();
    adc_monitor_start_adc1_dma();

    /* --- Step 6: Initialise slope compensation (configures TIM6 rate,
     *             NVIC priority 0, does NOT start TIM6 yet)              --- */
    slope_comp_init();

    /* --- Step 7: Configure TIM7 (PID) for 20 kHz --- */
    TIM7->PSC = TIM7_PRESCALER;
    TIM7->ARR = TIM7_PERIOD_COUNTS;
    TIM7->EGR = TIM_EGR_UG;   /* force update event to load PSC/ARR */

    /* TODO(debug): During bring-up, verify TIM7 period by scoping a GPIO
     * toggled in regulator_pid_tim7_isr().  Expect ~50 µs (20 kHz). */
    HAL_NVIC_SetPriority(TIM7_DAC_IRQn, NVIC_PRIORITY_PID_TIM7, 0u);
    HAL_NVIC_EnableIRQ(TIM7_DAC_IRQn);

    /* --- Step 8: Initialise PID state and default coefficients (§8.7) --- */
    pid_config.kp          = pid_kp;
    pid_config.ki          = pid_ki;
    pid_config.kd          = pid_kd;
    pid_config.dt_seconds  = 1.0f / (float)PID_EXECUTION_RATE_HZ;  /* 50 µs */
    pid_config.output_min  = (float)PID_OUTPUT_MIN;
    pid_config.output_max  = (float)PEAK_CMD_MAX_COUNTS;

    /* TODO(debug): Kp/Ki/Kd defaults are in regulator_config.h.  Tune via
     * debugger watch on pid_kp/pid_ki/pid_kd (live-writeable) (§8.7). */
    pid_init(&pid_state, &pid_config);

    /* --- Step 9: Enable HRTIM period (Timer A REP) interrupt; route the
     *  fault IRQ in the NVIC but leave FLT1/FLT2 disarmed until RUNNING --- */
    hrtim_enable_period_and_fault_interrupts();

    /* --- Step 10: Start HRTIM counters (outputs stay disabled) --- */
    hrtim_start_timers();

    /* --- Step 11: Apply forced-HIGH to static leg for default mode (BUCK):
     *              CHB1 HIGH, CHB2 LOW via Timer B forced output.
     *  NOTE: Outputs are NOT enabled via OENR here — that happens in
     *        regulator_start().  The forced state is prepared so outputs
     *        can be enabled safely.                                        --- */
    hrtim_apply_buck_mode_static_leg();

    /* --- Step 12: Initialise PD interface shared state --- */
    pd_interface_init();

    /* --- Step 13: Power path GPIO — ensure both paths are disabled at init --- */
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_SET);

    /* --- Step 14: Enter IDLE ---
     * The input path is off, so VS_GOOD (FLT1) is low here by design; the
     * fault IRQ stays disarmed until regulator_start() has VS up.  Never
     * overwrite a latched FAULT: regulator_clear_fault() is the only exit.  */
    if (regulator_state != REGULATOR_STATE_FAULT)
    {
        regulator_state = REGULATOR_STATE_IDLE;
    }
}

/* =========================================================================
 * regulator_start
 * =========================================================================*/

void regulator_start(void)
{
    /* Guard: only start from IDLE, and not while bench PWM owns the outputs */
    if (regulator_state != REGULATOR_STATE_IDLE || regulator_bench_pwm_active)
    {
        return;
    }

    /* Guard: valid setpoint required */
    uint32_t voltage_mv = target_voltage_mv;
    if (voltage_mv < SETPOINT_MIN_MV || voltage_mv > SETPOINT_MAX_MV)
    {
        return;
    }


    /* --- Enable input path FIRST so VS_MON sees the real supply voltage ---
     * INPUT_EN (and OUTPUT_EN) must be asserted before ADC2 can measure a
     * valid V_in.  Allow ≥ 2 ms for the voltage-divider network to settle
     * and for at least one ADC2 conversion to complete before reading back.
     * Called here in task context so HAL_Delay() is safe.                  */
    power_path_enable();

    /* Wait for the ADM1270 to bring VS up (VS_GOOD) with IS_GOOD high.
     * Until then FLT1 is legitimately asserted, so the fault IRQ stays
     * disarmed; failing here latches FAULT (input path dropped).           */
    RegulatorFaultSource input_failure;
    if (!input_path_power_up(&input_failure))
    {
        enter_fault(input_failure);
        return;
    }
    HAL_Delay(2u);

    /* Re-sample V_in with INPUT_EN now asserted. */
    /* --- Take a fresh V_in reading before mode selection (§5.1) ---
     *
     * adc_measurements.v_in_mv is only updated by adc_monitor_read_vin_result()
     * inside the TIM7 PID ISR.  On first entry to regulator_start() the TIM7
     * has not yet run, so the field holds its power-on value of 0.
     *
     * With v_in_mv = 0 the mode selector would always choose BOOST (because
     * target_voltage_mv > 0 + MODE_HYSTERESIS_MV), then the first TIM7
     * execution would read the actual V_in, fail the V_in range check for the
     * selected mode (e.g. BOOST: V_in > V_set − BOOST_VIN_MARGIN_MV; future
     * BUCK_BOOST: similar per-mode check), and call enter_fault() before a
     * single useful PID cycle completes — killing the outputs and leaving the
     * debug buffer nearly empty.
     *
     * Taking a synchronous ADC2 sample here (≤ 1 ms blocking) gives mode
     * selection a valid V_in reading.  The poll timeout is safe from the task
     * context in which regulator_start() is called.                          */
    adc_monitor_trigger_vin();
    adc_monitor_read_vin_result();

    /* --- Determine operating mode based on V_in vs setpoint (§5.1) --- */
    uint32_t v_in_mv = (uint32_t)adc_measurements.v_in_mv;
    regulator_vin_filt_mv  = v_in_mv;
    regulator_commanded_mv = voltage_mv;
    RegulatorMode wanted_mode = determine_mode_from_voltages(v_in_mv, voltage_mv);
    boost_precharge = (wanted_mode == REGULATOR_MODE_BOOST);
    vin_range_cycles = 0u;
    regulator_mode  = boost_precharge ? REGULATOR_MODE_BUCK : wanted_mode;

    /* --- Reset PID integrator (§8.8) --- */
    pid_reset_integrator(&pid_state);
    regulator_integrator_reset_requested = false;

    /* --- Configure soft-start (§11.2) ---
     * Number of PID cycles in SOFT_START_RAMP_MS:
     *   ramp_steps = SOFT_START_RAMP_MS × PID_EXECUTION_RATE_HZ / 1000
     *   At 20 kHz, 5 ms → 100 steps.                                      */
    uint32_t ramp_steps = (uint32_t)SOFT_START_RAMP_MS *
                          (uint32_t)PID_EXECUTION_RATE_HZ / 1000u;
    if (ramp_steps == 0u) { ramp_steps = 1u; }
    /* The V_out estimate starts at 0 and the ramp with it.  Seeding both
     * from the latest ADC1 sample (runs 1 to 31) read 36.5 V at the start
     * of run 32 (2026-09-28, boost), right after INPUT_EN closed the input
     * switch, and the relative OVP tripped on the first PID cycle before
     * anything switched; a single sample there is not trustworthy.  From 0
     * the estimate reaches the real value through VOUT_RISE_PERSIST_PERIODS
     * within 20 us, and while the ramp is below the output every pulse is
     * skipped, so a residual or primed V_out (run 1's 410 mV in buck, about
     * V_in in boost) costs only the ramp time to that voltage.           */
    uint32_t v_out_now_mv = 0u;
    regulator_vout_est_raw = 0u;
    vout_rise_budget_raw   = VOUT_RISE_BUDGET_MAX_RAW;
    vout_above_periods     = 0u;
    if (v_out_now_mv > voltage_mv) { v_out_now_mv = voltage_mv; }
    softstart_increment_mv = (voltage_mv - v_out_now_mv) / ramp_steps;
    if (softstart_increment_mv == 0u) { softstart_increment_mv = 1u; }
    softstart_setpoint_mv = v_out_now_mv;
    softstart_active      = true;

    /* --- Apply forced-HIGH to the static leg for the selected mode --- */
    if (regulator_mode == REGULATOR_MODE_BUCK)
    {
        hrtim_apply_buck_mode_static_leg();
    }
    else if (regulator_mode == REGULATOR_MODE_BUCK_BOOST)
    {
        hrtim_apply_buck_boost_legs();
    }
    else
    {
        hrtim_apply_boost_mode_static_leg();
    }

    /* Power path already enabled above; no second call needed. */

    /* VS is up and both fault lines are healthy: clear the stale flags set
     * while INPUT_EN was low and arm the fault IRQ before switching.        */
    hrtim_fault_irq_rearm();

    /* --- Enable HRTIM switching outputs for the active legs (§5.5) --- */
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, mode_switching_outputs(regulator_mode));

    /* --- Initialise slope compensation and start TIM6 --- */
    adc_monitor_scale_adc1_buffer();
    uint32_t v_out_mv = (uint32_t)adc_measurements.v_out_mv;
    slope_comp_update_step(v_out_mv, v_in_mv,
                            (SlopeCompMode)regulator_mode);
    slope_comp_start(0u);   /* peak starts at 0; ramps up with soft-start */

    /* --- Start PID timer (TIM7) --- */
    HAL_TIM_Base_Start_IT(&htim7);

    regulator_state = REGULATOR_STATE_RUNNING;
}

/* =========================================================================
 * regulator_stop
 * =========================================================================*/

void regulator_stop(void)
{
    /* Leave RUNNING before the outputs go off: the Timer A period ISR
     * re-enables TA1 every period while RUNNING (pulse skipping), and on
     * the bench (2026-09-27, run 19) a stop left TA1 enabled in IDLE because
     * one period fell between the disable and the state change.  A FAULT
     * stays latched: only regulator_clear_fault() re-arms the ADM1270 and
     * releases it.                                                          */
    if (regulator_state != REGULATOR_STATE_FAULT)
    {
        regulator_state = REGULATOR_STATE_IDLE;
    }

    /* Dropping INPUT_EN below makes VS_GOOD (FLT1) fall; that is a normal
     * shutdown, not a fault, so disarm the fault IRQ first.                 */
    hrtim_fault_irq_disarm();

    /* Disable all HRTIM switching outputs immediately (§11.3) */
    hrtim_disable_all_outputs();
    bench_pwm_restore();

    /* Stop slope compensation and zero DAC */
    slope_comp_stop();

    /* Stop PID timer */
    HAL_TIM_Base_Stop_IT(&htim7);

    /* Disable power path */
    power_path_disable();

    /* Reset soft-start state */
    softstart_active      = false;
    softstart_setpoint_mv = 0u;

    /* Reset PID integrator for clean restart */
    pid_reset_integrator(&pid_state);
}

/* =========================================================================
 * regulator_clear_fault
 * =========================================================================*/

RegulatorClearResult regulator_clear_fault(void)
{
    if (regulator_state != REGULATOR_STATE_FAULT)
    {
        return REGULATOR_CLEAR_NOT_IN_FAULT;
    }

    /* enter_fault() dropped INPUT_EN.  The ADM1270 (latch-off mode) ignores
     * ENABLE until its TIMER_OFF off-time has elapsed, so hold it low for
     * the full cool-down measured from the fault.                          */
    power_path_disable();
    uint32_t elapsed = HAL_GetTick() - regulator_fault_tick_ms;
    if (elapsed < ADM1270_COOLDOWN_MS)
    {
        HAL_Delay(ADM1270_COOLDOWN_MS - elapsed);
    }

    /* ENABLE low → high re-arms the ADM1270.  Bring VS up with the HRTIM
     * outputs and output path still off to prove the input is healthy,
     * then return to the IDLE power-path state (input off).                */
    RegulatorFaultSource failure;
    bool input_ok = input_path_power_up(&failure);
    power_path_disable();

    if (!input_ok)
    {
        regulator_last_fault_source = failure;
        regulator_fault_tick_ms     = HAL_GetTick();
        return (failure == REGULATOR_FAULT_HW_FLT2) ? REGULATOR_CLEAR_INPUT_OVERCURRENT
                                                    : REGULATOR_CLEAR_INPUT_NOT_GOOD;
    }

    /* Clear flags latched while the input was off; the fault IRQ itself is
     * re-armed by regulator_start() once VS is up.                         */
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C;

    /* Reset regulator state */
    pid_reset_integrator(&pid_state);
    regulator_integrator_reset_requested = false;
    regulator_last_fault_source          = REGULATOR_FAULT_NONE;
    consecutive_backstop_count           = 0u;
    regulator_fault                      = false;

    regulator_state = REGULATOR_STATE_IDLE;
    return REGULATOR_CLEAR_OK;
}

/* =========================================================================
 * regulator_debug_poll
 * =========================================================================*/

static void debug_complete(uint32_t cmd, uint32_t result)
{
    regulator_debug.result   = result;
    regulator_debug.last_cmd = cmd;
    regulator_debug.done_count++;
    regulator_debug.request  = REGULATOR_DEBUG_CMD_NONE;
}

/**
 * regulator_debug_poll_isr — Service the mailbox commands that never block
 * (SET_VOLTAGE, STOP) from the PID ISR.  With OUTPUT_EN high the USBPD CAD
 * task (priority 6) holds the CPU and the default task that runs
 * regulator_debug_poll() never gets it (2026-09-28 evening: a set-voltage
 * request sat unserviced for 10 s while regulating).  PC8 is also the
 * TCPP03 ENABLE pin in the USBPD BSP.
 */
static void regulator_debug_poll_isr(void)
{
    uint32_t cmd = regulator_debug.request;
    if (cmd == REGULATOR_DEBUG_CMD_STOP)
    {
        regulator_stop();
        debug_complete(cmd, 0u);
    }
    else if (cmd == REGULATOR_DEBUG_CMD_SET_VOLTAGE)
    {
        uint32_t mv = regulator_debug.arg;
        uint32_t result = REGULATOR_SET_OUT_OF_RANGE;
        if (mv >= SETPOINT_MIN_MV && mv <= SETPOINT_MAX_MV)
        {
            regulator_set_target_voltage(mv);
            result = REGULATOR_SET_OK;
        }
        debug_complete(cmd, result);
    }
}

void regulator_debug_poll(void)
{
    uint32_t cmd = regulator_debug.request;
    if (cmd == REGULATOR_DEBUG_CMD_NONE)
    {
        return;
    }

    uint32_t result;
    switch (cmd)
    {
        case REGULATOR_DEBUG_CMD_CLEAR_FAULT:
            result = (uint32_t)regulator_clear_fault();
            break;
        case REGULATOR_DEBUG_CMD_STOP:
            regulator_stop();
            result = 0u;
            break;
        case REGULATOR_DEBUG_CMD_BENCH_PWM_ON:
            result = (uint32_t)regulator_bench_pwm(true);
            break;
        case REGULATOR_DEBUG_CMD_BENCH_PWM_OFF:
            result = (uint32_t)regulator_bench_pwm(false);
            break;
        case REGULATOR_DEBUG_CMD_SET_VOLTAGE:
        case REGULATOR_DEBUG_CMD_START:
        {
            uint32_t mv = regulator_debug.arg;
            if (mv < SETPOINT_MIN_MV || mv > SETPOINT_MAX_MV)
            {
                result = REGULATOR_SET_OUT_OF_RANGE;
            }
            else if (cmd == REGULATOR_DEBUG_CMD_SET_VOLTAGE)
            {
                regulator_set_target_voltage(mv);
                result = REGULATOR_SET_OK;
            }
            else if (regulator_state != REGULATOR_STATE_IDLE)
            {
                result = REGULATOR_SET_NOT_IDLE;
            }
            else
            {
                regulator_set_target_voltage(mv);
                regulator_start();
                result = (regulator_state == REGULATOR_STATE_RUNNING)
                             ? REGULATOR_SET_OK : REGULATOR_SET_START_FAILED;
            }
            break;
        }
        default:
            result = REGULATOR_DEBUG_RESULT_UNKNOWN_CMD;
            break;
    }

    if (regulator_debug.request == cmd)   /* not already done by the ISR */
    {
        debug_complete(cmd, result);
    }
}

/* =========================================================================
 * regulator_bench_pwm
 * =========================================================================*/

RegulatorBenchResult regulator_bench_pwm(bool on)
{
    if (!on)
    {
        if (regulator_bench_pwm_active)
        {
            hrtim_fault_irq_disarm();
            hrtim_disable_all_outputs();
            bench_pwm_restore();
        }
        return REGULATOR_BENCH_OK;
    }

    if (regulator_bench_pwm_active)
    {
        return REGULATOR_BENCH_OK;
    }
    if (regulator_state != REGULATOR_STATE_IDLE)
    {
        return REGULATOR_BENCH_NOT_IDLE;
    }
    if (HAL_GPIO_ReadPin(PIN_INPUT_EN_PORT, PIN_INPUT_EN_PIN) == GPIO_PIN_SET)
    {
        return REGULATOR_BENCH_INPUT_PATH_ON;
    }
    if (!vs_good() || !is_good())
    {
        return REGULATOR_BENCH_FAULT_LINE_LOW;
    }

    HRTIM_Timerx_TypeDef *ta = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A];
    HRTIM_Timerx_TypeDef *tb = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B];

    /* Buck-mode leg configuration, then drop the comparator event so the
     * waveform depends only on PER/CMP2/CMP3 and the dead-time.            */
    hrtim_apply_buck_mode_static_leg();
    bench_saved_set_rst[0] = ta->SETx1R;
    bench_saved_set_rst[1] = ta->RSTx1R;
    bench_saved_set_rst[2] = tb->SETx1R;
    bench_saved_set_rst[3] = tb->RSTx1R;
    ta->SETx1R &= ~HRTIM_SET1R_EXTVNT4;
    ta->RSTx1R &= ~HRTIM_RST1R_EXTVNT4;
    tb->SETx1R &= ~HRTIM_SET1R_EXTVNT4;
    tb->RSTx1R &= ~HRTIM_RST1R_EXTVNT4;
    regulator_bench_pwm_active = true;

    hrtim_fault_irq_rearm();
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
                                            HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
    return REGULATOR_BENCH_OK;
}

/**
 * bench_pwm_restore — Put back the set/reset sources saved by bench PWM.
 * Register writes only: safe from the fault ISR.  Outputs must already be off.
 */
static void bench_pwm_restore(void)
{
    if (!regulator_bench_pwm_active)
    {
        return;
    }
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R = bench_saved_set_rst[0];
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R = bench_saved_set_rst[1];
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R = bench_saved_set_rst[2];
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = bench_saved_set_rst[3];
    regulator_bench_pwm_active = false;
}

/* =========================================================================
 * regulator_set_target_voltage
 * =========================================================================*/

void regulator_set_target_voltage(uint32_t voltage_mv)
{
    pd_interface_notify_voltage_contract(voltage_mv);
}

/* =========================================================================
 * regulator_get_state / regulator_get_mode
 * =========================================================================*/

RegulatorState regulator_get_state(void)
{
    return regulator_state;
}

RegulatorMode regulator_get_mode(void)
{
    return regulator_mode;
}

/* =========================================================================
 * ISR bodies
 * =========================================================================*/

/**
 * regulator_hrtim_tima_period_isr — HRTIM Timer A period (REP) ISR.
 *
 * Fires at every switching period reset (200 kHz at default settings).
 * Reloads DAC3 CH1 with the PID peak value (§7.6).
 * Counts CMP2 backstop events (§10.4).
 *
 * Priority: NVIC_PRIORITY_HRTIM (1) — below TIM6 (0), above TIM7 (2).
 * No FreeRTOS API calls allowed.
 */
void regulator_hrtim_tima_period_isr(void)
{
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        return;
    }
    const uint32_t cyc0 = DWT->CYCCNT;

    /* Check if this period reset was caused by the Compare 2 backstop.
     * We distinguish by checking the HRTIM Timer A interrupt status register:
     *   - REP flag (bit 0) = period/repetition interrupt
     *   - CMP2 flag (bit 5) = compare 2 match interrupt
     *
     * TODO(hardware): Wire CMP2 interrupt properly.  For now, backstop
     * counting is a TODO pending hardware verification that CMP2 fires
     * correctly.  See §10.3.
     */

    /* Reload DAC3 CH1 with current PID peak value.
     * Must happen before the blanking window expires (§7.6).              */
    slope_comp_reload_dac_peak();

    /* Glitch-free V_out for this period; see regulator_vout_est_raw.  The
     * estimate follows the sample, but rises only by the credit the pulses
     * issued so far have earned, and falls by a bounded amount.           */
    uint16_t raw      = adc1_dma_buffer[ADC1_DMA_INDEX_VD_MON];
    uint16_t vout_est = regulator_vout_est_raw;
    if (raw > vout_est)
    {
        uint16_t rise     = (uint16_t)(raw - vout_est);
        uint16_t rise_max = vout_rise_budget_raw;
        if (rise > rise_max) { rise = rise_max; }
        vout_est             = (uint16_t)(vout_est + rise);
        vout_rise_budget_raw = (uint8_t)(vout_rise_budget_raw - rise);
        if (raw > vout_est)
        {
            if (vout_above_periods == 0u)
            {
                vout_above_min_raw = raw;
                vout_above_max_raw = raw;
            }
            if (raw < vout_above_min_raw) { vout_above_min_raw = raw; }
            if (raw > vout_above_max_raw) { vout_above_max_raw = raw; }
            vout_above_periods++;
            if ((uint16_t)(vout_above_max_raw - vout_above_min_raw) > VOUT_RISE_PERSIST_SPREAD_RAW)
            {
                /* not one level: a glitch run; count again from here */
                vout_above_min_raw = raw;
                vout_above_max_raw = raw;
                vout_above_periods = 1u;
            }
            if (vout_above_periods >= VOUT_RISE_PERSIST_PERIODS)
            {
                /* sustained: the rise is real, as far as its lowest sample */
                if (vout_above_min_raw > vout_est) { vout_est = vout_above_min_raw; }
                vout_above_periods = 0u;
            }
        }
        else
        {
            vout_above_periods = 0u;
        }
    }
    else
    {
        vout_above_periods = 0u;
        uint16_t fall = (uint16_t)(vout_est - raw);
        vout_est = (uint16_t)(vout_est - ((fall < VOUT_FALL_RAW) ? fall : (uint16_t)VOUT_FALL_RAW));
    }
    regulator_vout_est_raw = vout_est;

#if !SYNC_RECT_ENABLED
    /* Pulse skipping, decided every switching period (2026-09-27, run 7).
     * Deciding it once per PID period let up to ten charge pulses through
     * before the next look at V_out; at no load each is about 30 mV on the
     * output, so the ripple was 0.4 V and the relative OVP tripped at 5.5 V.
     * ADC1 scans VD_MON continuously into adc1_dma_buffer; comparing the raw
     * count against a threshold the PID ISR keeps in raw counts costs a few
     * cycles here.  OENR and ODISR are write-1 registers, so a plain store
     * touches only the outputs named: the pulse output, TA1 (Q1) in buck
     * and TB2 (Q4) in boost, and the other leg's bootstrap refresh output,
     * TB2 (Q4) in buck and TA2 (Q2) in boost (2026-09-28, runs 31 and 32).
     * A skipped period switches nothing: the refresh pulse alone rings L1
     * against the node capacitances and pumps the output through the
     * off leg's reverse conduction (run 31, 29 to 32 V at no load with
     * every pulse skipped).  The static switch's gate holds on its
     * bootstrap capacitor meanwhile; the next enabled period refreshes it
     * before its pulse starts (CMP3 before CMP4).  A pulse that has
     * already started this period is cut short by the disable, which is
     * the right direction.                                                */
    {
        /* Buck-boost skips both pulse outputs, TA1 and TB2, and keeps the
         * Q2 refresh (TA2) that Q1's bootstrap needs, as in buck.         */
        const uint32_t skip_outputs = (regulator_mode == REGULATOR_MODE_BOOST)
                                          ? (HRTIM_OUTPUT_TB2 | HRTIM_OUTPUT_TA2)
                                          : (HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TB2);
        if (vout_est > regulator_skip_raw_threshold)
        {
            HRTIM1->sCommonRegs.ODISR = skip_outputs;
            regulator_pulses_skipped  = 1u;
        }
        else if ((uint32_t)vout_est + SKIP_HYSTERESIS_RAW < regulator_skip_raw_threshold)
        {
            HRTIM1->sCommonRegs.OENR  = skip_outputs;
            regulator_pulses_skipped  = 0u;
        }
        /* else: inside the band, keep the previous decision. */

        if (regulator_pulses_skipped == 0u)
        {
            /* One pulse this period: credit the rise it can produce. */
            uint16_t budget = (uint16_t)vout_rise_budget_raw + VOUT_RISE_PER_PULSE_RAW;
            vout_rise_budget_raw = (budget > VOUT_RISE_BUDGET_MAX_RAW)
                                       ? (uint8_t)VOUT_RISE_BUDGET_MAX_RAW : (uint8_t)budget;
        }

        regulator_period_trace[regulator_period_trace_index] =
            (uint16_t)(raw & 0x0FFFu) |
            (uint16_t)(regulator_pulses_skipped ? 0x8000u : 0u);
        regulator_period_trace_index =
            (uint16_t)((regulator_period_trace_index + 1u) % PERIOD_TRACE_LENGTH);
    }
#else
    vout_rise_budget_raw = VOUT_RISE_BUDGET_MAX_RAW;
#endif
    cyc_record(regulator_cyc_tima, DWT->CYCCNT - cyc0);

    /* TODO(debug): Confirm DAC reload timing with oscilloscope: probe DAC3
     * output (PA5 / DAC3_OUT1) and TA1 switching node; the DAC must settle
     * to the new peak value before CMP1 unmasks EEV4 (~100 ns in buck,
     * ~500 ns in boost from period reset). (§7.6) */

    /* Clear the HRTIM Timer A repetition interrupt flag */
    /* This is handled by the HAL callback mechanism or direct register clear:
     * HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].TIMxICR = HRTIM_TIMISR_REP;
     * The HAL IRQ handler clears it; we just need to implement the callback. */
}

/**
 * regulator_hrtim_fault_isr — HRTIM fault ISR body.
 *
 * Called from HRTIM1_FLT_IRQHandler when FLT1 or FLT2 asserts.
 * The HRTIM hardware has already forced all outputs to their fault state
 * (INACTIVE = LOW → all FETs off).
 *
 * Priority: NVIC_PRIORITY_HRTIM (1).
 * No FreeRTOS API calls allowed.
 */
void regulator_hrtim_fault_isr(void)
{
    /* Determine fault source from HRTIM interrupt status register */
    uint32_t hrtim_isr = HRTIM1->sCommonRegs.ISR;

    RegulatorFaultSource source = REGULATOR_FAULT_NONE;
    if (hrtim_isr & HRTIM_ISR_FLT1)
    {
        source |= REGULATOR_FAULT_HW_FLT1;
    }
    if (hrtim_isr & HRTIM_ISR_FLT2)
    {
        source |= REGULATOR_FAULT_HW_FLT2;
    }

    /* enter_fault() disarms this IRQ: the fault inputs are level-sensitive,
     * so while a line stays asserted clearing the flag would re-fire this
     * priority-1 ISR forever, starving every lower-priority interrupt.
     * Protection is unaffected: the HRTIM hardware keeps forcing the outputs
     * to their fault state independently of this interrupt.                 */
    enter_fault(source);
}

/**
 * regulator_pid_tim7_isr — PID computation ISR body.
 *
 * Fires at 20 kHz.  Sequence (§8, §10.2, §15.1):
 *   1. Read ADC measurements (V_out, V_in, I_L, etc.)
 *   2. Update PID coefficients from runtime-mutable variables.
 *   3. Check for integrator reset request (large setpoint change).
 *   4. Run soft-start setpoint ramp if active (§11.2).
 *   5. Run PID computation → new DAC peak value.
 *   6. Update slope compensation step (§7.2).
 *   7. Update telemetry (§15.1).
 *   8. Software safety checks (§10.2); enter fault if violated.
 *   9. Update regulator_ready flag.
 *
 * Priority: NVIC_PRIORITY_PID_TIM7 (2).
 * No FreeRTOS API calls allowed.
 */
void regulator_pid_tim7_isr(void)
{
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        return;
    }

    const uint32_t cyc0 = DWT->CYCCNT;
    regulator_debug_poll_isr();
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        return;   /* a STOP from the mailbox */
    }

    /* --- 1. Trigger and read ADC conversions --- */
    adc_monitor_trigger_vin();
    adc_monitor_scale_adc1_buffer();   /* latest free-running ADC1 scan */

    /* V_out from the per-period slew-limited estimate (see
     * regulator_vout_est_raw), not the latest single sample: the PID, the
     * soft-start, the software OVP and UVP and the debug log all see the
     * glitch-free value.  The raw sample stays in adc_measurements.v_out_mv. */
    uint32_t v_out_mv  = ((uint32_t)regulator_vout_est_raw
                          * VD_MON_FULL_SCALE_MV) / ADC_FULL_SCALE_COUNTS;
    uint32_t i_l_ma    = adc_measurements.i_inductor_ma;
    (void)i_l_ma;   /* telemetry only */

    /* Read V_in result from the ADC2 conversion triggered at the start;
     * at 20 kHz period (50 µs), ADC2 conversion (~354 ns) is complete.    */
    const uint32_t cyc_poll = DWT->CYCCNT;
    adc_monitor_read_vin_result();
    cyc_record(regulator_cyc_vin_poll, DWT->CYCCNT - cyc_poll);
    uint32_t v_in_mv = adc_measurements.v_in_mv;

    /* TODO(debug): If v_out_mv or v_in_mv reads zero, check ADC1/ADC2 DMA
     * init in stm32g4xx_hal_msp.c and confirm INPUT_EN is asserted before
     * regulator_start() powers the voltage-divider network (§5.1). */

    /* --- 2. Refresh PID config from runtime-mutable variables ---
     * This allows Kp/Ki/Kd to be tuned at runtime without restart (§8.7). */
    pid_config.kp = pid_kp;
    pid_config.ki = pid_ki;
    pid_config.kd = pid_kd;

    /* V_in low-pass for mode selection (VIN_FILTER_SHIFT). */
    {
        int32_t d = (int32_t)v_in_mv - (int32_t)regulator_vin_filt_mv;
        regulator_vin_filt_mv = (uint32_t)((int32_t)regulator_vin_filt_mv +
                                           (d / (1 << VIN_FILTER_SHIFT)));
    }

    /* --- 3. Integrator reset request (§8.8) ---
     * The setpoint is slewed (step 4a), so a new target never steps the
     * loop, and resetting the integrator mid-regulation would drop the
     * output; the request is only acknowledged.  A mode change still
     * resets it (change_mode_while_running).                              */
    regulator_integrator_reset_requested = false;

    /* --- 4a. Setpoint slew (SETPOINT_SLEW_MV_PER_CYCLE) --- */
    {
        uint32_t tgt = target_voltage_mv;
        uint32_t cmd = regulator_commanded_mv;
        if (cmd + SETPOINT_SLEW_MV_PER_CYCLE < tgt)      { cmd += SETPOINT_SLEW_MV_PER_CYCLE; }
        else if (cmd > tgt + SETPOINT_SLEW_MV_PER_CYCLE) { cmd -= SETPOINT_SLEW_MV_PER_CYCLE; }
        else                                             { cmd = tgt; }
        regulator_commanded_mv = cmd;
    }
    const uint32_t commanded_mv = regulator_commanded_mv;

    /* --- 3b. Mode selection (§5.1), every cycle on the slewed setpoint and
     * filtered V_in.  While precharging for boost step 4b owns the
     * handover; the precharge is dropped if the target leaves boost.     */
    if (boost_precharge)
    {
        if (determine_mode_from_voltages(regulator_vin_filt_mv, commanded_mv) !=
            REGULATOR_MODE_BOOST)
        {
            boost_precharge = false;
        }
    }
    else
    {
        RegulatorMode new_mode = determine_mode_from_voltages(regulator_vin_filt_mv,
                                                              commanded_mv);
        if (new_mode != regulator_mode)
        {
            if (new_mode == REGULATOR_MODE_BOOST && regulator_mode == REGULATOR_MODE_BUCK &&
                v_out_mv + BOOST_PRECHARGE_BELOW_VIN_MV < v_in_mv)
            {
                /* Stay in buck until the output is precharged (see
                 * boost_precharge); step 4b hands over.                      */
                boost_precharge = true;
            }
            else
            {
                change_mode_while_running(new_mode, v_out_mv, commanded_mv);
            }
        }
    }

    /* --- 4. Soft-start setpoint ramp (§11.2) --- */
    uint32_t effective_setpoint_mv;
    if (softstart_active)
    {
        softstart_setpoint_mv += softstart_increment_mv;
        if (softstart_setpoint_mv >= commanded_mv)
        {
            softstart_setpoint_mv = commanded_mv;
            softstart_active      = false;
        }
        effective_setpoint_mv = softstart_setpoint_mv;
    }
    else
    {
        effective_setpoint_mv = commanded_mv;
    }

    /* --- 4b. Boost precharge (see boost_precharge) ---
     * The buck ramp is capped under V_in; once it is at the cap and V_out
     * has followed, switch to boost, which restarts the ramp from V_out.  */
    if (boost_precharge)
    {
        uint32_t cap_mv = (v_in_mv > BOOST_PRECHARGE_BELOW_VIN_MV)
                              ? (v_in_mv - BOOST_PRECHARGE_BELOW_VIN_MV) : 0u;
        if (effective_setpoint_mv >= cap_mv)
        {
            effective_setpoint_mv = cap_mv;
            if (v_out_mv + BOOST_PRECHARGE_DONE_MARGIN_MV >= cap_mv)
            {
                boost_precharge = false;
                change_mode_while_running(REGULATOR_MODE_BOOST, v_out_mv,
                                          commanded_mv);
                effective_setpoint_mv = softstart_setpoint_mv;
            }
        }
    }

    /* --- 4c. Connect the output (see OUTPUT_CONNECT_MARGIN_MV) ---
     * Only in the final mode, after soft-start, with V_out near target.  */
#if OUTPUT_SWITCH_ENABLED
    if (!output_switch_on && !boost_precharge && !softstart_active &&
        v_out_mv + OUTPUT_CONNECT_MARGIN_MV >= commanded_mv)
    {
        HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_SET);
        output_switch_on = true;
    }
#endif

    /* --- 5. PID computation ---
     * Floor the peak-current command at half the inductor ripple minus
     * PEAK_FLOOR_NEG_MARGIN_MA, so the average inductor current the loop can
     * ask for is bounded below (regulator_config.h, peak_current_floor_counts).
     * The floor is also the integrator's lower bound inside pid_update.     */
    uint32_t comp_counts  = trip_delay_comp_counts(v_in_mv, v_out_mv, regulator_mode);
#if SYNC_RECT_ENABLED
    uint32_t floor_counts = peak_current_floor_counts(v_in_mv, v_out_mv, regulator_mode);
#else
    /* Discontinuous conduction: the average inductor current cannot go
     * negative, so the ripple/2 floor does not apply and the PID may ask
     * for the minimum pulse.                                              */
    uint32_t floor_counts = 0u;
#endif
    /* The trip-delay compensation below subtracts comp_counts from the PID
     * output, so any PID output below comp_counts is a DAC of 0: a dead zone
     * the integrator had to wind through for 1.1 ms in run 3.  Bounding the
     * PID output at comp_counts removes it; the floor still applies above.  */
    if (floor_counts < comp_counts) { floor_counts = comp_counts; }
    pid_config.output_min = (float)floor_counts;
    pid_config.output_max = (float)((regulator_mode == REGULATOR_MODE_BUCK_BOOST)
                                        ? PEAK_CMD_MAX_COUNTS_BB : PEAK_CMD_MAX_COUNTS);
    regulator_peak_floor_counts = (uint16_t)pid_config.output_min;

    float setpoint_f    = (float)effective_setpoint_mv;
    float measurement_f = (float)v_out_mv;
    float pid_out_f     = pid_update(&pid_state, &pid_config,
                                     setpoint_f, measurement_f);

    /* --- 5b. Trip-delay compensation ---
     * The PID output is the peak inductor current that should happen.  The
     * hardware reaches the DAC threshold and then keeps rising for
     * PEAK_TRIP_DELAY_NS, so the threshold handed to the DAC is lowered by
     * that overshoot (regulator_config.h, trip_delay_comp_counts).  Run 2 on
     * the bench (2026-09-27) showed about 1 A more average current than the
     * command allowed, which is this term at 24 V in and 4.7 uH.           */
    uint32_t peak_cmd_counts = (uint32_t)pid_out_f;
    regulator_trip_delay_comp_counts = (uint16_t)comp_counts;
    uint32_t dac_counts = (peak_cmd_counts > comp_counts) ? (peak_cmd_counts - comp_counts) : 0u;
    if (dac_counts > (uint32_t)PID_OUTPUT_MAX)
    {
        dac_counts = (uint32_t)PID_OUTPUT_MAX;
    }

    /* --- 6. Update slope compensation peak and step --- */
    slope_comp_set_peak(dac_counts);
    slope_comp_update_step(v_out_mv, v_in_mv, (SlopeCompMode)regulator_mode);

    /* --- 6b. Predicted on-time (buck, SYNC_RECT_ENABLED 0) ---
     * The peak-current comparator ends a pulse only when the DAC threshold
     * is below about 150 mV (DCM_MIN_ON_TIME_NS in regulator_config.h has
     * the evidence), so the pulse length is also set here from the PID's
     * peak-current command through the inductor law,
     * t_on = I_pk * L / (V_in - V_out), written to Timer A CMP2 (the TA1
     * reset).  The comparator still resets TA1 earlier when it does trip
     * and DCM_MAX_ON_TIME_NS still bounds CMP2, so this only ever shortens
     * a pulse.  Charge per pulse goes as I_pk squared, so the PID output
     * keeps its meaning and the plant gain grows with the operating point
     * (see pid_kp).  mA * nH / mV = ns.                                   */
#if !SYNC_RECT_ENABLED
    {
        uint32_t peak_ma = (peak_cmd_counts * 1000u) / DAC_COUNTS_PER_AMP;
        const uint32_t peak_max_ma = (regulator_mode == REGULATOR_MODE_BUCK_BOOST)
                                         ? DCM_MAX_PEAK_MA_BB : DCM_MAX_PEAK_MA;
        if (peak_ma > peak_max_ma) { peak_ma = peak_max_ma; }
        /* Inductor charging and discharging voltages per mode (both legs
         * DCM, see HRTIM_BOOST_SWITCHING_OUTPUTS; Vd a body-diode drop):
         *   buck        charge V_in - V_out, discharge V_out + Vd (Q2)
         *   boost       charge V_in,         discharge V_out - V_in + Vd (Q3)
         *   buck-boost  charge V_in,         discharge V_out + 2 Vd (Q2, Q3)
         * t_on = I_pk * L / charge, and t_on + t_off must fit in the window
         * after the pulse start for the current to reach zero:
         * t_on <= window * discharge / (charge + discharge).                 */
        uint32_t chg_mv, dis_mv;
        if (regulator_mode == REGULATOR_MODE_BUCK)
        {
            chg_mv = (v_in_mv > v_out_mv + 1000u) ? (v_in_mv - v_out_mv) : 1000u;
            dis_mv = v_out_mv + DCM_DIODE_DROP_MV;
        }
        else if (regulator_mode == REGULATOR_MODE_BOOST)
        {
            chg_mv = v_in_mv;
            dis_mv = ((v_out_mv > v_in_mv) ? (v_out_mv - v_in_mv) : 0u) + DCM_DIODE_DROP_MV;
        }
        else
        {
            chg_mv = v_in_mv;
            dis_mv = v_out_mv + 2u * DCM_DIODE_DROP_MV;
        }
        if (chg_mv < 1000u) { chg_mv = 1000u; }
        if (dis_mv < 500u)  { dis_mv = 500u; }
        uint32_t t_on_ns = (peak_ma * INDUCTOR_VALUE_NH) / chg_mv;
        const uint32_t window_ns = (uint32_t)(1000000000ull / HRTIM_SWITCHING_FREQ_HZ)
                                   - (BOOTSTRAP_REFRESH_NS + DCM_REFRESH_DEADTIME_NS)
                                   - DCM_WINDOW_MARGIN_NS;
        uint32_t t_dcm_ns = (uint32_t)(((uint64_t)window_ns * dis_mv) / (chg_mv + dis_mv));
        if (t_on_ns > t_dcm_ns)            { t_on_ns = t_dcm_ns; }
        if (t_on_ns > DCM_ON_TIME_CEIL_NS) { t_on_ns = DCM_ON_TIME_CEIL_NS; }
        if (t_on_ns < DCM_MIN_ON_TIME_NS)  { t_on_ns = DCM_MIN_ON_TIME_NS; }
        regulator_on_time_ns = (uint16_t)t_on_ns;
        const uint32_t cmp2 = DCM_PULSE_START_TICKS + HRTIM_NS_TO_TICKS(t_on_ns);
        /* Q1 (buck, buck-boost) on from CMP4 to Timer A CMP2; Q4 (boost,
         * buck-boost) on from CMP4 to Timer B CMP2, TB1's set, through the
         * dead-time generator.                                            */
        if (regulator_mode != REGULATOR_MODE_BOOST)
        {
            HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR = cmp2;
        }
        if (regulator_mode != REGULATOR_MODE_BUCK)
        {
            HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR = cmp2;
        }
    }
#endif

    /* TODO(debug): Verify slope_comp_step_counts in debugger watch.
     * Expect ~1–5 DAC counts/tick at 2 MHz with default inductor value.
     * If zero, check INDUCTOR_VALUE_UH / INDUCTOR_VALUE_UH_TENTHS in
     * regulator_config.h (§7.2, §16). */

    /* --- 7. Update telemetry --- */
    regulator_pid_output_dac_counts = (uint16_t)dac_counts;
    regulator_pid_error_mv          = (int32_t)effective_setpoint_mv - (int32_t)v_out_mv;

    /* --- 7b. Pulse-skip threshold (buck, no or light load) ---
     * The smallest charge pulse the modulator can make still moves V_out at
     * no load, so once V_out is above the setpoint the only way down is to
     * make no pulse at all.  The Timer A period ISR skips pulses whenever
     * the V_out estimate is above this threshold, PULSE_SKIP_ABOVE_MV over
     * the setpoint so that the PID, not the skip band, holds the setpoint
     * (regulator_config.h).  Refreshed here so the soft-start ramp is
     * respected.  Raw counts = mV * 4096 / the calibrated VD_MON full
     * scale.                                                               */
    regulator_skip_raw_threshold =
        (uint16_t)(((uint64_t)(effective_setpoint_mv + PULSE_SKIP_ABOVE_MV)
                    * ADC_FULL_SCALE_COUNTS) / VD_MON_FULL_SCALE_MV);

    /* --- 8. Software safety checks (§10.2) ---
     * Checked against the slewed target (step 4a), not the soft-start ramp: on the
     * first PID cycle the ramp is one increment (200 mV at 20 V), so a
     * residual V_out of a few hundred mV tripped the relative OVP before
     * any switching happened (bench, 2026-09-27: SW_OVP with V_out 410 mV).
     * UVP is already suppressed while softstart_active.                    */
    if (!software_safety_checks_pass(v_out_mv, v_in_mv,
                                      commanded_mv, regulator_mode))
    {
        /* Fault entry handled inside software_safety_checks_pass */
        return;
    }

    /* --- 9. Update regulator_ready flag (§9.1) ---
     * Regulation window: ±2% of setpoint (§9.3).                          */
    if (effective_setpoint_mv > 0u)
    {
        uint32_t tolerance_mv = effective_setpoint_mv / 50u;  /* 2% */
        int32_t error = regulator_pid_error_mv;
        regulator_ready = (error >= -(int32_t)tolerance_mv &&
                           error <=  (int32_t)tolerance_mv);
    }
    else
    {
        regulator_ready = false;
    }

    /* --- 10. High-speed debug sample capture (§15.3) ---
     * Record one sample per PID cycle (20 kHz) into the circular debug buffer.
     * All fields are derived from values already computed in this ISR so there
     * is no extra ADC conversion overhead.  The buffer is readable in a live
     * debugger memory/watch window without halting the CPU.                  */
    {
        DebugSample s;
        s.v_out_mv      = v_out_mv;
        s.v_in_mv       = v_in_mv;
        s.i_inductor_ma = (uint32_t)adc_measurements.i_inductor_ma;
        s.i_out_ma      = (uint32_t)adc_measurements.i_out_ma;
        s.dac_counts    = (uint16_t)dac_counts;
        s.error_mv      = regulator_pid_error_mv;
        s.state         = (uint8_t)regulator_state;
        s.mode          = (uint8_t)regulator_mode;
        debug_log_record(&s);
    }
    cyc_record(regulator_cyc_pid, DWT->CYCCNT - cyc0);
}

/* =========================================================================
 * Internal helper implementations
 * =========================================================================*/

/**
 * hrtim_configure_fault_levels_and_enable — Configure HRTIM fault response.
 *
 * CubeMX sets FaultLevel = NONE for all outputs and FaultEnable = NONE for
 * all timers.  We must correct both to ensure the HRTIM forces all FETs off
 * when FLT1 or FLT2 asserts (§10.1, §5.5).
 *
 * Approach:
 *   a) Reconfigure all four outputs via HAL with FaultLevel = INACTIVE.
 *   b) Enable FLT1 and FLT2 on Timer A and Timer B via direct register bits.
 */
static void hrtim_configure_fault_levels_and_enable(void)
{
    HRTIM_OutputCfgTypeDef output_cfg;
    memset(&output_cfg, 0, sizeof(output_cfg));

    output_cfg.Polarity              = HRTIM_OUTPUTPOLARITY_HIGH;
    output_cfg.IdleMode              = HRTIM_OUTPUTIDLEMODE_NONE;
    output_cfg.IdleLevel             = HRTIM_OUTPUTIDLELEVEL_INACTIVE;
    output_cfg.FaultLevel            = HRTIM_OUTPUTFAULTLEVEL_INACTIVE;
    output_cfg.ChopperModeEnable     = HRTIM_OUTPUTCHOPPERMODE_DISABLED;
    output_cfg.BurstModeEntryDelayed = HRTIM_OUTPUTBURSTMODEENTRY_REGULAR;

    /* Timer A Output 1 (CHA1): buck switching high-side
     * Set = Period reset, Reset = EEV4 | CMP2 (CMP2=backstop, matching IOC) */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_TIMPER;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_EEV_4 | HRTIM_OUTPUTRESET_TIMCMP2;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_OUTPUT_TA1, &output_cfg);

    /* Timer A Output 2 (CHA2): complementary low-side, no own Set/Reset */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_NONE;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_NONE;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                    HRTIM_OUTPUT_TA2, &output_cfg);

    /* Timer B Output 1 (CHB1): boost switching high-side
     * Set = EEV4 | CMP2 (CMP2=backstop, matching IOC), Reset = Period      */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_EEV_4 | HRTIM_OUTPUTSET_TIMCMP2;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_TIMPER;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                    HRTIM_OUTPUT_TB1, &output_cfg);

    /* Timer B Output 2 (CHB2): complementary low-side */
    output_cfg.SetSource   = HRTIM_OUTPUTSET_NONE;
    output_cfg.ResetSource = HRTIM_OUTPUTRESET_NONE;
    HAL_HRTIM_WaveformOutputConfig(&hhrtim1, HRTIM_TIMERINDEX_TIMER_B,
                                    HRTIM_OUTPUT_TB2, &output_cfg);

    /* Enable FLT1 and FLT2 response on Timer A and Timer B.
     * RM0440 §27.5.2: TIMxCR bits 20 (FLT1EN) and 21 (FLT2EN).           */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].TIMxCR |=
        (HRTIM_TIMCR_FLT1EN_BIT | HRTIM_TIMCR_FLT2EN_BIT);
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].TIMxCR |=
        (HRTIM_TIMCR_FLT1EN_BIT | HRTIM_TIMCR_FLT2EN_BIT);
}

/**
 * hrtim_configure_compare_registers — Set CMP1/CMP2/CMP3 for all timers (§10.3).
 *
 * CMP1: blanking window end — EEV4 (COMP1 output) is masked from the period
 *       reset until CMP1 fires, per HRTIM_TIMEEVFLT_BLANKINGCMP1 configured
 *       in the IOC.  Timer A (buck active) uses HRTIM_BLANKING_TICKS_BUCK;
 *       Timer B (boost active) uses HRTIM_BLANKING_TICKS_BOOST.
 *
 * CMP2: hardware backstop — ends the charge phase if COMP1 has not fired by
 *       MAX_ON_TIME_COUNTS.  Added as Reset source for TA1 (buck) and Set
 *       source for TB1 (boost) in hrtim_configure_fault_levels_and_enable().
 *
 * CMP3: bootstrap refresh end — the static leg output returns HIGH after the
 *       BOOTSTRAP_REFRESH_TICKS LOW pulse at each period reset.  Set as the
 *       SET source for the static leg output in hrtim_apply_*_mode_static_leg().
 */
static void hrtim_configure_compare_registers(void)
{
    /* CMP1: blanking window end per timer's active switching role.
     * Timer A is the active leg in BUCK; Timer B is the active leg in BOOST.
     * Both CMP1xR values are written so the blanking is correct regardless
     * of which timer is active at any given time.                           */
    /* Period from regulator_config.h (HRTIM_SWITCHING_FREQ_HZ).  The IOC/MX
     * init leaves the CubeMX default 0xFFDF (83 kHz at MUL32), which also made
     * the CMP2 backstop (computed for this period) a 35 % limit instead of
     * MAX_DUTY_CYCLE_PCT.  Timers are not started yet, so the active
     * register takes the value directly.                                    */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].PERxR = HRTIM_PERIOD_COUNTS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].PERxR = HRTIM_PERIOD_COUNTS;

    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP1xR = HRTIM_BLANKING_TICKS_BUCK;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP1xR = HRTIM_BLANKING_TICKS_BOOST;

    /* CMP2: hardware backstop — MAX_ON_TIME_COUNTS on both timers.
     * The backstop fires as Reset (buck, TA1) or Set (boost, TB1) per the
     * sources set in hrtim_configure_fault_levels_and_enable().             */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR = MAX_ON_TIME_COUNTS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR = MAX_ON_TIME_COUNTS;

    /* CMP3: bootstrap refresh end — BOOTSTRAP_REFRESH_TICKS on both timers.
     * The mode functions configure SETx1R = TIMCMP3 for the static leg so
     * the output rises after the brief bootstrap LOW pulse at period reset.  */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP3xR = BOOTSTRAP_REFRESH_TICKS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP3xR = BOOTSTRAP_REFRESH_TICKS;

#if !SYNC_RECT_ENABLED
    /* Timer A without synchronous rectification (regulator_config.h,
     * SYNC_RECT_ENABLED): the two outputs are programmed separately, so
     * dead-time insertion, which makes TA2 the complement of TA1 (forced
     * continuous conduction), is switched off for this timer before its
     * counter starts.  TA2 (Q2) is high from the period reset to CMP3: the
     * bootstrap refresh pulse, the same 200 ns Timer B gives Q4.  TA1 (Q1)
     * is set at CMP4, DCM_REFRESH_DEADTIME_NS after Q2 turns off; the
     * blanking end (CMP1) and the backstop (CMP2) move with it.  The mode
     * functions below set TA1's sources.                                   */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].OUTxR &= ~HRTIM_OUTR_DTEN;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP4xR = DCM_PULSE_START_TICKS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP1xR =
        DCM_PULSE_START_TICKS + HRTIM_BLANKING_TICKS_BUCK;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR =
        DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx2R = HRTIM_OUTPUTSET_TIMPER;
    /* Timer B, the boost pulse leg, gets the same window: Q4 (TB2, the
     * dead-time complement of TB1) is on from CMP4 to CMP2, after the Q2
     * refresh on the input leg has ended and Q1 is on again.  In run 31
     * (2026-09-28) the boost pulse started at the period reset, inside the
     * Q2 refresh, so both ends of L1 sat at 0 V for the first 250 ns of
     * every pulse and a 200 ns command charged nothing.                  */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP4xR = DCM_PULSE_START_TICKS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP1xR =
        DCM_PULSE_START_TICKS + HRTIM_BLANKING_TICKS_BOOST;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR =
        DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx2R = HRTIM_OUTPUTRESET_TIMCMP3;
#endif
}

/**
 * hrtim_enable_period_and_fault_interrupts — Enable HRTIM interrupts.
 *
 * Timer A repetition interrupt → DAC Y-intercept reload at each period (§7.6).
 * Fault interrupt → hardware fault handling (§10.1).
 *
 * Both at NVIC priority NVIC_PRIORITY_HRTIM (1).
 */
static void hrtim_enable_period_and_fault_interrupts(void)
{
    /* Enable Timer A repetition (period) interrupt */
    __HAL_HRTIM_TIMER_ENABLE_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                  HRTIM_TIM_IT_REP);
    HAL_NVIC_SetPriority(HRTIM1_TIMA_IRQn, NVIC_PRIORITY_HRTIM, 0u);
    HAL_NVIC_EnableIRQ(HRTIM1_TIMA_IRQn);

    /* Route the HRTIM fault interrupt.  FLT1/FLT2 sources stay disarmed
     * here: VS_GOOD is low whenever INPUT_EN is off, so they are armed by
     * regulator_start() only once VS is up (hrtim_fault_irq_rearm).         */
    HAL_NVIC_SetPriority(HRTIM1_FLT_IRQn, NVIC_PRIORITY_HRTIM, 0u);
    HAL_NVIC_EnableIRQ(HRTIM1_FLT_IRQn);
}

/**
 * hrtim_fault_irq_disarm — Mask FLT1/FLT2 interrupts and drop any pending one.
 *
 * Called from the fault ISR.  Leaves the fault flags set in HRTIM ISR so the
 * cause stays visible to a debugger until the fault is cleared.
 */
static void hrtim_fault_irq_disarm(void)
{
    __HAL_HRTIM_DISABLE_IT(&hhrtim1, HRTIM_IT_FLT1);
    __HAL_HRTIM_DISABLE_IT(&hhrtim1, HRTIM_IT_FLT2);
    NVIC_ClearPendingIRQ(HRTIM1_FLT_IRQn);
}

/**
 * hrtim_fault_irq_rearm — Clear fault flags, then unmask FLT1/FLT2 interrupts.
 *
 * Flags are cleared first so a stale flag cannot fire immediately.  If a line
 * re-asserts afterwards the ISR runs once more and disarms again.
 */
static void hrtim_fault_irq_rearm(void)
{
    HRTIM1->sCommonRegs.ICR = HRTIM_ICR_FLT1C | HRTIM_ICR_FLT2C;
    NVIC_ClearPendingIRQ(HRTIM1_FLT_IRQn);
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, HRTIM_IT_FLT1);
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, HRTIM_IT_FLT2);
}

/**
 * vs_good / is_good — Read the ADM1270 PWRGD / ~FAULT lines (high = good).
 */
static bool vs_good(void)
{
    return HAL_GPIO_ReadPin(PIN_VS_GOOD_PORT, PIN_VS_GOOD_PIN) == GPIO_PIN_SET;
}

static bool is_good(void)
{
    return HAL_GPIO_ReadPin(PIN_IS_GOOD_PORT, PIN_IS_GOOD_PIN) == GPIO_PIN_SET;
}

/**
 * input_path_power_up — Assert INPUT_EN and wait for the ADM1270 to bring
 * VS up.  Task context only (polls with HAL_Delay).
 *
 * @param failure  Set to REGULATOR_FAULT_HW_FLT2 if the ADM1270 tripped on
 *                 over-current, else REGULATOR_FAULT_HW_FLT1 if VS_GOOD
 *                 never rose.  Untouched on success.
 * @return true when VS_GOOD and IS_GOOD are both high.  INPUT_EN is left
 *         asserted either way; the caller decides what to do with it.
 */
static bool input_path_power_up(RegulatorFaultSource *failure)
{
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT, PIN_INPUT_EN_PIN, GPIO_PIN_SET);

    uint32_t t0 = HAL_GetTick();
    while (!vs_good() && is_good() && (HAL_GetTick() - t0) < INPUT_PGOOD_TIMEOUT_MS)
    {
        HAL_Delay(1u);
    }

    if (!is_good())
    {
        *failure = REGULATOR_FAULT_HW_FLT2;
        return false;
    }
    if (!vs_good())
    {
        *failure = REGULATOR_FAULT_HW_FLT1;
        return false;
    }
    return true;
}

/**
 * hrtim_start_timers — Start HRTIM counters (outputs remain disabled).
 *
 * Both Timer A and Timer B must be running for their compare and period
 * events to fire.  Outputs are enabled separately in regulator_start().
 */
static void hrtim_start_timers(void)
{
    /* Start Timer A and Timer B counters.
     * HAL_HRTIM_WaveformCounterStart_IT enables the timer with interrupts. */
    HAL_HRTIM_WaveformCounterStart_IT(&hhrtim1,
                                       HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B);
}

/**
 * hrtim_apply_buck_mode_static_leg — Configure Timer B for bootstrap-refreshed
 * static HIGH operation (buck mode output-side leg).
 *
 * In buck mode Timer B (output side) must hold CHB1 HIGH, with a brief LOW
 * pulse each period to recharge the bootstrap capacitor on the Q3 gate driver
 * (§5.4 bootstrap refresh design).
 *
 * Bootstrap refresh mechanism (CMP3-based):
 *   - At each Timer B period reset: CHB1 → LOW (RST source = TIMPER)
 *   - At count = BOOTSTRAP_REFRESH_TICKS (~200 ns): CHB1 → HIGH (SET source = CMP3)
 *   - CHB1 stays HIGH for the rest of the period (~4.8 µs at 200 kHz)
 *   CMP3xR is pre-programmed in hrtim_configure_compare_registers().
 *
 * Also restores Timer A (input side) to its switching configuration
 * (Set=TIMPER, Reset=EEV4|CMP2) after a potential boost-mode overwrite.
 *
 * During the 200 ns refresh pulse:
 *   Q1 (CHA1) is ON (Timer A just started its charge phase at the same period
 *   reset), Q3 (CHB1) is OFF, and Q4 (CHB2, complementary) is ON.  Current
 *   path: V_in → Q1 → L → Q4 → GND; output capacitor supplies the load.
 *   Effective duty-cycle reduction ≈ BOOTSTRAP_REFRESH_TICKS/HRTIM_PERIOD_COUNTS
 *   ≈ 4 %.  Monitor output ripple during bring-up and adjust if needed.
 */
static void hrtim_apply_buck_mode_static_leg(void)
{
    /* Restore Timer A (input side) switching configuration.
     * When transitioning from boost mode, Timer A's SET/RST were overwritten
     * for the bootstrap static leg.  Restore to buck switching sources.     */
#if SYNC_RECT_ENABLED
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R =
        HRTIM_OUTPUTSET_TIMPER;
#else
    /* The charge pulse starts at CMP4, after the Q2 bootstrap refresh pulse
     * (hrtim_configure_compare_registers); CMP2 is the backstop again after
     * boost mode used it as the pre-refresh turn-off.                      */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR =
        DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R =
        HRTIM_OUTPUTSET_TIMCMP4;
#endif
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R =
        HRTIM_OUTPUTRESET_EEV_4 | HRTIM_RST1R_CMP2;

    /* Configure Timer B (output side) for CMP3-based bootstrap refresh.
     * CMP3xR is already set to BOOTSTRAP_REFRESH_TICKS in
     * hrtim_configure_compare_registers(); only SET/RST sources are changed.*/
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = HRTIM_OUTPUTRESET_TIMPER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R = HRTIM_OUTPUTSET_TIMCMP3;
}

/**
 * hrtim_apply_boost_mode_static_leg — Configure Timer A for bootstrap-refreshed
 * static HIGH operation (boost mode input-side leg).
 *
 * In boost mode Timer A (input side) must hold CHA1 HIGH, with a brief LOW
 * pulse each period to recharge the bootstrap capacitor on the Q1 gate driver
 * (§5.4 bootstrap refresh design).
 *
 * Bootstrap refresh mechanism (CMP3-based, symmetric with buck mode):
 *   - At each Timer A period reset: CHA1 → LOW (RST source = TIMPER)
 *   - At count = BOOTSTRAP_REFRESH_TICKS (~200 ns): CHA1 → HIGH (SET source = CMP3)
 *   - CHA1 stays HIGH for the rest of the period (~4.8 µs at 200 kHz)
 *   CMP3xR is pre-programmed in hrtim_configure_compare_registers().
 *
 * Also restores Timer B (output side) to its switching configuration
 * (Set=EEV4|CMP2, Reset=TIMPER) after a potential buck-mode overwrite.
 *
 * During the 200 ns refresh pulse Timer B is simultaneously starting its
 * switching cycle (CHB1 also LOW at period reset).  Both low-side FETs Q2
 * (CHA2, after dead-time) and Q4 (CHB2, after dead-time) may be ON, clamping
 * inductor voltage to ~0 V.  Current change ΔI ≈ 0 over ~200 ns at 4.7 µH.
 * The boost blanking window (HRTIM_BLANKING_TICKS_BOOST = 2720 ticks = 500 ns)
 * covers the refresh pulse, preventing COMP1 false-trip on the switching
 * transient.  Verify safe operation during hardware bring-up.
 */
static void hrtim_apply_boost_mode_static_leg(void)
{
    /* Configure Timer A (input side) for CMP3-based bootstrap refresh.
     * CMP3xR is already set to BOOTSTRAP_REFRESH_TICKS in
     * hrtim_configure_compare_registers(); only SET/RST sources are changed. */
#if SYNC_RECT_ENABLED
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R = HRTIM_OUTPUTRESET_TIMPER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R = HRTIM_OUTPUTSET_TIMCMP3;
#else
    /* Without dead-time insertion on Timer A the refresh is explicit: TA2
     * (Q2) is high from the period reset to CMP3 (set up once in
     * hrtim_configure_compare_registers); Q1 turns off at CMP2, one
     * DCM_REFRESH_DEADTIME_NS before the period reset, and back on at CMP4,
     * the same margin after Q2 turns off.                                  */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR =
        HRTIM_PERIOD_COUNTS - DCM_REFRESH_DEADTIME_TICKS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx1R = HRTIM_RST1R_CMP2;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx1R = HRTIM_OUTPUTSET_TIMCMP4;
#endif

    /* Restore boost switching sources for Timer B (output side).
     * (These were overwritten by hrtim_apply_buck_mode_static_leg when
     * transitioning from buck mode, or are set here for initial boost start.) */
#if SYNC_RECT_ENABLED
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R =
        HRTIM_OUTPUTRESET_TIMPER;
#else
    /* TB1 low (so TB2, Q4, high) from CMP4 = DCM_PULSE_START_TICKS, see
     * hrtim_configure_compare_registers.                                  */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R =
        HRTIM_OUTPUTRESET_TIMCMP4;
#endif
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R =
        HRTIM_OUTPUTSET_EEV_4 | HRTIM_SET1R_CMP2;
#if !SYNC_RECT_ENABLED
    /* The PID ISR rewrites CMP2 every cycle (step 6b, up to
     * DCM_ON_TIME_CEIL_NS); until then the fixed DCM_MAX_ON_TIME_NS.      */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR =
        DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS;
#endif
}

/**
 * hrtim_apply_buck_boost_legs — REGULATOR_MODE_BUCK_BOOST (BUCK_BOOST_ENABLED):
 * Timer A as in buck (Q1 pulse CMP4 to CMP2, Q2 refresh from the period to
 * CMP3) and Timer B as in boost (Q4 pulse CMP4 to CMP2, Q3 not driven), so
 * Q1 and Q4 are on together and L1 charges from V_in.  When both turn off
 * the current freewheels from ground through Q2's body diode, through L1,
 * and into the output through Q3's.  The PID ISR writes the same CMP2 to
 * both timers.
 */
static void hrtim_apply_buck_boost_legs(void)
{
    hrtim_apply_buck_mode_static_leg();
#if !SYNC_RECT_ENABLED
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = HRTIM_OUTPUTRESET_TIMCMP4;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R =
        HRTIM_OUTPUTSET_EEV_4 | HRTIM_SET1R_CMP2;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR =
        DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS;
#endif
}

/**
 * mode_switching_outputs — HRTIM outputs enabled in a mode.
 *   buck:        TA1 TA2 (pulse, Q2 refresh), TB1 TB2 (Q3 static, refresh)
 *   boost:       HRTIM_BOOST_SWITCHING_OUTPUTS, TA1 TA2 (Q1 static, refresh)
 *   buck-boost:  TA1 TA2 (pulse, refresh), TB2 (pulse)
 */
static uint32_t mode_switching_outputs(RegulatorMode mode)
{
    if (mode == REGULATOR_MODE_BUCK)
    {
        return HRTIM_BUCK_SWITCHING_OUTPUTS | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2;
    }
    if (mode == REGULATOR_MODE_BUCK_BOOST)
    {
        return HRTIM_BUCK_SWITCHING_OUTPUTS | HRTIM_OUTPUT_TB2;
    }
    return HRTIM_BOOST_SWITCHING_OUTPUTS | HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2;
}

/**
 * change_mode_while_running — Reconfigure the HRTIM legs for new_mode and
 * restart the soft-start ramp from v_out_mv toward voltage_mv (§5.3, §11.4).
 * Called from the PID ISR on a setpoint-driven mode change and at the end
 * of the boost precharge.
 */
static void change_mode_while_running(RegulatorMode new_mode, uint32_t v_out_mv,
                                      uint32_t voltage_mv)
{
    /* Mode change during RUNNING: reconfigure HRTIM for the new mode.
     * Disable all outputs during the transition to prevent a partial
     * switching state (§5.3, §11.4).                                  */
    hrtim_disable_all_outputs();

    {
        uint32_t k = regulator_mode_log_index % 8u;
        regulator_mode_log[k][0] = HAL_GetTick();
        regulator_mode_log[k][1] = (uint32_t)regulator_mode | ((uint32_t)new_mode << 8);
        regulator_mode_log[k][2] = voltage_mv;
        regulator_mode_log[k][3] = regulator_vin_filt_mv;
        regulator_mode_log[k][4] = v_out_mv;
        regulator_mode_log_index++;
    }
    regulator_mode = new_mode;

    /* Reconfigure the static and switching legs for the new mode */
    if (regulator_mode == REGULATOR_MODE_BUCK)
    {
        hrtim_apply_buck_mode_static_leg();
    }
    else if (regulator_mode == REGULATOR_MODE_BUCK_BOOST)
    {
        hrtim_apply_buck_boost_legs();
    }
    else
    {
        hrtim_apply_boost_mode_static_leg();
    }
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, mode_switching_outputs(regulator_mode));

    /* Reset soft-start for the mode transition (§11.4) */
    uint32_t ramp_steps = (uint32_t)SOFT_START_RAMP_MS *
                          (uint32_t)PID_EXECUTION_RATE_HZ / 1000u;
    if (ramp_steps == 0u) { ramp_steps = 1u; }
    /* Ramp from the measured output (see regulator_start). */
    uint32_t v_out_now_mv = (v_out_mv > voltage_mv) ? voltage_mv : v_out_mv;
    softstart_increment_mv = (voltage_mv - v_out_now_mv) / ramp_steps;
    if (softstart_increment_mv == 0u) { softstart_increment_mv = 1u; }
    softstart_setpoint_mv = v_out_now_mv;
    softstart_active      = true;
    pid_reset_integrator(&pid_state);
}

/**
 * hrtim_disable_all_outputs — Disable all HRTIM outputs immediately.
 *
 * All FETs go to their idle/fault state (INACTIVE = LOW).
 * Used in regulator_stop() and enter_fault().
 */
static void hrtim_disable_all_outputs(void)
{
    HAL_HRTIM_WaveformOutputStop(&hhrtim1,
                                  HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
                                  HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2);
}

/**
 * power_path_enable — Assert INPUT_EN and OUTPUT_EN GPIOs.
 */
static void power_path_enable(void)
{
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_SET);
    /* OUTPUT_EN stays low here: the PID ISR (step 4c) connects VBUS once
     * V_out is regulating, if OUTPUT_SWITCH_ENABLED (regulator_config.h). */
    HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_RESET);
    output_switch_on = false;
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_RESET);
}

/**
 * power_path_disable — Deassert INPUT_EN and OUTPUT_EN GPIOs.
 */
static void power_path_disable(void)
{
    output_switch_on = false;
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_SET); // discharge VBUS
}

/**
 * determine_mode_from_voltages — Select buck or boost based on V_in vs
 * V_setpoint with hysteresis (§5.1).
 *
 * @param v_in_mv       Measured input voltage in millivolts.
 * @param v_setpoint_mv Target output voltage in millivolts.
 * @return REGULATOR_MODE_BUCK or REGULATOR_MODE_BOOST.
 */
static RegulatorMode determine_mode_from_voltages(uint32_t v_in_mv,
                                                   uint32_t v_setpoint_mv)
{
#if BUCK_BOOST_ENABLED
    /* Buck under V_in - BB_BELOW_VIN_MV, boost over V_in + BB_ABOVE_VIN_MV,
     * buck-boost between; each edge has BB_HYSTERESIS_MV against the mode
     * the regulator is in.                                                */
    const uint32_t lo = (v_in_mv > BB_BELOW_VIN_MV) ? (v_in_mv - BB_BELOW_VIN_MV) : 0u;
    const uint32_t hi = v_in_mv + BB_ABOVE_VIN_MV;
    const uint32_t h  = BB_HYSTERESIS_MV;
    switch (regulator_mode)
    {
        case REGULATOR_MODE_BUCK:
            if (v_setpoint_mv > hi + h) { return REGULATOR_MODE_BOOST; }
            if (v_setpoint_mv > lo + h) { return REGULATOR_MODE_BUCK_BOOST; }
            return REGULATOR_MODE_BUCK;
        case REGULATOR_MODE_BOOST:
            if (v_setpoint_mv + h < lo) { return REGULATOR_MODE_BUCK; }
            if (v_setpoint_mv + h < hi) { return REGULATOR_MODE_BUCK_BOOST; }
            return REGULATOR_MODE_BOOST;
        default:
            if (v_setpoint_mv + h < lo) { return REGULATOR_MODE_BUCK; }
            if (v_setpoint_mv > hi + h) { return REGULATOR_MODE_BOOST; }
            return REGULATOR_MODE_BUCK_BOOST;
    }
#endif
    if (v_in_mv > (v_setpoint_mv + MODE_HYSTERESIS_MV))
    {
        return REGULATOR_MODE_BUCK;
    }
    else if (v_setpoint_mv > (v_in_mv + MODE_HYSTERESIS_MV))
    {
        return REGULATOR_MODE_BOOST;
    }
    else
    {
        /* Within hysteresis band — maintain current mode (§5.1) */
        return regulator_mode;
    }
}

/**
 * enter_fault — Transition to FAULT state from any context.
 *
 * Safe to call from ISR (no FreeRTOS API).
 *
 * @param source  Bitmask of fault source(s).
 */
static void enter_fault(RegulatorFaultSource source)
{
    /* Disarm the fault IRQ first: dropping INPUT_EN below makes VS_GOOD fall,
     * and a stuck fault line must not re-enter the ISR (see fault ISR).    */
    hrtim_fault_irq_disarm();

    /* Record fault source FIRST before disabling outputs */
    regulator_last_fault_source = source;
    regulator_fault_tick_ms     = HAL_GetTick();

    /* Leave RUNNING before the outputs go off, for the same reason as in
     * regulator_stop(): the Timer A period ISR re-enables TA1 every period
     * while RUNNING, and it can preempt this function between the disable
     * below and a state change at the end.                                  */
    regulator_state  = REGULATOR_STATE_FAULT;

    /* Disable switching outputs (belt-and-suspenders: hardware fault logic
     * already forces them to INACTIVE, but also do it in software)          */
    hrtim_disable_all_outputs();
    bench_pwm_restore();

    /* Zero DAC — comparator threshold = 0 → additional safety (§4.4) */
    DAC3->DHR12R1 = 0u;

    /* Stop timers */
    TIM6->CR1 &= ~TIM_CR1_CEN;   /* stop TIM6 (slope comp) */
    TIM7->CR1 &= ~TIM_CR1_CEN;   /* stop TIM7 (PID)        */

    /* Drop the input and output paths.  INPUT_EN low is also the first half
     * of the ENABLE toggle the ADM1270 needs to re-arm after a current trip,
     * and starts its cool-down (see regulator_clear_fault).                */
    power_path_disable();

    /* Update shared state for PD stack */
    regulator_fault  = true;
    regulator_ready  = false;
}

/**
 * software_safety_checks_pass — Check all software safety conditions (§10.2).
 *
 * Returns true if all conditions are within bounds.
 * Returns false and calls enter_fault() if any condition is violated.
 */
static void record_fault_snapshot(uint32_t v_out_mv, uint32_t v_in_mv,
                                  uint32_t v_setpoint_mv)
{
    regulator_fault_snapshot[0] = v_out_mv;
    regulator_fault_snapshot[1] = v_in_mv;
    regulator_fault_snapshot[2] = adc1_dma_buffer[ADC1_DMA_INDEX_VD_MON];
    regulator_fault_snapshot[3] = regulator_vout_est_raw;
    regulator_fault_snapshot[4] = v_setpoint_mv;
    regulator_fault_snapshot[5] = regulator_period_trace_index;
}

static bool software_safety_checks_pass(uint32_t v_out_mv,
                                         uint32_t v_in_mv,
                                         uint32_t v_setpoint_mv,
                                         RegulatorMode mode)
{
    /* --- 1. Absolute OVP (§10.2) --- */
    if (v_out_mv > OVP_ABSOLUTE_MV)
    {
        record_fault_snapshot(v_out_mv, v_in_mv, v_setpoint_mv);
        enter_fault(REGULATOR_FAULT_SW_OVP);
        return false;
    }

    /* --- 2. Relative OVP --- */
    if (v_setpoint_mv > 0u)
    {
        uint32_t ovp_threshold = (v_setpoint_mv * OVP_RELATIVE_PCT) / 100u;
        if (v_out_mv > ovp_threshold)
        {
            record_fault_snapshot(v_out_mv, v_in_mv, v_setpoint_mv);
        enter_fault(REGULATOR_FAULT_SW_OVP);
            return false;
        }

        /* --- 3. Relative UVP (only when not in soft-start) --- */
        if (!softstart_active)
        {
            uint32_t uvp_threshold = (v_setpoint_mv * UVP_RELATIVE_PCT) / 100u;
            if (v_out_mv < uvp_threshold)
            {
                record_fault_snapshot(v_out_mv, v_in_mv, v_setpoint_mv);
                enter_fault(REGULATOR_FAULT_SW_UVP);
                return false;
            }
        }
    }

    /* --- 4. V_in range check (§10.2) ---
     * Latched only when out of range for VIN_RANGE_PERSIST_CYCLES PID
     * cycles in a row.  V_in is one ADC2 sample per cycle and VS_MON sees
     * the same switching ring as VD_MON: run 36 (2026-09-28, boost 28 V)
     * faulted after 32 s on one sample of 27.8 V while every logged cycle
     * read 23.0 to 24.0 V and the scope's CH1 read 24.1 V.  The supply
     * does not move in 1 ms; the ADM1270 and the FLT inputs cover the fast
     * cases.                                                             */
    bool vin_out_of_range;
    if (mode == REGULATOR_MODE_BUCK)
    {
        /* Buck requires V_in > V_out + margin.  Not while precharging for
         * boost: the buck ramp is capped under V_in by construction and
         * v_setpoint_mv is the boost target.                              */
        vin_out_of_range = !boost_precharge &&
                           (v_in_mv < (v_setpoint_mv + BUCK_VIN_MARGIN_MV));
    }
    else if (mode == REGULATOR_MODE_BUCK_BOOST)
    {
        vin_out_of_range = false;   /* works at any V_out / V_in */
    }
    else
    {
        /* Boost requires V_in < V_out - margin */
        vin_out_of_range = (v_setpoint_mv > BOOST_VIN_MARGIN_MV) &&
                           (v_in_mv > (v_setpoint_mv - BOOST_VIN_MARGIN_MV));
    }
    if (!vin_out_of_range)
    {
        vin_range_cycles = 0u;
    }
    else if (++vin_range_cycles >= VIN_RANGE_PERSIST_CYCLES)
    {
        record_fault_snapshot(v_out_mv, v_in_mv, v_setpoint_mv);
        enter_fault(REGULATOR_FAULT_SW_VIN_RANGE);
        return false;
    }

    return true;
}
