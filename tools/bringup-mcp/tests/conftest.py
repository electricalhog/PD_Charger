import os
import shutil
import stat
import sys
from pathlib import Path

import pytest

FAKE_BU = Path(__file__).with_name("fake_bu.py")


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    """A fake repo checkout with the fake bu at tools/bringup/bu."""
    root = tmp_path / "PD_Charger"
    (root / "tools/bringup").mkdir(parents=True)
    bu = root / "tools/bringup/bu"
    bu.write_text(f"#!/bin/sh\nexec {sys.executable} {FAKE_BU} \"$@\"\n")
    bu.chmod(bu.stat().st_mode | stat.S_IXUSR)
    (root / "bringup_out").mkdir()
    # a git repo so server_version and git push checks have something to read
    if shutil.which("git"):
        os.system(f"cd {root} && git init -q -b jonah-bench && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m init")
    return root


def write_config(root: Path, **overrides) -> Path:
    shell = overrides.pop("shell", None)
    lines = [
        "[repo]", f'root = "{root}"', "",
        "[bench]", 'state_file = "bringup_out/bench-state.json"', "",
        "[timeouts]", "default_s = 5", '"slow" = 1.0', "",
        "[limits]", "max_result_bytes = 20000", "max_field_bytes = 4000", "",
    ]
    if shell is not None:
        lines += ["[shell]", f"enabled = {'true' if shell.get('enabled') else 'false'}",
                  "allow = [" + ", ".join(f'"{a}"' for a in shell.get("allow", [])) + "]",
                  'deny_git_push_to = ["main", "master", "jonah-bench"]', "timeout_s = 20", "max_output_bytes = 300", ""]
    p = root / "bringup-mcp.toml"
    p.write_text("\n".join(lines))
    return p


@pytest.fixture
def config_path(repo: Path) -> Path:
    return write_config(repo)
