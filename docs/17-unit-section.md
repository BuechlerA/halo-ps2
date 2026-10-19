# `UNIT` section, version 1 (globals player constants, materials, bipeds, vehicles' basic data)

Output of `tools/halo2ps2/unitconv.py`, read by `engine/src/sim/unitdef.c` (contract: `engine/src/sim/unitdef.h`).
Stored as the `UNIT` section of `assets/hp2/<map>.unit.hp2` ([`.hp2` pack](07-hp2-format.md)). Little-endian, every
table 16-byte aligned, offsets relative to the section start. Cross references are **Xbox tag ids** (`u32`, 0 = none):
model, animation graph, collision model, physics, weapons, dialogue, sounds, damage effects: the other packs resolve them.
Workstream C of [Phase 3](14-phase3-plan.md).

## What goes in
* **Globals** (1 record, 832 B): from the `globals` tag: player information (speeds), player control (look), falling
  damage (and the **gravity** derived from it), grenades, difficulty, the default player biped, two water sounds, the
  default camera track.
* **Materials** (32 x 48 B): the per-material block of `globals` (melee hit sound, ...). The block carries **no per-material
  impact response** (see below), so that part of the contract is empty by data.
* **Bipeds** (576 B each, sorted by tag id): every `biped` tag of the map (14 on a10, not only the placed ones: AI units are
  spawned later): model / animation graph / collision model / physics / camera track / footsteps / melee damage ids, default
  weapons (<= 4), dialogue variants (<= 8), camera, collision pill, camera heights, jump, slopes, turning, landing
  thresholds, the tag's own motion block (zero for walkers), health / shield / recharge (from the **collision model**), hold
  and foot marker names, and the walking speeds **derived from the animation graph's root motion**.
* **Vehicles** (256 B each, sorted by tag id) and **seats** (128 B each): the minimum to place a vehicle later, see below.

