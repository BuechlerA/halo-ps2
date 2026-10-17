# Phase 3 plan: workstreams, packs and contracts

Phase 3 ("player, physics, weapons, HUD", [roadmap](04-roadmap.md)) is built as a set of **independent data converters**
(each with a host-tested pure-C reader) plus the engine systems that consume them. This file is the contract between
them. Gate M3: walk a10's first BSP, shoot placed targets, every converted weapon functional, 30 fps on the PCSX2
interpreter (ADR-022).

**Status (2026-10-17): all eight workstreams delivered, integrated and gated.** Pack sizes on a10: collision 9.2 MiB raw over 9 BSPs (1.05 MiB for BSP 0), CMOD 40 KiB, UNIT 19 KiB, WEAP 48 KiB, ASTB 79 KiB,
HUD0+FONT+STRS 36 KiB + HUDT 0.46 MiB, SND0 68 KiB + SNDD 1.4 MiB, FXD0 280 KiB + FXTX 0.62 MiB; the object pack grew from 5.0 to 6.6 MiB of models (weapons and effects roots). The objpack roots,
`scripts/build-world.sh` and the gate scenario `scripts/paths/a10game.cfg` are wired; the roadmap has the gate result.

## Conventions (all workstreams)
- **Repository rules** are in `CLAUDE.md`: C11, no `double`, no heap after level load, readers are pure C (no ps2sdk) so
  they build and run on the host under ASan/UBSan; game data never enters git or docs (describe formats, not contents).
- **Python**: stdlib only, runs as `python3 -I tools/halo2ps2/<tool>.py <map> -o <pack>`; shared parsing lives in
  `tools/halo2ps2/{mapinfo,tagindex,hp2}.py` (read them and an existing converter such as `scenconv.py` / `modelconv.py` first).
  Every module documents the byte layouts it **verified against real map data** in its docstring, and says which fields are
  unverified. Field layouts may be cross-checked against public tag documentation, but nothing is trusted until a
  real map confirms it (sizes, ranges, relations between fields).
- **Maps**: decompressed maps are `assets/decompressed/<map>.map` (a10 a30 b30 b40 c10 c20 c40 d20 d40 ... see
  `reports/map-headers.txt`). Converters must run on **all ten campaign maps** (not only a10); tests that need data skip
  with a message when `assets/` is missing (CI has no game data).
- **Packs**: each converter writes its own `.hp2` pack, `assets/hp2/<map>.<kind>.hp2`, sections as below. Section
  payloads are little-endian, 16-byte-aligned tables, offsets relative to the section start, a magic + version at the
  top. Cross references between sections/packs are **Xbox tag ids** (u32), as in the Phase 2 packs (`SCEN` keys `MODL`,
  `ANIM`).
- **Readers**: `engine/src/<dir>/<name>.[ch]`, pure C11, `*_open(const uint8_t *data, size_t size, T *out)` returning a
  status enum, lookups by tag id, every offset and count validated against the section size (corrupt input must be
  rejected, never crash: the host test flips bytes/truncates and walks everything). No heap. Header comment documents
  structs and the units (world units, radians, ticks of 1/30 s, 128 = 1.0 lightmap scale where relevant).
- **Tests**: `tools/tests/test_<tool>.py` (synthetic fixtures + skip-if-missing real-map checks), `engine/tests/test_<name>.c`
  (+ a Makefile rule that builds the pack with the Python tool first, see `test_model` / `test_scen` rules; **edit
  `engine/tests/Makefile` with small additions only**, other agents add lines too).
- **Do not touch** files outside your workstream (other agents work in parallel in the same tree); do not run `make -C
  engine` (the main ELF build belongs to the integrator) and do not commit. You may run host tests.
- **Docs**: one format document `docs/<NN>-<name>-section.md` per workstream (layout tables, what is verified, what is
  not, numbers on real maps: sizes, counts, conversion time), and a short **ADR proposal** (title, decision, why,
  consequences) in your final report so the integrator can add it to `docs/06-decisions.md`.
