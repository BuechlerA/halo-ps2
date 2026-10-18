#!/usr/bin/env python3
"""Convert Xbox `model` (group `mode`, engine version 5) tags into the PS2 `MODL` section plus the
`OTEX` object-texture section (docs/11-model-section.md, contract engine/src/world/model.h).

Usage:
    python3 -I tools/halo2ps2/modelconv.py assets/decompressed/a10.map -o assets/hp2/a10.model.hp2 [--tags 0xE6DE056A,...]
    python3 -I tools/halo2ps2/modelconv.py --info assets/hp2/a10.model.hp2      # sections of a pack
    python3 -I tools/halo2ps2/modelconv.py --info assets/decompressed/a10.map   # models of a map (with tag paths)

The Xbox `model` tag (all offsets inside the tag's data block; `[V]` verified on the retail maps, `[U]` not)
-------------------------------------------------------------------------------------------------------
Header, 0xE8 bytes
    +0x00 u32 flags (bit0 "blend shared normals"; 0 on all but 1-2 models per map) [U: not used]
    +0x04 s32 node list checksum, +0x08 f32[5] LOD cutoffs, +0x1C u16[5] node counts per LOD (equal on every
          model seen, [U]: not used), +0x30 f32 base map u scale, +0x34 f32 v scale  [V]
    reflexives {u32 count, u32 address, u32 unused}: +0xAC markers, +0xB8 nodes, +0xC4 regions,
          +0xD0 geometries, +0xDC shaders; the element arrays follow the header in tag space   [V]
node (0x9C) [V]: name[32]; s16 next sibling, first child, parent (-1 none), pad; f32 translation[3] at +0x28,
    f32 rotation (x y z w) at +0x34, f32 distance from parent at +0x44 (= |translation|, [V]), then the bind
    pose inverse: f32 scale at +0x68 (1.0 on every node), 3x3 matrix at +0x6C, translation at +0x90.
    QUATERNION CONVENTION [V]: the stored rotation is the *conjugate* of the usual Hamilton quaternion. The
    local-to-parent rotation is the standard quaternion matrix of (-x, -y, -z, w); verified on all 76 a10 models
    (tests/test_modelconv.py): the chain of default poses inverted reproduces the stored inverse matrices and
    translations to 4e-6 (a w-first or non-conjugated reading is off by O(1)). This converter writes the conjugate
    (-x, -y, -z, w), so `core/mat.c`'s `mat4_from_trs` (standard formula) applies directly.
    Vertices are in model space in the bind pose; the skin matrix of a node is
    world(node, pose) * inverse(world(node, default pose)).
marker group (0x40): name[32], pad; instances reflexive at +0x34. instance (0x20): u8 region, u8 permutation (index
    within the region), u8 node, pad, f32 translation[3], f32 rotation (x y z w; same convention as nodes) [V]
region (0x4C): name[32], pad, permutations reflexive at +0x40. permutation (0x58): name[32], u32 flags at +0x20
    (bit0 "cannot be chosen randomly": the "~blur" permutations), pad, s16[5] geometry index per LOD at +0x40, pad,
    permutation markers reflexive at +0x4C (empty on every model seen, ignored) [V]
LOD order [V]: the permutation's five geometry indices and the five cutoffs are stored SUPER LOW first, SUPER HIGH
    last. Evidence: on every model of a10 a30 b40 c10 (314) the cutoffs rise monotonically in that order, and the
    geometry listed first by a permutation is the one with the fewest triangles on every character and vehicle (the
    last listed has several times as many). 3 of 121 a10 permutations (debris, a shield generator variant, one head) are
    authored the other way round: their super high geometry is smaller than the lower ones.
    Engine LOD 0 = super high ... 4 = super low (model.h), i.e. the stored arrays are reversed on output.
LOD cutoffs [semantics: order V, unit U]: monotonic non-decreasing from super low to super high (0..400 over the models
    seen; weapons end at tens, characters and vehicles at 250..400, several models have 0 for the three lowest), so a LOD is
    *used when the quantity is >= its cutoff*, and the quantity grows when the object is closer: it is a projected size
    in pixels (HEK: "pixels"). Whether it is the sphere's diameter, radius or the screen height share, and at which
    frame width, is not recoverable from the data: `model_pick_lod` takes the bounding sphere's diameter in pixels of
    a 640-wide frame (the contract of model.h). [U]
geometry (0x30): u32 flags, pad, parts reflexive at +0x24. part (0x68): [V unless noted]
    +0x04 s16 shader index (into the shaders array), +0x06 s8 previous / next filthy part (unused),
    +0x08 u16[2] centroid node indices, +0x0C f32[2] their weights, +0x14 f32[3] centroid [U: unused here]
    +0x44 u32 = 1, +0x54 u32 = 5 (the DX8 TRIANGLESTRIP primitive type) [U: meaning inferred, constant]
    +0x48 u32 triangle count of the strip (the strip has count + 2 indices, degenerate triangles included)
    +0x4C u32 pointer to the index data (u16 indices inside the index buffer reached through the index header)
    +0x50 u32 pointer to a 12-byte D3D index buffer record (same value as +0x4C at +4 of it; not used)
    +0x58 u32 vertex count, +0x60 u32 Xbox physical address of the vertex data (not used),
    +0x64 u32 pointer to a 12-byte D3D vertex buffer record whose second dword is the pointer to the vertex data
shader reference (0x20): tag reference (group, path pointer, path length, tag id) + u16 permutation (non-zero on
    ~7 shaders per map, ignored).
Vertex and index buffers [V on all 623 / 789 / 722 / 817 parts of a10 / a30 / b40 / c10]: the index header's vertex region
    (`model_vertex_address` .. `model_index_address`) starts with one 12-byte record per part (reverse of tag order)
    followed by the vertex data; the index region starts with the same kind of records then the index data. Data of the
    parts is stored in reverse tag order, 32 bytes per vertex (the sum of vertex counts x 32 plus 12 bytes per part
    plus 16 reproduces the region size exactly), indices u16 with 4-byte padding between parts.
Vertex, 32 bytes [V]: f32 position[3]; u32 normal (11:11:10 signed bits, x in the low 11 bits, y next, z the top 10;
    divide by 1023 / 1023 / 511: unit length to 0.3 %); u32 binormal and u32 tangent (zero on every vertex seen, ignored);
    s16 u, v (texture coordinate = s16 / 32767 * model base map u/v scale); u8 node 0 * 3, u8 node 1 * 3 (0xFD = -3 = none);
    s16 weight of node 0 (32767 = 1.0; node 1 gets the remainder). Node numbers are model node indices (every one < the node
    count on all maps). Rigid vertices have node 1 = none and weight 32767; two-node vertices keep weights 2978..32767.
Strips [V]: a D3D triangle strip with degenerate triangles (repeated indices) as stitches, no 0xFFFF restarts anywhere,
    every index < the part's vertex count. Triangle i is (i[k], i[k+1], i[k+2]), odd k swapped; winding then equals the BSP's
    (front faces are clockwise about the outward normal: (b - a) x (c - a) points against the vertex normal on 99.5 % of
    triangles of cyborg part 2 and on 100 % of 16741 a10 BSP triangles). Output keeps that winding.
Shaders used by parts: soso (the bulk), sotr, schi, senv, sgla, smet, spla, swat; see objtex.py. A part is skinned when its
    vertices use more than one node or weights below 1, otherwise rigid (`rigid_node`).

PS2 output (docs/11-model-section.md for the full layout)
    MODL: models (sorted by tag id), nodes, markers, permutation records (`ranges`), parts, batches, VIF packets.
    Geometry is re-batched per part into <= 24 triangles / <= 24 distinct nodes; strips become triangle lists
    (degenerate triangles dropped); every batch is one VIF packet laid out as in model.h:
      qw0 f32 centre.xyz, scale: decoded position = centre + s16 * scale (scale = largest half extent / 32767)
      qw1 s32 n = 3 * triangles
      n x s16 x y z, palette slot of node 0
      n x s16 s t = uv * 2^uv_shift (uv_shift per part: largest power of two that fits), slot of node 1, weight of node 0 (0..32767)
      n x s8 normal x y z (127 = 1.0), 0
    Two influences per vertex (the strongest first); rigid vertices use slot 1 = slot 0 and weight 32767.
    OTEX: `TEXS`-layout blob (objtex.py) bound by Xbox shader tag id.
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
from halo2ps2 import bspconv, hp2, objtex, texconv  # noqa: E402

MAGIC = b"MODL"
VERSION = 1
HEADER_SIZE = 96
MODEL_REC, NODE_REC, MARKER_REC, PERM_REC, PART_REC, BATCH_REC = 80, 72, 68, 64, 32, 40
TABLES = ("models", "nodes", "markers", "perms", "parts", "batches", "vif")
DIR_AT = 0x20
REC_SIZES = {"models": MODEL_REC, "nodes": NODE_REC, "markers": MARKER_REC, "perms": PERM_REC,
             "parts": PART_REC, "batches": BATCH_REC, "vif": 1}
MODEL_LODS = 5
MAX_PALETTE = 24
MAX_BATCH_TRIS = 24
MAX_NODES = 64
MAX_REGIONS = 64                  # the C reader refuses more (callers may size selection arrays by this)
NAME_LEN = 32
NO_NODE = -1
ANY = 0xFFFF                      # marker region / permutation that applies to every one
NO_SHADER = 0xFFFFFFFF
PF_ALPHA_TEST, PF_BLEND = 1, 2    # part flags
PERM_NO_RANDOM = 1                # permutation flag: cannot be chosen randomly
VIF_UNPACK_V4_32, VIF_UNPACK_V4_16, VIF_UNPACK_V4_8 = bspconv.VIF_UNPACK_V4_32, bspconv.VIF_UNPACK_V4_16, bspconv.VIF_UNPACK_V4_8
TOPS_RELATIVE = bspconv.TOPS_RELATIVE
VU_HDR, VU_POS = 0, 4
VU_UV = VU_POS + MAX_BATCH_TRIS * 3
VU_NRM = VU_UV + MAX_BATCH_TRIS * 3
WEIGHT_ONE = 32767
MAX_UV_SHIFT = 14
RESIDENT_BUDGET = 9 * 2**20       # docs/02-specs.md: resident mission data (objects, weapons, bipeds, AI, scripts, animations, UI)

# ---- Xbox tag layout ---------------------------------------------------------------------------------
T_CUTOFFS, T_UV_SCALE = 0x08, 0x30
T_MARKERS, T_NODES, T_REGIONS, T_GEOMS, T_SHADERS = 0xAC, 0xB8, 0xC4, 0xD0, 0xDC
T_HEADER = 0xE8
T_NODE, T_MARKER_GROUP, T_MARKER, T_REGION, T_PERM, T_GEOM, T_PART, T_SHADER = 0x9C, 0x40, 0x20, 0x4C, 0x58, 0x30, 0x68, 0x20
VERTEX = struct.Struct("<3fIII2hBBh")
assert VERTEX.size == 32
REFLEXIVE_LIMIT = 1 << 16

MODEL_FMT, NODE_FMT, MARKER_FMT = "<I2H2H3I3ff5fI2I8x", "<32s4h3f4ff", "<32s2H2h3f4f"
PERM_FMT, PART_FMT, BATCH_FMT = "<32s2H10H8x", "<I6H3ff", "<2HB3xII24s"
PARSE_BATCH_FMT = "<2HB3sII24s"
for _fmt, _n in ((MODEL_FMT, MODEL_REC), (NODE_FMT, NODE_REC), (MARKER_FMT, MARKER_REC), (PERM_FMT, PERM_REC),
                 (PART_FMT, PART_REC), (BATCH_FMT, BATCH_REC)):
    assert struct.calcsize(_fmt) == _n, (_fmt, struct.calcsize(_fmt), _n)


class ConvertError(ValueError):
    pass


# ---- data model ---------------------------------------------------------------------------------------
@dataclass
class Node:
    name: str
    parent: int = NO_NODE
    first_child: int = NO_NODE
    next_sibling: int = NO_NODE
    translation: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)     # engine (Hamilton) x y z w
    scale: float = 1.0


@dataclass
class Marker:
    name: str
    region: int = ANY
    permutation: int = ANY             # flat permutation index or ANY
    node: int = NO_NODE
    translation: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)


@dataclass
class SrcPart:
    """One decoded Xbox part: vertices and triangles (already a list, correct winding)."""
    shader_id: int
    pos: list[tuple[float, float, float]]
    nrm: list[tuple[float, float, float]]
    uv: list[tuple[float, float]]
    node0: list[int]
    node1: list[int]                   # -1 = none
    weight0: list[int]                 # 0..32767
    tris: list[tuple[int, int, int]]


@dataclass
class Perm:
    name: str
    region: int
    flags: int = 0
    geometry: tuple[int, ...] = (0,) * MODEL_LODS   # geometry index per ENGINE LOD (0 = super high), -1 none


@dataclass
class ModelData:
    tag_id: int
    path: str
    nodes: list[Node]
    markers: list[Marker]
    region_count: int
    perms: list[Perm]
    lod_cutoff: tuple[float, ...]      # per engine LOD (0 = super high)
    geometries: dict[int, list[SrcPart]]     # only the geometries some permutation uses


# ---- reading the Xbox tag ------------------------------------------------------------------------------
def _name(raw: bytes) -> str:
    return raw.split(b"\0")[0].decode("latin1")[:NAME_LEN - 1]


class _Reader:
    def __init__(self, m: texconv.MapData):
        self.m = m
        self.r = m.region
        self.base = m.base
        h = m.index.header
        self.vtx_lo = h.model_vertex_address - m.base
        self.idx_lo = h.model_index_address - m.base
        ia, isz = m.index.index_region
        self.idx_hi = ia - m.base + isz

    def refl(self, off: int, stride: int, what: str) -> tuple[int, int]:
        if off < 0 or off + 12 > len(self.r):
            raise ConvertError(f"{what}: reflexive header outside the tag region")
        count, addr = struct.unpack_from("<II", self.r, off)
        if count == 0:
            return 0, 0
        o = addr - self.base
        if count > REFLEXIVE_LIMIT or o < 0 or o + count * stride > len(self.r):
            raise ConvertError(f"{what}: {count} elements at {addr:#x} outside the tag region")
        return count, o

    def part(self, o: int, node_count: int, uvs: tuple[float, float], shaders: list[int]) -> SrcPart:
        r = self.r
        shader = struct.unpack_from("<h", r, o + 0x04)[0]
        tri_count, idx_ptr = struct.unpack_from("<II", r, o + 0x48)
        vcount, = struct.unpack_from("<I", r, o + 0x58)
        vrec, = struct.unpack_from("<I", r, o + 0x64)
        ro = vrec - self.base
        if ro < self.vtx_lo or ro + 12 > self.idx_lo:
            raise ConvertError("part: vertex buffer record outside the vertex region")
        vptr = struct.unpack_from("<I", r, ro + 4)[0] - self.base
        if vcount == 0 or vcount > 0xFFFF or vptr < self.vtx_lo or vptr + vcount * VERTEX.size > self.idx_lo:
            raise ConvertError(f"part: {vcount} vertices at {vptr:#x} outside the vertex region")
        if tri_count > 0xFFFF:
            raise ConvertError("part: strip too long")
        ip = idx_ptr - self.base
        n_idx = tri_count + 2 if tri_count else 0
        if ip < self.idx_lo or ip + n_idx * 2 > self.idx_hi:
            raise ConvertError("part: index data outside the index region")
        idx = struct.unpack_from(f"<{n_idx}H", r, ip) if n_idx else ()
        if idx and max(idx) >= vcount:
            raise ConvertError("part: strip index beyond the vertex count")
        pos, nrm, uv, n0s, n1s, w0s = [], [], [], [], [], []
        su, sv = uvs
        for x, y, z, nr, _b, _t, u, v, n0, n1, w in VERTEX.iter_unpack(r[vptr:vptr + vcount * VERTEX.size]):
            pos.append((x, y, z))
            nx = ((nr & 0x7FF) ^ 0x400) - 0x400
            ny = (((nr >> 11) & 0x7FF) ^ 0x400) - 0x400
            nz = ((nr >> 22) ^ 0x200) - 0x200
            nrm.append((nx / 1023.0, ny / 1023.0, nz / 511.0))
            uv.append((u / 32767.0 * su, v / 32767.0 * sv))
            if n0 % 3 or n0 // 3 >= node_count:
                raise ConvertError(f"part: vertex node {n0} (x3) outside the model's {node_count} nodes")
            n1s_ = n1 - 256 if n1 >= 128 else n1
            if n1s_ >= 0 and (n1s_ % 3 or n1s_ // 3 >= node_count):
                raise ConvertError(f"part: vertex node {n1s_} (x3) outside the model's {node_count} nodes")
            n0s.append(n0 // 3)
            n1s.append(n1s_ // 3 if n1s_ >= 0 else -1)
            w0s.append(max(0, min(WEIGHT_ONE, w)))
        tris = strip_to_triangles(idx, pos)
        sid = shaders[shader] if 0 <= shader < len(shaders) else NO_SHADER
        return SrcPart(sid, pos, nrm, uv, n0s, n1s, w0s, tris)


def strip_to_triangles(idx, pos) -> list[tuple[int, int, int]]:
    """D3D strip -> triangle list in the strip's (clockwise-front) winding. Triangles with a repeated index (the
    stitches) and triangles of exactly zero area are dropped; the parity of the strip position is kept."""
    out = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        if k & 1:
            b, c = c, b
        p, q, r = pos[a], pos[b], pos[c]
        ux, uy, uz, vx, vy, vz = q[0] - p[0], q[1] - p[1], q[2] - p[2], r[0] - p[0], r[1] - p[1], r[2] - p[2]
        if uy * vz - uz * vy == 0.0 and uz * vx - ux * vz == 0.0 and ux * vy - uy * vx == 0.0:
            continue
        out.append((a, b, c))
    return out


def conj(q: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    """Halo stores the conjugate of the Hamilton quaternion (module docstring)."""
    return (-q[0], -q[1], -q[2], q[3])


def read_model(m: texconv.MapData, entry, skip_lod0: bool = False, keep_variants: set[str] | None = None) -> ModelData:
    """`skip_lod0`: the super high LOD is not converted (permutations use their high geometry for LOD 0 too).
    `keep_variants`: only the permutations named in the set, plus the first one of every region (what
    `model_select_permutations` picks for a placement without a variant), are converted; markers of dropped
    permutations go with them. None keeps every permutation."""
    rd = _Reader(m)
    r = m.region
    off = m.tag_off(entry)
    if off < 0 or off + T_HEADER > len(r):
        raise ConvertError(f"{entry.path}: model header outside the tag region")
    cutoffs = struct.unpack_from("<5f", r, off + T_CUTOFFS)
    uvs = struct.unpack_from("<2f", r, off + T_UV_SCALE)
    uvs = tuple(s if 0.0 < s <= 64.0 else 1.0 for s in uvs)
    # nodes
    nc, no = rd.refl(off + T_NODES, T_NODE, "nodes")
    if nc > MAX_NODES:
        raise ConvertError(f"{entry.path}: {nc} nodes (the PS2 format allows {MAX_NODES})")
    nodes = []
    for i in range(nc):
        o = no + i * T_NODE
        sib, child, parent = struct.unpack_from("<3h", r, o + 0x20)
        t = struct.unpack_from("<3f", r, o + 0x28)
        q = struct.unpack_from("<4f", r, o + 0x34)
        for link in (sib, child, parent):
            if not -1 <= link < nc:
                raise ConvertError(f"{entry.path}: node {i} link {link} out of range")
        nodes.append(Node(_name(r[o:o + 32]), parent, child, sib, t, conj(q)))
    # regions and permutations
    rc, ro = rd.refl(off + T_REGIONS, T_REGION, "regions")
    if rc > MAX_REGIONS:
        raise ConvertError(f"{entry.path}: {rc} regions (the PS2 format allows {MAX_REGIONS})")
    gc, go = rd.refl(off + T_GEOMS, T_GEOM, "geometries")
    perms: list[Perm] = []
    perm_base = []
    for ri in range(rc):
        pc, po = rd.refl(ro + ri * T_REGION + 0x40, T_PERM, "permutations")
        perm_base.append(len(perms))
        for pi in range(pc):
            o = po + pi * T_PERM
            flags, = struct.unpack_from("<I", r, o + 0x20)
            stored = struct.unpack_from("<5h", r, o + 0x40)          # super low first
            lods = tuple(stored[MODEL_LODS - 1 - k] for k in range(MODEL_LODS))
            for g in lods:
                if g >= gc:
                    raise ConvertError(f"{entry.path}: permutation geometry {g} of {gc}")
            lods = fill_lods(lods)
            if skip_lod0 and lods[1] >= 0:
                lods = (lods[1],) + lods[1:]
            perms.append(Perm(_name(r[o:o + 32]), ri, flags & PERM_NO_RANDOM, lods))
    total_perms = len(perms)
    flat_map: dict[int, int | None] = {i: i for i in range(len(perms))}
    if keep_variants is not None:
        kept, seen_regions = [], set()
        for i, p in enumerate(perms):
            first = p.region not in seen_regions
            seen_regions.add(p.region)
            if first or p.name in keep_variants:
                flat_map[i] = len(kept)
                kept.append(p)
            else:
                flat_map[i] = None
        perms = kept
    # shaders
    sc, so = rd.refl(off + T_SHADERS, T_SHADER, "shaders")
    shaders = [struct.unpack_from("<I", r, so + i * T_SHADER + 12)[0] for i in range(sc)]
    # geometries actually used
    used = sorted({g for p in perms for g in p.geometry if g >= 0})
    geoms: dict[int, list[SrcPart]] = {}
    for g in used:
        pc, po = rd.refl(go + g * T_GEOM + 0x24, T_PART, "parts")
        parts = []
        for pi in range(pc):
            part = rd.part(po + pi * T_PART, nc, uvs, shaders)
            if part.tris:
                parts.append(part)
        geoms[g] = parts
    # markers
    markers: list[Marker] = []
    mc, mo = rd.refl(off + T_MARKERS, T_MARKER_GROUP, "markers")
    for gi in range(mc):
        o = mo + gi * T_MARKER_GROUP
        name = _name(r[o:o + 32])
        ic, io = rd.refl(o + 0x34, T_MARKER, "marker instances")
        for k in range(ic):
            p = io + k * T_MARKER
            reg, perm, node, _pad = struct.unpack_from("<4B", r, p)
            t = struct.unpack_from("<3f", r, p + 4)
            q = struct.unpack_from("<4f", r, p + 16)
            old = perm_base[reg] + perm if reg < rc and perm < (perm_base[reg + 1] if reg + 1 < rc else total_perms) - perm_base[reg] else None
            if old is not None and flat_map[old] is None:
                continue                                   # the marker belongs to a permutation that was not kept
            flat = ANY if old is None else flat_map[old]
            markers.append(Marker(name, reg if reg < rc else ANY, flat, node if node < nc else NO_NODE, t, conj(q)))
    cut = tuple(reversed(cutoffs))
    if skip_lod0:
        cut = (cut[1],) + cut[1:]
    return ModelData(entry.tag_id, entry.path, nodes, markers, rc, perms, cut, geoms)


def fill_lods(lods: tuple[int, ...]) -> tuple[int, ...]:
    """A missing (-1) LOD takes the next more detailed one that exists, else the next less detailed (never seen on retail data)."""
    out = list(lods)
    for k in range(MODEL_LODS):
        if out[k] < 0:
            cand = [lods[j] for j in list(range(k - 1, -1, -1)) + list(range(k + 1, MODEL_LODS)) if lods[j] >= 0]
            out[k] = cand[0] if cand else -1
    return tuple(out)


# ---- baking ----------------------------------------------------------------------------------------------
def _f32(v: float) -> float:
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _f32_up(v: float) -> float:
    r = _f32(v)
    return r if r >= v else _f32(v * (1.0 + 2.0 ** -22) + 1e-30)


def _s16(v: float) -> int:
    return max(-32768, min(32767, int(round(v))))


def _s8(v: float) -> int:
    return max(-127, min(127, int(round(v * 127.0))))


def sphere(points) -> tuple[tuple[float, float, float], float]:
    """Bounding sphere (centre = box centre, float32 rounded; radius covers every point)."""
    pts = list(points)
    if not pts:
        return (0.0, 0.0, 0.0), 0.0
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    c = tuple(_f32((lo[k] + hi[k]) * 0.5) for k in range(3))
    return c, _f32_up(max(math.dist(c, p) for p in pts))


def influences(part: SrcPart, v: int) -> tuple[tuple[int, int], tuple[int, int]]:
    """((node, weight), (node, weight)) of vertex v, strongest first; weights sum to 32767; a rigid vertex has the
    same node twice with weights (32767, 0)."""
    n0, n1, w0 = part.node0[v], part.node1[v], part.weight0[v]
    if n1 < 0 or n1 == n0 or w0 >= WEIGHT_ONE:
        return (n0, WEIGHT_ONE), (n0, 0)
    w1 = WEIGHT_ONE - w0
    return ((n0, w0), (n1, w1)) if w0 >= w1 else ((n1, w1), (n0, w0))


@dataclass
class Batch:
    part: int                          # local part index
    triangles: int
    palette: list[int]
    vif: bytes


@dataclass
class BakedPart:
    shader_id: int
    triangles: int
    uv_shift: int
    rigid_node: int
    flags: int
    centre: tuple[float, float, float]
    radius: float
    batches: list[Batch]


def rigid_node_of(part: SrcPart, used: list[int]) -> int:
    first = influences(part, used[0])[0][0]
    for v in used:
        a, b = influences(part, v)
        if a[0] != first or b[1] != 0:
            return NO_NODE
    return first


def pack_batch(part: SrcPart, tris: list[tuple[int, int, int]], palette: list[int], shift: int) -> bytes:
    slot = {n: i for i, n in enumerate(palette)}
    pts = [part.pos[v] for t in tris for v in t]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    c = tuple(_f32((lo[k] + hi[k]) * 0.5) for k in range(3))
    half = max(max(abs(p[k] - c[k]) for p in pts) for k in range(3))
    scale = _f32_up(half / 32767.0) if half > 0.0 else 1.0
    pos_b, uv_b, nrm_b = bytearray(), bytearray(), bytearray()
    one = 1 << shift
    for t in tris:
        for v in t:
            p = part.pos[v]
            (na, wa), (nb, _wb) = influences(part, v)
            pos_b += struct.pack("<4h", *[_s16((p[k] - c[k]) / scale) for k in range(3)], slot[na])
            u, w = (x if math.isfinite(x) else 0.0 for x in part.uv[v])
            uv_b += struct.pack("<4h", _s16(u * one), _s16(w * one), slot[nb], wa)
            nx, ny, nz = part.nrm[v]
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            if ln > 0.0:
                nx, ny, nz = nx / ln, ny / ln, nz / ln
            nrm_b += struct.pack("<4b", _s8(nx), _s8(ny), _s8(nz), 0)
    n = len(tris) * 3
    raw = (bspconv._unpack(VIF_UNPACK_V4_32, TOPS_RELATIVE | VU_HDR, 2, struct.pack("<4f4i", *c, scale, n, 0, 0, 0))
           + bspconv._unpack(VIF_UNPACK_V4_16, TOPS_RELATIVE | VU_POS, n, bytes(pos_b))
           + bspconv._unpack(VIF_UNPACK_V4_16, TOPS_RELATIVE | VU_UV, n, bytes(uv_b))
           + bspconv._unpack(VIF_UNPACK_V4_8, TOPS_RELATIVE | VU_NRM, n, bytes(nrm_b)))
    raw += bytes(-len(raw) % 16)
    assert len(raw) == bspconv.packet_bytes(len(tris))
    return raw


def split_batches(part: SrcPart) -> list[tuple[list[tuple[int, int, int]], list[int]]]:
    """Greedy runs of triangles (strip order) with <= 24 triangles and <= 24 distinct nodes each."""
    out: list[tuple[list, list]] = []
    cur: list[tuple[int, int, int]] = []
    pal: list[int] = []
    for t in part.tris:
        nodes = {n for v in t for n, w in influences(part, v) if n >= 0}
        new = nodes - set(pal)
        if cur and (len(cur) == MAX_BATCH_TRIS or len(pal) + len(new) > MAX_PALETTE):
            out.append((cur, pal))
            cur, pal, new = [], [], nodes
        cur.append(t)
        pal += sorted(new)
    if cur:
        out.append((cur, pal))
    return out


def bake_part(index: int, part: SrcPart, flags: int) -> BakedPart:
    used = sorted({v for t in part.tris for v in t})
    uvs = [x for v in used for x in part.uv[v] if math.isfinite(x)]
    shift = bspconv.uv_shift([(x, 0.0) for x in uvs]) if uvs else 0
    shift = min(shift, MAX_UV_SHIFT)
    batches = [Batch(index, len(tris), pal, pack_batch(part, tris, pal, shift)) for tris, pal in split_batches(part)]
    c, r = sphere(part.pos[v] for v in used)
    return BakedPart(part.shader_id, len(part.tris), shift, rigid_node_of(part, used), flags, c, r, batches)


@dataclass
class BakedModel:
    data: ModelData
    parts: list[BakedPart]
    ranges: list[list[tuple[int, int]]]      # per permutation, per LOD: (first local part, count)
    centre: tuple[float, float, float]
    radius: float
    triangles_lod0: int


def bake_model(md: ModelData, shader_flags: dict[int, int] | None = None) -> BakedModel:
    shader_flags = shader_flags or {}
    parts: list[BakedPart] = []
    geom_range: dict[int, tuple[int, int]] = {}
    for g in sorted(md.geometries):
        first = len(parts)
        for sp in md.geometries[g]:
            parts.append(bake_part(len(parts), sp, shader_flags.get(sp.shader_id, 0)))
        geom_range[g] = (first, len(parts) - first)
    ranges = [[geom_range.get(g, (0, 0)) for g in p.geometry] for p in md.perms]
    lod0 = [pt for p in md.perms for pt in md.geometries.get(p.geometry[0], [])]
    centre, radius = sphere(pos for sp in lod0 for pos in sp.pos)
    tris, seen_regions = 0, set()
    for p, rg in zip(md.perms, ranges):
        if p.region not in seen_regions:
            seen_regions.add(p.region)
            first, count = rg[0]
            tris += sum(parts[i].triangles for i in range(first, first + count))
    return BakedModel(md, parts, ranges, centre, radius, tris)


# ---- serialising ------------------------------------------------------------------------------------------
def _name_bytes(name: str) -> bytes:
    return name.encode("latin1", "replace")[:NAME_LEN - 1].ljust(NAME_LEN, b"\0")


def build_modl(models: list[ModelData], shader_flags: dict[int, int] | None = None) -> bytes:
    baked = [bake_model(md, shader_flags) for md in sorted(models, key=lambda md: md.tag_id)]
    tags = [b.data.tag_id for b in baked]
    if len(set(tags)) != len(tags):
        raise ConvertError("duplicate model tag id")
    tabs = {name: bytearray() for name in TABLES}
    counts = {name: 0 for name in TABLES}
    total_tris = 0
    for b in baked:
        md = b.data
        if len(md.nodes) > MAX_NODES:
            raise ConvertError(f"{md.path}: too many nodes")
        first_node, first_marker, first_perm = counts["nodes"], counts["markers"], counts["perms"]
        first_part, first_batch = counts["parts"], counts["batches"]
        for nd in md.nodes:
            tabs["nodes"] += struct.pack(NODE_FMT, _name_bytes(nd.name), nd.parent, nd.first_child, nd.next_sibling, 0,
                                         *nd.translation, *nd.rotation, nd.scale)
        for mk in md.markers:
            tabs["markers"] += struct.pack(MARKER_FMT, _name_bytes(mk.name), mk.region, mk.permutation, mk.node, 0,
                                           *mk.translation, *mk.rotation)
        for p, rg in zip(md.perms, b.ranges):
            flat = [v for first, count in rg for v in (first_part + first, count)]
            tabs["perms"] += struct.pack(PERM_FMT, _name_bytes(p.name), p.region, p.flags, *flat)
        for pi, pt in enumerate(b.parts):
            tabs["parts"] += struct.pack(PART_FMT, pt.shader_id, counts["batches"], len(pt.batches), pt.triangles,
                                         pt.uv_shift, pt.rigid_node & 0xFFFF, pt.flags, *pt.centre, pt.radius)
            for bt in pt.batches:
                pal = bytes(bt.palette).ljust(MAX_PALETTE, b"\0")
                tabs["batches"] += struct.pack(BATCH_FMT, first_part + pi, bt.triangles, len(bt.palette),
                                               len(tabs["vif"]), len(bt.vif) // 16, pal)
                tabs["vif"] += bt.vif
                counts["batches"] += 1
        tabs["models"] += struct.pack(MODEL_FMT, md.tag_id, len(md.nodes), len(md.markers),
                                      md.region_count, len(md.perms), first_node, first_marker, first_perm,
                                      *b.centre, b.radius, *md.lod_cutoff, b.triangles_lod0, first_part, len(b.parts))
        counts["nodes"] += len(md.nodes)
        counts["markers"] += len(md.markers)
        counts["perms"] += len(md.perms)
        counts["parts"] += len(b.parts)
        counts["models"] += 1
        total_tris += sum(pt.triangles for pt in b.parts)
        assert counts["batches"] - first_batch == sum(len(pt.batches) for pt in b.parts)
    for name in TABLES[:-1]:
        assert len(tabs[name]) == counts[name] * REC_SIZES[name], name
    if counts["parts"] > 0xFFFF or counts["batches"] > 0xFFFF or counts["perms"] > 0xFFFF:
        raise ConvertError("too many parts / batches / permutations for the u16 indices")
    counts["vif"] = len(tabs["vif"])
    out = bytearray(HEADER_SIZE)
    directory = {}
    for name in TABLES:
        directory[name] = (len(out), counts[name])
        out += bytes(tabs[name]) + bytes(-len(tabs[name]) % 16)
    struct.pack_into("<4sHHII", out, 0, MAGIC, VERSION, 0, len(out), total_tris)
    for i, name in enumerate(TABLES):
        struct.pack_into("<II", out, DIR_AT + 8 * i, *directory[name])
    return bytes(out)


# ---- reading back (tests, --info, debugging); mirrors the validation of engine/src/world/model.c ------------
@dataclass
class Section:
    triangles: int
    models: list[dict]
    nodes: list[dict]
    markers: list[dict]
    perms: list[dict]
    parts: list[dict]
    batches: list[dict]
    vif: bytes
    size: int = 0


def _finite(*vals: float) -> bool:
    return all(math.isfinite(v) and abs(v) < 1e30 for v in vals)


def _terminated(raw: bytes) -> bool:
    return raw[NAME_LEN - 1] == 0


def parse_section(data: bytes) -> Section:
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ConvertError("not a MODL section")
    _m, ver, _r, total, tris = struct.unpack_from("<4sHHII", data)
    if ver != VERSION:
        raise ConvertError(f"MODL version {ver} (this reader handles {VERSION})")
    if total != len(data):
        raise ConvertError("section size field disagrees with the data")
    d = {}
    for i, name in enumerate(TABLES):
        off, count = struct.unpack_from("<II", data, DIR_AT + 8 * i)
        if off % 16 or off < HEADER_SIZE or off > len(data) or count * REC_SIZES[name] > len(data) - off:
            raise ConvertError(f"table {name}: bad offset/count")
        d[name] = (off, count)
    if d["parts"][1] > 0xFFFF or d["batches"][1] > 0xFFFF or d["perms"][1] > 0xFFFF:
        raise ConvertError("table counts exceed the u16 indices")

    def rec(name: str, fmt: str, i: int):
        return struct.unpack_from(fmt, data, d[name][0] + i * REC_SIZES[name])

    vif = data[d["vif"][0]:d["vif"][0] + d["vif"][1]]
    nn, nm, npm, npt, nb = d["nodes"][1], d["markers"][1], d["perms"][1], d["parts"][1], d["batches"][1]
    models, nodes, markers, perms, parts, batches = [], [], [], [], [], []
    for i in range(nn):
        name, par, ch, sib, _p, tx, ty, tz, qx, qy, qz, qw, sc = rec("nodes", NODE_FMT, i)
        if not _terminated(name) or not _finite(tx, ty, tz, qx, qy, qz, qw, sc):
            raise ConvertError(f"node {i}: bad name or value")
        nodes.append({"name": name.split(b"\0")[0].decode("latin1"), "parent": par, "first_child": ch,
                      "next_sibling": sib, "translation": (tx, ty, tz), "rotation": (qx, qy, qz, qw), "scale": sc})
    for i in range(nm):
        name, reg, perm, node, _p, tx, ty, tz, qx, qy, qz, qw = rec("markers", MARKER_FMT, i)
        if not _terminated(name) or not _finite(tx, ty, tz, qx, qy, qz, qw):
            raise ConvertError(f"marker {i}: bad name or value")
        markers.append({"name": name.split(b"\0")[0].decode("latin1"), "region": reg, "permutation": perm, "node": node,
                        "translation": (tx, ty, tz), "rotation": (qx, qy, qz, qw)})
    for i in range(npm):
        name, reg, flags, *rest = rec("perms", PERM_FMT, i)
        if not _terminated(name):
            raise ConvertError(f"permutation {i}: unterminated name")
        perms.append({"name": name.split(b"\0")[0].decode("latin1"), "region": reg, "flags": flags,
                      "ranges": [(rest[2 * k], rest[2 * k + 1]) for k in range(MODEL_LODS)]})
    for i in range(npt):
        sid, fb, bc, tr, shift, rigid, flags, cx, cy, cz, rad = rec("parts", PART_FMT, i)
        parts.append({"shader_id": sid, "first_batch": fb, "batch_count": bc, "triangles": tr, "uv_shift": shift,
                      "rigid_node": rigid - 0x10000 if rigid >= 0x8000 else rigid, "flags": flags,
                      "centre": (cx, cy, cz), "radius": rad})
    vif_at = 0
    for i in range(nb):
        pt, tr, pc, pad, off, qw, pal = rec("batches", PARSE_BATCH_FMT, i)
        if not 1 <= tr <= MAX_BATCH_TRIS or not 1 <= pc <= MAX_PALETTE or off != vif_at or off % 16 \
                or qw * 16 != bspconv.packet_bytes(tr) or off + qw * 16 > len(vif) or pt >= npt \
                or any(pad) or any(pal[pc:]):
            raise ConvertError(f"batch {i}: bad size/offset/palette")
        _check_packet(vif[off:off + qw * 16], tr)
        vif_at += qw * 16
        batches.append({"part": pt, "triangles": tr, "palette": list(pal[:pc]), "vif_offset": off, "vif_qwords": qw})
    if vif_at != len(vif):
        raise ConvertError("VIF data is not exactly covered by the batches")
    run_node = run_marker = run_perm = run_part = run_batch = 0
    last_tag = -1
    sum_tris = 0
    for i in range(d["models"][1]):
        (tag, ncnt, mcnt, rcnt, pcnt, fn, fm, fp, cx, cy, cz, rad, *rest) = rec("models", MODEL_FMT, i)
        cut, lod0, fpart, npart = rest[:5], rest[5], rest[6], rest[7]
        if tag <= last_tag or ncnt > MAX_NODES or rcnt > MAX_REGIONS or fn != run_node or fn + ncnt > nn or fm != run_marker \
                or fm + mcnt > nm or fp != run_perm or fp + pcnt > npm or fpart != run_part or fpart + npart > npt \
                or not _finite(cx, cy, cz, rad, *cut) or rad < 0.0:
            raise ConvertError(f"model {i}: bad ranges or values")
        last_tag = tag
        for k in range(fn, fn + ncnt):
            n = nodes[k]
            if any(not -1 <= n[f] < ncnt for f in ("parent", "first_child", "next_sibling")):
                raise ConvertError(f"model {i}: node link out of range")
        for k in range(fm, fm + mcnt):
            mk = markers[k]
            if not -1 <= mk["node"] < ncnt or (mk["region"] != ANY and mk["region"] >= rcnt) \
                    or (mk["permutation"] != ANY and mk["permutation"] >= pcnt):
                raise ConvertError(f"model {i}: marker out of range")
        last_region = 0
        for k in range(fp, fp + pcnt):
            pm = perms[k]
            if pm["region"] >= rcnt or pm["region"] < last_region:
                raise ConvertError(f"model {i}: permutation region out of order")
            last_region = pm["region"]
            for first, count in pm["ranges"]:
                if first < fpart or first + count > fpart + npart:
                    raise ConvertError(f"model {i}: permutation range outside the model's parts")
        for k in range(fpart, fpart + npart):
            pt = parts[k]
            if pt["first_batch"] != run_batch or pt["batch_count"] < 1 or pt["triangles"] < 1 \
                    or run_batch + pt["batch_count"] > nb or pt["uv_shift"] > MAX_UV_SHIFT or pt["flags"] > 3 \
                    or not -1 <= pt["rigid_node"] < ncnt or not _finite(*pt["centre"], pt["radius"]) or pt["radius"] < 0.0:
                raise ConvertError(f"part {k}: bad batch range or values")
            t = 0
            for bi in range(run_batch, run_batch + pt["batch_count"]):
                b = batches[bi]
                if b["part"] != k or any(n >= ncnt for n in b["palette"]):
                    raise ConvertError(f"batch {bi}: bad part or palette node")
                t += b["triangles"]
            if t != pt["triangles"]:
                raise ConvertError(f"part {k}: triangle count disagrees with its batches")
            sum_tris += t
            run_batch += pt["batch_count"]
        run_node += ncnt
        run_marker += mcnt
        run_perm += pcnt
        run_part += npart
        models.append({"tag_id": tag, "node_count": ncnt, "marker_count": mcnt, "region_count": rcnt,
                       "permutation_count": pcnt, "first_node": fn, "first_marker": fm, "first_range": fp,
                       "centre": (cx, cy, cz), "radius": rad, "lod_cutoff": tuple(cut), "triangles_lod0": lod0,
                       "first_part": fpart, "part_count": npart})
    if (run_node, run_marker, run_perm, run_part, run_batch) != (nn, nm, npm, npt, nb) or sum_tris != tris:
        raise ConvertError("tables are not exactly partitioned by the models")
    return Section(tris, models, nodes, markers, perms, parts, batches, vif, len(data))


def _check_packet(p: bytes, tris: int) -> None:
    n = tris * 3
    exp = [(VIF_UNPACK_V4_32, 2, VU_HDR), (VIF_UNPACK_V4_16, n, VU_POS), (VIF_UNPACK_V4_16, n, VU_UV),
           (VIF_UNPACK_V4_8, n, VU_NRM)]
    o = 0
    for k, (cmd, num, addr) in enumerate(exp):
        if struct.unpack_from("<I", p, o)[0] != (cmd << 24) | (num << 16) | TOPS_RELATIVE | addr:
            raise ConvertError("VIF packet: unexpected UNPACK word")
        o += 4
        if k == 0:
            if struct.unpack_from("<4i", p, o + 16) != (n, 0, 0, 0):
                raise ConvertError("VIF packet: bad vertex count constants")
            o += 32
        else:
            o += num * {1: 8, 2: 8, 3: 4}[k]
    if any(p[o:]):
        raise ConvertError("VIF packet: non-zero padding")


def decode_batch(sec: Section, batch: dict, part: dict) -> list[dict]:
    """Expanded vertices of a batch: model-space position, uv, node 0/1 (model node indices), weights, normal."""
    raw = sec.vif[batch["vif_offset"]:batch["vif_offset"] + batch["vif_qwords"] * 16]
    cx, cy, cz, scale = struct.unpack_from("<4f", raw, 4)
    n = batch["triangles"] * 3
    po = 4 + 32 + 4
    uo = po + n * 8 + 4
    no = uo + n * 8 + 4
    pal = batch["palette"]
    shift = float(1 << part["uv_shift"])
    out = []
    for i in range(n):
        x, y, z, s0 = struct.unpack_from("<4h", raw, po + i * 8)
        s, t, s1, w = struct.unpack_from("<4h", raw, uo + i * 8)
        nx, ny, nz, _ = struct.unpack_from("<4b", raw, no + i * 4)
        out.append({"pos": (cx + x * scale, cy + y * scale, cz + z * scale), "uv": (s / shift, t / shift),
                    "nodes": (pal[s0], pal[s1]), "weight": w / 32767.0, "normal": (nx / 127.0, ny / 127.0, nz / 127.0)})
    return out


# ---- whole map ------------------------------------------------------------------------------------------------
@dataclass
class ConvertResult:
    modl: bytes
    otex: bytes
    models: list[ModelData]
    otex_result: objtex.OtexResult
    seconds: float = 0.0
    untextured_parts: int = 0
    parts: int = 0
    triangles: int = 0
    skipped: list[int] = field(default_factory=list)

    @property
    def sections(self) -> list[tuple[str, bytes]]:
        return [("MODL", self.modl), ("OTEX", self.otex)]


def part_flags(info: texconv.ShaderInfo | None) -> int:
    if info is None:
        return 0
    if info.blend == texconv.BLEND_ALPHA_TEST:
        return PF_ALPHA_TEST
    return PF_BLEND if info.blend >= texconv.BLEND_ALPHA else 0


def add_sky_layers(m: texconv.MapData, md: ModelData) -> None:
    """A sky dome whose chicago shader stacks maps (the a10 `sky_x10` dome: nebula + stars) is drawn once per additive map: every part
    of such a shader is followed by a copy with a pseudo shader id (objtex.layer_shader_id) that resolves to that map, additive."""
    for parts in md.geometries.values():
        for sp in list(parts):
            if sp.shader_id == NO_SHADER:
                continue
            for index in objtex.chicago_layers(m, sp.shader_id):
                parts.append(SrcPart(objtex.layer_shader_id(sp.shader_id, index), sp.pos, sp.nrm, sp.uv, sp.node0, sp.node1, sp.weight0, sp.tris))


def convert_map(path: Path | texconv.MapData, model_tags: list[int] | None = None,
                tex_budget: int = objtex.DEFAULT_BUDGET, max_tex: int = texconv.MAX_TEX,
                skip_lod0: bool = False, variants: dict[int, set[str]] | None = None) -> ConvertResult:
    t0 = time.time()
    m = path if isinstance(path, texconv.MapData) else texconv.MapData.open(path)
    entries = {e.tag_id: e for e in m.index.entries if e.group == "mode"}
    if model_tags is None:
        wanted = sorted(entries)
        skipped: list[int] = []
    else:
        wanted = sorted(set(model_tags) & set(entries))
        skipped = sorted(set(model_tags) - set(entries))
    models = [read_model(m, entries[t], skip_lod0, variants.get(t) if variants is not None else None) for t in wanted]
    for md in models:
        if md.path.startswith("sky\\"):
            add_sky_layers(m, md)
    shader_ids = [sp.shader_id for md in models for parts in md.geometries.values() for sp in parts if sp.shader_id != NO_SHADER]
    otex = objtex.build_otex(m, shader_ids, tex_budget, max_tex)
    flags = {sid: part_flags(info) for sid, info in otex.shaders.items()}
    modl = build_modl(models, flags)
    parts = untextured = triangles = 0
    for md in models:
        for geo in md.geometries.values():
            for sp in geo:
                parts += 1
                triangles += len(sp.tris)
                info = otex.shaders.get(sp.shader_id)
                untextured += info is None or info.texture == texconv.NO_TEXTURE
    return ConvertResult(modl, otex.blob, models, otex, time.time() - t0, untextured, parts, triangles, skipped)


def build_sections(map_path: Path, model_tags: list[int] | None = None) -> list[tuple[str, bytes]]:
    """[("MODL", ...), ("OTEX", ...)] for the map's model tags (all of them when `model_tags` is None)."""
    return convert_map(map_path, model_tags).sections


