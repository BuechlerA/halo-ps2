# `TEXS` and `LMn` sections, version 1 (PS2-ready textures and lightmaps)

Output of `tools/halo2ps2/texconv.py`, read by `engine/src/world/tex.c`. They are stored as
sections of a [`.hp2` pack](07-hp2-format.md); the first converter writes a separate pack
(`assets/hp2/<map>.tex.hp2`) and the engine loads it next to the BSP pack. Section tags are
`TEXS` (one per map) and `LM0`..`LM12` (one per BSP that has a lightmap bitmap; a BSP without
one has no `LMn` section and is drawn without the lightmap pass). Little-endian, every table
and blob 16-byte aligned, all offsets relative to the start of the section.

## What goes in
The textures the structure BSPs need: for every shader tag id used by a BSP lightmap material
(`shader_id` in the [`BSPS` material record](08-bsp-section.md)) the base map is converted once
per (bitmap, alpha class) and bound to the shader id. Object, sky, particle and UI textures come
later and reuse the same texture record. Detail, bump, self-illumination and reflection maps are
dropped (ADR-004/005: no bump, no specular, no detail on PS2).

### Source facts (Xbox, engine version 5)
Bitmap tag and pixel layouts are documented in the docstring of `tools/halo2ps2/bitmaps.py`,
shader layouts in `texconv.py`. Verified on the shaders used by BSP materials of all ten campaign
maps (a10 a30 a50 b30 b40 c10 c20 c40 d20 d40):

| Fact | Status |
|---|---|
| `bitmap_data` is 48 bytes at `bitm+0x60`; width/height/format/flags/mips/pixel offset+size as in `inventory.py`; pixel offset is an absolute map-file offset | verified |
| Mip chain is stored largest first, level 0 has exactly `w*h*bpp` bytes (DXT: 4x4 blocks), level 1 follows it; smaller levels are padded in a way that is not modelled, so only levels 0 and 1 are addressed | verified (sizes on every a10 image; mip 1 equals the box-filtered mip 0 to RMS < 5 on every swizzled colour format of a10, except one r5g6b5 image at 11 and the p8-bump normal maps) |
| Uncompressed formats are Morton-swizzled (x in the even bits, y in the odd bits, the longer side continues linearly); DXT is not swizzled | verified |
| Lightmap pages are R5G6B5, swizzled, 8..512 px, no mips; the bitmap tag is the tag reference at `structure_bsp+0x00` (tag id at +0x0C, `0xFFFFFFFF` = none); the lightmap record's first `u16` is the page index; stored uv map straight to texel rows (v = 0 is the first row) | verified (no vertex of a10 BSP 1 lands on the atlas fill colour; with v flipped 25 % do) |
| senv: flags `u16` at +0x28 (bit 0 alpha-tested), type `u16` at +0x2A, base map reference at +0x88; there is no base map uv scale | verified (every alpha-tested shader is a grate / alpha test / canopy / trees / moss material, every blended-type one is terrain or cliff) |
| sotr / schi: counter `u8` +0x28, flags `u8` +0x29 (bit 0 alpha-test, bit 1 decal, bit 2 two-sided), framebuffer blend `u16` +0x2C, maps reflexive at +0x54, map element 100 B (sotr) or 220 B (schi); bitmap reference at element +0x1C / +0x6C, uv scale at +0x04,+0x08 / +0x54,+0x58 | verified for flags, blend (all light shaders add, rivers alpha) and bitmap references; uv scales plausible (one retail shader has 8 x 8), schi scale offsets only checked on sky shaders |
| schi map: colour function `u16` +0x2C and alpha function `u16` +0x2E of each map say how the **next** map combines with it (0 current, 1 next map, 2 multiply, 3 double multiply, 4 add, ...). The PS2 base map is the least-tiled map that is not a pure colour modulator (multiply / double multiply with alpha function 0 or 1): `chicago_base_map`, ADR-058 | verified on every multi-map schi of a10 a30 b40 c10 d40 (space skies: mask x3 then stars; a30 sky dusk: mask, twinkle, stars, then the gradient; horizon land: detail tile then painting; rivers: alpha mask then water) |
| sgla: diffuse scale +0x154, diffuse map +0x158; swat: base map +0x4C | verified (names), swat has no scale |
| `scex`, `smet`, `spla`, `soso` and every other group | not used by BSP materials; resolve to "untextured" |
| a8 decodes as white with the stored alpha; p8 / p8-bump as grey | unverified (no BSP base map uses them) |
| Alpha-tested senv textures keep their solid texels as low as alpha ~80 (holes at 0) | measured; hence the Otsu punch-through threshold (16..128) instead of a fixed 128 |

