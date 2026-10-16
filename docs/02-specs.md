# Project Specification — Halo: Combat Evolved on PlayStation 2

## 1. Goal

Play the **complete 10-mission Halo: Combat Evolved campaign** on a retail
PlayStation 2 at a **locked 60 frames per second**, from the user's own Xbox copy of
the game. Gameplay rules, encounter design, AI behaviour, scripting, weapons,
vehicles and level layouts must match the Xbox original as closely as the data
allows. Visual fidelity is traded away first; frame rate and gameplay are never.

## 2. Non-goals (explicitly out of scope)
- Multiplayer (split-screen or System Link), co-op campaign.
- The 13 multiplayer maps and the `xdemos` content on the disc.
- Bit-exact rendering parity with the Xbox (bump mapping, specular, detail maps).
- Emulation or recompilation of the Xbox executable (`default.xbe`). Nothing from
  the executable is reused; see [06-decisions.md](06-decisions.md).
- PAL 50 Hz (tracked as a late-phase item, not a launch requirement).

## 3. Hard requirements

| ID | Requirement | How it is verified |
|---|---|---|
| R1 | All 10 campaign missions completable start to finish, all difficulties, on real hardware | Playthrough log per mission, per milestone gate |
| R2 | **60 fps** (59.94 Hz): < 1 % missed vsyncs over a full mission, 0 missed in the 95th-percentile encounter | Built-in profiler overlay + frame-drop counter dumped at mission end |
| R3 | 30 Hz simulation tick, identical to the original engine's tick rate | Tick counter vs. wall clock; scripted sequences time-match the Xbox |
| R4 | Fits in 32 MB EE RAM + 4 MB VRAM + 2 MB SPU2 RAM with **no runtime heap allocation after level load** | Allocator asserts; per-level memory report |
| R5 | Single-layer DVD (≤ 4.37 GiB), boots on fat and slim NTSC consoles | ISO build script + hardware boot |
| R6 | Checkpoint/resume in RAM, save to 8 MB memory card ≤ 512 KB per slot | Save size report |
| R7 | Repository ships **no copyrighted assets**; the pipeline converts the user's own ISO | `.gitignore` + CI check that `assets/` is untracked |
| R8 | Every milestone verified on PCSX2 **and** on a real console | Milestone checklist |

## 4. Reference baseline (what we start from)

From the user's NTSC disc (read-only listing; sizes are the Xbox-compressed cache
files, decompressed sizes are measured by `tools/halo2ps2/mapinfo.py` in Phase 0):

| Mission | Map | Compressed size | Notable systems |
|---|---|---|---|
| 1 Pillar of Autumn | `a10` | 165.9 MiB | Interior BSPs, tutorial scripting, first encounters |
| 2 Halo | `a30` | 145.5 MiB | Huge outdoor BSP, Warthog, Banshee, Pelican drops |
| 3 Truth and Reconciliation | `a50` | 168.1 MiB | Night, gravity lift, Covenant interiors, many actors |
| 4 Silent Cartographer | `b30` | 155.3 MiB | Open island, vehicles, long sightlines |
| 5 Assault on the Control Room | `b40` | 178.2 MiB | Scorpion, Banshee, large bridges, snow fog |
| 6 343 Guilty Spark | `c10` | 159.2 MiB | Flood introduction, swamp, dense foliage |
| 7 The Library | `c20` | 73.4 MiB | **Mass Flood encounters** — worst-case AI/physics load |
| 8 Two Betrayals | `c40` | 158.6 MiB | b40 reversed, three-way battles, Sentinels |
| 9 Keyes | `d20` | 124.9 MiB | a50 interiors, Flood, dialogue-heavy |
| 10 The Maw | `d40` | 162.5 MiB | a10 reversed, Warthog run, timed scripting |
| Menus | `ui` | 13.5 MiB | Fonts, UI bitmaps, strings |

Campaign total **≈ 1.49 GiB** compressed. Measured with
`tools/halo2ps2/mapinfo.py` on the extracted disc (full table in
[reports/map-headers.txt](reports/map-headers.txt)):

