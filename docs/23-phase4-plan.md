# Phase 4 plan: scripting, AI, devices, cinematics, mission flow

Phase 4 ("AI, scripting, mission 1 complete", [roadmap](04-roadmap.md)) is the core reverse-engineering phase. **Gate M4: the Pillar of Autumn (`a10`)
completable start to finish on Legendary at 30 fps on the PCSX2 interpreter** (ADR-022). Because nobody can play 40 minutes headless, the gate is
checked with a *mission bot* (scripted/automatic player input that follows the mission's own objectives, see "Gate harness"); that limitation
is part of the gate statement.

Like Phase 3 the work splits into independent data workstreams (converter + host-tested pure-C reader/runtime) and an integration part done by
the integrator (the main session). This file is the contract. `docs/14-phase3-plan.md` conventions apply unchanged (C11, no `double`, no heap
after load, pure-C readers with corrupt-input tests, Python stdlib only, tests skip without `assets/`, one format doc per workstream, an ADR
proposal and a report at the end, never commit, never run `make -C engine`, small additions only to `engine/tests/Makefile`).

**Legal posture**: documents describe formats and aggregate statistics. Do not copy script text, level/encounter/character names, strings or
numbers of the content into docs, tests or code comments beyond what is needed to describe a layout (function names of the scripting API are the
engine's vocabulary and may be listed). Anything you print while exploring stays in your scratch directory.

## Stage 1 workstreams (parallel, independent)

| # | Name | Converter | Pack / sections | Reader / runtime (engine/src/…) | Doc |
|---|---|---|---|---|---|
| S | Scripting (HSC) | `scriptconv.py` | `<map>.script.hp2`: `SCRP` | `script/hs.[ch]` (+ `hs_core.c`: flow-control and arithmetic built-ins) | 24 |
| I | AI data | `aiconv.py` | `<map>.ai.hp2`: `AIDT` | `ai/aidef.[ch]` | 25 |
| P | Pathfinding | `pathconv.py` | `<map>.path.hp2`: `PAT0`..`PATZ` (one per BSP, like `COLn`) | `ai/path.[ch]` | 26 |
| D | Devices, triggers, scenario markup | `devconv.py` | `<map>.dev.hp2`: `DEVS` | `world/devdef.[ch]`, `sim/devices.[ch]` | 27 |
| C | Cinematics and camera | `cinconv.py` | `<map>.cin.hp2`: `CINE` | `world/cinedef.[ch]`, `sim/cine.[ch]` | 28 |
| M | Streamed audio (dialogue, music, ambience) | `streamconv.py` | `<map>.strm.hp2`: `STRM` index + raw stream data | `audio/strmdef.[ch]`, `audio/stream.[ch]` (EE player) | 29 |

Stage 2 (integrator, after stage 1): AI runtime (`ai/`: perception, behaviours, movement, combat on top of `aidef` + `path` + the Phase 3
`game`), the game-side built-ins of the script machine (grouped by domain), multi-BSP level manager (BSP switching, per-BSP arenas), objectives,
game flow (`game_save`, `game_won`, death/respawn, memory card), the mission bot and the gate runs.

### S. Scripting (HSC)
Source: the scenario's script nodes (`scenario` tag: script and global blocks, syntax data, string data). Converter output `SCRP`: scripts (name, type
startup / dormant / continuous / static / stub, return type, parameter list, root expression), globals (name, type, initial expression), the
expression node table (compact, self-contained: primitive / call / script call / global ref / parameter ref, value type, next, resolved
built-in **names**, constants and strings), plus a **usage report** for all ten maps (every built-in with call counts per map, grouped by domain:
control flow, math, logic, object, unit, ai, device, camera/cinematic, sound, hud/ui, game/flow, effects, debug/no-op; and the value types they
take). The reader is a **cooperative tree-walking virtual machine with an explicit stack (no recursion)** so a thread can suspend in
`sleep`, `sleep_until`, `wake`, `sleep_forever` and resume next tick:
```c
typedef enum { HS_VOID, HS_BOOL, HS_REAL, HS_SHORT, HS_LONG, HS_STRING, HS_SCRIPT, HS_HANDLE /* tag refs, objects, lists: u32 */ ... } hs_type;
typedef struct { uint8_t type; union { int32_t i; float f; uint32_t h; } v; } hs_value;
typedef struct hs_machine hs_machine;                      /* all state in preallocated pools, no heap */
typedef int (*hs_builtin)(hs_machine *m, void *host, const hs_value *args, unsigned nargs, hs_value *ret);   /* 0 ok, 1 = would block: retry next tick */
hs_status hs_open(hs_section *s, const uint8_t *data, size_t size);
void hs_init(hs_machine *m, const hs_section *s, void *host);   /* starts the startup scripts, activates continuous ones on first tick */
void hs_register(hs_machine *m, const char *name, hs_builtin fn);
void hs_tick(hs_machine *m);                               /* one 30 Hz tick: every runnable thread runs until it sleeps (with a node budget per tick) */
int  hs_wake_script(hs_machine *m, const char *name); int hs_global_get/set(...); hs_value hs_run_expression(...);   /* for tests and the host */
```
Built-ins that need the evaluator (flow control, `set`, arithmetic, comparisons, logic, `sleep*`, `wake`, `begin*`, `if`, `cond`, random) live in
`hs_core.c`; every other built-in is registered by the host. Calling an unregistered built-in must be a counted no-op returning the type's neutral
value (so an unimplemented built-in never stops a mission); the machine counts and names them for the log. Tests: synthetic scripts for every
control-flow construct and for sleeping/waking semantics (sleep n ticks, `sleep_until` polling interval, script recursion depth limits, thread
caps), corrupt sections, and a run of all startup/continuous scripts of every real map for 3000 ticks against a host whose domain built-ins
return neutral values: no crash, no endless loop (node budget), report nodes per tick.

### I. AI data
Source: `actor` and `actor_variant` tags (unit, weapon, grenades, health/shield scale, perception, movement, combat and behaviour parameters:
everything the AI runtime needs, as the tags have it, in documented units), and the scenario's AI blocks: encounters (squads, starting
locations with position / facing / actor type / variant / initial state / command list / flags, firing positions, move positions, per-difficulty
spawn counts, platoon and squad groups, maneuvering parameters, respawn rules), command lists (points and atoms), AI conversations (participants,
lines with sound tag ids, trigger conditions), `ai` related script data (actor type / state enums, team enums). Output `AIDT`. Reader
`ai/aidef.[ch]` with typed accessors by encounter index and by name (scripts address encounters, squads, command lists and conversations by name:
keep the name tables), by actor variant tag id, and by actor type. Export `referenced_models(map)` / `referenced_anims(map)` for objpack roots
(the units of every actor variant, their weapons) and `referenced_sounds(map)`. Tests: layouts verified on all ten maps (ranges, indices, units),
reader fuzzing.

