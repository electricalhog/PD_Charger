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
#include <math.h>           /* sqrtf: the current-command modulator (PID ISR step 6) */

/* PC8 is OUTPUT_EN on the power board and ENABLE on the X-NUCLEO-SRC1M1
 * TCPP0203.  While the shield is stacked the PD stack owns the pin and holds
 * it high (pd_power.c); the regulator must not drop it.  */
#if OUTPUT_EN_SHARED_WITH_TCPP
#define OUTPUT_EN_WRITE(state) ((void)(state))
#else
#define OUTPUT_EN_WRITE(state) HAL_GPIO_WritePin(PIN_OUTPUT_EN_PORT, PIN_OUTPUT_EN_PIN, (state))
#endif

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

/* =========================================================================
 * V_out sensing and pulse skipping (2026-09-28 evening)
 *
 * ADC1 converts its four channels once per switching period, triggered by
 * HRTIM ADC trigger 1 on master compare 1 so that VD_MON's sample ends
 * ADC_VD_SAMPLE_END_NS into the period, after the pulse and the inductor
 * discharge at every operating point swept (they end by about 2.5 us), and
 * before the next period's refresh edge.  DMA fills a ring of
 * ADC1_DMA_SCANS scans (adc_monitor.h).
 *
 * This replaced a free-running scan (9.9 us, two periods, landing at any
 * phase of the ringing) read by a 200 kHz Timer A ISR that estimated V_out
 * with a pulse-credit slew limiter and decided pulse skipping in software.
 * That ISR was 24 % of the CPU (bu profile), and the estimator needed three
 * rounds of rules against glitch runs (runs 10 to 38 and this evening: pairs,
 * staircases, a real overshoot it held off).
 *
 * Pulse skipping is ADC1 analog watchdog 1 on VD_MON, confirmed in its ISR
 * over AWD_FILTER_SAMPLES consecutive samples (the hardware filter does not
 * work with a multi-channel scan, adc_monitor.h): while pulsing the window
 * is [0, on], while skipping [off, 4095], so regulator_adc1_awd_isr runs only
 * when V_out crosses the band.  The PID ISR moves the window with the
 * setpoint.  V_out for the PID, soft-start, OVP/UVP and the debug log is the
 * trimmed mean of the last VOUT_MEDIAN_SAMPLES samples (highest and lowest dropped).
 * =========================================================================*/

/** 1 while the charge pulses are held off (V_out above the band); written
 *  by regulator_adc1_awd_isr.                                             */
volatile uint8_t regulator_pulses_skipped = 0u;

/** Skip band in raw VD_MON counts: skip above `on`, resume below `off`.
 *  Written by the PID ISR from the effective (soft-start) setpoint.       */
volatile uint16_t regulator_skip_raw_threshold = 0xFFFu;
volatile uint16_t regulator_skip_off_raw       = 0xFFFu;

/** Skip transitions (both directions) since start, and watchdog interrupts
 *  the software filter rejected (single out-of-window samples); telemetry. */
volatile uint32_t regulator_skip_events = 0u;
volatile uint32_t regulator_awd_rejected = 0u;

/** V_out in raw counts (trimmed mean), as the PID ISR last computed it.  */
volatile uint16_t regulator_vout_est_raw = 0u;

/** The skip band sits max(PULSE_SKIP_ABOVE_MV, setpoint * this / 1000)
 *  above the setpoint (runtime-tunable).  At +200 mV with the 330 ohm load
 *  (2026-09-28 evening, correct 200 kHz sampling) V_out swung 27.4 to 28.4 V
 *  around a 27.9 V mean: the band clipped the top, the PID saw a lasting
 *  positive error and sat at the peak limit, and the converter burst at
 *  about 9 kHz (audible) whatever kp.  With the band clear of the ripple the
 *  PID shrinks the pulses until they come every period: at 28 V, 1.5 % gave
 *  V_out sd 32 mV and 27 skip transitions/s, 3 % 56 mV and 7/s, 5 % 66 mV
 *  (sd was 272 mV and 17 500/s at +200 mV).                              */
volatile uint16_t regulator_skip_above_permille = SKIP_ABOVE_PERMILLE_DEFAULT;
_Static_assert(SKIP_ABOVE_PERMILLE_DEFAULT < (OVP_RELATIVE_PCT - 100u) * 10u / 2u,
               "skip band must stay well under the relative OVP");

/** Hysteresis on the skip decision, raw counts (3 counts is 57 mV).      */
#define SKIP_HYSTERESIS_RAW 3u

/** Bench alignment snapshot (mailbox SNAPSHOT): the VD_MON ring frozen at
 *  the marker, oldest first, and the PID cycles the pulses stay held off
 *  (the marker on SW2 the scope triggers on).                              */
volatile uint16_t regulator_snapshot_trace[ADC1_DMA_SCANS];
static volatile uint8_t snapshot_hold_cycles = 0u;
volatile uint8_t  regulator_adc_per_pid[16];
volatile uint32_t regulator_adc_per_pid_idx = 0u;
extern ADC_HandleTypeDef hadc1;
#define SNAPSHOT_HOLD_PID_CYCLES 2u   /* 100 us without pulses */

/** The last ADC1_DMA_SCANS VD_MON samples (one per period, oldest first),
 *  frozen when a software fault latches (the DMA ring keeps running).     */
volatile uint16_t regulator_fault_trace[ADC1_DMA_SCANS];

/* Boost starts (2026-09-28, runs 32 and 33).  Starting boost directly turns
 * Q1, the input leg's static switch, fully on into an empty output: V_in
 * steps across L1 and C_out, the tank rings toward 2 * V_in, and Q3's
 * reverse conduction holds the peak (run 33: 0, 1.0, 8.6, then 38 V within
 * 25 us).  Since 2026-10-12 the regulator never enters boost unless V_out
 * is already above V_in + BB_ABOVE_VIN_MV (determine_mode_from_voltages):
 * buck-boost carries the output across V_in, which replaced the buck
 * precharge that used to stand in for it.                                 */

/** PID cycles in a row with V_in outside the mode's range (see the V_in
 *  range check in software_safety_checks_pass). */
#define VIN_RANGE_PERSIST_CYCLES 20u   /* 1 ms at 20 kHz */
static uint16_t vin_range_cycles = 0u;

/* What the software safety check saw when it last latched a fault, for the
 * bench (2026-09-28, run 34: SW_OVP with the estimate at 31.2 V while the
 * period trace and debug log up to the fault read 29.1 V).  u32 each:
 * v_out_mv (the median), v_in_mv, the newest raw VD_MON sample, the median
 * in raw counts, the setpoint passed, skip transitions so far.           */
volatile uint32_t regulator_fault_snapshot[6] = {0u};


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
/* Since 2026-10-12 the loop commands output current (PID ISR step 5) and
 * its gains follow from the capacitance on the output, every cycle (step 2,
 * regulator_config.h LOOP_CROSSOVER_HZ): pid_kp [mA/mV] and pid_ki
 * [mA/(mV s)] report the values in use, and writing them has no effect.
 * Tune regulator_loop_fc_hz and regulator_c_int_uf / regulator_c_ext_uf.
 * The notes above are the history of the peak-current loop it replaced.   */
float   pid_kp = 0.0f;
float   pid_ki = 0.0f;
float   pid_kd = 0.0f;
volatile float regulator_loop_fc_hz = LOOP_CROSSOVER_HZ;
volatile float regulator_c_int_uf   = LOOP_C_INT_UF;
volatile float regulator_c_ext_uf   = LOOP_C_EXT_UF;

/** Loop telemetry (steps 5 and 6): the average output-current command, the
 *  peak current it asks of this mode, and the most this mode can deliver at
 *  the present operating point (the command's ceiling).                    */
volatile float regulator_i_cmd_ma = 0.0f;
volatile float regulator_i_pk_ma  = 0.0f;
volatile float regulator_i_max_ma = 0.0f;

