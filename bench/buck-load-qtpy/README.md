# buck-load-qtpy: the 20 V buck as a power-target load (QT Py RP2040)

This firmware turns the buck converter into a power-target load, commanded from the laptop over
the QT Py's USB console (`cmd ...` at 20 Hz; `bu load run`). The I2C link to the PD sink was
dropped on 2026-10-06: this QT Py's STEMMA pads cannot drive the bus.

- Frames are defined in [`../load-link`](../load-link); the control law is in
  [`../load-control`](../load-control). Both are unit-tested on the host:
  `cargo test --target x86_64-unknown-linux-gnu` in each directory.
- Status: the `power-stage` build runs on the bench (2026-10-07): 0.5, 2 and 3 W into the
  11 Ω ballast at a 15 V contract, and a 0.5 → 2 → 3 W staircase under an EPR contract
  (`../results/2026-10-07`). The buck needs 12–15 V on VIN for its gate drive.

## Pins and gating

`src/board.rs` maps the QT Py to the buck. The pin numbers come from the QT Py port of the buck
firmware (`_20v_Buck_Converter`, branch `electronic_load`); the QT Py is wired straight to the
buck's controller footprint.

- **Every build drives every gate pin from its first instruction:** DISABLE high, PWM low, fan
  off. The TI drivers enable themselves when DISABLE floats, so leaving the pins alone is not
  safe (`board.rs` explains the reset hazard).
- **The default build never releases them.** It runs the ADC telemetry and the USB console,
  and reports `state=no_power_stage`.
- **`--features power-stage` switches phase 2** (PWM GPIO6, DISABLE GPIO3) at 438.6 kHz, the
  port's frequency. That's the phase the port ran below 5 A. Phases 1 and 3 stay parked.

## Control

The control law runs every 1 ms:

1. Allowed power = min(host target, 7 W ballast limit, 0.8 · V_in · I_contract).
2. V\* = √(P · 10 Ω), slewed at +5 V/s and −2 V/s.
3. Duty = V\*/V_in plus an integral trim on the V_out error.

The −2 V/s step-down is far slower than the 10 Ω × 8.6 mF discharge, so the synchronous buck
never pushes energy back into VBUS.

Faults latch with all DISABLE lines high:

| Fault | Trigger |
|---|---|
| watchdog | no command for 250 ms |
| vin_sag | V_in below the host's `vin_min_mv` for more than 5 ms |
| vbus_lost | V_in below 4 V |
| input_over_current | estimated I_in more than 15 % over the contract for more than 20 ms |
| output_over_current | I_out above 1.5 A |
| output_over_voltage | V_out above 8.4 V + 1.5 V |

A command with `clear=1` and `arm=0` releases a latched fault (`bu load run --clear`).

```sh
tools/bringup/bu load run --p 500,2000,3000 --seconds 4 --limit-ma 500 --vin-min-mv 11500
```

holds each target for 4 s without disarming in between, then disarms; the `TEL` frames land in
`bringup_out/`.

## Flash

```sh
tools/bringup/bu load flash                  # from the repo root; add --power-stage when approved
```

It builds the firmware, sends `bootsel` on the QT Py's USB console, writes the UF2 to the RPI-RP2
drive, and waits for the console to come back.
- The firmware refuses BOOTSEL unless the higher buck tap reads < 4.5 V and the lower < 0.3 V,
  so VBUS must be off and +OUT discharged.
- The pico-sdk port firmware takes the 1200-baud touch instead.
- Without the console, hold BOOT and tap RESET.

`elf2uf2-rs` rejects ELFs from Rust 1.99's linker (OS/ABI = GNU); `bu load flash` writes the UF2
itself.

## Calibration constants

All of these are in `board.rs`, taken from the QT Py port (tuned on this board):

- **Voltage taps:** 118.5 counts/V on the 12-bit ADC. The 3.3k/330 dividers give 11.0 nominal.
- **Current sense:** 36.5 counts/A. The zero is measured at run time while the driver is
  disabled; about 20 counts on the bench.

Check VIN and +OUT against a meter. The current sense is coarse below 1 A.
