"""Regression check after the CCM-exit seed fix (bench 2026-11-16, branch claude/regulator-epr-pps, commit 001f42e).

Copy of 2026-11-15/scripts/pd_final.py; results land in 2026-11-16/.  Bench: G474 source (build/EprPps), 24 V in;
VBUS into the 25.3 ohm only (no QT Py); the NUCLEO-G431 sink on the Type-C cable (CC only).  NO PACK: the PD build
drives OUTPUT_DIS.  Only fixed 5 / 9 / 15 V contracts are requested.  Scope: CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out.

Phases (one per run, --phase): base, walk, hold, ripple, detach as on 2026-11-15, and
  pulse   15 V on the resistor (0.59 A, DCM): the output switch opens for 5 ms (switch-pulse), traced
The 25.3 ohm alone stays in DCM, so the new CCM exit seed is not exercised here.
Every phase stops at a regulator fault (never cleared).  Without --go it prints the plan only.

Run from the repository root:
  uv run --quiet --project tools/bringup python bench/results/2026-11-16/scripts/pd_seedfix.py --phase base --go
"""

from __future__ import annotations

import argparse
import gzip
import json
import shutil
import signal
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-12" / "scripts"))
sys.path.insert(0, str(HERE.parents[1] / "2026-11-13" / "scripts"))

R_OHM = 25.3
PDO_MA = 3000
CYCLES, HOLD_S, RESETS = 2, 30.0, 3
I_ABORT_MA = 2000               # 15 V / 25.3 ohm + 7 W / 15 V = 1.06 A
WALK = [9000, 15000, 9000, 5000, 15000, 5000]
LOAD_MW = [500, 3500, 7000, 3500, 500]
PHASES = ("base", "walk", "hold", "ripple", "load", "ccm", "reset", "detach", "pulse")
PLAN = {
    "base": "status reads; mailbox START 12 V and SET_VOLTAGE 12 V, both expected to be refused (PD_OWNS_VBUS)",
    "walk": f"{CYCLES} x contracts 5 -> 9 -> 15 -> 9 -> 5 -> 15 -> 5 V (about {CYCLES * len(WALK) * 1.5:.0f} s), "
            f"scope on 5 -> 15 V and 15 -> 5 V in the first cycle",
    "hold": f"15 V for {HOLD_S:.0f} s (9 W in the 25.3 ohm)",
    "ripple": "5 V and 15 V: one forced scope record each (24 ms at 12.5 MSa/s, CH4 at 0.5 V/div), with the status "
              "reads of the same 10 s",
    "load": "15 V: QT Py 0.5 / 3.5 / 7 / 3.5 / 0.5 W for 3 s each, then 7 W with switch-pulse 5 ms, traced",
    "ccm": "15 V: forced CCM over SWD, entry traced; 7 W for 3 s; 15 -> 9 -> 5 -> 15 V in CCM (scope on 15 -> 9 V); "
           "regulator_ccm_enable restored, exit traced",
    "reset": f"{RESETS} x G474 reset at 15 V; the sink re-attaches and asks for 15 V again (scope on the first)",
    "pulse": "15 V on the 25.3 ohm (DCM): switch-pulse 5 ms, traced on the switch",
    "detach": "sink Rd off at 15 V, forced scope record of TP2 falling (vSafe0V); Rd both, forced record of the re-attach",
}


