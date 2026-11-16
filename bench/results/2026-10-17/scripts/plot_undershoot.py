#!/usr/bin/env python3
"""Reconnect undershoot, recovery and release overshoot against T_unl for every pulse.

  plot_undershoot.py [report/pulse_metrics.csv [report/undershoot_summary.png]]

Terms: report/TEST_REPORT.md section 2. Hollow markers are sub-release pulses (T_unl <= 0.25 ms: the load never
left the node), so they show the background excursion E_bg. Dotted and dashed lines are the USB PD vSrcNew and
vSrcValid depths below (A) and the vSrcNew height above (C) the nominal voltage, as for the indicative comparison
in report section 6.7. Values are single 50 us ADC samples.
"""
import csv
import sys
from pathlib import Path

import matplotlib
import matplotlib.ticker
matplotlib.use("Agg")
import matplotlib.pyplot as plt

DAY = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1]) if len(sys.argv) > 1 else DAY / "report" / "pulse_metrics.csv"
dst = Path(sys.argv[2]) if len(sys.argv) > 2 else DAY / "report" / "undershoot_summary.png"

STYLE = {(5.0, "off"): ("tab:red", "o", "5 V"), (20.0, "off"): ("tab:blue", "s", "20 V"),
         (28.0, "off"): ("tab:green", "^", "28 V, SR off"), (28.0, "on"): ("tab:olive", "v", "28 V, SR on")}
NOMINAL = {5.0: "tab:red", 20.0: "tab:blue", 28.0: "tab:green"}

rows = list(csv.DictReader(open(src)))
fig, ax = plt.subplots(1, 3, figsize=(15.5, 4.8))
for key, (c, m, lab) in STYLE.items():
    g = [r for r in rows if (float(r["contract_v"]), r["rectifier"]) == key]
    for sub, kw in ((False, dict(color=c)), (True, dict(color=c, mfc="none", alpha=.55))):
        pts = [r for r in g if (r["kind"] == "sub-release") == sub]
        t = [float(r["t_unl_ms"]) for r in pts]
        ax[0].plot(t, [float(r["dv_us_mv"]) / 1000 for r in pts], m, label=None if sub else lab, **kw)
        ax[2].plot(t, [float(r["dv_os_mv"]) / 1000 for r in pts], m, **kw)
        if not sub:
            rec = [(float(r["t_unl_ms"]), float(r["rec2_ms"])) for r in pts if r["rec2_ms"] and float(r["rec2_ms"]) > 0]
            ax[1].plot([p[0] for p in rec], [p[1] for p in rec], m, color=c)
for v, c in NOMINAL.items():
    ax[0].axhline(0.05 * v, color=c, ls=":", lw=1)
    ax[0].axhline(0.05 * v + 0.5, color=c, ls="--", lw=1)
    ax[2].axhline(0.05 * v, color=c, ls=":", lw=1)
ax[0].set_title("A. ΔV_us (dotted: vSrcNew depth, dashed: vSrcValid depth)", fontsize=9)
ax[0].set_ylabel("ΔV_us = V_0 − min V_out after reconnect (V)")
ax[1].set_title("B. t_rec(2 %) of events that left the ±2 % band", fontsize=9)
ax[1].set_ylabel("t_rec(2 %) after OUTPUT_EN rises (ms)")
ax[1].set_yscale("log")
ax[1].set_yticks([0.1, 0.2, 0.5, 1, 2, 5, 10, 20])
ax[1].get_yaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
ax[1].minorticks_off()
ax[2].set_title("C. ΔV_os (dotted: vSrcNew height, +5 % of V_0)", fontsize=9)
ax[2].set_ylabel("ΔV_os = max V_out while open − V_0 (V)")
for a in ax:
    a.set_xlabel("T_unl = T_cmd − T_release (ms)")
    a.grid(alpha=.3)
ax[0].set_xlim(-3.3, 3.9)
ax[2].set_xlim(-3.3, 3.9)
ax[1].set_xlim(0.0, 3.9)
ax[0].legend(fontsize=8, loc="upper left")
fig.text(0.01, 0.005, "Hollow: sub-release pulses (T_unl ≤ 0.25 ms), the background floor E_bg. 50 µs ADC samples, n = 1–2 per point, one board. "
         "T_release: 5 V 1.35, 20 V 4.15, 28 V 3.90 (SR on 4.20) ms; ±0.15 ms. Limits are indicative, at the converter node.", fontsize=7.5)
fig.tight_layout(rect=(0, 0.03, 1, 1))
fig.savefig(dst, dpi=130)
print("wrote", dst)
