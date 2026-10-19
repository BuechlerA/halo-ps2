# `AIDT` section, version 1 (actors, actor variants, encounters, command lists, conversations)

Output of `tools/halo2ps2/aiconv.py`, read by `engine/src/ai/aidef.c` (contract: `engine/src/ai/aidef.h`). Stored as the `AIDT` section of
`assets/hp2/<map>.ai.hp2` ([`.hp2` pack](07-hp2-format.md)). Little-endian, every table 16-byte aligned, offsets relative to the section start.
Cross references to other packs are **Xbox tag ids** (`u32`, 0 = none): the unit (`bipd`), the weapon (`weap`), sounds (`snd!`), animation graphs
(`antr`). Workstream I of [Phase 4](23-phase4-plan.md). Like the other Phase 3/4 sections the data is kept as the tags have it, in documented
units, and every field says how far it was verified: **[V]** verified (a numeric relation to other data, a cached value, an index range, a BSP
point location or a name confirms it), **[P]** plausible (units, ranges and the order of fields fit the public tag documentation, nothing in the data
contradicts it), **[U]** layout known, meaning unverified.

## What goes in
* **Actors** (`actor`, 1272 B each, sorted by tag id) and **actor variants** (`actor_variant`, 568 B each, sorted by tag id): every tag of the map, not
  only the placed ones. The record is the **whole tag block** with the tag references flattened to their tag id (below), so a field is at the same
  offset as in the Xbox tag and nothing is lost: the reader names the fields that are identified (perception, looking, firing and burst
  parameters, grenades, drops, vitality, references, flags) and exposes **every** word by tag offset (`aidef_actor_f32/u32/u16`,
  `aidef_variant_*`). Fields whose meaning is not proved (the behaviour thresholds of the actor) stay raw and are listed below with their kind and
  range.
* The scenario's **actor palette** (variant tag ids; a squad's actor type indexes it), **encounters** with their **squads**, **platoons**,
  **starting locations**, **move positions**, **firing positions** and **player starting locations**; **command lists** with their **commands**
  (atoms) and **points**; **AI conversations** with **participants** and **lines**; the **AI animation references** (name + `antr` graph) and **AI
  script references** (names).
* **Name tables** (sorted index arrays) for encounters, command lists and conversations; squads and platoons are found by name inside their
  encounter (`"encounter/squad"` paths). Scripts address all of these by name; the section keeps the names (case-insensitive lookup, ASCII).
* Not converted: the `change colors` block of variants (visual: its count is kept in the record, the 32-byte entries are dropped), AI recording
  references (empty on all ten maps, the converter refuses a map that has some), the stale tail bytes of names.

