"""SW1/SW2/VBUS(AC) capture analysis: per-period structure, dead times, refresh, VBUS spectrum. usage: swan.py CSV"""
import sys, numpy as np
d = np.genfromtxt(sys.argv[1], delimiter=',', names=True)
t, s1, s2, vb = d['t_s'], d['CH2'], d['CH3'], d['CH4']
dt = t[1] - t[0]; print(f"{len(t)} pts, {dt*1e9:.1f} ns, span {(t[-1]-t[0])*1e6:.0f} us")
print(f"SW1 {s1.min():.1f}..{s1.max():.1f} V, SW2 {s2.min():.1f}..{s2.max():.1f} V")
# SW1 rising edges at mid level
lvl = (np.percentile(s1, 95) + np.percentile(s1, 5)) / 2
up = np.nonzero((s1[:-1] < lvl) & (s1[1:] >= lvl))[0]; dn = np.nonzero((s1[:-1] >= lvl) & (s1[1:] < lvl))[0]
if len(up) > 3:
    per = np.diff(up) * dt; print(f"SW1 rises {len(up)}, period {np.median(per)*1e9:.1f} ns -> {1/np.median(per)/1e3:.1f} kHz")
    hi = [(dn[dn > u][0] - u) * dt for u in up[:-1] if (dn > u).any()]
    print(f"SW1 high time median {np.median(hi)*1e9:.0f} ns (duty {np.median(hi)/np.median(per):.3f})")
# SW2 dips (Q4 refresh): SW2 normally at Vout, goes to ~0
l2 = np.percentile(s2, 50) / 2
d2 = np.nonzero((s2[:-1] >= l2) & (s2[1:] < l2))[0]; u2 = np.nonzero((s2[:-1] < l2) & (s2[1:] >= l2))[0]
if len(d2) > 2:
    w = [(u2[u2 > a][0] - a) * dt for a in d2[:-1] if (u2 > a).any()]
    print(f"SW2 dips {len(d2)}, interval {np.median(np.diff(d2))*dt*1e6:.2f} us, width {np.median(w)*1e9:.0f} ns")
v = vb - vb.mean()
print(f"VBUS AC (20 MHz BWL): sd {v.std()*1e3:.1f} mV, p-p {np.ptp(v)*1e3:.0f} mV")
# low-pass VBUS to 50 kHz by block averaging 2 us
n = max(1, int(2e-6 / dt)); m = len(v) // n; vl = v[:m*n].reshape(m, n).mean(1)
sp = np.abs(np.fft.rfft(vl * np.hanning(m))); fr = np.fft.rfftfreq(m, n*dt)
k = (fr > 500) & (fr < 200e3)
print(f"VBUS (2 us mean) sd {vl.std()*1e3:.1f} mV; lines:", ", ".join(f"{fr[i]/1e3:.1f} kHz {sp[i]*4/m*1e3:.0f} mV" for i in np.argsort(sp*k)[-5:][::-1]))