- **Report** (final message): files created, section layouts in one paragraph, the reader API, measured sizes on a10 and the
  largest map, what is unverified or approximated, and anything the integrator must wire (cfg keys, referenced model or
  animation tag ids exported through `referenced_models()` / `referenced_anims()`).

## Workstreams

| # | Name | Converter | Pack / sections | Reader (engine/src/…) | Doc |
|---|---|---|---|---|---|
| A | World collision | `collconv.py` | `<map>.coll.hp2`: `COL0`..`COL9`,`COLA`.. (one per BSP, same index order as `BSPn`) | `world/coll.[ch]` | 15 |
| B | Object collision models | `cmodconv.py` | `<map>.cmod.hp2`: `CMOD` | `world/cmod.[ch]` | 16 |
| C | Units: globals + bipeds (+ vehicles' basic data) | `unitconv.py` | `<map>.unit.hp2`: `UNIT` | `sim/unitdef.[ch]` | 17 |
| D | Weapons, projectiles, damage, grenades, equipment | `weapconv.py` | `<map>.weap.hp2`: `WEAP` | `sim/weapdef.[ch]` | 18 |
| E | Animation state tables | `ansmconv.py` | `<map>.ansm.hp2`: `ASTB` | `world/ansm.[ch]` | 19 |
| F | HUD, fonts, strings | `hudconv.py` | `<map>.hud.hp2`: `HUD0`, `FONT`, `STRS`, `HUDT` (TEXS blob) | `ui/huddef.[ch]` | 20 |
| G | Sound | `soundconv.py` | `<map>.snd.hp2`: `SND0`, `SNDD` | `audio/snddef.[ch]` | 21 |
| H | Effects: effect, particle, decal | `fxconv.py` | `<map>.fx.hp2`: `FXD0`, `FXTX` (TEXS blob) | `world/fxdef.[ch]` | 22 |

Integrator (not delegated): player controller and camera, DualShock 2 mapping, weapon logic, projectiles, damage, shields,
HUD and effects rendering, audio client, checkpoint snapshot, frame loop (30 Hz simulation tick), gate scenario,
`scripts/build-world.sh` wiring, `objpack.py` roots for the models/animations that weapons and units reference.

### Roots exported to the object pack
Weapons, units, projectiles and effects reference models and animation graphs that no scenario placement mentions. Each of
workstreams C, D, H exports `referenced_models(map_path) -> set[int]` (tag ids of `model` tags, as `modelconv` keys them)
and C, D, E export `referenced_anims(map_path) -> set[int]` so the integrator can add them to `objpack.py`'s roots.
Shaders/bitmaps of those models are then converted by the existing `objtex.py` (OTEX) path.

## Contracts

### A. World collision (`COLn`, `world/coll.h`)
Source: `structure_bsp` collision BSP (bsp3d nodes/planes/leaves, bsp2d references/nodes, surfaces, edges, vertices).
Output must let the engine answer, **without heap and in bounded time**:
```c
typedef struct { float t;            /* 0..1 fraction along the move */
                 float normal[3];    /* surface normal at the hit, facing the mover */
                 float point[3];     /* contact point */
                 uint16_t material;  /* collision material index (into globals' material table) */
                 uint16_t flags;     /* COLL_F_* : two-sided, invisible, climbable, breakable, ... as the data defines */
                 int32_t surface; } coll_hit;
coll_status coll_open(const uint8_t *data, size_t size, coll_bsp *out);   /* validates everything */
int coll_ray(const coll_bsp *b, const float a[3], const float b_[3], uint32_t ignore_flags, coll_hit *h);        /* segment a->b, 1 = hit */
int coll_sphere_sweep(const coll_bsp *b, const float a[3], const float b_[3], float radius, coll_hit *h);       /* sphere centre a->b */
int coll_point_in_solid(const coll_bsp *b, const float p[3]);                                                     /* solid leaf? */
int coll_sphere_overlap(const coll_bsp *b, const float c[3], float r, float push_out[3]);                         /* depenetration */
```
Semantics follow Halo's own collision BSP (front/back leaves, the 2D BSP per plane testing the contact point inside the
surface polygon, two-sided / invisible / climbable flags). A brute-force reference (every surface polygon, in Python or C)
must agree with `coll_ray` on thousands of random segments of a real BSP (state the agreement rate and explain every
disagreement), and `coll_sphere_sweep` must never end a sweep inside solid space on a10 BSP 0. Size budget: the section of
a BSP is part of the 7 MB "current BSP" budget (docs/02-specs.md), target ≤ 1.5 MiB per BSP raw.

