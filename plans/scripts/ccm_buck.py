#!/usr/bin/env python3
"""Buck forced CCM at 200-500 kHz: loss per frequency, loop margins and load steps (plans/ccm-buck-50w.md).

Three parts, one per flag (all by default):

--loss   Loss by mechanism for the buck PDOs and the 50 W target (20 V / 2.5 A) from 24 V, against the
         switching frequency, the dead time and the static-leg refresh interval.  The terms are the
         ones in loss_budget.py (same sources: EPC2308/EPC2302 datasheets, IHLP6767GZER4R7M11, KiCad
         path resistances) plus L1 core loss.
         Core loss uses Vishay's IHLP form (doc 34252, eq. 3): P = K0 fe^(Kf-1) Bpk^Kb f0 1e-14 with
         Kf - 1 = 0.19 and Kb = 2.12 (the exponents of the calculator's composite-core blocks, 0.17-0.19
         and 2.11-2.21).  K0 and ET100 for the 6767GZ-11 are not published, so K is calibrated to an
         assumed core loss at the 2026-10-19 DCM point (28 V, 25.3 ohm, SR on: 9.15 A triangle,
         t1 1.79 us, t2 1.53 us, 200 kHz; --core-ref-w, default 1.0 W).  The 2026-10-16 soak put L1 at
         +45 K with about 0.1 W of copper loss, so its heat was mostly core: 0.5-2 W is the plausible
         range, and every core-loss number here scales with that one assumption.  fe is the iGSE/MSE
         equivalent frequency 2/pi^2 (1/t_rise + 1/t_fall), used for the reference and the CCM points
         alike so the calibration carries.

--loop   Margins of the CCM voltage loop as firmware runs it (regulator.c step 6c): every PID cycle
         (20 kHz) D = (V_ref + u) / V_in with u = kp e + ki integral(e), e = V_ref - V_meas.  The plant
         from u to V_out is the averaged buck into the VBUS network of loop_model.py (board ceramics
         derated for DC bias, the output-switch path, the 25.3 ohm and the load), unity at DC.
         tau = 63 us of loop delay (the 2026-10-12 fit, which contains the 9-sample V_out mean).
         Reported: crossover, phase margin, and the modulus margin min|1 + T| (0.5 = 6 dB of
         sensitivity peak; the target).

--steps  Time-domain load steps with the PID ISR's sampling: V_out sampled as the 45 us mean, the duty
         computed every 50 us and applied 13 us later (ISR length plus the HRTIM update), held for
         50 us.  Switching-averaged, so ripple is not in it.  Compared with the [PD] window of
         plans/stability-requirements.md: vSrcNew +-5 %, vSrcValid +-5 % +-0.5 V, 5 ms.
"""
import argparse
import math

import numpy as np

from loop_model import c_mlcc

L_H = 4.7e-6
R_DCR = 3.98e-3 * 1.25
R_IL = 5e-3
R_IN_PATH = 2 * 5.6e-3 * 1.3 + 5e-3
R_OUT_SHUNT = 12e-3
T_OV_ON = 3e-9
T_OFF = 1e-9
V_DRV = 5.0
FETS = {
    "EPC2308": dict(rds=4.5e-3 * 1.4, qg=11.7e-9,
                    qoss=[(0, 0), (12.5, 13), (25, 24), (50, 37), (75, 50), (100, 57), (150, 72)]),
    "EPC2302": dict(rds=1.4e-3 * 1.5, qg=23e-9,
                    qoss=[(0, 0), (10, 33), (25, 55), (50, 85), (75, 110), (100, 133)]),
}
KF1, KB = 0.19, 2.12
REF = dict(ipk=9.15, t1=1.79e-6, t2=1.53e-6, f=200e3)     # 2026-10-19, metrics/sw_periods.json


def qoss(fet, v):
    return float(np.interp(v, *zip(*FETS[fet]["qoss"]))) * 1e-9


def vsd(i):
    return 2.0 + 0.025 * abs(i)


def pfet_rds(vout):
    vgs = min(0.84 * vout, 13.0)
    r = 5.6e-3 if vgs >= 10 else 5.6e-3 + (9.2e-3 - 5.6e-3) * (10 - vgs) / 5.5
    if vgs < 4.5:
        r = 9.2e-3 * 4.5 / vgs
    return r * 1.3


def core_w(di, t1, t2, f, k):
    fe = 2 / math.pi ** 2 * (1 / t1 + 1 / t2)
    return k * fe ** KF1 * di ** KB * f


def core_k(ref_w):
    return ref_w / core_w(REF["ipk"], REF["t1"], REF["t2"], REF["f"], 1.0)


