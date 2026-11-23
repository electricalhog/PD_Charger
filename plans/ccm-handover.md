# Inductor current at buck CCM entry and exit (2026-11-06: built and bench-tested, §4)

**Why.** Dan: think about the inductor current targets when switching in and out of CCM. Today the entry is
voltage-bumpless (`u0` reproduces V_out) and the exit presets the DCM loop to CCM's filtered current. Neither looks at
the inductor current itself.

## 1. What happens now (model: 24.4 V in, 200 kHz, L1 4.7 µH, buck)

The period starts with Q4's refresh during Q2's freewheel, so at the period boundary L1 is at its valley,
I_v = I_load − ΔI/2. DCM leaves L1 at 0 A at the same point.

The numbers below are model estimates; the DCM ceiling uses an approximate window.

| V_out | Ripple ΔI | Boundary I = ΔI/2 | DCM ceiling | Enter (85 %) → I_v | Exit (50 %) → I_v |
|---|---|---|---|---|---|
| 9 V | 6.0 A p-p | 3.0 A | 2.1 A | 1.8 A → −1.2 A | 1.1 A → −2.0 A |
| 15 V | 6.2 A p-p | 3.1 A | 2.2 A | 1.8 A → −1.2 A | 1.1 A → −2.0 A |
| 20 V | 3.8 A p-p | 1.9 A | 1.35 A | 1.15 A → −0.8 A | 0.7 A → −1.2 A |

- **Entry.** The DCM ceiling sits below the boundary, because the window leaves room for the refresh and margins. So
  every auto entry starts CCM with L1 at 0 A where steady CCM wants about −1 A.
  - The first CCM periods then run the right triangle shifted up by |I_v|: about 1 A more than the load goes into
    the output caps.
  - Volt-second balance per period does not remove that offset. It decays as an L1–C ring, damped by the load, the
    ESR and CCM's integrator.
  - Forced CCM and PPS entries at light load are worse: I_v ≈ I_load − ΔI/2, down to −3 A.
- **Exit.** CCM ends at the boundary with L1 at about −1.2 to −2 A. With Q2 off, that current returns to 0 through
  Q1's body diode into the input (about 1 µs, with recovery loss).
  - The DCM loop restarts at i0 = max(CCM's filtered current, 0.3 × ceiling).
  - CCM's current is ID_MON, which reads 22–24 % low at 15–18 V at 200 kHz (bench 2026-11-06). So DCM restarts below
    the load and V_out sags until the PID's integrator catches up. That is consistent with the earlier "sags on
    each CCM exit" (inference).
- **Hysteresis mixes two currents.** Entry compares the DCM model's I_cmd and exit compares ID_MON. With ID_MON 22 %
  low, the exit fires at about 1.4 A of true load at 15 V instead of 1.1 A, and the band between them shrinks.

## 2. Proposed targets

1. **One current for both thresholds, put as the valley.** Enter when I_v,est ≥ −h_in and leave when I_v,est ≤ −h_out,
   with I_v,est = I_load,est − ΔI/2 and ΔI = (V_in − V_out) V_out T / (V_in L).
   - In DCM, I_load,est is the DCM loop's I_cmd (its average output current); in CCM it is ID_MON once corrected
     (item 3).
   - Near the boundary the handover error is small by construction.
2. **Entry: pre-bias the first period to the steady valley.** ~~Hold off Q1~~ (superseded: a hold-off only shifts
   the pulse inside a fixed period, so the volt-seconds and L1's end current do not change). Built instead: change
   the first period's on-time by dt = I_v L / V_in (negative at light load).
   - This is deadbeat: after one period L1 is on the steady triangle.
   - It needs a one-period override, because the PID cycle (50 µs) spans 10 periods. Built in the PID ISR: the
     preload is released for exactly one Timer A update (UDIS, UPD flag, wait bounded to 2 periods), then the steady
     set is written.
3. **Exit: hand DCM the true load, and end on zero current.**
   - Preset i0 to the corrected load current, not ID_MON as read.
   - End the last CCM period with Q2 off at L1's zero crossing (DCM's rectifier window, SR_*) rather than at the
     boundary, so no current goes back through Q1's body diode.
4. **ID_MON in CCM first.** The exit threshold, i0, SW_OCP and the PPS limit all use it.

## 3. Checks before building

- ID_MON at 200 vs 400 kHz CCM at the same points, and with the refresh every 2nd period (interval 10 µs).
- The entry and exit transients as they are now: TP2 AC-coupled, triggered on the mode change, auto mode on a load
  step through the thresholds.
- Host test (`tests/buck_ccm`): add the DCM → CCM handover from L1 = 0 A and check the ring with and without the
  pre-bias.

## 4. Built and measured (2026-11-06)

Details are in `bench/results/2026-11-06/ccm_transitions/README.md`.

**Code** (`regulator.c` step 5a/5b/6c, `buck_ccm.c`, `regulator_config.h`):
- **ID_MON:** DCM gain/offset 1059 / −54, plus a buck CCM add-table by duty at 200 kHz (`ID_CCM_ADD_MA`; removed 2026-11-09: into a pack it read 158 mA high, bench 2026-11-09 section 14).
  - CCM and DCM are now within ±13 mA of V/R at 5–21 V.
  - With the QT Py adding 3.5–7 W, CCM is within 19 mA of DCM.
  - The cause of the CCM deficit is not found; it needs a probe on ID_MON (PC1).
- **Valley:** `buck_ccm_boundary_a()` gives the steady boundary current from the programmed edges, V_out and a 0.8 V
  freewheel drop. It is within 0.05 A of the measured valleys.
- **Entry:** one period at t_on + I_v L / V_in.
- **Exit:**
  - one more CCM period at t_on − I_v L / V_in, with Q3 kept on until DCM's set is taken;
  - DCM restarts at CCM's current plus DCM's command offset over the load (learned at the entry, or
    `CCM_EXIT_DCM_ADD_MA`);
  - no 30 % floor.
- **Switch:** `regulator_ccm_handover` (default 1; 0 gives the old behaviour).

**Bench** (25.3 Ω, forced transitions):
- **Entry:** the valley after one period is within 0.1 A of steady. Before, the offset was 0.9–2.2 A.
  - The TP2 overshoot fell from 475 / 743 / 444 mV to 90 / 308 / 268 mV at 9 / 15 / 20 V.
- **Exit:**
  - L1 ends at −0.05 to −0.23 A.
  - Q3's body diode no longer conducts; Q4's conducts 170–185 ns (500–900 ns before).
  - The first DCM pulse is within 4 % of steady.
  - The 20 V sag is −0.16 V, against −1.09 V before.

**Open:**
- **A V_out step between modes** (CCM settles 66–157 mV higher on TP2) is separate from L1. It was there before as
  well. Inference: VD_MON reads differently in DCM and CCM, and u0 carries it.
- **Auto transitions:** not captured. They need the QT Py load.
- **400 kHz:** not calibrated. DCM at 400 kHz and light load starves the CPU (uwTick froze, PD hung), so the
  correction and the handover run only at 200 kHz buck.
