#!/usr/bin/env python3
"""Convert the state tables of Xbox `model_animations` (group 'antr') tags into the PS2 `ASTB` section
(docs/19-ansm-section.md, engine/src/world/ansm.h): named animation lookup for units, weapons, first-person
arms, vehicles and devices, animation info (frame count, ticks, loops, variant chains, events) and the
aiming ranges, keyed by the graph's Xbox tag id like the `ANIM` section (animation index k of a graph here is
animation k of the same graph in ANIM).

Usage:
    python3 -I tools/halo2ps2/ansmconv.py assets/decompressed/a10.map -o assets/hp2/a10.ansm.hp2 [--all] [--tags ID ...]
    python3 -I tools/halo2ps2/ansmconv.py assets/decompressed/a10.map --info [--verify] [--dump 'characters\\cyborg\\cyborg']
    python3 -I tools/halo2ps2/ansmconv.py assets/hp2/a10.ansm.hp2 --info

Default graph set: `referenced_anims(map)` (every graph an object tag or a weapon's first-person slot names);
`--all` converts every `antr` tag of the map (cinematic graphs included).

=====================================================================================================
Xbox `model_animations` state tables, engine version 5, as read by this module
=====================================================================================================
Verified = checked on all ten campaign maps by an assertion in `read_graph` / `verify_graph` (indices in range,
labels printable, strides consistent) or by the name statistics in docs/19-ansm-section.md. Unverified = read
from the data but the meaning is taken from the public tag documentation or inferred. Offsets are into the
tag's data block; a reflexive is u32 count, u32 address (0x803A6000-based), u32 0. Slot lists are reflexives
of 16-bit animation indices into the `animations` block (-1 / 0xFFFF = none; entries past the end of a list
are "none" too).

Tag struct (VERIFIED, counts and strides on every graph):
    +0x00 objects (20 B)   +0x0C units (100 B)   +0x18 weapons (28 B)   +0x24 vehicles (116 B)
    +0x30 devices (96 B)   +0x3C unit damage (2 B per entry)   +0x48 first person weapons (28 B)
    +0x54 sound references (20 B)   +0x68 nodes (64 B)   +0x74 animations (180 B)

objects (20 B): +0 u16 animation, +2 u16 function (0..3 = object function A..D output, UNVERIFIED names;
    values seen 0..3), +4 u16 function controls (0 or 1 seen), 14 pad. Overlay animations driven by an object
    function (a sky's wind/scale/rotation, the warthog's `firing`, a monitor's talking loop). VERIFIED by name.

unit (100 B): +0x00 char[32] label ("stand", "crouch", "alert", seat names "W-driver", "P-riderLF", ...)
    +0x20 f32 yaw per frame right, f32 left, i16 frame count right, i16 left, +0x2C f32 pitch per frame down,
    f32 up, i16 frame count down, i16 up (the aiming range: per frame x count = range in radians; VERIFIED:
    the `aim-still` / `aim-move` overlay that goes with it has (right + left + 1) x (up + down + 1) frames
    in 810 of 857 cases, see the doc), +0x38 8 pad,
    +0x40 reflexive of unit-level animation slots (u16 each), +0x4C reflexive of IK points (64 B: char[32]
    marker, char[32] attach-to marker; names like 'right hand' / 'right hand wheel grip'), +0x58 reflexive of
    weapon class blocks (188 B).
class block, `unit weapon` (188 B): +0x00 char[32] name (weapon CLASS: "rifle", "pistol", "missile", "support",
    "unarmed", "fixed", "sword", ...), +0x20 char[32] grip marker, +0x40 char[32] hold marker ("right hand"),
    +0x60 the same 24 bytes of aiming range as the unit, 32 bytes unused, +0x98 reflexive of class slots
    (i16 each, up to 51), +0xA4 reflexive of IK points, +0xB0 reflexive of weapon type blocks (60 B).
weapon type (60 B): +0x00 char[32] label ("ar", "sg", "sr", "pr", "pp", "hp", "ne", "rl", "ft", "fr", ...: equal to
    the string at +0x30C of the weapon tag: VERIFIED for all 20 weapon tags of the ten maps, each label occurs as a
    type label in some graph), +0x20 16 pad, +0x30 reflexive of type slots (i16, up to 9). The weapon tag does
    not name its CLASS (nothing in its first 0x520 bytes distinguishes 'pistol' from 'rifle' for the plasma pistol,
    whose graph blocks sit under 'pistol'), and the data spells some classes wrong ('missle' 90 of 160 blocks,
    'unuarmed' 16): `ansm_find_class_for_type` picks the class block by label instead.
vehicle (116 B): +0x00 f32 right yaw per frame, f32 left, i16 right count, i16 left count (the steering range:
    pelican 60 deg, warthog 35 deg, ghost 30 deg, banshee 25 deg: 1 frame each), +0x5C reflexive of vehicle slots
    (i16), +0x68 reflexive of suspension (20 B: u16 mass point index, u16 animation, f32 full extension ground
    depth, f32 full compression ground depth, 8 pad; VERIFIED by name: 'suspension left back' ...).
device (96 B): +0x54 reflexive of device slots (position, power).
weapons (28 B, the weapon OBJECT's own graph): +0x10 reflexive of weapon slots (i16).
first person weapon (28 B): +0x10 reflexive of slots (i16, up to 28); 16 pad.
unit damage: a flat table of 176 i16 = 4 categories x 4 directions x 11 hit regions (VERIFIED: the animation
    names prefix s-ping / h-ping / s-kill / h-kill and front / left / right / back, in that order).
sound references (20 B): +0 tag reference ('snd!': 4 group, 4 path pointer, 4 length, 4 tag id), +16 u32 0.

animation (180 B, the fields ASTB uses; the full layout is in animconv.py):
    +0x00 char[32] name   +0x20 i16 type (0 base, 1 overlay, 2 replacement)   +0x22 i16 frame count
    +0x2E i16 loop frame   +0x30 f32 weight (0 or 1 ... unused)   +0x34 i16 key frame   +0x36 i16 second key frame
    +0x38 i16 next animation   +0x3A u16 flags (1 compressed, 2 world relative, 4 25 Hz)   +0x3C i16 sound
    (index into the graph's sound references, -1 none)   +0x3E i16 sound frame   +0x40 i8 left foot frame
    +0x41 i8 right foot frame   +0x42 i16 main animation   +0x44 f32 relative weight
    VERIFIED: `next animation` is -1 or the NEXT VARIANT (names `idle%1` -> `idle%2` -> `idle%3`; it is never
    the animation itself and never a looping marker), `main animation` is the first variant of the chain (the
    animation itself when it has no variants), `relative weight` is the CUMULATIVE probability of the chain
    position (0.33, 0.67, 1.00 for three variants); sound index < sound reference count and sound frame <
    frame count on all 8219 animations of the ten maps; the key frames are below the frame count and 0 for most
    animations (non-zero on `melee`, `throw-grenade`, a few others: the frame where the blow lands or the
    grenade leaves). The left/right foot frames can exceed the frame count (up to 2x: 'turn-left' of 7 frames
    has 7 and 14): they are passed through unchecked.

=====================================================================================================
`ASTB` section format, version 1 (little endian; offsets are absolute from the section start)
=====================================================================================================
    header (64 B)   "ASTB" | u16 version = 1 | u16 flags 0 | u32 graph_count | u32 graphs_off (64) | u32 body_off |
                    u32 total size | u32 anim_count (all graphs) | zero
    graph  x G (96 B, ascending tag id)   u32 tag_id | u32 flags 0 | 10 refs (u32 off, u32 count) in this
                    order: anims (32 B each), name index (u16 each), objects (8 B), units (64 B), vehicles (32 B),
                    devices (16 B), weapon objects (16 B), first person (16 B), damage (u16 each), sounds (u32 tag
                    id each) | zero
    body            16-aligned tables written bottom-up; strings (NUL terminated, offset 0 = ""), slot lists (u16,
                    deduplicated, trailing "none" trimmed)
    anim (32 B)     u32 name_off | u16 frame_count | u16 ticks | u16 flags (1 LOOPS, 2 WORLD_RELATIVE) | u8 type |
                    u8 fps | i16 next | i16 main | u16 loop_frame | i16 key_frame | i16 key_frame2 | i16 sound |
                    i16 sound_frame | u8 foot_left | u8 foot_right | u16 variant_weight (cumulative, /65535) | 2 pad
    unit (64 B)     u32 name_off | ref classes (class 64 B) | ref slots | ref ik (8 B) | aim 24 B | pad
    class (64 B)    u32 name_off | u32 grip_off | u32 hold_off | ref slots | ref ik | ref types | aim 24 B | pad
    type (16 B)     u32 label_off | ref slots | pad
    ik (8 B)        u32 marker_off | u32 attach_off
    aim (24 B)      f32 yaw_right_per_frame | f32 yaw_left_per_frame | u16 yaw_right_frames | u16 yaw_left_frames |
                    f32 pitch_down_per_frame | f32 pitch_up_per_frame | u16 pitch_down_frames | u16 pitch_up_frames
    vehicle (32 B)  f32 yaw_right | f32 yaw_left | u16 right_frames | u16 left_frames | ref slots | ref suspension |
                    pad;  suspension (16 B): u16 mass_point | i16 anim | f32 extension | f32 compression | pad
    device, weapon object, first person (16 B)   ref slots | pad
    object (8 B)    i16 anim | u16 function | u16 controls | pad
Every animation index stored anywhere is < the graph's animation count or "none" (the converter drops and reports
out-of-range references; the reader rejects them).
"""
from __future__ import annotations

