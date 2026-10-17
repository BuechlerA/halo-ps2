"""COLn conversion on a synthetic collision BSP (no game data needed): two cuboid rooms joined by a two-sided pane,
built as a complete Xbox structure_bsp block (winged edges, 2D BSPs, shared and orphan references), then read,
verified, compacted, serialised, parsed back and attacked. Set COLL_FIXTURE=<path> to also write the .hp2 the C reader
test (engine/tests/test_coll.c, which hard-codes the numbers of this world) opens. A skip-if-missing check converts
every BSP of the real a10 map (set COLL_ALL_MAPS=1 for all ten campaign maps).

The world (units are arbitrary): room R = x 0..10, room L = x -10..0, both y -10..10, z 0..5, faces looking inward.
Both rooms are closed, subdivided cuboids with their own vertices (so every edge has exactly two surfaces); the face
at x = 0 exists twice, as in the real data: PR (+x normal) listed by the right leaf and its coincident twin PL (-x normal)
listed by the left leaf, both flagged two-sided (a glass pane). A small closed cuboid "D" that no leaf lists (one of its
faces is only named by an orphan reference) tests the dropping of dead surfaces and vertices. Grid lines:
x {-10, -4, 0, 10}, y {-10, 3, 10}, z {0, 5}. Tree: node 0 = plane x = 0 (front: right room chain 1..5, back: left room
chain 6..10), each chain peels the five wall planes (front continues, back is solid) and ends in leaf 1 (R) / leaf 0 (L)."""
import math
import os
import random
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import bsp, collconv as cc, hp2  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
LOAD = 0x82000000
HI = cc.HI
NONE = cc.NO_CHILD
GX, GY, GZ = (-10.0, -4.0, 0.0, 10.0), (-10.0, 3.0, 10.0), (0.0, 5.0)
# palette: material types of the three collision materials of the world (stone, metal thick, glass)
PALETTE = [2, 7, 9]


class Mesh:
    """Polygons over a vertex table; builds the winged-edge records the Xbox structure uses."""

    def __init__(self) -> None:
        self.verts: list[tuple[float, float, float]] = []
        self.polys: list[dict] = []

    def vert(self, p, group: int) -> int:
        key = (group, p)
        if not hasattr(self, "_v"):
            self._v: dict = {}
        if key not in self._v:
            self._v[key] = len(self.verts)
            self.verts.append(p)
        return self._v[key]

    def edges(self) -> tuple[list[tuple[int, int, int, int, int, int]], list[int]]:
        """(edges, first edge per polygon): every undirected edge is shared by exactly two polygons."""
        directed: dict[tuple[int, int], tuple[int, int]] = {}      # (u, v) -> (polygon, position in the loop)
        for pi, poly in enumerate(self.polys):
            loop = poly["loop"]
            for i, u in enumerate(loop):
                directed[(u, loop[(i + 1) % len(loop)])] = (pi, i)
        index: dict[tuple[int, int], int] = {}
        edges: list[list[int]] = []
        for (u, v), (pi, i) in sorted(directed.items(), key=lambda kv: kv[1]):
            if (v, u) in index:
                continue
            twin = directed.get((v, u))
            assert twin is not None, f"edge {u}-{v} has no twin: the mesh is not closed"
            index[(u, v)] = len(edges)
            edges.append([u, v, -1, -1, pi, twin[0]])
        for (u, v), e in list(index.items()):
            pi, i = directed[(u, v)]
            loop = self.polys[pi]["loop"]
            nxt = (loop[(i + 1) % len(loop)], loop[(i + 2) % len(loop)])
            edges[e][2] = index.get(nxt, index.get((nxt[1], nxt[0])))
            qi, j = directed[(v, u)]
            loop2 = self.polys[qi]["loop"]
            nxt2 = (loop2[(j + 1) % len(loop2)], loop2[(j + 2) % len(loop2)])
            edges[e][3] = index.get(nxt2, index.get((nxt2[1], nxt2[0])))
        first = []
        for pi, poly in enumerate(self.polys):
            u, v = poly["loop"][0], poly["loop"][1]
            first.append(index.get((u, v), index.get((v, u))))
        return [tuple(e) for e in edges], first    # type: ignore[misc]


def axis_normal(axis: int, sign: int) -> tuple[float, float, float]:
    n = [0.0, 0.0, 0.0]
    n[axis] = float(sign)
    return tuple(n)    # type: ignore[return-value]


