"""Ripple and noise at the 28 V EPR contract, by the 2026-10-10 method (op28.py records).

usage: python3 noise28.py   (cap/v28_p{0,500,2000,3000}_{hf,hfw,lf} + _op.json;
                             writes ../metrics/noise28.json, ../metrics/noise28_tables.md,
                             ../figures/v28_spectra.png)

The metric code is 2026-10-10/scripts/analyze.py's, unchanged, except that CH4 is
AC-coupled here (op28.py): there is no scope DC level, the DC columns come from the board
ADC, and the < 8 Hz swing is shaped by the scope's AC-coupling corner (not comparable with
the DC-coupled 2026-10-10 figure).
"""
import json
from pathlib import Path

import numpy as np
from scipy import signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
CAP = HERE.parent / "data" / "cap"
OUT = HERE.parent
OPS = [("v28_p0", 28000, 0), ("v28_p500", 28000, 500), ("v28_p2000", 28000, 2000), ("v28_p3000", 28000, 3000)]
F_SPLIT = 10e3
BANDS_LF = [(10, 1e3), (1e3, 10e3)]
BANDS_HF = [(10e3, 100e3), (100e3, 1e6), (1e6, 20e6)]
BANDS_HFW = [(20e6, 62.5e6)]
F_SW_SOURCE, F_SW_LOAD = 200e3, 438.6e3


def load(lab):
    m = json.loads((CAP / f"{lab}.json").read_text())
    p = m["waveforms"]["CH4"]["preamble"]
    c = np.fromfile(CAP / f"{lab}_CH4.bin", dtype=np.uint8)
    v = (c.astype(np.float64) - p["yorig"] - p["yref"]) * p["yinc"]
    return v, 1.0 / p["xinc"], m


def psd(ac, fs, nperseg):
    f, pxx = signal.welch(ac, fs=fs, window="hann", nperseg=nperseg, noverlap=nperseg // 2,
                          detrend=False, scaling="density")
    return f, pxx


def band_rms(f, pxx, lo, hi):
    m = (f >= lo) & (f < hi)
    return float(np.sqrt(np.trapezoid(pxx[m], f[m]))) if m.sum() > 1 else float("nan")


def tones(ac, fs, fmin, fmax, n=8, min_db_over_floor=12.0):
    w = signal.windows.flattop(len(ac), sym=False)
    x = np.fft.rfft(ac * w)
    f = np.fft.rfftfreq(len(ac), 1 / fs)
    a = np.abs(x) * 2 / w.sum() / np.sqrt(2)
    m = (f >= fmin) & (f < fmax)
    fi, ai = f[m], a[m]
    floor = np.median(ai)
    pk, _ = signal.find_peaks(ai, distance=max(1, int(len(ac) / fs * max(fmin, 5.0))))
    pk = [i for i in pk if ai[i] > floor * 10 ** (min_db_over_floor / 20)]
    pk = sorted(pk, key=lambda i: ai[i], reverse=True)[:n]
    return [{"f_hz": round(float(fi[i]), 1), "mv_rms": round(1e3 * float(ai[i]), 3),
             "dbuv": round(20 * np.log10(ai[i] / 1e-6), 1)} for i in sorted(pk, key=lambda i: fi[i])], float(f[1] - f[0])


def sync_ripple_pp(ac, fs, fsw=F_SW_SOURCE, f_lp=2e6, blocks=4):
    n = int(round(fs / fsw))
    assert abs(fs / fsw - n) < 1e-6, "record rate must be a multiple of fsw"
    x = signal.sosfiltfilt(signal.butter(4, f_lp, "lp", fs=fs, output="sos"), ac)
    q = len(x) // n // blocks
    return float(np.mean([np.ptp(x[b * q * n:(b + 1) * q * n].reshape(q, n).mean(axis=0)) for b in range(blocks)]))