import argparse
import bisect
import math
import struct
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import animconv, hp2  # noqa: E402

ConvertError = animconv.ConvertError
MapTags = animconv.MapTags

MAGIC = b"ASTB"
VERSION = 1
HEADER_SIZE = 64
GRAPH_REC = 96
ANIM_REC = 32
UNIT_REC = 64
CLASS_REC = 64
TYPE_REC = 16
IK_REC = 8
VEH_REC = 32
SUSP_REC = 16
LIST_REC = 16                    # device / weapon object / first person
OBJ_REC = 8
ALIGN = 16
NONE = 0xFFFF
MAX_NAME = 64                    # reader limit on any string (incl. NUL)
ANSM_LOOPS, ANSM_WORLD_RELATIVE = 1, 2

# tag layout
ANTR_OBJECTS, ANTR_UNITS, ANTR_WEAPONS, ANTR_VEHICLES, ANTR_DEVICES = 0x00, 0x0C, 0x18, 0x24, 0x30
ANTR_DAMAGE, ANTR_FP, ANTR_SOUNDS, ANTR_ANIMS = 0x3C, 0x48, 0x54, 0x74
OBJECT_RECORD, UNIT_RECORD, CLASS_RECORD, TYPE_RECORD, IK_RECORD = 20, 100, 188, 60, 64
VEHICLE_RECORD, DEVICE_RECORD, WEAPON_RECORD, FP_RECORD, SOUND_RECORD, SUSP_RECORD = 116, 96, 28, 28, 20, 20
ANIM_RECORD = animconv.ANTR_ANIM_REC
WEAPON_FP_ANIM = 0x46C          # weapon tag: first person animation graph reference
OBJ_ANIM_AT = animconv.OBJ_ANIM_AT
AIM_FMT = "<ffhhffhh"
DAMAGE_CATEGORIES = ("s-ping", "h-ping", "s-kill", "h-kill")
DAMAGE_DIRECTIONS = ("front", "left", "right", "back")
DAMAGE_REGIONS = 11

# ---- slot tables -----------------------------------------------------------------------------------------------
# Names of the slots of every list, in order. A name is given only where animations of the campaign graphs carry
# it ("verified": the last word of the animation name referenced from this slot equals the slot name, or a
# documented alias, see ALIASES) or where a public document and a neighbouring verified slot agree
# ("assumed", marked in docs/19-ansm-section.md). "" = no animation of any campaign graph uses the slot.
L_TYPE, L_CLASS, L_UNIT, L_FP, L_WOBJ, L_VEHICLE, L_DEVICE = range(7)
LEVEL_NAMES = ("type", "class", "unit", "fp", "wobj", "vehicle", "device")

TYPE_SLOTS = ("reload-1", "reload-2", "chamber-1", "chamber-2", "fire-1", "fire-2", "", "", "melee")
CLASS_SLOTS = (
    "idle", "", "turn-left", "turn-right", "dive-front", "", "dive-left", "dive-right",      # 0..7
    "move-front", "move-back", "move-left", "move-right", "", "", "", "",                    # 8..15
    "airborne", "land-soft", "land-hard", "", "throw-grenade", "", "", "ready",              # 16..23
    "", "aim-still", "aim-move", "surprise-front", "surprise-back", "berserk",              # 24..29
    "evade-left", "evade-right", "signal-move", "signal-attack", "warn",                    # 30..34
    "stunned-front", "stunned-back", "stunned-left", "stunned-right",                       # 35..38
    "melee", "celebrate", "panic", "melee-airborne", "", "resurrect-front",                 # 39..44
    "resurrect-back", "melee-continuous", "feeding", "leap-start", "leap-airborne", "leap-melee",   # 45..50
)
UNIT_SLOTS = (
    "airborne-dead", "landing-dead", "acc-front-back", "acc-left-right", "acc-up-down", "push", "",    # 0..6
    "enter", "exit", "look", "talk", "emotions",                                                     # 7..11
    "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",                                      # 12..26
    "opening", "closing", "hovering",                                                                # 27..29
)
FP_SLOTS = (
    "idle", "posing", "fire-1", "moving", "overlays", "light-off", "light-on", "reload-empty", "reload-full",   # 0..8
    "overheated", "ready", "put-away", "overcharged", "melee", "fire-2", "overcharged-jitter",               # 9..15
    "throw-grenade", "ammunition", "misfire-1", "misfire-2", "throw-overheated", "overheating",              # 16..21
    "overheating-again", "enter", "exit-empty", "exit-full", "o-h-exit", "o-h-s-enter",                      # 22..27
)
WOBJ_SLOTS = ("", "", "", "", "reload-2", "chamber-1", "", "", "", "fire-1")
VEHICLE_SLOTS = ("steering", "roll", "throttle", "velocity", "braking", "ground-speed", "occupied", "unoccupied")
DEVICE_SLOTS = ("position", "power")
SLOT_NAMES = (TYPE_SLOTS, CLASS_SLOTS, UNIT_SLOTS, FP_SLOTS, WOBJ_SLOTS, VEHICLE_SLOTS, DEVICE_SLOTS)

# animations the authors pointed at a neighbouring slot: (level, slot) -> other last words seen there
ALIASES = {
    (L_CLASS, 6): ("evade-left",), (L_CLASS, 7): ("evade-right",),
    (L_CLASS, 17): ("land-hard",), (L_CLASS, 18): ("land-soft",),
    (L_CLASS, 26): ("aim-still",),
    (L_CLASS, 36): ("stunned-front",), (L_CLASS, 37): ("stunned-front",), (L_CLASS, 38): ("stunned-front",),
    (L_FP, 5): ("light-off",), (L_FP, 6): ("light-off",), (L_FP, 7): ("reload-full", "overheated"),
    (L_FP, 8): ("reload-empty", "overheated"), (L_FP, 9): ("overheated",),
    (L_FP, 23): ("enter",), (L_FP, 24): ("exit-empty",),
}


def action(level: int, slot: int) -> int:
    """The unified action code of the reader API: level << 8 | slot."""
    return level << 8 | slot


def slot_name(level: int, slot: int) -> str:
    names = SLOT_NAMES[level]
    return names[slot] if 0 <= slot < len(names) else ""


