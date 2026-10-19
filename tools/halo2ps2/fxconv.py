#!/usr/bin/env python3
"""Convert the effects of a Xbox map (``effect``, ``particle``, ``decal``, ``light``, ``point_physics``,
``particle_system`` tags and the bitmaps they draw) into the PS2 ``FXD0`` and ``FXTX`` sections
(docs/22-fx-section.md; the reader is engine/src/world/fxdef.c, its contract engine/src/world/fxdef.h).
Written to ``assets/hp2/<map>.fx.hp2``.  Pure stdlib.

Usage:
    python3 -I tools/halo2ps2/fxconv.py assets/decompressed/a10.map -o assets/hp2/a10.fx.hp2 [--all] [--budget BYTES]
    python3 -I tools/halo2ps2/fxconv.py --info assets/decompressed/a10.map      # what would be converted
    python3 -I tools/halo2ps2/fxconv.py --info assets/hp2/a10.fx.hp2            # what a pack holds

Which effects (``effect_closure``)
    Every tag of the groups matg foot weap proj jpt! bipd vehi eqip garb itmc is scanned, inside its own
    memory (its data block up to the next tag's data block: the root struct and all its nested blocks are
    contiguous in tag space, verified by the references found), for 16-byte tag references of group ``effe``
    whose tag id resolves to an effect tag.  That is the gameplay set: weapon firing / reload / impact effects, the
    projectiles' material responses and detonations, the globals' material impact effects (through
    ``foot``), unit effects.  The closure then follows what converted effects create: particle tags
    (collision and death effects), spawned object tags (projectiles, garbage: their effect references).
    ``--all`` converts every ``effect`` tag of the map as well (devices, cinematics, scenery emitters).

Layouts, engine version 5 (offsets inside the tag's own data block; a tag reference is 16 bytes: group
(byte reversed), path pointer, path length, tag id; a reflexive is ``u32 count, u32 address, u32 pad``).
"verified" = checked on the retail data (all ten campaign maps; cross-checks named), "unverified" = taken from
the public tag documentation (Invader's definitions) or a plausibility argument the data does not contradict.

effect (``effe``, struct 64 B)
    +0x00 u32 flags (bit 0 deleted when attachment deactivates, bit 1 "must be deterministic" Xbox, bit 2 same on PC,
          bit 3 disabled by the blood setting), +0x04 s16 loop start event, +0x06 s16 loop stop event (-1 = none),
          +0x08 f32 maximum damage radius (0 on most; 4..60 on explosions), +0x28 locations (32 B: marker name),
          +0x34 events (68 B)    [verified on the 3134 effect tags of the ten campaign maps: 64 + 32 x locations + 68 x events +
          104 x parts + 232 x particles equals the memory between the tag's data address and the next tag's, i.e. the
          strides and the +0x28 / +0x34 / +0x2C / +0x38 reflexives are right]
    event: +0x04 f32 skip fraction, +0x08 f32 delay bounds (min, max), +0x10 f32 duration bounds, +0x2C parts (104 B),
          +0x38 particles (232 B)   [verified: 0x00 and 0x18..0x2B are zero, the fields move with the bounds seen on
          rocket launcher / explosion effects: delay 0.8..0.9 s, duration 2..4 s]
    part: +0x00 u16 create in (0 any environment, 1 air only, 2 water only, 3 space only), +0x02 u16 violence mode (0 either,
          1 violent only, 2 nonviolent only), +0x04 u16 location (marker index), +0x06 u16 flags (bit 0 "face down
          regardless of location", decals), +0x14 u32 type class, +0x18 tag reference (the thing created: group
          jpt! damage effect, ligh light, snd! sound, deca decal, pctl particle system, anything else = an object tag
          class obje), +0x40 f32 velocity bounds, +0x48 f32 velocity cone angle, +0x4C f32 angular velocity bounds,
          +0x54 f32 radius modifier bounds, +0x60 / +0x64 u32 "a scales values", "b scales values"
          [verified: the class at +0x14 equals the group of the reference at +0x18 on every part except the object parts (class
          ``obje``, reference group garb / proj / scen ...); every reference resolves to a tag of its group; the create-in
          and violence values stay inside their enums; the a / b scales are 0 on every part]
    effect particle: +0x00 u16 create in, +0x02 u16 violence mode, +0x04 u16 create (0 independent of camera mode, 1 only in first
          person, 2 only in third person, 3 in first person if possible), +0x08 u16 location, +0x0C f32 relative direction
          yaw, +0x10 pitch, +0x14 f32[3] relative offset, +0x20 f32[3] relative direction vector (= the unit vector of yaw
          and pitch, not stored here), +0x54 tag reference (group ``part``), +0x64 u32 flags (bit 0 stay attached to
          marker, bit 1 random initial angle, bit 2 tint from object color, bit 3 interpolate tint as hsv, bit 4 across
          the long hue path), +0x68 u16 distribution function (0 start, 1 end, 2 constant, 3 buildup, 4 falloff, 5 buildup
          and falloff), +0x6C s16 count bounds, +0x70 f32 distribution radius bounds, +0x84 f32 velocity bounds,
          +0x8C f32 velocity cone angle, +0x90 f32 angular velocity bounds, +0xA0 f32 radius bounds, +0xB0 f32[4] tint lower
          bound (A, R, G, B), +0xC0 tint upper bound, +0xE0 / +0xE4 u32 a / b scales values
          [verified on all effect tags of the ten maps (17 000+ emitters): the vector at +0x20 equals (cos p cos y, cos p sin y,
          sin p) to 1e-3 on every one, which also pins yaw at +0x0C and pitch at +0x10; every location index is below the
          location count except 3 of the Scorpion's secondary fire bullet (index 1 of 1, cleared to "none"); tints are
          0..1; counts 0..2000 (the largest are cinematic effects), the distribution functions 0..5]
particle (``part``, 356 B)
    +0x00 u32 flags (bit 0 can animate backwards, 1 animation stops at rest, 2 animation starts on random frame, 3 animate
          once per frame, 4 dies at rest, 5 dies on contact with structure, 6 tint from diffuse texture, 7 dies on contact
          with water, 8 dies on contact with air, 9 self illuminated, 10 / 11 random horizontal / vertical mirroring)
    +0x04 bitmap ref, +0x14 point_physics ref, +0x24 material effects ref (unused here), +0x38 f32 lifespan bounds (s),
    +0x40 f32 fade in time, +0x44 f32 fade out time, +0x48 collision effect ref (effe), +0x58 death effect ref (effe),
    +0x68 f32 minimum size, +0x74 f32 radius animation (from, to: a multiple of the effect particle's radius over the
    life), +0x80 f32 animation rate bounds (frames per second), +0x88 f32 contact deterioration, +0x8C f32 fade start
    size, +0x90 f32 fade end size, +0x98 s16 first sequence index, +0x9A s16 initial sequence count, +0x9C s16 looping
    sequence count, +0x9E s16 final sequence count, +0xA8 f32 sprite size, +0xAC u16 orientation (0 screen facing,
    1 parallel to direction, 2 perpendicular), +0xD4 u16 shader type, +0xD8 u16 shader flags (bit 0 sort bias, bit 1
    nonlinear tint, bit 2 don't overdraw first person weapon), +0xDA u16 framebuffer blend function (0 alpha blend,
    1 multiply, 2 double multiply, 3 add, 4 subtract, 5 component min, 6 component max, 7 alpha-multiply add),
    +0xDC u16 framebuffer fade mode (0 none, 1 fade when perpendicular, 2 fade when parallel), +0xFC secondary bitmap
    (dropped).  [verified on 1052 particle tags: struct size 356 on every one (the data blocks are contiguous); the shader
    fields take only the values listed; lifespan min <= max; the offsets of the first three groups of fields agree with
    the public definitions and with the values (lifespans 0.07..30 s, fade sizes 5 / 4 px, sprite sizes 0.006..1)]
    UNVERIFIED and NOT enforced: the sequence fields.  Read as sequence indices (first index, then counts of whole sequences
    played initially / looped / at the end, each sequence being a list of sprites shown at the animation rate) they fit
    most retail particles (a bitmap with one sequence of 1..60 sprites and counts 0 / 1) but 10 of 914 particles have a
    first index past the bitmap's sequences and 38 have first + counts beyond them, so the reader hands the raw values over
    and ``fxdef_sprite`` clamps.
point physics (``pphy``, 64 B)
    +0x00 u32 flags (bit 0 flamethrower particle collision, 1 collides with structures, 2 collides with water surface, 3 uses
    simple wind, 4 uses damped wind, 5 no gravity), +0x04 f32 mass scale (not kept), +0x08 f32 water gravity scale, +0x0C
    f32 air gravity scale, +0x20 f32 density, +0x24 air friction, +0x28 water friction, +0x2C surface friction, +0x30
    elasticity.  [verified: the flags 0x3 on rocks, 0x20 on vacuum / space debris, surface friction and elasticity
    plausible (rubber 0.05 / 0.65, stones 0.1 / 0.35, glass); the gravity scales are -0.999 on stones (falls),
    +0.47 on hot smoke (rises), -0.02 on cold smoke.]  Kept as the file stores them: a NEGATIVE air gravity scale pulls
    the particle DOWN (unverified sign convention, argued from those three samples).
decal (``deca``, 268 B)
    +0x00 u16 flags (bit 0 geometry inherited by next decal, 1 interpolate colour in hsv, 2 more colours, 3 no random
    rotation, 4 water effect, 7 animation loop, 8 preserve aspect), +0x02 u16 type (0 scratch, 1 splatter, 2 burn, 3
    painted sign), +0x04 u16 layer (0 primary, 1 secondary, 2 light, 3 alpha tested, 4 water), +0x08 next decal in chain
    (ref deca), +0x18 f32 radius bounds, +0x2C f32 intensity bounds, +0x34 f32[3] colour lower (R, G, B), +0x40 colour upper,
    +0x58 s16 animation loop frame, +0x5A s16 animation speed, +0x78 f32 lifetime bounds (s), +0x80 f32 decay time
    bounds, +0xC0 u16 framebuffer blend function (as above), +0xD8 map ref (bitmap), +0xFC f32 maximum sprite extent
    [verified on 428 decal tags: struct size 268 on all; radius 0.035..1.25 wu, lifetime 0.1..300 s, blend 1..6, colours 0..1,
    layers 0..2, map reference in the first position of every one]
light (``ligh``, 352 B)
    +0x00 u32 flags (bit 0 dynamic, ...), +0x04 f32 radius, +0x08 f32 radius modifier bounds, +0x10 f32 falloff angle,
    +0x14 f32 cutoff angle (3.1416 = omnidirectional), +0x30 u32 colour interpolation flags, +0x38 f32[4] colour lower
    bound (A, R, G, B), +0x48 colour upper bound (CORRECTED 2026-10-19: the earlier +0x34 / +0x44 read a pad word too early and turned every light's colour
    by one channel: muzzle flashes cyan, the alert lights green; at +0x48 the assault rifle's flash is (0, 0.96, 0.91, 0.69), the frag explosion (1, 1, 0.81, 0.36),
    the burning flame (1, 1, 0.98, 0.32)), +0xF4 f32 duration, +0xFA u16 falloff function.
    [verified on 507 light tags: struct size 352; radius 0.5..15 wu (muzzle flash 2, explosion 10), colours 0..1, falloff
    function 0..4, cut-off angle pi on the light effects (spot cones 0.35..1.57 rad on flashlights and headlights, which
    effects do not create); duration 0 on lights without one and 3..180 on explosion lights, unit unverified (seconds)]
particle system (``pctl``, root 104 B + nested blocks): +0x38 point physics ref, +0x48 u16 system update physics,
    +0x50 physics constants (4 B floats), +0x5C particle types (128 B)
    type: +0x00 char[32] name, +0x20 u32 flags (bit 4 particles die in water ... see below), +0x24 u16 initial particle
    count, +0x28 u16 complex sprite render mode (0 simple, 1 rotational), +0x2C f32 radius, +0x54 u16 particle creation
    physics (0 default, 1 explosion, 2 jet), +0x5C physics constants, +0x68 states (192 B), +0x74 particle states (376 B)
    state: +0x00 name, +0x20 f32 duration bounds, +0x28 f32 transition time bounds, +0x34 f32 scale / animation rate /
    rotation rate multiplier, +0x40 f32[4] colour multiplier (A R G B), +0x50 f32 radius multiplier, +0x54 f32 minimum
    particle count, +0x58 f32 particle creation rate, +0xB0 u16 creation physics, +0xB2 u16 update physics, +0xB4 constants
    particle state: +0x00 name, +0x20 duration bounds, +0x28 transition time bounds, +0x30 bitmap ref, +0x40 u16 sequence
    index, +0x48 f32 scale bounds, +0x50 f32 animation rate bounds, +0x58 f32 rotation rate bounds (radians per
    second), +0x60 f32[4] colour 1 (A R G B), +0x70 colour 2, +0x80 f32 radius multiplier, +0x84 point physics ref,
    +0xE0 u16 shader flags, +0xE2 u16 framebuffer blend function, +0xE4 u16 fade mode, +0x16C physics constants
    [verified on all 108 particle systems of the ten maps: the nested blocks tile the tag's memory exactly (root 104 + constants
    + types + states + particle states + their constants = the data size: 1760 B on a 1 type / 2 state / 3 particle state
    system), the names ("fire to smoke", "birth", "death", "fire", "smoke plume"), the references (bitmap at +0x30, physics
    at +0x84), rotation rates of 150..400 degrees per second, colours 0..1 (fire yellow to orange, smoke grey), blend 3
    (add) on fire and 0 on smoke, every particle state's sequence index inside its bitmap's sequences.
    Unverified: how Halo's simulation consumes the durations, transition times and multipliers; they are exported
    verbatim for the effect renderer to approximate.]
bitmap (``bitm``, 108 B): +0x00 u16 type (0 2d, 1 3d, 2 cube, 3 sprites, 4 interface), +0x54 sequences (64 B: char[32] name,
    u16 first bitmap index, u16 bitmap count, +0x34 sprites reflexive), sprite (32 B: u16 bitmap index, +0x08 f32 left, right,
    top, bottom (0..1 of the image), +0x18 f32 registration x, y).  [verified on the 7823 sprites of the bitmaps the effects use:
    left < right and top < bottom, the bitmap index is below the image count, the rectangles lie in -0.0625..1 (one of
    them overhangs by 1/16 and is clamped); registration points -0.17..0.5 (units unverified, kept as stored);
    particle and decal bitmaps are sprite sheets (type 3: 1..8 images, 1..60 sprites per sequence) except bullet holes,
    tread marks and a flare sheet (type 2d: one sequence of ``count`` whole images, converted here to sprites covering
    each image)]
object tag spawned by a part: +0x28 model reference (group ``mode``) [verified: every garb tag the effects create on the ten maps
    (1..11 per map) has a resolvable ``mode`` reference there; projectiles mostly have none (bullets, beams); the same
    offset is verified for scen / mach / bipd ... in scenconv.py]
Bounds: retail data stores some bounds the wrong way round (delay 0.75..0.70, angular velocity 6.28..-6.28: 128..245 per map);
    Halo picks a value between the two whichever is larger, so the converter stores (min, max).

PS2 output: docs/22-fx-section.md (FXD0 tables, FXTX = TEXS blob keyed by bitmap tag id).
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import time
from collections import Counter, deque
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import bitmaps, hp2, tagindex, texconv  # noqa: E402

MAGIC = b"FXD0"
VERSION = 1
HEADER_SIZE = 192
NULL_ID = 0xFFFFFFFF
NONE16 = 0xFFFF
NAME_LEN = 32
SHORT_NAME = 16

# gameplay roots: groups whose memory is scanned for effect references
ROOT_GROUPS = ("matg", "foot", "weap", "proj", "jpt!", "bipd", "vehi", "eqip", "garb", "itmc")

# part kinds
K_NONE, K_DAMAGE, K_OBJECT, K_LIGHT, K_SOUND, K_DECAL, K_PCTL, K_EFFECT = range(8)
KIND_NAMES = ("none", "damage_effect", "object", "light", "sound", "decal", "particle_system", "effect")
KIND_OF_GROUP = {"jpt!": K_DAMAGE, "ligh": K_LIGHT, "snd!": K_SOUND, "deca": K_DECAL, "pctl": K_PCTL, "effe": K_EFFECT}

# tag layouts (docstring)
EFFE_SIZE, EVENT_SIZE, PART_SIZE, PCL_SIZE = 64, 68, 104, 232
LOC_SIZE = 32
PART_TAG_SIZE, DECAL_SIZE, LIGHT_SIZE, PHYS_SIZE = 356, 268, 352, 64
PCTL_ROOT, PCTL_TYPE, PCTL_STATE, PCTL_PSTATE, PCTL_CONST = 104, 128, 192, 376, 4
BITMAP_SEQ, BITMAP_SPRITE = 64, 32
MAX_EVENTS, MAX_PARTS, MAX_LOCS = 256, 256, 256
MAX_CONSTS = 16

FRAMEBUFFER_TO_TEX_BLEND = {0: texconv.BLEND_ALPHA, 1: texconv.BLEND_MULTIPLY, 2: texconv.BLEND_DOUBLE_MULTIPLY,
                            3: texconv.BLEND_ADD, 4: texconv.BLEND_SUBTRACT, 5: texconv.BLEND_ALPHA,
                            6: texconv.BLEND_ALPHA, 7: texconv.BLEND_ADD}

# FXTX texture policy: sprite sheets are single-mip (mips would blend neighbouring sprites)
FX_DEFAULT_BUDGET = 1 << 20          # bytes of pixels + CLUTs; the 6 MiB pool also holds BSP, lightmap and object textures
FX_FULL_RES_SIDE = 64                # sheets up to this side keep their size, larger ones are halved once
FX_MAX_DIM = 128


class FxError(ValueError):
    pass


# ---- table record formats (docs/22-fx-section.md; sizes asserted below) -------------------------------------------
# order of the tables in the header (offset u32, count u32 pairs from 0x10)
TABLES = ("effects", "locations", "events", "parts", "emitters", "particles", "physics", "decals", "lights",
          "pctl", "pctl_types", "pctl_states", "pctl_pstates", "consts", "bitmaps", "sequences", "sprites")
T = {n: i for i, n in enumerate(TABLES)}
HEADER_FMT = "<4sHHII" + "II" * len(TABLES)
EFFECT_FMT = "<IIhhfIHHI20x"
EVENT_FMT = "<f2f2fIHHI16x"
PART_FMT = "<BBHHBBI4sI2ff2f2fII24x"
EMITTER_FMT = "<BBBxHHff3fIHHhh2f2ff2f2f4B4BIII32x"
PARTICLE_FMT = "<IIHHII2f3f2f2f3f4hf4BBB26x"
PHYSICS_FMT = "<II7f12x"
DECAL_FMT = "<IHBBIHH2f2f3f3f2f2fhhf16x"
LIGHT_FMT = "<IIf2f2fI4f4ffH10x"
PCTL_FMT = "<IHHIH18x"
PCTL_TYPE_FMT = "<16sIHBBfIHHIHHIHH12x"
PCTL_STATE_FMT = "<16s2f2f3f4f3fHHIH"              # padded to 96 below
PCTL_PSTATE_FMT = "<16s2f2fHH2f2f2f4f4ffHBBHIH"     # padded to 112 below
BITMAP_FMT = "<IIHBBHH16x"
SEQUENCE_FMT = "<IHH"
SPRITE_FMT = "<HHffffff4x"
RECORDS = {"effects": EFFECT_FMT, "locations": f"<{NAME_LEN}s", "events": EVENT_FMT, "parts": PART_FMT,
           "emitters": EMITTER_FMT, "particles": PARTICLE_FMT, "physics": PHYSICS_FMT, "decals": DECAL_FMT,
           "lights": LIGHT_FMT, "pctl": PCTL_FMT, "pctl_types": PCTL_TYPE_FMT, "pctl_states": PCTL_STATE_FMT,
           "pctl_pstates": PCTL_PSTATE_FMT, "consts": "<f", "bitmaps": BITMAP_FMT, "sequences": SEQUENCE_FMT,
           "sprites": SPRITE_FMT}
REC_SIZE = {"effects": 48, "locations": 32, "events": 48, "parts": 80, "emitters": 128, "particles": 112,
            "physics": 48, "decals": 96, "lights": 80, "pctl": 32, "pctl_types": 64, "pctl_states": 96,
            "pctl_pstates": 112, "consts": 4, "bitmaps": 32, "sequences": 8, "sprites": 32}
# the two variable-size formats get their padding computed so the record sizes above hold
for _n in ("pctl_states", "pctl_pstates"):
    RECORDS[_n] += f"{REC_SIZE[_n] - struct.calcsize(RECORDS[_n])}x"
assert struct.calcsize(HEADER_FMT) <= HEADER_SIZE
for _n, _f in RECORDS.items():
    assert struct.calcsize(_f) == REC_SIZE[_n], (_n, struct.calcsize(_f), REC_SIZE[_n])


# ---- data model ---------------------------------------------------------------------------------------------------
@dataclass
class Effect:
    tag_id: int
    path: str
    flags: int
    loop_start: int
    loop_stop: int
    damage_radius: float
    locations: list[str]
    events: list["Event"]


@dataclass
class Event:
    skip: float
    delay: tuple[float, float]
    duration: tuple[float, float]
    parts: list["Part"]
    emitters: list["Emitter"]


@dataclass
class Part:
    create_in: int
    violence: int
    location: int
    flags: int
    kind: int
    tag_id: int
    group: str
    velocity: tuple[float, float]
    cone: float
    angvel: tuple[float, float]
    radius_mod: tuple[float, float]
    a_scales: int
    b_scales: int


@dataclass
class Emitter:
    create_in: int
    violence: int
    create: int
    location: int
    particle_tag: int
    yaw: float
    pitch: float
    offset: tuple[float, float, float]
    flags: int
    distribution: int
    count: tuple[int, int]
    dist_radius: tuple[float, float]
    velocity: tuple[float, float]
    cone: float
    angvel: tuple[float, float]
    radius: tuple[float, float]
    tint_lo: tuple[int, int, int, int]          # R G B A, 0..255
    tint_hi: tuple[int, int, int, int]
    a_scales: int
    b_scales: int


@dataclass
class ParticleDef:
    tag_id: int
    path: str
    flags: int
    bitmap_tag: int
    physics_tag: int
    collision_effect: int           # tag id of an effect, damage effect or sound (kind below), NULL_ID = none
    death_effect: int
    collision_kind: int             # K_EFFECT / K_DAMAGE / K_SOUND / K_NONE
    death_kind: int
    lifespan: tuple[float, float]
    fade_in: float
    fade_out: float
    min_size: float
    radius_anim: tuple[float, float]
    anim_rate: tuple[float, float]
    contact_deterioration: float
    fade_start_size: float
    fade_end_size: float
    seq_first: int
    seq_initial: int
    seq_loop: int
    seq_final: int
    sprite_size: float
    orientation: int
    blend: int
    fade_mode: int
    shader_flags: int


@dataclass
class PhysicsDef:
    tag_id: int
    path: str
    flags: int
    density: float
    air_friction: float
    water_friction: float
    surface_friction: float
    elasticity: float
    air_gravity: float
    water_gravity: float


@dataclass
class DecalDef:
    tag_id: int
    path: str
    flags: int
    type: int
    layer: int
    next_tag: int
    bitmap_tag: int
    blend: int
    radius: tuple[float, float]
    intensity: tuple[float, float]
    color_lo: tuple[float, float, float]
    color_hi: tuple[float, float, float]
    lifetime: tuple[float, float]
    decay: tuple[float, float]
    loop_frame: int
    anim_speed: int
    max_extent: float


@dataclass
class LightDef:
    tag_id: int
    path: str
    flags: int
    radius: float
    radius_mod: tuple[float, float]
    falloff_angle: float
    cutoff_angle: float
    interp_flags: int
    color_lo: tuple[float, float, float, float]       # A R G B
    color_hi: tuple[float, float, float, float]
    duration: float
    falloff_fn: int


@dataclass
class PctlState:
    name: str
    duration: tuple[float, float]
    transition: tuple[float, float]
    scale_mult: float
    anim_mult: float
    rot_mult: float
    color_mult: tuple[float, float, float, float]
    radius_mult: float
    min_count: float
    creation_rate: float
    creation_physics: int
    update_physics: int
    consts: list[float]


@dataclass
class PctlPState:
    name: str
    duration: tuple[float, float]
    transition: tuple[float, float]
    bitmap_tag: int
    sequence: int
    scale: tuple[float, float]
    anim_rate: tuple[float, float]
    rot_rate: tuple[float, float]
    color1: tuple[float, float, float, float]
    color2: tuple[float, float, float, float]
    radius_mult: float
    physics_tag: int
    blend: int
    fade_mode: int
    shader_flags: int
    consts: list[float]


@dataclass
class PctlType:
    name: str
    flags: int
    initial_count: int
    render_mode: int
    radius: float
    creation_physics: int
    consts: list[float]
    states: list[PctlState]
    pstates: list[PctlPState]


@dataclass
class PctlDef:
    tag_id: int
    path: str
    physics_tag: int
    update_physics: int
    types: list[PctlType]


@dataclass
class Sprite:
    image: int                      # index of the image inside the bitmap tag
    left: float
    right: float
    top: float
    bottom: float
    reg: tuple[float, float]


@dataclass
class BitmapDef:
    tag_id: int
    path: str
    kind: int                       # bitmap tag type: 0 2d, 3 sprites ...
    images: int
    sequences: list[list[Sprite]]


@dataclass
class FxSet:
    effects: list[Effect]
    particles: list[ParticleDef]
    physics: list[PhysicsDef]
    decals: list[DecalDef]
    lights: list[LightDef]
    pctl: list[PctlDef]
    bitmaps: list[BitmapDef]
    objects: list[int]                          # object tags created by parts (not converted, models exported)
    problems: list[str] = field(default_factory=list)
    stats: Counter = field(default_factory=Counter)
    attach: list[tuple[int, int, int, str]] = field(default_factory=list)   # (object tag, kind ATT_*, attached tag, marker name): FXAT


# ---- tag region access ---------------------------------------------------------------------------------------------
class Region:
    """Bounds-checked reads of the tag region of a decompressed map (offsets are region offsets)."""

    def __init__(self, m: texconv.MapData):
        self.m = m
        self.data = m.region
        self.base = m.base

    def u(self, fmt: str, off: int):
        if off < 0:
            raise FxError(f"negative offset {off}")
        try:
            return struct.unpack_from(fmt, self.data, off)
        except struct.error:
            raise FxError(f"read of {fmt} at {off:#x} outside the tag region") from None

    def reflexive(self, off: int, stride: int, limit: int) -> tuple[int, int]:
        count, addr = self.u("<II", off)
        if count == 0:
            return 0, 0
        o = addr - self.base
        if count > limit or o < 0 or o + count * stride > len(self.data):
            raise FxError(f"reflexive at {off:#x}: {count} x {stride} at {addr:#x} outside the tag region or too large")
        return count, o

    def tagref(self, off: int) -> tuple[str | None, int]:
        """(group, tag id) of the reference at ``off``; (None, NULL_ID) when null. The id is not checked."""
        raw = self.u("<4s", off)[0]
        tid = self.u("<I", off + 12)[0]
        if tid == NULL_ID:
            return None, NULL_ID
        return raw[::-1].decode("latin1"), tid

    def resolve(self, off: int, groups: tuple[str, ...] | None, what: str, problems: list[str]) -> int:
        """Tag id of the reference at ``off`` when it is non-null, resolves to a tag and (if ``groups``) has one of
        those groups; NULL_ID otherwise (a dangling or mistyped reference is noted in ``problems``)."""
        g, tid = self.tagref(off)
        if tid == NULL_ID:
            return NULL_ID
        e = self.m.by_id.get(tid)
        if e is None or (groups is not None and e.group not in groups) or g != e.group:
            problems.append(f"{what}: reference {g!r} {tid:#x} does not resolve to a {groups} tag")
            return NULL_ID
        return tid

    def block(self, e: tagindex.TagEntry) -> int:
        return e.data_address - self.base

    def cstr(self, off: int, n: int) -> str:
        if off < 0 or off + n > len(self.data):
            raise FxError(f"string at {off:#x} outside the tag region")
        return self.data[off:off + n].split(b"\0", 1)[0].decode("latin1")


def _fin(v: float, what: str, problems: list[str]) -> float:
    if not math.isfinite(v):
        problems.append(f"{what}: non-finite float replaced by 0")
        return 0.0
    return float(v)


def _bounds(r: Region, off: int, what: str, problems: list[str], stats: Counter) -> tuple[float, float]:
    a, b = r.u("<2f", off)
    a, b = _fin(a, what, problems), _fin(b, what, problems)
    if a > b:
        stats["swapped bounds"] += 1
        a, b = b, a
    return a, b


def _f(r: Region, off: int, what: str, problems: list[str]) -> float:
    return _fin(r.u("<f", off)[0], what, problems)


def _fvec(r: Region, off: int, n: int, what: str, problems: list[str]) -> tuple[float, ...]:
    return tuple(_fin(v, what, problems) for v in r.u(f"<{n}f", off))


# ---- which effects ---------------------------------------------------------------------------------------------------
class TagMemory:
    """Per-tag memory extents: a tag's root struct and nested blocks lie between its data address and the next tag's."""

    def __init__(self, m: texconv.MapData):
        self.m = m
        ents = sorted((e for e in m.index.entries if e.data_address), key=lambda e: e.data_address)
        self.ents = ents
        self.addrs = [e.data_address for e in ents]
        self.end = m.base + len(m.region)

    def extent(self, e: tagindex.TagEntry) -> tuple[int, int]:
        from bisect import bisect_right
        i = bisect_right(self.addrs, e.data_address)
        end = self.addrs[i] if i < len(self.addrs) else self.end
        return e.data_address - self.m.base, min(end, self.end) - self.m.base

    def refs(self, e: tagindex.TagEntry, groups: set[str]) -> list[tuple[int, str, int]]:
        """(offset in the tag, group, tag id) of every 16-byte tag reference in the tag's memory whose group is in
        ``groups`` and whose id resolves to a tag of that group (4-byte aligned)."""
        s, t = self.extent(e)
        data = self.m.region
        out = []
        by_id = self.m.by_id
        for o in range(s, t - 15, 4):
            tid = struct.unpack_from("<I", data, o + 12)[0]
            x = by_id.get(tid)
            if x is None or x.group not in groups or data[o:o + 4] != x.group.encode("ascii")[::-1]:
                continue
            out.append((o - s, x.group, tid))
        return out


# object attachments (FXAT): the object tag's attachments block (+0x140 reflexive, 72 B each: +0x00 tag reference (group, path ptr, len, id), +0x10 char[32]
# marker name, then the primary / secondary scale functions) [verified on a10 a30 b30: every reference resolves to a tag of its group on the 258 attachments;
# groups ligh 144, lsnd 77, cont 28, effe 24, pctl 4, mgs2 1]. Kept: particle systems and effects (what the engine can draw).
ATT_PCTL, ATT_EFFE, ATT_LIGH = 1, 2, 3
GLOW_BITMAP = "effects\\decals\\bullet holes\\bitmaps\\plasma burn outer glow"      # the soft disc the engine draws light attachments with
OBJECT_GROUPS = ("scen", "vehi", "bipd", "mach", "ctrl", "lifi", "eqip", "weap", "garb", "proj", "ssce", "plac")
ATTACH_OFF, ATTACH_SIZE = 0x140, 72
STATIC_LIGHT_GROUPS = ("scen", "mach", "ctrl", "lifi", "ssce", "plac")


def read_attachments(m: texconv.MapData) -> list[tuple[int, int, int, str]]:
    out = []
    for e in m.index.entries:
        if e.group not in OBJECT_GROUPS or not e.data_address:
            continue
        b = m.tag_off(e)
        if b < 0 or b + ATTACH_OFF + 8 > len(m.region):
            continue
        n, a = struct.unpack_from("<II", m.region, b + ATTACH_OFF)
        if not 0 < n < 16 or not 0 <= a - m.base < len(m.region) - n * ATTACH_SIZE:
            continue
        for k in range(n):
            raw = m.region[a - m.base + k * ATTACH_SIZE:a - m.base + (k + 1) * ATTACH_SIZE]
            grp, tid = raw[:4][::-1].decode("latin1"), struct.unpack_from("<I", raw, 12)[0]
            t = m.by_id.get(tid)
            if t is None or t.group != grp or grp not in ("pctl", "effe", "ligh"):
                continue
            if grp == "ligh" and e.group not in STATIC_LIGHT_GROUPS:
                continue                         # weapon / projectile / vehicle / unit lights are driven by functions (firing, headlights switch): not always on
            out.append((e.tag_id, {"pctl": ATT_PCTL, "effe": ATT_EFFE, "ligh": ATT_LIGH}[grp], tid, raw[0x10:0x30].split(b"\0")[0].decode("latin1")))
    return sorted(out)


def effect_closure(m: texconv.MapData, all_effects: bool = False) -> tuple[list[int], list[int]]:
    """(effect tag ids, object tag ids created by their parts), both sorted. See the module docstring."""
    mem = TagMemory(m)
    r = Region(m)
    seen: set[int] = set()
    queue: deque[int] = deque()

    def add(tid: int) -> None:
        if tid not in seen:
            seen.add(tid)
            queue.append(tid)

    for e in m.index.entries:
        if e.group in ROOT_GROUPS and e.data_address:
            for _o, _g, tid in mem.refs(e, {"effe"}):
                add(tid)
    if all_effects:
        for e in m.index.entries:
            if e.group == "effe":
                add(e.tag_id)
    for _o, kind, tid, _mk in read_attachments(m):
        if kind == ATT_EFFE:
            add(tid)
    objects: set[int] = set()
    seen_tags: set[int] = set()
    while queue:
        e = m.by_id[queue.popleft()]
        try:
            created = _created_tags(r, e)
        except FxError:
            continue                    # the full parse reports it
        for tid in created:
            x = m.by_id.get(tid)
            if x is None or tid in seen_tags:
                continue
            seen_tags.add(tid)
            if x.group == "part" or x.group not in ("deca", "ligh", "pctl", "jpt!", "snd!"):
                if x.group != "part":
                    objects.add(tid)
                for _o, _g, t2 in mem.refs(x, {"effe"}):
                    add(t2)
    return sorted(seen), sorted(objects)


def _created_tags(r: Region, e: tagindex.TagEntry) -> list[int]:
    """Tag ids an effect's parts and particles reference (no validation of the ids)."""
    b = r.block(e)
    out = []
    nev, eo = r.reflexive(b + 0x34, EVENT_SIZE, MAX_EVENTS)
    for i in range(nev):
        ev = eo + i * EVENT_SIZE
        n, po = r.reflexive(ev + 0x2C, PART_SIZE, MAX_PARTS)
        out += [r.tagref(po + k * PART_SIZE + 0x18)[1] for k in range(n)]
        n, po = r.reflexive(ev + 0x38, PCL_SIZE, MAX_PARTS)
        out += [r.tagref(po + k * PCL_SIZE + 0x54)[1] for k in range(n)]
    return [t for t in out if t != NULL_ID]


