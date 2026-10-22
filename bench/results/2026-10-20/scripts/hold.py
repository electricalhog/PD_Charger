"""Hold at a contract and summarize: status reads + one 102 ms trace.  usage: hold.py LABEL [reads]"""
import csv, json, statistics as st, subprocess, sys, time
ROOT = str(__import__("pathlib").Path(__file__).resolve().parents[4])  # repo root
def bu(*a, timeout=60):
    r = subprocess.run([f"{ROOT}/tools/bringup/bu", *a], cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    return json.loads(r.stdout)
label = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 8
rows = []
for _ in range(n):
    s = bu("regulator", "status"); c = s.get("control", {}); a = s.get("adc", {})
    rows.append(dict(state=s["state"], mode=c.get("mode"), ccm=c.get("ccm_active"), fsw=c.get("fsw_khz"),
                     icmd=c.get("i_cmd_ma"), imax=c.get("i_max_ma"), ton=c.get("on_time_ns"), u=c.get("ccm_u_mv"),
                     vin=a.get("v_in_mv"), vout=a.get("v_out_mv"), iout_f=c.get("i_out_filt_ma"), iin_f=c.get("i_in_filt_ma"),
                     pin=c.get("p_in_mw"), pout=c.get("p_out_mw"), ent=c.get("ccm_entries"), ex=c.get("ccm_exits"),
                     why=c.get("ccm_exit_reason"), faults=s.get("fault_source")))
    time.sleep(0.15)
bu("regulator", "trace-arm", "--trigger", "now", "--post", "2047"); time.sleep(0.3)
t = bu("regulator", "trace")
v = [int(r["v_out_mv"]) for r in csv.DictReader(open(f"{ROOT}/{t['file']}"))] if t.get("file") else []
tr = list(csv.DictReader(open(f"{ROOT}/{t['file']}"))) if t.get("file") else []
def m(k):
    xs = [r[k] for r in rows if isinstance(r[k], (int, float))]
    return round(st.mean(xs), 1) if xs else None
out = dict(label=label, state={r["state"] for r in rows}, mode={r["mode"] for r in rows}, ccm={r["ccm"] for r in rows},
           fsw={r["fsw"] for r in rows}, **{k: m(k) for k in ("icmd", "imax", "ton", "u", "vin", "vout", "iout_f", "iin_f", "pin", "pout")},
           entries=rows[-1]["ent"], exits=rows[-1]["ex"], exit_reason=rows[-1]["why"], faults=rows[-1]["faults"],
           trace=dict(n=len(v), mean=round(st.mean(v), 1) if v else None, sd=round(st.pstdev(v), 1) if v else None,
                      min=min(v) if v else None, max=max(v) if v else None,
                      ccm_samples=sum(int(r.get("ccm", 0)) for r in tr), skipping=sum(int(r["skipping"]) for r in tr),
                      at_limit=sum(int(r["at_limit"]) for r in tr), file=t.get("file")))
print(json.dumps(out, default=list))
