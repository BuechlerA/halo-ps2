#!/usr/bin/env python3
"""Convert Xbox `model_animations` (group 'antr') tags into the PS2 `ANIM` section
(docs/12-anim-section.md, engine/src/world/anim.h).

Usage:
    python3 -I tools/halo2ps2/animconv.py assets/decompressed/a10.map -o assets/hp2/a10.anim.hp2 [--tags 0xe6e50571 ...]
    python3 -I tools/halo2ps2/animconv.py assets/decompressed/a10.map --info [--verify]
    python3 -I tools/halo2ps2/animconv.py assets/hp2/a10.anim.hp2 --info

=====================================================================================================
Xbox `model_animations` layout (engine version 5), as read by this module
=====================================================================================================
Verified = checked by an assertion or a numeric test on all 8219 animations of the 10 campaign maps
(decoding raises on any violation; `--verify` re-checks the invariants). Unverified = read from the
data but not exercised/confirmed, or inferred from names. Offsets are into the tag's data block;
pointers are `0x803A6000`-based (tagindex.py); every block is a reflexive: u32 count, u32 address, u32 0.

Tag struct (0x80 bytes). VERIFIED:
    +0x00  objects  (12 B/elem)      +0x0C units       +0x18 weapons     +0x24 vehicles
    +0x30  devices                   +0x3C unit damage +0x48 fp weapons  +0x54 sound references
    +0x60  f32 limp body node radius, +0x64 u16 flags
    +0x68  nodes       reflexive, 64 B/elem
    +0x74  animations  reflexive, 180 B/elem
  Only `nodes` and `animations` are converted. The unit/weapon/vehicle/device/damage blocks (the
  graph's state tables: which animation plays in which state) and the sound references are NOT
  converted (anim.h has no place for them; the animation names carry the state, e.g.
  "stand rifle idle": see docs/12-anim-section.md "Not converted").

Node (64 B). VERIFIED:  +0 char[32] name, +0x20 i16 next sibling, +0x22 i16 first child, +0x24 i16
parent (the root stores 0, not -1), +0x28 u32 joint flags (1 ball-socket, 2 hinge, 4 none),
+0x2C f32[3] base vector, +0x38 f32 vector range, +0x3C pad. The node block is EMPTY for vehicles
and devices (warthog, ghost, doors, trees): their animations carry `node_count` (the model's node
count); the graph's node count is then the largest animation node count.

Animation (180 B). VERIFIED unless noted:
    +0x00 char[32] name           (e.g. "stand pistol idle%1": '%n' = variant n)
    +0x20 i16 type                0 base, 1 overlay, 2 replacement
    +0x22 i16 frame count         +0x24 i16 frame size (bytes of one frame of `frame data`, uncompressed)
    +0x26 i16 frame info type     0 none, 1 dx dy, 2 dx dy dyaw, 3 dx dy dz dyaw (f32 per component)
    +0x28 u32 node list checksum  == `model.node_list_checksum` (+4 of the model tag) of every model the
                                  graph can drive (checked on all maps); the compatibility test the engine uses
    +0x2C i16 node count          +0x2E i16 loop frame index
    +0x30 f32 weight, +0x34 i16 key frame, +0x36 i16 second key frame, +0x38 i16 next animation (-1 = none;
          chains the '%n' variants), +0x3A u16 flags, +0x3C i16 sound, +0x3E i16 sound frame,
          +0x40 i8 left foot frame, +0x41 i8 right foot frame, +0x42 i16 main animation, +0x44 f32 relative weight
          (UNVERIFIED semantics: read and ignored)
    +0x48 tag-data   frame info    (20 B: u32 size, u32 flags, u32 file offset, u32 address, u32 id)
    +0x5C u64 trans flags, +0x64 8 pad, +0x6C u64 rotation flags, +0x74 8 pad, +0x7C u64 scale flags, +0x84 8 pad
          bit n set = node n has that channel ANIMATED (stored per frame); clear = STATIC (stored in
          `default data`). Bits >= node count are garbage in some tags and are masked. 64-bit: nodes >= 32 use the
          second dword.
    +0x8C tag-data   default data
    +0xA0 tag-data   frame data
  flags: bit 0 compressed data (186 animations over the ten maps: cinematics and a few others), bit 1 world relative
  (16 animations, the c10 cinematics; informational), bit 2 25 Hz (never set). Nothing else is set.
  The tag has no "loops" flag; Halo's unit state machine loops idle/move states. anim.h's ANIM_LOOPS is
  therefore DERIVED (UNVERIFIED against the engine): base animations named idle*, move-*, airborne
  (names are "<posture> <weapon> <action>"). `loop frame index` is non-zero only for airborne (loop point) and
  land-* (a marker frame, not a loop point): it is passed through but only meaningful with ANIM_LOOPS.

Uncompressed data (flag 0 clear). VERIFIED (sizes match on every animation, quaternions are unit length):
    default data = for each node in order: [rot if not animated] [trans if not animated] [scale if not
                   animated]; rot = 4 x s16 (x y z w) / 32767, trans = 3 x f32 (world units, 1 wu = 3.048 m),
                   scale = f32
    frame data   = fc frames of `frame size` bytes; one frame = for each node in order: [rot] [trans]
                   [scale] if animated, same element encodings
    frame info   = fc frames of 2/3/4 f32: the MOTION OF THAT FRAME (per-frame deltas: a walk cycle's dx sum is
                   one stride, a 90 degree turn's dyaw sums to 1.571, turn-left has positive dyaw); VERIFIED on cyborg idle/walk/turn.
  Quaternions are not hemisphere-continuous (q and -q alternate between frames in some tracks); the
  converter does not rely on it and the runtime blends along the shortest path.
  QUATERNION CONVENTION (VERIFIED, same as the model tag, docs/11-model-section.md): Halo stores the CONJUGATE of the
  Hamilton quaternion, in animations as in model nodes. Posing the cyborg skeleton with the stored values read
  as Hamilton quaternions scatters the bind pose (feet 0.33 wu to the side), the conjugate stands it up
  (feet at 0.07, head 0.61, hands level with the hips, T pose) and makes the walk cycles walk. This module
  conjugates every rotation when decoding (`hamilton`), so the ANIM section and its rest poses use the engine's
  Hamilton convention (x y z w), exactly like the MODL nodes.

Compressed data (flag 1): frame data only; default data and (scale) are absent. VERIFIED by decoding the
c10/a10 cinematics and cross-checking one against its uncompressed twin (see "Cross-check" below). Header of
the blob (u32 offsets from the blob start):
    h[0] rotation key frame index lists   h[1] rotation default values    h[2] rotation key values
    h[3] translation channel table        h[4] translation key frame indices
    h[5] translation default values       h[6] translation key values
    h[7..10] scale (table, indices, defaults, keys): always == blob size, i.e. no scale in compressed data
  The rotation channel table sits right after the 44-byte header (offset 44, no pointer) and is one u32
  per rotation-animated node (ascending node order); a table entry is (first key << 12) | key count (12
  bits: <= 4095 keys). Key frame index lists are u16 frame numbers, ascending, ending at frame count - 1,
  never frame 0; channel c's keys are entries [first, first + count) of the concatenated list. h[0] ==
  44 + 4 * channels, h[1] == h[0] + 2 * total keys, h[2] == h[1] + 6 * node count (the default array has
  an entry for EVERY node: the static value, or for an animated node its frame-0 value), h[3] == h[2] + 6 *
  total keys, h[4] == h[3] + 4 * trans channels, h[5] == h[4] + 2 * keys, h[6] == h[5] + 12 * node count,
  h[7] == h[6] + 12 * keys. Translations are f32 x 3. Between keys the value is interpolated linearly
  (UNVERIFIED which blend Halo uses for rotation; nlerp is within the compression error), frame 0 is the
  default value, frames before the first key interpolate from it.
  Rotation values are 48-bit unit quaternions of four signed 12-bit components /2047, spread over the
  six bytes (little-endian 48-bit integer N, bit 0 = LSB), VERIFIED (sum of squares 1.0005 +- 0.0003 on
  every key of a 2812-key sample, hinge nodes have exactly two non-zero components, order cross-checked):
        x = bits 4..15                          (12 bits)
        y = bits 0..3 (high nibble) : bits 24..31 (low byte)
        z = bits 16..23 (high byte) : bits 44..47 (low nibble)
        w = bits 36..43 (high byte) : bits 32..35 (low nibble)
  (so the 'A,B,C,D' fields of the stream are x, z, y, w).
  Cross-check: the compressed cinematic "stand rifle idle" (chief x30 graph) and the uncompressed
  "stand rifle idle" of characters\\cyborg\\cyborg are the same motion: after decoding, per-frame node
  rotations differ by 0.048 degrees mean, 0.186 max (that is Halo's own compression error) and
  translations by 0.

Source oddities (all passed through, none affects the format): 505 uncompressed animations of the character graphs
hold a >100 degree rotation step between two adjacent frames (overlay pose sets like "aim-still" are not time
series; a few base animations, e.g. one "exit" frame, snap), and device animations ("device position") move
nodes by up to 1089 world units (platforms, the beam emitter); such tracks are stored as f32.

Graph <-> model: node i of the graph is node i of the model (VERIFIED: node names equal and counts equal for
every model that references a graph with a node block; the checksum equality holds for every graph whose
model is in the same map; weapon first-person graphs and `jenkins` have no model with that checksum,
because their node list is assembled at runtime / the model is elsewhere). Object tags reference the
model at +0x28 and the graph at +0x38 (tag reference: 4 group, 4 path pointer, 4 length, 4 tag id).

=====================================================================================================
`ANIM` section format, version 2 (little endian, offsets from the section start; docs/12-anim-section.md)
=====================================================================================================
    header (64 B)  "ANIM", u16 version 2, u16 flags 0, u32 graph_count, u32 anim_count, u32 graphs_off,
                   u32 anims_off, u32 pool_off, u32 total size, zero to 64
    graph (32 B)   u32 tag_id, u16 node_count, u16 anim_count, u32 first_anim, u32 default_off (pool
                   offset of node_count x 32 B anim_xform floats: x y z w, tx ty tz, scale), u32 model tag
                   id the rest pose was read from (0 = none: derived from animation data), u32 names_off (pool
                   offset of node_count x 32 B NUL-terminated node names in graph order; version 2: the engine
                   maps a model's nodes onto a graph by name, so a graph may order nodes differently from the
                   model, as the first-person weapon graphs do), zero to 32
    anim (96 B)    char[48] name, u16 type, u16 frame_count, u16 loop_frame, u16 flags, u16 node_count,
                   u16 root motion type (0..3 like `frame info type`; 0 = none or all zero), u16 node count of
                   the graph, 2 pad, f32 fps, u32 data_off, u32 data_bytes (pool-relative blob), u32 root_off
                   (pool-relative root motion block, 0 = none), u32 default_off (the graph's rest pose, so a
                   sample needs no graph lookup), zero to 96
    pool           16-byte aligned blobs: graph default poses, per-animation data, root motion; the first 16
                   bytes are zero so that offset 0 means "none"; identical blobs are stored once
    animation data blob:
        u8 mode[node_count] padded to 4: bits 0-1 rotation, 2-3 translation, 4-5 scale; 0 graph default,
        1 static (value follows), 2 animated (track follows); then for node 0..n-1 and channel rot, trans,
        scale in that order, skipping mode 0:
            static rot    4 x s16 x y z w /32767 (8 B)      static trans 3 x f32 (12 B)     static scale f32
            animated track (4-byte aligned, size a multiple of 4):
                u16 key count K (>= 2), u16 flags (1 = dense: K == frame_count, no index list;
                2 = u8 index list instead of u16, only when frame_count <= 256;
                4 = translation values are f32 x 3, not quantised, 8 = rotation about one axis only: keys are
                2 x s16 (axis component, w) / 32767, axis = flags >> 4 & 3 (0 x, 1 y, 2 z); hinge joints)
                translation, flags & 4 == 0: f32 base[3], f32 step[3]
                index list (unless dense): K x u8|u16 ascending frame numbers, first 0, last frame_count - 1,
                    padded to 4 bytes
                values: rotation K x 6 B, translation K x 6 B (u16 x 3: base + u16 * step) or K x 12 B f32,
                    scale K x f32; the track is padded to 4 bytes
            all rotations are in the engine's Hamilton convention (conjugates of the Halo values);
            rotation key (6 B): three s16 a b c: the smallest three components of the unit quaternion
                with the largest one made positive and dropped; 15-bit values (c << 1 | extra bit) of
                round(comp * 16383 * sqrt 2); the dropped component's index (0..3 = x y z w) is a's
                bit 0 | b's bit 1 << 1; c's bit 0 is zero. The dropped component is rebuilt as
                sqrt(1 - sum of squares).
    root motion block: f32 scale[4], then frame_count x k x s16 (k = 2, 3, 4 components dx dy [dz] dyaw),
        padded to 4 bytes; component = s16 / 32767 * scale[component]
    anim flags: 1 loops, 2 world relative, 4 STUB (only with build_section(..., animations_needed=...): the animation
    keeps its name, frame count and loop flags but has no pose data, every channel is mode 0 = the rest pose; the
    ASTB section of ansmconv.py references only the animations it names, see docs/19-ansm-section.md)
    Sampling (anim.c / `Section.sample`): frame f in [0, frame_count - 1] is evaluated by locating the key
    segment of each track (dense: floor(f); keyed: binary search) and interpolating linearly (nlerp for
    rotations along the shortest path). Looping animations wrap f >= frame_count to loop_frame +
    fmod(f - loop_frame, frame_count - loop_frame), and f in (frame_count - 1, frame_count) blends the last
    frame with the loop frame; non-looping animations clamp at the last frame.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, mapinfo, tagindex  # noqa: E402

MAGIC = b"ANIM"
VERSION = 2
HEADER_SIZE = 64
GRAPH_REC = 32
ANIM_REC = 96
NAME_LEN = 48
NODE_NAME_LEN = 32                  # per node name slot in the graph's name table (model node names are 32 B too)
MAX_NODES = 64
POOL_ALIGN = 16

# Xbox tag layout
NODE_REC = 64
ANTR_ANIM_REC = 180
ANTR_NODE_AT, ANTR_ANIM_AT = 0x68, 0x74
MODEL_NODE_REC = 156
MODEL_NODES_AT = 0xB8
OBJECT_GROUPS = ("bipd", "vehi", "weap", "eqip", "garb", "proj", "scen", "mach", "ctrl", "lifi", "plac", "ssce")
OBJ_MODEL_AT, OBJ_ANIM_AT = 0x28, 0x38      # tag references in the object base struct

# antr animation flags
F_COMPRESSED, F_WORLD_RELATIVE, F_PAL = 1, 2, 4
# ANIM flags (anim.h)
ANIM_LOOPS, ANIM_WORLD_RELATIVE = 1, 2
ANIM_STUB = 4                       # animations_needed: name/frames kept, no pose data (anim.h does not define it)
# channel modes
M_DEFAULT, M_STATIC, M_ANIMATED = 0, 1, 2
# track flags
T_DENSE, T_IDX8, T_F32, T_AXIS = 1, 2, 4, 8

WU_MM = 3048.0                      # millimetres per world unit
ROT_TOL_DEG = 0.18                  # keyed rotation tracks: max error vs the source frames, quantisation included
ROT_STATIC_DEG = 0.05               # a rotation channel within this of one value is static
TRANS_TOL = 0.35 / WU_MM            # keyed translation tracks (world units): 0.35 mm
AXIS_EPS = 1e-4                     # components this small are dropped by single-axis (hinge) rotation tracks
TRANS_STATIC = 0.08 / WU_MM
SCALE_TOL = 1e-3
SCALE_STATIC = 1e-4
LOOP_ACTIONS = ("idle", "move-front", "move-back", "move-left", "move-right", "airborne")
SQRT_HALF = math.sqrt(0.5)
Q15 = 16383
ANIM_FMT = "<48sHHHHHHH2xfIIII12x"
assert struct.calcsize(ANIM_FMT) == ANIM_REC
DEFAULT_BUDGET = 9 * 2**20          # docs/02-specs.md 5.3: resident mission data


class ConvertError(ValueError):
    pass


# ---- antr parsing ----------------------------------------------------------------------------
@dataclass
class Node:
    name: str
    next_sibling: int
    first_child: int
    parent: int
    joint_flags: int


@dataclass
class Anim:
    name: str
    type: int
    frame_count: int
    frame_size: int
    frame_info_type: int
    checksum: int
    node_count: int
    loop_frame: int
    next_anim: int
    flags: int
    trans_flags: int
    rot_flags: int
    scale_flags: int
    frame_info: bytes
    default_data: bytes
    frame_data: bytes


@dataclass
class Graph:
    tag_id: int
    path: str
    nodes: list[Node]
    anims: list[Anim]


@dataclass
class ModelRest:
    tag_id: int
    path: str
    checksum: int
    names: list[str]
    rot: list[tuple]
    trans: list[tuple]


class MapTags:
    """The tag region of a decompressed map plus its index."""

    def __init__(self, path: Path):
        self.path = Path(path)
        hdr = mapinfo.read_header(self.path)
        self.idx = tagindex.TagIndex.read(self.path)
        with self.path.open("rb") as f:
            f.seek(hdr.tag_data_offset)
            self.region = f.read(hdr.tag_data_size)
        self.byid = {e.tag_id: e for e in self.idx.entries}

    def off(self, addr: int) -> int:
        return addr - self.idx.base

    def u32(self, o: int) -> int:
        return struct.unpack_from("<I", self.region, o)[0]

    def blob(self, addr: int, size: int) -> bytes:
        if size == 0:
            return b""
        o = self.off(addr)
        if o < 0 or o + size > len(self.region):
            raise ConvertError(f"data block {addr:#x}+{size} outside the tag region")
        return self.region[o:o + size]

    def block(self, o: int, rec: int) -> list[bytes]:
        """Elements of the reflexive at tag offset o (count, address)."""
        count, addr = struct.unpack_from("<II", self.region, o)
        if count == 0:
            return []
        start = self.off(addr)
        if count > 4096 or start < 0 or start + count * rec > len(self.region):
            raise ConvertError(f"reflexive at {o:#x}: {count} x {rec} B outside the tag region")
        return [self.region[start + i * rec:start + (i + 1) * rec] for i in range(count)]


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin1")


def read_graph(t: MapTags, e: tagindex.TagEntry) -> Graph:
    o = t.off(e.data_address)
    nodes = []
    for b in t.block(o + ANTR_NODE_AT, NODE_REC):
        sib, child, par, _pad, fl = struct.unpack_from("<hhhhI", b, 32)
        nodes.append(Node(_cstr(b[:32]), sib, child, par, fl))
    anims = []
    for b in t.block(o + ANTR_ANIM_AT, ANTR_ANIM_REC):
        typ, fc, fs, fit = struct.unpack_from("<hhhh", b, 32)
        checksum, = struct.unpack_from("<I", b, 0x28)
        nn, loop = struct.unpack_from("<hh", b, 0x2C)
        nxt, flags = struct.unpack_from("<hH", b, 0x38)
        tf = struct.unpack_from("<2I", b, 0x5C)
        rf = struct.unpack_from("<2I", b, 0x6C)
        sf = struct.unpack_from("<2I", b, 0x7C)

        def data(at: int) -> bytes:
            size, _fl, _fo, ptr, _id = struct.unpack_from("<IIIII", b, at)
            return t.blob(ptr, size)

        anims.append(Anim(_cstr(b[:32]), typ, fc, fs, fit, checksum, nn, loop, nxt, flags,
                          tf[0] | tf[1] << 32, rf[0] | rf[1] << 32, sf[0] | sf[1] << 32,
                          data(0x48), data(0x8C), data(0xA0)))
    return Graph(e.tag_id, e.path, nodes, anims)


def read_model_rest(t: MapTags, e: tagindex.TagEntry) -> ModelRest:
    o = t.off(e.data_address)
    checksum = t.u32(o + 4)
    names, rot, trans = [], [], []
    for b in t.block(o + MODEL_NODES_AT, MODEL_NODE_REC):
        names.append(_cstr(b[:32]))
        trans.append(struct.unpack_from("<3f", b, 40))
        rot.append(struct.unpack_from("<4f", b, 52))
    return ModelRest(e.tag_id, e.path, checksum, names, rot, trans)


# ---- decoding to per-frame values -------------------------------------------------------------
def norm4(q) -> tuple:
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q) if n > 1e-12 else (0.0, 0.0, 0.0, 1.0)


def nlerp(a, b, t: float) -> tuple:
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
    if d < 0:
        b = (-b[0], -b[1], -b[2], -b[3])
    return norm4((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                  a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t))


def hamilton(q) -> tuple:
    """Halo stores the CONJUGATE of the Hamilton quaternion (docs/11-model-section.md; verified for animations by
    posing a skeleton with it: the std reading turns the bind pose into nonsense, the conjugate stands up).
    Everything this converter writes is in the engine's (Hamilton) convention, like the MODL nodes."""
    return (-q[0], -q[1], -q[2], q[3])


