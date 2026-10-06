import struct

from bringup import pd


def frame(ttype, tick, port, sop, data=b"", tag=None):
    """One stack trace frame as usbpd_trace.c writes it."""
    val = bytes([ttype]) + struct.pack("<I", tick) + bytes([port, sop]) + struct.pack(">H", len(data)) + data
    tag = ((port + 1) << 5 | pd.DEBUG_STACK_MESSAGE) if tag is None else tag
    return pd.SOF + bytes([tag]) + struct.pack(">H", len(val)) + val + pd.EOF


def msg(mtype, objs=(), msg_id=0, ext=False, body=b""):
    hdr = mtype | (2 << 6) | (1 << 8) | (msg_id << 9) | (len(objs) << 12) | (int(ext) << 15)
    return struct.pack("<H", hdr) + b"".join(struct.pack("<I", o) for o in objs) + body


def fixed(mv, ma, flags=0):
    return (mv // 50) << 10 | ma // 10 | flags


def test_parse_frames_skips_garbage_and_keeps_partial_tail():
    good = frame(6, 10, 0, 0, b"hello")
    buf = b"\x00\x13junk" + good + good[:7]
    frames, skipped = pd.parse_frames(buf)
    assert len(frames) == 1 and skipped == 6


def test_decode_source_caps_request_and_ps_rdy():
    caps = msg(1, [fixed(5000, 1500, 1 << 23), fixed(20000, 1500)])
    req = msg(2, [(2 << 28) | (150 << 10) | 150 | (1 << 22)])
    buf = frame(2, 100, 0, 0, caps) + frame(1, 101, 0, 0, req) + frame(2, 102, 0, 0, msg(6))
    d = pd.decode_trace(buf)
    evs = d["events"]
    assert evs[0]["msg"]["name"] == "Source_Capabilities"
    assert evs[0]["msg"]["pdos"][0] == {"pos": 1, "type": "fixed", "mv": 5000, "ma": 1500, "flags": ["EPR_capable"]}
    assert evs[1]["dir"] == "<-" and evs[1]["msg"]["rdo"]["pos"] == 2 and evs[1]["msg"]["rdo"]["epr_capable"]
    assert evs[2]["msg"]["name"] == "PS_RDY"
    assert "#2 20V/1.5A" in pd._line(evs[0])


def test_decode_epr_messages():
    mode = msg(10, [(3 << 24)])                                   # EPR_Mode Enter_Succeeded
    fail = msg(10, [(4 << 24) | (1 << 16)])                       # Enter_Failed, cable not EPR
    keep = msg(16, ext=True, body=struct.pack("<H", 2 | 1 << 15) + bytes([3, 0]))
    epr_caps_body = b"".join(struct.pack("<I", p) for p in [fixed(5000, 1500)] + [0] * 6 + [fixed(28000, 1500)])
    epr_caps = msg(17, ext=True, body=struct.pack("<H", len(epr_caps_body) | 1 << 15) + epr_caps_body)
    ereq = msg(9, [(8 << 28) | (150 << 10) | 150 | (1 << 22), fixed(28000, 1500)])
    d = pd.decode_trace(b"".join(frame(t, i, 0, 0, m) for i, (t, m) in
                                 enumerate([(2, mode), (2, fail), (1, keep), (2, epr_caps), (1, ereq)])))
    e = [x["msg"] for x in d["events"]]
    assert e[0]["epr_mode"]["action"] == "Enter_Succeeded"
    assert e[1]["epr_mode"] == {"action": "Enter_Failed", "data": "cable not EPR capable"}
    assert e[2]["name"] == "Extended_Control" and e[2]["control"] == "EPR_KeepAlive"
    assert [p["pos"] for p in e[3]["pdos"]] == [1, 8] and e[3]["pdos"][1]["mv"] == 28000
    assert e[4]["name"] == "EPR_Request" and e[4]["rdo"]["pos"] == 8 and e[4]["copy_of_pdo"]["mv"] == 28000


def test_decode_cad_notify_debug_hard_reset_and_gui_frames():
    buf = (frame(3, 1, 0, 2) + frame(9, 2, 0, 0, bytes([113])) + frame(6, 3, 0, 0, b"PD_POWER: x")
           + frame(2, 4, 0, 5) + pd.SOF + bytes([0x0C]) + struct.pack(">H", 2) + b"\x01\x02" + pd.EOF)
    d = pd.decode_trace(buf)
    ev = d["events"]
    assert ev[0]["type"] == "CADEVENT" and ev[1]["type"] == "NOTIF"
    assert ev[2]["text"] == "PD_POWER: x"
    assert ev[3]["msg"]["name"] == "HARD_RESET"
    assert ev[4]["gui"] == "DPM_MESSAGE_IND"


def test_decode_pdo_apdos_and_rdo():
    pps = (3 << 30) | (210 << 17) | (33 << 8) | 60
    avs = (3 << 30) | (1 << 28) | (480 << 17) | (150 << 8) | 140
    assert pd.decode_pdo(pps) == {"type": "SPR_PPS", "max_mv": 21000, "min_mv": 3300, "max_ma": 3000}
    assert pd.decode_pdo(avs) == {"type": "EPR_AVS", "max_mv": 48000, "min_mv": 15000, "pdp_w": 140}
    r = pd.decode_rdo((9 << 28) | (1 << 26) | (100 << 10) | 120)
    assert r["pos"] == 9 and r["op_ma"] == 1000 and r["max_ma"] == 1200 and r["mismatch"]


def test_status_field_count_matches_firmware_struct():
    # pd_power.h PdPowerStatus: 9 scalars, spr_pdo[7], epr_pdo[6], 16 scalars, all 32-bit (152 B in the ELF).
    assert len(pd.STATUS_FIELDS) == 9 + 7 + 6 + 16
