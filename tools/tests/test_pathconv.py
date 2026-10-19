"""PATn conversion on a synthetic navigation world (no game data needed), the section writer / parser, the checks of
``verify_mesh``, the reader of the pathfinding byte out of a structure_bsp block, and attacks on all of them. Set
PATH_FIXTURE=<path> to also write the .hp2 that the C reader test (engine/tests/test_path.c, which hard-codes the numbers of
this world) opens. A skip-if-missing check converts every BSP of the real a10 map (PATH_ALL_MAPS=1: the ten campaign maps).

The world (x east, y north, z up; units are world units):

    A1  x 0..3,   y 0..4,  z 0      two surfaces, merge into one sector                       material 2
    A2  x 3..6,   y 0..4,  z 0      two surfaces, merge into one sector (other material: not merged with A1)    material 5
    C   x 6..8,   y 1.5..2.5, z 0   a doorway; A2's east edge has the vertices (6, 1.5) and (6, 2.5)    material 6
    B   x 8..12,  y 0..4,  z 0      two surfaces (west side carries the doorway vertices)     material 7
    D   x 12..14, y 0..4,  z 0.15   a step of 0.15 up from B (riser quad at x = 12)
    E   x 14..16, y 0..4,  z 0.6    0.45 up from D (riser at x = 14): a vault
    F   x 16..19, y 0..4,  z -1.4   2.0 below E (wall at x = 16): a drop
    H   x 19..22, y 0..4,  z 1.0    2.4 above F (wall at x = 19): a drop down only
    L   a ladder volume x 18.8..19, y 1..1.3, z -1.4..1.0 (climbable walls and caps): CLIMB links F <-> H
    G   x 0..3,   y 10..13, z 0     an island
    X   x 30..32, y 0..2,  z 0, one quad whose surface the collision section drops (col_index -1): not a sector
    S   x 40..42, y 0..2,  z 0, tilted so far that it is not walkable (pf byte 0): not a sector
    Q1 Q2 Q3  x 50..56, y 0..4, z 0  a chain of three (materials 5, 8, 9) with a ceiling at z = 0.3 over Q2 only (headroom 9/32 wu)

Sectors: A1 A2 C B D E F H Ltop G Q1 Q2 Q3 = 13; links: walk 10, step 2, vault 1, drop 3, climb 2 = 18; components over WALK / STEP / CLIMB: 6."""
import math
import os
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[1]), str(Path(__file__).resolve().parent)]
from halo2ps2 import bsp, collconv as cc, hp2, pathconv as pc  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


class World:
    def __init__(self) -> None:
        self.verts: list[tuple[float, float, float]] = []
        self.vid: dict = {}
        self.polys: list[tuple[int, ...]] = []
        self.flags: list[int] = []
        self.material: list[int] = []
        self.col: list[int] = []
        self.names: list[str] = []

    def v(self, p) -> int:
        p = tuple(float(c) for c in p)
        if p not in self.vid:
            self.vid[p] = len(self.verts)
            self.verts.append(p)
        return self.vid[p]

    def add(self, pts, flags=0, material=0, name="", col=True) -> int:
        self.polys.append(tuple(self.v(p) for p in pts))
        self.flags.append(flags)
        self.material.append(material)
        self.col.append(len(self.polys) - 1 if col else -1)
        self.names.append(name)
        return len(self.polys) - 1

    def grid(self, x0, x1, y0, y1, z, nx, ny, material, name, flags=0, col=True):
        """Quads of a floor rectangle (counter-clockwise seen from above); the grid lines of every floor are shared."""
        xs = [x0 + (x1 - x0) * i / nx for i in range(nx + 1)]
        ys = [y0 + (y1 - y0) * j / ny for j in range(ny + 1)]
        for i in range(nx):
            for j in range(ny):
                self.add([(xs[i], ys[j], z), (xs[i + 1], ys[j], z), (xs[i + 1], ys[j + 1], z), (xs[i], ys[j + 1], z)],
                         flags, material, name, col)

    def nav_input(self) -> pc.NavInput:
        normals = []
        for loop in self.polys:
            n = cc.newell([self.verts[i] for i in loop])
            ln = math.sqrt(sum(c * c for c in n))
            normals.append(tuple(c / ln for c in n))
        pf = bytearray()
        for s, n in enumerate(normals):
            pf.append((0x40 if n[2] >= 0.71 else 0) | (0x80 if n[2] >= 0.71 and self.flags[s] & 8 else 0))
        inp = pc.NavInput(list(self.verts), list(self.polys), normals, list(self.flags), list(self.material), bytes(pf),
                          list(self.col), name="synthetic")
        inp.dropped_walkable = sum(1 for s in range(len(pf)) if pf[s] & 0x40 and self.col[s] < 0)
        return inp


