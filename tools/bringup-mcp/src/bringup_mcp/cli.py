"""`bringup-mcp` command line: serve, and the bench-side interlock commands."""

from __future__ import annotations

import argparse
import asyncio
import getpass
import json
import sys

from .config import ConfigError, load_config
from .gate import issue_token, read_state, write_state
from .schema import SchemaError


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="bringup-mcp", description="MCP server for tools/bringup/bu, with a bench interlock")
    p.add_argument("--config", help="path to bringup-mcp.toml (default: next to the package, or $BRINGUP_MCP_CONFIG)")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("serve", help="run the server")
    s.add_argument("--transport", choices=["stdio", "http"], default="http")
    s.add_argument("--host", help="override [http] host")
    s.add_argument("--port", type=int, help="override [http] port")

    b = sub.add_parser("bench", help="set the bench interlock state (run this on the bench host)")
    b.add_argument("power_stage", choices=["attached", "absent"])
    b.add_argument("--note", help="what is on the bench, e.g. 'proto board, VIN off'")
    b.add_argument("--keep-tokens", action="store_true", help="keep pending tokens (default: clear them)")
    b.add_argument("--tokens", action="store_true",
                   help="attached only: require a confirmation token per board-reaching actuate call (off by default)")

    a = sub.add_parser("allow", help="issue a confirmation token for one actuate command (one-shot unless --uses)")
    a.add_argument("command", help='exact bu command, e.g. "flash" or "regulator clear-fault"')
    a.add_argument("--minutes", type=float, default=10.0)
    a.add_argument("--uses", type=int, default=1, help="how many calls the token covers (default 1)")

    sub.add_parser("status", help="print the interlock state and pending tokens")
    sub.add_parser("check", help="load config, run `bu schema`, print tool count")

    args = p.parse_args(argv)
    try:
        cfg = load_config(args.config)
    except ConfigError as e:
        print(json.dumps({"ok": False, "error_type": "not_configured", "error": str(e)}, indent=1))
        return 2

    if args.cmd == "bench":
        st = write_state(cfg.state_file, args.power_stage, getpass.getuser(), args.note, args.keep_tokens, args.tokens)
        print(json.dumps({"ok": True, "bench": st.public()}, indent=1))
        return 0
    if args.cmd == "allow":
        try:
            tok = issue_token(cfg.state_file, args.command, args.minutes, getpass.getuser(), args.uses)
        except RuntimeError as e:
            print(json.dumps({"ok": False, "error_type": "refused", "error": str(e)}, indent=1))
            return 1
        print(json.dumps({"ok": True, "token": tok["token"], "command": tok["command"], "expires_at": tok["expires_at"],
                          "uses": tok["uses_left"],
                          "give_to_agent": f"confirmation_token={tok['token']} for {tok['command']}"}, indent=1))
        return 0
    if args.cmd == "status":
        print(json.dumps({"ok": True, "bench": read_state(cfg.state_file).public(), "config": str(cfg.path)}, indent=1))
        return 0

    from .server import build_state, make_server  # imported late: mcp is heavy
    try:
        st = build_state(cfg)
    except SchemaError as e:
        print(json.dumps({"ok": False, "error_type": "bad_output", "error": str(e)}, indent=1))
        return 2

    if args.cmd == "check":
        eff = {}
        for c in st.catalogue.commands:
            eff[c.effect] = eff.get(c.effect, 0) + 1
        print(json.dumps({"ok": True, "tools": len(st.tools), "bu_commands": len(st.catalogue.commands),
                          "by_effect": eff, "schema_sha256": st.catalogue.raw_sha256,
                          "bench": read_state(cfg.state_file).public(), "shell_enabled": cfg.shell.enabled}, indent=1))
        return 0

    server = make_server(st)
    if args.transport == "stdio":
        from mcp.server.stdio import stdio_server

        async def run_stdio() -> None:
            async with stdio_server() as (r, w):
                await server.run(r, w, server.create_initialization_options())

        asyncio.run(run_stdio())
        return 0

    import uvicorn
    from mcp.server.transport_security import TransportSecuritySettings

    host = args.host or cfg.http.host
    port = args.port or cfg.http.port
    app = server.streamable_http_app(
        streamable_http_path="/mcp",
        json_response=False,
        stateless_http=True,
        transport_security=TransportSecuritySettings(enable_dns_rebinding_protection=True,
                                                     allowed_hosts=list(cfg.http.allowed_hosts),
                                                     allowed_origins=[]),
    )
    print(json.dumps({"ok": True, "serving": f"http://{host}:{port}/mcp", "tools": len(st.tools),
                      "allowed_hosts": list(cfg.http.allowed_hosts), "bench": read_state(cfg.state_file).public()}),
          file=sys.stderr, flush=True)
    uvicorn.run(app, host=host, port=port, log_level="warning")
    return 0


if __name__ == "__main__":
    sys.exit(main())
