# Bench log 2026-11-03: boost and band forced CCM; PD-target sweep with ripple

| Field | Entry |
|---|---|
| Time | 18:5x–20:09 EST, remote (Claude Code through `bu`), on Dan's "sweep power and voltage ... use CCM whenever it is more efficient ... noise and power loss within reason" |
| Firmware | `claude/ccm-hf-buck-50w` at 6a73288 plus the uncommitted changes listed in section 5. The final flash is ELF sha1 `b2a049b0…` |
| Supply / load | V_in 24.3–24.5 V (VS_MON). 25.3 Ω on VBUS; the QT Py is the load. PD sink: a G431 running Rust usbpd (rev 72008c4). It can't measure VBUS, so the QT Py feeds it VBUS |
| Scope | CH1 SW1, CH3 SW2, CH4 VBUS at TP2. For ripple: AC coupling, 0.2 V/div, 20 MHz BWL, 2 µs/div raw capture |
| Scripts | `scripts/benchlib.py`, `scripts/sweep.py`. Raw data: `sweep_*.jsonl`, `ripple28_recheck.txt` |

Every number below is a tool reading (status reads, the 20 kHz regulator trace, the scope). The exceptions are the
"Model" and "Inference" items, which are labeled.

**Ripple metric.** Each capture is 6000 samples at 250 MSa/s. The metric is the p-p and the rms of a 200 ns moving
average, which filters out the edge spikes (mostly probe pickup).

- The raw spikes clip at the scope's −1.01 V / +1.88 V rails. In a 28 V capture, 91 low and 24 high of the 6000 samples
  sat on a rail, so the averaged value is a slight underestimate where spikes clip.
- `ripple()` now runs `bu scope run` before each capture. Without it, a raw capture leaves the scope stopped and the
  next capture repeats it.

## 1. Final sweep: 200 kHz, auto CCM (default) vs forced CCM (`sweep_final.jsonl`, `sweep_band.jsonl`)

Ripple is p-p / rms in mV. VD σ is the 102 ms trace's σ. Every point below had 0 CCM exits in a 5 s hold.

| Contract | Auto (`ccm_enable` 1) runs | Auto ripple | VD σ | Forced (2) runs | Forced ripple | DCM-only ripple (`ccm` 0, `sweep_ripple.jsonl`) |
|---|---|---|---|---|---|---|
| 5 V | buck DCM | 83 / 15 | 25 | buck CCM | 169 / 40 | 82 / 14 |
| 9 V | buck DCM | 116 / 31 | 29 | buck CCM | 276 / 85 | 110 / 29 |
| 15 V | buck DCM | 263 / 78 | 31 | buck CCM | 495 / 168 | 244 / 73 |
| 20 V | buck DCM | 362 / 108 | 34 | buck CCM | 557 / 183 | 359 / 105 |
| 24 V | **band CCM** (new) | **338 / 67** | 37 | band CCM | 343 / 68 | buck-boost DCM 780 / 260 |
| 28 V EPR | boost CCM | 566–576 / 190–196 (3 captures, `ripple28_recheck.txt`) | 37 | boost CCM | 565–575 / 190–196 (3) + 569 / 195 | buck-boost DCM 1082 / 356 |

Notes:

- **24 V:** band CCM cut the ripple to 44 % (p-p) and 26 % (rms) of buck-boost DCM's. Auto mode picks it because
  I_cmd is 948 mA, over the 400 mA entry for a buck-boost fallback. The integrator settled at −413 to −467 mV.
- **28 V:** boost CCM gives about half of buck-boost DCM's ripple. These are the same numbers as before today's change.
- **Excluded 28 V captures:**
  - The auto 28 V capture inside `sweep_final.jsonl` (20:04:56) was flat on the −1.01 V rail for all 6000 samples.
    It was taken while the AC coupling was still recovering from the EPR voltage step, so it is invalid.
  - The re-run at 20:06:52 read 80 / 14 mV with no clipping. This was not reproduced: 3 later captures in the same
    state read 568–576 mV, so the 80 mV value is excluded and unexplained.
- **Buck CCM at these loads (200 kHz, 0.2–0.8 A):** it has 1.5–2× the ripple of DCM. Auto mode stays in DCM there,
  which is what the auto rule intends (enter at 85 % of the DCM ceiling). The earlier 400 kHz forced-buck run
  (`sweep_ripple.jsonl`) measured 92/11, 137/19, 183/36 and 242/57 mV at 5/9/15/20 V.
- **Regulation:** V_out was within 0.7 % of the contract at every RUNNING point (status mean of 3–6 reads; the
  worst was 14895 mV at 15 V, auto/DCM).

## 2. Band CCM (new): 24 V from 24.3 V

**Problem.** Boost CCM can't go below V_in (1 − r)/(1 − D_min), which is about 1.01 V_in. Buck CCM in the regulator
runs only in BUCK mode. So V_out from 0.93 to 1.03 V_in ran buck-boost DCM, with about 0.78 V p-p ripple at 24 V.

