"""BSP geometry conversion on a synthetic BSP block: read, bake, round-trip, pack.
Set BSP_FIXTURE=<path> to also write the resulting .hp2 for the C reader test."""
import math
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import bsp, bspconv, hp2  # noqa: E402

LOAD = 0x81000000
NV = 40                      # vertices of the one material; a 40-triangle grid strip


def vertex(i: int) -> tuple[float, float, float, float, float, float, float]:
    x, y = (i % 8) * 3.0 - 11.0, (i // 8) * 2.5 + 100.0
    return x, y, 0.5 * math.sin(i), (i % 8) / 4.0, (i // 8) / 2.0, ((i % 8) / 8.0), (i // 8) / 5.0


def build_block() -> tuple[bytes, bsp.BspLocation]:
    blk = bytearray(bsp.HEADER_SIZE)
    root = len(blk); blk += bytes(0x27C)
    tris = [(i % (NV - 2), i % (NV - 2) + 1, i % (NV - 2) + 2) for i in range(40)]
    surf = len(blk); blk += b"".join(struct.pack("<3H", *t) for t in tris) + bytes(2)
    rend, lmv = bytearray(), bytearray()
    for i in range(NV):
        x, y, z, u, v, lu, lv = vertex(i)
        rend += struct.pack("<3fIII2f", x, y, z, 0, 0, 0, u, v)
        lmv += struct.pack("<I2h", 0, round(lu * 32767), round(lv * 32767))
    vbl = len(blk); blk += bytes(rend + lmv)
    mats = len(blk); blk += bytes(bsp.MATERIAL_SIZE)
    lm = len(blk); blk += bytes(bsp.LIGHTMAP_SIZE)
    struct.pack_into("<5I4s", blk, 0, LOAD + root, 1, LOAD + vbl, 1, LOAD + vbl, bsp.SBSP_MAGIC)
    struct.pack_into("<II", blk, root + 0xF8, len(tris), LOAD + surf)
    struct.pack_into("<II", blk, root + 0x104, 1, LOAD + lm)
    struct.pack_into("<6f", blk, root + 0xC8, -11.0, 10.0, 100.0, 117.5, -0.5, 0.5)
    struct.pack_into("<HH", blk, lm, 3, 0)
    struct.pack_into("<II", blk, lm + 0x14, 1, LOAD + mats)
    struct.pack_into("<I", blk, mats + 0x0C, 0xE1740042)
    struct.pack_into("<ii", blk, mats + 0x14, 0, len(tris))
    struct.pack_into("<I", blk, mats + 0xB4, NV)
    struct.pack_into("<I", blk, mats + 0xC8, NV)
    struct.pack_into("<4I", blk, mats + 0xEC, len(rend) + len(lmv), 0, 0, LOAD + vbl)
    return bytes(blk), bsp.BspLocation(0x800, len(blk), LOAD, "levels\\t\\ta")


def main() -> None:
    block, loc = build_block()
    mats, bounds = bspconv.read_materials(block, loc)
    assert len(mats) == 1 and len(mats[0].tris) == 40 and mats[0].shader_id == 0xE1740042
    assert mats[0].lightmap_page == 3 and bounds == (-11.0, 100.0, -0.5, 10.0, 117.5, 0.5)

    blob = bspconv.build_section(mats, bounds)
    sec = bspconv.parse_section(blob)
    assert sec.triangles == 40 and len(sec.batches) == 2            # 32 + 8
    assert [b["triangles"] for b in sec.batches] == [32, 8]
    assert all(b["vif_offset"] % 16 == 0 for b in sec.batches)
    flat = [vi for t in mats[0].tris for vi in t]
    got = [v for b in sec.batches for v in bspconv.decode_batch(sec, b)]
    assert len(got) == len(flat) == 120
    for g, vi in zip(got, flat):
        x, y, z, u, v, lu, lv = vertex(vi)
        radius = max(b["radius"] for b in sec.batches)
        assert math.dist(g["pos"], (x, y, z)) <= radius / 65534 * 1.8 + 1e-6, (g["pos"], (x, y, z))
        shift = sec.materials[0]["shift"]
        assert abs(g["uv"][0] - u) <= 1 / (1 << shift) and abs(g["uv"][1] - v) <= 1 / (1 << shift)
        assert abs(g["lmuv"][0] - lu) < 1e-4 and abs(g["lmuv"][1] - lv) < 1e-4
    for b in sec.batches:                       # every vertex lies inside its culling sphere
        assert all(math.dist(v["pos"], b["centre"]) <= b["radius"] * 1.0001
                   for v in bspconv.decode_batch(sec, b))

    # garbage uv (retail has a few |u| > 1e6) clamp instead of wrecking the whole material
    wild = bspconv.Material(1, 0, [(0, 0, 0), (1, 0, 0), (0, 1, 0)],
                            [(0.0, 0.0), (2.0e6, 1.0), (float("nan"), 0.5)], [(0.0, 0.0)] * 3, [(0, 1, 2)])
    assert bspconv.uv_shift(wild.uv) == 14 or bspconv.uv_shift(wild.uv) >= 12
    bspconv.parse_section(bspconv.build_section([wild], (0, 0, 0, 1, 1, 0)))

    # corruption is reported, never silently accepted
    for bad in (blob[:50], b"XXXX" + blob[4:], blob[:-16]):
        try:
            bspconv.parse_section(bad)
            raise SystemExit("bad section accepted")
        except bspconv.ConvertError:
            pass
    bad_block = bytearray(block)
    struct.pack_into("<3H", bad_block, bsp.HEADER_SIZE + 0x27C, 0, 1, NV)     # index == vertex count
    try:
        bspconv.read_materials(bytes(bad_block), loc)
        raise SystemExit("out-of-range index accepted")
    except bspconv.ConvertError:
        pass

    pack = hp2.build("test", [("RES", b"res"), ("BSP0", blob)])
    assert bspconv.parse_section(hp2.parse(pack).read("BSP0")).triangles == 40
    if os.environ.get("BSP_FIXTURE"):
        Path(os.environ["BSP_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
