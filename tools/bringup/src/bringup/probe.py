"""ST-LINK operations: list probes, flash, reset, and live memory access by ELF symbol.

Memory reads/writes use hot-plug attach, so the core keeps running (the
``debug_log`` ring buffer and other globals can be sampled without halting).
"""

from __future__ import annotations

import re
import struct
import tempfile
from pathlib import Path

from .config import ToolError, gdb_exe, out_file, paths, programmer_exe, rel, run, tail

# ------------------------------------------------------------------ helpers

_FMT = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i", "f32": "f", "u64": "Q", "i64": "q", "f64": "d"}


_ANSI = re.compile(r"\x1b\[[0-9;]*m")


def _prun(cfg: dict, args: list[str], timeout: float) -> tuple[str, int]:
    """Run STM32_Programmer_CLI and return (ANSI-stripped combined output, returncode)."""
    cp = run([_prog(cfg), *args], timeout=timeout)
    return _ANSI.sub("", cp.stdout + cp.stderr), cp.returncode


def _prog(cfg: dict) -> str:
    exe = programmer_exe(cfg)
    if not exe:
        raise ToolError("STM32_Programmer_CLI not found; set [probe].programmer in bringup.local.toml")
    return exe


def _connect(cfg: dict, mode: str | None = None) -> list[str]:
    c = ["-c", "port=SWD"]
    if sn := cfg.get("probe", {}).get("serial"):
        c.append(f"sn={sn}")
    if mode:
        c.append(f"mode={mode}")
    return c


def _check(out: str, rc: int, what: str) -> None:
    if rc != 0 or re.search(r"^\s*Error:", out, re.M):
        errs = re.findall(r"^\s*Error:.*$", out, re.M)
        raise ToolError(f"{what} failed", errors=[e.strip() for e in errs][:10], output_tail=tail(out, 25))


def _openocd_base(cfg: dict) -> list[str]:
    pc = cfg.get("probe", {})
    base = ["openocd", "-f", "interface/stlink.cfg"]
    if sn := pc.get("serial"):
        base += ["-c", f"adapter serial {sn}"]
    return base + ["-f", pc.get("openocd_target", "target/stm32g4x.cfg")]


def _is_peripheral(addr: int) -> bool:
    return 0x40000000 <= addr < 0x60000000


def _backend(cfg: dict) -> str:
    return cfg.get("probe", {}).get("backend", "cubeprog")


# ------------------------------------------------------------------ probes / flash

def list_probes(cfg: dict) -> dict:
    out, _ = _prun(cfg, ["-l", "st-link"], timeout=30)
    probes, cur = [], None
    for line in out.splitlines():
        if m := re.match(r"\s*ST-Link Probe (\d+)\s*:", line):
            cur = {"index": int(m.group(1))}
            probes.append(cur)
        elif cur is not None and (m := re.match(r"\s*([^:]+?)\s*:\s*(.+)$", line)):
            cur[re.sub(r"\W+", "_", m.group(1).strip().lower())] = m.group(2).strip()
    return {"ok": True, "count": len(probes), "probes": probes}


def flash(cfg: dict, elf: str | None = None, verify: bool = True, reset: bool = True) -> dict:
    P = paths(cfg)
    image = Path(elf) if elf else P.elf
    if not image.exists():
        raise ToolError(f"image not found: {image} (run build first)")
    if _backend(cfg) == "openocd":
        cmd = [*_openocd_base(cfg), "-c", f"program {image} {'verify ' if verify else ''}{'reset ' if reset else ''}exit"]
        cp = run(cmd, timeout=120)
        out = cp.stdout + cp.stderr
        if cp.returncode:
            raise ToolError("openocd flash failed", output_tail=tail(out, 25))
        return {"ok": True, "backend": "openocd", "image": rel(image), "verified": verify and "Verified OK" in out}
    args = [*_connect(cfg), "-w", str(image)]
    if verify:
        args.append("-v")
    if reset:
        args.append("-rst")
    out, rc = _prun(cfg, args, timeout=120)
    _check(out, rc, "flash")
    return {
        "ok": True,
        "backend": "cubeprog",
        "image": rel(image),
        "downloaded": "File download complete" in out,
        "verified": ("Download verified successfully" in out) if verify else None,
        "reset": reset,
    }


