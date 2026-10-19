#!/usr/bin/env python3
"""Load drop and reconnect at the present contract, recorded by the 20 kHz regulator trace.

Needs a G474 build with the regulator trace and the TIM4-timed SWITCH_PULSE (2026-10-17).
For each open time in --open-ms (shortest first), --repeat times:
1. checks the regulator is RUNNING at --contract-mv with the output switch closed;
2. `bu regulator trace-arm --trigger switch --post N`;
3. `bu regulator switch-pulse MS`: the PID ISR drops OUTPUT_EN, TIM4 raises it MS later (1 us
   ticks; the trace reports the measured time), so the bench side (25 ohm, QT Py) drops off and
   comes back;
4. `bu regulator trace` once the trace is DONE; the CSV goes to --out;
5. figures from the trace: V_out before, rise_ms (V_out rising out of the pre-trigger noise;
   superseded by the ensemble T_release), skipping, the peak while open (dV_os), the reconnect
   undershoot dV_us (keys dip_mv, dip_pct, dip_at_ms_after_reconnect), the time back within 1 %
   and 2 %, I_cmd against its ceiling, skipping and at-limit samples.
It stops at the first fault, and before any longer open time once dV_us exceeds --max-dip-pct.
The contract is not changed here: request it first (bu sink).

The load only leaves the node T_release after OUTPUT_EN falls: measured 1.35 ms at 5 V, 4.15 ms
at 20 V, 3.90 ms at 28 V (4.20 ms with SR on), +-0.15 ms (report/TEST_REPORT.md section 6.1).
Open times shorter than that leave the load on the node (sub-release pulses). The cause (turn-off
through the 51 kohm gate pull-up) is inferred; the gates were not probed (report U3).

  switch_step.py --contract-mv 28000 --open-ms 4.25,4.5,4.75,5.0 --out bench/results/<day>/data/steps
"""
import argparse
import csv
import json
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BU = ROOT / "tools" / "bringup" / "bu"


def bu(*args, timeout=60):
    r = subprocess.run([str(BU), *args], capture_output=True, text=True, timeout=timeout)
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        raise SystemExit(f"bu {' '.join(args)}: no JSON\n{r.stdout[-800:]}\n{r.stderr[-800:]}")


def check_running(contract_mv):
    s = bu("regulator", "status")
    c = s.get("control", {})
    problems = []
    if s.get("state") != "RUNNING":
        problems.append(f"state {s.get('state')} {s.get('fault_source')}")
    if c.get("target_mv") != contract_mv:
        problems.append(f"target {c.get('target_mv')} mV, expected {contract_mv}")
    if not s.get("lines", {}).get("OUTPUT_EN_PC8"):
        problems.append("output switch open")
    return s, problems


