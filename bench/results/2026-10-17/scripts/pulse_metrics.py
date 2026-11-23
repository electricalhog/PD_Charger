#!/usr/bin/env python3
"""Per-pulse metrics and generated tables for the 2026-10-17 Q5/Q6 reconnect tests.

  pulse_metrics.py collect        summaries + traces under data/ -> report/pulse_metrics.csv   (data/ is git-ignored)
  pulse_metrics.py tables         print every generated table from report/pulse_metrics.csv
  pulse_metrics.py update FILE    rewrite the <!-- BEGIN gen:NAME --> ... <!-- END gen:NAME --> blocks in FILE
  pulse_metrics.py check FILE     exit 1 if a block in FILE differs from report/pulse_metrics.csv, or if the
                                  T_RELEASE constants no longer match the traces

Terms: report/TEST_REPORT.md section 2. USB PD limits for the indicative comparison (not a conformance test):
vSrcNew = nominal +-5 %, vSrcValid = vSrcNew +-0.5 V, tSrcTransient 5 ms (USB-IF Source Power Requirements
Test Spec rev 0.78 and the Developer Days 2019 slides).
"""
import csv
import json
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DAY = HERE.parent
ROOT = DAY.parents[2]
CSV = DAY / "report" / "pulse_metrics.csv"
sys.path.insert(0, str(HERE))

# T_release (ms): ensemble mean of V_out - V_0 over aligned traces, 0.25 ms blocks, first time two consecutive
# blocks exceed +75 mV. 28 V with the rectifier on is not resolved by the ADC trace; 4.2 ms is from the TP2 scope.
T_RELEASE = {(5.0, "off"): 1.35, (20.0, "off"): 4.15, (28.0, "off"): 3.90, (28.0, "on"): 4.20}
ENS_MIN_TCMD = {5.0: 2.9, 20.0: 4.7, 28.0: 4.7}      # traces entering the ensemble: T_cmd at least this (ms)
ENS_THR_MV, T_CLASS_MS, FIT_MIN_UNL_MS = 75.0, 0.25, 0.5
SERIES = (("data/steps", "timer"), ("data/steps_tim4", "TIM4"), ("data/steps_tim4_sr", "TIM4"))
V_NEW, V_VALID_MV = 0.05, 500.0
R_LOAD_OHM = 25.3                                     # derived from a 15 V reading on 2026-10-16, not measured on its own
C_B_UF = {5.0: 44.0, 20.0: 16.0, 28.0: 10.0}          # converter-node ceramics at the DC bias (calc, Samsung curve)
C_B_MAX_UF = 44.0
C_L_UF = (("100–120", (100.0, 120.0)), ("271", (271.0,)))   # stated QT Py input; stored free fit of the 5 V connect record (metrics/connect_fit_free.json)
TS = 0.05                                             # trace sample period, ms
LOOP_FC_HZ, C_INT_UF, C_EXT_UF = 100.0, 50.0, 250.0  # regulator_config.h defaults; no runtime change to these before the pulses

FIELDS = ["run", "summary", "timing", "contract_v", "rectifier", "mode", "kind", "t_open_ms", "t_cmd_ms",
          "t_unl_ms", "v0_mv", "pre_sd_mv", "dv_os_mv", "dv_us_mv", "dv_us_pct", "t_min_ms", "rec2_ms", "rec1_ms",
          "skip_n", "railed_n", "trace", "v_min_mv", "t_below_new_ms", "t_below_valid_ms", "i_pre_ma", "i_rc_ma",
          "i_pk_ma", "i_post_ma", "ton_pre_ns", "ton_rc_ns", "slope_mv_ms", "skip_open", "skip_after",
          "post_rms_mv"]
TEXT = {"summary", "timing", "rectifier", "mode", "kind", "trace"}


def mean(x):
    x = list(x)
    return sum(x) / len(x)


def pstdev(x):
    m = mean(x)
    return mean((a - m) ** 2 for a in x) ** 0.5


def trace_rows(rel):
    with open(ROOT / rel) as f:
        return list(csv.DictReader(f))


def arrays(rows):
    g = lambda k, c=int: [c(r[k]) for r in rows]
    return g("t_ms", float), g("v_out_mv"), g("i_cmd_ma"), g("t_on_ns"), g("skipping"), g("switch_on")


def reconnect_index(t, sw):
    k0 = next(k for k in range(len(t)) if t[k] >= 0)
    return k0, next(k for k in range(k0 + 1, len(t)) if sw[k] and not sw[k - 1])


def derive(rows, v_nom, t_rc, v0, t_rel):
    t, v, ic, ton, sk, _ = arrays(rows)
    idx = lambda lo, hi: [k for k in range(len(t)) if lo <= t[k] < hi]
    pre, rc, far = idx(-1e9, 0), idx(t_rc - 0.25, t_rc), idx(t_rc + 10.0, 1e9)
    opn, aft = idx(0, t_rc), idx(t_rc, 1e9)
    lo_new = (1 - V_NEW) * v_nom
    d = {
        "v_min_mv": min(v[k] for k in aft),
        "t_below_new_ms": TS * sum(1 for k in aft if v[k] < lo_new),
        "t_below_valid_ms": TS * sum(1 for k in aft if v[k] < lo_new - V_VALID_MV),
        "i_pre_ma": mean(ic[k] for k in pre), "i_rc_ma": mean(ic[k] for k in rc),
        "i_pk_ma": max(ic[k] for k in idx(t_rc, t_rc + 0.5)), "i_post_ma": mean(ic[k] for k in far),
        "ton_pre_ns": mean(ton[k] for k in pre), "ton_rc_ns": mean(ton[k] for k in rc),
        "skip_open": sum(sk[k] for k in opn), "skip_after": sum(sk[k] for k in aft),
        "post_rms_mv": mean((v[k] - v0) ** 2 for k in far) ** 0.5, "slope_mv_ms": None,
    }
    sel = idx(t_rel + 0.1, t_rc - 0.05)
    if len(sel) >= 6:
        tm, vm = mean(t[k] for k in sel), mean(v[k] for k in sel)
        d["slope_mv_ms"] = sum((t[k] - tm) * (v[k] - vm) for k in sel) / sum((t[k] - tm) ** 2 for k in sel)
    return d


