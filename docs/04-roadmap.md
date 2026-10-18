# Roadmap

Phases are sequential; each ends with a **milestone gate** that must pass before the next phase
starts. Until Phase 6 a gate means "passes on the PCSX2 interpreter" (ADR-015/022): wherever a gate below says
"on hardware" read it that way; real-console verification of every gate is batched into Phase 6. Effort figures are
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
      [reports/tag-index.md](reports/tag-index.md) (`tagindex.py report`: layout, tag regions, BSPs) and
      [reports/tag-inventory.md](reports/tag-inventory.md) (`report.py`: bitmaps, sounds, script built-ins by name from the
      executable's function table, `hsnames.py`); reports only, no data
- [x] Choose pipeline parser: hand-written structs, ADR-011 (`tagindex.py` for the converters, `tagcache.py` for the inventory report)
- [x] CI: `.github/workflows/ci.yml` builds the ELF (ps2dev image), runs the tool
      tests, the host C tests and the asset-free check on every push
      (green on GitHub; its first run caught two real failures: LeakSanitizer on Linux
      and `make` missing from the ps2dev image)
- [x] Frame pacing without hardware: `scripts/profile-pcsx2.sh --interpreter`
      (ADR-015). 600 frames, 0 dropped, EE 0.05 ms avg/0.06 ms max per frame.
      Real-console TTY numbers are deferred to the Phase 6 console pass (ADR-022).

**Gate M0**: ELF runs at 60 Hz on hardware with a VU1-drawn triangle; every
campaign map decompresses and inventories cleanly.

## Phase 1 — Static world at 30 fps (≈ 4 pm; built and first measured at 60)
Prove the rendering budget on the hardest static content before writing gameplay.
**Status: complete. The target was lowered from 60 to 30 fps after the phase was finished (ADR-023); the 60 fps numbers below
remain valid as headroom and the 30 fps numbers are in the Gate M1 line. Gate M1 is met on the PCSX2 interpreter (ADR-022); the partial console results and the
open findings are recorded below and in [10-m1-hardware-check.md](10-m1-hardware-check.md) and move to Phase 6.**

- [x] `.hp2` pack format v1 ([07-hp2-format.md](07-hp2-format.md)): container,
      Python writer/reader (`tools/halo2ps2/hp2.py`), LZ4 block codec, EE-side
      reader (`engine/src/stream/hp2.c`) cross-tested on the host; the EE loader reads one
      section's payload straight from the file (`hp2_open_header` + `hp2_load_stored`)
- [x] BSP block location and per-BSP summary (`tools/halo2ps2/bsp.py`): clusters,
      leaves, nodes, triangles, lightmap materials, vertex counts for all 11 maps; the
      structure BSP layouts (clusters 104 B at +0x134, portals 64 B at +0x154, leaves, nodes, planes)
      verified on all 69 BSPs
- [x] Converter, geometry: `tools/halo2ps2/bspconv.py` writes **BSPS v2**
      ([08-bsp-section.md](08-bsp-section.md), ADR-018): per-(cluster set, material) batches of ≤ 24
      triangles as self-contained VIF packets, cluster/batch lists, convex portal polygons, the
      BSP tree for point location. Largest section 3.83 MiB raw (a10), under the 7 MB BSP budget
- [x] Converter, textures ([09-tex-section.md](09-tex-section.md), ADR-019): Xbox bitmaps (swizzled
      DXT/ARGB/565/…) → half-resolution PSMT8/PSMT4 with CLUTs and mips; shader → texture binding table;
      lightmap pages → luminance PSMT8 atlases (`LMn`) and per-vertex chroma baked into the BSP vertex
      colours. All ten campaign maps convert in 1–2.5 s; BSP textures ≤ 1.1 MiB per map
- [x] Engine: free-fly camera (DualShock 2) and scripted camera paths; cluster/portal visibility with
      portal scissors; `bsp_lit` + `bsp_lightmap` VU1 programs; VIF1 chain builder; texture residency
      (page-granular LRU, CLUT pool, 1.5 MB/frame upload budget, GIF-channel uploads); distance fog;
      sky gradient; EE clipping fallback for guard-band/near-plane triangles (ADR-017)
- [x] Profiler overlay on screen (EE/VIF1/GS ms, triangles, batches, clusters, texture traffic,
      dropped frames) and a once-a-second TTY line; framebuffer dump over TTY for headless checks
      (`fbdump`, `scripts/fb2png.py`); `halo.cfg` boot options (ADR-021)
- [x] Targets in order: `a10` interiors → `a30` (largest outdoor) → `b40` (longest sightlines) →
      `c10` (foliage alpha): all 28 BSPs of these four maps load and render on PCSX2; scripted
      fly-throughs of a30, b40 and a10 run 36 s with **0 dropped frames** (recompiler and
      interpreter, `scripts/profile-m1.sh`)
- [x] **Hardware, partial and deferred (ADR-022)**: on the console (2026-10-16) the texture self test matched, a30 ran 60 fps with
      14–17/2154 frames dropped (0.65–0.79 %, EE 4.6/5.0 ms), b40 BSP 11 dropped 40/2154 (1.86 %, EE 8.4/8.9 ms, GS ~7.9 ms).
      The b40 miss, lightmap look, mips and the clip budget are Phase 6 items

Known limitations carried out of Phase 1 (none blocks the gate, each is a measured follow-up):
- Mip chains are converted but the engine draws level 0 only (bilinear). Needs per-draw LOD selection;
  decide after the hardware numbers show whether distant terrain stalls the GS texture cache.
- The EE clipper costs ~2.5 ms in the worst interiors and is time-limited; a VU0/VU1 clipper is the fix
  if hardware needs the time. Real batches average ~22 triangles, so ≤ 400 batches/frame means ≲ 9.6 k
  visible triangles (budget text in [02-specs.md](02-specs.md) §5.2 needs revisiting at M1).
- Sky is a gradient and fog is global (one distance range per run): sky models and per-cluster fog are
  Phase 2 (they need the `model` converter and the fog tag reader). Additive/multiply shaders draw as
  alpha blends; `senv` foliage whose alpha the converter drops draws opaque.
- Confirmed on the console (2026-10-16): CSM1 CLUT swap, PSMT4 8x2 CLUT, nibble order. Still console-only questions: lightmap gain /
  overall look, pad behaviour (Phase 6).

**Gate M1**: noclip fly-through of **a30 and b40** at 30 fps (earlier measured at 60) with < 1 % dropped frames; memory report under
budget. **Met on the PCSX2 interpreter** (0 dropped on a30, b40 and a10 in recompiler and interpreter modes at 30 fps and at 60 fps, final tree 2026-10-16; EE build ≤ 9.2 ms; level arena ≤ 4.2 MB); console verification in Phase 6. Re-measured 2026-10-18 on the recompiler with the configs' fog and sky keys applied for the first time (ADR-059): still 0 dropped on all three, EE prep peak ≤ 9.7 ms, level arena ≤ 4.4 MB; the pre-fix ELF of the same tree gives the same numbers, so the rise since 2026-10-16 comes from later engine changes, not from fog.

## Phase 2 — Objects, animation, first person (≈ 4 pm) — done (PCSX2 gate)
- [x] Converter: `model` → VU1 batches (≤24 tris, ≤24 palette nodes, 2 influences, LOD chain) (`modelconv.py`, docs/11); `OTEX` textures
  (`objtex.py`); `model_animations` → quantised keyed tracks (`animconv.py`, docs/12); `scenario` placements, per-cluster probes, player
  first-person data (`scenconv.py`, docs/13); sky model + fog (`skyconv.py`, ADR-029); `objpack.py` combines them (a10: MODL 5.0 MiB, OTEX 1.3, ANIM 1.0, SCEN 0.06)
- [x] Engine: object table and placement per BSP (`sim/objects.c`), node matrices / bind-pose skin palette (`sim/skeleton.c`, ADR-025),
  `model.vsm` rigid+skin program, per-object lighting from BSP probes (ADR-026), LOD by projected size, 30 Hz animation playback,
  first-person arms + weapon (`sim/fpview.c`), Master Chief third-person (`chief`/`chiefcam` debug spawn), camera-locked sky (ADR-029)
- [x] Dynamic LOD controller wired to the profiler (`render/lodctl.c`, ADR-027); 30 fps pacing rework (ADR-028)
- Known limitations (carried to later phases): ANIM state tables / walk and fire animations are not converted (only idle poses);
  variant/permutation selection always uses the default; rotation pitch/roll sign and the LOD cut-off unit are unverified; OTEX textures are capped
  at 128 px on a10/b40; normals of skinned parts are not blended; no per-object dynamic lights or shadows; sky model animation (cloud drift) and the
  scenario's second a30 sky are not handled; `skyfog` (sky tag fog) is off by default because its Xbox-tuned distances over-darken the PS2 view.

**Gate M2**: a10 fully populated (all scenery, placed vehicles, idle bipeds animating) at 30 fps. **Met on the PCSX2 recompiler and
interpreter** (2026-10-16, final tree): a30, b40, a10, a10obj (BSP 4, 125 objects incl. first-person arms, sky on) and a10obj-stress
(every placement spawned) all 0 dropped of 900–1081 frames; EE build avg ≤ 16.6 ms / peak ≤ 20.2 ms of the 33.3 ms frame on the object maps (≤ 9.7 ms on the
world-only maps); level arena 12.5 MB (high water 13.4 MB) of 20 MB; sky costs about 2.4 ms EE and 2.3 ms GS. BSP 4 has only 10 script-created
placements, so the "stress" path is only slightly heavier than the normal one; the worst case measured earlier (about 1000 batches via `objcap`) cost about 5 ms EE.
Console verification in Phase 6 (ADR-022).

## Phase 3 — Player, physics, weapons, HUD (≈ 5 pm) — done (PCSX2 gate)
Built as eight data workstreams plus the engine integration ([14-phase3-plan.md](14-phase3-plan.md), ADR-030..042):
- [x] Collision: `COLn` world collision BSP (ray, sphere sweep, depenetration; agreement with a polygon brute force 99.6 %, no sweep ended in solid space over 22,200 sweeps), `CMOD` object hit
      shapes with damage regions and multipliers ([15](15-coll-section.md), [16](16-cmod-section.md)); biped capsule controller with stairs, slopes, crouch (with headroom check), jump, fall damage (`sim/player.c`)
- [x] Camera, DualShock 2 input (Halo CE layout), look acceleration, aim assist from the weapon's autoaim cone, scripted input for headless runs (`sim/input.c`)
- [x] Weapons: every weapon of all ten maps (20 distinct: pistol, assault rifle, shotgun, sniper, rocket launcher, needler, plasma pistol/rifle, flamethrower, fuel rods, vehicle guns, energy sword melee)
      through the pure weapon state machine; two-weapon inventory, switching and pickup, projectiles with gravity, speed profile and material responses, grenades (frag, plasma), melee, zoom, damage
      effects with regional multipliers, shields (stun delay and recharge), health, death animations ([17](17-unit-section.md), [18](18-weap-section.md), [19](19-ansm-section.md), ADR-040)
- [x] HUD: unit HUD (shield/health meters, panels), weapon HUD (ammo numbers, crosshairs, meters), grenade HUD, messages and pickup prompt with the HUD font, pause banner ([20](20-hud-section.md))
- [x] Effects: muzzle, impact and detonation effects, particles, decals (`FXD0`/`FXTX`, `sim/fx.c`, overlay pass) ([22](22-fx-section.md), ADR-041)
- [x] Audio: gameplay sounds converted to SPU2 ADPCM (1.4 MiB per map, verified decoder and encoder), IOP `audsrv.irx` embedded in the ELF, every sample uploaded at level load, positional volume and pan,
      HUD feedback sounds ([21](21-snd-section.md), ADR-030/038)
- [x] Checkpoint snapshot and revert in RAM (about 300 KB, `sim/save.c`, ADR-042)
- Known limitations (carried on): bodies do not block the player and nothing shoots back (AI is Phase 4); vehicles are not driven; `sound_looping` and `sound_environment` reverb are not played; 85-90 % of the reachable
  sounds do not fit the SPU2 budget (footsteps and scripted impacts dropped); HUD meter fill and flash rules are unverified, the sniper scope is not converted (anchors and the motion tracker blips were fixed against an Xbox frame, ADR-060);
  particle systems, lights and object parts of effects are not created, decals are screen-facing sprites, smoke puffs show their sprite edges; aim-assist angles and area-damage falloff are modelled; item collections untested;
  the player start position is a `halo.cfg` key (scenario spawn points arrive with the scripts in Phase 4).

**Gate M3**: "shooting gallery" — walk a10's first BSP, shoot placed targets, all weapons functional, 30 fps. **Met on the PCSX2 recompiler and interpreter** (2026-10-17, final tree): `a10game` (36 s of scripted walking,
turning, jumping, crouching, firing the rifle and the pistol, reloading, melee, a grenade and the zoom against the cryo bay's crewmen: 0 dropped of 1081 frames in both modes; EE build avg 12–14.5 ms, peak ≤ 18.5 ms of
33.3 ms; level arena 18.5 MB of 24 MB); a30, b40, a10, a10obj and a10obj-stress still 0 dropped. Engine-level check of "all weapons functional": `engine/tests/test_game.c` fires all 20 distinct weapons of the ten maps
at the crewman (projectiles spawn, ammo is spent or the weapon reloads, melee weapon strikes), throws a grenade, restores a checkpoint, lays out the HUD and runs the effects lifecycle against the real packs under
ASan/UBSan. Console verification in Phase 6 (ADR-022).