def uv_axes(axis: int, flip: int) -> tuple[int, int]:
    """2D coordinate axes of a reference on the plane ``axis`` stored with a +axis normal (hand-derived from the
    rule in collconv.py: cyclic order when the oriented normal points to +axis, swapped otherwise)."""
    u, v = (axis + 1) % 3, (axis + 2) % 3
    return (v, u) if flip else (u, v)


def make_world():
    """Returns (block, loc, info): the Xbox structure_bsp block and the facts the tests check."""
    mesh = Mesh()
    planes: list[tuple[float, float, float, float]] = []
    plane_ix: dict[tuple[int, float], int] = {}

    def plane(axis: int, d: float) -> int:
        if (axis, d) not in plane_ix:
            plane_ix[(axis, d)] = len(planes)
            planes.append((*axis_normal(axis, +1), d))
        return plane_ix[(axis, d)]

    cells: list[dict] = []      # every surface: cell rectangle in the plane, which leaf lists it

    def face(group: int, axis: int, d: float, sign: int, ranges: dict[int, list[float]], leaf, flags=0, mat=0, brk=0,
             name=""):
        """A cuboid face on plane axis = d, effective normal sign * axis, subdivided by the grid ranges of the other
        two axes (cells in ascending order)."""
        a1, a2 = (axis + 1) % 3, (axis + 2) % 3
        g1, g2 = ranges[a1], ranges[a2]
        for i in range(len(g1) - 1):
            for j in range(len(g2) - 1):
                corners = []
                for (p, q) in ((g1[i], g2[j]), (g1[i + 1], g2[j]), (g1[i + 1], g2[j + 1]), (g1[i], g2[j + 1])):
                    pt = [0.0, 0.0, 0.0]
                    pt[axis], pt[a1], pt[a2] = d, p, q
                    corners.append(tuple(pt))
                want = axis_normal(axis, sign)
                nw = cc.newell(corners)
                if sum(x * y for x, y in zip(nw, want)) < 0:
                    corners.reverse()
                loop = [mesh.vert(c, group) for c in corners]
                cells.append(dict(loop=loop, axis=axis, d=d, sign=sign, rect=(g1[i], g1[i + 1], g2[j], g2[j + 1]),
                                  axes=(a1, a2), leaf=leaf, flags=flags, mat=mat, brk=brk, name=name,
                                  poly_index=len(mesh.polys)))
                mesh.polys.append(dict(loop=loop))

    L, R = 0, 1
    # right room (group 1): leaf 1 lists everything except nothing; pane PR is two-sided, glass
    xr, yr, zr = (0.0, 10.0), GY, GZ
    face(1, 0, 0.0, +1, {1: yr, 2: zr}, R, flags=1, mat=2, name="PR")
    face(1, 0, 10.0, -1, {1: yr, 2: zr}, R, name="WXR")
    face(1, 1, -10.0, +1, {0: xr, 2: zr}, R, flags=4, mat=1, name="WY0R")           # climbable, metal
    face(1, 1, 10.0, -1, {0: xr, 2: zr}, R, flags=2, mat=1, name="WY1R")            # invisible, metal
    face(1, 2, 0.0, +1, {0: xr, 1: yr}, R, name="FR")
    face(1, 2, 5.0, -1, {0: xr, 1: yr}, R, name="CR")
    # left room (group 0): leaf 0; PL is the twin of PR
    xl = (-10.0, -4.0, 0.0)
    face(0, 0, 0.0, -1, {1: yr, 2: zr}, L, flags=1, mat=2, name="PL")
    face(0, 0, -10.0, +1, {1: yr, 2: zr}, L, name="WXL")
    face(0, 1, -10.0, +1, {0: xl, 2: zr}, L, name="WY0L")
    face(0, 1, 10.0, -1, {0: xl, 2: zr}, L, name="WY1L")
    face(0, 2, 0.0, +1, {0: xl, 1: yr}, L, name="FL")
    face(0, 2, 5.0, -1, {0: xl, 1: yr}, L, brk=5, flags=8, name="CL")                # breakable group 5
    # a dead cuboid (group 2), listed by no leaf, outward normals
    for axis, lo_, hi_ in ((0, 2.0, 3.0), (1, 2.0, 3.0), (2, 2.0, 3.0)):
        a1, a2 = (axis + 1) % 3, (axis + 2) % 3
        rng = {a1: (2.0, 3.0), a2: (2.0, 3.0)}
        face(2, axis, lo_, -1, rng, None, name="D")
        face(2, axis, hi_, +1, rng, None, name="D")
    for c in cells:
        c["plane"] = plane(c["axis"], c["d"])
        c["flip"] = 1 if c["sign"] < 0 else 0
    edges, first_edge = mesh.edges()
    surfaces = [((c["plane"] | (HI if c["flip"] else 0)), first_edge[c["poly_index"]], c["flags"], c["brk"], c["mat"])
                for c in cells]

    # ---- 2D BSPs (post-order numbering: children before parents, to exercise the converter's renumbering)
    n2: list[tuple[float, float, float, int, int]] = []

    def build2d(cs: list[dict], ua: int, va: int, axes: tuple[int, int]) -> int:
        if len(cs) == 1:
            return HI | cs[0]["surf"]
        for (pos, which) in ((0, 0), (1, 1)):                      # cut along u (rect[0..1]) or v (rect[2..3])
            vals = sorted({r for c in cs for r in c["rect"][2 * which:2 * which + 2]})
            for cut in vals[1:-1]:
                lo = [c for c in cs if c["rect"][2 * which + 1] <= cut + 1e-9]
                hi = [c for c in cs if c["rect"][2 * which] >= cut - 1e-9]
                if len(lo) + len(hi) == len(cs) and lo and hi:
                    # which = 0: rect coordinates run along axes[0], else axes[1]; line coefficient on (u, v)
                    axis_cut = axes[which]
                    a, b = (1.0, 0.0) if ua == axis_cut else (0.0, 1.0)
                    c0 = build2d(lo, ua, va, axes)
                    c1 = build2d(hi, ua, va, axes)
                    n2.append((a, b, cut, c0, c1))
                    return len(n2) - 1
        raise AssertionError("no guillotine cut")

    for i, c in enumerate(cells):
        c["surf"] = i
    refs: list[tuple[int, int]] = []
    leaf_refs: dict[int, list[int]] = {L: [], R: []}
    ref_of: dict[tuple, int] = {}

    def add_ref(key, cs: list[dict]) -> int:
        axis, d, sign = key
        flip = 1 if sign < 0 else 0
        ua, va = uv_axes(axis, flip)
        root = build2d(cs, ua, va, cs[0]["axes"])
        refs.append((plane(axis, d) | (HI if flip else 0), root))
        return len(refs) - 1

    # leaf lists: R first then L (R = [0, 6) plus the shared ref, L overlaps it); orphan last
    def cells_of(leaf, axis, d, sign):
        return [c for c in cells if c["leaf"] == leaf and c["axis"] == axis and c["d"] == d and c["sign"] == sign]

    shared_key = (1, 10.0, -1)
    shared_cells = cells_of(L, *shared_key) + cells_of(R, *shared_key)
    keys_R = [(0, 0.0, +1), (0, 10.0, -1), (1, -10.0, +1), (2, 0.0, +1), (2, 5.0, -1)]
    keys_L = [(0, -10.0, +1), (1, -10.0, +1), (2, 0.0, +1), (2, 5.0, -1), (0, 0.0, -1)]
    # R: pane, x=10 wall, y=-10 face, floor, ceiling, then the shared y=10 face; L: the shared ref, then its own
    r_ids = [add_ref(k, cells_of(R, *k)) for k in keys_R]
    shared = add_ref(shared_key, shared_cells)
    l_ids = [add_ref(k, cells_of(L, *k)) for k in keys_L]
    # shared_cells has a single y=10 face for R (x 0..10) and two for L: one tree for all three cells.
    dead = next(i for i, c in enumerate(cells) if c["name"] == "D")
    refs.append((plane(0, 2.0) | HI, HI | dead))                       # orphan: no leaf lists it (x = 2 face, outward -x)
    # leaf ranges must be contiguous: reorder the reference table so R = [pane..ceiling, shared], L = [shared, own...]
    order = r_ids + [shared] + l_ids + [len(refs) - 1]
    assert sorted(order) == list(range(len(refs)))
    new_index = {old: i for i, old in enumerate(order)}
    refs = [refs[old] for old in order]
    leaves = [(1, 1 + len(l_ids), new_index[shared]), (1, len(r_ids) + 1, new_index[r_ids[0]])]
    assert leaves[0][2] == len(r_ids) and leaves[1][2] == 0

    # ---- the 3D tree (plane, back, front as stored by the Xbox)
    def pl(axis: int, d: float) -> int:
        return plane(axis, d)

    LEAFF = HI
    nodes = [
        (pl(0, 0.0), 6, 1),                       # 0: x = 0, front -> right chain, back -> left chain
        (pl(0, 10.0), 2, NONE),                   # 1: x = 10: front (x >= 10) solid
        (pl(1, -10.0), NONE, 3),                  # 2: y = -10: back solid
        (pl(1, 10.0), 4, NONE),                   # 3: y = 10
        (pl(2, 0.0), NONE, 5),                    # 4: z = 0
        (pl(2, 5.0), LEAFF | 1, NONE),            # 5: z = 5 -> leaf 1 (R)
        (pl(0, -10.0), NONE, 7),                  # 6: x = -10 (left chain)
        (pl(1, -10.0), NONE, 8),                  # 7
        (pl(1, 10.0), 9, NONE),                   # 8
        (pl(2, 0.0), NONE, 10),                   # 9
        (pl(2, 5.0), LEAFF | 0, NONE),            # 10 -> leaf 0 (L)
    ]
    # ---- the block
    b = bytearray()

    def alloc(data: bytes, align: int = 4) -> int:
        while len(b) % align:
            b.append(0)
        addr = LOAD + len(b)
        b.extend(data)
        return addr

    alloc(bytes(bsp.HEADER_SIZE))
    root_addr = alloc(bytes(0x27C), 16)
    root = root_addr - LOAD
    pal = bytearray()
    for i, t in enumerate(PALETTE):
        pal += struct.pack("<4sIII", b"vnes", 0, 0, 0xE1000000 + i) + struct.pack("<HH", 0, t)
    cb_tables = [
        (len(nodes), alloc(b"".join(struct.pack("<3I", *n) for n in nodes))),
        (len(planes), alloc(b"".join(struct.pack("<4f", *p) for p in planes))),
        (len(leaves), alloc(b"".join(struct.pack("<HHI", f, c, first) for f, c, first in leaves))),
        (len(refs), alloc(b"".join(struct.pack("<II", *r) for r in refs))),
        (len(n2), alloc(b"".join(struct.pack("<3fII", *n) for n in n2))),
        (len(surfaces), alloc(b"".join(struct.pack("<IiBbh", *s) for s in surfaces))),
        (len(edges), alloc(b"".join(struct.pack("<6i", *e) for e in edges))),
    ]
    # vertex first_edge: any edge touching the vertex
    vfirst = {}
    for ei, e in enumerate(edges):
        vfirst.setdefault(e[0], ei)
        vfirst.setdefault(e[1], ei)
    cb_tables.append((len(mesh.verts), alloc(b"".join(struct.pack("<3fi", *v, vfirst[i])
                                                       for i, v in enumerate(mesh.verts)))))
    cb = bytearray(96)
    for k, (n, a) in enumerate(cb_tables):
        struct.pack_into("<II", cb, 12 * k, n, a)
    cb_addr = alloc(bytes(cb))
    struct.pack_into("<II", b, root + 0xB0, 1, cb_addr)
    struct.pack_into("<II", b, root + 0xA4, len(PALETTE), alloc(bytes(pal)))
    struct.pack_into("<5I4s", b, 0, root_addr, 0, 0, 0, 0, bsp.SBSP_MAGIC)
    loc = bsp.BspLocation(0x800, len(b), LOAD, "levels\\t\\coll")
    names = [c["name"] for c in cells]
    info = dict(cells=cells, names=names, nodes=nodes, planes=planes, leaves=leaves, refs=refs, n2=n2,
                surfaces=surfaces, edges=edges, mesh=mesh, shared_ref=new_index[shared], orphan_ref=len(refs) - 1)
    return bytes(b), loc, info