def lf_hump(f, pxx, lo=100.0, hi=8e3, ref=(20.0, 60.0)):
    fc = np.geomspace(lo, hi, 120)
    med = np.array([np.median(pxx[(f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))]) for c in fc])
    i = int(np.argmax(med))
    floor = float(np.sqrt(np.median(pxx[(f >= ref[0]) & (f < ref[1])])))
    return float(fc[i]), float(np.sqrt(med[i])), floor


def name_line(fhz):
    for base, nm in ((F_SW_SOURCE, "source fsw"), (F_SW_LOAD, "load fsw")):
        k = fhz / base
        if k >= 0.95 and abs(k - round(k)) < 0.02:
            return f"{nm} ×{round(k)}" if round(k) > 1 else nm
    return ""


rows, spectra = [], {}
for lab, vnom, p_mw in OPS:
    op = json.loads((CAP / f"{lab}_op.json").read_text())
    reg = op["reg_before"] + op["reg_after"]
    hf, fs_hf, m_hf = load(f"{lab}_hf")
    hfw, fs_hfw, m_hfw = load(f"{lab}_hfw")
    lf, fs_lf, m_lf = load(f"{lab}_lf")
    r = {"label": lab, "contract_v": vnom / 1000, "load_target_w": p_mw / 1000, "coupling": "AC"}
    r["vbus_adc_v"] = float(np.mean([x["v_out_mv"] for x in reg])) / 1000
    r["vin_supply_adc_v"] = float(np.mean([x["v_in_mv"] for x in reg])) / 1000
    r["on_time_ns"] = float(np.mean([x["on_ns"] for x in reg]))
    r["setpoint_err_pct_adc"] = 100 * (r["vbus_adc_v"] - vnom / 1000) / (vnom / 1000)
    ld = op.get("load", {})
    st = (ld.get("steps") or [{}])[0]
    r["buck_vin_qtpy_v"] = st.get("vin_mv", {}).get("mean", float("nan")) / 1000 if p_mw else float("nan")
    r["load_p_10ohm_w"] = st.get("pout_mw", {}).get("mean", float("nan")) / 1000 if p_mw else 0.0
    r["load_p_11ohm_w"] = r["load_p_10ohm_w"] * 10 / 11
    r["load_duty_pct"] = st.get("duty_pm", {}).get("mean", float("nan")) / 10 if p_mw else float("nan")
    for tag, v in (("bw20", hf), ("full", hfw)):
        ac = v - v.mean()
        r[f"vpp_{tag}_mv"] = 1e3 * float(np.ptp(ac))
        r[f"vrms_{tag}_mv"] = 1e3 * float(ac.std())
        r[f"vpp999_{tag}_mv"] = 1e3 * float(np.percentile(ac, 99.95) - np.percentile(ac, 0.05))
    r["ripple_sync_pp_mv"] = 1e3 * sync_ripple_pp(hf - hf.mean(), fs_hf)
    aclf = lf - lf.mean()
    hp = signal.sosfiltfilt(signal.butter(4, 8.0, "hp", fs=fs_lf, output="sos"), aclf)
    bp = signal.sosfiltfilt(signal.butter(4, 20e3, "lp", fs=fs_lf, output="sos"), hp)
    e = int(0.05 * fs_lf)
    r["pp_8hz_20khz_mv"] = 1e3 * float(np.ptp(bp[e:-e]))
    r["rms_8hz_20khz_mv"] = 1e3 * float(bp[e:-e].std())
    f_lf, p_lf = psd(hp[e:-e] - hp[e:-e].mean(), fs_lf, 2 ** 18)
    f_hf, p_hf = psd(hf - hf.mean(), fs_hf, 2 ** 17)
    f_hw, p_hw = psd(hfw - hfw.mean(), fs_hfw, 2 ** 17)
    for lo, hi in BANDS_LF:
        r[f"band_{int(lo)}_{int(hi)}_mv"] = 1e3 * band_rms(f_lf, p_lf, lo, hi)
    for lo, hi in BANDS_HF:
        r[f"band_{int(lo)}_{int(hi)}_mv"] = 1e3 * band_rms(f_hf, p_hf, lo, hi)
    for lo, hi in BANDS_HFW:
        r[f"band_{int(lo)}_{int(hi)}_mv"] = 1e3 * band_rms(f_hw, p_hw, lo, hi)
    hz, a, floor = lf_hump(f_lf, p_lf)
    r["lf_hump_hz"], r["lf_hump_uv_rthz"], r["lf_floor_20_60hz_uv_rthz"] = hz, 1e6 * a, 1e6 * floor
    r["lf_hump_db"] = 20 * np.log10(a / floor)
    t_lf, res_lf = tones(hp[e:-e] - hp[e:-e].mean(), fs_lf, 10, F_SPLIT)
    t_hf, res_hf = tones(hf - hf.mean(), fs_hf, F_SPLIT, 20e6)
    k20 = int(20e-3 * fs_lf)
    cs = np.concatenate([[0.0], np.cumsum(lf)])
    ma20 = (cs[k20:] - cs[:-k20]) / k20
    r["vpp_ma20ms_mv_ac"] = 1e3 * float(np.ptp(ma20))
    for t in t_lf + t_hf:
        t["id"] = name_line(t["f_hz"])
    r["tones_lf"], r["tones_hf"] = t_lf, t_hf
    r["clipped"] = {k: m["waveforms"]["CH4"]["clipped"] for k, m in (("bw20", m_hf), ("full", m_hfw), ("lf", m_lf))}
    r["scope"] = {k: {"fs": m["settings"]["sample_rate_sa_s"], "points": m["waveforms"]["CH4"]["points"],
                      "v_div": m["settings"]["channels"]["CH4"]["scale_v_div"],
                      "coupling": m["settings"]["channels"]["CH4"]["coupling"],
                      "bwl": m["settings"]["channels"]["CH4"]["bw_limit"], "acq": m["settings"]["acquire_type"]}
                  for k, m in (("bw20", m_hf), ("full", m_hfw), ("lf", m_lf))}
    rows.append(r)
    spectra[lab] = (f_lf, p_lf, f_hf, p_hf)
    print(f"{lab}: {r['vbus_adc_v']:.4f} V adc, {r['vpp_bw20_mv']:.0f} mVpp / {r['vrms_bw20_mv']:.1f} mVrms (20 MHz), "
          f"8 Hz-20 kHz {r['rms_8hz_20khz_mv']:.1f} mVrms, hump {r['lf_hump_hz']:.0f} Hz {r['lf_hump_db']:+.1f} dB")

