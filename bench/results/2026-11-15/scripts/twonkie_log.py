"""Log the Twonkie (USB-PD sniffer, in line on the Type-C port) while a real load is plugged in (bench 2026-11-15).

Passive only: `tw trace on` (PD packets on the console) and `tw vbus` (its INA: VBUS mV and mA) about 8 times a
second, with the G474's regulator status twice a second.  No CC, sink or inject command is sent.

With TRACE_MV (second argument) the regulator trace is armed on |PID error| > TRACE_MV, 1000 samples (50 ms) after
the trigger and 1048 before.  It is read out and armed again whenever it completes, and read once at a regulator
fault (the ring then holds the last 102 ms before the fault, triggered or not) together with the fault snapshot.
The fault is never cleared.

  uv run --quiet --project tools/bringup python bench/results/2026-11-15/scripts/twonkie_log.py [SECONDS [TRACE_MV]]
Output: bench/results/2026-11-15/twonkie_<time>.jsonl (one object per line: tw = a console line, reg = a status,
trace = a trace read out to traces/twonkie_<time>_<n>.csv).
"""
import json, re, shutil, sys, threading, time
from pathlib import Path
import serial
from bringup import probe, regulator
from bringup.config import REPO_ROOT, load_config

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0
trace_mv = int(sys.argv[2]) if len(sys.argv) > 2 else 0
stamp = time.strftime('%Y%m%d_%H%M%S')
out = Path(__file__).resolve().parent.parent / f"twonkie_{stamp}.jsonl"
cfg, t0, lock = load_config(), time.monotonic(), threading.Lock()
f = out.open("w")


def put(kind, **kw):
    with lock:
        f.write(json.dumps({"t": round(time.monotonic() - t0, 3), "k": kind, **kw}) + "\n"); f.flush()


def arm():
    r = regulator.command(cfg, "trace-arm", 3.0, arg=regulator.trace_arg("error", 1000, trace_mv))
    put("trace_arm", result=r.get("result"))


def trace_out(n, why):
    r = regulator.trace_read(cfg)
    dst = out.parent / "traces" / f"twonkie_{stamp}_{n}.csv"
    dst.parent.mkdir(exist_ok=True)
    shutil.copyfile(Path(REPO_ROOT) / r["file"], dst)
    put("trace", why=why, file=dst.name, **{k: r.get(k) for k in ("state", "samples", "count", "trigger_sample",
                                                                   "v_out_mv", "ccm_samples", "switch_edges_ms")})


def reg():
    n, fault_done = 0, False
    if trace_mv:
        arm()
    while time.monotonic() - t0 < secs:
        try:
            st = regulator.status(cfg); c, a = st["control"], st["adc"]
            put("reg", state=st["state"], fault=st["fault_source"], target_mv=c.get("target_mv"), mode=c.get("mode"),
                ccm=c.get("ccm_active"), v_out_mv=a.get("v_out_mv"), v_in_mv=a.get("v_in_mv"),
                i_out_ma=c.get("ccm_i_avg_ma"), i_in_ma=c.get("i_in_filt_ma"))
            if trace_mv and st["state"] == "FAULT" and not fault_done:
                fault_done = True
                trace_out(n, "fault"); n += 1
                put("fault", snapshot=probe.mem_read(cfg, "regulator_fault_snapshot", dtype="u32", count=6)["values"],
                    vd_raw=probe.mem_read(cfg, "regulator_fault_trace", dtype="u16", count=64)["values"])
            elif trace_mv and st["state"] != "FAULT":
                if probe.mem_read(cfg, "regulator_trace_ctl", dtype="u32", count=1)["values"][0] == 3:
                    trace_out(n, "done"); n += 1
                    arm()
        except Exception as e:
            put("reg_error", e=str(e)[:100])
        time.sleep(0.3 if trace_mv else 0.5)


threading.Thread(target=reg, daemon=True).start()
s = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.05)
s.reset_input_buffer(); s.write(b"tw trace on\n")
buf, nxt = b"", 0.0
vb = re.compile(r"VBUS = (-?\d+) mV ; (-?\d+) mA")
try:
    while time.monotonic() - t0 < secs:
        if time.monotonic() >= nxt:
            s.write(b"tw vbus\n"); nxt = time.monotonic() + 0.12
        buf += s.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace").strip("\r >")
            if not line or line == "tw vbus":
                continue
            m = vb.search(line)
            put("vbus", mv=int(m.group(1)), ma=int(m.group(2))) if m else put("tw", line=line)
finally:
    s.write(b"tw trace off\n"); time.sleep(0.2); s.close(); f.close()
print(out)