def slot_by_name(level: int, name: str) -> int:
    names = SLOT_NAMES[level]
    return names.index(name) if name and name in names else -1


# ---- source structures ---------------------------------------------------------------------------------------------
@dataclass
class RAnim:
    name: str
    type: int
    frame_count: int
    flags: int
    loop_frame: int
    next: int
    main: int
    key: int
    key2: int
    sound: int
    sound_frame: int
    foot_left: int
    foot_right: int
    rel_weight: float


@dataclass
class RIk:
    marker: str
    attach: str


@dataclass
class RType:
    label: str
    slots: list[int]


@dataclass
class RClass:
    name: str
    grip: str
    hold: str
    aim: tuple
    slots: list[int]
    ik: list[RIk]
    types: list[RType]


@dataclass
class RUnit:
    label: str
    aim: tuple
    slots: list[int]
    ik: list[RIk]
    classes: list[RClass]


@dataclass
class RVehicle:
    yaw: tuple                      # right per frame, left per frame, right frames, left frames
    slots: list[int]
    suspension: list[tuple]         # (mass point, animation, extension, compression)


@dataclass
class RGraph:
    tag_id: int
    path: str
    anims: list[RAnim]
    objects: list[tuple] = field(default_factory=list)       # (animation, function, controls)
    units: list[RUnit] = field(default_factory=list)
    vehicles: list[RVehicle] = field(default_factory=list)
    devices: list[list[int]] = field(default_factory=list)
    wobjs: list[list[int]] = field(default_factory=list)
    fps: list[list[int]] = field(default_factory=list)
    damage: list[int] = field(default_factory=list)
    sounds: list[int] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    def refs(self):
        """Every animation index slot of the graph, as (level, owner description, slot, animation)."""
        for ui, u in enumerate(self.units):
            for s, a in enumerate(u.slots):
                yield L_UNIT, f"unit {u.label}", s, a
            for c in u.classes:
                for s, a in enumerate(c.slots):
                    yield L_CLASS, f"unit {u.label} class {c.name}", s, a
                for t in c.types:
                    for s, a in enumerate(t.slots):
                        yield L_TYPE, f"unit {u.label} class {c.name} type {t.label}", s, a
        for i, sl in enumerate(self.fps):
            for s, a in enumerate(sl):
                yield L_FP, f"first person {i}", s, a
        for i, sl in enumerate(self.wobjs):
            for s, a in enumerate(sl):
                yield L_WOBJ, f"weapon object {i}", s, a
        for i, v in enumerate(self.vehicles):
            for s, a in enumerate(v.slots):
                yield L_VEHICLE, f"vehicle {i}", s, a
        for i, sl in enumerate(self.devices):
            for s, a in enumerate(sl):
                yield L_DEVICE, f"device {i}", s, a


# ---- tag reading --------------------------------------------------------------------------------------------------
def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin1")


def _printable(s: str) -> bool:
    return all(32 <= ord(c) < 127 for c in s)


class _Reader:
    """Reflexives inside a tag: `sub` parses (count, address) at an offset of a record or of the region."""

    def __init__(self, t: MapTags):
        self.t = t

    def sub(self, raw: bytes, off: int, rec: int, limit: int = 4096) -> list[bytes]:
        if off + 8 > len(raw):
            raise ConvertError(f"reflexive at {off:#x} outside its record")
        count, addr = struct.unpack_from("<II", raw, off)
        if count == 0:
            return []
        start = self.t.off(addr)
        if count > limit or start < 0 or start + count * rec > len(self.t.region):
            raise ConvertError(f"reflexive at {off:#x}: {count} x {rec} B at {addr:#x} outside the tag region")
        return [self.t.region[start + i * rec:start + (i + 1) * rec] for i in range(count)]

    def u16list(self, raw: bytes, off: int) -> list[int]:
        return [struct.unpack_from("<h", b, 0)[0] for b in self.sub(raw, off, 2)]


def _ik(rd: _Reader, raw: bytes, off: int, g: RGraph, where: str) -> list[RIk]:
    out = []
    for b in rd.sub(raw, off, IK_RECORD):
        m, a = _cstr(b[:32]), _cstr(b[32:64])
        if not (_printable(m) and _printable(a)):
            g.problems.append(f"{where}: IK point names are not printable")
            continue
        out.append(RIk(m, a))
    return out


def _label(raw: bytes, g: RGraph, where: str) -> str:
    s = _cstr(raw)
    if not _printable(s):
        raise ConvertError(f"{g.path}: {where}: label {s!r} is not printable")
    return s


def read_graph(t: MapTags, e) -> RGraph:
    """Parse the header fields and state tables of one `antr` tag. Out-of-range animation indices are dropped
    (replaced by -1) and listed in `problems`; structural damage raises ConvertError."""
    rd = _Reader(t)
    base = t.off(e.data_address)
    tag = t.region[base:base + 0x80]
    if len(tag) < 0x80:
        raise ConvertError(f"{e.path}: tag shorter than its struct")
    g = RGraph(e.tag_id, e.path, [])
    for b in rd.sub(tag, ANTR_ANIMS, ANIM_RECORD):
        name = _cstr(b[:32])
        typ, fc = struct.unpack_from("<hh", b, 0x20)
        loop, = struct.unpack_from("<h", b, 0x2E)
        key, key2, nxt, flags, snd, sndf = struct.unpack_from("<hhhHhh", b, 0x34)
        lf, rf = struct.unpack_from("<bb", b, 0x40)
        main, = struct.unpack_from("<h", b, 0x42)
        rw, = struct.unpack_from("<f", b, 0x44)
        if not _printable(name) or len(name) >= animconv.NAME_LEN:
            raise ConvertError(f"{e.path}: animation name {name!r} not printable or too long")
        g.anims.append(RAnim(name, typ, fc, flags, loop, nxt, main, key, key2, snd, sndf, lf, rf, rw))
    n = len(g.anims)

    def idx(v: int, where: str) -> int:
        if v < 0:
            return -1
        if v >= n:
            g.problems.append(f"{where}: animation index {v} outside 0..{n - 1}: dropped")
            return -1
        return v

    for i, a in enumerate(g.anims):
        a.next = idx(a.next, f"animation {i} next")
        a.main = idx(a.main, f"animation {i} main")
        if a.main < 0:
            a.main = i
        if not 0 <= a.type <= 2 or a.frame_count < 1:
            raise ConvertError(f"{e.path}: animation {a.name!r}: type {a.type}, {a.frame_count} frames")
    g.sounds = []
    for b in rd.sub(tag, ANTR_SOUNDS, SOUND_RECORD):
        g.sounds.append(struct.unpack_from("<I", b, 12)[0])
    for a in g.anims:
        if a.sound >= len(g.sounds):
            g.problems.append(f"animation {a.name!r}: sound index {a.sound} outside the {len(g.sounds)} sound references")
            a.sound = -1
        if a.sound < 0:
            a.sound_frame = -1
    for b in rd.sub(tag, ANTR_OBJECTS, OBJECT_RECORD):
        an, fn, fc = struct.unpack_from("<hHH", b, 0)
        g.objects.append((idx(an, "object overlay"), fn, fc))

    def slots(raw: bytes, off: int, where: str) -> list[int]:
        lst = [idx(v, where) for v in rd.u16list(raw, off)]
        while lst and lst[-1] < 0:
            lst.pop()
        return lst

    for ub in rd.sub(tag, ANTR_UNITS, UNIT_RECORD):
        label = _label(ub[:32], g, "unit label")
        u = RUnit(label, struct.unpack_from(AIM_FMT, ub, 0x20), slots(ub, 0x40, f"unit {label}"),
                  _ik(rd, ub, 0x4C, g, f"unit {label}"), [])
        for cb in rd.sub(ub, 0x58, CLASS_RECORD):
            cname = _label(cb[:32], g, f"unit {label} class name")
            c = RClass(cname, _cstr(cb[0x20:0x40]), _cstr(cb[0x40:0x60]), struct.unpack_from(AIM_FMT, cb, 0x60),
                       slots(cb, 0x98, f"unit {label} class {cname}"), _ik(rd, cb, 0xA4, g, f"unit {label} class {cname}"), [])
            if not (_printable(c.grip) and _printable(c.hold)):
                raise ConvertError(f"{g.path}: unit {label} class {cname}: marker names not printable")
            for tb in rd.sub(cb, 0xB0, TYPE_RECORD):
                tl = _label(tb[:32], g, f"unit {label} class {cname} type label") if tb[0] else ""
                c.types.append(RType(tl, slots(tb, 0x30, f"unit {label} class {cname} type {tl}")))
            u.classes.append(c)
        g.units.append(u)
    for vb in rd.sub(tag, ANTR_VEHICLES, VEHICLE_RECORD):
        susp = []
        for sb in rd.sub(vb, 0x68, SUSP_RECORD):
            mp, an, ext, comp = struct.unpack_from("<HHff", sb, 0)
            susp.append((mp, idx(an if an != NONE else -1, "suspension"), ext, comp))
        g.vehicles.append(RVehicle(struct.unpack_from("<ffhh", vb, 0), slots(vb, 0x5C, "vehicle"), susp))
    for db in rd.sub(tag, ANTR_DEVICES, DEVICE_RECORD):
        g.devices.append(slots(db, 0x54, "device"))
    for wb in rd.sub(tag, ANTR_WEAPONS, WEAPON_RECORD):
        g.wobjs.append(slots(wb, 0x10, "weapon object"))
    for fb in rd.sub(tag, ANTR_FP, FP_RECORD):
        g.fps.append(slots(fb, 0x10, "first person"))
    g.damage = [idx(v, "unit damage") for v in rd.u16list(tag, ANTR_DAMAGE)]
    return g


