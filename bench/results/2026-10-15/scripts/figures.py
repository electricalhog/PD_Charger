"""Figures for 2026-10-15 (25 ohm fixed on VBUS; interim DCM limits; predictive rectifier).

usage: python3 figures.py   (reads ../metrics/steps.json and ../data/cap; writes ../figures/*.png
                             and ../metrics/burst36.json)

Captures are grab.py single shots: CH2 SW1 and CH3 SW2 at 10 V/div DC, CH4 VBUS at 5 V/div DC,
12 us at 250 MSa/s (burst36_sr1: CH4 only, 6 s at 200 kSa/s).  Scope DC levels are good to about
a volt at 10 V/div; the high-current ground path adds an offset to the switch nodes.
"""
import json
from pathlib import Path

import numpy as np
from scipy import signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
CAP = HERE.parent / "data" / "cap"
FIG = HERE.parent / "figures"
STEPS = json.loads((HERE.parent / "metrics" / "steps.json").read_text())


def load(label, chans=("CH2", "CH3", "CH4")):
    m = json.loads((CAP / f"{label}.json").read_text())
    out = {}
    for ch in chans:
        p = m["waveforms"][ch]["preamble"]
        out[ch] = (np.fromfile(CAP / f"{label}_{ch}.bin", dtype=np.uint8).astype(float) - p["yorig"] - p["yref"]) * p["yinc"]
    p = m["waveforms"][chans[0]]["preamble"]
    t = p["xorig"] + np.arange(len(out[chans[0]])) * p["xinc"]
    return t, out, 1.0 / p["xinc"]


# 1. Current command against contract, per run (the load current V/25 for reference)
fig, ax = plt.subplots(figsize=(9, 5))
style = {"A": ("C0", "o"), "B": ("C1", "s"), "C": ("C2", "^"), "D": ("C3", "v"), "E": ("C4", "D")}
for run in STEPS["runs"]:
    xs, ys, lo, hi, fail = [], [], [], [], []
    for pt in run["points"]:
        if pt.get("rectifier") == "off":
            continue
        c = np.array(pt["i_cmd_ma"], float)
        if pt.get("result") == "did not hold":
            fail.append((pt["contract_v"], c.mean()))
            continue
        xs.append(pt["contract_v"]); ys.append(c.mean()); lo.append(c.min()); hi.append(c.max())
    col, mk = style[run["run"]]
    lab = f"run {run['run']}: " + ("rectifier on" if run["run"] == "E" else "rectifier off/none")
    ax.plot(xs, ys, marker=mk, color=col, lw=1, label=lab)
    for v, c in fail:
        ax.plot([v], [c], marker="x", color=col, ms=10, mew=2)
v = np.array([5, 9, 15, 20, 24, 28, 36])
ax.plot(v, 1000 * v / 25, "k--", lw=1, label="current into 25 ohm (V/25)")
ax.set_xlabel("contract [V]")
ax.set_ylabel("loop current command I_cmd [mA]  (x: contract not held)")
ax.set_title("2026-10-15: what the loop had to command for the 25 ohm, per run", fontsize=10)
ax.grid(alpha=.3)
ax.legend(fontsize=8)
fig.tight_layout()
fig.savefig(FIG / "icmd_by_run.png", dpi=90)


# 2. Switch nodes, rectifier off against on: one period, 0 = end of the charge pulse
def end_of_charge(sw, t, from_low=True):
    """First rising crossing of 15 V after at least 0.8 us below 2 V (Q4/Q2 side low)."""
    low = sw < 2.0
    run = np.convolve(low, np.ones(200), "same") > 195
    idx = np.where((sw[:-1] < 15) & (sw[1:] >= 15))[0]
    idx = [i for i in idx if i > 300 and run[i - 150]]
    return t[idx[0]]


def pair(labels, titles, out, node_for_edge, span=(-2.5e-6, 3.0e-6), ylim=(-8, 36)):
    fig, ax = plt.subplots(2, 1, figsize=(11, 6.5), sharex=True)
    for a, lab, ttl in zip(ax, labels, titles):
        t, V, fs = load(lab)
        t0 = end_of_charge(V[node_for_edge], t)
        k = (t > t0 + span[0]) & (t < t0 + span[1])
        for ch, name in (("CH2", "SW1 (Q1/Q2)"), ("CH3", "SW2 (Q3/Q4)"), ("CH4", "VBUS")):
            a.plot(1e6 * (t[k] - t0), V[ch][k], lw=.8, label=name)
        a.set_title(f"{lab}: {ttl}", fontsize=9)
        a.set_ylim(*ylim)
        a.set_ylabel("V")
        a.grid(alpha=.3)
        a.legend(fontsize=8, loc="upper right")
    ax[1].set_xlabel("us from the end of the charge pulse")
    fig.tight_layout()
    fig.savefig(FIG / out, dpi=90)


