#!/usr/bin/env python3
"""Convert the collision BSP of every Xbox ``structure_bsp`` of a map into the PS2 ``COLn`` sections, version 1
(docs/15-coll-section.md; the reader is engine/src/world/coll.c, its contract engine/src/world/coll.h).

Source layout.  Everything below was read off the retail maps (all 69 BSPs of the ten campaign maps and ui.map)
and checked by ``verify_source`` (counts, index ranges, tree shape, winged-edge closure, plane-versus-vertex
distance, 2D BSP lookups against the polygons); "unverified" marks what the data cannot confirm.  A reflexive is
``u32 count, u32 address, u32 pad``; ``structure_bsp`` +0x0B0 is the collision BSP reflexive (always one element,
96 bytes) whose eight reflexives are, in order (element sizes verified from the address gaps on every BSP):

    +0x00 bsp3d nodes     (12 B)  u32 plane, u32 back child, u32 front child
    +0x0C planes          (16 B)  f32 nx ny nz d     n.p = d; a point is in front iff n.p - d >= 0
    +0x18 leaves          ( 8 B)  u16 flags, u16 bsp2d reference count, u32 first reference
    +0x24 bsp2d references( 8 B)  u32 plane (bit 31 = flip), u32 root (bit 31: a surface, else a bsp2d node)
    +0x30 bsp2d nodes     (20 B)  f32 a, b, d (2D line), u32 child, u32 child
    +0x3C surfaces        (12 B)  u32 plane (bit 31 = flip), i32 first edge, u8 flags, i8 breakable, i16 material
    +0x48 edges           (24 B)  i32 start vertex, end vertex, forward edge, reverse edge, left surface, right surface
    +0x54 vertices        (16 B)  f32 x y z, i32 first edge

* **bsp3d tree.**  Child ``0xFFFFFFFF`` is solid space (also everything outside the world), bit 31 set a leaf
  (index in the low bits), otherwise a node index.  Root is node 0, every child index is greater than its parent, no
  node or leaf has two parents (checked on all maps).  Space inside a leaf is empty; the surfaces lie on the leaf's
  boundary planes.
* **Two-sided surfaces** (flag bit 0) always come as a pair: two coincident polygons with opposite orientation, each
  flagged two-sided and listed by its own front leaf (verified on all ten maps: 16,818 two-sided surfaces, every one has
  its twin: same vertex positions, opposite normal; ``Report.twin_missing``).  So a surface is only ever hit from its
  front; the back of a two-sided surface is the front of its twin.
* **Leaves.**  Flags bit 0 = "contains two-sided surfaces" (verified: set exactly when a surface reachable from the
  leaf's references is two-sided, on all maps; no other bit was ever seen).  A leaf owns the references
  ``first .. first + count`` (the u16 count always fits).  The ranges are NOT consecutive in leaf order on 40 of the 69 BSPs:
  3,565 of the 1.74 M references are owned by no leaf (orphans) and 5,013 by several leaves.  The converter re-emits the
  references leaf by leaf (one copy per owner, orphans dropped).
* **References.**  A reference is a plane of the leaf's boundary that carries surfaces, plus the root of the 2D BSP that
  names the surface of that plane at a point.  The flip bit is equal to the flip bit of every surface the tree reaches
  (never differs), so ``(plane, flip)`` identifies a surface orientation.
* **2D BSP.**  The point is projected onto the plane by dropping the dominant axis ``d`` of the plane normal (on exact ties the
  LATER axis wins: with the earlier-axis rule 33 of 33064 lookups on a10a failed, with this one none):
  ``(u, v) = ((d+1) % 3, (d+2) % 3)``, swapped when ``sigma`` is false, where ``sigma = (n[d] > 0) xor flip``.  Then
  ``side = a*u + b*v - dd``; ``side >= 0`` takes the SECOND child, otherwise the first.  A child (and a reference root) with
  bit 31 set is a surface index.  No child is ever ``0xFFFFFFFF``: the 2D BSP has no empty leaves, every point of the
  plane gets a surface, also points where there is none (a crossing of the plane in an open part of a leaf's face).  The
  lookup is therefore a candidate, not a decision (see docs/15, "Rays").  Verified: the 2D lookup of an interior point of
  every surface of every reference returns that surface (2,264,007 lookups on the 69 BSPs, none wrong).  The 2D trees
  of a BSP are trees (every node has exactly one parent, all reachable), their depth first stack need is at most 8.
* **Surfaces.**  Plane + flip: the surface faces the side of ``n * (flip ? -1 : 1)``, which is the winding of its
  polygon (counter-clockwise seen from the front; verified for every surface).  Flags: bit 0 two-sided, bit 1 invisible,
  bit 2 climbable, bit 3 breakable (bit meanings from the public tag documentation; the census of the 1,059,523 surfaces of
  the ten maps: flags 0: 1,042,705, 1: 9,684, 3 (two-sided + invisible): 4,248, 5 (two-sided + climbable): 48,
  7: 1,358, 9 (two-sided + breakable): 1,480, nothing else; the breakable byte is 0..31 and only non-zero on flag 9
  surfaces).  Material: index into the BSP's collision material
  palette (``structure_bsp`` +0x0A4, 20 B: a shader tag reference, then at +0x12 a u16).  That u16 equals the shader's
  material type (shader +0x22 in senv, sotr, sgla: equal in 1565 of the 1573 palette entries of the ten campaign maps;
  the other 8 are ``soso`` "invisible" shaders whose value is 0) and indexes the globals material table (0 dirt, 1 sand,
  2 stone, 3 snow, 4 wood, 5 metal hollow, 6 metal thin, 7 metal thick, 9 glass, 28 water, 31 ice: all consistent with
  the shader names).  The converter stores that type per surface.
* **Edges** form a winged-edge structure: surface ``s``'s polygon is walked from its first edge ``e``: if ``left(e) == s``
  take ``start(e)`` and go to ``forward(e)``, else take ``end(e)`` and go to ``reverse(e)``, until back at the first edge.
  Every loop closes, every edge is used exactly once as a left and once as a right side, the vertices lie on the
  plane to <= 4.1e-4.  Every polygon has 3..8 vertices and is convex; no degenerate one exists.  The converter keeps the vertex loops only (vertex indices per surface), not the edges.

Output section (``COLn``, little-endian, tables 16-aligned): see docs/15-coll-section.md.

Usage:
    python3 -I tools/halo2ps2/collconv.py assets/decompressed/a10.map -o assets/hp2/a10.coll.hp2 [--bsp N]
    python3 -I tools/halo2ps2/collconv.py --info assets/hp2/a10.coll.hp2
    python3 -I tools/halo2ps2/collconv.py --verify assets/decompressed/a10.map [--bsp N]
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bsp, hp2, mapinfo, tagindex  # noqa: E402

MAGIC = b"COLL"
VERSION = 1
HEADER_SIZE = 128
NODE_REC = 8
PLANE_REC = 16
LEAF_REC = 4
REF_REC = 4
N2_REC = 16
SURF_REC = 8
INDEX_REC = 2
VERT_REC = 12
# directory order (offset, count) at 0x30
TABLES = ("nodes", "planes", "leaves", "refs", "nodes2d", "surfaces", "indices", "vertices")
DIR_AT = 0x30
assert DIR_AT + 8 * len(TABLES) <= HEADER_SIZE

NO_CHILD = 0xFFFFFFFF
HI = 0x80000000
CHILD24_NONE = 0xFFFFFF
CHILD24_LEAF = 0x800000
LEAF_TWO_SIDED = 0x80000000      # leaf table entry bit 31
MAX_NODES = 0x7FFFFF             # 23-bit node / leaf indices
MAX_U15 = 0x7FFF                 # planes, 2D nodes, surfaces
MAX_VERTS = 0xFFFF
MAX_INDICES = 0xFFFFF
NO_MATERIAL = 0xFF
STACK_3D = 128                   # reader's explicit stacks (coll.h COLL_STACK_*); the converter checks the tree fits
STACK_2D = 64

F_TWO_SIDED, F_INVISIBLE, F_CLIMBABLE, F_BREAKABLE = 1, 2, 4, 8
KNOWN_FLAGS = 15

SRC_STRIDES = (12, 16, 8, 8, 20, 12, 24, 16)
SRC_NAMES = ("nodes", "planes", "leaves", "refs", "nodes2d", "surfaces", "edges", "vertices")


class CollError(ValueError):
    pass


# ---- reading the Xbox collision BSP ---------------------------------------------------
@dataclass
class Source:
    nodes: list[tuple[int, int, int]]            # (plane, back, front) as stored
    planes: list[tuple[float, float, float, float]]
    leaves: list[tuple[int, int, int]]           # (flags, ref count, first ref)
    refs: list[tuple[int, int]]                  # (plane | flip<<31, root)
    nodes2d: list[tuple[float, float, float, int, int]]
    surfaces: list[tuple[int, int, int, int, int]]   # (plane | flip<<31, first edge, flags, breakable, material)
    edges: list[tuple[int, int, int, int, int, int]]
    vertices: list[tuple[float, float, float, int]]
    palette: list[int]                           # collision material palette: material type per entry
    name: str = ""


def read_source(block: bytes, loc: bsp.BspLocation) -> Source:
    """Parse the collision BSP of one structure_bsp block; raises CollError on anything outside the block."""
    A = loc.address
    if len(block) < bsp.HEADER_SIZE or block[0x14:0x18] != bsp.SBSP_MAGIC:
        raise CollError(f"{loc.path}: not a structure_bsp block")
    root = struct.unpack_from("<I", block, 0)[0] - A
    if not 0 <= root <= len(block) - bsp.STRUCT_SIZE:
        raise CollError(f"{loc.path}: structure_bsp root outside the block")
    count, addr = struct.unpack_from("<II", block, root + 0xB0)
    cb = addr - A
    if count != 1 or cb < 0 or cb + 96 > len(block):
        raise CollError(f"{loc.path}: collision bsp reflexive ({count} at {addr:#x}) invalid")
    tables = []
    for k, stride in enumerate(SRC_STRIDES):
        n, a = struct.unpack_from("<II", block, cb + 12 * k)
        o = a - A
        if n and (o < 0 or n > 0x1000000 or o + n * stride > len(block)):
            raise CollError(f"{loc.path}: {SRC_NAMES[k]} {n} x {stride} at {a:#x} outside the block")
        tables.append(block[o:o + n * stride] if n else b"")
    pn, pa = struct.unpack_from("<II", block, root + 0xA4)
    po = pa - A
    if pn and (po < 0 or pn > 0xFFFF or po + pn * 20 > len(block)):
        raise CollError(f"{loc.path}: collision materials outside the block")
    palette = [struct.unpack_from("<H", block, po + i * 20 + 0x12)[0] for i in range(pn)]
    return Source(
        nodes=list(struct.iter_unpack("<3I", tables[0])),
        planes=list(struct.iter_unpack("<4f", tables[1])),
        leaves=list(struct.iter_unpack("<HHI", tables[2])),
        refs=list(struct.iter_unpack("<II", tables[3])),
        nodes2d=list(struct.iter_unpack("<3fII", tables[4])),
        surfaces=list(struct.iter_unpack("<IiBbh", tables[5])),
        edges=list(struct.iter_unpack("<6i", tables[6])),
        vertices=list(struct.iter_unpack("<3fi", tables[7])),
        palette=palette, name=loc.path.split("\\")[-1])


# ---- polygons, projection, lookups ---------------------------------------------------
def surface_polygons(src: Source) -> list[tuple[int, ...]]:
    """Vertex-index loop of every surface (counter-clockwise seen from its front); raises CollError when an edge
    chain does not close, leaves its surface, or an edge is not used exactly once per side."""
    ne, nv, ns = len(src.edges), len(src.vertices), len(src.surfaces)
    used = [0] * (2 * ne)
    out: list[tuple[int, ...]] = []
    for si, s in enumerate(src.surfaces):
        e0 = s[1]
        if not 0 <= e0 < ne:
            raise CollError(f"surface {si}: first edge {e0} outside 0..{ne}")
        e, loop = e0, []
        while True:
            ed = src.edges[e]
            if ed[4] == si and ed[5] != si:
                loop.append(ed[0])
                used[2 * e] += 1
                nxt = ed[2]
            elif ed[5] == si:
                loop.append(ed[1])
                used[2 * e + 1] += 1
                nxt = ed[3]
            else:
                raise CollError(f"surface {si}: edge {e} does not border it")
            if not 0 <= nxt < ne or len(loop) > 1024:
                raise CollError(f"surface {si}: edge chain leaves the table or does not close")
            e = nxt
            if e == e0:
                break
        if len(loop) < 3 or any(not 0 <= v < nv for v in loop):
            raise CollError(f"surface {si}: polygon of {len(loop)} vertices / index outside 0..{nv}")
        out.append(tuple(loop))
    if any(u != 1 for u in used):
        raise CollError(f"{sum(1 for u in used if u != 1)} edge sides are not used exactly once")
    return out


def effective_plane(src: Source, plane_flip: int) -> tuple[float, float, float, float]:
    p = src.planes[plane_flip & 0x7FFFFFFF]
    return (-p[0], -p[1], -p[2], -p[3]) if plane_flip & HI else p


def project_axes(n: tuple[float, float, float], flip: int) -> tuple[int, int]:
    """The 2D coordinates (axis for u, axis for v) of a reference whose plane has normal ``n`` (the plane as stored)."""
    d = 1 if abs(n[1]) >= abs(n[0]) else 0          # ties go to the later axis (verified, see the module docstring)
    if abs(n[2]) >= abs(n[d]):
        d = 2
    sigma = (n[d] > 0) != bool(flip)
    u, v = (d + 1) % 3, (d + 2) % 3
    return (u, v) if sigma else (v, u)


def lookup_2d(src: Source, ref: tuple[int, int], p: tuple[float, float, float]) -> int:
    """Surface that the 2D BSP of ``ref`` assigns to point ``p`` (assumed on the reference's plane)."""
    pl = src.planes[ref[0] & 0x7FFFFFFF]
    ua, va = project_axes(pl[:3], ref[0] >> 31)
    u, v = p[ua], p[va]
    c = ref[1]
    for _ in range(len(src.nodes2d) + 1):
        if c & HI:
            return c & 0x7FFFFFFF
        a, b, d, c0, c1 = src.nodes2d[c]
        c = c1 if a * u + b * v - d >= 0.0 else c0
    raise CollError("2D BSP loops")


def locate(src: Source, p: tuple[float, float, float]) -> int:
    """Leaf containing ``p`` (front when n.p - d >= 0), -1 for solid space."""
    if not src.nodes:
        return -1
    c = 0
    for _ in range(len(src.nodes) + 1):
        pl, back, front = src.nodes[c]
        q = src.planes[pl]
        nxt = front if q[0] * p[0] + q[1] * p[1] + q[2] * p[2] - q[3] >= 0.0 else back
        if nxt == NO_CHILD:
            return -1
        if nxt & HI:
            return nxt & 0x7FFFFFFF
        c = nxt
    raise CollError("3D BSP loops")


def newell(pts: list[tuple[float, float, float]]) -> tuple[float, float, float]:
    nx = ny = nz = 0.0
    for i, a in enumerate(pts):
        b = pts[(i + 1) % len(pts)]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])
    return nx, ny, nz