def collect():
    from switch_step import metrics
    items = []
    for sub, timing in SERIES:
        for p in sorted((DAY / sub).glob("summary_*.json")):
            d = json.loads(p.read_text())
            for i, s in enumerate(d["steps"]):
                items.append((p.stat().st_mtime, i, p, timing, d, s))
    items.sort(key=lambda x: (x[0], x[1]))
    owner = {it[5]["file"]: n for n, it in enumerate(items)}      # the last pulse to write a trace file owns it
    out, warn = [], []
    for n, (_, _, p, timing, d, s) in enumerate(items):
        v_nom, vc = d["contract_mv"], d["contract_mv"] / 1000
        kept = owner[s["file"]] == n
        rows = trace_rows(s["file"]) if kept else None
        pulse_ms = s["pulse_us_measured"] / 1000 if s.get("pulse_us_measured") else None
        m = metrics(rows, v_nom, pulse_ms) if kept else s
        if kept:
            for key in ("v_before_mv", "dip_mv", "peak_while_open_mv", "skipping_samples", "pre_sd_mv"):
                if key in s and s[key] != m[key]:
                    warn.append(f"run {n + 1}: {key} summary {s[key]} trace {m[key]}")
        t_cmd = pulse_ms if pulse_ms is not None else m["reconnect_sample_ms"]
        sr = "on" if s["rectifier_on"] else "off"
        t_unl = t_cmd - T_RELEASE[(vc, sr)]
        rec = {
            "run": n + 1, "summary": p.name, "timing": timing, "contract_v": vc, "rectifier": sr,
            "mode": "BUCK_BOOST" if vc > 24.6 else "BUCK", "kind": "sub-release" if t_unl <= T_CLASS_MS else "event",
            "t_open_ms": s.get("open_ms", d["open_ms"]), "t_cmd_ms": t_cmd, "t_unl_ms": t_unl,
            "v0_mv": m["v_before_mv"], "pre_sd_mv": m.get("pre_sd_mv"), "dv_os_mv": m["peak_while_open_mv"],
            "dv_us_mv": -m["dip_mv"], "dv_us_pct": -100.0 * m["dip_mv"] / m["v_before_mv"],
            "t_min_ms": m["dip_at_ms_after_reconnect"], "rec2_ms": m["settled_2pct_ms_after_reconnect"],
            "rec1_ms": m["settled_1pct_ms_after_reconnect"], "skip_n": m["skipping_samples"],
            "railed_n": m.get("railed_samples"), "trace": s["file"] if kept else "",
        }
        if kept:
            rec["mode"] = rows[0]["mode"]
            rec.update(derive(rows, v_nom, t_cmd, m["v_before_mv"], T_RELEASE[(vc, sr)]))
        out.append(rec)
    with open(CSV, "w", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        w.writeheader()
        for r in out:
            w.writerow({k: ("" if r.get(k) is None else (f"{r[k]:.6g}" if isinstance(r[k], float) else r[k]))
                        for k in FIELDS})
    print(f"{len(out)} pulses, {sum(1 for r in out if r['trace'])} traces -> {CSV.relative_to(ROOT)}")
    for w_ in warn:
        print("MISMATCH", w_)


def read():
    rows = []
    for r in csv.DictReader(open(CSV)):
        rows.append({k: (r[k] if k in TEXT else (None if r[k] == "" else float(r[k]))) for k in FIELDS})
    return rows


def f(x, nd=2, dash="—"):
    return dash if x is None else f"{x:.{nd}f}"


def rng(xs, nd=2):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "—"
    return f"{min(xs):.{nd}f}" if min(xs) == max(xs) else f"{min(xs):.{nd}f} to {max(xs):.{nd}f}"


def label(r):
    return f"{r['contract_v']:g} V {'buck' if r['mode'] == 'BUCK' else 'buck-boost'}" + (
        ", SR on" if r["rectifier"] == "on" else "")


def groups(rows, kind=None):
    g = {}
    for r in rows:
        if kind is None or r["kind"] == kind:
            g.setdefault((r["contract_v"], r["rectifier"]), []).append(r)
    return dict(sorted(g.items()))


def md(head, body):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
                     + ["| " + " | ".join(r) + " |" for r in body])


def fit(xs, ys):
    xm, ym = mean(xs), mean(ys)
    b = sum((x - xm) * (y - ym) for x, y in zip(xs, ys)) / sum((x - xm) ** 2 for x in xs)
    a = ym - b * xm
    sd = (sum((y - a - b * x) ** 2 for x, y in zip(xs, ys)) / (len(xs) - 2)) ** 0.5 if len(xs) > 2 else None
    return b, a, sd


