/*
 * Host test for Core/Src/buck_ccm.c: plays the register program back as the
 * HRTIM would run it, one tick at a time, and checks every edge.
 *
 *   make -C STM32CubeIDE/final/tests/buck_ccm
 *
 * Event model (what regulator.c programs in buck CCM, buck_ccm.h):
 *   Q1 TA1  set Timer A CMP4 (pulse start), reset Timer A CMP2
 *   Q2 TA2  set Timer A period and Timer C CMP2 (event 3), reset Timer A CMP3
 *           (Timer C CMP3, event 4, parked)
 *   Q3 TB1  set Timer B CMP4, reset Timer B CMP2
 *   Q4 TB2  set Timer B period, reset Timer B CMP3
 * Timers A and C run at the switching period, Timer B at the refresh interval,
 * all reset together.  The comparator (EEV4) is not modelled: it only ends
 * Q1 early, which lengthens the Q1-to-Q2 gap.
 */
#include "buck_ccm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#ifndef LIM_KL
#define LIM_KL  400.0f
#endif

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            if (failures < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                 printf(__VA_ARGS__); printf("\n"); }       \
        }                                                                   \
    } while (0)

static BuckCcmTimingCfg cfg(uint32_t period_ns, uint32_t dt_ns)
{
    BuckCcmTimingCfg c = {
        .period_ns = period_ns, .pulse_start_ns = 250u, .dt_ns = dt_ns, .refresh_ns = 150u,
        .refresh_interval_ns = 10000u, .min_on_ns = 100u, .min_q2_end_ns = 60u,
    };
    return c;
}

/* Run two Timer B periods; check the second. */
static void play(const BuckCcmTimingCfg *c, const BuckCcmTiming *t)
{
    const uint32_t pa = buck_ccm_ticks(c->period_ns);
    const uint32_t a_cmp4 = t->a_cmp4;
    CHECK(a_cmp4 == buck_ccm_ticks(c->pulse_start_ns), "a_cmp4 %u", a_cmp4);
    const uint32_t dt = buck_ccm_ticks(c->dt_ns);
    CHECK(t->a_cmp2 < pa && t->c_cmp2 < pa, "compare beyond the period: a_cmp2 %u c_cmp2 %u period %u",
          t->a_cmp2, t->c_cmp2, pa);
    CHECK(t->b_per <= BUCK_CCM_MAX_COMPARE && t->b_per % pa == 0u, "Timer B period %u", t->b_per);
    int q1 = 0, q2 = 1, q3 = 1, q4 = 0;
    uint32_t off1 = 0, off2 = 0, off3 = 0, off4 = 0, on1 = 0, q1_ticks = 0;
    for (uint32_t n = 0; n < 2u * t->b_per; n++)
    {
        const uint32_t a = n % pa, b = n % t->b_per;
        const int p1 = q1, p2 = q2, p3 = q3, p4 = q4;
        if (a == a_cmp4)                   { q1 = 1; }
        if (a == t->a_cmp2)                { q1 = 0; }
        if (a == 0u || a == t->c_cmp2)     { q2 = 1; }
        if (a == t->a_cmp3)                { q2 = 0; }
        if (b == t->b_cmp4)                { q3 = 1; }
        if (b == t->b_cmp2)                { q3 = 0; }
        if (b == 0u)                       { q4 = 1; }
        if (b == t->b_cmp3)                { q4 = 0; }
        if (!q1) { off1++; } else { off1 = 0; }
        if (!q2) { off2++; } else { off2 = 0; }
        if (!q3) { off3++; } else { off3 = 0; }
        if (!q4) { off4++; } else { off4 = 0; }
        if (n < t->b_per) { continue; }          /* settle */
        CHECK(!(q1 && q2), "Q1 and Q2 on together at tick %u (t_on %u ns, dt %u ns)", n, t->t_on_ns, c->dt_ns);
        CHECK(!(q3 && q4), "Q3 and Q4 on together at tick %u", n);
        if (q1 && !p1) { CHECK(off2 >= dt, "Q2->Q1 gap %u < %u ticks", off2, dt); on1 = n; }
        if (q2 && !p2) { CHECK(off1 >= dt, "Q1->Q2 gap %u < %u ticks (t_on %u)", off1, dt, t->t_on_ns); }
        if (q3 && !p3) { CHECK(off4 >= dt, "Q4->Q3 gap %u < %u ticks", off4, dt); }
        if (q4 && !p4) { CHECK(off3 >= dt, "Q3->Q4 gap %u < %u ticks", off3, dt); }
        if (q4)        { CHECK(q2 && !q1, "refresh outside Q2's on-time at tick %u", n); }
        /* Efficiency, not safety: Q3 carries the charge current, else it
         * flows through Q3's 2 V reverse conduction. */
        if (q1)        { CHECK(q3, "Q1 on while Q3 is off at tick %u", n); }
        if (!q1 && p1) { q1_ticks = n - on1; CHECK(q1_ticks == t->a_cmp2 - a_cmp4, "Q1 width"); }
    }
    const uint32_t want = buck_ccm_ticks(c->pulse_start_ns + t->t_on_ns) - a_cmp4;
    CHECK(abs((int)q1_ticks - (int)want) <= 1, "Q1 on %u ticks, want %u", q1_ticks, want);
}

static void test_timing(void)
{
    const uint32_t periods[] = { 2500u, 5000u };
    for (unsigned k = 0; k < 2; k++)
    {
        for (uint32_t dt = 10u; dt <= 40u; dt += 10u)
        {
            BuckCcmTimingCfg c = cfg(periods[k], dt);
            CHECK(buck_ccm_cfg_valid(&c), "config invalid: period %u dt %u", c.period_ns, dt);
            for (uint32_t ton = 0u; ton <= c.period_ns + 500u; ton += 37u)
            {
                BuckCcmTiming t;
                buck_ccm_timing(&c, ton, &t);
                CHECK(t.t_on_ns <= buck_ccm_max_on_ns(&c) && t.t_on_ns >= c.min_on_ns, "clamp %u", t.t_on_ns);
                play(&c, &t);
            }
        }
    }
    /* Longer refresh with the pulse start moved as regulator.c does
     * (refresh + 2 dt + 20 ns), every period and every 10 us. */
    const uint32_t refresh[] = { 200u, 300u, 600u };
    for (unsigned k = 0; k < 2; k++)
    {
        for (unsigned r = 0; r < 3; r++)
        {
            for (uint32_t ivl = 0u; ivl < 2u; ivl++)
            {
                BuckCcmTimingCfg c = cfg(periods[k], 20u);
                c.refresh_ns = refresh[r];
                c.pulse_start_ns = refresh[r] + 2u * 20u + 20u;
                if (c.pulse_start_ns < 250u) { c.pulse_start_ns = 250u; }
                c.refresh_interval_ns = ivl ? 10000u : periods[k];
                CHECK(buck_ccm_cfg_valid(&c), "refresh %u ivl %u invalid", c.refresh_ns, c.refresh_interval_ns);
                for (uint32_t ton = 0u; ton <= c.period_ns; ton += 53u)
                {
                    BuckCcmTiming t;
                    buck_ccm_timing(&c, ton, &t);
                    play(&c, &t);
                }
            }
        }
    }

    /* Configurations the firmware must refuse. */
    BuckCcmTimingCfg bad = cfg(2500u, 60u);          /* refresh + 2 dt reaches the pulse start */
    CHECK(!buck_ccm_cfg_valid(&bad), "dt 60 ns accepted");
    bad = cfg(3000u, 20u);                           /* 10 us is not a multiple of 3 us */
    CHECK(!buck_ccm_cfg_valid(&bad), "non-multiple refresh interval accepted");
    bad = cfg(2500u, 20u); bad.refresh_interval_ns = 15000u;   /* Timer B period past 0xFFDF */
    CHECK(!buck_ccm_cfg_valid(&bad), "Timer B period over range accepted");
}

