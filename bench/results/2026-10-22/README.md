# Bench log 2026-10-22: ab2443e flashed; forced CCM handbacks traced to VD_MON noise (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-22, about 21:06 – 21:31 EDT |
| Operator | Claude Code, remotely through `bu`. Dan: "OK to flash for CCM and PPS", then "go ahead with the traced CCM rerun" |
| Firmware | ab2443e, ELF sha256 `c187e98e8b488f49…`. Defaults: CCM's current is ID_MON (gain 952 ‰), limit 3 A |
| Hardware since 2026-10-21 (Dan) | 1× coax from IL_MON to scope CH1. A VCONN switch wire near D10 (PB6 = CC1) came loose; the CC1 VCONN enable (PA7) reads high in `pd status` |
| Load | 25.3 Ω on VBUS; the QT Py idle |
| Status | **ab2443e: forced CCM at 9 V handed back to DCM on over-band every few ms, once SW_OVP. The V_out reading was about 5× noisier than on 2026-10-21 while VBUS was quiet. With timer-synchronized ADC scans (section 5, 737665e plus the median, flashed at 21:49) the same 5 / 9 / 20 V forced-CCM sequence at 400 kHz runs with no exits and 9 V trace σ 25.7 mV (94.7 before)** |

## 1. First attempt (21:06): SW_OVP at 9 V

- **Setup:** stop, `regulator_ccm_enable` 2 and `regulator_buck_fsw_khz` 400, then a 9 V and then a 20 V request from the sink.
- **At 9 V:** CCM left on OVER_BAND seven times in 26 ms, 120 entries in all, then latched SW_OVP.
  - Fault snapshot: V_out 9968 mV, setpoint 9000 mV, V_in 24.06 V.
- **The 20 V request:** never reached ±5 % in tPSTransition, so the sink sent a Hard Reset each time.
- **Recovery:** cleared after CCM was set back to 0 and 200 kHz.

## 2. Traced rerun (21:28)

| Point | Trace σ V_out | Mean \|ΔV\| per 50 µs sample | Max \|ΔV\| | CCM exits | 2026-10-21, same point |
|---|---|---|---|---|---|
| 5 V hold, CCM | 19.7 mV | — | — | 0 | 38.3 mV |
| 5 → 9 V ramp | — | — | 586 mV | 10 OVER_BAND | 1 (RAMP_DOWN) |
| 9 V hold, CCM | 94.7 mV, p-p 603 mV | 139 mV | 586 mV | 69 more, all OVER_BAND | σ 36.9 mV, mean \|ΔV\| 28 mV, max 173 mV |

