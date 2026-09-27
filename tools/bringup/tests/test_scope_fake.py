"""Scope driver against an in-process fake DS1054Z speaking SCPI over TCP."""

import socket
import threading

import pytest

from bringup.scope import Scope

# 1200-point square wave: 100 samples high (byte 200), 100 low (byte 50) -> 6 periods.
WAVE = bytes(200 if (i // 100) % 2 == 0 else 50 for i in range(1200))
# format,type,points,count,xinc,xorig,xref,yinc,yorig,yref
PREAMBLE = "0,0,1200,1,1.000000e-06,-6.000000e-04,0,4.000000e-02,0,125"
PNG = b"\x89PNG\r\n\x1a\n" + b"x" * 5000


def _block(data: bytes) -> bytes:
    n = str(len(data)).encode()
    return b"#" + str(len(n)).encode() + n + data + b"\n"


class FakeScope(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.log = []

    def reply(self, cmd: str) -> bytes | None:
        c = cmd.upper()
        if c == "*IDN?":
            return b"RIGOL TECHNOLOGIES,DS1104Z,DS1ZA000000001,00.04.05.SP2\n"
        if c.startswith(":MEASURE:ITEM? FREQ"):
            return b"5.000000e+03\n"
        if c.startswith(":MEASURE:ITEM? RTIM"):
            return b"9.9E37\n"
        if c.startswith(":MEASURE:ITEM?"):
            return b"1.234000e+00\n"
        if c == ":SYSTEM:ERROR?":
            return b'0,"No error"\n'
        if c == ":WAVEFORM:PREAMBLE?":
            return PREAMBLE.encode() + b"\n"
        if c == ":WAVEFORM:DATA?":
            return _block(WAVE)
        if c.startswith(":DISPLAY:DATA?"):
            return _block(PNG)
        if c.endswith("?"):
            return b"1\n"
        return None

    def run(self):
        conn, _ = self.srv.accept()
        buf = b""
        with conn:
            while True:
                data = conn.recv(4096)
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    cmd = line.decode().strip()
                    self.log.append(cmd)
                    if (r := self.reply(cmd)) is not None:
                        # Send in small chunks to exercise block reassembly.
                        for i in range(0, len(r), 1000):
                            conn.sendall(r[i:i + 1000])


@pytest.fixture
def scope(tmp_path):
    fake = FakeScope()
    fake.start()
    cfg = {"scope": {"resource": f"tcp://127.0.0.1:{fake.port}", "timeout_s": 5},
           "output": {"dir": str(tmp_path)}, "project": {"dir": ".", "ioc": "x.ioc", "elf_cmake": "x", "elf_make": "x"}}
    with Scope(cfg) as s:
        s.fake = fake
        yield s


def test_idn(scope):
    assert scope.idn()["model"] == "DS1104Z"


def test_measure_invalid_is_null(scope):
    m = scope.measure(["CH1"], ["FREQuency", "RTIMe", "VPP"])["measurements"]["CH1"]
    assert m == {"FREQuency": 5000.0, "RTIMe": None, "VPP": 1.234}
    assert ":MEASure:ITEM? FREQuency,CHANnel1" in scope.fake.log


def test_capture_scales_and_summarises(scope, tmp_path):
    r = scope.capture(["CH1"])
    s = r["summary"]["CH1"]
    assert s["points"] == 1200
    assert s["max"] == pytest.approx((200 - 125) * 0.04)
    assert s["min"] == pytest.approx((50 - 125) * 0.04)
    assert s["freq_hz_est"] == pytest.approx(5000, rel=1e-6)
    assert s["duty_pct_est"] == pytest.approx(50, abs=1)
    # Following text query still works after a binary block (trailing newline consumed).
    assert scope.idn()["model"] == "DS1104Z"


def test_screenshot_png(scope, tmp_path):
    r = scope.screenshot()
    assert r["bytes"] == len(PNG) and r["file"].endswith(".png")
