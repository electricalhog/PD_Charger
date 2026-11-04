"""Decode USB PD BMC traffic from a scope CSV of CC1 (bu scope capture ... --raw).

usage: pd_decode.py CAPTURE.csv [--col CH2] [--idle V] [--around T0,T1]
Prints one line per burst: time (s, relative to the trigger), SOP or ordered set, sender (source/sink from the
header's power-role bit), message, message ID, CRC check.  A burst that does not decode is printed with its length.
"""
import argparse, csv, statistics as st, sys, zlib

DATA5 = {0b11110: 0x0, 0b01001: 0x1, 0b10100: 0x2, 0b10101: 0x3, 0b01010: 0x4, 0b01011: 0x5, 0b01110: 0x6,
         0b01111: 0x7, 0b10010: 0x8, 0b10011: 0x9, 0b10110: 0xA, 0b10111: 0xB, 0b11010: 0xC, 0b11011: 0xD,
         0b11100: 0xE, 0b11101: 0xF}
K = {0b11000: "S1", 0b10001: "S2", 0b00110: "S3", 0b00111: "R1", 0b11001: "R2", 0b01101: "EOP"}
ORDSETS = {("S1", "S1", "S1", "S2"): "SOP", ("S1", "S1", "S3", "S3"): "SOP'", ("S1", "S3", "S1", "S3"): "SOP''",
           ("R1", "R1", "R1", "R2"): "HARD_RESET", ("R1", "S1", "R1", "S3"): "CABLE_RESET"}
CTRL = {1: "GoodCRC", 2: "GotoMin", 3: "Accept", 4: "Reject", 5: "Ping", 6: "PS_RDY", 7: "Get_Source_Cap",
        8: "Get_Sink_Cap", 9: "DR_Swap", 10: "PR_Swap", 11: "VCONN_Swap", 12: "Wait", 13: "Soft_Reset",
        14: "Data_Reset", 16: "Not_Supported", 17: "Get_Source_Cap_Extended", 18: "Get_Status"}
DATA = {1: "Source_Capabilities", 2: "Request", 3: "BIST", 4: "Sink_Capabilities", 5: "Battery_Status",
        6: "Alert", 7: "Get_Country_Info", 8: "Enter_USB", 9: "EPR_Request", 10: "EPR_Mode", 15: "Vendor_Defined"}
EXT = {1: "Source_Capabilities_Extended", 2: "Status", 13: "Extended_Control_?", 16: "Extended_Control",
       17: "EPR_Source_Capabilities", 18: "EPR_Sink_Capabilities"}
EXTCTRL = {1: "EPR_Get_Source_Cap", 2: "EPR_Get_Sink_Cap", 3: "EPR_KeepAlive", 4: "EPR_KeepAlive_Ack"}


def load(path, col):
    rows = csv.reader(open(path))
    hdr = next(rows)
    ti, ci = 0, hdr.index(col) if col in hdr else 1
    t, v = [], []
    for r in rows:
        t.append(float(r[ti])); v.append(float(r[ci]))
    return t, v


def transitions(t, v, idle, hyst):
    out, state = [], None
    for ti, x in zip(t, v):
        s = 1 if x > idle + hyst else (0 if x < idle - hyst else state)
        if state is not None and s is not None and s != state:
            out.append(ti)
        state = s if s is not None else state
    return out


def bursts(tr, gap=20e-6):
    b, cur = [], []
    for x in tr:
        if cur and x - cur[-1] > gap:
            b.append(cur); cur = []
        cur.append(x)
    if cur:
        b.append(cur)
    return b


def bits_of(edges):
    iv = [b - a for a, b in zip(edges, edges[1:])]
    # The preamble (64 bits 0101...) has 96 transitions over 64 UI.
    ui = (edges[96] - edges[0]) / 64.0 if len(edges) > 100 else 3.33e-6
    bits, k = [], 0
    while k < len(iv):
        if iv[k] > 0.75 * ui:
            bits.append(0); k += 1
        else:
            bits.append(1); k += 2   # two half-UI intervals
    return bits, ui


