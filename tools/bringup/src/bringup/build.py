"""Build the firmware and return structured diagnostics."""

from __future__ import annotations

import hashlib
import re
import time

from .config import ToolError, out_file, paths, rel, run, tail

_DIAG = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?P<sev>fatal error|error|warning|note):\s*(?P<msg>.*)$")
_LD_ERR = re.compile(r"(undefined reference to .*|region `\S+' overflowed by \d+ bytes|multiple definition of .*|cannot find .*)")
_MEM = re.compile(r"^\s*(?P<region>\w+):\s+(?P<used>\d+(?:\.\d+)?\s*\w?B)\s+(?P<size>\d+(?:\.\d+)?\s*\w?B)\s+(?P<pct>[\d.]+)%")


def _parse(output: str, project_dir) -> dict:
    diags, seen = [], set()
    for line in output.splitlines():
        if m := _DIAG.match(line.strip()):
            d = m.groupdict()
            key = (d["file"], d["line"], d["msg"])
            if key in seen:
                continue
            seen.add(key)
            d["file"] = d["file"].replace(str(project_dir) + "/", "")
            d["severity"] = "error" if d.pop("sev") in ("error", "fatal error") else ("warning" if "warning" in line else "note")
            diags.append(d)
        elif m := _LD_ERR.search(line):
            diags.append({"file": None, "line": None, "col": None, "severity": "error", "msg": m.group(1)})
    memory = {m["region"]: {"used": m["used"], "size": m["size"], "pct": float(m["pct"])}
              for line in output.splitlines() if (m := _MEM.match(line))}
    return {
        "errors": [d for d in diags if d["severity"] == "error"],
        "warnings": [d for d in diags if d["severity"] == "warning"],
        "memory": memory,
    }


def build(cfg: dict, clean: bool = False, reconfigure: bool = False, jobs: int = 0,
          max_diags: int = 30) -> dict:
    P = paths(cfg)
    j = [f"-j{jobs}"] if jobs else []
    steps = []
    t0 = time.monotonic()
    if P.build_system == "cmake":
        build_dir = P.elf.parent
        if reconfigure or not (build_dir / "CMakeCache.txt").exists():
            steps.append(["cmake", "--preset", P.cmake_preset, *(["--fresh"] if reconfigure else [])])
        cmd = ["cmake", "--build", "--preset", P.cmake_preset, *j]
        if clean:
            cmd.append("--clean-first")
        steps.append(cmd)
    elif P.build_system == "make":
        if clean:
            steps.append(["make", "-C", str(P.project_dir), "clean"])
        steps.append(["make", "-C", str(P.project_dir), *(j or ["-j"])])
    else:
        raise ToolError(f"unknown build_system: {P.build_system}")

    output = ""
    rc = 0
    for cmd in steps:
        cp = run(cmd, cwd=P.project_dir, timeout=900)
        output += f"$ {' '.join(cmd)}\n{cp.stdout}\n{cp.stderr}\n"
        rc = cp.returncode
        if rc:
            break
    log = out_file(cfg, "build", "log")
    log.write_text(output)

    parsed = _parse(output, P.project_dir)
    ok = rc == 0 and P.elf.exists()
    result = {
        "ok": ok,
        "seconds": round(time.monotonic() - t0, 1),
        "build_system": P.build_system,
        "elf": rel(P.elf) if ok else None,
        "error_count": len(parsed["errors"]),
        "warning_count": len(parsed["warnings"]),
        "errors": parsed["errors"][:max_diags],
        "warnings": parsed["warnings"][:max_diags],
        "memory": parsed["memory"],
        "log": rel(log),
    }
    if ok:
        result["elf_sha1"] = hashlib.sha1(P.elf.read_bytes()).hexdigest()
        result["elf_mtime"] = P.elf.stat().st_mtime
    elif not parsed["errors"]:
        result["output_tail"] = tail(output, 40)
    return result
