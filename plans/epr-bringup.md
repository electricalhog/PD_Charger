# USB-C PD / EPR bring-up (side branch of agentic bring-up)

Branch: `claude/usb-c-epr-testing-9bt5lo`, which is `agentic-bringup-tools` plus a merge of
`jonah-bench` (the bench-run regulator firmware) plus the work below. Nothing here has run on
hardware yet. Every bench step below is a proposal for you to review.

## Assumptions (correct any that are wrong before flashing)

1. The Type-C receptacle's CC1/CC2 go straight to the NUCLEO-G474RE UCPD pins (PB6 = CC1,
   PB4 = CC2). There is no TCPP03 and no X-NUCLEO-SRC1M1 shield.
2. The receptacle's VBUS is this board's regulator output **after** the output switch
   (Q5/Q6, `OUTPUT_EN` = PC8, `OUTPUT_DIS` = PC9). The "transistors for VSource" are that switch.
3. VBUS after the switch has no ADC channel. VD_MON measures the regulator output before the
   switch.
4. No VCONN switch is fitted. The G474 UCPD cannot source VCONN on its own.
5. The laptop is an SPR sink (≤ 20 V). Only a few laptops accept EPR (28–48 V) at all.

## Findings that block any USB-C testing on `jonah-bench` as it is

| # | Problem | Consequence | Fixed here |
|---|---|---|---|
| 1 | The default task starts the regulator at `DEFAULT_TASK_TEST_VOLTAGE_MV` = **28 000 mV** at boot, with no PD negotiation. | With assumption 2, 28 V sits on the receptacle's VBUS as soon as the board powers up. **Do not flash `jonah-bench` with anything plugged into the receptacle.** | Yes: the PD build never starts the regulator without a contract. |
| 2 | `bu regulator start/set-voltage` can set any voltage up to 48 V. | Same as #1, triggered from the host. | Yes: the PD build refuses both commands with `PD_OWNS_VBUS`. |
| 3 | The X-NUCLEO-SRC1M1 BSP turns VBUS on over I2C to a TCPP03. On failure the attach handler calls `NVIC_SystemReset()`. | Without a TCPP03, every plug-in reboots the MCU. | Yes: that BSP is dropped and a failure leaves VBUS off. |
| 4 | That BSP also drives PC8 (TCPP03 `ENABLE`, which is `OUTPUT_EN` on this board) and reprograms ADC1 + DMA in `VBUSInit`. | The BSP can close the output switch behind the regulator, and it breaks the VD_MON DMA scan. | Yes: same as #3. |
| 5 | `USBPD_PWR_IF_SearchRequestedPDO` never writes `*Pdo`, so `EvaluateRequest` judged a request against an uninitialized stack variable. | Requests were accepted or rejected at random. | Yes |
| 6 | `USBPD_PWR_IF_GetPortPDOs` never sets `*Size`. | The number of PDOs in Source_Capabilities was undefined. | Yes |
| 7 | `USBPD_PWR_IF_SetProfile` always used PDO 1 (5 V). PS_RDY went out without waiting for VBUS. | Wrong voltage on a 9 V contract, and a PS_RDY that was never true. | Yes: it waits for ±5 % within tPSTransition. |
| 8 | The bundled ST USB-PD core library has no EPR (no Extended_Control, no EPR_Source_Capabilities, no EPR notifications). | A sink's `EPR_Mode (Enter)` gets `Not_Supported`. No EPR traffic is possible. | **No, this needs your action** (see "Library upgrade"). |

## What the branch adds

- `Core/Inc/pd_bench_config.h` holds every limit a sink can reach. Defaults:
  - 5 V and 9 V fixed PDOs at 500 mA.
  - EPR Mode Capable is advertised once the library supports it, but no EPR PDO is offered.
  - VCONN is off.
  - Compile-time checks:
    - the highest PDO must be reachable from 5 V within tPSTransition at the regulator's slew;
    - a 28 V PDO, if enabled, must stay below `OVP_ABSOLUTE_MV`.
- `Core/Src/pd_vbus.c`: `BSP_USBPD_PWR_*` built on the regulator and output switch.
  - VBUSOn = regulator at 5 V, and the switch closes once V_out regulates.
  - VBUSOff = `regulator_stop()`.
  - vSafe0V is reported after a timed `OUTPUT_DIS` discharge (see assumption 3).
  - A regulator FAULT during a contract triggers one Hard Reset. The FAULT stays latched until
    `bu regulator clear-fault`.
- `Core/Src/pd_policy.c` builds the PDO table and evaluates requests:
  - object positions 1–7 are SPR;
  - 8–13 are EPR, valid only in EPR mode;
  - every request goes through an over-current check.
  - It also answers the EPR "what to do" callback and fills Source_Capabilities_Extended.
- `pd_status` telemetry (all 32-bit words, version 1) and `bu pd status` show:
  - the PDOs, the last RDO and its verdict, the contract and transition time;
  - EPR entry counters;
  - the last 32 PD/CAD events with their ages;
  - a diagnosis.