### P. Pathfinding
Source: each `structure_bsp`'s pathfinding data (sectors, links, vertices, hints, object references, user hints: jump, climb, vault). Output one
`PATn` per BSP (same index order as `BSPn`/`COLn`). `ai/path.[ch]` offers, without heap and with bounded time:
`path_locate(bsp, pos) -> sector`, `path_route(bsp, from, to, radius_hint, out_waypoints[], max) -> count` (A* over sector links with the link
costs/widths of the data; portals between sectors give the waypoint line; returns the best partial route when the goal is unreachable and says so),
`path_reachable`, `path_nearest_sector`, hint queries (jump/climb/vault links the route may take), and a per-actor "blocked sector" mask for
doors/vehicles. Validate against a brute-force Dijkstra on the sector graph and against the collision BSP (`coll_ray` along the waypoint legs must
be clear for at least 95 % of legs on real maps; explain the rest), report query times on the host.

### D. Devices, triggers, scenario markup
Source: `device_machine`, `device_control`, `device_light_fixture` tags (power / position transition times, open and close times, flags: door,
elevator, auto-trigger...; control type, sound and effect refs, animation graph references) and scenario data: placed devices with initial position
and power, **device groups** (named, initial value, flags), **trigger volumes** (named oriented boxes), **cutscene flags**, **cutscene camera
points**, **player starting locations**, **object names** (index to name table, so scripts can address placed objects by name), starting profiles
(already in `SCEN`: reuse, do not duplicate), `sound_scenery` / looping-sound placements, decal placements. Output `DEVS`. Runtime
`sim/devices.[ch]` (pure): device table (power, position 0..1, desired position, transition timers, group membership), `device_set_position`,
`device_set_power`, group set/get, per-tick transitions with the tag's times, door and control activation rules as the data describes
(reach and activation conditions are inputs from the host), and events (`opened`, `closed`, `activated`) for the host. Provide
`volume_contains(volume, point)` and "any object of a list in volume" helpers. Tests: transitions against the tag times on real devices, volume
containment (random points vs an independent reference).

