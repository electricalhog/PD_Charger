"""Turn `bu schema` into MCP tool definitions, and tool arguments back into a `bu` argv.

The catalogue is read from the running CLI at startup, so the tools match the code on the
bench and never a copy of it. Two deliberate removals:

- any argument named `force` is dropped from every tool: `regulator clear-fault --force` and
  `cubemx generate --force` are Dan's to run, by hand, on the bench;
- the `schema` command itself is not exposed (the server already consumed it).
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any

from mcp.types import Tool, ToolAnnotations

EFFECTS = ("read", "write", "actuate")
REACHES = ("dut", "instrument")  # what an actuate command changes: the board, or a bench instrument
TOKEN_ARG = "confirmation_token"
DROPPED_ARGS = frozenset({"force"})
HIDDEN_COMMANDS = frozenset({"schema"})


class SchemaError(Exception):
    """`bu schema` did not give a usable catalogue."""


@dataclass(frozen=True)
class Arg:
    name: str
    flags: tuple[str, ...]
    positional: bool
    required: bool
    kind: str  # flag | value | list
    type: str  # str | int | float
    choices: tuple[Any, ...] | None
    default: Any
    help: str | None


@dataclass(frozen=True)
class Command:
    command: str  # "scope capture"
    argv_prefix: tuple[str, ...]
    effect: str
    reach: str  # dut | instrument (see REACHES); meaningful for actuate
    help: str | None
    args: tuple[Arg, ...]

    @property
    def tool_name(self) -> str:
        return "bu_" + re.sub(r"[^a-z0-9]+", "_", self.command.lower()).strip("_")

    @property
    def needs_token_when_attached(self) -> bool:
        return self.effect == "actuate" and self.reach == "dut"


@dataclass
class Catalogue:
    commands: list[Command]
    invoke: str
    raw_sha256: str
    by_tool: dict[str, Command] = field(init=False)

    def __post_init__(self) -> None:
        self.by_tool = {c.tool_name: c for c in self.commands}
        if len(self.by_tool) != len(self.commands):
            raise SchemaError("two bu commands map to the same tool name")


def parse_schema(raw_json: str, sha256: str, instrument_prefixes: tuple[str, ...] = ()) -> Catalogue:
    """`instrument_prefixes`: first words of commands that reach a bench instrument, not the board."""
    try:
        d = json.loads(raw_json)
    except json.JSONDecodeError as e:
        raise SchemaError(f"bu schema is not JSON: {e}") from e
    if not isinstance(d, dict) or not d.get("ok") or not isinstance(d.get("commands"), list):
        raise SchemaError(f"bu schema answered without ok/commands: {str(d)[:200]}")
    cmds: list[Command] = []
    for c in d["commands"]:
        name = c.get("command")
        if not name or name in HIDDEN_COMMANDS:
            continue
        effect = c.get("effect")
        if effect not in EFFECTS:
            raise SchemaError(f"command {name!r} has unknown effect {effect!r}")
        args: list[Arg] = []
        for a in c.get("args", []):
            if a.get("name") in DROPPED_ARGS:
                continue
            args.append(Arg(
                name=a["name"],
                flags=tuple(a.get("flags") or ()),
                positional=bool(a.get("positional")),
                required=bool(a.get("required")),
                kind=a.get("kind", "value"),
                type=a.get("type") or "str",
                choices=tuple(a["choices"]) if a.get("choices") else None,
                default=a.get("default"),
                help=a.get("help"),
            ))
        cmds.append(Command(
            command=name,
            argv_prefix=tuple(c.get("argv_prefix") or name.split()),
            effect=effect,
            reach="instrument" if name.split()[0] in instrument_prefixes else "dut",
            help=c.get("help"),
            args=tuple(args),
        ))
    if not cmds:
        raise SchemaError("bu schema listed no commands")
    return Catalogue(commands=cmds, invoke=str(d.get("invoke", "")), raw_sha256=sha256)


# ----------------------------------------------------------------- JSON schema

_JSON_TYPE = {"str": "string", "int": "integer", "float": "number"}


def _prop(a: Arg) -> dict[str, Any]:
    if a.kind == "flag":
        p: dict[str, Any] = {"type": "boolean"}
    elif a.kind == "list":
        p = {"type": "array", "items": {"type": _JSON_TYPE.get(a.type, "string")}, "minItems": 1}
    else:
        p = {"type": _JSON_TYPE.get(a.type, "string")}
    if a.choices:
        (p["items"] if a.kind == "list" else p)["enum"] = list(a.choices)
    desc = a.help or ""
    if a.default not in (None, False, ""):
        desc = (desc + " " if desc else "") + f"(bu default: {a.default})"
    if a.positional:
        desc = (desc + " " if desc else "") + "[positional]"
    if desc:
        p["description"] = desc.strip()
    return p


def input_schema(cmd: Command, token_arg: bool) -> dict[str, Any]:
    props: dict[str, Any] = {}
    required: list[str] = []
    for a in cmd.args:
        props[a.name] = _prop(a)
        if a.required:
            required.append(a.name)
    if token_arg:
        props[TOKEN_ARG] = {
            "type": "string",
            "description": ("One-shot confirmation token issued on the bench with `bringup-mcp allow "
                            f"\"{cmd.command}\"`. Required while the power stage is attached; ignored "
                            "while it is absent."),
        }
    schema: dict[str, Any] = {"type": "object", "properties": props, "additionalProperties": False}
    if required:
        schema["required"] = required
    return schema


def annotations(cmd: Command) -> ToolAnnotations:
    if cmd.effect == "read":
        return ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    if cmd.effect == "write":
        return ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False)
    return ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False)


_EFFECT_TEXT = {
    "read": "Effect: read. No file or hardware state changes.",
    "write": "Effect: write. Changes files in the repo checkout; reversible with git.",
    ("actuate", "dut"): ("Effect: actuate on the board (flash, memory, regulator, probe). Gated by the "
                         "bench interlock: refused until the bench state is set; while the power stage "
                         "is attached it needs a confirmation token issued on the bench (see bench_state)."),
    ("actuate", "instrument"): ("Effect: actuate on a bench instrument only (scope, analyzer, serial); "
                                "it cannot drive the board. Refused until the bench state is set; runs "
                                "without a token in either bench state."),
}


def tool_for(cmd: Command) -> Tool:
    desc = (cmd.help or f"bu {cmd.command}").strip()
    effect_text = _EFFECT_TEXT[(cmd.effect, cmd.reach)] if cmd.effect == "actuate" else _EFFECT_TEXT[cmd.effect]
    body = (f"{desc}\n\nRuns `bu {cmd.command}` on the bench host and returns its JSON object "
            f"unchanged under `result`, plus `_call` (argv, seconds, queue wait, bench state). "
            f"`result.ok` false is returned as an error of type bu_error. Artifact paths in the "
            f"result are relative to the repo root; read them through the filesystem server. "
            f"{effect_text}")
    return Tool(
        name=cmd.tool_name,
        title=f"bu {cmd.command}",
        description=body,
        inputSchema=input_schema(cmd, token_arg=cmd.needs_token_when_attached),
        annotations=annotations(cmd),
    )


# ----------------------------------------------------------------- argv building

class ArgumentError(ValueError):
    pass


def build_argv(cmd: Command, arguments: dict[str, Any] | None) -> list[str]:
    """Map validated tool arguments to a bu argv. Unknown keys are refused, not dropped."""
    args = dict(arguments or {})
    args.pop(TOKEN_ARG, None)
    known = {a.name for a in cmd.args}
    unknown = set(args) - known
    if unknown:
        raise ArgumentError(f"unknown argument(s) for bu {cmd.command}: {sorted(unknown)}")
    argv: list[str] = list(cmd.argv_prefix)
    options: list[str] = []
    for a in cmd.args:
        if a.name not in args or args[a.name] is None:
            if a.required:
                raise ArgumentError(f"missing required argument {a.name!r} for bu {cmd.command}")
            continue
        v = args[a.name]
        if a.kind == "flag":
            if not isinstance(v, bool):
                raise ArgumentError(f"{a.name} must be a boolean")
            if v:
                options.append(a.flags[0])
            continue
        if a.kind == "list":
            if not isinstance(v, list) or not v:
                raise ArgumentError(f"{a.name} must be a non-empty array")
            items = [_scalar(a, x) for x in v]
            if a.positional:
                argv.extend(items)
            else:
                for it in items:  # repeatable option (argparse append)
                    options.extend([a.flags[0], it])
            continue
        s = _scalar(a, v)
        if a.positional:
            argv.append(s)
        else:
            options.extend([a.flags[0], s])
    return argv + options


def _scalar(a: Arg, v: Any) -> str:
    if isinstance(v, bool):
        raise ArgumentError(f"{a.name} must be a {a.type}, not a boolean")
    if a.type == "int" and not isinstance(v, int):
        raise ArgumentError(f"{a.name} must be an integer")
    if a.type == "float" and not isinstance(v, (int, float)):
        raise ArgumentError(f"{a.name} must be a number")
    if a.type == "str" and not isinstance(v, str):
        raise ArgumentError(f"{a.name} must be a string")
    if a.choices and v not in a.choices:
        raise ArgumentError(f"{a.name} must be one of {list(a.choices)}")
    s = str(v)
    if "\x00" in s:
        raise ArgumentError(f"{a.name} contains a NUL byte")
    return s
