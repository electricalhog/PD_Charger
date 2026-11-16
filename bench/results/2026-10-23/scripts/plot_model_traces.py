"""Plot the host-model traces behind plan 9.4's table (bench/results/2026-10-23/data/model_traces/*.csv).
Run from the repo root.  Writes bench/results/2026-10-23/data/model_traces/model_traces.png."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

D = "bench/results/2026-10-23/data/model_traces"
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
LOOPS = [("20k", "Today: 20 kHz, integral only", "#2a78d6", "-"),
         ("100k", "100 kHz, best stable (ki 3142, kd 1 µs)", "#eb6834", "--"),
         ("400k", "Every period (400 kHz), kp 0.2, kd 4 µs", "#1baf7a", "-.")]
CXS = [(8, "8 µF on VBUS (board ceramics + QT Py)"), (55, "55 µF on VBUS (measured 2026-10-23)"),
       (300, "300 µF on VBUS (bulk)")]
EVENTS = [(5000.0, "Step 0.8 → 2.5 A at t = 0"), (17000.0, "Release 2.5 → 0.8 A at t = 0")]

def load(cx, loop):
    with open(os.path.join(D, f"trace_cx{cx}_{loop}.csv")) as f:
        rows = list(csv.DictReader(f))
    return [float(r["t_us"]) for r in rows], [1e3 * (float(r["vb"]) - 20.0) for r in rows]

plt.rcParams.update({"font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK2, "xtick.color": INK2,
                     "ytick.color": INK2, "text.color": INK})
fig, axes = plt.subplots(3, 2, figsize=(11, 9.5), facecolor=SURFACE, sharex="col")
for i, (cx, cx_title) in enumerate(CXS):
    ys_all = []
    for j, (t_ev, ev_title) in enumerate(EVENTS):
        ax = axes[i][j]
        ax.set_facecolor(SURFACE)
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
        ax.grid(True, color=GRID, linewidth=0.6)
        ax.axhline(0, color=INK2, linewidth=0.8)
        ext = []
        for key, label, color, ls in LOOPS:
            t, v = load(cx, key)
            pts = [(tt - t_ev, vv) for tt, vv in zip(t, v) if t_ev - 100 <= tt <= t_ev + 1500]
            x = [p[0] for p in pts]; y = [p[1] for p in pts]
            ax.plot(x, y, color=color, linestyle=ls, linewidth=2 if key == "20k" else 1.6, label=label)
            ys_all += y
            ext.append((min(y) if j == 0 else max(y), color))
        # extremes as direct labels, in ink
        txt = ("Dip: " if j == 0 else "Peak: ") + " / ".join(f"{e:+.0f}" for e, _ in ext) + " mV"
        ax.text(0.98, 0.04 if j == 1 else 0.96, txt, transform=ax.transAxes, ha="right",
                va="bottom" if j == 1 else "top", fontsize=8.5, color=INK2)
        ax.set_title(f"{cx_title}\n{ev_title}" if j == 0 else ev_title, fontsize=9.5, loc="left", color=INK)
        if j == 0:
            ax.set_ylabel("V_out − 20 V (mV), board node")
        if i == 2:
            ax.set_xlabel("time from the load change (µs)")
    lim = max(abs(min(ys_all)), abs(max(ys_all))) * 1.1
    for j in range(2):
        axes[i][j].set_ylim(-lim, lim)
handles, labels = axes[0][0].get_legend_handles_labels()
fig.suptitle("Host model, 20 V buck CCM at 400 kHz: V_out through a 0.8 → 2.5 A step and back\n"
             "(synchronous samples through the 0.8 µs divider RC; dips/peaks listed in the legend's order)",
             y=0.99, fontsize=10.5, color=INK)
fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.945), ncol=3, frameon=False, fontsize=9)
fig.tight_layout(rect=(0, 0, 1, 0.915))
out = os.path.join(D, "model_traces.png")
fig.savefig(out, dpi=130, facecolor=SURFACE)
print(out)
