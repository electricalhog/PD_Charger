/**
 * @file    buck_ccm.c
 * @brief   Buck forced-CCM modulator (buck_ccm.h, plans/ccm-buck-50w.md).
 */

#include "buck_ccm.h"

#include <math.h>

uint32_t buck_ccm_max_on_ns(const BuckCcmTimingCfg *c)
{
    const uint32_t used = c->pulse_start_ns + c->dt_ns + c->min_q2_end_ns;
    return (c->period_ns > used) ? (c->period_ns - used) : 0u;
}

float buck_ccm_refresh_share(const BuckCcmTimingCfg *c)
{
    return (c->refresh_interval_ns != 0u) ? ((float)c->refresh_ns / (float)c->refresh_interval_ns) : 0.0f;
}

bool buck_ccm_cfg_valid(const BuckCcmTimingCfg *c)
{
    if (c->period_ns == 0u || c->dt_ns < 10u || c->refresh_ns == 0u)                  { return false; }
    if (c->refresh_interval_ns % c->period_ns != 0u)                                   { return false; }
    if (buck_ccm_ticks(c->refresh_interval_ns) > BUCK_CCM_MAX_COMPARE)                 { return false; }
    /* Q2 off must come a dead time after Q3 is back on (the refresh ends
     * inside Q2's on-time), and Q2's off edge must be a valid compare. */
    if (c->refresh_ns + 2u * c->dt_ns >= c->pulse_start_ns)                            { return false; }
    if (buck_ccm_ticks(c->refresh_ns) < BUCK_CCM_MIN_COMPARE)                          { return false; }
    if (buck_ccm_ticks(c->pulse_start_ns - c->dt_ns) < BUCK_CCM_MIN_COMPARE)           { return false; }
    /* Q2 must be on before Q3 turns off ahead of a refresh. */
    if (c->min_q2_end_ns <= c->dt_ns)                                                  { return false; }
    if (buck_ccm_max_on_ns(c) < c->min_on_ns || c->min_on_ns == 0u)                    { return false; }
    return true;
}

void buck_ccm_timing_ticks(const BuckCcmTimingCfg *c, uint32_t t_on_ticks, BuckCcmTiming *out)
{
    const uint32_t max_on = buck_ccm_ticks(buck_ccm_max_on_ns(c));
    const uint32_t min_on = buck_ccm_ticks(c->min_on_ns);
    if (t_on_ticks > max_on) { t_on_ticks = max_on; }
    if (t_on_ticks < min_on) { t_on_ticks = min_on; }
    const uint32_t dt = buck_ccm_ticks(c->dt_ns);
    out->t_on_ns = (t_on_ticks * 1000u + BUCK_CCM_TICKS_PER_US / 2u) / BUCK_CCM_TICKS_PER_US;
    out->a_cmp4  = buck_ccm_ticks(c->pulse_start_ns);
    out->a_cmp2  = out->a_cmp4 + t_on_ticks;
    out->c_cmp2  = out->a_cmp2 + dt;
    out->a_cmp3  = out->a_cmp4 - dt;
    out->b_per   = buck_ccm_ticks(c->refresh_interval_ns);
    out->b_cmp2  = out->b_per - dt;
    out->b_cmp3  = buck_ccm_ticks(c->refresh_ns);
    out->b_cmp4  = out->b_cmp3 + dt;
}

void buck_ccm_timing(const BuckCcmTimingCfg *c, uint32_t t_on_ns, BuckCcmTiming *out)
{
    buck_ccm_timing_ticks(c, buck_ccm_ticks(t_on_ns), out);
}

float buck_ccm_u_initial(const BuckCcmTimingCfg *c, float i_load_a, float r_series_ohm, float vsd_v)
{
    if (i_load_a < 0.0f) { i_load_a = 0.0f; }
    return i_load_a * r_series_ohm + 2.0f * vsd_v * (float)c->dt_ns / (float)c->period_ns;
}

