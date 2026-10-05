# Decisions and deliberate absences

Each entry: the context, what was decided, why. Dated 2026-09-27 unless noted. Written so the
next reader can disagree with a reason rather than guess at one.

## One tool per `bu` command, generated from `bu schema` at startup

Context: 54 commands, each with its own arguments. The alternative was 12 area tools with an
`op` enum and a bag of optional arguments.

Decided: one tool per command, generated at startup.

Why: a per-command tool has a schema in which only that command's arguments exist and the
required ones are required, so an illegal call cannot be expressed. An area tool with optional
arguments admits meaningless combinations and makes the server, not the schema, the validator.
Generating at startup means the tools match the code on the bench, never a copy. TOOLS.md was
written for exactly this mapping (effect classes to annotations). The cost is 54 entries in the
client's tool list; Claude Code defers tool schemas, so that cost is small.

## `--force` is unreachable from the agent

`regulator clear-fault --force` retries after a repeat ADM1270 over-current; `cubemx generate
--force` overrides the guard against losing USER CODE. Both are removed from every generated
schema. Dan's own tool notes say not to let a model pass `--force` without a human; the
cleanest way to guarantee that is for the argument not to exist on this side.

## The interlock is a file on the bench, written only by local commands

Context: SKILL.md's safety rules ask the agent to "ask the user before" flashing or writing
control variables with the power stage attached. A request in a description is not a gate.

Decided: `bringup_out/bench-state.json`, with `power_stage` unset, `absent` or `attached`.
Unset refuses every actuate call (fail closed). `absent` runs them freely, which is Dan's bench
today. `attached` requires a one-shot token issued locally for one command.

Why: the person who knows what is on the bench is the person at the bench. A token issued by
Dan's hands for one named command, expiring and consumed on use, is the mechanical form of
"ask first". The server never writes the state itself, so no agent code path can flip it. No
default value exists: a missing file is refused, not assumed absent.

Absence: no MCP elicitation for confirmation. A client that cannot ask is a refusal, not a
pass, and the client here is a remote agent, not the person at the bench.

## Instrument-only actuate calls need no token while attached (added 2026-09-27 15:3x)

Context: the first bench session. Dan's `bu` marks every scope, analyzer and serial command
`actuate`, correctly (they change instrument state). Under the rule above a scope session
(channel scale, offset, timebase, trigger, acquire, capture) would have cost Dan a token per
command, a dozen for one measurement, and the person at the bench would learn to hand out
tokens without reading them, which is the failure the interlock exists to prevent.

Decided: a command's first word decides what it reaches. `[bench] instrument_prefixes`
(default `scope`, `la`, `serial`) marks commands that reach a bench instrument; their actuate
calls run without a token while attached. Everything else that actuates (`flash`, `mem write`,
`regulator *`, `probe reset`) reaches the board and keeps the token rule. Unset still refuses
all of them, so the fail-closed default is unchanged. The tool schema shows the difference:
instrument tools carry no `confirmation_token` argument.

Why the first word and not a per-command list: `bu` groups its commands by subject, so the
prefix is the subject, and a new `scope foo` command lands on the right side without an edit
here. The list is config, not code, so Dan can move a subject across the line without a rebuild.

## Tokens are opt-in (changed 2026-09-27 16:43)

Context: the first bench session. Jonah, thread 9094: "Cant it be made so you don't ned token
shuffling? Unless Dan want that as a securtiy boundaeyx"; Dan, 9096: "I'm frustrated the spec
makes it impossible to remote control the testing: which is exactly the problem I was trying
to work around"; Jonah, 9098: "Unless Dan want time grants, please edit the mcp to not need
them"; Jonah 9099: "I'm fine without the grants, allowed by me if Dan concurs"; Dan 9100: "No
grants is okay with me too, as long as I can ensure the service is stopped eventually".

Decided: `bench attached` alone lets actuate calls run, the same as `absent`, and tells the
agent through `bench_state` that the power stage is live. `bench attached --tokens` restores
the previous behaviour in full (per-command tokens, `--uses`, expiry). Unset still refuses
everything. The state file is the only switch, still written only by Dan's hands on the bench.
Stopping the service is `pm2 stop pd-bu` (and `tailscale serve reset` to close the port).

Why keep the mechanism at all: it costs nothing when off, and a session with a load, a new
board, or a second person on the bench may want it back. Why not delete the attached state:
the agent still needs to know whether its next flash reaches a powered stage, because SKILL.md
tells it to behave differently when it does; that is information, not a gate.

## A token may cover several uses (added 2026-09-27 16:0x)

Context: Dan, after the first closed-loop fault: "Keep trying until it works". Iterating a
control fix is build, flash, reset, read, repeat, and each flash and reset reaches the board.
One token per run would put Dan on token duty for the whole session.

