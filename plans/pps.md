# SPR PPS: source APDO, Current Limit, bench sink (2026-11-05)

**Why.** Dan's decision of 2026-10-21 (`ccm-buck-50w.md` 8.4, `regulation-strategy.md` 4.2.3): current limiting for a
battery happens inside a PPS contract. The sink sets an operating current, and the source folds the voltage in
Current Limit (CL), where the ±5 % checks don't apply. Battery step B0 is PPS into the resistor.

## 1. Requirements (USB PD R3.2 V1.1, `USB_PD_R3_2 V1.1 2024-10.pdf` in the usbpd checkout)

| Item | Value | Where |
|---|---|---|
| Ranges (3 A, 16–60 W PDP) | 9V Prog 5–11 V, 15V Prog 5–16 V, 20V Prog 5–21 V | Table 2.2 |
| Not in EPR mode | "PPS operation is not available in EPR Mode" | 2.8.2 |
| APDO | B31..30 = 11b, B29..28 = 00b (SPR PPS), B27 PPS Power Limited, max V B24..17 (100 mV), min V B15..8 (100 mV), max I B6..0 (50 mA) | `usbpd_def.h` |
| PPS RDO | position B31..28, B26 mismatch, B22 EPR capable, voltage B20..9 (20 mV), current B6..0 (50 mA) | Table 6.25 |
| vPpsNew / vPpsValid | RDO V ±5 % / an extra ±0.1 V | Table 7.23 |
| vPpsStep / vPpsSmallStep | 20 mV / ±500 mV | |
| tPpsSrcTransSmall / Large (PS_RDY after the Accept's GoodCRC) | 25 / 275 ms. Small: once VBUS is in vPpsNew **or the supply is in CL** | 7.1.4.2.1 |
| vPpsSlewPos / Neg | ≤ 30 mV/µs (the regulator slews at 80 mV/ms) | |
| tPpsTransient | 5 ms back within vPpsNew after a load step (CV, ≥ 60 mA) | |
| iPpsCLMin | 1 A. A request below it "Should" be rejected; if it is accepted, the limit is 1 A | 7.1.4.2.2 |
| iPpsCLNew | ±150 mA for 1–3 A | |
| iPpsCLStep / tPpsCLProgramSettle | 50 mA / 250 ms | |
| tPpsCVCLTransient / tPpsCLSettle / tPpsCLCVTransient | 250 / 250 / 275 ms | |
| iPpsCLTransient | new load ±100 mA while in CL | |
| vPpsShutdown | 0.85–0.95 × APDO minimum while in CL: the source May Hard Reset | |
| Real-time flag OMF | Set in CL, cleared in CV (PPS_Status) | 6.5.10 |
| tPPSRequest (sink) / tPPSTimeout (source) | Sink ≤ 10 s (usbpd re-requests every 5 s) / the ST PE Hard Resets after 14 s | `usbpd_core.h` |

## 2. Design

**Source (G474).**
- **APDO, behind CMake `-DPD_BENCH_PPS=ON`.** One SPR PPS APDO after the fixed PDOs: 5 V to the Prog range of the
  highest fixed PDO (11 / 16 / 21 V), at `PD_SRC_MAX_CURRENT_MA`.
- **Request evaluation (`pd_policy_evaluate_rdo`):**
  - Rejected: voltage outside the range or off the 20 mV grid, current above the APDO, or any PPS request in EPR mode.
  - Accepted below 1 A, with the limit set to 1 A (`pd_pps_cl_min_ma`, PD_PPS_CL_MIN_MA). Lowering that
    variable over SWD honours lower limits for resistor-only CL tests (bench only, non-compliant).
- **Regulator in a PPS contract:** the CCM setting stays (auto by default), with `regulator_ccm_ilim_ma` as the
  CL limit and `regulator_ccm_limiting` as OMF.
  - Not forced CCM (Dan, 2026-11-06): a sudden drop in V_out may need DCM. Since 2026-11-06 the limit acts in
    CCM and DCM alike (regulator.c step 5d): one `v_lim` lowers the reference of whichever loop runs, so CCM
    entries and exits keep it.
  - Leaving PPS restores `CCM_ILIM_MA_DEFAULT`.
  - The regulator's relative UVP (50 % of the setpoint) would latch SW_UVP in a deep fold. While limiting,
    `regulator_cl_uvp_mv` = 0.7 × the APDO minimum (3.5 V) replaces it, below vPpsShutdown.
  - Gap: in DCM the limit reads ID_MON, calibrated in CCM (1.05x + 24 mA); its DCM accuracy is a bench item.
- **PS_RDY (`USBPD_PWR_IF_SetProfile`):**
  - Sent once V is within ±5 % of the RDO voltage, **or** the regulator is limiting.
  - The step that picks the budget is the larger of the steps from the old setpoint and from V_out (a fold leaves
    V_out below the setpoint).
  - Budgets inside 275 ms (large step) and 25 ms (small step), measured end to end with the CC1 decoder.
- **VBUS checks (`pd_vbus_poll`):**
  - The −8 %/200 ms "VBUS lost" Hard Reset does not apply while limiting in PPS.
  - It is replaced there by vPpsShutdown: below 0.9 × APDO minimum for 10 ms, Hard Reset.
- **PPS_Status:** `Is_GetPPSStatus_Supported`, carrying V_out (20 mV), ID_MON (50 mA) and OMF.

**Status 2026-11-05.** P1 is done and P2 (CV) passed on the bench:
- PS_RDY within 25 ms for steps up to 500 mV (worst 18 ms) and within 275 ms for large steps (worst 212 ms for
  5 → 21 V).
- 0 Hard Resets in 25 steps and two holds (32 s at 9 V, 63 s at 21 V).

Details: `bench/results/2026-11-05/README.md`.

**Status 2026-11-06.** No forced CCM; the limit acts in DCM too (above). P4 run below spec (`pd_pps_cl_min_ma`
100 over SWD) into 25.3 Ω, all in DCM:
- limits 250/500/300 mA held (V/R within 20 mA), 60 s at 21 V / 500 mA, 0 Hard Resets;
- release fixed: the PPS budget is sized from V_out as well as the setpoint.
Forced CCM (run 3): the limiter works through entries and exits, but ID_MON reads 15–29 % low in CCM at 18–21 V,
so a 500 mA limit let about 700 mA through (outside ±150 mA). DCM and CCM at 6–9 V were within ±20 mA. Run 4, after the CCM calibration (`ccm_transitions`):
forced-CCM limits within −15..+8 mA of V/R, 0 Hard Resets. Details:
`bench/results/2026-11-06/README.md`.

**Sink (G431) and `bu`.**
- `pps <mV> <mA>`, built with `Pps` RDOs. The usbpd PE repeats the request every 5 s.
- `bu sink pps`, and the APDO and PPS fields in `bu pd status`.

## 3. Steps

| # | What | Pass |
|---|---|---|
| P1 | Firmware above, sink `pps`, tools | Builds; host checks of the RDO decode |
| P2 | Bench CV: PPS 5.00 → 21.00 V in 20 mV, 100 mV, 1 V and 16 V steps into 25.3 Ω | Accept → PS_RDY ≤ 25 / 275 ms (decoded on CC1), V within ±5 % ±0.1 V, the contract held 60 s (≥ 12 periodic requests), 0 Hard Resets |
| P3 | tPPSTimeout: the sink stops re-requesting | Source Hard Resets at about 14 s |
| P4 | Bench CL (B0): the limit below the load current | Current within ±150 mA, OMF set, no Hard Reset for 60 s, release back to the RDO voltage within 275 ms |

**Resolved 2026-11-06 (Dan):** test with a lower limit, below spec. Was: P4 needs a load above 1 A. The 25.3 Ω draws 1 A only at 25.3 V, above the 21 V PPS maximum, and the
QT Py is a constant-power load (CL into it collapses to vPpsShutdown). The options:
- the bench option that honours limits below 1 A, resistor only;
- a second resistor (≈ 12 Ω for 1.75 A at 21 V).
