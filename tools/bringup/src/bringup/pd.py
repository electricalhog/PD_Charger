"""USB PD stack: policy state over SWD (pd_power.c) and the ST tracer stream.

``status`` reads ``pd_power_status`` and the three runtime knobs
(``pd_vbus_source``, ``pd_profile``, ``pd_vbus_path_max_mv``) and decodes the
offered PDOs.  ``set`` writes the knobs; the firmware rebuilds the offer and
re-sends Source_Capabilities while a sink is attached.

``trace`` decodes the TRACER_EMB stream on LPUART1 (ST-LINK VCP, 921600 8N1),
the same stream STM32CubeMonitor-UCPD reads, so only one of them can hold the
port.  Frames are TLV: 4 x 0xFD, tag (port << 5 | 0x12 for stack traces),
16-bit big-endian length, value, 4 x 0xA5.  A stack trace value is: type,
32-bit LE HAL tick, port, SOP, 16-bit BE size, data (usbpd_trace.c).
Message frames carry the PD message without CRC: 16-bit LE header, then
data objects (or the extended header and data).
"""

from __future__ import annotations

import re
import struct
import time
from pathlib import Path

from . import probe
from .config import ToolError, out_file, paths, rel

# ------------------------------------------------------------------ firmware facts (pd_power.h)

SOURCES = {0: "BENCH_5V", 1: "REGULATOR"}
PROFILES = {0: "BENCH_5V", 1: "SPR", 2: "EPR"}
ERRORS = {0: "NONE", 1: "SOURCE_CANT", 2: "PATH_LIMIT", 3: "REGULATOR", 4: "SETTLE_TIMEOUT"}
STATUS_FIELDS = (["attached", "contract_mv", "contract_ma", "contract_pos", "epr_mode", "vbus_mv", "ibus_ma",
                  "n_spr_pdo", "n_epr_pdo"] + [f"spr_pdo{i}" for i in range(7)] + [f"epr_pdo{i}" for i in range(6)]
                 + ["last_notify", "n_notify", "n_requests", "n_rejects", "n_hard_resets", "n_epr_entries",
                    "n_epr_fails", "epr_last_action", "last_rdo", "last_error", "vconn_on", "tick_ms",
                    "tcpp_type", "tcpp_ack", "tcpp_flags", "tcpp_reads"])
EPR_ACTIONS = {1: "Enter", 2: "Enter_Acknowledged", 3: "Enter_Succeeded", 4: "Enter_Failed", 5: "Exit"}
EPR_FAIL_DATA = {0: "unknown", 1: "cable not EPR capable", 2: "source failed to become VCONN source",
                 3: "EPR Mode Capable not set in RDO", 4: "source unable now", 5: "EPR Mode Capable not set in PDO"}

# ------------------------------------------------------------------ USB PD message names (PD 3.1)

CTRL = {1: "GoodCRC", 2: "GotoMin", 3: "Accept", 4: "Reject", 5: "Ping", 6: "PS_RDY", 7: "Get_Source_Cap",
        8: "Get_Sink_Cap", 9: "DR_Swap", 10: "PR_Swap", 11: "VCONN_Swap", 12: "Wait", 13: "Soft_Reset",
        14: "Data_Reset", 15: "Data_Reset_Complete", 16: "Not_Supported", 17: "Get_Source_Cap_Extended",
        18: "Get_Status", 19: "FR_Swap", 20: "Get_PPS_Status", 21: "Get_Country_Codes",
        22: "Get_Sink_Cap_Extended", 23: "Get_Source_Info", 24: "Get_Revision"}
DATA = {1: "Source_Capabilities", 2: "Request", 3: "BIST", 4: "Sink_Capabilities", 5: "Battery_Status",
        6: "Alert", 7: "Get_Country_Info", 8: "Enter_USB", 9: "EPR_Request", 10: "EPR_Mode", 11: "Source_Info",
        12: "Revision", 15: "Vendor_Defined"}
