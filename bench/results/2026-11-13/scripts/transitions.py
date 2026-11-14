"""Transition suites for the battery-load build (bench 2026-11-13; plans/lipo-load.md section 7).

What a laptop on 48 V / 5 A asks of the converter, as far as this bench can show it: current steps and ramps
through the DCM / CCM boundary and the current-limit / voltage hand-offs (the 8S pack, a stiff power sink at
~31 V), and voltage slews through V_in, the buck / buck-boost / boost boundary (the 25.3 ohm resistor, VBUS
free).  Each scenario is one START; the regulator trace (20 kHz, 2048 samples = 102 ms) is armed just before
the transition the scenario is for.  No ammeter prompts (ID_MON is calibrated: 991 / +4, 2026-11-13).

WITHOUT --go NOTHING TOUCHES THE BOARD: the call prints the plan and exits.

  pack       THE PACK ON VBUS, THE RESISTOR OFF.  THIS TOUCHES THE PACK: run it only on Dan's go.  Every
             scenario reads the pack (output switch closed 30 ms, every FET off), sets the ceiling to the read +
             the margin for its top current (boost_cal.above_for), SW_OCP at ID_MON's full scale, and stops on
             a fault, on either current estimate over max(2 x, + 1 A) the limit of the moment (at most 7.5 A)
             for 3 samples, or on Ctrl-C.  Needs the 2026-11-13 BatteryNext build (entry window, slew).
  resistor   THE 25.3 OHM ON VBUS, NO PACK.  A START at 5 V first checks that nothing holds VBUS up.  Ceilings
             12-33 V (the battery build's cap is 34 V): starts from 5 V at the firmware's 80 V/s, ceiling steps
             across V_in both ways, and dwells on the boundary.

The suite stops at the first scenario that faults, aborts, or is refused, and never clears a fault.

Run from the repository root:
  uv run --quiet --project tools/bringup python bench/results/2026-11-13/scripts/transitions.py pack
  uv run --quiet --project tools/bringup python bench/results/2026-11-13/scripts/transitions.py resistor
add --go to run it; --only NAME[,NAME] runs a subset; --list prints the scenarios; --selftest runs both suites
against a simulated board (no hardware).
Output: bench/results/2026-11-13/transitions_<suite>_<time>.json (rewritten after every scenario) and the
traces in bench/results/2026-11-13/captures/.
"""

from __future__ import annotations

import argparse
import csv
import json
import signal
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-12" / "scripts"))
import boost_cal as bc  # noqa: E402

R_OHM = 25.3
PATH_MOHM = 170                 # the margin's path resistance (boost_cal default)
RES_ILIM_MA = 2000              # the resistor suite's limit: a backstop, 33 V / 25.3 ohm = 1.3 A
RES_HEAT_J_10MIN = 15000        # the resistor's budget: 25 W average over 10 minutes (bench/README.md)
POST = 2000                     # trace samples after the trigger (100 ms), 48 before it
NEEDS = ("regulator_ccm_ocp_fs_ma", "regulator_ccm_entry_blind", "regulator_ccm_ilim_slew_ma_ms")


@dataclass
class Ev:
    """At t seconds into the run: kind 'ilim' (a = mA), 'ramp' (a = target mA, b = mA/s), 'ceiling' (a = mV over
    the pack read; the resistor suite: absolute mV).  trace = (trigger, threshold_mV): armed just before it."""
    t: float
    kind: str
    a: int
    b: int = 0
    trace: tuple | None = None


@dataclass
class Sc:
    name: str
    what: str
    ccm: str                    # dcm / auto / forced
    start_ma: int               # the limit at START
    top_ma: int                 # the highest limit in the run: the pack margin and the stop level
    seconds: float
    events: list = field(default_factory=list)
    start_trace: tuple | None = None    # (trigger, threshold_mV) armed before START
    ceiling_mv: int = 0         # resistor suite: the ceiling at START


