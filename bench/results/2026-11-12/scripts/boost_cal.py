"""Boost calibration for the battery-load build before and on an 8S pack (bench 2026-11-12, plans/lipo-load.md).

Two phases, one mode (dcm / auto / forced CCM) per call.  WITHOUT --go NOTHING TOUCHES THE BOARD: the call prints
what it would do and exits.

  resistor   25.3 ohm on VBUS, NO PACK.  One run per ceiling (default 26, 28, 30, 32, 33 V; boost from 24.4 V), the
             current limit only a backstop.  Gives ID_MON against V/R (and the ammeter, if one is in series), the
             integrator u the boost law settles at (the board's offset over the model: the firmware learns it for
             the pack runs that follow without a reset), and shows the build runs boost at these voltages.
             Before the first step a START at 5 V checks that nothing holds VBUS up: with a pack there the
             firmware refuses it (PACK_AT_CEILING) and this phase stops.

  pack       The pack on VBUS with an ammeter in series.  THIS TOUCHES THE PACK: run it only on Dan's go for that
             run.  One run per current limit: the pack is read (output switch closed for 30 ms, every FET off),
             the ceiling is the read + a margin sized for that current (1.6 x I x the path resistance + 200 mV,
             300..1500 mV), so a limit that reads low is bounded by the ceiling, not by the reading.  Refused
             unless the pack reads 3.0-4.2 V a cell and the ceiling stays at or under 4.2 V a cell.

  ramp       The pack on VBUS.  THIS TOUCHES THE PACK: run it only on Dan's go for that run.  One run: one pack read,
             one START, the limit held at FROM, then the firmware's limit ramp (regulator_ccm_ilim_ramp_ma_s, the
             2026-11-12 BatteryNext build) up to TO at --rate, --hold at TO, down to FROM, held again.  --ccm auto
             (default) crosses DCM to CCM and back on the way: boost-kind CCM over buck-boost DCM enters at a DCM
             command of 400 mA and leaves under 200 mA (CCM_BOOST_ENTER_MA / _EXIT_MA), so FROM defaults to 150.
             The regulator trace (trigger ccm) is armed as the ramp starts: it records the DCM-to-CCM handover at
             20 kHz.  After the run: the entry and exit points, the limit's tracking per segment, the mode log.

Every run: the keepalive every sample (the firmware stops 2 s after the last one), a stop at the end and on
Ctrl-C / SIGTERM, and a stop when either current estimate stays over the step's abort level for 3 samples:
  - ID_MON as the limit reads it (regulator_ccm_i_avg_ma);
  - V_in x I_in / V_out, the input power over V_out.  It needs no ID_MON, and it is an upper bound on the pack
    current (the loss is in it: about 1.05 x in CCM, 1.3 x in DCM on the 4S ramp).
In CCM the firmware's SW_OCP is set to ID_MON's full scale for the run (about 5.3 A; the 2026-11-12 BatteryNext
build caps the level there, so an older build is refused) and restored after.

After each step, on a terminal, it asks for the ammeter's reading; with two or more it fits
meter = gain x ID_MON + offset in the firmware's terms (regulator_id_gain_permille / _offset_ma).  It changes
neither.

Run from the repository root:
  uv run --quiet --project tools/bringup python bench/results/2026-11-12/scripts/boost_cal.py resistor --ccm forced
  uv run --quiet --project tools/bringup python bench/results/2026-11-12/scripts/boost_cal.py pack --ccm dcm
  uv run --quiet --project tools/bringup python bench/results/2026-11-12/scripts/boost_cal.py ramp 150 5000 --rate 100
add --go to run it.  --selftest runs both phases against a simulated board (no hardware).
Output: bench/results/2026-11-12/boost_cal_<phase>_<ccm>_<first>-<last>.json (rewritten after every step).
"""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import shutil
import signal
import statistics
import sys
import threading
import time
from pathlib import Path

CCM_ENABLE = {"dcm": 0, "auto": 1, "forced": 2}
R_PATH_WORST = 0.13          # ohm, the 4S path's incremental resistance (bench 2026-11-09): the worst-case current
CELL_MIN_MV, CELL_MAX_MV = 3000, 4200
CEILING_MAX_MV = 34000       # BATTERY_CEILING_MAX_MV in the 2026-11-12 build
ILIM_MAX_MA = 5000
OCP_FULL_SCALE = 0xFFFF      # regulator_ccm_ocp_ma: SW_OCP at ID_MON's full scale (CCM_OCP_MA_FULL_SCALE, 2026-11-12)
ABORT_MAX_MA = 7500          # the script's stop level: V_in x I_in / V_out reads to about 10 A, ID_MON clips at 5.45 A
RAMP_DEFAULT_MA = [150, 5000]
RES_DEFAULT_MV = [26000, 28000, 30000, 32000, 33000]
PACK_DEFAULT_MA = {"dcm": [250, 500, 750, 1000], "auto": [500, 1000, 1500, 2000], "forced": [500, 1000, 1500, 2000]}
OVER_SAMPLES = 3


# ---------------------------------------------------------------- pure helpers
def above_for(ma: int, path_mohm: int) -> int:
    """The ceiling's margin over the pack read for a step at `ma`."""
    return max(300, min(1500, round(1.6 * ma * path_mohm / 1000.0 + 200)))


def abort_for_pack(ma: int) -> int:
    return min(ABORT_MAX_MA, max(2 * ma, ma + 600))


def abort_for_ramp(row: dict, top_ma: int) -> int:
    """The ramp's stop level follows the limit the firmware has now (the row's), not the top."""
    il = row.get("ilim_ma") or top_ma
    return min(ABORT_MAX_MA, max(2 * il, il + 1000))


def abort_for_resistor(mv: int, r_ohm: float) -> int:
    return round(1.6 * mv / r_ohm + 300)


def med(x: list[float]) -> float:
    """Median with linear interpolation inside the scope's code step (the grouped-data median, as ramp_table.py)."""
    m = statistics.median(x)
    u = sorted(set(x))
    if len(u) < 2:
        return m
    lsb = min(b - a for a, b in zip(u, u[1:]))
    below = sum(1 for v in x if v < m - lsb / 2)
    at = sum(1 for v in x if abs(v - m) <= lsb / 2)
    return m - lsb / 2 + lsb * (len(x) / 2 - below) / at if at else m


def fit(xs: list[float], ys: list[float]) -> dict | None:
    """Least squares y = a x + b; with one point, the ratio."""
    n = len(xs)
    if n == 0:
        return None
    if n == 1:
        return {"n": 1, "ratio": round(ys[0] / xs[0], 4) if xs[0] else None}
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return {"n": n, "ratio": round(my / mx, 4) if mx else None}
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    b = my - a * mx
    res = [y - (a * x + b) for x, y in zip(xs, ys)]
    return {"n": n, "gain": round(a, 4), "offset_ma": round(b, 1), "residual_max_ma": round(max(abs(r) for r in res), 1),
            "ratio_mean": round(sum(y / x for x, y in zip(xs, ys) if x) / n, 4)}


def tail_stats(rows: list[dict], settle_s: float) -> dict:
    w = [r for r in rows if r["state"] == "RUNNING" and r["t"] >= settle_s]
    if not w:
        return {"n": 0}
    def mean(k):
        v = [r[k] for r in w if r.get(k) is not None]
        return round(sum(v) / len(v), 1) if v else None
    out = {"n": len(w)}
    for k in ("v_out_mv", "v_in_mv", "id_raw_ma", "id_cal_ma", "id_lim_ma", "i_in_ma", "i_bound_ma", "u_mv", "on_ns"):
        out[k] = mean(k)
    out["ccm_frac"] = round(sum(1 for r in w if r.get("ccm")) / len(w), 2)
    out["lim_frac"] = round(sum(1 for r in w if r.get("lim")) / len(w), 2)
    out["skip_frac"] = round(sum(1 for r in w if r.get("skip")) / len(w), 2)
    ev = [r["skip_events"] for r in w if r.get("skip_events") is not None]
    out["skip_events"] = (ev[-1] - ev[0]) if len(ev) > 1 else None   # transitions inside the tail window
    out["modes"] = sorted({str(r.get("mode")) for r in w})
    out["kinds"] = sorted({r.get("kind") for r in w if r.get("kind") is not None})
    return out


