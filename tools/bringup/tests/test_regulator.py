"""Regulator status decoding, diagnosis and multi-region reads (no hardware)."""

import struct

import pytest

from bringup import probe, regulator
from bringup.config import ToolError


def _raw(state=3, src=0x02, fault_tick=1000, tick=1050, idr=0, odr=0, isr=0, ier=0, oenr=0, mbox=(0, 0, 0, 0)):
    return {
        "regulator_state": bytes([state]),
        "regulator_last_fault_source": bytes([src]),
        "regulator_fault_tick_ms": struct.pack("<I", fault_tick),
        "regulator_debug": struct.pack("<4I", *mbox),
        "uwTick": struct.pack("<I", tick),
        "adc_measurements": struct.pack("<5I", 12000, 20000, 0, 0, 0),
        "gpioa_idr": struct.pack("<I", idr),
        "gpioc_odr": struct.pack("<I", odr),
        "hrtim": struct.pack("<4I", isr, 0, ier, oenr),
    }


def test_decode_overcurrent_fault_with_cooldown():
    s = regulator.decode(_raw(state=3, src=0x02, idr=(1 << 12), odr=(1 << 9), isr=2))
    assert s["state"] == "FAULT" and s["fault_source"] == ["HW_FLT2"]
    assert s["lines"] == {"VS_GOOD_PA12": 1, "IS_GOOD_PA15": 0, "INPUT_EN_PC7": 0,
                          "OUTPUT_EN_PC8": 0, "OUTPUT_DIS_PC9": 1}
    text = "\n".join(s["diagnosis"])
    assert "over-current trip" in text
    assert "another 100 ms" in text  # 150 ms firmware cool-down, 50 ms elapsed
    assert "clear-fault" in text
    assert s["adc"]["v_in_mv"] == 20000


def test_diagnose_flags_running_without_armed_irq_and_outputs_when_idle():
    run = regulator.decode(_raw(state=2, src=0, odr=(1 << 7) | (1 << 8), idr=(1 << 12) | (1 << 15), ier=0, oenr=0xF))
    assert any("not armed" in x for x in run["diagnosis"])
    idle = regulator.decode(_raw(state=1, src=0, oenr=0b0101))
    assert idle["hrtim"]["outputs_enabled"] == ["TA1", "TB1"]
    assert any(x.startswith("DANGER") for x in idle["diagnosis"])


def test_tick_wraparound_fault_age():
    s = regulator.decode(_raw(fault_tick=0xFFFFFFF0, tick=0x10))
    assert s["fault_age_ms"] == 0x20


class _Cfg(dict):
    pass


def test_command_refuses_repeat_overcurrent_without_force(monkeypatch):
    raw = _raw(state=3, mbox=(0, 3, 5, 1))
    monkeypatch.setattr(regulator, "read_raw", lambda cfg: raw)
    with pytest.raises(ToolError, match="repeat ADM1270 over-current"):
        regulator.command({}, "clear-fault")


def test_command_clear_fault_roundtrip(monkeypatch):
    """Fake firmware: services the request on the first poll after the write."""
    state = {"raw": _raw(state=3, mbox=(0, 0, 7, 0)), "written": None}
    monkeypatch.setattr(regulator, "read_raw", lambda cfg: state["raw"])
    monkeypatch.setattr(probe, "symbols", lambda cfg: {"regulator_debug": (0x20006818, 16)})

    def fake_write(cfg, target, value, dtype):
        assert (target, dtype) == ("0x20006818", "u32")
        state["written"] = int(value)
        state["raw"] = _raw(state=1, src=0, mbox=(0, 0, 8, int(value)))
        return {"ok": False}  # firmware may already have zeroed request before readback
    monkeypatch.setattr(probe, "mem_write", fake_write)
    monkeypatch.setattr(regulator.time, "sleep", lambda s: None)

    r = regulator.command({}, "clear-fault")
    assert state["written"] == 1
    assert r["ok"] and r["result"] == "OK" and r["status"]["state"] == "IDLE"


def test_read_regions_single_session(monkeypatch):
    calls = []
    out = ("Reading 32-bit memory content\n"
           "0x20006808 : 00010003 000003E8\n"
           "0x48000010 : 00009000\n")

    def fake_prun(cfg, args, timeout):
        calls.append(args)
        return out, 0
    monkeypatch.setattr(probe, "_prun", fake_prun)
    got = probe.read_regions({}, [(0x20006809, 1), (0x2000680C, 4), (0x48000010, 4)])
    assert len(calls) == 1 and calls[0].count("-r32") == 3
    assert got == [b"\x00", struct.pack("<I", 1000), struct.pack("<I", 0x9000)]


def test_set_voltage_writes_arg_then_request(monkeypatch):
    """Newer firmware: 20-byte mailbox, arg at +16 written before the request."""
    state = {"raw": _raw(state=2, mbox=(0, 0, 3, 0)), "writes": []}
    monkeypatch.setattr(regulator, "read_raw", lambda cfg: state["raw"])
    monkeypatch.setattr(probe, "symbols", lambda cfg: {"regulator_debug": (0x20006818, 20)})

    def fake_write(cfg, target, value, dtype):
        state["writes"].append((target, int(value)))
        if target == "0x20006818":
            state["raw"] = _raw(state=2, mbox=(0, 0, 4, int(value)))

    monkeypatch.setattr(probe, "mem_write", fake_write)
    monkeypatch.setattr(regulator.time, "sleep", lambda s: None)
    out = regulator.command({}, "set-voltage", mv=24000)
    assert state["writes"] == [("0x20006828", 24000), ("0x20006818", 5)]
    assert out["ok"] and out["result"] == "OK" and out["mv"] == 24000


def test_set_voltage_refuses_old_mailbox(monkeypatch):
    monkeypatch.setattr(regulator, "read_raw", lambda cfg: _raw(state=2))
    monkeypatch.setattr(probe, "symbols", lambda cfg: {"regulator_debug": (0x20006818, 16)})
    with pytest.raises(ToolError, match="no arg field"):
        regulator.command({}, "set-voltage", mv=24000)
