#!/usr/bin/env python3
"""Boost and four-switch (band) forced CCM against DCM, every PD target, 10-24 V in (plans/regulation-strategy.md 3.5, 3.6).

Same loss terms and sources as ccm_buck.py (EPC2308, IHLP6767GZ core loss calibrated with --core-ref-w, KiCad path
resistances).  New here:

* One piecewise-linear CCM engine for all three CCM modes.  The inductor current i (SW1 -> SW2) is built from the
  interval voltages, its valley/peak set by the average I_L, and each edge is costed from the current it switches:
    - a hard turn-on (current flowing into the turning-on FET's body diode is not available): Qoss(V) V + overlap;
    - a current-driven turn-off: 0.5 V i T_OFF;
    - dead time: Vsd(i) |i| t_dt per edge.
  buck : Q1/Q2 switch, Q3 on, Q4 refresh every 5 us inside Q2-on (L sees 0 V for the refresh).
  boost: Q4/Q3 switch, Q1 on, Q2 refresh every 5 us inside Q4-on (L sees 0 V for the refresh).
  band : both legs every period: Q1Q4 (L sees V_in), Q1Q3 (V_in - V_out), Q2Q3 (-V_out).  No refresh needed (both
         low sides turn on every period).  D1 held at its maximum and D2 regulates above ~0.93 V_in.
* A DCM model of the present firmware modes (regulator.c step 5, rectifier faded in) at 200 kHz: buck, buck-boost
  (Q1Q4 charge from V_in, Q2Q3 discharge into V_out) and boost (Q4 charge, Q3 discharge V_out - V_in).  The DCM
  turn-on is at zero current with the node ringing: 0.5 Qoss V on average (no valley detection).
* The boost loop: with the feedforward D = 1 - V_in / (V_ref + u) the plant from u to V_out is unity at DC, so the
  ki that works in buck carries over, but the double pole moves to (1 - D)/(2 pi sqrt(L C)) and a RHP zero appears at
  (1 - D)^2 R / (2 pi L).  Margins with the same 63 us loop delay as ccm_buck.py.

Not in it: housekeeping (MCU, ADM1270, gate-drive quiescent), PCB copper beyond the listed paths, temperature
iteration.  Every number printed is *model*.
"""
import argparse
import math

import numpy as np

from ccm_buck import FETS, L_H, R_DCR, R_IL, R_IN_PATH, R_OUT_SHUNT, T_OFF, T_OV_ON, V_DRV, core_k, core_w, \
    pfet_rds, qoss, vsd
from loop_model import c_mlcc

FET = "EPC2308"
RDS = FETS[FET]["rds"]
QG = FETS[FET]["qg"]
T_REF_SLOT = 300e-9          # CCM refresh length (regulator_config.h CCM_REFRESH_NS)
REF_INTERVAL = 5e-6          # CCM_REFRESH_INTERVAL_NS
I_IN_MAX = 9.0               # input path (ADM1270 at ~10 A through R25 = 5 mOhm)
D1_MAX = {200e3: 0.93, 400e3: 0.86}
PDOS = [(5, 3.0), (9, 3.0), (15, 3.0), (20, 5.0), (28, 5.0)]
VINS = [10.0, 12.0, 15.0, 20.0, 24.0]


def r_out(vout):
    return 2 * pfet_rds(vout) + R_OUT_SHUNT


def ccm_wave(segs, i_avg):
    """segs: [(dt, v_L)] over one period.  Returns per-edge currents [(i_start)], i_min, i_max, irms^2."""
    t, i = [0.0], [0.0]
    for dt, v in segs:
        t.append(t[-1] + dt)
        i.append(i[-1] + v * dt / L_H)
    tp = t[-1]
    # mean of the zero-start waveform
    m = sum((t[k + 1] - t[k]) * (i[k] + i[k + 1]) / 2 for k in range(len(segs))) / tp
    off = i_avg - m
    i = [x + off for x in i]
    rms2 = sum((t[k + 1] - t[k]) * (i[k] ** 2 + i[k] * i[k + 1] + i[k + 1] ** 2) / 3 for k in range(len(segs))) / tp
    return i, min(i), max(i), rms2