EXT = {1: "Source_Capabilities_Extended", 2: "Status", 3: "Get_Battery_Cap", 4: "Get_Battery_Status",
       5: "Battery_Capabilities", 6: "Get_Manufacturer_Info", 7: "Manufacturer_Info", 8: "Security_Request",
       9: "Security_Response", 10: "Firmware_Update_Request", 11: "Firmware_Update_Response", 12: "PPS_Status",
       13: "Country_Info", 14: "Country_Codes", 15: "Sink_Capabilities_Extended", 16: "Extended_Control",
       17: "EPR_Source_Capabilities", 18: "EPR_Sink_Capabilities"}
EXT_CONTROL = {1: "EPR_Get_Source_Cap", 2: "EPR_Get_Sink_Cap", 3: "EPR_KeepAlive", 4: "EPR_KeepAlive_Ack"}
SOP = {0: "SOP", 1: "SOP'", 2: "SOP''", 3: "SOP'_DBG", 4: "SOP''_DBG", 5: "HARD_RESET", 6: "CABLE_RESET"}
TRACE_TYPES = {0: "FORMAT_TLV", 1: "MESSAGE_IN", 2: "MESSAGE_OUT", 3: "CADEVENT", 4: "PE_STATE", 5: "CAD_LOW",
               6: "DEBUG", 7: "SRC", 8: "SNK", 9: "NOTIF", 10: "POWER", 11: "TCPM", 12: "PRL_STATE",
               13: "PRL_EVENT", 14: "PHY_NOTFRWD", 15: "CPU", 16: "TIMEOUT", 18: "UCSI", 19: "MSG_MSC"}
GUI_TAGS = {0x01: "DPM_RESET_REQ", 0x02: "DPM_INIT_REQ", 0x03: "DPM_INIT_CNF", 0x04: "DPM_CONFIG_GET_REQ",
            0x05: "DPM_CONFIG_GET_CNF", 0x06: "DPM_CONFIG_SET_REQ", 0x07: "DPM_CONFIG_SET_CNF",
            0x08: "DPM_CONFIG_REJ", 0x09: "DPM_MESSAGE_REQ", 0x0A: "DPM_MESSAGE_CNF", 0x0B: "DPM_MESSAGE_REJ",
            0x0C: "DPM_MESSAGE_IND", 0x0D: "DPM_MESSAGE_RSP", 0x0E: "DPM_REGISTER_READ_REQ",
            0x0F: "DPM_REGISTER_READ_CNF", 0x10: "DPM_REGISTER_WRITE_REQ", 0x11: "DPM_REGISTER_WRITE_CNF",
            0x12: "DEBUG_STACK_MESSAGE"}
DEBUG_STACK_MESSAGE = 0x12
SOF, EOF = b"\xfd" * 4, b"\xa5" * 4


def _header_enums(cfg: dict | None) -> tuple[dict[int, str], dict[int, str]]:
    """USBPD_NOTIFY_* and USBPD_CAD_EVENT_* names from the project's usbpd_def.h."""
    notify, cad = {}, {}
    try:
        h = paths(cfg).project_dir / "Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_def.h"
        text = h.read_text(errors="replace")
    except Exception:  # noqa: BLE001 - names are a convenience
        return notify, cad
    for m in re.finditer(r"^\s*USBPD_NOTIFY_(\w+)\s*=\s*(\d+)U?\s*,", text, re.M):
        notify.setdefault(int(m.group(2)), m.group(1))
    for m in re.finditer(r"^\s*USBPD_CAD_EVENT_(\w+)\s*=\s*(\d+)[Uu]?\s*,?", text, re.M):
        cad.setdefault(int(m.group(2)), m.group(1))
    return notify, cad


# ------------------------------------------------------------------ PDO / RDO decoding

