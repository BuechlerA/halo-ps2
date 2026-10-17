#!/usr/bin/env python3
"""Convert the devices of an Xbox scenario and its markup (device groups, trigger volumes, cutscene flags and
camera points, player starting locations, the object-name table, sound scenery, decals, BSP switch volumes)
into the PS2 ``DEVS`` section, version 1 (docs/27-dev-section.md; reader engine/src/world/devdef.c, contract
engine/src/world/devdef.h; the pure device runtime is engine/src/sim/devices.c).

Everything below was read off the retail maps and checked on all ten campaign maps (a10 a30 a50 b30 b40 c10
c20 c40 d20 d40).  Offsets are inside the tag's own data block (tags) or the element (scenario blocks); a
reflexive is ``u32 count, u32 address, u32 pad``; a tag reference is ``char[4] group (byte-reversed), u32 path
pointer, u32 path length, u32 tag id``.  "verified" names the cross-check; "unverified" fields are read from
the data and carried raw.

Device tags (``mach`` ``ctrl`` ``lifi``; object part as in scenconv: type +0x00, model +0x28, animation graph
+0x38; the object struct ends at +0x17C)
    device part, common (verified: ranges on 120 mach, 17 ctrl and 19 lifi tags of the ten maps)
    +0x17C u32 flags (bit 0 position loops, bit 1 position not interpolated; only 0..2 seen)
    +0x180..+0x194 six f32 SECONDS: power transition, power acceleration, position transition, position
           acceleration, depowered position transition, depowered position acceleration.  verified: the
           engine's derived inverse rates at +0x274.. equal 1 / (30 t) for these values (ticks = 30 t), and
           machines' +0x320 ticks field equals round(30 x door open time) on all 120 machine tags.
    +0x198 u16 x 4 inputs A B C D of the device's object functions (enum 0 none, 1 power, 2 change in power,
           3 position, 4 change in position, 5 locked, 6 delay; values 0..6 seen; the NAMES are unverified)
    +0x1A0 six tag references: open (up), close (down), opened, closed, depowered, repowered (verified: groups
           ``snd!`` or ``effe`` or null on every tag), +0x200 f32 delay time (s), +0x20C delay effect reference,
           +0x21C f32 automatic activation radius (world units; non-zero only on door-type machines: verified)
    +0x274..+0x28C derived per-tick rates (ignored), +0x290 .. device-kind specific part
    machine (struct 0x324 B): +0x290 u16 type (0 door, 1 platform, 2 gear: verified, door-like radius and
           open time only on type 0), +0x292 u16 flags (bit 0 pathfinding obstacle, bit 1 but not when open,
           bit 2 elevator: bit 2 occurs only on platforms (verified); the names of 0 and 1 are unverified),
           +0x294 f32 door open time (s), +0x2E8 u16 collision response (0 / 1), +0x2EA u16 elevator node (0..2),
           +0x320 u32 door open time in ticks (derived; checked against +0x294)
    control (struct 0x318 B): +0x290 u16 type (0 toggle switch, 1 on button, 2 off button, 3 call button; all
           17 tags of the ten maps hold 0: only the zero value is verified), +0x292 u16 triggers-when (all 0),
           +0x294 f32 call value (all 0), +0x2E8 / +0x2F8 / +0x308 three references (on, off, deny effects)
    light fixture (struct 0x2D0 B): nothing beyond the device part; the light itself is an attachment (a
           ``ligh`` tag + marker name) in the tag's attachments block (+0x140), not read here
    device animation graph (``antr`` +0x30 devices, 96 B): +0x54 reflexive of i16 slots, slot 0 = position
           (a one-animation OVERLAY of 2..many frames: position 0..1 maps to frame 0..frames-1; verified on 88
           machine and 5 control graphs by name and overlay type), slot 1 = power (never present on the maps).

Scenario blocks (verified: stride = distance to the next non-empty block / count on all ten maps)
    +0x204 object names (36 B: char[32], u16 object type, u16 index into that type's placement block)
    +0x288 device groups (52 B): char[32] name, f32 initial value (0..1), u32 flags (bit 0 "can change only
           once"; only 0 / 1 seen), 12 zero bytes
    +0x294 machines (64 B), +0x2AC controls (64 B), +0x2C4 light fixtures (88 B), +0x2DC sound scenery (40 B):
           placement header as in scenconv (+0x00 u16 palette, +0x02 u16 name, +0x04 u16 flags, +0x06 u16
           permutation, +0x08 f32 position[3], +0x14 f32 rotation[3]); then
           +0x28 i16 power group, +0x2A i16 position group (-1 none; verified: always < the group count),
           +0x2C u32 device flags (bit 0 initially open, 1 initially off, 2 can change only once, 3 position
           reversed, 4 usable from both sides: only bits 0..4 occur), machines +0x30 u32 flags (bit 0 does not
           operate automatically, 1 one sided, 2 never appears locked, 3 opened by melee attack; bits 4 / 5 = one
           sided for player / does not close automatically never occur; unverified names), controls +0x30 u32
           (0 / 1) and +0x34 u16 (0..4): meaning unknown, carried raw as ``kind_flags`` and ``extra``, light
           fixtures +0x30 f32 colour rgb, +0x3C f32 intensity, +0x40 f32 falloff angle, +0x44 f32 cutoff angle,
           +0x20 u16 manual BSP flags (scenconv), sound scenery: +0x20 8 bytes (all zero).
    +0x354 player starting locations (52 B): f32 position[3], f32 facing (yaw, radians), i16 team, i16 BSP index,
           i16 game type x 4, 20 zero bytes.  The BSP index is 0 on every location, also those that lie only in
           another BSP (b40: BSP 3; verified by point location): it is carried raw and ``bsp_mask`` is LOCATED.
    +0x360 trigger volumes (96 B): u32 1 (all 1060 volumes), char[32] name at +4, 12 zero bytes, f32 forward[3]
           +0x30, f32 up[3] +0x3C, f32 position[3] +0x48, f32 extents[3] +0x54.  verified: forward and up are
           unit and orthogonal on all 1060 volumes, extents are positive, and the box is
           ``position + s0 forward + s1 (up x forward) + s2 up`` for s_i in [0, extent_i]: with this convention
           the volumes that switch between two BSPs (+0x39C) are inside both BSPs for 50.7 % of random interior
           points, against 14..27 % for the centred and the mirrored conventions (see the doc)
    +0x39C BSP switch trigger volumes (8 B): u16 trigger volume, u16 source BSP, u16 destination BSP, u16 unknown
           (0xFFFF or a small index, carried raw; verified: volume < volume count)
    +0x3A8 decals (16 B): u16 palette index, i8 yaw, i8 pitch, f32 position[3]; +0x3B4 decal palette (16 B: a
           ``deca`` tag reference).  The angles are signed bytes of 2 pi / 256 (the direction
           (cos p cos y, cos p sin y, sin p) is the surface normal: median |dot| 0.98 against the nearest BSP
           triangle on a10, see the doc)
    +0x4E4 cutscene flags (92 B): u32 0, char[32] name at +4, f32 position[3] +0x24, f32 yaw, f32 pitch +0x30,
           36 zero bytes (verified: 1012 of 1025 positions are inside a BSP, 9 within 0.5 wu)
    +0x4F0 cutscene camera points (104 B): u32 0, char[32] name at +4, u32 0, f32 position[3] +0x28,
           f32 yaw, pitch, roll +0x34, f32 field of view (radians) +0x40, 24 zero bytes (verified: 926 of 962
           inside a BSP; field of view 70 degrees on 959)

``DEVS`` section, version 1 (little endian; every table 16-aligned; offsets from the section start)
    header (128 B)  "DEVS" | u16 version = 1 | u16 0 | u32 total size | u32 counts x 12 (defs, devices, groups,
                    volumes, starts, cutscene flags, cameras, names, ambients, decals, decal types, bsp switches)
                    at 0x0C | u32 bsp count 0x3C | u32 offsets x 13 at 0x40 (the tables in the order of the counts,
                    then the name order table after names: defs, devices, groups, volumes, starts, flags, cameras,
                    names, name_order, ambients, decals, decal_types, bspsw) | u32 dropped devices 0x74 |
                    u32 dangling names 0x78 | zero
    def 160 B       u32 tag | u32 model | u32 anim | u8 kind (0 machine, 1 control, 2 light fixture) | u8 subtype |
                    u16 dev_flags | u16 kind_flags | u16 triggers | u32 ticks x 6 (power, power acc, position,
                    position acc, depowered, depowered acc) | f32 auto_radius | u32 delay_ticks | u32 open_ticks |
                    u8 fn_in x 4 | u32 fx x 10 (open, close, opened, closed, depowered, repowered, delay, ctl on,
                    ctl off, ctl deny: tag ids, 0 none) | u16 fx_snd_mask (bit i: fx[i] is a ``snd!`` tag, else an
                    ``effe`` tag) | u16 0 | u16 pos_anim | u16 power_anim (0xFFFF none) | u16 pos_frames |
                    u16 power_frames | u8 collision | u8 elevator_node | u16 0 | f32 call_value | char[32] name | 8
    device 80 B     u16 def | u16 flags (SCEN style: bit 0 initially visible, bits 1..3 not on easy / normal / hard,
                    bit 4 player appearance) | u32 bsp_mask | f32 pos[3] | f32 rot[3] | i16 power_group |
                    i16 position_group | u32 device_flags | u32 kind_flags | i16 name_index | u16 extra |
                    u32 scen_index (index in SCEN placements) | f32 colour[3], intensity, falloff, cutoff | 4
    group 48 B      char[32] name | f32 initial | u32 flags | 8
    volume 96 B     char[32] name | f32 forward[3] | f32 left[3] (= up x forward) | f32 up[3] | f32 position[3] |
                    f32 extents[3] | 4
    start 32 B      f32 pos[3] | f32 facing | i16 team | i16 bsp | i16 game type x 4 | u32 bsp_mask (located)
    cflag 64 B      char[32] name | f32 pos[3] | f32 yaw | f32 pitch | u32 bsp_mask | 8
    camera 64 B     char[32] name | f32 pos[3] | f32 yaw, pitch, roll | f32 fov | u32 bsp_mask
    name 48 B       char[32] name | u8 object type | u8 scen_type | u8 flags (bit 0: a placement carries the name,
                    bit 1: it is a device, bit 2: it is sound scenery) | u8 0 | u16 table_index (the tag's
                    placement index in its own kind) | u16 0 | u32 scen_index | u32 aux_index (device or ambient
                    index), both 0xFFFFFFFF when none
    name_order      u16 x names: indices sorted by name bytes (binary search)
    ambient 48 B    u32 ssce tag | u32 looping sound tag | f32 pos[3] | f32 rot[3] | i16 name_index | u16 flags |
                    u32 bsp_mask | u32 raw x 2
    decal 32 B      u16 type | i8 yaw_raw | i8 pitch_raw | f32 pos[3] | f32 yaw | f32 pitch | u32 bsp_mask | 4
    decal types     u32 ``deca`` tag id (0 none) per palette entry
    bspsw 8 B       u16 volume | u16 source | u16 destination | u16 unknown

Usage:
    python3 -I tools/halo2ps2/devconv.py assets/decompressed/a10.map -o assets/hp2/a10.dev.hp2
    python3 -I tools/halo2ps2/devconv.py --info assets/decompressed/a10.map
    python3 -I tools/halo2ps2/devconv.py --info assets/hp2/a10.dev.hp2
    python3 -I tools/halo2ps2/devconv.py --verify assets/decompressed/a10.map     # the two conventions of the doc
"""
from __future__ import annotations