/** Charge-pulse on-time commanded this PID period, ns (DCM, every mode). */
volatile uint16_t regulator_on_time_ns = 0u;

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

_Static_assert(AWD_FILTER_SAMPLES >= 1u && AWD_FILTER_SAMPLES <= 8u, "AWD_FILTER_SAMPLES 1..8");
_Static_assert(DCM_PULSE_START_TICKS + HRTIM_NS_TO_TICKS(DCM_ON_TIME_CEIL_NS) +
               DCM_REFRESH_DEADTIME_TICKS < HRTIM_PERIOD_COUNTS,
               "DCM_ON_TIME_CEIL_NS does not fit in the period");

/** The target the loop regulates to: target_voltage_mv followed at
 *  SETPOINT_SLEW_UP_MV_PER_CYCLE / SETPOINT_SLEW_DOWN_MV_PER_CYCLE (PID ISR
 *  step 4a).  Telemetry for bu.                                           */
volatile uint32_t regulator_commanded_mv = 0u;

/** What the relative OVP is measured against (PID ISR step 4a): the
 *  commanded setpoint, except during a downward transition, when it holds
 *  the level the transition started from until the setpoint is there and
 *  V_out is within OVP_HOLD_RELEASE_PCT of it.  Telemetry for bu.         */
volatile uint32_t regulator_ovp_ref_mv = 0u;

/** V_in low-passed for mode selection (VIN_FILTER_SHIFT).                */
volatile uint32_t regulator_vin_filt_mv = 0u;

/** The last three ADC2 V_in results (PID ISR step 1): the modulator, the
 *  checks and the filter above use their median, so one bad conversion
 *  cannot set a PID cycle's on-time.                                       */
static uint32_t vin_hist_mv[3];
static uint8_t  vin_hist_next = 0u;

static inline uint32_t median3(uint32_t a, uint32_t b, uint32_t c)
{
    if (a > b) { const uint32_t t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return (a > b) ? a : b;
}

/** Peak-current comparator thresholds, DAC counts: protection only
 *  (PEAK_PROTECT_PCT above the mode's commanded peak limit; step 6).       */
#define PEAK_PROTECT_MA(peak_ma) (((peak_ma) * PEAK_PROTECT_PCT / 100u) < PEAK_PROTECT_MAX_MA \
                                      ? ((peak_ma) * PEAK_PROTECT_PCT / 100u) : PEAK_PROTECT_MAX_MA)
#define PEAK_PROTECT_COUNTS       (PEAK_PROTECT_MA(DCM_MAX_PEAK_MA) * DAC_COUNTS_PER_AMP / 1000u)
#define PEAK_PROTECT_COUNTS_BB    (PEAK_PROTECT_MA(DCM_MAX_PEAK_MA_BB) * DAC_COUNTS_PER_AMP / 1000u)
#define PEAK_PROTECT_COUNTS_BOOST (PEAK_PROTECT_MA(DCM_MAX_PEAK_MA_BOOST) * DAC_COUNTS_PER_AMP / 1000u)
_Static_assert(PEAK_PROTECT_COUNTS <= PID_OUTPUT_MAX, "PEAK_PROTECT_COUNTS over the DAC range");

/* =========================================================================
 * Predictive synchronous rectification (regulator_config.h, SR_*)
 *
 * The rectifier edges come from Timer C (Q2, TA2: events 3 = C CMP2 on,
 * 4 = C CMP3 off) and Timer D (Q3, TB1: events 5 = D CMP3 on, 6 = D CMP4
 * off).  C and D only count: their outputs are never enabled.  All four
 * timers run in lockstep (started in one MCR write, same period and
 * prescaler) with preload, updating at their period roll-over (TRSTU); the
 * PID ISR holds the updates (CR1 TxUDIS) while it writes a cycle's compares,
 * so every period uses one consistent set of edges.  A write landing between
 * an old pulse end and a new rectifier start can then never overlap a pulse
 * switch and its rectifier (shoot-through).  "Parked" compares
 * (HRTIM_CMP_PARKED) never match: no rectifier that period.
 * =========================================================================*/
volatile uint8_t  regulator_sr_enable = SR_ENABLE_DEFAULT;   /**< runtime switch */
volatile uint16_t regulator_sr_on_ns  = 0u;   /**< last window programmed, ns from */
volatile uint16_t regulator_sr_off_ns = 0u;   /**< the period start; 0 = none       */
/** Rectifier fade-in, 0 to SR_BLEND_FULL: switching it on at once steps the
 *  plant gain (the 2-3 V diode drop leaves the freewheel path; at 5 V that
 *  is about +50 % charge per pulse) and overshot to the relative OVP on
 *  2026-10-15.  The window and the model's diode term follow the blend over
 *  SR_BLEND_FULL PID cycles; switching off is immediate. */
#define SR_BLEND_FULL 2000u   /* 100 ms at 20 kHz */
volatile uint16_t regulator_sr_blend = 0u;
/** Buck phase 2 (Q3 window, Q4 refresh) enabled: follows regulator_sr_enable
 *  from the PID ISR, so the switch works without a restart. */
static bool buck_phase2_on = false;
_Static_assert(HRTIM_PERIOD_COUNTS < HRTIM_CMP_PARKED, "parked compare must be beyond the period");

#define HRTIM_TIMER_ABCD_UDIS (HRTIM_CR1_TAUDIS | HRTIM_CR1_TBUDIS | HRTIM_CR1_TCUDIS | HRTIM_CR1_TDUDIS)
#define HRTIM_TIMER_ABCD_SWU  (HRTIM_CR2_TASWU | HRTIM_CR2_TBSWU | HRTIM_CR2_TCSWU | HRTIM_CR2_TDSWU)

/** Transfer every preloaded register of Timers A-D now (configuration
 *  writes outside the PID ISR: init, mode changes, bench PWM). */
static inline void hrtim_update_now(void)
{
    HRTIM1->sCommonRegs.CR2 |= HRTIM_TIMER_ABCD_SWU;
}

/** No rectifier edges (preloaded: from the next update). */
static void hrtim_park_sr(void)
{
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C].CMP2xR = HRTIM_CMP_PARKED;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C].CMP3xR = HRTIM_CMP_PARKED;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D].CMP3xR = HRTIM_CMP_PARKED;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D].CMP4xR = HRTIM_CMP_PARKED;
    regulator_sr_on_ns  = 0u;
    regulator_sr_off_ns = 0u;
}
_Static_assert(PEAK_PROTECT_COUNTS_BOOST <= PID_OUTPUT_MAX, "PEAK_PROTECT_COUNTS_BOOST over the DAC range");
_Static_assert(PEAK_PROTECT_COUNTS_BB <= PID_OUTPUT_MAX, "PEAK_PROTECT_COUNTS_BB over the DAC range");
_Static_assert(BUCK_BOOST_ENABLED, "mode selection relies on buck-boost to cross V_in (2026-10-12)");
_Static_assert(!SYNC_RECT_ENABLED, "the current-command modulator (PID ISR step 6) is DCM-only");

/** ISR cost in CPU cycles (DWT CYCCNT, 170 MHz), bring-up instrumentation
 *  (2026-09-28 evening: uwTick ran at 0.02 of real time in boost, the PID
 *  ISR active in every NVIC sample).  [0] last, [1] max, [2] running mean
 *  x16.  pid includes time preempted by the Timer A ISR.  Zero the max with
 *  a debugger write.                                                      */
volatile uint32_t regulator_cyc_pid[3];
volatile uint32_t regulator_cyc_tima[3];
volatile uint32_t regulator_cyc_awd[3];
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
static void hrtim_configure_adc_trigger(void);

