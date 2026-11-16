# 2026-11-15: USB-PD build at 15 V before the real load (branch `claude/regulator-epr-pps`)

**Bench.**
- G474 source flashed with `build/EprPps` (20:3x; ELF sha256 `2c2374e817e3352d…`), 24.5 V in.
- VBUS into the 25.3 Ω and the QT Py buck load. NUCLEO-G431 sink on the Type-C cable (CC only). No pack.
- Scope: CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out (TP2, ahead of the output switch).
- Only fixed 5 / 9 / 15 V contracts were requested (Dan). The build also offers 20 V, PPS and EPR 28/36/48 V.
- PD defaults new today: `regulator_ccm_down` 1, `regulator_ccm_kind_switch` 1, `regulator_ccm_entry_blind` 20;
  `regulator_ccm_ilim_slew_ma_ms` 0.

**Script.** `scripts/pd_final.py --phase <name> --go`; one JSON per phase (`pd_final_<phase>_<time>.json`), traces in
`traces/`, scope records in `captures/`.

## Results

| Phase (time) | What ran | Result |
|---|---|---|
| base (20:37) | mailbox START 12 V and SET_VOLTAGE 12 V at the 5 V contract | both refused `PD_OWNS_VBUS`; target stayed 5000 mV |
| walk (20:37) | 5 × (5→9→15→9→5→15→5 V), 30 requests | 30 of 30 contracts; 0 Hard Resets, 0 transition timeouts |
| hold (20:39) | 15 V for 60 s, 334 status reads | RUNNING throughout; V_out mean 14980 mV; ID_MON 594–615 mA |
| ripple (20:40) | 24 ms scope record at 5 V and at 15 V, 12.5 MSa/s | 5 µs means within ±25 mV at both |
| load (20:42) | QT Py 0.5 / 3.5 / 7 / 3.5 / 0.5 W at 15 V; then 7 W with `switch-pulse` 5 ms | see below |
| ccm (20:43) | CCM forced over SWD at 15 V; 7 W; 15→9→5→15 V in CCM; restore | see below |
| reset (20:45, 20:46) | G474 reset at 15 V, 4 times | back at 15 V in 0.7–1.0 s each time; 0 Hard Resets |
| detach (20:50) | QT Py unplugged; sink Rd off at 15 V, then Rd back; forced scope records of TP2 | TP2 under 0.8 V 4.6 ms after the last pulse, 0.00 V from 20 ms; below |

