"""BSP block location, summary and structure reading on a synthetic map region (no game data
needed), plus an optional check of every structure BSP of the real maps in assets/decompressed
(skipped when they are not there).

`build_world()` also serves tools/tests/test_bspconv.py: a complete synthetic Xbox BSP block with
two clusters, a portal, a three-node tree, two materials and a known surface -> cluster map."""
import math
import random
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import bsp, tagindex  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
LOAD = 0x81000000
LEAF = bsp.LEAF_FLAG
NONE = bsp.NO_CHILD

# ---- the synthetic world ----------------------------------------------------------------------
# Tree: x < -10 outside | -10 <= x < 0 leaf 0 = cluster 0 | 0 <= x < 10 leaf 1 = cluster 1 | x >= 10 outside
PLANES = [(1.0, 0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 10.0), (1.0, 0.0, 0.0, -10.0)]
NODES = [(0, 2, 1), (1, LEAF | 1, NONE), (2, NONE, LEAF | 0)]       # (plane, back, front)
NV = (40, 22)                                                       # vertices of the two materials
NTRI = (40, 20)
# surface -> clusters, the intended union of the leaf route and the subcluster route
LEAF_SURFACES = {0: list(range(0, 28)) + list(range(30, 34)) + list(range(50, 58)),
                 1: list(range(30, 34)) + list(range(40, 50))}
SUB_SURFACES = {0: [list(range(0, 20)), list(range(20, 34))], 1: [list(range(30, 40))]}
UNREACHABLE = [58, 59]
PORTAL_VERTS = [(0.0, -1.0, -1.0), (0.0, 1.0, -1.0), (0.0, 1.0, 1.0), (0.0, -1.0, 1.0)]    # CCW seen from +x


def expected_clusters() -> dict[int, set[int]]:
    out: dict[int, set[int]] = {}
    for c, ss in LEAF_SURFACES.items():
        for s in ss:
            out.setdefault(s, set()).add(c)
    for c, subs in SUB_SURFACES.items():
        for ss in subs:
            for s in ss:
                out.setdefault(s, set()).add(c)
    return out


