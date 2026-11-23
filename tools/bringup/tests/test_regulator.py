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

    def fake_write(cfg, target, value, dtype, **kw):
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

    def fake_write(cfg, target, value, dtype, **kw):
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


def test_trace_arg_packing():
    assert regulator.trace_arg("switch", 1800) == 1 | (1800 << 4)
    assert regulator.trace_arg("error", 100, 1500) == 3 | (100 << 4) | (150 << 16)
    with pytest.raises(ToolError):
        regulator.trace_arg("bogus", 10)
    with pytest.raises(ToolError):
        regulator.trace_arg("now", regulator.TRACE_LEN)


def test_pulse_us_rounds_to_microseconds_inside_the_firmware_range():
    assert regulator.pulse_us(1.0) == 1000
    assert regulator.pulse_us(1.4375) == 1438
    assert regulator.pulse_us(50) == 50000
    for bad in (0.0, 0.0004, 50.001):
        with pytest.raises(ToolError):
            regulator.pulse_us(bad)


def test_trace_read_orders_the_ring_and_aligns_the_trigger(monkeypatch, tmp_path):
    n = regulator.TRACE_LEN
    buf = bytearray(n * regulator.TRACE_SAMPLE.size)
    # Ring wrapped (count > n), next write at 100, trigger at slot 1000: oldest sample is slot 100.
    for idx in range(n):
        age = (idx - 100) % n                       # 0 = oldest
        switch_on = 0x08 if not (900 <= age < 1000) else 0
        flags = (2 | switch_on | (0x80 if idx == 1000 else 0) | (0x100 if age >= 2000 else 0)
                 | (0x200 if age >= 2040 else 0))
        regulator.TRACE_SAMPLE.pack_into(buf, idx * regulator.TRACE_SAMPLE.size, 28000 + age % 7, 1400, 1680, 1790, flags)
    ctl = regulator.TRACE_CTL.pack(3, 1, 1147, 0, 100, 1000, n + 500, 0, 5000, 850_034)
    monkeypatch.setattr(probe, "symbols", lambda cfg: {"regulator_trace_ctl": (0x1000, 40),
                                                       "regulator_trace": (0x2000, len(buf))})
    monkeypatch.setattr(probe, "read_regions", lambda cfg, regions: [ctl, bytes(buf)])
    monkeypatch.setattr(regulator, "out_file", lambda cfg, stem, ext: tmp_path / f"{stem}.{ext}")
    monkeypatch.setattr(regulator, "rel", lambda p: str(p))
    out = regulator.trace_read({})
    assert out["state"] == "DONE" and out["trigger"] == "switch" and out["samples"] == n
    assert out["trigger_sample"] == 900                # slot 1000 is 900 samples after slot 100
    assert out["switch_edges_ms"] == [0.0, 5.0]        # opens at the trigger, recloses 100 samples later
    assert out["pulse_us"] == 5000 and out["pulse_us_measured"] == 5000.2
    lines = (tmp_path / "regulator_trace.csv").read_text().splitlines()
    assert lines[0].startswith("t_ms,v_out_mv") and lines[1].startswith("-45.00,28000,")
    cols = lines[0].split(",")
    assert lines[901].split(",")[cols.index("trigger")] == "1"     # the trigger row
    assert lines[2001].split(",")[cols.index("ccm")] == "1" and lines[1].split(",")[cols.index("ccm")] == "0"
    assert out["ccm_samples"] == n - 2000
    assert lines[2041].split(",")[cols.index("ilim")] == "1" and lines[2001].split(",")[cols.index("ilim")] == "0"
    assert out["ilim_samples"] == n - 2040


def test_decode_ccm_limit_telemetry():
    raw = _raw(state=2, src=0)
    raw.update({"regulator_ccm_ilim_ma": struct.pack("<H", 500), "regulator_ccm_limiting": bytes([1]),
                "regulator_il_ma": struct.pack("<i", -120), "regulator_ccm_v_lim_mv": struct.pack("<f", 12650.5),
                "regulator_il_gain_permille": struct.pack("<h", 1032), "regulator_il_offset_ma": struct.pack("<h", -15)})
    ctl = regulator.decode(raw)["control"]
    assert ctl["ccm_ilim_ma"] == 500 and ctl["ccm_limiting"] == 1 and ctl["il_ma"] == -120
    assert ctl["ccm_v_lim_mv"] == 12650.5
    assert ctl["il_gain_permille"] == 1032 and ctl["il_offset_ma"] == -15


def test_ccm_pwm_arg_packing():
    assert regulator.ccm_pwm_arg(2100, 400) == 2100 | (400 << 16)
    with pytest.raises(ToolError):
        regulator.ccm_pwm_arg(2100, 300)
    with pytest.raises(ToolError):
        regulator.ccm_pwm_arg(70000, 400)
