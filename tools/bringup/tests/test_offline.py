"""Offline tests: no CubeMX, probe, or instruments needed."""

import math
import shutil
from pathlib import Path

import pytest

from bringup import usercode
from bringup.config import REPO_ROOT, ToolError
from bringup.cubemx import _parse_script_output
from bringup.ioc import Ioc
from bringup import analysis

REAL_IOC = REPO_ROOT / "STM32CubeIDE/final/final.ioc"
REAL_MAIN = REPO_ROOT / "STM32CubeIDE/final/Core/Src/main.c"


# ------------------------------------------------------------------ ioc

def test_ioc_roundtrip_is_byte_identical(tmp_path):
    dst = tmp_path / "x.ioc"
    shutil.copy(REAL_IOC, dst)
    Ioc(dst).save()
    assert dst.read_bytes() == REAL_IOC.read_bytes()


def test_ioc_unescapes_keys_and_values():
    ioc = Ioc(REAL_IOC)
    assert ioc.get("ADC1.Rank-2#ChannelRegularConversion") == "1"
    assert "Rank-2#ChannelRegularConversion" in ioc.get("ADC1.IPParameters").split(",")


def test_ioc_set_existing_keeps_index(tmp_path):
    dst = tmp_path / "x.ioc"
    shutil.copy(REAL_IOC, dst)
    ioc = Ioc(dst)
    ch = ioc.set("FREERTOS.configTOTAL_HEAP_SIZE", "20000")
    assert ch["old"] == "7000" and ch["side_effects"] == []
    ioc.save()
    text = dst.read_text()
    assert "FREERTOS.configTOTAL_HEAP_SIZE=20000\n" in text
    assert len(text.splitlines()) == len(REAL_IOC.read_text().splitlines())


def test_ioc_set_new_param_adds_to_ipparameters(tmp_path):
    dst = tmp_path / "x.ioc"
    shutil.copy(REAL_IOC, dst)
    ioc = Ioc(dst)
    ch = ioc.set("FREERTOS.configUSE_MALLOC_FAILED_HOOK", "1")
    assert ch["side_effects"] == ["added configUSE_MALLOC_FAILED_HOOK to FREERTOS.IPParameters"]
    ioc.save()
    re_ioc = Ioc(dst)
    assert re_ioc.get("FREERTOS.configUSE_MALLOC_FAILED_HOOK") == "1"
    assert re_ioc.get("FREERTOS.IPParameters").endswith(",configUSE_MALLOC_FAILED_HOOK")
    # Inserted in sorted position, not appended at EOF.
    keys = [k for k, _ in re_ioc.items() if k.startswith("FREERTOS.")]
    assert keys == sorted(keys)


def test_ioc_escapes_hash_and_colon(tmp_path):
    dst = tmp_path / "x.ioc"
    shutil.copy(REAL_IOC, dst)
    ioc = Ioc(dst)
    ioc.set("ADC1.SamplingTime-9#ChannelRegularConversion", "a:b")
    ioc.save()
    assert "ADC1.SamplingTime-9\\#ChannelRegularConversion=a\\:b" in dst.read_text()
    assert Ioc(dst).get("ADC1.SamplingTime-9#ChannelRegularConversion") == "a:b"


def test_ioc_pin_param_creates_gpioparameters(tmp_path):
    dst = tmp_path / "x.ioc"
    shutil.copy(REAL_IOC, dst)
    ioc = Ioc(dst)
    ioc.set("PA8.GPIO_Speed", "GPIO_SPEED_FREQ_HIGH")
    assert ioc.get("PA8.GPIOParameters") == "GPIO_Label,GPIO_Speed"
    ioc.set("PA8.Mode", "Output_TA1TA2")  # never listed
    assert ioc.get("PA8.GPIOParameters") == "GPIO_Label,GPIO_Speed"
    ioc.delete("PA8.GPIO_Speed")
    assert ioc.get("PA8.GPIOParameters") == "GPIO_Label"


def test_ioc_preserves_crlf(tmp_path):
    dst = tmp_path / "crlf.ioc"
    dst.write_bytes(b"#MicroXplorer\r\nA.IPParameters=x\r\nA.x=1\r\n")
    ioc = Ioc(dst)
    ioc.set("A.y", "2")
    ioc.save()
    assert dst.read_bytes() == b"#MicroXplorer\r\nA.IPParameters=x,y\r\nA.x=1\r\nA.y=2\r\n"


# ------------------------------------------------------------------ usercode

def test_usercode_parse_real_main():
    secs = {s.id: s for s in usercode.list_sections(REAL_MAIN)}
    assert "RTOS_THREADS" in secs and "ADC1_Init 2" in secs
    assert all(s.end_line > s.begin_line for s in secs.values())


