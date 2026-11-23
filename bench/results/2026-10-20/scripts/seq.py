"""Contract sequence with per-step summary; stops on fault. usage: seq.py mv,mv,... [settle_s]"""
import csv, json, statistics as st, subprocess, sys, time
ROOT = str(__import__("pathlib").Path(__file__).resolve().parents[4])  # repo root
def bu(*a, t=60):
    return json.loads(subprocess.run([f"{ROOT}/tools/bringup/bu", *a], cwd=ROOT, capture_output=True, text=True, timeout=t).stdout)
R = {0: "NONE", 1: "DISABLED", 2: "MODE", 3: "SWITCH", 4: "LIGHT_LOAD", 5: "OVER_BAND", 6: "VIN_RISE", 7: "STOP", 8: "BAD_CONFIG", 9: "DUTY", 10: "RAMP_DOWN"}
settle = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
for mv in [int(x) for x in sys.argv[1].split(",")]:
    if mv >= 28000:
        bu("sink", "epr", "100", "--wait", "2")
    r = bu("sink", "req", str(mv), "3000", "--until", "contract", "--timeout", "6")
    ok = any(e.get("event") == "contract" for e in r.get("events", []))
    time.sleep(settle)
    bu("regulator", "trace-arm", "--trigger", "now", "--post", "2047"); time.sleep(0.25)
    tr = bu("regulator", "trace"); rows = list(csv.DictReader(open(f"{ROOT}/{tr['file']}"))) if tr.get("file") else []
    v = [int(x["v_out_mv"]) for x in rows]
    s = bu("regulator", "status"); c = s.get("control", {})
    log = bu("mem", "read", "regulator_ccm_exit_log", "--type", "u32", "--count", "16")["values"]
    idx = bu("mem", "read", "regulator_ccm_exit_log_index", "--type", "u32")["values"][0]
    recent = [(log[2*k], R.get(log[2*k+1] & 0xFF), log[2*k+1] >> 8) for k in range(8) if log[2*k]]
    recent = sorted(recent)[-3:]
    print(json.dumps(dict(mv=mv, contract=ok, state=s["state"], faults=s["fault_source"], mode=c.get("mode"), fsw=c.get("fsw_khz"),
          ccm=c.get("ccm_active"), entries=c.get("ccm_entries"), exits=c.get("ccm_exits"), exit_log_n=idx, recent_exits=recent,
          u_mv=round(c.get("ccm_u_mv", 0)), ton=c.get("on_time_ns"), il_avg=c.get("il_avg_ma"), vout=s["adc"]["v_out_mv"],
          trace_sd=round(st.pstdev(v), 1) if v else None, trace_mean=round(st.mean(v)) if v else None,
          ccm_samples=sum(int(x.get("ccm", 0)) for x in rows))))
    if s["state"] != "RUNNING":
        print("STOP: not running"); break
