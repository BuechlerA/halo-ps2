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

## Open decisions (to be closed in Phase 0/1)
- OD-1 Parser backend for Xbox tags: Reclaimer vs. hand-written (Phase 0).
- OD-2 On-disc compression codec: LZ4-style vs. custom (needs EE decode speed test).
- OD-3 Keep Bink intro/credits as `.pss` or drop (disc space, IPU work).
- OD-4 Engine licence.