- CMake options in `STM32CubeIDE/final/CMakeLists.txt` (user-owned, so CubeMX leaves them alone):
  - `PD_VBUS_PATH_CHARGER` (default ON) selects everything above.
    OFF is the previous bench build: SRC1M1 BSP, 28 V auto-start. Use it only for dummy-load
    regulator runs with the receptacle unplugged.
  - `PD_VCONN` (default OFF) adds `_VCONN_SUPPORT` and GPIO VCONN switches. The pins are
    placeholders behind an `#error` until you set them.
- One hand edit outside USER CODE: the `RequestDPMWhatToDo` callback-table entry in
  `USBPD/App/usbpd_dpm_core.c`. CubeMX regeneration drops it. The v5 library calls that member
  with no NULL check during EPR entry, so a regenerated table **hard-faults on EPR entry**.

## Library upgrade (your decision; not done here)

EPR requires ST `stm32-mw-usbpd-core` ≥ v5.0.0. v5.4.1 (08-May-2026) implements USB PD R3.2
V1.1, including source EPR. The tooling sandbox refused to vendor the binary, so this is left to
you. The G4 device driver in the tree is already byte-identical to `stm32-mw-usbpd-device-g4`
v5.3.1, so only the core changes:

```sh
git clone --depth 1 --branch v5.4.1 https://github.com/STMicroelectronics/stm32-mw-usbpd-core /tmp/usbpd-core
D=STM32CubeIDE/final/Middlewares/ST/STM32_USBPD_Library/Core
cp /tmp/usbpd-core/inc/{usbpd_core.h,usbpd_def.h,usbpd_tcpm.h,usbpd_trace.h} $D/inc/
cp /tmp/usbpd-core/src/usbpd_trace.c $D/src/
cp /tmp/usbpd-core/lib/USBPDCORE_PD3_FULL_CM4_wc32.a $D/lib/
tools/bringup/bu build
```

What I checked against v5.4.1, from its headers and by disassembling the library:

