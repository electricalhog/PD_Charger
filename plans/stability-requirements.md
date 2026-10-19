# Stability requirements register

Status 2026-10-19, firmware = da5a649, i.e. 5ccd2c5 plus the 2026-10-17 trace/SWITCH_PULSE instrumentation (no
control-law, gain or limit change; runs 1-3 of the pulse series used an earlier uncommitted build, R§3.3). One row per requirement. Paths are from the repo root.

**Basis tags.** [PD] USB-IF Source Power Requirements Test Specification rev 0.78 (2020-05-06) and
the 2019 Developer Days slides; the numbers below are from those, not the base-specification
tables, and are indicative until checked against the PD 3.2 tables. [Plan] an acceptance
criterion already written in `plans/epr-bringup.md` or `plans/240w-roadmap.md`. [Prop] proposed
here; no earlier criterion existed.

**Status.** Closed: measured, criterion met, conditions cover the use. Partial: met, but not at
every voltage, load or rectifier state of the use, or against a stand-in stimulus. Fail: measured,
not met. Open: not measured. Calc: model or arithmetic only.

**Terms** (ΔV_us, ΔV_os, T_cmd, T_release, T_unl, E_bg, and what "dip" means):
`bench/results/2026-10-17/report/TEST_REPORT.md` section 2. Report sections are cited as `R§`.

PD limits used (vSrcNew = ±5 % of nominal, vSrcValid = vSrcNew ± 0.5 V beyond; tSrcTransient 5 ms
for loads ≥ 60 mA; load steps in 25 % increments at 150 mA/µs; asserted at the receptacle):

| Contract | vSrcNew lower / upper (V) | vSrcValid lower / upper (V) |
|---|---|---|
| 5 V | 4.75 / 5.25 | 4.25 / 5.75 |
| 20 V | 19.00 / 21.00 | 18.50 / 21.50 |
| 28 V | 26.60 / 29.40 | 26.10 / 29.90 |

## A. Load events (drop, reconnect, step)

| ID | Requirement and acceptance | Basis | Status | Evidence | Next test |
|---|---|---|---|---|---|
| A1 | After a load attach or step, V_out stays inside vSrcValid and spends ≤ 5 ms between vSrcNew and vSrcValid. Applied here to the converter node for the Q5/Q6 reconnect | [PD] 7.1.9 #1–#3; [Prop] for the converter node | Fail at 5 V; Partial at 20 and 28 V | R§6.7. 5 V: 3 of 3 events beyond vSrcValid (ΔV_us 1.22–1.24 V), window dwell ≤ 0.90 ms. 20 V: 2 of 5 beyond (T_unl ≥ 1.85 ms). 28 V: 2 of 5 beyond, SR on 3 of 8 (T_unl ≥ 1.30–1.60 ms). Not measured at the receptacle | F1, F3 |
| A2 | Release overshoot stays under the upper vSrcNew limit (+5 %: +0.25 / +1.00 / +1.40 V at 5 / 20 / 28 V) | [PD]; [Plan] epr-bringup Task 1 item 2 | Fail at 5 V (converter node); Partial at 20, 28 V | R§6.4. ΔV_os 0.33–0.36 V at 5 V (pulse skipping engaged while open), 0.19–0.29 V at 20 V, 0.22–0.47 V at 28 V, all for a soft release (T_unl 0.6–3.6 ms). 2026-10-10, a real load release: +2.3 % at 15 and 20 V (`bench/results/2026-10-10/README.md`, before the current-command loop) | A3 |
| A3 | Abrupt removal of ≥ 25 % of the offered 3 A (0.75 A) leaves the source running: no latched fault, no Hard Reset, V_out under the upper vSrcNew limit within 5 ms | [PD] 7.1.9; [Prop] fault criterion | Fail (2026-10-16) | Pulling the 25 Ω at 28 V (≈1.1 A to 0.09 A, SR off) latched HW_FLT1 (VS_GOOD low): last snapshot V_out 27.98 V, V_in 24.0 V, command 1455 mA; one Hard Reset, ≈95 sink re-attaches while latched (`bench/results/2026-10-16/README.md` step 8). No V_out record exists. The 16 Q5/Q6 pulses at 28 V never faulted, because the switch releases softly (R§6.4) | Pull at 5, 20, 28 V in that order with `trace-arm --trigger error --threshold-mv 300`, scope on VBUS and on the board input (J2), read PWRGD. Unknown whether the trace keeps sampling after the latch |
| A4 | No ringing after a load event: ζ ≥ 0.4 (2026-10-10 method), no limit cycle, no integrator windup | [Plan] epr-bringup Task 1 item 4 | Partial | 2026-10-10 (voltage-mode loop, before a4f9d9c): ringing 263 / 429 Hz, ζ ≈ 0.2. 2026-10-11 (current-command loop): pass on a release only, since the load ramps on over ≈1 s; 0.5 W release overdamped, ζ ≈ 3.5. 2026-10-17: rms(V_out − V_0)/σ_pre 0.8–1.3 after 10 ms, I_cmd tail within −2…+4 mA of pre-event (R§6.5); ζ not extractable from a charge-limited event | F2 |

