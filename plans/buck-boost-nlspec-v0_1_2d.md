# NLSpec: Buck-Boost Regulator Firmware

## Version

0.1.2c

---

**⚠ IMPORTANT: Review Appendix A before implementation.** Several open questions remain, including blocking items (Q1, Q10) that must be resolved with Dan before hardware testing. All questions are tagged with their source.

## Meta

- **NLSpec Spec Conformance:** NLSpec v0.2.1
- **Target MCU:** STM32G474RETx (170 MHz Cortex-M4F, LQFP64)
- **Target Board:** PD Regulator Prototype Rev 0 (Daniel Raymond, 2024-12-20)
- **Toolchain:** STM32CubeIDE, STM32CubeMX 6.16.1, STM32Cube FW_G4 V1.6.1, HAL
- **Lineage:** Reconciled from v0.1.1-ab and v0.1.1-bd. v0.1.2c incorporates review corrections from v0.1.2b. See Appendix F for divergence resolution log.
- **Jonah-prescribed items (not in transcripts, confirmed during preliminary spec review):** EPR scope (§1, §10.2 absolute OVP), boost mode as v0.1.2 deliverable (§1.1, §5.3), centralized config header requirements (§2.1), code quality mandates (`-Wall -Werror`, `_Static_assert`, doc-comments in §2.1 and §16).
- **Sources of truth (priority order):** manual corrections > KiCad schematics > (`dc-dc`, `input`, `output`, `PD_Charger`) > `final.ioc` > Round 2a debrief > Round 2b answers > Round 1 answers.
- **Clarification that EPR voltage range is in scope [Jonah-prescribed]:** USB PD Extended Power Range (up to 48 V / 5 A / 240 W) is in scope for this project and this firmware. The regulator must support all SPR and EPR voltage/current combinations within hardware limits (60 V input OVP, inductor/FET current ratings). This was not discussed in transcripts; Jonah confirmed EPR scope during preliminary spec review.
- **Architectural note:** The Round 2a debrief (latter half) contains significant discussion of slope compensation optimization strategies and software architecture decisions that informed this spec. Implementers should review `docs/buck-boost-round2a-debrief.md` for context on Timer ISR vs DMA vs DAC sawtooth tradeoffs.

---

## 1. System Purpose

This firmware controls a custom discrete buck-boost voltage regulator built around an H-bridge of enhancement-mode GaN FETs, a 4.7 µH inductor, and uP1966E gate drivers. The system receives DC input power (up to 60 V, protected by an ADM1270ACPZ hot-swap IC) and regulates it to a USB PD-negotiated output voltage.

The firmware has two primary responsibilities:

1. **Regulator control** — a hardware-assisted inner current loop and a software-based outer voltage loop that together produce a regulated output voltage.
2. **USB Power Delivery negotiation** — a FreeRTOS-hosted PD stack that negotiates voltage/current contracts with a connected USB-C sink device.

These two responsibilities are architecturally independent. The PD stack runs under FreeRTOS. The regulator control loop does not. They communicate through shared state and a function API (§9).

### 1.1 Scope

**In scope for v0.1.2:**

- Buck mode operation (V_in > V_out) — default mode, primary test target
- Boost mode operation (V_in < V_out) — fully specified, implementation required **[Jonah-prescribed; transcript says "not in scope" but Jonah confirmed during preliminary spec review]**
- USB PD Standard Power Range (SPR) and Extended Power Range (EPR) voltage support (5 V to 48 V)
- Peak current mode control via hardware comparator (COMP1 → EEV4 → HRTIM)
- Slope compensation of the current-mode comparator threshold
- Software PID voltage regulation loop
- ADC measurement of V_in, V_out, I_inductor, I_in, I_out
- Bootstrap capacitor refresh for the inactive H-bridge leg
- HRTIM fault handling (FLT1, FLT2)
- Software safety checks (OVP with absolute hard cap, UVP, V_in validation for both modes)
- Soft-start on initial enable and after mode changes
- Integration boundary with the USB PD stack (shared state + function API)

**Out of scope:**

- Four-switch buck-boost transition region (future spec revision)
- USB PD stack internals (handled by X-CUBE-TCPP middleware)
- PID coefficient tuning (empirical; values are runtime-configurable)
- PCB layout, thermal management, EMI
- Production test sequences

**Boundary exclusions:**

This spec does not define the USB PD negotiation logic, the TCPP middleware configuration, or the LPUART debug protocol format. These are provided by the CubeMX-generated project and ST middleware. The spec defines only the interface through which the PD stack communicates a voltage setpoint to the regulator, and the telemetry values the regulator must expose.

---

## 2. Prerequisite: CubeMX-Generated Base

This spec assumes the firmware is built on top of a fresh STM32CubeIDE project generated from `final.ioc`. Dan and Jonah configure the CubeMX project; the implementer writes only application code. All peripheral initialization — pin muxing, clock tree, DMA channels, NVIC priorities, UCPD/USBPD/TCPP configuration — is handled by the generated `MX_*_Init()` functions.

Where this spec states a peripheral configuration requirement (e.g., "Timer A Output 1 SetSource = Period"), that is a **requirement on the CubeMX project**, not on the firmware source code. Dan or Jonah must ensure the CubeMX project satisfies these before implementation begins.

The implementer should not modify CubeMX-generated initialization functions except where this spec explicitly requires post-init configuration (e.g., HRTIM output Set/Reset source assignment, backstop compare register). All such modifications are called out.

### 2.1 Centralized Pin Mapping and Configuration Header [Jonah-prescribed]

All hardware-derived constants, pin assignments, and compile-time configurable parameters must be centralized in a single header file (e.g., `regulator_config.h`). This file is the single point of truth for all hardware parameters in the firmware. This requirement was prescribed by Jonah during preliminary spec review and was not discussed in transcripts.

**Pin mapping entries** must include a doc-comment for each pin defining: the pin name, the MCU port/pin identifier, the net label from the schematic, what it connects to physically, and the signal direction. Example:

```c
/**
 * VD_MON — Output voltage sense (V_out)
 * MCU pin: PA0 / ADC1_IN1
 * Net: VD_MON on output.kicad_sch
 * Connected to: 100kΩ/5.82kΩ resistive divider from V_out rail
 * Direction: Analog input
 */
#define PIN_VD_MON_PORT    GPIOA
#define PIN_VD_MON_PIN     GPIO_PIN_0
#define ADC_CH_VD_MON      ADC_CHANNEL_1
```

**Compile-time configurable constants** (switching frequency, max duty cycle, blanking duration, OVP thresholds, etc.) must each have a doc-comment stating: the parameter name, expected units, valid range, and a brief rationale or derivation reference. Example:

```c
/**
 * HRTIM period register value — sets the switching frequency.
 * Units: HRTIM timer counts (183.82 ps/count at MUL32 prescaler)
 * Derivation: 170 MHz × 32 / f_sw. See §5.5.
 * 27200 = 200 kHz, 10880 = 500 kHz.
 */
#define HRTIM_PERIOD_COUNTS  27200u
```

Constants may be implemented as `#define` macros or `static const` variables — both are acceptable. If the toolchain supports `_Static_assert` (C11) or `static_assert` (C++11), compile-time range assertions are **highly recommended** for all configurable parameters. Example:

```c
_Static_assert(HRTIM_PERIOD_COUNTS >= 5440u && HRTIM_PERIOD_COUNTS <= 54400u,
               "HRTIM_PERIOD_COUNTS out of valid range [5440, 54400]");
```

### 2.2 CubeMX Configuration Requirements

The following peripheral configurations must be present in the CubeMX project. These are verified from the IOC file unless marked otherwise.

**HRTIM1:**

| Item                   | Configuration                                                      | Confirmed                 | Source         |
| ---------------------- | ------------------------------------------------------------------ | ------------------------- | -------------- |
| Master timer prescaler | `HRTIM_PRESCALERRATIO_MUL32` (5.44 GHz equivalent, 183.82 ps tick) | Yes                       | IOC            |
| Timer A outputs        | CHA1 (PA8), CHA2 (PA9) — input-side half-bridge (Q1/Q2)            | Yes                       | IOC            |
| Timer B outputs        | CHB1 (PA10), CHB2 (PA11) — output-side half-bridge (Q3/Q4)         | Yes                       | IOC            |
| Dead time insertion    | Enabled on both Timer A and Timer B                                | Yes                       | IOC            |
| EEV4                   | `HRTIM_EVENT_4`, sourced from COMP1 output                         | Yes                       | IOC            |
| CompareUnit1 (Timer A) | `__NULL` in IOC — duty cycle is NOT terminated by compare match    | Yes                       | IOC            |
| FLT1                   | PA12, active-low polarity                                          | Yes                       | IOC            |
| FLT2                   | PA15, active-low polarity                                          | Yes                       | IOC            |
| Timer A SetSource      | Period (counter reset → output HIGH → charge begins)               | **ASSUMED — see OPEN:Q1** | Circumstantial |
| Timer A ResetSource    | EEV4 (COMP1 trips → output LOW → discharge begins)                 | **ASSUMED — see OPEN:Q1** | Circumstantial |
| Fault action           | All outputs forced inactive (all FETs off)                         | Expected                  | CubeMX default |

**COMP1:**

| Item                    | Configuration                                                                                          | Source                               |
| ----------------------- | ------------------------------------------------------------------------------------------------------ | ------------------------------------ |
| Non-inverting input (+) | PA1 (IL_MON — inductor current sense, amplified at 50 V/V)                                             | IOC: `SharedAnalog_PA1`, `COMP1_INP` |
| Inverting input (−)     | DAC3_OUT1 (internal — slope-compensated peak current reference)                                        | IOC: `VP_COMP1_VS_DAC3OUT1`          |
| External output         | PA6 (`COMP1_OUT`)                                                                                      | IOC                                  |
| Event routing           | COMP1 output → HRTIM EEV4                                                                              | IOC                                  |
| Blanking window         | 100 ns default (compile-time constant, §2.1). Adjustable 100–500 ns empirically. Not confirmed in IOC. | [Jonah]                              |

**Comparator behavior:** When the voltage at PA1 (amplified inductor current) exceeds the voltage at DAC3_OUT1 (the decaying slope-compensated setpoint), COMP1 output goes high. This asserts EEV4, which resets the active timer output, ending the charge phase.

**DAC3:**

