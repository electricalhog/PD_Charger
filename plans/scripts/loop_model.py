#!/usr/bin/env python3
"""Voltage-loop and VBUS impedance model of the DCM regulator (plans/240w-roadmap.md section 2).

In DCM the converter is a controlled current source: the PI's command I_cmd becomes average
current into the output node, one PID period late.  So the loop is

    T(s) = (kp + ki/s) * g * exp(-s*tau) * Z_bus(s)        Z_out,closed(s) = Z_bus(s) / (1 + T(s))

- kp = 2*pi*fc*C_model and ki = kp*2*pi*fc/4: regulator.c step 2 with LOOP_CROSSOVER_HZ, and
  C_model = LOOP_C_INT_UF + LOOP_C_EXT_UF once the output switch is on.
- tau = 63 us, the loop delay measured on 2026-10-12 (regulator_config.h LOOP_* note).
- g = the modulator's gain (delivered / commanded current), 1 by default.
- Z_bus: what the regulator drives at its sense point (VD_MON, converter side of the output switch):
  - the board's five CL31B106KBHNNNE (10 uF, 50 V, X7R) under DC bias (Samsung curve), 6 mOhm
    ESR each; the electrolytics C14/C15 are not fitted;
  - the output switch path, Q5 + Q6 + R6, and the wiring to the bench node;
  - at the bench node: the 25.3 ohm, the QT Py buck's input (about 100-120 uF, user 2026-10-17;
    part and voltage behaviour not known; `load_caps` scales ten 50 V X7R 10 uF), and, if armed,
    the buck's output network reflected through its duty.

Run with no arguments for the cases below; the table compares |T| at the -180 degree frequency
with what the bench showed.
"""
import argparse

import numpy as np

# Samsung CL31B106KBHNNNE DC-bias curve (1 kHz, 1 Vrms), read off the characteristic sheet.
BIAS_V = [0, 2, 5, 10, 15, 20, 25, 28, 30, 36, 40, 48, 50]
BIAS_DC = [0, 1, -12, -38, -57, -68, -76, -80, -81, -85, -86, -88, -89]


def c_mlcc(v, c0=10e-6):
    return c0 * (1 + np.interp(v, BIAS_V, BIAS_DC) / 100)


def z_c(s, c, esr):
    return esr + 1 / (s * c)


def par(*zs):
    return 1 / sum(1 / z for z in zs)


def z_bus(s, v, r_load=25.3, load_caps=0.0, armed_duty=None, n_board=5,
          r_switch=0.025, l_wire=0.5e-6, r_wire=0.01):
    board = z_c(s, n_board * c_mlcc(v), 0.006 / n_board)
    node = [r_load + 0 * s] if r_load else []
    if load_caps > 0:
        node.append(z_c(s, load_caps * 10 * c_mlcc(v), 0.006 / 10))
    if armed_duty:
        # QT Py buck, one phase: 1 uH into 8.4 mF + 9 x 10 uF and the 11 ohm ballast, seen
        # through duty D as impedance / D^2 (its own control is ms-slow: treat D as fixed).
        out = par(z_c(s, 8.4e-3 + 90e-6, 0.01), 11.0 + 0 * s)
        node.append((s * 1e-6 + 0.02 + out) / armed_duty ** 2)
    ext = r_switch + r_wire + s * l_wire + par(*node) if node else None
    return par(board, ext) if ext is not None else board


def loop(f, v, fc=100.0, c_model=300e-6, tau=63e-6, g=1.0, **bus):
    s = 2j * np.pi * f
    kp = 2 * np.pi * fc * c_model
    ki = kp * 2 * np.pi * fc / 4
    zb = z_bus(s, v, **bus)
    t = (kp + ki / s) * g * np.exp(-s * tau) * zb
    return t, zb, zb / (1 + t)


def margins(f, t):
    mag, ph = np.abs(t), np.unwrap(np.angle(t))
    i_c = np.where(mag[:-1] >= 1)[0]
    fcross = f[i_c[-1]] if i_c.size else None
    pm = 180 + np.degrees(ph[i_c[-1]]) if i_c.size else None
    i_180 = np.where(np.degrees(ph) <= -180)[0]
    f180 = f[i_180[0]] if i_180.size else None
    t180 = mag[i_180[0]] if i_180.size else None
    return fcross, pm, f180, t180