def lerp3(a, b, t: float) -> tuple:
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def lerp1(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def qangle(a, b) -> float:
    """Rotation angle between two unit quaternions, degrees."""
    na = a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]
    nb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]
    d = min(1.0, abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / math.sqrt(na * nb))
    return math.degrees(2.0 * math.acos(d))


def sext(v: int, bits: int) -> int:
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def unpack_q48(n: int) -> tuple:
    """The Xbox compressed quaternion (see the module docstring) -> (x, y, z, w) as ints in -2048..2047."""
    def b(lo: int, w: int) -> int:
        return (n >> lo) & ((1 << w) - 1)
    x = sext(b(8, 8) << 4 | b(4, 4), 12)
    y = sext(b(0, 4) << 8 | b(24, 8), 12)
    z = sext(b(16, 8) << 4 | b(44, 4), 12)
    w = sext(b(36, 8) << 4 | b(32, 4), 12)
    return (x, y, z, w)


def pack_q48(q) -> int:
    """Inverse of unpack_q48 (used by the tests to build compressed animations)."""
    x, y, z, w = (max(-2048, min(2047, round(c * 2047))) & 0xFFF for c in q)
    return ((x >> 4) << 8 | (x & 15) << 4 | (y >> 8) | (y & 0xFF) << 24 | (z >> 4) << 16 |
            (z & 15) << 44 | (w >> 4) << 36 | (w & 15) << 32)


