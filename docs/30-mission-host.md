# Phase 4 stage 2: units, AI runtime and the script host

Stage 1 delivered the data and the pure runtimes (script machine `script/hs.[ch]`, AI data `ai/aidef`, pathfinding `ai/path`, devices `sim/devices`,
cinematics `sim/cine`, streamed audio `audio/stream`). Stage 2 connects them to the game. The integrator (main session) owns `sim/mission.[ch]`
(the shared state, handle encodings and object helpers: read `engine/src/sim/mission.h` first, it is the contract), `main.c`, the level flow
(BSP switching, saves) and the mission bot. Four workstreams run in parallel against that header:

| # | Name | Files you create | Doc |
|---|---|---|---|
| U | Units (locomotion, weapons, animation, death) | `engine/src/ai/units.c`, `engine/tests/test_units.c` | 31 |
| A | AI brain (spawn, perception, behaviours, movement, combat, command lists, conversations) and the `ai_*` built-ins | `engine/src/ai/ai.[ch]`, `engine/src/script/hs_host_ai.c`, `engine/tests/test_ai.c` | 32 |
| H1 | Script host: object, unit, player, device, effect built-ins | `engine/src/script/hs_host_obj.c`, `engine/tests/test_host_obj.c` | 33 |
| H2 | Script host: camera / cinematic, sound, hud / ui, game flow, debug built-ins | `engine/src/script/hs_host_misc.c`, `engine/tests/test_host_misc.c` | 34 |

Shared rules (all four): Phase 3 conventions (`docs/14-phase3-plan.md`) and the Phase 4 conventions (`docs/23-phase4-plan.md`: C11, no `double`, no heap,
host tests against the real a10 packs that skip when `assets/` is missing, never run `make -C engine`, never commit, small additions only to
`engine/tests/Makefile`, keep content names/strings out of docs and tests: legal posture). Read the existing headers rather than guessing:
`sim/game.h`, `sim/units.h`, `sim/mission.h`, `sim/objects.h`, `sim/devices.h`, `sim/cine.h`, `script/hs.h`, `ai/aidef.h`, `ai/path.h`,
`world/devdef.h`, `world/cinedef.h`, `audio/stream.h`, `sim/unitdef.h`, `sim/weapdef.h`, `world/ansm.h`, and the format docs 24-29.
Where a header is missing something you need, add a function to it in the smallest possible way (and say so in the report); do not rewrite
other workstreams' code. The tree is being edited concurrently: re-read a file before editing it, keep edits local.

**State and saves.** Checkpoints and memory-card saves copy mutable state as raw bytes (see `sim/save.h`). Everything you add to the mutable state
(units, AI actors, squads, timers) must be plain data without pointers (indices, fixed arrays, fixed-point or float values), live in a struct
that the integrator can embed in the checkpoint (`sizeof` stated in your report, target <= 80 KB for U, <= 160 KB for A), and be rebindable
(`units_rebind` / `ai_rebind` style function taking the immutable data pointers). Test snapshot/restore determinism: run N ticks, copy the state
out, run M more ticks and record a hash, restore the copy into fresh state and check the same hash after M ticks.

**Tick order** (mission_tick): sync player body -> script machine -> devices -> AI (decisions: sets unit controls) -> units (apply controls, move,
shoot, animate) -> game (player, weapons, projectiles, damage) -> cinematics. 30 Hz fixed. Budgets on the EE (30-100x slower than the host): the
whole AI + units step must stay under about 4 ms per tick with 16 active actors; the script machine ~0.3 ms; say what your host measurement
implies.

**Difficulty**: the game has `game_t.difficulty` (0 easy .. 3 legendary) and the UNIT section's difficulty table (`unitdef_difficulty`):
enemy damage, vitality, shield, recharge, rate of fire, projectile error, burstiness, target tracking/leading, grenade chance... The gate runs on
Legendary (3): apply the table where the AI uses the corresponding quantity.

