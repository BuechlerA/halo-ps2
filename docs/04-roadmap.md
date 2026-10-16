# Roadmap

Phases are sequential; each ends with a **milestone gate** that must pass on
PCSX2 *and* on a real console before the next phase starts. Effort figures are
rough person-months (pm) for experienced PS2/engine programmers and exist to set
expectations, not deadlines: this is a multi-year project for a small team.

## Phase 0 — Foundation (≈ 1 pm)
Repository, build, run loop, data inventory. **This phase is what the repo
skeleton already starts.**

- [x] Repo layout, docs, `.gitignore` keeping all game data out of git
- [x] EE build with ps2sdk (`engine/`), VU1 microprogram assembled with `dvp-as`
- [x] `tools/halo2ps2/mapinfo.py`: parse Xbox cache headers, decompress maps
- [x] Boot `halo-ps2.elf` on PCSX2: 512×448 CT16S/Z24, 60 Hz vsync loop
      (300 frames / 5.0 s, 0 dropped), VU1 program runs, profiler text over TTY
- [x] Same on real hardware (ps2link); visually confirm the VU1 triangle
      (2026-10-16: triangle matches PCSX2 on the console; hardware TTY / frame
      pacing numbers not captured yet)
- [x] Header inventory of all maps → [reports/map-headers.txt](reports/map-headers.txt)
- [x] Decompress all 10 campaign maps; produce tag inventory per map (group
      counts, raw data sizes, bitmap formats/resolutions, sound formats, script
      built-in usage, structure BSP counts) →
      [reports/tag-inventory.md](reports/tag-inventory.md) (reports only, no data)