# ---------------------------------------------------------------- the board
class Board:
    """The bringup tools, behind the few calls this script needs (FakeBoard stands in for --selftest)."""

    def __init__(self):
        from bringup import battery, probe, regulator
        from bringup.config import load_config
        self._battery, self._probe, self._regulator = battery, probe, regulator
        self.cfg = load_config()
        self.syms = battery._battery_syms(self.cfg)   # raises unless the ELF is the battery-load build

    def status(self) -> dict:
        return self._regulator.status(self.cfg)

    def command(self, name: str, mv: int | None = None) -> dict:
        return self._regulator.command(self.cfg, name, 5.0, mv=mv)

    def write(self, sym: str, value: int, dtype: str) -> None:
        self._probe.mem_write(self.cfg, sym, str(value), dtype)

    def read(self, sym: str, dtype: str) -> int:
        return self._probe.mem_read(self.cfg, sym, dtype=dtype, count=1)["values"][0]

    def read_n(self, sym: str, dtype: str, n: int) -> list[int]:
        return self._probe.mem_read(self.cfg, sym, dtype=dtype, count=n)["values"]

    def has(self, sym: str) -> bool:
        return sym in self._probe.symbols(self.cfg)

    def battery(self) -> dict:
        return self._battery._read(self.cfg, self.syms)

    def trace_arm(self, trigger: str, post: int, threshold_mv: int) -> dict:
        """The 20 kHz regulator trace (V_out, I_cmd, on-time, skipping / at-limit flags): resets the ring and arms."""
        return self._regulator.command(self.cfg, "trace-arm", 3.0,
                                       arg=self._regulator.trace_arg(trigger, post, threshold_mv))

    def trace_read(self, dst: Path) -> dict:
        from bringup.config import REPO_ROOT
        r = self._regulator.trace_read(self.cfg)
        src = Path(REPO_ROOT) / r["file"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        r["file"] = dst.name
        return r


def row_of(st: dict, t: float) -> dict:
    c, adc = st.get("control", {}), st.get("adc", {})
    v_out, v_in, i_in = adc.get("v_out_mv"), adc.get("v_in_mv"), c.get("i_in_filt_ma")
    bound = round(v_in * i_in / v_out, 1) if v_out and v_out > 1000 and v_in is not None and i_in is not None else None
    return {"t": round(t, 2), "state": st["state"], "fault": st["fault_source"], "mode": c.get("mode"),
            "ccm": c.get("ccm_active"), "kind": c.get("ccm_kind"), "lim": c.get("ccm_limiting"),
            "target_mv": c.get("target_mv"), "commanded_mv": c.get("commanded_mv"),
            "v_out_mv": v_out, "v_in_mv": v_in,
            "id_raw_ma": c.get("i_out_filt_ma"),    # ID_MON's 8-scan mean before gain and offset, filtered 1.6 ms
            "id_cal_ma": c.get("ccm_i_avg_ma"),     # the newest scan after gain and offset, filtered: what the limit acts on
            "id_lim_ma": c.get("ccm_i_ma"),         # the same, one PID cycle
            "i_in_ma": i_in,                        # IS_MON after its calibration, filtered
            "i_bound_ma": bound,                    # V_in x I_in / V_out
            "u_mv": c.get("ccm_u_mv"), "on_ns": c.get("on_time_ns"), "fsw_khz": c.get("fsw_khz"),
            "skip": c.get("pulses_skipped"),        # pulse skipping on at this sample (VD_MON over setpoint + 200 mV)
            "switch": st.get("lines", {}).get("OUTPUT_EN_PC8"), "dis": st.get("lines", {}).get("OUTPUT_DIS_PC9"),
            "ilim_ma": c.get("ccm_ilim_ma"),         # the limit the firmware has now (the ramp moves it)
            "ocp_fs_ma": c.get("ccm_ocp_fs_ma")}     # SW_OCP's full-scale level (ID_MON, after gain and offset)


class Stop(Exception):
    pass


def run_step(b, ceiling_mv: int, ilim_ma: int, seconds: float, abort_ma, interrupted: threading.Event,
             during=None, interval: float = 0.02, trace: dict | None = None) -> dict:
    """START at the ceiling, keep alive and log until `seconds`, a fault, an abort or an interrupt; always stop.
    `trace` = {"trigger", "threshold_mv", "post", "at", "file"}: arm the regulator trace `at` seconds into the
    run (so a start ramp cannot fire it) and read it after the stop."""
    lock = threading.Lock()
    armed = {"ok": False, "result": None}
    b.write("regulator_ccm_ilim_ma", ilim_ma, "u16")
    ka = b.battery()["keepalive"]
    start = b.command("start", mv=ceiling_mv)
    if not start.get("ok"):
        return {"ok": False, "start": start.get("result"), "explanation": start.get("explanation"),
                "battery": b.battery(), "rows": []}
    rows, done, t0 = [], threading.Event(), time.monotonic()
    info = {"abort": None}

    def logger() -> None:
        nonlocal ka
        over = 0
        while not done.is_set():
            with lock:
                ka = (ka + 1) & 0xFFFFFFFF
                b.write("regulator_battery_keepalive", ka, "u32")
                r = row_of(b.status(), time.monotonic() - t0)
                try:    # skip transitions since start (both directions); the 28 V step of 2026-11-12 hunted
                    r["skip_events"] = b.read("regulator_skip_events", "u32")
                except Exception:
                    r["skip_events"] = None
                if trace is not None and armed["result"] is None and r["t"] >= trace["at"] and r["state"] == "RUNNING":
                    try:
                        res = b.trace_arm(trace["trigger"], trace["post"], trace["threshold_mv"])
                        armed["ok"], armed["result"] = bool(res.get("ok")), res.get("result", "?")
                    except Exception as e:
                        armed["result"] = f"{type(e).__name__}: {e}"
                    r["trace_armed"] = armed["result"]
            rows.append(r)
            if r["state"] != "RUNNING":
                done.set()
                break
            lvl = abort_ma(r) if callable(abort_ma) else abort_ma
            hot = [k for k in ("id_cal_ma", "i_bound_ma") if r[k] is not None and r[k] > lvl]
            over = over + 1 if hot else 0
            if over >= OVER_SAMPLES:
                info["abort"] = f"{' and '.join(hot)} over {lvl} mA for {OVER_SAMPLES} samples (last: " \
                                f"ID_MON {r['id_cal_ma']} mA, V_in x I_in / V_out {r['i_bound_ma']} mA)"
                with lock:
                    b.command("stop")
                done.set()
                break
            time.sleep(interval)

    th = threading.Thread(target=logger, daemon=True)
    th.start()
    extra = None
    try:
        if during is not None:
            extra = during(lock, done, t0)
        while not done.is_set() and not interrupted.is_set() and time.monotonic() - t0 < seconds:
            time.sleep(0.05)
    finally:
        done.set()
        th.join(timeout=5.0)
        with lock:
            if b.status()["state"] == "RUNNING":
                b.command("stop")
    end = b.status()
    tr = None
    if trace is not None:
        tr = {"armed": armed["result"], "armed_at_s": trace["at"], "trigger": trace["trigger"],
              "threshold_mv": trace["threshold_mv"], "post": trace["post"]}
        if armed["ok"]:
            try:
                tr.update(b.trace_read(trace["file"]))
            except Exception as e:
                tr["error"] = f"{type(e).__name__}: {e}"
    return {"ok": bool(rows) and info["abort"] is None and not end["fault_source"] and end["state"] == "IDLE"
                  and rows[-1]["state"] == "RUNNING",
            "start": "OK", "abort": info["abort"], "interrupted": interrupted.is_set(),
            "end_state": end["state"], "end_fault": end["fault_source"], "battery": b.battery(), "rows": rows,
            "extra": extra, "trace": tr}


# ---------------------------------------------------------------- the scope (optional)
def period_mean_pp(t_s: list[float], v: list[float], period_s: float = 5e-6) -> float | None:
    """Range of the moving mean over one switching period: the slow swing with the 200 kHz ripple averaged out."""
    if len(t_s) < 3:
        return None
    w = max(1, int(round(period_s / (t_s[1] - t_s[0]))))
    if w >= len(v):
        return None
    acc = sum(v[:w]); lo = hi = acc / w
    for k in range(w, len(v)):
        acc += v[k] - v[k - w]
        m = acc / w
        lo, hi = min(lo, m), max(hi, m)
    return round(hi - lo, 3)


class ScopeTap:
    """CH2 V_in and CH4 V_out (converter side of Q5/Q6), 10x probes: one raw capture per step, trace medians."""

    def __init__(self, cfg: dict, out: Path, v_out_center: float, tdiv: float = 20e-6):
        from bringup.config import REPO_ROOT
        from bringup.scope import Scope
        self.root, self.out = Path(REPO_ROOT), out
        self.s = Scope(cfg).__enter__()
        s = self.s
        s.channel(2, display=True, coupling="DC", probe=10, scale=2, offset=-20, bwl="20M")
        s.channel(4, display=True, coupling="DC", probe=10, scale=2, offset=-v_out_center, bwl="20M")
        s.timebase(scale=tdiv, offset=0)   # 20 us/div: 240 us, 8 ns steps; 100 us/div sees a sub-ms swing
        s.trigger(source="CHAN4", slope="POS", level=v_out_center, sweep="AUTO")
        s.write(":RUN")
        s.write(":ACQuire:MDEPth 30000")

    def center(self, v_out_center: float) -> None:
        self.s.channel(4, scale=2, offset=-v_out_center)
        self.s.trigger(source="CHAN4", slope="POS", level=v_out_center, sweep="AUTO")
        self.s.write(":RUN")

    def grab(self, name: str) -> dict:
        s = self.s
        w = s.wait_trigger(timeout=3.0)
        if not w.get("triggered"):
            s.write(":TFORce")
            w = s.wait_trigger(timeout=3.0, arm=False)
        r = s.capture(["CHAN2", "CHAN4"], mode="raw")
        src = self.root / r["file"]
        with open(src, newline="") as f:
            rd = csv.DictReader(f)
            cols = {"t_s": [], "CHAN2": [], "CHAN4": []}
            for line in rd:
                for k in cols:
                    cols[k].append(float(line[k]))
        dst = self.out / f"{name}.csv.gz"
        with open(src, "rb") as fi, gzip.open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo)
        src.unlink()
        s.write(":RUN")
        return {"file": dst.name, "v_in_scope_v": round(med(cols["CHAN2"]), 4), "v_out_scope_v": round(med(cols["CHAN4"]), 4),
                "v_out_pp_v": round(max(cols["CHAN4"]) - min(cols["CHAN4"]), 3), "samples": len(cols["CHAN4"]),
                "v_out_period_mean_pp_v": period_mean_pp(cols["t_s"], cols["CHAN4"]),   # the swing under the 200 kHz ripple
                "warnings": r.get("warnings", [])}

    def close(self) -> None:
        self.s.__exit__(None, None, None)


