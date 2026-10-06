# Handoff: PD_Charger EPR bench, cloud session → local session on the bench laptop

**Repo / branch:** `electricalhog/PD_Charger`, branch `claude/usb-c-epr-testing-9bt5lo`. That is
`agentic-bringup-tools` plus a merge of `jonah-bench` plus this work. Everything is pushed; see
`git log agentic-bringup-tools..HEAD`. No PR exists; the user reviews all work and asked for none.

**Why move:** the next steps need the three boards on the laptop's USB ports. The cloud session
could build and unit-test everything but never touched hardware. **Nothing on this branch has run
on hardware.**

## What the next session should do

Bring the three-board bench up on the laptop, following the stage gates in `bench/README.md`
("Bring-up order"):

1. **Local USB setup.**
   - Install `tools/bringup/udev/99-bringup.rules` (it has the install line). It covers ST-LINK
     SWD/VCP, RP2040 BOOTSEL and the Rigol.
   - Copy `tools/bringup/bringup.local.toml.example` to `bringup.local.toml`.
   - Run `tools/bringup/bu doctor`: `bench_devices` lists the attached ST-LINK serials. Set
     `[probe].serial` to the G474's and `[sink].stlink_serial` to the G431's.
   - The `rust` check needs targets `thumbv7em-none-eabihf` and `thumbv6m-none-eabi`, plus
     `elf2uf2-rs`.
2. **Flash.**
   - G474: PD build (CMake default `PD_VBUS_PATH_CHARGER=ON`): `bu build && bu flash`.
   - G431 sink: `bench/pd-sink-g431/README.md`.
   - QT Py: `bench/buck-load-qtpy/README.md`, default build **without** `power-stage`.
3. **Work the stages in `bench/README.md`.** Stop and report on any surprise; that rule is in the
   hw-bringup skill.

## Where everything is documented (don't re-derive)

| Topic | Path |
|---|---|
| G474 PD/EPR work: assumptions, bugs fixed, the ST library upgrade, stage plan, stability work | `plans/epr-bringup.md` |
| Three-board wiring, limits, bring-up order, interlock | `bench/README.md` |
| Sink protocol and commands | `bench/pd-sink-g431/README.md` |
| Load firmware, pin gating, faults | `bench/buck-load-qtpy/README.md`, `bench/buck-load-qtpy/src/board.rs` |
| Why the load is controlled through V_out | `bench/LOAD_CONTROL.md` |
| Source PDO and EPR limits | `STM32CubeIDE/final/Core/Inc/pd_bench_config.h` |
| `bu` commands (`pd status`, `sink`, `bench status/sweep`) | `tools/bringup/TOOLS.md` |

## Open items that need the user (not decided in the cloud session)

1. **ST USB-PD core v5.4.1 is not vendored.** The cloud sandbox refused to add ST's binary. The
   steps are in `plans/epr-bringup.md` ("Library upgrade").
   - Until it's added, all `#if defined(USBPDCORE_EPR)` code in the G474 firmware compiles out.
   - That code has never been compiled against v5. Expect fallout around the GotoMin/Ping
     request functions in `USBPD/Target/usbpd_dpm_user.c`.
2. **The QT Py-to-buck pin map is a guess.** Only the analog pins are inferred (from the buck
   sketch's labels); the 7 gate/fan pins in `bench/buck-load-qtpy/src/board.rs` are
   placeholders. Confirm them with the user before building with `--features power-stage`.
   - Also unknown: the gate-driver part (does DISABLE hold both FETs off?) and the true ADC/divider
     scales. Calibrate against a meter.
3. **Regulator up-slew** (`SETPOINT_SLEW_MV_PER_CYCLE` = 20 V/s) blocks 15 V and 20 V PDOs, by
   a compile-time check against tPSTransition. Raising it changes the `jonah-bench` loop
   dynamics: the user decides, and it gets validated on a dummy load first.
4. **This shield setup is SPR only.** The SRC1M1 VBUS divider reads 19.8 V at full scale, and the
   TCPP02 is an SPR part. A 28 V EPR PDO also needs the G474 output-switch FET and capacitor
   ratings checked, plus VCONN hardware: the switches go on PA7/PB5, build with `-DPD_VCONN=ON`.

## Safety notes the next session must keep

- **Never run `jonah-bench`'s own firmware (or the `PD_VBUS_PATH_CHARGER=OFF` build) with
  anything on the Type-C receptacle.** Its default task starts the regulator at 28 V on boot.
- In the PD build the debug mailbox refuses `regulator start/set-voltage` (`PD_OWNS_VBUS`). Don't
  work around that with `mem write`.
- The load only arms with a contract. Every renegotiation stops it first. `bu bench sweep` always
  ends with `load off`.
- The QT Py firmware has no USB device. To reflash: hold BOOT, tap RESET.

## Bench facts the cloud session established

- G474 source PDOs: 5 V and 9 V at 500 mA. EPR Mode Capable is advertised only with the v5
  library.
- Sink:
  - TCPP02 at 0x34 and the load at 0x55 share I2C1 (PB8/PB9) at 100 kHz.
  - The sink's I2C must stay async (DMA), because usbpd sends GoodCRC in software.
  - Expect `EVT tcpp02 ok ack=0x20` at boot.
- QT Py STEMMA QT is I2C (GPIO22/23), not UART.
- Load power is capped at min(target, 7 W, 0.8·V·I_contract): 2.0 W at 5 V and 3.6 W at 9 V.

## Suggested skills

- **hw-bringup** (project skill, `.claude/skills/hw-bringup/SKILL.md`): safety rules, `bu`
  usage, and section 5c on PD.
- **anthropic-skills:diagnose:** when a stage misbehaves on hardware.
- **code-review:** before the user merges anything from this branch.

## User preferences

Concise and practical. State assumptions, include units, cite sources, use American English.
The user reviews all work; ask before flashing power-stage changes or writing control variables.
