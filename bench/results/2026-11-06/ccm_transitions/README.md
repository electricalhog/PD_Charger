# 2026-11-06: buck CCM entry and exit, before and after the inductor handover

**Bench.**
- V_in 24.4 V, buck at 200 kHz with the refresh every period (5000 ns), into 25.3 Ω only. The QT Py is off except in §1.
- PPS contracts (CV, 1500 mA). Transitions are forced over SWD (Dan, 2026-11-06):
  - entry: `regulator_ccm_enable` 0 → 2;
  - exit: 2 → 0 (exit DISABLED).
- Auto mode is not exercised, because at 25.3 Ω it stays in DCM (see §5).
- Probes: CH1 SW1, CH3 SW2, CH4 TP2 (AC, 20 MHz BWL), 10x. The scope captures 300k points per channel (4 ns), 100 µs/div, with a pulse-width trigger on SW1 at the first pulse of the new mode.

**Scripts** (`scripts/`):
- `trans.py` takes one capture.
- `ana.py` does the analysis and draws the plots.
- `ccm_idmon.py` (in `../scripts`) and `qtpy_cal.py` do the calibration.

**Data** (`data/`):
- `trans.jsonl`, with the raw CSV paths under `bringup_out/`;
- `trans_metrics.jsonl`;
- `cal_check*.jsonl`, `cal.jsonl`, `sw1_steady.jsonl`, `dcm_icmd_ratio.jsonl`.

**Before and after.**
- "Before" is the new firmware with `regulator_ccm_handover` 0, `regulator_id_ccm_corr` 0 and ID gain/offset 952 / 0, which is the behaviour up to 2026-11-06.
- "After" is the new defaults.

**L1 current: how it is reconstructed (inferred).**
- I_L = ∫(SW1 − SW2)/L with L1 = 4.7 µH.
- In DCM it is re-zeroed at every zero-current ring.
- In CCM it continues from the adjacent DCM ring, with a linear drift chosen so that the CCM window furthest from the step averages V/25.3 Ω.
- How far to trust each part:
  - Currents one period away from a DCM ring are robust: the start and end of a handover period, and the last CCM valleys before an exit.
  - Levels far from the step carry about ±0.2 A of drift from 8-bit probe differences.

## 1. Calibration: CCM's current (ID_MON corrected) against V/R

| V_out | Before (952/0, no CCM add): DCM | Before: CCM | After (1059/−54 + CCM table): DCM | After: CCM |
|---|---|---|---|---|
| 5 V | +24 | +27 | −3 | −1 |
| 9 V | +14 | −12 | 0 | +4 |
| 13 V | +4 | −108 | +11 | +1 |
| 15 V | −14 | −155 | +1 | −1 |
| 17 V | −19 | −174 | −1 | 0 |
| 19 V | −32 | −173 | −10 | −13 |
| 21 V | (auto in CCM) −118 | −126 | +6 (CCM) | +1 |

All values are error in mA. "Before" comes from `../data/ccm_idmon.jsonl` (200 kHz, refresh every period). "After" comes from `data/cal_check.jsonl`. Both are means of 10 reads.

- **DCM:** a fit over 24 points gives raw = 0.944 I + 51 mA, so the correction is 1.059 × raw − 54 mA.
- **CCM:** ID_MON reads low by a duty-dependent amount: 0 at 5 V, 164 mA at 17 V, about 103 mA at 21 V. This is added back by duty (`ID_CCM_ADD_MA`, 200 kHz buck only).
- **Check with extra load** (QT Py, same V_out both modes; `data/cal_check_qtpy.jsonl`):

  | Point | DCM | CCM |
  |---|---|---|
  | 15 V + 7 W | 1095 mA | 1099 mA |
  | 20 V + 3.5 W | 1099 mA | 1080 mA |

- **Cause not found.** The deficit is not the refresh: every second period gave the same ratio. It also does not follow the ripple: 9 V and 15 V have the same 6 A p-p and different deficits. Finding it needs a probe on ID_MON (PC1).
- **Only calibrated at 200 kHz and 24.4 V in.** 400 kHz was not calibrated: see §5.

## 2. What the inductor does (SW1/SW2, measured; current inferred)

