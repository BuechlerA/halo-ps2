# Script host H1: object, unit, player, device and effect built-ins

Phase 4 stage 2, workstream H1 ([contract](30-mission-host.md)). Code: `engine/src/script/hs_host_obj.[ch]` (`void hs_host_register_objects(hs_machine *m)`),
state in `mission_t.hobj` (`sim/mission.h`, type `mission_hostobj`), tests `engine/tests/test_host_obj.c` on the shared fixture
`engine/tests/mission_fixture.h` (`make -C engine/tests host_obj`, part of `make -C engine/tests`). Pure C11, no heap, no `double`. Legal posture as in
[23](23-phase4-plan.md): only built-in names (the engine's vocabulary) and aggregate numbers appear here; the tests find their objects, volumes, flags,
devices, animations, recordings and damage effects by *structure* in the real a10 packs and never name content.

103 built-ins are registered. Every call of the object / unit / player / device / effect domains that a10 makes is implemented (the soak below lists what
a10 hits: no built-in of these domains is left unregistered), plus the cheap ones of the other maps and a few extras the brief named
(`objects_distance_to_object`, `objects_distance_to_flag`, `object_get_health`, `unit_kill`, `unit_kill_silent`).

## Wiring (integrator)
1. `mission_tick` order becomes: `mission_sync_player` -> `hs_tick` -> `devices_tick` -> **`hs_host_obj_pre(m, in)`** -> AI -> `units_tick` -> `game_tick`
   -> **`hs_host_obj_post(m)`** -> `cine_tick`. `pre` feeds the running recordings to their units (`units_apply_cinedef`, flag `UF_NO_AI` while a
   recording runs, the unit is released when it ends) and latches the player's input for `player_action_test_*`; `post` moves attached objects with
   their parent's marker, keeps `unit_suspended` units in place and hands a swapped animation graph back. `mfix_tick` in the fixture is this order.
2. `hs_host_register_objects(hs)` before `hs_init`. H2 registers its own domains; **`object_beautify` and `player_effect_*` exist in both** (H2's
   `player_effect_*` is the full one, H1 deliberately does not register them; `object_beautify` is an identical no-op): register H1 first, H2 second.
3. Flags the main loop must honour (`m->hobj`): `player_input_disabled` (`player_enable_input false`: feed neutral input), `player_look_disabled`
   (`player_camera_control false`: ignore the look stick), `pitch_inverted` (`player0_look_invert_pitch`), `joystick_not_normal`. Zero = enabled.
4. Anything other than the scripts that destroys an object (AI erase, BSP switch cleanup) should call `hs_host_obj_forget(m, slot)` first (attachments,
   seats, flags and recordings of the slot go away); `object_destroy` does it itself. `mission_hostobj` is plain data (9,296 bytes, embed it with `mission_t`
   in checkpoints); `cine_state` (recordings) holds pointers into the CINE section and needs the cine module's rebind after a restore.
5. Fields the other modules read: `hobj.melee_inhibited` / `hobj.ranged_inhibited` (bit sets over object slots: AI must not melee / shoot with those
   units), `hobj.no_drop` (also `UF_NO_DROP` on the unit), `hobj.not_enterable` (the player may not enter that vehicle), `hobj.cannot_take_damage`.

## Handles: a change to `sim/mission.[ch]` (please review)
The compiled scripts carry **object names as constants typed object / unit / list** (value types 37, 38, 23 and 39..43; `object_destroy` has 163 such constant
calls on a10, `recording_play` 59, `custom_animation` 103), and a global typed unit can hold either a name or a value computed by `list_get`; the machine
cannot tell them apart (both are `HS_HANDLE`, `vtype` 37/38). The old rule (only 39..43 are names) therefore read most constants as slot numbers. New rule:
**a handle produced at run time carries `MISSION_OBJ_BIT` (0x40000000) | slot** (`mission_obj_handle(slot)`; list *elements* stay raw slots, a list handle
keeps bit 31), **a handle without the bit is a name index**. `mission_resolve_object` does this (small edit in `mission.c`, helper in `mission.h`,
the legacy raw-slot path stays for other value types). Every producer of single object handles must use `mission_obj_handle` (H1 does; `ai_*` that
return lists are unaffected). H2's host test still passes with the change.

## Semantics (what was inferred) and status
Legend: **full** = implemented as the vocabulary and the call sites suggest; **partial** = a documented part; **no-op** = explicit, returns the neutral value.
Value types and argument orders were checked against all ten maps (constant types per argument position), not guessed.

| Built-in(s) | Status | Semantics chosen |
|---|---|---|
| `players` `list_get` `list_count` | full | `players`: list with the player's body object. `list_get(list, i)`: object handle, `NONE` out of range or dead; an object argument is a one-element list. `list_count`: live objects. |
| `volume_test_objects` `_all` `volume_test_object` | full | any / all (empty = false) object of the list inside the volume; the point tested is feet + half the object's radius (`mission_object_pos`). **Awaited volumes**: whenever the player's body is in the tested list the result also goes to `mission_note_awaited_volume(m, volume, player_inside)` (ring of 8 in `mission_t`, kept by the coordinator's helper: entry stays 'awaited' while it is tested false, vanishes when true). `volume_test_object` (single object) does the same when the object is the player. |
| `volume_return_objects` | full | bipeds, vehicles, weapons, equipment (live, active BSP) inside the volume, the player's body included. |
| `volume_teleport_players_not_inside` | full | the player is moved to the flag (position and yaw) when outside. |
| `object_create` `object_create_anew` | full | spawn the named placement (`mission_object_by_name`, create = 1); a biped gets a unit if it has none; `_anew` destroys the live one first. Not-spawned names elsewhere are never created on demand (the original no-ops on a missing object; `mission_spawn_named` is only used by the create built-ins). |
| `object_create_containing` `object_destroy_containing` `object_create_anew_containing` | full | every resolved object name containing the string (substring, case sensitive). |
| `object_destroy` | full | destroys the slot, its unit, attachments, seats, flags, recordings; the player's body cannot be destroyed. |
| `object_destroy_all` | partial | bipeds, vehicles, weapons, equipment, projectiles only (scenery, devices and lights belong to the level here; the original removes everything the scenario spawned). |
| `object_teleport` | full | position and yaw of the flag; units through `units_teleport`, the player through `game_teleport`; machines and controls never move. |
| `object_set_facing` | partial | turns the object towards the flag's *position* (how scripts use it); the flag's own yaw is not used. |
| `objects_attach` `objects_detach` | partial | a table of 48 (parent, child, parent marker): in `post` the child takes the parent's marker world matrix (position **and** rotation, posed through `objects_pose`). The child's own marker argument is ignored (child origin on the marker). A child hangs on one parent. |
| `objects_can_see_object` `objects_can_see_flag` | full | any object of the list sees the target when the angle between its look direction (unit aim / player look / object yaw) and the target is within the degrees argument (taken as a half angle) and `coll_ray` from its eye finds no world surface. Objects do not block sight. |
| `objects_distance_to_object` `_flag` `object_get_health` | extra | minimum distance of the list (1e9 when none); health fraction. Not called by any map. |
| `object_set_scale` | partial | applied at once (the ticks argument is not modelled) by scaling the placement matrix; moving the object rebuilds it unscaled. |
| `object_set_permutation` | partial | MODL regions have no names: the permutation name is set in every region that has one. |
| `object_cannot_take_damage` `object_can_take_damage` `unit_impervious` | full | `UF_INVULNERABLE` on the unit **and** the game vitals are parked (`has_vitals = 0`, restored by `can_take`), so projectile hits (`game_damage_object`) and `units_damage` both do nothing. |
| `object_set_melee_attack_inhibited` `_ranged_` `unit_set_enterable_by_player` `unit_doesnt_drop_items` | partial | flags only (see wiring 5); no module reads the first three yet except `UF_NO_DROP`. |
| `object_set_collideable` `object_beautify` | no-op | |
| `unit` | full | object -> unit conversion (a handle, `NONE` when unresolved). |
| `unit_get_health` `unit_get_shield` `object_get_health` | full | fraction of the maximum (1 for an object without vitals, 0 when dead); the player uses `game_t.vitals`. |
| `unit_get_total_grenade_count` `unit_has_weapon_readied` | full | counts of the unit (the player: the game's); tag id of the readied weapon (WEAP) against the argument. |
| `unit_set_current_vitality` `units_set_current_vitality` `unit_set_maximum_vitality` | full, units unverified | **absolute tag units** (a10 passes 100, 75, 12.5-steps, 1.0): current is clamped to the maximum, health <= 0 kills (`units_kill`); maximum sets `max_health` / `max_shield`. Fraction semantics would make the 1.0 calls mean 'full' instead of 'nearly dead': not decidable from the data. |
| `unit_kill` `unit_kill_silent` | extra | `units_kill`. |
| `custom_animation` `unit_custom_animation_at_frame` `custom_animation_list` | full | exact name search in the graph given (`anim_find_graph(tag)`, then `anim_find_name`), unit: `units_custom_animation` (graph swap and return are the unit layer's), non-unit object: graph swapped for the duration and restored in `post`; unknown graph / name: nothing happens (counted in `missing_noted`). The interpolate flag is ignored. |
| `unit_stop_custom_animation` `unit_get_custom_animation_time` | full | stop; **remaining ticks** (scripts `sleep` on it: the built-in's use as `(sleep (...time...))` is what the call sites show). |
| `recording_play` `_and_delete` `_and_hover` `recording_kill` `recording_time` | full | recording index = CINE table index; `cine_play_begin` with the object slot as owner, `pre` applies the control with `units_apply_cinedef`, `UF_NO_AI` for the duration; at the end the unit is released (or the object destroyed for `_and_delete`, left under control for `_and_hover`); `recording_time` = **remaining ticks** (0 when none), same `sleep` use. The player cannot be recorded. |
| `unit_set_seat` | no-op | no vehicle argument: nothing to sit in. |
| `unit_enter_vehicle` `vehicle_load_magic` | partial | the unit is attached to the vehicle's origin (it travels with it), the seat label is remembered (16-bit hash). No seat animation, no vehicle logic. |
| `unit_exit_vehicle` `vehicle_unload` | partial | detach (all riders or those of the seat label); units stay where they are. |
| `vehicle_riders` `vehicle_test_seat_list` | full | riders as a list; whether any unit of a list sits in the vehicle (in the seat). |
| `unit_open` `unit_close` `unit_set_emotion` `vehicle_hover` `units_set_desired_flashlight_state` `unit_solo_player_integrated_night_vision_is_active` (false) `player_add_equipment` (starting profiles are not in SCEN) `device_group_change_only_once_more_set` | no-op | |
| `unit_suspended` | partial | the unit is teleported back to where it was when the call was made, every tick. |
| `player_enable_input` `player_camera_control` `player0_look_invert_pitch` `player0_look_pitch_is_inverted` `player0_joystick_set_is_normal` | full | flags in `hobj` (wiring 3). |
| `player_action_test_*` (10 tests) and `player_action_test_reset` | full | latched since the reset from the input of `hs_host_obj_pre`: accept = jump, primary trigger = fire, grenade trigger, action = switch / pick up, back = move_y < -0.5, look up / down / left / right = look stick beyond 0.3 in screen sense, all directions = all four. |
| `players_unzoom_all` | full | |
| `device_get_position` `device_get_power` `device_set_position` `_immediate` `device_set_power` | full | `sim/devices` (device by name constant or by object handle through the placement); `device_set_position` also answers `true` where a call site uses it as a bool. Scripted positions move regardless of power (devices.h). |
| `device_group_set` `_immediate` `device_group_get` | full | `_immediate` also snaps every member (position group, reversed when the placement says so; power group). |
| `device_one_sided_set` `device_operates_automatically_set` `device_set_never_appears_locked` | full | flip the machine flags in `dev_state.mflags`. |
| `effect_new` `effect_new_on_object_marker` | full | `fx_spawn` at the flag (forward = its yaw / pitch) / at the world transform of the model marker (forward = the marker's x axis); no `fx` in the mission: nothing. Decals are not made (`surface` 0). |
| `damage_object` `damage_new` | partial | `damage_object`: the damage effect's upper bound at once (units through `units_damage`, others `game_damage_object`, the player's vitals); `damage_new`: radial, `weap_damage_at_radius` with a fixed 0.5 roll over every object with vitals and the player's eye. No camera shake, no stun, no vehicles passthrough. |

## Not registered by H1 (belongs to H2 or to nobody)
`player_effect_*` (H2), `objects_predict` / `object_type_predict` / `object_pvs_*` (H2's no-op family), `scenery_animation_start[_at_frame]` (camera /
cinematic domain: no a10 call, no one has it), `ai_*` (A). Unregistered built-ins stay counted no-ops in the machine.

## Unverified or approximated
* The meaning of `objects_can_see_*` degrees (half angle used), that objects never block sight, the eye of a non-unit object (0.6 above its origin).
* Absolute vs. fractional vitality arguments (above); `unit_get_health` as a fraction (the call sites compare it with fractions).
* `recording_time` / `unit_get_custom_animation_time` as *remaining* ticks (strong evidence: 16 + 6 call sites sit directly inside `sleep`), and that a
  finished recording releases the unit.
* `object_set_facing` target, `object_destroy_all` extent, `object_set_permutation` region, scale ramp, seat and vehicle behaviour, `damage_*` model.
* `MISSION_OBJ_BIT` encoding is an engine-side convention (the data cannot say).
* Attached objects use the parent marker's rotation as well as its position; whether the original keeps the child's own orientation is unknown.

## Tests (`test_host_obj`, ASan + UBSan, a10 packs, real `ai/units.c` when present else the fixture's weak unit layer; both variants pass)
Each built-in group is called with real arguments and its effect checked (lists, names vs slots, create / anew / destroy / containing, teleport and facing,
volumes with the awaited ring, attachments against an independent marker-pose computation, sight with a clear / blocked / looking-away spot and a unit looking
at the player, vitals / damage immunity / kill, custom animations on the unit's own graph, a foreign graph, a scenery object, recordings with the control
checked against an independent `cine_play`, delete / hover / kill / destroy, devices incl. transitions, groups and flags, effects (particles counted) and
damage, player latches and flags, vehicle stubs, suspended units), a **fuzz** (5 seeds x 16,000 random calls of every built-in with dead handles, NaN,
infinities, out-of-range names and strings: found and fixed one float-to-int overflow), and two soaks of the real a10 script machine for 9000 ticks:
* **quiet** (player idle, H1 registered, the other domains unregistered): 137,811 nodes, 316 H1 calls over 18 built-ins (the mission waits on AI and sound
  that are not registered here: `players` 105, `volume_test_objects` 100, `object_teleport` 16, `recording_play` 14 ...), script errors 0, blocked calls 0,
  thread overflow 0, peak 19 threads; **awaited volumes at the end: 1** (the volume the first script waits for, tested 100 times); 25 distinct
  unregistered built-ins hit, none of this module's domains.
* **stress** (every dormant script woken every 450 ticks; other domains answer neutral values): 650,394 H1 calls over 50 distinct built-ins (`players`
  260,819, `volume_test_objects` 218,740, `device_get_position` 18,123, `objects_can_see_object` 32,787, `unit` 27,039, `recording_play` 324, `effect_new` 701 ...),
  errors 0, blocked 0, thread overflow 0, peak 105 threads, the player's body alive.
Nothing in H1 blocks: every built-in returns 0 at once (the original blocks in none of them either: scripts wait with `sleep` / `sleep_until`).
Cost: at `-O2` on the host a whole mission tick (scripts, devices, units, game: not only H1) averages 3.3 us in the quiet soak and 5.5 us in the stress soak
(72 H1 calls per tick on average); the EE is 30-100x slower, so even the stress case is 0.2 to 0.6 ms per tick for everything, most of it the game step.
The expensive calls are `objects_can_see_object` (one `coll_ray` per object of the list) and the marker poses of attachments (`objects_pose` per attachment
per tick); `players`, `list_get` and `volume_test_objects` are polled thousands of times by the scripts but cost a few hundred instructions each.

## ADR proposal
**ADR-0xx: Runtime object handles are tagged; the script host is split by domain and keeps its state in `mission_t`.**
*Decision*: object, unit and device values produced by built-ins at run time are `MISSION_OBJ_BIT | slot`; a handle without the bit is an object-name
index (the compiled constants), resolved through DEVS. The host built-ins live in one file per domain (`hs_host_obj.c`, `hs_host_misc.c`, `hs_host_ai.c`), reach every
subsystem through `mission_t`, and keep their persistent state (attachments, seats, damage flags, input locks, latched actions) in the plain-data
`mission_hostobj` inside it; per-tick work is two hooks (`hs_host_obj_pre` / `_post`) in the mission tick. *Why*: the data cannot distinguish a constant
object name from a computed object, and a flat, pointer-free state makes checkpoints and saves a memcpy. *Consequences*: every module that creates single
object handles must use `mission_obj_handle`; the mission tick has two more calls; `mission_t` grows by 9.3 KB.