def ccm_point(mode, vin, vout, iout, f=200e3, t_dt=20e-9, k_core=None):
    """Loss of one forced-CCM point [W] and its operating values; None when the mode cannot reach it."""
    tp = 1 / f
    n_per_ref = max(1, round(REF_INTERVAL * f))           # periods per refresh
    r_path = 2 * RDS + R_DCR + R_IL                       # two FETs in series with L1 at every instant
    ro = r_out(vout)
    p_in = vout * iout / 0.95
    for _ in range(6):
        i_in = p_in / vin
        vin_e = vin - i_in * R_IN_PATH
        if mode == "buck":
            il = iout
            d_ref = T_REF_SLOT / (n_per_ref * tp)          # share of time L sees 0 V in the refresh
            # V_out' = D1 V_in' with the refresh slot taking from Q2's time (L at 0 V there)
            vo_e = vout + iout * ro + il * r_path + 2 * vsd(il) * t_dt * f
            d1 = vo_e / vin_e
            if d1 > D1_MAX[f]:
                return None
            ton = d1 * tp
            segs = [(ton, vin_e - vo_e), (tp - ton, -vo_e)]
            edges = [("hard_on", vin, "SW1 Q1 on"), ("off", vin, "SW1 Q1 off")]
            refresh_v, refresh_i = vout, il
            n_sw = 2
        elif mode == "boost":
            # V_out' (1 - D2) = V_in' with the refresh inside Q4-on (L at 0 V for T_REF_SLOT / n periods)
            il = iout
            for _ in range(5):
                vo_e = vout + iout * ro
                v_drop = il * r_path + 2 * vsd(il) * t_dt * f
                r = T_REF_SLOT / (n_per_ref * tp)
                # volt-second: (vin_e - v_drop)(D2 - r) * 1 ... with L at -v_drop in the slot
                # charge: L sees vin_e - v_drop for (D2 - r), -v_drop for r; discharge: vin_e - vo_e - v_drop
                # balance: (vin_e - v_drop)(1 - r) - vo_e (1 - D2) = 0
                one_m_d2 = (vin_e - v_drop) * (1 - r) / vo_e
                d2 = 1 - one_m_d2
                il = iout / one_m_d2
            if d2 < r + 50e-9 * f:                         # Q4-on has to hold the refresh slot
                return None
            if d2 > 0.85:
                return None
            ton = d2 * tp
            segs = [(ton - r * tp, vin_e - v_drop), (r * tp, -v_drop), (tp - ton, vin_e - vo_e - v_drop)]
            edges = [("hard_on", vout, "SW2 Q4 on"), ("off", vout, "SW2 Q4 off")]
            refresh_v, refresh_i = vin, il
            n_sw = 2
        elif mode == "band":
            d1 = D1_MAX[f]
            il = iout
            for _ in range(5):
                vo_e = vout + iout * ro
                v_drop = il * r_path + 4 * vsd(il) * t_dt * f
                # V_out' = V_in' D1 / (1 - D2)  -> 1 - D2 = (V_in' - v_drop) D1 / V_out'
                one_m_d2 = ((vin_e - v_drop) * d1) / vo_e
                d2 = 1 - one_m_d2
                il = iout / one_m_d2
            if d2 < 0.02 or d2 > 0.5 or d2 > d1:
                return None
            # order: Q1Q4 [0, d2], Q1Q3 [d2, d1], Q2Q3 [d1, 1]
            segs = [(d2 * tp, vin_e - v_drop), ((d1 - d2) * tp, vin_e - vo_e - v_drop), ((1 - d1) * tp, -vo_e - v_drop)]
            edges = [("hard_on", vin, "SW1 Q1 on"), ("hard_on", vout, "SW2 Q4 on"),
                     ("off", vout, "SW2 Q4 off"), ("off", vin, "SW1 Q1 off")]
            refresh_v, refresh_i = 0.0, 0.0
            n_sw = 4
        else:
            raise ValueError(mode)
        i_nodes, i_v, i_pk, rms2 = ccm_wave(segs, il)
        # edge currents: hard turn-on at period start (i_nodes[0]); turn-offs at the node where that switch ends
        if mode == "buck":
            cur = [i_nodes[0], i_nodes[1]]
        elif mode == "boost":
            cur = [i_nodes[0], i_nodes[2]]
        else:
            cur = [i_nodes[0], i_nodes[0], i_nodes[1], i_nodes[2]]
        e = {}
        e["conduction (2 FETs + L1 DCR + R5)"] = rms2 * r_path * tp
        e["L1 core"] = (core_w(i_pk - i_v, segs[0][0], tp - segs[0][0], f, k_core) / f) if k_core else 0.0
        e_on = e_off = e_dt = 0.0
        for (kind, v, _), i in zip(edges, cur):
            if kind == "hard_on":
                soft = i < 0 and 2 * qoss(FET, v) / abs(i) <= t_dt
                e_on += 0.0 if soft else qoss(FET, v) * v + 0.5 * v * max(i, 0) * T_OV_ON
            else:
                e_off += 0.5 * v * abs(i) * T_OFF
            e_dt += vsd(i) * abs(i) * t_dt * 2 / 2          # each leg edge pair: one dead time per edge
        e["hard turn-on"] = e_on
        e["turn-off"] = e_off
        e["dead time"] = e_dt * 2                           # both dead times of each switching edge pair
        e["refresh"] = (qoss(FET, refresh_v) * refresh_v + 0.5 * refresh_v * refresh_i * T_OV_ON
                        + 2 * vsd(refresh_i) * refresh_i * t_dt) / n_per_ref if refresh_v else 0.0
        e["gate drive"] = QG * V_DRV * (n_sw + (2 / n_per_ref if refresh_v else 0))
        e["input path"] = i_in ** 2 * R_IN_PATH * tp
        e["output path"] = iout ** 2 * ro * tp
        p_loss = sum(e.values()) * f
        p_in = vout * iout + p_loss
    return dict(mode=mode, f=f, il=il, i_v=i_v, i_pk=i_pk, ripple=i_pk - i_v, rms=math.sqrt(rms2),
                d=(d1 if mode == "buck" else d2), i_in=p_in / vin, p_loss=p_loss, eff=vout * iout / p_in,
                e={k: v * f for k, v in e.items()})


