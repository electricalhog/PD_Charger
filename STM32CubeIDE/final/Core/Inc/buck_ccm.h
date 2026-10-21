/**
 * @file    buck_ccm.h
 * @brief   Buck forced-CCM modulator: duty law and HRTIM edge timing
 *          (plans/ccm-buck-50w.md).  No HAL: host-tested by
 *          tests/buck_ccm/test_buck_ccm.c.
 *
 * Legs in buck CCM (regulator.c programs the registers from BuckCcmTiming):
 *   Q1 (TA1)  on at pulse_start (Timer A CMP4), off at Timer A CMP2
 *   Q2 (TA2)  on one dead time after Q1 (Timer C CMP2 -> Timer A event 3),
 *             through the period boundary, off at Timer A CMP3, one dead
 *             time before Q1 turns on again
 *   Q3 (TB1)  held on: off one dead time before Timer B rolls over (CMP2),
 *             on again one dead time after the refresh (CMP4)
 *   Q4 (TB2)  Q3's bootstrap refresh: on at the Timer B roll-over, off at
 *             CMP3.  Timer B's period is refresh_interval_ns, a multiple of
 *             the switching period, so the refresh lands at the start of
 *             every Nth period, inside Q2's on-time: both low sides on and
 *             L1 sees 0 V for refresh_ns.
 *
 * Duty law (voltage mode with input feedforward), once per PID cycle:
 *   t_on = T (V_ref (1 - r) + kp e + u) / V_in,   u += ki e dt
 * with r = refresh_ns / refresh_interval_ns, the share of the period in
 * which L1 sees 0 V instead of -V_out (volt-second balance:
 * D V_in = V_out (1 - r) + losses; u carries the losses).
 *
 * Current limit (2026-10-21), when gains->ilim_a > 0: the duty is built on
 * min(V_ref, v_lim), where
 *   v_lim += kl (I_lim - I) dt   (entered at the measured V_out once I > I_lim)
 * and the voltage integrator u holds while v_lim is the lower (anti-windup:
 * V_out sits below V_ref by design).  At the PID rate the converter is a
 * voltage source, so the loop gain is kl / (the load's incremental
 * resistance): kl is set for the stiffest load, a battery behind the output
 * switch (about 45 mOhm), which makes a fold into a resistor slow (time
 * constant R / kl, 50 ms into 25 ohm) but never oscillatory.  A current
 * controller referenced to the measured V_out was tried first (host test,
 * 2026-10-21): the delay of the V_out mean turns it into the same voltage
 * source, and its second integrator rang with a 25 ohm load (zeta 0.07).
 */
#ifndef BUCK_CCM_H
#define BUCK_CCM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HRTIM ticks per microsecond: 170 MHz x 32 (regulator_config.h checks it). */
#define BUCK_CCM_TICKS_PER_US 5440u
/** Shortest non-zero compare and period value the HRTIM accepts at x32 (RM0440). */
#define BUCK_CCM_MIN_COMPARE  0x60u
/** Longest period / compare value (RM0440: 0xFFDF). */
#define BUCK_CCM_MAX_COMPARE  0xFFDFu

typedef struct
{
    uint32_t period_ns;            /**< switching period, Timers A, C, D */
    uint32_t pulse_start_ns;       /**< Q1 turn-on (Timer A CMP4)                 */
    uint32_t dt_ns;                /**< dead time on both legs, both edges        */
    uint32_t refresh_ns;           /**< Q4 refresh pulse                          */
    uint32_t refresh_interval_ns;  /**< Timer B period (multiple of period_ns)    */
    uint32_t min_on_ns;            /**< shortest Q1 pulse                         */
    uint32_t min_q2_end_ns;        /**< Q2 must be on this long before the period ends */
} BuckCcmTimingCfg;

