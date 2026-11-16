"""Log the Twonkie (USB-PD sniffer, in line on the Type-C port) while a real load is plugged in (bench 2026-11-15).

Passive only: `tw trace on` (PD packets on the console) and `tw vbus` (its INA: VBUS mV and mA) about 8 times a
second, with the G474's regulator status twice a second.  No CC, sink or inject command is sent.

  uv run --quiet --project tools/bringup python bench/results/2026-11-15/scripts/twonkie_log.py [SECONDS]
Output: bench/results/2026-11-15/twonkie_<time>.jsonl (one object per line: tw = a console line, reg = a status).
"""
import json, re, sys, threading, time
from pathlib import Path
import serial
from bringup import regulator
from bringup.config import load_config

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0
out = Path(__file__).resolve().parent.parent / f"twonkie_{time.strftime('%Y%m%d_%H%M%S')}.jsonl"
cfg, t0, lock = load_config(), time.monotonic(), threading.Lock()
f = out.open("w")


def put(kind, **kw):
    with lock:
        f.write(json.dumps({"t": round(time.monotonic() - t0, 3), "k": kind, **kw}) + "\n"); f.flush()


def reg():
    while time.monotonic() - t0 < secs:
        try:
            st = regulator.status(cfg); c, a = st["control"], st["adc"]
            put("reg", state=st["state"], fault=st["fault_source"], target_mv=c.get("target_mv"), mode=c.get("mode"),
                ccm=c.get("ccm_active"), v_out_mv=a.get("v_out_mv"), v_in_mv=a.get("v_in_mv"),
                i_out_ma=c.get("ccm_i_avg_ma"), i_in_ma=c.get("i_in_filt_ma"))
        except Exception as e:
            put("reg_error", e=str(e)[:100])
        time.sleep(0.5)


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