**Contract steps (source's time to PS_RDY, 5 each).** 5↔9 V 60 ms (one 74); 9↔15 V 84 ms (one 90); 5↔15 V 134 ms
(one 144). Scope, first cycle: 5→15 V and 15→5 V are monotonic at about 80 V/s, 25 µs means between 5.02 and
14.83 V on the scope's scale (it reads 0.3 V under the board at 15 V): no overshoot, no undershoot.

**Single status reads.** `v_out_mv` in a status read is one ADC sample. At a settled 5 V target they ranged
4760–6174 mV, at 15 V 14332–15626 mV (13 of 342 beyond ±300 mV in the hold), with the on-time unchanged. The scope
records at TP2 do not show them (5 µs means within ±25 mV over 24 ms). Dan: voltage is good, noise within reason.

**Load.**
- The QT Py reads its VIN as 13.55 V with the G474 at 15.02 V. Whether that is its tap or a drop in the wiring was
  not checked. With `vin_min_mv` 13500 it latched `vin_sag` at once (20:41 run, no load drawn); 12750 (0.85 ×, as on
  2026-11-06) ran.
- Staircase: QT Py `pout_mw` 500 / 3496 / 6997 / 3495 / 556 (means); ID_MON 608–1084 mA; V_out mean 14975 mV.
- `switch-pulse` 5 ms at 7 W (trace `pd_final_20261115_204249_pulse5ms.csv`): V_out max 15281 mV while open,
  min 13694 mV 0.1 ms after the reconnect, inside ±5 % 0.35 ms after it, RUNNING, no fault. The QT Py stayed armed.

**Forced CCM (regulator_ccm_enable 1 → 2 → 1).**
- Entry at 15 V (trace `…_ccm_entry.csv`): V_out 14867–15125 mV over the 100 ms after it; ID_MON 619–668 mA.
- 7 W in CCM for 4 s: CCM in every read, V_out 14660–15263 mV (single reads).
- 15→9→5→15 V in CCM: all three contracts, CCM in every read (10–11 per step), `regulator_ccm_exits` +0, PS_RDY
  84 / 60 / 134 ms. Scope on 15→9 V: −81 V/s, 1 ms means within ±65 mV of a line, landing 8.89–9.02 V (100 µs means).
- Restore to auto: exit LIGHT_LOAD, V_out 14953 mV after. The exit trace armed in CCM and triggered at once, so it
  holds 100 ms of CCM before the exit, not the exit itself.

**Source reset at 15 V.**
- The sink keeps its target, so its first request after the re-attach is 15 V again.
- Forced scope record of the 20:46 reset (`…_reset_15V.csv.gz`, TP2): 14.75 V → 3.22 V within 10 ms of the last
  pulse (the QT Py's back-feed level), switching again 139 ms later, 5.0 V 40 ms after that and straight on to 15 V
  at about 80 V/s (14.2 V 300 ms after the stop). The QT Py's VIN tap shows the same (3.26 V, then 4.4 / 6.7 /
  10.9 / 13.5 V at 60 ms steps).
- The edge-triggered record at 20:45 (CH4 falling through 10 V) did not trigger; why was not found.
- The first reset run (20:44) and the last (20:46) were marked failed by the script's own checks: the first expected
  5 V after the reset, the last was interrupted by the cable bump.

**Unplanned detach (20:47, Dan bumped the cable).** Source events: CAD_DETACHED, stack stop, CAD attach 343 ms later,
5 V contract. The status read at the detach shows IDLE with OUTPUT_DIS on; the QT Py's VIN read 3.27 V, then 4.65 V.
No Hard Reset, no fault. The sink's target went back to 5 V.

**vSafe0V (20:50, QT Py unplugged so nothing back-feeds VBUS; 25.3 Ω still on VBUS).**
- Sink `rd off` at the 15 V contract. Scope, TP2 (`…_detach_15V.csv.gz`, 1 ms means, 91 mV per count): 14.70 V
  5 ms before the last pulse; under 5 V after 1.4 ms, under 0.8 V after 4.6 ms, 0.09 V at 10 ms, 0.00 V from 20 ms
  to the end of the record (1.3 s), apart from the blip below.
- TP2 is ahead of the output switch and VBUS itself has no probe. TP2 can only fall that fast into the 25 Ω, so the
  switch was still conducting during the fall (inference).
- The status read 40 ms after `rd off`: IDLE, OUTPUT_EN off, OUTPUT_DIS on. `v_out_mv` then stays at one value
  (4674 mV): the ADC fields are only updated while RUNNING, so status cannot show vSafe0V. The script's "under
  800 mV" check failed for that reason, not because of the board.
- **A second attach 127 ms after the detach.** Source events: CAD_DETACHED, CAD_ATTACHED 127 ms later, CAD_DETACHED
  18 ms after that. TP2 rose to 4.78 V for about 10 ms (135–150 ms after the first detach) and was back at 0.09 V
  by 155 ms. Which side produced the second attach (the sink re-enabling Rd, or the CC lines) was not checked.
- Rd back (`rd both`): TP2 0.00 V before the attach, 4.82 V 10 ms after the first pulse, 5.03 V at 40 ms, then the
  sink's 15 V request: 14.68 V at 200 ms, no overshoot (max 14.75 V). No Hard Reset on either side.
- A Hard Reset's own vSafe0V step did not run tonight: nothing on this bench sent one.

**End state.** RUNNING at the 5 V contract, no fault, `regulator_ccm_enable` 1, QT Py unplugged. Source counters
since the last reset: 0 Hard Resets, 0 transition timeouts, 0 VBUS-on failures.

**Not tested tonight.** Anything above 15 V in this build, PPS, EPR, auto CCM entry (the bench load tops out near
1.1 A, which stays in DCM), a load step in CCM, a Hard Reset from either side.
