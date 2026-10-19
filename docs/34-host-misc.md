# Script host H2: camera / cinematic, sound, hud / ui, navigation points, game flow, player effect, debug (Phase 4 stage 2)

`engine/src/script/hs_host_misc.c` (+ `hs_host_misc.h`) registers 98 built-ins of the script machine ([doc 24](24-script-section.md)) by name with
`hs_host_register_misc(hs_machine *)`. The host pointer of every built-in is the `mission_t` of [doc 30](30-mission-host.md); the state this part owns lives in
`mission_t.misc` (`mission_misc_t`, plain data without pointers, 1,004 bytes, a zeroed struct is the initial state, defined in `sim/mission_misc.h`) and the
borrowed pointers and hooks in `mission_t.misc_env`. Domain argument types and call counts per map: [reports/script-usage.md](reports/script-usage.md). Test:
`engine/tests/test_host_misc.c` (+ `test_host_misc_fixture.h`), `make -C engine/tests host_misc` (part of `make -C engine/tests`). Legal posture as in
[doc 23](23-phase4-plan.md): built-in names are the engine's vocabulary; no script text, string or level / encounter name appears here.

Rules every built-in follows: arguments arrive evaluated, an unresolved handle (unknown camera point, flag, title, message index, object, sound) or a missing
module (`mission.cine`, `.cin`, `.stream`, HUD strings, `game`) makes the call a no-op (`mission_note_missing` for the unresolved ones); **no built-in returns 1
(blocks)**: the waits scripts use are `sleep_until` over `sound_impulse_time`, `camera_time`, `game_saving`, `hud_get_timer_ticks`, all of which reach their end
by themselves; results are written through the type the call site expects (`ret->type` is pre-set: real, short, long, bool and the enum handle of
`game_difficulty_get` all work).

## Changes to shared files (small, for the integrator)
* `sim/mission.h`: `#include "sim/mission_misc.h"` and two fields in `mission_t`: `mission_misc_t misc;` and `mission_misc_env_t misc_env;` (placed before `user`).
  `mission_navpoint_target` is declared there by the integrator; this part defines it.
* `audio/stream.h`: `typedef struct stream_player {` (the struct got a tag, one word) so that `struct stream_player *` in `mission_t` and `stream_player *` are the same type.
* `engine/tests/Makefile`: rule `host_misc` (+ the `test:` recipe line, `.PHONY`).
* No other file touched. `hs_host_register_misc` and `hs_host_register_objects` both register the `player_effect_*` built-ins (H1 only kicks the camera shake): register H2 **after**
  H1 so the envelope version here wins (the integration run below did exactly that).