@dataclass
class Decoded:
    """One value list per node and channel: length 1 = static, length frame_count = animated."""
    nn: int
    fc: int
    rot: list = field(default_factory=list)
    trans: list = field(default_factory=list)
    scale: list = field(default_factory=list)
    frame_info: list = field(default_factory=list)
    driven_rot: frozenset = frozenset()      # nodes whose rotation / translation channel the tag flags as animated (what an overlay animation drives)
    driven_trans: frozenset = frozenset()


def _bits(mask: int, nn: int) -> list[int]:
    return [n for n in range(nn) if mask >> n & 1]


def _decode_frame_info(a: Anim, d: Decoded) -> None:
    k = (0, 2, 3, 4)[a.frame_info_type] if 0 <= a.frame_info_type <= 3 else None
    if k is None:
        raise ConvertError(f"{a.name}: unknown frame info type {a.frame_info_type}")
    if k:
        if len(a.frame_info) != a.frame_count * 4 * k:
            raise ConvertError(f"{a.name}: frame info is {len(a.frame_info)} bytes, expected {a.frame_count * 4 * k}")
        d.frame_info = [struct.unpack_from("<%df" % k, a.frame_info, f * 4 * k) for f in range(a.frame_count)]


def decode(a: Anim) -> Decoded:
    """Decode one animation's tracks to per-frame values (rotations normalised)."""
    nn, fc = a.node_count, a.frame_count
    if not 1 <= nn <= MAX_NODES:
        raise ConvertError(f"{a.name}: node count {nn} outside 1..{MAX_NODES}")
    if fc < 1:
        raise ConvertError(f"{a.name}: frame count {fc}")
    d = Decoded(nn, fc, [None] * nn, [None] * nn, [None] * nn)
    rotn, trn, scn = _bits(a.rot_flags, nn), _bits(a.trans_flags, nn), _bits(a.scale_flags, nn)
    d.driven_rot, d.driven_trans = frozenset(rotn), frozenset(trn)
    if a.flags & F_COMPRESSED:
        return _decode_compressed(a, d, rotn, trn, scn)
    rs, ts, ss = set(rotn), set(trn), set(scn)
    dd, fd = a.default_data, a.frame_data
    want = 8 * (nn - len(rs)) + 12 * (nn - len(ts)) + 4 * (nn - len(ss))
    if len(dd) != want or len(fd) != fc * a.frame_size or a.frame_size != 8 * len(rs) + 12 * len(ts) + 4 * len(ss):
        raise ConvertError(f"{a.name}: data sizes (default {len(dd)}/{want}, frames {len(fd)}, frame size "
                           f"{a.frame_size}) do not match the channel flags")
    p = 0
    for n in range(nn):
        if n not in rs:
            q = struct.unpack_from("<4h", dd, p)
            p += 8
            d.rot[n] = [hamilton(norm4(tuple(c / 32767.0 for c in q)))]
        if n not in ts:
            d.trans[n] = [struct.unpack_from("<3f", dd, p)]
            p += 12
        if n not in ss:
            d.scale[n] = [struct.unpack_from("<f", dd, p)[0]]
            p += 4
    for n in range(nn):
        if n in rs:
            d.rot[n] = []
        if n in ts:
            d.trans[n] = []
        if n in ss:
            d.scale[n] = []
    for f in range(fc):
        p = f * a.frame_size
        for n in range(nn):
            if n in rs:
                q = struct.unpack_from("<4h", fd, p)
                p += 8
                d.rot[n].append(hamilton(norm4(tuple(c / 32767.0 for c in q))))
            if n in ts:
                d.trans[n].append(struct.unpack_from("<3f", fd, p))
                p += 12
            if n in ss:
                d.scale[n].append(struct.unpack_from("<f", fd, p)[0])
                p += 4
    _decode_frame_info(a, d)
    return d


def _expand_keys(blend, fc: int, first, keys: list[tuple[int, object]]) -> list:
    out = [first]
    prev_f, prev_v = 0, first
    for f, v in keys:
        for g in range(prev_f + 1, f + 1):
            out.append(blend(prev_v, v, (g - prev_f) / (f - prev_f)))
        prev_f, prev_v = f, v
    while len(out) < fc:
        out.append(prev_v)
    return out


