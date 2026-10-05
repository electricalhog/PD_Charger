"""Regulator / input-protection status and fault recovery over SWD.

The input power path is guarded by an ADM1270 (U5) hot-swap controller, set
up in latch-off mode. Its two open-drain outputs reach the MCU as HRTIM fault
inputs:

  VS_GOOD = ADM1270 PWRGD  -> PA12 / HRTIM FLT1  (high: VS above threshold)
  IS_GOOD = ADM1270 ~FAULT -> PA15 / HRTIM FLT2  (low: over-current latch)

After a current trip the ADM1270 only restarts once TIMER_OFF has recharged
AND ENABLE (INPUT_EN, PC7) goes low -> high. That timed sequence runs in
firmware (regulator_clear_fault); this module asks for it through the
``regulator_debug`` mailbox and reads back the result.

Board values come from the schematic netlist and the ADM1270 datasheet (Rev. A).
"""

from __future__ import annotations

import struct
import time

from . import probe
from .config import ToolError

# ------------------------------------------------------------------ board facts

PROTECTION = {
    "device": "ADM1270ACPZ (U5), latch-off mode (~FAULT not tied to ENABLE)",
    "current_limit_a": 25.0,        # V_SENSECL 50 mV (ISET=VCAP 3.6 V > 2.65 V) / R25 2 mOhm
    "current_trip_a": 24.5,         # V_CB = 50 mV - 1 mV offset: TIMER starts charging
    "severe_overcurrent_a": 50.0,   # V_SENSEOC 100 mV, ~2 us response
    "foldback_limit_at_vs0_a": 5.0,  # 10 mV at FLB = 0 V; FLB = VS * 33.3k/133.3k
    "foldback_inactive_above_vs_v": 4.4,  # FLB > 1.1 V
    "trip_delay_ms": 2.0,           # 2.0 V * C_TIMER 20 nF / 20 uA
    "cooldown_ms": 100.0,           # 2.0 V * C_TIMER_OFF 50 nF / 1 uA (firmware waits 150)
    "vs_good_rising_v": 4.60,       # FB_PG 1.0 V * (100k + 4.3k + 29k) / 29k
    "vs_good_falling_v": 4.46,      # 30 mV FB_PG hysteresis
    "vin_overvoltage_v": 57.7,      # OV 1.0 V * (68k + 1.2k) / 1.2k
    "vin_undervoltage": "UV pin tied to VCAP: disabled (only VCC UVLO 3.4 V rising / 3.0 V falling)",
    "pullups": "VS_GOOD/IS_GOOD 100k to power-board +3V3 (from VIN via Q7); floating if VIN absent",
}

STATES = {0: "INIT", 1: "IDLE", 2: "RUNNING", 3: "FAULT"}
FAULT_BITS = {
    0x01: ("HW_FLT1", "VS_GOOD low while the input path was on: VIN missing or low (VS < 4.6 V), "
                      "VIN over-voltage (> 57.7 V), or VS dragged down"),
    0x02: ("HW_FLT2", "ADM1270 over-current trip: > 24.5 A for 2 ms, or > 50 A instantly"),
    0x04: ("SW_OVP", "software output over-voltage"),
    0x08: ("SW_UVP", "software output under-voltage"),
    0x10: ("SW_VIN_RANGE", "V_in out of range for the selected mode"),
    0x20: ("SW_BACKSTOP", "too many consecutive cycle-by-cycle backstop events"),
}
CMD = {"clear-fault": 1, "stop": 2, "bench-pwm-on": 3, "bench-pwm-off": 4, "set-voltage": 5, "start": 6,
       "snapshot": 7}
ARG_CMDS = ("set-voltage", "start")   # take a millivolt argument in regulator_debug.arg
MODES = {0: "BUCK", 1: "BOOST", 2: "BUCK_BOOST"}
SET_RESULTS = {
    0: ("OK", ""),
    1: ("OUT_OF_RANGE", "outside the firmware's SETPOINT_MIN_MV..SETPOINT_MAX_MV"),
    2: ("NOT_IDLE", "start only from IDLE: stop the regulator or clear the fault first"),
    3: ("START_FAILED", "regulator_start() did not reach RUNNING: see status (fault source, input lines)"),
    4: ("PD_OWNS_VBUS", "firmware built with PD_VBUS_PATH_CHARGER: the output is the Type-C VBUS, "
                        "so only a USB-PD contract sets it; rebuild with -DPD_VBUS_PATH_CHARGER=OFF "
                        "for dummy-load runs with the receptacle disconnected"),
}
BENCH_RESULTS = {
    0: ("OK", ""),
    1: ("NOT_IDLE", "bench PWM is only allowed from IDLE (stop the regulator / clear the fault first)"),
    2: ("INPUT_PATH_ON", "INPUT_EN is high: bench PWM refuses while the input path could power the stage"),
    3: ("FAULT_LINE_LOW", "VS_GOOD or IS_GOOD is low, so the HRTIM hardware would hold the outputs off"),
}
CLEAR_RESULTS = {
    0: ("OK", "FAULT released, state IDLE with the input path off. Starting again is a separate step."),
    1: ("NOT_IN_FAULT", "nothing to clear"),
    2: ("INPUT_NOT_GOOD", "VS_GOOD did not rise within 50 ms of INPUT_EN: check VIN is present (4-57 V), "
                          "below the 57.7 V OV trip, and that VS isn't heavily loaded. Input path left off."),
    3: ("INPUT_OVERCURRENT", "ADM1270 tripped again while charging VS with the power stage off: likely a short "
                             "or heavy load on VS. Inspect the hardware before retrying. Input path left off."),
}
UNKNOWN_CMD = 0xFFFFFFFF