uint32_t buck_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                         float v_ref_v, float v_out_v, float v_in_v, float i_a)
{
    const float e = v_ref_v - v_out_v;
    float u = s->u_v + g->ki * e * g->dt_s;
    if (u > g->u_max_v) { u = g->u_max_v; }
    if (u < g->u_min_v) { u = g->u_min_v; }
    if (v_in_v < 1.0f) { v_in_v = 1.0f; }

    /* Current limit (buck_ccm.h): the reference the duty is built on,
     * v_lim, integrates down while I is over the limit and back up while it
     * is under, and hands back to V_ref once it reaches it. */
    float v_cmd = v_ref_v;
    uint8_t lim = 0u;
    if (g->ilim_a > 0.0f && (s->limiting || i_a > g->ilim_a))
    {
        /* Entry starts at the measured V_out (a load step has already
         * pulled it down), never above V_ref. */
        float v_lim = s->limiting ? s->v_lim_v : ((v_out_v < v_ref_v) ? v_out_v : v_ref_v);
        const float ei = g->ilim_a - i_a;
        /* no integration into a duty clamp it is pushing against */
        if (!((s->clamp > 0 && ei > 0.0f) || (s->clamp < 0 && ei < 0.0f)))
        {
            v_lim += g->kl * ei * g->dt_s;
        }
        if (v_lim < 0.0f) { v_lim = 0.0f; }
        if (v_lim < v_ref_v)
        {
            v_cmd = v_lim;
            lim   = 1u;
            u     = s->u_v;   /* anti-windup: the voltage integrator holds */
        }
        s->v_lim_v = v_lim;
    }

    const float r1    = 1.0f - buck_ccm_refresh_share(c);
    const float v_ff  = v_cmd * r1;
    const float n     = v_ff + (lim ? 0.0f : g->kp * e) + u;
    float t_on = (float)c->period_ns * n / v_in_v;
    const float max_on = (float)buck_ccm_max_on_ns(c);
    const float min_on = (float)c->min_on_ns;
    int8_t clamp = 0;
    if (t_on >= max_on)      { t_on = max_on; clamp = 1; }
    else if (t_on <= min_on) { t_on = min_on; clamp = -1; }

    /* At a clamp the integrator tracks the value that just reaches it
     * (back-calculation), so it unwinds from the boundary when the error
     * turns: holding the old value left it up to the full ki e dt per cycle
     * above, and a 20 V reconnect at 400 kHz (duty ceiling 0.86, need 0.82)
     * overshot 1.7 V on the way back (bench 2026-10-20). */
    if (!lim && ((clamp > 0 && e > 0.0f) || (clamp < 0 && e < 0.0f)))
    {
        const float u_edge = t_on * v_in_v / (float)c->period_ns - v_ff - g->kp * e;   /* lim == 0: v_ff on V_ref */
        u = (u_edge < s->u_v) == (clamp > 0) ? u_edge : s->u_v;
        if (u > g->u_max_v) { u = g->u_max_v; }
        if (u < g->u_min_v) { u = g->u_min_v; }
    }
    s->u_v        = u;
    s->limiting   = lim;
    s->clamp      = clamp;
    s->t_on_ticks = (uint32_t)(t_on * (float)BUCK_CCM_TICKS_PER_US * 1e-3f + 0.5f);
    return (uint32_t)(t_on + 0.5f);
}

float buck_ccm_ripple_a(float v_in_v, float v_out_v, uint32_t t_on_ns, float l_h)
{
    const float dv = v_in_v - v_out_v;
    if (dv <= 0.0f || l_h <= 0.0f) { return 0.0f; }
    return dv * (float)t_on_ns * 1e-9f / l_h;
}

float buck_ccm_unclip_a(float m_a, float ripple_a)
{
    if (ripple_a <= 1e-6f || 2.0f * m_a >= ripple_a) { return m_a; }
    if (m_a <= 0.0f)                                  { return -0.5f * ripple_a; }
    return sqrtf(2.0f * ripple_a * m_a) - 0.5f * ripple_a;
}