/** Outputs the pulse-skip watchdog turns off and on in a mode: the pulse
 *  output(s) and the other leg's bootstrap refresh.  A skipped period
 *  switches nothing: the refresh pulse alone rings L1 against the node
 *  capacitances and pumps the output through the off leg's reverse
 *  conduction (run 31, 29 to 32 V at no load with every pulse skipped).
 *  Buck-boost keeps the Q2 refresh (TA2) that Q1's bootstrap needs, as
 *  buck does.                                                             */
static inline uint32_t skip_outputs(RegulatorMode mode)
{
    /* Q3 (TB1) too: a skipped pulse must not leave a rectifier window that
     * drives current backwards.  Q2's window (TA2) may stay: with the pulse
     * switch and Q3 off it has no current path. */
    if (mode == REGULATOR_MODE_BOOST)      { return HRTIM_OUTPUT_TB2 | HRTIM_OUTPUT_TA2 | HRTIM_OUTPUT_TB1; }
    if (mode == REGULATOR_MODE_BUCK_BOOST) { return HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TB2 | HRTIM_OUTPUT_TB1; }
    /* Buck with phase 2 on: the Q4 refresh too.  Alone it rings SW2 up
     * through Q3's body diode and pumps the output: 2026-10-15, rectifier
     * on at 5 V, V_out climbed to the relative OVP (5.52 V) through 1121
     * skip transitions. */
    return HRTIM_OUTPUT_TA1 | (buck_phase2_on ? (HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2) : 0u);
}

/** Pulsing, with the watchdog window [0, on] (outputs already enabled). */
static void skip_state_reset_pulsing(void)
{
    regulator_pulses_skipped = 0u;
    adc_monitor_awd_window(0u, regulator_skip_raw_threshold);
    ADC1->ISR = ADC_ISR_AWD1;
}
static uint32_t mode_switching_outputs(RegulatorMode mode);
static void hrtim_disable_all_outputs(void);

static void change_mode_while_running(RegulatorMode new_mode, uint32_t v_out_mv,
                                      uint32_t voltage_mv);
static void power_path_enable(void);
static void power_path_disable(void);
static RegulatorMode determine_mode_from_voltages(uint32_t v_in_mv, uint32_t v_setpoint_mv,
                                                   uint32_t v_out_mv);
static void enter_fault(RegulatorFaultSource source);
static bool software_safety_checks_pass(uint32_t v_out_mv, uint32_t v_in_mv,
                                         uint32_t v_setpoint_mv, uint32_t ovp_ref_mv,
                                         RegulatorMode mode);

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

    /* Flash prefetch: PREFETCH_ENABLE is 0 in the generated
     * stm32g4xx_hal_conf.h; with 4 wait states at 170 MHz it cut the PID
     * ISR from 12.4 to 11.2 us (bench, 2026-09-28).                       */
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();

    /* DWT cycle counter for regulator_cyc_* */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* --- Step 5: Initialise ADC subsystem --- */
    adc_monitor_init();
    adc_monitor_awd_init();                    /* interrupt armed by regulator_start */
    adc_monitor_start_adc1_dma();              /* waits for HRTIM ADC trigger 1 */

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
    /* Gains and output limits are recomputed every PID cycle (steps 2, 5). */
    pid_config.kp          = 0.0f;
    pid_config.ki          = 0.0f;
    pid_config.kd          = 0.0f;
    pid_config.dt_seconds  = 1.0f / (float)PID_EXECUTION_RATE_HZ;  /* 50 µs */
    pid_config.output_min  = 0.0f;
    pid_config.output_max  = 0.0f;
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
    hrtim_update_now();

    /* --- Step 12: Initialise PD interface shared state --- */
    pd_interface_init();

    /* --- Step 13: Power path GPIO — ensure both paths are disabled at init --- */
    HAL_GPIO_WritePin(PIN_INPUT_EN_PORT,  PIN_INPUT_EN_PIN,  GPIO_PIN_RESET);
    OUTPUT_EN_WRITE(GPIO_PIN_RESET);
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
    vin_hist_mv[0] = vin_hist_mv[1] = vin_hist_mv[2] = v_in_mv;
    adc_monitor_trigger_vin();   /* the first PID cycle reads this one */
    regulator_commanded_mv = voltage_mv;
    regulator_ovp_ref_mv   = voltage_mv;
    /* Start in buck below the buck-boost band, otherwise in buck-boost, and
     * never in boost (see "Boost starts"): step 3b moves on to boost once
     * V_out is above V_in + BB_ABOVE_VIN_MV.                              */
    vin_range_cycles = 0u;
    regulator_mode = (voltage_mv + BB_HYSTERESIS_MV + BB_BELOW_VIN_MV < v_in_mv)
                         ? REGULATOR_MODE_BUCK : REGULATOR_MODE_BUCK_BOOST;

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
    /* The ramp starts at 0.  A residual or primed V_out (run 1's 410 mV in
     * buck, about V_in in boost) sits above the skip band until the ramp
     * reaches it, so the watchdog holds the pulses off meanwhile (armed
     * below, band set by the first PID cycle).                            */
    uint32_t v_out_now_mv = 0u;
    regulator_skip_raw_threshold = (uint16_t)((PULSE_SKIP_ABOVE_MV * ADC_FULL_SCALE_COUNTS) / VD_MON_FULL_SCALE_MV);
    regulator_skip_off_raw       = 0u;
    skip_state_reset_pulsing();
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
    hrtim_park_sr();
    hrtim_update_now();
    buck_phase2_on = (regulator_mode == REGULATOR_MODE_BUCK) && regulator_sr_enable;

    /* --- Enable HRTIM switching outputs for the active legs (§5.5) --- */
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, mode_switching_outputs(regulator_mode));

    /* --- Initialise slope compensation and start TIM6 --- */
    adc_monitor_scale_adc1_buffer();
    uint32_t v_out_mv = (uint32_t)adc_measurements.v_out_mv;
    slope_comp_update_step(v_out_mv, v_in_mv,
                            (SlopeCompMode)regulator_mode);
    slope_comp_start(0u);   /* peak starts at 0; ramps up with soft-start */

    /* --- Start the PID timer (TIM7); the pulse-skip watchdog is armed
     * after the state is RUNNING (below).                                 */
    HAL_TIM_Base_Start_IT(&htim7);

    regulator_state = REGULATOR_STATE_RUNNING;

    /* Arm the pulse-skip watchdog only now: its ISR disarms itself when the
     * state is not RUNNING, and armed earlier a boot sample above the
     * initial 200 mV window got there first on one boot in three: no
     * skipping for the whole run, and an unloaded boost precharge handover
     * pumped V_out to the OVP (2026-09-28 evening).  The PID ISR re-arms it
     * as well if it ever finds it off while RUNNING.                       */
    ADC1->ISR  = ADC_ISR_AWD1;
    ADC1->IER |= ADC_IER_AWD1IE;
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
    else if (cmd == REGULATOR_DEBUG_CMD_SNAPSHOT)
    {
        if (regulator_state != REGULATOR_STATE_RUNNING)
        {
            debug_complete(cmd, 1u);
            return;
        }
        uint32_t scan = adc_monitor_latest_scan();
        for (uint32_t k = 0u; k < ADC1_DMA_SCANS; k++)   /* oldest first */
        {
            scan = (scan + 1u) % ADC1_DMA_SCANS;
            regulator_snapshot_trace[k] = adc_monitor_vd_at(scan);
        }
        HRTIM1->sCommonRegs.ODISR = skip_outputs(regulator_mode);
        snapshot_hold_cycles = SNAPSHOT_HOLD_PID_CYCLES;
        debug_complete(cmd, 0u);
    }
    else if (cmd == REGULATOR_DEBUG_CMD_SET_VOLTAGE)
    {
        uint32_t mv = regulator_debug.arg;
        uint32_t result = REGULATOR_SET_OUT_OF_RANGE;
        if (PD_OWNS_VBUS)
        {
            result = REGULATOR_SET_PD_OWNS_VBUS;
        }
        else if (mv >= SETPOINT_MIN_MV && mv <= SETPOINT_MAX_MV)
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
            if (PD_OWNS_VBUS)
            {
                result = REGULATOR_SET_PD_OWNS_VBUS;
            }
            else if (mv < SETPOINT_MIN_MV || mv > SETPOINT_MAX_MV)
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
    /* Bench PWM still uses the dead-time pair on Timer B (TB2 = !TB1) with
     * the pre-2026-10-15 sources: Q3 static, Q4 refresh. */
    tb->SETx1R = HRTIM_OUTPUTSET_TIMCMP3;
    tb->RSTx1R = HRTIM_OUTPUTRESET_TIMPER;
    tb->OUTxR |= HRTIM_OUTR_DTEN;
    hrtim_park_sr();
    hrtim_update_now();
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
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].OUTxR &= ~HRTIM_OUTR_DTEN;
    hrtim_update_now();
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
    /* Unused since 2026-09-28 evening: the Timer A period interrupt is no
     * longer enabled (see "V_out sensing and pulse skipping" at the top).  */
}