# Registers (RM0440 / stm32g474xx.h)
GPIOA_IDR = 0x48000010
GPIOC_ODR = 0x48000814
HRTIM_ISR = 0x40016B88  # HRTIM1 common block (0x40016B80) + 0x08; ICR, IER, OENR follow
COOLDOWN_FW_MS = 150

_VARS = ["regulator_state", "regulator_last_fault_source", "regulator_fault_tick_ms", "regulator_debug", "uwTick",
         "adc_measurements", "regulator_bench_pwm_active"]
# Optional control telemetry: read when the firmware has the symbol.
_CONTROL = {"regulator_mode": "mode", "target_voltage_mv": "target_mv", "regulator_commanded_mv": "commanded_mv",
            "regulator_vin_filt_mv": "vin_filtered_mv", "regulator_on_time_ns": "on_time_ns",
            "regulator_pulses_skipped": "pulses_skipped"}


def _u(data: bytes) -> int:
    return int.from_bytes(data, "little")


def read_raw(cfg: dict) -> dict:
    syms = probe.symbols(cfg)
    missing = [v for v in _VARS if v not in syms]
    if "regulator_debug" in missing:
        raise ToolError("firmware has no regulator_debug mailbox; flash a build from the agentic-bringup-tools branch",
                        missing=missing)
    names = [v for v in list(_VARS) + list(_CONTROL) if v in syms]
    regions = [syms[v] for v in names] + [(GPIOA_IDR, 4), (GPIOC_ODR, 4), (HRTIM_ISR, 16)]
    data = probe.read_regions(cfg, regions)
    raw = dict(zip(names, data[:len(names)]))
    raw["gpioa_idr"], raw["gpioc_odr"], raw["hrtim"] = data[len(names):]
    return raw


def decode(raw: dict) -> dict:
    mbox = struct.unpack_from("<4I", raw["regulator_debug"])   # newer firmware appends arg
    idr, odr = _u(raw["gpioa_idr"]), _u(raw["gpioc_odr"])
    isr, _icr, ier, oenr = struct.unpack("<4I", raw["hrtim"])
    tick = _u(raw["uwTick"])
    src = _u(raw["regulator_last_fault_source"])
    s = {
        "state": STATES.get(_u(raw["regulator_state"]), _u(raw["regulator_state"])),
        "fault_source": [FAULT_BITS[b][0] for b in FAULT_BITS if src & b],
        "fault_age_ms": (tick - _u(raw["regulator_fault_tick_ms"])) & 0xFFFFFFFF,
        "lines": {
            "VS_GOOD_PA12": (idr >> 12) & 1,
            "IS_GOOD_PA15": (idr >> 15) & 1,
            "INPUT_EN_PC7": (odr >> 7) & 1,
            "OUTPUT_EN_PC8": (odr >> 8) & 1,
            "OUTPUT_DIS_PC9": (odr >> 9) & 1,
        },
        "hrtim": {
            "flag_FLT1": bool(isr & 1), "flag_FLT2": bool(isr & 2),
            "irq_armed_FLT1": bool(ier & 1), "irq_armed_FLT2": bool(ier & 2),
            "outputs_enabled": [n for i, n in enumerate(["TA1", "TA2", "TB1", "TB2"]) if oenr & (1 << i)],
        },
        "bench_pwm": bool(_u(raw["regulator_bench_pwm_active"])) if "regulator_bench_pwm_active" in raw else None,
        "mailbox": {"request": mbox[0], "result": mbox[1], "done_count": mbox[2], "last_cmd": mbox[3]},
        "uptime_ms": tick,
    }
    ctl = {}
    for sym, key in _CONTROL.items():
        if sym in raw:
            v = _u(raw[sym])
            ctl[key] = MODES.get(v, v) if key == "mode" else v
    if ctl:
        s["control"] = ctl
    if "adc_measurements" in raw:
        v_out, v_in, i_l, i_out, i_in = struct.unpack("<5I", raw["adc_measurements"])
        s["adc"] = {"v_in_mv": v_in, "v_out_mv": v_out, "i_inductor_ma": i_l, "i_out_ma": i_out, "i_in_ma": i_in,
                    "note": "updated by the PID ISR, so stale unless RUNNING"}
    s["diagnosis"] = diagnose(s)
    return s


