# VBUS regulation, noise and load steps at the SPR PDOs (2026-10-10)

VBUS was measured at the 5, 9, 15 and 20 V contracts:
- **15 and 20 V:** with the QT Py buck load at 0, 0.5, 2 and 3 W, plus a 0 → 3 W load-step run at each voltage.
- **5 and 9 V:** unloaded. The load's gate drive needs 12–15 V at its input, so it cannot run there.

`scripts/analyze.py` computes every number below into `metrics/metrics.json`. `scripts/vbus_noise.m` recomputes the noise metrics in MATLAB, and the two agree; see the cross-check section.

## Findings

**DC level.**
- At 9, 15 and 20 V the scope reads +0.11 to +0.38 % above the contract, and the board ADC reads −0.18 to +0.10 %. The scope sits 12–66 mV above the ADC at each of these points.
- At 5 V the gap is 134 mV: the scope reads 5.132 V (+2.6 %) and the ADC 4.998 V. That is out of line with the other points.
- The ADC has only a gain correction, made at 15 V, so an offset in its input path would show most at 5 V. A meter reading at 5 V would settle it.

**Load regulation, 0 → 3 W.**
- VBUS rises 15.2 mV at 15 V and 20.2 mV at 20 V, −0.10 % at both.
- The 6 s load-step records agree within 1.3 mV.

**Stability.** The source's voltage loop is stable but lightly damped.
- **Ringing.** After the step at load start-up, VBUS rings at 263 Hz (15 V) and 429 Hz (20 V). It loses 3.6× and 4.0× per cycle, which gives ζ ≈ 0.2.
- **Noise hump.** The noise spectrum has a broad hump in the same place at every contract, 233–524 Hz, even with no load. It grows with load:
  - 15 V: +10.0 dB over the 20–60 Hz floor unloaded, +18.1 dB at 3 W.
  - 20 V: +10.9 dB unloaded, +24.4 dB at 3 W.
- **Nothing grows.** No record shows a rising oscillation, and the load reported no faults. VBUS ended each load-step run within 1 mV of where it started.
- **Phase margin.** If the loop behaves as second order, ζ ≈ 0.2 means roughly 20° of phase margin. A loop-gain measurement would give it directly.

**Load steps.**

| Contract | Dip at load start-up | Back in band | Overshoot at release | Back in band |
|---|---|---|---|---|
| 15 V | −343 mV | 6.8 ms | +339 mV (+2.3 %) | 8.3 ms |
| 20 V | −214 mV | 0.2 ms | +453 mV (+2.3 %) | 3.6 ms |

The start-up step comes from the load. Between two telemetry frames, its output jumps from about 0.1 V to 0.8–1.3 V instead of following its 5 V/s ramp.

**Switching ripple.**
- The 200 kHz ripple (synchronous average, harmonics to 2 MHz) is 13 mV p-p at 5 V. It grows with voltage and load, to 155 mV p-p at 15 V, 3 W and 210 mV p-p at 20 V, 3 W.
- Its fundamental runs from 2.5 to 69.8 mV rms.
- The buck load's 438.6 kHz shows at only 1.5–1.9 mV rms, at 2 and 3 W.

**Spikes.**
- Narrow spikes set the peak-to-peak figures: 687–916 mV p-p in the 20 MHz bandwidth, against a 0.05–99.95 % range of 284–498 mV.
- In a two-probe capture at 5 V, 369 of the 676 VBUS spikes (55 %) had a VCONN spike within ±60 ns. So a good part of them is probe pickup.
- Treat the p-p figures as upper bounds until a tip-and-barrel measurement.

**Low-frequency lines.**
- With the source alone, lines appear at 2634 Hz (5.7 mV rms, 15 V) and 2074 Hz (9.4 mV rms, 20 V).
- With the load running, lines appear at 1390–1392 Hz (up to 19.0 mV) and 402–406 Hz (up to 23.8 mV rms, at 20 V, 3 W). The 402–406 Hz lines sit inside the hump.
- Below 8 Hz, VBUS moves 5–13 mV p-p.

