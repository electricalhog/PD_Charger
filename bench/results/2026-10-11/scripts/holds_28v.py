"""28 V EPR boost under load: hold noise, dips and the load-release step, from trans3.py captures.

usage: python3 holds_28v.py   (captures in ../data/cap; writes ../metrics/holds_28v.json and
                               ../figures/u28_3w_before_after.png, ../figures/u28_releases.png)

Each capture is CH4 on TP2, AC-coupled at 0.2 V/div (the DS1104Z cannot offset a fine scale
to 28 V), 6 s at 2 MSa/s, the QT Py load held at one power target for 4.5 s.  The load
ramps its output at 5 mV/ms, so applying it is a ~1 s ramp; the release (disarm) is the
step.  Levels are relative to the record's own baseline, so the AC coupling matters only for
the DC shift between load and no load, which these numbers do not use.
  hold      1.5 to 4.0 s.  "fast" = the 20 kHz low-pass minus its 5 ms running median
            (what is left after the slow wander): rms, peak to peak, and dips below -100 mV
            and -200 mV (at least 2 ms apart).
  release   the steepest rise of the 2 kHz low-pass between 3.9 and 4.6 s.  Peak of the
            20 kHz low-pass over the next 100 ms above the median of the 45 ms before;
            the time it stays above 0.9 of that peak (the pulse-skip plateau when the
            skip band clamps it); the time until it is within 1 % (280 mV) and 0.5 % for good.
  fit       the 0.5 W release, where the peak stays under the skip band, as the step
            response of a second-order loop: dV = A (exp(-p1 t) - exp(-p2 t)) + D
            (overdamped) or A exp(-s t) sin(w t) + D (underdamped), 2 kHz low-pass,
            0 to 40 ms after the step.  Reports the better fit's zeta and natural frequency.
"""
import json
from pathlib import Path

import numpy as np
from scipy import optimize, signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
CAP = HERE.parent / "data" / "cap"
LABELS = ["u28_p3000_ac", "u28_p500_ac_vinfix", "u28_p1000_ac_vinfix", "u28_p2000_ac_vinfix", "u28_p3000_ac_vinfix"]


def load(label):
    m = json.loads((CAP / f"{label}.json").read_text())
    p = m["waveforms"]["CH4"]["preamble"]
    v = (np.fromfile(CAP / f"{label}_CH4.bin", dtype=np.uint8).astype(float) - p["yorig"] - p["yref"]) * p["yinc"]
    fs = 1 / p["xinc"]
    t = p["xorig"] + np.arange(len(v)) / fs
    return m, t, v, fs


def fit_release(tt, y):
    """Second-order step response: the better of the overdamped and underdamped fits."""
    best = None
    def over(t, a, p1, p2, d):
        return a * (np.exp(-p1 * t) - np.exp(-p2 * t)) + d
    def under(t, a, s, w, d):
        return a * np.exp(-s * t) * np.sin(w * t) + d
    for name, f, p0, bounds in (("overdamped", over, (1.0, 200, 3000, 0), ([0, 1, 1, -1], [10, 5e4, 5e5, 1])),
                                ("underdamped", under, (0.5, 500, 1500, 0), ([0, 1, 1, -1], [10, 5e4, 5e4, 1]))):
        try:
            par, _ = optimize.curve_fit(f, tt, y, p0=p0, bounds=bounds, maxfev=20000)
        except RuntimeError:
            continue
        rms = float(np.sqrt(np.mean((f(tt, *par) - y) ** 2)))
        if name == "overdamped":
            p1, p2 = sorted(par[1:3])
            wn = np.sqrt(p1 * p2)
            zeta = (p1 + p2) / (2 * wn)
        else:
            s, w = par[1], par[2]
            wn = np.hypot(s, w)
            zeta = s / wn
        r = {"model": name, "zeta": round(float(zeta), 2), "fn_hz": round(float(wn / (2 * np.pi)), 1),
             "residual_rms_mv": round(1e3 * rms, 1), "params": [round(float(x), 4) for x in par]}
        if best is None or rms < best[0]:
            best = (rms, r, f, par)
    return best


