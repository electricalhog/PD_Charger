"""Single-shot raw captures from the DS1104Z for noise and transient analysis.

Runs in the bring-up venv (scope driver, no numpy).  Each capture saves the
8-bit ADC codes per channel (<label>_CHn.bin) and one <label>.json with the
waveform preamble per channel and every setting that affects the numbers.
Volts = (code - yorigin - yreference) * yincrement; t = xorigin + i * xincrement.
"""
import argparse
import json
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools" / "bringup" / "src"))
from bringup.config import load_config  # noqa: E402
from bringup.scope import Scope  # noqa: E402


def q(s, cmd):
    return s.query(cmd).strip()


def configure(s, a):
    for ch in (1, 2, 3, 4):
        on = ch in a.chans
        s.write(f":CHAN{ch}:DISP {'ON' if on else 'OFF'}")
    for ch in a.chans:
        s.write(f":CHAN{ch}:COUP {a.coupling}")
        s.write(f":CHAN{ch}:BWL {a.bwl}")
        s.write(f":CHAN{ch}:SCAL {a.scale[ch] if isinstance(a.scale, dict) else a.scale}")
        s.write(f":CHAN{ch}:OFFS {a.offset[ch] if isinstance(a.offset, dict) else a.offset}")
    s.write(":TIM:MODE MAIN")
    s.write(f":TIM:MAIN:SCAL {a.tb}")
    s.write(f":TIM:MAIN:OFFS {a.tboff}")
    s.write(f":ACQ:TYPE {a.atype}")
    s.write(":RUN")
    s.write(f":ACQ:MDEP {a.mdepth}")
    if a.trig:
        src, slope, level = a.trig.split(",")
        s.write(":TRIG:MODE EDGE")
        s.write(f":TRIG:EDG:SOUR {src}")
        s.write(f":TRIG:EDG:SLOP {slope}")
        s.write(f":TRIG:EDG:LEV {level}")
        s.write(f":TRIG:COUP {a.trigcoup}")
        s.write(":TRIG:SWE SING")
    else:
        s.write(":TRIG:SWE AUTO")


def settings(s, chans):
    st = {"timebase_s_div": float(q(s, ":TIM:MAIN:SCAL?")), "timebase_offset_s": float(q(s, ":TIM:MAIN:OFFS?")),
          "acquire_type": q(s, ":ACQ:TYPE?"), "mdepth": q(s, ":ACQ:MDEP?"),
          "sample_rate_sa_s": float(q(s, ":ACQ:SRAT?")), "channels": {}}
    for ch in chans:
        st["channels"][f"CH{ch}"] = {
            "scale_v_div": float(q(s, f":CHAN{ch}:SCAL?")), "offset_v": float(q(s, f":CHAN{ch}:OFFS?")),
            "coupling": q(s, f":CHAN{ch}:COUP?"), "bw_limit": q(s, f":CHAN{ch}:BWL?"),
            "probe": float(q(s, f":CHAN{ch}:PROB?"))}
    return st


def read_raw(s, ch):
    s.write(f":WAV:SOUR CHAN{ch}")
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
    return pre, bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "data" / "cap"))
    ap.add_argument("--chans", default="4")
    ap.add_argument("--scale", default="0.2", help="V/div, or ch:v,ch:v")
    ap.add_argument("--offset", default="0", help="V, or ch:v,ch:v")
    ap.add_argument("--coupling", default="DC")
    ap.add_argument("--bwl", default="20M")
    ap.add_argument("--tb", type=float, default=0.0005)
    ap.add_argument("--tboff", type=float, default=0.0)
    ap.add_argument("--mdepth", default="1200000")
    ap.add_argument("--atype", default="HRES")
    ap.add_argument("--settle", type=float, default=1.0, help="s between RUN and STOP (AUTO sweep)")
    ap.add_argument("--trig", default="", help="SOURCE,POS|NEG,level: single-shot on an edge")
    ap.add_argument("--trigcoup", default="HFR")
    ap.add_argument("--wait", type=float, default=30.0, help="max s to wait for a single-shot trigger")
    ap.add_argument("--no-config", action="store_true", help="reuse the scope's current setup")
    ap.add_argument("--note", default="")
    a = ap.parse_args()
    a.chans = [int(c) for c in a.chans.split(",")]
    for k in ("scale", "offset"):
        v = getattr(a, k)
        setattr(a, k, {int(x.split(":")[0]): float(x.split(":")[1]) for x in v.split(",")} if ":" in v else float(v))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cfg = load_config()
    with Scope(cfg) as s:
        if not a.no_config:
            configure(s, a)
        t0 = time.time()
        if a.trig:
            s.write(":SINGle")
            print("armed", flush=True)
            while time.time() - t0 < a.wait:
                if q(s, ":TRIG:STAT?") == "STOP":
                    break
                time.sleep(0.1)
            else:
                print("no trigger", flush=True)
                s.write(":STOP")
                return 2
        else:
            # Free-running: a forced single shot, so the record is one
            # complete acquisition at any timebase.
            s.write(":RUN")
            time.sleep(a.settle)
            s.write(":TRIG:SWE SING")
            s.write(":SINGle")
            time.sleep(0.2)
            s.write(":TFORce")
            t_force = time.time()
            while time.time() - t0 < a.settle + a.wait:
                if q(s, ":TRIG:STAT?") == "STOP":
                    break
                # A force before the pre-trigger record is full is ignored.
                if time.time() - t_force > 0.5:
                    s.write(":TFORce")
                    t_force = time.time()
                time.sleep(0.1)
            else:
                print("no complete acquisition", flush=True)
                return 2
        # The DS1104Z reports 0 points of RAW memory after a single shot
        # until it is told to STOP explicitly.
        s.write(":STOP")
        time.sleep(0.3)
        meta = {"label": a.label, "note": a.note, "time_unix": t0, "settings": settings(s, a.chans), "waveforms": {}}
        for ch in a.chans:
            pre, data = read_raw(s, ch)
            (out / f"{a.label}_CH{ch}.bin").write_bytes(data)
            clipped = sum(1 for b in data if b in (0, 255))
            meta["waveforms"][f"CH{ch}"] = {"preamble": pre, "points": len(data), "clipped": clipped}
        (out / f"{a.label}.json").write_text(json.dumps(meta, indent=1))
        s.write(":RUN")
        s.write(":TRIG:SWE AUTO")
    w = meta["waveforms"]
    print(json.dumps({"label": a.label, "srate": meta["settings"]["sample_rate_sa_s"],
                      "points": {k: v["points"] for k, v in w.items()}, "clipped": {k: v["clipped"] for k, v in w.items()},
                      "dt_s": round(time.time() - t0, 1)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