## Setup

- **Source:** the NUCLEO-G474 with the PD_Charger power board, fed from 24.56 V (meter).
  - Firmware: the PD build with real VBUS, `-DPD_VCONN=ON -DPD_BENCH_FORCE_VCONN=ON -DPD_BENCH_15V=ON -DPD_BENCH_20V=ON`.
  - PDOs: 5, 9, 15 and 20 V at 500 mA.
  - V_out sense recalibrated against the meter earlier the same day: 13.32 V at TP2, 24.56 V supply, at the 15 V contract. See `STM32CubeIDE/final/Core/Inc/regulator_config.h`.
- **Sink:** the NUCLEO-G431 (`bench/pd-sink-g431`) on SPR contracts, over the Apple 240 W cable.
- **Load:** the QT Py buck, on the power-stage build, into an 11 Ω / 10 W ballast, driven by `bu load run`.
  - Its telemetry computes power for 10 Ω, so the power into the real 11 Ω is 10/11 of that.
- **Scope:** a Rigol DS1104Z (8-bit, 100 MHz).
  - CH4 is on TP2 (VBUS at the power board), through a 10× passive probe with its ground lead.
  - The other channels were off, except in the two-probe capture.

## Method

### Records

Every capture is a single shot in high-resolution (HRES) acquisition: DC coupled, with the offset set to the expected VBUS.

| Record | Length | Sample rate | Scale | Bandwidth | Used for |
|---|---|---|---|---|---|
| 9.6 ms, 20 MHz | 1.2 Mpts | 125 MSa/s | 0.1 V/div | 20 MHz limit | Vpp, Vrms, 200 kHz ripple, spectrum 10 kHz–20 MHz, lines above 10 kHz |
| 9.6 ms, full BW | 1.2 Mpts | 125 MSa/s | 0.1 V/div | 100 MHz | Vrms full BW, 20–62.5 MHz band |
| 600 ms | 1.2 Mpts | 2 MSa/s | 0.05 V/div | 20 MHz limit | DC level, 8 Hz–20 kHz ripple, spectrum 10 Hz–10 kHz, lines, hump, < 8 Hz swing |
| 6 s load step | 12 Mpts | 2 MSa/s | 0.1 V/div | 20 MHz limit | DC shift, start-up dip, ringing, release |
| two-probe (5 V) | 600 kpts, 12 ms | 50 MSa/s | 0.2 V/div | 20 MHz limit | VBUS and VCONN spike coincidence |

**Clipping:**
- 20 MHz records: no clipped samples.
- 600 ms records: 0.007–0.059 % of samples clipped.
- Full-BW records: 0.021–0.051 % clipped. Their spikes fill the whole 8-bit range (1.264 V at 0.1 V/div), so full-BW Vpp is only known to be ≥ 1.26 V and is not reported.
- 20 V load-step record: 2 samples clipped.

### Processing

**Scaling:** volts = (code − yorigin − yreference) × yincrement, from each capture's own preamble.

**Time domain:**
- Vpp, and Vrms as the population standard deviation.
- 99.9 % range: the 0.05–99.95 percentile range. All three are taken with the mean removed.

**200 kHz ripple p-p:**
- The 20 MHz record goes through a 2 MHz low-pass (4th-order Butterworth, zero phase).
- It is then averaged cycle by cycle over each quarter of the record. 125 MSa/s is exactly 625 samples per cycle, and a quarter is 480 cycles, short enough that the ~25 ppm clock offset doesn't smear it.
- The result is the p-p of that average, as the mean over the four quarters.

**8 Hz–20 kHz ripple:**
- The 600 ms record goes through a 4th-order Butterworth high-pass at 8 Hz, then a 20 kHz low-pass, both zero phase.
- 50 ms is dropped at each end.

**< 8 Hz swing:** p-p of the 20 ms moving mean of the 600 ms record.

