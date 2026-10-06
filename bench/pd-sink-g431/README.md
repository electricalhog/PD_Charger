# pd-sink-g431: scriptable USB-PD sink for the bench

Firmware for a NUCLEO-G431RB that acts as the sink in the PD_Charger tests.

- It requests exactly the voltage and current you give it.
- It enters or leaves EPR mode only when told to.
- It reports every PD step as a text line on the ST-LINK virtual COM port.

It is built on [elagil/usbpd](https://github.com/elagil/usbpd) (sink policy engine, SPR + EPR)
and [embassy](https://github.com/embassy-rs/embassy) (UCPD driver). The UCPD glue follows usbpd's
`examples/embassy-stm32-g431cb-epr`. Both dependencies are pinned in `Cargo.toml` to the commits
that example builds against.

Status: builds (`cargo build --release`: 84.9 kB flash, 10.7 kB RAM, no warnings). **Not yet
run on hardware.**

## Wiring

The sink is the NUCLEO-G431RB plus the X-NUCLEO-SRC1M1 shield's TCPP02 (CC protection) and
receptacle. VBUS bypasses the shield's power path and goes straight to the buck load. The full
pin table and the QT Py link are in [`../README.md`](../README.md).

- **UCPD:** CC1 = PB6, CC2 = PB4, through the TCPP02.
- **I2C1:** PB8 SCL, PB9 SDA at 100 kHz, async with DMA1 CH3/CH4. Two devices share it: the
  TCPP02 at 0x34 and the load at 0x55.
- **TCPP02 control:** ENABLE = PC8, FLGn = PC5.
- **VBUS sense:** PA0, through the shield's 200k/40k divider (×6). Full scale is 19.8 V; above
  that, `vbus_sat=1`.
- **Commands:** LPUART1 (PA2/PA3), the ST-LINK VCP, 115200 8N1. LD2 (PA5) is on while a contract
  is active.

At boot the firmware sets the TCPP02 to Normal mode with both gate drivers open, the discharge
off and VCONN open (`EVT tcpp02 ok ack=0x20` expected), then polls its flags every 100 ms.

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
| `status` / `?` | `STATUS attached= epr_mode= target_mv= target_ma= contract_pos= contract_mv= contract_ma= contracts= hard_resets= epr_failures= vbus_mv= vbus_sat= tcpp_ok= tcpp_fault= tcpp_flags= hold= load_online=` |
| `caps` | last source capabilities, one `PDO pos= type= mv= ma=` line each, then `END` |
| `req <mV> [mA]` | Sets the target and renegotiates if attached. The target persists across attaches; the default is 5000 mV / 500 mA. The current is capped at the PDO's maximum (Capability Mismatch set if capped). In EPR mode the request is an EPR_Request with the PDO copy, SPR positions included. |
| `epr <W>` | Enter EPR mode with this operational PDP in watts. |
| `eprexit` | Leave EPR mode. |
| `getcaps` | Ask the source for its capabilities again (SPR or EPR, depending on mode). |
| `load` / `load status` | `LOAD online= state=off\|running\|fault\|no_power_stage fault= armed_cmd= p_target_mw= vin_mv= vout_mv= iout_ma= pout_mw= duty_pm= temp_mv= seq=` |
| `load p <mW>` | Set the load power target and allow arming. Takes effect only with a contract and no `hold`. |
| `load off` | Target 0; the load disarms within one 20 ms frame. |
| `load clear` | Clear a latched load fault (sent with arm low). |
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
| `EVT tcpp02 ok\|error\|fault\|clear …` | TCPP02 init result and flag changes |
| `EVT load state= fault= …` | the load changed state or faulted |
| `EVT load online\|offline` | the I2C link to the load came up or was lost (5 failed frames) |

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