def buck_point(vin, vout, iout, f, t_dt=20e-9, n_ref=4, fet="EPC2308", k_core=None, t_ref=150e-9):
    """Forced CCM buck: Q1 active, Q2 synchronous, Q3 held on, Q4 refreshed every n_ref periods."""
    t_sw = 1 / f
    rds = FETS[fet]["rds"]
    r_out = 2 * pfet_rds(vout) + R_OUT_SHUNT
    r_l = R_DCR + R_IL + rds                       # inductor path incl. Q3 (always on)
    d = (vout + iout * (r_l + r_out)) / vin
    for _ in range(4):                             # dead time leaves SW1 at -Vsd: duty makes it up
        d = (vout + iout * (r_l + r_out + d * rds + (1 - d) * rds) + 2 * vsd(iout) * t_dt * f) / vin
    ripple = (vin - vout) * d * t_sw / L_H
    i_pk, i_v = iout + ripple / 2, iout - ripple / 2
    irms2 = iout ** 2 + ripple ** 2 / 12
    q_in = qoss(fet, vin)
    e = {}
    e["FET conduction (Q1, Q2, Q3)"] = irms2 * rds * 2 * t_sw
    e["L1 DCR + R5"] = irms2 * (R_DCR + R_IL) * t_sw
    e["L1 core"] = (core_w(ripple, d * t_sw, (1 - d) * t_sw, f, k_core) / f) if k_core else 0.0
    soft = i_v < 0 and 2 * q_in / abs(i_v) <= t_dt
    e["Q1 turn-on (Qoss V + overlap)"] = 0.0 if soft else q_in * vin + 0.5 * vin * max(i_v, 0) * T_OV_ON
    e["Q1 turn-off"] = 0.5 * vin * max(i_pk, 0) * T_OFF
    e["dead time (Q2 3rd quadrant)"] = (vsd(i_pk) * max(i_pk, 0) + vsd(i_v) * max(i_v, 0)) * t_dt
    e["Q4 refresh (Q3/Q4)"] = (qoss(fet, vout) * vout + 0.5 * vout * iout * T_OV_ON
                               + 2 * vsd(iout) * iout * t_dt) / n_ref
    e["gate drive"] = FETS[fet]["qg"] * V_DRV * (2 + 2 / n_ref)
    p_in = vout * iout
    for _ in range(5):
        i_in = p_in / vin
        e["input path (Q7, Q8, R25)"] = i_in ** 2 * R_IN_PATH * t_sw
        e["output path (Q5, Q6, R6)"] = iout ** 2 * r_out * t_sw
        p_in = vout * iout + sum(e.values()) * f
    return dict(d=d, ripple=ripple, i_pk=i_pk, i_v=i_v, e=e, p_loss=sum(e.values()) * f,
                eff=vout * iout / p_in, t_off=(1 - d) * t_sw)


def part_loss(a):
    k = core_k(a.core_ref_w)
    print(f"Buck forced CCM from {a.vin:g} V, {a.fet}, L1 core loss calibrated to "
          f"{a.core_ref_w:g} W at the 2026-10-19 DCM point (see docstring)")
    pts = [(5, 3), (9, 3), (15, 3), (20, 2.5), (20, 5)]
    for t_dt, n_ref in ((50e-9, 1), (20e-9, 4)):
        print(f"\ndead time {t_dt * 1e9:.0f} ns, Q4 refresh every {n_ref} period(s)")
        print(f"{'point':>10} {'f kHz':>6} {'D':>5} {'dI A':>5} {'Ipk':>5} {'Ival':>5} {'off ns':>6} "
              f"{'core':>5} {'sw':>5} {'cond':>5} {'loss W':>6} {'eff':>6}")
        for v, i in pts:
            for f in (200e3, 300e3, 400e3, 500e3):
                r = buck_point(a.vin, v, i, f, t_dt, n_ref, a.fet, k)
                e = r["e"]
                sw = sum(e[x] for x in ("Q1 turn-on (Qoss V + overlap)", "Q1 turn-off",
                                         "dead time (Q2 3rd quadrant)", "Q4 refresh (Q3/Q4)",
                                         "gate drive")) * f
                cond = sum(e[x] for x in ("FET conduction (Q1, Q2, Q3)", "L1 DCR + R5",
                                           "input path (Q7, Q8, R25)", "output path (Q5, Q6, R6)")) * f
                print(f"{v:>4} V {i:<3g}A {f / 1e3:6.0f} {r['d']:5.3f} {r['ripple']:5.2f} {r['i_pk']:5.2f}"
                      f" {r['i_v']:5.2f} {r['t_off'] * 1e9:6.0f} {e['L1 core'] * f:5.2f} {sw:5.2f}"
                      f" {cond:5.2f} {r['p_loss']:6.2f} {r['eff'] * 100:5.1f}%")
    r = buck_point(a.vin, 20, 2.5, 400e3, 20e-9, 4, a.fet, k)
    print("\n20 V 2.5 A at 400 kHz, 20 ns, refresh /4, every term:")
    for name, e in r["e"].items():
        print(f"  {name:32s} {e * 1e6:6.3f} uJ  {e * 400e3:5.3f} W")
    print("\nCore-loss sensitivity, 20 V 2.5 A, 20 ns, refresh /4 (W at 200 / 400 kHz):")
    for ref in (0.5, 1.0, 2.0):
        kk = core_k(ref)
        lo = buck_point(a.vin, 20, 2.5, 200e3, 20e-9, 2, a.fet, kk)
        hi = buck_point(a.vin, 20, 2.5, 400e3, 20e-9, 4, a.fet, kk)
        print(f"  ref {ref:3.1f} W: core {lo['e']['L1 core'] * 200e3:4.2f} / {hi['e']['L1 core'] * 400e3:4.2f},"
              f" total {lo['p_loss']:4.2f} / {hi['p_loss']:4.2f}, eff {lo['eff'] * 100:4.1f} / {hi['eff'] * 100:4.1f} %")


