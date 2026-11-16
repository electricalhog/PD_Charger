"""SW2 (CH3) over 1.2 ms in CCM: Q3 should hold SW2 at V_out except Q4's refresh dips.  A Q3 dropout puts SW2
above V_out (current through Q3's reverse conduction).  Prints the fraction of samples above V_out + 1.5 V and
below 2 V, and the longest run above V_out + 1.5 V.
usage: sw2_check.py V_OUT_V"""
import csv, sys, time, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "../2026-11-03/scripts"))
sys.path.insert(0, "/home/daniel/GitHub/PD_Charger/bench/results/2026-11-03/scripts")
from benchlib import ROOT, bu
vo = float(sys.argv[1])
for c in ("1", "2", "4"):
    bu("scope", "chan", c, "--off")
bu("scope", "chan", "3", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "OFF")
bu("scope", "trigger", "--source", "CH3", "--slope", "NEG", "--level", round(vo / 2, 2), "--coupling", "DC", "--sweep", "NORM")
bu("scope", "timebase", "--scale", "1e-4", "--offset", "0")
bu("scope", "run"); time.sleep(0.5)
bu("scope", "acquire", "--mdepth", "1.2M", "--type", "NORM")
bu("scope", "trigger", "--source", "CH3", "--slope", "NEG", "--level", round(vo / 2, 2), "--coupling", "DC", "--sweep", "SING")
bu("scope", "single"); time.sleep(0.5)
ok = bu("scope", "wait", "--no-arm", "--timeout", 3).get("ok")
c = bu("scope", "capture", "CH3", "--raw", t=180)
rows = [[float(x) for x in r] for r in list(csv.reader(open(f"{ROOT}/{c['file']}")))[1:]]
v = [r[1] for r in rows]; dt = rows[1][0] - rows[0][0]
hi = [x > vo + 1.5 for x in v]
run = best = 0
for h in hi:
    run = run + 1 if h else 0; best = max(best, run)
print(dict(file=c["file"], trig=ok, n=len(v), dt_ns=round(dt * 1e9, 2), frac_above=round(sum(hi) / len(v), 4),
           frac_low=round(sum(x < 2 for x in v) / len(v), 4), longest_above_ns=round(best * dt * 1e9),
           vmax=round(max(v), 2), vmin=round(min(v), 2)))