import argparse
import math
import random
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bsp, bspconv, hp2, scenconv, tagindex, texconv  # noqa: E402

MAGIC = b"DEVS"
VERSION = 1
HEADER_SIZE = 128
NAME_LEN = 32
NONE32 = 0xFFFFFFFF
NONE16 = 0xFFFF
TICKS_PER_SECOND = 30
MAX_TICKS = 30 * 3600                    # a transition longer than an hour is corrupt
ALIGN = 16

DEF_REC, DEVICE_REC, GROUP_REC, VOLUME_REC, START_REC = 160, 80, 48, 96, 32
CFLAG_REC, CAMERA_REC, NAME_REC, AMBIENT_REC, DECAL_REC, BSPSW_REC = 64, 64, 48, 48, 32, 8
TABLES = ("defs", "devices", "groups", "volumes", "starts", "cflags", "cameras", "names", "name_order",
          "ambients", "decals", "decal_types", "bspsw")
RECORDS = {"defs": DEF_REC, "devices": DEVICE_REC, "groups": GROUP_REC, "volumes": VOLUME_REC,
           "starts": START_REC, "cflags": CFLAG_REC, "cameras": CAMERA_REC, "names": NAME_REC,
           "name_order": 2, "ambients": AMBIENT_REC, "decals": DECAL_REC, "decal_types": 4, "bspsw": BSPSW_REC}
COUNT_KEYS = ("defs", "devices", "groups", "volumes", "starts", "cflags", "cameras", "names", "ambients",
              "decals", "decal_types", "bspsw")
MAX_BSPS = 32
MAX_RECORDS = 1 << 20

# device kinds (DEVS def.kind)
K_MACHINE, K_CONTROL, K_LIGHT = 0, 1, 2
KIND_NAMES = ("machine", "control", "light_fixture")
MACHINE_TYPES = ("door", "platform", "gear")
CONTROL_TYPES = ("toggle", "on", "off", "call")
# placement flags, as scenconv
P_INITIALLY_VISIBLE = scenconv.P_INITIALLY_VISIBLE
# name flags
NF_RESOLVED, NF_DEVICE, NF_AMBIENT = 1, 2, 4
# device placement flags (scenario +0x2C)
DF_INITIALLY_OPEN, DF_INITIALLY_OFF, DF_CHANGE_ONCE, DF_REVERSED, DF_BOTH_SIDES = 1, 2, 4, 8, 16

# tag layout
OBJ_TYPE, OBJ_MODEL, OBJ_ANIM = 0x00, 0x28, 0x38
DEV_FLAGS, DEV_TIMES, DEV_FN, DEV_FX, DEV_DELAY, DEV_DELAY_FX, DEV_RADIUS = 0x17C, 0x180, 0x198, 0x1A0, 0x200, 0x20C, 0x21C
MACH_TYPE, MACH_FLAGS, MACH_OPEN, MACH_COLL, MACH_ELEV, MACH_TICKS = 0x290, 0x292, 0x294, 0x2E8, 0x2EA, 0x320
CTRL_TYPE, CTRL_TRIG, CTRL_CALL, CTRL_FX = 0x290, 0x292, 0x294, 0x2E8
STRUCT_SIZE = {K_MACHINE: 0x324, K_CONTROL: 0x318, K_LIGHT: 0x2D0}
OBJECT_TYPE = {K_MACHINE: 7, K_CONTROL: 8, K_LIGHT: 9}
GROUP_OF = {K_MACHINE: "mach", K_CONTROL: "ctrl", K_LIGHT: "lifi"}
ANTR_DEVICES, ANTR_ANIMS, ANTR_DEVICE_REC, ANTR_ANIM_REC, DEVICE_SLOTS = 0x30, 0x74, 96, 180, 0x54

# scenario layout
SCN_NAMES, SCN_GROUPS, SCN_STARTS, SCN_VOLUMES, SCN_BSPSW = 0x204, 0x288, 0x354, 0x360, 0x39C
SCN_DECALS, SCN_DECAL_PALETTE, SCN_CFLAGS, SCN_CAMERAS = 0x3A8, 0x3B4, 0x4E4, 0x4F0
NAME_SRC_REC, GROUP_SRC_REC, START_SRC_REC, VOLUME_SRC_REC, BSPSW_SRC_REC = 36, 52, 52, 96, 8
DECAL_SRC_REC, DECAL_PAL_REC, CFLAG_SRC_REC, CAMERA_SRC_REC = 16, 16, 92, 104
DEVICE_BLOCKS = ((K_MACHINE, "machine", 0x294, 64, 0x2A0, "mach"), (K_CONTROL, "control", 0x2AC, 64, 0x2B8, "ctrl"),
                 (K_LIGHT, "light_fixture", 0x2C4, 88, 0x2D0, "lifi"))
SSCE_BLOCK, SSCE_REC, SSCE_PALETTE, SSCE_SOUND = 0x2DC, 40, 0x2E8, 0x1FC
DECAL_ANGLE = 2.0 * math.pi / 256.0
# object type value -> (scenconv kind name, scen_type)
OBJ_TYPE_KIND = {0: ("biped", scenconv.BIPED), 1: ("vehicle", scenconv.VEHICLE), 2: ("weapon", scenconv.WEAPON),
                 3: ("equipment", scenconv.EQUIPMENT), 6: ("scenery", scenconv.SCENERY),
                 7: ("machine", scenconv.MACHINE), 8: ("control", scenconv.CONTROL),
                 9: ("light_fixture", scenconv.LIGHT_FIXTURE), 11: ("sound_scenery", scenconv.OTHER)}
