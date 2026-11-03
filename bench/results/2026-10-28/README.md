# Bench log 2026-10-28 to 11-02: CCM entry integrator from the fit; the 5 V forced-CCM entry overshoot traced to Q4's refresh drive (P2_L), holds after the rework (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-28, about 19:12 EDT |
| Operator | Claude Code, remotely through `bu`. Dan: "I'm ready to finish implementing ccm" (flashing for CCM approved earlier) |
| Firmware | 2690662 plus the entry change below, ELF sha256 `2c97f97873aebd1e…` |
| Load | 25.3 Ω on VBUS; the QT Py idle |
| Status | **Open. The integrator now starts where the bench needs it, and the 5 V start still overshot to SW_OVP. The integrator is not the cause.** |

## 1. The change

**Before.** After a start, CCM's integrator began at the formula value I R + 2 V_sd dt / T, about +0.04 V. The bench
needs −0.18 V at 5 V and −0.42 V at 20 V (400 kHz, 25.3 Ω).

**Now.**
- The integrator CCM last ran with is kept per setpoint band (below 7 V, 7–12 V, 12–17.5 V, above 17.5 V) across starts.
  It is scaled by V_ref within a band and tagged with the period it ran at.
- The first entry in a band starts from −(16 ‰ V_ref + 100 mV), the fit to the measured values (`CCM_U0_*`).
- **Host model** (new `test_entry`: 55 µF, the plant offset that gives the measured integrator, L1's entry offset):

  | Setpoint | Excursion, fit start | Excursion, formula start |
  |---|---|---|
  | 5 V | +0.13 / −0.10 V | +0.44 V |
  | 9 V | +0.14 / −0.12 V | +0.54 V |
  | 15 V | +0.03 / −0.01 V | +0.72 V |
  | 20 V | +0.22 / −0.23 V | +0.93 V |

  - **L1's entry offset:** the first CCM period starts from DCM's zero current, not from the ripple's value there.
    That is +0.58 A at 5 V and −1.01 A at 20 V.

## 2. The 5 V forced-CCM start at 400 kHz (19:12)

- **Sequence:** stop, `regulator_ccm_enable` 2, trace armed, then `sink req 9000`. The sink Hard Reset and the source
  restarted at 5 V.
- **SW_OVP 200 µs after CCM entered**, the same as 2026-10-23.

| t (ms) | V_out (mV) | Current column (mA) | On-time (ns) | |
|---|---|---|---|---|
| 25.60 | 4898 | 347 | 304 | DCM, 400 kHz |
| 25.65 | 4932 | 220 | 467 | CCM entry |
| 25.70 | 4949 | 557 | 467 | |
| 25.75 | 5105 | 535 | 466 | |
| 25.80 | 5208 | 528 | 463 | |
| 25.85 | 5381 | 515 | 456 | then SW_OVP (5.5 V) |

**What the trace shows:**
- **The on-time started at 467 ns.** Logged 5 V forced-CCM steady states: 476 ns, integrator −0.21 V (2026-10-21), and
  505 ns, +0.10 V (2026-10-22, section 2). So the duty started under both. The integrator read −0.42 V at the fault.
- **The current and V_out rose together.** The trace's current column read 0.51–0.56 A in CCM; V/R is 0.2 A. V_out
  rose about 0.45 V in 150 µs.
- **Inference, not measured:** at about 110 µF that is about 0.3 A more than the load, held for 200 µs. The host model's
  LC ring from L1's entry offset turns round within about 40 µs. So the starting value does not seem to be the cause.
- **Unchecked:** whether the 5 V entry on 2026-10-22 (section 5) came straight after a start, before any learned value.

**Not known:** what drives the rise.

## 3. Two more trips (19:18, 19:20)

**19:18, my sequencing error.**
- After `clear-fault` the board restarted from the PD stack before the scope and the trace were armed. The sink still
  targeted 9 V from the earlier request.
- I then wrote `regulator_ccm_enable` 2.
- **Fault snapshot:** commanded 9000 mV, V_out 10434 mV (SW_OVP), the CCM entry counter up by one, last exit
  OVER_BAND. `fsw_khz` read 200.
- No scope or trace record. The snapshot's currents are marked stale by the tool and are not used here.

**19:20, the planned start.**
- **Setup:** sink target 5 V, forced CCM, 400 kHz. The trace was armed, and the scope set to single on CH2 (Q3 gate,
  per Dan) rising through 9 V; then one `clear-fault`.
- **The trace repeats 19:12:** CCM entry at 25.65 ms, on-time 465 ns, V_out 4898 → 5398 mV in 200 µs, current column
  490–534 mA, then SW_OVP.
- **The scope did not trigger.** Dan: nothing on the screen.
- What Q3's gate did is not known: the missing trigger does not show its level.

## 4. The entry on the scope (19:23)

Dan: "ok to trip, just make sure the scope captures".

**Setup:**
- Start with `regulator_ccm_enable` 1 (by load): the board ran 5 V buck DCM at 400 kHz, CCM not active.
- Scope:
  - CH1 IL_MON (1×, 0.5 V/div);
  - CH2 Q3 gate (Dan; 10×, 5 V/div);
  - CH3 SW2 (10×, 2 V/div; Dan confirmed after the capture);
  - CH4 VBUS (10×, 0.5 V/div).
- **Trigger:** CH4 rising through 5.3 V with HF-reject coupling. CH4's spikes reach 5.56 V in steady DCM; with HF
  reject the scope did not trigger in 4 s of DCM. Single shot, 450 µs before and 150 µs after, 4 ns per point.
- Then `regulator_ccm_enable` 2. The scope triggered, and SW_OVP latched.

**Firmware trace:**
- 5 CCM samples, on-time 472 → 458 ns, V_out 5001 → 5467 mV, then SW_OVP.
- The current column read 486–557 mA.

**Scope, measured** (times from the trigger):

| Interval | CH2 (Q3 gate) | CH3 | CH4 VBUS mean | CH1 mean |
|---|---|---|---|---|
| DCM, before −106 µs | switching, −0.3 to 7.1 V; within about 0.3–0.7 V of CH3 throughout | switching, −0.7 to 7.6 V | 4.97 V | 0.04 V |
| −106 to −65 µs | held high, 9.2 V falling to 7.4 V; dips to about −2 V every 5 µs | held at 5.1–5.5 V; dips to about −2 V every 5 µs | 5.01 → 5.08 V | 0.05–0.12 V |
| −64 µs to the end | switching every 2.5 µs, −2 to 7.4 V; again within about 0.3–0.7 V of CH3 | about 7.7 V for about 1.6 µs, then about −2 V for about 0.6 µs, each period | 5.10 → 5.70 V (about 3 V/ms) | 0.10–0.12 V |

**Q3's gate drive, CH2 − CH3** (both 10× probes to board ground; Dan: CH3 is SW2), per 2.5 µs period mean:

| Interval | Q3 V_gs |
|---|---|
| DCM, before −107.5 µs | −0.17 V |
| −105 µs | +3.96 V |
| −90 µs | +3.05 V |
| −75 µs | +2.39 V |
| −67.5 µs | +1.94 V |
| −62.5 µs to the end | −0.26 to −0.31 V |

During the hold, the refresh dips every 5 µs do not bring V_gs back up: just before and after the −100.5 µs refresh it
read 3.69 V and 3.59 V. In the high parts of each period after −62.5 µs, CH2 sits 0.4–0.6 V under CH3 (for example
7.08 V against 7.70 V).

**Measured, IL_MON:**
- **−105 to −65 µs:** zero for 27–74 % of every period, peaks 0.19–0.49 V.
- **From −60 µs:** peaks 0.40–0.46 V.
- IL_MON reads only positive current, so its zeros may be negative current.

**Inference:**
- **The cause:** Q3's bootstrap droops from about 4 V to about 2 V over 40 µs after the entry, and the 200 ns / 5 µs
  refresh does not hold it. Q3 then drops out (driver UVLO?) and stays off.
  - With Q3 off, SW2 sits about 2.5 V above V_out while L1's current is positive.
  - It sits about 2 V below ground for about 0.6 µs each period, which is Q4's reverse conduction.
  - This pumps the output: VBUS rises from that moment.
- **The same signature as 2026-10-20** §3.1 (150 ns / 10 µs, 9 V, 200 kHz), which the 200 ns / 5 µs refresh fixed in
  steady state.
- **Not explained:** why Q3 does not come back once SW2 is at −2 V every period.

## 5. Refresh 400 ns every 7.5 µs (19:40)

Dan: "twice as long every 3 cycles instead of every 2".

- **Settings:** `regulator_ccm_refresh_ns` 400, `regulator_ccm_refresh_interval_ns` 7500, checked with the host build
  first: valid, longest pulse 1960 ns, refresh share 5.3 %.
- **Scope:** single on CH2 (Q3 gate) rising through 8.5 V, DC.
  - The first DCM check triggered (19:38), while Dan was adjusting CH3. Not used.
  - The second check stayed untriggered for 5 s of DCM.
- **The CCM start, from DCM 400 kHz:**
  - CCM entered at an on-time of 464 ns; V_out went 4984 → 5294 mV in 4 samples, then SW_OVP.
  - The same as the three 200 ns / 5 µs starts.
- **The scope did not trigger.** There is no capture of this entry.
- **After:** the refresh is back to 200 ns / 5 µs, `regulator_ccm_enable` 0.

Data: `data/trace_ccm400_5v_entry_rf400_7500_ovp.csv`.

## 6. Refresh 600 ns every period, 2.5 µs (19:42)

Dan: "go for a really strong refresh".
- **Settings:** 600 ns is the firmware's upper limit. Host check: valid, longest pulse 1760 ns, refresh share 24 % (4 %
  at the default). The integrator fit was made at 4 %.