def pack_suite() -> list[Sc]:
    s = []
    for ma in (1000, 2000, 3000, 4000, 5000):
        s.append(Sc(f"entry_{ma // 1000}A", f"forced CCM entry, limit {ma} mA: the 1 ms open-loop window, then "
                    "the limit climbs at 50 A/s from the current read in CCM", "forced", ma, ma, 4.0,
                    start_trace=("ccm", 0)))
    for rate in (5000, 20000, 50000):
        leg = 4850 / rate
        s.append(Sc(f"ramp_{rate // 1000}Aps", f"auto: limit 150 -> 5000 -> 150 mA at {rate // 1000} A/s through the "
                    "DCM-to-CCM entry (~400 mA) and the exit (<200 mA)", "auto", 150, 5000, 4.0 + 2 * leg,
                    [Ev(1.5, "ramp", 5000, rate, trace=("ccm", 0)), Ev(2.5 + leg, "ramp", 150, rate)]))
    s += [
        Sc("step_1to5A", "forced: limit 1 -> 5 A mid-run (re-seat, climb at 50 A/s)", "forced", 1000, 5000, 4.5,
           [Ev(2.0, "ilim", 5000, trace=("now", 0))]),
        Sc("step_5to1A", "forced: limit 5 -> 1 A mid-run (falls at once)", "forced", 5000, 5000, 4.5,
           [Ev(2.5, "ilim", 1000, trace=("now", 0))]),
        Sc("step_05to5A", "forced: entry at 0.5 A (the 2026-11-12 SW_OCP case), then 0.5 -> 5 A", "forced", 500,
           5000, 4.5, [Ev(2.0, "ilim", 5000)], start_trace=("ccm", 0)),
        Sc("step_5to03A", "forced: limit 5 -> 0.3 A mid-run (CCM held under 1 A, L1's valley negative)", "forced",
           5000, 5000, 4.5, [Ev(2.5, "ilim", 300, trace=("now", 0))]),
        Sc("square_2to4A", "forced: limit 2 <-> 4 A every 300 ms, 5 cycles (a load stepping)", "forced", 2000, 4000,
           6.0, [Ev(2.0 + 0.3 * k, "ilim", 4000 if k % 2 == 0 else 2000, trace=("now", 0) if k == 0 else None)
                 for k in range(10)]),
        Sc("exit_reentry", "auto: 3 A, limit to 150 mA (CCM exits to DCM), back to 3 A (re-entry: window and climb "
           "from the current read)", "auto", 3000, 3000, 6.0,
           [Ev(2.0, "ilim", 150), Ev(3.5, "ilim", 3000, trace=("ccm", 0))]),
        Sc("cc_to_cv", "forced, 4 A limit: the ceiling drops to the pack + 300 mV (the voltage loop takes over from "
           "the limit), then back", "forced", 4000, 4000, 6.0,
           [Ev(2.5, "ceiling", 300, trace=("now", 0)), Ev(4.5, "ceiling", -1)]),
        Sc("cv_to_cc", "forced, 4 A limit: as cc_to_cv, the trace on the way back (the limit retakes as the ceiling "
           "rises)", "forced", 4000, 4000, 6.0,
           [Ev(2.5, "ceiling", 300), Ev(4.5, "ceiling", -1, trace=("now", 0))]),
    ]
    return s