# ---------------------------------------------------------------- the plan (printed before anything runs)
def plan_text(a) -> str:
    en = CCM_ENABLE[a.ccm]
    L = []
    if a.phase == "resistor":
        L += [f"RESISTOR phase, no pack: {a.r_ohm} ohm on VBUS, regulator_ccm_enable {en} ({a.ccm}).",
              "  First: START at 5 V.  A pack on VBUS makes the firmware refuse it and the phase stops there.",
              f"  Then {len(a.steps)} runs of {a.seconds:g} s, {a.idle:g} s idle between, limit {a.ilim} mA as a backstop:"]
        for mv in a.steps:
            L.append(f"    ceiling {mv} mV: {1000 * mv / 1000 / a.r_ohm:.0f} mA and {mv / 1000 * mv / 1000 / a.r_ohm:.0f} W "
                     f"in the resistor; stop over {abort_for_resistor(mv, a.r_ohm)} mA")
    else:
        lo, hi = a.cells * CELL_MIN_MV, a.cells * CELL_MAX_MV
        L += [f"PACK phase: {a.cells}S on VBUS with an ammeter in series, regulator_ccm_enable {en} ({a.ccm}).  "
              "THIS TOUCHES THE PACK.",
              f"  Each run: a pack read (output switch closed 30 ms, every FET off), refused unless {lo}..{hi} mV;",
              f"  ceiling = the read + the margin below, refused over {hi} mV; {a.seconds:g} s; {a.idle:g} s idle between.",
              f"  Margin: {'--above-mv ' + str(a.above_mv) if a.above_mv else f'1.6 x I x {a.path_mohm} mOhm + 200 mV'}."]
        for ma in a.steps:
            ab = a.above_mv or above_for(ma, a.path_mohm)
            L.append(f"    limit {ma} mA: ceiling +{ab} mV (if the limit did nothing: about {ab / R_PATH_WORST / 1000:.1f} A "
                     f"into {R_PATH_WORST} ohm); stop over {abort_for_pack(ma)} mA"
                     )
        if en:
            L.append("  SW_OCP at ID_MON's full scale for the whole run (about 5.3 A: 5.35 A raw after 991 / +4).")
        if en == 0:
            L.append("  DCM in boost rectifies through Q3's reverse conduction (about 2-3 V): 2-3 W in Q3 at 1 A.")
        if en == 2:
            L.append("  Forced CCM at 200 kHz: L1 ripples about 4.9 A p-p at 30 V from 24.4 V, so under about 2 A out its "
                     "valley is negative.")
    L += ["  Every run stops at the end, on Ctrl-C, on a fault, or when ID_MON or V_in x I_in / V_out stays over the",
          f"  stop level for {OVER_SAMPLES} samples.  regulator_ccm_enable"
          + (" and regulator_ccm_ocp_ma are" if a.phase == "pack" and en else " is") + " restored at the end.",
          f"  Scope: {'CH2 V_in and CH4 V_out captured once per step' if a.scope else 'not used (--scope adds CH2 / CH4 medians)'}.",
          f"  Regulator trace: {('armed ' + str(a.trace_at) + ' s into each run, trigger ' + a.trace + ', ' + str(a.trace_post) + ' samples after it') if a.trace else 'not used (--trace error:800 catches a 0.8 V excursion at 20 kHz)'}."]
    return "\n".join(L)


# ---------------------------------------------------------------- the phases
def ask_meter(a, label: str):
    if a.no_prompt or not sys.stdin.isatty():
        return None
    while True:
        try:
            s = input(f"  ammeter during {label} [mA], Enter to skip: ").strip()
        except (EOFError, KeyboardInterrupt):
            return None
        if not s:
            return None
        try:
            return float(s)
        except ValueError:
            print("  a number in mA, or Enter")


def summarize(a, steps: list[dict], gain_now: int, offset_now: int) -> dict:
    """Fits in the firmware's terms: the limit's ID_MON is raw x gain / 1000 + offset."""
    out = {"id_gain_permille_now": gain_now, "id_offset_ma_now": offset_now}
    def raw(s):   # the newest-scan mean before gain and offset
        v = s["tail"].get("id_cal_ma")
        return None if v is None or not gain_now else (v - offset_now) * 1000.0 / gain_now
    for name, key in (("ammeter", "meter_ma"), ("v_over_r", "v_over_r_ma"), ("scope_v_over_r", "scope_v_over_r_ma")):
        pts = [(raw(s), s[key]) for s in steps if s.get(key) is not None and s.get("tail", {}).get("n") and raw(s)]
        f = fit([p[0] for p in pts], [p[1] for p in pts])
        if f:
            if "gain" in f:
                f["id_gain_permille"] = round(1000 * f["gain"])
            out[f"fit_{name}"] = f
    us = [s["tail"]["u_mv"] for s in steps if s.get("tail", {}).get("n") and s["tail"].get("ccm_frac", 0) > 0.9
          and s["tail"].get("lim_frac", 1) < 0.1 and s["tail"].get("u_mv") is not None]
    if us:
        out["boost_u_mv"] = {"n": len(us), "mean": round(sum(us) / len(us)), "min": round(min(us)), "max": round(max(us)),
                             "note": "CCM, not limiting: what CCM_BOOST_U0_MV should be near"}
    return out


def line_of(s: dict) -> str:
    t = s.get("tail", {})
    if not t.get("n"):
        return f"  no RUNNING samples after the settle time (start {s['run'].get('start')}, end {s['run'].get('end_state')} " \
               f"{s['run'].get('end_fault')})"
    f0 = lambda k: "n/a" if t.get(k) is None else f"{t[k]:.0f}"
    return (f"  ok {s['run']['ok']}  {'/'.join(t['modes'])} ccm {t['ccm_frac']} lim {t['lim_frac']}  "
            f"V_out {f0('v_out_mv')} mV  ID_MON {f0('id_cal_ma')} mA (raw {f0('id_raw_ma')})  "
            f"I_in {f0('i_in_ma')} mA  V_in I_in / V_out {f0('i_bound_ma')} mA  u {f0('u_mv')} mV"
            + (f"  scope V_out {s['scope']['v_out_scope_v']:.3f} V" if s.get("scope") and "v_out_scope_v" in s["scope"] else ""))


