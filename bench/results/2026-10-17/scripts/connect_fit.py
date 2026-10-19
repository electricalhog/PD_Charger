#!/usr/bin/env python3
"""Fit the 5 V load connect (2026-10-17) with a time-domain model of the regulator and VBUS.

The record is data/cap/connect5_raw.csv: CH4 on TP2 (VOUTa, before the output switch), 80 ns,
triggered on the start-up edge after a G474 reset.  At the connect the output switch (Q5/Q6)
closes onto the bench side: the 25.3 ohm, the QT Py's draw, and the bench-side capacitance,
which OUTPUT_DIS had pulled down.

Model, 1 us steps:
- VOUTa: the board's five ceramics at 5 V (about 44 uF, Samsung bias curve) and the converter's
  current g * clip(I_cmd(t - 63 us), 0, I_max).
- I_cmd from the PI as coded: kp = 2 pi 100 Hz 300 uF, ki = kp 2 pi 100 / 4 (regulator.c step 2),
  setpoint 5.0 V, integrator starting at its pre-connect value (fitted, near 0: no load before).
- The switch: a conductance rising linearly from 0 to 1/(25 mOhm) over T_sw from t_c.
- The bench side: C_d from V_d0, the 25.3 ohm and a constant 73 mA (the QT Py at 5 V).

Fitted: C_d, T_sw (bounded 0.1-0.8 ms: R27 10 kohm into the gates), g, I_int0 and t_c.  V_d0 is
fixed at 2.65 V, where the QT Py back-feeds its VIN (bench/README.md).  `--free` also fits V_d0
and lets T_sw run to 2 ms; `--qtpy` holds C_d at 100-120 uF (the user's figure for the QT Py
input) and frees V_d0 and T_sw (to 5 ms).  Writes metrics/connect_fit.json (connect_fit_free.json with --free)
and the matching figure.
"""
import json
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares

HERE = Path(__file__).resolve().parents[1]
DT = 1e-6
KP = 2 * np.pi * 100 * 300e-6
KI = KP * 2 * np.pi * 100 / 4
C_B = 5 * 10e-6 * 0.88          # five CL31B106KBHNNNE at about 5 V (-12 %)
V_SET, R_LOAD, I_QTPY, I_MAX, TAU = 5.0, 25.3, 0.073, 1.85, 63e-6


def measured():
    d = np.loadtxt(HERE / "data/cap/connect5_raw.csv", delimiter=",", skiprows=1)
    t, v = d[:, 0], d[:, 1]
    n = int(round(20e-6 / (t[1] - t[0])))
    lp = np.convolve(v, np.ones(n) / n, mode="same")
    k = int(round(DT / (t[1] - t[0])))
    t, lp = t[n:-n:k], lp[n:-n:k]
    i_pk = np.argmax(lp * ((t > -1e-3) & (t < 3e-3)))
    i_c = i_pk + np.argmin(np.gradient(lp[i_pk:i_pk + 500]))
    t0 = t[i_c] - 0.3e-3
    m = (t >= t0) & (t <= t[i_c] + 20e-3)
    return t[m] - t[i_c], lp[m]


def simulate(p, t):
    c_d, v_d0, t_sw, g, i_int0, t_c = p
    n = len(t)
    va, vd = np.empty(n), np.empty(n)
    a, b, integ = None, v_d0, i_int0
    d = int(round(TAU / DT))
    cmd_hist = np.zeros(n + d)
    a = V_SET + 0.1
    for i in range(n):
        e = V_SET - a
        integ = min(max(integ + KI * e * DT, -2.0), I_MAX)
        cmd = KP * e + integ
        cmd_hist[i + d] = cmd
        i_conv = g * min(max(cmd_hist[i], 0.0), I_MAX)
        if a > V_SET * 1.015:
            i_conv = 0.0                      # pulse skipping above the band
        x = (t[i] - t_c) / t_sw
        gsw = 0.0 if x <= 0 else min(x, 1.0) / 0.025
        i_sw = gsw * (a - b)
        i_bench = b / R_LOAD + (I_QTPY if b > 2.5 else 0.0)
        a += (i_conv - i_sw) / C_B * DT
        b += (i_sw - i_bench) / c_d * DT
        va[i], vd[i] = a, b
    return va, vd


def main():
    import sys
    free = "--free" in sys.argv
    qtpy = "--qtpy" in sys.argv          # the QT Py input held at 100-120 uF (user, 2026-10-17)
    t, v = measured()
    x0 = [110e-6 if qtpy else 60e-6, 2.65, 0.4e-3, 1.0, 0.0, -0.2e-3]
    lo = [100e-6 if qtpy else 5e-6, 0.0 if (free or qtpy) else 2.64, 20e-6 if (free or qtpy) else 100e-6, 0.3, -1.0, -0.6e-3]
    hi = [120e-6 if qtpy else 800e-6, 4.5 if (free or qtpy) else 2.66, 5e-3 if qtpy else (2e-3 if free else 800e-6),
          3.0, 1.5, 0.3e-3]
    fit = least_squares(lambda p: simulate(p, t)[0] - v, x0, bounds=(lo, hi), x_scale=[1e-5, 1, 1e-4, 1, 1, 1e-4])
    va, vd = simulate(fit.x, t)
    c_d, v_d0, t_sw, g, i_int0, t_c = fit.x
    out = {"c_bench_uf": round(c_d * 1e6, 1), "v_bench_start_v": round(v_d0, 2), "switch_ramp_us": round(t_sw * 1e6),
           "g": round(g, 3), "integrator_start_a": round(i_int0, 3), "t_c_us": round(t_c * 1e6),
           "rms_error_mv": round(float(np.sqrt(np.mean((va - v) ** 2))) * 1e3, 1),
           "low_measured_v": round(float(v.min()), 2), "low_model_v": round(float(va.min()), 2),
           "at_5ms_measured_v": round(float(v[np.argmin(abs(t - 5e-3))]), 2),
           "at_5ms_model_v": round(float(va[np.argmin(abs(t - 5e-3))]), 2),
           "fixed": {"c_board_uf": round(C_B * 1e6, 1), "kp_a_per_v": round(KP, 4), "ki_a_per_v_s": round(KI, 2),
                     "delay_us": TAU * 1e6, "r_load_ohm": R_LOAD, "i_qtpy_a": I_QTPY}}
    (HERE / ("metrics/connect_fit_qtpy.json" if qtpy else "metrics/connect_fit_free.json" if free else "metrics/connect_fit.json")).write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(9, 4.5))
        ax.plot(t * 1e3, v, lw=1, label="TP2 measured (20 us average)")
        ax.plot(t * 1e3, va, lw=1.2, label="model: VOUTa")
        ax.plot(t * 1e3, vd, lw=1, ls="--", label="model: bench side")
        ax.set_xlabel("ms from the steepest fall")
        ax.set_ylabel("V")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
        ax.set_title(f"5 V load connect: bench side {c_d * 1e6:.0f} uF from {v_d0:.1f} V, g {g:.2f}", fontsize=10)
        (HERE / "figures").mkdir(exist_ok=True)
        fig.tight_layout()
        fig.savefig(HERE / ("figures/connect5_fit_qtpy.png" if qtpy else "figures/connect5_fit_free.png" if free else "figures/connect5_fit.png"), dpi=90)
    except ImportError:
        pass


if __name__ == "__main__":
    main()