def _decode_compressed(a: Anim, d: Decoded, rotn: list[int], trn: list[int], scn: list[int]) -> Decoded:
    nn, fc = a.node_count, a.frame_count
    fd = a.frame_data
    where = f"{a.name} (compressed)"
    if scn or a.default_data:
        raise ConvertError(f"{where}: scale channels / default data in compressed data are not supported")
    if len(fd) < 44:
        raise ConvertError(f"{where}: frame data shorter than its header")
    h = struct.unpack_from("<11I", fd, 0)

    def table(off: int, n: int) -> list[tuple[int, int]]:
        if off + 4 * n > len(fd):
            raise ConvertError(f"{where}: channel table outside the data")
        return [(v >> 12, v & 0xFFF) for v in struct.unpack_from("<%dI" % n, fd, off)]

    def indices(off: int, total: int) -> tuple:
        if off + 2 * total > len(fd):
            raise ConvertError(f"{where}: key index list outside the data")
        return struct.unpack_from("<%dH" % total, fd, off)

    rt = table(44, len(rotn))
    rtot = sum(c for _, c in rt)
    if h[0] != 44 + 4 * len(rotn) or h[1] != h[0] + 2 * rtot or h[2] != h[1] + 6 * nn or h[3] != h[2] + 6 * rtot:
        raise ConvertError(f"{where}: rotation section offsets are inconsistent")
    tt = table(h[3], len(trn))
    ttot = sum(c for _, c in tt)
    if h[4] != h[3] + 4 * len(trn) or h[5] != h[4] + 2 * ttot or h[6] != h[5] + 12 * nn or h[7] != h[6] + 12 * ttot:
        raise ConvertError(f"{where}: translation section offsets are inconsistent")
    if h[7] != len(fd) or not h[7] == h[8] == h[9] == h[10]:
        raise ConvertError(f"{where}: data size {len(fd)} does not end at the last section")
    ridx, tidx = indices(h[0], rtot), indices(h[4], ttot)

    def channel_keys(tab, idx, ci: int) -> list[int]:
        s, c = tab[ci]
        ks = list(idx[s:s + c])
        if c == 0:          # flagged animated but no keys: the channel stays at its frame-0 value
            return ks
        if ks != sorted(set(ks)) or ks[0] < 1 or ks[-1] != fc - 1:
            raise ConvertError(f"{where}: key frames of channel {ci} are not ascending in 1..{fc - 1}")
        return ks

    for n in range(nn):
        q48 = int.from_bytes(fd[h[1] + 6 * n:h[1] + 6 * n + 6], "little")
        d.rot[n] = [hamilton(norm4(unpack_q48(q48)))]
        d.trans[n] = [struct.unpack_from("<3f", fd, h[5] + 12 * n)]
        d.scale[n] = [1.0]
    for ci, n in enumerate(rotn):
        ks = channel_keys(rt, ridx, ci)
        s = rt[ci][0]
        if not ks:
            continue
        keys = [(f, hamilton(norm4(unpack_q48(int.from_bytes(fd[h[2] + 6 * (s + j):h[2] + 6 * (s + j) + 6], "little")))))
                for j, f in enumerate(ks)]
        d.rot[n] = _expand_keys(nlerp, fc, d.rot[n][0], keys)
    for ci, n in enumerate(trn):
        ks = channel_keys(tt, tidx, ci)
        s = tt[ci][0]
        if not ks:
            continue
        keys = [(f, struct.unpack_from("<3f", fd, h[6] + 12 * (s + j))) for j, f in enumerate(ks)]
        d.trans[n] = _expand_keys(lerp3, fc, d.trans[n][0], keys)
    _decode_frame_info(a, d)
    return d


# ---- quantisers ---------------------------------------------------------------------------------
def pack_q16(q) -> tuple[int, int, int]:
    """Smallest-three, 15-bit: the three s16 words a, b, c of a 6-byte rotation key."""
    i = max(range(4), key=lambda k: abs(q[k]))
    s = 1.0 if q[i] >= 0 else -1.0
    v = [max(-Q15, min(Q15, round(q[k] * s * Q15 / SQRT_HALF))) for k in range(4) if k != i]
    return v[0] * 2 + (i & 1), v[1] * 2 + (i >> 1), v[2] * 2