def make_world() -> World:
    w = World()
    # A1: two surfaces (the right one carries the doorway vertices of A2's west side), merge into one sector
    w.add([(0, 0, 0), (1.5, 0, 0), (1.5, 4, 0), (0, 4, 0)], 0, 2, "A1")
    w.add([(1.5, 0, 0), (3, 0, 0), (3, 1.5, 0), (3, 2.5, 0), (3, 4, 0), (1.5, 4, 0)], 0, 2, "A1")
    # A2: west side carries (3, 1.5) and (3, 2.5) for A1, east side (6, 1.5) and (6, 2.5) for the doorway C
    w.add([(3, 0, 0), (4.5, 0, 0), (4.5, 4, 0), (3, 4, 0), (3, 2.5, 0), (3, 1.5, 0)], 0, 5, "A2")
    w.add([(4.5, 0, 0), (6, 0, 0), (6, 1.5, 0), (6, 2.5, 0), (6, 4, 0), (4.5, 4, 0)], 0, 5, "A2")
    w.add([(6, 1.5, 0), (8, 1.5, 0), (8, 2.5, 0), (6, 2.5, 0)], 0, 6, "C")
    # B: west side carries the doorway ends (8, 1.5) and (8, 2.5); its east side is one edge
    w.add([(8, 0, 0), (10, 0, 0), (10, 4, 0), (8, 4, 0), (8, 2.5, 0), (8, 1.5, 0)], 0, 7, "B")
    w.add([(10, 0, 0), (12, 0, 0), (12, 4, 0), (10, 4, 0)], 0, 7, "B")
    # D tread and its riser (vertical, facing -x, bottom edge = B's east edge reversed)
    w.add([(12, 0, .15), (14, 0, .15), (14, 4, .15), (12, 4, .15)], 0, 5, "D")
    w.add([(12, 4, 0), (12, 0, 0), (12, 0, .15), (12, 4, .15)], 0, 5, "riser BD")
    # E (0.45 above D), riser at x = 14 facing -x
    w.add([(14, 0, .6), (16, 0, .6), (16, 4, .6), (14, 4, .6)], 0, 5, "E")
    w.add([(14, 4, .15), (14, 0, .15), (14, 0, .6), (14, 4, .6)], 0, 5, "riser DE")
    # F, 2.0 below E: wall at x = 16 facing +x
    w.add([(16, 0, -1.4), (19, 0, -1.4), (19, 4, -1.4), (16, 4, -1.4)], 0, 5, "F")
    w.add([(16, 4, .6), (16, 0, .6), (16, 0, -1.4), (16, 4, -1.4)], 0, 5, "wall EF")
    # H, 2.4 above F: wall at x = 19 facing -x
    w.add([(19, 0, 1.0), (22, 0, 1.0), (22, 4, 1.0), (19, 4, 1.0)], 0, 5, "H")
    w.add([(19, 4, -1.4), (19, 0, -1.4), (19, 0, 1.0), (19, 4, 1.0)], 0, 5, "wall FH")
    # the ladder volume: climbable walls and caps (not two-sided here)
    lx0, lx1, ly0, ly1, lz0, lz1 = 18.8, 19.0, 1.0, 1.3, -1.4, 1.0
    cl = 4
    w.add([(lx0, ly1, lz1), (lx0, ly0, lz1), (lx1, ly0, lz1), (lx1, ly1, lz1)], cl, 5, "ladder top")
    w.add([(lx0, ly0, lz0), (lx0, ly1, lz0), (lx1, ly1, lz0), (lx1, ly0, lz0)], cl, 5, "ladder bottom")
    for a, b in (((lx0, ly0), (lx1, ly0)), ((lx1, ly0), (lx1, ly1)), ((lx1, ly1), (lx0, ly1)), ((lx0, ly1), (lx0, ly0))):
        w.add([(a[0], a[1], lz0), (b[0], b[1], lz0), (b[0], b[1], lz1), (a[0], a[1], lz1)], cl, 5, "ladder wall")
    # the island, a dropped floor, a steep one
    w.grid(0, 3, 10, 13, 0.0, 1, 1, 5, "G")
    w.add([(30, 0, 0), (32, 0, 0), (32, 2, 0), (30, 2, 0)], 0, 5, "X", col=False)
    w.add([(40, 0, 0), (42, 0, 0), (42, 2, 2.0), (40, 2, 2.0)], 0, 5, "S")
    # a chain Q1 - Q2 - Q3 with a low ceiling (0.3 up) over Q2 only: headroom 9/32 = 0.28 wu
    w.add([(50, 0, 0), (52, 0, 0), (52, 4, 0), (50, 4, 0)], 0, 5, "Q1")
    w.add([(52, 0, 0), (54, 0, 0), (54, 4, 0), (52, 4, 0)], 0, 8, "Q2")
    w.add([(54, 0, 0), (56, 0, 0), (56, 4, 0), (54, 4, 0)], 0, 9, "Q3")
    w.add([(52, 0, .3), (52, 4, .3), (54, 4, .3), (54, 0, .3)], 0, 5, "ceiling")
    return w


