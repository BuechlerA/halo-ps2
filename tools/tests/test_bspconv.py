"""BSPS v2 conversion on a synthetic BSP block: read, group by cluster set, bake, round-trip, pack.
Set BSP_FIXTURE=<path> to also write the resulting .hp2 for the C reader test
(engine/tests/test_bsp.c, which hard-codes the numbers of this world)."""
import math
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import bsp, bspconv, hp2  # noqa: E402
import test_bsp as world  # noqa: E402


def tri_key(tri) -> tuple:
    return tuple(sorted(tuple(round(c, 3) for c in p) for p in tri))


def source_tris(mats: list[bspconv.Material], pred) -> list[tuple]:
    """Triangles (as vertex position triples) of the surfaces whose cluster set satisfies pred."""
    exp = world.expected_clusters()
    out = []
    for m in mats:
        for t, s in zip(m.tris, m.surfaces):
            if s in exp and pred(exp[s]):
                out.append(tuple(m.pos[vi] for vi in t))
    return out


def decoded_tris(sec: bspconv.Section, batch_ids) -> list[tuple]:
    out = []
    for bi in batch_ids:
        vs = bspconv.decode_batch(sec, sec.batches[bi])
        out += [tuple(v["pos"] for v in vs[i:i + 3]) for i in range(0, len(vs), 3)]
    return out


def assert_same_tris(got: list[tuple], want: list[tuple], tol: float) -> None:
    assert len(got) == len(want), (len(got), len(want))
    free = list(want)
    for g in got:
        best = min(range(len(free)), key=lambda i: min(
            sum(math.dist(a, b) for a, b in zip(g, perm))
            for perm in (free[i], free[i][1:] + free[i][:1], free[i][2:] + free[i][:2])))
        w = free.pop(best)
        d = min(sum(math.dist(a, b) for a, b in zip(g, perm))
                for perm in (w, w[1:] + w[:1], w[2:] + w[:2]))
        assert d <= tol * 3, (d, tol)