| Channel | Mode                      | Internal Connection                   | Function                                                                     |
| ------- | ------------------------- | ------------------------------------- | ---------------------------------------------------------------------------- |
| CH1     | Internal (`DAC_OUT1_Int`) | COMP1_INM                             | Slope-compensated peak current setpoint                                      |
| CH2     | Internal (`DAC_OUT2_Int`) | None active (COMP3/COMP4 not enabled) | **OPEN:Q4** — reserved; firmware must not use without explicit specification |

Resolution: 12-bit (4096 counts). V_REF+ = 3.3 V (V_DDA). LSB = 3.3 V / 4096 ≈ 0.806 mV/count.

**ADC Channel Mapping:**

| Pin | ADC  | Channel | Net Label   | Signal                             | Confirmed |
| --- | ---- | ------- | ----------- | ---------------------------------- | --------- |
| PA0 | ADC1 | IN1     | VD_MON      | V_out (output voltage)             | Yes       |
| PA1 | ADC1 | IN2     | IL_MON      | I_inductor (shared with COMP1_INP) | Yes       |
| PC1 | ADC1 | IN7     | **OPEN:Q2** | IS_MON or ID_MON                   | No        |
| PB0 | ADC1 | IN15    | **OPEN:Q2** | ID_MON or IS_MON                   | No        |
| PA4 | ADC2 | IN17    | VS_MON      | V_in (input voltage)               | Yes       |

ADC1 sampling time: 2.5 cycles (as configured in IOC). PA1 (ADC1_IN2 / IL_MON) is shared with COMP1_INP. The ADC read of this channel is informational (telemetry) and must not interfere with the comparator's real-time operation.

**Naming convention (from schematic):** VD = V Delivery (output), VS = V Source (input), IL = I Inductor, ID = I Delivery (output current), IS = I Source (input current).

**Communication and Debug:**

| Peripheral | Pins                                  | Function                                         | Source           |
| ---------- | ------------------------------------- | ------------------------------------------------ | ---------------- |
| LPUART1    | PA2 (TX), PA3 (RX)                    | TRACER_EMB debug output, 921600 baud, 7-bit word | IOC              |
| I2C1       | PB8 (SCL), PB9 (SDA)                  | TCPP USB PD port controller communication        | IOC, X-CUBE-TCPP |
| UCPD1      | PB6 (CC1), PB4 (CC2)                  | USB PD PHY (CC line communication)               | IOC              |
| SWD        | PA13 (SWDIO), PA14 (SWCLK), PB3 (SWO) | Debug interface                                  | IOC              |
| UCPD1 DMA  | TX on DMA1_CH2, RX on DMA1_CH4        | UCPD data transfer                               | IOC              |

**FreeRTOS:**

| Parameter                                      | Value                      | Source |
| ---------------------------------------------- | -------------------------- | ------ |
| API                                            | CMSIS v1                   | IOC    |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 3                          | IOC    |
| Total heap                                     | 7000 bytes                 | IOC    |
| Default task                                   | Priority 0, 128-word stack | IOC    |

FreeRTOS is present exclusively for the USB PD stack (UCPD1 + X-CUBE-TCPP). The regulator control loop must **not** run inside an RTOS task. All regulator-related ISRs must have NVIC priorities numerically lower (higher urgency) than 3, so they are never masked by FreeRTOS critical sections.

**USB PD:**

| Parameter       | Value                                   | Source                                         |
| --------------- | --------------------------------------- | ---------------------------------------------- |
| Middleware      | X-CUBE-TCPP 4.2.0                       | IOC                                            |
| Role            | Source                                  | IOC (`GUI_INTERFACE.PDTypeName=BUCKBOOST_SRC`) |
| Port controller | TCPP0203 (via I2C1)                     | IOC                                            |
| PDO 0           | 5 V @ 100 mA (default/safe)             | CubeMX USBPD config                            |
| PDO 1           | Variable voltage @ 1500 mA (negotiated) | CubeMX USBPD config                            |

---

## 3. Hardware Parameters

### 3.1 Microcontroller

- **Part:** STM32G474xE (LQFP64)
- **SYSCLK:** 170 MHz (PLL from 16 MHz HSI, M÷4 = 4 MHz VCO input, N×85 = 340 MHz VCO, ÷2 = 170 MHz SYSCLK)
- **HRTIM clock:** 170 MHz × 32 (MUL32 prescaler) = 5.44 GHz equivalent tick rate, 183.82 ps resolution

**Clock source (resolved):** HSI confirmed as PLL source. 16 MHz HSI / 4 = 4 MHz VCO input × 85 = 340 MHz VCO / 2 = 170 MHz SYSCLK. The HSE oscillator pins (PF0/PF1) are configured in the IOC as `RCC_OSC_IN`/`RCC_OSC_OUT` but the oscillator is not enabled — HSI is the sole active source.

### 3.2 Power Stage

| Parameter                     | Value                                                                           | Source                         |
| ----------------------------- | ------------------------------------------------------------------------------- | ------------------------------ |
| Topology                      | Synchronous H-bridge, 4× enhancement-mode GaN FETs                              | dc-dc.kicad_sch.txt            |
| Inductor                      | 4.7 µH, single, series between half-bridge nodes                                | Round 1 answers Q2             |
| Input voltage range           | Up to 60 V (OVP limit from ADM1270)                                             | Round 1 answers Q3             |
| Output voltage range          | 5 V to 48 V (USB PD SPR: 5, 9, 15, 20 V; EPR: up to 28, 36, 48 V)               | USB PD 3.1 spec                |
| Maximum output current        | 5 A (USB PD EPR spec limit); PDO advertised max = 1500 mA (SPR, v0.1.2 default) | USB PD 3.1, Round 1 answers Q3 |
| Gate drivers                  | 2× uP1966E (uPI Semiconductor), half-bridge                                     | dc-dc.kicad_sch.txt            |
| Bootstrap cap hold time       | ~10 µs                                                                          | Round 2a debrief               |
| Bootstrap refresh requirement | Switch node to GND for ≥200 ns                                                  | Round 2a debrief               |
| Bootstrap UV lockout behavior | High-side FET drops to body diode; driver does not switch output polarity       | Round 2a debrief               |
| Input protection              | ADM1270ACPZ (60 V OVP) with P-ch disconnect FET                                 | input.kicad_sch.txt            |

### 3.3 Current Sensing

Three shunt resistors with dedicated current sense amplifiers:

| Shunt    | Ref | Value | Amplifier     | Gain   | Signal | Sensitivity | MCU Pin     | ADC                    |
| -------- | --- | ----- | ------------- | ------ | ------ | ----------- | ----------- | ---------------------- |
| Inductor | R5  | 5 mΩ  | INA281A2 (U7) | 50 V/V | IL_MON | 250 mV/A    | PA1         | ADC1 IN2 (+ COMP1_INP) |
| Output   | R6  | 12 mΩ | INA281A2 (U8) | 50 V/V | ID_MON | 600 mV/A    | **OPEN:Q2** | ADC1 IN7 or IN15       |
| Input    | R25 | 5 mΩ  | INA293A2 (U6) | 50 V/V | IS_MON | 250 mV/A    | **OPEN:Q2** | ADC1 IN7 or IN15       |

**Source:** `dc-dc.kicad_sch.txt` confirms U7 = INA281A2, U8 = INA281A2. `input.kicad_sch.txt` contains U6 with KiCad lib_id `INA281A2` but value field `INA293A2` — the schematic is internally contradictory. Dan's verbal statement ("all INA293s") adds a third claim. Both parts have 50 V/V gain for the A2 variant; the gain values above are correct regardless. The BOM should be verified during board bring-up.

**OPEN:Q2** — PC1 (ADC1_IN7) and PB0 (ADC1_IN15) are both configured for ADC input. One is ID_MON, the other IS_MON. The assignment is unconfirmed. Non-blocking for core regulation (which uses only V_out and IL_MON).

**R25 (input shunt) — RESOLVED [Jonah]:** Confirmed 5 mΩ by physical inspection (Jonah present when Dan read resistor code off the shunt). The schematic shows 2 mΩ — the schematic is stale. IS_MON sensitivity = 5 mΩ × 50 V/V = 250 mV/A (same as IL_MON).

### 3.4 Voltage Sensing

Two resistive dividers, identical topology:

| Signal | Net Label | R_high | R_low   | Ratio   | MCU Pin | ADC       |
| ------ | --------- | ------ | ------- | ------- | ------- | --------- |
| V_in   | VS_MON    | 100 kΩ | 5.82 kΩ | 0.05500 | PA4     | ADC2 IN17 |
| V_out  | VD_MON    | 100 kΩ | 5.82 kΩ | 0.05500 | PA0     | ADC1 IN1  |

Divider ratio: R_low / (R_high + R_low) = 5820 / 105820 = 0.05500. At 60 V → ADC pin ≈ 3.300 V (full scale, aligns with OVP limit).

### 3.5 DAC3 Parameters (Slope Compensation)

| Parameter                          | Value                                    | Source                              |
| ---------------------------------- | ---------------------------------------- | ----------------------------------- |
| Resolution                         | 12-bit (4096 counts)                     | STM32G474 DAC3                      |
| Output range                       | 0 to V_REF (3.3 V)                       | STM32G474 standard                  |
| Counts per volt                    | 4096 / 3.3 ≈ 1241 counts/V               | Calculated                          |
| Current resolution via comparator  | 3.3 / (4096 × 0.250) ≈ **3.22 mA/count** | Calculated (IL_MON gain = 250 mV/A) |
| Maximum representable peak current | 3.3 / 0.250 = **13.2 A**                 | Calculated                          |

### 3.6 HRTIM Output Mapping

| Timer | Output 1 (Hx1) | Output 2 (Hx2) | Pin Pair    | Drives                          |
| ----- | -------------- | -------------- | ----------- | ------------------------------- |
| A     | CHA1           | CHA2           | PA8 / PA9   | Input-side half-bridge (Q1/Q2)  |
| B     | CHB1           | CHB2           | PA10 / PA11 | Output-side half-bridge (Q3/Q4) |

Both timer outputs have dead time insertion enabled. CHA1/CHA2 are complementary; CHB1/CHB2 are complementary.

### 3.7 HRTIM Fault Inputs

| Fault | Pin  | Polarity   | Source                           |
| ----- | ---- | ---------- | -------------------------------- |
| FLT1  | PA12 | Active-low | ADM1270 ~{FAULT} output (likely) |
| FLT2  | PA15 | Active-low | **OPEN:Q5** — unconfirmed        |

