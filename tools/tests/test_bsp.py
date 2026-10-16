"""BSP block location and summary on a synthetic map region (no game data needed)."""
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import bsp, tagindex  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
LOAD = 0x81000000


def build_block() -> bytes:
    """Header + structure_bsp + 4 surfaces + 1 lightmap with 2 materials + 3 clusters."""
    blk = bytearray(bsp.HEADER_SIZE)
    root = len(blk)
    blk += bytes(0x27C)
    surf = len(blk); blk += bytes(4 * 6)
    clus = len(blk); blk += bytes(3 * 16)
    mats = len(blk); blk += bytes(2 * bsp.MATERIAL_SIZE)
    lm = len(blk); blk += bytes(bsp.LIGHTMAP_SIZE)
    struct.pack_into("<5I4s", blk, 0, LOAD + root, 2, LOAD + 0x100, 2, LOAD + 0x200, bsp.SBSP_MAGIC)
    struct.pack_into("<II", blk, root + 0xF8, 4, LOAD + surf)
    struct.pack_into("<II", blk, root + 0x128, 3, LOAD + clus)
    struct.pack_into("<II", blk, root + 0x104, 1, LOAD + lm)
    struct.pack_into("<II", blk, lm + 0x14, 2, LOAD + mats)
    for i, (first, n, rv, lv) in enumerate([(0, 3, 10, 9), (3, 1, 5, 0)]):
        o = mats + i * bsp.MATERIAL_SIZE
        struct.pack_into("<ii", blk, o + 0x14, first, n)
        struct.pack_into("<I", blk, o + 0xB4, rv)
        struct.pack_into("<I", blk, o + 0xC8, lv)
    return bytes(blk)


def main() -> None:
    block = build_block()
    loc = bsp.BspLocation(0x800, len(block), LOAD, "levels\\t\\ta")
    s = bsp.summarise(loc, block)
    assert not s.problems, s.problems
    assert s.counts["surfaces"] == s.triangles == 4 and s.counts["clusters"] == 3
    assert (s.lightmap_materials, s.rendered_vertices, s.lightmap_vertices) == (2, 15, 9)

    bad = bytearray(block)
    struct.pack_into("<ii", bad, bsp.HEADER_SIZE + 0x27C + 4 * 6 + 3 * 16 + 0x14, 2, 9)   # 2+9 > 4 surfaces
    assert any("exceeds" in p for p in bsp.summarise(loc, bytes(bad)).problems)
    for broken in (block[:12], bytes(len(block))):
        try:
            bsp.summarise(loc, broken)
            raise SystemExit("bad block accepted")
        except (bsp.BspError, struct.error):
            pass

    # locate(): scenario reflexive at +0x5A4 -> one entry
    tags = [("scnr", "levels/t/t", 0x600), ("sbsp", "levels/t/ta", 0)]
    region = bytearray(tagindex.TagIndex.build(tags))
    idx = tagindex.TagIndex.parse_region(bytes(region), 0)
    entry_addr = BASE + len(region)
    sbsp_id = idx.entries[1].tag_id
    region += struct.pack("<8I", 0x800, len(block), LOAD, 0, 0x73627370, 0, 0, sbsp_id)
    sc = idx.scenario.data_address - BASE
    struct.pack_into("<II", region, sc + bsp.SCENARIO_BSPS, 1, entry_addr)
    locs = bsp.locate(idx, bytes(region), 0x800 + len(block))
    assert [(l.file_offset, l.size, l.address, l.path) for l in locs] == [(0x800, len(block), LOAD, "levels/t/ta")]
    try:
        bsp.locate(idx, bytes(region), 0x800)
        raise SystemExit("out-of-file block accepted")
    except bsp.BspError:
        pass
    print("ok")


if __name__ == "__main__":
    main()
