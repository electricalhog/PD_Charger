"""Analyse trans.py captures (data/trans.jsonl): per capture
  - TP2 (CH4, AC): averaged over one switching period (5 us), baseline the pre-trigger mean; dip / overshoot after
    the step; settling = last time it is outside +-max(40 mV, 4 sd of the pre-trigger) of the final value (last 150 us)
  - SW1 (CH1) pulses at V_in / 2 (> 300 ns): widths of the last old-mode and first new-mode pulses
  - L1 current, inferred: I_L = integral of (SW1 - SW2) / L (L1 4.7 uH, CH1 - CH3).  The probes' offset and gain
    errors integrate, so a constant voltage offset is removed per mode: in DCM so that I_L at each period's
    zero-current ring (the mean over the 1 us before each Q1 rise) is 0, in CCM so that the valleys (I_L at each
    Q1 rise) of the last 300 us have no slope.  The level is set by the DCM side (0 A).  The CCM valleys after the
    step against their late mean are the handover's error.
usage: ana.py [TAG ...]    writes data/trans_metrics.jsonl and PNG overlays (before vs after) per kind and V_out
"""
import json, pathlib, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = pathlib.Path(__file__).resolve().parents[1]
ROOT = HERE.parents[3]
L, T = 4.7e-6, 5e-6


def load(f):
    d = np.loadtxt(ROOT / f, delimiter=",", skiprows=1)
    return d[:, 0], d[:, 1], d[:, 2], d[:, 3]


def pulses(t, v, lvl):
    hi = v > lvl
    up = np.where(~hi[:-1] & hi[1:])[0] + 1
    dn = np.where(hi[:-1] & ~hi[1:])[0] + 1
    out = []
    for u in up:
        k = np.searchsorted(dn, u)
        if k < len(dn) and t[dn[k]] - t[u] > 300e-9:
            out.append((t[u], t[dn[k]] - t[u], u))
    return out


def smooth(x, n):
    return np.convolve(x, np.ones(n) / n, mode="same")


