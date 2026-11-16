# Regulation strategy: efficient regulation from 10–24 V in, and current regulation for a battery (2026-11-02)

**Scope.** How the converter regulates every output it offers, efficiently and stably, from 10 V up to the 24 V bench
supply, and what has to be true before a battery goes on VBUS.

**Builds on:**
- `ccm-buck-50w.md`: buck forced CCM, the current limit, IL_MON;
- `240w-roadmap.md`: hardware gates and power levels;
- `stability-requirements.md`.

**Sources.** Numbers marked *model* come from `plans/scripts/loss_budget.py` and `ccm_buck.py`; neither includes core
loss or housekeeping. Bench numbers cite the log they come from. Inferences are labeled.

## 1. Where it stands

**Measured on the bench:**

| Topic | What is established | Log |
|---|---|---|
| Buck forced CCM, 5–20 V | Holds at 200 and 400 kHz. Ramps 5 → 9 → 20 V with no exits once Q4's gate input (P2_L) was reworked | 2026-10-20, 2026-10-28 |
| Q3 bootstrap | Needs the refresh every 5 µs, 300 ns long. Q3's V_gs before a refresh: 3.6–3.9 V; it drops out at 2.2–2.6 V | 2026-10-28 |
| CCM current limit (buck) | Holds a resistor fold with no fault (0.5 A → 12.652 V into 25.3 Ω). Hot-plug of drained VBUS-side caps survives at 9/15/20 V with a 1 A limit | 2026-10-21, 2026-11-02 |
| ID_MON (period mean) | Usable as the limit's input. On 2026-11-02 it read 653 / 631 / 609 mA where V/25.3 Ω is 787 / 654 / 790 mA, so linearity and offset are unverified | 2026-10-21, 2026-11-02 |
| IL_MON | Bandwidth-limited (Dan's capture). No peak-current mode; COMP1's amp thresholds are not a reliable protection | 2026-10-21 decision |
| PD with a fixed contract | A fold of a few percent holds (−6 % at 9 V, −8 % at 20 V for 1.5 s, no detach). A −17 % fold detached and hard-reset about 29 times | 2026-11-02 |
| CCM handbacks at 20 V (OVER_BAND) | 400 kHz: about 0.06/s, each dipping to ~18.5 V for ~1 ms. 200 kHz: about 0.4/s, worst dip 19.3 V | 2026-10-28, 2026-11-02 |
| 1 MHz | Works in pinned CCM 5–15 V. At 9 V a 3.4 kHz line that does not follow ki; VD_MON 2–3 % high | 2026-10-21 (1 MHz branch) |

**Not built:** boost CCM, buck-boost CCM, the limit outside buck, PPS, an input-current limit, and anything tested
below 24 V in.

## 2. Limits by input voltage

**The input path** (ADM1270 at about 10 A through R25 = 5 mΩ as built, Q7/Q8) caps the input current. Assume 9 A to
stay off the ADM1270's 2 ms trip, and η 0.93:

| V_in | Input envelope | Present DCM ceiling (*model*, 200 kHz), 5 / 9 / 15 / 20 / 28 V out |
|---|---|---|
| 10 V | ~84 W | 5 / 10 / 15 / 21 / 27 W |
| 15 V | ~126 W | 7 / 14 / 24 / 31 / 44 W |
| 20 V | ~167 W | 8 / 19 / 24 / 42 / 48 W |
| 24 V | ~201 W | 8 / 21 / 36 / 28 / 47 W |

What follows:
- **>100 W from 10 V is impossible on this hardware.** It needs R25 → 2 mΩ (the schematic value: 25 A) and lower-R
  input FETs (roadmap section 4). From 15 V, 100 W is at the edge (6.9 A in, *model*); from 24 V it fits.
- **From 10 V almost every PDO above ~10 W needs CCM,** and most need boost: buck covers only V_out below ~8 V
  (`BB_BELOW_VIN_MV`).
- **Other ranges:** ID_MON reads to 5.5 A (R6 12 mΩ), which is the 5 A PDO's edge. L1 saturates at 21 A.
- **The source must advertise only what the input can carry.** PDO currents have to follow V_in: at 10 V in, 28 V × 3 A
  (84 W) is the envelope itself.

## 3. Regulation strategy

### 3.1 Mode map

| Region (V_out against V_in) | Light load | Heavy load |
|---|---|---|
| Buck: V_out < D_max·V_in less margin | DCM, rectifier on, pulse skipping (as now) | Buck forced CCM (built) |
| Band: about 0.9–1.1 × V_in | DCM buck-boost (as now) | Four-switch CCM (to build, 3.6) |
| Boost: V_out > V_in/(1 − D_min) | DCM boost (as now) | Boost forced CCM (to build, 3.5) |

- **DCM** stays for light load and for down-ramps, because it cannot drive current backwards.
- **CCM** is entered by load (ID_MON) or by the PPS current-limit mode. It leaves on light load, a down-ramp, or a
  mode change, with hysteresis.

### 3.2 Switching frequency: 200 kHz in CCM, once the handback rule is fixed

**Why 200 kHz:**
- **Efficiency (*model*):** 200 kHz is equal or better at every point modeled: 98.4 % against 98.1 % at 20 V / 2.5 A
  from 24 V. At light load the gap is wider, because the large ripple gives Q1 a soft turn-on at 9–15 V.
- **Duty headroom:** the longest duty is 0.93 at 200 kHz, against 0.86 at 400 kHz and 0.66 at 1 MHz (the 260–300 ns
  refresh slot is a fixed cost per period). At 400 kHz this is what ends buck CCM near 22 V from 24 V (2026-11-02
  open item 2), and from 10 V in it would end buck CCM near 8 V.
- **Handback cost:** DCM at 200 kHz carries about 1.6 A at 20 V; at 400 kHz, 0.54 A (*model*). Every handback at
  400 kHz is a sag (measured: ~18.5 V for ~1 ms); at 200 kHz the worst seen was 19.3 V.

**Against it, measured:** handbacks are about 7× more frequent at 200 kHz with today's rule (3.4).

**Alternatives:**
- **400 kHz** stays as an option where ripple matters more than efficiency.
- **1 MHz is not pursued:** *model* 56–79 % at 5–9 V light load against 70–85 % at 200 kHz, plus the unexplained
  3.4 kHz line at 9 V.

### 3.3 Duty and loss levers, per region

| Lever | Status |
|---|---|
| Dead time (`regulator_ccm_deadtime_ns`, 10–40 ns) | 20 ns default, never swept with meters; *model* 0.08 W at 20 V / 2.5 A / 400 kHz |
| Static-leg refresh | 300 ns every 5 µs is required (2026-10-28); shorter or rarer drops Q3 out |
| Rectifier in DCM | Meter readings at 28 V / ~33 W, 2026-10-16: 9.56 W lost with it off, 1.74–2.72 W with it on |
| Input voltage | The biggest lever at high V_out: boost from 10 V carries 5.2 A in for 50 W, and the input path alone is ~0.5 W (*model*) |
| Output path (Q5, Q6, R6) | The largest term at 5 V / 3 A (0.34 W, *model*); a hardware change |

### 3.4 The CCM handback rule (fix first)

**Today:** CCM hands back to DCM when V_out sits over the pulse-skip band (max(1.5 %, 200 mV)) for 2 PID cycles.
VD_MON's 102 ms spread at 20 V / 200 kHz is σ 42 mV with single samples to +180 mV (2026-11-02), and the 2026-10-23
session reported outliers of ±0.2–0.6 V at 20 V in 1–2 % of samples. The rule trips on measurement noise.

**Proposal:**
- run the check on the median of 9 VD_MON samples the loop already uses, not on single samples;
- widen the band to max(3 %, 400 mV);
- require 1 ms (20 cycles).

The CCM integrator has no clamp above the setpoint, so the relative OVP (+10 %) stays the protection. Success means
zero handbacks in 30 minutes at 20 V, 200 and 400 kHz.

### 3.5 Boost forced CCM (to build)

- **Legs (mirror of buck):**
  - Q4/Q3 switch complementary;
  - Q1 is held on;
  - Q2 refreshes Q1's bootstrap every 5 µs for 300 ns, inside Q4's on-time, so L1 sees 0 V during the refresh, as in
    `240w-roadmap.md` section 5.
- **Duty law:** feedforward D = 1 − V_in/V_ref, with the same integral trim.
  - The small-signal gain from D to V_out is V_in/(1−D)², so the integral gain has to be scheduled with D.
  - The L–C resonance moves with (1−D)/(2π√(LC)).
  - The RHP zero is far above the loop: 81 kHz at 48 V / 5 A (roadmap section 5).
- **Current limit:** the same min(V_ref, v_lim) on ID_MON (output current). The input current is I_out/(1−D); the
  input envelope (3.7) bounds it.
- **Bench order:**
  1. gate check (`bench-ccm-pwm` extended to boost);
  2. 28 V from 24 V into 25.3 Ω;
  3. 20 V from 15 V once a lower input is available.

### 3.6 Four-switch CCM for the band (to build, after 3.5)

- **Why:** buck CCM ends at D_max·V_in, and boost CCM starts at V_in/(1 − D_min). Between them both legs switch.
  V_out = V_in·D₁/(1 − D₂).
- **Proposal:**
  - below V_in, hold D₂ at its minimum and regulate with D₁;
  - above V_in, hold D₁ at its maximum and regulate with D₂;
  - keep one shared voltage loop, so crossing V_in is bumpless.
- **Cost:** two hard turn-ons per period, so the band is the least efficient region; it stays narrow.
- **Until it exists,** the band stays in DCM buck-boost: 15–47 W *model* depending on V_in.

### 3.7 Low-input specifics (10–15 V)

- **An input-current limit in firmware,** below the ADM1270 trip: IS_MON (period mean, input-power trend on
  2026-10-21), calibrated against a meter. It lowers the output voltage command, or the PPS limit, before the ADM1270
  acts.
- **V_in sag:** feedforward already uses the median V_in. A brown-out policy is still needed below ~9 V: stop, or
  hand back to DCM.
- **Housekeeping:** both converters (LMR36015 5 V, LMR36006 3.3 V) accept 4.2–60 V in, so 10 V is fine. The ADM1270's
  UV setting has not been checked.
- **PDO currents follow V_in** (section 2).

## 4. Current regulation for a battery

### 4.1 What the 2026-11-02 resistor test shows

- **Works:** the limit engages and holds with the flag set, releases cleanly, and survives a drained-cap hot-plug at
  9/15/20 V. No regulator fault in any run.
- **Not shown:**
  - **Accuracy:** ID_MON's scatter (σ 21–33 mA) is as large as the 3 % steps used, and its ratio to V/R moved
    between points.
  - **The engage and release transients:** `bu` is too slow for the 102 ms trace.
- **Blocks a battery:** under a fixed contract, a fold past a few percent ends in PD detach/hard-reset loops (29 in
  one run). Hence PPS, as decided 2026-10-21.

### 4.2 Required before a battery is connected

1. **Current measurement calibrated against a meter.** Load current at 0.5 / 1 / 2 / 3 A, at 5 / 9 / 15 / 20 V, and
   during a fold. Accept within 5 % + 50 mA.
2. **A trace trigger on the limit flag** (`REGULATOR_TRACE_FLAG_ILIM`), so engage and release are captured at 20 kHz.
3. **PPS:** source APDO, sink request, the limit as the PPS current limit, and no ±5 % detach while limiting. Proven
   with the resistor.
4. **The limit wherever the pack voltage puts the converter:** a 6S pack (to 25.2 V) is buck-boost or boost from 24 V,
   and anything above ~8 V is buck-boost or boost from 10 V. Needs 3.5 and 3.6 first, unless the first pack sits well
   under V_in in buck.
5. **Hardware:**
   - reverse blocking between VBUS and the pack (the 5 V start and OUTPUT_DIS would otherwise back-feed or drain it);
   - a series resistor for the first runs;
   - contract or PPS voltage ≤ the pack's full voltage (4.2 V × cells).
6. **A connect sequence:** VBUS brought to the pack voltage before the switch closes, and the limit active from the
   first cycle.

### 4.3 Battery ladder (each step on Dan's OK)

| Step | What | Pass |
|---|---|---|
| B0 | Resistor: PPS at 9 / 15 / 20 V with the limit below V/R | Contract holds, current within 4.2.1's tolerance, no fault for 60 s |
| B1 | Small pack (e.g. 3S/4S, under V_in − 3 V), behind diode + 1 Ω, limit 0.5 A | Current within tolerance; no back-feed at start or stop; pack voltage never above the set CV |
| B2 | Same pack, limit 1 → 2 → 3 A | Same; temperatures logged |
| B3 | Target pack, resistor removed, up to the input envelope | Same; efficiency with meters |

## 5. Work order

1. **3.4, the handback rule:** firmware plus a 30-minute bench check at 200 and 400 kHz.
2. **4.2.2, the trace trigger on the limit flag:** firmware.
3. **4.2.1, meter calibration of ID_MON and IS_MON:** needs Dan at the bench.
4. **PPS (4.2.3):** the proposed session; resistor-only bench.
5. **3.5, boost CCM:** gate check, then 28 V from 24 V.
6. **3.7, the input-current limit,** then tests from a lower input. Needs a supply that can be set to 10–15 V.
7. **3.6, four-switch CCM for the band.**
8. **Battery ladder 4.3.**

Steps 1–2 can run without anyone at the bench. 3 and 6 cannot.

## 6. Decisions for Dan

1. **200 kHz as the CCM default** (3.2), conditional on 3.4.
2. **Battery chemistry and cell count for the first pack.** This decides whether step 7 (band CCM) comes before the
   battery.
3. **A 10–15 V input source:** is the bench supply adjustable, or is another supply needed? R25 → 2 mΩ if >100 W from
   10 V is a goal.
4. **Meters for the calibration step:** load current and input current.
