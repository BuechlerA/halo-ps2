#!/usr/bin/env python3
"""Convert Xbox structure BSP data into the PS2 `BSPn` section, format version 2
(docs/08-bsp-section.md): cluster/portal visibility data, the point-location tree and
per-cluster VU1 batches.

Usage:
    python3 -I tools/halo2ps2/bspconv.py assets/decompressed/a10.map -o assets/hp2/a10.hp2 [--bsp 0]
    python3 -I tools/halo2ps2/bspconv.py --info assets/hp2/a10.hp2
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bsp, hp2, mapinfo, tagindex  # noqa: E402

MAGIC = b"BSPS"
VERSION = 2
HEADER_SIZE = 144
MATERIAL_REC = 32
BATCH_REC = 32
CLUSTER_REC = 64
PORTAL_REC = 48
NODE_REC = 12
PLANE_REC = 16
MAX_TRIS = 24                  # 72 expanded vertices per VU1 batch
MAX_VERTS = MAX_TRIS * 3
# VU1 input buffer (qwords, TOPS-relative): constants, positions, texture/lightmap uv, colours
VU_HDR, VU_POS = 0, 4
VU_UV = VU_POS + MAX_VERTS
VU_COL = VU_UV + MAX_VERTS
VU_QWORDS = VU_COL + MAX_VERTS   # 220: every UNPACKed element occupies one qword in VU1 memory
VIF_NOP = 0
VIF_UNPACK_V4_32 = 0x6C        # 0x60 | (V4 << 2) | element size (0 = 32, 1 = 16, 2 = 8 bit)
VIF_UNPACK_V4_16 = 0x6D
VIF_UNPACK_V4_8 = 0x6E
TOPS_RELATIVE = 0x8000         # imm bit 15 (FLG)
UNSIGNED = 0x4000              # imm bit 14 (USN); clear = sign-extended
RENDERED_VERTEX, LIGHTMAP_VERTEX = 32, 8
NO_LIGHTMAP = 0xFFFF
NO_CLUSTER = 0xFFFF
NO_CHILD = bsp.NO_CHILD
LEAF_FLAG = bsp.LEAF_FLAG
SIZE_BUDGET = 7 * 2**20        # docs/02-specs.md: the largest BSP section stays under 7 MiB raw
# cluster flags
CL_SKY, CL_FOG, CL_WEATHER, CL_MIRROR, CL_FOG_BIT15 = 1, 2, 4, 8, 16
# directory order (offset, count) of the tables in the header
TABLES = ("materials", "batches", "vif", "clusters", "cluster_batches", "cluster_portals",
          "portals", "portal_vertices", "nodes", "planes", "leaves")
DIR_AT = 0x30
assert DIR_AT + 8 * len(TABLES) <= HEADER_SIZE and HEADER_SIZE % 16 == 0


class ConvertError(ValueError):
    pass


@dataclass
class Material:
    shader_id: int
    lightmap_page: int
    pos: list[tuple[float, float, float]]
    uv: list[tuple[float, float]]
    lmuv: list[tuple[float, float]]
    tris: list[tuple[int, int, int]]
    surfaces: list[int] = field(default_factory=list)   # BSP surface index of every triangle (empty: synthetic)


@dataclass
class Batch:
    material: int
    triangles: int
    vif: bytes
    centre: tuple[float, float, float]
    radius: float
    clusters: tuple[int, ...] = (0,)
    lo: tuple[float, float, float] = (0.0, 0.0, 0.0)     # exact vertex bounds of the batch
    hi: tuple[float, float, float] = (0.0, 0.0, 0.0)


@dataclass
class Section:
    bounds: tuple[float, ...]
    triangles: int
    materials: list[dict]
    batches: list[dict]
    clusters: list[dict]
    cluster_batches: list[int]
    cluster_portals: list[int]
    portals: list[dict]
    portal_vertices: list[tuple[float, float, float]]
    nodes: list[tuple[int, int, int]]       # (plane, front, back)
    planes: list[tuple[float, float, float, float]]
    leaves: list[int]
    vif: bytes
    size: int = 0


# ---- reading the Xbox BSP -----------------------------------------------------------
def read_materials(block: bytes, loc: bsp.BspLocation) -> tuple[list[Material], tuple[float, ...]]:
    A = loc.address
    ptr = struct.unpack_from("<I", block, 0)[0]
    root = ptr - A

    def refl(off: int, stride: int) -> bytes:
        count, addr = struct.unpack_from("<II", block, root + off)
        o = addr - A
        if count and (o < 0 or o + count * stride > len(block)):
            raise ConvertError(f"reflexive at +{off:#x} outside the block")
        return block[o:o + count * stride] if count else b""

    surfaces = refl(0xF8, 6)
    nsurf = len(surfaces) // 6
    bounds_raw = struct.unpack_from("<6f", block, root + 0xC8)
    bounds = (bounds_raw[0], bounds_raw[2], bounds_raw[4], bounds_raw[1], bounds_raw[3], bounds_raw[5])
    out: list[Material] = []
    lms = refl(0x104, bsp.LIGHTMAP_SIZE)
    for li in range(len(lms) // bsp.LIGHTMAP_SIZE):
        page = struct.unpack_from("<H", lms, li * bsp.LIGHTMAP_SIZE)[0]
        mcount, maddr = struct.unpack_from("<II", lms, li * bsp.LIGHTMAP_SIZE + 0x14)
        mo = maddr - A
        if mcount and (mo < 0 or mo + mcount * bsp.MATERIAL_SIZE > len(block)):
            raise ConvertError("lightmap materials outside the block")
        for m in range(mcount):
            mat = block[mo + m * bsp.MATERIAL_SIZE:mo + (m + 1) * bsp.MATERIAL_SIZE]
            shader_id = struct.unpack_from("<I", mat, 0x0C)[0]
            first, n = struct.unpack_from("<ii", mat, 0x14)
            rv, lv = struct.unpack_from("<I", mat, 0xB4)[0], struct.unpack_from("<I", mat, 0xC8)[0]
            if struct.unpack_from("<I", mat, 0xD8)[0]:
                raise ConvertError("uncompressed vertex data present (not seen on retail maps)")
            csize, _f, _o, cptr = struct.unpack_from("<4I", mat, 0xEC)
            if n == 0 or rv == 0:
                continue
            if csize != rv * RENDERED_VERTEX + lv * LIGHTMAP_VERTEX:
                raise ConvertError(f"vertex blob {csize} != {rv}x32 + {lv}x8")
            vo = cptr - A
            if vo < 0 or vo + csize > len(block):
                raise ConvertError("vertex blob outside the block")
            if first < 0 or first + n > nsurf:
                raise ConvertError("material surface range outside the surface array")
            blob = block[vo:vo + csize]
            pos, uv, lmuv = [], [], []
            for i in range(rv):
                x, y, z = struct.unpack_from("<3f", blob, i * RENDERED_VERTEX)
                u, v = struct.unpack_from("<2f", blob, i * RENDERED_VERTEX + 24)
                pos.append((x, y, z))
                uv.append((u, v))
            lbase = rv * RENDERED_VERTEX
            for i in range(rv):
                if i < lv:
                    a, b = struct.unpack_from("<2h", blob, lbase + i * LIGHTMAP_VERTEX + 4)
                    lmuv.append((a / 32767.0, b / 32767.0))
                else:
                    lmuv.append((0.0, 0.0))
            tris = [struct.unpack_from("<3H", surfaces, (first + s) * 6) for s in range(n)]
            if any(max(t) >= rv for t in tris):
                raise ConvertError("surface index beyond the material's vertex array")
            out.append(Material(shader_id, page if lv else NO_LIGHTMAP, pos, uv, lmuv, tris,
                                list(range(first, first + n))))
    return out, bounds


# ---- baking ---------------------------------------------------------------------------
def _f32(v: float) -> float:
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _s16(v: float) -> int:
    return max(-32768, min(32767, int(round(v))))


def _unpack(fmt: int, imm: int, num: int, data: bytes) -> bytes:
    """One VIF UNPACK: command word (cmd | num << 16 | imm) followed by `num` elements."""
    return struct.pack("<I", (fmt << 24) | (num << 16) | imm) + data


CHROMA_PAGES: dict | None = None     # lightmap page index -> full-resolution RGBA page of the BSP being baked
CHROMA_STRENGTH_BSP: float = 0.4     # its chroma strength (texconv.bsp_chroma)


def vertex_colour(material: Material, vertex: int) -> tuple[int, int, int, int]:
    """r, g, b, a of one vertex of `material` (0x80 = 1.0, modulate).

    With lightmap pages loaded (`convert_map(..., chroma=True)`) this is the chroma-only tint of the
    page at the vertex's lightmap uv (ADR-004: luminance lives in the LMn page, low-frequency colour
    in the vertex); otherwise neutral. The packet layout, sizes and the C reader do not depend on it."""
    if CHROMA_PAGES is not None and material.lightmap_page != NO_LIGHTMAP:
        page = CHROMA_PAGES.get(material.lightmap_page)
        if page is not None:
            from halo2ps2 import texconv
            r, g, b = texconv.vertex_chroma(page, material.lmuv[vertex], CHROMA_STRENGTH_BSP)
            return r, g, b, 0x80
    return 0x80, 0x80, 0x80, 0x80


SANE_UV = 64.0         # tiling beyond +-64 is not authored; larger values are garbage and clamp


def uv_shift(uvs: list[tuple[float, float]]) -> int:
    """Largest power of two such that every sane texture uv fits an s16 (retail data has a
    handful of vertices with |u| > 1e6, which are ignored here and clamp when baked)."""
    ref = max((abs(c) for uv in uvs for c in uv if math.isfinite(c) and abs(c) <= SANE_UV), default=1.0)
    return max(0, min(14, int(math.floor(math.log2(32767.0 / max(ref, 1.0))))))


def _morton(x: int, y: int, z: int) -> int:
    out = 0
    for b in range(10):
        out |= ((x >> b & 1) << (3 * b)) | ((y >> b & 1) << (3 * b + 1)) | ((z >> b & 1) << (3 * b + 2))
    return out


def _spatial_order(m: Material, tris: list[int]) -> list[int]:
    """Triangle indices sorted along a Morton curve of their centroids, so that every run of
    24 is spatially compact (tight culling spheres). Ties keep the source order."""
    if len(tris) <= 1:
        return tris
    cen = []
    for t in tris:
        a, b, c = (m.pos[vi] for vi in m.tris[t])
        cen.append(tuple((a[k] + b[k] + c[k]) / 3.0 for k in range(3)))
    lo = [min(c[k] for c in cen) for k in range(3)]
    span = [(max(c[k] for c in cen) - lo[k]) or 1.0 for k in range(3)]
    keys = [_morton(*(min(1023, int((c[k] - lo[k]) / span[k] * 1023.0)) for k in range(3))) for c in cen]
    return [t for _, _, t in sorted(zip(keys, range(len(tris)), tris))]


def bake_batch(index: int, m: Material, chunk: list[int], shift: int, clusters: tuple[int, ...]) -> Batch:
    """The self-contained VIF packet of <= 24 triangles of one material (docs/08-bsp-section.md)."""
    pts = [m.pos[vi] for t in chunk for vi in m.tris[t]]
    c = tuple(_f32(sum(p[k] for p in pts) / len(pts)) for k in range(3))
    r = max(math.dist(c, p) for p in pts)
    r32 = _f32(r)
    if r32 < r:
        r32 = _f32(r * (1.0 + 2.0 ** -22))
    scale = _f32(r32 / 32767.0) or 1.0          # positions are relative to the batch centre
    pos_b, uv_b, col_b = bytearray(), bytearray(), bytearray()
    for t in chunk:
        for vi in m.tris[t]:
            p = m.pos[vi]
            pos_b += struct.pack("<4h", *[_s16((p[k] - c[k]) / scale) for k in range(3)], 0)
            u, v = (x if math.isfinite(x) else 0.0 for x in m.uv[vi])
            uv_b += struct.pack("<4h", _s16(u * (1 << shift)), _s16(v * (1 << shift)),
                                _s16(m.lmuv[vi][0] * 32767), _s16(m.lmuv[vi][1] * 32767))
            col_b += bytes(vertex_colour(m, vi))
    n = len(chunk) * 3
    raw = (_unpack(VIF_UNPACK_V4_32, TOPS_RELATIVE | VU_HDR, 2,
                   struct.pack("<4f4i", *c, scale, n, 0, 0, 0))
           + _unpack(VIF_UNPACK_V4_16, TOPS_RELATIVE | VU_POS, n, bytes(pos_b))
           + _unpack(VIF_UNPACK_V4_16, TOPS_RELATIVE | VU_UV, n, bytes(uv_b))
           + _unpack(VIF_UNPACK_V4_8, TOPS_RELATIVE | UNSIGNED | VU_COL, n, bytes(col_b)))
    raw += bytes(-len(raw) % 16)
    assert len(raw) == packet_bytes(len(chunk))
    lo = tuple(min(p[k] for p in pts) for k in range(3))
    hi = tuple(max(p[k] for p in pts) for k in range(3))
    return Batch(index, len(chunk), raw, c, r32, clusters, lo, hi)


def packet_bytes(tris: int) -> int:
    """Exact size of a batch packet in bytes: four UNPACK words, 2 qwords of constants, then n
    elements of 8 (position), 8 (uv) and 4 (colour) bytes, padded to a quadword."""
    n = tris * 3
    return (4 + 32 + 4 + n * 8 + 4 + n * 8 + 4 + n * 4 + 15) & ~15


def trivial_structure(materials: list[Material]) -> bsp.Structure:
    """One cluster that owns every surface, one leaf, no tree: for synthetic inputs and tests."""
    total = 0
    for m in materials:
        if not m.surfaces:
            m.surfaces = list(range(total, total + len(m.tris)))
        total += len(m.tris)
    cl = bsp.Cluster(-1, -1, -1, -1, -1, 0, [], set())
    return bsp.Structure(total, [], [], [0], [()], [cl], [], 0, {s: {0} for m in materials for s in m.surfaces})


def convex_polygon(verts: list[tuple[float, float, float]], plane: tuple[float, float, float, float]
                   ) -> tuple[list[tuple[float, float, float]], bool]:
    """Portal polygon, counter-clockwise about the plane normal. A genuinely concave polygon
    (2 of 3676 on retail maps; most 'concave' ones only have collinear vertices) is replaced by
    its convex hull, which can only make the portal larger: visibility stays conservative."""
    n = len(verts)
    nx, ny, nz, _ = plane

    def cross_dot(a, b, c) -> float:
        u = [b[k] - a[k] for k in range(3)]
        w = [c[k] - b[k] for k in range(3)]
        return (u[1] * w[2] - u[2] * w[1]) * nx + (u[2] * w[0] - u[0] * w[2]) * ny + (u[0] * w[1] - u[1] * w[0]) * nz

    def length(a, b) -> float:
        return math.dist(a, b)

    concave = False
    for i in range(n):
        a, b, c = verts[i], verts[(i + 1) % n], verts[(i + 2) % n]
        if cross_dot(a, b, c) < -1e-3 * length(a, b) * length(b, c):
            concave = True
    if not concave:
        return list(verts), False
    # monotone chain on the dominant-axis projection; the result is made CCW about the normal
    drop = max(range(3), key=lambda k: abs(plane[k]))
    ax = [k for k in range(3) if k != drop]
    if plane[drop] < 0:
        ax.reverse()
    pts = sorted(set(verts), key=lambda p: (p[ax[0]], p[ax[1]]))

    def cr(o, a, b) -> float:
        return (a[ax[0]] - o[ax[0]]) * (b[ax[1]] - o[ax[1]]) - (a[ax[1]] - o[ax[1]]) * (b[ax[0]] - o[ax[0]])

    hull: list = []
    for seq in (pts, pts[::-1]):
        part: list = []
        for p in seq:
            while len(part) >= 2 and cr(part[-2], part[-1], p) <= 0:
                part.pop()
            part.append(p)
        hull += part[:-1]
    return hull, True


def uv_world(m: Material) -> float:
    """World units per texture uv unit of a material (+0x10 of its 32 byte record; 0 = unknown): sqrt(area in the world / area in uv space) over its triangles. The
    streamed texture variants (docs/37) use it with the shader's tiling and the texture size to tell how many texels a world unit has, so a surface asks for the sharper
    texture when the camera comes near enough to magnify the resident one. Degenerate (uv-less or zero area) materials give 0."""
    aw = au = 0.0
    for a, b, c in m.tris:
        pa, pb, pc = m.pos[a], m.pos[b], m.pos[c]
        e1 = (pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2])
        e2 = (pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2])
        cx, cy, cz = e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]
        ua, ub, uc = m.uv[a], m.uv[b], m.uv[c]
        du = abs((ub[0] - ua[0]) * (uc[1] - ua[1]) - (ub[1] - ua[1]) * (uc[0] - ua[0]))
        aw += 0.5 * (cx * cx + cy * cy + cz * cz) ** 0.5
        au += 0.5 * du
    if au < 1e-9 or aw < 1e-12:
        return 0.0
    return float((aw / au) ** 0.5)


def build_section(materials: list[Material], bounds: tuple[float, ...],
                  structure: bsp.Structure | None = None) -> bytes:
    st = structure or trivial_structure(materials)
    if len(st.clusters) > 0xFFFE or len(st.portals) > 0xFFFF:
        raise ConvertError("too many clusters or portals for the u16 tables")

    # 1. group triangles by (cluster set, material); a surface no cluster lists is never drawn
    groups: dict[tuple[tuple[int, ...], int], list[int]] = {}
    dropped = 0
    for mi, m in enumerate(materials):
        for t, s in enumerate(m.surfaces):
            cs = st.cluster_surfaces.get(s)
            if not cs:
                dropped += 1
                continue
            groups.setdefault((tuple(sorted(cs)), mi), []).append(t)
    shifts = [uv_shift(m.uv) for m in materials]

    # 2. batches, ordered by (lowest cluster, material, cluster set): a cluster's exclusive
    #    batches are contiguous and material-sorted
    batches: list[Batch] = []
    for (cs, mi), tris in sorted(groups.items(), key=lambda kv: (kv[0][0][0], kv[0][1], kv[0][0])):
        m = materials[mi]
        order = _spatial_order(m, tris)
        for start in range(0, len(order), MAX_TRIS):
            batches.append(bake_batch(mi, m, order[start:start + MAX_TRIS], shifts[mi], cs))
    if len(batches) > 0xFFFF:
        raise ConvertError("too many batches for the u16 cluster lists")

    # 3. per-cluster batch lists (a multi-cluster batch is listed by each of its clusters),
    #    ordered by material
    lists: list[list[int]] = [[] for _ in st.clusters]
    for bi, b in enumerate(batches):
        for c in b.clusters:
            lists[c].append(bi)
    for lst in lists:
        lst.sort(key=lambda bi: (batches[bi].material, bi))
    cb_flat, cl_recs = [], []
    portal_lists: list[list[int]] = [[] for _ in st.clusters]
    portals = []
    for k, p in enumerate(st.portals):
        verts, _ = convex_polygon(p.vertices, p.plane)
        cen = tuple(_f32(sum(v[i] for v in verts) / len(verts)) for i in range(3)) if verts else (0.0,) * 3
        rad = _f32(max((math.dist(cen, v) for v in verts), default=0.0))
        portals.append((p, verts, cen, rad))
        for c in {p.back, p.front}:
            if c >= 0:
                portal_lists[c].append(k)
    cp_flat = []
    world = tuple(bounds)
    for ci, cl in enumerate(st.clusters):
        pts_lo, pts_hi = [], []
        for bi in lists[ci]:
            pts_lo.append(batches[bi].lo)
            pts_hi.append(batches[bi].hi)
        for k in portal_lists[ci]:
            vs = portals[k][1]
            if vs:
                pts_lo.append(tuple(min(v[i] for v in vs) for i in range(3)))
                pts_hi.append(tuple(max(v[i] for v in vs) for i in range(3)))
        if pts_lo:
            lo = tuple(min(p[i] for p in pts_lo) for i in range(3))
            hi = tuple(max(p[i] for p in pts_hi) for i in range(3))
        else:                                    # empty cluster: the whole world is the safe bound
            lo, hi = (world[0], world[1], world[2]), (world[3], world[4], world[5])
        flags = ((CL_SKY if cl.sky >= 0 else 0) | (CL_FOG if cl.fog != -1 else 0) |
                 (CL_WEATHER if cl.weather >= 0 else 0) | (CL_MIRROR if cl.mirrors else 0) |
                 (CL_FOG_BIT15 if cl.fog != -1 and cl.fog & 0x8000 else 0))
        fog = -1 if cl.fog == -1 else cl.fog & 0x7FFF
        tris = sum(batches[bi].triangles for bi in lists[ci])
        cl_recs.append(struct.pack("<3fHh3fhh4I2hI8x", *lo, flags, fog, *hi, cl.sky, cl.weather,
                                   len(cb_flat), len(lists[ci]), len(cp_flat), len(portal_lists[ci]),
                                   cl.background_sound, cl.sound_environment, tris))
        cb_flat += lists[ci]
        cp_flat += portal_lists[ci]
    assert all(len(r) == CLUSTER_REC for r in cl_recs)

    # 4. serialise
    def pad16(b: bytes) -> bytes:
        return b + bytes(-len(b) % 16)

    vif, btab = bytearray(), bytearray()
    for b in batches:
        btab += struct.pack("<HHII3ffI", b.material, b.triangles, len(vif), len(b.vif) // 16, *b.centre, b.radius, 0)
        vif += b.vif
    batch_counts = [0] * len(materials)
    tri_counts = [0] * len(materials)
    for b in batches:
        batch_counts[b.material] += 1
        tri_counts[b.material] += b.triangles
    mtab = b"".join(struct.pack("<IHHHHIf12x", m.shader_id, m.lightmap_page, 0, batch_counts[i], shifts[i],
                                tri_counts[i], uv_world(m)) for i, m in enumerate(materials))
    ptab, pverts = bytearray(), bytearray()
    nverts = 0
    for p, verts, cen, rad in portals:
        a = p.front if p.front >= 0 else NO_CLUSTER
        b = p.back if p.back >= 0 else NO_CLUSTER
        ptab += struct.pack("<HHHHII4f3ff", a, b, len(verts), p.flags & 0xFFFF, nverts, 0, *p.plane, *cen, rad)
        for v in verts:
            pverts += struct.pack("<3f", *v)
        nverts += len(verts)
    assert len(ptab) == PORTAL_REC * len(portals)
    ntab = b"".join(struct.pack("<3I", pl, fr, bk) for pl, fr, bk in st.nodes)
    plane_tab = b"".join(struct.pack("<4f", *pl) for pl in st.planes)
    leaf_tab = b"".join(struct.pack("<H", c) for c in st.leaves)
    bodies = {"materials": (mtab, len(materials)), "batches": (bytes(btab), len(batches)),
              "clusters": (b"".join(cl_recs), len(cl_recs)),
              "cluster_batches": (struct.pack(f"<{len(cb_flat)}H", *cb_flat), len(cb_flat)),
              "cluster_portals": (struct.pack(f"<{len(cp_flat)}H", *cp_flat), len(cp_flat)),
              "portals": (bytes(ptab), len(portals)), "portal_vertices": (bytes(pverts), nverts),
              "nodes": (ntab, len(st.nodes)), "planes": (plane_tab, len(st.planes)),
              "leaves": (leaf_tab, len(st.leaves)), "vif": (bytes(vif), len(vif))}
    out = bytearray(HEADER_SIZE)
    directory = {}
    for name in TABLES:
        body, count = bodies[name]
        directory[name] = (len(out), count)
        out += pad16(body)
    struct.pack_into("<4sHHII6f", out, 0, MAGIC, VERSION, 0, len(out), sum(tri_counts), *bounds)
    for i, name in enumerate(TABLES):
        struct.pack_into("<II", out, DIR_AT + 8 * i, *directory[name])
    return bytes(out)


# ---- reading back (tests and --info); mirrors the validation of engine/src/world/bsp.c ----
def parse_section(data: bytes) -> Section:
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ConvertError("not a BSPS section")
    _m, ver, _r, total, tris, *rest = struct.unpack_from("<4sHHII6f", data)
    bounds = tuple(rest[:6])
    if ver != VERSION:
        raise ConvertError(f"BSPS version {ver} (this reader handles {VERSION})")
    if total != len(data):
        raise ConvertError("section size field disagrees with the data")
    d = {}
    for i, name in enumerate(TABLES):
        off, count = struct.unpack_from("<II", data, DIR_AT + 8 * i)
        d[name] = (off, count)
    sizes = {"materials": MATERIAL_REC, "batches": BATCH_REC, "vif": 1, "clusters": CLUSTER_REC,
             "cluster_batches": 2, "cluster_portals": 2, "portals": PORTAL_REC, "portal_vertices": 12,
             "nodes": NODE_REC, "planes": PLANE_REC, "leaves": 2}
    for name, (off, count) in d.items():
        if off % 16 or off < HEADER_SIZE or off > len(data) or count * sizes[name] > len(data) - off:
            raise ConvertError(f"table {name}: bad offset/count")
    nm, nb, nc, ncb, ncp, npo, npv, nn, npl, nl = (d[k][1] for k in (
        "materials", "batches", "clusters", "cluster_batches", "cluster_portals", "portals",
        "portal_vertices", "nodes", "planes", "leaves"))
    if nm > 0xFFFF or nb > 0xFFFF or nc > 0xFFFE or npo > 0xFFFF:
        raise ConvertError("table counts exceed the u16 indices")

    def rec(name: str, fmt: str, i: int):
        return struct.unpack_from(fmt, data, d[name][0] + i * sizes[name])

    vif = data[d["vif"][0]:d["vif"][0] + d["vif"][1]]
    materials = []
    for i in range(nm):
        sid, page, _r6, bc, shift, t, uvw = rec("materials", "<IHHHHIf", i)
        materials.append({"shader_id": sid, "lightmap_page": page, "batch_count": bc, "shift": shift, "triangles": t, "uv_world": uvw})
    batches = []
    for i in range(nb):
        mat, t, off, qw, cx, cy, cz, r, _ = rec("batches", "<HHII3ffI", i)
        if not 1 <= t <= MAX_TRIS or mat >= nm or off % 16 or off + qw * 16 > len(vif) \
                or qw * 16 != packet_bytes(t) or not r >= 0.0:
            raise ConvertError(f"batch {i}: bad size/offset")
        batches.append({"material": mat, "triangles": t, "vif_offset": off, "vif_qwords": qw,
                        "centre": (cx, cy, cz), "radius": r})
    if sum(b["triangles"] for b in batches) != tris:
        raise ConvertError("triangle counts disagree")
    for i, m in enumerate(materials):
        mine = [b for b in batches if b["material"] == i]
        if m["batch_count"] != len(mine) or m["triangles"] != sum(b["triangles"] for b in mine):
            raise ConvertError(f"material {i}: batch/triangle counts disagree with the batches")
    nodes = [rec("nodes", "<3I", i) for i in range(nn)]
    planes = [rec("planes", "<4f", i) for i in range(npl)]
    leaves = [rec("leaves", "<H", i)[0] for i in range(nl)]
    for i, (pl, fr, bk) in enumerate(nodes):
        if pl >= npl:
            raise ConvertError(f"node {i}: plane out of range")
        for c in (fr, bk):
            if c != NO_CHILD and not (c & LEAF_FLAG and c & ~LEAF_FLAG < nl) and not (i < c < nn):
                raise ConvertError(f"node {i}: bad child")
    if nn == 0 and nl > 1:
        raise ConvertError("no nodes but several leaves")
    if any(c != NO_CLUSTER and c >= nc for c in leaves):
        raise ConvertError("leaf cluster out of range")
    cb = [rec("cluster_batches", "<H", i)[0] for i in range(ncb)]
    cp = [rec("cluster_portals", "<H", i)[0] for i in range(ncp)]
    if any(v >= nb for v in cb) or any(v >= npo for v in cp):
        raise ConvertError("cluster list index out of range")
    clusters = []
    for i in range(nc):
        (x0, y0, z0, flags, fog, x1, y1, z1, sky, weather, fb, bc, fp, pc, bg, env, ct) = \
            rec("clusters", "<3fHh3fhh4I2hI", i)
        if fb + bc > ncb or fp + pc > ncp:
            raise ConvertError(f"cluster {i}: list range outside the list")
        clusters.append({"min": (x0, y0, z0), "max": (x1, y1, z1), "flags": flags, "fog": fog, "sky": sky,
                         "weather": weather, "first_batch": fb, "batch_count": bc, "first_portal": fp,
                         "portal_count": pc, "background_sound": bg, "sound_environment": env, "triangles": ct})
    pverts = [rec("portal_vertices", "<3f", i) for i in range(npv)]
    portals = []
    for i in range(npo):
        a, b, vn, flags, fv, _r, *plane = rec("portals", "<HHHHII4f3ff", i)
        if (a != NO_CLUSTER and a >= nc) or (b != NO_CLUSTER and b >= nc) or fv + vn > npv:
            raise ConvertError(f"portal {i}: bad cluster or vertex range")
        portals.append({"a": a, "b": b, "vertex_count": vn, "flags": flags, "first_vertex": fv,
                        "plane": tuple(plane[:4]), "centre": tuple(plane[4:7]), "radius": plane[7]})
    return Section(bounds, tris, materials, batches, clusters, cb, cp, portals, pverts, nodes, planes,
                   leaves, vif, len(data))


def find_cluster(sec: Section, p: tuple[float, float, float]) -> int:
    """Point location on a parsed section: -1 outside the world. Same walk as the C reader."""
    if not sec.nodes:
        return sec.leaves[0] if len(sec.leaves) == 1 and sec.leaves[0] != NO_CLUSTER else -1
    n = 0
    for _ in range(len(sec.nodes)):
        pl, fr, bk = sec.nodes[n]
        q = sec.planes[pl]
        c = fr if q[0] * p[0] + q[1] * p[1] + q[2] * p[2] - q[3] >= 0.0 else bk
        if c == NO_CHILD:
            return -1
        if c & LEAF_FLAG:
            cl = sec.leaves[c & ~LEAF_FLAG]
            return -1 if cl == NO_CLUSTER else cl
        n = c
    return -1


def decode_batch(sec: Section, batch: dict) -> list[dict]:
    """Expanded vertices of one batch: world position, texture uv, lightmap uv, colour."""
    mat = sec.materials[batch["material"]]
    raw = sec.vif[batch["vif_offset"]:batch["vif_offset"] + batch["vif_qwords"] * 16]
    streams: dict[int, tuple[int, list]] = {}
    i = 0
    while i < len(raw):
        word = struct.unpack_from("<I", raw, i)[0]
        i += 4
        if word == VIF_NOP:
            continue
        cmd, num, imm = word >> 24, (word >> 16) & 0xFF, word & 0xFFFF
        if not imm & TOPS_RELATIVE:
            raise ConvertError(f"VIF UNPACK {word:#010x} is not TOPS-relative")
        addr = imm & 0x3FF
        if cmd == VIF_UNPACK_V4_32 and addr == VU_HDR and num == 2:
            elems = [struct.unpack_from("<4f", raw, i), struct.unpack_from("<4i", raw, i + 16)]
            i += 32
        elif cmd == VIF_UNPACK_V4_16 and not imm & UNSIGNED:
            elems = [struct.unpack_from("<4h", raw, i + k * 8) for k in range(num)]
            i += num * 8
        elif cmd == VIF_UNPACK_V4_8 and imm & UNSIGNED:
            elems = [struct.unpack_from("<4B", raw, i + k * 4) for k in range(num)]
            i += num * 4
        else:
            raise ConvertError(f"unexpected VIF command {word:#010x}")
        streams[addr] = (cmd, elems)
    if set(streams) != {VU_HDR, VU_POS, VU_UV, VU_COL}:
        raise ConvertError("batch packet does not hold the four expected UNPACKs")
    (cx, cy, cz, scale), (n, z1, z2, z3) = streams[VU_HDR][1]
    if (z1, z2, z3) != (0, 0, 0) or n != batch["triangles"] * 3:
        raise ConvertError("batch constants disagree with the triangle count")
    pos, uv, col = streams[VU_POS][1], streams[VU_UV][1], streams[VU_COL][1]
    if not len(pos) == len(uv) == len(col) == n:
        raise ConvertError("vertex stream length does not match the triangle count")
    if (cx, cy, cz) != batch["centre"] or abs(scale * 32767.0 - batch["radius"]) > batch["radius"] * 1e-5:
        raise ConvertError("packet constants disagree with the batch record")
    return [{"pos": (cx + x * scale, cy + y * scale, cz + z * scale),
             "uv": (s / (1 << mat["shift"]), t / (1 << mat["shift"])),
             "lmuv": (ls / 32767.0, lt / 32767.0), "colour": tuple(c)}
            for (x, y, z, _), (s, t, ls, lt), c in zip(pos, uv, col)]


def section_tag(index: int) -> str:
    """Pack tag of BSP `index`: BSP0..BSP9, then BSPA..BSPZ (b40 and c40 have 13 BSPs, more than
    the ten digits docs/07-hp2-format.md allows)."""
    if not 0 <= index < 36:
        raise ConvertError(f"BSP index {index} does not fit a four-character tag")
    return "BSP" + "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[index]


def convert_block(block: bytes, loc: bsp.BspLocation) -> bytes:
    mats, bounds = read_materials(block, loc)
    return build_section(mats, bounds, bsp.read_structure(loc, block))


def convert_map(path: Path, bsp_indices: list[int] | None = None, chroma: bool = True) -> bytes:
    global CHROMA_PAGES, CHROMA_STRENGTH_BSP
    from halo2ps2 import texconv
    tmap = tinfos = None
    if chroma:
        tmap = texconv.MapData.open(path)
        tinfos = {b.index: b for b in texconv.scan_map(tmap)}
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
        locs = bsp.locate(idx, region, hdr.decompressed_size)
        sections = []
        for i, loc in enumerate(locs):
            if bsp_indices is not None and i not in bsp_indices:
                continue
            f.seek(loc.file_offset)
            CHROMA_PAGES = texconv.lightmap_pages_rgba(tmap, tinfos[i]) if chroma and i in tinfos else None
            CHROMA_STRENGTH_BSP = texconv.bsp_chroma(tinfos[i].path) if chroma and i in tinfos else texconv.CHROMA_STRENGTH
            try:
                sections.append((section_tag(i), convert_block(f.read(loc.size), loc)))
            finally:
                CHROMA_PAGES = None
    for tag, blob in sections:
        if len(blob) > SIZE_BUDGET:
            print(f"warning: {path.stem} {tag}: {len(blob) / 2**20:.2f} MiB raw exceeds the 7 MiB budget",
                  file=sys.stderr)
    return hp2.build(path.stem, sections)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--bsp", type=int, action="append", help="only this BSP index (repeatable)")
    ap.add_argument("--info", action="store_true", help="input is a .hp2: print its BSP sections")
    ap.add_argument("--no-chroma", action="store_true", help="neutral vertex colours (skip the lightmap chroma bake)")
    args = ap.parse_args(argv)
    if args.info:
        pk = hp2.parse(args.input.read_bytes())
        for s in pk.sections:
            if s.tag.startswith("BSP"):
                sec = parse_section(pk.read(s.tag))
                print(f"{s.tag}: {len(sec.materials)} materials, {len(sec.batches)} batches, "
                      f"{sec.triangles} triangles, {len(sec.clusters)} clusters, {len(sec.portals)} portals, "
                      f"{len(sec.nodes)} nodes, {s.raw_size / 2**20:.2f} MiB raw, "
                      f"{s.stored_size / 2**20:.2f} MiB stored")
        return 0
    if not args.out:
        ap.error("-o is required")
    data = convert_map(args.input, args.bsp, chroma=not args.no_chroma)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"wrote {args.out} ({len(data) / 2**20:.2f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
