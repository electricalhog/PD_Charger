"""Fill the gen: blocks of README.md from metrics/sw_periods.json, metrics/sw_nodes.json and data/status_*.jsonl."""
import json, re, statistics as st, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
T_SW, L_H, R_PATH = 5e-6, 4.7e-6, 0.0226
COND = [("28v_sr1_0w", "SR on, 25.3 Ω only"), ("28v_sr1_3w", "SR on, + buck 3 W"),
        ("28v_sr1_25w", "SR on, + buck 25 W, BB peak limit 12.3 A"), ("28v_sr0_0w", "SR off, 25.3 Ω only")]
M = lambda x: f"{x}".replace("-", "−")


def periods():
    P = json.loads((ROOT / "metrics/sw_periods.json").read_text())
    out = ["| Condition | Periods | t1 charge (µs) | t2 transfer (µs) | Ring (µs) | I_pk at 4.7 µH (A) | I_L rms (A) | ½·L·I_pk²·f (W) | SW1 before Q1 on (V) | SW2 plateau (V) | SW2 peak at Q4 off (V) | Ring f (MHz) | C_eff (pF) | Ring amplitude, first half-cycle (V) |",
           "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for k, name in COND:
        r = P[k]; md = lambda x: r[x]["median"] if r[x] else None
        t1, t2, ipk = md("t1_us"), md("t2_us"), md("ipk_a")
        irms = ipk * ((t1 + t2) * 1e-6 / (3 * T_SW)) ** 0.5
        pl = 0.5 * L_H * ipk ** 2 / T_SW
        f = md("ring_f_hz")
        out.append(f"| {name} | {r['periods_parsed']} | {t1:.2f} | {t2:.2f} | {md('ring_us'):.2f} | {ipk:.2f} | {irms:.2f} | {pl:.1f} | "
                   f"{md('sw1_before_on_v'):.1f} | {md('sw2_plateau_v'):.1f} | {md('sw2_peak_v'):.1f} | "
                   f"{f/1e6:.2f} | {r.get('ring_c_eff_pf')} | {md('ring_a_first_v'):.1f} |" if f else
                   f"| {name} | {r['periods_parsed']} | {t1:.2f} | {t2:.2f} | {md('ring_us'):.2f} | {ipk:.2f} | {irms:.2f} | {pl:.1f} | "
                   f"{md('sw1_before_on_v'):.1f} | {md('sw2_plateau_v'):.1f} | {md('sw2_peak_v'):.1f} | — (ring < 2 periods) | — | — |")
    out.append("")
    out.append("Medians over the record (1.2 ms). V_in 24.0 V (23.85 V at 25 W, regulator ADC) for I_pk = V_in·t1/L. Scope voltages uncorrected (§5, U2).")
    return "\n".join(out)


def edges():
    N = json.loads((ROOT / "metrics/sw_nodes.json").read_text())
    out = ["| Condition | Node | Hard rises / falls | Rise 10–90 % (ns) | Rise peak dV/dt (V/ns) | Overshoot above p95 level (V) | Fall 10–90 % (ns) | Fall peak dV/dt (V/ns) | Undershoot below p5 level (V) | Min / max (V) |",
           "|---|---|---|---|---|---|---|---|---|---|"]
    for k, name in COND:
        r = N["sw_" + k[0:]] if ("sw_" + k) in N else N["sw_" + k]
        for ch in ("SW1", "SW2"):
            c = r[ch]
            g = lambda e, x: (f"{c[e][x]['median']:.1f}" if c[e][x] else "—")
            n = lambda e: (c[e]["t_10_90_ns"]["n"] if c[e]["t_10_90_ns"] else 0)
            out.append(f"| {name} | {ch} | {n('rise')} / {n('fall')} | {g('rise','t_10_90_ns')} | {g('rise','dvdt_max_v_per_ns')} | {M(g('rise','overshoot_v'))} | "
                       f"{g('fall','t_10_90_ns')} | {g('fall','dvdt_max_v_per_ns')} | {M(g('fall','undershoot_v'))} | {M(c['min_v'])} / {c['max_v']} |")
    out.append("")
    out.append("Medians per edge. Hard edge: peak dV/dt ≥ 1.5 V/ns (2 ns samples); slower crossings are the L-C ring and are excluded. "
               "10–90 % is between the record's p5 and p95 levels, so SW1 rises that start from the ring or end in a slow approach read 40–53 ns despite 3–6 V/ns peaks. "
               "Edges near 5 ns are at the 100 MHz scope bandwidth (rise ≈ 3.5 ns); overshoot and dV/dt are lower bounds. 8-bit at 10 V/div: 0.31 V per code.")
    return "\n".join(out)


def spectra():
    N = json.loads((ROOT / "metrics/sw_nodes.json").read_text())
    bands = list(next(iter(N.values()))["SW1"]["spectrum_bands"].keys())
    out = ["| Condition | Node | " + " | ".join(f"{b} peak (dBµV)" for b in bands) + " |", "|---|---|" + "---|" * len(bands)]
    for k, name in COND:
        r = N["sw_" + k]
        for ch in ("SW1", "SW2"):
            c = r[ch]
            b = " | ".join(f"{c['spectrum_bands'][x]['peak_dbuv']} at {c['spectrum_bands'][x]['at_hz']/1e6:.2f} MHz" for x in bands)
            out.append(f"| {name} | {ch} | {b} |")
    out.append("")
    out.append(f"Hann window over the whole 1.2 ms record, peak amplitude per {next(iter(N.values()))['SW1']['bin_hz']} Hz bin, 1 µV reference. "
               "Switch-node voltage: the noise source, not a conducted-emission measurement (no LISN).")
    return "\n".join(out)


def status():
    out = ["| Condition | Reads | I_cmd min / mean / max (mA) | I_max (mA) | I_cmd / I_max (mean) | V_out mean (mV) | V_in mean (mV) |", "|---|---|---|---|---|---|---|"]
    for k, name in (("sr1_0w", "SR on, 25.3 Ω only"), ("sr1_3w", "SR on, + buck 3 W (run A)"), ("sr0_0w", "SR off, 25.3 Ω only")):
        rows = [json.loads(x) for x in (ROOT / f"data/status_28v_{k}.jsonl").read_text().split("\n\n") if x.strip()]
        ic = [r["control"]["i_cmd_ma"] for r in rows]; im = [r["control"]["i_max_ma"] for r in rows]
        out.append(f"| {name} | {len(rows)} | {min(ic):.0f} / {st.mean(ic):.0f} / {max(ic):.0f} | {min(im):.0f}–{max(im):.0f} | {st.mean(ic)/st.mean(im)*100:.0f} % | "
                   f"{st.mean(r['adc']['v_out_mv'] for r in rows):.0f} | {st.mean(r['adc']['v_in_mv'] for r in rows):.0f} |")
    return "\n".join(out)


BLOCKS = {"periods": periods, "edges": edges, "spectra": spectra, "status": status}

if __name__ == "__main__":
    p = ROOT / "README.md"
    t = p.read_text(encoding="utf-8")
    for name, fn in BLOCKS.items():
        pat = re.compile(rf"(<!-- BEGIN gen:{name} -->\n)(.*?)(<!-- END gen:{name} -->)", re.S)
        if not pat.search(t):
            sys.exit(f"missing block {name}")
        t = pat.sub(lambda m: m.group(1) + fn() + "\n" + m.group(3), t)
    p.write_text(t, encoding="utf-8")
    print("tables written")
