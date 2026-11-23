#!/bin/bash
# Peak current mode as the buck's only mode (regulator_pcmc_ccm = 2), into the 25.3 ohm resistor.
# usage: run.sh CEIL_MV FSW_KHZ N SECONDS NAME
#   N      Q4's refresh every N periods (regulator_ccm_refresh_interval_ns = N T)
# env:  LO HI RATE  triangle sweep of the target (mV, mV, mV/s; RATE 0 = none)
#       HOLD=1      output switch stays open (no load)
#       SCOPE=fast  raw CH1-3 capture at 5 s (per_period.py); SCOPE=slow  CH1+CH4 over the whole run; none
#       TRIG=ccm|error|late|sweep  THR LATE  the RAM trace's trigger (error: |setpoint - V_out| > THR mV;
#                       late: LATE s after the start, 100 ms of 50 us samples; sweep: a turnaround, 50 ms after)
CEIL=$1; FSW=$2; N=$3; SEC=$4; NAME=$5
LO=${LO:-0}; HI=${HI:-0}; RATE=${RATE:-0}; HOLD=${HOLD:-0}; SCOPE=${SCOPE:-fast}; TRIG=${TRIG:-ccm}; THR=${THR:-1500}
cd /home/daniel/GitHub/PD_Charger
B=tools/bringup/bu; D=bench/results/2026-11-22/pcmc_only; S=/tmp/claude-1000/-home-daniel--claude-spares-spare-uPUiWn/7871d0ac-c704-42e8-a5a8-680cb516fed8/scratchpad
PER_NS=$((1000000 / FSW)); IVL=$((N * PER_NS))
st=$($B regulator status | python3 -c "import json,sys;d=json.load(sys.stdin);print(d['state'],','.join(d['fault_source']))")
echo "pre: $st  fsw $FSW kHz  N $N (refresh every $IVL ns)  sweep $LO..$HI @ $RATE mV/s  hold_open $HOLD"
case "$st" in IDLE*) ;; *) echo "not IDLE, abort"; exit 1;; esac
w() { $B mem write $1 $2 --type $3 | grep -q '"verified": true' || echo "write $1 failed"; }
w regulator_ccm_enable 2 u8
w regulator_pcmc_ccm 2 u8
w regulator_buck_fsw_khz $FSW u16
w regulator_ccm_refresh_interval_ns $IVL u16
w regulator_sweep_lo_mv $LO u32
w regulator_sweep_hi_mv $HI u32
w regulator_sweep_rate_mv_s $RATE u32
w regulator_output_hold_open $HOLD u8
if [ "$TRIG" = late ]; then $B regulator trace-arm --trigger after --threshold-mv $(( ${LATE:-4} * 1000 )) --post 2000 >/dev/null
elif [ "$TRIG" = sweep ]; then $B regulator trace-arm --trigger sweep --post 1000 >/dev/null
elif [ "$TRIG" = error ]; then $B regulator trace-arm --trigger error --threshold-mv $THR --post 1500 >/dev/null
else $B regulator trace-arm --trigger $TRIG --post 1800 >/dev/null; fi
if [ "$SCOPE" = slow ]; then
  $B scope chan 4 --scale 5 --offset -12.5 >/dev/null
  $B scope timebase --scale $(python3 -c "print(($SEC+1)/12)") --offset $(python3 -c "print(($SEC+1)/2-0.5)") >/dev/null
  $B scope acquire --type NORM >/dev/null
  $B scope trigger --source CH4 --slope POS --level 2 --sweep SING >/dev/null
  $B scope single >/dev/null; sleep 1
else
  $B scope run >/dev/null
fi
$B battery run --ceiling-mv $CEIL --ilim-ma 2000 --seconds $SEC --interval 0.2 \
  --watch regulator_sweep_mv:u32 --watch regulator_sweep_turns:u32 --watch regulator_pcmc_ccm_active:u8 --watch regulator_pcmc_ccm_ton_ns:u16 \
  --watch regulator_pcmc_ccm_meas_ns:u16 --watch regulator_pcmc_ccm_ithr_ma:f32 --watch regulator_i_cmd_ma:f32 \
  --watch regulator_pcmc_ccm_samples:u32 --watch regulator_pcmc_ccm_bs_hits:u32 \
  --watch regulator_ccm_entries:u32 --watch regulator_ccm_exits:u32 --watch regulator_ccm_exit_reason:u8 \
  > $D/run_$NAME.json 2>&1 &