class Bench:
    def __init__(self):
        from bringup import load, pd, probe, regulator, sink
        from bringup.config import REPO_ROOT, load_config
        self.load, self.pd_, self.probe, self.reg, self.sink_ = load, pd, probe, regulator, sink
        self.cfg, self.root = load_config(), Path(REPO_ROOT)
        self.lock = threading.Lock()          # one SWD user at a time
        self.abort = threading.Event()
        self.abort_why = None
        self.rows: list[dict] = []
        self.t0 = time.monotonic()

    def t(self) -> float:
        return round(time.monotonic() - self.t0, 2)

    def status(self) -> dict:
        with self.lock:
            return self.reg.status(self.cfg)

    def command(self, name: str, **kw) -> dict:
        with self.lock:
            return self.reg.command(self.cfg, name, 5.0, **kw)

    def read(self, sym: str, dtype: str) -> int:
        with self.lock:
            return self.probe.mem_read(self.cfg, sym, dtype=dtype, count=1)["values"][0]

    def write(self, sym: str, value: int, dtype: str) -> None:
        with self.lock:
            self.probe.mem_write(self.cfg, sym, str(value), dtype)

    def pd(self) -> dict:
        with self.lock:
            s = self.pd_.status(self.cfg)
        return {k: s.get(k) for k in ("contract", "last_rdo", "counters", "diagnosis", "regulator_state", "v_out_mv")}

    def sink(self, cmd: str, until: str | None = None, timeout: float = 3.0, wait: float = 0.0) -> dict:
        r = self.sink_.exchange(self.cfg, cmd, timeout=timeout, until_event=until, wait=wait)
        return {k: r.get(k) for k in ("ok", "command", "reply", "events", "seconds", "error")}

    def sink_status(self) -> dict:
        for _ in range(3):
            r = self.sink("status")
            st = next((x for x in r.get("reply") or [] if x.get("kind") == "STATUS"), None)
            if st:
                return st
        return {"error": r}

    def row(self) -> dict:
        st = self.status()
        c, a = st.get("control", {}), st.get("adc", {})
        r = {"t": self.t(), "state": st["state"], "fault": st["fault_source"], "mode": c.get("mode"),
             "ccm": c.get("ccm_active"), "kind": c.get("ccm_kind"), "target_mv": c.get("target_mv"),
             "commanded_mv": c.get("commanded_mv"), "v_out_mv": a.get("v_out_mv"), "v_in_mv": a.get("v_in_mv"),
             "id_cal_ma": c.get("ccm_i_avg_ma"), "i_in_ma": c.get("i_in_filt_ma"), "on_ns": c.get("on_time_ns"),
             "skip": c.get("pulses_skipped"), "switch": st.get("lines", {}).get("OUTPUT_EN_PC8"),
             "dis": st.get("lines", {}).get("OUTPUT_DIS_PC9"), "exits": c.get("ccm_exits"),
             "entries": c.get("ccm_entries"), "exit_reason": c.get("ccm_exit_reason"), "lim": c.get("ccm_limiting")}
        self.rows.append(r)
        return r

    def logger(self, done: threading.Event) -> None:
        over = 0
        while not done.is_set():
            try:
                r = self.row()
            except Exception as e:      # a reset or a busy probe: note it and carry on
                self.rows.append({"t": self.t(), "error": str(e)[:120]})
                time.sleep(0.3)
                continue
            over = over + 1 if (r["id_cal_ma"] or 0) > I_ABORT_MA else 0
            if r["state"] == "FAULT" or over >= 3:
                self.abort_why = f"fault {r['fault']}" if r["state"] == "FAULT" else f"ID_MON {r['id_cal_ma']} mA"
                self.abort.set()
                return
            time.sleep(0.05)

    def req(self, mv: int, ma: int = PDO_MA) -> dict:
        """One fixed request; the sink's contract event, then the source's view."""
        t0 = time.monotonic()
        r = self.sink(f"req {mv} {ma}", until="contract", timeout=3.0)
        dt = round(time.monotonic() - t0, 3)
        time.sleep(0.4)
        p = self.pd()
        # a status read's v_out_mv is one ADC sample (2026-11-15: 14.3-15.6 V singles at 15 V, scope +-25 mV;
        # 2026-11-16 16:59 one read of 16747 mV with no fault), so the check takes the median of five
        rows = [self.row() for _ in range(5)]
        st = dict(rows[-1])
        st["v_out_mv"] = sorted(r["v_out_mv"] for r in rows)[2]
        st["v_out_reads"] = [r["v_out_mv"] for r in rows]
        got = (p.get("contract") or {}).get("mv")
        return {"t": self.t(), "req_mv": mv, "sink_ok": r.get("ok"), "sink_s": dt,
                "sink_events": [e.get("text") or e for e in (r.get("events") or [])][-4:],
                "contract_mv": got, "transition_ms": (p.get("contract") or {}).get("last_transition_ms"),
                "rdo": (p.get("last_rdo") or {}).get("result"), "counters": p.get("counters"),
                "v_out_mv": st["v_out_mv"], "v_out_reads": st["v_out_reads"], "state": st["state"], "mode": st["mode"], "ccm": st["ccm"],
                "ok": bool(r.get("ok")) and got == mv and st["state"] == "RUNNING" and abs(st["v_out_mv"] - mv) < 0.05 * mv}

    def trace_arm(self, trigger: str, post: int = 2000, thr: int = 0) -> str:
        return self.command("trace-arm", arg=self.reg.trace_arg(trigger, post, thr)).get("result")

    def trace_read(self, name: str) -> dict:
        import transitions as tr
        with self.lock:
            r = self.reg.trace_read(self.cfg)
        dst = D / "traces" / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.root / r["file"], dst)
        out = {k: r.get(k) for k in ("state", "trigger", "samples", "trigger_sample", "pulse_us")}
        out["file"] = dst.name
        try:
            out["stats"] = tr.trace_stats(dst, None)
        except Exception as e:
            out["stats_error"] = str(e)[:120]
        return out


