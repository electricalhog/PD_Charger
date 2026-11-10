"""Battery-load build (branch claude/lipo-load, plans/lipo-load.md): a LiPo on VBUS as a power sink.

The firmware has no USB.  START (the regulator mailbox, arg = the ceiling in mV) reads the pack with the output
switch closed and every FET off, starts at the pack voltage with the switch open, closes it once V_out is there and
ramps to the ceiling; ``regulator_ccm_ilim_ma`` is the current.  RUNNING stops when ``regulator_battery_keepalive``
has not changed for BATTERY_KEEPALIVE_MS (2 s), so ``run`` writes it every sample and stops the regulator on the
way out, also on SIGTERM or Ctrl-C.
"""

from __future__ import annotations

import json
import signal
import time

from . import probe, regulator
from .config import ToolError, out_file, rel

_VARS = {"regulator_battery_ceiling_mv": "ceiling_mv", "regulator_battery_pack_mv": "pack_mv",
         "regulator_battery_keepalive": "keepalive", "regulator_battery_vin_start_mv": "vin_start_mv",
         "regulator_battery_keepalive_stops": "keepalive_stops", "regulator_ccm_ilim_ma": "ilim_ma"}
ILIM_MAX_MA = 5000   # ID_MON reads to 5.5 A (R6 12 mOhm)


def _battery_syms(cfg: dict) -> dict:
    syms = probe.symbols(cfg)
    if "regulator_battery_ceiling_mv" not in syms:
        raise ToolError("the flashed ELF is not the battery-load build (no regulator_battery_* symbols)")
    return syms


def _read(cfg: dict, syms: dict) -> dict:
    names = list(_VARS)
    data = probe.read_regions(cfg, [syms[n] for n in names])
    return {_VARS[n]: int.from_bytes(d, "little") for n, d in zip(names, data)}


def _row(s: dict, t0: float) -> dict:
    c, adc = s.get("control", {}), s.get("adc", {})
    return {"t": round(time.monotonic() - t0, 2), "state": s["state"], "fault": s["fault_source"],
            "mode": c.get("mode"), "ccm": c.get("ccm_active"), "lim": c.get("ccm_limiting"),
            "target_mv": c.get("target_mv"), "commanded_mv": c.get("commanded_mv"),
            "v_out_mv": adc.get("v_out_mv"), "v_in_mv": adc.get("v_in_mv"),
            "i_out_ma": c.get("ccm_i_ma"), "i_in_ma": c.get("i_in_filt_ma"),   # IS_MON after its calibration
            "switch": s["lines"]["OUTPUT_EN_PC8"], "dis": s["lines"]["OUTPUT_DIS_PC9"]}


def status(cfg: dict) -> dict:
    syms = _battery_syms(cfg)
    s = regulator.status(cfg)
    return {"ok": True, "battery": _read(cfg, syms), "regulator": _row(s, time.monotonic()),
            "diagnosis": s["diagnosis"]}


def run(cfg: dict, ceiling_mv: int, ilim_ma: int, seconds: float, interval: float = 0.2) -> dict:
    syms = _battery_syms(cfg)
    if not 0 < ilim_ma <= ILIM_MAX_MA:
        raise ToolError(f"--ilim-ma must be 1..{ILIM_MAX_MA}")
    if regulator.status(cfg)["state"] != "IDLE":
        raise ToolError("regulator not IDLE: stop it (bu regulator stop) or clear the fault first")
    probe.mem_write(cfg, "regulator_ccm_ilim_ma", str(ilim_ma), "u16")

    stopped = False

    def stop(*_):
        nonlocal stopped
        stopped = True

    old_term = signal.signal(signal.SIGTERM, stop)
    rows, ka = [], _read(cfg, syms)["keepalive"]
    t0 = time.monotonic()
    start = None
    try:
        start = regulator.command(cfg, "start", 5.0, mv=ceiling_mv)
        if not start.get("ok"):
            return {"ok": False, "result": start.get("result"), "explanation": start.get("explanation"),
                    "battery": _read(cfg, syms)}
        next_t = time.monotonic()
        while not stopped and time.monotonic() - t0 < seconds:
            ka = (ka + 1) & 0xFFFFFFFF
            probe.mem_write(cfg, "regulator_battery_keepalive", str(ka), "u32")
            rows.append(_row(regulator.status(cfg), t0))
            if rows[-1]["state"] != "RUNNING":
                break
            next_t += interval
            try:
                time.sleep(max(0.0, next_t - time.monotonic()))
            except KeyboardInterrupt:
                stopped = True
    finally:
        try:
            if regulator.status(cfg)["state"] == "RUNNING":
                regulator.command(cfg, "stop", 3.0)
        finally:
            # The log goes out first: a SIGTERM through the uv wrapper can end the process before the summary.
            f = out_file(cfg, "battery_run", "json")
            f.write_text(json.dumps(rows))
            signal.signal(signal.SIGTERM, old_term)

    end = regulator.status(cfg)
    running = [r for r in rows if r["state"] == "RUNNING"]
    tail = running[-5:]
    mean = lambda k: round(sum(r[k] for r in tail) / len(tail)) if tail and all(r[k] is not None for r in tail) else None
    return {"ok": bool(running) and rows[-1]["state"] == "RUNNING",   # ran to the end (or Ctrl-C) without a fault
            "battery": _read(cfg, syms), "samples": len(rows), "stopped_early": stopped,
            "ended": rows[-1]["state"] if rows else None, "end_state": end["state"], "end_fault": end["fault_source"],
            "tail_mean": {k: mean(k) for k in ("v_out_mv", "i_out_ma", "v_in_mv", "i_in_ma")},
            "limiting_frac": round(sum(1 for r in running if r["lim"]) / len(running), 2) if running else None,
            "log": rel(f)}
