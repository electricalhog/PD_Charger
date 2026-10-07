"""Deliverable figures from the 2026-10-07 bench captures (EPR contract, VBUS bring-up, load steps)."""
import json
import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

D = Path(__file__).parent
TRACE_LOG = D / "epr_trace.log"

C_VBUS, C_CC1, C_VCONN = "#1f5fbf", "#0f9b8e", "#c2399b"
C_P, C_V, C_D, C_VIN = "#e07b00", "#1f5fbf", "#6b6b6b", "#0f9b8e"
plt.rcParams.update({"font.size": 11, "axes.spines.top": False, "axes.spines.right": False,
                     "axes.grid": True, "grid.alpha": 0.25, "figure.dpi": 150})


def medfilt(v, k):
    pad = k // 2
    vp = np.pad(v, pad, mode="edge")
    return np.array([np.median(vp[i:i + k]) for i in range(len(v))])


def load_csv(name):
    d = np.genfromtxt(D / name, delimiter=",", names=True)
    return d


# ---------------------------------------------------------------- 1. EPR sequence
def fig_epr():
    lines = TRACE_LOG.read_text().splitlines()
    msgs = []
    for ln in lines:
        m = re.match(r"\s*(\d+)\s+(<-|->)\s+(SOP'?)\s+(.*)$", ln)
        if not m:
            continue
        t, d, sop, text = int(m.group(1)), m.group(2), m.group(3), m.group(4).strip()
        if text.startswith("GoodCRC"):
            continue
        msgs.append((t, d, sop, text))
    t0 = msgs[0][0]
    keep, ka = [], 0
    for t, d, sop, text in msgs:
        if "KeepAlive" in text:
            if "Ack" not in text:
                ka += 1
            if ka > 2:
                continue
        keep.append((t - t0, d, sop, text))

    def label(d, sop, text):
        if sop == "SOP'":
            return "Discover Identity (SOP')" if "REQ" in text else "Discover Identity ACK: Apple 5 A / 50 V, EPR capable"
        text = text.replace("Extended_Control ", "")
        if text.startswith("EPR_Mode"):
            parts = text.split()
            return f"EPR_Mode {parts[1]}" + (f" ({parts[2]} W)" if parts[1] == "Enter" else "")
        if text.startswith("EPR_Source_Capabilities #1"):
            return "EPR_Source_Capabilities (chunk 0): 5 V, 9 V, 15 V @ 0.5 A"
        if text == "EPR_Source_Capabilities":
            return "chunk request" if d == "<-" else "EPR_Source_Capabilities (chunk 1)"
        if text.startswith("EPR_Request"):
            return "EPR_Request: PDO 3 (15 V), 500 mA"
        return text

    X = {"sink": 0.0, "src": 1.0, "cable": 2.0}
    fig, ax = plt.subplots(figsize=(10.5, 7.2))
    ax.set_xlim(-0.55, 2.55)
    n = len(keep)
    ax.set_ylim(n + 0.6, -1.1)
    ax.axis("off")
    for k, name in [("sink", "Sink\nNUCLEO-G431 + usbpd"), ("src", "Source\nNUCLEO-G474 + ST USB-PD 5.4.1"),
                    ("cable", "Cable e-marker\nApple 240 W (SOP')")]:
        ax.text(X[k], -0.75, name, ha="center", va="center", fontsize=11, fontweight="bold",
                bbox=dict(boxstyle="round,pad=0.4", fc="#f2f4f8", ec="#9aa3b5"))
        ax.plot([X[k], X[k]], [-0.35, n + 0.4], color="#9aa3b5", lw=1.2, zorder=0)
    for i, (dt, d, sop, text) in enumerate(keep):
        y = i + 0.25
        if sop == "SOP'":
            a, b = (X["src"], X["cable"]) if d == "->" else (X["cable"], X["src"])
            col = C_VCONN
        else:
            a, b = (X["src"], X["sink"]) if d == "->" else (X["sink"], X["src"])
            col = "#333333" if "KeepAlive" not in text else "#8a8a8a"
        ax.annotate("", xy=(b, y), xytext=(a, y), arrowprops=dict(arrowstyle="-|>", color=col, lw=1.4))
        ax.text((a + b) / 2, y - 0.12, label(d, sop, text), ha="center", va="bottom", fontsize=9.2, color=col)
        ax.text(-0.52, y, f"{dt:5d} ms", ha="left", va="center", fontsize=9, color="#555555", family="monospace")
    ax.set_title("EPR mode entry at 15 V (real VBUS): sequence on CC1, every message GoodCRC-acknowledged",
                 fontsize=12, loc="left", pad=30)
    fig.tight_layout()
    fig.savefig(D / "fig1_epr_sequence.png")
    plt.close(fig)
    return [(dt, label(d, s, t)) for dt, d, s, t in keep]