# ---- parsing the tags --------------------------------------------------------------------------------------------------
def _norm_location(v: int, nloc: int, what: str, problems: list[str]) -> int:
    if v >= nloc:
        problems.append(f"{what}: location {v} but the effect has {nloc} locations")
        return NONE16
    return v


def parse_effect(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> Effect:
    b = r.block(e)
    what = e.path
    flags, ls, lp = r.u("<Ihh", b)
    radius = _f(r, b + 8, what, problems)
    nloc, lo = r.reflexive(b + 0x28, LOC_SIZE, MAX_LOCS)
    locs = [r.cstr(lo + i * LOC_SIZE, LOC_SIZE) for i in range(nloc)]
    nev, eo = r.reflexive(b + 0x34, EVENT_SIZE, MAX_EVENTS)
    events: list[Event] = []
    for i in range(nev):
        ev = eo + i * EVENT_SIZE
        skip = _f(r, ev + 4, what, problems)
        if not 0.0 <= skip <= 1.0:
            problems.append(f"{what}: event {i} skip fraction {skip}")
            skip = min(1.0, max(0.0, skip))
        delay = _bounds(r, ev + 8, what, problems, stats)
        duration = _bounds(r, ev + 0x10, what, problems, stats)
        parts: list[Part] = []
        npart, po = r.reflexive(ev + 0x2C, PART_SIZE, MAX_PARTS)
        for k in range(npart):
            p = po + k * PART_SIZE
            cin, viol, loc, pflags = r.u("<4H", p)
            cls = r.u("<4s", p + 0x14)[0][::-1].decode("latin1")
            g, tid = r.tagref(p + 0x18)
            m_entry = r.m.by_id.get(tid)
            if tid == NULL_ID:
                stats["parts without a reference (dropped)"] += 1
                continue
            if m_entry is None or m_entry.group != g:
                problems.append(f"{what}: part {i}/{k} references {g!r} {tid:#x}: not a tag of that group")
                stats["parts with a dangling reference (dropped)"] += 1
                continue
            kind = KIND_OF_GROUP.get(g, K_OBJECT)
            if cls != g and kind != K_OBJECT:
                stats["parts whose class differs from the reference group (not objects)"] += 1
            if cin > 3 or viol > 2:
                raise FxError(f"{what}: part {i}/{k}: create in {cin} / violence {viol} out of range")
            parts.append(Part(cin, viol, _norm_location(loc, nloc, what, problems) if loc != NONE16 else NONE16,
                              pflags, kind, tid, g,
                              _bounds(r, p + 0x40, what, problems, stats), _f(r, p + 0x48, what, problems),
                              _bounds(r, p + 0x4C, what, problems, stats), _bounds(r, p + 0x54, what, problems, stats),
                              *r.u("<II", p + 0x60)))
        emitters: list[Emitter] = []
        ncl, co = r.reflexive(ev + 0x38, PCL_SIZE, MAX_PARTS)
        for k in range(ncl):
            c = co + k * PCL_SIZE
            cin, viol, create, _p6, loc = r.u("<5H", c)
            g, tid = r.tagref(c + 0x54)
            x = r.m.by_id.get(tid)
            if tid == NULL_ID:
                stats["particles without a reference (dropped)"] += 1
                continue
            if x is None or x.group != "part" or g != "part":
                problems.append(f"{what}: particle {i}/{k} references {g!r} {tid:#x}: not a particle tag")
                stats["particles with a dangling reference (dropped)"] += 1
                continue
            if cin > 3 or viol > 2 or create > 3:
                raise FxError(f"{what}: particle {i}/{k}: enum out of range")
            pflags = r.u("<I", c + 0x64)[0]
            dist = r.u("<H", c + 0x68)[0]
            if dist > 5:
                raise FxError(f"{what}: particle {i}/{k}: distribution function {dist}")
            cnt = r.u("<2h", c + 0x6C)
            cnt = (min(cnt), max(cnt))
            if cnt[0] < 0:
                raise FxError(f"{what}: particle {i}/{k}: negative count")
            yaw, pitch = _fvec(r, c + 0x0C, 2, what, problems)
            offs = _fvec(r, c + 0x14, 3, what, problems)
            tint = [_fvec(r, c + o, 4, what, problems) for o in (0xB0, 0xC0)]     # A R G B

            def rgba8(t):
                return tuple(max(0, min(255, int(round(v * 255.0)))) for v in (t[1], t[2], t[3], t[0]))
            emitters.append(Emitter(cin, viol, create, _norm_location(loc, nloc, what, problems) if loc != NONE16 else NONE16,
                                    tid, yaw, pitch, offs, pflags, dist, cnt,
                                    _bounds(r, c + 0x70, what, problems, stats), _bounds(r, c + 0x84, what, problems, stats),
                                    _f(r, c + 0x8C, what, problems), _bounds(r, c + 0x90, what, problems, stats),
                                    _bounds(r, c + 0xA0, what, problems, stats), rgba8(tint[0]), rgba8(tint[1]),
                                    *r.u("<II", c + 0xE0)))
        events.append(Event(skip, delay, duration, parts, emitters))
    return Effect(e.tag_id, e.path, flags, ls, lp, radius, locs, events)


def _angle_ok(v: int, lo: int, hi: int, what: str) -> int:
    if not lo <= v <= hi:
        raise FxError(f"{what}: value {v} outside {lo}..{hi}")
    return v


def parse_particle(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> ParticleDef:
    b = r.block(e)
    what = e.path
    if b < 0 or b + PART_TAG_SIZE > len(r.data):
        raise FxError(f"{what}: particle block outside the tag region")
    flags = r.u("<I", b)[0]
    bmp = r.resolve(b + 0x04, ("bitm",), what + " bitmap", problems)
    phys = r.resolve(b + 0x14, ("pphy",), what + " physics", problems)
    coll = r.resolve(b + 0x48, ("effe", "jpt!", "snd!"), what + " collision effect", problems)
    death = r.resolve(b + 0x58, ("effe", "jpt!", "snd!"), what + " death effect", problems)
    life = _bounds(r, b + 0x38, what, problems, stats)
    fi, fo, ms = (_f(r, b + o, what, problems) for o in (0x40, 0x44, 0x68))
    ra = _bounds(r, b + 0x74, what, problems, stats)
    ar = _bounds(r, b + 0x80, what, problems, stats)
    cd, fss, fes = (_f(r, b + o, what, problems) for o in (0x88, 0x8C, 0x90))
    s_first, s_init, s_loop, s_final = r.u("<4h", b + 0x98)
    sprite = _f(r, b + 0xA8, what, problems)
    orient = _angle_ok(r.u("<H", b + 0xAC)[0], 0, 2, what + " orientation")
    sflags, blend, fade = r.u("<3H", b + 0xD8)
    _angle_ok(blend, 0, 7, what + " blend function")
    _angle_ok(fade, 0, 2, what + " fade mode")
    _angle_ok(sflags, 0, 7, what + " shader flags")
    if min(s_init, s_loop, s_final) < 0 or s_first < 0:
        raise FxError(f"{what}: negative sequence index or count")
    kind = lambda t: K_NONE if t == NULL_ID else KIND_OF_GROUP[r.m.by_id[t].group]  # noqa: E731
    return ParticleDef(e.tag_id, e.path, flags, bmp, phys, coll, death, kind(coll), kind(death), life, fi, fo, ms, ra, ar, cd, fss, fes,
                       s_first, s_init, s_loop, s_final, sprite, orient, blend, fade, sflags)


def parse_physics(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> PhysicsDef:
    b = r.block(e)
    what = e.path
    if b < 0 or b + PHYS_SIZE > len(r.data):
        raise FxError(f"{what}: point physics block outside the tag region")
    flags = r.u("<I", b)[0]
    wg, ag = (_f(r, b + o, what, problems) for o in (8, 0x0C))
    den, af, wf, sf, el = (_f(r, b + o, what, problems) for o in (0x20, 0x24, 0x28, 0x2C, 0x30))
    return PhysicsDef(e.tag_id, e.path, flags, den, af, wf, sf, el, ag, wg)


def parse_decal(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> DecalDef:
    b = r.block(e)
    what = e.path
    if b < 0 or b + DECAL_SIZE > len(r.data):
        raise FxError(f"{what}: decal block outside the tag region")
    flags, typ, layer = r.u("<3H", b)
    _angle_ok(typ, 0, 3, what + " type")
    _angle_ok(layer, 0, 4, what + " layer")
    nxt = r.resolve(b + 0x08, ("deca",), what + " next decal", problems)
    bmp = r.resolve(b + 0xD8, ("bitm",), what + " map", problems)
    blend = _angle_ok(r.u("<H", b + 0xC0)[0], 0, 7, what + " blend function")
    loop, speed = r.u("<2h", b + 0x58)
    return DecalDef(e.tag_id, e.path, flags, typ, layer, nxt, bmp, blend,
                    _bounds(r, b + 0x18, what, problems, stats), _bounds(r, b + 0x2C, what, problems, stats),
                    _fvec(r, b + 0x34, 3, what, problems), _fvec(r, b + 0x40, 3, what, problems),  # type: ignore[arg-type]
                    _bounds(r, b + 0x78, what, problems, stats), _bounds(r, b + 0x80, what, problems, stats),
                    loop, speed, _f(r, b + 0xFC, what, problems))


def parse_light(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> LightDef:
    b = r.block(e)
    what = e.path
    if b < 0 or b + LIGHT_SIZE > len(r.data):
        raise FxError(f"{what}: light block outside the tag region")
    flags = r.u("<I", b)[0]
    return LightDef(e.tag_id, e.path, flags, _f(r, b + 4, what, problems), _bounds(r, b + 8, what, problems, stats),
                    _f(r, b + 0x10, what, problems), _f(r, b + 0x14, what, problems), r.u("<I", b + 0x30)[0],
                    _fvec(r, b + 0x38, 4, what, problems), _fvec(r, b + 0x48, 4, what, problems),  # type: ignore[arg-type]
                    _f(r, b + 0xF4, what, problems), _angle_ok(r.u("<H", b + 0xFA)[0], 0, 15, what + " falloff function"))


def _consts(r: Region, off: int, what: str, problems: list[str]) -> list[float]:
    n, o = r.reflexive(off, PCTL_CONST, MAX_CONSTS)
    return [_fin(v, what, problems) for v in r.u(f"<{n}f", o)] if n else []


def parse_pctl(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> PctlDef:
    b = r.block(e)
    what = e.path
    if b < 0 or b + PCTL_ROOT > len(r.data):
        raise FxError(f"{what}: particle system block outside the tag region")
    phys = r.resolve(b + 0x38, ("pphy",), what + " physics", problems)
    upd = _angle_ok(r.u("<H", b + 0x48)[0], 0, 15, what + " system update physics")
    nt, to = r.reflexive(b + 0x5C, PCTL_TYPE, 4)
    types = []
    for i in range(nt):
        t = to + i * PCTL_TYPE
        tw = f"{what} type {i}"
        flags = r.u("<I", t + 0x20)[0]
        init, = r.u("<H", t + 0x24)
        render = _angle_ok(r.u("<H", t + 0x28)[0], 0, 1, tw + " render mode")
        radius = _f(r, t + 0x2C, tw, problems)
        cphys = _angle_ok(r.u("<H", t + 0x54)[0], 0, 2, tw + " creation physics")
        ns, so = r.reflexive(t + 0x68, PCTL_STATE, 8)
        states = []
        for k in range(ns):
            s = so + k * PCTL_STATE
            states.append(PctlState(r.cstr(s, NAME_LEN), _bounds(r, s + 0x20, tw, problems, stats),
                                    _bounds(r, s + 0x28, tw, problems, stats), *_fvec(r, s + 0x34, 3, tw, problems),
                                    _fvec(r, s + 0x40, 4, tw, problems),  # type: ignore[arg-type]
                                    *_fvec(r, s + 0x50, 3, tw, problems),
                                    _angle_ok(r.u("<H", s + 0xB0)[0], 0, 15, tw + " state creation physics"),
                                    _angle_ok(r.u("<H", s + 0xB2)[0], 0, 15, tw + " state update physics"),
                                    _consts(r, s + 0xB4, tw, problems)))
        np_, po = r.reflexive(t + 0x74, PCTL_PSTATE, 8)
        pstates = []
        for k in range(np_):
            s = po + k * PCTL_PSTATE
            sflags, blend, fade = r.u("<3H", s + 0xE0)
            _angle_ok(blend, 0, 7, tw + " pstate blend")
            _angle_ok(fade, 0, 2, tw + " pstate fade")
            pstates.append(PctlPState(r.cstr(s, NAME_LEN), _bounds(r, s + 0x20, tw, problems, stats),
                                      _bounds(r, s + 0x28, tw, problems, stats),
                                      r.resolve(s + 0x30, ("bitm",), tw + " bitmap", problems),
                                      r.u("<H", s + 0x40)[0], _bounds(r, s + 0x48, tw, problems, stats),
                                      _bounds(r, s + 0x50, tw, problems, stats), _bounds(r, s + 0x58, tw, problems, stats),
                                      _fvec(r, s + 0x60, 4, tw, problems), _fvec(r, s + 0x70, 4, tw, problems),  # type: ignore[arg-type]
                                      _f(r, s + 0x80, tw, problems),
                                      r.resolve(s + 0x84, ("pphy",), tw + " physics", problems), blend, fade,
                                      _angle_ok(sflags, 0, 7, tw + " pstate shader flags"),
                                      _consts(r, s + 0x16C, tw, problems)))
        types.append(PctlType(r.cstr(t, NAME_LEN), flags, init, render, radius, cphys,
                              _consts(r, t + 0x5C, tw, problems), states, pstates))
    return PctlDef(e.tag_id, e.path, phys, upd, types)


def parse_bitmap(r: Region, e: tagindex.TagEntry, problems: list[str], stats: Counter) -> BitmapDef:
    """Sequences and sprites of a bitmap tag; for 2D bitmaps (no sprites) every sequence lists its whole images."""
    b = r.block(e)
    what = e.path
    images = bitmaps.parse_images(r.data, r.base, e.data_address)
    kind = r.u("<H", b)[0]
    ns, so = r.reflexive(b + 0x54, BITMAP_SEQ, 256)
    seqs: list[list[Sprite]] = []
    for i in range(ns):
        s = so + i * BITMAP_SEQ
        first, cnt = r.u("<2H", s + 0x20)
        nsp, spo = r.reflexive(s + 0x34, BITMAP_SPRITE, 4096)
        sprites: list[Sprite] = []
        for k in range(nsp):
            q = spo + k * BITMAP_SPRITE
            idx = r.u("<H", q)[0]
            left, right, top, bottom = r.u("<4f", q + 8)
            rx, ry = r.u("<2f", q + 0x18)
            if idx >= len(images) or not all(map(math.isfinite, (left, right, top, bottom, rx, ry))):
                raise FxError(f"{what}: sequence {i} sprite {k}: bitmap {idx} of {len(images)} or a non-finite value")
            c = [min(1.0, max(0.0, v)) for v in (left, right, top, bottom)]       # a few retail rects overhang by 1/16
            if c != [left, right, top, bottom]:
                stats["sprite rectangles clamped to the image"] += 1
            if not (c[0] < c[1] and c[2] < c[3]):
                raise FxError(f"{what}: sequence {i} sprite {k}: empty rectangle ({left}, {right}, {top}, {bottom})")
            sprites.append(Sprite(idx, c[0], c[1], c[2], c[3], (rx, ry)))
        if not sprites:
            if first + cnt > len(images) or cnt == 0:
                if cnt == 0:
                    problems.append(f"{what}: sequence {i} has neither sprites nor images")
                    seqs.append([])
                    continue
                raise FxError(f"{what}: sequence {i} images {first}+{cnt} of {len(images)}")
            sprites = [Sprite(first + j, 0.0, 1.0, 0.0, 1.0, (0.0, 0.0)) for j in range(cnt)]
        seqs.append(sprites)
    return BitmapDef(e.tag_id, e.path, kind, len(images), seqs)


# ---- collecting ------------------------------------------------------------------------------------------------------
def collect(m: texconv.MapData, all_effects: bool = False) -> FxSet:
    r = Region(m)
    problems: list[str] = []
    stats: Counter = Counter()
    effect_ids, object_ids = effect_closure(m, all_effects)
    effects: list[Effect] = []
    for tid in effect_ids:
        effects.append(parse_effect(r, m.by_id[tid], problems, stats))

    particles: dict[int, ParticleDef] = {}
    decals: dict[int, DecalDef] = {}
    lights: dict[int, LightDef] = {}
    pctls: dict[int, PctlDef] = {}
    phys: dict[int, PhysicsDef] = {}
    bmps: dict[int, BitmapDef] = {}

    def want_physics(tid: int) -> None:
        if tid != NULL_ID and tid not in phys:
            phys[tid] = parse_physics(r, m.by_id[tid], problems, stats)

    def want_bitmap(tid: int) -> None:
        if tid != NULL_ID and tid not in bmps:
            bmps[tid] = parse_bitmap(r, m.by_id[tid], problems, stats)

    for ef in effects:
        for ev in ef.events:
            for p in ev.parts:
                if p.kind == K_DECAL and p.tag_id not in decals:
                    d = parse_decal(r, m.by_id[p.tag_id], problems, stats)
                    decals[p.tag_id] = d
                    want_bitmap(d.bitmap_tag)
                    nxt = d.next_tag
                    while nxt != NULL_ID and nxt not in decals:          # decal chains
                        dd = parse_decal(r, m.by_id[nxt], problems, stats)
                        decals[nxt] = dd
                        want_bitmap(dd.bitmap_tag)
                        nxt = dd.next_tag
                elif p.kind == K_LIGHT and p.tag_id not in lights:
                    lights[p.tag_id] = parse_light(r, m.by_id[p.tag_id], problems, stats)
                elif p.kind == K_PCTL and p.tag_id not in pctls:
                    pc = parse_pctl(r, m.by_id[p.tag_id], problems, stats)
                    pctls[p.tag_id] = pc
                    want_physics(pc.physics_tag)
                    for t in pc.types:
                        for ps in t.pstates:
                            want_bitmap(ps.bitmap_tag)
                            want_physics(ps.physics_tag)
            for c in ev.emitters:
                if c.particle_tag not in particles:
                    pd = parse_particle(r, m.by_id[c.particle_tag], problems, stats)
                    particles[c.particle_tag] = pd
                    want_bitmap(pd.bitmap_tag)
                    want_physics(pd.physics_tag)
    attach = read_attachments(m)
    for _o, kind, tid, _mk in attach:
        if kind == ATT_LIGH:
            if tid not in lights:
                lights[tid] = parse_light(r, m.by_id[tid], problems, stats)
            for e in m.index.entries:
                if e.group == "bitm" and e.path == GLOW_BITMAP:
                    want_bitmap(e.tag_id)
        if kind == ATT_PCTL and tid not in pctls:
            pc = parse_pctl(r, m.by_id[tid], problems, stats)
            pctls[tid] = pc
            want_physics(pc.physics_tag)
            for t in pc.types:
                for ps in t.pstates:
                    want_bitmap(ps.bitmap_tag)
                    want_physics(ps.physics_tag)
    # the effect references of particles must resolve inside the output (or be dropped)
    out_effects = {e.tag_id for e in effects}
    for pd in particles.values():
        if pd.collision_kind == K_EFFECT and pd.collision_effect not in out_effects:
            pd.collision_effect, pd.collision_kind = NULL_ID, K_NONE
            stats["particle collision effects dropped (not converted)"] += 1
        if pd.death_kind == K_EFFECT and pd.death_effect not in out_effects:
            pd.death_effect, pd.death_kind = NULL_ID, K_NONE
            stats["particle death effects dropped (not converted)"] += 1
    return FxSet(effects, sorted(particles.values(), key=lambda x: x.tag_id), sorted(phys.values(), key=lambda x: x.tag_id),
                 sorted(decals.values(), key=lambda x: x.tag_id), sorted(lights.values(), key=lambda x: x.tag_id),
                 sorted(pctls.values(), key=lambda x: x.tag_id), sorted(bmps.values(), key=lambda x: x.tag_id),
                 object_ids, problems, stats, attach)


def object_models(m: texconv.MapData, objects: list[int]) -> set[int]:
    """Model tags (group ``mode``) of the object tags ``objects`` (the +0x28 reference every object tag carries)."""
    r = Region(m)
    out: set[int] = set()
    for tid in objects:
        try:
            g, mid = r.tagref(r.block(m.by_id[tid]) + 0x28)
        except FxError:
            continue
        if g == "mode" and mid in m.by_id and m.by_id[mid].group == "mode":
            out.add(mid)
    return out


def referenced_models(map_path: Path | str, all_effects: bool = True) -> set[int]:
    """Tag ids of the ``model`` tags of objects that effects create (debris, projectiles ...), keyed as modelconv keys
    models. For the roots of objpack.py. Defaults to the effects of the whole map: the gameplay set (``all_effects=False``)
    creates no object at all on the campaign maps, the debris comes from device and scenery effects."""
    m = texconv.MapData.open(Path(map_path))
    return object_models(m, effect_closure(m, all_effects)[1])


# ---- textures -----------------------------------------------------------------------------------------------------------
def fx_target(w: int, h: int, cap: int) -> tuple[int, int]:
    """Sheets up to 64 px keep their size, larger ones are halved once; then both sides are clamped to ``cap`` by halving
    (same shape), min 8."""
    if max(w, h) > FX_FULL_RES_SIDE:
        w, h = w // 2, h // 2
    while w > cap or h > cap:
        w, h = max(texconv.MIN_TEX, w // 2), max(texconv.MIN_TEX, h // 2)
    return max(texconv.MIN_TEX, w), max(texconv.MIN_TEX, h)


def estimate_bytes(sizes: list[tuple[int, int, bool]], cap: int) -> int:
    """Upper bound of pixels + CLUTs: PSMT8, one mip for sprite sheets, the 4-level chain for 2D images."""
    total = 0
    for w, h, sheet in sizes:
        tw, th = fx_target(w, h, cap)
        dims = texconv.mip_dims(tw, th)[:1 if sheet else texconv.MAX_MIPS]
        total += sum((lw * lh + 15) // 16 * 16 for lw, lh in dims) + 1024
    return total


@dataclass
class FxTextures:
    blob: bytes
    textures: list[texconv.Ps2Texture]
    tex_index: dict[tuple[int, int], int]            # (bitmap tag, image index) -> texture index
    cap: int
    estimate_cap: int
    bytes: int
    skipped: Counter = field(default_factory=Counter)


def build_textures(m: texconv.MapData, fx: FxSet, budget: int = FX_DEFAULT_BUDGET) -> FxTextures:
    """Convert every image that a sprite of a used bitmap points to. Bindings are keyed by bitmap tag id (the texture of
    its first image that is used)."""
    used: dict[tuple[int, int], tuple[bitmaps.BitmapImage, bool, int]] = {}      # key -> (image, sheet, first blend)
    skipped: Counter = Counter()
    blend_of: dict[int, int] = {}
    for pd in fx.particles:
        if pd.bitmap_tag != NULL_ID:
            blend_of.setdefault(pd.bitmap_tag, pd.blend)
    for d in fx.decals:
        if d.bitmap_tag != NULL_ID:
            blend_of.setdefault(d.bitmap_tag, d.blend)
    for pc in fx.pctl:
        for t in pc.types:
            for ps in t.pstates:
                if ps.bitmap_tag != NULL_ID:
                    blend_of.setdefault(ps.bitmap_tag, ps.blend)
    for bd in fx.bitmaps:
        images = bitmaps.parse_images(m.region, m.base, m.by_id[bd.tag_id].data_address)
        for seq in bd.sequences:
            for sp in seq:
                key = (bd.tag_id, sp.image)
                img = images[sp.image]
                if key in used:
                    continue
                if not img.decodable or img.width & (img.width - 1) or img.height & (img.height - 1):
                    skipped["image is not a decodable power-of-two 2D image"] += 1
                    used[key] = (img, bd.kind == 3, -1)
                    continue
                used[key] = (img, bd.kind == 3, blend_of.get(bd.tag_id, 0))
    ok = {k: v for k, v in used.items() if v[2] >= 0}
    sizes = [(img.width, img.height, sheet) for img, sheet, _b in ok.values()]
    cap = FX_MAX_DIM
    while cap > 32 and estimate_bytes(sizes, cap) > budget:
        cap //= 2
    textures: list[texconv.Ps2Texture] = []
    tex_index: dict[tuple[int, int], int] = {}
    for key in sorted(ok):
        img, sheet, _blend = ok[key]
        tw, th = fx_target(img.width, img.height, cap)
        level = 1 if img.mip_count >= 1 and img.width >> 1 >= tw and img.height >> 1 >= th else 0
        try:
            w, h, rgba = bitmaps.decode_level(img, m.pixels(img), level)
        except bitmaps.BitmapError as exc:
            skipped[f"image failed to decode: {exc}"] += 1
            continue
        found = texconv.alpha_class(rgba)
        rgba = texconv.resample_rgba(rgba, w, h, tw, th, found != texconv.CLASS_OPAQUE)
        tex = texconv.convert_rgba(key[0], rgba, tw, th, found)
        if sheet:
            tex.mips = tex.mips[:1]
        if img.width < texconv.MIN_TEX or img.height < texconv.MIN_TEX:
            tex.flags |= texconv.TF_UPSCALED
        tex_index[key] = len(textures)
        textures.append(tex)
    # one binding per bitmap tag: its lowest-numbered converted image
    shaders: list[texconv.ShaderInfo] = []
    for bd in fx.bitmaps:
        keys = sorted(k for k in tex_index if k[0] == bd.tag_id)
        if not keys:
            continue
        s = texconv.ShaderInfo(bd.tag_id, None, bd.path)
        s.texture = tex_index[keys[0]]
        s.blend = FRAMEBUFFER_TO_TEX_BLEND.get(blend_of.get(bd.tag_id, 0), texconv.BLEND_ALPHA)
        if textures[s.texture].flags & texconv.TF_PUNCH:
            s.flags |= texconv.BF_ALPHA_TEST
        shaders.append(s)
    blob = texconv.build_texs(textures, shaders)
    return FxTextures(blob, textures, tex_index, cap, FX_MAX_DIM, sum(t.total_bytes for t in textures), skipped)


# ---- section writer ------------------------------------------------------------------------------------------------------
def _name(s: str, n: int) -> bytes:
    return s.encode("latin1", "replace")[:n - 1].ljust(n, b"\0")


def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def _idx(table: dict[int, int], tag: int, what: str) -> int:
    if tag == NULL_ID:
        return NONE16
    try:
        return table[tag]
    except KeyError:
        raise FxError(f"{what}: tag {tag:#x} is not in the converted set") from None


def build(fx: FxSet, tex_index: dict[tuple[int, int], int] | None = None) -> bytes:
    """Serialise the FXD0 section. ``tex_index`` maps (bitmap tag, image) to FXTX texture indices (sprites of images that
    were not converted get texture 0xFFFF)."""
    tex_index = tex_index or {}
    if any(a.tag_id >= b.tag_id for t in (fx.effects, fx.particles, fx.physics, fx.decals, fx.lights, fx.pctl, fx.bitmaps)
           for a, b in zip(t, t[1:])):
        raise FxError("definitions must be sorted by strictly ascending tag id")
    pidx = {p.tag_id: i for i, p in enumerate(fx.particles)}
    phidx = {p.tag_id: i for i, p in enumerate(fx.physics)}
    didx = {p.tag_id: i for i, p in enumerate(fx.decals)}
    lidx = {p.tag_id: i for i, p in enumerate(fx.lights)}
    cidx = {p.tag_id: i for i, p in enumerate(fx.pctl)}
    bidx = {p.tag_id: i for i, p in enumerate(fx.bitmaps)}
    rec: dict[str, bytearray] = {n: bytearray() for n in TABLES}
    counts: dict[str, int] = {n: 0 for n in TABLES}

    def put(table: str, *vals) -> int:
        rec[table] += struct.pack(RECORDS[table], *vals)
        counts[table] += 1
        return counts[table] - 1

    # bitmaps first (sprites need sequential ranges)
    for bd in fx.bitmaps:
        first_seq = counts["sequences"]
        for seq in bd.sequences:
            first_sprite = counts["sprites"]
            for sp in seq:
                tex = tex_index.get((bd.tag_id, sp.image), NONE16) if tex_index else NONE16
                put("sprites", tex, sp.image, sp.left, sp.top, sp.right, sp.bottom, sp.reg[0], sp.reg[1])
            put("sequences", first_sprite, len(seq), 0)
        first_tex = min((v for (t, _i), v in tex_index.items() if t == bd.tag_id), default=NONE16)
        put("bitmaps", bd.tag_id, first_seq, len(bd.sequences), bd.kind, 0, first_tex, bd.images)
    for p in fx.physics:
        put("physics", p.tag_id, p.flags, p.density, p.air_friction, p.water_friction, p.surface_friction, p.elasticity,
            p.air_gravity, p.water_gravity)
    for p in fx.particles:
        put("particles", p.tag_id, p.flags, _idx(bidx, p.bitmap_tag, p.path), _idx(phidx, p.physics_tag, p.path),
            p.collision_effect, p.death_effect, *p.lifespan, p.fade_in, p.fade_out, p.min_size, *p.radius_anim,
            *p.anim_rate, p.contact_deterioration, p.fade_start_size, p.fade_end_size, p.seq_first, p.seq_initial,
            p.seq_loop, p.seq_final, p.sprite_size, p.orientation, p.blend, p.fade_mode, p.shader_flags,
            p.collision_kind, p.death_kind)
    for d in fx.decals:
        put("decals", d.tag_id, d.flags, d.type, d.layer, d.next_tag, _idx(bidx, d.bitmap_tag, d.path), d.blend,
            *d.radius, *d.intensity, *d.color_lo, *d.color_hi, *d.lifetime, *d.decay, d.loop_frame, d.anim_speed,
            d.max_extent)
    for L in fx.lights:
        put("lights", L.tag_id, L.flags, L.radius, *L.radius_mod, L.falloff_angle, L.cutoff_angle, L.interp_flags,
            *L.color_lo, *L.color_hi, L.duration, L.falloff_fn)

    def const_range(vals: list[float]) -> tuple[int, int]:
        first = counts["consts"]
        for v in vals:
            put("consts", v)
        return first, len(vals)

    for pc in fx.pctl:
        first_type = counts["pctl_types"]
        for t in pc.types:
            c0, cn = const_range(t.consts)
            s0 = counts["pctl_states"]
            for s in t.states:
                sc0, scn = const_range(s.consts)
                put("pctl_states", _name(s.name, SHORT_NAME), *s.duration, *s.transition, s.scale_mult, s.anim_mult,
                    s.rot_mult, *s.color_mult, s.radius_mult, s.min_count, s.creation_rate, s.creation_physics,
                    s.update_physics, sc0, scn)
            p0 = counts["pctl_pstates"]
            for ps in t.pstates:
                pc0, pcn = const_range(ps.consts)
                put("pctl_pstates", _name(ps.name, SHORT_NAME), *ps.duration, *ps.transition,
                    _idx(bidx, ps.bitmap_tag, pc.path), ps.sequence, *ps.scale, *ps.anim_rate, *ps.rot_rate,
                    *ps.color1, *ps.color2, ps.radius_mult, _idx(phidx, ps.physics_tag, pc.path), ps.blend, ps.fade_mode,
                    ps.shader_flags, pc0, pcn)
            put("pctl_types", _name(t.name, SHORT_NAME), t.flags, t.initial_count, t.render_mode, t.creation_physics,
                t.radius, c0, cn, 0, s0, len(t.states), 0, p0, len(t.pstates), 0)
        put("pctl", pc.tag_id, _idx(phidx, pc.physics_tag, pc.path), pc.update_physics, first_type, len(pc.types))
    for ef in fx.effects:
        first_loc = counts["locations"]
        for n in ef.locations:
            put("locations", _name(n, NAME_LEN))
        first_event = counts["events"]
        for ev in ef.events:
            first_part, first_emit = counts["parts"], counts["emitters"]
            for p in ev.parts:
                index = {K_DECAL: didx, K_LIGHT: lidx, K_PCTL: cidx}.get(p.kind)
                ref_index = index[p.tag_id] if index is not None else NULL_ID
                put("parts", p.create_in, p.violence, p.location, p.flags, p.kind, 0, p.tag_id, p.group.encode("ascii"),
                    ref_index, *p.velocity, p.cone, *p.angvel, *p.radius_mod, p.a_scales, p.b_scales)
            for c in ev.emitters:
                put("emitters", c.create_in, c.violence, c.create, c.location, _idx(pidx, c.particle_tag, ef.path),
                    c.yaw, c.pitch, *c.offset, c.flags, c.distribution, 0, *c.count, *c.dist_radius, *c.velocity, c.cone,
                    *c.angvel, *c.radius, *c.tint_lo, *c.tint_hi, c.a_scales, c.b_scales, c.particle_tag)
            put("events", ev.skip, *ev.delay, *ev.duration, first_part, len(ev.parts), len(ev.emitters), first_emit)
        put("effects", ef.tag_id, ef.flags, ef.loop_start, ef.loop_stop, ef.damage_radius, first_loc,
            len(ef.locations), len(ef.events), first_event)
    out = bytearray(HEADER_SIZE)
    offs: dict[str, int] = {}
    for n in TABLES:
        offs[n] = len(out)
        out += rec[n]
        _pad16(out)
    head = [MAGIC, VERSION, 0, len(out), 0]
    for n in TABLES:
        head += [offs[n], counts[n]]
    struct.pack_into(HEADER_FMT, out, 0, *head)
    return bytes(out)


# ---- reference reader ------------------------------------------------------------------------------------------------------
@dataclass
class Section:
    size: int
    tables: dict[str, list[tuple]]
    raw: bytes

    def names(self, table: str, field_index: int = 0) -> list[str]:
        return [t[field_index].split(b"\0", 1)[0].decode("latin1") for t in self.tables[table]]


def parse_section(data: bytes) -> Section:
    """Reference reader (the C reader is engine/src/world/fxdef.c); validates ranges and cross references and raises
    FxError on a bad section."""
    if len(data) < HEADER_SIZE:
        raise FxError("section shorter than the header")
    vals = struct.unpack_from(HEADER_FMT, data)
    if vals[0] != MAGIC:
        raise FxError("bad signature")
    if vals[1] != VERSION:
        raise FxError(f"unsupported version {vals[1]}")
    if vals[3] != len(data):
        raise FxError("inconsistent header size")
    tables: dict[str, list[tuple]] = {}
    for i, n in enumerate(TABLES):
        off, cnt = vals[5 + 2 * i], vals[6 + 2 * i]
        size = REC_SIZE[n]
        if off % 16 or off < HEADER_SIZE or off + cnt * size > len(data):
            raise FxError(f"table {n} outside the section")
        tables[n] = [struct.unpack_from(RECORDS[n], data, off + k * size) for k in range(cnt)]
    sec = Section(len(data), tables, data)
    _check(sec)
    return sec


def _check(s: Section) -> None:
    """Every range, cross reference, enum and string terminator (the C reader's fxdef_open does the same)."""
    t = s.tables
    n = {k: len(v) for k, v in t.items()}
    for name in ("effects", "particles", "physics", "decals", "lights", "pctl", "bitmaps"):
        ids = [r[0] for r in t[name]]
        if any(a >= b for a, b in zip(ids, ids[1:])):
            raise FxError(f"{name} not strictly ascending by tag id")

    def rng(first: int, count: int, table: str, what: str) -> None:
        if first + count > n[table]:
            raise FxError(f"{what}: range {first}+{count} outside {table} ({n[table]})")

    def idx(v: int, table: str, what: str) -> None:
        if v != NONE16 and v >= n[table]:
            raise FxError(f"{what}: index {v} outside {table} ({n[table]})")

    def finite(rec: tuple, what: str) -> None:
        if not all(math.isfinite(v) for v in rec if isinstance(v, float)):
            raise FxError(f"{what}: non-finite float")

    for tab in ("locations",):
        for r in t[tab]:
            if r[0][-1] != 0:
                raise FxError("unterminated location name")
    for r in t["effects"]:
        rng(r[5], r[6], "locations", "effect locations")
        rng(r[8], r[7], "events", "effect events")
        finite(r, "effect")
        for ev in t["events"][r[8]:r[8] + r[7]]:
            rng(ev[5], ev[6], "parts", "event parts")
            rng(ev[8], ev[7], "emitters", "event emitters")
            for p in t["parts"][ev[5]:ev[5] + ev[6]]:
                if p[2] != NONE16 and p[2] >= r[6]:
                    raise FxError("part location outside the effect's locations")
            for c in t["emitters"][ev[8]:ev[8] + ev[7]]:
                if c[3] != NONE16 and c[3] >= r[6]:
                    raise FxError("emitter location outside the effect's locations")
    for ev in t["events"]:
        finite(ev, "event")
        rng(ev[5], ev[6], "parts", "event parts")
        rng(ev[8], ev[7], "emitters", "event emitters")
    for r in t["parts"]:
        if r[4] > K_EFFECT or r[0] > 3 or r[1] > 2 or r[5] != 0 or r[7][0] == 0:
            raise FxError("part enum out of range")
        table = {K_DECAL: "decals", K_LIGHT: "lights", K_PCTL: "pctl"}.get(r[4])
        if table and r[8] >= n[table]:
            raise FxError("part reference index out of range")
        finite(r, "part")
    for r in t["emitters"]:
        if r[0] > 3 or r[1] > 2 or r[2] > 3 or r[11] > 5:
            raise FxError("emitter enum out of range")
        idx(r[4], "particles", "emitter particle")
        if r[4] == NONE16 or t["particles"][r[4]][0] != r[34]:
            raise FxError("emitter particle index and tag id disagree")
        finite(r, "emitter")
    for r in t["particles"]:
        idx(r[2], "bitmaps", "particle bitmap")
        idx(r[3], "physics", "particle physics")
        if r[23] > 2 or r[24] > 7 or r[25] > 2 or r[26] > 7 or r[27] > K_EFFECT or r[28] > K_EFFECT:
            raise FxError("particle enum out of range")
        finite(r, "particle")
    for r in t["physics"]:
        finite(r, "physics")
    for r in t["decals"]:
        idx(r[5], "bitmaps", "decal bitmap")
        if r[2] > 3 or r[3] > 4 or r[6] > 7:
            raise FxError("decal enum out of range")
        finite(r, "decal")
    for r in t["lights"]:
        if r[-1] > 15:
            raise FxError("light falloff function out of range")
        finite(r, "light")
    for r in t["pctl"]:
        idx(r[1], "physics", "particle system physics")
        rng(r[3], r[4], "pctl_types", "pctl types")
    for r in t["pctl_types"]:
        if r[0][-1] != 0 or r[3] > 1 or r[4] > 2:
            raise FxError("particle system type: bad name or enum")
        rng(r[6], r[7], "consts", "type constants")
        rng(r[9], r[10], "pctl_states", "type states")
        rng(r[12], r[13], "pctl_pstates", "type particle states")
        finite(r, "pctl type")
    for r in t["pctl_states"]:
        if r[0][-1] != 0 or r[15] > 15 or r[16] > 15:
            raise FxError("particle system state: bad name or enum")
        rng(r[17], r[18], "consts", "state constants")
        finite(r, "pctl state")
    for r in t["pctl_pstates"]:
        if r[0][-1] != 0 or r[23] > 7 or r[24] > 2 or r[25] > 7:
            raise FxError("particle state: bad name or enum")
        idx(r[5], "bitmaps", "particle state bitmap")
        idx(r[22], "physics", "particle state physics")
        rng(r[26], r[27], "consts", "particle state constants")
        finite(r, "pctl pstate")
    for r in t["consts"]:
        finite(r, "constant")
    for r in t["bitmaps"]:
        rng(r[1], r[2], "sequences", "bitmap sequences")
        for sq in t["sequences"][r[1]:r[1] + r[2]]:
            rng(sq[0], sq[1], "sprites", "sequence sprites")
            for sp in t["sprites"][sq[0]:sq[0] + sq[1]]:
                if sp[1] >= r[6]:
                    raise FxError("sprite image index outside the bitmap's images")
    for r in t["sequences"]:
        rng(r[0], r[1], "sprites", "sequence sprites")
    for r in t["sprites"]:
        finite(r, "sprite")
        if not (0.0 <= r[2] <= 1.0 and 0.0 <= r[4] <= 1.0 and 0.0 <= r[3] <= 1.0 and 0.0 <= r[5] <= 1.0):
            raise FxError("sprite rectangle outside the image")


def info_pack(path: Path) -> str:
    pk = hp2.parse(path.read_bytes())
    sec = parse_section(pk.read("FXD0"))
    tx = texconv.parse_texs(pk.read("FXTX"))
    L = [f"{path.name}: FXD0 {sec.size} bytes, FXTX {len(pk.read('FXTX'))} bytes ({len(tx.textures)} textures, "
         f"{len(tx.bindings)} bindings)"]
    L.append("  tables: " + ", ".join(f"{n} {len(sec.tables[n])}" for n in TABLES))
    return "\n".join(L)


# ---- reporting ------------------------------------------------------------------------------------------------------------
def describe(m: texconv.MapData, fx: FxSet, tx: FxTextures | None = None) -> str:
    name = m.path.stem
    ev = sum(len(e.events) for e in fx.effects)
    parts = Counter(KIND_NAMES[p.kind] for e in fx.effects for v in e.events for p in v.parts)
    emit = sum(len(v.emitters) for e in fx.effects for v in e.events)
    L = [f"{name}: {len(fx.effects)} effects, {ev} events, {sum(parts.values())} parts ({dict(parts)}), {emit} particle emitters",
         f"  definitions: {len(fx.particles)} particles, {len(fx.decals)} decals, {len(fx.lights)} lights, "
         f"{len(fx.pctl)} particle systems, {len(fx.physics)} point physics, {len(fx.bitmaps)} bitmaps "
         f"({sum(len(s) for b in fx.bitmaps for s in b.sequences)} sprites), {len(fx.objects)} object tags created"]
    if tx is not None:
        psm4 = sum(1 for t in tx.textures if t.psm == texconv.PSMT4)
        L.append(f"  FXTX: {len(tx.textures)} textures ({psm4} PSMT4), {tx.bytes / 1024:.0f} KiB pixels + CLUTs, "
                 f"max side {tx.cap}" + (f", skipped {dict(tx.skipped)}" if tx.skipped else ""))
    for k, v in sorted(fx.stats.items()):
        L.append(f"  note: {k}: {v}")
    for p in fx.problems[:10]:
        L.append(f"  problem: {p}")
    if len(fx.problems) > 10:
        L.append(f"  ... {len(fx.problems) - 10} more problems")
    return "\n".join(L)


FXAT_MAGIC, FXAT_REC = b"FXAT", 32


def build_attachments(fx: FxSet) -> bytes:
    """FXAT: 'FXAT', u32 version 2, u32 count, u32 tag id of the glow bitmap (0xFFFFFFFF none), then count x { u32 object tag, u32 attached tag, u8 kind, 3 pad, char[20] marker }, sorted by object tag.
    Only attachments whose tag is in the FXD0 (an effect or particle system that was converted) are kept."""
    have_e = {e.tag_id for e in fx.effects}
    have_p = {p.tag_id for p in fx.pctl}
    have_l = {p.tag_id for p in fx.lights}
    rows = [a for a in fx.attach if (a[1] == ATT_PCTL and a[2] in have_p) or (a[1] == ATT_EFFE and a[2] in have_e) or (a[1] == ATT_LIGH and a[2] in have_l)]
    glow = NULL_ID
    for b in fx.bitmaps:
        if b.path == GLOW_BITMAP:
            glow = b.tag_id
    out = bytearray(FXAT_MAGIC + struct.pack("<III", 2, len(rows), glow))
    for obj, kind, tid, marker in rows:
        out += struct.pack("<IIB3x20s", obj, tid, kind, marker.encode("latin1")[:19])
    return bytes(out)


def convert_map(path: Path, all_effects: bool = False, budget: int = FX_DEFAULT_BUDGET
                ) -> tuple[bytes, FxSet, FxTextures, texconv.MapData]:
    m = texconv.MapData.open(path)
    fx = collect(m, all_effects)
    tx = build_textures(m, fx, budget)
    fxd = build(fx, tx.tex_index)
    pack = hp2.build(path.stem, [("FXD0", fxd), ("FXTX", tx.blob), ("FXAT", build_attachments(fx))])
    return pack, fx, tx, m


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--all", action="store_true", help="convert every effect tag of the map, not only the gameplay set")
    ap.add_argument("--budget", type=int, default=FX_DEFAULT_BUDGET, help="FXTX pixel + CLUT bytes")
    ap.add_argument("--info", action="store_true", help="report instead of writing")
    args = ap.parse_args(argv)
    if args.info and args.input.suffix == ".hp2":
        print(info_pack(args.input))
        return 0
    t0 = time.time()
    if args.info:
        m = texconv.MapData.open(args.input)
        fx = collect(m, args.all)
        print(describe(m, fx, build_textures(m, fx, args.budget)))
        return 0
    if not args.out:
        ap.error("-o is required")
    pack, fx, tx, m = convert_map(args.input, args.all, args.budget)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(pack)
    print(describe(m, fx, tx))
    print(f"wrote {args.out} ({len(pack) / 1024:.0f} KiB pack, {time.time() - t0:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
