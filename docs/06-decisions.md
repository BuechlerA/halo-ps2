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

## Open decisions
None.