static void test_law(void)
{
    BuckCcmTimingCfg c = cfg(2500u, 20u);
    BuckCcmGains g = { .ki = 3142.0f, .kp = 0.0f, .u_min_v = -1.0f, .u_max_v = 2.0f, .dt_s = 50e-6f,
                       .dt_lim_s = 50e-6f };
    BuckCcmState s = { 0 };
    buck_ccm_reset(&s, 0.0f, 20.0f, 20.0f);
    /* Zero error, zero integrator: pure feedforward, D = V_ref (1 - r) / V_in. */
    uint32_t ton = buck_ccm_update(&s, &g, &c, 20.0f, 24.0f);
    const float want = 2500.0f * 20.0f * (1.0f - 150.0f / 10000.0f) / 24.0f;
    CHECK(fabsf((float)ton - want) <= 1.0f, "feedforward %u ns, want %.0f", ton, want);

    /* Averaged plant: V_out follows D V_in / (1 - r) less an IR drop, one
     * PID cycle late.  The integrator must take the drop out. */
    s.u_v = 0.0f;
    float v_out = 19.5f;
    for (int k = 0; k < 4000; k++)
    {
        buck_ccm_limit(&s, &g, 20.0f, v_out, 0.0f);
        ton = buck_ccm_update(&s, &g, &c, v_out, 24.0f);
        const float d = (float)ton / 2500.0f;
        v_out = d * 24.0f / (1.0f - 150.0f / 10000.0f) - 2.5f * 0.08f;
    }
    CHECK(fabsf(v_out - 20.0f) < 0.005f, "closed loop settled at %.4f V", v_out);
    CHECK(fabsf(s.u_v - 0.2f * (1.0f - 0.015f)) < 0.01f, "integrator %.3f V", s.u_v);

    /* Anti-windup: V_in too low for the setpoint pins t_on at the ceiling;
     * the integrator must not keep climbing. */
    s.u_v = 0.0f;
    for (int k = 0; k < 2000; k++) { buck_ccm_limit(&s, &g, 20.0f, 15.0f, 0.0f); (void)buck_ccm_update(&s, &g, &c, 15.0f, 18.0f); }
    CHECK(s.clamp == 1, "expected the high clamp");
    CHECK(s.u_v < 1.0f, "integrator wound up to %.2f V at the clamp", s.u_v);

    /* Initial integrator: 2.5 A x 40 mOhm + 2 x 2 V x 20 ns / 2.5 us. */
    const float u0 = buck_ccm_u_initial(&c, 2.5f, 0.040f, 2.0f);
    CHECK(fabsf(u0 - (0.1f + 0.032f)) < 1e-4f, "u0 %.4f", u0);
}


/* ---------------------------------------------------------------------------
 * Current limit on an averaged plant (switching-averaged, ripple not in it).
 * Board node: L1 (4.7 uH, 22 mOhm with R5 and the FETs) into five 10 uF X7R
 * derated to the bias; the output switch path (35 mOhm, 0.5 uH) to a bench
 * node with c_x of ceramics and a resistor and/or a battery (V_bat + R_bat).
 * The PID ISR as the firmware runs it: every 50 us, V_out the mean of the
 * last 45 us at the board node, I the mean inductor current over the
 * previous cycle's ADC2 IL_MON rank (13.9 to 23.3 us after that ISR began),
 * the new duty applied 13 us later.  plans/scripts/ccm_buck.py --steps
 * uses the same ISR timing.
 * ------------------------------------------------------------------------- */
static float c_mlcc(float v)   /* 10 uF 50 V X7R under DC bias (loop_model.py) */
{
    static const float bv[] = { 0, 2, 5, 10, 15, 20, 25, 28, 30, 36, 40, 48, 50 };
    static const float bd[] = { 0, 1, -12, -38, -57, -68, -76, -80, -81, -85, -86, -88, -89 };
    for (int k = 1; k < 13; k++)
    {
        if (v <= bv[k])
        {
            const float d = bd[k - 1] + (bd[k] - bd[k - 1]) * (v - bv[k - 1]) / (bv[k] - bv[k - 1]);
            return 10e-6f * (1.0f + d / 100.0f);
        }
    }
    return 10e-6f * 0.11f;
}

typedef struct
{
    float v_ref, v_in, ilim_a;
    float r_load;          /* ohm, 0: none              */
    float v_bat, r_bat;    /* battery, r_bat 0: none    */
    float c_x_uf;          /* bench-node ceramics [uF]  */
    float t_end_s;
    /* events */
    float t_load_s;   float r_load2, v_bat2, r_bat2;   /* load change (t_load_s > 0) */
    float t_ilim_s;   float ilim2_a;                   /* limit change              */
    float t_open_s, t_close_s;                         /* output switch open        */
    bool  battery_from_start;                          /* the battery is there at t = 0 */
    float il_cap_a;                                    /* per-period bound on the average L1 current (COMP1), 0: none */
    float v_ref2, t_ref_s;                             /* setpoint change, slewed 4 mV per cycle */
    bool  meas_ix;                                     /* the limit measures the switch current (ID_MON, R6) */
    bool  c_x_fixed;                                   /* c_x_uf is not derated with bias (bulk, mixed) */
    float sw_r0_ohm, sw_tau_us;                        /* Q5/Q6 turn-on: r_sw + r0 exp(-t / tau) after closing */
    bool  fast;                                        /* voltage law every 10 us on period-synchronous samples */
    float kp, ki, kd;                                  /* fast-loop gains (ki 0: SIM_GAINS') */
    float kff_l, kff_r;                                /* load-current feedforward (ID_MON, synchronous) */
    float rc_tau_us;                                   /* divider RC in front of the synchronous sample (0: none) */
    float sample_frac;                                 /* where in the period the sample is taken (0: 0.5) */
    float t_load2_s, r_load3;                          /* a second load change (t_load2_s > 0) */
    float vx_floor;                                    /* bench node held at or above this while the switch is open */
    bool  fw_sync;                                     /* the 20 kHz loop measures as the 2026-10-23 firmware does: V_out
                                                          over the last 9 periods (22.5 us), I_out over the last 8
                                                          (20 us), both read at the cycle that uses them */
    int   n_avg;                                       /* fast loop: samples averaged (1-4, 0: 4) */
    int   fast_div;                                    /* periods per fast update (1-4, 0: 4 = 100 kHz at 400 kHz) */
    bool  entry;                                       /* CCM enters at t = 0: the integrator starts at u0_v and L1
                                                          at its average plus il0_off_a (the first period starts from
                                                          DCM's zero current, not from the CCM ripple's value) */
    float u0_v, il0_off_a;
    float v_plant_off;                                 /* extra average switch-node volts (dead-time conduction at light
                                                          load): the integrator settles at I R - v_plant_off */
} SimCase;

typedef struct
{
    float i_end, v_end, i_sd, il_max, il_max_after_ev, vb_max, vb_min;
    float lim_share;     /* share of the last 2 ms spent limiting */
    float t_settle_s;    /* last time |I - I_end| > 5 % of I_end (+50 mA) */
    float u_end;
    float t_uvp_s, t_ovp_s;   /* first PID sample under 50 % / over 110 % of V_ref (0: none) */
    float vx_close;           /* bench node when the switch closed */
    float vb_after[4];        /* board node 50, 100, 150, 300 us after the switch closed */
    float w1_min, w1_max, w2_min, w2_max;   /* board node extremes after the 1st / 2nd load change */
    float w1_settle_s, w2_settle_s;         /* last time |vb - V_ref| > 0.1 V in each window, from its change */
    float e_max, e_min;                     /* board node extremes in the first 2 ms (entry cases) */
} SimResult;

static const BuckCcmGains SIM_GAINS = {
    .ki = 3142.0f, .kp = 0.0f, .kd = 0.0f, .u_min_v = -1.0f, .u_max_v = 2.0f, .dt_s = 50e-6f,
    .ilim_a = 0.0f, .kl = LIM_KL, .dt_lim_s = 50e-6f,
};

/** SIM_DUMP: when set, sim() writes "t_us,vb" every 0.5 us inside
 *  [g_dump_t0, g_dump_t1) and [g_dump_t2, g_dump_t3) (seconds). */
