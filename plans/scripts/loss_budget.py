#!/usr/bin/env python3
"""Loss budget per switching period for the PD_Charger power stage (plans/240w-roadmap.md).

Forced CCM at 200 kHz from a 24 V input, one line per USB PD fixed PDO of a 240 W source.
Prints energy per switching period (uJ) and watts by mechanism, for EPC2308 and EPC2302,
at the present 50 ns dead time and a 15 ns target, with the static leg refreshed every
period and every 8th.  Also prints the present DCM modulator's ceiling per contract
(regulator.c PID ISR step 5) against the 25 ohm bench load.

Sources (values typed in, not fitted):
- EPC2308 (rev. June 2026) and EPC2302 (rev. April 2026) datasheets: RDS(on) typ 4.5 / 1.4 mOhm
  at 25 C, x1.4 / x1.5 at about 100 C (Fig. 9); QOSS vs VDS read off Fig. 6; QG 11.7 / 23 nC;
  third-quadrant drop about 2 V plus 25 mV/A (Fig. 8).  EPC2302 is the planned final part
  (100 V); EPC2308 (150 V) is fitted now.
- IHLP6767GZER4R7M11: DCR 3.98 mOhm typ at 25 C (x1.25 for a hot winding), Isat 21 A.
- KiCad: R5 5 mOhm (IL_MON) in series with L1; R25 5 mOhm as built (schematic 2 mOhm);
  R6 12 mOhm; Q5-Q8 SiR5607DP, 5.6 mOhm typ at Vgs -10 V, 9.2 at -4.5 V (x1.3 hot).
Left out: inductor core loss, capacitor ESR, PCB copper and connectors, driver quiescent.
"""

F_SW = 200e3
T_SW = 1.0 / F_SW
L_H = 4.7e-6
V_IN = 24.0
R_DCR = 3.98e-3 * 1.25
R_IL_SHUNT = 5e-3          # R5
R_IN_SHUNT = 5e-3          # R25 as built
R_OUT_SHUNT = 12e-3        # R6
T_OV_ON = 3e-9             # V-I overlap of a hard turn-on
T_OFF = 1e-9               # channel turn-off before the node moves (current-driven edge)
V_DRV = 5.0

FETS = {
    "EPC2308": dict(rds=4.5e-3 * 1.4, qg=11.7e-9,
                    qoss=[(0, 0), (12.5, 13), (25, 24), (50, 37), (75, 50), (100, 57), (150, 72)]),
    "EPC2302": dict(rds=1.4e-3 * 1.5, qg=23e-9,
                    qoss=[(0, 0), (10, 33), (25, 55), (50, 85), (75, 110), (100, 133)]),
}

PDOS = [(5, 3), (9, 3), (15, 3), (20, 5), (28, 5), (36, 5), (48, 5)]


def interp(pts, x):
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


def qoss(fet, v):
    return interp(FETS[fet]["qoss"], v) * 1e-9


def vsd(i):
    return 2.0 + 0.025 * abs(i)


def pfet_rds(vout):
    """SiR5607DP behind the output-switch clamp (Vgs = -0.84 Vout, clamped at -13 V), hot."""
    vgs = min(0.84 * vout, 13.0)
    r = 5.6e-3 if vgs >= 10 else 5.6e-3 + (9.2e-3 - 5.6e-3) * (10 - vgs) / 5.5
    if vgs < 4.5:
        r = 9.2e-3 * 4.5 / vgs
    return r * 1.3


R_IN_PATH = 2 * 5.6e-3 * 1.3 + R_IN_SHUNT    # Q7 + Q8 at Vgs about -12 V (ADM1270), + R25