### B. Object collision models (`CMOD`, `world/cmod.h`)
Source: `collision_model` tags (and the per-object collision bounds the object tags carry). Goal is **hit detection and
damage regions**, not physics: per collision model, its regions (name, damage multiplier / "head" flag, permutation
names), and for hit tests a compact conservative shape per (region, node): node-local OBB or capsule (agent chooses, must be
conservative on real models: report the fraction of real model vertices inside their node's shape), plus per-model
damage constants (maximum health/shield, shield recharge fields if they live in the collision model). API:
`cmod_find(sec, tag_id)`, `cmod_ray(model, node_matrices, segment, out_region, out_t)` testing a segment against the posed
model, returning the nearest hit region. Keyed by the collision model tag id **and** by the object tag id that uses it
(`unitdef` links them).

### C. Units (`UNIT`, `sim/unitdef.h`)
Source: `globals` (player constants: look acceleration/pitch limits, aim assist, camera defaults, difficulty block, material
table with per-material impact responses, grenade table, vehicle/biped tuning) and every `biped` tag of the map (movement
speeds, acceleration, jump velocity, gravity scale, collision radius/height/pill, camera height standing/crouching, health,
shield, shield recharge delay/rate, head/body regions, crouch, weapon marker names, sound refs as tag ids, animation graph
tag id, collision model tag id). `vehicle` tags only as far as needed to place and drive a Warthog later: define what you
extract and why in the doc, but bipeds and `globals` come first and must be complete. Reader API: `unitdef_open`,
`unitdef_globals()`, `unitdef_biped(tag_id)`, material lookup `unitdef_material(index)`.

### D. Weapons (`WEAP`, `sim/weapdef.h`)
Source: every `weapon`, `projectile`, `damage_effect`, `equipment`, `item_collection` (and `grenade` data in `globals`
when it lives there) of the map: triggers (rounds per second, spread/error, recoil, ammo per shot, projectile tag, firing
effect, magazine size, reload time, charging, overheat, "melee"), magazines (capacity, rounds loaded/total, reload time),
the **first-person and third-person model and animation tag ids**, weapon-HUD interface tag id, sound/effect refs,
zoom levels, ready/put-away times, projectile definitions (speed, range, gravity/drag, guidance, damage effect, impact/detonation
effects, material responses, arming), damage effects (damage amounts and falloff radii, camera shake, rumble, side effects,
flags), grenade and equipment types. A pure-C **`weapdef_*` simulation helper** is part of the reader (so it is host-tested):
`weapon_fire_step(state, def, input) -> events` advancing a weapon's state machine in 1/30 s ticks (ready, firing, reload,
overheat/charge, ammo and magazine accounting) with no engine dependencies; host tests drive **every converted weapon**
from all ten maps through ready, a burst, a full reload and empty-magazine behaviour and assert sane numbers (fire rate vs.
tag, ammo conservation).

### E. Animation state tables (`ASTB`, `world/ansm.h`)
Phase 2's `ANIM` keeps keyed animation tracks and an idle pointer. First person, bipeds and weapons need **named animation
lookup**: the `model_animations` graph's unit blocks map (weapon class x weapon type x action) to animation indices
(`ready`, `fire`, `reload`, `melee`, `throw`, `idle`, `walk/run` in four directions, `crouch`, `jump`, `fall`, `land`,
`aim` overlays, ...), together with each animation's frame count / ticks per frame, loop flags, next-animation chains,
frame events (sound/effect/marker triggers) and the aiming-screen / overlay pitch ranges. Also the **first-person weapon
graphs** (the weapon's own `model_animations` for the arms+weapon model: ready, fire1, fire2, reload1, reload2, melee,
overheated, ...). Output must be consumable by `world/anim.c` (animation indices are indices into that graph's `ANIM`
entry, same tag-id keying); coordinate with `animconv.py` by importing it, **do not change its section layout**; if it needs
extra fields (names, frame events), add them to your own section keyed by the same graph tag id.

