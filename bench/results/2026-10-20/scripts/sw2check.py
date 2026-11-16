"""Capture SW1/SW2/VBUS (scope already set: CH4 DC 1 V/div BWL, NORMAL acq, 100 us/div, edge trig CH2 12 V) and summarize Q3 dropouts."""
import json, subprocess, sys, numpy as np
ROOT = str(__import__("pathlib").Path(__file__).resolve().parents[4])  # repo root
def bu(*a):
    return json.loads(subprocess.run([f"{ROOT}/tools/bringup/bu", *a], cwd=ROOT, capture_output=True, text=True, timeout=120).stdout)
bu("scope", "wait", "--timeout", "5")
f = bu("scope", "capture", "CH2", "CH3", "CH4", "--raw")["file"]
s = bu("regulator", "status"); c = s.get("control", {})
d = np.genfromtxt(f"{ROOT}/{f}", delimiter=',', names=True); t, s1, s2, vb = d['t_s'], d['CH2'], d['CH3'], d['CH4']; dt = t[1]-t[0]
low = (s2 < 2.0).astype(int); e = np.diff(np.r_[0, low, 0]); w = (np.nonzero(e == -1)[0]-np.nonzero(e == 1)[0])*dt
hi = (s2 > vb.mean()+1.2).astype(int); e2 = np.diff(np.r_[0, hi, 0]); wh = (np.nonzero(e2 == -1)[0]-np.nonzero(e2 == 1)[0])*dt
up = np.nonzero((s1[:-1] < 12) & (s1[1:] >= 12))[0]; dn = np.nonzero((s1[:-1] >= 12) & (s1[1:] < 12))[0]
per = np.median(np.diff(up))*dt if len(up) > 2 else float('nan')
hit = [(dn[dn > u][0]-u)*dt for u in up[:-1] if (dn > u).any()]
n = max(1, int(2e-6/dt)); m = len(vb)//n; vl = (vb-vb.mean())[:m*n].reshape(m, n).mean(1)
sp = np.abs(np.fft.rfft(vl*np.hanning(m))); fr = np.fft.rfftfreq(m, n*dt); k20 = np.argmin(abs(fr-20e3))
print(json.dumps(dict(label=sys.argv[1] if len(sys.argv) > 1 else "", file=f, fsw_khz=round(1e-3/per, 1), sw1_duty=round(np.median(hit)/per, 4),
      sw2_short_lows=int(((w > 60e-9) & (w < 400e-9)).sum()), sw2_long_lows=int((w > 400e-9).sum()), sw2_high_body=int((wh > 100e-9).sum()),
      vbus_dc=round(vb.mean(), 3), sw2_mean=round(s2.mean(), 3), vbus_lp_sd_mv=round(vl.std()*1e3, 1), line_20k_mv=round(sp[k20]*4/m*1e3, 1),
      state=s["state"], faults=s["fault_source"], ccm=c.get("ccm_active"), ton=c.get("on_time_ns"), u_mv=round(c.get("ccm_u_mv", 0), 1),
      vin=s["adc"]["v_in_mv"], refresh=(c.get("ccm_refresh_ns"), c.get("ccm_refresh_interval_ns")))))
