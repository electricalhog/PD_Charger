# EPR at 28 V: CC1, 80 V/s slews, boost stability under load (2026-10-11/12)

This is Task 1 of the EPR plan (`plans/epr-bringup.md`). It covers:
- **CC1 parity.** The cable was flipped so the CC1 side is checked against the CC2 benchmark.
- **Slews.** VBUS slews at 80 V/s in both directions.
- **28 V EPR.** The 28 V contract negotiates from 5 V in one request.
- **28 V under load.** The 28 V boost runs with the QT Py load at 0.5–3 W.

The firmware changed a lot over the two days. The VBUS dips at 28 V under load, the main finding, came from the V_in measurement. Every number below has a script in `scripts/` and lands in `metrics/`.

## Findings

**Both cable orientations work.**
- **Flipped (2026-10-11).** PD ran on CC2 and VCONN went out on CC1. That is the CC1 counterpart of the 2026-10-07 CC2 benchmark.
  - The CC1 VCONN switch measured 5.09 V, with a 0.36 ms rise (`cap/cc1_attach2`).
  - EPR timings from the PD trace (`data/pdtrace/20261011-190048_pdtrace.log`):
    - EPR_Mode Enter → Enter_Succeeded: 13 ms.
    - EPR_Request at the same voltage → PS_RDY: 39 ms.
  - EPR_KeepAlive arrives every 377–379 ms and every one is answered (`20261011-192316_pdtrace.log`).
- **Swapped ends (2026-10-12).** The cable's e-marker sits in one plug only, and the source needs to see its Ra. With that plug at the source, PD runs on CC1 and VCONN on CC2 (`bu pd status`: `cc2_pb5` = 1). All the 28 V results below are in this orientation.
- **Bench faults found on the way:**
  - The VCONN jumper was unplugged.
  - The cable was fitted with the e-marker plug at the sink, so the source saw no Ra and no contract came up.

**80 V/s slews** (`metrics/ramps.jsonl`, scope at 5 V/div DC, so ±0.2 V resolution and ±3 % gain):

| Capture | Transition | Slope | Overshoot past the final value | Inside ±5 % | Inside ±1 % |
|---|---|---|---|---|---|
| `v2_up20_p0b` | 5 → 20 V | 79.0 V/s | 69 mV (0.34 %) | 6.9 ms | (loop at fc 300 Hz then, limit cycle) |
| `v2_down5_p0` | 20 → 5 V | −78.9 V/s | 21 mV (0.39 %) | 17.5 ms | 18.3 ms |
| `v3_up28_p0` | 5 → 28 V, before the V_in fix | 81.4 V/s | 531 mV (1.84 %) | 9.7 ms | 471 ms |
| `v4_up28_p0` | 5 → 28 V | 80.7 V/s | 65 mV (0.22 %) | 9.7 ms | 24.1 ms |
| `v4_down5_p0` | 28 → 5 V | −81.2 V/s | 81 mV below 5 V | 26.7 ms | below resolution |

The ±5 % and ±1 % times count from the 90 % point of the ramp.

**28 V from 5 V in one request.**
- **Ramp and mode changes.** The ramp sits one layer above the switching mode. Buck → buck-boost happened at 22.94 V and buck-boost → boost at 27.94 V (board ADC).
- **The handovers still step.** They keep I_cmd, but at buck → buck-boost VBUS drops 0.44 V within 2 ms (0.47 V before the V_in fix, `v3_up28_p0`). It is back on the ramp in about 25 ms. So the buck and buck-boost charge models disagree by about that much. Buck-boost → boost falls at the end of the ramp.
- **Request to PS_RDY: 309 ms.** EPR_Request at 49699 ms, Accept at 49701 ms, PS_RDY at 50010 ms (`20261012-152543_pdtrace.log`). The source reports a 274–276 ms transition.
- **Within budget.** The sink's EPR tPSTransition is at least 830 ms.

**VBUS dips at 28 V under load: cause and fix.**
- **Before.** At 3 W, VBUS dipped 78 times a second by more than 100 mV, and the deepest dip was 423 mV.
- **Where a dip starts.** The switch nodes at a dip (`figures/dip28_3w_b_diagnosis.png`, `metrics/dip28_3w_b_periods.json`) show one PID cycle of short pulses, ten switching periods:
  - Q4's pulse fell from 500 to 320 ns.
  - Q3's body-diode conduction fell from 1088 to about 512 ns.
- **Energy lost.** Delivered energy goes as the square of the conduction time, so each of those pulses carried about 22 % of the normal energy. VBUS fell 274 mV within that 50 µs. With about 125 mA of load current, that puts about 18 µF on the regulator node at this timescale.
- **Cause: a bad V_in sample.** Steps 5–6 of the modulator used one ADC2 sample of V_in per PID cycle:
  - A single 2.5-cycle (59 ns) conversion, taken wherever in the switching period the ISR happened to start.
  - VS_MON is a 100 k / 5.82 k divider with no capacitor. It reaches the MCU over the header and jumper wires next to PHASE_2_P/N.
  - In boost, t_on ∝ √(V_out − V_in + Vd) / V_in. So a +2.7 V V_in error halves t_on, and a −4 V error stretches it by about 1.7×.
