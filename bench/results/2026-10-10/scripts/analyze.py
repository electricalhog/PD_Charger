"""Regulation, noise and transient metrics for the 2026-10-10 VBUS captures.

Inputs: cap/<label>.json + cap/<label>_CH4.bin (grab.py / trans2.py), cap/<label>_op.json (op.py).
Outputs (OUT): metrics/metrics.json, figures/fig_*.png.
Volts = (code - yorigin - yreference) * yincrement, from each capture's own preamble.
"""
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

HERE = Path(__file__).resolve().parent
CAP = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "data" / "cap"
OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE.parent
(OUT / "figures").mkdir(parents=True, exist_ok=True)
(OUT / "metrics").mkdir(parents=True, exist_ok=True)

OPS = [("v05_p0", 5000, 0), ("v09_p0", 9000, 0),
       ("v15_p0", 15000, 0), ("v15_p500", 15000, 500), ("v15_p2000", 15000, 2000), ("v15_p3000", 15000, 3000),
       ("v20_p0", 20000, 0), ("v20_p500", 20000, 500), ("v20_p2000", 20000, 2000), ("v20_p3000", 20000, 3000)]
# The 2 MSa/s record's floor is aliased above ~30 kHz (HRES decimation is not
# a complete anti-alias filter): its median ASD reads 2.6-9.9x the 125 MSa/s
# record's at 30-90 kHz, against 0.93-1.16x at 10-30 kHz (1.9x at 9 V).  The
# 9.6 ms record's floor reads up to 2.6x high at 3-10 kHz, where the 2-3.3 kHz
# lines leak through its 1 ms segments.  So: 600 ms record below 10 kHz,
# 9.6 ms record from 10 kHz to 20 MHz, full-bandwidth record above.
F_SPLIT = 10e3
BANDS_LF = [(10, 1e3), (1e3, 10e3)]                           # from the 600 ms, 2 MSa/s record
BANDS_HF = [(10e3, 100e3), (100e3, 1e6), (1e6, 20e6)]         # from the 20 MHz-limited record
BANDS_HFW = [(20e6, 62.5e6)]                                  # from the full-bandwidth record
F_SW_SOURCE, F_SW_LOAD = 200e3, 438.6e3                       # power board HRTIM, buck load PWM


def load(lab):
    m = json.loads((CAP / f"{lab}.json").read_text())
    p = m["waveforms"]["CH4"]["preamble"]
    c = np.fromfile(CAP / f"{lab}_CH4.bin", dtype=np.uint8)
    v = (c.astype(np.float64) - p["yorig"] - p["yref"]) * p["yinc"]
    return v, 1.0 / p["xinc"], m


def psd(ac, fs, nperseg):
    # detrend=False to match MATLAB pwelch (the caller removes the mean)
    f, pxx = signal.welch(ac, fs=fs, window="hann", nperseg=nperseg, noverlap=nperseg // 2,
                          detrend=False, scaling="density")
    return f, pxx


def band_rms(f, pxx, lo, hi):
    m = (f >= lo) & (f < hi)
    return float(np.sqrt(np.trapezoid(pxx[m], f[m]))) if m.sum() > 1 else float("nan")


def tones(ac, fs, fmin, fmax, n=8, min_db_over_floor=12.0):
    """Flat-top amplitude spectrum (V rms per line); the n largest local maxima in [fmin, fmax)."""
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
    """p-p of the fsw-synchronous ripple: low-pass at f_lp, then the cycle-by-cycle average over
    1/blocks of the record (short enough that the ~25 ppm clock offset does not smear it); mean of the blocks."""
    n = int(round(fs / fsw))
    assert abs(fs / fsw - n) < 1e-6, "record rate must be a multiple of fsw"
    x = signal.sosfiltfilt(signal.butter(4, f_lp, "lp", fs=fs, output="sos"), ac)
    q = len(x) // n // blocks
    return float(np.mean([np.ptp(x[b * q * n:(b + 1) * q * n].reshape(q, n).mean(axis=0)) for b in range(blocks)]))