def sec_names(w: World, mesh: pc.NavMesh) -> list[str]:
    return ["/".join(sorted({w.names[s] for s in sc.surfs})) for sc in mesh.sectors]


def kinds(mesh: pc.NavMesh) -> dict:
    out: dict = {}
    for l in mesh.links:
        out[pc.KIND_NAMES[l.kind]] = out.get(pc.KIND_NAMES[l.kind], 0) + 1
    return out


def find(mesh: pc.NavMesh, w: World, name: str) -> int:
    names = sec_names(w, mesh)
    return names.index(name)


def link_between(mesh: pc.NavMesh, a: int, b: int, kind: int):
    for l in mesh.links:
        if (l.src, l.dst, l.kind) == (a, b, kind):
            return l
    return None


# ---- tests ----------------------------------------------------------------------------------------
def check_world() -> None:
    w = make_world()
    inp = w.nav_input()
    mesh = pc.build_mesh(inp)
    rep = pc.verify_mesh(inp, mesh)
    assert not rep.problems, rep.problems
    names = sec_names(w, mesh)
    assert sorted(names) == sorted(["A1", "A2", "C", "B", "D", "E", "F", "H", "ladder top", "G", "Q1", "Q2", "Q3"]), names
    assert kinds(mesh) == dict(walk=10, step=2, vault=1, drop=3, climb=2), kinds(mesh)
    assert mesh.ncomp == 6, mesh.ncomp
    ix = {n: names.index(n) for n in names}
    # merged sectors: A1, A2 and B are two surfaces each
    assert len(mesh.sectors[ix["A1"]].surfs) == 2 and len(mesh.sectors[ix["A2"]].surfs) == 2
    assert len(mesh.sectors[ix["B"]].surfs) == 2 and len(mesh.sectors[ix["C"]].surfs) == 1
    assert all(len(sc.loop) >= 4 for sc in mesh.sectors)
    # walk links with the shared edge as portal, centroid-based cost
    l = link_between(mesh, ix["A1"], ix["A2"], pc.K_WALK)
    assert l and l.a_right == (3.0, 0.0, 0.0) and l.a_left == (3.0, 4.0, 0.0), l
    assert math.isclose(l.cost, 3.0) and l.dz == 0.0
    l = link_between(mesh, ix["A2"], ix["C"], pc.K_WALK)
    assert l and l.a_right == (6.0, 1.5, 0.0) and l.a_left == (6.0, 2.5, 0.0), l     # the run is the doorway only
    assert math.isclose(l.cost, 2.5), l.cost
    # step B <-> D: portal on B's east edge, rise 0.15
    l = link_between(mesh, ix["B"], ix["D"], pc.K_STEP)
    assert l and l.a_right == (12.0, 0.0, 0.0) and l.a_left == (12.0, 4.0, 0.0) and l.b_right == (12.0, 0.0, pc.f32(0.15))
    assert math.isclose(l.dz, 0.15) and link_between(mesh, ix["D"], ix["B"], pc.K_STEP).dz < 0
    # vault up, drop down, drop only
    assert link_between(mesh, ix["D"], ix["E"], pc.K_VAULT) and link_between(mesh, ix["E"], ix["D"], pc.K_DROP)
    assert link_between(mesh, ix["E"], ix["F"], pc.K_DROP) and not link_between(mesh, ix["F"], ix["E"], pc.K_VAULT)
    assert link_between(mesh, ix["H"], ix["F"], pc.K_DROP) and not any(l.src == ix["F"] and l.dst == ix["H"] and l.kind != pc.K_CLIMB for l in mesh.links)
    l = link_between(mesh, ix["F"], ix["H"], pc.K_CLIMB)
    assert l and l.a_right == l.a_left and math.isclose(l.dz, 2.4), l
    assert l.a_right[2] == f32(-1.4) if False else abs(l.a_right[2] + 1.4) < 1e-6
    # components
    comp = {n: mesh.sectors[ix[n]].comp for n in names}
    assert len({comp[n] for n in ("A1", "A2", "C", "B", "D")}) == 1 and comp["E"] != comp["D"] and comp["F"] == comp["H"]
    assert len({comp["G"], comp["E"], comp["ladder top"], comp["A1"], comp["F"], comp["Q1"]}) == 6 and comp["Q1"] == comp["Q2"] == comp["Q3"]
    # headroom: only Q2 has a ceiling (0.3 up), everything else is open
    assert [mesh.sectors[ix[n]].headroom for n in ("Q1", "Q2", "Q3", "A1", "F")] == [255, 9, 255, 255, 255], [mesh.sectors[ix[n]].headroom for n in ("Q1", "Q2", "Q3", "A1", "F")]
    # sector data: plane height, surface (largest source surface, COLn index), flags, material
    assert mesh.sectors[ix["D"]].centroid == (13.0, 2.0, 0.15) or all(abs(a - b) < 1e-9 for a, b in zip(mesh.sectors[ix["D"]].centroid, (13.0, 2.0, 0.15)))
    assert mesh.sectors[ix["ladder top"]].flags & pc.SF_CLIMBABLE
    assert mesh.sectors[ix["A1"]].material == 2 and mesh.sectors[ix["A2"]].material == 5
    assert w.names[next(s for s in range(len(w.polys)) if w.col[s] == mesh.sectors[ix["D"]].surface)] == "D"
    # the dropped surface and the steep one are not sectors; stats count them
    assert mesh.stats["dropped_walkable"] == 1 and inp.dropped_walkable == 1
    # unmerged variant: one sector per surface
    mesh2 = pc.build_mesh(inp, merge=False)
    assert len(mesh2.sectors) == sum(1 for s in range(len(w.polys)) if inp.pf[s] & 0x40 and w.col[s] >= 0)
    assert not pc.verify_mesh(inp, mesh2).problems