- [x] Choose pipeline parser: hand-written structs, ADR-011
- [x] CI: `.github/workflows/ci.yml` builds the ELF (ps2dev image), runs the tool
      tests, the host C tests and the asset-free check on every push
      (the workflow's steps were run in a clean clone with a stripped environment; it has not run on GitHub yet)
- [x] Frame pacing without hardware: `scripts/profile-pcsx2.sh --interpreter`
      (ADR-015). 600 frames, 0 dropped, EE 0.05 ms avg/0.06 ms max per frame.
      Real-console TTY numbers are deferred to the M1 hardware check.

**Gate M0**: ELF runs at 60 Hz on hardware with a VU1-drawn triangle; every
campaign map decompresses and inventories cleanly.

## Phase 1 — Static world at 60 fps (≈ 4 pm)
Prove the rendering budget on the hardest static content before writing gameplay.

- [x] `.hp2` pack format v1 ([07-hp2-format.md](07-hp2-format.md)): container,
      Python writer/reader (`tools/halo2ps2/hp2.py`), LZ4 block codec, EE-side
      reader (`engine/src/stream/hp2.c`) cross-tested on the host
- [x] BSP block location and per-BSP summary (`tools/halo2ps2/bsp.py`): clusters,
      leaves, nodes, triangles, lightmap materials, vertex counts for all 11 maps
- [x] Converter, first cut: `tools/halo2ps2/bspconv.py` bakes every BSP of all 11
      maps into per-material triangle batches of VIF `UNPACK` packets
      ([08-bsp-section.md](08-bsp-section.md)), read back by `engine/src/world/bsp.c`.
      Largest section 2.73 MiB raw (a10 BSP 6; Xbox block 8.2 MiB), well under the
      7 MB BSP budget; round trip error ≤ radius/65534 per batch (worst absolute
      0.023 units on b30's sky-sized batches), lightmap uv ≤ 2e-3.
      Not yet baked: per-cluster grouping and portals, collision, strips, lightmap
      atlases, texture conversion; no VU1 program or draw path consumes it yet
- Remaining items below
- Converter: `structure_bsp` render geometry → per-cluster baked VIF packets;
  bitmaps → PSMT8/PSMT4 with mips; lightmaps → luminance atlases + vertex chroma
- Engine: free-fly camera, cluster/portal visibility, `bsp_lit` + `bsp_lightmap`
  programs, PATH3 texture streaming with LRU residency, fog, sky
- Profiler overlay on screen (EE/VU1/GS ms, tris, uploads, dropped frames)
- Targets in order: `a10` interiors → `a30` (largest outdoor) → `b40` (longest
  sightlines) → `c10` (foliage alpha)

**Gate M1**: noclip fly-through of **a30 and b40** at 60 fps on hardware with
< 1 % dropped frames; memory report under budget.

## Phase 2 — Objects, animation, first person (≈ 4 pm)
- Converter: `model`/`gbxmodel` → VU1 strips with LOD chain; `model_animations`
  → quantised tracks; `scenario` placements
- Engine: object table, node matrices, `model_rigid`/`model_skin` programs,
  per-object lighting from BSP probes, LOD by screen size, scenery/devices
  placed, animation playback with interpolation at 60 Hz, first-person arms +
  weapon models, Master Chief third-person for cutscenes
- Dynamic LOD controller (draw-distance/particle caps) wired to the profiler

**Gate M2**: a10 fully populated (all scenery, placed vehicles, idle bipeds
animating) at 60 fps on hardware.

## Phase 3 — Player, physics, weapons, HUD (≈ 5 pm)
- Collision BSP + collision models; biped movement, stairs/slopes, crouch, jump
- Camera, DualShock 2 input, look acceleration/aim assist from `globals`
- Weapons: all campaign weapons, projectiles, grenades, melee, damage, shields,
  health, decals, impact effects, particles (`sprite` program)
- HUD (`weapon_hud_interface`, `unit_hud_interface`, `grenade_hud_interface`),
  fonts, strings; pause menu
- SPU2 audio path: `hsound.irx`, weapon/impact/ambient sounds, `sound_looping`
- Checkpoint snapshot/revert in RAM

**Gate M3**: "shooting gallery" — walk a10's first BSP, shoot placed targets,
all weapons functional, 60 fps.

## Phase 4 — AI, scripting, mission 1 complete (≈ 8 pm)
The core reverse-engineering phase.

- HSC interpreter + the built-ins used by `a10` (converter emits usage list)
- Encounters, squads, actor behaviours (Grunt, Jackal, Elite, Marine, Hunter
  for later), firing positions, pathfinding, vehicle AI stubs
- Devices (doors, lifts, cryo tube), trigger volumes, cutscenes, dialogue
  streaming via `hstream.irx`, music
- BSP transitions, mission objectives, death/respawn, memory card saves

**Gate M4**: **The Pillar of Autumn completable start to finish on Legendary on
hardware at 60 fps.**

## Phase 5 — Campaign breadth (≈ 12 pm)
One mission at a time, each adding its systems; order chosen so each adds the
minimum new tech:

| Order | Mission | New systems |
|---|---|---|
| 2 | a30 Halo | Vehicle physics (Warthog, Ghost, Banshee), Pelican drops, outdoor streaming |
| 3 | b30 Silent Cartographer | Hunters, Scorpion-less island, large open-world AI |
| 4 | a50 Truth and Reconciliation | Gravity lift, stealth-ish encounters, night lighting, Covenant ship interiors |
| 5 | b40 Assault on the Control Room | Scorpion, bridges, Banshee combat, snow |
| 6 | c10 343 Guilty Spark | Flood (infection forms, combat forms, carriers), swamp foliage, Sentinels |
| 7 | c20 The Library | **Performance stress: 30+ Flood at once** — pool sizes, AI LOD, budget tuning |
| 8 | c40 Two Betrayals | Three-faction combat, Sentinel beams, Banshee |
| 9 | d20 Keyes | Keyes ship interiors, Flood + Covenant |
| 10 | d40 The Maw | Warthog run timing, scripted collapse, credits |

**Gate M5**: all 10 missions completable on hardware; R2 holds per mission.

## Phase 6 — Hardening and release build (≈ 3 pm)
- 60 fps hardening pass across the worst encounters (Library, Control Room
  bridges, Maw run) with the profiler's per-encounter logs
- Memory card save UI, difficulty select, settings, attract/intro video (`.pss`)
- Disc build: `scripts/build-iso.sh` (seek-aware layout, ≤ 4.37 GiB), boot on
  fat/slim; PAL 50 Hz mode (sim stays 30 Hz, render 50 Hz with 3:5 interpolation)
- Final audio mix, subtitles, credits

**Gate M6**: release ISO burned and played through on a retail console.

## Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **60 fps unreachable in heavy encounters** (Library) | High | Project-defining | 30 Hz tick + interpolation halves sim cost; AI LOD (distant actors tick at 10 Hz); strict pools; dynamic LOD controller; measure at M1/M2 before committing further |
| 2 | 32 MB too small for resident data | High | High | Half-res palettised textures, per-BSP streaming, animation quantisation, drop unused permutations per mission (converter prunes by reachability) |
| 3 | Lightmap colour loss (luminance + vertex chroma) looks wrong in Covenant purple interiors | Medium | Medium | Optional 3-pass per-channel path for selected clusters; evaluate at M1 |
| 4 | AI behaviour fidelity (no source) | High | High | Build a behaviour test harness: record Xbox encounters (video) and compare; community documentation of `actor_variant` fields |
| 5 | HSC built-in count (~400) | Medium | Medium | Implement only the subset used by campaign scripts (converter report); stub with logging |
| 6 | Texture upload bandwidth per frame | Medium | High | Material sort + residency; cap 1.5 MB/frame; mip bias under pressure |
| 7 | DVD seek times during BSP switches | Medium | Low | Contiguous per-mission chunks, read-ahead on IOP |
| 8 | PCSX2 masks hardware bugs (DMA timing, cache) | High | Medium | Hardware gate at every milestone; ps2link TTY logging |
| 9 | Save size vs. memory card speed | Low | Medium | Compact serialisation, ≤ 512 KB |
| 10 | Legal: repository must stay asset-free | — | Project-ending | R7, CI check, docs only describe formats |

## What "60 fps" costs, stated plainly
Halo CE ran at 30 fps on hardware with roughly twice the CPU and RAM of a PS2 and
a far more capable GPU. Holding 60 on PS2 means: half-resolution palettised
textures, no bump/specular/detail maps, re-tuned LOD distances, 16-bit colour,
fewer simultaneous particles, and AI/physics code written from the start for the
EE's cache. The 30 Hz tick with interpolation is the single decision that makes
the target plausible; everything else is budget discipline. If M1 or M2 misses
budget by more than 30 % on hardware, the fallback order is: 640→512 width
(already default), Z24→Z16S, then field rendering — **never** a 30 fps target.