def decode_pdo(pdo: int) -> dict:
    kind = pdo >> 30
    if kind == 0:
        d = {"type": "fixed", "mv": ((pdo >> 10) & 0x3FF) * 50, "ma": (pdo & 0x3FF) * 10}
        flags = [n for b, n in ((29, "DRP"), (28, "USB_suspend"), (27, "unconstrained"), (26, "USB_comm"),
                                (25, "DRD"), (24, "unchunked"), (23, "EPR_capable")) if pdo >> b & 1]
        if flags:
            d["flags"] = flags
        return d
    if kind == 1:
        return {"type": "battery", "max_mv": ((pdo >> 20) & 0x3FF) * 50, "min_mv": ((pdo >> 10) & 0x3FF) * 50,
                "max_mw": (pdo & 0x3FF) * 250}
    if kind == 2:
        return {"type": "variable", "max_mv": ((pdo >> 20) & 0x3FF) * 50, "min_mv": ((pdo >> 10) & 0x3FF) * 50,
                "max_ma": (pdo & 0x3FF) * 10}
    sub = (pdo >> 28) & 3
    if sub == 0:
        return {"type": "SPR_PPS", "max_mv": ((pdo >> 17) & 0xFF) * 100, "min_mv": ((pdo >> 8) & 0xFF) * 100,
                "max_ma": (pdo & 0x7F) * 50}
    if sub == 1:
        return {"type": "EPR_AVS", "max_mv": ((pdo >> 17) & 0x1FF) * 100, "min_mv": ((pdo >> 8) & 0xFF) * 100,
                "pdp_w": pdo & 0xFF}
    return {"type": f"APDO{sub}", "raw": f"{pdo:#010x}"}


def decode_rdo(rdo: int) -> dict:
    return {"pos": (rdo >> 28) & 0xF, "op_ma": ((rdo >> 10) & 0x3FF) * 10, "max_ma": (rdo & 0x3FF) * 10,
            "mismatch": bool(rdo >> 26 & 1), "epr_capable": bool(rdo >> 22 & 1), "raw": f"{rdo:#010x}"}


def decode_message(data: bytes) -> dict:
    """PD message without CRC: header, then data objects or extended header + data."""
    if len(data) < 2:
        return {"raw": data.hex()}
    hdr = struct.unpack_from("<H", data)[0]
    mtype, ndo, ext = hdr & 0x1F, (hdr >> 12) & 7, bool(hdr >> 15)
    rev = {0: "1.0", 1: "2.0", 2: "3.x"}.get((hdr >> 6) & 3, "?")
    out = {"id": (hdr >> 9) & 7, "rev": rev}
    if ext:
        out["name"] = EXT.get(mtype, f"ext{mtype}")
        if len(data) >= 4:
            eh = struct.unpack_from("<H", data, 2)[0]
            out["ext"] = {"size": eh & 0x1FF, "chunked": bool(eh >> 15), "chunk": (eh >> 11) & 0xF,
                          "request_chunk": bool(eh >> 10 & 1)}
            body = data[4:]
            if mtype == 16 and body:
                out["control"] = EXT_CONTROL.get(body[0], f"type{body[0]}")
            elif mtype == 17 and len(body) >= 4:
                pdos = [struct.unpack_from("<I", body, i)[0] for i in range(0, len(body) - len(body) % 4, 4)]
                out["pdos"] = [{"pos": i + 1, **decode_pdo(p)} for i, p in enumerate(pdos) if p]
            elif mtype == 1 and len(body) >= 23:
                out["pdp_w"] = body[23] if len(body) > 23 else None
                out["epr_pdp_w"] = body[24] if len(body) > 24 else None
        return out
    if ndo == 0:
        out["name"] = CTRL.get(mtype, f"ctrl{mtype}")
        return out
    out["name"] = DATA.get(mtype, f"data{mtype}")
    objs = [struct.unpack_from("<I", data, 2 + 4 * i)[0] for i in range(ndo) if 2 + 4 * i + 4 <= len(data)]
    if mtype in (1, 4):
        out["pdos"] = [{"pos": i + 1, **decode_pdo(p)} for i, p in enumerate(objs)]
    elif mtype == 2 and objs:
        out["rdo"] = decode_rdo(objs[0])
    elif mtype == 9 and objs:
        out["rdo"] = decode_rdo(objs[0])
        if len(objs) > 1:
            out["copy_of_pdo"] = decode_pdo(objs[1])
    elif mtype == 10 and objs:
        a, d = objs[0] >> 24, (objs[0] >> 16) & 0xFF
        out["epr_mode"] = {"action": EPR_ACTIONS.get(a, a), "data": EPR_FAIL_DATA.get(d, d) if a == 4 else d}
    elif mtype == 15 and objs:
        v = objs[0]
        out["vdm"] = {"svid": f"{v >> 16:#06x}", "structured": bool(v >> 15 & 1), "cmd": v & 0x1F,
                      "cmd_type": {0: "REQ", 1: "ACK", 2: "NAK", 3: "BUSY"}.get((v >> 6) & 3)}
        out["vdos"] = [f"{o:#010x}" for o in objs[1:]]
    else:
        out["objects"] = [f"{o:#010x}" for o in objs]
    return out