res = {}
curves = {}
for label in LABELS:
    m, t, v, fs = load(label)
    v20 = signal.sosfiltfilt(signal.butter(4, 20e3, "lp", fs=fs, output="sos"), v)
    v2 = signal.sosfiltfilt(signal.butter(4, 2e3, "lp", fs=fs, output="sos"), v)
    dec = int(fs // 100e3)
    vf, tf = v20[::dec], t[::dec]
    fast = vf - signal.medfilt(vf, 501)
    hold = (tf > 1.5) & (tf < 4.0)
    x = fast[hold]
    pk, _ = signal.find_peaks(-x, height=0.1, distance=200)
    w = (t > 3.9) & (t < 4.6)
    tr = t[w][np.argmax(np.gradient(v2[w]))]
    pre = float(np.median(v20[(t > tr - 0.05) & (t < tr - 0.005)]))
    post = (t > tr) & (t < tr + 0.1)
    y20 = v20[post] - pre
    i_pk = int(np.argmax(y20))
    tp = t[post] - tr
    above = np.where(y20 >= 0.9 * y20[i_pk])[0]
    def settle(band):
        out = np.where(np.abs(y20) > band)[0]
        return round(1e3 * float(tp[out[-1]]), 1) if len(out) else 0.0
    st = m["load"]["steps"][0]
    r = {"load_pout_mw_telemetry": st["pout_mw"]["mean"], "load_vin_mv": st["vin_mv"]["mean"],
         "hold": {"fast_rms_mv": round(1e3 * float(np.std(x)), 1), "fast_pp_mv": round(1e3 * float(np.ptp(x))),
                  "dips_over_100mv_per_s": round(len(pk) / 2.5, 1),
                  "dips_over_200mv_per_s": round(float((-x[pk] > 0.2).sum()) / 2.5, 1),
                  "deepest_dip_mv": round(1e3 * float((-x[pk]).max())) if len(pk) else 0},
         "release": {"t_s": round(float(tr), 4), "peak_mv": round(1e3 * float(y20[i_pk])),
                     "peak_pct_of_28v": round(100 * float(y20[i_pk]) / 28.0, 2),
                     "peak_at_ms": round(1e3 * float(tp[i_pk]), 2),
                     "above_90pct_of_peak_ms": round(1e3 * float(tp[above[-1]] - tp[above[0]]), 1),
                     "within_1pct_after_ms": settle(0.28), "within_0p5pct_after_ms": settle(0.14)}}
    if label == "u28_p500_ac_vinfix":
        k = (t > tr) & (t < tr + 0.04)
        tt = (t[k] - tr)[::20]
        y = (v2[k] - pre)[::20]
        best = fit_release(tt, y)
        if best:
            r["release"]["fit"] = best[1]
            curves[label] = (tt, y, best[2](tt, *best[3]))
    res[label] = r
    curves.setdefault("traces", {})[label] = (t, v20, v2, tr, pre)
    print(label, json.dumps(r))

(HERE.parent / "metrics" / "holds_28v.json").write_text(json.dumps(res, indent=1) + "\n")

# Figure 1: 3 W hold before and after the V_in sensing fix
fig, ax = plt.subplots(2, 1, figsize=(11, 5.5), sharex=True, sharey=True)
for a, label, title in ((ax[0], "u28_p3000_ac", "before: V_in from one 59 ns ADC2 sample per PID cycle"),
                        (ax[1], "u28_p3000_ac_vinfix", "after: V_in 16x oversampled, median of three")):
    t, v20, v2, tr, pre = curves["traces"][label]
    a.plot(t[::40], 1e3 * v20[::40], lw=.3)
    a.set_title(f"28 V EPR, 3 W hold ({title})", fontsize=10)
    a.set_ylabel("VBUS, AC [mV]\n20 kHz low-pass")
    a.grid(alpha=.3)
ax[1].set_xlabel("t [s] (load armed at about -0.34 s, released at about 4.14 s)")
fig.tight_layout()
fig.savefig(HERE.parent / "figures" / "u28_3w_before_after.png", dpi=90)

# Figure 2: the release step at each power, and the 0.5 W fit
fig, ax = plt.subplots(1, 2, figsize=(12, 4.2))
for label in LABELS[1:]:
    t, v20, v2, tr, pre = curves["traces"][label]
    k = (t > tr - 0.005) & (t < tr + 0.06)
    ax[0].plot(1e3 * (t[k][::20] - tr), 1e3 * (v2[k][::20] - pre), lw=1,
               label=f"{res[label]['load_pout_mw_telemetry'] / 1000:.1f} W")
ax[0].axhline(420, color="k", lw=.6, ls=":")
ax[0].text(45, 430, "skip band +420 mV", fontsize=8)
ax[0].set_xlabel("ms from release")
ax[0].set_ylabel("VBUS, AC [mV] (2 kHz low-pass)")
ax[0].set_title("Load release at 28 V (the load disarms)", fontsize=10)
ax[0].legend(fontsize=8)
ax[0].grid(alpha=.3)
if "u28_p500_ac_vinfix" in curves:
    tt, y, yf = curves["u28_p500_ac_vinfix"]
    f = res["u28_p500_ac_vinfix"]["release"]["fit"]
    ax[1].plot(1e3 * tt, 1e3 * y, lw=1, label="0.5 W release")
    ax[1].plot(1e3 * tt, 1e3 * yf, lw=1, ls="--", label=f"{f['model']} fit: zeta {f['zeta']}, fn {f['fn_hz']} Hz")
    ax[1].set_xlabel("ms from release")
    ax[1].set_title("Second-order fit, 0.5 W (under the skip band)", fontsize=10)
    ax[1].legend(fontsize=8)
    ax[1].grid(alpha=.3)
fig.tight_layout()
fig.savefig(HERE.parent / "figures" / "u28_releases.png", dpi=90)
