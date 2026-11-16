"""The MCP server: tools generated from `bu schema`, one worker, one gate, typed outcomes."""

from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import dataclass
from typing import Any

from mcp.server import Server, ServerRequestContext
from mcp.types import (CallToolRequestParams, CallToolResult, ListToolsResult, PaginatedRequestParams,
                       TextContent, Tool)

from . import __version__, extras, gate, schema
from .config import Config
from .runner import Outcome, Worker

INSTRUCTIONS = """bringup-mcp fronts `tools/bringup/bu` on the bench host for the PD_Charger regulator.

Read this first:
1. Call bench_state before any actuate tool. If power_stage is unset, every actuate call is refused until Dan sets it on the bench. If it is attached, each actuate call that reaches the board (flash, mem write, regulator *, probe reset) needs a one-shot confirmation_token that Dan issues on the bench for that exact command; actuate calls that reach only an instrument (scope *, la *, serial *) run without one.
2. One call runs at a time (one ST-LINK, one scope). `_call.queued_s` says how long yours waited.
3. Results carry bu's JSON unchanged under `result`. Large artifacts (CSV, PNG, logs, patches) are files in bringup_out/; read them through the filesystem server, and use `bu_scope_analyze` rather than reading a raw CSV.
4. `verified: false` on a register write is not a failure: the readback cannot pass on write-only registers. Confirm by effect (regulator status, scope).
5. Errors have a type: bu_error, refused, timed_out, not_configured, invalid_arguments, bad_output, internal. A refusal names how to proceed.
The safety rules in .claude/skills/hw-bringup/SKILL.md apply to you; read them before the first actuate call."""


@dataclass
class State:
    cfg: Config
    catalogue: schema.Catalogue
    worker: Worker
    tools: list[Tool]


def load_catalogue(cfg: Config) -> schema.Catalogue:
    """Run `bu schema` once, synchronously, at startup. A failure here stops the server."""
    try:
        cp = subprocess.run([str(cfg.bu), "schema"], cwd=cfg.repo_root, capture_output=True, text=True,
                            timeout=cfg.timeout_for("schema") if "schema" in cfg.timeouts else 120)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise schema.SchemaError(f"could not run `bu schema`: {e}") from e
    if cp.returncode != 0 and not cp.stdout.strip():
        raise schema.SchemaError(f"`bu schema` exited {cp.returncode}: {cp.stderr[-800:]}")
    return schema.parse_schema(cp.stdout, hashlib.sha256(cp.stdout.encode()).hexdigest(), cfg.instrument_prefixes)


def build_state(cfg: Config) -> State:
    cat = load_catalogue(cfg)
    tools = [schema.tool_for(c) for c in cat.commands] + extras.extra_tools(cfg)
    return State(cfg=cfg, catalogue=cat, worker=Worker(cfg), tools=tools)


def _cap(obj: Any, cfg: Config) -> tuple[Any, bool]:
    """Bound the payload: long strings are replaced by a note with their size. Never silently."""
    truncated = False

    def walk(x: Any) -> Any:
        nonlocal truncated
        if isinstance(x, str) and len(x.encode()) > cfg.max_field_bytes:
            truncated = True
            return f"<{len(x.encode())} bytes omitted; first 512: {x[:512]!r}>"
        if isinstance(x, dict):
            return {k: walk(v) for k, v in x.items()}
        if isinstance(x, list):
            if len(x) > 500:
                truncated = True
                return [walk(v) for v in x[:500]] + [f"<{len(x) - 500} more items omitted>"]
            return [walk(v) for v in x]
        return x

    text = json.dumps(obj, default=str)
    if len(text.encode()) <= cfg.max_result_bytes:
        return obj, False
    capped = walk(obj)
    return capped, truncated or True


def to_result(out: Outcome, cfg: Config) -> CallToolResult:
    payload = out.payload()
    payload, truncated = _cap(payload, cfg)
    if truncated:
        payload["_truncated"] = True
    text = json.dumps(payload, indent=1, default=str)
    return CallToolResult(content=[TextContent(type="text", text=text)], structuredContent=payload, isError=not out.ok)


async def dispatch(st: State, name: str, arguments: dict[str, Any] | None) -> Outcome:
    cfg = st.cfg
    args = dict(arguments or {})
    if name == "bench_state":
        return await extras.bench_state(cfg)
    if name == "server_version":
        return await extras.server_version(cfg, st.worker, st.catalogue.raw_sha256)
    if name == "probe_pc_sample":
        n = args.get("samples")
        if not isinstance(n, int) or not 1 <= n <= 20:
            return Outcome(False, "invalid_arguments", "samples must be an integer from 1 to 20")
        return await extras.probe_pc_sample(cfg, st.worker, n)
    if name == "bu_test":
        return await extras.bu_test(cfg, st.worker)
    if name == "shell_run":
        return await extras.shell_run(cfg, st.worker, args.get("argv"), args.get("cwd"), args.get("stdin"), args.get("timeout_s"))
    cmd = st.catalogue.by_tool.get(name)
    if cmd is None:
        return Outcome(False, "invalid_arguments", f"unknown tool {name!r}")
    try:
        argv = schema.build_argv(cmd, args)
    except schema.ArgumentError as e:
        return Outcome(False, "invalid_arguments", str(e))
    token = args.get(schema.TOKEN_ARG)
    decision, bench = gate.decide(cfg.state_file, cmd.effect, cmd.reach, cmd.command, token if isinstance(token, str) else None)
    bench_info = {"power_stage": bench.power_stage}
    if not decision.allowed:
        return Outcome(False, "refused", decision.reason,
                       details={"how_to_proceed": decision.how_to_proceed, "bench": bench.public()},
                       call={"command": cmd.command, "argv": argv, "effect": cmd.effect})
    if decision.token_used:
        if not gate.consume_token(cfg.state_file, decision.token_used):
            return Outcome(False, "refused", "token_vanished",
                           details={"how_to_proceed": "The state file changed between check and use. Ask for a new token."},
                           call={"command": cmd.command, "argv": argv, "effect": cmd.effect})
        bench_info["token_consumed"] = True
    out = await st.worker.run_bu(argv, cmd.command)
    out.call = {**out.call, "effect": cmd.effect, "bench": bench_info}
    return out


def make_server(st: State) -> Server:
    async def on_list_tools(ctx: ServerRequestContext, params: PaginatedRequestParams | None) -> ListToolsResult:
        return ListToolsResult(tools=st.tools)

    async def on_call_tool(ctx: ServerRequestContext, params: CallToolRequestParams) -> CallToolResult:
        try:
            out = await dispatch(st, params.name, params.arguments)
        except Exception as e:  # noqa: BLE001 - a bug here must still reach the caller typed
            import traceback
            out = Outcome(False, "internal", f"{type(e).__name__}: {e}", details={"traceback": traceback.format_exc(limit=6)})
        return to_result(out, st.cfg)

    return Server("bringup-mcp", version=__version__, instructions=INSTRUCTIONS,
                  on_list_tools=on_list_tools, on_call_tool=on_call_tool)