if [ "$SCOPE" = fast ]; then sleep 5; $B scope capture CH1 CH2 CH3 --raw > $S/cap_$NAME.json; fi
wait
w regulator_sweep_rate_mv_s 0 u32
w regulator_output_hold_open 0 u8
python3 - "$D/run_$NAME.json" "$D" "$NAME" <<'PY'
import json,sys,shutil,os
d=json.load(open(sys.argv[1])); D=sys.argv[2]; n=sys.argv[3]
print('ok',d.get('ok'),'ended',d.get('ended'),'fault',d.get('end_fault'),'error',d.get('error'))
if d.get('log') and os.path.exists(d['log']):
    shutil.copy(d['log'], f"{D}/logs/run_{n}_log.json"); L=json.load(open(d['log']))
    R=[x for x in L if x['state']=='RUNNING']
    if R:
        v=[x['v_out_mv'] for x in R]; i=[x['i_out_ma'] for x in R]; c=[x['watch']['regulator_i_cmd_ma'] for x in R]
        e=[x['v_out_mv']-x['commanded_mv'] for x in R if x['switch'] or x['watch']['regulator_sweep_mv']]
        print('n',len(R),'v',min(v),max(v),round(sum(v)/len(v)),'i',min(i),max(i),'icmd',round(min(c)),round(max(c)),
              'ccm',sorted(set(x['ccm'] for x in L)),'pcmc',sorted(set(x['watch']['regulator_pcmc_ccm_active'] for x in R)),
              'vin',min(x['v_in_mv'] for x in R),max(x['v_in_mv'] for x in R))
        if e: print('V_out - commanded (switch closed or sweeping): min',min(e),'max',max(e),'mean',round(sum(e)/len(e)))
    print('first',L[0]['watch']); print('last',L[-1]['watch'])
PY
$B regulator trace > $S/trace_$NAME.json; python3 -c "import json,shutil;d=json.load(open('$S/trace_$NAME.json'));print('trace',d.get('file'),d.get('state'),d.get('count'),d.get('trigger_sample'),d.get('v_out_mv'))
f=d.get('file')
if f: shutil.copy(f,'$D/trace_$NAME.csv')"
$B mem read regulator_ccm_exit_log --type u32 --count 16 | python3 -c "import json,sys;v=json.load(sys.stdin)['values'];print('exits',[(v[i],v[i+1]&255,v[i+1]>>8) for i in range(0,16,2) if v[i+1]])"
if [ "$SCOPE" = fast ]; then
f=$(python3 -c "import json;d=json.load(open('$S/cap_$NAME.json'));print(d.get('csv') or d.get('file') or '')")
if [ -n "$f" ]; then xz -c $f > $D/capture_$NAME.csv.xz; python3 $D/per_period.py $f $D/periods_$NAME.txt >/dev/null
python3 - $D/periods_$NAME.txt <<'PY'
import sys,numpy as np
a=np.loadtxt(sys.argv[1])
per=np.median(np.diff(a[:,0])); print('periods',len(a),'median period us %.3f'%per)
for r in (0,1):
  m=a[:,2]==r; w=a[m,1]
  if len(w): print(' refresh',r,'n',len(w),'width %.0f+-%.0f [%.0f,%.0f]'%(w.mean(),w.std(),w.min(),w.max()),'il_on %.3f+-%.3f il_off %.3f+-%.3f il_min %.3f'%(a[m,3].mean(),a[m,3].std(),a[m,4].mean(),a[m,4].std(),a[m,6].mean()))
PY
fi; fi
if [ "$SCOPE" = slow ]; then
  $B scope wait --timeout 5 >/dev/null; $B scope capture CH1 CH4 > $S/cap_$NAME.json
  f=$(python3 -c "import json;d=json.load(open('$S/cap_$NAME.json'));print(d.get('csv') or d.get('file') or '')")
  [ -n "$f" ] && xz -c $f > $D/slow_$NAME.csv.xz && echo "slow capture $f"
  $B scope chan 4 --scale 10 --offset -20 >/dev/null; $B scope timebase --scale 2e-6 --offset 0 >/dev/null
  $B scope acquire --type NORM >/dev/null; $B scope trigger --source CH2 --slope POS --level 8 --sweep NORM >/dev/null
fi
