# bringup tool reference (for MCP / agent integration)

Everything here was measured on the development bench (Ubuntu, NUCLEO-G474RE with
ST-LINK V3, Rigol DS1104Z) on 2026-09-27 unless marked **untested**.

- Human setup and design notes: [README.md](README.md)
- Agent workflow and safety rules: [`.claude/skills/hw-bringup/SKILL.md`](../../.claude/skills/hw-bringup/SKILL.md)
- Machine-readable command catalogue: `tools/bringup/bu schema` (JSON, ~33 kB)

## 1. Calling convention

```
tools/bringup/bu <command> [<subcommand>] [args...]
```

- Run from anywhere; paths in results are relative to the repo root.
- **stdout is exactly one JSON object.** `"ok": true|false`; failures carry
  `"error"` plus details (`hint`, `output_tail`, ...). Exit code 0 iff `ok`.
- stderr is noise (pyvisa-py discovery warnings, uv); ignore it.
- Large outputs are written to `bringup_out/` (gitignored), named
  `<YYYYmmdd-HHMMSS>_<kind>.<ext>`, and returned as a repo-relative path.
- Config: `tools/bringup/bringup.toml` (committed defaults) deep-merged with
  `tools/bringup/bringup.local.toml` (gitignored: scope address, probe serial).
- First run creates `tools/bringup/.venv` via `uv` (needs PyPI access once, ~14 MB).

### Wrapping as an MCP server

`bu schema` lists all 56 commands with arguments (name, flag, positional,
required, kind `flag|value|list`, type, choices, default, help) and an
**effect class** you can map to MCP tool annotations and a confirmation policy:

| effect | meaning | suggested MCP annotation / policy |
|---|---|---|
| `read` | no state change (files or hardware read only) | `readOnlyHint: true`; auto-approve |
| `write` | modifies repo files (`.ioc`, sources, build dir); reversible with `git` | `destructiveHint: false`; approve or run on a branch |
| `actuate` | changes live hardware or instrument state (flash, reset, RAM/register writes, gate outputs, scope settings) | `destructiveHint: true`; require human confirmation for `flash`, `mem write`, `regulator *` when a power stage is attached |

Recommendations:

- **Serialize calls.** There is one ST-LINK and one scope; commands open and
  close their own session, and two concurrent probe or scope commands will
  collide. Use a single-worker queue in the MCP server.
- **Timeouts:** most calls take < 2 s; `cubemx generate` takes 20–50 s, and a raw
  scope capture 4–6 s. See §5. Give the MCP call a 600 s ceiling.
- **Return files, not blobs:** keep large artifacts on disk and hand the model the
  path plus the JSON summary. `scope analyze <csv>` re-derives numbers from a
  saved capture without touching hardware.
- The CLI never prompts interactively, so it is safe to call headlessly.

## 2. Command catalogue

Times and JSON sizes are measured (typical). "Artifact" = file written to `bringup_out/`.