def check_world() -> bytes:
    block, loc = world.build_world()
    mats, bounds = bspconv.read_materials(block, loc)
    assert [len(m.tris) for m in mats] == [40, 20] and mats[0].shader_id == 0xE1740042
    assert mats[0].surfaces == list(range(40)) and mats[1].surfaces == list(range(40, 60))
    assert mats[0].lightmap_page == 3 and bounds == (-11.0, 100.0, -0.5, 10.0, 117.5, 0.5)

    st = bsp.read_structure(loc, block)
    blob = bspconv.build_section(mats, bounds, st)
    assert blob == bspconv.convert_block(block, loc)
    sec = bspconv.parse_section(blob)
    assert len(blob) % 16 == 0 and sec.size == len(blob)

    # 58 reachable surfaces, none duplicated: 4 cluster-set x material groups -> 6 batches of <= 24
    assert sec.triangles == 58 and sum(b["triangles"] for b in sec.batches) == 58
    assert [(b["material"], b["triangles"]) for b in sec.batches] == [(0, 24), (0, 6), (0, 4), (1, 8), (0, 6), (1, 10)]
    assert all(b["triangles"] <= bspconv.MAX_TRIS and b["vif_offset"] % 16 == 0 for b in sec.batches)
    assert [(m["batch_count"], m["triangles"]) for m in sec.materials] == [(4, 40), (2, 18)]
    c0, c1 = sec.clusters
    lists = [sec.cluster_batches[c["first_batch"]:c["first_batch"] + c["batch_count"]] for c in sec.clusters]
    assert lists == [[0, 1, 2, 3], [2, 4, 5]], lists       # per cluster, ordered by material; batch 2 is shared
    for lst in lists:
        mat_order = [sec.batches[b]["material"] for b in lst]
        assert mat_order == sorted(mat_order)
    assert (c0["triangles"], c1["triangles"]) == (42, 20)

    # every cluster draws exactly the surfaces that list it (decoded back from the VIF packets)
    for ci, lst in enumerate(lists):
        assert_same_tris(decoded_tris(sec, lst), source_tris(mats, lambda cs, ci=ci: ci in cs), 1e-3)

    # packet layout: 4 UNPACKs, TOPS-relative, padded with zero words to a quadword
    for b in sec.batches:
        raw = sec.vif[b["vif_offset"]:b["vif_offset"] + b["vif_qwords"] * 16]
        n = b["triangles"] * 3
        assert len(raw) == bspconv.packet_bytes(b["triangles"]) and len(raw) <= 93 * 16
        w = struct.unpack_from("<I", raw, 0)[0]
        assert w == (0x6C << 24 | 2 << 16 | 0x8000 | 0)
        assert struct.unpack_from("<4f", raw, 4)[:3] == b["centre"]
        assert abs(struct.unpack_from("<4f", raw, 4)[3] * 32767 - b["radius"]) <= b["radius"] * 1e-6
        assert struct.unpack_from("<4i", raw, 20) == (n, 0, 0, 0)
        o = 36
        for fmt, addr, esize in ((0x6D, 4, 8), (0x6D, 76, 8), (0x6E, 148, 4)):
            w = struct.unpack_from("<I", raw, o)[0]
            usn = 0x4000 if fmt == 0x6E else 0
            assert w == (fmt << 24 | n << 16 | 0x8000 | usn | addr), hex(w)
            o += 4 + n * esize
        assert raw[o:] == bytes(len(raw) - o)
    assert bspconv.VU_QWORDS == 220

    # precision: positions, uv, lightmap uv, neutral colour
    for b in sec.batches:
        vs = bspconv.decode_batch(sec, b)
        assert all(math.dist(v["pos"], b["centre"]) <= b["radius"] * 1.0001 for v in vs)
        assert all(v["colour"] == (0x80, 0x80, 0x80, 0x80) for v in vs)
        m = mats[b["material"]]
        for v in vs:                 # find the vertex it came from by position
            vi = min(range(len(m.pos)), key=lambda i: math.dist(m.pos[i], v["pos"]))
            assert math.dist(m.pos[vi], v["pos"]) <= b["radius"] / 65534 * 1.8 + 1e-6
            shift = sec.materials[b["material"]]["shift"]
            assert abs(v["uv"][0] - m.uv[vi][0]) <= 1 / (1 << shift)
            assert abs(v["lmuv"][0] - m.lmuv[vi][0]) < 1e-4 and abs(v["lmuv"][1] - m.lmuv[vi][1]) < 1e-4

    # clusters: bounds, flags, attributes
    for ci, c in enumerate(sec.clusters):
        for t in decoded_tris(sec, lists[ci]):
            for p in t:
                assert all(c["min"][k] - 1e-3 <= p[k] <= c["max"][k] + 1e-3 for k in range(3))
        for k in sec.cluster_portals[c["first_portal"]:c["first_portal"] + c["portal_count"]]:
            for v in sec.portal_vertices[sec.portals[k]["first_vertex"]:][:4]:
                assert all(c["min"][i] <= v[i] <= c["max"][i] for i in range(3))
    assert c0["flags"] == bspconv.CL_SKY and c0["fog"] == -1 and c0["sky"] == 0
    assert (c0["background_sound"], c0["sound_environment"], c0["weather"]) == (0, 1, -1)
    assert c1["flags"] == (bspconv.CL_FOG | bspconv.CL_FOG_BIT15 | bspconv.CL_WEATHER | bspconv.CL_MIRROR)
    assert (c1["fog"], c1["sky"], c1["weather"], c1["background_sound"]) == (1, -1, 0, 2)

    # portal: cluster_a = front (+normal side) = 1, cluster_b = back = 0; polygon preserved
    assert len(sec.portals) == 1 and sec.cluster_portals == [0, 0]
    p = sec.portals[0]
    assert (p["a"], p["b"], p["vertex_count"], p["plane"]) == (1, 0, 4, (1.0, 0.0, 0.0, 0.0))
    assert sec.portal_vertices == world.PORTAL_VERTS
    assert abs(p["radius"] - math.sqrt(2.0)) < 1e-6 and p["centre"] == (0.0, 0.0, 0.0)

    # point location on the packed tree
    for pt, want in (((-5, 0, 0), 0), ((5, 3, -2), 1), ((15, 0, 0), -1), ((-15, 0, 0), -1), ((10.0, 0, 0), -1)):
        assert bspconv.find_cluster(sec, pt) == want
    assert sec.nodes == [(p_, fr, bk) for p_, bk, fr in world.NODES] and sec.planes == world.PLANES
    return blob