def load_graphs(src, tags: list[int] | None = None, everything: bool = False) -> list[RGraph]:
    """Source graphs of a map (a path or an opened MapTags): `tags` if given, else `referenced_anims` (gameplay graphs),
    else every `antr` tag with `everything`."""
    t = src if isinstance(src, MapTags) else MapTags(Path(src))
    antr = [e for e in t.idx.entries if e.group == "antr"]
    if tags is not None:
        want = {int(x) for x in tags}
        missing = want - {e.tag_id for e in antr}
        if missing:
            raise ConvertError("not model_animations tags of this map: " + ", ".join(f"{x:#010x}" for x in sorted(missing)))
        antr = [e for e in antr if e.tag_id in want]
    elif not everything:
        want = referenced_anims(t)
        antr = [e for e in antr if e.tag_id in want]
    return [read_graph(t, e) for e in antr]


# ---- roots for the integrator --------------------------------------------------------------------------------------
def referenced_anims(map_path) -> set[int]:
    """Tag ids of the `model_animations` tags that gameplay objects use: the animation graph (+0x38) of every object
    tag (biped, vehicle, weapon, equipment, projectile, scenery, device, light fixture, ...) and the first person
    graph (+0x46C) of every weapon. The integrator adds these to objpack.py's animation roots; ASTB keys are the same ids."""
    t = map_path if isinstance(map_path, MapTags) else MapTags(Path(map_path))
    out: set[int] = set()
    for e in t.idx.entries:
        if e.group not in animconv.OBJECT_GROUPS:
            continue
        o = t.off(e.data_address)
        for at in ([OBJ_ANIM_AT] + ([WEAPON_FP_ANIM] if e.group == "weap" else [])):
            if o + at + 16 > len(t.region):
                continue
            tid = t.u32(o + at + 12)
            if tid in t.byid and t.byid[tid].group == "antr":
                out.add(tid)
    return out


def referenced_sounds(map_path) -> set[int]:
    """Tag ids of the `sound` tags that the animation graphs of `referenced_anims` name (footsteps, weapon handling,
    melee, reload, grenade throws, ...): roots for the sound workstream."""
    t = map_path if isinstance(map_path, MapTags) else MapTags(Path(map_path))
    out: set[int] = set()
    for g in load_graphs(t):
        for tid in g.sounds:
            if tid in t.byid:
                out.add(tid)
    return out


def used_anims(g: RGraph, close: bool = True) -> set[int]:
    """Animation indices of the graph referenced by any state table (slots, damage, objects, suspension), plus the
    variant chains (main and next) of those when `close`."""
    used = {a for _, _, _, a in g.refs() if a >= 0}
    used |= {a for a in g.damage if a >= 0}
    used |= {a for a, _, _ in g.objects if a >= 0}
    used |= {s[1] for v in g.vehicles for s in v.suspension if s[1] >= 0}
    if close:
        todo = list(used)
        while todo:
            k = todo.pop()
            for j in (g.anims[k].main, g.anims[k].next):
                if j >= 0 and j not in used:
                    used.add(j)
                    todo.append(j)
    return used


def dead_anims(g: RGraph) -> set[int]:
    """Animations that no state table references although their name says they belong in one ('<posture> <class>
    <action>' with a known action word as last word, e.g. 'stand rifle dive-left' in the marine graph). Animations with
    free-form names ('pilot_fidget01', 'think_1', 'uplink_station') stay: scripts play those by name."""
    words = {w for names in SLOT_NAMES for w in names if w} | {w for al in ALIASES.values() for w in al}
    used = used_anims(g)
    return {i for i, a in enumerate(g.anims)
            if i not in used and " " in a.name.split("%", 1)[0] and _last_word(a.name) in words}


def needed_animations(map_path, graphs: list[int] | None = None) -> dict[int, set[int]]:
    """For `animconv.build_section(..., animations_needed=...)`: for every graph that has state tables, the animation
    indices worth converting, i.e. all except `dead_anims` (those become pose-less stubs; indices, names and frame
    counts stay). Graphs without any state-table slot (scenery, machines, cinematic props) are absent from the dict, so
    animconv converts them in full. `graphs` defaults to `referenced_anims`. The saving on the campaign maps is small
    (a few percent): the tables name nearly every animation, so the bigger lever is converting only `referenced_anims`."""
    t = map_path if isinstance(map_path, MapTags) else MapTags(Path(map_path))
    out: dict[int, set[int]] = {}
    for g in load_graphs(t, None if graphs is None else list(graphs)):
        if used_anims(g, close=False):
            out[g.tag_id] = set(range(len(g.anims))) - dead_anims(g)
    return out


# ---- section writer ---------------------------------------------------------------------------------------------------
def _pad(n: int, a: int = ALIGN) -> int:
    return (n + a - 1) // a * a


def loops(a: RAnim) -> bool:
    """The same derived rule as animconv.loop_flags (the tag has no loop flag): base animations named ... idle,
    move-*, airborne."""
    base = a.name.split("%", 1)[0].strip()
    return a.type == 0 and any(base.endswith(w) for w in animconv.LOOP_ACTIONS)


def anim_flags(a: RAnim) -> int:
    return (ANSM_LOOPS if loops(a) else 0) | (ANSM_WORLD_RELATIVE if a.flags & animconv.F_WORLD_RELATIVE else 0)


def _u8(v: int) -> int:
    return max(0, min(255, v))


class _Body:
    """The part of the section after the graph table; offsets handed out are absolute."""

    def __init__(self, base: int):
        self.base = base
        self.buf = bytearray()
        self.strings: dict[str, int] = {}
        self.lists: dict[tuple, tuple[int, int]] = {}

    def here(self) -> int:
        return self.base + len(self.buf)

    def align(self, a: int = ALIGN) -> None:
        self.buf += bytes(_pad(len(self.buf), a) - len(self.buf))

    def string(self, s: str) -> int:
        if not s:
            return 0
        if s not in self.strings:
            raw = s.encode("latin1") + b"\0"
            if len(raw) > MAX_NAME:
                raise ConvertError(f"string {s!r} longer than {MAX_NAME - 1}")
            self.strings[s] = self.here()
            self.buf += raw
        return self.strings[s]

    def slot_list(self, lst: list[int]) -> tuple[int, int]:
        lst = list(lst)
        while lst and lst[-1] < 0:
            lst.pop()
        if not lst:
            return 0, 0
        key = tuple(lst)
        if key not in self.lists:
            self.align(4)
            self.lists[key] = (self.here(), len(lst))
            self.buf += struct.pack("<%dH" % len(lst), *(NONE if v < 0 else v for v in lst))
        return self.lists[key]

    def table(self, records: list[bytes], a: int = ALIGN) -> tuple[int, int]:
        if not records:
            return 0, 0
        self.align(a)
        off = self.here()
        self.buf += b"".join(records)
        return off, len(records)