## `TEXS` layout
Header, 64 bytes:

| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `TEXS` |
| 0x04 | u16, u16 | version (1), reserved |
| 0x08 | u32 | texture count (< 0xFFFF) |
| 0x0C | u32 | binding count (<= 0xFFFF) |
| 0x10 | u32 | texture table offset (64) |
| 0x14 | u32 | binding table offset |
| 0x18 | u32 | data offset (first CLUT) |
| 0x1C | u32 | total section size |
| 0x20 | u32 | number of untextured bindings (statistics) |
| 0x24 | u8[28] | reserved |

**Texture record, 32 bytes**, in table order (index = binding `texture`):

| Offset | Type | Field |
|---|---|---|
| 0x00 | u32 | source Xbox bitmap tag id (debug) |
| 0x04 | u16, u16 | width, height of mip 0: powers of two, 8..256 |
| 0x08 | u8 | `psm`: `0x13` PSMT8, `0x14` PSMT4 (GS PSM codes) |
| 0x09 | u8 | mip count, 1..4 |
| 0x0A | u16 | flags: bit 0 punch-through, bit 1 blend, bit 2 upscaled from < 8 px |
| 0x0C | u32 | CLUT offset (16-aligned) |
| 0x10 | u32 | pixel offset (16-aligned, after the CLUT) |
| 0x14 | u32 | pixel bytes of all mips |
| 0x18 | u16, u16 | CLUT entries (256 for PSMT8, 16 for PSMT4), reserved |
| 0x1C | u32 | average colour RGBA8, alpha on the GS 0x80 scale (flat colour for far LOD) |

Per texture the data is `CLUT` then the mips. **Mip chain**: level 0 is `width x height`; each next
level halves both sides while the smaller side is still > 8 and fewer than 4 levels exist, so a
256 x 256 texture has 256/128/64/32, a 64 x 64 one 64/32/16/8, a 128 x 8 one a single level.
Level size is `w*h` bytes (PSMT8) or `w*h/2` (PSMT4) rounded up to 16; levels follow each other
largest first. Pixels are raster order (the GS does its own block swizzle on upload); PSMT4 packs
two texels per byte, the even texel in the **low** nibble. `pixel bytes` must equal the sum of
the level sizes; the reader checks it.

**CLUT blob**: PSMCT32 words `R | G<<8 | B<<16 | A<<24`, alpha on the GS scale (0x80 = 1.0,
Xbox alpha `a` becomes `(a*128+127)/255`). It is ready to upload as it is, as a 16 x 16 PSMCT32
image (PSMT8) or an 8 x 2 image (PSMT4), with `CSM1`. For PSMT8 the GS reads index `i` from the
**stored slot `swap(i)`**, where `swap` exchanges bits 3 and 4 (entries 8..15 trade places with
16..23 in every block of 32; `swap(swap(i)) = i`). The converter therefore stores
`blob[p] = palette[swap(p)]`, so palette index `i` sits at slot `swap(i)`. PSMT4 CLUTs are stored
straight (no swap; 16 entries, upload at `CSA = 0`). *Status: standard CSM1 behaviour, to be
confirmed on the console (self test, 2026-10-16).* Unused entries repeat the last colour.