def interior_point(pts: list[tuple[float, float, float]], normal: tuple[float, float, float]
                   ) -> tuple[float, float, float] | None:
    """A point strictly inside a (possibly concave) planar polygon given counter-clockwise about ``normal``:
    the centroid of the largest ear.  None for a degenerate polygon."""
    n = len(pts)
    best, best_area = None, 0.0
    idx = list(range(n))

    def cross(o, a, b):
        return ((a[1] - o[1]) * (b[2] - o[2]) - (a[2] - o[2]) * (b[1] - o[1]),
                (a[2] - o[2]) * (b[0] - o[0]) - (a[0] - o[0]) * (b[2] - o[2]),
                (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]))

    for i in idx:
        a, b, c = pts[i - 1], pts[i], pts[(i + 1) % n]
        cr = cross(b, c, a)
        area = 0.5 * (cr[0] * normal[0] + cr[1] * normal[1] + cr[2] * normal[2])
        if area <= best_area:
            continue
        ok = True
        for j in idx:
            if j in (i - 1, i, (i + 1) % n):
                continue
            q = pts[j]
            c1 = cross(a, b, q)
            c2 = cross(b, c, q)
            c3 = cross(c, a, q)
            if all(sum(x * y for x, y in zip(cc, normal)) >= 0 for cc in (c1, c2, c3)):
                ok = False
                break
        if ok:
            best, best_area = (a, b, c), area
    if best is None:
        return None
    return tuple(sum(q[i] for q in best) / 3.0 for i in range(3))   # type: ignore[return-value]


