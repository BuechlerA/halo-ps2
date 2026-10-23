#!/usr/bin/env python3
"""Convert the objects a Xbox scenario places, their definitions, the player's first-person set and
per-cluster object lighting into the PS2 ``SCEN`` section, version 1 (docs/13-scen-section.md; the
reader is engine/src/world/scen.c, its contract engine/src/world/scen.h).

Everything below was read off the retail maps (all ten campaign maps for the strides and fields,
a10 / a30 / b40 in depth); offsets are inside the tag's own data block, every pointer is an absolute
tag-space address, a reflexive is ``u32 count, u32 address, u32 pad``.  "verified" means checked
against the data (cross-checks named), "unverified" means taken from the public tag documentation
or a convention that the data does not contradict.

scenario (``scnr``)
    +0x030 skies (16 B tag references)       +0x204 object names (36 B: char[32], u16 object type, u16 index)
    +0x210 scenery (72 B)    +0x21C scenery palette (48 B)     +0x228 bipeds (120 B)  +0x234 palette
    +0x240 vehicles (120 B)  +0x24C palette                    +0x258 equipment (40 B) +0x264 palette
    +0x270 weapons (92 B)    +0x27C palette                    +0x288 device groups (52 B, not read)
    +0x294 machines (64 B)   +0x2A0 palette                    +0x2AC controls (64 B)  +0x2B8 palette
    +0x2C4 light fixtures (88 B)  +0x2D0 palette               +0x2DC sound scenery (40 B)  +0x2E8 palette
    +0x348 player starting profiles (104 B)    +0x354 player starting locations (52 B, not read)
    Palette entry (48 B): +0x00 tag reference {char[4] group (byte-reversed), u32 path pointer,
    u32 path length (0 on Xbox), u32 tag id}, 32 B unused.
    verified: every stride equals (address of the next non-empty block - this block) / count on all
    ten campaign maps; the sound scenery and every palette likewise.
    Placement, common to every type (verified: positions are inside a BSP on a10: 274/274 scenery,
    107/107 bipeds, 52/56 vehicles (the rest 0.02 away), ...):
        +0x00 u16 palette index        +0x02 u16 object name index (0xFFFF none)
        +0x04 u16 flags: bit 0 "not placed automatically" (the object is created by a script; verified: on
              a10 90 of 92 scenery with the bit set carry a name such as ``blam_shock_sparks_2`` or
              ``looking_panel_down_success`` while 177 of 182 without it are unnamed set dressing),
              bits 1..3 "not placed on easy / normal / hard" and bit 4 "use player appearance" (unverified
              meaning: only the bit patterns 0, 1, 3, 8 and 4 were seen)
        +0x06 u16 desired permutation (always 0 on scenery; 100, 10, 12 on 5 of 107 a10 bipeds): the number the model's
              permutation names end in (``sgt_johnson-100``, ``head_charles_helmet-10``, ``head_marcus_cap-101``,
              ``damaged torso-102``); a biped's value is carried in the SCEN placement (docs/13), ``variant`` stays empty
        +0x08 f32 x, y, z world units           +0x14 f32 yaw, pitch, roll radians
    Scenery and light fixtures additionally carry +0x20 u16 "manual BSP flags": bit n = BSP n.
    verified: on a10 scenery it equals the BSP set found by point location on 270 of 274 placements, and is a
    strict superset on the other 4 (placed in two BSPs).  No other placement type has it (the field is 0).
    Rotation: yaw is a rotation about +z, counter-clockwise seen from above, from +x towards +y (Halo is
    x forward, y left, z up).  Weakly verified (light fixtures, which are mounted on walls: a yaw of the
    opposite sign makes the local direction of the nearest wall less consistent across fixtures of the
    same kind, 0.58 against 0.52, and the BSP sun direction of a30 is predicted with the same sign, see the
    lighting section).  The matrix is Rz(yaw) * Ry(-pitch) * Rx(roll): positive pitch raises the nose
    (forward.z = sin pitch) and positive roll banks to the right.  Unverified for pitch and roll (a
    ground-alignment test with 2265 tilted objects was inconclusive: most are not ground aligned).

object tags (scen bipd vehi weap eqip mach ctrl lifi ssce)
    +0x00 u16 object type (scen 6, bipd 0, vehi 1, weap 2, eqip 3, mach 7, ctrl 8, lifi 9, ssce 11, verified)
    +0x04 f32 bounding radius, +0x08 f32[3] bounding offset (verified: the offset is non-zero for 36 of 98 a10
          object tags, e.g. a biped's is -0.04 in x, a chair's 0.5 forward)
    +0x28 model reference (group ``mode``), +0x38 animation graph reference (group ``antr``)
    The def radius is ``bounding radius + |bounding offset|``: a sphere around the object origin that
    contains the tag's bounding sphere.  (The model's own bounds are not used.)
weapon tag: +0x45C first person model (``mode``), +0x46C first person animations (``antr``); verified on
    the assault rifle, pistol and plasma pistol of a10 and a30 (paths ``weapons\\<name>\\fp\\fp``).

animation graph (``antr``)
    +0x00 objects (20 B)  +0x0C units (100 B)  +0x18 weapons  +0x24 vehicles  +0x30 devices  +0x3C unit damage
    +0x48 first person weapons  +0x54 sound references  +0x68 nodes (64 B)  +0x74 animations (180 B)
    animation: +0x00 char[32] name, +0x20 u16 type (0 base, 1 overlay, 2 replacement), +0x22 u16 frame count
    unit (100 B): +0x00 char[32] label ("stand", "crouch", "alert", seat labels ...), +0x40 reflexive of u16
        animation indices (unit level), +0x4C ik points, +0x58 reflexive of weapon blocks (188 B)
    unit weapon (188 B): +0x00 char[32] weapon name ("rifle", "pistol", "unarmed", "fixed" ...),
        +0x20 grip marker, +0x40 hold marker ("right hand"), +0x98 reflexive of u16 animation indices
        (idle, turn-left, turn-right, move-front, ... in that fixed order), +0xA4 ik, +0xB0 weapon types
    first person weapon: +0x10 reflexive of u16 animation indices, entry 0 = idle (verified on three weapons
        by name: ``first-person idle``)
    node (64 B): +0x00 char[32] name, +0x20 s16 next sibling, +0x22 s16 first child, +0x24 s16 parent
    verified by name on cyborg, marine, warthog, the a10 chairs / cryotube / doors, the pistol and
    assault rifle first-person graphs: ``unit[stand].weapon[unarmed or first].animations[0]`` is always an
    animation called ``<pose> <weapon> idle`` when the graph has one.

idle rule (``idle_animation``; the value stored in ``scen_def.idle_anim``)
    Only biped, vehicle and scenery defs get one (devices animate by position, weapons and equipment are
    pickups: static).  In the object's graph take unit ``stand`` (else the first unit), in it the weapon
    block ``unarmed`` (else every block in order) and its ``animations[0]``; accept the first candidate
    whose index is valid and whose name contains ``idle`` (so a cryotube whose only animation is
    ``stand opening`` stays static).  If no unit offers one: static (SCEN_NO_ANIM).
    ``anim_tag`` is stored for every object that has a graph, idle or not (device position animations,
    vehicle suspension, weapon fire animations are driven by the engine later).

globals (``matg``): +0x170 player information (first element +0x00 tag reference to the default player
    biped, ``characters\\cyborg\\cyborg``), +0x17C first person interface (first element +0x00 reference to the
    first person hands model, ``characters\\cyborg\\fp\\fp``).  Both verified by path.
    scenario player starting profile (104 B): +0x00 char[32] name, +0x20 f32 health modifier, +0x24 f32 shield
    modifier, +0x28 primary weapon reference, +0x38 u16 rounds loaded, +0x3A u16 rounds total, +0x3C secondary
    weapon reference.  verified on a10 / a30 / b40 / c10 / d20.
    Default weapon rule: the primary weapon of the first starting profile that gives one (else the first
    secondary).  On a10 profile 0 (``player0_starting_profile``) is unarmed, so the first weapon is the pistol of
    ``bridge0_profile``; on a30 / b40 / c10 / d20 it is the assault rifle.
    First person set (verified on a10's assault rifle, the same node check passes for every default weapon of the ten campaign
    maps): the hands model has 37 nodes and no markers, the weapon's first person model 5 nodes, and the first person
    animation graph 42 = 37 + 5 nodes: ONE graph animates the hands and
    the gun together (node names equal); the gun's root node (``frame gun``) hangs under the hands node
    ``frame r wriste`` in the graph's tree.  So ``fp_arms_anim_tag == weapon_fp_anim_tag``, both idle indices are
    the weapon's first person idle, and ``weapon_hand_marker`` holds that NODE name (the hands model has no
    marker).  The hands model's root node sits at the origin: there is no camera-space offset in the data (the
    arms' placement is in the animation frames), ``fp_offset`` is zero.

lighting probes (``cluster_lights``), see docs/13-scen-section.md for the derivation
    ambient = clamp(floor, avg lightmap colour x (1 - f/2)), sun = avg colour x f, scale 128 = 1.0 (the lightmap pass uses the
    same scale).  Verified: the direction of the sky light 0 (+0x68 yaw, +0x6C pitch of a sky tag light, 116 B at sky
    +0xC4; the direction TOWARDS the light is (cos p cos y, cos p sin y, sin p)) agrees with the lightmap brightness
    gradient of outdoor clusters (a30: mean cosine 0.82, 98..100 % of the area above 0.5; a10b / a10e 0.91 / 0.95).
    BSP triangles are wound clockwise seen from the front (the geometric normal e1 x e2 points INTO the surface).

Usage:
    python3 -I tools/halo2ps2/scenconv.py assets/decompressed/a10.map -o assets/hp2/a10.scen.hp2 [--bsp N]
    python3 -I tools/halo2ps2/scenconv.py --info assets/decompressed/a10.map [--lights]
    python3 -I tools/halo2ps2/scenconv.py --info assets/hp2/a10.scen.hp2
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
from halo2ps2 import bsp, bspconv, hp2, tagindex, texconv  # noqa: E402

MAGIC = b"SCEN"
VERSION = 1
HEADER_SIZE = 128
DEF_REC = 64
PLACEMENT_REC = 64
LIGHT_REC = 16
BSPTAB_REC = 8
PLAYER_REC = 80
NAME_LEN = 32
VARIANT_LEN = 16
MAX_BSPS = 32
NO_ANIM = 0xFFFF
MAX_DEFS = 0xFFFF

# scen_type
SCENERY, BIPED, VEHICLE, WEAPON, EQUIPMENT, MACHINE, CONTROL, LIGHT_FIXTURE, PROJECTILE, OTHER = range(10)
TYPE_NAMES = ("scenery", "biped", "vehicle", "weapon", "equipment", "machine", "control",
              "light_fixture", "projectile", "other")
# placement flags (docs/13): bit 0 is SCEN_P_INITIALLY_VISIBLE in scen.h; the others are carried raw
P_INITIALLY_VISIBLE, P_NOT_EASY, P_NOT_NORMAL, P_NOT_HARD, P_PLAYER_APPEARANCE = 1, 2, 4, 8, 16

# neutral lighting (what scen_cluster_light returns for a missing entry): 0.5 ambient + 0.5 sun from above
NEUTRAL_AMBIENT = (64, 64, 64)
NEUTRAL_SUN = (64, 64, 64)
NEUTRAL_DIR = (0.0, 0.0, 1.0)
AMBIENT_FLOOR = 20            # no cluster is ever darker than 20/128 per channel
PRIOR_AREA = 24.0             # world units^2 of the BSP-wide mean blended into every cluster's average
MAX_SUN_FRACTION = 0.75
GRADIENT_EPSILON = 0.5        # |gradient| below this (0..255 units): no usable direction

# scenario / tag offsets (docs above)
SCN_SKIES, SCN_NAMES = 0x30, 0x204
SCN_PROFILES = 0x348
PALETTE_REC = 48
NAME_REC = 36
KINDS = (  # name, placements, stride, palette, group, scen_type, object type value in the tag
    ("scenery", 0x210, 72, 0x21C, "scen", SCENERY, 6),
    ("biped", 0x228, 120, 0x234, "bipd", BIPED, 0),
    ("vehicle", 0x240, 120, 0x24C, "vehi", VEHICLE, 1),
    ("equipment", 0x258, 40, 0x264, "eqip", EQUIPMENT, 3),
    ("weapon", 0x270, 92, 0x27C, "weap", WEAPON, 2),
    ("machine", 0x294, 64, 0x2A0, "mach", MACHINE, 7),
    ("control", 0x2AC, 64, 0x2B8, "ctrl", CONTROL, 8),
    ("light_fixture", 0x2C4, 88, 0x2D0, "lifi", LIGHT_FIXTURE, 9),
    ("sound_scenery", 0x2DC, 40, 0x2E8, "ssce", None, 11),     # counted, skipped
)
MANUAL_BSP_KINDS = {"scenery", "light_fixture"}
PROFILE_REC = 104
OBJ_MODEL, OBJ_ANIM = 0x28, 0x38
WEAPON_FP_MODEL, WEAPON_FP_ANIM = 0x45C, 0x46C
ANIM_UNITS, ANIM_FP_WEAPONS, ANIM_NODES, ANIM_ANIMATIONS = 0x0C, 0x48, 0x68, 0x74
ANIM_REC, UNIT_REC, UNIT_WEAPON_REC, NODE_REC = 180, 100, 188, 64
SKY_LIGHTS, SKY_LIGHT_REC = 0xC4, 116
PROBE_RADII = (0.02, 0.05, 0.1, 0.25, 0.5)     # wu, point location tolerance (docs/13)


class ScenError(ValueError):
    pass


# ---- data model ---------------------------------------------------------------------------------
@dataclass
class Def:
    tag_id: int
    type: int
    name: str
    model_tag: int = 0
    anim_tag: int = 0
    idle_anim: int = NO_ANIM
    radius: float = 0.0


@dataclass
class Placement:
    tag_id: int                          # object tag (resolved to a def index when the section is built)
    kind: str
    flags: int
    pos: tuple[float, float, float]
    rot: tuple[float, float, float]
    name_index: int = -1
    bsp_mask: int = 0
    variant: str = ""
    permutation: int = 0                 # "desired permutation" (+0x06): selects the model permutations whose name ends in -<number> (docs/13)
    manual_bsp: int = 0
    mask_tier: str = "none"
    def_index: int = 0


@dataclass
class Player:
    fp_arms_model_tag: int = 0
    fp_arms_anim_tag: int = 0
    weapon_tag: int = 0                  # object tag id of the default weapon (-> weapon_def)
    weapon_fp_model_tag: int = 0
    weapon_fp_anim_tag: int = 0
    biped_tag: int = 0                   # object tag id of the player biped (-> biped_def)
    arms_idle_anim: int = NO_ANIM
    weapon_idle_anim: int = NO_ANIM
    weapon_hand_marker: str = ""
    fp_offset: tuple[float, float, float] = (0.0, 0.0, 0.0)
    weapon_def: int = 0                  # def index + 1 (set when the section is built)
    biped_def: int = 0
    notes: list[str] = field(default_factory=list)


@dataclass
class ClusterLight:
    ambient: tuple[int, int, int]
    sun: tuple[int, int, int]
    sun_dir: tuple[float, float, float]


@dataclass
class ObjectSet:
    defs: list[Def]
    placements: list[Placement]
    player: Player
    model_tags: list[int]
    anim_tags: list[int]
    counts: dict[str, int]                           # placements per kind, including skipped kinds
    bsp_count: int = 0
    bsp_counts: list[int] = field(default_factory=list)    # placements per BSP (a placement can count in several)
    mask_tiers: Counter = field(default_factory=Counter)
    unresolved: int = 0                              # placements dropped: null palette entry / bad tag
    problems: list[str] = field(default_factory=list)


# ---- tag region access --------------------------------------------------------------------------
class Region:
    """Bounds-checked reads of the tag region of a decompressed map (offsets are region offsets)."""

    def __init__(self, m: texconv.MapData):
        self.m = m
        self.data = m.region
        self.base = m.base

    def u(self, fmt: str, off: int):
        if off < 0:
            raise ScenError(f"negative offset {off}")
        try:
            return struct.unpack_from(fmt, self.data, off)
        except struct.error:
            raise ScenError(f"read of {fmt} at {off:#x} outside the tag region") from None

    def reflexive(self, off: int, stride: int, limit: int = 100000) -> tuple[int, int]:
        count, addr = self.u("<II", off)
        if count == 0:
            return 0, 0
        o = addr - self.base
        if count > limit or o < 0 or o + count * stride > len(self.data):
            raise ScenError(f"reflexive at {off:#x}: {count} x {stride} at {addr:#x} outside the tag region")
        return count, o

    def tagref(self, off: int) -> tuple[str | None, int]:
        """(group, tag id) of the tag reference at ``off``; (None, 0xFFFFFFFF) when null."""
        raw, tid = self.u("<4s", off)[0], self.u("<I", off + 12)[0]
        group = tagindex.fourcc(raw)
        if tid == 0xFFFFFFFF or group is None:
            return None, 0xFFFFFFFF
        return group, tid

    def entry(self, tid: int) -> tagindex.TagEntry | None:
        return self.m.by_id.get(tid)

    def block(self, e: tagindex.TagEntry) -> int:
        return e.data_address - self.base

    def cstr(self, off: int, n: int) -> str:
        raw = self.data[off:off + n].split(b"\0", 1)[0]
        return raw.decode("latin1")


def _tail(path: str) -> str:
    return path.rsplit("\\", 1)[-1][:NAME_LEN - 1]


# ---- animation graphs ---------------------------------------------------------------------------
def animation_names(r: Region, graph_block: int) -> list[str]:
    n, o = r.reflexive(graph_block + ANIM_ANIMATIONS, ANIM_REC, 4096)
    return [r.cstr(o + i * ANIM_REC, NAME_LEN) for i in range(n)]


def _u16_list(r: Region, at: int) -> list[int]:
    n, o = r.reflexive(at, 2, 4096)
    return list(r.u(f"<{n}H", o)) if n else []


def idle_animation(r: Region, graph_block: int) -> int:
    """The animation index a placed unit-like object loops (docs: idle rule); NO_ANIM if the graph has none."""
    names = animation_names(r, graph_block)

    def ok(i: int) -> bool:
        return 0 <= i < len(names) and "idle" in names[i].lower()

    n, o = r.reflexive(graph_block + ANIM_UNITS, UNIT_REC, 256)
    units = [(r.cstr(o + i * UNIT_REC, NAME_LEN), o + i * UNIT_REC) for i in range(n)]
    if not units:
        return NO_ANIM
    stand = [u for u in units if u[0] == "stand"]
    label, ub = (stand or units)[0]
    wn, wo = r.reflexive(ub + 0x58, UNIT_WEAPON_REC, 256)
    weapons = [(r.cstr(wo + i * UNIT_WEAPON_REC, NAME_LEN), wo + i * UNIT_WEAPON_REC) for i in range(wn)]
    order = [w for w in weapons if w[0] == "unarmed"] + [w for w in weapons if w[0] != "unarmed"]
    for _, wb in order:
        idx = _u16_list(r, wb + 0x98)
        if idx and ok(idx[0]):
            return idx[0]
    return NO_ANIM


def first_person_idle(r: Region, graph_block: int) -> int:
    names = animation_names(r, graph_block)
    n, o = r.reflexive(graph_block + ANIM_FP_WEAPONS, 0x1C, 64)
    if not n:
        return NO_ANIM
    idx = _u16_list(r, o + 0x10)
    if idx and 0 <= idx[0] < len(names) and "idle" in names[idx[0]].lower():
        return idx[0]
    return NO_ANIM


def graph_nodes(r: Region, graph_block: int) -> list[tuple[str, int]]:
    """(name, parent) per node of the animation graph (parent -1 for the root)."""
    n, o = r.reflexive(graph_block + ANIM_NODES, NODE_REC, 256)
    return [(r.cstr(o + i * NODE_REC, NAME_LEN), r.u("<h", o + i * NODE_REC + 0x24)[0]) for i in range(n)]


def model_nodes(r: Region, model_block: int) -> list[str]:
    n, o = r.reflexive(model_block + 0xB8, 156, 256)
    return [r.cstr(o + i * 156, NAME_LEN) for i in range(n)]


# ---- object definitions -------------------------------------------------------------------------
class DefTable:
    """Object definitions keyed by tag id, built lazily from the object tags."""

    def __init__(self, r: Region, problems: list[str]):
        self.r, self.problems = r, problems
        self.defs: dict[int, Def] = {}

    def get(self, tid: int, typ: int, group: str, objtype: int | None) -> Def | None:
        if tid in self.defs:
            return self.defs[tid]
        e = self.r.entry(tid)
        if e is None or e.group != group:
            self.problems.append(f"tag {tid:#x} is not a {group!r} tag")
            return None
        b = self.r.block(e)
        try:
            otype = self.r.u("<H", b)[0]
            radius, ox, oy, oz = self.r.u("<4f", b + 4)
            mg, mid = self.r.tagref(b + OBJ_MODEL)
            ag, aid = self.r.tagref(b + OBJ_ANIM)
            if objtype is not None and otype != objtype:
                self.problems.append(f"{e.path}: object type {otype}, expected {objtype}")
            d = Def(tid, typ, _tail(e.path))
            if not all(map(math.isfinite, (radius, ox, oy, oz))) or radius < 0:
                self.problems.append(f"{e.path}: bad bounding radius")
            else:
                d.radius = radius + math.sqrt(ox * ox + oy * oy + oz * oz)
            if mg == "mode" and self.r.entry(mid) is not None:
                d.model_tag = mid
            elif mg:
                self.problems.append(f"{e.path}: model reference {mg!r} not resolved")
            if ag == "antr" and self.r.entry(aid) is not None:
                d.anim_tag = aid
                if typ in (BIPED, VEHICLE, SCENERY):
                    d.idle_anim = idle_animation(self.r, self.r.block(self.r.entry(aid)))
            elif ag:
                self.problems.append(f"{e.path}: animation reference {ag!r} not resolved")
        except ScenError as ex:
            self.problems.append(f"{e.path}: {ex}")
            return None
        self.defs[tid] = d
        return d


# ---- player -------------------------------------------------------------------------------------
def read_player(r: Region, dt: DefTable, problems: list[str]) -> Player:
    p = Player()
    m = r.m
    glob = [e for e in m.index.entries if e.group == "matg"]
    scn = m.index.scenario
    try:
        if glob:
            gb = r.block(glob[0])
            n, o = r.reflexive(gb + 0x170, 8)
            if n:
                g, tid = r.tagref(o)
                if g == "bipd" and dt.get(tid, BIPED, "bipd", 0):
                    p.biped_tag = tid
            n, o = r.reflexive(gb + 0x17C, 8)
            if n:
                g, tid = r.tagref(o)
                if g == "mode" and r.entry(tid):
                    p.fp_arms_model_tag = tid
        sb = r.block(scn)
        n, o = r.reflexive(sb + SCN_PROFILES, PROFILE_REC, 64)
        weapon = 0
        for off in (0x28, 0x3C):                  # primary weapons of all profiles first, then secondary
            for i in range(n):
                g, tid = r.tagref(o + i * PROFILE_REC + off)
                if g == "weap" and r.entry(tid):
                    weapon = tid
                    break
            if weapon:
                break
        if weapon and dt.get(weapon, WEAPON, "weap", 2):
            p.weapon_tag = weapon
            wb = r.block(r.entry(weapon))
            mg, mid = r.tagref(wb + WEAPON_FP_MODEL)
            ag, aid = r.tagref(wb + WEAPON_FP_ANIM)
            if mg == "mode" and r.entry(mid):
                p.weapon_fp_model_tag = mid
            if ag == "antr" and r.entry(aid):
                p.weapon_fp_anim_tag = aid
                p.fp_arms_anim_tag = aid             # one graph drives hands and gun (docs)
                gblock = r.block(r.entry(aid))
                p.arms_idle_anim = p.weapon_idle_anim = first_person_idle(r, gblock)
                gnodes = graph_nodes(r, gblock)
                if p.weapon_fp_model_tag and p.fp_arms_model_tag:
                    arms = model_nodes(r, r.block(r.entry(p.fp_arms_model_tag)))
                    gun = model_nodes(r, r.block(r.entry(p.weapon_fp_model_tag)))
                    names = [n_ for n_, _ in gnodes]
                    if len(gnodes) != len(arms) + len(gun) or not (set(arms) | set(gun)) <= set(names):
                        p.notes.append("first person graph nodes are not hands + gun nodes")
                    elif gun:
                        par = gnodes[names.index(gun[0])][1]
                        if 0 <= par < len(gnodes) and gnodes[par][0] in arms:
                            p.weapon_hand_marker = gnodes[par][0]
                        else:
                            p.notes.append("gun root has no parent among the hands nodes")
        elif not weapon:
            p.notes.append("no starting profile gives a weapon")
    except ScenError as ex:
        problems.append(f"player: {ex}")
    return p


# ---- BSPs and point location --------------------------------------------------------------------
@dataclass
class BspProbe:
    index: int
    path: str
    structure: bsp.Structure
    bounds: tuple[float, float, float, float, float, float]   # x min max, y min max, z min max


def load_bsps(m: texconv.MapData) -> list[BspProbe]:
    out = []
    with m.path.open("rb") as f:
        for i, loc in enumerate(bsp.locate(m.index, m.region, m.header.decompressed_size)):
            f.seek(loc.file_offset)
            block = f.read(loc.size)
            st = bsp.read_structure(loc, block)
            root = struct.unpack_from("<I", block, 0)[0] - loc.address
            out.append(BspProbe(i, loc.path, st, struct.unpack_from("<6f", block, root + 0xC8)))
    return out


def locate_mask(probes: list[BspProbe], pos: tuple[float, float, float], manual: int = 0) -> tuple[int, str]:
    """Which BSPs a placement lies in, and how that was decided (docs/13, "BSP membership"):
    1. exact point location in every BSP (``exact``), plus the scenario's manual BSP flags (``flags``);
    2. if that is empty: the six axis neighbours at 0.02, 0.05, 0.1, 0.25, 0.5 wu, the first radius with a
       hit decides (``probe``);
    3. if still empty: every BSP whose world bounds contain the position (``bounds``);
    else 0 (``none``)."""
    n = len(probes)
    valid = (1 << n) - 1 if n < 32 else 0xFFFFFFFF
    exact = 0
    for p in probes:
        if bsp.find_cluster(p.structure, pos) >= 0:
            exact |= 1 << p.index
    mask = exact | (manual & valid)
    if mask:
        return mask, "exact" if exact else "flags"
    for rad in PROBE_RADII:
        for ax in range(3):
            for s in (-1.0, 1.0):
                q = list(pos)
                q[ax] += s * rad
                for p in probes:
                    if bsp.find_cluster(p.structure, tuple(q)) >= 0:
                        mask |= 1 << p.index
        if mask:
            return mask, "probe"
    for p in probes:
        b = p.bounds
        if b[0] <= pos[0] <= b[1] and b[2] <= pos[1] <= b[3] and b[4] <= pos[2] <= b[5]:
            mask |= 1 << p.index
    return (mask, "bounds") if mask else (0, "none")


# ---- collecting ---------------------------------------------------------------------------------
def collect_objects(m: texconv.MapData, probes: list[BspProbe], extra: dict[int, str] | None = None) -> ObjectSet:
    """Placements, definitions and player data of a map (no lighting). ``probes`` may be empty. ``extra`` maps object tag ids
    (biped / weapon) to their group: definitions that run-time spawning (AI units, dropped weapons) needs although no placement uses them."""
    r = Region(m)
    problems: list[str] = []
    dt = DefTable(r, problems)
    sb = r.block(m.index.scenario)
    nname, oname = r.reflexive(sb + SCN_NAMES, NAME_REC, 4096)
    counts: dict[str, int] = {}
    placements: list[Placement] = []
    unresolved = 0
    for kind, off, stride, pal_off, group, typ, objtype in KINDS:
        n, o = r.reflexive(sb + off, stride)
        counts[kind] = n
        if typ is None or n == 0:
            continue
        pn, po = r.reflexive(sb + pal_off, PALETTE_REC, 4096)
        palette: list[Def | None] = []
        for i in range(pn):
            g, tid = r.tagref(po + i * PALETTE_REC)
            palette.append(dt.get(tid, typ, group, objtype) if g == group else None)
        for i in range(n):
            e = o + i * stride
            pi, ni, raw = r.u("<3H", e)
            pos = r.u("<3f", e + 8)
            rot = r.u("<3f", e + 0x14)
            d = palette[pi] if pi < pn else None
            if d is None or not all(map(math.isfinite, pos + rot)):
                unresolved += 1
                continue
            flags = (raw & 0x1E) | (0 if raw & 1 else P_INITIALLY_VISIBLE)
            manual = r.u("<H", e + 0x20)[0] if kind in MANUAL_BSP_KINDS else 0
            mask, tier = locate_mask(probes, pos, manual) if probes else (0, "none")
            perm = r.u("<H", e + 6)[0] if kind == "biped" else 0     # a10: 100 on Sergeant Johnson, 10 / 12 on the two lab crew (assistant, technician)
            placements.append(Placement(d.tag_id, kind, flags, pos, rot, ni if ni < nname else -1,
                                        mask, "", perm, manual, tier))
    for tid, group in sorted((extra or {}).items()):
        if group == "bipd":
            dt.get(tid, BIPED, "bipd", 0)
        elif group == "weap":
            dt.get(tid, WEAPON, "weap", 2)
    player = read_player(r, dt, problems)
    order = sorted(dt.defs)
    index = {t: i for i, t in enumerate(order)}
    defs = [dt.defs[t] for t in order]
    if len(defs) > MAX_DEFS:
        raise ScenError(f"{len(defs)} definitions exceed the u16 placement index")
    for pl in placements:
        pl.def_index = index[pl.tag_id]
    if player.weapon_tag:
        player.weapon_def = index[player.weapon_tag] + 1
    if player.biped_tag:
        player.biped_def = index[player.biped_tag] + 1
    models = {d.model_tag for d in defs if d.model_tag}
    models |= {t for t in (player.fp_arms_model_tag, player.weapon_fp_model_tag) if t}
    anims = {d.anim_tag for d in defs if d.anim_tag}
    anims |= {t for t in (player.fp_arms_anim_tag, player.weapon_fp_anim_tag) if t}
    nb = len(probes)
    bsp_counts = [sum(1 for pl in placements if pl.bsp_mask >> b & 1) for b in range(nb)]
    return ObjectSet(defs, placements, player, sorted(models), sorted(anims), counts, nb, bsp_counts,
                     Counter(pl.mask_tier for pl in placements), unresolved, problems)


def collect(map_path: Path, extra: dict[int, str] | None = None) -> ObjectSet:
    """Everything of the SCEN section except the lighting probes."""
    m = texconv.MapData.open(map_path)
    return collect_objects(m, load_bsps(m), extra)


# ---- lighting probes ----------------------------------------------------------------------------
def sky_suns(r: Region) -> list[tuple[float, float, float] | None]:
    """Per scenario sky palette entry: unit vector towards its brightest light (colour luminance x power), or None."""
    out: list[tuple[float, float, float] | None] = []
    sb = r.block(r.m.index.scenario)
    n, o = r.reflexive(sb + SCN_SKIES, 16, 64)
    for i in range(n):
        g, tid = r.tagref(o + i * 16)
        e = r.entry(tid)
        sun = None
        if g == "sky " and e is not None:
            try:
                ln, lo = r.reflexive(r.block(e) + SKY_LIGHTS, SKY_LIGHT_REC, 64)
                best = 0.0
                for k in range(ln):
                    col = r.u("<3f", lo + k * SKY_LIGHT_REC + 0x50)
                    power = r.u("<f", lo + k * SKY_LIGHT_REC + 0x5C)[0]
                    yaw, pitch = r.u("<2f", lo + k * SKY_LIGHT_REC + 0x68)
                    lum = (0.299 * col[0] + 0.587 * col[1] + 0.114 * col[2]) * power
                    if all(map(math.isfinite, (lum, yaw, pitch))) and lum > best:
                        best = lum
                        sun = (math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch))
            except ScenError:
                sun = None
        out.append(sun)
    return out


def _clamp8(v: float) -> int:
    return max(0, min(255, int(v + 0.5)))


OBJECT_LM_GAIN = 1.7 * 1.8    # the BSP lightmap gain (scripts/build-world.sh LM_GAIN) x 1.8: a character stands in the light of all the walls around him, the
                              # area mean of the lightmap of a cluster is about half of its lit walls, and the Xbox bridge crew is as bright as the bulkheads (2026-10-18)


def cluster_lights(mats: list[bspconv.Material], st: bsp.Structure, pages: dict[int, texconv.Rgba],
                   suns: list[tuple[float, float, float] | None] | None = None,
                   gain: float = texconv.LM_DEFAULT_GAIN, gamma: float = 1.0, chroma: float = 1.0) -> list[ClusterLight]:
    """One ``ClusterLight`` per cluster of ``st`` (formula in docs/13-scen-section.md).

    Per lightmapped triangle: area, normal (``-(e1 x e2)``, BSP triangles are wound clockwise), colour = mean of
    four nearest-texel lightmap samples (three vertices and the centroid).  A triangle counts for every cluster
    that lists its surface (``st.cluster_surfaces``, the union of both routes)."""
    nc = len(st.clusters)
    tris: list[tuple[tuple[int, ...], float, tuple[float, float, float], tuple[float, float, float]]] = []
    for mat in mats:
        page = pages.get(mat.lightmap_page)
        if page is None:
            continue
        w, h, px = page

        def sample(u: float, v: float) -> tuple[int, int, int]:
            x = min(w - 1, max(0, int(u * w)))
            y = min(h - 1, max(0, int(v * h)))
            o = (y * w + x) * 4
            return px[o], px[o + 1], px[o + 2]

        for t, s in zip(mat.tris, mat.surfaces):
            cl = st.cluster_surfaces.get(s)
            if not cl:
                continue
            a, b, c = (mat.pos[k] for k in t)
            e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
            n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            ln = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
            if ln < 1e-9:
                continue
            uvs = [mat.lmuv[k] for k in t]
            samples = [sample(*uv) for uv in uvs]
            samples.append(sample(sum(uv[0] for uv in uvs) / 3.0, sum(uv[1] for uv in uvs) / 3.0))
            toned = [texconv.lightmap_tone_rgb(*sm, gain, gamma, chroma) for sm in samples]
            rgb = tuple(sum(sm[ch] for sm in toned) / 4.0 for ch in range(3))
            tris.append((tuple(cl), ln / 2.0, (-n[0] / ln, -n[1] / ln, -n[2] / ln), rgb))  # type: ignore[arg-type]
    # BSP-wide mean colour (each triangle once)
    total = sum(t[1] for t in tris)
    if total > 0:
        bsp_mean = tuple(sum(t[1] * t[3][ch] for t in tris) / total for ch in range(3))
    else:
        bsp_mean = tuple(float(v) for v in NEUTRAL_AMBIENT)
    area = [0.0] * nc
    csum = [[0.0] * 3 for _ in range(nc)]
    lsum = [0.0] * nc
    for cl, a, _n, rgb in tris:
        lum = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]
        for c in cl:
            area[c] += a
            lsum[c] += a * lum
            for ch in range(3):
                csum[c][ch] += a * rgb[ch]
    grad = [[0.0] * 3 for _ in range(nc)]
    for cl, a, n, rgb in tris:
        lum = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]
        for c in cl:
            w_ = a * (lum - lsum[c] / area[c])
            for ch in range(3):
                grad[c][ch] += w_ * n[ch]
    out: list[ClusterLight] = []
    for c in range(nc):
        a = area[c]
        mean = [(csum[c][ch] + PRIOR_AREA * bsp_mean[ch]) / (a + PRIOR_AREA) for ch in range(3)]
        mean_lum = 0.299 * mean[0] + 0.587 * mean[1] + 0.114 * mean[2]
        g = [x / a for x in grad[c]] if a > 0 else [0.0, 0.0, 0.0]
        gl = math.sqrt(sum(x * x for x in g))
        f = min(MAX_SUN_FRACTION, 3.0 * gl / mean_lum) if mean_lum > 1e-6 else 0.0
        sky = st.clusters[c].sky
        sun = suns[sky] if suns is not None and 0 <= sky < len(suns) else None
        if sun is not None:
            d = sun
        elif gl >= GRADIENT_EPSILON:
            d = (g[0] / gl, g[1] / gl, g[2] / gl)
        else:
            d, f = NEUTRAL_DIR, 0.0
        amb = tuple(_clamp8(max(float(AMBIENT_FLOOR), v * (1.0 - f / 2.0))) for v in mean)
        sunc = tuple(_clamp8(v * f) for v in mean)
        out.append(ClusterLight(amb, sunc, d))  # type: ignore[arg-type]
    return out


def map_lights(m: texconv.MapData, indices: list[int] | None) -> list[list[ClusterLight]]:
    """Lighting probes of the BSPs in ``indices`` (all when None); the others get an empty list."""
    r = Region(m)
    suns = sky_suns(r)
    infos = texconv.scan_map(m)
    locs = bsp.locate(m.index, m.region, m.header.decompressed_size)
    out: list[list[ClusterLight]] = []
    with m.path.open("rb") as f:
        for i, loc in enumerate(locs):
            if indices is not None and i not in indices:
                out.append([])
                continue
            f.seek(loc.file_offset)
            block = f.read(loc.size)
            mats, _ = bspconv.read_materials(block, loc)
            st = bsp.read_structure(loc, block)
            pages = texconv.lightmap_pages_rgba(m, infos[i])
            tone = texconv.bsp_tone(infos[i].path)
            out.append(cluster_lights(mats, st, pages, suns, tone[2] if tone else OBJECT_LM_GAIN, tone[3] if tone else texconv.LM_GAMMA, texconv.CHROMA_STRENGTH))
    return out


# ---- section writer / reference parser ----------------------------------------------------------
HEADER_FMT = "<4sHHIIIIIIIIII"      # magic, version, reserved, size, defs, placements, bsps, 5 offsets, lights
DEF_FMT = "<IIIBBH32sf12x"
PLACEMENT_FMT = "<HHI3f3f16shH12x"
LIGHT_FMT = "<3Bx3Bx3hxx"
PLAYER_FMT = "<6IHH3f32s8x"
for _f, _n in ((DEF_FMT, DEF_REC), (PLACEMENT_FMT, PLACEMENT_REC), (LIGHT_FMT, LIGHT_REC), (PLAYER_FMT, PLAYER_REC)):
    assert struct.calcsize(_f) == _n, (_f, struct.calcsize(_f), _n)
assert struct.calcsize(HEADER_FMT) <= HEADER_SIZE


def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def _str(s: str, n: int) -> bytes:
    raw = s.encode("latin1", "replace")[:n - 1]
    return raw.ljust(n, b"\0")


def _dir16(d: tuple[float, float, float]) -> tuple[int, int, int]:
    ln = math.sqrt(sum(c * c for c in d)) or 1.0
    q = [round(c / ln * 32767) for c in d]
    return q[0], q[1], q[2]


def build(defs: list[Def], placements: list[Placement], player: Player,
          lights: list[list[ClusterLight]], bsp_count: int | None = None) -> bytes:
    """Serialise a SCEN section. ``lights[b]`` are the cluster lights of BSP b (empty: none stored)."""
    nb = len(lights) if bsp_count is None else bsp_count
    if nb > MAX_BSPS or len(lights) > nb:
        raise ScenError(f"{nb} BSPs (maximum {MAX_BSPS})")
    if len(defs) > MAX_DEFS or any(a.tag_id >= b.tag_id for a, b in zip(defs, defs[1:])):
        raise ScenError("definitions must be sorted by strictly ascending tag id, at most 65535")
    out = bytearray(HEADER_SIZE)
    offs = {}
    offs["defs"] = len(out)
    for d in defs:
        out += struct.pack(DEF_FMT, d.tag_id, d.model_tag, d.anim_tag, d.type, 0, d.idle_anim,
                           _str(d.name, NAME_LEN), d.radius)
    _pad16(out)
    offs["placements"] = len(out)
    for p in placements:
        if not 0 <= p.def_index < len(defs):
            raise ScenError("placement definition index out of range")
        out += struct.pack(PLACEMENT_FMT, p.def_index, p.flags, p.bsp_mask, *p.pos, *p.rot,
                           _str(p.variant, VARIANT_LEN), p.name_index, p.permutation)
    _pad16(out)
    offs["lights"] = len(out)
    table = bytearray()
    nlights = 0
    for b in range(nb):
        cl = lights[b] if b < len(lights) else []
        table += struct.pack("<II", nlights, len(cl))
        for L in cl:
            out += struct.pack(LIGHT_FMT, *L.ambient, *L.sun, *_dir16(L.sun_dir))
        nlights += len(cl)
    _pad16(out)
    offs["tables"] = len(out)
    out += table
    _pad16(out)
    offs["player"] = len(out)
    out += struct.pack(PLAYER_FMT, player.fp_arms_model_tag, player.fp_arms_anim_tag, player.weapon_def,
                       player.weapon_fp_model_tag, player.weapon_fp_anim_tag, player.biped_def,
                       player.arms_idle_anim, player.weapon_idle_anim, *player.fp_offset,
                       _str(player.weapon_hand_marker, NAME_LEN))
    _pad16(out)
    per_type = [0] * 10
    for p in placements:
        per_type[TYPE_NAMES.index(p.kind) if p.kind in TYPE_NAMES else OTHER] += 1
    struct.pack_into(HEADER_FMT, out, 0, MAGIC, VERSION, 0, len(out), len(defs), len(placements), nb,
                     offs["defs"], offs["placements"], offs["lights"], offs["tables"], offs["player"], nlights)
    struct.pack_into("<10I", out, 0x38, *per_type)
    return bytes(out)


@dataclass
class Section:
    size: int
    defs: list[dict]
    placements: list[dict]
    lights: list[list[dict]]
    player: dict
    type_counts: list[int]


def parse_section(data: bytes) -> Section:
    """Reference reader (the C reader is engine/src/world/scen.c); raises ScenError on a bad section."""
    if len(data) < HEADER_SIZE:
        raise ScenError("section shorter than the header")
    magic, version, _r, size, nd, npl, nb, o_def, o_pl, o_li, o_tab, o_pla, nlights = struct.unpack_from(HEADER_FMT, data)
    if magic != MAGIC:
        raise ScenError("bad signature")
    if version != VERSION:
        raise ScenError(f"unsupported version {version}")
    if size != len(data) or nb > MAX_BSPS:
        raise ScenError("inconsistent header")
    for off, cnt, rec in ((o_def, nd, DEF_REC), (o_pl, npl, PLACEMENT_REC), (o_li, nlights, LIGHT_REC),
                          (o_tab, nb, BSPTAB_REC), (o_pla, 1, PLAYER_REC)):
        if off % 16 or off < HEADER_SIZE or off + cnt * rec > len(data):
            raise ScenError("table outside the section")
    defs = []
    for i in range(nd):
        tag, mt, at, typ, _p, idle, name, radius = struct.unpack_from(DEF_FMT, data, o_def + i * DEF_REC)
        defs.append(dict(tag_id=tag, model_tag=mt, anim_tag=at, type=typ, idle_anim=idle,
                         name=name.split(b"\0", 1)[0].decode("latin1"), radius=radius))
    pls = []
    for i in range(npl):
        d, fl, mask, x, y, z, yaw, pitch, roll, var, ni, perm = struct.unpack_from(
            PLACEMENT_FMT, data, o_pl + i * PLACEMENT_REC)
        pls.append(dict(def_index=d, flags=fl, bsp_mask=mask, pos=(x, y, z), rot=(yaw, pitch, roll),
                        variant=var.split(b"\0", 1)[0].decode("latin1"), name_index=ni, permutation=perm))
    allc = []
    for i in range(nlights):
        v = struct.unpack_from(LIGHT_FMT, data, o_li + i * LIGHT_REC)
        allc.append(dict(ambient=v[0:3], sun=v[3:6], sun_dir=tuple(c / 32767.0 for c in v[6:9])))
    lights = []
    for b in range(nb):
        first, n = struct.unpack_from("<II", data, o_tab + b * BSPTAB_REC)
        if first + n > nlights:
            raise ScenError("cluster light range outside the table")
        lights.append(allc[first:first + n])
    pv = struct.unpack_from(PLAYER_FMT, data, o_pla)
    player = dict(fp_arms_model_tag=pv[0], fp_arms_anim_tag=pv[1], weapon_def=pv[2], weapon_fp_model_tag=pv[3],
                  weapon_fp_anim_tag=pv[4], biped_def=pv[5], arms_idle_anim=pv[6], weapon_idle_anim=pv[7],
                  fp_offset=pv[8:11], weapon_hand_marker=pv[11].split(b"\0", 1)[0].decode("latin1"))
    return Section(size, defs, pls, lights, player, list(struct.unpack_from("<10I", data, 0x38)))


def build_section(map_path: Path, bsp_filter: list[int] | None = None, extra: dict[int, str] | None = None) -> bytes:
    """The SCEN section of a decompressed map. ``bsp_filter`` restricts the (expensive) lighting probes to those
    BSP indices (the others store no probes and read back as the neutral default); placements and their BSP masks
    always cover every BSP of the map."""
    m = texconv.MapData.open(map_path)
    probes = load_bsps(m)
    objs = collect_objects(m, probes, extra)
    lights = map_lights(m, bsp_filter) if probes else []
    return build(objs.defs, objs.placements, objs.player, lights, len(probes))


# ---- reporting ----------------------------------------------------------------------------------
def describe(m: texconv.MapData, objs: ObjectSet, lights: list[list[ClusterLight]] | None = None) -> str:
    r = Region(m)
    path = lambda tid: r.entry(tid).path if r.entry(tid) else f"#{tid:#x}"  # noqa: E731
    L = [f"{m.path.stem}: {len(objs.placements)} placements, {len(objs.defs)} definitions, "
         f"{objs.unresolved} unresolved"]
    L.append("  placements per type: " + ", ".join(f"{k} {v}" for k, v in objs.counts.items() if v))
    L.append("  skipped (counted only): sound_scenery " + str(objs.counts.get("sound_scenery", 0)))
    L.append("  BSP membership tiers: " + ", ".join(f"{k} {v}" for k, v in sorted(objs.mask_tiers.items())))
    L.append("  placements per BSP: " + " ".join(f"{i}:{c}" for i, c in enumerate(objs.bsp_counts)))
    L.append(f"  in no BSP: {sum(1 for p in objs.placements if p.bsp_mask == 0)}")
    idle = sum(1 for d in objs.defs if d.idle_anim != NO_ANIM)
    L.append(f"  definitions: {sum(1 for d in objs.defs if d.model_tag)} with a model, "
             f"{sum(1 for d in objs.defs if d.anim_tag)} with an animation graph, {idle} with an idle animation")
    L.append(f"  model tags ({len(objs.model_tags)}): " + ", ".join(path(t) for t in objs.model_tags[:6]) + " ...")
    L.append(f"  animation graphs ({len(objs.anim_tags)}): " + ", ".join(path(t) for t in objs.anim_tags))
    p = objs.player
    L.append("  player:")
    L.append(f"    biped {path(p.biped_tag) if p.biped_tag else '-'}, weapon {path(p.weapon_tag) if p.weapon_tag else '-'}")
    L.append(f"    arms model {path(p.fp_arms_model_tag) if p.fp_arms_model_tag else '-'}, "
             f"fp model {path(p.weapon_fp_model_tag) if p.weapon_fp_model_tag else '-'}, "
             f"fp graph {path(p.fp_arms_anim_tag) if p.fp_arms_anim_tag else '-'}")
    L.append(f"    idle anim arms/weapon {p.arms_idle_anim}/{p.weapon_idle_anim}, hand node {p.weapon_hand_marker!r}, "
             f"offset {p.fp_offset}" + (f", notes {p.notes}" if p.notes else ""))
    if lights is not None:
        for b, cl in enumerate(lights):
            if cl:
                amb = sorted(sum(c.ambient) // 3 for c in cl)
                L.append(f"  BSP {b}: {len(cl)} cluster lights, ambient grey min/median/max "
                         f"{amb[0]}/{amb[len(amb) // 2]}/{amb[-1]}")
    for pr in objs.problems[:10]:
        L.append(f"  problem: {pr}")
    return "\n".join(L)


def info_pack(path: Path) -> str:
    sec = parse_section(hp2.parse(path.read_bytes()).read("SCEN"))
    tc = ", ".join(f"{n} {c}" for n, c in zip(TYPE_NAMES, sec.type_counts) if c)
    return (f"{path.name}: SCEN {sec.size} bytes, {len(sec.defs)} defs, {len(sec.placements)} placements ({tc}), "
            f"{len(sec.lights)} BSPs with {sum(len(c) for c in sec.lights)} cluster lights, player {sec.player}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--bsp", type=int, action="append", help="lighting probes only for this BSP index (repeatable)")
    ap.add_argument("--info", action="store_true", help="print counts, tag sets and player data instead of writing")
    ap.add_argument("--lights", action="store_true", help="with --info on a map: also compute and summarise the probes")
    args = ap.parse_args(argv)
    if args.info and args.input.suffix == ".hp2":
        print(info_pack(args.input))
        return 0
    if args.info:
        m = texconv.MapData.open(args.input)
        objs = collect_objects(m, load_bsps(m))
        print(describe(m, objs, map_lights(m, args.bsp) if args.lights else None))
        return 0
    if not args.out:
        ap.error("-o is required")
    blob = build_section(args.input, args.bsp)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, [("SCEN", blob)]))
    print(f"wrote {args.out} (SCEN {len(blob) / 1024:.1f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
