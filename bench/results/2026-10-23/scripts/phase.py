"""Sample-phase sweep in forced CCM at the present contract MV. Usage: phase.py MV OUT.jsonl SEL:PM,PM,... [SEL:...]
Aborts (back to automatic placement) when VBUS leaves +-14 % of MV or the regulator stops."""
import json, subprocess, statistics as st, sys, time
MV, OUT = int(sys.argv[1]), sys.argv[2]; L, VR = 3, 1
plan = [(int(a.split(':')[0]), [int(p) for p in a.split(':')[1].split(',')]) for a in sys.argv[3:]]
def bu(*a): return json.loads(subprocess.run(["tools/bringup/bu", *a], capture_output=True, text=True).stdout)
def setp(sel, pm):
    bu("mem", "write", "regulator_adc_phase_sel", str(sel), "--type", "u8")
    bu("mem", "write", "regulator_adc_phase_permille", str(pm), "--type", "u16")
def meas():
    vd, vs = [], []
    for _ in range(4):
        vd += bu("mem", "read", "adc1_dma_buffer", "--type", "u16", "--count", "192")['values'][VR::3]
        vs += bu("mem", "read", "adc2_dma_buffer", "--type", "u16", "--count", "96")['values'][VR::3]
    vb = bu("scope", "measure", "CH4", "--items", "VAVG")["measurements"]["CH4"]["VAVG"]
    return st.median(vd) * 70645 / 4096, st.median(vs), st.pstdev(vd), vb, bu("regulator", "status")
bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "1", "--offset", str(-min(MV / 1000.0, 20)), "--coupling", "DC", "--bwl", "20M")
bu("scope", "timebase", "--scale", "1e-3", "--offset", "0")
bu("scope", "trigger", "--source", "CH2", "--slope", "POS", "--level", "12", "--sweep", "AUTO"); time.sleep(1.5)
lo, hi = MV * 0.86 / 1000, MV * 1.14 / 1000
for sel, pms in [(0, [500])] + plan:
    for pm in pms:
        setp(sel, pm); time.sleep(0.6)
        vdm, vsm, sd, vb, s = meas(); c = s.get("control", {})
        rec = {"mv": MV, "sel": sel, "pm": pm, "vd_mv": round(vdm), "vs_raw": vsm, "vd_sd": round(sd, 1), "vbus": vb,
               "state": s["state"], "on_ns": c.get("on_time_ns"), "u_mv": round(c.get("ccm_u_mv") or 0)}
        print(json.dumps(rec), flush=True); open(OUT, "a").write(json.dumps(rec) + "\n")
        if s["state"] != "RUNNING" or vb is None or not (lo < vb < hi):
            setp(0, 500); print("ABORT"); sys.exit(1)
setp(0, 500); print("done")
