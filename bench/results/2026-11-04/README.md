# Bench 2026-11-04: load steps 5–28 V: noise, regulation, efficiency estimate (IS_MON / ID_MON)

| | |
|---|---|
| Firmware | `claude/ccm-hf-buck-50w` at 5461c61 plus `IS_MON_ADC3_MEAN` (below), CCM auto (`regulator_ccm_enable` 1) |
| Supply / load | V_in 24.24–24.43 V (VS_MON). 25.3 Ω on VBUS always. "Heavy": plus the QT Py load at 7 W (10 Ω params; ~6.4 W into its 11 Ω). PD sink: G431, 3 A requests |
| Instruments | Regulator trace (VD_MON at TP2, 20 kHz, 102 ms); scope CH4 = TP2 (AC, 20 MHz BWL), CH1 = SW1 (TP3), CH3 = SW2 (TP4), CH2 = CC1 (unused) |
| Script / data | `scripts/steps.py` (run with `tools/bringup/.venv/bin/python`), `data/steps.jsonl`, `data/traces/`; switch nodes `scripts/edges.py`, `data/sw/` |

## 0. Firmware change: IS_MON period mean (ADC3)

IS_MON was sampled once per period in the quiet phase (ADC1, with VD_MON), so in buck DCM it missed the input pulse:
31 mA at 15 V / 8.9 W out. PB0 is also ADC3_IN12. ADC3 now converts it free-running with 256× oversampling, the same
as ID_MON on ADC2 (`adc_monitor.c`, `IS_MON_ADC3_MEAN`). After the change it read 426 mA at the same point.

- **Zero offset:** with `regulator stop` (input path off) ADC3 reads 9 counts = **29 mA** (0.71 W at 24.4 V). Not
  resolved whether this is amplifier offset or real current upstream of the input switch. It is **not** subtracted below.
- **Gain:** not checked against a meter.

## 1. Efficiency estimate (light = 25.3 Ω only, so P_out = V²/R is exact)

| Contract | Mode | P_out V²/R | P_in IS_MON | **η = V²/R ÷ IS** | η = ID ÷ IS | ID_MON ÷ V²/R |
|---|---|---|---|---|---|---|
| 5 V | buck DCM | 0.99 W | 2.06 W | **48 %** | 57 % | 1.19 |
| 9 V | buck DCM | 3.20 W | 4.76 W | **67 %** | 73 % | 1.09 |
| 15 V | buck DCM | 8.89 W | 10.88 W | **82 %** | 85 % | 1.04 |
| 20 V | buck DCM | 15.81 W | 18.21 W | **87 %** | 91 % | 1.05 |
| 24 V | band CCM | 22.77 W | 23.63 W | **96 %** | 101 % | 1.04 |
| 28 V | boost CCM | 30.99 W | 32.18 W | **96 %** | 78 % | 0.81 |