# ---- verification -------------------------------------------------------------------
@dataclass
class Report:
    name: str
    counts: dict[str, int]
    problems: list[str] = field(default_factory=list)
    max_plane_dev: float = 0.0
    max_polygon: int = 0
    concave: int = 0
    degenerate: int = 0
    unreferenced: int = 0
    winding_bad: int = 0
    lookup_checked: int = 0
    lookup_bad: int = 0
    leaf_flag_bad: int = 0
    depth3d: int = 0
    stack2d: int = 0
    flags: Counter = field(default_factory=Counter)
    materials: Counter = field(default_factory=Counter)
    breakable: Counter = field(default_factory=Counter)
    two_sided_in_leaves: int = 0
    orphan_refs: int = 0         # references no leaf owns (dropped by the converter)
    shared_refs: int = 0         # references owned by several leaves (emitted once per owner)
    unreachable: int = 0         # surfaces only orphan references reach (dropped)
    two_sided: int = 0           # surfaces with the two-sided flag
    twin_missing: int = 0        # two-sided surfaces without a coincident, oppositely oriented two-sided twin


def tree_depths(src: Source) -> tuple[int, int, list[str]]:
    """(3D depth, 2D stack need) and structural problems of both trees (iterative; children must come later, every
    node and leaf must have exactly one parent, every node must be reachable)."""
    probs: list[str] = []
    nn, nl, ns = len(src.nodes), len(src.leaves), len(src.surfaces)
    depth = 0
    if nn:
        parents = [0] * nn
        leaf_parents = [0] * nl
        dep = [0] * nn
        for k, (pl, back, front) in enumerate(src.nodes):
            if pl >= len(src.planes):
                probs.append(f"node {k}: plane {pl} outside")
            for c in (back, front):
                if c == NO_CHILD:
                    continue
                if c & HI:
                    if (c & ~HI) >= nl:
                        probs.append(f"node {k}: leaf {c & ~HI} outside")
                    else:
                        leaf_parents[c & ~HI] += 1
                        depth = max(depth, dep[k] + 1)
                elif not k < c < nn:
                    probs.append(f"node {k}: child {c} not a later node")
                else:
                    parents[c] += 1
                    dep[c] = max(dep[c], dep[k] + 1)
                    depth = max(depth, dep[c])
        if parents[0] or any(p != 1 for p in parents[1:]):
            probs.append("3D nodes without exactly one parent")
        if any(p != 1 for p in leaf_parents):
            probs.append(f"{sum(1 for p in leaf_parents if p != 1)} leaves without exactly one parent")
    elif nl > 1:
        probs.append("leaves without nodes")
    # 2D: depth of the tree under each reference root; stack need of a depth-first walk pushing one sibling
    n2 = len(src.nodes2d)
    parents2 = [0] * n2
    for k, (_a, _b, _d, c0, c1) in enumerate(src.nodes2d):
        for c in (c0, c1):
            if c & HI:
                if (c & ~HI) >= ns:
                    probs.append(f"2D node {k}: surface {c & ~HI} outside")
            elif not 0 <= c < n2:
                probs.append(f"2D node {k}: child {c} outside")
            else:
                parents2[c] += 1
    for r in src.refs:
        c = r[1]
        if not c & HI and 0 <= c < n2:
            parents2[c] += 1
        elif not c & HI:
            probs.append(f"reference root {c} outside")
    if any(p != 1 for p in parents2):
        probs.append(f"{sum(1 for p in parents2 if p != 1)} 2D nodes without exactly one parent")
    stack = 0
    dep2 = [0] * n2          # depth of each node below its root (children are visited through an explicit stack)
    seen = [False] * n2
    for r in src.refs:
        c = r[1]
        if c & HI or not 0 <= c < n2:
            continue
        st = [c]
        dep2[c] = 1
        while st:
            k = st.pop()
            if seen[k]:
                probs.append("2D node reached twice")
                continue
            seen[k] = True
            stack = max(stack, dep2[k])
            for ch in src.nodes2d[k][3:5]:
                if not ch & HI and 0 <= ch < n2:
                    dep2[ch] = dep2[k] + 1
                    st.append(ch)
    if not all(seen):
        probs.append(f"{seen.count(False)} 2D nodes unreachable")
    return depth, stack, probs


