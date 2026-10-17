"""Model conversion on a synthetic Xbox tag region (no game data needed): vertex / strip decoding, the
quaternion convention, batching limits, the MODL section round trip and corruption handling, object
shader resolution and OTEX, then an optional check of a real map (assets/decompressed/a10.map, skipped
when it is missing).
Set MODEL_FIXTURE=<path> to also write the .hp2 the C reader test (engine/tests/test_model.c) opens; that
test hard-codes the numbers of the world built by `build_world()` below."""
import math
import os
import random
import struct
import sys
import tempfile
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1])]
from halo2ps2 import hp2, modelconv as mc, objtex, tagindex, texconv as tc  # noqa: E402

BASE = tagindex.XBOX_TAG_BASE
ROOT = Path(__file__).resolve().parents[2]


def close(a, b, tol=1e-6) -> bool:
    return len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


def fails(fn, exc=mc.ConvertError) -> bool:
    try:
        fn()
    except exc:
        return True
    return False


# ---- synthetic Xbox model tags ---------------------------------------------------------------------
def pack_normal(n) -> int:
    x, y, z = (round(n[0] * 1023) & 0x7FF, round(n[1] * 1023) & 0x7FF, round(n[2] * 511) & 0x3FF)
    return x | y << 11 | z << 22


def unit(v):
    ln = math.sqrt(sum(c * c for c in v))
    return tuple(c / ln for c in v)


class V:
    """A source vertex (floating point)."""
    def __init__(self, pos, nrm, uv, n0, n1=-1, w0=32767):
        self.pos, self.nrm, self.uv, self.n0, self.n1, self.w0 = pos, unit(nrm), uv, n0, n1, w0

    def pack(self, uvscale) -> bytes:
        return struct.pack("<3fIII2hBBh", *self.pos, pack_normal(self.nrm), 0, 0,
                           round(self.uv[0] / uvscale[0] * 32767), round(self.uv[1] / uvscale[1] * 32767),
                           self.n0 * 3, 0xFD if self.n1 < 0 else self.n1 * 3, self.w0)


class PartSpec:
    def __init__(self, shader: int, verts: list[V], strip: list[int]):
        self.shader, self.verts, self.strip = shader, verts, strip


class ModelSpec:
    def __init__(self, path, nodes, markers, regions, geometries, shaders, uvscale=(1.0, 1.0), cutoffs=(0, 9, 18, 35, 70)):
        self.path, self.nodes, self.markers, self.regions = path, nodes, markers, regions
        self.geometries, self.shaders, self.uvscale, self.cutoffs = geometries, shaders, uvscale, cutoffs
        # nodes: (name, sibling, child, parent, translation, quaternion x y z w AS STORED IN THE TAG)
        # markers: (name, [(region, perm_in_region, node, translation, quaternion)])
        # regions: (name, [(perm name, flags, [s16 x5 geometry, super low first])])


def tag_bytes(spec: ModelSpec, addr: int, vtx_ptrs: list[int], idx_ptrs: list[int], rec_vtx: list[int],
              rec_idx: list[int], shader_ids: list[int], part_base: int) -> bytes:
    """The model tag block; every pointer is absolute. Parts are numbered globally from `part_base`."""
    buf = bytearray(mc.T_HEADER)

    def alloc(data: bytes) -> int:
        buf.extend(bytes(-len(buf) % 4))
        a = addr + len(buf)
        buf.extend(data)
        return a

    def refl(off: int, count: int, a: int) -> None:
        struct.pack_into("<II", buf, off, count, a if count else 0)
    struct.pack_into("<If5f5H", buf, 0, 0, 0x12345678, *spec.cutoffs, 0, 0, 0, 0, 0)
    struct.pack_into("<2f", buf, mc.T_UV_SCALE, *spec.uvscale)
    nodes = bytearray()
    for name, sib, child, parent, t, q in spec.nodes:
        n = bytearray(mc.T_NODE)
        n[:len(name)] = name.encode()
        struct.pack_into("<3h", n, 0x20, sib, child, parent)
        struct.pack_into("<3f4ff", n, 0x28, *t, *q, math.dist(t, (0, 0, 0)))
        struct.pack_into("<f", n, 0x68, 1.0)
        nodes += n
    refl(mc.T_NODES, len(spec.nodes), alloc(bytes(nodes)))
    groups = bytearray()
    for name, insts in spec.markers:
        ib = b"".join(struct.pack("<4B3f4f", r, p, nd, 0, *t, *q) for r, p, nd, t, q in insts)
        g = bytearray(mc.T_MARKER_GROUP)
        g[:len(name)] = name.encode()
        struct.pack_into("<II", g, 0x34, len(insts), alloc(ib))
        groups += g
    refl(mc.T_MARKERS, len(spec.markers), alloc(bytes(groups)))
    regions = bytearray()
    for name, perms in spec.regions:
        pb = bytearray()
        for pname, flags, lods in perms:
            p = bytearray(mc.T_PERM)
            p[:len(pname)] = pname.encode()
            struct.pack_into("<I", p, 0x20, flags)
            struct.pack_into("<5h", p, 0x40, *lods)
            pb += p
        r = bytearray(mc.T_REGION)
        r[:len(name)] = name.encode()
        struct.pack_into("<II", r, 0x40, len(perms), alloc(bytes(pb)))
        regions += r
    refl(mc.T_REGIONS, len(spec.regions), alloc(bytes(regions)))
    geoms = bytearray()
    k = part_base
    for parts in spec.geometries:
        pb = bytearray()
        for ps in parts:
            p = bytearray(mc.T_PART)
            struct.pack_into("<IhbbIIf", p, 0, 0, ps.shader, -1, -1, 0, 0, 0)
            struct.pack_into("<I", p, 0x44, 1)
            struct.pack_into("<IIII", p, 0x48, len(ps.strip) - 2, idx_ptrs[k], rec_idx[k], 5)
            struct.pack_into("<I", p, 0x58, len(ps.verts))
            struct.pack_into("<II", p, 0x60, 0x02000000 + k * 0x1000, rec_vtx[k])
            pb += p
            k += 1
        g = bytearray(mc.T_GEOM)
        struct.pack_into("<II", g, 0x24, len(parts), alloc(bytes(pb)))
        geoms += g
    refl(mc.T_GEOMS, len(spec.geometries), alloc(bytes(geoms)))
    sh = b"".join(struct.pack("<4sIII", b"osos", 0x80000000, 0, sid) + bytes(16) for sid in shader_ids)
    refl(mc.T_SHADERS, len(shader_ids), alloc(sh))
    return bytes(buf)


