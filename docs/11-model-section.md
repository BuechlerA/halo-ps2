# `MODL` and `OTEX` sections, version 1 (object models and their textures)

Output of `tools/halo2ps2/modelconv.py` (+ `objtex.py`), read by `engine/src/world/model.c` (contract:
`engine/src/world/model.h`) and, for `OTEX`, by `tex.c`. Both are sections of a [`.hp2` pack](07-hp2-format.md);
the converter writes them next to each other (`assets/hp2/<map>.model.hp2`, or inside the object pack built by
`objpack.py`). Little-endian, every table 16-byte aligned, all offsets relative to the start of the section.

The pack tags are `MODL` and `OTEX`. `OTEX` is a [`TEXS`](09-tex-section.md) blob byte for byte (the
signature inside stays `TEXS`, so `tex_open` reads it unchanged); the pack tag tells it from the BSP's texture
section. Its binding table is keyed by the **Xbox shader tag id** that model parts carry (`model_part.shader_id`).

## What goes in
`model` (group `mode`) tags. `build_sections(map_path, model_tags)` converts exactly the tags it is given (the
objects a scenario places), sorted by tag id; `None` converts every model of the map. Per model: skeleton, markers,
and the geometry **of every permutation at every LOD**, re-batched into VU1 packets. Extra levers, API only:
`convert_map(..., skip_lod0=True)` drops the super high LOD (permutations use their high geometry for LOD 0), and
`convert_map(..., variants={model_tag: {names}})` keeps only the permutations named in the set plus the first
permutation of every region (what `model_select_permutations` picks for a placement without a variant).

## Source facts (Xbox `model` tag, engine version 5)
The byte layout of every block is in the docstring of `modelconv.py`; the facts that shape the format:

| Fact | Status |
|---|---|
| Header 0xE8 bytes; reflexives at +0xAC markers, +0xB8 nodes, +0xC4 regions, +0xD0 geometries, +0xDC shaders; element sizes 0x40 (marker group) / 0x20 (instance) / 0x9C (node) / 0x4C (region) / 0x58 (permutation) / 0x30 (geometry) / 0x68 (part) / 0x20 (shader reference) | verified (the arrays follow each other with exactly these strides on every model of a10) |
| Vertices are **32 bytes**: f32 position, u32 normal (11:11:10 signed, x low), 2 unused u32 (binormal, tangent, always 0), s16 u v, u8 node0 x 3, u8 node1 x 3 (0xFD = none), s16 weight of node 0 | verified (normals have unit length to 0.3 %, positions form the expected meshes: Master Chief, Warthog, pistol, Elite and Pelican were rendered back from the converted section) |
| uv = s16 / 32767 x the model's base map scale (header +0x30/+0x34, up to 3) | verified by rendering the textured models (tyre treads, armour plates line up), unverified for tag-level uv outside [-1, 1] x scale |
| Vertex and index data live in the two index-header regions; every part reaches its vertex data through a 12-byte record (dword 1 = pointer) and its indices through a direct pointer; strips have `triangle count + 2` indices (stitched with degenerate triangles, no 0xFFFF restarts) | verified on all parts of a10 a30 b40 c10 (every pointer in range, every index below the vertex count, the vertex region size equals records + 32 x vertices) |
| Node numbers in vertices are **model** node indices (no per-part palette) | verified (all below the node count on four maps) |
| Strip triangle `k` is `(i[k], i[k+1], i[k+2])`, odd `k` swapped; the result winds like the BSP (front = clockwise about the outward normal) | verified (99.5 % of the triangles of one skinned part and 100 % of 16741 a10 BSP triangles have `(b - a) x (c - a)` opposite the vertex normal) |
| The five permutation geometry indices and the five cutoffs are stored **super low first**, super high last; cutoffs rise monotonically in that order | verified (314 models, 4 maps) |
| LOD cutoffs are projected sizes in pixels; which size exactly (diameter / radius / frame width) | **unverified**: `model_pick_lod` takes the bounding-sphere diameter in pixels of a 640-wide frame |
| Halo quaternions (nodes, markers) are the **conjugate** of the Hamilton quaternion `core/mat.c` expects | verified on all 76 a10 models: the chain of default poses, inverted, reproduces the tag's stored inverse bind pose (3x3 at node +0x6C, translation at +0x90) to 4e-6 |
| Node `scale` is 1.0 on every node (the stored inverse scale is 1.0) | verified (all four maps); the section stores `scale = 1.0` |

