"""VBUS/VCONN spike coincidence in the two-probe capture (survey_5v: CH3 = VCONN, CH4 = VBUS, 5 V contract).

Both channels are high-passed at 2 MHz; a spike is |x| > 5 sigma of its channel; VBUS samples within
10 samples of an event's start belong to that event; an event counts as coincident when VCONN has a spike
within +-w samples.  Writes metrics/spikes.json.
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal

HERE = Path(__file__).resolve().parent
CAP = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "data" / "cap"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parent / "metrics"

m = json.loads((CAP / "survey_5v.json").read_text())
v = {}
for ch, w in m["waveforms"].items():
    p = w["preamble"]
    c = np.fromfile(CAP / f"survey_5v_{ch}.bin", dtype=np.uint8).astype(np.float64)
    v[ch] = (c - p["yorig"] - p["yref"]) * p["yinc"]
fs = 1.0 / m["waveforms"]["CH4"]["preamble"]["xinc"]
sos = signal.butter(4, 2e6, "hp", fs=fs, output="sos")
h3, h4 = signal.sosfiltfilt(sos, v["CH3"]), signal.sosfiltfilt(sos, v["CH4"])
idx = np.where(np.abs(h4) > 5 * h4.std())[0]
ev = [int(idx[0])] if len(idx) else []
for i in idx[1:]:
    if i - ev[-1] > 10:
        ev.append(int(i))
w = int(40e-9 * fs) + 1
co = sum(1 for e in ev if np.max(np.abs(h3[max(0, e - w):e + w + 1])) > 5 * h3.std())
res = {"fs": fs, "points": len(h4), "record_ms": 1e3 * len(h4) / fs, "vbus_spike_events": len(ev),
       "with_vconn_spike": co, "fraction": co / len(ev), "window_ns": 1e9 * w / fs,
       "vbus_hp2mhz_rms_mv": 1e3 * float(h4.std()), "vconn_hp2mhz_rms_mv": 1e3 * float(h3.std()),
       "corr_hp2mhz": float(np.corrcoef(h3, h4)[0, 1]),
       "median_event_interval_us": 1e6 * float(np.median(np.diff(ev)) / fs) if len(ev) > 1 else float("nan")}
(OUT / "spikes.json").write_text(json.dumps(res, indent=1))
print(json.dumps(res, indent=1))
