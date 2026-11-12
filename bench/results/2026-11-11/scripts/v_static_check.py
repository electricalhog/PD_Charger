"""Static V_out / V_in check against the scope (bench 2026-11-11 section 6): no switching at any time.

V_out: the board's pack read (START at 5 V: output switch closed for 30 ms with every FET off, VD_MON read, refused
with PACK_AT_CEILING) while the scope captures CH4 single-shot on the rising edge.  The scope value is the median of
the trace samples on the plateau (5-25 ms after the edge), interpolated inside the code step.
V_in: the scope's CH2 median over the same capture against adc.v_in_mv read from the board at idle.

Run from the repository root:  uv run --quiet --project tools/bringup python bench/results/2026-11-11/scripts/v_static_check.py [n]
Output: bench/results/2026-11-11/vcal/ (static.json, the capture CSVs).
"""
from __future__ import annotations

import csv
import json
import shutil
import statistics
import sys
import time
from pathlib import Path

from bringup import battery, regulator
from bringup.config import REPO_ROOT, load_config
from bringup.scope import Scope

OUT = Path(REPO_ROOT) / "bench/results/2026-11-11/vcal"


def med(x: list[float]) -> float:
    """Median with linear interpolation inside the ADC code step (the grouped-data median)."""
    u = sorted(set(x))
    m = statistics.median(x)
    if len(u) < 2:
        return m
    lsb = min(q - p for p, q in zip(u, u[1:]))
    below = sum(1 for v in x if v < m - lsb / 2)
    at = sum(1 for v in x if abs(v - m) <= lsb / 2)
    return m - lsb / 2 + lsb * (len(x) / 2 - below) / at if at else m


def load(path: Path) -> dict[str, list[float]]:
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    return {name: [float(r[i]) for r in rows[1:]] for i, name in enumerate(rows[0])}


def mean(x: list[float]) -> float:
    return sum(x) / len(x)


def main() -> int:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    cfg = load_config()
    syms = battery._battery_syms(cfg)
    OUT.mkdir(parents=True, exist_ok=True)
    if regulator.status(cfg)["state"] != "IDLE":
        print("not IDLE: stopping"); return 1
    res = {"reads": []}
    with Scope(cfg) as s:
        s.channel(2, display=True, coupling="DC", probe=10, scale=2, offset=-20, bwl="20M")
        s.channel(4, display=True, coupling="DC", probe=10, scale=2, offset=-15.5, bwl="20M")
        s.timebase(scale=5e-3, offset=20e-3)
        s.write(":ACQuire:MDEPth 30000")
        s.trigger(source="CHAN4", slope="POS", level=11, sweep="NORMal")
        s.write(":RUN")
        time.sleep(1.0)
        for k in range(n):
            vin_b = [regulator.status(cfg)["adc"]["v_in_mv"] for _ in range(5)]
            s.write(":SINGle")
            time.sleep(1.0)
            pack_mv = battery.measure_pack(cfg, syms)
            w = s.wait_trigger(timeout=4.0, arm=False)
            if not w.get("triggered"):
                print(f"read {k}: pack {pack_mv} mV, scope did not trigger ({w})")
                res["reads"].append({"k": k, "pack_mv": pack_mv, "triggered": False})
                s.write(":RUN"); time.sleep(1.0)
                continue
            r = s.capture(["CHAN2", "CHAN4"], mode="raw")
            dst = OUT / f"static_{k}.csv"
            shutil.move(str(Path(REPO_ROOT) / r["file"]), dst)
            d = load(dst)
            t, c2, c4 = d["t_s"], d["CHAN2"], d["CHAN4"]
            pl = [v for ti, v in zip(t, c4) if 5e-3 <= ti <= 25e-3]
            pre = [v for ti, v in zip(t, c4) if ti < -2e-3]
            row = {"k": k, "pack_mv": pack_mv, "triggered": True, "file": dst.name,
                   "vout_scope_plateau": med(pl), "vout_scope_plateau_mean": mean(pl),
                   "vout_scope_before": med(pre) if pre else None,
                   "vin_scope": med(c2), "vin_scope_mean": mean(c2),
                   "vin_board_mv": vin_b, "n_plateau": len(pl), "sample_rate": r.get("sample_rate_sa_s")}
            res["reads"].append(row)
            print(f"read {k}: pack {pack_mv} mV | scope plateau {row['vout_scope_plateau']:.3f} V (mean "
                  f"{row['vout_scope_plateau_mean']:.3f}), before {row['vout_scope_before']} | V_in scope "
                  f"{row['vin_scope']:.3f} V board {mean(vin_b):.0f} mV", flush=True)
            s.write(":RUN")
            time.sleep(2.0)
        s.trigger(source="CHAN1", slope="POS", level=12, sweep="AUTO")
        s.timebase(scale=20e-6, offset=0)
        s.write(":RUN")
    res["end_state"] = regulator.status(cfg)["state"]
    (OUT / "static.json").write_text(json.dumps(res, indent=1))
    print("end state", res["end_state"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
