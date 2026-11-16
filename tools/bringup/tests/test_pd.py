"""pd_status decoding (no hardware)."""

import re
from pathlib import Path

import pytest

from bringup import pd
from bringup.config import ToolError

REPO = Path(__file__).resolve().parents[3]
PROJECT = REPO / "STM32CubeIDE/final"


def _words(**kw):
    w = dict.fromkeys(pd._FIELDS, 0)
    w.update(magic=pd.MAGIC, version=pd.VERSION)
    w.update(kw)
    return [w[f] for f in pd._FIELDS]


def test_field_order_matches_pd_policy_h():
    """The decoder's word order must be the C struct's member order."""
    src = (PROJECT / "Core/Inc/pd_policy.h").read_text()
    body = re.search(r"typedef struct\s*\{((?:(?!typedef struct).)*?)\}\s*PdStatus;", src, re.S).group(1)
    sizes = {"PD_POLICY_MAX_SPR_PDO": pd.SPR_MAX, "PD_POLICY_MAX_EPR_PDO": pd.EPR_MAX,
             "PD_STATUS_EVENTS": pd.EVENTS}
    fields = []
    for name, dim in re.findall(r"uint32_t\s+(\w+)(?:\[(\w+)\])?\s*;", body):
        fields += [f"{name}{i}" for i in range(sizes[dim])] if dim else [name]
    assert fields == pd._FIELDS


def test_decode_contract_epr_and_events():
    pdo1 = (100 << 10) | 50 | (1 << 23)          # 5 V 500 mA, EPR Mode Capable
    pdo2 = (180 << 10) | 50                      # 9 V 500 mA
    epr = (560 << 10) | 50                       # 28 V 500 mA
    rdo = (2 << 28) | (1 << 22) | (50 << 10) | 50
    w = _words(lib_epr=1, epr_offered=1, spr_pdo_count=2, epr_pdo_count=1, spr_pdo0=pdo1, spr_pdo1=pdo2,
               epr_pdo0=epr, attach_count=1, contract_count=2, last_rdo=rdo, contract_mv=9000,
               contract_ma=500, contract_position=2, transition_ms=210, epr_mode=1, epr_enter_requests=1,
               epr_enter_succeeded=1, ev_count=3, ev_tick0=900, ev_code0=0x102, ev_tick1=950, ev_code1=16,
               ev_tick2=990, ev_code2=114)
    s = pd.decode(w, 1000, {16: "POWER_EXPLICIT_CONTRACT", 114: "EPRMODE_SUCCEEDED"}, {2: "ATTACHED"})
    assert s["spr_pdos"][0]["mv"] == 5000 and "EPR_MODE_CAPABLE" in s["spr_pdos"][0]["flags"]
    assert s["epr_pdos"][0] == {"position": 8, "raw": f"{epr:#010x}", "type": "FIXED", "mv": 28000, "ma": 500}
    assert s["contract"] == {"mv": 9000, "ma": 500, "position": 2, "last_transition_ms": 210}
    assert s["last_rdo"]["position"] == 2 and s["last_rdo"]["epr_mode_capable"] and s["last_rdo"]["result"] == "ACCEPT"
    assert [e["event"] for e in s["events"]] == ["CAD_ATTACHED", "POWER_EXPLICIT_CONTRACT", "EPRMODE_SUCCEEDED"]
    assert s["events"][0]["age_ms"] == 100
    assert s["epr"]["in_epr_mode"] and s["diagnosis"] == []


def test_diagnosis_without_epr_library_and_failures():
    s = pd.decode(_words(attach_count=2, vbus_on_failures=1, transition_timeouts=1, epr_enter_failed=1,
                         last_rdo=(9 << 28), last_rdo_result=1), 0, {}, {})
    text = "\n".join(s["diagnosis"])
    assert "without EPR" in text and "unable to start" in text and "tPSTransition" in text
    assert "cable not EPR capable" in text and "BAD_POSITION" in text


def test_event_ring_wraps_to_last_32():
    kw = {"ev_count": 40}
    kw.update({f"ev_code{i}": 0x200 for i in range(pd.EVENTS)})
    s = pd.decode(_words(**kw), 0, {}, {})
    assert len(s["events"]) == pd.EVENTS and s["events"][0]["seq"] == 8
    assert s["events"][-1]["event"] == "FAULT_HARD_RESET"


def test_bad_magic_is_an_error():
    w = _words()
    w[0] = 0
    with pytest.raises(ToolError):
        pd.decode(w, 0, {}, {})


def test_notification_names_parse_from_project_header():
    names = pd._enum(PROJECT / "Middlewares/ST/STM32_USBPD_Library/Core/inc/usbpd_def.h", "USBPD_NOTIFY_")
    assert names[16] == "POWER_EXPLICIT_CONTRACT" and names[30] == "HARDRESET_RX"