When either fault input asserts, the HRTIM must force all outputs to their safe state (all FETs off).

---

## 4. System States

The regulator operates in one of four top-level states:

```
            ┌──────────┐
     reset──▶  INIT    │
            └────┬─────┘
                 │ peripherals ready
                 ▼
            ┌──────────┐
            │  IDLE     │◀────── disable command or fault cleared
            └────┬─────┘
                 │ enable command (from PD stack or debug)
                 ▼
            ┌──────────┐
            │ RUNNING   │──────▶ fault ──▶ ┌────────┐
            └──────────┘                   │ FAULT  │
                                           └────────┘
```

### 4.1 INIT

Entered at power-on reset. CubeMX-generated `MX_*_Init()` functions execute. The firmware then performs post-init configuration:

1. Configure HRTIM Timer A and Timer B Set/Reset sources (see §5.5).
2. Configure backstop Compare 1 registers on Timer A and Timer B (see §10.3).
3. Configure DAC3 CH1 initial value to 0 (comparator threshold = 0 → no switching).
4. Start COMP1.
5. Configure ADC sampling triggers and channels.
6. Start ADC1 and ADC2.
7. Configure slope compensation timer (frequency, step size — stopped, not yet running).
8. Configure the non-switching leg for the default operating mode (buck: Timer B = wire to V_out).
9. Enable HRTIM fault inputs.
10. Verify no active fault.
11. Transition to IDLE.

### 4.2 IDLE

All HRTIM outputs are inactive (all FETs off). The PID loop does not run. ADCs may be read for telemetry. DAC3 CH1 is held at 0.

Transition to RUNNING requires:
- An explicit enable command (source: PD stack API call, or debug interface).
- A valid voltage setpoint (> 0 mV, ≤ 48000 mV).
- A known operating mode (buck or boost), determined by comparing V_in to V_setpoint.
- No active fault condition.

### 4.3 RUNNING

The regulator is actively switching. The inner current loop and outer voltage loop are both active. The system is in either BUCK or BOOST sub-mode (see §5).

Transition out of RUNNING occurs on:
- Disable command → IDLE
- Fault assertion (FLT1 or FLT2) → FAULT
- Software-detected overvoltage, undervoltage, or V_in violation → FAULT
- Setpoint of 0 mV received → IDLE
- Max on-time backstop fires 3 consecutive times → FAULT (§10.4)

### 4.4 FAULT

All HRTIM outputs are forced to safe state by hardware (if fault came from FLT1/FLT2) or by software. The PID integrator is reset. DAC3 CH1 is set to 0. The slope compensation timer and PID timer are disabled.

The fault condition is **latched**. Clearing requires:
1. The hardware fault input to deassert (if hardware-sourced).
2. An explicit fault-clear command from the PD stack or debug interface.
3. Return to IDLE (not directly to RUNNING).

Recovery from a fault condition is **not automatic** in v0.1.2.

---

## 5. Operating Modes and Switching Behavior

### 5.1 Mode Selection

The default operating mode is **BUCK**. The operating mode is determined by comparing V_in (measured) to V_setpoint (commanded):

- **Buck mode:** V_in > V_setpoint + V_hysteresis
- **Boost mode:** V_in < V_setpoint − V_hysteresis

V_hysteresis = 1.0 V (compile-time constant).

When V_in is within ±V_hysteresis of V_setpoint, the system remains in its current mode. The four-switch transition region is out of scope for v0.1.2.

Mode transitions while RUNNING are permitted but require a controlled sequence:
1. Disable HRTIM outputs (all FETs off).
2. Reconfigure which timer is the switching leg and which is the wire leg.
3. Reconfigure EEV4 to act on the correct timer.
4. Reconfigure HRTIM output polarity so EEV4 ends the charge phase in the new mode.
5. Update software safety check thresholds for the new mode.
6. Reset PID integrator.
7. Re-enable HRTIM outputs.
8. Perform soft-start to the target voltage (§11.2).

The output capacitor maintains V_out during the all-off window.

### 5.2 Buck Mode (Default)

**Active switching leg:** Timer A (input-side half-bridge).
**Static leg:** Timer B (output-side half-bridge), held with high-side FET on (output inductor node connected to V_out as a "wire").

| Phase     | Timer A State | Timer B State       | Input Node | Output Node | dI_L/dt             |
| --------- | ------------- | ------------------- | ---------- | ----------- | ------------------- |
| Charge    | HIGH (V_in)   | HIGH (V_out) — wire | V_in       | V_out       | +(V_in − V_out) / L |
| Discharge | LOW (GND)     | HIGH (V_out) — wire | GND        | V_out       | −V_out / L          |

**Cycle timing:**
1. At HRTIM Timer A period reset → CHA1 output goes HIGH (charge phase begins). This is the **Set** event.
2. Inductor current ramps up at (V_in − V_out) / L.
3. When IL_MON reaches the slope-compensated threshold on DAC3 CH1, COMP1 fires.
4. COMP1 output triggers EEV4 → CHA1 output goes LOW (discharge phase begins). This is the **Reset** event.
5. Inductor current ramps down at −V_out / L until the next period reset.

Timer A Output 2 (CHA2, PA9) is the complementary low-side drive with dead time insertion.

**Output-side fixed state (buck mode):** Timer B must hold the output-side half-bridge with CHB1 (high-side, PA10) ON and CHB2 (low-side, PA11) OFF. The mechanism for holding Timer B in this state is an implementation choice.

### 5.3 Boost Mode

**Active switching leg:** Timer B (output-side half-bridge).
**Static leg:** Timer A (input-side half-bridge), held with high-side FET on (input inductor node connected to V_in as a "wire").

| Phase     | Timer A State      | Timer B State                      | Input Node | Output Node | dI_L/dt                        |
| --------- | ------------------ | ---------------------------------- | ---------- | ----------- | ------------------------------ |
| Charge    | HIGH (V_in) — wire | LOW (GND) / CHB1 LOW, CHB2 HIGH    | V_in       | GND         | +V_in / L                      |
| Discharge | HIGH (V_in) — wire | HIGH (V_out) / CHB1 HIGH, CHB2 LOW | V_in       | V_out       | +(V_in − V_out) / L (negative) |

**Cycle timing:**
1. At HRTIM Timer B period reset → CHB1 goes LOW, CHB2 goes HIGH (charge phase begins, inductor output to GND). This is the **Set** event.
2. Inductor current ramps up at V_in / L.
3. When IL_MON reaches the slope-compensated threshold on DAC3 CH1, COMP1 fires.
4. COMP1 output triggers EEV4 → CHB1 goes HIGH, CHB2 goes LOW (discharge phase begins, inductor output to V_out). This is the **Reset** event.
5. Inductor current ramps down at (V_in − V_out) / L until the next period reset.

**Input-side fixed state (boost mode):** Timer A must hold CHA1 (high-side, PA8) ON and CHA2 (low-side, PA9) OFF.

**EEV4 routing:** In buck mode, EEV4 acts on Timer A. In boost mode, EEV4 acts on Timer B. The firmware must reconfigure the EEV4 target timer when switching operating modes. EEV4 triggers the output from inactive → active state (Round 2a debrief). The polarity must be configured such that this transition ends the charge phase in each mode.

**Note on polarity:** In buck mode, the "inactive" state of CHA1 is HIGH and EEV4 resets it to LOW. In boost mode, the "inactive" state of CHB1 is LOW and EEV4 sets it to HIGH. The HRTIM output polarity configuration handles this difference. The comparator and DAC behavior is identical in both modes — the comparator always fires when inductor current exceeds the threshold.

### 5.4 Bootstrap Capacitor Refresh

The uP1966E gate drivers use bootstrap capacitors that can hold the high-side FET on for approximately 10 µs. Recharging requires the switch node at GND for ≥200 ns.

**Which leg needs refresh:** In each operating mode, the "static" leg — whose high-side FET is held on continuously — needs periodic refresh. The switching leg does not need explicit refresh because its switch node touches ground every cycle during the discharge phase.

**v0.1.2 behavior (naive):** At the end of every switching period, the static leg is briefly cycled to its low-side state (switch node = GND) for ≥200 ns and ≤500 ns, then returned to its high-side state.

**Buck mode refresh:** The refresh occurs at the end of the discharge phase (both nodes at GND). Inductor current forms a short-circuit loop through two low-side FETs. In a lossless system, dI/dt = 0. In practice, inductor DCR causes slight current decay. This is benign.

**Boost mode refresh:** The refresh occurs at the end of the discharge phase. Input node switches from V_in to GND while output node is at V_out. Inductor sees V_out across it (reverse-biased), causing current to ramp down at V_out / L — much faster than normal. This is why the refresh must be short (≤500 ns).

**Implementation:** The refresh pulse is generated by the HRTIM. The static leg's timer must produce a brief low pulse at the end of each period. **The specific HRTIM register configuration is intentional ambiguity.** The constraint is: the static leg's switch node must be at GND for ≥200 ns and ≤500 ns, once per period, at the end of the period.

**Future optimization (not in scope):** Refresh only every N periods, where N × T_sw ≤ 10 µs. At 200 kHz (5 µs), N ≤ 2. The architecture must not preclude this.

### 5.5 HRTIM Configuration

#### Set/Reset Sources (OPEN:Q1)

This spec assumes the following HRTIM Set/Reset source configuration:

**Timer A (buck mode active):**
- CHA1 SetSource = Timer A Period (counter reset event)
- CHA1 ResetSource = EEV4 (COMP1 output)
- Output polarity: Active = LOW (so "Set on Period" drives HIGH, "Reset on EEV4" drives LOW)

**Timer B (boost mode active):**
- CHB1 SetSource = Timer B Period (counter reset event)
- CHB1 ResetSource = EEV4 (COMP1 output)
- Output polarity: Active = HIGH (so "Set on Period" drives LOW, "Reset on EEV4" drives HIGH)

**Supporting evidence:** CompareUnit1 for Timer A is `__NULL` in the IOC, ruling out compare-register-based duty termination. EEV4 is the only configured external event. Dan confirmed in the debrief that "Event 4 only needs to trigger the HRTIM to go from its inactive to active state."

**OPEN:Q1** — The CubeMX IOC file does not contain Set/Reset source assignments; these are in the HAL init code (`MX_HRTIM1_Init()`). Dan must confirm this assumption is correct before proceeding — either by sharing the HAL init code or by direct confirmation. **If this assumption is wrong, the entire switching behavior description in §5.2 and §5.3 is inverted.**

