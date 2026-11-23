"""Tools that are not a `bu` command: bench state, server version, PC sampling, the offline
test suite, and the allowlisted shell. Each is small and goes through the same worker.
"""

from __future__ import annotations

import glob
import hashlib
import os
import re
import shutil
import tomllib
from pathlib import Path
from typing import Any

from mcp.types import Tool, ToolAnnotations

from . import __version__
from .config import Config
from .gate import read_state
from .runner import Outcome, Worker

READ = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)


def extra_tools(cfg: Config) -> list[Tool]:
    tools = [
        Tool(
            name="bench_state",
            title="Bench interlock state",
            description=("The bench interlock as this server reads it right now: power_stage is "
                         "attached, absent or unset, plus pending confirmation tokens (command and expiry, "
                         "never the token itself). Call this before any actuate tool. unset means every "
                         "actuate call is refused until Dan runs `bringup-mcp bench ...` on the bench host. "
                         "Effect: read."),
            inputSchema={"type": "object", "properties": {}, "additionalProperties": False},
            annotations=READ,
        ),
        Tool(
            name="server_version",
            title="Which code is running",
            description=("Identifies the running code: this server's version, the repo commit and whether "
                         "tools/ is modified, the sha256 of the `bu schema` the tools were generated from, "
                         "and the config path. Use it when behaviour does not match what you were told. "
                         "Effect: read."),
            inputSchema={"type": "object", "properties": {}, "additionalProperties": False},
            annotations=READ,
        ),
        Tool(
            name="probe_pc_sample",
            title="Sample the program counter",
            description=("Where is the core executing? Attaches over SWD in hot-plug mode n times, reads PC "
                         "and LR each time, and resolves them to function names with addr2line against the "
                         "built ELF. This is how the fault-loop and CPU-saturation bugs were found on "
                         "2026-09-27. The core keeps running. Not yet run through this server on the bench. "
                         "Effect: read (hot-plug attach only)."),
            inputSchema={
                "type": "object",
                "properties": {
                    "samples": {"type": "integer", "minimum": 1, "maximum": 20, "description": "How many attach-and-read cycles (each ~0.3 s)."},
                },
                "required": ["samples"],
                "additionalProperties": False,
            },
            annotations=READ,
        ),
        Tool(
            name="bu_test",
            title="Run the offline test suite",
            description=("Runs `uv run pytest -q` in tools/bringup (the 27 offline tests: ioc round-trip, USER "
                         "CODE edits, CubeMX parsing, regulator decoding, analysis, fake scope, schema). "
                         "Touches no hardware. Returns the exit code and the last lines. Effect: write "
                         "(creates .pytest_cache)."),
            inputSchema={"type": "object", "properties": {}, "additionalProperties": False},
            annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False),
        ),
    ]
    if cfg.shell.enabled:
        tools.append(Tool(
            name="shell_run",
            title="Run an allowlisted program in the repo",
            description=(f"Runs one program from the allowlist {list(cfg.shell.allow)} with an argv list "
                         "(no shell, no globbing, no pipes), cwd pinned inside the repo checkout, timeout "
                         f"{cfg.shell.timeout_s:g} s, output capped at {cfg.shell.max_output_bytes} bytes with "
                         "the full size reported. Refused: sudo, any program not on the list, `git push` to "
                         f"{list(cfg.shell.deny_git_push_to)} or with --force, `gh pr merge`. Effect: whatever "
                         "the program does; treat as destructive. Not reversible by this server."),
            inputSchema={
                "type": "object",
                "properties": {
                    "argv": {"type": "array", "items": {"type": "string"}, "minItems": 1,
                             "description": "Program and arguments, e.g. [\"git\", \"status\", \"--short\"]."},
                    "cwd": {"type": "string", "description": "Directory relative to the repo root. Default: the repo root."},
                    "stdin": {"type": "string", "description": "Text to feed on stdin."},
                    "timeout_s": {"type": "number", "minimum": 1, "maximum": cfg.shell.timeout_s},
                },
                "required": ["argv"],
                "additionalProperties": False,
            },
            annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=True),
        ))
    return tools


# ------------------------------------------------------------------ handlers

async def bench_state(cfg: Config) -> Outcome:
    st = read_state(cfg.state_file)
    return Outcome(True, result=st.public())


