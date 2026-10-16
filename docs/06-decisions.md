# Architecture Decision Records

Short, numbered, append-only. Reverse a decision by adding a new record.

## ADR-001 Clean-room C engine driven by converted data (2026-10-16)
**Decision**: Reimplement Blam gameplay in C11 on the EE; convert the user's
Xbox cache files offline. No decompilation/recompilation of `default.xbe`.
**Why**: x86→MIPS recompilation cannot reach 60 fps with half the CPU and a
different GPU model; the data carries the game. **Consequence**: AI and
scripting semantics must be reverse-engineered behaviourally (risk #4).

## ADR-002 30 Hz simulation tick, 60 Hz presentation with interpolation
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

## Open decisions
None.
