"""Configuration: one TOML file, read once, validated, failing closed.

Nothing here has a silent default that decides what the agent may do. The repo root and
the `bu` path must be present; the bench state lives in its own file (see gate.py) so that
setting it is a deliberate act on the bench host, never a config default.
"""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_CONFIG_NAME = "bringup-mcp.toml"
ENV_CONFIG = "BRINGUP_MCP_CONFIG"


class ConfigError(Exception):
    """The configuration cannot be used. The server refuses to start (or a tool refuses to run)."""


@dataclass(frozen=True)
class ShellConfig:
    enabled: bool
    allow: tuple[str, ...]
    deny_git_push_to: tuple[str, ...]
    timeout_s: float
    max_output_bytes: int


@dataclass(frozen=True)
class HttpConfig:
    host: str
    port: int
    allowed_hosts: tuple[str, ...]


DEFAULT_INSTRUMENT_PREFIXES = ("scope", "la", "serial")


@dataclass(frozen=True)
class Config:
    path: Path
    repo_root: Path
    bu: Path
    state_file: Path
    http: HttpConfig
    shell: ShellConfig
    # First words of bu commands that reach an instrument (scope, analyzer, serial port) and
    # not the device under test. Their actuate calls change instrument state only, so while the
    # power stage is attached they run without a token; unset still refuses them.
    instrument_prefixes: tuple[str, ...] = DEFAULT_INSTRUMENT_PREFIXES
    timeouts: dict[str, float] = field(default_factory=dict)
    default_timeout_s: float = 30.0
    max_result_bytes: int = 65536
    max_field_bytes: int = 8192

    def timeout_for(self, command: str) -> float:
        return float(self.timeouts.get(command, self.default_timeout_s))


def find_config_path(explicit: str | None = None) -> Path:
    if explicit:
        return Path(explicit).expanduser().resolve()
    if env := os.environ.get(ENV_CONFIG):
        return Path(env).expanduser().resolve()
    # The package's project directory (…/bringup-mcp), then the current directory.
    here = Path(__file__).resolve().parents[2] / DEFAULT_CONFIG_NAME
    if here.exists():
        return here
    return (Path.cwd() / DEFAULT_CONFIG_NAME).resolve()


def load_config(explicit: str | None = None) -> Config:
    path = find_config_path(explicit)
    if not path.exists():
        raise ConfigError(f"config file not found: {path} (set {ENV_CONFIG} or pass --config)")
    try:
        raw = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(f"config is not valid TOML: {path}: {e}") from e

    repo = raw.get("repo")
    if not isinstance(repo, dict) or "root" not in repo:
        raise ConfigError(f"config needs [repo] root = \"/path/to/PD_Charger\": {path}")
    repo_root = Path(repo["root"]).expanduser().resolve()
    if not repo_root.is_dir():
        raise ConfigError(f"[repo] root is not a directory: {repo_root}")
    bu = _under(repo_root, repo.get("bu", "tools/bringup/bu"))
    if not bu.is_file():
        raise ConfigError(f"bu launcher not found at {bu} (config [repo] bu)")
    if not os.access(bu, os.X_OK):
        raise ConfigError(f"bu launcher is not executable: {bu}")

    bench = raw.get("bench", {})
    state_file = _under(repo_root, bench.get("state_file", "bringup_out/bench-state.json"))
    instrument_prefixes = tuple(str(p) for p in bench.get("instrument_prefixes", DEFAULT_INSTRUMENT_PREFIXES))

    http = raw.get("http", {})
    port = int(http.get("port", 40050))
    host = str(http.get("host", "127.0.0.1"))
    allowed_hosts = tuple(str(h) for h in http.get("allowed_hosts", [f"127.0.0.1:{port}", f"localhost:{port}"]))

    sh = raw.get("shell", {})
    shell = ShellConfig(
        enabled=bool(sh.get("enabled", False)),
        allow=tuple(str(a) for a in sh.get("allow", [])),
        deny_git_push_to=tuple(str(b) for b in sh.get("deny_git_push_to", ["main", "master"])),
        timeout_s=float(sh.get("timeout_s", 120)),
        max_output_bytes=int(sh.get("max_output_bytes", 65536)),
    )
    if shell.enabled and not shell.allow:
        raise ConfigError("[shell] enabled = true but allow = [] : an empty allowlist is a misconfiguration, not a narrower grant")

    timeouts_raw = raw.get("timeouts", {})
    default_timeout = float(timeouts_raw.get("default_s", 30))
    timeouts = {k: float(v) for k, v in timeouts_raw.items() if k != "default_s"}

    limits = raw.get("limits", {})
    return Config(
        path=path,
        repo_root=repo_root,
        bu=bu,
        state_file=state_file,
        http=HttpConfig(host=host, port=port, allowed_hosts=allowed_hosts),
        shell=shell,
        instrument_prefixes=instrument_prefixes,
        timeouts=timeouts,
        default_timeout_s=default_timeout,
        max_result_bytes=int(limits.get("max_result_bytes", 65536)),
        max_field_bytes=int(limits.get("max_field_bytes", 8192)),
    )


def _under(root: Path, p: str) -> Path:
    q = Path(p).expanduser()
    return q.resolve() if q.is_absolute() else (root / q).resolve()