# ---- CLI -------------------------------------------------------------------------------------------------------
def describe(sec: Section, paths: dict[int, str] | None = None) -> list[str]:
    lines = []
    for i, md in enumerate(sec.models):
        parts = sec.parts[md["first_part"]:md["first_part"] + md["part_count"]]
        batches = sum(p["batch_count"] for p in parts)
        size = sum(b["vif_qwords"] * 16 for p in parts for b in sec.batches[p["first_batch"]:p["first_batch"] + p["batch_count"]])
        skinned = sum(1 for p in parts if p["rigid_node"] < 0)
        name = (paths or {}).get(md["tag_id"], "")
        lines.append(f"{md['tag_id']:#010x} {name:<48} nodes {md['node_count']:2d} markers {md['marker_count']:3d} "
                     f"perms {md['permutation_count']:2d}/{md['region_count']} parts {len(parts):3d} (skinned {skinned:3d}) "
                     f"batches {batches:4d} tris {sum(p['triangles'] for p in parts):6d} lod0 {md['triangles_lod0']:5d} "
                     f"vif {size / 1024:7.1f} KiB")
    return lines


def info(path: Path) -> None:
    if path.suffix == ".map":
        r = convert_map(path)
        sec = parse_section(r.modl)
        print("\n".join(describe(sec, {md.tag_id: md.path for md in r.models})))
        print(f"{len(sec.models)} models, {len(sec.parts)} parts, {len(sec.batches)} batches, {sec.triangles} triangles, "
              f"MODL {len(r.modl) / 2**20:.2f} MiB, OTEX {len(r.otex) / 2**20:.2f} MiB")
        return
    pk = hp2.parse(path.read_bytes())
    for s in pk.sections:
        if s.tag == "MODL":
            sec = parse_section(pk.read(s.tag))
            print("\n".join(describe(sec)))
            print(f"MODL: {len(sec.models)} models, {len(sec.parts)} parts, {len(sec.batches)} batches, "
                  f"{sec.triangles} triangles, {s.raw_size / 2**20:.2f} MiB raw, {s.stored_size / 2**20:.2f} MiB stored")
        elif s.tag == "OTEX":
            v = texconv.parse_texs(pk.read(s.tag))
            print(f"OTEX: {len(v.textures)} textures, {len(v.bindings)} bindings "
                  f"({sum(1 for b in v.bindings if b['texture'] == texconv.NO_TEXTURE)} untextured), "
                  f"{s.raw_size / 2**20:.2f} MiB raw, {s.stored_size / 2**20:.2f} MiB stored")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--tags", help="comma separated model tag ids (default: every model of the map)")
    ap.add_argument("--info", action="store_true", help="print a summary per model of a .hp2 pack or a .map")
    ap.add_argument("--tex-budget", type=int, default=objtex.DEFAULT_BUDGET, help="object texture bytes per map")
    ap.add_argument("--max-tex", type=int, default=texconv.MAX_TEX, help="largest texture side (power of two)")
    ap.add_argument("--skip-lod0", action="store_true", help="drop the super high LOD (about a third less geometry)")
    args = ap.parse_args(argv)
    if args.info:
        info(args.input)
        return 0
    if not args.out:
        ap.error("-o is required")
    tags = [int(t, 0) for t in args.tags.split(",") if t] if args.tags else None
    r = convert_map(args.input, tags, args.tex_budget, args.max_tex, args.skip_lod0)
    pack = hp2.build(args.input.stem, r.sections)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(pack)
    o = r.otex_result
    print(f"wrote {args.out} ({len(pack) / 2**20:.2f} MiB): {len(r.models)} models, {r.parts} parts "
          f"({r.untextured_parts} untextured), {r.triangles} source triangles, "
          f"MODL {len(r.modl) / 2**20:.2f} MiB, OTEX {len(r.otex) / 2**20:.2f} MiB "
          f"({len(o.textures)} textures, max side {o.max_dim}, {o.untextured_shaders}/{len(o.shaders)} shaders untextured), "
          f"{r.seconds:.1f} s")
    if r.skipped:
        print(f"warning: {len(r.skipped)} requested tag ids are not model tags of this map: "
              + ", ".join(f"{t:#x}" for t in r.skipped), file=sys.stderr)
    if len(r.modl) > RESIDENT_BUDGET:
        print(f"warning: MODL is {len(r.modl) / 2**20:.2f} MiB, over the {RESIDENT_BUDGET // 2**20} MiB resident budget "
              "(pass --tags with the models the scenario places)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