| Command | Effect | What it does | Time | JSON | Artifact |
|---|---|---|---|---|---|
| `schema` | read | command catalogue | 0.1 s | 33 kB | – |
| `doctor [--offline]` | read | toolchain/probe/scope/analyzer checks | 0.2 s offline, ~3 s online | 1–3 kB | – |
| `ioc get KEY…` | read | read .ioc keys (unescaped) | 0.1 s | < 1 kB | – |
| `ioc search REGEX [--limit]` | read | regex over keys+values | 0.1 s | 1–6 kB | – |
| `ioc ips` / `ioc pins [PIN]` | read | enabled IPs / pin config | 0.1 s | 1–4 kB | – |
| `ioc set K=V… [--dry-run]` | write | edit .ioc, maintains IPParameters/GPIOParameters | 0.1 s | < 1 kB | – |
| `ioc delete KEY… [--dry-run]` | write | remove keys | 0.1 s | < 1 kB | – |
| `ioc diff [--ref]` | read | semantic diff vs a git ref | 0.1 s | < 2 kB | – |
| `usercode scan [--all]` | read | files with USER CODE sections | 0.1 s | 5.5 kB | – |
| `usercode list FILE` | read | section ids + line numbers | 0.1 s | ~6 kB (main.c) | – |
| `usercode get FILE ID` | read | section body | 0.1 s | body size | – |
| `usercode set FILE ID (--text\|--from\|stdin)` | write | replace a section body (CRLF-safe) | 0.1 s | < 1 kB | – |
| `cubemx generate [--apply] [--only GLOB…] [--force]` | write | headless CubeMX in a temp copy; dry-run by default; reports USER CODE loss + changes outside USER CODE | 20–50 s | 2–10 kB (diff budget `--diff-lines`) | `cubemx.log` 146 kB, `cubemx_diff.patch` 5–7 kB |
| `cubemx script CMD…` | write | raw CubeMX script commands | 20–50 s | < 2 kB | `cubemx.log` |
| `build [--clean] [--reconfigure] [-j]` | write | CMake/Ninja build, parsed diagnostics, FLASH/RAM % | 0.1 s no-op, 1.7 s clean | 0.3–5 kB | `build.log` 0.1–11 kB |
| `probe list` | read | ST-LINK serial/firmware/board | 0.3 s | < 1 kB | – |
| `probe reset [--hard]` | actuate | reset core (hot-plug) | 0.3 s | < 1 kB | – |
| `flash [--elf] [--no-verify] [--no-reset]` | actuate | program + verify + reset | 4 s | < 1 kB | – |
| `sym [REGEX]` | read | ELF symbols (addr, size), max 300 | 0.1 s | ≤ 21 kB | – |
| `layout EXPR` | read | struct layout with offsets (gdb `ptype /o`) | 0.2 s | < 1 kB | – |
| `mem read TARGET [--type --count --size --save]` | read | live RAM/register read by symbol or address, core keeps running | 0.2 s | ≤ 2 kB (max 256 values inline) | `mem_*.bin` if `--save` or > 1 kB (e.g. `debug_log` 12.3 kB) |
| `mem write TARGET VALUE [--type]` | actuate | live write; aligned single bus write for 8/16/32-bit | 0.3 s | < 1 kB | – |
| `regulator status` | read | state, fault source, ADM1270 lines, HRTIM flags/IRQ/outputs, ADC, diagnosis, protection thresholds | 0.15 s | 1.3 kB (+1 kB protection table) | – |
| `regulator clear-fault [--force]` | actuate | firmware: ADM1270 cool-down → INPUT_EN toggle → verify → FAULT→IDLE | 0.1–0.5 s | 1.5 kB | – |
| `regulator stop` | actuate | outputs off, power path off | 0.2 s | 1.5 kB | – |
| `regulator bench-pwm on\|off` | actuate | open-loop gate test (IDLE, input path off, fault lines high) | 0.2 s | 1.5 kB | – |
| `regulator set-voltage MV` | actuate | new regulation target through the mailbox (`arg` word); firmware slews to it | 0.3 s | 1.5 kB | – |
| `regulator start MV` | actuate | set target and start from IDLE | 0.3–1 s | 1.5 kB | – |
| `scope idn` / `scope state` | read | identity / full channel+timebase+trigger+acquire state | 0.1 s / 1.2 s | < 1 kB / 1 kB | – |
| `scope chan N …`, `timebase`, `trigger`, `acquire` | actuate | configure; returns SCPI error queue + new state | 0.3–1.5 s | < 1 kB | – |
| `scope run\|stop\|single\|force\|clear\|autoscale\|reset` | actuate | acquisition control | 0.1–3 s | < 1 kB | – |
| `scope wait [--timeout] [--no-arm]` | actuate | arm single + wait for trigger | ≤ timeout | < 1 kB | – |
| `scope measure SRC… [--items]` | read | scope's built-in measurements (duty in %) | ~0.45 s per item×channel | < 1 kB | – |
| `scope delay ITEM A B` | read | RDELay/FDELay/RPHase/FPHase | 0.2 s | < 1 kB | – |
| `scope capture SRC… [--raw] [--points] [--threshold] [--delay]` | actuate (stops scope in `--raw`) | download samples → CSV → analysis (levels, freq, duty, jitter, edges, delay stats) | 0.3 s screen, 4–6 s raw 600 k pts | 3–7 kB | CSV: 32 kB (screen), 15 MB (4 ch × 300 k), 22 MB (2 ch × 600 k) |
| `scope analyze CSV [--threshold] [--delay]` | read | same analysis offline | 0.6–1.3 s per 1.2 M samples | 4–7 kB | – |
| `scope screenshot` | read | PNG of the scope screen | 1.8 s | < 1 kB | PNG 40–90 kB |
| `scope scpi CMD` | actuate | raw SCPI escape hatch | 0.1 s | < 1 kB | – |
| `la scan\|decoders\|capture\|decode\|edges` | read | sigrok-cli capture/decode (**untested**: sigrok-cli not installed) | – | – | `.sr`, decode `.txt` |
| `serial list [--all]` / `serial capture …` | read | USB serial ports / capture (**capture untested**) | 0.1 s / `--seconds` | < 2 kB | `.log` / `.bin` |

