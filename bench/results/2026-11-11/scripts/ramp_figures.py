"""Figures for the power ramp (bench 2026-11-11 section 5) from ramp/points.json and the capture CSVs:
figures/efficiency.png, figures/switch_nodes.png, figures/vout_noise.png.  Run with system python3 from the
repository root, after ramp_table.py."""
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

R = Path("bench/results/2026-11-11/ramp"); F = Path("bench/results/2026-11-11/figures")

def cap(name):
    """A capture CSV by the name ramp.json gives it; committed gzipped (numpy reads .csv.gz directly)."""
    p = R / name
    return p if p.exists() else p.with_name(p.name + ".gz")

pts = json.loads((R / "points.json").read_text())
SURF, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9"
C = {"dcm": "#2a78d6", "ccm": "#eb6834"}          # colour follows the conduction mode in every figure
SW = {"CHAN1": "#1baf7a", "CHAN3": "#6250d6"}     # switch nodes
NAME = {"dcm": "DCM, diode rectification", "ccm": "CCM, synchronous rectification"}
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.edgecolor": GRID, "axes.labelcolor": INK2,
                     "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK2, "ytick.labelcolor": INK2,
                     "axes.spines.top": False, "axes.spines.right": False, "figure.facecolor": SURF,
                     "axes.facecolor": SURF, "savefig.facecolor": SURF, "axes.grid": True, "grid.color": GRID,
                     "grid.linewidth": 0.6, "axes.axisbelow": True})

def series(mode):
    s = sorted((p for p in pts if p["mode"] == mode), key=lambda p: p["p_out_scope_w"])
    return (np.array([p["p_out_scope_w"] for p in s]), np.array([100 * p["eff_scope"] for p in s]),
            np.array([p["p_in_scope_w"] - p["p_out_scope_w"] for p in s]), s)

# ---------------------------------------------------------------- 1. efficiency and loss against output power
fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 7.2), sharex=True, height_ratios=[3, 2])
for mode in ("dcm", "ccm"):
    x, eff, loss, s = series(mode)
    a1.plot(x, eff, "-o", color=C[mode], lw=2, ms=6, mec=SURF, mew=1.5, label=NAME[mode])
    a2.plot(x, loss, "-o", color=C[mode], lw=2, ms=6, mec=SURF, mew=1.5)
xd, ed, ld, sd = series("dcm"); xc, ec, lc, sc = series("ccm")
a1.annotate(f"{ec[-1]:.1f} % at {xc[-1]:.0f} W", (xc[-1], ec[-1]), xytext=(-8, -16), textcoords="offset points",
            ha="right", color=INK, fontsize=10)
a1.annotate(f"{ed[-1]:.1f} % at {xd[-1]:.0f} W", (xd[-1], ed[-1]), xytext=(8, -4), textcoords="offset points",
            color=INK, fontsize=10)
a1.text(4.5, 82.6, "DCM, diode rectification", color=INK2, fontsize=9.5)
a1.text(46, ec[-1] - 4.6, "CCM, synchronous rectification", color=INK2, fontsize=9.5, ha="center")
a2.annotate(f"{ld[-1]:.1f} W", (xd[-1], ld[-1]), xytext=(8, -3), textcoords="offset points", color=INK)
a2.annotate(f"{lc[-1]:.1f} W", (xc[-1], lc[-1]), xytext=(-4, 9), textcoords="offset points", color=INK, ha="right")
# the firmware's own handover points (2 A up, about 1.19 A down at this voltage)
p_up = xd[-1]; p_dn = 1.19 * np.interp(1.19, [p["i_out_a"] for p in sc], [p["vout_scope"] for p in sc])
for ax in (a1, a2):
    ax.axvspan(p_dn, p_up, color="#f0efec", zorder=0)
    ax.axvline(p_up, color=MUTED, lw=0.8, ls=(0, (4, 3)))
a1.text((p_dn + p_up) / 2, 99.2, "both modes can run\n(hysteresis)", ha="center", va="top", color=INK2, fontsize=9)
a1.text(p_up + 0.8, 71.2, "automatic: CCM above 2 A", color=INK2, fontsize=9, va="bottom")
a1.set_ylim(70, 100); a1.set_ylabel("Efficiency (%)"); a1.legend(loc="lower right", frameon=False, labelcolor=INK2)
a2.set_ylim(0, 8); a2.set_ylabel("Loss (W)"); a2.set_xlabel("Output power into the 4S pack (W)"); a2.set_xlim(0, 72)
a1.set_title("Efficiency from 1.5 to 69 W: 24.4 V in, 15.1-15.4 V out (4-switch buck-boost in buck mode, 200 kHz)",
             loc="left", color=INK, fontsize=11.5, pad=12)
fig.text(0.01, 0.008, "Currents: ID_MON and IS_MON, both checked against an ammeter (within 0.3 % and 1.1 %).\n"
         "Voltages: medians of the scope's trace samples (DS1054Z, 30k samples per point), not the board's readings.",
         color=MUTED, fontsize=8, va="bottom")
fig.tight_layout(rect=(0, 0.045, 1, 1)); fig.savefig(F / "efficiency.png", dpi=160); plt.close(fig)

