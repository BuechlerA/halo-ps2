"""Bitmap / sound / script statistics on a synthetic tag region (no game data needed)."""
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import inventory, tagindex  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
SYNTAX_NODES = 4
STRINGS = b"\0begin\0sleep\0"


def build() -> tuple[bytes, tagindex.TagIndex]:
    # Tag data blocks are zero-filled by build(); the 0x600-byte scenario holds the script tagdata.
    tags = [("scnr", "levels/t/t", 0x600), ("bitm", "ui/b", 0x70), ("snd!", "sfx/s", 0xA4)]
    region = bytearray(tagindex.TagIndex.build(tags))
    idx = tagindex.TagIndex.parse_region(bytes(region), 0)
    addr = {e.group: e.data_address for e in idx.entries}
    off = lambda a: a - BASE  # noqa: E731
    tail = BASE + len(region)          # scratch space appended after the tag structs

    def alloc(blob: bytes) -> int:
        nonlocal tail
        a = tail
        region.extend(blob + bytes(-len(blob) % 4))
        tail = BASE + len(region)
        return a

    # bitmap: two images, dxt1 256x256 (compressed|pow2) and a8r8g8b8 64x64 cube (swizzled|pow2)
    def bdata(w, h, typ, fmt, flags, size):
        b = bytearray(48)
        b[0:4] = b"mtib"
        struct.pack_into("<6H", b, 4, w, h, 1, typ, fmt, flags)
        struct.pack_into("<I", b, 0x1C, size)
        return bytes(b)
    blk = alloc(bdata(256, 256, 0, 14, 0b11, 32768) + bdata(64, 64, 2, 11, 0b1001, 16384))
    struct.pack_into("<II", region, off(addr["bitm"]) + 0x60, 2, blk)

    # sound: stereo 44100 Xbox ADPCM, one pitch range with two permutations
    s = off(addr["snd!"])
    struct.pack_into("<H", region, s + 6, 1)
    struct.pack_into("<HH", region, s + 0x6C, 1, 1)
    perm = bytearray(2 * 124)
    struct.pack_into("<I", perm, 0x40, 1000)
    struct.pack_into("<I", perm, 124 + 0x40, 500)
    pr = bytearray(72)
    struct.pack_into("<II", pr, 0x3C, 2, alloc(bytes(perm)))
    struct.pack_into("<II", region, s + 0x98, 1, alloc(bytes(pr)))

    # scripts: one dead node, a begin(), two sleep() function names and a value node
    syn = bytearray(b"script node".ljust(0x38, b"\0"))
    def node(salt, fn, typ, soff):
        return struct.pack("<HHHHIII", salt, fn, typ, 9, 0xFFFFFFFF, soff, 0)
    syn += node(0, 0, 0, 0) + node(1, 0, 2, 1) + node(2, 19, 2, 7) + node(3, 19, 2, 7)
    sc = off(addr["scnr"])
    struct.pack_into("<I", region, sc + 0x474, len(syn))
    struct.pack_into("<I", region, sc + 0x480, alloc(bytes(syn)))
    struct.pack_into("<I", region, sc + 0x488, len(STRINGS))
    struct.pack_into("<I", region, sc + 0x494, alloc(STRINGS))
    return bytes(region), idx


def main() -> None:
    data, idx = build()
    reg = inventory.Region(data)
    b = inventory.bitmap_stats(reg, idx)
    assert (b["tags"], b["images"], b["pixel_bytes"]) == (1, 2, 32768 + 16384)
    assert b["formats"] == {"dxt1": 1, "a8r8g8b8": 1}
    assert b["types"] == {"2d": 1, "cube": 1}
    assert b["flags"] == {"pow2": 2, "compressed": 1, "swizzled": 1}
    assert b["resolutions"] == {"256x256": 1, "64x64": 1}

    s = inventory.sound_stats(reg, idx)
    assert (s["tags"], s["permutations"], s["sample_bytes"]) == (1, 2, 1500)
    assert s["kinds"] == {"xbox-adpcm 44100 Hz stereo": 2}

    sc = inventory.script_stats(reg, idx)
    assert sc["nodes"] == 3, sc            # the salt-0 node is a free slot
    assert sc["builtins"] == {"sleep": 2, "begin": 1}, sc
    assert not reg.problems, reg.problems

    # a pointer outside the region is reported, not raised
    bad = bytearray(data)
    bsp = {e.group: e.data_address for e in idx.entries}["bitm"] - BASE
    struct.pack_into("<II", bad, bsp + 0x60, 1, 0x70000000)
    r2 = inventory.Region(bytes(bad))
    assert inventory.bitmap_stats(r2, idx)["images"] == 0 and r2.problems
    print("ok")


if __name__ == "__main__":
    main()
