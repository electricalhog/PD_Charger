"""USB-PD source status over SWD: offered PDOs, contract, EPR mode, event log.

Reads the firmware's ``pd_status`` block (Core/Inc/pd_policy.h): every field
is a 32-bit word in a fixed order (PD_STATUS_VERSION), so no ELF struct
layout is needed.  Notification names are parsed from the project's own
usbpd_def.h, so they follow whichever USB-PD core library is installed.

This shows what the firmware decided; the bytes on the CC wire are in the
UCPD tracer on LPUART1 (STM32CubeMonitor-UCPD) or a logic-analyzer capture
(`bu la decode` with the usb_power_delivery decoder).
"""

from __future__ import annotations

import re
import struct
from functools import lru_cache
from pathlib import Path

from . import probe
from .config import ToolError, paths

MAGIC = 0x54534450          # "PDST"
VERSION = 1
SPR_MAX, EPR_MAX, EVENTS = 7, 6, 32

# Field order of PdStatus (pd_policy.h), version 1.
_FIELDS = (
    ["magic", "version", "lib_epr", "epr_offered", "spr_pdo_count", "epr_pdo_count"]
    + [f"spr_pdo{i}" for i in range(SPR_MAX)] + [f"epr_pdo{i}" for i in range(EPR_MAX)]
    + ["attach_count", "detach_count", "hard_reset_count", "contract_count", "last_rdo",
       "last_rdo_result", "contract_mv", "contract_ma", "contract_position", "transition_ms",
       "transition_timeouts", "vbus_on_failures", "fault_hard_resets", "epr_mode",
       "epr_enter_requests", "epr_enter_succeeded", "epr_enter_failed", "epr_exits",
       "epr_enter_mode_replies", "ev_count"]
    + [f"ev_tick{i}" for i in range(EVENTS)] + [f"ev_code{i}" for i in range(EVENTS)]
)
SIZE = 4 * len(_FIELDS)

RDO_RESULTS = {0: "ACCEPT", 1: "BAD_POSITION", 2: "EPR_PDO_OUTSIDE_EPR_MODE", 3: "NOT_FIXED", 4: "OVER_CURRENT"}
PD_EVENTS = {0x200: "FAULT_HARD_RESET", 0x201: "TRANSITION_TIMEOUT", 0x202: "VBUS_ON_FAILED",
             0x203: "SETUP_POWER", 0x204: "SETUP_POWER_ERR", 0x205: "POWER_NOT_READY"}
PD_EV_DPM_WHAT_TO_DO = 0x210  # | USBPD_CORE_ActionType_TypeDef
DPM_ACTIONS = {1: "ENTER_USB", 2: "DATA_RESET", 3: "ENTER_MODE", 4: "CHECK_PDO"}
REG_STATES = {0: "INIT", 1: "IDLE", 2: "RUNNING", 3: "FAULT"}


@lru_cache(maxsize=4)
def _enum(header: Path, prefix: str) -> dict[int, str]:
    if not header.exists():
        return {}
    rx = re.compile(rf"\b{prefix}(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?\b")
    return {int(v, 0): name for name, v in rx.findall(header.read_text(errors="replace"))}


def _names(cfg: dict) -> tuple[dict[int, str], dict[int, str]]:
    h = paths(cfg).project_dir / "Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_def.h"
    return _enum(h, "USBPD_NOTIFY_"), _enum(h, "USBPD_CAD_EVENT_")


def decode_pdo(pdo: int, position: int) -> dict:
    kind = (pdo >> 30) & 3
    if kind != 0:
        return {"position": position, "raw": f"{pdo:#010x}", "type": ["FIXED", "BATTERY", "VARIABLE", "APDO"][kind]}
    d = {"position": position, "raw": f"{pdo:#010x}", "type": "FIXED",
         "mv": ((pdo >> 10) & 0x3FF) * 50, "ma": (pdo & 0x3FF) * 10}
    if position == 1:
        d["flags"] = [n for b, n in ((29, "DRP"), (28, "USB_SUSPEND"), (27, "UNCONSTRAINED"), (26, "USB_COMM"),
                                     (25, "DRD"), (24, "UNCHUNKED"), (23, "EPR_MODE_CAPABLE")) if pdo >> b & 1]
    return d


def decode_rdo(rdo: int) -> dict:
    return {"raw": f"{rdo:#010x}", "position": (rdo >> 28) & 0xF, "capability_mismatch": bool(rdo >> 26 & 1),
            "epr_mode_capable": bool(rdo >> 22 & 1), "operating_ma": ((rdo >> 10) & 0x3FF) * 10,
            "max_ma": (rdo & 0x3FF) * 10}


