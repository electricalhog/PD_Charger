"""Read and print the source's UCPD event ring (usbpd_phy_hw_if.c, ucpd_ring) over SWD."""
import json, pathlib, subprocess

ROOT = str(pathlib.Path(__file__).resolve().parents[4])
KIND = {1: "RX end", 2: "send", 3: "SEND REFUSED", 4: "HARD RESET RX"}
CTRL = {1: "GoodCRC", 3: "Accept", 6: "PS_RDY", 13: "Soft_Reset"}


def rd(sym, n):
    r = subprocess.run([f"{ROOT}/tools/bringup/bu", "mem", "read", sym, "--type", "u32", "--count", str(n)],
                       cwd=ROOT, capture_output=True, text=True)
    return json.loads(r.stdout)["values"]


def name(h):
    typ, ndo, ext = h & 0x1F, (h >> 12) & 7, h >> 15
    if ext:
        return f"ext{typ}"
    return CTRL.get(typ, f"ctrl{typ}") if ndo == 0 else f"data{typ}"


ring, idx, frz = rd("ucpd_ring", 64), rd("ucpd_ring_idx", 1)[0], rd("ucpd_ring_freeze", 1)[0]
try:
    hp = rd("ucpd_ring_hp", 192)
except Exception:
    hp = [0] * 192
ev = []
for j in range(32):
    k = (idx + j) % 32
    cyc, w = ring[2 * k], ring[2 * k + 1]
    if w:
        ev.append((cyc, w >> 24, (w >> 16) & 0xFF, w & 0xFFFF, hp[6 * k:6 * k + 6]))
t_end = ev[-1][0]
print(f"idx {idx} frozen {frz}")
prev = prev_hp = None
print("hp: us spent since the previous entry in TIM6 / TIM7 PID / TIM4 / HRTIM A / ADC1_2 / HRTIM fault")
for cyc, kind, ipsr, h, hpv in ev:
    d_hp = [round(((a - b) & 0xFFFFFFFF) / 170.0) for a, b in zip(hpv, prev_hp)] if prev_hp else []
    prev_hp = hpv
    dt = ((cyc - prev) & 0xFFFFFFFF) / 170.0 if prev is not None else 0.0
    rel = -(((t_end - cyc) & 0xFFFFFFFF) / 170.0)
    who = "src" if h >> 8 & 1 else "snk"
    print(f"{rel / 1000:10.3f} ms  +{dt:9.1f} us  {KIND.get(kind, kind):14s} ipsr {ipsr:3d}  0x{h:04x} {who} {name(h)} id {(h >> 9) & 7}  hp {d_hp}")
    prev = cyc