- **Scope:** CH4 rising through 5.3 V with HF reject, 450 µs before and 150 µs after. It did not trigger in 5 s of DCM.
  CH3 is now 5 V/div (Dan).
- **Trace:**
  - CCM entered at an on-time of 369 ns.
  - V_out went 4984 → 5415 mV over 0.65 ms while the on-time fell to 312 ns; the integrator read −0.87 V at the fault.
  - Then SW_OVP.

**Scope, measured** (2.5 µs windows every 10 µs; V_gs = CH2 − CH3):

| t from the trigger | Q3 V_gs mean | CH4 VBUS mean |
|---|---|---|
| −450 to −230 µs (DCM) | −0.14 to −0.16 V | 4.96 V |
| −220 µs | +0.45 V (entry inside the window) | 4.98 V |
| −210 µs | +2.58 V | 5.01 V |
| −200 µs | +2.05 V | 5.05 V |
| −190 µs | +0.94 V | 5.08 V |
| −180 µs to the end | about 0 V (−0.13 to +0.07 V); single windows at +20 µs (+1.82 V) and +140 µs (+1.04 V) | 5.09 → 5.44 V |

**What this shows:**
- **Q3 still drops out,** 30–40 µs after the entry, against about 42 µs with 200 ns / 5 µs (section 4). A refresh 12× the
  default did not keep it on.