**PSD:** Welch, with a periodic Hann window and 50 % overlap, and no detrend per segment.
- The high-passed 600 ms record uses 2^18 points (ENBW 11.4 Hz).
- The 9.6 ms records use 2^17 points (ENBW 1.43 kHz).
- The two are stitched at 10 kHz.

**Band rms:** √(∫ PSD df), trapezoidal over [lo, hi).

**Spike coincidence** (`spikes.py`, two-probe capture):
- Both channels are high-passed at 2 MHz, and a spike is |x| > 5σ of its channel.
- VBUS samples within 10 samples (200 ns) of an event's start count as that event.
- An event is coincident when VCONN has a spike within ±3 samples (±60 ns).

**Lines:**
- A periodic flat-top window over the whole record, in V rms per line (dBµV re 1 µV rms).
- Line spacing: 2 Hz for the high-passed 600 ms record (500 ms used), 104 Hz for the 9.6 ms record.
- Each band keeps the 8 largest local maxima that are ≥ 12 dB over the band's median.

**Noise hump:**
- The ASD of the 600 ms record, smoothed by a sliding 1/3-octave median, which is robust to discrete lines.
- Its peak between 100 Hz and 8 kHz, and the peak's height over the 20–60 Hz median.
- The frequency is good to about ±1/6 octave.

**Load steps:** the record goes through 20 kHz and 1 kHz zero-phase low-passes.
- **Steady band:** the 0.1–99.9 percentile range of the 20 kHz low-pass, over the no-load stretch before the run (and after it, for the release).
- **Dip:** the lowest 20 kHz-low-passed point within 0.4 s after the run starts, relative to the no-load mean.
- **Release:** the steepest rise of the 1 kHz low-pass within 150 ms of the load's last running telemetry frame.
- **Overshoot:** the highest point from 2 ms before to 50 ms after the release, relative to the no-load mean afterwards.
- **Back in band:** the first moment the 20 kHz low-pass is inside the steady band and stays there for 10 ms. For the dip, the band's upper edge is raised 50 mV, because VBUS sits 15–20 mV higher under load.
- **Ringing:**
  - Take the extrema (prominence ≥ 15 mV) of a 2 kHz low-pass over the 12 ms after the dip, merged so they alternate in sign about the no-load level.
  - The frequency is 1 / (trough to next trough), and the decay is the ratio of those two troughs.
  - ζ = ln(decay) / √(4π² + ln²(decay)).

### Why the spectrum is stitched at 10 kHz

HRES decimation is not a complete anti-alias filter. Comparing the median ASD of the two records band by band, at all ten points:

| Band | 600 ms record ÷ 9.6 ms record | Why |
|---|---|---|
| 3–10 kHz | 0.39–1.29 | The 9.6 ms record reads high where the 1.4–3.3 kHz lines leak through its 1 ms segments. |
| 10–30 kHz | 0.93–1.16 at nine points, 1.9 at 9 V | The records agree. |
| 30–90 kHz | 2.6–9.9 | The 600 ms record is aliased. |

### A record that was discarded

Earlier 12 s load-step records at 100 kSa/s showed a "5 Hz oscillation". It was the 200 kHz ripple aliased: 200 kHz is twice that sample rate, and the G474's and the scope's clocks differ by about 25 ppm. Those records are not used.

### MATLAB cross-check

`vbus_noise.m` recomputes the operating-point metrics from the exported `.mat` files, in MATLAB R2025b with the Signal Processing Toolbox.
- DC, Vpp, Vrms, the < 8 Hz swing, the 200 kHz ripple p-p and every band except 10 Hz–1 kHz agree within 0.002 %.
- The 10 Hz–1 kHz band agrees within 0.042 %.
- The 8 Hz–20 kHz rms and p-p agree within 2.0 % and 2.3 %. MATLAB's `filtfilt` and SciPy's `sosfiltfilt` pad the record ends differently.
- Lines: 155 of 160 are identical, with amplitudes within 0.071 %.
  - The other 5 (1.9–8.7 mV rms) differ in peak picking, because `findpeaks` and `find_peaks` apply the minimum spacing differently.
  - None of those 5 appears in the tables below.