def lf_hump(f, pxx, lo=100.0, hi=8e3, ref=(20.0, 60.0)):
    """Broadband hump of the low-frequency noise: the ASD smoothed with a sliding 1/3-octave median
    (robust to discrete lines), its peak in [lo, hi), and the peak's height over the 20-60 Hz median."""
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
    r = {"label": lab, "contract_v": vnom / 1000, "load_target_w": p_mw / 1000}
    # DC and regulation
    r["vbus_scope_v"] = float(lf.mean())
    r["vbus_adc_v"] = float(np.mean([x["v_out_mv"] for x in reg])) / 1000
    r["vin_supply_adc_v"] = float(np.mean([x["v_in_mv"] for x in reg])) / 1000
    r["on_time_ns"] = float(np.mean([x["on_ns"] for x in reg]))
    r["setpoint_err_pct_scope"] = 100 * (r["vbus_scope_v"] - vnom / 1000) / (vnom / 1000)
    r["setpoint_err_pct_adc"] = 100 * (r["vbus_adc_v"] - vnom / 1000) / (vnom / 1000)
    ld = op.get("load", {})
    st = (ld.get("steps") or [{}])[0]
    r["buck_vin_qtpy_v"] = st.get("vin_mv", {}).get("mean", float("nan")) / 1000 if p_mw else float("nan")
    r["load_vout_v"] = st.get("vout_mv", {}).get("mean", float("nan")) / 1000 if p_mw else float("nan")
    r["load_p_10ohm_w"] = st.get("pout_mw", {}).get("mean", float("nan")) / 1000 if p_mw else 0.0
    r["load_p_11ohm_w"] = r["load_p_10ohm_w"] * 10 / 11
    r["load_duty_pct"] = st.get("duty_pm", {}).get("mean", float("nan")) / 10 if p_mw else float("nan")
    # time-domain noise
    for tag, v in (("bw20", hf), ("full", hfw)):
        ac = v - v.mean()
        r[f"vpp_{tag}_mv"] = 1e3 * float(np.ptp(ac))
        r[f"vrms_{tag}_mv"] = 1e3 * float(ac.std())
        r[f"vpp999_{tag}_mv"] = 1e3 * float(np.percentile(ac, 99.95) - np.percentile(ac, 0.05))
    r["ripple_sync_pp_mv"] = 1e3 * sync_ripple_pp(hf - hf.mean(), fs_hf)
    aclf = lf - lf.mean()
    r["vpp_lf_mv"] = 1e3 * float(np.ptp(aclf))
    r["vrms_lf_mv"] = 1e3 * float(aclf.std())
    # 8 Hz high-pass (4th order, zero phase) so the slow wander, reported on
    # its own below (20 ms moving mean), does not leak into the 10 Hz bins.
    hp = signal.sosfiltfilt(signal.butter(4, 8.0, "hp", fs=fs_lf, output="sos"), aclf)
    bp = signal.sosfiltfilt(signal.butter(4, 20e3, "lp", fs=fs_lf, output="sos"), hp)
    e = int(0.05 * fs_lf)                                  # drop 50 ms filter edges
    r["pp_8hz_20khz_mv"] = 1e3 * float(np.ptp(bp[e:-e]))
    r["rms_8hz_20khz_mv"] = 1e3 * float(bp[e:-e].std())
    # spectra
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
    r["rbw_lf_hz"] = float(1.5 * fs_lf / 2 ** 18)      # Hann ENBW = 1.5 bins
    r["rbw_hf_hz"] = float(1.5 * fs_hf / 2 ** 17)
    t_lf, res_lf = tones(hp[e:-e] - hp[e:-e].mean(), fs_lf, 10, F_SPLIT)
    t_hf, res_hf = tones(hf - hf.mean(), fs_hf, F_SPLIT, 20e6)
    # very low frequency: the swing of the 20 ms mean over the 600 ms record
    k20 = int(20e-3 * fs_lf)
    cs = np.concatenate([[0.0], np.cumsum(lf)])
    ma20 = (cs[k20:] - cs[:-k20]) / k20                   # 20 ms moving mean, 'valid' part
    r["vpp_ma20ms_mv"] = 1e3 * float(np.ptp(ma20))
    for t in t_lf + t_hf:
        t["id"] = name_line(t["f_hz"])
    r["tones_lf"], r["tones_hf"] = t_lf, t_hf
    r["tone_bin_lf_hz"], r["tone_bin_hf_hz"] = res_lf, res_hf
    r["clipped"] = {k: m["waveforms"]["CH4"]["clipped"] for k, m in (("bw20", m_hf), ("full", m_hfw), ("lf", m_lf))}
    r["scope"] = {k: {"fs": m["settings"]["sample_rate_sa_s"], "points": m["waveforms"]["CH4"]["points"],
                      "v_div": m["settings"]["channels"]["CH4"]["scale_v_div"],
                      "offset_v": m["settings"]["channels"]["CH4"]["offset_v"],
                      "bwl": m["settings"]["channels"]["CH4"]["bw_limit"], "acq": m["settings"]["acquire_type"]}
                  for k, m in (("bw20", m_hf), ("full", m_hfw), ("lf", m_lf))}
    rows.append(r)
    spectra[lab] = (f_lf, p_lf, f_hf, p_hf, f_hw, p_hw)
    print(f"{lab}: {r['vbus_scope_v']:.4f} V scope, {r['vbus_adc_v']:.4f} V adc, "
          f"{r['vpp_bw20_mv']:.0f} mVpp / {r['vrms_bw20_mv']:.1f} mVrms (20 MHz), "
          f"8 Hz-20 kHz {r['rms_8hz_20khz_mv']:.1f} mVrms, <8 Hz swing {r['vpp_ma20ms_mv']:.0f} mVpp, "
          f"top LF tone {max(t_lf, key=lambda t: t['mv_rms']) if t_lf else None}")

