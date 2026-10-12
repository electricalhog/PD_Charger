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
| 8 | The bundled ST USB-PD core library has no EPR (no Extended_Control, no EPR_Source_Capabilities, no EPR notifications). | A sink's `EPR_Mode (Enter)` gets `Not_Supported`. No EPR traffic is possible. | Yes: the V5.3 core from `jonah-bench` (ce537f5, X-CUBE-TCPP 4.2.0) is merged in (see "Library upgrade"). |

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

## Library upgrade

**Done 2026-10-05:** merged from `jonah-bench` (ce537f5): ST USB-PD core V5.3 (X-CUBE-TCPP
4.2.0, `PD3_FULL`, defines `USBPDCORE_EPR`) with matching TRACER_EMB and GUI_INTERFACE. The
`#if defined(USBPDCORE_EPR)` paths now compile. The default build, `-DPD_VCONN=ON` and
`-DPD_VBUS_PATH_CHARGER=OFF` all build with no errors. GotoMin/Ping needed no change. Not yet
run on hardware. The original notes for v5.4.1 follow; the callback and EPR entry
analysis was done against v5.4.1, so re-check it on the bench against V5.3 with `bu pd trace`.


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

- ~~The `#if defined(USBPDCORE_EPR)` code paths have never been compiled.~~ They compile against V5.3 (2026-10-05).
- v5.4.0 deprecates GotoMin and Ping. Expect compile errors in `USBPD_DPM_RequestGotoMin` /
  `RequestPing` in `usbpd_dpm_user.c` if those enum members were removed. Delete or guard
  those two functions.
- Utilities/GUI_INTERFACE and TRACER_EMB compatibility with v5. Build and read the errors.

## Bench log 2026-10-05/06 (local session)

- **VBUS start loop against a dead sink.** The G474 PD build was flashed while the sink's TCPP02
  sat unconfigured (its I2C bus was wedged by the QT Py). Its hibernate dead-battery Rd showed
  on CC1, so the source attached, started VBUS and cycled 1355 times, about every 6 s.
  - It ended in a latched SW_OVP: V_out 5529 mV against the 5500 mV trip (110 % of 5 V), at
    VIN 20 V, into only the buck's input capacitors.
  - That's a light-load overshoot of about +10.6 %. Expect it again at 5 V with no load.
  - Don't flash or reset the G474 with a real-VBUS build while the sink's TCPP02 is not in
    Normal mode.
