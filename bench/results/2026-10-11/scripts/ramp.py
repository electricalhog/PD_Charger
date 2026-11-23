"""Ramp metrics from a grab.py capture of VBUS (CH4): slope, end-of-ramp overshoot, settling.

usage: python3 ramp.py LABEL [LABEL ...]   (captures in ../data/cap)
VBUS is low-passed at 20 kHz (zero phase) before the edge and peak measurements.
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.ndimage import uniform_filter1d

CAP = Path(__file__).resolve().parent.parent / "data" / "cap"


def load(label, ch="CH4"):
    m = json.loads((CAP / f"{label}.json").read_text())
    p = m["waveforms"][ch]["preamble"]
    c = np.fromfile(CAP / f"{label}_{ch}.bin", dtype=np.uint8).astype(float)
    v = (c - p["yorig"] - p["yref"]) * p["yinc"]
    t = p["xorig"] + np.arange(len(v)) * p["xinc"]
    return t, v, 1.0 / p["xinc"]


def metrics(label):
    t, v, fs = load(label)
    lp = signal.sosfiltfilt(signal.butter(4, 20e3, "lp", fs=fs, output="sos"), v)
    k = int(fs * 1e-3)
    ma = uniform_filter1d(lp, k, mode="nearest")                    # 1 ms mean (edges held, not zero-padded)
    v0, v1 = np.median(ma[: int(0.05 * fs)]), np.median(ma[-int(0.1 * fs):])
    up = v1 > v0
    span = v1 - v0
    lvl = lambda f: v0 + f * span
    cross = lambda f: t[np.argmax((ma > lvl(f)) if up else (ma < lvl(f)))]
    t10, t90 = cross(0.1), cross(0.9)
    slope = 0.8 * span / (t90 - t10)
    after = t > t90
    ext = (lp[after].max() if up else lp[after].min())
    over_mv = 1e3 * (ext - v1) if up else 1e3 * (v1 - ext)          # positive = past the final value
    def settle(band_v):
        inside = np.abs(lp - v1) <= band_v
        idx = np.where(after & ~inside)[0]
        return float(t[idx[-1]] - t90) * 1e3 if len(idx) else 0.0
    ripple = 1e3 * np.ptp(lp[t > t[-1] - 0.1])
    return {"label": label, "from_v": round(v0, 3), "to_v": round(v1, 3), "slope_v_per_s": round(slope, 1),
            "t10_90_ms": round(1e3 * (t90 - t10), 1), "overshoot_mv": round(over_mv, 1),
            "overshoot_pct": round(100 * over_mv / 1e3 / abs(v1), 2),
            "settle_5pct_ms": round(settle(0.05 * abs(v1)), 1), "settle_1pct_ms": round(settle(0.01 * abs(v1)), 1),
            "final_ripple_20k_mvpp": round(ripple, 1)}


if __name__ == "__main__":
    for lab in sys.argv[1:]:
        print(json.dumps(metrics(lab)))
