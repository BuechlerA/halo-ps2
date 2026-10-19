# AI runtime (`ai/ai.[ch]`) and the `ai` script built-ins (`script/hs_host_ai.c`)

Phase 4 stage 2, workstream A ([contract](30-mission-host.md), data [25](25-ai-section.md), pathfinding [26](26-path-section.md), units [31](31-units.md)).
The AI **decides**, the unit layer **executes**: `ai_tick` reads unit / object / player state and writes `unit_control_t` through `units_set_control`. The
only direct writes into other modules are listed under "What it touches". Pure C11, no heap, no `double`, no recursion; compiles clean with
`-Wall -Wextra -Wdouble-promotion -Wshadow`. Legal posture as in [23](23-phase4-plan.md): no names or strings of the content appear here or in the test
(the test picks its data by properties).

## Files
| File | What |
|---|---|
| `engine/src/ai/ai.h`, `ai.c` | the runtime (spawning, perception, behaviours, movement, combat, command lists, conversations, orders, queries) |
| `engine/src/script/hs_host_ai.c`, `hs_host_ai.h` | `void hs_host_register_ai(hs_machine *)`: 73 `ai`-domain built-ins (table below) and per-built-in call counters |
| `engine/tests/test_ai.c`, rule `ai` in `engine/tests/Makefile` (also run by `make -C engine/tests`) | host test on the real a10 packs |

## State, saves, wiring
`ai_state` is plain data without pointers: **56,568 bytes** (target 160 KB); `ai_t` = `ai_state` + borrowed pointers (`ai_env`) + hooks + a transient target table:
**60,576 bytes**. Layout of the state: 64 actors x 324 B, 1024 squads x 20 B, 128 encounters x 32 B, 64 conversations, 48 profiles x 184 B (a digest of
variant + actor + biped + weapon, rebuilt from the tags by `ai_rebind`), difficulty scales, noise list, relations, statistics counters.
* Checkpoint: `memcpy(out, ai_state_ptr(ai), ai_state_size())`. Restore: `ai_init(ai, &env)`, copy the bytes back, `ai_rebind(ai, &env)` (0 = ok; -1 = magic /
  version / section counts / any actor index inconsistent: the AI is then cleared). The object table, `units_t` and the game's vitals are saved by their owners
  and must be restored together (actors name units and objects by index). Tested: 200 ticks, snapshot, 300 ticks (hash), restore into a fresh `ai_t`, 300 ticks:
  identical hash (AI state bytes + unit positions + health); also two independent runs of the same crowd hash equal.
* `ai_env`: `aidef`, `game`, `units`, `path` + `scratch` (path section of the **current BSP** and memory from `path_scratch_bytes`), `bsp`. `mission_t.ai` is `struct ai *` = `ai_t *`.
* Per tick (stage-2 order): `ai_tick(ai)` after the script machine and devices, **before** `units_tick`. It does not need `mission_t`.
* Level start: `ai_place_all_initial(ai)`. BSP switch: `game_set_collision`, then `ai_set_bsp(ai, bsp, path_bsp, scratch)`: encounters placed (by `ai_place` or at start) while
  another BSP was loaded wait as *pending* and are placed now; actors of other BSPs are frozen (not ticked) and routes of the old mesh dropped.
* Hooks (all optional): `ai_set_sound_hook` (conversation lines: the integrator plays the take and returns its length in ticks, 0 = unknown = 3 s), `ai_set_script_hook`
  (`script` atoms: name index of the AIDT script references -> `hs_wake_script`), `ai_set_object_hook` (look-at-object atoms: `mission_object_by_name(m, idx, 0)`),
  `ai_set_log_hook` (missing SCEN definitions...).
* The AI spawns its own actors (`objects_spawn_def` + `game_object_spawned` + `units_attach` + `units_apply_variant` + `units_teleport`); do not also call
  `mission_spawn_def_at` for them. Placed bipeds created by scripts (`object_create`) are plain units (not actors) until `ai_command_list_by_unit` / `ai_attach*`
  gives them a brain.