## What the integrator wires
* `hs_host_register_misc(&hs)` after `hs_setup`, **after** H1's registration function.
* Once per mission tick, any time before `cine_tick`: `misc_tick(&mission)` (refreshes the player camera pose the `camera_set` transition starts from, the frame of the object a
  `camera_set_relative` camera hangs on, the navigation points' positions). Without it the built-ins still work (they refresh the player pose themselves at the call) but a relative camera
  does not follow a moving object and navpoints are only re-resolved when queried.
* `mission.misc_env`: `.strm` (the STRM index, used for the length of dialogue lines when `mission.stream` is NULL; with a stream player its own `sec` is used), `.snd` (SND0: resident vs streamed
  decision and the length of resident impulses), `.user` + `.play` (resident impulse: call `gameaudio_play(a, tag, pos, &cam, gain)`), `.loop_start` / `.loop_stop` (resident looping
  sounds). Pointers and hooks are not part of a snapshot: set them again after a restore.
* Flags `main` serves between ticks: `save_requested` (`MISC_SAVE_NORMAL / NO_TIMEOUT / UNSAFE`: write the checkpoint, **clear the word**; if nobody does, `game_saving` lapses it after `MISC_SAVE_TICKS` = 60),
  `want_bsp` (`switch_bsp`), `misc.revert_requested` (`game_revert`; set `misc.reverted` after a revert so `game_reverted` answers true once), `won` / `lost`, `misc.skip_ticks` (`game_skip_ticks`, may fast-forward),
  `misc.game_speed` (informational), `misc.hud_hidden` / `hud_blink` (`MISC_HUD_*`: the HUD builder skips the hidden parts), `misc.timer_*` (`misc_timer_ticks()`), `misc.screen_effect`.
* The HUD draws `cine_msg_slot(i)` (help, objective, scenario help; the help messages respect `show_hud_help_text`), `cine_title_alpha`, `cine_letterbox_bar`, the fade overlay (docs/28) and
  the countdown timer. An icon segment of a message is the private-use unit U+E000 + icon number.
* The player's camera adds shake from `misc_player_effect_get(&m, rot, trans, rumble)` (amplitudes already scaled by the envelope; rumble 0..1) when it is non-zero (`player_effect_start` also kicks `game.shake`).
* The mission bot: `mission_navpoint_target(&m, pos)` (below), `misc.help_shown`, `misc.objectives_shown`, `mission.objective_flags` (bit n set when objective message n was shown, n < 32),
  `misc.objective_msg1` / `help_msg1` (last message index + 1), `misc.scenario_help_shown`.
* After `cinematic_start` the mission has `cinematic = 1` (input off, HUD hidden); `cinematic_stop` clears it.
* A checkpoint copies `mission_misc_t` as bytes (no pointers inside). Streams are not part of the save: `stream_kill` the slots after a revert, then `misc.imp[].slot` / `loops[].running` are stale but harmless
  (a new `sound_*_start` replaces them; `sound_impulse_time` answers from the record's end tick, which was saved with the mission tick).

## Navigation points (for the mission bot)
`activate_team_nav_point_flag(navpoint, team, flag, height)` / `_object(navpoint, team, object, height)` fill `misc.nav[16]`; `deactivate_team_nav_point_flag(team, flag)` / `_object(team, object)` clear the entry
with the same kind, team and flag / object (the same activation again refreshes it). Entry fields (`misc_nav`): `kind` (`MISC_NAV_FLAG` / `MISC_NAV_OBJECT`), `team`, `type` (the script's navpoint constant), `ref` (cutscene
flag index / object slot), `height`, `tick` (activation tick), `serial` (activation counter), `pos` (world position, flags: the DEVS cutscene flag + height; objects: `mission_object_pos` + height, **re-resolved on every
`misc_nav_refresh` / `misc_tick` / `mission_navpoint_target`**) and `live` (an object navpoint whose object was destroyed is kept but not live, and skipped). A full table drops the oldest activation.
`int mission_navpoint_target(mission_t *, float out_pos[3])` refreshes the positions and returns the live active entry with the largest `serial` (the most recently activated), or 0 and leaves `out_pos` alone.
`misc_nav_count` / `misc_nav_get(i)` walk the active ones.

## The built-ins (a10 = call sites in the a10 script; "impl" = full semantics as inferred, "state" = the effect is kept as data for the integrator / HUD, "no-op" = typed no-op that does nothing)
Arguments are listed in script order, verified on a10 (and a30 for the object navpoints) from the call-site types.

### camera / cinematic (the domain has 23 distinct built-ins on a10: 18 here, `custom_animation` and `recording_*` are H1's; `scenery_animation_*` is nobody's yet, see below)
| built-in | a10 | status | semantics |
|---|---:|---|---|
| `camera_set(point, ticks)` | 184 | impl | `cine_cam_set` to the DEVS camera point (position, yaw / pitch / roll, fov; fov outside 0.05..3 rad -> 70 degrees) over `ticks` (0 = cut), starting from the camera's current pose or the player's; implies camera control; smooth easing (`MISC_CAMERA_EASE`, U) |
| `camera_control(on)` | 11 | impl | `cine_cam_control`: on keeps the camera where it is, off hands it back to the player camera |
| `camera_time()` | 12 | impl | ticks until the running `camera_set` arrives (0 = there); real / short / bool result types |
| `camera_set_relative(point, ticks, object)` | 10 | impl | as `camera_set` with the point in the object's frame (its position and yaw; pitch / roll ignored, U); `misc_tick` keeps the anchor on the moving object; an object that does not exist yet is a no-op |
| `camera_set_first_person(unit)` / `camera_set_dead(unit)` | 0 / 0 | impl | `cine_cam_set_first_person / dead` (the host renders the unit's view / the death camera; the unit argument is not used: the player's) |
| `cinematic_start` / `cinematic_stop` | 6 / 4 | impl | `cine_start` / `cine_stop` and `mission.cinematic` 1 / 0 |
| `cinematic_show_letterbox(on)` | 6 | impl | `cine_letterbox` (bars slide in 15 ticks, docs/28 constants U) |
| `cinematic_set_title(title)` | 3 | impl | `cine_title_show` of the CINE title index (unknown index: no-op) |
| `fade_out(r, g, b, ticks)` / `fade_in(r, g, b, ticks)` | 22 / 16 | impl | `cine_fade_out / in` from the current alpha in that colour; 0 ticks is instant |
| `cinematic_set_near_clip_distance(d)` | 4 | impl | `cine_cam_set_near_clip` |
| `cinematic_skip_start_internal` / `_stop_internal` | 1 / 1 | impl | `cine_skip_begin / end` (a skippable section; the player's skip input is `cine_skip_request`, the cutscene logic stays in the scripts) |
| `cinematic_screen_effect_start(on)` / `_stop` | 4 / 4 | state | `misc.screen_effect` (the Xbox convolution / filter / video effects have no PS2 counterpart) |
| `cinematic_screen_effect_set_convolution / _filter / _filter_desaturation_tint / _set_video` | 4 / 3 / 3 / 0 | no-op | |
| `cinematic_suppress_bsp_object_creation(on)` | 0 | state | `misc.suppress_bsp_objects` (the BSP switch code may read it) |

### sound
| built-in | a10 | status | semantics |
|---|---:|---|---|
| `sound_impulse_start(sound, object, scale)` | 197 | impl | **Streamed** (the sound is in STRM and not resident in SND0): picks a variant (xorshift, deterministic per mission), `stream_play_sound` in the slot of its kind (dialogue / music / ambience) with gain = scale x class gain x distance attenuation (min / max distance of the tag, source object against the player; the player's own body and "no object" are centred at full gain) and pan from the bearing; a new sound in a busy slot **replaces** the old one (the old record's time becomes 0). **Resident** (SND0): the `play` hook with the object position (NULL when none) and the same gain; a source beyond the max distance is not played. Either way a record `{tag, end_tick}` is kept: the natural length of the sound is `frames * 30 / rate` ticks of the chosen variant (streams) or of the longest permutation (resident; `max_play_ms` as a fallback). Unknown sound / none: no-op. With `mission.stream == NULL` the length still comes from `misc_env.strm`, so scripts wait as long as they would with sound |
| `sound_impulse_time(sound)` | 137 | impl | remaining ticks of the last start of that sound (0 when over, never started or replaced), as short / long / real: what scripts `sleep_until (<= x 0)` or `sleep x` on |
| `sound_impulse_stop(sound)` | 1 | impl | `stream_stop` (0.05 s fade) if it is streaming, record ends now |
| `sound_impulse_predict` | 0 | no-op | |
| `sound_looping_start(looping_sound, object, scale)` | 26 | impl | streamed loop (STRM `strm_find_loop`, track 0, music in the music slot, the rest in the single ambience slot): `stream_play_loop`; a loop whose sound is resident in SND0, or when the stream player refuses: the `loop_start` hook. Starting a loop in a slot replaces the one that was there (its record stops running) |
| `sound_looping_stop(looping_sound)` | 8 | impl | `stream_stop` (the track's fade-out, then its end sound) or the `loop_stop` hook; stopping a loop that is not running does nothing |
| `sound_looping_set_alternate(looping_sound, on)` | 6 | impl | `stream_loop_alternate`; remembered per tag, so one set before the start applies at the start |
| `sound_looping_predict` | 0 | no-op | |
| `sound_class_set_gain(class, gain, ticks)` | 3 | partial | the class name (a string constant) is matched by substring to a group (music / ambient / dialog / other); the group's gain (0..4) multiplies the gain of later starts and ramps the busy stream voice of that group (`stream_set_gain`, `ticks / 30` s). Resident (SPU2) effects are not scaled (no hook) |

### hud / ui
| built-in | a10 | status | semantics |
|---|---:|---|---|
| `hud_set_help_text(message)` / `hud_set_objective_text(message)` | 27 / 3 | impl | the constant is the **index of the message in the level's message set** (HUD pack `STRS`, `hud_globals.level_messages`; verified: all 30 a10 call sites resolve); its text segments are concatenated (icon segments become U+E000 + icon) and posted with `cine_msg_post` as help / objective (no timeout, replaced by the next of the kind). Counters and bits for the bot (above). Out of the set: no-op |
| `show_hud_help_text(on)` | 39 | impl | `cine_msg_set_visible(HELP)` (default visible) |
| `enable_hud_help_flash(on)` | 28 | state | `misc.help_flash` |
| `display_scenario_help(n)` | 4 | impl | string `n` of the scenario's help list (`cinedef_get_string`) as `CINE_MSG_SCENARIO_HELP` for `CINE_SCENARIO_HELP_TICKS` (10 s, U) |
| `show_hud(on)`, `hud_show_crosshair / _health / _shield / _motion_sensor(on)` | 6, 2 each | state | `misc.hud_hidden` bits (`MISC_HUD_*`) |
| `hud_blink_health / _shield / _motion_sensor(on)` | 2 each | state | `misc.hud_blink` bits |
| `show_hud_timer(on)`, `pause_hud_timer(on)`, `hud_set_timer_position(x, y, anchor)`, `hud_set_timer_time(minutes, seconds)`, `hud_set_timer_warning_time(minutes, seconds)`, `hud_get_timer_ticks()` | 0 on a10 (d40 only) | impl | a countdown timer on mission ticks: set starts it (running), pause freezes the remaining ticks, `hud_get_timer_ticks` / `misc_timer_ticks` return the ticks left (0 when over); position / warning / shown are state |
| `numeric_countdown_timer_set(ticks, active)` / `_stop`, `time_code_show(on)`, `time_code_start(on)` | 0 (d40 / c20) | state | `misc.ncd_*`, `time_code_*` |
| `activate_team_nav_point_flag / _object`, `deactivate_team_nav_point_flag / _object` | 1 / 0 / 1 / 0 (a30 / a50 / d40: 12..50) | impl | navigation point table, see above |

### game / flow
| built-in | a10 | status | semantics |
|---|---:|---|---|
| `game_difficulty_get()` / `game_difficulty_get_real()` | 6 / 8 | impl | `mission.difficulty` (0 easy .. 3 legendary) as the enum handle (verified: the a10 difficulty constants are 0..3) or a number, whichever the call site wants |
| `game_save()`, `game_save_no_timeout()`, `game_save_totally_unsafe()`, `game_save_cancel()` | 18, 3, 1, 0 | impl | set / clear the `save_requested` bits (main writes the checkpoint) |
| `game_saving()` | 1 | impl | true while a request is pending, for at most `MISC_SAVE_TICKS` (60) after it (then the request lapses); false afterwards, so `sleep_until (not (game_saving))` ends even if main never serves it |
| `game_safe_to_save()` | 0 | impl | not during a cinematic, after `game_won` / `game_lost` or while the player is dead; the AI's "enemies in sight" is not asked (U) |
| `game_all_quiet()` | 4 | partial | always true (the AI runtime has no query yet; scripts that wait for it proceed) |
| `game_safe_to_speak()` | 0 | impl | no dialogue-slot impulse still running |
| `game_is_cooperative()` | 16 | impl | false |
| `game_won()` / `game_lost()` | 1 / 0 | impl | `mission.won` / `.lost` = 1 |
| `game_revert()` / `game_reverted()` | 2 / 1 | impl | `misc.revert_requested` = 1 (main serves) / `misc.reverted` (set by main after a revert) |
| `switch_bsp(index)`, `scenario_switch_bsp(index)` | 12 | impl | `mission.want_bsp = index` (main switches between ticks); unchanged for the current BSP, a negative index or one beyond the DEVS BSP count; `want_teleport` = 0 (the player keeps its position: the BSPs overlap at the switch volume), `want_pos` is not touched |
| `structure_bsp_index()` | 17 | impl | `mission.cur_bsp` (short or real) |
| `game_time()` | 0 | impl | `mission.tick` |
| `game_speed(x)`, `game_skip_ticks(n)` | 1 / 4 | state | `misc.game_speed`, `misc.skip_ticks` += n (main may act on them) |

### scripted player effect (also registered by H1: see above)
`player_effect_set_max_rotation(yaw, pitch, roll)` (degrees, clamped to 90, kept as radians), `_set_max_translation(x, y, z)`, `_set_max_rumble(low, high)` store the maxima; `player_effect_start(intensity, attack_seconds)`
begins an envelope (linear attack to `intensity`, held), `player_effect_stop(decay_seconds)` decays it linearly from its current level. `misc_player_effect_get` evaluates it at the current tick; `start` also sets
`game.shake` to the larger pitch / yaw maximum x intensity (at most 0.1 rad) so a build that only renders `game.shake` shows something.

### debug / hints: typed no-ops (void results), nothing changes
`objects_predict`, `object_beautify`, `object_pvs_activate`, `object_type_predict`, `rasterizer_model_ambient_reflection_tint`, `garbage_collect_now`, `object_pvs_set_camera`, `breakable_surfaces_enable`, `cls`,
`breakable_surfaces_reset`, `object_pvs_clear`, `object_pvs_set_object`, `render_lights`, `rasterizer_lights_reset_for_new_map` (79 calls on a10, besides `print` / `inspect`, which the machine itself provides).

### not registered here
`custom_animation*`, `recording_*`, `unit_*` (H1); `ai_*`, `ai_conversation*` (A); `scenery_animation_start / _at_frame` (domain camera/cinematic, 0 calls on a10, 11 + 3 on four other maps: it plays a graph animation on a placed
scenery object, the same machinery as H1's `custom_animation`: left to H1); `sound_looping_set_scale`, `sound_impulse_start_effect`, `core_save`, `quit` and the like do not occur in any of the ten maps.

## Inferred semantics (unverified against the original)
Argument orders were taken from the call sites of the ten maps and agree with the engine's documented signatures, but what the engine does with them is inferred from names and from how scripts wait on them:
* `sound_impulse_time` returns the **remaining ticks** (the call sites feed `sleep` and `<=` comparisons; short / long / real results all occur) of the playing sound; it is the natural length of the variant that was
  picked (the stream index's frames / rate, including the encoder's padding of up to 63 frames), not what the audio hardware has actually played (underruns, a stolen slot) — scripts must not depend on the disc.
* Dialogue replaces dialogue (one voice per kind), which the original may not (it mixes several); the timings of scripts that start two lines at once therefore differ.
* The `camera_set` easing (smooth), the fade semantics (from the current alpha), letterbox size / speed and message durations are docs/28's choices; relative cameras use only the object's yaw.
* `skip_fraction` ("probability of not playing") of the sound tags is **ignored** for script impulses (a skipped line would change a script's timing); `gain_mod`, pitch ranges and cones are ignored.
* `game_safe_to_save` / `game_all_quiet` do not look at the AI; `game_saving` is bounded at 2 s.
* The shake / rumble amplitude units of `player_effect_*` (degrees for rotation, world units for translation, 0..1 rumble) are read from the value ranges used by the scripts.
* The `sound_class_set_gain` class names are matched by substring (three groups); a10 uses two distinct classes (music and one other).
* `hud_set_help_text`'s argument is a message index of the level's set (all 30 a10 sites resolve to a non-empty message); that `display_scenario_help` indexes the scenario's help list is from the vocabulary (doc 28).

## Tests and numbers (`make -C engine/tests host_misc`, ASan + UBSan, needs the a10 packs and the CINE pack the Makefile converts; skips without them)
* **Semantics on the real a10 world** (a `mission_t` built from the packs by `test_host_misc_fixture.h`; `engine/tests/mission_fixture.h` (H1) has no HUD / SND / STRM packs, so this part keeps its own
  fixture, which switches to H1's weak unit layer with `-DHM_WITH_OBJ`): camera transitions arrive exactly on the DEVS point after `ticks`, `camera_time` counts down, relative camera composes with the object frame, cut,
  unknown point no-op, camera control, first-person / dead modes; letterbox slide, fades in colour, titles, cinematic flag; dialogue length within the variants' lengths, decreasing and staying 0, `game_safe_to_speak`, slot replacement,
  stop, unknown sounds; resident impulses through the hook (position, gain, distance cut, the script's wait unaffected); loops (streamed slot, resident hook, alternate, stop twice, alternate before start); the **real stream
  player on the real a10 STRM** with a file backed `stream_io` (dialogue and music play, steal counted, 0 read errors); help / objective / scenario help messages (non-empty, counters, bits, visibility, timeout), HUD flags, the
  countdown timer (set, pause, resume, run out); navigation points (flag, object following a moving object, destroyed object fallback, deactivate, unknown, full table); flow (difficulty, save request / lapse / cancel, revert,
  time, BSP switch, won / lost); the player effect envelope; every typed no-op leaves `misc`, the cine state, `want_bsp` and the object count unchanged.
* **Every call site**: all 1,053 call sites of this domain in the a10 script run once with the script's own arguments (`hs_run_expression`): 0 script errors, 27 + 3 HUD messages shown, the only unresolved handles are the 10
  `camera_set_relative` calls (their object does not exist yet when called out of context).
* **Hostile arguments**: 90,000 calls with random types and values (none, `HS_HANDLE_NONE`, NaN, huge, list handles) on a full mission and on an empty `mission_init` one: no crash, none blocks.
* **Soak** (a10 script machine, 9000 ticks = 5 min, player idle, whole `mission_tick` incl. game, host ASan build, this domain alone registered): 0 script errors, 0 blocked calls, 0 thread overflows, 0 tick-budget hits;
  *idle*: 44 of the 98 built-ins called 4,746 times (`camera_set` 67, `sound_impulse_start` 44, `sound_looping_start` 8, `fade_out` 9, `game_saving` 61, `structure_bsp_index` 4,416 polls), the longest dialogue wait 652 ticks (21.7 s),
  2.9 s host time; *every dormant script woken at 0 / 300 / 900 / 1800*: 54 built-ins, 18,767 calls (`sound_impulse_start` 92, `sound_impulse_time` 222, `cinematic_show_letterbox` 24, `camera_set` 73, `hud_set_help_text` 5), same
  maximum, 3.2 s. The calls of an idle player are 0.5 per tick (2.1 with all dormant scripts woken), each O(1) (`sound_impulse_start` scans at most 16 records): the host cost is below the timer's resolution, estimate under 20 us on the EE.
  The scripts block on built-ins that belong to the other hosts (`players`, `volume_test_objects` ...), which is why the idle soak calls only 44 of them: see the next point.
* **Integration soak** (opt-in: `cc ... -DHM_WITH_OBJ -Wno-unused-function test_host_misc.c ../src/script/hs_host_obj.c ... + stubs for the three `units_*` functions H1 needs`; not in the Makefile because it depends on H1 and U):
  with H1's built-ins registered too the same two soaks have **0 errors, 0 blocked calls** and the only unregistered built-ins hit are the `ai_*` ones (`ai_actors` and `ai_command_list_status` polled 1.7k / 44k times); the
  woken run reaches 61 dialogue starts with a longest wait of 292 ticks.
* `unregistered` after this part alone (idle): 41 built-ins (H1's objects / units / devices / players, A's `ai_*`), 65 woken.

## Not done / to wire later
* `game_all_quiet`, `game_safe_to_save` need an AI query; `sound_class_set_gain` for resident effects needs a gameaudio class gain; `skip_fraction` and pitch of impulses; mixing of several simultaneous dialogue lines.
* Draw side of navigation points, timers, help flash and HUD switches (data only here); the camera shake and rumble from `misc_player_effect_get`.
* `scenery_animation_start / _at_frame` (above).

## ADR proposal
**ADR-04x: The H2 script host keeps waits data-driven and deterministic: no built-in blocks, sound lengths come from the stream index, state is one small pointer-free struct.**
*Decision*: the camera / cinematic, sound, hud, navigation, game-flow and debug built-ins (`script/hs_host_misc.c`) never block; the waits scripts perform end by themselves because `sound_impulse_time` is
computed from the **natural length** of the chosen stream variant (STRM frames / rate, or the longest SPU2 permutation) against the mission tick, whether or not a stream player exists, `camera_time` from the
cinematic camera, `game_saving` is bounded at 60 ticks; side effects that belong to the integrator (checkpoint, BSP switch, revert, HUD switches, timers, navigation points) are written to
`mission_t` flags and one 1 KB plain-data struct (`mission_misc_t`) that checkpoints copy as bytes, hooks and borrowed pointers sit outside it. Navigation points are kept as a 16 entry table with
re-resolved positions and `mission_navpoint_target` gives the mission bot the most recent one. *Why*: scripts wait on these built-ins for minutes (dialogue, camera moves, saves); coupling them to
the audio hardware or to main's timing would make mission pace, saves and the bot non-deterministic and a missing module would hang the mission, while the data gives the same timing everywhere. *Consequences*:
dialogue timing is the file's, not the DAC's (an underrun does not delay the script); one dialogue voice means overlapping lines replace each other; the integrator owns the HUD / camera / save side of the
flags and must clear `save_requested`; the effect of `player_effect_*` and the HUD flags needs the render side to read `misc`.