def vertex(mi: int, i: int) -> tuple[float, float, float, float, float, float, float]:
    """x y z, texture uv, lightmap uv of vertex i of material mi."""
    if mi == 0:
        x, y = (i % 8) * 3.0 - 11.0, (i // 8) * 2.5 + 100.0
        return x, y, 0.5 * math.sin(i), (i % 8) / 4.0, (i // 8) / 2.0, (i % 8) / 8.0, (i // 8) / 5.0
    return i * 0.5 - 5.0, 50.0 + (i % 2) * 0.7, 0.25 * math.cos(i), i / 8.0, (i % 3) / 3.0, i / 22.0, 0.5


def tris_of(mi: int) -> list[tuple[int, int, int]]:
    n = NV[mi] - 2
    return [(k % n, k % n + 1, k % n + 2) for k in range(NTRI[mi])]


class Blk:
    def __init__(self) -> None:
        self.b = bytearray()

    def alloc(self, data: bytes, align: int = 4) -> int:
        while len(self.b) % align:
            self.b.append(0)
        addr = LOAD + len(self.b)
        self.b += data
        return addr


def build_world() -> tuple[bytes, bsp.BspLocation]:
    B = Blk()
    B.alloc(bytes(bsp.HEADER_SIZE))
    root_addr = B.alloc(bytes(0x27C), 16)
    root = root_addr - LOAD

    def put(off: int, count: int, addr: int) -> None:
        struct.pack_into("<II", B.b, root + off, count, addr)

    # collision bsp: nodes + planes (+ the leaf count in its third reflexive)
    nodes = B.alloc(b"".join(struct.pack("<3I", *n) for n in NODES))
    planes = B.alloc(b"".join(struct.pack("<4f", *p) for p in PLANES))
    cbsp_addr = B.alloc(bytes(96))
    cb = cbsp_addr - LOAD
    struct.pack_into("<II", B.b, cb, len(NODES), nodes)
    struct.pack_into("<II", B.b, cb + 12, len(PLANES), planes)
    struct.pack_into("<I", B.b, cb + 0x18, 2)
    put(0xB0, 1, cbsp_addr)

    # leaves, leaf surfaces
    leaf_surf, leaves = [], bytearray()
    for li in (0, 1):
        leaves += struct.pack("<3HHHHI", 0xFF00, 0xFF00, 0xFF00, 0, li, len(LEAF_SURFACES[li]), len(leaf_surf))
        leaf_surf += LEAF_SURFACES[li]
    put(0xE0, 2, B.alloc(bytes(leaves)))
    put(0xEC, len(leaf_surf), B.alloc(b"".join(struct.pack("<II", s, 0) for s in leaf_surf)))

    # surfaces + materials
    surf = bytearray()
    for mi in (0, 1):
        for t in tris_of(mi):
            surf += struct.pack("<3H", *t)
    put(0xF8, len(surf) // 6, B.alloc(bytes(surf)))
    mats = bytearray(2 * bsp.MATERIAL_SIZE)
    first = 0
    for mi in (0, 1):
        rend, lmv = bytearray(), bytearray()
        for i in range(NV[mi]):
            x, y, z, u, v, lu, lv = vertex(mi, i)
            rend += struct.pack("<3fIII2f", x, y, z, 0, 0, 0, u, v)
            lmv += struct.pack("<I2h", 0, round(lu * 32767), round(lv * 32767))
        blob = B.alloc(bytes(rend + lmv))
        o = mi * bsp.MATERIAL_SIZE
        struct.pack_into("<I", mats, o + 0x0C, 0xE1740042 + mi)
        struct.pack_into("<ii", mats, o + 0x14, first, NTRI[mi])
        struct.pack_into("<I", mats, o + 0xB4, NV[mi])
        struct.pack_into("<I", mats, o + 0xC8, NV[mi])
        struct.pack_into("<4I", mats, o + 0xEC, len(rend) + len(lmv), 0, 0, blob)
        first += NTRI[mi]
    mat_addr = B.alloc(bytes(mats))
    lm = bytearray(bsp.LIGHTMAP_SIZE)
    struct.pack_into("<HH", lm, 0, 3, 0)
    struct.pack_into("<II", lm, 0x14, 2, mat_addr)
    put(0x104, 1, B.alloc(bytes(lm)))

    # portal 0 (back = cluster 0, front = cluster 1) in plane 0, then the clusters
    verts = B.alloc(b"".join(struct.pack("<3f", *v) for v in PORTAL_VERTS))
    portal = bytearray(bsp.PORTAL_SIZE)
    struct.pack_into("<hhi3ffI", portal, 0, 0, 1, 0, 0.0, 0.0, 0.0, 1.5, 0)
    struct.pack_into("<II", portal, 0x34, len(PORTAL_VERTS), verts)
    put(0x154, 1, B.alloc(bytes(portal)))
    clusters = bytearray(2 * bsp.CLUSTER_SIZE)
    attrs = [(0, -1, 0, 1, -1, 0), (-1, -32767, 2, 0, 0, 1)]         # sky fog bg env weather mirrors
    for c in (0, 1):
        e = c * bsp.CLUSTER_SIZE
        sky, fog, bgs, env, wea, mir = attrs[c]
        struct.pack_into("<5h", clusters, e, sky, fog, bgs, env, wea)
        struct.pack_into("<I", clusters, e + 0x50, mir)
        subs = bytearray()
        for ss in SUB_SURFACES[c]:
            lst = B.alloc(struct.pack(f"<{len(ss)}I", *ss))
            subs += struct.pack("<6f", -10, 0, -1, 1, -1, 1) + struct.pack("<IIi", len(ss), lst, 0)
        struct.pack_into("<II", clusters, e + 0x34, len(SUB_SURFACES[c]), B.alloc(bytes(subs)))
        struct.pack_into("<II", clusters, e + 0x5C, 1, B.alloc(struct.pack("<h", 0)))
    put(0x134, 2, B.alloc(bytes(clusters)))
    put(0x178, 0, 0)
    struct.pack_into("<6f", B.b, root + 0xC8, -11.0, 10.0, 100.0, 117.5, -0.5, 0.5)
    struct.pack_into("<5I4s", B.b, 0, root_addr, 2, 0, 2, 0, bsp.SBSP_MAGIC)
    return bytes(B.b), bsp.BspLocation(0x800, len(B.b), LOAD, "levels\\t\\ta")


def expect_error(fn, *exc) -> None:
    try:
        fn()
    except exc or (bsp.BspError,):
        return
    raise SystemExit("corrupt input accepted")


def poke(block: bytes, off: int, fmt: str, *vals) -> bytes:
    b = bytearray(block)
    struct.pack_into(fmt, b, off, *vals)
    return bytes(b)


def check_structure() -> None:
    block, loc = build_world()
    root = struct.unpack_from("<I", block, 0)[0] - LOAD
    st = bsp.read_structure(loc, block)
    assert st.surface_count == 60 and len(st.clusters) == 2 and len(st.portals) == 1
    assert st.cluster_surfaces == expected_clusters()
    assert sorted(set(range(60)) - set(st.cluster_surfaces)) == UNREACHABLE
    assert st.leaves == [0, 1] and st.nodes == [(p, fr, bk) for p, bk, fr in NODES] and st.planes == PLANES
    p = st.portals[0]
    assert (p.back, p.front, p.plane, p.vertices) == (0, 1, PLANES[0], PORTAL_VERTS)
    assert st.clusters[1].mirrors == 1 and st.clusters[1].fog == -32767 and st.clusters[0].portals == [0]
    # point location: the reference walk the C reader mirrors
    for pt, want in (((-5, 0, 0), 0), ((5, 3, -2), 1), ((15, 0, 0), -1), ((-15, 0, 0), -1), ((0.0, 1, 1), 1),
                     ((-1e-3, 0, 0), 0), ((9.999, 0, 0), 1), ((10.0, 0, 0), -1)):
        assert bsp.find_cluster(st, pt) == want, (pt, want)

    # every table is range-checked: corrupt each one and expect BspError
    nodes_at = struct.unpack_from("<I", block, root + 0xB0 + 4)[0] - LOAD
    cb = nodes_at
    leaves_at = struct.unpack_from("<I", block, root + 0xE0 + 4)[0] - LOAD
    clusters_at = struct.unpack_from("<I", block, root + 0x134 + 4)[0] - LOAD
    portals_at = struct.unpack_from("<I", block, root + 0x154 + 4)[0] - LOAD
    node0 = struct.unpack_from("<I", block, cb + 4)[0] - LOAD
    sub_at = struct.unpack_from("<I", block, clusters_at + 0x34 + 4)[0] - LOAD
    bad = {
        "no collision bsp": poke(block, root + 0xB0, "<I", 0),
        "node plane": poke(block, node0, "<I", 3),
        "node child backwards": poke(block, node0 + 12 * 1 + 4, "<I", 1),
        "node child past the end": poke(block, node0 + 4, "<I", 3),
        "node leaf child": poke(block, node0 + 12 + 4, "<I", LEAF | 2),
        "leaf count mismatch": poke(block, struct.unpack_from("<I", block, root + 0xB0 + 4)[0] - LOAD + 0x18, "<I", 3),
        "leaf cluster": poke(block, leaves_at + 8, "<H", 2),
        "leaf surface range": poke(block, leaves_at + 0x0C, "<I", 1000),
        "leaf surface index": poke(block, struct.unpack_from("<I", block, root + 0xEC + 4)[0] - LOAD, "<I", 60),
        "subcluster surface": poke(block, struct.unpack_from("<I", block, sub_at + 0x1C)[0] - LOAD, "<I", 61),
        "portal plane": poke(block, portals_at + 4, "<i", 3),
        "portal cluster": poke(block, portals_at + 2, "<h", 2),
        "portal vertices outside": poke(block, portals_at + 0x38, "<I", 0x70000000),
        "cluster portal list": poke(block, struct.unpack_from("<I", block, clusters_at + 0x5C + 4)[0] - LOAD, "<h", 5),
        "cluster lists a portal that is not its own": poke(block, portals_at, "<hh", 1, 1),
        "clusters outside the block": poke(block, root + 0x134 + 4, "<I", LOAD + len(block) - 8),
        "root outside": poke(block, 0, "<I", LOAD + len(block)),
        "no signature": poke(block, 0x14, "<4s", b"abcd"),
    }
    for name, blob in bad.items():
        try:
            bsp.read_structure(loc, blob)
        except bsp.BspError:
            continue
        raise SystemExit(f"read_structure accepted: {name}")


def check_summary() -> None:
    block, loc = build_world()
    s = bsp.summarise(loc, block)
    assert not s.problems, s.problems
    assert s.counts["surfaces"] == s.triangles == 60 and s.counts["clusters"] == 2
    assert s.counts["cluster_portals"] == 1 and s.counts["leaves"] == 2 and s.counts["collision_bsp"] == 1
    assert (s.lightmap_materials, s.rendered_vertices, s.lightmap_vertices) == (2, 62, 62)
    # second material claims 20 surfaces from 50 -> beyond the 60 surfaces
    root = struct.unpack_from("<I", block, 0)[0] - LOAD
    lm = struct.unpack_from("<I", block, root + 0x104 + 4)[0] - LOAD
    mats = struct.unpack_from("<I", block, lm + 0x14 + 4)[0] - LOAD
    broken = poke(block, mats + bsp.MATERIAL_SIZE + 0x14, "<ii", 50, 20)
    assert any("exceeds" in p for p in bsp.summarise(loc, broken).problems)
    for b in (block[:12], bytes(len(block))):
        expect_error(lambda b=b: bsp.summarise(loc, b), bsp.BspError, struct.error)


def check_locate() -> None:
    block, _ = build_world()
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
    expect_error(lambda: bsp.locate(idx, bytes(region), 0x800), bsp.BspError)


# ---- the real maps (skipped without game data) -------------------------------------------------
def check_real_maps() -> None:
    maps = Path(__file__).resolve().parents[2] / "assets" / "decompressed"
    paths = sorted(maps.glob("*.map")) if maps.is_dir() else []
    if not paths:
        print("real maps: skipped (no assets/decompressed)")
        return
    from halo2ps2 import mapinfo
    rnd = random.Random(5)
    nbsp = unreached = surfaces = wrong = tested = 0
    for path in paths:
        hdr = mapinfo.read_header(path)
        idx = tagindex.TagIndex.read(path)
        with path.open("rb") as f:
            f.seek(hdr.tag_data_offset)
            region = f.read(hdr.tag_data_size)
            for loc in bsp.locate(idx, region, hdr.decompressed_size):
                f.seek(loc.file_offset)
                st = bsp.read_structure(loc, f.read(loc.size))      # raises on any out-of-range index
                nbsp += 1
                surfaces += st.surface_count
                unreached += st.surface_count - len(st.cluster_surfaces)
                # portal orientation: points either side of the polygon land in (front, back)
                for p in st.portals:
                    if len(p.vertices) < 3 or p.back == p.front:
                        continue
                    for _ in range(4):
                        w = [rnd.random() for _ in p.vertices]
                        tot = sum(w)
                        pt = [sum(wi * v[k] for wi, v in zip(w, p.vertices)) / tot for k in range(3)]
                        a = bsp.find_cluster(st, tuple(pt[k] + p.plane[k] * 0.03 for k in range(3)))
                        b = bsp.find_cluster(st, tuple(pt[k] - p.plane[k] * 0.03 for k in range(3)))
                        if a >= 0 and b >= 0 and a != b:
                            tested += 1
                            wrong += (a, b) == (p.back, p.front)       # sides swapped
    assert nbsp >= 60 or len(paths) < 11
    assert unreached <= 0.001 * surfaces + 8 * nbsp, (unreached, surfaces)
    assert wrong == 0, (wrong, tested)             # front is the +normal side, on every decisive sample
    print(f"real maps: {nbsp} BSPs, {surfaces} surfaces ({unreached} unreferenced), "
          f"{tested} decisive portal samples, {wrong} with swapped sides")


def main() -> None:
    check_summary()
    check_structure()
    check_locate()
    check_real_maps()
    print("ok")


if __name__ == "__main__":
    main()