def point_on(c: dict, u: float, v: float) -> tuple[float, float, float]:
    p = [0.0, 0.0, 0.0]
    p[c["axis"]] = c["d"]
    p[c["axes"][0]], p[c["axes"][1]] = u, v
    return tuple(p)    # type: ignore[return-value]


def poke(block: bytes, off: int, fmt: str, *vals) -> bytes:
    b = bytearray(block)
    struct.pack_into(fmt, b, off, *vals)
    return bytes(b)


def expect(fn, what: str) -> None:
    try:
        fn()
    except cc.CollError:
        return
    raise SystemExit(f"accepted: {what}")


# ---- tests ---------------------------------------------------------------------------------------
def check_source() -> None:
    block, loc, info = make_world()
    src = cc.read_source(block, loc)
    assert (len(src.nodes), len(src.planes), len(src.leaves), len(src.refs)) == (11, 13, 2, 12), \
        (len(src.nodes), len(src.planes), len(src.leaves), len(src.refs))
    assert len(src.surfaces) == 32 and src.palette == PALETTE and src.name == "coll"
    polys = cc.surface_polygons(src)
    assert len(polys) == 32 and all(len(p) == 4 for p in polys)
    rep = cc.verify_source(src)
    assert not rep.problems, rep.problems
    assert rep.lookup_bad == 0 and rep.lookup_checked > 20
    assert rep.leaf_flag_bad == 0 and rep.two_sided_in_leaves == 2 and rep.two_sided == 4 and rep.twin_missing == 0
    assert (rep.orphan_refs, rep.shared_refs, rep.unreferenced) == (1, 1, 5), (rep.orphan_refs, rep.shared_refs, rep.unreferenced)
    assert rep.unreachable == 6 and rep.max_plane_dev < 1e-9 and rep.concave == 0 and rep.degenerate == 0
    assert rep.depth3d == 6 and rep.stack2d == 2, (rep.depth3d, rep.stack2d)
    # point location: rooms, pane plane (ties go to the front child), solid space
    for p, want in (((5, 0, 2.5), 1), ((-5, 0, 2.5), 0), ((0.0, 0, 2.5), 1), ((-20, 0, 2.5), -1), ((5, 0, -1), -1),
                    ((5, 11, 2.5), -1), ((10.0, 0, 2.5), -1), ((9.999, 0, 2.5), 1)):
        assert cc.locate(src, p) == want, (p, want)
    # 2D lookups at the cell centres return the cell, for every cell a reference's tree reaches (both rooms'
    # ceilings run with swapped axes, the shared y = 10 face has one tree for three cells)
    reached = set()
    for ri, (pf, rt) in enumerate(src.refs):
        stack, reach = [rt], []
        while stack:
            c = stack.pop()
            if c & HI:
                reach.append(c & 0x7FFFFFFF)
            else:
                stack.extend(src.nodes2d[c][3:5])
        for ci in reach:
            c = info["cells"][ci]
            assert (c["plane"] | (HI if c["flip"] else 0)) == pf
            r = c["rect"]
            got = cc.lookup_2d(src, (pf, rt), point_on(c, (r[0] + r[1]) / 2, (r[2] + r[3]) / 2))
            assert got == ci, f"2D lookup of cell {c['name']}: {got} != {ci}"
            reached.add(ci)
    assert len(reached) == 27                                    # D: one cell by the orphan reference, five by none
    assert [info["cells"][i]["name"] for i in range(32) if i not in reached] == ["D"] * 5
    # projection rule: ties go to the later axis; the flip bit swaps
    assert cc.project_axes((1, 0, 0), 0) == (1, 2) and cc.project_axes((1, 0, 0), 1) == (2, 1)
    assert cc.project_axes((0, 0, 1), 0) == (0, 1) and cc.project_axes((0, 0, -1), 0) == (1, 0)
    assert cc.project_axes((0, 1, 0), 0) == (2, 0) and cc.project_axes((0, -1, 0), 0) == (0, 2)
    s = math.sqrt(0.5)
    assert cc.project_axes((s, s, 0), 0) == (2, 0)              # tie x/y -> y (later axis)
    assert cc.project_axes((s, 0, s), 0) == (0, 1)              # tie x/z -> z
    assert cc.project_axes((0, s, s), 0) == (0, 1)              # tie y/z -> z


