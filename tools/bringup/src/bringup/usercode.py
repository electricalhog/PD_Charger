"""List, read and replace CubeMX ``USER CODE BEGIN/END`` sections.

Only code inside these sections survives regeneration. Sections are addressed
by the text after ``BEGIN`` (e.g. ``2``, ``ADC1_Init 2``, ``RTOS_THREADS``);
duplicates inside one file get a ``#n`` suffix (``Includes#2``).
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from pathlib import Path

from .config import ToolError

_BEGIN = re.compile(r"USER CODE BEGIN\s+(.+?)\s*(?:\*/|-->)?\s*$")
_END = re.compile(r"USER CODE END\s+(.+?)\s*(?:\*/|-->)?\s*$")

SCAN_SUFFIXES = {".c", ".h", ".s", ".S", ".txt", ".cmake", ".ld"}
SCAN_EXCLUDE_DIRS = {"build", ".git", ".settings", "Debug", "Release"}


def _splitlines(text: str) -> list[str]:
    """Split on \n only (keeping endings); str.splitlines also breaks on \f, \x1c, U+2028..."""
    return re.findall(r"[^\n]*\n|[^\n]+\Z", text)


@dataclass
class Section:
    id: str
    name: str
    begin_line: int  # 1-based line number of the BEGIN marker
    end_line: int    # 1-based line number of the END marker
    body: str        # text strictly between the marker lines

    @property
    def empty(self) -> bool:
        return not self.body.strip()


def parse(text: str, path: Path | str = "<text>") -> list[Section]:
    lines = _splitlines(text)
    sections: list[Section] = []
    seen: dict[str, int] = {}
    open_: tuple[str, int] | None = None
    for i, line in enumerate(lines):
        if m := _BEGIN.search(line):
            if open_:
                raise ToolError(f"{path}:{i + 1}: nested USER CODE BEGIN inside '{open_[0]}'")
            open_ = (m.group(1), i)
        elif m := _END.search(line):
            if not open_ or m.group(1) != open_[0]:
                raise ToolError(f"{path}:{i + 1}: USER CODE END '{m.group(1)}' without matching BEGIN")
            name, bi = open_
            seen[name] = seen.get(name, 0) + 1
            sid = name if seen[name] == 1 else f"{name}#{seen[name]}"
            sections.append(Section(sid, name, bi + 1, i + 1, "".join(lines[bi + 1:i])))
            open_ = None
    if open_:
        raise ToolError(f"{path}: USER CODE BEGIN '{open_[0]}' never closed")
    return sections


def _read(path: Path) -> str:
    # newline="" keeps CRLF intact; CubeMX emits CRLF for some files and LF for others.
    with path.open(newline="", errors="replace") as f:
        return f.read()


def list_sections(path: Path) -> list[Section]:
    return parse(_read(path), path)


def get_section(path: Path, sid: str) -> Section:
    for s in list_sections(path):
        if s.id == sid:
            return s
    raise ToolError(f"no USER CODE section '{sid}' in {path}",
                    available=[s.id for s in list_sections(path)])


def set_section(path: Path, sid: str, body: str) -> dict:
    sec = get_section(path, sid)
    text = _read(path)
    eol = "\r\n" if "\r\n" in text else "\n"
    lines = _splitlines(text)
    body = body.replace("\r\n", "\n")
    if body and not body.endswith("\n"):
        body += "\n"
    body = body.replace("\n", eol)
    old = sec.body
    lines[sec.begin_line:sec.end_line - 1] = [body] if body else []
    with path.open("w", newline="") as f:
        f.write("".join(lines))
    return {"file": str(path), "section": sid, "old_lines": old.count("\n"), "new_lines": body.count("\n")}


def iter_files(root: Path):
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.suffix in SCAN_SUFFIXES and not (SCAN_EXCLUDE_DIRS & set(p.relative_to(root).parts)):
            yield p


def snapshot(root: Path, include_drivers: bool = False) -> dict[str, dict[str, str]]:
    """{relative file: {section id: sha1(body)}} for every file with USER CODE sections."""
    out: dict[str, dict[str, str]] = {}
    for p in iter_files(root):
        rp = p.relative_to(root)
        if not include_drivers and rp.parts[0] in ("Drivers", "Middlewares"):
            continue
        text = _read(p)
        if "USER CODE BEGIN" not in text:
            continue
        try:
            secs = parse(text, rp)
        except ToolError:
            continue
        out[str(rp)] = {s.id: hashlib.sha1(s.body.encode()).hexdigest() for s in secs}
    return out


def strip_user_code(text: str) -> str:
    """Replace every USER CODE body with a placeholder so only generated regions remain."""
    lines = _splitlines(text)
    try:
        secs = parse(text)
    except ToolError:
        return text
    for s in reversed(secs):
        lines[s.begin_line:s.end_line - 1] = ["<user code>\n"] if s.body else []
    return "".join(lines)