- **VBUS on the scope at the 9 V hold, as 45 µs means** (what VD_MON's trimmed mean spans): σ 11.7 mV, p-p 90 mV, mean
  sample-to-sample change 11.9 mV. The output is quiet and VD_MON is not. The over-band check takes two samples over
  the band, so the measurement noise alone makes CCM hand back to DCM.
- **The integrator** sat at +102 mV at 5 V, against −210 mV on 2026-10-21. The on-time was 505 ns against 476 ns.
- **ID_MON** read normally: 221 mA at 5 V and 374–386 mA at 9 V, as the 2026-10-21 fit predicts. The 616–648 mA of
  attempt 1 was taken during the handback cycling.
- **5 V DCM** trace σ was 18–26 mV today, against 12.7 mV on 2026-10-21.
- **IL_MON on CH1** (5 V DCM, 2 µs/div) is a clean, slow hump of about 0.45 V per pulse, lagging SW1 by about 1 µs.
  There is no ringing from the coax load.

Data: `data/trace_ccm400_5v_hold.csv`, `data/trace_ccm400_5to9v_overband.csv`, `data/trace_ccm400_9v_hold.csv`.

## 3. State at 21:31

- ab2443e, RUNNING, 5 V DCM, CCM off, no fault.
- Scope: CH1 IL_MON (1×, 0.2 V/div); CH2–CH4 as the parent left them; trigger CH2 rising 12 V, AUTO.

## 4. Dan's direction (21:3x)

- The coax stays: it gives the scope a better inductor-current view, and the probe ground moved slightly, which
  changed the noise.
- The loose wire was CC1, reattached.
- Priorities: timer-synchronized ADC reads, so the 1 nF caps can come off both dividers, and a fast voltage loop at
  100 kHz. Don't rely on IL_MON.

## 5. Timer-synchronized scans on hardware (21:47 – 21:51)

**Firmware.** 737665e plus the V_out median (the next commit). ELF sha256 `4e9e5eb994da78d9…` was the first flash; the
median build replaced it at 21:49.
- Once per switching period, HRTIM ADC trigger 1 on Timer C CMP4 starts ADC1 [VD_MON, IS_MON] and ADC2
  [VS_MON, ID_MON] together.
- 24.5-cycle samples for the dividers, no oversampling.
- The trigger is placed at 3.6 µs into the 5 µs period outside CCM, and centered in the longer of Q1's and Q2's
  intervals in CCM.
- Plan section 9.

**First flash: SW_OVP at the first start (5 V DCM).**
- The 64 VD_MON samples before the trip held outliers from 0 to 567 counts (0–9.8 V) around 290 (5.0 V).
- The trimmed mean of 9 drops one high and one low sample. It read 5.59 V.
- In steady state afterwards the outliers are rare, so these were most likely the start-up transients:
  - the output switch closing into the drained VBUS capacitance;
  - which the old 3.6 µs window had smoothed.
- **Fix:** the V_out estimate is now the median of 9, and the skip watchdog needs 3 consecutive out-of-band samples
  instead of 2.

**With the median build:**

| Point | Ring outliers (> 12 counts from the median) | Trace σ V_out | Mean \|ΔV\| | Max \|ΔV\| | CCM exits |
|---|---|---|---|---|---|
| 5 V DCM 200 kHz | VD 1 / 384, VS 0 / 192 | 24.8 mV (23.8 at the end) | — | — | — |
| 5 V forced CCM 400 kHz | VD 16 / 384 | 32.0 mV | 37.5 mV | 173 mV | 0 |
| 9 V forced CCM 400 kHz | VD 14 / 384 | 25.7 mV | 28.4 mV | 156 mV | 0 |
| 20 V forced CCM 400 kHz | VD 32 / 384 (later reads: ±13–37 counts), VS 16 / 192 | 44.5 mV | 48.8 mV | 500 mV | 0 |

- **CCM exits:** the up-ramps 5 → 9 → 20 V passed with no CCM exit, and the 400 mV error trigger never fired. Section
  2 had 200 exits at 9 V on the old sampling.
- **PD:** 20 V was negotiated at the first attempt (transition 145 ms).
- **Comparison with 2026-10-21 (windowed VD_MON, before the coax):** 9 V σ 36.9 mV, 20 V 24.1 mV. 20 V is noisier now
  and not yet explained. The outliers show no tie to the Q4-refresh periods.
- **Currents:**
  - ID_MON read 436 mA at 9 V and 744 mA at 20 V (synchronized, 8-scan means), against 0.36 / 0.79 A V/R plus the QT
    Py.
  - `il_ma` read nonsense: the clipping correction ran on IL_MON, which isn't converted any more. Fixed in the next
    commit (not flashed).

Data: `data/trace_sync_dcm_5v.csv`, `data/trace_sync_ccm400_{5,9,20}v.csv`.

## 6. `switch-pulse 20` at 20 V / 400 kHz with synchronized scans and the 3 A ID_MON limit (2026-10-23 13:40)

Dan: "Ok to try 20v".
- **Setup:** forced CCM at 400 kHz, the 20 V contract, the 25.3 Ω, the median build, defaults: limit 3 A on ID_MON
  (gain 952 ‰), `regulator_ccm_peak_ma` 0.
- **Scope:** CH1 IL_MON (1× coax), CH3 SW2, CH4 VBUS; triggered on IL_MON rising through 0.8 V (about 3.2 A).

**No fault.** The regulator stayed RUNNING in CCM (one OVER_BAND handback and re-entry, one limit entry). On
2026-10-20 the same pulse, without the limit, dipped −5.2 V, rang to +2.9 V and latched SW_OVP.

| From the reconnect | Board V_out (trace, 50 µs) | ID_MON | Limit |
|---|---|---|---|
| −0.05 ms (switch open) | 20.01 V | 7 mA | — |
| +0.05 ms | 19.82 V | 0.94 A | — |
| +0.10 ms | 16.11 V | 5.23 A | engaged, v_lim from the measured V_out |
| +0.15 ms | 15.44 V (minimum) | 5.19 A | |
| +0.25 ms | 15.70 V | 0.30 A | |
| +1.0 / +2.0 / +3.0 ms | 16.57 / 17.52 / 18.45 V | 0.6 A | v_lim rising about 1 V/ms (kl × (3 A − 0.6 A)) |
| +4.35 ms | about 19.5 V | | hands back |

- **Overall:** maximum after the reconnect 20.16 V, no overshoot. Below 19 V (vSrcNew) for about 3.5 ms, inside PD's
  5 ms.
- **Host model** (2026-10-22 explore run): dip to 15.2–18 V, peak 20.4–20.8 V.
- **Scope (5 µs means), the reconnect:**
  - VBUS on the bench side had drained to 3.2 V during the open.
  - L1 carried about 6.5 A (IL_MON, bandwidth-limited) for about 75 µs.
  - VBUS recharged from 3.2 to 15.4 V in about 125 µs.
- **VBUS-side capacitance:** the charge balance (L1 about 600 µC plus the board ceramics about 75 µC, over 12.2 V) gives
  about 55 µF effective at 3–15 V. Dan estimated more than 200 µF at 0 V; ceramic capacitance falls with bias, so the
  two agree.
- **The cost is the slow recovery.** Once the inrush is over, the limit's v_lim climbs at kl × (I_lim − I). That is
  kept low for battery stability, so it takes 4 ms to get back to 20 V with only 0.6 A flowing.

Data: `data/trace_switch_pulse20_ilim3a_sync.csv`, `data/scope_switch_pulse20_ilmon_sw2_vbus_5us.csv`.

## 7. End state

The board runs the median build (737665e plus the median) at boot defaults, CCM off: 5 V contract, buck DCM 200 kHz,
no fault. The scope is back to CH1 IL_MON, CH2 SW1, CH3 SW2, CH4 VBUS, 2 µs/div, CH2 trigger.

## 8. Next

1. **The 1 nF caps:** with the scans synchronized and 24.5-cycle samples, the firmware no longer needs them. Remove one
   divider's cap, then repeat the table above.
2. **The 100 kHz voltage loop:** the host model says a faster loop on V_out alone does not change the load step on
   this hardware (plan section 9). The fast interrupt is not built yet; Dan's decision.
3. **The limit's recovery after an inrush** (section 6): a faster climb of v_lim while I is far under the limit,
   checked against the battery cases in the host test.