def resistor_suite() -> list[Sc]:
    s = []
    for mv in (12000, 20000, 24000, 26000, 30000, 33000):
        s.append(Sc(f"start_{mv // 1000}V_auto", f"START 5 -> {mv / 1000:g} V at 80 V/s into 25.3 ohm, auto CCM; "
                    "trace on the first mode change", "auto", RES_ILIM_MA, RES_ILIM_MA, 3.0,
                    start_trace=("mode", 0), ceiling_mv=mv))
    s += [
        Sc("start_33V_forced", "START 5 -> 33 V, forced CCM; trace on the first mode change", "forced", RES_ILIM_MA,
           RES_ILIM_MA, 3.0, start_trace=("mode", 0), ceiling_mv=33000),
        Sc("start_33V_dcm", "START 5 -> 33 V, DCM only; trace on the first mode change", "dcm", RES_ILIM_MA,
           RES_ILIM_MA, 3.0, start_trace=("mode", 0), ceiling_mv=33000),
        Sc("up_20to28V", "auto, 20 V: ceiling to 28 V across V_in (buck -> buck-boost -> boost)", "auto",
           RES_ILIM_MA, RES_ILIM_MA, 4.0, [Ev(2.0, "ceiling", 28000, trace=("mode", 0))], ceiling_mv=20000),
        Sc("down_28to20V", "auto, 28 V: ceiling to 20 V across V_in (boost -> buck-boost -> buck)", "auto",
           RES_ILIM_MA, RES_ILIM_MA, 4.0, [Ev(2.0, "ceiling", 20000, trace=("mode", 0))], ceiling_mv=28000),
        Sc("swing_33to12to33V", "auto, 33 V: ceiling to 12 V and back; trace on any 0.5 V error", "auto",
           RES_ILIM_MA, RES_ILIM_MA, 5.0, [Ev(1.5, "ceiling", 12000, trace=("error", 500)), Ev(3.0, "ceiling", 33000)],
           ceiling_mv=33000),
    ]
    # CCM -> CCM downward (2026-11-14, regulator_ccm_down): boost -> band at BOOST -> BB, band -> buck at BB -> BUCK
    s += [
        # boost -> band happens inside buck-boost mode (no mode trigger) and an SWD command takes ~0.4 s, so the
        # trace cannot be timed to a 27 -> 24 V step (13:43-13:44 runs).  From 33 V (BOOST mode) the BOOST -> BB
        # change at ~31 V fires the mode trigger and the band switch at ~25.2 V lands ~75 ms later, inside 100 ms.
        Sc("down_33to24V", "auto, 33 V (boost CCM, BOOST mode): ceiling to 24 V (boost -> band); trace on the "
           "BOOST -> buck-boost mode change", "auto", RES_ILIM_MA, RES_ILIM_MA, 4.0,
           [Ev(2.0, "ceiling", 24000, trace=("mode", 0))], ceiling_mv=33000),
        Sc("down_24to21V", "auto, 24 V (band CCM): ceiling to 21 V (band -> buck); trace on the mode change", "auto",
           RES_ILIM_MA, RES_ILIM_MA, 4.0, [Ev(2.0, "ceiling", 21000, trace=("mode", 0))], ceiling_mv=24000),
        Sc("down_33to12V_forced", "forced CCM, 33 V: ceiling to 12 V (boost -> band -> buck); trace on the mode "
           "change", "forced", RES_ILIM_MA, RES_ILIM_MA, 4.0, [Ev(2.0, "ceiling", 12000, trace=("mode", 0))],
           ceiling_mv=33000),
    ]
    for mv in (24000, 24400, 24800):
        s.append(Sc(f"dwell_{mv}mV", f"auto, {mv / 1000:g} V, at V_in: 4 s on the boundary; trace on a mode change "
                    "(chatter)", "auto", RES_ILIM_MA, RES_ILIM_MA, 4.0, [Ev(0.8, "ceiling", mv, trace=("mode", 0))],
                    ceiling_mv=mv))
    return s


SUITES = {"pack": pack_suite, "resistor": resistor_suite}


# ---------------------------------------------------------------- trace analysis
def trace_stats(path: Path, limit_ma: int | None) -> dict:
    """From a trace CSV: V_out range, the per-cycle current in CCM (i_cmd_ma there is the current the limit reads:
    ID_MON's newest scan), mode and CCM edges, skipping, all from the trigger on."""
    with path.open() as f:
        rows = [r for r in csv.DictReader(f)]
    if not rows or "mode" not in rows[0]:
        return {"samples": len(rows)}
    after = [r for r in rows if float(r["t_ms"]) >= 0.0]
    v = [int(r["v_out_mv"]) for r in after]
    ccm_i = [int(r["i_cmd_ma"]) for r in after if r.get("ccm") == "1"]
    edges, ccm_edges = [], []
    for p, r in zip(rows, rows[1:]):
        if r["mode"] != p["mode"]:
            edges.append({"t_ms": float(r["t_ms"]), "from": p["mode"], "to": r["mode"]})
        if r.get("ccm") != p.get("ccm"):
            ccm_edges.append({"t_ms": float(r["t_ms"]), "ccm": int(r.get("ccm") or 0)})
    out = {"samples": len(rows), "v_out_mv": {"min": min(v), "max": max(v)} if v else None,
           "ccm_i_ma": {"min": min(ccm_i), "max": max(ccm_i)} if ccm_i else None,
           "mode_changes": edges, "ccm_edges": ccm_edges,
           "skipping_samples": sum(r.get("skipping") == "1" for r in after)}
    if ccm_i and limit_ma:
        out["ccm_i_over_limit_ma"] = max(ccm_i) - limit_ma
    if "u_mv" in rows[0]:
        u = [float(r["u_mv"]) for r in after if r.get("ccm") == "1"]
        out["u_mv"] = {"min": min(u), "max": max(u)} if u else None
    return out