/**
 * regulator_adc1_awd_isr — ADC1 analog watchdog 1: V_out crossed the skip
 * band (after AWD_FILTER_SAMPLES consecutive samples).  Toggles the pulse
 * outputs and swings the window to the other edge of the band.
 * Priority: NVIC_PRIORITY_HRTIM (1), above the PID ISR, which also writes
 * the window (inside a critical section).
 */
void regulator_adc1_awd_isr(void)
{
    const uint32_t cyc0 = DWT->CYCCNT;
    ADC1->ISR = ADC_ISR_AWD1;
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        ADC1->IER &= ~ADC_IER_AWD1IE;
        return;
    }
    /* Software filter: act only if the last AWD_FILTER_SAMPLES samples are
     * all out of the window; otherwise a later sample re-raises the flag.  */
    {
        uint16_t v[8];
        adc_monitor_vd_recent(v, AWD_FILTER_SAMPLES);
        for (uint32_t k = 0u; k < AWD_FILTER_SAMPLES; k++)
        {
            bool out = regulator_pulses_skipped ? (v[k] < regulator_skip_off_raw)
                                                : (v[k] > regulator_skip_raw_threshold);
            if (!out)
            {
                regulator_awd_rejected++;
                cyc_record(regulator_cyc_awd, DWT->CYCCNT - cyc0);
                return;
            }
        }
    }
    const uint32_t outs = skip_outputs(regulator_mode);
    if (regulator_pulses_skipped == 0u)
    {
        HRTIM1->sCommonRegs.ODISR = outs;
        regulator_pulses_skipped  = 1u;
        adc_monitor_awd_window(regulator_skip_off_raw, 0xFFFu);
    }
    else
    {
        if (snapshot_hold_cycles == 0u) { HRTIM1->sCommonRegs.OENR = outs; }
        regulator_pulses_skipped = 0u;
        adc_monitor_awd_window(0u, regulator_skip_raw_threshold);
    }
    regulator_skip_events++;
    cyc_record(regulator_cyc_awd, DWT->CYCCNT - cyc0);
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
    {   /* bench: ADC1 results per PID period (DMA ring advance), last 16 */
        static uint32_t prev_next = 0u;
        uint32_t next = ADC1_DMA_BUFFER_LENGTH - hadc1.DMA_Handle->Instance->CNDTR;
        regulator_adc_per_pid[regulator_adc_per_pid_idx++ & 15u] =
            (uint8_t)((next + ADC1_DMA_BUFFER_LENGTH - prev_next) % ADC1_DMA_BUFFER_LENGTH);
        prev_next = next;
    }
    if (snapshot_hold_cycles != 0u && --snapshot_hold_cycles == 0u &&
        regulator_pulses_skipped == 0u)
    {
        HRTIM1->sCommonRegs.OENR = skip_outputs(regulator_mode);   /* marker over */
    }
    regulator_debug_poll_isr();
    if (regulator_state != REGULATOR_STATE_RUNNING)
    {
        return;   /* a STOP from the mailbox */
    }
    if ((ADC1->IER & ADC_IER_AWD1IE) == 0u)
    {
        ADC1->IER |= ADC_IER_AWD1IE;   /* see regulator_start */
    }

    /* --- 1. ADC conversions ---
     * V_in: the result of the conversion the previous cycle started (16
     * oversampled, 13.9 us, adc_monitor_init), then the next one.  The
     * cycle uses the median of the last three results.                     */
    const uint32_t cyc_poll = DWT->CYCCNT;
    if (adc_monitor_poll_vin_result())
    {
        vin_hist_mv[vin_hist_next] = adc_measurements.v_in_mv;
        vin_hist_next = (uint8_t)((vin_hist_next + 1u) % 3u);
    }
    adc_monitor_trigger_vin();
    cyc_record(regulator_cyc_vin_poll, DWT->CYCCNT - cyc_poll);
    uint32_t v_in_mv = median3(vin_hist_mv[0], vin_hist_mv[1], vin_hist_mv[2]);

    /* V_out as the trimmed mean of the last VOUT_MEDIAN_SAMPLES VD_MON
     * samples (see "V_out sensing and pulse skipping"): the PID, soft-start,
     * OVP/UVP and the debug log all see it.  The newest raw sample is in
     * adc_measurements.v_out_mv.                                          */
    regulator_vout_est_raw = adc_monitor_update(VOUT_MEDIAN_SAMPLES);   /* also the currents */
    uint32_t v_out_mv  = ((uint32_t)regulator_vout_est_raw
                          * VD_MON_FULL_SCALE_MV) / ADC_FULL_SCALE_COUNTS;
    uint32_t i_l_ma    = adc_measurements.i_inductor_ma;
    (void)i_l_ma;   /* telemetry only */

    /* TODO(debug): If v_out_mv or v_in_mv reads zero, check ADC1/ADC2 DMA
     * init in stm32g4xx_hal_msp.c and confirm INPUT_EN is asserted before
     * regulator_start() powers the voltage-divider network (§5.1). */

    /* --- 2. Loop gains from the capacitance on the output (regulator_config.h,
     * LOOP_CROSSOVER_HZ): kp = 2 pi fc C [mA/mV], PI zero at fc / 4.  The
     * VBUS side counts once the output switch is on (step 4c).            */
    {
        const float c_uf = regulator_c_int_uf + (output_switch_on ? regulator_c_ext_uf : 0.0f);
        const float w_c  = 6.2831853f * regulator_loop_fc_hz;
        pid_kp = w_c * c_uf * 1e-6f;
        pid_ki = pid_kp * w_c * 0.25f;
        pid_config.kp = pid_kp;
        pid_config.ki = pid_ki;
        pid_config.kd = 0.0f;
    }

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

    /* --- 4a. Setpoint slew and the relative-OVP reference ---
     * Rising, the OVP reference follows the setpoint.  Falling, V_out trails
     * the setpoint by as much as the load allows (DCM, no reverse flow), so
     * the reference holds the level the transition started from until the
     * setpoint is at the target and V_out is back within
     * OVP_HOLD_RELEASE_PCT of it (regulator_config.h).  A real overshoot
     * above the old level still trips.                                     */
    {
        uint32_t tgt = target_voltage_mv;
        uint32_t cmd = regulator_commanded_mv;
        if (tgt < cmd && regulator_ovp_ref_mv < cmd) { regulator_ovp_ref_mv = cmd; }
        if (cmd + SETPOINT_SLEW_UP_MV_PER_CYCLE < tgt)        { cmd += SETPOINT_SLEW_UP_MV_PER_CYCLE; }
        else if (cmd > tgt + SETPOINT_SLEW_DOWN_MV_PER_CYCLE) { cmd -= SETPOINT_SLEW_DOWN_MV_PER_CYCLE; }
        else                                                  { cmd = tgt; }
        regulator_commanded_mv = cmd;
        if (regulator_ovp_ref_mv < cmd ||
            (cmd == tgt && v_out_mv * 100u <= cmd * (100u + OVP_HOLD_RELEASE_PCT)))
        {
            regulator_ovp_ref_mv = cmd;
        }
    }
    const uint32_t commanded_mv = regulator_commanded_mv;

    /* --- 3b. Mode selection (§5.1), every cycle: where the slewed setpoint
     * is headed, gated by where V_out is (determine_mode_from_voltages).
     * The loop's current command carries over unchanged (step 5).       */
    {
        RegulatorMode new_mode = determine_mode_from_voltages(regulator_vin_filt_mv,
                                                              commanded_mv, v_out_mv);
        if (new_mode != regulator_mode)
        {
            change_mode_while_running(new_mode, v_out_mv, commanded_mv);
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

    /* --- 4c. Connect the output (see OUTPUT_CONNECT_MARGIN_MV) ---
     * After soft-start, with V_out near the target.                       */
#if OUTPUT_SWITCH_ENABLED
    if (!output_switch_on && !softstart_active &&
        v_out_mv + OUTPUT_CONNECT_MARGIN_MV >= commanded_mv)
    {
        OUTPUT_EN_WRITE(GPIO_PIN_SET);
        output_switch_on = true;
    }
#endif

    /* --- 5. Voltage loop: average output-current command ---
     * The PID's output is the average current into the output node, I_cmd
     * [mA] (2026-10-12).  Step 6 turns it into this mode's DCM peak current
     * and on-time, so the plant the loop sees is C dV/dt = I_cmd - I_load
     * in every mode and at every V_in: a mode change, or V_in moving during
     * a ramp, leaves the command where it was.  Its ceiling is what the mode
     * can deliver at this operating point, so the integrator cannot wind
     * past it.
     *
     * Inductor charging and discharging voltages per mode (both legs DCM,
     * see HRTIM_BOOST_SWITCHING_OUTPUTS; Vd a body-diode drop):
     *   buck        charge V_in - V_out, discharge V_out + Vd (Q2)
     *   boost       charge V_in,         discharge V_out - V_in + Vd (Q3)
     *   buck-boost  charge V_in,         discharge V_out + 2 Vd (Q2, Q3)
     * A pulse to peak current I delivers Q = L I^2 k / 2 to the output, with
     * k = 1/charge + 1/discharge in buck (the output takes the current while
     * L charges and while it discharges) and k = 1/discharge in boost and
     * buck-boost (only while it discharges).                               */
    /* With the rectifier on (regulator_sr_enable) the freewheel path has no
     * diode drop for most of the discharge: the model leaves it out. */
    if (!regulator_sr_enable)                      { regulator_sr_blend = 0u; }
    else if (regulator_sr_blend < SR_BLEND_FULL)   { regulator_sr_blend++; }
    const uint32_t blend = regulator_sr_blend;
    const uint32_t vd_mv = (DCM_DIODE_DROP_MV * (SR_BLEND_FULL - blend)) / SR_BLEND_FULL;
    uint32_t chg_mv, dis_mv;
    if (regulator_mode == REGULATOR_MODE_BUCK)
    {
        chg_mv = (v_in_mv > v_out_mv + 1000u) ? (v_in_mv - v_out_mv) : 1000u;
        dis_mv = v_out_mv + vd_mv;
    }
    else if (regulator_mode == REGULATOR_MODE_BOOST)
    {
        chg_mv = v_in_mv;
        dis_mv = ((v_out_mv > v_in_mv) ? (v_out_mv - v_in_mv) : 0u) + vd_mv;
    }
    else
    {
        chg_mv = v_in_mv;
        dis_mv = v_out_mv + 2u * vd_mv;
    }
    if (chg_mv < 1000u) { chg_mv = 1000u; }
    if (dis_mv < 500u)  { dis_mv = 500u; }
    const float chg_v   = (float)chg_mv * 1e-3f;
    const float dis_v   = (float)dis_mv * 1e-3f;
    const float k_per_v = (regulator_mode == REGULATOR_MODE_BUCK) ? (1.0f / chg_v + 1.0f / dis_v)
                                                                  : (1.0f / dis_v);
    const float l_h     = (float)INDUCTOR_VALUE_NH * 1e-9f;
    const float t_sw_s  = 1.0f / (float)HRTIM_SWITCHING_FREQ_HZ;

    /* Longest pulse: the current must reach zero inside the window after
     * the pulse start, t_on + t_off <= window, so
     * t_on <= window * discharge / (charge + discharge); and at most
     * DCM_ON_TIME_CEIL_NS.  With the mode's peak limit that sets the ceiling. */
    const uint32_t window_ns = (uint32_t)(1000000000ull / HRTIM_SWITCHING_FREQ_HZ)
                               - (BOOTSTRAP_REFRESH_NS + DCM_REFRESH_DEADTIME_NS)
                               - DCM_WINDOW_MARGIN_NS;
    uint32_t t_lim_ns = (window_ns * dis_mv) / (chg_mv + dis_mv);   /* < 5000 * 70000 */
    if (t_lim_ns > DCM_ON_TIME_CEIL_NS) { t_lim_ns = DCM_ON_TIME_CEIL_NS; }
    const float peak_max_a = (float)((regulator_mode == REGULATOR_MODE_BUCK_BOOST) ? DCM_MAX_PEAK_MA_BB
                                     : (regulator_mode == REGULATOR_MODE_BOOST)    ? DCM_MAX_PEAK_MA_BOOST
                                                                                   : DCM_MAX_PEAK_MA) * 1e-3f;
    float i_pk_lim_a = (float)t_lim_ns * 1e-9f * chg_v / l_h;
    if (i_pk_lim_a > peak_max_a) { i_pk_lim_a = peak_max_a; }
    const float i_max_ma = 1e3f * 0.5f * l_h * i_pk_lim_a * i_pk_lim_a * k_per_v / t_sw_s;

    pid_config.output_min = 0.0f;
    pid_config.output_max = i_max_ma;
    const float i_cmd_ma = pid_update(&pid_state, &pid_config,
                                      (float)effective_setpoint_mv, (float)v_out_mv);

    /* --- 6. Modulator: I_cmd -> peak current -> on-time (Timer A/B CMP2) ---
     * The charge this period is Q = I_cmd T_sw, so I_pk = sqrt(2 Q / (L k))
     * and t_on = I_pk L / charge.  Below DCM_MIN_ON_TIME_NS the minimum
     * pulse still runs and pulse skipping (step 7b) holds the output down.
     * Q1 (buck, buck-boost) is on from CMP4 to Timer A CMP2; Q4 (boost,
     * buck-boost) from CMP4 to Timer B CMP2, TB1's set, through the
     * dead-time generator.  In buck-boost both end at the same CMP2.      */
    const float i_pk_a = sqrtf(2.0f * (i_cmd_ma * 1e-3f) * t_sw_s / (l_h * k_per_v));
    uint32_t t_on_ns = (uint32_t)(1e9f * i_pk_a * l_h / chg_v);
    if (t_on_ns > t_lim_ns)           { t_on_ns = t_lim_ns; }
    if (t_on_ns < DCM_MIN_ON_TIME_NS) { t_on_ns = DCM_MIN_ON_TIME_NS; }
    regulator_on_time_ns = (uint16_t)t_on_ns;
    regulator_i_cmd_ma   = i_cmd_ma;
    regulator_i_pk_ma    = 1e3f * i_pk_a;
    regulator_i_max_ma   = i_max_ma;
    const uint32_t cmp2 = DCM_PULSE_START_TICKS + HRTIM_NS_TO_TICKS_RT(t_on_ns);

    /* --- 6b. Rectifier window (regulator_config.h, SR_*) ---
     * Predicted discharge t_dis = t_on * charge / discharge voltage, the
     * rectifier's discharge voltages (no diode drop).  On one dead time
     * after the pulse ends, off at SR_END_FRACTION_PCT of t_dis less
     * SR_END_MARGIN_NS (the body diode takes the rest), and two dead times
     * before the period ends at the latest.  Shorter than SR_MIN_WINDOW_NS:
     * not switched. */
    uint32_t sr_on_ns = 0u, sr_off_ns = 0u;
    if (regulator_sr_enable)
    {
        const uint32_t sr_dis_mv = (regulator_mode == REGULATOR_MODE_BOOST)
                                       ? ((v_out_mv > v_in_mv) ? (v_out_mv - v_in_mv) : 0u)
                                       : v_out_mv;
        if (sr_dis_mv >= 500u)
        {
            const uint32_t start_ns = BOOTSTRAP_REFRESH_NS + DCM_REFRESH_DEADTIME_NS;
            const uint32_t end_ns   = start_ns + t_on_ns;
            const uint32_t t_dis_ns = (uint32_t)((float)t_on_ns * (float)chg_mv / (float)sr_dis_mv);
            uint32_t off_ns = end_ns + (uint32_t)(((uint64_t)t_dis_ns * SR_END_FRACTION_PCT * blend) /
                                                  (100u * SR_BLEND_FULL));
            off_ns = (off_ns > end_ns + SR_END_MARGIN_NS) ? (off_ns - SR_END_MARGIN_NS) : 0u;
            const uint32_t latest_ns = (uint32_t)(1000000000ull / HRTIM_SWITCHING_FREQ_HZ) - 2u * SR_DEADTIME_NS;
            if (off_ns > latest_ns) { off_ns = latest_ns; }
            const uint32_t on_ns = end_ns + SR_DEADTIME_NS;
            if (off_ns >= on_ns + SR_MIN_WINDOW_NS)
            {
                sr_on_ns  = on_ns;
                sr_off_ns = off_ns;
            }
        }
    }
    const uint32_t sr_on  = sr_off_ns ? HRTIM_NS_TO_TICKS_RT(sr_on_ns)  : HRTIM_CMP_PARKED;
    const uint32_t sr_off = sr_off_ns ? HRTIM_NS_TO_TICKS_RT(sr_off_ns) : HRTIM_CMP_PARKED;
    regulator_sr_on_ns  = (uint16_t)sr_on_ns;
    regulator_sr_off_ns = (uint16_t)sr_off_ns;

    if (regulator_mode == REGULATOR_MODE_BUCK && (regulator_sr_enable != 0u) != buck_phase2_on)
    {
        buck_phase2_on = (regulator_sr_enable != 0u);
        if (buck_phase2_on) { HRTIM1->sCommonRegs.OENR  = HRTIM_OUTPUT_TB2 | (regulator_pulses_skipped ? 0u : HRTIM_OUTPUT_TB1); }
        else                { HRTIM1->sCommonRegs.ODISR = HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2; }
    }

    /* One consistent set of edges per period: hold the preload transfers of
     * Timers A-D while writing, release together. */
    HRTIM1->sCommonRegs.CR1 |= HRTIM_TIMER_ABCD_UDIS;
    if (regulator_mode != REGULATOR_MODE_BOOST)
    {
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP2xR = cmp2;
        /* Q2 rectifier (buck, buck-boost) */
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C].CMP2xR = sr_on;
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C].CMP3xR = sr_off;
    }
    else
    {
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C].CMP2xR = HRTIM_CMP_PARKED;
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C].CMP3xR = HRTIM_CMP_PARKED;
    }
    if (regulator_mode != REGULATOR_MODE_BUCK)
    {
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR = cmp2;
        /* Q3 rectifier (boost, buck-boost) */
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D].CMP3xR = sr_on;
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D].CMP4xR = sr_off;
    }
    else
    {
        /* Buck: Q3 carries the current from the pulse start to the
         * rectifier's end; no window, Q3 stays off (body diode). */
        /* Its start fades with the blend from the rectifier's start back
         * to the pulse start (Q3's drop also leaves the charge path). */
        const uint32_t q3_on = sr_off_ns
            ? sr_on - (uint32_t)(((uint64_t)(sr_on - DCM_PULSE_START_TICKS) * blend) / SR_BLEND_FULL)
            : HRTIM_CMP_PARKED;
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D].CMP3xR = q3_on;
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D].CMP4xR = sr_off;
    }
    HRTIM1->sCommonRegs.CR1 &= ~HRTIM_TIMER_ABCD_UDIS;

    /* The peak-current comparator (COMP1 -> EEV4) is a protection backstop
     * only, PEAK_PROTECT_PCT above the mode's peak limit: the on-time ends
     * every pulse.  With the threshold at the command (until 2026-10-12)
     * it cut Q1 at the end of its 100 ns blanking while Q4 ran to CMP2, and
     * buck-boost pulses came out at about 100 ns.                         */
    const uint32_t dac_counts = (regulator_mode == REGULATOR_MODE_BUCK_BOOST) ? PEAK_PROTECT_COUNTS_BB
                              : (regulator_mode == REGULATOR_MODE_BOOST)      ? PEAK_PROTECT_COUNTS_BOOST
                                                                              : PEAK_PROTECT_COUNTS;
    slope_comp_set_peak(dac_counts);
