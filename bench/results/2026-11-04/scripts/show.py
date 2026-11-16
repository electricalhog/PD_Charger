"""One line per section of a steps.py record."""
import json, sys
for l in open(sys.argv[1]):
    d = json.loads(l)
    print(d["contract_mv"], d["t"])
    for k in ("light", "heavy"):
        x = d.get(k)
        if not x:
            continue
        s = x["status"]
        print(f"  {k}: v {s['v']} vin {s['vin']} pin {s['pin']} pout {s['pout']} iin {s['iin']} iout {s['iout']} kinds {s['kinds']} "
              f"eta {x.get('eta_adc_pct')} pvr {x.get('p_out_vr_w')} trace {x['trace'].get('mean')}/{x['trace'].get('sd')}/"
              f"{x['trace'].get('min')}..{x['trace'].get('max')} ripple {x['ripple'].get('pp_ma200ns_mv')}/{x['ripple'].get('rms_ma200ns_mv')}/{x['ripple'].get('raw_span_999_mv')}")
    for k in ("A", "B"):
        if k in d:
            print(" ", k, json.dumps({kk: v for kk, v in d[k].items() if kk != "load"}))
    if "B" in d:
        print("  load", d["B"].get("load"), d["B"].get("load_fault"))
    print("  pd", d.get("pd"), "after", d["after"]["state"])