# load regulation (scope DC, same settings family; ADC as the second reading)
reg_tab = {}
for v in (15000, 20000):
    rr = {r["load_target_w"]: r for r in rows if r["contract_v"] == v / 1000}
    v0, v3 = rr[0.0]["vbus_scope_v"], rr[3.0]["vbus_scope_v"]
    a0, a3 = rr[0.0]["vbus_adc_v"], rr[3.0]["vbus_adc_v"]
    reg_tab[v] = {"scope_noload_v": v0, "scope_3w_v": v3, "load_reg_pct_scope": 100 * (v0 - v3) / v3,
                  "adc_noload_v": a0, "adc_3w_v": a3, "load_reg_pct_adc": 100 * (a0 - a3) / a3}


# ---------------------------------------------------------------- transients
# u15/u20: 6 s singles at 2 MSa/s (12M points, HRES) across a 0 -> 3 W run.
# (The earlier 100 kSa/s records alias the 200 kHz ripple to a ~5 Hz beat:
# 200 kHz is twice the sample rate and the two clocks differ by ~25 ppm.)
def movmean(x, k):
    cs = np.concatenate([[0.0], np.cumsum(x)])
    y = np.full(len(x), np.nan)
    y[k // 2:k // 2 + len(x) - k + 1] = (cs[k:] - cs[:-k]) / k
    return y


def transient(lab):
    v, fs, m = load(lab)
    pre = m["waveforms"]["CH4"]["preamble"]
    t = pre["xorig"] + np.arange(len(v)) * pre["xinc"]           # s, 0 = forced trigger
    ld = m["load"]
    frames = json.loads((CAP / Path(ld["file"]).name).read_text())   # bu load run TEL frames
    arm = ld["t0_unix"] - m["t_force_unix"]
    ft = np.array([fr["t_s"] for fr in frames]) + arm
    fp = np.array([fr["pout_mw"] for fr in frames]) / 1000
    run = np.array([fr["state"] == "running" for fr in frames])
    lp20k = signal.sosfiltfilt(signal.butter(4, 20e3, "lp", fs=fs, output="sos"), v)
    lp1k = signal.sosfiltfilt(signal.butter(4, 1e3, "lp", fs=fs, output="sos"), v)
    ma20 = movmean(v, int(20e-3 * fs))
    # release: the steepest 1 kHz-low-passed rise within 150 ms of the last running frame
    t_last = ft[run][-1]
    w = (t > t_last - 0.05) & (t < t_last + 0.15)
    d = np.gradient(lp1k[w], t[w])
    t_rel = float(t[w][np.argmax(d)])
    seg_before = (t > t.min() + 0.05) & (t < arm - 0.02)
    seg_hold = (t > arm + 1.6) & (t < t_rel - 0.05)
    seg_after = (t > t_rel + 0.3) & (t < t.max() - 0.05)
    before, hold, after = (float(np.nanmean(ma20[s_])) for s_ in (seg_before, seg_hold, seg_after))
    band = {k: 1e3 * float(np.ptp(lp1k[s_])) for k, s_ in (("before", seg_before), ("hold", seg_hold), ("after", seg_after))}
    def recover(sig, t0, lo, hi, hold_s=0.010, search_s=0.3):
        """Time from t0 until sig re-enters [lo, hi] and stays there hold_s; NaN if not within search_s."""
        w = (t >= t0) & (t <= t0 + search_s)
        inside = (sig[w] >= lo) & (sig[w] <= hi)
        need = int(hold_s * fs)
        run_ = 0
        for i, ok in enumerate(inside):
            run_ = run_ + 1 if ok else 0
            if run_ >= need:
                return 1e3 * float(t[w][i - need + 1] - t0)
        return float("nan")
    # steady bands of the 20 kHz and 1 kHz low-passed signal (0.1 %..99.9 % of the segment)
    def band_of(sig, seg):
        return float(np.percentile(sig[seg], 0.1)), float(np.percentile(sig[seg], 99.9))
    b20_before, b20_after = band_of(lp20k, seg_before), band_of(lp20k, seg_after)
    b1_before, b1_after = band_of(lp1k, seg_before), band_of(lp1k, seg_after)
    # arming: the deepest 20 kHz-low-passed point in the first 0.4 s after the run starts
    wa = (t > arm) & (t < arm + 0.4)
    i_arm = int(np.argmin(lp20k[wa]))
    t_dip = float(t[wa][i_arm])
    dip20 = 1e3 * float(lp20k[wa][i_arm] - before)
    dip1 = 1e3 * float(np.min(lp1k[wa]) - before)
    rec_dip = recover(lp20k, t_dip, b20_before[0], b20_before[1] + 0.05)
    # ringing after the arming dip: extrema (prominence >= 15 mV) of a 2 kHz low-pass over the
    # next 12 ms, merged to alternate in sign about the no-load level; the period is first trough
    # to next trough, the decay their ratio, and zeta = ln(decay) / sqrt(4 pi^2 + ln(decay)^2).
    lp2k = signal.sosfiltfilt(signal.butter(4, 2e3, "lp", fs=fs, output="sos"), v)
    wg = (t >= t_dip - 0.001) & (t <= t_dip + 0.012)
    yy, tt = 1e3 * (lp2k[wg] - before), 1e3 * (t[wg] - t_dip)
    idx = np.concatenate([signal.find_peaks(yy, prominence=15)[0], signal.find_peaks(-yy, prominence=15)[0]])
    ext = []
    for i in sorted(idx):
        if ext and np.sign(yy[i]) == np.sign(ext[-1][1]):
            if abs(yy[i]) > abs(ext[-1][1]):
                ext[-1] = (float(tt[i]), float(yy[i]))
        else:
            ext.append((float(tt[i]), float(yy[i])))
    k0 = next(k for k, e in enumerate(ext) if e[1] < 0 and e[0] > -0.5)
    ring = ext[k0:]
    ring_hz = 1e3 / (ring[2][0] - ring[0][0]) if len(ring) >= 3 else float("nan")
    decay = ring[0][1] / ring[2][1] if len(ring) >= 3 else float("nan")
    zeta = np.log(decay) / np.sqrt(4 * np.pi ** 2 + np.log(decay) ** 2) if decay == decay else float("nan")
    # release: the highest 20 kHz-low-passed point within 50 ms of the release step
    wr = (t >= t_rel - 0.002) & (t <= t_rel + 0.05)
    i_rel = int(np.argmax(lp20k[wr]))
    t_peak = float(t[wr][i_rel])
    peak20 = 1e3 * float(lp20k[wr][i_rel] - after)
    peak20_hold = 1e3 * float(lp20k[wr][i_rel] - hold)
    peak1 = 1e3 * float(np.max(lp1k[wr]) - after)
    rec_rel = recover(lp20k, t_peak, b20_after[0], b20_after[1])
    return {"label": lab, "fs": fs, "t": t, "v": v, "lp20k": lp20k, "lp1k": lp1k, "ma20": ma20, "ft": ft, "fp": fp,
            "arm_s": arm, "release_s": t_rel, "before_v": before, "hold_v": hold, "after_v": after,
            "dc_shift_hold_mv": 1e3 * (hold - before), "lf_band_1khz_mv": band,
            "band20k_before_mv": [1e3 * (x - before) for x in b20_before],
            "band20k_after_mv": [1e3 * (x - after) for x in b20_after],
            "arming_t_s": t_dip, "arming_dip_20k_mv": dip20, "arming_dip_1k_mv": dip1, "arming_recover_ms": rec_dip,
            "arming_ring_extrema_ms_mv": ring, "arming_ring_hz": ring_hz, "arming_ring_decay_per_cycle": decay,
            "arming_ring_zeta": float(zeta),
            "release_peak_t_s": t_peak, "release_peak_20k_mv": peak20, "release_peak_20k_vs_hold_mv": peak20_hold,
            "release_peak_1k_mv": peak1, "release_recover_ms": rec_rel}


trs = {v: transient(f"u{v}_p3000") for v in (15, 20)}

# ---------------------------------------------------------------- figures
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.25, "figure.dpi": 150,
                     "axes.spines.top": False, "axes.spines.right": False})