def verify_source(src: Source, polys: list[tuple[int, ...]] | None = None, lookup: bool = True) -> Report:
    """Run every structural and geometric check described in the module docstring; problems are returned, not raised."""
    cnt = {n: len(getattr(src, n)) for n in SRC_NAMES}
    rep = Report(src.name, cnt)
    P = rep.problems
    if polys is None:
        try:
            polys = surface_polygons(src)
        except CollError as e:
            P.append(str(e))
            return rep
    rep.depth3d, rep.stack2d, probs = tree_depths(src)
    P.extend(probs)
    nl, nr, ns = len(src.leaves), len(src.refs), len(src.surfaces)
    # leaves: reference ranges inside the table; orphan (owned by no leaf) and shared references are counted
    owners = [0] * nr
    for k, (_fl, count, first) in enumerate(src.leaves):
        if count and first + count > nr:
            P.append(f"leaf {k}: references {first}+{count} outside 0..{nr}")
            return rep
        for r in range(first, first + count):
            owners[r] += 1
    rep.orphan_refs = owners.count(0)
    rep.shared_refs = sum(1 for o in owners if o > 1)
    for k, (pf, root) in enumerate(src.refs):
        if (pf & 0x7FFFFFFF) >= len(src.planes):
            P.append(f"reference {k}: plane outside")
            return rep
    for si, s in enumerate(src.surfaces):
        if (s[0] & 0x7FFFFFFF) >= len(src.planes):
            P.append(f"surface {si}: plane outside")
            return rep
        rep.flags[s[2]] += 1
        rep.breakable[s[3]] += 1
        if s[2] & ~KNOWN_FLAGS:
            P.append(f"surface {si}: unknown flag bits {s[2]:#x}")
        m = s[4]
        rep.materials["none" if m == -1 else "ok" if 0 <= m < len(src.palette) else "BAD"] += 1
    if rep.materials["BAD"]:
        P.append(f"{rep.materials['BAD']} surfaces with a material outside the palette")
    # polygons: planarity, winding, convexity, degeneracy
    for si, poly in enumerate(polys):
        pts = [src.vertices[v][:3] for v in poly]
        pl = effective_plane(src, src.surfaces[si][0])
        dev = max(abs(pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] - pl[3]) for p in pts)
        rep.max_plane_dev = max(rep.max_plane_dev, dev)
        rep.max_polygon = max(rep.max_polygon, len(poly))
        nw = newell(pts)
        ln = math.sqrt(sum(c * c for c in nw))
        if ln < 1e-9:
            rep.degenerate += 1
            continue
        if sum(a * b for a, b in zip(nw, pl[:3])) <= 0.0:
            rep.winding_bad += 1
        # convex: every consecutive turn has the same sense about the plane normal
        sense = 0
        for i in range(len(pts)):
            a, b, c = pts[i - 1], pts[i], pts[(i + 1) % len(pts)]
            e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            e2 = (c[0] - b[0], c[1] - b[1], c[2] - b[2])
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            d = cr[0] * pl[0] + cr[1] * pl[1] + cr[2] * pl[2]
            if d < -1e-7:
                sense = -1
        if sense:
            rep.concave += 1
    if rep.max_plane_dev > 5e-3:
        P.append(f"vertices up to {rep.max_plane_dev:.2e} off their surface plane")
    if rep.winding_bad:
        P.append(f"{rep.winding_bad} polygons wound against their plane normal")
    # two-sided surfaces come as coincident pairs of opposite orientation (every one of the 12000 on the retail maps)
    twins: dict[tuple, list[tuple[float, float, float]]] = {}
    for si, poly in enumerate(polys):
        if src.surfaces[si][2] & F_TWO_SIDED:
            rep.two_sided += 1
            key = tuple(sorted(tuple(round(c, 3) for c in src.vertices[v][:3]) for v in poly))
            twins.setdefault(key, []).append(effective_plane(src, src.surfaces[si][0])[:3])
    for group in twins.values():
        for k, n in enumerate(group):
            if not any(m is not n and sum(a * b for a, b in zip(n, m)) < -0.99 for m in group):
                rep.twin_missing += 1
    # vertex first_edge consistency
    for vi, v in enumerate(src.vertices):
        e = v[3]
        if not 0 <= e < len(src.edges) or vi not in (src.edges[e][0], src.edges[e][1]):
            P.append(f"vertex {vi}: first edge {e} does not touch it")
            break
    # references: surfaces reachable, plane / flip agreement, 2D lookups
    reach: list[set[int]] = []
    referenced = [False] * ns
    for k, (pf, root) in enumerate(src.refs):
        found = []
        st = [root]
        while st:
            c = st.pop()
            if c & HI:
                found.append(c & 0x7FFFFFFF)
            elif 0 <= c < len(src.nodes2d):
                st.extend(src.nodes2d[c][3:5])
        reach.append(set(found))
        for s in found:
            if s >= ns:
                P.append(f"reference {k}: surface {s} outside")
                return rep
            referenced[s] = True
            if src.surfaces[s][0] != pf:
                P.append(f"reference {k}: surface {s} lies on plane/flip {src.surfaces[s][0]:#x}, "
                         f"reference says {pf:#x}")
                return rep
    rep.unreferenced = referenced.count(False)
    owned = [False] * ns
    for r in range(nr):
        if owners[r]:
            for sv in reach[r]:
                owned[sv] = True
    rep.unreachable = owned.count(False)
    # leaf flag <-> two-sided
    for k, (fl, count, first) in enumerate(src.leaves):
        two = any(src.surfaces[s][2] & F_TWO_SIDED for r in range(first, first + count) for s in reach[r])
        rep.two_sided_in_leaves += two
        if fl & ~1:
            P.append(f"leaf {k}: unknown flag bits {fl:#x}")
        if bool(fl & 1) != two:
            rep.leaf_flag_bad += 1
    if lookup:
        cache: dict[int, tuple[float, float, float] | None] = {}
        for k, (pf, root) in enumerate(src.refs):
            for s in reach[k]:
                if s not in cache:
                    pts = [src.vertices[v][:3] for v in polys[s]]
                    nn = effective_plane(src, src.surfaces[s][0])[:3]
                    cache[s] = interior_point(pts, nn)
                q = cache[s]
                if q is None:
                    continue
                rep.lookup_checked += 1
                try:
                    if lookup_2d(src, (pf, root), q) != s:
                        rep.lookup_bad += 1
                except CollError:
                    rep.lookup_bad += 1
        if rep.lookup_bad:
            P.append(f"{rep.lookup_bad} of {rep.lookup_checked} 2D lookups of an interior point returned another surface")
    return rep