def check_source_attacks() -> None:
    block, loc, info = make_world()
    src = cc.read_source(block, loc)
    root = struct.unpack_from("<I", block, 0)[0] - LOAD
    cb = struct.unpack_from("<I", block, root + 0xB0 + 4)[0] - LOAD
    ptr = [struct.unpack_from("<I", block, cb + 12 * k + 4)[0] - LOAD for k in range(8)]
    # reader: tables outside the block, wrong reflexive count, missing signature
    expect(lambda: cc.read_source(poke(block, root + 0xB0, "<I", 2), loc), "two collision bsps")
    expect(lambda: cc.read_source(poke(block, cb + 4, "<I", LOAD + 10 ** 7), loc), "nodes outside")
    expect(lambda: cc.read_source(poke(block, 0x14, "4s", b"nope"), loc), "signature")
    expect(lambda: cc.read_source(poke(block, root + 0xA4 + 4, "<I", LOAD + 10 ** 7), loc), "palette outside")
    # polygons: edge chain that does not close / border / use
    e0 = ptr[6]
    expect(lambda: cc.surface_polygons(cc.read_source(poke(block, e0 + 8, "<i", 0), loc)), "forward edge pointing elsewhere")
    expect(lambda: cc.surface_polygons(cc.read_source(poke(block, ptr[5] + 4, "<i", 9999), loc)), "first edge outside")
    expect(lambda: cc.surface_polygons(cc.read_source(poke(block, ptr[5] + 4, "<i", 5), loc)), "wrong first edge")
    # verify reports (does not raise) geometry and structure problems
    base = cc.verify_source(src)
    assert not base.problems
    v = list(src.vertices)
    v[0] = (v[0][0] + 0.5, v[0][1], v[0][2], v[0][3])
    bad = cc.Source(**{**src.__dict__, "vertices": v})
    assert any("off their surface plane" in p for p in cc.verify_source(bad).problems)
    s2 = list(src.surfaces)
    s2[3] = (s2[3][0] ^ HI, *s2[3][1:])
    bad = cc.Source(**{**src.__dict__, "surfaces": s2})
    assert any("wound against" in p or "plane/flip" in p for p in cc.verify_source(bad).problems)
    lv = list(src.leaves)
    lv[1] = (lv[1][0] & ~1, lv[1][1], lv[1][2])
    bad = cc.Source(**{**src.__dict__, "leaves": lv})
    assert cc.verify_source(bad).leaf_flag_bad == 1
    nodes = list(src.nodes)
    nodes[1] = (nodes[1][0], 1, nodes[1][2])
    bad = cc.Source(**{**src.__dict__, "nodes": nodes})
    assert any("not a later node" in p for p in cc.verify_source(bad).problems)
    nodes = list(src.nodes)
    nodes[2] = (nodes[2][0], nodes[2][1], 3 if nodes[2][2] != 3 else 4)
    bad = cc.Source(**{**src.__dict__, "nodes": nodes})
    assert any("parent" in p for p in cc.verify_source(bad).problems)
    n2 = list(src.nodes2d)
    n2[0] = (*n2[0][:3], HI | 999, n2[0][4])
    bad = cc.Source(**{**src.__dict__, "nodes2d": n2})
    assert any("surface" in p for p in cc.verify_source(bad).problems)
    refs = list(src.refs)
    refs[0] = (refs[0][0] ^ HI, refs[0][1])
    bad = cc.Source(**{**src.__dict__, "refs": refs})
    assert any("plane/flip" in p for p in cc.verify_source(bad).problems)
    s3 = list(src.surfaces)
    i_pl = next(i for i, c in enumerate(info["cells"]) if c["name"] == "PL")
    s3[i_pl] = (s3[i_pl][0], s3[i_pl][1], 0, 0, s3[i_pl][4])           # PL loses its flag: PR has no twin left
    bad = cc.Source(**{**src.__dict__, "surfaces": s3})
    assert cc.verify_source(bad).twin_missing == 1
    # the builder refuses what the layout cannot hold
    pols = cc.surface_polygons(src)
    big = list(pols)
    big[0] = tuple(range(16))
    expect(lambda: cc.build_section(src, big), "16-gon")
    conc = list(pols)
    p0 = conc[0]
    conc[0] = (p0[0], p0[2], p0[1], p0[3])                       # bow-tie / reversed turn
    expect(lambda: cc.build_section(src, conc), "concave polygon")
    expect(lambda: cc.build_section(cc.Source(**{**src.__dict__, "palette": [300, 1, 2]}), pols), "material type 300")
    brk = list(src.surfaces)
    brk[0] = (brk[0][0], brk[0][1], 8, 40, brk[0][4])
    expect(lambda: cc.build_section(cc.Source(**{**src.__dict__, "surfaces": brk}), pols), "breakable id 40")


