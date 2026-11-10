"""PPS CV steps into the 25.3 ohm (QT Py off): per step, the source's SetProfile wait (pd_status
transition_ms), the time on CC1 from the end of the GoodCRC after Accept to the start of PS_RDY, and V_out.

usage: pps_cv.py [MV ...]       default ladder below; the first request is from whatever contract is in place
Scope: CH2 = CC1 (0.5 V/div), CH4 = TP2 (5 V/div DC), HRES 60k at 50 ms/div (10 us per point), single on CH2 falling through 0.5 V (the sink's
Request).  CC1 bursts are runs under BURST_V; the messages are, in order, Request, GoodCRC, Accept, GoodCRC,
PS_RDY, GoodCRC (any other burst is printed).  Restores the scope with steps.scope_restore() at the end.
Appends one JSON line per step to data/pps_cv.jsonl.
"""
import csv, json, pathlib, sys, time
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-03" / "scripts"))
sys.path.insert(0, str(HERE.parents[1] / "2026-11-04" / "scripts"))
from benchlib import ROOT, bu
from steps import scope_restore

LADDER = [9020, 9040, 9140, 9240, 9740, 10240, 11240, 12000, 15000, 21000, 20500, 20000, 5000, 21000, 9000]
BURST_V, GAP_S = 1.25, 25e-6
TRIG_V = 0.5   # 1.25 V triggered on switching noise on CC1 at 15-21 V (2026-11-05)
OUT = HERE.parent / "data" / "pps_cv.jsonl"


def arm():
    for c in ("1", "3"):
        bu("scope", "chan", c, "--off")
    bu("scope", "chan", "2", "--on", "--probe", "10", "--scale", "0.5", "--offset", "-1.5", "--coupling", "DC", "--bwl", "20M")
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "20M")
    # The memory depth only changes while running, and not while AUTO sweep keeps taking 3 M-point frames at
    # 50 ms/div (ignored, 2026-11-05): NORM sweep on the idle CC1 first, then the timebase, then the depth.
    bu("scope", "trigger", "--source", "CH2", "--slope", "NEG", "--level", TRIG_V, "--coupling", "DC", "--sweep", "NORM")
    bu("scope", "timebase", "--scale", "0.05", "--offset", "0.25")
    bu("scope", "run")
    for _ in range(3):
        time.sleep(0.5)
        a = bu("scope", "acquire", "--mdepth", "60k", "--type", "HRES")   # 2 channels: 6k/60k/600k...
        if str(a.get("acquire", {}).get("mdepth")) == "60000":
            break
    else:
        raise SystemExit(f"memory depth not set: {a}")
    bu("scope", "trigger", "--source", "CH2", "--slope", "NEG", "--level", TRIG_V, "--coupling", "DC", "--sweep", "SING")
    # Short: the sink's own 5 s PPS re-request can trigger it too (checked by psrdy_minus_wait_ms).
    bu("scope", "single"); time.sleep(0.5)


def bursts(t, v):
    out, start, last = [], None, None
    for ti, x in zip(t, v):
        if x < BURST_V:
            if start is None or ti - last > GAP_S:
                if start is not None:
                    out.append((start, last))
                start = ti
            last = ti
    if start is not None:
        out.append((start, last))
    return out


def pd_now():
    d = bu("pd", "status")
    c = d.get("counters", {})
    return {"transition_ms": (d.get("contract") or {}).get("last_transition_ms"), "contract": d.get("contract"),
            "hard_resets": c.get("hard_reset_count"), "timeouts": c.get("transition_timeouts"),
            "rdo_result": (d.get("last_rdo") or {}).get("result")}


def v_out(n=8):
    vs = []
    for _ in range(n):
        vs.append(bu("regulator", "status").get("adc", {}).get("v_out_mv", 0)); time.sleep(0.05)
    return round(sum(vs) / len(vs)), min(vs), max(vs)


def step(mv, ma=1500):
    before = pd_now()
    arm()
    r = bu("sink", "pps", mv, ma, "--until", "contract", "--timeout", 3)
    time.sleep(0.3)
    after = pd_now()
    if not bu("scope", "wait", "--no-arm", "--timeout", 3).get("ok"):
        cc = None
    else:
        cc = bu("scope", "capture", "CH2", "CH4", "--raw", t=180)
    rec = {"mv": mv, "ma": ma, "sink_matched": r.get("matched"), **after,
           "hard_resets_delta": (after["hard_resets"] or 0) - (before["hard_resets"] or 0)}
    if cc and cc.get("file"):
        rows = list(csv.reader(open(f"{ROOT}/{cc['file']}")))
        t = [float(x[0]) for x in rows[1:]]; v2 = [float(x[1]) for x in rows[1:]]; v4 = [float(x[2]) for x in rows[1:]]
        b = bursts(t, v2)
        rec["file"] = cc["file"]
        rec["bursts_ms"] = [(round(s * 1e3, 3), round((e - s) * 1e3, 3)) for s, e in b]
        if len(b) >= 5:
            rec["goodcrc_to_psrdy_ms"] = round((b[4][0] - b[3][1]) * 1e3, 2)
            if after["transition_ms"] is not None:
                rec["psrdy_minus_wait_ms"] = round(rec["goodcrc_to_psrdy_ms"] - after["transition_ms"], 2)
        # TP2 at the PS_RDY burst and 50 ms after it, 1 ms means
        if len(b) >= 5:
            def mean_at(t0):
                xs = [x for ti, x in zip(t, v4) if t0 <= ti < t0 + 1e-3]
                return round(sum(xs) / len(xs) * 1e3) if xs else None
            rec["tp2_at_psrdy_mv"] = mean_at(b[4][0]); rec["tp2_50ms_after_mv"] = mean_at(b[4][0] + 50e-3)
    rec["v_out_mean_min_max"] = v_out()
    print(json.dumps(rec)); sys.stdout.flush()
    with open(OUT, "a") as f:
        f.write(json.dumps(rec) + "\n")
    return rec


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    ladder = [int(x) for x in sys.argv[1:]] or LADDER
    try:
        for mv in ladder:
            rec = step(mv)
            if rec["hard_resets_delta"] or not rec["sink_matched"]:
                print("stop: hard reset or no contract"); break
    finally:
        scope_restore()


if __name__ == "__main__":
    main()