def _ref(r: tuple[int, int]) -> bytes:
    return struct.pack("<II", *r)


def _aim(a: tuple) -> bytes:
    return struct.pack(AIM_FMT, *a)


def _ik_table(body: _Body, ik: list[RIk]) -> tuple[int, int]:
    return body.table([struct.pack("<II", body.string(i.marker), body.string(i.attach)) for i in ik], 4)


def build_section_from_graphs(graphs: list[RGraph]) -> bytes:
    graphs = sorted(graphs, key=lambda g: g.tag_id)
    if len({g.tag_id for g in graphs}) != len(graphs):
        raise ConvertError("duplicate graph tag id")
    base = _pad(HEADER_SIZE + GRAPH_REC * len(graphs))
    body = _Body(base)
    body.buf += bytes(ALIGN)                       # offset base+0 is never a valid table (and "" is offset 0)
    grecs: list[bytes] = []
    anim_total = 0
    for g in graphs:
        n = len(g.anims)
        if n > 0xFFFF:
            raise ConvertError(f"{g.path}: {n} animations")
        for _, owner, s, a in g.refs():
            if a >= n:
                raise ConvertError(f"{g.path}: {owner} slot {s}: animation {a} out of range")
        # animations, name index
        arecs = []
        for a in g.anims:
            fps = 25 if a.flags & animconv.F_PAL else 30
            ticks = a.frame_count if fps == 30 else round(a.frame_count * 30 / fps)
            loop = a.loop_frame if 0 <= a.loop_frame < a.frame_count else 0
            arecs.append(struct.pack("<IHHHBBhhHhhhhBBHH", body.string(a.name), a.frame_count, min(ticks, 0xFFFF),
                                     anim_flags(a), a.type, fps, a.next, a.main, loop, a.key, a.key2, a.sound,
                                     a.sound_frame, _u8(a.foot_left), _u8(a.foot_right),
                                     max(0, min(65535, round(a.rel_weight * 65535))), 0))
        anim_ref = body.table(arecs)
        order = sorted(range(n), key=lambda i: (g.anims[i].name.encode("latin1"), i))
        body.align(4)
        nidx_off = body.here()
        body.buf += struct.pack("<%dH" % n, *order)
        nidx_ref = (nidx_off, n) if n else (0, 0)
        obj_ref = body.table([struct.pack("<hHHH", a, fn, fc, 0) for a, fn, fc in g.objects], 4)
        # units, bottom-up
        urecs = []
        for u in g.units:
            crecs = []
            for c in u.classes:
                trecs = []
                for t in c.types:
                    trecs.append(struct.pack("<I8s4x", body.string(t.label), _ref(body.slot_list(t.slots))))
                types_ref = body.table(trecs, 4)
                crecs.append(struct.pack("<III8s8s8s24s4x", body.string(c.name), body.string(c.grip), body.string(c.hold),
                                         _ref(body.slot_list(c.slots)), _ref(_ik_table(body, c.ik)), _ref(types_ref),
                                         _aim(c.aim)))
            classes_ref = body.table(crecs, 4)
            urecs.append(struct.pack("<I8s8s8s24s12x", body.string(u.label), _ref(classes_ref),
                                     _ref(body.slot_list(u.slots)), _ref(_ik_table(body, u.ik)), _aim(u.aim)))
        units_ref = body.table(urecs, 4)
        vrecs = []
        for v in g.vehicles:
            srecs = [struct.pack("<HhffI", mp, an, ext, comp, 0) for mp, an, ext, comp in v.suspension]
            vrecs.append(struct.pack("<ffHH8s8s4x", v.yaw[0], v.yaw[1], max(0, v.yaw[2]), max(0, v.yaw[3]),
                                     _ref(body.slot_list(v.slots)), _ref(body.table(srecs, 4))))
        veh_ref = body.table(vrecs, 4)
        dev_ref = body.table([struct.pack("<8s8x", _ref(body.slot_list(s))) for s in g.devices], 4)
        wo_ref = body.table([struct.pack("<8s8x", _ref(body.slot_list(s))) for s in g.wobjs], 4)
        fp_ref = body.table([struct.pack("<8s8x", _ref(body.slot_list(s))) for s in g.fps], 4)
        body.align(4)
        dmg_ref = (body.here(), len(g.damage)) if g.damage else (0, 0)
        body.buf += struct.pack("<%dH" % len(g.damage), *(NONE if v < 0 else v for v in g.damage))
        body.align(4)
        snd_ref = (body.here(), len(g.sounds)) if g.sounds else (0, 0)
        body.buf += struct.pack("<%dI" % len(g.sounds), *g.sounds)
        grecs.append(struct.pack("<II" + "8s" * 10 + "8x", g.tag_id, 0, _ref(anim_ref), _ref(nidx_ref), _ref(obj_ref),
                                 _ref(units_ref), _ref(veh_ref), _ref(dev_ref), _ref(wo_ref), _ref(fp_ref),
                                 _ref(dmg_ref), _ref(snd_ref)))
        anim_total += n
    body.align()
    total = base + len(body.buf)
    head = struct.pack("<4sHHIIIII36x", MAGIC, VERSION, 0, len(graphs), HEADER_SIZE, base, total, anim_total)
    out = head + b"".join(grecs) + bytes(base - HEADER_SIZE - GRAPH_REC * len(graphs)) + bytes(body.buf)
    assert len(head) == HEADER_SIZE and len(out) == total and all(len(r) == GRAPH_REC for r in grecs)
    return out


def build_section(map_path, tags: list[int] | None = None, everything: bool = False,
                  problems: list[str] | None = None) -> bytes:
    gs = load_graphs(Path(map_path) if not isinstance(map_path, MapTags) else map_path, tags, everything)
    if problems is not None:
        for g in gs:
            problems.extend(f"{g.path}: {p}" for p in g.problems)
    return build_section_from_graphs(gs)


# ---- reference reader (engine/src/world/ansm.c mirrors it) --------------------------------------------------------------
@dataclass
class SAnim:
    name: str
    frame_count: int
    ticks: int
    flags: int
    type: int
    fps: int
    next: int
    main: int
    loop_frame: int
    key: int
    key2: int
    sound: int
    sound_frame: int
    foot_left: int
    foot_right: int
    weight: float                 # cumulative variant probability

    @property
    def loops(self) -> bool:
        return bool(self.flags & ANSM_LOOPS)


@dataclass
class SType:
    label: str
    slots: list[int]


@dataclass
class SClass:
    name: str
    grip: str
    hold: str
    aim: tuple
    slots: list[int]
    ik: list[tuple]
    types: list[SType]


@dataclass
class SUnit:
    name: str
    aim: tuple
    slots: list[int]
    ik: list[tuple]
    classes: list[SClass]


@dataclass
class SVehicle:
    yaw: tuple
    slots: list[int]
    suspension: list[tuple]


