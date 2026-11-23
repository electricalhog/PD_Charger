#!/usr/bin/env bash
# DCM current-calibration sweep on the 4S pack: one 20 s battery run per setting, 8 s idle between.
# Usage: ilim_sweep.sh [mA ...] (default 100 150 200 250).  Dan reads the ammeter during each step; results go to
# bench/results/2026-11-09/ilim_sweep_<first>-<last>.jsonl.
set -u
cd /home/daniel/GitHub/PD_Charger || exit 1
if [ $# -gt 0 ]; then steps=("$@"); else steps=(100 150 200 250); fi
out=bench/results/2026-11-09/ilim_sweep_${steps[0]}-${steps[-1]}.jsonl
: > "$out"
for ma in "${steps[@]}"; do
  echo
  echo "=================================================================="
  echo "  $(date +%T)  STEP: ${ma} mA setting  -- read the ammeter now (20 s)"
  echo "=================================================================="
  res=$(tools/bringup/bu battery run --ceiling-mv 16000 --ilim-ma "$ma" --seconds 20)
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); d['set_ma']=$ma; print(json.dumps(d))" >> "$out"
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); r=[x for x in json.load(open(d['log'])) if x['state']=='RUNNING'] if d.get('log') else []; print('  done: ok', d.get('ok'), 'end', d.get('end_state'), d.get('end_fault'), 'ccm_frac', round(sum(x['ccm'] for x in r)/len(r),2) if r else None, 'tail', d.get('tail_mean'))"
  if ! echo "$res" | grep -q '"end_state": "IDLE"'; then echo "  NOT IDLE after the step: stopping the sweep"; exit 1; fi
  if echo "$res" | grep -q '"end_fault": \[\]'; then :; else echo "  FAULT: stopping the sweep"; exit 1; fi
  echo "  $(date +%T)  idle 8 s (meter should read ~0)"
  sleep 8
done
echo
echo "SWEEP DONE $(date +%T)"