async def server_version(cfg: Config, worker: Worker, schema_sha: str) -> Outcome:
    head = await worker.run_raw(["git", "rev-parse", "HEAD"], cwd=cfg.repo_root, timeout=10, call={"command": "git rev-parse"}, stdin=None)
    branch = await worker.run_raw(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=cfg.repo_root, timeout=10, call={"command": "git branch"}, stdin=None)
    dirty = await worker.run_raw(["git", "status", "--porcelain", "--", "tools"], cwd=cfg.repo_root, timeout=10, call={"command": "git status"}, stdin=None)
    res: dict[str, Any] = {
        "server_version": __version__,
        "config_path": str(cfg.path),
        "repo_root": str(cfg.repo_root),
        "bu": str(cfg.bu),
        "schema_sha256": schema_sha,
        "bench": read_state(cfg.state_file).public(),
        "shell_enabled": cfg.shell.enabled,
    }
    res["repo_commit"] = head.details.get("stdout", "").strip() if head.ok else "unknown"
    res["repo_branch"] = branch.details.get("stdout", "").strip() if branch.ok else "unknown"
    if dirty.ok:
        res["tools_dirty"] = bool(dirty.details.get("stdout", "").strip())
    else:
        res["tools_dirty"] = "unknown"
    return Outcome(True, result=res)


def _programmer_exe(cfg: Config) -> str | None:
    """Same search as bringup.config.programmer_exe, without importing Dan's package."""
    local = cfg.repo_root / "tools/bringup/bringup.local.toml"
    base = cfg.repo_root / "tools/bringup/bringup.toml"
    configured = ""
    for p in (base, local):
        if p.exists():
            try:
                configured = tomllib.loads(p.read_text()).get("probe", {}).get("programmer", configured) or configured
            except tomllib.TOMLDecodeError:
                pass
    if configured:
        return shutil.which(configured) or (configured if Path(configured).exists() else None)
    if w := shutil.which("STM32_Programmer_CLI"):
        return w
    for g in ("~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
              "/usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
              "/opt/st/stm32cubeide_*/plugins/*cubeprogrammer*/tools/bin/STM32_Programmer_CLI"):
        hits = sorted(glob.glob(os.path.expanduser(g)), reverse=True)
        if hits:
            return hits[0]
    return None


def _elf_path(cfg: Config) -> Path | None:
    base = cfg.repo_root / "tools/bringup/bringup.toml"
    if not base.exists():
        return None
    try:
        pc = tomllib.loads(base.read_text()).get("project", {})
    except tomllib.TOMLDecodeError:
        return None
    d = cfg.repo_root / pc.get("dir", "")
    elf = pc.get("elf_cmake") if pc.get("build_system", "cmake") == "cmake" else pc.get("elf_make")
    return (d / elf) if elf else None


_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_REG = re.compile(r"^\s*(PC|LR)\s*:?\s*(0x[0-9A-Fa-f]+)", re.M)


async def probe_pc_sample(cfg: Config, worker: Worker, samples: int) -> Outcome:
    prog = _programmer_exe(cfg)
    if not prog:
        return Outcome(False, "not_configured", "STM32_Programmer_CLI not found (set [probe].programmer in tools/bringup/bringup.local.toml)")
    elf = _elf_path(cfg)
    addr2line = shutil.which("arm-none-eabi-addr2line")
    out: list[dict[str, Any]] = []
    for i in range(samples):
        r = await worker.run_raw([prog, "-c", "port=SWD", "mode=HOTPLUG", "-coreReg"], cwd=cfg.repo_root,
                                 timeout=20, call={"command": "probe pc-sample", "i": i}, stdin=None)
        if not r.ok:
            return Outcome(False, r.error_type, r.message, details={"samples_so_far": out}, call=r.call)
        text = _ANSI.sub("", r.details.get("stdout", "") + r.details.get("stderr", ""))
        regs = {m.group(1): m.group(2) for m in _REG.finditer(text)}
        if "PC" not in regs:
            return Outcome(False, "bad_output", "no PC register in programmer output (no probe? core held?)",
                           details={"output_tail": text[-1500:], "samples_so_far": out}, call=r.call)
        sample: dict[str, Any] = {"pc": regs["PC"]}
        if "LR" in regs:
            sample["lr"] = regs["LR"]
        if elf and elf.exists() and addr2line:
            addrs = [regs["PC"]] + ([regs["LR"]] if "LR" in regs else [])
            a = await worker.run_raw([addr2line, "-f", "-e", str(elf), *addrs],
                                     cwd=cfg.repo_root, timeout=10, call={"command": "addr2line"}, stdin=None)
            if a.ok:
                lines = a.details.get("stdout", "").splitlines()
                # addr2line -f prints function then file:line, per address
                if len(lines) >= 2:
                    sample["pc_function"], sample["pc_location"] = lines[0], lines[1]
                if len(lines) >= 4:
                    sample["lr_function"], sample["lr_location"] = lines[2], lines[3]
        out.append(sample)
    res: dict[str, Any] = {"samples": out, "count": len(out), "programmer": prog}
    if not (elf and elf.exists()):
        res["symbols"] = "unresolved: built ELF not found; run bu build first"
    elif not addr2line:
        res["symbols"] = "unresolved: arm-none-eabi-addr2line not on PATH"
    return Outcome(True, result=res)


