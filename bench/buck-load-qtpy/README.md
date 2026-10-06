# buck-load-qtpy: the 20 V buck as a power-target load (QT Py RP2040)

This firmware turns the buck converter into a power-target load, commanded by the bench PD sink
over I2C on the QT Py's STEMMA QT port: I2C1, SDA1 = GPIO22, SCL1 = GPIO23, target address 0x55.

- Frames are defined in [`../load-link`](../load-link); the control law is in
  [`../load-control`](../load-control). Both are unit-tested on the host:
  `cargo test --target x86_64-unknown-linux-gnu` in each directory.
- Status: builds both ways (13.4 kB flash, no warnings). **Not yet run on hardware.**

## Power-stage gating

`src/board.rs` maps the QT Py's pins to the buck:

- **Analog pins:** assumed to follow the buck sketch's labels (A0 = I_SENSE, A1 = V_in,
  A2 = V_out, A3 = NTC).
- **Seven gate and fan pins:** placeholders, because nothing in the repos says how they are wired.

So the default build **never touches a gate pin**. It runs the I2C link and ADC telemetry and
reports `state=no_power_stage`. After checking every line of the table in `board.rs` against
your wiring:

```sh
cargo build --release --features power-stage
```

The power stage runs one phase (the 10 Ω ballast never needs more than about 0.85 A). Phases 2
and 3 are parked with PWM low and DISABLE high. PWM runs at 250 kHz (125 MHz / 500).

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
| vin_sag | V_in below 90 % of the contract for more than 5 ms |
| vbus_lost | V_in below 4 V |
| input_over_current | estimated I_in more than 15 % over the contract for more than 20 ms |
| output_over_current | I_out above 1.5 A |
| output_over_voltage | V_out above 8.4 V + 1.5 V |

`load clear` from the sink, sent with arm low, releases a latched fault.

## Flash

Hold BOOT, tap RESET; the QT Py mounts as `RPI-RP2`. Then:

```sh
cargo install elf2uf2-rs          # once
cargo run --release               # or: --features power-stage
```

## Calibration constants

All of these are in `board.rs`:

- **V divider 9.78:** from the sketch's 31.7 counts/V on a 10-bit, 3.3 V ADC.
- **Current sense:** 31.5 mV/A with a 69.3 mV offset (the sketch's 2.2 A).
- **ADC reference:** 3.3 V.

Check V_in and V_out against a meter. The current sense is coarse below 1 A and is used only for
the over-current trip; power comes from V_out²/R.
