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
    BuckCcmGains g = { .ki = 3142.0f, .kp = 0.0f, .u_min_v = -1.0f, .u_max_v = 2.0f, .dt_s = 50e-6f };
    BuckCcmState s = { 0 };
    /* Zero error, zero integrator: pure feedforward, D = V_ref (1 - r) / V_in. */
    uint32_t ton = buck_ccm_update(&s, &g, &c, 20.0f, 20.0f, 24.0f, 0.0f);
    const float want = 2500.0f * 20.0f * (1.0f - 150.0f / 10000.0f) / 24.0f;
    CHECK(fabsf((float)ton - want) <= 1.0f, "feedforward %u ns, want %.0f", ton, want);

    /* Averaged plant: V_out follows D V_in / (1 - r) less an IR drop, one
     * PID cycle late.  The integrator must take the drop out. */
    s.u_v = 0.0f;
    float v_out = 19.5f;
    for (int k = 0; k < 4000; k++)
    {
        ton = buck_ccm_update(&s, &g, &c, 20.0f, v_out, 24.0f, 0.0f);
        const float d = (float)ton / 2500.0f;
        v_out = d * 24.0f / (1.0f - 150.0f / 10000.0f) - 2.5f * 0.08f;
    }
    CHECK(fabsf(v_out - 20.0f) < 0.005f, "closed loop settled at %.4f V", v_out);
    CHECK(fabsf(s.u_v - 0.2f * (1.0f - 0.015f)) < 0.01f, "integrator %.3f V", s.u_v);

    /* Anti-windup: V_in too low for the setpoint pins t_on at the ceiling;
     * the integrator must not keep climbing. */
    s.u_v = 0.0f;
    for (int k = 0; k < 2000; k++) { (void)buck_ccm_update(&s, &g, &c, 20.0f, 15.0f, 18.0f, 0.0f); }
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
} SimCase;

typedef struct
{
    float i_end, v_end, i_sd, il_max, il_max_after_ev, vb_max, vb_min;
    float lim_share;     /* share of the last 2 ms spent limiting */
    float t_settle_s;    /* last time |I - I_end| > 5 % of I_end (+50 mA) */
    float u_end;
} SimResult;

static const BuckCcmGains SIM_GAINS = {
    .ki = 3142.0f, .kp = 0.0f, .u_min_v = -1.0f, .u_max_v = 2.0f, .dt_s = 50e-6f,
    .ilim_a = 0.0f, .kl = LIM_KL,
};

