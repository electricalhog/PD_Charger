"""Helpers for the 2026-11-03 sweeps: one bu call per function, every number a tool reading."""
import json, pathlib, statistics as st, subprocess, time

ROOT = str(pathlib.Path(__file__).resolve().parents[4])


def bu(*a, t=60):
    r = subprocess.run([f"{ROOT}/tools/bringup/bu", *map(str, a)], cwd=ROOT, capture_output=True, text=True, timeout=t)
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        return {"ok": False, "error": r.stdout[-500:] + r.stderr[-500:]}


def w(var, val, typ="u8"):
    return bu("mem", "write", var, val, "--type", typ).get("ok")


def status():
    s = bu("regulator", "status"); c = s.get("control", {}); a = s.get("adc", {})
    return dict(state=s.get("state"), fault=s.get("fault_source"), mode=c.get("mode"), tgt=c.get("target_mv"),
                v=a.get("v_out_mv"), vin=a.get("v_in_mv"), ccm=c.get("ccm_active"), kind=c.get("ccm_kind"),
                fsw=c.get("fsw_khz"), ton=c.get("on_time_ns"), u=c.get("ccm_u_mv"), iout=a.get("i_out_ma"),
                iid=c.get("ccm_i_ma"), iin=a.get("i_in_ma"), pin=c.get("p_in_mw"), pout=c.get("p_out_mw"),
                lim=c.get("ccm_limiting"), ent=c.get("ccm_entries"), ex=c.get("ccm_exits"),
                why=c.get("ccm_exit_reason"), skip=c.get("pulses_skipped"), icmd=c.get("i_cmd_ma"))


def status_n(n=6, gap=0.1):
    rows = []
    for _ in range(n):
        rows.append(status()); time.sleep(gap)
    out = dict(rows[-1])
    for k in ("v", "vin", "ton", "iout", "iid", "iin", "pin", "pout", "u"):
        xs = [r[k] for r in rows if isinstance(r[k], (int, float))]
        if xs:
            out[k] = round(st.mean(xs), 1); out[k + "_sd"] = round(st.pstdev(xs), 1)
    out["ccm_seen"] = sorted({r["ccm"] for r in rows})
    return out


def req(mv, ma=3000, epr=False):
    if epr:
        bu("sink", "epr", "140", "--wait", "3")
    r = bu("sink", "req", mv, ma, "--until", "contract", "--timeout", "6")
    return bool(r.get("ok"))


def pd():
    d = bu("pd", "status"); c = d.get("counters", {})
    return dict(contract_mv=(d.get("contract") or {}).get("mv"), hard_resets=c.get("hard_reset_count"),
                detaches=c.get("detach_count"))


def ripple_setup(scale=0.2):
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", scale, "--offset", "0", "--coupling", "AC", "--bwl", "20M")
    bu("scope", "trigger", "--source", "CH4", "--slope", "POS", "--level", "0", "--sweep", "AUTO")
    bu("scope", "timebase", "--scale", "2e-6", "--offset", "0")


def ripple_restore():
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "OFF")
    bu("scope", "trigger", "--source", "CH1", "--slope", "POS", "--level", "5", "--sweep", "AUTO")


def ripple():
    """CH4 (TP2, VBUS) AC, 20 MHz BWL, raw 2 us/div capture: p-p and rms of the 200 ns moving average (the edge
    spikes are mostly probe pickup), and the raw 0.1-99.9 percentile span with them."""
    import csv as _csv
    bu("scope", "run")   # a raw capture leaves the scope stopped
    time.sleep(0.8)
    c = bu("scope", "capture", "CH4", "--raw")
    if not c.get("file"):
        return {"error": c.get("error")}
    v = [float(r[1]) for r in list(_csv.reader(open(f"{ROOT}/{c['file']}")))[1:]]
    sr = c.get("sample_rate_sa_s") or 250e6
    n = max(1, int(200e-9 * sr))
    if len(v) <= n:
        return {"error": f"capture has {len(v)} samples", "file": c["file"]}
    acc = sum(v[:n]); ma = []
    for k in range(n, len(v)):
        ma.append(acc / n); acc += v[k] - v[k - n]
    s = sorted(v); q = lambda p: s[int(p * (len(s) - 1))]
    m = st.mean(ma)
    return dict(file=c["file"], sr=sr, n=len(v), pp_ma200ns_mv=round(1e3 * (max(ma) - min(ma)), 1),
                rms_ma200ns_mv=round(1e3 * (st.mean((x - m) ** 2 for x in ma)) ** 0.5, 1),
                raw_span_999_mv=round(1e3 * (q(0.999) - q(0.001)), 1), raw_min=min(v), raw_max=max(v))
