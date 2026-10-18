# Architecture Decision Records

Short, numbered, append-only. Reverse a decision by adding a new record.

## ADR-001 Clean-room C engine driven by converted data (2026-10-16)
**Decision**: Reimplement Blam gameplay in C11 on the EE; convert the user's
Xbox cache files offline. No decompilation/recompilation of `default.xbe`.
**Why**: x86→MIPS recompilation cannot reach 60 fps with half the CPU and a
different GPU model; the data carries the game. **Consequence**: AI and
scripting semantics must be reverse-engineered behaviourally (risk #4).

## ADR-002 30 Hz simulation tick, 60 Hz presentation with interpolation
**Presentation rate superseded by ADR-023** (30 fps for now; the interpolation design is kept for the 60 fps stretch).
**Decision**: Keep Halo's fixed 30 Hz tick; render every vblank by
interpolating render snapshots. **Why**: preserves gameplay timing exactly and
halves per-frame sim cost; it is the only way 60 fps is plausible. **Consequence**:
one frame of presentation latency; render reads snapshots, never live state.

## ADR-003 512×448, PSMCT16S double buffer, PSMZ24, interlaced output from a full-height buffer (FFMD=0)
**Decision**: as titled, dithering on. **Why**: leaves 2.25 MB VRAM for textures;
16-bit colour is acceptable on 480i; Z24 is needed for Halo's outdoor depth
range. **Fallbacks** (runtime flags): 640×448, Z16S, field rendering.

## ADR-004 Lightmaps as luminance-in-alpha second pass + vertex chroma
**Decision**: BSP pass 1 = base texture modulated by vertex colour (carries
low-frequency lightmap chroma); pass 2 = lightmap luminance texture blended
`Cd × As`. **Why**: GS cannot multiply RGB×RGB in one blend; 3-pass per-channel
is too expensive for all geometry. **Consequence**: coloured bounce light is
approximated; revisit per-cluster 3-pass for Covenant interiors after M1.

## ADR-005 Textures palettised at half resolution
**Decision**: Default PSMT8 at half the Xbox resolution (max 256²), PSMT4 for
UI/particles, PSMCT16 exceptions by allow-list. **Why**: no DXT on GS; 8 bpp at
half res = ¼ of the memory of 8 bpp at full res ≈ DXT1 footprint. **Consequence**:
visible texture blur on close surfaces; hero textures on the allow-list.

## ADR-006 No heap after load; arenas and fixed pools
**Decision**: `core/mem.c` arenas (level, bsp, frame) + fixed pools sized from
scenario counts. **Why**: 32 MB with zero paper headroom and 60 fps determinism.

## ADR-007 All geometry through VU1; EE builds DMA chains only
**Decision**: Static geometry baked offline as VIF packets referenced by DMA
`ref` tags; one microprogram per material class. **Why**: EE has 8 KB D-cache
and no SIMD worth using for vertex work; VU1 + VIF unpack is the hardware path.

## ADR-008 Pipeline in Python; engine and tools licensed separately
**Decision**: `tools/` is Python 3 (stdlib first; Reclaimer/Invader as optional
external GPLv3 helpers), may be GPLv3. `engine/` is original code and keeps its
own licence (to be chosen before first public release). **Why**: the pipeline
benefits from existing Halo 1 Xbox tag definitions; the engine should not be
licence-bound by them.

## ADR-009 PCSX2 for iteration, real hardware gates every milestone
**Superseded in part by ADR-022**: the per-milestone console gate is deferred to Phase 6.
**Decision**: as titled. **Why**: PCSX2 does not model DMA timing, cache or VU
hazards faithfully.

## ADR-010 Scope: campaign only
**Decision**: no multiplayer, co-op or split-screen; `xdemos`, MP maps ignored.
**Why**: split-screen at 60 fps is not credible on this hardware; the goal is the
campaign.

## ADR-011 Xbox tag parsers are hand-written
**Decision**: the tag parsers in `tools/` are written by hand from
`docs/05-halo-ce-data.md`, for the tag types we consume (bitmap, model, BSP,
scenario, sound, ...). Reclaimer stays an optional cross-check oracle in tests
only. **Why**: we own every byte layout and Xbox quirk, avoid a hard dependency,
and need only a handful of tag types. Closes OD-1.

## ADR-012 On-disc codec: custom, LZ4 as fallback
**Decision**: develop a custom codec tuned for the EE (16-byte-aligned tokens,
128-bit `lq`/`sq` copies). It is adopted only if it beats an LZ4 port on real tag
data in decode MB/s or ratio, measured on hardware; otherwise ship LZ4.
**Why**: the EE is in-order with an 8 KB D-cache and no unaligned loads, so
byte-wise LZ4 copies are a poor fit. Closes OD-2.

## ADR-013 Bink intro/credits dropped until campaign runs end to end
**Decision**: no intro or credits video until the campaign plays start to finish.
Revisit then as `.pss` MPEG-2 (budget in `02-specs.md`). In-game cutscenes are
real-time scenario/`camera_track` driven and are unaffected. **Why**: video is
branding and credits only; it costs disc space and IPU work. Closes OD-3.

## ADR-014 Engine licence: GPL-3.0-or-later
**Decision**: `engine/` and `tools/` are both GPL-3.0-or-later (SPDX
`GPL-3.0-or-later`). Game data is never part of the repo or the licence.
**Why**: a single copyleft licence for the whole project, compatible with the
GPLv3 helpers in ADR-008, with an explicit patent grant. Closes OD-4.
Supersedes the "engine keeps its own licence" clause of ADR-008.

## ADR-015 PCSX2 interpreter stands in for hardware between milestone gates
**Extended by ADR-022**: the interpreter is now also the milestone gate until Phase 6.
**Decision**: day-to-day development checks frame pacing and correctness in PCSX2
with all recompilers off (`scripts/profile-pcsx2.sh --interpreter`), run headless
against a scratch copy of the user's config. The real-console check required by
every milestone gate still happens, but only at gates M1 onwards, not per commit.
**Why**: iteration speed. The interpreter is the closest PCSX2 gets to unmodelled
instruction cost and exposes timing bugs the JIT hides, but it does not model
real DMA/cache timing, so it cannot replace the gate. Phase 0's hardware visual
check (triangle) is done; hardware TTY/pacing numbers are not and are folded
into M1. **Consequence**: roadmap items checked off by interpreter runs say so.

## ADR-016 `.hp2` ships LZ4 blocks until the custom codec wins
**Decision**: pack format v1 stores sections raw or as standard LZ4 blocks
(codec ids 0/1) with a per-section CRC-32 of the raw data and 2048-byte section
alignment. The custom EE codec of ADR-012 gets a new codec id if it is adopted.
**Why**: a standard codec lets any LZ4 tool inspect payloads and lets the Python
writer and C reader check each other; nothing downstream depends on the codec.

## ADR-017 Reverse-Z projection; triangles are never clipped on VU1
**Decision**: the world transform is a single matrix with **reverse-Z** (`clip.z = near`,
`clip.w = view depth`, post-divide depth `near / w`, larger = nearer, GS test GEQUAL, Z24 cleared to 0).
VU1 (`bsp.vsm`) tests every vertex *after* the perspective divide (`|x'|, |y'| <= 7`, the guard band of the
GS 12.4 coordinate range, and `0 <= z' <= 1`, i.e. `w >= near` and in front of the eye) and **collapses** triangles that fail to a zero-area triangle at (0,0). The EE
finds the same triangles (exact 8-corner box test per batch, then a per-triangle test with a slightly
tighter margin), clips them in homogeneous space (`render/clip.c`, attributes interpolated linearly in
clip space, i.e. perspective-correct) and draws the result through PATH2, nearest batches first within a
time budget (`clipbudget`, default 6 ms: the EE is idle ~10 ms per frame while Phase 1 has no simulation; at 2.5 ms
near terrain visibly disappeared on a30; revisit when ticks need the EE).
**Why**: CLIPW compares against |w|, so a test written as `x > w` never flags vertices behind the camera; the first
version did exactly that and drew mirrored geometry (found on hardware: "a garbled mess of meshes and planes"; it
looked plausible enough in PCSX2 to be missed until an independent software reference disagreed).
Reverse-Z keeps 24-bit precision at Halo's draw distances. Real clipping inside VU1 is a large
program that only a small fraction of triangles need; in narrow interiors that fraction is high (walls
inches from the camera), so dropping them would punch holes. Collapsing instead of setting ADC: ADC on the
third vertex of a triangle-list primitive did not keep the GS vertex queue in step in PCSX2 (killed
triangles leaked into their neighbours as streaks).
**Consequences**: the EE clipper costs up to ~8 ms in worst-case interiors (measured on PCSX2; not yet on
hardware); beyond the budget the farthest clipped triangles disappear. A VU1 or VU0 clipper is the
follow-up if the M1 hardware numbers need it.

## ADR-018 `BSPS` version 2: cluster batches, portals, BSP tree
**Decision**: visibility comes from the Xbox clusters, portals and collision-BSP tree. A surface's cluster
set is the union of the leaf route and the subcluster route. Batches are built per (cluster set, material),
at most 24 triangles each, with no geometry duplicated; each cluster lists its batches and the engine
de-duplicates with a per-frame stamp. Surfaces in neither route (195 of 1.94 M) are dropped. Version 1 is
rejected. Source layouts and the unverified list are in [08-bsp-section.md](08-bsp-section.md).
**Why**: per-material batches alone cannot be culled by visibility; 24 triangles (72 vertices) is what fits
the 16 KB VU1 data memory with double-buffered input and output.
**Consequence**: real batches average about 22 triangles, so the 400-batch frame budget of
[02-specs.md](02-specs.md) §5.2 only holds up to roughly 9 600 visible triangles; measured scenes use
450–800 batches. The budget is revisited at M1 (strips or larger batches would change it).

## ADR-019 `TEXS` / `LMn`: PS2-ready textures and luminance lightmaps
**Decision**: `TEXS` holds palettised base-map textures (PSMT8, or PSMT4 when 16 colours stay under an RMS
of 3.0) at half resolution (8–256 px), up to four mips, CLUT stored upload-ready (CSM1 order); a shader
binding table maps BSP `shader_id` to texture, blend class and uv scale. `LMn` holds half-resolution
luminance PSMT8 pages with a shared grey-ramp CLUT (alpha = index); the lightmap's colour goes into the BSP
vertex colours (`vertex_chroma`, baked by `bspconv.py`). Details in [09-tex-section.md](09-tex-section.md).
**Why**: base maps only (no detail, bump, specular) put BSP textures under 1.1 MiB per map; luminance +
vertex chroma is ADR-004.
**Consequences**: the mip chain is converted but **not yet used by the engine** (it needs per-draw LOD
selection; textures are drawn at level 0, bilinear). The lightmap gain is a constant (`build-world.sh`
defaults to 1.5; 1.0 already equals a 2x modulate), to be judged on the console in Phase 6; the CSM1 CLUT swap
is confirmed on the console (self test, 2026-10-16). Shaders whose alpha the converter drops (some `senv` foliage) draw opaque.

## ADR-020 Two-pass BSP lighting and the GS conventions the renderer relies on
**Decision**: pass 1 draws base texture x vertex colour (TFX modulate, 0x80 = 1.0); pass 2 redraws the same
batches with the lightmap page as an alpha-only texture and blend `Cd * As` (A = Cd, B = 0, C = As, D = 0,
decal, Z test GEQUAL with Z writes masked). Fog is applied in pass 1 only. Fog uses the GS convention
(`F = 255` is no fog) from `F16 = clamp(w * k + b, 0, 4080)` written to the XYZF2 w lane; the sky is a
horizon-aware vertical gradient written by the clear packet. PSMT4 CLUTs are uploaded as 8x2 PSMCT32 images.
**Why**: it is the cheapest correct implementation of ADR-004 on the GS; a second pass costs one more VU1
transform of the visible batches.
**Consequences**: fog darkens the lightmap pass's input, so thick fog is slightly wrong in lit areas;
per-cluster fog and the sky models are Phase 2 (sky needs the `model` converter).

## ADR-021 Pipelined frame loop, runtime config and headless verification
**Decision**: the EE builds frame N+1 (visibility, material sort, texture residency, one VIF1 source chain
whose REF tags carry the previous batch's MSCAL in their upper 64 bits) while the GS draws frame N;
uploads run on the GIF channel between frames, geometry through VIF1/PATH1+PATH2, ordered by FLUSHA.
Boot options come from an optional `host:halo.cfg` (`core/config.c`): pack paths, scripted camera path,
fog/sky, frame count, `fbdump <frame>`. `fbdump` reads the framebuffer back (GS local->host) and prints it as
hex over TTY; `scripts/fb2png.py` turns that into a PNG. `scripts/run-headless.sh` runs the ELF against a
scratch dir and `scripts/profile-m1.sh` runs the M1 fly-throughs.
**Why**: prep (about 5 ms) plus GS (up to 14 ms) does not fit one vblank unless they overlap; PCSX2 has no
CLI screenshot, and a framebuffer dump is the only way to check rendering without a human at the console.
**Consequences**: texture uploads for frame N+1 happen after frame N finished, so the cache only protects
textures used in the last two frames. The Phase 0 triangle demo (`vu1_tri`) is removed; its job is covered
by `selftest 1` (known PSMT4/PSMT8 patterns) and by the real renderer.

## ADR-022 Real-console checks are deferred to the polish phases; the PCSX2 interpreter is the gate until then
**Decision**: milestone gates M1–M5 are passed on the PCSX2 interpreter (`scripts/profile-pcsx2.sh --interpreter`,
`scripts/profile-m1.sh --interpreter`, headless `halo.cfg` runs). Real-console verification is batched into
Phase 6 (hardening), and may be done earlier at the owner's choice. Everything needed to do it stays in the
repository ([10-m1-hardware-check.md](10-m1-hardware-check.md), `scripts/paths/*.cfg`, the self test, the
overlay's GS/EE/drop log).
**Why**: the M1 console runs (2026-10-16) showed PCSX2 predicts what the project needs day to day: the PSMT4/PSMT8
CLUT path and the self-test patterns matched exactly, and a30's EE time was 4.60 / 5.02 ms on the console against
3.7–4.9 ms modelled. Console sessions cost far more than they return per iteration (ps2link must be restarted by
hand after every run, EE `printf` does not reach its TTY, results are read off the TV).
**Known divergence, so nobody over-trusts the gate**: PCSX2 does not model GS fill or DMA contention. On b40 (BSP 11,
the heaviest views) it reported 0 dropped frames while the console dropped 40 of 2154 (1.86 %, over the 1 % bar) with EE
8.4/8.9 ms and GS ~7.9 ms. Until a console run exists, treat GS cost as **unmeasured**: keep EE budgets with margin
(a PCSX2 pass means EE ≲ 10 ms on the heaviest views, not 16 ms), do not add GS-heavy features (more passes, bigger
textures, dense alpha) without noting the risk, and keep the list below current.
**Open console findings carried to Phase 6**: b40 BSP 11 drops (cause not isolated: GS peaks vs texture uploads; the
overlay now logs the first dropped frames with EE/GS ms and upload KB); lightmap gain look; whether mips are needed;
the clip budget (6 ms) versus frame time on the console; EE `printf` over ps2link.
**Consequences**: roadmap gates read "on the PCSX2 interpreter" until Phase 6; risk #8 stays High and is retired only by the
Phase 6 console pass; R8 in [02-specs.md](02-specs.md) changes accordingly.

## ADR-023 The frame-rate target is 30 fps for now; 60 fps is a deferred stretch goal
**Decision**: the current target is a **locked 30 fps**: one frame per two vblanks (29.97 Hz), simulation tick and
presentation both at 30 Hz, no render interpolation. 60 fps (one frame per vblank, 30 Hz tick with interpolation, ADR-002)
stays the stretch goal and is revisited after Phase 4, or earlier if the budgets show the room; the code keeps it one
setting away (`fps 60` in `halo.cfg`, the interpolation design in [03-engine-architecture.md](03-engine-architecture.md) §3).
Supersedes the 60 fps wording of ADR-002, R2 and the "never a 30 fps target" rule of the roadmap.
**Why**: the Phase 1 renderer already holds 60 fps in PCSX2, but the console runs showed the GS side (unmeasured in PCSX2) is
the first thing to run out (b40 BSP 11: 1.86 % dropped at 60), and everything still to come (objects, skinning, AI, particles,
scripting) adds to both processors. Halo CE itself shipped at 30. Fixing the target at 30 buys every budget a factor two and
removes the interpolation machinery from the critical path while the content systems are written.
**Consequences**: frame budgets are 33.3 ms ([02-specs.md](02-specs.md) §5.1; sim and prep no longer share a 16.7 ms slot);
a "dropped frame" is a flip interval above 1.5 frames (50 ms; the profiler's `drop_ms`); R2 counts those. Geometry and fill
budgets are *not* raised (GS cost is still unmeasured on hardware, ADR-022): the extra time is margin. Measurements taken at
60 fps (Phase 1 console runs) are kept in the docs as they were, labelled as 60 fps results. The risk register's "60 fps
unreachable" entry becomes "30 fps unreachable in heavy encounters", and 60 fps is tracked as a stretch evaluation in Phase 6.

## ADR-024 Object data formats: `MODL`, `OTEX`, `ANIM`, `SCEN`
**Decision**: a map's objects are converted into one pack (`assets/hp2/<map>.obj.hp2`, `tools/halo2ps2/objpack.py`) holding four
sections, each restricted to what the scenario places plus the player's first-person set (`scenconv.collect` feeds the model and
animation converters):
- `MODL` ([11-model-section.md](11-model-section.md)): one VIF packet per batch of at most 24 triangles and 24 skeleton nodes (strips
  expanded to lists, two node influences per vertex, s16 positions relative to a per-batch centre, s8 normals), organised as
  region x permutation x LOD (index 0 = super high). Rotations are stored in the engine's Hamilton convention (Halo stores the
  conjugate). Vertices are in **bind-pose model space**.
- `OTEX`: the TEXS layout (docs/09) keyed by the Xbox shader id of each model part; base maps only, textures capped (128 px on a10/b40)
  to a 3 MiB budget; untextured shaders draw flat.
- `ANIM` ([12-anim-section.md](12-anim-section.md)): per-graph keyed tracks, 15-bit smallest-three rotation keys (4-byte hinge keys),
  s16/f32 translation keys chosen closed loop to within 0.18 degrees / 0.35 mm of the Xbox frames, about 45 % of the Xbox size.
  `ANIM_LOOPS` is derived from the animation names (the tag has no loop flag). Graph state tables and root motion are not used yet.
- `SCEN` ([13-scen-section.md](13-scen-section.md)): definitions, placements (with a BSP mask by point location), the player's
  first-person data, per-cluster object lighting.
**Why**: the engine then needs no tag parser; every reader is pure C, bounds-validating and fuzz-tested on the host.
**Consequences**: a10's object pack is 4.1 MiB stored (MODL 5.0, OTEX 1.2, ANIM 1.0 MiB raw), 7.3 MiB resident of the 9 MiB mission-data
budget: heavy but inside it. If a map exceeds ~5 MiB of MODL the levers are dropping the super-high LOD or unused permutations
(converter keywords exist). The LOD cutoff unit (pixel diameter at 640 wide) and the soso alpha rule are unverified.

## ADR-025 Object rendering: VU1 skinning program, bind-pose palette, one batch at a time
**Decision**: `vu1/model.vsm` (appended to the BSP program in `world.vsm`) transforms each vertex by two node matrices
`clip = c1 + (c0 - c1) * w0`, lights it with ambient + one directional term, and runs the same post-divide clip test, collapse
and fog as `bsp.vsm`. The palette is built on the EE per object: for every node `skin = inverse(default pose world) * posed world *
placement` (static objects skip the skeleton: skin = placement), composed with the camera clip matrix, plus the sun direction in
the node's local frame (five quadwords per node, at most 24 nodes per batch). Per batch the chain carries the batch packet (a REF
into the section, UNPACKed into the idle buffer while the previous batch runs), then FLUSHE, the palette, MSCAL; consecutive
batches with an identical palette skip the palette upload. VIF BASE/OFFSET move to the model layout (136/220) for the pass and
return to the BSP layout (16/220) at the next frame header.
**Why**: the EE never touches a vertex; a per-batch palette keeps VU1 memory inside 1 KiB; the bind-pose matrix is what Halo's
data requires (the first attempt used plain node matrices and produced spiky arms).
**Consequences**: the object pass costs about 5 us of EE time per batch (about 1 ms for the normal a10 population, 5 ms for 1000
batches); normals of skinned parts are not blended (node 0's frame lights the vertex).

## ADR-026 Object lighting from per-cluster probes
**Decision**: Halo lights objects from the BSP lightmap. We bake, per BSP cluster, an ambient colour (mean lightmap colour of its
surfaces with a floor of 20/128) and one directional light (the sky's brightest light when the cluster sees the sky, else the
lightmap gradient, else up) into `SCEN`; the engine looks the cluster up with `bsp_find_cluster` at the object's centre.
**Why**: it reuses the lightmap's own scale (128 = 1.0) so objects and walls match and costs one table read per object.
**Consequences**: no per-object dynamic lights or shadows yet; the ambient/sun split is an approximation, tunable at conversion time.

## ADR-027 Dynamic LOD controller and object draw limits
**Decision**: `render/lodctl.c` raises a quality level (0..4: draw distance x1.0..0.4, model LOD bias 0..2, particle cap x1..0.25) as
soon as a frame is dropped or its cost exceeds the budget, holds the level for 2 s, and recovers one step at a time after sustained slack
(90 frames under 75 % of the budget). The object pass also culls by distance (default 200 units), by frustum, and below 3 px, and caps
batches per frame (default 900). The controller ignores the first frames (loading).
**Why**: the Phase 2 gate needs a controlled way to give up detail instead of frames; the same hook later throttles particles.
**Consequences**: the cost signal is the larger of EE build time and GS time as observed by the profiler (GS time is unmeasured in PCSX2, ADR-022).

## ADR-028 30 fps pacing: kick right after the flip, burn vsyncs before presenting
**Decision**: the first 30 fps loop waited the extra vblank after the flip and before the kick, which made any frame whose EE build exceeded
one vblank (16.7 ms) slip a whole extra vblank although 33 ms were available. The loop now kicks immediately after the flip, builds the next
frame, waits for the GS, burns the vsyncs that have not happened yet (`passed = floor(elapsed / 16.68 ms)`, burn `vblanks - 1 - passed`),
then presents; a frame is dropped when its flip interval exceeds 2.5 frame times of vblanks (41.7 ms at 30 fps).
`video_flip` is split into `video_wait_gpu` + `video_present` because `graph_wait_vsync` clears the GS FINISH flag.
**Consequences**: the Phase 1 30 fps numbers (0 dropped) were measured with the earlier loop and stay valid (their EE times were under one vblank).

## ADR-029 Sky: an EE-drawn model behind the world, `SKY0` section
**Decision**: the scenario's first sky tag is converted by `skyconv.py` into a `SKY0` section (sky model tag id, fog colour and
distances, ambient colours); the sky model itself goes through `modelconv` like any object. The engine draws it first, with the camera
position added to its vertices (so it never moves relative to the viewer), Z test "always" and no Z write, then draws the world over it.
The pass runs on the EE (`obj_render_sky`: decode the batch packets, project, clip with `clip_polygon`, pack into DIRECT chunks), not on VU1,
because the model is a few hundred triangles and the VU1 object program's depth/guard-band conventions do not fit a camera-locked dome.
The config key `sky3d 0` turns it off; `skyfog 1` takes the world's linear fog (colour, start, opaque distance / max density) from the sky tag
instead of the `fog` key (default off: the sky's own fog values are tuned for Xbox draw distances and darken the PS2 view too much).
**Why**: a30's ring and cloud sky was the most visible gap reported after the first console run; the model is data we already convert.
**Consequences**: the sky costs about one extra textured-triangle pass of EE time (see docs/04-roadmap.md Phase 2 for numbers); sky model animation
(a30 clouds drift) and the second a30 sky (switched by script) are not handled; a10's sky (space) draws the same way.

## ADR-030 Audio through ps2sdk's stock `audsrv.irx`
**Decision**: the Phase 3 audio path uses the IOP server `audsrv.irx` shipped with ps2sdk (embedded in the ELF with `bin2c`, loaded with
`SifExecModuleBuffer` after `sbv_patch_enable_lmb()`, on top of the BIOS's `rom0:LIBSD`) and its ADPCM API instead of the planned custom
`hsound.irx`. Sounds are converted to the 16-byte `APCM` header + SPU2 ADPCM blocks audsrv loads (`soundconv.py`), uploaded to SPU2 RAM at level
load and played on pooled voices with per-play volume and pan. `hstream.irx` (dialogue/music streaming) stays Phase 4.
**Why**: it removes an IOP-side module (and the IOP toolchain from the critical path) for the gameplay sound set, works on PCSX2 and on the
console from the same ELF, and the audsrv API already covers voices, volume and pan. Verified on PCSX2 (`audiotest` config key: audsrv starts,
a sample loads and plays on a voice); audible quality is a console question (Phase 6, ADR-022).
**Consequences**: audsrv owns the SPU2 memory map and voice allocation (24 voices on core 0), so reverb presets and 3D distance filtering are limited
to what the client computes (volume, pan); `sound_environment` reverb is deferred; without `sbv_patch_enable_lmb` the module load hangs (found on PCSX2).

## ADR-031 Phase 3 as data converters plus an engine integrator
**Decision**: Phase 3 is split into eight data workstreams (world collision, object collision models, units/globals, weapons, animation state tables,
HUD, sound, effects), each a Python converter with its own pack (`assets/hp2/<map>.<kind>.hp2`), a host-tested pure-C reader and a format doc, plus the
engine systems that consume them (player controller, weapons, projectiles, damage, HUD and effect rendering, audio client, checkpoints). The contract is
`docs/14-phase3-plan.md`; cross references are Xbox tag ids, as in Phase 2.
**Why**: the converters are independent reverse-engineering jobs with tests against real maps; keeping their packs separate lets each be rebuilt and
validated alone.
**Consequences**: the engine loads up to eight extra packs per level (cfg keys `coll`, `cmod`, `unit`, `weap`, `ansm`, `hud`, `snd`, `fx`); objpack gets extra
roots (models/animations the weapons and units reference).

## ADR-032 World collision from the Xbox collision BSP (`COLn`)
**Decision**: each `structure_bsp` collision BSP becomes a compact `COLn` section (8-byte 3D nodes, the 2D BSP, surfaces reduced to vertex loops, edges dropped). Ray hits take the
tree's candidate surface and accept it when the crossing lies inside the polygon or solid space follows 1 mm further; sphere sweeps and depenetration test faces, edge cylinders and
vertex spheres against the polygons through a capsule walk of the tree; the two-sided surfaces of the data are coincident twin pairs, so the reader has no back-face logic.
**Why**: the Xbox data has no empty 2D leaves (the 2D tree names a surface even in open space) and about 1 mm slop between planes and polygons; dropping edges halves the size.
**Consequences**: 0.3 to 1.66 MiB per BSP (62 MiB for the ten maps); results can differ from the original engine by millimetres at polygon edges and in sliver leaves; a sphere that starts inside a wall needs `coll_sphere_overlap` before sweeping; EE cost is unmeasured (host: about 460 walking sweeps per millisecond). Details: docs/15-coll-section.md.

## ADR-033 Object hit shapes: conservative node-local boxes (`CMOD`)
**Decision**: object hit detection uses one oriented box per (region, permutation, node, surface material), fitted to that group's collision vertices in the node's default-pose frame (smaller of
the axis-aligned and principal-axes box plus 0.002 wu), posed with the node's world matrix and tested with a ray or sphere. Damage tables (materials with head flag and multipliers, regions, vitality
constants) live in the same section. **Why**: the originals are small node-local BSPs; a box per group is conservative for 100 % of the source vertices and costs at most 31 box tests per model.
**Consequences**: concave parts are filled in, render geometry outside the collision geometry is not hittable (as in the original), the tag bounding radius cannot be used for culling, the collision
permutation is chosen by name. Details: docs/16-cmod-section.md.

## ADR-034 Unit constants in their source units (`UNIT`)
**Decision**: `globals` player constants, every biped, the basic vehicle data, seats and the material table are stored as the tags have them (wu/tick for jump and vehicle speeds, wu/s for player
speeds and landing velocities); gravity is derived from the data (0.0035652 wu/tick^2 = 3.2087 wu/s^2 on every map, proved by the cached falling velocities); walking speeds come from `globals`
(`use player physics`) and the animation graph's root motion; health, shield and recharge come from the collision model. **Why**: units are proved against the data and a tuning pass compares
directly with the tags. **Consequences**: the engine converts at load (`player_params_from_tags`); look limits and per-material impact responses have no tag source; the acceleration unit of
`globals` is unproved (engine constant); driving a Warthog needs the physics tag in a later phase. Details: docs/17-unit-section.md.

## ADR-035 Weapons as tables plus a pure 30 Hz state machine (`WEAP`)
**Decision**: one `WEAP` section holds every weapon, projectile, damage effect, equipment, grenade and item collection; projectile velocities stay in wu/tick; ready, put-away and melee times
come from the first-person animation frame counts (the tags have none); `sim/weapdef.c` has the weapon state machine (ready, rate ramp, spread, ammo and reserve, reload incl. shell-by-shell,
charge, heat, battery, melee, put away, zoom) with no engine dependencies. **Why**: deterministic and host-tested with every weapon of all ten maps. **Consequences**: the material-modifier order,
the area-damage falloff and charge-on-release are modelled, not verified; item collections are untested (no campaign map has one). Details: docs/18-weap-section.md.

## ADR-036 Animation state tables as a separate section (`ASTB`)
**Decision**: the `model_animations` state blocks (unit/class/type tables, first person, vehicle, device, damage, per-animation variants and events) are converted into `ASTB`, keyed by the
same graph tag id and animation index as `ANIM`; lookups are by name (unit label, weapon class, weapon type label). **Why**: ANIM's layout stays fixed and its pose data small.
**Consequences**: ANIM and ASTB must be built from the same graph set (objpack passes the union of the roots); loops and the weapon class are resolved by the engine. Details: docs/19-ansm-section.md.

## ADR-037 HUD data: pre-resolved 640x480 layouts and alpha-ramp textures (`HUD0`)
**Decision**: `hudconv.py` writes HUD records (anchors resolved to a 640x480 virtual screen), a sprite table, `FONT`, `STRS` and `HUDT` (all HUD bitmaps as white alpha ramps or converted
textures, one mip, power-of-two padded). **Why**: the engine needs no tag parser or layout maths; the data is host-tested and fuzzed. **Consequences**: the anchor rule for non-top-left anchors,
meter semantics and overlay flags are unverified until Phase 6; the sniper scope and screen effects are not converted. Details: docs/20-hud-section.md.

## ADR-038 Gameplay sounds as pre-wrapped audsrv samples (`SND0`/`SNDD`)
**Decision**: gameplay `snd!` tags (roots: weapons, projectiles, damage effects, effects, bipeds, globals, equipment, unit HUDs) become SPU2 ADPCM mono <= 22050 Hz, one audsrv sample (APCM
header + blocks) per permutation, <= 1.4 MiB resident per map chosen by loss per byte (drops reported; weapon sounds are never dropped). **Why**: fits ADR-030 and the SPU2 budget; the Xbox format
was verified on 39 M block boundaries. **Consequences**: 85-90 % of the reachable audio does not fit (footsteps and scripted impacts are the casualties); quality about 25 dB SNR; dialogue and music
are Phase 4. Details: docs/21-snd-section.md.

## ADR-039 Effects as tables plus a sprite-sheet texture pack (`FXD0`/`FXTX`)
**Decision**: the gameplay effect closure (roots `matg foot weap proj jpt! bipd vehi eqip garb itmc` plus particle death/collision effects) is converted into flat tables (effects, events, parts,
emitters, particle/decal/light/physics/particle-system definitions, bitmap sequences); sounds, damage effects and objects are referenced by tag id; sprite sheets are single-mip PSMT8/PSMT4 textures
keyed by bitmap tag id. **Why**: a billboard renderer on the EE needs no VU program. **Consequences**: dynamics of particle systems, lights and object parts are not created by the runtime
(ADR-041). Details: docs/22-fx-section.md.

## ADR-040 The game simulation: one pure-C module over the data readers
**Decision**: `sim/game.c` owns the 30 Hz tick: player (capsule controller against `phys_world`, `sim/player.c`), inventory of two weapons with `weapon_fire_step`, projectiles (flight, gravity,
material responses, impact and detonation damage, area damage through the detonation effect's damage parts), damage with the collision regions' multipliers (`sim/vitals.c`), death animations from
the ASTB damage table, pickups, grenades, melee, zoom, camera shake. It emits events (`GE_*`) that audio, effects and the log consume; it is host-tested against the real a10 packs (every weapon of
all maps fires, the checkpoint restores state). **Why**: no engine dependency means the whole gameplay loop runs under ASan in CI-style tests. **Consequences**: player/object collision is
world-only (bodies do not block the player yet); enemy fire at the player and AI are Phase 4; vehicles are not driven.

## ADR-041 Overlay pass: HUD, particles and decals as GS sprites
**Decision**: after the object pass the renderer calls an overlay pass that draws effect billboards (depth tested, read-only Z, sorted by blend then depth) and then the HUD (`ui/hud.c` layout of
the HUD0 records, `render/sprite.c` emitter through PATH2 DIRECT, 512x448 from the 640x480 layout). HUD and effect textures use their own residency key ranges (`bsp_render_reserve_keys`).
**Why**: reuses the chain and texture cache with no new VU program. **Consequences**: decals are screen-facing sprites (no projective geometry), multiply blends are approximated by dark
alpha, near-camera particles are capped in size, particle collision and particle systems are not simulated; visual tuning is part of the PCSX2 pass (ADR-022).

## ADR-042 Checkpoints as a state snapshot
**Decision**: `checkpoint_save` copies the game state, object table, first-person view and effects (about 300 KB) into a static block; `checkpoint_revert` restores it (also after the player has
been dead for 3 s). Start saves, Select+Start reverts until scripts call `game_save`. **Why**: the state is plain data and pointers stay valid in one session. **Consequences**: script and AI
state join the snapshot in Phase 4; saving to memory card is Phase 5.

## ADR-043 Scripts: a cooperative tree-walking machine over `SCRP`
**Decision**: `SCRP` holds the reachable expression trees of a scenario as pre-order 16-byte nodes with built-ins resolved to names; `script/hs.[ch]` evaluates them with explicit frame and value stacks (no C
recursion, no heap), one thread per script (128 in the pool), `sleep` / `sleep_until` by suspending the thread (default poll 30 ticks), special forms in the evaluator, library built-ins in `hs_core.c`, every
other built-in registered by name by the host; an unregistered one is a counted neutral no-op. The mutable state is one fixed-size (72 KB) pointer-free `hs_state` that checkpoints and memory-card saves copy
as bytes. **Why**: the original is itself a node-tree interpreter (17.5 k nodes on a10); a tree walk keeps the converter trivial and checkable against the data, an explicit stack makes suspension free and
bounded, a flat state makes saves and bit-exact restore tests trivial. **Consequences**: about 100 node evaluations per tick on average (about 50 us on the EE), bounded by budgets; the host owns every
game built-in and `hs_unregistered()` shows what is missing; sleeping is tick-quantised as in the original. Details: docs/24-script-section.md.

## ADR-044 Pathfinding: a navigation mesh derived from the collision BSP (`PATn`)
**Decision**: the Xbox data has no sectors, links or hints (one walkable byte per collision surface), so `pathconv.py` derives a mesh per BSP: convex walkable sectors with plane, headroom and thickness,
links typed WALK / STEP / VAULT / DROP / CLIMB with width and cost, components; `ai/path.c` answers `path_route` with A* over sectors plus a funnel, with radius, height, kind-mask and drop filters and
caller-provided scratch (no heap). **Why**: the actors and the mission bot need routes through a level whose data does not describe them; deriving from the collision surfaces keeps routes consistent with
what the capsule controller can walk. **Consequences**: the rules and thresholds are ours (validated on all 69 BSPs by sphere-swept leg tests), not read from the data; at most 300 KiB per BSP; areas the
player reaches only through scripted devices are disconnected until the doors are modelled as links (the bot then uses counted assists). Details: docs/26-path-section.md.

## ADR-045 AI data: actors, variants, encounters and command lists as flat tables (`AIDT`)
**Decision**: `AIDT` carries every actor and actor-variant tag as the whole Xbox record with references flattened to tag ids (so each field sits at its tag offset and nothing is lost), plus the scenario's
encounters, squads, starting locations, firing positions, command lists, conversations and the difficulty-dependent blocks; the reader names the identified fields (verified / plausible / unverified is
marked per field). **Why**: the behavioural thresholds are raw words in the tags; keeping the record whole lets the runtime tune constants in one place without a format change. **Consequences**:
unverified fields are used only with conservative defaults; unit and weapon tags the actors name are pulled into the object pack by `referenced_object_tags`. Details: docs/25-ai-section.md.

## ADR-046 Devices as Q32 fixed-point state machines (`DEVS`)
**Decision**: machines (doors, lifts, switches), controls and light fixtures are converted to a flat `DEVS` section (device definitions, placements, groups, trigger volumes, the script-name to placement
table, scenario markup) and run by the pure `sim/devices.c` with 1/30 s ticks and Q32 positions/powers, deterministic and without heap; device bodies do not collide with objects (a door is passable
when its power is high; the BSP stays the only collision). **Why**: scripts drive doors by power and position and wait on them; fixed-point makes saves and tests bit-exact. **Consequences**: doors do not
shove or block bodies; trigger volumes are tested against the player and unit bodies every tick. Details: docs/27-dev-section.md.

## ADR-047 Cinematics: recorded animations as control streams plus camera tracks (`CINE`)
**Decision**: scripted scenes (the intro and the mission's cutscenes) are `CINE` data: per-recording control streams for actors (replayed through the units' control record), camera tracks, titles, fades
and letterbox events, driven by `sim/cine.c` from script built-ins; the renderer draws letterbox bars, fade and titles in the overlay pass; the player is frozen while a cinematic runs. **Why**: the
originals are recorded animations driving the same engine objects, not pre-rendered video, so the existing renderer and objects play them; no FMV, no new disc budget. **Consequences**: recordings
reference objects of other BSPs (the intro crosses four BSP switches); effect overlays on actors are not drawn. Details: docs/28-cine-section.md.

## ADR-048 Streamed audio: IMA ADPCM from disc through an IO thread into the audsrv ring (`STRM`/`STRD`)
**Decision**: dialogue, music and long ambience stay in the Xbox IMA ADPCM container (stored, 2048-aligned in `STRD`, about 120 MB per map), are read in ranges by an IO thread, decoded on the EE with a
24-tap resampler and pushed to a 53 ms audsrv ring, three voices; a small `STRM` index (19-393 KiB) is resident. `sound_impulse_time` comes from the index (ADR-051). **Why**: audio does not fit in
2 MB of SPU2 RAM (ADR-038) and the EE reads disc ranges with no stall when done on a thread. **Consequences**: decoding costs EE time per voice; quality was measured (SNR, level), not heard (ADR-022);
the audsrv ring must be fed from the vsync burn loop as well as the tick (`stream_ee_pump`). Details: docs/29-strm-section.md.

## ADR-049 Units: the player's capsule controller driven by a control record
**Decision**: non-player bipeds are `unit_t` over object slots (`ai/units.c`); locomotion is `player_tick` fed by a command made from `unit_control_t` with the actor's own speeds from root motion, weapons
use the Phase 3 state machine and `game_fire_projectile`, health is the game's vitals scaled by the difficulty block, animation is the ASTB base slots, state is a pointer-free `units_t` (40 KB, 380 B per
unit) with a distance LOD and an event list. **Why**: one physics (the player's) to validate, no second set of tag numbers to prove; EE cost is dominated by capsule sweeps, so the knobs are sweeps per tick
and rate (settled skip, LOD). **Consequences**: actors collide with the BSP only; aim and fire overlays are not drawn; the EE cost per moving actor is the main risk of the AI + units budget. Details:
docs/31-units.md.

## ADR-050 AI runtime: a pointer-free state machine per actor that writes unit controls only
**Decision**: `ai/ai.[ch]` keeps all mutable AI state (actors, squads, encounters, conversations, noise, relations, counters) in one 56 KB pointer-free `ai_state`; per actor a behaviour state (sleep, idle,
patrol, alert, search, combat, charge, retreat, flee, follow) with command lists above it; perception staggered over 3-4 ticks with at most 4 rays per look; movement by `path_route` (one per tick, six
cached waypoints); combat from the named variant fields and the difficulty table; the `ai_*` built-ins map onto it through `mission_t.ai`. **Why**: the original is a large hierarchical planner whose
thresholds are not in the data; a small robust machine over the named fields reproduces the visible behaviours within the EE budget and is bit-reproducible. **Consequences**: behaviour is plausible, not
identical; sight ignores objects; tuning constants are in one place. Details: docs/32-ai-runtime.md.

## ADR-051 The script host: tagged handles, split by domain, waits data-driven
**Decision**: object, unit and device values built-ins produce at run time are `MISSION_OBJ_BIT | slot`, lists `MISSION_LIST_BIT | slot` (ring of 64), a handle with neither bit is an object-name index
resolved through DEVS; the host built-ins live in `hs_host_obj.c`, `hs_host_misc.c`, `hs_host_ai.c`, reach everything through `mission_t` and keep persistent state in plain-data structs (`mission_hostobj`
9.3 KB, `mission_misc` 1 KB). No built-in blocks: waits end by themselves because `sound_impulse_time` is computed from the stream index length, `camera_time` from the camera track and `game_saving` is
bounded; integrator side effects (checkpoint, BSP switch, revert, HUD, navigation points) are `mission_t` flags. **Why**: the data cannot tell a constant name from a computed object; scripts wait for
minutes on dialogue and cameras and coupling that to audio hardware or main's timing would make pace, saves and the bot non-deterministic. **Consequences**: dialogue timing is the file's, not the DAC's;
overlapping lines replace each other (one dialogue voice). Details: docs/30-mission-host.md, 33-host-objects.md, 34-host-misc.md.

## ADR-052 The mission app and the multi-BSP level manager
**Decision**: `mission_app.c` owns static `units_t`, script machine, devices, cinematics, AI and bot; the mission tick order is sync player, scripts, devices, object pre-hook, AI, units, game, object
post-hook, misc, cinematics. A persistent arena holds models, packs and textures; each BSP's data (render, collision, navigation) lives in a second arena released on a switch (`s_bsp_mark`); the object
table keeps objects of every BSP, inactive ones frozen and not drawn; texture keys are stable across switches. Switches happen on `want_bsp` from the script (loading stalls 0.5-2.4 s on PCSX2,
unmasked). **Why**: a10 has nine BSPs and the intro crosses four; reloading models per switch would cost far more than the BSP data. **Consequences**: the level arena stays near 21 MB of 24 MB; frame drops
concentrate at switches; masking the load with the cinematic fade is a Phase 6 item.

## ADR-053 Mission checkpoints and memory-card saves
**Decision**: `msave` = game checkpoint + units + AI + script state + devices + cinematics + host misc/objects, as raw struct copies with pointer fixups; the card image is a zero-run RLE (`blob_pack`)
written through libmc; death, `game_save` and the pause menu use it. **Why**: all state is plain data (ADR-043, 046, 049-051). **Consequences**: a save is valid only for the same build and pack
(section hash); saving blocks the tick for the card write.

## ADR-054 Gate M4 is checked with a mission bot
**Decision**: a10 cannot be played headless, so `sim/bot.c` plays it inside the engine: it picks the nearest hostile it can see, fights with the weapon it holds, picks up weapons when unarmed, goes to the
trigger volume or navigation point the scripts wait on, follows `path_route` waypoints, and cycles the control "exercise" pattern when scripts wait for tutorial actions. When it cannot reach a goal for
several seconds it teleports to the goal (counted **assist**, reported). The gate statement reports the assists and states what the bot does not cover (human timing, camera, vehicles, hand-to-hand
choices). **Why**: PCSX2 headless is the gate (ADR-022) and the scripts' progress depends on world state the bot can create; counting assists keeps the claim honest. **Consequences**: M4 means "the
mission runs start to finish under script, AI and device control at 30 fps on PCSX2", not "a human can win it".

## ADR-055 xemu is the Xbox reference emulator for parity checks
**Decision**: the original Xbox build is run in xemu (Homebrew cask, `/Applications/xemu.app`) from `scripts/xemu-setup.sh` / `scripts/xemu-run.sh`,
with a project-local config under `assets/xemu/` and the user-supplied MCPX ROM, flash BIOS and HDD image kept outside the repository
(procedure in [35-xbox-reference.md](35-xbox-reference.md)).
**Why**: parity questions (lighting, bump/specular, sky, object placement) were answered only from tag data and `refrender.py`; a running
reference gives the picture the port is meant to match. xemu is the maintained Xbox emulator and runs natively on Apple silicon.
**Consequences**: the boot files cannot be redistributed or downloaded by scripts, so a fresh checkout cannot boot the reference until the user
supplies them; xemu timings say nothing about Xbox performance, so it is used for picture parity only. Booted to the Bungie logo with real files on 2026-10-17.

## ADR-056 `ANIM` version 2 carries graph node names; models map onto graphs by name
**Decision**: the ANIM graph record gains `names_off` (node_count x 32 B names in graph order, version 2) and `sim/objects.c` drives a model's node by the graph node
of the same name, cached per (model, graph); the first-person weapon attaches to the hand node the graph names as the gun root's parent; `node_offset` and the
"arms first, gun last" assumption are gone. **Why**: the first-person weapon graphs order their nodes breadth-first over hands + gun, so the index rule posed
30 of 37 hand bones with another bone's track and the gun with fingertip tracks (the garbled view model of the a10 playtest); custom cinematic graphs on
other models have the same exposure. **Consequences**: packs must be rebuilt (`anim_open` rejects version 1); about 60 KB more per map; one strcmp pass per
new (model, graph) pair. Details: docs/12-anim-section.md section 8.

## ADR-057 EE passes between the frame header and the BSP batches must not write VU1 data memory; untextured blended materials are not drawn
**Decision**: the sky pass (`obj_render_sky`, ADR-029) emits its GS state with a PATH2 A+D packet only (`emit_gs_state`); it no longer calls
`emit_part_state`, whose trailing `UNPACK V4-32` writes two quadwords at VU1 absolute addresses 2..3. In the object program's layout those are the part's
uv scale and GIF-tag template, but the sky pass runs right after `emit_frame_header` and before the BSP batches, where addresses 0..3 hold the BSP
program's clip matrix: the world's z row and translation row were replaced by `(1, 1, 0, 0)` and the bits of a GIF tag for the whole frame, in every
frame that had a sky model loaded (every config with an `obj` pack, since Phase 2). Depth collapsed and height leaked into x and y, so the exterior BSP of
a10 read as a flat grey lightmapped blob (F10 of the a10 playtest review) and the hangar as fans of black-and-white slivers converging on the screen
centre (F11); the object pass looked right because it carries its own palette matrices, which is why the Phase 2/3 gate runs (timing gates, frames checked
for objects) did not catch it. Two further changes ride along: the sky pass is skipped when no visible cluster has the sky flag
(`render_ctx_t.sky_visible` from `vis_t.sky_visible`, saving the EE pass in interiors), and pass 1 of the BSP renderer no longer draws a material that is
both untextured and blended (`mb->tex < 0 && blend`): glass and light shaders whose base map is a cube map (a10 `cryo glass`, `monitor glass`,
docs/09) used to add a flat grey over whatever was behind them.
**Why**: VU1 absolute addresses are a per-program convention (top of each `.vsm`); the only safe rule for an EE-side pass that is called inside the BSP
frame is "GS registers only". Found with `sky3d 0` (frame identical to the no-sky render: 64 % of the pixels differed with the sky on, including walls
nowhere near the sky) and confirmed on the `a10obj` M2 fly-through config, whose world was the same blob before the fix.
**Consequences**: every a10 scene with the sky loaded now renders the world correctly (exterior hull plating, hangar, bridge, cryo bay); what remains in the
hangar is the EE clip budget: at the hangar cinematic poses 140..170 batches straddle the near plane or guard band (about 2700 triangles), the 6 ms budget
covers ~50 of them and the rest are dropped on VU1 (ADR-017); measured at both poses this loses no visible pixel (the dropped triangles sit
behind nearer geometry or outside the view), so it stays a frame-time concern only. Measurements in docs/08 ("EE clipping load").

## ADR-058 A chicago shader's base map is its least-tiled non-modulating map
**Decision**: `texconv.resolve_shader` no longer takes the first map of a `shader_transparent_chicago` as the PS2 base map. Each schi map carries a
colour function and an alpha function (u16 at +0x2C / +0x2E of the 220-byte map record) that say how the *next* map combines with it; a map that
multiplies (2) or double-multiplies (3) the next one while contributing no alpha of its own (alpha function current 0 or next map 1) is a mask or
detail tile and is skipped, and of the remaining maps the one with the smallest uv tiling is taken (`chicago_base_map`, docs/09). On the retail data
this changes the a10 space sky from the 64 px grey `star mask` (x3) to the 256 px `stars` map (x16 x10), the a30 dusk sky from the same mask to the
`sky dusk` gradient, and a30's `horizon land` from its detail tile to the horizon painting; the a30 rivers keep their first map (it supplies the
alpha), as do the Pelican thrusters and every single-map shader. sotr (generic) shaders are untouched.
**Why**: the a10 intro's "flat grey blob" that survived ADR-057 was the sky dome textured with the star mask: the blurry grey mask tiled three times
over a 20400-unit sphere, alpha-blended over the clear colour, in front of nothing (the exterior BSP is not visible from the frame-400 pose at all).
**Consequences**: object packs must be rebuilt (`objpack.py`, the `OTEX` section) for the sky change to show; the BSP `TEXS` packs are unchanged on
the ten campaign maps (no BSP material of theirs is a multi-map chicago shader whose base changes). Stars converted at half resolution are faint
(the source is a mostly black y8 map); the masked twinkle and the additive nebula layer of the three-map variant are still dropped.

## ADR-059 Render options apply to every config, not only to configs with an object pack
**Decision**: `main.c` calls `apply_options()` once, right after the first `load_bsp`, for every config; before, the only call sat inside the
`if (s_have_objects)` block, so the render keys (`textures`, `lightmaps`, `debug`, `onlybatch`, `clipbudget`, `fog`, `sky`, `horizon`, `skyfog`)
were silently ignored whenever the config had no `obj` line. The call no longer needs to follow `bsp_render_set_sky_pass`: `apply_options` reads
only the config and `s_world.sky`, which `world_load_objects` fills before the first BSP load, and `bsp_render_set_world` does not read the options.
**Why**: the M1 fly-throughs (`scripts/paths/a30.cfg`, `b40.cfg`) set fog and sky colours that had never applied, and world-only debugging configs
could not use `debug` or `clipbudget`. **Consequences**: a30 and b40 now draw with their configured fog and sky gradient (the sky colour visibly changes;
fog only tints geometry beyond 80/100 units). On the PCSX2 recompiler (2026-10-18) a30, b40 and a10 still drop 0 frames, and EE prep, VIF1/GS ms,
triangles and level arena are the same as the pre-fix ELF to within 0.01 ms: the VU programs compute the per-vertex fog value either way, and PCSX2 does not
model GS fill, so any fog cost on the GS stays unmeasured until Phase 6. A world-only a30 config now honours `debug 32` (clip stats 0) and `clipbudget`.

## ADR-060 HUD anchors on the title-safe frame, the motion sensor bottom left, tracker blips (`HUD0` version 2)
**Decision**: `hudconv.py` resolves every HUD anchor to the corners of the 7.5 % title-safe frame of the 640x480 virtual screen (48 / 36 px in) or the screen centre,
offsets x right / y down at every anchor; the unit HUD's motion sensor (background, foreground, centre) is laid out from the bottom-left anchor whatever the HUD's anchor,
its centre offset counted inward from that corner; `hud_globals` carries the `hud_sensor_blip` sprite (found by tag path) and `ui/hud.c` draws one blip per living unit
within the motion range, friendly (teams 1 / 2) yellow and hostile (3 / 4 / 5) red, from the unit tag's default team or the actor's team. **Why**: the a10 playtest (F6) found
the tracker top right and everything flush with the screen edges; a gameplay frame of the Xbox build (xemu) pinned the rule down to 2 px, and the sensor images carry the radar
in their bottom-left corner. **Consequences**: HUD packs must be rebuilt (`hudconv.py` per map, 0.3 s each; the reader rejects version 1); the velocity-sensitivity rule
(still enemies hidden) and blip fading are not modelled; `motion scale` is exported but the radar radius comes from the centre offset. Details: docs/20-hud-section.md.

## ADR-061 HUD offsets count inward from right anchors, meter bitmaps are flat masks, digits keep their cell size, weapon overlays are drawn
**Decision**: `hudconv.py` places right-anchored elements at `x = anchor - off_x - w` (the offset counts inward, like the motion sensor centre already did; y stays down,
bottom anchors keep their unverified mirror); the a8y8 meter bitmaps (`hud_unit_meters`, `hud_ammo_meters`) are converted to white alpha masks (their luminance is the tag's
fill-order ramp, not a colour), so a meter is one flat tint clipped by its value; `ui/hud.c` draws a digit sprite at its own cell size (18 x 12) centred on the 9 px advance
instead of squeezing the cell into the advance, and draws the weapon HUD's overlay sub elements (role 9: the "x" and clip icon after the ammo count). **Why**: the HUD of the
a10 game scene was a "buggy mess" (no readable ammo digits, health segments 60 px right of the shield plate, shield fill 13 px past the panel, black-to-blue luminance
gradients on the bars); an Xbox attract-demo frame (xemu, 3.5 px per virtual pixel) shows the shield / health plate, the "540 x" panel and the three-row ammo bar, and the
corrected layout lands within 2 px of it. **Consequences**: `HUD0` stays version 2, so a pack built before this change is accepted but wrong: rebuild every HUD pack
(`hudconv.py` per map). Meters clip by width, not by the Xbox's per-pixel luminance threshold (partial segments show, the ammo bar empties right to left rather than
row by row); meter translucency / opacity and the grenade type of the panel colour are not modelled. Details: docs/20-hud-section.md.

## ADR-062 Falling out of the world kills the player; the bot holds floorless hops and is rescued; the world is not drawn around a void camera
**Decision**: (1) `main.c` kills an airborne player who is more than 30 wu below the loaded BSP's lowest vertex (`FALL_KILL_MARGIN`); the existing death path reverts to the last checkpoint after 3 s.
(2) While the player is airborne with no floor within 80 wu below, a camera that lies in no BSP cluster draws no world (`bsp_render_set_void_view`, `vis_t.skip_outside`); the free camera (no game) still draws every cluster in view.
(3) The mission bot's assisted hop onto a volume with no floor under it (volumes of the next BSP lie in its space, beyond the loaded BSP's collision) holds the player there for 20 ticks so the scripts can poll the volume,
then puts the player back on the last standing position (`rescues`, counted as assists); a volume that three hops in a row do not satisfy is set aside for 60 s; three awaited volumes in a row that are off the navigation mesh
make the bot hop onto the current one; the ignore list holds 16 volumes and the shun list 12 hostiles (4 slots were overwritten before they expired). **Why**: the a10 mission run (2026-10-18) spent about 100 s of
its 750 s with the player falling forever after a hop onto such a volume (BSP 0's floor ends at x = -33.1, volume 91 lies at x = -29.4): the renderer found no cluster and drew every cluster in view (up to 55k triangles,
2441 batches, EE 35-50 ms), 1635 of 20749 frames (7.88 %) were late, and the scripts only advanced because the falling player happened to cross the next volumes. **Consequences**: the same run now
wins at 773 s with 60 assists (24 of them rescues), no fall, no void frame, 712 of 24256 frames late (2.94 %); a human who clips out of the map dies instead of falling for ever. The remaining late frames are real scene load, not
the bot: the BSP 6 finale (EE 29-52 ms, 173 consecutive frames) and the crowd scenes of BSP 0 / 8 (EE 16.5 ms but the GS finishes late: 21k object triangles, 964 batches). Diagnostics added: `[bot]` lines for every assist
and rescue (target, floor below, awaited volumes), `prof2 1` (EE prep split into sim / object animation / render build per 60 frames), unit body integrations and airborne units in the mission status line.

