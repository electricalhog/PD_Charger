"""Voltage sweep into the 25.3 ohm with efficiency, loss and switch-node captures (bench 2026-11-14).

One START at 12 V (auto CCM), then the ceiling stepped 12 -> 33 V and back to 12 V without stopping, so every
mode and CCM kind change happens inside the run, both ways (plans/lipo-load.md sections 9 and 10).  At each point:
SETTLE_S, then one raw capture of CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out (30k points, 240 us at 125 MSa/s); the
board is logged throughout (keepalive, ID_MON, IS_MON, mode, CCM kind).

Efficiency as on 2026-11-11 (bench README section 5): V_in and V_out are the medians of the scope trace samples,
I_out is ID_MON (ccm_i_avg_ma, after gain and offset) and I_in is IS_MON (i_in_filt_ma), both calibrated against
the ammeter (2026-11-11 / 2026-11-13).  Loss = V_in I_in - V_out I_out.

THE 25.3 OHM ON VBUS, NO PACK: a START at 5 V first checks that nothing holds VBUS up.  Without --go it prints the
plan only.  The run stops on a fault (never cleared), on either current estimate over 1.6 x 33 V / 25.3 ohm + 0.3 A
for 3 samples, or on Ctrl-C.

Run from the repository root:
  uv run --quiet --project tools/bringup python bench/results/2026-11-14/scripts/sweep.py --go
Output: bench/results/2026-11-14/sweep_<time>.json and captures/sweep_<time>_<dir>_<mV>.csv.gz.
"""

from __future__ import annotations

import argparse
import gzip
import json
import shutil
import signal
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "2026-11-12" / "scripts"))
import boost_cal as bc  # noqa: E402

R_OHM = 25.3
ILIM_MA = 2000                 # a backstop: 33 V / 25.3 ohm = 1.3 A
SETTLE_S = 2.0
UP_MV = [12000, 14000, 16000, 18000, 20000, 21000, 22000, 22500, 23000, 23500, 24000, 24400, 24800, 25200,
         25600, 26000, 27000, 28000, 29000, 30000, 31000, 32000, 33000]
DOWN_MV = UP_MV[-2::-1]

_row_of = bc.row_of


def row_of(st: dict, t: float) -> dict:
    """boost_cal's row plus the CCM exit reason and the last CCM -> CCM switch ((from << 4) | to)."""
    r = _row_of(st, t)
    c = st.get("control", {})
    r["exit_reason"], r["kind_last"] = c.get("ccm_exit_reason"), c.get("ccm_kind_switch_last")
    return r


bc.row_of = row_of


def scope_setup(s, v_out: float) -> None:
    s.channel(1, display=True, coupling="DC", probe=10, scale=5, offset=-17, bwl="OFF")     # SW1
    s.channel(3, display=True, coupling="DC", probe=10, scale=5, offset=-17, bwl="OFF")     # SW2
    s.channel(2, display=True, coupling="DC", probe=10, scale=2, offset=-24, bwl="20M")     # V_in
    s.channel(4, display=True, coupling="DC", probe=10, scale=2, offset=-v_out, bwl="20M")  # V_out
    s.timebase(scale=20e-6, offset=0)
    s.trigger(source="CHAN3", slope="NEG", level=v_out / 2, sweep="AUTO")
    s.write(":RUN")
    s.write(":ACQuire:MDEPth 30000")


def scope_center(s, v_out: float) -> None:
    """Plain writes: Scope.channel / trigger read the whole state back, which took about 10.5 s per point while CCM
    was on in the 10:53 run (0.8 s in DCM) and put 14.1 kJ in the resistor instead of the planned 5.3 kJ."""
    s.write(f":CHANnel4:OFFSet {-v_out:g}")
    s.write(f":TRIGger:EDGe:LEVel {v_out / 2:g}")
    s.write(":RUN")


def grab(s, root: Path, dst: Path) -> dict:
    w = s.wait_trigger(timeout=3.0)
    if not w.get("triggered"):
        s.write(":TFORce")
        w = s.wait_trigger(timeout=3.0, arm=False)
    r = s.capture(["CHAN1", "CHAN2", "CHAN3", "CHAN4"], mode="raw")
    src = root / r["file"]
    with open(src, "rb") as fi, gzip.open(dst, "wb") as fo:
        shutil.copyfileobj(fi, fo)
    src.unlink()
    s.write(":RUN")
    return {"file": dst.name, "triggered": w.get("triggered"), "sample_rate_sa_s": r.get("sample_rate_sa_s"),
            "warnings": r.get("warnings", [])}