FX_SLOTS = ("open", "close", "opened", "closed", "depowered", "repowered", "delay", "on", "off", "deny")


class DevError(ValueError):
    pass


# ---- data model -------------------------------------------------------------------------------------------
@dataclass
class DevDef:
    tag_id: int
    kind: int
    name: str = ""
    model_tag: int = 0
    anim_tag: int = 0
    subtype: int = 0
    dev_flags: int = 0
    kind_flags: int = 0
    triggers: int = 0
    ticks: tuple[int, ...] = (0, 0, 0, 0, 0, 0)
    auto_radius: float = 0.0
    delay_ticks: int = 0
    open_ticks: int = 0
    fn_in: tuple[int, ...] = (0, 0, 0, 0)
    fx: tuple[int, ...] = (0,) * 10
    fx_snd_mask: int = 0
    pos_anim: int = NONE16
    power_anim: int = NONE16
    pos_frames: int = 0
    power_frames: int = 0
    collision: int = 0
    elevator_node: int = 0
    call_value: float = 0.0


@dataclass
class Device:
    tag_id: int
    kind: int
    flags: int
    pos: tuple[float, float, float]
    rot: tuple[float, float, float]
    power_group: int = -1
    position_group: int = -1
    device_flags: int = 0
    kind_flags: int = 0
    name_index: int = -1
    extra: int = 0
    bsp_mask: int = 0
    scen_index: int = NONE32
    light: tuple[float, ...] = (0.0,) * 6        # colour rgb, intensity, falloff, cutoff
    def_index: int = 0


@dataclass
class Group:
    name: str
    initial: float
    flags: int


@dataclass
class Volume:
    name: str
    forward: tuple[float, float, float]
    up: tuple[float, float, float]
    position: tuple[float, float, float]
    extents: tuple[float, float, float]

    @property
    def left(self) -> tuple[float, float, float]:
        return cross(self.up, self.forward)


@dataclass
class Start:
    pos: tuple[float, float, float]
    facing: float
    team: int
    bsp: int
    types: tuple[int, int, int, int]
    bsp_mask: int = 0


@dataclass
class CutFlag:
    name: str
    pos: tuple[float, float, float]
    yaw: float
    pitch: float
    bsp_mask: int = 0


@dataclass
class Camera:
    name: str
    pos: tuple[float, float, float]
    ori: tuple[float, float, float]
    fov: float
    bsp_mask: int = 0


@dataclass
class NameEntry:
    name: str
    obj_type: int
    table_index: int
    scen_type: int = scenconv.OTHER
    flags: int = 0
    scen_index: int = NONE32
    aux_index: int = NONE32


@dataclass
class Ambient:
    ssce_tag: int
    sound_tag: int
    pos: tuple[float, float, float]
    rot: tuple[float, float, float]
    name_index: int
    flags: int
    bsp_mask: int = 0
    raw: tuple[int, int] = (0, 0)


@dataclass
class Decal:
    type: int
    yaw_raw: int
    pitch_raw: int
    pos: tuple[float, float, float]
    bsp_mask: int = 0

    @property
    def yaw(self) -> float:
        return self.yaw_raw * DECAL_ANGLE

    @property
    def pitch(self) -> float:
        return self.pitch_raw * DECAL_ANGLE


@dataclass
class BspSwitch:
    volume: int
    source: int
    dest: int
    unknown: int


@dataclass
class DevSet:
    defs: list[DevDef] = field(default_factory=list)
    devices: list[Device] = field(default_factory=list)
    groups: list[Group] = field(default_factory=list)
    volumes: list[Volume] = field(default_factory=list)
    starts: list[Start] = field(default_factory=list)
    cflags: list[CutFlag] = field(default_factory=list)
    cameras: list[Camera] = field(default_factory=list)
    names: list[NameEntry] = field(default_factory=list)
    ambients: list[Ambient] = field(default_factory=list)
    decal_types: list[int] = field(default_factory=list)
    decals: list[Decal] = field(default_factory=list)
    bspsw: list[BspSwitch] = field(default_factory=list)
    bsp_count: int = 0
    dropped_devices: int = 0
    dangling_names: int = 0
    problems: list[str] = field(default_factory=list)
    scen_count: int = 0                       # placements of the SCEN section the indices refer to


def cross(a, b) -> tuple[float, float, float]:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def volume_contains(v: Volume, p) -> bool:
    """Reference point test (the C ``volume_contains`` is tested against an independent copy of this)."""
    d = (p[0] - v.position[0], p[1] - v.position[1], p[2] - v.position[2])
    for axis, ext in zip((v.forward, v.left, v.up), v.extents):
        s = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2]
        if s < 0.0 or s > ext:
            return False
    return True


def _finite(*vals) -> bool:
    return all(math.isfinite(v) for v in vals)


def _tail(path: str) -> str:
    return scenconv._tail(path)


# ---- tag extents -------------------------------------------------------------------------------------------
def tag_extents(m: texconv.MapData) -> dict[int, int]:
    """Bytes of every tag's data block: up to the next block (the last one to the end of the tag region)."""
    ents = sorted((e for e in m.index.entries if e.data_address >= m.base and e.group != "sbsp"),
                  key=lambda e: e.data_address)
    out: dict[int, int] = {}
    for a, b in zip(ents, ents[1:]):
        out[a.tag_id] = b.data_address - a.data_address
    if ents:
        out[ents[-1].tag_id] = m.base + len(m.region) - ents[-1].data_address
    return out


# ---- device tags -------------------------------------------------------------------------------------------
def _ticks(seconds: float, what: str, problems: list[str]) -> int:
    if not math.isfinite(seconds) or seconds < 0.0 or seconds * TICKS_PER_SECOND > MAX_TICKS:
        problems.append(f"{what}: time {seconds!r} outside 0..3600 s")
        return 0
    return int(round(seconds * TICKS_PER_SECOND))


def device_animations(r: scenconv.Region, graph_block: int, problems: list[str], what: str) -> tuple[int, int, int, int]:
    """(position anim, power anim, position frames, power frames) of a device animation graph (antr +0x30)."""
    n, o = r.reflexive(graph_block + ANTR_DEVICES, ANTR_DEVICE_REC, 64)
    if not n:
        return NONE16, NONE16, 0, 0
    sn, so = r.reflexive(o + DEVICE_SLOTS, 2, 64)
    slots = list(r.u(f"<{sn}h", so)) if sn else []
    an, ao = r.reflexive(graph_block + ANTR_ANIMS, ANTR_ANIM_REC, 4096)
    out = []
    for k in (0, 1):
        idx = slots[k] if k < len(slots) else -1
        if 0 <= idx < an:
            frames = r.u("<h", ao + idx * ANTR_ANIM_REC + 0x22)[0]
            out.append((idx, max(0, frames)))
        else:
            if idx >= an:
                problems.append(f"{what}: device slot {k} animation {idx} >= {an}")
            out.append((NONE16, 0))
    return out[0][0], out[1][0], out[0][1], out[1][1]


