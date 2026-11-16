"""Score host-model replays (CSV t_us,vb,vx,il; switch closes at 30 ms) against a bench switch-pulse record
(2.5 us medians: t_us,il_mon_v,sw2_v,vbus_v), aligned on VBUS rising through 4 V; RMS over 0-300 us.
Usage: fit_replay.py BENCH.csv SW2_OFFSET_V MODEL.csv [MODEL.csv ...]"""
import csv, math, os, sys
B, off = sys.argv[1], float(sys.argv[2])
b = list(csv.DictReader(open(B)))
bt = [float(r['t_us']) for r in b]; bsw = [float(r['sw2_v']) + off for r in b]; bvb = [float(r['vbus_v']) for r in b]
bil = [float(r['il_mon_v']) / 0.25 for r in b]
t0b = next(t for t, v in zip(bt, bvb) if v > 4.0)
bench = {round(t - t0b): (s, v, i) for t, s, v, i in zip(bt, bsw, bvb, bil) if 0 <= t - t0b <= 300}
t14b = next(t for t in sorted(bench) if bench[t][1] > 14.0)
print("bench: VBUS 4->14 V in %d us, board min %.2f V, IL max %.1f A" % (t14b, min(v[0] for v in bench.values()),
      max(v[2] for v in bench.values())))
res = []
for f in sys.argv[3:]:
    m = list(csv.DictReader(open(f)))
    mt = [float(r['t_us']) for r in m]; mvb = [float(r['vb']) for r in m]; mvx = [float(r['vx']) for r in m]
    mil = [float(r['il']) for r in m]
    t0m = next((t for t, v in zip(mt, mvx) if t > 30000 and v > 4.0), None)
    if t0m is None: continue
    eb = ex = 0.0; n = 0
    for t, v, x in zip(mt, mvb, mvx):
        d = round(t - t0m)
        if d in bench and d % 5 == 0:
            eb += (v - bench[d][0]) ** 2; ex += (x - bench[d][1]) ** 2; n += 1
    if n < 20: continue
    t14 = next((t - t0m for t, x in zip(mt, mvx) if t > t0m and x > 14.0), 999)
    res.append((math.sqrt(eb / n) + math.sqrt(ex / n), math.sqrt(eb / n), math.sqrt(ex / n), t14,
                min(v for t, v in zip(mt, mvb) if t > t0m - 50), max(v for t, v in zip(mt, mvb) if t > t0m),
                max(mil), os.path.basename(f)))
res.sort()
for r in res[:10]:
    print("rms board %.2f, VBUS %.2f V | VBUS 4->14 V %3.0f us, board min %.2f max %.2f, IL max %.1f | %s" % r[1:])
