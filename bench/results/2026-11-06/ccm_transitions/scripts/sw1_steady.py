"""Steady-state SW1 (CH1 alone, 1 GSa/s, 10 us/div, 120k points, BWL off) in DCM (ccm_enable 1) or forced CCM (2),
and the inductor waveform estimated from it (L1 = 4.7 uH, V_in from VS_MON, V_out from VD_MON):
  CCM: high time t_h (SW1 > V_in/2) -> dI = (V_in - V_out) t_h / L.  The dead-time rise before Q1 (ZVS, driven by the
       valley current -I_v) and the fall at Q1 turn-off (driven by I_pk) cross the same 20-80 % band of V_in, so
       t_rise / t_fall = I_pk / (-I_v) (same node charge both ways; inference: assumes both edges are current-driven)
       -> I_v = -dI t_fall / (t_rise + t_fall)... solved below, mean = I_v + dI / 2.
  DCM: t_on (SW1 high) and t_off (Q1 off -> SW1 rises through V_out / 2 at the zero-current ring);
       I_pk = (V_in - V_out) t_on / L, mean = I_pk (t_on + t_off) / (2 T).
usage: sw1_steady.py CCM_ENABLE TAG     appends to ../data/sw1_steady.jsonl"""
import csv, json, pathlib, statistics as st, sys, time
sys.path.insert(0, "/home/daniel/GitHub/PD_Charger/bench/results/2026-11-03/scripts")
from benchlib import ROOT, bu, w, status_n
L = 4.7e-6
OUT = pathlib.Path(__file__).resolve().parents[1] / "data" / "sw1_steady.jsonl"


def capture(vin):
    for c in ("2", "3", "4"):
        bu("scope", "chan", c, "--off")
    bu("scope", "chan", "1", "--on", "--probe", "10", "--scale", "5", "--offset", "-12", "--coupling", "DC", "--bwl", "OFF")
    bu("scope", "trigger", "--source", "CH1", "--slope", "POS", "--level", round(vin / 2, 2), "--coupling", "DC", "--sweep", "NORM")
    bu("scope", "timebase", "--scale", "1e-5", "--offset", "5e-5")
    bu("scope", "run"); time.sleep(0.4)
    bu("scope", "acquire", "--mdepth", "120k", "--type", "NORM")
    bu("scope", "trigger", "--source", "CH1", "--slope", "POS", "--level", round(vin / 2, 2), "--coupling", "DC", "--sweep", "SING")
    bu("scope", "single"); time.sleep(0.4)
    bu("scope", "wait", "--no-arm", "--timeout", 3)
    c = bu("scope", "capture", "CH1", "--raw", t=60)
    rows = [[float(x) for x in r] for r in list(csv.reader(open(f"{ROOT}/{c['file']}")))[1:]]
    return c["file"], [r[0] for r in rows], [r[1] for r in rows]


def cross(t, v, lvl, i0, up):
    for i in range(i0, len(v) - 1):
        if (v[i] < lvl <= v[i + 1]) if up else (v[i] > lvl >= v[i + 1]):
            return i, t[i] + (lvl - v[i]) / (v[i + 1] - v[i]) * (t[i + 1] - t[i])
    return None, None


def analyse(t, v, vin, vout, ccm):
    lo, mid, hi = 0.2 * vin, 0.5 * vin, 0.8 * vin
    out = []; i = 0
    while True:
        i, tr50 = cross(t, v, mid, i, True)
        if i is None:
            break
        _, tr20 = cross(t, v, lo, max(0, i - 200), True)
        _, tr80 = cross(t, v, hi, max(0, i - 200), True)
        j, tf50 = cross(t, v, mid, i + 1, False)
        if j is None:
            break
        _, tf80 = cross(t, v, hi, max(0, j - 200), False)
        _, tf20 = cross(t, v, lo, max(0, j - 200), False)
        if tf50 - tr50 < 300e-9:   # the DCM zero-current ring crossing V_in / 2, not a Q1 pulse
            i = j + 1; continue
        rec = dict(t_h=tf50 - tr50, t_rise=(tr80 - tr20) if tr80 and tr20 else None,
                   t_fall=(tf20 - tf80) if tf80 and tf20 else None)
        if not ccm:
            k0 = next((q for q in range(j, len(v)) if v[q] < 2.0), None)   # SW1 down after Q1 off
            k, tz = cross(t, v, vout / 2, k0, True) if k0 is not None else (None, None)   # zero-current ring start
            rec["t_off"] = (tz - tf50) if k else None
        out.append(rec); i = j + 1
    return out


def main():
    ccm, tag = int(sys.argv[1]), sys.argv[2]
    w("regulator_ccm_enable", ccm, "u8"); time.sleep(1.0)
    s = status_n(6, 0.1); vin, vout = s["vin"] / 1000, s["v"] / 1000
    f, t, v = capture(vin)
    ps = analyse(t, v, vin, vout, ccm == 2)
    T = 5e-6
    m = lambda k: st.median([p[k] for p in ps if p.get(k)]) if any(p.get(k) for p in ps) else None
    r = dict(tag=tag, ccm_enable=ccm, ccm_seen=s["ccm_seen"], file=f, v_in=vin, v_out=vout, i_v_over_r_ma=round(vout / 25.3 * 1e3, 1),
             id_mon_raw_ma=s["iout"], n_periods=len(ps), t_h_ns=round(m("t_h") * 1e9, 1), t_rise_ns=round(m("t_rise") * 1e9, 2) if m("t_rise") else None,
             t_fall_ns=round(m("t_fall") * 1e9, 2) if m("t_fall") else None, ton_cmd_ns=s["ton"])
    if ccm == 2:
        dI = (vin - vout) * m("t_h") / L
        r["dI_pp_a"] = round(dI, 3)
        if m("t_rise") and m("t_fall"):
            # t_rise/t_fall = I_pk/(-I_v), I_pk = I_v + dI  ->  I_v = -dI t_fall / (t_rise + t_fall)
            iv = -dI * m("t_fall") / (m("t_rise") + m("t_fall"))
            r.update(i_valley_a=round(iv, 3), i_peak_a=round(iv + dI, 3), i_mean_wave_ma=round((iv + dI / 2) * 1e3, 1))
    else:
        ton, toff = m("t_h"), m("t_off")
        if toff:
            ipk = (vin - vout) * ton / L
            r.update(t_off_ns=round(toff * 1e9, 1), i_pk_a=round(ipk, 3), i_pk_from_toff_a=round(vout * toff / L, 3),
                     i_mean_wave_ma=round(ipk * (ton + toff) / (2 * T) * 1e3, 1))
    print(json.dumps(r)); sys.stdout.flush()
    with open(OUT, "a") as fh:
        fh.write(json.dumps(r) + "\n")
    w("regulator_ccm_enable", 1, "u8")


if __name__ == "__main__":
    main()
