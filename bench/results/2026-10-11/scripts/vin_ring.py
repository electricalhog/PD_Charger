"""V_in and V_out as the PID loop saw them: the firmware's 20 kHz debug ring (debug_log), read over SWD.

usage:
  uv run --project tools/bringup --with numpy python vin_ring.py read P_MW SECONDS OUT.json
      hold the QT Py load at P_MW (0: no load run) at the 28 V contract and read the ring
      repeatedly for SECONDS - 3 s (the core keeps running)
  python3 vin_ring.py summary LABEL=FILE.json [LABEL=FILE.json ...]
      statistics of saved reads, to ../metrics/vin_ring.json

The ring holds 512 DebugSample records (24 B: v_out_mv, v_in_mv, i_inductor_ma, i_out_ma,
dac_counts, state, mode, error_mv), one per PID cycle.  A read of all 12 KB takes longer
than the 25.6 ms the ring spans, so a read is a mix of cycles from several laps: fine for
statistics, not a time series.  v_in_mv is the newest ADC2 result; before the 2026-10-12
fix (data/ring/*_before.json) it was the single 2.5-cycle sample the modulator used.
"""
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
SAMPLE = np.dtype([("vo", "<u4"), ("vi", "<u4"), ("il", "<u4"), ("io", "<u4"), ("dac", "<u2"),
                   ("st", "u1"), ("md", "u1"), ("err", "<i4")])
RING = 512


def read(p_mw, seconds, out):
    sys.path.insert(0, str(REPO / "tools" / "bringup" / "src"))
    from bringup import probe
    from bringup.config import load_config
    cfg = load_config()
    syms = probe.symbols(cfg, "")
    regions = [(syms["debug_log"][0], RING * SAMPLE.itemsize + 8), (syms["regulator_skip_events"][0], 4),
               (syms["regulator_on_time_ns"][0], 2), (syms["regulator_i_cmd_ma"][0], 4)]
    load = None
    if p_mw > 0:
        load = subprocess.Popen([str(REPO / "tools" / "bringup" / "bu"), "load", "run", "--p", str(p_mw),
                                 "--seconds", str(seconds), "--limit-ma", "500", "--contract-mv", "28000",
                                 "--vin-min-mv", "25000"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(2.0)
    recs, t0 = [], time.time()
    while time.time() - t0 < seconds - 3.0:
        a, b, c, d = probe.read_regions(cfg, regions)
        s = np.frombuffer(a[:RING * SAMPLE.itemsize], dtype=SAMPLE)
        wi, tc = struct.unpack("<II", a[RING * SAMPLE.itemsize:])
        recs.append({"t": time.time() - t0, "wi": wi, "tc": tc, "skip": struct.unpack("<I", b)[0],
                     "ton": struct.unpack("<H", c)[0], "icmd": struct.unpack("<f", d)[0],
                     "vo": s["vo"].tolist(), "vi": s["vi"].tolist(), "md": s["md"].tolist(), "err": s["err"].tolist()})
    if load:
        load.communicate(timeout=60)
    Path(out).write_text(json.dumps(recs))


def stats(recs):
    vi = np.concatenate([r["vi"] for r in recs]).astype(float)
    vo = np.concatenate([r["vo"] for r in recs]).astype(float)
    med = float(np.median(vi))
    out = {"reads": len(recs), "cycles": int(len(vi)), "duration_s": round(recs[-1]["t"] - recs[0]["t"], 1),
           "skip_transitions": recs[-1]["skip"] - recs[0]["skip"],
           "i_cmd_ma_mean": round(float(np.mean([r["icmd"] for r in recs])), 1),
           "on_time_ns_mean": round(float(np.mean([r["ton"] for r in recs])), 1)}
    for name, x in (("v_in", vi), ("v_out", vo)):
        out[name] = {"median_mv": float(np.median(x)), "sd_mv": round(float(np.std(x)), 1),
                     "min_mv": float(x.min()), "max_mv": float(x.max()),
                     "pct_0.1_99.9_mv": [float(v) for v in np.percentile(x, [0.1, 99.9])]}
    out["v_in"]["cycles_off_median"] = {f">{th}mV": {"high": int((vi > med + th).sum()), "low": int((vi < med - th).sum())}
                                       for th in (500, 1000, 2000, 3000)}
    return out


def summary(pairs):
    res = {}
    for pair in pairs:
        label, f = pair.split("=", 1)
        res[label] = stats(json.loads(Path(f).read_text()))
    out = HERE.parent / "metrics" / "vin_ring.json"
    out.write_text(json.dumps(res, indent=1) + "\n")
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    if sys.argv[1] == "read":
        read(int(sys.argv[2]), float(sys.argv[3]), sys.argv[4])
    else:
        summary(sys.argv[2:])
