"""Load steps at 5-9-15-20-24-28(EPR) V, CCM auto: noise, regulation, step response, ADC efficiency estimate.

usage: steps.py OUT.jsonl [--ladder 5000,9000,...] [--open-ms 2] [--qtpy-mw 7000]

Per contract:
  light  25.3 ohm only: status x6 (incl. IS_MON/ID_MON p_in/p_out), 102 ms trace (VD_MON 20 kHz), ripple (CH4).
  A      switch-pulse OPEN_MS (Q5/Q6 let go ~1.4 ms after OUTPUT_EN): the output switch opens (25.3 ohm + VBUS side off) and recloses.  Regulator trace
         triggered by the pulse; scope CH4 (TP2, AC, 20 MHz BWL) 1.2 s record forced right after the step.
  heavy  (>= 15 V, the QT Py needs 12-15 V to switch) QT Py armed at --qtpy-mw: status x6, trace, ripple.
  B      QT Py disarm (all DISABLE high at once, a true step down): scope CH4 as in A (the regulator trace's
         error trigger fired on load noise in the trial, so it is not used).
The QT Py's arm is a ramp (V_out* slews at 5 V/s), not a step, so the up-step is not measured.
Every number is a tool reading; p_in/p_out are the board's filtered IS_MON/ID_MON products (see README caveats).
"""
import argparse, csv, json, signal, statistics as st, subprocess, sys, time
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[2] / "2026-11-03" / "scripts"))
from benchlib import ROOT, bu, w, status, req, pd, ripple, ripple_setup  # noqa: E402

LADDER = [5000, 9000, 15000, 20000, 24000, 28000]
R_LOAD = 25.3


def status_n(n=6, gap=0.1):
    rows = []
    for _ in range(n):
        s = bu("regulator", "status"); c = s.get("control", {}); a = s.get("adc", {})
        rows.append(dict(state=s.get("state"), mode=c.get("mode"), kind=c.get("ccm_kind"), ccm=c.get("ccm_active"),
                         v=a.get("v_out_mv"), vin=a.get("v_in_mv"), pin=c.get("p_in_mw"), pout=c.get("p_out_mw"),
                         iin=c.get("i_in_filt_ma"), iout=c.get("i_out_filt_ma"), fsw=c.get("fsw_khz"),
                         ex=c.get("ccm_exits")))
        time.sleep(gap)
    out = dict(rows[-1])
    for k in ("v", "vin", "pin", "pout", "iin", "iout"):
        xs = [r[k] for r in rows if isinstance(r[k], (int, float))]
        if xs:
            out[k] = round(st.mean(xs), 1); out[k + "_sd"] = round(st.pstdev(xs), 1)
    out["kinds"] = sorted({r["kind"] for r in rows if r["kind"] is not None})
    return out


def read_trace():
    tr = bu("regulator", "trace")
    if not tr.get("file"):
        return None, tr
    rows = list(csv.DictReader(open(f"{ROOT}/{tr['file']}")))
    return rows, tr


def trace_stats(rows):
    v = [int(r["v_out_mv"]) for r in rows]
    return dict(n=len(v), mean=round(st.mean(v), 1), sd=round(st.pstdev(v), 1), min=min(v), max=max(v),
                ccm=sum(r["ccm"] == "1" for r in rows), skip=sum(r["skipping"] == "1" for r in rows))


def trace_now():
    bu("regulator", "trace-arm", "--trigger", "now"); time.sleep(0.3)
    rows, tr = read_trace()
    return dict(file=tr.get("file"), **trace_stats(rows)) if rows else {"error": tr.get("error")}


def step_from_trace(rows, file, band_pct=1.0):
    """Deviation about the pre-trigger mean, and the last time outside +-band_pct after the trigger."""
    trig = next((i for i, r in enumerate(rows) if r["trigger"] == "1"), None)
    if trig is None or trig < 20:
        return dict(file=file, error=f"trigger index {trig}")
    t = [float(r["t_ms"]) for r in rows]; v = [int(r["v_out_mv"]) for r in rows]
    base = st.mean(v[:trig]); t0 = t[trig]
    dev = [x - base for x in v]; post = dev[trig:]
    band = base * band_pct / 100
    out_idx = [i for i in range(trig, len(v)) if abs(dev[i]) > band]
    imax = max(range(trig, len(v)), key=lambda i: dev[i]); imin = min(range(trig, len(v)), key=lambda i: dev[i])
    return dict(file=file, base_mv=round(base), pre_sd_mv=round(st.pstdev(v[:trig]), 1),
                peak_mv=round(max(post)), peak_t_ms=round(t[imax] - t0, 2),
                dip_mv=round(min(post)), dip_t_ms=round(t[imin] - t0, 2),
                last_out_1pct_ms=round(t[out_idx[-1]] - t0, 2) if out_idx else None,
                samples_out_1pct=len(out_idx), ilim=sum(r["ilim"] == "1" for r in rows[trig:]),
                post_ms=round(t[-1] - t0, 1))


