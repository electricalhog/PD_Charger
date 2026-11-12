"""Efficiency ramp on the 4S pack with scope captures at every point (bench 2026-11-11 section 5).

Two battery runs: DCM (regulator_ccm_enable 0) and CCM (1).  Within a run the current limit is stepped in RAM, so
CCM is entered once and then walked down to its light-load edge and up.  At each point: settle, then one raw
capture of CH1 SW1, CH2 V_in, CH3 SW2, CH4 V_out (30k points, 240 us at 125 MSa/s), and at the FFT points a second
capture of CH4 AC-coupled (300k points, 2.4 ms).  A thread keeps the keepalive going and logs the board every
0.25 s; the probe is shared under one lock.

Run from the repository root:  uv run --quiet --project tools/bringup python bench/results/2026-11-11/scripts/power_ramp.py
Output: bench/results/2026-11-11/ramp/ (ramp.json, board_<mode>.json, the capture CSVs).
"""

from __future__ import annotations

import json
import shutil
import sys
import threading
import time
from pathlib import Path

from bringup import battery, probe, regulator
from bringup.config import REPO_ROOT, load_config
from bringup.scope import Scope

OUT = Path(REPO_ROOT) / "bench/results/2026-11-11/ramp"
ABOVE_MV = 800
SETTLE_S = 2.5
PHASES = [
    ("dcm", 0, [100, 250, 500, 750, 1000, 1250, 1500, 1750, 2000]),
    ("ccm", 1, [2500, 2000, 1750, 1500, 1300, 3000, 3500, 4000, 4500]),
]
FFT_POINTS = {("dcm", 500), ("dcm", 1500), ("ccm", 1500), ("ccm", 3000), ("ccm", 4500)}
KEYS = ("ccm_active", "ccm_limiting", "sr_enable", "sr_blend", "fsw_khz", "buck_fsw_khz", "i_out_filt_ma",
        "i_in_filt_ma", "p_in_mw", "p_out_mw", "ccm_i_ma", "ccm_i_avg_ma", "target_mv", "commanded_mv", "mode")


def scope_dc(s: Scope) -> None:
    s.channel(1, display=True, coupling="DC", probe=10, scale=5, offset=-12, bwl="OFF")
    s.channel(3, display=True, coupling="DC", probe=10, scale=5, offset=-12, bwl="OFF")
    s.channel(2, display=True, coupling="DC", probe=10, scale=2, offset=-20, bwl="20M")
    s.channel(4, display=True, coupling="DC", probe=10, scale=2, offset=-15.5, bwl="20M")
    s.timebase(scale=20e-6, offset=0)
    s.trigger(source="CHAN1", slope="POS", level=12, sweep="AUTO")
    s.write(":RUN")
    s.write(":ACQuire:MDEPth 30000")


def scope_ac(s: Scope, scale: float) -> None:
    s.channel(4, coupling="AC", scale=scale, offset=0, bwl="20M")
    s.timebase(scale=200e-6, offset=0)
    s.write(":RUN")
    s.write(":ACQuire:MDEPth 300000")


def grab(s: Scope, sources: list[str], name: str) -> dict:
    w = s.wait_trigger(timeout=3.0)
    if not w.get("triggered"):   # nothing switching (idle): force one acquisition
        s.write(":TFORce")
        w = s.wait_trigger(timeout=3.0, arm=False)
    r = s.capture(sources, mode="raw")
    dst = OUT / f"{name}.csv"
    shutil.move(str(Path(REPO_ROOT) / r["file"]), dst)
    s.write(":RUN")
    return {"file": dst.name, "triggered": w.get("triggered"), "sample_rate_sa_s": r.get("sample_rate_sa_s"),
            "download_s": r.get("download_s"), "warnings": r.get("warnings", [])}


