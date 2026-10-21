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
    uint32_t ton = buck_ccm_update(&s, &g, &c, 20.0f, 20.0f, 24.0f);
    const float want = 2500.0f * 20.0f * (1.0f - 150.0f / 10000.0f) / 24.0f;
    CHECK(fabsf((float)ton - want) <= 1.0f, "feedforward %u ns, want %.0f", ton, want);

    /* Averaged plant: V_out follows D V_in / (1 - r) less an IR drop, one
     * PID cycle late.  The integrator must take the drop out. */
    s.u_v = 0.0f;
    float v_out = 19.5f;
    for (int k = 0; k < 4000; k++)
    {
        ton = buck_ccm_update(&s, &g, &c, 20.0f, v_out, 24.0f);
        const float d = (float)ton / 2500.0f;
        v_out = d * 24.0f / (1.0f - 150.0f / 10000.0f) - 2.5f * 0.08f;
    }
    CHECK(fabsf(v_out - 20.0f) < 0.005f, "closed loop settled at %.4f V", v_out);
    CHECK(fabsf(s.u_v - 0.2f * (1.0f - 0.015f)) < 0.01f, "integrator %.3f V", s.u_v);

    /* Anti-windup: V_in too low for the setpoint pins t_on at the ceiling;
     * the integrator must not keep climbing. */
    s.u_v = 0.0f;
    for (int k = 0; k < 2000; k++) { (void)buck_ccm_update(&s, &g, &c, 20.0f, 15.0f, 18.0f); }
    CHECK(s.clamp == 1, "expected the high clamp");
    CHECK(s.u_v < 1.0f, "integrator wound up to %.2f V at the clamp", s.u_v);

    /* Initial integrator: 2.5 A x 40 mOhm + 2 x 2 V x 20 ns / 2.5 us. */
    const float u0 = buck_ccm_u_initial(&c, 2.5f, 0.040f, 2.0f);
    CHECK(fabsf(u0 - (0.1f + 0.032f)) < 1e-4f, "u0 %.4f", u0);
}

int main(void)
{
    test_timing();
    test_law();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("buck_ccm: all checks passed\n");
    return 0;
}
