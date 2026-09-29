"""Rigol DS1054Z (DS1000Z series) control over SCPI.

Transports:
  tcp://HOST[:5555]  raw socket, no VISA needed (recommended; set a static IP on the scope)
  usb                first Rigol USBTMC device via pyvisa-py (needs a udev rule for 1ab1:04ce)
  <VISA resource>    anything pyvisa understands

Reference: Rigol "MSO1000Z/DS1000Z Series Programming Guide".
"""

from __future__ import annotations

import csv
import math
import re
import socket
import time

from .config import ToolError, out_file, rel

INVALID = 9.9e37  # returned by :MEASure when the measurement is not possible

MEASURE_ITEMS = [
    "VMAX", "VMIN", "VPP", "VTOP", "VBASe", "VAMP", "VAVG", "VRMS", "OVERshoot", "PREShoot",
    "MARea", "MPARea", "PERiod", "FREQuency", "RTIMe", "FTIMe", "PWIDth", "NWIDth", "PDUTy",
    "NDUTy", "RDELay", "FDELay", "RPHase", "FPHase", "TVMAX", "TVMIN", "PSLEWrate", "NSLEWrate",
    "VUPper", "VMID", "VLOWer", "VARIance", "PVRMS", "PPULses", "NPULses", "PEDGes", "NEDGes",
]
DEFAULT_ITEMS = ["VPP", "VAVG", "VMAX", "VMIN", "FREQuency", "PDUTy", "RTIMe", "FTIMe"]


# ------------------------------------------------------------------ transports

