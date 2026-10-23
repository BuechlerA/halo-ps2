# Units: locomotion, weapons, animation and death of non-player bipeds (workstream U of Phase 4 stage 2)

Code: `engine/src/ai/units.c`, contract `engine/src/sim/units.h`. Test: `engine/tests/test_units.c` (`make -C engine/tests units`, real a10 packs, skipped without
`assets/`). Context: [30-mission-host.md](30-mission-host.md) (stage 2 split, tick order), [17-unit-section.md](17-unit-section.md) (biped numbers),
[19-ansm-section.md](19-ansm-section.md) (animation tables), [18-weap-section.md](18-weap-section.md), [28-cine-section.md](28-cine-section.md) (recordings).

A **unit** is a biped object that is not the player: the AI actors, scripted crew, recorded-animation actors. A `unit_t` is attached to an object-table slot and
owns a body (the Phase 3 capsule controller), a control input (`unit_control_t`: throttle, aim, `UC_*` flags), a weapon (`weap_state`), grenades and an animation
state. Health lives where the game keeps it: `game_t.unit_state[obj].vitals`. The module is pure C11, no heap, no `double`, pointer-free state
(`sizeof(units_t)` = 40,084 bytes: 380 per unit x 96, by-object table 2.5 KB, event list 1 KB; the checkpoint code copies it as raw bytes).

## 1. The API (contract and what was added)

The functions of `sim/units.h` as the integrator wrote it (`units_init / attach / detach / of_object / tick / set_control / damage / kill / alive / teleport /
give_weapon / eye / teams_hostile`) keep their semantics. Additions (all in `units.h`; the AI, H1 and the fixture already use them):