def sym(bits, i):
    return sum(b << j for j, b in enumerate(bits[i:i + 5])) if i + 5 <= len(bits) else None


def decode(bits):
    for p in range(20, min(len(bits) - 20, 90)):
        ks = tuple(K.get(sym(bits, p + 5 * j)) for j in range(4))
        name = ORDSETS.get(ks)
        if name is None:
            # one corrupted K-code is allowed (spec: 3 of 4)
            for o, n in ORDSETS.items():
                if sum(a == b for a, b in zip(o, ks)) >= 3 and all(x is not None for x in ks[:2]):
                    name = n
                    break
        if name is None:
            continue
        if name in ("HARD_RESET", "CABLE_RESET"):
            return dict(set=name)
        q, nib = p + 20, []
        while True:
            s = sym(bits, q)
            if s is None:
                return dict(set=name, err="no EOP", nibbles=len(nib))
            if K.get(s) == "EOP":
                break
            if s not in DATA5:
                return dict(set=name, err=f"bad symbol at {len(nib)}", nibbles=len(nib))
            nib.append(DATA5[s]); q += 5
        by = bytes(nib[i] | (nib[i + 1] << 4) for i in range(0, len(nib) - 1, 2))
        if len(by) < 6:
            return dict(set=name, err=f"{len(by)} bytes")
        crc_ok = (zlib.crc32(by[:-4]) & 0xFFFFFFFF) == int.from_bytes(by[-4:], "little")
        h = int.from_bytes(by[:2], "little")
        typ, ndo, ext, mid, prole = h & 0x1F, (h >> 12) & 7, h >> 15, (h >> 9) & 7, (h >> 8) & 1
        if ext:
            msg = EXT.get(typ, f"ext{typ}")
            if typ == 16 and len(by) >= 5:
                msg = EXTCTRL.get(by[4], f"ExtCtrl{by[4]}")
        elif ndo == 0:
            msg = CTRL.get(typ, f"ctrl{typ}")
        else:
            msg = DATA.get(typ, f"data{typ}")
        return dict(set=name, hdr=f"0x{h:04x}", msg=msg, id=mid, sender="source" if prole else "sink",
                    crc_ok=crc_ok, nbytes=len(by))
    return dict(err="no ordered set", nbits=len(bits))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("csv"); ap.add_argument("--col", default="CH2")
    ap.add_argument("--idle", type=float, default=None); ap.add_argument("--hyst", type=float, default=0.25)
    ap.add_argument("--around", default=""); ap.add_argument("--ma", type=float, default=0.4e-6)
    a = ap.parse_args()
    t, v = load(a.csv, a.col)
    n = max(1, int(round(a.ma / (t[1] - t[0]))))   # moving average: switching spikes out, BMC edges stay
    if n > 1:
        acc, out = sum(v[:n]), []
        for k in range(n, len(v)):
            out.append(acc / n); acc += v[k] - v[k - n]
        t, v = t[n // 2:n // 2 + len(out)], out
    idle = a.idle if a.idle is not None else 0.85   # BMC swings about 0..1.75 V on this bench (CC idles at 1.68 V)
    tr = transitions(t, v, idle, a.hyst)
    lo, hi = (map(float, a.around.split(","))) if a.around else (-1e9, 1e9)
    prev_end = None
    for b in bursts(tr):
        if not (lo <= b[0] <= hi):
            prev_end = b[-1]; continue
        bits, ui = bits_of(b)
        d = decode(bits)
        gap = f"{(b[0] - prev_end) * 1e6:8.1f}us" if prev_end is not None else "        -"
        print(f"{b[0]:+.6f} s  gap {gap}  len {(b[-1] - b[0]) * 1e6:6.1f}us  UI {ui * 1e6:.2f}us  {d}")
        prev_end = b[-1]
    print(f"idle {idle:.3f} V, {len(tr)} transitions", file=sys.stderr)


if __name__ == "__main__":
    main()
