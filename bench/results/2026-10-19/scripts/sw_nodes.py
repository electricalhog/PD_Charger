"""Switch-node metrics from a two-channel raw scope record (t_s, CH2 = SW1, CH3 = SW2).

Per channel: levels, hard edges only (peak dV/dt >= 1.5 V/ns) for the edge statistics, switching frequency, duty, per-edge 10-90 % time (edges found with 20/80 % hysteresis), peak dV/dt,
overshoot above the high level and undershoot below the low level in the 300 ns after
the edge, ring frequency (peak of the edge-averaged spectrum of v - level, 20-300 ns after the edge), and a Hann
spectrum of the whole record in dBuV (1 uV reference, peak amplitude per bin).
Usage: sw_nodes.py CSV [CSV ...] --out metrics.json
"""
import argparse, json
from pathlib import Path
import numpy as np

BANDS_HZ = [(150e3, 500e3), (500e3, 5e6), (5e6, 30e6), (30e6, 100e6)]
WIN_NS = 300.0
HARD_V_PER_NS = 1.5   # slower crossings are the L-C ring, not switching edges


def load(p):
    a = np.loadtxt(p, delimiter=",", skiprows=1)
    return a[:, 0], {"SW1": a[:, 1], "SW2": a[:, 2]}


def hyst_edges(v, lo, hi):
    """Indices of the first sample past 80 % (rising) or under 20 % (falling), with 20/80 % hysteresis."""
    a, b = lo + 0.2 * (hi - lo), lo + 0.8 * (hi - lo)
    s = np.full(len(v), -1, np.int8)
    s[v > b] = 1
    s[v < a] = 0
    idx = np.where(s >= 0, np.arange(len(v)), 0)
    np.maximum.accumulate(idx, out=idx)
    s = s[idx]
    d = np.diff(s.astype(np.int16))
    return np.flatnonzero(d == 1) + 1, np.flatnonzero(d == -1) + 1


def interp_t(t, v, i, level):
    return t[i] + (level - v[i]) * (t[i + 1] - t[i]) / (v[i + 1] - v[i])


def edge_metrics(t, v, dt, lo, hi, idx, rising):
    l10, l90 = lo + 0.1 * (hi - lo), lo + 0.9 * (hi - lo)
    w = int(round(WIN_NS * 1e-9 / dt)); back = int(round(500e-9 / dt)); skip = int(round(20e-9 / dt))
    rows, ring = [], []
    for i in idx:
        if i - back < 0 or i + w >= len(v):
            continue
        pre = v[i - back:i + 1]
        cand = np.flatnonzero(pre <= l10) if rising else np.flatnonzero(pre >= l90)
        if not len(cand):
            continue
        j0 = cand[-1] + i - back
        fwd = v[j0:i + w]
        hit = np.flatnonzero(fwd >= l90) if rising else np.flatnonzero(fwd <= l10)
        if not len(hit):
            continue
        j1 = j0 + hit[0]
        t0 = interp_t(t, v, j0, l10 if rising else l90)
        t1 = interp_t(t, v, j1 - 1, l90 if rising else l10)
        after = v[j1:j1 + w]
        level = hi if rising else lo
        ext = float(after.max() - hi) if rising else float(lo - after.min())
        seg = v[j0:j1 + 1]
        rows.append({"t_10_90_ns": (t1 - t0) * 1e9,
                     "dvdt_max_v_per_ns": float(np.max(np.abs(np.diff(seg)))) / (dt * 1e9) if len(seg) > 1 else None,
                     "overshoot_v" if rising else "undershoot_v": ext})
        r = after[skip:] - level
        ring.append(np.abs(np.fft.rfft((r - r.mean()) * np.hanning(len(r)))))
    ring_hz = None
    if ring:
        m = np.mean(ring, axis=0); f = np.fft.rfftfreq(len(r), dt)
        k = 1 + int(np.argmax(m[1:]))
        ring_hz = float(f[k])
    return rows, ring_hz


