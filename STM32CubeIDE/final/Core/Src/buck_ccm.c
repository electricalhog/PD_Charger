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

float buck_ccm_boundary_a(const BuckCcmTimingCfg *c, const BuckCcmTiming *t, float i_load_a, float v_out_v,
                          float v_fw_v, float l_h)
{
    const float tick = 1e-6f / (float)BUCK_CCM_TICKS_PER_US;
    const float T = (float)c->period_ns * 1e-9f;
    float t_ref = (float)t->b_cmp3 * tick;
    if (c->refresh_interval_ns > c->period_ns)
    {
        t_ref *= (float)c->period_ns / (float)c->refresh_interval_ns;
    }
    const float a = (float)t->a_cmp4 * tick - t_ref;      /* fall before Q1 */
    const float on = (float)(t->a_cmp2 - t->a_cmp4) * tick;
    const float b = T - (float)t->a_cmp2 * tick;          /* fall after Q1 */
    const float v_f = v_out_v + v_fw_v;
    const float d1 = v_f * a / l_h, d2 = v_f * b / l_h, rise = d1 + d2;
    /* mean - boundary, segment by segment (the flat part adds nothing) */
    const float rel = (-0.5f * d1 * a + (0.5f * rise - d1) * on + 0.5f * d2 * b) / T;
    return i_load_a - rel;
}

