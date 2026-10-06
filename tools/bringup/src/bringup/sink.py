"""Talk to the bench USB-PD sink (bench/pd-sink-g431) over its ST-LINK VCP.

Protocol (one ASCII line each way, CRLF from the sink): a command gets
``OK ...``, ``ERR ...``, a ``STATUS``/``VERSION`` line, or data lines closed
by ``END``.  ``EVT ...`` lines arrive any time.  Every line is ``WORD
key=value ...``, so this module parses them generically.

With two Nucleos on the bench, "first ST-LINK VCP" is ambiguous: set
``[sink].port`` or ``[sink].stlink_serial`` in bringup.local.toml.
"""

from __future__ import annotations

import time

from .config import ToolError
from .serialmon import list_ports

TERMINAL = ("OK", "ERR", "END", "STATUS", "VERSION")
ACTUATING = {"req", "epr", "eprexit", "getcaps"}


def parse_line(line: str) -> dict:
    """``EVT contract pos=2 mv=9000`` -> {"kind": "EVT", "event": "contract", "pos": 2, "mv": 9000}."""
    words = line.split()
    if not words:
        return {}
    out: dict = {"kind": words[0]}
    rest = []
    for w in words[1:]:
        if "=" in w:
            k, v = w.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
        else:
            rest.append(w)
    if out["kind"] == "EVT" and rest:
        out["event"] = rest.pop(0)
    if rest:
        out["text"] = " ".join(rest)
    return out


def _port(cfg: dict) -> str:
    sc = cfg.get("sink", {})
    if sc.get("port"):
        return sc["port"]
    want = sc.get("stlink_serial", "")
    stlinks = [p for p in list_ports()["ports"] if p["stlink"]]
    if want:
        for p in stlinks:
            if p["serial"] == want:
                return p["device"]
        raise ToolError(f"no ST-LINK VCP with serial {want}", ports=stlinks)
    if len(stlinks) == 1:
        return stlinks[0]["device"]
    raise ToolError("set [sink].port or [sink].stlink_serial: "
                    f"{len(stlinks)} ST-LINK VCPs found", ports=stlinks)


def exchange(cfg: dict, command: str, timeout: float = 2.0, until_event: str | None = None,
             wait: float = 0.0) -> dict:
    """Send one command; return its reply lines and every EVT seen.

    until_event: after the reply, keep reading until an ``EVT <name>`` arrives
    (e.g. ``contract`` after ``req``) or timeout.  wait: extra seconds to
    collect events after the reply."""
    import serial
    port = _port(cfg)
    baud = int(cfg.get("sink", {}).get("baud", 115200))
    try:
        ser = serial.Serial(port, baud, timeout=0.05)
    except serial.SerialException as e:
        raise ToolError(f"cannot open {port}: {e}", hint="user must be in the 'dialout' group") from e
    reply, events, buf = [], [], b""
    done = matched = False
    t0 = time.monotonic()
    deadline = t0 + timeout
    with ser:
        ser.reset_input_buffer()
        ser.write(command.strip().encode() + b"\n")
        while time.monotonic() < deadline:
            buf += ser.read(512)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                p = parse_line(raw.decode(errors="replace").strip())
                if not p:
                    continue
                if p["kind"] == "EVT":
                    events.append(p)
                    matched = matched or (until_event is not None and p.get("event") == until_event)
                    continue
                reply.append(p)
                if p["kind"] in TERMINAL and not done:
                    done = True
                    if not until_event:
                        # Reply complete: stop now, or after `wait` more seconds of events.
                        deadline = min(deadline, time.monotonic() + wait)
            if done and until_event and matched:
                break
    if not done:
        raise ToolError(f"no reply to {command!r} within {timeout} s", port=port, events=events)
    ok = not any(r.get("kind") == "ERR" for r in reply)
    result = {"ok": ok, "port": port, "command": command, "reply": reply, "events": events,
              "seconds": round(time.monotonic() - t0, 2)}
    if until_event:
        result["matched"] = matched
        if not matched:
            result["ok"] = False
            result["error"] = f"no EVT {until_event} within {timeout} s"
    if not ok and "error" not in result:
        result["error"] = next((r.get("text", "") for r in reply if r.get("kind") == "ERR"), "ERR")
    return result