def verdict(sc: Sc, suite: str, rec: dict) -> list[str]:
    """Flags for the reader (not pass / fail: a model said what to expect, the trace says what happened)."""
    f = []
    run = rec.get("run", {})
    if run.get("end_fault"):
        f.append(f"FAULT {run['end_fault']}")
    if run.get("abort"):
        f.append(f"ABORT {run['abort']}")
    if rec.get("trace_triggered") is False:
        trig = (sc.start_trace or next((e.trace for e in sc.events if e.trace), ("?",)))[0]
        f.append(f"trace never fired (no '{trig}' event after it was armed)"
                 + (": no chatter" if trig == "mode" and sc.name.startswith("dwell") else ""))
    tr = rec.get("trace_stats") or {}
    if suite == "pack" and tr.get("ccm_i_ma"):
        top = rec.get("trace_limit_ma") or sc.top_ma
        if tr["ccm_i_ma"]["max"] > top + 150:
            f.append(f"CCM current {tr['ccm_i_ma']['max']} mA, over {top} + 150")
        if tr["ccm_i_ma"]["min"] < 50:
            f.append(f"CCM current down to {tr['ccm_i_ma']['min']} mA (ID_MON reads no negative)")
    if suite == "resistor" and tr.get("v_out_mv"):
        if tr["v_out_mv"]["max"] > rec.get("ceiling_max_mv", 0) + 1000:
            f.append(f"V_out {tr['v_out_mv']['max']} mV, over the ceiling + 1 V")
        if sc.name.startswith("dwell") and len(tr.get("mode_changes", [])) > 2:
            f.append(f"{len(tr['mode_changes'])} mode changes in 100 ms (chatter)")
    return f


