# `CINE` section, version 1 (recorded animations, camera tracks, titles, cinematic runtime)

Output of `tools/halo2ps2/cinconv.py` (workstream C of [Phase 4](23-phase4-plan.md)), read by `engine/src/world/cinedef.c`
(contract `engine/src/world/cinedef.h`) and driven by the pure runtime `engine/src/sim/cine.c` (`cine.h`). One section in
`assets/hp2/<map>.cin.hp2` (an [`.hp2` pack](07-hp2-format.md) holding only `CINE`). Little-endian, tables 16-byte aligned, offsets
relative to the section start, tag ids are Xbox tag ids (u32) as everywhere else. No game text, names or numbers are reproduced here:
the document describes formats and quotes aggregate statistics only.

```
python3 -I tools/halo2ps2/cinconv.py assets/decompressed/a10.map -o assets/hp2/a10.cin.hp2
python3 -I tools/halo2ps2/cinconv.py --info assets/decompressed/a10.map          # summary, problems
python3 -I tools/halo2ps2/cinconv.py --golden assets/hp2/a10.cin.hp2             # per recording state hashes (test reference)
```

## 1. What goes in

| Part | Source in the map | Section content |
|---|---|---|
| Recorded animations | `scenario +0x36C` (64 B records + event streams) | name, tick length, version words, initial control state, the event stream **verbatim** (deduplicated) |
| Camera tracks | every `camera_track` (`trak`) tag | tag id, control points (position + orientation, conjugated to the engine's convention) |
| Cutscene titles | `scenario +0x4FC` (96 B) | name, box, style, colours (GS words), fade in / up / fade out in **ticks**, string index |
| Help strings | the scenario's `ingame help text` ustr (`+0x584`) via `hudconv.Collector.ustr` | the UTF-16 strings the titles and `display_scenario_help` use |
| Animation references | `scenario +0x444` (ai animation references) and the `antr` entries of `+0x4B4` (script references) | (name, graph tag id, resolved) records and a sorted unique list of graph tag ids |

Not stored, on purpose: the level's `hud_message_text` (`+0x594`; hudconv already puts it into the HUD pack's `STRS` as
`level_messages`, where `hud_set_help_text` / `hud_set_objective_text` look their message up **by name**, see section 8), cutscene
flags and cutscene camera points (workstream D's `DEVS`; the camera state machine takes a pose, see section 6), script-referenced
sounds (only exported as roots, `referenced_sounds`).

**Recorded animations are not position data.** The stream is a recording of a unit's *control input* (aim yaw and pitch, throttle,
control flags, weapon index). `recording_play` (and `_and_delete`, `_and_hover`, `recording_kill`, `recording_time`) feed it to a unit
and the unit's own physics, animation graph and (for vehicles) driving model produce the motion; nothing in the data is a position
or an orientation of the object. The engine must therefore have a unit controller that accepts the control state of `cine_play`
(throttle x forward / y left, aim, flags) exactly as it accepts player or AI input. This was derived from the data (no field is
anything like a position; the state is what a controller consumes) and from the script vocabulary; it is not documented elsewhere.

## 2. Verified layouts of the Xbox data

All of this was checked on the ten campaign maps (`a10 a30 a50 b30 b40 c10 c20 c40 d20 d40`); the full byte layouts are in the
docstring of `cinconv.py`. Status: **V** verified against the data (the cross-check is named), **U** unverified (a convention the data
does not contradict), **D** derived (reverse engineered and consistent with every check, no external confirmation).