def decode(words: list[int], now_ms: int, notify: dict[int, str], cad: dict[int, str]) -> dict:
    w = dict(zip(_FIELDS, words))
    if w["magic"] != MAGIC:
        raise ToolError("pd_status magic mismatch: firmware too old, or pd_policy_init() has not run",
                        magic=f"{w['magic']:#010x}")
    if w["version"] != VERSION:
        raise ToolError(f"pd_status version {w['version']}, this tool decodes {VERSION}")

    def ev_name(code: int) -> str:
        if code in PD_EVENTS:
            return PD_EVENTS[code]
        if code & ~0xF == PD_EV_DPM_WHAT_TO_DO:
            return "DPM_WHAT_TO_DO_" + DPM_ACTIONS.get(code & 0xF, str(code & 0xF))
        if code & 0x100:
            return "CAD_" + cad.get(code & 0xFF, str(code & 0xFF))
        return notify.get(code, f"NOTIFY_{code}")

    n = w["ev_count"]
    events = []
    for k in range(max(0, n - EVENTS), n):
        i = k % EVENTS
        events.append({"seq": k, "age_ms": (now_ms - w[f"ev_tick{i}"]) & 0xFFFFFFFF,
                       "event": ev_name(w[f"ev_code{i}"])})

    s = {
        "library_has_epr": bool(w["lib_epr"]),
        "epr_mode_capable_offered": bool(w["epr_offered"]),
        "spr_pdos": [decode_pdo(w[f"spr_pdo{i}"], i + 1) for i in range(min(w["spr_pdo_count"], SPR_MAX))],
        "epr_pdos": [decode_pdo(w[f"epr_pdo{i}"], 8 + i) for i in range(min(w["epr_pdo_count"], EPR_MAX))],
        "counters": {k: w[k] for k in ("attach_count", "detach_count", "hard_reset_count", "contract_count",
                                       "transition_timeouts", "vbus_on_failures", "fault_hard_resets")},
        "contract": {"mv": w["contract_mv"], "ma": w["contract_ma"], "position": w["contract_position"],
                     "last_transition_ms": w["transition_ms"]} if w["contract_mv"] else None,
        "last_rdo": {**decode_rdo(w["last_rdo"]), "result": RDO_RESULTS.get(w["last_rdo_result"], w["last_rdo_result"])}
                    if w["last_rdo"] else None,
        "epr": {"in_epr_mode": bool(w["epr_mode"]), "enter_acknowledged": w["epr_enter_requests"],
                "enter_succeeded": w["epr_enter_succeeded"], "enter_failed": w["epr_enter_failed"],
                "exits": w["epr_exits"], "dpm_enter_replies": w["epr_enter_mode_replies"]},
        "events": events,
    }
    s["diagnosis"] = diagnose(s)
    return s


def diagnose(s: dict) -> list[str]:
    out = []
    c, e = s["counters"], s["epr"]
    if not s["library_has_epr"]:
        out.append("USB-PD core library without EPR (USBPDCORE_EPR undefined): a sink's EPR_Mode (Enter) gets "
                   "Not_Supported. Install ST stm32-mw-usbpd-core >= v5.0.0 (see plans/epr-bringup.md).")
    if c["vbus_on_failures"]:
        out.append(f"{c['vbus_on_failures']} attach(es) with the regulator unable to start: run `bu regulator status`.")
    if c["transition_timeouts"]:
        out.append(f"{c['transition_timeouts']} voltage transition(s) did not reach +/-5 % in tPSTransition; "
                   "the sink saw no PS_RDY and Hard Reset. Check the regulator slew and load.")
    if c["fault_hard_resets"]:
        out.append("The regulator faulted during a contract and the firmware sent a Hard Reset; "
                   "the FAULT stays latched until `bu regulator clear-fault`.")
    if e["enter_failed"] and not e["enter_succeeded"]:
        out.append("EPR entry failed: without VCONN (PD_VCONN) the cable e-marker cannot answer SOP' Discover "
                   "Identity, so the source reports 'cable not EPR capable'. The tracer shows the failure code.")
    if s["last_rdo"] and s["last_rdo"]["result"] != "ACCEPT":
        out.append(f"Last request rejected: {s['last_rdo']['result']}.")
    if c["attach_count"] and not c["contract_count"] and not out:
        out.append("Attached but no contract yet: check the tracer for Source_Capabilities / Request.")
    return out


GPIOA_ODR, GPIOB_ODR = 0x48000014, 0x48000414


def status(cfg: dict) -> dict:
    syms = probe.symbols(cfg)
    if "pd_status" not in syms:
        raise ToolError("firmware has no pd_status; flash a build from the EPR bring-up branch")
    addr, size = syms["pd_status"]
    if size != SIZE:
        raise ToolError(f"pd_status is {size} bytes, this tool expects {SIZE}: firmware and tool disagree")
    regions = [(addr, SIZE)]
    names = [n for n in ("uwTick", "regulator_state", "adc_measurements", "pd_bench_dry_run") if n in syms]
    regions += [syms[n] for n in names]
    # VCONN switch enables (pd_bench_config.h): PA7 -> CC1, PB5 -> CC2 (GPIOx_ODR).
    regions += [(GPIOA_ODR, 4), (GPIOB_ODR, 4)]
    data = probe.read_regions(cfg, regions)
    words = list(struct.unpack(f"<{len(_FIELDS)}I", data[0]))
    extra = dict(zip(names, data[1:1 + len(names)]))
    odr_a, odr_b = (int.from_bytes(d[:4], "little") for d in data[1 + len(names):])
    now = int.from_bytes(extra.get("uwTick", b"\0\0\0\0")[:4], "little")
    notify, cad = _names(cfg)
    out = {"ok": True, **decode(words, now, notify, cad)}
    if "regulator_state" in extra:
        out["regulator_state"] = REG_STATES.get(extra["regulator_state"][0], extra["regulator_state"][0])
    if "adc_measurements" in extra:
        out["v_out_mv"] = int.from_bytes(extra["adc_measurements"][:4], "little")
    if "pd_bench_dry_run" in extra:
        out["dry_run"] = bool(int.from_bytes(extra["pd_bench_dry_run"][:4], "little"))
    out["vconn_switch"] = {"cc1_pa7": odr_a >> 7 & 1, "cc2_pb5": odr_b >> 5 & 1}
    return out
