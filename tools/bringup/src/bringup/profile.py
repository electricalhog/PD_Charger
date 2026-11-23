"""Statistical CPU profile over SWD while the core runs: no firmware support needed.

Each sample reads three words in one hot-plug session, back to back:
  DWT_PCSR  (0xE000101C)  the program counter, sampled by the DWT on read
  SCB_ICSR  (0xE000ED04)  VECTACTIVE: 0 in thread mode, else exception number
  pxCurrentTCB            the FreeRTOS task that owns thread mode
PCs map to functions through the ELF's symbol table (arm-none-eabi-nm), exception
numbers to IRQ names through the CMSIS device header. The DWT must be enabled
(DEMCR.TRCENA); PCSR reads 0xFFFFFFFF otherwise, and while the core is halted.
Samples come as fast as the programmer issues reads (a few kHz), so this sees
where time goes, not individual ISR timing.
"""

from __future__ import annotations

import bisect
import collections
import re
import struct
import subprocess
from pathlib import Path

from . import probe
from .config import ToolError, gdb_exe, paths, run

DWT_PCSR = 0xE000101C
SCB_ICSR = 0xE000ED04
DEMCR = 0xE000EDFC
TCB_NAME_OFFSET_DEFAULT = 0x34   # FreeRTOS TCB_t.pcTaskName on this port (checked with gdb when available)
CORE_EXCEPTIONS = {2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault", 11: "SVCall",
                   12: "DebugMon", 14: "PendSV", 15: "SysTick"}


def _text_symbols(elf: Path) -> tuple[list[int], list[tuple[int, int, str]]]:
    cp = run(["arm-none-eabi-nm", "-S", "--defined-only", str(elf)], timeout=30)
    syms = []
    for line in cp.stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "tTW":
            syms.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
    syms.sort()
    return [s[0] for s in syms], syms


def _function_of(pc: int, starts: list[int], syms: list[tuple[int, int, str]]) -> str:
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0:
        a, n, name = syms[i]
        if a <= pc < a + max(n, 2):
            return name
    return f"?{pc:#010x}"


def _irq_names(cfg: dict) -> dict[int, str]:
    """Exception number -> name, from the CMSIS device header's IRQn_Type."""
    names = dict(CORE_EXCEPTIONS)
    root = paths(cfg).project_dir
    for h in root.glob("Drivers/CMSIS/Device/ST/*/Include/stm32*xx.h"):
        if re.fullmatch(r"stm32[a-z]\d{3}xx\.h", h.name):
            for m in re.finditer(r"^\s*(\w+)_IRQn\s*=\s*(\d+)\s*,", h.read_text(errors="replace"), re.M):
                names[int(m.group(2)) + 16] = m.group(1)
    return names


def _sample(cfg: dict, tcb_addr: int, n: int, batch: int = 200) -> list[tuple[int, int, int]]:
    """n samples of (pc, icsr, tcb), reads kept in order (read_regions collapses repeats)."""
    out: list[tuple[int, int, int]] = []
    rx = re.compile(r"^\s*0x([0-9A-Fa-f]{8})\s*:\s*([0-9A-Fa-f]{8})", re.M)
    while len(out) < n:
        k = min(batch, n - len(out))
        args = [*probe._connect(cfg, "HOTPLUG")]
        for _ in range(k):
            args += ["-r32", f"{DWT_PCSR:#010x}", "4", "-r32", f"{SCB_ICSR:#010x}", "4", "-r32", f"{tcb_addr:#010x}", "4"]
        text, rc = probe._prun(cfg, args, timeout=120)
        probe._check(text, rc, "PC sampling read")
        vals = [(int(a, 16), int(w, 16)) for a, w in rx.findall(text)]
        want = [DWT_PCSR, SCB_ICSR, tcb_addr]
        for j in range(0, len(vals) - 2, 3):
            if [vals[j][0], vals[j + 1][0], vals[j + 2][0]] == want:
                out.append((vals[j][1], vals[j + 1][1], vals[j + 2][1]))
        if k and not vals:
            raise ToolError("PC sampling returned no data", output_tail=text[-800:])
    return out


