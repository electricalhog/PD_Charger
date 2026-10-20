"""Per-period structure of the 4-switch buck-boost from SW1 (CH2) and SW2 (CH3) raw records.

Hard edges: clusters of |dV/dt| > 1.5 V/ns (4 ns difference) closer than 60 ns, with a step
of > 5 V between the median of -60..-20 ns before and +40..+80 ns after the cluster.
Period = SW1 hard rise with SW2 low (< 8 V 30 ns later, Q4 on) to the next such rise. In each period:
  t1 charge     SW1 hard rise -> SW2 hard rise (Q1 + Q4 on, L charges from V_in)
  t2 transfer   SW2 hard rise -> SW2 under half its plateau or SW1 over V_in/2, whichever first
                (L discharges into V_out; with SR off SW2 stays high and SW1 snaps up instead)
  ring          rest of the period; differential v = SW1 - SW2 rings on L and the node capacitance
I_pk = V_in * t1 / L and the charge to the output 0.5 * I_pk * t2 per period, with V_in from --vin.
Ring (only with >= 900 ns of ring, >= 4 half-cycles and > 2 V): frequency from the zero crossings of the differential about its ring mean,
C_eff = 1 / ((2 pi f)^2 L), amplitude of the first and last half-cycles, Q from their decay.
Usage: sw_periods.py CSV --vin V --label NAME [--out metrics.json (merged)]
"""
import argparse, json
from pathlib import Path
import numpy as np

L_H = 4.7e-6


def hard_edges(t, v, dt):
    d = (v[4:] - v[:-4]) / (4 * dt * 1e9)
    hit = np.flatnonzero(np.abs(d) > 1.5)
    if not len(hit):
        return []
    gap = int(60e-9 / dt)
    groups = np.split(hit, np.flatnonzero(np.diff(hit) > gap) + 1)
    out = []
    for g in groups:
        a, b = g[0], g[-1] + 4
        pre = v[max(a - int(60e-9 / dt), 0):max(a - int(20e-9 / dt), 1)]
        post = v[b + int(40e-9 / dt):b + int(80e-9 / dt)]
        if not len(pre) or not len(post):
            continue
        v0, v1 = float(np.median(pre)), float(np.median(post))
        if abs(v1 - v0) < 5:
            continue
        mid = 0.5 * (v0 + v1)
        seg = v[a:b + 1]
        k = np.flatnonzero(np.diff(np.sign(seg - mid)) != 0)
        i = a + (k[0] if len(k) else 0)
        tm = t[i] + (mid - v[i]) * dt / (v[i + 1] - v[i]) if v[i + 1] != v[i] else t[i]
        win = v[a:b + int(100e-9 / dt)]
        out.append({"t": float(tm), "rising": v1 > v0, "v_before": v0, "v_after": v1,
                    "peak": float(win.max() if v1 > v0 else win.min())})
    return out


