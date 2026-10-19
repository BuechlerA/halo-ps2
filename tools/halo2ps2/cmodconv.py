#!/usr/bin/env python3
"""Convert the Xbox `collision_model` tags (group `coll`) of a map, and the object tags that use them, into the PS2
``CMOD`` section, version 1 (docs/16-cmod-section.md; reader engine/src/world/cmod.c, contract cmod.h).

The goal is hit detection and damage regions, not physics: per (region, permutation, node, surface material) one
conservative node-local oriented box built from the model's collision geometry, plus the damage constants and tables
the hit needs (materials with their damage multipliers, regions with their flags, nodes).

Usage:
    python3 -I tools/halo2ps2/cmodconv.py assets/decompressed/a10.map -o assets/hp2/a10.cmod.hp2
    python3 -I tools/halo2ps2/cmodconv.py --info assets/decompressed/a10.map     # what the map holds
    python3 -I tools/halo2ps2/cmodconv.py --info assets/hp2/a10.cmod.hp2         # what a pack holds
    python3 -I tools/halo2ps2/cmodconv.py --measure assets/decompressed/a10.map  # fit quality against real geometry

The Xbox `collision_model` tag (engine version 5; offsets inside the tag block; ``[V]`` verified on the 157 distinct
collision models of the ten campaign maps, ``[I]`` inferred from values + public tag documentation, ``[U]`` unused)
-----------------------------------------------------------------------------------------------------------------
Header 0x298 bytes. Reflexives are ``{u32 count, u32 address, u32 pad}`` with absolute tag-space addresses.
    +0x000 u32 flags: bit 0 takes shield damage for children, 1 takes body damage for children, 2 always shields
           friendly damage, 3 passes area damage to children, 4 parent never takes body damage for us, 5 only damaged
           by explosives, 6 only damaged while occupied   [I: names from the public documentation; only the values
           0, 1, 3, 7, 8, 0x20, 0x28, 0x42, 0x48 occur]
    +0x004 s16 indirect damage material (-1 none; 1 or 2 on bipeds)  [V: 124 of 157 are -1, the others 1 or 2]
    +0x008 f32 maximum body vitality (25 crewman, 40 grunt, 50 marine, 60 captain, 75 cyborg, 100 elite, 200 hunter,
           450 wraith ...; 0 on 84 models: scenery, devices, the Warthog)  [V: values match the characters]
    +0x00C f32 body system shock (150 on one model, else 0) [I]   +0x094 f32 body destroyed threshold (banshee 50, ghost 70,
           wraith 200, sentinel 40; 0 elsewhere) [I]
    +0x0CC f32 maximum shield vitality (cyborg 75, marine 50, elite / jackal 100, sentinel 80, monitor 1e10 = never)  [V]
    +0x0D2 u16 shield material type (an index into the globals material table; +0x0D0 u16 is 0) [I]
    +0x10C f32 shield stun time, seconds (cyborg 6, marine 8, elite 12, jackal 15) [I]
    +0x110 f32 shield recharge time, seconds (cyborg 4, marine 1, elite 10)  [V: +0x1C0 = 1 / (30 x this) on all 6 models
           that have one]   +0x1C0 f32 shield recharge rate per 1/30 s tick (1.0 where there is no shield) [V]
    effect references (16 B, destroyed / damaged / depleted effects) at +0x70 +0x84 +0x98 +0xA8 +0xBC and +0x188 +0x198
           +0x1A8 [U: effects are workstream H's business]
    +0x234 materials (72 B)   +0x240 regions (84 B)   +0x24C modifiers (always empty, [U])   +0x268 bounding box
           (zero on 155 of 157 models, [U])   +0x280 pathfinding spheres (32 B, [U])   +0x28C nodes (64 B)
    [V] every reflexive: the element arrays follow each other in tag space with exactly these strides.
material (72): char name[32] ("head", "body", "legs", "metal", "glass" ...); u32 flags at +0x20 (bit 0 = head);
    u16 material type at +0x24 (an index into the globals material table); f32 shield leak percentage +0x28 (0, 0.2 or
    1), f32 shield damage multiplier +0x2C (0 / 1 / 0.8), f32 body damage multiplier +0x3C (0 / 1 / 0.8 / 0.5); the
    rest is zero [V: strides, names; flag bit 0 only on the "head" materials of the biped models; multiplier
    positions [I]: the cyborg's "legs" is the only 0.8]. 0 means "not authored" (every vehicle, device and prop).
region (84): char name[32]; u32 flags at +0x20 (damage region flags: bit 0 lives until object dies, 1 forces object to
    die, 2 dies when object dies, 3 dies when object is damaged, 4 disappears when shield is off, 5 inhibits melee
    attack, 6 inhibits weapon attack, 7 inhibits walking, 8 forces drop weapon, 9 causes head-maimed scream);
    +0x28 f32 damage threshold (0.1..0.5 on flood regions, else 0); +0x38 tag reference (16 B: destroyed effect);
    permutations reflexive at +0x48. [V: stride; the flag values agree with the region names: crewman "head" 0x2, "shield"
    regions 0x10, flood "legs" 0x80, left / right arm 0x20 / 0x140, head 0x200, vehicles 0x4; the damage threshold
    position by its values] permutation (32): char name[32] only ("__base", "~damaged", "~shield_off", "~blur",
    "base00" ...).
node (64): char name[32]; s16 region at +0x20 (-1: the node has no collision geometry), s16 parent +0x22, s16 next
    sibling +0x24, s16 first child +0x26 (-1 none), zero up to +0x34, bsps reflexive at +0x34. [V: the node tree is
    consistent (each of the 705 nodes that has a parent is on its parent's first-child / next-sibling chain, 157 roots);
    a node's bsp count equals the permutation count of its region (0 for region -1) on all 862 nodes]
    Node names, count and order equal those of the render `model` tag on 527 of 529 object / model pairs (the other
    two are one door whose names differ with the same count): node index n is the same node in both. Region names and
    counts agree on 527 of 529 (the tree-pine scenery names its collision region "__unnamed"). Permutations do NOT
    correspond: the collision model has one or two ("__base", "~damaged"), the render model many cosmetic ones.
per-node bsp (0x60 B, one per permutation of the node's region, in permutation order): eight reflexives
    bsp3d nodes (12 B), planes (16), leaves (8), bsp2d references (8), bsp2d nodes (20), surfaces (12), edges (24),
    vertices (16)  [V: strides]. Only surfaces, edges and vertices are read here:
    surface (12): s32 plane, s32 first edge, u8 flags (bit 0 two sided, bit 1 invisible: 26 + 4 of 21678 surfaces), u8
    breakable surface, s16 material (always inside the model's material table [V]); edge (24): s32 start vertex, end
    vertex, forward edge, reverse edge, left surface, right surface; vertex (16): f32 point[3], s32 first edge.
    A surface's polygon is the ring of edges from its first edge: at an edge whose left surface is the surface the
    ring emits the start vertex and continues with `forward`, at an edge whose right surface it is, it emits the end
    vertex and continues with `reverse` [V: every one of 21678 surfaces closes into a ring, every vertex of every
    node bsp is on a ring].
SPACE [V]: the vertices of a node's bsp are in the **node's own frame at the default pose** (bones point along +x:
    the head node of the cyborg spans x -0.03..0.10, y +-0.06, z +-0.05). Evidence: transforming the render model's
    vertices (dominant node) into node-local space with the inverse of the default-pose chain gives boxes within a few
    centimetres of the collision boxes on every node of the cyborg (docs/16). The engine therefore poses a shape with
    the node's world matrix (node-local -> world, `skel_world`), not with the skin matrix.

object tags (scen bipd vehi weap eqip garb mach ctrl lifi): +0x00 u16 object type, +0x04 f32 bounding radius, +0x08 f32[3]
    bounding offset, +0x14 f32[3] origin offset (non-zero on 9 of 389: caskets, the engineer, flood infection forms),
    +0x28 model reference, +0x70 collision model reference (group `coll`), +0x104 f32 render bounding radius (equals
    +0x04 on 383 of 389, larger on big vehicles) [V: +0x70 holds a `coll` tag on 52 vehicles, 140 bipeds, 71 equipment,
    68 weapons, 73 machines, 11 controls, 119 scenery, 26 garbage and 2 light fixtures of the ten maps (counted per
    map); every other object tag has a null reference; no projectile has one]. Three big vehicle-like tags (two scenery,
    one vehicle) also hold a `coll` reference at +0x2B4 / +0x2BC: a different field, ignored.

CMOD output (docs/16-cmod-section.md for the full layout)
    Header 128 B, then 16-aligned tables: models (96 B, sorted by collision tag id), objects (48 B, sorted by object tag
    id), materials (48), regions (48), permutation names (32), nodes (8), shapes (96).
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, modelconv, scenconv, tagindex, texconv  # noqa: E402

MAGIC = b"CMOD"
VERSION = 1
HEADER_SIZE = 128
MODEL_REC, OBJECT_REC, MATERIAL_REC, REGION_REC, PERM_REC, NODE_REC, SHAPE_REC = 96, 48, 48, 48, 32, 8, 96
TABLES = ("models", "objects", "materials", "regions", "perms", "nodes", "shapes")
REC_SIZES = dict(zip(TABLES, (MODEL_REC, OBJECT_REC, MATERIAL_REC, REGION_REC, PERM_REC, NODE_REC, SHAPE_REC)))
NAME_LEN = 32
MAX_MODELS = 0xFFFF
MAX_NODES = 64                    # the engine's skeleton limit (skeleton.h SKEL_MAX_NODES)
MAX_REGIONS = 64
MAX_PERMS_PER_REGION = 64
MAX_MATERIALS = 256
MAX_SHAPES_PER_MODEL = 0xFFFF
MARGIN = 0.002                    # world units added to every half extent (about 6 mm): rounding and ray-math slack
SHAPE_HEAD = 1                    # shape flag: the shape's surface material has the head flag
NO_INDEX = -1

HEADER_FMT = "<4sHHI7I7If2I48x"
MODEL_FMT = "<IIhHHHHHIIIII7fI24x"
OBJECT_FMT = "<IIHBB f3f3ff 4x".replace(" ", "")
MATERIAL_FMT = "<32sHHfff"
REGION_FMT = "<32sIfIHH"
PERM_FMT = "<32s"
NODE_FMT = "<4h"
SHAPE_FMT = "<6HI3ff3ff12f"
for _fmt, _n in ((HEADER_FMT, HEADER_SIZE), (MODEL_FMT, MODEL_REC), (OBJECT_FMT, OBJECT_REC),
                 (MATERIAL_FMT, MATERIAL_REC), (REGION_FMT, REGION_REC), (PERM_FMT, PERM_REC),
                 (NODE_FMT, NODE_REC), (SHAPE_FMT, SHAPE_REC)):
    assert struct.calcsize(_fmt) == _n, (_fmt, struct.calcsize(_fmt), _n)

# ---- Xbox tag layout -------------------------------------------------------------------------------
C_FLAGS, C_INDIRECT, C_BODY, C_SHOCK, C_BODY_DESTROYED = 0x00, 0x04, 0x08, 0x0C, 0x94
C_SHIELD, C_SHIELD_MATERIAL, C_STUN, C_RECHARGE, C_RATE = 0xCC, 0xD2, 0x10C, 0x110, 0x1C0
C_MATERIALS, C_REGIONS, C_NODES = 0x234, 0x240, 0x28C
C_HEADER = 0x298
C_MATERIAL, C_REGION, C_PERM, C_NODE, C_BSP = 72, 84, 32, 64, 0x60
BSP_STRIDES = (12, 16, 8, 8, 20, 12, 24, 16)         # 3d nodes, planes, leaves, 2d refs, 2d nodes, surfaces, edges, vertices
BSP_SURFACES, BSP_EDGES, BSP_VERTICES = 5, 6, 7
OBJECT_GROUPS = ("scen", "bipd", "vehi", "weap", "eqip", "garb", "mach", "ctrl", "lifi", "ssce", "plac", "proj")
O_TYPE, O_RADIUS, O_OFFSET, O_ORIGIN, O_MODEL, O_COLL, O_RENDER = 0x00, 0x04, 0x08, 0x14, 0x28, 0x70, 0x104


class CmodError(ValueError):
    pass


# ---- data model ------------------------------------------------------------------------------------
@dataclass
class Material:
    name: str
    flags: int
    material_type: int
    shield_leak: float
    shield_multiplier: float
    body_multiplier: float


@dataclass
class Region:
    name: str
    flags: int
    damage_threshold: float
    destroyed_effect: int                # tag id, 0 none
    perms: list[str]


@dataclass
class Node:
    name: str
    region: int
    parent: int
    first_child: int
    next_sibling: int


@dataclass
class Shape:
    node: int
    region: int
    perm: int
    material: int
    flags: int
    vertices: int
    center: tuple[float, float, float]
    half: tuple[float, float, float]
    axes: tuple[tuple[float, float, float], ...]       # three unit rows, node-local; u_k = axes[k] . (p - center)


@dataclass
class CollModel:
    tag_id: int
    path: str
    flags: int = 0
    indirect_damage_material: int = -1
    shield_material: int = 0
    max_body_vitality: float = 0.0
    body_system_shock: float = 0.0
    body_destroyed_threshold: float = 0.0
    max_shield_vitality: float = 0.0
    shield_stun_time: float = 0.0
    shield_recharge_time: float = 0.0
    shield_recharge_rate: float = 0.0
    materials: list[Material] = field(default_factory=list)
    regions: list[Region] = field(default_factory=list)
    nodes: list[Node] = field(default_factory=list)
    shapes: list[Shape] = field(default_factory=list)
    source_vertices: int = 0
    empty_bsps: int = 0


@dataclass
class ObjectDef:
    tag_id: int
    path: str
    group: str
    coll_tag: int
    type: int
    radius: float
    offset: tuple[float, float, float]
    origin: tuple[float, float, float]
    render_radius: float
    model_tag: int = 0


@dataclass
class CmodSet:
    models: list[CollModel]
    objects: list[ObjectDef]
    problems: list[str] = field(default_factory=list)


# ---- float helpers -----------------------------------------------------------------------------------
def f32(v: float) -> float:
    return struct.unpack("<f", struct.pack("<f", v))[0]


def f32_up(v: float) -> float:
    """The smallest float32 >= v."""
    r = f32(v)
    if r >= v:
        return r
    bits = struct.unpack("<I", struct.pack("<f", r))[0]
    bits = bits + 1 if r >= 0 else bits - 1
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def _finite(*vals: float) -> bool:
    return all(math.isfinite(v) for v in vals)


# ---- box fitting ---------------------------------------------------------------------------------------
def _jacobi(a: list[list[float]]) -> tuple[list[float], list[list[float]]]:
    """Eigen decomposition of a symmetric 3x3 matrix: (eigenvalues, eigenvectors as columns of v)."""
    v = [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    a = [row[:] for row in a]
    for _ in range(60):
        off = abs(a[0][1]) + abs(a[0][2]) + abs(a[1][2])
        if off < 1e-18:
            break
        for p, q in ((0, 1), (0, 2), (1, 2)):
            if abs(a[p][q]) < 1e-30:
                continue
            theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q])
            t = (1.0 if theta >= 0 else -1.0) / (abs(theta) + math.sqrt(theta * theta + 1.0))
            c = 1.0 / math.sqrt(t * t + 1.0)
            s = t * c
            for k in range(3):
                akp, akq = a[k][p], a[k][q]
                a[k][p], a[k][q] = c * akp - s * akq, s * akp + c * akq
            for k in range(3):
                apk, aqk = a[p][k], a[q][k]
                a[p][k], a[q][k] = c * apk - s * aqk, s * apk + c * aqk
            for k in range(3):
                vkp, vkq = v[k][p], v[k][q]
                v[k][p], v[k][q] = c * vkp - s * vkq, s * vkp + c * vkq
    return [a[0][0], a[1][1], a[2][2]], v


def _project(points, axes):
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    for p in points:
        for k in range(3):
            u = axes[k][0] * p[0] + axes[k][1] * p[1] + axes[k][2] * p[2]
            if u < lo[k]:
                lo[k] = u
            if u > hi[k]:
                hi[k] = u
    return lo, hi


def _box_from_axes(points, axes, margin: float):
    """A box with the given (float32-rounded) axes, containing every point: (center, half, axes, volume)."""
    axes = tuple(tuple(f32(c) for c in row) for row in axes)
    lo, hi = _project(points, axes)
    mid = [(lo[k] + hi[k]) / 2.0 for k in range(3)]
    center = tuple(f32(sum(axes[k][i] * mid[k] for k in range(3))) for i in range(3))
    # re-project against the rounded centre so the half extents hold for the stored numbers
    half = [0.0] * 3
    for p in points:
        d = (p[0] - center[0], p[1] - center[1], p[2] - center[2])
        for k in range(3):
            u = abs(axes[k][0] * d[0] + axes[k][1] * d[1] + axes[k][2] * d[2])
            if u > half[k]:
                half[k] = u
    half_t = tuple(f32_up(h + margin) for h in half)
    return center, half_t, axes, 8.0 * half_t[0] * half_t[1] * half_t[2]


def fit_box(points: list[tuple[float, float, float]], margin: float = MARGIN):
    """Conservative oriented box of a point set, node-local: the smaller of the axis-aligned box and the box on the
    principal axes of the points. Returns (center, half, axes, volume of the axis-aligned box)."""
    ident = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    aabb = _box_from_axes(points, ident, margin)
    best = aabb
    n = len(points)
    if n >= 4:
        mean = [sum(p[k] for p in points) / n for k in range(3)]
        cov = [[sum((p[i] - mean[i]) * (p[j] - mean[j]) for p in points) / n for j in range(3)] for i in range(3)]
        _w, v = _jacobi(cov)
        axes = [tuple(v[i][k] for i in range(3)) for k in range(3)]
        order = sorted(range(3), key=lambda k: -_w[k])
        axes = [axes[k] for k in order]
        cand = _box_from_axes(points, axes, margin)
        if cand[3] < aabb[3] * 0.98:
            best = cand
    return best[0], best[1], best[2], aabb[3]


# ---- reading the Xbox tags --------------------------------------------------------------------------------------
def _name(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin1")[:NAME_LEN - 1]


def read_bsp(r: scenconv.Region, o: int, material_count: int) -> dict[int, list[tuple[float, float, float]]]:
    """The vertices of one node bsp, grouped by the material of the surfaces that use them: {material: [points]}.
    Raises CmodError for anything inconsistent."""
    counts = []
    for i, stride in enumerate(BSP_STRIDES):
        c, a = r.reflexive(o + i * 12, stride, 1 << 16)
        counts.append((c, a))
    nv, ov = counts[BSP_VERTICES]
    ne, oe = counts[BSP_EDGES]
    ns, osf = counts[BSP_SURFACES]
    if ns == 0:
        return {}
    if nv == 0 or ne == 0:
        raise CmodError("bsp has surfaces but no vertices / edges")
    verts = [r.u("<3f", ov + i * 16) for i in range(nv)]
    if not all(_finite(*v) for v in verts):
        raise CmodError("non-finite vertex")
    edges = [r.u("<6i", oe + i * 24) for i in range(ne)]
    for st, en, fw, rv, ls, rs in edges:
        if not (0 <= st < nv and 0 <= en < nv and 0 <= fw < ne and 0 <= rv < ne and -1 <= ls < ns and -1 <= rs < ns):
            raise CmodError("edge index out of range")
    groups: dict[int, set[int]] = {}
    for s in range(ns):
        _plane, first, _flags, _brk, mat = r.u("<iiBBh", osf + s * 12)
        if not 0 <= first < ne:
            raise CmodError("surface first edge out of range")
        if not 0 <= mat < material_count:
            raise CmodError(f"surface material {mat} outside the {material_count} materials")
        ring: list[int] = []
        e = first
        for _ in range(ne + 1):
            st, en, fw, rv, ls, rs = edges[e]
            if ls == s:
                ring.append(st)
                e = fw
            elif rs == s:
                ring.append(en)
                e = rv
            else:
                raise CmodError("surface ring leaves the surface")
            if e == first:
                break
        else:
            raise CmodError("surface ring does not close")
        if len(ring) < 3:
            raise CmodError("surface with fewer than three vertices")
        groups.setdefault(mat, set()).update(ring)
    return {m: [verts[i] for i in sorted(vs)] for m, vs in sorted(groups.items())}


def read_collision_model(r: scenconv.Region, e: tagindex.TagEntry,
                         points_out: dict | None = None) -> CollModel:
    """Parse one `coll` tag. ``points_out``, when given, receives {(node, perm, material): [points]}."""
    try:
        return _read_collision_model(r, e, points_out)
    except scenconv.ScenError as ex:
        raise CmodError(f"{e.path}: {ex}") from None
    except CmodError as ex:
        raise CmodError(f"{e.path}: {ex}") from None


def _read_collision_model(r: scenconv.Region, e: tagindex.TagEntry, points_out: dict | None) -> CollModel:
    b = r.block(e)
    if b < 0 or b + C_HEADER > len(r.data):
        raise CmodError("header outside the tag region")
    m = CollModel(e.tag_id, e.path)
    m.flags = r.u("<I", b + C_FLAGS)[0]
    m.indirect_damage_material = r.u("<h", b + C_INDIRECT)[0]
    m.max_body_vitality = r.u("<f", b + C_BODY)[0]
    m.body_system_shock = r.u("<f", b + C_SHOCK)[0]
    m.body_destroyed_threshold = r.u("<f", b + C_BODY_DESTROYED)[0]
    m.max_shield_vitality = r.u("<f", b + C_SHIELD)[0]
    m.shield_material = r.u("<H", b + C_SHIELD_MATERIAL)[0]
    m.shield_stun_time = r.u("<f", b + C_STUN)[0]
    m.shield_recharge_time = r.u("<f", b + C_RECHARGE)[0]
    m.shield_recharge_rate = r.u("<f", b + C_RATE)[0]
    floats = (m.max_body_vitality, m.body_system_shock, m.body_destroyed_threshold, m.max_shield_vitality,
              m.shield_stun_time, m.shield_recharge_time, m.shield_recharge_rate)
    if not _finite(*floats):
        raise CmodError("non-finite damage constants")
    mc, mo = r.reflexive(b + C_MATERIALS, C_MATERIAL, MAX_MATERIALS)
    for i in range(mc):
        p = mo + i * C_MATERIAL
        flags, mtype = r.u("<IH", p + 0x20)
        leak, shield_mult = r.u("<2f", p + 0x28)
        body_mult = r.u("<f", p + 0x3C)[0]
        if not _finite(leak, shield_mult, body_mult):
            raise CmodError("non-finite material multiplier")
        m.materials.append(Material(_name(r.data[p:p + 32]), flags & 0xFFFF, mtype, leak, shield_mult, body_mult))
    rc, ro = r.reflexive(b + C_REGIONS, C_REGION, MAX_REGIONS)
    for i in range(rc):
        p = ro + i * C_REGION
        flags = r.u("<I", p + 0x20)[0]
        thr = r.u("<f", p + 0x28)[0]
        if not math.isfinite(thr):
            raise CmodError("non-finite region damage threshold")
        _g, eid = r.tagref(p + 0x38)
        pc, po = r.reflexive(p + 0x48, C_PERM, MAX_PERMS_PER_REGION)
        perms = [_name(r.data[po + k * C_PERM:po + k * C_PERM + 32]) for k in range(pc)]
        m.regions.append(Region(_name(r.data[p:p + 32]), flags, thr, 0 if eid == 0xFFFFFFFF else eid, perms))
    nc, no = r.reflexive(b + C_NODES, C_NODE, MAX_NODES)
    for i in range(nc):
        p = no + i * C_NODE
        reg, par, nxt, chi = r.u("<4h", p + 0x20)
        for link in (par, nxt, chi):
            if not -1 <= link < nc:
                raise CmodError(f"node {i} link {link} out of range")
        if not -1 <= reg < rc:
            raise CmodError(f"node {i} region {reg} outside the {rc} regions")
        m.nodes.append(Node(_name(r.data[p:p + 32]), reg, par, chi, nxt))
    for i in range(nc):
        p = no + i * C_NODE
        node = m.nodes[i]
        bc, ba = r.reflexive(p + 0x34, C_BSP, MAX_PERMS_PER_REGION)
        want = len(m.regions[node.region].perms) if node.region >= 0 else 0
        if bc != want:
            raise CmodError(f"node {i}: {bc} bsps, its region has {want} permutations")
        for k in range(bc):
            groups = read_bsp(r, ba + k * C_BSP, len(m.materials))
            if not groups:
                m.empty_bsps += 1
            for mat, pts in groups.items():
                m.source_vertices += len(pts)
                center, half, axes, _vol = fit_box(pts)
                flags = SHAPE_HEAD if m.materials[mat].flags & 1 else 0
                m.shapes.append(Shape(i, node.region, k, mat, flags, len(pts), center, half, axes))
                if points_out is not None:
                    points_out[(i, k, mat)] = pts
    m.shapes.sort(key=lambda s: (s.region, s.perm, s.node, s.material))
    if len(m.shapes) > MAX_SHAPES_PER_MODEL:
        raise CmodError("too many shapes")
    return m


def read_objects(r: scenconv.Region, problems: list[str]) -> list[ObjectDef]:
    out = []
    for e in r.m.index.entries:
        if e.group not in OBJECT_GROUPS or not r.m.index.in_tag_region(e.data_address):
            continue
        b = r.block(e)
        try:
            otype = r.u("<H", b + O_TYPE)[0]
            radius = r.u("<f", b + O_RADIUS)[0]
            offset = r.u("<3f", b + O_OFFSET)
            origin = r.u("<3f", b + O_ORIGIN)
            render = r.u("<f", b + O_RENDER)[0]
            cg, ctid = r.tagref(b + O_COLL)
            mg, mtid = r.tagref(b + O_MODEL)
        except scenconv.ScenError as ex:
            problems.append(f"{e.path}: {ex}")
            continue
        ce = r.entry(ctid) if cg == "coll" else None
        if ce is None or ce.group != "coll":
            continue
        if otype > 0xFF or not _finite(radius, render, *offset, *origin) or radius < 0 or render < 0:
            problems.append(f"{e.path}: bad object bounds")
            continue
        out.append(ObjectDef(e.tag_id, e.path, e.group, ctid, otype, radius, offset, origin, render,
                             mtid if mg == "mode" and r.entry(mtid) is not None else 0))
    out.sort(key=lambda o: o.tag_id)
    return out


def collect(source: Path | texconv.MapData) -> CmodSet:
    """Every collision model of the map and every object tag that references one."""
    mp = source if isinstance(source, texconv.MapData) else texconv.MapData.open(source)
    r = scenconv.Region(mp)
    problems: list[str] = []
    models = []
    for e in mp.index.entries:
        if e.group != "coll":
            continue
        try:
            models.append(read_collision_model(r, e))
        except CmodError as ex:
            problems.append(str(ex))
    models.sort(key=lambda m: m.tag_id)
    have = {m.tag_id for m in models}
    objs = [o for o in read_objects(r, problems) if o.coll_tag in have]
    if len(models) > MAX_MODELS:
        raise CmodError("too many collision models")
    return CmodSet(models, objs, problems)


# ---- writer -----------------------------------------------------------------------------------------------------
def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def _str(s: str, n: int) -> bytes:
    return s.encode("latin1", "replace")[:n - 1].ljust(n, b"\0")


def build(models: list[CollModel], objects: list[ObjectDef]) -> bytes:
    if len(models) > MAX_MODELS or any(a.tag_id >= b.tag_id for a, b in zip(models, models[1:])):
        raise CmodError("models must be sorted by strictly ascending tag id, at most 65535")
    if any(a.tag_id >= b.tag_id for a, b in zip(objects, objects[1:])):
        raise CmodError("objects must be sorted by strictly ascending tag id")
    index = {m.tag_id: i for i, m in enumerate(models)}
    tabs: dict[str, bytearray] = {t: bytearray() for t in TABLES}
    counts = {t: 0 for t in TABLES}
    perm_total = 0
    vertices = empties = 0
    for m in models:
        first = {"materials": counts["materials"], "regions": counts["regions"], "nodes": counts["nodes"],
                 "shapes": counts["shapes"]}
        for mt in m.materials:
            tabs["materials"] += struct.pack(MATERIAL_FMT, _str(mt.name, NAME_LEN), mt.flags, mt.material_type,
                                             mt.shield_leak, mt.shield_multiplier, mt.body_multiplier)
        for rg in m.regions:
            tabs["regions"] += struct.pack(REGION_FMT, _str(rg.name, NAME_LEN), rg.flags, rg.damage_threshold,
                                           rg.destroyed_effect, perm_total, len(rg.perms))
            for pn in rg.perms:
                tabs["perms"] += struct.pack(PERM_FMT, _str(pn, NAME_LEN))
            perm_total += len(rg.perms)
        for nd in m.nodes:
            tabs["nodes"] += struct.pack(NODE_FMT, nd.region, nd.parent, nd.first_child, nd.next_sibling)
        for s in m.shapes:
            tabs["shapes"] += struct.pack(SHAPE_FMT, s.node, s.region, s.perm, s.material, s.flags, 0, s.vertices,
                                          *s.center, 0.0, *s.half, 0.0,
                                          *(c for row in s.axes for c in (*row, 0.0)))
        counts["materials"] += len(m.materials)
        counts["regions"] += len(m.regions)
        counts["nodes"] += len(m.nodes)
        counts["shapes"] += len(m.shapes)
        counts["perms"] = perm_total
        tabs["models"] += struct.pack(MODEL_FMT, m.tag_id, m.flags, m.indirect_damage_material, m.shield_material,
                                      len(m.nodes), len(m.regions), len(m.materials), 0,
                                      first["materials"], first["regions"], first["nodes"], first["shapes"],
                                      len(m.shapes), m.max_body_vitality, m.body_system_shock,
                                      m.body_destroyed_threshold, m.max_shield_vitality, m.shield_stun_time,
                                      m.shield_recharge_time, m.shield_recharge_rate, m.source_vertices)
        vertices += m.source_vertices
        empties += m.empty_bsps
    for o in objects:
        if o.coll_tag not in index:
            raise CmodError("object references a collision model that is not in the section")
        tabs["objects"] += struct.pack(OBJECT_FMT, o.tag_id, o.coll_tag, index[o.coll_tag], o.type, 0, o.radius,
                                       *o.offset, *o.origin, o.render_radius)
    counts["models"], counts["objects"] = len(models), len(objects)
    out = bytearray(HEADER_SIZE)
    offs = {}
    for t in TABLES:
        offs[t] = len(out)
        out += tabs[t]
        _pad16(out)
    struct.pack_into(HEADER_FMT, out, 0, MAGIC, VERSION, 0, len(out),
                     counts["models"], counts["objects"], counts["materials"], counts["regions"], counts["perms"],
                     counts["nodes"], counts["shapes"], *(offs[t] for t in TABLES), MARGIN, vertices, empties)
    return bytes(out)


# ---- reference parser (the C reader is engine/src/world/cmod.c) -------------------------------------------------
@dataclass
class Section:
    size: int
    margin: float
    models: list[dict]
    objects: list[dict]
    materials: list[dict]
    regions: list[dict]
    perms: list[str]
    nodes: list[dict]
    shapes: list[dict]


def parse_section(data: bytes) -> Section:
    """Reference reader; raises CmodError on a bad section (the same checks as ``cmod_open``)."""
    if len(data) < HEADER_SIZE:
        raise CmodError("section shorter than the header")
    h = struct.unpack_from(HEADER_FMT, data)
    magic, version, _r, size = h[:4]
    counts = dict(zip(TABLES, h[4:11]))
    offs = dict(zip(TABLES, h[11:18]))
    margin = h[18]
    if magic != MAGIC:
        raise CmodError("bad signature")
    if version != VERSION:
        raise CmodError(f"unsupported version {version}")
    if size != len(data):
        raise CmodError("size field does not match")
    if counts["models"] > MAX_MODELS or not math.isfinite(margin):
        raise CmodError("inconsistent header")
    for t in TABLES:
        off, cnt, rec = offs[t], counts[t], REC_SIZES[t]
        if off % 16 or off < HEADER_SIZE or off > len(data) or cnt * rec > len(data) - off:
            raise CmodError(f"{t} table outside the section")

    def rows(t: str, fmt: str):
        return [struct.unpack_from(fmt, data, offs[t] + i * REC_SIZES[t]) for i in range(counts[t])]

    def name(raw: bytes) -> str:
        if raw[-1] != 0:
            raise CmodError("name not terminated")
        return raw.split(b"\0", 1)[0].decode("latin1")

    materials = []
    for nm, fl, mt, leak, sm, bm in rows("materials", MATERIAL_FMT):
        if not _finite(leak, sm, bm):
            raise CmodError("non-finite material")
        materials.append(dict(name=name(nm), flags=fl, material_type=mt, shield_leak=leak, shield_multiplier=sm,
                              body_multiplier=bm))
    regions = []
    for nm, fl, thr, eff, fp, pc in rows("regions", REGION_FMT):
        if not math.isfinite(thr) or fp + pc > counts["perms"]:
            raise CmodError("bad region")
        regions.append(dict(name=name(nm), flags=fl, damage_threshold=thr, destroyed_effect=eff, first_perm=fp,
                            perm_count=pc))
    perms = [name(p[0]) for p in rows("perms", PERM_FMT)]
    nodes = [dict(region=a, parent=b, first_child=c, next_sibling=d) for a, b, c, d in rows("nodes", NODE_FMT)]
    shapes = []
    for v in rows("shapes", SHAPE_FMT):
        node, region, perm, mat, flags, _p, nv = v[:7]
        fl = v[7:]
        center, half = fl[0:3], fl[4:7]
        axes = tuple(tuple(fl[8 + 4 * k:11 + 4 * k]) for k in range(3))
        if not _finite(*fl) or any(x < 0 for x in half):
            raise CmodError("bad shape box")
        for k in range(3):
            if abs(sum(c * c for c in axes[k]) - 1.0) > 0.01:
                raise CmodError("shape axis not unit length")
        shapes.append(dict(node=node, region=region, perm=perm, material=mat, flags=flags, vertices=nv,
                           center=center, half=half, axes=axes))
    models, nxt = [], dict(materials=0, regions=0, nodes=0, shapes=0, perms=0)
    prev = -1
    for v in rows("models", MODEL_FMT):
        (tag, flags, indirect, shield_mat, ncount, rcount, mcount, _p, f_mat, f_reg, f_node, f_shape, scount) = v[:13]
        consts = v[13:20]
        verts = v[20]
        if tag <= prev and prev >= 0:
            raise CmodError("models not strictly ascending")
        prev = tag
        if (f_mat, f_reg, f_node, f_shape) != (nxt["materials"], nxt["regions"], nxt["nodes"], nxt["shapes"]):
            raise CmodError("model ranges do not partition the tables")
        if ncount > MAX_NODES or rcount > MAX_REGIONS or mcount > MAX_MATERIALS:
            raise CmodError("model counts out of range")
        if f_mat + mcount > counts["materials"] or f_reg + rcount > counts["regions"] or \
                f_node + ncount > counts["nodes"] or f_shape + scount > counts["shapes"]:
            raise CmodError("model range outside its table")
        if not _finite(*consts):
            raise CmodError("non-finite damage constants")
        for ni in range(ncount):
            nd = nodes[f_node + ni]
            if not (-1 <= nd["region"] < rcount and all(-1 <= nd[k] < ncount for k in ("parent", "first_child", "next_sibling"))):
                raise CmodError("node link out of range")
        for ri in range(rcount):
            rg = regions[f_reg + ri]
            if rg["first_perm"] != nxt["perms"] or rg["perm_count"] > MAX_PERMS_PER_REGION:
                raise CmodError("region permutations do not partition the permutation table")
            nxt["perms"] += rg["perm_count"]
        for si in range(scount):
            s = shapes[f_shape + si]
            if s["node"] >= ncount or s["region"] >= rcount or s["material"] >= mcount:
                raise CmodError("shape index out of range")
            if nodes[f_node + s["node"]]["region"] != s["region"]:
                raise CmodError("shape region does not match its node")
            if s["perm"] >= regions[f_reg + s["region"]]["perm_count"]:
                raise CmodError("shape permutation out of range")
        nxt["materials"] += mcount
        nxt["regions"] += rcount
        nxt["nodes"] += ncount
        nxt["shapes"] += scount
        models.append(dict(tag_id=tag, flags=flags, indirect_damage_material=indirect, shield_material=shield_mat,
                           node_count=ncount, region_count=rcount, material_count=mcount, first_material=f_mat,
                           first_region=f_reg, first_node=f_node, first_shape=f_shape, shape_count=scount,
                           max_body_vitality=consts[0], body_system_shock=consts[1],
                           body_destroyed_threshold=consts[2], max_shield_vitality=consts[3],
                           shield_stun_time=consts[4], shield_recharge_time=consts[5],
                           shield_recharge_rate=consts[6], source_vertices=verts))
    if (nxt["materials"], nxt["regions"], nxt["nodes"], nxt["shapes"], nxt["perms"]) != (
            counts["materials"], counts["regions"], counts["nodes"], counts["shapes"], counts["perms"]):
        raise CmodError("tables are not covered by the models")
    objects, prev = [], -1
    for tag, ctag, mi, typ, _p, radius, *rest in rows("objects", OBJECT_FMT):
        if tag <= prev and prev >= 0:
            raise CmodError("objects not strictly ascending")
        prev = tag
        if mi >= len(models) or models[mi]["tag_id"] != ctag or not _finite(radius, *rest) or radius < 0:
            raise CmodError("bad object")
        objects.append(dict(tag_id=tag, coll_tag=ctag, model_index=mi, type=typ, radius=radius, offset=tuple(rest[0:3]),
                            origin=tuple(rest[3:6]), render_radius=rest[6]))
    return Section(size, margin, models, objects, materials, regions, perms, nodes, shapes)


def build_section(map_path: Path) -> bytes:
    cs = collect(map_path)
    return build(cs.models, cs.objects)


# ---- measuring the fit on real data ---------------------------------------------------------------------------------
def default_pose_worlds(r: scenconv.Region, model_entry: tagindex.TagEntry):
    """(rotation 3x3, translation) of every node of a render model at its default pose (column convention:
    p_parent = R p + t)."""
    off = r.block(model_entry)
    nc, no = r.reflexive(off + modelconv.T_NODES, modelconv.T_NODE, modelconv.MAX_NODES)
    nodes = []
    for i in range(nc):
        o = no + i * modelconv.T_NODE
        _sib, _child, parent = r.u("<3h", o + 0x20)
        t = r.u("<3f", o + 0x28)
        q = modelconv.conj(r.u("<4f", o + 0x34))
        nodes.append((parent, t, q))
    world: list = [None] * nc

    def mat(q):
        x, y, z, w = q
        return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]

    def get(i: int, depth: int = 0):
        if world[i] is not None:
            return world[i]
        if depth > nc:
            raise CmodError("render model node cycle")
        parent, t, q = nodes[i]
        rl = mat(q)
        if parent < 0:
            world[i] = (rl, list(t))
        else:
            rp, tp = get(parent, depth + 1)
            rw = [[sum(rp[a][k] * rl[k][c] for k in range(3)) for c in range(3)] for a in range(3)]
            tw = [sum(rp[a][k] * t[k] for k in range(3)) + tp[a] for a in range(3)]
            world[i] = (rw, tw)
        return world[i]

    for i in range(nc):
        get(i)
    return world


def _box_distance(shape: dict, p) -> float:
    """Distance from a point to the box (0 inside)."""
    d = [p[k] - shape["center"][k] for k in range(3)]
    s = 0.0
    for k in range(3):
        a = shape["axes"][k]
        out = abs(a[0] * d[0] + a[1] * d[1] + a[2] * d[2]) - shape["half"][k]
        if out > 0:
            s += out * out
    return math.sqrt(s)


def _inside(shape: dict, p) -> bool:
    d = [p[k] - shape["center"][k] for k in range(3)]
    for k in range(3):
        a = shape["axes"][k]
        if abs(a[0] * d[0] + a[1] * d[1] + a[2] * d[2]) > shape["half"][k]:
            return False
    return True


def _percentile(sorted_vals: list[float], q: float) -> float:
    return sorted_vals[min(len(sorted_vals) - 1, int(q * len(sorted_vals)))] if sorted_vals else 0.0


def measure(map_path: Path, render: bool = True) -> dict:
    """Fit quality on the real map, measured against the *section* (so after float32 rounding):
      * source collision vertices inside their (node, perm, material) shape (the conservativeness guarantee);
      * box volume against the axis-aligned box;
      * the object bounding sphere (radius around origin + offset) against the collision vertices at the default pose;
      * render-model vertices (dominant node, first permutation of every region, best LOD, node-local default pose):
        inside a shape of their own node / inside any shape of the model (permutation 0) / the node has no shape, and
        how far outside the rest are."""
    mp = texconv.MapData.open(map_path)
    r = scenconv.Region(mp)
    cs = collect(mp)
    sec = parse_section(build(cs.models, cs.objects))
    model_by_tag = {m["tag_id"]: i for i, m in enumerate(sec.models)}
    res = dict(models=len(cs.models), shapes=len(sec.shapes), coll_vertices=0, coll_inside=0, render_vertices=0,
               render_inside_node=0, render_inside_any=0, render_no_shape=0, render_models=0, sphere_objects=0,
               sphere_contained=0, sphere_vertices=0, sphere_vertices_in=0, obb_chosen=0, volume_ratio=[],
               outside_dist=[], problems=list(cs.problems), render_by_model=[], sphere_by_object=[])
    cpts: dict[int, dict] = {}
    for m in cs.models:
        pts: dict = {}
        read_collision_model(r, mp.by_id[m.tag_id], pts)
        cpts[m.tag_id] = pts
        sm = sec.models[model_by_tag[m.tag_id]]
        by_key = {(s["node"], s["perm"], s["material"]): s
                  for s in sec.shapes[sm["first_shape"]:sm["first_shape"] + sm["shape_count"]]}
        for key, plist in pts.items():
            res["coll_vertices"] += len(plist)
            res["coll_inside"] += sum(1 for p in plist if _inside(by_key[key], p))
        for s in m.shapes:
            if s.axes != ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)):
                res["obb_chosen"] += 1
            vol_aabb = fit_box(pts[(s.node, s.perm, s.material)])[3]
            res["volume_ratio"].append(8.0 * s.half[0] * s.half[1] * s.half[2] / vol_aabb if vol_aabb > 0 else 1.0)
    for o in cs.objects:
        if not o.model_tag:
            continue
        sm = sec.models[model_by_tag[o.coll_tag]]
        worlds = default_pose_worlds(r, mp.by_id[o.model_tag])
        if len(worlds) != sm["node_count"]:
            res["problems"].append(f"{o.path}: model has {len(worlds)} nodes, collision model {sm['node_count']}")
            continue
        n_all = n_in = 0
        for (node, perm, _mat), plist in cpts[o.coll_tag].items():
            if perm != 0:
                continue
            rw, tw = worlds[node]
            for p in plist:
                w = [sum(rw[a][k] * p[k] for k in range(3)) + tw[a] for a in range(3)]
                n_all += 1
                n_in += math.dist(w, o.offset) <= o.radius + 1e-4
        if n_all:
            res["sphere_objects"] += 1
            res["sphere_contained"] += n_in == n_all
            res["sphere_vertices"] += n_all
            res["sphere_vertices_in"] += n_in
            res["sphere_by_object"].append((o.path, n_in, n_all))
    if not render:
        return res
    pairs: dict[tuple[int, int], ObjectDef] = {}
    for o in cs.objects:
        if o.model_tag:
            pairs.setdefault((o.model_tag, o.coll_tag), o)
    for (mtag, ctag), o in sorted(pairs.items()):
        sm = sec.models[model_by_tag[ctag]]
        try:
            md = modelconv.read_model(mp, mp.by_id[mtag], keep_variants=set())
        except (modelconv.ConvertError, texconv.TexError, scenconv.ScenError, struct.error) as ex:
            res["problems"].append(f"{o.path}: render model unreadable: {ex}")
            continue
        if len(md.nodes) != sm["node_count"]:
            continue
        worlds = default_pose_worlds(r, mp.by_id[mtag])
        shapes = [s for s in sec.shapes[sm["first_shape"]:sm["first_shape"] + sm["shape_count"]] if s["perm"] == 0]
        by_node: dict[int, list] = {}
        for s in shapes:
            by_node.setdefault(s["node"], []).append(s)
        tot = ins_node = ins_any = nos = 0
        dists: list[float] = []
        seen_geoms = set()
        for pm in md.perms:
            g = pm.geometry[0]
            if g < 0 or g in seen_geoms or g not in md.geometries:
                continue
            seen_geoms.add(g)
            for part in md.geometries[g]:
                for p, n0, n1, w in zip(part.pos, part.node0, part.node1, part.weight0):
                    if n1 >= 0 and w < 16384:
                        n0 = n1
                    tot += 1
                    cand = by_node.get(n0)
                    rw, tw = worlds[n0]
                    d = [p[k] - tw[k] for k in range(3)]
                    loc = [sum(rw[a][i] * d[a] for a in range(3)) for i in range(3)]
                    if not cand:
                        nos += 1
                    elif any(_inside(s, loc) for s in cand):
                        ins_node += 1
                        ins_any += 1
                        continue
                    # inside or near any shape of the model: each shape in its own node frame
                    best = math.inf
                    for s in shapes:
                        rs, ts = worlds[s["node"]]
                        ds = [p[k] - ts[k] for k in range(3)]
                        ls = [sum(rs[a][i] * ds[a] for a in range(3)) for i in range(3)]
                        best = min(best, _box_distance(s, ls))
                        if best == 0.0:
                            break
                    if best == 0.0:
                        ins_any += 1
                    else:
                        dists.append(best)
        res["render_vertices"] += tot
        res["render_inside_node"] += ins_node
        res["render_inside_any"] += ins_any
        res["render_no_shape"] += nos
        res["render_models"] += 1
        res["outside_dist"] += dists
        res["render_by_model"].append((o.path, tot, ins_node, ins_any, nos, max(dists, default=0.0)))
    return res


# ---- reporting --------------------------------------------------------------------------------------------------------
def describe(cs: CmodSet, mp: texconv.MapData | None = None) -> str:
    L = [f"{len(cs.models)} collision models, {len(cs.objects)} object tags referencing one, "
         f"{sum(len(m.shapes) for m in cs.models)} shapes, {sum(m.source_vertices for m in cs.models)} source vertices, "
         f"{sum(m.empty_bsps for m in cs.models)} empty node bsps"]
    by_group: dict[str, int] = {}
    for o in cs.objects:
        by_group[o.group] = by_group.get(o.group, 0) + 1
    L.append("  objects per group: " + ", ".join(f"{g} {n}" for g, n in sorted(by_group.items())))
    for m in cs.models:
        extra = []
        if m.max_body_vitality:
            extra.append(f"body {m.max_body_vitality:g}")
        if m.max_shield_vitality:
            extra.append(f"shield {m.max_shield_vitality:g} (recharge {m.shield_recharge_time:g} s)")
        L.append(f"  {m.path}: {len(m.nodes)} nodes, {len(m.regions)} regions, {len(m.materials)} materials, "
                 f"{len(m.shapes)} shapes" + (", " + ", ".join(extra) if extra else ""))
    for p in cs.problems[:10]:
        L.append(f"  problem: {p}")
    return "\n".join(L)


def info_pack(path: Path) -> str:
    sec = parse_section(hp2.parse(path.read_bytes()).read("CMOD"))
    return (f"{path.name}: CMOD {sec.size} bytes, {len(sec.models)} models, {len(sec.objects)} objects, "
            f"{len(sec.shapes)} shapes, {len(sec.materials)} materials, {len(sec.regions)} regions, "
            f"{len(sec.perms)} permutations, {len(sec.nodes)} nodes")


def format_measure(res: dict) -> str:
    pct = lambda a, b: f"{100.0 * a / b:.3f} %" if b else "n/a"  # noqa: E731
    vr = sorted(res["volume_ratio"])
    L = [f"{res['models']} collision models, {res['shapes']} shapes ({res['obb_chosen']} oriented off the node axes)",
         f"collision vertices inside their shape: {res['coll_inside']} of {res['coll_vertices']} "
         f"({pct(res['coll_inside'], res['coll_vertices'])})"]
    if vr:
        L.append(f"box volume / axis-aligned box volume: median {vr[len(vr) // 2]:.3f}, min {vr[0]:.3f}, "
                 f"mean {sum(vr) / len(vr):.3f}")
    L.append(f"object bounding sphere around the collision vertices (default pose): contains all of them for "
             f"{res['sphere_contained']} of {res['sphere_objects']} objects; vertices inside "
             f"{res['sphere_vertices_in']} of {res['sphere_vertices']} ({pct(res['sphere_vertices_in'], res['sphere_vertices'])})")
    if res.get("render_models"):
        tot = res["render_vertices"]
        d = sorted(res["outside_dist"])
        L.append(f"render vertices ({res['render_models']} model / collision pairs, dominant node, first permutation of each region):")
        L.append(f"  {tot} vertices: inside a shape of their own node {res['render_inside_node']} ({pct(res['render_inside_node'], tot)}), "
                 f"inside some shape of the model {res['render_inside_any']} ({pct(res['render_inside_any'], tot)}), "
                 f"their node has no shape {res['render_no_shape']} ({pct(res['render_no_shape'], tot)})")
        L.append(f"  outside every shape: {len(d)} ({pct(len(d), tot)}); distance to the nearest box (wu): "
                 f"median {_percentile(d, 0.5):.4f}, p95 {_percentile(d, 0.95):.4f}, max {d[-1] if d else 0.0:.4f}")
    for p in res["problems"][:10]:
        L.append(f"problem: {p}")
    return "\n".join(L)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="print what the map / pack holds instead of writing")
    ap.add_argument("--measure", action="store_true", help="fit quality against the real geometry (slow: reads render models)")
    ap.add_argument("--no-render", action="store_true", help="with --measure: skip the render-model vertex comparison")
    args = ap.parse_args(argv)
    if args.info and args.input.suffix == ".hp2":
        print(info_pack(args.input))
        return 0
    if args.measure:
        t = time.time()
        print(format_measure(measure(args.input, not args.no_render)))
        print(f"({time.time() - t:.1f} s)")
        return 0
    if args.info:
        print(describe(collect(args.input)))
        return 0
    if not args.out:
        ap.error("-o is required")
    t = time.time()
    cs = collect(args.input)
    blob = build(cs.models, cs.objects)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, [("CMOD", blob)]))
    print(f"wrote {args.out} (CMOD {len(blob) / 1024:.1f} KiB, {len(cs.models)} models, {len(cs.objects)} objects, "
          f"{sum(len(m.shapes) for m in cs.models)} shapes, {time.time() - t:.1f} s)")
    for p in cs.problems:
        print(f"  problem: {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