#### Switching Frequency

- **Initial value:** 200 kHz (period = 5.0 µs)
- **Target value:** 500 kHz (period = 2.0 µs)
- **Period register at 200 kHz:** 170,000,000 Hz × 32 / 200,000 Hz = **27,200 counts** (exact)
- **Period register at 500 kHz:** 170,000,000 Hz × 32 / 500,000 Hz = **10,880 counts** (exact)

The switching frequency is a compile-time constant in v0.1.2. It is defined as a period register value, not a frequency, to avoid floating-point math at runtime.

```
#define HRTIM_PERIOD_COUNTS  27200u   /* 200 kHz */
```

The same period value is used for both Timer A and Timer B. They run synchronously from the Master timer.

#### Dead Time

Dead time insertion is enabled on both Timer A and Timer B. The dead time value is configured in the CubeMX-generated init code. **This spec does not prescribe a specific dead time value.** The implementer must verify the generated value is appropriate for the GaN FETs (typical: 10–50 ns rising, 10–50 ns falling).

#### Blanking

After each switching transition, IL_MON exhibits ringing from parasitic inductance and capacitance. The HRTIM's blanking window must suppress this ringing to prevent false comparator trips.

**Initial blanking duration:** 100 ns (compile-time constant in centralized config header, §2.1). Adjust empirically in the range 100–500 ns based on oscilloscope observation of IL_MON ringing.

If blanking is not configured, the system will exhibit false comparator trips and erratic duty cycle behavior. This is a required configuration, not optional.

---

## 6. Inner Control Loop: Peak Current Mode

The inner loop is implemented almost entirely in hardware. It executes autonomously every switching period with no CPU involvement in steady state.

### 6.1 Signal Chain

```
PID output (DAC counts)
    │
    ▼
DAC3 CH1 ──→ slope compensation ramp (decreasing within period)
    │
    ▼
COMP1 INM (inverting input)
                                        │
IL_MON (PA1) ──→ COMP1 INP             │
                   (non-inverting)       ▼
                                    COMP1 output
                                        │
                                        ▼
                                    EEV4 → HRTIM
                                        │
                                        ▼
                                  Active timer output resets
                                  (charge phase terminates)
```

### 6.2 Current-to-Voltage Scaling

```
V_COMP1_INP = I_L × R_shunt × G_amp = I_L × 5 mΩ × 50 = I_L × 0.250 V/A
V_DAC3 = DAC_count × (3.3 V / 4096) = DAC_count × 0.000806 V/count
DAC_count = I_peak × 0.250 / 0.000806 = I_peak × 310.2 counts/A
ΔI per count = 0.000806 / 0.250 = 3.22 mA/count
I_max = 4095 × 3.22 mA = 13.2 A
```

These constants must be defined as named compile-time values. The implementer may use fixed-point arithmetic. Floating-point is not prohibited but is discouraged in ISR context.

---

## 7. Slope Compensation

### 7.1 Purpose

In peak current mode control at duty cycles above 50%, the system is susceptible to subharmonic oscillation. Slope compensation adds an artificial downward ramp to the comparator threshold within each switching period, ensuring stability regardless of duty cycle.

### 7.2 Stability Criterion

The compensation slope must exceed 50% of the inductor current downslope in the worst-case operating condition.

**Buck mode:** |dI/dt|_down = V_out / L. At V_out = 20 V, L = 4.7 µH: downslope = 4.26 A/µs. Minimum compensation slope = **2.13 A/µs**.

**Boost mode:** |dI/dt|_down = (V_out − V_in) / L. At V_out = 20 V, V_in = 5 V, L = 4.7 µH: downslope = 3.19 A/µs. Minimum compensation slope = 1.60 A/µs. The buck worst case governs.

Converting to DAC units: 2.13 A/µs × 310 counts/A = ~660 counts/µs minimum. At a timer ISR rate of 2 MHz (500 ns period): ~330 counts per tick.

### 7.3 Slope Value

**⚠ UNITS CLARIFICATION REQUIRED (OPEN:Q10):** Dan's initial values from the transcripts were "4,000 A/s" and "8,000 A/s." Physics requires the slope compensation to be in the A/µs (≡ MA/s) range. Dan's values are likely in different units (DAC counts per timer tick, or a normalized step value). **The implementer must verify the intended slope value with Dan before hardware testing.**

**Default for bring-up:** 4 MA/s (4,000,000 A/s) — conservative, exceeds 50% of worst-case downslope.

This value is a **runtime-mutable** parameter to support bring-up tuning. Writing a new slope value causes the step size to be recomputed and takes effect on the next switching period.

### 7.4 Behavior

At the start of each switching period (HRTIM period reset event), DAC3 CH1 is loaded with the **peak value** (Y-intercept from the PID). Over the course of the period, DAC3 CH1 decreases linearly. At any time t within a switching period:

```
DAC(t) = DAC_peak − floor(slope_a_per_s × DAC_COUNTS_PER_AMP × t)
```

The constraint: the DAC value at the instant the comparator fires must be within **±2 DAC counts** of the ideal linear ramp value.

### 7.5 Ramp Mechanism

**The mechanism for generating the ramp is intentional ambiguity.** The implementer may use any of:

1. **Timer ISR staircase + Transport machanism** (current implementation): A timer (e.g., TIM6) fires at a rate >> f_sw. Each ISR reads DAC3 CH1, subtracts a fixed precomputed step, writes it back. The step size must be precomputed once at init (or when slope/frequency changes) and stored as a constant — it must **not** be recomputed in the ISR. Possible use of DMA for low overhead transfrom of counter values to DAC.
2. **DMA from pre-computed table:** A table of DAC values for one period is pre-computed in RAM. DMA transfers one value per timer tick.
3. **DAC3 hardware sawtooth generator:** RM0440 §22.4.5. If the sawtooth reset can be triggered by the HRTIM period event, this eliminates all CPU involvement.

**CPU budget (Timer ISR approach):** At 2 MHz, the ISR fires every 500 ns on a 170 MHz core. Estimate 10–30 cycles ≈ 60–180 ns ≈ 12–36% CPU. Acceptable for v0.1.2 but motivates future migration.

### 7.6 Ramp Reset

At each HRTIM period reset, DAC3 CH1 must be reloaded with the current PID peak value. **The DAC must be reloaded before the blanking window expires.** If the DAC still holds the previous period's minimum value when blanking ends, the comparator will fire immediately and the charge phase will be zero-length.

---

## 8. Outer Control Loop: PID Voltage Regulation

### 8.1 Architecture

The outer loop is a discrete-time PID controller operating on voltage error. Its output is the peak current setpoint — the Y-intercept of the slope compensation ramp.

```
V_setpoint (from PD stack)
    │
    ▼
  error = V_setpoint − V_out_measured
    │
    ▼
  PID controller
    │
    ▼
  current-limit function (pass-through in v0.1.2;
    code must accommodate future current-limiting wrapper)
    │
    ▼
  I_peak_setpoint (DAC counts)
    │
    ▼
  DAC3 CH1 peak value (Y-intercept for slope ramp)
```

### 8.2 Execution Rate

- **Default:** 20 kHz (50 µs period, every 10th switching period at 200 kHz).
- **Maximum:** Equal to f_sw.
- **Minimum:** 1 kHz. Below this, transient response degrades unacceptably.
- **Jitter:** ±5% acceptable.

The PID executes in a dedicated hardware timer ISR — **not** an RTOS task. The PID timer is decoupled from the HRTIM. The delay between PID computation and the next DAC Y-intercept reset must not exceed 10 switching periods (50 µs at 200 kHz).

### 8.3 Timing Constraint

The PID timing is relaxed. The hardware inner loop provides cycle-by-cycle current regulation, so a delay of several switching periods between PID output update and its effect on the DAC peak value is acceptable. The PID output does not need to be synchronized to the HRTIM period.

### 8.4 PID Algorithm

Standard discrete PID with anti-windup:

```
error[n] = setpoint - measurement

P = Kp × error[n]
I = I_prev + Ki × error[n] × dt
D = Kd × (error[n] - error[n-1]) / dt

output = P + I + D

// Anti-windup: clamp integrator
if output > OUTPUT_MAX:
    I = I - (output - OUTPUT_MAX)
    output = OUTPUT_MAX
if output < OUTPUT_MIN:
    I = I - (output - OUTPUT_MIN)
    output = OUTPUT_MIN

I_prev = I
error[n-1] = error[n]
```

The specific anti-windup variant (conditional integration, back-calculation, etc.) is an implementation choice provided it prevents integrator windup when the output is saturated.

### 8.5 PID Input and Sampling

The PID input is V_out measured by ADC1_IN1 (PA0, VD_MON).

V_out should be sampled **synchronously** with respect to the switching period to avoid aliasing the output voltage ripple into the PID input. The recommended approach is HRTIM-triggered ADC injected conversion at a fixed phase within the switching period (e.g., midpoint of discharge).

If HRTIM-triggered injected conversion is not used (e.g., software-triggered), the implementer must implement averaging or filtering to attenuate switching-frequency ripple. The PID must not see raw switching ripple as a voltage error signal.

The spec does not prescribe the unit system (raw ADC counts, millivolts, or fixed-point intermediate). The implementer must choose one, ensure target and measurement are in the same unit, and document the choice.

V_in (PA4 / ADC2_IN17) is on a separate ADC instance (PA4 is not available on ADC1 on this package). Used for mode selection and telemetry. Does not need to be synchronous with switching.

### 8.6 PID Output

The PID output is in DAC counts (0–4095), representing the peak current threshold. Clamping bounds:

```c
#define PID_OUTPUT_MIN    0       /* DAC counts — zero current, regulator idle */
#define PID_OUTPUT_MAX    1550    /* DAC counts — 5 A peak */
```

**Rationale for 1550 counts / 5 A:** USB PD EPR specifies a maximum of 5 A. 5 A × 310 counts/A ≈ 1550 counts. Dan must confirm that the inductor saturation current and GaN FET absolute maximum both exceed 5 A with adequate margin — see **OPEN:Q7**. I_MAX must be a named constant in the centralized config header (§2.1).

The PID output is written to a shared variable that the slope compensation ramp reader consumes at the next period reset. This write is atomic (single 32-bit store to a `volatile uint32_t`). No mutex is required — 32-bit aligned writes are atomic on Cortex-M4.

### 8.7 PID Coefficients

Initial values are placeholders. Tuning is empirical and out of scope.