def scope_step_arm(v_mv, span_frac):
    """CH4 AC 20 MHz BWL, 100 ms/div with the trigger 0.02 s from the right edge, single with an unreachable
    level: scope_step_read forces the trigger right after the step, so the record holds the 1.2 s before it."""
    for c in ("1", "2", "3"):
        bu("scope", "chan", c, "--off")
    scale = min((s for s in (0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0) if s * 4 >= span_frac * v_mv / 1000), default=5.0)
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", scale, "--offset", "0", "--coupling", "AC", "--bwl", "20M")
    bu("scope", "acquire", "--mdepth", "120k", "--type", "HRES")
    bu("scope", "timebase", "--scale", "0.1", "--offset", "-0.58")
    bu("scope", "trigger", "--source", "CH4", "--slope", "POS", "--level", round(4.5 * scale, 3), "--coupling", "DC",
       "--sweep", "SING")
    bu("scope", "single"); time.sleep(1.5)
    from bringup.scope import _Visa   # held open across the step so the force goes out within ms of it
    return scale, _Visa("usb", 5.0)


def scope_force(h):
    h.write(":TFORce"); h.close()


def _ma(t, v, w):
    n = max(1, int(round(w / (t[1] - t[0]))))
    acc = sum(v[:n]); out = []
    for k in range(n, len(v)):
        out.append(acc / n); acc += v[k] - v[k - n]
    return t[n // 2:n // 2 + len(out)], out


def scope_step_read(v_mv, scale, mode):
    """A: event = largest 10 us-MA excursion 0.6-0.05 s before the force.  B: the disarm is 0.25-0.20 s before the
    force (SIGTERM, then <= 50 ms to the arm=0 frame); its 1 ms-MA deviation in the 100 ms after -0.26 s against
    the 100 ms before.  lf_*: 1 ms moving average over the record before the event (0-1 kHz noise)."""
    time.sleep(0.5)
    c = bu("scope", "capture", "CH4", "--raw", t=180)
    if not c.get("file"):
        return dict(error=c.get("error"))
    rows = list(csv.reader(open(f"{ROOT}/{c['file']}")))[1:]
    t = [float(r[0]) for r in rows]; v = [float(r[1]) for r in rows]
    tm, ma = _ma(t, v, 10e-6)
    t1, m1 = _ma(t, v, 1e-3)
    clip = sum(1 for x in v if abs(x) >= 3.9 * scale)
    band = 0.01 * v_mv / 1000
    if mode == "A":
        med = sorted(ma)[len(ma) // 2]
        k_ev = max((k for k in range(len(ma)) if -0.6 < tm[k] < -0.05), key=lambda k: abs(ma[k] - med))
        k0 = next((k for k in range(k_ev, -1, -1) if abs(ma[k] - med) < 0.25 * abs(ma[k_ev] - med)), 0)
        t_ev = tm[k0]
        pre = [x for x, tt in zip(ma, tm) if t_ev - 0.105 < tt < t_ev - 0.015]
        base = st.mean(pre)
        win = [k for k in range(len(ma)) if t_ev - 0.015 <= tm[k] <= t_ev + 0.1]
        d = {k: ma[k] - base for k in win}
        out = [tm[k] - t_ev for k in win if abs(d[k]) > band and tm[k] > t_ev]
        kp = max(win, key=lambda k: d[k]); km = min(win, key=lambda k: d[k])
        res = dict(peak_mv=round(1e3 * d[kp]), peak_t_ms=round(1e3 * (tm[kp] - t_ev), 2),
                   dip_mv=round(1e3 * d[km]), dip_t_ms=round(1e3 * (tm[km] - t_ev), 2),
                   last_out_1pct_ms=round(1e3 * out[-1], 2) if out else None)
    else:
        t_ev = -0.26
        pre = [x for x, tt in zip(m1, t1) if -0.36 < tt < -0.26]
        post = [x for x, tt in zip(m1, t1) if -0.26 <= tt < -0.16]
        base = st.mean(pre)
        res = dict(pre100_pp_mv=round(1e3 * (max(pre) - min(pre))), post100_max_mv=round(1e3 * (max(post) - base)),
                   post100_min_mv=round(1e3 * (min(post) - base)))
    lf = [x for x, tt in zip(m1, t1) if tt < t_ev - 0.015]
    mu = st.mean(lf)
    return dict(file=c["file"], dt_ns=round((t[1] - t[0]) * 1e9), scale_v_div=scale, samples_near_clip=clip,
                event_t_s=round(t_ev, 4), lf_pp_mv=round(1e3 * (max(lf) - min(lf))),
                lf_rms_mv=round(1e3 * st.pstdev(lf), 1), lf_s=round(len(lf) * (t1[1] - t1[0]), 2), **res)


def scope_restore():
    bu("scope", "chan", "1", "--on", "--probe", "10", "--scale", "5", "--offset", "-12", "--coupling", "DC", "--bwl", "OFF")
    bu("scope", "chan", "2", "--on", "--probe", "10", "--scale", "0.5", "--offset", "-1.5", "--coupling", "DC", "--bwl", "20M")
    bu("scope", "chan", "3", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "OFF")
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "20M")
    bu("scope", "acquire", "--mdepth", "AUTO", "--type", "NORM")
    bu("scope", "timebase", "--scale", "2e-6", "--offset", "0")
    bu("scope", "trigger", "--source", "CH1", "--slope", "POS", "--level", "5", "--coupling", "DC", "--sweep", "AUTO")


def point(v_mv):
    s = status_n(6)
    r = dict(status=s, trace=trace_now())
    ripple_setup(0.2 if v_mv < 20000 else 0.5)
    for c in ("1", "2", "3"):
        bu("scope", "chan", c, "--off")
    bu("scope", "acquire", "--mdepth", "AUTO", "--type", "NORM")
    r["ripple"] = ripple()
    if s.get("pin") and s.get("pout"):
        r["eta_adc_pct"] = round(100 * s["pout"] / s["pin"], 1)
    return r


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--ladder", default="")
    ap.add_argument("--open-ms", default="10"); ap.add_argument("--qtpy-mw", type=int, default=7000)
    a = ap.parse_args()
    ladder = [int(x) for x in a.ladder.split(",")] if a.ladder else LADDER
    f = open(a.out, "a")
    w("regulator_ccm_enable", 1)
    try:
        for mv in ladder:
            rec = dict(t=time.strftime("%H:%M:%S"), contract_mv=mv, req_ok=req(mv, epr=mv > 20000 + 4000))
            time.sleep(2.0)
            light = point(mv)
            rec["light"] = light
            if light["status"]["state"] != "RUNNING":
                rec["stop"] = "not RUNNING"; f.write(json.dumps(rec) + "\n"); print(json.dumps(rec)); return 1
            light["p_out_vr_w"] = round((light["trace"]["mean"] / 1000) ** 2 / R_LOAD, 2)
            # A: switch-pulse
            bu("regulator", "trace-arm", "--trigger", "switch", "--post", "1800")
            sc, h = scope_step_arm(mv, 0.5)
            sp = bu("regulator", "switch-pulse", a.open_ms)
            time.sleep(0.15); scope_force(h)
            sco = scope_step_read(mv, sc, "A")
            rows, tr = read_trace()
            rec["A"] = dict(open_ms=float(a.open_ms), pulse_ok=sp.get("ok"),
                            trace=step_from_trace(rows, tr.get("file")) if rows else {"error": tr.get("error")},
                            scope=sco)
            if mv >= 15000:
                lr = subprocess.Popen([f"{ROOT}/tools/bringup/bu", "load", "run", "--p", str(a.qtpy_mw), "--seconds", "60",
                                       "--limit-ma", "3000", "--contract-mv", str(mv), "--vin-min-mv", str(int(mv * 0.85))],
                                      cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                time.sleep(4.0)
                rec["heavy"] = heavy = point(mv)
                heavy["p_out_vr_w"] = round((heavy["trace"]["mean"] / 1000) ** 2 / R_LOAD, 2) if "mean" in heavy["trace"] else None
                # B: disarm step
                sc, h = scope_step_arm(mv, 0.06)
                lr.send_signal(signal.SIGTERM)
                time.sleep(0.25); scope_force(h)
                sco = scope_step_read(mv, sc, "B")
                out, _ = lr.communicate(timeout=30)
                try:
                    lj = json.loads(out)
                except json.JSONDecodeError:
                    lj = {"error": out[-400:]}
                rec["B"] = dict(scope=sco, load=lj.get("steps") or lj.get("error"),
                                load_fault=lj.get("fault") or lj.get("last_fault"))
            rec["pd"] = pd(); rec["after"] = status_n(2)
            print(json.dumps(rec)); f.write(json.dumps(rec) + "\n"); f.flush()
            if rec["after"]["state"] != "RUNNING":
                return 1
    finally:
        scope_restore()
    return 0


if __name__ == "__main__":
    sys.exit(main())