def synthetic_map(tmp: Path, specs: list[ModelSpec]) -> tuple[tc.MapData, dict]:
    """A tag region with `specs` as model tags, three soso shaders (opaque, alpha-tested decal, untextured meter),
    two bitmaps and the vertex / index regions the way the Xbox compiler lays them out."""
    all_parts = [ps for s in specs for g in s.geometries for ps in g]
    nparts = len(all_parts)
    names = [("scnr", "levels\\t\\t", 0x600), ("bitm", "levels\\t\\bitmaps\\plain", 0x6C + 48),
             ("bitm", "levels\\t\\bitmaps\\holes", 0x6C + 48),
             ("soso", "levels\\t\\shaders\\skin", 0x100), ("soso", "levels\\t\\shaders\\decal", 0x100),
             ("soso", "levels\\t\\shaders\\nobase", 0x100), ("soso", "levels\\t\\shaders\\opaqueflag", 0x100),
             ("smet", "levels\\t\\shaders\\meter", 0x100)]
    shader_names = ["skin", "decal", "nobase", "opaqueflag", "meter"]
    sizes = [tag_bytes(s, 0, [0] * nparts, [0] * nparts, [0] * nparts, [0] * nparts, [0] * len(s.shaders), 0) for s in specs]
    tags = names + [("mode", s.path, len(b)) for s, b in zip(specs, sizes)]
    vtx_len = 12 * nparts + sum(32 * len(p.verts) for p in all_parts)
    idx_len = 12 * nparts + sum((2 * len(p.strip) + 3) // 4 * 4 for p in all_parts)
    region0 = tagindex.TagIndex.build(tags, model_parts=nparts, vertex_bytes=bytes(vtx_len), index_bytes=bytes(idx_len))
    idx0 = tagindex.TagIndex.parse_region(region0, 0)
    va, ia = idx0.header.model_vertex_address, idx0.header.model_index_address
    # vertex / index data in REVERSE part order after the record arrays (as the Xbox compiler does)
    vtx, idx = bytearray(12 * nparts), bytearray(12 * nparts)
    vptr, iptr, rec_v, rec_i = [0] * nparts, [0] * nparts, [0] * nparts, [0] * nparts
    cur = {}
    k = 0
    for s in specs:
        for g in s.geometries:
            for ps in g:
                cur[k] = (s, ps)
                k += 1
    for k in reversed(range(nparts)):
        s, ps = cur[k]
        vptr[k] = va + len(vtx)
        vtx += b"".join(v.pack(s.uvscale) for v in ps.verts)
        iptr[k] = ia + len(idx)
        raw = struct.pack(f"<{len(ps.strip)}H", *ps.strip)
        idx += raw + bytes(-len(raw) % 4)
    for k in range(nparts):
        rec_v[k] = va + 12 * (nparts - 1 - k)
        rec_i[k] = ia + 12 * (nparts - 1 - k)
        struct.pack_into("<III", vtx, 12 * (nparts - 1 - k), 0xDEAD0000 + k, vptr[k], 0)
        struct.pack_into("<III", idx, 12 * (nparts - 1 - k), 0xBEEF0000 + k, iptr[k], 0)
    region = bytearray(tagindex.TagIndex.build(tags, model_parts=nparts, vertex_bytes=bytes(vtx), index_bytes=bytes(idx)))
    idx1 = tagindex.TagIndex.parse_region(bytes(region), 0)
    assert idx1.header.model_vertex_address == va and idx1.header.model_index_address == ia
    ent = {e.path.split("\\")[-1]: e for e in idx1.entries if e.group != "mode"}
    modes = [e for e in idx1.entries if e.group == "mode"]
    sid = {n: ent[n].tag_id for n in shader_names}
    part_base = 0
    for s, e in zip(specs, modes):
        ids = [sid[n] for n in s.shaders]
        blob = tag_bytes(s, e.data_address, vptr, iptr, rec_v, rec_i, ids, part_base)
        assert len(blob) == len(sizes[specs.index(s)])
        o = e.data_address - BASE
        region[o:o + len(blob)] = blob
        part_base += sum(len(g) for g in s.geometries)
    off = lambda name: ent[name].data_address - BASE  # noqa: E731

    def ref(at: int, name: str | None) -> None:
        struct.pack_into("<4sIII", region, at, b"mtib", 0x80000000, 0, ent[name].tag_id if name else 0xFFFFFFFF)
    # bitmaps: "plain" is a 64x64 dxt1 (red, mip 1 blue) at file offset 1000; "holes" a 64x64 dxt3 with half the texels
    # transparent at file offset 6000
    for name, fmt, mips, poff, psize in (("plain", 14, 6, 1000, 2816), ("holes", 15, 0, 6000, 4096)):
        b = off(name)
        struct.pack_into("<II", region, b + 0x60, 1, ent[name].data_address + 0x6C)
        struct.pack_into("<4s6HH", region, b + 0x6C, b"mtib", 64, 64, 1, 0, fmt, 0x83, 0)
        struct.pack_into("<H", region, b + 0x6C + 0x14, mips)
        struct.pack_into("<II", region, b + 0x6C + 0x18, poff, psize)
    for name, flags, base in (("skin", 0, "plain"), ("decal", 8, "holes"), ("nobase", 2, None), ("opaqueflag", 4, "holes")):
        o = off(name)
        struct.pack_into("<H", region, o + 0x28, flags)
        struct.pack_into("<2f", region, o + 0x9C, 1.0, 1.0)
        ref(o + 0xA4, base)
    holes = struct.pack("<8B", *[0xA5] * 8) + struct.pack("<HHI", 0x07E0, 0x07E0, 0)
    mapfile = tmp / "t.map"
    red = struct.pack("<HHI", 0xF800, 0xF800, 0)
    data = bytearray(bytes(1000) + red * 256 + struct.pack("<HHI", 0x001F, 0x001F, 0) * 64 + bytes(2184))
    data += bytes(6000 + 4096 - len(data))
    data[6000:6000 + 4096] = holes * 256
    mapfile.write_bytes(bytes(data))
    m = tc.MapData(mapfile, None, idx1, bytes(region), {e.tag_id: e for e in idx1.entries})  # type: ignore[arg-type]
    return m, {"shaders": sid, "modes": [e.tag_id for e in modes]}


# ---- the world: three models ------------------------------------------------------------------------------
def conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def grid_strip(cols: int, nodes_for, z0=0.0, urange=(-1.0, 2.0)) -> PartSpec:
    """A 2 x cols ribbon in the x-z plane facing -y, as one strip (2 * cols vertices, 2 * cols - 2 triangles)."""
    verts = []
    for c in range(cols):
        for r in range(2):
            n0, n1, w = nodes_for(c, r)
            verts.append(V((c * 0.5, 0.0, z0 + r * 0.5), (0.0, -1.0, 0.0), (urange[0] + c / (cols - 1) * (urange[1] - urange[0]), r * 1.5 - 0.75), n0, n1, w))
    strip = []
    for c in range(cols):
        strip += [2 * c, 2 * c + 1]
    return PartSpec(0, verts, strip)


def quad(shader: int, node: int, z: float) -> PartSpec:
    vs = [V((x, 0.0, z + y), (0.0, 0.0, 1.0), (x, y), node) for x, y in ((0, 0), (1, 0), (0, 1), (1, 1))]
    return PartSpec(shader, vs, [0, 1, 2, 3])


def stitched_strip() -> PartSpec:
    """Two strips joined by degenerate stitches (the 3,3,4,4 pattern of the retail data), then a zero-area triangle."""
    vs = [V((i * 1.0, 0.0, (i % 2) * 1.0), (0, 0, 1), (i * 0.1, 0.0), 1) for i in range(8)]
    vs.append(V((9.0, 0.0, 9.0), (0, 0, 1), (0, 0), 1))
    vs.append(V((10.0, 0.0, 10.0), (0, 0, 1), (0, 0), 1))
    vs.append(V((11.0, 0.0, 11.0), (0, 0, 1), (0, 0), 1))        # 8, 9, 10 collinear: zero area
    return PartSpec(0, vs, [0, 1, 2, 3, 3, 4, 4, 5, 6, 7, 8, 9, 10])


def build_world() -> list[ModelSpec]:
    # model A: a small biped. Nodes: root -> spine -> (head, arm). Stored quaternions are Halo's (the conjugate of Hamilton).
    s2 = math.sqrt(0.5)
    nodes_a = [("root", -1, 1, -1, (0.0, 0.0, 1.0), (0.0, 0.0, 0.0, 1.0)),
               ("spine", -1, 2, 0, (0.0, 0.0, 0.5), (0.0, 0.0, s2, s2)),
               ("head", 3, -1, 1, (0.1, 0.0, 0.4), (0.0, 0.0, 0.0, 1.0)),
               ("arm", -1, -1, 1, (0.0, 0.3, 0.3), (s2, 0.0, 0.0, s2))]
    markers_a = [("primary trigger", [(0, 0, 3, (0.1, 0.2, 0.3), (0.0, 0.0, s2, s2))]),
                 ("head cam", [(1, 1, 2, (0.0, 0.0, 0.1), (0.0, 0.0, 0.0, 1.0)), (255, 255, 0, (1.0, 2.0, 3.0), (0.0, 0.0, 0.0, 1.0))])]
    # LOD arrays are stored super low first. perms: region 0 torso {base, damaged}, region 1 head {base, helmet}
    regions_a = [("torso", [("base", 0, [2, 2, 1, 1, 0]), ("damaged", 0, [3, 3, 3, 3, 3])]),
                 ("head", [("base", 0, [2, 2, 1, 1, 0]), ("helmet", 1, [4, 4, 4, 4, 4])])]
    skin = lambda c, r: (0, 1, 32767 - 4000 * c) if c < 6 else (1, 3, 20000)  # noqa: E731
    g0 = [grid_strip(20, skin), quad(1, 3, 2.0)]            # LOD 0: skinned ribbon (38 tris) + rigid decal quad (2 tris)
    g1 = [grid_strip(8, lambda c, r: (1, -1, 32767), urange=(-1.0, 1.9))]       # rigid on node 1, 14 tris
    g2 = [quad(0, 0, 0.0)]
    g3 = [stitched_strip()]                                 # damaged torso: 13-index strip, 6 valid triangles + stitches + a collinear one
    g4 = [quad(2, 2, 5.0)]                                  # helmet, untextured shader "nobase"
    a = ModelSpec("levels\\t\\models\\biped", nodes_a, markers_a, regions_a, [g0, g1, g2, g3, g4],
                  ["skin", "decal", "nobase"], uvscale=(2.0, 3.0), cutoffs=(0, 9, 18, 35, 70))
    # model B: 40 nodes, a ribbon whose vertices use many distinct nodes (palette limit of 24 per batch)
    nodes_b = [(f"bone{i}", i + 1 if i + 1 < 40 else -1, -1, -1, (float(i), 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)) for i in range(40)]
    wide = grid_strip(30, lambda c, r: ((c * 2 + r) % 40, (c * 7 + r * 3 + 1) % 40, 20000), urange=(-1.0, 0.9))
    b = ModelSpec("levels\\t\\models\\spider", nodes_b, [], [("body", [("base", 0, [0, 0, 0, 0, 0])])], [[wide]],
                  ["skin"], cutoffs=(0, 0, 0, 175, 300))
    # model C: a rigid prop, alpha-tested shader, one LOD geometry used by every LOD, no markers
    c = ModelSpec("levels\\t\\models\\prop", [("prop", -1, -1, -1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))], [],
                  [("__unnamed", [("__base", 0, [0, 0, 0, 0, 0])])], [[quad(1, 0, 0.0), quad(3, 0, 3.0)]],
                  ["skin", "decal", "nobase", "opaqueflag", "meter"], cutoffs=(5, 50, 120, 250, 400))
    return [a, b, c]


