#!/usr/bin/env python3
"""TP2 scope capture of the 28 V, SR-on, 6 ms pulse (README step 10) -> metrics/tp2_28v_6ms.json.

  tp2_metrics.py [data/scope_28v/tp2_28v_6ms.csv] [data/scope_28v/step_28v_6ms_1.csv]

Scope t = 0 is the scope trigger (falling through 26.8 V), which lands on the reconnect undershoot.  Baseline = mean of
-12...-7 ms.  A k-us mean is the boxcar mean of k us of 20 ns samples, plotted at the window centre.  dV_us = baseline
minus the lowest k-us mean in 0...1 ms after the trigger.  Recovery = last 5 us mean outside +-2 % / +-1 % of the
baseline in 0...10 ms.  The regulator trace of the same pulse supplies the skipping/railed samples.
"""
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
R = round


def box(x, n):
    c = np.cumsum(np.insert(x, 0, 0.0))
    return (c[n:] - c[:-n]) / n


def main():
    scope = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "data/scope_28v/tp2_28v_6ms.csv"
    trace = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "data/scope_28v/step_28v_6ms_1.csv"
    d = np.loadtxt(scope, delimiter=",", skiprows=1)
    t, v = d[:, 0] * 1e3, d[:, 1]
    dt_us = (t[1] - t[0]) * 1e3
    codes = np.unique(v)
    m = (t >= -12) & (t < -7)
    base = float(v[m].mean())
    out = {"record": {"samples": len(v), "sample_us": R(dt_us, 3), "t_start_ms": R(float(t[0]), 2), "t_end_ms": R(float(t[-1]), 2),
                      "codes_used": len(codes), "quantum_v": R(float(np.median(np.diff(codes))), 3),
                      "lowest_v": R(float(v.min()), 3), "highest_v": R(float(v.max()), 3)},
           "baseline": {"window_ms": [-12, -7], "mean_v": R(base, 3), "raw_sd_v": R(float(v[m].std()), 3),
                        "raw_pp_v": R(float(np.ptp(v[m])), 3)}}
    wins = {}
    for us in (1, 2.5, 5, 10, 20, 50):
        n = int(round(us / dt_us))
        b = box(v, n)
        tc = t[:len(b)] + n * dt_us / 2e3
        i = int(np.argmin(np.where((tc >= 0) & (tc < 1.0), b, 1e9)))
        wins[f"{us:g}_us"] = {"min_v": R(float(b[i]), 3), "at_us_after_trigger": R(float(tc[i]) * 1e3, 1),
                              "dv_us_v": R(base - float(b[i]), 3), "dv_us_pct": R(100 * (base - float(b[i])) / base, 1)}
    out["undershoot_by_averaging_window"] = wins
    n5 = int(round(5 / dt_us))
    b5 = box(v, n5)
    tc5 = t[:len(b5)] + n5 * dt_us / 2e3
    rec = {}
    for pct in (2, 1):
        outside = (np.abs(b5 - base) > base * pct / 100) & (tc5 >= 0) & (tc5 < 10)
        rec[f"within_{pct}_pct_ms_after_trigger"] = R(float(tc5[np.where(outside)[0].max()]), 3)
        rec[f"band_{pct}_pct_v"] = R(base * pct / 100, 3)
    out["recovery_5us_mean"] = rec
    ripple = []
    for lo in np.arange(-13.0, 0.0, 0.5):
        w, r = (tc5 >= lo) & (tc5 < lo + 0.5), (t >= lo) & (t < lo + 0.5)
        ripple.append({"from_ms": float(lo), "pp_5us_mean_v": R(float(np.ptp(b5[w])), 3), "sd_5us_mean_v": R(float(b5[w].std()), 3),
                       "raw_sd_v": R(float(v[r].std()), 3), "raw_pp_v": R(float(np.ptp(v[r])), 2)})
    out["windowed_ripple_0p5ms"] = ripple
    fine = []
    for lo in np.round(np.arange(-13.0, -0.35, 0.1), 1):
        w = (tc5 >= lo) & (tc5 < lo + 0.1)
        fine.append((float(lo), float(np.ptp(b5[w])), float(b5[w].std())))
    quiet = np.array([f[1] for f in fine if f[0] < -3.0])
    out["ripple_onset_0p1ms"] = {
        "quiet_windows_ms": [-13.0, -3.0], "quiet_pp_5us_mean_v": {"median": R(float(np.median(quiet)), 3), "p90": R(float(np.percentile(quiet, 90)), 3), "max": R(float(quiet.max()), 3), "n": len(quiet)},
        "windows": [{"from_ms": lo, "pp_5us_mean_v": R(pp, 3), "sd_5us_mean_v": R(sd, 3)} for lo, pp, sd in fine if -2.4 <= lo <= -1.0]}
    tr = np.genfromtxt(trace, delimiter=",", names=True, dtype=None, encoding=None)
    pre = tr["t_ms"] < 0
    summ = json.loads((trace.parent / "summary_28v_6ms.json").read_text())["steps"][0]
    sk = tr["t_ms"][tr["skipping"] > 0]
    sel = (tr["t_ms"] >= 5.95) & (tr["t_ms"] <= 6.3 + 1e-9)
    out["regulator_trace"] = {"v0_mv": R(float(tr["v_out_mv"][pre].mean())), "reconnect_sample_ms": summ["reconnect_sample_ms"],
                              "pulse_us_measured": summ["pulse_us_measured"], "skipping_samples_ms": [R(float(x), 2) for x in sk],
                              "railed_samples": int(((tr["i_cmd_ma"] >= tr["i_max_ma"] - 2) & (tr["t_ms"] >= 0)).sum()),
                              "i_cmd_max_ma": int(tr["i_cmd_ma"].max()),
                              "v_out_mv_5p95_to_6p30_ms": [int(x) for x in tr["v_out_mv"][sel]]}
    p = HERE / "metrics" / "tp2_28v_6ms.json"
    p.write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1)[:3500])


if __name__ == "__main__":
    main()
