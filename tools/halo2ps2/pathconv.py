#!/usr/bin/env python3
"""Build the PS2 pathfinding sections ``PAT0``..``PATZ`` (one per ``structure_bsp``, same index order as ``BSPn`` /
``COLn``) of ``assets/hp2/<map>.path.hp2`` (docs/26-path-section.md; reader engine/src/ai/path.[ch]).

What the Xbox data holds (verified on all 69 BSPs of the ten campaign maps and ui.map; ``--verify`` re-runs it)
------------------------------------------------------------------------------------------------------------
``structure_bsp`` +0x1E4 is a reflexive named "pathfinding surfaces": ONE BYTE per collision-BSP surface (the count equals
the number of collision surfaces on every BSP).  Nothing else of the BSP is pathfinding data: there are NO sector, link,
vertex, object-reference or hint blocks (the +0x1F0 reflexive that would hold edge data is empty on all 69 BSPs; the
neighbouring words are zero).  The byte has three values on the retail maps: 0x00, 0x40 and 0xC0.

* bit 6 (0x40) = "walkable": set on a surface exactly when the effective normal's z component is >= 0.71 (the lowest
  flagged surface has 0.71002, the highest unflagged upward one 0.70998; 197,888 + 2,284 + 382 + 12 + 539 + 199 flagged
  surfaces, 0 exceptions).  The flag is independent of every other surface flag (two-sided, invisible, climbable and
  breakable surfaces are flagged by their slope alone), so it carries no information beyond the geometry.
* bit 7 (0x80) = "walkable but breakable": set on exactly the walkable surfaces whose collision flags say breakable
  (199 of 199; the breakable glass floors).

The scenario holds no hint data either (jump / climb / vault hints are Halo 2 constructs): the original Halo CE engine
navigates over the flagged collision surfaces (the "pathfinding surfaces"), and its actors jump / vault obstacles through
their own movement behaviour.  So the converter DERIVES the navigation mesh the reader needs from exactly those flagged
surfaces (every statement in the next section is therefore mine, not from the data; the doc says so too).

Derived navigation mesh
-----------------------
* **Sectors** = the flagged surfaces (convex polygons, 3..8 vertices, counter-clockwise seen from above because nz >= 0.71)
  merged greedily when two neighbours (a shared edge run) keep the union convex in plan, planar to ``PLANE_TOL`` (vertical
  deviation) and of equal "kind" (breakable bit, invisible / climbable flags, material type); at most ``MAX_VERTS``
  vertices.  Surfaces the collision section drops (no leaf reaches them) are not walkable and are skipped.  A sector keeps
  the COLn index of its largest source surface and that surface's material type.
* **WALK links**: two sectors sharing an edge run (the collision mesh is a closed 2-manifold, so adjacency is exact: the
  directed edge (a, b) of one loop is the edge (b, a) of the other).  The portal is the run, endpoints are loop vertices.
* **STEP / VAULT / DROP links** cross a riser: a chain of at most ``CHAIN_DEPTH`` non-walkable surfaces with
  -0.35 <= nz < 0.71 (walls, steep slopes) between a boundary edge of sector A and a boundary edge of sector B whose
  directions are anti-parallel (face to face), whose plan overlap is at least ``PORTAL_MIN`` long, whose plan offset is at most
  ``RUN_MAX`` + ``RUN_PER_RISE`` x rise, and where the chain never leaves the vertical band of the two edges (plus
  ``BAND_EPS``): an obstacle taller than both floors is a wall, not a step.  The rise (B above A) classifies the pair:
  |rise| <= ``STEP_MAX`` both directions STEP; rise up to ``VAULT_MAX`` VAULT upward and DROP downward; a higher rise
  only DROP downward (down to ``DROP_MAX``).  The portal is the plan overlap, with both sides' 3D endpoints (the portal
  of a WALK link has the same endpoints on both sides).
* **CLIMB links**: components of climbable collision surfaces (ladder volumes) whose footprint touches walkable, non
  climbable sectors at its bottom and top (vertical gap <= ``CLIMB_MAX``): bottom <-> top, both directions.
* **Costs** (``cost`` u16 in 1/``COST_SCALE`` wu, rounded up so the straight-line centroid distance stays an admissible,
  consistent heuristic): centroid(A) -> portal midpoint(s) -> centroid(B) plus a fixed penalty per kind.
* **Components** over the symmetric kinds (WALK, STEP, CLIMB): union-find ids (``comp``), for O(1) negative reachability.
* **Headroom** per sector (u8, 1/32 wu): the lowest overhead surface (normal z < -0.3) above any part of the polygon, found by
  clipping every overhead polygon against the sector in plan.  The floors of hidden voids, crawl spaces and the lower layer of
  a stacked pair of floors have 0.1..0.3 wu; the query's ``height`` decides which sectors an actor enters.
* **Thickness** per sector (u8, 1/64 wu): the smallest extent of the convex polygon across any of its edges (slivers), used so a
  partial route never ends in a sector narrower than the actor.
* **Link width** (byte 3 of the link record, 1/16 wu): the plan length of the portal; a query skips links narrower than 2 radius.
* **Grid**: uniform plan grid of sector bounding boxes for point location.

Output section: see docs/26-path-section.md (little-endian, tables 16-byte aligned).

Usage:
    python3 -I tools/halo2ps2/pathconv.py assets/decompressed/a10.map -o assets/hp2/a10.path.hp2 [--bsp N]
    python3 -I tools/halo2ps2/pathconv.py --info assets/hp2/a10.path.hp2
    python3 -I tools/halo2ps2/pathconv.py --verify assets/decompressed/a10.map [--bsp N]
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bsp, collconv, hp2  # noqa: E402

MAGIC = b"PATH"
VERSION = 1
HEADER_SIZE = 128
SECTOR_REC = 32
POINT_REC = 12
INDEX_REC = 2
LINK_REC = 12
CELLSTART_REC = 4
CELLITEM_REC = 2
TABLES = ("sectors", "points", "indices", "links", "cellstart", "cellitems")
DIR_AT = 0x48
assert DIR_AT + 8 * len(TABLES) <= HEADER_SIZE

# link kinds (bit k of a kind mask)
K_WALK, K_STEP, K_VAULT, K_DROP, K_CLIMB, K_JUMP = range(6)
KIND_NAMES = ("walk", "step", "vault", "drop", "climb", "jump")
SYMMETRIC_KINDS = (K_WALK, K_STEP, K_CLIMB)

# sector flags
SF_BREAKABLE, SF_INVISIBLE, SF_CLIMBABLE, SF_TWO_SIDED = 1, 2, 4, 8
NO_SURFACE = 0xFFFF
NO_MATERIAL = 255

# Tunables.  World units (1 wu = 10 ft).  Player step height in the engine is 0.17, jump apex about 0.57.
WALK_NZ = 0.71            # the data's own walkable threshold (pathfinding surface bit 6)
STEP_MAX = 0.20
VAULT_MAX = 0.55
DROP_MAX = 3.0
CLIMB_MAX = 14.0
PLANE_TOL = 0.02          # merged sector: vertical deviation of any vertex from the sector plane
MAX_VERTS = 12            # per sector polygon (the reader allows 15)
CHAIN_DEPTH = 3
RISER_NZ_LO, RISER_NZ_HI = -0.35, WALK_NZ
PORTAL_MIN = 0.08
RUN_MAX, RUN_PER_RISE = 0.45, 1.5
BAND_EPS = 0.05
ANTI_PARALLEL = 0.9       # cos of the largest angle off exactly opposite directions of the two edges
COST_SCALE = 64           # cost units per wu (u16) for BSPs whose longest link allows it; else the largest power of two that fits (header word)
DZ_SCALE = 256            # link dz units per wu (s16)
WIDTH_SCALE = 16          # link width units per wu (u8, the link record's byte 3; 255 = unlimited: a ladder)
PENALTY = {K_WALK: 0.0, K_STEP: 0.03, K_VAULT: 0.6, K_DROP: 0.3, K_CLIMB: 1.0, K_JUMP: 1.0}
MAX_GRID = 96             # cells per axis
THICK_SCALE = 64          # sector thickness units per wu (u8; 255 = 3.98 wu or more)
HEAD_SCALE = 32           # sector headroom units per wu (u8; 255 = unobstructed up to 7.97 wu)
HEAD_CELL = 4.0           # plan grid of the overhead surfaces
HEAD_SAMPLE = 0.7         # headroom is the minimum over the centroid and the points 70 % of the way to each vertex
HEAD_IGNORE = 0.01        # an overhead surface this close above the floor is the floor's own two-sided twin
MAX_SECTORS = 0xFFFE

F_INVISIBLE, F_CLIMBABLE, F_BREAKABLE = collconv.F_INVISIBLE, collconv.F_CLIMBABLE, collconv.F_BREAKABLE


class PathError(ValueError):
    pass


# ---- input ----------------------------------------------------------------------------------------
@dataclass
class NavInput:
    """What the mesh builder needs of one collision BSP (the real reader fills it from the Xbox block, the tests
    build it by hand)."""
    verts: list[tuple[float, float, float]]
    polys: list[tuple[int, ...]]                 # vertex loop per surface, counter-clockwise about its front normal
    normals: list[tuple[float, float, float]]    # effective (front) unit normal per surface
    flags: list[int]                             # collision surface flags (1 two-sided, 2 invisible, 4 climbable, 8 breakable)
    material: list[int]                          # globals material type per surface, 255 none
    pf: bytes                                    # the "pathfinding surfaces" byte per surface
    col_index: list[int]                         # index of the surface in the COLn section, -1 when the section drops it
    name: str = ""
    dropped_walkable: int = 0                    # flagged surfaces the collision section drops (not used)


def read_pathfinding_bytes(block: bytes, loc: bsp.BspLocation, nsurf: int) -> bytes:
    """structure_bsp +0x1E4: one byte per collision surface."""
    A = loc.address
    root = struct.unpack_from("<I", block, 0)[0] - A
    if not 0 <= root <= len(block) - bsp.STRUCT_SIZE:
        raise PathError(f"{loc.path}: structure_bsp root outside the block")
    count, addr = struct.unpack_from("<II", block, root + 0x1E4)
    o = addr - A
    if count == 0:
        return bytes(nsurf)
    if count != nsurf or o < 0 or o + count > len(block):
        raise PathError(f"{loc.path}: pathfinding surfaces ({count} at {addr:#x}) do not match the {nsurf} collision surfaces")
    ecount, _ = struct.unpack_from("<II", block, root + 0x1F0)
    if ecount:
        raise PathError(f"{loc.path}: pathfinding edges present ({ecount}): layout unknown")
    return bytes(block[o:o + count])


def read_input(block: bytes, loc: bsp.BspLocation) -> NavInput:
    src = collconv.read_source(block, loc)
    polys = collconv.surface_polygons(src)
    pf = read_pathfinding_bytes(block, loc, len(src.surfaces))
    cp = collconv.compact(src, polys)
    col_index = [-1] * len(src.surfaces)
    for new, old in enumerate(cp.surf_old):
        col_index[old] = new
    normals, flags, material = [], [], []
    for s in src.surfaces:
        n = collconv.effective_plane(src, s[0])[:3]
        normals.append((n[0], n[1], n[2]))
        flags.append(s[2])
        m = src.palette[s[4]] if 0 <= s[4] < len(src.palette) else NO_MATERIAL
        material.append(m if m < 255 else NO_MATERIAL)
    verts = [(v[0], v[1], v[2]) for v in src.vertices]
    inp = NavInput(verts, polys, normals, flags, material, pf, col_index, name=src.name)
    inp.dropped_walkable = sum(1 for s in range(len(pf)) if pf[s] & 0x40 and col_index[s] < 0)
    return inp


# ---- geometry helpers --------------------------------------------------------------------------
def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def f32p(p) -> tuple[float, float, float]:
    return (f32(p[0]), f32(p[1]), f32(p[2]))


def area2(loop, V) -> float:
    """Twice the signed plan (xy) area, positive for counter-clockwise."""
    a = 0.0
    n = len(loop)
    for i in range(n):
        p, q = V[loop[i]], V[loop[(i + 1) % n]]
        a += p[0] * q[1] - q[0] * p[1]
    return a


def convex_plan(loop, V) -> bool:
    n = len(loop)
    for i in range(n):
        a, b, c = V[loop[i - 1]], V[loop[i]], V[loop[(i + 1) % n]]
        e1x, e1y, e2x, e2y = b[0] - a[0], b[1] - a[1], c[0] - b[0], c[1] - b[1]
        cr = e1x * e2y - e1y * e2x
        scale = max(math.hypot(e1x, e1y), math.hypot(e2x, e2y), 1e-9)
        if cr < -3e-4 * scale:
            return False
    return True


def fit_plane(loop, V):
    """(nx, ny, nz, d) of the area-weighted plane (n.p = d, n.z > 0) or None when degenerate / not upward."""
    pts = [V[i] for i in loop]
    nx, ny, nz = collconv.newell(pts)
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if ln < 1e-12 or nz <= 0.0:
        return None
    nx, ny, nz = nx / ln, ny / ln, nz / ln
    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)
    cz = sum(p[2] for p in pts) / len(pts)
    return (nx, ny, nz, nx * cx + ny * cy + nz * cz)


def z_on(plane, x: float, y: float) -> float:
    return (plane[3] - plane[0] * x - plane[1] * y) / plane[2]


def planar_ok(loop, V, tol: float = PLANE_TOL) -> bool:
    pl = fit_plane(loop, V)
    if pl is None or pl[2] < 0.70:
        return False
    return all(abs(V[i][2] - z_on(pl, V[i][0], V[i][1])) <= tol for i in loop)


def centroid_plan(loop, V):
    """Plan-area centroid (x, y); falls back to the vertex mean for a vanishing area."""
    a = 0.0
    cx = cy = 0.0
    n = len(loop)
    for i in range(n):
        p, q = V[loop[i]], V[loop[(i + 1) % n]]
        w = p[0] * q[1] - q[0] * p[1]
        a += w
        cx += (p[0] + q[0]) * w
        cy += (p[1] + q[1]) * w
    if abs(a) < 1e-12:
        return (sum(V[i][0] for i in loop) / n, sum(V[i][1] for i in loop) / n)
    return (cx / (3.0 * a), cy / (3.0 * a))


def dist3(a, b) -> float:
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def lerp3(a, b, t: float):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


# ---- the mesh ------------------------------------------------------------------------------------
@dataclass
class Sector:
    loop: list[int]                         # vertex ids of the input (counter-clockwise in plan)
    surfs: list[int]                        # source surfaces
    key: tuple
    area: float = 0.0
    alive: bool = True
    plane: tuple = ()
    centroid: tuple = ()
    surface: int = NO_SURFACE
    material: int = NO_MATERIAL
    flags: int = 0
    comp: int = 0
    headroom: int = 255
    thickness: int = 255


@dataclass
class Link:
    src: int
    dst: int
    kind: int
    a_right: tuple          # 3D points on the source sector (portal as seen walking from src to dst)
    a_left: tuple
    b_right: tuple          # same portal on the destination sector
    b_left: tuple
    cost: float = 0.0
    dz: float = 0.0         # destination minus source height at the portal midpoints


@dataclass
class NavMesh:
    sectors: list[Sector]
    links: list[Link]
    bounds: tuple
    ncomp: int
    stats: dict = field(default_factory=dict)


def _sector_key(inp: NavInput, s: int) -> tuple:
    return (inp.pf[s] & 0x80, inp.flags[s] & (F_INVISIBLE | F_CLIMBABLE | F_BREAKABLE), inp.material[s])


def merge_loops(LA: list[int], LB: list[int]):
    """Union of two counter-clockwise loops that share one contiguous edge run (directed edge (u, v) of A is (v, u) of B);
    None when they share no edge, or several separate runs, or the result repeats a vertex."""
    nA, nB = len(LA), len(LB)
    posB = {(LB[j], LB[(j + 1) % nB]): j for j in range(nB)}
    shared = [i for i in range(nA) if (LA[(i + 1) % nA], LA[i]) in posB]
    if not shared or len(shared) >= min(nA, nB):
        return None
    sset = set(shared)
    starts = [i for i in shared if (i - 1) % nA not in sset]
    if len(starts) != 1:
        return None
    s, k = starts[0], len(shared)
    jstart = posB[(LA[(s + k) % nA], LA[(s + k - 1) % nA])]
    for m in range(k):                                     # B's run must be the same edges, consecutive
        if posB.get((LA[(s + k - m) % nA], LA[(s + k - m - 1) % nA])) != (jstart + m) % nB:
            return None
    merged = [LA[(s + k + t) % nA] for t in range(nA - k + 1)]
    merged += [LB[(jstart + k + 1 + t) % nB] for t in range(nB - k - 1)]
    if len(set(merged)) != len(merged):
        return None
    return merged


def build_sectors(inp: NavInput, merge: bool = True) -> tuple[list[Sector], dict[int, int]]:
    """One sector per walkable surface, then greedy merging.  Returns (alive sectors renumbered, surface -> sector)."""
    V = inp.verts
    secs: list[Sector] = []
    for s, pf in enumerate(inp.pf):
        if not pf & 0x40 or inp.col_index[s] < 0:
            continue
        loop = list(inp.polys[s])
        a2 = area2(loop, V)
        if len(loop) < 3 or a2 <= 1e-12:
            continue                                   # degenerate (none on the retail maps)
        secs.append(Sector(loop, [s], _sector_key(inp, s), area=a2 * 0.5))
    owner: dict[tuple[int, int], int] = {}
    for i, sc in enumerate(secs):
        n = len(sc.loop)
        for j in range(n):
            owner[(sc.loop[j], sc.loop[(j + 1) % n])] = i
    merges = 0
    if merge:
        changed = True
        while changed:
            changed = False
            for i in sorted(range(len(secs)), key=lambda q: secs[q].area):
                S = secs[i]
                if not S.alive:
                    continue
                n = len(S.loop)
                cand = []
                for j in range(n):
                    t = owner.get((S.loop[(j + 1) % n], S.loop[j]))
                    if t is not None and t != i and t not in cand:
                        cand.append(t)
                for t in sorted(cand, key=lambda q: secs[q].area):
                    T = secs[t]
                    if not T.alive or T.key != S.key:
                        continue
                    merged = merge_loops(S.loop, T.loop)
                    if merged is None or len(merged) > MAX_VERTS or len(merged) < 3:
                        continue
                    if not convex_plan(merged, V) or not planar_ok(merged, V):
                        continue
                    for sc in (S, T):
                        m = len(sc.loop)
                        for j in range(m):
                            owner.pop((sc.loop[j], sc.loop[(j + 1) % m]), None)
                    S.loop = merged
                    S.surfs = S.surfs + T.surfs
                    S.area += T.area
                    T.alive = False
                    m = len(merged)
                    for j in range(m):
                        owner[(merged[j], merged[(j + 1) % m])] = i
                    merges += 1
                    changed = True
                    break
    alive = [sc for sc in secs if sc.alive]
    if len(alive) > MAX_SECTORS:
        raise PathError(f"{len(alive)} sectors do not fit u16 ids")
    sec_of = {}
    for i, sc in enumerate(alive):
        for s in sc.surfs:
            sec_of[s] = i
        big = max(sc.surfs, key=lambda q: abs(area2(inp.polys[q], V)))
        sc.surface = inp.col_index[big]
        sc.material = inp.material[big]
        fl = 0
        for s in sc.surfs:
            fl |= (SF_BREAKABLE if inp.pf[s] & 0x80 else 0) | (SF_INVISIBLE if inp.flags[s] & F_INVISIBLE else 0) \
                | (SF_CLIMBABLE if inp.flags[s] & F_CLIMBABLE else 0) | (SF_TWO_SIDED if inp.flags[s] & 1 else 0)
        sc.flags = fl
    return alive, sec_of


def link_cost(kind: int, cA, cB, pa_mid, pb_mid) -> float:
    c = dist3(cA, pa_mid) + dist3(pa_mid, pb_mid) + dist3(pb_mid, cB)
    pen = PENALTY[kind]
    dz = pb_mid[2] - pa_mid[2]
    if kind == K_DROP:
        pen += 0.5 * max(0.0, -dz)
    elif kind in (K_VAULT, K_CLIMB):
        pen += 0.5 * abs(dz)
    return c + pen


def min_width(loop, V) -> float:
    """Thickness of a convex sector: the smallest extent of its plan polygon across any of its edges."""
    pts = [(V[v][0], V[v][1]) for v in loop]
    n = len(pts)
    best = 1e9
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        L = math.hypot(ex, ey)
        if L < 1e-9:
            continue
        nx, ny = ey / L, -ex / L
        w = max((p[0] - ax) * nx + (p[1] - ay) * ny for p in pts) - min((p[0] - ax) * nx + (p[1] - ay) * ny for p in pts)
        best = min(best, w)
    return best


def _clip_convex(subject: list[tuple[float, float]], clipper: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Sutherland-Hodgman: the part of the convex polygon ``subject`` inside the convex counter-clockwise ``clipper`` (plan)."""
    out = subject
    n = len(clipper)
    for i in range(n):
        if not out:
            break
        ax, ay = clipper[i]
        bx, by = clipper[(i + 1) % n]
        inp, out = out, []
        m = len(inp)
        for j in range(m):
            p, q = inp[j], inp[(j + 1) % m]
            sp = (bx - ax) * (p[1] - ay) - (by - ay) * (p[0] - ax)
            sq = (bx - ax) * (q[1] - ay) - (by - ay) * (q[0] - ax)
            if sp >= 0:
                out.append(p)
            if (sp >= 0) != (sq >= 0):
                t = sp / (sp - sq)
                out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    return out


