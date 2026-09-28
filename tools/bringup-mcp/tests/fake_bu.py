#!/usr/bin/env python3
"""A stand-in for tools/bringup/bu used by the offline tests.

Prints one JSON object per call like the real thing. `schema` returns a small catalogue with
one command of each effect and the argument kinds the real schema uses. Every other command
echoes its argv, so tests can check the argv the server built. Special commands:
  fail   -> {"ok": false, "error": "..."}
  slow   -> sleeps SECONDS (first arg) then answers ok
  junk   -> prints something that is not JSON
"""

import json
import sys
import time

SCHEMA = {
    "ok": True,
    "invoke": "tools/bringup/bu <argv_prefix...> <args...>",
    "effects": {"read": "no state change", "write": "modifies repo files", "actuate": "changes live hardware"},
    "commands": [
        {"command": "schema", "argv_prefix": ["schema"], "effect": "read", "help": "catalogue", "args": []},
        {"command": "doctor", "argv_prefix": ["doctor"], "effect": "read", "help": "check toolchain",
         "args": [{"name": "offline", "flags": ["--offline"], "positional": False, "required": False,
                   "kind": "flag", "type": "str", "choices": None, "default": None, "help": "skip hardware checks"}]},
        {"command": "ioc get", "argv_prefix": ["ioc", "get"], "effect": "read", "help": "read .ioc keys",
         "args": [{"name": "keys", "flags": None, "positional": True, "required": True,
                   "kind": "list", "type": "str", "choices": None, "default": None, "help": None}]},
        {"command": "ioc set", "argv_prefix": ["ioc", "set"], "effect": "write", "help": "edit .ioc",
         "args": [{"name": "pairs", "flags": None, "positional": True, "required": True,
                   "kind": "list", "type": "str", "choices": None, "default": None, "help": "KEY=VALUE"},
                  {"name": "dry_run", "flags": ["--dry-run"], "positional": False, "required": False,
                   "kind": "flag", "type": "str", "choices": None, "default": None, "help": None}]},
        {"command": "flash", "argv_prefix": ["flash"], "effect": "actuate", "help": "program the target",
         "args": [{"name": "elf", "flags": ["--elf"], "positional": False, "required": False,
                   "kind": "value", "type": "str", "choices": None, "default": None, "help": "ELF path"},
                  {"name": "no_verify", "flags": ["--no-verify"], "positional": False, "required": False,
                   "kind": "flag", "type": "str", "choices": None, "default": None, "help": None}]},
        {"command": "regulator clear-fault", "argv_prefix": ["regulator", "clear-fault"], "effect": "actuate",
         "help": "re-arm the ADM1270",
         "args": [{"name": "timeout", "flags": ["--timeout"], "positional": False, "required": False,
                   "kind": "value", "type": "float", "choices": None, "default": 3.0, "help": None},
                  {"name": "force", "flags": ["--force"], "positional": False, "required": False,
                   "kind": "flag", "type": "str", "choices": None, "default": None, "help": "retry after over-current"}]},
        {"command": "scope capture", "argv_prefix": ["scope", "capture"], "effect": "actuate", "help": "download waveforms",
         "args": [{"name": "sources", "flags": None, "positional": True, "required": True,
                   "kind": "list", "type": "str", "choices": None, "default": None, "help": None},
                  {"name": "raw", "flags": ["--raw"], "positional": False, "required": False,
                   "kind": "flag", "type": "str", "choices": None, "default": None, "help": None},
                  {"name": "delay", "flags": ["--delay"], "positional": False, "required": False,
                   "kind": "list", "type": "str", "choices": None, "default": None, "help": "A:edge,B:edge (repeatable)"},
                  {"name": "points", "flags": ["--points"], "positional": False, "required": False,
                   "kind": "value", "type": "int", "choices": None, "default": None, "help": None}]},
        {"command": "scope trigger", "argv_prefix": ["scope", "trigger"], "effect": "actuate", "help": "trigger setup",
         "args": [{"name": "slope", "flags": ["--slope"], "positional": False, "required": False,
                   "kind": "value", "type": "str", "choices": ["POS", "NEG"], "default": None, "help": None}]},
        {"command": "fail", "argv_prefix": ["fail"], "effect": "read", "help": "always fails", "args": []},
        {"command": "slow", "argv_prefix": ["slow"], "effect": "read", "help": "sleeps",
         "args": [{"name": "seconds", "flags": None, "positional": True, "required": True,
                   "kind": "value", "type": "float", "choices": None, "default": None, "help": None}]},
        {"command": "junk", "argv_prefix": ["junk"], "effect": "read", "help": "not json", "args": []},
        {"command": "big", "argv_prefix": ["big"], "effect": "read", "help": "huge output", "args": []},
    ],
}


def main() -> int:
    argv = sys.argv[1:]
    if argv == ["schema"]:
        print(json.dumps(SCHEMA))
        return 0
    if argv[:1] == ["fail"]:
        print(json.dumps({"ok": False, "error": "scope not configured", "hint": "set [scope].resource"}))
        return 1
    if argv[:1] == ["slow"]:
        time.sleep(float(argv[1]))
        print(json.dumps({"ok": True, "slept": float(argv[1])}))
        return 0
    if argv[:1] == ["junk"]:
        print("this is not json")
        return 0
    if argv[:1] == ["big"]:
        print(json.dumps({"ok": True, "blob": "x" * 200000, "n": 1}))
        return 0
    print(json.dumps({"ok": True, "argv": argv}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
