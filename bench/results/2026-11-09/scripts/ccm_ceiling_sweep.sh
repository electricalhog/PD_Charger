#!/usr/bin/env bash
# CCM current calibration on the 4S pack in voltage mode: one 20 s battery run per ceiling, the current limit only a
# backstop.  Usage: [ILIM=mA] ccm_ceiling_sweep.sh [above] MV...  MV is the ceiling, or with "above" mV over the pack
# read at each start.  Dan reads the output ammeter during each step; results go to
# bench/results/2026-11-09/ccm_ceiling_<first>-<last>.jsonl (ccm_above_... with "above").
set -u
cd /home/daniel/GitHub/PD_Charger || exit 1
ilim=${ILIM:-4500}
opt=--ceiling-mv; name=ceiling
if [ "${1:-}" = above ]; then opt=--above-pack-mv; name=above; shift; fi
if [ $# -gt 0 ]; then steps=("$@"); else steps=(16100 16250 16400); fi
out=bench/results/2026-11-09/ccm_${name}_${steps[0]}-${steps[-1]}.jsonl
: > "$out"
for mv in "${steps[@]}"; do
  echo
  echo "=================================================================="
  echo "  $(date +%T)  STEP: ${name} ${mv} mV, limit ${ilim} mA  -- read the ammeter now (20 s)"
  echo "=================================================================="
  res=$(tools/bringup/bu battery run "$opt" "$mv" --ilim-ma "$ilim" --seconds 20)
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); d['step_mv']=$mv; print(json.dumps(d))" >> "$out"
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); r=[x for x in json.load(open(d['log'])) if x['state']=='RUNNING'] if d.get('log') else []; print('  done: ok', d.get('ok'), 'end', d.get('end_state'), d.get('end_fault'), 'ccm_frac', round(sum(x['ccm'] for x in r)/len(r),2) if r else None, 'lim_frac', d.get('limiting_frac'), 'ceiling', d.get('ceiling_mv'), 'tail', d.get('tail_mean'))"
  if ! echo "$res" | grep -q '"end_state": "IDLE"'; then echo "  NOT IDLE after the step: stopping the sweep"; exit 1; fi
  if echo "$res" | grep -q '"end_fault": \[\]'; then :; else echo "  FAULT: stopping the sweep"; exit 1; fi
  echo "  $(date +%T)  idle 8 s (meter should read ~0)"
  sleep 8
done
echo
echo "SWEEP DONE $(date +%T)"