def independent_triangles(ps: PartSpec) -> list[tuple]:
    """(parity, vertex indices) of the valid triangles of a strip (an own implementation of the strip rules)."""
    out = []
    s = ps.strip
    for k in range(len(s) - 2):
        a, b, c = s[k:k + 3]
        if len({a, b, c}) < 3:
            continue
        p = [ps.verts[i].pos for i in (a, b, c)]
        u = [p[1][i] - p[0][i] for i in range(3)]
        w = [p[2][i] - p[0][i] for i in range(3)]
        cross = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        if cross == (0, 0, 0):
            continue
        out.append((k & 1, (a, b, c)))
    return out


# ---- tests --------------------------------------------------------------------------------------------------
def test_strip() -> None:
    pos = [(float(i), float(i % 2), 0.0) for i in range(6)]
    # a plain strip alternates the winding of every other triangle back to the strip's orientation
    assert mc.strip_to_triangles([0, 1, 2, 3, 4, 5], pos) == [(0, 1, 2), (1, 3, 2), (2, 3, 4), (3, 5, 4)]
    # a repeated index drops the triangles it touches but the parity of the strip position carries on
    assert mc.strip_to_triangles([0, 1, 2, 3, 3, 4, 4, 5], pos) == [(0, 1, 2), (1, 3, 2)]
    assert mc.strip_to_triangles([0, 1, 2, 2, 3, 4], pos) == [(0, 1, 2), (2, 4, 3)]
    # zero-area triangles (collinear points) are dropped too
    line = [(float(i), 0.0, 0.0) for i in range(4)]
    assert mc.strip_to_triangles([0, 1, 2, 3], line) == []
    assert mc.strip_to_triangles([0], pos) == [] and mc.strip_to_triangles([], pos) == []
    assert mc.conj((1.0, 2.0, 3.0, 4.0)) == (-1.0, -2.0, -3.0, 4.0)


