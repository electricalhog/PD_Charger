"""n switch-pulse steps of MS ms with the trace on the switch trigger; summarize. usage: steps.py MS N SETPOINT_MV"""
import csv, json, subprocess, sys, time, numpy as np
ROOT = str(__import__("pathlib").Path(__file__).resolve().parents[4])  # repo root
def bu(*a):
    return json.loads(subprocess.run([f"{ROOT}/tools/bringup/bu", *a], cwd=ROOT, capture_output=True, text=True, timeout=60).stdout)
ms, n, sp = float(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
for k in range(n):
    bu("regulator", "trace-arm", "--trigger", "switch", "--post", "1900")
    p = bu("regulator", "switch-pulse", str(ms))
    if not p.get("ok"):
        print("pulse refused:", p.get("result")); break
    time.sleep(0.4)
    tr = bu("regulator", "trace"); r = list(csv.DictReader(open(f"{ROOT}/{tr['file']}")))
    t = np.array([float(x['t_ms']) for x in r]); v = np.array([int(x['v_out_mv']) for x in r])
    sw = np.array([int(x['switch_on']) for x in r]); ccm = np.array([int(x['ccm']) for x in r])
    rc = t[(t > 0) & (sw == 1)]
    s = bu("regulator", "status")
    if len(rc) == 0:
        print(f"step {k}: no reconnect in trace; state {s['state']} {s['fault_source']}"); break
    rc = rc[0]; a = (t >= rc) & (t < rc + 15); dv = v[a] - sp
    big = np.nonzero(np.abs(dv) > 0.05 * sp)[0]
    print(f"step {k}: open min {v[(t>=0)&(t<rc)].min()-sp:+d} mV | after reconnect: dip {dv.min():+d} peak {dv.max():+d} mV, "
          f"outside 5% for {(big[-1]*0.05 if len(big) else 0):.2f} ms | ccm samples {ccm.sum()}/{len(ccm)} | {s['state']} {s['fault_source']} exits {s['control']['ccm_exits']}")
    if s["state"] != "RUNNING": break
    time.sleep(1.0)