# ---------------------------------------------------------------- one scenario
def run_scenario(b, sc: Sc, suite: str, interrupted: threading.Event, out_dir: Path, scale: float = 1.0) -> dict:
    rec = {"name": sc.name, "what": sc.what, "ccm": sc.ccm, "started": time.strftime("%F %T")}
    st = b.status()
    if st["state"] != "IDLE":
        raise bc.Stop(f"regulator is {st['state']} before {sc.name}")
    if suite == "pack":
        pack_mv = bc.pack_read(SimpleNamespace(cells=8), b)
        above = bc.above_for(sc.top_ma, PATH_MOHM)
        ceiling = pack_mv + above
        hi = min(8 * bc.CELL_MAX_MV, bc.CEILING_MAX_MV)
        if ceiling > hi:
            raise bc.Stop(f"{sc.name}: ceiling {ceiling} mV (pack {pack_mv} + {above}) over {hi} mV")
        rec.update(pack_read_mv=pack_mv, above_mv=above, ceiling_mv=ceiling)
        abort = lambda r: bc.abort_for_ramp(r, sc.top_ma)   # noqa: E731  follows the limit of the moment
        ceil_of = lambda a: ceiling if a < 0 else pack_mv + a   # noqa: E731  -1: back to the start's
        b.write("regulator_ccm_ocp_ma", bc.OCP_FULL_SCALE, "u16")
    else:
        pack_mv, ceiling = 0, sc.ceiling_mv
        ceil_of = lambda a: a   # noqa: E731
        rec["ceiling_mv"] = ceiling
        abort = bc.abort_for_resistor(max([ceiling] + [e.a for e in sc.events if e.kind == "ceiling"]), R_OHM)
    rec["ceiling_max_mv"] = max([ceiling] + [ceil_of(e.a) for e in sc.events if e.kind == "ceiling"])
    b.write("regulator_ccm_enable", bc.CCM_ENABLE[sc.ccm], "u8")
    b.write("regulator_ccm_ilim_ramp_ma_s", 0, "u16")
    b.write("regulator_ccm_ilim_target_ma", sc.start_ma, "u16")
    n0 = {k: b.read(k, "u32") for k in ("regulator_ccm_entries", "regulator_ccm_exits", "regulator_ccm_kind_switches")
          if b.has(k)}
    stem = f"trace_{suite}_{sc.name}_{time.strftime('%Y%m%d_%H%M%S')}"   # never over an earlier run's
    marks, armed = [], {}

    def during(lock, done, t0):
        for ev in sc.events:
            while not done.is_set() and not interrupted.is_set() and time.monotonic() - t0 < ev.t * scale:
                time.sleep(0.01)
            if done.is_set() or interrupted.is_set():
                return marks
            with lock:
                if ev.trace:
                    res = b.trace_arm(ev.trace[0], POST, ev.trace[1])
                    armed.update(ok=bool(res.get("ok")), result=res.get("result"), at_s=round(time.monotonic() - t0, 3))
                if ev.kind == "ilim":
                    b.write("regulator_ccm_ilim_ma", ev.a, "u16")
                    armed.setdefault("limit_ma", ev.a) if ev.trace else None
                elif ev.kind == "ramp":
                    b.write("regulator_ccm_ilim_target_ma", ev.a, "u16")
                    b.write("regulator_ccm_ilim_ramp_ma_s", ev.b, "u16")
                elif ev.kind == "ceiling":
                    r = b.command("set-voltage", mv=ceil_of(ev.a))
                    if not r.get("ok"):
                        marks.append({"t": round(time.monotonic() - t0, 3), "kind": "ceiling", "refused": r.get("result")})
                        continue
            marks.append({"t": round(time.monotonic() - t0, 3), "kind": ev.kind, "a": ceil_of(ev.a) if ev.kind == "ceiling"
                          else ev.a, "b": ev.b})
            print(f"    {marks[-1]['t']:6.2f} s  {ev.kind} {marks[-1]['a']}" + (f" at {ev.b} mA/s" if ev.kind == "ramp" else "")
                  + ("  (trace armed)" if ev.trace else ""), flush=True)
        return marks

    trace = None
    if sc.start_trace:
        trace = {"trigger": sc.start_trace[0], "threshold_mv": sc.start_trace[1], "post": POST, "at": 0,
                 "file": out_dir / "captures" / f"{stem}.csv"}
    print(f"\n  {time.strftime('%T')}  {sc.name}: {sc.what}\n    limit {sc.start_ma} mA, ceiling {ceiling} mV, "
          f"{sc.ccm}, {sc.seconds * scale:g} s", flush=True)
    run = bc.run_step(b, ceiling, sc.start_ma, sc.seconds * scale, abort, interrupted, during=during, trace=trace)
    rows = run.pop("rows")
    rec["run"], rec["marks"] = run, marks
    rec["counts"] = {k.replace("regulator_", ""): b.read(k, "u32") - n0[k] for k in n0}
    tfile = None
    if run.get("trace") and run["trace"].get("file"):
        tfile = out_dir / "captures" / run["trace"]["file"]
        rec["trace_limit_ma"] = sc.top_ma
    elif armed.get("ok"):
        tfile = out_dir / "captures" / f"{stem}.csv"
        try:
            rec["trace"] = b.trace_read(tfile)
            rec["trace"]["armed_at_s"] = armed.get("at_s")
            tfile = out_dir / "captures" / rec["trace"]["file"]
        except Exception as e:
            rec["trace"] = {"error": f"{type(e).__name__}: {e}"}
            tfile = None
        rec["trace_limit_ma"] = armed.get("limit_ma") or sc.top_ma
    tr_info = (run.get("trace") if run.get("trace") and run["trace"].get("file") else rec.get("trace")) or {}
    if tfile is not None and tr_info.get("trigger_sample") is None:
        # ARMED and never fired: the ring holds the last 102 ms before the stop, not samples after an event
        rec["trace_triggered"] = False
        tfile = None
    if tfile is not None and tfile.exists():
        rec["trace_triggered"] = True
        try:
            rec["trace_stats"] = trace_stats(tfile, rec.get("trace_limit_ma") if suite == "pack" else None)
        except Exception as e:
            rec["trace_stats"] = {"error": f"{type(e).__name__}: {e}"}
    ok = [r for r in rows if r["state"] == "RUNNING"]
    if ok:
        def mx(k):
            v = [r[k] for r in ok if r.get(k) is not None]
            return max(v) if v else None
        def mn(k):
            v = [r[k] for r in ok if r.get(k) is not None]
            return min(v) if v else None
        rec["logged"] = {"n": len(ok), "id_lim_ma_max": mx("id_lim_ma"), "id_cal_ma_max": mx("id_cal_ma"),
                         "i_bound_ma_max": mx("i_bound_ma"), "v_out_mv": [mn("v_out_mv"), mx("v_out_mv")],
                         "modes": sorted({r["mode"] for r in ok if r.get("mode")})}
    rec["rows"] = rows
    rec["flags"] = verdict(sc, suite, rec)
    ts = rec.get("trace_stats") or {}
    print(f"    {'ok' if run.get('ok') else 'NOT OK'}  logged ID_MON max {rec.get('logged', {}).get('id_lim_ma_max')} mA, "
          f"V_out {rec.get('logged', {}).get('v_out_mv')} mV, CCM entries/exits {rec['counts']}"
          + (f"\n    trace: V_out {ts.get('v_out_mv')}, CCM current {ts.get('ccm_i_ma')}, mode changes "
             f"{[(e['t_ms'], e['to']) for e in ts.get('mode_changes', [])][:6]}" if ts else "")
          + (f"\n    FLAGS: {'; '.join(rec['flags'])}" if rec["flags"] else ""), flush=True)
    if run.get("start") != "OK":
        raise bc.Stop(f"{sc.name}: START refused: {run.get('start')} ({run.get('explanation')})")
    if run.get("abort"):
        raise bc.Stop(f"{sc.name}: stopped the run: {run['abort']}")
    if run.get("end_fault") or run.get("end_state") != "IDLE":
        raise bc.Stop(f"{sc.name}: FAULT or not IDLE: {run.get('end_state')} {run.get('end_fault')} (not cleared; "
                      "read `bu regulator status`)")
    return rec