## Layout
Header, 256 B: `char[4] "AIDT"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (must equal the section size), `u32` reserved at `0x0C`, then
the **table directory** at `0x10`: for each of the 23 tables, in the order below, `u32 offset, u32 count` (8 B); the rest is zero.

| # | Table | Record | Max | Contents |
|---:|---|---:|---:|---|
| 0 | `actors` | 1272 B | 255 | tag image, strictly ascending tag id (ids in table 2) |
| 1 | `variants` | 568 B | 1024 | tag image (ids in table 3) |
| 2 | `actor_ids` | 4 | 255 | `u32` tag id of actor *i*, strictly ascending (binary search) |
| 3 | `variant_ids` | 4 | 1024 | likewise for variants |
| 4 | `palette` | 8 | 255 | `u32 variant_tag`, `u32 variant_index` (into `variants`, `0xFFFFFFFF` none) |
| 5 | `encounters` | 80 B | 1024 | scenario order |
| 6 | `squads` | 112 B | 16384 | encounter after encounter, squads in the encounter's order |
| 7 | `starts` | 28 B | 65536 | squad after squad |
| 8 | `moves` | 40 B | 65536 | squad after squad |
| 9 | `firings` | 20 B | 65536 | encounter after encounter |
| 10 | `platoons` | 48 B | 8192 | encounter after encounter |
| 11 | `pstarts` | 32 B | 4096 | encounter after encounter |
| 12 | `cmdlists` | 64 B | 1024 | scenario order |
| 13 | `commands` | 32 B | 65536 | list after list |
| 14 | `points` | 20 B | 65536 | list after list |
| 15 | `convs` | 64 B | 512 | scenario order |
| 16 | `parts` | 64 B | 4096 | conversation after conversation |
| 17 | `lines` | 48 B | 8192 | conversation after conversation |
| 18 | `anims` | 48 B | 256 | AI animation references, scenario order |
| 19 | `scripts` | 32 B | 256 | AI script references (names) |
| 20 | `enc_names` | 4 | 1024 | `u32` encounter indices sorted by lower-case name, ties by index |
| 21 | `cmd_names` | 4 | 1024 | likewise for command lists |
| 22 | `conv_names` | 4 | 512 | likewise for conversations |

A parent's children are a **contiguous range** of the child table (`first`, `count` fields) and the ranges of consecutive parents **tile the child
table exactly** (the reader checks it), so a child index is always inside its parent's range. Names are `char[32]`, NUL terminated, printable ASCII
(the Xbox tags keep stale bytes after the NUL: they are cut). Offsets below are inside the record. `f32` angles are radians; see Units.

**Palette**: `u32 variant_tag @0`, `u32 variant_index @4`.

**Encounter, 80 B**: `name @0`, `u32 flags @32` (`AIDEF_ENC_*`: bit 0 not initially created, bit 1 respawn enabled, 2 initially blind, 3 deaf, 4
braindead, 5 3D firing positions, 6 manual BSP index specified), `u16 team @36` (`AIDEF_TEAM_*`), `u16 search_behavior @38` (1 on every encounter
[U]), `u16 w28 @40` [U], `i16 manual_bsp @42`, `i16 precomputed_bsp @44`, `u32 first_squad, squad_count @48/52`, `first_platoon, platoon_count
@56/60`, `first_firing, firing_count @64/68`, `first_pstart, pstart_count @72/76`.

**Squad, 112 B**: `name @0`, `i16 actor_type @32` (palette index, -1 none), `i16 platoon @34` (index inside the encounter, -1), `u16 initial_state
@36`, `u16 return_state @38` (`AIDEF_STATE_*`), `u32 flags @40` (`AIDEF_SQUAD_*`), `u16 unique_leader @44`, `i16 maneuver_squad @46` (squad of the same
encounter, -1), `f32 delay_time @48` (s), `u32 masks[7] @52` (attacking, attacking search, attacking guard, defending, defending search, defending
guard, pursuing: 26-bit masks, `0x03FFFFFF` by default [U]), `u16 normal_count @80`, `u16 insane_count @82`, `u16 major_upgrade @84`, `u16
w82 @86, w84 @88, w86 @90` [U], `u32 first_start, start_count @92/96`, `first_move, move_count @100/104`.

**Starting location, 28 B** (where one actor of the squad is placed): `f32 pos[3] @0` (wu), `f32 facing @12`, `i16 cluster @16`, `i16 w12 @18`, `u16 w14
@20`, `u16 w16 @22`, `i16 w18 @24` [U], `i16 command_list @26` (index into the command lists, -1).
**Move position, 40 B**: `pos[3] @0`, `facing @12`, `weight @16`, `time_min @20`, `time_max @24` (s), `i16 animation @28` (-1 on all), `i16 sequence
@30`, `i16 cluster @32`, `i32 surface @36` (pathfinding surface of the BSP, -1 none).
**Firing position, 20 B**: `pos[3] @0`, `u16 group @12` [U], `i16 cluster @14`, `i32 surface @16`.
**Platoon, 48 B**: `name @0`, `u32 w20 @32`, `u16 w30 @36`, `i16 w32 @38`, `u16 w3c @40`, `i16 w3e @42` [U]. **Player start, 32 B**: `pos[3]`,
`facing @12`, `u16 team @16`, `u16 bsp @18`, `u16 types[4] @20` (zero on every map).
**Command list, 64 B**: `name @0`, `u32 flags @32` (bit 5 = manual BSP index specified [V]), `i16 manual_bsp @36`, `i16 precomputed_bsp @38`, `u32
first_command, command_count @40/44`, `first_point, point_count @48/52`.
**Command (atom), 32 B**: `u16 type @0` (`AIDEF_ATOM_*`, 0..27), `u16 modifier @2`, `f32 param1 @4`, `f32 param2 @8`, `i16 point1 @12`, `point2 @14`
(points of the list), `animation @16` (AI animation reference), `script @18` (AI script reference), `recording @20`, `command @22` (loop target),
`object_name @24` (scenario object name index; the table belongs to workstream D), all `-1` = none. **Point, 20 B**: `pos[3] @0`, `i32 surface @12`.
**Conversation, 64 B**: `name @0`, `u32 flags @32`, `f32 trigger_distance @36`, `f32 run_to_player_distance @40`, `first_participant,
participant_count @44/48`, `first_line, line_count @52/56`. **Participant, 64 B**: `u16 flags @0`, `w02 @2`, `selection_type @4`, `actor_type @6`
(`AIDEF_ACTOR_*`), `i16 use_object @8`, `new_name_object @10`, `i16 variants[6] @12` (dialogue variant numbers, -1 unused), `i32 encounter_index
@24` (-1 none), `char[32] encounter_name @28`. **Line, 48 B**: `u16 flags @0`, `i16 participant @2` (index inside the conversation, -1), `u16
addressee @4`, `i16 addressee_participant @6`, `f32 delay @8` (s), `u32 sounds[6] @12` (`snd!` tag ids of the alternative takes, 0 unused).
**AI animation reference, 48 B**: `name @0`, `u32 graph_tag @32`. **AI script reference, 32 B**: `name @0`.

## Actor (tag offsets = record offsets; flattened references: bytes `+0..+11` zero, the tag id at `+12`)
| Offset | Field | Unit / range on the 131 actors | |
|---|---|---|---|
| 0x00, 0x04 | `flags`, `more_flags` (u32) | bit meanings unnamed; the mounted-weapon actor carries stray ASCII in `more_flags` | [P] |
| 0x14 | `type` (u16, `AIDEF_ACTOR_*`) | 0..15, all 13 values that occur match the character class of the tag path | [V] |
| 0x18 | `max_vision_distance` | wu, 12..40 | [P] |
| 0x1C, 0x20, 0x28 | `central_vision_angle`, `max_vision_angle`, `peripheral_vision_angle` | rad: 0.873 (50 deg) / 1.222 (70 deg) / 2.094 (120 deg) on 123 actors, ascending on all; two actors use wide-open sentinels (2.0 / 2.0 / 20 or 40) | [P]/[V order] |
| 0x2C | `peripheral_distance` | wu, 0..6 | [P] |
| 0x34, 0x40 | `standing_gun_offset`, `crouching_gun_offset` (3 f32) | wu, x 0..0.28, y -0.23..0.095, z 0.37..0.55; the crouching one is zero on every actor | [P] |
| 0x4C | `hearing_distance` | wu, 20..70 | [P] |
| 0x50, 0x54 | `notice_projectile_chance`, `notice_vehicle_chance` | 0..1 | [P] |
| 0x60, 0x64, 0x68 | `combat_`, `guard_`, `non_combat_perception_time` | s, 0..0.9 / 0..1.8 / 0..3 | [P] |
| 0x6C, 0x70, 0x74 | `inv_*_perception_time` | cached `1 / (t * 30)`, equal to it to 1e-6 on all 131 | **[V]** |
| 0x80 | `fractions_80` (4 f32) | 0..0.85: behaviour thresholds | [U] |
| 0x90 | `values_90` (3 f32) | 0..5 | [U] |
| 0x9C, 0xA0 | `angle_9c`, `angle_9c_cos` | rad 0 or 0.698 (40 deg); the second is its cosine | **[V]** |
| 0xA4, 0xAC | `max_aiming_deviation`, `max_looking_deviation` (yaw, pitch) | rad, 0.70..3.14; cosines cached at 0x12C..0x138 (`cos_aiming_deviation`, `cos_looking_deviation`), equal to cos() to 1e-6 | **[V]** |
| 0xB4 | `looking_angles` (8 f32) | rad, 0..1.571: look deltas and idle ranges | [P] |
| 0xD4 | `idle_times` (10 x (min, max)) | s, 0..30, `min <= max` on all | [P] |
| 0x148, 0x264, 0x4C4 | weapon, projectile, actor reference ids | null on all 131 | [V] |

Words that exist but are not identified (all other words of the 1272 bytes are zero on all ten maps). Kind `f32` is a float, `u16` a 16-bit
value in the low half of the word; a `(min, max)` pair is two consecutive floats with `min <= max` on all 131 actors:

| Offsets | Kind | Range |
|---|---|---|
| 0x024 | stray | 0 on 124, a stray value on 7 |
| 0x268 | 2 x u16 | 0..4, 0..4 |
| 0x26C, 0x2A4, 0x2F8 | u16 | 0..4, 0..14, 0..5 |
| 0x270/0x274, 0x288/0x28C, 0x298/0x29C, 0x2D0/0x2D4 | (min, max) | 0..3 / 0..5; 0..20 / 0..25; 0..10 / 0..15; 0..6 / 0..7 |
| 0x278 | 2 x u16 | 0..3, 0..2 |
| 0x2A0, 0x2A8, 0x2AC, 0x2B0, 0x2D8, 0x2DC | f32 | 0..1, 0..1, 0..0.8, 0..17, 0..2, 0..0.8 |
| 0x2E0/0x2E4 | (min, max) | 0..0.8 / 0..0.95 |
| 0x2FC, 0x300, 0x304, 0x308, 0x30C | f32 | 0..3, 0..3, 0..6, 0..4, 0..5 |
| 0x310, 0x314, 0x318, 0x31C | f32 | 0..999 (999 = unlimited), 0..999, 0..0.9, 0..4 |
| 0x320, 0x324, 0x328, 0x32C, 0x330, 0x334 | f32 | 0..10, 0..0.3, 0..10, 0..7.5, 0..0.785 (45 deg), 0..5 |
| 0x33C/0x340, 0x344/0x348, 0x34C/0x350 | (min, max) | 0..14 / 0..18; 0..6 / 0..9; 0..3 / 0..9 |
| 0x354 | 2 x u16 | 0..8, 0..8 |
| 0x378..0x3A8 (13 words) | f32 | 0..8; the pair 0x384/0x388 is (min, max) |
| 0x3B8/0x3BC, 0x3C0/0x3C4, 0x3C8, 0x3CC | (min, max), (min, max), f32, f32 | 0..8 / 0..16; 0..6 / 0..8; 0..4; 0..5 |
| 0x3F8/0x3FC, 0x400/0x404 | (min, max) | 0..4 / 0..8; 0..2 / 0..5 |

These hold the actor's behaviour tuning: according to the public tag documentation the sections after perception and looking are, in this order,
danger / retreat triggers, panic (chances, cowering), defensive (hide / attack / pursue shield fractions), crouching and evasion thresholds, cover
(damage threshold, seek distance), search, melee leap, berserk, firing-position times and the grenade reference. **The assignment of the words
above to those names is not proved** (the tag data has no field names; the ranges, the pairs, the classes of character with larger or smaller
values fit it), so the converter and the reader do not name them: the AI runtime reads the ones it implements with `aidef_actor_f32(&a, 0x...)` and
documents which offset it took, and each such use should add its evidence here.

## Actor variant
| Offset | Field | Unit / range on the 297 variants | |
|---|---|---|---|
| 0x00 | `flags` | values 0, 1, 8, 16, 24, 48, 64, 112, 128 (bits 0, 3..7), bits unnamed | [U] |
| 0x10, 0x20, 0x30 | `actor_tag`, `unit_tag`, `major_variant_tag` | an actor and a biped on 268 of 297 (the other 29 have neither: vehicle-seat gunners and pilots), the stronger variant of the same character on 64 | **[V]** groups and presence |
| 0x4C | `movement_type` (u16) | 0..2, non-zero on 51 | [P] |
| 0x50, 0x54, 0x5C | `initial_crouch_chance`, `crouch_time`, `run_time` | 0..0.7; s bounds 0..8; s bounds 0..4 (set on 84 variants) | [P] |
| 0x70 | `weapon_tag` | `weap` (234 of 297, else null) | **[V]** |
| 0x74, 0x78, 0x7C | `max_firing_distance`, `rate_of_fire`, `projectile_error` | wu 0..140 (>100 only on vehicle gunners); rounds/s 0..10; rad 0..0.349 (20 deg) | [P] |
| 0x80, 0x88, 0x8C, 0x90 | `first_burst_delay` (bounds), `new_target_firing_pattern_time`, `surprise_delay_time`, `surprise_fire_wildly_time` | s, 0..6 / 0..15 / 0..3 / 0..0.6 | [P] |
| 0x94, 0x98 | `death_fire_wildly_chance`, `death_fire_wildly_time` | 0..0.25, s 0..3 | [P] |
| 0x9C | `desired_combat_range` (bounds) | wu, 0..70 | [P] |
| 0xA4, 0xB0 | `custom_stand_gun_offset`, `custom_crouch_gun_offset` | zero on all | [P] |
| 0xBC, 0xC0, 0xC4, 0xC8 | `target_tracking`, `target_leading`, `weapon_damage_modifier`, `damage_per_second` | 0..1, 0..1, 0..0.6, 0 on all | [P] |
| 0xCC, 0xD0, 0xD4, 0xDC | `burst_origin_radius` (wu), `burst_origin_angle`, `burst_return_length` (wu bounds), `burst_return_angle` | 0..0.75, rad 0..1.047, 0..1.25, rad 0..0.785 | [P] |
| 0xE0, 0xE8, 0xF0 | `burst_duration`, `burst_separation` (s bounds), `burst_angular_velocity` | 0..5, 0..6, rad 0..0.524 | [P] |
| 0xF8, 0xFC | `special_damage_modifier`, `special_projectile_error` | 0..0.4, 0 on all | [P] |
| 0x154, 0x158, 0x15C | `special_fire` (u16 mode 0..2, u16 situation 0..3), `special_fire_chance`, `special_fire_delay` | 0..1, s 0..15 (set on 37 variants) | [P] |
| 0x180 | `grenade_kind` (u16 x 4: type 0 fragmentation / 1 plasma, trajectory 0..2, stimulus 0..2, minimum enemies 0..3) | | [P] |
| 0x188, 0x190, 0x194, 0x19C | `grenade_enemy_radius` (wu), `grenade_velocity` (wu/tick: x / 30 = wu/s, 0..0.333), `grenade_ranges` (wu bounds, max 14), `grenade_collateral_radius` (wu) | | [P] |
| 0x1A0, 0x1A4, 0x1A8 | `grenade_chance`, `grenade_check_time`, `encounter_grenade_timeout` | 0..1, s 0..1, s 0..30 | [P] |
| 0x1CC | `equipment_tag` | null on all | [V] |
| 0x1D0, 0x1D4, 0x1D8, 0x1E0 | `grenade_count` (u16 min, max 0..3), `dont_drop_grenades_chance`, `drop_weapon_loaded` (bounds 0..1.2), `drop_weapon_ammo` (u16 min, max 0..600) | | [P] |
| 0x200, 0x204, 0x208 | `body_vitality`, `shield_vitality`, `shield_sapping_radius` | hit points 0..350 (non-zero on 276) / 0..300 (88): absolute values, the globals difficulty block scales them; radius 0 on all | [P] |
| 0x20C | `forced_shader_permutation` (u16) | 0..3 | [U] |
| 0x22C | `change_color_count` (count kept, pointer dropped) | 0..1 | [V] |

Unidentified non-zero words of the variant: `0x100`, `0x104`, `0x108`, `0x10C`, `0x118`, `0x11C`, `0x120`, `0x124`, `0x130`, `0x134`, `0x138`, `0x13C` (f32, 0..8:
three groups of four values, `0x138/0x13C` a (min, max) pair), `0x148`, `0x14C`, `0x150` (f32, set on 5 / 3 / 16 variants, 0..120), `0x160/0x164`,
`0x168/0x16C`, `0x170/0x174` (f32 pairs, 0..30; `0x170/0x174` is (min, max) on all 297) and `0x18C` (stray: 0 or 64 on 9). Six
vehicle-seat variants carry stale text in `0x180..0x18F` (uninitialised tag memory: a tag-reference shape with an impossible grenade type); the
converter **zeroes those 16 bytes** and reports it (one variant on a10, a30, b30, b40, c40, d40), so every grenade enum is in range.

## Units (the proofs)
| Quantity | Unit | Evidence |
|---|---|---|
| time | 30 ticks per second, fields in seconds | the actor's three perception times are cached as `1 / (t * 30)` at 0x6C..0x74 on all 131 actors |
| angles | radians (60-degree and 90-degree values are exact: 1.0472, 1.5708) | the cosines of the aiming / looking deviations are cached at 0x12C..0x138 and equal `cos()` of the angles to 1e-6; the stationary angle 0x9C has its cosine at 0xA0 |
| distances | world units | start, move and firing positions lie in the BSPs of the map (below) |
| grenade velocity | wu/tick | values are whole numbers over 30 (4 / 30, 5 / 30, 7 / 30, 10 / 30 wu/s) |
| fractions, chances | 0..1 | every such field is within it on all maps |
| difficulty counts | actors per squad | 0..32, at most the starting locations of the squad on 97 % of the squads |

## Source data (Xbox, engine version 5)
The offsets are in the docstring of `tools/halo2ps2/aiconv.py` (scenario +0x420 actor palette, +0x42C encounters, +0x438 command lists, +0x444 AI
animation references, +0x450 AI script references, +0x45C AI recording references, +0x468 conversations; every child block). Strides were derived
from the distance between consecutive blocks on all ten maps (the same method as `scenconv`).

| Fact | Status |
|---|---|
| Strides: palette 16, encounter 176, squad 232, platoon 172, firing position 24, player start 52, move position 80, starting location 28, command list 96, command 32, point 20, AI animation 60, AI script 40, conversation 116, participant 84, line 124 | **[V]** (identical on every map; the pointers of consecutive parents chain exactly) |
| Actor palette entries are `actv` references | **[V]** (every entry of every map) |
| Starting location `cluster` = the BSP cluster containing the position | **[V]** 9842 of 9902 located positions (the other 60 differ without a pattern we found, probably positions on BSP boundaries); 227 positions are outside every BSP |
| Firing position `cluster` | **[V]** 46154 of 46196 located (976 outside every BSP) |
| Move position `cluster` | **[V]** 1569 of 1591 located; `facing` in -pi..pi; `time_min <= time_max` on all 1649; `animation` -1 on all |
| Encounter flags bit 6 + `manual_bsp` | **[V]** 220 of 220: the BSP is one the start locations lie in |
| Encounter `precomputed_bsp` | **[V]** a BSP some start location lies in on 444 of 444 encounters that have located starts |
| Squad initial / return state 0..11 (the 12 actor states) | **[V]** range on all 2988 squads; the squads whose names say fleeing use 11, those whose names say searching mostly 10 |
| Squad `maneuver_squad` | **[V]** a squad of its own encounter on 317 of 325; the other 8 dangle (deleted squads) and are set to -1 and reported |
| Squad `platoon` | **[V]** inside the encounter's platoons except 3 dangling ones (set to -1) |
| Starting location `command_list` | **[V]** a valid index on all 1614 that set one |
| Command list flag bit 5 + `manual_bsp` | **[V]** 231 of 233 name a BSP of the list's points |
| Command atom types 0..27 | **[V]** type 13 (animate) carries a valid animation index on all 45 of its 47 atoms that set one, 12 (script) a valid script index (its 1 atom), 20 (loop) a valid command index of its own list on all 178, 14 (recording) never appears (there are no recordings); the names of the other types follow the public tag documentation [P]; point indices are not always valid (types that do not use them leave stale values) |
| Conversation participant `encounter_index` | **[V]** the encounter of `encounter_name` on 35 of 35 that name one; dialogue variant numbers 100 / 101 are variants of two biped tags |
| Line sounds are `snd!` references | **[V]** 445 references, all resolved |
| Encounter `team` | **[V]** 0..9; the biped `default_team` values (1 player, 2 human, 3 covenant, 4 flood, 5 sentinel) fix the names; encounters use only 0 (505) and 4 (1) |
| `AIDEF_ENC_RESPAWN_ENABLED` is never set; the squad respawn words (+0x84 / +0x86 of the squad record) are non-zero on 2 squads; all other squad bytes up to the child blocks are zero | respawn rules are **absent from the retail data** [V]; the two non-zero squads are kept raw [U] |
| `masks`, `w28`, `w82`, `w84`, `w86`, `w12`, `w14`, `w16`, `w18`, platoon words, `group` of firing positions, `selection_type`, participant `w02` | [U] kept raw |
| Spawn count per difficulty | **[U]** the tag has the normal and the insane count only; `aidef_squad_spawn_count` takes the normal count for easy and normal, the midpoint (rounded up) for hard, the insane count for impossible |

Every byte of the scenario's AI blocks that is not zero is captured by a named field (checked on all ten maps; the only non-captured bytes are
the stale tails of names and the unused tail of platoons).

## Anomalies in the retail data (what the converter does)
* **Dangling indices**: 8 `maneuver_squad` and 3 `platoon` indices point at squads / platoons that no longer exist (a10 3, a50 1, b30 2, b40 3, c40
  2): they become -1 and are listed in the converter's `problems`. Any index the reader would reject is sanitised the same way, so a converted pack
  always opens.
* **Stale bytes**: String32 fields keep the tail of an older, longer string after the NUL (cut); platoon blocks keep stale text after +0x40
  (ignored); six variants keep stale text at +0x180 (zeroed, above); the mounted-weapon actor keeps ASCII in `more_flags` (kept raw: flag bits
  of that actor are not interpreted).
* **Duplicate names**: a10 has 23 repeated command-list names, a10 / a50 / c40 have 1 / 1 / 4 repeated squad names inside an encounter; lookups
  return the lowest index (the sorted name tables break ties by index).
* **Dangling references**: none; every tag reference of the ten maps resolves (`problems` is empty for them).

## Reader (`engine/src/ai/aidef.[ch]`)
`aidef_open(sec, data, size)` -> `aidef_status` (`OK`, `ERR_TRUNCATED`, `ERR_MAGIC`, `ERR_LAYOUT`, `ERR_VERSION`). Typed accessors, each returning
1 when the record exists and a **neutral** struct (zero, indices -1) and 0 otherwise; a failed open leaves the section zeroed so every accessor
works on it. Pure C11, no heap, no `double`; the EE compiler accepts it with `-Wdouble-promotion`.
* actors / variants: `aidef_find_actor`, `aidef_get_actor(index)`, `aidef_actor(tag_id)`, `aidef_find_variant`, `aidef_get_variant`,
  `aidef_actor_variant(tag_id)`, `aidef_variant_actor(v)` (the actor a variant names), `aidef_variant_major(v)`; `aidef_actor_f32/u32/u16(a, tag_off)`
  and `aidef_variant_*` read any word of the raw record by tag offset (0 beyond the record or misaligned);
* palette: `aidef_palette_count/tag/variant`; `aidef_squad_variant(sq)` = palette[actor_type];
* encounters: `aidef_encounter_count`, `aidef_get_encounter`, `aidef_find_encounter(name)`; squads: `aidef_get_squad(global index)`,
  `aidef_encounter_squad(enc, k)`, `aidef_find_squad(enc, name)`, `aidef_find_squad_path("encounter/squad")`; `aidef_get_start/get_move(sq, k)`,
  `aidef_get_firing/get_platoon/get_pstart(enc, k)`, `aidef_find_platoon(enc, name)`; `aidef_squad_spawn_count(sq, difficulty)`;
* command lists: `aidef_command_list_count`, `aidef_get_command_list`, `aidef_find_command_list(name)`, `aidef_get_command(list, k)`,
  `aidef_get_point(list, k)`; conversations: `aidef_conversation_count`, `aidef_get_conversation`, `aidef_find_conversation(name)`,
  `aidef_get_participant(conv, k)`, `aidef_get_line(conv, k)`; `aidef_anim_ref_count/get_anim_ref`, `aidef_script_ref_count/name`;
* enums `aidef_actor_type`, `aidef_actor_state`, `aidef_team`, `aidef_atom_type`, flags `AIDEF_ENC_*` / `AIDEF_SQUAD_*`, and the debugging names
  `aidef_actor_type_name` / `state` / `team` / `atom`.

**Validation (`aidef_open`)**: size field (`TRUNCATED` if larger than the buffer, `LAYOUT` if smaller), signature, version; every directory entry:
count <= its maximum, offset 16-aligned, past the header, `count * record` inside the section (64-bit products); actor and variant id tables
strictly ascending and as long as the record tables; actors: type <= 15 and every word that is not a flag / enum / reference / u16 pair a finite
float; variants: the same, `movement_type`, `special_fire` and the grenade enums <= 7, minimum enemies <= 255, change colors <= 255; palette entries
name the variant they index; the child ranges of encounters, squads, command lists and conversations **tile** their tables exactly; encounter
`team` < 10; squads: printable terminated names, `actor_type` in -1..palette count, `platoon` and `maneuver_squad` inside the encounter, states <= 11,
leader <= 4, upgrade <= 4, counts <= 1000, finite delay; starting locations: finite floats, `command_list` in -1..lists; moves, firings, points, player
starts: finite floats; atoms: type <= 27, finite parameters; conversations: finite distances, line `participant` inside the conversation;
participants: `encounter_index` in -1..encounters, terminated names; names of platoons, command lists, conversations, animation and script references
terminated and printable; the three name tables are permutations-in-range sorted by name (case-insensitive) with ties by index.
`engine/tests/test_aidef.c` truncates at every length, corrupts every table of the directory and about 80 individual fields, and flips every byte
of the section three ways: a corrupt section is rejected or safe to walk completely (ASan / UBSan). Words that are not floats accept any bit
pattern (checked).

## Roots for the object pack and the sound converter
`aiconv.referenced_models(map)` returns the `model` tag ids of the unit and the weapon of every actor variant of the map, `referenced_anims(map)`
the `model_animations` graphs of the same plus the graphs of the AI animation references (the `animate` atoms), `referenced_sounds(map)` the `snd!`
ids of the conversation lines and of the `udlg` unit-dialogue tags of the variants' units (16-byte references from +0x10, up to 256 per tag; they are
the dialogue `soundconv` skips and `streamconv` streams). Counts (models / animation graphs / sounds): a10 9 / 5 / 864, a30 13 / 8 / 989, a50 14 / 11 /
996, b30 13 / 8 / 925, b40 13 / 7 / 939, c10 17 / 13 / 844, c20 14 / 8 / 9, c40 22 / 14 / 169, d20 19 / 11 / 154, d40 19 / 13 / 108. **The AI adds no
model that the scenario placements, `unitconv` and `weapconv` roots do not already cover** (0 on all ten maps), and adds animation graphs only
through the AI animation references (a50 5, b30 1, c10 1).

## Numbers
Conversion takes 0.02..0.09 s per map (it reads tags only). Section sizes (raw / in the pack): a10 131.6 / 64 KiB (9 actors, 16 variants, 39
encounters, 144 squads, 296 starting locations, 10 move positions, 2381 firing positions, 124 command lists with 558 commands and 270 points, 20
conversations), the largest b40 626.8 / 238 KiB (13 actors, 31 variants, 75 encounters, 985 squads, 1103 starting locations, 591 move positions,
20789 firing positions, 32 command lists). The firing positions are the bulk (20 B each: 406 KiB on b40, 46 KiB on a10); everything else together is
below 215 KiB on every map.

| map | actors | variants | palette | encounters | squads | starts | moves | firings | platoons | cmd lists | commands | points | convs | lines | AIDT KiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 9 | 16 | 18 | 39 | 144 | 296 | 10 | 2381 | 41 | 124 | 558 | 270 | 20 | 23 | 131.6 |
| a30 | 13 | 21 | 10 | 22 | 179 | 390 | 3 | 4645 | 72 | 3 | 8 | 6 | 59 | 81 | 167.8 |
| a50 | 15 | 24 | 34 | 33 | 530 | 1180 | 337 | 4976 | 77 | 54 | 203 | 104 | 44 | 122 | 267.8 |
| b30 | 13 | 22 | 10 | 32 | 115 | 280 | 143 | 4470 | 39 | 7 | 16 | 8 | 19 | 55 | 154.6 |
| b40 | 13 | 31 | 19 | 75 | 985 | 1103 | 591 | 20789 | 84 | 32 | 127 | 93 | 25 | 46 | 626.8 |
| c10 | 16 | 37 | 34 | 39 | 255 | 2938 | 512 | 2293 | 22 | 76 | 358 | 215 | 5 | 22 | 240.8 |
| c20 | 7 | 24 | 46 | 81 | 169 | 1096 | 0 | 1560 | 25 | 98 | 542 | 328 | 0 | 0 | 139.4 |
| c40 | 19 | 47 | 39 | 67 | 247 | 836 | 38 | 2746 | 56 | 56 | 174 | 97 | 5 | 12 | 176.0 |
| d20 | 14 | 40 | 64 | 62 | 172 | 868 | 13 | 1554 | 36 | 33 | 102 | 43 | 0 | 0 | 127.0 |
| d40 | 12 | 35 | 35 | 56 | 192 | 1142 | 2 | 1758 | 31 | 69 | 237 | 145 | 0 | 0 | 142.6 |

Also on the ten maps: 35 player starting locations of encounters, 483 platoons, 327 participants, 23 AI animation references (a10 6, a50 9, b30 3,
c10 4, d40 1), 2 AI script references (b30, c40). Aggregates: squads start `guarding at guard position` 1208, `searching` 992, `none` 242, `alert`
229, `moving randomly` 179, `sleeping` 56, `fleeing` 35 and 47 in the other moving / guarding states; the automatic-migration squad flag (bit 5)
is set on 1057 of the 2988; the atom histogram is dominated by go to (766), loop (178), pause (177), crouch (145), animation mode (122), look random (105),
move immediate (98).

## Integration notes
* `scripts/build-world.sh` should call `aiconv.py <map> -o assets/hp2/<map>.ai.hp2` (the pack holds one section, `AIDT`); the AI runtime loads it
  into the **persistent arena** (a10: 132 KiB, b40 627 KiB raw) next to the UNIT, WEAP, SND0 and HUD data.
* The data refers to BSPs by index in the order of the scenario's BSP list (the same order as `BSPn` / `COLn`) and to pathfinding surfaces of those
  BSPs by index (workstream P defines how surfaces map to its sectors: `surface` is -1 or an index below the BSP's pathfinding surface count).
* Object-name indices in atoms and participants (`object_name`, `use_object`, `new_name_object`) index the scenario's object-name table (workstream D).
* Squad spawning: a squad places `aidef_squad_spawn_count(sq, difficulty)` actors at its starting locations; the data has more starting locations
  than the Legendary count on 97 % of the squads and fewer on the other 3 % (the tag does not say whether a start location is reused: [U]).
* The variant's `weapon_tag` and the unit's own default weapon (`unitdef`) are different data: the variant's wins when non-null.

## Tests
`tools/tests/test_aiconv.py` builds a synthetic scenario (3 actors, 4 variants with every reference kind, a stale-bytes variant, a palette, 3
encounters with squads / platoons / starts / moves / firing positions / player starts, 4 command lists with a repeated name, 2 conversations, AI
animation and script references, units, weapons, dialogue and sound tags) and checks collection field by field, the flattened references, the
sanitising of dangling indices and stale bytes, the roots, the writer and its reference parser (name tables, record offsets) and that the sizes in
`aidef.c` equal the converter's; corrupt maps (reflexives outside the region, recordings, a bad actor type, a wrong-group palette entry, NaN
positions, a non-finite float word); then converts all ten campaign maps when `assets/decompressed/` exists and checks the ranges, units and
indices of the tables above (cached-value proofs, cones, bounds, indices, groups of every reference, roots). It writes the pack
`engine/tests/test_aidef.c` reads (`make -C engine/tests`, with the converted real packs as extra arguments when present).
