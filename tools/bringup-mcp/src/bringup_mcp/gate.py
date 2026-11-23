"""The bench interlock: one decision point every actuate call passes through.

State lives in a JSON file on the bench host, written only by `bringup-mcp bench ...` and
`bringup-mcp allow ...` run locally. The server only reads it. Three states:

- unset: the file is missing or unreadable. Every actuate call is refused. Fail closed.
- absent: no power stage is attached (bare Nucleo). Actuate calls run.
- attached: the power stage is attached. Actuate calls run, and the state is there so the
  agent knows the power stage is live (SKILL.md's safety rules are its to follow). If the bench
  was set with `--tokens`, an actuate call that reaches the board additionally needs a token
  that was issued on the bench for that exact command, has not expired, and has uses left
  (one-shot unless issued with `--uses N`). Tokens were the default until 2026-09-27 16:43;
  Jonah and Dan asked for testing to run without them (DECISIONS.md). An actuate call that reaches only a bench instrument (scope, analyzer,
  serial port) runs without one: it cannot drive the power stage, and a scope session is a
  dozen such calls.

A refusal is not a failure: it is returned as its own outcome with the reason and what to do.
"""

from __future__ import annotations

import datetime as dt
import json
import secrets
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal

PowerStage = Literal["attached", "absent", "unset"]

_WORDS = ("amber", "birch", "cedar", "delta", "ember", "flint", "grove", "heron", "iris", "jade",
          "kelp", "lumen", "maple", "north", "onyx", "pearl", "quill", "ridge", "slate", "tidal",
          "umber", "vale", "wren", "xenon", "yarrow", "zinc")


@dataclass(frozen=True)
class BenchState:
    power_stage: PowerStage
    set_at: str | None
    set_by: str | None
    note: str | None
    tokens: tuple[dict[str, Any], ...]
    file: Path
    read_error: str | None = None
    tokens_required: bool = False

    def public(self) -> dict[str, Any]:
        d: dict[str, Any] = {"power_stage": self.power_stage, "state_file": str(self.file)}
        if self.power_stage == "attached":
            d["tokens_required"] = self.tokens_required
        if self.set_at:
            d["set_at"] = self.set_at
        if self.set_by:
            d["set_by"] = self.set_by
        if self.note:
            d["note"] = self.note
        if self.read_error:
            d["read_error"] = self.read_error
        live = [t for t in self.tokens if not t.get("used_at") and not _expired(t)]
        d["pending_tokens"] = [{"command": t["command"], "expires_at": t["expires_at"],
                                "uses_left": _uses_left(t)} for t in live]
        return d


@dataclass(frozen=True)
class Decision:
    allowed: bool
    reason: str
    how_to_proceed: str | None = None
    token_used: str | None = None


def _now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def _iso(t: dt.datetime) -> str:
    return t.replace(microsecond=0).isoformat()


def _uses_left(tok: dict[str, Any]) -> int:
    """Tokens issued before 0.1.2 carry no uses_left; they are one-shot."""
    try:
        return int(tok.get("uses_left", 1))
    except (TypeError, ValueError):
        return 0


def _expired(tok: dict[str, Any]) -> bool:
    try:
        return dt.datetime.fromisoformat(tok["expires_at"]) <= _now()
    except (KeyError, ValueError):
        return True


def read_state(file: Path) -> BenchState:
    if not file.exists():
        return BenchState("unset", None, None, None, (), file,
                          read_error=f"state file missing: {file}")
    try:
        d = json.loads(file.read_text())
        ps = d.get("power_stage")
        if ps not in ("attached", "absent"):
            return BenchState("unset", None, None, None, (), file,
                              read_error=f"power_stage in {file} is {ps!r}, expected attached or absent")
        return BenchState(ps, d.get("set_at"), d.get("set_by"), d.get("note"),
                          tuple(d.get("tokens", [])), file,
                          tokens_required=bool(d.get("tokens_required", False)))
    except (OSError, json.JSONDecodeError) as e:
        return BenchState("unset", None, None, None, (), file, read_error=f"cannot read {file}: {e}")