def check_chunking_and_colour_hook() -> None:
    # one cluster, one material, 60 triangles on a line: 24 + 24 + 12, spatially sorted
    n = 62
    pos = [(float(i), float((i * 7919) % 13), 0.0) for i in range(n)]
    m = bspconv.Material(1, 0, pos, [(0.0, 0.0)] * n, [(0.0, 0.0)] * n, [(i, i + 1, i + 2) for i in range(60)])
    sec = bspconv.parse_section(bspconv.build_section([m], (0, 0, 0, 1, 1, 1)))
    assert [b["triangles"] for b in sec.batches] == [24, 24, 12] and len(sec.clusters) == 1
    assert sec.cluster_batches == [0, 1, 2] and sec.nodes == [] and sec.leaves == [0]
    assert bspconv.find_cluster(sec, (1e6, 0, 0)) == 0          # no tree: the single leaf
    # the colour hook feeds the 4th UNPACK
    orig = bspconv.vertex_colour
    try:
        bspconv.vertex_colour = lambda mat, vi: (vi % 256, 2, 3, 0xFF)
        sec = bspconv.parse_section(bspconv.build_section([m], (0, 0, 0, 1, 1, 1)))
        v = bspconv.decode_batch(sec, sec.batches[0])
        assert all(c["colour"][1:] == (2, 3, 0xFF) for c in v)
        assert len({c["colour"][0] for c in v}) > 1
    finally:
        bspconv.vertex_colour = orig
    assert orig(m, 3) == (0x80, 0x80, 0x80, 0x80)
    # garbage uv (retail has a few |u| > 1e6) clamp instead of wrecking the whole material
    wild = bspconv.Material(1, 0, [(0, 0, 0), (1, 0, 0), (0, 1, 0)],
                            [(0.0, 0.0), (2.0e6, 1.0), (float("nan"), 0.5)], [(0.0, 0.0)] * 3, [(0, 1, 2)])
    assert bspconv.uv_shift(wild.uv) >= 12
    bspconv.parse_section(bspconv.build_section([wild], (0, 0, 0, 1, 1, 0)))
    # a material nothing references produces no batches but keeps its slot
    sec = bspconv.parse_section(bspconv.build_section([m, bspconv.Material(2, 0, pos[:3], [(0, 0)] * 3, [(0, 0)] * 3,
                                                                           [(0, 1, 2)])], (0, 0, 0, 1, 1, 1)))
    assert len(sec.materials) == 2 and sec.materials[1]["triangles"] == 1


def check_portal_polygons() -> None:
    plane = (0.0, 0.0, 1.0, 0.0)
    square = [(0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (2.0, 2.0, 0.0), (0.0, 2.0, 0.0)]
    assert bspconv.convex_polygon(square, plane) == (square, False)
    collinear = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (2.0, 0.0, 0.0), (2.0, 2.0, 0.0), (0.0, 2.0, 0.0)]
    assert bspconv.convex_polygon(collinear, plane)[1] is False          # collinear vertices are not concave
    dent = [(0.0, 0.0, 0.0), (4.0, 0.0, 0.0), (4.0, 4.0, 0.0), (2.0, 1.0, 0.0), (0.0, 4.0, 0.0)]
    hull, changed = bspconv.convex_polygon(dent, plane)
    assert changed and sorted(hull) == sorted([dent[0], dent[1], dent[2], dent[4]])
    # the hull stays counter-clockwise about the normal, also for a flipped normal
    for pl, poly in ((plane, dent), ((0.0, 0.0, -1.0, 0.0), dent[::-1])):
        h, _ = bspconv.convex_polygon(poly, pl)
        area = sum(h[i][0] * h[(i + 1) % len(h)][1] - h[(i + 1) % len(h)][0] * h[i][1] for i in range(len(h)))
        assert (area > 0) == (pl[2] > 0)