## Phase 4 — AI, scripting, mission 1 complete (≈ 8 pm)
The core reverse-engineering phase.

- HSC interpreter + the built-ins used by `a10` (converter emits usage list)
- Encounters, squads, actor behaviours (Grunt, Jackal, Elite, Marine, Hunter
  for later), firing positions, pathfinding, vehicle AI stubs
- Devices (doors, lifts, cryo tube), trigger volumes, cutscenes, dialogue
  streaming via `hstream.irx`, music
- BSP transitions, mission objectives, death/respawn, memory card saves

**Gate M4**: **The Pillar of Autumn completable start to finish on Legendary on
at 30 fps.**

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
- 30 fps hardening pass across the worst encounters (Library, Control Room
  bridges, Maw run) with the profiler's per-encounter logs; then evaluate the 60 fps stretch (ADR-023): interpolation
  path, measured headroom per mission, go/no-go recorded as an ADR
- Memory card save UI, difficulty select, settings, attract/intro video (`.pss`)
- Disc build: `scripts/build-iso.sh` (seek-aware layout, ≤ 4.37 GiB), boot on
  fat/slim; PAL 50 Hz mode (sim stays 30 Hz, render 50 Hz with 3:5 interpolation)
- Final audio mix, subtitles, credits
- **Real-console verification pass** (ADR-022): re-run every gate M1–M5 on hardware ([10-m1-hardware-check.md](10-m1-hardware-check.md) is the
  template), fix what PCSX2 hid (GS fill, DMA contention, CLUT/lightmap look, the b40 BSP 11 drops, clip budget), then retire risk #8