class Tap:
    """The scope, single-shot on V_out (CH4) at 20 ms/div, 300k points: [-100, +140] ms about the trigger."""

    def __init__(self, bench: Bench, stamp: str):
        from bringup.scope import Scope
        self.b, self.stamp = bench, stamp
        self.sources = ["CHAN1", "CHAN2", "CHAN3", "CHAN4"]
        self.s = Scope(bench.cfg).__enter__()
        s = self.s
        s.channel(1, display=True, coupling="DC", probe=10, scale=5, offset=-17, bwl="OFF")     # SW1
        s.channel(3, display=True, coupling="DC", probe=10, scale=5, offset=-17, bwl="OFF")     # SW2
        s.channel(2, display=True, coupling="DC", probe=10, scale=2, offset=-24, bwl="20M")     # V_in
        s.channel(4, display=True, coupling="DC", probe=10, scale=2, offset=-8, bwl="20M")      # V_out, 0..16 V
        s.write(":RUN"); s.write(":ACQuire:MDEPth 300000")
        s.timebase(scale=20e-3, offset=20e-3)
        s.trigger(source="CHAN4", slope="NEG", level=10.0, sweep="SINGLE", coupling="HFR")

    def arm(self, slope: str, level: float) -> str:
        s = self.s
        s.write(f":TRIGger:EDGe:SLOPe {'POSitive' if slope == 'POS' else 'NEGative'}")
        s.write(f":TRIGger:EDGe:LEVel {level:g}")
        s.write(":SINGle"); time.sleep(0.5)
        return s.query(":TRIGger:STATus?")

    def grab(self, name: str, timeout: float = 3.0) -> dict:
        s = self.s
        w = s.wait_trigger(timeout=timeout, arm=False)
        if not w.get("triggered"):
            s.write(":RUN")
            return {"triggered": False, "last_status": w.get("last_status")}
        c = s.capture(self.sources, mode="raw")
        dst = D / "captures" / f"pd_seedfix_{self.stamp}_{name}.csv.gz"
        dst.parent.mkdir(parents=True, exist_ok=True)
        with open(self.b.root / c["file"], "rb") as fi, gzip.open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo)
        (self.b.root / c["file"]).unlink()
        return {"triggered": True, "file": dst.name, "sample_rate_sa_s": c.get("sample_rate_sa_s"),
                "warnings": c.get("warnings", [])}

    def close(self) -> None:
        try:
            self.s.write(":TRIGger:SWEep AUTO"); self.s.write(":TRIGger:COUPling DC"); self.s.write(":RUN")
        finally:
            self.s.__exit__(None, None, None)


def stats(rows: list[dict], key: str) -> dict | None:
    v = [r[key] for r in rows if r.get(key) is not None]
    return {"n": len(v), "min": min(v), "max": max(v), "mean": round(sum(v) / len(v), 1)} if v else None


class Abort(Exception):
    pass