### C. Cinematics and camera
Source: `scenario` recorded animations (keyframed recordings used by scripted units: `unit_recorded_animation`, `object_*` playback),
`camera_track` tags, cutscene titles (`unicode_string_list` of the cutscene title style and text), `custom animations` referenced by the scenario
for scripted objects, `hud_message_text` / `ustr` data used by objective and help-text messages (`show_hud_help_text`, objectives), and the
fade / letterbox constants the engine needs. Output `CINE`. Runtime `sim/cine.[ch]` (pure): recorded-animation playback (time -> object
position/orientation/animation state, event stream), camera-track sampler (control points to camera pose with the original interpolation),
the cinematic camera state machine (`camera_set` to a cutscene camera point with easing, `camera_control`, field of view, letterbox, fade in/out
with colour), a title queue (timed text), and a subtitle/objective message queue API the HUD draws. Tests: playback timing against the tag
lengths, interpolation continuity, queue behaviour.

### M. Streamed audio
Dialogue (`sound` tags flagged streamed / class unit dialog), music (`sound_looping` with start / loop / end tracks, class music) and long
ambience cannot live in SPU2 RAM (Phase 3's `SND0` marks them `STREAMED`). Converter `streamconv.py`: decode the Xbox ADPCM (decoder verified in
`tools/halo2ps2/soundconv.py` / `vag.py`, import them), mono-fold dialogue, keep music stereo, resample to <= 22050 Hz (dialogue) and <= 32000 Hz
(music), re-encode as **IMA ADPCM 4-bit** (cheap to decode on the EE: reference decoder in C, bit-exact with the Python one), write one
`<map>.strm.hp2` whose `STRM` section is the index (sound tag id -> stream: format, rate, channels, sample count, byte offset and size in the
data region, loop points, for `sound_looping`: tracks with gains and fades) and whose data sits in further 2048-aligned sections or after the index
(documented), so the EE can read any stream with `file_read_at` ranges. Sizes: report per map (expect tens of MB; DVD budget is 4.7 GB). EE
player `audio/stream.[ch]`: opens the pack, up to 3 concurrent streams (one dialogue, one music, one ambience), reads ahead in 32-64 KB chunks
without stalling the frame (request/poll API called once per 30 Hz tick), decodes into a mixing ring, feeds `audsrv` (`audsrv_play_audio` /
`audsrv_set_format` / `audsrv_wait_audio`: read the audsrv header in `~/ps2dev/ps2sdk/ee/include/audsrv.h`; the Phase 3 client `audio/audio.c`
shows how the IRX is loaded) with volume, fade in/out and looping per stream; a pure-C part (mixer + decoder + ring) host-tested with synthetic
data, the EE glue verified headless on PCSX2 with a log (bytes streamed, underruns, EE ms per tick) via a small test cfg key you document (do not
edit main.c: describe the wiring in the report; you may build a separate test ELF only if you need to, in your scratch dir).

## Roots exported to the object pack
Every workstream whose data references models or animation graphs (I: actor units and weapons; D: device models; C: scripted-object graphs)
exports `referenced_models(map_path)` / `referenced_anims(map_path)` exactly like Phase 3; `objpack.py` unions them. Sound roots: I, D, C export
`referenced_sounds(map_path)` (tag ids) for `soundconv.py`'s `--roots` and for `streamconv.py`.

## Gate harness (integrator)
`scripts/paths/a10mission.cfg` runs the whole of a10 headless: `game <prefix>` with all packs, `mission 1` (script machine on), `bot 1` (the mission
bot). The bot is an engine-side module (`sim/bot.c`) that reads the mission's own state (current objective position from scripts' navpoints and
trigger volumes the scripts wait on, enemies in view) and produces `game_input_t`: walks the sector route to the next target, aims at the nearest
hostile (aim assist does the rest), fires, reloads, throws grenades, picks up weapons, uses controls. It may teleport itself over a stuck spot after
a timeout (counted and reported: the gate states the number of assists). The gate passes when `game_won` (or the last BSP's end condition) fires,
with < 1 % dropped frames at 30 fps in recompiler and interpreter modes, EE budget margin reported.

## Integration notes
- cfg: `mission 0/1` (script machine), `bot 0/1`, `difficulty 3` (Legendary), `pack/tex/obj/game` as in Phase 3; new pack keys follow `<prefix>.<kind>.hp2`.
- Memory: persistent arena (models, animations, scenario, sound/HUD/AI/script data) + a BSP arena released on `switch_bsp` (BSP, lightmaps,
  collision, pathfinding of the current BSP only).
- Everything stays 30 Hz fixed tick; scripts, AI, devices, cinematics tick inside `game_tick` order: script machine -> devices -> AI -> player ->
  weapons/projectiles -> cinematics.