* The difficulty comes from `game_t.difficulty` at `ai_init` (`ai_set_difficulty` changes it).
* Statistics (`ai_state.st_*`, deterministic): placed, noticed, alerts, routes (+ partial / failed / deferred), stuck, assists, rays, shots, deaths, state changes, atoms run
  / unsupported per type, conversation lines...

### What it touches outside `ai_state`
`units_set_control` every tick for every driven actor; `units_apply_variant`, `units_attach`, `units_teleport`, `units_kill`, `units_detach` on spawn / erase / kill / hops;
`objects_spawn_def`, `objects_destroy`, `object_t.bsp_mask` of spawned objects; `game_t.unit_state[obj]` reset on erase; `units_custom_animation` for `animate` atoms. Nothing else.
`game_t.ev` (the shots of the player) and `game_t.player` / `vitals` are read.

## Design

### Spawning
`ai_place(ref)`: for each squad of the reference (encounter, squad or platoon) with an actor variant, `aidef_squad_spawn_count(sq, difficulty)` actors are created at a
deterministic random subset of the squad's starting locations (cyclic reuse with a 0.4 wu offset when the count exceeds them); the object definition is
`scen_find_def(variant.unit_tag)` (missing: counted `st_skipped_no_def` + log, the squad is skipped; **0 missing on a10**), team = the encounter's when set else the biped's
default, weapon / grenades / health / shield by `units_apply_variant` (variant values scaled by the globals' difficulty table: enemy / friend vitality, shield, recharge; projectile
error; damage modifier). The initial behaviour comes from the squad's initial state (sleeping -> `AIS_SLEEP`, guarding / none -> idle at the start location, moving ->
patrol over the squad's move positions or its starting locations (loop, back and forth, random), searching -> hunt, fleeing -> flee), the return state from the squad's return state,
the start location's command list is run as a *soft* list. Squads with a delay time spawn that many seconds after the placement (`ai_timer_expire` forces them; the flag
"no timer delay" cancels the delay). Squad flags / orders set by scripts before placement are applied to the actors. `ai_place` is idempotent while the squad's actors live and places a fresh set
after they died / were erased. Encounters of another BSP are pending (above). Placement snaps the start to the navigation mesh (`path_locate_ex`) when it lies on it.

### Perception (per actor every 4 ticks, every 3 in combat, staggered by actor index)
Targets are the player and every live unit of a hostile team (table built once per tick; hostility = `units_teams_hostile` with the relations overridden by `ai_allegiance*`).
Per look an actor takes at most the **4 nearest** candidates inside its cones and ray-tests them (`coll_ray`, BSP only) in that order:
* range `max_vision_distance` (halved while sleeping); the cone: angle to the body's facing within `central_vision_angle` = full rate, within `max_vision_angle` = half, within
  `peripheral_vision_angle` and `peripheral_distance` = quarter (the angles are used as **half**-angles; the target the actor already fights is seen within 12 wu from any side);
* a sighting accumulates `notice += dt * rate * (1 - 0.4 d / range) / t` with `t` = the actor's `combat` / `guard` / `non_combat` perception time (guard time when alert, searching or
  following; x3 while sleeping); at 1 the target is *acquired*: the first burst is delayed by the variant's first-burst delay x the difficulty's new-target delay, and the actor's
  squad mates (same encounter, same squad or within 30 wu), neighbours of its team (10 wu), the linked encounters (`ai_link_activation`) and the squad's maneuver squad are alerted (suspicious, they search the spot);
* a lost target is remembered 2 s (the actor keeps fighting towards its last position), then searched for 12 s, then forgotten; squads flagged "never search" give up at once;
* hearing: shots are noises (the player's `GE_FIRE` events and the AI's own bursts); an unaware actor of a hostile team within `hearing_distance` (half asleep) turns suspicious towards it;
  damage taken is handled like a sighting of the nearest hostile within 60 wu (the shooter, whether or not it is in view);