def compute_headroom(inp: NavInput, secs: list[Sector]) -> list[int]:
    """Per sector the free height above its floor, in 1/HEAD_SCALE wu (255: nothing within 7.97 wu): the lowest point, over the
    whole polygon, of any surface that faces down (normal z < -0.3: a ceiling, an overhang, the underside of a two-sided sheet)
    above it.  Every overhead polygon is clipped against the sector in plan and the clearance taken at the corners of the
    overlap (both planes are linear, so the minimum is there).  Stacked floors (a walkable sheet over a lower walkable layer),
    crawl spaces and hanging wall segments show up as small values; the host chooses which headroom its actors need
    (path_query.height)."""
    V = inp.verts
    grid: dict[tuple[int, int], list[int]] = defaultdict(list)
    for s, loop in enumerate(inp.polys):
        if inp.normals[s][2] >= -0.3 or inp.col_index[s] < 0:
            continue
        xs = [V[v][0] for v in loop]
        ys = [V[v][1] for v in loop]
        for gx in range(int(math.floor(min(xs) / HEAD_CELL)), int(math.floor(max(xs) / HEAD_CELL)) + 1):
            for gy in range(int(math.floor(min(ys) / HEAD_CELL)), int(math.floor(max(ys) / HEAD_CELL)) + 1):
                grid[(gx, gy)].append(s)
    out = []
    for sc in secs:
        poly = [(V[v][0], V[v][1]) for v in sc.loop]
        xs = [p[0] for p in poly]
        ys = [p[1] for p in poly]
        seen: set[int] = set()
        best = 1e9
        for gx in range(int(math.floor(min(xs) / HEAD_CELL)), int(math.floor(max(xs) / HEAD_CELL)) + 1):
            for gy in range(int(math.floor(min(ys) / HEAD_CELL)), int(math.floor(max(ys) / HEAD_CELL)) + 1):
                for s in grid.get((gx, gy), ()):
                    if s in seen:
                        continue
                    seen.add(s)
                    loop = inp.polys[s]
                    sub = [(V[v][0], V[v][1]) for v in loop]
                    if max(p[0] for p in sub) < min(xs) or min(p[0] for p in sub) > max(xs) or \
                            max(p[1] for p in sub) < min(ys) or min(p[1] for p in sub) > max(ys):
                        continue
                    if area2(loop, V) < 0:                  # a down-facing polygon is clockwise in plan: reverse it
                        sub = sub[::-1]
                    n = inp.normals[s]
                    a = V[loop[0]]
                    clip = _clip_convex(sub, poly)
                    if len(clip) < 3 or abs(sum(clip[i][0] * clip[(i + 1) % len(clip)][1] - clip[(i + 1) % len(clip)][0] * clip[i][1]
                                                 for i in range(len(clip)))) < 2e-6:
                        continue                        # only touching: no overlap in plan
                    for (x, y) in clip:
                        zz = a[2] - (n[0] * (x - a[0]) + n[1] * (y - a[1])) / n[2]
                        d = zz - z_on(sc.plane, x, y)
                        if d > HEAD_IGNORE and d < best:
                            best = d
        out.append(255 if best * HEAD_SCALE >= 255 else int(best * HEAD_SCALE))
    return out