| | min | max |
|---|---|---|
| Decompressed cache file (campaign) | 135 MiB (`c20`) | 267 MiB (`b40`) |
| Resident tag data (index + all non-BSP tags, incl. animations, collision, AI, scripts) | 8.5 MiB (`c20`) | 16.2 MiB (`b40`) |
| Share of the Xbox tag-space limit (23 MiB, `0x1700000`) | 37 % | 70.5 % |

Of that tag data, 2.3–4.9 MiB per map is model vertex data (converted to VU1
strips on PS2) and 5.9–11.0 MiB is tag structs
([reports/tag-inventory.md](reports/tag-inventory.md)). Everything else in a
cache file is raw data streamed by the Xbox engine: bitmap pixels, sound samples
and the structure BSPs. Our resident budget (9 MB, below) covers both the
structs and the converted models, i.e. ~55 % of the worst-case Xbox figure, so
conversion must shrink data (quantised animations, pruned permutations,
pointer-free layouts), not just reformat it.

Xbox Halo gameplay constants to preserve: 30 ticks/s simulation; 1 world unit =
10 feet (3.048 m); default field of view 70°; player speed, jump, gravity and
weapon tables come straight from the `globals`/`biped`/`weapon` tags and are not
hand-tuned.

## 5. Performance budgets

### 5.1 Frame (16.67 ms, 59.94 Hz)
Simulation runs every other frame. The schedule is fixed so frame time is
predictable; slack on non-tick frames is used for streaming and audio.

| Frame type | EE work | Budget |
|---|---|---|
| **Tick frame** (odd) | Simulation tick (objects, AI, physics, scripts, animation state) | ≤ 9.0 ms |
| | Render prep (visibility, LOD select, DMA chain build, texture residency) | ≤ 5.0 ms |
| | Audio/pad/SIF housekeeping | ≤ 1.0 ms |
| | Reserve | 1.6 ms |
| **Render frame** (even) | Render prep (interpolated transforms) | ≤ 5.0 ms |
| | Decompression / streaming / sound bank uploads | ≤ 7.0 ms |
| | Checkpoint snapshot (when due), profiler | ≤ 2.0 ms |
| | Reserve | 2.6 ms |
| **Every frame** | VU1 geometry (runs in parallel with the EE) | ≤ 14 ms |
| | GS fill incl. texture uploads (parallel) | ≤ 14 ms |

If a tick overruns its budget the frame is held (one dropped frame), the event is
logged, and the dynamic LOD controller lowers draw distance / particle caps for the
next 2 s. R2 counts these.

### 5.2 Geometry and fill (per frame)
| Metric | Budget | Notes |
|---|---|---|
| Triangles sent to GS | **25 000** typical, 40 000 peak | Xbox Halo scenes ran ~30–60 k at 30 fps; LOD tables are re-tuned |
| VU1 batches (MSCAL) | ≤ 400 | Static geometry pre-baked as VIF packets |
| Skinned characters at LOD0 | ≤ 8 (≈ 1 500 tris each after re-LOD) | Halo's own LOD chain: 3 000 → 1 500 → 750 → ... |
| Texture upload (PATH3) | ≤ 1.5 MB | Texture window 2.25 MB, residency sorted by material |
| Overdraw equivalent | ≤ 6 full screens | Fill is not the constraint |
| Particles | ≤ 512 sprites | GS sprite primitives, expanded on VU1 |

