# bringup-mcp

An MCP server that fronts `tools/bringup/bu` (Dan's bring-up CLI for the PD_Charger regulator)
so an agent on another machine can run the closed loop over the tailnet: ioc, CubeMX, USER
CODE, build, flash, SWD memory, regulator mailbox, scope. It adds the one thing the CLI cannot
provide on its own: a bench interlock that a remote agent cannot talk its way past.

Written 2026-09-27 by gritty-obsidian for Dan Raymond and Jonah. Offline tests pass; it has
not yet been run on the bench. The integration checklist at the end is what turns it from
"built" into "verified".

## What it does

- Reads `bu schema` once at startup and generates one MCP tool per `bu` command (54 on the
  current branch), named `bu_<command>`, e.g. `bu_scope_capture`, `bu_regulator_clear_fault`.
  Arguments, types, enums and required-ness come from the schema, so the tools cannot drift
  from the CLI. Any `--force` argument is removed: those stay Dan's, by hand.
- Maps Dan's effect classes to MCP annotations: `read` is read-only, `write` is non-destructive
  (git-reversible), `actuate` is destructive.
- Gates every `actuate` call through the bench interlock (below).
- Runs one external call at a time (one ST-LINK, one scope) and reports the queue wait.
- Applies a per-command timeout (TOOLS.md's measured times plus margin) and kills on expiry.
- Returns `bu`'s JSON unchanged under `result`, with `_call` (argv, seconds, queued_s, effect,
  bench state). Every failure has a type: `bu_error`, `refused`, `timed_out`, `not_configured`,
  `invalid_arguments`, `bad_output`, `internal`. A refusal says how to proceed.
- Caps any result at 64 kB and any single field at 8 kB, and marks the result `_truncated`.
- Extra tools: `bench_state`, `server_version` (which code is running), `probe_pc_sample`
  (PC and LR over SWD hot-plug, resolved with addr2line: the technique that found both firmware
  bugs on 2026-09-27), `bu_test` (the offline pytest suite), and, only when enabled in config,
  `shell_run` (allowlisted programs, argv only, cwd pinned inside the repo, `git push` to
  protected branches and `gh pr merge` refused, no sudo ever).

## The bench interlock

State lives in `bringup_out/bench-state.json` on the bench host. The server only reads it; the
two commands below write it and are meant to be run by Dan, locally.

| power_stage | actuate calls |
|---|---|
| unset (file missing or unreadable) | refused, always. The server fails closed. |
| `absent` (bare Nucleo) | run without ceremony, as on Dan's bench today |
| `attached` (power board connected) | run; the agent is told the power stage is live and SKILL.md's rules are its to follow. With `bench attached --tokens`, board-reaching calls (flash, mem write, regulator, probe) additionally need a `confirmation_token` issued for that exact command; instrument-only calls (scope, la, serial) never do |

```
bringup-mcp bench absent   --note "nucleo only"          # or: bench attached --note "proto board, VIN off"
bringup-mcp bench attached --tokens --note "24V, load"  # same, but every board-reaching call needs a token
bringup-mcp allow "regulator clear-fault" --minutes 10   # prints a token; give it to the agent
bringup-mcp allow flash --uses 20 --minutes 240          # one token, twenty flashes, four hours
bringup-mcp status                                       # state and pending tokens
```

Tokens are bound to one command and expire. By default a token is consumed on first use;
`--uses N` issues one that covers N calls of that command (Dan's "keep trying until it works",
2026-09-27), so an iteration of flash and reset does not need a token per run. `bench_state` shows
pending tokens by command and expiry, never the token string. Setting the bench state clears
pending tokens unless `--keep-tokens` is passed.

`read` and `write` effects are never gated: reading is harmless and writes are git-reversible.
Which commands count as instrument-only is `[bench] instrument_prefixes` in the config (first
word of the `bu` command; default `scope`, `la`, `serial`).

## Install on the bench host

Requires `uv` and Python 3.11+. Unzip or copy this directory to `tools/bringup-mcp/` in the
PD_Charger checkout (or anywhere; the config names the repo).

```
cd tools/bringup-mcp
cp bringup-mcp.example.toml bringup-mcp.toml      # edit [repo] root and [http] allowed_hosts
uv sync
uv run bringup-mcp check                          # loads config, runs `bu schema`, prints the tool count
uv run bringup-mcp bench absent --note "nucleo only"
```

`check` fails, with a typed JSON error, if the repo root, `bu`, or the schema is not usable.

## Run it under pm2 behind tailscale serve

The server speaks MCP streamable HTTP itself, on `127.0.0.1:<port>/mcp`; no Supergateway
needed. `tailscale serve` publishes it tailnet-only with HTTPS. Host-header protection is on:
`[http] allowed_hosts` must include the tailnet name with the port, or the server answers 421.

```
pm2 start --name pd-bu --cwd /home/daniel/GitHub/PD_Charger/tools/bringup-mcp -- uv run bringup-mcp serve --transport http
pm2 save
tailscale serve --bg --https=40050 http://127.0.0.1:40050
tailscale serve status
```

Then on the agent's side:

```
claude mcp add --transport http pd-bu https://framework16.tail88868f.ts.net:40050/mcp
```

Smoke test from any tailnet device:

```
curl -s https://framework16.tail88868f.ts.net:40050/mcp -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}'
```

Expect `serverInfo.name` = `bringup-mcp`.

## Enabling the shell (Jonah's direction of 2026-09-27)

Set `[shell] enabled = true` and keep `allow` to what the loop needs (`git`, `gh`, `uv`,
`arm-none-eabi-addr2line`). Then `shell_run` appears as a tool. It runs an argv list with no
shell, cwd inside the repo only, output capped with the full size reported. It refuses `sudo`,
anything off the list, `git push` to `deny_git_push_to` branches or with `--force`, and
`gh pr merge`. It is not gated by the bench interlock, because it cannot reach the hardware
except through programs on the allowlist; do not put `STM32_Programmer_CLI` on the list, use
the `bu_*` tools for that so the interlock applies.

## Tests

```
uv run pytest -q          # 13 offline tests against tests/fake_bu.py
```

They cover schema-to-tool generation, argv building, refusals for bad arguments, typed errors
for `ok:false`, timeouts and non-JSON output, the interlock in all three states with token
binding, one-shot use and expiry, serialization of concurrent calls, output capping, the shell
allowlist and its guards, and config failing closed. They prove the mechanism, not the bench.

## Integration checklist (on the bench, with Dan)

1. `uv run bringup-mcp check` shows 54 `bu` commands and the effect counts from TOOLS.md.
2. `bench absent`; through the client: `bench_state`, `bu_doctor {offline:true}`,
   `bu_ioc_get`, `bu_build`, `bu_probe_list`, `bu_mem_read` on `uwTick` twice (values move).
3. `server_version` shows the right commit and `tools_dirty` false.
4. `probe_pc_sample {samples: 3}` resolves PC to a function name on the running Nucleo.
5. `bench attached --tokens`; `bu_flash` is refused with `power_stage_attached_token_required`;
   `allow "flash"`; `bu_flash` with the token runs; the same token is refused a second time.
6. `bu_scope_capture` with the DS1104Z on USB returns a CSV path and summary under 90 s.
7. Two overlapping calls from two clients: the second reports `queued_s` > 0, neither fails.
8. Restart the server after any change to it; the client keeps the old tool schema until it
   reconnects.

## Files

- `src/bringup_mcp/config.py`: the TOML, validated, failing closed.
- `src/bringup_mcp/schema.py`: `bu schema` to tools; tool arguments to argv.
- `src/bringup_mcp/gate.py`: the interlock state file, tokens, and the one `decide()`.
- `src/bringup_mcp/runner.py`: the single worker, timeouts, typed outcomes.
- `src/bringup_mcp/extras.py`: bench_state, server_version, probe_pc_sample, bu_test, shell_run.
- `src/bringup_mcp/server.py`: the MCP server and dispatch.
- `src/bringup_mcp/cli.py`: `serve`, `bench`, `allow`, `status`, `check`.
- `DECISIONS.md`: what was decided, what was left out, and why.
