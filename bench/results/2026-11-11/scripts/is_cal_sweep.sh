#!/usr/bin/env bash
# IS_MON (input current) calibration into the 25.3 ohm resistor, buck DCM: one 20 s battery run per ceiling, the
# current limit only a backstop, 8 s idle before the first step and between steps (Dan reads the idle draw).
# Usage: [ILIM=mA] is_cal_sweep.sh [mV ...] (default 5000 8000 11000 14000 17000 20000).  Dan reads the input
# ammeter during each step; results go to bench/results/2026-11-11/is_cal_<first>-<last>.jsonl.
set -u
cd /home/daniel/GitHub/PD_Charger || exit 1
ilim=${ILIM:-1500}
if [ $# -gt 0 ]; then steps=("$@"); else steps=(5000 8000 11000 14000 17000 20000); fi
out=bench/results/2026-11-11/is_cal_${steps[0]}-${steps[-1]}.jsonl
: > "$out"
echo "  $(date +%T)  idle 8 s before the first step (read the idle draw)"
sleep 8
for mv in "${steps[@]}"; do
  echo
  echo "=================================================================="
  echo "  $(date +%T)  STEP: ceiling ${mv} mV, limit ${ilim} mA  -- read the input ammeter now (20 s)"
  echo "=================================================================="
  res=$(tools/bringup/bu battery run --ceiling-mv "$mv" --ilim-ma "$ilim" --seconds 20)
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); d['step_mv']=$mv; print(json.dumps(d))" >> "$out"
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); r=[x for x in json.load(open(d['log'])) if x['state']=='RUNNING'] if d.get('log') else []; print('  done: ok', d.get('ok'), 'end', d.get('end_state'), d.get('end_fault'), 'ccm_frac', round(sum(x['ccm'] for x in r)/len(r),2) if r else None, 'lim_frac', d.get('limiting_frac'), 'tail', d.get('tail_mean'))"
  if ! echo "$res" | grep -q '"end_state": "IDLE"'; then echo "  NOT IDLE after the step: stopping the sweep"; exit 1; fi
  if echo "$res" | grep -q '"end_fault": \[\]'; then :; else echo "  FAULT: stopping the sweep"; exit 1; fi
  echo "  $(date +%T)  idle 8 s (meter should read the idle draw)"
  sleep 8
done
echo
echo "SWEEP DONE $(date +%T)"