# --- loop ------------------------------------------------------------------------------------------

def z_node(s, v, r_load, load_caps, n_board=5, r_switch=0.025, l_wire=0.5e-6, r_wire=0.01):
    """Impedance at the board node (VD_MON): board ceramics || (switch path + bench node)."""
    zb = 0.006 / n_board + 1 / (s * n_board * c_mlcc(v))
    node = [r_load + 0 * s]
    if load_caps > 0:
        node.append(0.006 / 10 + 1 / (s * load_caps * 10 * c_mlcc(v)))
    zn = 1 / sum(1 / z for z in node)
    zx = r_switch + r_wire + s * l_wire + zn
    return 1 / (1 / zb + 1 / zx)


def plant(s, v, r_load, load_caps, rds=6.3e-3):
    zn = z_node(s, v, r_load, load_caps)
    return zn / (zn + s * L_H + R_DCR + R_IL + 2 * rds)


def loop_t(f, v, r_load, load_caps, ki, kp, tau=63e-6):
    s = 2j * np.pi * f
    return (kp + ki / s) * plant(s, v, r_load, load_caps) * np.exp(-s * tau)


def loop_margins(f, t):
    mag, ph = np.abs(t), np.unwrap(np.angle(t))
    ic = np.where(mag >= 1)[0]
    fc = f[ic[-1]] if ic.size else float("nan")
    pm = 180 + np.degrees(ph[ic[-1]]) if ic.size else float("nan")
    mm = np.min(np.abs(1 + t))
    k = np.argmin(np.abs(1 + t))
    return fc, pm, mm, f[k]


def part_loop(a):
    f = np.logspace(1, np.log10(9.9e3), 6000)       # up to Nyquist of the 20 kHz loop
    print(f"\nCCM voltage loop (u -> V_out unity at DC), tau 63 us, QT Py ceramics seen: {a.load_caps:g}")
    print(f"{'V':>3} {'R ohm':>6} {'f_LC kHz':>8} {'ki 1/s':>7} {'kp':>5} {'f_c Hz':>7} {'PM':>5} "
          f"{'min|1+T|':>8} {'at Hz':>7}")
    for v, r in ((5, 1.67), (9, 3.0), (15, 5.0), (20, 25.3), (20, 8.0)):
        c = 5 * c_mlcc(v)
        f_lc = 1 / (2 * np.pi * np.sqrt(L_H * c)) / 1e3
        for fc_target, kp in ((300, 0.0), (500, 0.0), (500, 0.2), (1000, 0.0), (1500, 0.0)):
            ki = 2 * np.pi * fc_target
            t = loop_t(f, v, r, a.load_caps, ki, kp)
            fc, pm, mm, fm = loop_margins(f, t)
            print(f"{v:>3} {r:6.2f} {f_lc:8.1f} {ki:7.0f} {kp:5.2f} {fc:7.0f} {pm:5.0f} {mm:8.2f} {fm:7.0f}")


# --- steps -----------------------------------------------------------------------------------------

