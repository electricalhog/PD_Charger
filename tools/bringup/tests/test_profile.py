from bringup import probe, profile


def test_function_of_bisects_text_symbols():
    syms = [(0x100, 0x20, "a"), (0x200, 0x10, "b")]
    starts = [s[0] for s in syms]
    assert profile._function_of(0x110, starts, syms) == "a"
    assert profile._function_of(0x20e, starts, syms) == "b"
    assert profile._function_of(0x150, starts, syms).startswith("?")


def test_sample_keeps_repeated_reads_in_order(monkeypatch):
    """read_regions collapses repeated addresses; the sampler must not."""
    tcb = 0x20000100
    out = "".join(f"0xE000101C : {pc:08X}\n0xE000ED04 : 0000002C\n0x20000100 : 20001ED0\n"
                  for pc in (0x08001000, 0x08002000, 0x08003000))
    monkeypatch.setattr(probe, "_connect", lambda cfg, mode=None: [])
    monkeypatch.setattr(probe, "_prun", lambda cfg, args, timeout: (out, 0))
    monkeypatch.setattr(probe, "_check", lambda *a: None)
    got = profile._sample({}, tcb, 3, batch=3)
    assert [s[0] for s in got] == [0x08001000, 0x08002000, 0x08003000]
    assert all(s[1] & 0x1FF == 0x2C and s[2] == 0x20001ED0 for s in got)
