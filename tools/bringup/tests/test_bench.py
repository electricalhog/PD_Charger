"""Three-board sweep sequencing against a fake sink (no hardware)."""

import csv

import pytest

from bringup import bench, sink
from bringup.config import ToolError


class FakeBench:
    """Answers sink commands like the firmware; the load faults above fault_at_mw."""

    def __init__(self, fault_at_mw=None, load_state="off"):
        self.sent = []
        self.contract_mv = 5000
        self.p = 0
        self.fault_at_mw = fault_at_mw
        self.load_state = load_state

    def __call__(self, cfg, command, timeout=2.0, until_event=None, wait=0.0):
        self.sent.append(command)
        words = command.split()
        reply, events = [{"kind": "OK"}], []
        if words[0] == "status":
            reply = [sink.parse_line(f"STATUS attached=CC1 epr_mode=0 contract_mv={self.contract_mv} "
                                     f"contract_ma=500 hard_resets=0 vbus_mv={self.contract_mv} tcpp_ok=1 "
                                     "tcpp_fault=0 tcpp_flags=0x20 hold=0 load_online=1")]
        elif words[0] == "req":
            self.contract_mv = int(words[1])
            events = [sink.parse_line(f"EVT contract pos=2 mv={words[1]} ma=500 epr_mode=0")]
        elif command == "load status":
            state, fault = self.load_state, "none"
            if self.fault_at_mw is not None and self.p >= self.fault_at_mw:
                state, fault = "fault", "input_over_current"
            reply = [sink.parse_line(f"LOAD online=1 state={state} fault={fault} vin_mv={self.contract_mv} "
                                     f"vout_mv=3000 iout_ma=300 pout_mw={self.p} duty_pm=500 temp_mv=1600")]
        elif words[:2] == ["load", "p"]:
            self.p = int(words[2])
        elif command == "load off":
            self.p = 0
        r = {"ok": True, "reply": reply, "events": events, "command": command}
        if until_event:
            r["matched"] = any(e.get("event") == until_event for e in events)
            r["ok"] = r["matched"]
        return r


@pytest.fixture
def cfg(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "out_file", lambda c, stem, ext: tmp_path / f"{stem}.{ext}")
    monkeypatch.setattr(bench, "rel", lambda p: str(p))
    return {"sink": {"port": "/dev/fake"}, "probe": {"serial": ""}}


def test_sweep_steps_contracts_and_powers_and_ends_off(cfg, monkeypatch):
    fake = FakeBench(load_state="running")
    monkeypatch.setattr(sink, "exchange", fake)
    r = bench.sweep(cfg, [5000, 9000], [500, 1000], swd=False, dwell=0)
    assert r["ok"] and r["points"] == 4
    assert [c for c in fake.sent if c.startswith("req")] == ["req 5000 500", "req 9000 500"]
    # Every renegotiation is preceded by load off, and the run ends with it.
    for i, c in enumerate(fake.sent):
        if c.startswith("req"):
            assert fake.sent[i - 1] == "load off"
    assert fake.sent[-1] == "load off"
    rows = list(csv.DictReader(open(r["file"])))
    assert [(x["contract_target_mv"], x["p_target_mw"]) for x in rows] == [
        ("5000", "500"), ("5000", "1000"), ("9000", "500"), ("9000", "1000")]


def test_sweep_stops_at_load_fault(cfg, monkeypatch):
    fake = FakeBench(fault_at_mw=1000, load_state="running")
    monkeypatch.setattr(sink, "exchange", fake)
    r = bench.sweep(cfg, [5000, 9000], [500, 1000, 2000], swd=False, dwell=0)
    assert not r["ok"] and "input_over_current" in r["stopped"]
    assert r["points"] == 2 and fake.sent[-1] == "load off"
    assert not any(c == "req 9000 500" for c in fake.sent)


def test_preflight_refuses_telemetry_only_load_unless_allowed(cfg, monkeypatch):
    monkeypatch.setattr(sink, "exchange", FakeBench(load_state="no_power_stage"))
    with pytest.raises(ToolError) as e:
        bench.sweep(cfg, [5000], [500], swd=False, dwell=0)
    assert any("power-stage" in p for p in e.value.details["problems"])
    r = bench.sweep(cfg, [5000], [500], swd=False, dwell=0, allow_no_power_stage=True)
    assert r["points"] == 1


def test_swd_sweep_needs_probe_serial(cfg, monkeypatch):
    monkeypatch.setattr(sink, "exchange", FakeBench(load_state="off"))
    with pytest.raises(ToolError, match="probe"):
        bench.sweep(cfg, [5000], [500], swd=True, dwell=0)


def _ports(*serials):
    return {"ok": True, "ports": [{"device": f"/dev/ttyACM{i}", "serial": sn, "stlink": True, "vid": "0483",
                                   "pid": "374e", "description": "STLINK-V3"} for i, sn in enumerate(serials)]}


def test_devices_maps_roles_by_serial(monkeypatch):
    monkeypatch.setattr(bench, "list_ports", lambda: _ports("SRC1", "SNK2"))
    r = bench.devices({"probe": {"serial": "SRC1"}, "sink": {"stlink_serial": "SNK2"}})
    assert [(d["device"], d["role"]) for d in r["stlinks"]] == [
        ("/dev/ttyACM0", "source (G474)"), ("/dev/ttyACM1", "sink (G431)")]


def test_devices_flags_unmapped_or_missing(monkeypatch):
    monkeypatch.setattr(bench, "list_ports", lambda: _ports("SRC1", "SNK2"))
    with pytest.raises(ToolError) as e:
        bench.devices({"probe": {"serial": ""}, "sink": {}})
    assert len(e.value.details["problems"]) == 2
    with pytest.raises(ToolError, match="not mapped") as e:
        bench.devices({"probe": {"serial": "GONE"}, "sink": {"stlink_serial": "SNK2"}})
    assert "GONE is not attached" in e.value.details["problems"][0]
    # One board and nothing configured is fine.
    monkeypatch.setattr(bench, "list_ports", lambda: _ports("ONLY"))
    assert bench.devices({"probe": {}, "sink": {}})["stlinks"][0]["role"] is None
