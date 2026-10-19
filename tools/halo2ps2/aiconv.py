#!/usr/bin/env python3
"""Convert the AI data of an Xbox map (``actor`` and ``actor_variant`` tags plus the scenario's AI blocks: encounters,
squads, starting locations, move and firing positions, platoons, command lists, conversations, AI animation and script
references) into the PS2 ``AIDT`` section, version 1 (docs/25-ai-section.md; the reader is engine/src/ai/aidef.c, its
contract engine/src/ai/aidef.h).

Everything below was read off the retail maps (all ten campaign maps) and every offset is checked against real data:
"[V]" = verified (a numeric relation to other data, a cached field, an index range, a BSP point location or a name
confirms it), "[P]" = plausible (value ranges, units and the order of fields fit the public tag documentation, nothing
in the data contradicts it), "[U]" = layout known, meaning unverified.  Offsets are inside the tag's data block, every
pointer is an absolute tag-space address, a reflexive is ``u32 count, u32 address, u32 pad`` and a tag reference is 16 B
``char[4] group (byte-reversed), u32 path pointer, u32 path length (0), u32 tag id`` (id 0xFFFFFFFF = null).

UNITS: the game ticks at 30 Hz; fields in seconds say so; distances are world units (1 wu = 3.048 m); angles are
radians; fractions are 0..1.  [V: perception times are cached as 1 / (seconds * 30), cosines of the looking deviations are
cached, burst and combat ranges are 0..100 wu, angles lie in 0..pi on every actor except mounted weapons]

actor (``actr``, 1272 B, identical content of one tag path on every map, 32 distinct tags on the ten maps)
    The section keeps the whole 1272-byte block ("image") with the tag references flattened to their tag id (see
    ``flatten_refs``).  ACTOR_NAMED lists every field that is identified; the rest of the non-zero words are listed in
    ACTOR_WORDS with their kind so that nothing is lost and the C reader exposes them through ``aidef_raw_*`` by tag offset.
    +0x00 u32 flags, +0x04 u32 more flags [P], +0x14 u16 type [V: the enum of ACTOR_TYPES (0..15); the type of every actor agrees with the character class its tag path names, on all
    ten maps],
    perception +0x18 maximum vision distance, +0x1C central vision angle (50 deg), +0x20 maximum vision angle (70 deg), +0x28
    peripheral vision angle (120 deg), +0x2C peripheral distance, +0x34 standing gun offset (3 f32), +0x40 crouching gun offset
    (zero on every actor), +0x4C hearing distance, +0x50 notice projectile chance, +0x54 notice vehicle chance, +0x60 / +0x64 /
    +0x68 combat / guard / non-combat perception time (s), +0x6C / +0x70 / +0x74 their cached inverses 1 / (t * 30) [V],
    +0xA4..+0xB0 maximum aiming deviation (yaw, pitch) and maximum looking deviation (yaw, pitch) with their cosines cached at
    +0x12C..+0x138 [V], +0xB4..+0xD0 eight looking angles [P], +0xD4..+0x120 ten (min, max) idle times in seconds [P],
    +0x13C weapon reference, +0x258 projectile reference, +0x4B8 actor reference (null on every actor).

actor variant (``actv``, 568 B, or 600 B when it has a ``change colors`` block of 32 B: 78 of 297)
    +0x00 u32 flags, +0x04 actor reference [V], +0x14 unit reference [V: an actor and a biped on 268 of 297, neither on the 29 vehicle-seat
    variants], +0x24 major variant reference [V: an ``actv`` of the same character], +0x4C u16 movement type, +0x50 initial
    crouch chance, +0x54 / +0x5C crouch / run time bounds (s) [P], +0x64 weapon reference [V], +0x74 maximum firing distance,
    +0x78 rate of fire (rounds per second), +0x7C projectile error angle, +0x80 first burst delay bounds (s), +0x88 new target
    firing pattern time, +0x8C surprise delay time, +0x90 surprise fire-wildly time, +0x94 death fire-wildly chance, +0x98 death
    fire-wildly time, +0x9C desired combat range bounds (wu), +0xA4 / +0xB0 custom stand / crouch gun offsets (zero everywhere),
    +0xBC target tracking, +0xC0 target leading, +0xC4 weapon damage modifier, +0xC8 damage per second (zero everywhere), +0xCC
    burst origin radius, +0xD0 burst origin angle, +0xD4 burst return length bounds, +0xDC burst return angle, +0xE0 burst
    duration bounds (s), +0xE8 burst separation bounds (s), +0xF0 burst angular velocity, +0xF8 special damage modifier, +0xFC special
    projectile error [P], +0x154 u16 special fire mode, u16 situation, +0x158 special fire chance, +0x15C special fire delay,
    +0x180 u16 grenade type (0 fragmentation, 1 plasma [P]), u16 trajectory, +0x184 u16 grenade stimulus, u16
    minimum enemy count, +0x188 enemy radius, +0x190 grenade velocity (wu/tick = x / 30), +0x194 grenade range bounds (wu),
    +0x19C collateral damage radius, +0x1A0 grenade chance, +0x1A4 grenade check time, +0x1A8 encounter grenade timeout,
    +0x1C0 equipment reference (null everywhere), +0x1D0 u16 grenade count min / max, +0x1D4 don't-drop-grenades chance, +0x1D8
    drop-weapon-loaded bounds, +0x1E0 u16 drop-weapon-ammo min / max, +0x200 body vitality, +0x204 shield vitality [P: absolute hit
    points, 0..350, the order of magnitude of the collision models' vitals], +0x20C u16 forced shader permutation, +0x22C reflexive ``change colors``
    (count kept, pointer dropped: not converted).

scenario (``scnr``): actor palette +0x420 (16 B tag reference ``actv`` [V]), encounters +0x42C (176 B), command lists +0x438
    (96 B), AI animation references +0x444 (60 B: String32 + ``antr`` reference), AI script references +0x450 (40 B: String32),
    AI recording references +0x45C (stride unknown, empty on every map: refused if not), AI conversations +0x468 (116 B).
    [V: strides from the distance between consecutive blocks on all ten maps]
encounter (176 B): +0x00 name, +0x20 u32 flags [V: bit 0 not initially created, bit 5 3D firing positions, bit 6 manual BSP index
    specified; bit 1 respawn enabled never set], +0x24 u16 team, +0x26 u16 (always 1) [U], +0x28 u16 [U], +0x2A i16 manual BSP index [V:
    a BSP its start locations lie in when bit 6 is set, 220 of 220], +0x7E i16 precomputed BSP index [V: a BSP its start locations lie in, 444 of 444],
    +0x80 squads (232 B), +0x8C platoons (172 B), +0x98 firing positions (24 B), +0xA4 player starting locations (52 B)
squad (232 B): +0x00 name, +0x20 i16 actor type = palette index [V range], +0x22 i16 platoon [V range], +0x24 u16 initial state,
    +0x26 u16 return state [V: 0..11 on all 2988 squads, the 12 actor states], +0x28 u32 flags, +0x2C u16 unique
    leader type (0..4) [P], +0x4E i16 maneuver-to squad [V: a squad of its own encounter on 317 of 325, the other 8 dangle], +0x50 f32 squad delay time (s),
    +0x54 u32 x 7 attack / defend squad masks [U], +0x7C u16 normal / +0x7E u16 insane difficulty count [P: <= start locations on
    97 %], +0x80 u16 major upgrade (0..4) [P], +0x82/+0x84/+0x86 u16 [U], +0xC4 move positions (80 B), +0xD0 starting locations (28 B)
starting location (28 B): +0x00 f32[3] position, +0x0C f32 facing, +0x10 i16 cluster [V: the BSP cluster of the position on every
    located one], +0x12 i16 [U], +0x14 u16 [U], +0x16 u16 [U], +0x18 i16 [U], +0x1A i16 command list [V: always < command lists]
move position (80 B): +0x00 position, +0x0C facing [V: -pi..pi], +0x10 weight, +0x14 / +0x18 time bounds (s) [V: min <= max], +0x1C
    i16 animation (-1 on every one), +0x1E i16 sequence, +0x28 i16 cluster [V], +0x4C i32 pathfinding surface [P]; all else zero
firing position (24 B): +0x00 position, +0x0C u16 group [U], +0x0E i16 cluster [V], +0x14 i32 pathfinding surface [P]
platoon (172 B): +0x00 name, +0x20 u32 [U], +0x30 u16, +0x32 i16, +0x3C u16, +0x3E i16 [U]; the rest of the block is zero or stale text
player starting location of an encounter (52 B): position, facing; the rest is zero on every map
command list (96 B): +0x00 name, +0x20 u32 flags [V: bit 5 = manual BSP index specified: then +0x2C names a BSP of its points, 231 of
    233], +0x2C i16 manual BSP index, +0x2E i16 precomputed BSP index [P], +0x30 commands (32 B), +0x3C points (20 B)
command (32 B): +0x00 u16 atom type (0..27 [V: script = 12 appears once with a valid script index, recording = 14 never, animate = 13
    with valid animation indices, loop = 20 with command indices]), +0x02 u16 modifier, +0x04 f32 parameter 1, +0x08 f32 parameter 2, +0x0C
    i16 point 1, +0x0E point 2, +0x10 animation, +0x12 script, +0x14 recording, +0x16 command, +0x18 object name; +0x1A..+0x1F zero
point (20 B): +0x00 position, +0x0C i32 pathfinding surface (-1 none) [P]
conversation (116 B): +0x00 name, +0x20 u32 flags, +0x24 f32 trigger distance, +0x28 f32 run-to-player distance [P], +0x50 participants
    (84 B), +0x5C lines (124 B)
participant (84 B): +0x00 u16, +0x02 u16, +0x04 u16 selection type, +0x06 u16 actor type [V: ACTOR_TYPES values], +0x08 i16 use-object name,
    +0x0A i16 new-name object, +0x18 i16 x 6 variant numbers [V: 100 / 101 are dialogue variants of two biped tags], +0x24 String32 encounter name,
    +0x44 i32 encounter index [V: = the encounter of that name, 35 of 35]
line (124 B): +0x00 u16 flags, +0x02 i16 participant, +0x04 u16 addressee, +0x06 i16 addressee participant, +0x0C f32 delay (s),
    +0x1C six 16 B ``snd!`` references [V]

Usage:
    python3 -I tools/halo2ps2/aiconv.py assets/decompressed/a10.map -o assets/hp2/a10.ai.hp2
    python3 -I tools/halo2ps2/aiconv.py --info assets/decompressed/a10.map      # print what was read
    python3 -I tools/halo2ps2/aiconv.py --info assets/hp2/a10.ai.hp2            # print a converted pack
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halo2ps2 import hp2, scenconv, tagindex, texconv, unitconv  # noqa: E402

MAGIC = b"AIDT"
VERSION = 1
HEADER_SIZE = 256
NAME_LEN = 32
NO_TAG = 0
MAX_ACTORS = 255
MAX_VARIANTS = 1024
MAX_PALETTE = 255
MAX_ENCOUNTERS = 1024
MAX_SQUADS = 16384
MAX_STARTS = 65536
MAX_MOVES = 65536
MAX_FIRINGS = 65536
MAX_PLATOONS = 8192
MAX_PSTARTS = 4096
MAX_CMDLISTS = 1024
MAX_COMMANDS = 65536
MAX_POINTS = 65536
MAX_CONVS = 512
MAX_PARTS = 4096
MAX_LINES = 8192
MAX_ANIMS = 256
MAX_SCRIPTS = 256
MAX_COUNT = 1000                     # difficulty counts per squad
ACTOR_TYPES = ("elite", "jackal", "grunt", "hunter", "engineer", "assassin", "player", "marine", "crew", "combat form",
               "infection form", "carrier form", "monitor", "sentinel", "none", "mounted weapon")
ACTOR_STATES = ("none", "sleeping", "alert", "moving (repeat same position)", "moving (loop)", "moving (loop back and forth)",
                "moving (loop randomly)", "moving (randomly)", "guarding", "guarding (at guard position)", "searching",
                "fleeing")
TEAMS = ("default by unit", "player", "human", "covenant", "flood", "sentinel", "unused 6", "unused 7", "unused 8",
         "unused 9")
ATOM_TYPES = ("pause", "go to", "go to and face", "move in direction", "look", "animation mode", "crouch", "shoot", "grenade",
              "vehicle", "running jump", "targeted jump", "script", "animate", "recording", "action", "vocalize", "targeting",
              "initiative", "wait", "loop", "die", "move immediate", "look random", "look player", "look object",
              "set radius", "teleport")
MAX_STATE = len(ACTOR_STATES) - 1
MAX_ACTOR_TYPE = len(ACTOR_TYPES) - 1
MAX_ATOM = len(ATOM_TYPES) - 1
MAX_LEADER = 4
MAX_UPGRADE = 4

# ---- source offsets (docstring above) --------------------------------------------------------------
SCN_PALETTE, SCN_ENCOUNTERS, SCN_COMMAND_LISTS, SCN_ANIMS, SCN_SCRIPTS, SCN_RECORDINGS, SCN_CONVERSATIONS = (
    0x420, 0x42C, 0x438, 0x444, 0x450, 0x45C, 0x468)
PALETTE_REC, ENC_REC, SQUAD_SRC, PLATOON_REC, FIRING_REC, PSTART_REC = 16, 176, 232, 172, 24, 52
MOVE_REC, START_REC, CMDLIST_REC, COMMAND_REC, POINT_REC = 80, 28, 96, 32, 20
ANIMREF_REC, SCRIPTREF_REC, CONV_REC, PART_REC, LINE_REC = 60, 40, 116, 84, 124
ACTOR_SRC, VARIANT_SRC = 1272, 568
ACTOR_REFS = {0x13C: ("weap",), 0x258: ("proj",), 0x4B8: ("actr",)}
VARIANT_REFS = {0x04: ("actr",), 0x14: ("bipd", "unit"), 0x24: ("actv",), 0x64: ("weap",), 0x1C0: ("eqip",)}
VARIANT_CHANGE_COLORS = 0x22C


class AiError(ValueError):
    pass


# ---- images of the actor and variant tags ----------------------------------------------------------
# (offset, struct format, name).  The image keeps the tag layout; tag references are flattened (12 zero bytes, then
# the tag id at +12), so the offsets of the docstring are also the offsets inside a record of the section.
ACTOR_NAMED: list[tuple[int, str, str]] = [
    (0x00, "I", "flags"), (0x04, "I", "more_flags"), (0x14, "H", "type"),
    (0x18, "f", "max_vision_distance"), (0x1C, "f", "central_vision_angle"), (0x20, "f", "max_vision_angle"),
    (0x28, "f", "peripheral_vision_angle"), (0x2C, "f", "peripheral_distance"),
    (0x34, "3f", "standing_gun_offset"), (0x40, "3f", "crouching_gun_offset"),
    (0x4C, "f", "hearing_distance"), (0x50, "f", "notice_projectile_chance"), (0x54, "f", "notice_vehicle_chance"),
    (0x60, "f", "combat_perception_time"), (0x64, "f", "guard_perception_time"), (0x68, "f", "non_combat_perception_time"),
    (0x6C, "f", "inv_combat_perception_time"), (0x70, "f", "inv_guard_perception_time"),
    (0x74, "f", "inv_non_combat_perception_time"),
    (0x80, "4f", "fractions_80"), (0x90, "3f", "values_90"), (0x9C, "f", "angle_9c"), (0xA0, "f", "angle_9c_cos"),
    (0xA4, "2f", "max_aiming_deviation"), (0xAC, "2f", "max_looking_deviation"), (0xB4, "8f", "looking_angles"),
    (0xD4, "20f", "idle_times"),
    (0x12C, "2f", "cos_aiming_deviation"), (0x134, "2f", "cos_looking_deviation"),
    (0x13C + 12, "I", "weapon_tag"), (0x258 + 12, "I", "projectile_tag"), (0x4B8 + 12, "I", "actor_tag"),
]
# every other non-zero word of the actor: kind "f" (float), "H2" (two u16), "I" (u32 / enum): offsets of the words
# that are NOT floats (the reader validates the rest as finite)
ACTOR_NONFLOAT = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x24, 0x13C, 0x140, 0x144, 0x148, 0x258, 0x25C, 0x260, 0x264, 0x268,
                  0x26C, 0x278, 0x2A4, 0x2F8, 0x354, 0x4B8, 0x4BC, 0x4C0, 0x4C4}
VARIANT_NAMED: list[tuple[int, str, str]] = [
    (0x00, "I", "flags"), (0x04 + 12, "I", "actor_tag"), (0x14 + 12, "I", "unit_tag"), (0x24 + 12, "I", "major_variant_tag"),
    (0x4C, "H", "movement_type"), (0x50, "f", "initial_crouch_chance"), (0x54, "2f", "crouch_time"), (0x5C, "2f", "run_time"),
    (0x64 + 12, "I", "weapon_tag"),
    (0x74, "f", "max_firing_distance"), (0x78, "f", "rate_of_fire"), (0x7C, "f", "projectile_error"),
    (0x80, "2f", "first_burst_delay"), (0x88, "f", "new_target_firing_pattern_time"), (0x8C, "f", "surprise_delay_time"),
    (0x90, "f", "surprise_fire_wildly_time"), (0x94, "f", "death_fire_wildly_chance"), (0x98, "f", "death_fire_wildly_time"),
    (0x9C, "2f", "desired_combat_range"), (0xA4, "3f", "custom_stand_gun_offset"), (0xB0, "3f", "custom_crouch_gun_offset"),
    (0xBC, "f", "target_tracking"), (0xC0, "f", "target_leading"), (0xC4, "f", "weapon_damage_modifier"),
    (0xC8, "f", "damage_per_second"), (0xCC, "f", "burst_origin_radius"), (0xD0, "f", "burst_origin_angle"),
    (0xD4, "2f", "burst_return_length"), (0xDC, "f", "burst_return_angle"), (0xE0, "2f", "burst_duration"),
    (0xE8, "2f", "burst_separation"), (0xF0, "f", "burst_angular_velocity"), (0xF8, "f", "special_damage_modifier"),
    (0xFC, "f", "special_projectile_error"),
    (0x154, "2H", "special_fire"), (0x158, "f", "special_fire_chance"), (0x15C, "f", "special_fire_delay"),
    (0x180, "4H", "grenade_kind"), (0x188, "f", "grenade_enemy_radius"), (0x190, "f", "grenade_velocity"),
    (0x194, "2f", "grenade_ranges"), (0x19C, "f", "grenade_collateral_radius"), (0x1A0, "f", "grenade_chance"),
    (0x1A4, "f", "grenade_check_time"), (0x1A8, "f", "encounter_grenade_timeout"),
    (0x1C0 + 12, "I", "equipment_tag"),
    (0x1D0, "2H", "grenade_count"), (0x1D4, "f", "dont_drop_grenades_chance"), (0x1D8, "2f", "drop_weapon_loaded"),
    (0x1E0, "2H", "drop_weapon_ammo"),
    (0x200, "f", "body_vitality"), (0x204, "f", "shield_vitality"), (0x208, "f", "shield_sapping_radius"),
    (0x20C, "H", "forced_shader_permutation"), (VARIANT_CHANGE_COLORS, "I", "change_color_count"),
]
VARIANT_NONFLOAT = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x4C, 0x64, 0x68,
                    0x6C, 0x70, 0x154, 0x180, 0x184, 0x18C, 0x1C0, 0x1C4, 0x1C8, 0x1CC, 0x1D0, 0x1E0, 0x20C, 0x22C, 0x230,
                    0x234}
ACTOR_REC = ACTOR_SRC
VARIANT_REC = VARIANT_SRC


def _decode(img: bytes, named: list[tuple[int, str, str]]) -> dict:
    out = {}
    for off, fmt, name in named:
        vals = struct.unpack_from("<" + fmt, img, off)
        out[name] = vals[0] if len(vals) == 1 else vals
    return out


def decode_actor(img: bytes) -> dict:
    return _decode(img, ACTOR_NAMED)


def decode_variant(img: bytes) -> dict:
    return _decode(img, VARIANT_NAMED)


# ---- record layouts of the tables --------------------------------------------------------------------
def _layout(fields: list[tuple[str, str]], size: int):
    return unitconv._layout(fields, size)


ENCOUNTER_L, ENCOUNTER_REC = _layout([
    ("name", "32s"), ("flags", "I"), ("team", "H"), ("search_behavior", "H"), ("w28", "H"), ("manual_bsp", "h"),
    ("precomputed_bsp", "h"), ("pad0", "H"),
    ("first_squad", "I"), ("squad_count", "I"), ("first_platoon", "I"), ("platoon_count", "I"), ("first_firing", "I"),
    ("firing_count", "I"), ("first_pstart", "I"), ("pstart_count", "I")], 80)
SQUAD_L, SQUAD_REC = _layout([
    ("name", "32s"), ("actor_type", "h"), ("platoon", "h"), ("initial_state", "H"), ("return_state", "H"), ("flags", "I"),
    ("unique_leader", "H"), ("maneuver_squad", "h"), ("delay_time", "f"), ("masks", "7I"), ("normal_count", "H"),
    ("insane_count", "H"), ("major_upgrade", "H"), ("w82", "H"), ("w84", "H"), ("w86", "H"),
    ("first_start", "I"), ("start_count", "I"), ("first_move", "I"), ("move_count", "I")], 112)
START_L, START_REC_OUT = _layout([
    ("pos", "3f"), ("facing", "f"), ("cluster", "h"), ("w12", "h"), ("w14", "H"), ("w16", "H"), ("w18", "h"),
    ("command_list", "h")], 28)
MOVE_L, MOVE_REC_OUT = _layout([
    ("pos", "3f"), ("facing", "f"), ("weight", "f"), ("time_min", "f"), ("time_max", "f"), ("animation", "h"),
    ("sequence", "h"), ("cluster", "h"), ("pad0", "H"), ("surface", "i")], 40)
FIRING_L, FIRING_REC_OUT = _layout([
    ("pos", "3f"), ("group", "H"), ("cluster", "h"), ("surface", "i")], 20)
PLATOON_L, PLATOON_REC_OUT = _layout([
    ("name", "32s"), ("w20", "I"), ("w30", "H"), ("w32", "h"), ("w3c", "H"), ("w3e", "h")], 48)
PSTART_L, PSTART_REC_OUT = _layout([
    ("pos", "3f"), ("facing", "f"), ("team", "H"), ("bsp", "H"), ("types", "4H"), ("pad0", "I")], 32)
CMDLIST_L, CMDLIST_REC_OUT = _layout([
    ("name", "32s"), ("flags", "I"), ("manual_bsp", "h"), ("precomputed_bsp", "h"), ("first_command", "I"), ("command_count", "I"),
    ("first_point", "I"), ("point_count", "I"), ("pad1", "2I")], 64)
COMMAND_L, COMMAND_REC_OUT = _layout([
    ("type", "H"), ("modifier", "H"), ("param1", "f"), ("param2", "f"), ("point1", "h"), ("point2", "h"),
    ("animation", "h"), ("script", "h"), ("recording", "h"), ("command", "h"), ("object_name", "h"), ("pad0", "3H")], 32)
POINT_L, POINT_REC_OUT = _layout([("pos", "3f"), ("surface", "i"), ("pad0", "I")], 20)
CONV_L, CONV_REC_OUT = _layout([
    ("name", "32s"), ("flags", "I"), ("trigger_distance", "f"), ("run_to_player_distance", "f"),
    ("first_participant", "I"), ("participant_count", "I"), ("first_line", "I"), ("line_count", "I"), ("pad0", "I")], 64)
PART_L, PART_REC_OUT = _layout([
    ("flags", "H"), ("w02", "H"), ("selection_type", "H"), ("actor_type", "H"), ("use_object", "h"),
    ("new_name_object", "h"), ("variants", "6h"), ("encounter_index", "i"), ("encounter_name", "32s")], 64)
LINE_L, LINE_REC_OUT = _layout([
    ("flags", "H"), ("participant", "h"), ("addressee", "H"), ("addressee_participant", "h"), ("delay", "f"),
    ("sounds", "6I"), ("pad0", "3I")], 48)
ANIM_L, ANIM_REC_OUT = _layout([("name", "32s"), ("graph_tag", "I"), ("pad0", "3I")], 48)
SCRIPT_L, SCRIPT_REC_OUT = _layout([("name", "32s")], 32)
PALETTE_L, PALETTE_REC_OUT = _layout([("variant_tag", "I"), ("variant_index", "I")], 8)
NO_INDEX = 0xFFFFFFFF

# table directory: (name, record size, maximum count); the order is the order of the directory in the header
TABLES: list[tuple[str, int, int]] = [
    ("actors", ACTOR_REC, MAX_ACTORS), ("variants", VARIANT_REC, MAX_VARIANTS), ("actor_ids", 4, MAX_ACTORS),
    ("variant_ids", 4, MAX_VARIANTS), ("palette", PALETTE_REC_OUT, MAX_PALETTE),
    ("encounters", ENCOUNTER_REC, MAX_ENCOUNTERS), ("squads", SQUAD_REC, MAX_SQUADS), ("starts", START_REC_OUT, MAX_STARTS),
    ("moves", MOVE_REC_OUT, MAX_MOVES), ("firings", FIRING_REC_OUT, MAX_FIRINGS), ("platoons", PLATOON_REC_OUT, MAX_PLATOONS),
    ("pstarts", PSTART_REC_OUT, MAX_PSTARTS), ("cmdlists", CMDLIST_REC_OUT, MAX_CMDLISTS),
    ("commands", COMMAND_REC_OUT, MAX_COMMANDS), ("points", POINT_REC_OUT, MAX_POINTS), ("convs", CONV_REC_OUT, MAX_CONVS),
    ("parts", PART_REC_OUT, MAX_PARTS), ("lines", LINE_REC_OUT, MAX_LINES), ("anims", ANIM_REC_OUT, MAX_ANIMS),
    ("scripts", SCRIPT_REC_OUT, MAX_SCRIPTS), ("enc_names", 4, MAX_ENCOUNTERS), ("cmd_names", 4, MAX_CMDLISTS),
    ("conv_names", 4, MAX_CONVS),
]
TABLE_INDEX = {n: i for i, (n, _, _) in enumerate(TABLES)}
DIR_OFFSET = 16
assert DIR_OFFSET + 8 * len(TABLES) <= HEADER_SIZE


# ---- tag access ------------------------------------------------------------------------------------
class Reader(unitconv.Reader):
    """unitconv.Reader plus raw reads and name sanitising."""

    def raw(self, off: int, n: int) -> bytes:
        if off < 0 or off + n > len(self.r.data):
            raise AiError(f"read of {n} bytes at {off:#x} outside the tag region")
        return bytes(self.r.data[off:off + n])

    def i(self, fmt: str, off: int):
        try:
            return self.r.u("<" + fmt, off)
        except scenconv.ScenError as ex:
            raise AiError(str(ex)) from None

    def refl_checked(self, off: int, stride: int, limit: int) -> tuple[int, int]:
        try:
            return self.r.reflexive(off, stride, limit)
        except scenconv.ScenError as ex:
            raise AiError(str(ex)) from None

    def finite_f(self, off: int, what: str, default: float = 0.0) -> float:
        v = self.i("f", off)[0]
        if not math.isfinite(v):
            self.problems.append(f"{what}: non-finite float at {off:#x}")
            return default
        return v

    def clean_name(self, off: int, what: str) -> str:
        raw = self.raw(off, NAME_LEN).split(b"\0", 1)[0]
        if any(c < 0x20 or c > 0x7E for c in raw):
            self.problems.append(f"{what}: name with non-printable bytes")
            raw = bytes(c if 0x20 <= c <= 0x7E else 0x3F for c in raw)
        return raw.decode("ascii")


def _tail(path: str) -> str:
    return path.rsplit("\\", 1)[-1]


def flatten_refs(img: bytearray, refs: dict, rd: Reader, what: str) -> None:
    """Replace every 16-byte tag reference of an image by 12 zero bytes and the tag id (0 = none). ``refs`` maps the
    offset to the groups the reference may name (a null reference of another group is fine)."""
    for off, groups in refs.items():
        g = tagindex.fourcc(bytes(img[off:off + 4]))
        tid = struct.unpack_from("<I", img, off + 12)[0]
        if tid == 0xFFFFFFFF or g is None:
            tid = 0
        else:
            e = rd.m.by_id.get(tid)
            if e is None or e.group != g or g not in groups:
                rd.problems.append(f"{what}: reference at +{off:#x} ({g!r} {tid:#x}) not resolved")
                tid = 0
        img[off:off + 16] = bytes(12) + struct.pack("<I", tid)


def read_actor(rd: Reader, e: tagindex.TagEntry) -> bytes:
    b = rd.block(e)
    img = bytearray(rd.raw(b, ACTOR_SRC))
    flatten_refs(img, ACTOR_REFS, rd, e.path)
    return bytes(img)


def read_variant(rd: Reader, e: tagindex.TagEntry) -> bytes:
    b = rd.block(e)
    img = bytearray(rd.raw(b, VARIANT_SRC))
    flatten_refs(img, VARIANT_REFS, rd, e.path)       # the unit is a biped on 268 variants, a null `unit` on the others
    kind = struct.unpack_from("<4H", img, 0x180)      # grenade type / trajectory / stimulus / minimum enemies
    if kind[0] > 7 or kind[1] > 7 or kind[2] > 7 or kind[3] > 255:
        # six vehicle-seat variants carry stale text here (uninitialised tag memory): zero the 16 bytes
        rd.problems.append(f"{e.path}: stale bytes at +0x180 (grenade words {kind}) zeroed")
        img[0x180:0x190] = bytes(16)
    n = struct.unpack_from("<I", img, VARIANT_CHANGE_COLORS)[0]
    img[VARIANT_CHANGE_COLORS + 4:VARIANT_CHANGE_COLORS + 12] = bytes(8)     # pointer and pad: the block is not converted
    struct.pack_into("<I", img, VARIANT_CHANGE_COLORS, n if n < 256 else 0)
    return bytes(img)


def check_image(img: bytes, nonfloat: set[int], what: str, problems: list[str]) -> None:
    for off in range(0, len(img), 4):
        if off in nonfloat:
            continue
        v = struct.unpack_from("<I", img, off)[0]
        if (v & 0x7F800000) == 0x7F800000:
            problems.append(f"{what}: non-finite float word at +{off:#x}")


# ---- the AI set -------------------------------------------------------------------------------------
@dataclass
class AiSet:
    actors: list[tuple[int, bytes]]
    variants: list[tuple[int, bytes]]
    palette: list[int]
    encounters: list[unitconv.Rec] = field(default_factory=list)
    squads: list[unitconv.Rec] = field(default_factory=list)
    starts: list[unitconv.Rec] = field(default_factory=list)
    moves: list[unitconv.Rec] = field(default_factory=list)
    firings: list[unitconv.Rec] = field(default_factory=list)
    platoons: list[unitconv.Rec] = field(default_factory=list)
    pstarts: list[unitconv.Rec] = field(default_factory=list)
    cmdlists: list[unitconv.Rec] = field(default_factory=list)
    commands: list[unitconv.Rec] = field(default_factory=list)
    points: list[unitconv.Rec] = field(default_factory=list)
    convs: list[unitconv.Rec] = field(default_factory=list)
    parts: list[unitconv.Rec] = field(default_factory=list)
    lines: list[unitconv.Rec] = field(default_factory=list)
    anims: list[unitconv.Rec] = field(default_factory=list)
    scripts: list[unitconv.Rec] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)
    sound_tags: set[int] = field(default_factory=set)       # snd! ids of conversation lines


def _rec(layout, size, **vals) -> unitconv.Rec:
    r = unitconv.Rec(layout, size)
    for k, v in vals.items():
        r.set(k, v)
    return r


def _idx(v: int, count: int, rd: Reader, what: str, allow_none: bool = True) -> int:
    """An index that must be -1 (none) or < count; anything else is reported and becomes -1."""
    if v == -1 and allow_none:
        return -1
    if 0 <= v < count:
        return v
    rd.problems.append(f"{what}: index {v} outside 0..{count}")
    return -1


def read_actors(rd: Reader) -> tuple[list[tuple[int, bytes]], list[tuple[int, bytes]]]:
    actors = [(e.tag_id, read_actor(rd, e)) for e in rd.entries("actr")]
    variants = [(e.tag_id, read_variant(rd, e)) for e in rd.entries("actv")]
    for tid, img in actors:
        check_image(img, ACTOR_NONFLOAT, f"actor {tid:#x}", rd.problems)
        t = struct.unpack_from("<H", img, 0x14)[0]
        if t > MAX_ACTOR_TYPE:
            raise AiError(f"actor {tid:#x}: type {t}")
    for tid, img in variants:
        check_image(img, VARIANT_NONFLOAT, f"variant {tid:#x}", rd.problems)
    if len(actors) > MAX_ACTORS or len(variants) > MAX_VARIANTS:
        raise AiError("too many actors or variants for the section limits")
    return actors, variants


def read_palette(rd: Reader, sb: int) -> list[int]:
    n, o = rd.refl_checked(sb + SCN_PALETTE, PALETTE_REC, MAX_PALETTE)
    return [rd.ref(o + i * PALETTE_REC, "actv") for i in range(n)]


def read_scenario(rd: Reader, sb: int, palette: list[int]) -> AiSet:
    s = AiSet([], [], palette)
    # --- AI animation / script / recording references
    n, o = rd.refl_checked(sb + SCN_ANIMS, ANIMREF_REC, MAX_ANIMS)
    for i in range(n):
        q = o + i * ANIMREF_REC
        s.anims.append(_rec(ANIM_L, ANIM_REC_OUT, name=rd.clean_name(q, f"ai animation {i}"),
                            graph_tag=rd.ref(q + 0x20, "antr")))
    n, o = rd.refl_checked(sb + SCN_SCRIPTS, SCRIPTREF_REC, MAX_SCRIPTS)
    for i in range(n):
        s.scripts.append(_rec(SCRIPT_L, SCRIPT_REC_OUT, name=rd.clean_name(o + i * SCRIPTREF_REC, f"ai script {i}")))
    n, _ = rd.refl_checked(sb + SCN_RECORDINGS, 4, 4096)
    if n:
        raise AiError(f"{n} AI recording references: stride unknown, not supported")
    # --- command lists (their counts are needed for the start locations)
    cn, co = rd.refl_checked(sb + SCN_COMMAND_LISTS, CMDLIST_REC, MAX_CMDLISTS)
    for i in range(cn):
        q = co + i * CMDLIST_REC
        nm = rd.clean_name(q, f"command list {i}")
        flags = rd.i("I", q + 0x20)[0]
        bsp, pre = rd.i("2h", q + 0x2C)
        nc, ca = rd.refl_checked(q + 0x30, COMMAND_REC, 4096)
        np_, pa = rd.refl_checked(q + 0x3C, POINT_REC, 4096)
        rec = _rec(CMDLIST_L, CMDLIST_REC_OUT, name=nm, flags=flags, manual_bsp=bsp, precomputed_bsp=pre,
                   first_command=len(s.commands),
                   command_count=nc, first_point=len(s.points), point_count=np_)
        s.cmdlists.append(rec)
        for k in range(nc):
            c = ca + k * COMMAND_REC
            typ, mod = rd.i("2H", c)
            if typ > MAX_ATOM:
                rd.problems.append(f"command list {i}: atom type {typ}")
                typ = 0
            p1 = rd.finite_f(c + 4, f"command list {i}")
            p2 = rd.finite_f(c + 8, f"command list {i}")
            idx = rd.i("7h", c + 12)
            s.commands.append(_rec(COMMAND_L, COMMAND_REC_OUT, type=typ, modifier=mod, param1=p1, param2=p2, point1=idx[0],
                                   point2=idx[1], animation=idx[2], script=idx[3], recording=idx[4], command=idx[5],
                                   object_name=idx[6]))
        for k in range(np_):
            p = pa + k * POINT_REC
            pos = tuple(rd.finite_f(p + 4 * a, f"command list {i} point") for a in range(3))
            s.points.append(_rec(POINT_L, POINT_REC_OUT, pos=pos, surface=rd.i("i", p + 12)[0]))
    # --- encounters
    en, eo = rd.refl_checked(sb + SCN_ENCOUNTERS, ENC_REC, MAX_ENCOUNTERS)
    for i in range(en):
        q = eo + i * ENC_REC
        nm = rd.clean_name(q, f"encounter {i}")
        sn, sa = rd.refl_checked(q + 0x80, SQUAD_SRC, MAX_SQUADS)
        pn, pa = rd.refl_checked(q + 0x8C, PLATOON_REC, MAX_PLATOONS)
        fn, fa = rd.refl_checked(q + 0x98, FIRING_REC, MAX_FIRINGS)
        tn, ta = rd.refl_checked(q + 0xA4, PSTART_REC, MAX_PSTARTS)
        team = rd.i("H", q + 0x24)[0]
        if team >= len(TEAMS):
            rd.problems.append(f"encounter {i}: team {team}")
            team = 0
        rec = _rec(ENCOUNTER_L, ENCOUNTER_REC, name=nm, flags=rd.i("I", q + 0x20)[0], team=team,
                   search_behavior=rd.i("H", q + 0x26)[0], w28=rd.i("H", q + 0x28)[0],
                   manual_bsp=rd.i("h", q + 0x2A)[0], precomputed_bsp=rd.i("h", q + 0x7E)[0],
                   first_squad=len(s.squads), squad_count=sn, first_platoon=len(s.platoons), platoon_count=pn,
                   first_firing=len(s.firings), firing_count=fn, first_pstart=len(s.pstarts), pstart_count=tn)
        s.encounters.append(rec)
        for k in range(pn):
            p = pa + k * PLATOON_REC
            s.platoons.append(_rec(PLATOON_L, PLATOON_REC_OUT, name=rd.clean_name(p, f"encounter {i} platoon {k}"),
                                   w20=rd.i("I", p + 0x20)[0], w30=rd.i("H", p + 0x30)[0], w32=rd.i("h", p + 0x32)[0],
                                   w3c=rd.i("H", p + 0x3C)[0], w3e=rd.i("h", p + 0x3E)[0]))
        for k in range(fn):
            p = fa + k * FIRING_REC
            pos = tuple(rd.finite_f(p + 4 * a, f"encounter {i} firing position") for a in range(3))
            s.firings.append(_rec(FIRING_L, FIRING_REC_OUT, pos=pos, group=rd.i("H", p + 0x0C)[0],
                                  cluster=rd.i("h", p + 0x0E)[0], surface=rd.i("i", p + 0x14)[0]))
        for k in range(tn):
            p = ta + k * PSTART_REC
            pos = tuple(rd.finite_f(p + 4 * a, f"encounter {i} player start") for a in range(3))
            s.pstarts.append(_rec(PSTART_L, PSTART_REC_OUT, pos=pos, facing=rd.finite_f(p + 12, "player start facing"),
                                  team=rd.i("H", p + 0x10)[0], bsp=rd.i("H", p + 0x12)[0],
                                  types=rd.i("4H", p + 0x14)))
        for k in range(sn):
            p = sa + k * SQUAD_SRC
            what = f"encounter {i} squad {k}"
            mn, ma = rd.refl_checked(p + 0xC4, MOVE_REC, MAX_MOVES)
            ln, la = rd.refl_checked(p + 0xD0, START_REC, MAX_STARTS)
            st0, st1 = rd.i("2H", p + 0x24)
            if st0 > MAX_STATE or st1 > MAX_STATE:
                rd.problems.append(f"{what}: states {st0} {st1}")
                st0, st1 = min(st0, MAX_STATE), min(st1, MAX_STATE)
            leader = rd.i("H", p + 0x2C)[0]
            upg = rd.i("H", p + 0x80)[0]
            if leader > MAX_LEADER:
                rd.problems.append(f"{what}: unique leader type {leader}")
                leader = 0
            if upg > MAX_UPGRADE:
                rd.problems.append(f"{what}: major upgrade {upg}")
                upg = 0
            nc_, ic_ = rd.i("2H", p + 0x7C)
            if nc_ > MAX_COUNT or ic_ > MAX_COUNT:
                rd.problems.append(f"{what}: difficulty counts {nc_} {ic_}")
                nc_, ic_ = min(nc_, MAX_COUNT), min(ic_, MAX_COUNT)
            rec = _rec(SQUAD_L, SQUAD_REC, name=rd.clean_name(p, what),
                       actor_type=_idx(rd.i("h", p + 0x20)[0], len(palette), rd, what + " actor type"),
                       platoon=_idx(rd.i("h", p + 0x22)[0], pn, rd, what + " platoon"),
                       initial_state=st0, return_state=st1, flags=rd.i("I", p + 0x28)[0], unique_leader=leader,
                       maneuver_squad=_idx(rd.i("h", p + 0x4E)[0], sn, rd, what + " maneuver squad"),
                       delay_time=rd.finite_f(p + 0x50, what), masks=rd.i("7I", p + 0x54), normal_count=nc_,
                       insane_count=ic_, major_upgrade=upg, w82=rd.i("H", p + 0x82)[0], w84=rd.i("H", p + 0x84)[0],
                       w86=rd.i("H", p + 0x86)[0], first_start=len(s.starts), start_count=ln,
                       first_move=len(s.moves), move_count=mn)
            s.squads.append(rec)
            for j in range(ln):
                q2 = la + j * START_REC
                pos = tuple(rd.finite_f(q2 + 4 * a, what + " start") for a in range(3))
                w = rd.i("hhHHhh", q2 + 0x10)
                s.starts.append(_rec(START_L, START_REC_OUT, pos=pos, facing=rd.finite_f(q2 + 12, what + " start facing"),
                                     cluster=w[0], w12=w[1], w14=w[2], w16=w[3], w18=w[4],
                                     command_list=_idx(w[5], cn, rd, what + " start command list")))
            for j in range(mn):
                q2 = ma + j * MOVE_REC
                pos = tuple(rd.finite_f(q2 + 4 * a, what + " move") for a in range(3))
                f = [rd.finite_f(q2 + 12 + 4 * a, what + " move") for a in range(4)]
                an, seq = rd.i("2h", q2 + 0x1C)
                s.moves.append(_rec(MOVE_L, MOVE_REC_OUT, pos=pos, facing=f[0], weight=f[1], time_min=f[2], time_max=f[3],
                                    animation=an, sequence=seq, cluster=rd.i("h", q2 + 0x28)[0],
                                    surface=rd.i("i", q2 + 0x4C)[0]))
    # --- conversations
    vn, vo = rd.refl_checked(sb + SCN_CONVERSATIONS, CONV_REC, MAX_CONVS)
    for i in range(vn):
        q = vo + i * CONV_REC
        what = f"conversation {i}"
        pn, pa = rd.refl_checked(q + 0x50, PART_REC, MAX_PARTS)
        ln, la = rd.refl_checked(q + 0x5C, LINE_REC, MAX_LINES)
        s.convs.append(_rec(CONV_L, CONV_REC_OUT, name=rd.clean_name(q, what), flags=rd.i("I", q + 0x20)[0],
                            trigger_distance=rd.finite_f(q + 0x24, what), run_to_player_distance=rd.finite_f(q + 0x28, what),
                            first_participant=len(s.parts), participant_count=pn, first_line=len(s.lines), line_count=ln))
        for k in range(pn):
            p = pa + k * PART_REC
            w = rd.i("6H", p)
            enc_i = rd.i("i", p + 0x44)[0]
            if enc_i != -1 and not 0 <= enc_i < en:
                rd.problems.append(f"{what} participant {k}: encounter index {enc_i}")
                enc_i = -1
            s.parts.append(_rec(PART_L, PART_REC_OUT, flags=w[0], w02=w[1], selection_type=w[2], actor_type=w[3],
                                use_object=rd.i("h", p + 8)[0], new_name_object=rd.i("h", p + 10)[0],
                                variants=rd.i("6h", p + 0x18), encounter_index=enc_i,
                                encounter_name=rd.clean_name(p + 0x24, what + " participant")))
        for k in range(ln):
            p = la + k * LINE_REC
            flags, part, addr, addr_part = rd.i("HhHh", p)
            if part != -1 and not 0 <= part < pn:
                rd.problems.append(f"{what} line {k}: participant {part}")
                part = -1
            sounds = [rd.ref(p + 0x1C + 16 * v, "snd!") for v in range(6)]
            s.sound_tags.update(t for t in sounds if t)
            s.lines.append(_rec(LINE_L, LINE_REC_OUT, flags=flags, participant=part, addressee=addr,
                                addressee_participant=addr_part, delay=rd.finite_f(p + 0x0C, what + " line"),
                                sounds=tuple(sounds)))
    return s


def collect_map(m: texconv.MapData) -> AiSet:
    rd = Reader(m)
    actors, variants = read_actors(rd)
    sb = rd.block(m.index.scenario)
    palette = read_palette(rd, sb)
    s = read_scenario(rd, sb, palette)
    s.actors, s.variants = actors, variants
    s.problems = rd.problems
    return s


def collect(map_path: Path) -> AiSet:
    return collect_map(texconv.MapData.open(Path(map_path)))


# ---- roots for the object pack and the sound converter ---------------------------------------------
def _ref_id(rd: Reader, off: int, group: str) -> int:
    saved = len(rd.problems)
    t = rd.ref(off, group)
    del rd.problems[saved:]
    return t


def unit_assets(rd: Reader, variants: list[tuple[int, bytes]]) -> dict:
    """The units, weapons and dialogue tags the variants refer to, with their model / graph / dialogue ids."""
    out = {"models": set(), "anims": set(), "dialogue": set()}
    for _tid, img in variants:
        unit = struct.unpack_from("<I", img, 0x14 + 12)[0]
        weap = struct.unpack_from("<I", img, 0x64 + 12)[0]
        for tag, group, doff in ((unit, "bipd", 0x2B4), (weap, "weap", None)):
            e = rd.m.by_id.get(tag)
            if tag == 0 or e is None or e.group != group:
                continue
            b = rd.block(e)
            for off, g, key in ((0x28, "mode", "models"), (0x38, "antr", "anims")):
                t = _ref_id(rd, b + off, g)
                if t:
                    out[key].add(t)
            if doff is not None:
                n, o = rd.refl_checked(b + doff, 24, 256)
                for i in range(n):
                    t = _ref_id(rd, o + i * 24 + 8, "udlg")
                    if t:
                        out["dialogue"].add(t)
    return out


def referenced_object_tags(map_path: Path) -> dict[int, str]:
    """Object tags (biped and weapon tag id -> group) every actor variant spawns: the SCEN definitions the AI needs to create them
    at run time (no placement mentions them)."""
    m = texconv.MapData.open(Path(map_path))
    rd = Reader(m)
    _, variants = read_actors(rd)
    out: dict[int, str] = {}
    for _tid, img in variants:
        unit = struct.unpack_from("<I", img, 0x14 + 12)[0]
        weap = struct.unpack_from("<I", img, 0x64 + 12)[0]
        for tag, group in ((unit, "bipd"), (weap, "weap")):
            e = m.by_id.get(tag)
            if tag and e is not None and e.group == group:
                out[tag] = group
    return out


def referenced_models(map_path: Path) -> set[int]:
    """Tag ids of the ``model`` tags of every unit and weapon an actor variant of the map uses (the objpack roots for the
    AI characters no scenario placement mentions), keyed like modelconv."""
    m = texconv.MapData.open(Path(map_path))
    rd = Reader(m)
    _, variants = read_actors(rd)
    return unit_assets(rd, variants)["models"]


def referenced_anims(map_path: Path) -> set[int]:
    """Tag ids of the ``model_animations`` graphs of those units and weapons plus the graphs the scenario's AI animation
    references (command list ``animate`` atoms) name, keyed like animconv."""
    m = texconv.MapData.open(Path(map_path))
    rd = Reader(m)
    _, variants = read_actors(rd)
    out = set(unit_assets(rd, variants)["anims"])
    sb = rd.block(m.index.scenario)
    n, o = rd.refl_checked(sb + SCN_ANIMS, ANIMREF_REC, MAX_ANIMS)
    for i in range(n):
        t = _ref_id(rd, o + i * ANIMREF_REC + 0x20, "antr")
        if t:
            out.add(t)
    return out


def dialogue_sounds(rd: Reader, dialogue: set[int]) -> set[int]:
    """The ``snd!`` tags of ``udlg`` (unit dialogue) tags: 16-byte references from +0x10 on (4112 B: 256 of them)."""
    out = set()
    for tid in dialogue:
        e = rd.m.by_id.get(tid)
        if e is None or e.group != "udlg":
            continue
        b = rd.block(e)
        for k in range(256):
            off = b + 0x10 + 16 * k
            if off + 16 > len(rd.r.data):
                break
            t = _ref_id(rd, off, "snd!")
            if t:
                out.add(t)
    return out


def referenced_sounds(map_path: Path) -> set[int]:
    """Tag ids of the ``sound`` tags the AI uses: every sound of the scenario's AI conversations and the unit dialogue
    (``udlg``) sounds of the units of the actor variants, for soundconv's ``--roots`` and streamconv."""
    m = texconv.MapData.open(Path(map_path))
    rd = Reader(m)
    _, variants = read_actors(rd)
    sb = rd.block(m.index.scenario)
    palette = read_palette(rd, sb)
    s = read_scenario(rd, sb, palette)
    return set(s.sound_tags) | dialogue_sounds(rd, unit_assets(rd, variants)["dialogue"])