def test_influences() -> None:
    p = mc.SrcPart(1, [(0, 0, 0)] * 4, [(0, 0, 1)] * 4, [(0, 0)] * 4, [2, 2, 3, 3], [-1, 5, 4, 3], [32767, 1000, 30000, 100], [])
    assert mc.influences(p, 0) == ((2, 32767), (2, 0))                      # rigid
    assert mc.influences(p, 1) == ((5, 31767), (2, 1000))                  # node 1 is the stronger: it comes first
    assert mc.influences(p, 2) == ((3, 30000), (4, 2767))
    assert mc.influences(p, 3) == ((3, 32767), (3, 0))                      # the same node twice is rigid
    assert mc.rigid_node_of(p, [0]) == 2 and mc.rigid_node_of(p, [0, 1]) == -1 and mc.rigid_node_of(p, [3]) == 3


def test_tag_reading() -> None:
    with tempfile.TemporaryDirectory() as d:
        specs = build_world()
        m, ids = synthetic_map(Path(d), specs)
        models = [mc.read_model(m, e) for e in m.index.entries if e.group == "mode"]
        a, b, c = models
        assert [md.path.split("\\")[-1] for md in models] == ["biped", "spider", "prop"]
        # nodes: links copied, rotation is the CONJUGATE of the stored quaternion
        assert [n.name for n in a.nodes] == ["root", "spine", "head", "arm"]
        assert (a.nodes[1].parent, a.nodes[1].first_child, a.nodes[1].next_sibling) == (0, 2, -1)
        assert (a.nodes[2].parent, a.nodes[2].next_sibling) == (1, 3)
        assert close(a.nodes[1].rotation, (0.0, 0.0, -math.sqrt(0.5), math.sqrt(0.5))) and a.nodes[1].translation == (0.0, 0.0, 0.5)
        # LOD order reversed to engine order (0 = super high), cutoffs too
        assert a.lod_cutoff == (70.0, 35.0, 18.0, 9.0, 0.0) and c.lod_cutoff == (400.0, 250.0, 120.0, 50.0, 5.0)
        assert [p.name for p in a.perms] == ["base", "damaged", "base", "helmet"] and [p.region for p in a.perms] == [0, 0, 1, 1]
        assert a.perms[0].geometry == (0, 1, 1, 2, 2) and a.perms[1].geometry == (3,) * 5 and a.perms[3].geometry == (4,) * 5
        assert sorted(a.geometries) == [0, 1, 2, 3, 4] and a.region_count == 2
        # markers flattened; region/perm (within region) become region + FLAT permutation; 255 = ANY
        assert [(k.name, k.region, k.permutation, k.node) for k in a.markers] == [
            ("primary trigger", 0, 0, 3), ("head cam", 1, 3, 2), ("head cam", mc.ANY, mc.ANY, 0)]
        assert close(a.markers[0].rotation, (0.0, 0.0, -math.sqrt(0.5), math.sqrt(0.5)))
        # geometry: counts and decoded values (uv scale 2 x 3 applied, normals unit, nodes divided by 3)
        p0 = a.geometries[0][0]
        assert len(p0.pos) == 40 and p0.node0[:2] == [0, 0] and p0.node1[0] == 1 and p0.weight0[0] == 32767
        assert p0.weight0[2 * 3] == 32767 - 12000 and (p0.node0[2 * 6], p0.node1[2 * 6], p0.weight0[2 * 6]) == (1, 3, 20000)
        assert abs(p0.uv[0][0] - (-1.0)) < 2e-4 and abs(p0.uv[-1][0] - 2.0) < 2e-4 and abs(p0.uv[1][1] - 0.75) < 4e-4
        assert all(abs(math.sqrt(sum(c * c for c in n)) - 1.0) < 0.004 for n in p0.nrm) and abs(p0.nrm[0][1] + 1.0) < 2e-3
        assert abs(p0.pos[3][0] - 0.5) < 1e-7
        assert len(p0.tris) == 38
        # the stitched strip: 13 indices = 11 strip triangles; stitches and the collinear triple are dropped
        want = independent_triangles(specs[0].geometries[3][0])
        st = a.geometries[3][0]
        assert len(st.tris) == len(want) == 6
        for (par, (x, y, z)), (u, v, w) in zip(want, st.tris):
            assert (u, v, w) == ((x, y, z) if par == 0 else (x, z, y))
        # shader ids resolve through the shader array; permutation variants share geometry
        assert a.geometries[0][1].shader_id == ids["shaders"]["decal"] and c.geometries[0][0].shader_id == ids["shaders"]["decal"]
        assert c.geometries[0][1].shader_id == ids["shaders"]["opaqueflag"]
        assert sorted(b.geometries) == [0] and len(b.geometries[0][0].pos) == 60
        # winding: strip order, (b - a) x (c - a) points against the normal (BSP convention) for the ribbon in the x-z plane
        t0 = p0.tris[0]
        P = [p0.pos[i] for i in t0]
        cross_y = (P[1][2] - P[0][2]) * (P[2][0] - P[0][0]) - (P[1][0] - P[0][0]) * (P[2][2] - P[0][2])
        assert cross_y * p0.nrm[0][1] < 0


