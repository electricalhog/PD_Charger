"""Load transient record: one 12 s single-shot of VBUS covering ramp-up, hold and release.

usage: python3 trans3.py LABEL VNOM_MV P_MW VIN_MIN_MV OFFSET_V [V_PER_DIV [COUPLING]]
Above 20 V the DS1104Z cannot offset a fine scale far enough (10x probe: +-20 V
at 0.5 V/div and below), so 28 V and up use AC coupling with offset 0.
(trans2.py with the CH4 scale as an option, default 0.2 V/div; 2026-10-11/12)
The scope is forced right after the load run starts; with the trigger point
1 s from the left edge the record spans about [-1 s, +11 s].
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
CAP = HERE.parent / "data" / "cap"
BU = str(REPO / "tools" / "bringup" / "bu")
sys.path.insert(0, str(REPO / "tools" / "bringup" / "src"))
from bringup.config import load_config  # noqa: E402
from bringup.scope import Scope  # noqa: E402

label, vnom, p_mw, vin_min, offset = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
scale = float(sys.argv[6]) if len(sys.argv) > 6 else 0.2
coupling = sys.argv[7].upper() if len(sys.argv) > 7 else "DC"
q = lambda s, c: s.query(c).strip()  # noqa: E731
with Scope(load_config()) as s:
    for ch in (1, 2, 3):
        s.write(f":CHAN{ch}:DISP OFF")
    s.write(":CHAN4:DISP ON")
    s.write(f":CHAN4:COUP {coupling}")
    s.write(":CHAN4:BWL 20M")
    s.write(f":CHAN4:SCAL {scale}")
    s.write(f":CHAN4:OFFS {offset}")
    s.write(":TIM:MODE MAIN")
    s.write(":TIM:MAIN:SCAL 0.5")
    s.write(":TIM:MAIN:OFFS 2.5")
    s.write(":ACQ:TYPE HRES")
    s.write(":RUN")
    s.write(":ACQ:MDEP 12000000")
    s.write(":TRIG:SWE SING")
    time.sleep(1.0)
    s.write(":SINGle")
    time.sleep(0.3)
    t_launch = time.time()
    load = subprocess.Popen([BU, "load", "run", "--p", str(p_mw), "--seconds", "4.5", "--limit-ma", "500",
                             "--contract-mv", str(vnom), "--vin-min-mv", str(vin_min)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.4)
    t_force = time.time()
    s.write(":TFORce")
    out, err = load.communicate(timeout=60)
    t0 = time.time()
    while time.time() - t0 < 30:
        if q(s, ":TRIG:STAT?") == "STOP":
            break
        time.sleep(0.2)
    s.write(":STOP")
    time.sleep(0.3)
    st = {"timebase_s_div": float(q(s, ":TIM:MAIN:SCAL?")), "timebase_offset_s": float(q(s, ":TIM:MAIN:OFFS?")),
          "acquire_type": q(s, ":ACQ:TYPE?"), "mdepth": q(s, ":ACQ:MDEP?"), "sample_rate_sa_s": float(q(s, ":ACQ:SRAT?")),
          "channels": {"CH4": {"scale_v_div": float(q(s, ":CHAN4:SCAL?")), "offset_v": float(q(s, ":CHAN4:OFFS?")),
                               "coupling": q(s, ":CHAN4:COUP?"), "bw_limit": q(s, ":CHAN4:BWL?"),
                               "probe": float(q(s, ":CHAN4:PROB?"))}}}
    s.write(":WAV:SOUR CHAN4")
    s.write(":WAV:MODE RAW")
    s.write(":WAV:FORM BYTE")
    pre = s._preamble()
    n = int(pre["points"])
    data = bytearray()
    start = 1
    while start <= n:
        stop = min(start + 250000 - 1, n)
        s.write(f":WAV:STAR {start}")
        s.write(f":WAV:STOP {stop}")
        s.io.write(":WAV:DATA?")
        data += s.io.read_block()
        start = stop + 1
    s.write(":RUN")
    s.write(":TRIG:SWE AUTO")
(CAP / f"{label}_CH4.bin").write_bytes(data)
try:
    lj = json.loads(out)
except json.JSONDecodeError:
    lj = {"error": (out + err)[-400:]}
meta = {"label": label, "contract_mv": vnom, "p_mw": p_mw, "t_launch_unix": t_launch, "t_force_unix": t_force,
        "settings": st, "waveforms": {"CH4": {"preamble": pre, "points": len(data),
                                              "clipped": sum(1 for b in data if b in (0, 255))}},
        "load": lj}
(CAP / f"{label}.json").write_text(json.dumps(meta, indent=1))
print(json.dumps({"label": label, "points": len(data), "srate": st["sample_rate_sa_s"],
                  "clipped": meta["waveforms"]["CH4"]["clipped"], "load_ok": lj.get("ok"), "fault": lj.get("fault"),
                  "load_t0": lj.get("t0_unix"), "force": round(t_force, 3)}))