## Layout
Header, 128 B: `char[4] "UNIT"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (= section size), `u32 biped count` (<= 255),
`u32 vehicle count` (<= 255), `u32 material count` (<= 64), `u32 seat count` (<= 1024), table offsets `globals 0x1C`, `bipeds
0x20`, `vehicles 0x24`, `materials 0x28`, `seats 0x2C`, `u32 player biped tag id 0x30` (the globals' default biped),
`u32 its index + 1 in the biped table 0x34` (0 when the map has no such biped); the rest is zero.

Every record is a list of fields in this order with natural alignment (floats and `u32` at 4, `u16` at 2); the tables
below give the byte offsets the C reader hard-codes (`unitconv.py`'s `*_FIELDS` lists are the single source, the tests
assert the offsets).

**Globals, 832 B**: `0x00 u32 default_biped_tag, coop_effect_tag, camera_track_tag, water_enter_sound, water_exit_sound`;
`0x20 f32` player information: `walk_speed, run_forward, run_backward, run_sideways, run_accel, sneak_forward,
sneak_backward, sneak_sideways, sneak_accel, airborne_accel, pi_unk_2c, pi_unk_6c, stun_move_penalty, stun_turn_penalty,
stun_jump_penalty, stun_min_time, stun_max_time, fp_idle_min, fp_idle_max, fp_skip_fraction` (20 floats, `0x20..0x6F`);
`0x70 f32` player control: `magnetism_friction, magnetism_adhesion, inconsequential_scale, look_a, look_b, look_c,
look_pitch_rate_deg, look_yaw_rate_deg, look_f`; `0x94 u16 min_weapon_swap_ticks, min_autolevel_ticks`; `0x98 f32
min_vehicle_flip_angle`; `0x9C f32[6] look_function`; `0xB4 f32` falling damage: `harmful_fall_min, harmful_fall_max,
max_fall_distance, gravity, fall_velocity_max, fall_velocity_harm_min, fall_velocity_harm_max`; `0xD0 u32[6] damage_tags`
(`jpt!`: falling, distance, vehicle environment collision, vehicle killed unit, vehicle collision, flaming death); `0xF0`
grenades: 2 x 32 B (`u16 max_count, u16 unk, u32 throw_effect, hud_interface, equipment, projectile`; 0 frag, 1 plasma);
`0x130 f32[124]` difficulty = 31 groups x 4 levels (easy, normal, hard, impossible), group g at `0x130 + 16 g`.

**Biped, 576 B**: `0x000 u32 tag_id, model_tag, anim_tag, collision_tag, physics_tag, camera_track_tag, footsteps_tag,
melee_damage_tag`; `0x020 u32[4] weapon_tags`; `0x030 u32[16] dialogue` (8 x `variant number, udlg tag id`); `0x070 f32`:
`bounding_radius, bounding_offset[3], render_radius, accel_scale, camera_fov, camera_stiffness, pitch_auto_level,
pitch_range[2], stand_height (0x9C), crouch_height, radius (0xA4), autoaim_width, stand_camera (0xAC), crouch_camera,
crouch_time, jump_velocity (0xB8), slope[7] (0xBC: max slope, downhill falloff / cutoff / velocity scale, uphill falloff /
cutoff / velocity scale), turn_speed (0xD8), turn_threshold, landing[6] (0xE0: soft time, hard time, min soft velocity,
min hard velocity, max hard velocity, death velocity), tag_motion[7] (0xF8: max velocity, max sidestep, acceleration,
deceleration, angular velocity max, angular acceleration max, crouch velocity modifier), aim[5] (0x114: aiming velocity /
acceleration max, casual aiming modifier, looking velocity / acceleration max), anim_speed[4] (0x128: stand run front /
back / side, crouch front), grenade_velocity, health (0x13C), shield, shield_recharge_time, shield_stun_time,
shield_recharge_rate (0x14C)`; `0x150 u32 unit_flags, biped_flags, coll_flags`; `0x15C u16 object_flags, default_team,
pelvis_node, head_node, shield_material, body_material, grenade_type, grenade_count`; `0x16C u8 weapon_count,
dialogue_count, 2 pad`; `0x170 char[32] name, hold_marker, aux_marker, camera_marker`; `0x1F0 char[32] x 2` foot markers.

**Vehicle, 256 B**: `0x00 u32 tag_id, model_tag, anim_tag, collision_tag, physics_tag, camera_track_tag`; `0x18 u32[4]
weapon_tags`; `0x28 f32 bounding_radius, bounding_offset[3], accel_scale, camera_fov, max_forward (0x40), max_reverse,
accel, decel, max_left_turn, max_right_turn, wheel_circumference, turn_rate, health (0x60), shield,
shield_recharge_time`; `0x6C u32 unit_flags, vehicle_flags, coll_flags`; `0x78 u16 type, first_seat, seat_count,
object_flags, default_team, shield_material`; `0x84 u8 weapon_count, pad`; `0x86 char[32] name`.
**Seat, 128 B**: `u32 flags, f32 pitch_min, f32 pitch_max, u32 pad, char[32] label, marker, camera_marker`.
**Material, 48 B**: `u32 melee_sound, impact_sound, effect, particle_tag, f32 param_a, param_b, u16 particle_count, u16 pad`.

## Units (the proofs)
| Quantity | Unit | Evidence |
|---|---|---|
| time | 30 ticks per second | the cached falling velocities below |
| world unit | 3.048 m (10 ft) | same: g = 9.78 m/s^2 |
| **gravity** | **0.0035652 wu/tick^2** (3.2087 wu/s^2) | `globals` falling damage caches `sqrt(2 g d)` for d = 16.5, 3 and 6 wu: 0.3430, 0.14626, 0.20684; the three give the same g to 1e-4 on all ten maps (the converter reports a problem otherwise) |
| player speeds (`run_forward` ...) | wu per **second** | the cyborg's `stand rifle move-front` animation has 1.5 wu of root motion in 20 frames = 0.075 wu/tick = 2.25 wu/s = `run_forward`; back / side 1.34 wu in 20 frames = 2.01 = 2.0; the crouch cycles' 0.9 / -0.65 / 0.6 wu in 30 frames (1 s) = the sneak values 0.9 / 0.65 / 0.6. The test asserts it on every map |
| `jump_velocity` | wu per **tick** (0.07 = 2.1 wu/s) | Halo tag convention; with g the apex is 0.687 wu (0.98 wu if `accel_scale` 0.7 scales gravity: unverified) = 2.1 m / 3.0 m for a 2.13 m tall player (standing collision height 0.7 wu) |
| landing velocities, shield / stun / transition times | wu per second, seconds | the hard-landing threshold 5 wu/s sits in the middle of the harmful falling distances 3..6 wu (= 4.39..6.2 wu/s at g); `crouch_time` 0.2 s is cached as 1 / (0.2 x 30) = 0.1667 per tick at +0x4CC on every biped; the shield recharge time (s) is cached as `1 / (time x 30)` at +0x1C0 on every biped with a shield |
| vehicle speeds, accelerations | wu per tick, wu per tick^2 (plausibility, not measured) | warthog 0.255 wu/tick = 7.65 wu/s = 23 m/s, reached in 93 ticks (3.1 s) at 0.00275; ghost .225, banshee .22, scorpion .14; a per-second reading (0.78 m/s) is absurd |
| angles | radians (look rates, vehicle turn angles: **degrees**) | slopes 0.7854 = 45 deg and their cached cosines/sines; `look_pitch_rate_deg` = 60, yaw 120; warthog turn +-30 |
| player accelerations (`run_accel` .32, `sneak_accel` .16, `airborne_accel` .035) | **unproved** | as a per-tick blend factor towards the target speed they give full speed in about 9 ticks (plausible); as wu/tick^2 the ramp would be instantaneous, as wu/s^2 it would take 7 s |

## Source data (Xbox, engine version 5; verified unless marked)
The offsets of every field are in the docstring of `tools/halo2ps2/unitconv.py` with their evidence. In short
(all ten campaign maps, a10 in depth; "[V]" verified, "[P]" plausible, "[U]" meaning unknown):

| Fact | Status |
|---|---|
| `globals`: reflexives at +0x0F8 sounds, +0x104 camera, +0x110 player control (128 B), +0x11C difficulty (644 B), +0x128 grenades (68 B), +0x170 player information, +0x188 falling damage (152 B), +0x194 materials (884 B, 32 on every map); identical numbers on all ten maps (only tag ids differ) | [V] sizes from block distances, references by path |
| Player information: run / sneak speeds | [V] against the cyborg's root motion |
| Player information: `walk_speed` (+0x30, 1.25), stun penalties, first-person idle bounds | [P] ranges and names |
| Player information: +0x2C (0.512), +0x6C (0.05) | [U] stored as `pi_unk_2c`, `pi_unk_6c` |
| Player control: look rates (60 / 120 deg/s), magnetism, minimum swap / autolevel ticks, flip angle, look function (0 .05 .1 .25 .58 1) | [P] (the two rates identified by magnitude) |
| Player control: +0x40..+0x54 other four floats (1.0, 3.0, 0.85, 0.5) | [U] "look acceleration group", order unverified (`look_a/b/c/f`) |
| Difficulty: groups 0..8, 10..21, 23, 24 (enemy / friend damage, vitality, shield, recharge, infection forms, rate of fire, projectile error, burstiness, target delays, tracking, leading, overcharge, special fire, guidance, melee delay, grenade chance / timer) | [V] by the monotonic easy..impossible patterns; groups 9, 22, 25..27 are zero; 28..30 (.15 .25 .5 .75 / .05 .1 .3 .5 / .25 .5 .75 1) [U] |
| Grenades: references (effect, HUD interface, equipment, projectile) | [V] paths; `max_count` 4 [P]; the second u16 is 0 [U] |
| Falling damage: distances, cached velocities, the six damage effects in order | [V] distances / velocities; effect order [P] (paths are not names we can read) |
| Materials: melee hit sound, sweetener sound (glass), particle effect (glass), two unknown floats (ice 0.125, glass 10) | [V] melee sounds follow the standard material order (dirt, sand, stone, snow, wood, metal x3, rubber, glass, force field, grunt, ...); the collision models' shield material indices 22 / 23 / 30 / 16 are the cyborg / human armor / elite / jackal shield entries. The 33rd standard entry (hunter shield) is absent: 32 on all maps |
| Object: model +0x28, graph +0x38, collision model +0x70, physics +0x80, bounding radius / offset, acceleration scale (cyborg .7, elite .6, grunt .9, marine 1), render radius | [V] groups, paths and the render radius equal to the bounding radius on the cyborg; the meaning of the acceleration scale [U] |
| Unit: flags, team, camera FOV (70 deg on every unit but one), camera track, default weapons (36 B, reference at +0), dialogue variants (24 B, `udlg` at +0x08), melee damage, grenade velocity / type / count, AI aiming / looking limits | [V] references; the numbers [P]. Camera marker names are empty on every biped; `pitch_range` and `pitch_auto_level` are 0 on every unit of the ten maps: **the player's look limits are not in the tags** (the cyborg's animation graph has an aiming screen of +-70 deg yaw / 45 deg pitch, which is the animation overlay range, not a camera limit) |
| Biped: turning speed / threshold, flags (bit 9 "use player physics" on every walker), slopes, footsteps `foot`, jump velocity, landing block, camera heights, crouch time, collision heights / radius, pelvis and head node, contact points | [V]: slope sines and cosines are cached in the tag (+0x4C8..+0x4E0), crouch rate cached, pelvis / head index "bip01 pelvis" / "bip01 head" on 114 of 143 bipeds (the other 29 use 0xFFFF: flood, sentinels, monitor, turrets), standing height 0.7 = the cyborg model's height |
| Biped motion block (+0x324..+0x34C: max velocity, acceleration, ...): zero on every walking biped; set on the engineer (max velocity / sidestep 1.8 / 1.4), sentinel (2.25 / 1.8) and monitor (4.0 / 4.0), accelerations .06, .06, .2, i.e. the flyers | [V] layout, unit [U] (flyers) |
| Collision model vitals: `maximum body vitality` +0x08, `maximum shield vitality` +0xCC, shield material +0xD2, stun time +0x10C, recharge time +0x110 | [V] the cached rate +0x1C0 = 1 / (recharge x 30) on every biped with a shield; values 75 / 75 (cyborg), 100 / 100 (elite), 50 / 50 (marine), 40 / 0 (grunt), 25 / 0 (crewman), 200 / 0 (hunter), 60 / 100 (jackal), 80 / 80 (sentinel); the monitor's shield is 1e10 and its health 0 (invulnerable) |
| Vehicle: type enum (tank 0 ... turret 6), speeds, turns, wheel circumference, turn rate, seats (284 B: flags, label, marker, camera marker +0x84, pitch bounds +0xC8 / +0xCC) | [V] types by name (scorpion 0, warthog 1, pelican 3, ghost 4, banshee 5, gun turret 6); numbers [P]; flyers' turn fields use other units |
| `anim_speed`, `hold_marker`, `aux_marker` (graph: stand unit first, first non-empty +0x40 / +0x20 of the weapon blocks) | derived: the first matching animation by weapon preference `rifle`, `unarmed`, any; the sideways value is the mean of left and right. AI bipeds' values (marine 2.10 / 1.50 / 1.75 / 0.98, elite 2.60, grunt 1.88) are root motion of the first such animation and approximate |

### Vehicles: what is extracted and why
To **place** a vehicle and seat its occupants later the engine needs: the model, animation graph, collision model and physics tag ids,
the bounding sphere, `type` and flags (`vehicle_flags` as the tag has them), speeds / acceleration / turn limits, default weapons,
and per seat the flags (driver, gunner, ...), label, marker name the occupant attaches to, camera marker and camera pitch limits.
**Not extracted** (a Warthog that drives needs them, a later workstream): the `physics` tag contents (mass points, powered masses),
per-type tuning (jeep steering overdamp / torque / suspension, hover and flight parameters, tank turret rates), AI vehicle data,
powered seats, seat HUD and acceleration scales, boost. The `vehicle` tag group on the maps also holds devices and scenery (chairs,
cryotube, docked lifepods, the ring: type `turret`, no speed): `is_driveable` (speed > 0 or a seat with a non-turret type) separates the
real vehicles for the model roots. a10: 13 `vehicle` tags, 8 driveable, 44 seats.

## Reader (`engine/src/sim/unitdef.[ch]`)
`unitdef_open(sec, data, size)` -> `unitdef_status` (`OK`, `ERR_TRUNCATED`, `ERR_MAGIC`, `ERR_LAYOUT`, `ERR_VERSION`);
`unitdef_globals(sec, &out)`, `unitdef_difficulty(sec, group, level)`, `unitdef_biped(sec, tag_id, &out)` (+ `find_biped`,
`get_biped`, `unitdef_player_biped`), `unitdef_vehicle(sec, tag_id, &out)` / `unitdef_vehicle_seat(sec, &veh, i, &out)`,
`unitdef_material(sec, index, &out)` and `unitdef_material_name(index)` (the standard names, for debugging: engine knowledge, not tag
data). Absent records give a neutral struct (zero, nodes `UNITDEF_NO_NODE`) and a return of 0; a failed open leaves the section
zeroed, so every accessor works on it. Pure C11, no heap, no `double`.

**Validation (`unitdef_open`)**: size field (`TRUNCATED` if larger than the buffer, `LAYOUT` if smaller), signature, version;
counts (<= 255 / 255 / 64 / 1024); every table 16-aligned, past the header and inside the section (64-bit products); globals:
all float blocks finite, `gravity` finite and positive; bipeds: strictly ascending tag ids, finite floats, non-negative radius /
heights / vitals / bounding radius, `weapon_count <= 4`, `dialogue_count <= 8`, dialogue variants <= 0xFFFF, every string
NUL-terminated; vehicles: ascending ids, finite floats, non-negative bounding radius, `type <= 6`, `first_seat + seat_count`
inside the seat table, `weapon_count <= 4`, terminated name; seats: finite pitch bounds, terminated strings; materials: finite
floats; the player index is `<=` the biped count and names the player tag. `engine/tests/test_unitdef.c` truncates at every length,
corrupts each table, about 40 individual fields, and flips every byte of the section three ways: a corrupt section is rejected or safe to
walk completely (ASan / UBSan). With a real pack as second argument (the Makefile passes `assets/hp2/a10.unit.hp2` when it exists)
it checks the plausibility ranges on real data.

## Roots for the object pack
`unitconv.referenced_models(map_path, vehicles=True)` and `referenced_anims(...)` return the `model` / `model_animations` tag ids of every
biped (and, with `vehicles`, every driveable vehicle), keyed like `modelconv` / `animconv`. They add little to what scenarios
place (new models beyond `scenconv`'s set): a10 2, a30 6, a50 4, b30 6, b40 2, c10 4, c20 3, c40 7, d20 6, d40 4 (animation graphs: 2, 4, 3,
5, 1, 3, 3, 6, 5, 4); the vehicles add none (they are all placed on those maps).

## Numbers
Conversion takes about 0.07 s per map (it reads tags only). `UNIT` section: a10 19.1 KiB (14 bipeds, 13 vehicles, 44 seats; the
largest), a30 15.9, a50 15.4, b30 14.5, b40 16.1, c10 14.7, c20 6.9, c40 16.2, d20 14.5, d40 18.6 KiB; the synthetic test world 3.3 KiB.

## Tests
`tools/tests/test_unitconv.py` builds a synthetic map (globals with every block, two bipeds with collision models, a graph with
move animations, a warthog and a chair with seats, 3 materials), checks every field, the roots, the writer and its reference parser
and its record offsets, the problem reporting (inconsistent gravity, inconsistent recharge cache, unresolved reference) and corrupt
maps (reflexive limits and bounds, object type, NaN, vehicle type, missing blocks, two globals); then converts all ten campaign
maps when `assets/decompressed/` exists (plausibility ranges, the run-speed proof, vitals cache, pelvis / head node names, roots).
It writes the pack `engine/tests/test_unitdef.c` reads (`make -C engine/tests`).

## The player biped on a10 (facts for the player controller)
`characters\cyborg\cyborg`, selected by `globals` as the default biped; identical on all ten maps. Speeds are wu/s unless stated.
| Fact | Value | Source |
|---|---|---|
| run forward / backward / sideways | 2.25 / 2.0 / 2.0 (0.075 / 0.0667 / 0.0667 wu/tick) | globals, verified against root motion |
| sneak (crouch) forward / backward / sideways | 0.9 / 0.65 / 0.6 | globals, verified |
| walking speed (partial stick?) | 1.25 | globals, meaning unverified |
| run / sneak / airborne acceleration | 0.32 / 0.16 / 0.035 | globals, unit unproved (see Units) |
| jump velocity | 0.07 wu/tick (2.1 wu/s); apex 0.687 wu, 39 ticks in the air at g; 0.98 wu / 56 ticks if the 0.7 acceleration scale scales gravity | biped, g derived |
| gravity | 0.0035652 wu/tick^2 = 3.2087 wu/s^2 | globals falling damage |
| acceleration scale | 0.7 (marine 1, elite 0.6) | object +0x20, meaning unverified |
| collision radius / standing height / crouching height | 0.2 / 0.7 / 0.5 wu (pill from the feet) | biped; the model is 0.7 high |
| bounding radius / offset | 0.45 / (-0.02, 0, 0) | object |
| camera height standing / crouching, transition | 0.62 / 0.35 wu above the feet, 0.2 s | biped (head node 0.61 in the bind pose) |
| field of view | 1.2217 rad = 70 deg | unit |
| health / shield (maximum vitality) | 75 / 75 | collision model |
| shield recharge time / stun time / material | 4 s (0.00833 per tick) / 6 s (meaning of the stun time unverified) / 22 (cyborg energy shield) | collision model |
| max slope, downhill falloff / cutoff / scale, uphill falloff / cutoff / scale | 45 deg, 20 / 45 deg / 1.25, 20 / 45 deg / 0.65 | biped |
| landing: soft / hard time, min soft / min hard / max hard velocity | 0.6 s / 1.0 s, 1.5 / 5 / 8 wu/s, death velocity 0 (unused) | biped |
| harmful falling distance, max falling distance | 3..6 wu, 16.5 wu | globals |
| moving turning speed, stationary turning threshold | 5.894 rad/s, 1.396 rad (80 deg) | biped (not the player's look rate) |
| stun penalties (movement / turning / jumping), max stun time | 0.8 / 0.8 / 1.0, 1.25 s | globals, [P] |
| look rates pitch / yaw (player control) | 60 / 120 deg/s | globals, [P]; look limits (pitch range) are not in the tags |
| magnetism friction / adhesion, minimum weapon swap | 0.6 / 0.7, 7 ticks | globals |
| grenades | frag velocity 10, type 0, count 4; globals: 2 types (frag, plasma), max 4 each | biped, globals |
| default weapons | assault rifle, pistol | biped weapon list |
| hold marker / foot markers / pelvis, head node | `right hand` (+ `left hand gun grip`) / `left foot`, `right foot` / 0 `bip01 pelvis`, 12 `bip01 head` | graph, biped |
| flags | unit 0x00448008, biped 0x202 (bit 9: the walking speeds come from globals, not from the biped) | tag |