def build_mesh(inp: NavInput, merge: bool = True) -> NavMesh:
    V = inp.verts
    secs, sec_of = build_sectors(inp, merge)
    n = len(secs)
    stats: dict = dict(walkable_surfaces=sum(1 for s, p in enumerate(inp.pf) if p & 0x40), sectors=n,
                       dropped_walkable=inp.dropped_walkable)
    # planes and centroids
    for sc in secs:
        pl = fit_plane(sc.loop, V)
        if pl is None:
            raise PathError("sector without a plane")
        sc.plane = pl
        cx, cy = centroid_plan(sc.loop, V)
        sc.centroid = (cx, cy, z_on(pl, cx, cy))
    for sc, hr in zip(secs, compute_headroom(inp, secs)):
        sc.headroom = hr
    for sc in secs:
        t = min_width(sc.loop, V) * THICK_SCALE
        sc.thickness = 255 if t >= 255 else int(t)
    stats["low_headroom_sectors"] = sum(1 for sc in secs if sc.headroom < 16)      # under 0.5 wu
    # directed edge -> surface over every surface (the mesh is closed: every directed edge has exactly one owner)
    # (the data is not a manifold where two-sided / invisible / climbable surfaces overlay a floor: a directed edge can have
    # several owners, so every lookup goes through the list and filters by what it needs)
    edge_owner: dict[tuple[int, int], list[int]] = defaultdict(list)
    for s, loop in enumerate(inp.polys):
        m = len(loop)
        for j in range(m):
            edge_owner[(loop[j], loop[(j + 1) % m])].append(s)
    links: dict[tuple, Link] = {}

    def add(l: Link) -> None:
        key = (l.src, l.dst, l.kind, l.a_right, l.a_left)
        if key not in links:
            links[key] = l

    def riser(s: int) -> bool:
        return RISER_NZ_LO <= inp.normals[s][2] < RISER_NZ_HI and not inp.pf[s] & 0x40

    # directed edges of the final sector loops (overlapping floors can give an edge two owners: every owner is a neighbour)
    sec_owner: dict[tuple[int, int], list[int]] = defaultdict(list)
    for i, sc in enumerate(secs):
        m = len(sc.loop)
        for j in range(m):
            sec_owner[(sc.loop[j], sc.loop[(j + 1) % m])].append(i)

    # --- WALK links: runs of edges whose reverse belongs to the same sector, around each loop
    sec_edges: list[list[tuple[list[int], list[int]]]] = []
    for i, sc in enumerate(secs):
        loop = sc.loop
        m = len(loop)
        nb, nbs = [], []
        for j in range(m):
            a, b = loop[j], loop[(j + 1) % m]
            nbs.append([t for t in sec_owner.get((b, a), ()) if t != i])
            nb.append([q for q in edge_owner.get((b, a), ()) if q not in sec_of and riser(q)])
        sec_edges.append(list(zip(nb, nbs)))
        for t in sorted({t for w in nbs for t in w}):
            flags = [t in w for w in nbs]
            if all(flags):
                raise PathError("a sector entirely surrounded by one walkable neighbour")
            for j0 in [j for j in range(m) if flags[j] and not flags[j - 1]]:
                k = 0
                while flags[(j0 + k) % m] and k < m:
                    k += 1
                p0, p1 = V[loop[j0]], V[loop[(j0 + k) % m]]
                right, left = f32p(p0), f32p(p1)
                cost = link_cost(K_WALK, sc.centroid, secs[t].centroid, lerp3(right, left, 0.5), lerp3(right, left, 0.5))
                add(Link(i, t, K_WALK, right, left, right, left, cost, 0.0))
    stats["walk_links"] = len(links)
    # --- STEP / VAULT / DROP over risers
    pairs: dict[tuple, tuple] = {}
    for i, sc in enumerate(secs):
        loop = sc.loop
        m = len(loop)
        for j in range(m):
            risers, nsec = sec_edges[i][j]
            if nsec or not risers:
                continue
            a, b = V[loop[j]], V[loop[(j + 1) % m]]
            seen = set(risers)
            frontier = list(risers)
            zlo_c = min(V[v][2] for q in risers for v in inp.polys[q])
            zhi_c = max(V[v][2] for q in risers for v in inp.polys[q])
            found = []
            for _depth in range(CHAIN_DEPTH):
                nxt = []
                for q in frontier:
                    ql = inp.polys[q]
                    qm = len(ql)
                    for e in range(qm):
                        x, y = ql[e], ql[(e + 1) % qm]
                        for o in edge_owner.get((y, x), ()):
                            if o in seen:
                                continue
                            if o in sec_of:
                                t = sec_of[o]
                                if t != i and t in sec_owner.get((y, x), ()):
                                    found.append((t, (y, x), (zlo_c, zhi_c)))
                            elif riser(o):
                                seen.add(o)
                                nxt.append(o)
                for o in nxt:                      # the next layer's heights count from the next layer on
                    for v in inp.polys[o]:
                        zlo_c = min(zlo_c, V[v][2])
                        zhi_c = max(zhi_c, V[v][2])
                frontier = nxt
                if not frontier:
                    break
            for t, (y, x), band in found:
                c, d = V[y], V[x]                       # B's edge, traversed y -> x in B's loop
                hit = _step_portal(a, b, c, d, band)
                if hit is None:
                    continue
                (p0, p1), (q0, q1) = hit
                ra = tuple(sorted((tuple(round(v, 2) for v in p0), tuple(round(v, 2) for v in p1))))
                rb = tuple(sorted((tuple(round(v, 2) for v in q0), tuple(round(v, 2) for v in q1))))
                key = (i, t, ra, rb) if i < t else (t, i, rb, ra)
                if key in pairs:
                    continue
                pairs[key] = (i, t, p0, p1, q0, q1)
    for (i, t, p0, p1, q0, q1) in pairs.values():
        ma, mb = lerp3(p0, p1, 0.5), lerp3(q0, q1, 0.5)
        rise = mb[2] - ma[2]
        fwd = _kind_for(rise)
        back = _kind_for(-rise)
        if fwd is not None:
            add(Link(i, t, fwd, f32p(p0), f32p(p1), f32p(q0), f32p(q1),
                     link_cost(fwd, secs[i].centroid, secs[t].centroid, ma, mb), rise))
        if back is not None:
            add(Link(t, i, back, f32p(q1), f32p(q0), f32p(p1), f32p(p0),
                     link_cost(back, secs[t].centroid, secs[i].centroid, mb, ma), -rise))
    stats["step_links"] = sum(1 for l in links.values() if l.kind == K_STEP)
    stats["vault_links"] = sum(1 for l in links.values() if l.kind == K_VAULT)
    stats["drop_links"] = sum(1 for l in links.values() if l.kind == K_DROP)
    # --- CLIMB links
    for l in _climb_links(inp, secs, sec_of):
        add(l)
    stats["climb_links"] = sum(1 for l in links.values() if l.kind == K_CLIMB)
    ordered = sorted(links.values(), key=lambda l: (l.src, l.dst, l.kind, l.a_right, l.a_left))
    for l in ordered:
        if abs(l.dz) * DZ_SCALE > 32767:
            raise PathError(f"link dz {l.dz:.1f} does not fit")
    comp = _components(n, ordered)
    for sc, c in zip(secs, comp[0]):
        sc.comp = c
    pts = [V[v] for sc in secs for v in sc.loop]
    if pts:
        bounds = tuple(min(p[k] for p in pts) for k in range(3)) + tuple(max(p[k] for p in pts) for k in range(3))
    else:
        bounds = (0.0,) * 6
    return NavMesh(secs, ordered, bounds, comp[1], stats)