def metrics(rows, v_set, pulse_ms):
    t = [float(r["t_ms"]) for r in rows]
    v = [int(r["v_out_mv"]) for r in rows]
    sw = [int(r["switch_on"]) for r in rows]
    skip = [int(r["skipping"]) for r in rows]
    pre = [v[k] for k in range(len(rows)) if t[k] < 0]
    v0 = sum(pre) / len(pre) if pre else float(v_set)
    sd = statistics.pstdev(pre) if len(pre) > 1 else 0.0
    k_open = next((k for k in range(len(rows)) if t[k] >= 0), 0)
    k_close = next((k for k in range(k_open + 1, len(rows)) if sw[k] and not sw[k - 1]), None)
    end_open = k_close if k_close is not None else len(rows)
    out = {"v_before_mv": round(v0), "pre_sd_mv": round(sd, 1), "pre_ms": round(-t[0], 2), "post_ms": round(t[-1], 2)}
    # The load letting go: V_out climbing out of the pre-trigger noise, or skipping starting.
    rise_band = max(4 * sd, 30.0)       # out of the pre-trigger noise, three samples running
    out["rise_ms"] = next((t[k] for k in range(k_open, end_open - 2)
                           if all(v[j] - v0 > rise_band for j in (k, k + 1, k + 2))), None)
    out["skip_ms"] = next((t[k] for k in range(k_open, end_open) if skip[k]), None)
    open_v = v[k_open:end_open]
    out["peak_while_open_mv"] = max(open_v) - round(v0) if open_v else None
    if k_close is not None:
        out["reconnect_sample_ms"] = t[k_close]
        t_rc = pulse_ms if pulse_ms is not None else t[k_close]
        after = v[k_close:]
        k_min = k_close + min(range(len(after)), key=after.__getitem__)
        out["dip_mv"] = v[k_min] - round(v0)
        out["dip_pct"] = round(100.0 * (v[k_min] - v0) / v0, 1)
        out["dip_at_ms_after_reconnect"] = round(t[k_min] - t_rc, 2)
        for pct in (2, 1):
            band = v0 * pct / 100
            last_out = max((k for k in range(k_close, len(rows)) if abs(v[k] - v0) > band), default=None)
            out[f"settled_{pct}pct_ms_after_reconnect"] = (
                0.0 if last_out is None else
                ("not within the record" if last_out == len(rows) - 1 else round(t[last_out + 1] - t_rc, 2)))
    out["i_cmd_max_ma"] = max(int(r["i_cmd_ma"]) for r in rows)
    out["i_max_min_ma"] = min(int(r["i_max_ma"]) for r in rows)
    out["at_limit_samples"] = sum(int(r["at_limit"]) for r in rows)   # on-time at its window limit
    # The command at its ceiling, whichever limit set it (in buck-boost the peak current, not the window).
    out["railed_samples"] = sum(1 for r in rows if int(r["i_cmd_ma"]) >= int(r["i_max_ma"]) - 2)
    out["skipping_samples"] = sum(skip)
    out["rectifier_on"] = any(int(r["rectifier"]) for r in rows)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--contract-mv", type=int, required=True)
    ap.add_argument("--open-ms", default="3",
                    help="comma-separated open times, 0.001-50 ms (TIM4, 1 us); run shortest first")
    ap.add_argument("--post", type=int, default=1900, help="trace samples after the trigger (50 us each)")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--pause", type=float, default=2.0, help="seconds between pulses")
    ap.add_argument("--max-dip-pct", type=float, default=15.0,
                    help="no longer open times once the reconnect undershoot dV_us (dip_pct) exceeds this")
    ap.add_argument("--out", required=True, help="directory for the trace CSVs and the summary")
    a = ap.parse_args()
    opens = sorted(float(x) for x in a.open_ms.split(","))
    out_dir = Path(a.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    results, stop = [], None
    for ms in opens:
        for i in range(a.repeat):
            status, problems = check_running(a.contract_mv)
            if problems:
                stop = f"{ms:g} ms #{i}: not starting: {'; '.join(problems)}"
                break
            arm = bu("regulator", "trace-arm", "--trigger", "switch", "--post", str(a.post))
            if not arm.get("ok"):
                stop = f"trace-arm failed: {arm}"
                break
            time.sleep(0.2)                  # pre-trigger samples: 0.2 s fills the 2048-sample ring
            pulse = bu("regulator", "switch-pulse", f"{ms:g}")
            if not pulse.get("ok"):
                stop = f"switch-pulse failed: {pulse.get('result')} {pulse.get('explanation', '')}"
                break
            tr = None
            for _ in range(10):
                time.sleep(0.3)
                tr = bu("regulator", "trace")
                if tr.get("state") == "DONE":
                    break
            if not tr or tr.get("state") != "DONE":
                stop = f"{ms:g} ms #{i}: trace not DONE: {tr}"
                break
            n = 0                            # never overwrite an earlier run of the same open time
            while (dst := out_dir / f"step_{a.contract_mv // 1000}v_{ms:g}ms_{n}.csv").exists():
                n += 1
            shutil.copy(ROOT / tr["file"], dst)
            with dst.open() as f:
                rows = list(csv.DictReader(f))
            p_us = tr.get("pulse_us_measured")
            m = {"open_ms": ms, "repeat": i, "pulse_us": tr.get("pulse_us"), "pulse_us_measured": p_us}
            m.update(metrics(rows, a.contract_mv, p_us / 1000 if p_us else None))
            status_after, problems = check_running(a.contract_mv)
            m.update(file=str(dst.relative_to(ROOT)) if dst.is_relative_to(ROOT) else str(dst),
                     after=problems or "running")
            results.append(m)
            print(json.dumps(m), flush=True)
            if problems:
                stop = f"stopping: {'; '.join(problems)}"
                break
            if m.get("dip_pct") is not None and -m["dip_pct"] > a.max_dip_pct:
                stop = f"{ms:g} ms: dV_us {m['dip_pct']} % is past --max-dip-pct {a.max_dip_pct:g}"
                break
            time.sleep(a.pause)
        if stop:
            print(stop, file=sys.stderr)
            break
    tag = "_".join(f"{x:g}" for x in opens) if len(opens) <= 3 else f"{opens[0]:g}-{opens[-1]:g}"
    summary = {"contract_mv": a.contract_mv, "open_ms": opens, "post": a.post, "stopped": stop, "steps": results}
    (out_dir / f"summary_{a.contract_mv // 1000}v_{tag}ms.json").write_text(json.dumps(summary, indent=1) + "\n")


if __name__ == "__main__":
    main()
