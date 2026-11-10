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
    float ki;        /**< integral gain [1/s]                         */
    float kp;        /**< proportional gain [V/V]                     */
    float kd;        /**< derivative gain on V_out [s] (damping)      */
    float u_min_v;   /**< integrator floor [V]                        */
    float u_max_v;   /**< integrator ceiling [V]                      */
    float dt_s;      /**< buck_ccm_update period [s]                  */
    float ilim_a;    /**< current limit [A]; <= 0: off                */
    float kl;        /**< limit integral gain [V/(A s)]               */
    float dt_lim_s;  /**< buck_ccm_limit period [s]                   */
    float kff_l;     /**< load-current feedforward: kff_l dI_out/dt [H] (L1: 4.7e-6) */
    float kff_r;     /**< and kff_r I_out [ohm] (the series drop)     */
} BuckCcmGains;

typedef struct
{
    float u_v;       /**< integrator: the loss voltage the feedforward misses [V] */
    int8_t clamp;    /**< last update: +1 at the longest pulse, -1 at the shortest */
    uint8_t limiting;/**< last update: the current limit set the duty           */
    float v_lim_v;   /**< the limit's voltage reference [V] (valid while limiting) */
    float v_cmd_v;   /**< reference the duty is built on: min(V_ref, v_lim) [V]    */
    float v_prev_v;  /**< last V_out buck_ccm_update saw (derivative) [V]          */
    float i_out_a;   /**< output current for the feedforward, set by the caller [A] */
    float i_prev_a;  /**< its value at the last update [A]                          */
    uint32_t t_on_ticks; /**< last update's on-time in HRTIM ticks (184 ps): 1 ns
                              of duty is 9.6 mV at 24 V, 0.2 A into a battery */
    float v_floor_v; /**< no-reverse floor, set by the caller [V]; <= 0: off.  The
                          law's reference never goes below it while the current
                          limit is not acting, so with the integrator's offset
                          L1's average current settles at 0 A or above (a
                          battery on VBUS would otherwise drive power back into
                          V_in); while it acts, its positive target does.  It
                          must be the source's open-circuit voltage, not the
                          loaded V_out, or it holds L1 at its present current */
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

/* CCM handover (plans/ccm-handover.md, 2026-11-06).  The period starts in
 * Q2's freewheel with Q4's refresh, so in steady buck CCM L1 is at its
 * valley at the period boundary; DCM leaves it at 0 A there.  One period
 * whose on-time differs by dt moves the boundary current by V_in dt / L
 * (the rest of the period's volt-seconds are unchanged), so the entry's
 * first period takes dt = I_v L / V_in (0 -> I_v) and the exit's last one
 * dt = -I_v L / V_in (I_v -> 0). */

/** L1's current at the period boundary for a mean of i_load_a, from the
 *  programmed edges: flat while Q4 refreshes (to b_cmp3, averaged over the
 *  refresh interval), down at -(v_out + v_fw) until Q1 turns on (a_cmp4),
 *  up during the on-time, down again to the period end.  v_fw is the
 *  freewheel path's drop (Q2 and Q3).  The rise is taken as the sum of the
 *  two falls (steady state), so only the fall voltage and the edge times
 *  enter, not the losses that lengthen the on-time.  Negative at light
 *  load [A]. */
float buck_ccm_boundary_a(const BuckCcmTimingCfg *c, const BuckCcmTiming *t, float i_load_a, float v_out_v,
                          float v_fw_v, float l_h);
/** The one-period on-time change [HRTIM ticks, signed] that moves L1's
 *  boundary current by di_a: di L / v_in. */
int32_t buck_ccm_handover_ticks(float di_a, float v_in_v, float l_h);

/** Integrator value for a bumpless entry from DCM at load current i_load_a:
 *  the series-resistance drop plus the dead-time drop (body diodes at vsd_v). */
float buck_ccm_u_initial(const BuckCcmTimingCfg *c, float i_load_a, float r_series_ohm, float vsd_v);

/** The current limit (buck_ccm.h top), once per dt_lim_s: sets s->v_cmd_v
 *  to min(V_ref, v_lim) and s->limiting.  With ilim_a <= 0, v_cmd = V_ref. */
void buck_ccm_limit(BuckCcmState *s, const BuckCcmGains *g, float v_ref_v, float v_out_v, float i_a);

/** One update of the voltage law, every dt_s, on s->v_cmd_v: returns the Q1
 *  on-time [ns] and sets s->t_on_ticks, clamped; at a clamp the integrator
 *  tracks the value that reaches it (no wind-up), and it holds while the
 *  limit is active. */
uint32_t buck_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                         float v_out_v, float v_in_v);
/* With kff_l or kff_r set, the caller puts the output current in s->i_out_a
 * before each update: the duty adds kff_l dI_out/dt + kff_r I_out, so the
 * inductor is driven toward a load step as the step is measured, before
 * V_out has moved (feedforward: it does not enter the loop gain). */

/** Reset the law's state for an entry: integrator u0, no limit, v_cmd = V_ref. */
void buck_ccm_reset(BuckCcmState *s, float u0_v, float v_ref_v, float v_out_v);

/** Inductor ripple p-p [A] for a Q1 on-time: (V_in - V_out) t_on / L. */
float buck_ccm_ripple_a(float v_in_v, float v_out_v, uint32_t t_on_ns, float l_h);