def test_tag_errors() -> None:
    with tempfile.TemporaryDirectory() as d:
        specs = build_world()
        m, _ = synthetic_map(Path(d), specs)
        entry = [e for e in m.index.entries if e.group == "mode"][0]
        base = m.tag_off(entry)
        region = bytearray(m.region)

        def with_region(r: bytearray) -> tc.MapData:
            return tc.MapData(m.path, None, m.index, bytes(r), m.by_id)  # type: ignore[arg-type]
        r = bytearray(region)
        struct.pack_into("<II", r, base + mc.T_NODES, 3, 0x7FFFFFF0)                  # nodes outside the region
        assert fails(lambda: mc.read_model(with_region(r), entry))
        r = bytearray(region)
        struct.pack_into("<I", r, base + mc.T_NODES, 65)                              # too many nodes
        assert fails(lambda: mc.read_model(with_region(r), entry))
        geoms_at = struct.unpack_from("<I", region, base + mc.T_GEOMS + 4)[0] - BASE
        parts_at = struct.unpack_from("<I", region, geoms_at + 0x28)[0] - BASE
        r = bytearray(region)
        struct.pack_into("<I", r, parts_at + 0x58, 0xFFFF)                            # vertex count beyond the region
        assert fails(lambda: mc.read_model(with_region(r), entry))
        r = bytearray(region)
        struct.pack_into("<I", r, parts_at + 0x4C, m.index.header.model_vertex_address)    # index pointer in the vertex region
        assert fails(lambda: mc.read_model(with_region(r), entry))
        r = bytearray(region)
        struct.pack_into("<I", r, parts_at + 0x48, 5000)                              # strip longer than the data
        assert fails(lambda: mc.read_model(with_region(r), entry))
        r = bytearray(region)
        regions_at = struct.unpack_from("<I", region, base + mc.T_REGIONS + 4)[0] - BASE
        perms_at = struct.unpack_from("<I", region, regions_at + 0x44)[0] - BASE
        struct.pack_into("<h", r, perms_at + 0x40, 99)                                # geometry index out of range
        assert fails(lambda: mc.read_model(with_region(r), entry))
        r = bytearray(region)
        vrec = struct.unpack_from("<I", region, parts_at + 0x64)[0] - BASE
        vptr = struct.unpack_from("<I", region, vrec + 4)[0] - BASE
        r[vptr + 28] = 7                                                              # node index not a multiple of 3
        assert fails(lambda: mc.read_model(with_region(r), entry))
        r = bytearray(region)
        r[vptr + 28] = 3 * 9                                                          # node beyond the model's nodes
        assert fails(lambda: mc.read_model(with_region(r), entry))


def decoded_triangles(sec: mc.Section, mi: int, perm: int, lod: int) -> list[list[dict]]:
    md = sec.models[mi]
    first, count = sec.perms[md["first_range"] + perm]["ranges"][lod]
    out = []
    for k in range(first, first + count):
        part = sec.parts[k]
        for bi in range(part["first_batch"], part["first_batch"] + part["batch_count"]):
            vs = mc.decode_batch(sec, sec.batches[bi], part)
            out += [vs[i:i + 3] for i in range(0, len(vs), 3)]
    return out