int32_t buck_ccm_handover_ticks(float di_a, float v_in_v, float l_h)
{
    if (v_in_v < 1.0f) { v_in_v = 1.0f; }
    const float ticks = di_a * l_h / v_in_v * 1e6f * (float)BUCK_CCM_TICKS_PER_US;
    return (int32_t)((ticks >= 0.0f) ? (ticks + 0.5f) : (ticks - 0.5f));
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

void buck_ccm_reset(BuckCcmState *s, float u0_v, float v_ref_v, float v_out_v)
{
    s->u_v        = u0_v;
    s->clamp      = 0;
    s->limiting   = 0u;
    s->ramp       = 0u;
    s->slewing    = 0u;
    s->slew_on    = 0u;
    s->slew_entry = 0u;
    s->du_v       = 0.0f;
    s->blind      = 0u;
    s->v_lim_v    = v_ref_v;
    s->v_cmd_v    = v_ref_v;
    s->v_prev_v   = v_out_v;
    s->i_prev_a   = s->i_out_a;
    s->v_floor_v  = 0.0f;   /* the caller sets it per update */
}

void buck_ccm_limit_from(BuckCcmState *s, float v_ref_v, float v_out_v)
{
    /* The reference starts where V_out is and v_lim walks it up at kl (I_lim - I): the current rises to the
     * limit, or V_out reaches V_ref and the limit hands back.  Entering at V_ref instead, the voltage law
     * drove the whole gap at once, and the limit acted only once a scan read over it (2026-11-09
     * section 17: 5.3 A at a 3.5 A limit).  u keeps running meanwhile (s->ramp): held at its preset, its
     * error (the board makes about 0.5 V over the model) put V_out below v_lim, so v_lim reached V_ref under
     * the limit, handed back, and u then wound up at once (host model, 4.5 A at 3.5 A). */
    if (v_out_v < v_ref_v)
    {
        s->v_lim_v  = (v_out_v > 0.0f) ? v_out_v : 0.0f;
        s->v_cmd_v  = s->v_lim_v;
        s->limiting = 1u;
        s->ramp     = 1u;
    }
}

void buck_ccm_blind_begin(BuckCcmState *s, float v_out_v)
{
    /* At a DCM-to-CCM entry the law's first errors are readings taken across the mode change: VD_MON read
     * ~0.3 V higher within 0.1 ms of a 5 A forced entry while the current fell (bench 2026-11-13, README
     * section 10), the integrator chased it to its floor, the current dropped to ~0 for 8 ms and the two
     * integrators rang for 25 ms.  The load cannot move much in a millisecond, so the entry's guess holds it
     * better than a reading the law cannot yet trust. */
    s->blind     = 1u;
    s->v_blind_v = v_out_v;
    s->v_prev_v  = v_out_v;
}

void buck_ccm_blind_end(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c, bool boost,
                        float v_out_v)
{
    if (!s->blind) { return; }
    if (s->blind == 1u)   /* ended before the law ran blind: nothing to carry over */
    {
        s->blind    = 0u;
        s->v_prev_v = v_out_v;
        return;
    }
    float v_cmd = s->v_cmd_v;
    if (!s->limiting && (s->v_floor_v > 0.0f) && (v_cmd < s->v_floor_v)) { v_cmd = s->v_floor_v; }
    const float ref = (boost ? v_cmd : v_cmd * (1.0f - buck_ccm_refresh_share(c))) + g->kp * (v_cmd - v_out_v);
    float u = s->u_v + (s->ref_blind_v - ref);
    if (u > g->u_max_v) { u = g->u_max_v; }
    if (u < g->u_min_v) { u = g->u_min_v; }
    s->u_v      = u;
    s->blind    = 0u;
    s->v_prev_v = v_out_v;
}

void buck_ccm_limit_slew_from(BuckCcmState *s, float i_a)
{
    /* The limit loop's gain is kl over the path's incremental resistance, and into a battery over
     * milliseconds that path is stiff: an 8S pack, its wiring, fuse and ammeter, 0.03-0.04 Ohm in the
     * board-fitted model against the 0.13-0.17 Ohm of a slow fit.  Stepping the limit there from ~1.1 A
     * (forced entry) to 5 A, v_lim overran by the ~0.5 ms the newest ID_MON scan lags and the current
     * peaked 6.3-6.7 A (model; bench 2026-11-12: SW_OCP 7.4 ms after a 5 A forced entry).  Slewed, each
     * step is small against the lag, so the overrun is too; and with u running meanwhile it learns the
     * offset at the rising current, which held at u0 overshot instead on a soft path (0.17 Ohm: 6.9 A at
     * 50 A/s with u held, 5.00 A with u running; host model). */
    s->ilim_slew_a = (i_a > 0.0f) ? i_a : 0.0f;
    s->slew_on     = 1u;
    s->slew_entry  = 1u;
}

void buck_ccm_limit(BuckCcmState *s, const BuckCcmGains *g, float v_ref_v, float v_out_v, float i_a)
{
    /* v_lim integrates down while I is over the limit and back up while it
     * is under, and hands back to V_ref once it reaches it. */
    float v_cmd = v_ref_v;
    uint8_t lim = 0u;
    /* With slew_a_s > 0 the limit acted on rises no faster than that, from an entry (buck_ccm_limit_slew_from)
     * or a step of g->ilim_a alike (2026-11-13: a step mid-run meets the same stiff path as an entry); it
     * falls at the same rate when SLEW_DOWN, else at once.  Without history it starts at the limit. */
    float ilim_a = g->ilim_a;
    if (g->slew_a_s > 0.0f && ilim_a > 0.0f)
    {
        const float step = g->slew_a_s * g->dt_lim_s;
        if (!s->slew_on) { s->ilim_slew_a = ilim_a; s->slew_on = 1u; }
        if (s->ilim_slew_a < ilim_a)      { s->ilim_slew_a = (ilim_a - s->ilim_slew_a > step) ? s->ilim_slew_a + step : ilim_a; }
        else if (s->ilim_slew_a > ilim_a) { s->ilim_slew_a = (BUCK_CCM_SLEW_DOWN && s->ilim_slew_a - ilim_a > step)
                                                             ? s->ilim_slew_a - step : ilim_a; }
        ilim_a = s->ilim_slew_a;
    }
    else { s->slew_on = 0u; }
    s->slewing = s->slew_on && (s->ilim_slew_a < g->ilim_a);
    /* A climb starting mid-run while the limit acts: u has held at the limit's steady error, which drifts
     * with the operating point (the board's loss falls ~0.1 V/A), and integrating that stale error later (in
     * the climb, or at the hand-back to V_ref) put 0.6 A over a 5 A limit at 0.17 Ohm (host model).  So the
     * reference re-seats on V_out, the law's u takes the difference (s->du_v, for the caller: bumpless) and
     * u runs through the climb as after an entry. */
    if (s->slewing && !s->slew_entry && s->limiting && v_out_v < v_ref_v)
    {
        s->du_v      += s->v_lim_v - v_out_v;
        s->v_lim_v    = v_out_v;
        s->slew_entry = 1u;
    }
    if (ilim_a > 0.0f && (s->limiting || i_a > ilim_a))
    {
        /* Entry starts at the measured V_out (a load step has already
         * pulled it down), never above V_ref. */
        float v_lim = s->limiting ? s->v_lim_v : ((v_out_v < v_ref_v) ? v_out_v : v_ref_v);
        const float ei = ilim_a - i_a;
        /* no integration into a duty clamp it is pushing against */
        if (!((s->clamp > 0 && ei > 0.0f) || (s->clamp < 0 && ei < 0.0f)))
        {
            v_lim += g->kl * ei * g->dt_lim_s;
        }
        if (v_lim < 0.0f) { v_lim = 0.0f; }
        if (v_lim < v_ref_v)
        {
            v_cmd = v_lim;
            lim   = 1u;
        }
        s->v_lim_v = v_lim;
    }
    s->v_cmd_v  = v_cmd;
    s->limiting = lim;
    if (!lim || i_a >= ilim_a) { s->ramp = 0u; }
    if (!s->slewing)                      { s->slew_entry = 0u; }
    if (lim && s->slewing && s->slew_entry) { s->ramp = 1u; }   /* u runs while an entry's limit climbs */
}

uint32_t buck_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                         float v_out_v, float v_in_v)
{
    /* no-reverse floor on the reference, not the duty: the duty that
     * balances V_floor in the model makes about 0.5 V more on the board
     * (u0's offset, CCM_U0_*), so a duty floor forced current into a stiff
     * pack past the limit (bench 2026-11-09 section 11); through the law
     * the integrator's offset applies.  Not while the current limit acts:
     * its target is a positive current, and with u held a floor there kept
     * 3 A in at a 2 A limit (section 15). */
    if (s->blind) { v_out_v = s->v_blind_v; s->v_prev_v = v_out_v; }   /* open-loop window: no reading */
    float v_cmd = s->v_cmd_v;
    const bool floored = !s->limiting && (s->v_floor_v > 0.0f) && (v_cmd < s->v_floor_v);
    if (floored) { v_cmd = s->v_floor_v; }
    const float e = v_cmd - v_out_v;
    /* anti-windup: the voltage integrator holds while the limit sets v_cmd
     * (V_out sits below V_ref by design) */
    const bool hold = (s->limiting && !s->ramp) || s->blind;
    float u = hold ? s->u_v : (s->u_v + g->ki * e * g->dt_s);
    if (u > g->u_max_v) { u = g->u_max_v; }
    if (u < g->u_min_v) { u = g->u_min_v; }
    if (v_in_v < 1.0f) { v_in_v = 1.0f; }

    const float r1   = 1.0f - buck_ccm_refresh_share(c);
    const float v_ff = v_cmd * r1;
    const float d_v  = -g->kd * (v_out_v - s->v_prev_v) / g->dt_s;   /* on V_out: no kick from v_cmd */
    s->v_prev_v = v_out_v;
    const float ff_i = g->kff_l * (s->i_out_a - s->i_prev_a) / g->dt_s + g->kff_r * s->i_out_a;
    s->i_prev_a = s->i_out_a;
    float n = v_ff + g->kp * e + d_v + ff_i + u;
    s->ref_blind_v = v_ff + g->kp * e;   /* the reference term (buck_ccm_blind_end) */
    if (s->blind) { s->blind = 2u; }
    float t_on = (float)c->period_ns * n / v_in_v;
    const float max_on = (float)buck_ccm_max_on_ns(c);
    const float min_on = (float)c->min_on_ns;
    int8_t clamp = floored ? -1 : 0;
    if (t_on >= max_on)      { t_on = max_on; clamp = 1; }
    else if (t_on <= min_on) { t_on = min_on; clamp = -1; }

    /* At a clamp the integrator tracks the value that just reaches it
     * (back-calculation), so it unwinds from the boundary when the error
     * turns: holding the old value left it up to the full ki e dt per cycle
     * above, and a 20 V reconnect at 400 kHz (duty ceiling 0.86, need 0.82)
     * overshot 1.7 V on the way back (bench 2026-10-20). */
    if (!hold && ((clamp > 0 && e > 0.0f) || (clamp < 0 && e < 0.0f)))
    {
        const float u_edge = t_on * v_in_v / (float)c->period_ns - v_ff - g->kp * e - d_v - ff_i;
        u = (u_edge < s->u_v) == (clamp > 0) ? u_edge : s->u_v;
        if (u > g->u_max_v) { u = g->u_max_v; }
        if (u < g->u_min_v) { u = g->u_min_v; }
    }
    s->u_v        = u;
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

/* --- Boost forced CCM (buck_ccm.h) --------------------------------------- */

uint32_t boost_ccm_min_on_ns(const BuckCcmTimingCfg *c)
{
    return c->pulse_start_ns;
}

uint32_t boost_ccm_max_on_ns(const BuckCcmTimingCfg *c)
{
    const uint32_t used = 2u * c->dt_ns + c->min_q2_end_ns;
    uint32_t m = (c->period_ns > used) ? (c->period_ns - used) : 0u;
    const uint32_t cap = (uint32_t)((float)c->period_ns * BOOST_CCM_MAX_DUTY);
    return (m < cap) ? m : cap;
}

bool boost_ccm_cfg_valid(const BuckCcmTimingCfg *c)
{
    if (c->period_ns == 0u || c->refresh_ns == 0u)                                     { return false; }
    if (c->refresh_interval_ns != c->period_ns)                                        { return false; }
    if (buck_ccm_ticks(c->period_ns) > BUCK_CCM_MAX_COMPARE)                           { return false; }
    if (buck_ccm_ticks(c->dt_ns) < BUCK_CCM_MIN_COMPARE)                               { return false; }
    /* Q4 still on a margin after Q1 is back on */
    if (c->pulse_start_ns < c->refresh_ns + 2u * c->dt_ns + 10u)                       { return false; }
    if (c->min_q2_end_ns == 0u)                                                        { return false; }
    if (boost_ccm_max_on_ns(c) <= boost_ccm_min_on_ns(c))                              { return false; }
    return true;
}

bool band_ccm_cfg_valid(const BuckCcmTimingCfg *c)
{
    if (c->period_ns == 0u || c->refresh_ns == 0u)                                     { return false; }
    if (c->refresh_interval_ns != c->period_ns)                                        { return false; }
    if (buck_ccm_ticks(c->period_ns) > BUCK_CCM_MAX_COMPARE)                           { return false; }
    if (buck_ccm_ticks(c->dt_ns) < BUCK_CCM_MIN_COMPARE)                               { return false; }
    if (c->pulse_start_ns < c->min_on_ns || c->min_on_ns == 0u)                        { return false; }
    if (c->min_q2_end_ns == 0u)                                                        { return false; }
    /* Q1 back on before Q3's interval ends */
    if (c->refresh_ns + 2u * c->dt_ns + c->min_q2_end_ns >= c->period_ns - c->dt_ns)   { return false; }
    if (boost_ccm_max_on_ns(c) <= boost_ccm_min_on_ns(c))                              { return false; }
    return true;
}

void boost_ccm_timing_ticks(const BuckCcmTimingCfg *c, uint32_t t_on_ticks, BoostCcmTiming *out)
{
    const uint32_t max_on = buck_ccm_ticks(boost_ccm_max_on_ns(c));
    const uint32_t min_on = buck_ccm_ticks(boost_ccm_min_on_ns(c));
    if (t_on_ticks > max_on) { t_on_ticks = max_on; }
    if (t_on_ticks < min_on) { t_on_ticks = min_on; }
    const uint32_t dt = buck_ccm_ticks(c->dt_ns);
    const uint32_t per = buck_ccm_ticks(c->period_ns);
    out->t_on_ns = (t_on_ticks * 1000u + BUCK_CCM_TICKS_PER_US / 2u) / BUCK_CCM_TICKS_PER_US;
    out->a_cmp2  = dt;
    out->a_cmp3  = dt + buck_ccm_ticks(c->refresh_ns);
    out->a_cmp4  = out->a_cmp3 + dt;
    out->b_cmp2  = t_on_ticks;
    out->b_cmp3  = t_on_ticks + dt;
    out->b_cmp4  = per - dt;
}

float boost_ccm_v_min(const BuckCcmTimingCfg *c, float v_in_v)
{
    const float r = buck_ccm_refresh_share(c);
    return v_in_v * (1.0f - r) / (1.0f - (float)boost_ccm_min_on_ns(c) / (float)c->period_ns);
}

float boost_ccm_v_max(const BuckCcmTimingCfg *c, float v_in_v)
{
    const float r = buck_ccm_refresh_share(c);
    return v_in_v * (1.0f - r) / (1.0f - (float)boost_ccm_max_on_ns(c) / (float)c->period_ns);
}

uint32_t boost_ccm_update(BuckCcmState *s, const BuckCcmGains *g, const BuckCcmTimingCfg *c,
                          float v_out_v, float v_in_v)
{
    /* no-reverse floor on the reference, not while limiting (see buck_ccm_update) */
    if (s->blind) { v_out_v = s->v_blind_v; s->v_prev_v = v_out_v; }   /* open-loop window: no reading */
    float v_cmd = s->v_cmd_v;
    const bool floored = !s->limiting && (s->v_floor_v > 0.0f) && (v_cmd < s->v_floor_v);
    if (floored) { v_cmd = s->v_floor_v; }
    const float e = v_cmd - v_out_v;
    const bool hold = (s->limiting && !s->ramp) || s->blind;
    float u = hold ? s->u_v : (s->u_v + g->ki * e * g->dt_s);
    if (u > g->u_max_v) { u = g->u_max_v; }
    if (u < g->u_min_v) { u = g->u_min_v; }
    if (v_in_v < 1.0f) { v_in_v = 1.0f; }

    const float vin_r = v_in_v * (1.0f - buck_ccm_refresh_share(c));
    const float d_v  = -g->kd * (v_out_v - s->v_prev_v) / g->dt_s;
    s->v_prev_v = v_out_v;
    const float ff_i = g->kff_l * (s->i_out_a - s->i_prev_a) / g->dt_s + g->kff_r * s->i_out_a;
    s->i_prev_a = s->i_out_a;
    const float per = (float)c->period_ns;
    const float max_on = (float)boost_ccm_max_on_ns(c);
    const float min_on = (float)boost_ccm_min_on_ns(c);
    const float rest = v_cmd + g->kp * e + d_v + ff_i;   /* n = rest + u */
    s->ref_blind_v = v_cmd + g->kp * e;   /* the reference term (buck_ccm_blind_end) */
    if (s->blind) { s->blind = 2u; }
    float n = rest + u;
    float t_on;
    int8_t clamp = floored ? -1 : 0;
    if (n <= vin_r) { t_on = min_on; clamp = -1; }
    else
    {
        t_on = per * (1.0f - vin_r / n);
        if (t_on >= max_on)      { t_on = max_on; clamp = 1; }
        else if (t_on <= min_on) { t_on = min_on; clamp = -1; }
    }
    /* Back-calculation at a clamp, as buck_ccm_update. */
    if (!hold && ((clamp > 0 && e > 0.0f) || (clamp < 0 && e < 0.0f)))
    {
        const float u_edge = vin_r / (1.0f - t_on / per) - rest;
        u = (u_edge < s->u_v) == (clamp > 0) ? u_edge : s->u_v;
        if (u > g->u_max_v) { u = g->u_max_v; }
        if (u < g->u_min_v) { u = g->u_min_v; }
    }
    s->u_v        = u;
    s->clamp      = clamp;
    s->t_on_ticks = (uint32_t)(t_on * (float)BUCK_CCM_TICKS_PER_US * 1e-3f + 0.5f);
    return (uint32_t)(t_on + 0.5f);
}

float buck_ccm_exit_seed_ma(float i_avg_ma, float i_newest_ma, bool handover, float add_learned_ma,
                            float add_table_ma, float floor_ma, float i_max_ma)
{
    const bool dropped = 2.0f * i_newest_ma < i_avg_ma;
    float i0 = (i_newest_ma < i_avg_ma) ? i_newest_ma : i_avg_ma;
    if (handover)           { i0 += (add_learned_ma >= 0.0f && !dropped) ? add_learned_ma : add_table_ma; }
    else if (i0 < floor_ma) { i0 = floor_ma; }
    if (i0 > i_max_ma)      { i0 = i_max_ma; }
    if (i0 < 0.0f)          { i0 = 0.0f; }
    return i0;
}

/* ---- Buck CCM peak current mode (buck_ccm.h) ---- */

#define PCMC_RESET_HEADROOM_A 0.2f   /* pcmc_ccm_command: the ramp's start under DAC3's full scale */

void pcmc_ccm_command(float i_cmd_a, float v_in_v, float v_out_v, float l_h, uint32_t t_q1_ns, uint32_t t_on_ns,
                      uint32_t tau_ns, float i_thr_max_a, float zero_counts, float counts_per_a, uint32_t step_ns,
                      float ma_min_frac, uint32_t t_ramp_ns, PcmcCcmCmd *out)
{
    const float m1 = (v_in_v > v_out_v) ? (v_in_v - v_out_v) / l_h : 0.0f;   /* A/s */
    const float m2 = (v_out_v > 0.0f) ? v_out_v / l_h : 0.0f;
    const float d  = (v_in_v > 0.0f) ? v_out_v / v_in_v : 1.0f;
    float ma = 0.0f;
    if (m2 > m1) { ma = 1.25f * 0.5f * (m2 - m1); }
    if (d > 0.45f) { ma += 0.1f * m2; }
    if (ma < ma_min_frac * m2) { ma = ma_min_frac * m2; }
    const float t_on = (float)t_on_ns * 1e-9f;
    float i_thr = i_cmd_a + 0.5f * m1 * t_on - m1 * (float)tau_ns * 1e-9f;
    if (i_thr > i_thr_max_a) { i_thr = i_thr_max_a; }
    const float t_r = (float)t_q1_ns * 1e-9f + (float)t_ramp_ns * 1e-9f;
    /* The ramp's start within DAC3's range: less slope rather than a clipped start, which would move the
     * threshold itself (bench 2026-11-22, 20 V at 200 kHz with 0.8 m2: reset 24 A, clipped at 16 A, and the
     * command wound up to its ceiling with V_out short) */
    const float reset_max = (4095.0f - zero_counts) / counts_per_a - PCMC_RESET_HEADROOM_A;
    if (ma > 0.0f && t_r > 0.0f && i_thr + ma * t_r > reset_max)
    {
        ma = (reset_max > i_thr) ? (reset_max - i_thr) / t_r : 0.0f;
    }
    const float reset = i_thr + ma * t_r;
    float c = zero_counts + reset * counts_per_a + 0.5f;
    if (c < 0.0f)    { c = 0.0f; }
    if (c > 4095.0f) { c = 4095.0f; }
    float s = ma * (float)step_ns * 1e-9f * counts_per_a * 16.0f + 0.5f;
    if (s < 0.0f)     { s = 0.0f; }
    if (s > 65535.0f) { s = 65535.0f; }
    out->i_thr_a      = i_thr;
    out->m_a_a_per_us = ma * 1e-6f;
    out->reset_a      = reset;
    out->reset_counts = (uint32_t)c;
    out->step_12p4    = (uint32_t)s;
}

#define PCMC_CAPTURE_GUARD_TICKS 64u   /* register-read skew and capture latency */
/* A TA1-reset capture lands 67 ticks (12.3 ns) after the compare that reset
 * the output: bench 2026-11-22, 400 kHz, every pulse at the CMP2 backstop
 * 8029 captured at 8096.  Without it a backstop-ended pulse fell outside the
 * guard, the tracker never saw one, and the backstop could not move out. */
#define PCMC_CAPTURE_LATENCY_TICKS 67u

bool pcmc_ccm_capture(uint32_t cpt_b, uint32_t per, uint32_t n, uint32_t a4, uint32_t bs, uint32_t *t_on_ticks,
                      bool *at_backstop)
{
    if (per == 0u || cpt_b < per || cpt_b >= n * per) { return false; }   /* the refresh period, or not a count */
    uint32_t cpt = cpt_b % per;
    if (cpt <= a4 + PCMC_CAPTURE_LATENCY_TICKS) { return false; }
    cpt -= PCMC_CAPTURE_LATENCY_TICKS;                         /* the turn-off's compare time */
    if (cpt > bs + PCMC_CAPTURE_GUARD_TICKS) { return false; }
    *t_on_ticks  = cpt - a4;
    *at_backstop = (cpt + PCMC_CAPTURE_GUARD_TICKS / 4u >= bs);
    return true;
}

uint32_t pcmc_ccm_track(uint32_t track_ticks, uint32_t meas_ticks)
{
    if (meas_ticks >= track_ticks) { return track_ticks + (meas_ticks - track_ticks + 2u) / 4u; }
    return track_ticks - (track_ticks - meas_ticks + 2u) / 4u;
}

uint32_t pcmc_ccm_backstop(uint32_t track_ticks, uint32_t margin_ticks)
{
    const uint32_t m = track_ticks / 8u;
    return track_ticks + ((m > margin_ticks) ? m : margin_ticks);
}