# ------------------------------------------------------------------ tracer stream

def parse_frames(buf: bytes) -> tuple[list[tuple[int, bytes]], int]:
    """(tag, value) for every complete TLV frame; also the count of bytes skipped as garbage."""
    frames, i, skipped = [], 0, 0
    while True:
        j = buf.find(SOF, i)
        if j < 0:
            skipped += len(buf) - i
            break
        skipped += j - i
        k = j
        while k < len(buf) and buf[k] == 0xFD:   # tolerate more than 4 SOF bytes
            k += 1
        if k + 3 > len(buf):
            break
        tag, n = buf[k], struct.unpack_from(">H", buf, k + 1)[0]
        end = k + 3 + n
        if end + 4 > len(buf):
            break
        if buf[end:end + 4] != EOF:
            skipped += 1
            i = j + 1
            continue
        frames.append((tag, bytes(buf[k + 3:end])))
        i = end + 4
    return frames, skipped


def decode_trace(buf: bytes, cfg: dict | None = None) -> dict:
    notify_names, cad_names = _header_enums(cfg)
    frames, skipped = parse_frames(buf)
    events, counts = [], {}
    for tag, val in frames:
        kind = tag & 0x1F
        if kind != DEBUG_STACK_MESSAGE:
            ev = {"gui": GUI_TAGS.get(kind, f"tag{kind:#x}"), "port": tag >> 5, "len": len(val)}
            events.append(ev)
            counts[ev["gui"]] = counts.get(ev["gui"], 0) + 1
            continue
        if len(val) < 9:
            continue
        ttype, tick, port, sop = val[0], struct.unpack_from("<I", val, 1)[0], val[5], val[6]
        size = struct.unpack_from(">H", val, 7)[0]
        data = val[9:9 + size]
        name = TRACE_TYPES.get(ttype, f"type{ttype}")
        counts[name] = counts.get(name, 0) + 1
        ev = {"t_ms": tick, "port": port, "type": name}
        if ttype in (1, 2):
            ev["dir"] = "<-" if ttype == 1 else "->"
            ev["sop"] = SOP.get(sop, sop)
            if sop in (5, 6):
                ev["msg"] = {"name": SOP[sop]}
            else:
                ev["msg"] = decode_message(data)
        elif ttype == 3:
            ev["cad"] = cad_names.get(sop, sop)
        elif ttype == 9:
            code = data[0] if data else sop
            ev["notify"] = notify_names.get(code, code)
        elif ttype in (6, 7, 8, 10):
            ev["text"] = data.decode(errors="replace").rstrip("\0")
        else:
            ev["sop"] = sop
            if data:
                ev["data"] = data.hex()
        events.append(ev)
    return {"frames": len(frames), "skipped_bytes": skipped, "counts": counts, "events": events}


def _line(ev: dict) -> str:
    if "gui" in ev:
        return f"{'':>9}  GUI {ev['gui']} ({ev['len']} B)"
    t = f"{ev['t_ms']:>9}"
    if "msg" in ev:
        m = ev["msg"]
        s = f"{t}  {ev['dir']} {ev['sop']:<5} {m.get('name', '?')}"
        if "control" in m:
            s += f" {m['control']}"
        if "pdos" in m:
            s += " " + ", ".join(f"#{p['pos']} {p.get('mv', p.get('max_mv', 0)) / 1000:g}V"
                                 f"{'/' + format(p['ma'] / 1000, 'g') + 'A' if 'ma' in p else ''}" for p in m["pdos"])
        if "rdo" in m:
            r = m["rdo"]
            s += f" pos {r['pos']} {r['op_ma']} mA{' EPR' if r['epr_capable'] else ''}{' mismatch' if r['mismatch'] else ''}"
        if "epr_mode" in m:
            s += f" {m['epr_mode']['action']} {m['epr_mode']['data']}"
        if "vdm" in m:
            s += f" svid {m['vdm']['svid']} cmd {m['vdm']['cmd']} {m['vdm']['cmd_type']}"
        return s
    if "cad" in ev:
        return f"{t}  CAD {ev['cad']}"
    if "notify" in ev:
        return f"{t}  NOTIFY {ev['notify']}"
    if "text" in ev:
        return f"{t}  {ev['type']} {ev['text']}"
    return f"{t}  {ev['type']} sop={ev.get('sop')} {ev.get('data', '')}"


