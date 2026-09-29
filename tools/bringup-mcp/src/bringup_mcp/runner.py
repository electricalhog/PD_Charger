"""Run `bu` (and other bench programs) as subprocesses: one at a time, with a timeout, and
turn what happened into one of a small set of typed outcomes.

Outcome types, shared by every tool:
- ok            the program ran and said ok
- bu_error      bu ran and returned {"ok": false, ...}; its JSON is carried as details
- refused       a gate said no (see gate.py); nothing ran
- timed_out     the process exceeded its timeout and was killed
- not_configured  the launcher or program is missing or not executable
- invalid_arguments  the tool arguments do not map to a valid command line
- bad_output    the program exited but did not print one JSON object
- internal      a bug in this server; the traceback is in details
"""

from __future__ import annotations

import asyncio
import json
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .config import Config

ERROR_TYPES = ("bu_error", "refused", "timed_out", "not_configured", "invalid_arguments", "bad_output", "internal")


@dataclass
class Outcome:
    ok: bool
    error_type: str | None = None
    message: str | None = None
    result: Any = None          # bu's JSON, unchanged
    details: dict[str, Any] = field(default_factory=dict)
    call: dict[str, Any] = field(default_factory=dict)

    def payload(self) -> dict[str, Any]:
        d: dict[str, Any] = {"ok": self.ok}
        if not self.ok:
            d["error_type"] = self.error_type
            d["error"] = self.message
        if self.result is not None:
            d["result"] = self.result
        if self.details:
            d["details"] = self.details
        if self.call:
            d["_call"] = self.call
        return d


class Worker:
    """One ST-LINK, one scope: every external call goes through this lock, in order."""

    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self._lock = asyncio.Lock()
        self.calls = 0

    async def run_bu(self, argv: list[str], command: str, timeout_s: float | None = None) -> Outcome:
        timeout = timeout_s if timeout_s is not None else self.cfg.timeout_for(command)
        full = [str(self.cfg.bu), *argv]
        return await self.run_json(full, cwd=self.cfg.repo_root, timeout=timeout,
                                   call={"command": command, "argv": argv, "timeout_s": timeout})

    async def run_json(self, full_argv: list[str], cwd: Path, timeout: float, call: dict[str, Any]) -> Outcome:
        """Run a program that prints exactly one JSON object on stdout."""
        raw = await self.run_raw(full_argv, cwd=cwd, timeout=timeout, call=call, stdin=None)
        if not raw.ok:
            return raw
        stdout: str = raw.details.pop("stdout", "")
        stderr: str = raw.details.pop("stderr", "")
        rc: int = raw.details.pop("returncode", 0)
        try:
            obj = json.loads(stdout)
        except json.JSONDecodeError:
            return Outcome(False, "bad_output", "program did not print one JSON object on stdout",
                           details={"returncode": rc, "stdout_tail": stdout[-2000:], "stderr_tail": stderr[-2000:]},
                           call=raw.call)
        if isinstance(obj, dict) and obj.get("ok") is False:
            return Outcome(False, "bu_error", str(obj.get("error", "bu reported ok=false")),
                           result=obj, details={"returncode": rc}, call=raw.call)
        return Outcome(True, result=obj, call=raw.call)

    async def run_raw(self, full_argv: list[str], cwd: Path, timeout: float, call: dict[str, Any],
                      stdin: str | None) -> Outcome:
        """Run any program; stdout/stderr/returncode land in details. Serialized on the worker lock."""
        queued_from = time.monotonic()
        async with self._lock:
            queued_s = round(time.monotonic() - queued_from, 3)
            self.calls += 1
            started = time.monotonic()
            call = {**call, "queued_s": queued_s}
            try:
                proc = await asyncio.create_subprocess_exec(
                    *full_argv, cwd=str(cwd),
                    stdin=asyncio.subprocess.PIPE if stdin is not None else asyncio.subprocess.DEVNULL,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                )
            except FileNotFoundError:
                return Outcome(False, "not_configured", f"program not found: {full_argv[0]}", call=call)
            except PermissionError:
                return Outcome(False, "not_configured", f"program not executable: {full_argv[0]}", call=call)
            except Exception as e:  # noqa: BLE001
                return Outcome(False, "internal", f"{type(e).__name__}: {e}",
                               details={"traceback": traceback.format_exc(limit=5)}, call=call)
            try:
                out_b, err_b = await asyncio.wait_for(
                    proc.communicate(stdin.encode() if stdin is not None else None), timeout=timeout)
            except asyncio.TimeoutError:
                proc.kill()
                try:
                    await asyncio.wait_for(proc.wait(), timeout=5)
                except asyncio.TimeoutError:
                    pass
                call["seconds"] = round(time.monotonic() - started, 3)
                return Outcome(False, "timed_out", f"killed after {timeout}s", call=call)
            call["seconds"] = round(time.monotonic() - started, 3)
            return Outcome(True, details={
                "stdout": out_b.decode(errors="replace"),
                "stderr": err_b.decode(errors="replace"),
                "returncode": proc.returncode,
            }, call=call)