def lookup_section(sec: cc.Section, ref: tuple[int, int, int], p) -> int:
    pl = sec.planes[ref[0]]
    ua, va = cc.project_axes(pl[:3], ref[1])
    c = ref[2]
    u, v = p[ua], p[va]
    for _ in range(len(sec.nodes2d) + 1):
        if c & HI:
            return c & 0x7FFFFFFF
        a, b, d, fr, bk = sec.nodes2d[c]
        c = fr if a * u + b * v - d >= 0 else bk
    raise AssertionError("loop")


def check_build() -> bytes:
    block, loc, info = make_world()
    src = cc.read_source(block, loc)
    polys = cc.surface_polygons(src)
    cp = cc.compact(src, polys)
    assert (cp.orphan_refs, cp.shared_refs, cp.dropped_surfaces) == (1, 1, 6), (cp.orphan_refs, cp.shared_refs, cp.dropped_surfaces)
    assert len(cp.refs) == 12 and cp.leaf_first == [0, 6, 12], (len(cp.refs), cp.leaf_first)
    assert cp.dropped_vertices == 8
    # children come after their parents even though the source numbered them the other way round
    for k, (_a, _b, _d, c0, c1) in enumerate(cp.nodes2d):
        for c in (c0, c1):
            assert c & HI or c > k
    blob = cc.build_section(src, polys)
    assert blob == cc.convert_block(block, loc) and len(blob) % 16 == 0
    sec = cc.parse_section(blob)
    assert (len(sec.nodes), len(sec.planes), len(sec.leaves), len(sec.refs), len(sec.surfaces)) == (11, 13, 2, 12, 26), \
        (len(sec.nodes), len(sec.planes), len(sec.leaves), len(sec.refs), len(sec.surfaces))
    assert sec.depth == 6 and sec.stack2d == 2 and len(sec.vertices) == len(src.vertices) - 8
    assert [l[0] for l in sec.leaves] == [True, True]               # both leaves list a two-sided pane
    assert [(l[1], l[2]) for l in sec.leaves] == [(0, 6), (6, 6)]
    # tree and planes survive exactly
    assert sec.planes == src.planes
    for (pl, fr, bk), (spl, sbk, sfr) in zip(sec.nodes, src.nodes):
        assert (pl, fr, bk) == (spl, sfr, sbk)
    # every surface: same plane/flip, flags, material type, breakable group and the polygon's coordinates
    by_old = {}
    for i, old in enumerate(cp.surf_old):
        s_src = src.surfaces[old]
        s = sec.surfaces[i]
        assert (s["plane"], s["flip"]) == (s_src[0] & 0x7FFFFFFF, s_src[0] >> 31)
        assert s["flags"] == s_src[2] and s["material"] == PALETTE[s_src[4]]
        assert s["breakable"] == (s_src[3] if s_src[2] & cc.F_BREAKABLE else 0)
        pts = [sec.vertices[v] for v in s["poly"]]
        want = [tuple(src.vertices[v][:3]) for v in polys[old]]
        assert pts == want, (pts, want)
        by_old[old] = i
    # 2D lookups through the section agree with the source lookups, for every cell of every emitted reference
    n_checked = 0
    for ri, (pl, flip, root) in enumerate(sec.refs):
        for ci, c in enumerate(info["cells"]):
            if (c["plane"], c["flip"]) != (pl, flip) or c["leaf"] is None:
                continue
            r = c["rect"]
            p = point_on(c, (r[0] + r[1]) / 2, (r[2] + r[3]) / 2)
            got = lookup_section(sec, (pl, flip, root), p)
            old_ref = [x for x in range(len(src.refs)) if src.refs[x][0] == (pl | (HI if flip else 0))]
            want = {cc.lookup_2d(src, src.refs[x], p) for x in old_ref}
            assert cp.surf_old[got] in want, (ri, c["name"], got, want)
            n_checked += 1
    assert n_checked >= 20
    # the pane (surface flags two-sided, glass) is where the right leaf lists it
    pane = [i for i, s in enumerate(sec.surfaces) if s["flags"] & 1]
    assert len(pane) == 4 and all(sec.surfaces[i]["material"] == 9 for i in pane)
    # header facts
    assert struct.unpack_from("<4sH", blob)[0] == b"COLL"
    lo, hi = sec.bounds[:3], sec.bounds[3:]
    assert lo == (-10.0, -10.0, 0.0) and hi == (10.0, 10.0, 5.0), (lo, hi)
    return blob