# ---------------------------------------------------------------- 2. switch nodes
sel = [("dcm", 500), ("dcm", 1500), ("ccm", 1500), ("ccm", 4500)]
fig, axs = plt.subplots(2, 4, figsize=(13, 5.6), sharex=True, sharey=True)
for j, (mode, ma) in enumerate(sel):
    p = next(q for q in pts if q["mode"] == mode and q["set_ma"] == ma)
    d = np.genfromtxt(cap(p["dc_file"]), delimiter=",", names=True)
    t = d["t_s"] * 1e6; k = (t >= -2.0) & (t <= 10.5)
    for i, ch in enumerate(("CHAN1", "CHAN3")):
        ax = axs[i, j]; ax.plot(t[k], d[ch][k], color=SW[ch], lw=1.1)
        ax.set_ylim(-6, 32); ax.set_yticks([0, 10, 20, 30])
        if j == 0: ax.set_ylabel(("SW1 (input leg)" if i == 0 else "SW2 (output leg)") + " (V)")
        if i == 1: ax.set_xlabel("Time (us)")
    axs[0, j].set_title(f"{'DCM' if mode == 'dcm' else 'CCM'}  {p['i_out_a']:.1f} A  {p['p_out_scope_w']:.0f} W",
                        color=INK, fontsize=10.5, loc="left")
    axs[0, j].plot([], [])
    for i in (0, 1): axs[i, j].spines["bottom"].set_color(C[mode]); axs[i, j].spines["bottom"].set_linewidth(2)
fig.suptitle("Switch nodes across the load range (24.4 V in, 15.2 V out; 125 MSa/s, no bandwidth limit)", x=0.01, ha="left",
             color=INK, fontsize=11.5)
fig.text(0.01, 0.005, "Bottom rule: blue = DCM (rectifier FETs off, body diodes conduct), orange = CCM (both legs driven "
         "synchronously).  DCM's ringing after the inductor current reaches zero is L1 against the switch-node capacitance.",
         color=MUTED, fontsize=8)
fig.tight_layout(rect=(0, 0.03, 1, 0.95)); fig.savefig(F / "switch_nodes.png", dpi=160); plt.close(fig)

# ---------------------------------------------------------------- 3. V_out noise: ripple and spectrum
sel = [p for p in pts if p["ac_file"]]
sel.sort(key=lambda p: (p["mode"] != "dcm", p["i_out_a"]))
fig, axs = plt.subplots(len(sel), 2, figsize=(12, 2.05 * len(sel) + 0.9), width_ratios=[2, 3], sharex="col", sharey="col")
for i, p in enumerate(sel):
    d = np.genfromtxt(cap(p["ac_file"]), delimiter=",", names=True)
    v = d["CHAN4"] - d["CHAN4"].mean(); t = d["t_s"]; dt = float(np.median(np.diff(t))); n = len(v)
    col = C[p["mode"]]
    k = slice(n // 2, n // 2 + int(25e-6 / dt))
    axs[i, 0].plot((t[k] - t[k][0]) * 1e6, v[k] * 1e3, color=col, lw=1.0)
    w = np.hanning(n); spec = np.abs(np.fft.rfft(v * w)) / (w.sum() / 2) / np.sqrt(2)     # V rms per bin
    f = np.fft.rfftfreq(n, dt); kk = (f >= 2e3) & (f <= 20e6)
    # peak envelope in log-spaced bins (120 a decade): the 417 Hz bins are a solid block above 1 MHz otherwise
    edges = np.logspace(np.log10(2e3), np.log10(20e6), 481); idx = np.digitize(f[kk], edges)
    fe = np.array([f[kk][idx == b].mean() for b in np.unique(idx)])
    se = np.array([spec[kk][idx == b].max() for b in np.unique(idx)])
    axs[i, 1].plot(fe, 20 * np.log10(np.maximum(se, 1e-7) / 1e-3), color=col, lw=1.2)
    rms = float(v.std() * 1e3); pp = float((v.max() - v.min()) * 1e3)
    band = (f > 150e3) & (f < 250e3); f1 = f[band][np.argmax(spec[band])]; a1v = spec[band].max() * 1e3
    p.update(noise_rms_mv=rms, noise_pp_mv=pp, fund_khz=f1 / 1e3, fund_mv_rms=a1v)
    axs[i, 0].set_title(f"{'DCM' if p['mode'] == 'dcm' else 'CCM'}  {p['i_out_a']:.1f} A  {p['p_out_scope_w']:.0f} W   "
                        f"{rms:.0f} mV rms, {pp/1e3:.1f} V p-p", loc="left", color=INK, fontsize=10)
    axs[i, 1].set_title(f"{f1/1e3:.0f} kHz line: {a1v:.0f} mV rms", loc="left", color=INK2, fontsize=9.5)
    axs[i, 0].set_ylabel("V_out ripple (mV)"); axs[i, 1].set_ylabel("dBmV rms")
axs[-1, 0].set_xlabel("Time (us)"); axs[-1, 1].set_xlabel("Frequency (Hz)")
axs[0, 1].set_xscale("log"); axs[0, 1].set_ylim(-40, 60)
fig.suptitle("V_out noise at five load points: ripple and spectrum (AC-coupled, 20 MHz limit, 2.4 ms at 125 MSa/s, Hann, peak per 1/120 decade)",
             x=0.01, ha="left", color=INK, fontsize=11.5)
fig.text(0.01, 0.004, "Probe: 10x with a ground lead on V_out at the converter side of the output switch, before the pack's "
         "leads, so the spikes include probe pickup.  Blue = DCM, orange = CCM.", color=MUTED, fontsize=8)
fig.tight_layout(rect=(0, 0.02, 1, 0.965)); fig.savefig(F / "vout_noise.png", dpi=160); plt.close(fig)
(R / "points.json").write_text(json.dumps(pts, indent=1))
for p in sel: print(p["mode"], p["set_ma"], "rms %.0f mV pp %.0f mV fund %.0f kHz %.1f mV rms scale %s" % (p["noise_rms_mv"], p["noise_pp_mv"], p["fund_khz"], p["fund_mv_rms"], p["ac_scale"]))
