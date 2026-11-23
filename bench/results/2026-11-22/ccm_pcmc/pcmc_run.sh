#!/bin/bash
# usage: pcmc_run.sh CEIL_MV FSW_KHZ SECONDS NAME
CEIL=$1; FSW=$2; SEC=$3; NAME=$4
cd /home/daniel/GitHub/PD_Charger
B=tools/bringup/bu; D=bench/results/2026-11-22/ccm_pcmc; S=/tmp/claude-1000/-home-daniel--claude-spares-spare-uPUiWn/7871d0ac-c704-42e8-a5a8-680cb516fed8/scratchpad
st=$($B regulator status | python3 -c "import json,sys;d=json.load(sys.stdin);print(d['state'],','.join(d['fault_source']))")
echo "pre: $st"; case "$st" in IDLE*) ;; *) echo "not IDLE, abort"; exit 1;; esac
$B mem write regulator_ccm_enable 2 --type u8 | grep -q '"verified": true' || echo "ccm_enable write failed"
$B mem write regulator_pcmc_ccm 1 --type u8 | grep -q '"verified": true' || echo "pcmc write failed"
$B mem write regulator_buck_fsw_khz $FSW --type u16 | grep -q '"verified": true' || echo "fsw write failed"
$B regulator trace-arm --trigger ccm --post 1800 >/dev/null
$B scope run >/dev/null
$B battery run --ceiling-mv $CEIL --ilim-ma 2000 --seconds $SEC --interval 0.5 \
  --watch regulator_pcmc_ccm_active:u8 --watch regulator_pcmc_ccm_ton_ns:u16 --watch regulator_pcmc_ccm_meas_ns:u16 \
  --watch regulator_pcmc_ccm_ithr_ma:f32 --watch regulator_pcmc_ccm_bs_hits:u32 --watch regulator_pcmc_ccm_samples:u32 \
  --watch regulator_i_cmd_ma:f32 --watch regulator_pcmc_dac_counts:u16 --watch regulator_fsw_khz:u16 > $D/run_$NAME.json 2>&1 &
sleep 5; $B scope capture CH1 CH2 CH3 --raw > $S/cap_$NAME.json; wait
python3 - "$D/run_$NAME.json" "$D" "$NAME" <<'PY'
import json,sys,shutil,os
d=json.load(open(sys.argv[1])); D=sys.argv[2]; n=sys.argv[3]
print('ok',d.get('ok'),'ended',d.get('ended'),'fault',d.get('end_fault'),'tail',d.get('tail_mean'))
if d.get('log') and os.path.exists(d['log']):
    shutil.copy(d['log'], f"{D}/logs/run_{n}_log.json"); L=json.load(open(d['log']))
    R=[x for x in L if x['state']=='RUNNING']
    if R:
        v=[x['v_out_mv'] for x in R]; i=[x['i_out_ma'] for x in R]; c=[x['watch']['regulator_i_cmd_ma'] for x in R]
        print('n',len(R),'v',min(v),max(v),round(sum(v)/len(v)),'i',min(i),max(i),'icmd',round(min(c)),round(max(c)),'ccm',sorted(set(x['ccm'] for x in L)),'pcmc',sorted(set(x['watch']['regulator_pcmc_ccm_active'] for x in L)))
    print('first',L[0]['watch']); print('last',L[-1]['watch'])
PY
$B regulator trace > $S/trace_$NAME.json; python3 -c "import json;d=json.load(open('$S/trace_$NAME.json'));print('trace',d['file'],d['state'],d['count'],d['trigger_sample'],d['v_out_mv'])"
$B mem read regulator_ccm_exit_log --type u32 --count 16 | python3 -c "import json,sys;v=json.load(sys.stdin)['values'];print('exits',[(v[i],v[i+1]&255,v[i+1]>>8) for i in range(0,16,2) if v[i+1]])"
f=$(python3 -c "import json;d=json.load(open('$S/cap_$NAME.json'));print(d.get('csv') or d.get('file') or '')")
if [ -n "$f" ]; then xz -c $f > $D/capture_$NAME.csv.xz; python3 $D/per_period.py $f $D/periods_$NAME.txt >/dev/null
python3 - $D/periods_$NAME.txt <<'PY'
import sys,numpy as np
a=np.loadtxt(sys.argv[1])
per=np.median(np.diff(a[:,0])); print('periods',len(a),'median period us %.3f'%per)
for r in (0,1):
  m=a[:,2]==r; w=a[m,1]
  if len(w): print(' refresh',r,'n',len(w),'width %.0f+-%.0f [%.0f,%.0f]'%(w.mean(),w.std(),w.min(),w.max()),'il_on %.3f+-%.3f il_off %.3f+-%.3f'%(a[m,3].mean(),a[m,3].std(),a[m,4].mean(),a[m,4].std()))
PY
fi