def band(r):
    lo = (1 - V_NEW) * r["contract_v"] * 1000
    return "in" if r["v_min_mv"] >= lo else ("window" if r["v_min_mv"] >= lo - V_VALID_MV else "beyond")


def release_ensemble(rows):
    """{(V, SR): (ensemble crossing at +40/+75/+100 mV, per-trace crossings at +75 mV, n traces)} from the traces."""
    res = {}
    for key, g in groups(rows).items():
        v_nom = key[0]
        use = [x for x in g if x["trace"] and x["t_cmd_ms"] >= ENS_MIN_TCMD[v_nom]]
        if not use:
            continue
        tr = []
        for x in use:
            t, v, _, _, _, sw = arrays(trace_rows(x["trace"]))
            k0, kc = reconnect_index(t, sw)
            tr.append(([t[k] for k in range(k0, kc)], [v[k] - x["v0_mv"] for k in range(k0, kc)]))
        n_min = min(len(d) for _, d in tr)

        def cross(d, thr):
            for i in range(len(d) - 9):
                if mean(d[i:i + 5]) > thr and mean(d[i + 5:i + 10]) > thr:
                    return i
            return None
        ens = [mean(d[i] for _, d in tr) for i in range(n_min)]
        at = {}
        for thr in (40.0, ENS_THR_MV, 100.0):
            i = cross(ens, thr)
            at[thr] = None if i is None else tr[0][0][i]
        ref = at[ENS_THR_MV] or T_RELEASE[key]
        per = []
        for ts, d in tr:
            i = cross(d, ENS_THR_MV)
            per.append(None if i is None else ts[i])
        res[key] = (at, per, len(tr), ref)
    return res


def tbl_release(rows):
    body = []
    for key, (at, per, n, ref) in release_ensemble(rows).items():
        ok = [p for p in per if p is not None and p >= 0.5 * ref]
        early = [p for p in per if p is not None and p < 0.5 * ref]
        notes = []
        if early:
            notes.append(f"{len(early)} crossing at {', '.join(f'{p:.2f}' for p in early)} ms ignored (background excursion)")
        if len(ok) < n - len(early):
            notes.append("no crossing in some traces")
        unresolved = key == (28.0, "on")
        body.append([f"{key[0]:g} V, SR {key[1]}", str(n), f(at[ENS_THR_MV]),
                     f"{f(at[40.0])} / {f(at[100.0], dash='none')}",
                     "unresolved (σ_pre 61–80 mV)" if unresolved else (
                         f"{rng(ok)} (n = {len(ok)})" + ("; " + "; ".join(notes) if notes else "")),
                     f"{T_RELEASE[key]:.2f}" + (" (TP2 scope)" if unresolved else "")])
    return md(["Configuration", "Traces in ensemble", "T_release, ensemble at +75 mV (ms)",
               "Ensemble at +40 / +100 mV (ms)", "Single traces at +75 mV (ms)", "T_release used (ms)"], body)


def tbl_floor(rows):
    body = []
    for (v, sr), g in groups(rows).items():
        sub = [x for x in g if x["kind"] == "sub-release"]
        ev = [x for x in g if x["kind"] == "event"]
        sd = [x["pre_sd_mv"] for x in g if x["pre_sd_mv"]]
        two = lambda key: f"{mean(x[key] for x in sub) / 1000:.3f} / {max(x[key] for x in sub) / 1000:.2f}" if sub else "—"
        body.append([f"{v:g} V, SR {sr}", str(len(g)), str(len(sub)), str(len(ev)), two("dv_us_mv"), two("dv_os_mv"),
                     f"{mean(sd):.0f} ({min(sd):.0f}–{max(sd):.0f})"])
    return md(["Configuration", "Pulses", "Sub-release (T_unl ≤ 0.25 ms)", "Events", "ΔV_us of sub-release pulses, V: mean / max (= E_bg)",
               "ΔV_os of sub-release pulses, V: mean / max", "σ_pre, mV: mean (min–max)"], body)


def tbl_matrix(rows):
    cfgs = [(20.0, "off"), (28.0, "off"), (28.0, "on")]
    tcs = sorted({x["t_open_ms"] for x in rows if x["kind"] == "event" and x["contract_v"] != 5.0})
    body = []
    for tc in tcs:
        line = [f"{tc:.2f}"]
        for cfg in cfgs:
            g = [x for x in rows if (x["contract_v"], x["rectifier"]) == cfg and abs(x["t_open_ms"] - tc) < 1e-9
                 and x["kind"] == "event"]
            line.append("—" if not g else f"{g[0]['t_unl_ms']:.2f}: " + ", ".join(f"{x['dv_us_mv'] / 1000:.2f}" for x in g))
        body.append(line)
    return md(["Nominal T_cmd (ms)", "20 V, SR off: T_unl (ms): ΔV_us (V)", "28 V, SR off: T_unl: ΔV_us",
               "28 V, SR on: T_unl: ΔV_us (each pulse)"], body)


