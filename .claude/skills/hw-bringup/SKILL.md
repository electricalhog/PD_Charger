---
name: hw-bringup
description: Closed-loop STM32 hardware bring-up for the PD_Charger firmware - edit CubeMX .ioc parameters, regenerate code headlessly, edit USER CODE sections, build, flash the NUCLEO-G474RE, read/write live RAM over SWD, and validate on a Rigol DS1054Z scope or a sigrok logic analyzer. Use when asked to configure peripherals, regenerate CubeMX code, flash the board, probe signals, or verify firmware behaviour on hardware.
---

# Hardware bring-up tools

Every tool is one CLI, `tools/bringup/bu`, run from the repo root. Each call prints
**one JSON object**; `"ok": false` (exit 1) always carries an `"error"` string.
Large outputs (captures, logs, patches, screenshots) go to `bringup_out/` and the
JSON gives the path — Read PNG screenshots to see them, open CSVs only if the
`summary` isn't enough.

Start every session with `tools/bringup/bu doctor` (add `--offline` when no
hardware is attached) and stop if a check you need is failing.

## Safety rules (power electronics — read first)

This firmware drives a 4-switch buck-boost via HRTIM. A bad dead-time, polarity,
or duty can short a half-bridge.

- **Ask the user before** flashing firmware that changes HRTIM, COMP/DAC fault,
  or gate-drive config when the power stage has input power, and before any
  `mem write` to a control variable (setpoints, duty, PID gains, enables).
- Before the first flash of a changed PWM config, verify gate signals with the
  scope/analyzer **with the power stage unpowered** (dead-time, polarity,
  complementary outputs never both high).
- Never `--force` a CubeMX apply without the user's OK.
- If a measurement is surprising, stop and report; don't iterate blindly on hardware.
- A FAULT is information, not an obstacle. Before `bu regulator clear-fault`,
  run `bu regulator status`, work out **why** it tripped, and fix the cause
  (config, code, setpoint) first. Never loop clear-fault → start → trip.
  An `INPUT_OVERCURRENT` result means stop and tell the user.

### What the hardware protects on its own (ADM1270 on the input path)
The analog input protection acts regardless of what the firmware does:

| Condition | ADM1270 response |
|---|---|
| Input current > 24.5 A for 2 ms, or > 50 A (~2 µs) | FET off, **latched**; `IS_GOOD` low → HRTIM FLT2 → firmware FAULT |
| Charging into VS near 0 V | current folds back to ~5 A |
| VIN > 57.7 V | FET off while OV persists; `VS_GOOD` low → FLT1 |
| VS < 4.46 V (falling) / > 4.60 V (rising) | `VS_GOOD` (PWRGD) low / high |

The HRTIM hardware forces all gate outputs off on either line within
nanoseconds, independent of the CPU. It does **not** protect against
shoot-through inside the bridge below the current limit, bad dead-time, or
output-side over-voltage: those still depend on correct firmware.

## The loop

```
ioc set  ->  cubemx generate (dry-run)  ->  review report  ->  cubemx generate --apply
         ->  usercode set (only inside USER CODE)  ->  build  ->  flash
         ->  scope / la / mem read  ->  compare to expectation  ->  repeat
```

### 1. Configure: `.ioc`
```
bu ioc search 'HRTIM1.*DeadTime'          # regex over keys+values
bu ioc get FREERTOS.configTOTAL_HEAP_SIZE
bu ioc ips                                # enabled peripherals
bu ioc pins PA8                           # pin config
bu ioc set KEY=VALUE [KEY=VALUE ...] [--dry-run]
bu ioc delete KEY
bu ioc diff [--ref HEAD]                  # semantic diff vs git
```
Keys are unescaped (`ADC1.Rank-2#ChannelRegularConversion`). `set` maintains
`IPParameters`/`GPIOParameters` automatically. **CubeMX does not validate
values** — a typo is pasted into the C code, so the build is the validator.
Enabling a brand-new peripheral or middleware is multi-key and error-prone;
prefer asking the user to do it in the CubeMX GUI.

