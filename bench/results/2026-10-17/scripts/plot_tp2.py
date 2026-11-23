#!/usr/bin/env python3
"""TP2 scope capture (scope_step.py) over the regulator trace of the same pulse.

  plot_tp2.py data/scope_28v/tp2_28v_6ms.csv data/scope_28v/step_28v_6ms_1.csv figures/tp2_28v_6ms.png --open-ms 6.0
The scope trigger (falling through setpoint - drop) is t = 0; the trace's t = 0 is the switch opening, so
the trace is shifted by the reconnect time plus --align-ms (fixed by eye on the reconnect undershoot minima: 0.03 ms).
"""
import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("scope_csv")
ap.add_argument("trace_csv")
ap.add_argument("png")
ap.add_argument("--open-ms", type=float, required=True)
ap.add_argument("--align-ms", type=float, default=0.03)
a = ap.parse_args()

d = np.loadtxt(a.scope_csv, delimiter=",", skiprows=1)
t, v = d[:, 0] * 1e3, d[:, 1]
dt_us = (t[1] - t[0]) * 1e3
box = lambda us: np.convolve(v, np.ones(max(int(us / dt_us), 1)) / max(int(us / dt_us), 1), mode="same")
f5, f25 = box(5.0), box(2.5)
tr = np.genfromtxt(a.trace_csv, delimiter=",", names=True)
tt = tr["t_ms"] - a.open_ms - a.align_ms
vt = tr["v_out_mv"] / 1000
base = v[(t > -12) & (t < -7)].mean()
ok = (t > -12.5) & (t < 10)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 4.6), gridspec_kw={"width_ratios": [1.4, 1]})
for ax, lo, hi in ((ax1, -9, 3), (ax2, -0.3, 1.0)):
    m = (t > lo) & (t < hi)
    ax.plot(t[m], v[m], color="0.8", lw=0.5, label="TP2 raw (20 ns, 20 MHz BW)")
    ax.plot(t[m & ok], f5[m & ok], color="C0", lw=1.2, label="TP2, 5 us mean")
    ax.plot(t[m & ok], f25[m & ok], color="C2", lw=0.8, label="TP2, 2.5 us mean") if hi < 2 else None
    mt = (tt > lo) & (tt < hi)
    ax.plot(tt[mt], vt[mt], "o-", color="C3", ms=3, lw=0.8, label="regulator trace (50 us)")
    ax.axhline(base, color="k", lw=0.4, ls=":")
    ax.set_xlim(lo, hi)
    ax.set_xlabel("ms from the scope trigger (V_out falling through 26.8 V)")
    ax.set_ylabel("V")
    ax.grid(alpha=0.3)
ax1.set_ylim(23.5, 30.5)
ax2.set_ylim(23.5, 29.5)
ax1.set_title("28 V, rectifier on, %.1f ms open: whole event" % a.open_ms)
ax2.set_title("reconnect undershoot (dV_us)")
ax1.legend(loc="lower left", fontsize=8)
fig.tight_layout()
fig.savefig(a.png, dpi=130)
print(a.png)