def check(b: Bench) -> None:
    if b.abort.is_set():
        raise Abort(b.abort_why)


def ensure(b: Bench, mv: int, res: dict) -> None:
    r = b.req(mv)
    res.setdefault("setup", []).append(r)
    if not r["ok"]:
        raise Abort(f"could not get the {mv} mV contract: {r}")


def ph_base(b: Bench, tap, res: dict) -> None:
    res["sink"], res["pd"], res["regulator"] = b.sink_status(), b.pd(), b.row()
    try:
        res["load"] = b.load.exchange(b.cfg, "status").get("reply")
    except Exception as e:
        res["load"] = f"no QT Py: {str(e)[:80]}"
    res["mailbox"] = {}
    for name in ("start", "set-voltage"):
        r = b.command(name, mv=12000)
        after = b.row()
        res["mailbox"][name] = {"result": r.get("result"), "ok": r.get("ok"), "target_mv_after": after["target_mv"],
                                "v_out_mv_after": after["v_out_mv"]}
    res["pass"] = all(m["result"] == "PD_OWNS_VBUS" and m["target_mv_after"] == 5000 for m in res["mailbox"].values())


def ph_walk(b: Bench, tap, res: dict) -> None:
    ensure(b, 5000, res)
    res["steps"], prev = [], 5000
    for cyc in range(CYCLES):
        for mv in WALK:
            check(b)
            cap = None
            if cyc == 0 and (prev, mv) in ((5000, 15000), (15000, 5000)) and tap:
                armed = tap.arm("POS" if mv > prev else "NEG", 10.0)
                cap = {"armed": armed}
            r = b.req(mv)
            r.update(cycle=cyc, from_mv=prev)
            if cap is not None:
                cap.update(tap.grab(f"{prev // 1000}to{mv // 1000}V"))
                r["capture"] = cap
            res["steps"].append(r)
            print(f"  {time.strftime('%T')} {prev:5d} -> {mv:5d} mV  ok {r['ok']}  PS_RDY {r['transition_ms']} ms  "
                  f"V_out {r['v_out_mv']} mV  {r['mode']}", flush=True)
            if not r["ok"]:
                raise Abort(f"step {prev} -> {mv} failed: {r}")
            prev = mv
            time.sleep(0.6)
    res["pass"] = all(s["ok"] for s in res["steps"])


def ph_hold(b: Bench, tap, res: dict) -> None:
    ensure(b, 15000, res)
    n0 = len(b.rows)
    t_end = time.monotonic() + HOLD_S
    while time.monotonic() < t_end:
        check(b)
        time.sleep(0.2)
    rows = b.rows[n0:]
    res["v_out_mv"], res["id_cal_ma"], res["i_in_ma"], res["v_in_mv"] = (stats(rows, k) for k in
                                                                       ("v_out_mv", "id_cal_ma", "i_in_ma", "v_in_mv"))
    res["states"] = sorted({str(r.get("state")) for r in rows if "state" in r})
    res["pd"] = b.pd()
    res["pass"] = res["states"] == ["RUNNING"] and abs(res["v_out_mv"]["mean"] - 15000) < 300


def ph_ripple(b: Bench, tap, res: dict) -> None:
    res["points"] = []
    s = tap.s
    s.write(":TIMebase:MAIN:SCALe 0.002"); s.write(":TIMebase:MAIN:OFFSet 0"); s.write(":CHANnel4:SCALe 0.5")
    try:
        for mv in (5000, 15000):
            ensure(b, mv, res)
            check(b)
            s.write(f":CHANnel4:OFFSet {-mv / 1000:g}")
            n0 = len(b.rows)
            time.sleep(5.0)
            s.write(":SINGle"); time.sleep(0.5); s.write(":TFORce")
            cap = tap.grab(f"ripple_{mv // 1000}V", timeout=4.0)
            time.sleep(5.0)
            v = [r["v_out_mv"] for r in b.rows[n0:] if r.get("commanded_mv") == mv and r.get("v_out_mv")]
            res["points"].append({"mv": mv, "capture": cap, "status_v_out_mv": stats(b.rows[n0:], "v_out_mv"),
                                  "status_reads_off_by_300mV": sum(abs(x - mv) > 300 for x in v), "status_reads": len(v)})
    finally:
        s.write(":CHANnel4:SCALe 2"); s.write(":CHANnel4:OFFSet -8")
        s.write(":TIMebase:MAIN:SCALe 0.02"); s.write(":TIMebase:MAIN:OFFSet 0.02")
    res["pass"] = all(pt["capture"].get("triggered") for pt in res["points"])