# ---------------------------------------------------------------- 2. VBUS bring-up
def fig_vbus():
    reg = [json.loads(l) for l in (D / "vbus_reg.jsonl").read_text().splitlines()]
    fig, axs = plt.subplots(1, 3, figsize=(15, 4.6), gridspec_kw={"width_ratios": [1.35, 1, 1]})
    a = load_csv("vbus_attach_5v.csv")
    t = a["t_s"] * 1000
    ax = axs[0]
    ax.plot(t, a["CHAN4"], color=C_VBUS, alpha=0.15, lw=0.6)
    ax.plot(t, medfilt(a["CHAN4"], 21), color=C_VBUS, lw=2, label="VBUS (TP2)")
    ax.plot(t, medfilt(a["CHAN3"], 21), color=C_VCONN, lw=1.6, label="CC2 = VCONN")
    ax.plot(t, medfilt(a["CHAN2"], 21), color=C_CC1, lw=1.6, label="CC1 (PD line)")
    ax.axvline(0, color="#999999", ls=":", lw=1)
    ax.set_title("a) G474 reset and fresh attach: VCONN + VBUS on, 5 V contract", loc="left", fontsize=11)
    ax.set_xlabel("time from VCONN on [ms]")
    ax.set_ylabel("volts")
    ax.set_ylim(-0.5, 6.5)
    ax.legend(loc="center right", fontsize=9, framealpha=0.9)
    ax.annotate("G474 reset:\nVCONN off, VBUS\nheld at 2.8 V by\nthe QT Py back-feed", xy=(-128, 0.3), xytext=(-200, 3.25), fontsize=8,
                arrowprops=dict(arrowstyle="->", color="#777777"))
    ax.annotate("Source_Capabilities\non CC1", xy=(30, 0.9), xytext=(160, 2.4), fontsize=8.5,
                arrowprops=dict(arrowstyle="->", color="#777777"))
    stats = {}
    for ax, name, mv, ms in [(axs[1], "vbus_5to9.csv", 9000, 182), (axs[2], "vbus_9to15.csv", 15000, 268)]:
        d = load_csv(name)
        t = d["t_s"] * 1000
        v = medfilt(d["CHAN4"], 15)
        ax.plot(t, d["CHAN4"], color=C_VBUS, alpha=0.15, lw=0.6)
        ax.plot(t, v, color=C_VBUS, lw=2, label="VBUS (TP2, scope)")
        r = next(x for x in reg if x["contract_mv"] == mv)
        ax.axhline(mv / 1000, color="#444444", ls="--", lw=1, label=f"contract {mv/1000:g} V")
        ax.axhline(r["adc_vout_mv"] / 1000, color="#e07b00", ls=":", lw=1.4,
                   label=f"power board ADC {r['adc_vout_mv']/1000:.2f} V")
        lo, hi = np.median(v[t < -20]), np.median(v[t > t.max() - 150])
        stats[mv] = (lo, hi)
        ax.set_title(f"{'b' if mv == 9000 else 'c'}) request {mv/1000:g} V: PS_RDY after {ms} ms "
                     f"(20 V/s slew)", loc="left", fontsize=11)
        ax.set_xlabel("time from trigger [ms]")
        ax.set_ylim(lo - 1.5, mv / 1000 + 1.5)
        ax.legend(loc="lower right", fontsize=8.5, framealpha=0.9)
    fig.tight_layout()
    fig.savefig(D / "fig2_vbus_bringup.png")
    plt.close(fig)
    return stats, reg