## 3. External programs and libraries

| Program | Version on bench | Used by | Required? |
|---|---|---|---|
| STM32CubeMX | 6.16.1 (`/usr/local/STMicroelectronics/STM32Cube/STM32CubeMX/`) | `cubemx` (`-q` script mode; needs a display, or `xvfb-run` if `DISPLAY` is unset) | for `.ioc` → code only |
| STM32CubeProgrammer CLI | 2.20.0 (bundled with STM32CubeIDE 1.17) | `flash`, `probe`, `mem`, `regulator` | yes (or OpenOCD, below) |
| OpenOCD | 0.12.0+dev | alternative probe backend (`[probe].backend = "openocd"`) | optional, **untested** |
| arm-none-eabi-gcc / binutils | 14.2.1 | `build`; `nm` for `sym`/`mem`; `size` | yes |
| CMake / Ninja | 4.2.3 / 1.13.2 | `build` (CubeMX-generated CMake project) | yes |
| gdb-multiarch | 17.1 | `layout` (reads the ELF only) | optional |
| git | any | `ioc diff` | optional |
| sigrok-cli | not installed | `la` | optional |
| uv | 0.11.9 | runs the tool, manages `.venv` | yes |
| Python | 3.12.13 (uv-managed; ≥ 3.11 required for `tomllib`) | everything | yes |
| pyvisa / pyvisa-py / pyusb | 1.16.2 / 0.8.1 / 1.3.1 | scope over USB (raw USBTMC) | for scope over USB |
| pyserial | 3.5 | `serial` | for serial |
| pytest | 9.1.1 (dev) | `uv run pytest` (27 offline tests incl. a fake SCPI scope) | dev only |

Not used by the tool at runtime: KiCad (`kicad-cli` was used once to extract
the netlist for the ADM1270 thresholds in `regulator.py`).

## 4. File types

| Type | Read | Written | Notes |
|---|---|---|---|
| `.ioc` (Java .properties) | ✓ | ✓ `ioc set/delete`, `cubemx --apply` | keys/values unescaped for the caller; unchanged lines byte-identical; CRLF/LF preserved |
| `.c` `.h` `.s` `.txt` `.cmake` `.ld` | ✓ | ✓ `usercode set`, `cubemx --apply` | only inside `USER CODE BEGIN/END`; CRLF/LF preserved per file |
| `CMakeLists.txt`, `.mxproject`, `.cproject` | ✓ | ✓ `cubemx --apply` | CubeMX-generated |
| `.elf` | ✓ | ✓ `build` (2.9 MB debug ELF; 147 kB flash image) | symbols (`nm`), struct layouts (gdb), flash image |
| `.hex` `.bin` `.map` | – | ✓ `build` (make backend) | |
| `.json` | – | stdout | one object per call |
| `.csv` | ✓ `scope analyze` | ✓ `scope capture` | header `t_s,CH1,…`; time relative to trigger (s, 12 significant digits), volts (5 sig. digits) |
| `.png` | – | ✓ `scope screenshot` | 800×480 scope screen |
| `.log` | – | ✓ build / cubemx / serial text | |
| `.patch` | – | ✓ `cubemx generate` | unified diff of the whole generation |
| `.bin` | – | ✓ `mem read --save`, `serial --hex` | raw little-endian memory |
| `.sr` / `.txt` | ✓ | ✓ `la` | sigrok session / decoder annotations (**untested**) |
| `.toml` | ✓ | – | configuration |
| `.rules` | – | – | udev rule you install by hand |

## 5. Data sizes and throughput