Decided: `allow <command> --uses N` issues one token that covers N calls of that one command,
expiring as before. The default stays one use. `bench_state` shows the uses left. Everything
else about the interlock is unchanged: the command binding, the expiry, the refusal when unset,
and that only Dan's hands issue it. A larger N is his statement of how far "keep trying" goes.

Why not "attached, unattended": that would make one flag mean two things. The bench state says
what is on the bench; the token says what the person there has agreed to.

## `write` effects are not gated

Edits to `.ioc`, USER CODE, CubeMX regeneration and builds change files in a git checkout on a
branch Dan named for this (`jonah-bench`). They are reversible with git and touch no hardware.
Gating them would train the agent to expect refusals on harmless work.

## One worker, in order, with the wait reported

One ST-LINK and one scope. Concurrent commands collide (TOOLS.md). A single asyncio lock
serializes every external call, including `server_version`'s git reads, so nothing races the
probe. `queued_s` is reported so a slow result can be told apart from a slow queue.

## Timeouts per command, from measured values

TOOLS.md gives measured times; the config carries them with margin (`cubemx generate` 600 s,
`scope capture` 90 s, default 30 s). On expiry the process is killed and the result says
`timed_out`. A call that hangs on a dialog on the bench (CubeMX wanting a display, a udev
prompt) surfaces as `timed_out` with the argv, which is honest but not yet the ethos's
"awaiting a person" state.

Absence: no detection of OS dialogs. The bench is headless Linux under pm2; the known
dialog-shaped hazards (CubeMX needing `DISPLAY`, scope udev rule) are one-time setup that
`bu doctor` reports. If a recurring one appears, add detection then, with evidence.

## Results pass `bu`'s JSON through unchanged

The agent reads TOOLS.md and SKILL.md, which describe `bu`'s result fields. Reshaping them
here would put a second description between the agent and the truth. The server adds only
`_call` and, on failure, `error_type`.

## `verified: false` stays a success

`mem write` to write-only registers returns `ok: true, verified: false` because the readback
cannot pass. Dan: "mem write is too useful to not use even though it throws an error". The
server keys success on `ok` only and leaves `verified` for the agent to read.

## Output capped, and the cap is visible

Any result over 64 kB, or any field over 8 kB, is replaced by a size note and the result is
marked `_truncated`. `sym` (21 kB) and `schema`-sized outputs fit. Captures live on disk and
are read through the filesystem server or `bu scope analyze`.

## The shell exists, off by default, allowlisted, and not interlocked

Context: Jonah, 2026-09-27, thread 8989: "I'm thinking exposing the gh and git cli commands ;
and the shell commands". The transcript shows the agent needed git (38 calls), pytest, and a
programmer loop for PC sampling.

Decided: `shell_run` appears only when `[shell] enabled = true`; it runs an argv list (no
shell), cwd inside the repo, output capped; refuses `sudo`, anything off `allow`, `git push`
to protected branches or with `--force`, and `gh pr merge`. It is not gated by the bench
interlock.

Why not interlocked: the interlock exists for hardware. The shell reaches hardware only
through programs on the allowlist, so the allowlist is the gate: keep `STM32_Programmer_CLI`
and `openocd` off it and use the `bu_*` tools, which are interlocked. PC sampling, the one
programmer use the transcript needed outside `bu`, is its own read tool instead.

## `probe_pc_sample` is a separate read tool

It re-implements the loop the transcript's agent ran by hand (`-coreReg` six times, addr2line
on PC and LR). Hot-plug attach does not halt the core. It finds the programmer the way
`bringup.config` does, without importing Dan's package, so this server has no import-time
dependency on `tools/bringup` and a broken `bu` still lets `server_version` and `bench_state`
answer. Marked in its description as not yet run through this server on the bench.

## No import of Dan's package; everything through the `bu` launcher

The launcher is the one code path Dan documented and tested. Importing `bringup` in-process
would create a second path with its own config loading and error handling, and would make
this server's uptime depend on that package importing cleanly.

## Transport: the server speaks streamable HTTP itself

No Supergateway. `mcp` 2.x provides the ASGI app; uvicorn serves it on 127.0.0.1 and
`tailscale serve` fronts it. Host-header (DNS-rebinding) protection is on, so
`[http] allowed_hosts` must name the tailnet host and port; a wrong Host answers 421. Stateless
mode, so a pm2 restart does not strand sessions.

Absence: no bearer token. Tailnet membership plus Dan's ACL is the access control today.
Revisit before the power stage runs under load.

## Not built, on purpose

- A scope, probe or serial server of its own: `bu` fronts them.
- A general shell over the tailnet: the allowlisted one above is the whole grant.
- Artifact streaming: files stay on disk; the filesystem server reads them.
- Sending commits or pushes on the agent's behalf outside `shell_run`'s guards.