- **Dry run.** CMake `-DPD_BENCH_DRY_RUN=ON` (`pd_vbus.c`) tells the stack VBUS follows the
  contract but never starts the regulator.
  - Use it for protocol work (VCONN, SOP', EPR messages) when nothing should take power.
  - `bu pd status` reports `dry_run` and the VCONN switch outputs (`cc1_pa7`, `cc2_pb5`).
- **E-marked cable.** The G474 ↔ SRC1M1 link is an e-marked cable.
  - With `PD_VCONN=ON` the CAD reported ATTACHED, not ATTEMC: no Ra on CC2 at the G474 end.
    So VCONN stayed off and no SOP' discovery ran.
  - Likely the cable's e-marker is in the plug at the SRC1M1 end. Swap the cable ends, or
    check which plug carries it.
  - The tracer on the G474 VCP works (`bu pd trace --port <G474 VCP>`, reset inside the
    window to catch an attach).

- **PD timers never ran (fixed 2026-10-06).** Nothing called `USBPD_DPM_TimerCounter()`:
  FreeRTOS owns SysTick and the tick hook is off. So no PE or protocol-layer timer ever
  expired.
  - Symptoms: one Source_Capabilities burst and no resends; after Accept, no PS_RDY, so the
    sink sent a Hard Reset 500 ms later.
  - Now called from the 1 ms TIM2 time base (`main.c`, USER CODE Callback 1).
  - `pd_status` has SETUP_POWER / SETUP_POWER_ERR / POWER_NOT_READY events for this path.
- **First contracts (dry run, e-marked cable, sink TCPP02 in Normal mode).** 5 V/500 mA on the
  first attempt; then 9 V (position 2) and back to 5 V, with no Hard Resets.
- **First EPR entry attempt.** Sink EPR_Mode Enter (10 W) → G474 Enter_Acknowledged → G474
  VCONN_Swap, because it is not VCONN source.
  - No Ra was seen at attach, so there was no ATTEMC and VCONN was never turned on.
  - The usbpd sink answers VCONN_Swap with Soft_Reset, which aborts the entry; both sides
    renegotiate 5 V SPR.
  - To get past VCONN, the G474 must see the e-marker's Ra on its CC2 at attach: swap the cable
    ends, or add a bench option that forces VCONN on at attach.
- **QT Py pads.** GPIO22/23 (STEMMA I2C) cannot drive low; GPIO24/25 cannot drive high. Pins
  the firmware drives (GPIO3/4/5/6/20) read back correctly. The I2C load link cannot work on
  this QT Py.

- **Later the same day (2026-10-06, evening):**
  - **Sink detach debounce.** Once the PE timers ran, the source resent Source_Capabilities
    every 159 ms. The sink, with no debounce, declared a detach on each burst. It now waits
    15 ms (tPDDebounce).
  - **VCONN on ATTEMC.** When the CAD sees the cable's Ra, the DPM now powers the e-marker on
    that line (`usbpd_dpm_user.c`). ST's core only sets VconnStatus.
  - **VCONN switch measured** at 4.06 V on CC1, about 0.9 V under the 5 V rail.
  - **`PD_BENCH_FORCE_VCONN`** makes the source VCONN source when no Ra is seen.
    - The first version enabled it at attach. Once, the CAD had picked CC1 while PD ran on CC2,
      so VCONN went onto the CC wire. It was switched off over SWD (GPIOB BSRR) within about a
      minute.
    - It now enables only after an explicit contract has proven which line carries PD. VCONN
      never goes on both lines (`vconn_set`).
  - **Cable orientation (Apple 240 W):**

    | E-marker at | G474 attach | PD |
    |---|---|---|
    | far end | ATTACHED, no Ra | contracts work; EPR stops at VCONN_Swap |
    | G474 end | ATTEMC, VCONN on | no message decoded in either direction, though DC attach looked right on both sides |

    In the second orientation the G474 at times saw Rd on both lines (a debug accessory) and
    refused to attach.
  - **State at push:** the G474 cycles attach without a contract and the sink has about 17,000
    Hard Resets. Scope CH2 (CC1) / CH3 (CC2): about 0.2 V / 0.1 V DC with common-mode spikes
    on every channel and no BMC captured. The CH1 and CH2 probes were intermittent. SOP' cable
    discovery is untested.

## Bench log 2026-10-07

Results, figures and raw captures: `bench/results/2026-10-07/`.

- **Bad CC jumper.** The G474's CC1 (PB6) reached the cable through about 60 kΩ. The sink's CC1
  sat at 0.19 V, just under its 0.2 V attach threshold, and the G474 read CC1 as open.
  - That explains the 2026-10-06 symptoms put down to cable orientation. At a few kΩ the DC
    attach still worked but no BMC got through, and the e-marker's Ra seen through it looked
    like Rd.
  - The jumpers were replaced; attach, contracts and VCONN then worked first time.
- **EPR entry works end to end.** It took two fixes:
  - **G474:** `USBPD_VDM_UserInit` never called `USBPD_PE_InitVDM_Callback`. The PE took the
    cable's Discover Identity ACK and stalled, with no Enter_Succeeded or Enter_Failed. It now
    registers the callbacks at init, ST's pattern for VCONN without VDM (`usbpd_dpm_user.c`,
    `usbpd_vdm_user.c`). `PE_VDMSupport` stays off; turning it on changed nothing.
  - **Sink:** usbpd calls `request()` without `inform()` after attach and after EPR entry. So
    the sink answered EPR_Source_Capabilities with a plain Request, and the G474 Hard Reset as
    the spec requires. The sink now decides EPR mode from the capabilities themselves
    (`pd.rs`, `caps_are_epr`).
  - Result, in dry run and at 15 V real VBUS: Enter → Enter_Succeeded in 13 ms with the SOP'
    cable check, then EPR_Request → Accept → PS_RDY. EPR_KeepAlive repeats every 378 ms.