| Fact | Status |
|---|---|
| Strides: recorded animation 64, ai animation reference 60, script reference 40, title 96, track point 60; each equals (next block - this block) / count where the next block follows directly; the recorded animation records end exactly where the first event stream starts and the 349 streams are packed 4-aligned | V |
| Recording header: name char[32], version s8 (4 on all 349), raw flag s8 (0 on all), unit control data version s16 (4 on 271, 3 on 71, 0 on 7), length s16 (35 .. 3524 ticks), data {size, 0, 0, pointer, 0} | V |
| Event stream header size by control data version: 64 B (v0), 70 B (v3), 72 B (v4): the three aim vectors (f32[3] each) of the header equal the unit vectors of the quantised yaw / pitch pairs that end the header to 0.5 count on all 349 | V |
| Event coding: `op = type << 2 \| kind`, kind 2 / 3 = u8 / u16 wait follows, kind 1 = wait 1, kind 0 = no wait, then the payload of the type (table in section 4). **All 349 streams tokenise exactly to their last byte** | V |
| Tick rule: every event applies its payload and then waits; length = 1 + sum of waits **equals the tag's length for all 349** | V |
| Aim: counts of pi / 1000 rad, 2000 per turn; the yaw by plain integer addition never leaves [-1000, 1000] and the pitch (86,504 small and 593 large changes over 252,773 ticks) stays in [-475, 490] inside the physical +-500 on every recording | V |
| Header words: `mode` byte 0 goes 0 -> 3 through the one type 2 event (73 recordings = exactly the 73 headers that hold 0), control flags at +6, weapon index at +4, throttle floats at +8 | D |
| Names of the control flag bits (HEK `unit_control_flags` order), of `mode` / `aux` / `word2` / `extra_a` / `extra_b`, the meaning of weapon index 0 / 1 | U |
| Which of facing / aiming / looking the aim events drive (aiming == looking on 349 of 349, facing has the aiming yaw on 346 and pitch on 274): the section keeps ONE yaw and ONE pitch and drops facing | U |
| Title block: text box (425 / 597 on all 44, left 0 / 50 / 275), string index valid on all 44, times are whole numbers (0, 30, 60 ... 210) so they are **ticks**, style 1 on 37 and 0 on 7 (HEK order plain, bold, ...), justification 0 on all | V (box, index, ticks) U (style and justification names) |
| Title strings come from the scenario's ingame help text ustr (`display_scenario_help` indexes the same list) | V (the index range check); U (that `display_scenario_help` uses it: from the script vocabulary only) |
| Script references: 40 B, tag reference at +0x18, every one of 910 resolves to a live tag of its group (snd! 570, lsnd 186, antr 55, effe 33, scen 29, jpt! 8, vehi 8, bipd 8, actv 9, weap 2, mach 2) | V |
| ai animation references: tag reference to an `antr` at +0x20 on all 23; the name is an animation of the graph on 14 of 23 | V (graph) / the 9 others are labels |
| Camera track: flags 0, control points at +0x04 (60 B: position, orientation x y z w, 32 B zero), 11 distinct tracks: ten with 9 points at 22.5 degree steps from -90 to +90 about the y axis, one with 6 points; unit quaternions (norm 1.0000); Halo stores the conjugate of the Hamilton quaternion (as in `ANIM`), the converter conjugates: verified by geometry (the first point is above the unit looking straight down, the last below it looking up) | V |
| Interpolation along a track, easing of `camera_set`, letterbox size and speed, fade semantics | U (section 6) |

## 3. Section layout

