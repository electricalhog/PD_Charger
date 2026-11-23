"""One row per switching period from a raw CH1 (IL_MON) / CH2 (SW1) / CH3 (SW2) capture.

usage: python3 per_period.py capture.csv out.txt   (xz -d the .csv.xz first)
Columns: SW1 rise time (us), Q1 width (ns, SW1 > 12 V), refresh (an SW2 dip below 4 V within 1.2 us
before the rise), IL_MON 50-150 ns after the rise (valley, V), IL_MON 20 ns before SW1's fall (V),
IL_MON peak, IL_MON minimum after the fall, mean SW2.  IL_MON: 1.636 V at 0 A, 100 mV/A.
"""
import sys, numpy as np
f=sys.argv[1]
d=np.loadtxt(f,delimiter=',',skiprows=1)
t,c1,c2,c3=d[:,0],d[:,1],d[:,2],d[:,3]
dt=t[1]-t[0]
h=c2>12; r=np.where(~h[:-1]&h[1:])[0]; fl=np.where(h[:-1]&~h[1:])[0]
h3=c3<4; r3=np.where(~h3[:-1]&h3[1:])[0]   # SW2 falling below 4 V (Q4 refresh)
rows=[]
for k,i in enumerate(r):
    j=fl[fl>i]
    if not len(j): break
    j=j[0]
    w=(j-i)*dt
    nxt = r[k+1] if k+1<len(r) else len(t)-1
    # refresh: SW2 dip within 1.2 us before this rise
    ref = np.any((r3< i)&(r3> i-int(1.2e-6/dt)))
    il_on = np.median(c1[i+int(50e-9/dt):i+int(150e-9/dt)])
    il_off = c1[max(j-int(20e-9/dt),0)]
    il_pk = c1[i:j+int(300e-9/dt)].max()
    il_min = c1[j:nxt].min() if nxt>j else np.nan
    rows.append((t[i]*1e6,w*1e9,int(ref),il_on,il_off,il_pk,il_min,np.mean(c3[i:nxt])))
rows=np.array(rows)
np.savetxt(sys.argv[2],rows,fmt='%.3f',header='t_us width_ns refresh il_on il_off il_pk il_min sw2_mean')
print(len(rows))