## ADR-063 Frame pacing no longer loses a vblank to the stream pump; carried units are pinned, not integrated; objects draw nearest first under a batch cap that follows the LOD level
**Decision**: (1) The vsync burn before the flip (`main.c`) recomputes the elapsed vblanks after `stream_ee_pump` and only then waits for a vsync; a pump that ran across the vblank it was about to wait for used to make `graph_wait_vsync` catch the *next* one, so the frame landed three vblanks (50 ms) after the last flip instead of two.
(2) `units_pin` holds a unit that a script suspended (`unit_suspended`) or an attachment carries (`objects_attach`): position and yaw are set, velocity zeroed, `on_ground` kept at 1 and the settle count kept, so `step_body` skips the capsule sweeps (they used `units_teleport` every tick, which cleared `on_ground`, so every carried unit swept for nothing and ran the airborne action).
(3) `mission_sync_devices` decodes each device's placement and animation once (static table) instead of every tick. (4) The object pass culls first, ranks by nearest surface, poses and draws nearest first and stops at the batch cap (objects beyond it cost no pose, light or model walk), and the cap is
900 x 100/100/100/85/60 % at the LOD controller's levels 0-4 (`lodctl_batch_cap`). (5) Two options, both **off by default** because they change the simulation: `unitlod <wu>` (bodies farther than that from the player integrate every other tick, the existing distance LOD that nothing enabled) and `cinelod 1`
(all bodies integrate every other tick while a cinematic runs). (6) Diagnostics: `prof2 1` splits the sim tick (mapp, serve, bspsw, stream, fx, audio, hud: avg/peak per 60 frames), `[prof] DROP` lines carry the EE time, the GPU wait, the vsync burn and the present time, `airlog 1` lists units that are not on the ground.
**Why**: measured on the a10mission run (2026-10-18), not guessed. The "crowd scenes of BSP 0 / 8" were not GS-bound: their drop lines read EE 16.6 ms, GPU wait 0.0, burn 16.8, present 16.7, i.e. the pacing race above (EE work ends within a pump of the vblank); 470 of the 703 late frames were of this kind. The BSP 6 finale
(EE 29-52 ms) was EE-bound: sim 11-17 ms of which `units` 5 ms (18 body integrations per tick at about 0.27 ms each, mostly carried or cutscene units), `ai` 1.5, `post` 1.2, devices 1.2 (the per-tick decode), `stream` 1.4; render build 15-28 ms with the object pass at its 900-batch cap after posing every visible object.
**Consequences**: the same run (same seed, same bot) now ends with identical simulation results - `game_won` at tick 23210, 60 assists of which 24 rescues, 24256 frames - and **90 of 24256 frames late (0.37 %, was 703 = 2.90 %)** on the recompiler at normal speed; `a10game` 0 of 1081. 60 of the 90 are still the finale (EE 34-41 ms: sim 9-15 ms, build 15-20 ms), 14 more the BSP 6 load (one 2.4 s stall per BSP switch, 8 for BSP 0 / 7), the rest single stalls. Budget decision for the finale: it stays over budget by 1-8 ms for about 2 s
at the top LOD level; the way to buy that back is `cinelod 1` (about 2.5 ms of `units`), but a run with it (and with `unitlod 20`) takes a different path through the mission (win at tick 24201-24600, 73-89 assists instead of 60, 0.7-0.9 % late), so the Gate M4 numbers are quoted without them until the bot's route is stable against sim changes.
`vis` in the profiler now counts drawn objects only (objects past the cap are in `skip`). Carried and suspended units now play the ground idle animation instead of the airborne one.