```c
float pid_kp = 1.0f;
float pid_ki = 100.0f;
float pid_kd = 0.0f;
```

These must be **runtime-modifiable** (not compile-time constants) to support tuning without reflashing.

### 8.8 PID Integrator Reset

The PID integrator (I_prev) must be reset to 0 on:
- Transition from IDLE to RUNNING
- Transition from FAULT to IDLE
- Operating mode change (buck ↔ boost)
- Setpoint change exceeding 20% of the previous setpoint

The 20% threshold prevents integrator windup during large PD voltage transitions (e.g., 5 V → 20 V = 300% step). It is a runtime-configurable parameter:

```c
uint8_t pid_integrator_reset_threshold_pct = 20;
```

---

## 9. PD ↔ Regulator Interface

The USB PD stack (X-CUBE-TCPP, FreeRTOS-managed) negotiates the output voltage with the connected sink device. The regulator firmware must receive the negotiated voltage and expose regulator status back to the PD stack.

### 9.1 Shared State

| Variable            | Direction      | Type                | Description                                  |
| ------------------- | -------------- | ------------------- | -------------------------------------------- |
| `target_voltage_mv` | PD → Regulator | `volatile uint32_t` | Negotiated output voltage in millivolts      |
| `regulator_ready`   | Regulator → PD | `volatile bool`     | True when output is within regulation window |
| `regulator_fault`   | Regulator → PD | `volatile bool`     | True when regulator is in fault state        |

Access to these variables does not require a mutex — they are written by one context and read by another, and are naturally atomic on a 32-bit ARM core for aligned types. The `volatile` qualifier is sufficient.

### 9.2 Function API

The PD stack must be able to set a target voltage, trigger mode changes, and shut down the regulator:

```c
void regulator_set_target_voltage(uint32_t voltage_mv);
void regulator_stop(void);
void regulator_clear_fault(void);
```

These functions write to shared state and/or set flags that the PID ISR reads on its next execution. They must be safe to call from any context (ISR or task) and must not block.

**Setpoint behavior:**
- Valid setpoint range: 5000–48000 mV (5 V to 48 V). A setpoint of 0 mV means "no voltage requested."
- If the regulator is in IDLE, a new setpoint does not start it automatically. An explicit enable command is still required.
- If the regulator is in RUNNING, the PID loop picks up the new setpoint on its next iteration.
- The regulator must transition to IDLE if it receives a 0 mV setpoint while RUNNING.
- If the setpoint change exceeds 20% of the previous setpoint, the PID integrator is reset (§8.8).

### 9.3 Voltage Transition Behavior

When the target voltage changes (e.g., 5 V → 20 V) while RUNNING:

- The PID setpoint is updated. The PID ramps to the new voltage naturally.
- If the transition requires a mode change (buck ↔ boost), the mode transition sequence in §5.1 executes.
- No special soft-start is needed for PDO transitions while already running (the PID loop provides controlled slewing).

**Transition acceptance criteria:**
- Voltage transitions must settle to within **±2% of the new target** within **100 ms**.
- During the transient, V_out must not overshoot above **110% of the new target**.
- During the transient, V_out must not undershoot below **80% of the old target**.
- These bounds are initial targets for tuning — if they prove infeasible, they may be relaxed with documented justification.

### 9.4 Source PDO Advertisement

| PDO Index | Voltage  | Max Current | Notes              |
| --------- | -------- | ----------- | ------------------ |
| 0         | 5 V      | 100 mA      | Default / safe     |
| 1         | Variable | 1500 mA     | Negotiated voltage |

The specific voltages advertised and the negotiation logic are handled by X-CUBE-TCPP.

---

## 10. Fault Handling

### 10.1 Hardware Faults (FLT1, FLT2)

When either fault input asserts (goes low), the HRTIM hardware autonomously forces all outputs to their fault state. **The required fault state is: all outputs LOW (all FETs off).** This must be configured in the HRTIM fault output state registers during post-init.

The firmware must also register an HRTIM fault interrupt to:
1. Set the system state to FAULT.
2. Set DAC3 CH1 to 0.
3. Disable the PID timer and slope compensation timer.
4. Log the fault source (FLT1, FLT2, or both) via LPUART1 if tracing is enabled.
5. Optionally report the fault to the PD stack via `regulator_fault`.

**The firmware must not automatically restart after a hardware fault.**

### 10.2 Software Safety Checks

The PID loop ISR must check the following conditions on every execution. If any condition is violated, the firmware must execute a controlled shutdown (disable HRTIM outputs, zero DAC, set FAULT state):

| Condition                    | Threshold                                  | Rationale                                                                    |
| ---------------------------- | ------------------------------------------ | ---------------------------------------------------------------------------- |
| V_out overvoltage (relative) | V_out_target × 1.10 (110% of setpoint)     | Output/load protection                                                       |
| V_out overvoltage (absolute) | 52.8 V hard cap, **compile-time constant** | 110% of USB PD EPR maximum (48 V × 1.10); hardware ceiling is 60 V (ADM1270) |
| V_out undervoltage           | V_out_target × 0.50 (50% of setpoint)      | Loss of regulation                                                           |
| V_in out-of-range (buck)     | V_in < V_out_target + margin               | Buck requires V_in > V_out                                                   |
| V_in out-of-range (boost)    | V_in > V_out_target − margin               | Boost requires V_in < V_out                                                  |

The relative thresholds and margin values are runtime-mutable tuning parameters. The 52.8 V absolute hard cap is a compile-time constant (not runtime-tunable) — it represents 110% of the USB PD EPR maximum voltage (48 V). The true hardware ceiling is the ADM1270 OVP at 60 V.

### 10.3 Maximum On-Time / Overcurrent Protection

If the comparator never fires during a switching period, the charge phase runs for the entire period and inductor current is unbounded.

**Required protection:** The HRTIM must have a hardware maximum duty cycle limit configured via a Compare Unit (Compare 1) on the active timer. Software-only backstops are **not acceptable** — the compare match fires within one HRTIM clock cycle (~184 ps); an ISR has microseconds of latency.

```c
#define MAX_DUTY_CYCLE_PCT   85    /* percent of period */
#define MAX_ON_TIME_COUNTS   (HRTIM_PERIOD_COUNTS * MAX_DUTY_CYCLE_PCT / 100)
/* At 200 kHz: 0.85 × 27200 = 23120 counts */
```

**Rationale for 85%:** At 200 kHz, 85% duty leaves 4,080 counts ≈ 750 ns before period end. This provides margin for the discharge phase and the bootstrap refresh window (≥200 ns).

**Timing constraint:** `backstop_counts ≤ period_counts − (bootstrap_refresh_ns / tick_ps)`. At 200 kHz with 200 ns refresh: backstop ≤ 27200 − 1088 = 26112 (96.0%). The 85% default satisfies this.

**CubeMX update required:** The IOC currently shows CompareUnit1 as `__NULL`. The CubeMX project must be updated to configure Timer A Compare 1 (and Timer B Compare 1, for boost mode) for this backstop.

### 10.4 Maximum On-Time Violation Counting

The firmware must count consecutive backstop events per switching period. A single backstop event is a transient condition (e.g., momentary load step). Multiple consecutive events indicate a persistent problem.

If more than **3** consecutive periods hit the backstop without COMP1 firing, this is a fault condition → controlled shutdown → FAULT state. The threshold of 3 is a runtime-mutable parameter:

```c
uint8_t max_consecutive_backstops = 3;
```

The counter resets to 0 any time COMP1 fires normally during a period.

---

## 11. Startup, Shutdown, and Soft-Start

### 11.1 Startup Sequence

On system boot, after CubeMX-generated initialization completes:

1. FreeRTOS scheduler starts. Default task initializes USB PD stack (`MX_USBPD_Init()`, `MX_TCPP_Init()`).
2. PD interface is initialized (registers shared state with PD stack).
3. Regulator initialization executes:
   a. Configure HRTIM Set/Reset sources per §5.5.
   b. Configure backstop Compare 1 registers per §10.3.
   c. Set DAC3 CH1 to 0 (zero current setpoint — safe state).
   d. Precompute slope compensation step size.
   e. Configure slope compensation timer (stopped).
   f. Configure PID state (zero integrator, load default coefficients).
   g. Configure the non-switching leg for default mode (buck: Timer B = wire to V_out).
   h. Enable COMP1.
   i. Configure ADC sampling.
   j. Start ADC1 and ADC2.
   k. Enable HRTIM fault inputs.
   l. Verify no active fault.
   m. Transition to IDLE. **Do not start switching yet.**
4. When a target voltage is set (PD negotiation or debug command) and an enable command is received:
   a. `regulator_set_target_voltage()` updates PID setpoint.
   b. Enable HRTIM Timer A and Timer B outputs (switching begins), start slope compensation timer, start PID timer.
   c. Perform soft-start (§11.2).

### 11.2 Soft-Start

**Note:** Soft-start was not discussed in transcripts. It is included as standard SMPS practice. **OPEN:Q12** — Dan to confirm this feature is desired and provide ramp time requirements.

When the regulator transitions from IDLE to RUNNING (initial enable or after mode change):

1. Enable HRTIM switching outputs.
2. Enable slope compensation timer.
3. Gradually increase the PID setpoint from zero to the target value over a configurable ramp time (order of milliseconds). During soft-start, the PID integrator is held at zero — only the proportional ramp drives the setpoint.
4. Enable full PID closed-loop control when the ramp completes.

Soft-start prevents inrush current and output voltage overshoot on initial enable. The ramp rate and duration are compile-time constants.

### 11.3 Shutdown

`regulator_stop()` must:

1. Disable HRTIM switching outputs (all FETs off).
2. Set DAC3 CH1 to 0.
3. Disable slope compensation timer.
4. Disable PID timer.
5. Transition to IDLE.

---

## 12. Timing Sequence Within One Switching Period

```
Time →

|←————————— T_sw (one switching period) ——————————→|
|                                                    |
|  Period   Blanking                    EEV4         |
|  Reset    Window     Charge Phase    (COMP1 trips) |
|  ↓        ↓←————→↓                   ↓             |
|  ┌────────┬──────┬───────────────────┐              |
|  │ DAC    │BLANK │  Switching leg    │ Switching    |
|  │ reset  │      │  HIGH            │ leg LOW      |
|  │ to PID │      │  Inductor charges │ Discharge    |
|  │ output │      │  IL ramps up      │ IL ramps down|
|  └────────┴──────┴───────────────────┘              |
|                                                    |
|  DAC ramp: PID_output → PID_output − (slope × t)  |
|  COMP1 trips when IL_MON ≥ DAC value               |
```

