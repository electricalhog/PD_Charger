"""`bu` - agent-facing bring-up CLI. Every command prints one JSON object.

Exit code 0 when the JSON has ok=true, 1 otherwise. Large outputs (captures,
logs, diffs, screenshots) are written under bringup_out/ and referenced by path.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import traceback
from pathlib import Path

from . import build as build_mod
from . import cubemx, logic, probe, profile, regulator, serialmon, usercode
from .config import REPO_ROOT, ToolError, cubemx_exe, gdb_exe, load_config, paths, programmer_exe, rel, run
from .ioc import Ioc, diff_ioc


# ------------------------------------------------------------------ helpers

def _project_file(cfg: dict, f: str) -> Path:
    p = Path(f)
    for cand in (p, paths(cfg).project_dir / p, REPO_ROOT / p):
        if cand.exists():
            return cand.resolve()
    raise ToolError(f"file not found: {f} (paths may be relative to the project dir)")


def _read_body(args) -> str:
    if args.text is not None:
        return args.text
    if args.from_file:
        return Path(args.from_file).read_text()
    return sys.stdin.read()


# ------------------------------------------------------------------ ioc

def cmd_ioc(cfg, a):
    P = paths(cfg)
    ioc = Ioc(P.ioc)
    if a.op == "get":
        vals = {k: ioc.get(k) for k in a.keys}
        missing = [k for k, v in vals.items() if v is None]
        return {"ok": not missing, "values": vals, **({"missing": missing} if missing else {})}
    if a.op == "search":
        hits = ioc.search(a.pattern)
        return {"ok": True, "count": len(hits), "matches": dict(list(hits.items())[:a.limit]),
                "truncated": len(hits) > a.limit}
    if a.op == "ips":
        return {"ok": True, "ips": ioc.ips()}
    if a.op == "pins":
        pins = ioc.pins()
        if a.pin:
            pins = {k: v for k, v in pins.items() if k.upper().startswith(a.pin.upper())}
        return {"ok": True, "pins": pins}
    if a.op == "set":
        changes = []
        for kv in a.assignments:
            if "=" not in kv:
                raise ToolError(f"expected KEY=VALUE, got {kv!r}")
            k, v = kv.split("=", 1)
            changes.append(ioc.set(k.strip(), v))
        if not a.dry_run:
            ioc.save()
        return {"ok": True, "saved": not a.dry_run, "changes": changes,
                "next": "bu cubemx generate  (dry-run first), then --apply, then bu build"}
    if a.op == "delete":
        ch = [ioc.delete(k) for k in a.keys]
        if not a.dry_run:
            ioc.save()
        return {"ok": True, "saved": not a.dry_run, "changes": ch}
    if a.op == "diff":
        cp = run(["git", "show", f"{a.ref}:{rel(P.ioc)}"], cwd=REPO_ROOT, timeout=30)
        if cp.returncode:
            raise ToolError(f"git show failed: {cp.stderr.strip()}")
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".ioc", delete=False) as f:
            f.write(cp.stdout)
        try:
            d = diff_ioc(Ioc(Path(f.name)), ioc)
        finally:
            Path(f.name).unlink()
        return {"ok": True, "ref": a.ref, **d}


# ------------------------------------------------------------------ usercode

def cmd_usercode(cfg, a):
    if a.op == "scan":
        root = paths(cfg).project_dir
        files = {}
        for p in usercode.iter_files(root):
            rp = p.relative_to(root)
            if rp.parts[0] in ("Drivers", "Middlewares") and not a.all:
                continue
            text = p.read_text(errors="replace")
            if "USER CODE BEGIN" not in text:
                continue
            secs = usercode.parse(text, rp)
            non_empty = [s.id for s in secs if not s.empty]
            files[str(rp)] = {"sections": len(secs), "non_empty": non_empty}
        return {"ok": True, "project": rel(root), "files": files}
    path = _project_file(cfg, a.file)
    if a.op == "list":
        secs = usercode.list_sections(path)
        return {"ok": True, "file": rel(path), "sections": [
            {"id": s.id, "begin_line": s.begin_line, "end_line": s.end_line, "lines": s.body.count("\n"),
             "empty": s.empty} for s in secs]}
    if a.op == "get":
        s = usercode.get_section(path, a.id)
        return {"ok": True, "file": rel(path), "id": s.id, "begin_line": s.begin_line, "end_line": s.end_line,
                "body": s.body}
    if a.op == "set":
        r = usercode.set_section(path, a.id, _read_body(a))
        r["file"] = rel(path)
        return {"ok": True, **r}


# ------------------------------------------------------------------ doctor

def cmd_doctor(cfg, a):
    P = paths(cfg)
    checks = {}

    def check(name, fn):
        try:
            checks[name] = {"ok": True, **fn()}
        except ToolError as e:
            checks[name] = {"ok": False, "error": str(e), **e.details}
        except Exception as e:  # noqa: BLE001 - doctor must never crash
            checks[name] = {"ok": False, "error": f"{type(e).__name__}: {e}"}

    def exe(path, ver_args=None):
        if not path:
            raise ToolError("not found")
        out = {"path": path}
        if ver_args:
            cp = run([path, *ver_args], timeout=30)
            out["version"] = (cp.stdout or cp.stderr).strip().splitlines()[0] if (cp.stdout or cp.stderr).strip() else None
        return out

    check("project", lambda: {"dir": rel(P.project_dir), "ioc": rel(P.ioc), "build_system": P.build_system,
                              "elf": rel(P.elf), "elf_exists": P.elf.exists(),
                              "keep_user_code": Ioc(P.ioc).get("ProjectManager.KeepUserCode")})
    check("cubemx", lambda: exe(cubemx_exe(cfg)))
    check("arm_gcc", lambda: exe(shutil.which("arm-none-eabi-gcc"), ["--version"]))
    check("cmake", lambda: exe(shutil.which("cmake"), ["--version"]))
    check("ninja", lambda: exe(shutil.which("ninja"), ["--version"]))
    check("gdb", lambda: exe(gdb_exe()))
    check("stm32_programmer", lambda: exe(programmer_exe(cfg)))
    check("openocd", lambda: exe(shutil.which("openocd")))
    if not a.offline:
        def _probes():
            r = probe.list_probes(cfg)
            if not r["count"]:
                raise ToolError("no ST-LINK detected (board unplugged, or needs udev rules)")
            return r
        check("stlink_probes", _probes)
        check("serial_ports", serialmon.list_ports)
        check("sigrok", lambda: logic.scan(cfg))

        def _scope():
            from .scope import Scope
            with Scope(cfg) as s:
                return s.idn()
        check("scope", _scope)
    return {"ok": all(c["ok"] for c in checks.values()), "checks": checks}


# ------------------------------------------------------------------ scope

def _thresholds(specs: list[str] | None) -> dict:
    """['1.65'] -> {'*': 1.65}; ['CH2=1.2'] -> {'CH2': 1.2}; resolved per channel by analysis."""
    out: dict = {}
    for sp in specs or []:
        if "=" in sp:
            ch, v = sp.split("=", 1)
            out[ch.strip().upper()] = float(v)
        else:
            out["*"] = float(sp)
    return out


def cmd_scope(cfg, a):
    if a.op == "analyze":
        from . import analysis
        return analysis.analyze(a.file, _thresholds(a.threshold), a.delay)
    from .scope import Scope
    with Scope(cfg) as s:
        op = a.op
        if op == "idn":
            return {"ok": True, **s.idn()}
        if op == "state":
            return {"ok": True, **s.state()}
        if op in ("run", "stop", "single", "force", "autoscale", "clear", "reset"):
            return s.action(op)
        if op == "chan":
            disp = True if a.on else False if a.off else None
            return s.channel(a.n, disp, a.scale, a.offset, a.coupling, a.probe, a.bwl, a.invert)
        if op == "timebase":
            return s.timebase(a.scale, a.offset)
        if op == "trigger":
            return s.trigger(a.source, a.slope, a.level, a.sweep, a.coupling, a.holdoff)
        if op == "acquire":
            return s.acquire(a.mdepth, a.type, a.averages)
        if op == "measure":
            return s.measure(a.sources, a.items.split(",") if a.items else None)
        if op == "delay":
            return s.measure_between(a.item, a.a, a.b)
        if op == "wait":
            return s.wait_trigger(a.timeout, arm=not a.no_arm)
        if op == "capture":
            return s.capture(a.sources, "raw" if a.raw else "normal", a.points, _thresholds(a.threshold), a.delay)
        if op == "screenshot":
            return s.screenshot()
        if op == "scpi":
            return s.raw(a.command)


# ------------------------------------------------------------------ parser

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="bu", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("schema", help="machine-readable catalogue of all commands/args (for MCP tool generation)")
    d = sub.add_parser("doctor", help="check toolchain, probe, scope, analyzer")
    d.add_argument("--offline", action="store_true", help="skip hardware checks")

    # ioc
    io = sub.add_parser("ioc", help="read/edit the CubeMX .ioc").add_subparsers(dest="op", required=True)
    g = io.add_parser("get"); g.add_argument("keys", nargs="+")
    g = io.add_parser("search", help="regex over keys and values"); g.add_argument("pattern"); g.add_argument("--limit", type=int, default=200)
    io.add_parser("ips", help="enabled peripherals/middleware")
    g = io.add_parser("pins"); g.add_argument("pin", nargs="?", help="prefix filter, e.g. PA or PB14")
    g = io.add_parser("set", help="KEY=VALUE ... (keeps IPParameters/GPIOParameters in sync)")
    g.add_argument("assignments", nargs="+"); g.add_argument("--dry-run", action="store_true")
    g = io.add_parser("delete"); g.add_argument("keys", nargs="+"); g.add_argument("--dry-run", action="store_true")
    g = io.add_parser("diff", help="semantic diff vs a git ref"); g.add_argument("--ref", default="HEAD")

    # usercode
    uc = sub.add_parser("usercode", help="USER CODE sections").add_subparsers(dest="op", required=True)
    g = uc.add_parser("scan", help="all files with user sections"); g.add_argument("--all", action="store_true", help="include Drivers/Middlewares")
    g = uc.add_parser("list"); g.add_argument("file")
    g = uc.add_parser("get"); g.add_argument("file"); g.add_argument("id")
    g = uc.add_parser("set", help="replace a section body (from --text, --from FILE, or stdin)")
    g.add_argument("file"); g.add_argument("id"); g.add_argument("--text"); g.add_argument("--from", dest="from_file")

    # cubemx
    cx = sub.add_parser("cubemx", help="headless CubeMX").add_subparsers(dest="op", required=True)
    g = cx.add_parser("generate", help="generate code (dry-run unless --apply)")
    g.add_argument("--apply", action="store_true", help="copy generated files into the project")
    g.add_argument("--force", action="store_true", help="apply even if USER CODE would be lost")
    g.add_argument("--diff-lines", type=int, default=200)
    g.add_argument("--only", action="append", metavar="GLOB",
                   help="with --apply: only copy generated files matching this project-relative glob (repeatable)")
    g = cx.add_parser("script", help="run raw CubeMX script commands"); g.add_argument("commands", nargs="+")

    # build
    b = sub.add_parser("build", help="compile firmware")
    b.add_argument("--clean", action="store_true"); b.add_argument("--reconfigure", action="store_true")
    b.add_argument("-j", "--jobs", type=int, default=0)

    # probe / flash / memory
    pr = sub.add_parser("probe", help="ST-LINK").add_subparsers(dest="op", required=True)
    pr.add_parser("list")
    g = pr.add_parser("reset"); g.add_argument("--hard", action="store_true")
    f = sub.add_parser("flash", help="program the target (default: built ELF)")
    f.add_argument("--elf"); f.add_argument("--no-verify", action="store_true"); f.add_argument("--no-reset", action="store_true")
    s = sub.add_parser("sym", help="list ELF symbols (addr,size)"); s.add_argument("pattern", nargs="?", default="")
    s = sub.add_parser("layout", help="type layout with offsets (gdb ptype /o)"); s.add_argument("expr")
    m = sub.add_parser("mem", help="live memory by symbol/address").add_subparsers(dest="op", required=True)
    g = m.add_parser("read"); g.add_argument("target", help="symbol, symbol+off, or 0xADDR")
    g.add_argument("--type", default="u32"); g.add_argument("--count", type=int); g.add_argument("--size", type=int)
    g.add_argument("--save", action="store_true")
    g = m.add_parser("write"); g.add_argument("target"); g.add_argument("value"); g.add_argument("--type", default="u32")

    g = sub.add_parser("profile", help="statistical CPU profile over SWD (DWT PC sampling): time per ISR/task and function")
    g.add_argument("--samples", type=int, default=1000); g.add_argument("--top", type=int, default=25)
    g.add_argument("--lines", type=int, default=0, help="also the N hottest PCs with source lines (addr2line)")

    # regulator / input protection
    rg = sub.add_parser("regulator", help="state, ADM1270 input protection, fault recovery").add_subparsers(dest="op", required=True)
    rg.add_parser("status", help="state, fault source, protection lines, HRTIM fault/output state, diagnosis")
    g = rg.add_parser("clear-fault", help="firmware re-arms the ADM1270 (cool-down + ENABLE toggle), verifies VS, releases FAULT")
    g.add_argument("--timeout", type=float, default=3.0)
    g.add_argument("--force", action="store_true", help="retry even though the last attempt hit a repeat over-current")
    g = rg.add_parser("stop", help="controlled stop: outputs off, input/output path off"); g.add_argument("--timeout", type=float, default=3.0)
    g = rg.add_parser("bench-pwm", help="open-loop gate-signal test (IDLE, input path off): TA1/TA2/TB1/TB2 at PER/CMP2/CMP3 + dead-time")
    g.add_argument("action", choices=["on", "off"]); g.add_argument("--timeout", type=float, default=3.0)
    g = rg.add_parser("set-voltage", help="new target (mV) while running or idle; the firmware slews to it")
    g.add_argument("mv", type=int); g.add_argument("--timeout", type=float, default=3.0)
    g = rg.add_parser("sweep", help="set-voltage through MV,MV,...; per point: control telemetry, debug_log stats, "
                                    "optional scope VAVG/VPP; stops at the first fault")
    g.add_argument("targets", help="comma list of mV, e.g. 24000,20000,12000")
    g.add_argument("--dwell", type=float, default=1.0, help="seconds after the slew before sampling")
    g.add_argument("--scope", metavar="CHn", help="also measure VAVG/VPP on this channel")
    g = rg.add_parser("start", help="set the target (mV) and start from IDLE (after stop or clear-fault)")
    g.add_argument("mv", type=int); g.add_argument("--timeout", type=float, default=5.0)

    # scope
    sc = sub.add_parser("scope", help="Rigol DS1054Z").add_subparsers(dest="op", required=True)
    for n in ("idn", "state", "run", "stop", "single", "force", "autoscale", "clear", "reset", "screenshot"):
        sc.add_parser(n)
    g = sc.add_parser("chan"); g.add_argument("n", type=int, choices=[1, 2, 3, 4])
    g.add_argument("--on", action="store_true"); g.add_argument("--off", action="store_true")
    g.add_argument("--scale", type=float, help="V/div"); g.add_argument("--offset", type=float, help="V")
    g.add_argument("--coupling", choices=["DC", "AC", "GND", "dc", "ac", "gnd"]); g.add_argument("--probe", type=float)
    g.add_argument("--bwl", choices=["20M", "OFF", "20m", "off"]); g.add_argument("--invert", type=lambda x: x.lower() in ("1", "on", "true"))
    g = sc.add_parser("timebase"); g.add_argument("--scale", type=float, help="s/div"); g.add_argument("--offset", type=float)
    g = sc.add_parser("trigger"); g.add_argument("--source"); g.add_argument("--slope", choices=["POS", "NEG", "RFAL", "pos", "neg", "rfal"])
    g.add_argument("--level", type=float); g.add_argument("--sweep", choices=["AUTO", "NORM", "SING", "auto", "norm", "sing"])
    g.add_argument("--coupling"); g.add_argument("--holdoff", type=float)
    g = sc.add_parser("acquire"); g.add_argument("--mdepth", help="AUTO,12k,120k,1.2M,12M,24M (per enabled ch count)")
    g.add_argument("--type", choices=["NORM", "AVER", "PEAK", "HRES", "norm", "aver", "peak", "hres"]); g.add_argument("--averages", type=int)
    g = sc.add_parser("measure"); g.add_argument("sources", nargs="+"); g.add_argument("--items", help="comma list, e.g. VPP,FREQuency,PDUTy")
    g = sc.add_parser("delay", help="two-source timing: RDELay/FDELay/RPHase/FPHase"); g.add_argument("item"); g.add_argument("a"); g.add_argument("b")
    g = sc.add_parser("wait", help="arm single and wait for trigger"); g.add_argument("--timeout", type=float, default=10); g.add_argument("--no-arm", action="store_true")
    g = sc.add_parser("capture", help="download waveform(s) to CSV + summary"); g.add_argument("sources", nargs="+")
    g.add_argument("--raw", action="store_true", help="full memory depth (stops scope)"); g.add_argument("--points", type=int)
    g.add_argument("--threshold", action="append", metavar="[CHn=]V",
                   help="edge threshold in volts, global or per channel (default: midpoint of low/high levels)")
    g.add_argument("--delay", action="append", metavar="CHa:edge,CHb:edge",
                   help="edge-to-edge delay stats over all periods, e.g. CH2:fall,CH3:rise (repeatable)")
    g = sc.add_parser("analyze", help="re-analyse a saved capture CSV offline (no scope needed)")
    g.add_argument("file")
    g.add_argument("--threshold", action="append", metavar="[CHn=]V")
    g.add_argument("--delay", action="append", metavar="CHa:edge,CHb:edge")
    g = sc.add_parser("scpi", help="raw SCPI; queries end with ?"); g.add_argument("command")

    # logic analyzer
    la = sub.add_parser("la", help="logic analyzer via sigrok-cli").add_subparsers(dest="op", required=True)
    la.add_parser("scan"); la.add_parser("decoders")
    g = la.add_parser("capture"); g.add_argument("--channels", required=True, help="e.g. D0=HI,D1=LO")
    g.add_argument("--samplerate", default="4m"); g.add_argument("--samples", type=int); g.add_argument("--time-ms", type=int)
    g.add_argument("--trigger", help="e.g. HI=r"); g.add_argument("--driver"); g.add_argument("--config", action="append")
    g.add_argument("-P", "--decoder", action="append", help="e.g. i2c:scl=D2:sda=D3 (repeatable, stacks)")
    g.add_argument("-A", "--annotations")
    g = la.add_parser("decode"); g.add_argument("file"); g.add_argument("-P", "--decoder", action="append", required=True)
    g.add_argument("-A", "--annotations")
    g = la.add_parser("edges", help="per-channel stats of an existing .sr"); g.add_argument("file")

    # serial
    se = sub.add_parser("serial", help="VCP / UART capture").add_subparsers(dest="op", required=True)
    g = se.add_parser("list"); g.add_argument("--all", action="store_true", help="include non-USB ports")
    g = se.add_parser("capture"); g.add_argument("--seconds", type=float, default=3); g.add_argument("--port"); g.add_argument("--baud", type=int)
    g.add_argument("--hex", action="store_true"); g.add_argument("--until"); g.add_argument("--send")
    g.add_argument("--bytesize", type=int, default=8); g.add_argument("--parity", default="N")
    return p


# Side-effect class per command, for MCP tool annotations / confirmation policy.
#   read      : reads files/hardware state only
#   write     : modifies files in the repo (sources, .ioc) - reversible with git
#   actuate   : changes live hardware state (flash, reset, memory, gate outputs, scope settings)
EFFECTS = {
    "doctor": "read", "schema": "read",
    "ioc get": "read", "ioc search": "read", "ioc ips": "read", "ioc pins": "read", "ioc diff": "read",
    "ioc set": "write", "ioc delete": "write",
    "usercode scan": "read", "usercode list": "read", "usercode get": "read", "usercode set": "write",
    "cubemx generate": "write", "cubemx script": "write", "build": "write",
    "probe list": "read", "probe reset": "actuate", "flash": "actuate",
    "sym": "read", "layout": "read", "mem read": "read", "mem write": "actuate",
    "profile": "read", "regulator status": "read", "regulator clear-fault": "actuate", "regulator stop": "actuate",
    "regulator bench-pwm": "actuate", "regulator set-voltage": "actuate", "regulator sweep": "actuate", "regulator start": "actuate",
    "scope analyze": "read", "scope idn": "read", "scope state": "read", "scope measure": "read",
    "scope delay": "read", "scope screenshot": "read", "scope capture": "actuate",
    "la scan": "read", "la decoders": "read", "la decode": "read", "la edges": "read", "la capture": "read",
    "serial list": "read", "serial capture": "read",
}


def _schema(parser: argparse.ArgumentParser) -> dict:
    """Machine-readable command catalogue (for generating MCP tool definitions)."""
    def args_of(p):
        out = []
        for act in p._actions:
            if isinstance(act, (argparse._HelpAction, argparse._SubParsersAction)):
                continue
            kind = ("flag" if isinstance(act, (argparse._StoreTrueAction, argparse._StoreFalseAction))
                    else "list" if isinstance(act, argparse._AppendAction) or act.nargs in ("+", "*") else "value")
            out.append({
                "name": act.dest, "flags": act.option_strings or None, "positional": not act.option_strings,
                "required": bool(act.required) if act.option_strings else act.nargs not in ("?", "*"),
                "kind": kind, "type": getattr(act.type, "__name__", None) if act.type else "str",
                "choices": list(act.choices) if act.choices else None,
                "default": act.default if act.default not in (None, False, argparse.SUPPRESS) else None,
                "help": act.help,
            })
        return out

    cmds = []

    def walk(p, prefix):
        subs = [a for a in p._actions if isinstance(a, argparse._SubParsersAction)]
        if not subs:
            name = " ".join(prefix)
            cmds.append({"command": name, "argv_prefix": prefix, "effect": EFFECTS.get(name, "actuate"),
                         "help": p.description or None, "args": args_of(p)})
            return
        helps = {ca.dest: ca.help for ca in subs[0]._choices_actions}
        for n, sp in subs[0].choices.items():
            sp.description = sp.description or helps.get(n)
            walk(sp, [*prefix, n])

    walk(parser, [])
    return {"ok": True, "invoke": "tools/bringup/bu <argv_prefix...> <args...>  (prints one JSON object; exit 1 if ok=false)",
            "effects": {"read": "no state change", "write": "modifies repo files (git-reversible)",
                        "actuate": "changes live hardware/instrument state"},
            "commands": cmds}


def dispatch(cfg: dict, a) -> dict:
    c = a.cmd
    if c == "schema":
        return _schema(build_parser())
    if c == "doctor":
        return cmd_doctor(cfg, a)
    if c == "ioc":
        return cmd_ioc(cfg, a)
    if c == "usercode":
        return cmd_usercode(cfg, a)
    if c == "cubemx":
        if a.op == "generate":
            return cubemx.generate(cfg, apply=a.apply, force=a.force, diff_lines=a.diff_lines, only=a.only)
        return cubemx.run_script(cfg, a.commands)
    if c == "build":
        return build_mod.build(cfg, clean=a.clean, reconfigure=a.reconfigure, jobs=a.jobs)
    if c == "probe":
        return probe.list_probes(cfg) if a.op == "list" else probe.reset(cfg, a.hard)
    if c == "flash":
        return probe.flash(cfg, a.elf, verify=not a.no_verify, reset=not a.no_reset)
    if c == "sym":
        syms = probe.symbols(cfg, a.pattern)
        return {"ok": True, "count": len(syms),
                "symbols": {k: {"addr": f"{v[0]:#010x}", "size": v[1]} for k, v in list(syms.items())[:300]}}
    if c == "layout":
        return probe.layout(cfg, a.expr)
    if c == "mem":
        if a.op == "read":
            return probe.mem_read(cfg, a.target, a.size, a.type, a.count, a.save)
        return probe.mem_write(cfg, a.target, a.value, a.type)
    if c == "profile":
        return profile.profile(cfg, a.samples, a.top, a.lines)
    if c == "regulator":
        if a.op == "status":
            return regulator.status(cfg)
        if a.op == "sweep":
            return regulator.sweep(cfg, [int(x) for x in a.targets.split(",")], a.dwell, scope_source=a.scope)
        name = f"bench-pwm-{a.action}" if a.op == "bench-pwm" else a.op
        return regulator.command(cfg, name, a.timeout, getattr(a, "force", False), getattr(a, "mv", None))
    if c == "scope":
        return cmd_scope(cfg, a)
    if c == "la":
        if a.op == "scan":
            return logic.scan(cfg)
        if a.op == "decoders":
            return logic.list_decoders(cfg)
        if a.op == "capture":
            return logic.capture(cfg, a.channels, a.samplerate, a.samples, a.time_ms, a.trigger, a.driver,
                                 a.decoder, a.annotations, a.config)
        if a.op == "decode":
            return {"ok": True, **logic.decode(cfg, a.file, a.decoder, a.annotations)}
        return {"ok": True, **logic.edges(cfg, a.file)}
    if c == "serial":
        if a.op == "list":
            return serialmon.list_ports(a.all)
        return serialmon.capture(cfg, a.seconds, a.port, a.baud, a.hex, a.until, a.send, a.bytesize, a.parity)
    raise ToolError(f"unknown command {c}")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = dispatch(load_config(), args)
    except ToolError as e:
        result = {"ok": False, "error": str(e), **e.details}
    except KeyboardInterrupt:
        result = {"ok": False, "error": "interrupted"}
    except Exception as e:  # noqa: BLE001 - always emit JSON for the agent
        result = {"ok": False, "error": f"{type(e).__name__}: {e}", "traceback": traceback.format_exc(limit=5)}
    print(json.dumps(result, indent=1, default=str))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