- The noise-hump and ringing metrics are Python-only.

### Limits on these numbers

- **Absolute DC.** The scope's absolute accuracy here is of order 1 %, from its offset accuracy and the probe's ratio tolerance. Changes between points at the same settings are good to a few mV. The board ADC has a gain correction from 15 V only. For compliance-grade DC, read a meter at each PDO.
- **Probe pickup.** The spikes and the 20–62.5 MHz band (22.5–29.4 mV rms, nearly independent of load) are likely dominated by pickup on the probe's ground lead. With VBUS off, TP2 read −1.47 V DC (the output-discharge state), so no noise floor could be taken that way. For standards work:
  - Re-measure with the probe tip and barrel on TP2, or a 50 Ω coax with a DC block.
  - Take a floor with the tip shorted to its own ground at the same spot.
- **Bandwidth.** HRES averages neighboring samples, so it also low-passes each record. This is negligible in the 20 MHz-limited records, but may attenuate the top of the 20–62.5 MHz band.
- **Not measured:**
  - Line regulation, since the supply is fixed.
  - Loaded behavior at 5 and 9 V.
  - The loop's gain and phase margin.

## Results

### DC level and regulation

| Contract | Load target | Into 11 Ω | VBUS, scope | VBUS, board ADC | Error, scope | Error, ADC | Buck VIN (QT Py) | Load duty | Source on-time |
|---|---|---|---|---|---|---|---|---|---|
| 5 V | – | – | 5.1322 V | 4.998 V | +2.64 % | -0.04 % | – | – | 225 ns |
| 9 V | – | – | 9.0343 V | 9.009 V | +0.38 % | +0.10 % | – | – | 292 ns |
| 15 V | – | – | 15.0167 V | 14.974 V | +0.11 % | -0.18 % | – | – | 433 ns |
| 15 V | 0.5 W | 0.45 W | 15.0174 V | 15.005 V | +0.12 % | +0.03 % | 14.603 V | 15.1 % | 705 ns |
| 15 V | 2 W | 1.82 W | 15.0317 V | 15.003 V | +0.21 % | +0.02 % | 14.616 V | 31.0 % | 921 ns |
| 15 V | 3 W | 2.73 W | 15.0319 V | 15.007 V | +0.21 % | +0.05 % | 14.616 V | 37.8 % | 1018 ns |
| 20 V | – | – | 20.0379 V | 19.984 V | +0.19 % | -0.08 % | – | – | 664 ns |
| 20 V | 0.5 W | 0.45 W | 20.0484 V | 19.982 V | +0.24 % | -0.09 % | 19.556 V | 11.6 % | 1023 ns |
| 20 V | 2 W | 1.82 W | 20.0513 V | 19.993 V | +0.26 % | -0.03 % | 19.562 V | 23.4 % | 1339 ns |
| 20 V | 3 W | 2.72 W | 20.0581 V | 20.012 V | +0.29 % | +0.06 % | 19.566 V | 28.5 % | 1504 ns |

Error is relative to the contract. The QT Py reads its input about 2.6 % low.

### Load regulation

Load regulation = (V_no load − V_3 W) / V_3 W.

| Contract | No load (scope) | 3 W (scope) | Shift, scope | Load regulation, scope | Shift, board ADC | Load regulation, ADC | Shift, 6 s run (scope) |
|---|---|---|---|---|---|---|---|
| 15 V | 15.0167 V | 15.0319 V | +15.2 mV | -0.10 % | +33 mV | -0.22 % | +16.5 mV |
| 20 V | 20.0379 V | 20.0581 V | +20.2 mV | -0.10 % | +28 mV | -0.14 % | +19.6 mV |

### Ripple and noise, time domain (mV)