## Skinning: what the renderer needs
Vertex positions are in **model space at the bind pose**. The matrix to upload for palette node `n` is
`world(n, pose) * inverse(world(n, default pose))`; the default pose is the `model_node` translation/rotation chain
(unit scale), whose inverse is `R^T`, `-R^T t` per node (no stored inverse is needed). Rotations in `model_node` and
`model_marker` are **already in the Hamilton convention** (`-x, -y, -z, w` of the tag), so the standard
quaternion-to-matrix formula of `mat4_from_trs` applies. **Animation data uses the same Halo convention in the tag:
any converter that writes Halo quaternions to the engine must conjugate them the same way.**

## `MODL` layout
Header, 96 bytes:

| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `MODL` |
| 0x04 | u16, u16 | version (1), reserved |
| 0x08 | u32 | total section size |
| 0x0C | u32 | total triangles of all parts (statistics, checked) |
| 0x20 | (u32 offset, u32 count) x 7 | directory in the order models, nodes, markers, perms, parts, batches, vif (vif count = bytes) |

Records (`s` = signed):

| Table | Size | Fields |
|---|---|---|
| model | 80 | `u32 tag_id`, `u16 node_count, marker_count`, `u16 region_count, permutation_count`, `u32 first_node, first_marker, first_perm` (the header's `first_range`), `f32 centre[3], radius`, `f32 lod_cutoff[5]` (**engine LOD order: index 0 = super high**), `u32 triangles_lod0`, `u32 first_part, part_count`, 8 reserved |
| node | 72 | `char name[32]`, `s16 parent, first_child, next_sibling, pad`, `f32 translation[3]`, `f32 rotation[4]` (x y z w, Hamilton), `f32 scale` |
| marker | 68 | `char name[32]`, `u16 region, permutation`, `s16 node, pad`, `f32 translation[3]`, `f32 rotation[4]`. `permutation` is the model's **flat** permutation index; `0xFFFF` in either field = applies to every region / permutation |
| permutation (`ranges`) | 64 | `char name[32]`, `u16 region, flags` (bit 0: cannot be chosen randomly), `{u16 first_part, u16 part_count}[5]` per LOD (**index 0 = super high**), 8 reserved |
| part | 32 | `u32 shader_id`, `u16 first_batch, batch_count, triangles, uv_shift`, `s16 rigid_node`, `u16 flags` (1 alpha-tested, 2 blended), `f32 centre[3], radius` |
| batch | 40 | `u16 part, triangles`, `u8 palette_count`, 3 zero bytes, `u32 vif_offset, vif_qwords`, `u8 palette[24]` (zero beyond `palette_count`) |

Names are at most 31 characters plus NUL. A model's rows are contiguous in every table and the models follow each
other in tag-id order (`model_find` is a binary search). Parts of one model are listed geometry by geometry; the
permutation ranges point at them, so **permutations and LODs that use the same Xbox geometry share parts** (`first_part` is an
index into the whole part table). A geometry no permutation uses is not converted. `rigid_node >= 0`: every
vertex of the part follows that node at weight 1; `-1`: skinned (several nodes, or weights below 1).
A part's bounding sphere is in model space at the bind pose.

### The batch packet (`vif_offset`, 16-byte aligned, `vif_qwords * 16` bytes)
Exactly what `vu1/model.vsm` consumes, all TOPS-relative, then zero padding to a quadword:

| Command | Address | Contents |
|---|---|---|
| `UNPACK V4-32` (0x6C), 2 qw | 0 | qw0: `f32 centre.xyz, scale`; qw1: `s32 n = 3 x triangles`, 0, 0, 0 |
| `UNPACK V4-16` (0x6D), n | 4 | `s16 x y z` and the **palette slot of the first node** |
| `UNPACK V4-16`, n | 4 + 72 | `s16 s, t` (uv x 2^`uv_shift`), **slot of the second node**, **weight of the first node** (0..32767) |
| `UNPACK V4-8` (0x6E, signed: no 0x4000 flag), n | 4 + 144 | `s8 nx ny nz` (127 = 1.0), 0 |

Encodings: position = `centre + s16 * scale`, `scale` = the largest half extent of the batch's box / 32767 (rounded up in
f32), `centre` = the box centre; positions are model space. uv: `uv_shift` is the largest power of two (<= 14) for which
every uv of the part fits an s16, so the renderer multiplies by `1 / 2^uv_shift` (and by the binding's `uv_scale`). Normals are
unit length before rounding. A vertex keeps its **two strongest influences, strongest first**: slot 0 / weight `w`,
slot 1 / `32767 - w`; a rigid vertex has slot 1 = slot 0 and weight 32767. Palette slots index the batch's `palette[]` (model
node indices). A batch holds at most 24 triangles **and** at most 24 distinct nodes (the greedy splitter closes a batch early
when the next triangle would need more nodes). Triangles are expanded lists in the BSP's winding; the Xbox stitching
(degenerate triangles) and zero-area triangles are dropped.

