"""ID_MON CCM vs DCM at a fixed contract with the QT Py adding a constant-power load (same V_out -> same load
current in both modes).  Per (mV, P): QT Py armed, 6 s settle, 10 status reads in auto (DCM expected), then
regulator_ccm_enable = 2 for 10 reads, back to 1; QT Py disarmed (SIGTERM).
usage: qtpy_cal.py MV:P_MW [MV:P_MW ...]      appends to ../data/cal.jsonl with tag qtpy"""
import json, pathlib, signal, subprocess, sys, time
sys.path.insert(0, "/home/daniel/GitHub/PD_Charger/bench/results/2026-11-03/scripts")
from benchlib import ROOT, bu, w, status_n
OUT = pathlib.Path(__file__).resolve().parents[1] / "data" / (sys.argv.pop(1)[6:] if len(sys.argv) > 1 and sys.argv[1].startswith("--out=") else "cal.jsonl")


def rec(mv, p, ccm):
    w("regulator_ccm_enable", ccm, "u8"); time.sleep(1.0)
    s = status_n(10, 0.1); s.update(mv=mv, qtpy_mw=p, ccm_enable=ccm, tag="qtpy", r_only_ma=round(s["v"] / 25.3, 1))
    print(json.dumps({k: s[k] for k in ("mv", "qtpy_mw", "ccm_enable", "ccm_seen", "v", "iout", "iout_sd", "iid", "iin", "ton", "r_only_ma")}))
    sys.stdout.flush()
    with open(OUT, "a") as f:
        f.write(json.dumps(s) + "\n")


cur = None
try:
    for arg in sys.argv[1:]:
        mv, p = (int(x) for x in arg.split(":"))
        if mv != cur:
            assert bu("sink", "req", mv, "--until", "contract", "--timeout", 3).get("matched"); cur = mv; time.sleep(1)
        lr = None
        if p:
            lr = subprocess.Popen([f"{ROOT}/tools/bringup/bu", "load", "run", "--p", str(p), "--seconds", "40",
                                   "--limit-ma", "3000", "--contract-mv", str(mv), "--vin-min-mv", str(int(mv * 0.85))],
                                  cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(6.0)
        rec(mv, p, 1); rec(mv, p, 2); w("regulator_ccm_enable", 1, "u8"); time.sleep(1.0)
        if lr:
            lr.send_signal(signal.SIGTERM); out, _ = lr.communicate(timeout=30)
            try:
                lj = json.loads(out); print("load:", lj.get("ok"), lj.get("fault") or lj.get("last_fault"))
            except json.JSONDecodeError:
                print("load out:", out[-300:])
finally:
    w("regulator_ccm_enable", 1, "u8")
