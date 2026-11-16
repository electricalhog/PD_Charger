"""Switch-node captures: CH1 = SW1 (TP3, Q1/Q2), CH3 = SW2 (TP4, Q3/Q4), DC, 10x, BWL off.

usage: edges.py LABEL
  edge: CH1 alone (1 GSa/s), 20 ns/div, single on a rising then a falling edge at V_in/2; prints 2 ns samples.
  period: CH1 + CH3, 1 us/div, single on CH1 rising; prints each channel's 1/50/99 percentiles and, for CH1,
          the fraction of the record in each level band (high > 0.8 V_in, low < 2 V, below -1 V, between).
Leaves CH2 and CH4 off; restore with steps.scope_restore().
"""
import csv, pathlib, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "2026-11-03" / "scripts"))
from benchlib import ROOT, bu


def single(src, slope, level):
    bu("scope", "trigger", "--source", src, "--slope", slope, "--level", round(level, 2), "--coupling", "DC", "--sweep", "SING")
    bu("scope", "single"); time.sleep(0.3)
    if not bu("scope", "wait", "--no-arm", "--timeout", 3).get("ok"):
        return None
    return bu("scope", "capture", *(("CH1", "CH3") if period else ("CH1",)), "--raw", t=60)


def load(f):
    rows = list(csv.reader(open(f"{ROOT}/{f}")))
    return rows[0], [[float(x) for x in r] for r in rows[1:]]


label = sys.argv[1]
vin = bu("regulator", "status").get("adc", {}).get("v_in_mv", 24000) / 1000
st = bu("regulator", "status"); c = st.get("control", {})
print(label, "state", st.get("state"), "V_out", st.get("adc", {}).get("v_out_mv"), "V_in", vin, "kind", c.get("ccm_kind"),
      "mode", c.get("mode"), "t_on", c.get("on_time_ns"), "sr", c.get("sr_enable"))
for ch in ("2", "4"):
    bu("scope", "chan", ch, "--off")
bu("scope", "chan", "1", "--on", "--probe", "10", "--scale", "5", "--offset", "-12", "--coupling", "DC", "--bwl", "OFF")
bu("scope", "acquire", "--mdepth", "AUTO", "--type", "NORM")

period = False
bu("scope", "chan", "3", "--off")
bu("scope", "timebase", "--scale", "2e-8", "--offset", "0")
for slope in ("POS", "NEG"):
    c = single("CH1", slope, vin / 2)
    if not c or not c.get("file"):
        print(f"  {slope}: no trigger"); continue
    _, rows = load(c["file"])
    print(f"  SW1 {slope} {c['file']}:", " ".join(f"{r[0]*1e9:.0f}:{r[1]:.1f}" for r in rows if -20e-9 <= r[0] <= 60e-9 and round(r[0] * 1e9) % 2 == 0))

period = True
bu("scope", "chan", "3", "--on", "--probe", "10", "--scale", "5", "--offset", "-15", "--coupling", "DC", "--bwl", "OFF")
bu("scope", "timebase", "--scale", "1e-6", "--offset", "4e-6")
c = single("CH1", "POS", vin / 2)
if c and c.get("file"):
    hdr, rows = load(c["file"])
    for j, name in ((1, "SW1"), (2, "SW2")):
        if j >= len(hdr):
            continue
        v = sorted(r[j] for r in rows); q = lambda p: v[int(p * (len(v) - 1))]
        line = f"  {name} {c['file']}: p1 {q(0.01):.1f} p50 {q(0.5):.1f} p99 {q(0.99):.1f}"
        if j == 1:
            n = len(v)
            line += (f"  high {sum(x > 0.8 * vin for x in v) / n:.2f} low(<2V) {sum(-1 <= x < 2 for x in v) / n:.2f}"
                     f" below-1V {sum(x < -1 for x in v) / n:.2f} between {sum(2 <= x <= 0.8 * vin for x in v) / n:.2f}")
        print(line)
    dt = rows[1][0] - rows[0][0]
    print(f"  period record {len(rows)} pts, {dt * 1e9:.0f} ns/pt")
else:
    print("  period: no trigger")
