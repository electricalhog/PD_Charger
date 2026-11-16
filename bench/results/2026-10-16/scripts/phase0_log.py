#!/usr/bin/env python3
"""Phase 0 logger (plans/240w-roadmap.md): board ADC reads per step, with the bench readings.

Takes N `bu regulator status` reads (read-only, about 0.5 s each), averages the board's ADC
and loop telemetry, and appends one JSON line per step to data/phase0.jsonl, next to whatever
was read off the bench: supply current, the meters, temperatures.  The board's current reads are
single samples per PID period (adc_monitor.h), so one read is noisy; the mean of 20 is not.

  phase0_log.py --label "28V sr1" --vin 24.41 --iin 1.62 --vbus 27.95 --r 25.1 \
                --temp Q1=48 --temp L1=41 --temp amb=23

--vin: meter at the board's input (XT60), V.   --iin: supply current, A.
--vbus: meter at the 25 ohm, V.                 --r: the load's resistance, ohm.
--iout: measured output current, A; without it the output current is V_out / R.
Efficiency is printed from the bench readings when they are given, and from the ADC otherwise.
"""
import argparse
import json
import statistics as st
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BU = ROOT / "tools" / "bringup" / "bu"
OUT = Path(__file__).resolve().parents[1] / "data" / "phase0.jsonl"
KEYS_ADC = ["v_in_mv", "v_out_mv", "i_in_ma", "i_out_ma", "i_inductor_ma"]
KEYS_CTL = ["vin_filtered_mv", "i_cmd_ma", "i_max_ma", "i_pk_ma", "on_time_ns", "sr_on_ns", "sr_off_ns"]


def read_status():
    r = subprocess.run([str(BU), "regulator", "status"], capture_output=True, text=True, timeout=30)
    return json.loads(r.stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--label", required=True)
    ap.add_argument("--n", type=int, default=20, help="status reads to average (default 20)")
    ap.add_argument("--vin", type=float, help="meter at the board input, V")
    ap.add_argument("--iin", type=float, help="supply current, A")
    ap.add_argument("--vbus", type=float, help="meter at the load, V")
    ap.add_argument("--iout", type=float, help="measured output current, A (else V_out / R)")
    ap.add_argument("--r", type=float, default=25.0, help="load resistance, ohm (default 25)")
    ap.add_argument("--idle-w", type=float, default=0.0,
                    help="board housekeeping input power with the regulator idle, W (subtracted for the converter loss)")
    ap.add_argument("--temp", action="append", default=[], metavar="NAME=C")
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    reads, t0 = [], time.time()
    for _ in range(a.n):
        s = read_status()
        if s.get("state") != "RUNNING":
            print(f"regulator is {s.get('state')} {s.get('fault_source')}: stopping", file=sys.stderr)
            break
        c, d = s.get("control", {}), s.get("adc", {})
        reads.append({"mode": c.get("mode"), "sr_enable": c.get("sr_enable"), "sr_blend": c.get("sr_blend"),
                      **{k: d.get(k) for k in KEYS_ADC}, **{k: c.get(k) for k in KEYS_CTL}})
    rec = {"label": a.label, "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t0)),
           "seconds": round(time.time() - t0, 1), "n": len(reads), "note": a.note,
           "bench": {"vin_v": a.vin, "iin_a": a.iin, "vbus_v": a.vbus, "iout_a": a.iout, "r_ohm": a.r, "idle_w": a.idle_w,
                     "temps_c": {k: float(v) for k, v in (t.split("=", 1) for t in a.temp)}}}
    if reads:
        mean = {k: round(st.fmean(r[k] for r in reads if r[k] is not None), 1)
                for k in KEYS_ADC + KEYS_CTL if any(r[k] is not None for r in reads)}
        sd = {k: round(st.pstdev(r[k] for r in reads if r[k] is not None), 1) for k in mean}
        rec["adc_mean"], rec["adc_sd"] = mean, sd
        rec["mode"] = sorted({r["mode"] for r in reads})
        rec["sr"] = sorted({(r["sr_enable"], r["sr_blend"]) for r in reads})
        rec["adc_p_in_w"] = round(st.fmean(r["v_in_mv"] * r["i_in_ma"] for r in reads) * 1e-6, 2)
        rec["adc_p_out_w"] = round(st.fmean(r["v_out_mv"] * r["i_out_ma"] for r in reads) * 1e-6, 2)
    b = rec["bench"]
    if b["vin_v"] and b["iin_a"]:
        rec["p_in_w"] = round(b["vin_v"] * b["iin_a"], 2)
    if b["vbus_v"]:
        rec["p_out_w"] = round(b["vbus_v"] * (b["iout_a"] if b["iout_a"] is not None else b["vbus_v"] / b["r_ohm"]), 2)
    if "p_in_w" in rec and "p_out_w" in rec:
        rec["eff_pct"] = round(100 * rec["p_out_w"] / rec["p_in_w"], 1)
        rec["loss_w"] = round(rec["p_in_w"] - rec["p_out_w"] - b["idle_w"], 2)
        if "adc_mean" in rec and rec["adc_mean"].get("v_out_mv"):
            i_load = 1e3 * (b["iout_a"] if b["iout_a"] is not None else b["vbus_v"] / b["r_ohm"])
            rec["cmd_over_load"] = round(rec["adc_mean"]["i_cmd_ma"] / i_load, 3)
    OUT.parent.mkdir(exist_ok=True)
    with OUT.open("a") as f:
        f.write(json.dumps(rec) + "\n")
    show = {k: rec.get(k) for k in ("label", "n", "mode", "sr", "p_in_w", "p_out_w", "eff_pct", "loss_w",
                                    "adc_p_in_w", "adc_p_out_w", "cmd_over_load")}
    print(json.dumps(show))
    if "adc_mean" in rec:
        print(json.dumps({"adc_mean": rec["adc_mean"], "adc_sd": rec["adc_sd"]}))
    print(f"appended to {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