def execute(a, b, out_dir: Path, interrupted: threading.Event, tap=None) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"boost_cal_{a.phase}_{a.ccm}_{a.steps[0]}-{a.steps[-1]}.json"
    st = b.status()
    if st["state"] != "IDLE":
        raise Stop(f"regulator is {st['state']}, not IDLE: stop it or clear the fault first")
    gain_now, offset_now = b.read("regulator_id_gain_permille", "i16"), b.read("regulator_id_offset_ma", "i16")
    ccm_before, ocp_before = b.read("regulator_ccm_enable", "u8"), b.read("regulator_ccm_ocp_ma", "u16")
    result = {"phase": a.phase, "ccm": a.ccm, "started": time.strftime("%F %T"), "args": {k: v for k, v in vars(a).items()},
              "steps": []}
    steps = result["steps"]

    def save():
        result["summary"] = summarize(a, steps, gain_now, offset_now)
        out.write_text(json.dumps(result, indent=1))

    def scope_during(name):
        if tap is None:
            return None
        def during(lock, done, t0):
            while not done.is_set() and time.monotonic() - t0 < a.settle:
                time.sleep(0.05)
            if done.is_set():
                return None
            try:
                return tap.grab(name)
            except Exception as e:   # the capture is extra: the run goes on
                return {"error": f"{type(e).__name__}: {e}"}
        return during

    try:
        if a.phase == "resistor":
            # Nothing may hold VBUS up: with a pack there the firmware refuses a 5 V ceiling.
            b.write("regulator_ccm_ilim_ma", min(a.ilim, 500), "u16")
            probe5 = b.command("start", mv=5000)
            if b.status()["state"] == "RUNNING":
                b.command("stop")
            pack_mv = b.battery()["pack_mv"]
            result["vbus_check"] = {"start_5v": probe5.get("result"), "pack_mv": pack_mv}
            if not probe5.get("ok") or pack_mv > 1500:
                raise Stop(f"START at 5 V gave {probe5.get('result')} with VBUS at {pack_mv} mV through the closed switch: "
                           "something holds VBUS up (a pack?).  The resistor phase needs VBUS free.")
            print(f"  VBUS free ({pack_mv} mV through the closed switch)")
            time.sleep(min(a.idle, 2.0))
        if a.phase == "pack" and CCM_ENABLE[a.ccm]:
            need_fs(b)
            b.write("regulator_ccm_ocp_ma", OCP_FULL_SCALE, "u16")
            result["ocp_fs_ma"] = ocp_fs_of(b)
        b.write("regulator_ccm_enable", CCM_ENABLE[a.ccm], "u8")   # after the 5 V check, which runs as the board was
        for n, step in enumerate(a.steps):
            if interrupted.is_set():
                break
            rec = {"step": step}
            if a.phase == "resistor":
                ceiling, ilim, abort = step, a.ilim, abort_for_resistor(step, a.r_ohm)
                label = f"ceiling {step} mV into {a.r_ohm} ohm ({a.ccm})"
            else:
                st = b.status()
                if st["state"] != "IDLE":
                    raise Stop(f"regulator is {st['state']} before the step")
                read = b.command("start", mv=5000)   # the pack read: refused with PACK_AT_CEILING
                pack_mv = b.battery()["pack_mv"]
                if read.get("result") != "PACK_AT_CEILING":
                    if b.status()["state"] == "RUNNING":
                        b.command("stop")
                    raise Stop(f"pack read: START at 5 V gave {read.get('result')} (VBUS {pack_mv} mV): no pack on VBUS?")
                lo, hi = a.cells * CELL_MIN_MV, a.cells * CELL_MAX_MV
                if not lo <= pack_mv <= hi:
                    raise Stop(f"pack reads {pack_mv} mV, outside {lo}..{hi} mV for {a.cells}S")
                above = a.above_mv or above_for(step, a.path_mohm)
                ceiling, ilim, abort = pack_mv + above, step, abort_for_pack(step)
                rec.update(pack_read_mv=pack_mv, above_mv=above)
                if ceiling > hi or ceiling > CEILING_MAX_MV:
                    raise Stop(f"ceiling {ceiling} mV (pack {pack_mv} + {above}) is over {min(hi, CEILING_MAX_MV)} mV")
                label = f"limit {step} mA, pack {pack_mv} mV + {above} mV ({a.ccm})"
            rec.update(ceiling_mv=ceiling, ilim_ma=ilim, abort_ma=abort)
            if tap is not None:
                tap.center(round(ceiling / 1000.0 - (0 if a.phase == "resistor" else 0.5)))
            print(f"\n==================================================================\n"
                  f"  {time.strftime('%T')}  STEP {n + 1}/{len(a.steps)}: {label} -- read the ammeter now ({a.seconds:g} s)\n"
                  f"==================================================================", flush=True)
            trace = None
            if a.trace:
                trig, _, thr = a.trace.partition(":")
                trace = {"trigger": trig, "threshold_mv": int(thr or 0), "post": a.trace_post, "at": a.trace_at,
                         "file": out_dir / "captures" / f"trace_{a.phase}_{a.ccm}_{step}.csv"}
            run = run_step(b, ceiling, ilim, a.seconds, abort, interrupted,
                           during=scope_during(f"{a.phase}_{a.ccm}_{step}"), trace=trace)
            rows = run.pop("rows")
            rec["run"] = run
            if run.get("trace"):
                tr = run["trace"]
                print(f"  trace: armed {tr.get('armed')} at {tr['armed_at_s']:g} s ({tr['trigger']}"
                      f"{' ' + str(tr['threshold_mv']) + ' mV' if tr['trigger'] == 'error' else ''}): "
                      + (f"{tr.get('state')}, {tr.get('samples')} samples, trigger at sample {tr.get('trigger_sample')}, "
                         f"V_out {tr.get('v_out_mv')}, skipping {tr.get('skipping_samples')}, at_limit "
                         f"{tr.get('at_limit_samples')} -> captures/{tr.get('file')}" if tr.get("samples") is not None
                         else tr.get("error", "not read")), flush=True)
            rec["tail"] = tail_stats(rows, a.settle)
            rec["rows"] = rows
            if run.get("extra"):
                rec["scope"] = run["extra"]
            t = rec["tail"]
            if a.phase == "resistor" and t.get("n"):
                rec["v_over_r_ma"] = round(t["v_out_mv"] / a.r_ohm, 1)   # VD_MON read 0.3-0.7 V high at 15 V (2026-11-11)
                if rec.get("scope", {}).get("v_out_scope_v"):
                    rec["scope_v_over_r_ma"] = round(1000 * rec["scope"]["v_out_scope_v"] / a.r_ohm, 1)
            steps.append(rec)
            save()
            print(line_of(rec), flush=True)
            if run.get("start") != "OK":
                raise Stop(f"START refused: {run.get('start')} ({run.get('explanation')})"
                           + ("  Over 25 V needs the 2026-11-12 build (BATTERY_CEILING_MAX_MV 34000)."
                              if run.get("start") == "OUT_OF_RANGE" else ""))
            if run.get("abort"):
                raise Stop(f"stopped the run: {run['abort']}")
            if run.get("end_fault") or run.get("end_state") != "IDLE":
                raise Stop(f"FAULT or not IDLE after the step: {run.get('end_state')} {run.get('end_fault')} "
                           "(not cleared; read `bu regulator status`)")
            if rows and rows[-1]["state"] != "RUNNING" and not run.get("interrupted"):
                raise Stop("the regulator left RUNNING during the step with no fault (keepalive?)")
            if interrupted.is_set():
                break
            rec["meter_ma"] = ask_meter(a, label)
            save()
            if n + 1 < len(a.steps):
                print(f"  {time.strftime('%T')}  idle {a.idle:g} s (the meter should read about 0)", flush=True)
                t_end = time.monotonic() + a.idle
                while time.monotonic() < t_end and not interrupted.is_set():
                    time.sleep(0.05)
    except Stop as e:
        result["stopped"] = str(e)
        print(f"\n  STOPPED: {e}", flush=True)
    finally:
        try:
            if b.status()["state"] == "RUNNING":
                b.command("stop")
        finally:
            b.write("regulator_ccm_enable", ccm_before, "u8")
            b.write("regulator_ccm_ocp_ma", ocp_before, "u16")
            result["interrupted"] = interrupted.is_set()
            result["ended"] = time.strftime("%F %T")
            save()
    s = result["summary"]
    print(f"\n  {time.strftime('%T')}  DONE: {len(steps)} of {len(a.steps)} steps -> {out}")
    for k in ("fit_ammeter", "fit_scope_v_over_r", "fit_v_over_r"):
        if k in s:
            print(f"  {k}: {s[k]}  (now {gain_now} / {offset_now:+d})")
    if "boost_u_mv" in s:
        print(f"  u in CCM, not limiting: {s['boost_u_mv']['mean']} mV ({s['boost_u_mv']['min']}..{s['boost_u_mv']['max']})")
    return result


def ocp_fs_of(b) -> int:
    """SW_OCP's full-scale level as the firmware computes it while running (regulator_ccm_ocp_fs_ma reads 0 until then):
    (ADC_ID_FULL_SCALE_MA 5500 - CCM_OCP_FS_SWING_MA 150) x gain / 1000 + offset."""
    g, o = b.read("regulator_id_gain_permille", "i16"), b.read("regulator_id_offset_ma", "i16")
    return (5350 * g) // 1000 + o


def need_fs(b) -> None:
    """SW_OCP at full scale needs the firmware that caps the level at ID_MON's full scale: on an older build 0xFFFF
    in regulator_ccm_ocp_ma is a level ID_MON can never reach, so SW_OCP would be off."""
    if not b.has("regulator_ccm_ocp_fs_ma"):
        raise Stop("SW_OCP at full scale needs the 2026-11-12 BatteryNext build (regulator_ccm_ocp_fs_ma): on this "
                   "firmware 0xFFFF would turn SW_OCP off")


