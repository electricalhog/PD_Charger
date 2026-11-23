#!/usr/bin/env python3
"""Switch-node timing and ring at 28 V, rectifier on (2026-10-16, 22:45).

Reads data/cap/sw_28v_sr1_raw.csv (scope memory: CH2 SW1, CH3 SW2, CH4 VBUS; 4 ns, 1.2 ms) and
writes metrics/sw_ring.json: per-period charge and discharge times, the body-diode clamp after
the current reaches zero, the free ring's frequency, and the spikes beyond the clamp levels near
switching edges.
"""
import json
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
DT, L_H, V_IN = 4e-9, 4.7e-6, 24.56

d = np.loadtxt(HERE / "data/cap/sw_28v_sr1_raw.csv", delimiter=",", skiprows=1)
s1, s2, vb = d[:, 1], d[:, 2], d[:, 3]
hi1 = s1 > 12
up = np.where(~hi1[:-1] & hi1[1:])[0] + 1
dn = np.where(hi1[:-1] & ~hi1[1:])[0] + 1
# Charge starts: SW1 rises while SW2 has been low (Q4 on).  Clamp starts: SW1 rises from a discharge.
charge = [i for i in up if i > 25 and s2[i - 25:i - 2].max() < 5]
clamp = [i for i in up if i > 25 and s2[i - 25:i - 2].mean() > 20]
t_c, t_d, t_k = [], [], []
for c in charge:
    e = dn[dn > c]
    k = [x for x in clamp if e.size and e[0] < x < e[0] + 1250]
    if not (e.size and k):
        continue
    e2 = dn[dn > k[0]]
    if not e2.size:
        continue
    t_c.append((e[0] - c) * DT)
    t_d.append((k[0] - e[0]) * DT)
    t_k.append((e2[0] - k[0]) * DT)
t_c, t_d, t_k = map(np.array, (t_c, t_d, t_k))
v_out = vb.mean()
i_pk = V_IN * t_c.mean() / L_H

# Free ring between a clamp and the next charge: rising zero crossings of SW1 - SW2, band-passed
# (60 ns boxcar low-pass minus a 1 us boxcar), so edge noise and the slow drift drop out.
v = s1 - s2
bp = np.convolve(v, np.ones(15) / 15, mode="same") - np.convolve(v, np.ones(250) / 250, mode="same")
charge_a = np.array(charge)
ring_periods = []
for k in clamp:
    e, nx = dn[dn > k], charge_a[charge_a > k]
    if not (e.size and nx.size):
        continue
    a, b = e[0] + 40, nx[0] - 60
    if not 150 < b - a < 1500:
        continue
    seg = bp[a:b]
    z = np.where((seg[:-1] < 0) & (seg[1:] >= 0))[0]
    ring_periods += list(np.diff(z) * DT)
t_ring = float(np.median(ring_periods))
f_ring = 1 / t_ring
c_eq = 1 / ((2 * np.pi * f_ring) ** 2 * L_H)

edges = np.sort(np.concatenate([np.where(np.abs(np.diff((s1 > 12).astype(int))))[0],
                                np.where(np.abs(np.diff((s2 > 15).astype(int))))[0]]))
periods = len(s1) * DT / 5e-6


def beyond(mask):
    idx = np.where(mask)[0]
    dist = np.array([np.min(np.abs(edges - i)) for i in idx[::max(1, len(idx) // 400)]]) * DT * 1e9
    return {"ns_per_period": round(len(idx) * DT * 1e9 / periods, 1),
            "ns_from_edge_median_p90": [round(x) for x in np.percentile(dist, [50, 90])]}


segs = [vb[i:i + 40] - vb[i - 60:i - 10].mean() for i in up[5:60] if i > 60]
spec = np.mean([np.abs(np.fft.rfft(s * np.hanning(len(s)))) for s in segs], axis=0)
out = {
    "periods_analyzed": int(len(t_c)),
    "vbus_mean_v": round(float(v_out), 2),
    "charge_ns_mean_min_max": [round(x * 1e9) for x in (t_c.mean(), t_c.min(), t_c.max())],
    "discharge_ns_mean_min_max": [round(x * 1e9) for x in (t_d.mean(), t_d.min(), t_d.max())],
    "clamp_ns_mean": round(float(t_k.mean()) * 1e9),
    "i_pk_a_from_charge": round(float(i_pk), 2),
    "zero_crossing_ns_after_charge": round(float(i_pk * L_H / v_out) * 1e9),
    "ring_period_ns_median_iqr": [round(t_ring * 1e9)] + [round(x * 1e9) for x in np.percentile(ring_periods, [25, 75])],
    "ring_mhz": round(float(f_ring) / 1e6, 2),
    "ring_c_eq_nf": round(float(c_eq) * 1e9, 2),
    "sw1_below_minus4v": beyond(s1 < -4),
    "sw2_above_35v": beyond(s2 > 35),
    "sw1_min_v": round(float(s1.min()), 1), "sw2_max_v": round(float(s2.max()), 1),
    "vbus_burst_after_sw1_rise_mhz": round(float(np.fft.rfftfreq(40, DT)[spec[1:].argmax() + 1]) / 1e6, 1),
    "vbus_burst_pp_v": round(float(np.mean([np.ptp(s) for s in segs])), 2),
}
(HERE / "metrics/sw_ring.json").write_text(json.dumps(out, indent=1) + "\n")
print(json.dumps(out, indent=1))