## B. Setpoint ramps and mode changes

| ID | Requirement and acceptance | Basis | Status | Evidence | Next test |
|---|---|---|---|---|---|
| B1 | Up-ramp: no OVP or backstop trip; overshoot ≤ +5 % at the end of the ramp; slew ≤ 30 mV/µs (vSrcSlewPos) | [Plan] Task 1 items 1–2; [PD] | Closed at 20, 28 V; Open ≥ 36 V | 80 V/s (0.08 mV/µs): 5→20 V +0.34 %, 5→28 V +0.22 % (`bench/results/2026-10-11/README.md`). Scope 88 V/s on 2026-10-17 vs 80 V/s in firmware terms, unresolved | After D1 |
| B2 | ±5 % of the target within 650 ms of the request | [Plan] Task 1 item 3 | Closed at 28 V | 9.7 ms after the ramp's 90 % point; request to PS_RDY 309 ms | — |
| B3 | Handover steps (buck → buck-boost → boost) stay inside ±5 % | [Plan] Task 1 item 2 | Partial | Buck → buck-boost: −0.44 V (1.6 %) within 2 ms, back on the ramp in ≈25 ms. Buck-boost → boost: V_out falls at the end of the ramp, not quantified (2026-10-11) | Per-mode correction of the charge model |
| B4 | Downward transition: no regeneration into the bench supply, ±5 % of the new target inside tPSTransition, no Hard Reset | [Plan] epr-bringup, "Downward transitions" and item 4 | Closed unloaded; Open loaded and ≥ 36 V | −78.9 V/s 20→5 V, ±5 % in 17.5 ms; −81.2 V/s 28→5 V, 26.7 ms (unloaded). V_in not logged during the ramps; guard is VS_MON < V_in + 4 V | Down-ramp with the 25 Ω on, VS_MON logged |
| B5 | Boost start without ringing (runs 32/33 rang to 36–38 V at a 28 V target) | [Plan] epr-bringup item 3 | Open | 28 V is buck-boost at V_in 24 V since 2026-10-15. Boost applies at 36 and 48 V, started once (2026-10-15 36 V burst: oscillation, D1) | D1 |

## C. Loop margin and delay

