"""Capture text/binary from the ST-LINK virtual COM port (or any serial port).

Note: on this firmware LPUART1 carries the binary UCPD tracer protocol for
STM32CubeMonitor-UCPD, not printf text - use ``--hex`` to see it raw, or read
``debug_log`` over SWD with ``bu mem read`` instead.
"""

from __future__ import annotations

import time

from .config import ToolError, out_file, rel

ST_VID = 0x0483


def list_ports(all_ports: bool = False) -> dict:
    """USB serial ports only unless all_ports (Linux lists 32 phantom /dev/ttyS* otherwise)."""
    from serial.tools import list_ports as lp
    ports = [{"device": p.device, "vid": f"{p.vid:04x}" if p.vid else None, "pid": f"{p.pid:04x}" if p.pid else None,
              "serial": p.serial_number, "description": p.description, "stlink": p.vid == ST_VID}
             for p in lp.comports() if all_ports or p.vid is not None]
    return {"ok": True, "ports": ports}


def _auto_port() -> str:
    for p in list_ports()["ports"]:
        if p["stlink"]:
            return p["device"]
    raise ToolError("no ST-LINK virtual COM port found", ports=list_ports()["ports"])


def capture(cfg: dict, seconds: float = 3.0, port: str | None = None, baud: int | None = None,
            hex_out: bool = False, until: str | None = None, send: str | None = None,
            bytesize: int = 8, parity: str = "N") -> dict:
    import serial
    sc = cfg.get("serial", {})
    port = port or sc.get("port", "auto")
    if port == "auto":
        port = _auto_port()
    baud = baud or int(sc.get("baud", 115200))
    try:
        ser = serial.Serial(port, baud, bytesize=bytesize, parity=parity, timeout=0.1)
    except serial.SerialException as e:
        raise ToolError(f"cannot open {port}: {e}", hint="user must be in the 'dialout' group") from e
    data = bytearray()
    t0 = time.monotonic()
    matched = False
    with ser:
        ser.reset_input_buffer()
        if send:
            ser.write(send.encode().decode("unicode_escape").encode("latin-1"))
        while time.monotonic() - t0 < seconds:
            data += ser.read(4096)
            if until and until.encode() in data:
                matched = True
                break
    f = out_file(cfg, "serial", "bin" if hex_out else "log")
    f.write_bytes(bytes(data))
    result = {"ok": True, "port": port, "baud": baud, "bytes": len(data), "seconds": round(time.monotonic() - t0, 2),
              "file": rel(f)}
    if until:
        result["matched"] = matched
    if hex_out:
        result["hex_head"] = data[:512].hex(" ")
    else:
        text = data.decode(errors="replace")
        result["text_tail"] = "\n".join(text.splitlines()[-80:])
    return result