#if SLOPE_COMP_ENABLED
    slope_comp_update_step(v_out_mv, v_in_mv, (SlopeCompMode)regulator_mode);
#else
    /* No ramp: the DAC holds the threshold until the next PID cycle, so it
     * is written here rather than in the 200 kHz period ISR.              */
    slope_comp_reload_dac_peak();
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
    {
        uint32_t above_mv = (effective_setpoint_mv * regulator_skip_above_permille) / 1000u;
        if (above_mv < PULSE_SKIP_ABOVE_MV) { above_mv = PULSE_SKIP_ABOVE_MV; }
        uint32_t on = ((effective_setpoint_mv + above_mv)
                       * ADC_FULL_SCALE_COUNTS) / VD_MON_FULL_SCALE_MV;   /* < 70000 * 4096 */
        if (on > 0xFFEu) { on = 0xFFEu; }
        uint32_t off = (on > SKIP_HYSTERESIS_RAW) ? (on - SKIP_HYSTERESIS_RAW) : 0u;
        __disable_irq();   /* the watchdog ISR writes the window too */
        regulator_skip_raw_threshold = (uint16_t)on;
        regulator_skip_off_raw       = (uint16_t)off;
        if (regulator_pulses_skipped) { adc_monitor_awd_window(off, 0xFFFu); }
        else                          { adc_monitor_awd_window(0u, on); }
        __enable_irq();
    }   /* < 70000 * 4096 */

    /* --- 8. Software safety checks (§10.2) ---
     * Checked against the slewed target (step 4a), not the soft-start ramp: on the
     * first PID cycle the ramp is one increment (200 mV at 20 V), so a
     * residual V_out of a few hundred mV tripped the relative OVP before
     * any switching happened (bench, 2026-09-27: SW_OVP with V_out 410 mV).
     * UVP is already suppressed while softstart_active.  The relative OVP
     * uses regulator_ovp_ref_mv (step 4a).                                  */
    if (!software_safety_checks_pass(v_out_mv, v_in_mv, commanded_mv,
                                      regulator_ovp_ref_mv, regulator_mode))
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
        s.v_in_mv       = adc_measurements.v_in_mv;   /* newest ADC2 result, not the median */
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

    /* Dead time between TB1 (Q3) and TB2 (Q4): PHASE2_DEADTIME_NS on both
     * edges, prescaler unchanged (tDTG = tHRTIM / 8).  Used by bench PWM
     * only since 2026-10-15: the dead-time generator makes TB2 the
     * complement of TB1, and the rectifier must end mid-period, so the
     * regulator programs TB2 (Q4) from its own sources and keeps the dead
     * times as compare spacing (SR_DEADTIME_NS, DCM_REFRESH_DEADTIME_NS). */
    {
        uint32_t dt = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].DTxR;
        dt &= ~(HRTIM_DTR_DTR_Msk | HRTIM_DTR_DTF_Msk);
        dt |= (PHASE2_DEADTIME_TICKS << HRTIM_DTR_DTR_Pos) | (PHASE2_DEADTIME_TICKS << HRTIM_DTR_DTF_Pos);
        HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].DTxR = dt;
    }
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].OUTxR &= ~HRTIM_OUTR_DTEN;

    /* Timers C and D: counters for the rectifier edges (see "Predictive
     * synchronous rectification"), Timer A's prescaler and period,
     * continuous, compares parked. */
    {
        const uint32_t psc = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].TIMxCR & HRTIM_TIMCR_CK_PSC;
        const uint32_t idx[2] = { HRTIM_TIMERINDEX_TIMER_C, HRTIM_TIMERINDEX_TIMER_D };
        for (uint32_t k = 0u; k < 2u; k++)
        {
            HRTIM_Timerx_TypeDef *t = &HRTIM1->sTimerxRegs[idx[k]];
            t->TIMxCR = psc | HRTIM_TIMCR_CONT;
            t->PERxR  = HRTIM_PERIOD_COUNTS;
            t->REPxR  = 0u;
        }
    }
    hrtim_park_sr();

    /* Preload on A-D, transferred at each period roll-over (TRSTU). */
    {
        const uint32_t idx[4] = { HRTIM_TIMERINDEX_TIMER_A, HRTIM_TIMERINDEX_TIMER_B,
                                  HRTIM_TIMERINDEX_TIMER_C, HRTIM_TIMERINDEX_TIMER_D };
        for (uint32_t k = 0u; k < 4u; k++)
        {
            HRTIM1->sTimerxRegs[idx[k]].TIMxCR |= HRTIM_TIMCR_PREEN | HRTIM_TIMCR_TRSTU;
        }
    }
    hrtim_update_now();
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
    /* No Timer A period interrupt since 2026-09-28 evening: pulse skipping
     * is the ADC1 analog watchdog (top of file), at the same priority.     */
    HAL_NVIC_SetPriority(ADC1_2_IRQn, NVIC_PRIORITY_HRTIM, 0u);
    HAL_NVIC_EnableIRQ(ADC1_2_IRQn);

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
/**
 * hrtim_configure_adc_trigger — Master timer at the switching period with
 * MCMP1 at ADC_TRIGGER_TICKS, routed to HRTIM ADC trigger 1 (ADC1's regular
 * trigger in final.ioc).  Started with Timers A and B in one write
 * (hrtim_start_timers), so the trigger keeps a fixed phase in their period.
 * final.ioc keeps ADC trigger 1 on the master period (CubeMX generation
 * hangs with MASTER_CMP1 while the master compare unit is not configured
 * there, 2026-09-28); ADC1R is set to master compare 1 here.  Its
 * postscaler (HRTIM1.postscaler1) must be 0: at 0x0F, from the original
 * design, only one trigger in 16 reached the ADC (12.5 kHz, found by
 * logging the DMA advance per PID period).
 */
