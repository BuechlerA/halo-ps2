"""Round-trip test with a synthetic Xbox-style cache file (no game data needed)."""
import struct
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import mapinfo  # noqa: E402


def make_header(decompressed_size: int, tag_off: int, tag_size: int) -> bytes:
    h = bytearray(mapinfo.HEADER_SIZE)
    struct.pack_into("<6I", h, 0, mapinfo.MAGIC_HEAD, 5, decompressed_size, 0, tag_off, tag_size)
    h[0x20:0x20 + 4] = b"test"
    h[0x40:0x40 + 9] = b"01.10.12."
    struct.pack_into("<H", h, 0x60, 0)
    struct.pack_into("<I", h, 0x64, 0xDEADBEEF)
    struct.pack_into("<I", h, 0x7FC, mapinfo.MAGIC_FOOT)
    return bytes(h)


def test_roundtrip():
    body = bytes(range(256)) * 4096  # 1 MiB, compressible
    hdr = make_header(mapinfo.HEADER_SIZE + len(body), 0x100, 0x800)
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "test.map"
        src.write_bytes(hdr + zlib.compress(body, 9))
        parsed = mapinfo.read_header(src)
        assert parsed.engine_name == "xbox" and parsed.name == "test"
        assert mapinfo.is_compressed(src, parsed)
        out = Path(td) / "out" / "test.map"
        n = mapinfo.decompress(src, out)
        assert n == parsed.decompressed_size
        assert out.read_bytes() == hdr + body
        rows = mapinfo.info_rows([src])
        assert rows[0]["compressed"] and rows[0]["ratio"] > 1


def test_rejects_garbage():
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "junk.map"
        p.write_bytes(b"\0" * mapinfo.HEADER_SIZE)
        try:
            mapinfo.read_header(p)
        except ValueError:
            return
        raise AssertionError("expected ValueError")


if __name__ == "__main__":
    test_roundtrip()
    test_rejects_garbage()
    print("ok")