def point(fet, vout, iout, t_dt, n_refresh):
    rds = FETS[fet]["rds"]
    pout = vout * iout
    r_out = 2 * pfet_rds(vout) + R_OUT_SHUNT
    loss = {}
    p_in = pout / 0.95
    for _ in range(6):
        i_in = p_in / V_IN
        if vout < V_IN - 2:                   # buck: Q1 active, Q2 rectifier, Q3 static, Q4 refresh
            d = vout / V_IN
            i_avg = iout
            ripple = (V_IN - vout) * d * T_SW / L_H
            v_sw = V_IN
            v_leg_static = vout
            i_active2, i_rect2 = d, 1 - d
        else:                                 # boost: Q4 active, Q3 rectifier, Q1 static, Q2 refresh
            d = 1 - V_IN / vout
            i_avg = i_in
            ripple = V_IN * d * T_SW / L_H
            v_sw = vout
            v_leg_static = V_IN
            i_active2, i_rect2 = d, 1 - d
        i_pk = i_avg + ripple / 2
        i_v = i_avg - ripple / 2
        irms2 = i_avg ** 2 + ripple ** 2 / 12
        q = qoss(fet, v_sw)
        e = {}
        e["FET conduction"] = irms2 * rds * (1 + i_active2 + i_rect2) * T_SW   # static + active + rectifier
        e["L1 DCR + R5"] = irms2 * (R_DCR + R_IL_SHUNT) * T_SW
        soft = i_v < 0 and 2 * q / abs(i_v) <= t_dt
        e["hard turn-on (Qoss V + overlap)"] = 0.0 if soft else q * v_sw + 0.5 * v_sw * max(i_v, 0) * T_OV_ON
        e["turn-off at peak"] = 0.5 * v_sw * i_pk * T_OFF
        e["dead time (3rd quadrant)"] = (vsd(i_pk) * i_pk + vsd(i_v) * abs(i_v)) * t_dt
        e["static-leg refresh"] = (qoss(fet, v_leg_static) * v_leg_static
                                   + 0.5 * v_leg_static * i_avg * T_OV_ON
                                   + 2 * vsd(i_avg) * i_avg * t_dt) / n_refresh
        e["gate drive"] = FETS[fet]["qg"] * V_DRV * (2 + 2 / n_refresh)
        e["input path (Q7, Q8, R25)"] = i_in ** 2 * R_IN_PATH * T_SW
        e["output path (Q5, Q6, R6)"] = iout ** 2 * r_out * T_SW
        p_loss = sum(e.values()) * F_SW
        p_in = pout + p_loss
        loss = e
    return dict(mode="buck" if vout < V_IN - 2 else "boost", d=d, i_avg=i_avg, ripple=ripple,
                i_pk=i_pk, i_v=i_v, irms=irms2 ** 0.5, i_in=p_in / V_IN, e=loss,
                p_loss=sum(loss.values()) * F_SW, eff=pout / p_in)


def dcm_ceiling(vout, v_in=24.6):
    """regulator.c step 5 with the rectifier fully faded in (no diode term)."""
    window = 5000 - (200 + 50) - 300
    if vout < v_in - 3:
        chg, dis, k_buck, pk_max = v_in - vout, vout, True, 6.0
    elif vout <= v_in + 7:
        chg, dis, k_buck, pk_max = v_in, vout, False, 10.0
    else:
        chg, dis, k_buck, pk_max = v_in, vout - v_in, False, 10.0
    k = 1 / chg + 1 / dis if k_buck else 1 / dis
    t_lim = min(window * dis / (chg + dis), 4000) * 1e-9
    i_pk = min(t_lim * chg / L_H, pk_max)
    i_max = 0.5 * L_H * i_pk ** 2 * k / T_SW
    mode = "buck" if k_buck else ("buck-boost" if vout <= v_in + 7 else "boost")
    return mode, t_lim * 1e9, i_pk, i_max


def main():
    import argparse
    global V_IN
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--vin", type=float, default=V_IN, help="input voltage, V (default 24)")
    V_IN = ap.parse_args().vin
    print("Present DCM modulator ceiling (model units), V_in 24.6 V, rectifier on")
    print(f"{'V':>4} {'mode':>10} {'t_lim ns':>9} {'Ipk A':>6} {'Imax A':>7} {'Pmax W':>7} {'25R W':>6}")
    for v in (5, 9, 15, 20, 24, 28, 36, 48):
        mode, t_lim, i_pk, i_max = dcm_ceiling(v)
        print(f"{v:>4} {mode:>10} {t_lim:9.0f} {i_pk:6.2f} {i_max:7.2f} {i_max * v:7.1f} {v * v / 25:6.1f}")
    print()
    print(f"Forced CCM, V_in {V_IN:g} V, 200 kHz, 4.7 uH")
    for fet in FETS:
        for t_dt, n_ref in ((50e-9, 1), (15e-9, 8)):
            print(f"{fet}, dead time {t_dt * 1e9:.0f} ns, static leg refreshed every {n_ref} period(s)")
            print(f"{'PDO':>9} {'mode':>5} {'D':>5} {'I_L':>5} {'dI':>5} {'Ipk':>5} {'Ival':>5} {'Iin':>5}"
                  f" {'uJ/per':>7} {'W':>5} {'eff':>6}  largest terms (W)")
            for v, i in PDOS:
                r = point(fet, v, i, t_dt, n_ref)
                top = sorted(r["e"].items(), key=lambda kv: -kv[1])[:4]
                tops = ", ".join(f"{k} {e * F_SW:.2f}" for k, e in top)
                print(f"{v:>3} V {i} A {r['mode']:>5} {r['d']:5.2f} {r['i_avg']:5.1f} {r['ripple']:5.1f}"
                      f" {r['i_pk']:5.1f} {r['i_v']:5.1f} {r['i_in']:5.1f} {r['p_loss'] / F_SW * 1e6:7.1f}"
                      f" {r['p_loss']:5.2f} {r['eff'] * 100:5.1f}%  {tops}")
            print()
    r = point("EPC2308", 48, 5, 15e-9, 8)
    print("48 V 5 A, EPC2308, 15 ns, refresh /8, every term (uJ per period, W):")
    for k, e in r["e"].items():
        print(f"  {k:34s} {e * 1e6:6.2f} uJ  {e * F_SW:5.2f} W")


if __name__ == "__main__":
    main()
