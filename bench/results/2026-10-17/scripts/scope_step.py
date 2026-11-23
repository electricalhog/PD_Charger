#!/usr/bin/env python3
"""One switch pulse with TP2 (scope CH4) at full memory depth, then the scope put back as found.

Needs the regulator RUNNING at --contract-mv with the output switch closed (see switch_step.py).
1. saves `bu scope state` to --out/scope_user_state.json;
2. CH4 DC, --vdiv V/div, 20 MHz limit, centred as near the setpoint as the scope allows (its offset
   stops at -20 V below 5 V/div, so 28 V needs 2.5 V/div: 10-30 V on screen; at 1 V/div the signal
   was off the top and the trigger level clamped to 25 V); CH2/CH3 hidden for the capture;
   --tdiv s/div with the trigger --pre-div divisions from the left (1 ms/div, 7.5: the open at -6 ms
   and the reconnect undershoot at 0 both in the 12 ms record); trigger CH4 falling at setpoint - --drop-v, HF reject
   (the switching spikes fired plain edge triggers at once, 2026-10-17 step 4), single;
3. runs switch_step.py for one pulse of --open-ms (its regulator trace goes to --out as usual);
4. waits for the trigger, then `bu scope capture CH4 --raw` and a screenshot, copied to --out;
5. always restores every channel, the timebase, trigger and acquisition from the saved state, then
   RUN.  `--restore FILE` only does step 5.

  scope_step.py --contract-mv 28000 --open-ms 6 --out bench/results/2026-10-17/data/scope_28v
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
BU = ROOT / "tools" / "bringup" / "bu"


def bu(*args, timeout=60):
    r = subprocess.run([str(BU), *map(str, args)], capture_output=True, text=True, timeout=timeout)
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        raise SystemExit(f"bu {' '.join(map(str, args))}: no JSON\n{r.stdout[-800:]}\n{r.stderr[-800:]}")


def restore(state):
    """Put back the channels, timebase, trigger and acquisition `bu scope state` recorded."""
    errors = []
    for name, c in state["channels"].items():
        n = name[-1]
        r = bu("scope", "chan", n, "--on" if c["display"] else "--off", "--probe", c["probe"],
               "--scale", c["scale_v_div"], "--offset", c["offset_v"], "--coupling", c["coupling"],
               "--bwl", c["bw_limit"])
        errors += r.get("errors", []) or []
    tb = state["timebase"]
    errors += bu("scope", "timebase", "--scale", tb["scale_s_div"], "--offset", tb["offset_s"]).get("errors", []) or []
    tr = state["trigger"]
    errors += bu("scope", "trigger", "--source", tr["source"], "--slope", tr["slope"], "--level", tr["level_v"],
                 "--sweep", tr["sweep"], "--coupling", tr["coupling"], "--holdoff", tr["holdoff_s"]).get("errors", []) or []
    bu("scope", "run")                    # memory depth can only be set while running
    acq = state["acquire"]
    errors += bu("scope", "acquire", "--type", acq["type"], "--mdepth", acq["mdepth"]).get("errors", []) or []
    after = bu("scope", "state")
    diffs = [k for k in ("channels", "timebase") if after.get(k) != state.get(k)]
    diffs += [f"trigger.{k}" for k in ("mode", "sweep", "source", "slope", "level_v", "coupling", "holdoff_s")
              if after.get("trigger", {}).get(k) != tr.get(k)]
    diffs += [f"acquire.{k}" for k in ("type", "mdepth") if after.get("acquire", {}).get(k) != acq.get(k)]
    return {"errors": [e for e in errors if e and not str(e).startswith("0,")], "differs": diffs}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--contract-mv", type=int)
    ap.add_argument("--open-ms", type=float, default=6.0)
    ap.add_argument("--vdiv", type=float, default=2.5, help="CH4 V/div (offset limit -20 V below 5 V/div)")
    ap.add_argument("--drop-v", type=float, default=1.2, help="trigger this far under the setpoint (falling)")
    ap.add_argument("--tdiv", type=float, default=1e-3, help="s/div")
    ap.add_argument("--pre-div", type=float, default=7.5, help="divisions before the trigger (12 on screen)")
    ap.add_argument("--out")
    ap.add_argument("--restore", help="only restore the scope from this saved state JSON")
    a = ap.parse_args()
    if a.restore:
        print(json.dumps(restore(json.loads(Path(a.restore).read_text()))))
        return
    if a.contract_mv is None or a.out is None:
        ap.error("--contract-mv and --out are required (or --restore FILE)")
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    state = bu("scope", "state")
    if not state.get("ok"):
        raise SystemExit(f"scope state failed: {state}")
    saved = out / "scope_user_state.json"
    if not saved.exists():                # keep the first one: that is the user's setup
        saved.write_text(json.dumps(state, indent=1) + "\n")
    state = json.loads(saved.read_text())
    v_set = a.contract_mv / 1000
    level = v_set - a.drop_v
    result = {"contract_mv": a.contract_mv, "open_ms": a.open_ms, "trigger_v": level}
    try:
        bu("scope", "chan", 4, "--on", "--scale", a.vdiv)
        bu("scope", "chan", 4, "--offset", -min(level, 20.0))       # the scope refuses more at <5 V/div
        bu("scope", "chan", 4, "--coupling", "DC", "--bwl", "20M")
        for n in (2, 3):
            if state["channels"][f"CH{n}"]["display"]:
                bu("scope", "chan", n, "--off")
        bu("scope", "timebase", "--scale", a.tdiv, "--offset", (6 - a.pre_div) * a.tdiv)
        trig = bu("scope", "trigger", "--source", "CHAN4", "--slope", "NEG", "--level", level,
                  "--sweep", "SING", "--coupling", "HFR")["trigger"]
        ch4 = bu("scope", "state")["channels"]["CH4"]
        result["ch4"] = {"scale_v_div": ch4["scale_v_div"], "offset_v": ch4["offset_v"], "trigger_level_v": trig["level_v"]}
        if abs(trig["level_v"] - level) > 0.05 or abs(ch4["scale_v_div"] - a.vdiv) > 1e-6:
            raise SystemExit(f"scope would not take the setup (wanted {a.vdiv} V/div, trigger {level} V): {result['ch4']}")
        bu("scope", "single")
        step = subprocess.run([sys.executable, str(HERE / "switch_step.py"), "--contract-mv", str(a.contract_mv),
                               "--open-ms", f"{a.open_ms:g}", "--max-dip-pct", "100", "--out", str(out)],
                              capture_output=True, text=True, timeout=120)
        result["switch_step"] = [json.loads(x) for x in step.stdout.splitlines() if x.startswith("{")]
        if step.stderr.strip():
            result["switch_step_stderr"] = step.stderr.strip()[-400:]
        w = bu("scope", "wait", "--no-arm", "--timeout", 3)
        result["triggered"] = w.get("triggered")
        if w.get("triggered"):
            cap = bu("scope", "capture", "CH4", "--raw", timeout=120)
            dst = out / f"tp2_{a.contract_mv // 1000}v_{a.open_ms:g}ms.csv"
            shutil.copy(ROOT / cap["file"], dst)
            result["capture"] = {"file": str(dst.relative_to(ROOT)), "sample_rate_sa_s": cap.get("sample_rate_sa_s"),
                                 "warnings": cap.get("warnings")}
            shot = bu("scope", "screenshot")
            if shot.get("ok"):
                png = out / f"tp2_{a.contract_mv // 1000}v_{a.open_ms:g}ms.png"
                shutil.copy(ROOT / shot["file"], png)
                result["screenshot"] = str(png.relative_to(ROOT))
    finally:
        result["restore"] = restore(state)
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