def stat(rows, key):
    x = np.array([r[key] for r in rows if r.get(key) is not None], float)
    if not len(x):
        return None
    return {"n": int(len(x)), "median": round(float(np.median(x)), 4), "p5": round(float(np.percentile(x, 5)), 4),
            "p95": round(float(np.percentile(x, 95)), 4), "max": round(float(x.max()), 4)}


def spectrum(v, dt):
    n = len(v)
    w = np.hanning(n)
    amp = 2.0 * np.abs(np.fft.rfft((v - v.mean()) * w)) / w.sum()
    f = np.fft.rfftfreq(n, dt)
    db = 20 * np.log10(np.maximum(amp, 1e-12) / 1e-6)
    return f, amp, db


def channel(t, v, dt):
    lo, hi = float(np.percentile(v, 5)), float(np.percentile(v, 95))
    mid = 0.5 * (lo + hi)
    r_idx, f_idx = hyst_edges(v, lo, hi)
    per = np.diff(t[r_idx])
    rise, ring_r = edge_metrics(t, v, dt, lo, hi, r_idx, True)
    fall, ring_f = edge_metrics(t, v, dt, lo, hi, f_idx, False)
    hard = lambda rows: [r for r in rows if (r["dvdt_max_v_per_ns"] or 0) >= HARD_V_PER_NS]
    n_res = {"rising": len(rise) - len(hard(rise)), "falling": len(fall) - len(hard(fall))}
    rise, fall = hard(rise), hard(fall)
    f, amp, db = spectrum(v, dt)
    bands = {}
    for a, b in BANDS_HZ:
        m = (f >= a) & (f < b)
        j = np.argmax(db[m])
        bands[f"{a/1e6:g}-{b/1e6:g} MHz"] = {"peak_dbuv": round(float(db[m][j]), 1), "at_hz": round(float(f[m][j]))}
    fsw = 1.0 / float(np.median(per)) if len(per) else None
    harm = []
    if fsw:
        for h in (1, 2, 3, 5, 10, 25, 50, 100):
            j = np.argmin(np.abs(f - h * fsw))
            sl = slice(max(j - 3, 0), j + 4)
            harm.append({"harmonic": h, "hz": round(float(f[sl][np.argmax(amp[sl])])), "dbuv": round(float(db[sl].max()), 1)})
    return {"low_v_p5": round(lo, 3), "high_v_p95": round(hi, 3), "min_v": round(float(v.min()), 3), "max_v": round(float(v.max()), 3),
            "mean_v": round(float(v.mean()), 3), "duty_pct": round(float(np.mean(v > mid) * 100), 2),
            "fsw_hz": round(fsw, 1) if fsw else None, "periods": int(len(per)),
            "rising_edges": int(len(r_idx)), "resonant_crossings": n_res, "period_us_p5_p50_p95": [round(float(np.percentile(per, q)) * 1e6, 3) for q in (5, 50, 95)] if len(per) else None,
            "rise": {**{k: stat(rise, k) for k in ("t_10_90_ns", "dvdt_max_v_per_ns", "overshoot_v")}, "ring_hz_mean_spectrum": round(ring_r) if ring_r else None},
            "fall": {**{k: stat(fall, k) for k in ("t_10_90_ns", "dvdt_max_v_per_ns", "undershoot_v")}, "ring_hz_mean_spectrum": round(ring_f) if ring_f else None},
            "spectrum_bands": bands, "harmonics": harm, "bin_hz": round(float(f[1]), 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = {}
    for p in a.csv:
        t, ch = load(p)
        dt = float(np.median(np.diff(t)))
        out[Path(p).stem] = {"points": int(len(t)), "dt_ns": round(dt * 1e9, 3), "span_ms": round((t[-1] - t[0]) * 1e3, 4),
                             **{k: channel(t, v, dt) for k, v in ch.items()}}
    Path(a.out).write_text(json.dumps(out, indent=1))
    print(json.dumps(out, indent=1)[:200])


if __name__ == "__main__":
    main()
