"""Three-board bench: G474 source (SWD), G431 sink (VCP), QT Py buck load (I2C via the sink).

``status`` snapshots all three.  ``sweep`` steps contracts and power targets
and logs one CSV row per point:

    for each voltage:  load off -> req <mV> <mA> (until contract)
        for each power: load p <mW> -> dwell -> sample sink, load, source
    load off

A sweep stops at the first sign of trouble: a load fault, a sink Hard Reset
or EPR failure, a TCPP02 fault, or a source regulator FAULT, and always
ends with ``load off``.

Two ST-LINKs are attached, so set ``[probe].serial`` (G474) and ``[sink]``
(G431 VCP) in bringup.local.toml.
"""

from __future__ import annotations

import csv
import time

from . import pd, regulator, sink
from .config import ToolError, out_file, rel
from .serialmon import list_ports


def devices(cfg: dict) -> dict:
    """Which attached ST-LINK is the source (G474) and which the sink (G431).

    Roles come from bringup.local.toml: [probe].serial (source, used by
    flash/mem/pd/regulator) and [sink].stlink_serial or [sink].port (sink VCP).
    One ST-LINK and nothing set is fine (single-board work); two need both."""
    stlinks = [p for p in list_ports()["ports"] if p["stlink"]]
    src_sn = cfg.get("probe", {}).get("serial", "")
    sc = cfg.get("sink", {})
    sink_sn, sink_port = sc.get("stlink_serial", ""), sc.get("port", "")
    mapped = []
    for p in stlinks:
        role = None
        if src_sn and p["serial"] == src_sn:
            role = "source (G474)"
        elif (sink_sn and p["serial"] == sink_sn) or (sink_port and p["device"] == sink_port):
            role = "sink (G431)"
        mapped.append({"device": p["device"], "serial": p["serial"], "role": role})
    problems = []
    if len(stlinks) >= 2:
        if not src_sn:
            problems.append("[probe].serial unset: flash/mem/pd/regulator could talk to either board")
        if not (sink_sn or sink_port):
            problems.append("[sink].stlink_serial unset: bu sink/bench cannot pick the G431's VCP")
    if src_sn and not any(p["serial"] == src_sn for p in stlinks):
        problems.append(f"[probe].serial {src_sn} is not attached")
    if sink_sn and not any(p["serial"] == sink_sn for p in stlinks):
        problems.append(f"[sink].stlink_serial {sink_sn} is not attached")
    if src_sn and src_sn == sink_sn:
        problems.append("source and sink are set to the same ST-LINK")
    if problems:
        raise ToolError("bench ST-LINKs not mapped", problems=problems, stlinks=mapped,
                        hint="copy tools/bringup/bringup.local.toml.example to bringup.local.toml "
                             "and fill in the serials shown here")
    return {"stlinks": mapped}


def _one(rows: list[dict], kind: str) -> dict:
    return next((r for r in rows if r.get("kind") == kind), {})


def sink_status(cfg: dict) -> dict:
    return _one(sink.exchange(cfg, "status")["reply"], "STATUS")


def load_status(cfg: dict) -> dict:
    return _one(sink.exchange(cfg, "load status")["reply"], "LOAD")


def _source(cfg: dict) -> dict:
    """G474 over SWD; errors become a field, not an exception."""
    out: dict = {}
    try:
        r = regulator.status(cfg)
        out["regulator"] = {"state": r.get("state"), "fault_source": r.get("fault_source"),
                            "adc": r.get("adc", {})}
    except ToolError as e:
        out["regulator_error"] = str(e)
    try:
        p = pd.status(cfg)
        out["pd"] = {"contract": p.get("contract"), "counters": p.get("counters"), "epr": p.get("epr")}
    except ToolError as e:
        out["pd_error"] = str(e)
    return out


def preflight(s: dict, ld: dict) -> list[str]:
    """Reasons the bench is not ready to sink power."""
    problems = []
    if s.get("attached") in (None, "none"):
        problems.append("sink not attached (CC): check the cable and the TCPP02 (tcpp_ok)")
    if not s.get("tcpp_ok"):
        problems.append("TCPP02 not in Normal mode: I2C wiring/pull-ups or ENABLE (PC8)")
    if s.get("tcpp_fault"):
        problems.append(f"TCPP02 fault flags {s.get('tcpp_flags')}")
    if not ld.get("online"):
        problems.append("load offline: STEMMA QT wiring (SDA PB9, SCL PB8, GND) or QT Py firmware")
    elif ld.get("state") == "no_power_stage":
        problems.append("load built without the power-stage feature (telemetry only)")
    elif ld.get("state") == "fault":
        problems.append(f"load fault {ld.get('fault')}: `bu sink load clear`")
    return problems


def status(cfg: dict, swd: bool = True) -> dict:
    s = sink_status(cfg)
    ld = load_status(cfg)
    out = {"ok": True, "sink": s, "load": ld, "ready": preflight(s, ld) or "ready"}
    if swd:
        out["source"] = _source(cfg)
    return out