def tbl_pulses(rows):
    body = []
    key = lambda x: (x["contract_v"], x["rectifier"], x["t_cmd_ms"], x["run"])
    for x in sorted(rows, key=key):
        sub = x["kind"] == "sub-release"
        tr = bool(x["trace"])
        body.append([f"{int(x['run'])}", label(x) + (" ¹" if x["timing"] == "timer" else ""),
                     f"{x['t_cmd_ms']:.3f}", f"{x['t_unl_ms']:+.2f}" + (" *" if sub else ""),
                     f(x["dv_os_mv"] / 1000), f(x["dv_us_mv"] / 1000), f(x["dv_us_pct"], 1),
                     f(x["t_min_ms"], 2), "—" if x["rec2_ms"] == 0 else f(x["rec2_ms"], 2),
                     f(x["t_below_new_ms"], 2) if tr else "—", f(x["railed_n"], 0) if x["railed_n"] is not None else "—",
                     f"{x['skip_open']:.0f} / {x['skip_after']:.0f}" if tr else f"{x['skip_n']:.0f} total ²",
                     "" if tr else "trace overwritten"])
    return md(["Run", "Configuration", "T_cmd (ms)", "T_unl (ms)", "ΔV_os (V)", "ΔV_us (V)", "% of V_0", "t_min (ms)",
               "t_rec 2 % (ms)", "Time < 0.95 V_nom (ms)", "Railed", "Skip samples: open / after", "Note"], body)


def tbl_open(rows):
    body = []
    for (v, sr), g in groups(rows, "event").items():
        g = [x for x in g if x["trace"] and x["t_unl_ms"] >= FIT_MIN_UNL_MS]
        if not g:
            continue
        sl = [x["slope_mv_ms"] / 1000 for x in g if x["slope_mv_ms"] is not None]
        body.append([label(g[0]), str(len(g)), rng([x["t_unl_ms"] for x in g], 2), rng([x["dv_os_mv"] / 1000 for x in g]),
                     rng(sl, 3),
                     rng([100 * (x["i_rc_ma"] - x["i_pre_ma"]) / x["i_pre_ma"] for x in g], 1),
                     rng([100 * (x["ton_rc_ns"] - x["ton_pre_ns"]) / x["ton_pre_ns"] for x in g], 1),
                     rng([x["skip_open"] for x in g], 0)])
    return md(["Configuration", "n", "T_unl (ms)", "ΔV_os (V)", "V_out slope after T_release + 0.1 ms (V/ms)",
               "I_cmd, 0.25 ms before reconnect vs before the event (%)", "t_on, same (%)", "Skip samples while open"], body)


def tbl_loop(rows):
    body = []
    for (v, sr), g in groups(rows, "event").items():
        g = [x for x in g if x["trace"]]
        big = [x for x in g if x["dv_us_mv"] >= 800]
        if len(big) >= 3 and max(x["dv_us_mv"] for x in big) - min(x["dv_us_mv"] for x in big) > 800:
            b, a, sd = fit([x["dv_us_mv"] for x in big], [x["i_pk_ma"] - x["i_rc_ma"] for x in big])
            kp = f"{b:.3f} (n = {len(big)}, residual sd {sd:.0f} mA)"
        elif big:
            kp = f"{mean((x['i_pk_ma'] - x['i_rc_ma']) / x['dv_us_mv'] for x in big):.3f} (mean of {len(big)})"
        else:
            kp = "—"
        far = [x for x in g if x["t_unl_ms"] >= FIT_MIN_UNL_MS]
        body.append([label(g[0]), str(len(g)), f"{mean(x['i_pre_ma'] for x in g):.0f}", kp,
                     rng([x["i_pk_ma"] / x["i_pre_ma"] for x in far], 2), rng([x["railed_n"] for x in g], 0),
                     rng([x["post_rms_mv"] / x["pre_sd_mv"] for x in g], 1),
                     rng([x["i_post_ma"] - x["i_pre_ma"] for x in g], 0)])
    return md(["Configuration", "Event traces", "I_cmd before the event (mA)", "(I_pk − I_rc) / ΔV_us (mA/mV), pulses with ΔV_us ≥ 0.8 V",
               "I_pk / I_cmd before (T_unl ≥ 0.5 ms)", "Railed samples per trace",
               "rms(V_out − V_0) / σ_pre, 10 ms to end", "I_cmd 10 ms to end minus before (mA)"], body)


def tbl_rectifier(rows):
    body, pairs = [], []
    for tc in sorted({x["t_open_ms"] for x in rows if x["contract_v"] == 28.0 and x["rectifier"] == "on"}):
        pick = lambda sr: [x for x in rows if x["contract_v"] == 28.0 and x["rectifier"] == sr
                           and abs(x["t_open_ms"] - tc) < 1e-9]
        off, on = pick("off"), pick("on")
        if not off or not on:
            continue
        mo, mf = mean(x["dv_us_mv"] for x in on) / 1000, off[0]["dv_us_mv"] / 1000
        pr = abs(on[0]["dv_us_mv"] - on[1]["dv_us_mv"]) / 1000
        pairs.append(pr)
        body.append([f"{tc:.2f}", f(mf), f(mo), f"{mo - mf:+.2f}", f(pr), f(off[0]["railed_n"], 0),
                     " / ".join(f(x["railed_n"], 0) for x in on)])
    sd = mean(p ** 2 / 2 for p in pairs) ** 0.5
    pre = lambda sr: [x["pre_sd_mv"] for x in rows if x["contract_v"] == 28.0 and x["rectifier"] == sr and x["pre_sd_mv"]]
    off_s, on_s = pre("off"), pre("on")
    return (md(["Nominal T_cmd (ms)", "ΔV_us, SR off (V)", "ΔV_us, SR on, mean of 2 (V)", "On − off (V)",
                "SR-on repeat difference (V)", "Railed samples, SR off", "Railed samples, SR on (each pulse)"], body)
            + f"\n\nPooled sd of the SR-on repeats {sd:.2f} V (four pairs). σ_pre: SR off {mean(off_s):.0f} mV "
              f"(n = {len(off_s)}, {min(off_s):.0f}–{max(off_s):.0f}); SR on {mean(on_s):.0f} mV "
              f"(n = {len(on_s)}, {min(on_s):.0f}–{max(on_s):.0f}); ratio {mean(on_s) / mean(off_s):.1f}.")