static FILE *g_dump = NULL;
static float g_dump_t0, g_dump_t1, g_dump_t2, g_dump_t3;
static SimResult sim(const SimCase *k)
{
    const float dt = 0.02e-6f, L = 4.7e-6f, r_l = 0.022f, r_sw = 0.035f, l_w = 0.5e-6f;
    const float c_b = 5.0f * c_mlcc(k->v_ref);
    const float c_x = k->c_x_uf * 1e-6f * (k->c_x_fixed ? 1.0f : (c_mlcc(k->v_ref) / 10e-6f));
    BuckCcmTimingCfg c = {
        .period_ns = 2500u, .pulse_start_ns = 260u, .dt_ns = 20u, .refresh_ns = 200u,
        .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u,
    };
    const float r = buck_ccm_refresh_share(&c);
    BuckCcmGains g = SIM_GAINS;
    g.ilim_a = k->ilim_a;
    if (k->fast)
    {
        g.dt_s = 2.5e-6f * (float)((k->fast_div > 0) ? k->fast_div : 4);
        g.kp = k->kp;  g.kd = k->kd;  g.kff_l = k->kff_l;  g.kff_r = k->kff_r;
        if (k->ki > 0.0f) { g.ki = k->ki; }
    }
    const int n_per = (int)(2.5e-6f / dt + 0.5f);   /* one switching period */
    const int n_avg = (k->n_avg > 0) ? k->n_avg : 4;
    float vsync[4] = { 0 };  int vsync_n = 0;
    float isync[4] = { 0 };
    float vrc = k->v_ref;
    float r_load = k->r_load, v_bat = k->v_bat, r_bat = k->r_bat;
    float v_ref = k->v_ref;
    if (!k->battery_from_start) { r_bat = 0.0f; }
    /* start regulated at V_ref with the resistor alone */
    float i0 = (r_load > 0.0f) ? k->v_ref / r_load : 0.0f;
    float il = i0, vb = k->v_ref + i0 * r_sw, ix = i0, vx = k->v_ref;
    BuckCcmState s = { 0 };
    buck_ccm_reset(&s, k->entry ? k->u0_v : i0 * (r_l + r_sw) - k->v_plant_off, k->v_ref, k->v_ref);
    if (k->entry) { il += k->il0_off_a; }
    float d = (k->v_ref * (1.0f - r) + s.u_v) / k->v_in, d_next = d;
    const int n_isr = (int)(50e-6f / dt + 0.5f), n_apply = (int)(13e-6f / dt + 0.5f);
    const int n_end = (int)(k->t_end_s / dt);
    double vsum = 0.0, isum = 0.0;  int vcnt = 0, icnt = 0;
    float i_meas = 0.0f;
    int apply_at = -1;
    SimResult res = { 0 };
    res.vb_min = 1e9f;
    res.e_min  = 1e9f;
    const int n_tail = (int)(2e-3f / dt);
    double tail_i = 0.0, tail_i2 = 0.0;  int tail_n = 0, tail_lim = 0, tail_isr = 0;
    const int hist_cap = n_end / n_isr + 2;
    float *hist_i = malloc(sizeof(float) * (size_t)hist_cap), *hist_t = malloc(sizeof(float) * (size_t)hist_cap);
    int hist_n = 0;
    for (int n = 0; n < n_end; n++)
    {
        const float t = (float)n * dt;
        const int ph = n % n_isr;
        if (ph >= n_isr - (int)((k->fw_sync ? 22.5e-6f : 45e-6f) / dt)) { vsum += vb; vcnt++; }
        if (k->fw_sync ? (ph >= n_isr - (int)(20e-6f / dt))
                       : (ph >= (int)(13.9e-6f / dt) && ph < (int)(23.3e-6f / dt))) { isum += k->meas_ix ? ix : il; icnt++; }
        if (ph == 0 && n > 0)
        {
            const float v_meas = (float)(vsum / (vcnt ? vcnt : 1));
            if (k->t_ref_s > 0.0f && t >= k->t_ref_s)
            {
                if (v_ref + 0.004f < k->v_ref2) { v_ref += 0.004f; } else if (v_ref > k->v_ref2 + 0.004f) { v_ref -= 0.004f; } else { v_ref = k->v_ref2; }
            }
            if (t > 1e-3f && res.t_uvp_s == 0.0f && v_meas < 0.5f * v_ref) { res.t_uvp_s = t; }
            if (t > 1e-3f && res.t_ovp_s == 0.0f && v_meas > 1.1f * v_ref) { res.t_ovp_s = t; }
            if (k->fw_sync) { i_meas = (float)(isum / (icnt ? icnt : 1)); }
            buck_ccm_limit(&s, &g, v_ref, v_meas, i_meas);
            uint32_t ton = 0u;
            (void)ton;
            if (!k->fast)
            {
                ton = buck_ccm_update(&s, &g, &c, v_meas, k->v_in);
                BuckCcmTiming tt;
                buck_ccm_timing_ticks(&c, s.t_on_ticks, &tt);
                d_next = (float)(tt.a_cmp2 - tt.a_cmp4) / (float)buck_ccm_ticks(2500u);
                apply_at = n + n_apply;
            }
            if (!k->fw_sync) { i_meas = (float)(isum / (icnt ? icnt : 1)); }   /* read next cycle */
            vsum = isum = 0.0;  vcnt = icnt = 0;
            if (n >= n_end - n_tail) { tail_isr++; if (s.limiting) { tail_lim++; } }
#ifdef SIM_TRACE
            if (n % (n_isr * 10) == 0) { printf("T %.2f ms v %.3f i %.3f lim %d vl %.4f u %.4f ton %u\n", 1e3f * t, v_meas, i_meas, s.limiting, s.v_lim_v, s.u_v, ton); }
#endif
            if (hist_n < hist_cap) { hist_i[hist_n] = i_meas; hist_t[hist_n] = t; hist_n++; }
        }
        if (n == apply_at) { d = d_next; }   /* before a new update can move apply_at */
        if (k->fast)
        {
            const int ph_s = (int)(((k->sample_frac > 0.0f) ? k->sample_frac : 0.5f) * (float)n_per);
            if (n % n_per == ph_s) { isync[vsync_n & 3] = ix; vsync[vsync_n++ & 3] = vrc; }   /* synchronous samples */
            if (n > 0 && n % (((k->fast_div > 0) ? k->fast_div : 4) * n_per) == 0)
            {
                float v_f = 0.0f;
                for (int j = 0; j < n_avg; j++) { v_f += vsync[(vsync_n - 1 - j) & 3]; }
                v_f /= (float)n_avg;
                s.i_out_a = isync[(vsync_n - 1) & 3];
                (void)buck_ccm_update(&s, &g, &c, v_f, k->v_in);
                BuckCcmTiming tt;
                buck_ccm_timing_ticks(&c, s.t_on_ticks, &tt);
                d_next = (float)(tt.a_cmp2 - tt.a_cmp4) / (float)buck_ccm_ticks(2500u);
                apply_at = n + n_per;   /* preloaded: the next period boundary */
            }
        }
        if (k->rc_tau_us > 0.0f) { vrc += (vb - vrc) * dt / (k->rc_tau_us * 1e-6f); } else { vrc = vb; }
        if (k->t_load2_s > 0.0f && n == (int)(k->t_load2_s / dt)) { r_load = k->r_load3; }
        if (k->t_load_s > 0.0f && t >= k->t_load_s)
        {
            const bool w2 = (k->t_load2_s > 0.0f && t >= k->t_load2_s);
            float *mn = w2 ? &res.w2_min : &res.w1_min, *mx = w2 ? &res.w2_max : &res.w1_max;
            if (*mn == 0.0f || vb < *mn) { *mn = vb; }
            if (vb > *mx) { *mx = vb; }
            if (fabsf(vb - v_ref) > 0.1f)
            {
                if (w2) { res.w2_settle_s = t - k->t_load2_s; } else { res.w1_settle_s = t - k->t_load_s; }
            }
        }
        if (k->t_load_s > 0.0f && n == (int)(k->t_load_s / dt))
        {
            r_load = k->r_load2; v_bat = k->v_bat2; r_bat = k->r_bat2;
        }
        if (k->t_ilim_s > 0.0f && n == (int)(k->t_ilim_s / dt)) { g.ilim_a = k->ilim2_a; }
        const bool sw_on = !(t >= k->t_open_s && t < k->t_close_s);
        /* explicit update, small step */
        const float v_sw = d * k->v_in + r * vb + k->v_plant_off;
        il += dt * (v_sw - vb - il * r_l) / L;
        if (k->il_cap_a > 0.0f && il > k->il_cap_a) { il = k->il_cap_a; }
        float r_sw_t = r_sw;
        if (k->sw_r0_ohm > 0.0f && t >= k->t_close_s)
        {
            r_sw_t += k->sw_r0_ohm * expf(-(t - k->t_close_s) / (k->sw_tau_us * 1e-6f));
        }
        if (n == (int)(k->t_close_s / dt)) { res.vx_close = vx; }
        {
            static const float after_us[4] = { 50.0f, 100.0f, 150.0f, 300.0f };
            for (int j = 0; j < 4; j++)
            {
                if (n == (int)((k->t_close_s + after_us[j] * 1e-6f) / dt)) { res.vb_after[j] = vb; }
            }
        }
        /* implicit in ix: r_sw_t / l_w is stiff while the switch turns on */
        if (sw_on) { ix = (ix + dt * (vb - vx) / l_w) / (1.0f + dt * r_sw_t / l_w); } else { ix = 0.0f; }
        float i_load = (r_load > 0.0f) ? vx / r_load : 0.0f;
        if (r_bat > 0.0f)
        {
            /* the battery is stiff: solve the node with it implicitly */
            const float a = dt / c_x;
            vx = (vx + a * (ix - i_load + v_bat / r_bat)) / (1.0f + a / r_bat);
        }
        else
        {
            vx += dt * (ix - i_load) / c_x;
        }
        if (!sw_on && vx < k->vx_floor) { vx = k->vx_floor; }   /* what held VBUS at 3.2 V on the bench */
        vb += dt * (il - ix) / c_b;
        if (g_dump != NULL && n % 25 == 0 &&
            ((t >= g_dump_t0 && t < g_dump_t1) || (t >= g_dump_t2 && t < g_dump_t3)))
        {
            fprintf(g_dump, "%.2f,%.5f,%.5f,%.5f\n", 1e6f * t, vb, vx, il);
        }
        if (il > res.il_max) { res.il_max = il; }
        if (t < 2e-3f) { if (vb > res.e_max) { res.e_max = vb; } if (vb < res.e_min) { res.e_min = vb; } }
        const float t_ev = (k->t_load_s > 0.0f) ? k->t_load_s : ((k->t_close_s > 0.0f) ? k->t_close_s : 0.0f);
        if (t >= t_ev && il > res.il_max_after_ev) { res.il_max_after_ev = il; }
        if (t >= t_ev && t > 1e-3f) { if (vb > res.vb_max) { res.vb_max = vb; } if (vb < res.vb_min) { res.vb_min = vb; } }
        if (n >= n_end - n_tail) { tail_i += il; tail_i2 += (double)il * il; tail_n++; }
    }
    res.i_end = (float)(tail_i / tail_n);
    res.i_sd  = sqrtf((float)fmax(0.0, tail_i2 / tail_n - (tail_i / tail_n) * (tail_i / tail_n)));
    res.v_end = vb;
    res.lim_share = tail_isr ? (float)tail_lim / (float)tail_isr : 0.0f;
    res.u_end = s.u_v;
    const float band = 0.05f * fabsf(res.i_end) + 0.05f;
    res.t_settle_s = 0.0f;
    for (int j = 0; j < hist_n; j++) { if (fabsf(hist_i[j] - res.i_end) > band) { res.t_settle_s = hist_t[j]; } }
    free(hist_i);
    free(hist_t);
    return res;
}

