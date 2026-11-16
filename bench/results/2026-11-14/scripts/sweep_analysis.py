"""Per-point table and figures for the 12 -> 33 -> 12 V sweep (sweep.py): sweep_points_<stamp>.json,
figures/sweep_efficiency.png, figures/sweep_switch_nodes.png.  Run with system python3 (numpy, matplotlib) from the
repository root:  python3 bench/results/2026-11-14/scripts/sweep_analysis.py [sweep_<stamp>.json]

Method as on 2026-11-11 (ramp_table.py): V_in and V_out are the medians of the scope's trace samples (CH2, CH4),
interpolated inside the code step; I_out is ID_MON (ccm_i_avg_ma, calibrated 991 / +4 on 2026-11-13), I_in is
IS_MON (i_in_filt_ma), both board means over the 1.5 s before the capture to its end.  Loss = V_in I_in - V_out I_out.
V_out^2 / 25.3 ohm is kept as a cross-check on I_out only (the resistor warms; its value is not tracked)."""
import json
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

D = Path("bench/results/2026-11-14"); CAP = D / "captures"; F = D / "figures"
src = Path(sys.argv[1]) if len(sys.argv) > 1 else sorted(D.glob("sweep_2*.json"))[-1]
if not src.is_absolute() and not src.exists():
    src = D / src
run = json.loads(src.read_text())
stamp = src.stem.replace("sweep_", "")
rows = run["rows"]
KIND = {0: "dcm", 1: "buck", 2: "boost", 3: "band"}


def med(x):
    """Median with linear interpolation inside the ADC code step (the grouped-data median)."""
    u = np.unique(x); m = float(np.median(x))
    if len(u) < 2:
        return m
    lsb = float(np.min(np.diff(u)))
    below = np.count_nonzero(x < m - lsb / 2); at = np.count_nonzero(np.abs(x - m) <= lsb / 2)
    return m - lsb / 2 + lsb * (len(x) / 2 - below) / at if at else m


pts = []
for p in run["points"]:
    w = [r for r in rows if p["t_cap0"] - 1.5 <= r["t"] <= p["t_cap1"] and r["state"] == "RUNNING"]
    m = lambda k: float(np.mean([r[k] for r in w if r.get(k) is not None]))   # noqa: E731
    states = {(r["ccm"], r["kind"]) for r in w}
    ccm, kind = max(states, key=lambda s: sum((r["ccm"], r["kind"]) == s for r in w))
    d = np.genfromtxt(CAP / p["cap"]["file"], delimiter=",", names=True)
    vin_s, vout_s = med(d["CHAN2"]), med(d["CHAN4"])
    i_out, i_in = m("id_cal_ma") / 1e3, m("i_in_ma") / 1e3
    o = {"dir": p["dir"], "set_mv": p["set_mv"], "n": len(w), "state": KIND[kind] if ccm else "dcm",
         "mixed": len(states) > 1, "modes": sorted({r["mode"] for r in w}),
         "on_ns": m("on_ns"), "fsw_khz_board": m("fsw_khz"),
         "t_cap0": p["t_cap0"],
         "i_out_a": i_out, "i_in_a": i_in, "vin_scope": vin_s, "vout_scope": vout_s,
         "vin_board": m("v_in_mv") / 1e3, "vout_board": m("v_out_mv") / 1e3,
         "p_in_w": vin_s * i_in, "p_out_w": vout_s * i_out, "eff": vout_s * i_out / (vin_s * i_in),
         "loss_w": vin_s * i_in - vout_s * i_out, "i_out_r_a": vout_s / run["r_ohm"],
         "vout_pp": float(d["CHAN4"].max() - d["CHAN4"].min()), "file": p["cap"]["file"]}
    pts.append(o)
    print(f"{o['dir']:4s} {o['set_mv']:5d} {o['state']:5s}{'*' if o['mixed'] else ' '} on {o['on_ns']:5.0f} ns "
          f"f {o['fsw_khz_board']:4.0f} kHz  Vin {vin_s:6.3f} Vout {vout_s:6.3f} "
          f"(board {o['vout_board']:6.3f})  Iin {i_in:5.3f} Iout {i_out:5.3f} (V/R {o['i_out_r_a']:5.3f})  "
          f"Pout {o['p_out_w']:5.1f} W  eff {100 * o['eff']:5.1f} %  loss {o['loss_w']:4.2f} W")