# ---- section writer ----------------------------------------------------------------------------------
def _pad16(b: bytearray) -> None:
    b += bytes(-len(b) % 16)


def _name_index(recs: list[unitconv.Rec]) -> list[int]:
    return sorted(range(len(recs)), key=lambda i: (recs[i].vals["name"].lower(), i))


def table_blobs(s: AiSet) -> dict[str, list[bytes]]:
    pal = []
    vindex = {tid: i for i, (tid, _) in enumerate(s.variants)}
    for t in s.palette:
        pal.append(unitconv.Rec(PALETTE_L, PALETTE_REC_OUT, {"variant_tag": t, "variant_index": vindex.get(t, NO_INDEX)}).pack())
    out = {
        "actors": [img for _, img in s.actors], "variants": [img for _, img in s.variants],
        "actor_ids": [struct.pack("<I", t) for t, _ in s.actors],
        "variant_ids": [struct.pack("<I", t) for t, _ in s.variants], "palette": pal,
    }
    for name, lst in (("encounters", s.encounters), ("squads", s.squads), ("starts", s.starts), ("moves", s.moves),
                      ("firings", s.firings), ("platoons", s.platoons), ("pstarts", s.pstarts), ("cmdlists", s.cmdlists),
                      ("commands", s.commands), ("points", s.points), ("convs", s.convs), ("parts", s.parts),
                      ("lines", s.lines), ("anims", s.anims), ("scripts", s.scripts)):
        out[name] = [r.pack() for r in lst]
    out["enc_names"] = [struct.pack("<I", i) for i in _name_index(s.encounters)]
    out["cmd_names"] = [struct.pack("<I", i) for i in _name_index(s.cmdlists)]
    out["conv_names"] = [struct.pack("<I", i) for i in _name_index(s.convs)]
    return out


