# pd-sink-g431: scriptable USB-PD sink for the bench

Firmware for a NUCLEO-G431RB that acts as the sink in the PD_Charger tests.

- It requests exactly the voltage and current you give it.
- It enters or leaves EPR mode only when told to.
- It reports every PD step as a text line on the ST-LINK virtual COM port.

It is built on [elagil/usbpd](https://github.com/elagil/usbpd) (sink policy engine, SPR + EPR)
and [embassy](https://github.com/embassy-rs/embassy) (UCPD driver). The UCPD glue follows usbpd's
`examples/embassy-stm32-g431cb-epr`. Both dependencies are pinned in `Cargo.toml` to the commits
that example builds against.

Status: builds (`cargo build --release`: 74.7 kB flash, 8.5 kB RAM, no warnings). **Not yet run
on hardware.**

## Wiring (NUCLEO-G431RB)

| Signal | MCU pin | Arduino | Morpho |
|---|---|---|---|
| CC1 | PB6 (UCPD1_CC1) | D10 = CN5-3 | CN10-17 |
| CC2 | PB4 (UCPD1_CC2) | D5 = CN9-6 | CN10-27 |
| GND | — | CN6-6/7 | CN7-19/20 |
| VBUS | — | not to the Nucleo: VBUS goes to the load (RP2040 buck input) | |

- Commands: LPUART1, PA2/PA3, the ST-LINK VCP, 115200 8N1.
- LD2 (PA5) is on while a contract is active.
- The UCPD presents Rd (sink) from power-up.
- No VCONN: the source powers the cable e-marker.

## Build and flash

```sh
cd bench/pd-sink-g431
cargo build --release                      # needs: rustup target add thumbv7em-none-eabihf
# probe-rs:
cargo run --release
# or STM32CubeProgrammer, picking the G431's ST-LINK by serial:
STM32_Programmer_CLI -c port=SWD sn=<G431 ST-LINK SN> -w target/thumbv7em-none-eabihf/release/pd-sink-g431 -v -rst
```

## Protocol

Every line is `WORD key=value ...`. The host sends one command per line (LF or CR). Each command
gets exactly one of:

- `OK ...` or `ERR ...`;
- a `STATUS` or `VERSION` line;
- data lines closed by `END`.

`EVT ...` lines arrive at any time.

| Command | Effect |
|---|---|
| `status` / `?` | `STATUS attached= epr_mode= target_mv= target_ma= contract_pos= contract_mv= contract_ma= contracts= hard_resets= epr_failures=` |
| `caps` | last source capabilities, one `PDO pos= type= mv= ma=` line each, then `END` |
| `req <mV> [mA]` | Sets the target and renegotiates if attached. The target persists across attaches; the default is 5000 mV / 500 mA. The current is capped at the PDO's maximum (Capability Mismatch set if capped). In EPR mode the request is an EPR_Request with the PDO copy, SPR positions included. |
| `epr <W>` | Enter EPR mode with this operational PDP in watts. |
| `eprexit` | Leave EPR mode. |
| `getcaps` | Ask the source for its capabilities again (SPR or EPR, depending on mode). |
| `version`, `help` | — |

Events:

| Event | Meaning |
|---|---|
| `EVT boot` | firmware started |
| `EVT attach cc=CC1\|CC2` | partner attached on that CC line |
| `EVT caps epr_mode= n=` | capabilities received, followed by one `EVT pdo …` per PDO |
| `EVT request pos= mv= ma= epr_request=` | request sent |
| `EVT contract pos= mv= ma= epr_mode=` | accepted and PS_RDY received |
| `EVT epr_enter pdp_w=` | EPR entry started |
| `EVT epr_enter_failed reason=…` | the source's Enter Failed reason, e.g. `CableNotEprCapable`, `SourceFailedToBecomeVconnSource` |
| `EVT epr_exit` | EPR mode left |
| `EVT hard_reset` | Hard Reset |
| `EVT detach` | partner detached |
| `EVT warn …` / `EVT error …` | problems the sink worked around or couldn't |
| `EVT pe_stopped result=…` | the policy engine stopped |

From the repo root, `bu sink` wraps this protocol. Set `[sink].port` or `[sink].stlink_serial`
in `tools/bringup/bringup.local.toml` when both Nucleos are plugged in.

```sh
tools/bringup/bu sink status
tools/bringup/bu sink req 9000 400 --until contract --timeout 3
tools/bringup/bu sink epr 14 --wait 3        # collect the EPR entry events for 3 s
```

## Known limits

- **EPR mode is tracked by the DPM.** usbpd's `is_epr_capabilities()` is true only when there
  are more than 7 PDOs. The PD_Charger default config sends EPR capabilities with only the 7 SPR
  slots, so the sink marks EPR mode itself, from its own `epr` command until a failure, an exit,
  a Hard Reset or a detach.
- **A source-initiated EPR exit is not tracked** beyond what the policy engine does. The next
  `getcaps` or `req` may then use the wrong request type until the source's caps arrive.
- **Sink_Capabilities** is the library default: 5 V, 1 A.
