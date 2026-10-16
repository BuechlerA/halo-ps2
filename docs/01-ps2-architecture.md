# PlayStation 2 Architecture — Port-Relevant Research

This is the hardware model the whole project is designed against. Every number
here is either a published hardware figure or a conservative planning value that
Phase 0/1 must confirm on real hardware. Where a figure is a planning value, it
is marked **(plan)**.

## 1. Big picture

The PS2 is not a "weaker Xbox". It is a different machine:

| | Xbox (Halo's home) | PlayStation 2 |
|---|---|---|
| CPU | Intel Pentium III (Coppermine), 733 MHz, 128 KB L2, out-of-order x86 | Emotion Engine: MIPS R5900, 294.9 MHz, in-order 2-issue, **no L2**, 8 KB D-cache |
| Vector units | SSE on the CPU | **VU0** (4 KB code / 4 KB data) + **VU1** (16 KB code / 16 KB data), independent SIMD processors |
| GPU | NV2A: 2 vertex shaders, 4 pixel pipes, 4 texture units, register combiners, DXT | **Graphics Synthesizer**: 16 pixel pipes, fixed-function, **one texture per pass**, no texture compression, no shaders |
| Video RAM | Shared with main RAM (64 MB UMA) | **4 MB eDRAM** (framebuffers *and* textures must fit, textures streamed every frame) |
| Main RAM | 64 MB DDR, 6.4 GB/s | **32 MB RDRAM**, 3.2 GB/s |
| Sound | MCPX DSP, 64 voices + 3D | SPU2: 48 ADPCM voices, **2 MB** sound RAM, hardware reverb |
| I/O CPU | — | IOP: MIPS R3000A @ 36.9 MHz, 2 MB RAM (drives DVD, pads, memcard, SPU2) |
| Storage | 5x DVD, 8 GB HDD | 4x DVD (~5 MB/s), no HDD assumed |
| Output | 640×480 | 640×448i NTSC typical (480p possible with component cable) |
| Halo CE shipped at | 30 fps, 640×480 | **our target: 60 fps** |

So the port is **roughly half the CPU, half the RAM, a tenth of the texture
memory, no pixel shaders, and a 2× frame rate target**. That is why this is a
re-architecture, not a recompile. See [03-engine-architecture.md](03-engine-architecture.md).

## 2. Emotion Engine (EE)

### 2.1 Core
- MIPS R5900, 294.912 MHz, 64-bit integer regs, 128-bit multimedia (MMI) instructions
  working on the same 128-bit GPRs.
- 2-way superscalar, **in-order**, 6-stage pipeline. No branch target buffer worth
  relying on; branches are cheap but mispredicts are not free.
- Caches: 16 KB I-cache (2-way), **8 KB D-cache** (2-way, 64-byte lines), write-back.
  There is **no L2**. A cache miss to RDRAM costs on the order of 40+ cycles.
  → Data layout and streaming access patterns dominate EE performance. Random
  pointer chasing (classic C object graphs) is the enemy.
- **16 KB scratchpad (SPR)** at `0x70000000`: single-cycle on-chip RAM, DMA-able
  (DMAC channels `toSPR`/`fromSPR`). Used as a software-managed cache: stream
  working sets in with DMA, compute, stream out.
- FPU (COP1): single precision only, **not IEEE** (no NaN/Inf, no denormals). Fast
  enough for gameplay math. Doubles are software-emulated — never use `double`.
- COP0 `Count` register increments every cycle → free high-resolution timer for profiling.
- Memory: 32 MB RDRAM at `0x00000000`–`0x02000000`. The kernel reserves the first
  1 MB; programs load at `0x00100000`. Uncached view at `0x2xxxxxxx`, uncached
  accelerated at `0x3xxxxxxx`. Peak bandwidth 3.2 GB/s shared by EE, DMAC, GIF, IPU.

### 2.2 VU0 (vector unit 0)
- 4-wide float SIMD, 32 × 128-bit `VF` regs, 16 × 16-bit `VI` regs.
- **Macro mode**: acts as COP2 of the EE core; instructions are issued inline from
  EE code (`vmulax`, `vmadd`, `lqc2`/`sqc2`...). This is how matrix × vector, skinning
  palettes and collision math get vectorised without leaving C.
- **Micro mode**: runs its own 4 KB program on 4 KB data memory, in parallel with the
  EE. Small, but useful for batch jobs (animation blending, culling tests).
- No EFU (no fast `ESQRT`/`ERCPR`), but it has `DIV`/`SQRT`/`RSQRT` on the lower pipe.

### 2.3 VU1 (vector unit 1)
- Same ISA as VU0 plus the **EFU** (`EEXP`, `ESQRT`, `ERSQRT`, `ESIN`, ...), and
  `XGKICK`, which hands a block of VU1 data memory to the GS as a GIF packet.
- **16 KB micro memory** (program), **16 KB data memory**. Data memory is where the
  VIF unpacks vertex batches; the standard pattern is double buffering (`XTOP`/`XITOP`):
  the VIF fills buffer B while the microprogram transforms buffer A.
- Dual-issue: one upper (FMAC: `MUL/ADD/MADD...`) + one lower (`LQ/SQ/DIV/branch`)
  per cycle at 294.9 MHz → ~2.4 GFLOPS peak FMAC. Realistically VU1 transforms,
  lights and clips roughly **10–20 M vertices/s** with a tight microprogram.
- Classic pipeline hazards we must respect: `Q` and `P` registers have **no
  interlock** (use `WAITQ`/`WAITP`), integer register written immediately before a
  branch is read stale, FMAC results have 4-cycle latency (hardware stalls, costs time).
- **All geometry goes through VU1.** The EE never touches per-vertex data at runtime.

### 2.4 DMAC, VIF, GIF
- 10 DMA channels: `VIF0`, `VIF1`, `GIF`, `fromIPU`, `toIPU`, `SIF0`, `SIF1`, `SIF2`,
  `fromSPR`, `toSPR`. Transfers are in 16-byte quadwords; **source chain mode**
  with DMA tags (`cnt`, `next`, `ref`, `refe`, `call`, `ret`, `end`) lets the EE build a
  display list once and let hardware walk it.
- **VIF1** decodes VIF codes: `UNPACK` (expands packed vertex formats — e.g. 16-bit
  positions, 8-bit colours — into 128-bit VU1 data memory), `MSCAL` (run a VU1
  program), `MPG` (upload microcode), `FLUSH`, `DIRECT` (pass a GIF packet straight
  to the GS via PATH2).
- **GIF** has three paths into the GS: PATH1 (VU1 `XGKICK`), PATH2 (VIF1 `DIRECT`),
  PATH3 (GIF DMA channel). PATH3 runs **concurrently** with PATH1 — this is how
  textures are uploaded while VU1 keeps drawing.
- GIF tag formats: PACKED (one register per qword, easy), REGLIST (two 64-bit
  registers per qword, half the size), IMAGE (raw texture data).

### 2.5 IPU
- MPEG-2 macroblock decoder (IDCT, VLC decode, colour-space conversion) with its own
  DMA channels. Used for FMV (Sony's `.pss` container) and occasionally as a
  texture decompressor. Our use: cinematics only, if we keep any pre-rendered video.

## 3. Graphics Synthesizer (GS)

### 3.1 Numbers
- 147.456 MHz, 16 pixel pipelines (8 when texturing).
- **4 MB eDRAM** on a 2560-bit bus: 1024-bit write + 1024-bit read + 512-bit texture
  read, ~48 GB/s. This is why fill rate is enormous: **2.4 Gpixel/s** untextured,
  **1.2 Gpixel/s** textured (plan on ~50% of that with Z-test + blending).
- Peak 75 M tiny triangles/s. Irrelevant; VU1 is the triangle bottleneck.
- Internal precision: 12.4 fixed-point screen coordinates on a 4096×4096 virtual
  canvas; drawing offset (`XYOFFSET`) places the viewport at 2048,2048 so guard-band
  clipping is mostly free. Z is 16/24/32-bit integer; W (perspective `Q`) is float.

### 3.2 What the GS *cannot* do (and what Halo's renderer assumed)
| Halo CE (NV2A) feature | GS reality | Our answer |
|---|---|---|
| Multi-texturing (base × detail × lightmap in one pass) | One texture per primitive | Multi-pass with alpha blending; cut passes (see architecture) |
| Register-combiner "shaders" (`shader_environment`, `shader_model` specular/bump) | Fixed function: `DECAL`, `MODULATE`, `HIGHLIGHT`, `HIGHLIGHT2` | Bake to vertex colour + lightmap, drop bump/specular, keep reflection cube maps as sphere-mapped pass where cheap |
| DXT1/3/5 compressed textures (4–8 bpp) | **No compression**; formats PSMCT32/24/16, **PSMT8/PSMT4 palettised** | Offline requantise to 8-bit (and 4-bit) palettes with mip chains |
| Framebuffer multiply (`Cd × Cs`) | Blend = `(A−B)×C>>7 + D`, `C ∈ {As, Ad, FIX}` → RGB×RGB impossible | Luminance lightmap in alpha (`Cd×As`), chroma in vertex colour |
| Hardware texture memory = 64 MB UMA | ~2 MB texture window inside 4 MB eDRAM | Per-frame texture streaming via PATH3 |
| Stencil, dest alpha tricks | 1-bit dest alpha in 16-bit modes, `ATE`/`DATE` tests | Design shadows (Halo uses blob shadows + lightmaps) without stencil |

### 3.3 What the GS does well
- Fill. Overdraw, large alpha-blended particles, full-screen passes are cheap.
- Alpha test, Z test, fog (per-vertex `F` → `FOGCOL`), dithering, scissor, 6 mip levels
  with bilinear/trilinear, per-pixel LOD (`K`/`L`), 8-bit CLUT swapping (one 1 KB
  CLUT upload per texture or shared palettes).
- 16-bit framebuffer with dithering looks acceptable on CRT/480i output and halves
  framebuffer memory and bandwidth.

### 3.4 Video output
- NTSC 59.94 Hz interlaced. Framebuffer widths that fit page layouts well: 512 and 640.
  Typical 60 fps PS2 games render **512×448** (or 640×448) into a full-height
  buffer every vblank and let the CRTC read alternate lines per field (GS
  `SMODE2.FFMD = 0`, called "field" read mode in the GS manual and in ps2sdk's
  `GRAPH_MODE_FIELD`), so effective motion resolution is 60 fps. Some games halve height ("field
  rendering", 512×224 per field) to reach 60; it looks noticeably worse and we do
  not plan on it unless Phase 1 forces it.
- 480p via component cable is possible (`GRAPH_MODE_HDTV_480P`) at identical cost.

### 3.5 VRAM budget at our chosen mode
| Buffer | Format | Size |
|---|---|---|
| Front colour 512×448 | PSMCT16S | 448 KB |
| Back colour 512×448 | PSMCT16S | 448 KB |
| Z 512×448 | PSMZ24 (stored in 32-bit words) | 896 KB |
| **Texture window** | PSMT8 / PSMT4 / PSMCT16 + CLUTs | **~2.25 MB** |

PSMZ16S instead of Z24 would free another 448 KB but Halo's outdoor draw distances
(Silent Cartographer, Assault on the Control Room) need the Z precision. Decision
recorded in [06-decisions.md](06-decisions.md).

## 4. IOP (I/O processor)
- MIPS R3000A (the PS1 CPU), 36.864 MHz, 2 MB RAM, runs Sony's `IRX` module system.
- Owns: CD/DVD drive (`cdvdman`, streaming reads), DualShock 2 (`padman`/`sio2man`),
  memory cards (`mcman`), USB, and **SPU2 DMA** (`libsd`). Talks to the EE over SIF
  RPC (shared bus DMA).
- Too slow for heavy decompression (~5–10 MB/s for LZ-class codecs **(plan)**), so
  it reads raw compressed chunks from disc into EE RAM and the EE decompresses.
- Our IOP modules: a streaming server (read-ahead ring for level chunks, dialogue,
  music) and a sound driver (voice allocation, ADPCM uploads, reverb config).

## 5. SPU2
- 2 cores × 24 voices = **48 hardware ADPCM voices**, 48 kHz mixing, per-voice pitch,
  ADSR envelope, volume; hardware reverb per core (needs a work area in sound RAM).
- **2 MB sound RAM.** PS2 ADPCM ("VAG") is 4-bit: 16 bytes encode 28 samples
  (~3.5:1). Xbox Halo also used a 4-bit ADPCM, so bank sizes translate ~1:1.
- Streaming from DVD into a ring buffer in sound RAM handles dialogue and music.

## 6. DVD
- 4× DVD ≈ 5.5 MB/s outer edge, ~3.5 MB/s inner **(plan: 4 MB/s average)**;
  seeks 100–300 ms. Single-layer 4.37 GiB, dual-layer 7.95 GiB.
- Consequences: level loads of ~30 MB resident data ≈ 8 s plus seeks; BSP
  transitions should load ≤ 8 MB (≈ 2–3 s). Disc layout must be seek-aware
  (per-mission contiguous chunks).

## 7. Toolchain and test loop (as installed on this machine)
- `ps2dev` toolchain at `~/ps2dev`: `mips64r5900el-ps2-elf-gcc` **15.2.0** (EE),
  `mipsel-none-elf-gcc` (IOP), `dvp-as` (VU0/VU1 microcode assembler), `ps2sdk`
  (libdma, libpacket2, libdraw, libgraph, libmath3d, libpad, libaudsrv, libcdvd,
  libmc...), `gsKit`, `ps2client` (network loader for a real PS2 running ps2link).
- No VCL (Sony's VU scheduling preprocessor) is installed; we write `.vsm` by hand
  or add OpenVCL later. Hand-scheduled VU code is fine for the small number of
  microprograms we need.
- Emulator: PCSX2 (`/Applications/PCSX2.app`, BIOS configured) with EE/VU/GS
  debugger. **PCSX2 is forgiving** about DMA timing, cache behaviour and VU hazards;
  milestones are gated on the PCSX2 interpreter and re-checked on a real console in
  Phase 6 (ADR-022; FreeMcBoot + ps2link over the network adapter, or an ELF launched
  from USB via wLaunchELF). PCSX2 does not model GS fill or DMA contention.
- Host tools for the asset pipeline run on macOS (Python 3 + a C/C++ converter).

## 8. Reference reading
- ps2dev wiki / ps2sdk headers in `~/ps2dev/ps2sdk/ee/include` (authoritative for API).
- Sony EE User's Manual, EE Core Instruction Set, VU User's Manual, GS User's Manual
  (widely mirrored; the register-level truth for GIF tags, GS registers, VIF codes).
- PlayStation Developer Wiki, *Graphics Synthesizer* page (fill-rate derivations).
- `~/ps2dev/ps2sdk/samples/draw/vu1` — the double-buffered VU1 pattern this
  skeleton copies.
- Tyra (h4570) — open-source C++ PS2 engine, good reference for VU1 renderer and
  clipping structure.