def load_run(b: Bench, mw: list[int], seconds: float) -> dict:
    r = b.load.run_load(b.cfg, mw, seconds, limit_ma=PDO_MA, contract_mv=15000, vin_min_mv=12750, clear=True)   # 0.85 x, as on 2026-11-06: the QT Py reads 13.55 V at 15 V
    return {k: v for k, v in r.items() if k in ("ok", "steps", "last", "fault", "frames", "stopped_early")}


def ph_load(b: Bench, tap, res: dict) -> None:
    ensure(b, 15000, res)
    n0 = len(b.rows)
    res["staircase"] = load_run(b, LOAD_MW, 3.0)
    res["staircase_v_out_mv"] = stats(b.rows[n0:], "v_out_mv")
    res["staircase_id_cal_ma"] = stats(b.rows[n0:], "id_cal_ma")
    check(b)
    # a hard load drop and connect: the output switch opens for 5 ms while the QT Py holds 7 W
    res["pulse"] = {"arm": b.trace_arm("switch")}

    def pulse():
        time.sleep(4.0)                 # the load's +5 V/s ramp to 7 W takes about 1.7 s
        res["pulse"]["cmd"] = b.command("switch-pulse", arg=b.reg.pulse_us(5.0)).get("result")

    th = threading.Thread(target=pulse); th.start()
    res["pulse"]["load"] = load_run(b, [7000], 6.0)
    th.join()
    time.sleep(0.3)
    res["pulse"]["trace"] = b.trace_read(f"pd_final_{res['stamp']}_pulse5ms.csv")
    res["pulse"]["after"] = b.row()
    res["pd"] = b.pd()
    res["pass"] = (not b.abort.is_set()) and res["pulse"]["after"]["state"] == "RUNNING"


def ph_ccm(b: Bench, tap, res: dict) -> None:
    ensure(b, 15000, res)
    keep = b.read("regulator_ccm_enable", "u8")
    res["ccm_enable_before"] = keep
    try:
        res["entry"] = {"arm": b.trace_arm("ccm"), "before": b.row()}
        b.write("regulator_ccm_enable", 2, "u8")
        time.sleep(0.5)
        res["entry"]["after"] = b.row()
        res["entry"]["trace"] = b.trace_read(f"pd_final_{res['stamp']}_ccm_entry.csv")
        check(b)
        n0 = len(b.rows)
        time.sleep(3.0)
        res["ccm_15V"] = {k: stats(b.rows[n0:], k) for k in ("v_out_mv", "id_cal_ma")}
        res["ccm_15V"]["ccm_all"] = all(r.get("ccm") == 1 for r in b.rows[n0:] if "ccm" in r)
        n0 = len(b.rows)
        res["ccm_load"] = load_run(b, [7000], 4.0)
        res["ccm_load"]["v_out_mv"] = stats(b.rows[n0:], "v_out_mv")
        res["ccm_load"]["ccm_all"] = all(r.get("ccm") == 1 for r in b.rows[n0:] if "ccm" in r)
        check(b)
        res["down"], prev = [], 15000
        exits0 = b.row()["exits"]
        for mv in (9000, 5000, 15000):
            cap = {"armed": tap.arm("NEG", 12.0)} if (tap and (prev, mv) == (15000, 9000)) else None
            n0 = len(b.rows)
            r = b.req(mv)
            time.sleep(1.0)
            seg = [x for x in b.rows[n0:] if "ccm" in x]
            r.update(from_mv=prev, ccm_all=all(x["ccm"] == 1 for x in seg), ccm_samples=len(seg),
                     exits_delta=b.row()["exits"] - exits0, exit_reason=b.rows[-1]["exit_reason"])
            if cap is not None:
                cap.update(tap.grab("ccm_15to9V"))
                r["capture"] = cap
            res["down"].append(r)
            print(f"  {time.strftime('%T')} CCM {prev:5d} -> {mv:5d} mV  ok {r['ok']}  CCM held {r['ccm_all']} "
                  f"({r['ccm_samples']} reads)  exits +{r['exits_delta']}  V_out {r['v_out_mv']} mV", flush=True)
            check(b)
            prev = mv
        res["exit"] = {"arm": b.trace_arm("ccm")}
    finally:
        b.write("regulator_ccm_enable", keep, "u8")
    time.sleep(0.8)
    res["exit"]["after"] = b.row()
    res["exit"]["trace"] = b.trace_read(f"pd_final_{res['stamp']}_ccm_exit.csv")
    res["ccm_enable_after"] = b.read("regulator_ccm_enable", "u8")
    res["pd"] = b.pd()
    res["pass"] = (res["entry"]["after"]["ccm"] == 1 and all(d["ok"] and d["ccm_all"] and d["exits_delta"] == 0
                                                             for d in res["down"])
                   and res["ccm_enable_after"] == keep and not b.abort.is_set())


