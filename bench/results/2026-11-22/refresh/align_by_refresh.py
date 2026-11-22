"""Average IL_MON/SW1/SW2 over Q1 pulses, grouped by the time since SW2 last came back up from a refresh dip."""
import lzma, sys
import numpy as np
f, pre_us, post_us, step = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
d = np.genfromtxt(lzma.open(f, "rt") if f.endswith(".xz") else f, delimiter=",", names=True)
t, il, sw, sw2 = d["t_s"], d["CH1"], d["CH2"], d["CH3"]
dt = t[1] - t[0]
hi = sw > 12.0
P = []
for i in np.flatnonzero(~hi[:-1] & hi[1:]):
    j = i + 1
    while j < len(sw) and hi[j]: j += 1
    if (j - i) * dt > 300e-9: P.append(i)
lo2 = sw2 < 3.0
ups = np.flatnonzero(lo2[:-1] & ~lo2[1:]) + 1          # SW2 back above 3 V (refresh end)
pre, post = int(pre_us * 1e-6 / dt), int(post_us * 1e-6 / dt)
P = [i for i in P if i - pre >= 0 and i + post < len(il)]
groups = {}
for i in P:
    k = np.searchsorted(ups, i) - 1
    if k < 0: continue
    since = (i - ups[k]) * dt * 1e6
    key = round(since * 2) / 2                           # 0.5 us bins
    groups.setdefault(key, []).append(i)
ref = np.mean([il[i - pre:i].mean() for i in P])
keys = sorted(g for g in groups if len(groups[g]) >= 20)
print(f"{len(P)} pulses, {dt*1e9:.1f} ns/sample; IL_MON minus {ref:.4f} V (100 mV/A); groups = us from SW2's refresh rise to Q1 on")
print("   t ns" + "".join(f" | {k:4.1f}us n={len(groups[k]):<3d} IL   SW2" for k in keys))
for x in range(int(-pre_us * 1000), int(post_us * 1000), step):
    k = pre + int(x * 1e-9 / dt)
    row = f"{x:7d}"
    for g in keys:
        I = groups[g]
        row += f" | {np.mean([il[i - pre + k] for i in I]) - ref:+.3f}      {np.mean([sw2[i - pre + k] for i in I]):5.1f}"
    print(row)
