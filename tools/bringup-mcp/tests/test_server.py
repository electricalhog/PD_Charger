"""Offline tests through the real MCP server object and the SDK's in-memory client.

They exercise the mechanism (schema to tools, argv building, the gate, timeouts, serialization,
the shell allowlist, output capping) against a fake `bu`. They cannot prove anything about the
real CLI or the bench; that is the integration checklist in README.md.
"""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path

import pytest
from mcp.client import Client

from bringup_mcp import gate
from bringup_mcp.config import load_config
from bringup_mcp.server import build_state, make_server
from tests.conftest import write_config

pytestmark = pytest.mark.anyio


@pytest.fixture
def anyio_backend():
    return "asyncio"


def _server(config_path: Path):
    cfg = load_config(str(config_path))
    st = build_state(cfg)
    return cfg, st, make_server(st)


def _payload(res) -> dict:
    assert res.content and res.content[0].type == "text"
    return json.loads(res.content[0].text)


async def test_tools_generated_from_schema(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        tools = {t.name: t for t in (await c.list_tools()).tools}
    assert "bu_schema" not in tools, "the schema command itself is not exposed"
    assert {"bu_doctor", "bu_ioc_get", "bu_ioc_set", "bu_flash", "bu_regulator_clear_fault",
            "bu_scope_capture", "bench_state", "server_version", "probe_pc_sample", "bu_test"} <= set(tools)
    assert "shell_run" not in tools, "shell is off unless configured"
    # effect -> annotations
    assert tools["bu_doctor"].annotations.read_only_hint is True
    assert tools["bu_ioc_set"].annotations.destructive_hint is False and tools["bu_ioc_set"].annotations.read_only_hint is False
    assert tools["bu_flash"].annotations.destructive_hint is True
    # --force never reachable; token arg only on actuate tools
    cf = tools["bu_regulator_clear_fault"].input_schema["properties"]
    assert "force" not in cf and "confirmation_token" in cf
    assert "confirmation_token" not in tools["bu_doctor"].input_schema["properties"]
    # required positional list, enum choices, additionalProperties closed
    s = tools["bu_scope_capture"].input_schema
    assert s["required"] == ["sources"] and s["additionalProperties"] is False
    # instrument-only actuate tools carry no token argument and say so
    assert "confirmation_token" not in s["properties"]
    assert "instrument only" in tools["bu_scope_capture"].description
    assert "confirmation_token" not in tools["bu_scope_trigger"].input_schema["properties"]
    assert "confirmation_token" in tools["bu_flash"].input_schema["properties"]
    assert tools["bu_scope_trigger"].input_schema["properties"]["slope"]["enum"] == ["POS", "NEG"]


async def test_argv_building_and_passthrough(config_path):
    cfg, st, server = _server(config_path)
    gate.write_state(cfg.state_file, "absent", "test", None, keep_tokens=False)
    async with Client(server) as c:
        r = _payload(await c.call_tool("bu_doctor", {"offline": True}))
        assert r["ok"] and r["result"]["argv"] == ["doctor", "--offline"]
        r = _payload(await c.call_tool("bu_ioc_get", {"keys": ["A", "B.C"]}))
        assert r["result"]["argv"] == ["ioc", "get", "A", "B.C"]
        r = _payload(await c.call_tool("bu_scope_capture", {"sources": ["CH2", "CH3"], "raw": True,
                                                              "delay": ["CH2:fall,CH3:rise", "CH3:fall,CH2:rise"], "points": 600000}))
        assert r["result"]["argv"] == ["scope", "capture", "CH2", "CH3", "--raw", "--delay", "CH2:fall,CH3:rise",
                                       "--delay", "CH3:fall,CH2:rise", "--points", "600000"]
        assert r["_call"]["command"] == "scope capture" and r["_call"]["effect"] == "actuate"
        assert r["_call"]["bench"]["power_stage"] == "absent"


async def test_invalid_arguments_are_refused_not_dropped(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        r = await c.call_tool("bu_doctor", {"offline": True, "bogus": 1})
        p = _payload(r)
        assert r.is_error and p["error_type"] == "invalid_arguments" and "bogus" in p["error"]
        r = await c.call_tool("bu_ioc_get", {})
        assert _payload(r)["error_type"] == "invalid_arguments"
        r = await c.call_tool("bu_scope_trigger", {"slope": "UP"})
        assert _payload(r)["error_type"] == "invalid_arguments"


async def test_bu_error_timeout_and_bad_output_are_typed(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        p = _payload(await c.call_tool("bu_fail", {}))
        assert p["ok"] is False and p["error_type"] == "bu_error" and p["result"]["hint"] == "set [scope].resource"
        t0 = time.monotonic()
        p = _payload(await c.call_tool("bu_slow", {"seconds": 5}))
        assert p["error_type"] == "timed_out" and time.monotonic() - t0 < 4
        p = _payload(await c.call_tool("bu_junk", {}))
        assert p["error_type"] == "bad_output" and "not json" in p["details"]["stdout_tail"]


async def test_gate_unset_absent_attached(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        # unset: every actuate refused, instrument ones included; read passes
        p = _payload(await c.call_tool("bu_flash", {}))
        assert p["error_type"] == "refused" and p["error"] == "bench_state_unset"
        assert _payload(await c.call_tool("bu_scope_capture", {"sources": ["CH1"]}))["error"] == "bench_state_unset"
        assert "bringup-mcp bench" in p["details"]["how_to_proceed"]
        assert _payload(await c.call_tool("bu_doctor", {}))["ok"]
        # absent: actuate runs
        gate.write_state(cfg.state_file, "absent", "dan", "bare nucleo", keep_tokens=False)
        p = _payload(await c.call_tool("bu_flash", {"no_verify": True}))
        assert p["ok"] and p["result"]["argv"] == ["flash", "--no-verify"]
        # attached: token required, one-shot, command-bound, expiry honoured
        gate.write_state(cfg.state_file, "attached", "dan", "proto board", keep_tokens=False)
        p = _payload(await c.call_tool("bu_flash", {}))
        assert p["error"] == "power_stage_attached_token_required"
        # ... but an instrument-only actuate runs without one
        p = _payload(await c.call_tool("bu_scope_trigger", {"slope": "NEG"}))
        assert p["ok"] and p["result"]["argv"] == ["scope", "trigger", "--slope", "NEG"]
        assert p["_call"]["bench"] == {"power_stage": "attached"}
        tok = gate.issue_token(cfg.state_file, "regulator clear-fault", 10, "dan")["token"]
        p = _payload(await c.call_tool("bu_flash", {"confirmation_token": tok}))
        assert p["error"] == "token_for_other_command"
        p = _payload(await c.call_tool("bu_regulator_clear_fault", {"confirmation_token": "amber-birch-000"}))
        assert p["error"] == "token_unknown"
        p = _payload(await c.call_tool("bu_regulator_clear_fault", {"confirmation_token": tok, "timeout": 2.5}))
        assert p["ok"] and p["result"]["argv"] == ["regulator", "clear-fault", "--timeout", "2.5"]
        assert p["_call"]["bench"]["token_consumed"] is True
        p = _payload(await c.call_tool("bu_regulator_clear_fault", {"confirmation_token": tok}))
        assert p["error"] == "token_already_used"
        tok2 = gate.issue_token(cfg.state_file, "flash", 0, "dan")["token"]  # expires immediately
        p = _payload(await c.call_tool("bu_flash", {"confirmation_token": tok2}))
        assert p["error"] == "token_expired"
        # a multi-use token covers exactly that many calls of its command
        tok3 = gate.issue_token(cfg.state_file, "flash", 10, "dan", uses=2)["token"]
        b = _payload(await c.call_tool("bench_state", {}))["result"]
        assert {"command": "flash", "uses_left": 2} in [{"command": x["command"], "uses_left": x["uses_left"]} for x in b["pending_tokens"]]
        assert _payload(await c.call_tool("bu_flash", {"confirmation_token": tok3}))["ok"]
        assert _payload(await c.call_tool("bu_flash", {"confirmation_token": tok3}))["ok"]
        p = _payload(await c.call_tool("bu_flash", {"confirmation_token": tok3}))
        assert p["error"] == "token_already_used"
        # bench_state never leaks token strings
        b = _payload(await c.call_tool("bench_state", {}))["result"]
        assert b["power_stage"] == "attached" and all("token" not in t for t in b["pending_tokens"])


async def test_write_effect_needs_no_interlock(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        p = _payload(await c.call_tool("bu_ioc_set", {"pairs": ["A=1"], "dry_run": True}))
        assert p["ok"] and p["result"]["argv"] == ["ioc", "set", "A=1", "--dry-run"]


async def test_calls_are_serialized(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        t0 = time.monotonic()
        a, b = await asyncio.gather(c.call_tool("bu_slow", {"seconds": 0.6}), c.call_tool("bu_slow", {"seconds": 0.6}))
        elapsed = time.monotonic() - t0
    pa, pb = _payload(a), _payload(b)
    assert pa["ok"] and pb["ok"]
    assert elapsed >= 1.1, "two slow calls must not overlap on the one worker"
    assert max(pa["_call"]["queued_s"], pb["_call"]["queued_s"]) >= 0.5


async def test_output_is_capped_and_says_so(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        p = _payload(await c.call_tool("bu_big", {}))
        assert p["ok"] and p["_truncated"] is True and "200000 bytes omitted" in p["result"]["blob"]
        assert p["result"]["n"] == 1


async def test_server_version_reports_running_code(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        v = _payload(await c.call_tool("server_version", {}))["result"]
    assert v["schema_sha256"] == st.catalogue.raw_sha256 and len(v["schema_sha256"]) == 64
    assert v["repo_branch"] in ("jonah-bench", "unknown") and "tools_dirty" in v


async def test_shell_allowlist_and_guards(repo):
    cfgp = write_config(repo, shell={"enabled": True, "allow": ["git", "echo", "sh"]})
    cfg, st, server = _server(cfgp)
    async with Client(server) as c:
        assert "shell_run" in {t.name for t in (await c.list_tools()).tools}
        p = _payload(await c.call_tool("shell_run", {"argv": ["echo", "hi"]}))
        assert p["ok"] and p["result"]["stdout"] == "hi\n" and p["result"]["returncode"] == 0
        p = _payload(await c.call_tool("shell_run", {"argv": ["rm", "-rf", "x"]}))
        assert p["error_type"] == "refused" and p["error"] == "program_not_allowlisted"
        p = _payload(await c.call_tool("shell_run", {"argv": ["sudo", "echo"]}))
        assert p["error"] == "sudo_never"
        p = _payload(await c.call_tool("shell_run", {"argv": ["git", "push", "origin", "main"]}))
        assert p["error"] == "git_push_protected_branch"
        p = _payload(await c.call_tool("shell_run", {"argv": ["git", "push", "--force", "origin", "feature"]}))
        assert p["error"] == "git_push_force"
        p = _payload(await c.call_tool("shell_run", {"argv": ["git", "push"]}))  # current branch jonah-bench is denied in the test config
        assert p["error"] == "git_push_protected_branch"
        p = _payload(await c.call_tool("shell_run", {"argv": ["echo", "x"], "cwd": "../.."}))
        assert p["error"] == "cwd_outside_repo"
        p = _payload(await c.call_tool("shell_run", {"argv": ["sh", "-c", "yes | head -c 1000"]}))
        assert p["ok"] and p["result"]["stdout_truncated"] is True and p["result"]["stdout_bytes"] == 1000
        p = _payload(await c.call_tool("shell_run", {"argv": ["sh", "-c", "sleep 5"], "timeout_s": 1}))
        assert p["error_type"] == "timed_out"


async def test_shell_disabled_is_a_refusal_even_if_called(config_path):
    cfg, st, server = _server(config_path)
    async with Client(server) as c:
        p = _payload(await c.call_tool("shell_run", {"argv": ["echo", "hi"]}))
        assert p["error_type"] == "refused" and p["error"] == "shell_disabled"


def test_config_fails_closed(tmp_path):
    from bringup_mcp.config import ConfigError
    with pytest.raises(ConfigError):
        load_config(str(tmp_path / "missing.toml"))
    bad = tmp_path / "bad.toml"
    bad.write_text('[repo]\nroot = "/nonexistent/dir"\n')
    with pytest.raises(ConfigError):
        load_config(str(bad))


def test_cli_bench_allow_status(config_path, capsys):
    from bringup_mcp.cli import main
    assert main(["--config", str(config_path), "allow", "flash"]) == 1  # unset: refused
    assert main(["--config", str(config_path), "bench", "attached", "--note", "proto"]) == 0
    assert main(["--config", str(config_path), "allow", "flash", "--minutes", "5"]) == 0
    assert main(["--config", str(config_path), "allow", "flash", "--minutes", "5", "--uses", "3"]) == 0
    assert '"uses": 3' in capsys.readouterr().out
    capsys.readouterr()
    assert main(["--config", str(config_path), "status"]) == 0
    status = json.loads(capsys.readouterr().out)
    assert status["bench"]["power_stage"] == "attached" and status["bench"]["pending_tokens"][0]["command"] == "flash"
    assert main(["--config", str(config_path), "check"]) == 0
    check = json.loads(capsys.readouterr().out)
    assert check["bu_commands"] == 11 and check["by_effect"] == {"read": 6, "write": 1, "actuate": 4}