def reset(cfg: dict, hard: bool = False) -> dict:
    if _backend(cfg) == "openocd":
        cp = run([*_openocd_base(cfg), "-c", "init; reset run; exit"], timeout=30)
        if cp.returncode:
            raise ToolError("openocd reset failed", output_tail=tail(cp.stdout + cp.stderr))
        return {"ok": True}
    out, rc = _prun(cfg, [*_connect(cfg, "HOTPLUG"), "-hardRst" if hard else "-rst"], timeout=30)
    _check(out, rc, "reset")
    return {"ok": True, "hard": hard}


# ------------------------------------------------------------------ symbols

def symbols(cfg: dict, pattern: str = "") -> dict[str, tuple[int, int]]:
    elf = paths(cfg).elf
    if not elf.exists():
        raise ToolError(f"ELF not found: {elf} (run build first)")
    cp = run(["arm-none-eabi-nm", "-S", "--defined-only", str(elf)], timeout=30)
    rx = re.compile(pattern) if pattern else None
    out = {}
    for line in cp.stdout.splitlines():
        parts = line.split()
        if len(parts) == 4 and (rx is None or rx.search(parts[3])):
            out[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return out


def resolve(cfg: dict, target: str) -> tuple[int, int | None, str]:
    """'0x20000000', 'symbol' or 'symbol+0x10' -> (address, symbol size or None, label)."""
    m = re.fullmatch(r"(?P<base>[^+]+?)(?:\+(?P<off>0x[0-9a-fA-F]+|\d+))?", target.strip())
    if not m:
        raise ToolError(f"bad address/symbol: {target}")
    base, off = m["base"], int(m["off"], 0) if m["off"] else 0
    if re.fullmatch(r"0x[0-9a-fA-F]+|\d+", base):
        return int(base, 0) + off, None, target
    syms = symbols(cfg)
    if base not in syms:
        close = [s for s in syms if base.lower() in s.lower()][:10]
        raise ToolError(f"symbol not found: {base}", similar=close)
    addr, size = syms[base]
    return addr + off, size - off, target


def layout(cfg: dict, expr: str) -> dict:
    """Type layout with byte offsets (gdb 'ptype /o'), read from the ELF only - no target needed."""
    gdb = gdb_exe()
    if not gdb:
        raise ToolError("no arm gdb found (arm-none-eabi-gdb or gdb-multiarch)")
    elf = paths(cfg).elf
    cp = run([gdb, "-batch", "-nx", "-ex", f"ptype /o {expr}", "-ex", f"print sizeof({expr})", str(elf)], timeout=60)
    if "No symbol" in cp.stderr or cp.returncode:
        raise ToolError(f"gdb could not resolve '{expr}'", stderr=tail(cp.stderr, 10))
    return {"ok": True, "expr": expr, "ptype": cp.stdout.strip()}


# ------------------------------------------------------------------ memory

def read_regions(cfg: dict, regions: list[tuple[int, int]]) -> list[bytes]:
    """Read several (address, size) regions in ONE probe session (hot-plug, core keeps running)."""
    if _backend(cfg) == "openocd":
        with tempfile.TemporaryDirectory() as td:
            files = [Path(td) / f"r{i}.bin" for i in range(len(regions))]
            cmds = "; ".join(f"dump_image {f} {a:#x} {n}" for f, (a, n) in zip(files, regions))
            cp = run([*_openocd_base(cfg), "-c", f"init; {cmds}; exit"], timeout=60)
            if cp.returncode or not all(f.exists() for f in files):
                raise ToolError("openocd memory read failed", output_tail=tail(cp.stdout + cp.stderr))
            return [f.read_bytes() for f in files]
    args, spans = [*_connect(cfg, "HOTPLUG")], []
    for addr, size in regions:
        aligned = addr & ~3
        n = ((addr + size + 3) & ~3) - aligned
        args += ["-r32", f"{aligned:#010x}", str(n)]
        spans.append((aligned, n, addr - aligned, size))
    out, rc = _prun(cfg, args, timeout=60)
    _check(out, rc, "memory read")
    words: dict[int, int] = {}
    for m in re.finditer(r"^\s*0x([0-9A-Fa-f]{8})\s*:\s*((?:[0-9A-Fa-f]{8}[ \t]*)+)$", out, re.M):
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[base + 4 * i] = int(w, 16)
    result = []
    for aligned, n, start, size in spans:
        try:
            data = b"".join(struct.pack("<I", words[aligned + 4 * i]) for i in range(n // 4))
        except KeyError:
            raise ToolError("could not parse memory dump", output_tail=tail(out, 20)) from None
        result.append(data[start:start + size])
    return result


def _read_bytes(cfg: dict, addr: int, size: int) -> bytes:
    return read_regions(cfg, [(addr, size)])[0]


def mem_read(cfg: dict, target: str, size: int | None = None, dtype: str = "u32", count: int | None = None,
             save: bool = False) -> dict:
    addr, sym_size, label = resolve(cfg, target)
    if dtype not in _FMT:
        raise ToolError(f"unknown type {dtype}", types=list(_FMT))
    esize = struct.calcsize(_FMT[dtype])
    if size is None:
        size = count * esize if count else (sym_size or esize)
    data = _read_bytes(cfg, addr, size)
    n = len(data) // esize
    values = list(struct.unpack(f"<{n}{_FMT[dtype]}", data[:n * esize]))
    result = {"ok": True, "target": label, "address": f"{addr:#010x}", "size": size, "type": dtype,
              "values": values[:256], "truncated": n > 256}
    if save or size > 1024:
        f = out_file(cfg, "mem_" + re.sub(r"\W+", "_", label), "bin")
        f.write_bytes(data)
        result["file"] = rel(f)
    if size <= 256:
        result["hex"] = data.hex(" ")
    return result


def mem_write(cfg: dict, target: str, value: str, dtype: str = "u32", volatile_target: bool = False) -> dict:
    """volatile_target: the firmware consumes the word at once (a mailbox request cleared by an ISR),
    so a failed read-back verify means nothing; the caller confirms by the effect."""
    addr, _, label = resolve(cfg, target)
    if dtype not in _FMT:
        raise ToolError(f"unknown type {dtype}", types=list(_FMT))
    v = float(value) if dtype.startswith("f") else int(value, 0)
    payload = struct.pack("<" + _FMT[dtype], v)
    if _backend(cfg) == "openocd":
        cmds = "; ".join(f"mwb {addr + i:#x} {b:#x}" for i, b in enumerate(payload))
        cp = run([*_openocd_base(cfg), "-c", f"init; {cmds}; exit"], timeout=30)
        if cp.returncode:
            raise ToolError("openocd write failed", output_tail=tail(cp.stdout + cp.stderr))
    else:
        # One aligned bus write where possible so an ISR never sees a torn value.
        n = len(payload)
        if n in (1, 2, 4) and addr % n == 0:
            word = int.from_bytes(payload, "little")
            args = [f"-w{n * 8}", f"{addr:#010x}", f"{word:#x}"]
        elif n == 8 and addr % 4 == 0:
            lo, hi = struct.unpack("<II", payload)
            args = ["-w32", f"{addr:#010x}", f"{lo:#x}", "-w32", f"{addr + 4:#010x}", f"{hi:#x}"]
        else:
            args = [a for i, b in enumerate(payload) for a in ("-w8", f"{addr + i:#010x}", f"{b:#x}")]
        out, rc = _prun(cfg, [*_connect(cfg, "HOTPLUG"), *args], timeout=30)
        if volatile_target and "Failed to download data" in out and "No debug probe" not in out:
            return {"ok": True, "target": label, "address": f"{addr:#010x}", "type": dtype, "written": v,
                    "verified": False, "note": "consumed by the firmware before the programmer's read-back"}
        if _is_peripheral(addr) and "Failed to download data" in out and "No debug probe" not in out:
            # STM32_Programmer_CLI verifies each write by reading it back. Write-only or
            # self-clearing peripheral registers (HRTIM OENR/ODISR, xxICR flag clears, ...)
            # always fail that check although the write itself was performed.
            return {"ok": True, "target": label, "address": f"{addr:#010x}", "type": dtype, "written": v,
                    "verified": False,
                    "note": "peripheral register: write issued, programmer read-back differed "
                            "(normal for write-only/self-clearing registers); confirm via its effect"}
        _check(out, rc, "memory write")
    readback = _read_bytes(cfg, addr, len(payload))
    result = {"ok": readback == payload, "target": label, "address": f"{addr:#010x}", "type": dtype,
              "written": v, "readback": struct.unpack("<" + _FMT[dtype], readback)[0], "verified": readback == payload}
    if readback != payload and _is_peripheral(addr):
        result.update(ok=True, note="peripheral register reads back differently (normal for status/flag bits)")
    return result