**Change.** Band CCM uses the boost CCM legs and duty law with two differences:

- Q1's off window (Q2 on inside it) is widened to `CCM_BAND_Q2_NS` = 640 ns + 2 × 20 ns dead time.
- Q4's pulse may end inside that window, down to 150 ns.

Volt-second balance gives V_in (T − t_window) = V_out (T − t_on) whether Q4 ends before or after Q1 turns back on.
The host test (`test_band`) plays every on-time tick by tick and checks dead times and shoot-through on both legs.
It gives a range of 22.4–86.9 V at 24.9 V in, and holds 24 V, 25.5 V, 19 V and 9.5 V through 25 ↔ 4–9 Ω load steps.

**Bench result** (24 V contract, 25.3 Ω):

| Run | Kind | V_out | t_on | u | Exits / 5 s | Ripple p-p / rms |
|---|---|---|---|---|---|---|
| forced | band (3) | 23893–23950 mV | 494–496 ns | −458 to −467 mV | 0 | 343–348 / 67–68 |
| auto | band (3) | 24054–24088 mV | 495 ns | −413 to −467 mV | 0 | 335–338 / 67 |

- **Entry transient** (`20261103-200906_regulator_trace.csv`, learned u): V_out stayed between 23.87 and 24.27 V from
  the entry through the next 90 ms.
- **Starting integrator.** The first build started u at +0.32 V (CCM_BOOST_U0_MV plus a dead-time term). The bench
  settled at −0.46 V, so the formula had the wrong sign. The unlearned starting value is now
  `CCM_BAND_U0_PERMILLE` (−19 ‰ of V_ref), taken from this measurement. Its first entry after a reflash was not
  traced separately.
- **Model:** `plans/scripts/ccm_bb.py` rates band CCM above buck-boost DCM at V_out = V_in. At 15 → 15 V it gives
  95.8 / 97.9 / 98.2 % against 93.8 / 95.0 / 95.0 % at 0.3 / 0.75 / 1.5 A.
  - The model's band sequence is Q1Q4 → Q1Q3 → Q2Q3. The firmware's at 24 V (t_on 495 ns < window 680 ns) is
    Q2Q4 (L at 0 V) → Q2Q3 → Q1Q3.
  - **Inference:** this adds some conduction loss in the 0 V interval. The model was not rerun for this sequence.

## 3. Power loss

This was not measured.

- **The board can't measure it in these modes.** ID_MON is valid only when SW2 doesn't switch (buck DCM). Against
  V²/R it read 21 % low in buck CCM, 30 % low in boost CCM and 51 % low in buck-boost DCM (earlier today, same
  load). IS_MON is a phase-dependent sample.
  - **Inference:** SW2 edges disturb the sense amplifier, a hardware limit.
- **P_out is known; P_in is not.** `sweep.py` records P_out = V²/25.3 Ω (1.0–31 W across the ladder). Measuring
  P_in needs external input meters.
- **The CCM choices rest on the model.** The model's crossover is what the auto rule implements:
  - buck/boost CCM near the DCM ceiling;
  - boost or band CCM from 400 mA where the fallback is buck-boost DCM.

## 4. PD: EPR 28 V hard resets: found and fixed (source timer-server race)

**Symptom.** The 28 V EPR contract ended in a sink-sent hard reset every 8–96 s. It happened in DCM and CCM, with or
without SWD polling. Three of these occurred during the sweep in section 1.

**Root cause** (each step measured, see the evidence below):

1. The ST stack sends GoodCRC from the UCPD interrupt (priority 5). `PRL_Received` in the binary core arms a 30 µs
   "minimum GoodCRC" timer (TIM1 CH1, 1 MHz, wraps every 10 ms). `PRL_GoodCRC` then busy-waits on
   `USBPD_TIM_IsExpired()` (disassembly of `usbpd_prl.o`: `adc: bl USBPD_TIM_IsExpired; cmp r0,#0; beq adc`).
2. Our `usbpd_devices_conf.h` armed the timer in two steps with interrupts on: write the compare (CNT + 30), then
   clear the CC flag.
3. If a regulator ISR preempts between the two for more than 30 µs, the compare matches during the preemption and
   the clear then erases that match. The PID ISR (priority 2) takes up to 43 µs and about 60 % of the CPU at 28 V.
4. The next match is a full wrap away, so the GoodCRC goes out 10 ms late.
5. The sink retries its EPR_KeepAlive twice, then sends Soft_Reset three times, none acknowledged, then a hard reset.

**Fix.** The compare write and the flag clear are now one critical section of a few cycles (`TIMX_SETEVENT`), applied
to all four timer channels. A `#error` in `pd_policy.c` fails the build if a CubeMX regeneration restores ST's version.

**Result.** 604 s at 28 V EPR with **0 hard resets**: the contract stayed at 28000 mV, position 8, in EPR mode, and
the regulator held 28026 mV in boost CCM. Before the fix, the drops in this session came after 8, 24, 47, 72 and 96 s.