def _kind_for(rise: float):
    """Kind of the link that climbs ``rise`` (negative: down)."""
    if abs(rise) <= STEP_MAX:
        return K_STEP
    if rise > 0:
        return K_VAULT if rise <= VAULT_MAX else None
    return K_DROP if -rise <= DROP_MAX else None


def _step_portal(a, b, c, d, band_z):
    """Portal between sector edge a -> b (A's edge, interior on its left) and B's edge c -> d across a riser, or None.
    Returns ((pA0, pA1), (pB0, pB1)): 3D end points of the overlap on A's edge and on B's edge, p0 at the 'right'
    (start) end as seen walking from A to B.  The two edges must be anti-parallel, overlap in plan by at least
    PORTAL_MIN, lie at most RUN_MAX + RUN_PER_RISE x rise apart (B in front of A's edge) and the riser's vertices must
    stay inside the vertical band of the edges (+ BAND_EPS)."""
    ex, ey = b[0] - a[0], b[1] - a[1]
    L = math.hypot(ex, ey)
    fx, fy = d[0] - c[0], d[1] - c[1]
    M = math.hypot(fx, fy)
    if L < 1e-6 or M < 1e-6:
        return None
    ux, uy = ex / L, ey / L
    if (fx * ux + fy * uy) / M > -ANTI_PARALLEL:
        return None
    # B's edge in the parameter of A's edge (t along a -> b in plan, in wu); c -> d runs backwards along it
    tc = (c[0] - a[0]) * ux + (c[1] - a[1]) * uy
    td = (d[0] - a[0]) * ux + (d[1] - a[1]) * uy
    lo, hi = max(0.0, min(tc, td)), min(L, max(tc, td))
    if hi - lo < PORTAL_MIN:
        return None
    # B in front of A's edge: the outward (right) normal of a -> b is (uy, -ux)
    off = ((c[0] + d[0]) * 0.5 - a[0]) * uy - ((c[1] + d[1]) * 0.5 - a[1]) * ux
    zlo = min(a[2], b[2], c[2], d[2])
    zhi = max(a[2], b[2], c[2], d[2])
    rise = ((c[2] + d[2]) - (a[2] + b[2])) * 0.5
    if off < -0.02 or off > RUN_MAX + RUN_PER_RISE * abs(rise):
        return None
    if band_z[0] < zlo - BAND_EPS or band_z[1] > zhi + BAND_EPS:
        return None
    ta, tb = lo / L, hi / L
    pA0, pA1 = lerp3(a, b, ta), lerp3(a, b, tb)
    # position of the same plan parameters on c -> d: t = tc + (td - tc) * u
    def on_cd(t: float):
        u = (t - tc) / (td - tc) if abs(td - tc) > 1e-12 else 0.0
        return lerp3(c, d, min(1.0, max(0.0, u)))
    # c -> d runs from t = tc (high) down to td (low) when tc > td: B's point at a-parameter t
    pB0, pB1 = on_cd(lo), on_cd(hi)
    return (pA0, pA1), (pB0, pB1)