cols = {0: "#4a4a4a", 500: "#2f6fd6", 2000: "#d98200", 3000: "#c2399b"}
fig, axs = plt.subplots(2, 2, figsize=(14, 9), sharey=True)
for ax, v in zip(axs.flat, (5000, 9000, 15000, 20000)):
    for lab, vnom, p_mw in OPS:
        if vnom != v:
            continue
        f_lf, p_lf, f_hf, p_hf, f_hw, p_hw = spectra[lab]
        mlf = (f_lf >= 10) & (f_lf < F_SPLIT)
        mhf = (f_hf >= F_SPLIT) & (f_hf < 20e6)
        f = np.concatenate([f_lf[mlf], f_hf[mhf]])
        a = np.sqrt(np.concatenate([p_lf[mlf], p_hf[mhf]]))
        ax.loglog(f, a, lw=0.8, color=cols[p_mw], label=f"{p_mw / 1000:g} W" if v >= 15000 else "no load")
    ax.axvline(F_SPLIT, color="#999999", lw=0.8, ls=":")
    ax.set_title(f"VBUS {v / 1000:g} V contract: noise spectral density at TP2", loc="left")
    ax.set_xlim(10, 20e6)
    ax.set_xlabel("frequency [Hz]  (10 Hz–10 kHz: 600 ms record at 2 MSa/s; 10 kHz–20 MHz: 9.6 ms at 125 MSa/s; 20 MHz limit)")
    ax.set_ylabel("V/√Hz")
    ax.legend(fontsize=9, loc="lower left")
