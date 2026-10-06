# Three-board PD bench

```
 G474 PD_Charger (source) ──Type-C cable──> SRC1M1 shield receptacle ──VBUS/GND──> buck VIN
   SWD: bu pd/regulator                       TCPP02 (CC protection)               buck VOUT ──> 10 Ω / 10 W
                                              │ CC1/CC2, I2C, ENABLE, FLGn, VSENSE
                                         NUCLEO-G431RB  (bench/pd-sink-g431)
                                              │ I2C1 (PB8/PB9), shared with the TCPP02
                                         QT Py RP2040 on the buck  (bench/buck-load-qtpy)
   host ──VCP──> G431:  bu sink …, bu bench …
```

| Directory | What |
|---|---|
| `pd-sink-g431/` | Sink firmware (Rust/embassy + usbpd). PD policy, TCPP02, VBUS sense, I2C controller for the load, interlock. |
| `buck-load-qtpy/` | Load firmware (Rust/embassy-rp). I2C target at 0x55, 1 kHz power-target loop. |
| `load-link/` | I2C frame definitions shared by both firmwares (host-tested). |
| `load-control/` | The load's control law: power → V_out, limits, faults (host-tested). |
| `LOAD_CONTROL.md` | Why the load is controlled this way. |

## Wiring

**G431 ↔ SRC1M1 shield.** Plugged onto the Arduino headers these connect by themselves.
Only the TCPP02 and the receptacle are used; leave the shield's power input unconnected.

| Shield signal | G431 | Arduino | Morpho |
|---|---|---|---|
| TCPP02 CC1 (MCU side) | PB6 | D10 = CN5-3 | CN10-17 |
| TCPP02 CC2 (MCU side) | PB4 | D5 = CN9-6 | CN10-27 |
| I2C SCL | PB8 | D15 = CN5-10 | CN10-3 |
| I2C SDA | PB9 | D14 = CN5-9 | CN10-5 |
| TCPP02 ENABLE | PC8 | — | CN10-2 |
| TCPP02 FLGn | PC5 | D0 = CN9-1 | CN10-6 |
| VBUS sense (÷6) | PA0 | A0 = CN8-1 | CN7-28 |
| 3V3 / 5V / GND | | CN6-4 / CN6-5 / CN6-6 | |

Pin sources: `STM32CubeIDE/final/TCPP/Target/src1m1_conf.h`. Arduino names come from the
stm32duino NUCLEO_G431RB/G474RE variants. Morpho positions follow the Nucleo-64 layout; check
them against UM2505.

**QT Py STEMMA QT ↔ G431** (4-pin JST-SH; I2C1 SDA1 = GPIO22, SCL1 = GPIO23 on the QT Py):

| Wire | G431 |
|---|---|
| black, GND | GND |
| blue, SDA | PB9 (same bus as the TCPP02) |
| yellow, SCL | PB8 |
| red, 3V3 | **not connected** (each board has its own regulator) |

Both ends enable their internal pull-ups (~40–50 kΩ). If the shield has no I2C pull-ups, add
4.7 kΩ from SDA and SCL to 3V3 on the G431 side.

**Power:**
- receptacle VBUS and GND → buck VIN; buck VOUT → 10 Ω / 10 W resistor;
- route the power return through the Type-C GND, not through the STEMMA ground wire.

## Limits on this setup

- **SPR only.**
  - The shield's VBUS divider puts 19.8 V at full scale on PA0, and 28 V would drive PA0
    to 4.7 V, above VDDA. The sink reports `vbus_sat=1` and warns.
  - The TCPP02 is an SPR (20 V class) part.
  - Don't run 28 V EPR through this shield.
  - EPR mode at SPR voltages is fine (`bu bench sweep --epr <W>`).
- **Load power** is the lowest of:
  - the host target;
  - 7 W (10 W ballast derated to 70 %);
  - 0.8 × V_contract × I_contract: 2.0 W at 5 V and 3.6 W at 9 V for the G474's 500 mA PDOs.

## Bring-up order

1. **Flash the sink** (`pd-sink-g431/README.md`). Flash the **load without** `power-stage`.
   With no source: `bu sink status` shows `tcpp_ok=1`, and `bu sink load status` shows
   `online=1 state=no_power_stage`.
2. **Check the load's ADC mapping.** With the Type-C cable unplugged from the source, put a
   current-limited bench supply (e.g. 9.0 V) on the buck VIN and confirm
   `vin_mv` in `bu sink load status` within a few %. This checks A1 and the 9.78 divider in
   `buck-load-qtpy/src/board.rs`.
3. **Connect the G474 source** (PD build). `bu sink status` shows a 5 V contract;
   `bu bench status` shows all three boards.
4. **Run the PD sequence with no power:**
   `bu bench sweep --mv 5000,9000 --mw 0 --allow-no-power-stage --no-swd`.
   It checks renegotiation, `hold`, and the load link.
5. **Confirm every gate/fan line** in `board.rs` against your QT Py wiring, then build the load
   with `--features power-stage`.
6. **First power at 5 V and a small target:**
   `bu sink req 5000 500 --until contract`, then `bu sink load p 250` (V_out ≈ 1.6 V). Scope the
   switch node and V_out, then `bu sink load off`.
7. **Sweep:** `bu bench sweep --mv 5000,9000 --mw 250,500,1000,2000,3000 --ma 500`. The CSV lands
   in `bringup_out/`. Set `[probe].serial` (G474) and `[sink].stlink_serial` (G431) in
   `tools/bringup/bringup.local.toml` first, because two ST-LINKs are attached.

## Interlock (sink firmware)

The load is armed only when all of these hold:
- an explicit contract exists;
- no renegotiation is in progress (`hold`);
- the TCPP02 is in Normal mode with no fault flags and FLGn high;
- the host set a target with `load p`.

Every `req`, `epr`, `eprexit` and `getcaps` first holds the load and waits (≤ 500 ms) until it
reports it is not running; otherwise the command is dropped. New source capabilities and Hard
Resets also set `hold`; the next contract clears it.

A load fault clears the host target. `load clear` resets the fault, then `load p` re-arms.

The load enforces its own limits independently:
- it faults (gate drivers off) after 250 ms without a command frame;
- it faults on V_in under 90 % of the contract or under 4 V;
- it faults on the estimated input current over the contract, and on output over-current or
  over-voltage.
