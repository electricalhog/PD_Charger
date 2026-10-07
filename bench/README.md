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

## The bench as built (2026-10-05)

The diagram above was the plan. The bench on the laptop differs:

- **Source:** the G474 drives the PD_Charger power board. There is no shield on the G474.
  - A 24 V bench supply feeds the power board.
  - The power board's output goes straight to the buck's HV input (VIN), not through any
    Type-C VBUS.
- **CC and VCONN:** the G474's CC1/CC2 (PB6/PB4) are wired to the SRC1M1 on the G431, with
  the NPN→PNP VCONN switches on PA7/PB5 (active high, `pd_bench_config.h`). The shield's power
  path is unused, so the sink's VBUS sense (PA0) reads 0 V.
- **Load link:** the QT Py's STEMMA QT (GPIO22 SDA / GPIO23 SCL) goes to the G431's I2C1
  (PB9/PB8), on the same bus as the TCPP02. The rotary encoder that used to sit on that port
  is removed.
- **QT Py:** wired straight to the buck's controller footprint. The Datalore-IP-rp2040 adapter
  was never built. Pins follow the QT Py port of the buck firmware; see
  `buck-load-qtpy/src/board.rs`.
- **Direction:** the old electronic-load setup ran the converter as a boost, with its load
  resistor (0.332 Ω per that firmware) on the HV side. On this bench it runs as a buck, HV to
  LV, taking power from VBUS. +OUT (the LV side, with the 8400 µF bank) carries an 11 Ω / 10 W
  ballast.
- **Scope:** CH2 = TP3, CH3 = TP4, CH4 = TP2 on the power board. TP2 is V_out and TP3 the
  buck-leg switch node, per the 2026-09-27/28 run notes in `regulator_config.h`. CH1 is free.

**Things found on the hardware:**

- **Buck gate drivers enable when not driven.** DISABLE is pulled low inside the TI drivers,
  and the RP2040 resets with every pad pulled down. Whenever the QT Py is not driving its pins
  and VIN is high enough for the 12 V gate rail, all three low-side FETs are on and +OUT is
  shorted through 1 µH per phase. The firmware therefore:
  - drives DISABLE high from its first instruction;
  - refuses BOOTSEL unless the higher tap reads < 4.5 V and the lower < 0.3 V.
- **VIN never reads 0 V.** The QT Py's 3V3 back-feeds VIN to about 2.65 V through the buck's
  3V3 regulator (IC7). With the G474's output switch closed, this shows on TP2.
- **The buck needs 12–15 V on VIN.** Its 12 V gate rail comes from VIN (IC8, about 11.9 V), so
  it cannot switch at a 5 V or 9 V contract. Load runs use the 15 V contract (G474 built with
  `-DPD_BENCH_15V=ON`; request 9 V first, or enter EPR mode).
- **TCPP02 ack register.** It echoes the control bits (Normal = 0x10), not the bit-reversed
  codes in `tcpp0203.h`. The sink's check is fixed; expect `EVT tcpp02 ok ack=0x18`.
- **A wedged I2C bus froze the sink, PD included.** embassy-stm32's I2C busy-waits for a free
  bus without yielding (1 s default). The sink now:
  - caps that wait at 2 ms;
  - skips I2C while I2C1 reads BUSY, with `EVT i2c stuck busy` and `EVT i2c free`;
  - retries the TCPP02 setup until it takes.
- **Load link not working yet.**
  - The QT Py's I2C1 target never ACKs 0x55. `bu load sniff`/`edges`/`glitch` show a clean
    100 kHz bus, and the block flags itself addressed (STOP_DET with STOP_DET_IFADDRESSED).
  - Changes so far: pad drive raised from embassy's 2 mA to 12 mA; pico-sdk's 100 kHz timing
    registers; general call off.
  - Open; see `bu load sartest`.

**Load link moved to USB (2026-10-06).** The I2C link is dropped and the STEMMA cable is
unplugged.
- The QT Py takes `cmd seq= arm= clear= p_mw= limit_ma= contract_mv= vin_min_mv=` on its USB
  console and answers with `TEL ...`.
- The sink no longer talks to the load: no `load` commands, no load fields in STATUS.
- `bu load run --p MW[,MW…] --seconds S [--limit-ma --contract-mv --vin-min-mv --clear]` is the
  laptop side (2026-10-07): `cmd` at 20 Hz, steps without disarming, disarms at the end.
- `bu bench status/sweep` still call the removed sink `load` commands and fail until they move
  to `bu load run`.

**QT Py tools.** The load firmware has its own USB console. `bu load status | run | bootsel |
flash [--power-stage]` use it. The I2C diagnostics (`pins | sniff | edges | glitch | i2c |
i2cpoll | sartest | sarscan`) went with the I2C link; the firmware answers them with `ERR`.
- `bu load flash` builds the firmware, asks it for BOOTSEL, writes the UF2 to the RPI-RP2
  drive, and waits for the console to come back. The pico-sdk port firmware takes the 1200-baud
  touch instead.
- The bench Rust crates pin Rust 1.99.0 (`rust-toolchain.toml`).

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