static void print_res(const char *name, const SimResult *r)
{
    printf("  %-38s I %.3f A (sd %.3f) V %.2f  IL max %.2f A  Vb %.2f..%.2f  lim %.0f%%  settled %.1f ms  u %.3f\n",
           name, r->i_end, r->i_sd, r->v_end, r->il_max, r->vb_min < 1e8f ? r->vb_min : 0.0f, r->vb_max, 100.0f * r->lim_share,
           1e3f * r->t_settle_s, r->u_end);
}

static void test_limit(void)
{
    printf("current limit, averaged plant (400 kHz, 24 V in, kl %.0f V/(A s)):\n", (double)LIM_KL);
    /* 1. The bench check: 20 V into 25.3 ohm (0.79 A), limit 0.5 A applied
     *    at 5 ms: V_out folds to 12.65 V (time constant R / kl) and holds
     *    without a limit cycle or an overshoot. */
    {
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 0.0f, .r_load = 25.3f, .c_x_uf = 8.0f,
                      .t_end_s = 400e-3f, .t_ilim_s = 5e-3f, .ilim2_a = 0.5f, .t_open_s = 1.0f, .t_close_s = 1.0f };
        SimResult r = sim(&k);
        print_res("20 V, 25.3 ohm, limit 0 -> 0.5 A", &r);
        CHECK(fabsf(r.i_end - 0.5f) < 0.005f, "limit into 25.3 ohm settled at %.3f A", r.i_end);
        CHECK(fabsf(r.v_end - 0.5f * 25.3f) < 0.15f, "V_out %.2f V, want 12.65", r.v_end);
        CHECK(r.i_sd < 0.015f && r.lim_share > 0.99f, "limit cycle: sd %.3f A, limiting %.2f", r.i_sd, r.lim_share);
        CHECK(r.vb_max < 20.3f, "overshoot to %.2f V on the fold", r.vb_max);
        CHECK(r.vb_min > 12.4f, "fold undershot to %.2f V", r.vb_min);
        CHECK(r.t_settle_s < 300e-3f, "settled only at %.1f ms", 1e3f * r.t_settle_s);
        CHECK(fabsf(r.u_end - 0.016f) < 0.01f, "voltage integrator moved to %.3f V while limiting", r.u_end);
    }
    /* 2. Released: the limit goes from 0.5 to 3 A after the fold; V_out
     *    returns to 20 V without overshooting 5 % and the integrator is intact. */
    {
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 0.5f, .r_load = 25.3f, .c_x_uf = 8.0f,
                      .t_end_s = 500e-3f, .t_ilim_s = 350e-3f, .ilim2_a = 3.0f, .t_open_s = 1.0f, .t_close_s = 1.0f,
                      .t_load_s = 300e-3f, .r_load2 = 25.3f };
        SimResult r = sim(&k);
        print_res("limit 0.5 -> 3 A (release)", &r);
        CHECK(fabsf(r.v_end - 20.0f) < 0.05f, "released to %.2f V", r.v_end);
        CHECK(r.vb_max < 20.0f * 1.05f, "release overshoot to %.2f V", r.vb_max);
        CHECK(r.lim_share == 0.0f, "still limiting after the release");
        CHECK(fabsf(r.u_end - 0.015f) < 0.02f, "voltage integrator %.3f V after the release", r.u_end);
    }
    /* 3. A battery that is there from the start at the setpoint (4S, 14.8 V,
     *    no current), then the setpoint slews to 16.8 V as a contract change
     *    does: the limit holds 2 A without a limit cycle, L1 never far over. */
    const float rbats[] = { 0.01f, 0.05f, 0.2f };
    for (int j = 0; j < 3; j++)
    {
        SimCase k = { .v_ref = 14.8f, .v_in = 24.0f, .ilim_a = 2.0f, .c_x_uf = 8.0f,
                      .v_bat = 14.8f, .r_bat = rbats[j], .battery_from_start = true,
                      .t_end_s = 60e-3f, .t_ref_s = 5e-3f, .v_ref2 = 16.8f,
                      .t_open_s = 1.0f, .t_close_s = 1.0f };
        SimResult r = sim(&k);
        char name[64];  snprintf(name, sizeof name, "battery %.0f mOhm, 14.8 -> 16.8 V, 2 A", 1e3f * rbats[j]);
        print_res(name, &r);
        CHECK(fabsf(r.i_end - 2.0f) < 0.02f, "battery current %.3f A, want 2 A", r.i_end);
        CHECK(r.i_sd < 0.03f, "battery: limit cycle, sd %.3f A", r.i_sd);
        CHECK(r.il_max < 2.6f, "battery: L1 peaked at %.2f A", r.il_max);
    }
    /* 3b. Hot-plugged at 16.8 V (reported: the output capacitors and the
     *     battery meet before any loop can act; the 9 A comparator is the
     *     bound there).  It must still settle at the limit. */
    for (int j = 0; j < 3; j++)
    {
        SimCase k = { .v_ref = 16.8f, .v_in = 24.0f, .ilim_a = 2.0f, .c_x_uf = 8.0f,
                      .t_end_s = 40e-3f, .t_load_s = 5e-3f, .v_bat2 = 14.8f, .r_bat2 = rbats[j],
                      .t_open_s = 1.0f, .t_close_s = 1.0f };
        SimResult r = sim(&k);
        char name[64];  snprintf(name, sizeof name, "battery %.0f mOhm hot-plugged, 2 A", 1e3f * rbats[j]);
        print_res(name, &r);
        CHECK(fabsf(r.i_end - 2.0f) < 0.02f && r.i_sd < 0.03f, "hot-plug: %.3f A sd %.3f", r.i_end, r.i_sd);
        CHECK(r.t_settle_s < 15e-3f, "hot-plug: settled only at %.1f ms", 1e3f * r.t_settle_s);
    }
    /* 4. Load step through the limit: 2 -> 4 A at 20 V, limit 3 A. */
    {
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 3.0f, .r_load = 10.0f, .c_x_uf = 8.0f,
                      .t_end_s = 120e-3f, .t_load_s = 5e-3f, .r_load2 = 5.0f, .t_open_s = 1.0f, .t_close_s = 1.0f };
        SimResult r = sim(&k);
        print_res("20 V, 2 -> 4 A load, limit 3 A", &r);
        CHECK(fabsf(r.i_end - 3.0f) < 0.02f, "step into the limit: %.3f A", r.i_end);
        CHECK(r.il_max_after_ev < 6.0f, "step into the limit: L1 peaked at %.2f A", r.il_max_after_ev);
    }
    /* 5. Under the limit nothing changes: 20 V, 25.3 ohm, limit 3 A. */
    {
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 3.0f, .r_load = 25.3f, .c_x_uf = 8.0f,
                      .t_end_s = 20e-3f, .t_open_s = 1.0f, .t_close_s = 1.0f };
        SimResult r = sim(&k);
        print_res("20 V, 25.3 ohm, limit 3 A (idle)", &r);
        CHECK(r.lim_share == 0.0f && fabsf(r.v_end - 20.0f) < 0.02f, "limit acted under its level");
    }
    /* 6. The 2026-10-20 dump: switch open 20 ms at 20 V with 25.3 ohm on the
     *    bench node, then reconnected (bench: -5.2 V, then +2.9 V, SW_OVP).
     *    Reported: the PID-rate limit cannot reach a ring at 10-20 kHz, a
     *    per-period bound on L1 (COMP1) would. */
    const float lims[] = { 0.0f, 1.5f, 1.5f, 1.5f };
    const float caps[] = { 0.0f, 0.0f, 3.0f, 2.0f };
    for (int j = 0; j < 4; j++)
    {
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = lims[j], .r_load = 25.3f, .c_x_uf = 8.0f,
                      .t_end_s = 40e-3f, .t_open_s = 5e-3f, .t_close_s = 25e-3f, .il_cap_a = caps[j] };
        SimResult r = sim(&k);
        char name[64];  snprintf(name, sizeof name, "switch-pulse 20 ms, limit %.1f, L1 cap %.1f", lims[j], caps[j]);
        print_res(name, &r);
    }
}