def unpack_q16(a: int, b: int, c: int) -> tuple:
    i = (a & 1) | (b & 1) << 1
    x, y, z = ((a - (a & 1)) // 2, (b - (b & 1)) // 2, c // 2)
    v = [e * SQRT_HALF / Q15 for e in (x, y, z)]
    w = math.sqrt(max(0.0, 1.0 - v[0] * v[0] - v[1] * v[1] - v[2] * v[2]))
    out, it = [0.0] * 4, iter(v)
    for k in range(4):
        out[k] = w if k == i else next(it)
    return tuple(out)


def f32(v: float) -> float:
    return struct.unpack("<f", struct.pack("<f", v))[0]


def pack_qs16(q) -> tuple[int, int, int, int]:
    """Static rotation: x y z w as s16 / 32767."""
    q = norm4(q)
    return tuple(max(-32767, min(32767, round(c * 32767))) for c in q)


def unpack_qs16(v) -> tuple:
    return norm4(tuple(c / 32767.0 for c in v))


def planar_axis(vals: list[tuple]) -> int:
    """Index (0..2) of the axis a rotation track turns about when it turns about one axis of the node
    frame only (hinge joints: x = y = 0 ...), else -1."""
    for ax in range(3):
        if all(abs(q[k]) <= AXIS_EPS for q in vals for k in range(3) if k != ax):
            return ax
    return -1


def pack_planar(q, ax: int) -> tuple[int, int]:
    c, w = q[ax], q[3]
    if w < 0:
        c, w = -c, -w
    n = math.hypot(c, w) or 1.0
    return round(c / n * 32767), round(w / n * 32767)


def unpack_planar(c: int, w: int, ax: int) -> tuple:
    q = [0.0, 0.0, 0.0, w / 32767.0]
    q[ax] = c / 32767.0
    return tuple(q)


# ---- key reduction --------------------------------------------------------------------------------
def reduce_keys(src: list, quant: list, err, blend, tol: float) -> list[int]:
    """Closed-loop key selection (recursive split at the worst frame): the returned ascending frame
    list always holds 0 and the last frame, and linearly interpolating the QUANTISED key values
    (`quant`) reproduces every source frame within `tol` (`err` measures the distance)."""
    last = len(src) - 1
    keys = {0, last}
    stack = [(0, last)]
    while stack:
        i, j = stack.pop()
        if j - i < 2:
            continue
        worst, wf = -1.0, -1
        span = j - i
        for f in range(i + 1, j):
            e = err(blend(quant[i], quant[j], (f - i) / span), src[f])
            if e > worst:
                worst, wf = e, f
        if worst > tol:
            keys.add(wf)
            stack.append((i, wf))
            stack.append((wf, j))
    return sorted(keys)


def _pad4(n: int) -> int:
    return (n + 3) & ~3


def _u8idx(fc: int) -> bool:
    return fc <= 256


def _track_header(k: int, flags: int) -> bytes:
    return struct.pack("<HH", k, flags)


def _index_bytes(keys: list[int], idx8: bool) -> bytes:
    raw = bytes(keys) if idx8 else struct.pack("<%dH" % len(keys), *keys)
    return raw + bytes(_pad4(len(raw)) - len(raw))


def encode_rot_track(vals: list[tuple], fc: int) -> tuple[bytes, float] | None:
    """Animated rotation track -> (bytes, max error in degrees vs the source frames), or None when the
    channel is static within ROT_STATIC_DEG. Tracks that turn about one axis only (hinges) use 4-byte keys."""
    if max(qangle(vals[0], q) for q in vals) <= ROT_STATIC_DEG:
        return None
    ax = planar_axis(vals)
    if ax >= 0:
        packed = [pack_planar(q, ax) for q in vals]
        quant = [unpack_planar(c, w, ax) for c, w in packed]
        per, fmt, flags0 = 4, "<2h", T_AXIS | ax << 4
    else:
        packed = [pack_q16(q) for q in vals]
        quant = [unpack_q16(*p) for p in packed]
        per, fmt, flags0 = 6, "<3h", 0
    keys = reduce_keys(vals, quant, qangle, nlerp, ROT_TOL_DEG)
    idx8 = _u8idx(fc)
    keyed = 4 + _pad4(len(keys) * (1 if idx8 else 2)) + _pad4(per * len(keys))
    dense = 4 + _pad4(per * fc)
    if len(keys) == fc or dense <= keyed:
        keys, flags, index = list(range(fc)), T_DENSE | flags0, b""
    else:
        flags, index = (T_IDX8 if idx8 else 0) | flags0, _index_bytes(keys, idx8)
    body = b"".join(struct.pack(fmt, *packed[f]) for f in keys)
    blob = _track_header(len(keys), flags) + index + body + bytes(_pad4(len(body)) - len(body))
    return blob, max(_interp_err_rot(vals, quant, keys), 0.0)


def _interp_err_rot(src, quant, keys) -> float:
    worst = 0.0
    for a, b in zip(keys, keys[1:]):
        for f in range(a, b + 1):
            worst = max(worst, qangle(nlerp(quant[a], quant[b], (f - a) / (b - a)), src[f]))
    return worst


def _dist3(a, b) -> float:
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


def encode_trans_track(vals: list[tuple], fc: int) -> tuple[bytes, float] | None:
    lo = [min(v[k] for v in vals) for k in range(3)]
    hi = [max(v[k] for v in vals) for k in range(3)]
    if max(_dist3(vals[0], v) for v in vals) <= TRANS_STATIC:
        return None
    base = [f32(c) for c in lo]
    step = [f32((hi[k] - lo[k]) / 65535.0) for k in range(3)]
    # float32 quantisation of the range could clip a value by one step at most: widen the step slightly
    step = [f32(s * (1 + 1e-6)) if s > 0 else 0.0 for s in step]
    q16 = [tuple(min(65535, max(0, round((v[k] - base[k]) / step[k]))) if step[k] > 0 else 0 for k in range(3))
           for v in vals]
    quant = [tuple(base[k] + q[k] * step[k] for k in range(3)) for q in q16]
    use_f32 = max(_dist3(v, q) for v, q in zip(vals, quant)) > TRANS_TOL * 0.4
    if use_f32:
        quant = [tuple(f32(c) for c in v) for v in vals]
    keys = reduce_keys(vals, quant, _dist3, lerp3, TRANS_TOL)
    idx8 = _u8idx(fc)
    per = 12 if use_f32 else 6
    head = 0 if use_f32 else 24
    keyed = 4 + head + _pad4(len(keys) * (1 if idx8 else 2)) + _pad4(per * len(keys))
    dense = 4 + head + _pad4(per * fc)
    if len(keys) == fc or dense <= keyed:
        keys, flags, index = list(range(fc)), T_DENSE, b""
    else:
        flags, index = (T_IDX8 if idx8 else 0), _index_bytes(keys, idx8)
    if use_f32:
        flags |= T_F32
        body = b"".join(struct.pack("<3f", *vals[f]) for f in keys)
        hdr = _track_header(len(keys), flags)
    else:
        body = b"".join(struct.pack("<3H", *q16[f]) for f in keys)
        hdr = _track_header(len(keys), flags) + struct.pack("<3f3f", *base, *step)
    blob = hdr + index + body + bytes(_pad4(len(body)) - len(body))
    worst = 0.0
    for a, b in zip(keys, keys[1:]):
        for f in range(a, b + 1):
            worst = max(worst, _dist3(lerp3(quant[a], quant[b], (f - a) / (b - a)), vals[f]))
    return blob, worst


def encode_scale_track(vals: list[float], fc: int) -> tuple[bytes, float] | None:
    if max(abs(v - vals[0]) for v in vals) <= SCALE_STATIC:
        return None
    quant = [f32(v) for v in vals]
    keys = reduce_keys(vals, quant, lambda a, b: abs(a - b), lerp1, SCALE_TOL)
    idx8 = _u8idx(fc)
    keyed = 4 + _pad4(len(keys) * (1 if idx8 else 2)) + 4 * len(keys)
    dense = 4 + 4 * fc
    if len(keys) == fc or dense <= keyed:
        keys, flags, index = list(range(fc)), T_DENSE, b""
    else:
        flags, index = (T_IDX8 if idx8 else 0), _index_bytes(keys, idx8)
    blob = _track_header(len(keys), flags) + index + struct.pack("<%df" % len(keys), *(vals[f] for f in keys))
    worst = max((abs(lerp1(quant[a], quant[b], (f - a) / (b - a)) - vals[f])
                 for a, b in zip(keys, keys[1:]) for f in range(a, b + 1)), default=0.0)
    return blob, worst


# ---- animation blobs ---------------------------------------------------------------------------------
@dataclass
class RestPose:
    rot: list[tuple]
    trans: list[tuple]
    scale: list[float]


@dataclass
class ErrStats:
    rot_max: float = 0.0       # degrees
    trans_max: float = 0.0     # millimetres
    scale_max: float = 0.0
    rot_sum2: float = 0.0
    rot_n: int = 0
    trans_sum2: float = 0.0
    trans_n: int = 0

    def add(self, other: "ErrStats") -> None:
        self.rot_max = max(self.rot_max, other.rot_max)
        self.trans_max = max(self.trans_max, other.trans_max)
        self.scale_max = max(self.scale_max, other.scale_max)
        self.rot_sum2 += other.rot_sum2
        self.rot_n += other.rot_n
        self.trans_sum2 += other.trans_sum2
        self.trans_n += other.trans_n


FACE_OVERLAYS = ("stand emotions", "stand talk")


def is_face_overlay(a: Anim) -> bool:
    """The two overlays a talking unit's face plays (`unit_set_emotion` picks a frame of the first, the sound's mouth data scales the second)."""
    return a.type == 1 and a.name.strip().lower() in FACE_OVERLAYS


def encode_animation(d: Decoded, rest: RestPose, overlay: bool = False) -> tuple[bytes, ErrStats]:
    """The data blob of one animation (see the module docstring). Static values equal to the graph's rest
    pose become mode 0; the error numbers are the keyed tracks' maxima against the source frames (the
    complete figures come from measure_errors).

    `overlay` (an animation of type 1): the tag stores a full pose, but only the channels its flags mark as animated are
    the overlay's (a face overlay moves the eyelids and the jaw, its other channels are the authoring pose). Those channels
    are written whatever their value (a single frame is a static channel, never the rest shortcut) and every other channel
    is mode 0 ("not driven"); anim_overlay_modes() in the engine reads the modes back as the mask."""
    nn, fc = d.nn, d.fc
    modes = bytearray(nn)
    parts: list[bytes] = []
    st = ErrStats()
    for n in range(nn):
        # rotation
        r = d.rot[n]
        tr = encode_rot_track(r, fc) if len(r) > 1 else None
        if overlay and n not in d.driven_rot:
            pass
        elif len(r) > 1 and tr is not None:
            modes[n] |= M_ANIMATED
            parts.append(tr[0])
            st.rot_max = max(st.rot_max, tr[1])
        else:
            q = r[0]
            if not overlay and n < len(rest.rot) and qangle(q, rest.rot[n]) <= ROT_STATIC_DEG:
                pass
            else:
                modes[n] |= M_STATIC
                parts.append(struct.pack("<4h", *pack_qs16(q)))
        # translation
        t = d.trans[n]
        tt = encode_trans_track(t, fc) if len(t) > 1 else None
        if overlay and n not in d.driven_trans:
            pass
        elif len(t) > 1 and tt is not None:
            modes[n] |= M_ANIMATED << 2
            parts.append(tt[0])
            st.trans_max = max(st.trans_max, tt[1] * WU_MM)
        else:
            v = t[0]
            if not overlay and n < len(rest.trans) and _dist3(v, rest.trans[n]) <= TRANS_STATIC:
                pass
            else:
                modes[n] |= M_STATIC << 2
                parts.append(struct.pack("<3f", *v))
        # scale
        s = d.scale[n]
        ts = encode_scale_track(s, fc) if len(s) > 1 else None
        if overlay:
            pass
        elif len(s) > 1 and ts is not None:
            modes[n] |= M_ANIMATED << 4
            parts.append(ts[0])
            st.scale_max = max(st.scale_max, ts[1])
        else:
            v = s[0]
            if n < len(rest.scale) and abs(v - rest.scale[n]) <= SCALE_STATIC:
                pass
            else:
                modes[n] |= M_STATIC << 4
                parts.append(struct.pack("<f", v))
    head = bytes(modes) + bytes(_pad4(nn) - nn)
    blob = head + b"".join(parts)
    assert len(blob) % 4 == 0
    return blob, st


def encode_root_motion(d: Decoded, fit: int) -> tuple[int, bytes]:
    """(type, block): type 0 when there is no frame info or all of it is zero."""
    if not d.frame_info or all(c == 0.0 for fi in d.frame_info for c in fi):
        return 0, b""
    k = len(d.frame_info[0])
    scale = [max(abs(fi[c]) for fi in d.frame_info) for c in range(k)]
    scale = [f32(s * (1 + 1e-6)) for s in scale] + [0.0] * (4 - k)
    vals = []
    for fi in d.frame_info:
        for c in range(k):
            vals.append(round(fi[c] / scale[c] * 32767.0) if scale[c] > 0 else 0)
    body = struct.pack("<%dh" % len(vals), *vals)
    return fit, struct.pack("<4f", *scale) + body + bytes(_pad4(len(body)) - len(body))


def loop_flags(a: Anim) -> int:
    base = a.name.split("%", 1)[0].strip()
    loops = a.type == 0 and any(base.endswith(w) for w in LOOP_ACTIONS)
    return (ANIM_LOOPS if loops else 0) | (ANIM_WORLD_RELATIVE if a.flags & F_WORLD_RELATIVE else 0)


# ---- the section -----------------------------------------------------------------------------------------
@dataclass
class GraphSource:
    """What the section writer needs about one graph (tests build these without a map)."""
    tag_id: int
    path: str
    node_count: int
    anims: list[Anim]
    rest: RestPose
    model_tag: int = 0
    problems: list[str] = field(default_factory=list)
    node_names: list[str] = field(default_factory=list)     # graph order; missing names are written empty


@dataclass
class BuildStats:
    graph_bytes: dict = field(default_factory=dict)     # tag id -> pool bytes of its unique blobs
    errors: ErrStats = field(default_factory=ErrStats)
    source_bytes: int = 0
    dedup_saved: int = 0


class _Pool:
    def __init__(self) -> None:
        self.buf = bytearray(POOL_ALIGN)           # offset 0 is reserved: 0 means "none"
        self.seen: dict[bytes, int] = {}

    def add(self, blob: bytes, dedupe: bool = True) -> tuple[int, bool]:
        if dedupe and blob in self.seen:
            return self.seen[blob], True
        while len(self.buf) % POOL_ALIGN:
            self.buf.append(0)
        off = len(self.buf)
        self.buf += blob
        if dedupe:
            self.seen[blob] = off
        return off, False


def rest_from_model(m: ModelRest | None, node_count: int) -> RestPose | None:
    if m is None:
        return None
    rot = [hamilton(norm4(r)) for r in m.rot[:node_count]]
    trans = [tuple(t) for t in m.trans[:node_count]]
    while len(rot) < node_count:
        rot.append((0.0, 0.0, 0.0, 1.0))
        trans.append((0.0, 0.0, 0.0))
    return RestPose(rot, trans, [1.0] * node_count)


def rest_from_anims(anims: list[Anim], node_count: int) -> RestPose:
    """No model: the first animation's frame 0 (base animations first)."""
    rot = [(0.0, 0.0, 0.0, 1.0)] * node_count
    trans = [(0.0, 0.0, 0.0)] * node_count
    scale = [1.0] * node_count
    order = sorted(range(len(anims)), key=lambda i: (anims[i].type != 0, i))
    if order:
        d = decode(anims[order[0]])
        for n in range(d.nn):
            rot[n], trans[n], scale[n] = d.rot[n][0], tuple(d.trans[n][0]), d.scale[n][0]
    return RestPose(rot, trans, scale)


def build_section_from_graphs(graphs: list[GraphSource], stats: BuildStats | None = None,
                              root_motion: bool = True,
                              animations_needed: dict[int, "Iterable[int]"] | None = None) -> bytes:
    """Serialise graphs (sorted by tag id here) into an ANIM section.

    `animations_needed` maps a graph tag id to the animation indices that are really used. Every other animation of
    such a graph is written as a STUB: same name, type, frame count and loop flags (so indices, names and timing stay
    identical to a full conversion and ASTB's indices stay valid), but no pose data (all channels use the graph's rest
    pose, ~nodes bytes) and no root motion, plus the flag `ANIM_STUB` (4; anim.c ignores it). Graphs that are not in the
    dict are converted in full, as is everything when it is None (workstream E, ansmconv.needed_animations)."""
    graphs = sorted(graphs, key=lambda g: g.tag_id)
    if len({g.tag_id for g in graphs}) != len(graphs):
        raise ConvertError("duplicate graph tag id")
    stats = stats if stats is not None else BuildStats()
    pool = _Pool()
    graph_recs, anim_recs = [], []
    for g in graphs:
        if not 1 <= g.node_count <= MAX_NODES or len(g.anims) > 0xFFFF:
            raise ConvertError(f"{g.path}: {g.node_count} nodes, {len(g.anims)} animations")
        default = b"".join(struct.pack("<4f3ff", *g.rest.rot[n], *g.rest.trans[n], g.rest.scale[n])
                           for n in range(g.node_count))
        default_off, _ = pool.add(default)
        names = b"".join(_node_name_bytes(g.node_names[n] if n < len(g.node_names) else "", g.path) for n in range(g.node_count))
        names_off, _ = pool.add(names)
        graph_recs.append(struct.pack("<IHHII I I 8x", g.tag_id, g.node_count, len(g.anims), len(anim_recs),
                                      default_off, g.model_tag, names_off))
        gbytes = len(default)
        needed = None if animations_needed is None or g.tag_id not in animations_needed \
            else {int(i) for i in animations_needed[g.tag_id]}
        if needed is not None and any(not 0 <= i < len(g.anims) for i in needed):
            raise ConvertError(f"{g.path}: animations_needed holds an index outside 0..{len(g.anims) - 1}")
        for ai, a in enumerate(g.anims):
            stub = needed is not None and ai not in needed
            if stub:
                if not 1 <= a.node_count <= MAX_NODES or a.frame_count < 1:
                    raise ConvertError(f"{a.name}: node count {a.node_count}, frame count {a.frame_count}")
                d = Decoded(a.node_count, a.frame_count)
                blob, est = bytes(_pad4(a.node_count)), ErrStats()      # every channel mode 0: the graph's rest pose
            else:
                d = decode(a)
                blob, est = encode_animation(d, g.rest, is_face_overlay(a))
            if d.nn > g.node_count:
                raise ConvertError(f"{g.path}: animation {a.name!r} has {d.nn} nodes, graph {g.node_count}")
            stats.errors.add(est)
            data_off, shared = pool.add(blob)
            rtype, rblock = encode_root_motion(d, a.frame_info_type) if root_motion and not stub else (0, b"")
            root_off = pool.add(rblock)[0] if rblock else 0
            if not shared:
                gbytes += len(blob)
            else:
                stats.dedup_saved += len(blob)
            gbytes += len(rblock)
            stats.source_bytes += len(a.frame_data) + len(a.default_data) + len(a.frame_info)
            name = a.name.encode("latin1")
            if len(name) >= NAME_LEN:
                raise ConvertError(f"{g.path}: animation name {a.name!r} too long")
            loop_frame = a.loop_frame if 0 <= a.loop_frame < a.frame_count else 0
            anim_recs.append(struct.pack(ANIM_FMT, name, a.type, a.frame_count, loop_frame,
                                         loop_flags(a) | (ANIM_STUB if stub else 0), d.nn,
                                         rtype, g.node_count, 25.0 if a.flags & F_PAL else 30.0, data_off,
                                         len(blob), root_off, default_off))
        stats.graph_bytes[g.tag_id] = gbytes
    graphs_off = HEADER_SIZE
    anims_off = graphs_off + GRAPH_REC * len(graph_recs)
    pool_off = _pad16(anims_off + ANIM_REC * len(anim_recs))
    body = bytearray(b"".join(graph_recs)) + b"".join(anim_recs)
    body += bytes(pool_off - HEADER_SIZE - len(body))
    total = pool_off + len(pool.buf)
    pool.buf += bytes(_pad16(total) - total)
    total = pool_off + len(pool.buf)
    head = struct.pack("<4sHHIIIIII32x", MAGIC, VERSION, 0, len(graph_recs), len(anim_recs), graphs_off,
                       anims_off, pool_off, total)
    out = head + bytes(body) + bytes(pool.buf)
    assert len(out) == total and len(head) == HEADER_SIZE
    return out


def _pad16(n: int) -> int:
    return (n + 15) & ~15


def _node_name_bytes(name: str, path: str) -> bytes:
    raw = name.encode("latin1", "replace")
    if len(raw) >= NODE_NAME_LEN:
        raise ConvertError(f"{path}: node name {name!r} too long")
    return raw.ljust(NODE_NAME_LEN, b"\0")


# ---- map -> graphs -----------------------------------------------------------------------------------------
def collect_models(t: MapTags) -> dict[int, ModelRest]:
    return {e.tag_id: read_model_rest(t, e) for e in t.idx.entries if e.group == "mode"}


def graph_users(t: MapTags, models: dict[int, ModelRest]) -> dict[int, list[int]]:
    """antr tag id -> model tag ids of the object tags that reference it (sorted, unique)."""
    users: dict[int, set[int]] = defaultdict(set)
    for e in t.idx.entries:
        if e.group in OBJECT_GROUPS:
            o = t.off(e.data_address)
            mt, at = t.u32(o + OBJ_MODEL_AT + 12), t.u32(o + OBJ_ANIM_AT + 12)
            if at in t.byid and t.byid[at].group == "antr" and mt in models:
                users[at].add(mt)
    return {k: sorted(v) for k, v in users.items()}


def graph_source(g: Graph, models: dict[int, ModelRest], users: dict[int, list[int]]) -> GraphSource:
    """Pick the rest pose model (a model whose node checksum equals the animations', preferring those
    objects actually pair with the graph), check names/counts, and set the node count."""
    problems = []
    nn = max([a.node_count for a in g.anims] + [len(g.nodes)] + [1])
    checks = {a.checksum for a in g.anims}
    if len(checks) > 1:
        problems.append(f"animations carry {len(checks)} different node list checksums")
    by_chk = sorted((m for m in models.values() if m.checksum in checks), key=lambda m: m.tag_id)
    used = [models[u] for u in users.get(g.tag_id, [])]
    for m in used:
        if m.checksum not in checks:
            problems.append(f"model {m.path} (used with this graph) has a different node list checksum")
        elif len(m.names) != nn:
            problems.append(f"model {m.path}: {len(m.names)} nodes, graph {nn}")
        elif g.nodes and [n.name for n in g.nodes] != m.names:
            problems.append(f"model {m.path}: node names differ from the graph's")
    if g.nodes and len(g.nodes) != nn:
        problems.append(f"node block has {len(g.nodes)} nodes, animations {nn}")
    pick = next((m for m in used if m.checksum in checks), None) or (by_chk[0] if by_chk else None)
    rest = rest_from_model(pick, nn) if pick and len(pick.names) == nn else None
    if rest is None:
        if not pick:
            problems.append("no model with this node list checksum: rest pose derived from the animations")
        rest = rest_from_anims(g.anims, nn)
        pick = None
    return GraphSource(g.tag_id, g.path, nn, g.anims, rest, pick.tag_id if pick else 0, problems, [n.name for n in g.nodes])


def load_graphs(src, anim_tags: list[int] | None = None) -> list[GraphSource]:
    """Graph sources of a map (a path or an opened MapTags)."""
    t = src if isinstance(src, MapTags) else MapTags(Path(src))
    models = collect_models(t)
    users = graph_users(t, models)
    antr = [e for e in t.idx.entries if e.group == "antr"]
    if anim_tags is not None:
        want = {int(x) for x in anim_tags}
        have = {e.tag_id for e in antr}
        if want - have:
            raise ConvertError("not model_animations tags of this map: " + ", ".join(f"{x:#010x}" for x in sorted(want - have)))
        antr = [e for e in antr if e.tag_id in want]
    return [graph_source(read_graph(t, e), models, users) for e in antr]


def build_section(map_path: Path, anim_tags: list[int] | None = None, stats: BuildStats | None = None,
                  root_motion: bool = True, animations_needed: dict[int, "Iterable[int]"] | None = None) -> bytes:
    """The ANIM section of a decompressed map: every `model_animations` tag (anim_tags None) or only the
    tags with these ids, sorted by tag id. `animations_needed` (graph tag id -> animation indices) turns the other
    animations of those graphs into pose-less stubs, see build_section_from_graphs."""
    return build_section_from_graphs(load_graphs(Path(map_path), anim_tags), stats, root_motion, animations_needed)


# ---- reading the section back (reference reader and sampler; engine/src/world/anim.c mirrors it) -------------
@dataclass
class SGraph:
    tag_id: int
    node_count: int
    anim_count: int
    first_anim: int
    default_off: int
    model_tag: int
    names_off: int = 0


@dataclass
class SAnim:
    name: str
    type: int
    frame_count: int
    loop_frame: int
    flags: int
    node_count: int
    root_type: int
    graph_nodes: int
    fps: float
    data_off: int
    data_bytes: int
    root_off: int
    default_off: int

    @property
    def root_bytes(self) -> int:
        return 16 + _pad4(2 * (self.root_type + 1) * self.frame_count) if self.root_type else 0


@dataclass
class Section:
    data: bytes
    graphs: list[SGraph]
    anims: list[SAnim]
    pool_off: int
    _cache: dict = field(default_factory=dict, repr=False)

    def graph_index(self, tag_id: int) -> int:
        return next((i for i, g in enumerate(self.graphs) if g.tag_id == tag_id), -1)

    def graph_anims(self, gi: int) -> list[SAnim]:
        g = self.graphs[gi]
        return self.anims[g.first_anim:g.first_anim + g.anim_count]

    def pool(self, off: int, size: int) -> bytes:
        return self.data[self.pool_off + off:self.pool_off + off + size]

    def rest_pose(self, gi: int) -> RestPose:
        g = self.graphs[gi]
        raw = self.pool(g.default_off, 32 * g.node_count)
        v = [struct.unpack_from("<8f", raw, 32 * n) for n in range(g.node_count)]
        return RestPose([x[0:4] for x in v], [x[4:7] for x in v], [x[7] for x in v])

    def node_names(self, gi: int) -> list[str]:
        g = self.graphs[gi]
        raw = self.pool(g.names_off, NODE_NAME_LEN * g.node_count)
        return [_cstr(raw[NODE_NAME_LEN * n:NODE_NAME_LEN * (n + 1)]) for n in range(g.node_count)]

    def blob(self, a: SAnim) -> bytes:
        return self.pool(a.data_off, a.data_bytes)

    def root_motion(self, a: SAnim, frame: int) -> tuple:
        k = (0, 2, 3, 4)[a.root_type]
        if not k or not a.root_off:
            return (0.0, 0.0, 0.0, 0.0)
        frame = max(0, min(a.frame_count - 1, frame))
        raw = self.pool(a.root_off, 16 + 2 * k * a.frame_count)
        scale = struct.unpack_from("<4f", raw, 0)
        vals = struct.unpack_from("<%dh" % k, raw, 16 + 2 * k * frame)
        out = [vals[c] / 32767.0 * scale[c] for c in range(k)]
        return tuple(out + [0.0] * (4 - k))

    def tracks(self, gi: int, ai: int) -> list:
        """Parsed per node [rot, trans, scale]; each channel is ('static', value) (also for mode 0, the
        graph default) or ('track', dense, keys list or None, values list). Raises ConvertError on a
        malformed blob."""
        if (gi, ai) not in self._cache:
            self._cache[(gi, ai)] = self._parse_tracks(gi, ai)
        return self._cache[(gi, ai)]

    def _parse_tracks(self, gi: int, ai: int) -> list:
        g = self.graphs[gi]
        a = self.graph_anims(gi)[ai]
        rest = self.rest_pose(gi)
        b = self.blob(a)
        nn, fc = a.node_count, a.frame_count
        modes = b[:nn]
        pos = _pad4(nn)
        out = []
        for n in range(nn):
            node = []
            for ch in range(3):
                mode = modes[n] >> (2 * ch) & 3
                if mode == M_DEFAULT:
                    node.append(("static", (rest.rot[n], rest.trans[n], rest.scale[n])[ch]))
                elif mode == M_STATIC:
                    if ch == 0:
                        node.append(("static", unpack_qs16(struct.unpack_from("<4h", b, pos))))
                        pos += 8
                    elif ch == 1:
                        node.append(("static", struct.unpack_from("<3f", b, pos)))
                        pos += 12
                    else:
                        node.append(("static", struct.unpack_from("<f", b, pos)[0]))
                        pos += 4
                elif mode == M_ANIMATED:
                    k, flags = struct.unpack_from("<HH", b, pos)
                    pos += 4
                    base = step = None
                    if ch == 1 and not flags & T_F32:
                        base = struct.unpack_from("<3f", b, pos)
                        step = struct.unpack_from("<3f", b, pos + 12)
                        pos += 24
                    keys = None
                    if not flags & T_DENSE:
                        w = 1 if flags & T_IDX8 else 2
                        keys = list(struct.unpack_from("<%d%s" % (k, "B" if w == 1 else "H"), b, pos))
                        pos += _pad4(w * k)
                    per = {0: 4 if flags & T_AXIS else 6, 1: 6 if not flags & T_F32 else 12, 2: 4}[ch]
                    vals = []
                    for j in range(k):
                        o = pos + per * j
                        if ch == 0 and flags & T_AXIS:
                            vals.append(unpack_planar(*struct.unpack_from("<2h", b, o), flags >> 4 & 3))
                        elif ch == 0:
                            vals.append(unpack_q16(*struct.unpack_from("<3h", b, o)))
                        elif ch == 1 and base is not None:
                            u = struct.unpack_from("<3H", b, o)
                            vals.append(tuple(base[c] + u[c] * step[c] for c in range(3)))
                        elif ch == 1:
                            vals.append(struct.unpack_from("<3f", b, o))
                        else:
                            vals.append(struct.unpack_from("<f", b, o)[0])
                    pos += _pad4(per * k)
                    node.append(("track", bool(flags & T_DENSE), keys, vals))
                else:
                    raise ConvertError("bad channel mode")
            out.append(node)
        if pos != len(b):
            raise ConvertError(f"blob is {len(b)} bytes, tracks end at {pos}")
        return out

    def sample(self, gi: int, ai: int, frame: float) -> list[tuple]:
        """Reference sampler, the contract of anim_sample: [(rot, trans, scale)] for every graph node."""
        g = self.graphs[gi]
        a = self.graph_anims(gi)[ai]
        rest = self.rest_pose(gi)
        tracks = self.tracks(gi, ai)
        fc = a.frame_count
        last = fc - 1
        loops = bool(a.flags & ANIM_LOOPS)
        if not frame >= 0.0:
            frame = 0.0
        wrap_t = 0.0
        if loops and fc > 1:
            lf = a.loop_frame if 0 <= a.loop_frame < fc - 1 else 0
            if frame >= fc:
                frame = lf + math.fmod(frame - lf, fc - lf)
            if frame > last:
                wrap_t, frame = frame - last, float(last)
                lf_frame = float(lf)
        elif frame > last:
            frame = float(last)

        def at(track, f: float, blend):
            _, dense, keys, vals = track
            i = int(f)
            if dense:
                lo = min(i, len(vals) - 2)
                return blend(vals[lo], vals[lo + 1], f - lo)
            lo, hi = 0, len(keys) - 1
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if keys[mid] <= i:
                    lo = mid
                else:
                    hi = mid
            return blend(vals[lo], vals[lo + 1], (f - keys[lo]) / (keys[lo + 1] - keys[lo]))

        def value(ch: tuple, f: float, blend):
            return ch[1] if ch[0] == "static" else at(ch, f, blend)

        out = []
        for n in range(g.node_count):
            if n >= a.node_count:
                out.append((rest.rot[n], rest.trans[n], rest.scale[n]))
                continue
            r, t, s = tracks[n]
            res = []
            for ch, blend in ((r, nlerp), (t, lerp3), (s, lerp1)):
                v = value(ch, frame, blend)
                if wrap_t > 0.0:
                    v = blend(v, value(ch, lf_frame, blend), wrap_t)
                res.append(v)
            out.append(tuple(res))
        return out


def parse_section(data: bytes) -> Section:
    if len(data) < HEADER_SIZE:
        raise ConvertError("shorter than the header")
    magic, version, _flags, ng, na, goff, aoff, poff, total = struct.unpack_from("<4sHHIIIIII", data)
    if magic != MAGIC or version != VERSION:
        raise ConvertError(f"bad magic/version {magic!r}/{version}")
    if total != len(data) or goff != HEADER_SIZE or aoff != goff + GRAPH_REC * ng or poff < aoff + ANIM_REC * na:
        raise ConvertError("inconsistent header")
    graphs = []
    for i in range(ng):
        tag, nn, cnt, first, doff, model, noff = struct.unpack_from("<IHHIIII", data, goff + GRAPH_REC * i)
        graphs.append(SGraph(tag, nn, cnt, first, doff, model, noff))
    anims = []
    for i in range(na):
        (name, typ, fc, loop, flags, nn, rtype, gnodes, fps, doff, dbytes, roff,
         defoff) = struct.unpack_from(ANIM_FMT, data, aoff + ANIM_REC * i)
        anims.append(SAnim(_cstr(name), typ, fc, loop, flags, nn, rtype, gnodes, fps, doff, dbytes, roff, defoff))
    return Section(data, graphs, anims, poff)


# ---- error measurement ---------------------------------------------------------------------------------------------
def measure_errors(sec: Section, graphs: list[GraphSource], frames: str = "all") -> ErrStats:
    """Sample every animation of the section at every source frame with the reference sampler and
    compare with the decoded Xbox frames: rotation degrees, translation millimetres."""
    st = ErrStats()
    for gs in graphs:
        gi = sec.graph_index(gs.tag_id)
        for ai, a in enumerate(gs.anims):
            d = decode(a)
            for f in range(d.fc):
                pose = sec.sample(gi, ai, float(f)) if frames == "all" else None
                for n in range(d.nn):
                    r = d.rot[n][min(f, len(d.rot[n]) - 1)]
                    t = d.trans[n][min(f, len(d.trans[n]) - 1)]
                    er = qangle(pose[n][0], r)
                    et = _dist3(pose[n][1], t) * WU_MM
                    st.rot_max, st.trans_max = max(st.rot_max, er), max(st.trans_max, et)
                    st.rot_sum2 += er * er
                    st.rot_n += 1
                    st.trans_sum2 += et * et
                    st.trans_n += 1
                    es = abs(pose[n][2] - d.scale[n][min(f, len(d.scale[n]) - 1)])
                    st.scale_max = max(st.scale_max, es)
    return st


def verify_graph(g: GraphSource) -> list[str]:
    """Invariants of the decoded source data: unit rotations, continuity, plausible translations."""
    out = []
    for a in g.anims:
        d = decode(a)
        for n in range(d.nn):
            for q in d.rot[n]:
                if abs(sum(c * c for c in q) - 1.0) > 1e-5:
                    out.append(f"{a.name} node {n}: rotation not unit length")
                    break
            for t in d.trans[n]:
                if max(abs(c) for c in t) > 400.0:
                    out.append(f"{a.name} node {n}: translation {t} implausible")
                    break
    return out


# ---- CLI --------------------------------------------------------------------------------------------------------------
def _mib(n: int) -> str:
    return f"{n / 2**20:.2f} MiB"


def print_info(sec: Section, names: dict[int, str] | None = None, stats: BuildStats | None = None) -> None:
    print(f"{len(sec.graphs)} graphs, {len(sec.anims)} animations, {len(sec.data)} bytes")
    print(f"{'tag id':<11}{'nodes':>6}{'anims':>7}{'frames':>8}{'bytes':>10}  model/path")
    for gi, g in enumerate(sec.graphs):
        an = sec.graph_anims(gi)
        if stats and g.tag_id in stats.graph_bytes:
            nbytes = stats.graph_bytes[g.tag_id]
        else:
            seen: dict[int, int] = {}
            for a in an:
                seen[a.data_off] = a.data_bytes
                if a.root_off:
                    seen[a.root_off] = a.root_bytes
            nbytes = sum(seen.values()) + 32 * g.node_count
        label = (names or {}).get(g.tag_id, f"model {g.model_tag:#010x}" if g.model_tag else "no model")
        print(f"{g.tag_id:#010x}{g.node_count:>6}{g.anim_count:>7}{sum(a.frame_count for a in an):>8}"
              f"{nbytes:>10}  {label}")
    print(f"total {_mib(len(sec.data))}: {100 * len(sec.data) / DEFAULT_BUDGET:.1f}% of the {_mib(DEFAULT_BUDGET)} "
          f"resident budget (docs/02-specs.md 5.3)")


def parse_tags(values: list[str], t_paths: dict[int, str]) -> list[int]:
    out = []
    by_path = {v.lower(): k for k, v in t_paths.items()}
    for v in values:
        try:
            out.append(int(v, 0))
        except ValueError:
            hits = [k for p, k in by_path.items() if v.lower() in p]
            if not hits:
                raise ConvertError(f"no model_animations tag matches {v!r}")
            out.extend(hits)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map, or a .hp2 with --info")
    ap.add_argument("-o", "--out", type=Path, help="write an .hp2 pack holding the ANIM section")
    ap.add_argument("--tags", nargs="+", help="only these model_animations tags (0x tag ids or path substrings)")
    ap.add_argument("--info", action="store_true", help="print per graph: nodes, animations, bytes")
    ap.add_argument("--verify", action="store_true", help="also measure the quantisation error against the Xbox frames")
    ap.add_argument("--no-root-motion", action="store_true", help="drop the frame info (root motion) blocks")
    args = ap.parse_args(argv)
    if args.input.suffix == ".hp2":
        sec = parse_section(hp2.parse(args.input.read_bytes()).read("ANIM"))
        print_info(sec)
        return 0
    t0 = time.time()
    tags = MapTags(args.input)
    paths = {e.tag_id: e.path for e in tags.idx.entries if e.group == "antr"}
    sel = parse_tags(args.tags, paths) if args.tags else None
    graphs = load_graphs(tags, sel)
    stats = BuildStats()
    data = build_section_from_graphs(graphs, stats, not args.no_root_motion)
    dt = time.time() - t0
    for g in graphs:
        for p in g.problems:
            print(f"warning: {g.path}: {p}", file=sys.stderr)
    if args.info or args.verify:
        sec = parse_section(data)
        print_info(sec, {g.tag_id: g.path for g in graphs}, stats)
        print(f"Xbox source data {_mib(stats.source_bytes)} -> {_mib(len(data))} "
              f"({100 * len(data) / max(1, stats.source_bytes):.0f}%); shared blobs saved {_mib(stats.dedup_saved)}; "
              f"converted in {dt:.1f} s")
        if args.verify:
            e = measure_errors(sec, graphs)
            print(f"error vs the Xbox frames: rotation max {e.rot_max:.3f} deg rms "
                  f"{math.sqrt(e.rot_sum2 / max(1, e.rot_n)):.4f}; translation max {e.trans_max:.3f} mm rms "
                  f"{math.sqrt(e.trans_sum2 / max(1, e.trans_n)):.4f}; scale max {e.scale_max:.2e}")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        pack = hp2.build(args.input.stem, [("ANIM", data)])
        args.out.write_bytes(pack)
        print(f"wrote {args.out} ({len(pack) / 2**20:.2f} MiB, ANIM {len(data) / 2**20:.2f} MiB raw, {dt:.1f} s)")
    elif not (args.info or args.verify):
        ap.error("-o or --info is required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