def vbus_free(b) -> dict:
    """boost_cal's resistor check: a START at 5 V must run (a pack would make the firmware refuse it)."""
    b.write("regulator_ccm_ilim_ma", 500, "u16")
    probe5 = b.command("start", mv=5000)
    if b.status()["state"] == "RUNNING":
        b.command("stop")
    pack_mv = b.battery()["pack_mv"]
    if not probe5.get("ok") or pack_mv > 1500:
        raise bc.Stop(f"START at 5 V gave {probe5.get('result')} with VBUS at {pack_mv} mV: something holds VBUS up "
                      "(a pack?).  The resistor suite needs VBUS free.")
    return {"start_5v": probe5.get("result"), "pack_mv": pack_mv}


def execute(suite: str, scs: list[Sc], b, out_dir: Path, interrupted: threading.Event, idle_s: float,
            scale: float = 1.0) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "captures").mkdir(parents=True, exist_ok=True)
    out = out_dir / f"transitions_{suite}_{time.strftime('%Y%m%d_%H%M%S')}.json"
    result = {"suite": suite, "started": time.strftime("%F %T"), "scenarios": []}
    def save():
        out.write_text(json.dumps(result, indent=1))
    st = b.status()
    if st["state"] != "IDLE":
        raise bc.Stop(f"regulator is {st['state']}, not IDLE: stop it or clear the fault first")
    missing = [s for s in NEEDS if not b.has(s)]
    if missing:
        result["stopped"] = f"needs the 2026-11-13 BatteryNext build (missing {missing})"
        print(f"\n  STOPPED: {result['stopped']}", flush=True)
        save()
        return result
    keep = {k: b.read(k, t) for k, t in (("regulator_ccm_enable", "u8"), ("regulator_ccm_ocp_ma", "u16"),
                                         ("regulator_ccm_ilim_ma", "u16"))}
    result["firmware"] = {k: b.read(k, t) for k, t in (("regulator_ccm_entry_blind", "u8"),
                                                        ("regulator_ccm_ilim_slew_ma_ms", "u16"),
                                                        ("regulator_id_gain_permille", "i16"),
                                                        ("regulator_id_offset_ma", "i16"))}
    try:
        if suite == "resistor":
            result["vbus_check"] = vbus_free(b)
            print(f"  VBUS free ({result['vbus_check']['pack_mv']} mV through the closed switch)", flush=True)
        for n, sc in enumerate(scs):
            if interrupted.is_set():
                break
            if n:
                time.sleep(idle_s * scale)
            rec = {"name": sc.name}
            result["scenarios"].append(rec)
            try:
                rec.update(run_scenario(b, sc, suite, interrupted, out_dir, scale))
            finally:
                save()
    except bc.Stop as e:
        result["stopped"] = str(e)
        print(f"\n  STOPPED: {e}", flush=True)
    finally:
        try:
            if b.status()["state"] == "RUNNING":
                b.command("stop")
        finally:
            b.write("regulator_ccm_ilim_ramp_ma_s", 0, "u16")
            for k, v in keep.items():
                b.write(k, v, "u8" if k == "regulator_ccm_enable" else "u16")
            result["interrupted"] = interrupted.is_set()
            result["ended"] = time.strftime("%F %T")
            save()
    print(f"\n  {time.strftime('%T')}  DONE -> {out}")
    for r in result["scenarios"]:
        print(f"    {r['name']:20s} {'ok' if r.get('run', {}).get('ok') else 'NOT OK':6s} "
              + ("; ".join(r.get("flags", [])) or "no flags"))
    return result