- **USB-PD core v5.4.1** from ST's GitHub, replacing v5.3.0.
  - The G4 device driver (v5.3.1) and the tracer (V1.12.1) are code-identical to what we had.
  - `usbpd_dpm_core.c` stays our CubeMX FreeRTOS copy, not the generic one the core now ships.
- **VCONN on CC2 measured.** These are the benchmark for the CC1 switch (results folder):
  - 5.04 V with the e-marker powered;
  - on with the attach, off about 35 ms after a detach;
  - a 0.75 ms soft rise.

  The 4.06 V seen on 2026-10-06 was most likely the bad jumper.
- **Real VBUS:** 5, 9 and 15 V contracts with no faults; 5 → 9 V took 182 ms and 9 → 15 V took
  268 ms.
  - 15 V is the bench option `-DPD_BENCH_15V=ON` (`pd_bench_config.h`). At 20 V/s, 5 → 15 V
    takes 500 ms, so 15 V is reached from 9 V or in EPR mode.
  - The source now uses the EPR transition budget for every request made in EPR mode
    (`usbpd_pwr_if.c`).
- **VBUS reads 11 % low at TP2.**

  | Reading at the 15 V contract | Value |
  |---|---|
  | TP2, scope at 0.5 V/div | 13.29 V |
  | QT Py | 12.95 V |
  | VD_MON (board ADC) | 15.03 V |
  | VS_MON on the 24 V supply | 20.1 V |

  - The gap is the same at 0 and 3 W, so it is the sensing.
  - Likely cause: the divider loading changed when the SRC1M1 shield came off; it had put its
    VBUS divider on PA0 = VD_MON.
  - Recalibrate `VD_MON_FULL_SCALE_MV` and `VS_MON_FULL_SCALE_MV` against a meter.
- **vSafe0V is out of reach.** With VBUS off, the QT Py back-feeds the buck input to about 2.8 V
  through IC7. After a Hard Reset the source waits for vSafe0V, gives up (POWER_NOT_READY),
  detaches and re-attaches.
- **`bu sink rd off` leaves the sink stale.** Its PD loop logs Hard Resets while detached and
  sends one at the next attach. For clean attach tests, reset the G474 (`bu probe reset`).
- **First buck load runs.** The QT Py was flashed with `--features power-stage` (BOOTSEL with
  VBUS off). At the 15 V contract it ran 0.5, 2 and 3 W into the 11 Ω ballast.
  - Then a 0.5 → 2 → 3 W staircase under the EPR contract:
    `bu load run --p 500,2000,3000 --seconds 4 --limit-ma 500 --vin-min-mv 11500`.
  - VBUS at TP2 held 13.3 V, with no faults.
  - The buck needs 12–15 V on VIN for its gate drive, so it cannot run at 5 or 9 V.

## Bench log 2026-10-10

Results, methods and tables: `bench/results/2026-10-10/`. The raw captures stay on the bench
machine.

- **VBUS sense recalibrated.** At the 15 V contract a meter read 13.32 V at TP2 and 24.56 V on
  the supply, against 15.00 V on VD_MON and 20.07 V on VS_MON.
  - `VD_MON_FULL_SCALE_MV` is now 71560 (was 80600) and `VS_MON_FULL_SCALE_MV` 69500 (was 56800),
    in `regulator_config.h`.
  - It is a gain-only correction, so redo it whenever the wiring on PA0 or PA4 changes.