static SimResult sim(const SimCase *k)
{
    const float dt = 0.02e-6f, L = 4.7e-6f, r_l = 0.022f, r_sw = 0.035f, l_w = 0.5e-6f;
    const float c_b = 5.0f * c_mlcc(k->v_ref);
    const float c_x = k->c_x_uf * 1e-6f * (c_mlcc(k->v_ref) / 10e-6f);
    BuckCcmTimingCfg c = {
        .period_ns = 2500u, .pulse_start_ns = 260u, .dt_ns = 20u, .refresh_ns = 200u,
        .refresh_interval_ns = 5000u, .min_on_ns = 100u, .min_q2_end_ns = 60u,
    };
    const float r = buck_ccm_refresh_share(&c);
    BuckCcmGains g = SIM_GAINS;
    g.ilim_a = k->ilim_a;
    float r_load = k->r_load, v_bat = k->v_bat, r_bat = k->r_bat;
    float v_ref = k->v_ref;
    if (!k->battery_from_start) { r_bat = 0.0f; }
    /* start regulated at V_ref with the resistor alone */
    float i0 = (r_load > 0.0f) ? k->v_ref / r_load : 0.0f;
    float il = i0, vb = k->v_ref + i0 * r_sw, ix = i0, vx = k->v_ref;
    BuckCcmState s = { .u_v = i0 * (r_l + r_sw), .clamp = 0, .limiting = 0u, .v_lim_v = 0.0f };
    float d = (k->v_ref * (1.0f - r) + s.u_v) / k->v_in, d_next = d;
    const int n_isr = (int)(50e-6f / dt + 0.5f), n_apply = (int)(13e-6f / dt + 0.5f);
    const int n_end = (int)(k->t_end_s / dt);
    double vsum = 0.0, isum = 0.0;  int vcnt = 0, icnt = 0;
    float i_meas = 0.0f;
    int apply_at = -1;
    SimResult res = { 0 };
    res.vb_min = 1e9f;
    const int n_tail = (int)(2e-3f / dt);
    double tail_i = 0.0, tail_i2 = 0.0;  int tail_n = 0, tail_lim = 0, tail_isr = 0;
    const int hist_cap = n_end / n_isr + 2;
    float *hist_i = malloc(sizeof(float) * (size_t)hist_cap), *hist_t = malloc(sizeof(float) * (size_t)hist_cap);
    int hist_n = 0;
    for (int n = 0; n < n_end; n++)
    {
        const float t = (float)n * dt;
        const int ph = n % n_isr;
        if (ph >= n_isr - (int)(45e-6f / dt)) { vsum += vb; vcnt++; }
        if (ph >= (int)(13.9e-6f / dt) && ph < (int)(23.3e-6f / dt)) { isum += il; icnt++; }
        if (ph == 0 && n > 0)
        {
            const float v_meas = (float)(vsum / (vcnt ? vcnt : 1));
            if (k->t_ref_s > 0.0f && t >= k->t_ref_s)
            {
                if (v_ref + 0.004f < k->v_ref2) { v_ref += 0.004f; } else if (v_ref > k->v_ref2 + 0.004f) { v_ref -= 0.004f; } else { v_ref = k->v_ref2; }
            }
            const uint32_t ton = buck_ccm_update(&s, &g, &c, v_ref, v_meas, k->v_in, i_meas);
            i_meas = (float)(isum / (icnt ? icnt : 1));   /* read next cycle */
            vsum = isum = 0.0;  vcnt = icnt = 0;
            (void)ton;
            BuckCcmTiming tt;
            buck_ccm_timing_ticks(&c, s.t_on_ticks, &tt);
            d_next = (float)(tt.a_cmp2 - tt.a_cmp4) / (float)buck_ccm_ticks(2500u);
            apply_at = n + n_apply;
            if (n >= n_end - n_tail) { tail_isr++; if (s.limiting) { tail_lim++; } }
#ifdef SIM_TRACE
            if (n % (n_isr * 10) == 0) { printf("T %.2f ms v %.3f i %.3f lim %d vl %.4f u %.4f ton %u\n", 1e3f * t, v_meas, i_meas, s.limiting, s.v_lim_v, s.u_v, ton); }
#endif
            if (hist_n < hist_cap) { hist_i[hist_n] = i_meas; hist_t[hist_n] = t; hist_n++; }
        }
        if (n == apply_at) { d = d_next; }
        if (k->t_load_s > 0.0f && n == (int)(k->t_load_s / dt))
        {
            r_load = k->r_load2; v_bat = k->v_bat2; r_bat = k->r_bat2;
        }
        if (k->t_ilim_s > 0.0f && n == (int)(k->t_ilim_s / dt)) { g.ilim_a = k->ilim2_a; }
        const bool sw_on = !(t >= k->t_open_s && t < k->t_close_s);
        /* explicit update, small step */
        const float v_sw = d * k->v_in + r * vb;
        il += dt * (v_sw - vb - il * r_l) / L;
        if (k->il_cap_a > 0.0f && il > k->il_cap_a) { il = k->il_cap_a; }
        if (sw_on) { ix += dt * (vb - vx - ix * r_sw) / l_w; } else { ix = 0.0f; }
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
        vb += dt * (il - ix) / c_b;
        if (il > res.il_max) { res.il_max = il; }
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

int main(void)
{
    test_timing();
    test_law();
    test_unclip();
    test_limit();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("buck_ccm: all checks passed\n");
    return 0;
}
