"""Read and edit STM32CubeMX .ioc files (Java .properties format) without CubeMX.

Keys and values are presented unescaped (``ADC1.Rank-2#ChannelRegularConversion``)
and re-escaped on write the way CubeMX does (``\\#``, ``\\:``). Unchanged lines are
written back byte-for-byte so diffs stay minimal.

CubeMX keeps a per-IP index of which parameters were set by the user:
``<IP>.IPParameters`` for peripherals/middleware and ``<PIN>.GPIOParameters`` for
pins. A parameter missing from that index is treated as default and silently
ignored by CubeMX, so ``set``/``delete`` keep those lists in sync.

CubeMX does NOT validate values on load or generate (a bogus value is pasted
verbatim into the generated C), so the build is the real validator.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .config import ToolError

_PIN_RE = re.compile(r"^P[A-K]\d+")
# Pin keys that CubeMX never lists in GPIOParameters.
_PIN_UNLISTED = {"Mode", "Signal", "Locked", "GPIOParameters"}


def _unescape(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            out.append({"t": "\t", "n": "\n", "r": "\r", "f": "\f"}.get(nxt, nxt))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _escape(s: str, is_key: bool) -> str:
    out = []
    for i, c in enumerate(s):
        if c == "\\":
            out.append("\\\\")
        elif c in "#!=:":
            out.append("\\" + c)
        elif c == " " and (is_key or i == 0):
            out.append("\\ ")
        else:
            out.append(c)
    return "".join(out)


def _split(line: str) -> tuple[str, str]:
    i = 0
    while i < len(line):
        if line[i] == "\\":
            i += 2
            continue
        if line[i] in "=:":
            return line[:i], line[i + 1:]
        i += 1
    return line, ""


@dataclass
class _Line:
    raw: str
    key: str | None = None  # unescaped; None for comments/blank lines
    value: str | None = None


class Ioc:
    def __init__(self, path: Path):
        self.path = Path(path)
        if not self.path.exists():
            raise ToolError(f".ioc not found: {self.path}")
        with self.path.open(newline="") as f:
            content = f.read()
        self.eol = "\r\n" if "\r\n" in content else "\n"
        self.lines: list[_Line] = []
        raw_lines = content.replace("\r\n", "\n").split("\n")
        if raw_lines and raw_lines[-1] == "":
            raw_lines.pop()
        for raw in raw_lines:
            s = raw.lstrip()
            if not s or s[0] in "#!":
                self.lines.append(_Line(raw))
                continue
            if raw.endswith("\\") and not raw.endswith("\\\\"):
                raise ToolError("multi-line .properties values are not supported", line=raw)
            k, v = _split(s)
            self.lines.append(_Line(raw, _unescape(k.strip()), _unescape(v.lstrip())))
        self.changes: list[dict] = []

    # ---------------------------------------------------------------- queries
    def _index(self, key: str) -> int | None:
        for i, ln in enumerate(self.lines):
            if ln.key == key:
                return i
        return None

    def get(self, key: str) -> str | None:
        i = self._index(key)
        return None if i is None else self.lines[i].value

    def items(self) -> list[tuple[str, str]]:
        return [(ln.key, ln.value) for ln in self.lines if ln.key is not None]

    def search(self, pattern: str) -> dict[str, str]:
        rx = re.compile(pattern, re.IGNORECASE)
        return {k: v for k, v in self.items() if rx.search(k) or rx.search(v)}

    def ips(self) -> list[str]:
        """Enabled IPs/middleware from Mcu.IPn."""
        return [v for k, v in self.items() if re.fullmatch(r"Mcu\.IP\d+", k)]

    def pins(self) -> dict[str, dict[str, str]]:
        out: dict[str, dict[str, str]] = {}
        for k, v in self.items():
            if _PIN_RE.match(k) and "." in k:
                pin, param = k.split(".", 1)
                out.setdefault(pin, {})[param] = v
        return out

    # ---------------------------------------------------------------- edits
    def _write_line(self, key: str, value: str) -> None:
        raw = f"{_escape(key, True)}={_escape(value, False)}"
        i = self._index(key)
        if i is not None:
            self.lines[i] = _Line(raw, key, value)
            return
        # Insert in sorted position among key lines (CubeMX keeps them sorted).
        pos = len(self.lines)
        for j, ln in enumerate(self.lines):
            if ln.key is not None and ln.key > key:
                pos = j
                break
        self.lines.insert(pos, _Line(raw, key, value))

    def _param_list(self, key: str) -> tuple[str, str] | None:
        """Return (list_key, param) for the IPParameters/GPIOParameters index covering key."""
        if "." not in key:
            return None
        prefix, param = key.split(".", 1)
        if _PIN_RE.match(prefix):
            if param in _PIN_UNLISTED:
                return None
            return f"{prefix}.GPIOParameters", param
        if param == "IPParameters" or param.endswith("_Checked"):
            return None
        list_key = f"{prefix}.IPParameters"
        return (list_key, param) if self.get(list_key) is not None else None

    def set(self, key: str, value: str) -> dict:
        old = self.get(key)
        change = {"key": key, "old": old, "new": value, "side_effects": []}
        if old != value:
            self._write_line(key, value)
        pl = self._param_list(key)
        if pl:
            list_key, param = pl
            cur = self.get(list_key)
            items = [x for x in (cur or "").split(",") if x]
            if param not in items:
                items.append(param)
                self._write_line(list_key, ",".join(items))
                change["side_effects"].append(f"added {param} to {list_key}")
        elif old is None and "." in key and not _PIN_RE.match(key):
            prefix = key.split(".", 1)[0]
            change["warning"] = (
                f"new key and '{prefix}' has no IPParameters index; CubeMX may ignore it. "
                "Check the generated code. Enabling new peripherals/middleware is safer in the GUI."
            )
        self.changes.append(change)
        return change

    def delete(self, key: str) -> dict:
        i = self._index(key)
        if i is None:
            raise ToolError(f"key not found: {key}")
        old = self.lines.pop(i).value
        change = {"key": key, "old": old, "new": None, "side_effects": []}
        pl = self._param_list(key)
        if pl:
            list_key, param = pl
            items = [x for x in (self.get(list_key) or "").split(",") if x and x != param]
            self._write_line(list_key, ",".join(items))
            change["side_effects"].append(f"removed {param} from {list_key}")
        self.changes.append(change)
        return change

    def text(self) -> str:
        return self.eol.join(ln.raw for ln in self.lines) + self.eol

    def save(self, path: Path | None = None) -> None:
        with (path or self.path).open("w", newline="") as f:
            f.write(self.text())


def diff_ioc(a: Ioc, b: Ioc) -> dict:
    da, db = dict(a.items()), dict(b.items())
    return {
        "added": {k: db[k] for k in sorted(db.keys() - da.keys())},
        "removed": {k: da[k] for k in sorted(da.keys() - db.keys())},
        "changed": {k: {"old": da[k], "new": db[k]} for k in sorted(da.keys() & db.keys()) if da[k] != db[k]},
    }
