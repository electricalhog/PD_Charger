"""Regulator status summary (and, with an argument, a 102 ms trace's statistics). Run from the repo root."""
import json, subprocess, sys, csv, statistics as st, time
def bu(*a): return json.loads(subprocess.run(["tools/bringup/bu", *a], capture_output=True, text=True).stdout)
d = bu("regulator", "status"); c = d.get("control", {}); a = d["adc"]
keys = ['mode','commanded_mv','fsw_khz','ccm_enable','ccm_active','ccm_limiting','i_cmd_ma','i_out_filt_ma','i_in_filt_ma',
        'p_in_mw','p_out_mw','on_time_ns','ccm_u_mv','ccm_entries','ccm_exits','ccm_exit_reason','ccm_limit_entries']
print(d['state'], d['fault_source'], d['diagnosis'])
print("adc", {k: a[k] for k in ('v_in_mv', 'v_out_mv', 'i_out_ma', 'i_in_ma')})
print({k: c.get(k) for k in keys})
if len(sys.argv) > 1:
    bu("regulator", "trace-arm", "--trigger", "now", "--post", "2000"); time.sleep(0.5)
    t = bu("regulator", "trace"); rows = list(csv.DictReader(open(t['file'])))
    v = [int(r['v_out_mv']) for r in rows]
    print("trace %s v mean %.1f sd %.1f ccm %d" % (t['file'], st.mean(v), st.pstdev(v), t['ccm_samples']))
