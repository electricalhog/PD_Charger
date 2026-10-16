# Three-board PD bench

As built (updated 2026-10-15):

```
 24 V supply ─> PD_Charger power board (G474, SWD: bu pd / bu regulator)
                   │ VBUS (TP2)
                   ├──> 25 Ω / 25 W on a heat sink, fixed (since 2026-10-15; scope CH1)
                   └──> buck load VIN ── buck VOUT (+OUT, 8.4 mF) ──> 11 Ω / 10 W ballast
                         QT Py RP2040 on the buck (bench/buck-load-qtpy), USB console: bu load
 G474 CC1/CC2 + VCONN ──Apple 240 W cable──> SRC1M1 receptacle on the NUCLEO-G431 sink
                                             (bench/pd-sink-g431, VCP: bu sink); CC only, no VBUS
```

| Directory | What |
|---|---|
| `pd-sink-g431/` | Sink firmware (Rust/embassy + usbpd): PD policy, EPR, TCPP02. It carries CC only. |
| `buck-load-qtpy/` | Electronic-load firmware (Rust/embassy-rp): a power-target loop on the buck, commanded over its USB console. |
| `load-link/` | The load's command and telemetry definitions (host-tested). |
| `load-control/` | The load's control law: power → V_out, limits, faults (host-tested). |
| `LOAD_CONTROL.md` | Why the load is controlled this way (written before it was built; see "The electronic load"). |

## The bench as built