def test_bake_and_section() -> mc.Section:
    with tempfile.TemporaryDirectory() as d:
        specs = build_world()
        m, ids = synthetic_map(Path(d), specs)
        res = mc.convert_map(m)
    sec = mc.parse_section(res.modl)
    assert [md["tag_id"] for md in sec.models] == sorted(ids["modes"]) and len(sec.models) == 3
    ma, mb, mcc = (next(x for x in sec.models if x["tag_id"] == t) for t in ids["modes"])
    assert (ma["node_count"], ma["marker_count"], ma["region_count"], ma["permutation_count"]) == (4, 3, 2, 4)
    assert mb["node_count"] == 40 and mcc["node_count"] == 1 and mb["marker_count"] == 0
    assert ma["lod_cutoff"] == (70.0, 35.0, 18.0, 9.0, 0.0)
    # every batch obeys the limits, palette slots point at the model's nodes, whole thing decodes to the source triangles
    for bt in sec.batches:
        assert 1 <= bt["triangles"] <= 24 and 1 <= len(bt["palette"]) <= 24
    assert any(len(bt["palette"]) > 12 for bt in sec.batches), "the 40-node ribbon should need big palettes"
    # model B: 58 triangles; batches stop at 24 triangles or 24 nodes
    pb = sec.parts[mb["first_part"]]
    batches = sec.batches[pb["first_batch"]:pb["first_batch"] + pb["batch_count"]]
    assert pb["triangles"] == 58 and sum(b["triangles"] for b in batches) == 58 and pb["rigid_node"] == -1
    assert len(batches) >= 3 and all(b["triangles"] < 24 or len(b["palette"]) <= 24 for b in batches)
    assert any(b["triangles"] < 24 for b in batches[:-1]), "a palette-limited batch closes early"
    # A: part kinds, flags (decal soso -> blended; untextured -> none), uv shifts, rigid nodes
    pa = sec.parts[ma["first_part"]:ma["first_part"] + ma["part_count"]]
    assert [p["triangles"] for p in pa] == [38, 2, 14, 2, len(independent_triangles(specs[0].geometries[3][0])), 2]
    by_geom = {}
    for pm in sec.perms[ma["first_range"]:ma["first_range"] + 4]:
        for lod, (first, count) in enumerate(pm["ranges"]):
            by_geom[(pm["name"], pm["region"], lod)] = (first, count)
    f0, n0 = by_geom[("base", 0, 0)]
    assert n0 == 2 and sec.parts[f0]["triangles"] == 38 and sec.parts[f0]["rigid_node"] == -1
    assert sec.parts[f0 + 1]["triangles"] == 2 and sec.parts[f0 + 1]["rigid_node"] == 3
    assert sec.parts[f0 + 1]["flags"] == mc.PF_BLEND and sec.parts[f0]["flags"] == 0
    f1, n1 = by_geom[("base", 0, 1)]
    assert by_geom[("base", 0, 2)] == (f1, n1) and by_geom[("base", 0, 3)] == by_geom[("base", 0, 4)]   # shared geometry shares parts
    assert sec.parts[f1]["triangles"] == 14 and sec.parts[f1]["rigid_node"] == 1
    fh, nh = by_geom[("helmet", 1, 0)]
    assert nh == 1 and sec.parts[fh]["flags"] == 0 and sec.parts[fh]["rigid_node"] == 2
    fd, nd = by_geom[("damaged", 0, 0)]
    assert sec.parts[fd]["triangles"] == len(independent_triangles(specs[0].geometries[3][0]))
    assert sec.parts[f0]["uv_shift"] in (12, 13) and sec.parts[f1]["uv_shift"] >= 12
    pc = sec.parts[mcc["first_part"]:mcc["first_part"] + 2]
    assert [p["rigid_node"] for p in pc] == [0, 0] and pc[0]["flags"] == mc.PF_BLEND and pc[1]["flags"] == 0   # decal soso / opaque flag soso
    # triangles of LOD 0 (one permutation per region): 38 + 2 (torso base) + 2 (head base... = geometry 0 again? no: head base is LOD0 geometry 0)
    assert ma["triangles_lod0"] == sum(sec.parts[k]["triangles"] for k in range(f0, f0 + n0)) * 2
    # round trip of geometry: decode batches and compare with the source triangles within the quantisation
    ga = specs[0].geometries[0][0]
    want = []
    for par, (x, y, z) in independent_triangles(ga):
        tri = (x, y, z) if par == 0 else (x, z, y)
        want.append(tuple(ga.verts[i].pos for i in tri))
    got = [tuple(v["pos"] for v in t) for t in decoded_triangles(sec, sec.models.index(ma), 0, 0)[:38]]
    assert len(got) == 38
    for g, w in zip(got, want):
        for gp, wp in zip(g, w):
            assert math.dist(gp, wp) < 2e-4, (gp, wp)
    verts = [v for t in decoded_triangles(sec, sec.models.index(ma), 0, 0)[:38] for v in t]
    src_v = [ga.verts[i] for par, (x, y, z) in independent_triangles(ga) for i in ((x, y, z) if par == 0 else (x, z, y))]
    for v, sv in zip(verts, src_v):
        assert abs(v["uv"][0] - sv.uv[0]) <= 2.0 ** -sec.parts[f0]["uv_shift"] and abs(v["uv"][1] - sv.uv[1]) <= 2.0 ** -sec.parts[f0]["uv_shift"]
        assert math.dist(v["normal"], sv.nrm) < 0.02
        n0_, n1_ = v["nodes"]
        if sv.n1 < 0 or sv.w0 >= 32767:
            assert n0_ == n1_ == sv.n0 and v["weight"] == 1.0
        else:
            assert {n0_, n1_} == {sv.n0, sv.n1}
            w_first = sv.w0 / 32767.0 if n0_ == sv.n0 else 1.0 - sv.w0 / 32767.0
            assert abs(v["weight"] - w_first) < 1e-4 and v["weight"] >= 0.5 - 1e-4
    # a triangle never mixes batches: every triangle's three vertices carry slots of the same palette (decoded above)
    return sec


def test_section_corruption() -> None:
    with tempfile.TemporaryDirectory() as d:
        m, _ = synthetic_map(Path(d), build_world())
        blob = mc.convert_map(m).modl
    mc.parse_section(blob)
    rng = random.Random(7)

    def flip(i: int, v: int, n: int = 1) -> bytes:
        return blob[:i] + int(v).to_bytes(n, "little") + blob[i + n:]
    # truncation at every length is rejected
    for n in list(range(0, len(blob), 7)) + [len(blob) - 1]:
        assert fails(lambda n=n: mc.parse_section(blob[:n])), n
    assert fails(lambda: mc.parse_section(blob + bytes(16)))
    assert fails(lambda: mc.parse_section(b"NOPE" + blob[4:]))
    assert fails(lambda: mc.parse_section(flip(4, 2, 2)))
    bad = 0
    for i in list(range(0, mc.HEADER_SIZE)) + sorted(rng.sample(range(mc.HEADER_SIZE, len(blob)), 2500)):
        for v in (blob[i] ^ 0xFF, 0, 0x7F):
            b2 = flip(i, v & 255)
            if b2 == blob:
                continue
            try:
                mc.parse_section(b2)
            except mc.ConvertError:
                bad += 1
            except Exception as exc:                                  # a crash is a bug
                raise AssertionError(f"byte {i}: {type(exc).__name__}: {exc}")
    assert bad > 600, bad
    # table directory entries
    for t in range(len(mc.TABLES)):
        for field, val in ((0, 17), (0, 0xFFFFFFF0), (4, 0xFFFFFFF), (4, 0xFFFFFFFF)):
            assert fails(lambda t=t, f=field, v=val: mc.parse_section(flip(mc.DIR_AT + 8 * t + f, v, 4))), (t, field, val)


