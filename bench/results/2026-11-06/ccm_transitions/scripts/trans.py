"""One buck CCM transition captured on the scope: SW1 (CH1), SW2 (CH3), TP2 (CH4, AC, 20 MHz BWL), 100 us/div,
300k points per channel (4 ns), CH1 pulse-width trigger at V_in / 2 on the first pulse of the new mode:
  entry: regulator_ccm_enable 0 -> 2 (forced), trigger on the first pulse wider than W (DCM t_on < W < CCM's)
  exit:  regulator_ccm_enable 2 -> 0 (exit DISABLED; auto would not exit at 20 V once ID_MON reads right), trigger on the first pulse narrower than W
The PPS contract (MV) is set first; QT Py off, 25.3 ohm only.  Writes over SWD: regulator_ccm_enable (Dan,
2026-11-06).  Telemetry after the step: regulator_ccm_entry/exit_dt_ns and _iv_ma, handover timeouts.
usage: trans.py entry|exit MV W_NS TAG        appends to ../data/trans.jsonl (the CSV stays in the bu capture dir)
"""
import json, pathlib, sys, time
sys.path.insert(0, "/home/daniel/GitHub/PD_Charger/bench/results/2026-11-03/scripts")
from benchlib import bu, w, status_n
OUT = pathlib.Path(__file__).resolve().parents[1] / "data" / "trans.jsonl"
TELEM = ("regulator_ccm_handover", "regulator_id_ccm_corr", "regulator_id_gain_permille", "regulator_id_offset_ma",
         "regulator_ccm_entry_dt_ns", "regulator_ccm_entry_iv_ma", "regulator_ccm_exit_dt_ns",
         "regulator_ccm_exit_iv_ma", "regulator_ccm_exit_i0_ma", "regulator_ccm_handover_timeouts")
SIGNED16 = {"regulator_ccm_entry_dt_ns", "regulator_ccm_entry_iv_ma", "regulator_ccm_exit_dt_ns",
            "regulator_ccm_exit_iv_ma", "regulator_ccm_exit_i0_ma", "regulator_id_offset_ma"}


def rd(sym):
    d = bu("mem", "read", sym)
    b = bytes.fromhex(d.get("hex", "00").replace(" ", ""))
    return int.from_bytes(b, "little", signed=sym in SIGNED16)


def scpi(c):
    return bu("scope", "scpi", c)


def setup(vin, vout, when, w_ns):
    bu("scope", "chan", "2", "--off")
    bu("scope", "chan", "1", "--on", "--probe", "10", "--scale", "5", "--offset", "-12", "--coupling", "DC", "--bwl", "OFF")
    bu("scope", "chan", "3", "--on", "--probe", "10", "--scale", "5", "--offset", "-12", "--coupling", "DC", "--bwl", "OFF")
    bu("scope", "chan", "4", "--on", "--probe", "10", "--scale", "0.5", "--offset", "0", "--coupling", "AC", "--bwl", "20M")
    bu("scope", "timebase", "--scale", "1e-4", "--offset", "3e-4")   # trigger at 1/4: -300 .. +900 us
    bu("scope", "run"); time.sleep(0.3)
    bu("scope", "acquire", "--mdepth", "300k", "--type", "NORM")
    for c in (":TRIGger:MODE PULSe", ":TRIGger:PULSe:SOURce CHAN1", f":TRIGger:PULSe:WHEN {when}",
              f":TRIGger:PULSe:WIDTh {w_ns * 1e-9:.4e}", f":TRIGger:PULSe:LEVel {vin / 2:.2f}", ":TRIGger:SWEep SINGle"):
        scpi(c)
    bu("scope", "single"); time.sleep(0.8)


def main():
    kind, mv, w_ns, tag = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    r = bu("sink", "pps", mv, 1500, "--until", "contract", "--timeout", 3)
    assert r.get("matched"), r
    pre, post = (0, 2) if kind == "entry" else (2, 0)
    w("regulator_ccm_enable", pre, "u8"); time.sleep(1.5)
    s0 = status_n(6, 0.1)
    assert s0["state"] == "RUNNING" and not s0["fault"], s0
    vin, vout = s0["vin"] / 1000, s0["v"] / 1000
    setup(vin, vout, "PGReater" if kind == "entry" else "PLESs", w_ns)
    w("regulator_ccm_enable", post, "u8")
    trig = bu("scope", "wait", "--no-arm", "--timeout", 4)
    time.sleep(0.5)
    s1 = status_n(6, 0.1)
    tel = {k.replace("regulator_", ""): rd(k) for k in TELEM}
    c = bu("scope", "capture", "CH1", "CH3", "CH4", "--raw", t=180) if trig.get("triggered", True) else {}
    rec = dict(kind=kind, mv=mv, w_ns=w_ns, tag=tag, t=time.strftime("%H:%M:%S"), trig=trig, file=c.get("file"),
               v_in=vin, v_out=vout, i_v_over_r_ma=round(vout / 25.3 * 1e3, 1), pre=s0, post=s1, **tel)
    print(json.dumps({k: rec[k] for k in ("kind", "mv", "tag", "file", "v_out")} |
                     {k: v for k, v in tel.items()} | {"ccm_after": s1["ccm"], "state": s1["state"], "fault": s1["fault"],
                                                       "why": s1["why"], "trig": trig.get("triggered", trig)}))
    with open(OUT, "a") as f:
        f.write(json.dumps(rec) + "\n")


if __name__ == "__main__":
    main()