CASES = [
    # label, V, kwargs, what the bench showed
    ("10/12 20 V, no 25 ohm, fc 300", 20, dict(fc=300, r_load=None), "limit cycle 3.9 kHz"),
    ("10/12 20 V, no 25 ohm, fc 100", 20, dict(fc=100, r_load=None), "held (200 mV p-p slow ripple)"),
    ("10/15-16 28 V, 25 ohm, fc 100", 28, dict(), "held (coil whine, 5 Hz swing)"),
    ("10/15 36 V, 25 ohm, fc 100", 36, dict(), "oscillated 3.92 kHz, 5.3 V p-p"),
    ("48 V, 25 ohm, fc 100", 48, dict(), "(prediction)"),
    ("36 V, 25 ohm, c_ext 0", 36, dict(c_model=50e-6), "(prediction: the c_ext test)"),
    ("28 V, 25 ohm, load armed D 0.25", 28, dict(armed_duty=0.25), "(prediction)"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--load-caps", type=float, default=0.0,
                    help="share of the QT Py's ten VIN ceramics the loop sees (0 = none, 1 = all, as 50 V X7R)")
    ap.add_argument("--g", type=float, default=1.0, help="modulator gain")
    ap.add_argument("--plot", help="write |Z_out,closed| and |T| per case to this PNG")
    ap.add_argument("--step", nargs=2, type=float, metavar=("V", "AMPS"), action="append", default=[],
                    help="VBUS response to a load-current step at contract V (AMPS > 0 adds load, "
                         "< 0 removes it), 25.3 ohm on; repeatable.  Linear: a removal whose peak "
                         "passes the skip band (1.5 %% above the setpoint) is clipped by pulse skipping.")
    a = ap.parse_args()
    f = np.logspace(0, 5, 4000)
    print(f"load ceramics seen: {a.load_caps:g}, g = {a.g:g}, tau 63 us, C_model 300 uF unless noted")
    print(f"{'case':36s} {'C_board':>8s} {'f_c':>7s} {'PM':>6s} {'f_-180':>8s} {'|T|':>5s} "
          f"{'Zpk':>7s} {'at':>7s}  bench")
    rows = []
    for label, v, kw, seen in CASES:
        kw = {"load_caps": a.load_caps, "g": a.g, **kw}
        t, zb, zc = loop(f, v, **kw)
        fcross, pm, f180, t180 = margins(f, t)
        k = np.argmax(np.abs(zc))
        rows.append((label, f, t, zc))
        print(f"{label:36s} {5 * c_mlcc(v) * 1e6:6.1f}uF "
              f"{(f'{fcross:5.0f}Hz' if fcross else '     -'):>7s} "
              f"{(f'{pm:5.0f}' if pm is not None else '    -'):>6s} "
              f"{(f'{f180:6.0f}Hz' if f180 else '      -'):>8s} {(t180 or 0):5.2f} "
              f"{np.abs(zc[k]):5.1f}ohm {f[k]:5.0f}Hz  {seen}")
    for v, amps in a.step:
        fs, n = 2e6, 2 ** 17
        ff = np.fft.rfftfreq(n, 1 / fs)
        ff[0] = 1e-3
        _, _, zcl = loop(ff, v, load_caps=a.load_caps, g=a.g)
        dv = -np.cumsum(np.fft.irfft(zcl, n=n)) * amps      # impulse response integrated: step
        tt = np.arange(n) / fs
        k = np.argmax(np.abs(dv[:int(5e-3 * fs)]))
        band = 0.015 * v
        note = "  (removal past the skip band: clipped near +%.2f V)" % band if amps < 0 and dv[k] > band else ""
        print(f"step {amps:+.2f} A at {v:g} V: extreme {dv[k]:+.2f} V at {tt[k] * 1e6:.0f} us, "
              f"{dv[int(1e-3 * fs)]:+.2f} V at 1 ms, {dv[int(5e-3 * fs)]:+.2f} V at 5 ms{note}")
    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(2, 1, figsize=(9, 7), sharex=True)
        for label, f, t, zc in rows:
            ax[0].loglog(f, np.abs(zc), label=label)
            ax[1].loglog(f, np.abs(t), label=label)
        ax[0].set_ylabel("|Z_out, closed loop| [ohm]")
        ax[1].set_ylabel("|T| (loop gain)")
        ax[1].axhline(1, color="k", lw=0.8)
        ax[1].set_xlabel("frequency [Hz]")
        for x in ax:
            x.grid(which="both", alpha=0.3)
        ax[0].legend(fontsize=7)
        ax[0].set_title(f"DCM loop model: load ceramics seen {a.load_caps:g}, g {a.g:g}", fontsize=10)
        fig.tight_layout()
        fig.savefig(a.plot, dpi=90)
        print("wrote", a.plot)


if __name__ == "__main__":
    main()
