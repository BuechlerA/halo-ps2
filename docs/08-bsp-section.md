# `BSPn` section format, version 1 (static world geometry)

Output of `tools/halo2ps2/bspconv.py`; stored as a `BSP0`..`BSP9` section of a
[`.hp2` pack](07-hp2-format.md). Little-endian, every table and packet 16-byte aligned.
This version bakes **per-material triangle batches**; per-cluster grouping, portal
lists, collision and lightmap atlases come later (they need the cluster and
subcluster layouts, which are not verified yet).

## Source data (Xbox, verified on all campaign maps)
Each lightmap holds materials; a material owns `surface_count` consecutive 6-byte
surfaces (three u16 indices **local to that material's vertex arrays**) and a
compressed vertex blob: `rendered_count` × 32 bytes, then `lightmap_count` × 8 bytes.
Rendered vertex: `f32 pos[3]`, `u32 normal`, `u32 binormal`, `u32 tangent`, `f32 uv[2]`.
Lightmap vertex: `u32 normal`, `s16 uv[2]` (value / 32767). Normals/tangents are
dropped: world surfaces are lit only by the lightmap.

## Section layout
| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `BSPS` |
| 0x04 | u16, u16 | version (1), reserved |
| 0x08 | u32 | material count |
| 0x0C | u32 | batch count |
| 0x10 | u32 | triangle count (sum over batches) |
| 0x14 | u32 | offset of the material table |
| 0x18 | u32 | offset of the batch table |
| 0x1C | u32 | offset of the VIF data |
| 0x20 | f32[6] | world bounds min xyz, max xyz |
| 0x38 | u32 | total section size |
| 0x3C | u32 | reserved |

Header is 64 bytes. **Material record, 32 bytes**:

| Offset | Type | Field |
|---|---|---|
| 0x00 | u32 | shader tag id (resolved to a PS2 material by the RES section later) |
| 0x04 | u16 | lightmap page (index into the BSP's lightmap bitmap, 0xFFFF = none) |
| 0x06 | u16 | first batch of this material in the batch table |
| 0x08 | u16 | batch count |
| 0x0A | u16 | uv shift: texture uv stored as s16 = uv × 2^shift (clamped; see below) |
| 0x0C | u32 | triangle count |
| 0x10 | u8[16] | reserved |

**Batch record, 32 bytes**: `u16 material`, `u16 triangles` (≤ 32), `u32 vif offset`
(from the start of the VIF data, 16-aligned), `u32 vif quadwords`, `f32 centre[3]`,
`f32 radius`, `u32 reserved`. Centre (mean of the batch's vertices) and radius (largest
distance from it) bound the batch for frustum culling and also define its position
encoding: `q = round((p - centre) / (radius / 32767))` as s16, decoded as
`p = centre + q × radius / 32767`. The error is therefore at most `radius / 65534` per
axis (about 0.1 mm for a 3-unit batch), independent of how large the map is; VU1 applies
`centre` and `radius / 32767` from the batch header (one multiply-add per vertex).

Texture uv shift is the largest power of two that fits every |uv| ≤ 64 of the material
into an s16; a few retail vertices carry garbage uv (|u| > 10^6) and clamp.

## VIF packet per batch
Triangles are expanded (3 vertices each, no strips yet): `n = 3 × triangles ≤ 96`.
```
UNPACK V4-16 signed, TOPS-relative, addr 4,       num n   x y z 0      (s16 ×4, w = 0)
UNPACK V4-16 signed, TOPS-relative, addr 4 + 96,  num n   s t u v      (s16 ×4: texture st, lightmap uv)
NOP padding to a quadword
```
The engine appends `MSCAL`. Lightmap uv are s16 = uv × 32767; texture uv use the
material's shift. VU1 memory addresses 0..3 are reserved for the GIF header and
per-draw constants, which are written by the engine's `cnt` packet.