/* IL_MON clips at 0 A (INA281, unidirectional): the mean of a clipped
 * triangle, sampled at evenly spread phases, back to the average current. */
static void test_unclip(void)
{
    const float ripples[] = { 0.5f, 1.8f, 3.2f, 6.0f };
    for (int j = 0; j < 4; j++)
    {
        const float dp = ripples[j];
        for (float i = -0.4f * dp; i <= 3.0f; i += 0.05f)
        {
            /* the clipped mean of a triangle from i - dp/2 to i + dp/2 */
            double m = 0.0;
            const int n = 4000;
            for (int k = 0; k < n; k++)
            {
                const double x = (double)i - 0.5 * dp + dp * (k + 0.5) / n;
                m += (x > 0.0) ? x : 0.0;
            }
            m /= n;
            const float back = buck_ccm_unclip_a((float)m, dp);
            CHECK(fabsf(back - i) < 2e-3f, "unclip ripple %.1f: I %.3f -> m %.4f -> %.4f", dp, i, m, back);
        }
    }
    CHECK(buck_ccm_unclip_a(0.0f, 2.0f) == -1.0f, "a zero reading is the valley at 0");
    CHECK(buck_ccm_unclip_a(0.7f, 0.0f) == 0.7f, "no ripple, no change");
    /* the 2026-10-21 bench point: 0.5 A at 12.65 V from 24 V, 400 kHz */
    const float dp = buck_ccm_ripple_a(24.0f, 12.65f, 1318u, 4.7e-6f);
    CHECK(fabsf(dp - 3.18f) < 0.02f, "ripple %.3f A", dp);
}

#ifdef SIM_EXPLORE
/* Calibrate the reconnect: bench-side C and the Q5/Q6 turn-on against
 * 2026-10-20 (no bound: about -5.2 / +2.9 V) and 2026-10-21 (3.5 A bound:
 * board 14.68 / 11.18 / 10.02 V at 50 / 100 / 150 us, SW_UVP). */
static void explore_reconnect(void)
{
    /* firmware options on the plausible plant range */
    const float cxs[] = { 200.0f, 400.0f };
    const float taus[] = { 60.0f, 100.0f };
    const float r0s[] = { 2.0f, 5.0f };
    const float lims[] = { 0.0f, 3.0f, 1.5f };
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 2; c++) for (int d = 0; d < 3; d++)
    {
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = lims[d], .r_load = 25.3f, .c_x_uf = cxs[a], .c_x_fixed = true,
                      .t_end_s = 40e-3f, .t_open_s = 5e-3f, .t_close_s = 21e-3f, .meas_ix = true,
                      .sw_r0_ohm = r0s[c], .sw_tau_us = taus[b] };
        SimResult r = sim(&k);
        printf("cx %3.0f tau %3.0f r0 %3.1f lim %.1f | vb 50/100/150/300us %5.2f %5.2f %5.2f %5.2f  min %5.2f max %5.2f  uvp %.2f ovp %.2f ms  end I %.2f V %.2f\n",
               cxs[a], taus[b], r0s[c], lims[d], r.vb_after[0], r.vb_after[1], r.vb_after[2], r.vb_after[3],
               r.vb_min, r.vb_max, r.t_uvp_s ? 1e3f * (r.t_uvp_s - k.t_close_s) : -1.0f,
               r.t_ovp_s ? 1e3f * (r.t_ovp_s - k.t_close_s) : -1.0f, r.i_end, r.v_end);
    }
}
#endif

#ifdef SIM_FAST
/* Fast-loop gain sweep: 20 V, load step 0.8 -> 2.5 A at 5 ms and back at
 * 15 ms, across bench-side C; plus the switch-pulse reconnect. */
typedef struct { float dip, peak, sd, settle_ms; bool ok; } StepMetrics;
static int g_fast_div = 4;
static StepMetrics step_case_ff(bool fast, float kp, float ki, float kd, float cx, bool fixed, int n_avg,
                                float kff_l, float kff_r)
{
    StepMetrics m = { 0 };
    SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 0.0f, .r_load = 25.3f, .c_x_uf = cx, .c_x_fixed = fixed,
                  .t_end_s = 25e-3f, .t_load_s = 5e-3f, .r_load2 = 8.0f, .t_open_s = 1.0f, .t_close_s = 1.0f,
                  .fast = fast, .kp = kp, .ki = ki, .kd = kd, .n_avg = n_avg, .kff_l = kff_l, .kff_r = kff_r,
                  .fast_div = g_fast_div };
    SimResult r = sim(&k);
    m.dip = r.vb_min - 20.0f;  m.peak = r.vb_max - 20.0f;  m.sd = r.i_sd;
    m.ok = (r.t_uvp_s == 0.0f && r.t_ovp_s == 0.0f && fabsf(r.v_end - 20.0f) < 0.1f);
    return m;
}
static StepMetrics step_case(bool fast, float kp, float ki, float kd, float cx, bool fixed, int n_avg)
{
    return step_case_ff(fast, kp, ki, kd, cx, fixed, n_avg, 0.0f, 0.0f);
}
#ifndef SIM_DIV
#define SIM_DIV 4
#endif
static const int g_div_sweep = SIM_DIV;   /* periods per fast update: 4 = 100 kHz at 400 kHz */
typedef struct { float dip, pk_step, pk_rel, dip_rel, st_ms, rt_ms, sd; bool ok; } StepM2;
static StepM2 step2(bool fast, int div, float kp, float ki, float kd, float cx, bool fixed, int n_avg)
{
    g_fast_div = div;
    SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 0.0f, .r_load = 25.3f, .c_x_uf = cx, .c_x_fixed = fixed,
                  .t_end_s = 30e-3f, .t_load_s = 5e-3f, .r_load2 = 8.0f, .t_load2_s = 17e-3f, .r_load3 = 25.3f,
                  .t_open_s = 1.0f, .t_close_s = 1.0f, .fast = fast, .kp = kp, .ki = ki, .kd = kd, .n_avg = n_avg,
                  .fast_div = div, .rc_tau_us = 0.8f, .sample_frac = 0.5f };
    SimResult r = sim(&k);
    StepM2 m = { r.w1_min - 20.0f, r.w1_max - 20.0f, r.w2_max - 20.0f, r.w2_min - 20.0f,
                 1e3f * r.w1_settle_s, 1e3f * r.w2_settle_s, r.i_sd, false };
    m.ok = (r.t_uvp_s == 0.0f && r.t_ovp_s == 0.0f && fabsf(r.v_end - 20.0f) < 0.1f && r.i_sd < 0.05f
            && m.st_ms < 11.0f && m.rt_ms < 11.0f);
    return m;
}
static void explore_fast(void)
{
    /* bench-side C: board ceramics only (8 uF of QT Py ceramics), the 55 uF
     * measured 2026-10-23 (fixed), and 300 uF bulk (fixed) */
    const float cxs[] = { 8.0f, 55.0f, 300.0f };
    const bool fixed[] = { false, true, true };
    printf("divider RC 0.8 us, synchronous samples mid-period; step 0.8 -> 2.5 A at 5 ms, back at 17 ms (20 V)\n");
    printf("  [cx: step dip / overshoot, settle ms | release peak / undershoot, settle ms]\n");
    {
        printf("legacy 20 kHz (ki 3142, 45 us mean):");
        for (int a = 0; a < 3; a++)
        {
            StepM2 m = step2(false, 4, 0, 0, 0, cxs[a], fixed[a], 4);
            printf("  [%3.0f: %+.2f/%+.2f %.1f | %+.2f/%+.2f %.1f%s]", cxs[a], m.dip, m.pk_step, m.st_ms, m.pk_rel, m.dip_rel, m.rt_ms, m.ok ? "" : " X");
        }
        printf("\n");
    }
    const float kis[] = { 3142.0f, 6283.0f, 12566.0f, 25133.0f };
    const float kps[] = { 0.0f, 0.05f, 0.1f, 0.2f };
    const float kds[] = { 0.0f, 1e-6f, 2e-6f, 3e-6f, 4e-6f };
    const int navs[] = { 1, 4 };
    for (int q = 0; q < 2; q++) for (int i = 0; i < 4; i++) for (int p = 0; p < 4; p++) for (int d = 0; d < 5; d++)
    {
        bool all = true; char buf[400]; int o = 0;
        for (int a = 0; a < 3; a++)
        {
            StepM2 m = step2(true, g_div_sweep, kps[p], kis[i], kds[d], cxs[a], fixed[a], navs[q]);
            all = all && m.ok;
            o += snprintf(buf + o, sizeof buf - o, "  [%3.0f: %+.2f/%+.2f %.1f | %+.2f/%+.2f %.1f]", cxs[a], m.dip, m.pk_step, m.st_ms,
                          m.pk_rel, m.dip_rel, m.rt_ms);
        }
        if (all) { printf("div %d n%d ki %5.0f kp %.2f kd %.0e:%s\n", g_div_sweep, navs[q], kis[i], kps[p], kds[d], buf); }
    }
}
#endif

