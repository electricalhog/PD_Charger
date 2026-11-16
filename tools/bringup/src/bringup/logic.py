"""Logic analyzer capture and protocol decode via sigrok-cli.

Works with anything libsigrok supports (fx2lafw Saleae clones, DSLogic,
Rigol MSO digital channels, ...). Install with ``sudo apt install sigrok-cli``.
"""

from __future__ import annotations

import re
import shutil

from .config import ToolError, out_file, rel, run, tail


def _cli(cfg: dict) -> str:
    exe = shutil.which(cfg.get("logic", {}).get("sigrok_cli", "sigrok-cli"))
    if not exe:
        raise ToolError("sigrok-cli not installed", hint="sudo apt install sigrok-cli (and pulseview for manual viewing)")
    return exe


def scan(cfg: dict) -> dict:
    cp = run([_cli(cfg), "--scan"], timeout=30)
    devices = [ln.strip() for ln in cp.stdout.splitlines() if ln.strip() and not ln.startswith("The following")]
    return {"ok": True, "devices": devices, **({"stderr": tail(cp.stderr, 10)} if cp.stderr.strip() else {})}


def list_decoders(cfg: dict) -> dict:
    cp = run([_cli(cfg), "-L"], timeout=30)
    txt = cp.stdout
    sec = txt.split("Supported protocol decoders:", 1)[-1].split("Supported input formats:", 1)[0]
    decs = {m[1]: m[2].strip() for m in re.finditer(r"^\s+(\S+)\s+(.+)$", sec, re.M)}
    return {"ok": True, "decoders": decs}


def capture(cfg: dict, channels: str, samplerate: str = "4m", samples: int | None = None,
            time_ms: int | None = None, trigger: str | None = None, driver: str | None = None,
            decoders: list[str] | None = None, annotations: str | None = None, config: list[str] | None = None) -> dict:
    """channels: sigrok channel spec, e.g. 'D0=PWM_HI,D1=PWM_LO,D2=SCL,D3=SDA'.
    trigger: e.g. 'PWM_HI=r' (r/f/e/0/1). decoders: e.g. ['i2c:scl=SCL:sda=SDA']."""
    drv = driver or cfg.get("logic", {}).get("driver", "fx2lafw")
    sr_file = out_file(cfg, "logic", "sr")
    conf = [f"samplerate={samplerate}", *(config or [])]
    cmd = [_cli(cfg), "-d", drv, "--config", ":".join(conf), "-C", channels, "-o", str(sr_file)]
    if samples:
        cmd += ["--samples", str(samples)]
    elif time_ms:
        cmd += ["--time", f"{time_ms}ms"]
    else:
        cmd += ["--time", "100ms"]
    if trigger:
        cmd += ["-t", trigger, "-w"]  # -w: wait for trigger, discard pre-trigger samples
    cp = run(cmd, timeout=120)
    if cp.returncode or not sr_file.exists():
        raise ToolError("capture failed", stderr=tail(cp.stderr, 15), command=" ".join(cmd))
    result = {"ok": True, "file": rel(sr_file), "summary": edges(cfg, sr_file)}
    if decoders:
        result["decode"] = decode(cfg, str(sr_file), decoders, annotations)
    return result


def edges(cfg: dict, sr_file) -> dict:
    """Per-channel transition stats from a .sr capture (frequency / duty / pulse widths)."""
    cp = run([_cli(cfg), "-i", str(sr_file), "-O", "csv:time=true:dedup=true:header=true"], timeout=120)
    if cp.returncode:
        return {"error": tail(cp.stderr, 5)}
    rows, names = [], None
    for ln in cp.stdout.splitlines():
        if not ln or ln.startswith(";"):
            continue
        parts = ln.split(",")
        if names is None:
            names = [p.strip().split(" ")[0] for p in parts[1:]]
            continue
        try:
            rows.append((_t(parts[0]), [int(x) for x in parts[1:]]))
        except ValueError:
            continue
    if not rows or not names:
        return {"error": "no samples parsed"}
    t_end = rows[-1][0]
    out = {}
    for ci, name in enumerate(names):
        trans = [(t, v[ci]) for i, (t, v) in enumerate(rows) if i == 0 or v[ci] != rows[i - 1][1][ci]]
        rises = [t for t, v in trans[1:] if v == 1]
        falls = [t for t, v in trans[1:] if v == 0]
        high = sum((trans[i + 1][0] if i + 1 < len(trans) else t_end) - t for i, (t, v) in enumerate(trans) if v == 1)
        s = {"initial": trans[0][1], "rising": len(rises), "falling": len(falls),
             "duty_pct": round(100 * high / t_end, 3) if t_end else None}
        if len(rises) >= 2:
            per = (rises[-1] - rises[0]) / (len(rises) - 1)
            s["freq_hz"] = 1 / per if per else None
            s["first_high_pulse_s"] = next((f - rises[0] for f in falls if f > rises[0]), None)
        out[name] = s
    return {"duration_s": t_end, "channels": out}


def _t(s: str) -> float:
    m = re.fullmatch(r"\s*([\d.eE+-]+)\s*(s|ms|us|µs|ns)?\s*", s)
    if not m:
        raise ValueError(s)
    return float(m[1]) * {"s": 1, "ms": 1e-3, "us": 1e-6, "µs": 1e-6, "ns": 1e-9, None: 1}[m[2]]


def decode(cfg: dict, sr_file: str, decoders: list[str], annotations: str | None = None, max_lines: int = 200) -> dict:
    cmd = [_cli(cfg), "-i", sr_file]
    for d in decoders:
        cmd += ["-P", d]
    if annotations:
        cmd += ["-A", annotations]
    cp = run(cmd, timeout=120)
    if cp.returncode:
        raise ToolError("decode failed", stderr=tail(cp.stderr, 15))
    lines = cp.stdout.splitlines()
    f = out_file(cfg, "logic_decode", "txt")
    f.write_text(cp.stdout)
    return {"file": rel(f), "lines": len(lines), "head": lines[:max_lines], "truncated": len(lines) > max_lines}
