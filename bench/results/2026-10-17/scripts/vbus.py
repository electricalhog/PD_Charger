#!/usr/bin/env python3
"""Analysis of the scope records in data/cap/ (2026-10-17 README steps 2, 3, 4 and 6).

  vbus.py        writes metrics/vbus.json

Every record gets a validity block: distinct ADC codes, quantum, window, samples at the lowest and highest code, and for
the 8 us records the lag-5 autocorrelation (the 200 kHz switching period is 5 us, so a periodic sampling pattern repeats
every 5 samples).  The two 2.4 s AC-coupled records are clipped at both ends of the 8-bit window and point-sampled at 8 us;
the 100 us averages the first analysis took from them contain an aliased ripple residual.  "alias_test" decimates the dense
12 ms record to 8 us and reproduces those statistics; the first-analysis numbers are kept under "first_analysis_retracted"
only so that they can be regenerated.  Node quantities come from "dense_12ms", the ramp, reset_from5v and connect5 records,
and from the 20 kHz ADC traces (pulse_metrics.py blocks excursions and spectra).
"""
import json
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
CAP = HERE / "data" / "cap"
R = round
PRE_END = -0.3e-3  # reset record: the 20 us mean starts falling ~0.2 ms before the 4.6 V trigger; analyse the steady part only


def load(name):
    d = np.loadtxt(CAP / name, delimiter=",", skiprows=1)
    return d[:, 0], d[:, 1], d[1, 0] - d[0, 0]


def box(x, n):
    c = np.cumsum(np.insert(x, 0, 0.0))
    return (c[n:] - c[:-n]) / n