def pack_read(a, b) -> int:
    """START at 5 V, which the firmware refuses with PACK_AT_CEILING after reading the pack (switch closed 30 ms)."""
    st = b.status()
    if st["state"] != "IDLE":
        raise Stop(f"regulator is {st['state']} before the pack read")
    read = b.command("start", mv=5000)
    pack_mv = b.battery()["pack_mv"]
    if read.get("result") != "PACK_AT_CEILING":
        if b.status()["state"] == "RUNNING":
            b.command("stop")
        raise Stop(f"pack read: START at 5 V gave {read.get('result')} (VBUS {pack_mv} mV): no pack on VBUS?")
    lo, hi = a.cells * CELL_MIN_MV, a.cells * CELL_MAX_MV
    if not lo <= pack_mv <= hi:
        raise Stop(f"pack reads {pack_mv} mV, outside {lo}..{hi} mV for {a.cells}S")
    return pack_mv


def ramp_times(a) -> dict:
    fr, to = a.steps
    leg = (to - fr) / a.rate
    up = a.settle
    top = up + leg
    down = top + a.hold
    bottom = down + leg
    return {"up_s": up, "top_s": round(top, 2), "down_s": round(down, 2), "bottom_s": round(bottom, 2),
            "end_s": round(bottom + a.settle, 2), "leg_s": round(leg, 2)}


CCM_EXITS = {0: "NONE", 1: "DISABLED", 2: "MODE", 3: "SWITCH", 4: "LIGHT_LOAD", 5: "OVER_BAND", 6: "VIN_RISE",
             7: "STOP", 8: "BAD_CONFIG", 9: "DUTY", 10: "RAMP_DOWN"}


def ramp_summary(rows: list[dict], tm: dict, marks: dict) -> dict:
    """Entry and exit points, the limit's tracking per segment, mode changes."""
    run = [r for r in rows if r["state"] == "RUNNING"]
    up0 = marks.get("up_at_s", tm["up_s"])
    dn0 = marks.get("down_at_s", tm["down_s"])
    def seg(r):
        t = r["t"]
        if t < up0:
            return "hold_from"
        if t < up0 + tm["leg_s"]:
            return "up"
        if t < dn0:
            return "hold_to"
        if t < dn0 + tm["leg_s"]:
            return "down"
        return "hold_back"
    out = {"segments": {}, "entries": [], "exits": [], "mode_changes": []}
    for name in ("hold_from", "up", "hold_to", "down", "hold_back"):
        w = [r for r in run if seg(r) == name and r.get("ilim_ma") and r.get("id_cal_ma") is not None]
        if not w:
            continue
        err = [r["id_cal_ma"] - r["ilim_ma"] for r in w]
        out["segments"][name] = {
            "n": len(w), "ilim_ma": [w[0]["ilim_ma"], w[-1]["ilim_ma"]],
            "err_ma": {"mean": round(sum(err) / len(err)), "min": min(err), "max": max(err)},
            "ccm_frac": round(sum(1 for r in w if r.get("ccm")) / len(w), 2),
            "lim_frac": round(sum(1 for r in w if r.get("lim")) / len(w), 2),
            "modes": sorted({str(r.get("mode")) for r in w})}
    pt = lambda r: {"t": r["t"], "ilim_ma": r.get("ilim_ma"), "id_cal_ma": r.get("id_cal_ma"),
                    "v_out_mv": r.get("v_out_mv"), "mode": r.get("mode"), "kind": r.get("kind")}
    for p, r in zip(run, run[1:]):
        if not p.get("ccm") and r.get("ccm"):
            out["entries"].append(pt(r))
        if p.get("ccm") and not r.get("ccm"):
            out["exits"].append(pt(r))
        if p.get("mode") != r.get("mode"):
            out["mode_changes"].append({**pt(r), "from": p.get("mode")})
    for k in ("id_cal_ma", "i_bound_ma", "ilim_ma"):
        v = [r[k] for r in run if r.get(k) is not None]
        out[f"max_{k}"] = max(v) if v else None
    return out


def ramp_plan_text(a) -> str:
    fr, to = a.steps
    tm = ramp_times(a)
    en = CCM_ENABLE[a.ccm]
    above = a.above_mv or above_for(to, a.path_mohm)
    lo, hi = a.cells * CELL_MIN_MV, a.cells * CELL_MAX_MV
    L = [f"RAMP phase: {a.cells}S on VBUS with an ammeter in series, regulator_ccm_enable {en} ({a.ccm}).  "
         "THIS TOUCHES THE PACK.",
         f"  One pack read (output switch closed 30 ms, every FET off), refused unless {lo}..{hi} mV; ceiling = the read"
         f" + {above} mV (sized for {to} mA), refused over {hi} mV.",
         f"  Limit {fr} mA for {a.settle:g} s; the firmware ramps it to {to} mA at {a.rate} mA/s ({tm['leg_s']:g} s); "
         f"{a.hold:g} s at {to} mA (read the ammeter); back to {fr} mA ({tm['leg_s']:g} s); {a.settle:g} s; stop.  "
         f"{tm['end_s']:g} s in all.",
         f"  If the limit did nothing: about {above / R_PATH_WORST / 1000:.1f} A into {R_PATH_WORST} ohm.  Stop when ID_MON "
         f"or V_in x I_in / V_out stays over max(2 x, + 1000 mA) the limit of the moment (at most {ABORT_MAX_MA} mA) "
         f"for {OVER_SAMPLES} samples."
         + ("  SW_OCP at ID_MON's full scale (about 5.3 A)." if en else "")]
    if en == 1:
        L.append("  Auto CCM: boost-kind CCM enters at a DCM command of 400 mA, leaves under 200 mA (ID_MON mean).")
    L += ["  The firmware clears the ramp rate at every stop and fault.  regulator_ccm_enable"
          + (", regulator_ccm_ocp_ma" if en else "") + " and the ramp registers are restored at the end.",
          f"  Regulator trace: {('trigger ' + a.trace + ', armed ' + str(a.trace_at) + ' s in, ' + str(a.trace_post) + ' samples after it') if a.trace else 'not used'}."]
    return "\n".join(L)