def f32(x):
    return pc.f32(x)


def check_merge_rules() -> None:
    # two coplanar quads merge; a concave union (an L) does not; different keys do not; a bent pair beyond the tolerance does not
    def build(polys, flags=None, mats=None):
        w = World()
        for i, p in enumerate(polys):
            w.add(p, (flags or [0] * len(polys))[i], (mats or [0] * len(polys))[i], f"s{i}")
        inp = w.nav_input()
        return pc.build_mesh(inp), inp

    q1 = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)]
    q2 = [(2, 0, 0), (4, 0, 0), (4, 2, 0), (2, 2, 0)]
    m, _ = build([q1, q2])
    assert len(m.sectors) == 1 and len(m.links) == 0
    q3 = [(2, 0, 0), (4, 0, 0), (4, 1, 0), (2, 1, 0)]        # L shape with q1 + q3 (shares only part of q1's east edge): no shared edge pair
    m, _ = build([q1, q3])
    assert len(m.sectors) == 2
    q4 = [(2, 2, 0), (4, 2, 0), (4, 4, 0), (2, 4, 0)]        # q1 + q4 touch at a corner only
    m, _ = build([q1, q4])
    assert len(m.sectors) == 2 and len(m.links) == 0
    # L: q1 (0..2) + q5 on top of the east half of q1's north edge is not an edge pair; build the L from three quads sharing edges
    qa = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)]
    qb = [(2, 0, 0), (4, 0, 0), (4, 2, 0), (2, 2, 0)]
    qc = [(0, 2, 0), (2, 2, 0), (2, 4, 0), (0, 4, 0)]
    m, _ = build([qa, qb, qc])
    assert len(m.sectors) in (2, 3)                          # any pair may merge, never all three into a concave L
    assert all(pc.convex_plan(sc.loop, build([qa, qb, qc])[1].verts) for sc in m.sectors)
    m, _ = build([q1, q2], mats=[1, 2])
    assert len(m.sectors) == 2 and len(m.links) == 2
    m, _ = build([q1, q2], flags=[0, 2])                      # invisible differs
    assert len(m.sectors) == 2
    # a slope of 0.5 over 2 wu bends the plane by 0.5 at the corner: not mergeable; 0.01 is
    for rise, want in ((0.5, 2), (0.01, 1)):
        a = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)]
        b = [(2, 0, 0), (4, 0, rise), (4, 2, rise), (2, 2, 0)]
        m, _ = build([a, b])
        assert len(m.sectors) == want, (rise, len(m.sectors))
    # more than MAX_VERTS vertices never come out of the merger
    ring = []
    n = 16
    for i in range(n):                                        # a fan of triangles around a 16-gon: whatever merges stays <= 12 vertices
        a0, a1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
        ring.append([(0, 0, 0), (math.cos(a0) * 3, math.sin(a0) * 3, 0), (math.cos(a1) * 3, math.sin(a1) * 3, 0)])
    m, inp = build(ring)
    assert all(len(sc.loop) <= pc.MAX_VERTS for sc in m.sectors) and not pc.verify_mesh(inp, m).problems