# ---------------------------------------------------------------- 3. load steps
def fig_load():
    tel = [r for r in json.loads((D / "load_steps_tel.json").read_text()) if r.get("kind") == "TEL"]
    t = np.array([r["t_s"] for r in tel])
    p = np.array([r["pout_mw"] for r in tel]) / 1000
    tgt = np.array([r["p_target"] for r in tel]) / 1000
    vo = np.array([r["vout_mv"] for r in tel]) / 1000
    du = np.array([r["duty_pm"] for r in tel]) / 10
    vi = np.array([r["vin_mv"] for r in tel]) / 1000
    run = np.array([r["state"] == "running" for r in tel])

    s = load_csv("load_steps_vbus.csv")
    st, sv = s["t_s"], s["CHAN4"]
    # Align the scope's roll record to the run: on the scope screen the
    # buck's switching widens the ripple band from -7.2 s to +4.6 s (12 s,
    # the run's length); the 50 Sa/s record is too sparse to see it.
    t_start = -7.2
    st_al = st - t_start

    fig, axs = plt.subplots(3, 1, figsize=(11, 8.6), sharex=True,
                            gridspec_kw={"height_ratios": [1.2, 1, 1]})
    ax = axs[0]
    ax.step(t, tgt, where="post", color="#444444", ls="--", lw=1.2, label="target (host)")
    ax.plot(t[run], p[run], ".", ms=3, color=C_P, label="P_out = V_out² / 10 Ω (QT Py)")
    ax.plot(t[run], p[run] * 10 / 11, ".", ms=2, color=C_P, alpha=0.35, label="into the actual 11 Ω")
    ax.set_ylabel("output power [W]")
    ax.set_ylim(-0.2, 3.6)
    ax.legend(loc="upper left", fontsize=9)
    ax.set_title("Buck load staircase under the 15 V EPR contract (0.5 → 2 → 3 W, 4 s each)", loc="left")
    ax = axs[1]
    ax.plot(t, vo, color=C_V, lw=1.6, label="+OUT (buck output)")
    ax.set_ylabel("+OUT [V]")
    ax.set_ylim(-0.3, 6.5)
    ax2 = ax.twinx()
    ax2.plot(t, du, color=C_D, lw=1.2, ls="-.", label="duty")
    ax2.set_ylabel("duty [%]")
    ax2.set_ylim(-3, 65)
    ax2.grid(False)
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=9)
    ax = axs[2]
    m = (st_al > -2) & (st_al < 14.5)
    ax.plot(st_al[m], sv[m], color=C_VBUS, alpha=0.15, lw=0.5, label="VBUS at TP2 (raw, pickup noise)")
    roll = np.convolve(sv, np.ones(50) / 50, mode="same")
    ax.plot(st_al[m], roll[m], color=C_VBUS, lw=2, label="VBUS at TP2 (scope, 1 s mean)")
    ax.plot(t, vi, color=C_VIN, lw=1.4, label="VIN at the buck (QT Py ADC)")
    ax.set_ylabel("volts")
    ax.set_ylim(11.5, 14.5)
    ax.set_xlabel("time from arming [s]")
    ax.legend(loc="lower right", fontsize=9)
    fig.tight_layout()
    fig.savefig(D / "fig3_load_steps.png")
    plt.close(fig)

    steps = []
    for i, ptarget in enumerate((500, 2000, 3000)):
        a, b = 4 * i + 2, 4 * i + 4
        sel = run & (t >= a) & (t < b)
        ms_ = (st_al >= a) & (st_al < b)
        steps.append({"target_mw": ptarget, "vout_v": vo[sel].mean(), "p_w": p[sel].mean(),
                      "p11_w": p[sel].mean() * 10 / 11, "duty_pct": du[sel].mean(), "vin_v": vi[sel].mean(),
                      "vbus_tp2_v": float(np.median(sv[ms_])) if ms_.any() else None})
    idle = (st_al < -0.5) | (st_al > 13)
    return steps, float(np.median(sv[idle])), float(t_start)


if __name__ == "__main__":
    seq = fig_epr()
    vb, reg = fig_vbus()
    steps, vbus_idle, t_start = fig_load()
    out = {"epr_sequence": seq, "vbus_levels": {str(k): v for k, v in vb.items()}, "vbus_reg": reg,
           "load_steps": steps, "vbus_idle_tp2": vbus_idle, "scope_run_start_s": t_start}
    (D / "summary.json").write_text(json.dumps(out, indent=1, default=float))
    print(json.dumps(out, indent=1, default=float))