## Permutations and LOD
`model_parts(perm, lod)`: `perm` is the flat permutation index (0..`permutation_count` - 1, region order), `lod` 0 = super
high ... 4 = super low. Every permutation has all five LODs (identical ranges where the tag reuses a geometry). A LOD
the tag leaves empty (never seen) is filled from the nearest more detailed one.
`model_select_permutations(variant)`: per region, the permutation whose name equals `variant` (exact, case-sensitive),
else the first of the region; a region without permutations gets `0xFFFF`. At most 64 regions per model (`model_open`
refuses more), so `uint16_t out[64]` always suffices.
`model_pick_lod(m, diameter_px)`: the smallest LOD index `l` with `diameter_px >= lod_cutoff[l]`, else 4 (NaN and
negative sizes give 4). The cutoffs are the tag's, reordered so that index 0 is super high, so the rule reads "use the most
detailed LOD whose cutoff the projected size reaches". Unit uncertainty: see the table above.

## What `model_open` checks
Signature and version, size equal to the section size, every table inside the section and 16-aligned, counts that fit the
u16 indices, name fields NUL-terminated, every float finite, nodes' links in range, markers' node / region / permutation in
range, permutation regions in order and below `region_count`, <= 64 nodes and regions per model, every permutation range inside
its model's parts, the models partitioning the node / marker / permutation / part / batch tables in order, every part's batch
range contiguous with matching triangle sums, `rigid_node` and every palette entry below the model's node count, and the VIF
data covered exactly in batch order, each packet verified word for word (commands, addresses, counts, zero padding, finite
constants). The accessors re-check their indices against the tables, so a forged `model_info` cannot read outside the buffer.
`engine/tests/test_model.c` truncates the section at every length, flips every byte of the header and tables three ways
(plus every third VIF byte) and walks whatever still opens under ASan/UBSan; the Python twin is `modelconv.parse_section`.

## `OTEX`
`objtex.py`. Shader groups used by model parts: `soso` (shader_model, 127..147 distinct shaders per campaign map), `sotr`,
`schi`, `senv`, `sgla`, `smet` (HUD meters), `spla`, `swat`. `sotr`/`schi`/`senv`/`sgla`/`swat` go through
`texconv.resolve_shader`. `soso` (layout in `objtex.py`): flags `u16` at +0x28 (bit 1 two-sided, bit 2 *not* alpha-tested, bit 3
alpha-blended decal, bit 5 no culling), map scale at +0x9C/+0xA0 (1.0 everywhere), base map at +0xA4. A soso whose bit 2 is
clear is an alpha-test shader (blend 1) **only when its base map really carries alpha** (most stay opaque: 4 + 5 + 1 of 127 a10 soso carry alpha), a decal is
blend 2. A shader without a decodable base map (`smet`, `spla`, a soso with no base map, cube or 3D bitmaps) is untextured
(flat grey, flag 8); objects draw those flat. Part `flags` follow the binding's blend (1 -> alpha-tested, >= 2 -> blended).
Detail, multipurpose (specular / colour-change mask), reflection and self-illumination maps are dropped (ADR-004/005), so
armour that Halo tints through the colour-change channel shows its grey base map.
Textures are converted once per (bitmap, alpha class) with texconv's quantiser, half resolution, power of two. **Budget:** the
side cap starts at 256 and halves (down to 32) until the *estimated* size (every texture as PSMT8 with its mip chain, an
upper bound) fits `--tex-budget` (default 3 MiB of the 6 MiB pool; BSP textures need <= 1.1 MiB, lightmaps <= 0.45 MiB).

