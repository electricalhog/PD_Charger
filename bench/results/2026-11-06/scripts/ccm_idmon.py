"""ID_MON in buck CCM vs DCM at the same point: PPS CV (1500 mA, no limit reached) at each voltage into the 25.3 ohm
(QT Py off), read in auto (DCM at these currents) and then with regulator_ccm_enable = 2 (forced CCM, SWD,
Dan 2026-11-06).  Same resistor and V_out in both, so the CCM/DCM ratio is the reading's error there; V/R is
the inferred reference.

usage: ccm_idmon.py [--tag TAG] [--out FILE] [MV ...]     default 5000..21000 in 1 V steps
Appends one JSON line per voltage and mode to data/ccm_idmon.jsonl (or FILE), with TAG and the live
regulator_buck_fsw_khz / regulator_ccm_refresh_interval_ns; regulator_ccm_enable back to 1 at the end.
"""
import json, pathlib, sys, time
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-03" / "scripts"))
from benchlib import bu, w, status_n

R_LOAD = 25.3
OUT = HERE.parent / "data" / "ccm_idmon.jsonl"
TAG = ""


def rd(sym):
    d = bu("mem", "read", sym)
    v = d.get("values")
    return v[0] if v else int.from_bytes(bytes.fromhex(d.get("hex", "00").replace(" ", "")), "little")


def point(mv, ccm):
    w("regulator_ccm_enable", ccm, "u8")
    time.sleep(1.0)
    s = status_n(10, 0.1)
    s.update(mv=mv, ccm_enable=ccm, i_v_over_r=round(s["v"] / R_LOAD, 1),   # inferred: V / 25.3 ohm
             tag=TAG, fsw_set=rd("regulator_buck_fsw_khz"), refresh_ivl_ns=rd("regulator_ccm_refresh_interval_ns"))
    s["iout_over_vr"] = round(s["iout"] / s["i_v_over_r"], 3) if s["i_v_over_r"] else None
    s["eta_vr"] = round(s["v"] * s["i_v_over_r"] / (s["vin"] * s["iin"]), 3) if s["iin"] else None   # inferred
    print(json.dumps({k: s[k] for k in ("mv", "ccm_enable", "ccm_seen", "kind", "fsw", "v", "iout", "iid",
                                        "i_v_over_r", "iout_over_vr", "iin", "eta_vr", "ton")})); sys.stdout.flush()
    with open(OUT, "a") as f:
        f.write(json.dumps(s) + "\n")


def main():
    global TAG, OUT
    args = sys.argv[1:]
    if "--tag" in args:
        k = args.index("--tag"); TAG = args[k + 1]; del args[k:k + 2]
    if "--out" in args:
        k = args.index("--out"); OUT = pathlib.Path(args[k + 1]); del args[k:k + 2]
    mvs = [int(x) for x in args] or list(range(5000, 21001, 1000))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    try:
        for mv in mvs:
            r = bu("sink", "pps", mv, 1500, "--until", "contract", "--timeout", 3)
            if not r.get("matched"):
                print("stop: no contract", mv); break
            point(mv, 1)
            point(mv, 2)
    finally:
        w("regulator_ccm_enable", 1, "u8")


if __name__ == "__main__":
    main()