- **The fix** (`adc_monitor.c`, `regulator.c`):
  - ADC2 now averages 16 conversions of 24.5 + 12.5 cycles, 13.9 µs in all. They fall at 16 evenly spread points of the switching period over 2.8 periods.
  - The PID ISR reads the previous cycle's result and starts the next.
  - The modulator, the V_in checks and the mode filter use the median of the last three results.

V_in as the loop saw it, at 28 V EPR (`metrics/vin_ring.json`):

| Load | V_in σ (before → after) | V_in range | Cycles more than 1 V off | V_out σ | V_out minimum |
|---|---|---|---|---|---|
| none | 108 → 44 mV | 20.06–27.18 V → 23.64–24.38 V | 38 of 29 184 → 0 | 22 → 21 mV | 27.85 → 27.83 V |
| 3 W | 183 → 38 mV | 19.11–27.62 V → 23.64–24.37 V | 117 of 52 224 → 0 | 92 → 26 mV | 26.99 → 27.80 V |

VBUS at TP2 over the 3 W hold, 1.5–4.0 s (`metrics/holds_28v.json`, `figures/u28_3w_before_after.png`):

| | Dips > 100 mV | Dips > 200 mV | Deepest | Fast rms | Fast p-p |
|---|---|---|---|---|---|
| Before | 78 /s | 15.6 /s | 423 mV | 60.3 mV | 1114 mV |
| After | 0 /s | 0 /s | – | 14.9 mV | 180 mV |