### 2. Regenerate: CubeMX (~20-50 s)
```
bu cubemx generate            # DRY RUN in a temp copy; nothing in the tree changes
bu cubemx generate --apply    # copy generated files into the project
```
Read the report before applying:
- `user_code_lost` must be empty (apply is refused otherwise).
- `generated_diffs` = changes **outside** USER CODE. They should correspond to
  what you changed in the .ioc. Anything else is a hand edit to generated code
  that regeneration will silently revert.
- Full patch: `full_patch` path.

**Known drift (as of this branch):** regenerating the unchanged .ioc reverts
hand edits in `FreeRTOSConfig.h` (heap 20000->7000, malloc-failed hook, stack
overflow check), `main.c` (defaultTask stack 512->128), UCPD/tracer IRQ
priorities in `usbpd_devices_conf.h`, `tracer_emb_conf.h`, `tracer_emb_hw.c`,
and the custom sources list in `cmake/stm32cubemx/CMakeLists.txt`. Do not
`--apply` until the user has decided how to fix this (move settings into the
.ioc / USER CODE / root CMakeLists.txt).

### 3. Edit code: USER CODE sections only
```
bu usercode scan                                  # files with sections, which are non-empty
bu usercode list Core/Src/main.c                  # section ids + line numbers
bu usercode get  Core/Src/main.c "ADC1_Init 2"
bu usercode set  Core/Src/main.c "ADC1_Init 2" --from snippet.c   # or --text / stdin
```
Paths are relative to the project dir (`STM32CubeIDE/final`). Line endings are
preserved. Hand-written modules (`regulator.c`, `pid_controller.c`, ...) are not
generated — edit those normally.

### 4. Build
```
bu build [--clean] [--reconfigure]
```
Returns `errors`/`warnings` as `{file,line,col,msg}`, FLASH/RAM usage, ELF sha1.
Uses the CubeMX-generated CMake project (`build/Debug/final.elf`).

### 5. Flash and inspect the target
```
bu probe list
bu flash                         # built ELF, verify + reset
bu probe reset [--hard]
bu sym 'regex'                   # ELF symbols -> addr,size
bu layout debug_log              # struct layout w/ offsets (gdb ptype /o), no target needed
bu mem read debug_log+12288 --type u32 --count 2    # live, core keeps running
bu mem read debug_log --save     # whole struct -> .bin (decode with `layout`)
bu mem write some_var 1.5 --type f32                # ASK FIRST (see safety)
bu mem write 0x40016B98 0x1                         # peripheral registers work too (here: HRTIM ODISR, TA1 off)
```
Writes to write-only/self-clearing peripheral registers (HRTIM OENR/ODISR,
flag-clear registers) come back `ok: true, verified: false`: the
programmer's read-back check fails although the write happened. Confirm
the effect instead (e.g. `regulator status` outputs, or the scope).
Types: u8 i8 u16 i16 u32 i32 f32 u64 i64 f64. Telemetry on this board is the
`debug_log` ring buffer in RAM (LPUART1 carries the binary UCPD tracer, not text).

### 5b. Regulator state and fault recovery
```
bu regulator status        # state, fault source, VS_GOOD/IS_GOOD/INPUT_EN, HRTIM flags/IRQ/outputs, ADC, diagnosis
bu regulator clear-fault   # firmware: 150 ms ADM1270 cool-down -> INPUT_EN toggle -> verify VS_GOOD/IS_GOOD
                           #   -> input path off, FAULT -> IDLE. Result OK / INPUT_NOT_GOOD / INPUT_OVERCURRENT
bu regulator stop          # outputs off, input/output path off (keeps a latched FAULT)
```
`clear-fault` does **not** restart switching. After an `INPUT_OVERCURRENT`
result the tool refuses to retry without `--force`: only use that after the
user has confirmed the hardware is OK. With no power board attached (or VIN
off) the fault lines float and the firmware sits in FAULT; that's expected.

### 6. Measure: Rigol DS1054Z (reports as DS1104Z)
**Analyse sample data, not pictures.** `scope capture` saves a CSV and returns
per-channel levels, frequency, duty, period jitter and interpolated edge
times; `scope analyze <csv>` re-runs that offline with other thresholds or
delay pairs (no scope needed). Use `screenshot` only to sanity-check the
setup (traces on screen, not clipped, not overlapping).
```
bu scope capture CH2 CH3 --raw --threshold 1.65 --delay CH2:fall,CH3:rise --delay CH3:fall,CH2:rise
bu scope analyze bringup_out/<file>.csv --threshold CH2=1.2 --delay CH2:fall,CH3:rise
```
- `--raw` downloads acquisition memory at the real sample rate (scope is
  stopped): e.g. 2 channels, `acquire --mdepth 600k`, 100 us/div = 600k points
  at 500 MSa/s = ~100 switching periods in ~4 s. Without `--raw` you get the
  1200 screen points (interpolated at fast timebases).