def simulate(v_ref, r_seq, ki, kp, vin=24.0, load_caps=0.0, t_end=6e-3, dt=0.25e-6,
             integ0=None, r_switch=0.035, l_wire=0.5e-6):
    """Averaged buck, board C || (r_switch + l_wire -> node C, R_load(t)); PID ISR at 20 kHz."""
    c_b = 5 * c_mlcc(v_ref)
    c_x = load_caps * 10 * c_mlcc(v_ref) if load_caps > 0 else 0.0
    r_l = R_DCR + R_IL + 2 * 6.3e-3
    r0 = r_seq(0.0)
    i0 = v_ref / r0
    il, vb, ix, vx = i0, v_ref, i0, v_ref
    integ = (i0 * (r_l + r_switch)) if integ0 is None else integ0
    d = (v_ref + integ) / vin
    d_next, t_apply = d, None
    hist = []                                       # (t, vb) for the 45 us mean
    n_isr = int(round(50e-6 / dt))
    out_t, out_v, out_vx = [], [], []
    for n in range(int(t_end / dt)):
        t = n * dt
        hist.append(vb)
        if n % n_isr == 0 and n > 0:
            m = int(round(45e-6 / dt))
            v_meas = float(np.mean(hist[-m:]))
            e = v_ref - v_meas
            integ += ki * e * 50e-6
            u = kp * e + integ
            d_next = min(max((v_ref + u) / vin, 0.0), 0.95)
            t_apply = t + 13e-6
            hist = hist[-m:]
        if t_apply is not None and t >= t_apply:
            d, t_apply = d_next, None
        r = r_seq(t)
        dil = (d * vin - vb - il * r_l) / L_H
        if c_x > 0:
            dix = (vb - vx - ix * r_switch) / l_wire
            dvx = (ix - vx / r) / c_x
            dvb = (il - ix) / c_b
        else:
            dix = 0.0
            dvx = 0.0
            ix = vb / (r + r_switch)
            dvb = (il - ix) / c_b
        il += dil * dt
        vb += dvb * dt
        ix += dix * dt
        vx += dvx * dt
        if c_x == 0:
            vx = vb - ix * r_switch
        out_t.append(t)
        out_v.append(vb)
        out_vx.append(vx)
    return np.array(out_t), np.array(out_v), np.array(out_vx)


def pd_window(v):
    return 0.05 * v, 0.05 * v + 0.5


def part_steps(a):
    print("\nCCM load steps (averaged, PID ISR sampling), node after the output switch; "
          "[PD] vSrcNew / vSrcValid")
    print(f"{'V':>3} {'step':>14} {'ki':>5} {'kp':>4} {'dip V':>6} {'over V':>6} {'t>new ms':>8} "
          f"{'valid':>5} {'ring kHz':>8}")
    for v, i_lo, i_hi in ((5, 0.6, 3.0), (9, 0.6, 3.0), (15, 0.6, 3.0), (20, 0.8, 2.5)):
        for fc, kp in ((300, 0.0), (500, 0.0), (1000, 0.0)):
            ki = 2 * np.pi * fc
            for up in (True, False):
                r_a, r_b = (v / i_lo, v / i_hi) if up else (v / i_hi, v / i_lo)
                t_step = 1e-3
                tt, vb, vx = simulate(v, lambda t: r_a if t < t_step else r_b, ki, kp,
                                      load_caps=a.load_caps)
                post = vx[tt >= t_step] - v
                new, valid = pd_window(v)
                dip, over = -min(post.min(), 0), max(post.max(), 0)
                outside = np.abs(post) > new
                t_out = (np.nonzero(outside)[0][-1] * (tt[1] - tt[0]) * 1e3) if outside.any() else 0.0
                ok = "yes" if np.abs(post).max() <= valid and t_out <= 5 else "NO"
                sp = np.abs(np.fft.rfft((post - post.mean())[: int(2e-3 / (tt[1] - tt[0]))]))
                fr = np.fft.rfftfreq(int(2e-3 / (tt[1] - tt[0])), tt[1] - tt[0])
                ring = fr[np.argmax(sp[1:]) + 1] / 1e3
                lbl = f"{i_lo:g}->{i_hi:g} A" if up else f"{i_hi:g}->{i_lo:g} A"
                print(f"{v:>3} {lbl:>14} {ki:5.0f} {kp:4.1f} {dip:6.2f} {over:6.2f} {t_out:8.2f} "
                      f"{ok:>5} {ring:8.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--loss", action="store_true")
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--steps", action="store_true")
    ap.add_argument("--vin", type=float, default=24.0)
    ap.add_argument("--fet", default="EPC2308", choices=list(FETS))
    ap.add_argument("--core-ref-w", type=float, default=1.0,
                    help="assumed L1 core loss at the 2026-10-19 DCM point, W")
    ap.add_argument("--load-caps", type=float, default=0.0,
                    help="share of the QT Py's ten VIN ceramics behind the output switch (0 to 1)")
    a = ap.parse_args()
    every = not (a.loss or a.loop or a.steps)
    if a.loss or every:
        part_loss(a)
    if a.loop or every:
        part_loop(a)
    if a.steps or every:
        part_steps(a)


if __name__ == "__main__":
    main()