fig.tight_layout()
fig.savefig(OUT / "figures" / "fig_spectra.png")
plt.close(fig)

fig, axs = plt.subplots(2, 3, figsize=(16, 8), gridspec_kw={"width_ratios": [2.2, 1, 1]})
for row, (v, tr) in zip(axs, trs.items()):
    t = tr["t"]
    dec = slice(None, None, 200)                                   # 10 kSa/s for drawing
    for ax, (lo, hi), ttl in zip(row, ((t.min(), t.max()), (tr["arming_t_s"] - 0.02, tr["arming_t_s"] + 0.06),
                                       (tr["release_s"] - 0.01, tr["release_s"] + 0.05)),
                                 ("whole run", "load arming", "release")):
        m = (t >= lo) & (t <= hi)
        step = 200 if ttl == "whole run" else 4
        ax.plot(t[m][::step], 1e3 * (tr["lp20k"][m][::step] - tr["before_v"]), color="#2f6fd6", lw=0.5, alpha=0.5,
                label="VBUS, 20 kHz low-pass")
        ax.plot(t[m][::step], 1e3 * (tr["lp1k"][m][::step] - tr["before_v"]), color="#14213d", lw=1.3,
                label="VBUS, 1 kHz low-pass")
        ax.set_ylim(-400, 400) if ttl != "whole run" else ax.set_ylim(-200, 250)
        ax.set_xlim(lo, hi)
        ax.set_title(f"{v} V contract, {ttl}", loc="left")
        ax.set_xlabel("time [s]")
        if ttl != "whole run":
            ax.plot(t[m][::step], 1e3 * (tr["v"][m][::step] - tr["before_v"]), color="#c2399b", lw=0.3, alpha=0.35,
                    label="VBUS raw (20 MHz limit, HRES)")
            ax.legend(fontsize=7, loc="upper right")
        if ttl == "whole run":
            ax.set_ylabel(f"VBUS − {tr['before_v']:.3f} V [mV]")
            ax2 = ax.twinx()
            ax2.step(tr["ft"], tr["fp"], where="post", color="#d98200", lw=1.4, label="load power (QT Py)")
            ax2.set_ylabel("load power [W]")
            ax2.set_ylim(-0.2, 4.0)
            ax2.grid(False)
            h1, l1 = ax.get_legend_handles_labels()
            h2, l2 = ax2.get_legend_handles_labels()
            ax.legend(h1 + h2, l1 + l2, fontsize=8, loc="upper right")
        if ttl == "release":
            ax.axvline(tr["release_s"], color="#d98200", lw=1, ls="--")