def tbl_pd(rows):
    body = []
    for (v, sr), g in groups(rows, "event").items():
        vn = v * 1000
        lo, lov, hi = (1 - V_NEW) * vn, (1 - V_NEW) * vn - V_VALID_MV, (1 + V_NEW) * vn
        real = [x for x in g if x["t_unl_ms"] >= FIT_MIN_UNL_MS and x["v_min_mv"] is not None]
        ok = [x for x in real if band(x) == "in"]
        win = [x for x in real if band(x) == "window"]
        bad = [x for x in real if band(x) == "beyond"]
        last_ok = max((x["t_unl_ms"] for x in real if band(x) != "beyond"), default=None)
        first_bad = min((x["t_unl_ms"] for x in bad), default=None)
        tw = [x["t_below_new_ms"] - x["t_below_valid_ms"] for x in real if x["trace"]]
        body.append([label(g[0]), f"{lo / 1000:.2f} / {lov / 1000:.2f}", str(len(real)),
                     f"{len(ok)} / {len(win)} / {len(bad)}", f"{f(last_ok)} / {f(first_bad)}",
                     f(max(tw)) if tw else "—",
                     f"{max(x['dv_os_mv'] for x in g) / 1000:.2f} V against +{(hi - vn) / 1000:.2f} V"])
    return md(["Configuration", "V_out lower limit: vSrcNew / vSrcValid (V)", "Events with T_unl ≥ 0.5 ms",
               "V_min in vSrcNew / in the window / beyond vSrcValid", "T_unl (ms): last within vSrcValid / first beyond",
               "Longest time in the vSrcNew–vSrcValid window (ms)", "Largest ΔV_os"], body)


def tbl_slopes(rows):
    body = []
    for (v, sr), g in groups(rows, "event").items():
        pts = [x for x in g if x["t_unl_ms"] >= FIT_MIN_UNL_MS and v != 5.0]
        if len(pts) < 4:
            continue
        b, a, sd = fit([x["t_unl_ms"] for x in pts], [x["dv_us_mv"] / 1000 for x in pts])
        body.append([label(pts[0]), str(len(pts)), f"{b:.2f}", f"{sd:.2f}", f"{a:+.2f}"])
    return md(["Configuration", "n (T_unl ≥ 0.5 ms)", "Slope of ΔV_us on T_unl (V/ms)", "Residual sd (V)", "Intercept (V)"], body)


def tbl_summary(rows):
    cols = list(groups(rows).items())
    head = ["Quantity"] + [f"{v:g} V {'buck' if g[0]['mode'] == 'BUCK' else 'buck-boost'}, SR {sr}" for (v, sr), g in cols]
    ev = lambda g: [x for x in g if x["kind"] == "event"]
    real = lambda g: [x for x in ev(g) if x["t_unl_ms"] >= FIT_MIN_UNL_MS]
    kept = lambda g: [x for x in real(g) if x["v_min_mv"] is not None]
    e_bg_mv = {}
    for x in rows:
        if x["kind"] == "sub-release":
            e_bg_mv[x["contract_v"]] = max(e_bg_mv.get(x["contract_v"], 0.0), x["dv_us_mv"])
    at_bg = lambda x: x["dv_us_mv"] <= e_bg_mv.get(x["contract_v"], 0.0) and x["t_min_ms"] > 1.0
    resp = lambda g: [x for x in real(g) if not at_bg(x)]

    def e_bg(key, g):
        sub = [x["dv_us_mv"] / 1000 for x in g if x["kind"] == "sub-release"]
        return f"{max(sub):.2f}" if sub else "no control pulses"

    def slope(key, g):
        pts = real(g)
        if key[0] == 5.0 or len(pts) < 4:
            return "—"
        return f"{fit([x['t_unl_ms'] for x in pts], [x['dv_us_mv'] / 1000 for x in pts])[0]:.2f}"

    def t_rec(key, g):
        r = resp(g)
        n0 = sum(1 for x in r if x["rec2_ms"] == 0)
        return rng([x["rec2_ms"] for x in r if x["rec2_ms"]]) + (f" ({n0} of {len(r)} never left the band)" if n0 else "")

    def limits(key, g):
        k = kept(g)
        ok = max((x["t_unl_ms"] for x in k if band(x) != "beyond"), default=None)
        bad = min((x["t_unl_ms"] for x in k if band(x) == "beyond"), default=None)
        return f"{f(ok)} / {f(bad)}"

    spec = [
        ("Pulses: sub-release + events", lambda k, g: f"{len(g)}: {len(g) - len(ev(g))} + {len(ev(g))}"),
        ("T_release used (ms)", lambda k, g: f"{T_RELEASE[k]:.2f}" + (" (TP2 scope)" if k == (28.0, "on") else "")),
        ("E_bg: largest ΔV_us of a sub-release pulse (V)", e_bg),
        ("Events with T_unl ≥ 0.5 ms: n; T_unl (ms)", lambda k, g: f"{len(real(g))}; {rng([x['t_unl_ms'] for x in real(g)])}"),
        ("ΔV_us of those events (V)", lambda k, g: rng([x["dv_us_mv"] / 1000 for x in real(g)])),
        ("ΔV_us / V_0 (%)", lambda k, g: rng([x["dv_us_pct"] for x in real(g)], 1)),
        ("Slope of ΔV_us on T_unl (V/ms)", slope),
        ("…of which at background level (ΔV_us ≤ E_bg of that voltage, t_min > 1 ms)", lambda k, g: str(sum(1 for x in real(g) if at_bg(x)))),
        ("t_min after the reconnect sample (ms), events above background", lambda k, g: rng([x["t_min_ms"] for x in resp(g)])),
        ("t_rec(2 %) (ms), events above background", t_rec),
        ("ΔV_os while open (V)", lambda k, g: rng([x["dv_os_mv"] / 1000 for x in real(g)])),
        ("V_min beyond vSrcValid, events with a retained trace",
         lambda k, g: f"{sum(1 for x in kept(g) if band(x) == 'beyond')} of {len(kept(g))}"),
        ("T_unl (ms): last event within vSrcValid / first beyond", limits),
        ("Railed samples per event trace", lambda k, g: rng([x["railed_n"] for x in ev(g) if x["trace"]], 0)),
        ("σ_pre over all pulses, mV: mean (min–max)",
         lambda k, g: f"{mean(x['pre_sd_mv'] for x in g if x['pre_sd_mv']):.0f} "
                      f"({min(x['pre_sd_mv'] for x in g if x['pre_sd_mv']):.0f}–{max(x['pre_sd_mv'] for x in g if x['pre_sd_mv']):.0f})"),
    ]
    return md(head, [[name] + [fn(k, g) for k, g in cols] for name, fn in spec])