def wait_contract(b: Bench, timeout: float = 8.0) -> dict:
    t_end = time.monotonic() + timeout
    st = {}
    while time.monotonic() < t_end:
        st = b.sink_status()
        if st.get("contract_mv"):
            break
        time.sleep(0.2)
    return st


def ph_reset(b: Bench, tap, res: dict) -> None:
    """The sink keeps its 15 V target, so after the source's reset it asks for 15 V again by itself (seen 20:44)."""
    res["cycles"] = []
    ensure(b, 15000, res)
    hr0 = b.sink_status().get("hard_resets")
    if tap:
        tap.s.write(":TIMebase:MAIN:SCALe 0.2"); tap.s.write(":TIMebase:MAIN:OFFSet 0.6")   # [-0.6, +1.8] s
    try:
        for k in range(RESETS):
            check(b)
            # TP2 (CH4) is ahead of the output switch and did not fall through 10 V at 20:45, so the record is
            # forced just before the reset; the QT Py's VIN tap (VBUS, after the switch) is polled through it.
            cap = None
            if tap and k == 0:
                cap = {"armed": tap.arm("NEG", 10.0)}
                time.sleep(0.5)
                tap.s.write(":TFORce")
                cap["t_force"] = b.t()
            vbus, stop_poll = [], threading.Event()

            def poll():
                while not stop_poll.is_set():
                    try:
                        r = b.load.exchange(b.cfg, "status", timeout=0.5).get("reply") or {}
                        vbus.append((b.t(), r.get("vin_mv")))
                    except Exception:
                        pass

            th = threading.Thread(target=poll, daemon=True); th.start()
            time.sleep(0.3)
            n0 = len(b.rows)
            t0 = time.monotonic()
            t_call = b.t()
            with b.lock:
                b.probe.reset(b.cfg)
            t_reset = b.t()
            p, row = {}, {}
            while time.monotonic() - t0 < 8.0:      # the source's own view: a contract since the reset, back at 15 V
                try:
                    p, row = b.pd(), b.row()
                except Exception:
                    time.sleep(0.1); continue
                if (p.get("contract") or {}).get("mv") == 15000 and abs((row.get("v_out_mv") or 0) - 15000) < 750:
                    break
                time.sleep(0.05)
            dt = round(time.monotonic() - t0, 2)
            if cap is not None:
                cap.update(tap.grab("reset_15V", timeout=4.0))
            time.sleep(1.0)
            stop_poll.set(); th.join(timeout=2.0)
            seg = [r for r in b.rows[n0:] if r.get("v_out_mv") is not None]
            st, row, p = b.sink_status(), b.row(), b.pd()
            c = {"cycle": k, "t_reset_call": t_call, "t_reset": t_reset, "back_at_15V_s": dt, "capture": cap,
                 "qtpy_vin_mv": vbus,
                 "v_out_min_mv_in_reads": min((r["v_out_mv"] for r in seg), default=None),
                 "sink": {x: st.get(x) for x in ("attached", "contract_mv", "hard_resets", "contracts")},
                 "v_out_mv": row["v_out_mv"], "state": row["state"], "fault": row["fault"], "counters": p.get("counters"),
                 "ok": (st.get("contract_mv") == 15000 and st.get("hard_resets") == hr0 and row["state"] == "RUNNING"
                        and abs(row["v_out_mv"] - 15000) < 750 and (p.get("counters") or {}).get("hard_reset_count") == 0)}
            res["cycles"].append(c)
            print(f"  {time.strftime('%T')} reset {k}: 15 V again after {dt} s, V_out {row['v_out_mv']} mV, "
                  f"counters {c['counters']}, ok {c['ok']}", flush=True)
            if not c["ok"]:
                raise Abort(f"no clean re-attach after reset {k}: {c}")
            time.sleep(1.0)
    finally:
        if tap:
            tap.s.write(":TIMebase:MAIN:SCALe 0.02"); tap.s.write(":TIMebase:MAIN:OFFSet 0.02")
    res["pass"] = all(c["ok"] for c in res["cycles"])


