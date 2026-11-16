"""Configuration loading, path resolution and shared helpers."""

from __future__ import annotations

import datetime as _dt
import glob
import os
import shutil
import subprocess
import tomllib
from dataclasses import dataclass
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parents[2]  # tools/bringup
REPO_ROOT = Path(os.environ.get("BRINGUP_REPO", TOOL_DIR.parents[1])).resolve()


class ToolError(Exception):
    """An expected failure that should be reported to the caller as JSON, not a traceback."""

    def __init__(self, message: str, **details):
        super().__init__(message)
        self.details = details


def _deep_merge(base: dict, override: dict) -> dict:
    out = dict(base)
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def load_config() -> dict:
    cfg: dict = {}
    for name in ("bringup.toml", "bringup.local.toml"):
        p = TOOL_DIR / name
        if p.exists():
            cfg = _deep_merge(cfg, tomllib.loads(p.read_text()))
    return cfg


def repo_path(p: str | Path) -> Path:
    p = Path(p).expanduser()
    return p if p.is_absolute() else REPO_ROOT / p


@dataclass
class Paths:
    project_dir: Path
    ioc: Path
    build_system: str
    cmake_preset: str
    elf: Path
    out_dir: Path


def paths(cfg: dict) -> Paths:
    pc = cfg["project"]
    project_dir = repo_path(pc["dir"])
    build_system = pc.get("build_system", "cmake")
    elf = project_dir / (pc["elf_cmake"] if build_system == "cmake" else pc["elf_make"])
    return Paths(
        project_dir=project_dir,
        ioc=project_dir / pc["ioc"],
        build_system=build_system,
        cmake_preset=pc.get("cmake_preset", "Debug"),
        elf=elf,
        out_dir=repo_path(cfg.get("output", {}).get("dir", "bringup_out")),
    )


def out_file(cfg: dict, stem: str, ext: str) -> Path:
    """Timestamped path in the output dir, e.g. bringup_out/20260926-214501_scope_ch1.csv."""
    d = paths(cfg).out_dir
    d.mkdir(parents=True, exist_ok=True)
    ts = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return d / f"{ts}_{stem}.{ext.lstrip('.')}"


def find_exe(configured: str, names: list[str], globs: list[str]) -> str | None:
    if configured:
        p = shutil.which(configured) or (configured if Path(configured).exists() else None)
        return p
    for n in names:
        if p := shutil.which(n):
            return p
    for g in globs:
        hits = sorted(glob.glob(os.path.expanduser(g)), reverse=True)  # newest version dir first
        if hits:
            return hits[0]
    return None


def cubemx_exe(cfg: dict) -> str | None:
    return find_exe(
        cfg.get("cubemx", {}).get("path", ""),
        ["STM32CubeMX", "stm32cubemx"],
        [
            "/usr/local/STMicroelectronics/STM32Cube/STM32CubeMX/STM32CubeMX",
            "~/STM32CubeMX/STM32CubeMX",
            "/opt/st/STM32CubeMX/STM32CubeMX",
        ],
    )


def programmer_exe(cfg: dict) -> str | None:
    return find_exe(
        cfg.get("probe", {}).get("programmer", ""),
        ["STM32_Programmer_CLI"],
        [
            "~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
            "/usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI",
            "/opt/st/stm32cubeide_*/plugins/*cubeprogrammer*/tools/bin/STM32_Programmer_CLI",
        ],
    )


def gdb_exe() -> str | None:
    return find_exe(
        "",
        ["arm-none-eabi-gdb", "gdb-multiarch"],
        ["/opt/st/stm32cubeide_*/plugins/*gnu-tools-for-stm32*/tools/bin/arm-none-eabi-gdb"],
    )


def run(cmd: list[str], cwd: Path | None = None, timeout: float | None = None,
        input: str | None = None) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            cmd, cwd=cwd, timeout=timeout, input=input,
            capture_output=True, text=True, errors="replace",
        )
    except FileNotFoundError as e:
        raise ToolError(f"executable not found: {cmd[0]}") from e
    except subprocess.TimeoutExpired as e:
        raise ToolError(f"timed out after {timeout}s: {' '.join(cmd)}",
                        stdout_tail=tail(e.stdout or ""), stderr_tail=tail(e.stderr or "")) from e


def tail(text: str | bytes, n: int = 40) -> str:
    if isinstance(text, bytes):
        text = text.decode(errors="replace")
    return "\n".join(text.splitlines()[-n:])


def rel(p: Path) -> str:
    try:
        return str(p.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(p)