typedef struct
{
    uint32_t a_cmp4;   /**< Q1 on (pulse start)       */
    uint32_t a_cmp2;   /**< Q1 off                    */
    uint32_t a_cmp3;   /**< Q2 off                    */
    uint32_t c_cmp2;   /**< Q2 on                     */
    uint32_t b_per;    /**< Timer B period: Q4 on     */
    uint32_t b_cmp2;   /**< Q3 off                    */
    uint32_t b_cmp3;   /**< Q4 off                    */
    uint32_t b_cmp4;   /**< Q3 on                     */
    uint32_t t_on_ns;  /**< Q1 on-time programmed     */
} BuckCcmTiming;

typedef struct
{
    float ki;        /**< integral gain [1/s]            */
    float kp;        /**< proportional gain [V/V]        */
    float u_min_v;   /**< integrator floor [V]           */
    float u_max_v;   /**< integrator ceiling [V]         */
    float dt_s;      /**< PID period [s]                 */
    float ilim_a;    /**< current limit [A]; <= 0: off   */
    float kl;        /**< limit integral gain [V/(A s)]  */
} BuckCcmGains;

typedef struct
{
    float u_v;       /**< integrator: the loss voltage the feedforward misses [V] */
    int8_t clamp;    /**< last update: +1 at the longest pulse, -1 at the shortest */
    uint8_t limiting;/**< last update: the current limit set the duty           */
    float v_lim_v;   /**< the limit's voltage reference [V] (valid while limiting) */
    uint32_t t_on_ticks; /**< last update's on-time in HRTIM ticks (184 ps): 1 ns
                              of duty is 9.6 mV at 24 V, 0.2 A into a battery */
} BuckCcmState;

static inline uint32_t buck_ccm_ticks(uint32_t ns)
{
    return (ns * BUCK_CCM_TICKS_PER_US) / 1000u;   /* exact below 789 us */
}

/** Longest Q1 pulse the timing allows: Q2 must turn on at least
 *  min_q2_end_ns before the period ends. */
uint32_t buck_ccm_max_on_ns(const BuckCcmTimingCfg *c);

/** True when every edge of the configuration keeps its dead time, the
 *  refresh fits inside Q2's on-time, and every register value is in range. */
bool buck_ccm_cfg_valid(const BuckCcmTimingCfg *c);

/** Register values for a Q1 on-time (clamped to [min_on, max_on]). */
void buck_ccm_timing(const BuckCcmTimingCfg *c, uint32_t t_on_ns, BuckCcmTiming *out);
/** The same for an on-time in HRTIM ticks (BuckCcmState.t_on_ticks). */
void buck_ccm_timing_ticks(const BuckCcmTimingCfg *c, uint32_t t_on_ticks, BuckCcmTiming *out);

/** Refresh share r: the fraction of time L1 sees 0 V during the refresh. */
float buck_ccm_refresh_share(const BuckCcmTimingCfg *c);

/** Integrator value for a bumpless entry from DCM at load current i_load_a:
 *  the series-resistance drop plus the dead-time drop (body diodes at vsd_v). */
float buck_ccm_u_initial(const BuckCcmTimingCfg *c, float i_load_a, float r_series_ohm, float vsd_v);

/** One PID cycle of the duty law; returns the Q1 on-time [ns], clamped,
 *  at a clamp the integrator tracks the value that reaches it (no wind-up).
 *  i_a is the measured inductor (or output) current for the limit. */
uint32_t buck_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                         float v_ref_v, float v_out_v, float v_in_v, float i_a);

/** Inductor ripple p-p [A] for a Q1 on-time: (V_in - V_out) t_on / L. */
float buck_ccm_ripple_a(float v_in_v, float v_out_v, uint32_t t_on_ns, float l_h);

/** Average inductor current from the average of a sensor that clips at 0
 *  (IL_MON's INA281 is unidirectional): a triangle of p-p ripple_a sampled
 *  at evenly spread phases is uniform over [I - ripple/2, I + ripple/2], so
 *  the clipped mean is m = (I + ripple/2)^2 / (2 ripple) while the valley is
 *  below 0, and m = I once it is not.  Returns I (down to -ripple/2). */
float buck_ccm_unclip_a(float m_a, float ripple_a);

#ifdef __cplusplus
}
#endif

#endif /* BUCK_CCM_H */
