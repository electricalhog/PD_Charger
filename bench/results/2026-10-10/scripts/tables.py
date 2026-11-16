"""Markdown and CSV tables from metrics/metrics.json (analyze.py)."""
import csv
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
M = json.loads((HERE.parent / "metrics" / "metrics.json").read_text())
OPS = M["operating_points"]
OUT = HERE.parent / "metrics"


def md(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join(out)


def w(v):
    return "–" if v == 0 else f"{v:g} W"


def nan(x, fmt):
    return "–" if x != x else format(x, fmt)


# 1. DC level and regulation
head = ["Contract", "Load target", "Into 11 Ω", "VBUS, scope", "VBUS, board ADC", "Error, scope", "Error, ADC",
        "Buck VIN (QT Py)", "Load duty", "Source on-time"]
rows = []
for r in OPS:
    rows.append([f"{r['contract_v']:g} V", w(r["load_target_w"]), nan(r["load_p_11ohm_w"] if r["load_target_w"] else float("nan"), ".2f") + (" W" if r["load_target_w"] else ""),
                 f"{r['vbus_scope_v']:.4f} V", f"{r['vbus_adc_v']:.3f} V", f"{r['setpoint_err_pct_scope']:+.2f} %",
                 f"{r['setpoint_err_pct_adc']:+.2f} %", nan(r["buck_vin_qtpy_v"], ".3f") + (" V" if r["load_target_w"] else ""),
                 nan(r["load_duty_pct"], ".1f") + (" %" if r["load_target_w"] else ""), f"{r['on_time_ns']:.0f} ns"])
t1 = md(head, rows)

# 2. load regulation
lr = M["load_regulation"]
tr = M["transients"]
rows = []
for v in ("15000", "20000"):
    x = lr[v]
    t = tr[str(int(v) // 1000)]
    rows.append([f"{int(v) / 1000:g} V", f"{x['scope_noload_v']:.4f} V", f"{x['scope_3w_v']:.4f} V",
                 f"{1e3 * (x['scope_3w_v'] - x['scope_noload_v']):+.1f} mV", f"{x['load_reg_pct_scope']:+.2f} %",
                 f"{1e3 * (x['adc_3w_v'] - x['adc_noload_v']):+.0f} mV", f"{x['load_reg_pct_adc']:+.2f} %",
                 f"{t['dc_shift_hold_mv']:+.1f} mV"])
t2 = md(["Contract", "No load (scope)", "3 W (scope)", "Shift, scope", "Load regulation, scope", "Shift, board ADC",
         "Load regulation, ADC", "Shift, 6 s run (scope)"], rows)

# 3. time-domain ripple and noise
head = ["Contract", "Load", "Vpp 20 MHz", "Vrms 20 MHz", "99.9 % range 20 MHz", "200 kHz ripple p-p", "Vrms full BW",
        "8 Hz–20 kHz rms", "8 Hz–20 kHz p-p", "< 8 Hz swing"]
rows = [[f"{r['contract_v']:g} V", w(r["load_target_w"]), f"{r['vpp_bw20_mv']:.0f}", f"{r['vrms_bw20_mv']:.1f}",
         f"{r['vpp999_bw20_mv']:.0f}", f"{r['ripple_sync_pp_mv']:.0f}", f"{r['vrms_full_mv']:.1f}",
         f"{r['rms_8hz_20khz_mv']:.1f}", f"{r['pp_8hz_20khz_mv']:.0f}", f"{r['vpp_ma20ms_mv']:.0f}"] for r in OPS]
t3 = md(head, rows)

# 4. band rms
bands = [("band_10_1000_mv", "10 Hz–1 kHz"), ("band_1000_10000_mv", "1–10 kHz"), ("band_10000_100000_mv", "10–100 kHz"),
         ("band_100000_1000000_mv", "100 kHz–1 MHz"), ("band_1000000_20000000_mv", "1–20 MHz"),
         ("band_20000000_62500000_mv", "20–62.5 MHz")]
rows = [[f"{r['contract_v']:g} V", w(r["load_target_w"])] + [f"{r[k]:.2f}" for k, _ in bands] for r in OPS]
t4 = md(["Contract", "Load"] + [b for _, b in bands], rows)

# 4b. low-frequency noise hump
rows = [[f"{r['contract_v']:g} V", w(r["load_target_w"]), f"{r['lf_floor_20_60hz_uv_rthz']:.0f}",
         f"{r['lf_hump_hz']:.0f} Hz", f"{r['lf_hump_uv_rthz']:.0f}", f"{r['lf_hump_db']:+.1f} dB"] for r in OPS]
t4b = md(["Contract", "Load", "Floor 20–60 Hz, µV/√Hz", "Hump peak", "Hump level, µV/√Hz", "Hump over floor"], rows)

# 5. dominant lines
rows = []
for r in OPS:
    lt = sorted(r["tones_lf"], key=lambda t: -t["mv_rms"])[:3]
    ht = sorted(r["tones_hf"], key=lambda t: -t["mv_rms"])[:3]
    f = lambda t: f"{t['f_hz'] / 1e3:.1f} kHz" if t["f_hz"] >= 1e4 else f"{t['f_hz']:.0f} Hz"
    rows.append([f"{r['contract_v']:g} V", w(r["load_target_w"]),
                 ", ".join(f"{f(t)}: {t['mv_rms']:.1f} ({t['dbuv']:.0f} dBµV)" for t in lt),
                 ", ".join(f"{f(t)}{' (' + t['id'] + ')' if t['id'] else ''}: {t['mv_rms']:.1f} ({t['dbuv']:.0f} dBµV)" for t in ht)])
t5 = md(["Contract", "Load", "Largest lines below 10 kHz, mV rms", "Largest lines 10 kHz–20 MHz, mV rms"], rows)

# 6. transients
rows = []
for v in ("15", "20"):
    t = tr[v]
    lo, hi = t["band20k_before_mv"]
    rows.append([f"{v} V", f"{lo:+.0f} / {hi:+.0f} mV",
                 f"{t['arming_dip_20k_mv']:.0f} mV ({t['arming_dip_1k_mv']:.0f} mV)", f"{t['arming_recover_ms']:.1f} ms",
                 f"{t['release_peak_20k_mv']:+.0f} mV ({t['release_peak_1k_mv']:+.0f} mV)",
                 f"{100 * t['release_peak_20k_mv'] / 1e3 / float(v):+.1f} %", f"{t['release_recover_ms']:.1f} ms"])
t6 = md(["Contract", "Steady band, no load (20 kHz LP)", "Dip at load start-up, 20 kHz LP (1 kHz LP)", "Back in band",
         "Overshoot at release, 20 kHz LP (1 kHz LP)", "Overshoot / contract", "Back in band"], rows)
rows = []
for v in ("15", "20"):
    t = tr[v]
    ex = ", ".join(f"{m:+.0f} mV @ {ms:.2f} ms" for ms, m in t["arming_ring_extrema_ms_mv"][:4])
    rows.append([f"{v} V", ex, f"{t['arming_ring_hz']:.0f} Hz", f"{t['arming_ring_decay_per_cycle']:.1f}×",
                 f"{t['arming_ring_zeta']:.2f}"])
t7 = md(["Contract", "Extrema after the start-up dip (2 kHz LP, re no-load level)", "Ring frequency", "Decay per cycle",
         "ζ (log decrement)"], rows)

(OUT / "tables.md").write_text("\n\n".join([t1, t2, t3, t4, t4b, t5, t6, t7]) + "\n")
with (OUT / "summary.csv").open("w", newline="") as fh:
    keys = [k for k in OPS[0] if not isinstance(OPS[0][k], (list, dict))]
    wr = csv.DictWriter(fh, fieldnames=keys)
    wr.writeheader()
    for r in OPS:
        wr.writerow({k: r[k] for k in keys})
with (OUT / "tones_python.csv").open("w", newline="") as fh:
    wr = csv.writer(fh)
    wr.writerow(["label", "f_hz", "mv_rms", "dbuv", "id"])
    for r in OPS:
        for t in r["tones_lf"] + r["tones_hf"]:
            wr.writerow([r["label"], t["f_hz"], t["mv_rms"], t["dbuv"], t["id"]])
print((OUT / "tables.md").read_text())