## U. Units (`sim/units.h` is the contract)
Implement `ai/units.c`: `units_attach` (reads the object's biped via `unitdef_biped`, sets up the body, `player_params_t` from the biped tag using
the walking speeds of the *actor* (root-motion `anim_speed[]` of the biped, run/walk/crouch), health and shield from the actor variant when given
else the collision model, weapon), `units_tick` (apply control: body turns to `aim_yaw` at the biped's turn speed; throttle -> velocity with
acceleration; collision against the BSP through the same capsule controller as the player (`player_tick` with a `player_cmd_t` built from the
control: reuse it, do not duplicate physics); jump, crouch; fall damage if cheap), weapons (`weapon_fire_step` with the unit's weapon def, `game_fire_projectile`
with owner = object slot, error from the variant's projectile error and the difficulty table, burst control is the AI's job: you just fire while
UC_FIRE is held), grenades (spawn the grenade projectile like the player's), melee (damage via `game_damage_object` in front of the unit),
animation selection from the ASTB tables (`ansm_lookup`: idle, move-front/back/left/right, turn, airborne, land, fire, reload, melee, throw,
death by the damage table (`ansm_damage_anim`), flinch) written into the object's `anim`/`frame` (object animations advance in `objects_tick`:
set `o->anim`, reset `frame` when the action changes, mark `OBJ_F_ANIMATED`; a unit can also drive a one-shot animation: `UA_CUSTOM` with `custom_anim`),
death (when the vitals die: death animation, drop the weapon as a pickup object if the biped does, stop the unit; `units_kill`), flinch on damage,
team table (`units_teams_hostile`), `units_teleport`, `units_give_weapon`, `units_eye`. Recorded animations (`cinedef_ctl`: throttle x/y, aim, flags,
weapon) map onto `unit_control_t` (provide `units_apply_cinedef(unit, const cinedef_ctl*)` in `units.h`). Players' bodies: the player is not a unit
(its biped object is a hidden placeholder), but units must see it as a valid target position (the AI queries it separately).
Tests: spawn the a10 crewman and a Covenant biped from the real packs, walk them with scripted controls through BSP 0, check they stay out of
solid space, turn rate and speeds match the tags, animations are selected (names), weapons fire projectiles that damage a target, death
works, 96 units tick under 0.5 ms host, snapshot determinism.

## A. AI brain (`ai/ai.h` is yours to design; keep it small and pointer-free)
Implement what the mission needs from the data in `aidef` (docs/25) with the pathfinding of `ai/path` (docs/26) and the units of workstream U:
- **Spawning**: `ai_place(encounter or squad)` creates the squads' actors at their starting locations for the difficulty (`aidef_squad_spawn_count`),
  with the actor variant of the squad's palette entry (unit tag -> a biped definition in SCEN: check `scen_find_def`; if the definition is missing, report it
  in the log and skip: the integrator extends the SCEN roots), the weapon and grenades of the variant, health/shield from the variant scaled by the
  difficulty table, initial state (sleeping, alert, moving, guarding, searching...), command list, team from the encounter. Encounters not
  "initially created" wait for a script; others are placed at level start (`ai_place_all_initial`). Squad/encounter names: `aidef_find_*`.
- **Perception**: per actor, staggered over ticks (each actor refreshes every 3-6 ticks), vision cone/range from the actor tag, hearing from gunfire
  events, line of sight with `coll_ray`, memory of the last known position and time, "magically see" (script) and alert propagation inside an
  encounter. Targets: the player and all hostile units (`units_teams_hostile`).