def _task_names(cfg: dict, tcbs: set[int]) -> dict[int, str]:
    off = TCB_NAME_OFFSET_DEFAULT
    gdb = gdb_exe()
    if gdb:
        cp = subprocess.run([gdb, "-batch", "-nx", "-ex", "print (int)&((TCB_t*)0)->pcTaskName", str(paths(cfg).elf)],
                            capture_output=True, text=True, timeout=60)
        if m := re.search(r"=\s*(\d+)", cp.stdout):
            off = int(m.group(1))
    tcbs = [t for t in tcbs if 0x20000000 <= t < 0x20100000]
    if not tcbs:
        return {}
    data = probe.read_regions(cfg, [(t + off, 16) for t in tcbs])
    return {t: d.split(b"\0")[0].decode(errors="replace") or f"tcb@{t:#x}" for t, d in zip(tcbs, data)}


def _source_lines(elf: Path, pcs: list[int]) -> dict[int, str]:
    if not pcs:
        return {}
    cp = run(["arm-none-eabi-addr2line", "-e", str(elf), "-f", "-C", "-i", *[f"{p:#x}" for p in pcs]], timeout=60)
    lines, out = cp.stdout.splitlines(), {}
    # -i may print inlined frames: pair output by walking with a lookahead on the "file:line" rows
    i = 0
    for pc in pcs:
        frames = []
        while i + 1 < len(lines):
            frames.append(f"{lines[i]} {Path(lines[i + 1].split(' ')[0]).name}")
            i += 2
            if i >= len(lines) or ":" in lines[i]:   # next row is another file:line -> still inlined chain
                if i < len(lines) and ":" in lines[i] and not lines[i].startswith("?"):
                    continue
            break
        out[pc] = " <- ".join(frames)
    return out


def profile(cfg: dict, samples: int = 1000, top: int = 25, lines: int = 0) -> dict:
    elf = paths(cfg).elf
    if not elf.exists():
        raise ToolError(f"ELF not found: {elf} (run build first)")
    syms = probe.symbols(cfg)
    if "pxCurrentTCB" not in syms:
        raise ToolError("pxCurrentTCB not in the ELF (not a FreeRTOS build?)")
    (demcr,) = probe.read_regions(cfg, [(DEMCR, 4)])
    if not struct.unpack("<I", demcr)[0] & (1 << 24):
        raise ToolError("DWT is off (DEMCR.TRCENA clear): PCSR reads nothing useful",
                        fix="bu mem write 0xE000EDFC 0x01000000   (or enable it in firmware init)")
    starts, text = _text_symbols(elf)
    irqs = _irq_names(cfg)
    raw = _sample(cfg, syms["pxCurrentTCB"][0], samples)
    tasks = _task_names(cfg, {t for _, _, t in raw})

    # The three reads of a sample are microseconds apart and the ISRs here are shorter than that, so
    # the PC and the context are two independent statistics: never cross-tabulate them.
    by_fn, by_ctx, by_pc = collections.Counter(), collections.Counter(), collections.Counter()
    halted = 0
    for pc, icsr, tcb in raw:
        if pc == 0xFFFFFFFF:
            halted += 1
            continue
        by_fn[_function_of(pc, starts, text)] += 1
        by_pc[pc] += 1
        vect = icsr & 0x1FF
        by_ctx[f"ISR {irqs.get(vect, f'exc{vect}')}" if vect else f"task {tasks.get(tcb, f'{tcb:#x}')}"] += 1
    n = sum(by_ctx.values()) or 1
    pct = lambda c: round(100.0 * c / n, 1)  # noqa: E731
    out = {
        "ok": True, "samples": len(raw), "halted_samples": halted,
        "contexts": [{"context": c, "pct": pct(k)} for c, k in by_ctx.most_common()],
        "functions": [{"fn": f, "pct": pct(k)} for f, k in by_fn.most_common(top)],
        "note": "statistical, percent of samples. contexts (SCB_ICSR / pxCurrentTCB) and functions (DWT_PCSR) are "
                "separate reads microseconds apart, so they are independent marginals, not a cross-tabulation",
    }
    if lines:
        hot = [pc for pc, _ in by_pc.most_common(lines)]
        src = _source_lines(elf, hot)
        out["lines"] = [{"pc": f"{pc:#010x}", "pct": pct(by_pc[pc]), "where": src.get(pc, "")} for pc in hot]
    return out