def test_usercode_set_preserves_crlf_and_other_bytes(tmp_path):
    f = tmp_path / "main.c"
    shutil.copy(REAL_MAIN, f)
    orig = REAL_MAIN.read_bytes()
    assert b"\r\n" in orig
    old = usercode.get_section(f, "RTOS_THREADS").body
    usercode.set_section(f, "RTOS_THREADS", "  foo();\n  bar();\n")
    new = f.read_bytes()
    assert b"  foo();\r\n  bar();\r\n" in new
    assert new.count(b"\n") == new.count(b"\r\n")
    usercode.set_section(f, "RTOS_THREADS", old)
    assert f.read_bytes() == orig


def test_usercode_duplicates_and_errors():
    text = "/* USER CODE BEGIN X */\na\n/* USER CODE END X */\n/* USER CODE BEGIN X */\nb\n/* USER CODE END X */\n"
    assert [s.id for s in usercode.parse(text)] == ["X", "X#2"]
    with pytest.raises(ToolError):
        usercode.parse("/* USER CODE BEGIN A */\n")
    with pytest.raises(ToolError):
        usercode.parse("/* USER CODE BEGIN A */\n/* USER CODE END B */\n")


def test_usercode_formfeed_does_not_shift_lines():
    text = "x\f y\n/* USER CODE BEGIN A */\nbody\n/* USER CODE END A */\n"
    (s,) = usercode.parse(text)
    assert (s.begin_line, s.end_line, s.body) == (2, 4, "body\n")


def test_strip_user_code_ignores_user_edits():
    a = "gen1\n/* USER CODE BEGIN A */\nmine\n/* USER CODE END A */\ngen2\n"
    b = "gen1\n/* USER CODE BEGIN A */\nother\n/* USER CODE END A */\ngen2\n"
    assert usercode.strip_user_code(a) == usercode.strip_user_code(b)


# ------------------------------------------------------------------ cubemx / scope

def test_cubemx_script_output_parsing():
    out = "junk\nconfig load /x/final.ioc\n1 : Invalid condition id\nOK\nproject generate\npathGccArm : y\nKO\nexit\nBye bye\n"
    r = _parse_script_output(out, ["config load /x/final.ioc", "project generate"])
    assert [x["status"] for x in r] == ["OK", "KO"]


def test_analysis_square_wave():
    dt = 1e-6
    t = [i * dt for i in range(1000)]
    v = [3.3 if (i // 25) % 4 == 0 else 0.0 for i in range(1000)]  # 100 us period, 25% duty
    r = analysis.channel_report(v, t)
    assert math.isclose(r["freq_hz"], 10_000, rel_tol=1e-6)
    assert math.isclose(r["duty_pct"], 25, abs_tol=0.5)
    assert r["high"] == pytest.approx(3.3) and r["low"] == pytest.approx(0.0)


def test_analysis_dead_time_stats(tmp_path):
    """Complementary pair with 12 ns / 17 ns dead-times, 1 ns samples, finite edges."""
    per, dt = 1000e-9, 1e-9
    t = [i * dt for i in range(5000)]

    def ramp(x, t0, tr=8e-9):  # 0..3.3 V linear edge starting at t0
        return 3.3 * min(max((x - t0) / tr, 0.0), 1.0)
    hi, lo = [], []
    for x in t:
        p = x % per
        # HI: rises at 0, falls at 600 ns. LO: rises 12 ns after HI falls, falls 17 ns before HI rises.
        hi.append(ramp(p, 0) - ramp(p, 600e-9))
        lo.append(ramp(p, 612e-9) - ramp(p, per - 17e-9))
    f = tmp_path / "c.csv"
    f.write_text("t_s,CH2,CH3\n" + "".join(f"{a:.12e},{b:.5g},{c:.5g}\n" for a, b, c in zip(t, hi, lo)))
    r = analysis.analyze(f, {"*": 1.65}, ["CH2:fall,CH3:rise", "CH3:fall,CH2:rise"])
    d1, d2 = r["delays"]
    assert d1["count"] >= 4 and d1["mean_s"] == pytest.approx(12e-9, abs=0.2e-9)
    assert d2["count"] >= 4 and d2["mean_s"] == pytest.approx(17e-9, abs=0.2e-9)
    assert r["channels"]["CH2"]["freq_hz"] == pytest.approx(1e6, rel=1e-6)


def test_parse_delay_rejects_garbage():
    with pytest.raises(ToolError):
        analysis.parse_delay("CH2-fall")