**Evidence**

| Source | What it showed |
|---|---|
| Sink log (`sink_epr28_watch2/3.json`) | At each hard reset: `last_tx=0x008d` (Soft_Reset) 1 ms before, `last_rx` = the previous EPR_KeepAlive_Ack 385 ms before |
| CC1 on scope CH2, BMC decoded (`scripts/pd_decode.py`, `bringup_out/20261103-205936_scope_ch2_ch4.csv`) | Normal: KeepAlive → source GoodCRC 51–54 µs after it ends → ack 1.6 ms later. Failure: KeepAlive id 4 ×3, Soft_Reset ×3, all valid CRC, no GoodCRC; HARD_RESET; then the source's GoodCRC **for KeepAlive id 4**, 0.9 ms after the hard reset |
| Source UCPD event ring (`ucpd_ring_drop1/2.txt`, `scripts/ucpd_ring.py`) | RX end → GoodCRC send 37–49 µs in every normal exchange; **10034 / 10036 µs** before each drop, both in the UCPD IRQ (IPSR 79) |
| Preemption accumulators on every ISR above UCPD | During the 10036 µs stall, ISRs took 6028 µs (all TIM7 PID): its normal ~60 % share (224.5 ms of each 374.6 ms). So the UCPD ISR spent about 4 ms of CPU in its own busy-wait |
| PHY send-refusal counters | 0 refusals before the reset. The one refusal (TX DMA busy) came 150 ms after it, during the stack restart |

**Sink side (bench tool).** The G431 sink can't sense VBUS (`vbus_mv=0`), and that turned one hard reset into a
detach.

- Its `wait_for_vbus` returned at once, so SinkWaitCap (465 ms) expired before the source's capabilities, which
  come about 1.1 s after a hard reset. The sink then sent 3 hard resets and gave up (`sink_epr28_watch.json`).
- **Fix (`bench/pd-sink-g431`):** wait 1 s (`VBUS_RECOVER_MS`) after any hard reset. Each sent hard reset is now
  logged with the last tx/rx headers.
- After the fix, a drop produced one hard reset (`sink_epr28_watch2.json`). The source still showed `CAD_DETACHED`
  905 ms after `HARDRESET_RX`, and the cause of that detach is open. It no longer matters for EPR stability now that
  the hard resets are gone.

## 5. Firmware changes in this build (uncommitted at the time of this log)

- **Boost forced CCM** (`buck_ccm.[ch]`, regulator 6c), at 200 kHz: Q1 held on with a Q2 refresh inside each Q4 pulse.
- **Band forced CCM** (`band_ccm_cfg_valid`; `CCM_KIND_BAND`; `CCM_BAND_*` in `regulator_config.h`):
  - Timer B CMP1 (blanking end) follows Q1's late turn-on and is restored on exit.
  - The ADC quiet interval starts after both Q3's and Q1's turn-on.
- **CCM kind per mode:**
  - BUCK mode: buck.
  - BOOST mode: boost.
  - BUCK_BOOST mode: boost at 1.03 × its floor or more; band from 1.015 × the band's floor up to that.
- **Auto CCM (`regulator_ccm_enable` 1) is the default**, and buck fsw is 200 kHz (was 400).
- **Over-band handback:** max(3 % V_ref, 400 mV, skip band), held 20 PID cycles.
- **Relative OVP:** must persist 3 PID cycles. Absolute OVP stays immediate.
  - This was added after a SW_OVP at the 5 V restart that followed an EPR hard reset. The VD burst outliers were
    median-of-9.
- **ADC2** free-runs with 256× oversampling, so VS and ID are period means.
- **USBPD timer server** (`usbpd_devices_conf.h` `TIMX_SETEVENT`): atomic compare + flag clear (section 4), with a
  `#error` guard in `pd_policy.c`.
- **Bench instrumentation, left in:**
  - `ucpd_dbg` and the frozen `ucpd_ring` in ST's PHY sources (`usbpd_phy_hw_if.c`, `usbpd_hw_if_it.c`);
  - `isr_hp_cyc` per-ISR cycle accumulators in `stm32g4xx_it.c` (two DWT reads per ISR).
- **`pd_status_event`** drops an event identical to the newest one, so POWER_NOT_READY spam can't flush the ring.
- **Host tests** (`make -C STM32CubeIDE/final/tests/buck_ccm`): all pass, including `test_boost` and `test_band`.

## 6. Open

1. The source detaches about 0.9 s after a received hard reset (section 4). This is rarer now that the EPR resets
   are fixed.
2. Measuring power loss and efficiency needs external meters. A fix for ID_MON when SW2 switches is a hardware change.
3. Boost CCM at 28 V has 0.57 V p-p ripple. It is the largest ripple left, about 2 % of V_out.
4. The band CCM's first entry after a reflash, with the new −19 ‰ u0, is untested by trace. Band was tested only at
   V_in ≈ 24.3 V, because the supply is fixed.
