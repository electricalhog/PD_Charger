"""Three switching periods of SW1 and SW2 per record (one row per record)."""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

C = {"SW1": "#2a78d6", "SW2": "#eb6834"}
INK, MUTED, GRID, SURF = "#0b0b0b", "#6b6a64", "#e4e3df", "#fcfcfb"
out, recs = sys.argv[1], sys.argv[2:]
fig, axes = plt.subplots(len(recs), 1, figsize=(10, 3.2 * len(recs)), sharex=True, facecolor=SURF)
axes = np.atleast_1d(axes)
for ax, spec in zip(axes, recs):
    path, title = spec.split("=", 1)
    a = np.loadtxt(path, delimiter=",", skiprows=1)
    t = (a[:, 0] - a[0, 0]) * 1e6
    m = (t >= 100) & (t < 115)
    ax.set_facecolor(SURF)
    for k, col in ((1, "SW1"), (2, "SW2")):
        ax.plot(t[m] - 100, a[m, k], color=C[col], lw=1.0, label=col)
    ax.set_title(title, loc="left", color=INK, fontsize=10)
    ax.set_ylabel("V", color=MUTED)
    ax.grid(True, color=GRID, lw=0.6)
    for s in ax.spines.values():
        s.set_visible(False)
    ax.tick_params(colors=MUTED)
    ax.legend(loc="upper right", frameon=False, ncol=2, labelcolor=INK)
axes[-1].set_xlabel("µs (from 100 µs into the record)", color=MUTED)
fig.tight_layout()
fig.savefig(out, dpi=130, facecolor=SURF)
