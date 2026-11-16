"""Boost -> band in CCM on the way down, caught on the scope (bench 2026-11-14).

The change happens inside buck-boost mode, so the regulator trace has no trigger for it (mode never changes; an SWD
command takes ~0.4 s, so a 100 ms trace cannot be timed to the step).  Here the scope does it: one START at 27 V
(auto CCM, boost kind) into the 25.3 ohm, then the ceiling to 24 V; the scope is armed single-shot on V_out (CH4)
falling through 25.8 V (HF reject) at 2 ms/div, 300k points per channel (12.5 MSa/s), the trigger 6 ms from the
left, so the whole 27 -> 24 V ramp at 80 V/s and the switch (between 25.6 and 25.2 V in the sweep) are inside.

THE 25.3 OHM ON VBUS, NO PACK (the 5 V START check first).  Without --go it prints the plan only.

Run from the repository root:
  uv run --quiet --project tools/bringup python bench/results/2026-11-14/scripts/boost_band_scope.py --go
Output: bench/results/2026-11-14/boost_band_<time>.json, captures/boost_band_<time>.csv.gz,
figures/boost_band_<time>.png (plot with --plot FILE.json, system python3).
"""

from __future__ import annotations

import argparse
import gzip
import json
import shutil
import signal
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-12" / "scripts"))
sys.path.insert(0, str(HERE.parents[1] / "2026-11-13" / "scripts"))

R_OHM, ILIM_MA, FROM_MV, TO_MV, TRIG_V = 25.3, 2000, 27000, 24000, 25.8
D = HERE.parent


def run() -> int:
    import boost_cal as bc
    import transitions as tr
    from bringup.config import REPO_ROOT
    from bringup.scope import Scope
    root = Path(REPO_ROOT)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = D / f"boost_band_{stamp}.json"
    interrupted = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: interrupted.set())
    b = bc.Board()
    if b.status()["state"] != "IDLE":
        print("regulator not IDLE: stopping"); return 1
    keep = {k: b.read(k, t) for k, t in (("regulator_ccm_enable", "u8"), ("regulator_ccm_ilim_ma", "u16"))}
    res = {"started": time.strftime("%F %T"), "from_mv": FROM_MV, "to_mv": TO_MV, "trigger_v": TRIG_V}
    try:
        with Scope(b.cfg) as s:
            s.channel(1, display=True, coupling="DC", probe=10, scale=5, offset=-17, bwl="OFF")
            s.channel(3, display=True, coupling="DC", probe=10, scale=5, offset=-17, bwl="OFF")
            s.channel(2, display=True, coupling="DC", probe=10, scale=2, offset=-24, bwl="20M")
            s.channel(4, display=True, coupling="DC", probe=10, scale=1, offset=-25.5, bwl="20M")
            s.write(":RUN"); s.write(":ACQuire:MDEPth 300000")
            s.timebase(scale=2e-3, offset=6e-3)
            s.trigger(source="CHAN4", slope="NEG", level=TRIG_V, sweep="SINGLE", coupling="HFR")
            res["vbus_check"] = tr.vbus_free(b)
            time.sleep(1.0)
            b.write("regulator_ccm_enable", bc.CCM_ENABLE["auto"], "u8")
            b.write("regulator_ccm_ilim_ramp_ma_s", 0, "u16")
            b.write("regulator_ccm_ilim_target_ma", ILIM_MA, "u16")
            n0 = {k: b.read(k, "u32") for k in ("regulator_ccm_entries", "regulator_ccm_exits",
                                                  "regulator_ccm_kind_switches")}

            def during(lock, done, t0):
                while time.monotonic() - t0 < 1.8 and not done.is_set():
                    time.sleep(0.02)
                s.write(":SINGle"); time.sleep(0.4)
                st0 = s.query(":TRIGger:STATus?")
                with lock:
                    r = b.command("set-voltage", mv=TO_MV)
                t_set = round(time.monotonic() - t0, 3)
                w = s.wait_trigger(timeout=2.5, arm=False)
                return {"armed_status": st0, "set": r.get("result"), "t_set": t_set, "scope": w}

            run_ = bc.run_step(b, FROM_MV, ILIM_MA, 4.5, bc.abort_for_resistor(FROM_MV, R_OHM), interrupted,
                               during=during)
            res["rows"] = run_.pop("rows"); res["run"] = run_
            res["counts"] = {k.replace("regulator_", ""): b.read(k, "u32") - n0[k] for k in n0}
            print(f"  run ok {run_['ok']}, {run_['end_state']} {run_['end_fault']}, abort {run_['abort']}, "
                  f"counts {res['counts']}, during {run_['extra']}", flush=True)
            if (run_.get("extra") or {}).get("scope", {}).get("triggered"):
                c = s.capture(["CHAN1", "CHAN2", "CHAN3", "CHAN4"], mode="raw")
                dst = D / "captures" / f"boost_band_{stamp}.csv.gz"
                with open(root / c["file"], "rb") as fi, gzip.open(dst, "wb") as fo:
                    shutil.copyfileobj(fi, fo)
                (root / c["file"]).unlink()
                res["capture"] = {"file": dst.name, "sample_rate_sa_s": c.get("sample_rate_sa_s"),
                                  "warnings": c.get("warnings", [])}
            s.trigger(sweep="AUTO", coupling="DC"); s.write(":RUN")
    finally:
        try:
            if b.status()["state"] == "RUNNING":
                b.command("stop")
        finally:
            for k, v in keep.items():
                b.write(k, v, "u8" if k == "regulator_ccm_enable" else "u16")
            res["ended"] = time.strftime("%F %T")
            out.write_text(json.dumps(res, indent=1))
    print(f"  -> {out}")
    return 0 if res.get("capture") else 1


