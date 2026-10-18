"""Synthetic-map test for tagcache (no game data needed)."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import mapinfo, tagcache  # noqa: E402

BASE = mapinfo.XBOX_TAG_BASE
INDEX_OFF = 0x800


def build_map() -> bytes:
    """Two tags: a scenario and a bitmap with one 64x64 DXT1 2D image."""
    buf = bytearray(0x4000)
    addr = lambda off: BASE + (off - INDEX_OFF)  # noqa: E731
    paths = {0x400: b"levels\\test\\test\0", 0x420: b"ui\\bitmaps\\a\0"}
    for off, s in paths.items():
        buf[INDEX_OFF + off:INDEX_OFF + off + len(s)] = s
    bm_tag, bm_elem = 0x500, 0x600
    struct.pack_into("<II", buf, INDEX_OFF + bm_tag + 0x60, 1, addr(INDEX_OFF + bm_elem))
    struct.pack_into("<4sHHHHHHhhHHII", buf, INDEX_OFF + bm_elem, b"mtib", 64, 64, 1, 0, 14, 0, 0, 0, 6, 0, 0, 2816)
    struct.pack_into("<9I", buf, INDEX_OFF, addr(INDEX_OFF + 36), 0, 0, 2, 0, 0, 0, 0, 0)
    for i, (g, path, data) in enumerate([(b"rncs", 0x400, 0x700), (b"mtib", 0x420, bm_tag)]):
        struct.pack_into("<4s4s4sIIIII", buf, INDEX_OFF + 36 + 32 * i, g, b"\xff" * 4, b"\xff" * 4,
                         0xE1740000 + i, addr(INDEX_OFF + path), addr(INDEX_OFF + data), 0, 0)
    struct.pack_into("<6I", buf, 0, mapinfo.MAGIC_HEAD, 5, len(buf), 0, INDEX_OFF, 0x3000)
    struct.pack_into("<I", buf, 0x7FC, mapinfo.MAGIC_FOOT)
    buf[0x20:0x24] = b"test"
    return bytes(buf)


def test_index_and_bitmaps():
    m = tagcache.CacheMap(build_map())
    assert [(t.group, t.path) for t in m.tags] == [("scnr", "levels\\test\\test"), ("bitm", "ui\\bitmaps\\a")]
    (t, w, h, d, ty, fmt, mips, size), = list(m.bitmaps())
    assert (w, h, d, ty, tagcache.BITMAP_FORMATS[fmt], mips, size) == (64, 64, 1, 0, "dxt1", 6, 2816)
    assert m.script_function_usage() == (0, {})


def test_rejects_size_mismatch():
    try:
        tagcache.CacheMap(build_map() + b"\0")
    except ValueError:
        return
    raise AssertionError("expected ValueError")


if __name__ == "__main__":
    test_index_and_bitmaps()
    test_rejects_size_mismatch()
    print("ok")