(OUT / "metrics" / "noise28.json").write_text(json.dumps({"operating_points": rows}, indent=1) + "\n")


def md(head, rr):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    return "\n".join(out + ["| " + " | ".join(x) + " |" for x in rr])


w = lambda v: "–" if v == 0 else f"{v:g} W"  # noqa: E731
t1 = md(["Load target", "Into 11 Ω", "VBUS, board ADC", "Error, ADC", "Supply, board ADC", "Buck VIN (QT Py)",
         "Load duty", "Source on-time"],
        [[w(r["load_target_w"]), (f"{r['load_p_11ohm_w']:.2f} W" if r["load_target_w"] else "–"),
          f"{r['vbus_adc_v']:.3f} V", f"{r['setpoint_err_pct_adc']:+.2f} %", f"{r['vin_supply_adc_v']:.3f} V",
          (f"{r['buck_vin_qtpy_v']:.3f} V" if r["load_target_w"] else "–"),
          (f"{r['load_duty_pct']:.1f} %" if r["load_target_w"] else "–"), f"{r['on_time_ns']:.0f} ns"] for r in rows])
t3 = md(["Load", "Vpp 20 MHz", "Vrms 20 MHz", "99.9 % range 20 MHz", "200 kHz ripple p-p", "Vrms full BW",
         "8 Hz–20 kHz rms", "8 Hz–20 kHz p-p"],
        [[w(r["load_target_w"]), f"{r['vpp_bw20_mv']:.0f}", f"{r['vrms_bw20_mv']:.1f}", f"{r['vpp999_bw20_mv']:.0f}",
          f"{r['ripple_sync_pp_mv']:.0f}", f"{r['vrms_full_mv']:.1f}", f"{r['rms_8hz_20khz_mv']:.1f}",
          f"{r['pp_8hz_20khz_mv']:.0f}"] for r in rows])