def _climb_links(inp: NavInput, secs: list[Sector], sec_of: dict[int, int]) -> list[Link]:
    """CLIMB links for components of climbable surfaces (ladder volumes): the lowest and the highest walkable,
    non-climbable sector whose boundary lies within ``CLIMB_REACH`` (plan) of the component's footprint."""
    V = inp.verts
    climb = [s for s, f in enumerate(inp.flags) if f & F_CLIMBABLE and inp.col_index[s] >= 0]
    if not climb:
        return []
    vert_surfs: dict[int, list[int]] = defaultdict(list)
    for s in climb:
        for v in inp.polys[s]:
            vert_surfs[v].append(s)
    seen: set[int] = set()
    comps: list[list[int]] = []
    for s in climb:
        if s in seen:
            continue
        seen.add(s)
        st, comp = [s], []
        while st:
            u = st.pop()
            comp.append(u)
            for v in inp.polys[u]:
                for w in vert_surfs[v]:
                    if w not in seen:
                        seen.add(w)
                        st.append(w)
        comps.append(comp)
    # index sectors by plan cells for the reach search
    cell = 2.0
    grid: dict[tuple[int, int], list[int]] = defaultdict(list)
    for i, sc in enumerate(secs):
        xs = [V[v][0] for v in sc.loop]
        ys = [V[v][1] for v in sc.loop]
        for gx in range(int(math.floor(min(xs) / cell)), int(math.floor(max(xs) / cell)) + 1):
            for gy in range(int(math.floor(min(ys) / cell)), int(math.floor(max(ys) / cell)) + 1):
                grid[(gx, gy)].append(i)
    out: list[Link] = []
    for comp in comps:
        vs = {v for s in comp for v in inp.polys[s]}
        xs = [V[v][0] for v in vs]
        ys = [V[v][1] for v in vs]
        zs = [V[v][2] for v in vs]
        zmin, zmax = min(zs), max(zs)
        if zmax - zmin < 3 * STEP_MAX:
            continue                                   # a flat climbable patch is not a ladder
        reach = 0.4
        x0, x1, y0, y1 = min(xs) - reach, max(xs) + reach, min(ys) - reach, max(ys) + reach
        cand = set()
        for gx in range(int(math.floor(x0 / cell)), int(math.floor(x1 / cell)) + 1):
            for gy in range(int(math.floor(y0 / cell)), int(math.floor(y1 / cell)) + 1):
                cand.update(grid.get((gx, gy), ()))
        cx, cy = (min(xs) + max(xs)) * 0.5, (min(ys) + max(ys)) * 0.5
        best_lo = best_hi = None
        for i in cand:
            sc = secs[i]
            if sc.flags & SF_CLIMBABLE:
                continue
            sx = [V[v][0] for v in sc.loop]
            sy = [V[v][1] for v in sc.loop]
            if max(sx) < x0 or min(sx) > x1 or max(sy) < y0 or min(sy) > y1:
                continue
            p = _closest_on_loop(sc, V, cx, cy)
            z = p[2]
            if math.hypot(p[0] - cx, p[1] - cy) > 0.5 * max(max(xs) - min(xs), max(ys) - min(ys)) + 0.6:
                continue
            if abs(z - zmin) <= 0.35 and (best_lo is None or abs(z - zmin) < abs(best_lo[1][2] - zmin)):
                best_lo = (i, p)
            if abs(z - zmax) <= 0.35 and (best_hi is None or abs(z - zmax) < abs(best_hi[1][2] - zmax)):
                best_hi = (i, p)
        if best_lo and best_hi and best_lo[0] != best_hi[0]:
            (i, p), (t, q) = best_lo, best_hi
            rise = q[2] - p[2]
            fp, fq = f32p(p), f32p(q)
            out.append(Link(i, t, K_CLIMB, fp, fp, fq, fq, link_cost(K_CLIMB, secs[i].centroid, secs[t].centroid, p, q), rise))
            out.append(Link(t, i, K_CLIMB, fq, fq, fp, fp, link_cost(K_CLIMB, secs[t].centroid, secs[i].centroid, q, p), -rise))
    return out


