# Bench log 2026-10-21: buck CCM at 1 MHz, light load (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-21, about 05:00 EDT |
| Operator | Claude Code, remotely through `bu`, on Dan's request "try 1mhz for light load ccm". Nobody at the bench |
| Branch | `claude/ccm-1mhz-light` (worktree), 54c3601 = bb06331 plus 1 MHz and pinned CCM. ELF sha256 `9dcc62c2…` |
| Setup | As 2026-10-20 (`../2026-10-20/README.md`): 25.3 Ω on VBUS, QT Py idle, 24 V in, sink contracts. CH2 SW1, CH3 SW2, CH4 VBUS (DC, 20 MHz limit) |
| End state | bb06331 image reflashed (`79938803…`), 5 V buck DCM, 200 kHz, CCM off, no fault |

## Method

- **Run settings:**
  - `regulator_buck_fsw_khz` = 1000;
  - `regulator_ccm_enable` = 3 (pinned): DCM cannot run at 1 MHz;
  - `regulator_ccm_ocp_ma` = 5500;
  - refresh 200 ns every 5 µs, so every 5th period.
- **How the period took effect.** It only changes at buck entry. A 9 V request with the regulator stopped made PD hard-reset and re-attach, and `VBUSOn` restarted the regulator at 1 MHz.

## Results (25.3 Ω)

| Contract | SW1 | Duty | Q3 dropouts | VBUS (scope, DC) | VBUS σ (2 µs mean) | Trace σ | Largest line | u |
|---|---|---|---|---|---|---|---|---|
| 5 V | 1.000 MHz | 0.196–0.200 | 0 | 5.03 V | 9–12 mV | 25 mV | ≤ 5.4 mV | −0.34 V |
| 9 V | 1.000 MHz | 0.348 | 0 | 8.88 V | 35–40 mV | 65–69 mV | 31–52 mV at 3.3–3.5 kHz | −0.55 V |
| 15 V | 1.000 MHz | 0.588 | 0 | 14.96 V | 25 mV | 37 mV | 7.6 mV at 6.0 kHz | −0.58 V |

- **No faults.** One entry per (re)start, and no CCM exits besides the stop. Contract changes 5 → 9 → 15 → 9 → 5 V all stayed in CCM, including the down-ramps.
- **The 3.4 kHz line at 9 V is not a loop-gain effect:**

  | ki | Line |
  |---|---|
  | 1571 /s | 30.5 mV at 3320 Hz |
  | 3142 /s | 36.2 mV at 3486 Hz |
  | 6283 /s | 44.9 mV at 3809 Hz |

  The likely cause is VD_MON's oversampled window (8 conversions over 3.58 µs) catching Q1's edges, about 4 per window at 1 MHz, so the reading depends on where the edge falls.
- **VD_MON bias at 1 MHz.** The scope's VBUS against VD_MON (which regulates to the setpoint) was 8.88 V at 9 V and 14.96 V at 15 V. At 200 kHz the same probe read 9.12–9.14 V at 9 V. So VD_MON reads about 2–3 % high at 1 MHz, 9–15 V; at 5 V there is no offset.
- **Integrator offset.** u is −0.34 to −0.58 V, against −0.02 to −0.46 V at 200 kHz. Fixed timing errors (Q1 is 14 ns wider than commanded) are a bigger share of a 1 µs period.

## Efficiency

**Not measured:** there were no meters, and the board's current sensors are unusable in CCM (2026-10-20 §4.5).

**Model** (`plans/scripts/ccm_buck.py`, buck_point; refresh every 5 µs, dt 20 ns):

| Point | 200 kHz | 400 kHz | 1 MHz |
|---|---|---|---|
| 5 V / 0.2 A | 70.3 % | 69.4 % | 56.0 % |
| 9 V / 0.36 A | 85.1 % | 84.8 % | 78.9 % |
| 12 V / 0.47 A | 89.7 % | 89.9 % | 86.2 % |
| 15 V / 0.6 A | 93.6 % | 93.4 % | 90.7 % |

- At 1 MHz, Q1's hard turn-on (Q_oss·V_in·f ≈ 0.55 W) dominates.
- Soft switching needs a valley current of about −2.4 A with a 20 ns dead time. 200 kHz gets it at 9–15 V; 1 MHz never does (ripple 0.9–1.3 A p-p).
- So 1 MHz is the lowest-ripple option, not the efficient one. Light-load efficiency points toward lower frequency with a deliberate negative valley (ZVS / TCM), or DCM with pulse skipping.

**Check with a meter at the bench:** P_in at J2 and P_out at the load, 200 kHz against 1 MHz at 5 / 9 / 15 V.

## Open

- **The 9 V limit cycle and the VD_MON bias.** Sample VD_MON in a quiet part of the period, or with an integer number of periods per window.
- **1 MHz tops out near 15.8 V** at 24 V in, with the 260 ns refresh slot.