FIELDS = ["t_s", "contract_target_mv", "p_target_mw", "contract_mv", "contract_ma", "sink_vbus_mv",
          "load_state", "load_fault", "load_vin_mv", "load_vout_mv", "load_iout_ma", "load_pout_mw",
          "load_duty_pm", "load_temp_mv", "src_state", "src_vin_mv", "src_vout_mv", "src_iout_ma",
          "src_transition_ms", "sink_hard_resets", "note"]


def _row(t0: float, mv: int, p_mw: int, s: dict, ld: dict, src: dict, note: str = "") -> dict:
    reg = src.get("regulator", {})
    adc = reg.get("adc", {}) or {}
    contract = (src.get("pd") or {}).get("contract") or {}
    return {
        "t_s": round(time.monotonic() - t0, 2), "contract_target_mv": mv, "p_target_mw": p_mw,
        "contract_mv": s.get("contract_mv"), "contract_ma": s.get("contract_ma"), "sink_vbus_mv": s.get("vbus_mv"),
        "load_state": ld.get("state"), "load_fault": ld.get("fault"), "load_vin_mv": ld.get("vin_mv"),
        "load_vout_mv": ld.get("vout_mv"), "load_iout_ma": ld.get("iout_ma"), "load_pout_mw": ld.get("pout_mw"),
        "load_duty_pm": ld.get("duty_pm"), "load_temp_mv": ld.get("temp_mv"),
        "src_state": reg.get("state"), "src_vin_mv": adc.get("v_in_mv"), "src_vout_mv": adc.get("v_out_mv"),
        "src_iout_ma": adc.get("i_out_ma"), "src_transition_ms": contract.get("last_transition_ms"),
        "sink_hard_resets": s.get("hard_resets"), "note": note,
    }


def _trouble(s: dict, ld: dict, src: dict, hard_resets0: int) -> str | None:
    if ld.get("state") == "fault":
        return f"load fault {ld.get('fault')}"
    if (s.get("hard_resets") or 0) > hard_resets0:
        return "Hard Reset seen by the sink"
    if s.get("tcpp_fault"):
        return f"TCPP02 fault flags {s.get('tcpp_flags')}"
    if (src.get("regulator") or {}).get("state") == "FAULT":
        return f"source regulator FAULT {src['regulator'].get('fault_source')}"
    if s.get("attached") in (None, "none"):
        return "sink detached"
    return None


def sweep(cfg: dict, voltages_mv: list[int], powers_mw: list[int], ma: int = 500, dwell: float = 2.0,
          swd: bool = True, epr_w: int | None = None, allow_no_power_stage: bool = False) -> dict:
    s0 = sink_status(cfg)
    ld0 = load_status(cfg)
    problems = [p for p in preflight(s0, ld0)
                if not (allow_no_power_stage and "power-stage" in p)]
    if problems:
        raise ToolError("bench not ready", problems=problems, sink=s0, load=ld0)
    if swd and not cfg.get("probe", {}).get("serial"):
        raise ToolError("two ST-LINKs on the bench: set [probe].serial to the G474's ST-LINK "
                        "(or pass --no-swd)")

    f = out_file(cfg, "bench_sweep", "csv")
    rows: list[dict] = []
    stop: str | None = None
    t0 = time.monotonic()
    hard_resets0 = s0.get("hard_resets") or 0
    try:
        if epr_w:
            sink.exchange(cfg, "load off")
            r = sink.exchange(cfg, f"epr {epr_w}", timeout=5.0, wait=3.0)
            failed = [e for e in r["events"] if e.get("event") == "epr_enter_failed"]
            if not r["ok"] or failed:
                stop = f"EPR entry failed: {failed[0].get('reason') if failed else r.get('error')}"
        for mv in ([] if stop else voltages_mv):
            sink.exchange(cfg, "load off")
            r = sink.exchange(cfg, f"req {mv} {ma}", timeout=4.0, until_event="contract")
            if not r["ok"]:
                stop = f"no contract at {mv} mV: {r.get('error')}"
                break
            for p_mw in powers_mw:
                sink.exchange(cfg, f"load p {p_mw}")
                time.sleep(dwell)
                s = sink_status(cfg)
                ld = load_status(cfg)
                src = _source(cfg) if swd else {}
                trouble = _trouble(s, ld, src, hard_resets0)
                rows.append(_row(t0, mv, p_mw, s, ld, src, trouble or ""))
                if trouble:
                    stop = trouble
                    break
            if stop:
                break
    finally:
        try:
            sink.exchange(cfg, "load off")
        except ToolError:
            pass
        with f.open("w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)
    return {"ok": stop is None, "points": len(rows), "file": rel(f), "stopped": stop,
            "rows": rows[-10:], **({"error": stop} if stop else {})}