### 5.3 Memory (EE RAM, 32 MB)
| Region | Size | Notes |
|---|---|---|
| Kernel reserve | 1.0 MB | `0x0`–`0x100000`, not ours |
| Code + static data | 2.5 MB | Measured by linker map; budget enforced in CI |
| Resident mission data (objects, weapons, bipeds, AI, scripts, animations, UI) | 9.0 MB | Converted tag space; all `.map` tags except BSP + raw media |
| Current BSP (render geometry as VIF packets, collision BSP, portals, pathfinding, decals) | 7.0 MB | Streamed at BSP transitions |
| Textures resident in RAM (GS-native, incl. lightmaps and CLUTs) | 6.0 MB | Shared BSP/object pool, LRU by material |
| Dynamic simulation (object pool, effects, particles, projectiles, rag/physics) | 2.5 MB | Fixed-size pools, no heap |
| DMA chains, VIF packets (double buffered) | 1.0 MB | |
| Streaming buffers (DVD ring, decompression scratch) | 1.5 MB | |
| Checkpoint snapshot (serialised sim state) | 0.5 MB | R6 |
| Audio staging (bank upload staging, stream headers) | 0.5 MB | |
| Scratch / per-frame arena | 0.5 MB | Reset every frame |
| **Total** | **32.0 MB** | Zero headroom on paper → Phase 1 must measure and rebalance |

VRAM (4 MB): see [01-ps2-architecture.md §3.5](01-ps2-architecture.md).
SPU2 RAM (2 MB): 1.5 MB sound banks (common + mission), 256 KB stream ring,
~200 KB reverb work area, remainder for the system. IOP RAM (2 MB): ~700 KB
modules, 1 MB read-ahead buffers.

### 5.4 Loading
| Event | Budget |
|---|---|
| Mission start (resident data + first BSP + banks) | ≤ 25 s |
| BSP transition | ≤ 4 s (Xbox also paused here) |
| Checkpoint revert | instant (RAM) |

## 6. Asset conversion specification (summary — details in [05-halo-ce-data.md](05-halo-ce-data.md))

| Xbox data | PS2 target |
|---|---|
| Bitmaps DXT1/3/5, swizzled, up to 512² | PSMT8 (+ CLUT) at **half resolution**, mip chain to 8×8; PSMT4 for UI/particles; PSMCT16 only for the few textures that palettise badly. Max 256². |
| Lightmaps (per-BSP RGB bitmaps) | Luminance PSMT8 atlases (alpha channel use) + low-frequency chroma baked into BSP vertex colours |
| `gbxmodel`/`model` compressed vertices, up to 2 weights | VU1-ready strips: s16 position, s8 normal, s16 UV, 2 bone indices + weight; pre-built VIF `UNPACK` headers |
| BSP render geometry | Per-cluster, per-material strips as baked VIF packets; per-cluster bounding volumes and portal lists |
| Collision BSP, pathfinding, markers, scenery placements | Compact binary, pointer-free, loaded in place |
| Animations (compressed keyframes) | Quantised 16-bit rotation (quaternion) / position tracks at the original 30 Hz |
| Sounds (Xbox ADPCM, 22/44 kHz) | PS2 ADPCM 22.05 kHz mono (stereo only for music); dialogue and music streamed |
| Scripts (compiled HSC node tree in `scenario`) | Same node tree, re-indexed; interpreted at runtime |
| Strings (UTF-16) | UTF-16 subset tables + bitmap fonts converted from `font` tags |
| Bink videos (intro/credits) | Dropped or re-encoded to `.pss` MPEG-2 for the IPU (intro + credits only, ≤ 300 MB) |

Each mission becomes one `.hp2` pack file with sections: `RES` (resident),
`BSPn` (one per structure BSP), `TEX`, `SND`, `STR`. Sections are LZ-compressed
on disc (EE decompresses at ~40 MB/s **(plan)**).

## 7. Input
DualShock 2 mapping modelled on Halo 2/MCC console defaults: left stick move,
right stick look, R1 fire, L1 grenade, R2 zoom, L2 melee-reserved, X jump,
Square action/reload, Circle crouch, Triangle swap weapon, L3 flashlight,
R3 melee, D-pad grenade select, Start pause, Select scoreboard-reserved. Look
acceleration and dead zones copied from the Xbox `globals` tag (player control
block).

## 8. Quality bar for "done"
A mission is done when, on a real console: it loads inside budget, is completable
on Legendary without softlocks, every scripted event fires, R2 holds across the
whole mission, memory report shows no region over budget, and the checkpoint save
round-trips through a memory card.