@dataclass
class SGraph:
    tag_id: int
    anims: list[SAnim]
    name_index: list[int]
    objects: list[tuple]
    units: list[SUnit]
    vehicles: list[SVehicle]
    devices: list[list[int]]
    wobjs: list[list[int]]
    fps: list[list[int]]
    damage: list[int]
    sounds: list[int]

    def find_anim(self, name: str) -> int:
        """Exact name (lowest index); else a name without variant suffix finds the head of its variant chain: the first
        sorted entry 'name%...' (like ansm_find_anim)."""
        keys = [self.anims[i].name for i in self.name_index]
        lo = bisect.bisect_left(keys, name)
        while lo < len(keys) and keys[lo].startswith(name):
            if keys[lo] == name:
                return self.name_index[lo]
            if keys[lo][len(name)] == "%":
                return self.anims[self.name_index[lo]].main
            lo += 1
        return -1

    def find_unit(self, label: str) -> int:
        return next((i for i, u in enumerate(self.units) if u.name == label), -1)

    def find_class(self, unit: int, name: str) -> int:
        return next((i for i, c in enumerate(self.units[unit].classes) if c.name == name), -1)

    def find_type(self, unit: int, cls: int, label: str) -> int:
        return next((i for i, t in enumerate(self.units[unit].classes[cls].types) if t.label == label), -1)

    def find_class_for_type(self, unit: int, label: str) -> int:
        """The class block of the unit whose type block `label` has the most animations (ties: the first)."""
        best, score = -1, 0
        for ci, c in enumerate(self.units[unit].classes):
            for t in c.types:
                n = sum(1 for a in t.slots if a >= 0)
                if t.label == label and n > score:
                    best, score = ci, n
        return best

    def lookup(self, unit: str, cls: str, wtype: str, act: int) -> int:
        level, slot = act >> 8, act & 0xFF

        def pick(slots: list[int]) -> int:
            return slots[slot] if slot < len(slots) else -1
        if level == L_FP:
            return pick(self.fps[0]) if self.fps else -1
        if level == L_WOBJ:
            return pick(self.wobjs[0]) if self.wobjs else -1
        if level == L_VEHICLE:
            return pick(self.vehicles[0].slots) if self.vehicles else -1
        if level == L_DEVICE:
            return pick(self.devices[0]) if self.devices else -1
        ui = self.find_unit(unit)
        if ui < 0:
            return -1
        if level == L_UNIT:
            return pick(self.units[ui].slots)
        ci = self.find_class(ui, cls)
        if ci < 0:
            return -1
        if level == L_CLASS:
            return pick(self.units[ui].classes[ci].slots)
        ti = self.find_type(ui, ci, wtype)
        if level == L_TYPE and ti >= 0:
            return pick(self.units[ui].classes[ci].types[ti].slots)
        return -1

    def damage_anim(self, category: int, direction: int, region: int) -> int:
        regions = len(self.damage) // 16
        if not (0 <= category < 4 and 0 <= direction < 4 and 0 <= region < regions):
            return -1
        return self.damage[(category * 4 + direction) * regions + region]

    def pick_variant(self, k: int, r: float) -> int:
        """Walk the variant chain from its head by cumulative weight, `r` in [0, 1)."""
        cur = self.anims[k].main
        for _ in range(len(self.anims)):
            a = self.anims[cur]
            if a.next < 0 or r < a.weight:
                return cur
            cur = a.next
        return cur


@dataclass
class Section:
    data: bytes
    graphs: list[SGraph]

    def find_graph(self, tag_id: int) -> SGraph | None:
        return next((g for g in self.graphs if g.tag_id == tag_id), None)


def parse_section(data: bytes) -> Section:
    def need(cond: bool, msg: str) -> None:
        if not cond:
            raise ConvertError(msg)
    need(len(data) >= HEADER_SIZE, "shorter than the header")
    magic, version, flags, ng, goff, body, total, anim_total = struct.unpack_from("<4sHHIIIII", data)
    need(magic == MAGIC and version == VERSION, f"bad magic/version {magic!r}/{version}")
    need(total == len(data) and goff == HEADER_SIZE and body >= goff + GRAPH_REC * ng and body <= total, "inconsistent header")

    def ref(off: int, count: int, rec: int, what: str) -> int:
        need(count == 0 or (off >= body and off + count * rec <= total), f"{what}: table outside the section")
        return off

    def string(off: int) -> str:
        if off == 0:
            return ""
        need(body <= off < total, "string outside the section")
        end = data.find(b"\0", off, min(total, off + MAX_NAME))
        need(end >= 0, "unterminated string")
        return data[off:end].decode("latin1")

    def u16s(off: int, count: int, what: str) -> list[int]:
        ref(off, count, 2, what)
        return list(struct.unpack_from("<%dH" % count, data, off)) if count else []

    graphs, prev, seen = [], -1, 0
    for gi in range(ng):
        v = struct.unpack_from("<II" + "II" * 10, data, goff + GRAPH_REC * gi)
        tag, gflags, refs = v[0], v[1], [(v[2 + 2 * i], v[3 + 2 * i]) for i in range(10)]
        need(tag > prev, "graphs not in ascending tag id order")
        prev = tag
        (a_o, a_n), (n_o, n_n), (o_o, o_n), (u_o, u_n), (v_o, v_n), (d_o, d_n), (w_o, w_n), (f_o, f_n), (m_o, m_n), \
            (s_o, s_n) = refs
        na = a_n
        seen += na
        ref(a_o, na, ANIM_REC, "animations")
        anims = []
        for i in range(na):
            (noff, fc, ticks, fl, typ, fps, nxt, main, loop, k1, k2, snd, sndf, fl_, fr_, wt,
             _r) = struct.unpack_from("<IHHHBBhhHhhhhBBHH", data, a_o + ANIM_REC * i)
            need(-1 <= nxt < na and 0 <= main < na and -1 <= snd < s_n and fc >= 1 and typ <= 2, "bad animation record")
            need(fl & ~3 == 0 and loop < fc and (sndf < fc), "bad animation fields")
            anims.append(SAnim(string(noff), fc, ticks, fl, typ, fps, nxt, main, loop, k1, k2, snd, sndf, fl_, fr_,
                               wt / 65535.0))
        need(n_n == na, "name index count")
        nidx = u16s(n_o, n_n, "name index")
        need(sorted(nidx) == list(range(na)), "name index is not a permutation")
        need(all(anims[a].name.encode("latin1") <= anims[b].name.encode("latin1") for a, b in zip(nidx, nidx[1:])),
             "name index not sorted")

        def slots(off: int, count: int, what: str) -> list[int]:
            lst = u16s(off, count, what)
            need(all(x == NONE or x < na for x in lst), f"{what}: animation index out of range")
            return [-1 if x == NONE else x for x in lst]

        def iks(off: int, count: int) -> list[tuple]:
            ref(off, count, IK_REC, "ik")
            return [(string(a), string(b)) for a, b in (struct.unpack_from("<II", data, off + IK_REC * i) for i in range(count))]

        objs = []
        ref(o_o, o_n, OBJ_REC, "objects")
        for i in range(o_n):
            an, fn, fc, _p = struct.unpack_from("<hHHH", data, o_o + OBJ_REC * i)
            need(-1 <= an < na, "object animation")
            objs.append((an, fn, fc))
        units = []
        ref(u_o, u_n, UNIT_REC, "units")
        for ui in range(u_n):
            (noff, c_o, c_n, sl_o, sl_n, ik_o, ik_n) = struct.unpack_from("<IIIIIII", data, u_o + UNIT_REC * ui)
            aim = struct.unpack_from(AIM_FMT, data, u_o + UNIT_REC * ui + 28)
            classes = []
            ref(c_o, c_n, CLASS_REC, "classes")
            for ci in range(c_n):
                (cn, grip, hold, cs_o, cs_n, cik_o, cik_n, t_o, t_n) = struct.unpack_from("<9I", data, c_o + CLASS_REC * ci)
                caim = struct.unpack_from(AIM_FMT, data, c_o + CLASS_REC * ci + 36)
                types = []
                ref(t_o, t_n, TYPE_REC, "types")
                for ti in range(t_n):
                    lab, ts_o, ts_n = struct.unpack_from("<III", data, t_o + TYPE_REC * ti)
                    types.append(SType(string(lab), slots(ts_o, ts_n, "type slots")))
                classes.append(SClass(string(cn), string(grip), string(hold), caim, slots(cs_o, cs_n, "class slots"),
                                      iks(cik_o, cik_n), types))
            units.append(SUnit(string(noff), aim, slots(sl_o, sl_n, "unit slots"), iks(ik_o, ik_n), classes))
        vehicles = []
        ref(v_o, v_n, VEH_REC, "vehicles")
        for i in range(v_n):
            yr, yl, rc, lc, vs_o, vs_n, su_o, su_n = struct.unpack_from("<ffHHIIII", data, v_o + VEH_REC * i)
            ref(su_o, su_n, SUSP_REC, "suspension")
            susp = []
            for j in range(su_n):
                mp, an, ext, comp = struct.unpack_from("<HhffI", data, su_o + SUSP_REC * j)[:4]
                need(-1 <= an < na, "suspension animation")
                susp.append((mp, an, ext, comp))
            vehicles.append(SVehicle((yr, yl, rc, lc), slots(vs_o, vs_n, "vehicle slots"), susp))

        def lists(off: int, count: int, what: str) -> list[list[int]]:
            ref(off, count, LIST_REC, what)
            return [slots(*struct.unpack_from("<II", data, off + LIST_REC * i), what) for i in range(count)]

        devices, wobjs, fps = lists(d_o, d_n, "devices"), lists(w_o, w_n, "weapon objects"), lists(f_o, f_n, "first person")
        damage = slots(m_o, m_n, "damage")
        ref(s_o, s_n, 4, "sounds")
        sounds = list(struct.unpack_from("<%dI" % s_n, data, s_o)) if s_n else []
        graphs.append(SGraph(tag, anims, nidx, objs, units, vehicles, devices, wobjs, fps, damage, sounds))
    need(seen == anim_total, "animation total")
    return Section(data, graphs)