def main() -> int:
    cfg = load_config()
    syms = battery._battery_syms(cfg)
    OUT.mkdir(parents=True, exist_ok=True)
    lock = threading.Lock()
    result = {"above_mv": ABOVE_MV, "phases": []}
    with Scope(cfg) as s:
        scope_dc(s)
        time.sleep(1.0)
        result["idle"] = grab(s, ["CHAN1", "CHAN2", "CHAN3", "CHAN4"], "idle_dc")
        for mode, ccm_enable, limits in PHASES:
            if regulator.status(cfg)["state"] != "IDLE":
                print("not IDLE before the", mode, "phase: stopping"); break
            probe.mem_write(cfg, "regulator_ccm_enable", str(ccm_enable), "u8")
            pack_mv = battery.measure_pack(cfg, syms)
            probe.mem_write(cfg, "regulator_ccm_ilim_ma", str(limits[0]), "u16")
            ka = battery._read(cfg, syms)["keepalive"]
            start = regulator.command(cfg, "start", 5.0, mv=pack_mv + ABOVE_MV)
            if not start.get("ok"):
                print("start refused:", start.get("result")); break
            rows, stop_evt, t0 = [], threading.Event(), time.monotonic()

            def logger() -> None:
                nonlocal ka
                while not stop_evt.is_set():
                    with lock:
                        ka = (ka + 1) & 0xFFFFFFFF
                        probe.mem_write(cfg, "regulator_battery_keepalive", str(ka), "u32")
                        st = regulator.status(cfg)
                    c, adc = st.get("control", {}), st.get("adc", {})
                    row = {"t": round(time.monotonic() - t0, 3), "state": st["state"], "fault": st["fault_source"],
                           "v_out_mv": adc.get("v_out_mv"), "v_in_mv": adc.get("v_in_mv"),
                           "is_raw_ma": adc.get("i_in_ma"), "id_8scan_ma": adc.get("i_out_ma")}
                    row.update({k: c.get(k) for k in KEYS})
                    rows.append(row)
                    if st["state"] != "RUNNING":
                        stop_evt.set()
                    time.sleep(0.02)

            th = threading.Thread(target=logger, daemon=True)
            th.start()
            phase = {"mode": mode, "pack_read_mv": pack_mv, "ceiling_mv": pack_mv + ABOVE_MV, "points": []}
            try:
                for ma in limits:
                    if stop_evt.is_set():
                        break
                    with lock:
                        probe.mem_write(cfg, "regulator_ccm_ilim_ma", str(ma), "u16")
                    t_set = time.monotonic() - t0
                    time.sleep(SETTLE_S)
                    pt = {"set_ma": ma, "t_set": round(t_set, 3), "t_cap0": round(time.monotonic() - t0, 3)}
                    pt["dc"] = grab(s, ["CHAN1", "CHAN2", "CHAN3", "CHAN4"], f"{mode}_{ma}_dc")
                    if (mode, ma) in FFT_POINTS and not stop_evt.is_set():
                        for scale in (0.5, 1.0):
                            scope_ac(s, scale)
                            time.sleep(1.5)
                            pt["ac"] = grab(s, ["CHAN4"], f"{mode}_{ma}_ac")
                            pt["ac"]["scale_v_div"] = scale
                            if not pt["ac"]["warnings"]:
                                break
                        scope_dc(s)
                        time.sleep(0.8)
                    pt["t_cap1"] = round(time.monotonic() - t0, 3)
                    phase["points"].append(pt)
                    print(f"  {time.strftime('%T')} {mode} {ma} mA: captured ({pt['t_cap1'] - pt['t_set']:.1f} s)",
                          rows[-1]["state"] if rows else "", flush=True)
            finally:
                stop_evt.set()
                th.join(timeout=5.0)
                with lock:
                    if regulator.status(cfg)["state"] == "RUNNING":
                        regulator.command(cfg, "stop", 3.0)
                    probe.mem_write(cfg, "regulator_ccm_enable", "1", "u8")
                (OUT / f"board_{mode}.json").write_text(json.dumps(rows))
            end = regulator.status(cfg)
            phase.update(end_state=end["state"], end_fault=end["fault_source"], samples=len(rows))
            result["phases"].append(phase)
            (OUT / "ramp.json").write_text(json.dumps(result, indent=1))
            print(f"  {time.strftime('%T')} {mode} phase done: {end['state']} {end['fault_source']}", flush=True)
            if end["state"] != "IDLE" or end["fault_source"]:
                print("  fault or not IDLE: stopping"); break
            time.sleep(5.0)
    (OUT / "ramp.json").write_text(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