def check_empty_and_tags() -> bytes:
    empty = cc.Source([], [], [], [], [], [], [], [], [])
    blob = cc.build_section(empty)
    sec = cc.parse_section(blob)
    assert not sec.nodes and not sec.surfaces and sec.leaves == [] and len(blob) == 128 + 16
    assert cc.section_tag(0) == "COL0" and cc.section_tag(9) == "COL9" and cc.section_tag(10) == "COLA"
    assert cc.section_tag(12) == "COLC" and cc.section_tag(35) == "COLZ"
    expect(lambda: cc.section_tag(36), "BSP index 36")
    return blob


def check_section_attacks(blob: bytes) -> None:
    expect(lambda: cc.parse_section(blob[:100]), "short section")
    expect(lambda: cc.parse_section(b"XXXX" + blob[4:]), "magic")
    expect(lambda: cc.parse_section(blob[:4] + struct.pack("<H", 9) + blob[6:]), "version")
    expect(lambda: cc.parse_section(blob[:-16]), "size mismatch")
    for i in range(len(cc.TABLES)):
        at = cc.DIR_AT + 8 * i
        expect(lambda: cc.parse_section(blob[:at] + struct.pack("<I", len(blob) + 16) + blob[at + 4:]), f"table {i} offset")
        expect(lambda: cc.parse_section(blob[:at + 4] + struct.pack("<I", 1 << 28) + blob[at + 8:]), f"table {i} count")
        expect(lambda: cc.parse_section(blob[:at] + struct.pack("<I", 130) + blob[at + 4:]), f"table {i} alignment")