def execute_ramp(a, b, out_dir: Path, interrupted: threading.Event) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    fr, to = a.steps
    out = out_dir / f"boost_cal_ramp_{a.ccm}_{fr}-{to}_{a.rate}.json"
    tm = ramp_times(a)
    st = b.status()
    if st["state"] != "IDLE":
        raise Stop(f"regulator is {st['state']}, not IDLE: stop it or clear the fault first")
    if not b.has("regulator_ccm_ilim_ramp_ma_s"):
        result = {"phase": "ramp", "stopped": "the ramp needs the firmware's limit ramp (the 2026-11-12 BatteryNext build)"}
        print(f"\n  STOPPED: {result['stopped']}", flush=True)
        return result
    ccm_before, ocp_before = b.read("regulator_ccm_enable", "u8"), b.read("regulator_ccm_ocp_ma", "u16")
    result = {"phase": "ramp", "ccm": a.ccm, "started": time.strftime("%F %T"), "args": dict(vars(a)), "times": tm}
    def save():
        out.write_text(json.dumps(result, indent=1))
    en = CCM_ENABLE[a.ccm]
    try:
        if en:
            need_fs(b)
        pack_mv = pack_read(a, b)
        above = a.above_mv or above_for(to, a.path_mohm)
        ceiling = pack_mv + above
        hi = a.cells * CELL_MAX_MV
        if ceiling > hi or ceiling > CEILING_MAX_MV:
            raise Stop(f"ceiling {ceiling} mV (pack {pack_mv} + {above}) is over {min(hi, CEILING_MAX_MV)} mV")
        result.update(pack_read_mv=pack_mv, above_mv=above, ceiling_mv=ceiling)
        b.write("regulator_ccm_enable", en, "u8")
        if en:
            b.write("regulator_ccm_ocp_ma", OCP_FULL_SCALE, "u16")
            result["ocp_fs_ma"] = ocp_fs_of(b)
        b.write("regulator_ccm_ilim_ramp_ma_s", 0, "u16")
        b.write("regulator_ccm_ilim_target_ma", fr, "u16")
        n0 = {k: b.read(k, "u32") for k in ("regulator_ccm_entries", "regulator_ccm_exits")}
        print(f"\n==================================================================\n"
              f"  {time.strftime('%T')}  RAMP {fr} -> {to} -> {fr} mA at {a.rate} mA/s, pack {pack_mv} mV + {above} mV "
              f"({a.ccm}), {tm['end_s']:g} s\n"
              f"==================================================================", flush=True)
        marks = {}
        def during(lock, done, t0):
            def until(t):
                while not done.is_set() and not interrupted.is_set() and time.monotonic() - t0 < t:
                    time.sleep(0.02)
                return not done.is_set() and not interrupted.is_set()
            if not until(tm["up_s"]):
                return marks
            with lock:
                b.write("regulator_ccm_ilim_target_ma", to, "u16")
                b.write("regulator_ccm_ilim_ramp_ma_s", a.rate, "u16")
            marks["up_at_s"] = round(time.monotonic() - t0, 2)
            print(f"  {time.strftime('%T')}  up: {fr} -> {to} mA ({tm['leg_s']:g} s)", flush=True)
            if not until(marks["up_at_s"] + tm["leg_s"]):
                return marks
            print(f"  {time.strftime('%T')}  TOP: {to} mA for {a.hold:g} s -- read the ammeter now", flush=True)
            if not until(marks["up_at_s"] + tm["leg_s"] + a.hold):
                return marks
            with lock:
                b.write("regulator_ccm_ilim_target_ma", fr, "u16")
            marks["down_at_s"] = round(time.monotonic() - t0, 2)
            print(f"  {time.strftime('%T')}  down: {to} -> {fr} mA ({tm['leg_s']:g} s)", flush=True)
            return marks
        trace = None
        if a.trace:
            trig, _, thr = a.trace.partition(":")
            trace = {"trigger": trig, "threshold_mv": int(thr or 0), "post": a.trace_post, "at": a.trace_at,
                     "file": out_dir / "captures" / f"trace_ramp_{a.ccm}_{fr}-{to}_{a.rate}.csv"}
        run = run_step(b, ceiling, fr, tm["end_s"], lambda r: abort_for_ramp(r, to), interrupted,
                       during=during, trace=trace)
        rows = run.pop("rows")
        result["run"], result["rows"], result["marks"] = run, rows, marks
        result["counts"] = {k.replace("regulator_", ""): b.read(k, "u32") - n0[k] for k in n0}
        try:
            idx = b.read("regulator_ccm_exit_log_index", "u32")
            log = b.read_n("regulator_ccm_exit_log", "u32", 16)
            result["exit_log"] = [{"tick": log[2 * i], "why": CCM_EXITS.get(log[2 * i + 1] & 0xFF, log[2 * i + 1] & 0xFF),
                                   "entries": log[2 * i + 1] >> 8} for i in range(8) if log[2 * i]]
            result["exit_log_index"] = idx
        except Exception as e:
            result["exit_log"] = f"{type(e).__name__}: {e}"
        result["ramp_rate_after"] = b.read("regulator_ccm_ilim_ramp_ma_s", "u16")
        result["summary"] = ramp_summary(rows, tm, marks)
        save()
        if run.get("trace"):
            tr = run["trace"]
            print(f"  trace: armed {tr.get('armed')} at {tr['armed_at_s']:g} s ({tr['trigger']}): "
                  + (f"{tr.get('state')}, trigger at sample {tr.get('trigger_sample')}, ccm samples {tr.get('ccm_samples')} "
                     f"-> captures/{tr.get('file')}" if tr.get("samples") is not None else tr.get("error", "not read")))
        sm = result["summary"]
        for name, g in sm["segments"].items():
            print(f"  {name:9s} limit {g['ilim_ma'][0]}->{g['ilim_ma'][1]} mA  ID_MON - limit {g['err_ma']['mean']:+d} "
                  f"({g['err_ma']['min']:+d}..{g['err_ma']['max']:+d}) mA  ccm {g['ccm_frac']} lim {g['lim_frac']}  "
                  f"{'/'.join(g['modes'])}")
        for k in ("entries", "exits", "mode_changes"):
            print(f"  {k}: {sm[k] if sm[k] else 'none'}")
        print(f"  CCM entries / exits in the run: {result['counts']}; ramp rate after the stop: {result['ramp_rate_after']}")
        if run.get("start") != "OK":
            raise Stop(f"START refused: {run.get('start')} ({run.get('explanation')})")
        if run.get("abort"):
            raise Stop(f"stopped the run: {run['abort']}")
        if run.get("end_fault") or run.get("end_state") != "IDLE":
            raise Stop(f"FAULT or not IDLE after the ramp: {run.get('end_state')} {run.get('end_fault')} "
                       "(not cleared; read `bu regulator status`)")
        if not run.get("interrupted"):
            result["meter_top_ma"] = ask_meter(a, f"the top hold at {to} mA")
    except Stop as e:
        result["stopped"] = str(e)
        print(f"\n  STOPPED: {e}", flush=True)
    finally:
        try:
            if b.status()["state"] == "RUNNING":
                b.command("stop")
        finally:
            b.write("regulator_ccm_ilim_ramp_ma_s", 0, "u16")
            b.write("regulator_ccm_enable", ccm_before, "u8")
            b.write("regulator_ccm_ocp_ma", ocp_before, "u16")
            result["interrupted"] = interrupted.is_set()
            result["ended"] = time.strftime("%F %T")
            save()
    print(f"\n  {time.strftime('%T')}  DONE -> {out}")
    return result


# ---------------------------------------------------------------- --selftest: a simulated board
class FakeBoard:
    """IDLE / RUNNING with the firmware's START rules, a resistor or a pack on VBUS, ID_MON reading id_gain x the
    truth in CCM.  Enough to run both phases end to end; it models no dynamics."""

    def __init__(self, pack_mv: int = 0, r_ohm: float = 25.3, r_path: float = 0.15, id_gain: float = 0.7,
                 ignore_limit: bool = False, ceiling_max: int = CEILING_MAX_MV, new_fw: bool = True):
        self.pack_mv, self.r_ohm, self.r_path, self.id_gain = pack_mv, r_ohm, r_path, id_gain
        self.ignore_limit, self.ceiling_max = ignore_limit, ceiling_max
        self.state, self.ceiling = "IDLE", 0
        self.mem = {"regulator_ccm_ilim_ma": 500, "regulator_battery_keepalive": 0, "regulator_ccm_enable": 1,
                    "regulator_ccm_ocp_ma": 5000, "regulator_id_gain_permille": 991, "regulator_id_offset_ma": 4,
                    "regulator_ccm_entries": 0, "regulator_ccm_exits": 0, "regulator_ccm_exit_log_index": 0}
        if new_fw:   # the 2026-11-12 BatteryNext build: the limit ramp and SW_OCP's input-side bound
            self.mem.update({"regulator_ccm_ocp_fs_ma": 5305, "regulator_ccm_ilim_ramp_ma_s": 0,
                             "regulator_ccm_ilim_target_ma": 0})
        self.read_mv, self.log = 0, []
        self.ccm_on, self.t_last = False, None

    def _ramp(self):
        now = time.monotonic()
        dt, self.t_last = (0.0 if self.t_last is None else now - self.t_last), now
        rate = self.mem.get("regulator_ccm_ilim_ramp_ma_s", 0)
        if rate:
            il, tgt = self.mem["regulator_ccm_ilim_ma"], max(1, self.mem["regulator_ccm_ilim_target_ma"])
            d = rate * dt
            il = min(tgt, il + d) if il < tgt else max(tgt, il - d)
            self.mem["regulator_ccm_ilim_ma"] = il

    def _point(self):
        ccm = self.mem["regulator_ccm_enable"] == 2 or (self.mem["regulator_ccm_enable"] == 1 and self.ccm_on)
        g = self.id_gain if ccm else 1.0
        if self.pack_mv:
            i_v = (self.ceiling - self.pack_mv) / self.r_path            # what the ceiling would push, mA
            i = i_v if self.ignore_limit else min(i_v, self.mem["regulator_ccm_ilim_ma"] / g)
            v, lim = self.pack_mv + i * self.r_path, i < i_v
        else:
            i_v = self.ceiling / self.r_ohm
            i = min(i_v, self.mem["regulator_ccm_ilim_ma"] / g)
            v, lim = i * self.r_ohm, i < i_v
        return v, i, ccm, lim, g

    def status(self):
        st = {"state": self.state, "fault_source": [], "lines": {"OUTPUT_EN_PC8": int(self.state == "RUNNING"),
                                                                 "OUTPUT_DIS_PC9": 0}}
        if self.state == "RUNNING":
            self._ramp()
            v, i, ccm, lim, g = self._point()
            if self.mem["regulator_ccm_enable"] == 1:   # auto, boost kind: enter at 400 mA, leave under 200 mA
                was, self.ccm_on = self.ccm_on, (i >= 200 if self.ccm_on else i >= 400)
                if self.ccm_on != was:
                    self.mem["regulator_ccm_entries" if self.ccm_on else "regulator_ccm_exits"] += 1
            raw = i * g
            st["control"] = {"mode": "BOOST" if v > 25400 else "BUCK_BOOST" if v > 23400 else "BUCK", "ccm_active": int(ccm),
                             "ccm_kind": 2 if ccm else 0, "ccm_limiting": int(lim), "target_mv": self.ceiling,
                             "commanded_mv": self.ceiling, "i_out_filt_ma": round(raw),
                             "ccm_i_avg_ma": round(raw * 0.991 + 4), "ccm_i_ma": round(raw * 0.991 + 4),
                             "i_in_filt_ma": round(v * i / (0.95 if ccm else 0.8) / 24400), "ccm_u_mv": -440.0 if ccm else 0.0,
                             "on_time_ns": 900, "fsw_khz": 200, "ccm_ilim_ma": round(self.mem["regulator_ccm_ilim_ma"]),
                             "i_out_bound_ma": round(i / 0.95)}
            st["adc"] = {"v_out_mv": round(v), "v_in_mv": 24400}
        return st

    def command(self, name, mv=None):
        self.log.append((name, mv))
        if name == "stop":
            self.state = "IDLE"
            if "regulator_ccm_ilim_ramp_ma_s" in self.mem:
                self.mem["regulator_ccm_ilim_ramp_ma_s"] = 0   # the firmware clears it at every stop
            return {"ok": True}
        if not 5000 <= mv <= self.ceiling_max:
            return {"ok": False, "result": "OUT_OF_RANGE"}
        self.read_mv = self.pack_mv
        if self.pack_mv >= mv:
            return {"ok": False, "result": "PACK_AT_CEILING"}
        self.state, self.ceiling = "RUNNING", mv
        self.ccm_on, self.t_last = False, None
        return {"ok": True, "result": "OK"}

    def write(self, sym, value, dtype):
        self.mem[sym] = value

    def read(self, sym, dtype):
        if sym not in self.mem and sym.startswith(("regulator_ccm_ilim_ramp", "regulator_ccm_ilim_target",
                                                   "regulator_ccm_ocp_fs")):
            raise KeyError(sym)   # an older build has no such symbol
        return self.mem.get(sym, 0)

    def read_n(self, sym, dtype, n):
        return [0] * n

    def has(self, sym):
        return sym in self.mem

    def battery(self):
        return {"pack_mv": self.read_mv, "keepalive": self.mem["regulator_battery_keepalive"]}

    def trace_arm(self, trigger, post, threshold_mv):
        self.log.append(("trace-arm", trigger))
        return {"ok": True, "result": "OK"}

    def trace_read(self, dst):
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text("t_ms,v_out_mv\n0.00,28000\n")
        return {"ok": True, "state": "DONE", "samples": 1, "trigger_sample": 0, "v_out_mv": {"min": 28000, "max": 28000},
                "skipping_samples": 0, "at_limit_samples": 0, "file": dst.name}


