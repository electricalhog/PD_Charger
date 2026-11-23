"""Per-period switch-node timing around a VBUS dip at 28 V boost, 3 W (capture dip28_3w_b).

usage: python3 dip_periods.py [LABEL]   (default dip28_3w_b; writes ../metrics/LABEL_periods.json
                                          and ../figures/LABEL_diagnosis.png)

The capture: HF-reject trigger on a -0.3 V VBUS step (CH4, AC), CH2 SW1 and CH3 SW2 (AC,
10 V/div), 12 ms at 250 MSa/s.  Each switching period is found from SW2's falling crossing
of a level a third of the way up its range.  Q4's pulse runs until SW2 rises back through
that level; the Q3 body-diode conduction that follows is the time SW2 stays at its
post-pulse plateau (with 0.57 V per count at 10 V/div, the plateau is flat to a count or
two, so from reaching to leaving 1.2 V around its median over 0.3 to 0.5 us after the
edge).  In DCM boost the
inductor's peak current is V_in t_charge / L and it discharges over L I_pk / (V_out + Vd -
V_in), so the conduction time tracks the energy delivered.
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
CAP = HERE.parent / "data" / "cap"
label = sys.argv[1] if len(sys.argv) > 1 else "dip28_3w_b"
m = json.loads((CAP / f"{label}.json").read_text())
V = {}
for ch in ("CH2", "CH3", "CH4"):
    p = m["waveforms"][ch]["preamble"]
    V[ch] = (np.fromfile(CAP / f"{label}_{ch}.bin", dtype=np.uint8).astype(float) - p["yorig"] - p["yref"]) * p["yinc"]
p = m["waveforms"]["CH4"]["preamble"]
fs = 1 / p["xinc"]
n = len(V["CH4"])
t = p["xorig"] + np.arange(n) / fs
vb = signal.sosfiltfilt(signal.butter(4, 50e3, "lp", fs=fs, output="sos"), V["CH4"])
sw1, sw2 = V["CH2"], V["CH3"]
per = int(round(5e-6 * fs))
lo, hi = np.percentile(sw2, 1), np.percentile(sw2, 99)
thr = lo + (hi - lo) / 3
fall = np.where((sw2[:-1] >= thr) & (sw2[1:] < thr))[0] + 1
starts = [fall[0]]
for e in fall[1:]:
    if e - starts[-1] > 0.8 * per:
        starts.append(e)
rows = []
for s in starts[:-1]:
    w = sw2[s:s + per]
    on = int(np.argmax(w > thr))
    post = w[on:]
    plat = np.median(post[int(0.3e-6 * fs):int(0.5e-6 * fs)])
    on_plat = np.abs(post - plat) <= 1.2
    entry = int(np.argmax(on_plat))
    cond = int(np.argmax(~on_plat[entry:]))
    rows.append((t[s], on / fs * 1e9, cond / fs * 1e9, plat, vb[s]))
R = np.array(rows)
base = np.median(R[(R[:, 0] < -0.2e-3), 1])
odd = np.where(np.abs(R[:, 1] - base) > 50)[0]
groups = np.split(odd, np.where(np.diff(odd) > 1)[0] + 1) if len(odd) else []
vb_ms = lambda a, b: float(np.mean(vb[(t >= a) & (t < b)]))  # noqa: E731
res = {"label": label, "periods": len(R), "q4_on_ns_median": float(np.median(R[:, 1])),
       "conduction_ns_median": float(np.median(R[:, 2])),
       "short_runs": [{"t_start_us": round(1e6 * R[g[0], 0], 2), "periods": len(g),
                       "q4_on_ns": [round(float(x)) for x in R[g, 1]],
                       "conduction_ns": [round(float(x)) for x in R[g, 2]]} for g in groups],
       "vbus_ac_mv": {"-100..-60us": round(1e3 * vb_ms(-100e-6, -60e-6)), "-10..0us": round(1e3 * vb_ms(-10e-6, 0)),
                      "0..25us": round(1e3 * vb_ms(0, 25e-6)), "0.5..1ms": round(1e3 * vb_ms(0.5e-3, 1e-3))}}
(HERE.parent / "metrics" / f"{label}_periods.json").write_text(json.dumps(res, indent=1) + "\n")
print(json.dumps(res, indent=1))

fig = plt.figure(figsize=(11, 10))
gs = fig.add_gridspec(3, 2, height_ratios=[1, 1, 1.2])
a0 = fig.add_subplot(gs[0, :])
k = (t > -0.4e-3) & (t < 1.2e-3)
a0.plot(1e3 * t[k][::20], 1e3 * vb[k][::20], lw=.8)
a0.set_ylabel("VBUS, AC [mV]\n(50 kHz low-pass)")
a0.set_title(f"{label}: 28 V boost, 3 W: one PID cycle of short pulses, then the dip")
a1 = fig.add_subplot(gs[1, :], sharex=a0)
kk = (R[:, 0] > -0.4e-3) & (R[:, 0] < 1.2e-3)
a1.plot(1e3 * R[kk, 0], R[kk, 1], ".", ms=3, label="Q4 pulse (SW2 low)")
a1.plot(1e3 * R[kk, 0], R[kk, 2], ".", ms=3, label="Q3 body-diode conduction")
a1.set_ylabel("per period [ns]")
a1.set_xlabel("t [ms] (trigger at 0)")
a1.legend(loc="upper right", fontsize=8)
for a in (a0, a1):
    a.grid(alpha=.3)
for j, (i, name) in enumerate(((groups[0][0] - 3, "normal period") if groups else (0, "normal period"),
                               (groups[0][2], "short period") if groups else (1, ""))):
    a = fig.add_subplot(gs[2, j])
    s = int((R[i, 0] - t[0]) * fs)
    a0_, b0_ = s - int(0.4e-6 * fs), s + int(4.8e-6 * fs)
    a.plot(1e6 * (t[a0_:b0_] - R[i, 0]), sw1[a0_:b0_], lw=.7, label="SW1 (AC)")
    a.plot(1e6 * (t[a0_:b0_] - R[i, 0]), sw2[a0_:b0_], lw=.7, label="SW2 (AC)")
    a.set_title(f"{name}, t = {1e6 * R[i, 0]:.1f} us", fontsize=9)
    a.set_xlabel("us from SW2 falling")
    a.set_ylim(-32, 14)
    a.grid(alpha=.3)
    if j == 0:
        a.set_ylabel("V (AC, 10 V/div)")
        a.legend(fontsize=8, loc="lower right")
fig.tight_layout()
fig.savefig(HERE.parent / "figures" / f"{label}_diagnosis.png", dpi=90)