def _closest_on_loop(sc: Sector, V, x: float, y: float):
    """Closest point (plan) of the sector polygon to (x, y), lifted onto the sector plane."""
    loop = sc.loop
    n = len(loop)
    inside = True
    best = (1e30, None)
    for j in range(n):
        p, q = V[loop[j]], V[loop[(j + 1) % n]]
        ex, ey = q[0] - p[0], q[1] - p[1]
        if ex * (y - p[1]) - ey * (x - p[0]) < 0:
            inside = False
        L2 = ex * ex + ey * ey
        t = 0.0 if L2 < 1e-18 else max(0.0, min(1.0, ((x - p[0]) * ex + (y - p[1]) * ey) / L2))
        px, py = p[0] + ex * t, p[1] + ey * t
        dd = (px - x) ** 2 + (py - y) ** 2
        if dd < best[0]:
            best = (dd, (px, py))
    px, py = (x, y) if inside else best[1]
    return (px, py, z_on(sc.plane, px, py))


def _components(n: int, links: list[Link]) -> tuple[list[int], int]:
    parent = list(range(n))

    def find(a: int) -> int:
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for l in links:
        if l.kind in SYMMETRIC_KINDS:
            ra, rb = find(l.src), find(l.dst)
            if ra != rb:
                parent[max(ra, rb)] = min(ra, rb)
    ids: dict[int, int] = {}
    out = []
    for i in range(n):
        r = find(i)
        if r not in ids:
            ids[r] = len(ids)
        out.append(ids[r])
    return out, len(ids)


# ---- serialisation ---------------------------------------------------------------------------------
def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def _poly_hits_rect(poly, rx0, ry0, rx1, ry1) -> bool:
    """Convex polygon (counter-clockwise, plan) versus an axis-aligned rectangle: separating-axis test over the polygon's
    edges (the rectangle's own axes are the caller's bounding-box test)."""
    n = len(poly)
    for j in range(n):
        ax, ay = poly[j]
        bx, by = poly[(j + 1) % n]
        ex, ey = bx - ax, by - ay
        # outward normal of a counter-clockwise edge is (ey, -ex); the rectangle is outside when every corner is beyond
        out = True
        for cx, cy in ((rx0, ry0), (rx1, ry0), (rx1, ry1), (rx0, ry1)):
            if (cx - ax) * ey - (cy - ay) * ex <= 1e-9:
                out = False
                break
        if out:
            return False
    return True


def _grid_for(cell: float, x0: float, y0: float, w: float, h: float, bb, polys):
    nx = max(1, int(math.floor((w + 2e-3) / cell)) + 1)
    ny = max(1, int(math.floor((h + 2e-3) / cell)) + 1)
    cells: list[list[int]] = [[] for _ in range(nx * ny)]
    for i, (bx0, by0, bx1, by1) in enumerate(bb):
        gx0 = max(0, int(math.floor((bx0 - 1e-4 - x0) / cell)))
        gx1 = min(nx - 1, int(math.floor((bx1 + 1e-4 - x0) / cell)))
        gy0 = max(0, int(math.floor((by0 - 1e-4 - y0) / cell)))
        gy1 = min(ny - 1, int(math.floor((by1 + 1e-4 - y0) / cell)))
        whole = (gx1 - gx0 + 1) * (gy1 - gy0 + 1) <= 2
        for gy in range(gy0, gy1 + 1):
            for gx in range(gx0, gx1 + 1):
                if whole or _poly_hits_rect(polys[i], x0 + gx * cell - 2e-4, y0 + gy * cell - 2e-4,
                                            x0 + (gx + 1) * cell + 2e-4, y0 + (gy + 1) * cell + 2e-4):
                    cells[gy * nx + gx].append(i)
    start, items = [0], []
    for c in cells:
        items.extend(c)
        start.append(len(items))
    return nx, ny, start, items