1. **Period reset** (t = 0): HRTIM counter wraps. Switching leg output goes HIGH (charge phase begins). DAC3 CH1 is loaded with the PID output value (Y-intercept).
2. **Blanking window** (t = 0 to t_blank): COMP1 output is masked to prevent false triggers from switching transient ringing. Duration: 100 ns default, compile-time configurable (§2.1, §5.5).
3. **Charge phase** (t_blank to t_trip): Inductor current ramps up. DAC value ramps down (slope compensation). COMP1 monitors IL_MON vs. DAC3 CH1.
4. **COMP1 trip** (t = t_trip): Inductor current meets the slope-compensated threshold. EEV4 fires. Switching leg output goes LOW.
5. **Discharge phase** (t_trip to T_sw): Inductor current ramps down. Switching leg at GND.
6. **Bootstrap refresh** (near t = T_sw): Non-switching leg's switch node briefly brought to GND for ≥200 ns, ≤500 ns.

---

## 13. FreeRTOS Integration

### 13.1 Role

FreeRTOS is present exclusively for the USB PD stack. The regulator control loop does not use FreeRTOS primitives and must not be blocked by FreeRTOS scheduling.

### 13.2 Interrupt Priority Partitioning

FreeRTOS `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` = 3. This means:

- **Priorities 0–2:** Above FreeRTOS. Cannot use FreeRTOS API calls. Cannot be masked by FreeRTOS critical sections.
- **Priorities 3–15:** FreeRTOS-managed. Can use `FromISR` API variants.

**Required assignment:**

| Interrupt                       | Priority | Rationale                     |
| ------------------------------- | -------- | ----------------------------- |
| Slope compensation timer (TIM6) | 0        | Most latency-sensitive        |
| HRTIM period/fault              | 1        | Must never be delayed by RTOS |
| PID timer                       | 2        | Below HRTIM, above RTOS       |
| ADC1/ADC2 completion            | 2        | Paired with PID               |
| UCPD1 / PD stack                | ≥ 3      | RTOS-managed                  |
| LPUART1                         | 6        | Debug, lowest priority        |

All regulator ISRs must have NVIC priority numerically less than 3. No FreeRTOS API calls from regulator control ISRs.

---

## 14. ADC Measurement Subsystem

### 14.1 Channels

| Channel         | Signal      | Purpose          | Peripheral | Sampling Time |
| --------------- | ----------- | ---------------- | ---------- | ------------- |
| ADC1_IN1 (PA0)  | VD_MON      | V_out voltage    | ADC1       | 2.5 cycles    |
| ADC1_IN2 (PA1)  | IL_MON      | Inductor current | ADC1       | 2.5 cycles    |
| ADC1_IN7 (PC1)  | **OPEN:Q2** | I_out or I_in    | ADC1       | 2.5 cycles    |
| ADC1_IN15 (PB0) | **OPEN:Q2** | I_out or I_in    | ADC1       | 2.5 cycles    |
| ADC2_IN17 (PA4) | VS_MON      | V_in voltage     | ADC2       | 2.5 cycles    |

### 14.2 Scaling Constants

All scaling is from 12-bit ADC raw counts to physical units (millivolts or milliamps).

**Voltage channels (VD_MON, VS_MON):**
```
V_mV = (uint32_t)ADC_raw * 60000u / 4096u     /* ≈ 14.65 mV/count */
```

**Inductor current (IL_MON):**
```
I_mA = (uint32_t)ADC_raw * 13200u / 4096u     /* ≈ 3.22 mA/count */
```

**Output current (ID_MON):**
```
I_mA = (uint32_t)ADC_raw * 5500u / 4096u      /* ≈ 1.34 mA/count */
```

**Input current (IS_MON):**
R25 = 5 mΩ (confirmed, see §3.3). Gain = 50 V/V. Sensitivity = 250 mV/A (same as IL_MON).
```
I_mA = (uint32_t)ADC_raw * 13200u / 4096u     /* ≈ 3.22 mA/count */
```

---

## 15. Diagnostic / Debug Interface

The firmware must expose telemetry via LPUART1 (921600 baud, 7-bit word, matching `final.ioc`).

### 15.1 Required Telemetry (Read-Only)

| Value                       | Type     | Units       | Update Rate            |
| --------------------------- | -------- | ----------- | ---------------------- |
| V_in measured               | uint32_t | mV          | Every PID cycle        |
| V_out measured              | uint32_t | mV          | Every PID cycle        |
| I_inductor measured         | uint32_t | mA          | Every PID cycle        |
| I_out measured              | uint32_t | mA          | Every PID cycle        |
| I_in measured               | uint32_t | mA          | Every PID cycle        |
| PID output (DAC counts)     | uint16_t | counts      | Every PID cycle        |
| PID error                   | int32_t  | mV          | Every PID cycle        |
| PID setpoint                | uint16_t | mV          | On change              |
| Current duty cycle          | uint16_t | HRTIM ticks | Every switching period |
| System state                | enum     | —           | On change              |
| Operating mode (BUCK/BOOST) | enum     | —           | On change              |
| Fault status                | bitfield | —           | On change              |

### 15.2 Required Configurable Parameters

| Parameter                           | Type     | Default | Range       | Hot-Swappable?                 |
| ----------------------------------- | -------- | ------- | ----------- | ------------------------------ |
| Switching frequency (period counts) | uint16_t | 27200   | 5440–54400  | No (requires restart)          |
| PID Kp                              | float    | 1.0     | 0.0–100.0   | Yes                            |
| PID Ki                              | float    | 100.0   | 0.0–10000.0 | Yes                            |
| PID Kd                              | float    | 0.0     | 0.0–100.0   | Yes                            |
| PID execution rate (Hz)             | uint32_t | 20000   | 1000–500000 | No                             |
| Slope compensation (A/s)            | uint32_t | 4000000 | 0–10000000  | Yes                            |
| Voltage setpoint (mV)               | uint16_t | 0       | 0–48000     | Yes                            |
| Max duty cycle (%)                  | uint8_t  | 85      | 50–96       | Yes                            |
| Max consecutive backstops           | uint8_t  | 3       | 1–10        | Yes                            |
| Operating mode                      | enum     | BUCK    | BUCK, BOOST | Yes (triggers mode transition) |
| OVP threshold (% of target)         | uint8_t  | 110     | 105–150     | Yes                            |
| UVP threshold (% of target)         | uint8_t  | 50      | 20–90       | Yes                            |
| Integrator reset threshold (%)      | uint8_t  | 20      | 5–100       | Yes                            |

The diagnostic output format and transport mechanism for configuration writes are not specified (implementation choice). The diagnostic output must be parseable and must not interfere with regulator timing (use DMA for UART TX, or buffer and transmit in low-priority context).

---

## 16. Definition of Done

Each item is binary — it passes or it does not.

### Peripheral Configuration
- [ ] CubeMX project generates and compiles cleanly for STM32G474RETx.
- [ ] Post-init configuration completes: HRTIM Set/Reset sources, backstop Compare 1, DAC3 CH1 = 0, COMP1 started, ADCs started.
- [ ] System enters IDLE state after initialization.
- [ ] HRTIM1 Timer A outputs complementary PWM at 200 kHz on PA8/PA9 with dead time.
- [ ] HRTIM1 Timer B outputs complementary signal on PA10/PA11.
- [ ] COMP1 configured: PA1 (+), DAC3_OUT1 (−); output routes to EEV4.
- [ ] EEV4 terminates the switching leg's charge phase when COMP1 fires.
- [ ] FLT1 and FLT2 force all HRTIM outputs to safe state when asserted.

### State Machine
- [ ] System transitions IDLE → RUNNING on enable command with valid setpoint.
- [ ] System transitions RUNNING → FAULT on FLT1 or FLT2 assertion.
- [ ] System transitions RUNNING → IDLE on disable command or 0 mV setpoint.
- [ ] System transitions FAULT → IDLE on fault-clear command (only after hardware fault deasserted).
- [ ] FAULT state is latched — system does not return to RUNNING without passing through IDLE.
- [ ] All HRTIM outputs are LOW (all FETs off) in IDLE and FAULT states.

### Buck Mode
- [ ] Timer A switches; Timer B holds high-side on.
- [ ] Charge phase begins at period reset (CHA1 goes HIGH).
- [ ] Charge phase terminates when IL_MON exceeds DAC3 CH1 (COMP1 → EEV4 → CHA1 goes LOW).
- [ ] With V_in = 30 V (bench supply), V_out regulates to within ±2% of setpoint at each target (5 V, 12 V, 20 V) under: (a) no-load, (b) 500 mA, (c) 1500 mA.
- [ ] Peak current mode is hardware-autonomous: fixing DAC to a constant (removing PID) produces a fixed-duty-cycle output.

### Boost Mode
- [ ] Timer B switches; Timer A holds high-side on.
- [ ] Charge phase begins at period reset (CHB1 goes LOW, CHB2 goes HIGH).
- [ ] Charge phase terminates when IL_MON exceeds DAC3 CH1 (COMP1 → EEV4 → CHB1 goes HIGH).
- [ ] With V_in = 12 V, V_out = 20 V regulates to within ±2% under: (a) no-load, (b) 500 mA.
- [ ] Mode switch from buck to boost completes via disable → reconfigure → soft-start without power stage damage.

### Slope Compensation
- [ ] DAC3 CH1 resets to PID peak value at each period start.
- [ ] DAC3 CH1 decreases monotonically within each period.
- [ ] DAC3 CH1 value at any point within a period is within ±2 counts of ideal linear ramp.
- [ ] Slope value is runtime-mutable without stopping the regulator.

### PID
- [ ] PID executes at configured rate (default 20 kHz).
- [ ] PID output is clamped to [0, PID_OUTPUT_MAX].
- [ ] PID integrator resets on state transitions, mode changes, and >20% setpoint changes.
- [ ] PID coefficients are changeable at runtime without restarting.
- [ ] Anti-windup prevents integrator accumulation when output is saturated.

### Voltage Transitions
- [ ] Voltage transitions settle to within ±2% of new target within 100 ms.
- [ ] Overshoot does not exceed 110% of new target during transitions.
- [ ] Undershoot does not drop below 80% of old target during transitions.