static void hrtim_configure_adc_trigger(void)
{
    HRTIM1->sMasterRegs.MPER   = HRTIM_PERIOD_COUNTS;
    HRTIM1->sMasterRegs.MCMP1R = ADC_TRIGGER_TICKS;
    HRTIM1->sMasterRegs.MCR    = (HRTIM1->sMasterRegs.MCR & ~HRTIM_MCR_CK_PSC) |
                                 (HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].TIMxCR & HRTIM_TIMCR_CK_PSC) |
                                 HRTIM_MCR_CONT;
    HRTIM1->sCommonRegs.ADC1R  = HRTIM_ADC1R_AD1MC1;
}

static void hrtim_start_timers(void)
{
    hrtim_configure_adc_trigger();
    /* Start Timer A and Timer B counters.
     * HAL_HRTIM_WaveformCounterStart_IT enables the timer with interrupts. */
    HAL_HRTIM_WaveformCounterStart_IT(&hhrtim1,
                                       HRTIM_TIMERID_MASTER |
                                       HRTIM_TIMERID_TIMER_A | HRTIM_TIMERID_TIMER_B |
                                       HRTIM_TIMERID_TIMER_C | HRTIM_TIMERID_TIMER_D);
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
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP1xR =
        DCM_PULSE_START_TICKS + HRTIM_BLANKING_TICKS_BUCK;

    /* TA2 (Q2): bootstrap refresh from the period start to CMP3, and the
     * rectifier window (Timer C CMP2 on, CMP3 off). */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx2R = HRTIM_SET2R_PER | HRTIM_SET2R_TIMEVNT3;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx2R = HRTIM_RST2R_CMP3 | HRTIM_RST2R_TIMEVNT4;

    /* Timer B (output side), enabled in buck only with the rectifier
     * (mode_switching_outputs): Q4 (TB2) gives Q3's bootstrap refresh from
     * the period start to CMP3; Q3 (TB1) carries the inductor current from
     * the pulse start (Timer D CMP3) to the rectifier's end (Timer D CMP4),
     * and is off by CMP2 = one dead time before the next refresh in any
     * case.  Off outside that window, so the DCM ring cannot push charge
     * back through it (2026-10-12, Q3 held on: 163 against 47 mA). */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR = HRTIM_PERIOD_COUNTS - DCM_REFRESH_DEADTIME_TICKS;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R = HRTIM_SET1R_TIMEVNT5;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = HRTIM_RST1R_TIMEVNT6 | HRTIM_RST1R_CMP2;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx2R = HRTIM_SET2R_PER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx2R = HRTIM_RST2R_CMP3;
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

    /* TA2 (Q2): the refresh only; no rectifier on the input leg in boost. */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].SETx2R = HRTIM_SET2R_PER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].RSTx2R = HRTIM_RST2R_CMP3;

    /* Timer B: Q4 (TB2) charges L1 from CMP4 to CMP2 (or the peak
     * comparator); Q3 (TB1) rectifies from Timer D CMP3 to CMP4 and is off
     * at the period start in any case, a pulse start (CMP4) before Q4. */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx2R = HRTIM_SET2R_CMP4;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx2R = HRTIM_RST2R_CMP2 | HRTIM_RST2R_EXTVNT4;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R = HRTIM_SET1R_TIMEVNT5;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = HRTIM_RST1R_TIMEVNT6 | HRTIM_RST1R_PER;
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
    /* Q4 drops SW2 to ground at the same edge Q1 starts, and the IL_MON
     * amplifier rings on that step: Q1's comparator gets the boost blanking
     * here (hrtim_apply_buck_mode_static_leg restores the buck one).        */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].CMP1xR =
        DCM_PULSE_START_TICKS + HRTIM_BLANKING_TICKS_BOOST;
