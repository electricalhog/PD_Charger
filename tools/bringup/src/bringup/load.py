"""The bench buck load's own USB console (bench/buck-load-qtpy, QT Py RP2040).

The sink commands the load over I2C (``bu sink load ...``); this module talks
to the QT Py directly over its USB CDC port for what the sink cannot do:
raw ADC counts, the link-pin check, and reflashing.

``flash`` builds the firmware, asks it (or the pico-sdk port firmware it
replaced) to reboot into BOOTSEL, and writes a UF2 to the RPI-RP2 drive.  The
firmware refuses BOOTSEL unless both buck taps are low (main.rs
``bootsel_allowed``): the RP2040 resets with every pad pulled down, which
enables the gate drivers with the low-side FETs on.
"""

from __future__ import annotations

import struct
import time
from pathlib import Path

from .config import REPO_ROOT, ToolError, out_file, rel, run, tail
from .serialmon import list_ports
from .sink import parse_line

RPI_VID, PICO_CDC_PID = "2e8a", "000a"
PRODUCT = "buck-load-qtpy"
FIRMWARE_DIR = REPO_ROOT / "bench" / "buck-load-qtpy"
ELF = FIRMWARE_DIR / "target" / "thumbv6m-none-eabi" / "release" / "buck-load-qtpy"

# UF2 (https://github.com/microsoft/uf2), RP2040 family.
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
FLASH_START, FLASH_END = 0x10000000, 0x11000000
PAGE = 256


def _port(cfg: dict) -> str:
    lc = cfg.get("load", {})
    if lc.get("port"):
        return lc["port"]
    cands = [p for p in list_ports()["ports"] if p["vid"] == RPI_VID and p["pid"] == PICO_CDC_PID]
    ours = [p for p in cands if PRODUCT in (p["description"] or "") or p["serial"] == PRODUCT]
    if ours:
        return ours[0]["device"]
    if len(cands) == 1:
        return cands[0]["device"]   # pico-sdk port firmware, before the first flash
    raise ToolError(f"no {PRODUCT} USB console found ({len(cands)} RP2040 CDC ports)",
                    hint="set [load].port in bringup.local.toml", ports=list_ports()["ports"])


def exchange(cfg: dict, command: str, timeout: float = 2.0) -> dict:
    """One console command, one reply line (parsed like the sink's lines)."""
    import serial
    port = _port(cfg)
    try:
        ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0)
    except serial.SerialException as e:
        raise ToolError(f"cannot open {port}: {e}", hint="user must be in the 'dialout' group") from e
    buf = b""
    with ser:
        ser.reset_input_buffer()
        try:
            ser.write(command.encode() + b"\n")
        except serial.SerialTimeoutException as e:
            raise ToolError(f"{port} accepts no data: the load firmware is not running its USB task",
                            hint="reset the QT Py (RESET button or replug)") from e
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            buf += ser.read(512)
            if b"\n" in buf:
                break
    line = buf.split(b"\n", 1)[0].decode(errors="replace").strip()
    if not line:
        raise ToolError(f"no reply to {command!r} from {port}")
    reply = parse_line(line)
    return {"ok": reply.get("kind") not in ("ERR",), "port": port, "reply": reply, "line": line}


TEL_FIELDS = ("vin_mv", "vout_mv", "iout_ma", "pout_mw", "duty_pm", "temp_mv")