* `ai_magically_see_*` acquire directly; blind / deaf / braindead / "ignore all" flags switch senses off.
Gap: sight and fire ignore **objects** (crates, scenery with collision): the line of sight is the BSP's, projectiles are stopped by objects, so an actor may waste a burst on cover.

### Behaviours (per actor state machine, `think()`)
Priority: braindead (nothing) > command list (forced lists always; soft lists only while the actor is not fighting) > sleeping > combat variants > alert / search > calm (orders, return state).
| State | Behaviour |
|---|---|
| idle | stand at `home` facing its yaw; walks back when pushed > 1.4 wu away |
| patrol | walk between the squad's move positions (weights ignored) or starting locations, dwell 2-5 s (the move position's times when set) |
| alert | face the noise / last position for 2.5 s, then search it |
| search | go to the last known position, look around until the 12 s are over; with no target at all (initial / return state "searching", orders attack): **hunt** the nearest hostile within 70 wu (its position is used as a scent, 1.5 s refresh) |
| combat | see below |
| charge | combat with melee range, for berserk actors and species that always charge (hunters, flood forms) |
| retreat | `ai_retreat`: fall back to the squad's home while fighting back (fighting withdrawal), then hold (orders become defend) |
| flee | run away from the threat to the best of 8 sampled points 12 wu around (far from the threat, on the mesh), re-chosen every 1.5 s; used by unarmed species (crew, engineers) as soon as a hostile is aware, and by badly hurt actors for 5 s (below) |
| follow | friends follow the player (or a squad: `ai_follow_target_ai`): slot in a fan behind the leader (angle 0.8 rad apart, radius 2.2 + 0.35 rank), walk when within 6 wu, hold facing the leader's facing within 3.5 wu; a follower more than 16 wu from its leader and 8 from the enemy breaks off the fight to catch up |
Default following: human-team actors in idle with no orders start following the player once he is within 15 wu (`follow_default`, on; `ai_follow_target_disable` / `ai_defend` stop it).
Species constants (the actor tag's behaviour thresholds are raw words, docs/25; none of them is read): retreat below this fraction of health + shield: elite 0.18, grunt 0.45 (also when alone and below 0.75),
jackal 0.35, marine 0.25, crew 0.30 (flee as soon as unarmed and threatened), hunters / flood never; retreat lasts 5 s, then 5 s cooldown before the next. Berserk (script) never retreats.

### Combat (`do_combat`)
* **Aim**: eye -> chest of the target, plus lead (`target_leading` + difficulty, projectile speed of the weapon's trigger 0, capped 45 ticks), plus the burst offset; the aim angles are smoothed with a rate
  from `target_tracking` + difficulty (0.15..1 per tick). The body turns at the biped's rate (unit layer).
* **Bursts**: wait (first burst delay) -> fire for `burst_duration` -> pause `burst_separation` x difficulty burst-separation scale -> ...; the offset of a burst starts at `burst_origin_radius` x the
  difficulty's burstiness (random direction) and decays 4 % per tick; the actor fires only when it sees the target, is within `max_firing_distance` and faces it within 0.4 rad; the variant's
  rate of fire below the weapon's makes `UC_FIRE` pulse (duty = rof / weapon rate); the difficulty's rate-of-fire / projectile-error scales are applied by the unit layer.
* **Position**: fighting positions come first: the encounter's firing positions are scanned 48 at a time (rotating window, distance and range only), the 3 best are ray-tested for sight to the
  target, one is chosen (not taken by a squad mate, range between 1 wu and 0.85 x fire distance, within 35 wu of the actor, preferring the middle of the desired combat range); re-evaluated every
  1.3 s. Without one: approach until within 0.7 x `desired_combat_range` max, back off below 0.75 x min, otherwise hold and strafe (0.5 throttle, 20-45 ticks, random pauses, probed for walls)
  and crouch by the variant's movement type (always / switch with `crouch_time` / `run_time`; initial chance).
* **Reload** when the magazine is empty and has reserve (only for weapons that spend rounds: the energy weapons list a magazine with 0 rounds per shot and are never "empty");
  **grenades** every `grenade_check_time` when the target is within the grenade range, with chance x difficulty scale, encounter timeout x difficulty timer; **melee** within 1.15 wu (unarmed, out of ammo,
  melee species, or within 0.7 wu), cooldown 1.2 s x difficulty melee scale.

### Movement
`nav_move(goal, radius)`: a route from `path_route` (kinds walk / step / vault / drop (<= 1.5 wu) / climb; radius of the unit; headroom of its crouch height; expansion cap 400) cached as 6 waypoints (a
longer route is re-planned from the last one), re-planned at most every 10 ticks and when the goal moved more than 2 wu, **at most one route per tick** (`max_routes_per_tick`, rotating start so nobody starves;
deferred requests are counted), skipped when the goal is within 5 wu and a sphere sweep is clear; failures / off-mesh actors fall back to a straight line (counted). Vault and jump links set `UC_JUMP` near the landing
point, ladders hop the actor to the top (`st_climbs`, counted, not an assist). Separation: other actors within 0.8 wu push the heading. Stuck detection every 15 ticks (moved < 0.2 wu): 1 s re-plan
(`st_stuck`), 3 s teleport to the next waypoint (`st_assists`: **the gate reports them**). Heading mode turns in place first when the goal is more than 1.3 rad off; strafing mode (combat) decomposes the velocity against the body yaw.
Distance LOD: an actor further than 120 wu from the player that is not fighting, forced active or on a forced list idles (`st_dormant_ticks`).

### Command lists
`ai_command_list` (forced), `ai_command_list_by_unit` (forced, attaches a brain to a plain unit), start locations' lists (soft: interrupted by combat, resumed after). At most 8 atoms per tick; every atom either runs or
completes at once, an unsupported one is counted (`atom_skipped[type]`) and completes. Parameters are the tags' (units: seconds, wu); the atom fields not used by a type hold stale values (docs/25), so only the documented ones are read.
| Atom | Runtime |
|---|---|
| pause, wait | wait `param1` s (min 1 tick) |
| go to, go to and face | route to point 1 within the list's radius (0.55, `set radius`), timeout 40 s (counted); *and face*: then turn to point 2 (1.5 s max) |
| look, look player, look object | face point 1 / the player / the object (by the hook) for `param1` s (min 0.1 s), the look persists |
| look random | a random point of the range point 1..point 2 (or a random yaw around home) for a random time in [`param1`, `param2`] |
| crouch, shoot, grenade | crouch / fire at point 1 / throw for `param1` s (min 0.3 / 1 / 1 s) |
| animate | `units_custom_animation` of the AI animation reference (graph tag + name); waits its length; counted when it cannot be resolved |
| script | hook; loop | jump to the command (an infinite loop of instant atoms yields after 8 per tick) |
| die | `units_kill` (modifier 1 = silent); move immediate, teleport | `units_teleport` to point 1; set radius |
| animation mode, action, vocalize, targeting, initiative, vehicle | complete at once (counted as instant, no effect) |
| move in direction, running / targeted jump, recording | unsupported: complete at once, counted (none occurs on a10) |
A list that reaches its end finishes (`ai_command_list_status` = 2); the actor stays where it ended (that becomes its home). `ai_command_list_advance` skips the current atom.
Status: 0 no list, 1 running, 2 finished (also when the unit is gone, so `sleep_until (= status 2)` cannot hang on a dead actor); the built-in over an object list is 1 while any unit runs.

### Conversations
`ai_conversation(c)`: status 1 (starting): each participant is resolved to the nearest living actor of its encounter (matching actor type preferred), 5 s to find at least one, else aborted (6); status 2 playing: lines in order, the line
delay (s) before each, the speaker's object, the take = the sound whose index matches the speaker's dialogue variant in the participant's variant list (else the first), the sound hook gets
`AI_SND_LINE_START` and returns the length (else 3 s); a missing speaker skips the line; at the end `AI_SND_CONV_END`, status 4 for one tick, then 5 (done). `ai_conversation_stop` ends it at once (5),
`..._advance` cuts the current line, `..._line` is the line index. Scripts test `< 4` (running) and `> 4` (over) on a10 / a30: the status values are chosen for that. Proximity triggering (`trigger_distance`) is
**not** implemented (the flags are unverified, scripts start every conversation of a10 themselves); `ai_dialogue_triggers` only stores the flag.

## Script built-ins
Argument types and result types from `reports/script-usage.md` and verified on the call sites of all ten maps (argument order checked). `ai` values: bits 0..15 encounter, bit 31 + 16..30 squad of the encounter,
bit 30 + 16..29 platoon (matches every constant of a10). Object arguments go through `mission_resolve_object` / `mission_arg_list`; `ai_actors` returns a mission list of object slots; no ai built-in returns a single object.
| Status | Built-ins |
|---|---|
| implemented | `ai`, `ai_place`, `ai_actors`, `ai_living_count`, `ai_living_fraction`, `ai_nonswarm_count`, `ai_erase`, `ai_erase_all`, `ai_kill`, `ai_kill_silent`, `ai_migrate`, `ai_migrate_by_unit`, `ai_spawn_actor`, `ai_renew`, `ai_timer_expire`, `ai_free`, `ai_free_units`, `ai_attach`, `ai_attach_free`, `ai_detach`, `ai_teleport_to_starting_location`, `ai_teleport_to_starting_location_if_unsupported`, `ai_attack`, `ai_defend`, `ai_retreat`, `ai_follow_target_players`, `ai_follow_target_ai`, `ai_follow_target_disable`, `ai_berserk`, `ai_braindead`, `ai_braindead_by_unit`, `ai_set_blind`, `ai_set_deaf`, `ai_disregard`, `ai_try_to_fight`, `ai_try_to_fight_player`, `ai_try_to_fight_nothing`, `ai_force_active`, `ai_allow_dormant`, `ai_grenades`, `ai_magically_see_players`, `ai_magically_see_encounter`, `ai_link_activation`, `ai_allegiance`, `ai_allegiance_remove`, `ai_allegiance_broken`, `ai_command_list`, `ai_command_list_by_unit`, `ai_command_list_advance`, `ai_command_list_status`, `ai_conversation`, `ai_conversation_stop`, `ai_conversation_advance`, `ai_conversation_status`, `ai_conversation_line` |
| partial (semantics approximated, see below) | `ai_status`, `ai_strength`, `ai_maneuver` (hunt orders + sees the players), `ai_migrate_and_speak` (migrates, says nothing), `ai_playfight` (flag only: actors still fight normally), `ai_dialogue_triggers` (flag only) |
| counted no-ops (right result type) | `ai_maneuver_enable`, `ai_follow_distance`, `ai_prefer_target`, `ai_automatic_migration_target`, `ai_reconnect`, `ai_go_to_vehicle`, `ai_going_to_vehicle` (0), `ai_exit_vehicle`, `ai_vehicle_encounter`, `ai_vehicle_enterable_distance`, `ai_vehicle_enterable_disable`, `ai_vehicle_enterable_actor_type` |
Result conventions for `sleep_until`: counts are exact (`ai_living_count`; real or short as the call site wants); `ai_living_fraction` = living / actors placed since the last erase (0 before anything was placed);
`ai_strength` = mean (health + shield) / maximum of the living (0 when none); `ai_status` 0 none, 1 idle / patrol / follow, 2 alert, 3 searching, 4 some fighting, 5 half or more fighting, 6 all (>= 2 actors) fighting
(**invented ladder**: b40 / a50 compare against 2, 4, 5, 6; a10 does not call it). Semantics that are guesses: `ai_magically_see_encounter (a, b)` = the actors of `a` see `b`'s nearest; `ai_link_activation (a, b)` = `b` is alerted
when `a` is first activated (an actor of `a` acquires a target); `ai_try_to_fight (a, b)` = `a` attacks and sees `b`; `ai_allow_dormant false` = forced active; `ai_defend` = hold where they are; `ai_retreat` = fall back to home;
`ai_migrate` = the actors take the destination squad's behaviour and one of its starting places (a squad without places only changes behaviour).
With `mission_t.ai == NULL` every built-in is a no-op returning the neutral value; unresolved references never fail.
Test result of the soak (below): the real a10 machine with every dormant script woken calls 14 of the built-ins (e.g. `ai_actors` 8729 x, `ai_command_list_status` 238 x polling) with 0 script errors.

## Cost (host, `-O2`, Apple Silicon; the tests measure themselves, `make -C engine/tests ai` prints the sanitizer build, which is ~30x slower)
Crowds of every encounter of a BSP up to 20+ actors, 600 ticks after 60 ticks of settling, the player among them; actors die during the run (alive at the end in brackets):
| BSP | actors placed (alive at end) | `ai_tick` avg | `ai_tick` worst | whole tick (AI + units + game + objects) avg | routes | rays |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 17 (7) | 5.6 us | 74 us | 19.6 us | 102 | 3104 |
| 1 | 31 (17) | 8.9 us | 55 us | 43.1 us | 111 | 7032 |
| 2 | 25 (18) | 9.9 us | 86 us | 51.0 us | 120 | 9042 |
About **0.3-0.5 us per actor-tick** on average. On the EE (30-100x slower): average 0.2-1.0 ms per tick for 16-30 actors, **worst ticks 1.7-8.6 ms** (one route search plus up to 19 sight rays on the
same tick; the A* is the largest single item, capped at one per tick and 400 expansions, deferred requests wait for the next tick). Stagger / caps in place: looks every 4 / 3 ticks per actor, at most 4 rays per look, firing-position scan windows of 48, one route per tick, dormant actors beyond 120 wu.
The brief's 2.5 ms EE budget for 16 actors holds on average with margin; the worst tick can exceed it by a factor of 2-3 on the slow end of the range: the route cap is already 1 per tick, so the remaining levers are `path_query.max_expand` or
spreading the search over ticks (not done). **Not measured on the EE.**

## Tests (`engine/tests/test_ai.c`, all on the real a10 packs, ASan / UBSan build, 22 s)
1. references: every encounter, squad and platoon of a10 round-trips; one past the end, none, stray patterns are rejected.
2. spawning on BSP 0..2: counts equal the Legendary counts, health = variant x the table's factor (friend / enemy), actors on the navigation mesh, units attached, place twice = nothing, kill, erase, place again; all four difficulties spawn;
   pending placement for another BSP and the placement when its BSP arrives, frozen actors of the other BSP, initial placement.
3. hostile pairs (an armed human encounter and a covenant one per BSP, one of each put in sight of the other, everything else where it was placed), 30 s: noticed after 42-45 ticks, first shot at 52-61, first damage 52-60,
   first death at 62-179 (deaths 2+1, 0+3, 1+1), actors travelled up to 32 wu; routes 16-41, stuck 1-4, assists 0-2 (the BSP 2 run had 5 failed routes: actors off the mesh use a straight line).
4. the player as a target (noticed in 44 ticks, hits him, nobody targets him after he dies), friends following the player (mean distance 16.6 -> 3.5 wu in 25 s).
5. every command list of a10 on BSP 0..2 (80 of the 124 belong to BSP 0..2) run on a test actor for 25 s: 19 finish, 61 loop; 1494 atoms run, **no unsupported atom occurs**; a list's first two go-to points reached in order (230 ticks, no assist).
6. a conversation with resolvable participants (1 line spoken: only one participant of that one resolves; hook, status 5 and end events checked; stop in the middle).
7. determinism (two runs, same hash), snapshot / restore (above), corrupt states refused; built-ins: 24 of 73 called directly with results and effects checked, 4000 random-argument calls over all 73 + 90 ticks (no sanitizer report), the state still rebinds;
   soak: 3000 ticks of the real a10 script machine (H1 object built-ins + the ai ones, everything else neutral, every dormant script woken at tick 90): 0 script errors.
Without `ai/units.c` the behavioural tests are skipped (the fixture's stand-in cannot move or shoot).

## Unverified or approximated
* Perception: cone angles as half-angles, the three-level rate, notice time formulas, memory 2 s + search 12 s, hearing by raw distance: from the named fields and common sense, not from data. The actor tag's behaviour thresholds (docs/25 raw words)
  are **not read**: retreat fractions per species, panic, charge rules, search times, grenade stimulus / min-enemies are my constants (`species_of`, header constants of ai.c).
* Burst model (offset radius x burstiness, decay, pulse duty), aim smoothing, crouch timers, strafing: designed here.
* Squad semantics: `delay_time` counted from the placement (the data may start it at activation), `maneuver_squad` = alerted when the squad acquires a target, `masks`, platoon words, `w*` words ignored, respawn rules absent from the data.
* Command atom parameters (`pause` / `look` times in `param1`, `look random` bounds, point ranges, `modifier` ignored everywhere) are the common reading; stale fields are never read. Status values of `ai_command_list_status` (0 / 1 / 2:
  a10 compares with 1 and 2), `ai_conversation_status` (< 4 running, > 4 over: a10 / a30 compare so) and `ai_status` (invented, see above).
* The enemy damage multiplier of the difficulty table is applied by the unit layer (`dmg_scale`), not here; `ai_try_to_fight*`, `ai_playfight`, `ai_prefer_target` are approximations or flags.
* Sight and shots do not see objects (above); vehicles are not supported (actors in vehicle seats idle); no unit-vs-unit collision except my 0.8 wu separation.
* Dialogue take selection (`variants[]` of the participant against the biped's dialogue variants) is a guess: with no match the first take plays.
* Conversation / command-list `object_name` look targets need the object hook (without it the look is skipped).
* Host numbers only; nothing run on the EE.

## ADR proposal
**ADR-04x: The AI is a pointer-free state machine per actor on top of aidef + path + units; it writes unit controls only.**
*Decision*: `ai/ai.[ch]` keeps all mutable AI state (actors, squads, encounters, conversations, profile cache, noise list, relations, counters) in one 56 KB pointer-free `ai_state` that checkpoints copy as bytes and `ai_rebind`
re-derives and validates; per actor a behaviour state (sleep / idle / patrol / alert / search / combat / charge / retreat / flee / follow) with command lists above it; perception staggered over 3-4 ticks with at most 4 rays per look;
movement by `path_route` (one route per tick, 6 cached waypoints, stuck detection with counted assists); combat from the named fields of the variant and the difficulty table (bursts, aim, grenades, melee); the `ai_*` built-ins map onto
`ai_*` functions through `mission_t.ai`. *Why*: the original AI is a large hierarchical planner whose thresholds are not in the data (raw words); a small robust machine over the named fields reproduces the visible behaviours (notice, approach,
take position, burst fire, retreat, search, follow, scripted lists) within the EE budget, is bit-reproducible (tests, saves, the bot) and leaves content-specific tuning to constants in one place. *Consequences*: behaviour is plausible, not
identical; sight ignores objects; the unverified items above are the places to tune once the gate shows what matters; per-tick cost is about 0.4 us per actor on the host (an estimated 0.2-1 ms on the EE, spikes of one route + a few rays).