/** L1's offset when CCM enters from DCM: the first period starts at zero
 *  current, where CCM's ripple would have it at valley + V t_start / L. */
static float entry_il_off(float v, float v_in, float i_load)
{
    const float L = 4.7e-6f, T = 2.5e-6f, t_start = 0.26e-6f;
    const float ripple = (v_in - v) * (v / v_in) * T / L;
    return ripple / 2.0f - i_load - v * t_start / L;
}

/* Forced CCM entry at light load, 25.3 ohm, the fitted 55 uF VBUS side, a
 * plant offset that makes the integrator measured at 400 kHz (2026-10-21/23)
 * and L1's entry offset.  The first entry in a band starts from the fit
 * (regulator_config.h CCM_U0_*); the old formula start (I R + 2 V_sd dt / T)
 * latched SW_OVP at 5 V on the bench (5.05 -> 5.74 V). */
static void test_entry(void)
{
    printf("entry (25.3 ohm, 55 uF):\n");
    const float vs[] = { 5.0f, 9.0f, 15.0f, 20.0f }, um[] = { -0.18f, -0.23f, -0.33f, -0.42f };
    for (int j = 0; j < 4; j++)
    {
        const float i = vs[j] / 25.3f;
        const float u_fit = -(0.016f * vs[j] + 0.100f), u_formula = 0.04f * i + 2.0f * 2.0f * 20.0f / 2500.0f;
        const float u0s[2] = { u_fit, u_formula };
        float hi[2], lo[2];
        for (int a = 0; a < 2; a++)
        {
            SimCase k = { .v_ref = vs[j], .v_in = 24.0f, .r_load = 25.3f, .c_x_uf = 55.0f, .c_x_fixed = true,
                          .t_end_s = 10e-3f, .t_open_s = 1.0f, .t_close_s = 1.0f, .fw_sync = true,
                          .entry = true, .u0_v = u0s[a], .il0_off_a = entry_il_off(vs[j], 24.0f, i),
                          .v_plant_off = i * (0.022f + 0.035f) - um[j] };
            const SimResult r = sim(&k);
            hi[a] = r.e_max - vs[j];  lo[a] = r.e_min - vs[j];
        }
        printf("  %4.1f V (L1 %+.2f A): fit u0 %+.3f V -> %+.3f / %+.3f V, formula u0 %+.3f V -> %+.3f / %+.3f V\n",
               vs[j], entry_il_off(vs[j], 24.0f, i), u_fit, hi[0], lo[0], u_formula, hi[1], lo[1]);
        /* inside the doubled skip band (2 x 200 mV, 2 x 1.5 %) and PD's 5 % */
        CHECK(hi[0] < 0.25f && lo[0] > -0.05f * vs[j], "entry %.0f V: %+.3f / %+.3f V", vs[j], hi[0], lo[0]);
    }
}

#ifdef SIM_DUMP
/* Traces behind plan 9.4's table: 9 CSVs (C x loop), V_out at the board
 * node around the 0.8 -> 2.5 A step (5 ms) and the release (17 ms). */
static void dump_traces(const char *dir)
{
    const float cxs[] = { 8.0f, 55.0f, 300.0f };
    const bool fixed[] = { false, true, true };
    struct { const char *name; bool fast; int div; float kp, ki, kd; } loops[3] = {
        { "20k",  false, 4, 0.0f, 0.0f,     0.0f },
        { "100k", true,  4, 0.0f, 3142.0f,  1e-6f },
        { "400k", true,  1, 0.2f, 12566.0f, 4e-6f },
    };
    g_dump_t0 = 4.9e-3f; g_dump_t1 = 6.5e-3f; g_dump_t2 = 16.9e-3f; g_dump_t3 = 18.5e-3f;
    for (int a = 0; a < 3; a++) for (int l = 0; l < 3; l++)
    {
        char path[256];
        snprintf(path, sizeof path, "%s/trace_cx%.0f_%s.csv", dir, cxs[a], loops[l].name);
        g_dump = fopen(path, "w");
        if (g_dump == NULL) { printf("cannot write %s\n", path); return; }
        fprintf(g_dump, "t_us,vb,vx,il\n");
        SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .r_load = 25.3f, .c_x_uf = cxs[a], .c_x_fixed = fixed[a],
                      .t_end_s = 19e-3f, .t_load_s = 5e-3f, .r_load2 = 8.0f, .t_load2_s = 17e-3f, .r_load3 = 25.3f,
                      .t_open_s = 1.0f, .t_close_s = 1.0f, .fast = loops[l].fast, .fast_div = loops[l].div,
                      .n_avg = 1, .kp = loops[l].kp, .ki = loops[l].ki, .kd = loops[l].kd, .rc_tau_us = 0.8f,
                      .sample_frac = 0.5f };
        SimResult r = sim(&k);
        fclose(g_dump);
        g_dump = NULL;
        printf("%s: step min %+.2f, release max %+.2f\n", path, r.w1_min - 20.0f, r.w2_max - 20.0f);
    }
}
#endif

#ifdef SIM_REPLAY
/* 2026-10-23 bench replay: switch-pulse at 20 V forced CCM 400 kHz into 25.3 ohm,
 * 55 uF VBUS side held at >= 3.2 V while open, the 3 A limit on the switch
 * current, today's 20 kHz loop.  argv: r0 tau_us open_ms vx_floor out.csv */
static float g_close_shift_us = 0.0f;   /* where in the 50 us PID cycle the switch closes */
static void replay(float r0, float tau, float open_ms, float floor_v, float il_cap, float cx_uf, const char *out)
{
    const float t_close = 30e-3f + g_close_shift_us * 1e-6f;
    SimCase k = { .v_ref = 20.0f, .v_in = 24.0f, .ilim_a = 3.0f, .r_load = 25.3f, .c_x_uf = cx_uf, .c_x_fixed = true,
                  .t_end_s = 40e-3f, .t_open_s = t_close - open_ms * 1e-3f, .t_close_s = t_close, .meas_ix = true,
                  .sw_r0_ohm = r0, .sw_tau_us = tau, .vx_floor = floor_v, .fw_sync = true, .il_cap_a = il_cap };
    g_dump_t0 = t_close - 200e-6f; g_dump_t1 = t_close + 6e-3f; g_dump_t2 = 1.0f; g_dump_t3 = 1.0f;
    g_dump = fopen(out, "w");
    fprintf(g_dump, "t_us,vb,vx,il\n");
    SimResult r = sim(&k);
    fclose(g_dump); g_dump = NULL;
    printf("cap %.1f r0 %.1f tau %.0f: vx0 %.2f  vb 50/100/150/300 us %.2f %.2f %.2f %.2f  vb min %.2f max %.2f  IL max %.2f  uvp %d ovp %d\n",
           il_cap, r0, tau, r.vx_close, r.vb_after[0], r.vb_after[1], r.vb_after[2], r.vb_after[3], r.vb_min, r.vb_max,
           r.il_max_after_ev, r.t_uvp_s > 0, r.t_ovp_s > 0);
}
#endif


/* Boost CCM (buck_ccm.h): plays the program tick by tick and checks every
 * edge on both legs, then runs the duty law on an averaged boost plant.
 *   Q1 TA1  reset Timer A period, set Timer A CMP4
 *   Q2 TA2  set Timer A CMP2, reset Timer A CMP3
 *   Q4 TB2  set Timer B period, reset Timer B CMP2
 *   Q3 TB1  set Timer B CMP3, reset Timer B CMP4                            */
