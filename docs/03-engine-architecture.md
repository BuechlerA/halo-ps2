# Engine Architecture

## 1. Approach: clean-room reimplementation driven by the original data

The Xbox executable cannot be emulated or recompiled onto a PS2 at any frame rate,
let alone 60. The gameplay, however, lives almost entirely in **data**: Halo's
"Blam" engine is tag-driven. Weapons, bipeds, AI behaviour parameters,
encounters, scripts, animations, geometry, lighting and sounds are all in the
`.map` cache files. We therefore:

1. **Convert** the user's own Xbox cache files offline into PS2-native pack files
   (host tools, `tools/`).
2. **Reimplement the engine's rules** in C on the EE, reading those packs. The
   behaviours are specified from the publicly documented tag semantics and
   observation of the original game; no Xbox code is decompiled or reused.

This is the same strategy as the open-source reimplementations of Quake-era
engines, except we also own the data converter.

```
 Xbox disc (user's)                   host (macOS)                              PS2
 ┌───────────────┐   extract-xiso   ┌───────────────────┐   halo2ps2   ┌──────────────┐
 │ maps/a10.map  │ ───────────────▶ │ decompress + parse│ ───────────▶ │ a10.hp2      │
 │ (zlib, v5)    │                  │ Xbox tag structs  │              │ RES BSP0..n  │
 └───────────────┘                  │ → PS2 layouts     │              │ TEX SND STR  │
                                    └───────────────────┘              └──────┬───────┘
                                                                              ▼
                                                                    halo-ps2.elf (this repo)
```

## 2. Processor assignment

```
            ┌──────────────────────── EE core (294.9 MHz) ─────────────────────────┐
            │  sim/   object system · physics · AI · HSC script VM · animation state │
            │  world/ cluster/portal visibility · LOD · streaming                    │
            │  render/ DMA-chain builder · texture residency · material sort         │
            │  VU0 macro-mode: matrix math, skinning palettes, collision SIMD        │
            └───────────┬───────────────────────────────────────────┬───────────────┘
                        │ VIF1 chains (UNPACK + MSCAL)               │ GIF PATH3 (texture IMAGE)
                        ▼                                           ▼
            ┌─── VU1 (16 KB) ───┐   XGKICK (PATH1)     ┌──────── GS (4 MB eDRAM) ────────┐
            │ xform · light ·   │ ───────────────────▶ │ 512×448 CT16S ×2 · Z24 · 2.25 MB │
            │ clip · skin ·     │                      │ texture window                   │
            │ sprites · decals  │                      └──────────────────────────────────┘
            └───────────────────┘
                        SIF RPC
            ┌─────────────────────────── IOP (36.9 MHz, 2 MB) ──────────────────────────┐
            │  hstream.irx: DVD read-ahead for packs, dialogue, music                    │
            │  hsound.irx:  SPU2 voice allocator, ADPCM uploads, reverb, 3D panning     │
            │  padman / mcman (stock)                                                   │
            └───────────────────────────────────────────────────────────────────────────┘
```

Rule of thumb: **the EE never touches a vertex** and **VU1 never touches game
state**. The EE builds command lists; hardware executes them.

## 3. Timing model: 30 Hz simulation, 60 Hz presentation

Halo CE's simulation is a fixed 30 Hz tick. We keep that exactly (R3) and present
at 60 Hz by **interpolating** between the previous and current tick for everything
visible: object transforms, camera, animation node matrices, particle positions.
This is the same technique MCC uses to run the engine above 30 fps, and it halves
the simulation cost per rendered frame.

```
 vblank:   |0        |1        |2        |3        |4
 EE:       [tick k  ][prep  ] [prep ][stream ] [tick k+1][prep ] [prep ][stream ]
 VU1/GS:            [frame 2k        ] [frame 2k+1     ] [frame 2k+2      ] ...
 shown:    frame 2k-1 (t=k-0.5)  frame 2k (t=k)   frame 2k+1 (t=k+0.5) ...
```