### F. HUD (`HUD0`/`FONT`/`STRS`/`HUDT`, `ui/huddef.h`)
Source: `hud_globals`, `unit_hud_interface`, `weapon_hud_interface` (crosshairs, ammo counters, static elements, overlays,
meters), `grenade_hud_interface`, `hud_messages`/`unicode_string_list` entries the gameplay HUD uses, `font` tags (glyph
atlas bitmap + character table), and the **HUD bitmaps** converted to the same `TEXS` layout `tex.c` reads (extend the
bitmap conversion path of `texconv.py` through its public functions; PSMT8/PSMT4 with CLUT, no mips needed). Anchors and
offsets must be pre-resolved to a 640x480 virtual screen so the engine scales to 512x448 itself. The reader exposes
elements with: bitmap/sequence index to UVs, position, scale, colour (including state-dependent colour: low health/ammo),
blend (add/multiply/alpha), the meter (shield/health bar) definitions and the crosshair sets per weapon-HUD tag.

### G. Sound (`SND0`/`SNDD`, `audio/snddef.h`)
Source: `sound` tags (Xbox ADPCM, 16-bit PCM, ogg-less) referenced by gameplay data: weapons, projectiles, damage effects,
effects, bipeds, `globals` materials (footsteps/impacts), and `sound_looping` (record the loop/detail structure; engine
plays the looping part only). **Dialogue and music are out of scope** (Phase 4 streaming). Convert to **PS2 SPU2 ADPCM
(VAG blocks)**: decode Xbox ADPCM to PCM, resample to <= 22050 Hz mono (stereo permutations folded), re-encode with a
real encoder (4-bit ADPCM, 28 samples/16 bytes, loop flags set for looping). Per sound: permutations, base rate,
gain/pitch ranges, min/max distance, "random pitch bounds", class (so the engine can pick voice priority), and a **budget
mode**: the total SPU2-resident set must be <= 1.4 MiB; the converter takes `--roots <tag ids>` (default: everything
reachable from weapon/projectile/damage/effect/biped/globals tags) and drops the least important permutations to fit,
reporting what it dropped. Verify the encoder by decoding your own VAG in a reference decoder and reporting SNR against the
source PCM (state the numbers). The engine side (loading into SPU2 through `audsrv.irx`, ADR proposed by the integrator)
is not yours.

### H. Effects (`FXD0`/`FXTX`, `world/fxdef.h`)
Source: `effect` (events, parts: damage effect / particle / decal / light / sound / projectile / object / ...), `particle`
(sprite parameters: bitmap, size/rotation/colour/alpha over lifetime, velocity, gravity, lifetime ranges, blend mode), `decal`
(bitmap sequence, size, lifetime, fade, blend), `particle_system` only if a weapon or impact effect uses it, and the `light`
tags effects reference. Convert everything reachable from the weapon/projectile/damage-effect/biped/globals material tables
(roots exported by D and C: coordinate by reading the same sources; recompute them yourself, do not import other agents'
unfinished code) into compact tables an EE-side billboard/sprite renderer can consume (**no VU programs in this workstream**),
plus the particle/decal bitmaps as a `TEXS` blob (`FXTX`). Reader API: `fxdef_find_effect(tag_id)`, event and part iteration,
particle/decal definition lookup.

## Engine integration (integrator) overview
`sim/world_sim` runs a fixed 30 Hz tick: pad -> player intent -> biped controller (sphere sweeps against `COLn`) -> weapons
(`weapdef` state machine) -> projectiles (ray/segment tests vs. `COLn` and `CMOD`) -> damage -> objects/animation ->
effects. Rendering interpolates nothing at 30 fps. A **scripted-input test mode** (`input` keys in `halo.cfg`) lets the
PCSX2 gate exercise walking, shooting and reloading headless; results are asserted in log lines (`[game] ...`).