| Contract | Load | Vpp 20 MHz | Vrms 20 MHz | 99.9 % range 20 MHz | 200 kHz ripple p-p | Vrms full BW | 8 Hz–20 kHz rms | 8 Hz–20 kHz p-p | < 8 Hz swing |
|---|---|---|---|---|---|---|---|---|---|
| 5 V | – | 707 | 15.8 | 299 | 13 | 27.6 | 11.8 | 116 | 7 |
| 9 V | – | 687 | 14.2 | 284 | 26 | 29.0 | 8.9 | 84 | 5 |
| 15 V | – | 732 | 22.8 | 338 | 43 | 30.6 | 12.3 | 112 | 6 |
| 15 V | 0.5 W | 732 | 38.2 | 353 | 89 | 48.1 | 18.1 | 238 | 7 |
| 15 V | 2 W | 851 | 54.4 | 413 | 133 | 59.5 | 23.1 | 351 | 7 |
| 15 V | 3 W | 866 | 60.7 | 413 | 155 | 70.5 | 29.5 | 414 | 7 |
| 20 V | – | 756 | 35.5 | 338 | 75 | 43.6 | 25.8 | 266 | 8 |
| 20 V | 0.5 W | 841 | 58.3 | 358 | 132 | 70.3 | 40.1 | 520 | 10 |
| 20 V | 2 W | 916 | 82.4 | 498 | 186 | 84.8 | 44.9 | 549 | 10 |
| 20 V | 3 W | 911 | 91.6 | 468 | 210 | 94.9 | 57.4 | 566 | 13 |

### Noise by band (mV rms, from the PSD)

| Contract | Load | 10 Hz–1 kHz | 1–10 kHz | 10–100 kHz | 100 kHz–1 MHz | 1–20 MHz | 20–62.5 MHz |
|---|---|---|---|---|---|---|---|
| 5 V | – | 9.93 | 5.86 | 2.19 | 3.67 | 2.80 | 22.56 |
| 9 V | – | 7.72 | 2.54 | 2.53 | 7.02 | 3.48 | 24.27 |
| 15 V | – | 8.62 | 7.97 | 2.35 | 14.91 | 4.13 | 22.53 |
| 15 V | 0.5 W | 13.44 | 11.25 | 3.37 | 32.20 | 4.26 | 25.06 |
| 15 V | 2 W | 17.87 | 13.69 | 3.35 | 47.50 | 4.42 | 27.63 |
| 15 V | 3 W | 22.39 | 17.07 | 3.97 | 55.44 | 4.33 | 28.72 |
| 20 V | – | 18.57 | 17.20 | 4.08 | 25.75 | 4.84 | 25.69 |
| 20 V | 0.5 W | 29.80 | 24.70 | 6.20 | 44.46 | 5.31 | 27.25 |
| 20 V | 2 W | 30.74 | 30.94 | 7.23 | 62.86 | 5.45 | 29.41 |
| 20 V | 3 W | 45.35 | 34.59 | 5.64 | 70.92 | 5.41 | 28.01 |

### Low-frequency noise hump

| Contract | Load | Floor 20–60 Hz, µV/√Hz | Hump peak | Hump level, µV/√Hz | Hump over floor |
|---|---|---|---|---|---|
| 5 V | – | 176 | 524 Hz | 427 | +7.7 dB |
| 9 V | – | 175 | 233 Hz | 549 | +9.9 dB |
| 15 V | – | 187 | 242 Hz | 594 | +10.0 dB |
| 15 V | 0.5 W | 200 | 420 Hz | 760 | +11.6 dB |
| 15 V | 2 W | 165 | 376 Hz | 1029 | +15.9 dB |
| 15 V | 3 W | 160 | 376 Hz | 1283 | +18.1 dB |
| 20 V | – | 278 | 376 Hz | 980 | +10.9 dB |
| 20 V | 0.5 W | 191 | 420 Hz | 1965 | +20.3 dB |
| 20 V | 2 W | 218 | 420 Hz | 1912 | +18.8 dB |
| 20 V | 3 W | 165 | 420 Hz | 2730 | +24.4 dB |

### Largest lines (mV rms)