pair(("sr0_bb28", "sr1_bb28"),
     ("28 V buck-boost, rectifier off (run D, 18:51)", "28 V buck-boost, rectifier on (run E, 19:18)"),
     "sw_28v_bb_off_on.png", "CH3")


def buck_pair():
    fig, ax = plt.subplots(2, 1, figsize=(11, 6.5), sharex=True)
    for a, lab, ttl in zip(ax, ("sr0_buck5", "sr1_buck5b"),
                           ("5 V buck, rectifier off (19:14)", "5 V buck, rectifier on, faded in (19:17)")):
        t, V, fs = load(lab)
        sw1 = V["CH2"]
        i0 = np.where((sw1[:-1] < 12) & (sw1[1:] >= 12))[0]
        i0 = [i for i in i0 if i > 300 and np.all(sw1[i - 120:i - 20] < 6)][0]   # Q1 turn-on
        t0 = t[i0]
        k = (t > t0 - 0.5e-6) & (t < t0 + 5.0e-6)
        for ch, name in (("CH2", "SW1 (Q1/Q2)"), ("CH3", "SW2 (Q3/Q4)"), ("CH4", "VBUS")):
            a.plot(1e6 * (t[k] - t0), V[ch][k], lw=.8, label=name)
        a.set_title(f"{lab}: {ttl}", fontsize=9)
        a.set_ylim(-8, 30)
        a.set_ylabel("V")
        a.grid(alpha=.3)
        a.legend(fontsize=8, loc="upper right")
    ax[1].set_xlabel("us from Q1 turn-on")
    fig.tight_layout()
    fig.savefig(FIG / "sw_5v_buck_off_on.png", dpi=90)


buck_pair()

# 3. The 36 V burst
t, V, fs = load("burst36_sr1", ("CH4",))
v = V["CH4"]
lp2 = signal.sosfiltfilt(signal.butter(4, 2e3, "lp", fs=fs, output="sos"), v)
lp20 = signal.sosfiltfilt(signal.butter(4, 20e3, "lp", fs=fs, output="sos"), v)
hi = lp2 > 33
i0, i1 = int(np.argmax(hi)), len(hi) - 1 - int(np.argmax(hi[::-1]))
seg = lp20[i0 + int(0.2 * fs):i1 - int(0.05 * fs)]
f, P = signal.welch(seg - seg.mean(), fs=fs, nperseg=2 ** 14)
k = (f > 20) & (f < 20e3)
fpk = float(f[k][np.argmax(P[k])])
res = {"above_33v_ms": round(1e3 * float(t[i1] - t[i0]), 0), "hold_mean_v_scope": round(float(seg.mean()), 2),
       "hold_pp_v_20khz": round(float(np.ptp(seg)), 2), "hold_rms_mv_20khz": round(1e3 * float(seg.std()), 0),
       "dominant_hz": round(fpk, 0), "note": "scope at 5 V/div DC, about +-1 V absolute; for scale, the meter read 35.32 V on a steady, no-load 36 V contract on 2026-10-12"}
(HERE.parent / "metrics" / "burst36.json").write_text(json.dumps(res, indent=1) + "\n")
fig, ax = plt.subplots(1, 3, figsize=(13, 3.8), gridspec_kw={"width_ratios": [2.2, 1.2, 1.4]})
ax[0].plot(t[::20], lp2[::20], lw=.7)
ax[0].set_xlabel("s")
ax[0].set_ylabel("VBUS [V] (2 kHz LP)")
ax[0].set_title("36 V request from 28 V; Hard Reset by the 2 s cap", fontsize=9)
z = (t > t[i0] + 1.0) & (t < t[i0] + 1.005)
ax[1].plot(1e3 * (t[z] - t[z][0]), lp20[z], lw=.8)
ax[1].set_xlabel("ms")
ax[1].set_title("5 ms of the hold (20 kHz LP)", fontsize=9)
ax[2].semilogy(f[k] / 1e3, np.sqrt(P[k]), lw=.8)
ax[2].set_xlabel("kHz")
ax[2].set_ylabel("V/rtHz")
ax[2].set_title(f"hold spectrum: peak {fpk / 1e3:.2f} kHz", fontsize=9)
for a in ax:
    a.grid(alpha=.3)
fig.tight_layout()
fig.savefig(FIG / "burst36.png", dpi=90)
print(json.dumps(res))