- **20 V PDO** with `-DPD_BENCH_20V=ON`, which needs `PD_BENCH_15V`. In SPR mode 20 V is reached
  from 15 V (250 ms at 20 V/s). 5 → 20 V takes 750 ms, which only the EPR budget covers.
- **Regulation** (scope at TP2, and the board ADC):
  - At 9, 15 and 20 V: +0.11 to +0.38 % by the scope, −0.18 to +0.10 % by the ADC.
  - At 5 V the scope reads 5.132 V and the ADC 4.998 V. A meter reading at 5 V is still needed.
  - 0 → 3 W raises VBUS by 15.2 mV at 15 V and 20.2 mV at 20 V (−0.10 %).
- **Voltage loop: stable, lightly damped.**
  - After the load's start-up step, VBUS rings at 263 Hz (15 V) and 429 Hz (20 V), ζ ≈ 0.2.
  - The low-frequency noise has a hump at 233–524 Hz at every contract. It grows with load, up to
    +24 dB over the 20–60 Hz floor at 20 V, 3 W.
  - Releasing 3 W overshoots +2.3 %, back in band in 8.3 ms (15 V) and 3.6 ms (20 V).
- **Noise at TP2** (20 MHz bandwidth):
  - The 200 kHz ripple is 13 mV p-p at 5 V, up to 155 mV (15 V, 3 W) and 210 mV (20 V, 3 W).
  - Vpp is 687–916 mV, set by spikes. 55 % of them coincide with spikes on the VCONN probe, so a
    good part is pickup; this needs a tip-and-barrel re-measurement.
  - Lines appear at 2.1–2.6 kHz with the source alone, and at 0.40 and 1.39 kHz with the load
    running, up to 23.8 mV rms.
- **The load jumps at start-up.** Its output goes from about 0.1 V to 0.8–1.3 V between two
  telemetry frames, instead of following its 5 V/s ramp. That is the −343 mV (15 V) and −214 mV
  (20 V) step VBUS sees.
- **Scope traps** (DS1104Z):
  - At 100 kSa/s the 200 kHz ripple aliases to a 5 Hz beat (the clocks differ by 25 ppm). It
    looked like a loop oscillation.
  - HRES at 2 MSa/s is aliased above about 30 kHz, so the spectra are stitched at 10 kHz.
  - RAW reads after `:SINGle` need an explicit `:STOP`.
  - `:TFORce` is ignored until the pre-trigger memory fills.
- **`bu load run`** now stops on SIGTERM or Ctrl-C, disarms the load, and reports
  `stopped_early`.

## EPR plan (decided 2026-10-11)

Goal: the most capable DC-DC PD source we can build. The input is 2-8S LiPo (6.0-33.6 V); the
bench has a fixed 24 V supply only.

**Contracts.**
- Fixed EPR PDOs at 28, 36 and 48 V. No AVS for now.
- They are enabled in that order, each behind its own compile-time gate.
- The power board was designed with parts rated at least 60 V; the part numbers are not
  verified.
- 28 V must negotiate directly from 5 V, with no intermediate steps.

**Slew.**
- Up: 4 mV/cycle (80 V/s). 5 -> 28 / 36 / 48 V takes 288 / 388 / 538 ms, inside the 750 ms
  EPR budget.
- Down: a separate constant, also 80 V/s.
- OVP is referenced to the higher of the old target and VD_MON while a downward transition
  runs, so a slow decay does not trip it.

**Downward transitions: timed reverse pulses.** The INA281 sense is unidirectional, so the
reverse current is set by timing, not measured.
- In buck, the low side is held on for t_rev, so I = -V_out * t_rev / L. A per-pulse cap
  (about -2 A) and an energy budget per ms bound it.
