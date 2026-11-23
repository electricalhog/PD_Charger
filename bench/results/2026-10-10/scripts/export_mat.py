"""Export every capture to MATLAB .mat (v5, compressed): raw 8-bit codes plus scaling.

In MATLAB:  v = (double(code) - yorig - yref) * yinc;   t = xorig + (0:numel(v)-1)' * xinc;
"""
import json
import sys
from pathlib import Path

import numpy as np
from scipy.io import savemat

CAP = Path(sys.argv[1])
OUT = Path(sys.argv[2])
OUT.mkdir(parents=True, exist_ok=True)
OPS = {"v05_p0": (5, 0), "v09_p0": (9, 0), "v15_p0": (15, 0), "v15_p500": (15, 0.5), "v15_p2000": (15, 2),
       "v15_p3000": (15, 3), "v20_p0": (20, 0), "v20_p500": (20, 0.5), "v20_p2000": (20, 2), "v20_p3000": (20, 3)}
KIND = {"hf": "9.6 ms single, 20 MHz bandwidth limit", "hfw": "9.6 ms single, full bandwidth",
        "lf": "600 ms single, 20 MHz bandwidth limit, HRES at 2 MSa/s"}
index = []
for js in sorted(CAP.glob("*.json")):
    lab = js.stem
    if lab.endswith("_op") or not (CAP / f"{lab}_CH4.bin").exists():
        continue
    m = json.loads(js.read_text())
    if "waveforms" not in m or m["waveforms"]["CH4"]["points"] == 0:
        continue
    base, _, kind = lab.rpartition("_")
    if lab.startswith("u1") or lab.startswith("u2"):
        base, kind = lab, "transient"
    if base not in OPS and kind != "transient":
        continue
    p = m["waveforms"]["CH4"]["preamble"]
    ch = m["settings"]["channels"]["CH4"]
    code = np.fromfile(CAP / f"{lab}_CH4.bin", dtype=np.uint8)
    cv, lw = (OPS[base] if base in OPS else (int(lab[1:3]), 3))
    d = {"code": code.reshape(-1, 1), "yinc": p["yinc"], "yorig": p["yorig"], "yref": p["yref"],
         "xinc": p["xinc"], "xorig": p["xorig"], "fs": 1.0 / p["xinc"], "label": lab,
         "contract_v": float(cv), "load_target_w": float(lw), "kind": KIND.get(kind, "6 s single at 2 MSa/s across a 0->3 W load run"),
         "v_div": ch["scale_v_div"], "offset_v": ch["offset_v"], "coupling": ch["coupling"], "bw_limit": ch["bw_limit"],
         "probe": ch["probe"], "acquire": m["settings"]["acquire_type"], "clipped_samples": m["waveforms"]["CH4"]["clipped"]}
    if kind == "transient":
        d["load_arm_s"] = m["load"]["t0_unix"] - m["t_force_unix"]
    savemat(OUT / f"{lab}.mat", d, do_compression=True)
    index.append({"file": f"{lab}.mat", "label": lab, "kind": d["kind"], "fs": d["fs"], "points": int(code.size),
                  "contract_v": cv, "load_target_w": lw})
(OUT / "index.json").write_text(json.dumps(index, indent=1))
tot = sum((OUT / i["file"]).stat().st_size for i in index)
print(f"{len(index)} files, {tot / 1e6:.1f} MB")