def read_def(r: scenconv.Region, ext: dict[int, int], e: tagindex.TagEntry, kind: int,
             problems: list[str]) -> DevDef | None:
    """One device tag, or None (with a problem note) when it is too short or has a wrong object type."""
    size = ext.get(e.tag_id, 0)
    if size < STRUCT_SIZE[kind]:
        problems.append(f"{e.path}: tag block {size} bytes, a {KIND_NAMES[kind]} needs {STRUCT_SIZE[kind]}")
        return None
    b = r.block(e)
    try:
        d = DevDef(e.tag_id, kind, _tail(e.path))
        if r.u("<H", b + OBJ_TYPE)[0] != OBJECT_TYPE[kind]:
            problems.append(f"{e.path}: object type {r.u('<H', b)[0]}, expected {OBJECT_TYPE[kind]}")
        mg, mid = r.tagref(b + OBJ_MODEL)
        ag, aid = r.tagref(b + OBJ_ANIM)
        if mg == "mode" and r.entry(mid) is not None:
            d.model_tag = mid
        if ag == "antr" and r.entry(aid) is not None:
            d.anim_tag = aid
            d.pos_anim, d.power_anim, d.pos_frames, d.power_frames = device_animations(
                r, r.block(r.entry(aid)), problems, e.path)
        d.dev_flags = r.u("<I", b + DEV_FLAGS)[0] & 0xFFFF
        times = r.u("<6f", b + DEV_TIMES)
        d.ticks = tuple(_ticks(t, f"{e.path}: time {i}", problems) for i, t in enumerate(times))
        d.fn_in = tuple(min(255, v) for v in r.u("<4H", b + DEV_FN))
        delay = r.u("<f", b + DEV_DELAY)[0]
        d.delay_ticks = _ticks(delay, f"{e.path}: delay", problems)
        radius = r.u("<f", b + DEV_RADIUS)[0]
        if _finite(radius) and radius >= 0.0:
            d.auto_radius = radius
        else:
            problems.append(f"{e.path}: activation radius {radius!r}")
        fx = [0] * 10
        mask = 0
        refs = [(i, b + DEV_FX + 16 * i) for i in range(6)] + [(6, b + DEV_DELAY_FX)]
        if kind == K_CONTROL:
            refs += [(7 + i, b + CTRL_FX + 16 * i) for i in range(3)]
        for slot, off in refs:
            g, tid = r.tagref(off)
            if g is None:
                continue
            if g not in ("snd!", "effe") or r.entry(tid) is None:
                problems.append(f"{e.path}: {FX_SLOTS[slot]} reference of group {g!r} not usable")
                continue
            fx[slot] = tid
            if g == "snd!":
                mask |= 1 << slot
        d.fx, d.fx_snd_mask = tuple(fx), mask
        if kind == K_MACHINE:
            d.subtype = r.u("<H", b + MACH_TYPE)[0]
            d.kind_flags = r.u("<H", b + MACH_FLAGS)[0]
            d.open_ticks = _ticks(r.u("<f", b + MACH_OPEN)[0], f"{e.path}: door open time", problems)
            tk = r.u("<I", b + MACH_TICKS)[0]
            if tk != d.open_ticks:
                problems.append(f"{e.path}: door open ticks {tk} != 30 x open time ({d.open_ticks})")
            d.collision = min(255, r.u("<H", b + MACH_COLL)[0])
            d.elevator_node = min(255, r.u("<H", b + MACH_ELEV)[0])
            if d.subtype > 2:
                problems.append(f"{e.path}: machine type {d.subtype}")
        elif kind == K_CONTROL:
            d.subtype = r.u("<H", b + CTRL_TYPE)[0]
            d.triggers = r.u("<H", b + CTRL_TRIG)[0]
            call = r.u("<f", b + CTRL_CALL)[0]
            d.call_value = call if _finite(call) else 0.0
            if d.subtype > 3:
                problems.append(f"{e.path}: control type {d.subtype}")
        return d
    except scenconv.ScenError as ex:
        problems.append(f"{e.path}: {ex}")
        return None


# ---- the SCEN placement order ---------------------------------------------------------------------------------
def scen_indices(r: scenconv.Region, problems: list[str]) -> tuple[dict[str, list[int | None]], int]:
    """Per scenario placement block (kind name): the index each placement has in the SCEN section, None for the ones
    scenconv drops (unresolved palette entry, non-finite transform) and for the skipped sound scenery.  Replicates the
    loop of ``scenconv.collect_objects`` (the converter cross-checks the count against it).  Also returns the
    number of placements."""
    sb = r.block(r.m.index.scenario)
    dt = scenconv.DefTable(r, [])
    out: dict[str, list[int | None]] = {}
    cursor = 0
    for kind, off, stride, pal_off, group, typ, objtype in scenconv.KINDS:
        n, o = r.reflexive(sb + off, stride)
        if typ is None or n == 0:
            out[kind] = [None] * n
            continue
        pn, po = r.reflexive(sb + pal_off, scenconv.PALETTE_REC, 4096)
        palette = []
        for i in range(pn):
            g, tid = r.tagref(po + i * scenconv.PALETTE_REC)
            palette.append(dt.get(tid, typ, group, objtype) if g == group else None)
        lst: list[int | None] = []
        for i in range(n):
            e = o + i * stride
            pi = r.u("<H", e)[0]
            vals = r.u("<3f", e + 8) + r.u("<3f", e + 0x14)
            d = palette[pi] if pi < pn else None
            if d is None or not _finite(*vals):
                lst.append(None)
            else:
                lst.append(cursor)
                cursor += 1
        out[kind] = lst
    return out, cursor


# ---- scenario markup ---------------------------------------------------------------------------------------
def _name(r: scenconv.Region, off: int) -> str:
    return r.cstr(off, NAME_LEN)[:NAME_LEN - 1]


def read_groups(r: scenconv.Region, sb: int, problems: list[str]) -> list[Group]:
    n, o = r.reflexive(sb + SCN_GROUPS, GROUP_SRC_REC, 4096)
    out = []
    for i in range(n):
        e = o + i * GROUP_SRC_REC
        init, flags = r.u("<fI", e + 32)
        if not _finite(init) or not 0.0 <= init <= 1.0:
            problems.append(f"device group {i}: initial value {init!r}")
            init = min(1.0, max(0.0, init)) if _finite(init) else 0.0
        out.append(Group(_name(r, e), init, flags))
    return out


def read_volumes(r: scenconv.Region, sb: int, problems: list[str]) -> list[Volume]:
    n, o = r.reflexive(sb + SCN_VOLUMES, VOLUME_SRC_REC, 65535)
    out = []
    zero = (0.0, 0.0, 0.0)
    for i in range(n):
        e = o + i * VOLUME_SRC_REC
        fw, up = r.u("<3f", e + 0x30), r.u("<3f", e + 0x3C)
        pos, ext = r.u("<3f", e + 0x48), r.u("<3f", e + 0x54)
        if not _finite(*fw, *up, *pos, *ext) or min(ext) < 0.0:
            problems.append(f"trigger volume {i}: not finite or negative extents")
            out.append(Volume(_name(r, e + 4), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), zero, zero))   # keeps the indices
        else:
            out.append(Volume(_name(r, e + 4), fw, up, pos, ext))
    return out


def read_starts(r: scenconv.Region, sb: int, probes, problems: list[str]) -> list[Start]:
    n, o = r.reflexive(sb + SCN_STARTS, START_SRC_REC, 256)
    out = []
    for i in range(n):
        e = o + i * START_SRC_REC
        pos = r.u("<3f", e)
        facing = r.u("<f", e + 12)[0]
        team, bsp_idx, *types = r.u("<6h", e + 16)
        if not _finite(*pos, facing):
            problems.append(f"starting location {i}: not finite")
            pos, facing = (0.0, 0.0, 0.0), 0.0
        mask = scenconv.locate_mask(probes, pos)[0] if probes else 0
        out.append(Start(pos, facing, team, bsp_idx, tuple(types), mask))        # type: ignore[arg-type]
    return out


def read_cflags(r: scenconv.Region, sb: int, probes, problems: list[str]) -> list[CutFlag]:
    n, o = r.reflexive(sb + SCN_CFLAGS, CFLAG_SRC_REC, 4096)
    out = []
    for i in range(n):
        e = o + i * CFLAG_SRC_REC
        pos = r.u("<3f", e + 0x24)
        yaw, pitch = r.u("<2f", e + 0x30)
        if not _finite(*pos, yaw, pitch):
            problems.append(f"cutscene flag {i}: not finite")
            pos, yaw, pitch = (0.0, 0.0, 0.0), 0.0, 0.0
        out.append(CutFlag(_name(r, e + 4), pos, yaw, pitch, scenconv.locate_mask(probes, pos)[0] if probes else 0))
    return out


def read_cameras(r: scenconv.Region, sb: int, probes, problems: list[str]) -> list[Camera]:
    n, o = r.reflexive(sb + SCN_CAMERAS, CAMERA_SRC_REC, 4096)
    out = []
    for i in range(n):
        e = o + i * CAMERA_SRC_REC
        pos = r.u("<3f", e + 0x28)
        ori = r.u("<3f", e + 0x34)
        fov = r.u("<f", e + 0x40)[0]
        if not _finite(*pos, *ori, fov):
            problems.append(f"camera point {i}: not finite")
            pos, ori, fov = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0
        out.append(Camera(_name(r, e + 4), pos, ori, fov, scenconv.locate_mask(probes, pos)[0] if probes else 0))
    return out


