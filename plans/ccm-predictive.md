# Predictive synchronous rectification, DCM through CCM (plan, 2026-10-15)

Goal: high-power load steps (QT Py buck into 25 Ω / 25 W, bursts to 125 W, 100 ms–2 s, ≤ 25 W
average over any 10 minutes) and loop tuning at 20–48 V from the 24 V bench supply (> 10 A).

## Where the power limit really is

L1 is 4.7 µH at 200 kHz, so the ripple is large and the converter is DCM or boundary over most of
the range:

| Operating point | Ripple p-p | CCM above (average I_L) |
|---|---|---|
| buck 20 V from 24 V | 3.5 A | 1.8 A (36 W out) |
| boost 28 V from 24 V | 3.6 A | 1.8 A in (≈ 43 W) |
| boost 48 V from 24 V | 12.8 A | 6.4 A in (≈ 150 W) |

Today's ceiling comes from the DCM peak limits (2.5 A buck, 4 A boost and buck-boost) and the
period window, not from DCM itself. At the boundary with 12 A peaks, 48 V boost delivers about
130 W. L1 saturates at 21 A and IL_MON (INA281A2, 5 mΩ) reads up to 13.2 A, forward only.

The second limit is the GaN body diodes: Q2 and Q3 are never driven as rectifiers, so the
rectified current flows at about 2 V. That is about 5 W at 125 W out, and it heats parts that are
not sized for it.

## Load limit

The QT Py is a buck at up to 90 % duty, so the power it can put into 25 Ω is (0.9 V_BUS)² / 25:
13 W at 20 V, 19 W at 24 V, 25 W at 28 V, 42 W at 36 V and 75 W at 48 V. 125 W needs about
15 Ω or less behind the buck at 48 V, or the resistor switched straight onto VBUS (92 W at 48 V).

## Design

**The outer loop stays.** The PI still commands the average output current I_cmd, so
everything below is bumpless by construction, as the mode handovers are today.

**Inner modulator: predictive, per PID cycle (20 kHz, 10 switching periods).**
- **State:** the inductor current at the start of the period, i0. In DCM it is 0; in CCM it is
  predicted.
- **Per period:** charge at slope m1 for t_on. Then rectify synchronously at slope −m2 until either
  the predicted zero crossing (diode emulation: DCM, SR off at t_zero) or the next period (CCM,
  i_end > 0). Slopes per mode: buck (V_in − V_out)/L and V_out/L; boost V_in/L and
  (V_out − V_in)/L; buck-boost V_in/L and V_out/L.
- **Choosing t_on:** iterate the period map over the 10 periods. Choose t_on so the charge delivered
  to the output equals I_cmd · 50 µs and i_end stays on a trajectory toward the steady state. This
  is a deadbeat-limited update with gain < 1, so model error cannot ring.
- **Choosing t_SR:** the end-of-cycle prediction, minus a margin for model error, so a wrong
  prediction leaves a little body-diode conduction rather than negative current.
- **Same code in every regime.** The same few float operations, done 10 times, cover DCM,
  boundary and CCM. There are no regime switches.

**Observer correction.** Add IL_MON to ADC2, oversampled across the period like V_in. That gives
the period-average inductor current while it is positive. A slow complementary correction then
fixes i0 and the model terms the loop now hides:
- at no load I_cmd is 177 mA for a few mA of real load;
- the GaN drop is 2 V, not 0.7 V;
- 50 ns dead time;
- the Q2 refresh leaves a negative pre-current.

**HRTIM.**
- **Explicit sets and resets.** Both legs move to explicit set/reset sources with no
  dead-time-generator complement, because diode emulation needs the SR to end mid-period.
- **Dead time.** It becomes explicit compare spacing, as on Timer A today.
- **More events than compares.** Each leg needs pulse start, pulse end, SR on, SR off, refresh end
  and blanking end, which is more than four compare units. The SR edges come from Timer C and D
  compares as cross-timer events (all timers reset together on the master).
- **Bootstrap refresh.** It stays a short low-side pulse on the static leg. In buck CCM, Q3 is held
  on and Q4 gets the refresh (the LM5176 approach). That is the source of the "Q3 held on costs
  power" result from 2026-10-12, which happens only while Q3 has no current to carry.

**Protection.**
- COMP1 stays a peak backstop, at 150 % of the new peak limit.
- The SR window is clamped so the predicted current never goes below −I_neg (−2 A).
- On the input, VS_MON must stay under its start value + 4 V while current flows backward.
- On a fault, the existing all-off HRTIM fault state still applies.

**Reverse flow** comes last. A negative I_cmd, with the SR on through zero, gives controlled
down-slews; this replaces the planned timed reverse pulses.

## Phases

0. **Bench.**
   - Load firmware: 25 Ω / 25 W params, burst limit 125 W, a 10-minute energy budget enforced in
     firmware and in the host script, and duty-only operation above 34 V (its taps saturate at
     34.6 V). Power then comes from CH1 on the resistor.
   - PDO currents raised to match. Flashing the QT Py needs VBUS off.
1. **Observer.** IL_MON on ADC2. Run the predictive model next to today's DCM modulator and compare
   with IL_MON and the scope (SW2, CH1). Fit the model terms. No change to switching.
2. **SR in DCM.** Diode emulation with a conservative margin; check SW2 for body-diode steps and
   negative-current ringing. Then raise the peak limits in steps (4 → 8 → 12 A) with load bursts.
3. **Boundary and CCM.** Let i_end > 0; this needs Q3 held on in buck with the Q4 refresh.
4. **Reverse flow** for down-slews.
5. **Tuning at 20–48 V.** Load-step metrics as on 2026-10-10/11 (dip, recovery, ζ, regulation),
   bursts up to 125 W within the budget, retune kp/ki for the measured plant.

Each phase is flashed only after review. Power-stage changes are flashed on approval.

## Progress (2026-10-15)

Bench: 25 Ω / 25 W fixed on VBUS; the QT Py buck is back on its 11 Ω / 10 W.

- **Interim power.** Peak limits raised to buck 6 A, buck-boost 10 A, boost 10 A, with the
  comparator capped at 12.5 A. Buck-boost runs up to V_in + 7 V, because DCM boost near V_in is
  limited by the period window (28 V collapsed to 22 V in boost). Boost falls back to buck-boost on
  a sag. PS_RDY waits for the ramp to finish, and VBUS more than 8 % under the setpoint for 200 ms
  ends in a Hard Reset. PDOs are 3 A. Contracts above 28 V are capped at 2 s.
- **Phase 2 (rectifier) works at 5–28 V.**
  - Timers C and D give the rectifier edges, Q4 is driven explicitly, and A–D are preloaded.
  - `regulator_sr_enable` boots 0. It fades in over 100 ms, because switching it on at once at
    5 V overshot to the relative OVP. Pulse skipping in buck also stops the Q4 refresh, which
    otherwise pumps the output.
  - The current command dropped 17–20 % in buck at 9–20 V and 11–14 % in buck-boost at 24–28 V,
    for the same load. The 28 V peak current fell from 9.9 to 9.1 A, and the FETs ran noticeably
    cooler. No reverse current: each conduction ends with a short body-diode tail.
- **36 V with the rectifier:**
  - It held 34.9 V (scope) for the 2 s cap.
  - The loop sat at 95 % of a window-limited ceiling (6.4–7.2 A peaks) and oscillated at 3.9 kHz:
    5.3 V p-p, audible.
  - Next: measure the margin at 28 V by lowering `regulator_loop_fc_hz`, and set the gains per
    mode. 36 and 48 V need Phase 3 (CCM) for headroom.