(D / f"sweep_points_{stamp}.json").write_text(json.dumps(pts, indent=1))

# ---------------------------------------------------------------- style (2026-11-11 figures)
SURF, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9"
C = {"dcm": "#2a78d6", "buck": "#eb6834", "band": "#1baf7a", "boost": "#6250d6"}
NAME = {"dcm": "DCM (buck)", "buck": "CCM buck", "band": "CCM band (buck-boost)", "boost": "CCM boost"}
SW = {"CHAN1": "#1baf7a", "CHAN3": "#6250d6"}
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.edgecolor": GRID, "axes.labelcolor": INK2,
                     "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK2, "ytick.labelcolor": INK2,
                     "axes.spines.top": False, "axes.spines.right": False, "figure.facecolor": SURF,
                     "axes.facecolor": SURF, "savefig.facecolor": SURF, "axes.grid": True, "grid.color": GRID,
                     "grid.linewidth": 0.6, "axes.axisbelow": True})
F.mkdir(exist_ok=True)

# ---------------------------------------------------------------- 1. efficiency and loss against V_out, up and down
fig, (a1, a2) = plt.subplots(2, 1, figsize=(10, 7.8), sharex=True, height_ratios=[3, 2])
vin = float(np.median([p["vin_scope"] for p in pts]))
for d_, ls, fill in (("up", "-", True), ("down", (0, (4, 2)), False)):
    s = [p for p in pts if p["dir"] == d_]
    s.sort(key=lambda p: p["vout_scope"])
    x = np.array([p["vout_scope"] for p in s])
    a1.plot(x, [100 * p["eff"] for p in s], ls=ls, color=MUTED, lw=1.2, zorder=1)
    a2.plot(x, [p["loss_w"] for p in s], ls=ls, color=MUTED, lw=1.2, zorder=1)
    for p in s:
        kw = dict(color=C[p["state"]], ms=8, mew=2, zorder=3, mec=C[p["state"]] if not fill else SURF,
                  mfc=C[p["state"]] if fill else SURF, marker="o" if d_ == "up" else "D")
        a1.plot(p["vout_scope"], 100 * p["eff"], **kw)
        a2.plot(p["vout_scope"], p["loss_w"], **kw)
for ax in (a1, a2):
    ax.axvline(vin, color=MUTED, lw=0.8, ls=(0, (2, 2)))
a1.text(vin + 0.15, 99.3, f"V_in {vin:.2f} V", color=INK2, fontsize=9, va="top")
h = [plt.Line2D([], [], color=C[k], marker="s", ls="", ms=9, label=NAME[k]) for k in C]
h += [plt.Line2D([], [], color=INK2, marker="o", ls="-", ms=7, mfc=INK2, mec=SURF, label="ceiling stepped up"),
      plt.Line2D([], [], color=INK2, marker="D", ls=(0, (4, 2)), ms=7, mfc=SURF, mec=INK2, label="stepped down")]
a1.legend(handles=h, loc="lower right", frameon=False, labelcolor=INK2, ncol=2, fontsize=9)
a1.set_ylim(70, 100); a1.set_ylabel("Efficiency (%)")
a2.set_ylim(0, max(p["loss_w"] for p in pts) * 1.15); a2.set_ylabel("Loss (W)")
a2.set_xlabel("V_out (V), into 25.3 ohm")
a1.set_title(f"Voltage sweep 12 -> 33 -> 12 V into 25.3 ohm, {vin:.1f} V in, auto CCM "
             f"({min(p['p_out_w'] for p in pts):.0f}-{max(p['p_out_w'] for p in pts):.0f} W)",
             loc="left", color=INK, fontsize=11.5, pad=12)
fig.text(0.01, 0.008, "Currents: ID_MON (I_out) and IS_MON (I_in), calibrated against the ammeter.  Voltages: medians of the "
         "scope's trace samples (30k per point).\nColour = the converter's state over the point's window (board log); "
         "one START, the ceiling stepped without stopping.\nMost of the up / down gap is ID_MON, not the converter: it "
         "reads 3 % higher after the turn at 33 V at the same operating point (sweep_sensor_check.png).", color=MUTED, fontsize=8, va="bottom")
fig.tight_layout(rect=(0, 0.07, 1, 1)); fig.savefig(F / "sweep_efficiency.png", dpi=160); plt.close(fig)

