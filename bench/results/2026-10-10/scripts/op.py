"""One operating point: optional load, power-board telemetry, three raw scope captures.

usage: python3 op.py LABEL VNOM_MV [P_MW] [--vin-min MV]
Writes cap/LABEL_{hf,hfw,lf}* (grab.py), cap/LABEL_reg.json and, with a load,
cap/LABEL_load.json (the bu load run summary; its TEL frames file path inside).
"""
import json
import signal
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
BU = str(REPO / "tools" / "bringup" / "bu")
CAP = HERE.parent / "data" / "cap"
GRAB = ["uv", "run", "--quiet", "--project", str(REPO / "tools" / "bringup"), "python", str(HERE / "grab.py")]


def bu(*args):
    r = subprocess.run([BU, *args], capture_output=True, text=True)
    return json.loads(r.stdout)


def reg_samples(n=5):
    out = []
    for _ in range(n):
        d = bu("regulator", "status")
        a, c = d["adc"], d["control"]
        out.append({"state": d["state"], "fault": d["fault_source"], "v_out_mv": a["v_out_mv"], "v_in_mv": a["v_in_mv"],
                    "i_l_ma": a["i_inductor_ma"], "on_ns": c["on_time_ns"], "skipped": c["pulses_skipped"]})
    return out


def grab(label, **kw):
    args = [label]
    for k, v in kw.items():
        args += [f"--{k}", str(v)]
    r = subprocess.run(GRAB + args, capture_output=True, text=True)
    line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-400:]
    return json.loads(line) if line.startswith("{") else {"error": line}


def mean_v(label, ch="CH4"):
    m = json.loads((CAP / f"{label}.json").read_text())
    p = m["waveforms"][ch]["preamble"]
    c = np.fromfile(CAP / f"{label}_{ch}.bin", dtype=np.uint8).astype(float)
    v = (c - p["yorig"] - p["yref"]) * p["yinc"]
    return float(v.mean()), m["waveforms"][ch]["clipped"] / len(c)


def main():
    label, vnom = sys.argv[1], int(sys.argv[2])
    p_mw = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 0
    vin_min = int(sys.argv[sys.argv.index("--vin-min") + 1]) if "--vin-min" in sys.argv else 0
    log = {"label": label, "contract_mv": vnom, "p_target_mw": p_mw}
    load = None
    if p_mw:
        log["load_launch_unix"] = time.time()
        load = subprocess.Popen([BU, "load", "run", "--p", str(p_mw), "--seconds", "120", "--limit-ma", "500",
                                 "--contract-mv", str(vnom), "--vin-min-mv", str(vin_min)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(3.0)
    log["reg_before"] = reg_samples()
    vest = np.mean([r["v_out_mv"] for r in log["reg_before"]]) / 1000
    caps = {}
    # 1. 20 MHz-limited wideband single: 6 ms at 200 MSa/s
    for scale in (0.1, 0.2):
        caps["hf"] = grab(f"{label}_hf", chans=4, scale=scale, offset=-round(vest, 2), bwl="20M", tb=0.0005,
                          mdepth=1200000, atype="HRES", settle=0.5, note=f"{label}: 20 MHz BW single")
        mu, clip = mean_v(f"{label}_hf")
        if clip < 1e-3:
            break
    # 2. same, full bandwidth
    caps["hfw"] = grab(f"{label}_hfw", chans=4, scale=scale, offset=-round(mu, 2), bwl="OFF", tb=0.0005,
                       mdepth=1200000, atype="HRES", settle=0.5, note=f"{label}: full BW single")
    # 3. long single for the LF spectrum and the DC level: 600 ms at 2 MSa/s
    for scale_lf in (0.05, 0.1):
        caps["lf"] = grab(f"{label}_lf", chans=4, scale=scale_lf, offset=-round(mu, 3), bwl="20M", tb=0.05,
                          mdepth=1200000, atype="HRES", settle=1.7, note=f"{label}: 600 ms single")
        mu_lf, clip_lf = mean_v(f"{label}_lf")
        if clip_lf < 1e-3:
            break
    log["captures"] = caps
    log["mean_hf_v"], log["mean_lf_v"] = mu, mu_lf
    log["reg_after"] = reg_samples()
    if load:
        log["load_stop_unix"] = time.time()
        load.send_signal(signal.SIGTERM)
        out, err = load.communicate(timeout=60)
        try:
            log["load"] = json.loads(out)
        except json.JSONDecodeError:
            log["load"] = {"error": (out + err)[-400:]}
    (CAP / f"{label}_op.json").write_text(json.dumps(log, indent=1))
    reg = log["reg_before"] + log["reg_after"]
    summ = {"label": label, "lf_mean_v": round(mu_lf, 4), "hf_mean_v": round(mu, 4),
            "adc_vout_mv": round(float(np.mean([r["v_out_mv"] for r in reg]))),
            "adc_vin_mv": round(float(np.mean([r["v_in_mv"] for r in reg]))),
            "on_ns": [r["on_ns"] for r in reg], "faults": sorted({f for r in reg for f in r["fault"]}),
            "clips": {k: v.get("clipped") for k, v in caps.items()}, "scales": [scale, scale_lf]}
    if load:
        lj = log["load"]
        summ["load"] = {"ok": lj.get("ok"), "fault": lj.get("fault"),
                        "steps": [{k: s.get(k) for k in ("p_mw", "vin_mv", "vout_mv", "pout_mw", "duty_pm")}
                                  for s in lj.get("steps", [])]}
    print(json.dumps(summ))


if __name__ == "__main__":
    main()