def diagnose(s: dict) -> list[str]:
    d, L, h = [], s["lines"], s["hrtim"]
    state = s["state"]
    if state == "FAULT":
        src = s["fault_source"] or ["NONE"]
        for name in src:
            d.append(f"FAULT latched: {name}"
                     + next((f" - {txt}" for b, (n, txt) in FAULT_BITS.items() if n == name), ""))
        left = COOLDOWN_FW_MS - s["fault_age_ms"]
        if left > 0:
            d.append(f"ADM1270 cool-down: firmware will wait another {left} ms on clear-fault")
        if s["mailbox"]["last_cmd"] == CMD["clear-fault"] and s["mailbox"]["result"] == 3:
            d.append("last clear-fault attempt found a repeat over-current: inspect hardware before retrying")
        d.append("recover with: bu regulator clear-fault")
    if L["INPUT_EN_PC7"] == 0 and L["VS_GOOD_PA12"] == 0:
        d.append("VS_GOOD low with INPUT_EN off: expected (VS is switched off)")
    if L["INPUT_EN_PC7"] == 0 and L["IS_GOOD_PA15"] == 0:
        d.append("IS_GOOD low with INPUT_EN off: ADM1270 still latched, or VIN absent (pull-ups are powered from VIN), "
                 "or the power board is disconnected (lines float)")
    if L["INPUT_EN_PC7"] == 1 and L["IS_GOOD_PA15"] == 0:
        d.append("ADM1270 over-current latch active with INPUT_EN on")
    if L["INPUT_EN_PC7"] == 1 and L["VS_GOOD_PA12"] == 0 and L["IS_GOOD_PA15"] == 1:
        d.append("INPUT_EN on but VS_GOOD low: VIN missing/low, VIN over-voltage, or VS still ramping")
    if state == "RUNNING":
        if not (h["irq_armed_FLT1"] and h["irq_armed_FLT2"]):
            d.append("WARNING: RUNNING with the HRTIM fault IRQ not armed (firmware bug)")
        if not L["INPUT_EN_PC7"]:
            d.append("WARNING: RUNNING with INPUT_EN off")
    elif h["outputs_enabled"] and s.get("bench_pwm") and not L["INPUT_EN_PC7"]:
        d.append(f"bench PWM active on {h['outputs_enabled']} (open-loop, input path off)")
    elif h["outputs_enabled"]:
        d.append(f"DANGER: HRTIM outputs {h['outputs_enabled']} enabled while state is {state}")
    if state in ("IDLE", "INIT") and not d:
        d.append("idle: switching off, input path off")
    return d


DEBUG_RECORD = struct.Struct("<IIIIHBBi")   # v_out, v_in, i_l, i_out, dac, state, mode, error (debug_log.h)
DEBUG_RECORDS = 512


def debug_log_stats(cfg: dict) -> dict:
    """Summary of the firmware's 512-cycle debug_log ring (25.6 ms at 20 kHz)."""
    addr, size = probe.symbols(cfg)["debug_log"]
    (raw,) = probe.read_regions(cfg, [(addr, DEBUG_RECORD.size * DEBUG_RECORDS)])
    recs = [DEBUG_RECORD.unpack_from(raw, i * DEBUG_RECORD.size) for i in range(DEBUG_RECORDS)]
    vo = [r[0] for r in recs]
    io = [r[3] for r in recs]
    mean = sum(vo) / len(vo)
    return {"vout_mv": round(mean), "vout_sd_mv": round((sum((v - mean) ** 2 for v in vo) / len(vo)) ** 0.5),
            "vout_min_mv": min(vo), "vout_max_mv": max(vo), "iout_ma": round(sum(io) / len(io)),
            "dac_mean": round(sum(r[4] for r in recs) / len(recs)),
            "modes": sorted({MODES.get(r[6], r[6]) for r in recs})}