```
0x00 "CINE" | u16 version = 1 | u16 0 | u32 total size
0x0C u32 rec_count, rec_off        records 80 B                 0x14 u32 pool_off, 0x18 pool_bytes   stream pool
0x1C u32 track_count, track_off    tracks 32 B                  0x24 u32 point_count, point_off      points 32 B
0x2C u32 title_count, title_off    titles 64 B                  0x34 u32 string_count, string_off    strings 8 B
0x3C u32 char_units, char_off      UTF-16LE, NUL after each     0x44 u32 help_tag  (ustr tag id, 0 = none)
0x48 u32 anim_ref_count, anim_ref_off   records 48 B             0x50 u32 graph_count, graph_off      u32 tag ids, ascending
(header padded to 128 B; every table starts 16-aligned; the pool's first 16 bytes are reserved so offset 0 means "none")

recording (80 B)  char name[32] | u32 stream_off (pool relative, 4-aligned) | u32 stream_bytes | u32 length (ticks) |
                  u8 version, raw, ctl_version, 0 | u16 flags | s16 weapon, extra_a, extra_b | u8 mode, aux | u16 word2 |
                  f32 throttle x, y | s16 yaw, pitch (counts) | u32 event_count | u32 0
track (32 B)      u32 tag_id | u32 flags | u32 first_point | u32 point_count | char name[16]     sorted by tag id
point (32 B)      f32 x y z | f32 qx qy qz qw (engine convention) | u32 0
title (64 B)      char name[32] | s16 top, left, bottom, right | u16 string (index or 0xFFFF) | u16 style | u16 justify | u16 0 |
                  u32 colour | u32 shadow (GS RGBA words, R in the low byte, 0x80 = 1.0) | u16 fade_in, up, fade_out | u16 0
string (8 B)      u32 first unit | u32 units         (the string list is the ingame help list, strings are indexed as in the tag)
anim ref (48 B)   char name[32] | u32 graph tag | u32 resolved (1 = name is an animation of the graph) | 8 B zero
```

The stream pool holds each distinct stream once: a10 has 99 recordings but 70 distinct streams (30 recordings repeat an earlier one
under the same name; `cinedef_find_rec` finds the first).

## 4. The event stream

State before the first event: the record's initial control state. Playback keeps `next_time` (0 at the start); an event whose
`next_time <= tick` is applied, then `next_time += wait`. `wait` is the one the op announces, so several events can share a tick
(kind 0) and the last event of every stream is a pure wait (type 1) that brings `next_time` to the final tick. The recording lasts
`length = 1 + sum(waits)` ticks (ticks 0 .. length - 1). Once past the end the state holds ("hover").

| type (op >> 2) | ops | payload | effect | seen |
|---|---|---|---|---|
| 1 | 04..07 | - | pure wait | 349 (310 with a u8 wait) |
| 2 | 08..0B | u8 | `mode` = value (always 3) | 73 |
| 4 | 10..13 | u16 | control flags = value | 1,089 |
| 5 | 14..17 | u16 | weapon index = value (0 or 1) | 4 |
| 6 | 18..1B | f32 x, f32 y | throttle (x forward, y left) | 1,877 |
| 14 | 38..3B | s8 d_yaw, s8 d_pitch | aim += (counts) | 86,504 |
| 22 | 58..5B | s16 d_yaw, s16 d_pitch | aim += (counts): the large form, 409 of 593 are the yaw crossing +-1000 | 593 |

Yaw wraps into [-1000, 1000) after every aim event, pitch clamps to +-500 (never engages on real data, checked by the tests).
Throttle values seen: (0, 0), (1, 0), (-1, 0), (0, 1), (0, -1), (0.707, -0.707). Control flag values seen: 0, 1, 2, 0x100, 0x140,
0x200, 0x300, 0x400, 0x600, 0x800, 0x801, 0x900, 0x2000 (type 4 events; headers also hold 0x12, 0x8E4, 0x8EE): see `cinedef.h` for the
assumed bit names.

Header (before the events), by control data version: `u8 mode, u8 aux, u16 word2, s16 weapon, u16 flags, f32 throttle[2]`, three
unit vectors (f32 x 3 each: facing, aiming, looking), for version >= 3 a u32 of uninitialised padding (stack addresses, tag-id-like
words) and an s16 (`extra_a`), for version 4 one more s16 (`extra_b`, always -1), then six s16 (facing, aiming, looking yaw / pitch).
Everything but the throttle, flags, weapon, mode and the aiming yaw / pitch is dropped or kept raw (see the converter docstring).

## 5. Reader (`cinedef.h`)