| Contract | Load | Largest lines below 10 kHz, mV rms | Largest lines 10 kHz–20 MHz, mV rms |
|---|---|---|---|
| 5 V | – | 484 Hz: 2.3 (67 dBµV), 838 Hz: 2.1 (66 dBµV), 344 Hz: 1.9 (66 dBµV) | 200.0 kHz (source fsw): 2.5 (68 dBµV), 93.1 kHz: 1.3 (63 dBµV), 400.0 kHz (source fsw ×2): 0.9 (59 dBµV) |
| 9 V | – | 250 Hz: 3.0 (69 dBµV), 230 Hz: 1.4 (63 dBµV), 216 Hz: 1.4 (63 dBµV) | 200.0 kHz (source fsw): 6.3 (76 dBµV), 93.1 kHz: 2.3 (67 dBµV), 400.0 kHz (source fsw ×2): 2.0 (66 dBµV) |
| 15 V | – | 2634 Hz: 5.7 (75 dBµV), 256 Hz: 2.3 (67 dBµV), 2622 Hz: 2.2 (67 dBµV) | 200.0 kHz (source fsw): 14.0 (83 dBµV), 400.0 kHz (source fsw ×2): 3.7 (71 dBµV), 600.0 kHz (source fsw ×3): 1.9 (66 dBµV) |
| 15 V | 0.5 W | 3270 Hz: 6.0 (76 dBµV), 402 Hz: 4.3 (73 dBµV), 308 Hz: 4.2 (73 dBµV) | 200.0 kHz (source fsw): 30.8 (90 dBµV), 400.0 kHz (source fsw ×2): 7.5 (78 dBµV), 600.0 kHz (source fsw ×3): 3.3 (70 dBµV) |
| 15 V | 2 W | 1390 Hz: 7.0 (77 dBµV), 406 Hz: 6.6 (76 dBµV), 234 Hz: 4.6 (73 dBµV) | 200.0 kHz (source fsw): 46.1 (93 dBµV), 400.0 kHz (source fsw ×2): 10.5 (80 dBµV), 600.0 kHz (source fsw ×3): 3.9 (72 dBµV) |
| 15 V | 3 W | 1392 Hz: 10.0 (80 dBµV), 406 Hz: 10.0 (80 dBµV), 358 Hz: 6.0 (76 dBµV) | 200.0 kHz (source fsw): 53.5 (95 dBµV), 400.0 kHz (source fsw ×2): 11.7 (81 dBµV), 600.0 kHz (source fsw ×3): 4.0 (72 dBµV) |
| 20 V | – | 2074 Hz: 9.4 (80 dBµV), 464 Hz: 6.8 (77 dBµV), 514 Hz: 5.5 (75 dBµV) | 200.0 kHz (source fsw): 24.3 (88 dBµV), 400.0 kHz (source fsw ×2): 5.7 (75 dBµV), 600.0 kHz (source fsw ×3): 2.6 (68 dBµV) |
| 20 V | 0.5 W | 404 Hz: 12.2 (82 dBµV), 2408 Hz: 10.4 (80 dBµV), 2392 Hz: 9.5 (80 dBµV) | 200.0 kHz (source fsw): 43.0 (93 dBµV), 400.0 kHz (source fsw ×2): 8.9 (79 dBµV), 600.0 kHz (source fsw ×3): 3.2 (70 dBµV) |
| 20 V | 2 W | 1392 Hz: 14.0 (83 dBµV), 394 Hz: 9.3 (79 dBµV), 2638 Hz: 9.3 (79 dBµV) | 200.0 kHz (source fsw): 61.2 (96 dBµV), 400.0 kHz (source fsw ×2): 10.7 (81 dBµV), 10.7 kHz: 2.7 (69 dBµV) |
| 20 V | 3 W | 404 Hz: 23.8 (88 dBµV), 1392 Hz: 19.0 (86 dBµV), 464 Hz: 16.3 (84 dBµV) | 200.0 kHz (source fsw): 69.8 (97 dBµV), 400.0 kHz (source fsw ×2): 11.0 (81 dBµV), 10.2 kHz: 2.5 (68 dBµV) |

