import struct

from bringup import load


def elf32(segments):
    """Minimal little-endian ELF32 with one PT_LOAD per (paddr, data)."""
    phoff, phentsize = 52, 32
    data_off = phoff + phentsize * len(segments)
    hdr = bytearray(52)
    hdr[:6] = b"\x7fELF\x01\x01"
    struct.pack_into("<I", hdr, 0x1C, phoff)
    struct.pack_into("<HH", hdr, 0x2A, phentsize, len(segments))
    ph, body = bytearray(), bytearray()
    for paddr, data in segments:
        ph += struct.pack("<8I", 1, data_off + len(body), paddr, paddr, len(data), len(data), 5, 4)
        body += data
    return bytes(hdr + ph + body)


def test_uf2_blocks_cover_flash_segments_only():
    segs = load.elf_flash_segments(elf32([(0x10000000, b"\xaa" * 256), (0x100001c0, b"\x55" * 64),
                                          (0x20000000, b"\x11" * 16)]))
    assert [a for a, _ in segs] == [0x10000000, 0x100001c0]
    uf2 = load.uf2_from_segments(segs)
    assert len(uf2) == 2 * 512
    m0, m1, flags, addr, size, n, total, fam = struct.unpack_from("<8I", uf2, 512)
    assert (m0, m1, flags, fam) == (load.UF2_MAGIC0, load.UF2_MAGIC1, 0x2000, load.RP2040_FAMILY_ID)
    assert (addr, size, n, total) == (0x10000100, 256, 1, 2)
    payload = uf2[512 + 32:512 + 32 + 256]
    assert payload[:0xc0] == bytes(0xc0) and payload[0xc0:] == b"\x55" * 64
    assert struct.unpack_from("<I", uf2, 1020)[0] == load.UF2_MAGIC_END