def plot(js: Path) -> None:
    import numpy as np
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    res = json.loads(js.read_text())
    d = np.genfromtxt(D / "captures" / res["capture"]["file"], delimiter=",", names=True)
    t = d["t_s"] * 1e3; sw1, vin, sw2, vout = d["CHAN1"], d["CHAN2"], d["CHAN3"], d["CHAN4"]
    dt = float(np.median(np.diff(d["t_s"])))
    # SW1 goes low once a period in both: boost's bootstrap refresh (~300 ns, 4 samples at 80 ns), band's Q2
    # window (~640 ns, 8 samples).  The switch is the first low run of 6 samples or more.
    e = np.diff(np.r_[0, (sw1 < 5.0).astype(int), 0]); st = np.flatnonzero(e == 1); ln = np.flatnonzero(e == -1) - st
    long_ = np.flatnonzero(ln >= 6)
    k_band = int(st[long_[0]]) if len(long_) else None
    t_sw = float(t[k_band]) if k_band is not None else None
    # V_out smoothed over 25 us (5 periods)
    n = max(1, int(25e-6 / dt)); vs = np.convolve(vout, np.ones(n) / n, "same")
    SURF, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9"
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.edgecolor": GRID,
                         "axes.labelcolor": INK2, "xtick.color": MUTED, "ytick.color": MUTED,
                         "xtick.labelcolor": INK2, "ytick.labelcolor": INK2, "axes.spines.top": False,
                         "axes.spines.right": False, "figure.facecolor": SURF, "axes.facecolor": SURF,
                         "savefig.facecolor": SURF, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6})
    fig = plt.figure(figsize=(13, 8.4))
    gs = fig.add_gridspec(3, 2, height_ratios=[2.2, 1, 1])
    a = fig.add_subplot(gs[0, :])
    # the raw samples carry +-2 V of switching pickup on the probe's ground lead (and clip at the screen edge):
    # one-period (5 us) and five-period (25 us) means only
    n1 = max(1, int(5e-6 / dt)); v1 = np.convolve(vout, np.ones(n1) / n1, "same")
    a.plot(t[n:-n], v1[n:-n], color="#c9c7c0", lw=0.6, label="V_out, 5 us mean (one period)")
    a.plot(t[n:-n], vs[n:-n], color="#6250d6", lw=1.8, label="V_out, 25 us mean")
    a.set_ylabel("V_out (V)"); a.set_xlabel("Time from V_out falling through 25.8 V (ms)")
    out = {}
    if t_sw is not None:
        a.axvline(t_sw, color=INK2, lw=0.9, ls=(0, (3, 2)))
        a.text(t_sw + 0.15, a.get_ylim()[1] - 0.2, f"boost -> band (SW1 low pulse 4 -> 8 samples)\nat {t_sw:.2f} ms, "
               f"V_out {vs[k_band]:.2f} V", va="top", color=INK, fontsize=9.5)
        def win(t0, t1):
            m = (t >= t0) & (t <= t1); return vs[m]
        pre, post = win(t_sw - 2.0, t_sw - 0.05), win(t_sw + 0.05, t_sw + 2.0)
        slope = np.polyfit(t[(t >= t_sw - 2) & (t <= t_sw - 0.05)], pre, 1)
        fit_post = np.polyval(slope, t[(t >= t_sw + 0.05) & (t <= t_sw + 2.0)])
        dev = post - fit_post
        out = {"t_switch_ms": t_sw, "v_out_at_switch": float(vs[k_band]),
               "ramp_v_per_s_before": float(slope[0] * 1e3),
               "dev_from_ramp_2ms_after_mv": [float(dev.min() * 1e3), float(dev.max() * 1e3)]}
    a.legend(loc="upper right", frameon=False, labelcolor=INK2)
    a.set_title("Boost -> band in CCM on the way down: 27 -> 24 V at 80 V/s into 25.3 ohm, 24.6 V in "
                "(scope, single shot)", loc="left", color=INK, fontsize=11.5)
    for j, (lab, t0) in enumerate((("boost, before", (t_sw or 0) - 0.03), ("band, after", (t_sw or 0) + 0.02))):
        m = (t >= t0) & (t <= t0 + 0.02)
        for i, (sig, nm, col) in enumerate(((sw1, "SW1", "#1baf7a"), (sw2, "SW2", "#6250d6"))):
            ax = fig.add_subplot(gs[1 + i, j])
            ax.plot((t[m] - t0) * 1e3, sig[m], color=col, lw=1.0)
            ax.set_ylim(-6, 36); ax.set_ylabel(f"{nm} (V)")
            if i == 0: ax.set_title(f"{lab}: {t0:.2f} ms + 20 us", loc="left", color=INK, fontsize=10)
            if i == 1: ax.set_xlabel("Time (us)")
    fig.text(0.01, 0.005, "Bottom rows: 20 us of each switch node at 12.5 MSa/s (80 ns per sample, so edges are "
             "coarse).  Scope V_out reads about 0.45 V under the board's (2026-11-14 sweep).\nThe switch time is the first SW1 low pulse of 6 samples or more (boost's refresh pulse is 4, "
             "band's Q2 window 8).", color=MUTED, fontsize=8)
    fig.tight_layout(rect=(0, 0.02, 1, 1))
    (D / "figures").mkdir(exist_ok=True)
    png = D / "figures" / f"{js.stem}.png"; fig.savefig(png, dpi=150)
    res["analysis"] = out; js.write_text(json.dumps(res, indent=1))
    print(json.dumps(out, indent=1), "\n->", png)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--go", action="store_true"); p.add_argument("--plot")
    a = p.parse_args()
    if a.plot:
        plot(Path(a.plot)); sys.exit(0)
    print(f"BOOST -> BAND: START {FROM_MV / 1000:g} V into 25.3 ohm (auto CCM), ceiling to {TO_MV / 1000:g} V at "
          f"1.8 s, stop at 4.5 s (about 1.1 A, 29 W at most).  THE 25.3 OHM ON VBUS, NO PACK.")
    sys.exit(run() if a.go else 0)