### Startup
- [ ] Soft-start ramps output voltage from 0 to target without overshoot > 5%.
- [ ] Soft-start executes on initial enable and after mode changes.

### Safety
- [ ] Hardware backstop compare register prevents charge phase exceeding 85% of period.
- [ ] Software OVP triggers FAULT at V_out > 110% of setpoint or V_out > 52.8 V absolute.
- [ ] Software UVP triggers FAULT at V_out < 50% of setpoint.
- [ ] V_in validation triggers FAULT in both buck and boost modes per §10.2.
- [ ] 3 consecutive backstop violations → FAULT.
- [ ] Hardware fault (FLT1/FLT2) forces all outputs off within one HRTIM clock cycle.
- [ ] Bootstrap refresh pulse occurs on the static leg, ≥200 ns, ≤500 ns, at end of period.

### FreeRTOS and PD
- [ ] Regulator control interrupts (slope comp, HRTIM, PID) are at priorities 0–2.
- [ ] PD stack interrupts are at priority ≥ 3.
- [ ] Voltage setpoint updates from PD stack are visible to PID within one PID period.
- [ ] No FreeRTOS API calls from regulator control ISRs.
- [ ] Connecting a PD sink that requests 20 V causes regulation to 20 V output.
- [ ] PD stack operates normally while regulator is running.

### Telemetry
- [ ] All five analog measurements (V_in, V_out, I_L, I_out, I_in) are readable.
- [ ] Voltage readings are accurate to ±1% across 0–30 V range.
- [ ] Current readings are accurate to ±5% across 0–5 A range.
- [ ] LPUART1 outputs diagnostic data matching scope measurements within ADC resolution.

### Code Quality [Jonah-prescribed]
- [ ] All hardware constants are defined in the centralized config header (§2.1), not scattered across source files.
- [ ] All pin mappings include doc-comments per §2.1 (pin name, port, net label, physical connection, direction).
- [ ] All compile-time configurable constants include doc-comments per §2.1 (name, units, valid range, derivation reference).
- [ ] All conversion constants are derived from §3 hardware parameters with derivation documented.
- [ ] Compile-time static assertions validate configurable parameter ranges where supported by toolchain.
- [ ] Switching frequency is a compile-time parameter.
- [ ] Code compiles with `-Wall -Werror` (or equivalent strict warnings).

---

## Appendix A: Open Questions (Spec Defects)

Questions tagged `[Jonah]` were answered by Jonah. Questions tagged `[Dan needed]` require Dan's input. Questions tagged `[Tuning]` are resolved empirically during bring-up.

| ID  | Question                                                                        | Status                                                                   | Resolution / Assumption                                                                                                                                                                                                                              | Source                                 |
| --- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------- |
| Q1  | HRTIM Timer A SetSource = Period, ResetSource = EEV4?                           | **Blocking — [Dan needed]**                                              | Assumed yes per §5.5. Software must define Set/Reset sources in the centralized pin mapping header (§2.1) so they are visible and auditable. Dan to confirm the intended configuration (e.g., by sharing `MX_HRTIM1_Init()` or verbally confirming). | Circumstantial from IOC                |
| Q2  | ADC1 IN7 (PC1) and IN15 (PB0) — which is IS_MON, which is ID_MON?               | **Blocking for telemetry — [Dan needed]**                                | Non-blocking for core regulation (PID uses only V_out and IL_MON). Dan's header file resolves this. Pin mapping must be defined in centralized header per §2.1.                                                                                      | IOC ambiguity                          |
| Q3  | Input shunt R25 = 2 mΩ (schematic) or 5 mΩ (transcript)?                        | **Resolved — [Jonah]**                                                   | **5 mΩ.** Jonah was present when Dan read the value off the resistor code on the physical shunt. Schematic is stale. IS_MON sensitivity = 5 mΩ × 50 V/V = 250 mV/A.                                                                                  | Jonah physical inspection              |
| Q4  | DAC3 CH2 purpose — reserved or connected to something?                          | Non-blocking — [Dan needed]                                              | Assumed reserved. Firmware must not use without explicit specification.                                                                                                                                                                              | IOC shows internal config, no consumer |
| Q5  | FLT2 (PA15) — what is it connected to?                                          | Non-blocking — [Dan needed]                                              | Treated identically to FLT1 in firmware. Dan to trace physical connection.                                                                                                                                                                           | IOC confirms active-low config         |
| Q6  | Blanking window duration                                                        | **Resolved — [Jonah]**                                                   | Compile-time constant, default 100 ns. Adjustable in centralized config header per §2.1 within range 100–500 ns. Empirical tuning via scope.                                                                                                         | Standard SMPS practice                 |
| Q7  | Maximum allowable inductor peak current                                         | Partially resolved — [Jonah], **[Dan needed]** for absolute confirmation | Engineering estimate: 5 A default aligns with USB PD EPR spec maximum (48 V × 5 A = 240 W). Dan to confirm inductor saturation current and FET abs max ratings. DAC_MAX must be recomputed if I_max changes.                                         | USB PD 3.1 EPR spec                    |
| Q8  | Software safety check thresholds                                                | [Tuning]                                                                 | Starting values in §10.2. Refined during bring-up.                                                                                                                                                                                                   | —                                      |
| Q9  | PID coefficients                                                                | [Tuning]                                                                 | Placeholders in §8.7. Refined empirically.                                                                                                                                                                                                           | —                                      |
| Q10 | Slope compensation units — Dan said "4,000 A/s" but physics requires MA/s range | **Blocking — [Dan needed]**                                              | Assumed 4 MA/s per physics. Dan's "4,000" and "8,000" values likely in different units (DAC counts/tick, or a normalized step). Must verify with Dan before hardware testing. [Jonah concurs this needs Dan's input.]                                | Transcript vs physics                  |
| Q11 | PLL source — IOC math says HSI but HSE pins are configured                      | **Resolved — [Jonah, agent-assisted]**                                   | **HSI confirmed.** 16 MHz / 4 = 4 MHz VCO input × 85 = 340 MHz VCO / 2 = 170 MHz SYSCLK. HSE pins (PF0/PF1) are configured as `RCC_OSC_IN`/`RCC_OSC_OUT` in IOC but oscillator is not enabled — HSI is sole source.                                  | Agent code exploration + IOC           |
| Q12 | Soft-start feature and ramp time requirements                                   | **[Dan needed]**                                                         | Spec assumes soft-start is required (standard SMPS practice) with "order of milliseconds" ramp time, but neither the feature nor parameters were discussed in transcripts. Dan to confirm feature is desired and provide target ramp time.           | Spec-added, §11.2                      |

---

## Appendix B: Future Scope (Not Specified, Must Not Be Precluded)

1. **Four-switch buck-boost mode.** Both timers switch per period (see LT8390 datasheet page 14).
2. **Dynamic switching frequency.** Increase f_sw when duty cycle is small to maintain CCM.
3. **Hardware slope compensation.** DAC3 sawtooth generator or DMA-based for zero CPU overhead.
4. **Current-limiting outer loop.** A second PID or threshold limiting inductor/output current.
5. **Adaptive slope compensation.** Slope as a function of V_in, V_out, and operating mode.
6. **Bootstrap refresh optimization.** Refresh every N periods instead of every period.

---

## Appendix C: Design Rationale

**Why peak current mode control?** Inherent cycle-by-cycle current limiting, faster transient response than voltage mode, eliminates need for firmware duty cycle computation. The tradeoff is slope compensation at duty > 50%. The STM32G474's COMP + HRTIM event path makes this natural.

**Why a software PID for the outer loop?** The inner hardware loop handles cycle-by-cycle dynamics. The outer loop only adjusts the current setpoint slowly (relative to f_sw). A multi-period delay is acceptable because the inner loop maintains stability independently.

**Why Timer ISR for slope compensation (this version)?** Simplest approach proven to work. CPU cost (~12–36% at 2 MHz) is acceptable for bring-up. Premature optimization risks new failure modes before the control loop is tuned. Architecture permits migration to DAC sawtooth or DMA without structural changes.

**Why not FreeRTOS for the control loop?** PID executes every 50 µs. Slope comp ISR every 100–500 ns. FreeRTOS task switch overhead (several µs) and priority inversion risk are unacceptable.

**Why bootstrap refresh every period?** Wastes ~4% of period energy but guarantees no UV lockout. Optimization to every-N is straightforward to add.

**Why 85% max duty and not 95%?** At 200 kHz, 95% leaves ~250 ns — barely enough for 200 ns bootstrap plus register update latency. 85% leaves ~750 ns with comfortable margin.

**Why target-relative OVP with 52.8 V absolute cap?** Relative thresholds catch failures at every operating point (a 5 V target at 7 V is 40% overshoot). The 52.8 V cap (110% of USB PD EPR maximum 48 V) provides a hard software safety limit independent of software state, below the 60 V hardware OVP (ADM1270). This accommodates both SPR (≤20 V) and EPR (≤48 V) PDO ranges without firmware changes.

**Why reset PID integrator on >20% setpoint change?** USB PD transitions include 5 V → 20 V (300% step). Integrator state from 5 V is inappropriate for 20 V and causes overshoot. The threshold catches all standard PD transitions.

**Why soft-start?** Standard SMPS practice. Prevents inrush current surge into the output capacitor bank and load, which can cause voltage overshoot and stress the GaN FETs. Also required after mode changes when the switching topology is reconfigured.

**Why separate ADC instances for V_in and V_out?** PA4 is not available on ADC1 on this package. V_in requires ADC2.

**Why hardware-only backstop?** At 200 kHz, the HRTIM period is 5 µs. A software ISR has 1–5 µs entry latency on Cortex-M4. The hardware compare fires in ~184 ps. For overcurrent protection, sub-nanosecond response is not optional.

---

## Appendix D: Scaling Reference Table

V_REF = 3.3 V, ADC = 12-bit (4096 counts), DAC = 12-bit (4096 counts).

| Signal         | Shunt/Divider      | Amp/Gain | Sensitivity | Physical/count | Max Readable |
| -------------- | ------------------ | -------- | ----------- | -------------- | ------------ |
| V_out (VD_MON) | 100k/5.82k divider | —        | 55.0 mV/V   | 14.65 mV       | 60.0 V       |
| V_in (VS_MON)  | 100k/5.82k divider | —        | 55.0 mV/V   | 14.65 mV       | 60.0 V       |
| I_L (IL_MON)   | 5 mΩ               | 50 V/V   | 250 mV/A    | 3.22 mA        | 13.2 A       |
| I_out (ID_MON) | 12 mΩ              | 50 V/V   | 600 mV/A    | 1.34 mA        | 5.5 A        |
| I_in (IS_MON)  | 5 mΩ               | 50 V/V   | 250 mV/A    | 3.22 mA        | 13.2 A       |
| DAC → Current  | —                  | —        | 250 mV/A    | 3.22 mA/count  | 13.2 A       |