def read_decals(r: scenconv.Region, sb: int, probes, problems: list[str]) -> tuple[list[int], list[Decal]]:
    pn, po = r.reflexive(sb + SCN_DECAL_PALETTE, DECAL_PAL_REC, 4096)
    types = []
    for i in range(pn):
        g, tid = r.tagref(po + i * DECAL_PAL_REC)
        types.append(tid if g == "deca" and r.entry(tid) is not None else 0)
    n, o = r.reflexive(sb + SCN_DECALS, DECAL_SRC_REC, 1 << 16)
    out = []
    for i in range(n):
        e = o + i * DECAL_SRC_REC
        typ, yaw, pitch = r.u("<Hbb", e)
        pos = r.u("<3f", e + 4)
        if typ >= pn or not _finite(*pos):
            problems.append(f"decal {i}: type {typ} of {pn} or not finite")
            continue
        out.append(Decal(typ, yaw, pitch, pos, scenconv.locate_mask(probes, pos)[0] if probes else 0))
    return types, out


def read_bspsw(r: scenconv.Region, sb: int, volumes: int, bsps: int, problems: list[str]) -> list[BspSwitch]:
    n, o = r.reflexive(sb + SCN_BSPSW, BSPSW_SRC_REC, 4096)
    out = []
    for i in range(n):
        v, s, d, u = r.u("<4H", o + i * BSPSW_SRC_REC)
        if v >= volumes or (bsps and (s >= bsps or d >= bsps)):
            problems.append(f"BSP switch {i}: volume {v} / BSPs {s} -> {d} out of range")
            continue
        out.append(BspSwitch(v, s, d, u))
    return out


# ---- collecting ----------------------------------------------------------------------------------------------
def collect(m: texconv.MapData, probes: list) -> DevSet:
    """Everything of the DEVS section. ``probes`` (``scenconv.load_bsps``) may be empty: every BSP mask is then 0.
    A tag block that does not fit the tag region raises ``DevError``."""
    try:
        return _collect(m, probes)
    except scenconv.ScenError as ex:
        raise DevError(str(ex)) from None


def _collect(m: texconv.MapData, probes: list) -> DevSet:
    r = scenconv.Region(m)
    ds = DevSet(bsp_count=len(probes))
    problems = ds.problems
    sb = r.block(m.index.scenario)
    ext = tag_extents(m)
    ds.groups = read_groups(r, sb, problems)
    ngroups = len(ds.groups)
    ds.volumes = read_volumes(r, sb, problems)
    ds.starts = read_starts(r, sb, probes, problems)
    ds.cflags = read_cflags(r, sb, probes, problems)
    ds.cameras = read_cameras(r, sb, probes, problems)
    ds.decal_types, ds.decals = read_decals(r, sb, probes, problems)
    ds.bspsw = read_bspsw(r, sb, len(ds.volumes), len(probes), problems)
    sidx, ds.scen_count = scen_indices(r, problems)
    nnames = r.reflexive(sb + SCN_NAMES, NAME_SRC_REC, 4096)[0]

    # devices, kind by kind in scenario order
    defs: dict[int, DevDef] = {}
    src_to_dev: dict[tuple[str, int], int] = {}
    for kind, kname, off, stride, pal_off, group in DEVICE_BLOCKS:
        n, o = r.reflexive(sb + off, stride)
        if n == 0:
            continue
        pn, po = r.reflexive(sb + pal_off, scenconv.PALETTE_REC, 4096)
        palette: list[DevDef | None] = []
        for i in range(pn):
            g, tid = r.tagref(po + i * scenconv.PALETTE_REC)
            e = r.entry(tid) if g == group else None
            if e is None:
                palette.append(None)
                continue
            if tid not in defs:
                d = read_def(r, ext, e, kind, problems)
                if d is None:
                    palette.append(None)
                    continue
                defs[tid] = d
            palette.append(defs[tid])
        for i in range(n):
            e = o + i * stride
            pi, ni, raw = r.u("<3H", e)
            pos, rot = r.u("<3f", e + 8), r.u("<3f", e + 0x14)
            d = palette[pi] if pi < pn else None
            if d is None or not _finite(*pos, *rot) or sidx[kname][i] is None:
                ds.dropped_devices += 1
                continue
            pg, qg = r.u("<2h", e + 0x28)
            dflags = r.u("<I", e + 0x2C)[0]
            if pg != -1 and not 0 <= pg < ngroups:
                problems.append(f"{kname} {i}: power group {pg} of {ngroups}")
                pg = -1
            if qg != -1 and not 0 <= qg < ngroups:
                problems.append(f"{kname} {i}: position group {qg} of {ngroups}")
                qg = -1
            manual = r.u("<H", e + 0x20)[0] if kind == K_LIGHT else 0
            mask = scenconv.locate_mask(probes, pos, manual)[0] if probes else 0
            dev = Device(d.tag_id, kind, (raw & 0x1E) | (0 if raw & 1 else P_INITIALLY_VISIBLE), pos, rot, pg, qg,
                         dflags, 0, ni if ni < nnames else -1, 0, mask, sidx[kname][i])        # type: ignore[arg-type]
            if kind == K_MACHINE:
                dev.kind_flags = r.u("<I", e + 0x30)[0]
            elif kind == K_CONTROL:
                dev.kind_flags = r.u("<I", e + 0x30)[0]
                dev.extra = r.u("<H", e + 0x34)[0]
            else:
                lt = r.u("<6f", e + 0x30)
                dev.light = lt if _finite(*lt) else (0.0,) * 6
                if not _finite(*lt):
                    problems.append(f"light fixture {i}: colour / angles not finite")
            ds.devices.append(dev)
            src_to_dev[(kname, i)] = len(ds.devices) - 1
    order = sorted(defs)
    index = {t: k for k, t in enumerate(order)}
    ds.defs = [defs[t] for t in order]
    for dev in ds.devices:
        dev.def_index = index[dev.tag_id]

    # sound scenery (skipped by SCEN: looping sound reference of the palette tag)
    sn, so = r.reflexive(sb + SSCE_BLOCK, SSCE_REC)
    src_to_amb: dict[int, int] = {}
    if sn:
        pn, po = r.reflexive(sb + SSCE_PALETTE, scenconv.PALETTE_REC, 4096)
        palette_snd: list[tuple[int, int] | None] = []
        for i in range(pn):
            g, tid = r.tagref(po + i * scenconv.PALETTE_REC)
            e = r.entry(tid) if g == "ssce" else None
            if e is None or ext.get(tid, 0) < SSCE_SOUND + 16:
                palette_snd.append(None)
                continue
            sg, sid = r.tagref(r.block(e) + SSCE_SOUND)
            palette_snd.append((tid, sid if sg in ("lsnd", "snd!") and r.entry(sid) is not None else 0))
        for i in range(sn):
            e = so + i * SSCE_REC
            pi, ni, raw = r.u("<3H", e)
            pos, rot = r.u("<3f", e + 8), r.u("<3f", e + 0x14)
            p = palette_snd[pi] if pi < pn else None
            if p is None or not _finite(*pos, *rot):
                continue
            mask = scenconv.locate_mask(probes, pos)[0] if probes else 0
            src_to_amb[i] = len(ds.ambients)
            ds.ambients.append(Ambient(p[0], p[1], pos, rot, ni if ni < nnames else -1,
                                       (raw & 0x1E) | (0 if raw & 1 else P_INITIALLY_VISIBLE), mask,
                                       r.u("<2I", e + 0x20)))

    # object names: the placement that carries a name owns it (verified unique); a name without a carrier is dangling
    nn, no = r.reflexive(sb + SCN_NAMES, NAME_SRC_REC, 4096)
    carrier: dict[int, tuple[str, int]] = {}
    for kname, off, stride in [(k[1], k[2], k[3]) for k in DEVICE_BLOCKS] + [
            (k, off, stride) for k, off, stride, _p, _g, _t, _o in scenconv.KINDS if k not in ("machine", "control", "light_fixture")]:
        n, o = r.reflexive(sb + off, stride)
        for i in range(n):
            nm = r.u("<H", o + i * stride + 2)[0]
            if nm < nn and nm not in carrier:
                carrier[nm] = (kname, i)
    for k in range(nn):
        e = no + k * NAME_SRC_REC
        otype, tindex = r.u("<2H", e + 32)
        kname, stype = OBJ_TYPE_KIND.get(otype, ("", scenconv.OTHER))
        ne = NameEntry(_name(r, e), otype, tindex, stype)
        c = carrier.get(k)
        if c is None:
            ds.dangling_names += 1
        else:
            ne.flags |= NF_RESOLVED
            si = sidx.get(c[0], [])
            ne.scen_index = si[c[1]] if c[1] < len(si) and si[c[1]] is not None else NONE32
            if (c[0], c[1]) in src_to_dev:
                ne.flags |= NF_DEVICE
                ne.aux_index = src_to_dev[(c[0], c[1])]
            elif c[0] == "sound_scenery" and c[1] in src_to_amb:
                ne.flags |= NF_AMBIENT
                ne.aux_index = src_to_amb[c[1]]
            if kname and c[0] != kname:
                problems.append(f"object name {k}: table says {kname}, carried by a {c[0]}")
            elif kname and c[1] != tindex:
                problems.append(f"object name {k}: table index {tindex}, carried by placement {c[1]}")
        ds.names.append(ne)
    return ds


