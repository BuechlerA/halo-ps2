#!/usr/bin/env python3
"""Convert the weapons, projectiles, damage effects, equipment, grenades and item collections of an Xbox map
into the PS2 ``WEAP`` section, version 1 (docs/18-weap-section.md; reader and weapon state machine:
engine/src/sim/weapdef.c, contract engine/src/sim/weapdef.h).

Every layout below was read off the retail maps (all ten campaign maps; a10 in depth) and cross-checked with the
plausibility rules of ``check_*`` (run on every conversion, ``WeapSet.problems`` must stay empty on retail maps:
test_weapconv.py asserts it).  "verified" = a relation in the data confirms it (named); "unverified" = taken
from the public tag documentation or a convention the data does not contradict.  Offsets are inside the tag's own
data block, pointers are absolute tag-space addresses, a reflexive is ``u32 count, u32 address, u32 pad`` and a tag
reference is 16 bytes (``char[4] group`` byte-reversed, ``u32 path pointer``, ``u32 path length``, ``u32 tag id``).

weapon (``weap``, 0x508 B; object type 2).  Sub-blocks follow the struct in field order.
    +0x028 model ref, +0x038 animation graph ref (third person / world; none on the retail weapons except none)
    +0x17C item: u32 flags, +0x180 u16 hud message index, +0x182 u16 sort order, +0x184 f32 scale
    +0x248 material effects ref (``foot``), +0x258 collision sound, +0x2E0 f32[2] detonation delay,
    +0x2E8 detonating effect, +0x2F8 detonation effect
    +0x308 u32 weapon flags: 0x1 vertical heat display, 0x2 mutually exclusive triggers, 0x4 attacks automatically on
           bump, 0x8 must be readied, 0x10 doesn't count toward max, 0x20 aim assist only when zoomed, 0x40 prevents
           grenade throwing, 0x80 must be picked up, 0x100 holds triggers when dropped, 0x200 prevents melee,
           0x400 detonates when dropped, 0x800 cannot fire at maximum age, 0x1000 secondary trigger overrides
           grenades, 0x2000 does not depower active camo, 0x4000 enables night vision, 0x8000 AIs use melee damage
           (verified by weapon: sniper 0x4020 = night vision + aim assist only when zoomed, plasma rifle / pistol 0x800
           = battery weapons, fuel rod 0x400 = detonates when dropped)
    +0x30C char label: a short code ("ar", "pr", "hp", "cannon") followed by stale bytes of an older, longer label;
           only the string up to the first NUL means something.  The "rifle" at +0x320 on a few weapons is stale too.
    +0x32C u16 secondary trigger mode (banshee 2), +0x32E u16 max alternate shots loaded
    +0x33C ready effect ref
    +0x34C f32 heat recovery threshold, +0x350 overheated threshold, +0x354 heat detonation threshold,
    +0x358 heat detonation fraction, +0x35C heat loss per second, +0x360 heat illumination
           (verified: plasma rifle 0.25 / 1 / 1 / 0 / 0.3 / 1, plasma pistol loses 0.65 per second; 0 on every
           weapon that has no heat; overheat is reachable: heat per round x rate > heat loss, see the doc)
    +0x374 overheated effect ref, +0x384 detonation effect ref, +0x394 player melee damage (``jpt!``), +0x3A4 melee response
    +0x3CC f32 near reticle range, +0x3D0 far, +0x3D4 intersection (verified: 4 / 16 / 80 on all vehicle turrets)
    +0x3DA u16 zoom levels, +0x3DC f32 zoom min, +0x3E0 zoom max (verified: pistol 1 level 2x, sniper 2 levels 2x..8x)
    +0x3E4 f32 autoaim angle, +0x3E8 autoaim range, +0x3EC magnetism angle, +0x3F0 magnetism range, +0x3F4 deviation angle
    +0x45C first person model ref, +0x46C first person animation graph ref (verified, docs/13)
    +0x480 weapon HUD interface ref (``wphi``), +0x490 pickup sound, +0x4A0 zoom in sound, +0x4B0 zoom out sound
    +0x4CC f32 active camo ding, +0x4D0 f32 active camo regrowth rate
    +0x4E2 u16 weapon type (0 none, 1 shotgun, 2 needler, 3 plasma pistol, 4 plasma rifle: verified, one per weapon)
    +0x4F0 magazines (0x70 B), +0x4FC triggers (0x114 B)
    Not exported: u16 pairs at +0x330/+0x334 and the f32 at +0x338 (meaning unknown), flashlight values at +0x418.. and
    the vehicle movement penalties at +0x40C (the integrator has no use for them yet).

magazine (0x70 B): +0x00 u32 flags (0x1 wastes rounds when reloaded: flamethrower; 0x2 every round must be chambered:
    scorpion, banshee, shotgun), +0x04 u16 rounds recharged, +0x06 u16 rounds total initial, +0x08 u16 rounds total
    maximum, +0x0A u16 rounds loaded maximum, +0x14 f32 reload time (seconds), +0x18 u16 rounds reloaded (per reload),
    +0x1C f32 chamber time, +0x38 reloading effect ref, +0x48 chambering effect ref, +0x64 equipment reflexive
    (28 B: u16 rounds, +0x0C equipment ref).
    verified: loaded max <= total max wherever there is a magazine; rounds reloaded <= loaded max; the pistol's reload time
    of 2.17 s equals its first person reload animation (65 frames at 30 Hz); the player starting profiles hold
    (loaded, total) pairs such as 6 / 48 for the pistol and 60 / 180 for the assault rifle, so "total" is the
    reserve and does not include the loaded rounds.  A magazine of all zeros is an energy weapon (heat and age
    instead of rounds) or a vehicle gun with unlimited ammunition.

trigger (0x114 B): +0x00 u32 flags (0x1 tracks fired projectiles, 0x2 random firing effects, 0x4 can fire with partial
    ammo, 0x8 does not repeat automatically, 0x10 locks in on/off state, 0x20 projectiles use weapon origin, 0x40 sticks when
    dropped, 0x80 ejects during chamber, 0x100 discharging spews, 0x200 analog rate of fire, 0x400 use error when
    unzoomed, 0x800 projectile vector cannot be adjusted, 0x1000 projectiles have identical error;
    verified: sniper 0x408, rocket launcher / fuel rod 0x8, turrets 0x1020)
    +0x04 f32 rounds per second min, +0x08 max, +0x0C acceleration time, +0x10 deceleration time, +0x14 blurred rate
    (verified: assault rifle 15 / 15, pistol 3.5, warthog chain gun 8..15 over 1 s, plasma rifle 7..10 over 1.8 s)
    +0x20 u16 magazine index (0xFFFF none), +0x22 u16 rounds per shot, +0x24 u16 minimum rounds loaded,
    +0x26 u16 projectiles between tracers (3 on the assault rifle), +0x2E u16 firing noise
    +0x38 f32 error acceleration time, +0x3C error deceleration time (unverified meaning, non-zero exactly on
    weapons whose error angle range is non-degenerate)
    +0x48 f32 charging time, +0x4C charged time, +0x50 u16 overcharged action, +0x54 f32 charged illumination, +0x58 spew time,
    +0x5C charging effect ref (verified: plasma pistol 0.6 s, hunter fuel rod / fuel rod 1.25 s; zero elsewhere)
    +0x6C u16 distribution function (0 point, 1 horizontal fan), +0x6E u16 projectiles per shot (shotgun 15, 1 elsewhere)
    +0x70 f32 distribution angle, +0x78 f32 minimum error, +0x7C f32 error angle min, +0x80 error angle max
    (radians; verified: assault rifle 2.0..6.5 degrees, pistol 0.2..0.4, sniper 0.5, shotgun 10)
    +0x84 f32[3] first person offset (only the rocket launcher has one)
    +0x94 projectile ref (``proj``), +0xA4 f32 ejection port recovery time, +0xA8 illumination recovery time,
    +0xB8 f32 heat generated per round, +0xBC f32 age generated per round (battery; verified plasma rifle 0.08 / 0.005)
    +0x108 firing effects reflexive (0x84 B: u16 shot count lower / upper, +0x24 firing effect ref, +0x34 misfire
    effect, +0x44 empty effect, +0x54 firing damage ref, +0x64 misfire damage, +0x74 empty damage)
    Not exported: six f32 at +0xF0..+0x104 (multiples of 1/30 or 1/90 seconds: meaning unknown).

projectile (``proj``, 0x24C B; object type 5, verified):  +0x028 model ref
    +0x17C u32 flags (0x1 oriented along velocity, 0x2 AI must use ballistic aiming, 0x4 detonation max time if
    attached, 0x8 super combining explosion, ...: verified needler 0xB, plasma grenade 0x6, frag 0x2)
    +0x180 u16 detonation timer starts (0 immediately, 1 after first bounce, 2 when at rest), +0x182 u16 impact noise
    +0x18C super detonation effect, +0x19C f32 AI perception radius, +0x1A0 collision radius, +0x1A4 arming time,
    +0x1A8 danger radius, +0x1AC detonation effect, +0x1BC f32 timer min, +0x1C0 timer max, +0x1C4 minimum velocity,
    +0x1C8 maximum range, +0x1CC air gravity scale, +0x1D0 / +0x1D4 air damage range, +0x1D8 water gravity scale,
    +0x1DC / +0x1E0 water damage range, +0x1E4 initial velocity, +0x1E8 final velocity, +0x1EC guided angular
    velocity, +0x1F0 u16 detonation noise, +0x1F4 detonation started effect, +0x204 flyby sound, +0x214 attached
    detonation damage, +0x224 impact damage, +0x240 material responses (160 B x up to 33, indexed by material type)
    VELOCITIES ARE WORLD UNITS PER TICK (1/30 s): verified, sniper 33.33 = 1000 wu/s and the same 1000 as range,
    pistol 10 (300 wu/s, about 900 m/s at 10 ft per world unit), plasma bolts 1.667 -> 0.833 (50 -> 25 wu/s, they slow
    down), tank shell 3.33, frag grenade 0 (thrown with the globals' speed).  The ranges, radii and the guided
    angular velocity are plain units (world units, radians per second).
    material response (160 B): +0x00 u16 flags (0x1 cannot be overpenetrated), +0x02 u16 response (0 disappear,
    1 detonate, 2 reflect, 3 overpenetrate, 4 attach; verified: 1..4 occur, 0 only for the default fill),
    +0x04 effect ref, +0x24 u16 potential response, +0x26 u16 potential flags, +0x28 f32 skip fraction,
    +0x2C / +0x30 f32 angle min / max (radians, +0x30 = pi on the reflective materials), +0x34 / +0x38 velocity min / max,
    +0x3C potential effect ref, +0x60 f32 angular noise, +0x64 velocity noise, +0x68 detonation effect ref,
    +0x90 f32 initial friction, +0x94 maximum distance, +0x98 parallel friction, +0x9C perpendicular friction.
    The block has 33 entries (30, 31 or 32 on a few projectiles): one per material type, in the globals' order.

damage effect (``jpt!``, 0x2A0 B): +0x00 f32 radius min, +0x04 radius max, +0x08 cutoff scale, +0x0C u32 flags,
    +0x24 u16 screen flash type, +0x26 u16 priority, +0x34 f32 duration, +0x38 u16 fade function, +0x44 f32 maximum
    intensity, +0x4C f32[4] colour (a, r, g, b),
    +0x5C f32 low frequency rumble (frequency, +0x60 duration, +0x64 u16 fade), +0x70 high frequency (same),
    +0x98 f32 camera impulse duration, +0x9C u16 fade, +0xA0 f32 rotation, +0xA4 pushback, +0xA8 / +0xAC jitter,
    +0xB8 f32 permanent impulse angle, +0xCC f32 camera shake duration, +0xD0 u16 falloff, +0xD4 f32 random translation,
    +0xD8 random rotation, +0xDC u16 wobble function, +0xE0 wobble period, +0xE4 wobble weight, +0x114 sound ref,
    +0x194 f32 breaking forward velocity / radius / exponent, +0x1AC outward velocity / radius / exponent,
    +0x1C4 u16 side effect (0 none, 1 harmless, 2 lethal to the unsuspecting, 3 emp), +0x1C6 u16 category (2 bullet,
    3 grenade, 4 high explosive, 5 sniper, 6 melee, 7 flame, 8 mounted weapon, 9 vehicle, 10 plasma, 11 needle,
    12 shotgun: verified by tag name, every assault rifle / pistol bullet is 2, every melee 6, shotgun pellet 12),
    +0x1C8 u32 damage flags (0x1 does not hurt owner, 0x2 can cause headshots, 0x4 pings resistant units, 0x8 does not
    hurt friends, 0x10 does not ping units, 0x20 detonates explosives, 0x40 only hurts shields, 0x80 causes flaming death,
    ...: verified pistol / sniper 0x2, frag 0x20, flame 0x80), +0x1CC f32 area-of-effect core radius, +0x1D0 f32 damage
    lower bound, +0x1D4 / +0x1D8 damage upper bound min / max (verified: assault rifle 10, pistol 25, sniper 101, shotgun
    pellet 8 / 18 / 25, frag grenade 80 / 120 / 120, melee 40 / 50 / 60), +0x1DC vehicle passthrough penalty,
    +0x1E0 active camouflage damage, +0x1E4 stun, +0x1E8 maximum stun, +0x1EC stun time,
    +0x21C f32[33] damage modifiers: one per material type (unverified order; the sniper bullet has 0.05 on the two hunter
    entries, the shock waves have none).  The rest of the 0x2A0 bytes (+0x1F0..+0x21B) is not exported.
    All amounts are in the unit of the biped's health and shield values (docs/18).

equipment (``eqip``, ~0x3D8 B; object type 3): item fields as the weapon, +0x308 u16 powerup type (0 none, 1 double speed,
    2 over shield, 3 active camouflage, 4 full-spectrum vision, 5 health, 6 grenade), +0x30A u16 grenade type (0 frag,
    1 plasma; verified: frag / plasma grenade equipment), +0x30C f32 powerup time (active camo 45 s, over shield 60 s),
    +0x310 pickup sound.  Ammunition pickups are equipment too: the magazine's equipment block names them.
globals (``matg``) +0x128 grenades (68 B x 2, index = grenade type: frag, plasma; verified by path): +0x00 u16 maximum
    count (4), +0x02 u16 (0), +0x04 throwing effect, +0x14 HUD interface (``grhi``), +0x24 equipment ref, +0x34 projectile ref.
item collection (``itmc``): no campaign map has one (verified: zero tags on all ten), the layout (permutations reflexive
    at +0x00, u16 spawn time at +0x0C; permutation 0x54 B: +0x20 f32 weight, +0x24 item ref) is UNVERIFIED and the
    converter refuses to guess when a map does have some (``WeapError``).

Usage:
    python3 -I tools/halo2ps2/weapconv.py assets/decompressed/a10.map -o assets/hp2/a10.weap.hp2
    python3 -I tools/halo2ps2/weapconv.py --info assets/decompressed/a10.map     # table of weapons
    python3 -I tools/halo2ps2/weapconv.py --info assets/hp2/a10.weap.hp2
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
from halo2ps2 import hp2, scenconv, tagindex, texconv  # noqa: E402

MAGIC = b"WEAP"
VERSION = 1
HEADER_SIZE = 128
WEAPON_REC, MAGAZINE_REC, TRIGGER_REC, FX_REC = 256, 64, 128, 32
PROJECTILE_REC, MATRESP_REC, DAMAGE_REC, EQUIPMENT_REC = 192, 64, 384, 96
GRENADE_REC, ITMC_REC, ITMC_ENTRY_REC = 32, 32, 32
MAX_MAGAZINES = MAX_TRIGGERS = 4
MAX_FX = 16
MAX_MATERIALS = 33
NO_MAGAZINE = 0xFFFF
MAX_TAGS = 4096
TICKS = 30.0

# source layouts (docstring)
W_SIZE, M_SIZE, T_SIZE, FX_SIZE, ME_SIZE = 0x508, 0x70, 0x114, 0x84, 28
P_SIZE, MR_SIZE, J_SIZE, E_SIZE, G_SIZE = 0x24C, 160, 0x2A0, 0x308 + 0x20, 68
GLOBALS_GRENADES = 0x128
FP_WEAPONS, FP_LIST, FP_STRIDE = 0x48, 0x10, 0x1C
FP_SLOTS = {"ready": 10, "put_away": 11, "reload_empty": 7, "reload_full": 8, "melee": 13, "overheated": 9,
            "throw": 16, "fire": 2}
ANIMS, ANIM_REC = 0x74, 180

SOUND_GROUPS = ("snd!", "lsnd")
FX_GROUPS = ("effe", "snd!", "lsnd")          # a firing effect slot holds an effect or a sound


class WeapError(ValueError):
    pass


# ---- data model -----------------------------------------------------------------------------------
@dataclass
class Magazine:
    flags: int = 0
    rounds_recharged: int = 0
    rounds_total_initial: int = 0
    rounds_total_max: int = 0
    rounds_loaded_max: int = 0
    rounds_reloaded: int = 0
    reload_time: float = 0.0
    chamber_time: float = 0.0
    reload_effect: int = 0
    chamber_effect: int = 0
    equipment: list[tuple[int, int]] = field(default_factory=list)      # (equipment tag id, rounds)


@dataclass
class FireEffect:
    shots_lo: int = 0
    shots_hi: int = 0
    effects: tuple[int, int, int] = (0, 0, 0)          # firing, misfire, empty effect tags (effect or sound)
    sound_mask: int = 0                                # bit i set: effects[i] is a sound tag, not an effect
    damages: tuple[int, int, int] = (0, 0, 0)          # firing, misfire, empty damage effect tags


@dataclass
class Trigger:
    flags: int = 0
    rps_min: float = 0.0
    rps_max: float = 0.0
    accel_time: float = 0.0
    decel_time: float = 0.0
    blurred_rate: float = 0.0
    magazine: int = NO_MAGAZINE
    rounds_per_shot: int = 0
    min_rounds_loaded: int = 0
    tracer_interval: int = 0
    firing_noise: int = 0
    projectiles_per_shot: int = 1
    distribution_function: int = 0
    overcharged_action: int = 0
    error_accel_time: float = 0.0
    error_decel_time: float = 0.0
    minimum_error: float = 0.0
    error_min: float = 0.0
    error_max: float = 0.0
    distribution_angle: float = 0.0
    fp_offset: tuple[float, float, float] = (0.0, 0.0, 0.0)
    charging_time: float = 0.0
    charged_time: float = 0.0
    charged_illumination: float = 0.0
    spew_time: float = 0.0
    ejection_recovery: float = 0.0
    illumination_recovery: float = 0.0
    heat_per_round: float = 0.0
    age_per_round: float = 0.0
    projectile: int = 0
    charging_effect: int = 0
    fx: list[FireEffect] = field(default_factory=list)


@dataclass
class Weapon:
    tag_id: int
    name: str
    model_tag: int = 0
    anim_tag: int = 0
    fp_model_tag: int = 0
    fp_anim_tag: int = 0
    hud_tag: int = 0
    pickup_sound: int = 0
    zoom_in_sound: int = 0
    zoom_out_sound: int = 0
    collision_sound: int = 0
    ready_effect: int = 0
    overheated_effect: int = 0
    detonation_effect: int = 0
    melee_damage: int = 0
    melee_response: int = 0
    flags: int = 0
    item_flags: int = 0
    secondary_mode: int = 0
    max_alternate_shots: int = 0
    weapon_type: int = 0
    zoom_levels: int = 0
    zoom_min: float = 0.0
    zoom_max: float = 0.0
    heat_recovery: float = 0.0
    overheated: float = 0.0
    heat_detonation: float = 0.0
    heat_detonation_fraction: float = 0.0
    heat_loss: float = 0.0
    heat_illumination: float = 0.0
    near_reticle: float = 0.0
    far_reticle: float = 0.0
    intersection_reticle: float = 0.0
    autoaim_angle: float = 0.0
    autoaim_range: float = 0.0
    magnetism_angle: float = 0.0
    magnetism_range: float = 0.0
    deviation_angle: float = 0.0
    camo_ding: float = 0.0
    camo_regrowth: float = 0.0
    item_scale: float = 0.0
    detonation_delay: tuple[float, float] = (0.0, 0.0)
    hud_message: int = 0
    sort_order: int = 0
    fp_ticks: dict[str, int] = field(default_factory=dict)
    label: str = ""
    magazines: list[Magazine] = field(default_factory=list)
    triggers: list[Trigger] = field(default_factory=list)


@dataclass
class MaterialResponse:
    flags: int = 0
    response: int = 0
    effect: int = 0
    potential_response: int = 0
    potential_flags: int = 0
    skip_fraction: float = 0.0
    angle: tuple[float, float] = (0.0, 0.0)
    velocity: tuple[float, float] = (0.0, 0.0)
    potential_effect: int = 0
    angular_noise: float = 0.0
    velocity_noise: float = 0.0
    detonation_effect: int = 0
    initial_friction: float = 0.0
    max_distance: float = 0.0
    parallel_friction: float = 0.0
    perpendicular_friction: float = 0.0


@dataclass
class Projectile:
    tag_id: int
    name: str
    model_tag: int = 0
    flags: int = 0
    timer_starts: int = 0
    impact_noise: int = 0
    ai_perception_radius: float = 0.0
    collision_radius: float = 0.0
    arming_time: float = 0.0
    danger_radius: float = 0.0
    timer: tuple[float, float] = (0.0, 0.0)
    min_velocity: float = 0.0               # world units per tick
    max_range: float = 0.0
    air_gravity: float = 0.0
    air_range: tuple[float, float] = (0.0, 0.0)
    water_gravity: float = 0.0
    water_range: tuple[float, float] = (0.0, 0.0)
    initial_velocity: float = 0.0           # world units per tick
    final_velocity: float = 0.0
    guided_angular_velocity: float = 0.0
    detonation_noise: int = 0
    super_detonation: int = 0
    detonation_effect: int = 0
    detonation_started: int = 0
    flyby_sound: int = 0
    attached_damage: int = 0
    impact_damage: int = 0
    object_radius: float = 0.0
    materials: list[MaterialResponse] = field(default_factory=list)


@dataclass
class Damage:
    tag_id: int
    name: str
    radius: tuple[float, float] = (0.0, 0.0)
    cutoff_scale: float = 0.0
    effect_flags: int = 0
    flash_type: int = 0
    flash_priority: int = 0
    flash_duration: float = 0.0
    flash_fade: int = 0
    flash_intensity: float = 0.0
    flash_color: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    low_rumble: tuple[float, float, int] = (0.0, 0.0, 0)       # frequency, duration, fade function
    high_rumble: tuple[float, float, int] = (0.0, 0.0, 0)
    impulse_duration: float = 0.0
    impulse_fade: int = 0
    impulse_rotation: float = 0.0
    impulse_pushback: float = 0.0
    impulse_jitter: tuple[float, float] = (0.0, 0.0)
    permanent_angle: float = 0.0
    shake_duration: float = 0.0
    shake_falloff: int = 0
    shake_translation: float = 0.0
    shake_rotation: float = 0.0
    shake_wobble_function: int = 0
    shake_wobble_period: float = 0.0
    shake_wobble_weight: float = 0.0
    sound: int = 0
    break_forward: tuple[float, float, float] = (0.0, 0.0, 0.0)
    break_outward: tuple[float, float, float] = (0.0, 0.0, 0.0)
    side_effect: int = 0
    category: int = 0
    damage_flags: int = 0
    aoe_core_radius: float = 0.0
    damage_lower: float = 0.0
    damage_upper: tuple[float, float] = (0.0, 0.0)
    vehicle_passthrough: float = 0.0
    active_camo: float = 0.0
    stun: float = 0.0
    max_stun: float = 0.0
    stun_time: float = 0.0
    modifiers: tuple[float, ...] = (0.0,) * MAX_MATERIALS


@dataclass
class Equipment:
    tag_id: int
    name: str
    model_tag: int = 0
    anim_tag: int = 0
    item_flags: int = 0
    powerup_type: int = 0
    grenade_type: int = 0
    powerup_time: float = 0.0
    pickup_sound: int = 0
    collision_sound: int = 0
    detonating_effect: int = 0
    detonation_effect: int = 0
    detonation_delay: tuple[float, float] = (0.0, 0.0)
    item_scale: float = 0.0
    hud_message: int = 0


@dataclass
class Grenade:
    max_count: int = 0
    mp_spawn_default: int = 0
    throw_effect: int = 0
    hud_tag: int = 0
    equipment: int = 0
    projectile: int = 0


@dataclass
class ItemCollection:
    tag_id: int
    spawn_time: int = 0
    entries: list[tuple[float, int]] = field(default_factory=list)       # (weight, item tag id)


@dataclass
class WeapSet:
    weapons: list[Weapon] = field(default_factory=list)
    projectiles: list[Projectile] = field(default_factory=list)
    damages: list[Damage] = field(default_factory=list)
    equipment: list[Equipment] = field(default_factory=list)
    grenades: list[Grenade] = field(default_factory=list)
    collections: list[ItemCollection] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)


# ---- tag access -----------------------------------------------------------------------------------
def _tail(path: str) -> str:
    return path.rsplit("\\", 1)[-1][:31]


class Reader:
    """Bounds-checked field reads of one tag block, with tag reference resolution."""

    def __init__(self, r: scenconv.Region, base: int, what: str, problems: list[str]):
        self.r, self.b, self.what, self.problems = r, base, what, problems

    def u(self, fmt: str, off: int):
        try:
            return self.r.u(fmt, self.b + off)
        except scenconv.ScenError as ex:
            raise WeapError(f"{self.what}: {ex}") from None

    def u16(self, off: int) -> int:
        return self.u("<H", off)[0]

    def u32(self, off: int) -> int:
        return self.u("<I", off)[0]

    def f(self, off: int) -> float:
        v = self.u("<f", off)[0]
        if not math.isfinite(v):
            raise WeapError(f"{self.what}: non-finite float at {off:#x}")
        return v

    def fs(self, off: int, n: int) -> tuple[float, ...]:
        return tuple(self.f(off + 4 * i) for i in range(n))

    def ref(self, off: int, groups: str | tuple[str, ...]) -> int:
        """Tag id of the reference at ``off`` (0 when null or not resolvable, which is recorded)."""
        if isinstance(groups, str):
            groups = (groups,)
        try:
            g, tid = self.r.tagref(self.b + off)
        except scenconv.ScenError as ex:
            raise WeapError(f"{self.what}: {ex}") from None
        if g is None or tid in (0, 0xFFFFFFFF):          # real null references carry 0xFFFFFFFF, zeroed ones 0
            return 0
        e = self.r.entry(tid)
        if g not in groups or e is None or e.group != g:
            self.problems.append(f"{self.what}: reference at {off:#x} ({g!r}, {tid:#x}) is not a {groups} tag")
            return 0
        return tid

    def reflexive(self, off: int, stride: int, limit: int) -> tuple[int, int]:
        try:
            n, o = self.r.reflexive(self.b + off, stride, limit)
        except scenconv.ScenError as ex:
            raise WeapError(f"{self.what}: {ex}") from None
        return n, o - self.b


def r_group(rd: "Reader", tid: int) -> str | None:
    e = rd.r.entry(tid)
    return e.group if e else None


def _sub(parent: Reader, off: int, what: str) -> Reader:
    return Reader(parent.r, parent.b + off, f"{parent.what} {what}", parent.problems)


def label_string(raw: bytes) -> str:
    s = raw.split(b"\0", 1)[0]
    return "".join(c if 32 <= ord(c) < 127 else "?" for c in s.decode("latin1"))[:15]


# ---- first person animation timing ----------------------------------------------------------------
def fp_ticks(r: scenconv.Region, graph_tag: int, problems: list[str], what: str) -> dict[str, int]:
    """Frame counts (= ticks, animations run at 30 Hz) of the first person animations a weapon uses:
    the graph's first-person weapon block lists animation indices in the order idle, posing, fire 1, moving,
    overlays, light off, light on, reload empty, reload full, overheated, ready, put away, overcharged, melee,
    fire 2, overcharged melee, throw grenade, ...  (verified by animation name on every weapon of the ten maps:
    the name of each used slot contains the slot's word)."""
    out: dict[str, int] = {}
    e = r.entry(graph_tag)
    if e is None:
        return out
    gb = r.block(e)
    try:
        n, o = r.reflexive(gb + ANIMS, ANIM_REC, 4096)
        nf, of = r.reflexive(gb + FP_WEAPONS, FP_STRIDE, 64)
        if not nf:
            return out
        cnt, addr = r.u("<II", of + FP_LIST)
        if cnt > 64 or addr - r.base < 0 or addr - r.base + 2 * cnt > len(r.data):
            raise WeapError(f"{what}: first person animation list outside the tag region")
        ids = r.u(f"<{cnt}H", addr - r.base) if cnt else ()
    except scenconv.ScenError as ex:
        raise WeapError(f"{what}: {ex}") from None
    words = {"ready": "ready", "put_away": "put-away", "reload_empty": ("reload", "overheat"),
             "reload_full": ("reload", "overheat"),
             "melee": "melee", "overheated": "overheated", "throw": "throw", "fire": "fire"}
    for key, slot in FP_SLOTS.items():
        if slot >= cnt or ids[slot] >= n:
            continue
        i = ids[slot]
        name = r.cstr(o + i * ANIM_REC, 32).lower()
        frames = r.u("<H", o + i * ANIM_REC + 0x22)[0]
        w = words[key] if isinstance(words[key], tuple) else (words[key],)
        if not any(x in name for x in w):
            problems.append(f"{what}: first person slot {slot} ({key}) is animation {name!r}")
            continue
        out[key] = min(frames, 0xFFFF)
    return out


# ---- readers per tag group ------------------------------------------------------------------------
def read_magazine(w: Reader, i: int, ofs: int, problems: list[str]) -> Magazine:
    m = _sub(w, ofs, f"magazine {i}")
    g = Magazine(flags=m.u32(0), rounds_recharged=m.u16(4), rounds_total_initial=m.u16(6),
                 rounds_total_max=m.u16(8), rounds_loaded_max=m.u16(0xA), reload_time=m.f(0x14),
                 rounds_reloaded=m.u16(0x18), chamber_time=m.f(0x1C),
                 reload_effect=m.ref(0x38, "effe"), chamber_effect=m.ref(0x48, "effe"))
    n, o = m.reflexive(0x64, ME_SIZE, 64)
    for k in range(n):
        q = _sub(m, o + k * ME_SIZE, f"equipment {k}")
        tid = q.ref(0x0C, "eqip")
        if tid:
            g.equipment.append((tid, q.u16(0)))
    return g


def read_trigger(w: Reader, i: int, ofs: int, problems: list[str]) -> Trigger:
    t = _sub(w, ofs, f"trigger {i}")
    g = Trigger(flags=t.u32(0), rps_min=t.f(4), rps_max=t.f(8), accel_time=t.f(0xC), decel_time=t.f(0x10),
                blurred_rate=t.f(0x14), magazine=t.u16(0x20), rounds_per_shot=t.u16(0x22),
                min_rounds_loaded=t.u16(0x24), tracer_interval=t.u16(0x26), firing_noise=t.u16(0x2E),
                projectiles_per_shot=t.u16(0x6E), distribution_function=t.u16(0x6C), overcharged_action=t.u16(0x50),
                error_accel_time=t.f(0x38), error_decel_time=t.f(0x3C), minimum_error=t.f(0x78),
                error_min=t.f(0x7C), error_max=t.f(0x80), distribution_angle=t.f(0x70), fp_offset=t.fs(0x84, 3),
                charging_time=t.f(0x48), charged_time=t.f(0x4C), charged_illumination=t.f(0x54),
                spew_time=t.f(0x58), ejection_recovery=t.f(0xA4), illumination_recovery=t.f(0xA8),
                heat_per_round=t.f(0xB8), age_per_round=t.f(0xBC), projectile=t.ref(0x94, "proj"),
                charging_effect=t.ref(0x5C, "effe"))
    n, o = t.reflexive(0x108, FX_SIZE, 256)
    if n > MAX_FX:
        raise WeapError(f"{t.what}: {n} firing effects")
    for k in range(n):
        f = _sub(t, o + k * FX_SIZE, f"firing effect {k}")
        effects = tuple(f.ref(off, FX_GROUPS) for off in (0x24, 0x34, 0x44))
        mask = sum(1 << j for j, tid in enumerate(effects) if tid and r_group(f, tid) != "effe")
        g.fx.append(FireEffect(f.u16(0), f.u16(2), effects, mask,
                               (f.ref(0x54, "jpt!"), f.ref(0x64, "jpt!"), f.ref(0x74, "jpt!"))))
    return g


def read_weapon(r: scenconv.Region, e: tagindex.TagEntry, problems: list[str]) -> Weapon:
    w = Reader(r, r.block(e), e.path, problems)
    if w.u16(0) != 2:
        problems.append(f"{e.path}: object type {w.u16(0)}, expected 2 (weapon)")
    g = Weapon(e.tag_id, _tail(e.path))
    g.model_tag, g.anim_tag = w.ref(0x28, "mode"), w.ref(0x38, "antr")
    g.fp_model_tag, g.fp_anim_tag = w.ref(0x45C, "mode"), w.ref(0x46C, "antr")
    g.hud_tag = w.ref(0x480, "wphi")
    g.pickup_sound, g.zoom_in_sound, g.zoom_out_sound = (w.ref(0x490, SOUND_GROUPS), w.ref(0x4A0, SOUND_GROUPS),
                                                         w.ref(0x4B0, SOUND_GROUPS))
    g.collision_sound = w.ref(0x258, SOUND_GROUPS)
    g.ready_effect, g.overheated_effect, g.detonation_effect = w.ref(0x33C, "effe"), w.ref(0x374, "effe"), w.ref(0x384, "effe")
    g.melee_damage, g.melee_response = w.ref(0x394, "jpt!"), w.ref(0x3A4, "jpt!")
    g.flags, g.item_flags = w.u32(0x308), w.u32(0x17C)
    g.secondary_mode, g.max_alternate_shots = w.u16(0x32C), w.u16(0x32E)
    g.weapon_type, g.zoom_levels = w.u16(0x4E2), w.u16(0x3DA)
    g.zoom_min, g.zoom_max = w.f(0x3DC), w.f(0x3E0)
    (g.heat_recovery, g.overheated, g.heat_detonation, g.heat_detonation_fraction, g.heat_loss,
     g.heat_illumination) = w.fs(0x34C, 6)
    g.near_reticle, g.far_reticle, g.intersection_reticle = w.fs(0x3CC, 3)
    (g.autoaim_angle, g.autoaim_range, g.magnetism_angle, g.magnetism_range, g.deviation_angle) = w.fs(0x3E4, 5)
    g.camo_ding, g.camo_regrowth = w.fs(0x4CC, 2)
    g.item_scale = w.f(0x184)
    g.detonation_delay = w.fs(0x2E0, 2)
    g.hud_message, g.sort_order = w.u16(0x180), w.u16(0x182)
    g.label = label_string(r.data[w.b + 0x30C:w.b + 0x31C])
    nm, om = w.reflexive(0x4F0, M_SIZE, 64)
    nt, ot = w.reflexive(0x4FC, T_SIZE, 64)
    if nm > MAX_MAGAZINES or nt > MAX_TRIGGERS:
        raise WeapError(f"{e.path}: {nm} magazines, {nt} triggers")
    g.magazines = [read_magazine(w, i, om + i * M_SIZE, problems) for i in range(nm)]
    g.triggers = [read_trigger(w, i, ot + i * T_SIZE, problems) for i in range(nt)]
    if g.fp_anim_tag:
        g.fp_ticks = fp_ticks(r, g.fp_anim_tag, problems, e.path)
    return g


def read_material(m: Reader) -> MaterialResponse:
    return MaterialResponse(
        flags=m.u16(0), response=m.u16(2), effect=m.ref(4, "effe"), potential_response=m.u16(0x24),
        potential_flags=m.u16(0x26), skip_fraction=m.f(0x28), angle=m.fs(0x2C, 2), velocity=m.fs(0x34, 2),
        potential_effect=m.ref(0x3C, "effe"), angular_noise=m.f(0x60), velocity_noise=m.f(0x64),
        detonation_effect=m.ref(0x68, "effe"), initial_friction=m.f(0x90), max_distance=m.f(0x94),
        parallel_friction=m.f(0x98), perpendicular_friction=m.f(0x9C))


def read_projectile(r: scenconv.Region, e: tagindex.TagEntry, problems: list[str]) -> Projectile:
    p = Reader(r, r.block(e), e.path, problems)
    if p.u16(0) != 5:
        problems.append(f"{e.path}: object type {p.u16(0)}, expected 5 (projectile)")
    g = Projectile(e.tag_id, _tail(e.path))
    g.model_tag = p.ref(0x28, "mode")
    g.object_radius = p.f(4)
    g.flags, g.timer_starts, g.impact_noise = p.u32(0x17C), p.u16(0x180), p.u16(0x182)
    g.super_detonation = p.ref(0x18C, "effe")
    g.ai_perception_radius, g.collision_radius, g.arming_time, g.danger_radius = p.fs(0x19C, 4)
    g.detonation_effect = p.ref(0x1AC, "effe")
    g.timer = p.fs(0x1BC, 2)
    g.min_velocity, g.max_range, g.air_gravity = p.fs(0x1C4, 3)
    g.air_range = p.fs(0x1D0, 2)
    g.water_gravity = p.f(0x1D8)
    g.water_range = p.fs(0x1DC, 2)
    g.initial_velocity, g.final_velocity, g.guided_angular_velocity = p.fs(0x1E4, 3)
    g.detonation_noise = p.u16(0x1F0)
    g.detonation_started = p.ref(0x1F4, "effe")
    g.flyby_sound = p.ref(0x204, SOUND_GROUPS)
    g.attached_damage, g.impact_damage = p.ref(0x214, "jpt!"), p.ref(0x224, "jpt!")
    n, o = p.reflexive(0x240, MR_SIZE, 64)
    if n > MAX_MATERIALS:
        raise WeapError(f"{e.path}: {n} material responses")
    g.materials = [read_material(_sub(p, o + i * MR_SIZE, f"material {i}")) for i in range(n)]
    return g


def read_damage(r: scenconv.Region, e: tagindex.TagEntry, problems: list[str]) -> Damage:
    j = Reader(r, r.block(e), e.path, problems)
    g = Damage(e.tag_id, _tail(e.path))
    g.radius, g.cutoff_scale, g.effect_flags = j.fs(0, 2), j.f(8), j.u32(0xC)
    g.flash_type, g.flash_priority, g.flash_duration = j.u16(0x24), j.u16(0x26), j.f(0x34)
    g.flash_fade, g.flash_intensity, g.flash_color = j.u16(0x38), j.f(0x44), j.fs(0x4C, 4)
    g.low_rumble = (j.f(0x5C), j.f(0x60), j.u16(0x64))
    g.high_rumble = (j.f(0x70), j.f(0x74), j.u16(0x78))
    g.impulse_duration, g.impulse_fade = j.f(0x98), j.u16(0x9C)
    g.impulse_rotation, g.impulse_pushback, g.impulse_jitter = j.f(0xA0), j.f(0xA4), j.fs(0xA8, 2)
    g.permanent_angle = j.f(0xB8)
    g.shake_duration, g.shake_falloff, g.shake_translation, g.shake_rotation = j.f(0xCC), j.u16(0xD0), j.f(0xD4), j.f(0xD8)
    g.shake_wobble_function, g.shake_wobble_period, g.shake_wobble_weight = j.u16(0xDC), j.f(0xE0), j.f(0xE4)
    g.sound = j.ref(0x114, SOUND_GROUPS)
    g.break_forward, g.break_outward = j.fs(0x194, 3), j.fs(0x1AC, 3)
    g.side_effect, g.category, g.damage_flags = j.u16(0x1C4), j.u16(0x1C6), j.u32(0x1C8)
    g.aoe_core_radius, g.damage_lower, g.damage_upper = j.f(0x1CC), j.f(0x1D0), j.fs(0x1D4, 2)
    (g.vehicle_passthrough, g.active_camo, g.stun, g.max_stun, g.stun_time) = j.fs(0x1DC, 5)
    g.modifiers = j.fs(0x21C, MAX_MATERIALS)
    return g


def read_equipment(r: scenconv.Region, e: tagindex.TagEntry, problems: list[str]) -> Equipment:
    q = Reader(r, r.block(e), e.path, problems)
    if q.u16(0) != 3:
        problems.append(f"{e.path}: object type {q.u16(0)}, expected 3 (equipment)")
    return Equipment(
        e.tag_id, _tail(e.path), model_tag=q.ref(0x28, "mode"), anim_tag=q.ref(0x38, "antr"),
        item_flags=q.u32(0x17C), powerup_type=q.u16(0x308), grenade_type=q.u16(0x30A), powerup_time=q.f(0x30C),
        pickup_sound=q.ref(0x310, SOUND_GROUPS), collision_sound=q.ref(0x258, SOUND_GROUPS),
        detonating_effect=q.ref(0x2E8, "effe"), detonation_effect=q.ref(0x2F8, "effe"),
        detonation_delay=q.fs(0x2E0, 2), item_scale=q.f(0x184), hud_message=q.u16(0x180))


def read_grenades(r: scenconv.Region, glob: tagindex.TagEntry, problems: list[str]) -> list[Grenade]:
    q = Reader(r, r.block(glob), glob.path, problems)
    n, o = q.reflexive(GLOBALS_GRENADES, G_SIZE, 16)
    out = []
    for i in range(n):
        g = _sub(q, o + i * G_SIZE, f"grenade {i}")
        out.append(Grenade(g.u16(0), g.u16(2), g.ref(4, "effe"), g.ref(0x14, "grhi"), g.ref(0x24, "eqip"),
                           g.ref(0x34, "proj")))
    return out


def read_collection(r: scenconv.Region, e: tagindex.TagEntry, problems: list[str]) -> ItemCollection:
    """UNVERIFIED layout (no campaign map has an item collection): permutations reflexive at +0x00 (0x54 B each:
    +0x20 f32 weight, +0x24 item reference), u16 spawn time at +0x0C.  Every item reference must resolve to an
    item group, otherwise the guess is wrong and we refuse to convert."""
    q = Reader(r, r.block(e), e.path, problems)
    n, o = q.reflexive(0, 0x54, 256)
    c = ItemCollection(e.tag_id, q.u16(0xC))
    for i in range(n):
        p = _sub(q, o + i * 0x54, f"permutation {i}")
        g, tid = r.tagref(p.b + 0x24)
        if g not in ("weap", "eqip", "garb") or r.entry(tid) is None:
            raise WeapError(f"{e.path}: permutation {i} does not reference an item; the item_collection layout "
                            f"is unverified, refusing to convert")
        c.entries.append((p.f(0x20), tid))
    return c


# ---- plausibility ---------------------------------------------------------------------------------
def check_set(s: WeapSet) -> None:
    """Relations every retail weapon must satisfy; violations are recorded in ``s.problems``."""
    P = s.problems.append
    for w in s.weapons:
        for i, m in enumerate(w.magazines):
            tag = f"{w.name} magazine {i}"
            if m.rounds_total_max and m.rounds_loaded_max > m.rounds_total_max:
                P(f"{tag}: loaded max {m.rounds_loaded_max} above total max {m.rounds_total_max}")
            if m.rounds_total_max and m.rounds_total_initial > m.rounds_total_max:
                P(f"{tag}: initial {m.rounds_total_initial} above total max {m.rounds_total_max}")
            if m.rounds_loaded_max and not 0 < m.rounds_reloaded <= m.rounds_loaded_max:
                P(f"{tag}: rounds reloaded {m.rounds_reloaded} with loaded max {m.rounds_loaded_max}")
            if not 0.0 <= m.reload_time <= 10.0 or not 0.0 <= m.chamber_time <= 10.0:
                P(f"{tag}: reload / chamber time {m.reload_time} / {m.chamber_time}")
        for i, t in enumerate(w.triggers):
            tag = f"{w.name} trigger {i}"
            if not 0.0 <= t.rps_min <= t.rps_max <= 30.0:
                P(f"{tag}: rounds per second {t.rps_min} .. {t.rps_max}")
            if t.magazine != NO_MAGAZINE and t.magazine >= len(w.magazines):
                P(f"{tag}: magazine {t.magazine} of {len(w.magazines)}")
            if t.magazine != NO_MAGAZINE and t.magazine < len(w.magazines):
                m = w.magazines[t.magazine]
                if m.rounds_loaded_max and t.rounds_per_shot > m.rounds_loaded_max:
                    P(f"{tag}: {t.rounds_per_shot} rounds per shot with {m.rounds_loaded_max} loaded")
            if not 0 <= t.projectiles_per_shot <= 32:
                P(f"{tag}: {t.projectiles_per_shot} projectiles per shot")
            if not (0.0 <= t.error_min <= t.error_max <= math.pi / 2 and 0.0 <= t.minimum_error <= math.pi / 2):
                P(f"{tag}: error angles {t.minimum_error} / {t.error_min} .. {t.error_max}")
            if not 0.0 <= t.charging_time <= 10.0 or not 0.0 <= t.heat_per_round <= 1.0 or not 0.0 <= t.age_per_round <= 1.0:
                P(f"{tag}: charge / heat / age {t.charging_time} / {t.heat_per_round} / {t.age_per_round}")
            if t.rps_max > 0 and not t.projectile and not t.fx:
                P(f"{tag}: fires without a projectile or an effect")
            for f in t.fx:
                if f.shots_lo > f.shots_hi:
                    P(f"{tag}: firing effect shot counts {f.shots_lo} .. {f.shots_hi}")
        if not 0 <= w.zoom_levels <= 3 or w.zoom_min > w.zoom_max:
            P(f"{w.name}: zoom {w.zoom_levels} levels {w.zoom_min} .. {w.zoom_max}")
        if not 0.0 <= w.heat_recovery <= w.overheated <= 4.0 and w.overheated:
            P(f"{w.name}: heat thresholds {w.heat_recovery} / {w.overheated}")
        if w.overheated > 0 and w.heat_loss <= 0:
            P(f"{w.name}: overheats at {w.overheated} but never cools down")
        if not 0.0 <= w.autoaim_angle <= math.pi / 2 or not 0.0 <= w.autoaim_range <= 200.0:
            P(f"{w.name}: autoaim {w.autoaim_angle} / {w.autoaim_range}")
    for p in s.projectiles:
        tag = f"projectile {p.name}"
        for v, n in ((p.initial_velocity, "initial"), (p.final_velocity, "final"), (p.min_velocity, "minimum")):
            if not 0.0 <= v * TICKS <= 1500.0:
                P(f"{tag}: {n} velocity {v * TICKS} wu/s")
        if not 0.0 <= p.max_range <= 5000.0 or not 0.0 <= p.air_gravity <= 8.0 or not 0.0 <= p.water_gravity <= 8.0:
            P(f"{tag}: range / gravity {p.max_range} / {p.air_gravity} / {p.water_gravity}")
        if not p.timer[0] <= p.timer[1] or not p.air_range[0] <= p.air_range[1] or not p.water_range[0] <= p.water_range[1]:
            P(f"{tag}: unordered bounds {p.timer} {p.air_range} {p.water_range}")
        for i, m in enumerate(p.materials):
            if m.response > 4 or m.potential_response > 4 or not 0.0 <= m.angle[0] <= m.angle[1] <= 2 * math.pi:
                P(f"{tag}: material {i} response {m.response} / {m.potential_response} angle {m.angle}")
    for d in s.damages:
        tag = f"damage {d.name}"
        if not 0.0 <= d.radius[0] <= d.radius[1] <= 200.0:
            P(f"{tag}: radius {d.radius}")
        if d.damage_lower < 0 or not d.damage_lower <= d.damage_upper[0] <= d.damage_upper[1]:
            P(f"{tag}: damage {d.damage_lower} / {d.damage_upper}")
        if d.category > 12 or d.side_effect > 3:
            P(f"{tag}: category {d.category} side effect {d.side_effect}")
        if not all(0.0 <= v <= 16.0 for v in d.modifiers):
            P(f"{tag}: damage modifier out of range")
    for q in s.equipment:
        if q.powerup_type > 6 or q.grenade_type > 1 or not 0.0 <= q.powerup_time <= 600.0:
            P(f"equipment {q.name}: powerup {q.powerup_type} grenade {q.grenade_type} time {q.powerup_time}")


# ---- collecting -----------------------------------------------------------------------------------
def collect_map(m: texconv.MapData) -> WeapSet:
    r = scenconv.Region(m)
    s = WeapSet()
    for e in m.index.entries:
        if e.group == "weap":
            s.weapons.append(read_weapon(r, e, s.problems))
        elif e.group == "proj":
            s.projectiles.append(read_projectile(r, e, s.problems))
        elif e.group == "jpt!":
            s.damages.append(read_damage(r, e, s.problems))
        elif e.group == "eqip":
            s.equipment.append(read_equipment(r, e, s.problems))
        elif e.group == "itmc":
            s.collections.append(read_collection(r, e, s.problems))
    glob = [e for e in m.index.entries if e.group == "matg"]
    if glob:
        s.grenades = read_grenades(r, glob[0], s.problems)
    for lst in (s.weapons, s.projectiles, s.damages, s.equipment, s.collections):
        lst.sort(key=lambda x: x.tag_id)
    # references that must resolve inside the set
    wt, pt, dt = ({x.tag_id for x in lst} for lst in (s.weapons, s.projectiles, s.damages))
    et = {x.tag_id for x in s.equipment}
    for w in s.weapons:
        for ref in (w.melee_damage, w.melee_response):
            if ref and ref not in dt:
                s.problems.append(f"{w.name}: melee damage {ref:#x} is not a damage effect of the map")
        for t in w.triggers:
            if t.projectile and t.projectile not in pt:
                s.problems.append(f"{w.name}: projectile {t.projectile:#x} missing")
            for f in t.fx:
                for ref in f.damages:
                    if ref and ref not in dt:
                        s.problems.append(f"{w.name}: firing damage {ref:#x} missing")
        for mg in w.magazines:
            for tid, _ in mg.equipment:
                if tid not in et:
                    s.problems.append(f"{w.name}: magazine equipment {tid:#x} missing")
    for p in s.projectiles:
        for ref in (p.attached_damage, p.impact_damage):
            if ref and ref not in dt:
                s.problems.append(f"projectile {p.name}: damage {ref:#x} missing")
    for g in s.grenades:
        if g.equipment not in et or g.projectile not in pt:
            s.problems.append("grenade: equipment or projectile missing")
    check_set(s)
    return s


def collect(map_path: Path) -> WeapSet:
    return collect_map(texconv.MapData.open(map_path))


def referenced_models(map_path: Path) -> set[int]:
    """Tag ids of the ``model`` tags the weapons, projectiles and equipment of the map use (world and first person
    models), keyed like modelconv: the objpack roots for everything that is not placed by the scenario."""
    m = texconv.MapData.open(map_path)
    s = collect_map(m)
    out = set()
    for w in s.weapons:
        out |= {w.model_tag, w.fp_model_tag}
    for p in s.projectiles:
        out.add(p.model_tag)
    for q in s.equipment:
        out.add(q.model_tag)
    out.discard(0)
    return out


def referenced_anims(map_path: Path) -> set[int]:
    """Tag ids of the ``model_animations`` graphs the weapons (first person and world), projectiles and equipment use."""
    m = texconv.MapData.open(map_path)
    s = collect_map(m)
    out = set()
    for w in s.weapons:
        out |= {w.anim_tag, w.fp_anim_tag}
    for q in s.equipment:
        out.add(q.anim_tag)
    out.discard(0)
    # projectiles carry no graph of their own (verified: the animation reference is null on all of them)
    return out


# ---- section writer -------------------------------------------------------------------------------
def _pad16(n: int) -> int:
    return (n + 15) & ~15


def _cstr(s: str, n: int) -> bytes:
    return s.encode("latin1", "replace")[:n - 1].ljust(n, b"\0")


def pack_weapon(w: Weapon, first_mag: int, first_trig: int) -> bytes:
    b = bytearray(WEAPON_REC)
    struct.pack_into("<17I", b, 0, w.tag_id, w.model_tag, w.anim_tag, w.fp_model_tag, w.fp_anim_tag, w.hud_tag,
                     w.pickup_sound, w.zoom_in_sound, w.zoom_out_sound, w.collision_sound, w.ready_effect,
                     w.overheated_effect, w.detonation_effect, w.melee_damage, w.melee_response, 0, w.flags)
    struct.pack_into("<I4H", b, 0x44, w.item_flags, w.secondary_mode, w.max_alternate_shots, w.weapon_type, w.zoom_levels)
    struct.pack_into("<21f", b, 0x50, w.zoom_min, w.zoom_max, w.heat_recovery, w.overheated, w.heat_detonation,
                     w.heat_detonation_fraction, w.heat_loss, w.heat_illumination, w.near_reticle, w.far_reticle,
                     w.intersection_reticle, w.autoaim_angle, w.autoaim_range, w.magnetism_angle, w.magnetism_range,
                     w.deviation_angle, w.camo_ding, w.camo_regrowth, w.item_scale, *w.detonation_delay)
    struct.pack_into("<6H", b, 0xA4, w.hud_message, w.sort_order, first_mag, len(w.magazines), first_trig,
                     len(w.triggers))
    struct.pack_into("<8H", b, 0xB0, *(w.fp_ticks.get(k, 0) for k in
                                       ("ready", "put_away", "reload_empty", "reload_full", "melee", "overheated",
                                        "throw", "fire")))
    b[0xC0:0xD0] = _cstr(w.label, 16)
    b[0xD0:0xF0] = _cstr(w.name, 32)
    return bytes(b)


def pack_magazine(m: Magazine) -> bytes:
    b = bytearray(MAGAZINE_REC)
    struct.pack_into("<I6H", b, 0, m.flags, m.rounds_recharged, m.rounds_total_initial, m.rounds_total_max,
                     m.rounds_loaded_max, m.rounds_reloaded, min(len(m.equipment), 2))
    struct.pack_into("<2f2I", b, 0x10, m.reload_time, m.chamber_time, m.reload_effect, m.chamber_effect)
    for i, (tid, rounds) in enumerate(m.equipment[:2]):
        struct.pack_into("<I", b, 0x20 + 4 * i, tid)
        struct.pack_into("<H", b, 0x28 + 2 * i, rounds)
    return bytes(b)


def pack_trigger(t: Trigger, first_fx: int) -> bytes:
    b = bytearray(TRIGGER_REC)
    struct.pack_into("<I5f", b, 0, t.flags, t.rps_min, t.rps_max, t.accel_time, t.decel_time, t.blurred_rate)
    struct.pack_into("<8H", b, 0x18, t.magazine, t.rounds_per_shot, t.min_rounds_loaded, t.tracer_interval,
                     t.firing_noise, t.projectiles_per_shot, t.distribution_function, t.overcharged_action)
    struct.pack_into("<6f", b, 0x28, t.error_accel_time, t.error_decel_time, t.minimum_error, t.error_min,
                     t.error_max, t.distribution_angle)
    struct.pack_into("<3f", b, 0x40, *t.fp_offset)
    struct.pack_into("<8f", b, 0x4C, t.charging_time, t.charged_time, t.charged_illumination, t.spew_time,
                     t.ejection_recovery, t.illumination_recovery, t.heat_per_round, t.age_per_round)
    struct.pack_into("<2I2H", b, 0x6C, t.projectile, t.charging_effect, first_fx, len(t.fx))
    return bytes(b)


def pack_fx(f: FireEffect) -> bytes:
    b = bytearray(FX_REC)
    struct.pack_into("<2H6IB", b, 0, f.shots_lo, f.shots_hi, *f.effects, *f.damages, f.sound_mask)
    return bytes(b)


def pack_projectile(p: Projectile, first_mat: int) -> bytes:
    b = bytearray(PROJECTILE_REC)
    struct.pack_into("<3I2H", b, 0, p.tag_id, p.model_tag, p.flags, p.timer_starts, p.impact_noise)
    struct.pack_into("<17f", b, 0x10, p.ai_perception_radius, p.collision_radius, p.arming_time, p.danger_radius,
                     *p.timer, p.min_velocity, p.max_range, p.air_gravity, *p.air_range, p.water_gravity,
                     *p.water_range, p.initial_velocity, p.final_velocity, p.guided_angular_velocity)
    struct.pack_into("<H", b, 0x54, p.detonation_noise)
    struct.pack_into("<6I", b, 0x58, p.super_detonation, p.detonation_effect, p.detonation_started, p.flyby_sound,
                     p.attached_damage, p.impact_damage)
    struct.pack_into("<2Hf", b, 0x70, first_mat, len(p.materials), p.object_radius)
    b[0x80:0xA0] = _cstr(p.name, 32)
    return bytes(b)


def pack_material(m: MaterialResponse) -> bytes:
    b = bytearray(MATRESP_REC)
    struct.pack_into("<2HI2Hf", b, 0, m.flags, m.response, m.effect, m.potential_response, m.potential_flags,
                     m.skip_fraction)
    struct.pack_into("<2f2f", b, 0x10, *m.angle, *m.velocity)
    struct.pack_into("<I2fI4f", b, 0x20, m.potential_effect, m.angular_noise, m.velocity_noise, m.detonation_effect,
                     m.initial_friction, m.max_distance, m.parallel_friction, m.perpendicular_friction)
    return bytes(b)


def pack_damage(d: Damage) -> bytes:
    b = bytearray(DAMAGE_REC)
    struct.pack_into("<I2f2I", b, 0, d.tag_id, *d.radius, 0, 0)       # cutoff / flags written below
    struct.pack_into("<f", b, 0x0C, d.cutoff_scale)
    struct.pack_into("<I", b, 0x10, d.effect_flags)
    struct.pack_into("<2Hf2H", b, 0x14, d.flash_type, d.flash_priority, d.flash_duration, d.flash_fade, 0)
    struct.pack_into("<5f", b, 0x20, d.flash_intensity, *d.flash_color)
    struct.pack_into("<2fH", b, 0x34, d.low_rumble[0], d.low_rumble[1], d.low_rumble[2])
    struct.pack_into("<2fH", b, 0x40, d.high_rumble[0], d.high_rumble[1], d.high_rumble[2])
    struct.pack_into("<fHxxfffff", b, 0x4C, d.impulse_duration, d.impulse_fade, d.impulse_rotation, d.impulse_pushback,
                     *d.impulse_jitter, d.permanent_angle)
    struct.pack_into("<fHxxffHxxff", b, 0x68, d.shake_duration, d.shake_falloff, d.shake_translation, d.shake_rotation,
                     d.shake_wobble_function, d.shake_wobble_period, d.shake_wobble_weight)
    struct.pack_into("<I", b, 0x84, d.sound)
    struct.pack_into("<6f", b, 0x88, *d.break_forward, *d.break_outward)
    struct.pack_into("<2HI", b, 0xA0, d.side_effect, d.category, d.damage_flags)
    struct.pack_into("<8f", b, 0xA8, d.aoe_core_radius, d.damage_lower, *d.damage_upper, d.vehicle_passthrough,
                     d.active_camo, d.stun, d.max_stun)
    struct.pack_into("<f", b, 0xC8, d.stun_time)
    struct.pack_into(f"<{MAX_MATERIALS}f", b, 0xD0, *d.modifiers)
    b[0x154:0x174] = _cstr(d.name, 32)
    return bytes(b)


def pack_equipment(q: Equipment) -> bytes:
    b = bytearray(EQUIPMENT_REC)
    struct.pack_into("<4I2Hf", b, 0, q.tag_id, q.model_tag, q.anim_tag, q.item_flags, q.powerup_type, q.grenade_type,
                     q.powerup_time)
    struct.pack_into("<4I3fH", b, 0x18, q.pickup_sound, q.collision_sound, q.detonating_effect, q.detonation_effect,
                     *q.detonation_delay, q.item_scale, q.hud_message)
    b[0x40:0x60] = _cstr(q.name, 32)
    return bytes(b)


def pack_grenade(g: Grenade) -> bytes:
    b = bytearray(GRENADE_REC)
    struct.pack_into("<2H4I", b, 0, g.max_count, g.mp_spawn_default, g.throw_effect, g.hud_tag, g.equipment, g.projectile)
    return bytes(b)


HDR = struct.Struct("<4sHHI")
COUNTS_AT, OFFSETS_AT = 0x0C, 0x38
TABLES = ("weapons", "magazines", "triggers", "fx", "projectiles", "materials", "damages", "equipment", "grenades",
          "collections", "permutations")
RECORD = dict(weapons=WEAPON_REC, magazines=MAGAZINE_REC, triggers=TRIGGER_REC, fx=FX_REC, projectiles=PROJECTILE_REC,
              materials=MATRESP_REC, damages=DAMAGE_REC, equipment=EQUIPMENT_REC, grenades=GRENADE_REC,
              collections=ITMC_REC, permutations=ITMC_ENTRY_REC)


def build_section_from(s: WeapSet) -> bytes:
    tables: dict[str, list[bytes]] = {k: [] for k in TABLES}
    for w in s.weapons:
        first_mag, first_trig = len(tables["magazines"]), len(tables["triggers"])
        tables["weapons"].append(pack_weapon(w, first_mag, first_trig))
        tables["magazines"] += [pack_magazine(m) for m in w.magazines]
        for t in w.triggers:
            tables["triggers"].append(pack_trigger(t, len(tables["fx"])))
            tables["fx"] += [pack_fx(f) for f in t.fx]
    for p in s.projectiles:
        tables["projectiles"].append(pack_projectile(p, len(tables["materials"])))
        tables["materials"] += [pack_material(m) for m in p.materials]
    tables["damages"] = [pack_damage(d) for d in s.damages]
    tables["equipment"] = [pack_equipment(q) for q in s.equipment]
    tables["grenades"] = [pack_grenade(g) for g in s.grenades]
    for c in s.collections:
        b = bytearray(ITMC_REC)
        struct.pack_into("<4I", b, 0, c.tag_id, len(tables["permutations"]), len(c.entries), c.spawn_time)
        tables["collections"].append(bytes(b))
        for weight, tid in c.entries:
            e = bytearray(ITMC_ENTRY_REC)
            struct.pack_into("<fI", e, 0, weight, tid)
            tables["permutations"].append(bytes(e))
    for k, lim in (("weapons", 0xFFFF), ("projectiles", 0xFFFF), ("damages", 0xFFFF), ("equipment", 0xFFFF),
                   ("magazines", 0xFFFF), ("triggers", 0xFFFF), ("fx", 0xFFFF), ("materials", 0xFFFF)):
        if len(tables[k]) > lim:
            raise WeapError(f"{len(tables[k])} {k} exceed the u16 indices")
    out = bytearray(HEADER_SIZE)
    offsets = []
    for k in TABLES:
        offsets.append(len(out))
        for rec in tables[k]:
            assert len(rec) == RECORD[k]
            out += rec
        out += bytes(_pad16(len(out)) - len(out))
    struct.pack_into("<4sHHI", out, 0, MAGIC, VERSION, 0, len(out))
    struct.pack_into(f"<{len(TABLES)}I", out, COUNTS_AT, *(len(tables[k]) for k in TABLES))
    struct.pack_into(f"<{len(TABLES)}I", out, OFFSETS_AT, *offsets)
    return bytes(out)


def build_section(map_path: Path) -> bytes:
    return build_section_from(collect(map_path))


# ---- reference parser (tests, --info) -------------------------------------------------------------
def parse_section(blob: bytes) -> dict[str, list[bytes]]:
    """Split a ``WEAP`` section into raw records per table, validating the header the way the C reader does."""
    if len(blob) < HEADER_SIZE or blob[:4] != MAGIC:
        raise WeapError("not a WEAP section")
    magic, version, _, size = HDR.unpack_from(blob)
    if version != VERSION or size != len(blob):
        raise WeapError("bad version or size")
    n = len(TABLES)
    counts = struct.unpack_from(f"<{n}I", blob, COUNTS_AT)
    offsets = struct.unpack_from(f"<{n}I", blob, OFFSETS_AT)
    out = {}
    for k, c, o in zip(TABLES, counts, offsets):
        rec = RECORD[k]
        if o % 16 or o < HEADER_SIZE or o + c * rec > len(blob):
            raise WeapError(f"table {k} outside the section")
        out[k] = [blob[o + i * rec:o + (i + 1) * rec] for i in range(c)]
    return out


def describe(s: WeapSet) -> str:
    lines = [f"{'weapon':<22}{'rate':>9}{'mag':>9}{'reload':>8}{'speed':>8}{'damage':>14}"]
    pr = {p.tag_id: p for p in s.projectiles}
    dm = {d.tag_id: d for d in s.damages}
    for w in s.weapons:
        t = w.triggers[0] if w.triggers else None
        mg = w.magazines[0] if w.magazines else None
        p = pr.get(t.projectile) if t else None
        d = dm.get(p.impact_damage) if p else None
        rate = f"{t.rps_min:g}-{t.rps_max:g}" if t else "-"
        mag = f"{mg.rounds_loaded_max}/{mg.rounds_total_max}" if mg else "-"
        rel = f"{mg.reload_time:.2f}" if mg else "-"
        spd = f"{p.initial_velocity * TICKS:.0f}" if p else "-"
        dmg = f"{d.damage_lower:g}/{d.damage_upper[0]:g}/{d.damage_upper[1]:g}" if d else "-"
        lines.append(f"{w.name:<22}{rate:>9}{mag:>9}{rel:>8}{spd:>8}{dmg:>14}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="decompressed .map (or, with --info, a .weap.hp2)")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--info", action="store_true")
    args = ap.parse_args(argv)
    if args.input.suffix == ".hp2":
        pk = hp2.parse(args.input.read_bytes())
        tabs = parse_section(pk.read("WEAP"))
        print(", ".join(f"{k} {len(v)}" for k, v in tabs.items()))
        return 0
    t0 = time.time()
    s = collect(args.input)
    if args.info:
        print(describe(s))
        for p in s.problems:
            print("problem:", p)
        return 0
    if not args.out:
        ap.error("-o is required")
    blob = build_section_from(s)
    data = hp2.build(args.input.stem, [("WEAP", blob)])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"{args.input.stem}: {len(s.weapons)} weapons, {len(s.projectiles)} projectiles, {len(s.damages)} damage effects, "
          f"{len(s.equipment)} equipment, {len(s.grenades)} grenades; WEAP {len(blob)} B, pack {len(data)} B; "
          f"{len(s.problems)} problems; {time.time() - t0:.1f} s")
    for p in s.problems[:20]:
        print("problem:", p)
    return 1 if s.problems else 0


if __name__ == "__main__":
    sys.exit(main())