| Function | What it is for |
|---|---|
| `units_apply_variant(s, g, unit, const aidef_variant_def*)` | AI spawn, after `units_attach`: health / shield of the variant (absolute values) x the difficulty table, weapon, grenade type and count, projectile error x difficulty, damage modifier, `variant_tag` |
| `units_apply_cinedef(s, unit, const cinedef_ctl*)` | recorded animations: throttle x / y -> forward / left, yaw and pitch counts -> radians (pi/1000 each), CROUCH JUMP PRIMARY_FIRE SECONDARY_FIRE GRENADE MELEE RELOAD -> `UC_*` (the others, e.g. LOOK_DONT_TURN, are ignored), weapon index -> `weapon_slot`. Call once per tick with `cine_play.ctl` |
| `units_custom_animation(s, g, unit, graph, anim, scale, hold)` / `units_stop_custom` | script / command-list animations (`UA_CUSTOM`): any animation of any ANIM graph (`graph` -1 = the unit's own), played once at `scale` x speed, then the unit returns to its normal animation (or holds the last frame until `units_stop_custom`); returns the ticks it lasts, 0 for a bad index |
| `units_lookup_anim(s, g, unit, action, dir, &is_overlay)` | the animation (index in the unit's graph) the unit would use for a state, for tests, tools and the AI (durations of one-shot actions) |
| `units_events(s, &n)` | what happened in the last `units_tick`: `UE_FIRE` (weapon tag, trigger, muzzle, direction), `UE_RELOAD`, `UE_MELEE` (arg 1 = it hit), `UE_THROW`, `UE_DEATH`, `UE_FLINCH`, `UE_LAND`. The game's own event list is cleared at the start of `game_tick`, which runs after `units_tick`, so a unit's shots could not be reported there: sound, muzzle flashes and the AI's hearing read this list |
| `units_rebind(s, g)` / `units_state_size()` | after the state was copied back (checkpoint revert, memory card): rebuilds the object -> unit table and detaches units whose object is gone; it does not change simulation state (the snapshot test depends on that) |
| `units_t.lod_distance` | wu from the player beyond which a unit's body is integrated every other tick over a double step (0 = off, the default; section 4) |
| `UF_NO_DROP` | a unit with this flag does not drop its weapon when it dies |

`unit_t` got internal fields (`prev_flags`, `rng`, `anim_*`, `proj_error`, `rate_mult`, `hit_dir`, ...: see the header); the AI may read them
(`proj_error`, `rate_mult`, `grenade_type`, `fire_hold`, `action`, `body`, `wst`, `pp` are the useful ones).

Changes outside `units.[ch]` (all small, each said in the report):

* `sim/game.h`, `sim/game.c`: `game_set_vitals` (replace the vitals of an object), `game_throw_grenade` (a grenade with the player's grenade physics, owner
  = the object slot: `game_fire_projectile` cannot set the grenade flag), `game_damage_player` (melee blows on the player), and two fields of `game_unit_t`: `ally`
  (the projectiles of an ally of the player never hurt the player: before, every shot with an owner >= 0 did) and `dmg_scale` (multiplier on the damage of the projectiles
  an object owns, units.c sets it from the difficulty table and the variant; 0 means 1). `proj_step` uses both.
* `sim/phys.[ch]`, `sim/player.[ch]`: `phys_sweep_capsule_n` (the capsule sweep with a cap on the number of spheres) and `player_params_t.sweep_spheres`
  (0 = as before, so the player is bit-identical). Actors set 2 (and a one sphere ground probe): 3 sweeps instead of 4..7 per walking tick, the dominant cost (section 8).
* `engine/tests/Makefile`: the `units` rule (sanitizer build plus an -O2 build with `UNITS_BENCH=1` that enforces the tick budget).

## 2. Tick order and what a tick does

`units_tick` runs before `game_tick` (mission order: script -> devices -> AI -> units -> game -> cinematics); `objects_tick` must come after it (it advances the frames
units.c sets up). Per active unit, in this order: invulnerability top-up, **death** (a unit the game killed gets its death animation, weapon drop and stops), flinch
detection (health drop), the one-shot countdown (melee blow, grenade release, landing, flinch, custom animation), new one-shots from control **edges**
(`UC_MELEE`, `UC_GRENADE`, `UC_RELOAD`: held flags act once), body (turn, then `player_tick`), landing and fall damage, weapon step and shots, the base action,
animation, object position and root matrix. Units of an inactive BSP (`OBJ_F_INACTIVE`) and `UF_PLAYER` units are skipped; a unit whose object was freed detaches itself.
The hit events of the previous `game_tick` give the direction of a hit: a projectile impact on an object carries the way back to the shooter (`GE_IMPACT.normal`), kept
as `hit_dir` and used for the death and flinch animation direction (area damage and melee have no impact event: front).

`unit_action` meaning: `UA_AIRBORNE` after 3 ticks without ground, `UA_RELOAD` while the weapon reloads, `UA_FIRE` for the length of the fire animation after a shot,
`UA_MOVE` above 0.25 wu/s, `UA_MELEE / THROW / LAND / FLINCH / CUSTOM` while they last (they block firing; melee, throw and custom also block moving, landing and flinch slow it),
`UA_DEAD` forever.

## 3. Locomotion

Controls: `throttle_fwd` / `throttle_left` (-1..1, fractions of the unit's speeds, relative to the **body** yaw), `aim_yaw` / `aim_pitch` (world radians), `UC_CROUCH`, `UC_JUMP`
(edge), `UC_WALK`. The body turns towards `aim_yaw` at the biped's `turn_speed` (rad/s, tag), always: the tag's stationary turning threshold is not used. Movement is
`player_tick` with a `player_cmd_t` made from the control (move_y = forward, move_x = -left); `player_tick` applies a stick deadzone and rescales, so the stick value is the inverse
(0.15 + 0.85 t) and a throttle of t gives exactly t of the speed. Nothing of the capsule physics is duplicated: slopes, stairs (step height 0.25 x height), gravity (globals), jump
(biped), crouch (biped heights, 6/s smoothing) are the player's.

**Speeds are the actor's, not the player's**: `pp.run_speed / back_speed / strafe_speed / crouch_speed` start as the biped's root-motion `anim_speed[]` (UNIT section: stand run front / back /
side, crouch front) and then follow the move animation the unit actually plays (`refresh_move_speeds`, ADR-084): the ground speed of its root motion over one cycle, per posture, weapon and
graph (the marines' `stand rifle move-front` 2.10 wu/s, `alert rifle move-front` 0.42, `alert pistol move-front` 0.67). A direction whose animation has no root motion keeps the tag's speed; cfg
`unitspeeds 1` restores the tag speeds. Recordings carry controls, not positions (docs/28), so this is what decides where a recorded walk ends.
**Posture**: `UF_ALERT` (set by `unit_set_seat unit "alert"`) puts a unit in the graph's `alert` block, armed or not; unarmed non-player units are alert by default (ADR-080); a unit created anew
has no `UF_ALERT`. The `FORCE_ALERT` bit of a recording is not a posture. Missing values (some bipeds have only a front speed) are derived (back 0.75, side 0.8, crouch 0.5 of the front speed; no speed at all: the globals' walk speed, so
a recording can still move such a unit). `UC_WALK` uses the player's walk / run ratio of the globals. Measured on a10 (four walking species with weapons: run 1.9 .. 2.6 wu/s, turn 3.1 ..
5.9 rad/s): every measured speed (front, back, left, right, crouch) is within 1 % of the tag, turn rates within 2 % (the test requires 10 %).

Collision: BSP only (capsule vs `COLn`). Units do not collide with each other, with the player or with objects (the AI must separate them if it wants to). A unit that cannot move
(`settled`: on the ground, no velocity, no input, crouch at its goal) skips the physics completely and re-checks its ground every 8 ticks: a standing crowd is free.

Falling: `landed_speed` of the capsule gives fall damage with the globals' falling thresholds and damage effect (same rule as the player), and the landing animation (soft / hard by the
biped's landing velocities; the unit moves at 0.6 / 0.2 of its speed while it lasts).

## 4. The distance LOD

`units_t.lod_distance` (wu, default 0 = off): a unit farther than that from the player integrates its body every other tick (staggered by unit index, `dt` doubled; a pending `UC_JUMP`
forces the full rate). Turning, weapons, animation and everything else keep their 30 Hz. Speeds stay right (test: -2.5 %, the acceleration ramp is coarser). It halves the physics of the
crowd nobody looks at; the integrator sets it (a value of 30..40 wu suggests itself for a10) and may feed it from the AI's activity radius instead.

## 5. Weapons, grenades, melee

* **Shots**: `weapon_fire_step` with the unit's weapon (decoded from WEAP on use, no per-unit copy), `UC_FIRE` / `UC_FIRE2` are the triggers (burst control is the AI's job). Each shot
  spawns the trigger's projectiles with `game_fire_projectile(owner = the object slot)` from the eye + 0.25 along the aim - 0.08 (no weapon markers), direction = aim (clamped to +-70
  degrees yaw / 45 degrees pitch of the body: the aiming screen of the graphs) + a random deviation of (weapon error angle of the shot + `proj_error`). The projectile hits objects, the player
  and the world through the game's own code (damage x `dmg_scale`, allies' shots pass the player).