def collect_map(map_path: Path, with_bsps: bool = True) -> DevSet:
    m = texconv.MapData.open(map_path)
    return collect(m, scenconv.load_bsps(m) if with_bsps else [])


# ---- roots for the other packs ---------------------------------------------------------------------------------
def models_of(ds: DevSet) -> set[int]:
    return {d.model_tag for d in ds.defs if d.model_tag}


def anims_of(ds: DevSet) -> set[int]:
    return {d.anim_tag for d in ds.defs if d.anim_tag}


def sounds_of(ds: DevSet) -> set[int]:
    return {t for d in ds.defs for t in d.fx if t} | {a.sound_tag for a in ds.ambients if a.sound_tag}


def effects_of(ds: DevSet) -> set[int]:
    return {t for d in ds.defs for i, t in enumerate(d.fx) if t and not d.fx_snd_mask >> i & 1}


def referenced_models(map_path: Path) -> set[int]:
    """Tag ids of the ``model`` tags of the device tags the scenario places (keyed like modelconv).  Placed devices are
    already SCEN placements, so objpack has them; this is the explicit root list."""
    return models_of(collect_map(map_path, False))


def referenced_anims(map_path: Path) -> set[int]:
    """Tag ids of the ``model_animations`` graphs of the same device tags (keyed like animconv / ansmconv)."""
    return anims_of(collect_map(map_path, False))


def referenced_sounds(map_path: Path) -> set[int]:
    """Tag ids to hand to ``soundconv.py --roots`` / ``streamconv``: the ``snd!`` and ``effe`` references of the device
    tags (soundconv follows effects to their sounds) and the looping sounds of the sound scenery."""
    return sounds_of(collect_map(map_path, False))


def referenced_effects(map_path: Path) -> set[int]:
    """Tag ids of the ``effe`` tags among the device references (roots for ``fxconv`` if it is run with explicit roots)."""
    return effects_of(collect_map(map_path, False))


# ---- section writer / reference parser -------------------------------------------------------------------------
DEF_FMT = "<III BB HHH 6I f II 4B 10I HH HHHH BBH f 32s 8x"
DEVICE_FMT = "<HHI 3f3f hh II hH I 6f 4x"
GROUP_FMT = "<32s fI 8x"
VOLUME_FMT = "<32s 3f3f3f3f3f 4x"
START_FMT = "<3f f hh 4h I"
CFLAG_FMT = "<32s 3f ff I 8x"
CAMERA_FMT = "<32s 3f 3f f I"
NAME_FMT = "<32s BBBB HH II"
AMBIENT_FMT = "<II 3f3f hH I 2I"
DECAL_FMT = "<Hbb 3f ff I 4x"
BSPSW_FMT = "<4H"
for _f, _n in ((DEF_FMT, DEF_REC), (DEVICE_FMT, DEVICE_REC), (GROUP_FMT, GROUP_REC), (VOLUME_FMT, VOLUME_REC),
               (START_FMT, START_REC), (CFLAG_FMT, CFLAG_REC), (CAMERA_FMT, CAMERA_REC), (NAME_FMT, NAME_REC),
               (AMBIENT_FMT, AMBIENT_REC), (DECAL_FMT, DECAL_REC), (BSPSW_FMT, BSPSW_REC)):
    assert struct.calcsize(_f.replace(" ", "")) == _n, (_f, struct.calcsize(_f.replace(" ", "")), _n)
HEADER_FMT = "<4sHHI 12I I"          # magic, version, reserved, size, 12 counts, bsp count (0x40 follows)


def _s(fmt: str) -> struct.Struct:
    return struct.Struct(fmt.replace(" ", ""))


S_DEF, S_DEVICE, S_GROUP, S_VOLUME, S_START = (_s(DEF_FMT), _s(DEVICE_FMT), _s(GROUP_FMT), _s(VOLUME_FMT), _s(START_FMT))
S_CFLAG, S_CAMERA, S_NAME, S_AMBIENT, S_DECAL, S_BSPSW = (_s(CFLAG_FMT), _s(CAMERA_FMT), _s(NAME_FMT),
                                                           _s(AMBIENT_FMT), _s(DECAL_FMT), _s(BSPSW_FMT))


def _str(s: str, n: int = NAME_LEN) -> bytes:
    return s.encode("latin1", "replace")[:n - 1].ljust(n, b"\0")


def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % ALIGN)


def name_order(names: list[NameEntry]) -> list[int]:
    return sorted(range(len(names)), key=lambda i: (_str(names[i].name), i))


def build(ds: DevSet) -> bytes:
    """Serialise a DEVS section."""
    if ds.bsp_count > MAX_BSPS:
        raise DevError(f"{ds.bsp_count} BSPs (maximum {MAX_BSPS})")
    out = bytearray(HEADER_SIZE)
    offs: dict[str, int] = {}
    counts = {"defs": len(ds.defs), "devices": len(ds.devices), "groups": len(ds.groups),
              "volumes": len(ds.volumes), "starts": len(ds.starts), "cflags": len(ds.cflags),
              "cameras": len(ds.cameras), "names": len(ds.names), "ambients": len(ds.ambients),
              "decals": len(ds.decals), "decal_types": len(ds.decal_types), "bspsw": len(ds.bspsw)}
    if any(a.tag_id >= b.tag_id for a, b in zip(ds.defs, ds.defs[1:])):
        raise DevError("definitions must be sorted by strictly ascending tag id")
    for key, n in counts.items():
        if n > MAX_RECORDS:
            raise DevError(f"{n} {key}")
    for tag in ("defs",):
        offs[tag] = len(out)
        for d in ds.defs:
            out += S_DEF.pack(d.tag_id, d.model_tag, d.anim_tag, d.kind, d.subtype, d.dev_flags, d.kind_flags,
                              d.triggers, *d.ticks, d.auto_radius, d.delay_ticks, d.open_ticks, *d.fn_in, *d.fx,
                              d.fx_snd_mask, 0, d.pos_anim, d.power_anim, d.pos_frames, d.power_frames,
                              d.collision, d.elevator_node, 0, d.call_value, _str(d.name))
        _pad16(out)
    offs["devices"] = len(out)
    for v in ds.devices:
        if not 0 <= v.def_index < len(ds.defs):
            raise DevError("device definition index out of range")
        out += S_DEVICE.pack(v.def_index, v.flags, v.bsp_mask, *v.pos, *v.rot, v.power_group, v.position_group,
                             v.device_flags, v.kind_flags, v.name_index, v.extra, v.scen_index, *v.light)
    _pad16(out)
    offs["groups"] = len(out)
    for g in ds.groups:
        out += S_GROUP.pack(_str(g.name), g.initial, g.flags)
    _pad16(out)
    offs["volumes"] = len(out)
    for v in ds.volumes:
        out += S_VOLUME.pack(_str(v.name), *v.forward, *v.left, *v.up, *v.position, *v.extents)
    _pad16(out)
    offs["starts"] = len(out)
    for s in ds.starts:
        out += S_START.pack(*s.pos, s.facing, s.team, s.bsp, *s.types, s.bsp_mask)
    _pad16(out)
    offs["cflags"] = len(out)
    for c in ds.cflags:
        out += S_CFLAG.pack(_str(c.name), *c.pos, c.yaw, c.pitch, c.bsp_mask)
    _pad16(out)
    offs["cameras"] = len(out)
    for c in ds.cameras:
        out += S_CAMERA.pack(_str(c.name), *c.pos, *c.ori, c.fov, c.bsp_mask)
    _pad16(out)
    offs["names"] = len(out)
    for n in ds.names:
        out += S_NAME.pack(_str(n.name), n.obj_type, n.scen_type, n.flags, 0, n.table_index, 0, n.scen_index, n.aux_index)
    _pad16(out)
    offs["name_order"] = len(out)
    for i in name_order(ds.names):
        out += struct.pack("<H", i)
    _pad16(out)
    offs["ambients"] = len(out)
    for a in ds.ambients:
        out += S_AMBIENT.pack(a.ssce_tag, a.sound_tag, *a.pos, *a.rot, a.name_index, a.flags, a.bsp_mask, *a.raw)
    _pad16(out)
    offs["decals"] = len(out)
    for d in ds.decals:
        out += S_DECAL.pack(d.type, d.yaw_raw, d.pitch_raw, *d.pos, d.yaw, d.pitch, d.bsp_mask)
    _pad16(out)
    offs["decal_types"] = len(out)
    for t in ds.decal_types:
        out += struct.pack("<I", t)
    _pad16(out)
    offs["bspsw"] = len(out)
    for b in ds.bspsw:
        out += S_BSPSW.pack(b.volume, b.source, b.dest, b.unknown)
    _pad16(out)
    struct.pack_into(HEADER_FMT.replace(" ", ""), out, 0, MAGIC, VERSION, 0, len(out),
                     *(counts[k] for k in COUNT_KEYS), ds.bsp_count)
    struct.pack_into("<13I", out, 0x40, *(offs[t] for t in TABLES))
    struct.pack_into("<II", out, 0x74, ds.dropped_devices, ds.dangling_names)
    return bytes(out)