- `--delay A:edge,B:edge` pairs every A edge with the next B edge before
  the following A edge and returns count/mean/min/max/std. Dead-time on
  every cycle is a single command.
- Report which threshold you used: edge timing moves by ~0.3 ns between a
  fixed 1.65 V and the 50 % auto level on these signals.

bu scope idn | state | run | stop | single | force | autoscale | clear
bu scope chan 1 --on --probe 10 --scale 2 --offset -4 --coupling DC [--bwl 20M]
bu scope timebase --scale 1e-6 [--offset 0]
bu scope trigger --source CH1 --slope POS --level 1.6 --sweep SING
bu scope wait --timeout 5                 # arms SINGLE, waits for STOP
bu scope measure CH1 CH2 [--items VPP,FREQuency,PDUTy,RTIMe]
bu scope delay RDELay CH1 CH2             # edge-to-edge timing (dead-time!)
bu scope capture CH1 CH2 [--raw]          # CSV + per-channel min/max/mean/freq/duty
bu scope screenshot                       # PNG; Read it to look at the screen
bu scope scpi ':ACQuire:SRATe?'           # raw escape hatch
```
`null` measurements mean the scope couldn't measure on the current screen
(fix timebase/scale). A `warning` about clipping in `capture` means adjust
scale/offset. Prefer `measure` for numbers; use `screenshot` to sanity-check setup.

Lessons from the first bench session (DS1104Z fw 00.04.04.SP3 over USB):
- **Always screenshot after changing the setup** and look at it. Stacked
  channels at the same offset hide each other; a trace offset off-screen
  gives nonsense numbers. With 4 channels at 2 V/div, offsets +3.5/0/-3.5/-7.5 V
  stack 3.3 V logic without overlap.
- Check `scope state` probe ratios before trusting amplitudes: a probe
  menu at 1x on a 10x probe reads 10x low (and vice versa).
- Use `acquire --type NORM` for edge timing (HRES averages away edges).
  Sample rate halves per enabled channel pair: turn unused channels off
  for 500 MSa/s (2 ch) or 1 GSa/s (1 ch).
- `measure` returns PDUTy/NDUTy in percent (the tool converts the scope's ratio).
- This firmware has no mixed-edge delay items (FRDelay/RFDelay time out);
  RDELay/FDELay need the same edge on both channels. For dead-time use
  `scope capture CH2 CH3 --threshold 1.65` and subtract the `edges` times
  (interpolated crossings, relative to the trigger).
- Overshoot/undershoot of ±1-2 V on 3.3 V logic is ground-lead ringing, and
  a slow edge on one probe inflates edge-to-edge timing: trust a measurement
  only when it agrees with the configured register value, and say which
  edge and threshold was used.
- `resource = "usb"` uses raw USB (pyvisa-py). The kernel usbtmc driver
  truncates DS1000Z replies to 52 bytes, so don't use `/dev/usbtmcN` for
  data.

### 7. Measure: logic analyzer (sigrok)
```
bu la scan
bu la capture --channels D0=HI_A,D1=LO_A --samplerate 24m --time-ms 5 [--trigger HI_A=r]
bu la capture --channels D2=SCL,D3=SDA --samplerate 4m --time-ms 50 -P i2c:scl=SCL:sda=SDA
bu la decode bringup_out/<file>.sr -P uart:rx=D0:baudrate=115200
```
Returns per-channel edge counts, frequency, duty.

### 8. Serial
```
bu serial list
bu serial capture --seconds 3 [--baud 921600 --bytesize 7] [--hex] [--until 'READY']
```

## Configuration
Defaults: `tools/bringup/bringup.toml`. Machine-specific values (scope address,
ST-LINK serial, analyzer driver) go in `tools/bringup/bringup.local.toml`
(gitignored). If `doctor` says the scope is not configured, ask the user for its
IP rather than scanning the network.