| ID | Requirement and acceptance | Basis | Status | Evidence | Next test |
|---|---|---|---|---|---|
| C1 | Phase margin ≥ 45° at 28 V | [Plan] roadmap Phase 1 exit | Calc | Model 35° (`plans/scripts/loop_model.py`); not measured | F2 |
| C2 | Loop gain below 1 at the −180° crossing at each operating voltage | [Plan] roadmap section 2 | Calc; fails at 36, 48 V | 0.47 / 0.76 / 1.01 / 1.26 at 20 / 28 / 36 / 48 V; 36 V margin 13° (0.89) | Roadmap tests 1–3 at 36 V |
| C3 | Loop delay τ ≤ 15 µs (phase crossover ≈17 kHz) | [Plan] roadmap section 5.1 | Fail | ≈63–64 µs (2026-10-11 fit; model); crossover 3.97 kHz | Phase 1: HRTIM-triggered ADC per period |
| C4 | Current loop 20–40 kHz, above the L–C resonance (≈15 kHz at 48 V, 18 kHz at 36 and 20 V) | [Plan] roadmap section 5.2 | Calc; fails today | I_cmd updates once per 20 kHz PID cycle | Phase 1 |
| C5 | Gains from the capacitance the loop sees, following V_out (≈44 µF at 5 V to 6 µF at 48 V on board) | [Plan] roadmap section 5.4 | Open | Gains assume 300 µF (`LOOP_C_INT_UF` 50 + `LOOP_C_EXT_UF` 250); 2026-10-11 fit gave C/G ≈ 25 µF (crossover ≈1.2 kHz, not 100 Hz). kp_closed from reconnect I_cmd peaks 0.19 mA/mV at 5 and 20 V (= configured), 0.16 at 28 V SR on, 0.09 at 28 V SR off (R§6.5). kp switches on the firmware flag, which leads the physical switch by T_release (1.35–4.2 ms) | Retune (kp kept, ki ×8 suggested 2026-10-11); repeat R§6.3 at T_cmd 5.0 / 5.5 / 6.0 ms |
| C6 | Limits outside the fast path; anti-windup | [Plan] roadmap section 5.5 | Partial | No windup after 41 retained event traces (R§6.5); ceiling-riding at 36 V not tested | Roadmap test 1 (trace i_cmd against i_max in a 36 V burst) |
| C7 | Input-filter (Middlebrook) margin; V_in ripple per step | [Plan] roadmap section 5.6 | Calc | Margin ≈7× at 240 W from 24 V (supply and leads ≈30 mΩ); bench supply output impedance unmeasured; V_in not logged per pulse | F4, F6 |
| C8 | No DPWM limit cycle | [Plan] roadmap section 5.7 | Calc | HRTIM step 184 ps = 3.5 mV at 48 V, under VD_MON's 17 mV LSB; integral action | — |
| C9 | Boost RHP zero well above the voltage-loop crossover | [Plan] roadmap section 5.3 | Calc | 81 / 108 / 140 kHz at 48 / 36 / 28 V against ≈1.2 kHz | — |

## D. Sustained oscillation

| ID | Requirement and acceptance | Basis | Status | Evidence | Next test |
|---|---|---|---|---|---|
| D1 | 36 V into 25 Ω for 2 s with no 3.9 kHz line, rectifier on and off | [Plan] roadmap Phase 1 exit | Fail (SR on); Open (SR off) | 2026-10-15 burst: 5.3 V p-p, 3.92 kHz, 2.09 s, ended by the 2 s cap (Hard Reset) | Roadmap tests 1–3: trace i_cmd/i_max; `regulator_c_ext_uf` 250→0; C from an up-ramp, SR off |
| D2 | 28 V hold ≥ 2 s, no sustained oscillation, rectifier on and off | [Prop] | Partial | No 3.9 kHz line on 2026-10-16 (SR on, 500 µs/div). 2026-10-17 ADC traces, pooled (R§6.2): spectra σ 32.5 mV SR off, 69.1 mV SR on; excursions below V_0 − 0.25 V: 4 in 643 ms SR off (deepest −0.42 V), 47 in 638 ms SR on (deepest −0.50 V); no spectral line above the local median at 24.6 or 321 Hz. Earlier scope observations: ≈1 V p-p at ≈5 Hz (2026-10-16 step 9); ±0.5 V at a few Hz (2026-10-15). Each trace is 102 ms, so the 2 s hold is not covered. Causes open | Hold 28 V for 10 s, SR off and on, `trace-arm --trigger now` plus scope at 500 ms/div |
| D3 | 48 V hold | [Prop] | Open; Calc predicts unstable | Loop gain 1.26 at −180° | After D1 |
| D4 | Pulse skipping at light load: bounded transition rate, no V_out excursion above the skip band | [Prop] | Open | ≈3.5 transitions/s at 3 W (2026-10-11); 75/s in one `regulator_skip_events` reading (150 counts in 2 s, 28 V SR on, 2026-10-17 11:11:52) against 0 SKIPPING-flag transitions in the trace windows (R§10.1 U11); cause open | Log VD_MON per period across a transition |
| D5 | No sub-harmonic of the switching frequency (line at fsw/2 = 100 kHz, HRTIM period 200 kHz) in steady state, SR on and off | [Prop] | Fail at 5 V (one record; probe artefact not excluded); Open at 20 and 28 V | 5 V, SR on, 3.7 ms clean window of the reset capture (README step 6): Hann lines 99,730 Hz 275 mV pk, 299,730 Hz 283 mV, 499,730 Hz 269 mV; 200 kHz line 22 mV (R§7 D5) | One more 5 V record with a ground-spring tip, then 20 and 28 V, SR on and off: 24 ms at 80 ns, spectrum, HRTIM compare value logged per period |