# ---------------------------------------------------------------- 2. switch nodes at one point per state
want = [("up", 16000), ("up", 22500), ("up", 24400), ("up", 30000)]
sel = [next(p for p in pts if p["dir"] == d_ and p["set_mv"] == mv) for d_, mv in want]
fig, axs = plt.subplots(2, 4, figsize=(14, 5.8), sharex=True, sharey=True)
for j, p in enumerate(sel):
    d = np.genfromtxt(CAP / p["file"], delimiter=",", names=True)
    t = d["t_s"] * 1e6; k = (t >= -2.0) & (t <= 13.0)
    for i, ch in enumerate(("CHAN1", "CHAN3")):
        ax = axs[i, j]; ax.plot(t[k], d[ch][k], color=SW[ch], lw=1.1)
        ax.set_ylim(-6, 38); ax.set_yticks([0, 10, 20, 30])
        if j == 0: ax.set_ylabel(("SW1 (input leg)" if i == 0 else "SW2 (output leg)") + " (V)")
        if i == 1: ax.set_xlabel("Time (us)")
        ax.spines["bottom"].set_color(C[p["state"]]); ax.spines["bottom"].set_linewidth(2.5)
    axs[0, j].set_title(f"{NAME[p['state']]}\n{p['vout_scope']:.1f} V out, {p['i_out_a']:.2f} A, on {p['on_ns']:.0f} ns, "
                        f"{100 * p['eff']:.1f} %", color=INK, fontsize=10, loc="left")
fig.suptitle(f"Switch nodes in each state ({vin:.1f} V in, 25.3 ohm; 125 MSa/s, no bandwidth limit)", x=0.01,
             ha="left", color=INK, fontsize=11.5)
fig.text(0.01, 0.005, "Bottom rule: the state's colour (as in the efficiency figure).  DCM: rectifier FETs off, both nodes ring once the "
         "inductor current reaches zero.  CCM buck: SW2 held at V_out.  Band and boost: both legs switch.", color=MUTED, fontsize=8)
fig.tight_layout(rect=(0, 0.03, 1, 0.95)); fig.savefig(F / "sweep_switch_nodes.png", dpi=160); plt.close(fig)

# ---------------------------------------------------------------- 3. the current sensors against V_out / R, in time
fig, (b1, b2) = plt.subplots(2, 1, figsize=(10, 6.4), sharex=True)
for p in pts:
    kw = dict(color=C[p["state"]], ms=7, mew=1.5, marker="o" if p["dir"] == "up" else "D",
              mfc=C[p["state"]] if p["dir"] == "up" else SURF, mec=SURF if p["dir"] == "up" else C[p["state"]])
    b1.plot(p["t_cap0"], p["i_out_a"] / p["i_out_r_a"], **kw)
    b2.plot(p["t_cap0"], 100 * p["vout_scope"] * p["i_out_r_a"] / p["p_in_w"], **kw)
t_turn = next(p["t_cap0"] for p in pts if p["dir"] == "down")
for ax in (b1, b2):
    ax.axvline(t_turn - 7, color=MUTED, lw=0.8, ls=(0, (2, 2)))
b1.text(t_turn - 4, 1.055, "33 V: stepping down from here", color=INK2, fontsize=9, va="top")
b1.axhline(1.0, color=INK2, lw=0.8)
b1.set_ylabel("ID_MON / (V_out/R)"); b1.set_ylim(0.94, 1.07)
b2.set_ylabel("Efficiency, I_out = V_out/R (%)"); b2.set_ylim(70, 100)
b2.set_xlabel("Time into the run (s)")
b1.set_title("Cross-check: ID_MON against V_out / R, and efficiency without ID_MON", loc="left", color=INK, fontsize=11.5)
fig.text(0.01, 0.008, "25.3 ohm is the resistor's nominal value; it warms over the run and its temperature coefficient is "
         "not known, so the lower panel's absolute level is uncertain by that.\nColours and markers as in the "
         "efficiency figure.", color=MUTED, fontsize=8, va="bottom")
fig.tight_layout(rect=(0, 0.06, 1, 1)); fig.savefig(F / "sweep_sensor_check.png", dpi=160); plt.close(fig)
print(f"-> {F / 'sweep_efficiency.png'}, {F / 'sweep_switch_nodes.png'}, {F / 'sweep_sensor_check.png'}")