* **Ammo**: units never run out of magazines (the reserve is topped up to one magazine each tick); empty magazines reload by themselves (`WEAP_EV_AUTO_RELOAD`), `UC_RELOAD` reloads on
  demand. The reload takes the weapon's time; the animation is stretched to fit it.
* **Difficulty**: units of teams hostile to the player (covenant, flood, sentinels) fire at `rate_mult` x the weapon's rate (globals group "rate of fire", 1.5 on Legendary) and their
  `proj_error` is the variant's x the "projectile error" group (0.5 on Legendary). Left to the AI, with `unitdef_difficulty`: burstiness, new target delay, burst separation, target tracking and
  leading, overcharge chance, special fire delay, melee delay, grenade chance and timer (groups 12..24).
* **Grenades**: `UC_GRENADE` edge with a grenade of `grenade_type` in `grenades[]` starts the throw animation; at its key frame the grenade leaves along the **aim exactly** (no loft: the AI
  chooses `aim_pitch` for the arc; the speed is the biped's `grenade_velocity`, wu/s as the player's throw uses it) through `game_throw_grenade`.
* **Melee**: `UC_MELEE` edge starts the melee animation (type slot, else class slot, else 20 ticks); at its key frame (else half way) the blow hits the nearest living object with vitals in
  front (1.1 wu, +-60 degrees, wall check by a ray), skipping friendly units, or the player when the unit is hostile to the player. Damage tag = the weapon's melee damage, else the biped's
  own, x `dmg_scale`.

## 6. Health, difficulty, damage

`units_attach` takes health / shield / recharge from the game's vitals of the object (the collision model) or the biped, and scales them by the globals' difficulty block: **enemy** (hostile to
the player: groups vitality, shield, recharge = 1, 2, 3) or **friend** (player and human teams: groups 5, 6, 7), at `game_t.difficulty`. `units_apply_variant` does the same with the variant's
absolute values. `dmg_scale` = enemy damage (group 0) or friend damage (group 4) x the variant's weapon damage modifier; the projectile code applies it to the damage of everything that unit
shoots at anyone (the Phase 3 code applied "enemy damage" to the player only: the meaning of the damage groups on other targets is [P]).
Team table (`units_teams_hostile`): player and human friendly; covenant hostile to both and to flood; flood hostile to all; sentinels hostile to player, human and flood, neutral to covenant;
equal teams, team 0 and teams > 5 never hostile. Scripted allegiances are the AI's business (it can wrap the function).

`units_damage` goes through `game_damage_object` (events, kill statistics fire as for any object), `units_kill` kills through the same path (`silent`: sets the dead flag without events),
`UF_INVULNERABLE` makes `units_damage` a no-op and revives a unit the game killed (a one-tick hit larger than health + shield is undone on the next tick, with its `GE_KILL` already issued).