static void boost_play(const BuckCcmTimingCfg *c, const BoostCcmTiming *t, bool band)
{
    const uint32_t per = buck_ccm_ticks(c->period_ns), dt = buck_ccm_ticks(c->dt_ns);
    CHECK(t->a_cmp2 >= BUCK_CCM_MIN_COMPARE && t->b_cmp4 < per && t->b_cmp3 < t->b_cmp4,
          "compares a2 %u b3 %u b4 %u per %u", t->a_cmp2, t->b_cmp3, t->b_cmp4, per);
    int q1 = 1, q2 = 0, q3 = 0, q4 = 0;
    uint32_t off1 = 0, off2 = 0, off3 = 0, off4 = 0, q4_on_at = 0, q2_on_at = 0, q2_off_at = 0;
    bool q2_seen = false;
    for (uint32_t n = 0; n < 3u * per; n++)
    {
        const uint32_t k = n % per;
        int n1 = q1, n2 = q2, n3 = q3, n4 = q4;
        if (k == 0u)        { n1 = 0; n4 = 1; }
        if (k == t->a_cmp2) { n2 = 1; }
        if (k == t->a_cmp3) { n2 = 0; }
        if (k == t->a_cmp4) { n1 = 1; }
        if (k == t->b_cmp2) { n4 = 0; }
        if (k == t->b_cmp3) { n3 = 1; }
        if (k == t->b_cmp4) { n3 = 0; }
        if (n >= per)
        {
            if (n1 && !q1) { CHECK(n - off2 >= dt, "Q1 on %u ticks after Q2 off", n - off2); }
            if (n2 && !q2) { CHECK(n - off1 >= dt, "Q2 on %u ticks after Q1 off", n - off1); q2_on_at = k; q2_seen = true; }
            if (n3 && !q3) { CHECK(n - off4 >= dt, "Q3 on %u ticks after Q4 off", n - off4); }
            if (n4 && !q4) { CHECK(n - off3 >= dt, "Q4 on %u ticks after Q3 off", n - off3); q4_on_at = n; }
            if (!n2 && q2) { q2_off_at = k; CHECK(band || n4, "refresh ends with Q4 off (k %u)", k); }
            if (n2 && !band) { CHECK(n4, "Q2 on while Q4 is off (k %u): L1 would see -V_out", k); }
        }
        if (!n1 && q1) { off1 = n; }
        if (!n2 && q2) { off2 = n; }
        if (!n3 && q3) { off3 = n; }
        if (!n4 && q4) { off4 = n; CHECK(n < per || n - q4_on_at == t->b_cmp2, "Q4 pulse"); }
        CHECK(!(n1 && n2) && !(n3 && n4), "shoot-through at %u: %d%d %d%d", n, n1, n2, n3, n4);
        q1 = n1; q2 = n2; q3 = n3; q4 = n4;
    }
    CHECK(q2_seen && q2_off_at - q2_on_at == buck_ccm_ticks(c->refresh_ns), "refresh %u ticks", q2_off_at - q2_on_at);
}

typedef struct { float v_min, v_max, v_end, il_max, u_end; int clamps; } BoostSim;

/* Averaged boost from 'v0' at V_ref with a load step r1 -> r2 at 10 ms. */
static const BuckCcmTimingCfg *boost_sim_cfg;   /* NULL: boost at 200 kHz */
static BoostSim boost_sim(float v_in, float v_ref, float v0, float r1, float r2, float c_uf, float ilim)
{
    BuckCcmTimingCfg c = { .period_ns = 5000u, .pulse_start_ns = 360u, .dt_ns = 20u, .refresh_ns = 300u,
                           .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u };
    if (boost_sim_cfg) { c = *boost_sim_cfg; }
    const BuckCcmGains g = { .ki = 3142.0f, .kp = 0.0f, .u_min_v = -1.0f, .u_max_v = 2.0f, .dt_s = 50e-6f,
                             .ilim_a = ilim, .kl = 400.0f, .dt_lim_s = 50e-6f };
    BuckCcmState s = { 0 };
    buck_ccm_reset(&s, 0.0f, v_ref, v0);
    const float l = 4.7e-6f, cc = c_uf * 1e-6f, rs = 0.03f,
                rp = (float)(c.refresh_ns + 2u * c.dt_ns) / (float)c.period_ns;   /* Q1's off window */
    float v = v0, il = v0 / r1 / (v_in / v0), d = 1.0f - v_in * (1.0f - rp) / v0, vmeas = v0;
    BoostSim r = { 1e9f, -1e9f, 0, 0, 0, 0 };
    const float h = 0.25e-6f;
    for (int cyc = 0; cyc < 400; cyc++)               /* 20 ms */
    {
        const float rl = (cyc < 200) ? r1 : r2;
        float io = vmeas / rl;
        buck_ccm_limit(&s, &g, v_ref, vmeas, io);
        boost_ccm_update(&s, &g, &c, vmeas, v_in);
        if (s.clamp) { r.clamps++; }
        BoostCcmTiming t;
        boost_ccm_timing_ticks(&c, s.t_on_ticks, &t);
        const float dn = (float)t.t_on_ns / 5000.0f;
        float acc = 0; int na = 0;
        for (int k = 0; k < 200; k++)                 /* 50 us; the new duty lands 13 us in */
        {
            if (k == 52) { d = dn; }
            const float vl = v_in * (1.0f - rp) - v * (1.0f - d) - il * rs;
            il += vl / l * h;
            v  += (il * (1.0f - d) - v / rl) / cc * h;
            if (cyc > 2) { if (v < r.v_min) { r.v_min = v; } if (v > r.v_max) { r.v_max = v; } }
            if (il > r.il_max) { r.il_max = il; }
            if (k >= 20) { acc += v; na++; }
        }
        vmeas = acc / (float)na;
    }
    r.v_end = v; r.u_end = s.u_v;
    return r;
}

static void test_boost(void)
{
    printf("boost CCM:\n");
    for (uint32_t dtn = 20u; dtn <= 40u; dtn += 20u)
    {
        BuckCcmTimingCfg c = { .period_ns = 5000u, .pulse_start_ns = 300u + 2u * dtn + 20u, .dt_ns = dtn,
                               .refresh_ns = 300u, .refresh_interval_ns = 5000u, .min_on_ns = 100u,
                               .min_q2_end_ns = 60u };
        CHECK(boost_ccm_cfg_valid(&c), "cfg dt %u", dtn);
        for (uint32_t on = 0u; on <= 5000u; on += 37u)
        {
            BoostCcmTiming t;
            boost_ccm_timing_ticks(&c, buck_ccm_ticks(on), &t);
            CHECK(t.t_on_ns >= boost_ccm_min_on_ns(&c) - 1u && t.t_on_ns <= boost_ccm_max_on_ns(&c) + 1u,
                  "on %u -> %u", on, t.t_on_ns);
            boost_play(&c, &t, false);
        }
    }
    BuckCcmTimingCfg bad = { .period_ns = 2500u, .pulse_start_ns = 360u, .dt_ns = 20u, .refresh_ns = 300u,
                             .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u };
    CHECK(!boost_ccm_cfg_valid(&bad), "400 kHz with a 5 us refresh interval must be refused");
    bad.period_ns = 5000u; bad.dt_ns = 10u; bad.pulse_start_ns = 340u;
    CHECK(!boost_ccm_cfg_valid(&bad), "10 ns dead time (CMP2 under the minimum) must be refused");

    const struct { float vin, vref, r1, r2; } pts[] = {
        { 24.9f, 28.0f, 25.3f, 25.3f }, { 24.9f, 28.0f, 25.3f, 9.0f }, { 24.9f, 28.0f, 9.0f, 25.3f },
        { 15.0f, 20.0f, 25.3f, 5.0f }, { 10.0f, 20.0f, 20.0f, 6.7f }, { 10.0f, 28.0f, 25.3f, 9.3f },
        { 20.0f, 20.5f, 25.3f, 25.3f },
    };
    for (unsigned j = 0; j < sizeof pts / sizeof pts[0]; j++)
    {
        const BoostSim r = boost_sim(pts[j].vin, pts[j].vref, pts[j].vref, pts[j].r1, pts[j].r2, 55.0f, 0.0f);
        printf("  %4.1f -> %4.1f V, %5.1f -> %5.1f ohm: V %6.3f..%6.3f end %6.3f, IL max %5.2f A, u %+.3f, clamps %d\n",
               pts[j].vin, pts[j].vref, pts[j].r1, pts[j].r2, r.v_min, r.v_max, r.v_end, r.il_max, r.u_end, r.clamps);
        CHECK(fabsf(r.v_end - pts[j].vref) < 0.05f * pts[j].vref, "boost %.1f->%.1f end %.3f",
              pts[j].vin, pts[j].vref, r.v_end);
        CHECK(r.v_max < 1.1f * pts[j].vref, "boost overshoot %.3f", r.v_max);
    }
    /* The limit folds a boost output only down to boost_ccm_v_min (about
     * V_in): below that the current is the load's, at the shortest pulse. */
    BuckCcmTimingCfg c0 = { .period_ns = 5000u, .pulse_start_ns = 360u, .dt_ns = 20u, .refresh_ns = 300u,
                            .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u };
    const float vmin = boost_ccm_v_min(&c0, 24.9f);
    const BoostSim lim = boost_sim(24.9f, 28.0f, 28.0f, 25.3f, 9.0f, 55.0f, 2.9f);
    const BoostSim floor = boost_sim(24.9f, 28.0f, 28.0f, 25.3f, 9.0f, 55.0f, 1.5f);
    printf("  limit 2.9 A, 28 V into 9 ohm: end %.3f V (I %.2f A); limit 1.5 A: end %.3f V, floor %.3f V\n",
           lim.v_end, lim.v_end / 9.0f, floor.v_end, vmin);
    /* R / kl = 22 ms into 9 ohm: 10 ms after the step it is still folding */
    CHECK(lim.v_end < 27.5f && lim.v_end / 9.0f > 2.9f, "boost limit 2.9 A end %.3f", lim.v_end);
    CHECK(floor.v_end > vmin - 0.5f && floor.v_end < vmin + 0.5f, "boost limit floor end %.3f", floor.v_end);
}