- `render/` consumes a double-buffered **render snapshot** (compact POD arrays of
  visible object transforms, node palettes, effect states) written by the tick;
  it never reads live sim structures. Interpolation factor `alpha ∈ {0, 0.5}`.
- Input is sampled every frame (60 Hz) but applied at tick boundaries, with the
  look delta accumulated so aiming feels 60 Hz.
- One frame of latency is accepted (identical to the original's render-behind).

## 4. Memory model
- **No heap after load.** `core/mem.c` provides arenas: `g_level_arena` (reset per
  mission), `g_bsp_arena` (reset per BSP), `g_frame_arena` (reset per frame),
  fixed-capacity pools for every dynamic object kind (sizes from the scenario's
  object counts + headroom). Overflow is an assert in debug and a logged drop in
  release.
- All pack data is **position-independent and loaded in place**: offsets, not
  pointers; 16-byte alignment everywhere; VIF/GIF packets pre-built so a mesh draw
  is a DMA `ref` tag pointing into the loaded section.
- Hot per-tick data (object table, physics bodies, AI actor state) is laid out as
  structure-of-arrays so tick loops stream through the 8 KB D-cache; cold data
  (names, tag metadata) is kept separate.
- Scratchpad: used by physics (collision query working set) and the DMA-chain
  builder (packet assembly before `toSPR→GIF` is unnecessary — we write chains
  directly to uncached RAM with `sqc2`/128-bit stores).

## 5. Rendering pipeline

### 5.1 Frame graph
1. **Visibility** (EE): camera cluster → portal traversal with frustum culling
   (Halo BSPs already carry clusters and portals); objects tested by cluster
   membership then bounding sphere. Output: visible BSP cluster list, visible
   object list, light list.
2. **Material sort** (EE): draws bucketed by VU1 program → texture → CLUT to
   minimise microprogram swaps and texture uploads. Transparent last, sorted
   back-to-front by cluster.
3. **Texture residency** (EE): for each required texture, if not in the VRAM
   window, append a PATH3 `IMAGE` upload to the texture DMA chain and
   evict LRU. Budget guard: if the frame would exceed 1.5 MB of uploads, drop the
   highest mip level of the largest offenders for this frame.
4. **Chain build** (EE): a single VIF1 source-chain per frame: `ref` tags into
   baked geometry packets, interleaved with `cnt` packets carrying per-draw
   constants (matrices, lights, fog, GS register setup via `DIRECT`).
5. **Kick**: GIF chain (textures) and VIF1 chain (geometry) started; EE moves on
   to the next tick / prep. GS `FINISH` + vsync synchronise presentation.

### 5.2 VU1 microprograms (`engine/src/vu1/*.vsm`)
One program per material class, all sharing a common data layout and
double-buffered input (`XTOP`):

| Program | Inputs per vertex | Passes | Used for |
|---|---|---|---|
| `bsp_lit` | pos s16, uv s16, colour u8 | base texture × vertex colour (modulate) | BSP opaque pass 1 |
| `bsp_lightmap` | pos s16, lm-uv s16 | lightmap luminance in alpha, blend `Cd×As` | BSP opaque pass 2 |
| `model_skin` | pos s16, normal s8, uv s16, 2 bone idx + weight | Gouraud dir+ambient light, up to 2 bones, modulate | bipeds, vehicles, weapons, FP arms |
| `model_rigid` | pos s16, normal s8, uv s16 | as above, 1 node | scenery, devices, projectiles |
| `sprite` | centre, size, colour, uv rect | camera-facing quad expansion → GS SPRITE | particles, lens flares, contrails (as strips) |
| `decal` | pos, uv, colour | Z-biased blended tris | decals, blob shadows |
| `sky` | pos, uv | no Z write, fog off | skybox models |
| `glass/water` | pos, uv (+ env uv) | alpha + optional sphere-map 2nd pass | `shader_transparent_*` subset |

Each program clips against the guard band with `CLIPW` + ADC (the cheap PS2 trick
from the sample code); true near-plane clipping is done only for the
`model_skin` first-person program where polygons actually cross the near plane.

### 5.3 Lighting model mapping
| Halo | PS2 |
|---|---|
| BSP: lightmap (RGB radiosity) × base × detail, bump + specular | Pass 1 base×vertex chroma, pass 2 lightmap luminance. No detail/bump/specular. |
| Objects: lightmap-sampled ambient + up to N dynamic lights, per-pixel | Per-vertex on VU1: ambient from the BSP lighting probe at the object position + 1 directional (sun/main light) + up to 2 nearest point lights |
| Fog: per-pixel planar + atmospheric | GS per-vertex fog `F` from distance; planar fog approximated per cluster |
| Dynamic shadows: blob projected | `decal` program blob under bipeds/vehicles |
| Lens flares, light volumes | Sprites |

### 5.4 Resolution and formats
512×448, PSMCT16S double-buffered, PSMZ24, interlaced output from a full-height buffer (FFMD=0), dithering on.
Fallback knobs (all runtime): 640×448, Z16S, field rendering. Chosen in
[06-decisions.md](06-decisions.md).

## 6. Simulation (`engine/src/sim/`)
Reimplements the Blam object model with the original tag semantics:

- **Object system**: object table with typed headers (`biped`, `vehicle`, `weapon`,
  `equipment`, `projectile`, `scenery`, `device_machine`, `device_control`,
  `device_light_fixture`, `placeholder`, `sound_scenery`), node matrices,
  attachments, parent/child, damage sections, markers.
- **Physics**: Halo's own (pre-Havok) model — capsule/sphere bipeds against the
  collision BSP (`structure_bsp` collision tree + `collision_model` per object),
  vehicle `physics` tag mass-points and powered suspension, projectiles with
  gravity/drag tables. Fixed-point-free: single-precision float with VU0.
- **Damage**: `damage_effect` tags, shields/health, regions, `material_type`
  responses.
- **Animation** (`anim/`): `model_animations` graphs: state machines for bipeds
  and vehicles, overlays (aim, look), blend between two animations max, IK-free
  (Halo CE has none). Node palettes built on VU0.
- **Units/weapons**: weapon triggers, magazines, firing modes, grenades, melee,
  zoom, dual-handedness of first-person model; aim assist tables.
- **Devices and triggers**: machines, controls, light fixtures, trigger volumes.
- **AI** (`ai/`): `actor`/`actor_variant` behaviour (charge, retreat, search,
  guard, vehicle use, grenade throwing, danger evaluation), encounters/squads/
  platoons, firing positions, pathfinding over the BSP's `pathfinding` sectors.
  This is the largest single reverse-engineering effort of the project.
- **Scripting** (`script/`): interpreter for the compiled Halo Script (HSC) node
  tree stored in the `scenario` tag — expression nodes, globals, `sleep`/`wake`,
  the ~400 built-in functions. Only functions actually used by campaign scripts
  need implementing (the converter emits a usage report).
- **Cutscenes**: camera tracks (`camera_track`), cutscene flags/titles, letterbox.
- **Game state**: checkpoint = serialised snapshot of the object table, globals,
  script state, AI state (≤ 512 KB); the revert path restores in RAM.

## 7. World and streaming (`engine/src/world/`, `stream/`)
- Pack format `.hp2`: header, section table, each section LZ-compressed
  (LZ4-class, custom tiny decoder on the EE).
- Mission load: `RES` → level arena, first `BSP` → bsp arena, `TEX` subset by
  material → texture pool, `SND` banks → SPU2 via IOP.
- BSP switch (`switch_bsp` script command): fade, flush bsp arena, stream next
  `BSPn`, rebuild residency.
- Dialogue/music: `hstream.irx` pulls from `SND` stream regions straight into the
  SPU2 ring; EE only issues cue commands.

## 8. Audio (`engine/src/audio/`)
`sound` tags → permutations in SPU2 RAM; `sound_looping` → voice pairs with
cross-fade; 3D: per-voice volume/pan from listener distance and the tag's
distance curve; `sound_environment` → core reverb preset per cluster.
48 voices total: 8 reserved (UI, dialogue, music stereo), 40 pooled with
priority eviction.

## 9. Platform layer (`engine/src/platform/`)
GS/VIF/DMA init, double buffer swap, vsync, pad (libpad), timers (COP0 Count),
SIF RPC client for the IOP modules, memory card (libmc), TTY logging, profiler
overlay (EE ms, VU1 busy via `VIF1_STAT`, GS via `FINISH` timestamps, texture
upload bytes, triangle counts, dropped frames).

## 10. Source layout

```
engine/
  Makefile                 ps2sdk-style; builds halo-ps2.elf (dependency files, host tests in tests/)
  src/
    main.c                 boot, halo.cfg, pipelined frame loop
    platform/              video.c pad.c fbdump.c profiler.c timer.h
    core/                  mem.c (arenas) config.c (halo.cfg)
    render/                bsp_render.c (frame build + kick)  vis.c (cluster/portal visibility)
                           camera.c flycam.c  clip.c (EE clipper)  chain.c upload.c gs.h (DMA/GS encodings)
                           texcache.c (VRAM residency)  text.c (overlay font)  selftest.c
    vu1/                   bsp.vsm (bsp_lit + bsp_lightmap entry points)
    world/                 bsp.c (BSPS v2 reader)  tex.c (TEXS/LMn reader)  world.c (loading, texture source)
    stream/                hp2.c file.c
    sim/ anim/ ai/ script/ audio/ ui/ save/    later phases
iop/
  hstream/ hsound/         IRX modules (IOP toolchain)
tools/
  halo2ps2/                Python pipeline: map parsing -> .hp2 packs (bsp, bspconv, bitmaps, texconv, ...)
scripts/                   run-pcsx2.sh run-headless.sh profile-pcsx2.sh profile-m1.sh build-world.sh
                           fb2png.py campos.py scout-views.py refrender.py paths/*.cfg
docs/                      this folder
assets/                    gitignored: extracted ISO, converted packs
```

Pure-C modules (everything except `platform/`, `bsp_render.c`, `selftest.c`, `world.c`, `file.c`) have host
tests in `engine/tests/` built with ASan/UBSan (`make -C engine/tests`, also run by CI).

## 10a. Phase 1 as built
Frame: `vis_run` (portals, frustum, per-batch culling) -> material sort -> `tc_acquire` for each texture
(uploads queued on a GIF chain) -> one VIF1 chain: frame constants, then per material a state chunk
(A+D through PATH2, `FLUSHA` first) and one REF tag per batch carrying the previous MSCAL, then the
lightmap pass grouped by page, the EE-clipped polygons (PATH2 `DIRECT`), the overlay and a GS FINISH.
Decisions: ADR-017 (projection and clipping), ADR-018/019 (data formats), ADR-020 (lighting, fog, sky),
ADR-021 (loop, config, headless checks). VU1 data memory layout is documented at the top of `vu1/bsp.vsm`.

## 11. Coding rules (enforced by review)
- C11, `-G0`, no `double`, no `malloc` after load, no recursion in tick paths,
  no `float` ↔ `int` conversions in inner loops without VU0.
- Every DMA-visible struct is `__attribute__((aligned(16)))` and documented with
  its qword layout next to the VU1 program that consumes it.
- Each subsystem exposes `xxx_init / xxx_tick / xxx_render_prep / xxx_shutdown`
  and a `xxx_debug_overlay`.
- Profiler zones around every tick and prep stage; budgets from
  [02-specs.md](02-specs.md) are asserted in debug builds.