/** Average inductor current from the average of a sensor that clips at 0
 *  (IL_MON's INA281 is unidirectional): a triangle of p-p ripple_a sampled
 *  at evenly spread phases is uniform over [I - ripple/2, I + ripple/2], so
 *  the clipped mean is m = (I + ripple/2)^2 / (2 ripple) while the valley is
 *  below 0, and m = I once it is not.  Returns I (down to -ripple/2). */
float buck_ccm_unclip_a(float m_a, float ripple_a);

/* ---------------------------------------------------------------------------
 * Boost forced CCM (plans/regulation-strategy.md 3.5), 200 kHz: the refresh
 * interval is the switching period.  Legs, all edges in one Timer A/B period:
 *   Q4 (TB2)  on at the period start (Timer B period), off at Timer B CMP2
 *             (the on-time, at least pulse_start_ns)
 *   Q3 (TB1)  the rectifier: on one dead time after Q4 (Timer B CMP3), off
 *             one dead time before the period ends (Timer B CMP4)
 *   Q1 (TA1)  held on: off at the period start (Timer A period), on again at
 *             Timer A CMP4 = refresh + 2 dead times
 *   Q2 (TA2)  Q1's bootstrap refresh: Timer A CMP2 (one dead time) to CMP3,
 *             inside Q4's on-time, so L1 sees 0 V for refresh_ns
 * Duty law, once per PID cycle (volt-second balance:
 * V_in (T - t_refresh) = V_out (T - t_on); u carries the losses):
 *   t_on = T (1 - V_in (1 - r) / (V_ref + kp e + u)),   u += ki e dt
 * The plant from u to V_out is unity at DC, as in buck; the L-C pole moves to
 * (1 - D) / (2 pi sqrt(L C)) and a RHP zero sits at (1 - D)^2 R / (2 pi L)
 * (plans/scripts/ccm_bb.py: 34 kHz or more at 10-28 V, ki 3142 keeps 77 deg).
 * The current limit (buck_ccm_limit) is shared: it lowers v_cmd, which in
 * boost cannot go below V_in (1 - r) / (1 - D_min).
 * -------------------------------------------------------------------------*/
typedef struct
{
    uint32_t a_cmp2;   /**< Q2 on (Q1 went off at the period start) */
    uint32_t a_cmp3;   /**< Q2 off                                  */
    uint32_t a_cmp4;   /**< Q1 on                                   */
    uint32_t b_cmp2;   /**< Q4 off                                  */
    uint32_t b_cmp3;   /**< Q3 on                                   */
    uint32_t b_cmp4;   /**< Q3 off                                  */
    uint32_t t_on_ns;  /**< Q4 on-time programmed                   */
} BoostCcmTiming;

/** Longest boost duty the law commands (V_out up to 1/(1-0.75) = 4 V_in). */
#define BOOST_CCM_MAX_DUTY 0.75f

/** Shortest Q4 pulse: the refresh plus both dead times plus a margin
 *  (cfg->pulse_start_ns, which ccm_timing_cfg builds as exactly that). */
uint32_t boost_ccm_min_on_ns(const BuckCcmTimingCfg *c);
/** Longest Q4 pulse: Q3 on for min_q2_end_ns, a dead time each side, and
 *  never over BOOST_CCM_MAX_DUTY. */
uint32_t boost_ccm_max_on_ns(const BuckCcmTimingCfg *c);
/** True when the refresh interval is the period, every edge keeps its dead
 *  time and every compare is valid (dead time >= 18 ns: CMP2 = one dead time). */
bool boost_ccm_cfg_valid(const BuckCcmTimingCfg *c);
/** Band CCM (V_out about V_in): the boost legs with Q1's off window
 *  (refresh_ns + 2 dt, Q2 on inside it) widened to CCM_BAND_Q2_NS and Q4's
 *  pulse allowed to end inside it (pulse_start_ns, the shortest Q4 pulse, is
 *  then shorter than the window).  Volt-second balance holds either way:
 *  V_in (T - t_window) = V_out (T - t_on), so the boost law and timing
 *  apply unchanged; the shortest pulse reaches V_in (1 - r) / (1 - D_min),
 *  below V_in.  As boost_ccm_cfg_valid, without the Q4-covers-the-window
 *  rule; the shortest Q4 pulse must still be min_on_ns. */
bool band_ccm_cfg_valid(const BuckCcmTimingCfg *c);
/** Register values for a Q4 on-time in ticks (clamped). */
void boost_ccm_timing_ticks(const BuckCcmTimingCfg *c, uint32_t t_on_ticks, BoostCcmTiming *out);
/** Lowest V_out the boost law reaches (at the shortest pulse) for V_in [V]. */
float boost_ccm_v_min(const BuckCcmTimingCfg *c, float v_in_v);
/** Highest, at the longest pulse. */
float boost_ccm_v_max(const BuckCcmTimingCfg *c, float v_in_v);
/** The boost duty law (above): returns Q4's on-time [ns], sets s->t_on_ticks
 *  and s->clamp; the same integrator, limit and back-calculation rules as
 *  buck_ccm_update. */
uint32_t boost_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                          float v_out_v, float v_in_v);

#ifdef __cplusplus
}
#endif

#endif /* BUCK_CCM_H */
