# Bench log 2026-10-22: ab2443e flashed; forced CCM handbacks traced to VD_MON noise (remote)

| Field | Entry |
|---|---|
| Date | 2026-10-22, about 21:06 – 21:31 EDT |
| Operator | Claude Code, remotely through `bu`. Dan: "OK to flash for CCM and PPS", then "go ahead with the traced CCM rerun" |
| Firmware | ab2443e, ELF sha256 `c187e98e8b488f49…`. Defaults: CCM's current is ID_MON (gain 952 ‰), limit 3 A |
| Hardware since 2026-10-21 (Dan) | 1× coax from IL_MON to scope CH1. A VCONN switch wire near D10 (PB6 = CC1) came loose; the CC1 VCONN enable (PA7) reads high in `pd status` |
| Load | 25.3 Ω on VBUS; the QT Py idle |
| Status | **Forced CCM at 9 V hands back to DCM on over-band every few ms, and once latched SW_OVP. The cause is in the V_out measurement: VD_MON is about 5× noisier than on 2026-10-21 while VBUS on the scope is quiet. Likely the new IL_MON coax (PA1, next to VD_MON on PA0, and a second ground path). Not confirmed** |

## 1. First attempt (21:06): SW_OVP at 9 V

- **Setup:** stop, `regulator_ccm_enable` 2 and `regulator_buck_fsw_khz` 400, then a 9 V and then a 20 V request from the sink.
- **At 9 V:** CCM left on OVER_BAND seven times in 26 ms, 120 entries in all, then latched SW_OVP.
  - Fault snapshot: V_out 9968 mV, setpoint 9000 mV, V_in 24.06 V.
- **The 20 V request:** never reached ±5 % in tPSTransition, so the sink sent a Hard Reset each time.
- **Recovery:** cleared after CCM was set back to 0 and 200 kHz.

## 2. Traced rerun (21:28)

| Point | Trace σ V_out | Mean \|ΔV\| per 50 µs sample | Max \|ΔV\| | CCM exits | 2026-10-21, same point |
|---|---|---|---|---|---|
| 5 V hold, CCM | 19.7 mV | — | — | 0 | 38.3 mV |
| 5 → 9 V ramp | — | — | 586 mV | 10 OVER_BAND | 1 (RAMP_DOWN) |
| 9 V hold, CCM | 94.7 mV, p-p 603 mV | 139 mV | 586 mV | 69 more, all OVER_BAND | σ 36.9 mV, mean \|ΔV\| 28 mV, max 173 mV |

- **VBUS on the scope at the 9 V hold, as 45 µs means** (what VD_MON's trimmed mean spans): σ 11.7 mV, p-p 90 mV, mean
  sample-to-sample change 11.9 mV. The output is quiet and VD_MON is not. The over-band check takes two samples over
  the band, so the measurement noise alone makes CCM hand back to DCM.
- **The integrator** sat at +102 mV at 5 V, against −210 mV on 2026-10-21. The on-time was 505 ns against 476 ns.
- **ID_MON** read normally: 221 mA at 5 V and 374–386 mA at 9 V, as the 2026-10-21 fit predicts. The 616–648 mA of
  attempt 1 was taken during the handback cycling.
- **5 V DCM** trace σ was 18–26 mV today, against 12.7 mV on 2026-10-21.
- **IL_MON on CH1** (5 V DCM, 2 µs/div) is a clean, slow hump of about 0.45 V per pulse, lagging SW1 by about 1 µs.
  There is no ringing from the coax load.

Data: `data/trace_ccm400_5v_hold.csv`, `data/trace_ccm400_5to9v_overband.csv`, `data/trace_ccm400_9v_hold.csv`.

## 3. End state

- The board runs ab2443e: RUNNING, 5 V contract, buck DCM 200 kHz, `regulator_ccm_enable` 0, no latched fault.
- On the scope:
  - CH1 is IL_MON at 1×, 0.2 V/div.
  - CH2, CH3 and CH4 are as the parent session left them: SW1, SW2, and VBUS at 1 V/div with offset −5 V.
  - The trigger is CH2 rising at 12 V, AUTO.
- The switch-pulse test was not run.

## 4. Next

1. Remove the IL_MON coax, or lift its shield at the scope end. Then repeat the 9 V CCM hold. If VD_MON's σ returns to
   about 37 mV, the coax was the cause.
2. Then the planned `switch-pulse 20` at 20 V / 400 kHz with the 3 A ID_MON limit. The host model predicts a peak of
   20.4–20.8 V (22.3 V without the limit) with an unchanged dip.