def _cs(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("latin1")


def parse_section(data: bytes) -> dict:
    """Reference reader (the C reader is engine/src/world/devdef.c); raises DevError on a bad section.  Returns a dict of
    lists of dicts keyed like the tables."""
    if len(data) < HEADER_SIZE:
        raise DevError("section shorter than the header")
    magic, version, _r, size = struct.unpack_from("<4sHHI", data)
    if magic != MAGIC:
        raise DevError("bad signature")
    if version != VERSION:
        raise DevError(f"unsupported version {version}")
    if size != len(data):
        raise DevError("inconsistent size")
    counts = dict(zip(COUNT_KEYS, struct.unpack_from("<12I", data, 0x0C)))
    counts["name_order"] = counts["names"]
    bsp_count = struct.unpack_from("<I", data, 0x3C)[0]
    offs = dict(zip(TABLES, struct.unpack_from("<13I", data, 0x40)))
    if bsp_count > MAX_BSPS:
        raise DevError("too many BSPs")
    for t in TABLES:
        n, o = counts[t], offs[t]
        if o % ALIGN or o < HEADER_SIZE or o > len(data) or n > MAX_RECORDS or o + n * RECORDS[t] > len(data):
            raise DevError(f"table {t} outside the section")
    res: dict = {"bsp_count": bsp_count, "counts": counts}
    dropped, dangling = struct.unpack_from("<II", data, 0x74)
    res["dropped_devices"], res["dangling_names"] = dropped, dangling

    def rows(t: str, st: struct.Struct):
        return [st.unpack_from(data, offs[t] + i * RECORDS[t]) for i in range(counts[t])]

    res["defs"] = []
    for v in rows("defs", S_DEF):
        res["defs"].append(dict(tag_id=v[0], model_tag=v[1], anim_tag=v[2], kind=v[3], subtype=v[4], dev_flags=v[5],
                                kind_flags=v[6], triggers=v[7], ticks=v[8:14], auto_radius=v[14], delay_ticks=v[15],
                                open_ticks=v[16], fn_in=v[17:21], fx=v[21:31], fx_snd_mask=v[31], pos_anim=v[33],
                                power_anim=v[34], pos_frames=v[35], power_frames=v[36], collision=v[37],
                                elevator_node=v[38], call_value=v[40], name=_cs(v[41])))
    res["devices"] = [dict(def_index=v[0], flags=v[1], bsp_mask=v[2], pos=v[3:6], rot=v[6:9], power_group=v[9],
                           position_group=v[10], device_flags=v[11], kind_flags=v[12], name_index=v[13], extra=v[14],
                           scen_index=v[15], light=v[16:22]) for v in rows("devices", S_DEVICE)]
    res["groups"] = [dict(name=_cs(v[0]), initial=v[1], flags=v[2]) for v in rows("groups", S_GROUP)]
    res["volumes"] = [dict(name=_cs(v[0]), forward=v[1:4], left=v[4:7], up=v[7:10], position=v[10:13], extents=v[13:16])
                      for v in rows("volumes", S_VOLUME)]
    res["starts"] = [dict(pos=v[0:3], facing=v[3], team=v[4], bsp=v[5], types=v[6:10], bsp_mask=v[10])
                     for v in rows("starts", S_START)]
    res["cflags"] = [dict(name=_cs(v[0]), pos=v[1:4], yaw=v[4], pitch=v[5], bsp_mask=v[6]) for v in rows("cflags", S_CFLAG)]
    res["cameras"] = [dict(name=_cs(v[0]), pos=v[1:4], ori=v[4:7], fov=v[7], bsp_mask=v[8]) for v in rows("cameras", S_CAMERA)]
    res["names"] = [dict(name=_cs(v[0]), obj_type=v[1], scen_type=v[2], flags=v[3], table_index=v[5], scen_index=v[7],
                         aux_index=v[8]) for v in rows("names", S_NAME)]
    res["name_order"] = [struct.unpack_from("<H", data, offs["name_order"] + 2 * i)[0] for i in range(counts["names"])]
    res["ambients"] = [dict(ssce_tag=v[0], sound_tag=v[1], pos=v[2:5], rot=v[5:8], name_index=v[8], flags=v[9],
                            bsp_mask=v[10], raw=v[11:13]) for v in rows("ambients", S_AMBIENT)]
    res["decals"] = [dict(type=v[0], yaw_raw=v[1], pitch_raw=v[2], pos=v[3:6], yaw=v[6], pitch=v[7], bsp_mask=v[8])
                     for v in rows("decals", S_DECAL)]
    res["decal_types"] = [struct.unpack_from("<I", data, offs["decal_types"] + 4 * i)[0] for i in range(counts["decal_types"])]
    res["bspsw"] = [dict(volume=v[0], source=v[1], dest=v[2], unknown=v[3]) for v in rows("bspsw", S_BSPSW)]
    return res


def build_section(map_path: Path) -> bytes:
    """The DEVS section of a decompressed map (BSP masks by point location in every BSP)."""
    m = texconv.MapData.open(map_path)
    ds = collect(m, scenconv.load_bsps(m))
    check_scen(m, ds)
    return build(ds)


def check_scen(m: texconv.MapData, ds: DevSet) -> None:
    """Cross-check the placement order replica against the real SCEN collection (same count and per-kind order)."""
    try:
        objs = scenconv.collect_objects(m, [])
    except scenconv.ScenError as ex:
        raise DevError(str(ex)) from None
    if len(objs.placements) != ds.scen_count:
        raise DevError(f"SCEN placement count {len(objs.placements)} != replicated order {ds.scen_count}")
    for dev in ds.devices:
        p = objs.placements[dev.scen_index]
        if p.kind != KIND_NAMES[dev.kind] or p.pos != dev.pos or p.tag_id != dev.tag_id:
            raise DevError(f"device scen_index {dev.scen_index} does not match the SCEN placement")


# ---- verification of the two conventions the data does not state ---------------------------------------------------
def verify_volume_convention(ds: DevSet, probes: list, samples: int = 30, seed: int = 5) -> dict[str, float]:
    """The BSP switch volumes straddle the seam of two BSPs, so random points inside them should be inside the source and
    destination BSP.  Returns, per hypothesis about the box (``corner`` = position is the minimum corner and the extents
    run along forward / left / up; ``center`` = position is the centre, extents the full size; ``left`` = up x forward or its
    mirror), the fraction of points inside either BSP.  The converter's convention (``corner``, up x forward) must win."""
    rnd = random.Random(seed)
    hits: Counter = Counter()
    total = 0
    for sw in ds.bspsw:
        if sw.source >= len(probes) or sw.dest >= len(probes):
            continue
        v = ds.volumes[sw.volume]
        lf = cross(v.up, v.forward)
        for sign, lname in ((1.0, "up x forward"), (-1.0, "forward x up")):
            axes = (v.forward, tuple(sign * c for c in lf), v.up)
            for mode in ("corner", "center"):
                rnd2 = random.Random(seed + sw.volume)
                for _ in range(samples):
                    u = [rnd2.random() for _ in range(3)]
                    s = [u[j] * v.extents[j] if mode == "corner" else (u[j] - 0.5) * v.extents[j] for j in range(3)]
                    p = tuple(v.position[d] + sum(s[j] * axes[j][d] for j in range(3)) for d in range(3))
                    if any(bsp.find_cluster(probes[b].structure, p) >= 0 for b in (sw.source, sw.dest)):
                        hits[(mode, lname)] += 1
        total += samples
    return {f"{k[0]}, left = {k[1]}": hits[k] / total for k in hits} if total else {}


def verify_decals(m: texconv.MapData, ds: DevSet, probes: list, limit: int = 400, tolerance: float = 0.15) -> dict[str, float]:
    """Compare the decal direction (cos p cos y, cos p sin y, sin p), angles = signed byte x 2 pi / 256, with the normal of
    the nearest rendered BSP triangle within ``tolerance`` wu of the decal position.  Returns the count matched and the
    median |dot| and fractions above 0.95 / 0.7 (a flush decal gives 1; designers may project obliquely, so not all are 1),
    over all decals and without the single most common (yaw, pitch) pair, which is an editor default that carries no
    direction (docs/27)."""
    locs = bsp.locate(m.index, m.region, m.header.decompressed_size)
    grids: dict[int, tuple] = {}
    cell = 2.0

    def grid_of(bi: int):
        if bi not in grids:
            loc = locs[bi]
            with m.path.open("rb") as f:
                f.seek(loc.file_offset)
                block = f.read(loc.size)
            mats, _ = bspconv.read_materials(block, loc)
            tris, g = [], {}
            for mat in mats:
                for t in mat.tris:
                    a, b, c = (mat.pos[k] for k in t)
                    n = cross(tuple(b[i] - a[i] for i in range(3)), tuple(c[i] - a[i] for i in range(3)))
                    ln = math.sqrt(sum(x * x for x in n))
                    if ln < 1e-9:
                        continue
                    n = tuple(-x / ln for x in n)
                    tris.append((a, b, c, n))
                    lo = [min(a[i], b[i], c[i]) for i in range(3)]
                    hi = [max(a[i], b[i], c[i]) for i in range(3)]
                    for gx in range(int(lo[0] // cell), int(hi[0] // cell) + 1):
                        for gy in range(int(lo[1] // cell), int(hi[1] // cell) + 1):
                            for gz in range(int(lo[2] // cell), int(hi[2] // cell) + 1):
                                g.setdefault((gx, gy, gz), []).append(len(tris) - 1)
            grids[bi] = (tris, g)
        return grids[bi]

    def dist(p, tri):
        a, b, c, n = tri
        d = sum((p[i] - a[i]) * n[i] for i in range(3))
        q = tuple(p[i] - d * n[i] for i in range(3))
        v0, v1, v2 = (tuple(x[i] - a[i] for i in range(3)) for x in (c, b, q))
        dot = lambda x, y: sum(i * j for i, j in zip(x, y))  # noqa: E731
        d00, d01, d02, d11, d12 = dot(v0, v0), dot(v0, v1), dot(v0, v2), dot(v1, v1), dot(v1, v2)
        den = d00 * d11 - d01 * d01
        if abs(den) < 1e-12:
            return None
        u, w = (d11 * d02 - d01 * d12) / den, (d00 * d12 - d01 * d02) / den
        return abs(d) if u >= -0.05 and w >= -0.05 and u + w <= 1.05 else None

    vals: list[tuple[float, tuple[int, int]]] = []
    nz: list[tuple[int, float]] = []
    step = max(1, len(ds.decals) // limit)
    for dec in ds.decals[::step]:
        if not dec.bsp_mask:
            continue
        tris, g = grid_of((dec.bsp_mask & -dec.bsp_mask).bit_length() - 1)
        c0 = tuple(int(math.floor(dec.pos[k] / cell)) for k in range(3))
        best = None
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for idx in g.get((c0[0] + dx, c0[1] + dy, c0[2] + dz), ()):
                        dd = dist(dec.pos, tris[idx])
                        if dd is not None and (best is None or dd < best[0]):
                            best = (dd, idx)
        if best is None or best[0] > tolerance:
            continue
        y, pch = dec.yaw, dec.pitch
        fwd = (math.cos(pch) * math.cos(y), math.cos(pch) * math.sin(y), math.sin(pch))
        vals.append((abs(sum(fwd[k] * tris[best[1]][3][k] for k in range(3))), (dec.yaw_raw, dec.pitch_raw)))
        nz.append((dec.pitch_raw, tris[best[1]][3][2]))
    if not vals:
        return {"matched": 0}
    common, ccount = Counter(p for _, p in vals).most_common(1)[0]
    out: dict[str, float] = {"matched": len(vals), "most_common_pair_fraction": ccount / len(vals)}
    for label, sel in (("all", [v for v, _ in vals]), ("without_most_common_pair", [v for v, p in vals if p != common])):
        sel.sort()
        if sel:
            out[f"{label}: median"] = sel[len(sel) // 2]
            out[f"{label}: above_0.95"] = sum(v > 0.95 for v in sel) / len(sel)
            out[f"{label}: above_0.7"] = sum(v > 0.7 for v in sel) / len(sel)
    down = [z for p_, z in nz if p_ <= -56]
    up = [z for p_, z in nz if p_ >= 56]
    if down:
        out["pitch <= -56: surface faces up"] = sum(z > 0.9 for z in down) / len(down)
    if up:
        out["pitch >= 56: surface faces down"] = sum(z < -0.9 for z in up) / len(up)
    return out


# ---- reporting ----------------------------------------------------------------------------------------------
def describe(ds: DevSet, name: str = "") -> str:
    L = [f"{name}: {len(ds.defs)} device tags, {len(ds.devices)} placed devices "
         f"({', '.join(f'{KIND_NAMES[k]} {sum(1 for d in ds.devices if d.kind == k)}' for k in range(3))}), "
         f"{ds.dropped_devices} dropped"]
    L.append(f"  groups {len(ds.groups)}, volumes {len(ds.volumes)}, starting locations {len(ds.starts)}, "
             f"cutscene flags {len(ds.cflags)}, camera points {len(ds.cameras)}, object names {len(ds.names)} "
             f"({ds.dangling_names} dangling), sound scenery {len(ds.ambients)}, decals {len(ds.decals)} of "
             f"{len(ds.decal_types)} types, BSP switches {len(ds.bspsw)}, BSPs {ds.bsp_count}")
    mt = Counter(MACHINE_TYPES[d.subtype] for d in ds.defs if d.kind == K_MACHINE and d.subtype < 3)
    L.append("  machine tags per type: " + ", ".join(f"{k} {v}" for k, v in sorted(mt.items())))
    tk = [d.ticks[2] for d in ds.defs if d.kind == K_MACHINE]
    if tk:
        L.append(f"  machine position transition ticks min/max {min(tk)}/{max(tk)}, door open ticks max "
                 f"{max(d.open_ticks for d in ds.defs)}")
    for pr in ds.problems[:10]:
        L.append(f"  problem: {pr}")
    return "\n".join(L)


def info_pack(path: Path) -> str:
    s = parse_section(hp2.parse(path.read_bytes()).read("DEVS"))
    c = s["counts"]
    return f"{path.name}: DEVS " + ", ".join(f"{k} {v}" for k, v in c.items() if k != "name_order") + \
        f", BSPs {s['bsp_count']}, dropped devices {s['dropped_devices']}, dangling names {s['dangling_names']}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="print counts instead of writing")
    ap.add_argument("--no-bsp", action="store_true", help="skip point location (every BSP mask is 0)")
    ap.add_argument("--verify", action="store_true", help="on a map: test the volume box and decal angle conventions")
    args = ap.parse_args(argv)
    if args.info and args.input.suffix == ".hp2":
        print(info_pack(args.input))
        return 0
    if args.verify:
        m = texconv.MapData.open(args.input)
        probes = scenconv.load_bsps(m)
        ds = collect(m, probes)
        print(args.input.stem, "volume convention (fraction of BSP switch volume points inside the BSPs):")
        for k, v in sorted(verify_volume_convention(ds, probes).items()):
            print(f"  {k:<32} {v:.3f}")
        print(" decal normals:", {k: round(v, 3) for k, v in verify_decals(m, ds, probes).items()})
        return 0
    if args.info:
        print(describe(collect_map(args.input, not args.no_bsp), args.input.stem))
        return 0
    if not args.out:
        ap.error("-o is required")
    m = texconv.MapData.open(args.input)
    ds = collect(m, [] if args.no_bsp else scenconv.load_bsps(m))
    check_scen(m, ds)
    blob = build(ds)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, [("DEVS", blob)]))
    print(f"wrote {args.out} (DEVS {len(blob) / 1024:.1f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