# ---- the section -------------------------------------------------------------------
def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


@dataclass
class Compact:
    """The source with dead data dropped and everything renumbered for the section: references re-emitted in leaf order
    (one copy per owning leaf; orphan references are gone), 2D nodes in preorder, surfaces no leaf can reach dropped
    (stable order), unused vertices dropped (stable order)."""
    refs: list[tuple[int, int]]                  # (plane | flip << 31, root: HI | new surface or new 2D node)
    leaf_first: list[int]                        # nl + 1 entries
    nodes2d: list[tuple[float, float, float, int, int]]    # children HI | new surface or new 2D node (second, first)
    surf_old: list[int]
    polys: list[tuple[int, ...]]                 # per new surface, new vertex indices
    vert_old: list[int]
    orphan_refs: int = 0
    shared_refs: int = 0
    dropped_surfaces: int = 0
    dropped_vertices: int = 0


def compact(src: Source, polys: list[tuple[int, ...]]) -> Compact:
    nl = len(src.leaves)
    emitted: list[int] = []
    leaf_first: list[int] = []
    owners = [0] * len(src.refs)
    for fl, count, first in src.leaves:
        leaf_first.append(len(emitted))
        if count:
            if first + count > len(src.refs):
                raise CollError("leaf references outside the table")
            for r in range(first, first + count):
                owners[r] += 1
                emitted.append(r)
    leaf_first.append(len(emitted))
    # 2D nodes: preorder over the emitted references; a root shared by several references is numbered once
    new2 = [-1] * len(src.nodes2d)
    order: list[int] = []
    reach: set[int] = set()
    for r in emitted:
        root = src.refs[r][1]
        st = [root]
        while st:
            c = st.pop()
            if c & HI:
                reach.add(c & 0x7FFFFFFF)
                continue
            if new2[c] != -1:
                continue                                # shared root (a node never has two parents, see tree_depths)
            new2[c] = len(order)
            order.append(c)
            _a, _b, _d, c0, c1 = src.nodes2d[c]
            st.append(c1)                               # first child follows its parent: push it last
            st.append(c0)
    surf_old = sorted(reach)
    new_s = {o: i for i, o in enumerate(surf_old)}
    used_v = sorted({v for o in surf_old for v in polys[o]})
    new_v = {o: i for i, o in enumerate(used_v)}

    def ch(c: int) -> int:
        return HI | new_s[c & 0x7FFFFFFF] if c & HI else new2[c]

    refs = [(src.refs[r][0], ch(src.refs[r][1])) for r in emitted]
    nodes2d = [(a, b, d, ch(c0), ch(c1)) for a, b, d, c0, c1 in (src.nodes2d[o] for o in order)]
    return Compact(refs, leaf_first, nodes2d, surf_old, [tuple(new_v[v] for v in polys[o]) for o in surf_old], used_v,
                   owners.count(0), sum(1 for o in owners if o > 1), len(src.surfaces) - len(surf_old),
                   len(src.vertices) - len(used_v))