| Data | Size | Time |
|---|---|---|
| Typical JSON result | 0.1–7 kB (largest: `schema` 33 kB, `sym` 21 kB) | – |
| Scope screen capture (`capture`, no `--raw`) | 1200 points/channel, CSV ≈ 32 kB for 2 ch | 0.3 s |
| Scope raw capture | 600 k pts × 2 ch @ 500 MSa/s = 1.2 ms → CSV 22 MB | 4.6 s download + ~1 s analysis |
| | 300 k pts × 4 ch @ 250 MSa/s → CSV 15 MB | 4.5 s |
| Scope screenshot | PNG 40–90 kB | 1.8 s |
| CubeMX generation | temp copy of the project (14 MB), log 146 kB, patch 5–7 kB | 20–50 s |
| Build | ELF 2.9 MB | 0.1 s no-op, 1.7 s clean |
| Flash + verify | 147 kB image | 4 s |
| SWD memory read | any size; `debug_log` = 12,300 bytes | 0.15–0.2 s per session (several regions per session) |
| `bringup_out/` after this session | 77 MB | prune old captures; nothing reads them except `scope analyze` |

Scope sample-rate limits (DS1104Z): 1 GSa/s with 1 channel, 500 MSa/s with 2,
250 MSa/s with 3–4. Always check `sample_rate_sa_s` in the result: an explicit
`--mdepth 600k` fell back to 250 MSa/s where `auto` gave 500 MSa/s.

## 6. Permissions

### Operating system
| Resource | Needed for | How it was granted on the bench |
|---|---|---|
| ST-LINK USB (`0483:374e`) | flash/probe/mem/regulator | STM32CubeIDE's udev rules (`/etc/udev/rules.d/49-stlinkv3.rules`) |
| Rigol USB (`1ab1:04ce`) | scope over USB | `tools/bringup/udev/99-bringup.rules` → group `plugdev` (user must be in `plugdev`); **needs sudo once** |
| Serial ports | `serial` | group `dialout` |
| Logic analyzer USB | `la` | sigrok's own udev rules (not installed yet) |
| Display (X11/Wayland) or `xvfb-run` | `cubemx` | desktop session |
| Network | first `uv` run (PyPI); optional scope over LAN (`tcp://host:5555`) | – |
| Filesystem | writes `STM32CubeIDE/final/**` (write commands), `bringup_out/`, `tools/bringup/.venv`, temp dirs `/tmp/bringup-mx-*` (removed after use) | repo checkout |

No root access is needed at runtime; the only sudo step is installing the scope's udev rule.

### Agent / MCP policy
- **`actuate` commands change real hardware.** With the power stage attached,
  `flash`, `mem write` (setpoints, gains, HRTIM registers), `regulator
  clear-fault|bench-pwm|set-voltage|start` and `probe reset` need a human in the loop (see the
  skill's safety rules). On a bare Nucleo they are harmless.
- `mem write` to write-only/self-clearing peripheral registers returns
  `ok: true, verified: false` (the programmer's read-back check can't pass);
  confirm by the effect (`regulator status`, scope).
- `regulator clear-fault` refuses to retry after a repeat ADM1270 over-current
  unless `--force`: don't let a model pass `--force` without a human.
- In Claude Code, allowing `Bash(tools/bringup/bu:*)` removes per-call
  prompts. During development the auto-mode classifier once blocked a change
  that relaxed `mem write` verification and re-enabled a gate output in one step;
  expect similar checks from other agent hosts.

## 7. Hardware coverage and known limits

Verified: NUCLEO-G474RE (ST-LINK V3 J16M9); Rigol DS1104Z firmware 00.04.04.SP3 over USB.
Not yet run: OpenOCD backend, sigrok logic analyzer, serial capture, scope over LAN, the real power stage.

- The kernel `usbtmc` driver truncates DS1000Z replies to 52 bytes; the tool uses raw
  USB (pyvisa-py) for `resource = "usb"`. `/dev/usbtmcN` works for text queries only.
- DS1000Z firmware 00.04.04 has no `FRDelay`/`RFDelay`; use `scope capture --delay`.
- `cubemx generate --apply` without `--only` still reverts four pre-existing hand
  edits (FreeRTOS heap/hooks, UCPD/tracer IRQ priorities, custom sources in
  `cmake/stm32cubemx/CMakeLists.txt`); the dry-run report lists them.
- On the bench without a power board the firmware auto-starts, then faults on
  `SW_VIN_RANGE` (floating ADC); `regulator clear-fault` returns it to IDLE.