## ADR-064 Intro parity with the Xbox: sky node pose and palette, additive blends, lightmap tone curve and chroma, cinematic near plane
**Decision**: (1) `obj_render_sky` skins the sky model with the bind-pose inverse of its node (a sky has no animation, so Halo poses its nodes with the identity and
the skin matrix `world(pose) * inverse(world(default))` reduces to the inverse); a10's `sky_x10`/`space` node is yawed 90 degrees by default, which turns the dome, galaxy and
planets by -90 degrees: without it the intro's planet stood behind the camera. (2) `SKY0` carries the model of the scenario's second sky (u32 at +60, was padding); the sky pass
draws the palette entry of the camera's cluster (`vis_t.sky_index`, the camera cluster's `sky` field, else the first visible cluster with a sky): a10's exterior BSP 7 (the intro's
space shots) has one cluster with sky 1, the nebula sky; every window onto space elsewhere is sky 0. (3) A sky dome whose chicago shader stacks maps (nebula + stars) is converted
as one part per additive map (`modelconv.add_sky_layers`, pseudo shader ids `objtex.layer_shader_id`). (4) `mat_binding_t.add`: additive materials (shader blend 3) draw as
`Cs + Cd` in the BSP (doubled: the exhaust map is a 104 grey), object and sky passes; every other blend still draws as alpha. (5) Lightmap luminance goes through a tone curve
(`texconv.lightmap_curve`: white point 249 -> 127 x gain, exponent 0.5, gain 1.7) instead of the linear gain 1.5, and the vertex chroma is pulled 60 % towards neutral
(`CHROMA_STRENGTH` 0.4). (6) The cinematic near plane is never below the engine default 0.05: the VU1 object program drops every object nearer than about 0.04 (the captain 0.27 away
in the bridge vanished at the script's 0.01).
**Why**: frame-by-frame comparison of the PS2 intro with the Xbox intro video at the same times (`scripts/cmp-intro.sh`, 2026-10-18): black sky with a teal smear, no planet, yellow-green
bridge, white hangar floors, no Keyes in the bridge's long shot. The lightmap pages of a10 are bimodal and amber (BSP 1: a quarter of the texels at 170..249, mean colour 120 / 90 / 51),
so linear gain and full chroma doubled every lit surface to white and tinted it yellow, while the mean brightness of the intro matched the Xbox within 15 %.
**Consequences**: object packs (`objpack.py`: SKY0, sky layers, a30/a50 second skies) and TEX/BSP packs (`texconv.py`, `bspconv.py`: tone curve, chroma) must be rebuilt
(`scripts/build-world.sh`); a30 now draws its cluster-selected sky (unverified against the Xbox). Not fixed, found by the same comparison: colour-change tints (crew jumpsuits are red / yellow /
orange on the Xbox, near-black here; Keyes' uniform and the marines too), Cortana's purple hologram (multi-stage `sotr` colours), screen effects (desaturation, convolution), the
exhaust and galaxy glow (additive sprites / bloom), the halo ring in the first shot (edge-on here), marines' rifles, and the intro's pacing at the end of the bridge (about 4 s early: `sound_impulse_time`).

## ADR-065 Colour change (`CCHG`, a second pass per part), object lighting on the BSP's tone curve, XBE analysis
**Decision**: (1) Object tags carry up to four change colours (object tag +0x164, each with up to eight permutations of cumulative weight and a lower / upper rgb bound); a `shader_model`
with a colour change source a..d (+0x4C) and a multipurpose map (+0xBC, blue channel = mask) gets a second texture in its `OTEX` binding (the base map with the mask as alpha,
`tex_binding.change` / `texture2`, the 16-bit field that was reserved at +10 and the first of the free bytes at +24), and `objpack.py` writes a `CCHG` section (`chgconv.py`,
`world/cchg.[ch]`). The object pass draws such a part a second time over the first, alpha blended at the same depth, with the ambient and sun colours multiplied by the object's
change colour (picked by a seed from the object index and definition), so where the mask is 1 the result is base x tint x light. Keyes' uniform keeps only its grey permutation
(`chgconv.GREY_ONLY`; the Xbox intro shows grey). The first-person arms pick with seed 1. (2) The object lighting probes (`scenconv.cluster_lights`) use the BSP's tone curve and chroma
strength (`texconv.lightmap_tone_rgb`) with a gain of 1.7 x 1.8: the linear gain 1.0 they used left characters about 2.5 times darker than the walls they stand against.
(3) `default.xbe` was analysed (docs/36-xbe-analysis.md, `tools/xbe/`).
**Why**: the Xbox intro shows red / yellow / orange / purple crew jumpsuits, khaki-free skin tones and a captain as bright as the bulkheads; here they were black, pink and charcoal.
The tag data answered colour change completely; the executable showed that Halo assembles its combiner programs at runtime and that its environment pass adds the lightmap to a vertex term instead
of doubling it.
**Consequences**: object packs must be rebuilt (`objpack.py` / `build-world.sh`; the `OTEX` and `SCEN` sections change, a `CCHG` section is new, a pack without it still loads with no tints);
models are not duplicated (the second pass reuses the part's batches), the cost is one more draw of the tinted parts (crew, marines' faces, props with a change colour) and ~0.4 MiB of `OTEX` on a10.
Marine armour, rifles, Cortana's hologram colour and the screen filters are still open (ADR-064).

## Open decisions
None.