/* Band CCM (buck_ccm.h): the boost program with Q1's off window widened to
 * 640 ns + 2 dt and Q4 pulses down to 150 ns, ending inside it. */
static void test_band(void)
{
    printf("band CCM:\n");
    const BuckCcmTimingCfg c = { .period_ns = 5000u, .pulse_start_ns = 150u, .dt_ns = 20u, .refresh_ns = 640u,
                                 .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u };
    CHECK(band_ccm_cfg_valid(&c), "band cfg");
    CHECK(!boost_ccm_cfg_valid(&c), "boost must refuse a Q4 pulse shorter than the window");
    for (uint32_t on = 0u; on <= 5000u; on += 23u)
    {
        BoostCcmTiming t;
        boost_ccm_timing_ticks(&c, buck_ccm_ticks(on), &t);
        boost_play(&c, &t, true);
    }
    printf("  V_out range at 24.9 V in: %.2f..%.2f V\n", boost_ccm_v_min(&c, 24.9f), boost_ccm_v_max(&c, 24.9f));
    CHECK(boost_ccm_v_min(&c, 24.9f) < 0.92f * 24.9f, "band reaches 0.92 V_in");
    boost_sim_cfg = &c;
    const struct { float vin, vref, r1, r2; } pts[] = {
        { 24.9f, 24.0f, 25.3f, 25.3f }, { 24.9f, 24.0f, 25.3f, 9.0f }, { 24.9f, 24.0f, 9.0f, 25.3f },
        { 24.9f, 25.5f, 25.3f, 9.0f }, { 20.0f, 19.0f, 25.3f, 7.0f }, { 10.0f, 9.5f, 25.3f, 4.0f },
    };
    for (unsigned j = 0; j < sizeof pts / sizeof pts[0]; j++)
    {
        const BoostSim r = boost_sim(pts[j].vin, pts[j].vref, pts[j].vref, pts[j].r1, pts[j].r2, 55.0f, 0.0f);
        printf("  %4.1f -> %4.1f V, %5.1f -> %5.1f ohm: V %6.3f..%6.3f end %6.3f, IL max %5.2f A, u %+.3f, clamps %d\n",
               pts[j].vin, pts[j].vref, pts[j].r1, pts[j].r2, r.v_min, r.v_max, r.v_end, r.il_max, r.u_end, r.clamps);
        CHECK(fabsf(r.v_end - pts[j].vref) < 0.02f * pts[j].vref, "band %.1f->%.1f end %.3f",
              pts[j].vin, pts[j].vref, r.v_end);
        CHECK(r.v_max < 1.08f * pts[j].vref, "band overshoot %.3f", r.v_max);
    }
    boost_sim_cfg = NULL;
}

/* CCM handover (buck_ccm.h, 2026-11-06), switch-level with a stiff V_out at
 * 200 kHz: L1 per period from the programmed edges (0 V while Q4 refreshes,
 * V_in - V_out while Q1 is on, -V_out otherwise).  Entry: from DCM's 0 A, one
 * period at t_on + buck_ccm_handover_ticks(I_v) must land on the steady
 * valley; exit: from the valley, one period at t_on + ticks(-I_v) must end
 * at 0 A.  Both within 5 % of the ripple. */
static float handover_period(const BuckCcmTimingCfg *c, uint32_t t_on_ticks, float i0, float v_in, float v_out,
                             float v_fw, float *mean)
{
    const float L = 4.7e-6f, tick = 1e-6f / (float)BUCK_CCM_TICKS_PER_US;
    BuckCcmTiming t;
    buck_ccm_timing_ticks(c, t_on_ticks, &t);
    const float T = (float)c->period_ns * 1e-9f, t_ref = (float)t.b_cmp3 * tick;
    const float t1 = (float)t.a_cmp4 * tick, t2 = (float)t.a_cmp2 * tick;
    /* segments: [0, t_ref) flat, [t_ref, t1) -V_out, [t1, t2) V_in - V_out, [t2, T) -V_out */
    const float seg_t[4] = { t_ref, t1 - t_ref, t2 - t1, T - t2 };
    const float seg_v[4] = { 0.0f, -(v_out + v_fw), v_in - v_out, -(v_out + v_fw) };
    float i = i0, area = 0.0f;
    for (int k = 0; k < 4; k++)
    {
        const float i1 = i + seg_v[k] * seg_t[k] / L;
        area += 0.5f * (i + i1) * seg_t[k];
        i = i1;
    }
    if (mean) { *mean = area / T; }
    return i;
}

static void test_handover(void)
{
    printf("CCM handover (200 kHz, stiff V_out, 24.4 V in):\n");
    const BuckCcmTimingCfg c = { .period_ns = 5000u, .pulse_start_ns = 360u, .dt_ns = 20u, .refresh_ns = 300u,
                                 .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u };
    const float L = 4.7e-6f, v_in = 24.4f, tick = 1e-6f / (float)BUCK_CCM_TICKS_PER_US;
    const float vs[] = { 5.0f, 9.0f, 15.0f, 20.0f };
    for (int j = 0; j < 8; j++)
    {
        for (int q = 0; q < 2; q++)
        {
            /* the second half with a 0.8 V freewheel drop (CCM_HANDOVER_VFW_MV) */
            const float v = vs[j % 4], vfw = (j < 4) ? 0.0f : 0.8f, i_load = q ? 2.0f : v / 25.3f;
            /* steady on-time: volt-second balance over the period */
            BuckCcmTiming t0;
            buck_ccm_timing(&c, 1000u, &t0);
            const float T = 5e-6f, t_ref = (float)t0.b_cmp3 * tick;
            const uint32_t ton = buck_ccm_ticks((uint32_t)(1e9f * (v + vfw) * (T - t_ref) / (v_in + vfw) + 0.5f));
            float m0;
            (void)handover_period(&c, ton, 0.0f, v_in, v, vfw, &m0);
            const float iv_true = i_load - m0;                /* steady valley for this mean */
            float ms;
            const float drift = handover_period(&c, ton, iv_true, v_in, v, vfw, &ms) - iv_true;
            BuckCcmTiming tt;
            buck_ccm_timing_ticks(&c, ton, &tt);
            const float di = (v_in - v) * (float)tt.t_on_ns * 1e-9f / L;
            const float iv = buck_ccm_boundary_a(&c, &tt, i_load, v, vfw, L);
            const int32_t de = buck_ccm_handover_ticks(iv, v_in, L), dx = buck_ccm_handover_ticks(-iv, v_in, L);
            const float i_entry = handover_period(&c, (uint32_t)((int32_t)ton + de), 0.0f, v_in, v, vfw, NULL);
            const float i_exit  = handover_period(&c, (uint32_t)((int32_t)ton + dx), iv_true, v_in, v, vfw, NULL);
            printf("  %4.1f V %4.2f A fw %.1f V: ripple %.2f A, valley %+.3f A (model %+.3f), entry dt %+5.0f ns -> %+.3f A "
                   "(none: 0 A, off %+.3f), exit dt %+5.0f ns -> %+.3f A\n",
                   v, i_load, vfw, di, iv_true, iv, (float)de * tick * 1e9f, i_entry, -iv_true,
                   (float)dx * tick * 1e9f, i_exit);
            CHECK(fabsf(drift) < 0.01f * di, "handover %.0f V: steady drift %.3f A", v, drift);
            CHECK(fabsf(i_entry - iv_true) < 0.05f * di, "handover entry %.0f V %.2f A: %.3f vs %.3f", v, i_load, i_entry, iv_true);
            CHECK(fabsf(i_exit) < 0.05f * di, "handover exit %.0f V %.2f A: %.3f", v, i_load, i_exit);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
#ifdef SIM_REPLAY
    if (argc == 9) { g_close_shift_us = (float)atof(argv[8]); argc = 8; }
    if (argc == 8) { replay((float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]),
                            (float)atof(argv[5]), (float)atof(argv[6]), argv[7]); return 0; }
#endif
    test_timing();
    test_law();
#ifdef SIM_EXPLORE
    explore_reconnect();
    return 0;
#endif
#ifdef SIM_FAST
    explore_fast();
    return 0;
#endif
#ifdef SIM_DUMP
    dump_traces(SIM_DUMP);
    return 0;
#endif
    test_unclip();
    test_limit();
    test_entry();
    test_handover();
    test_boost();
    test_band();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("buck_ccm: all checks passed\n");
    return 0;
}