def test_shaders_and_otex() -> None:
    with tempfile.TemporaryDirectory() as d:
        m, ids = synthetic_map(Path(d), build_world())
        sh = ids["shaders"]
        skin = objtex.resolve_object_shader(m, sh["skin"])
        assert skin.group == "soso" and skin.bitmap_id is not None and skin.alpha_test and skin.blend == tc.BLEND_ALPHA_TEST
        # flags 0 means alpha-tested by default; the opaque map decides later (build_otex turns the binding opaque)
        decal = objtex.resolve_object_shader(m, sh["decal"])
        assert decal.blend == tc.BLEND_ALPHA and decal.flags & tc.BF_DECAL and tc.klass_cap_for(decal) == tc.CLASS_BLEND
        nobase = objtex.resolve_object_shader(m, sh["nobase"])
        assert nobase.bitmap_id is None and nobase.flags & tc.BF_UNTEXTURED and nobase.flags & tc.BF_TWO_SIDED and nobase.blend == tc.BLEND_OPAQUE
        opq = objtex.resolve_object_shader(m, sh["opaqueflag"])
        assert not opq.alpha_test and opq.blend == tc.BLEND_OPAQUE and tc.klass_cap_for(opq) == tc.CLASS_OPAQUE
        meter = objtex.resolve_object_shader(m, sh["meter"])
        assert meter.bitmap_id is None and meter.flags & tc.BF_UNTEXTURED and "unsupported" in meter.note
        assert objtex.resolve_object_shader(m, 0x12345678).flags & tc.BF_UNTEXTURED
        res = objtex.build_otex(m, list(sh.values()) + list(sh.values()))
        v = tc.parse_texs(res.blob)
        assert res.blob[:4] == b"TEXS" and [b["shader_id"] for b in v.bindings] == sorted(sh.values())
        bind = {b["shader_id"]: b for b in v.bindings}
        # "skin": opaque dxt1 under an alpha-tested soso -> the binding becomes opaque; "decal": dxt3 with holes keeps blend
        assert bind[sh["skin"]]["blend"] == tc.BLEND_OPAQUE and bind[sh["skin"]]["texture"] != tc.NO_TEXTURE
        assert bind[sh["decal"]]["blend"] == tc.BLEND_ALPHA and bind[sh["decal"]]["flags"] & tc.BF_DECAL
        assert v.textures[bind[sh["decal"]]["texture"]]["flags"] & tc.TF_BLEND
        assert bind[sh["nobase"]]["texture"] == tc.NO_TEXTURE and bind[sh["meter"]]["texture"] == tc.NO_TEXTURE
        assert res.untextured_shaders == 2 and len(res.textures) == 3          # plain/opaque, holes/blend, holes/opaque-cap
        assert mc.part_flags(res.shaders[sh["decal"]]) == mc.PF_BLEND and mc.part_flags(res.shaders[sh["skin"]]) == 0
        assert mc.part_flags(None) == 0 and mc.part_flags(res.shaders[sh["meter"]]) == 0
        # soso with the alpha-test flag clear and a map with holes becomes a punch-through texture
        region = bytearray(m.region)
        o = m.tag_off(m.by_id[sh["opaqueflag"]])
        struct.pack_into("<H", region, o + 0x28, 0)
        m2 = tc.MapData(m.path, None, m.index, bytes(region), m.by_id)  # type: ignore[arg-type]
        r2 = objtex.build_otex(m2, [sh["opaqueflag"]])
        t2 = tc.parse_texs(r2.blob)
        assert t2.bindings[0]["blend"] == tc.BLEND_ALPHA_TEST and t2.textures[0]["flags"] & tc.TF_PUNCH
        assert mc.part_flags(r2.shaders[sh["opaqueflag"]]) == mc.PF_ALPHA_TEST
        # budget: a tiny budget lowers the maximum side
        small = objtex.build_otex(m, list(sh.values()), budget=4096)
        assert small.max_dim < 256 and all(t.width <= small.max_dim for t in small.textures)
        assert objtex.target_size(512, 512, 256) == (256, 256) and objtex.target_size(512, 512, 64) == (64, 64)
        assert objtex.target_size(4, 4, 256) == (8, 8) and objtex.target_size(1024, 128, 128) == (128, 16)
        assert objtex.estimate_bytes([(256, 256)], 256) > objtex.estimate_bytes([(256, 256)], 64)


def test_pack(sec: mc.Section) -> None:
    with tempfile.TemporaryDirectory() as d:
        m, ids = synthetic_map(Path(d), build_world())
        r = mc.convert_map(m, [e.tag_id for e in m.index.entries if e.group == "mode"][:2])
        assert len(mc.parse_section(r.modl).models) == 2
        # unknown tag ids are reported, not fatal; an empty list gives empty, valid sections
        r0 = mc.convert_map(m, [0xDEADBEEF])
        assert r0.skipped == [0xDEADBEEF] and mc.parse_section(r0.modl).models == [] and tc.parse_texs(r0.otex).bindings == []
        full = mc.convert_map(m)
        pack = hp2.build("t", full.sections)
        pk = hp2.parse(pack)
        assert pk.read("MODL") == full.modl and pk.read("OTEX") == full.otex
        assert mc.parse_section(pk.read("MODL")).models == sec.models
        # permutation filter: first of every region plus the named variants; markers of dropped permutations go too
        biped = ids["modes"][0]
        for keep, names, nmark in (({"damaged"}, ["base", "damaged", "base"], 2), ({"helmet"}, ["base", "base", "helmet"], 3),
                                   (set(), ["base", "base"], 2), ({"nothing"}, ["base", "base"], 2)):
            rv = mc.convert_map(m, [biped], variants={biped: keep})
            sv = mc.parse_section(rv.modl)
            md = sv.models[0]
            assert [pm["name"] for pm in sv.perms[:md["permutation_count"]]] == names, (keep, names)
            assert md["marker_count"] == nmark and len(rv.models[0].geometries) == len({g for pm in rv.models[0].perms for g in pm.geometry})
            assert [mk["permutation"] for mk in sv.markers if mk["region"] == 1 and mk["permutation"] != mc.ANY] == \
                ([2] if "helmet" in keep else [])
            assert len(rv.modl) < len(mc.convert_map(m, [biped]).modl) or keep == {"damaged", "helmet"}
        assert mc.convert_map(m, [biped], variants={}).modl == mc.convert_map(m, [biped]).modl        # model not listed: unfiltered
        skip = mc.convert_map(m, skip_lod0=True)
        s2 = mc.parse_section(skip.modl)
        assert len(skip.modl) < len(full.modl) and s2.models[0]["lod_cutoff"][0] == s2.models[0]["lod_cutoff"][1]
        if os.environ.get("MODEL_FIXTURE"):
            Path(os.environ["MODEL_FIXTURE"]).write_bytes(pack)