# ---- digest: one number that the C reader reproduces through its public API -------------------------------------------
DIGEST_SLOTS = 64                # every slot list is walked to this many entries (the largest real list has 51)


def digest(sec: Section) -> int:
    """FNV-1a (32 bit) over a canonical walk of everything the reader exposes (engine/tests/test_ansm.c walks the
    same way through ansm.h and must produce the same number: the cross-check of the two readers on any pack)."""
    h = 0x811C9DC5

    def feed(b: bytes) -> None:
        nonlocal h
        for c in b:
            h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF

    def u(v: int) -> None:
        feed(struct.pack("<I", v & 0xFFFFFFFF))

    def f(v: float) -> None:
        feed(struct.pack("<f", v))

    def st(x: str) -> None:
        feed(x.encode("latin1") + b"\0")

    def slots(lst: list[int], with_len: bool = True) -> None:
        if len(lst) > DIGEST_SLOTS:
            raise ConvertError("slot list longer than the digest walk")
        if with_len:                 # the reader exposes the length of unit / class / type / vehicle lists only
            u(len(lst))
        for i in range(DIGEST_SLOTS):
            u(lst[i] if i < len(lst) else -1)

    def aim(a: tuple) -> None:
        f(a[0]); f(a[1]); u(a[2]); u(a[3]); f(a[4]); f(a[5]); u(a[6]); u(a[7])

    def ik(lst: list[tuple]) -> None:
        u(len(lst))
        for m, a in lst:
            st(m)
            st(a)

    u(len(sec.graphs))
    for g in sec.graphs:
        u(g.tag_id)
        u(len(g.anims))
        for a in g.anims:
            st(a.name)
            for v in (a.frame_count, a.ticks, a.flags, a.type, a.fps, a.next, a.main, a.loop_frame, a.key, a.key2,
                      a.sound, a.sound_frame, g.sounds[a.sound] if a.sound >= 0 else 0, a.foot_left, a.foot_right,
                      round(a.weight * 65535)):
                u(v)
            u(g.find_anim(a.name))
        u(len(g.sounds))
        for t in g.sounds:
            u(t)
        u(len(g.objects))
        for an, fn, fc in g.objects:
            u(an); u(fn); u(fc)
        u(len(g.units))
        for un in g.units:
            st(un.name)
            aim(un.aim)
            slots(un.slots)
            ik(un.ik)
            u(len(un.classes))
            for c in un.classes:
                st(c.name); st(c.grip); st(c.hold)
                aim(c.aim)
                slots(c.slots)
                ik(c.ik)
                u(len(c.types))
                for t in c.types:
                    st(t.label)
                    slots(t.slots)
        u(len(g.vehicles))
        for v in g.vehicles:
            f(v.yaw[0]); f(v.yaw[1]); u(v.yaw[2]); u(v.yaw[3])
            slots(v.slots)
            u(len(v.suspension))
            for mp, an, ext, comp in v.suspension:
                u(mp); u(an); f(ext); f(comp)
        for blocks in (g.devices, g.wobjs, g.fps):
            u(len(blocks))
            for b in blocks:
                slots(b, False)
        u(len(g.damage))
        for i in range(len(g.damage) // 16 * 16):
            u(g.damage[i])
    return h


# ---- verification on real data ------------------------------------------------------------------------------------------
def _last_word(name: str) -> str:
    return name.split("%", 1)[0].strip().split(" ")[-1]


@dataclass
class Survey:
    refs: Counter = field(default_factory=Counter)             # (level) -> references
    exact: Counter = field(default_factory=Counter)
    alias: Counter = field(default_factory=Counter)
    other: Counter = field(default_factory=Counter)            # (level, slot name, last word) -> n
    unnamed: Counter = field(default_factory=Counter)          # (level, slot) -> n, slots without a name
    max_slot: Counter = field(default_factory=Counter)         # level -> highest used slot
    damage_total: int = 0
    damage_cat_ok: int = 0          # the category word of the animation name equals the table's category
    damage_dir_ok: int = 0          # ... and the direction word equals the table's direction
    damage_regions: dict = field(default_factory=lambda: defaultdict(Counter))   # region index -> last words
    aim_ok: int = 0
    aim_bad: int = 0
    aim_zero: int = 0


def survey_graph(g: RGraph, s: Survey | None = None) -> Survey:
    """Name agreement between slots and the animations they point at, damage table layout, aiming range vs overlay
    frame count."""
    s = s or Survey()
    for level, owner, slot, a in g.refs():
        if a < 0:
            continue
        s.refs[level] += 1
        s.max_slot[level] = max(s.max_slot[level], slot)
        want = slot_name(level, slot)
        word = _last_word(g.anims[a].name)
        if not want:
            s.unnamed[(level, slot)] += 1
        elif word == want:
            s.exact[level] += 1
        elif word in ALIASES.get((level, slot), ()):
            s.alias[level] += 1
        else:
            s.other[(level, want, word)] += 1
    if g.damage:
        regions = len(g.damage) // 16
        for i, a in enumerate(g.damage):
            if a < 0:
                continue
            cat, rest = divmod(i, 4 * regions)
            dr, region = divmod(rest, regions)
            nm = g.anims[a].name.split("%", 1)[0].split(" ")
            s.damage_total += 1
            if len(nm) >= 3 and nm[0] == DAMAGE_CATEGORIES[cat]:
                s.damage_cat_ok += 1
                if nm[1] == DAMAGE_DIRECTIONS[dr]:
                    s.damage_dir_ok += 1
                    s.damage_regions[region][nm[2]] += 1
    for u in g.units:
        for c in u.classes:
            right, left, rc, lc, down, up, dc, uc = c.aim
            for slot in (25, 26):
                if slot < len(c.slots) and c.slots[slot] >= 0:
                    fc = g.anims[c.slots[slot]].frame_count
                    if rc + lc + uc + dc == 0:
                        s.aim_zero += 1
                    elif fc == (rc + lc + 1) * (uc + dc + 1):
                        s.aim_ok += 1
                    else:
                        s.aim_bad += 1
    return s


def check_graph(g: RGraph) -> list[str]:
    """Structural invariants of a source graph (used by --verify and the tests)."""
    out = list(g.problems)
    n = len(g.anims)
    for k, a in enumerate(g.anims):
        if a.next >= 0 and g.anims[a.next].main != a.main and a.next != a.main:
            out.append(f"animation {a.name!r}: next animation {g.anims[a.next].name!r} is not in the same variant chain")
        if a.sound >= 0 and a.sound_frame >= a.frame_count:
            out.append(f"animation {a.name!r}: sound frame {a.sound_frame} >= {a.frame_count} frames")
        if a.key >= a.frame_count or a.key2 >= a.frame_count:
            out.append(f"animation {a.name!r}: key frame outside the animation")
    for _, owner, s, a in g.refs():
        if a >= n:
            out.append(f"{owner} slot {s}: {a} out of range")
    return out


# ---- CLI --------------------------------------------------------------------------------------------------------------------
def _mib(n: int) -> str:
    return f"{n / 2**20:.2f} MiB"


def describe(sec: Section, names: dict[int, str] | None = None) -> str:
    lines = [f"{len(sec.graphs)} graphs, {sum(len(g.anims) for g in sec.graphs)} animations, {len(sec.data)} bytes",
             f"{'tag id':<11}{'anims':>6}{'units':>6}{'class':>6}{'types':>6}{'fp':>4}{'veh':>4}{'dev':>4}{'dmg':>5}{'snd':>5}  path"]
    for g in sec.graphs:
        classes = sum(len(u.classes) for u in g.units)
        types = sum(len(c.types) for u in g.units for c in u.classes)
        lines.append(f"{g.tag_id:#010x}{len(g.anims):>6}{len(g.units):>6}{classes:>6}{types:>6}{len(g.fps):>4}"
                     f"{len(g.vehicles):>4}{len(g.devices):>4}{len(g.damage):>5}{len(g.sounds):>5}  "
                     f"{(names or {}).get(g.tag_id, '')}")
    return "\n".join(lines)


def dump_graph(g: RGraph) -> str:
    lines = [f"{g.path} ({g.tag_id:#010x}): {len(g.anims)} animations, {len(g.sounds)} sound references"]

    def nm(a: int) -> str:
        return g.anims[a].name if a >= 0 else "-"
    for u in g.units:
        lines.append(f"  unit {u.label!r}: slots " + ", ".join(f"{slot_name(L_UNIT, s) or s}={nm(a)}" for s, a in enumerate(u.slots) if a >= 0))
        for c in u.classes:
            lines.append(f"    class {c.name!r} (grip {c.grip!r}, hold {c.hold!r}): " +
                         ", ".join(f"{slot_name(L_CLASS, s) or s}={nm(a)}" for s, a in enumerate(c.slots) if a >= 0))
            for t in c.types:
                lines.append(f"      type {t.label!r}: " + ", ".join(f"{slot_name(L_TYPE, s) or s}={nm(a)}" for s, a in enumerate(t.slots) if a >= 0))
    for i, sl in enumerate(g.fps):
        lines.append(f"  first person {i}: " + ", ".join(f"{slot_name(L_FP, s) or s}={nm(a)}" for s, a in enumerate(sl) if a >= 0))
    for i, sl in enumerate(g.wobjs):
        lines.append(f"  weapon object {i}: " + ", ".join(f"{slot_name(L_WOBJ, s) or s}={nm(a)}" for s, a in enumerate(sl) if a >= 0))
    for i, v in enumerate(g.vehicles):
        lines.append(f"  vehicle {i}: steering {math.degrees(v.yaw[0] * v.yaw[2]):.0f}/{math.degrees(v.yaw[1] * v.yaw[3]):.0f} deg, " +
                     ", ".join(f"{slot_name(L_VEHICLE, s) or s}={nm(a)}" for s, a in enumerate(v.slots) if a >= 0) +
                     f"; {len(v.suspension)} suspension")
    for i, sl in enumerate(g.devices):
        lines.append(f"  device {i}: " + ", ".join(f"{slot_name(L_DEVICE, s) or s}={nm(a)}" for s, a in enumerate(sl) if a >= 0))
    for a, fn, fc in g.objects:
        lines.append(f"  object overlay {nm(a)} function {fn} controls {fc}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map, or an .ansm.hp2 with --info")
    ap.add_argument("-o", "--out", type=Path, help="write an .hp2 pack holding the ASTB section")
    ap.add_argument("--all", action="store_true", help="every model_animations tag (default: referenced_anims)")
    ap.add_argument("--tags", nargs="+", help="only these tags (0x tag ids or path substrings)")
    ap.add_argument("--info", action="store_true", help="print per graph: animations and table sizes")
    ap.add_argument("--verify", action="store_true", help="name agreement of slots, invariants, section round trip")
    ap.add_argument("--digest", action="store_true", help="print the digest of the section (see test_ansm.c)")
    ap.add_argument("--dump", nargs="+", metavar="PATH", help="print the state tables of graphs matching these path substrings")
    args = ap.parse_args(argv)
    if args.input.suffix == ".hp2":
        sec = parse_section(hp2.parse(args.input.read_bytes()).read("ASTB"))
        print(f"{digest(sec):08x}" if args.digest else describe(sec))
        return 0
    t0 = time.time()
    tags = MapTags(args.input)
    paths = {e.tag_id: e.path for e in tags.idx.entries if e.group == "antr"}
    sel = None
    if args.tags:
        sel = animconv.parse_tags(args.tags, paths)
    graphs = load_graphs(tags, sel, args.all)
    data = build_section_from_graphs(graphs)
    dt = time.time() - t0
    for g in graphs:
        for p in g.problems:
            print(f"warning: {g.path}: {p}", file=sys.stderr)
    sec = parse_section(data)
    if args.info or args.verify:
        print(describe(sec, {g.tag_id: g.path for g in graphs}))
        print(f"{_mib(len(data))}, {dt:.1f} s")
    if args.dump:
        for g in graphs:
            if any(d.lower() in g.path.lower() for d in args.dump):
                print(dump_graph(g))
    if args.verify:
        sv = Survey()
        bad = 0
        for g in graphs:
            survey_graph(g, sv)
            for p in check_graph(g):
                print(f"problem: {g.path}: {p}")
                bad += 1
        for level in range(7):
            if sv.refs[level]:
                print(f"{LEVEL_NAMES[level]:<8} {sv.refs[level]:>6} references, highest slot {sv.max_slot[level]:>2}: "
                      f"name = slot {sv.exact[level]}, alias {sv.alias[level]}, other {sum(n for (lv, _, _), n in sv.other.items() if lv == level)}")
        for (lv, want, word), n in sv.other.most_common(12):
            print(f"  mismatch {LEVEL_NAMES[lv]} slot {want!r} -> {word!r}: {n}")
        if sv.unnamed:
            print("  references to unnamed slots:", dict(sv.unnamed))
        print(f"damage tables: {sv.damage_total} entries, category word agrees for {sv.damage_cat_ok}, direction too for "
              f"{sv.damage_dir_ok} (the rest reuse a neighbouring animation); region index -> most common region word: " +
              ", ".join(f"{r}:{c.most_common(1)[0][0]}" for r, c in sorted(sv.damage_regions.items())))
        print(f"aiming range: overlay frame count == (right+left+1)*(up+down+1) for {sv.aim_ok}, differs for {sv.aim_bad}, "
              f"no range data for {sv.aim_zero}")
        print(f"{bad} structural problems")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        pack = hp2.build(args.input.stem, [("ASTB", data)])
        args.out.write_bytes(pack)
        print(f"wrote {args.out} ({len(pack) / 2**10:.1f} KiB, ASTB {len(data) / 2**10:.1f} KiB raw, {len(graphs)} graphs, {dt:.1f} s)")
    elif not (args.info or args.verify or args.dump):
        ap.error("-o, --info, --verify or --dump is required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