def build(s: AiSet) -> bytes:
    if [t for t, _ in s.actors] != sorted({t for t, _ in s.actors}) or \
            [t for t, _ in s.variants] != sorted({t for t, _ in s.variants}):
        raise AiError("actor and variant ids must be strictly ascending")
    blobs = table_blobs(s)
    out = bytearray(HEADER_SIZE)
    for name, rec, limit in TABLES:
        items = blobs[name]
        if len(items) > limit:
            raise AiError(f"table {name}: {len(items)} entries, limit {limit}")
        _pad16(out)
        struct.pack_into("<II", out, DIR_OFFSET + 8 * TABLE_INDEX[name], len(out), len(items))
        for b in items:
            if len(b) != rec:
                raise AssertionError(f"{name}: record of {len(b)} bytes, expected {rec}")
            out += b
    _pad16(out)
    struct.pack_into("<4sHHI", out, 0, MAGIC, VERSION, 0, len(out))
    return bytes(out)


def build_section(map_path: Path) -> bytes:
    return build(collect(map_path))


# ---- reference parser (tests, --info) ------------------------------------------------------------------
RECORD_LAYOUTS = {
    "encounters": ENCOUNTER_L, "squads": SQUAD_L, "starts": START_L, "moves": MOVE_L, "firings": FIRING_L,
    "platoons": PLATOON_L, "pstarts": PSTART_L, "cmdlists": CMDLIST_L, "commands": COMMAND_L, "points": POINT_L,
    "convs": CONV_L, "parts": PART_L, "lines": LINE_L, "anims": ANIM_L, "scripts": SCRIPT_L, "palette": PALETTE_L,
}


