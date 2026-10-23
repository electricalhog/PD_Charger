"""One pass: 5 V DCM, then forced CCM 400 kHz at 5/9/20 V; ring statistics, trace, scope VBUS. Ends at 5 V DCM, CCM off.
Usage (repo root): pass.py TAG SCANLEN OUT.jsonl"""
import json, subprocess, statistics as st, sys, time, csv
TAG, L, OUT = sys.argv[1], int(sys.argv[2]), sys.argv[3]
VR = 1 if L == 3 else 0
def bu(*a): return json.loads(subprocess.run(["tools/bringup/bu", *a], capture_output=True, text=True).stdout)
def ring(reps=8):
    vd, vs = [], []
    for _ in range(reps):
        vd += bu("mem", "read", "adc1_dma_buffer", "--type", "u16", "--count", str(64 * L))['values'][VR::L]
        vs += bu("mem", "read", "adc2_dma_buffer", "--type", "u16", "--count", str(32 * L))['values'][VR::L]
    def stats(x):
        m = st.median(x); o = [v for v in x if abs(v - m) > 12]; core = [v for v in x if abs(v - m) <= 12]
        return {"median": m, "n": len(x), "outliers": len(o), "out_min": min(o) if o else None,
                "out_max": max(o) if o else None, "sd_core": round(st.pstdev(core), 2)}
    return stats(vd), stats(vs)
def trace():
    bu("regulator", "trace-arm", "--trigger", "now", "--post", "2000"); time.sleep(0.5)
    t = bu("regulator", "trace"); rows = list(csv.DictReader(open(t['file'])))
    if not rows: return None
    v = [int(r['v_out_mv']) for r in rows]; d = [abs(v[k] - v[k - 1]) for k in range(1, len(v))]
    return {"file": t['file'], "sd": round(st.pstdev(v), 1), "pp": max(v) - min(v), "mean_dv": round(st.mean(d), 1),
            "max_dv": max(d), "ccm": sum(r['ccm'] == '1' for r in rows)}
def scope_vbus(mv):
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "1", "--offset", str(-min(mv / 1000.0, 20)),
       "--coupling", "DC", "--bwl", "20M")
    bu("scope", "timebase", "--scale", "1e-3", "--offset", "0"); time.sleep(1.5)
    return bu("scope", "measure", "CH4", "--items", "VAVG")["measurements"]["CH4"]["VAVG"]
def exits(): return bu("mem", "read", "regulator_ccm_exits", "--type", "u32")['values'][0]
def point(name, mv):
    e0 = exits(); time.sleep(2)
    s = bu("regulator", "status"); c = s.get("control", {})
    rv, rs = ring(); tr = trace(); vb = scope_vbus(mv); e1 = exits()
    rec = {"tag": TAG, "point": name, "state": s["state"], "faults": s["fault_source"], "v_out_mv": s["adc"]["v_out_mv"],
           "v_in_mv": s["adc"]["v_in_mv"], "vd_median_mv": round(rv["median"] * 70645 / 4096), "vbus_scope_v": vb,
           "ring_vd": rv, "ring_vs": rs, "trace": tr, "ccm_exits_during": e1 - e0, "ccm_u_mv": c.get("ccm_u_mv"),
           "i_out_ma": s["adc"]["i_out_ma"]}
    print(json.dumps(rec), flush=True); open(OUT, "a").write(json.dumps(rec) + "\n")
    return rec["state"] == "RUNNING" and not rec["faults"] and tr is not None
def safe():
    bu("sink", "req", "5000", "--until", "contract"); time.sleep(1); bu("regulator", "stop")
    bu("mem", "write", "regulator_ccm_enable", "0", "--type", "u8"); bu("mem", "write", "regulator_buck_fsw_khz", "200", "--type", "u16")
    bu("sink", "req", "5000", "--until", "contract", "--wait", "4"); time.sleep(3)
    s = bu("regulator", "status"); print("END", s["state"], s["fault_source"])
CCM_ONLY = len(sys.argv) > 4 and sys.argv[4] == "ccm-only"   # already in forced CCM at 5 V
ok = True if CCM_ONLY else point("dcm_5v", 5000)
if ok:
    if not CCM_ONLY:
        bu("regulator", "stop"); bu("mem", "write", "regulator_buck_fsw_khz", "400", "--type", "u16")
        bu("mem", "write", "regulator_ccm_enable", "2", "--type", "u8")
        # a request for another voltage makes the stack Hard Reset and restart the regulator (a 5 V one may not)
        bu("sink", "req", "9000", "--until", "contract", "--wait", "3"); time.sleep(2)
        bu("sink", "req", "5000", "--until", "contract", "--wait", "2"); time.sleep(3)
    ok = point("ccm400_5v", 5000)
    for mv in (9000, 20000):
        if not ok: break
        bu("sink", "req", str(mv), "--until", "contract", "--wait", "2"); time.sleep(2)
        ok = point("ccm400_%dv" % (mv // 1000), mv)
if ok: safe()
else: print("STOPPED: not running after a point; left as is for diagnosis")