def analyse(rec):
    t, sw1, sw2, tp2 = load(rec["file"])
    dt = t[1] - t[0]
    vin = rec["v_in"]
    ps = pulses(t, sw1, vin / 2)
    rises = np.array([p[0] for p in ps]); widths = np.array([p[1] for p in ps])
    # the step: entry -> first pulse of a CCM period (SW1 never rings back up to V_out / 2 between pulses);
    # classify each period by the minimum of SW1 + SW2 between pulses: CCM's Q2 + Q3 hold SW1 ~0, SW2 ~V_out, DCM rings
    ccm = []
    for k in range(len(ps) - 1):
        a, b = ps[k][2], ps[k + 1][2]
        seg = sw1[a + int(ps[k][1] / dt) + 50: b - 25]
        ccm.append(bool(len(seg)) and seg.max() < 0.3 * rec["v_out"])
    ccm.append(ccm[-1] if ccm else False)
    ccm = np.array(ccm)
    if rec["kind"] == "entry":
        k0 = int(np.argmax(ccm)) if ccm.any() else None              # first CCM period
    else:
        k0 = int(np.argmax(~ccm)) if (~ccm).any() else None          # first DCM period (it may be a long pulse)
    t_step = rises[k0] if k0 is not None else 0.0
    # inductor current
    vl = sw1 - sw2
    il = np.cumsum(vl) * dt / L
    idx = np.array([p[2] for p in ps])
    n1us = int(1e-6 / dt)
    dcm_k = [k for k in range(1, len(ps)) if not ccm[k - 1] and (k < len(ps))]
    ccm_k = [k for k in range(len(ps)) if ccm[k]]
    # DCM: every period ends at 0 A, so the current is re-zeroed at each ring (the mean over the 1 us before each
    # Q1 rise that follows a DCM period), interpolated between rings
    an = {k: (t[idx[k] - n1us // 2], il[idx[k] - n1us: idx[k] - 10].mean()) for k in dcm_k if idx[k] - n1us > 0}
    if rec["kind"] == "entry":
        ka = k0                      # the ring just before the first CCM period
        an = {k: v for k, v in an.items() if k <= k0}
    else:
        ka = k0 + 1                  # the first ring after the first DCM period (the step's own end is measured)
        an = {k: v for k, v in an.items() if k >= k0 + 1}
    ta = np.array([v[0] for k, v in sorted(an.items())]); ia = np.array([v[1] for k, v in sorted(an.items())])
    base = np.interp(t, ta, ia)
    # CCM (and, at an exit, the first DCM period): from that ring's level, plus a drift linear in time that makes
    # the 300 us window of CCM furthest from the step average V / 25.3 ohm (charge balance, inferred)
    t0a, i0a = an[ka]
    i_load = rec["mv"] * 1e-3 / 25.3                                # setpoint / 25.3 ohm (a VD_MON outlier can move v_out)
    if rec["kind"] == "entry":
        win = (t > t[-1] - 310e-6) & (t < t[-1] - 10e-6)
        side = t >= t0a
    else:
        win = (t > t[0] + 10e-6) & (t < t[0] + 310e-6)
        side = t < t0a
    tc = t[win].mean()
    e_ccm = ((il[win] - i0a).mean() - i_load) / (tc - t0a)          # A/s
    base = np.where(side, i0a + e_ccm * (t - t0a), base)
    il_c = il - base
    e_dcm = float(np.polyfit(ta, ia, 1)[0]) if len(ta) > 2 else 0.0
    vk = np.array([(t[idx[k]], il_c[idx[k]]) for k in ccm_k])
    if rec["kind"] == "entry":
        steady = vk[vk[:, 0] > vk[-1, 0] - 300e-6, 1].mean()
        first = vk[vk[:, 0] >= t_step][:12]
    else:
        steady = vk[vk[:, 0] < vk[0, 0] + 300e-6, 1].mean()
        first = vk[vk[:, 0] < t_step][-4:]
    # TP2
    n5 = int(T / dt)
    v = smooth(tp2, n5)
    pre = (t > t[0] + 10e-6) & (t < t_step - 10e-6)
    base, sd = v[pre].mean(), v[pre].std()
    post = (t > t_step) & (t < t[-1] - 10e-6)
    fin = v[(t > t[-1] - 160e-6) & (t < t[-1] - 10e-6)].mean()
    band = max(0.040, 4 * sd)
    out_idx = np.where(post & (np.abs(v - fin) > band))[0]
    settle = (t[out_idx[-1]] - t_step) if len(out_idx) else 0.0
    # pulses around the step
    near = [(round((rises[k] - t_step) * 1e6, 2), round(widths[k] * 1e9), bool(ccm[k])) for k in range(max(0, k0 - 3), min(len(ps), k0 + 6))]
    dcm_w = [widths[k] for k in range(len(ps)) if not ccm[k]]
    m = dict(kind=rec["kind"], mv=rec["mv"], tag=rec["tag"], file=rec["file"], t_step_us=round(t_step * 1e6, 2),
             tp2_dip_mv=round((v[post].min() - base) * 1e3), tp2_over_mv=round((v[post].max() - base) * 1e3),
             tp2_final_mv=round((fin - base) * 1e3), tp2_settle_us=round(settle * 1e6), tp2_band_mv=round(band * 1e3),
             valley_steady_a=round(float(steady), 3),
             valleys_a=[round(float(x), 3) for x in first[:, 1]],
             valley_err_first_a=round(float(first[1, 1] - steady), 3) if rec["kind"] == "entry" and len(first) > 1 else None,
             dcm_ton_median_ns=round(float(np.median(dcm_w)) * 1e9) if dcm_w else None,
             pulses_near_step=near, e_dcm_v=round(e_dcm * L, 3), e_ccm_v=round(e_ccm * L, 3),
             tel={k: rec.get(k) for k in ("ccm_entry_dt_ns", "ccm_entry_iv_ma", "ccm_exit_dt_ns", "ccm_exit_iv_ma", "ccm_exit_i0_ma",
                                          "ccm_handover_timeouts", "ccm_handover", "id_ccm_corr")},
             post=dict(ccm=rec["post"]["ccm"], state=rec["post"]["state"], why=rec["post"]["why"]))
    if rec["kind"] == "exit":
        # SW1 above V_in after the last CCM period: current back through Q1's body diode (no gate before DCM's pulse)
        a = np.searchsorted(t, t_step - T); b = np.searchsorted(t, t_step + 2 * T)
        m["sw1_max_v_exit"] = round(float(sw1[a:b].max()), 2)
        m["sw1_max_v_ccm"] = round(float(sw1[: max(1, a - 1)].max()), 2)
        # first DCM pulses' widths vs the steady DCM on-time
        m["first_dcm_widths_ns"] = [round(widths[k] * 1e9) for k in range(k0, min(len(ps), k0 + 4))]
        # the last CCM period: its Q1 width, L1 at its end (the boundary, 360 ns before a CCM Q1 rise would be;
        # inferred), and the body diodes from SW2: above V_out + 1 V is Q3's (Q3 off, I_L > 0), below -1 V is
        # Q4's (I_L < 0; the refresh with Q4 on sits at -0.3 V)
        kl = k0 - 1
        tbd = rises[kl] + T - 360e-9
        jb = np.searchsorted(t, tbd)
        m["last_ccm_width_ns"] = round(widths[kl] * 1e9)
        m["il_end_a"] = round(float(il_c[jb - 10: jb + 10].mean()), 3)
        a2, b2 = np.searchsorted(t, rises[kl]), np.searchsorted(t, t_step)
        m["q3_diode_ns"] = round(float(np.sum(sw2[a2:b2] > rec["v_out"] + 1.0)) * dt * 1e9)
        m["q4_diode_ns"] = round(float(np.sum(sw2[a2:b2] < -1.0)) * dt * 1e9)
        m["q4_diode_ccm_ns"] = round(float(np.sum(sw2[a2 - int(T / dt):a2] < -1.0)) * dt * 1e9)   # the period before
    else:
        m["first_ccm_width_ns"] = round(widths[k0] * 1e9)
        m["steady_ccm_width_ns"] = round(float(np.median(widths[k0 + 20:k0 + 60])) * 1e9)
    return m, dict(t=t - t_step, v=v - base, il=il_c, sw1=sw1, sw2=sw2)


def main():
    tags = sys.argv[1:]
    recs = [json.loads(l) for l in open(HERE / "data" / "trans.jsonl")]
    recs = [r for r in recs if r.get("file") and (not tags or r["tag"] in tags)]
    # last record per (kind, mv, tag)
    last = {}
    for r in recs:
        last[(r["kind"], r["mv"], r["tag"])] = r
    res = {}
    with open(HERE / "data" / "trans_metrics.jsonl", "w") as f:
        for key, r in sorted(last.items()):
            m, w = analyse(r)
            res[key] = (m, w)
            f.write(json.dumps(m) + "\n")
            print(json.dumps({k: m[k] for k in m if k not in ("file", "tel", "pulses_near_step")}))
    for kind in ("entry", "exit"):
        for mv in sorted({k[1] for k in res if k[0] == kind}):
            pair = [(tag, res[(kind, mv, tag)]) for tag in ("before", "after") if (kind, mv, tag) in res]
            if not pair:
                continue
            fig, ax = plt.subplots(3, 1, figsize=(11, 9), sharex=False)
            col = {"before": "#c0392b", "after": "#2471a3"}
            for tag, (m, w) in pair:
                s = (w["t"] > -60e-6) & (w["t"] < 880e-6)
                ax[0].plot(w["t"][s] * 1e6, w["v"][s] * 1e3, color=col[tag], lw=1,
                           label=f"{tag}: dip {m['tp2_dip_mv']} / over +{m['tp2_over_mv']} mV, settle {m['tp2_settle_us']} us")
                z = (w["t"] > -15e-6) & (w["t"] < 40e-6)
                ax[1].plot(w["t"][z] * 1e6, w["il"][z], color=col[tag], lw=0.8, label=f"{tag}")
                ax[2].plot(w["t"][z] * 1e6, w["sw1"][z] + (0 if tag == "before" else 0), color=col[tag], lw=0.6,
                           label=f"{tag} SW1")
            ax[0].set_ylabel("TP2 AC, 5 us mean [mV]"); ax[0].set_xlabel("time from the first new-mode period [us]")
            ax[0].legend(fontsize=8); ax[0].grid(alpha=0.3)
            ax[1].set_ylabel("I_L inferred [A]"); ax[1].axhline(0, color="k", lw=0.5); ax[1].legend(fontsize=8); ax[1].grid(alpha=0.3)
            ax[2].set_ylabel("SW1 [V]"); ax[2].set_xlabel("time [us]"); ax[2].legend(fontsize=8); ax[2].grid(alpha=0.3)
            fig.suptitle(f"Buck CCM {kind} at {mv / 1000:g} V, 25.3 ohm, 200 kHz: before (red) vs after (blue)")
            fig.tight_layout()
            fig.savefig(HERE / f"{kind}_{mv // 1000}V.png", dpi=110)
            plt.close(fig)


if __name__ == "__main__":
    main()