# ---- optional: a real map -----------------------------------------------------------------------------------------------
def real_inverse_check(m: tc.MapData, entry, md: mc.ModelData) -> float:
    """Largest deviation between the chain of our default poses (Hamilton rotations from the converted, conjugated
    quaternions) inverted and the inverse bind pose the tag stores (3x3 matrix at +0x6C, translation at +0x90)."""
    r = m.region
    count, addr = struct.unpack_from("<II", r, m.tag_off(entry) + mc.T_NODES)
    base = addr - m.base

    def qmat(q):
        x, y, z, w = q
        return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    world = []
    worst = 0.0
    for i, n in enumerate(md.nodes):
        rl = qmat(n.rotation)
        if n.parent >= 0:
            rp, tp = world[n.parent]
            rw = [[sum(rp[a][k] * rl[k][b] for k in range(3)) for b in range(3)] for a in range(3)]
            tw = [sum(rp[a][k] * n.translation[k] for k in range(3)) + tp[a] for a in range(3)]
        else:
            rw, tw = rl, list(n.translation)
        world.append((rw, tw))
        stored = struct.unpack_from("<9f3f", r, base + i * mc.T_NODE + 0x6C)
        inv_rot = [[rw[b][a] for b in range(3)] for a in range(3)]
        inv_t = [-sum(inv_rot[a][k] * tw[k] for k in range(3)) for a in range(3)]
        err_r = min(max(abs(inv_rot[a][b] - stored[3 * a + b]) for a in range(3) for b in range(3)),
                    max(abs(inv_rot[a][b] - stored[3 * b + a]) for a in range(3) for b in range(3)))
        worst = max(worst, err_r, max(abs(inv_t[a] - stored[9 + a]) for a in range(3)))
    return worst


def test_real_map() -> None:
    path = ROOT / "assets" / "decompressed" / "a10.map"
    if not path.exists():
        print("skip: no a10.map for the real-data check")
        return
    m = tc.MapData.open(path)
    entries = [e for e in m.index.entries if e.group == "mode"]
    assert entries
    res = mc.convert_map(m)
    sec = mc.parse_section(res.modl)                        # the validating reader accepts the whole map
    assert len(sec.models) == len(entries) and sec.triangles > 10000
    tex = tc.parse_texs(res.otex)
    assert len(tex.bindings) == len(res.otex_result.shaders) and len(tex.textures) > 10
    assert res.otex_result.texture_bytes <= objtex.DEFAULT_BUDGET
    # the quaternion convention: the stored inverse bind pose equals the inverse of our default pose chain
    worst = 0.0
    for e, md in zip(sorted(entries, key=lambda e: e.tag_id), res.models):
        assert md.tag_id == e.tag_id
        worst = max(worst, real_inverse_check(m, e, md))
    assert worst < 2e-3, worst
    # every decoded vertex lies inside its part's sphere and normals are unit length
    for mi, md in enumerate(sec.models[:12]):
        for k in range(md["first_part"], md["first_part"] + md["part_count"]):
            part = sec.parts[k]
            for bi in range(part["first_batch"], min(part["first_batch"] + part["batch_count"], part["first_batch"] + 3)):
                for v in mc.decode_batch(sec, sec.batches[bi], part):
                    assert math.dist(v["pos"], part["centre"]) <= part["radius"] * 1.0001 + 1e-4
                    assert abs(math.sqrt(sum(c * c for c in v["normal"])) - 1.0) < 0.06 or v["normal"] == (0.0, 0.0, 0.0)
    # LOD order: LOD 0 has at least as many triangles as LOD 4 (3 of 121 a10 permutations are authored the other way round)
    perms = [sum(sec.parts[k]["triangles"] for k in range(first, first + count)) for md in sec.models
             for pm in sec.perms[md["first_range"]:md["first_range"] + md["permutation_count"]] for first, count in (pm["ranges"][0], pm["ranges"][4])]
    ok = sum(1 for i in range(0, len(perms), 2) if perms[i] >= perms[i + 1])
    assert ok >= 0.95 * len(perms) / 2, (ok, len(perms) // 2)
    two = mc.build_sections(path, [res.models[0].tag_id])
    assert [t for t, _ in two] == ["MODL", "OTEX"] and len(mc.parse_section(two[0][1]).models) == 1
    print(f"real map: {len(sec.models)} models, {len(sec.parts)} parts, {len(sec.batches)} batches, "
          f"MODL {len(res.modl) / 2**20:.2f} MiB, OTEX {len(res.otex) / 2**20:.2f} MiB, bind pose error {worst:.1e}")


if __name__ == "__main__":
    test_strip()
    test_influences()
    test_tag_reading()
    test_tag_errors()
    section = test_bake_and_section()
    test_section_corruption()
    test_shaders_and_otex()
    test_pack(section)
    test_real_map()
    print("test_modelconv: ok")