def build_section(src: Source, polys: list[tuple[int, ...]] | None = None) -> bytes:
    """Serialise one collision BSP. Raises CollError for anything the compact layout cannot hold (the retail data
    never does: see tree_depths / verify_source)."""
    if polys is None:
        polys = surface_polygons(src)
    nn, npl, nl = len(src.nodes), len(src.planes), len(src.leaves)
    depth, stack2d, probs = tree_depths(src)
    if probs:
        raise CollError("; ".join(probs[:3]))
    if depth > STACK_3D or stack2d > STACK_2D:
        raise CollError(f"tree depth {depth} / 2D stack {stack2d} exceed the reader's stacks {STACK_3D} / {STACK_2D}")
    cp = compact(src, polys)
    nr, n2, ns, nv = len(cp.refs), len(cp.nodes2d), len(cp.surf_old), len(cp.vert_old)
    if nn >= MAX_NODES or nl >= MAX_NODES:
        raise CollError(f"{nn} nodes / {nl} leaves exceed the 23-bit child fields")
    if npl > MAX_U15 or n2 > MAX_U15 or ns > MAX_U15:
        raise CollError(f"{npl} planes / {n2} 2D nodes / {ns} surfaces exceed 15 bits")
    if nv > MAX_VERTS:
        raise CollError(f"{nv} vertices exceed u16 indices")
    out = bytearray(HEADER_SIZE)
    offs: dict[str, int] = {}

    def child24(c: int) -> int:
        if c == NO_CHILD:
            return CHILD24_NONE
        return CHILD24_LEAF | (c & ~HI) if c & HI else c

    offs["nodes"] = len(out)
    for pl, back, front in src.nodes:
        out += struct.pack("<II", child24(front) | (pl & 0xFF) << 24, child24(back) | (pl >> 8) << 24)
    _pad16(out)
    offs["planes"] = len(out)
    for p in src.planes:
        out += struct.pack("<4f", *p)
    _pad16(out)
    offs["leaves"] = len(out)
    for k, (fl, count, first) in enumerate(src.leaves):
        out += struct.pack("<I", cp.leaf_first[k] | (LEAF_TWO_SIDED if fl & 1 else 0))
    out += struct.pack("<I", nr)                   # sentinel: end of the last leaf
    _pad16(out)

    def root16(c: int) -> int:
        return 0x8000 | (c & 0x7FFFFFFF) if c & HI else c

    offs["refs"] = len(out)
    for pf, root in cp.refs:
        out += struct.pack("<HH", (pf & 0x7FFFFFFF) | (0x8000 if pf & HI else 0), root16(root))
    _pad16(out)
    offs["nodes2d"] = len(out)
    for a, b, d, c0, c1 in cp.nodes2d:
        out += struct.pack("<3fHH", a, b, d, root16(c1), root16(c0))     # front (side >= 0) first, then back
    _pad16(out)
    offs["surfaces"] = len(out)
    index_data = bytearray()
    first = 0
    for i, old in enumerate(cp.surf_old):
        s = src.surfaces[old]
        poly = cp.polys[i]
        if not 3 <= len(poly) <= 15:
            raise CollError(f"surface {old}: {len(poly)} vertices (3..15 fit)")
        mat = NO_MATERIAL
        if 0 <= s[4] < len(src.palette):
            mat = src.palette[s[4]]
            if mat > 254:
                raise CollError(f"surface {old}: material type {mat} does not fit u8")
        brk = s[3] if s[2] & F_BREAKABLE else 0
        if not 0 <= brk <= 31 or s[2] & ~KNOWN_FLAGS:
            raise CollError(f"surface {old}: breakable id {s[3]} / flags {s[2]:#x} do not fit")
        pts = [src.vertices[cp.vert_old[v]][:3] for v in poly]
        if _concave(pts, effective_plane(src, s[0])[:3]):
            raise CollError(f"surface {old}: concave polygon (the reader's polygon tests need convex ones)")
        out += struct.pack("<IHBB", first | len(poly) << 20 | brk << 24,
                           (s[0] & 0x7FFFFFFF) | (0x8000 if s[0] & HI else 0), s[2], mat)
        index_data += struct.pack(f"<{len(poly)}H", *poly)
        first += len(poly)
    if first > MAX_INDICES:
        raise CollError("polygon index table too large")
    _pad16(out)
    offs["indices"] = len(out)
    out += index_data
    _pad16(out)
    offs["vertices"] = len(out)
    for o in cp.vert_old:
        out += struct.pack("<3f", *src.vertices[o][:3])
    _pad16(out)
    lo = [min(src.vertices[o][i] for o in cp.vert_old) for i in range(3)] if nv else [0.0] * 3
    hi = [max(src.vertices[o][i] for o in cp.vert_old) for i in range(3)] if nv else [0.0] * 3
    counts = {"nodes": nn, "planes": npl, "leaves": nl, "refs": nr, "nodes2d": n2, "surfaces": ns,
              "indices": first, "vertices": nv}
    struct.pack_into("<4sHHII6fII", out, 0, MAGIC, VERSION, 0, len(out), depth, *lo, *hi, stack2d, 0)
    for i, name in enumerate(TABLES):
        struct.pack_into("<II", out, DIR_AT + 8 * i, offs[name], counts[name])
    return bytes(out)