- Pulses run only while VBUS sits above the slewed setpoint, and only while VS_MON stays below
  (V_in at the start of the transition) + 4 V. Otherwise VBUS decays passively.
- Energy check: 48 -> 5 V on < 300 uF of VBUS capacitance is about 0.34 J. Into the supply's
  4000 uF that is about +3 V, so the bench needs no input bleed.
- Full bidirectional sync-rect CCM is deferred. Peak-current control is already out because of
  the current-sense amplifier's bandwidth.
- OUTPUT_DIS (a few hundred ohms through a SOT-23-6 FET) is only an emergency clamp for severe
  overshoot: few-ms pulses with a duty limit, never a normal down-ramp.

**Task 1: boost stability.** At 24 V in, 5 -> 28 V directly at 80 V/s, at 0, 0.5, 2 and 3 W:
1. no OVP or backstop trip;
2. overshoot at most +5 % (29.4 V), at the end of the ramp and at the buck -> boost handover;
3. inside +-5 % within 650 ms of the request;
4. zeta >= 0.4 on a 0 -> 3 W step, measured with the 2026-10-10 method. This includes retuning
   the loop, which rang at zeta ~0.2 at SPR;
5. ripple and noise reported in the 2026-10-10 tables.

Scope: CH2 and CH3 on the inductor switch nodes, CH4 on TP2.

**Protocol: PD 3.2 conformance as an EPR source**, verified on the bench:
- Get_Revision answers 3.2. `usbpd_dpm_user.h` overrides the core's 3.2 with 3.1 and must be
  fixed.
- Get_Source_Info returns the right PDP.
- The EPR flow at 28, 36 and 48 V: EPR_Source_Capabilities with the EPR PDOs at position 8 and
  up, EPR_Request, EPR_KeepAlive, and EPR exit back to SPR (using the down-transition above).
- Negative cases:
  - an EPR request without EPR mode -> Hard Reset;
  - a missed KeepAlive -> exit;
  - a cable without a 50 V e-marker -> no EPR PDOs.
  The sink firmware gets test hooks for these.
- CC1 is checked with the cable flipped (2026-10-11).

**Output switch above 28 V.** The Q5/Q6 gate drive clamps Vgs at -13 V (13 V zener across the
51 kohm pull-up, 10 kohm R27 in the pull-down since 2026-10-12; bench/README.md). R27 is an 0603
that dissipates 123 mW at 48 V: fine for test runs, upsize it for sustained 48 V.

**Load above 32 V (risk accepted: the load board is already damaged and spares exist).**
- The buck load stays on VBUS at 36 and 48 V, behind an explicit bench flag in the sink
  firmware.
- IC8 (L6983, 38 V) and the QT Py VIN tap (9.78:1, about 32 V for 3.3 V) are over their limits
  there. A 10 kohm series resistor on the tap is recommended.
- Before the first 36 V contract with the load attached, check that the G474's overcurrent and
  backstop limits are armed, in case IC8 fails short and +OUT shorts VBUS.

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
5. **Voltage-loop damping.** On 2026-10-10 a load step rang at 263–429 Hz with ζ ≈ 0.2, and the
   noise peaking near 400 Hz grew with load. Measure the loop gain and phase margin, and retune,
   before raising `PD_SRC_MAX_CURRENT_MA`.
6. **CC pin exposure.** The receptacle CC pins sit next to VBUS. A tilted plug can short CC to
   VBUS. The G474 UCPD pins are FT I/O, rated for about 5 V, not 20–48 V. ST's answer is TCPP-class
   protection on CC. Before EPR voltages, put CC/VBUS short protection between the receptacle
   and PB4/PB6.

## Open questions

- Are assumptions 1–4 right, and which GPIOs are free for VCONN?
- What output-switch FET part and output-capacitor voltage ratings are fitted? Required for any
  28 V test. The schematic values show SiR5607DP on an AON6411 footprint.
- Which EPR sink will you use for Stages 4–5?
