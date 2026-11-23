"""bench PD sink protocol: line parsing and one exchange over a fake port (no hardware)."""

import sys
import types

import pytest

from bringup import sink


def test_parse_event_status_and_pdo_lines():
    assert sink.parse_line("EVT contract pos=2 mv=9000 ma=500 epr_mode=0") == {
        "kind": "EVT", "event": "contract", "pos": 2, "mv": 9000, "ma": 500, "epr_mode": 0}
    st = sink.parse_line("STATUS attached=CC1 epr_mode=1 contract_mv=28000")
    assert st["attached"] == "CC1" and st["contract_mv"] == 28000
    assert sink.parse_line("ERR not attached") == {"kind": "ERR", "text": "not attached"}
    assert sink.parse_line("EVT epr_enter_failed reason=CableNotEprCapable")["reason"] == "CableNotEprCapable"
    assert sink.parse_line("") == {}


class FakeSerial:
    """Replies to `req` with OK, then the events a renegotiation produces."""

    script = {}

    def __init__(self, port, baud, timeout):
        self.out = b""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.out += self.script[data.decode().strip()]

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk


@pytest.fixture
def fake_port(monkeypatch):
    mod = types.SimpleNamespace(Serial=FakeSerial, SerialException=OSError)
    monkeypatch.setitem(sys.modules, "serial", mod)
    return FakeSerial


def test_req_until_contract(fake_port):
    fake_port.script = {"req 9000 400": b"OK\r\nEVT request pos=2 mv=9000 ma=400 epr_request=0\r\n"
                                        b"EVT contract pos=2 mv=9000 ma=400 epr_mode=0\r\n"}
    r = sink.exchange({"sink": {"port": "/dev/fake"}}, "req 9000 400", timeout=1.0, until_event="contract")
    assert r["ok"] and r["matched"]
    assert r["reply"] == [{"kind": "OK"}]
    assert [e["event"] for e in r["events"]] == ["request", "contract"]


def test_caps_reads_through_end(fake_port):
    fake_port.script = {"caps": b"PDO pos=1 type=fixed mv=5000 ma=500 epr_capable=1\r\n"
                                b"PDO pos=2 type=fixed mv=9000 ma=500\r\nEND\r\n"}
    r = sink.exchange({"sink": {"port": "/dev/fake"}}, "caps", timeout=1.0)
    assert r["ok"] and [x.get("mv") for x in r["reply"][:2]] == [5000, 9000]
    assert r["reply"][-1] == {"kind": "END"}


def test_err_reply_and_missing_event(fake_port):
    fake_port.script = {"epr 28": b"ERR not attached\r\n"}
    r = sink.exchange({"sink": {"port": "/dev/fake"}}, "epr 28", timeout=0.3, until_event="contract")
    assert not r["ok"] and "contract" in r["error"]