def plan_text(suite: str, scs: list[Sc], idle_s: float) -> str:
    total = sum(s.seconds for s in scs) + idle_s * (len(scs) - 1)
    lines = [f"{suite.upper()} suite: {len(scs)} scenarios, about {total / 60:.1f} min with {idle_s:g} s between."]
    if suite == "pack":
        lines += ["  THE PACK ON VBUS, THE 25.3 OHM OFF.  THIS TOUCHES THE PACK: each scenario reads it (switch closed "
                  "30 ms) and STARTs.",
                  "  Ceiling: the pack read + 1.6 x top x 170 mOhm + 200 mV (300..1500 mV).  SW_OCP at ID_MON's full "
                  "scale (~5.3 A).",
                  "  Stop on a fault, on either current estimate over max(2 x, + 1 A) the limit of the moment (<= 7.5 A) "
                  "for 3 samples, or Ctrl-C.",
                  f"  Time at 4-5 A: about {sum(s.seconds for s in scs if s.top_ma >= 4000):.0f} s in total "
                  "(160 W into the pack at 5 A; board temperature not logged)."]
    else:
        heat = sum(min(max([s.ceiling_mv] + [e.a for e in s.events if e.kind == 'ceiling']), 34000) ** 2 / 1e6 / R_OHM
                   * s.seconds for s in scs)
        lines += ["  THE 25.3 OHM ON VBUS, NO PACK.  A START at 5 V first checks VBUS is free.",
                  f"  The resistor takes about {heat / 1000:.1f} kJ over the suite (budget {RES_HEAT_J_10MIN / 1000:g} kJ "
                  "per 10 min); the limit (2 A) is a backstop."]
    for s in scs:
        ev = ", ".join(f"{e.t:g} s {e.kind} {e.a}" + (f"@{e.b}" if e.kind == "ramp" else "") +
                       (" [trace]" if e.trace else "") for e in s.events)
        lines.append(f"  {s.name:20s} {s.seconds:4.1f} s  {s.what}" + (f"  [{ev}]" if ev else "")
                     + ("  [trace from START]" if s.start_trace else ""))
    return "\n".join(lines)


# ---------------------------------------------------------------- --selftest
class FakeBoard(bc.FakeBoard):
    """boost_cal's simulated board with the 2026-11-13 symbols and SET_VOLTAGE while running."""

    def __init__(self, **kw):
        super().__init__(**kw)
        self.mem.update({"regulator_ccm_entry_blind": 20, "regulator_ccm_ilim_slew_ma_ms": 50,
                         "regulator_id_gain_permille": 991, "regulator_id_offset_ma": 4})
        self.traces, self.fire = [], True

    def command(self, name, mv=None):
        if name == "set-voltage":
            self.log.append((name, mv))
            if not 5000 <= mv <= self.ceiling_max:
                return {"ok": False, "result": "OUT_OF_RANGE"}
            self.ceiling = mv
            return {"ok": True, "result": "OK"}
        return super().command(name, mv)

    def trace_read(self, dst):
        dst.parent.mkdir(parents=True, exist_ok=True)
        v = self.ceiling or 31200
        dst.write_text("t_ms,v_out_mv,i_cmd_ma,i_max_ma,t_on_ns,mode,skipping,switch_on,rectifier,at_limit,pulse_open,"
                       "trigger,ccm,ilim,v_cmd_mv,u_mv\n"
                       f"-0.05,{v - 200},600,0,1200,BUCK_BOOST,0,1,0,0,0,0,0,0,{v - 200},0\n"
                       f"0.00,{v - 100},610,0,1200,BUCK_BOOST,0,1,0,0,0,1,1,1,{v - 100},-440\n"
                       f"0.05,{v},900,0,1210,BOOST,0,1,0,0,0,0,1,1,{v - 50},-440\n")
        self.traces.append(dst.name)
        return {"ok": True, "state": "DONE" if self.fire else "ARMED", "samples": 3,
                "trigger_sample": 1 if self.fire else None, "file": dst.name}


