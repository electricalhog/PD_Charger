"""Contract ladder 5-9-15-20-24-28(EPR) with CCM off (0), forced (2) and auto (1), 25.3 ohm on VBUS.

usage: sweep.py OUT.jsonl [--modes 0,2,1] [--hold 5] [--no-scope]
Per point: status x6, a 102 ms regulator trace (VD_MON at 20 kHz), CCM exits over --hold s, PD counters, and VBUS
ripple on scope CH4 (TP2, AC, 20 MHz BWL, 2 us/div and 1 ms/div screen captures).  P_out_vr = V^2 / 25.3 ohm (the
board's ID_MON and IS_MON are not true means outside buck DCM, 2026-11-03 README).  Every number is a tool reading.
"""
import argparse, csv, json, statistics as st, sys, time
from benchlib import ROOT, bu, w, status_n, req, pd, ripple, ripple_setup, ripple_restore

LADDER = [(5000, False), (9000, False), (15000, False), (20000, False), (24000, False), (28000, True)]


def trace():
    bu("regulator", "trace-arm", "--trigger", "now"); time.sleep(0.3)
    tr = bu("regulator", "trace")
    if not tr.get("file"):
        return {"error": tr.get("error")}
    rows = list(csv.DictReader(open(f"{ROOT}/{tr['file']}")))
    v = [int(r["v_out_mv"]) for r in rows]
    return dict(file=tr["file"], n=len(v), mean=round(st.mean(v)), sd=round(st.pstdev(v), 1), min=min(v), max=max(v),
                ccm=sum(r["ccm"] == "1" for r in rows), skip=sum(r["skipping"] == "1" for r in rows),
                at_limit=sum(r["at_limit"] == "1" for r in rows), ton_mean=round(st.mean(int(r["t_on_ns"]) for r in rows)))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--modes", default="0,2,1")
    ap.add_argument("--hold", type=float, default=5.0); ap.add_argument("--no-scope", action="store_true")
    ap.add_argument("--ladder", default="")
    a = ap.parse_args()
    ladder = LADDER if not a.ladder else [(int(x), int(x) > 20000 + 4000) for x in a.ladder.split(",")]
    f = open(a.out, "a")
    if not a.no_scope:
        ripple_setup(0.2)
    try:
        for mode in map(int, a.modes.split(",")):
            w("regulator_ccm_enable", mode)
            for mv, epr in ladder:
                ok = req(mv, epr=epr)
                time.sleep(1.5)
                s0 = status_n(6)
                rec = dict(t=time.strftime("%H:%M:%S"), ccm_enable=mode, contract_mv=mv, req_ok=ok, status=s0)
                if s0["state"] != "RUNNING":
                    rec["stop"] = "not RUNNING"; print(json.dumps(rec)); f.write(json.dumps(rec) + "\n"); return 1
                rec["trace"] = trace()
                x0 = s0["ex"]; time.sleep(a.hold); s1 = status_n(3)
                rec["exits_in_hold"] = (s1["ex"] or 0) - (x0 or 0); rec["hold_s"] = a.hold; rec["status_after"] = s1
                rec["pd"] = pd()
                v = s1["v"] / 1000.0
                rec["p_out_vr_w"] = round(v * v / 25.3, 2)
                if not a.no_scope:
                    rec["ripple"] = ripple()
                print(json.dumps(rec)); f.write(json.dumps(rec) + "\n"); f.flush()
                if s1["state"] != "RUNNING":
                    return 1
    finally:
        if not a.no_scope:
            ripple_restore()
    return 0


if __name__ == "__main__":
    sys.exit(main())
