#!/usr/bin/env bash
# V_in / V_out readings against the scope (CH2 V_in, CH4 V_out, 10x probes): idle, then a DCM step and a CCM step on
# the 4S pack, ceiling pack + ABOVE mV (default 800), 20 s each.  Five scope VAVG/VMAX/VMIN readings 6-15 s into each
# step, while `bu battery run` logs the board.  Usage: [ABOVE=mV] scope_v_check.sh [dcm_mA] [ccm_mA]
# (default 1000 3500).  Results: bench/results/2026-11-11/scope_v_check.jsonl (section 4).
set -u
cd /home/daniel/GitHub/PD_Charger || exit 1
bu=tools/bringup/bu
above=${ABOVE:-800}
dcm_ma=${1:-1000}; ccm_ma=${2:-3500}
out=bench/results/2026-11-11/scope_v_check.jsonl
: > "$out"
$bu scope chan 2 --on --coupling DC --probe 10 --scale 2 --offset -20 --bwl 20M > /dev/null || exit 1
$bu scope chan 4 --on --coupling DC --probe 10 --scale 2 --offset -15.5 --bwl 20M > /dev/null || exit 1
$bu scope timebase --scale 100e-6 > /dev/null || exit 1
$bu scope run > /dev/null
sleep 1
scope_read() {   # $1 label: five readings, one per second, as JSON lines
  for k in 1 2 3 4 5; do
    $bu scope measure CHAN2 CHAN4 --items VAVG,VMAX,VMIN | python3 -c "import json,sys; d=json.load(sys.stdin); d['label']='$1'; d['k']=$k; print(json.dumps({'scope': d}))" >> "$out"
    sleep 1
  done
}
echo "  $(date +%T)  idle (regulator IDLE, output switch open)"
scope_read idle
restore() { $bu mem write regulator_ccm_enable 1 --type u8 > /dev/null; }
trap restore EXIT
for step in "dcm $dcm_ma" "ccm $ccm_ma"; do
  set -- $step; mode=$1; ma=$2
  if [ "$mode" = dcm ]; then $bu mem write regulator_ccm_enable 0 --type u8 > /dev/null || exit 1; else restore || exit 1; fi
  echo "  $(date +%T)  STEP: $mode $ma mA, ceiling pack + $above mV (20 s)"
  res_file=$(mktemp)
  $bu battery run --above-pack-mv "$above" --ilim-ma "$ma" --seconds 20 > "$res_file" &
  run_pid=$!
  sleep 6
  scope_read "$mode $ma"
  wait $run_pid
  python3 -c "import json,sys; d=json.load(open('$res_file')); d['step']='$mode $ma'; print(json.dumps({'run': d}))" >> "$out"
  python3 -c "import json; d=json.load(open('$res_file')); print('  done: ok', d.get('ok'), d.get('end_state'), d.get('end_fault'), 'pack', d.get('pack_read_mv'), 'tail', d.get('tail_mean'))"
  rm -f "$res_file"
  if ! grep -q '"end_state": "IDLE"' <(tail -1 "$out"); then echo "  NOT IDLE after the step: stopping"; exit 1; fi
  if ! grep -q '"end_fault": \[\]' <(tail -1 "$out"); then echo "  FAULT: stopping"; exit 1; fi
  echo "  $(date +%T)  idle 8 s"
  sleep 8
done
echo "  $(date +%T)  idle again"
scope_read idle2
echo "DONE $(date +%T)"