def analyse(path, vin, label):
    a = np.loadtxt(path, delimiter=",", skiprows=1)
    t, s1, s2 = a[:, 0], a[:, 1], a[:, 2]
    dt = float(np.median(np.diff(t)))
    e1, e2 = hard_edges(t, s1, dt), hard_edges(t, s2, dt)
    def sw2_at(tt):
        return float(s2[min(int(np.searchsorted(t, tt + 30e-9)), len(s2) - 1)])
    r1 = [e for e in e1 if e["rising"] and sw2_at(e["t"]) < 8.0]
    r2 = [e for e in e2 if e["rising"]]
    f2 = [e for e in e2 if not e["rising"]]
    rows = []
    for p, q in zip(r1, r1[1:]):
        t0, t3 = p["t"], q["t"]
        c = [e for e in r2 if t0 < e["t"] < t3]
        if len(c) != 1:
            continue
        e = c[0]
        i1 = int(np.searchsorted(t, e["t"] + 100e-9)); i3 = int(np.searchsorted(t, t3))
        plat = float(np.median(s2[i1:i1 + int(200e-9 / dt)]))
        below = np.flatnonzero((s2[i1:i3] < 0.5 * plat) | (s1[i1:i3] > 0.5 * vin))
        if not len(below):
            continue
        i2 = i1 + below[0]
        t2 = float(t[i2])
        # ring on the differential, from t2 + 50 ns to 50 ns before the next start
        ra, rb = i2 + int(50e-9 / dt), i3 - int(50e-9 / dt)
        ring = None
        if rb - ra > int(900e-9 / dt):   # at least two periods of a ~2.4 MHz ring
            d = s1[ra:rb] - s2[ra:rb]
            m = float(np.mean(d))
            zc = np.flatnonzero(np.diff(np.sign(d - m)) != 0)
            f = 1.0 / (2 * float(np.median(np.diff(zc) * dt))) if len(zc) >= 5 else None
            ok = f is not None and f <= 20e6 and float(np.max(np.abs(d - m))) > 2.0   # > 20 MHz: edge ringing
            if ok:
                amps = [float(np.max(np.abs(d[zc[k]:zc[k + 1]] - m))) for k in range(len(zc) - 1)]
                n = len(amps) - 1
                q_fac = (np.pi * n / np.log(amps[0] / amps[-1])) / 2 if n >= 2 and amps[-1] > 0 and amps[0] > amps[-1] else None
                ring = {"f_hz": f, "a_first_v": amps[0], "a_last_v": amps[-1], "half_cycles": len(amps), "mean_v": m, "q": q_fac}
        t1, tr = e["t"] - t0, t2 - e["t"]
        ipk = vin * t1 / L_H
        rows.append({"period_us": (t3 - t0) * 1e6, "t1_us": t1 * 1e6, "t2_us": tr * 1e6, "ring_us": (t3 - t2) * 1e6,
                     "ipk_a": ipk, "q_out_uc": 0.5 * ipk * tr * 1e6, "sw1_before_on_v": p["v_before"],
                     "sw2_before_on_v": None, "sw2_peak_v": e["peak"], "sw2_plateau_v": plat, "ring": ring})
    # SW2 level just before Q4 turns on (hard fall at the period start)
    for r, p in zip(rows, r1):
        c = [e for e in f2 if abs(e["t"] - p["t"]) < 100e-9]
        r["sw2_before_on_v"] = c[0]["v_before"] if c else None

    def st(key, sub=None):
        x = np.array([(r[sub][key] if sub else r[key]) for r in rows if (r.get(sub) if sub else True) and ((r[sub][key] if sub else r[key]) is not None)], float)
        if not len(x):
            return None
        return {"n": int(len(x)), "median": round(float(np.median(x)), 4), "p5": round(float(np.percentile(x, 5)), 4),
                "p95": round(float(np.percentile(x, 95)), 4), "min": round(float(x.min()), 4), "max": round(float(x.max()), 4)}

    span = float(t[-1] - t[0])
    res = {"file": Path(path).name, "vin_v": vin, "l_uh": L_H * 1e6, "span_ms": round(span * 1e3, 4),
           "sw1_hard_rises": len(r1), "sw2_hard_rises": len(r2), "periods_parsed": len(rows)}
    for k in ("period_us", "t1_us", "t2_us", "ring_us", "ipk_a", "q_out_uc", "sw1_before_on_v", "sw2_before_on_v", "sw2_peak_v", "sw2_plateau_v"):
        res[k] = st(k)
    for k in ("f_hz", "a_first_v", "a_last_v", "half_cycles", "mean_v", "q"):
        res["ring_" + k] = st(k, "ring")
    if res["ring_f_hz"]:
        f = res["ring_f_hz"]["median"]
        res["ring_c_eff_pf"] = round(1e12 / ((2 * np.pi * f) ** 2 * L_H), 1)
    qsum = sum(r["q_out_uc"] for r in rows) * 1e-6
    tcov = sum(r["period_us"] for r in rows) * 1e-6
    res["i_out_from_timing_a"] = round(qsum / tcov, 4) if tcov else None
    return label, res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv"); ap.add_argument("--vin", type=float, required=True)
    ap.add_argument("--label", required=True); ap.add_argument("--out")
    a = ap.parse_args()
    label, res = analyse(a.csv, a.vin, a.label)
    if a.out:
        p = Path(a.out)
        d = json.loads(p.read_text()) if p.exists() else {}
        d[label] = res
        p.write_text(json.dumps(d, indent=1))
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