def trace(cfg: dict, seconds: float = 5.0, port: str | None = None, file: str | None = None,
          show: int = 200) -> dict:
    if file:
        raw = Path(file).read_bytes()
        src = file
    else:
        import serial
        from .serialmon import _auto_port
        port = port or cfg.get("serial", {}).get("port", "auto")
        if port == "auto":
            port = _auto_port()
        try:
            ser = serial.Serial(port, 921600, timeout=0.1)
        except serial.SerialException as e:
            raise ToolError(f"cannot open {port}: {e}",
                            hint="close STM32CubeMonitor-UCPD (it holds the port); user must be in 'dialout'") from e
        data = bytearray()
        t0 = time.monotonic()
        with ser:
            ser.reset_input_buffer()
            while time.monotonic() - t0 < seconds:
                data += ser.read(4096)
        raw = bytes(data)
        f = out_file(cfg, "pdtrace", "bin")
        f.write_bytes(raw)
        src = rel(f)
    d = decode_trace(raw, cfg)
    log = out_file(cfg, "pdtrace", "log")
    log.write_text("\n".join(_line(e) for e in d["events"]) + "\n")
    return {"ok": True, "source": src, "bytes": len(raw), "frames": d["frames"], "skipped_bytes": d["skipped_bytes"],
            "counts": d["counts"], "log": rel(log), "lines": [_line(e) for e in d["events"][-show:]],
            "note": None if raw else "no bytes: the stack only traces on events (attach a sink), or the target "
                                     "is not running"}


# ------------------------------------------------------------------ policy state over SWD

def _read_status(cfg: dict) -> dict:
    syms = probe.symbols(cfg)
    need = ["pd_power_status", "pd_vbus_source", "pd_profile", "pd_vbus_path_max_mv"]
    missing = [n for n in need if n not in syms]
    if missing:
        raise ToolError(f"firmware has no {', '.join(missing)} (build with pd_power.c)")
    addr, size = syms["pd_power_status"]
    if size != 4 * len(STATUS_FIELDS):
        raise ToolError(f"pd_power_status is {size} bytes, the tool expects {4 * len(STATUS_FIELDS)}: "
                        "update STATUS_FIELDS in pd.py to match pd_power.h")
    blobs = probe.read_regions(cfg, [(addr, size)] + [(syms[n][0], 4) for n in need[1:]])
    vals = dict(zip(STATUS_FIELDS, struct.unpack(f"<{len(STATUS_FIELDS)}I", blobs[0])))
    vals["ibus_ma"] = struct.unpack("<i", struct.pack("<I", vals["ibus_ma"]))[0]
    knobs = [struct.unpack("<I", b)[0] for b in blobs[1:]]
    return {"vals": vals, "source": knobs[0], "profile": knobs[1], "path_max_mv": knobs[2]}


