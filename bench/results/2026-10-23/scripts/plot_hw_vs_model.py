"""Bench switch-pulse reconnects (20 V, forced CCM 400 kHz, 25.3 ohm, 3 A ID_MON limit) against the fitted host
model.  Run from the repo root; writes bench/results/2026-10-23/data/hw_vs_model/hw_vs_model.png."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
D = "bench/results/2026-10-23/data/hw_vs_model"
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"
SW2_OFFSET = 0.29   # scope CH3 read 19.71 V on a 20.00 V node before the event
def rd(p): return list(csv.DictReader(open(os.path.join(D, p))))
plt.rcParams.update({"font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK2, "xtick.color": INK2,
                     "ytick.color": INK2, "text.color": INK})
fig, axes = plt.subplots(1, 3, figsize=(14, 4.8), facecolor=SURFACE)
for ax in axes:
    ax.set_facecolor(SURFACE); ax.grid(True, color=GRID, linewidth=0.6)
    for sp in ("top", "right"): ax.spines[sp].set_visible(False)
# bench 20 ms scope record, aligned on VBUS rising through 4 V
b = rd("hw_pulse20_scope_2p5us.csv")
bt = [float(r["t_us"]) for r in b]; bvb = [float(r["vbus_v"]) for r in b]
t0b = next(t for t, v in zip(bt, bvb) if v > 4.0)
m = rd("model_fit_phase0.csv")
mt = [float(r["t_us"]) for r in m]; mvx = [float(r["vx"]) for r in m]
t0m = next(t for t, v in zip(mt, mvx) if t > 30000 and v > 4.0)
def win(t, y, t0, lo=-50, hi=300): return zip(*[(tt - t0, yy) for tt, yy in zip(t, y) if lo <= tt - t0 <= hi])
# panel 1: board node and VBUS
ax = axes[0]
x, y = win(bt, [float(r["sw2_v"]) + SW2_OFFSET for r in b], t0b); ax.plot(x, y, color=BLUE, lw=2, label="Bench: board node (SW2)")
x, y = win(mt, [float(r["vb"]) for r in m], t0m); ax.plot(x, y, color=BLUE, lw=1.6, ls="--", label="Model: board node")
x, y = win(bt, bvb, t0b); ax.plot(x, y, color=ORANGE, lw=2, label="Bench: VBUS")
x, y = win(mt, mvx, t0m); ax.plot(x, y, color=ORANGE, lw=1.6, ls="--", label="Model: VBUS")
ax.set_title("Reconnect after a 20 ms open: voltages", loc="left", fontsize=10)
ax.set_xlabel("µs from VBUS rising through 4 V"); ax.set_ylabel("V"); ax.legend(frameon=False, fontsize=8, loc="lower right")
# panel 2: inductor current
ax = axes[1]
x, y = win(bt, [float(r["il_mon_v"]) / 0.25 for r in b], t0b); ax.plot(x, y, color=AQUA, lw=2, label="Bench: IL_MON (bandwidth-limited)")
x, y = win(mt, [float(r["il"]) for r in m], t0m); ax.plot(x, y, color=AQUA, lw=1.6, ls="--", label="Model: L1 average (capped 7.5 A)")
ax.set_title("Reconnect after a 20 ms open: inductor current", loc="left", fontsize=10)
ax.set_xlabel("µs from VBUS rising through 4 V"); ax.set_ylabel("A"); ax.legend(frameon=False, fontsize=8, loc="upper right")
# panel 3: slow recovery, regulator trace (50 us) vs model at two PID phases
ax = axes[2]
for ms, color, label in ((20, BLUE, "Bench trace, 20 ms open"), (10, ORANGE, "Bench trace, 10 ms open")):
    tr = rd(f"hw_pulse{ms}_trace.csv")
    x, y = win([float(r["t_ms"]) - ms for r in tr], [int(r["v_out_mv"]) / 1000 for r in tr], 0, -0.5, 6.0)
    ax.plot(x, y, color=color, lw=2, label=label)
for f, sh, color, label in (("model_fit_phase0.csv", 0, BLUE, "Model, close at the cycle start"),
                            ("model_fit_phase30.csv", 30, ORANGE, "Model, close 30 µs into the cycle")):
    mm = rd(f)
    x, y = win([(float(r["t_us"]) - 30000 - sh) / 1000 for r in mm], [float(r["vb"]) for r in mm], 0, -0.5, 6.0)
    ax.plot(x, y, color=color, lw=1.6, ls="--", label=label)
ax.set_title("Recovery under the 3 A limit (board node)", loc="left", fontsize=10)
ax.set_xlabel("ms from the reconnect"); ax.set_ylabel("V"); ax.legend(frameon=False, fontsize=8, loc="lower right")
fig.suptitle("Bench vs host model, switch-pulse at 20 V, forced CCM 400 kHz, 25.3 Ω, 3 A limit on ID_MON (2026-10-23).  "
             "Model fitted: VBUS side 85 µF, L1 capped 7.5 A (COMP1), Q5/Q6 2 Ω decaying in 70 µs", fontsize=9.5, y=0.99)
fig.tight_layout(rect=(0, 0, 1, 0.95))
out = os.path.join(D, "hw_vs_model.png"); fig.savefig(out, dpi=130, facecolor=SURFACE); print(out)
