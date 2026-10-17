# `WEAP` section, version 1 (weapons, projectiles, damage effects, equipment, grenades, item collections)

Output of `tools/halo2ps2/weapconv.py`, read by `engine/src/sim/weapdef.c` (contract: `engine/src/sim/weapdef.h`),
which also holds the pure-C weapon state machine `weapon_fire_step`. Stored as the `WEAP` section of
`assets/hp2/<map>.weap.hp2` (a [`.hp2` pack](07-hp2-format.md)). Little-endian, every table 16-byte aligned, all offsets
relative to the start of the section, no pointers: records refer to each other by index ranges and to everything else by
**Xbox tag id** (u32, 0 = none), the same key the `MODL`, `ANIM`, `SND0`, `FXD0` and `HUD0` sections use.
Workstream D of [Phase 3](14-phase3-plan.md).

## What goes in
Everything of these groups that the map's tag index contains (not only what the scenario places: vehicle guns, the
Warthog's turret and the like are weapons too):

| Source tag | Table | Notes |
|---|---|---|
| `weapon` | weapons (+ magazines, triggers, firing effects) | trigger rate, error, ammunition, projectile, charge, heat, battery, zoom, aim assist, first person / world model and animation ids, HUD interface id, sounds, melee damage, first person animation timing |
| `projectile` | projectiles (+ material responses) | physics, guidance, arming, detonation, flyby sound, damage tags, one response per material type |
| `damage_effect` | damage effects | amounts, radii, category, side effect, flags, camera impulse and shake, screen flash, rumble, material modifiers |
| `equipment` | equipment | powerups, grenade pickups, ammunition pickups |
| `globals` (`matg`) | grenades | the grenade table (type, maximum count, equipment, projectile, throwing effect, HUD) |
| `item_collection` | collections (+ permutations) | **no campaign map has one** (zero tags on all ten), layout unverified, see below |