def dcm_point(vin, vout, iout, f=200e3, t_dt=20e-9, k_core=None, window_ns=4450):
    """Present DCM (200 kHz), rectifier on.  Mode choice as regulator.c (BB band V_in - 2 .. V_in + 7)."""
    tp = 1 / f
    if vout < vin - 2:
        mode, chg, dis, e_out = "buck", vin - vout, vout, None
    elif vout <= vin + 7:
        mode, chg, dis = "bb", vin, vout
    else:
        mode, chg, dis = "boost", vin, vout - vin
    if mode == "buck":
        k = 1 / chg + 1 / dis
    else:
        k = 1 / dis
    ipk = math.sqrt(2 * iout * vout / (L_H * f) / (vout * k)) if mode != "buck" else math.sqrt(2 * iout / (L_H * f * k))
    if mode == "boost":
        # P_out = V_out (0.5 Ipk t2 f) with t2 = L Ipk / (V_out - V_in)  ->  I_out = 0.5 L Ipk^2 f / (V_out - V_in)
        ipk = math.sqrt(2 * iout * dis / (L_H * f))
    if mode == "bb":
        ipk = math.sqrt(2 * iout * dis / (L_H * f))
    t1, t2 = L_H * ipk / chg, L_H * ipk / dis
    if (t1 + t2) * 1e9 > window_ns or ipk > (6.0 if mode == "buck" else 10.0):
        return None
    rms2 = ipk ** 2 * (t1 + t2) * f / 3
    r_path = 2 * RDS + R_DCR + R_IL
    ro = r_out(vout)
    e = {}
    e["conduction (2 FETs + L1 DCR + R5)"] = rms2 * r_path * tp
    e["L1 core"] = core_w(ipk, t1, t2, f, k_core) / f if k_core else 0.0
    if mode == "buck":
        e["turn-on (ring)"] = 0.5 * qoss(FET, vin) * vin
        e["turn-off"] = 0.5 * vin * ipk * T_OFF
        e["dead time"] = vsd(ipk) * ipk * t_dt
        e["refresh"] = qoss(FET, vout) * vout
        n_sw = 4
    elif mode == "bb":
        e["turn-on (ring)"] = 0.5 * (qoss(FET, vin) * vin + qoss(FET, vout) * vout)
        e["turn-off"] = 0.5 * (vin + vout) * ipk * T_OFF
        e["dead time"] = 2 * vsd(ipk) * ipk * t_dt
        e["refresh"] = 0.0
        n_sw = 4
    else:
        e["turn-on (ring)"] = 0.5 * qoss(FET, vout) * vout
        e["turn-off"] = 0.5 * vout * ipk * T_OFF
        e["dead time"] = vsd(ipk) * ipk * t_dt
        e["refresh"] = qoss(FET, vin) * vin
        n_sw = 4
    e["gate drive"] = QG * V_DRV * n_sw
    p_in = vout * iout / 0.95
    for _ in range(5):
        i_in = p_in / vin
        e["input path"] = i_in ** 2 * R_IN_PATH * tp
        e["output path"] = iout ** 2 * ro * tp
        p_loss = sum(e.values()) * f
        p_in = vout * iout + p_loss
    return dict(mode="dcm-" + mode, ipk=ipk, p_loss=p_loss, eff=vout * iout / p_in, i_in=p_in / vin,
                e={k: v * f for k, v in e.items()})


