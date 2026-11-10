# 2026-11-05: SPR PPS, first slice (CV only)

Firmware: `PD_BENCH_15V/20V/24V/EPR_28V/FORCE_VCONN` plus the new **`PD_BENCH_PPS`** (plans/pps.md). Sink: `pps <mV> <mA>`
(bench/pd-sink-g431). V_in 24.45 V. Load: 25.3 Ω only; the QT Py is off.

**Scope.** CH2 = CC1 (0.5 V/div), CH4 = TP2 (5 V/div). CH1 = SW1 and CH3 = SW2 are off during captures and
restored after. Captures are HRES, 60k points at 50 ms/div (10 µs per point), single-shot on CC1 falling through
0.5 V. Script: `scripts/pps_cv.py`, data: `data/pps_cv.jsonl`; three captures are in `data/cc1/`.

## 1. Capabilities and contract

- **Advertised.** Source_Capabilities = 5/9/15/20/24 V fixed at 3 A, plus **PPS 5.0–21.0 V, 3 A at position 6**
  (`bu pd status` and the sink's `caps` agree). EPR 28 V stays at position 8.
- **First request.** `pps 9000 1500` was accepted. The contract shows PPS, CL = 1500 mA, forced CCM on, not in CL
  (25.3 Ω draws 0.36 A), V_out 9.003 V.
- **Contract hold.**

  | Hold | Periodic re-requests | Hard Resets | Regulator |
  |---|---|---|---|
  | 9 V, 32 s | 7 (each ready in 8 ms) | 0 | — |
  | 21 V, 63 s | 12 | 0 | RUNNING throughout; VD_MON single samples 20.82–21.42 V |

- **Back to a fixed contract.** `req 15000` restored `regulator_ccm_enable` = 1 (auto),
  `regulator_ccm_ilim_ma` = 3000 and `regulator_cl_uvp_mv` = 0.

## 2. CV steps: PS_RDY timing (CC1)

The CC1 bursts are, in order, Request, GoodCRC, Accept, GoodCRC, PS_RDY, GoodCRC; six were found in every
capture. The "GoodCRC → PS_RDY" column runs from the end of the GoodCRC after Accept to the start of PS_RDY,
which is the interval the spec bounds. The "wait" column is SetProfile's in-range wait (`pd_status.transition_ms`).
In every capture of the second run, PS_RDY started **1.95–1.97 ms** after the wait ended.

| From → to (mV) | Step | Wait (ms) | GoodCRC → PS_RDY (ms) | Spec (ms) | Hard Reset |
|---|---|---|---|---|---|
| 9000 → 9020 | +20 mV | 8 | 9.97 | 25 | 0 |
| 9020 → 9040 | +20 mV | 10 | 11.96 | 25 | 0 |
| 9040 → 9140 | +100 mV | 10 | 11.96 | 25 | 0 |
| 9140 → 9240 | +100 mV | 10 | 11.96 | 25 | 0 |
| 9240 → 9740 | +500 mV | 16 | 17.96 | 25 | 0 |
| 9740 → 10240 | +500 mV | 16 | 17.96 | 25 | 0 |
| 10240 → 11240 | +1 V | 22 | 23.96 | 275 | 0 |
| 11240 → 12000 | +760 mV | 18 | 19.96 | 275 | 0 |
| 12000 → 15000 | +3 V | 46 | 47.96 | 275 | 0 |
| 9000 → 15000 | +6 V | 84 | 85.96 | 275 | 0 |
| 15000 → 21000 | +6 V | 84 | 85.96 | 275 | 0 |
| 21000 → 20500 | −500 mV | 16 | 17.96 | 25 | 0 |
| 20500 → 20000 | −500 mV | 16 | 17.96 | 25 | 0 |
| 20000 → 5000 | −15 V | 196 | 197.97 | 275 | 0 |
| **5000 → 21000** | **+16 V** | **210** | **211.97** | **275** | 0 |
| 21000 → 15000 | −6 V | 86 | 87.95 | 275 | 0 |
| 15000 → 9000 | −6 V | 84 | 85.96 | 275 | 0 |
| 21000 → 9000 (first run) | −12 V | 160 | (no capture) | 275 | 0 |

- **Margins.** The worst large step leaves 63 ms of the 275 ms; the worst small step (500 mV) leaves 7 ms of the
  25 ms. Small steps are set by the 80 mV/ms slew plus the 10 ms in-range filter (5 × 2 ms).
- **V_out.** The mean of 8 VD_MON reads after each step was within 1.3 % of the request (worst 9140 → 9018 mV).
- **Not checked.** TP2 at 5 V/div reads 0.2–0.4 V above VD_MON at 5–9 V, so it cannot check vPpsValid (±0.1 V
  beyond ±5 %). PS_RDY is gated on VD_MON within ±5 %.

## 3. Bench notes (scope)

- **Depth won't change in AUTO sweep.** The DS1104Z ignores `:ACQ:MDEP` while AUTO sweep keeps taking 3 M-point
  frames at 50 ms/div. Each attempt also stops it answering the error query for a while. Setting NORM sweep on the
  idle CC1 first fixed it.
- **2-channel depths.** With two channels on, 120k is not a valid depth (6k/60k/600k/…). A 2-channel AUTO
  capture timed out and its process held VISA ("Resource busy") until killed.
- **Trigger level.** A 1.25 V trigger on CC1 fired on switching noise at 15–21 V, giving empty records. 0.5 V
  works (BMC lows reach about 0 V).

## 4. Not done yet

- **CL (P4 / battery B0).** Not run. 25.3 Ω draws 1 A only at 25.3 V, above the PPS maximum, so a fold needs
  either `pd_pps_cl_min_ma` lowered over SWD (non-compliant, bench only) or a heavier load (plans/pps.md, open).
- **tPPSTimeout (P3).** Not run: the usbpd crate re-requests every 5 s on its own, and the sink has no switch to
  stop that yet.
- **Get_PPS_Status.** Answered from VD_MON, ID_MON and OMF, but the sink cannot send it yet, so it is untested.
