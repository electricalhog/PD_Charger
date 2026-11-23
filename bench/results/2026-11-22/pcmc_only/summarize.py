"""Summaries of a pcmc_only run: the SWD log (0.2 s samples) and the RAM trace (50 us samples).
usage: python3 summarize.py NAME     (run from the repo root)"""
import json, csv, sys
D = 'bench/results/2026-11-22/pcmc_only'; n = sys.argv[1]
L = json.load(open(f'{D}/logs/run_{n}_log.json'))
R = [x for x in L if x['state'] == 'RUNNING']
print('log: %d running samples, ccm %s, pcmc %s, entries %s exits %s' % (len(R), sorted(set(x['ccm'] for x in R)),
      sorted(set(x['watch']['regulator_pcmc_ccm_active'] for x in R)), R[-1]['watch']['regulator_ccm_entries'],
      R[-1]['watch']['regulator_ccm_exits']))
S = [x for x in R if x['commanded_mv'] == x['target_mv'] and x['switch']]
if S:
    e = [x['v_out_mv'] - x['commanded_mv'] for x in S]; i = [x['i_out_ma'] for x in S]; c = [x['watch']['regulator_i_cmd_ma'] for x in S]
    print('settled (commanded = target, switch on): n %d  V_out-cmd mean %+d [%+d, %+d]  I_out %d..%d  i_cmd %d..%d  i_cmd-I_out mean %+d' %
          (len(S), sum(e) / len(e), min(e), max(e), min(i), max(i), min(c), max(c), sum(a - b for a, b in zip(c, i)) / len(S)))
W = [x for x in R if x['watch']['regulator_sweep_mv']]
if W:
    print('sweep: %d samples, turns %s, V %d..%d' % (len(W), W[-1]['watch'].get('regulator_sweep_turns', '-'),
          min(x['commanded_mv'] for x in W), max(x['commanded_mv'] for x in W)))
    for lo in range(4000, 22000, 3000):
        B = [x for x in W if lo <= x['commanded_mv'] < lo + 3000]
        if not B: continue
        e = [x['v_out_mv'] - x['commanded_mv'] for x in B]; d = [x['watch']['regulator_i_cmd_ma'] - x['i_out_ma'] for x in B]
        print('  %5.1f-%4.1f V  n %3d  V_out-cmd mean %+5.0f [%+5d, %+5d]  I_out %4d..%4d  i_cmd-I_out %+5.0f' % (lo / 1e3, (lo + 3000) / 1e3,
              len(B), sum(e) / len(e), min(e), max(e), min(x['i_out_ma'] for x in B), max(x['i_out_ma'] for x in B), sum(d) / len(d)))
try:
    T = list(csv.DictReader(open(f'{D}/trace_{n}.csv')))
except FileNotFoundError:
    T = []
if T:
    sw = next((k for k, x in enumerate(T) if x['switch_on'] == '1'), None)
    print('trace: %d samples, ccm %s, switch closes at %s ms' % (len(T), sorted(set(x['ccm'] for x in T)), T[sw]['t_ms'] if sw is not None else '-'))
    if T[0]['switch_on'] == '1':   # a late trace: V_out statistics at 50 us
        import statistics as st
        v = [int(x['v_out_mv']) for x in T]; c = [int(x['i_cmd_ma']) for x in T]
        print('  late trace: V_out mean %.0f sd %.0f p-p %d [%d, %d]  i_cmd mean %.0f sd %.0f' % (st.mean(v), st.pstdev(v), max(v) - min(v), min(v), max(v), st.mean(c), st.pstdev(c)))
        return_early = True
    else:
        return_early = False
    for x in ([] if return_early else T[:120:10] + T[120::150]):
        print('  t %6s  V_out %5s  v_cmd %5s  err %+6d  i_cmd %4s  t_on %4s  sw %s' % (x['t_ms'], x['v_out_mv'], x['v_cmd_mv'],
              int(x['v_out_mv']) - int(x['v_cmd_mv']), x['i_cmd_ma'], x['t_on_ns'], x['switch_on']))
