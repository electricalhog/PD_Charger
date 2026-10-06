# Commanding the RP2040 buck as an electronic load

> **Update:** the buck is now driven by a QT Py RP2040. The sink (NUCLEO-G431RB) talks to it over
> I2C on the QT Py's STEMMA QT port, not USB-CDC, and both ends are implemented in this
> directory: see [`README.md`](README.md), `buck-load-qtpy/`, `load-link/` and `load-control/`.
> The analysis below (power through V_out, local limits, no reverse power) is what they implement.

Sources reviewed:

- [electricalhog/20v-buck-converter](https://github.com/electricalhog/20v-buck-converter) for the
  hardware: Eagle v6 schematic, and `Datalore-IP-rp2040-v1.sch` for the RP2040 controller module;
- [electricalhog/_20v_Buck_Converter](https://github.com/electricalhog/_20v_Buck_Converter) for
  the firmware: an Arduino sketch for the SAMD51 "Datalore IP M4" module;
- [electricalhog/FPV-Static-Electronic_Load](https://github.com/electricalhog/FPV-Static-Electronic_Load)
  for the load UI: CircuitPython 6.1 on a Datalore IP M4 with an ILI9341 touch display.

## What exists

**Power stage**

- Three-phase synchronous buck: three high-side/low-side FET pairs, with a gate driver per phase
  that has its own `DISABLE` input.
- One current-sense amplifier after the three inductors, so it measures **output** current.
- VIN and VOUT dividers, an NTC, and an 8400 µF bulk output capacitor.
- Designed for 8S LiPo in and 20 V out at up to about 50 A (`SET_CURRENT 50`, `OVER_CURRENT 80`).

**RP2040 module nets** (from `Datalore-IP-rp2040-v1.sch`)

| RP2040 | Net | Buck function |
|---|---|---|
| GPIO0 / GPIO2 / GPIO4 | PWM_0..2 | phase 1..3 PWM. These are PWM slices 0A, 1A, 2A: independent counters, so interleaving is a counter preload, as in the SAMD51 code. |
| GPIO1 / GPIO3 / GPIO5 | DISABLE_0..2 | gate-driver disable per phase |
| GPIO6 | FAN | fan |
| GPIO26 / ADC0 | INPUT_TAP | VIN divider |
| GPIO27 / ADC1 | OUTPUT_TAP | VOUT divider |
| GPIO28 / ADC2 | CURRENT_SENSE (buffered) | output current |
| GPIO29 / ADC3 | TEMP_SENSE (buffered) | NTC |
| USB D+/D− | — | **the only data link off the module** |

**Firmware.** The sketch regulates a fixed `SET_VOLTAGE` of 20 V with a ±0.1 %/iteration duty
step. It has no command interface, it targets the SAMD51 TCC timers, and there is no RP2040 port
yet. Two things in it matter for this use:

- `calc_saturation()` divides by `(FREQUENCY / 1000)`, which is integer `250 / 1000 = 0`. The
  duty cap therefore always clamps to 95 %, and the "saturation" limit is not in effect.
- `undervolt_protect()` assumes 8 cells: a 3.5 V/cell cut-out, i.e. 28 V. Fed from VBUS it would
  read 5–28 V as a dead pack.

**Load UI.** The electronic-load repo is a touch-GUI skeleton: three tabs with placeholder buttons,
no measurement and no control. Its Datalore IP M4 module uses the same footprint as the buck's
controller site.

So there is nothing to command yet. The decision is where the control loop lives and what it
accepts.

## Recommendation

**1. The RP2040 owns the loop; everything else sends targets.**
- The loop and every protection limit run on the RP2040.
- The host (`bu`) and, optionally, the CircuitPython touch UI only send targets and read telemetry.
- CircuitPython's garbage-collector pauses (milliseconds) make it unsuitable for the loop itself;
  it's fine as a front end.

**2. Port the existing sketch to the RP2040 with the arduino-pico core.** It's the smallest step
from the code you have:
- The `write_pwm` / `output_enable` / phase-shedding structure carries over.
- Only the timer setup changes: three `pwm_slice`s preloaded 0, ⅓ and ⅔ of the period, then
  enabled together with `pwm_set_mask_enabled`.
- `Serial` over USB-CDC is the command port.

A Rust (embassy-rp) port would match the sink, but it is a rewrite.

**3. Command power through the output voltage, not the current sense.** The load is a buck into
a ballast resistor, so

  P_out = V_out² / R  →  V_out\* = √(P\* · R)

- V_out sense is good: with the sketch's 31.7 counts/V on a 10-bit ADC, the divider maps about
  32 V to full scale, which is ≈ 8 mV per count on the RP2040's 12-bit ADC.
- The current sense is scaled for tens of amps: 9.77 counts/A on 10 bits with a 2.2 A
  calibration offset, ≈ 25 mA per count on 12 bits. Its zero offset dominates below about 1 A.

So:
1. Calibrate R once: run a fixed V_out at a few amps and compute R = V/I, or measure it.
2. Run a V_out loop to V_out\*.
3. Use the current sense only as a cross-check and for over-current above about 2 A.

**Choosing R.** V_out ≤ D_max · V_in, so the maximum load power is (D_max·V_in)² / R. Pick R for
the lowest V_in at which you need full power. With D_max = 0.9 and **R = 8 Ω**:

| V_in (contract) | V_out max | P max (stage) | P max at a 0.5 A contract, η ≈ 0.85 |
|---|---|---|---|
| 5 V | 4.5 V | 2.5 W | 2.1 W |
| 9 V | 8.1 V | 8.2 W | 3.8 W |
| 20 V | 18 V | 40 W | 8.5 W |
| 28 V (EPR) | 25.2 V | 79 W | 11.9 W |

At the bench's 500 mA contracts, the **contract current** is the binding limit, not the stage.
Rate the ballast for the highest P\* you will command (≥ 25 W for SPR work).

**4. Local limits on the RP2040.** These act without the host:
- **I_in limit:** P\* ≤ η·V_in·I_contract, with I_contract from the `limit` command. Use a
  conservative η, e.g. 0.8, until you have measured it.
- **V_in sag:** if V_in < 90 % of the contract voltage, back off immediately. That is the
  signature of the source current-limiting or faulting.
- **VBUS drop:** if V_in < 4 V (Hard Reset or detach), all DISABLE lines high and the target
  goes to 0.
- **Over-temperature and V_out max:** below the output-capacitor and ballast ratings.
- **Watchdog:** no command for N seconds → off.

**5. Never let the synchronous buck run backwards into VBUS.** A synchronous buck is
bidirectional: when duty drops faster than the output can discharge through R, the low-side FET
drives the inductor current negative and pumps output-capacitor energy into the input.

- At 10 V, ≈ 8.6 mF stores 0.43 J. VBUS has little capacitance, the G431 sink has no blocking
  device, and the G474 source can't absorb energy (DCM, no synchronous rectification). Its V_out
  would rise into SW_OVP, and the source would Hard Reset.
- **Slew V_out\* downward no faster than the ballast discharges it.** τ = R·C ≈ 8 Ω × 8.6 mF
  ≈ 69 ms, so the step-down rate should be ≤ V_out/τ.
- **Stop by setting all DISABLE lines high (both FETs off), not by commanding 0 % duty.** The
  current `write_pwm` clamps the compare value to ≥ 1 and always keeps one phase enabled, so a
  0 % "off" leaves the low side switching.
- **Optional:** a Schottky diode or an ideal-diode controller in series with the load input
  blocks reverse flow outright, at the cost of one drop.

**6. Inrush and standby.** Before an explicit contract, a sink may draw only standby power.
- The load starts disabled and enables only on a `limit` command, which the host sends after
  `bu sink … --until contract`.
- Soft-start V_out from 0. The sketch's soft start ramps duty against an 8.4 mF capacitor; keep
  it, and cap the ramp so I_in stays under I_contract.

## Proposed load protocol (same shape as the sink's)

USB-CDC, one ASCII line per command, replies `OK`/`ERR`/`STATUS`, unsolicited `EVT`:

| Command | Effect |
|---|---|
| `limit <in_mA> <contract_mV>` | arm: input-current and V_in-sag limits from the contract; required before any power |
| `p <mW>` | power target → V_out\* = √(P·R), slewed, clamped by the limits |
| `v <mV>` | direct V_out target (calibration, debugging) |
| `r <mΩ>` | ballast resistance used for `p` |
| `off` | DISABLE all phases, target 0, disarm |
| `status` | `STATUS state= vin_mv= vout_mv= iout_ma= p_mw= duty= phases= temp_c= limit_ma= reason=` |
| `EVT fault reason=vin_sag\|vbus_lost\|over_temp\|watchdog\|over_current` | the load backed off on its own |

## Host orchestration (what `bu` does for a test point)

```
bu sink req 9000 500 --until contract     # contract at 9 V, 500 mA
load: limit 500 9000                      # arm the load from the contract
load: p 1000 … p 3500 (steps)             # sweep the power target
each step: load status + bu pd status + bu regulator status (V_out, I_out, mode)
load: off  (before every renegotiation, then re-arm from the new contract)
```

That gives source regulation, transition and efficiency data per PDO.

The same loop drives EPR: `bu sink epr <W>`, then `req` at an EPR or SPR position, then re-arm
the load. A `bu load` module wrapping the protocol above is a small addition once the RP2040
firmware exists.

## Open items

- Which gate driver IC3–IC5 is, and whether its `DISABLE` holds both FETs off. The behavior in
  point 5 depends on it.
- The ADC reference on the RP2040 module (the `ADC_REF` shunt reference): its value sets every
  scale factor above, so calibrate against a meter.