def _concave(pts: list[tuple[float, float, float]], n: tuple[float, float, float]) -> bool:
    """True when a polygon (counter-clockwise about ``n``) turns the wrong way at some vertex (same threshold as verify)."""
    for i in range(len(pts)):
        a, b, c = pts[i - 1], pts[i], pts[(i + 1) % len(pts)]
        e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        e2 = (c[0] - b[0], c[1] - b[1], c[2] - b[2])
        cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] < -1e-7:
            return True
    return False


@dataclass
class Section:
    size: int
    depth: int
    stack2d: int
    bounds: tuple[float, ...]
    nodes: list[tuple[int, int, int]]            # (plane, front, back) with NO_CHILD / HI | leaf / node
    planes: list[tuple[float, float, float, float]]
    leaves: list[tuple[bool, int, int]]          # (two-sided flag, first ref, ref count)
    refs: list[tuple[int, int, int]]             # (plane, flip, root); root is HI | surface or a 2D node
    nodes2d: list[tuple[float, float, float, int, int]]    # a, b, d, front, back (HI | surface or node)
    surfaces: list[dict]
    vertices: list[tuple[float, float, float]]


def parse_section(data: bytes) -> Section:
    """Reference reader (the C reader is engine/src/world/coll.c); raises CollError on a bad section."""
    if len(data) < HEADER_SIZE:
        raise CollError("section shorter than the header")
    magic, version, _r, size, depth, *rest = struct.unpack_from("<4sHHII6fII", data)
    lo_hi, stack2d = rest[:6], rest[6]
    if magic != MAGIC:
        raise CollError("bad signature")
    if version != VERSION:
        raise CollError(f"unsupported version {version}")
    if size != len(data):
        raise CollError("inconsistent size")
    d = {}
    for i, name in enumerate(TABLES):
        d[name] = struct.unpack_from("<II", data, DIR_AT + 8 * i)
    recs = dict(nodes=NODE_REC, planes=PLANE_REC, leaves=LEAF_REC, refs=REF_REC, nodes2d=N2_REC, surfaces=SURF_REC,
                indices=INDEX_REC, vertices=VERT_REC)
    for name, (off, cnt) in d.items():
        n = cnt + (1 if name == "leaves" else 0)
        if off % 16 or off < HEADER_SIZE or off + n * recs[name] > len(data):
            raise CollError(f"{name} table outside the section")
    nodes = []
    for i in range(d["nodes"][1]):
        w0, w1 = struct.unpack_from("<II", data, d["nodes"][0] + i * NODE_REC)
        pl = (w0 >> 24) | (w1 >> 24) << 8

        def dec(c: int) -> int:
            return NO_CHILD if c == CHILD24_NONE else (HI | (c & 0x7FFFFF)) if c & CHILD24_LEAF else c
        nodes.append((pl, dec(w0 & 0xFFFFFF), dec(w1 & 0xFFFFFF)))
    planes = list(struct.iter_unpack("<4f", data[d["planes"][0]:d["planes"][0] + d["planes"][1] * PLANE_REC]))
    lw = struct.unpack_from(f"<{d['leaves'][1] + 1}I", data, d["leaves"][0])
    leaves = [(bool(lw[i] & LEAF_TWO_SIDED), lw[i] & 0x7FFFFFFF, (lw[i + 1] & 0x7FFFFFFF) - (lw[i] & 0x7FFFFFFF))
              for i in range(d["leaves"][1])]
    refs = []
    for i in range(d["refs"][1]):
        pf, root = struct.unpack_from("<HH", data, d["refs"][0] + i * REF_REC)
        refs.append((pf & 0x7FFF, pf >> 15, HI | (root & 0x7FFF) if root & 0x8000 else root))
    n2 = []
    for i in range(d["nodes2d"][1]):
        a, b, dd, c1, c0 = struct.unpack_from("<3fHH", data, d["nodes2d"][0] + i * N2_REC)
        n2.append((a, b, dd, HI | (c1 & 0x7FFF) if c1 & 0x8000 else c1, HI | (c0 & 0x7FFF) if c0 & 0x8000 else c0))
    idx = struct.unpack_from(f"<{d['indices'][1]}H", data, d["indices"][0])
    surfs = []
    for i in range(d["surfaces"][1]):
        w, pf, fl, mat = struct.unpack_from("<IHBB", data, d["surfaces"][0] + i * SURF_REC)
        first, nv, brk = w & 0xFFFFF, (w >> 20) & 15, (w >> 24) & 31
        if first + nv > len(idx):
            raise CollError(f"surface {i}: polygon outside the index table")
        surfs.append(dict(plane=pf & 0x7FFF, flip=pf >> 15, flags=fl, material=mat, breakable=brk,
                          poly=tuple(idx[first:first + nv])))
    verts = list(struct.iter_unpack("<3f", data[d["vertices"][0]:d["vertices"][0] + d["vertices"][1] * VERT_REC]))
    return Section(size, depth, stack2d, tuple(lo_hi), nodes, planes, leaves, refs, n2, surfs, verts)


