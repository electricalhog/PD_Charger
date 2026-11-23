"""Count V_out dips in RAM traces: an episode is V_out more than 800 mV under v_cmd (switch on).
usage: python3 dips.py trace.csv [...]"""
import csv, sys, statistics as st
for n in sys.argv[1:]:
    T = list(csv.DictReader(open(n)))
    if len(T) < 700: print(n, 'short', len(T)); continue
    k0 = 0 if T[0]['switch_on'] == '1' else 600
    v = [int(x['v_out_mv']) for x in T]; sp = [int(x['v_cmd_mv']) for x in T]
    ev = []; inside = False
    for k in range(k0, len(T)):
        low = v[k] < sp[k] - 800
        if low and not inside: ev.append((float(T[k]['t_ms']), min(v[k:k + 20]) - sp[k]))
        inside = low
    span = float(T[-1]['t_ms']) - float(T[k0]['t_ms'])
    e = [a - b for a, b in zip(v[k0:], sp[k0:])]
    print('%-44s dips %2d in %3.0f ms  V_out-v_cmd mean %+5.0f sd %4.0f [%+5d, %+5d]  %s' % (n.split('/')[-1], len(ev), span,
          st.mean(e), st.pstdev(e), min(e), max(e), [(round(a, 1), b) for a, b in ev[:6]]))