## E. Protection and faults

| ID | Requirement and acceptance | Basis | Status | Evidence | Next test |
|---|---|---|---|---|---|
| E1 | No OVP, backstop or UVP trip, and no supervision Hard Reset (V_out under setpoint − 8 % for 200 ms), in ramps, holds and soft releases | [Plan] Task 1 item 1 | Closed at 5, 20, 28 V | 43 pulses, none faulted; longest t_rec(2 %) 10.65 ms | — |
| E2 | Independent cross-check of the OVP sense above 28 V before 36/48 V runs | 2026-10-11 README; roadmap | Partial | D3 (leaking clamp) removed and VD_MON recalibrated (a385dbf): within ≈2 % to 48 V, against 15 % low at 36 V before | Scope or DMM on VBUS during the first 36 V run |
| E3 | CC and VBUS short protection (TCPP-class) before EPR voltages | [Plan] epr-bringup item 6 | Open | UCPD pins are 5 V-tolerant I/O | Hardware |
| E4 | V_in excursions stay under the 60 V parts (Q7, Q8, ADM1270); no regeneration into a supply that cannot sink | [Plan] roadmap section 5 | Partial | I_cmd ≥ 0 in CCM; reverse pulses only while VS_MON < V_in + 4 V; no V_in record at a drop or down-ramp | F4 |

## F. Measurement prerequisites

| ID | Needed for | Status | Notes |
|---|---|---|---|
| F1 | Load-side voltage, Q5/Q6 gate and load current during a reconnect; decides H1/H2/H3 of R§10 and sets what ΔV_us means | Open | Probe next to TP2, triggered on OUTPUT_EN; current probe in the load path (R§10 tests T1–T3) |
| F2 | Calibrated ΔI(t) at the converter node: firmware step or sine added to the PI output, logged at 20 kHz, averaged over repeats. Gives Z_out, phase margin (C1) and ζ (A4) | Open | The QT Py no longer converts (2026-10-17), so the reconnect is the only transient source; ΔI(t) there is neither controlled nor measured |
| F3 | Measurement at the receptacle, where [PD] asserts | Open | TP2 is before R6, VD and Q5/Q6 |
| F4 | V_in per event | Open | The trace has no V_in column |
| F5 | Build identity per run (git HEAD, dirty flag, ELF SHA-256) | Open | Not recorded for the 2026-10-17/18 runs (R§3) |
| F6 | Bench supply output impedance and capacitance | Open | Enters C7 and E4 |

## Order and dependencies

1. F5 (trivial) and one more 5 V record for D5 (ground-spring tip; decides whether the 100 kHz line is real), then A3 (the unexplained fault is the open failure closest to this test).
2. F1 (probe the load side), then re-read R§6.3: it fixes what ΔV_us measures.
3. F2 (injection step), which gives A4, C1 and a stimulus comparable with the model's 7.9 V step.
4. C5 retune (ki ×8, gains matched to the measured C), then repeat R§6.3 at 20 and 28 V.
5. Phase 1 (C3, C4), then D1 with the tests in roadmap section 2, then D3.

Re-test after any change to kp, ki, fc, the kp scheduling, the loop delay or the output
capacitance: R§6.3 at T_cmd 5.0 / 5.5 / 6.0 ms, 20 and 28 V, n ≥ 5 each, randomised order.