def ccm_best(vin, vout, iout, f, k):
    pts = [p for p in (ccm_point(m, vin, vout, iout, f, k_core=k) for m in ("buck", "band", "boost")) if p]
    return min(pts, key=lambda p: p["p_loss"]) if pts else None


def part_map(a):
    k = core_k(a.core_ref_w)
    print(f"Best CCM mode vs present DCM, 200 kHz, dead time 20 ns, core ref {a.core_ref_w:g} W.  *model*")
    print(f"I_out capped by the input path ({I_IN_MAX:g} A in).  '-' = mode cannot reach the point.")
    hdr = (f"{'Vin':>4} {'Vout':>4} {'Iout':>5} {'P W':>5} | {'CCM':>5} {'D':>5} {'I_L':>5} {'dI':>5} {'Ipk':>5} "
           f"{'loss':>5} {'eff':>6} | {'DCM':>8} {'Ipk':>5} {'loss':>5} {'eff':>6} | pick")
    for vin in VINS:
        print("\n" + hdr)
        for vout, imax in PDOS:
            for frac in (0.1, 0.25, 0.5, 0.75, 1.0):
                iout = imax * frac
                c = ccm_best(vin, vout, iout, 200e3, k)
                if c and c["i_in"] > I_IN_MAX:
                    continue
                d = dcm_point(vin, vout, iout, 200e3, k_core=k)
                if not c and not d:
                    print(f"{vin:4.0f} {vout:4.0f} {iout:5.2f} {vout * iout:5.1f} | unreachable")
                    continue
                cs = (f"{c['mode']:>5} {c['d']:5.3f} {c['il']:5.2f} {c['ripple']:5.2f} {c['i_pk']:5.2f} "
                      f"{c['p_loss']:5.2f} {c['eff'] * 100:5.1f}%") if c else f"{'-':>5} {'':>29}"
                ds = (f"{d['mode']:>8} {d['ipk']:5.2f} {d['p_loss']:5.2f} {d['eff'] * 100:5.1f}%") if d else f"{'-':>8} {'':>18}"
                pick = "CCM" if (c and (not d or c["p_loss"] < d["p_loss"])) else "DCM"
                print(f"{vin:4.0f} {vout:4.0f} {iout:5.2f} {vout * iout:5.1f} | {cs} | {ds} | {pick}")


def crossover(a):
    """Lowest load at which CCM loses less than DCM, per (V_in, V_out)."""
    k = core_k(a.core_ref_w)
    print(f"\nCCM-over-DCM crossover: lowest I_out [A] where CCM loss < DCM loss (200 kHz).  *model*")
    print(f"{'Vin':>4} " + " ".join(f"{v:>12}" for v, _ in PDOS))
    out = {}
    for vin in VINS:
        row = []
        for vout, imax in PDOS:
            x = None
            for iout in np.arange(0.05, imax + 1e-9, 0.05):
                c = ccm_best(vin, vout, iout, 200e3, k)
                if not c or c["i_in"] > I_IN_MAX:
                    continue
                d = dcm_point(vin, vout, iout, 200e3, k_core=k)
                if not d or c["p_loss"] < d["p_loss"]:
                    x = (iout, c["mode"], d is None)
                    break
            out[(vin, vout)] = x
            row.append(f"{x[0]:5.2f} {x[1]:>5}{'*' if x[2] else ' '}" if x else f"{'none':>12}")
        print(f"{vin:4.0f} " + " ".join(f"{r:>12}" for r in row))
    print("  * = DCM's ceiling (window/peak) ends first: CCM from there is required, not chosen.")
    return out


