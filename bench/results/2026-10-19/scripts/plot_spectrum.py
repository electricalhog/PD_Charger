"""SW1 and SW2 spectra (Hann, peak per bin, dBuV), max per log-spaced bin, 150 kHz-100 MHz."""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COL = ["#2a78d6", "#eb6834"]
INK, MUTED, GRID, SURF = "#0b0b0b", "#6b6a64", "#e4e3df", "#fcfcfb"
out, recs = sys.argv[1], sys.argv[2:]
edges = np.logspace(np.log10(150e3), np.log10(100e6), 400)
fig, axes = plt.subplots(1, 2, figsize=(11, 3.8), sharey=True, facecolor=SURF)
for j, spec in enumerate(recs):
    path, label = spec.split("=", 1)
    a = np.loadtxt(path, delimiter=",", skiprows=1)
    dt = float(np.median(np.diff(a[:, 0])))
    for ax, k, name in ((axes[0], 1, "SW1 (Q1/Q2 node)"), (axes[1], 2, "SW2 (Q3/Q4 node)")):
        v = a[:, k] - a[:, k].mean()
        w = np.hanning(len(v))
        amp = 2 * np.abs(np.fft.rfft(v * w)) / w.sum()
        f = np.fft.rfftfreq(len(v), dt)
        idx = np.digitize(f, edges)
        fx, dbx = [], []
        for b in range(1, len(edges)):
            m = idx == b
            if m.any():
                fx.append(f[m][np.argmax(amp[m])]); dbx.append(20 * np.log10(amp[m].max() / 1e-6))
        ax.semilogx(fx, dbx, color=COL[j], lw=1.2, label=label)
        ax.set_title(name, loc="left", color=INK, fontsize=10)
for ax in axes:
    ax.set_facecolor(SURF); ax.grid(True, which="both", color=GRID, lw=0.5)
    for s in ax.spines.values():
        s.set_visible(False)
    ax.tick_params(colors=MUTED); ax.set_xlabel("Hz", color=MUTED)
axes[0].set_ylabel("dBµV (peak per bin, max per log bin)", color=MUTED)
axes[1].legend(loc="upper right", frameon=False, labelcolor=INK)
fig.tight_layout(); fig.savefig(out, dpi=130, facecolor=SURF)