def check_chain_rules() -> None:
    """Risers: a wall taller than both floors is not a step; a thin wall between floors of the same height is not crossed;
    a low curb is; far apart or misaligned edges are not."""
    def stairs(height, wall_top=None, gap=0.0, shift=0.0, depth=1):
        w = World()
        w.add([(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)], 0, 0, "A")
        x = 2.0
        zb = height
        w.add([(x + gap, 2 + shift, zb), (x + gap, 0 + shift, zb), (x + gap + 2, 0 + shift, zb), (x + gap + 2, 2 + shift, zb)][::1], 0, 0, "B") if False else None
        w.add([(x + gap, 0 + shift, zb), (x + gap + 2, 0 + shift, zb), (x + gap + 2, 2 + shift, zb), (x + gap, 2 + shift, zb)], 0, 0, "B")
        top = wall_top if wall_top is not None else max(0.0, zb)
        lo = min(0.0, zb)
        # riser quad: bottom edge = A's east edge reversed, top edge = B's west edge reversed
        w.add([(2, 2, 0), (2, 0, 0), (x + gap, 0 + shift, zb), (x + gap, 2 + shift, zb)], 0, 0, "riser")
        inp = w.nav_input()
        return pc.build_mesh(inp), inp
    m, _ = stairs(0.15)
    assert kinds(m).get("step") == 2, kinds(m)
    m, _ = stairs(0.45)
    assert kinds(m) == dict(vault=1, drop=1), kinds(m)
    m, _ = stairs(2.0)
    assert kinds(m) == dict(drop=1), kinds(m)
    m, _ = stairs(5.0)
    assert not m.links, kinds(m)
    m, _ = stairs(0.15, shift=1.0)                            # shifted by half the edge: overlap of 1 wu remains
    assert kinds(m).get("step") == 2
    m, _ = stairs(0.15, shift=1.95)                           # overlap 0.05 < PORTAL_MIN
    assert not m.links
    m, _ = stairs(0.15, gap=2.0)                              # B 2 wu away: the riser is a long ramp
    assert not m.links or all(l.kind != pc.K_STEP for l in m.links)

    # a tall thin wall between two floors of equal height: the riser quads go up and over (z band check)
    w = World()
    w.add([(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)], 0, 0, "A")
    w.add([(2.2, 0, 0), (4, 0, 0), (4, 2, 0), (2.2, 2, 0)], 0, 0, "B")
    w.add([(2, 2, 0), (2, 0, 0), (2, 0, 3), (2, 2, 3)], 0, 0, "wall1")         # faces -x, 3 high
    w.add([(2, 2, 3), (2, 0, 3), (2.2, 0, 3), (2.2, 2, 3)], 0, 0, "cap")       # the wall's top: a walkable sector 3 up (drop only)
    w.add([(2.2, 0, 0), (2.2, 2, 0), (2.2, 2, 3), (2.2, 0, 3)], 0, 0, "wall2")
    inp = w.nav_input()
    m = pc.build_mesh(inp)
    nm = sec_names(w, m)
    a, b = nm.index("A"), nm.index("B")
    assert not any({l.src, l.dst} == {a, b} for l in m.links), kinds(m)         # no step over the wall (z band)
    assert kinds(m) == dict(drop=2), kinds(m)                                   # but the cap drops onto both floors