Alpha classes (chosen per texture from the shader's allowance and the pixels):
* **opaque**: alpha dropped, all entries alpha 0x80;
* **punch** (alpha-tested shaders, or binary alpha): palette entry 0 is the transparent texel
  (alpha 0, RGB = mean of the opaque colours so bilinear filtering does not fringe), the other
  entries are opaque; threshold per texture (`alpha_threshold`); mips are alpha-weighted;
* **blend**: alpha takes part in the palette (RGB 5/6/5 bits + alpha 4 bits are clustered together).
A shader's `keep_alpha` decides what is allowed: senv without the alpha-test flag drops alpha
(it is a specular/bump mask there); sotr/schi/sgla keep it; swat drops it.

Quantiser: 5/6/5-bit histogram, deterministic median cut (`texconv.Quantiser`), palette entries are
population-weighted box means with bit-replicated channels (white stays white). Lower mips are
classified through the recorded cuts. PSMT4 is selected automatically when the 16-colour palette
reproduces the base image within an RMS of 3.0 (0..255 units; `PSMT4_MAX_RMS`), which in the
retail data happens for flat, low-detail textures. No dithering.

**Binding record, 32 bytes**, sorted by shader id (strictly ascending, binary search):

| Offset | Type | Field |
|---|---|---|
| 0x00 | u32 | shader tag id (the `shader_id` of a BSP material) |
| 0x04 | u16 | texture index, `0xFFFF` = untextured (draw flat) |
| 0x06 | u16 | flags: 1 alpha-test, 2 two-sided, 4 decal, 8 untextured, 0x10 sky (reserved), 0x20 senv blended-type base |
| 0x08 | u8 | blend: 0 opaque, 1 alpha test, 2 alpha blend, 3 add, 4 multiply, 5 double multiply, 6 subtract |
| 0x09 | u8 | shader group: 1 senv, 2 sotr, 3 schi, 4 sgla, 5 swat, 0 other |
| 0x0A | u16 | reserved |
| 0x0C | f32, f32 | uv scale u, v (multiply the baked texture uv; 1.0 unless the shader tiles its first map) |
| 0x14 | u32 | flat colour RGBA8: the texture's average, or grey `0x80808080` when untextured |
| 0x18 | u8[8] | reserved |

Framebuffer blend functions 5 and 6 (component min/max) map to alpha blend, 7 (alpha-multiply
add) to add. Water (swat) and glass (sgla) are alpha blended.

Untextured fallback ("flat grey"): shaders with no base map (e.g. self-illumination only), cube
or 3D base maps (cryo glass, monitor glass), unknown tag ids and unsupported shader groups.
The engine draws an untextured **opaque** material flat (vertex colour, lightmap on top) and
**skips** an untextured **blended** one (ADR-057): with no base map an additive or alpha-blended
material can only add a flat grey over what is behind it.

What stays flat on a10 and why (the 14 of 55 materials of BSP 7, the exterior, that the
playtest review F10 counted; the other 41 are `senv` hull and panel shaders with DXT base maps,
converted as usual, see `resolve_shader`):

| shader | group, flags | materials (BSP 7 / all a10) | base map | why it stays flat |
|---|---|---|---|---|
| `levels\a10\shaders\cryo glass` | sotr, two-sided, add | 11 / 41 | `cube map glass` (cube, 64 px) | cube-map reflection only (ADR-004: no environment maps); skipped by the engine, the tube/window shows what is behind it |
| `levels\a10\shaders\monitor glass` | sotr, two-sided, alpha | 2 / 4 | `cube map pillar controls` (cube) | same |
| `levels\a10\shaders\monitor glow` | senv, opaque | 1 / 2 | none (self-illumination map only) | no base map; drawn flat grey under its lightmap |

The playtest's "flat grey blob" was not these materials: the exterior's hull shaders were bound
and uploaded all along, and the blob was the clip-matrix corruption of ADR-057.

## `LMn` layout (lightmap pages of BSP n)
Header, 64 bytes: `char[4] "LMAP"`, `u16 version (1)`, `u16 reserved`, `u32 page slots`,
`u32 page table offset (64)`, `u32 CLUT offset`, `u32 data offset`, `u32 total size`,
`f32 gain` (luminance gain the converter applied, default 1.0), `u32 pages in use`, 28 reserved.
The page table has one 32-byte slot per BSP lightmap page number (the `lightmap_page` of a BSP
material); width 0 marks a page nothing references:

| Offset | Type | Field |
|---|---|---|
| 0x00 | u16, u16 | width, height of the stored page (powers of two) |
| 0x04 | u8 | `psm`, always `0x13` (PSMT8) |
| 0x06 | u16 | flags (0) |
| 0x08 | u32, u32 | pixel offset (16-aligned), pixel bytes (`w*h` rounded up to 16) |
| 0x10 | u16, u16 | source (Xbox) width, height |
| 0x14 | u32 | mean page colour RGBA8 (alpha 0xFF), for a flat fallback |
| 0x18 | u32 | mean luminance 0..255 |
| 0x1C | u32 | reserved |

After the table comes **one shared CLUT** (1024 bytes, CSM1 order, entry `i` = `(i, i, i, i)`: the
alpha equals the index), then the pages.

Pages are **luminance only** (Rec.601 luma of the RGB565 texel, x gain, clamped), half the Xbox
size in each direction (min 8; lightmaps are low-frequency), raster order. Intended blend
(ADR-004 pass 2): draw the page as a PSMT8 texture with the grey-ramp CLUT, `TFX = MODULATE`/
`TCC = RGBA`, GS blend `Cd = Cd * As` (`A = Cd, B = zero, C = As, D = zero`, FIX unused). GS alpha
0x80 is 1.0 and the ramp goes up to 0xFF, so a luminance index of 0x80 leaves the base texture
unchanged and 0xFF doubles it (overbright headroom). Whether Halo's lightmap range maps 1.0 to
0x80 or 0xFF is **unverified**: it is a single constant (`--lm-gain`, header `gain`) to tune when
the first BSP renders. A page may be a luminance-only fallback of a flat `avg_rgba` surface.

**Chroma** is not in the page: `texconv.vertex_chroma(page_rgba, lm_uv)` returns the page colour
at a lightmap uv (bilinear, clamped) divided by its luminance, scaled so 0x80 = 1.0 and clamped to
0..255 (neutral `(128,128,128)` for black). The geometry converter bakes it into the vertex colour
of pass 1 (`Rgba` pages from `lightmap_pages_rgba(map, bsp_info)`); with the base texture modulate
of pass 1 and the luminance of pass 2 this gives `base * chroma * luminance`.

Page size budget: the converted pages of one BSP must fit `LM_BUDGET_BYTES` (512 KiB, one BSP is
resident at a time); if half resolution does not fit, every page is halved again (up to three
times) and the shrink is reported.

## Measured sizes (all ten campaign maps)
`python3 -I tools/halo2ps2/texconv.py assets/decompressed/<map>.map -o <out>.tex.hp2`.
Texture pool of [02-specs.md §5.3](02-specs.md) = 6.0 MiB (BSP + object textures + lightmaps);
"tex" is pixels plus CLUTs of the textures the BSPs need, "LM" the largest single BSP.

| map | BSPs (with LM) | shaders | textures (PSMT4) | tex KiB | untextured shaders / materials | LM total KiB | LM largest BSP KiB | seconds |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 9 (9) | 47 | 31 (21) | 847 | 3 / 47 of 1866 | 1360 | 188 | 1.7 |
| a30 | 2 (2) | 44 | 32 (12) | 900 | 0 / 0 of 278 | 509 | 309 | 1.1 |
| a50 | 4 (4) | 33 | 19 (6) | 598 | 1 / 1 of 916 | 790 | 252 | 0.9 |
| b30 | 2 (2) | 30 | 26 (12) | 810 | 0 / 0 of 294 | 461 | 249 | 1.1 |
| b40 | 13 (10) | 75 | 37 (21) | 759 | 0 / 0 of 1449 | 1883 | 392 | 1.9 |
| c10 | 6 (6) | 59 | 24 (10) | 780 | 0 / 0 of 2099 | 1304 | 354 (521 before the shrink of BSP 1) | 1.9 |
| c20 | 4 (4) | 47 | 16 (11) | 273 | 0 / 0 of 1701 | 621 | 174 (697 before the shrink of all four) | 2.5 |
| c40 | 13 (11) | 75 | 36 (21) | 737 | 0 / 0 of 1465 | 1780 | 416 | 1.8 |
| d20 | 5 (5) | 35 | 20 (6) | 609 | 2 / 4 of 1184 | 924 | 282 | 1.0 |
| d40 | 10 (9) | 61 | 44 (32) | 1037 | 3 / 21 of 1994 | 1787 | 437 | 2.2 |

Everything is **well inside the budget**: BSP textures need at most 1.0 MiB and one BSP's
lightmaps at most 0.44 MiB, i.e. under a quarter of the 6.0 MiB pool, leaving 4.5 MiB for object
textures and the LRU slack. The only over-budget case at the converter's default was c20
(697 KiB for its largest BSP at half resolution), fixed by quarter resolution lightmaps. The
bigger risks are VRAM, not RAM: a 256 x 256 PSMT8 texture plus CLUT is 65 KiB, so the 2.25 MiB
texture window holds ~35 of them. Sizes are small because base maps only are converted (no
detail/bump/illumination maps) and because Halo's BSP textures are mostly desaturated.

## Reader
`engine/src/world/tex.h` (`tex_open`, `tex_get`, `tex_mip`, `tex_clut`, `tex_find_binding`,
`lm_open`, `lm_get_page`). `tex_open` / `lm_open` validate every table (sizes, pow2 dimensions,
mip rule, CLUT and pixel ranges, sorted unique bindings, texture indices) so accessors cannot read
outside the buffer. `engine/tests/test_tex.c` builds on the pack written by
`tools/tests/test_texconv.py`, then corrupts every byte of every table and truncates the sections
at every length under ASan/UBSan. The Python twin is `texconv.parse_texs` / `parse_lmap`.

## Open points
* Lightmap gain (0x80 vs 0xFF for 1.0) needs the Phase 6 console pass; the PSMT8 CLUT swap is confirmed on the console.
* The a10 bridge (BSP 1) was reported "far darker than the Xbox" (playtest review F11) on frames that ADR-057 has since
  shown were drawn with a corrupted projection, so the observation is void. The data does not single the bridge out
  either (2026-10-18, lightmap sampled at every baked vertex, gain 1.5): raw luma mean 72 on BSP 1 against 49 in the
  cryo bay (BSP 0), 64 in BSP 4 and 85 in the hangar; brightest channel / luma 1.10 with no vertex beyond the 2.0 chroma
  clamp (BSP 4: 4.4 %), 0 % blue-dominant texels, reconstructed brightness 1.01 of the ideal. The post-fix bridge dumps
  average 32 (frame 1500, Keyes against the dark console wall) and 92 (frame 2400, the lit ceiling). The global gain
  stays 1.5; a xemu capture of the same frames (docs/35) is the next step if the bridge still reads dark on screen.
* Detail maps could return as a second pass if fill allows; `senv` keeps their tags at +0xB8,
  +0xCC, +0xFC with scales at +0xB4, +0xC8, +0xF8.
* Sprite sheets, cube maps and 3D bitmaps (particles, sky, reflections) are skipped by the
  decoder (`BitmapImage.decodable`) and handled with the object/effect textures.
* Texture dedupe across shaders is by (bitmap, alpha class); CLUT sharing between textures is not
  attempted.