- **DCM** (before an entry): t_on is 772 / 1456 / 2812 ns at 9 / 15 / 20 V.
  - L1 discharges with SW1 at −3.1 V (Q2's reverse conduction) and SW2 at V_out + 2 V (Q3 is off, body diode).
  - It then rings at 0 A.
  - Using that drop, the pulse-method mean is within +10 / +6 / +2 % of V/R (`sw1_steady.jsonl`).
- **Steady CCM valley**, measured one period from a DCM ring:

  | V_out | Valley |
  |---|---|
  | 9 V | −2.54 to −2.60 A |
  | 15 V | −2.39 to −2.44 A |
  | 20 V | −1.12 to −1.16 A |

  - Ripple is 5.6–6.0 A p-p at 9–15 V and 3.6 A at 20 V.
  - While Q2 is on, SW1 sits at −0.6 to −0.9 V. The ideal valley model (no drop) is 0.09–0.20 A too shallow; with a 0.8 V freewheel drop it is within 0.05 A.
- **Old entry** (first CCM period at the steady on-time from 0 A):
  - The whole triangle starts about 2 A high at 9–15 V and 0.9 A high at 20 V.
  - It works down over 6–8 periods as an L1–C ring: V_out overshoots by up to 0.74 V (`entry_*V.png`, red).
- **Old exit:**
  - ODISR takes Q3 off mid-way through the last CCM period. SW2 jumps to V_out + 2 V, so Q3's body diode conducts for 0.4–1.4 µs.
  - L1 then reverses through Q4's body diode for 0.5–0.9 µs (SW2 −2.8 V), while Q2 is still on.
  - DCM then restarts at i0 = max(ID_MON as read, 30 % of the ceiling):
    - At 9 V that is too high: first DCM t_on 940 vs 772 ns steady, +291 mV, six skip bursts.
    - At 15–20 V it is too low, because ID_MON read 20 % low and DCM's command runs above the load: 2368 vs 2812 ns at 20 V, **−1.09 V not recovered after 880 µs**.

## 3. The handover (built)

The handover is an on-time change for one period, not a hold-off. A shift within the period keeps the volt-seconds, so it cannot move the current.
- Changing one period's on-time by dt moves the boundary current by V_in·dt/L.
- `buck_ccm_boundary_a()` gives the steady boundary current from the programmed edges, V_out and the 0.8 V freewheel drop (`CCM_HANDOVER_VFW_MV`).
- `buck_ccm_handover_ticks()` gives dt = ΔI·L/V_in.

**Entry:**
- The first period runs t_on + dt, with dt = I_v·L/V_in, taking L1 from 0 A to I_v.
- That set is released for exactly one Timer A update, then the steady set is written. UDIS stays held, and the wait on the UPD flag is bounded to 2 periods; `regulator_ccm_handover_timeouts` stayed 0 throughout.

**Exit:**
- One more CCM period runs at t_on − I_v·L/V_in, ending at about 0 A.
- Q3 and Q4's outputs go off only after the DCM set has been taken.
- DCM restarts at CCM's current plus what DCM's command runs above the load:
  - the value learned at the entry (I_cmd − measured), if the setpoint is within 500 mV of the entry's;
  - otherwise `CCM_EXIT_DCM_ADD_MA` by V_out: 169 mA at 5 V, 150 at 9 V, 83 at 15 V, 82 at 20 V (`dcm_icmd_ratio.jsonl`).

**Host test:** `tests/buck_ccm` test_handover uses a switch-level stiff-V_out model, 5–20 V, 0.2–2 A, with and without the drop.
- Entry lands on the valley and exit ends at 0 A, within 5 % of the ripple.
- Without the handover, the offset is the full −I_v.

## 4. Before / after (same points, `entry_*V.png`, `exit_*V.png`)

**Entry** (0 → 2):

| V_out | First CCM on-time (steady) | Valley after the 1st period − steady valley | TP2 max / min | TP2 final | Settle (±40 mV of final) |
|---|---|---|---|---|---|
| 9 V before | 1684 ns (1696) | **+2.14 A** | +475 / −172 mV | +42 mV | 259 µs |
| 9 V after | 1208 ns (dt −473) | **−0.08 A** | +90 / 0 mV | +66 mV | 57 µs |
| 15 V before | 2816 ns (2816) | **+2.02 A** | +743 / +2 mV | +149 mV | 246 µs |
| 15 V after | 2380 ns (dt −428) | **−0.05 A** | +308 / +19 mV | +157 mV | 359 µs |
| 20 V before | 3768 ns (3764) | **+0.92 A** | +444 / −11 mV | +62 mV | 503 µs |
| 20 V after | 3588 ns (dt −176) | **+0.07 A** | +268 / −1 mV | +102 mV | 366 µs |

**Exit** (2 → 0):

| V_out | Last CCM on-time | I_L at its end | Q3 diode / Q4 diode (one steady period) | First DCM t_on (steady DCM) | TP2 min / max / final | Skip bursts in 880 µs | Settle |
|---|---|---|---|---|---|---|---|
| 9 V before | 1700 ns | via the diodes | 396 / 872 ns (48) | 940 ns (772) | −34 / +291 / +98 mV | 6 | 891 µs |
| 9 V after | 2164 ns (+476) | −0.18 A | 4 / 176 ns (48) | 768 ns | −36 / +146 / −31 mV | 0 | 400 µs |
| 15 V before | 2812 ns | via the diodes | 512 / 896 ns (88) | 1528 ns (1456) | −456 / +88 / −26 mV | 2 | 753 µs |
| 15 V after | 3232 ns (+424) | −0.23 A | 8 / 168 ns (112) | 1500 ns | −128 / +138 / −46 mV | 1 | 174 µs |
| 20 V before | 3752 ns | via the diodes | 1444 / 496 ns (136) | 2368 ns (2812) | **−1093** / +60 / −1089 mV | 0 | not settled |
| 20 V after | 3932 ns (+186) | −0.05 A | 8 / 184 ns (132) | 2820 ns | −164 / +81 / −159 mV | 0 | 308 µs |

How the numbers are defined:
- "Steady valley" is the last CCM valleys of the exit capture with the same setting, because those are robust.
- The diode times count SW2 > V_out + 1 V (Q3's body diode) and SW2 < −1 V (Q4's) over the last CCM period. Bracketed values are the same count over a steady CCM period (dead time).
- All runs: RUNNING, no fault, 0 handover timeouts.

What changed:
- **Entry:** the inductor offset is gone. After one period the valley is within 0.1 A of steady, against 0.9–2.2 A before, and the L1–C ring no longer appears.
- **Exit:**
  - Q3's body diode no longer conducts (≤ 8 ns).
  - Q4's body diode conducts for 170–185 ns, against 50–130 ns of dead time in a steady period and 500–900 ns before.
  - The first DCM pulse is within 4 % of steady DCM.
  - The 20 V sag is −0.16 V, against −1.09 V before.

## 5. What is left (measured; causes are inference)

- **V_out steps between modes, separate from L1.**
  - After an entry, TP2 settles 66 / 157 / 102 mV higher at 9 / 15 / 20 V than in DCM. That is the same as before (+42 / +149 / +62), so it is not the handover.
  - After an exit, TP2 settles 31 / 46 / 159 mV lower.
  - The 15 V after-entry peak (+308 mV), and its ~360 µs settle, follow the first CCM periods' duty: the valleys rise by 0.3 A in periods 2–4.
  - Inference: VD_MON reads differently in DCM and CCM (ADC sync placement), and CCM's u0 carries it.
  - The one skip burst at the 15 V exit comes from DCM starting above its skip band. Probe TP2 against VD_MON in both modes before changing u0 or the sync.
- **Auto transitions** were not captured: at 25.3 Ω auto stays in DCM. They need the QT Py above about 7 W (20 V) or 19 W (15 V).
- **400 kHz:** not calibrated. The ID correction and handover apply only at 200 kHz buck (`sw_period_ticks == HRTIM_PERIOD_COUNTS`).
  - At the 5 V PPS point, DCM at 400 kHz starved the CPU: the skip-watchdog ISR and the PID ISR took 100 %, so TIM2's uwTick froze.
  - The PD task then hung, the sink went into Hard Reset, and the source's PE stayed in SRC_READY with no caps until `bu probe reset`.
  - So 400 kHz cannot be entered by an fsw change at low V.
- **IS_MON** reads 3–11 % lower in CCM. Not investigated.