def check_section() -> None:
    w = make_world()
    inp = w.nav_input()
    mesh = pc.build_mesh(inp)
    data = pc.build_section(mesh, inp.verts)
    s = pc.parse_section(data)
    assert len(s.sectors) == 13 and len(s.links) == 18 and s.ncomp == 6 and s.size == len(data) and s.cost_scale == 64
    assert [sec["headroom"] for sec in s.sectors].count(9) == 1
    # thickness: A1 is 3 x 4 (3 wu = 192 units), the doorway C 2 x 1 (64), the ladder cap 0.2 x 0.3 (12 units)
    names0 = sec_names(w, mesh)
    assert [s.sectors[names0.index(n)]["thickness"] for n in ("A1", "C", "ladder top", "B")] == [192, 64, 12, 255], [s.sectors[names0.index(n)]["thickness"] for n in ("A1", "C", "ladder top", "B")]
    assert s.thresholds == (pc.STEP_MAX, pc.VAULT_MAX, pc.DROP_MAX) or all(abs(a - b) < 1e-6 for a, b in zip(s.thresholds, (pc.STEP_MAX, pc.VAULT_MAX, pc.DROP_MAX)))
    # grid: every sector appears in the cells its polygon touches; the cell of a sector's centroid lists it
    for i, sec in enumerate(s.sectors):
        cx, cy = sec["centroid"][0], sec["centroid"][1]
        gx = int((cx - s.origin[0]) / s.cell)
        gy = int((cy - s.origin[1]) / s.cell)
        cell = gy * s.grid[0] + gx
        assert i in s.cell_items[s.cell_start[cell]:s.cell_start[cell + 1]], (i, cx, cy)
    assert s.cell_start[0] == 0 and s.cell_start[-1] == len(s.cell_items)
    assert all(a <= b for a, b in zip(s.cell_start, s.cell_start[1:]))
    # costs are rounded up in cost units, ordering by source sector
    srcs = []
    for i, sec in enumerate(s.sectors):
        srcs += [i] * sec["link_count"]
    assert srcs == [l.src for l in mesh.links]
    for l, rec in zip(mesh.links, s.links):
        assert rec["to"] == l.dst and rec["kind"] == l.kind and rec["cost"] >= l.cost * 64 - 1e-6 and rec["cost"] < l.cost * 64 + 1
    # compression removed the collinear vertex of the shared edges but kept the portal ends: A1 is a 4-gon, A2 a 4-gon plus the doorway ends
    names = sec_names(w, mesh)
    a1 = s.sectors[names.index("A1")]
    a2 = s.sectors[names.index("A2")]
    assert a1["nverts"] == 4 and a2["nverts"] == 6, (a1["nverts"], a2["nverts"])
    assert pc.section_tag(0) == "PAT0" and pc.section_tag(9) == "PAT9" and pc.section_tag(10) == "PATA" and pc.section_tag(35) == "PATZ"
    try:
        pc.section_tag(36)
    except pc.PathError:
        pass
    else:
        raise SystemExit("tag 36 accepted")
    # attacks on the parser
    def bad(blob, what):
        try:
            pc.parse_section(blob)
        except pc.PathError:
            return
        raise SystemExit(f"accepted: {what}")
    bad(data[:100], "short")
    bad(b"PATX" + data[4:], "magic")
    bad(data[:4] + struct.pack("<H", 2) + data[6:], "version")
    bad(data[:8] + struct.pack("<I", len(data) + 16) + data[12:], "size")
    for i, name in enumerate(pc.TABLES):
        off = pc.DIR_AT + 8 * i
        bad(data[:off] + struct.pack("<I", 4) + data[off + 4:], f"{name} offset")
        bad(data[:off] + struct.pack("<I", 1) + data[off + 4:], f"{name} offset alignment")
        bad(data[:off + 4] + struct.pack("<I", 0x7FFFFFF) + data[off + 8:], f"{name} count")
    # an empty world (no walkable surface) serialises and parses
    empty = World()
    empty.add([(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)], 0, 0, "wall")
    einp = empty.nav_input()
    emesh = pc.build_mesh(einp)
    es = pc.parse_section(pc.build_section(emesh, einp.verts))
    assert not es.sectors and not es.links and es.ncomp == 0 and es.grid == (1, 1) and es.cell_start == [0, 0]