This replaced the original plan (the sink's receptacle feeding the load) on 2026-10-05.

- **Source:** the G474 drives the PD_Charger power board. There is no shield on the G474.
  - A 24 V bench supply feeds the power board.
  - The power board's output (VBUS, TP2) goes straight to the loads: the buck's HV input (VIN)
    and, since 2026-10-15, the fixed 25 Ω. The Type-C cable carries no VBUS: the sink's VBUS
    sense reads 0 V.
- **CC and VCONN:**
  - The G474's CC1/CC2 (PB6/PB4) run over the Apple 240 W cable to the SRC1M1 receptacle on the
    G431.
  - The VCONN switches (NPN→PNP) are on PA7/PB5, active high (`pd_bench_config.h`).
  - The shield's power path is unused, so the sink's VBUS sense (PA0) reads 0 V.
- **Electronic load:** the QT Py RP2040 on the 20 V buck converter; see "The electronic load".
- **Scope:**
  - CH1: the 25 Ω resistor (the probe connection is unreliable).
  - CH2 = TP3: SW1, the Q1/Q2 node.
  - CH3 = TP4: SW2, the Q3/Q4 node.
  - CH4 = TP2: VBUS.

**Things found on the hardware:**

- **Output switch gate drive (rework 2026-09-28, R27 changed 2026-10-12).** Q5/Q6 are
  SiR5607DP P-FETs (-60 V, Vgs +-20 V), back to back with a common source. The trace from
  the Q9 pull-down to their gates is cut and replaced by R27 (10 kohm, 0603; 5.1 kohm until
  2026-10-12), and a 13 V zener (1/8 W) sits across the 51 kohm gate pull-up R13.
  - Switch on: Vgs = -0.84 x V_out up to about 15.5 V (-4.2 V at 5 V), then clamped at -13 V.
  - R27 dissipates 23 mW at 28 V, 53 mW at 36 V and 123 mW at 48 V, which is about a
    quarter over an 0603's 0.1 W: fine for test runs, use a 1206 for sustained 48 V. The
    zener takes 42 mW at 48 V.
  - Turn-off discharges the gates through 51 kohm only: about 0.5 ms per time constant.
- **Buck gate drivers enable when not driven.** DISABLE is pulled low inside the TI drivers,
  and the RP2040 resets with every pad pulled down. Whenever the QT Py is not driving its pins
  and VIN is high enough for the 12 V gate rail, all three low-side FETs are on and +OUT is
  shorted through 1 µH per phase. The firmware therefore:
  - drives DISABLE high from its first instruction;
  - refuses BOOTSEL unless the higher tap reads < 4.5 V and the lower < 0.3 V.
- **VIN never reads 0 V.** The QT Py's 3V3 back-feeds VIN to about 2.65 V through the buck's
  3V3 regulator (IC7). With the G474's output switch closed, this shows on TP2.
- **The buck needs 12–15 V on VIN.** Its 12 V gate rail comes from VIN (IC8, about 11.9 V), so
  it cannot switch at a 5 V or 9 V contract. Load runs use the 15 V or 20 V contract (G474 built
  with `-DPD_BENCH_15V=ON`, plus `-DPD_BENCH_20V=ON` for 20 V). In SPR mode step up through 9 V
  and 15 V, or enter EPR mode.
- **The VBUS sense depends on the wiring.** Taking the SRC1M1 shield off the G474 changed the
  loading on PA0 and PA4 (VD_MON, VS_MON), and TP2 read 11 % below the ADC. It was recalibrated
  against a meter on 2026-10-10 (`regulator_config.h`). Redo that whenever this wiring changes.
- **D3, the clamp on VD_MON, leaked (2026-10-12).** Above about 20 V the ADC read low: 2.9 % at
  28 V and 15.3 % at 36 V (meter 42.5 V for an ADC 36.0 V). D3 is disconnected and VD_MON
  recalibrated (`results/2026-10-11/README.md`). D2 on VS_MON is the same part and
  still fitted.
- **TCPP02 ack register.** It echoes the control bits (Normal = 0x10), not the bit-reversed
  codes in `tcpp0203.h`. The sink's check is fixed; expect `EVT tcpp02 ok ack=0x18`.
- **A wedged I2C bus froze the sink, PD included.** embassy-stm32's I2C busy-waits for a free
  bus without yielding (1 s default). The sink now:
  - caps that wait at 2 ms;
  - skips I2C while I2C1 reads BUSY, with `EVT i2c stuck busy` and `EVT i2c free`;
  - retries the TCPP02 setup until it takes.

## The electronic load

**Hardware.**
- The 20 V three-phase buck board. The firmware switches phase 2 only, at 438.6 kHz; phases 1
  and 3 stay parked.
- The board ran as a boost in its old electronic-load life (load resistor on the HV side). Here
  it runs as a buck, from VBUS on VIN into +OUT.
- +OUT carries an 8.4 mF bank and an **11 Ω / 10 W** ballast.
- The QT Py is wired straight to the buck's controller footprint (`buck-load-qtpy/src/board.rs`).
- **12–15 V on VIN is needed to switch.** The gate rail comes from VIN through IC8 (L6983, about
  11.9 V out), so it cannot switch at 5 or 9 V.
- **Voltage taps.** Both read 118.5 counts/V on the 12-bit ADC, so they saturate at about 34.6 V.
  Above that the load cannot see its own VIN (36 and 48 V contracts).
- **Rated 38 V.** IC8 is a 38 V part, and the taps put more than 3.3 V on the RP2040's pins
  above about 34.6 V. The load stays on VBUS at 36 and 48 V by decision (risk accepted on
  2026-10-11). It saw about 42.5 V and 47 V on 2026-10-12.

**Firmware.**
- The `power-stage` build switches; the default build only reports.
- Control every 1 ms:
  - allowed power = min(host target, 7 W, 0.8 · V_in · I_contract);
  - V_out* = √(P · 10 Ω), slewed at +5 / −2 V/s;
  - duty = V_out*/V_in plus an integral trim, at most 90 %.
- **10 Ω, not 11.** The parameters (`Params::BENCH_10R_10W` in `load-control`) still assume a
  10 Ω ballast. The `pout_mw` it reports is for 10 Ω; the power into the real 11 Ω is 10/11 of
  that, and the 7 W cap is about 6.4 W real.
- Faults latch with every DISABLE line high:
  - no command for 250 ms;
  - V_in under the host's minimum, or under 4 V;
  - estimated input current over the contract;
  - I_out over 1.5 A;
  - V_out over 8.4 + 1.5 V.

**Link.**
- The QT Py's USB console takes `cmd seq= arm= clear= p_mw= limit_ma= contract_mv= vin_min_mv=`
  and answers with `TEL ...` frames.
- `bu load run --p MW[,MW…] --seconds S [--limit-ma --contract-mv --vin-min-mv --clear]` sends
  commands at 20 Hz, steps between targets without disarming, and disarms at the end.
- The original I2C link to the sink (0x55 on the sink's I2C1) never worked: the QT Py's I2C
  target would not ACK. It was dropped on 2026-10-06, the STEMMA cable unplugged, and the sink's
  `load` commands removed.
- `bu bench status/sweep` still call those removed commands.

**The fixed 25 Ω (since 2026-10-15).**
- A 25 Ω / 25 W resistor on a large heat sink sits directly on VBUS. It is not switched and not
  part of the electronic load, so every contract loads it with V²/25:

  | VBUS | 5 V | 9 V | 15 V | 20 V | 24 V | 28 V | 36 V | 48 V |
  |---|---|---|---|---|---|---|---|---|
  | Power | 1 W | 3.2 W | 9 W | 16 W | 23 W | 31 W | 52 W | 92 W |

- **Heat budget (the user's):** up to 5× rating in short steps, averaging no more than 25 W
  over 10 minutes; 28 V (31 W) is fine continuously.
- **Firmware backstop (bench builds with the 36/48 V PDOs):** a contract above 28 V ends in a
  Hard Reset after 2 s (`PD_BENCH_HIGH_V_MAX_MS`).

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

**Power:**
- power board VBUS (TP2) and GND → buck VIN and the 25 Ω; buck +OUT → the 11 Ω / 10 W ballast;
- the QT Py's STEMMA QT port is unused since the I2C link was dropped (2026-10-06).

## Limits on this setup

- **No VBUS through the sink.** The shield and the TCPP02 carry CC only, so their SPR (20 V
  class) ratings do not limit the contract. The EPR contracts to 48 V on 2026-10-12 ran this way.
  Don't wire VBUS to the shield: its divider and the TCPP02 are not rated for EPR.
- **The electronic load:**
  - it switches only with VIN at 12–15 V or more;
  - it blinds above about 34.6 V;
  - it is over its parts' ratings at 36 and 48 V (see "The electronic load");
  - its power is at most about 6.4 W real into 11 Ω (7 W at the firmware's 10 Ω);
  - it also stays under 0.8 · V · the `--limit-ma` you pass. Pass the PDO current, 3 A since
    2026-10-15.
- **The fixed 25 Ω:** its V²/25 at every contract, under the heat budget above.

## Bring-up order

1. **Flash the sink** (`pd-sink-g431/README.md`).
2. **Flash the load** with VBUS off: `bu load flash`, or `bu load flash --power-stage` once the
   gate lines in `board.rs` are confirmed. The firmware refuses BOOTSEL unless VIN reads < 4.5 V
   and +OUT < 0.3 V.
3. **Check the load's ADC** against a meter: `bu load status` (`vin_mv`, `vout_mv`).
4. **Connect the G474 source** (PD build). `bu sink status` shows a 5 V contract and
   `bu pd status` the source's side.
5. **First load power** at a 15 or 20 V contract (`bu sink req 15000 3000 --until contract`):
   `bu load run --p 500 --seconds 4 --limit-ma 3000 --contract-mv 15000 --vin-min-mv 13500`.
   Scope the switch node and VBUS. Remember the 25 Ω already takes V²/25.
6. Set `[probe].serial` (G474) and `[sink].stlink_serial` (G431) in
   `tools/bringup/bringup.local.toml` first: two ST-LINKs are attached.

## Interlock

The sink-side interlock went with the I2C link. Now:
- **The host.** `bu load run` arms the load only for its run, and disarms at the end, on a fault,
  or on SIGTERM/Ctrl-C. Disarm before every renegotiation.
- **The load itself:** the faults under "The electronic load". Above all, the 250 ms command
  watchdog, so a dead host cannot leave it running.
