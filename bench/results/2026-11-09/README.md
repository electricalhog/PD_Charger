# 2026-11-09: bench check after the setup was power-cycled

Firmware as flashed on 2026-11-06 (PPS, CCM handover, ID_MON calibration). Load 25.3 Ω, QT Py off.

## 1. Found: SW_UVP latched at power-up

- `bu regulator status`: FAULT, SW_UVP, fault at 136 ms after boot (`regulator_fault_tick_ms`).
- Fault snapshot (`regulator_fault_snapshot`): V_out 17 mV, **V_in 33 mV**, setpoint 5000 mV.
- So the sink attached and the regulator started for the 5 V contract before the 24 V input was up, and V_out
  could not rise (inference from the snapshot: the source boots from its ST-LINK USB before the bench supply).
- Since then, 254 attaches each ended in VBUS_ON_FAILED → Hard Reset, with the fault latched and no switching.
- VS_GOOD and IS_GOOD were 1 when checked, so input was present by then.

**Recovery:** one `bu regulator clear-fault` → RUNNING, V_in 24.42 V, V_out 4.97 V, 5 V contract.

**Gap (firmware):** a start with V_in at about 0 latches SW_UVP instead of waiting for the input. A source that
boots before its input supply has to be power-cycled or cleared by hand. Fixed in section 4.

## 2. Functional check after recovery

| Request | State | Mode | V_out (mV) | I_out, CCM's current (mA) | V/R (mA, inferred) | V_in (mV) | IS_MON (mA) |
|---|---|---|---|---|---|---|---|
| fixed 9 V | RUNNING | BUCK DCM | 9020 | 354 | 357 | 24382 | 196 |
| fixed 15 V | RUNNING | BUCK DCM | 14936 | 594 | 590 | 24382 | 444 |
| fixed 20 V | RUNNING | BUCK DCM | 19851 | 790 | 785 | 24365 | 747 |
| PPS 12 V, 1.5 A | RUNNING | BUCK DCM | 12038 | 486 | 476 | 24382 | 319 |
| PPS 5 V, 1.5 A | RUNNING | BUCK DCM | 4949 | 193 | 196 | 24399 | 87 |
| fixed 15 V | RUNNING | BUCK DCM | 15143 | 595 | 599 | 24365 | 441 |

Single status reads. All requests matched, no new Hard Reset (count 254 before and after), no fault.

At 15 V, mean of 20 reads: V_out 14954 mV, V_in 24373 mV, IS_MON 447.5 mA (σ 4.2), ID_MON corrected 595 mA.

## 3. IS_MON against Dan's ammeter (24 V input, 10 mA display steps)

Fixed contracts into 25.3 Ω, buck DCM, 200 kHz. IS_MON is the mean of 20 status reads (σ 3–4 mA). P_out is V²/R
(inferred).

| V_out (mV) | V_in (mV) | Ammeter (mA) | IS_MON (mA) | IS_MON − meter | P_out / P_in (meter) |
|---|---|---|---|---|---|
| 4990 | 24408 | 100 | 85.5 | −14.5 | 0.40 |
| 15028 | 24372 | 400 | 446.2 | +46.2 | 0.92 |
| 20052 | 24367 | 710 | 748.9 | +38.9 | 0.92 |

- **Not one line.** A least-squares line gives IS_MON = 1.087 × meter − 11.5 mA, with residuals of −12, +23 and
  −11 mA. IS_MON reads low at 5 V and high at 15–20 V.
- **Not separated yet.** The meter may include housekeeping current that IS_MON does not see, and buck DCM's input
  current is a short pulse (about 0.45 µs at 5 V). Either could bend the curve (inference).

**Zero point (Type-C unplugged, regulator IDLE, INPUT_EN off):** ammeter about 10 mA (housekeeping upstream of the
input switch). ADC3 DR (IS_MON) read directly at 0x50000440: 10–11 counts, so **IS_MON's zero is +32 mA**.

With the 10 mA housekeeping removed from the meter and the 32 mA zero removed from IS_MON (assuming the idle
housekeeping also holds while switching, which is not checked):

| V_out | Stage current, meter − 10 (mA) | IS_MON − 32 (mA) | Ratio |
|---|---|---|---|
| 5 V | 90 | 53.5 | 0.59 |
| 15 V | 390 | 414.2 | 1.06 |
| 20 V | 700 | 716.9 | 1.02 |

- At 15–20 V, IS_MON is a 32 mA offset plus a gain within 2–6 %.
- At 5 V it reads 40 % low. That fits the short DCM input pulse (about 0.45 µs) not being followed by the amplifier,
  but this is not checked (inference). A probe on IS_MON, or more points between 5 and 15 V, would check it.

## 4. Fix: a start without input stays IDLE (no SW_UVP latch)

`regulator_start()` now reads V_in (VS_MON) after the input path comes up. If it is below `VIN_START_MIN_MV`
(9000 mV, `regulator_config.h`), it drops the input path and returns IDLE with no fault. The next VBUSOn (the next
attach or Hard Reset) tries again. Refusals are counted in `regulator_start_low_vin_count`; the V_in of the latest
one is in `regulator_start_low_vin_mv`. `bu regulator status` shows both.

- **Why VS_GOOD did not catch it:** not established. VS_GOOD passed the start's wait, yet V_in read 33 mV.
- **Normal start (flashed, 9000 mV):** RUNNING, buck, V_in 24433 mV, V_out 4915 mV, 5 V contract, 0 refusals, 0 Hard
  Resets.
- **Refusal path (temporary build with the threshold at 30000 mV, above the 24.4 V input), about 15 s:**
  - state IDLE with no fault source, INPUT_EN 0, OUTPUT_EN 0, OUTPUT_DIS 1;
  - 50 refusals at V_in 24535–24552 mV;
  - 26 attaches, 26 Hard Resets, 51 VBUS_ON_FAILED, 0 fault Hard Resets: one retry about every 0.6 s, with no latch.
- **Restored to 9000 mV and flashed:** RUNNING, V_in 24450 mV, V_out 4967 mV, 5 V contract at the first attach.
- **Not tested:** the supply itself coming up after boot. The test above kept the input on and raised the threshold.
  A supply that drops out while RUNNING still latches a fault (SW_VIN_RANGE in buck, SW_UVP in buck-boost).
- The sink's TCPP02 read `tcpp_fault` 1, `tcpp_ok` 0 (ack 0x18). It read the same earlier today, before this
  flash. Not investigated.