def selftest() -> int:
    import tempfile
    fails = []
    def check(ok, what):
        print(("  ok   " if ok else "  FAIL ") + what)
        if not ok:
            fails.append(what)
    def args(phase, ccm, steps, **kw):
        d = dict(phase=phase, ccm=ccm, steps=steps, seconds=0.5, settle=0.15, idle=0.0, ilim=2000, r_ohm=25.3, cells=8,
                 path_mohm=170, above_mv=None, scope=False, no_prompt=True, go=True,
                 trace="error:800" if phase == "resistor" else None, trace_at=0.05, trace_post=1536, scope_tdiv=20e-6,
                 rate=100, hold=5.0)
        d.update(kw)
        return argparse.Namespace(**d)
    tmp = Path(tempfile.mkdtemp(prefix="boost_cal_selftest_"))
    ev = threading.Event()

    check(abort_for_pack(5000) == 7500 and abort_for_pack(3000) == 6000, "SW_OCP and stop levels")
    check(above_for(250, 170) == 300 and above_for(1000, 170) == 472 and above_for(5000, 170) == 1500, "ceiling margins")
    f = fit([700, 1400, 2100], [1004, 2004, 3004])
    check(abs(f["gain"] - 1.4286) < 1e-3 and abs(f["offset_ma"] - 4) < 0.2, f"fit {f}")
    check(abs(med([1.0] * 3 + [2.0] * 5 + [3.0] * 2) - 1.9) < 1e-9, "interpolated median")
    import math
    ts = [k * 8e-9 for k in 30000 * [0]]; ts = [k * 8e-9 for k in range(30000)]
    vv = [28 + math.sin(2 * math.pi * 7000 * t) + 0.5 * math.sin(2 * math.pi * 200e3 * t) for t in ts]
    pm = period_mean_pp(ts, vv)
    check(pm is not None and 1.9 < pm < 2.05, f"period-mean p-p drops the 200 kHz ripple, keeps the 7 kHz swing ({pm})")

    b = FakeBoard()
    r = execute(args("resistor", "forced", [26000, 30000]), b, tmp, ev)
    t = r["steps"][1]["tail"]
    check(len(r["steps"]) == 2 and all(s["run"]["ok"] for s in r["steps"]), "resistor: both steps ran")
    check(abs(t["v_out_mv"] - 30000) < 5 and abs(t["id_raw_ma"] - 0.7 * 30000 / 25.3) < 2, f"resistor tail {t['v_out_mv']} mV")
    fr = r["summary"].get("fit_v_over_r", {})
    check(abs(fr.get("gain", 0) - 1 / 0.7) < 0.01, f"resistor fit against V/R finds the 0.7x reading: {fr}")
    check(r["summary"].get("boost_u_mv", {}).get("mean") == -440, "resistor: u reported")
    tr = r["steps"][0]["run"].get("trace") or {}
    check(tr.get("armed") == "OK" and tr.get("samples") == 1 and ("trace-arm", "error") in b.log
          and (tmp / "captures" / tr.get("file", "")).exists(), f"resistor: trace armed in-run and read: {tr}")
    check(b.mem["regulator_ccm_enable"] == 1 and b.state == "IDLE", "resistor: CCM enable restored, IDLE")

    b = FakeBoard(pack_mv=30000)
    r = execute(args("resistor", "forced", [32000]), b, tmp, ev)
    check("stopped" in r and not r["steps"] and b.state == "IDLE" and ("start", 32000) not in b.log,
          "resistor phase refuses with a pack on VBUS")

    b = FakeBoard(pack_mv=30000)
    r = execute(args("pack", "dcm", [250, 500, 1000]), b, tmp, ev)
    check(len(r["steps"]) == 3 and all(s["run"]["ok"] for s in r["steps"]), "pack dcm: three steps ran")
    check(all(abs(s["tail"]["id_cal_ma"] - s["step"]) < 12 and s["tail"]["lim_frac"] == 1.0 for s in r["steps"]),
          "pack dcm: the limit holds each step")
    check([s["ceiling_mv"] for s in r["steps"]] == [30300, 30336, 30472], "pack: ceilings from the read")

    b = FakeBoard(pack_mv=30000)
    r = execute(args("pack", "forced", [500, 1000]), b, tmp, ev)
    check(all(s["run"]["ok"] for s in r["steps"]) and abs(r["steps"][1]["tail"]["i_bound_ma"] / (1000 / 0.7) - 1 / 0.95) < 0.1,
          "pack forced, ID_MON 0.7x: runs, the input-power bound shows the real current")
    check(b.mem["regulator_ccm_ocp_ma"] == 5000 and b.mem["regulator_ccm_enable"] == 1, "pack: SW_OCP and CCM enable restored")
    check(r.get("ocp_fs_ma") == 5305, "pack forced: SW_OCP at full scale for the run")

    b = FakeBoard(pack_mv=30000, ignore_limit=True)
    r = execute(args("pack", "dcm", [500, 1000]), b, tmp, ev)
    check(len(r["steps"]) == 1 and r["steps"][0]["run"]["abort"] and "stopped" in r and b.state == "IDLE",
          "pack: a limit that does nothing is stopped by the abort level, and the sweep ends")

    for mv, why in ((23000, "under 3.0 V a cell"), (33700, "over 4.2 V a cell"), (33400, "ceiling over 4.2 V a cell")):
        b = FakeBoard(pack_mv=mv)
        r = execute(args("pack", "dcm", [500]), b, tmp, ev)
        check("stopped" in r and not r["steps"] and b.state == "IDLE", f"pack refused: {why} ({mv} mV)")

    b = FakeBoard(pack_mv=0)
    r = execute(args("pack", "dcm", [500]), b, tmp, ev)
    check("stopped" in r and not r["steps"] and b.state == "IDLE", "pack phase refuses with no pack")

    b = FakeBoard(ceiling_max=25000)
    r = execute(args("resistor", "dcm", [28000]), b, tmp, ev)
    check("OUT_OF_RANGE" in r.get("stopped", ""), "old firmware (25 V ceiling) is reported")

    b = FakeBoard(pack_mv=30000)
    ev2 = threading.Event()
    threading.Timer(0.2, ev2.set).start()
    r = execute(args("pack", "dcm", [500, 1000], seconds=5.0), b, tmp, ev2)
    check(r["interrupted"] and len(r["steps"]) == 1 and b.state == "IDLE", "Ctrl-C stops the run and the sweep")

    b = FakeBoard(pack_mv=30000, id_gain=1.0, new_fw=False)
    r = execute(args("pack", "forced", [5000]), b, tmp, ev)
    check("stopped" in r and "full scale" in r["stopped"] and not r["steps"] and ("start", 5000) not in b.log,
          "pack 5000 forced on the old firmware: refused before any START (0xFFFF would turn SW_OCP off)")
    b = FakeBoard(pack_mv=30000, id_gain=1.0)
    r = execute(args("pack", "forced", [5000], seconds=0.3), b, tmp, ev)
    check(r["steps"] and r.get("ocp_fs_ma") == 5305 and r["steps"][0]["run"]["ok"]
          and b.mem["regulator_ccm_ocp_ma"] == 5000, "pack 5000 forced on the new firmware: SW_OCP full scale, restored")

    b = FakeBoard(pack_mv=30000, id_gain=1.0)
    ra = args("ramp", "auto", [150, 1500], rate=3000, hold=0.1, settle=0.15, trace="ccm", trace_at=0.1)
    ra.seconds = ramp_times(ra)["end_s"]
    r = execute_ramp(ra, b, tmp, ev)
    sm = r.get("summary", {})
    ent, ex = sm.get("entries", []), sm.get("exits", [])
    check("stopped" not in r and r["run"]["ok"], f"ramp: ran ({r.get('stopped')})")
    check(len(ent) == 1 and 380 <= ent[0]["ilim_ma"] <= 520 and len(ex) == 1 and ex[0]["ilim_ma"] < 220,
          f"ramp: one CCM entry near 400 mA up, one exit under 200 mA down ({ent}, {ex})")
    check(sm.get("max_ilim_ma") == 1500 and {"up", "down"} <= set(sm.get("segments", {})), "ramp: reached the top, both legs")
    check(r["counts"] == {"ccm_entries": 1, "ccm_exits": 1} and r["ramp_rate_after"] == 0
          and b.mem["regulator_ccm_ilim_ramp_ma_s"] == 0 and b.mem["regulator_ccm_ocp_ma"] == 5000
          and b.mem["regulator_ccm_enable"] == 1 and b.state == "IDLE", "ramp: counters, rate cleared, SW_OCP / enable restored")
    check((tmp / "captures" / "trace_ramp_auto_150-1500_3000.csv").exists(), "ramp: trace read")
    b = FakeBoard(pack_mv=30000, new_fw=False)
    r = execute_ramp(ra, b, tmp, ev)
    check("limit ramp" in r.get("stopped", "") and not any(c[0] == "start" for c in b.log),
          "ramp on the old firmware: refused before any START")

    shutil.rmtree(tmp, ignore_errors=True)
    print("selftest:", "all passed" if not fails else f"{len(fails)} FAILED")
    return 1 if fails else 0