- **VBUS rose at about 1.3 V/ms,** against about 3 V/ms in section 4.

**Inference:**
- The drop-out does not depend on the refresh, so a bootstrap that runs out of charge is unlikely to be the cause.
- It falls within one PID cycle (50 µs) of the entry. A firmware write in the next cycle is a candidate, as is the
  driver itself (uP1966E).
- **Next:** CH2 on TB1 (PA10, the driver's Q3 input). That shows whether the MCU keeps commanding Q3 on.

**After:** the refresh is back to 200 ns / 5 µs, `regulator_ccm_enable` 0, the board in FAULT.

Data: `data/scope_ccm_entry_rf600_2500_1942_ch1234_4ns.csv`, `data/trace_ccm400_5v_entry_rf600_2500_ovp.csv`,
screenshot `scope_ccm_entry_rf600_1942.png`.

## 7. CH2 on PA10 (TB1, Q3's driver input), 19:47

Dan: "ch2 on PA10, go".
- **Settings:** default refresh (200 ns / 5 µs). CH2 at 1 V/div, offset −1.5 V, 10× probe setting. Same VBUS trigger as
  sections 4 and 6; it did not trigger in 5 s of DCM.
- **Trace:** CCM entered at an on-time of 471 ns; V_out went 5001 → 5467 mV in 5 samples; then SW_OVP (last exit
  OVER_BAND).
- **Scope:**
  - **CH2 read 0.00 V for the whole 600 µs window, with no edges.** That includes −100 to −65 µs, when SW2 (CH3) sat at
    4.6–5.9 V without switching, as in section 4 while Q3 was conducting.
  - The DCM check before the entry read CH2 −0.04 to +0.04 V.
- **Not known:** whether CH2 saw PA10. A 0.00 V flat line through an interval when Q3 was evidently on is not a valid
  TB1 measurement.

Data: `data/scope_ccm_entry_pa10_1947_ch1234_4ns.csv`, `data/trace_ccm400_5v_entry_1947_ovp.csv`.

**Retry at 19:48** (Dan: "oh it fell off, try again"):
- Same setup.
- **Trace:** CCM entry, then SW_OVP after 4 CCM samples.
- **CH2 read −0.05 to 0.00 V for the whole window, again with no edges.** Still not a valid TB1 measurement.
- **SW2 this time:** −2.5 to +9.5 V from −105 to −65 µs, so it was not held at V_out as in sections 4 and 7.

Data: `data/scope_ccm_entry_pa10_1948_ch1234_4ns.csv`, `data/trace_ccm400_5v_entry_1948_ovp.csv`.

**Probe check with the bench gate test (19:50).**
- **Setup:** IDLE, input and output paths off. `bench-ccm-pwm --t-on-ns 470 --fsw 400`; the firmware reported TA1, TA2,
  TB1 and TB2 enabled.
- **CH2 (PA10) read −0.04 to 0.00 V,** with no trigger at 1.65 V in NORM.
- **CH3 (SW2) read −2.6 to 5.2 V** with edges.
- Gate test off afterwards.
- **So CH2 is not seeing a switching TB1.** The probe contact or the point it is on needs checking.

Files: `scope_bench_ccm_pwm_pa10_1950.png`, `data/scope_bench_ccm_pwm_pa10_1950.csv`.

**2026-10-29, 17:17: probe replaced** (Dan: the old probe was broken).
- The board had rebooted to its boot defaults (5 V DCM, 200 kHz, CCM off). I stopped it to IDLE.
- **Gate test again (`bench-ccm-pwm --t-on-ns 470 --fsw 400`):** CH2 shows TB1 with a 5.00 µs period, high 95.2 %
  (4.76 µs). The low is about 240 ns, which is the 200 ns refresh plus two 20 ns dead times. CH2 read −1.5 to 4.9 V,
  with ringing on the edges.
- Gate test off.

Files: `scope_bench_ccm_pwm_pa10_1029.png`, `data/scope_bench_ccm_pwm_pa10_1029.csv`.

## 8. The entry with CH2 on TB1 (2026-10-29, 17:18)

**Setup:**
- Restart from IDLE with `sink req 5000` (Hard Reset, then a 5 V contract). `bu regulator start` is refused in the PD
  build.
- DCM at 400 kHz, `regulator_ccm_enable` 1, then 2. Default refresh 200 ns / 5 µs.
- **Scope:** CH1 IL_MON, CH2 PA10 (TB1, 1 V/div), CH3 SW2, CH4 VBUS. Trigger CH4 through 5.3 V with HF reject,
  450 µs before and 150 µs after; it stayed armed through 5 s of DCM.

**Trace:** CCM entered at an on-time of 470 ns; V_out went 4967 → 5467 mV in 5 samples; then SW_OVP (last exit
OVER_BAND).

**Scope, measured** (times from the trigger):
- **TB1:**
  - Low in DCM.
  - High from −111.63 µs, the CCM entry.
  - From there, low for 240 ns every 5.00 µs (falling at x.87, rising at x.63 µs), the refresh plus two dead times.
  - This continues unchanged to +132.8 µs. 49 rising and 49 falling edges, no others.
- **SW2:**
  - −110 to −75 µs: 4.6–5.9 V between refreshes.
  - At each refresh it swings to −2.5 to −3.4 V and up to 6–9 V.
  - From about −70 µs: −2.8 to +8.3 V every period, the pattern of sections 4 and 6 after Q3 dropped out.
  - At the refreshes from −66.9 µs it reads −2.5 to +0.9 V.
- **VBUS:** 4.97 V in DCM; 5.07 V at −70 µs; then rising to 5.24 V at −25 µs.

**What this shows:** the MCU keeps commanding Q3 on, with the normal refresh, through the whole event. Q3 stops
conducting about 40 µs after the entry while TB1 is still high. So the drop-out is on the driver / power side, not in
the firmware or HRTIM.

**Inference:**
- During the refreshes SW2 goes to −2.5 to −3.4 V, not near 0 V. That suggests Q4 is not turning on hard in the
  refresh, or is late.
- **Next:** probe TB2 (PA11) and Q4's gate.

Data: `data/scope_ccm_entry_tb1_1029_1718_ch1234_4ns.csv`, `data/trace_ccm400_5v_entry_1029_1718_ovp.csv`, screenshot
`scope_ccm_entry_tb1_1029_1718.png`.

## 9. Q4's gate in the bench gate test (2026-10-29, 17:23)

Dan: "ch2 on G4, maybe the low side isn't driving". The board was cleared, then stopped to IDLE with the input path off.

**Schematic** (`KiCad/dc-dc.kicad_sch`, connections traced from the file):
- **U3 (uP1966E, output leg):** Q3 turns on through R9 2.2 Ω and off through R10 1 Ω; Q4 turns on through R12 1 Ω and
  off through R11 2.2 Ω.
- **Bootstrap:** C6 100 nF from BOOT to SW2 through R8 0 Ω.
- **Supply:** VCC +5 V with C5 1 µF.

**Gate test** `bench-ccm-pwm --t-on-ns 470 --fsw 400` (firmware: TA1, TA2, TB1, TB2 enabled):
- **NORM trigger on CH2 rising through 2.5 V:** never triggered.
- **AUTO, triggered on SW2 falling** (24 µs, 4 ns per point):
  - **CH2 (G4) read −0.49 to +0.59 V for the whole capture.**
  - At the refreshes (SW2 under 0 V for 232–240 ns every 5 µs, down to −3.1 V), G4 read −0.3 to +0.4 V.
  - On 2026-10-29 at 17:17 the same test showed TB1 low for 240 ns every 5 µs, so TB2 should command Q4 on in those
    windows.

**So:** Q4's gate does not rise during the refresh, if the probe is on G4. The probe contact on G4 is not verified
separately; this probe read PA10 correctly at 17:17.

**Not known:** whether TB2 (PA11) reaches U3's LI pin.

**Next:** PA11 in the same gate test.

Files: `scope_bench_ccm_pwm_g4_1029_1724.png`, `data/scope_bench_ccm_pwm_g4_1029_1724.csv`.

## 10. P2_L (2026-10-29, 17:29–17:34)

Dan: "p2_l was disconnected; ch2 back on g3, rerun test".

**Gate test, 17:29** (input off; CH2 G3, CH3 SW2):
- SW2 rang throughout.
- The 240 ns refresh windows read SW2 −2.8 to +3.1 V.
- Not conclusive without power. Data: `data/scope_bench_ccm_pwm_g3_1029_1729.csv`.

**5 V CCM entry, 17:30** (restart by `sink req 5000`, DCM 400 kHz, then forced CCM; default refresh; VBUS trigger
through 5.3 V with HF reject):
- **SW_OVP again** (last exit OVER_BAND).
- The firmware trace had filled on a DCM error sample before the entry, so it has no CCM samples.

**Scope** (the window ran −324 to +276 µs from the trigger):

| t from the trigger | Q3 V_gs mean | VBUS mean |
|---|---|---|
| before −109 µs (DCM) | −0.26 to −0.28 V | 4.98 V |
| −104 µs | +3.39 V | 5.01 V |
| −89 µs | +2.64 V | 5.06 V |
| −74 µs | +2.06 V | 5.08 V |
| −69 µs | +1.87 V | 5.08 V |
| −64 µs to +131 µs | −0.24 to −0.31 V | 5.10 → 5.69 V |
| from +146 µs | about 0 V, switching stopped (the fault) | 5.67 V falling |

- **Q3 off windows in the hold (V_gs < 0.8 V):** 228–264 ns every 5 µs, from −102.8 µs to −67.8 µs.
  - SW2's median in each window is −2.46 V, range −3.4 to +8.3 V.
  - That is the same as section 8, before P2_L was reconnected.
- **During the hold SW2 also swung −2.8 to +8.6 V in every window,** and IL_MON peaked at 0.46–1.17 V. In sections 4
  and 8 SW2 sat at 4.6–5.9 V between refreshes.

**Inference:**
- At the refreshes SW2 still sits about 2.5 V below ground rather than near 0 V. So Q4 does not seem to turn on in
  the refresh, even after P2_L.
- **To check:** G4 in the gate test, as in section 9.

Files: `scope_ccm_entry_g3_1029_1730.png`, `data/scope_ccm_entry_g3_1029_1730_ch1234_4ns.csv`.

**Correction to the 17:29–17:30 runs:** Dan, at 17:33: "I still had it disconnected". So P2_L was still open during
them.

**Gate test with CH2 on G4, 17:32:** CH2 read exactly 0.00 V, with no noise. That is the signature of the broken or
detached probe on 2026-10-28, so not used.

**5 V CCM entry after Dan reconnected P2_L, 17:34** (CH2 G3, CH3 SW2, CH4 VBUS; default refresh):
- **Trace:** CCM entered at an on-time of 472 ns; V_out went 5001 → 5450 mV in 5 samples; then SW_OVP (last exit
  OVER_BAND).
- **Scope:**
  - **Q3 V_gs:** +3.86 V at −109 µs, falling to +2.06 V at −74 µs, then off from −69 µs.
  - **VBUS:** 4.96 V in DCM, 5.07 V at −74 µs, 5.68 V at +126 µs.
  - **The refreshes** at −105.5 µs to −70.5 µs, 232–268 ns each: SW2's median −2.16 to −2.46 V, minimum −2.8 to
    −3.4 V. The same as section 8.
- **So:** no change from before the reconnection. Whether G4 is driven is still unmeasured.

Files: `scope_ccm_entry_g3_1029_1734.png`, `data/scope_ccm_entry_g3_1029_1734_ch1234_4ns.csv`,
`data/trace_ccm400_5v_entry_1029_1734_ovp.csv`.

## 11. After Dan's rework (2026-11-02, 20:36)

Dan: "back after some rework with Ch1 on sw1, ch2 on pa11, ch3 on sw2, and ch4 on TP2". The board was IDLE, uptime
636 s.

**Gate test** `bench-ccm-pwm --t-on-ns 470 --fsw 400` (input off; TA1, TA2, TB1, TB2 enabled):
- **CH2 (PA11, 1 V/div):** −1.14 to +2.08 V, mean 0.49 V.
  - A slow ramp around 0.5–1 V with spikes every 2.5 µs.
  - Crossings of 1.65 V last 0–20 ns.
  - No 200 ns logic pulse every 5 µs, which TB2 should give.
- **Others:** CH1 (SW1, scope set 1X) −0.3 to 3.1 V; CH3 (SW2) −1.8 to 5.2 V; CH4 (TP2) 2.46–4.09 V, mean 2.47 V.
- **Not a valid TB2 measurement.** Asked Dan about the probe on PA11 and the TB2 → P2_L wiring.

Files: `scope_bench_ccm_pwm_pa11_1102_2036.png`, `data/scope_bench_ccm_pwm_pa11_1102_2036.csv`.

**Gate test again, 20:43** (Dan: "probe fixed on pa11, ch1 is 10x"; CH1 set to 10X, 5 V/div):
- **CH2 (PA11):** 196 ns pulses at 3.31 V every 5.00 µs, 5 of them in 24 µs. This is TB2's refresh.
- **SW2 during each TB2 pulse:** median 0.31 V, range −0.31 to +1.85 V. In the CCM entries of sections 8 and 10 SW2 sat
  at a median of −2.46 V in the refreshes. That run was with power, this one with the input off, so the two are not
  directly comparable.
- **CH1 (SW1):** −2.8 to +33.4 V (edges and ringing). CH4 (TP2): 2.46–3.14 V.

Files: `scope_bench_ccm_pwm_pa11_1102_2043.png`, `data/scope_bench_ccm_pwm_pa11_1102_2043.csv`.

## 12. The 5 V forced-CCM entry holds (2026-11-02, 20:49–20:52)

**Setup and conditions:**
- CH1 SW1 (10×), CH2 PA11 (TB2), CH3 SW2, CH4 TP2 (VOUTa, the board node). No current channel on the scope this run
  (Dan: "note the loss of id_mon on the scope").
- Before the run the USB-C cable had to be reconnected (no attach since boot) and the 25.3 Ω load reconnected. Without
  the load, DCM read I_cmd 0, I_out 33 mA and skipped pulses at 5.1–5.3 V.
- With the load: I_cmd 329 mA, I_out 232 mA, on-time 301 ns, V_out 5053 mV.
- **Scope trigger:** CH4 through 5.3 V with HF reject, single; it stayed armed for 5 s of DCM. The scope's window is
  −324 to +276 µs (offset −24 µs).
- Then `regulator_ccm_enable` 2. Default refresh, 400 kHz.

**PD during the run:**
- Since the cable reconnect, the charger logged 10 attaches and 10 detaches, and 5 contracts.
- After one detach the board was IDLE with the last CCM exit STOP, and no fault. It then re-attached and restarted on
  its own.
- The sink's NUCLEO (ST-LINK VCP) was no longer on USB at 20:52.

**Firmware trace (armed before the write; it captured one of the restarts):**
- Output switch closed at 4.95 ms; CCM entered at 24.9 ms, after CCM_SWITCH_SETTLE.
- **During CCM, 24.9 to 50.0 ms (503 samples):** V_out 4880–5087 mV, mean 4998 mV, σ 24.5 mV. On-time 463 → 478 ns.
  The current column read 168–406 mA in the first 0.2 ms, then 228–266 mA.
- **No overshoot.** This is a forced-CCM start at 5 V / 400 kHz, the case that latched SW_OVP on 2026-10-23 and 10-28.

**Live status, 20:52, three reads 1 s apart:**
- RUNNING, CCM active (entries 4, exits 3); V_out 4967–5001 mV, V_in 24.4–24.5 V.
- On-time 477–478 ns, integrator −0.122 to −0.131 V, CCM current 242–244 mA, not limiting, no fault.

**Scope:**
- The trigger fired at some point in these restarts.
- **The capture:** TP2 rising 4.64 → 5.75 V over 600 µs (about 1.9 V/ms), with TB2 low throughout and SW2 in its DCM
  pattern (−0.3 to 7.4 V). That is DCM, not a CCM entry.
- **Not explained.** It fell in the attach/detach period.

**Inference:** with P2_L reworked, the 5 V forced-CCM entry no longer overshoots. Not yet measured: Q3's V_gs (no probe
on G3 this run) and whether the refreshes now pull SW2 to ground with power.

Files:
- `data/trace_ccm400_5v_forced_start_1102_2051.csv`;
- `data/scope_tp2_rise_dcm_1102_2052.csv`;
- `scope_tp2_rise_dcm_1102_2052.png`.

Files:
- screenshot `scope_ccm_entry_1923.png`;
- `data/scope_ccm_entry_1923_ch1234_4ns.csv`;
- `data/trace_ccm400_5v_entry_1923_ovp.csv`.

**End state:** FAULT (SW_OVP), outputs off, `regulator_ccm_enable` 2, `regulator_buck_fsw_khz` 400, sink target 5 V.
Not cleared.

Data: `data/trace_ccm400_5v_entry_u0fit_ovp.csv` (19:12), `data/trace_ccm400_5v_entry_1920_ovp.csv` (19:20).