def check_corruption(blob: bytes) -> None:
    def rejected(b: bytes) -> bool:
        try:
            bspconv.parse_section(b)
        except bspconv.ConvertError:
            return True
        return False

    for bad in (blob[:50], b"XXXX" + blob[4:], blob[:-16], blob + bytes(16), bytes(blob[:4]) + b"\x01\x00" + blob[6:]):
        assert rejected(bad)
    # version 1 sections are refused outright
    assert rejected(blob[:4] + struct.pack("<H", 1) + blob[6:])
    sec = bspconv.parse_section(blob)
    offs = {name: struct.unpack_from("<II", blob, bspconv.DIR_AT + 8 * i) for i, name in enumerate(bspconv.TABLES)}
    # every table: misaligned, out-of-range and oversized directory entries are rejected
    for i, name in enumerate(bspconv.TABLES):
        off, cnt = offs[name]
        at = bspconv.DIR_AT + 8 * i
        for entry in ((off + 4, cnt), (len(blob) + 16, cnt), (off, 0xFFFFFFFF), (off, len(blob)), (4, cnt)):
            b = bytearray(blob)
            struct.pack_into("<II", b, at, *entry)
            assert rejected(bytes(b)), (name, entry)
    # no byte flip of the tables (or of the VIF packets' command/pad words) may crash the parser
    for at in range(0, len(blob) - len(sec.vif)):
        for x in (0xFF, 0x01):
            b = bytearray(blob)
            b[at] ^= x
            try:
                bspconv.parse_section(bytes(b))
            except bspconv.ConvertError:
                pass
    # structurally targeted corruptions
    nodes_at, nn = offs["nodes"]
    cl_at, _ = offs["clusters"]
    cb_at, _ = offs["cluster_batches"]
    po_at, _ = offs["portals"]
    le_at, _ = offs["leaves"]
    ba_at, _ = offs["batches"]
    ma_at, _ = offs["materials"]

    def poke(off: int, fmt: str, *v) -> bytes:
        b = bytearray(blob)
        struct.pack_into(fmt, b, off, *v)
        return bytes(b)

    for what, bad in (
            ("node plane", poke(nodes_at, "<I", 99)), ("node child backwards", poke(nodes_at + 12 + 4, "<I", 1)),
            ("leaf child", poke(nodes_at + 12 + 4, "<I", world.LEAF | 7)),
            ("leaf cluster", poke(le_at, "<H", 5)), ("cluster batch ref", poke(cb_at, "<H", 6)),
            ("cluster list range", poke(cl_at + 0x24, "<I", 100)), ("portal cluster", poke(po_at, "<H", 9)),
            ("portal vertex range", poke(po_at + 4, "<H", 9)), ("batch triangles 25", poke(ba_at + 2, "<H", 25)),
            ("batch material", poke(ba_at, "<H", 9)), ("batch offset", poke(ba_at + 4, "<I", 8)),
            ("material counts", poke(ma_at + 8, "<H", 9))):
        assert rejected(bad), what


def check_real_maps() -> None:
    root = Path(__file__).resolve().parents[2] / "assets" / "decompressed"
    path = root / "a10.map"
    if not path.is_file():
        print("real map conversion: skipped (no assets/decompressed/a10.map)")
        return
    from halo2ps2 import mapinfo, tagindex
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    biggest = 0
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
        locs = bsp.locate(idx, region, hdr.decompressed_size)
        for i in (6, 7):                                   # the largest retail BSP by triangles, and a small one
            loc = locs[i]
            f.seek(loc.file_offset)
            block = f.read(loc.size)
            mats, bounds = bspconv.read_materials(block, loc)
            st = bsp.read_structure(loc, block)
            sec = bspconv.parse_section(bspconv.build_section(mats, bounds, st))
            reachable = sum(1 for m in mats for s in m.surfaces if s in st.cluster_surfaces)
            assert sec.triangles == reachable and len(sec.clusters) == len(st.clusters)
            assert len(sec.portals) == len(st.portals) and sec.size < bspconv.SIZE_BUDGET
            assert all(len(c["min"]) == 3 and all(c["min"][k] <= c["max"][k] for k in range(3)) for c in sec.clusters)
            for c in sec.clusters:                          # a cluster's batches carry its triangles
                assert c["triangles"] == sum(sec.batches[sec.cluster_batches[c["first_batch"] + k]]["triangles"]
                                             for k in range(c["batch_count"]))
            biggest = max(biggest, sec.size)
    print(f"real map conversion: ok ({biggest / 2**20:.2f} MiB raw for the largest checked BSP)")


def check_tags() -> None:
    assert [bspconv.section_tag(i) for i in (0, 9, 10, 12, 35)] == ["BSP0", "BSP9", "BSPA", "BSPC", "BSPZ"]
    for bad in (-1, 36):
        try:
            bspconv.section_tag(bad)
        except bspconv.ConvertError:
            continue
        raise SystemExit("bad BSP index accepted")
    hp2.build("t", [(bspconv.section_tag(i), b"x") for i in range(13)])


def main() -> None:
    check_tags()
    blob = check_world()
    check_chunking_and_colour_hook()
    check_portal_polygons()
    check_corruption(blob)
    check_real_maps()

    # a pack holding the world as BSP0 and a tree-less section as BSP1 (the C test reads both)
    m = bspconv.Material(1, 0, [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 0)] * 3, [(0, 0)] * 3, [(0, 1, 2)])
    flat = bspconv.build_section([m], (0, 0, 0, 1, 1, 0))
    pack = hp2.build("test", [("RES", b"res"), ("BSP0", blob), ("BSP1", flat)])
    got = hp2.parse(pack)
    assert bspconv.parse_section(got.read("BSP0")).triangles == 58
    assert bspconv.parse_section(got.read("BSP1")).triangles == 1
    if os.environ.get("BSP_FIXTURE"):
        Path(os.environ["BSP_FIXTURE"]).write_bytes(pack)
    print("ok")


if __name__ == "__main__":
    main()
