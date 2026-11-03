"""CCM current-limit test with a resistor on VBUS (plans/regulation-strategy.md, section 5).

usage: cc_test.py MV[,MV...] [--frac 0.97] [--hotplug-ma 1000] [--no-hotplug]

Per contract (buck CCM already running, forced):
  1. baseline at the default limit: status reads, one 102 ms trace;
  2. limit = frac x the measured CCM current (ID_MON period mean, the limit's own input), so a resistor
     folds VBUS by (1 - frac), inside PD's +-5 %; trace the engage, then hold 1.5 s and read status;
  3. limit back to the default; trace the release;
  4. switch-pulse 20 ms with the limit at --hotplug-ma (drained VBUS-side caps reconnect): dip, peak,
     samples in the limit, time outside +-5 %.
Prints one JSON line per phase; stops at the first state other than RUNNING.  Every number is a tool
reading: the firmware's status fields and its 20 kHz trace (V_out is VD_MON at the board node).
"""
import argparse, csv, json, pathlib, statistics as st, subprocess, sys, time

ROOT = str(pathlib.Path(__file__).resolve().parents[4])


def bu(*a, t=60):
    r = subprocess.run([f"{ROOT}/tools/bringup/bu", *a], cwd=ROOT, capture_output=True, text=True, timeout=t)
    return json.loads(r.stdout)


def status(n=6):
    rows = []
    for _ in range(n):
        s = bu("regulator", "status"); c = s.get("control", {})
        rows.append(dict(state=s["state"], faults=s["fault_source"], v=s["adc"]["v_out_mv"], i=c.get("ccm_i_ma"),
                         lim=c.get("ccm_limiting"), ilim=c.get("ccm_ilim_ma"), ccm=c.get("ccm_active"),
                         fsw=c.get("fsw_khz"), vlim=c.get("ccm_v_lim_mv"), ton=c.get("on_time_ns")))
        time.sleep(0.12)
    m = lambda k: round(st.mean(r[k] for r in rows if isinstance(r[k], (int, float))), 1)
    return dict(state=rows[-1]["state"], faults=rows[-1]["faults"], ccm=sorted({r["ccm"] for r in rows}),
                fsw=rows[-1]["fsw"], v_mv=m("v"), i_ma=m("i"), i_sd=round(st.pstdev(r["i"] for r in rows), 1),
                limiting=sorted({r["lim"] for r in rows}), ilim_ma=rows[-1]["ilim"], v_lim_mv=m("vlim"), ton_ns=m("ton"))


def trace_read():
    tr = bu("regulator", "trace")
    if not tr.get("file"):
        return None
    return list(csv.DictReader(open(f"{ROOT}/{tr['file']}"))), tr["file"]


def summarize(rows, v_ref):
    t = [float(r["t_ms"]) for r in rows]; v = [int(r["v_out_mv"]) for r in rows]; i = [int(r["i_cmd_ma"]) for r in rows]
    lim = [int(r.get("ilim", 0)) for r in rows]
    return dict(n=len(v), v_min=min(v), v_max=max(v), v_last20_mean=round(st.mean(v[-400:])), v_last20_sd=round(st.pstdev(v[-400:]), 1),
                i_last20_mean=round(st.mean(i[-400:])), i_last20_sd=round(st.pstdev(i[-400:]), 1),
                ilim_samples=sum(lim), over_5pct_ms=round(sum(1 for x in v if x > v_ref * 1.05) * 0.05, 2),
                under_5pct_ms=round(sum(1 for x in v if x < v_ref * 0.95) * 0.05, 2))


def pd_counters():
    d = bu("pd", "status"); c = d.get("counters", {})
    return dict(contract_mv=(d.get("contract") or {}).get("mv"), hard_resets=c.get("hard_reset_count"),
                detaches=c.get("detach_count"), contracts=c.get("contract_count"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mvs"); ap.add_argument("--frac", type=float, default=0.97)
    ap.add_argument("--hotplug-ma", type=int, default=1000); ap.add_argument("--no-hotplug", action="store_true")
    ap.add_argument("--default-ma", type=int, default=3000)
    a = ap.parse_args()
    for mv in [int(x) for x in a.mvs.split(",")]:
        r = bu("sink", "req", str(mv), "3000", "--until", "contract", "--timeout", "8")
        ok = any(e.get("event") == "contract" for e in r.get("events", []))
        time.sleep(2.5)
        bu("mem", "write", "regulator_ccm_ilim_ma", str(a.default_ma), "--type", "u16")
        time.sleep(0.3)
        base = status(); print(json.dumps(dict(phase="baseline", mv=mv, contract=ok, **base, pd=pd_counters())))
        if base["state"] != "RUNNING" or not base["ccm"] == [1]:
            print("STOP: not RUNNING in CCM"); return 1
        lim_ma = int(round(a.frac * base["i_ma"]))
        bu("regulator", "trace-arm", "--trigger", "now", "--post", "2047"); bu("mem", "write", "regulator_ccm_ilim_ma", str(lim_ma), "--type", "u16")
        time.sleep(0.3); tr = trace_read()
        eng = summarize(tr[0], mv) if tr else None
        time.sleep(1.5); held = status()
        print(json.dumps(dict(phase="limit", mv=mv, set_ma=lim_ma, engage_trace=eng, engage_file=tr[1] if tr else None, held=held, pd=pd_counters())))
        if held["state"] != "RUNNING":
            print("STOP"); return 1
        bu("regulator", "trace-arm", "--trigger", "now", "--post", "2047"); bu("mem", "write", "regulator_ccm_ilim_ma", str(a.default_ma), "--type", "u16")
        time.sleep(0.3); tr = trace_read(); time.sleep(1.0); after = status()
        print(json.dumps(dict(phase="release", mv=mv, release_trace=summarize(tr[0], mv) if tr else None, release_file=tr[1] if tr else None, after=after, pd=pd_counters())))
        if after["state"] != "RUNNING":
            print("STOP"); return 1
        if a.no_hotplug:
            continue
        bu("mem", "write", "regulator_ccm_ilim_ma", str(a.hotplug_ma), "--type", "u16"); time.sleep(0.5)
        bu("regulator", "trace-arm", "--trigger", "switch", "--post", "1900")
        p = bu("regulator", "switch-pulse", "20"); time.sleep(0.5)
        tr = trace_read(); s2 = status(3)
        hp = None
        if tr:
            rows = tr[0]; t = [float(x["t_ms"]) for x in rows]; sw = [int(x["switch_on"]) for x in rows]
            rc = next((tt for tt, w in zip(t, sw) if tt > 0 and w == 1), None)
            if rc is not None:
                after_rc = [x for x in rows if rc <= float(x["t_ms"]) < rc + 30]
                v = [int(x["v_out_mv"]) for x in after_rc]
                hp = dict(reconnect_ms=rc, dip_mv=min(v) - mv, peak_mv=max(v) - mv,
                          ilim_samples=sum(int(x["ilim"]) for x in after_rc),
                          below_5pct_ms=round(sum(1 for x in v if x < 0.95 * mv) * 0.05, 2),
                          above_5pct_ms=round(sum(1 for x in v if x > 1.05 * mv) * 0.05, 2))
        print(json.dumps(dict(phase="hotplug", mv=mv, limit_ma=a.hotplug_ma, pulse=p.get("result"), result=hp,
                              file=tr[1] if tr else None, after=s2, pd=pd_counters())))
        bu("mem", "write", "regulator_ccm_ilim_ma", str(a.default_ma), "--type", "u16")
        if s2["state"] != "RUNNING":
            print("STOP"); return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