def check_xbox_block() -> None:
    """The pathfinding byte of a structure_bsp block: size rules and the end-to-end conversion of the collision test's world."""
    import test_collconv as tc
    block, loc, info = tc.make_world()
    src = cc.read_source(block, loc)
    n = len(src.surfaces)
    # no reflexive: zeros
    assert pc.read_pathfinding_bytes(block, loc, n) == bytes(n)
    b = bytearray(block)
    pf = bytes(0x40 if src.planes[s[0] & 0x7FFFFFFF][2] * (-1 if s[0] & cc.HI else 1) > 0.9 else 0 for s in src.surfaces)
    addr = tc.LOAD + len(b)
    b += pf
    root = struct.unpack_from("<I", b, 0)[0] - tc.LOAD
    struct.pack_into("<II", b, root + 0x1E4, n, addr)
    loc2 = bsp.BspLocation(loc.file_offset, len(b), loc.address, loc.path)
    assert pc.read_pathfinding_bytes(bytes(b), loc2, n) == pf
    for count, why in ((n - 1, "count"), (n + 1, "count")):
        c = bytearray(b)
        struct.pack_into("<II", c, root + 0x1E4, count, addr)
        try:
            pc.read_pathfinding_bytes(bytes(c), loc2, n)
        except pc.PathError:
            pass
        else:
            raise SystemExit(f"accepted a pathfinding table with the wrong {why}")
    c = bytearray(b)
    struct.pack_into("<II", c, root + 0x1E4, n, addr + len(b))        # outside the block
    try:
        pc.read_pathfinding_bytes(bytes(c), loc2, n)
    except pc.PathError:
        pass
    else:
        raise SystemExit("accepted a table outside the block")
    c = bytearray(b)
    struct.pack_into("<II", c, root + 0x1F0, 3, addr)                  # an edge table appears: layout unknown
    try:
        pc.read_pathfinding_bytes(bytes(c), loc2, n)
    except pc.PathError:
        pass
    else:
        raise SystemExit("accepted pathfinding edges")
    # end to end: the floors of the two rooms (two and four cells) become one sector each, on the COLn surfaces
    inp = pc.read_input(bytes(b), loc2)
    assert inp.pf == pf and sum(1 for p in inp.pf if p) == 7 and len(inp.col_index) == n
    assert sum(1 for c_ in inp.col_index if c_ < 0) == 6          # the dead cuboid's surfaces are not in the collision section
    mesh = pc.build_mesh(inp)
    assert not pc.verify_mesh(inp, mesh).problems
    assert len(mesh.sectors) == 2 and mesh.ncomp == 2 and not mesh.links
    assert sorted(len(sc.surfs) for sc in mesh.sectors) == [2, 4]      # FR two cells, FL four; the dead cuboid's top is dropped
    for sc in mesh.sectors:
        assert sc.surface in inp.col_index and inp.material[next(s for s in sc.surfs)] == sc.material
    # the whole converter entry point on the block
    data, _inp, _mesh = pc.convert_block(bytes(b), loc2)
    assert len(pc.parse_section(data).sectors) == 2


