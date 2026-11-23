import json, subprocess, sys, time, numpy as np
B='tools/bringup/bu'
def bu(*a):
    r=subprocess.run([B,*a],capture_output=True,text=True); 
    try: return json.loads(r.stdout)
    except Exception: return {'ok':False,'raw':r.stdout[-300:]}
def status():
    d=bu('regulator','status'); c=d['control']
    return d['state'], d['fault_source'], c['ccm_active'], c['on_time_ns'], c['ccm_u_mv'], d['adc']['v_out_mv'], c['ccm_exits']
def analyse(f):
    a=np.loadtxt(f,delimiter=',',skiprows=1); t=a[:,0]; vgs=a[:,1]-a[:,2]; sw2=a[:,2]
    off=np.where(vgs<1.0)[0]; g=[x for x in np.split(off,np.where(np.diff(off)>10)[0]+1) if len(x)>5]
    rows=[]
    for k,x in enumerate(g):
        t0,t1=t[x[0]],t[x[-1]]
        pre=(t>=t0-0.6e-6)&(t<t0-0.1e-6); post=(t>t1+0.3e-6)&(t<=t1+0.8e-6)
        if pre.sum()<10 or post.sum()<10: continue
        rows.append((t0, (t1-t0)*1e9, vgs[pre].mean(), vgs[post].mean(), np.median(sw2[x[0]:x[-1]+1])))
    hold=vgs>1.0
    return rows, vgs[hold].min() if hold.any() else None, vgs[hold].mean() if hold.any() else None
out=[]
for ns,ivl in [tuple(map(int,s.split(','))) for s in sys.argv[1:]]:
    bu('mem','write','regulator_ccm_refresh_ns',str(ns),'--type','u16'); bu('mem','write','regulator_ccm_refresh_interval_ns',str(ivl),'--type','u16')
    time.sleep(0.6)
    st=status()
    if st[0]!='RUNNING' or not st[2]: print('STOP', ns, ivl, st); break
    bu('scope','single'); time.sleep(0.4)
    cap=bu('scope','capture','CHAN2','CHAN3','--raw')
    rows,vmin,vmean=analyse(cap['file'])
    pre=[r[2] for r in rows]; post=[r[3] for r in rows]; sw=[r[4] for r in rows]; w=[r[1] for r in rows]
    st2=status()
    print(f"rf {ns:3d} ns / {ivl:5d} ns: refreshes {len(rows)} (Q3 off {np.mean(w):.0f} ns)  Vgs before {np.mean(pre):.2f} (min {np.min(pre):.2f})  after {np.mean(post):.2f}  step {np.mean(post)-np.mean(pre):+.2f}  hold min {vmin:.2f} mean {vmean:.2f}  SW2 med {np.mean(sw):.2f}  | on {st2[3]} u {st2[4]:.0f} V {st2[5]} exits {st2[6]} {st2[0]} {st2[1]}  file {cap['file']}")
    sys.stdout.flush()
    if np.min(pre)<2.5: print('below 2.5 V before a refresh: stop'); break
bu('mem','write','regulator_ccm_refresh_ns','200','--type','u16'); bu('mem','write','regulator_ccm_refresh_interval_ns','5000','--type','u16')
print('restored 200/5000', status())
