"""Headless STM32CubeMX code generation with a safety report.

Generation always runs in a throw-away copy of the project. The result is
compared against the real tree and only copied back when ``apply=True`` and no
USER CODE section was lost. The report separates:

* ``user_code_lost``   - USER CODE bodies that changed/disappeared (should never happen;
                         blocks apply unless forced).
* ``generated_diffs``  - changes outside USER CODE. Expected where the .ioc changed;
                         anything else is a hand edit to generated code that
                         regeneration will clobber.
"""

from __future__ import annotations

import difflib
import hashlib
import os
import shutil
import tempfile
import time
from pathlib import Path

from . import usercode
from .config import ToolError, cubemx_exe, out_file, paths, rel, run, tail
from .ioc import Ioc

_IGNORE = shutil.ignore_patterns("build", ".git", "mx.scratch", "*_save")
_TEXT_SUFFIXES = {".c", ".h", ".s", ".txt", ".cmake", ".ld", ".mxproject", ".cproject", ".project", ".json", ".ioc"}


def _hash_tree(root: Path) -> dict[str, str]:
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ("build", ".git")]
        for f in filenames:
            p = Path(dirpath) / f
            out[str(p.relative_to(root))] = hashlib.sha1(p.read_bytes()).hexdigest()
    return out


def _parse_script_output(stdout: str, commands: list[str]) -> list[dict]:
    """CubeMX echoes each script command, then prints OK or KO. Exit code is always 0."""
    results, current = [], None
    for line in stdout.splitlines():
        s = line.strip()
        if s in commands:
            current = {"command": s, "status": None}
            results.append(current)
        elif current and current["status"] is None and s in ("OK", "KO"):
            current["status"] = s
    return results


def run_script(cfg: dict, commands: list[str]) -> dict:
    exe = cubemx_exe(cfg)
    if not exe:
        raise ToolError("STM32CubeMX not found; set [cubemx].path in bringup.local.toml")
    commands = [*commands, "exit"]
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(commands) + "\n")
        script = f.name
    env_cmd = [exe, "-q", script]
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY") and shutil.which("xvfb-run"):
        env_cmd = ["xvfb-run", "-a", *env_cmd]
    t0 = time.monotonic()
    try:
        cp = run(env_cmd, timeout=cfg.get("cubemx", {}).get("timeout_s", 600))
    finally:
        os.unlink(script)
    log = out_file(cfg, "cubemx", "log")
    log.write_text(cp.stdout + "\n--- stderr ---\n" + cp.stderr)
    results = _parse_script_output(cp.stdout, commands[:-1])
    ok = len(results) == len(commands) - 1 and all(r["status"] == "OK" for r in results)
    return {"ok": ok, "commands": results, "seconds": round(time.monotonic() - t0, 1), "log": rel(log),
            **({} if ok else {"stdout_tail": tail(cp.stdout, 30)})}


def _read_text(p: Path) -> str | None:
    if p.suffix not in _TEXT_SUFFIXES:
        return None
    try:
        with p.open(newline="") as f:
            return f.read()
    except (UnicodeDecodeError, OSError):
        return None


def generate(cfg: dict, apply: bool = False, force: bool = False, diff_lines: int = 200) -> dict:
    P = paths(cfg)
    ioc = Ioc(P.ioc)
    warnings = []
    if ioc.get("ProjectManager.KeepUserCode") != "true":
        warnings.append("ProjectManager.KeepUserCode is not 'true': CubeMX will DISCARD user code")
        if apply and not force:
            raise ToolError("refusing to apply: KeepUserCode is not true (use --force to override)")

    tmp = Path(tempfile.mkdtemp(prefix="bringup-mx-"))
    work = tmp / P.project_dir.name
    try:
        shutil.copytree(P.project_dir, work, ignore=_IGNORE, symlinks=True)
        before_uc = usercode.snapshot(work)
        before = _hash_tree(work)
        gen = run_script(cfg, [f"config load {work / P.ioc.name}", "project generate"])
        if not gen["ok"]:
            return {"ok": False, "applied": False, "generator": gen, "warnings": warnings}

        after = _hash_tree(work)
        after_uc = usercode.snapshot(work)
        added = sorted(after.keys() - before.keys())
        removed = sorted(before.keys() - after.keys())
        modified = sorted(k for k in before.keys() & after.keys() if before[k] != after[k])

        lost = []
        for f, secs in before_uc.items():
            new = after_uc.get(f, {})
            for sid, h in secs.items():
                if new.get(sid) != h:
                    lost.append({"file": f, "section": sid, "state": "missing" if sid not in new else "changed"})

        full_diff, gen_diffs = [], []
        for f in modified:
            a, b = _read_text(P.project_dir / f), _read_text(work / f)
            if a is None or b is None:
                gen_diffs.append({"file": f, "binary_or_unreadable": True})
                continue
            sl = usercode._splitlines
            full_diff += difflib.unified_diff(sl(a), sl(b), f"a/{f}", f"b/{f}")
            ga, gb = usercode.strip_user_code(a), usercode.strip_user_code(b)
            if ga != gb:
                d = list(difflib.unified_diff(sl(ga), sl(gb), f"a/{f}", f"b/{f}", n=2))
                gen_diffs.append({"file": f, "lines_changed": sum(1 for x in d if x[:1] in "+-") - 2, "diff": d})

        diff_path = out_file(cfg, "cubemx_diff", "patch")
        diff_path.write_text("".join(full_diff))

        # Budget the inline diff so the JSON stays readable; full patch is on disk.
        budget = diff_lines
        for g in gen_diffs:
            if "diff" in g:
                g["diff"], budget = "".join(g["diff"][:max(budget, 0)]), budget - len(g["diff"])
                if budget < 0:
                    g["truncated"] = True

        applied = False
        if apply:
            if lost and not force:
                warnings.append("not applied: USER CODE would be lost (use --force to override)")
            else:
                for f in added + modified:
                    dst = P.project_dir / f
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(work / f, dst)
                applied = True

        return {
            "ok": not lost,
            "applied": applied,
            "dry_run": not apply,
            "generator": gen,
            "files_added": added,
            "files_modified": modified,
            "files_removed_by_cubemx": removed,
            "user_code_lost": lost,
            "generated_diffs": gen_diffs,
            "full_patch": rel(diff_path),
            "warnings": warnings,
        }
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