def sweep(cfg: dict, targets_mv: list[int], dwell_s: float = 1.0, settle_timeout_s: float = 5.0,
          scope_source: str | None = None) -> dict:
    """set-voltage through a list of targets; after the slew and a dwell, record control telemetry,
    debug_log statistics and (optionally) the scope's VAVG/VPP on one channel. Stops at the first fault."""
    sc = None
    if scope_source:
        from .scope import Scope
        sc = Scope(cfg)
    rows = []
    for mv in targets_mv:
        res = command(cfg, "set-voltage", mv=mv)
        if not res.get("ok"):
            return {"ok": False, "error": f"set-voltage {mv} failed", "detail": res, "rows": rows}
        t0 = time.monotonic()
        while True:
            s = decode(read_raw(cfg))
            if s["state"] != "RUNNING" or s.get("control", {}).get("commanded_mv") == mv \
                    or time.monotonic() - t0 > settle_timeout_s:
                break
            time.sleep(0.2)
        time.sleep(dwell_s)
        s = decode(read_raw(cfg))
        row = {"target_mv": mv, "state": s["state"], **s.get("control", {})}
        if s["state"] != "RUNNING":
            row["fault_source"] = s["fault_source"]
            syms = probe.symbols(cfg)
            if "regulator_fault_snapshot" in syms:
                (snap,) = probe.read_regions(cfg, [syms["regulator_fault_snapshot"]])
                row["fault_snapshot"] = list(struct.unpack_from("<6I", snap))
            rows.append(row)
            return {"ok": False, "error": f"regulator left RUNNING at {mv} mV", "rows": rows}
        row.update(debug_log_stats(cfg))
        if sc:
            m = sc.measure([scope_source], ["VAVG", "VPP"])["measurements"]
            row.update({f"scope_{k.lower()}": v for k, v in next(iter(m.values())).items()})
        rows.append(row)
    return {"ok": True, "rows": rows}


def status(cfg: dict) -> dict:
    return {"ok": True, **decode(read_raw(cfg)), "protection": PROTECTION}


def command(cfg: dict, name: str, timeout: float = 3.0, force: bool = False, mv: int | None = None) -> dict:
    before = decode(read_raw(cfg))
    mb = before["mailbox"]
    if mb["request"]:
        raise ToolError("mailbox busy: firmware hasn't serviced the previous request (is the default task running?)",
                        mailbox=mb)
    if name == "clear-fault":
        if before["state"] != "FAULT":
            return {"ok": True, "result": "NOT_IN_FAULT", "explanation": CLEAR_RESULTS[1][1], "status": before}
        if mb["last_cmd"] == CMD["clear-fault"] and mb["result"] == 3 and not force:
            raise ToolError("previous clear-fault hit a repeat ADM1270 over-current; refusing to re-energize VS again "
                            "without --force (a short on VS would be hit every attempt)", status=before)

    addr, size = probe.symbols(cfg)["regulator_debug"]
    if name in ARG_CMDS:
        if mv is None:
            raise ToolError(f"'{name}' needs a voltage in mV")
        if size < 20:
            raise ToolError(f"firmware mailbox has no arg field: '{name}' needs a newer build", mailbox_size=size)
        probe.mem_write(cfg, f"{addr + 16:#x}", str(int(mv)), "u32")
    # The firmware may service the request from an ISR before the programmer reads it back.
    probe.mem_write(cfg, f"{addr:#x}", str(CMD[name]), "u32", volatile_target=True)
    t0 = time.monotonic()
    while True:
        after = decode(read_raw(cfg))
        m = after["mailbox"]
        if m["request"] == 0 and m["done_count"] != mb["done_count"]:
            break
        if time.monotonic() - t0 > timeout:
            raise ToolError(f"firmware did not complete '{name}' within {timeout}s (default task stalled?)",
                            status=after)
        time.sleep(0.2)

    res = m["result"]
    out = {"ok": True, "command": name, "seconds": round(time.monotonic() - t0, 2), "status": after}
    if res == UNKNOWN_CMD:
        out.update(ok=False, error="firmware rejected the command (UNKNOWN_CMD)")
    elif name == "clear-fault":
        code, text = CLEAR_RESULTS.get(res, (str(res), "unknown result"))
        out.update(ok=code in ("OK", "NOT_IN_FAULT"), result=code, explanation=text)
    elif name in ARG_CMDS:
        code, text = SET_RESULTS.get(res, (str(res), "unknown result"))
        out.update(ok=code == "OK", result=code, mv=mv, **({"explanation": text} if text else {}))
        if code == "OK" and name == "set-voltage":
            out["note"] = "the firmware slews to the new target (SETPOINT_SLEW_MV_PER_CYCLE); poll status for commanded_mv"
    elif name == "snapshot":
        out.update(ok=res == 0, result="OK" if res == 0 else "NOT_RUNNING",
                   note="VD_MON ring frozen in regulator_snapshot_trace (oldest first); pulses held off 100 us as a "
                        "scope marker (SW2 stays high: pulse-width trigger > 60 us)")
    elif name.startswith("bench-pwm"):
        code, text = BENCH_RESULTS.get(res, (str(res), "unknown result"))
        out.update(ok=code == "OK", result=code, **({"explanation": text} if text else {}))
    return out
