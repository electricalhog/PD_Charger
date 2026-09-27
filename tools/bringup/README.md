# bringup — agent tools for closed-loop hardware bring-up

A single CLI (`bu`) that gives an agent (or you) every step of the loop
**.ioc → CubeMX generate → USER CODE edit → build → flash → measure** with JSON
output. Agent-facing usage and safety rules live in
[`.claude/skills/hw-bringup/SKILL.md`](../../.claude/skills/hw-bringup/SKILL.md).

```bash
tools/bringup/bu doctor            # what's installed / connected
tools/bringup/bu --help            # all commands
```

`bu` runs through `uv`, which creates `tools/bringup/.venv` on first use
(pyserial, pyvisa, pyvisa-py, pyusb).

## Setup

| Piece | Needed for | Notes |
|---|---|---|
| STM32CubeMX 6.16 | `cubemx` | autodetected in `/usr/local/STMicroelectronics/...`; must match the `.ioc` version |
| arm-none-eabi-gcc, cmake, ninja | `build` | |
| STM32_Programmer_CLI | `flash`, `probe`, `mem` | autodetected from the CubeIDE install; `backend = "openocd"` also works |
| gdb-multiarch / arm-none-eabi-gdb | `layout` | reads struct layouts from the ELF |
| Rigol DS1054Z | `scope` | LAN recommended: set a static IP on the scope, then `resource = "tcp://<ip>:5555"`. USB needs `udev/99-bringup.rules` |
| sigrok-cli | `la` | `sudo apt install sigrok-cli`; set `[logic].driver` for your analyzer |

Machine-specific settings go in `tools/bringup/bringup.local.toml` (gitignored):

```toml
[scope]
resource = "tcp://192.168.1.50:5555"

[probe]
serial = "002F00343234510836303532"   # only needed with several ST-LINKs attached
```

## Design notes

- **CubeMX runs in a temp copy.** `cubemx generate` is a dry run by default; it
  reports which files changed, whether any USER CODE was lost, and every change
  outside USER CODE. `--apply` copies the result back, and refuses if user code
  would be lost.
- **CubeMX does not validate `.ioc` values**; it pastes them into the C code
  as-is. `ioc set` keeps the `IPParameters`/`GPIOParameters` indexes in sync
  (CubeMX ignores parameters missing from those lists), and the build catches
  bad values.
- **Live memory access uses hot-plug attach**, so the core keeps running while
  `mem read` samples `debug_log` or other globals by ELF symbol.
- Line endings are preserved: CubeMX writes CRLF in some files and LF in others.

## Status

| Area | Verified on this machine |
|---|---|
| ioc, usercode, build, sym, layout, cubemx (dry run) | yes, against `STM32CubeIDE/final` |
| flash, probe, mem | yes, on the NUCLEO-G474RE (flash + verify, live reads by symbol) |
| regulator status / stop / bench-pwm | status + stop on the Nucleo; clear-fault and bench-pwm need the firmware start issue fixed (see below) |
| scope | yes, DS1104Z over USB: state/config, measure, capture + edge timing, screenshot, single-shot trigger across a core reset |
| la | **not yet run**: sigrok-cli isn't installed |

Offline tests: `cd tools/bringup && uv run pytest`.