def section_tag(index: int) -> str:
    """Pack tag of BSP ``index``: COL0..COL9, then COLA..COLZ (same index order as BSPn)."""
    if not 0 <= index < 36:
        raise CollError(f"BSP index {index} does not fit a four-character tag")
    return "COL" + "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[index]


def convert_block(block: bytes, loc: bsp.BspLocation) -> bytes:
    src = read_source(block, loc)
    return build_section(src)


def _blocks(path: Path):
    hdr = mapinfo.read_header(path)
    idx = tagindex.TagIndex.read(path)
    with path.open("rb") as f:
        f.seek(hdr.tag_data_offset)
        region = f.read(hdr.tag_data_size)
        locs = bsp.locate(idx, region, hdr.decompressed_size)
        for i, loc in enumerate(locs):
            f.seek(loc.file_offset)
            yield i, loc, f.read(loc.size)


def convert_map(path: Path, bsp_indices: list[int] | None = None) -> list[tuple[str, bytes]]:
    out = []
    for i, loc, block in _blocks(path):
        if bsp_indices is not None and i not in bsp_indices:
            continue
        out.append((section_tag(i), convert_block(block, loc)))
    return out


def describe(tag: str, data: bytes) -> str:
    s = parse_section(data)
    fl = Counter(x["flags"] for x in s.surfaces)
    return (f"{tag}: {len(data) / 2**20:.2f} MiB, {len(s.nodes)} nodes (depth {s.depth}), {len(s.planes)} planes, "
            f"{len(s.leaves)} leaves, {len(s.refs)} refs, {len(s.nodes2d)} 2D nodes (stack {s.stack2d}), "
            f"{len(s.surfaces)} surfaces {dict(sorted(fl.items()))}, {len(s.vertices)} vertices")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--bsp", type=int, action="append", help="only this BSP index (repeatable)")
    ap.add_argument("--info", action="store_true", help="input is a .hp2: print its COL sections")
    ap.add_argument("--verify", action="store_true", help="input is a map: run every source check, print the report")
    args = ap.parse_args(argv)
    if args.info:
        pk = hp2.parse(args.input.read_bytes())
        for s in pk.sections:
            if s.tag.startswith("COL"):
                print(describe(s.tag, pk.read(s.tag)))
        return 0
    if args.verify:
        rc = 0
        for i, loc, block in _blocks(args.input):
            if args.bsp and i not in args.bsp:
                continue
            rep = verify_source(read_source(block, loc))
            print(f"{args.input.stem} {i} {rep.name}: depth {rep.depth3d} stack2d {rep.stack2d} polygon<={rep.max_polygon} "
                  f"concave {rep.concave} degenerate {rep.degenerate} planedev {rep.max_plane_dev:.1e} "
                  f"lookups {rep.lookup_checked - rep.lookup_bad}/{rep.lookup_checked} "
                  f"leafflag-bad {rep.leaf_flag_bad} two-sided {rep.two_sided} (untwinned {rep.twin_missing}) flags {dict(sorted(rep.flags.items()))} "
                  f"breakable {dict(rep.breakable)} materials {dict(rep.materials)}")
            for p in rep.problems:
                print("  problem:", p, file=sys.stderr)
                rc = 1
        return rc
    if not args.out:
        ap.error("-o is required")
    secs = convert_map(args.input, args.bsp)
    for tag, blob in secs:
        if len(blob) > 1.5 * 2**20:
            print(f"note: {args.input.stem} {tag}: {len(blob) / 2**20:.2f} MiB raw is above the 1.5 MiB target",
                  file=sys.stderr)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, secs))
    print(f"wrote {args.out} ({sum(len(b) for _, b in secs) / 2**20:.2f} MiB raw in {len(secs)} sections)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
