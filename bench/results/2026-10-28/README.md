# Bench log 2026-10-28: CCM entry integrator from the fit; the 5 V forced-CCM start still latches SW_OVP (remote)

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

Files:
- screenshot `scope_ccm_entry_1923.png`;
- `data/scope_ccm_entry_1923_ch1234_4ns.csv`;
- `data/trace_ccm400_5v_entry_1923_ovp.csv`.

**End state:** FAULT (SW_OVP), outputs off, `regulator_ccm_enable` 2, `regulator_buck_fsw_khz` 400, sink target 5 V.
Not cleared.

Data: `data/trace_ccm400_5v_entry_u0fit_ovp.csv` (19:12), `data/trace_ccm400_5v_entry_1920_ovp.csv` (19:20).