def parse_section(data: bytes) -> dict:
    """Decode a section into {table name: [record dict]} (actors / variants: dicts of the named fields plus ``raw``)."""
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise AiError("not an AIDT section")
    ver, _res, size = struct.unpack_from("<HHI", data, 4)
    if ver != VERSION or size != len(data):
        raise AiError("bad version or size")
    out: dict = {"header": {"size": size}}
    for name, rec, limit in TABLES:
        off, cnt = struct.unpack_from("<II", data, DIR_OFFSET + 8 * TABLE_INDEX[name])
        if off % 16 or off < HEADER_SIZE or cnt > limit or off + cnt * rec > size:
            raise AiError(f"table {name} outside the section")
        recs = [data[off + i * rec:off + (i + 1) * rec] for i in range(cnt)]
        if name in RECORD_LAYOUTS:
            out[name] = [unitconv.parse_rec(RECORD_LAYOUTS[name], r) for r in recs]
        elif name == "actors":
            out[name] = [dict(decode_actor(r), raw=r) for r in recs]
        elif name == "variants":
            out[name] = [dict(decode_variant(r), raw=r) for r in recs]
        else:
            out[name] = [struct.unpack("<I", r)[0] for r in recs]
    return out


# ---- reporting ------------------------------------------------------------------------------------------
def counts(s: AiSet) -> dict[str, int]:
    return {"actors": len(s.actors), "variants": len(s.variants), "palette": len(s.palette),
            "encounters": len(s.encounters), "squads": len(s.squads), "starts": len(s.starts), "moves": len(s.moves),
            "firings": len(s.firings), "platoons": len(s.platoons), "pstarts": len(s.pstarts),
            "cmdlists": len(s.cmdlists), "commands": len(s.commands), "points": len(s.points), "convs": len(s.convs),
            "parts": len(s.parts), "lines": len(s.lines), "anims": len(s.anims), "scripts": len(s.scripts)}


def describe(m: texconv.MapData, s: AiSet) -> str:
    c = counts(s)
    lines = [f"{m.path.stem}: " + ", ".join(f"{v} {k}" for k, v in c.items())]
    lines += [f"  problem: {p}" for p in s.problems]
    return "\n".join(lines)


def info_pack(path: Path) -> str:
    pk = hp2.parse(Path(path).read_bytes())
    sec = parse_section(pk.read("AIDT"))
    c = {k: len(v) for k, v in sec.items() if isinstance(v, list)}
    return f"{path.name}: AIDT {pk.find('AIDT').raw_size} B: " + ", ".join(f"{v} {k}" for k, v in c.items())


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
    s = collect_map(m)
    if args.info:
        print(describe(m, s))
        return 0
    if not args.out:
        ap.error("-o is required")
    blob = build(s)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(hp2.build(args.input.stem, [("AIDT", blob)]))
    c = counts(s)
    print(f"wrote {args.out} (AIDT {len(blob) / 1024:.1f} KiB: {c['encounters']} encounters, {c['squads']} squads, "
          f"{c['starts']} start locations, {c['firings']} firing positions, {c['cmdlists']} command lists, "
          f"{c['variants']} variants)")
    for p in s.problems:
        print(f"  problem: {p}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