**Gate M6**: release ISO burned and played through on a retail console.

## Risk register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **30 fps unreachable in heavy encounters** (Library); 60 fps is the stretch (ADR-023) | Medium | Project-defining | AI LOD (distant actors tick at 10 Hz); strict pools; dynamic LOD controller; measure at M1/M2 before committing further |
| 2 | 32 MB too small for resident data | High | High | Half-res palettised textures, per-BSP streaming, animation quantisation, drop unused permutations per mission (converter prunes by reachability) |
| 3 | Lightmap colour loss (luminance + vertex chroma) looks wrong in Covenant purple interiors | Medium | Medium | Optional 3-pass per-channel path for selected clusters; evaluate at M1 |
| 4 | AI behaviour fidelity (no source) | High | High | Build a behaviour test harness: record Xbox encounters (video) and compare; community documentation of `actor_variant` fields |
| 5 | HSC built-in count (~400) | Medium | Medium | Implement only the subset used by campaign scripts (converter report); stub with logging |
| 6 | Texture upload bandwidth per frame | Medium | High | Material sort + residency; cap 1.5 MB/frame; mip bias under pressure |
| 7 | DVD seek times during BSP switches | Medium | Low | Contiguous per-mission chunks, read-ahead on IOP |
| 8 | PCSX2 masks hardware bugs (DMA timing, cache) | High | Medium | PCSX2 interpreter as the gate until the Phase 6 console pass (ADR-022); GS cost treated as unmeasured; ps2link TTY / on-screen overlay |
| 9 | Save size vs. memory card speed | Low | Medium | Compact serialisation, ≤ 512 KB |
| 10 | Legal: repository must stay asset-free | — | Project-ending | R7, CI check, docs only describe formats |

## What the frame-rate target costs, stated plainly
Halo CE ran at 30 fps on hardware with roughly twice the CPU and RAM of a PS2 and a far more capable GPU. **The
current target is 30 fps (ADR-023)**: it already means half-resolution palettised textures, no bump/specular/detail
maps, re-tuned LOD distances, 16-bit colour, fewer simultaneous particles, and AI/physics code written from the
start for the EE's cache, but every budget has a 33 ms slot instead of 16.7 ms. **60 fps is the stretch**: the
30 Hz tick with render interpolation (ADR-002, [03-engine-architecture.md](03-engine-architecture.md) §3) is the decision
that would make it plausible, and everything else would be budget discipline; Phase 1 already runs there on PCSX2
(`fps 60`), while the console showed the GS side is the unmeasured risk (ADR-022). If M1 or M2 misses budget by more
than 30 % at 30 fps, the fallback order is: 640→512 width (already default), Z24→Z16S, then field rendering; the
target itself only moves again by a new ADR.