def boost_loop(a):
    print("\nBoost CCM voltage loop: u -> V_out with D = 1 - V_in/(V_ref + u), tau 63 us, board ceramics only.  *model*")
    print(f"{'Vin':>4} {'Vout':>4} {'R':>6} {'D':>5} {'f_LC kHz':>8} {'RHPz kHz':>8} {'ki':>6} {'f_c Hz':>7} {'PM':>5} "
          f"{'min|1+T|':>8}")
    f = np.logspace(1, np.log10(9.9e3), 4000)
    s = 2j * np.pi * f
    for vin, vout, r in ((24, 28, 25.3), (24, 28, 5.6), (20, 28, 5.6), (15, 20, 4.0), (10, 20, 4.0), (10, 28, 9.3),
                         (12, 15, 5.0), (10, 15, 5.0)):
        d = 1 - vin / vout
        c = 5 * c_mlcc(vout)
        le = L_H / (1 - d) ** 2
        rd = R_DCR + R_IL + 2 * RDS
        f_lc = 1 / (2 * np.pi * np.sqrt(le * c)) / 1e3
        f_z = r * (1 - d) ** 2 / (2 * np.pi * L_H) / 1e3
        plant = (1 - s * le / r) / (1 + s * (le / r + rd * c) + s ** 2 * le * c)
        for ki in (3142.0, 1571.0):
            t = (ki / s) * plant * np.exp(-s * 63e-6)
            mag, ph = np.abs(t), np.unwrap(np.angle(t))
            ic = np.where(mag >= 1)[0]
            fc = f[ic[-1]] if ic.size else float("nan")
            pm = 180 + np.degrees(ph[ic[-1]]) if ic.size else float("nan")
            mm = np.min(np.abs(1 + t))
            print(f"{vin:4.0f} {vout:4.0f} {r:6.1f} {d:5.3f} {f_lc:8.2f} {f_z:8.1f} {ki:6.0f} {fc:7.0f} {pm:5.0f} {mm:8.2f}")


def terms(a):
    k = core_k(a.core_ref_w)
    print("\nEvery term, selected points [W].  *model*")
    for vin, vout, iout in ((24, 28, 3.0), (24, 28, 5.0), (20, 20, 3.0), (15, 15, 3.0), (10, 9, 3.0), (10, 20, 3.0),
                            (12, 20, 3.0), (15, 28, 3.0)):
        c = ccm_best(vin, vout, iout, 200e3, k)
        d = dcm_point(vin, vout, iout, 200e3, k_core=k)
        print(f"  {vin} V -> {vout} V {iout} A:")
        for p in (c, d):
            if p:
                print(f"    {p['mode']:>9} loss {p['p_loss']:5.2f}  " + ", ".join(f"{n} {v:.2f}" for n, v in p["e"].items()))


def freq(a):
    k = core_k(a.core_ref_w)
    print("\nBoost / band CCM: 200 vs 400 kHz loss [W].  *model*")
    for vin, vout, iout in ((24, 28, 1.0), (24, 28, 3.0), (20, 20, 1.0), (20, 20, 3.0), (10, 20, 2.0), (15, 28, 2.0)):
        r = []
        for f in (200e3, 400e3):
            c = ccm_best(vin, vout, iout, f, k)
            r.append(f"{c['mode']} {c['p_loss']:.2f} W dI {c['ripple']:.2f} A" if c else "-")
        print(f"  {vin} -> {vout} V {iout} A: " + "  |  ".join(r))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--core-ref-w", type=float, default=1.0)
    a = p.parse_args()
    part_map(a)
    crossover(a)
    terms(a)
    freq(a)
    boost_loop(a)


if __name__ == "__main__":
    main()