def validity(name, lag=None, block_ms=None):
    t, v, dt = load(name)
    codes = np.unique(v)
    lo, hi = float(v.min()), float(v.max())
    out = {"samples": len(v), "sample_us": R(dt * 1e6, 3), "span_ms": R(float((t[-1] - t[0]) * 1e3), 2),
           "codes_used": len(codes), "quantum_mv": R(float(np.median(np.diff(codes))) * 1e3, 2),
           "lowest_v": R(lo, 4), "highest_v": R(hi, 4), "window_v": R(hi - lo, 3),
           "at_lowest_code": int((v == lo).sum()), "at_highest_code": int((v == hi).sum()),
           "at_lowest_pct": R(100.0 * float((v == lo).mean()), 2), "at_highest_pct": R(100.0 * float((v == hi).mean()), 2)}
    if lag:
        x = v - v.mean()
        out[f"autocorr_lag{lag}"] = R(float(np.mean(x[:-lag] * x[lag:]) / x.var()), 3)
    if block_ms:
        n = int(round(block_ms * 1e-3 / dt))
        bm = v[:len(v) // n * n].reshape(-1, n).mean(1)
        out["block_means_mv"] = {"block_ms": block_ms, "blocks": len(bm), "aligned_to": "record start",
                                 "min": R(float(bm.min()) * 1e3, 1), "max": R(float(bm.max()) * 1e3, 1),
                                 "record_mean": R(float(v.mean()) * 1e3, 1)}
    return out


def first_analysis(name):
    t, v, dt = load(name)
    v = v - v.mean()
    w = np.hanning(len(v))
    sp = np.abs(np.fft.rfft(v * w)) * 2 / w.sum()
    f = np.fft.rfftfreq(len(v), dt)
    bands = {}
    for lo, hi in ((1, 20), (20, 100), (100, 500), (500, 2000), (2000, 3500), (3500, 5000), (5000, 10000)):
        m = (f >= lo) & (f < hi)
        k = np.argmax(sp[m])
        bands[f"{lo}-{hi} Hz"] = {"top_hz": R(float(f[m][k]), 1), "top_mv_pk": R(float(sp[m][k]) * 1e3, 1),
                                 "band_mv_rms": R(float(np.sqrt(np.sum((sp[m] / np.sqrt(2)) ** 2))) * 1e3, 1)}
    n = int(round(100e-6 / dt))
    lp = np.convolve(v, np.ones(n) / n, mode="same")
    idx = np.where((lp[1:-1] < lp[:-2]) & (lp[1:-1] <= lp[2:]) & (lp[1:-1] < -0.25))[0] + 1
    ev = []
    for i in idx:
        if not ev or t[i] - t[ev[-1]] > 1e-3:
            ev.append(i)
    iv = np.diff(t[ev]) * 1e3
    return {"average_samples": n, "rms_average_mv": R(float(lp.std()) * 1e3), "pp_average_v": R(float(np.ptp(lp)), 2),
            "minima_below_0p25v": len(ev), "interval_ms_p10_p50_p90": [R(float(x), 2) for x in np.percentile(iv, [10, 50, 90])],
            "depth_v_median": R(float(np.median(lp[ev])), 2), "bands": bands,
            "white_noise_prediction_mv": R(float(v.std()) / np.sqrt(n) * 1e3), "raw_sigma_v": R(float(v.std()), 3)}


def dense_12ms(name="vbus28_ac_12ms.csv"):
    t, v, dt = load(name)
    v = v - v.mean()
    box_stats = {}
    for label, us in (("5us", 5), ("100us", 100), ("500us", 500), ("1ms", 1000)):
        m = box(v, int(round(us * 1e-6 / dt)))
        box_stats[label] = {"rms_mv": R(float(m.std()) * 1e3), "pp_v": R(float(np.ptp(m)), 3)}
    n5 = int(round(5e-6 / dt))
    m5 = box(v, n5)
    tc5 = t[:len(m5)] + n5 * dt / 2
    med = float(np.median(m5))
    idx = np.where(m5 < med - 0.25)[0]
    gap = int(round(1e-3 / dt))
    runs = []
    if len(idx):
        s = p = idx[0]
        for i in idx[1:]:
            if i - p > gap:
                runs.append((s, p))
                s = i
            p = i
        runs.append((s, p))
    events = [{"t_start_ms": R(float(tc5[s]) * 1e3, 3), "t_end_ms": R(float(tc5[e]) * 1e3, 3),
               "duration_us": R(float(tc5[e] - tc5[s]) * 1e6), "min_v_vs_record_mean": R(float(m5[s:e + 1].min()), 3)}
              for s, e in runs]
    n8 = int(round(8e-6 / dt))
    res = []
    for ph in range(0, n8, 5):
        x = v[ph::n8]
        ma = np.convolve(x, np.ones(12) / 12, mode="valid")
        ma = ma - ma.mean()
        mn = np.where((ma[1:-1] < ma[:-2]) & (ma[1:-1] <= ma[2:]) & (ma[1:-1] < -0.25))[0] + 1
        last, c = -9999, 0
        for i in mn:
            if i - last > 125:
                c += 1
                last = i
        res.append((float(ma.std()), float(np.ptp(ma)), c))
    res = np.array(res)
    lo_code, hi_code = v.min(), v.max()
    nb = len(v) // n5
    blk = v[:nb * n5].reshape(nb, n5)
    f_lo = (blk <= lo_code + 1e-9).mean(1)
    clip = {"blocks_5us": nb,
            "r_block_mean_vs_fraction_at_lowest_code": R(float(np.corrcoef(blk.mean(1), f_lo)[0, 1]), 2),
            "pct_at_lowest_code_in_blocks_p5_p50_p95_max": [R(float(x) * 100, 1) for x in (*np.percentile(f_lo, [5, 50, 95]), f_lo.max())],
            "pct_at_lowest_code_in_events": [R(float((v[s:e + n5] <= lo_code + 1e-9).mean()) * 100, 1) for s, e in runs]}
    return {"span_ms": R(float((t[-1] - t[0]) * 1e3), 2), "boxcar": box_stats, "clip_check": clip,
            "five_us_mean": {"median_v": R(med, 3), "rms_mv": R(float(m5.std()) * 1e3), "pp_v": R(float(np.ptp(m5)), 3),
                             "threshold": "below median - 0.25 V", "events": events},
            "alias_test": {"phases": len(res), "decimated_to_us": 8, "average_samples": 12,
                           "rms_mv_mean_min_max": [R(float(res[:, 0].mean()) * 1e3), R(float(res[:, 0].min()) * 1e3),
                                                   R(float(res[:, 0].max()) * 1e3)],
                           "pp_v_mean": R(float(res[:, 1].mean()), 2),
                           "minima_below_0p25v_per_s_mean": R(float(res[:, 2].mean()) / (len(v) * dt)),
                           "true_boxcar_12_samples_rms_mv": R(float(box(v, int(round(96e-6 / dt))).std()) * 1e3)}}


def ramp(name="vbus_20to28_ramp.csv"):
    t, v, dt = load(name)
    n = int(round(1e-3 / dt))
    lp = np.convolve(v, np.ones(n) / n, mode="same")[n:-n]
    tt = t[n:-n]
    v1 = float(np.median(lp[tt > tt[-1] - 0.03]))

    def fit(lo, hi):
        m = (lp > lo) & (lp < hi)
        p = np.polyfit(tt[m], lp[m], 1)
        return p, float((lp[m] - np.polyval(p, tt[m])).std())

    windows = []
    for lo, hi in ((22, v1 - 1), (21, 27), (22, 27), (23, 26), (21, 27.5), (20.5, 27.5), (22, 26), (24, 27)):
        p, sd = fit(lo, hi)
        windows.append({"from_v": R(lo, 2), "to_v": R(hi, 2), "slope_v_per_s": R(float(p[0]), 1), "residual_rms_mv": R(sd * 1e3)})
    p, sd = fit(22, v1 - 1)
    t_end = (v1 - p[1]) / p[0]
    after = lp[tt > t_end]
    k205, k275 = np.where(lp >= 20.5)[0][0], np.where(lp >= 27.5)[0][0]
    last = tt > tt[-1] - 0.08
    return {"sample_us": R(dt * 1e6, 3), "end_v": R(v1, 2), "slope_v_per_s": R(float(p[0]), 1), "residual_rms_mv": R(sd * 1e3),
            "windows": windows, "slope_range_v_per_s": [min(w["slope_v_per_s"] for w in windows), max(w["slope_v_per_s"] for w in windows)],
            "t_20p5_to_27p5_ms": R(float(tt[k275] - tt[k205]) * 1e3, 1),
            "rate_20p5_to_27p5_v_per_s": R(7.0 / float(tt[k275] - tt[k205]), 1),
            "after_line_end_1ms_average_minus_end_v": [R(float((after - v1).min()), 3), R(float((after - v1).max()), 3)],
            "last_80ms_1ms_average": {"sd_mv": R(float(lp[last].std()) * 1e3), "pp_mv": R(float(np.ptp(lp[last])) * 1e3)}}


def reset_from5v(name="reset_from5v_raw.csv"):
    t, v, dt = load(name)
    pre = t < PRE_END
    n2, n50, n20 = (int(round(x * 1e-6 / dt)) for x in (2, 50, 20))
    l2, l50, l20 = box(v, n2), box(v, n50), box(v, n20)
    t2, t50, t20 = t[:len(l2)] + n2 * dt / 2, t[:len(l50)] + n50 * dt / 2, t[:len(l20)] + n20 * dt / 2
    inside = (t50 - n50 * dt / 2 >= t[0]) & (t50 + n50 * dt / 2 <= PRE_END)
    x = v[pre] - v[pre].mean()
    w = np.hanning(len(x))
    sp = np.abs(np.fft.rfft(x * w)) * 2 / w.sum()
    fr = np.fft.rfftfreq(len(x), dt)

    def line(lo, hi):
        m = (fr >= lo) & (fr <= hi)
        k = np.argmax(sp[m])
        return {"hz": R(float(fr[m][k])), "mv_pk": R(float(sp[m][k]) * 1e3, 1)}
    nb = len(x) // n50
    bl = x[:nb * n50].reshape(nb, n50).mean(1)
    trig = float(t20[np.where((t20 > 0) & (l20 < 4.6))[0][0]])
    first_below = lambda lvl: R((float(t20[np.where((t20 > 0) & (l20 < lvl))[0][0]]) - trig) * 1e6, 1)
    return {"validity": validity(name), "pre_trigger_samples": int(pre.sum()),
            "pre_trigger_mean_v": R(float(v[pre].mean()), 3), "pre_trigger_raw_min_max_v": [R(float(v[pre].min()), 3), R(float(v[pre].max()), 3)],
            "pre_trigger_2us_average_min_max_v": [R(float(l2[t2 < PRE_END].min()), 2), R(float(l2[t2 < PRE_END].max()), 2)],
            "pre_trigger_50us_average": {"pp_v": R(float(np.ptp(l50[inside])), 3), "sd_mv": R(float(l50[inside].std()) * 1e3), "n": int(inside.sum())},
            "pre_trigger_50us_blocks": {"pp_v": R(float(np.ptp(bl)), 3), "sd_mv": R(float(bl.std()) * 1e3), "n": nb},
            "pre_trigger_bin_hz": R(float(fr[1])), "pre_trigger_window_ms": R(len(x) * dt * 1e3, 2),
            "pre_window_end_ms_before_trigger": R(-PRE_END * 1e3, 2),
            "collapse_onset_20us_average_v": {str(us): R(float(l20[int(np.argmin(abs(t20 - us * 1e-6)))]), 3) for us in (-500, -300, -200, -100, -60, -20, 0)},
            "lines": {"99.75_khz_band": line(90e3, 110e3), "200_khz_band": line(190e3, 210e3), "300_khz_band": line(290e3, 310e3),
                      "500_khz_band": line(490e3, 510e3), "20_90_khz": line(20e3, 90e3), "0.3_3_khz": line(300, 3000)},
            "rectangular_window_90_110_khz": {"hz": R(float(fr[(fr > 90e3) & (fr < 110e3)][np.argmax((np.abs(np.fft.rfft(x)) * 2 / len(x))[(fr > 90e3) & (fr < 110e3)])])),
                                              "mv_pk": R(float((np.abs(np.fft.rfft(x)) * 2 / len(x))[(fr > 90e3) & (fr < 110e3)].max()) * 1e3)},
            "fall": {"trigger_v": 4.6, "t_below_4v_us": first_below(4.0), "t_below_3v_us": first_below(3.0), "t_below_2p5v_us": first_below(2.5),
                     "min_20us_average_v": R(float(l20.min()), 2), "last_1ms_mean_v": R(float(v[t > t[-1] - 1e-3].mean()), 2)}}


def connect5(name="connect5_raw.csv"):
    t, v, dt = load(name)
    n20, n50 = (int(round(x * 1e-6 / dt)) for x in (20, 50))
    l20, l50 = box(v, n20), box(v, n50)
    t20, t50 = t[:len(l20)] + n20 * dt / 2, t[:len(l50)] + n50 * dt / 2
    kpk = int(np.argmax(l20))
    sl = (l20[n20:] - l20[:-n20]) / (n20 * dt * 1e6)
    ks = kpk + int(np.argmin(sl[kpk:kpk + 6000]))
    k1 = int(round(1e-6 / dt))
    tt = t[n20:-n20:k1]
    ll = np.convolve(v, np.ones(n20) / n20, mode="same")[n20:-n20:k1]
    ip = int(np.argmax(ll * ((tt > -1e-3) & (tt < 3e-3))))
    ic = ip + int(np.argmin(np.gradient(ll[ip:ip + 500])))
    t_ref = float(tt[ic])
    klow = ic + int(np.argmin(ll[ic:ic + 2000]))
    k50 = int(np.argmin(np.where(t50 > t_ref, l50, 1e9)[:]))
    low50 = float(l50[np.where((t50 > t_ref) & (t50 < t_ref + 2e-3))[0]].min())
    t_low50 = float(t50[np.where((t50 > t_ref) & (t50 < t_ref + 2e-3))[0][np.argmin(l50[(t50 > t_ref) & (t50 < t_ref + 2e-3)])]])
    back = lambda lvl: R((float(t50[np.where((t50 > t_low50) & (l50 >= lvl))[0][0]]) - t_low50) * 1e3, 2)
    hi, lo = float(l20[kpk]), float(ll[klow])
    seg = l20[kpk:int(np.argmin(l20[kpk:kpk + 20000])) + kpk]
    t90 = float(t20[kpk + np.where(seg <= lo + 0.9 * (hi - lo))[0][0]])
    t10 = float(t20[kpk + np.where(seg <= lo + 0.1 * (hi - lo))[0][0]])
    lev = lambda dms: R(float(ll[int(np.argmin(abs(tt - (t_ref + dms * 1e-3))))]), 3)
    return {"validity": validity(name), "start_mean_first_0p5ms_v": R(float(v[t < t[0] + 0.5e-3].mean()), 3),
            "peak_20us_average_v": R(hi, 3), "peak_t_ms": R(float(t20[kpk]) * 1e3, 3),
            "steepest_20us_spaced_slope_v_per_us": R(float(sl[ks]), 3), "steepest_20us_spaced_t_ms": R(float(t20[ks] + n20 * dt / 2) * 1e3, 3),
            "fit_time_origin_ms": R(t_ref * 1e3, 3), "gradient_at_origin_v_per_us": R(float(np.gradient(ll[ip:ip + 500])[ic - ip]), 3),
            "slope_pm10us_at_origin_v_per_us": R(float((ll[ic + 10] - ll[ic - 10]) / 20.0), 4),
            "low_20us_average_v": R(lo, 3), "low_t_after_origin_us": R((float(tt[klow]) - t_ref) * 1e6),
            "low_50us_average_v": R(low50, 3),
            "fall_90_to_10_pct_us": R((t10 - t90) * 1e6),
            "back_to_v_ms_after_low_50us_average": {"4.0": back(4.0), "4.5": back(4.5), "4.8": back(4.8), "5.0": back(5.0)},
            "levels_v_after_origin_ms": {str(d): lev(d) for d in (0.3, 0.5, 1, 2, 5, 10, 15, 20)},
            "last_1ms_mean_v": R(float(v[t > t[-1] - 1e-3].mean()), 3)}


def main():
    out = {"records": {n: validity(n, 5 if "2s4" in n or "12ms" in n else None, 20 if "2s4" in n else None) for n in (
        "vbus28_ac_12ms.csv", "vbus28_ac_2s4_unarmed.csv", "vbus28_ac_2s4_loadrun.csv", "vbus_20to28_ramp.csv",
        "reset_from5v_raw.csv", "connect5_raw.csv")},
        "dense_12ms": dense_12ms(),
        "first_analysis_retracted": {"note": "100 us averages of the clipped, point-sampled 8 us AC records; not node measurements",
                                     "unarmed_28v": first_analysis("vbus28_ac_2s4_unarmed.csv"),
                                     "load_run_3w_28v": first_analysis("vbus28_ac_2s4_loadrun.csv")},
        "ramp_20_to_28v": ramp(), "reset_from5v": reset_from5v(), "connect5": connect5()}
    (HERE / "metrics" / "vbus.json").write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