- PE→DPM callback struct: one member, `USBPD_PE_RequestDPMWhatToDo`, is appended after
  `IsPowerReady`. The branch provides it (#ifdef `USBPDCORE_EPR`).
- After the cable check, the PE calls that member with action 3 (`REPLY_ENTER_MODE`) **without
  a NULL check**. Any return other than `USBPD_ACCEPT` (10) makes it send Enter Failed, code 4.
- The PE reads `USBPD_CORE_DATATYPE_SRC_PDO_EPR` (24) as the **EPR-only** PDO list (positions 8+)
  and pads the SPR part to 7 objects itself.
- Source EPR entry checks, in order:
  1. PDO 1 bit 23 (EPR Mode Capable); fail code 5.
  2. RDO bit 22; fail code 3.
  3. VCONN; fail code 2.
  4. SOP' Discover Identity of the cable e-marker; fail code 1.
  5. The DPM callback above.
- `PD3_FULL` defines `USBPDCORE_EPR`, `PPS` and `SPR_AVS`. `DPM_Settings` gains
  `Is_EPR_Supported_SRC`, which `USBPD_DPM_UserInit` sets.

What I could **not** verify:

- The `#if defined(USBPDCORE_EPR)` code paths have never been compiled. The current library
  doesn't define the macro, and the sandbox wouldn't let me build against v5.
- v5.4.0 deprecates GotoMin and Ping. Expect compile errors in `USBPD_DPM_RequestGotoMin` /
  `RequestPing` in `usbpd_dpm_user.c` if those enum members were removed. Delete or guard
  those two functions.
- Utilities/GUI_INTERFACE and TRACER_EMB compatibility with v5. Build and read the errors.

## Bench plan

Stop and report on any surprise. A FAULT is information; read `bu regulator status` before any
`clear-fault`.

**Stage 0: offline.** Run `bu build` (PD build is the default) and `bu doctor`. Review
`pd_bench_config.h`.

**Stage 1: regulator alone, OFF build, receptacle unplugged.**
1. Build with `cmake --preset Debug -DPD_VBUS_PATH_CHARGER=OFF`.
2. Change `DEFAULT_TASK_TEST_VOLTAGE_MV` to 5000 first.
3. Put a dummy load at the board output: 18 Ω gives 0.28 A at 5 V and 0.5 A at 9 V (the
   offered 500 mA). Step down in resistance only after that holds.
4. Confirm 5 V and 9 V hold within 5 % and that overshoot on the 5 → 9 V step stays under
   9.9 V (relative OVP). The 5 → 9 V ramp takes 200 ms at 20 V/s.
5. Rebuild ON (`--reconfigure`) when done.

**Stage 2: PD at SPR with a PD tester or trigger as the sink.** Use something with Rd that
lets you pick the PDO and shows the message log, for example:
- a USB-PD analyzer or tester (POWER-Z KM003C class);
- a second Nucleo running an open-source sink. [elagil/usbpd](https://github.com/elagil/usbpd)
  has an embassy-stm32 G431 EPR sink example.

Steps:
1. Plug in the tester. Expect `bu pd status` to show:
   - `attach_count` 1;
   - a 5 V contract;
   - events `CAD_ATTACHED … POWER_EXPLICIT_CONTRACT`.
2. Scope VBUS: 5 V within 275 ms (tVBUSON) of attach.
3. Request 9 V. Expect `transition_ms` ≈ 200 and no Hard Reset.
4. Unplug. Expect VBUS to drop and `contract` to clear.
5. To see the wire, read the UCPD tracer on LPUART1 (ST-LINK VCP) with STM32CubeMonitor-UCPD,
   or capture CC1/CC2 with `bu la` and the sigrok `usb_power_delivery` decoder.

**Stage 3: laptop at SPR.** Only after Stage 2 is clean.
- Expect negotiation at 5 V or 9 V, 500 mA: 2.5–4.5 W. Most laptops then report a slow
  charger or don't charge; this stage checks negotiation, not charging.
- The worst case on VBUS is the highest offered PDO plus regulator overshoot (OVP trips at 110 %).
  That's far below the 20 V every SPR laptop port accepts.
- If the laptop requests 9 V and Hard Resets, read `transition_timeouts` and the event log.

**Stage 4: EPR handshake.** Needs the library upgrade and an EPR-capable sink (tester or the
elagil sink).
- Expect `EPR_Mode (Enter)` from the sink, then `Enter Acknowledged`.
- Without VCONN, `Enter Failed` follows, reason 1 or 2.
- That is real EPR data on the wire, and `bu pd status` counts it (`epr.enter_failed`).

**Stage 5: EPR mode at SPR voltage.** Needs VCONN hardware plus a 5 A EPR cable with an e-marker.
1. Fit a 5 V switch per CC line (≥ 1 W VCONN per the Type-C spec; e-markers draw far less).
   Set the pins in `pd_bench_config.h` and build with `-DPD_VCONN=ON`.
2. Scope the unused CC line at ~5 V when VCONN is on.
3. Expect `Enter Succeeded`, then `EPR_Source_Capabilities`. It is a chunked extended message:
   seven SPR slots plus the EPR PDOs, none with the defaults.
4. The sink sends an `EPR_Request` for 5 V or 9 V, then `EPR_KeepAlive` / `EPR_KeepAlive_Ack`
   about every 0.5 s.
5. `bu pd status` shows `in_epr_mode: true`.

**Stage 6: 28 V EPR PDO.** Set `PD_EPR_PDO_28V_ENABLE` only after everything in its comment
holds. The compile-time check also fails at the current slew: 5 → 28 V takes 1150 ms against
an 830 ms minimum tPSTransition.

## Stability work before 15/20 V or EPR voltage

1. **Setpoint up-slew.** `SETPOINT_SLEW_MV_PER_CYCLE` = 1 at 20 kHz gives 20 V/s:
   - 5 → 15 V takes 500 ms; 5 → 20 V takes 750 ms;
   - the sink's tPSTransition is 450 ms min.
   So 15 V and 20 V PDOs fail the compile-time check today, and most laptops want 20 V.

   Proposal:
   - add a separate up-slew of about 3 mV/cycle (60 V/s, 5 → 20 V in 250 ms) and keep 1 mV/cycle
     down (the 28 → 20 V OVP case);
   - validate the up-step overshoot into a dummy load before any PD use.

   Not done here, because it changes `jonah-bench` loop dynamics.
2. **Output current.** The stage is DCM-only (`SYNC_RECT_ENABLED` 0, `DCM_MAX_PEAK_MA` 2500).
   Bench loads so far were 330 Ω. Measure the current each voltage holds within 5 % before
   raising `PD_SRC_MAX_CURRENT_MA`.
3. **Boost start ringing.** Runs 32 and 33 rang to 36–38 V at a 28 V target. Any EPR voltage
   above V_in is boost, so 28 V EPR waits for that to be closed.
4. **Downward transitions.** A buck in DCM can't pull V_out down. With a light load, a step
   down (or EPR exit) may miss the ±5 % window and end in a Hard Reset, which is safe but noisy.
5. **CC pin exposure.** The receptacle CC pins sit next to VBUS. A tilted plug can short CC to
   VBUS. The G474 UCPD pins are FT I/O, rated for about 5 V, not 20–48 V. ST's answer is TCPP-class
   protection on CC. Before EPR voltages, put CC/VBUS short protection between the receptacle
   and PB4/PB6.

## Open questions

- Are assumptions 1–4 right, and which GPIOs are free for VCONN?
- What output-switch FET part and output-capacitor voltage ratings are fitted? Required for any
  28 V test. The schematic values show SiR5607DP on an AON6411 footprint.
- Which EPR sink will you use for Stages 4–5?