# ---------------------------------------------------------------- main
def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("phase", nargs="?", choices=["resistor", "pack", "ramp"])
    p.add_argument("steps", nargs="*", type=int,
                   help="resistor: ceilings in mV; pack: current limits in mA; ramp: FROM TO in mA (default 150 5000)")
    p.add_argument("--ccm", choices=list(CCM_ENABLE),
                   help="dcm (regulator_ccm_enable 0), auto (1) or forced CCM (2); default dcm, ramp auto")
    p.add_argument("--rate", type=int, default=100, help="ramp: mA/s, 10..2000 (default 100)")
    p.add_argument("--hold", type=float, default=5.0, help="ramp: seconds at TO (default 5)")
    p.add_argument("--go", action="store_true", help="run it; without this the plan is printed and nothing is touched")
    p.add_argument("--seconds", type=float, help="per run (default 12 resistor, 20 pack)")
    p.add_argument("--settle", type=float, default=5.0, help="seconds before the samples count (default 5)")
    p.add_argument("--idle", type=float, default=8.0, help="seconds between runs (default 8)")
    p.add_argument("--ilim", type=int, default=2000, help="resistor phase: the backstop limit in mA (default 2000)")
    p.add_argument("--r-ohm", type=float, default=25.3, help="resistor phase: the resistor (default 25.3)")
    p.add_argument("--cells", type=int, default=8, help="pack phase: cells in series (default 8)")
    p.add_argument("--path-mohm", type=int, default=170,
                   help="pack phase: resistance from the converter to the pack's EMF, for the margin (default 170)")
    p.add_argument("--above-mv", type=int, help="pack phase: a fixed margin over the pack read (300..1500)")
    p.add_argument("--scope", action="store_true", help="capture CH2 V_in / CH4 V_out once per step (trace medians)")
    p.add_argument("--scope-tdiv", type=float, default=20e-6, metavar="S",
                   help="scope seconds per division for the capture (default 20e-6: 240 us at 8 ns; 100e-6 shows a "
                        "sub-ms swing at 40 ns, still 125 points per switching period)")
    p.add_argument("--trace", metavar="TRIGGER[:MV]",
                   help="arm the 20 kHz regulator trace in each step: now, error:MV (|setpoint - V_out| over MV), "
                        "ceiling (DCM window limit, not the peak cap), ccm; read after the stop into captures/")
    p.add_argument("--trace-at", type=float, help="seconds into the run to arm it (default 1; ramp: 0.5 s before it starts)")
    p.add_argument("--trace-post", type=int, default=1536, help="samples after the trigger, 50 us each (default 1536 "
                                                                "of 2048: 77 ms after, 25 ms before)")
    p.add_argument("--no-prompt", action="store_true", help="do not ask for the ammeter reading")
    p.add_argument("--selftest", action="store_true", help="run both phases against a simulated board")
    a = p.parse_args()
    if a.selftest:
        return selftest()
    if not a.phase:
        p.error("phase: resistor, pack or ramp")
    if a.ccm is None:
        a.ccm = "auto" if a.phase == "ramp" else "dcm"
    if a.phase == "ramp":
        a.steps = a.steps or list(RAMP_DEFAULT_MA)
        if len(a.steps) != 2 or not 100 <= a.steps[0] < a.steps[1] <= ILIM_MAX_MA:
            p.error(f"ramp: FROM TO, 100 <= FROM < TO <= {ILIM_MAX_MA} mA")
        if not 10 <= a.rate <= 2000:
            p.error("--rate: 10..2000 mA/s")
        if not 0 <= a.hold <= 60:
            p.error("--hold: 0..60 s")
        if a.scope:
            p.error("--scope is not supported in the ramp phase")
        if a.seconds is not None:
            p.error("ramp: the length follows from FROM, TO, --rate, --hold and --settle; no --seconds")
        a.seconds = ramp_times(a)["end_s"]
        if a.trace is None:
            a.trace = "ccm"
        if a.trace_at is None:
            a.trace_at = max(0.5, a.settle - 0.5)
    if a.trace_at is None:
        a.trace_at = 1.0
    if not a.steps:
        a.steps = RES_DEFAULT_MV if a.phase == "resistor" else PACK_DEFAULT_MA[a.ccm]
    if a.seconds is None:
        a.seconds = 12.0 if a.phase == "resistor" else 20.0
    if a.settle >= a.seconds:
        p.error("--settle must be under --seconds")
    if a.trace:
        trig, _, thr = a.trace.partition(":")
        if trig not in ("now", "error", "ceiling", "ccm"):
            p.error("--trace must be now, error:MV, ceiling or ccm")
        if trig == "error" and not thr.isdigit():
            p.error("--trace error needs a threshold: error:800")
        if not 0 <= a.trace_post < 2048:
            p.error("--trace-post must be 0..2047")
        if a.trace_at >= a.seconds:
            p.error("--trace-at must be under --seconds")
    if a.phase == "resistor":
        if not all(5000 <= s <= CEILING_MAX_MV for s in a.steps):
            p.error(f"resistor ceilings: 5000..{CEILING_MAX_MV} mV")
        if not 0 < a.ilim <= ILIM_MAX_MA:
            p.error(f"--ilim: 1..{ILIM_MAX_MA}")
    elif a.phase == "pack":
        if not all(0 < s <= ILIM_MAX_MA for s in a.steps):
            p.error(f"pack limits: 1..{ILIM_MAX_MA} mA")
        if a.above_mv is not None and not 300 <= a.above_mv <= 1500:
            p.error("--above-mv: 300..1500")
        if not 2 <= a.cells <= 8:
            p.error("--cells: 2..8")
    if a.phase != "resistor" and a.above_mv is not None and not 300 <= a.above_mv <= 1500:
        p.error("--above-mv: 300..1500")
    print(ramp_plan_text(a) if a.phase == "ramp" else plan_text(a))
    if not a.go:
        print("\nNothing was touched.  Add --go to run it" + (" (it touches the pack: on Dan's go for this run)."
                                                              if a.phase != "resistor" else "."))
        return 0

    interrupted = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: interrupted.set())
    signal.signal(signal.SIGTERM, lambda *_: interrupted.set())
    from bringup.config import REPO_ROOT
    out_dir = Path(REPO_ROOT) / "bench/results/2026-11-12"
    b = Board()
    tap = None
    try:
        if a.scope:
            (out_dir / "captures").mkdir(parents=True, exist_ok=True)
            tap = ScopeTap(b.cfg, out_dir / "captures", 30.0, tdiv=a.scope_tdiv)
        r = execute_ramp(a, b, out_dir, interrupted) if a.phase == "ramp" else execute(a, b, out_dir, interrupted, tap)
    finally:
        if tap is not None:
            tap.close()
    return 1 if r.get("stopped") or r.get("interrupted") else 0


if __name__ == "__main__":
    sys.exit(main())