def build_grid(secs: list[Sector], V) -> tuple[float, float, float, int, int, list[int], list[int]]:
    """Uniform plan grid over the sectors (polygon against cell, conservative by 0.2 mm): (cell, x0, y0, nx, ny,
    cell_start, items).  The cell size is the best of a few multiples of the median sector extent (fewest bytes)."""
    if not secs:
        return 1.0, 0.0, 0.0, 1, 1, [0, 0], []
    polys = [[(V[v][0], V[v][1]) for v in sc.loop] for sc in secs]
    bb = [(min(p[0] for p in q), min(p[1] for p in q), max(p[0] for p in q), max(p[1] for p in q)) for q in polys]
    x0 = min(b[0] for b in bb)
    y0 = min(b[1] for b in bb)
    w = max(b[2] for b in bb) - x0
    h = max(b[3] for b in bb) - y0
    ext = sorted(max(b[2] - b[0], b[3] - b[1]) for b in bb)
    x0, y0 = f32(x0 - 1e-3), f32(y0 - 1e-3)
    best = None
    for mult in (1.0, 1.5, 2.0, 3.0, 4.0):
        cell = f32(max(mult * ext[len(ext) // 2], 0.5, w / MAX_GRID, h / MAX_GRID))
        nx, ny, start, items = _grid_for(cell, x0, y0, w, h, bb, polys)
        size = 4 * len(start) + 2 * len(items)
        if best is None or size < best[0]:
            best = (size, cell, nx, ny, start, items)
        if cell <= max(0.5, w / MAX_GRID, h / MAX_GRID) + 1e-6:
            break
    _s, cell, nx, ny, start, items = best
    return cell, x0, y0, nx, ny, start, items


def compress_loops(secs: list[Sector], links: list[Link], V) -> list[list[int]]:
    """Per sector, the loop without vertices that are collinear and carry no portal end point and no change of
    neighbour; the loop keeps the portal vertices of every link (their end points are loop vertices)."""
    need: list[set[tuple]] = [set() for _ in secs]
    for l in links:
        for p in (l.a_right, l.a_left):
            need[l.src].add(p)
    out = []
    for i, sc in enumerate(secs):
        loop = sc.loop
        keep = []
        n = len(loop)
        for j in range(n):
            a, b, c = V[loop[j - 1]], V[loop[j]], V[loop[(j + 1) % n]]
            e1x, e1y, e2x, e2y = b[0] - a[0], b[1] - a[1], c[0] - b[0], c[1] - b[1]
            cr = e1x * e2y - e1y * e2x
            L = max(math.hypot(e1x, e1y), 1e-9)
            colinear = abs(cr) / L < 1e-6 and (e1x * e2x + e1y * e2y) > 0 and abs(b[2] - (a[2] + c[2]) * 0.5) < 1e-4
            if colinear and f32p(b) not in need[i]:
                continue
            keep.append(loop[j])
        if len(keep) < 3:
            keep = list(loop)
        out.append(keep)
    return out


def build_section(mesh: NavMesh, V) -> bytes:
    secs, links = mesh.sectors, mesh.links
    loops = compress_loops(secs, links, V)
    points: dict[tuple, int] = {}
    plist: list[tuple] = []

    def pt(p) -> int:
        key = f32p(p)
        if key not in points:
            if len(plist) >= 0xFFFF:
                raise PathError("more than 65535 points")
            points[key] = len(plist)
            plist.append(key)
        return points[key]

    index_data = bytearray()
    first_index = []
    nverts = []
    for loop in loops:
        first_index.append(len(index_data) // 2)
        nverts.append(len(loop))
        for v in loop:
            index_data += struct.pack("<H", pt(V[v]))
    # links grouped per source sector (already sorted by src)
    link_first = [0] * (len(secs) + 1)
    for l in links:
        link_first[l.src + 1] += 1
    for i in range(len(secs)):
        link_first[i + 1] += link_first[i]
    link_data = bytearray()
    cost_scale = COST_SCALE
    max_cost = max((l.cost for l in links), default=0.0)
    while cost_scale > 1 and max_cost * cost_scale > 65535 - 1:
        cost_scale //= 2
    if max_cost * cost_scale > 65535 - 1:
        raise PathError(f"link cost {max_cost:.1f} wu does not fit u16")
    for l in links:
        cost = int(math.ceil(l.cost * cost_scale - 1e-9))
        width = 255 if l.kind == K_CLIMB else min(254, int(round(math.hypot(l.a_left[0] - l.a_right[0], l.a_left[1] - l.a_right[1]) * WIDTH_SCALE)))
        link_data += struct.pack("<HBB2HHh", l.dst, l.kind, width, pt(l.a_right), pt(l.a_left), cost, int(round(l.dz * DZ_SCALE)))
    cell, gx0, gy0, nx, ny, cstart, citems = build_grid(secs, V)
    out = bytearray(HEADER_SIZE)
    offs: dict[str, int] = {}
    offs["sectors"] = len(out)
    for i, sc in enumerate(secs):
        cx, cy, cz = sc.centroid
        out += struct.pack("<3fIIHBBHHBBBB", cx, cy, cz, first_index[i], link_first[i], link_first[i + 1] - link_first[i],
                           nverts[i], sc.flags, sc.comp, sc.surface, sc.material, sc.headroom, sc.thickness, 0)
    _pad16(out)
    offs["points"] = len(out)
    for p in plist:
        out += struct.pack("<3f", *p)
    _pad16(out)
    offs["indices"] = len(out)
    out += index_data
    _pad16(out)
    offs["links"] = len(out)
    out += link_data
    _pad16(out)
    offs["cellstart"] = len(out)
    out += struct.pack(f"<{len(cstart)}I", *cstart)
    _pad16(out)
    offs["cellitems"] = len(out)
    out += struct.pack(f"<{len(citems)}H", *citems)
    _pad16(out)
    counts = {"sectors": len(secs), "points": len(plist), "indices": len(index_data) // 2, "links": len(links),
              "cellstart": len(cstart), "cellitems": len(citems)}
    struct.pack_into("<4sHHII6ffffHHfffI", out, 0, MAGIC, VERSION, 0, len(out), mesh.ncomp, *mesh.bounds,
                     cell, gx0, gy0, nx, ny, STEP_MAX, VAULT_MAX, DROP_MAX, cost_scale)
    for i, name in enumerate(TABLES):
        struct.pack_into("<II", out, DIR_AT + 8 * i, offs[name], counts[name])
    return bytes(out)


# ---- reference parser ----------------------------------------------------------------------------------
@dataclass
class ParsedSection:
    size: int
    ncomp: int
    bounds: tuple
    cell: float
    origin: tuple
    grid: tuple
    thresholds: tuple
    cost_scale: int
    sectors: list[dict]
    points: list[tuple]
    indices: list[int]
    links: list[dict]
    cell_start: list[int]
    cell_items: list[int]


def parse_section(data: bytes) -> ParsedSection:
    """Reference reader (the C reader is engine/src/ai/path.c); raises PathError on a bad section."""
    if len(data) < HEADER_SIZE:
        raise PathError("section shorter than the header")
    (magic, version, _r, size, ncomp, *rest) = struct.unpack_from("<4sHHII6ffffHHfffI", data)
    bounds = tuple(rest[:6])
    cell, gx0, gy0, nx, ny, smax, vmax, dmax, cost_scale = rest[6:]
    if magic != MAGIC:
        raise PathError("bad signature")
    if version != VERSION:
        raise PathError(f"unsupported version {version}")
    if size != len(data):
        raise PathError("inconsistent size")
    d = {name: struct.unpack_from("<II", data, DIR_AT + 8 * i) for i, name in enumerate(TABLES)}
    recs = dict(sectors=SECTOR_REC, points=POINT_REC, indices=INDEX_REC, links=LINK_REC, cellstart=CELLSTART_REC,
                cellitems=CELLITEM_REC)
    for name, (off, cnt) in d.items():
        if off % 16 or off < HEADER_SIZE or off + cnt * recs[name] > len(data):
            raise PathError(f"{name} table outside the section")
    sectors = []
    for i in range(d["sectors"][1]):
        cx, cy, cz, fi, fl, lc, nv, flg, comp, surf, mat, head, thick, _r2 = struct.unpack_from(
            "<3fIIHBBHHBBBB", data, d["sectors"][0] + i * SECTOR_REC)
        sectors.append(dict(centroid=(cx, cy, cz), first_index=fi, first_link=fl, link_count=lc, nverts=nv, flags=flg,
                            comp=comp, surface=surf, material=mat, headroom=head, thickness=thick))
    points = list(struct.iter_unpack("<3f", data[d["points"][0]:d["points"][0] + d["points"][1] * POINT_REC]))
    indices = list(struct.unpack_from(f"<{d['indices'][1]}H", data, d["indices"][0]))
    links = []
    for i in range(d["links"][1]):
        to, kind, fl, ar, al, cost, dz = struct.unpack_from("<HBB2HHh", data, d["links"][0] + i * LINK_REC)
        links.append(dict(to=to, kind=kind, width=fl, a_right=ar, a_left=al, cost=cost, dz=dz))
    cstart = list(struct.unpack_from(f"<{d['cellstart'][1]}I", data, d["cellstart"][0]))
    citems = list(struct.unpack_from(f"<{d['cellitems'][1]}H", data, d["cellitems"][0]))
    for s in sectors:
        if s["first_index"] + s["nverts"] > len(indices) or s["first_link"] + s["link_count"] > len(links):
            raise PathError("sector ranges outside their tables")
    return ParsedSection(size, ncomp, bounds, cell, (gx0, gy0), (nx, ny), (smax, vmax, dmax), cost_scale, sectors, points, indices,
                         links, cstart, citems)


def section_tag(index: int) -> str:
    """Pack tag of BSP ``index``: PAT0..PAT9, then PATA..PATZ (same index order as BSPn / COLn)."""
    if not 0 <= index < 36:
        raise PathError(f"BSP index {index} does not fit a four-character tag")
    return "PAT" + "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[index]


# ---- verification (works on a mesh + input; used by --verify and the tests) -----------------------------------------------
@dataclass
class Report:
    name: str
    problems: list[str] = field(default_factory=list)
    counts: dict = field(default_factory=dict)


def verify_mesh(inp: NavInput, mesh: NavMesh) -> Report:
    V = inp.verts
    rep = Report(inp.name)
    P = rep.problems
    secs, links = mesh.sectors, mesh.links
    walk = [s for s, p in enumerate(inp.pf) if p & 0x40 and inp.col_index[s] >= 0]
    # the data's own rule
    nz_bad = sum(1 for s in range(len(inp.pf))
                 if abs(inp.normals[s][2] - WALK_NZ) > 1e-3 and bool(inp.pf[s] & 0x40) != (inp.normals[s][2] > WALK_NZ))
    brk_bad = sum(1 for s in range(len(inp.pf)) if bool(inp.pf[s] & 0x80) != bool(inp.pf[s] & 0x40 and inp.flags[s] & F_BREAKABLE))
    other_bits = sum(1 for p in inp.pf if p & ~0xC0)
    rep.counts.update(walkable=len(walk), nz_rule_exceptions=nz_bad, breakable_rule_exceptions=brk_bad, unknown_bits=other_bits)
    if nz_bad:
        P.append(f"{nz_bad} surfaces break the nz >= 0.71 rule")
    if brk_bad:
        P.append(f"{brk_bad} surfaces break the breakable rule")
    if other_bits:
        P.append(f"{other_bits} surfaces carry unknown pathfinding bits")
    # every walkable surface in exactly one sector; area conserved
    used = Counter(s for sc in secs for s in sc.surfs)
    if sorted(used) != walk or any(v != 1 for v in used.values()):
        P.append("walkable surfaces are not covered exactly once by the sectors")
    a_src = sum(abs(area2(inp.polys[s], V)) * 0.5 for s in walk)
    a_sec = sum(area2(sc.loop, V) * 0.5 for sc in secs)
    if abs(a_src - a_sec) > 1e-6 * max(a_src, 1.0):
        P.append(f"plan area {a_sec:.6f} != sum of the surfaces {a_src:.6f}")
    maxdev = 0.0
    for i, sc in enumerate(secs):
        if not 3 <= len(sc.loop) <= MAX_VERTS:
            P.append(f"sector {i}: {len(sc.loop)} vertices")
        if area2(sc.loop, V) <= 0 or not convex_plan(sc.loop, V):
            P.append(f"sector {i}: not convex / not counter-clockwise in plan")
        pl = fit_plane(sc.loop, V)
        if pl is None or pl[2] < 0.70:
            P.append(f"sector {i}: plane unusable")
            continue
        maxdev = max(maxdev, max(abs(V[v][2] - z_on(pl, V[v][0], V[v][1])) for v in sc.loop))
    if maxdev > PLANE_TOL + 1e-9:
        P.append(f"sector vertices up to {maxdev:.4f} off their plane")
    rep.counts["max_plane_dev"] = maxdev
    # vertices inside the BSP's vertex bounds
    if inp.verts and mesh.sectors:
        lo = [min(v[k] for v in inp.verts) for k in range(3)]
        hi = [max(v[k] for v in inp.verts) for k in range(3)]
        b = mesh.bounds
        if any(b[k] < lo[k] - 1e-4 or b[3 + k] > hi[k] + 1e-4 for k in range(3)):
            P.append("sector bounds outside the BSP's vertex bounds")
    # links: sectors in range, WALK portals are loop vertices of both sides, reverse link exists
    pairs = {(l.src, l.dst, l.kind) for l in links}
    n = len(secs)
    for l in links:
        if not (0 <= l.src < n and 0 <= l.dst < n) or l.src == l.dst:
            P.append(f"link {l.src}->{l.dst} out of range")
            continue
        if l.kind == K_WALK:
            ls = {f32p(V[v]) for v in secs[l.src].loop}
            ld = {f32p(V[v]) for v in secs[l.dst].loop}
            if not ({l.a_right, l.a_left} <= ls and {l.b_right, l.b_left} <= ld) or l.a_right != l.b_right or l.a_left != l.b_left:
                P.append(f"walk link {l.src}->{l.dst}: portal ends are not shared loop vertices")
            if (l.dst, l.src, K_WALK) not in pairs:
                P.append(f"walk link {l.src}->{l.dst} has no reverse")
        elif l.kind in (K_STEP, K_CLIMB):
            if (l.dst, l.src, l.kind) not in pairs:
                P.append(f"{KIND_NAMES[l.kind]} link {l.src}->{l.dst} has no reverse")
        elif l.kind == K_VAULT and (l.dst, l.src, K_DROP) not in pairs:
            P.append(f"vault link {l.src}->{l.dst} has no drop back")
        d = dist3(secs[l.src].centroid, secs[l.dst].centroid)
        if l.cost + 1e-9 < d:
            P.append(f"link {l.src}->{l.dst}: cost {l.cost:.3f} below the centroid distance {d:.3f}")
    c = Counter(l.kind for l in links)
    rep.counts.update(sectors=n, links=len(links), **{f"{KIND_NAMES[k]}_links": v for k, v in sorted(c.items())})
    # components consistent with the symmetric links
    for l in links:
        if l.kind in SYMMETRIC_KINDS and secs[l.src].comp != secs[l.dst].comp:
            P.append("component ids disagree with a symmetric link")
            break
    return rep


# ---- driver -------------------------------------------------------------------------------------
def _blocks(path: Path):
    for i, loc, block in collconv._blocks(path):
        yield i, loc, block


def convert_block(block: bytes, loc: bsp.BspLocation) -> tuple[bytes, NavInput, NavMesh]:
    inp = read_input(block, loc)
    mesh = build_mesh(inp)
    return build_section(mesh, inp.verts), inp, mesh


def convert_map(path: Path, bsp_indices: list[int] | None = None) -> list[tuple[str, bytes]]:
    out = []
    for i, loc, block in _blocks(path):
        if bsp_indices is not None and i not in bsp_indices:
            continue
        out.append((section_tag(i), convert_block(block, loc)[0]))
    return out


def describe(tag: str, data: bytes) -> str:
    s = parse_section(data)
    kinds = Counter(l["kind"] for l in s.links)
    kd = " ".join(f"{KIND_NAMES[k]} {v}" for k, v in sorted(kinds.items()))
    return (f"{tag}: {len(data) / 1024:.1f} KiB, {len(s.sectors)} sectors ({len(s.indices)} loop entries), {len(s.points)} points, "
            f"{len(s.links)} links ({kd}), {s.ncomp} components, grid {s.grid[0]}x{s.grid[1]} cells of {s.cell:.2f} wu "
            f"({len(s.cell_items)} items)")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--bsp", type=int, action="append", help="only this BSP index (repeatable)")
    ap.add_argument("--info", action="store_true", help="input is a .hp2: print its PAT sections")
    ap.add_argument("--verify", action="store_true", help="input is a map: build every mesh and run the checks")
    args = ap.parse_args(argv)
    if args.info:
        pk = hp2.parse(args.input.read_bytes())
        for s in pk.sections:
            if s.tag.startswith("PAT"):
                print(describe(s.tag, pk.read(s.tag)))
        return 0
    if args.verify:
        rc = 0
        for i, loc, block in _blocks(args.input):
            if args.bsp and i not in args.bsp:
                continue
            inp = read_input(block, loc)
            mesh = build_mesh(inp)
            rep = verify_mesh(inp, mesh)
            print(f"{args.input.stem} {i} {inp.name}: {rep.counts} stats {mesh.stats}")
            for p in rep.problems:
                print("  problem:", p, file=sys.stderr)
                rc = 1
        return rc
    if not args.out:
        ap.error("-o is required")
    secs = convert_map(args.input, args.bsp)
    for tag, blob in secs:
        if len(blob) > 300 * 1024:
            print(f"note: {args.input.stem} {tag}: {len(blob) / 1024:.0f} KiB raw is above the 300 KiB target", file=sys.stderr)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, secs))
    print(f"wrote {args.out} ({sum(len(b) for _, b in secs) / 1024:.0f} KiB raw in {len(secs)} sections)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