def run_load(cfg: dict, p_mw: list[int], seconds: float, limit_ma: int = 500, contract_mv: int = 0,
             vin_min_mv: int = 0, clear: bool = False, period_s: float = 0.05) -> dict:
    """Hold the load at each power target in ``p_mw`` for ``seconds``.

    Sends ``cmd`` every 50 ms (the firmware faults after 250 ms without one),
    stepping the target without disarming in between, and keeps every
    ``TEL`` reply tagged with the target in force.  Always ends with a few
    ``arm=0`` frames, on a fault or an exception too, so the load never waits
    on its watchdog.  Per-step stats cover the second half of each step,
    after the 5 mV/ms V_out ramp.
    """
    import json

    import serial
    port = _port(cfg)
    try:
        ser = serial.Serial(port, 115200, timeout=0, write_timeout=1.0)
    except serial.SerialException as e:
        raise ToolError(f"cannot open {port}: {e}", hint="user must be in the 'dialout' group") from e
    tel: list[dict] = []
    seq = 0
    buf = b""
    target = p_mw[0]
    t0 = time.monotonic()

    def send(arm: bool, clear_fault: bool = False) -> None:
        nonlocal seq
        seq = (seq + 1) & 0xFF
        ser.write((f"cmd seq={seq} arm={int(arm)} clear={int(clear_fault)} p_mw={target if arm else 0} "
                   f"limit_ma={limit_ma} contract_mv={contract_mv} vin_min_mv={vin_min_mv}\n").encode())

    def drain(armed: bool) -> None:
        nonlocal buf
        buf += ser.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace").strip()
            if line:
                r = parse_line(line)
                r["t_s"] = round(time.monotonic() - t0, 3)
                r["p_target"] = target if armed else 0
                tel.append(r)

    total = seconds * len(p_mw)
    with ser:
        ser.reset_input_buffer()
        try:
            if clear:
                for _ in range(3):
                    send(False, clear_fault=True)
                    time.sleep(period_s)
            t0 = time.monotonic()
            next_t = t0
            while (el := time.monotonic() - t0) < total:
                target = p_mw[min(int(el // seconds), len(p_mw) - 1)]
                send(True)
                drain(True)
                if tel and tel[-1].get("state") == "fault":
                    break
                next_t += period_s
                time.sleep(max(0.0, next_t - time.monotonic()))
        finally:
            for _ in range(3):
                send(False)
                time.sleep(period_s)
            drain(False)

    frames = [r for r in tel if r.get("kind") == "TEL"]
    f = out_file(cfg, "load_run", "json")
    f.write_text(json.dumps(frames))
    steps = []
    for i, p in enumerate(p_mw):
        a, b = seconds * (i + 0.5), seconds * (i + 1)
        sel = [r for r in frames if a <= r["t_s"] < b and r.get("state") == "running"]
        st = {"p_mw": p, "frames": len(sel)}
        for k in TEL_FIELDS:
            v = [r[k] for r in sel if isinstance(r.get(k), int)]
            if v:
                st[k] = {"min": min(v), "max": max(v), "mean": round(sum(v) / len(v))}
        steps.append(st)
    faults = [r for r in frames if r.get("fault") not in (None, "none")]
    return {"ok": not faults and all(s["frames"] for s in steps), "port": port, "frames": len(frames),
            "fault": faults[0] if faults else None, "last": frames[-1] if frames else None,
            "steps": steps, "other_lines": [r for r in tel if r.get("kind") != "TEL"][:5], "file": rel(f)}


def elf_flash_segments(elf: bytes) -> list[tuple[int, bytes]]:
    """(load address, bytes) of every PT_LOAD segment with file data in flash."""
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        raise ToolError("not a little-endian ELF32 file")
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    segs = []
    for i in range(phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", elf, phoff + i * phentsize)
        if p_type == 1 and p_filesz and FLASH_START <= p_paddr < FLASH_END:
            segs.append((p_paddr, elf[p_offset:p_offset + p_filesz]))
    if not segs:
        raise ToolError("no flash segments in the ELF")
    return sorted(segs)


def uf2_from_segments(segs: list[tuple[int, bytes]]) -> bytes:
    """256-byte UF2 blocks covering every segment (gaps inside a page are 0)."""
    pages: dict[int, bytearray] = {}
    for addr, data in segs:
        for i, b in enumerate(data):
            a = addr + i
            pages.setdefault(a & ~(PAGE - 1), bytearray(PAGE))[a & (PAGE - 1)] = b
    out = bytearray()
    order = sorted(pages)
    for n, base in enumerate(order):
        payload = bytes(pages[base]) + bytes(476 - PAGE)
        out += struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY_ID, base, PAGE, n, len(order),
                           RP2040_FAMILY_ID) + payload + struct.pack("<I", UF2_MAGIC_END)
    return bytes(out)


def _rpi_rp2_mount(wait_s: float = 15.0) -> Path:
    """The RPI-RP2 drive's mount point, mounting it with udisksctl if needed."""
    deadline = time.monotonic() + wait_s
    tried = set()
    while time.monotonic() < deadline:
        r = run(["findmnt", "-rn", "-o", "TARGET", "-S", "LABEL=RPI-RP2"])
        if r.returncode == 0 and r.stdout.strip():
            return Path(r.stdout.split("\n")[0].strip().replace("\\x20", " "))
        r = run(["blkid", "-L", "RPI-RP2"])
        dev = r.stdout.strip()
        if dev and dev not in tried:
            tried.add(dev)
            run(["udisksctl", "mount", "-b", dev])
        time.sleep(0.3)
    raise ToolError("RPI-RP2 drive did not appear", hint="the QT Py did not enter BOOTSEL; check `bu load status`")


def _wait_console(cfg: dict, wait_s: float = 15.0) -> dict:
    deadline = time.monotonic() + wait_s
    last = None
    while time.monotonic() < deadline:
        try:
            return exchange(cfg, "version", timeout=1.0)
        except ToolError as e:
            last = e
            time.sleep(0.3)
    raise ToolError(f"{PRODUCT} console did not come back: {last}")


def flash(cfg: dict, power_stage: bool = False, build: bool = True) -> dict:
    steps = {}
    if build:
        cmd = ["cargo", "build", "--release"] + (["--features", "power-stage"] if power_stage else [])
        r = run(cmd, cwd=FIRMWARE_DIR, timeout=900)
        if r.returncode != 0:
            raise ToolError("cargo build failed", stderr_tail=tail(r.stderr))
        steps["build"] = " ".join(cmd)
    uf2 = uf2_from_segments(elf_flash_segments(ELF.read_bytes()))

    import serial
    already = run(["findmnt", "-rn", "-o", "TARGET", "-S", "LABEL=RPI-RP2"])
    if already.returncode == 0 and already.stdout.strip():
        steps["bootsel"] = "already in the bootloader (RPI-RP2 mounted)"
        return _write_uf2(cfg, uf2, steps, power_stage)
    port = _port(cfg)
    try:
        boot = exchange(cfg, "bootsel", timeout=2.0)
        if boot["reply"].get("kind") != "OK":
            raise ToolError(f"load refused BOOTSEL: {boot['line']}",
                            hint="VBUS must be off and +OUT discharged (main.rs bootsel_allowed)")
        steps["bootsel"] = boot["line"]
    except ToolError as e:
        if "refused" in str(e):
            raise
        # pico-sdk port firmware: no console, but it honours the 1200-baud touch.
        with serial.Serial(port, 1200):
            time.sleep(0.2)
        steps["bootsel"] = "1200-baud touch"
    return _write_uf2(cfg, uf2, steps, power_stage)


def _write_uf2(cfg: dict, uf2: bytes, steps: dict, power_stage: bool) -> dict:
    mount = _rpi_rp2_mount()
    target = mount / "firmware.uf2"
    with open(target, "wb") as f:
        f.write(uf2)
        f.flush()
    steps["uf2"] = f"{len(uf2) // 512} blocks -> {target}"
    v = _wait_console(cfg)
    return {"ok": True, "elf": rel(ELF), "power_stage": power_stage, "steps": steps, "version": v["line"]}