The V_in median also moved, from 24.43 V (the single sample's fixed phase) to 24.06 V (the period average). The VS_MON gain was calibrated against a meter with the old sampling, so it needs a fresh meter reading.

**Loop behavior at 28 V** (`figures/u28_releases.png`).
- **No ringing.** The QT Py load ramps its output at 5 mV/ms, so applying it takes about a second; the release (disarm) is the only step. VBUS returns without ringing at every load.
- **Fit at 0.5 W.** The 0.5 W release stays under the pulse-skip band, so it can be fitted. It is overdamped: two real poles at 163 /s and 7476 /s, ζ ≈ 3.5, residual 10 mV rms.
- **The loop as it really is.** For the PI loop (kp = 0.1885 mA/mV, ki = 29.6 mA/mV/s), those poles mean the plant looks like C/G ≈ 25 µF, not the 300 µF the gains assume. So the loop crosses over near 1.2 kHz, not 100 Hz.
- **Why the tail is slow.** The slow pole is the PI zero at 25 Hz: a 6 ms tail.
- **The skip plateau.** From 1 W up, the release hits the skip band (setpoint + 420 mV). It sits there while the integrator unwinds, for 2.8 ms at 1 W and 8.3 ms at 3 W.

| Load released | Peak | Peak / 28 V | Time at ≥ 90 % of peak | Inside ±1 % | Inside ±0.5 % |
|---|---|---|---|---|---|
| 0.5 W | +418 mV | +1.49 % | 1.4 ms | 3.2 ms | 7.5 ms |
| 1 W | +520 mV | +1.86 % | 2.8 ms | 6.4 ms | 10.1 ms |
| 2 W | +609 mV | +2.17 % | 5.1 ms | 15.8 ms | 19.6 ms |
| 3 W | +666 mV | +2.38 % | 8.3 ms | 21.8 ms | 25.6 ms |

**Noise at 28 V** (`metrics/noise28_tables.md`, `figures/v28_spectra.png`). These are the 2026-10-10 records and metrics, AC-coupled.
- **Low-frequency noise** is lower than at 20 V on 2026-10-10 (old loop) at the same loads. The 8 Hz–20 kHz rms is 10.7 / 14.7 / 21.7 / 21.2 mV at 0 / 0.5 / 2 / 3 W, against 25.8 / 40.1 / 44.9 / 57.4 mV then. The hump stands +2 to +9 dB over the floor, against +11 to +24 dB.
- **Switching ripple and spikes** are higher than at 20 V:
  - The 200 kHz ripple is 95–241 mV p-p.
  - The 99.9 % range in the 20 MHz bandwidth is 761–821 mV.
  - The 20–62.5 MHz band is about 50 mV rms.
- **Caveat on spikes.** On 2026-10-10 half of the spikes were probe pickup. Treat these p-p figures as upper bounds until a tip-and-barrel measurement.

| Load | Vpp 20 MHz | Vrms 20 MHz | 99.9 % range 20 MHz | 200 kHz ripple p-p | Vrms full BW | 8 Hz–20 kHz rms | 8 Hz–20 kHz p-p |
|---|---|---|---|---|---|---|---|
| – | 1160 | 39.9 | 766 | 95 | 65.5 | 10.7 | 137 |
| 0.5 W | 1165 | 54.0 | 771 | 147 | 76.4 | 14.7 | 158 |
| 2 W | 1239 | 74.9 | 821 | 208 | 91.6 | 21.7 | 191 |
| 3 W | 1204 | 84.5 | 761 | 241 | 98.9 | 21.2 | 188 |

All values are in mV. The board ADC read 28.019–28.028 V (+0.07 to +0.10 %) at every load. The QT Py read 28.26 V at its input.

## Task 1 scorecard (28 V, 24 V in)

| Criterion | Result | |
|---|---|---|
| No OVP or backstop trip | none in any ramp, hold or release run; no fault in the 25 min since the flash, FLT1/FLT2 clear | pass |
| Overshoot ≤ +5 % | end of ramp +0.22 %; load release +2.38 % at 3 W; the buck → buck-boost handover dips 0.44 V rather than overshooting | pass |
| Inside ±5 % within 650 ms of the request | 9.7 ms after the ramp's 90 % point, PS_RDY at 309 ms | pass |
| ζ ≥ 0.4 on a 0 → 3 W step | the load ramps on in about 1 s; the release steps show no ringing; ζ ≈ 3.5 at 0.5 W | pass, by the release |
| Ripple and noise in the 2026-10-10 tables | above | done |

The ramps ran unloaded. The load cannot start at 5 V, because its gate drive needs 12–15 V at its input.

## Open items

- **PS_RDY validity.** Before the PH2_N wiring fix, the source sent PS_RDY for 24 V while VBUS had stalled at 21.9 V. PS_RDY should wait for VBUS to be in range.
- **Meter readings at 28 V.** Two calibrations need a meter:
  - VBUS at TP2. The ADC reads 28.02 V, the QT Py 28.26 V, and the scope 28.85 V at 5 V/div.
  - V_in (VS). The VS_MON gain predates the oversampling.
- **Loop retune.**
  - Set the gains for the plant as measured: C/G ≈ 25 µF, about 64 µs of loop delay. Keep kp, and raise ki about 8× (the PI zero from 25 to about 200 Hz). That would shorten the 6 ms tail and the skip plateaus.
  - A firmware loop-gain measurement would replace the fit.
- **Handover dip.** The handover drops 0.44 V at buck → buck-boost on the way up. Per-mode corrections to the charge model would remove it.
- **DCM charge model.** At no load the loop commands 177 mA, for a few mA of real load. The model ignores the GaN reverse-conduction drop (about 2 V, not 0.7 V), the 50 ns phase-2 dead time, and the negative inductor current the Q2 refresh leaves at the start of each pulse.
- **Pulse skipping at 3 W.** About 3.5 skip transitions a second remain. The PID's V_out never passed 28.11 V, so they come from short excursions in the per-period VD_MON samples the watchdog sees. The cause is not known yet.

## Setup

- **Source.** The NUCLEO-G474 with the PD_Charger power board, fed by the 24 V bench supply (24.56 V by meter on 2026-10-10).
  - The PD build has real VBUS: `PD_VCONN`, `PD_BENCH_FORCE_VCONN`, `PD_BENCH_15V`, `PD_BENCH_20V`, `PD_BENCH_24V` and `PD_BENCH_EPR_28V` all ON.
  - PDOs: 5 / 9 / 15 / 20 / 24 V and EPR 28 V, all at 500 mA.
- **Sink.** The NUCLEO-G431 (`bench/pd-sink-g431`), now clocked at 170 MHz, over the Apple 240 W cable, e-marker plug at the source. PD runs on the source's CC1, VCONN on CC2.
- **Load.** The QT Py buck into 11 Ω / 10 W. Its telemetry assumes 10 Ω, so the power into 11 Ω is 10/11 of the figure shown.
- **Scope.** DS1104Z with 10× probes:
  - CH2 on SW1 (Q1/Q2) and CH3 on SW2 (Q3/Q4).
  - CH4 on TP2 (VBUS). Above 20 V, CH4 is AC-coupled, because the scope cannot offset a fine scale past 20 V.

## Files

- **`scripts/`:**
  - `trans3.py`: the 6 s load-hold record.
  - `op28.py`: the noise record set; it calls `../2026-10-10/scripts/grab.py`.
  - `vin_ring.py`: the PID loop's debug ring over SWD.
  - `ramp.py`, `transient.py`, `dip_periods.py`, `holds_28v.py`, `noise28.py`: the analysis.
- **`metrics/`:**
  - `ramps.jsonl`, `vin_ring.json`, `dip28_3w_b_periods.json`.
  - `holds_28v.json`, `noise28.json`, `noise28_tables.md`.
- **`figures/`:**
  - `dip28_3w_b_diagnosis.png`, `u28_3w_before_after.png`.
  - `u28_releases.png`, `v28_spectra.png`, `u28_p3000_ac.png`.
- **`data/`:** kept out of git and stored only on the bench machine.
  - `cap/`: raw captures and load telemetry.
  - `ring/`: debug-ring reads.
  - `pdtrace/`: the three PD traces cited above.