- 200 kHz and its harmonics are the power board's HRTIM switching frequency.
- The 93.1 kHz line at 5 and 9 V has no identified source yet.
- Full lists are in `metrics/tones_python.csv` and `metrics/vbus_tones_matlab.csv`.

### Load steps (0 → 3 W ramp, then instant release)

| Contract | Steady band, no load (20 kHz LP) | Dip at load start-up, 20 kHz LP (1 kHz LP) | Back in band | Overshoot at release, 20 kHz LP (1 kHz LP) | Overshoot / contract | Back in band |
|---|---|---|---|---|---|---|
| 15 V | -45 / +43 mV | -343 mV (-336 mV) | 6.8 ms | +339 mV (+225 mV) | +2.3 % | 8.3 ms |
| 20 V | -99 / +95 mV | -214 mV (-174 mV) | 0.2 ms | +453 mV (+307 mV) | +2.3 % | 3.6 ms |

The values in parentheses are the 1 kHz low-pass. Overshoot / contract uses the 20 kHz low-pass.

| Contract | Extrema after the start-up dip (2 kHz LP, re no-load level) | Ring frequency | Decay per cycle | ζ (log decrement) |
|---|---|---|---|---|
| 15 V | -330 mV @ -0.09 ms, +149 mV @ 1.90 ms, -92 mV @ 3.71 ms, +54 mV @ 5.31 ms | 263 Hz | 3.6× | 0.20 |
| 20 V | -177 mV @ -0.01 ms, +98 mV @ 1.13 ms, -44 mV @ 2.33 ms, +8 mV @ 3.49 ms | 429 Hz | 4.0× | 0.22 |

## Figures

- `figures/fig_spectra.png`: the ASD at TP2 per contract, with all loads overlaid, stitched at 10 kHz.
- `figures/vbus_spectra_matlab.png`: the same, from MATLAB's `pwelch`.
- `figures/fig_time.png`: 50 µs of the 20 MHz record at 15 V unloaded, 15 V at 3 W and 20 V at 3 W, plus 20 ms of the 600 ms record at 20 V, 3 W.
- `figures/fig_transients.png`: both load-step runs, with zooms on start-up and release.

## Files

- **`metrics/`:**
  - `metrics.json`, which holds every metric and the scope settings per record.
  - `tables.md`, `summary.csv` and `spikes.json`.
  - `tones_python.csv`, `vbus_noise_matlab.csv` and `vbus_tones_matlab.csv`.
- **`scripts/`:**
  - `grab.py`, `op.py` and `trans2.py`: capture scripts. They run in the bring-up venv (`uv run --project tools/bringup python …`).
  - `analyze.py`, `tables.py` and `spikes.py`: the analysis. They need numpy ≥ 2, scipy and matplotlib.
  - `export_mat.py`: the `.mat` export.
  - `vbus_noise.m`: the MATLAB cross-check.
- **`data/`:** kept out of git (about 95 MB) and stored only on the bench machine.
  - `cap/`: the raw 8-bit captures (`*_CHn.bin` plus `.json`), the per-point logs (`*_op.json`), and the load's telemetry (`*_load_run.json`). It also holds the two-probe capture (`survey_5v`) and the attempted floor (`floor_coarse`).
  - `mat/`: the same captures for MATLAB, plus `index.json`.

To redo the analysis:

```sh
cd bench/results/2026-10-10/scripts
python3 analyze.py && python3 tables.py && python3 spikes.py
python3 export_mat.py ../data/cap ../data/mat
matlab -batch "addpath(pwd); vbus_noise('../data/mat', '../metrics', '../figures')"
```

In MATLAB, one capture in volts:

```matlab
d = load('v20_p3000_hf.mat');
v = (double(d.code) - d.yorig - d.yref) * d.yinc;
```

`d.fs` is the sample rate, and the other fields carry the scope settings.