def write_state(file: Path, power_stage: Literal["attached", "absent"], set_by: str,
                note: str | None, keep_tokens: bool, tokens_required: bool = False) -> BenchState:
    prev = read_state(file)
    d = {
        "power_stage": power_stage,
        "set_at": _iso(_now()),
        "set_by": set_by,
        "note": note,
        "tokens_required": bool(tokens_required) if power_stage == "attached" else False,
        "tokens": list(prev.tokens) if keep_tokens else [],
    }
    file.parent.mkdir(parents=True, exist_ok=True)
    tmp = file.with_suffix(file.suffix + ".tmp")
    tmp.write_text(json.dumps(d, indent=1))
    tmp.replace(file)
    return read_state(file)


def issue_token(file: Path, command: str, minutes: float, issued_by: str, uses: int = 1) -> dict[str, Any]:
    st = read_state(file)
    if st.power_stage == "unset":
        raise RuntimeError(f"bench state is unset ({st.read_error}); set it with `bringup-mcp bench` first")
    if uses < 1:
        raise RuntimeError("uses must be at least 1")
    tok = {
        "token": f"{secrets.choice(_WORDS)}-{secrets.choice(_WORDS)}-{secrets.randbelow(900) + 100}",
        "command": command,
        "issued_at": _iso(_now()),
        "issued_by": issued_by,
        "expires_at": _iso(_now() + dt.timedelta(minutes=minutes)),
        "uses_left": uses,
        "used_at": None,
    }
    d = json.loads(file.read_text())
    tokens = [t for t in d.get("tokens", []) if not t.get("used_at") and not _expired(t)]
    tokens.append(tok)
    d["tokens"] = tokens
    tmp = file.with_suffix(file.suffix + ".tmp")
    tmp.write_text(json.dumps(d, indent=1))
    tmp.replace(file)
    return tok


def decide(file: Path, effect: str, reach: str, command: str, token: str | None) -> tuple[Decision, BenchState]:
    """The one gate. Read and write effects pass; actuate depends on the bench state, on whether
    the bench was set with `--tokens`, and on whether the command reaches the board (`dut`) or
    only an instrument (`instrument`)."""
    st = read_state(file)
    if effect != "actuate":
        return Decision(True, f"{effect} effect needs no interlock"), st
    if st.power_stage == "unset":
        return Decision(False, "bench_state_unset",
                        "On the bench host run `bringup-mcp bench absent` (bare Nucleo) or "
                        "`bringup-mcp bench attached` (power stage connected). The server fails closed "
                        f"until then. Detail: {st.read_error}"), st
    if st.power_stage == "absent":
        return Decision(True, "power stage absent; actuate runs without a token"), st
    # attached
    if reach == "instrument":
        return Decision(True, "power stage attached; instrument-only actuate runs without a token"), st
    if not st.tokens_required:
        return Decision(True, "power stage attached; bench set without --tokens, actuate runs"), st
    if not token:
        return Decision(False, "power_stage_attached_token_required",
                        f"Ask Dan to run `bringup-mcp allow \"{command}\"` on the bench and pass the token "
                        f"he gives you as confirmation_token. Tokens are one-shot and expire."), st
    match = None
    for t in st.tokens:
        if t.get("token") == token:
            match = t
            break
    if match is None:
        return Decision(False, "token_unknown", "That token was never issued (or the state file was reset). Ask for a new one."), st
    if match.get("command") != command:
        return Decision(False, "token_for_other_command",
                        f"That token was issued for `{match.get('command')}`, not `{command}`. Ask for one for this command."), st
    if match.get("used_at") or _uses_left(match) < 1:
        return Decision(False, "token_already_used", f"Used up at {match.get('used_at')}. Ask for a new one."), st
    if _expired(match):
        return Decision(False, "token_expired", f"Expired at {match.get('expires_at')}. Ask for a new one."), st
    left = _uses_left(match)
    return Decision(True, f"power stage attached; valid token, {left} use(s) left before this call", token_used=token), st


def consume_token(file: Path, token: str) -> bool:
    """Spend one use; the token is marked used when none are left. Returns False if the file
    changed underneath and the token is gone."""
    try:
        d = json.loads(file.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    for t in d.get("tokens", []):
        if t.get("token") == token and not t.get("used_at") and _uses_left(t) >= 1:
            t["uses_left"] = _uses_left(t) - 1
            t["last_used_at"] = _iso(_now())
            if t["uses_left"] == 0:
                t["used_at"] = t["last_used_at"]
            tmp = file.with_suffix(file.suffix + ".tmp")
            tmp.write_text(json.dumps(d, indent=1))
            tmp.replace(file)
            return True
    return False