def status(cfg: dict) -> dict:
    r = _read_status(cfg)
    v = r["vals"]
    notify_names, _ = _header_enums(cfg)
    spr = [v[f"spr_pdo{i}"] for i in range(min(v["n_spr_pdo"], 7))]
    epr = [v[f"epr_pdo{i}"] for i in range(min(v["n_epr_pdo"], 6))]
    act, dat = v["epr_last_action"] >> 8, v["epr_last_action"] & 0xFF
    return {
        "ok": True,
        "knobs": {"vbus_source": SOURCES.get(r["source"], r["source"]), "profile": PROFILES.get(r["profile"], r["profile"]),
                  "vbus_path_max_mv": r["path_max_mv"]},
        "attached": bool(v["attached"]),
        "contract": ({"mv": v["contract_mv"], "ma": v["contract_ma"], "pos": v["contract_pos"]}
                     if v["contract_mv"] else None),
        "epr_mode": bool(v["epr_mode"]), "vconn_on": bool(v["vconn_on"]),
        "vbus_mv": v["vbus_mv"], "ibus_ma": v["ibus_ma"], "vbus_sample_tick_ms": v["tick_ms"],
        "offer": {"spr": [{"pos": i + 1, **decode_pdo(p)} for i, p in enumerate(spr)],
                  "epr": [{"pos": i + 8, **decode_pdo(p)} for i, p in enumerate(epr)]},
        "last_rdo": decode_rdo(v["last_rdo"]) if v["last_rdo"] else None,
        "last_error": ERRORS.get(v["last_error"], v["last_error"]),
        "last_notify": notify_names.get(v["last_notify"], v["last_notify"]) if v["n_notify"] else None,
        "epr_last_mode_do": {"action": EPR_ACTIONS.get(act, act), "data": dat} if v["epr_last_action"] else None,
        "counters": {k: v[k] for k in ("n_notify", "n_requests", "n_rejects", "n_hard_resets", "n_epr_entries",
                                        "n_epr_fails")},
        "tcpp": decode_tcpp(v),
    }


def decode_tcpp(v: dict) -> dict | None:
    """TCPP0203 ACK/FLAG registers (tcpp0203.h). Note the VCONN switch ACK codes
    are swapped relative to the control register: ACK 2 = CC1, 1 = CC2."""
    if not v["tcpp_reads"]:
        return None
    a, f = v["tcpp_ack"], v["tcpp_flags"]
    return {
        "reads": v["tcpp_reads"] & 0x7FFFFFFF, "last_read_failed": bool(v["tcpp_reads"] >> 31),
        "type": f"{v['tcpp_type']:#04x}",
        "vconn_switch": {0: "open", 1: "CC2", 2: "CC1", 3: "both?"}[a & 3],
        "gate_provider": "closed" if a >> 2 & 1 else "open",
        "gate_consumer": "open" if a >> 3 & 1 else "closed",
        "power_mode": {0: "hibernate", 1: "low_power", 2: "normal", 3: "?"}[(a >> 4) & 3],
        "vbus_discharge": bool(a >> 6 & 1), "vconn_discharge": bool(a >> 7 & 1),
        "flags": [n for b, n in ((0, "OCP_VCONN"), (1, "OCP_VBUS"), (2, "OVP_VBUS"), (3, "OVP_CC"), (4, "OTP"),
                                 (5, "VBUS_OK"), (6, "VCONN_PWR")) if f >> b & 1],
        "raw": {"ack": f"{a:#04x}", "flags": f"{f:#04x}"},
    }


def set_knobs(cfg: dict, source: str | None = None, profile: str | None = None,
              path_max_mv: int | None = None) -> dict:
    rev = lambda d, k, what: next((n for n, s in d.items() if s == k.upper()), None)  # noqa: E731
    writes = []
    if source is not None:
        n = rev(SOURCES, source, "source")
        if n is None:
            raise ToolError(f"unknown source {source}", choices=list(SOURCES.values()))
        writes.append(("pd_vbus_source", n))
    if profile is not None:
        n = rev(PROFILES, profile, "profile")
        if n is None:
            raise ToolError(f"unknown profile {profile}", choices=list(PROFILES.values()))
        writes.append(("pd_profile", n))
    if path_max_mv is not None:
        if not 5000 <= path_max_mv <= 48000:
            raise ToolError("path max must be 5000..48000 mV")
        writes.append(("pd_vbus_path_max_mv", path_max_mv))
    if not writes:
        raise ToolError("nothing to set (--source, --profile, --path-max-mv)")
    for name, val in writes:
        probe.mem_write(cfg, name, str(val), "u32")
    time.sleep(0.1)   # pd_power_poll runs every 10 ms and rebuilds the offer
    return {"ok": True, "written": dict(writes), "status": status(cfg)}