def share(v0_mv, t_unl_ms, c_l, c_b):
    """Passive charge sharing: C_L (R_LOAD || C_L, discharged for T_unl) is attached to C_b at V_0."""
    tau_ms = R_LOAD_OHM * c_l * 1e-3
    return c_l / (c_l + c_b) * v0_mv / 1000 * (1 - math.exp(-t_unl_ms / tau_ms))


def tbl_model(rows):
    body, src = [], []
    for (v, sr), g in groups(rows, "event").items():
        ev = [x for x in g if x["t_unl_ms"] >= FIT_MIN_UNL_MS]
        meas = [x["dv_us_mv"] / 1000 for x in ev]
        for name, cls in C_L_UF:
            def pred(cb):
                return [share(x["v0_mv"], x["t_unl_ms"], cl, cb) for x in ev for cl in cls]
            m_rep = [m for m in meas for _ in cls]
            nom, big = pred(C_B_UF[v]), pred(C_B_MAX_UF)
            body.append([label(g[0]), str(len(ev)), f"{C_B_UF[v]:g}", name,
                         rng([R_LOAD_OHM * cl * 1e-3 for cl in cls], 1), rng(nom), rng(meas),
                         rng([a / b for a, b in zip(nom, m_rep)], 1), rng([a / b for a, b in zip(big, m_rep)], 1)])
            src += [x["i_pk_ma"] * 0.05 / (C_B_UF[v] + cl) for x in ev if x["i_pk_ma"] is not None for cl in cls]
    return (md(["Configuration", "n (T_unl ≥ 0.5 ms)", "C_b (µF)", "C_L (µF)", "R·C_L (ms)", "Predicted ΔV_us (V)",
                "Measured ΔV_us (V)", "Predicted ÷ measured", "Predicted ÷ measured, C_b = 44 µF"], body)
            + f"\n\nSource term not subtracted: I_pk × 50 µs / (C_b + C_L) = {min(src):.2f} to {max(src):.2f} V "
              f"(I_pk = peak I_cmd within 0.5 ms of the reconnect). R = {R_LOAD_OHM} Ω.")


EXC_V, EXC_DEEP_V, EXC_GAP, EXC_AFTER_MS = 0.25, 0.40, 20, 10.0   # V, V, samples (= 1 ms), ms after the reconnect sample


