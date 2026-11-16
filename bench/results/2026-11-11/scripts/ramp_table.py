"""Per-point table of the power ramp (bench 2026-11-11 section 5): board means over each point's window, the scope's
V_in / V_out from the DC capture's trace samples, efficiency both ways.  Writes ramp/points.json.  Run with system
python3 (numpy) from the repository root.

Scope voltages are the MEDIAN of the trace samples (Dan, 2026-11-11: the scope is accurate that way; its own
"measured" values are not), interpolated inside the 92 mV code step so the result is not rounded to a code.  The
plain median and the mean are kept in points.json for comparison."""
import json, sys
from pathlib import Path
import numpy as np

R = Path("bench/results/2026-11-11/ramp")

def cap(name):
    """A capture CSV by the name ramp.json gives it; committed gzipped (numpy reads .csv.gz directly)."""
    p = R / name
    return p if p.exists() else p.with_name(p.name + ".gz")



def med(x):
    """Median with linear interpolation inside the ADC code step (the grouped-data median)."""
    u = np.unique(x); m = float(np.median(x))
    if len(u) < 2:
        return m
    lsb = float(np.min(np.diff(u)))
    below = np.count_nonzero(x < m - lsb / 2); at = np.count_nonzero(np.abs(x - m) <= lsb / 2)
    return m - lsb / 2 + lsb * (len(x) / 2 - below) / at if at else m


ramp = json.loads((R / "ramp.json").read_text())
out = []
for ph in ramp["phases"]:
    rows = json.loads((R / f"board_{ph['mode']}.json").read_text())
    for pt in ph["points"]:
        w = [r for r in rows if pt["t_set"] + 1.5 <= r["t"] <= pt["t_cap1"] and r["state"] == "RUNNING"]
        m = lambda k: float(np.mean([r[k] for r in w]))
        d = np.genfromtxt(cap(pt["dc"]["file"]), delimiter=",", names=True)
        vin_s, vout_s = med(d["CHAN2"]), med(d["CHAN4"])
        sw1 = d["CHAN1"]; t = d["t_s"]
        thr = 12.0
        edges = np.flatnonzero((sw1[:-1] < thr) & (sw1[1:] >= thr))
        f_sw = float(1.0 / np.median(np.diff(t[edges]))) if len(edges) > 2 else None
        i_out, i_in = m("ccm_i_avg_ma") / 1e3, m("i_in_filt_ma") / 1e3
        vin_b, vout_b = m("v_in_mv") / 1e3, m("v_out_mv") / 1e3
        o = {"mode": ph["mode"], "set_ma": pt["set_ma"], "n": len(w), "ccm": m("ccm_active"), "lim": m("ccm_limiting"),
             "sr_enable": m("sr_enable"), "sr_blend": m("sr_blend"), "fsw_khz_board": m("fsw_khz"), "fsw_khz_scope": f_sw and f_sw / 1e3,
             "i_out_a": i_out, "i_in_a": i_in, "vin_board": vin_b, "vout_board": vout_b, "vin_scope": vin_s, "vout_scope": vout_s,
             "p_out_scope_w": vout_s * i_out, "p_in_scope_w": vin_s * i_in, "eff_scope": vout_s * i_out / (vin_s * i_in),
             "p_out_board_w": vout_b * i_out, "eff_board": vout_b * i_out / (vin_b * i_in),
             "vin_scope_mean": float(d["CHAN2"].mean()), "vout_scope_mean": float(d["CHAN4"].mean()),
             "vin_scope_median_code": float(np.median(d["CHAN2"])), "vout_scope_median_code": float(np.median(d["CHAN4"])),
             "vout_pp_scope": float(d["CHAN4"].max() - d["CHAN4"].min()), "pack_read_mv": ph["pack_read_mv"],
             "dc_file": pt["dc"]["file"], "ac_file": pt.get("ac", {}).get("file"), "ac_scale": pt.get("ac", {}).get("scale_v_div"),
             "warnings": pt["dc"]["warnings"] + pt.get("ac", {}).get("warnings", [])}
        out.append(o)
        print(f"{o['mode']} {o['set_ma']:5d} n{o['n']:2d} ccm {o['ccm']:.0f} lim {o['lim']:.1f} sr {o['sr_enable']:.0f}/{o['sr_blend']:.0f} "
              f"f {o['fsw_khz_board']:.0f}/{(o['fsw_khz_scope'] or 0):.0f}k Iout {i_out:.3f} Iin {i_in:.3f} Vin b{vin_b:.2f} s{vin_s:.2f} "
              f"Vout b{vout_b:.2f} s{vout_s:.2f} Pout {o['p_out_scope_w']:.1f} W eff scope {100*o['eff_scope']:.1f}% board {100*o['eff_board']:.1f}% "
              f"loss {o['p_in_scope_w']-o['p_out_scope_w']:.2f} W pp {o['vout_pp_scope']:.2f} {o['warnings']}")
(R / "points.json").write_text(json.dumps(out, indent=1))