- **Behaviours** (a state machine per actor, simple and robust): idle/guard, alert, combat (choose a firing position from the encounter's firing
  positions in range and with line of sight; approach, crouch/strafe, burst fire with the variant's burst timing, reload, throw grenades per the
  variant's grenade rules, melee when adjacent for melee species), retreat when health/shield low (fall back to cover / away), charge for
  berserk-prone species, search when the target is lost, follow-the-leader for squads and friendly marines (stay within a radius of the player or
  their squad leader when they have no hostile in view), maneuver to move positions, flee, vehicle stubs (actors that are in vehicles keep
  idle). Command lists: `ai_command_list` runs atoms (go to, go to and face, pause, look, crouch, shoot, animate, script wake, wait, loop...); all
  atoms of the real maps must at least advance (unsupported ones complete immediately and are counted).
- **Movement**: `path_route` between the actor and its goal, re-planned at most every 10 ticks and when the goal moves more than a few units,
  waypoint following with `unit_control_t` (throttle, aim), stuck detection (unit not progressing for 1 s: re-plan; after 3 s teleport to the
  next waypoint, counted), blocked-door awareness is not needed (doors do not block).
- **Conversations / vocalizations**: `ai_conversation` start/stop built-ins call a hook (`mission` has `stream`/`gameaudio`; expose a callback in
  `ai.h` `ai_set_sound_hook(fn)` the integrator wires: dialogue lines with participants, delays, "run to player" distance; conversations may be
  started by script or by proximity per the data).
- **Script built-ins** (`script/hs_host_ai.c`: `void hs_host_register_ai(hs_machine *m)`): all `ai_*` and `ai`-domain built-ins that appear on a10 (the list
  with call counts is in `docs/reports/script-usage.md`, domain `ai`; 37 distinct on a10), plus the others of the union that are cheap; semantics
  from the built-in vocabulary (e.g. `ai_place`, `ai_kill`, `ai_erase`, `ai_actors`, `ai_living_count`, `ai_living_fraction`, `ai_command_list`,
  `ai_attack`, `ai_defend`, `ai_retreat`, `ai_follow_target_*`, `ai_magically_see_*`, `ai_link_activation`, `ai_allegiance`, `ai_conversation*`,
  `ai_set_orders`...). Each returns sensible values for `sleep_until` conditions (counts and fractions are the usual ones: `(<= (ai_living_count x) 0)`).
  Keep a table of which built-ins are implemented, partially, or as no-ops in your doc.
Tests: from the real a10 data spawn several encounters on BSP 0..2 positions with the matching COLn / PATn sections loaded, put a hostile unit pair
in sight of each other and run 30 s: they must notice, approach, shoot, take damage and die (stats printed); a squad with a command list follows it;
perception cost and per-tick cost for 16 actors (host µs); determinism and snapshot/restore as above; fuzz the sleep_until-style queries.

## H1 / H2. Script host built-ins
Each built-in is a function `int fn(hs_machine *m, void *host, const hs_value *args, unsigned nargs, hs_value *ret)` (host = `mission_t *`), registered with
`hs_register(m, "name", fn)` by a `hs_host_register_*(hs_machine *m)` function you export. Domains and argument types per built-in: `docs/reports/script-usage.md`
(and docs/24 for the value-type numbers and the handle conventions in `sim/mission.h`). Implement **every built-in of your domains that a10 calls**
(the list sorted by call count is in `docs/24-script-section.md`), and the cheap ones used by other maps; the rest stay counted no-ops (the machine does that). Pure
data-lookups (names, volumes, cutscene flags, camera points) go through DEVS/CINE/AIDT with the helpers of `mission.h`; things that need a module that
is not wired yet (stream player, AI) go through the pointers in `mission_t` and must tolerate NULL (no-op). Return values must be right for the
conditions scripts wait on (`sleep_until`): e.g. `volume_test_objects` returns whether any object of a list is inside a volume, `sound_impulse_time` returns the
remaining ticks of a dialogue, `device_get_position`, `unit_get_health`, `objects_distance_to_object`, `players`, `list_get`/`list_count`... Blocking built-ins
(`sound_impulse_time`-style waits are NOT blocking; `sleep` is the machine's) return 1 only where the original blocks.

- **H1**: object (create / create_anew / destroy / teleport / attach / detach / set_scale / hide / get_position / distance / can_see / list / ...), unit (seats and
  vehicles are stubs that still return plausible values: `unit_set_seat`, `vehicle_load_magic`... make the unit appear in the vehicle object's location
  or ignore, documented), player (`players`, `player_enable_input`, `player0_*`, camera and control locks: set `mission` flags the main loop honours: add
  `player_input_enabled`, `player_look_enabled` fields to `mission_t` if you need them), device (`device_set_position`, `_power`, `device_group_*`, `device_get_position`...),
  effects (`effect_new`, `effect_new_on_object_marker`, `damage_new`, `object_set_...`): spawn through `fx_spawn` / `game` damage; marker positions via the model markers.
  Also `custom_animation` / `unit_custom_animation` (units + `UA_CUSTOM`) and `recording_play` / `recording_kill` / `recording_time` (CINE recordings drive units through `units_apply_cinedef`).
- **H2**: camera / cinematic (`camera_set`, `camera_control`, `camera_set_first_person`, `cinematic_*`, `fade_*`, letterbox, `cutscene_*`, titles, `camera_time`, `camera_predict_resources`
  as no-op...) through `sim/cine.h` and `mission.cinematic`, sound (`sound_impulse_start/stop/time/predict`, `sound_looping_start/stop`, `sound_looping_set_alternate` through the stream
  player and `gameaudio`; positional impulses through `gameaudio_play`; dialogue through `stream_*`; all NULL-safe), hud / ui (`hud_set_help_text`, `show_hud_help_text`,
  `enable_hud_help_flash`, objectives, `hud_*`, `show_hud`, `cinematic_show_letterbox`, navpoints (a small navpoint table in `mission_t`: add fields if needed; the HUD draws them
  via the `hud` module: leave the drawing to the integrator but keep the data), game flow (`game_difficulty_get`, `game_won`, `game_lost`, `game_save*`, `game_revert`, `switch_bsp` /
  `scenario_switch_bsp` via `mission.want_bsp`, `mission_objectives`-style built-ins, `core_save`, `quit`...) and the debug / no-op family (print, `object_pvs_*`, `*_predict`, `rasterizer_*`, `render_*`: explicit no-ops that return the right type).
Tests: a `mission_t` built from the real a10 packs (see `engine/tests/test_game.c` for loading; build helpers in a shared test header
`engine/tests/mission_fixture.h` that H1 creates first and H2 reuses: whichever of you starts first writes it, the other waits for the file or writes
its own copy under a different name), call each built-in with real arguments and check effects (objects created, positions, device states, list contents, flags, cine state), and
a **soak run**: register your domain's built-ins, run the real a10 script machine for 9000 ticks (5 minutes) with the player idle and report counts of calls per built-in, unregistered built-ins hit,
errors, and anything that blocks forever.