Heavy (P_out has the QT Py's unmeasured input, so only ID_MON): 15 V DCM 86 %, 20 V buck CCM 97 %, 24 V band 102 %,
28 V boost 83 %.

- **ID_MON is not usable for η outside buck/band at the 1.04–1.05 ratio.** It reads 19 % low in boost CCM, and its
  heavy-load η at 24 V is above 100 %.
- **The buck DCM loss is mostly reverse conduction (§5).** In buck DCM only TA1/TA2 are enabled
  (`regulator_sr_enable` 0), so Q4 is never driven.
  - The whole output current flows through Q4 in reverse, at 2.1–2.6 V (2.6 V calibrated, 2.1 V uncalibrated).
  - Q2 freewheels in reverse at 1.9 V.
  - The loss scales with I_out, not as a fixed loss (estimate = V_sd × I_out; inference):

  | Contract | Loss P_in − V²/R | Q4 estimate | Q2 estimate |
  |---|---|---|---|
  | 5 V | 1.07 W | 0.42–0.51 W | not captured |
  | 9 V | 1.56 W | 0.75–0.93 W | not captured |
  | 15 V | 1.99 W | 1.25–1.54 W | 0.35 W (1.9 V × 0.185 A, triangle from the 660 ns freewheel) |
  | 20 V | 2.40 W | 1.66–2.06 W | not captured |
  | 24 V band CCM | 0.86 W | Q4 driven | |
  | 28 V boost CCM | 1.19 W | Q4 driven | |

  This replaces the earlier inference of a fixed ~0.9 W loss and a mode-dependent IS_MON error.
- **IS_MON is still unverified** in gain and in its 29 mA zero. If the 29 mA is pure offset, the 24 V band loss
  would be 0.15 W (99 %), which is implausible. So either the 29 mA is real current or the gain reads low. One meter
  reading would settle it.

## 2. Regulation (VD_MON trace mean, 102 ms)

- **Set point:** within ±0.7 mV of target at every point, light and heavy (≤ 0.005 %).
- **Load regulation light → heavy:** ≤ 0.9 mV (≤ 0.006 %).
- **Absolute accuracy:** rests on the VD_MON calibration (2026-10-17); it was not independently metered today.
- **At the load:** the QT Py's VIN tap read 13.58 / 18.28 / 21.84 / 25.52 V at 15 / 20 / 24 / 28 V. That is about 9 %
  low at every voltage. **Inference:** proportional, so its tap calibration rather than drop.

## 3. Noise

| Contract | Mode light → heavy | LF sd / p-p, light (trace) | LF heavy | Ripple p-p / rms, light (200 ns avg, 20 MHz) | Ripple heavy |
|---|---|---|---|---|---|
| 5 V | DCM | 24 / 189 mV | — | 79 / 15 mV (1.6 %) | — |
| 9 V | DCM | 31 / 241 mV | — | 146 / 29 mV (1.6 %) | — |
| 15 V | DCM → DCM | 34 / 259 mV | 53 / 466 mV | 276 / 70 mV (1.8 %) | 393 / 104 mV |
| 20 V | DCM → buck CCM | 33 / 293 mV | 59 / 380 mV | 368 / 97 mV (1.8 %) | 596 / 162 mV (3.0 %) |
| 24 V | band → band | 37 / 275 mV | 61 / 500 mV | 377 / 60 mV (1.6 %) | 455 / 71 mV |
| 28 V | boost → boost | 39 / 259 mV | 68 / 604 mV | 682 / 192 mV (2.4 %) | 751 / 205 mV (2.7 %) |

- **Raw edge spikes** (0.1–99.9 % span):
  - 0.8–1.0 V in DCM.
  - About 5 V in CCM. These ran past the 0.5 V/div screen, so they are clipped; the earlier inference is probe pickup.
- **Slow wander:** the 1.2 s scope records at 15 V show ±0.10–0.15 V at roughly 5–8 Hz, with and without the QT Py.
  The 102 ms trace is too short to resolve it.

## 4. Load steps

**A: `switch-pulse 10` ms.** The output switch opens, so 25.3 Ω, the sink and the QT Py's input are all off the
regulator, then it recloses.
- Q5/Q6 let go 0.8–4.6 ms after OUTPUT_EN falls (V first above +1 %, DCM points).
- The release is a clean full-load → 0 step.
- The reconnect is a **hot-plug** into the VBUS side, which has sagged through the 25.3 Ω for the rest of the 10 ms.
- Columns are from the trace; the scope dip (10 µs avg) is in brackets.

| Contract | Mode | Release overshoot | Reconnect dip | Reconnect overshoot | Back within 5 % | Within 1 % |
|---|---|---|---|---|---|---|
| 5 V | DCM | +469 mV (+9.4 %) | −1.08 V (−22 %) [−1.16] | +5.9 % | 1.85 ms | 77 ms |
| 9 V | DCM | +487 mV (+5.4 %) | −3.43 V (−38 %) [−3.49] | +2.7 % | 1.45 ms | 75 ms |
| 15 V | DCM | +920 mV (+6.1 %) | −7.01 V (−47 %) [−7.24] | +2.0 % | 1.05 ms | 29 ms |
| 20 V | DCM | +1111 mV (+5.6 %) | −9.12 V (−46 %) [−9.10] | +1.8 % | 0.85 ms | 21 ms |
| 24 V | band CCM | +93 mV (+0.4 %) | −3.20 V (−13 %) [−4.31] | +5.9 % | 0.15 ms | 0.25 ms |
| 28 V | boost CCM | +113 mV (+0.4 %) | −4.56 V (−16 %) [−5.06] | +6.3 % | 0.15 ms | 0.15 ms |

- **DCM releases:** they keep rising for as long as the switch is open, so the overshoot depends on the open time.
- **CCM:** holds the release to 0.4 % and recovers from the hot-plug in ≤ 0.25 ms.
- **DCM:** the tail to ±1 % takes 21–77 ms.
- **Faults:** none at any point. Current limit 0 samples, 0 hard resets, 0 detaches.

**B: QT Py disarm** (≈7 W off at once, 0.25–0.5 A). Not resolved.
- At 15 V it was ≤ +221 mV in the 100 ms after, against 559 mV p-p of wander in the 100 ms before.
- The 20–28 V records clipped on switching spikes at 0.5 V/div and are invalid.
- A resolvable step needs a bigger switched load (the QT Py's 25 W overload build) or a trigger tied to the disarm.

## 5. Switch nodes: cross-conduction and rectification (CH1 = SW1, CH3 = SW2; 1 GSa/s single channel, 100 MHz)

Edges are at 20 ns/div on one channel; periods are at 1 µs/div on CH1 + CH3. The levels are calibrated from known
levels in the same records, because 5 V/div DC is only good to about ±1 V:
- CH1: Q1 on = V_in, Q2 on = 0, giving gain 1.058 and offset −1.02 V.
- CH3: Q3 on = 0, Q4 on = V_out at 28 V, giving gain 1.03 and offset −1.0 V.

Edge values below are raw readings.

| Point | SW1 (Q1/Q2) | SW2 (Q3/Q4) |
|---|---|---|
| 28 V boost CCM | Q1 held on | Rising: 30.6 V plateau (Q4 reverse) for ~12 ns, then Q4 on at 28 V. Falling: Q3 hard on |
| 24 V band CCM | Rising: −3.3 V plateau (Q2 reverse) for ≥ 18 ns, then Q1 hard on in ~4 ns (rings to 29.7 V). Falling: partial soft fall 24 → 11 V over ~18 ns, then Q2 on | Falling: 26.4 V plateau (Q4 reverse) for ~10 ns, then Q3 pulls it to 0 in ~6 ns (±10 V ringing) |
| 20 V forced buck CCM | Rising: soft partial rise to ~8 V (negative valley current), then Q1 on. Falling: linear fall to −2.2 V in ~18 ns (Q2 reverse), then Q2 on (−1.0 V raw = 0 V) | Q4 on at V_out, except a ~300 ns low every period (Q3 refresh, then Q4 reverse) |
| 15 V buck DCM | Q1 on 1442 ns (t_on 1454); Q2 **not driven**, freewheel at −1.9 V (calibrated) for 660 ns; then DCM ringing | **Q4 not driven**: 17.6 V (calibrated) = V_out + 2.6 V during the whole conduction; rings afterwards |

- **Cross-conduction: none seen on either leg.** Every hard edge is preceded by a dead-time plateau, the other
  switch's reverse conduction, of the expected ~10–20 ns (dead time 20 ns). No edge is held at an intermediate
  level while both switches are on.
- **Dead-time loss in CCM:** about 2.5 V × 1.3 A × 2 × 20 ns × 200 kHz ≈ 26 mW per leg.
- **Corrections to the first version of this README:**
  - "SW1 not checked, probe on CC1" was wrong: CH1 is SW1, CH2 is CC1.
  - "SW2 doesn't switch in buck" was wrong:
    - In buck CCM SW2 drops for the ~300 ns refresh every period.
    - In buck DCM Q3/Q4 are both off, and SW2 follows Q4's reverse conduction and the DCM ringing.
- **The DCM rectification loss is the open item, not cross-conduction.** `regulator_sr_enable` (default 0) adds
  rectifier windows. On 2026-10-15 the first build without fade-in latched SW_OVP at 5 V. The next build, with a
  100 ms fade-in, held 5–28 V with no fault: freewheel SW1 −0.5 V instead of −3 V, and the user reported the FETs
  cooler. It ran at 28 V on 2026-10-19. The default is still 0.
  With the ADC3 IS_MON mean, an SR 0/1 A/B at 15 V and 20 V would show the gain directly, since P_out = V²/R is the
  same in both.