#if !SYNC_RECT_ENABLED
    /* Q4 (TB2) pulses with Q1 (CMP4 to CMP2, or the peak comparator); Q3
     * (TB1) rectifies with Q2 (Timer D CMP3 to CMP4, off at the period
     * start in any case). */
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx2R = HRTIM_SET2R_CMP4;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx2R = HRTIM_RST2R_CMP2 | HRTIM_RST2R_EXTVNT4;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].SETx1R = HRTIM_SET1R_TIMEVNT5;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].RSTx1R = HRTIM_RST1R_TIMEVNT6 | HRTIM_RST1R_PER;
    HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B].CMP2xR =
        DCM_PULSE_START_TICKS + MAX_ON_TIME_COUNTS;
#endif
}

/**
 * mode_switching_outputs — HRTIM outputs enabled in a mode.
 *   buck:        TA1 TA2 (pulse, Q2 refresh); phase 2 off, Q3 conducts in
 *                reverse.  2026-10-12, 20 V no load: holding Q3 on let the
 *                DCM ring push output charge back into V_in through Q1's
 *                body diode, and its bootstrap refresh rang SW2 by +-22 V;
 *                the loop needed 163 mA of command against 47 mA with phase
 *                2 off.  Synchronous rectification by predicted conduction
 *                time is the planned replacement.
 *   boost:       HRTIM_BOOST_SWITCHING_OUTPUTS, TA1 TA2 (Q1 static, refresh)
 *   buck-boost:  TA1 TA2 (pulse, refresh), TB2 (pulse)
 */