bands = [("band_10_1000_mv", "10 Hz–1 kHz"), ("band_1000_10000_mv", "1–10 kHz"), ("band_10000_100000_mv", "10–100 kHz"),
         ("band_100000_1000000_mv", "100 kHz–1 MHz"), ("band_1000000_20000000_mv", "1–20 MHz"),
         ("band_20000000_62500000_mv", "20–62.5 MHz")]
t4 = md(["Load"] + [b for _, b in bands], [[w(r["load_target_w"])] + [f"{r[k]:.2f}" for k, _ in bands] for r in rows])
t4b = md(["Load", "Floor 20–60 Hz, µV/√Hz", "Hump peak", "Hump level, µV/√Hz", "Hump over floor"],
         [[w(r["load_target_w"]), f"{r['lf_floor_20_60hz_uv_rthz']:.0f}", f"{r['lf_hump_hz']:.0f} Hz",
           f"{r['lf_hump_uv_rthz']:.0f}", f"{r['lf_hump_db']:+.1f} dB"] for r in rows])
fl = lambda t: f"{t['f_hz'] / 1e3:.1f} kHz" if t["f_hz"] >= 1e4 else f"{t['f_hz']:.0f} Hz"  # noqa: E731
t5 = md(["Load", "Largest lines below 10 kHz, mV rms", "Largest lines 10 kHz–20 MHz, mV rms"],
        [[w(r["load_target_w"]),
          ", ".join(f"{fl(t)}: {t['mv_rms']:.1f} ({t['dbuv']:.0f} dBµV)" for t in sorted(r["tones_lf"], key=lambda t: -t["mv_rms"])[:3]),
          ", ".join(f"{fl(t)}{' (' + t['id'] + ')' if t['id'] else ''}: {t['mv_rms']:.1f} ({t['dbuv']:.0f} dBµV)"
                    for t in sorted(r["tones_hf"], key=lambda t: -t["mv_rms"])[:3])] for r in rows])
(OUT / "metrics" / "noise28_tables.md").write_text("\n\n".join([t1, t3, t4, t4b, t5]) + "\n")
print((OUT / "metrics" / "noise28_tables.md").read_text())

fig, ax = plt.subplots(figsize=(10, 5))
for (lab, _, p_mw), col in zip(OPS, ("k", "#4c72b0", "#dd8452", "#c44e52")):
    f_lf, p_lf, f_hf, p_hf = spectra[lab]
    mlf = (f_lf >= 10) & (f_lf < F_SPLIT)
    mhf = (f_hf >= F_SPLIT) & (f_hf < 20e6)
    ax.loglog(np.concatenate([f_lf[mlf], f_hf[mhf]]), np.sqrt(np.concatenate([p_lf[mlf], p_hf[mhf]])), lw=0.8,
              color=col, label=f"{p_mw / 1000:g} W" if p_mw else "no load")
ax.set_xlabel("frequency [Hz]  (10 Hz–10 kHz: 600 ms record at 2 MSa/s; 10 kHz–20 MHz: 9.6 ms at 125 MSa/s; 20 MHz limit)")
ax.set_ylabel("V/√Hz")
ax.set_title("VBUS at TP2, 28 V EPR (boost), AC-coupled", loc="left")
ax.grid(alpha=.3, which="both")
ax.legend()
fig.tight_layout()
fig.savefig(OUT / "figures" / "v28_spectra.png", dpi=90)