## Layout
Header, 128 bytes: `char[4] "WEAP"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (must equal the section size),
then 11 `u32` counts at `0x0C` and 11 `u32` table offsets at `0x38`, in the table order of the next table.

| Table | Record | Count |
|---|---|---|
| weapons | 256 B | weapons, **strictly ascending** tag id |
| magazines | 64 B | all magazines, weapon after weapon (a weapon owns `first, count`) |
| triggers | 128 B | all triggers, same scheme |
| fx | 32 B | all firing effects, trigger after trigger |
| projectiles | 192 B | projectiles, strictly ascending tag id |
| materials | 64 B | all material responses, projectile after projectile (material index = position) |
| damages | 384 B | damage effects, strictly ascending tag id |
| equipment | 96 B | equipment, strictly ascending tag id |
| grenades | 32 B | the globals' grenade table (**index = grenade type**: 0 frag, 1 plasma) |
| collections | 32 B | item collections, ascending tag id (`u32 tag, first, count, spawn time`) |
| permutations | 32 B | `f32 weight, u32 item tag` |

Indices into weapons / magazines / triggers / fx / projectiles / materials / damages / equipment are `u16` (a count above
65535 is refused by the writer and the reader).

**Weapon, 256 B** (`u32` unless noted): `0x00 tag_id`, `model_tag` (world / third person), `anim_tag` (its graph, 0 on every
retail weapon), `fp_model_tag`, `fp_anim_tag` (one graph animates hands and gun, docs/13), `hud_tag` (`wphi`),
`pickup_sound`, `zoom_in_sound`, `zoom_out_sound`, `collision_sound`, `ready_effect`, `overheated_effect`,
`detonation_effect` (heat), `melee_damage`, `melee_response`, `0x3C reserved`, `0x40 flags`, `0x44 item_flags`;
`0x48 u16 secondary_mode, max_alternate_shots, weapon_type, zoom_levels`; `0x50 f32 zoom_min, zoom_max, heat_recovery,
overheated, heat_detonation, heat_detonation_fraction, heat_loss (per second), heat_illumination, near_reticle,
far_reticle, intersection_reticle, autoaim_angle, autoaim_range, magnetism_angle, magnetism_range, deviation_angle,
camo_ding, camo_regrowth, item_scale, detonation_delay[2]`; `0xA4 u16 hud_message, sort_order, first_magazine,
magazine_count, first_trigger, trigger_count`; `0xB0 u16 fp_ticks[8]` = first person animation lengths in ticks (ready,
put away, reload empty, reload full, melee, overheated, throw, fire; 0 = the graph has none); `0xC0 char label[16]`;
`0xD0 char name[32]` (tag path tail); 16 reserved. Weapon flags: `0x2` mutually exclusive triggers, `0x20` aim assist only
when zoomed, `0x200` prevents melee, `0x400` detonates when dropped, `0x800` cannot fire at maximum age (battery),
`0x4000` night vision (the full list is in the converter's docstring).

**Magazine, 64 B**: `0x00 u32 flags` (`1` wastes rounds when reloaded, `2` every round must be chambered), `0x04 u16
rounds_recharged, rounds_total_initial, rounds_total_max, rounds_loaded_max, rounds_reloaded, equipment_count`, `0x10 f32
reload_time, chamber_time` (seconds), `0x18 u32 reload_effect, chamber_effect`, `0x20 u32 equipment_tag[2]`, `0x28 u16
equipment_rounds[2]`. `rounds_total_*` is the **reserve**, loaded rounds are separate (verified, see below).

**Trigger, 128 B**: `0x00 u32 flags` (`4` can fire with partial ammunition, `8` does not repeat automatically, `0x20`
projectiles use weapon origin, `0x400` error when unzoomed, `0x1000` identical error), `0x04 f32 rps_min, rps_max,
accel_time, decel_time, blurred_rate`, `0x18 u16 magazine (0xFFFF none), rounds_per_shot, min_rounds_loaded,
tracer_interval, firing_noise, projectiles_per_shot, distribution_function, overcharged_action`, `0x28 f32
error_accel_time, error_decel_time, minimum_error, error_min, error_max, distribution_angle`, `0x40 f32 fp_offset[3]`,
`0x4C f32 charging_time, charged_time, charged_illumination, spew_time, ejection_recovery, illumination_recovery,
heat_per_round, age_per_round`, `0x6C u32 projectile, charging_effect`, `0x74 u16 first_fx, fx_count`.

**Firing effect, 32 B**: `u16 shots_lo, shots_hi`, `u32 effects[3]` (firing, misfire, empty: an effect **or a sound** tag,
`0x1C u8 sound_mask` says which: bit i = sound; the plasma rifle's empty click is a sound), `u32 damages[3]` (damage effect
tags: this is where the camera shake and rumble of the shot live).

**Projectile, 192 B**: `0x00 u32 tag_id, model_tag, flags`, `0x0C u16 timer_starts, impact_noise`, `0x10 f32
ai_perception_radius, collision_radius, arming_time, danger_radius, timer_lo, timer_hi, min_velocity, max_range,
air_gravity, air_range_lo, air_range_hi, water_gravity, water_range_lo, water_range_hi, initial_velocity,
final_velocity, guided_angular_velocity`, `0x54 u16 detonation_noise`, `0x58 u32 super_detonation, detonation_effect,
detonation_started, flyby_sound, attached_damage, impact_damage`, `0x70 u16 first_material, material_count`, `0x74 f32
object_radius`, `0x80 char name[32]`. **Velocities are world units per tick (1/30 s), exactly as stored in the tag
(x 30 = world units per second)**; `max_range` and the damage ranges are world units, `guided_angular_velocity` radians
per second, times are seconds.

**Material response, 64 B**: `u16 flags, response` (0 disappear, 1 detonate, 2 reflect, 3 overpenetrate, 4 attach),
`u32 effect`, `u16 potential_response, potential_flags`, `f32 skip_fraction`, `0x10 f32 angle_min, angle_max,
velocity_min, velocity_max`, `0x20 u32 potential_effect`, `f32 angular_noise, velocity_noise`, `u32 detonation_effect`,
`0x30 f32 initial_friction, max_distance, parallel_friction, perpendicular_friction`. 30 to 33 entries per projectile,
one per material type in the globals' order (a projectile that has none has 0).

**Damage effect, 384 B**: `0x00 u32 tag_id`, `f32 radius_lo, radius_hi, cutoff_scale`, `u32 flags`; screen flash
`0x14 u16 type, priority, f32 duration, u16 fade (+2), 0x20 f32 intensity, color[4] (a r g b)`; rumble
`0x34 f32 low frequency, duration, u16 fade` and `0x40` the same for the high frequency motor; camera impulse `0x4C f32
duration, u16 fade (+2), f32 rotation, pushback, jitter_lo, jitter_hi, permanent angle`; camera shake `0x68 f32 duration,
u16 falloff (+2), f32 random translation, random rotation, u16 wobble function (+2), f32 wobble period, wobble weight`;
`0x84 u32 sound`; breaking effect `0x88 f32 forward velocity, radius, exponent, outward velocity, radius, exponent`;
`0xA0 u16 side_effect, category`, `0xA4 u32 damage flags`, `0xA8 f32 aoe_core_radius, damage_lower, damage_upper_lo,
damage_upper_hi, vehicle_passthrough, active_camo, stun, max_stun, stun_time`; `0xD0 f32 modifiers[33]` (one per
material type), `0x154 char name[32]`.

**Equipment, 96 B**: `u32 tag_id, model_tag, anim_tag, item_flags`, `u16 powerup_type (1 double speed, 2 over shield,
3 active camo, 4 full-spectrum vision, 5 health, 6 grenade), grenade_type (0 frag, 1 plasma)`, `f32 powerup_time` (s),
`u32 pickup_sound, collision_sound, detonating_effect, detonation_effect`, `f32 detonation_delay[2], item_scale`, `u16
hud_message`, `0x40 char name[32]`.

**Grenade, 32 B**: `u16 max_count, mp_spawn_default`, `u32 throw_effect, hud_tag (grhi), equipment, projectile`.

## Reader (`weapdef.h`)
`weap_open` validates before anything is handed out: size field (`WEAP_ERR_TRUNCATED` when larger than the buffer,
`WEAP_ERR_LAYOUT` when smaller), signature, version, every table 16-aligned past the header and inside the section
(64-bit products), u16 index limits, strictly ascending tag ids, `first + count` of every range inside its table, at most
4 magazines and 4 triggers per weapon (the data has at most 1 and 2), at most 33 materials, trigger magazine index
`0xFFFF` or inside its weapon, NUL-terminated names, every float finite and `|x| <= 1e7`, rates 0..1000 per second,
non-negative times, heat and age. A failed open leaves `weap_section` zeroed. Lookups: `weap_find_{weapon, projectile,
damage, equipment, collection}(tag)` (binary search, -1 when absent); getters fill plain structs
(`weap_get_{weapon, projectile, damage, equipment, grenade}`, `weap_get_fx`, `weap_get_material`, `weap_collection[_entry]`)
and return 0 with a zeroed struct for an index out of range. Records are decoded with byte loads (no alignment
assumptions on the buffer), no heap, no `double`.

Pure helpers:
* `weap_damage_at_range(proj, dmg, dist, rand01)`: full `damage_upper` (picked between its two values) up to
  `air_range[0]`, linear to `damage_lower` at `air_range[1]`. Fits the data: the assault rifle is 10 at any range, the shotgun
  pellet 18..25 inside 1.5 wu and 8 beyond 3 wu, the sniper bullet 101 everywhere.
* `weap_damage_at_radius(dmg, dist, rand01)`: area damage, full inside `max(aoe_core_radius, radius_lo)`, linear to
  `damage_lower` at `radius_hi`, 0 beyond; a damage effect with `radius_hi == 0` (bullets, melee) does not spread.
  **A model, unverified** (the tags give the numbers, not the falloff curve).
* `weap_projectile_speed(proj, age, lifetime)`: initial to final velocity over the flight (the plasma bolts slow down).

## Weapon state machine (`weapon_fire_step`)
`weapon_fire_step(state, weapon, input, events)` advances one 30 Hz tick; `weap_weapon` is the unpacked weapon with its
magazines and triggers, `weapon_state_init` readies a fresh weapon (every magazine loaded to its maximum from
`rounds_total_initial`; `weapon_state_set_ammo` overrides, e.g. from a scenario placement or a player profile).
Modes: ready (taking out), idle, reload, overheated, melee, putting away, stowed. Input per tick: `fire[trigger]` held,
`reload`, `melee`, `zoom`, `switch_out` pressed. Events (flags plus per trigger shot, projectile count, tracer, spread
angle, charge fraction, rounds used): ready, dry fire, reload start / done / cancel / automatic, overheated, cooled,
charging, charged, melee start / hit, put away, zoom, battery dead.

Rules (all from tag numbers; durations in ticks are rounded to the nearest tick, at least one for a positive time):
* **Ready / put away / melee** last the first person animation's frame count (30 Hz, `fp_ticks`), `WEAP_DEFAULT_*` (15 / 10 /
  20 ticks) when the graph has none (verified: 9 weapons have a first person graph, the vehicle guns, the sword and the fuel
  rod do not). Nothing fires while readying, putting away or in melee; the READY event comes on tick `ready_ticks` and a trigger
  held at that moment fires on the same tick. Melee damage lands half way (event MELEE_HIT).
* **Rate**: the trigger's rounds per second ramp from `rps_min` to `rps_max` over `accel_time` while it is held and back
  over `decel_time`. A shot is possible when its cooldown (ticks, fractional) has run out; each shot adds
  `30 / rate` ticks, with the fractional part carried, so the long-run rate is the tag's to within one tick. A magazine with
  "every round must be chambered" floors the interval at `chamber_time`; a trigger with rate 0 (the Scorpion's cannon) fires every
  `chamber_time`, and without a chamber time once per pull. `does not repeat automatically` fires once per pull.
* **Error**: the spread angle blooms from `error_min` towards `error_max` over `error_accel_time` while held and relaxes over
  `error_decel_time` (`minimum_error` is the floor); the event carries the angle of the shot.
* **Ammunition**: a trigger with a magazine of `rounds_loaded_max > 0` needs `max(rounds_per_shot, min_rounds_loaded)` loaded rounds
  (less when "can fire with partial ammunition" and some are left) and consumes `rounds_per_shot`. Without enough: a DRY_FIRE
  event once per pull and, with reserve, an automatic reload; a trigger without a magazine or with a zero magazine (vehicle
  guns, energy weapons) has unlimited ammunition. When the shot takes the last round and there is reserve, the reload starts
  by itself (AUTO_RELOAD). Conservation, checked on every tick of the host test: `loaded + reserve + wasted + rounds_fired`
  is constant per weapon.
* **Reload** lasts the magazine's `reload_time` (rounded), else the first person reload animation (empty or full variant), else 60
  ticks; each cycle loads `rounds_reloaded` rounds (`rounds_loaded_max` when 0) from the reserve. When that is less than the
  magazine (the shotgun loads one shell per 0.4 s) the cycles repeat until full or out of reserve and **pulling the trigger
  cancels the reload** (RELOAD_CANCEL). "Wastes rounds when reloaded" (flamethrower) throws the rounds still loaded away
  (`wasted`). A chambered magazine delays the next shot by `chamber_time` after a reload.
* **Charge**: a trigger with `charging_time` charges while held (ticks counted in whole ticks; CHARGED on the tick it is
  reached), fires on release with the charge fraction (1 = full; a tap fires a partial charge), or when held for
  `charging_time + charged_time` when that is non-zero. It does not start without ammunition or on a dead battery.
* **Heat and battery**: every round adds `heat_per_round`, heat cools by `heat_loss` per second; at `overheated` the weapon goes
  OVERHEATED, ignores triggers, and recovers when heat falls to `heat_recovery`. `age_per_round` builds up the battery age
  (never decreases); with the flag "cannot fire at maximum age" a weapon at age 1 clicks (DRY_FIRE | BATTERY_DEAD).
* **Zoom** cycles 0..`zoom_levels` and back to 0.

What the state machine does not do: grenade throwing (equipment and the globals' grenade table are data only), firing
effects and sounds (the events carry the trigger, the integrator looks the effect up with `weap_get_fx`), projectile
flight, hit detection and damage application (tables above), the `secondary_mode` semantics (stored, not interpreted),
`flags` bits other than the ones named in `weapdef.h`.

**Recoil**: the weapon tags have no recoil field. What the player feels is the firing damage effect of the shot (`fx.damages[0]`):
its camera shake (duration, random rotation, falloff) and rumble, plus the first person fire animation. Examples of the
camera shake in the data: assault rifle 0.05 s / 0.5 degrees, pistol 0.2 s / 0.2 degrees, rocket launcher 1 s / 2 degrees,
Scorpion cannon 0.5 s / 1 degree; rumble on every weapon.

## Source data and what was verified
Layouts are in the docstring of `weapconv.py` (byte offsets of every field read). All ten campaign maps convert with **zero
plausibility problems** and the 20 distinct weapons are bit-identical in every map that has them. Facts that confirm the layouts:

| Fact | Status |
|---|---|
| Struct sizes: weapon 0x508, magazine 0x70, trigger 0x114, firing effect 0x84, projectile 0x24C (sub-blocks follow in field order), material response 0xA0 (33 x 160 = the block), damage effect 0x2A0, grenade 0x44 | verified (every block address is the previous one plus its size) |
| Weapon flags | verified by weapon (sniper = night vision + aim assist only when zoomed, plasma weapons = cannot fire at max age, fuel rod = detonates when dropped) |
| Weapon type +0x4E2 (0 / 1 / 2 / 3 / 4) | verified: shotgun 1, needler 2, plasma pistol 3, plasma rifle 4, all others 0 |
| Label +0x30C | a short code (`ar`, `pr`, `hp`, ...) followed by stale bytes of an older label ("rifle" at +0x320): only the string up to the first NUL means something |
| Trigger rate fields | verified: assault rifle 15 rps, pistol 3.5, sniper 2, rocket launcher 0.5, chain gun 8..15 over 1 s, plasma rifle 7..10 over 1.8 s; trigger flags 8 on every semi-automatic weapon |
| Trigger offsets of rounds per shot / tracer interval / projectiles per shot / error angles | verified: 1 / 3 / 1 / 2.0..6.5 deg (assault rifle), 15 pellets and 10 deg (shotgun), 0.2..0.4 deg (pistol), 0.5 deg (sniper) |
| Magazine fields | verified: loaded max never exceeds total max, rounds reloaded <= loaded max, ammunition pickups of the magazines (`<weapon> ammo` equipment, e.g. 240 rounds for the assault rifle) all exist in the map, starting profiles of the scenario hold (loaded, total) pairs such as 6 / 48 for the pistol, 60 / 180 for the assault rifle: **total is the reserve, loaded rounds are not part of it** |
| Pistol reload time 2.17 s | verified against the first person reload animation: 65 frames at 30 Hz (the test asserts it) |
| First person animation list order (17+ entries: idle, posing, fire 1, moving, overlays, light off, light on, reload empty, reload full, overheated, ready, put away, overcharged, melee, fire 2, ...) | verified by animation name on every weapon with a graph: the converter records a problem when a used slot's animation name does not contain the slot's word |
| Zoom: levels at +0x3DA, range at +0x3DC | verified: pistol and rocket launcher 1 level at 2x, sniper 2 levels 2x..8x |
| Projectile velocity unit | verified: world units per tick (sniper 33.33 = 1000 wu/s equal to its 1000 wu range, pistol 10 = 300 wu/s, plasma bolts 1.667 -> 0.833 slow down, tank shell 3.33, thrown grenades 0 because the globals' throw speed applies) |
| Projectile detonation effect / started effect / flyby sound / attached and impact damage refs | verified by path: rocket, frag, plasma grenade, needler, charged plasma bolt reference `...effects\explosion`-like tags, the plasma grenade's started effect is `detonation start`, the needler's attached damage is `detonation damage` |
| Material responses: 33 per projectile, responses 1..4 | verified (30 / 31 / 32 on a few) |
| Damage category, side effect | verified by tag name (assault rifle and pistol bullets 2, sniper 5, shotgun pellet 12, needle 11, every melee 6 except 4 plasma melees, frag explosion 3, rocket 4) |
| Damage amounts at +0x1D0..0x1D8 | verified: assault rifle 10, pistol 25, sniper 101, shotgun pellet 8 / 18 / 25, melee 40 / 50 / 60, frag grenade 80 / 120, rocket 80 / 300..330; always lower <= upper_lo <= upper_hi |
| Camera shake / rumble / screen flash fields | verified by plausibility: the rocket launcher's firing effect shakes longest and hardest, a melee flashes red (r 1.0, g 0.09, b 0.11), a plasma bolt flashes cyan-green |
| Damage radius +0x00..0x04 | plausible: bullets 0, melee 0.5, explosions 1.5..3.25, shock waves 3.5..100 |
| Grenade table | verified by path (frag, plasma), equipment grenade types 0 / 1 on every map |

**Unverified or approximated**
* The 33 `modifiers` of a damage effect: assumed to be indexed by the material types in the same order as the projectile's
  material responses (only coarse checks: all values in 0..16, the sniper bullet 0.05 on the two hunter entries).
* `trigger.error_accel_time` / `error_decel_time` (+0x38 / +0x3C): meaning inferred (non-zero only on weapons whose error angle
  range is wide), values plausible (assault rifle 0.6 s / 1 s).
* `damage_effect` +0x1DC (vehicle passthrough penalty), stun fields, side effect `3` (emp), most flags bits, `weapon.secondary_mode`.
* Not exported because their meaning is unknown: the u16 pairs at weapon +0x330 / +0x334 and the f32 at +0x338, the six f32 at
  trigger +0xF0..+0x104 (multiples of 1/30 and 1/90 s), the flashlight values at weapon +0x418, the vehicle movement penalties.
* Item collections: layout unverified and **not exercised by any map** (the converter reads permutations of 0x54 bytes with the
  weight at +0x20 and the item reference at +0x24 and raises `WeapError` when a reference does not resolve to an item instead of guessing).
* The falloff of area damage (above), the interpretation of release-to-fire charging, the automatic reload rules and the default
  durations are the state machine's model, not tag data.
* Real-console behaviour (30 fps, timing of animations against the 30 Hz simulation) is not tested before Phase 6 (ADR-022).

## Numbers on the real maps
All ten campaign maps, `python3 -I tools/halo2ps2/weapconv.py <map> -o assets/hp2/<map>.weap.hp2`, 0.1 s per map.

| map | weapons | magazines | triggers | fx | projectiles | material responses | damage effects | equipment | WEAP bytes | pack bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 6 | 6 | 8 | 9 | 12 | 394 | 43 | 6 | 48032 | 10240 |
| a30 | 10 | 10 | 12 | 13 | 13 | 429 | 54 | 7 | 56704 | 12288 |
| a50 | 11 | 10 | 11 | 12 | 12 | 396 | 52 | 9 | 53920 | 12288 |
| b30 | 13 | 12 | 14 | 15 | 15 | 495 | 59 | 11 | 64832 | 14336 |
| b40 | 16 | 15 | 18 | 19 | 19 | 627 | 68 | 9 | 78912 | 16384 |
| c10 | 9 | 9 | 10 | 11 | 14 | 458 | 59 | 10 | 60320 | 14336 |
| c20 | 9 | 9 | 10 | 11 | 14 | 454 | 59 | 10 | 60064 | 14336 |
| c40 | 18 | 17 | 19 | 20 | 22 | 719 | 80 | 9 | 90784 | 18432 |
| d20 | 15 | 15 | 17 | 18 | 18 | 591 | 70 | 12 | 77056 | 16384 |
| d40 | 14 | 14 | 16 | 17 | 16 | 526 | 70 | 9 | 71744 | 16384 |

(a10: 48032 raw bytes, 10240 on disc after LZ4 + the pack's 2048 byte sectors; the largest, c40: 90784 raw, 18432 packed.)
Every map has 2 grenades (frag, plasma; maximum 4 each) and no item collection.

Object pack roots (`referenced_models(map)`, `referenced_anims(map)`, keyed like `modelconv` / `animconv`): a10 15 models and 5
animation graphs (MODL 0.83 MiB + OTEX 0.52 MiB, ANIM 133 KiB when converted on their own), c40 31 models and 11 graphs
(MODL 1.87 MiB + OTEX 1.06 MiB, ANIM 233 KiB). Models: weapon world models, first person models, projectile and equipment
models; graphs: first person graphs (a weapon has no other).

### Weapons of the campaign
Rate = trigger 0 (or the first trigger that fires), magazine = loaded / reserve maximum, reload = one cycle in the machine
(tag time, else first person animation), speed = initial projectile velocity in world units per second (x 30 of the stored
per-tick value), damage = the projectile's direct damage (a dash: the damage is in the detonation, `explosion`). Produced by
`engine/tests/test_weapdef -t` from the converted packs; every weapon of all ten maps (121 weapons, 135 triggers) passes the
host test below.

| weapon | rate | magazine | reload | speed (wu/s) | damage |
|---|---|---|---|---|---|
| assault rifle | 15/s | 60 / 600 | 3.40 s | 324 | 10 |
| pistol | 3.5/s | 12 / 120 | 2.17 s | 300 | 25 |
| plasma rifle | 7-10/s | battery | - | 50 | 12-14 (min 10) |
| plasma pistol | charge 0.60 s (trigger 1: overcharge) | battery | - | 25 | 16-20 (min 10) |
| shotgun | 1/s | 12 / 60 | 0.40 s per shell | 140 | 18-25 (min 8), 15 pellets |
| sniper rifle | 2/s | 4 / 24 | 2.77 s | 1000 | 101 |
| rocket launcher | 0.5/s | 2 / 8 | 5.00 s | 12 | explosion |
| needler | 3-10/s | 20 / 80 | 1.00 s | 4 | 0 (impact: the needle detonates, attached damage) |
| flamethrower | 15/s | 100 / 1200 | 3.40 s | 10 | explosion |
| fuel rod | charge 1.25 s | 4 / 24 | 5.00 s | 14 | explosion |
| hunter fuel rod | charge 1.25 s | unlimited | - | 14 | explosion |
| energy sword | (no trigger) | - | - | - | melee only |
| sentinel | 30/s | unlimited | - | 1200 | 1 |
| warthog gun | 8-15/s | unlimited | - | 293 | 16-24 (min 16) |
| scorpion cannon | 1 per 4.0 s (chamber) | 1 / unlimited | - | 100 | explosion |
| banshee gun | 5-6/s | 1 / unlimited | - | 50 | 10-14 (min 10) |
| ghost gun | 5-6/s | 50 / 300 | 2.00 s | 50 | 10-14 (min 10) |
| c gun turret gun | 4-5/s | battery | - | 25 | 12-14 (min 8) |
| cd gun turret gun | 8-10/s | battery | - | 25 | 12-14 (min 8) |
| wraith mortar | 0.5/s | 10 / 50 | 2.00 s | 10 | explosion |

(The damage unit is the biped's health / shield value space: a pistol shot of 25 against the units' 75 / 75.
workstream C's `UNIT` section holds the units' maxima; the relation is not asserted here.)

## Host tests
* `tools/tests/test_weapconv.py`: a synthetic map (weapon with a magazine, ammunition pickup, firing effect carrying a sound,
  projectile with three material responses, two damage effects, two equipment, one grenade, a first person graph) through the
  collector, the plausibility rules, corrupt tag data (reflexive outside the region, too many magazines, infinity, 40
  materials), the section writer and its reference parser; all ten real maps when `assets/decompressed/` has them
  (counts equal the tag counts, assault rifle / pistol / sniper relations, build twice = identical bytes, hp2 round trip).
  `WEAP_FIXTURE=<path>` writes the synthetic pack for the C test.
* `engine/tests/test_weapdef.c` (`make -C engine/tests`): every accessor against the fixture, then the state machine on
  three synthetic weapons with exact expectations (burst: 10 shots 3 ticks apart for 10 rps, tracer every third, spread
  bloom and relax; reload 60 ticks; empty magazine with and without reserve; last round; melee; put away; charge CHARGED on
  tick 15; heat; battery; zoom; the shotgun's shell-by-shell reload and its interruption; one shot per pull), then every
  truncation and every byte corrupted five ways (rejected or safe to walk and simulate completely, ASan / UBSan), then
  **every weapon of every `assets/hp2/*.weap.hp2`**: ready (READY on exactly `ready_ticks`, no shot before), a burst per
  trigger (first shot on tick 0; intervals between the tag's slowest and fastest rate, mean interval equal to
  `30 / rps` within one tick for constant rates, one shot per pull for non-repeating triggers, rounds used = shots x
  rounds per shot), a full, partial and short-reserve reload per magazine (exact cycle count and ticks, loaded and
  reserve afterwards), an empty magazine (one click per pull, no shot, automatic reload with reserve), a dead battery,
  overheating (reached within 30 s of fire for weapons without a magazine, no shot while hot, cools down), charge,
  melee, put away, zoom, and 5000 ticks of random input with the ammunition conservation checked on every tick.
