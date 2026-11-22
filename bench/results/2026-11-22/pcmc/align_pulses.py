"""Average IL_MON (CH1), SW1 (CH2) and SW2 (CH3, optional) over every Q1 pulse in a raw `bu scope capture` CSV.

A pulse = SW1 above 12 V for more than 300 ns (ringing crossings are dropped).  Prints the averaged
IL_MON (minus its level before the pulse), SW1 and SW2 against the time from SW1's rising 12 V crossing.
    python3 align_pulses.py capture.csv[.xz] [pre_us] [post_us] [step_ns]
"""
import lzma, sys
import numpy as np

f = sys.argv[1]
pre_us, post_us, step = (float(sys.argv[2]) if len(sys.argv) > 2 else 1.0,
                         float(sys.argv[3]) if len(sys.argv) > 3 else 2.5,
                         int(sys.argv[4]) if len(sys.argv) > 4 else 50)
d = np.genfromtxt(lzma.open(f, "rt") if f.endswith(".xz") else f, delimiter=",", names=True)
t, il, sw = d["t_s"], d["CH1"], d["CH2"]
sw2 = d["CH3"] if "CH3" in d.dtype.names else None
dt = t[1] - t[0]
hi = sw > 12.0
P = []
for i in np.flatnonzero(~hi[:-1] & hi[1:]):
    j = i + 1
    while j < len(sw) and hi[j]:
        j += 1
    if (j - i) * dt > 300e-9:
        P.append(i)
pre, post = int(pre_us * 1e-6 / dt), int(post_us * 1e-6 / dt)
P = [i for i in P if i - pre >= 0 and i + post < len(il)]
avg = lambda x: np.mean([x[i - pre:i + post] for i in P], axis=0)
a1, a2 = avg(il), avg(sw)
a3 = avg(sw2) if sw2 is not None else None
b0 = a1[: int(0.2e-6 / dt)].mean()
print(f"{len(P)} pulses, {dt * 1e9:.1f} ns/sample, IL_MON reference {b0:.4f} V (100 mV/A)")
print("   t ns  IL_MON-ref V   SW1 V" + ("   SW2 V" if a3 is not None else ""))
for x in range(int(-pre_us * 1000), int(post_us * 1000), step):
    k = pre + int(x * 1e-9 / dt)
    print(f"{x:7d}    {a1[k] - b0:+.3f}    {a2[k]:6.1f}" + (f"  {a3[k]:6.1f}" if a3 is not None else ""))
