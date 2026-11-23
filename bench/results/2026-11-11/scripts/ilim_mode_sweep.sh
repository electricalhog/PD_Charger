#!/usr/bin/env bash
# Current-limit sweep on the 4S pack in one mode: one 20 s battery run per limit, the ceiling ABOVE mV (default 800)
# over the pack read at each start, 8 s idle between.  Usage: [ABOVE=mV] [START_DELAY=s] ilim_mode_sweep.sh dcm|ccm mA...
#   dcm: regulator_ccm_enable 0 for the sweep (restored to 1 at the end).
#   ccm: regulator_ccm_enable 1, the CCM trace armed before each step and read after it.
# Dan reads the output ammeter during each step; results go to
# bench/results/2026-11-11/ilim_<mode>_<first>-<last>.jsonl (2026-11-11 section 3).
set -u
sleep "${START_DELAY:-0}"
cd /home/daniel/GitHub/PD_Charger || exit 1
mode=${1:-}; shift || true
if [ "$mode" != dcm ] && [ "$mode" != ccm ]; then echo "usage: ilim_mode_sweep.sh dcm|ccm mA..."; exit 1; fi
if [ $# -eq 0 ]; then echo "no limits given"; exit 1; fi
above=${ABOVE:-800}
steps=("$@")
out=bench/results/2026-11-11/ilim_${mode}_${steps[0]}-${steps[-1]}.jsonl
: > "$out"
bu=tools/bringup/bu
restore() { $bu mem write regulator_ccm_enable 1 --type u8 > /dev/null; }
if [ "$mode" = dcm ]; then trap restore EXIT; $bu mem write regulator_ccm_enable 0 --type u8 > /dev/null || exit 1
else restore || exit 1; fi
for ma in "${steps[@]}"; do
  echo
  echo "=================================================================="
  echo "  $(date +%T)  STEP: ${mode} ${ma} mA, ceiling pack + ${above} mV  -- read the ammeter now (20 s)"
  echo "=================================================================="
  [ "$mode" = ccm ] && $bu regulator trace-arm --trigger ccm --post 1536 > /dev/null
  res=$($bu battery run --above-pack-mv "$above" --ilim-ma "$ma" --seconds 20)
  trace=null
  if [ "$mode" = ccm ]; then
    trace=$($bu regulator trace | python3 -c "import json,sys; d=json.load(sys.stdin); print(json.dumps({k: d.get(k) for k in ('state', 'file', 'ccm_samples', 'ilim_samples')}))")
  fi
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); d['set_ma']=$ma; d['mode']='$mode'; d['trace']=json.loads('''$trace'''); print(json.dumps(d))" >> "$out"
  echo "$res" | python3 -c "import json,sys; d=json.load(sys.stdin); r=[x for x in json.load(open(d['log'])) if x['state']=='RUNNING'] if d.get('log') else []; print('  done: ok', d.get('ok'), 'end', d.get('end_state'), d.get('end_fault'), 'ccm_frac', round(sum(x['ccm'] for x in r)/len(r),2) if r else None, 'lim_frac', d.get('limiting_frac'), 'ceiling', d.get('ceiling_mv'), 'tail', d.get('tail_mean'))"
  [ "$mode" = ccm ] && echo "  trace: $trace"
  if ! echo "$res" | grep -q '"end_state": "IDLE"'; then echo "  NOT IDLE after the step: stopping the sweep"; exit 1; fi
  if echo "$res" | grep -q '"end_fault": \[\]'; then :; else echo "  FAULT: stopping the sweep"; exit 1; fi
  echo "  $(date +%T)  idle 8 s (meter should read ~0)"
  sleep 8
done
echo
echo "SWEEP DONE ($mode) $(date +%T)"
