"""PPS Current Limit into the 25.3 ohm (QT Py off), with the limit in CCM and DCM (regulator.c step 5d) and the
CCM setting left at auto.  Below spec: pd_pps_cl_min_ma is lowered over SWD (Dan, 2026-11-06) so limits under 1 A
fold the resistor; it is put back to 1000 at the end.

usage: pps_cl.py [MIN_MA] [ccm]      MIN_MA default 100; `ccm`: CCM_LADDER with regulator_ccm_enable = 2 (forced)
                                     written over SWD between steps (Dan, 2026-11-06), back to 1 at the end
Per step: sink `pps MV MA`, then regulator status sampled for HOLD seconds (V_out, CCM's current = ID_MON
corrected, limiting, CCM active, mode).  pd_status transition_ms, rdo result and Hard Resets before/after.
Appends one JSON line per step to data/pps_cl.jsonl.  Stops on a Hard Reset or a missed contract.
"""
import json, pathlib, statistics as st, sys, time
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-03" / "scripts"))
from benchlib import bu, w, status

R_LOAD = 25.3
# (mV, mA, hold s): CV, fold, release; then 21 V CV, a 60 s fold, a deeper fold, release.
LADDER = [(9000, 1500, 3), (9000, 250, 10), (9000, 1500, 5),
          (21000, 1500, 3), (21000, 500, 60), (21000, 300, 10), (21000, 1500, 5), (9000, 1500, 5)]
# Forced CCM: folds and releases in CCM, then a fold started in DCM (auto) with CCM forced during it (CCM entry
# while limiting).  ("ccm", value, hold s) writes regulator_ccm_enable and samples.
CCM_LADDER = [(9000, 1500, 3), ("ccm", 2, 3), (9000, 250, 10), (9000, 1500, 5),
              (21000, 1500, 3), (21000, 500, 30), (21000, 300, 10), (21000, 1500, 5),
              ("ccm", 1, 2), (21000, 500, 5), ("ccm", 2, 10), (21000, 1500, 5), ("ccm", 1, 2), (9000, 1500, 5)]
OUT = HERE.parent / "data" / "pps_cl.jsonl"


def pd_now():
    d = bu("pd", "status")
    c = d.get("counters", {})
    return {"transition_ms": (d.get("contract") or {}).get("last_transition_ms"), "contract": d.get("contract"),
            "hard_resets": c.get("hard_reset_count"), "rdo_result": (d.get("last_rdo") or {}).get("result"),
            "pps_shutdown_resets": (d.get("pps") or {}).get("shutdown_resets")}


def step(mv, ma, hold):
    before = pd_now()
    t0 = time.time()
    if mv == "ccm":
        r = {"matched": w("regulator_ccm_enable", ma, "u8")}
    else:
        r = bu("sink", "pps", mv, ma, "--until", "contract", "--timeout", 3)
    rows = []
    while time.time() - t0 < hold:
        s = status(); s["t"] = round(time.time() - t0, 2); rows.append(s)
    after = pd_now()
    tail = rows[-5:] if len(rows) >= 5 else rows
    mean = lambda k: round(st.mean([x[k] for x in tail if isinstance(x[k], (int, float))]), 1) if tail else None
    rec = {"mv": mv, "ma": ma, "hold_s": hold, "sink_matched": r.get("matched"), **after,
           "hard_resets_delta": (after["hard_resets"] or 0) - (before["hard_resets"] or 0),
           "n": len(rows), "v_tail": mean("v"), "i_ccm_tail": mean("iid"), "i_out_tail": mean("iout"),
           "i_v_over_r_tail": round(mean("v") / R_LOAD) if tail else None,   # inferred: V / 25.3 ohm
           "lim_frac": round(sum(1 for x in rows if x["lim"]) / len(rows), 2) if rows else None,
           "ccm_seen": sorted({x["ccm"] for x in rows}), "modes": sorted({str(x["mode"]) for x in rows}),
           "states": sorted({str(x["state"]) for x in rows}),
           "ccm_entries_exits": (rows[-1]["ent"], rows[-1]["ex"], rows[-1]["why"]) if rows else None, "rows": rows}
    print(json.dumps({k: v for k, v in rec.items() if k not in ("rows", "contract")})); sys.stdout.flush()
    with open(OUT, "a") as f:
        f.write(json.dumps(rec) + "\n")
    return rec


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    min_ma = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    ladder = CCM_LADDER if "ccm" in sys.argv[2:] else LADDER
    assert w("pd_pps_cl_min_ma", min_ma, "u32")
    try:
        for mv, ma, hold in ladder:
            rec = step(mv, ma, hold)
            if rec["hard_resets_delta"] or not rec["sink_matched"]:
                print("stop: hard reset or no contract"); break
    finally:
        w("pd_pps_cl_min_ma", 1000, "u32")
        if ladder is CCM_LADDER:
            w("regulator_ccm_enable", 1, "u8")


if __name__ == "__main__":
    main()