fig.tight_layout()
fig.savefig(OUT / "figures" / "fig_transients.png")
plt.close(fig)

fig, axs = plt.subplots(2, 2, figsize=(14, 7.5))
for ax, lab, span in ((axs[0, 0], "v15_p0", 50e-6), (axs[0, 1], "v15_p3000", 50e-6),
                      (axs[1, 0], "v20_p3000", 50e-6), (axs[1, 1], "v20_p3000_lf", 20e-3)):
    lab_hf = lab if lab.endswith("_lf") else f"{lab}_hf"
    v, fs, m = load(lab_hf)
    n = int(span * fs)
    tt = np.arange(n) / fs * (1e6 if span < 1e-3 else 1e3)
    ax.plot(tt, 1e3 * (v[:n] - v.mean()), lw=0.6, color="#2f6fd6")
    ax.set_xlabel("time [µs]" if span < 1e-3 else "time [ms]")
    ax.set_ylabel("VBUS AC [mV]")
    what = "20 MHz limit, 125 MSa/s" if not lab.endswith("_lf") else "600 ms record, 2 MSa/s HRES"
    ax.set_title(f"{lab.replace('_lf', '')}: {what}", loc="left")
fig.tight_layout()
fig.savefig(OUT / "figures" / "fig_time.png")
plt.close(fig)

clean = [{k: v for k, v in r.items()} for r in rows]
tr_out = {v: {k: tr[k] for k in ("before_v", "hold_v", "after_v", "dc_shift_hold_mv", "lf_band_1khz_mv",
                                  "band20k_before_mv", "band20k_after_mv", "arming_t_s", "arming_dip_20k_mv",
                                  "arming_dip_1k_mv", "arming_recover_ms", "arming_ring_extrema_ms_mv", "arming_ring_hz",
                                  "arming_ring_decay_per_cycle", "arming_ring_zeta", "release_peak_t_s", "release_peak_20k_mv",
                                  "release_peak_20k_vs_hold_mv", "release_peak_1k_mv", "release_recover_ms",
                                  "arm_s", "release_s", "fs")}
          for v, tr in trs.items()}
(OUT / "metrics" / "metrics.json").write_text(json.dumps({"operating_points": clean, "load_regulation": reg_tab,
                                              "transients": tr_out}, indent=1, default=float))
print(json.dumps({"load_regulation": reg_tab, "transients": tr_out}, indent=1, default=float))