async def bu_test(cfg: Config, worker: Worker) -> Outcome:
    uv = shutil.which("uv")
    if not uv:
        return Outcome(False, "not_configured", "uv not on PATH")
    r = await worker.run_raw([uv, "run", "--quiet", "pytest", "-q"], cwd=cfg.repo_root / "tools/bringup",
                             timeout=600, call={"command": "pytest"}, stdin=None)
    if not r.ok:
        return r
    rc = r.details.get("returncode")
    tail = "\n".join((r.details.get("stdout", "") + r.details.get("stderr", "")).splitlines()[-25:])
    res = {"returncode": rc, "passed": rc == 0, "tail": tail}
    if rc == 0:
        return Outcome(True, result=res, call=r.call)
    return Outcome(False, "bu_error", f"pytest exit {rc}", result=res, call=r.call)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


async def shell_run(cfg: Config, worker: Worker, argv: list[str], cwd: str | None, stdin: str | None,
                    timeout_s: float | None) -> Outcome:
    if not cfg.shell.enabled:
        return Outcome(False, "refused", "shell_disabled", details={"how_to_proceed": "Dan enables [shell] in bringup-mcp.toml on the bench host."})
    if not argv or not all(isinstance(a, str) for a in argv):
        return Outcome(False, "invalid_arguments", "argv must be a non-empty list of strings")
    if any("\x00" in a for a in argv):
        return Outcome(False, "invalid_arguments", "argv contains a NUL byte")
    prog = Path(argv[0]).name
    if prog == "sudo":
        return Outcome(False, "refused", "sudo_never", details={"how_to_proceed": "Anything needing root is Dan's to run."})
    if prog not in cfg.shell.allow:
        return Outcome(False, "refused", "program_not_allowlisted",
                       details={"program": prog, "allow": list(cfg.shell.allow),
                                "how_to_proceed": "Ask Dan to add it to [shell] allow if it belongs there."})
    if prog == "git" and len(argv) > 1 and argv[1] == "push":
        rest = argv[2:]
        if any(a in ("-f", "--force", "--force-with-lease") or a.startswith("--force") for a in rest):
            return Outcome(False, "refused", "git_push_force", details={"how_to_proceed": "Force pushes are Dan's to run."})
        targets = [a for a in rest if not a.startswith("-")][1:]  # after the remote
        branch_names = {t.split(":")[-1].removeprefix("refs/heads/") for t in targets}
        if not targets:
            cur = await worker.run_raw(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=cfg.repo_root, timeout=10,
                                       call={"command": "git branch"}, stdin=None)
            branch_names = {cur.details.get("stdout", "").strip()} if cur.ok else {"unknown"}
        blocked = branch_names & set(cfg.shell.deny_git_push_to)
        if blocked or "unknown" in branch_names:
            return Outcome(False, "refused", "git_push_protected_branch",
                           details={"branches": sorted(branch_names), "deny": list(cfg.shell.deny_git_push_to),
                                    "how_to_proceed": "Push a feature branch, or ask Dan."})
    if prog == "gh" and len(argv) > 2 and argv[1] == "pr" and argv[2] == "merge":
        return Outcome(False, "refused", "gh_pr_merge", details={"how_to_proceed": "Merging is Dan's decision; open the PR and stop."})
    root = cfg.repo_root
    wd = (root / cwd).resolve() if cwd else root
    if root not in (wd, *wd.parents):
        return Outcome(False, "refused", "cwd_outside_repo", details={"cwd": str(wd), "repo_root": str(root)})
    if not wd.is_dir():
        return Outcome(False, "invalid_arguments", f"cwd is not a directory: {wd}")
    timeout = min(float(timeout_s), cfg.shell.timeout_s) if timeout_s else cfg.shell.timeout_s
    exe = shutil.which(argv[0]) if "/" not in argv[0] else argv[0]
    if not exe:
        return Outcome(False, "not_configured", f"{argv[0]} is allowlisted but not on PATH")
    r = await worker.run_raw([exe, *argv[1:]], cwd=wd, timeout=timeout,
                             call={"command": "shell", "argv": argv, "cwd": str(wd), "timeout_s": timeout}, stdin=stdin)
    if not r.ok:
        return r
    cap = cfg.shell.max_output_bytes
    res: dict[str, Any] = {"returncode": r.details.get("returncode")}
    for k in ("stdout", "stderr"):
        text = r.details.get(k, "")
        b = text.encode()
        res[f"{k}_bytes"] = len(b)
        if len(b) > cap:
            res[k] = b[:cap].decode(errors="replace")
            res[f"{k}_truncated"] = True
        else:
            res[k] = text
    return Outcome(True, result=res, call=r.call)
