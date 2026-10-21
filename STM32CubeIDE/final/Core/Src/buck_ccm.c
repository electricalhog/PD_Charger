/**
 * @file    buck_ccm.c
 * @brief   Buck forced-CCM modulator (buck_ccm.h, plans/ccm-buck-50w.md).
 */

#include "buck_ccm.h"

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

void buck_ccm_timing(const BuckCcmTimingCfg *c, uint32_t t_on_ns, BuckCcmTiming *out)
{
    const uint32_t max_on = buck_ccm_max_on_ns(c);
    if (t_on_ns > max_on)       { t_on_ns = max_on; }
    if (t_on_ns < c->min_on_ns) { t_on_ns = c->min_on_ns; }
    const uint32_t dt = buck_ccm_ticks(c->dt_ns);
    out->t_on_ns = t_on_ns;
    out->a_cmp2  = buck_ccm_ticks(c->pulse_start_ns + t_on_ns);
    out->c_cmp2  = out->a_cmp2 + dt;
    out->a_cmp3  = buck_ccm_ticks(c->pulse_start_ns) - dt;
    out->b_per   = buck_ccm_ticks(c->refresh_interval_ns);
    out->b_cmp2  = out->b_per - dt;
    out->b_cmp3  = buck_ccm_ticks(c->refresh_ns);
    out->b_cmp4  = out->b_cmp3 + dt;
}

float buck_ccm_u_initial(const BuckCcmTimingCfg *c, float i_load_a, float r_series_ohm, float vsd_v)
{
    if (i_load_a < 0.0f) { i_load_a = 0.0f; }
    return i_load_a * r_series_ohm + 2.0f * vsd_v * (float)c->dt_ns / (float)c->period_ns;
}

uint32_t buck_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                         float v_ref_v, float v_out_v, float v_in_v)
{
    const float e = v_ref_v - v_out_v;
    float u = s->u_v + g->ki * e * g->dt_s;
    if (u > g->u_max_v) { u = g->u_max_v; }
    if (u < g->u_min_v) { u = g->u_min_v; }
    if (v_in_v < 1.0f) { v_in_v = 1.0f; }

    const float v_ff  = v_ref_v * (1.0f - buck_ccm_refresh_share(c));
    float t_on = (float)c->period_ns * (v_ff + g->kp * e + u) / v_in_v;
    const float max_on = (float)buck_ccm_max_on_ns(c);
    const float min_on = (float)c->min_on_ns;
    int8_t clamp = 0;
    if (t_on >= max_on)      { t_on = max_on; clamp = 1; }
    else if (t_on <= min_on) { t_on = min_on; clamp = -1; }

    /* Conditional integration: at a clamp the integrator keeps its value
     * when the error pushes further into it. */
    if ((clamp > 0 && e > 0.0f) || (clamp < 0 && e < 0.0f)) { u = s->u_v; }
    s->u_v   = u;
    s->clamp = clamp;
    return (uint32_t)(t_on + 0.5f);
}