def ph_detach(b: Bench, tap, res: dict) -> None:
    """vSafe0V (QT Py unplugged, so nothing back-feeds VBUS): the sink's Rd off at 15 V, then Rd back.  The source's
    V_out reading (VD_MON) and TP2 on the scope are both ahead of the output switch; VBUS itself has no probe."""
    ensure(b, 15000, res)
    res["before"] = {"sink": b.sink_status(), "pd": b.pd()}

    def forced(name):
        if not tap:
            return None
        c = {"armed": tap.arm("NEG", 10.0)}
        time.sleep(0.8)
        tap.s.write(":TFORce")
        c["t_force"] = b.t()
        time.sleep(0.2)
        return c

    if tap:
        tap.s.write(":TIMebase:MAIN:SCALe 0.2"); tap.s.write(":TIMebase:MAIN:OFFSet 0.6")   # [-0.6, +1.8] s
    try:
        cap = forced("detach")
        n0 = len(b.rows)
        t0 = time.monotonic()
        res["t_rd_off"] = b.t()
        res["rd_off"] = b.sink("rd off")
        time.sleep(2.5)
        if cap is not None:
            cap.update(tap.grab("detach_15V", timeout=4.0))
            res["capture_detach"] = cap
        time.sleep(1.0)
        seg = [r for r in b.rows[n0:] if "state" in r]
        idle = next((r for r in seg if r["state"] != "RUNNING"), None)
        low = next((r for r in seg if (r["v_out_mv"] if r["v_out_mv"] is not None else 99999) < 800), None)
        tail = [r["v_out_mv"] for r in seg[-10:] if r.get("v_out_mv") is not None]
        res["detached"] = {"first_not_running_s": round(idle["t"] - res["t_rd_off"], 2) if idle else None,
                           "first_read_under_800mV_s": round(low["t"] - res["t_rd_off"], 2) if low else None,
                           "v_out_mv_last_reads": tail, "dis_last": seg[-1].get("dis") if seg else None,
                           "switch_last": seg[-1].get("switch") if seg else None,
                           "state_last": seg[-1].get("state") if seg else None, "pd": b.pd(), "sink": b.sink_status()}
        with b.lock:
            res["detached"]["events"] = b.pd_.status(b.cfg).get("events")
        cap2 = forced("reattach")
        res["t_rd_both"] = b.t()
        res["rd_both"] = b.sink("rd both")
        st = wait_contract(b, 10.0)
        res["reattach"] = {"s_after_rd_both": round(b.t() - res["t_rd_both"], 2), "sink": st}
        time.sleep(3.0)
        if cap2 is not None:
            cap2.update(tap.grab("reattach", timeout=4.0))
            res["capture_reattach"] = cap2
        res["reattach"].update(row=b.row(), pd=b.pd(), sink_after=b.sink_status())
        with b.lock:
            res["reattach"]["events"] = b.pd_.status(b.cfg).get("events")
    finally:
        if tap:
            tap.s.write(":TIMebase:MAIN:SCALe 0.02"); tap.s.write(":TIMebase:MAIN:OFFSet 0.02")
    d = res["detached"]
    res["pass"] = bool(idle) and bool(low) and res["reattach"]["row"]["state"] == "RUNNING"