`cinedef_open` validates once: header and table ranges and alignment, names and strings terminated, string ranges inside the
character table, titles' string index in range, tracks sorted ascending with >= 2 finite points inside the point table, graph list
ascending, record initial yaw in [-1000, 1000] / pitch in +-500, finite throttle, control data version 0 / 3 / 4, and **every event
stream decoded to its end** (known types only, no payload cut short, finite throttles, event count and `1 + sum(waits)` equal to the
record's `events` and `length`). After a successful open nothing the accessors do can read outside the buffer; a failed open leaves
the struct zeroed and every accessor neutral. Records are read with byte loads. The API: `cinedef_get_rec / find_rec / rec_stream`,
the event decoder `cinedef_next_event` + `cinedef_apply_event`, `cinedef_get_track / find_track / get_point`, `cinedef_get_title /
find_title / get_string` (copies UTF-16 into a caller buffer), `cinedef_get_anim_ref / get_graph / has_graph`.

## 6. Runtime (`cine.h`)

Pure C11, no heap, no `double`, host-tested; `cine_state` is 6.5 KB. All times are ticks of 1/30 s, advanced by `cine_tick` (last in
the tick order of [23-phase4-plan.md](23-phase4-plan.md)). Conventions are `render/camera.h`'s: +z up, yaw counter-clockwise from +x,
positive pitch looks up, +y left, a roll is positive when the camera banks right, `fov` is the horizontal field of view in radians
(a cutscene camera point holds 1.2217 = 70 degrees by default; whether the Xbox value is horizontal or vertical is **U**).

| Part | API | Script built-ins it serves (usage over the ten maps) |
|---|---|---|
| Playback | `cine_play_start / tick / seek`, `cinedef_ctl` with a `changed` mask (aim, flags, throttle, weapon, mode), `done`, `tick` = `recording_time`; a pool `cine_play_begin / get / find / kill` keyed by an opaque owner id with `CINE_PLAY_DELETE / HOVER` flags | `recording_play` 143, `_and_delete` 65, `_and_hover` 51, `recording_kill` 56, `recording_time` 100 |
| Camera track | `cine_track_sample` (uniform parameter), `cine_track_sample_pitch` (the segment whose control point pitches bracket the aim pitch; no assumption of even spacing), `cine_track_world` (unit origin + yaw -> world pose), quaternion <-> yaw / pitch / roll | the unit camera of `camera_track` tags (units' `camera_track_tag` in `UNIT`) |
| Cinematic camera | `cine_cam_control`, `cine_cam_set` (pose, ticks, ease), `cine_cam_set_relative` + `cine_cam_set_anchor`, `cine_cam_set_first_person / dead`, `cine_cam_set_fov`, `cine_cam_set_near_clip`, `cine_cam_time`, `cine_cam_pose`, `cine_pose_to_camera` | `camera_set` 682, `camera_control` 67, `camera_time` 41, `camera_set_relative` 17, `camera_set_first_person` 5, `camera_set_dead` 1, `cinematic_set_near_clip_distance` 16 |
| Screen | `cine_start / stop`, `cine_letterbox` + `cine_letterbox_bar`, `cine_fade_out / in`, `cine_skip_begin / end / request / pending` | `cinematic_start` 41, `_stop` 30, `cinematic_show_letterbox` 45, `fade_out` 119, `fade_in` 99, `cinematic_skip_start_internal / stop_internal` 10 each |
| Titles | `cine_title_show` (from a section title), `cine_title_post`, `cine_title_alpha` (0..1 ramp: up over fade_in, hold `up`, down over fade_out; 0 during the delay), 4 slots | `cinematic_set_title` 43 |
| Messages | `cine_msg_post / post_ascii / clear / set_visible / slot`, kinds help, objective, scenario help, subtitle, 8 slots, 128 UTF-16 units each | `hud_set_help_text` 70, `show_hud_help_text` 125, `hud_set_objective_text` 44, `display_scenario_help` 12 |

Camera transitions: `cine_cam_set` starts from the current camera (the pose it already holds, else the player pose the host
refreshes with `cine_cam_set_player_pose` every tick) and arrives exactly on the target after `ticks` ticks, also when interrupted
by another call; yaw and roll interpolate along the shortest arc, position, pitch and fov linearly; `CINE_EASE_SMOOTH` is `3u^2 -
2u^3`. A relative camera composes its pose with an anchor frame `{position, yaw}` the host updates each tick (the object's own pitch and
roll are ignored: **U**). Fades start from the current alpha: `fade_out` runs it to 1 over the given ticks in the given colour,
`fade_in` to 0; a zero time is instant. `cine_letterbox` slides the bars in `CINE_LETTERBOX_TICKS` steps (exact integer position, so
0 and 1 are exact) to `CINE_LETTERBOX_FRACTION` of the screen height per bar. **Constants no tag carries** (verified: none of the tags
read has a fade, letterbox, transition or default message duration field; the fade colour and time are arguments of the script call):
`CINE_LETTERBOX_FRACTION` 0.125, `CINE_LETTERBOX_TICKS` 15, the default easing (the built-in should pick `CINE_EASE_SMOOTH` or
`LINEAR`, one line), `CINE_SCENARIO_HELP_TICKS` 300 are **choices, not data**.

Track sampling: uniform Catmull-Rom through the positions (end points repeated) and normalised lerp of the quaternions. The control
points lie on arcs of 22.5 degrees, where any smooth interpolation agrees within a few percent of the radius, but the original
curve is **U**. `cine_track_sample_pitch` is what a chase camera uses: pitch lookup follows the aim exactly even on the one track whose points are
not evenly spaced (angles 90, 50, 0, -46.8, -80.2, -80.2).

## 7. Numbers

`python3 -I tools/halo2ps2/cinconv.py <map>` (Python 3.14) takes 0.07 s per map including the interpreter start.

| map | recordings | ticks | events | section | tracks | titles | help strings | graphs | script sounds | hud messages (HUD pack) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 99 | 44,426 | 9,614 | 37.5 KiB | 7 | 3 | 7 | 15 | 200 | 31 |
| a30 | 63 | 41,107 | 16,366 | 66.9 KiB | 7 | 2 | 5 | 4 | 54 | 11 |
| a50 | 34 | 25,421 | 10,055 | 41.7 KiB | 6 | 6 | 4 | 9 | 51 | 4 |
| b30 | 34 | 29,172 | 11,891 | 46.6 KiB | 7 | 3 | 3 | 3 | 25 | 10 |
| b40 | 24 | 20,167 | 6,366 | 30.5 KiB | 10 | 3 | 7 | 3 | 40 | 3 |
| c10 | 51 | 67,586 | 26,263 | 98.4 KiB | 5 | 10 | 11 | 8 | 85 | 3 |
| c20 | 1 | 596 | 339 | 2.9 KiB | 2 | 6 | 6 | 3 | 41 | 3 |
| c40 | 12 | 5,151 | 1,297 | 10.0 KiB | 8 | 4 | 6 | 3 | 86 | 10 |
| d20 | 20 | 13,600 | 5,482 | 23.4 KiB | 6 | 3 | 3 | 2 | 56 | 7 |
| d40 | 11 | 5,547 | 2,816 | 13.5 KiB | 8 | 4 | 4 | 9 | 118 | 6 |

349 recordings, 252,773 ticks (2.3 hours of control input), 90,489 events in total; the stream coding costs 1.3 bytes per tick. The
largest section is c10's at 98 KiB (the packs are LZ4 compressed, 76 KiB), the ten maps together are 371 KiB. Host timings, -O2 on an Apple core: `cinedef_open`
(which decodes every stream) 0.016 ms for a10 and 0.05 ms for c10, playback 3.4 ns per tick of a recording (a rough EE estimate: 100 times
slower, 0.4 microseconds), a track sample 16 ns (pitch lookup 51 ns), an idle `cine_tick` 13 ns. Nothing here is a budget concern.

## 8. Integration notes (for the integrator)

* **Packs / roots**: `scripts/build-world.sh` runs `cinconv.py <map> -o assets/hp2/<map>.cin.hp2`. `cinconv.referenced_anims(map)`
  returns the `antr` tag ids the scenario names for scripted objects (the script references and the ai animation references: 15 / 4 / 9 /
  3 / 3 / 8 / 3 / 3 / 2 / 9 on the maps above, most of them already roots through objects), `referenced_sounds(map)` the `snd!` / `lsnd`
  ids of the script references (a superset: dialogue and music go to `streamconv.py`, the rest to `soundconv.py --roots`). Add both to
  `objpack.py` / the sound roots. `referenced_models` does not exist for this workstream (the recordings name no model).
* **Hardware-free state**: `cine_play` is fed to a unit controller per tick: throttle (x forward, y left, magnitude <= 1), aim yaw /
  pitch (`cine_ctl_yaw / pitch`, radians), flags (`CINEDEF_CF_*`), weapon index; the `changed` mask says what moved this tick. A
  recording that was made for a vehicle drives the vehicle's seat unit; the vehicle model does the rest. Because the motion is the
  physics' answer to recorded input, a controller that differs from the original will drift: the integrator can correct with the
  scripts' own `object_teleport` calls, as the mission does after its cutscenes.
* **Script built-ins** (registered by the host, `script/hs.h`): map them as in the table of section 6; `camera_set` takes a
  cutscene camera point (workstream D's `DEVS`: position, yaw / pitch / roll in radians, fov in radians) and a tick count and calls
  `cine_cam_set`; `recording_play*` resolves the recorded animation **by name** with `cinedef_find_rec` and the unit by object handle;
  `recording_time` is `cine_play.tick`; `cinematic_set_title` calls `cine_title_show(find_title(name))`; `hud_set_help_text name` looks
  the message up with `huddef_find_message` in the level's `hmt` set and posts its text with `cine_msg_post` (the HUD draws
  `cine_msg_slot(i)`, `cine_title_alpha`, `cine_letterbox_bar` and `fade_alpha`); `display_scenario_help n` posts
  `cinedef_get_string(n)` with `CINE_SCENARIO_HELP_TICKS`.
* **Renderer**: `cine_cam_pose` + `cine_pose_to_camera` fill `camera_t` (roll is not expressible there: the pose keeps it); during
  `cinematic` the HUD and player input are off (`cine_screen.cinematic`), the fade overlay is one quad, the letterbox two.
* **cfg**: no keys. Everything is driven by the script machine; a standalone test can call `cine_play_start` directly.

## 9. Verification

* `python3 -I tools/tests/test_cinconv.py`: the event coder / walker / reference player with a hand-computed world (tick rule, aim
  wrap and clamp, every wait kind, all three header versions), a synthetic scenario (tracks, titles, strings, references, roots), the
  strict section parser, corrupt tag data and corrupt sections, and on every campaign map found: conversion without problems,
  length == 1 + sum of waits, aim integer ranges, unit quaternions, string indices, roots inside the map's `antr` tags.
* `make -C engine/tests cine` (part of `test`): `test_cinedef` (fixture fields, malformed events, every truncation and a flip of
  every byte of the fixture and of every real section under ASan / UBSan, every real recording played and hashed against the Python
  reference player) and `test_cine` (playback timing against the tag lengths on all 349 recordings, determinism, seek equals
  sequential play, track continuity and pitch lookup, quaternion round trips, camera transitions and conventions, screen state,
  titles, messages, recording pool).
* The two C players and the Python player share one *interpretation* of the stream; what is verified independently of it is the
  tokenisation, the tick rule and the aim ranges (section 2).

## 10. Unverified or approximated

The meaning of mode / aux / word2 / extra words and of the control flag names; whether the first event applies on tick 0 or 1
(+-1 tick); the dynamics of the facing vector (dropped); the original interpolation along camera tracks, the easing of `camera_set`
and the exact fade semantics (fades here start from the current alpha); the letterbox size / speed and the default message
duration; relative cameras ignoring the object's pitch and roll; horizontal vs vertical field of view; the HEK names of the title
style and justification enums and of `display_scenario_help` using the ingame help list.