## 7. Animation

The object table has **one** animation per object, so a unit shows its *base* animation; overlays (aim-still / aim-move, fire, shield pings) and the upper-body replacement of a reload
cannot be drawn and are not played. Selection (`units_lookup_anim` is the same code):

1. Block: unit label `crouch` when the body is crouched (> 0.5), else `stand` (the other labels such as `alert`, `flee`, seats are not used); a slot missing in the crouch block is looked up
   in the stand block.
2. Class: `ansm_find_class_for_type(unit, weapon label)` (the weapon tag's label is its type label, docs/19 section 1.5); an unarmed unit, or a label without class, uses the class named
   `unarmed` (or its misspelling), else class 0.
3. Slot per state (the table is the slot table of `ansm.h`):

| State | Slot | Played as |
|---|---|---|
| `UA_IDLE` | class idle (variant picked at random per start) | loops |
| `UA_MOVE` | class move-front / back / left / right by the velocity relative to the body (sticky: the other axis must exceed 1.3 x before it switches; missing direction -> front) | loops, **rate = speed / the tag speed** of that direction (0.35..1.8), phase kept when the direction changes |
| turning in place (> 0.12 rad left to turn) | class turn-left / turn-right | restarts while the body still turns |
| `UA_AIRBORNE` | class airborne | loops |
| `UA_LAND` | class land-soft / land-hard (the other if missing) | once |
| `UA_FIRE` | type fire-1 (fire-2 for the secondary trigger) | **overlay: the base animation keeps playing**; `units_lookup_anim` returns it with `is_overlay` = 1 |
| `UA_RELOAD` | type reload-1 | replacement type, played as the base animation only while the unit stands (moving: move animation), stretched to the weapon's reload time |
| `UA_MELEE` | type melee, else class melee | once; the blow lands at the animation's key frame; no slot: idle, the blow still lands after 20 ticks |
| `UA_THROW` | class throw-grenade (key frame = release) | once |
| `UA_FLINCH` | damage table category 1 (health ping), direction of the hit, chest (else gut) when it is a base animation, else class stunned-front | once, <= 40 ticks |
| `UA_DEAD` | damage table category 3 (health kill, else 2), direction of the killing hit (front when unknown), chest / gut by a random pick; airborne -> unit-block airborne-dead; fallbacks: landing-dead, then any non-overlay animation whose name contains "kill", "dead" or "death" | once, then the last frame stays |
| `UA_CUSTOM` | the caller's animation, any graph | once or held |

No slot at all: the object keeps what it has and the lookup is not repeated every tick. A unit never crashes on a graph that lacks slots (test: no ASTB section, no weapons, no collision).
Names found for the two species the test tours (printed by the test, stand posture; the exact set differs per graph): idle `<posture> <class> idle` (variant `%0` on some), `... move-front`
(back / left / right), `... turn-left`, `... airborne`, `... land-soft`, `... throw-grenade`, `... melee` (class slot, only on one of the two graphs), fire `... <type label> fire-1`
(overlay), reload `... <type label> reload-1` (replacement), flinch and death from the damage table (`h-ping <direction> <region>`, `h-kill <direction> <region>`).

## 8. Death, corpse, weapon

When the vitals die (the game's projectile damage, `units_damage`, `units_kill`, fall damage) the next `units_tick` (or `units_kill` itself): action `UA_DEAD`, controls zeroed, death animation
(started at frame 0; the game's own immediate death pose is replaced), `UE_DEATH`, the body keeps falling until it rests and is then frozen (no more physics, no sync), the corpse and its hit
shapes stay (projectiles still hit it). **Weapon drop**: unless `UF_NO_DROP`, an object of the weapon's definition is created at the corpse (when the map's SCEN has a definition for it and the
table has 48 slots to spare) with the unit's BSP mask; the player's pickup code takes it (fresh ammo). a10: the definitions exist for the weapons the placed units carry.

## 9. Numbers (host, Apple silicon, clang -O2; `make -C engine/tests units`)

96 units, all attached on BSP 0 of a10 (`units_tick` only, ms per tick):

| Scenario | ms per tick |
|---|---|
| all standing (settled) | 0.016 .. 0.020 |
| a sixth walking, a sixth standing and shooting, the rest idle (a realistic mix) | 0.045 .. 0.05 |
| all walking and shooting, random headings (every unit regularly bumps a wall) | 0.40 .. 0.43 |
| **all 96 walking, random headings** (worst case) | **0.48 .. 0.53** |
| the same with the distance LOD (every unit far) | 0.35 .. 0.38 |

Sanitizer build: x 25..30. Almost all of it is `coll_sphere_sweep` (sampled before the 2 sphere change: over 95 % in the BSP sweep): 3 sweeps (2 sphere slide, 1 sphere ground probe) per walking unit-tick on a clear run (the full
capsule needs 4..7), about 5 with wall contact, 0.7 .. 0.9 µs each. **EE estimate**: the EE is 30-100x slower than this host, so one walking unit costs 100-400 µs of the 4 ms AI + units budget
(16 walking actors: 1.5 .. 6 ms): moving actors are the cost, standing, shooting and reloading ones are free. Keep the number of simultaneously moving actors down, set `lod_distance`, and
see the AI document for what else moves. (Measured on PCSX2 in the gate, not here.)

Other measurements: attach of all placed bipeds, speeds (front, back, left, right, crouch) of every species within 1 % of the tags, 3000 ticks x 12 scripted units never inside solid space,
a unit kills a unit in 239 ticks (18 of 71 shots hit at 4 wu), an invulnerable target survives 700 ticks of fire while the shooter reloads 3 times, a hostile shooter empties the player's shield
and health, an ally's shots do not, a grenade detonates 46 ticks after the throw, melee takes 100 of 280 health + shield (Legendary scale), a long fall hurts, the snapshot test (32
fighting units, 200 ticks, copy, 300 ticks, hash; restore into the same or a fresh `units_t`, 300 ticks: same hash).

## 10. Unverified or approximated

* Overlays (aim, fire) and the reload's upper-body replacement are not drawn (one animation per object); the data for them is exposed by `units_lookup_anim`.
* Muzzle position (eye + 0.25 - 0.08), melee reach (1.1 wu, +-60 degrees), flinch rule (a hit of >= 12 % of maximum health, 120 ticks apart), `MOVING_SPEED`, the +-70 / 45 degree aim clamp
  (constants, not read from the graph's aim ranges) are my choices.
* The body turns at `turn_speed` always; the biped's stationary turning threshold and Halo's turn-in-place logic are not modelled.
* Damage groups: enemy / friend damage apply to every target of that unit's projectiles and blows [P]; rate of fire and projectile error apply to hostile teams only.
* Grenade velocity unit (wu/s, as the player's throw uses it) and the lack of loft are as the Phase 3 player code; ally projectiles still hurt other allies and other objects.
* `UC_ZOOM`, `UC_FLASHLIGHT` and `weapon_slot` are ignored (one weapon per unit, no weapon switching or pickup by units); no vehicles or seats.
* Units do not collide with each other. Fall damage and landing use the globals' and the biped's thresholds as given.
* `game_tick` with `has_phys` = 0 passes an all-NULL `phys_world` to `player_tick` and crashes (Phase 3 code, not touched); units use an empty world in that case.

## 11. Notes for the AI, the script host and the integrator

* Set the control once and it stays; `UC_MELEE` / `UC_GRENADE` / `UC_RELOAD` act on the edge (clear them to fire again), `UC_FIRE` is level.
* `units_eye` gives the eye and the clamped aim direction; the shot starts there.
* Recorded animations: `units_apply_cinedef(units, unit, &cine_play.ctl)` every tick after `cine_play_tick`, `UF_NO_AI` set by the caller.
* Events: read `units_events` after `mission_tick` (or after `units_tick`) for sound and muzzle flashes of units; the player's keep coming from the game's list.
* Checkpoints: copy `units_t` (40 KB) with the game state, then `units_rebind`.
* After `mission_destroy_object` the unit detaches itself on the next tick (or call `units_detach`).

## 12. ADR proposal

**ADR-0xx Units: the player's capsule controller driven by a control record, actor speeds from root motion, one base animation per object.**
Decision: non-player bipeds are `unit_t`s over object slots (`ai/units.c`); locomotion is `player_tick` fed by a `player_cmd_t` made from `unit_control_t` with the actor's own
speeds (root motion in the UNIT section), weapons the Phase 3 state machine and `game_fire_projectile`, health the game's vitals scaled by the difficulty block, animation the ASTB base
slots (overlays and replacements not drawn), state a pointer-free `units_t` of 40 KB with a distance LOD and an event list. Why: one physics (the player's) to validate; no second set of tag numbers
to prove; the budget on the EE is dominated by capsule sweeps, so the knobs are sweeps per tick (2 spheres + 1 ground sphere for actors) and rate (settled skip, LOD).
Consequences: actors collide with the BSP only; fire and aim overlays are invisible until the object table grows a second animation slot; the EE cost per moving actor (3-5 sweeps per tick) is
the main risk of the 4 ms AI + units budget and is to be measured in the gate.
