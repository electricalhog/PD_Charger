"""Load-step metrics for a trans3.py capture (VBUS on CH4, any coupling), as in 2026-10-10.

usage: python3 transient.py LABEL [LABEL ...]   (captures and TEL frames in ../data/cap)

Levels are relative to the no-load stretch before the run, so AC coupling (needed above
20 V on the DS1104Z) changes only the DC shift, which then decays with the ~10 Hz corner.
  dip       lowest 20 kHz-low-passed point within 0.4 s after the run starts
  ring      extrema (prominence >= 15 mV) of a 2 kHz low-pass over 12 ms after the dip,
            merged to alternate in sign; frequency 1/(trough to next trough),
            decay = ratio of the two troughs, zeta = ln(decay)/sqrt(4 pi^2 + ln(decay)^2)
  release   steepest 1 kHz-low-passed rise within 150 ms of the last running TEL frame;
            overshoot = highest 20 kHz-low-passed point from 2 ms before to 50 ms after
  back in band  first time the 20 kHz low-pass is inside the no-load 0.1..99.9 % band
            (dip: upper edge +50 mV) and stays 10 ms
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal

CAP = Path(__file__).resolve().parent.parent / "data" / "cap"


def transient(label):
    m = json.loads((CAP / f"{label}.json").read_text())
    p = m["waveforms"]["CH4"]["preamble"]
    c = np.fromfile(CAP / f"{label}_CH4.bin", dtype=np.uint8)
    v = (c.astype(np.float64) - p["yorig"] - p["yref"]) * p["yinc"]
    fs = 1.0 / p["xinc"]
    t = p["xorig"] + np.arange(len(v)) / fs
    ld = m["load"]
    frames = json.loads((CAP / Path(ld["file"]).name).read_text())
    arm = ld["t0_unix"] - m["t_force_unix"]
    ft = np.array([fr["t_s"] for fr in frames]) + arm
    run = np.array([fr["state"] == "running" for fr in frames])
    sos = lambda f: signal.butter(4, f, "lp", fs=fs, output="sos")
    lp20k, lp2k, lp1k = (signal.sosfiltfilt(sos(f), v) for f in (20e3, 2e3, 1e3))
    t_last = ft[run][-1]
    w = (t > t_last - 0.05) & (t < t_last + 0.15)
    t_rel = float(t[w][np.argmax(np.gradient(lp1k[w], t[w]))])
    seg_b = (t > t.min() + 0.05) & (t < arm - 0.02)
    seg_a = (t > t_rel + 0.3) & (t < t.max() - 0.05)
    ref_b, ref_a = float(np.median(lp20k[seg_b])), float(np.median(lp20k[seg_a]))
    band = lambda seg, ref: (float(np.percentile(lp20k[seg], 0.1)) - ref, float(np.percentile(lp20k[seg], 99.9)) - ref)
    bb, ba = band(seg_b, ref_b), band(seg_a, ref_a)

    def recover(t0, ref, lo, hi, hold_s=0.010, search_s=0.3):
        ww = (t >= t0) & (t <= t0 + search_s)
        inside = (lp20k[ww] - ref >= lo) & (lp20k[ww] - ref <= hi)
        need, run_ = int(hold_s * fs), 0
        for i, ok in enumerate(inside):
            run_ = run_ + 1 if ok else 0
            if run_ >= need:
                return 1e3 * float(t[ww][i - need + 1] - t0)
        return float("nan")

    wa = (t > arm) & (t < arm + 0.4)
    i_dip = int(np.argmin(lp20k[wa])); t_dip = float(t[wa][i_dip])
    dip = 1e3 * (float(lp20k[wa][i_dip]) - ref_b)
    rec_dip = recover(t_dip, ref_b, bb[0], bb[1] + 0.05)
    wg = (t >= t_dip - 0.001) & (t <= t_dip + 0.012)
    yy, tt = 1e3 * (lp2k[wg] - ref_b), 1e3 * (t[wg] - t_dip)
    idx = np.concatenate([signal.find_peaks(yy, prominence=15)[0], signal.find_peaks(-yy, prominence=15)[0]])
    ext = []
    for i in sorted(idx):
        if ext and np.sign(yy[i]) == np.sign(ext[-1][1]):
            if abs(yy[i]) > abs(ext[-1][1]):
                ext[-1] = (float(tt[i]), float(yy[i]))
        else:
            ext.append((float(tt[i]), float(yy[i])))
    k0 = next((k for k, e in enumerate(ext) if e[1] < 0 and e[0] > -0.5), None)
    ring = ext[k0:] if k0 is not None else []
    if len(ring) >= 3 and ring[2][1] < 0:
        ring_hz, decay = 1e3 / (ring[2][0] - ring[0][0]), ring[0][1] / ring[2][1]
        zeta = float(np.log(decay) / np.sqrt(4 * np.pi ** 2 + np.log(decay) ** 2))
    else:
        ring_hz = decay = zeta = float("nan")
    wr = (t >= t_rel - 0.002) & (t <= t_rel + 0.05)
    i_pk = int(np.argmax(lp20k[wr]))
    peak = 1e3 * (float(lp20k[wr][i_pk]) - ref_a)
    rec_rel = recover(float(t[wr][i_pk]), ref_a, ba[0], ba[1])
    clipped = int(((c == 0) | (c == 255)).sum())
    return {"label": label, "coupling": m["settings"]["channels"]["CH4"]["coupling"],
            "band_before_mv": [round(1e3 * x) for x in bb], "dip_mv": round(dip), "dip_back_in_band_ms": round(rec_dip, 1),
            "ring_extrema_ms_mv": [(round(a, 2), round(b)) for a, b in ring[:4]], "ring_hz": round(ring_hz),
            "decay_per_cycle": round(decay, 2), "zeta": round(zeta, 2),
            "release_overshoot_mv": round(peak), "release_back_in_band_ms": round(rec_rel, 1),
            "band_after_mv": [round(1e3 * x) for x in ba], "clipped_samples": clipped,
            "arm_s": round(arm, 3), "dip_t_s": round(t_dip, 4), "release_t_s": round(t_rel, 4)}


if __name__ == "__main__":
    for lab in sys.argv[1:]:
        print(json.dumps(transient(lab)))
