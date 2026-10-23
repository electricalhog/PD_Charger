"""switch-pulse at the present contract in forced CCM, several lengths; per pulse the regulator trace (switch
trigger) and a raw scope record (CH1 IL_MON 1x, CH3 SW2, CH4 VBUS) triggered on IL_MON rising (the reconnect
inrush).  Saves 2.5 us medians.  Stops at the first fault.  Usage (repo root): pulses.py OUTDIR MS[,MS...]"""
import json, subprocess, statistics as st, sys, time, csv, os
OUT = sys.argv[1]; MSS = [int(x) for x in sys.argv[2].split(',')]
def bu(*a): return json.loads(subprocess.run(["tools/bringup/bu", *a], capture_output=True, text=True).stdout)
bu("scope", "chan", "1", "--on", "--probe", "1", "--scale", "0.5", "--offset", "-1.0", "--coupling", "DC")
bu("scope", "scpi", ":CHAN2:DISP OFF")
bu("scope", "chan", "3", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC")
bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "20M")
bu("scope", "timebase", "--scale", "1e-4", "--offset", "4e-4"); bu("scope", "scpi", ":ACQ:MDEP AUTO")
for ms in MSS:
    bu("scope", "trigger", "--source", "CH1", "--slope", "POS", "--level", "0.6", "--sweep", "SING")
    bu("scope", "scpi", ":TRIG:COUP HFR"); bu("scope", "single"); time.sleep(1.5)
    bu("regulator", "trace-arm", "--trigger", "switch", "--post", "1900")
    r = bu("regulator", "switch-pulse", str(ms)); time.sleep(1.0)
    s = bu("regulator", "status"); c = s.get("control", {})
    t = bu("regulator", "trace")
    sc = bu("scope", "capture", "CH1", "CH3", "CH4", "--raw")
    rec = {"ms": ms, "state": s["state"], "faults": s["fault_source"], "trace": t.get("file"),
           "scope": sc.get("file"), "sr": sc.get("sample_rate_sa_s"), "ccm_exits": c.get("ccm_exits"),
           "limit_entries": c.get("ccm_limit_entries")}
    if sc.get("file"):
        rows = list(csv.reader(open(sc["file"])))[1:]
        n = max(1, int(round(2.5e-6 * sc["sample_rate_sa_s"])))
        with open(os.path.join(OUT, f"hw_pulse{ms}_scope_2p5us.csv"), "w") as f:
            f.write("t_us,il_mon_v,sw2_v,vbus_v\n")
            for k in range(0, len(rows) - n, n):
                seg = rows[k:k + n]
                f.write("%.2f,%.4f,%.3f,%.3f\n" % (float(seg[0][0]) * 1e6, st.median(float(x[1]) for x in seg),
                        st.median(float(x[2]) for x in seg), st.median(float(x[3]) for x in seg)))
    if t.get("file"):
        os.system(f"cp {t['file']} {OUT}/hw_pulse{ms}_trace.csv")
    print(json.dumps(rec), flush=True)
    if s["state"] != "RUNNING" or s["fault_source"]:
        print("STOP"); sys.exit(1)
    time.sleep(2)