def ph_pulse(b: Bench, tap, res: dict) -> None:
    ensure(b, 15000, res)
    time.sleep(2.0)
    res["before"] = b.row()
    res["arm"] = b.trace_arm("switch")
    res["cmd"] = b.command("switch-pulse", arg=b.reg.pulse_us(5.0)).get("result")
    time.sleep(1.0)
    res["trace"] = b.trace_read(f"pd_seedfix_{res['stamp']}_pulse5ms.csv")
    time.sleep(2.0)
    res["after"] = b.row()
    res["pd"] = b.pd()
    res["pass"] = (not b.abort.is_set()) and res["after"]["state"] == "RUNNING" \
        and abs(res["after"]["v_out_mv"] - 15000) < 750


RUN = {"pulse": ph_pulse, "base": ph_base, "walk": ph_walk, "hold": ph_hold, "ripple": ph_ripple, "load": ph_load, "ccm": ph_ccm, "reset": ph_reset,
       "detach": ph_detach}
USES_SCOPE = ("walk", "ripple", "ccm", "reset", "detach")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--phase", choices=PHASES, required=True)
    p.add_argument("--go", action="store_true")
    p.add_argument("--no-scope", action="store_true")
    a = p.parse_args()
    print(f"PD FINAL, phase {a.phase}: {PLAN[a.phase]}\n  G474 EprPps build, 24 V in, 25.3 ohm on VBUS, "
          f"G431 sink on CC.  NO PACK.")
    if not a.go:
        print("(not run: add --go)")
        return 0
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = D / f"pd_seedfix_{a.phase}_{stamp}.json"
    b = Bench()
    signal.signal(signal.SIGINT, lambda *_: (setattr(b, "abort_why", "interrupted"), b.abort.set()))
    res = {"phase": a.phase, "stamp": stamp, "started": time.strftime("%F %T"), "plan": PLAN[a.phase]}
    keep = b.read("regulator_ccm_enable", "u8")
    tap = Tap(b, stamp) if (a.phase in USES_SCOPE and not a.no_scope) else None
    if tap and a.phase == "detach":
        tap.sources = ["CHAN1", "CHAN4"]      # SW1 and V_out: half the download
    done = threading.Event()
    lg = threading.Thread(target=b.logger, args=(done,), daemon=True)
    lg.start()
    try:
        RUN[a.phase](b, tap, res)
    except Abort as e:
        res["aborted"] = str(e)
        res["pass"] = False
        print(f"  ABORTED: {e}", flush=True)
    finally:
        done.set(); lg.join(timeout=3.0)
        try:
            if tap:
                tap.close()
            if b.read("regulator_ccm_enable", "u8") != keep:
                b.write("regulator_ccm_enable", keep, "u8")
            if res.get("aborted"):          # back to 5 V; a fault is left as it is
                res["abort_req_5V"] = b.sink("req 5000 3000", until="contract", timeout=3.0)
            res["end"] = {"regulator": b.row(), "sink": b.sink_status(), "pd": b.pd()}
        finally:
            res["rows"] = b.rows
            res["ended"] = time.strftime("%F %T")
            out.write_text(json.dumps(res, indent=1, default=str))
    e = res["end"]["regulator"]
    print(f"  phase {a.phase}: pass {res.get('pass')}; end {e['state']} fault {e['fault']} V_out {e['v_out_mv']} mV, "
          f"sink contract {res['end']['sink'].get('contract_mv')} mV, hard resets (sink) "
          f"{res['end']['sink'].get('hard_resets')}, counters {res['end']['pd'].get('counters')}\n  -> {out}")
    return 0 if res.get("pass") else 1


if __name__ == "__main__":
    sys.exit(main())