def plan_text() -> str:
    pts = UP_MV + DOWN_MV
    heat = sum(mv ** 2 / 1e6 / R_OHM for mv in pts) * (SETTLE_S + 3.0)
    return (f"SWEEP: START at 12 V into 25.3 ohm (auto CCM, limit {ILIM_MA} mA as a backstop), then the ceiling "
            f"{len(UP_MV)} points up to 33 V and {len(DOWN_MV)} back down to 12 V, about {SETTLE_S + 3:.0f} s each "
            f"(about {len(pts) * (SETTLE_S + 3) / 60:.1f} min; the resistor takes about {heat / 1000:.1f} kJ, "
            f"at most {33 ** 2 / R_OHM:.0f} W).\n  THE 25.3 OHM ON VBUS, NO PACK; scope CH1 SW1, CH2 V_in, CH3 SW2, "
            f"CH4 V_out.\n  Points (mV): up {UP_MV}\n               down {DOWN_MV}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--go", action="store_true")
    a = p.parse_args()
    print(plan_text())
    if not a.go:
        print("\n(not run: add --go)")
        return 0
    from bringup.config import REPO_ROOT
    from bringup.scope import Scope
    root = Path(REPO_ROOT)
    out_dir = root / "bench/results/2026-11-14"
    cap_dir = out_dir / "captures"
    cap_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = out_dir / f"sweep_{stamp}.json"
    interrupted = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: interrupted.set())
    signal.signal(signal.SIGTERM, lambda *_: interrupted.set())
    b = bc.Board()
    if b.status()["state"] != "IDLE":
        print("regulator not IDLE: stopping"); return 1
    keep = {k: b.read(k, t) for k, t in (("regulator_ccm_enable", "u8"), ("regulator_ccm_ilim_ma", "u16"))}
    result = {"started": time.strftime("%F %T"), "r_ohm": R_OHM, "settle_s": SETTLE_S, "points": [],
              "firmware": {k: b.read(k, "u8") for k in ("regulator_ccm_kind_switch", "regulator_ccm_down",
                                                         "regulator_ccm_entry_blind")}}
    save = lambda: out.write_text(json.dumps(result, indent=1))   # noqa: E731
    with Scope(b.cfg) as s:
        scope_setup(s, 12.0)
        try:
            b.write("regulator_ccm_ilim_ma", 500, "u16")
            probe5 = b.command("start", mv=5000)
            if b.status()["state"] == "RUNNING":
                b.command("stop")
            pack_mv = b.battery()["pack_mv"]
            result["vbus_check"] = {"start_5v": probe5.get("result"), "pack_mv": pack_mv}
            if not probe5.get("ok") or pack_mv > 1500:
                result["stopped"] = f"VBUS not free: START 5 V {probe5.get('result')}, {pack_mv} mV"
                print(result["stopped"]); save(); return 1
            time.sleep(1.0)
            b.write("regulator_ccm_enable", bc.CCM_ENABLE["auto"], "u8")
            b.write("regulator_ccm_ilim_ramp_ma_s", 0, "u16")
            b.write("regulator_ccm_ilim_target_ma", ILIM_MA, "u16")
            n0 = {k: b.read(k, "u32") for k in ("regulator_ccm_entries", "regulator_ccm_exits",
                                                  "regulator_ccm_kind_switches")}

            def during(lock, done, t0):
                for d, pts in (("up", UP_MV), ("down", DOWN_MV)):
                    for mv in pts:
                        if done.is_set() or interrupted.is_set():
                            return
                        with lock:
                            r = b.command("set-voltage", mv=mv)
                        t_set = time.monotonic() - t0
                        scope_center(s, mv / 1000)
                        time.sleep(SETTLE_S)
                        if done.is_set() or interrupted.is_set():
                            return
                        pt = {"dir": d, "set_mv": mv, "set_result": r.get("result"), "t_set": round(t_set, 3),
                              "t_cap0": round(time.monotonic() - t0, 3)}
                        pt["cap"] = grab(s, root, cap_dir / f"sweep_{stamp}_{d}_{mv}.csv.gz")
                        pt["t_cap1"] = round(time.monotonic() - t0, 3)
                        result["points"].append(pt)
                        print(f"  {time.strftime('%T')}  {d:4s} {mv:5d} mV  {r.get('result')}", flush=True)

            run = bc.run_step(b, 12000, ILIM_MA, 0.5, bc.abort_for_resistor(33000, R_OHM), interrupted,
                              during=during)
            result["rows"] = run.pop("rows")
            result["run"] = run
            result["counts"] = {k.replace("regulator_", ""): b.read(k, "u32") - n0[k] for k in n0}
            print(f"  run ok {run.get('ok')}, end {run.get('end_state')} {run.get('end_fault')}, abort {run.get('abort')}, "
                  f"counts {result['counts']}")
        finally:
            try:
                if b.status()["state"] == "RUNNING":
                    b.command("stop")
            finally:
                for k, v in keep.items():
                    b.write(k, v, "u8" if k == "regulator_ccm_enable" else "u16")
                result["ended"] = time.strftime("%F %T")
                save()
    print(f"  -> {out}")
    return 0 if result.get("run", {}).get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