def check_real_maps() -> None:
    maps = ["a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"] if os.environ.get("COLL_ALL_MAPS") else ["a10"]
    if not (ROOT / "assets/decompressed/a10.map").exists():
        print("real map conversion: skipped (no assets/decompressed/a10.map)")
        return
    rng = random.Random(7)
    for name in maps:
        path = ROOT / "assets/decompressed" / f"{name}.map"
        if not path.exists():
            continue
        n = 0
        for i, loc, block in cc._blocks(path):
            src = cc.read_source(block, loc)
            polys = cc.surface_polygons(src)
            blob = cc.build_section(src, polys)
            sec = cc.parse_section(blob)
            cp = cc.compact(src, polys)
            assert len(sec.surfaces) == len(src.surfaces) - cp.dropped_surfaces
            assert len(sec.vertices) == len(src.vertices) - cp.dropped_vertices
            assert len(blob) < 2 * 2**20, (name, i, len(blob))
            # polygons survive, flags and materials too (spot check), the 2D trees agree with the source on random cells
            for k in rng.sample(range(len(sec.surfaces)), min(300, len(sec.surfaces))):
                old = cp.surf_old[k]
                assert [sec.vertices[v] for v in sec.surfaces[k]["poly"]] == [tuple(src.vertices[v][:3]) for v in polys[old]]
                assert sec.surfaces[k]["flags"] == src.surfaces[old][2]
            if i == 0 or name == "a10" and i == 1:
                rep = cc.verify_source(src, polys)
                assert not rep.problems and rep.lookup_bad == 0, (name, i, rep.problems)
            n += 1
        assert n > 0
        print(f"real map {name}: {n} BSPs converted and parsed")
    # section tag order of a pack: COL0.. in the BSP order
    secs = cc.convert_map(ROOT / "assets/decompressed/a10.map", [0, 1])
    assert [t for t, _ in secs] == ["COL0", "COL1"]


def main() -> None:
    check_source()
    check_source_attacks()
    blob = check_build()
    empty = check_empty_and_tags()
    check_section_attacks(blob)
    check_real_maps()
    if os.environ.get("COLL_FIXTURE"):
        Path(os.environ["COLL_FIXTURE"]).write_bytes(hp2.build("coll", [("COL0", blob), ("COL1", empty)]))
    print("test_collconv: ok")


if __name__ == "__main__":
    main()
