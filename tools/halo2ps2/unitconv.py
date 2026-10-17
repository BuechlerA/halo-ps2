#!/usr/bin/env python3
"""Convert the unit data of an Xbox map (``globals`` player constants, every ``biped`` tag, the basic data of every
``vehicle`` tag) into the PS2 ``UNIT`` section, version 1 (docs/17-unit-section.md; the reader is
engine/src/sim/unitdef.c, contract engine/src/sim/unitdef.h).

Everything below was read off the retail maps (all ten campaign maps; a10 in depth) and every offset is checked
against real data: "[V]" = verified (a numeric relation to other data, an animation, a cached field, a name or a
monotonic pattern confirms it), "[P]" = plausible (value ranges and names fit the public tag documentation, nothing
in the data contradicts it), "[U]" = layout known, meaning unverified.  Offsets are inside the tag's data block, every
pointer is an absolute tag-space address, a reflexive is ``u32 count, u32 address, u32 pad`` and a tag reference is
16 B ``char[4] group (byte-reversed), u32 path pointer, u32 path length (0), u32 tag id`` (id 0xFFFFFFFF = null).

UNITS OF THE SOURCE DATA (the point of this module's checks)
    * time: the game ticks at 30 Hz.  Fields in seconds are marked (s).
    * velocities of units are per TICK (jump velocity 0.07 wu/tick = 2.1 wu/s) [V: the cached falling velocities of
      the globals tag are sqrt(2 g d) for d = 3, 6, 16.5 wu with ONE g = 0.003565 wu/tick^2 on all ten maps =
      3.2085 wu/s^2 = 9.78 m/s^2 at 1 wu = 3.048 m; so a tick is 1/30 s]
    * player speeds in ``globals`` are wu per SECOND, accelerations in the same block are not scaled to a time unit
      we can prove [V: the player's run-forward value 2.25 equals the root motion of the cyborg's ``stand rifle
      move-front`` animation, 1.5 wu in 20 frames = 0.075 wu/tick = 2.25 wu/s, and run-backward / sideways 2.0 =
      1.34 wu in 20 frames, sneak 0.9 / 0.65 / 0.6 = the crouch cycles' 0.9 / -0.65 / 0.6 wu in 30 frames = 1 s]
    * the biped's landing velocities (soft / hard landing, death) are wu per SECOND [P: the harmful falling distances
      3..6 wu of ``globals`` are 4.39..6.2 wu/s at g, and the biped's minimum hard landing velocity is 5]
    * vehicle speeds and accelerations are per tick / per tick^2 [P: warthog 0.255 wu/tick = 7.65 wu/s = 23 m/s and
      it needs 0.255 / 0.00275 = 93 ticks to get there; ghost .225, banshee .22, scorpion .14 fit; a per-second
      reading (0.255 wu/s) is absurd]
    * angles are radians except where a field says degrees (``globals`` look rates, vehicle turn angles).

globals (``matg``), engine version 5
    +0x0F8 sound references (2 x 16 B: water enter, water exit)                                          [V: paths]
    +0x104 camera (1 x 16 B tag reference ``trak``: the default unit camera track)                        [V: path]
    +0x110 player control (1 x 128 B, + 6 floats of look function data)                                   [V: size]
    +0x11C difficulty (1 x 644 B)                      +0x128 grenades (2 x 68 B)
    +0x170 player information (1 element; first element: default biped)  +0x188 falling damage (1 x 152 B)
    +0x194 materials (32 x 884 B)
    player information element: +0x00 default biped reference, +0x2C f32 [U] (0.512), +0x30 walking speed [P],
        +0x34 run forward [V], +0x38 run backward [V], +0x3C run sideways [V], +0x40 run acceleration [U],
        +0x44 sneak forward [V], +0x48 sneak backward [V], +0x4C sneak sideways [V], +0x50 sneak acceleration [U],
        +0x54 airborne acceleration [U], +0x6C f32 [U] (0.05), +0x80 stun movement penalty [P], +0x84 stun turning
        penalty [P], +0x88 stun jumping penalty [P], +0x8C minimum stun time [P], +0x90 maximum stun time (s) [P],
        +0x9C/+0xA0 first person idle time bounds (s) [P], +0xA4 first person skip fraction [P],
        +0xB8 coop respawn effect reference [V: path]
    player control element: +0x00 magnetism friction, +0x04 magnetism adhesion, +0x08 inconsequential target scale [P],
        +0x40 .. +0x54 six floats [U: 1.0, 3.0, 0.85, 60 (pitch rate, degrees/s), 120 (yaw rate), 0.5; the two rates
        are identified by magnitude, the other four are the look acceleration group: seconds to start, scale,
        decay, peg threshold in some order], +0x6C u16 minimum weapon swap ticks, +0x6E u16 minimum autolevelling
        ticks [P], +0x70 minimum angle for vehicle flip (rad) [P], +0x74 look function (6 f32: 0, .05, .1, .25, .58, 1)
        [P: a monotone response curve]
    difficulty element: 31 groups of 4 f32 (easy, normal, hard, impossible) at +0x10 * group.  [V] groups 0..8 (enemy
        damage .3 1 1.4 1.8, enemy vitality, enemy shield, enemy recharge, friend damage/vitality/shield/recharge,
        infection forms) and 10..21, 23, 24 follow the public names in order and the expected monotonic pattern;
        9, 22, 25..27 are all zero; 28..30 [U].
    grenade element (68 B): +0x00 u16 maximum count [P], +0x02 u16 [U], +0x04 throwing effect, +0x14 HUD interface,
        +0x24 equipment, +0x34 projectile (tag references) [V: paths]
    falling damage element: +0x08/+0x0C harmful falling distance bounds (3, 6), +0x10 falling damage (``jpt!``),
        +0x28 maximum falling distance (16.5), +0x2C distance damage, +0x3C vehicle environment collision damage,
        +0x4C vehicle killed unit damage, +0x5C vehicle collision damage, +0x6C flaming death damage,
        +0x8C/+0x90/+0x94 cached velocities for 16.5 / 3 / 6 wu (the proof of g above)
    material element (884 B; the index is the material type, the name is not stored): +0x94 f32 [U] (0.125 on index 31
        only), +0x2D4 f32 [U] (10 on index 9 only), +0x2E4 effect reference [U] (null on all), +0x2F4 sound reference
        (``glass_medium`` on index 9 only) [U], +0x31C particle effects reflexive (index 9 only: 1 element whose
        +0x00 is a ``part`` reference) [U], +0x364 melee hit sound [V: names match the standard material order
        (dirt, sand, stone, snow, wood, ... glass, force field, ...) and the shield material indices of the biped
        collision models (cyborg 22, marine 23, elite 30, jackal 16) are the matching shield entries]

object (every unit): +0x00 u16 object type (biped 0, vehicle 1), +0x02 u16 flags, +0x04 bounding radius,
    +0x08 bounding offset, +0x20 acceleration scale (cyborg .7, elite .6, grunt .9, marines 1; "scales gravity" [U]),
    +0x28 model, +0x38 animation graph, +0x70 collision model, +0x80 physics (tag references) [V: groups and paths],
    +0x104 render bounding radius [V: = bounding radius on the cyborg].
unit (+0x17C): +0x17C u32 flags, +0x180 u16 default team, +0x1A0 camera field of view (rad; 1.2217 = 70 deg on every unit
    seen) [V], +0x1A4 camera stiffness [U], +0x1A8 / +0x1C8 camera / submerged camera marker names (String32; empty on
    every biped), +0x1E8 pitch auto level, +0x1EC pitch range (both 0 on every unit of the ten maps: the look limits
    are not in the tags), +0x1F4 camera tracks (16 B references ``trak``), +0x264 aiming velocity maximum (rad/s,
    2 pi), +0x268 aiming acceleration maximum, +0x26C casual aiming modifier, +0x270 looking velocity maximum,
    +0x274 looking acceleration maximum (AI turning, zero on the player biped) [P], +0x288 melee damage (``jpt!``) [V],
    +0x2B4 dialogue variants (24 B: u16 variant number, ``udlg`` reference at +0x08) [V: sargeant = 100 and
    sarge2 = 101 are the only non-zero numbers], +0x2C0 grenade velocity, +0x2C4 u16 grenade type (0 frag, 1 plasma),
    +0x2C6 u16 grenade count [P], +0x2D8 weapons (36 B: ``weap`` reference at +0) [V], +0x2E4 seats (284 B).
biped (+0x2F0): +0x2F0 moving turning speed (rad/s), +0x2F4 u32 flags (bit 1 passes through other bipeds, bit 9 "use
    player physics": set on every walking biped), +0x2F8 stationary turning threshold (rad), +0x314 reference ``jpt!``
    (unused), +0x324 bank angle, +0x328 bank apply time, +0x32C bank decay time, +0x330 pitch ratio, +0x334 maximum
    velocity, +0x338 maximum sidestep velocity, +0x33C acceleration, +0x340 deceleration, +0x344 angular velocity
    maximum, +0x348 angular acceleration maximum, +0x34C crouch velocity modifier  (all 0 on every biped of the ten maps:
    speeds come from ``globals`` for the player and from the animation root motion for everything else), +0x358
    maximum slope angle (45 deg) [V: its cosine is cached at +0x4D0], +0x35C downhill falloff angle, +0x360 downhill
    cutoff angle, +0x364 downhill velocity scale, +0x368 uphill falloff angle, +0x36C uphill cutoff angle, +0x370
    uphill velocity scale [V: sines cached at +0x4D4..+0x4E0], +0x38C footsteps (reference ``foot``), +0x3B4 jump
    velocity (wu/tick) [V], +0x3D4 maximum soft landing time (s), +0x3D8 maximum hard landing time (s), +0x3DC
    minimum soft landing velocity, +0x3E0 minimum hard landing velocity, +0x3E4 maximum hard landing velocity,
    +0x3E8 death hard landing velocity [P], +0x400 standing camera height, +0x404 crouching camera height, +0x408
    crouch transition time (s) [V: +0x4CC caches 1 / (time * 30) = 0.1667 on every biped], +0x424 standing collision
    height, +0x428 crouching collision height, +0x42C collision radius, +0x430 autoaim width [V: names and ranges, the
    standing height 0.7 of the cyborg equals its model height: head node z 0.61 + feet 0.07 in docs/12], +0x4C8 cached
    cosine of the stationary turning threshold [V], +0x4E4 u16 pelvis node, u16 head node [V: they index "bip01 pelvis" /
    "bip01 head" of the model on all 114 bipeds that have them; 29 bipeds (flood, sentinels, monitor, turrets) use 0xFFFF], +0x4E8 contact points (64 B: String32 marker name at +0x20: "left foot", "right foot").
vehicle (+0x2F0): +0x2F0 u32 flags, +0x2F4 u16 type (0 tank, 1 jeep, 2 boat, 3 plane, 4 scout, 5 fighter, 6 turret [V:
    scorpion 0, warthog 1, pelican 3, ghost 4, banshee 5, gun turret 6]), +0x2F8 maximum forward speed, +0x2FC maximum
    reverse speed, +0x300 speed acceleration, +0x304 speed deceleration, +0x308 maximum left turn, +0x30C maximum right
    turn (degrees: 30 / -30 on the warthog), +0x310 wheel circumference, +0x314 turn rate [P].
    seat (284 B): +0x00 u32 flags, +0x04 label String32 ("W-driver"), +0x24 marker name ("driver"), +0x84 camera marker
    name, +0xC8 / +0xCC camera pitch bounds (rad) [V: warthog driver -30..15 deg, passenger -50..90, gunner -20..35].
collision model (the vitals; read here because the contract asks for health / shield / recharge): +0x00 u32 flags,
    +0x04 u16 indirect damage material, +0x08 maximum body vitality [V: 75 cyborg, 100 elite, 40 grunt, 50 marine, 25
    crewman], +0xCC maximum shield vitality [V], +0xD2 u16 shield material [V], +0x10C shield stun time (s) [U],
    +0x110 shield recharge time (s) [V: +0x1C0 caches 1 / (time * 30) on every biped with a shield], +0x1C0 cached
    recharge per tick.
animation graph (``antr``): +0x0C units (100 B), in them +0x58 weapons (188 B): +0x00 name, +0x20 marker, +0x40 hold
    marker ("right hand", "right hand elite", "left hand jackal") [V: scenconv docs]; +0x74 animations (180 B):
    +0x00 name, +0x22 frame count, +0x26 frame info type (1 dx dy, 2 dx dy dyaw, 3 dx dy dz dyaw), +0x48 frame info
    data.  The derived ``anim_speed`` of a biped is the root motion of its stand/crouch move animations.

Usage:
    python3 -I tools/halo2ps2/unitconv.py assets/decompressed/a10.map -o assets/hp2/a10.unit.hp2
    python3 -I tools/halo2ps2/unitconv.py --info assets/decompressed/a10.map      # print what was read
    python3 -I tools/halo2ps2/unitconv.py --info assets/hp2/a10.unit.hp2          # print a converted pack
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, scenconv, tagindex, texconv  # noqa: E402

MAGIC = b"UNIT"
VERSION = 1
HEADER_SIZE = 128
NAME_LEN = 32
MAX_BIPEDS = 255
MAX_VEHICLES = 255
MAX_SEATS = 1024
MAX_MATERIALS = 64
MAX_WEAPONS = 4
MAX_DIALOGUE = 8
DIFF_GROUPS = 31
NO_TAG = 0                      # tag ids in the section: 0 = none (0xFFFFFFFF in the source)
TICKS_PER_SECOND = 30

# ---- source offsets (docstring above) --------------------------------------------------------------
G_SOUNDS, G_CAMERA, G_CONTROL, G_DIFFICULTY, G_GRENADES = 0x0F8, 0x104, 0x110, 0x11C, 0x128
G_PLAYER, G_FALLING, G_MATERIALS = 0x170, 0x188, 0x194
CONTROL_REC, DIFFICULTY_REC, GRENADE_REC, FALLING_REC, MATERIAL_REC, PLAYER_MIN = 128, 644, 68, 152, 884, 0xB8 + 16
SEAT_REC, WEAPON_REC, DIALOGUE_REC, CONTACT_REC = 284, 36, 24, 64
VEHICLE_TYPES = ("tank", "jeep", "boat", "plane", "scout", "fighter", "turret")
MATERIAL_NAMES = (   # the standard material type order (stored by index only; names are for reports)
    "dirt", "sand", "stone", "snow", "wood", "metal (hollow)", "metal (thin)", "metal (thick)", "rubber", "glass",
    "force field", "grunt", "hunter armor", "hunter skin", "elite", "jackal", "jackal energy shield", "engineer skin",
    "engineer force field", "flood combat form", "flood carrier form", "cyborg armor", "cyborg energy shield",
    "human armor", "human skin", "sentinel", "monitor", "plastic", "water", "leaves", "elite energy shield", "ice",
    "hunter shield")
DIFFICULTY_NAMES = {
    0: "enemy_damage", 1: "enemy_vitality", 2: "enemy_shield", 3: "enemy_recharge", 4: "friend_damage",
    5: "friend_vitality", 6: "friend_shield", 7: "friend_recharge", 8: "infection_forms", 10: "rate_of_fire",
    11: "projectile_error", 12: "burstiness", 13: "new_target_delay", 14: "burst_separation", 15: "target_tracking",
    16: "target_leading", 17: "overcharge_chance", 18: "special_fire_delay", 19: "guidance_vs_player",
    20: "melee_delay_base", 21: "melee_delay_scale", 23: "grenade_chance_scale", 24: "grenade_timer_scale"}


class UnitError(ValueError):
    pass


# ---- record layouts ------------------------------------------------------------------------------
# One list per record: (field, struct format).  Offsets follow from the order (natural alignment, no implicit
# padding: every "pad" is spelled out), the sizes are asserted below and are the contract with unitdef.c.
def _layout(fields: list[tuple[str, str]], size: int) -> tuple[list[tuple[str, str, int]], int]:
    out, off = [], 0
    for name, fmt in fields:
        n = struct.calcsize("<" + fmt)
        a = min(4, struct.calcsize("<" + fmt[-1])) if not fmt.endswith("s") else 1
        off = (off + a - 1) // a * a
        out.append((name, fmt, off))
        off += n
    if off > size:
        raise AssertionError(f"layout is {off} B, record is {size} B")
    return out, size


GLOBALS_FIELDS = [
    ("default_biped_tag", "I"), ("coop_effect_tag", "I"), ("camera_track_tag", "I"), ("water_enter_sound", "I"),
    ("water_exit_sound", "I"), ("pad0", "3I"),
    # player information (0x20)
    ("walk_speed", "f"), ("run_forward", "f"), ("run_backward", "f"), ("run_sideways", "f"), ("run_accel", "f"),
    ("sneak_forward", "f"), ("sneak_backward", "f"), ("sneak_sideways", "f"), ("sneak_accel", "f"),
    ("airborne_accel", "f"), ("pi_unk_2c", "f"), ("pi_unk_6c", "f"),
    ("stun_move_penalty", "f"), ("stun_turn_penalty", "f"), ("stun_jump_penalty", "f"), ("stun_min_time", "f"),
    ("stun_max_time", "f"), ("fp_idle_min", "f"), ("fp_idle_max", "f"), ("fp_skip_fraction", "f"),
    # player control (0x70)
    ("magnetism_friction", "f"), ("magnetism_adhesion", "f"), ("inconsequential_scale", "f"),
    ("look_a", "f"), ("look_b", "f"), ("look_c", "f"), ("look_pitch_rate_deg", "f"), ("look_yaw_rate_deg", "f"),
    ("look_f", "f"), ("min_weapon_swap_ticks", "H"), ("min_autolevel_ticks", "H"), ("min_vehicle_flip_angle", "f"),
    ("look_function", "6f"),
    # falling damage (0xB4)
    ("harmful_fall_min", "f"), ("harmful_fall_max", "f"), ("max_fall_distance", "f"), ("gravity", "f"),
    ("fall_velocity_max", "f"), ("fall_velocity_harm_min", "f"), ("fall_velocity_harm_max", "f"),
    ("damage_tags", "6I"), ("pad1", "2I"),
    # grenades (0xF0): 2 x 32
    ("grenades", "64s"),
    # difficulty (0x140)
    ("difficulty", f"{DIFF_GROUPS * 4}f"),
]
GLOBALS_L, GLOBALS_REC = _layout(GLOBALS_FIELDS, 832)
GRENADE_FIELDS = [("max_count", "H"), ("unk", "H"), ("throw_effect", "I"), ("hud_interface", "I"), ("equipment", "I"),
                  ("projectile", "I")]    # 20 B used of 32
DAMAGE_TAG_NAMES = ("falling", "distance", "vehicle_environment", "vehicle_killed_unit", "vehicle_collision",
                    "flaming_death")

BIPED_FIELDS = [
    ("tag_id", "I"), ("model_tag", "I"), ("anim_tag", "I"), ("collision_tag", "I"), ("physics_tag", "I"),
    ("camera_track_tag", "I"), ("footsteps_tag", "I"), ("melee_damage_tag", "I"), ("weapon_tags", "4I"),
    ("dialogue", "16I"),                         # 8 x (u32 variant number, u32 dialogue tag id)
    ("bounding_radius", "f"), ("bounding_offset", "3f"), ("render_radius", "f"), ("accel_scale", "f"),
    ("camera_fov", "f"), ("camera_stiffness", "f"), ("pitch_auto_level", "f"), ("pitch_range", "2f"),
    ("stand_height", "f"), ("crouch_height", "f"), ("radius", "f"), ("autoaim_width", "f"),
    ("stand_camera", "f"), ("crouch_camera", "f"), ("crouch_time", "f"), ("jump_velocity", "f"),
    ("slope", "7f"),                             # max slope, downhill falloff/cutoff/scale, uphill falloff/cutoff/scale
    ("turn_speed", "f"), ("turn_threshold", "f"),
    ("landing", "6f"),                           # soft time, hard time, min soft v, min hard v, max hard v, death v
    ("tag_motion", "7f"),                        # max velocity, sidestep, accel, decel, ang vel, ang accel, crouch mod
    ("aim", "5f"),                               # aiming vel/accel max, casual modifier, looking vel/accel max
    ("anim_speed", "4f"),                        # stand run fwd/back/side, crouch fwd (wu/s, from the animation graph)
    ("grenade_velocity", "f"),
    ("health", "f"), ("shield", "f"), ("shield_recharge_time", "f"), ("shield_stun_time", "f"),
    ("shield_recharge_rate", "f"),
    ("unit_flags", "I"), ("biped_flags", "I"), ("coll_flags", "I"),
    ("object_flags", "H"), ("default_team", "H"), ("pelvis_node", "H"), ("head_node", "H"),
    ("shield_material", "H"), ("body_material", "H"), ("grenade_type", "H"), ("grenade_count", "H"),
    ("weapon_count", "B"), ("dialogue_count", "B"), ("pad", "2B"),
    ("name", "32s"), ("hold_marker", "32s"), ("aux_marker", "32s"), ("camera_marker", "32s"),
    ("foot_markers", "64s"),
]
BIPED_L, BIPED_REC = _layout(BIPED_FIELDS, 0x240)

VEHICLE_FIELDS = [
    ("tag_id", "I"), ("model_tag", "I"), ("anim_tag", "I"), ("collision_tag", "I"), ("physics_tag", "I"),
    ("camera_track_tag", "I"), ("weapon_tags", "4I"),
    ("bounding_radius", "f"), ("bounding_offset", "3f"), ("accel_scale", "f"), ("camera_fov", "f"),
    ("max_forward", "f"), ("max_reverse", "f"), ("accel", "f"), ("decel", "f"), ("max_left_turn", "f"),
    ("max_right_turn", "f"), ("wheel_circumference", "f"), ("turn_rate", "f"),
    ("health", "f"), ("shield", "f"), ("shield_recharge_time", "f"),
    ("unit_flags", "I"), ("vehicle_flags", "I"), ("coll_flags", "I"),
    ("type", "H"), ("first_seat", "H"), ("seat_count", "H"), ("object_flags", "H"), ("default_team", "H"),
    ("shield_material", "H"), ("weapon_count", "B"), ("pad", "B"),
    ("name", "32s"),
]
VEHICLE_L, VEHICLE_REC = _layout(VEHICLE_FIELDS, 0x100)

SEAT_FIELDS = [
    ("flags", "I"), ("pitch_min", "f"), ("pitch_max", "f"), ("pad", "I"),
    ("label", "32s"), ("marker", "32s"), ("camera_marker", "32s"),
]
SEAT_L, SEAT_REC_OUT = _layout(SEAT_FIELDS, 128)

MATERIAL_FIELDS = [
    ("melee_sound", "I"), ("impact_sound", "I"), ("effect", "I"), ("particle_tag", "I"),
    ("param_a", "f"), ("param_b", "f"), ("particle_count", "H"), ("pad", "H"),
]
MATERIAL_L, MATERIAL_REC_OUT = _layout(MATERIAL_FIELDS, 48)

# header: +0 magic, +4 u16 version, +8 u32 size, +0x0C biped count, +0x10 vehicle count, +0x14 material count,
# +0x18 seat count, +0x1C globals offset, +0x20 bipeds, +0x24 vehicles, +0x28 materials, +0x2C seats,
# +0x30 u32 player biped tag id (0 none), +0x34 u32 biped index of it + 1 (0 none)


@dataclass
class Rec:
    """A record under construction: field name -> value(s)."""
    layout: list
    size: int
    vals: dict = field(default_factory=dict)

    def set(self, name: str, value) -> None:
        self.vals[name] = value

    def pack(self) -> bytes:
        buf = bytearray(self.size)
        known = {n for n, _, _ in self.layout}
        for n in self.vals:
            if n not in known:
                raise AssertionError(f"unknown field {n}")
        for name, fmt, off in self.layout:
            v = self.vals.get(name)
            if v is None:
                continue
            if fmt.endswith("s"):
                n = int(fmt[:-1] or 1)
                if isinstance(v, str):                       # text: keep the terminating NUL
                    raw = v.encode("latin1")[:n - 1]
                else:                                        # packed sub-records (grenades, foot markers)
                    raw = bytes(v)
                    if len(raw) > n:
                        raise AssertionError(f"{name}: {len(raw)} bytes for a {n} byte field")
                buf[off:off + len(raw)] = raw
            else:
                vs = v if isinstance(v, (tuple, list)) else (v,)
                struct.pack_into("<" + fmt, buf, off, *vs)
        return bytes(buf)


def parse_rec(layout: list, data: bytes, off: int = 0) -> dict:
    """Reference parser of one record (tests, --info)."""
    out = {}
    for name, fmt, o in layout:
        vals = struct.unpack_from("<" + fmt, data, off + o)
        if fmt.endswith("s"):
            out[name] = vals[0]
        else:
            out[name] = vals[0] if len(vals) == 1 else vals
    return out


def cstring(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin1")


# ---- tag access ------------------------------------------------------------------------------------
class Reader:
    """Bounds-checked access to the tag region (scenconv.Region) with unit-specific helpers."""

    def __init__(self, m: texconv.MapData):
        self.m = m
        self.r = scenconv.Region(m)
        self.problems: list[str] = []

    def u(self, fmt: str, off: int):
        try:
            return self.r.u(fmt, off)
        except scenconv.ScenError as ex:
            raise UnitError(str(ex)) from None

    def f(self, off: int) -> float:
        v = self.u("<f", off)[0]
        if not math.isfinite(v):
            raise UnitError(f"non-finite float at {off:#x}")
        return v

    def floats(self, off: int, n: int) -> tuple[float, ...]:
        return tuple(self.f(off + 4 * i) for i in range(n))

    def refl(self, off: int, stride: int, limit: int = 4096) -> tuple[int, int]:
        try:
            return self.r.reflexive(off, stride, limit)
        except scenconv.ScenError as ex:
            raise UnitError(str(ex)) from None

    def ref(self, off: int, group: str | None = None) -> int:
        """Tag id of the reference at ``off`` (NO_TAG when null, of another group than ``group``, or not in the map)."""
        try:
            g, tid = self.r.tagref(off)
        except scenconv.ScenError as ex:
            raise UnitError(str(ex)) from None
        if g is None or tid == 0xFFFFFFFF or (tid == 0 and not g.strip("\0")):    # null (or all-zero) reference
            return NO_TAG
        e = self.m.by_id.get(tid)
        if e is None or e.group != g or (group is not None and g != group):
            self.problems.append(f"reference at {off:#x}: {g!r} {tid:#x} not resolved")
            return NO_TAG
        return tid

    def name(self, off: int, n: int = NAME_LEN) -> str:
        return self.r.cstr(off, n)

    def entries(self, group: str) -> list[tagindex.TagEntry]:
        return sorted((e for e in self.m.index.entries if e.group == group), key=lambda e: e.tag_id)

    def block(self, e: tagindex.TagEntry) -> int:
        return self.r.block(e)


def _tail(path: str) -> str:
    return path.rsplit("\\", 1)[-1][:NAME_LEN - 1]


# ---- globals ----------------------------------------------------------------------------------------
def read_globals(rd: Reader) -> Rec:
    glob = rd.entries("matg")
    if len(glob) != 1:
        raise UnitError(f"{len(glob)} globals tags")
    gb = rd.block(glob[0])
    g = Rec(GLOBALS_L, GLOBALS_REC)

    n, o = rd.refl(gb + G_SOUNDS, 16)
    if n >= 1:
        g.set("water_enter_sound", rd.ref(o, "snd!"))
    if n >= 2:
        g.set("water_exit_sound", rd.ref(o + 16, "snd!"))
    n, o = rd.refl(gb + G_CAMERA, 16)
    if n >= 1:
        g.set("camera_track_tag", rd.ref(o, "trak"))

    n, o = rd.refl(gb + G_PLAYER, 8)               # element size unknown (> 0xC8): the minimum is checked
    if n < 1:
        raise UnitError("globals has no player information")
    if o + PLAYER_MIN > len(rd.r.data):
        raise UnitError("player information outside the tag region")
    g.set("default_biped_tag", rd.ref(o, "bipd"))
    g.set("coop_effect_tag", rd.ref(o + 0xB8, "effe"))
    pi = rd.floats
    g.set("walk_speed", pi(o + 0x30, 1)[0])
    for name, at in (("run_forward", 0x34), ("run_backward", 0x38), ("run_sideways", 0x3C), ("run_accel", 0x40),
                     ("sneak_forward", 0x44), ("sneak_backward", 0x48), ("sneak_sideways", 0x4C),
                     ("sneak_accel", 0x50), ("airborne_accel", 0x54), ("pi_unk_2c", 0x2C), ("pi_unk_6c", 0x6C),
                     ("stun_move_penalty", 0x80), ("stun_turn_penalty", 0x84), ("stun_jump_penalty", 0x88),
                     ("stun_min_time", 0x8C), ("stun_max_time", 0x90), ("fp_idle_min", 0x9C), ("fp_idle_max", 0xA0),
                     ("fp_skip_fraction", 0xA4)):
        g.set(name, rd.f(o + at))

    n, o = rd.refl(gb + G_CONTROL, CONTROL_REC)
    if n < 1:
        raise UnitError("globals has no player control")
    for name, at in (("magnetism_friction", 0x00), ("magnetism_adhesion", 0x04), ("inconsequential_scale", 0x08),
                     ("look_a", 0x40), ("look_b", 0x44), ("look_c", 0x48), ("look_pitch_rate_deg", 0x4C),
                     ("look_yaw_rate_deg", 0x50), ("look_f", 0x54), ("min_vehicle_flip_angle", 0x70)):
        g.set(name, rd.f(o + at))
    g.set("min_weapon_swap_ticks", rd.u("<H", o + 0x6C)[0])
    g.set("min_autolevel_ticks", rd.u("<H", o + 0x6E)[0])
    ln, lo = rd.refl(o + 0x74, 4, 64)
    look = [rd.f(lo + 4 * i) for i in range(ln)][:6]
    g.set("look_function", tuple(look + [0.0] * (6 - len(look))))

    n, o = rd.refl(gb + G_FALLING, FALLING_REC)
    if n < 1:
        raise UnitError("globals has no falling damage")
    hmin, hmax, dmax = rd.f(o + 0x08), rd.f(o + 0x0C), rd.f(o + 0x28)
    g.set("harmful_fall_min", hmin)
    g.set("harmful_fall_max", hmax)
    g.set("max_fall_distance", dmax)
    vmax, vmin, vhmax = rd.f(o + 0x8C), rd.f(o + 0x90), rd.f(o + 0x94)
    g.set("fall_velocity_max", vmax)
    g.set("fall_velocity_harm_min", vmin)
    g.set("fall_velocity_harm_max", vhmax)
    gs = [v * v / (2.0 * d) for v, d in ((vmax, dmax), (vmin, hmin), (vhmax, hmax)) if d > 0]
    if not gs or max(gs) - min(gs) > 1e-3 * max(gs):
        rd.problems.append(f"falling velocities do not give one gravity: {gs}")
    g.set("gravity", sum(gs) / len(gs) if gs else 0.0)
    g.set("damage_tags", tuple(rd.ref(o + at, "jpt!") for at in (0x10, 0x2C, 0x3C, 0x4C, 0x5C, 0x6C)))

    gn, go = rd.refl(gb + G_GRENADES, GRENADE_REC, 16)
    gbuf = bytearray(64)
    for i in range(min(gn, 2)):
        e = go + i * GRENADE_REC
        struct.pack_into("<HHIIII", gbuf, 32 * i, rd.u("<H", e)[0], rd.u("<H", e + 2)[0], rd.ref(e + 0x04, "effe"),
                         rd.ref(e + 0x14, "grhi"), rd.ref(e + 0x24, "eqip"), rd.ref(e + 0x34, "proj"))
    g.set("grenades", bytes(gbuf))

    dn, do = rd.refl(gb + G_DIFFICULTY, DIFFICULTY_REC)
    if dn < 1:
        raise UnitError("globals has no difficulty block")
    g.set("difficulty", rd.floats(do, DIFF_GROUPS * 4))
    return g


def read_materials(rd: Reader) -> list[Rec]:
    gb = rd.block(rd.entries("matg")[0])
    n, o = rd.refl(gb + G_MATERIALS, MATERIAL_REC, MAX_MATERIALS)
    out = []
    for i in range(n):
        e = o + i * MATERIAL_REC
        m = Rec(MATERIAL_L, MATERIAL_REC_OUT)
        m.set("melee_sound", rd.ref(e + 0x364, "snd!"))
        m.set("impact_sound", rd.ref(e + 0x2F4, "snd!"))
        m.set("effect", rd.ref(e + 0x2E4, "effe"))
        m.set("param_a", rd.f(e + 0x94))
        m.set("param_b", rd.f(e + 0x2D4))
        pn, po = rd.refl(e + 0x31C, 8, 64)
        m.set("particle_count", pn)
        if pn:
            m.set("particle_tag", rd.ref(po, "part"))
        out.append(m)
    return out


# ---- animation derived speeds ---------------------------------------------------------------------
def _frame_info_sum(rd: Reader, ab: int) -> tuple[float, float, int] | None:
    """(sum dx, sum dy, frame count) of the animation block at ``ab`` or None when it has no root motion."""
    fc, fit = rd.u("<hh", ab + 0x22)[0], rd.u("<h", ab + 0x26)[0]
    k = {1: 2, 2: 3, 3: 4}.get(fit)
    if k is None or fc <= 0:
        return None
    size, _fl, _fo, ptr, _id = rd.u("<5I", ab + 0x48)
    o = ptr - rd.r.base
    if size != fc * 4 * k or o < 0 or o + size > len(rd.r.data):
        return None
    sx = sy = 0.0
    for f in range(fc):
        v = struct.unpack_from("<2f", rd.r.data, o + f * 4 * k)
        sx += v[0]
        sy += v[1]
    if not (math.isfinite(sx) and math.isfinite(sy)):
        return None
    return sx, sy, fc


def graph_markers_and_speeds(rd: Reader, graph_tid: int, cache: dict) -> dict:
    """Hold / aux marker names and the stand / crouch move speeds (wu/s) of an animation graph."""
    if graph_tid in cache:
        return cache[graph_tid]
    res = {"hold": "", "aux": "", "speed": (0.0, 0.0, 0.0, 0.0)}
    cache[graph_tid] = res
    e = rd.m.by_id.get(graph_tid)
    if e is None or e.group != "antr":
        return res
    gb = rd.block(e)
    un, uo = rd.refl(gb + 0x0C, 100, 256)
    units = [(rd.name(uo + i * 100), uo + i * 100) for i in range(un)]
    order = [u for u in units if u[0] == "stand"] + [u for u in units if u[0] != "stand"]
    for _label, ub in order:                        # first non-empty hold marker (stand unit first), likewise aux
        wn, wo = rd.refl(ub + 0x58, 188, 256)
        for j in range(wn):
            hold, aux = rd.name(wo + j * 188 + 0x40), rd.name(wo + j * 188 + 0x20)
            res["hold"] = res["hold"] or hold
            res["aux"] = res["aux"] or aux
    an, ao = rd.refl(gb + 0x74, 180, 4096)
    best: dict[str, tuple[int, float]] = {}

    def consider(key: str, pref: int, speed: float) -> None:
        if key not in best or pref < best[key][0]:
            best[key] = (pref, speed)

    for i in range(an):
        ab = ao + i * 180
        name = rd.name(ab).split("%")[0]
        parts = name.split(" ")
        if len(parts) != 3 or parts[0] not in ("stand", "crouch") or not parts[2].startswith("move-"):
            continue
        fi = _frame_info_sum(rd, ab)
        if fi is None:
            continue
        sx, sy, fc = fi
        d = parts[2][5:]
        mag = {"front": sx, "back": -sx, "left": sy, "right": -sy}.get(d)
        if mag is None or mag <= 0.0:
            continue
        pref = 0 if parts[1] == "rifle" else 1 if parts[1] == "unarmed" else 2
        consider(parts[0] + d, pref, mag / fc * TICKS_PER_SECOND)
    sp = [best.get(k, (0, 0.0))[1] for k in ("standfront", "standback", "standleft", "crouchfront")]
    # sideways: mean of left and right when both exist
    left, right = best.get("standleft"), best.get("standright")
    if left and right:
        sp[2] = (left[1] + right[1]) / 2.0
    elif right:
        sp[2] = right[1]
    res["speed"] = tuple(sp)
    return res


# ---- units ---------------------------------------------------------------------------------------
def read_vitals(rd: Reader, coll_tid: int) -> dict:
    """Health / shield / recharge from the collision model of a unit (all zero without one)."""
    v = {"health": 0.0, "shield": 0.0, "recharge_time": 0.0, "stun_time": 0.0, "recharge_rate": 0.0,
         "flags": 0, "shield_material": 0, "body_material": 0xFFFF}
    e = rd.m.by_id.get(coll_tid)
    if e is None or e.group != "coll":
        return v
    cb = rd.block(e)
    v["flags"] = rd.u("<I", cb)[0]
    v["body_material"] = rd.u("<H", cb + 4)[0]
    v["health"] = rd.f(cb + 0x08)
    v["shield"] = rd.f(cb + 0xCC)
    v["shield_material"] = rd.u("<H", cb + 0xD2)[0]
    v["stun_time"] = rd.f(cb + 0x10C)
    v["recharge_time"] = rd.f(cb + 0x110)
    v["recharge_rate"] = rd.f(cb + 0x1C0)
    if v["recharge_time"] > 0 and abs(v["recharge_rate"] * v["recharge_time"] * TICKS_PER_SECOND - 1.0) > 1e-3:
        rd.problems.append(f"{e.path}: cached recharge rate {v['recharge_rate']} != 1 / ({v['recharge_time']} s * 30)")
    return v


def read_unit_common(rd: Reader, e: tagindex.TagEntry, rec: Rec, otype: int) -> dict:
    """Object + unit fields shared by bipeds and vehicles. Returns extra values for the caller."""
    b = rd.block(e)
    got = rd.u("<H", b)[0]
    if got != otype:
        raise UnitError(f"{e.path}: object type {got}, expected {otype}")
    rec.set("tag_id", e.tag_id)
    rec.set("name", _tail(e.path))
    rec.set("model_tag", rd.ref(b + 0x28, "mode"))
    rec.set("anim_tag", rd.ref(b + 0x38, "antr"))
    coll = rd.ref(b + 0x70, "coll")
    rec.set("collision_tag", coll)
    rec.set("physics_tag", rd.ref(b + 0x80, "phys"))
    rec.set("object_flags", rd.u("<H", b + 2)[0])
    rec.set("bounding_radius", rd.f(b + 4))
    rec.set("bounding_offset", rd.floats(b + 8, 3))
    rec.set("accel_scale", rd.f(b + 0x20))
    rec.set("unit_flags", rd.u("<I", b + 0x17C)[0])
    rec.set("default_team", rd.u("<H", b + 0x180)[0])
    rec.set("camera_fov", rd.f(b + 0x1A0))
    cn, co = rd.refl(b + 0x1F4, 16, 16)
    rec.set("camera_track_tag", rd.ref(co, "trak") if cn else NO_TAG)
    wn, wo = rd.refl(b + 0x2D8, WEAPON_REC, 64)
    weapons = [rd.ref(wo + i * WEAPON_REC, "weap") for i in range(min(wn, MAX_WEAPONS))]
    rec.set("weapon_tags", tuple(weapons + [NO_TAG] * (MAX_WEAPONS - len(weapons))))
    rec.set("weapon_count", len([w for w in weapons if w]))
    return {"b": b, "coll": coll}


def read_biped(rd: Reader, e: tagindex.TagEntry, cache: dict) -> Rec:
    rec = Rec(BIPED_L, BIPED_REC)
    x = read_unit_common(rd, e, rec, 0)
    b = x["b"]
    rec.set("render_radius", rd.f(b + 0x104))
    rec.set("camera_stiffness", rd.f(b + 0x1A4))
    rec.set("camera_marker", rd.name(b + 0x1A8))
    rec.set("pitch_auto_level", rd.f(b + 0x1E8))
    rec.set("pitch_range", rd.floats(b + 0x1EC, 2))
    rec.set("aim", rd.floats(b + 0x264, 5))
    rec.set("melee_damage_tag", rd.ref(b + 0x288, "jpt!"))
    dn, do = rd.refl(b + 0x2B4, DIALOGUE_REC, 256)
    dial: list[int] = []
    for i in range(min(dn, MAX_DIALOGUE)):
        tid = rd.ref(do + i * DIALOGUE_REC + 8, "udlg")
        if tid:
            dial += [rd.u("<H", do + i * DIALOGUE_REC)[0], tid]
    if dn > MAX_DIALOGUE:
        rd.problems.append(f"{e.path}: {dn} dialogue variants, {MAX_DIALOGUE} kept")
    rec.set("dialogue", tuple(dial + [0] * (2 * MAX_DIALOGUE - len(dial))))
    rec.set("dialogue_count", len(dial) // 2)
    rec.set("grenade_velocity", rd.f(b + 0x2C0))
    rec.set("grenade_type", rd.u("<H", b + 0x2C4)[0])
    rec.set("grenade_count", rd.u("<H", b + 0x2C6)[0])

    rec.set("turn_speed", rd.f(b + 0x2F0))
    rec.set("biped_flags", rd.u("<I", b + 0x2F4)[0])
    rec.set("turn_threshold", rd.f(b + 0x2F8))
    rec.set("tag_motion", rd.floats(b + 0x334, 7))
    rec.set("slope", rd.floats(b + 0x358, 7))
    rec.set("footsteps_tag", rd.ref(b + 0x38C, "foot"))
    rec.set("jump_velocity", rd.f(b + 0x3B4))
    rec.set("landing", rd.floats(b + 0x3D4, 6))
    rec.set("stand_camera", rd.f(b + 0x400))
    rec.set("crouch_camera", rd.f(b + 0x404))
    rec.set("crouch_time", rd.f(b + 0x408))
    rec.set("stand_height", rd.f(b + 0x424))
    rec.set("crouch_height", rd.f(b + 0x428))
    rec.set("radius", rd.f(b + 0x42C))
    rec.set("autoaim_width", rd.f(b + 0x430))
    pelvis, head = rd.u("<2H", b + 0x4E4)
    rec.set("pelvis_node", pelvis)
    rec.set("head_node", head)
    cn, co = rd.refl(b + 0x4E8, CONTACT_REC, 16)
    foot = bytearray(64)
    for i in range(min(cn, 2)):
        nm = rd.name(co + i * CONTACT_REC + 0x20).encode("latin1")[:31]
        foot[32 * i:32 * i + len(nm)] = nm
    rec.set("foot_markers", bytes(foot))

    v = read_vitals(rd, x["coll"])
    rec.set("health", v["health"])
    rec.set("shield", v["shield"])
    rec.set("shield_recharge_time", v["recharge_time"])
    rec.set("shield_stun_time", v["stun_time"])
    rec.set("shield_recharge_rate", v["recharge_rate"])
    rec.set("coll_flags", v["flags"])
    rec.set("shield_material", v["shield_material"])
    rec.set("body_material", v["body_material"])

    if rec.vals["anim_tag"]:
        gm = graph_markers_and_speeds(rd, rec.vals["anim_tag"], cache)
        rec.set("hold_marker", gm["hold"])
        rec.set("aux_marker", gm["aux"])
        rec.set("anim_speed", gm["speed"])
    return rec


def read_vehicle(rd: Reader, e: tagindex.TagEntry, first_seat: int) -> tuple[Rec, list[Rec]]:
    rec = Rec(VEHICLE_L, VEHICLE_REC)
    x = read_unit_common(rd, e, rec, 1)
    b = x["b"]
    rec.set("vehicle_flags", rd.u("<I", b + 0x2F0)[0])
    typ = rd.u("<H", b + 0x2F4)[0]
    if typ >= len(VEHICLE_TYPES):
        raise UnitError(f"{e.path}: vehicle type {typ}")
    rec.set("type", typ)
    rec.set("max_forward", rd.f(b + 0x2F8))
    rec.set("max_reverse", rd.f(b + 0x2FC))
    rec.set("accel", rd.f(b + 0x300))
    rec.set("decel", rd.f(b + 0x304))
    rec.set("max_left_turn", rd.f(b + 0x308))
    rec.set("max_right_turn", rd.f(b + 0x30C))
    rec.set("wheel_circumference", rd.f(b + 0x310))
    rec.set("turn_rate", rd.f(b + 0x314))
    v = read_vitals(rd, x["coll"])
    rec.set("health", v["health"])
    rec.set("shield", v["shield"])
    rec.set("shield_recharge_time", v["recharge_time"])
    rec.set("coll_flags", v["flags"])
    rec.set("shield_material", v["shield_material"])
    sn, so = rd.refl(b + 0x2E4, SEAT_REC, 64)
    seats = []
    for i in range(sn):
        s = so + i * SEAT_REC
        sr = Rec(SEAT_L, SEAT_REC_OUT)
        sr.set("flags", rd.u("<I", s)[0])
        sr.set("label", rd.name(s + 0x04))
        sr.set("marker", rd.name(s + 0x24))
        sr.set("camera_marker", rd.name(s + 0x84))
        sr.set("pitch_min", rd.f(s + 0xC8))
        sr.set("pitch_max", rd.f(s + 0xCC))
        seats.append(sr)
    rec.set("first_seat", first_seat)
    rec.set("seat_count", sn)
    return rec, seats


@dataclass
class UnitSet:
    globals: Rec
    materials: list[Rec]
    bipeds: list[Rec]
    vehicles: list[Rec]
    seats: list[Rec]
    problems: list[str] = field(default_factory=list)

    def biped_by_tag(self, tid: int) -> Rec | None:
        return next((b for b in self.bipeds if b.vals["tag_id"] == tid), None)


def collect_map(m: texconv.MapData) -> UnitSet:
    rd = Reader(m)
    glob = read_globals(rd)
    mats = read_materials(rd)
    cache: dict = {}
    bipeds = [read_biped(rd, e, cache) for e in rd.entries("bipd")]
    vehicles: list[Rec] = []
    seats: list[Rec] = []
    for e in rd.entries("vehi"):
        v, s = read_vehicle(rd, e, len(seats))
        vehicles.append(v)
        seats += s
    if len(bipeds) > MAX_BIPEDS or len(vehicles) > MAX_VEHICLES or len(seats) > MAX_SEATS:
        raise UnitError("too many units for the section limits")
    return UnitSet(glob, mats, bipeds, vehicles, seats, rd.problems)


def collect(map_path: Path) -> UnitSet:
    return collect_map(texconv.MapData.open(Path(map_path)))


# ---- roots for the object pack -----------------------------------------------------------------------
def is_driveable(v: Rec) -> bool:
    """A vehicle the player or the AI can drive or ride, as opposed to the map's ``vehicle`` tags that are devices or
    scenery (chairs, cryotubes, the ring, docked lifepods: turret type, no speed)."""
    return v.vals["max_forward"] > 0.0 or v.vals["seat_count"] > 0 and v.vals["type"] != 6


def models_of(us: UnitSet, vehicles: bool = True) -> set[int]:
    out = {b.vals["model_tag"] for b in us.bipeds if b.vals["model_tag"]}
    if vehicles:
        out |= {v.vals["model_tag"] for v in us.vehicles if v.vals["model_tag"] and is_driveable(v)}
    return out


def anims_of(us: UnitSet, vehicles: bool = True) -> set[int]:
    out = {b.vals["anim_tag"] for b in us.bipeds if b.vals["anim_tag"]}
    if vehicles:
        out |= {v.vals["anim_tag"] for v in us.vehicles if v.vals["anim_tag"] and is_driveable(v)}
    return out


def referenced_models(map_path: Path, vehicles: bool = True) -> set[int]:
    """Tag ids of the ``model`` tags of every biped (and, with ``vehicles``, every driveable vehicle) of the map,
    keyed like modelconv. The objpack roots for units that no scenario placement mentions."""
    return models_of(collect(map_path), vehicles)


def referenced_anims(map_path: Path, vehicles: bool = True) -> set[int]:
    """Tag ids of the ``model_animations`` tags of the same units, keyed like animconv."""
    return anims_of(collect(map_path), vehicles)


# ---- section writer ------------------------------------------------------------------------------------
def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def build(us: UnitSet) -> bytes:
    out = bytearray(HEADER_SIZE)
    offs = {}
    for key, recs, size in (("globals", [us.globals], GLOBALS_REC), ("bipeds", us.bipeds, BIPED_REC),
                            ("vehicles", us.vehicles, VEHICLE_REC), ("materials", us.materials, MATERIAL_REC_OUT),
                            ("seats", us.seats, SEAT_REC_OUT)):
        _pad16(out)
        offs[key] = len(out)
        for r in recs:
            blob = r.pack()
            assert len(blob) == size
            out += blob
    _pad16(out)
    player = us.globals.vals.get("default_biped_tag", 0)
    idx = next((i + 1 for i, b in enumerate(us.bipeds) if b.vals["tag_id"] == player), 0) if player else 0
    struct.pack_into("<4sHHIIIII", out, 0, MAGIC, VERSION, 0, len(out), len(us.bipeds), len(us.vehicles),
                     len(us.materials), len(us.seats))
    struct.pack_into("<5I", out, 0x1C, offs["globals"], offs["bipeds"], offs["vehicles"], offs["materials"],
                     offs["seats"])
    struct.pack_into("<2I", out, 0x30, player, idx)
    return bytes(out)


def build_section(map_path: Path) -> bytes:
    return build(collect(map_path))


@dataclass
class Section:
    header: dict
    globals: dict
    bipeds: list[dict]
    vehicles: list[dict]
    seats: list[dict]
    materials: list[dict]


def parse_section(data: bytes) -> Section:
    """Reference parser of the section (the writer's inverse; the C reader is checked against the same bytes)."""
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise UnitError("not a UNIT section")
    ver, _res, size, nb, nv, nm, ns, og, ob, ov, om, os_, player, pidx = struct.unpack_from("<HHIIIIIIIIIIII", data, 4)
    if ver != VERSION or size != len(data):
        raise UnitError("bad version or size")
    for off, n, rec in ((ob, nb, BIPED_REC), (ov, nv, VEHICLE_REC), (om, nm, MATERIAL_REC_OUT), (os_, ns, SEAT_REC_OUT),
                        (og, 1, GLOBALS_REC)):
        if off % 16 or off < HEADER_SIZE or off + n * rec > size:
            raise UnitError("table outside the section")
    hdr = {"biped_count": nb, "vehicle_count": nv, "material_count": nm, "seat_count": ns, "player_biped": player,
           "player_biped_index": pidx}
    return Section(hdr, parse_rec(GLOBALS_L, data, og),
                   [parse_rec(BIPED_L, data, ob + i * BIPED_REC) for i in range(nb)],
                   [parse_rec(VEHICLE_L, data, ov + i * VEHICLE_REC) for i in range(nv)],
                   [parse_rec(SEAT_L, data, os_ + i * SEAT_REC_OUT) for i in range(ns)],
                   [parse_rec(MATERIAL_L, data, om + i * MATERIAL_REC_OUT) for i in range(nm)])


# ---- reporting ------------------------------------------------------------------------------------------
def describe(m: texconv.MapData, us: UnitSet) -> str:
    g = us.globals.vals
    lines = [f"{m.path.stem}: {len(us.bipeds)} bipeds, {len(us.vehicles)} vehicles ({len(us.seats)} seats), "
             f"{len(us.materials)} materials; gravity {g['gravity']:.6f} wu/tick^2 "
             f"({g['gravity'] * TICKS_PER_SECOND ** 2:.4f} wu/s^2)"]
    lines.append("  player: run fwd/back/side {:.3f} {:.3f} {:.3f} wu/s, sneak {:.3f} {:.3f} {:.3f}, walk {:.3f}".format(
        g["run_forward"], g["run_backward"], g["run_sideways"], g["sneak_forward"], g["sneak_backward"],
        g["sneak_sideways"], g["walk_speed"]))
    for b in us.bipeds:
        v = b.vals
        lines.append("  biped {:<34s} r {:.2f} h {:.2f}/{:.2f} cam {:.2f}/{:.2f} jump {:.3f} hp {:g} sh {:g} (rech {:g}s) "
                     "speed {:.2f} {:.2f} {:.2f} {:.2f} wpn {} dlg {} hold {!r}".format(
                         v["name"], v["radius"], v["stand_height"], v["crouch_height"], v["stand_camera"],
                         v["crouch_camera"], v["jump_velocity"], v["health"], v["shield"], v["shield_recharge_time"],
                         *v["anim_speed"], v["weapon_count"], v["dialogue_count"], cstring(v["hold_marker"])
                         if isinstance(v["hold_marker"], bytes) else v["hold_marker"]))
    for v in us.vehicles:
        d = v.vals
        lines.append("  vehicle {:<30s} {:<7s} fwd {:.4f} rev {:.4f} acc {:.5f} seats {}{}".format(
            d["name"], VEHICLE_TYPES[d["type"]], d["max_forward"], d["max_reverse"], d["accel"], d["seat_count"],
            "" if is_driveable(v) else "  (not driveable)"))
    lines += [f"  problem: {p}" for p in us.problems]
    return "\n".join(lines)


def info_pack(path: Path) -> str:
    pk = hp2.parse(Path(path).read_bytes())
    sec = parse_section(pk.read("UNIT"))
    g = sec.globals
    out = [f"{path.name}: UNIT {pk.find('UNIT').raw_size} B: {len(sec.bipeds)} bipeds, {len(sec.vehicles)} vehicles, "
           f"{len(sec.seats)} seats, {len(sec.materials)} materials, gravity {g['gravity']:.6f}"]
    for b in sec.bipeds:
        out.append(f"  biped {cstring(b['name']):<34s} hp {b['health']:g} shield {b['shield']:g} radius {b['radius']:.2f}")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or a .hp2 with --info)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true", help="print what was read instead of writing")
    args = ap.parse_args(argv)
    if args.info and args.input.suffix == ".hp2":
        print(info_pack(args.input))
        return 0
    m = texconv.MapData.open(args.input)
    us = collect_map(m)
    if args.info:
        print(describe(m, us))
        return 0
    if not args.out:
        ap.error("-o is required")
    blob = build(us)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, [("UNIT", blob)]))
    print(f"wrote {args.out} (UNIT {len(blob) / 1024:.1f} KiB: {len(us.bipeds)} bipeds, {len(us.vehicles)} vehicles, "
          f"{len(us.seats)} seats)")
    for p in us.problems:
        print(f"  problem: {p}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