---

## Appendix E: Derived Electrical Parameters

| Parameter                                    | Value         | Derivation              |
| -------------------------------------------- | ------------- | ----------------------- |
| Buck dI/dt (charge, 30 V → 5 V)              | 5.32 A/µs     | (30 − 5) / 4.7e-6       |
| Buck dI/dt (charge, 30 V → 20 V)             | 2.13 A/µs     | (30 − 20) / 4.7e-6      |
| Buck dI/dt (discharge, 5 V out)              | −1.06 A/µs    | −5 / 4.7e-6             |
| Buck dI/dt (discharge, 20 V out)             | −4.26 A/µs    | −20 / 4.7e-6            |
| Boost dI/dt (charge, V_in = 12 V)            | 2.55 A/µs     | 12 / 4.7e-6             |
| Boost dI/dt (discharge, 12 V → 20 V)         | −1.70 A/µs    | (12 − 20) / 4.7e-6      |
| Min slope compensation (buck worst case)     | 2.13 A/µs     | ½ × 4.26 A/µs           |
| V_out ADC counts at 5 V                      | ~341          | 5 × 0.05500 / 0.000806  |
| V_out ADC counts at 20 V                     | ~1365         | 20 × 0.05500 / 0.000806 |
| Peak current DAC for 5 A (I_max default)     | ~1550         | 5 × 310 counts/A        |
| Peak current DAC for 10 A (hardware ceiling) | ~3103         | 10 × 0.250 / 3.3 × 4096 |
| HRTIM period at 200 kHz                      | 27,200 counts | 170e6 × 32 / 200e3      |
| HRTIM period at 500 kHz                      | 10,880 counts | 170e6 × 32 / 500e3      |
| Backstop at 85% (200 kHz)                    | 23,120 counts | 0.85 × 27,200           |

---

## Appendix F: Divergence Resolution Log (v0.1.1-ab vs v0.1.1-bd)

| #   | Topic                     | -ab Position                     | -bd Position                     | Resolution                                     | Rationale                                                                                             |
| --- | ------------------------- | -------------------------------- | -------------------------------- | ---------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| 1   | U6/U7/U8 mapping          | U6=input, U7=inductor, U8=output | U6=inductor, U7=output, U8=input | **-ab**                                        | U6 is in input.kicad_sch (input sub-sheet)                                                            |
| 2   | HRTIM period              | 27,200 exact                     | 27,174 approx                    | **-ab**                                        | Exact integer: 170e6×32/200e3                                                                         |
| 3   | PLL source                | HSI 16 MHz                       | HSE 24 MHz                       | **-ab** (confirmed)                            | HSI verified: 16/4=4 MHz VCO in, ×85=340, /2=170 MHz. HSE pins configured but oscillator not enabled. |
| 4   | PD interface              | Callback                         | Shared volatile + function API   | **-bd** (expanded)                             | Bidirectional state (ready/fault) more complete                                                       |
| 5   | Soft-start                | Absent                           | Present                          | **-bd**                                        | Standard SMPS practice                                                                                |
| 6   | Overshoot limit           | ≤20%                             | ≤10%                             | **-bd** (10%)                                  | Tighter is standard; relaxable with justification                                                     |
| 7   | Undershoot limit          | ≤80% old target                  | Absent                           | **-ab**                                        | More complete transient spec                                                                          |
| 8   | Integrator reset >20%     | Yes                              | No (anti-windup only)            | **-ab** (per user decision)                    | Proactive for 5V→20V transitions                                                                      |
| 9   | Backstop → fault          | 3 consecutive                    | Immediate                        | **-ab** (per user decision)                    | Tolerant of transients, runtime-configurable                                                          |
| 10  | Absolute OVP              | Absent                           | 25 V (SPR only)                  | **Modified** — 52.8 V (110% × 48 V EPR max)    | EPR extends to 48 V; 25 V forecloses EPR; hardware ceiling is 60 V                                    |
| 11  | Module structure          | Prescribed                       | Not prescribed                   | Behavior-first; advisory boundaries (per user) | NLSpec philosophy                                                                                     |
| 12  | ADC sampling              | Software OK                      | Sync recommended                 | **-bd**                                        | Prevents ripple aliasing                                                                              |
| 13  | I2C1 purpose              | "Not used"                       | TCPP comms                       | **-bd**                                        | Confirmed by middleware config                                                                        |
| 14  | State machine             | 4-state formal                   | Informal                         | **-ab**                                        | Reduces ambiguity                                                                                     |
| 15  | V_in check (boost)        | Buck only                        | Both modes                       | **-bd**                                        | Spec covers both modes                                                                                |
| 16  | Mode change soft-start    | No                               | Yes                              | **-bd**                                        | Prevents inrush after topology change                                                                 |
| 17  | DoD test currents         | 3 A                              | 1500 mA (PDO max)                | **-bd**                                        | Matches contractual PDO limit                                                                         |
| 18  | FET part number           | EPC2302                          | Not specified                    | Omitted                                        | Unverifiable from project files                                                                       |
| 19  | Bootstrap refresh physics | Analyzed (dI/dt per mode)        | "must not corrupt waveform"      | **-ab** analysis in spec body                  | Implementer needs the physics                                                                         |

---

## Appendix G: Recommended Module Boundaries

> **This appendix is advisory.** The normative spec is §1–§16 and the Definition of Done (§16). These module boundaries are a suggested organizational structure. They may be adopted, modified, or ignored without affecting spec compliance.

**Regulator Control:** Owns the state machine (INIT → IDLE → RUNNING → FAULT), PID state, slope compensation state. Entry points: `regulator_init()`, `regulator_start()`, `regulator_stop()`, `regulator_set_target_voltage()`, `regulator_get_status()`, `regulator_clear_fault()`.

**Hardware Parameters:** Defines all hardware-derived constants from §3 as named constants or inline functions. Provides conversion functions between physical units and ADC/DAC counts. All constants derived from §3 with derivation in comments.

**PD Interface:** Bridges the PD stack and regulator. Initializes shared state. Calls `regulator_set_target_voltage()` when the negotiated voltage changes.

**ISR Ownership (suggested):**

| ISR                             | Owner               | Priority | Purpose                   |
| ------------------------------- | ------------------- | -------- | ------------------------- |
| Slope compensation timer (TIM6) | Regulator           | 0        | Decrement DAC3 CH1        |
| HRTIM period                    | Regulator           | 1        | DAC Y-intercept reset     |
| HRTIM fault                     | Regulator           | 1        | FLT1/FLT2 handling        |
| HRTIM compare backstop          | Regulator           | 1        | Max on-time counting      |
| PID timer (TIM7 or similar)     | Regulator           | 2        | PID computation, ADC read |
| UCPD1                           | PD stack (FreeRTOS) | ≥ 3      | PD protocol               |
| DMA1 CH2/CH4                    | PD stack (FreeRTOS) | ≥ 3      | UCPD DMA                  |

---

## Appendix H: Component Cross-Reference

| Ref          | Part                       | Role                       | Datasheet Note                                     |
| ------------ | -------------------------- | -------------------------- | -------------------------------------------------- |
| U (MCU)      | STM32G474xE                | Microcontroller            | RM0440 for peripheral details                      |
| Q1–Q4        | Enhancement-mode GaN FETs  | H-bridge                   | Bootstrap gate drive required                      |
| U1, U3       | uP1966E                    | Half-bridge gate drivers   | Bootstrap hold ~10 µs, UVLO safety                 |
| U6           | INA293A2 (schematic value) | Input current sense amp    | 50 V/V; see §3.3 note on lib_id contradiction      |
| U7           | INA281A2                   | Inductor current sense amp | 50 V/V, −0.3 to +50 V CM                           |
| U8           | INA281A2                   | Output current sense amp   | 50 V/V, −0.3 to +50 V CM                           |
| L1           | 4.7 µH                     | Power inductor             | Must support >5 A saturation with margin (OPEN:Q7) |
| R5           | 5 mΩ                       | Inductor current shunt     |                                                    |
| R6           | 12 mΩ                      | Output current shunt       |                                                    |
| R25          | 5 mΩ                       | Input current shunt        | Confirmed by physical inspection [Jonah]           |
| (protection) | ADM1270ACPZ                | Input hot-swap / OVP       | ~FAULT → FLT1 (likely)                             |

---

## Appendix I: Source Document Index

| Document                   | Path (relative to project root)        | Content                                        |
| -------------------------- | -------------------------------------- | ---------------------------------------------- |
| CubeMX IOC file            | `docs/final.ioc.txt`                   | Peripheral config, pin assignments, clock tree |
| Round 1 questions          | `docs/buck-boost-round1-questions.md`  | Initial clarification questions                |
| Round 1 answers            | `docs/buck-boost-round1-answers.md`    | Dan's answers to round 1                       |
| Round 2 questions          | `docs/buck-boost-round2-questions.md`  | Follow-up questions                            |
| Round 2a debrief           | `docs/buck-boost-round2a-debrief.md`   | Switching states, EEV4, bootstrap, slope comp  |
| Round 2b questions         | `docs/buck-boost-round2b-questions.md` | Remaining open questions                       |
| Round 2b answers           | `docs/buck-boost-round2b-answers.md`   | Resolution of round 2b questions               |
| System overview transcript | `docs/buck-boost-transcript.md`        | Initial system architecture discussion         |
| DC-DC schematic            | `docs/dc-dc.kicad_sch.txt`             | H-bridge, gate drivers, shunts, CSAs           |
| Input protection schematic | `docs/input.kicad_sch.txt`             | ADM1270, R25, U6 (INA293A2)                    |
| Output schematic           | `docs/output.kicad_sch.txt`            | Output filtering, voltage dividers             |
| PD Charger top-level       | `docs/PD_Charger.kicad_sch.txt`        | Hierarchical connections, MCU pin mapping      |
| STM32G474 datasheet        | `docs/stm32g474x_BCE_.pdf`             | Pin alt functions, electrical specs            |
| Divergence analysis        | `docs/divergence-analysis.md`          | v0.1.1-ab vs v0.1.1-bd analysis                |