class _Socket:
    def __init__(self, host: str, port: int, timeout: float):
        try:
            self.s = socket.create_connection((host, port), timeout=timeout)
        except OSError as e:
            raise ToolError(f"cannot connect to scope at {host}:{port}: {e}") from e
        self.s.settimeout(timeout)
        self.buf = b""

    def write(self, cmd: str) -> None:
        self.s.sendall(cmd.encode() + b"\n")

    def _recv(self, n: int) -> bytes:
        return self.s.recv(n)

    def _read_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self._recv(max(65536, n - len(self.buf)))
            if not chunk:
                raise ToolError("scope closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_line(self) -> str:
        # Skips blank lines, which also swallows the newline that trails a binary block.
        while True:
            while b"\n" not in self.buf:
                chunk = self._recv(65536)
                if not chunk:
                    raise ToolError("scope closed the connection")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            if line.strip():
                return line.decode(errors="replace").strip()

    def read_block(self) -> bytes:
        """IEEE 488.2 definite-length block: #<n><len><data>."""
        while b"#" not in self.buf:
            self.buf = self._recv(65536)
        self.buf = self.buf[self.buf.index(b"#"):]
        ndigits = int(self._read_exact(2)[1:2])
        length = int(self._read_exact(ndigits))
        return self._read_exact(length)

    def close(self) -> None:
        self.s.close()


class _UsbtmcDev(_Socket):
    """Linux kernel usbtmc driver (/dev/usbtmcN).

    Only for text queries: the DS1000Z sets end-of-message on every 64-byte
    packet, so the kernel driver truncates replies to 52 bytes (screenshots
    and waveforms fail). ``resource = "usb"`` uses raw USB via pyvisa instead.
    """

    def __init__(self, path: str, timeout: float):
        import os
        self.os = os
        try:
            self.fd = os.open(path, os.O_RDWR)
        except OSError as e:
            raise ToolError(f"cannot open {path}: {e}",
                            hint="install tools/bringup/udev/99-bringup.rules (needs sudo), then replug the scope") from e
        self.buf = b""
        self.timeout = timeout

    def write(self, cmd: str) -> None:
        self.os.write(self.fd, cmd.encode() + b"\n")

    def _recv(self, n: int) -> bytes:
        try:
            chunk = self.os.read(self.fd, n)
        except TimeoutError as e:  # kernel returns ETIMEDOUT (default 5 s)
            raise ToolError("scope read timed out") from e
        if not chunk:
            raise ToolError("usbtmc read returned no data")
        return chunk

    def read_line(self) -> str:
        # Each read() returns one USBTMC message; an empty read is end-of-message.
        # The DS1000Z sends *IDN? without a terminator, so EOM also ends a line.
        while b"\n" not in self.buf:
            chunk = self.os.read(self.fd, 65536)
            if not chunk:
                if self.buf.strip():
                    line, self.buf = self.buf, b""
                    return line.decode(errors="replace").strip()
                raise ToolError("usbtmc read returned no data")
            self.buf += chunk
            if not chunk.endswith(b"\n") and len(chunk) < 65536:
                line, self.buf = self.buf, b""  # short message without terminator = complete
                return line.decode(errors="replace").strip()
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").strip()

    def close(self) -> None:
        self.os.close(self.fd)


def _find_rigol_usbtmc() -> str | None:
    """/dev/usbtmcN whose USB parent is a Rigol (vendor 1ab1)."""
    import glob
    from pathlib import Path
    for dev in sorted(glob.glob("/dev/usbtmc*")):
        sys_dev = Path("/sys/class/usbmisc") / Path(dev).name / "device"
        for parent in [sys_dev.resolve(), *sys_dev.resolve().parents]:
            vid = parent / "idVendor"
            if vid.exists():
                if vid.read_text().strip() == "1ab1":
                    return dev
                break
    return None


class _Visa:
    def __init__(self, resource: str, timeout: float):
        try:
            import pyvisa
        except ImportError as e:
            raise ToolError("pyvisa not installed (run via tools/bringup/bu so uv installs deps)") from e
        import warnings
        rm = pyvisa.ResourceManager("@py")
        if resource == "usb":
            with warnings.catch_warnings():  # pyvisa-py nags about optional LAN-discovery packages
                warnings.simplefilter("ignore")
                listed = rm.list_resources("USB?*INSTR")
            # Rigol vendor id 0x1AB1 = 6833 (pyvisa-py prints it in decimal)
            found = [r for r in listed if r.split("::")[1].upper() in ("6833", "0X1AB1")]
            if not found:
                raise ToolError("no Rigol USBTMC device found", resources=list(listed),
                                hint="check USB cable and udev rule for 1ab1:04ce (see tools/bringup/README.md)")
            resource = found[0]
        try:
            self.inst = rm.open_resource(resource)
        except Exception as e:  # pyvisa raises many types
            raise ToolError(f"cannot open VISA resource {resource}: {e}") from e
        self.inst.timeout = int(timeout * 1000)
        self.inst.read_termination = "\n"
        self.inst.write_termination = "\n"
        self.inst.chunk_size = 1024 * 1024

    def write(self, cmd: str) -> None:
        self.inst.write(cmd)

    def read_line(self) -> str:
        return self.inst.read().strip()

    def read_block(self) -> bytes:
        # Read #<n><len> then exactly <len> bytes: the DS1000Z flags end-of-message
        # on every USB packet, so "read until EOM" stops after 52 bytes.
        hdr = self.inst.read_bytes(2)
        while hdr[:1] != b"#":
            hdr = hdr[1:] + self.inst.read_bytes(1)
        length = int(self.inst.read_bytes(int(hdr[1:2])))
        data = self.inst.read_bytes(length)
        try:
            self.inst.read_bytes(1)  # trailing newline
        except Exception:  # noqa: BLE001 - some firmware omits it
            pass
        return data

    def close(self) -> None:
        self.inst.close()


# ------------------------------------------------------------------ scope

class Scope:
    def __init__(self, cfg: dict):
        sc = cfg.get("scope", {})
        res = sc.get("resource", "")
        timeout = float(sc.get("timeout_s", 10))
        if not res:
            raise ToolError("scope not configured: set [scope].resource in tools/bringup/bringup.local.toml",
                            example='resource = "tcp://192.168.1.50:5555"')
        if m := re.fullmatch(r"tcp://([^:/]+)(?::(\d+))?/?", res):
            self.io = _Socket(m.group(1), int(m.group(2) or 5555), timeout)
        elif res.startswith("/dev/usbtmc"):
            self.io = _UsbtmcDev(res, timeout)
        else:
            self.io = _Visa(res, timeout)
        self.cfg = cfg

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.io.close()

    def write(self, cmd: str) -> None:
        self.io.write(cmd)
        time.sleep(0.02)  # DS1000Z drops commands sent back-to-back too quickly

    def query(self, cmd: str) -> str:
        self.io.write(cmd)
        return self.io.read_line()

    def query_float(self, cmd: str) -> float | None:
        try:
            v = float(self.query(cmd))
        except ValueError:
            return None
        return None if abs(v) >= INVALID * 0.99 else v

    def errors(self) -> list[str]:
        errs = []
        for _ in range(20):
            e = self.query(":SYSTem:ERRor?")
            if e.startswith("0,") or e.startswith("+0,"):
                break
            errs.append(e)
        return errs

    # -------------------------------------------------------------- queries
    def idn(self) -> dict:
        idn = self.query("*IDN?")
        parts = idn.split(",")
        return {"idn": idn, "model": parts[1] if len(parts) > 1 else None,
                "serial": parts[2] if len(parts) > 2 else None, "firmware": parts[3] if len(parts) > 3 else None}

    def state(self) -> dict:
        chans = {}
        for n in range(1, 5):
            c = f":CHANnel{n}"
            chans[f"CH{n}"] = {
                "display": self.query(f"{c}:DISPlay?") == "1",
                "scale_v_div": self.query_float(f"{c}:SCALe?"),
                "offset_v": self.query_float(f"{c}:OFFSet?"),
                "coupling": self.query(f"{c}:COUPling?"),
                "probe": self.query_float(f"{c}:PROBe?"),
                "bw_limit": self.query(f"{c}:BWLimit?"),
                "invert": self.query(f"{c}:INVert?") == "1",
            }
        return {
            "channels": chans,
            "timebase": {"scale_s_div": self.query_float(":TIMebase:MAIN:SCALe?"),
                         "offset_s": self.query_float(":TIMebase:MAIN:OFFSet?"),
                         "mode": self.query(":TIMebase:MODE?")},
            "trigger": {"status": self.query(":TRIGger:STATus?"), "mode": self.query(":TRIGger:MODE?"),
                        "sweep": self.query(":TRIGger:SWEep?"), "source": self.query(":TRIGger:EDGe:SOURce?"),
                        "slope": self.query(":TRIGger:EDGe:SLOPe?"), "level_v": self.query_float(":TRIGger:EDGe:LEVel?"),
                        "coupling": self.query(":TRIGger:COUPling?"), "holdoff_s": self.query_float(":TRIGger:HOLDoff?")},
            "acquire": {"type": self.query(":ACQuire:TYPE?"), "mdepth": self.query(":ACQuire:MDEPth?"),
                        "srate_sa_s": self.query_float(":ACQuire:SRATe?")},
        }

    # -------------------------------------------------------------- config
    def channel(self, n: int, display: bool | None = None, scale: float | None = None, offset: float | None = None,
                coupling: str | None = None, probe: float | None = None, bwl: str | None = None,
                invert: bool | None = None) -> dict:
        c = f":CHANnel{n}"
        if display is not None:
            self.write(f"{c}:DISPlay {'ON' if display else 'OFF'}")
        if probe is not None:  # before scale: scale is in probe-corrected volts
            self.write(f"{c}:PROBe {probe:g}")
        if coupling:
            self.write(f"{c}:COUPling {coupling.upper()}")
        if bwl:
            self.write(f"{c}:BWLimit {bwl.upper()}")
        if invert is not None:
            self.write(f"{c}:INVert {'ON' if invert else 'OFF'}")
        if scale is not None:
            self.write(f"{c}:SCALe {scale:g}")
        if offset is not None:
            self.write(f"{c}:OFFSet {offset:g}")
        return {"ok": True, "errors": self.errors(), "channel": self.state()["channels"][f"CH{n}"]}

    def timebase(self, scale: float | None = None, offset: float | None = None) -> dict:
        if scale is not None:
            self.write(f":TIMebase:MAIN:SCALe {scale:g}")
        if offset is not None:
            self.write(f":TIMebase:MAIN:OFFSet {offset:g}")
        return {"ok": True, "errors": self.errors(), "scale_s_div": self.query_float(":TIMebase:MAIN:SCALe?"),
                "offset_s": self.query_float(":TIMebase:MAIN:OFFSet?")}

    def trigger(self, source: str | None = None, slope: str | None = None, level: float | None = None,
                sweep: str | None = None, coupling: str | None = None, holdoff: float | None = None) -> dict:
        self.write(":TRIGger:MODE EDGE")
        if source:
            src = source.upper()
            self.write(f":TRIGger:EDGe:SOURce {'CHAN' + src[-1] if src[-1].isdigit() and not src.startswith('D') else src}")
        if slope:
            self.write(f":TRIGger:EDGe:SLOPe {slope.upper()}")
        if level is not None:
            self.write(f":TRIGger:EDGe:LEVel {level:g}")
        if coupling:
            self.write(f":TRIGger:COUPling {coupling.upper()}")
        if holdoff is not None:
            self.write(f":TRIGger:HOLDoff {holdoff:g}")
        if sweep:
            self.write(f":TRIGger:SWEep {sweep.upper()}")
        return {"ok": True, "errors": self.errors(), "trigger": self.state()["trigger"]}

    def acquire(self, mdepth: str | None = None, atype: str | None = None, averages: int | None = None) -> dict:
        if atype:
            self.write(f":ACQuire:TYPE {atype.upper()}")
        if averages:
            self.write(f":ACQuire:AVERages {averages}")
        if mdepth:
            self.write(":RUN")  # MDEPth can only be changed while running
            self.write(f":ACQuire:MDEPth {_mdepth(mdepth)}")
        return {"ok": True, "errors": self.errors(), "acquire": self.state()["acquire"]}

    def action(self, what: str) -> dict:
        cmd = {"run": ":RUN", "stop": ":STOP", "single": ":SINGle", "force": ":TFORce",
               "autoscale": ":AUToscale", "clear": ":CLEar", "reset": "*RST"}[what]
        self.write(cmd)
        if what in ("autoscale", "reset"):
            time.sleep(3)
        return {"ok": True, "action": what, "trigger_status": self.query(":TRIGger:STATus?")}

    def wait_trigger(self, timeout: float = 10.0, arm: bool = True) -> dict:
        """Arm single-shot and wait until the acquisition completes (status STOP)."""
        if arm:
            self.write(":SINGle")
            time.sleep(0.3)
        t0 = time.monotonic()
        status = ""
        while time.monotonic() - t0 < timeout:
            status = self.query(":TRIGger:STATus?")
            if status == "STOP":
                return {"ok": True, "triggered": True, "seconds": round(time.monotonic() - t0, 2)}
            time.sleep(0.1)
        return {"ok": True, "triggered": False, "last_status": status, "seconds": timeout}

    # -------------------------------------------------------------- measurements
    def measure(self, sources: list[str], items: list[str] | None = None) -> dict:
        items = items or DEFAULT_ITEMS
        out = {}
        for src in sources:
            s = _src(src)
            vals = {it: self.query_float(f":MEASure:ITEM? {it},{s}") for it in items}
            for it in ("PDUTy", "NDUTy"):  # the scope returns duty as a 0..1 ratio
                if vals.get(it) is not None:
                    vals[it] = round(vals[it] * 100, 3)
            out[src.upper()] = vals
        return {"ok": True, "note": "null = measurement not possible on current screen (e.g. <1 period visible); "
                                    "PDUTy/NDUTy in percent; times in seconds, voltages in volts",
                "measurements": out}

    def measure_between(self, item: str, src_a: str, src_b: str) -> dict:
        """Two-source items: RDELay, FDELay, RPHase, FPHase."""
        return {"ok": True, "item": item, "a": src_a, "b": src_b,
                "value": self.query_float(f":MEASure:ITEM? {item},{_src(src_a)},{_src(src_b)}")}

    # -------------------------------------------------------------- data
    def _preamble(self) -> dict:
        p = self.query(":WAVeform:PREamble?").split(",")
        keys = ["format", "type", "points", "count", "xinc", "xorig", "xref", "yinc", "yorig", "yref"]
        return {k: float(v) for k, v in zip(keys, p)}

    def capture(self, sources: list[str], mode: str = "normal", points: int | None = None,
                thresholds: dict[str, float | None] | None = None, delays: list[str] | None = None) -> dict:
        """Download waveforms to CSV and analyse the samples.

        'normal' = the 1200 screen points (interpolated at fast timebases; works while running).
        'raw'    = acquisition memory at the real sample rate (scope is stopped first); use this
                   for statistics over many periods (e.g. dead-time on every switching cycle).
        """
        mode = mode.lower()
        if mode == "raw":
            self.write(":STOP")
        self.write(f":WAVeform:MODE {'RAW' if mode == 'raw' else 'NORMal'}")
        self.write(":WAVeform:FORMat BYTE")
        cols, warnings, pre = {}, [], None
        t_dl = time.monotonic()
        for src in sources:
            self.write(f":WAVeform:SOURce {_src(src)}")
            pre = self._preamble()
            total = int(pre["points"]) if mode == "raw" else 1200
            if points:
                total = min(total, points)
            data = bytearray()
            start = 1
            while start <= total:
                stop = min(start + 250000 - 1, total)
                if mode == "raw":
                    self.write(f":WAVeform:STARt {start}")
                    self.write(f":WAVeform:STOP {stop}")
                self.io.write(":WAVeform:DATA?")
                data += self.io.read_block()
                if mode != "raw":
                    break
                start = stop + 1
            cols[src.upper()] = [(b - pre["yorig"] - pre["yref"]) * pre["yinc"] for b in data]
            clipped = sum(1 for b in data if b in (0, 255))
            if clipped > len(data) * 0.001:
                warnings.append(f"{src.upper()}: {clipped} samples at the ADC rail (clipped): "
                                "adjust scale/offset, levels and edges near the rail are wrong")
        n = min(len(v) for v in cols.values()) if cols else 0
        xinc, xorig = pre["xinc"], pre["xorig"]
        f = out_file(self.cfg, "scope_" + "_".join(s.lower() for s in cols), "csv")
        with f.open("w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["t_s", *cols.keys()])
            for i in range(n):
                w.writerow([f"{xorig + i * xinc:.12e}", *(f"{cols[k][i]:.5g}" for k in cols)])
        from . import analysis
        result = analysis.analyze(f, thresholds, delays)
        result.update(mode=mode, file=rel(f), sample_rate_sa_s=1 / xinc if xinc else None,
                      download_s=round(time.monotonic() - t_dl, 2),
                      note="times are relative to the trigger; edges interpolated at threshold_v")
        if warnings:
            result["warnings"] = warnings
        return result

    def screenshot(self) -> dict:
        self.io.write(":DISPlay:DATA? ON,OFF,PNG")
        data = self.io.read_block()
        ext = "png" if data[:4] == b"\x89PNG" else "bmp"
        f = out_file(self.cfg, "scope_screen", ext)
        f.write_bytes(data)
        return {"ok": True, "file": rel(f), "bytes": len(data)}

    def raw(self, cmd: str) -> dict:
        if "?" in cmd:
            return {"ok": True, "command": cmd, "response": self.query(cmd)}
        self.write(cmd)
        return {"ok": True, "command": cmd, "errors": self.errors()}


def _mdepth(x: str) -> str:
    """'600k' / '1.2M' / 'auto' -> '600000' / '1200000' / 'AUTO'. Valid values depend on
    enabled channels: 1 ch 12k..24M, 2 ch 6k..12M, 3-4 ch 3k..6M (x10 steps)."""
    m = re.fullmatch(r"\s*([\d.]+)\s*([kKmM]?)\s*", x)
    if not m:
        return x.upper()
    return str(int(round(float(m[1]) * {"": 1, "k": 1e3, "m": 1e6}[m[2].lower()])))


def _src(s: str) -> str:
    s = s.upper()
    if re.fullmatch(r"(CH|CHAN|CHANNEL)?[1-4]", s):
        return "CHANnel" + s[-1]
    if re.fullmatch(r"D\d{1,2}", s):  # MSO models only
        return s
    if s in ("MATH",):
        return s
    raise ToolError(f"bad source '{s}', use CH1..CH4 (or MATH, D0..D15 on MSO)")