def median(x):
    x = sorted(x)
    n = len(x)
    return x[n // 2] if n % 2 else (x[n // 2 - 1] + x[n // 2]) / 2


def clusters(idx, d, hit):
    """Samples of idx with hit(d[i]), merged into events when closer than EXC_GAP samples."""
    ev = []
    for i in idx:
        if hit(d[i]):
            if ev and i - ev[-1][-1] < EXC_GAP:
                ev[-1].append(i)
            else:
                ev.append([i])
    return ev


def tbl_excursions(rows):
    """Excursions of the 50 us ADC samples from V_0 in the window from the reconnect sample + 10 ms to the end of the record."""
    body = []
    for (v, sr), g in groups(rows).items():
        n_tr = post_n = pre_n = flagged = 0
        neg, pos, pre_neg = [], [], []
        for x in g:
            if not x["trace"]:
                continue
            t, vv, _, _, sk, sw = arrays(trace_rows(x["trace"]))
            _, krc = reconnect_index(t, sw)
            d = [(a - x["v0_mv"]) / 1000 for a in vv]
            post = [i for i in range(len(t)) if t[i] >= t[krc] + EXC_AFTER_MS]
            pre = [i for i in range(len(t)) if t[i] < 0]
            n_tr, post_n, pre_n = n_tr + 1, post_n + len(post), pre_n + len(pre)
            for e in clusters(post, d, lambda a: a < -EXC_V):
                neg.append((min(d[i] for i in e), len(e)))
                flagged += any(any(sk[max(0, i - 2):i + 3]) for i in e)
            pos += clusters(post, d, lambda a: a > EXC_V)
            pre_neg += clusters(pre, d, lambda a: a < -EXC_V)
        ms = TS * post_n
        depth = [a for a, _ in neg]
        body.append([f"{v:g} V, SR {sr}", str(n_tr), f"{ms:.0f}", f"{len(neg)} ({sum(n for _, n in neg)})", f"{1000 * len(neg) / ms:.1f}",
                     f"{min(depth):.3f} / {median(depth):.3f}" if neg else "—",
                     f"{median([n for _, n in neg]):g} / {max(n for _, n in neg)}" if neg else "—",
                     str(sum(1 for a in depth if a < -EXC_DEEP_V)), str(len(pos)), str(flagged), f"{len(pre_neg)} in {TS * pre_n:.0f}"])
    return md(["Configuration", "Traces", "Window (ms)", "Events below −0.25 V (samples)", "Events per s",
               "Depth, V: deepest / median", "Samples per event: median / max", "Events below −0.40 V",
               "Events above +0.25 V", "Events with the skipping flag within ±2 samples", "Same threshold before the trigger: events in ms"], body)


def tbl_spectra(rows):
    """Amplitude spectra of V_out in the same window (last N samples, N common to the group), Hann window, rms-combined over traces."""
    import numpy as np
    bands = ((20, 100), (100, 500), (500, 2000), (2000, 10000))
    lines = (24.6, 321.0)
    b1, b2 = [], []
    for (v, sr), g in groups(rows).items():
        if v == 5.0:
            continue
        recs = []
        for x in g:
            if not x["trace"]:
                continue
            t, vv, _, _, sk, sw = arrays(trace_rows(x["trace"]))
            _, krc = reconnect_index(t, sw)
            first = next(i for i in range(len(t)) if t[i] >= t[krc] + EXC_AFTER_MS)
            recs.append((np.array(vv, float) / 1000.0, np.array(sk), len(t) - first))
        n = min(r[2] for r in recs)
        w = np.hanning(n)
        f_hz = np.fft.rfftfreq(n, TS * 1e-3)
        a2, p, sig, trans = [], [], [], 0
        for vv, sk, _ in recs:
            xx = vv[-n:] - vv[-n:].mean()
            big_x = np.fft.rfft(xx * w)
            a2.append((2 * np.abs(big_x) / w.sum()) ** 2)
            p.append(2 * np.abs(big_x) ** 2 / (n * (w ** 2).sum()))
            sig.append(xx.std() * 1e3)
            trans += int(np.sum(np.abs(np.diff(sk[-n:])) > 0))
        amp, pw = np.sqrt(np.mean(a2, axis=0)), np.mean(p, axis=0)
        rms = [float(np.sqrt(pw[(f_hz >= lo) & (f_hz < hi)].sum())) * 1e3 for lo, hi in bands]
        cell = []
        for target in lines:
            m = (f_hz > target - 15) & (f_hz < target + 15)
            k = int(np.argmax(amp[m]))
            mb = ((f_hz > target - 200) & (f_hz < target - 40)) | ((f_hz > target + 40) & (f_hz < target + 200))
            cell += [f"{f_hz[m][k]:.0f}", f"{amp[m][k] * 1e3:.1f}", f"{np.median(amp[mb]) * 1e3:.1f}"]
        name = f"{v:g} V, SR {sr}"
        b1.append([name, str(len(recs)), f"{n * TS:.1f}", f"{f_hz[1]:.1f}", f"{mean(sig):.1f} ({min(sig):.1f}–{max(sig):.1f})"]
                  + [f"{r:.1f}" for r in rms] + [f"{math.sqrt(sum(r * r for r in rms)):.1f}", str(trans)])
        b2.append([name] + cell)
    return (md(["Configuration", "Traces", "Window (ms)", "Bin (Hz)", "σ of V_out, mV: mean (min–max)"]
               + [f"Band rms {lo}–{hi} Hz (mV)" for lo, hi in bands] + ["√Σ bands² (mV)", "Skipping-flag transitions in the windows"], b1)
            + "\n\n" + md(["Configuration", "Peak bin near 24.6 Hz (Hz)", "Amplitude (mV pk)", "Local median (mV)",
                           "Peak bin near 321 Hz (Hz)", "Amplitude (mV pk)", "Local median (mV)"], b2))


def pi_gains(c_uf):
    """kp (mA/mV) and ki (mA/mV/s) as regulator.c step 2 forms them."""
    kp = 6.2831853 * LOOP_FC_HZ * c_uf * 1e-6
    return kp, kp * 6.2831853 * LOOP_FC_HZ * 0.25


def reconstruct(v, ic, sp, k0, kc, kp, ki, kp_prev, dt=0.05e-3):
    """I_cmd (mA) for samples k0..kc-1 from the measured V_out (mV) and setpoint sp, forward-Euler PI as pid_controller.c.
    The integrator at the last closed sample is the measured I_cmd there minus kp_prev times its error."""
    integ = ic[k0 - 1] - kp_prev * (sp - v[k0 - 1])
    out = []
    for i in range(k0, kc):
        e = sp - v[i]
        integ += ki * e * dt
        out.append(kp * e + integ)
    return out


def regress(xs, ys):
    xm, ym = mean(xs), mean(ys)
    return sum((x - xm) * (y - ym) for x, y in zip(xs, ys)) / sum((x - xm) ** 2 for x in xs)


def tbl_schedule(rows):
    """I_cmd while the OUTPUT_EN flag is low, rebuilt from the measured V_out with the open (50 uF) and closed (300 uF) gains."""
    kp_c, ki_c = pi_gains(C_INT_UF + C_EXT_UF)
    kp_o, ki_o = pi_gains(C_INT_UF)
    b1, b2 = [], []
    for (v, sr), g in groups(rows).items():
        rms_o, rms_c, mx_o, n_s, clamp, sl = [], [], 0.0, 0, 0, []
        for x in g:
            if not x["trace"]:
                continue
            tr = trace_rows(x["trace"])
            t, vv, ic, _, _, sw = arrays(tr)
            im = [int(r["i_max_ma"]) for r in tr]
            k0, kc = reconnect_index(t, sw)
            sp = x["contract_v"] * 1000
            po = reconstruct(vv, ic, sp, k0, kc, kp_o, ki_o, kp_c)
            pc = reconstruct(vv, ic, sp, k0, kc, kp_c, ki_c, kp_c)
            m = ic[k0:kc]
            eo, ec = [a - b for a, b in zip(po, m)], [a - b for a, b in zip(pc, m)]
            rms_o.append(mean(e * e for e in eo) ** 0.5)
            rms_c.append(mean(e * e for e in ec) ** 0.5)
            mx_o = max(mx_o, max(abs(e) for e in eo))
            n_s += kc - k0
            clamp += sum(1 for i in range(k0, kc) if ic[i] <= 0 or ic[i] >= im[i] - 2)
            sel = [i for i in range(k0, kc) if t[i] >= 1.5]
            if len(sel) >= 25:
                vs = [vv[i] for i in sel]
                sl.append((regress(vs, [ic[i] for i in sel]), regress(vs, [po[i - k0] for i in sel]),
                           regress(vs, [pc[i - k0] for i in sel])))
        name = f"{v:g} V, SR {sr}"
        b1.append([name, str(len(rms_o)), str(n_s), rng(rms_o), f"{mx_o:.2f}", rng(rms_c), str(clamp)])
        if sl:
            b2.append([name, str(len(sl))] + [rng([s_[k] for s_ in sl], 4) for k in range(3)])
    return (md(["Configuration", "Traces", "Open-interval samples", "rms error, open gains (mA), per trace",
                "Largest absolute error, open gains (mA)", "rms error, closed gains (mA), per trace",
                "Samples at a clamp (I_cmd ≤ 0 or ≥ I_max − 2 mA)"], b1)
            + "\n\n" + md(["Configuration", "Traces with ≥ 25 open samples after 1.5 ms",
                           "dI_cmd/dV_out measured (mA/mV)", "Rebuilt, open gains", "Rebuilt, closed gains"], b2)
            + f"\n\nGains: open kp {kp_o:.4f} mA/mV, ki {ki_o:.2f} mA/mV/s (C = {C_INT_UF:g} µF); closed kp {kp_c:.4f}, ki {ki_c:.2f} "
              f"(C = {C_INT_UF + C_EXT_UF:g} µF); f_c = {LOOP_FC_HZ:g} Hz. Trace I_cmd is truncated to 1 mA, so two truncations "
              f"alone give {(2 / 12) ** 0.5:.2f} mA rms.")


BLOCKS = {"summary": tbl_summary, "release": tbl_release, "floor": tbl_floor, "excursions": tbl_excursions, "spectra": tbl_spectra, "matrix": tbl_matrix, "open": tbl_open, "loop": tbl_loop,
          "schedule": tbl_schedule, "rectifier": tbl_rectifier, "pd": tbl_pd, "slopes": tbl_slopes, "model": tbl_model, "pulses": tbl_pulses}


def render(fn, rows):
    text = fn(rows)
    text = re.sub(r"(?<![\w.])-0(?![\d.])", "0", text)
    return re.sub(r"(?<![\w.\-])-(?=\d)", "\u2212", text)


def check_constants(rows):
    bad = 0
    for key, (at, _, _, _) in release_ensemble(rows).items():
        if key == (28.0, "on"):
            continue
        if at[ENS_THR_MV] is None or abs(at[ENS_THR_MV] - T_RELEASE[key]) > 0.01:
            print(f"T_RELEASE{key} = {T_RELEASE[key]} but the traces give {at[ENS_THR_MV]}")
            bad += 1
    return bad


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "tables"
    if cmd == "collect":
        collect()
    elif cmd == "tables":
        rows = read()
        for name, fn in BLOCKS.items():
            print(f"### {name}\n{render(fn, rows)}\n")
    elif cmd in ("update", "check"):
        path = Path(sys.argv[2])
        text, rows, bad = path.read_text(), read(), 0
        for name, fn in BLOCKS.items():
            pat = re.compile(rf"(<!-- BEGIN gen:{name} -->\n)(.*?)(\n?<!-- END gen:{name} -->)", re.S)
            m = pat.search(text)
            if not m:
                if f"<!-- BEGIN gen:{name} -->" in text:
                    print(f"gen:{name} has a BEGIN marker without a matching END marker")
                    bad += 1
                continue
            new = render(fn, rows)
            if m.group(2) != new:
                bad += 1
                print(f"gen:{name} {'rewritten' if cmd == 'update' else 'differs from the data'}")
                if cmd == "update":
                    sep = "" if m.group(3).startswith("\n") else "\n"
                    text = text[:m.start(2)] + new + sep + text[m.end(2):]
        if cmd == "update":
            path.write_text(text)
        else:
            bad += check_constants(rows)
            if bad:
                sys.exit(1)
            print("generated blocks match the data; T_RELEASE matches the traces")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