def selftest() -> int:
    import shutil
    import tempfile
    fails = []
    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            fails.append(what)
    tmp = Path(tempfile.mkdtemp(prefix="transitions_selftest_"))
    ev = threading.Event()
    scale = 0.06
    b = FakeBoard(pack_mv=31200, r_path=0.04, id_gain=1.0)
    r = execute("pack", pack_suite(), b, tmp, ev, 0.0, scale)
    names = [s["name"] for s in r["scenarios"]]
    check(not r.get("stopped") and names == [s.name for s in pack_suite()], f"pack: every scenario ran ({r.get('stopped')})")
    check(all(s.get("run", {}).get("ok") for s in r["scenarios"]), "pack: every run ok")
    starts = [c for c in b.log if c[0] == "start"]
    check(sum(1 for c in starts if c[1] == 5000) == len(pack_suite()), "pack: one pack read per scenario")
    check(all(c[1] <= 31200 + 1500 for c in starts), "pack: every ceiling within the pack + 1.5 V")
    sv = [c for c in b.log if c[0] == "set-voltage"]
    check(sv and min(c[1] for c in sv) == 31500 and max(c[1] for c in sv) > 31500, f"pack: cc/cv ceiling steps {sv}")
    check(b.mem["regulator_ccm_ilim_ramp_ma_s"] == 0 and b.mem["regulator_ccm_enable"] == 1
          and b.mem["regulator_ccm_ocp_ma"] == 5000, "pack: ramp off, CCM enable and SW_OCP restored")
    check(any("step_1to5A" in t for t in b.traces) and any("entry_5A" in f.name for f in (tmp / "captures").iterdir()),
          "pack: traces read (event-armed and START-armed)")
    s15 = next(s for s in r["scenarios"] if s["name"] == "step_1to5A")
    check(s15.get("trace_stats", {}).get("ccm_i_ma") == {"min": 610, "max": 900} and s15.get("trace_limit_ma") == 5000,
          f"pack: trace stats {s15.get('trace_stats')}")

    b = FakeBoard(pack_mv=0)
    r = execute("resistor", resistor_suite(), b, tmp, ev, 0.0, scale)
    check(not r.get("stopped") and len(r["scenarios"]) == len(resistor_suite()), f"resistor: every scenario ran ({r.get('stopped')})")
    check(r.get("vbus_check", {}).get("start_5v") == "OK", "resistor: VBUS-free check first")
    check(("set-voltage", 28000) in b.log and ("set-voltage", 12000) in b.log, "resistor: ceiling steps sent")

    b = FakeBoard(pack_mv=0)
    b.fire = False
    r = execute("resistor", [s for s in resistor_suite() if s.name == "dwell_24400mV"], b, tmp, ev, 0.0, scale)
    s0 = r["scenarios"][0]
    check(s0.get("trace_triggered") is False and "trace_stats" not in s0 and any("no chatter" in x for x in s0["flags"]),
          f"resistor: an untriggered trace is flagged, not analysed ({s0.get('flags')})")

    b = FakeBoard(pack_mv=31200)
    r = execute("resistor", resistor_suite(), b, tmp, ev, 0.0, scale)
    check("VBUS" in r.get("stopped", "") and not r["scenarios"], "resistor with a pack on VBUS: stopped before any scenario")

    b = FakeBoard(pack_mv=31200)
    del b.mem["regulator_ccm_entry_blind"]
    r = execute("pack", pack_suite(), b, tmp, ev, 0.0, scale)
    check("2026-11-13" in r.get("stopped", "") and not any(c[0] == "start" for c in b.log),
          "pack on an older build: refused before any START")

    shutil.rmtree(tmp, ignore_errors=True)
    print("selftest:", "all passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


# ---------------------------------------------------------------- main
def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("suite", nargs="?", choices=sorted(SUITES))
    p.add_argument("--go", action="store_true", help="run it (without: print the plan)")
    p.add_argument("--only", help="comma-separated scenario names")
    p.add_argument("--list", action="store_true")
    p.add_argument("--idle", type=float, help="seconds between scenarios (default: pack 6, resistor 3)")
    p.add_argument("--selftest", action="store_true")
    a = p.parse_args()
    if a.selftest:
        return selftest()
    if not a.suite:
        p.error("name a suite: pack or resistor")
    scs = SUITES[a.suite]()
    if a.only:
        want = a.only.split(",")
        bad = [w for w in want if w not in {s.name for s in scs}]
        if bad:
            p.error(f"no such scenario {bad}; --list shows them")
        scs = [s for s in scs if s.name in want]
    idle = a.idle if a.idle is not None else (6.0 if a.suite == "pack" else 3.0)
    print(plan_text(a.suite, scs, idle))
    if a.list or not a.go:
        if not a.list:
            print("\n(not run: add --go)")
        return 0
    interrupted = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: interrupted.set())
    signal.signal(signal.SIGTERM, lambda *_: interrupted.set())
    from bringup.config import REPO_ROOT
    r = execute(a.suite, scs, bc.Board(), Path(REPO_ROOT) / "bench/results/2026-11-13", interrupted, idle)
    return 1 if r.get("stopped") or r.get("interrupted") else 0


if __name__ == "__main__":
    sys.exit(main())