## Measured sizes
`python3 -I tools/halo2ps2/modelconv.py assets/decompressed/<map>.map -o assets/hp2/<map>.model.hp2` converts **every model of the
map, all permutations and LODs** (most would never be placed; compare the placed sets below). Resident budget for everything the
mission keeps (objects, animations, AI, scripts, ...) is 9 MiB ([02-specs.md](02-specs.md) §5.3).

| map | models | parts | batches | source triangles | MODL MiB raw (packed) | MODL without super high | OTEX MiB (max side) | untextured parts | seconds |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 76 | 623 | 4791 | 106845 | 6.61 (3.28) | 4.86 | 1.64 (128) | 26 | 2.5 |
| a30 | 56 | 789 | 6176 | 138084 | 8.51 (4.15) | 5.60 | 1.58 (128) | 19 | 2.7 |
| a50 | 75 | 751 | 5139 | 113680 | 7.03 (3.49) | 5.00 | 1.73 (128) | 7 | 2.5 |
| b30 | 86 | 674 | 5606 | 125873 | 7.77 (3.89) | 5.72 | 1.90 (128) | 14 | 2.8 |
| b40 | 82 | 722 | 5681 | 127462 | 7.87 (4.05) | 5.38 | 1.96 (128) | 10 | 2.9 |
| c10 | 100 | 811 | 5707 | 126706 | 7.84 (3.86) | 5.57 | 1.61 (128) | 12 | 2.6 |
| c20 | 63 | 328 | 2921 | 66116 | 4.09 (2.09) | 2.91 | 2.31 (256) | 19 | 2.0 |
| c40 | 102 | 677 | 5924 | 134273 | 8.30 (4.33) | 5.71 | 2.04 (128) | 14 | 3.2 |
| d20 | 89 | 573 | 5146 | 116255 | 7.18 (3.82) | 5.09 | 1.78 (128) | 16 | 2.7 |
| d40 | 98 | 558 | 5065 | 114634 | 7.09 (3.62) | 5.12 | 1.84 (128) | 26 | 2.9 |

Expanded triangle lists cost about 62 bytes per triangle (3 vertices x 20 bytes + 48 bytes of packet header per 24-triangle
batch; an average batch holds 22 triangles). That is the price of the contract's one-VIF-packet-per-batch format: the whole-map
conversions above are within 9 MiB but leave nothing for everything else, so the converter is meant to run on the placed set:
with the models `scenconv.collect` found (a10 63, a30 34, b40 66, c10 73 models) MODL is 4.96 / 5.01 / 6.89 / 5.32 MiB, 3.68 /
3.09 / 4.47 / 3.67 MiB without the super high LOD, and 4.27 / 4.09 / 5.12 / 4.08 MiB (3.29 / 2.62 / 3.54 / 2.93 without
super high) when only the first permutation of every region is kept (`variants`, scenario variants are all empty today).

## Open points
* LOD cutoff unit (diameter vs radius vs screen share) is a guess until compared with the original's behaviour; `model_pick_lod`
  is one function and the cutoffs are the tag's.
* Detail / multipurpose / colour-change maps are not converted: team colours and specular masks need a second texture pass.
* Part `prev/next filthy` chains, the permutation marker blocks and the per-part centroid fields are ignored.
* The 0xFFFF strip restart does not occur in retail data and is treated as an error (index beyond the vertex count).