def check_rule_report() -> None:
    """verify_mesh reports a flag that breaks the nz rule and a breakable bit without a breakable surface."""
    w = make_world()
    inp = w.nav_input()
    mesh = pc.build_mesh(inp)
    bad = bytearray(inp.pf)
    bad[0] = 0                                                  # a flat floor surface unflagged
    inp2 = pc.NavInput(inp.verts, inp.polys, inp.normals, inp.flags, inp.material, bytes(bad), inp.col_index)
    assert pc.verify_mesh(inp2, mesh).problems
    bad = bytearray(inp.pf)
    bad[0] |= 0x80
    inp3 = pc.NavInput(inp.verts, inp.polys, inp.normals, inp.flags, inp.material, bytes(bad), inp.col_index)
    assert any("breakable" in p for p in pc.verify_mesh(inp3, mesh).problems)
    bad = bytearray(inp.pf)
    bad[0] |= 0x01
    inp4 = pc.NavInput(inp.verts, inp.polys, inp.normals, inp.flags, inp.material, bytes(bad), inp.col_index)
    assert any("unknown" in p for p in pc.verify_mesh(inp4, mesh).problems)


def check_fixture() -> None:
    out = os.environ.get("PATH_FIXTURE")
    if not out:
        return
    w = make_world()
    inp = w.nav_input()
    mesh = pc.build_mesh(inp)
    empty = World()
    empty.add([(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)], 0, 0, "wall")
    einp = empty.nav_input()
    secs = [("PAT0", pc.build_section(mesh, inp.verts)), ("PAT1", pc.build_section(pc.build_mesh(einp), einp.verts))]
    Path(out).write_bytes(hp2.build("pathfix", secs))


def check_real(all_maps: bool) -> None:
    maps = ROOT / "assets" / "decompressed"
    names = ["a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"] if all_maps else ["a10"]
    if not (maps / "a10.map").exists():
        print("skip: assets/decompressed/a10.map missing (real-map checks)")
        return
    total = 0
    for name in names:
        path = maps / f"{name}.map"
        if not path.exists():
            continue
        for i, loc, block in pc._blocks(path):
            inp = pc.read_input(block, loc)
            mesh = pc.build_mesh(inp)
            rep = pc.verify_mesh(inp, mesh)
            assert not rep.problems, (name, i, rep.problems[:3])
            data = pc.build_section(mesh, inp.verts)
            s = pc.parse_section(data)
            assert len(s.sectors) == len(mesh.sectors) and len(s.links) == len(mesh.links)
            total += 1
    print(f"real maps: {total} BSPs converted and verified")


def main() -> None:
    check_world()
    check_merge_rules()
    check_chain_rules()
    check_section()
    check_xbox_block()
    check_rule_report()
    check_fixture()
    check_real(os.environ.get("PATH_ALL_MAPS") == "1")
    print("test_pathconv: ok")


if __name__ == "__main__":
    main()