static uint32_t mode_switching_outputs(RegulatorMode mode)
{
    /* Q3 (TB1) is enabled in every mode: it is only ever set by a
     * rectifier window (Timer D), which is parked while
     * regulator_sr_enable is 0.  Buck adds phase 2 (Q3 window, Q4 refresh)
     * only with the rectifier on (the PID ISR follows changes in buck). */
    if (mode == REGULATOR_MODE_BUCK)
    {
        return HRTIM_BUCK_SWITCHING_OUTPUTS |
               (regulator_sr_enable ? (HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2) : 0u);
    }
    if (mode == REGULATOR_MODE_BUCK_BOOST)
    {
        return HRTIM_BUCK_SWITCHING_OUTPUTS | HRTIM_OUTPUT_TB2 | HRTIM_OUTPUT_TB1;
    }
    return HRTIM_BOOST_SWITCHING_OUTPUTS | HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2;
}

/**
 * change_mode_while_running — Reconfigure the HRTIM legs for new_mode (§5.3).
 * Called from the PID ISR (step 3b).  v_out_mv and voltage_mv go to the
 * mode log only: the setpoint ramp and the loop state carry over.
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
    hrtim_park_sr();      /* the next PID cycle programs the new mode's window */
    hrtim_update_now();
    buck_phase2_on = (regulator_mode == REGULATOR_MODE_BUCK) && regulator_sr_enable;
    HAL_HRTIM_WaveformOutputStart(&hhrtim1, mode_switching_outputs(regulator_mode));
    skip_state_reset_pulsing();

    /* Nothing else changes: the setpoint ramp lives above the switching
     * mode (step 4a) and the loop commands output current in every mode
     * (step 5), so the transfer is bumpless.  Until 2026-10-12 this
     * restarted soft-start from V_out and zeroed the integrator; in the
     * middle of an 80 V/s ramp that left the loop asking for almost nothing
     * (buck-boost at 23 V stalled and drooped, bench 2026-10-11/12).      */
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
    OUTPUT_EN_WRITE(GPIO_PIN_RESET);
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
    OUTPUT_EN_WRITE(GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_OUTPUT_DIS_PORT,PIN_OUTPUT_DIS_PIN,GPIO_PIN_SET); // discharge VBUS
}

/**
 * determine_mode_from_voltages — Pick the switching mode (§5.1).
 *
 * Buck below V_in - BB_BELOW_VIN_MV, boost above V_in + BB_ABOVE_VIN_MV,
 * buck-boost between, each edge with BB_HYSTERESIS_MV against the mode the
 * regulator is in.  The setpoint says where the output is headed; leaving
 * buck-boost also needs V_out to be there, because buck and boost only have
 * authority on their own side of V_in (2026-10-11: boost entered with V_out
 * 2.6 V under V_in pinned the output at V_in less a diode drop, and its DCM
 * on-time bound shrinks toward zero as V_out approaches V_in).  So buck and
 * boost never meet directly: buck-boost works at any ratio and carries the
 * output across V_in, whichever way the setpoint or V_in moves.
 *
 * @param v_in_mv       Filtered input voltage (regulator_vin_filt_mv).
 * @param v_setpoint_mv Slewed setpoint (regulator_commanded_mv).
 * @param v_out_mv      V_out estimate.
 */
static RegulatorMode determine_mode_from_voltages(uint32_t v_in_mv,
                                                   uint32_t v_setpoint_mv,
                                                   uint32_t v_out_mv)
{
    const uint32_t lo = (v_in_mv > BB_BELOW_VIN_MV) ? (v_in_mv - BB_BELOW_VIN_MV) : 0u;
    const uint32_t hi = v_in_mv + BB_ABOVE_VIN_MV;
    const uint32_t h  = BB_HYSTERESIS_MV;
    switch (regulator_mode)
    {
        case REGULATOR_MODE_BUCK:
            return (v_setpoint_mv > lo + h) ? REGULATOR_MODE_BUCK_BOOST : REGULATOR_MODE_BUCK;
        case REGULATOR_MODE_BOOST:
            /* Also on a sag: boost below the band cannot pull V_out back up
             * (its discharge voltage V_out - V_in + Vd goes to a diode drop,
             * the window caps the pulse; 2026-10-15, 28 V into 25 ohm sat at
             * 22 V in boost). */
            return (v_setpoint_mv + h < hi || v_out_mv + REGULATOR_BOOST_FALLBACK_MV < hi)
                       ? REGULATOR_MODE_BUCK_BOOST : REGULATOR_MODE_BOOST;
        default:
            if (v_setpoint_mv > hi + h && v_out_mv > hi) { return REGULATOR_MODE_BOOST; }
            if (v_setpoint_mv + h < lo && v_out_mv < lo) { return REGULATOR_MODE_BUCK; }
            return REGULATOR_MODE_BUCK_BOOST;
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

static void record_fault_snapshot(uint32_t v_out_mv, uint32_t v_in_mv,
                                  uint32_t v_setpoint_mv)
{
    regulator_fault_snapshot[0] = v_out_mv;
    regulator_fault_snapshot[1] = v_in_mv;
    uint32_t scan = adc_monitor_latest_scan();
    regulator_fault_snapshot[2] = adc_monitor_vd_at(scan);
    regulator_fault_snapshot[3] = regulator_vout_est_raw;
    regulator_fault_snapshot[4] = v_setpoint_mv;
    regulator_fault_snapshot[5] = regulator_skip_events;
    for (uint32_t k = 0u; k < ADC1_DMA_SCANS; k++)   /* oldest first */
    {
        scan = (scan + 1u) % ADC1_DMA_SCANS;
        regulator_fault_trace[k] = adc_monitor_vd_at(scan);
    }
}

/**
 * software_safety_checks_pass — Check all software safety conditions (§10.2).
 *
 * v_setpoint_mv is the slewed setpoint (UVP, V_in range); ovp_ref_mv is what
 * the relative OVP is measured against (regulator_ovp_ref_mv, PID ISR step
 * 4a).  Returns true if all conditions are within bounds.  Returns false and
 * calls enter_fault() if any condition is violated.
 */
static bool software_safety_checks_pass(uint32_t v_out_mv,
                                         uint32_t v_in_mv,
                                         uint32_t v_setpoint_mv,
                                         uint32_t ovp_ref_mv,
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
        uint32_t ovp_threshold = (ovp_ref_mv * OVP_RELATIVE_PCT) / 100u;
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
     * cycles in a row.  Run 36 (2026-09-28, boost 28 V) faulted after 32 s
     * on one V_in sample of 27.8 V while every logged cycle read 23.0 to
     * 24.0 V and the scope's CH1 read 24.1 V; V_in has been the median of
     * three oversampled conversions since 2026-10-12 (step 1).  The supply
     * does not move in 1 ms; the ADM1270 and the FLT inputs cover the fast
     * cases.                                                             */
    bool vin_out_of_range;
    if (mode == REGULATOR_MODE_BUCK)
    {
        /* Buck requires V_in > V_out + margin. */
        vin_out_of_range = (v_in_mv < (v_setpoint_mv + BUCK_VIN_MARGIN_MV));
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
