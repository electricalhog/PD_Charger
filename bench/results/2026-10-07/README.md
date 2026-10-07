# Bench results 2026-10-07: EPR contract, VBUS bring-up, load application

Slides built from these files:
[PD Charger EPR Bring-up](https://claude.ai/artifact/LuVh4dgh7WFQNYriDjH7Pz) (private until
shared from its Share menu).

## Setup

- **Source:** NUCLEO-G474 driving the PD_Charger power board from a 24 V supply. The PD build
  ran with real VBUS: `cmake -S . -B build/Debug -DPD_VCONN=ON -DPD_BENCH_FORCE_VCONN=ON
  -DPD_BENCH_15V=ON`, USB-PD core v5.4.1.
- **VBUS:** the power board's output is wired straight to the buck load's input (VIN), not
  through Type-C VBUS.
- **CC:** an Apple 240 W e-marked cable from the G474 side to the sink, with the e-marker at the
  G474 end, so VCONN goes on CC2 (PB5).
- **Sink:** NUCLEO-G431 with the X-NUCLEO-SRC1M1, `bench/pd-sink-g431`.
- **Load:** QT Py RP2040 on the buck converter, `bench/buck-load-qtpy` built with
  `--features power-stage`. An 11 Ω / 10 W resistor on +OUT.
- **Scope:** Rigol DS1104Z, 10× probes, 20 MHz bandwidth limit. CH2 = CC1, CH3 = CC2 (VCONN),
  CH4 = TP2 (VBUS).

## Results

| Result | Numbers | Files |
|---|---|---|
| EPR contract at 15 V | EPR_Mode Enter → Enter_Succeeded in 13 ms, SOP' cable check included. EPR_Request 15 V / 500 mA → PS_RDY in 38 ms (83 ms after Enter). EPR_KeepAlive every 378 ms, each acknowledged. | `fig1_epr_sequence.png`; `epr_trace.log` (decoded); `epr_trace.bin` (raw tracer stream, `bu pd trace --file`); `epr_status.txt` |
| VBUS bring-up | Fresh attach (G474 reset) → 5 V contract in 71 ms. 5 → 9 V: PS_RDY at 182 ms. 9 → 15 V: 268 ms. No Hard Resets or transition timeouts. | `fig2_vbus_bringup.png`; `vbus_attach_5v`, `vbus_5to9`, `vbus_9to15` (`.csv`: `t_s`, `CHAN2`, `CHAN3`, `CHAN4` in V; `_screen.png`); `vbus_reg.jsonl` (power board ADC per contract) |
| Load application under the EPR contract | Targets 0.5 / 2 / 3 W → +OUT 2.24 / 4.47 / 5.48 V, duty 17.1 / 35.0 / 42.8 %, 0.45 / 1.82 / 2.73 W into 11 Ω. VBUS at TP2 13.31–13.33 V, buck VIN 12.96 V, no faults. | `fig3_load_steps.png`; `load_steps_tel.json` (QT Py `TEL` frames tagged with the host target); `load_steps_run.json` (`bu load run` summary); `load_steps_vbus.csv` and `_screen.png` (24 s roll of TP2) |
| VCONN on CC2, the benchmark for CC1 | 5.04 V average powering the e-marker. On with the attach, 120 ms after the sink's Rd appears; off about 35 ms after a detach. Edge: 0 → 2.3 V in about 130 µs, an 80 µs plateau, then about 5.5 mV/µs to 5.0 V (0.75 ms total, no overshoot). E-marker identity `0x1C0005AC 0x00000000 0x72060100 0x110A2640`: Apple, passive, 50 V, 5 A, EPR capable, USB 2.0. | `vconn_cc2_edge.png` (200 µs/div); `vconn_cc2_on_attach.png` (50 ms/div); `vconn_cc2_off_detach.png` (20 ms/div). These came from the dry-run build; VCONN does not depend on it. |

`summary.json` holds the numbers the figures were drawn from.

## Caveats

- **VBUS reads low at TP2.** It sits about 11 % under the contract (13.29 V at the 15 V
  contract) while the power board's own V_out sense (VD_MON) reads on target. Its input sense
  also reads 20.1 V on the 24 V supply.
  - The gap does not change with load, so it is the sensing, not a drop in the wiring.
  - Likely cause: the divider loading changed when the SRC1M1 shield came off the G474.
  - Recalibration is pending; until then every contract voltage runs about 11 % low.
- **Power in the telemetry assumes 10 Ω.** It is V_out² / 10 Ω, load-control's ballast model.
  The real ballast is 11 Ω, so it takes about 9 % less.
- **The attach capture starts from a G474 reset** (`bu probe reset`). While the G474 is in
  reset, VBUS sits at about 2.8 V: the QT Py back-feeds the buck input through the buck's 3V3
  regulator.
- **Every scope trace carries pickup.** Probe ground leads add about ±0.5 V of spikes; the plots
  median-filter them.

## Regenerate

`python3 make_figs.py` in this folder (matplotlib, numpy) rebuilds the three figures and
`summary.json` from the files here.
