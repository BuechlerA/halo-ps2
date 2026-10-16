# `BSPn` section format, version 2 (static world geometry + visibility)

Output of `tools/halo2ps2/bspconv.py`; stored as a section of a [`.hp2` pack](07-hp2-format.md),
read by `engine/src/world/bsp.c`. Little-endian, every table and packet 16-byte aligned.
Version 2 carries what Phase 1 needs from `structure_bsp`: per-cluster VU1 batches, the
cluster/portal graph with portal polygons, and the BSP tree for point location. Collision,
lightmap atlases and textures are separate steps (the `TEXS` section is another tool's output).

**Version 1 is gone.** `bsp_open` rejects any section whose version field is not 2 with
`BSP_ERR_VERSION` (`BSP_ERR_MAGIC` for a bad signature); `parse_section` in the converter does the
same. There is no v1 compatibility path: regenerate packs with the converter.

**Section tags.** `BSP0`..`BSP9`, then `BSPA`..`BSPZ`: b40 and c40 have 13 BSPs, more than ten digits
(docs/07 should say so). The index is the scenario's BSP index.

## Source data (Xbox, verified)
The Xbox `structure_bsp` layout, every field below, is documented with its verification in the
docstring of `tools/halo2ps2/bsp.py` and was checked on all 69 BSPs of all 11 maps (10 campaign +
`ui`): every table lies inside its block, every index is in range, every surface is reachable
except the few listed below. The chain that matters:

* **Tree.** `collision bsp` (+0xB0, one element) holds the bsp3d nodes (12 B: plane, back child,
  front child; child `0xFFFFFFFF` none, bit 31 leaf) and the planes (16 B: nx ny nz d, `n.p = d`;
  front is `n.p - d >= 0`). Root is node 0, children always come later, no node has two parents.
  The render `leaves` (+0xE0, 16 B) have the same count and order as the collision leaves.
* **Leaf -> cluster.** Leaf +0x08 u16 cluster, +0x0A u16 surface count, +0x0C **u32** first index into
  the leaf surface table (+0xEC, 8 B: u32 surface index, u32 unused). The first index needs 32 bits
  (a10b has 78052 leaf surfaces).
* **Clusters** (+0x134, **104 B**; the lens flare markers at +0x128 were mistaken for clusters in the
  first version of `bsp.py`): sky, fog, background sound, sound environment, weather as s16 at +0x00..,
  subclusters (+0x34, 36 B: bounds, u32 surface list), portal list (+0x5C, s16 indices). Cluster
  surface indices (+0x44) are always empty on Xbox; the surfaces are reached through the subclusters.
* **Cluster portals** (+0x154, 64 B): s16 back cluster, s16 front cluster, s32 plane, vertices
  reflexive (+0x34, f32 x3). The polygons are planar (< 5e-4), counter-clockwise seen from the front.
* **Surface -> cluster.** Two independent routes: leaves (leaf -> its cluster, leaf -> surfaces) and the
  clusters' subclusters. They agree on every surface but one (b40_c1); the converter takes the union.
  A surface is drawn when any cluster that references it is visible. 4% of surfaces (77265 of
  1.94 M) belong to more than one cluster.

### Not verified / not used
* **Unreferenced surfaces.** 21 of 69 BSPs have surfaces that neither route lists (at most 37 per BSP,
  195 in total of 1.94 M, e.g. b40_c has 37). They have real area (not degenerate). They are dropped.
  Inference, not verified: the Xbox renderer draws cluster -> subcluster -> surface, so it never draws
  them either.
* The render `nodes` table (+0xBC, 6 B) has the collision node count but is not a tree; the first 6 bytes
  of a leaf look like the same kind of value. Purpose unidentified; ignored. Leaf surface +0x04 is
  not a collision node index; ignored. Leaf +0x06 is always 0.
* Cluster fog (+0x02): -1 none, otherwise the low 15 bits index the BSP's fog planes (all 700 uses in
  range); bit 15 is set on 634 of 700 and its meaning is unknown (kept as `BSP_CLUSTER_FOG_BIT15`).
  Sky is an index into the scenario's sky palette (not range-checked; the scenario is not read here).
  Decal range (+0x0C/+0x0E), transition BSP (+0x0A, always 0), lens flare marker range (+0x40), predicted
  resources (+0x28), portal flags (+0x18, always 0 on retail maps) are not carried over.
* Mirrors (clusters with mirror surfaces: only b40_c) are flagged (`BSP_CLUSTER_MIRROR`), not converted.
* Triangle winding is the source order; front-face orientation was not checked against normals.
* Ties: a point exactly on a plane goes to the front child (`>=`). Whether the Xbox engine breaks ties the
  same way is unknown; irrelevant except exactly on a portal plane.
* 2 of 3676 portal polygons are really concave (the others only have collinear vertices); the converter
  replaces those by their convex hull (conservative).
* `bsp_find_cluster` returns -1 for solid space (`NO_CHILD`), as well as outside the map.

## Section layout
Header, 144 bytes:

| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `BSPS` |
| 0x04 | u16, u16 | version (**2**), reserved |
| 0x08 | u32 | total section size |
| 0x0C | u32 | triangle count (sum over batches; unreferenced surfaces excluded) |
| 0x10 | f32[6] | world bounds min xyz, max xyz |
| 0x28 | u32[2] | reserved |
| 0x30 | 11 x {u32 offset, u32 count} | table directory, order below |
| 0x88 | u8[8] | reserved |

Directory order, element size, count meaning (every offset is 16-aligned and at least 144; count
times element size must fit the section; tables are padded to 16 bytes):

| # | Table | Element | Count |
|---|---|---|---|
| 0 | materials | 32 B | materials |
| 1 | batches | 32 B | batches (**all**, ordered by lowest cluster, then material) |
| 2 | vif | bytes | size of the VIF data in bytes |
| 3 | clusters | 64 B | clusters |
| 4 | cluster batch list | u16 | total entries |
| 5 | cluster portal list | u16 | total entries |
| 6 | portals | 48 B | portals |
| 7 | portal vertices | 12 B (f32 x3) | total vertices |
| 8 | nodes | 12 B | nodes |
| 9 | planes | 16 B | planes |
| 10 | leaves | u16 | leaves |

**Material, 32 B**: `u32 shader tag id`, `u16 lightmap page` (`0xFFFF` none), `u16 reserved`
(was `first_batch`; batches are no longer contiguous per material), `u16 batch count`, `u16 uv shift`
(texture uv stored as s16 = uv x 2^shift), `u32 triangle count`, 16 reserved. Materials keep the order
of the Xbox lightmap materials that have surfaces and vertices (same as `read_materials`), so a
material that ends up without batches keeps its slot.

**Batch, 32 B**: `u16 material`, `u16 triangles` (1..24), `u32 vif offset` (into the VIF data,
16-aligned), `u32 vif quadwords`, `f32 centre[3]`, `f32 radius`, `u32 reserved`. The centre (mean of the
batch's vertices, rounded to f32) and radius (largest distance, rounded up) bound it for frustum culling
and define the position encoding below. The radius is the same for the table and the packet constants.

**Cluster, 64 B**:

| Offset | Type | Field |
|---|---|---|
| 0x00 | f32[3] | bounds min (exact bounds of the cluster's batches and portal vertices; the whole world for an empty cluster) |
| 0x0C | u16 | flags: 1 sky (sky index >= 0: draw the sky when visible), 2 fog valid, 4 weather, 8 mirror, 16 fog bit 15 |
| 0x0E | s16 | fog plane index (-1 none) |
| 0x10 | f32[3] | bounds max |
| 0x1C | s16, s16 | sky index, weather index (-1 none) |
| 0x20 | u32, u32 | first entry, count in the cluster batch list |
| 0x28 | u32, u32 | first entry, count in the cluster portal list |
| 0x30 | s16, s16 | background sound, sound environment (-1 none) |
| 0x34 | u32 | triangles in the cluster's batches (shared batches counted in each cluster) |
| 0x38 | u8[8] | reserved |

**Cluster batch list**: u16 batch indices. A cluster's list is ordered by material (then batch index) so
the engine can draw material by material. A batch that belongs to several clusters is listed by each of
them; to draw each batch once per frame the engine keeps a per-batch frame stamp:
`if (stamp[b] != frame) { stamp[b] = frame; draw(b); }`.

**Cluster portal list**: u16 portal indices touching the cluster (as `cluster_a` or `cluster_b`; rebuilt
from the portals, equal to the Xbox lists).

**Portal, 48 B**:

| Offset | Type | Field |
|---|---|---|
| 0x00 | u16 | `cluster_a`: the cluster on the **positive** side of the plane (`n.p - d >= 0`); `0xFFFF` none |
| 0x02 | u16 | `cluster_b`: the cluster on the negative side |
| 0x04 | u16 | vertex count |
| 0x06 | u16 | flags (0) |
| 0x08 | u32 | first vertex in the portal vertex table |
| 0x0C | u32 | reserved |
| 0x10 | f32[4] | plane nx ny nz d, `n.p = d`, unit normal |
| 0x20 | f32[3] | centre (mean of the polygon vertices) |
| 0x2C | f32 | radius around the centre |

The polygon is convex (concave ones are replaced by their hull), planar, counter-clockwise seen from
the positive side, 3..50 vertices on retail maps. To clip the view frustum through a portal: test
the camera's side of the plane to pick the cluster on the far side, then clip the frustum against the
polygon's edges. 15 retail portals have the same cluster on both sides; skip those.

**Tree**: `nodes` are `u32 plane, u32 front child, u32 back child` (note: front first, the Xbox order is
back first); a child is `0xFFFFFFFF` (nothing: solid or outside), `0x80000000 | leaf` or a node index
that is strictly greater than the node's own index. `planes` are `f32 nx, ny, nz, d`. `leaves` map a
leaf to its cluster (u16, `0xFFFF` none, never seen on retail maps). Root = node 0. Without nodes
(synthetic or empty data) a single leaf means "cluster of that leaf everywhere".
`int bsp_find_cluster(const bsp_section*, const float pos[3])` walks from the root (front when
`n.p - d >= 0`) and returns the cluster, or -1 outside the map. It terminates in at most `node_count`
steps, whatever the data.

## Batching: one batch never spans a cluster set
Every reachable surface belongs to a set of clusters (usually one). Triangles are grouped by
(cluster set, material), each group is ordered along a Morton curve of the triangle centroids (so the
batches are spatially tight and cull well) and cut into batches of at most **24 triangles = 72 expanded
vertices**. A batch therefore contains triangles of exactly one cluster set, never of different sets,
and each triangle exists exactly once: no geometry is duplicated, only the u16 list entries are. This
never drops a visible surface (a surface shared by clusters A and B sits in a batch listed by both)
and costs about half a batch of slack per group. Batches are stored ordered by (lowest cluster of the
set, material, set), so a cluster's exclusive batches are contiguous in memory.

Retail numbers: about 22 triangles per batch on average; a cluster lists up to 540 batches (a10a),
a whole BSP 370..2900. The Phase 1 budget in docs/02-specs.md ("VU1 batches <= 400") is only reachable
if the visible set is culled to roughly 9600 triangles.

## VIF packet per batch (self-contained, TOPS-relative)
Exactly four UNPACKs, then zero words to a quadword boundary. n = 3 x triangles <= 72.
```
a) UNPACK V4-32            cmd 0x6C, addr 0,       num 2   imm 0x8000
     qw0 = f32 centre.x, centre.y, centre.z, scale          scale = radius / 32767
     qw1 = s32 n (lane x), 0, 0, 0
b) UNPACK V4-16 signed     cmd 0x6D, addr 4,       num n   imm 0x8004      x y z 0   (s16, position - centre)
c) UNPACK V4-16 signed     cmd 0x6D, addr 4+72,    num n   imm 0x804C      s t lmu lmv
d) UNPACK V4-8 unsigned    cmd 0x6E, addr 4+144,   num n   imm 0xC094      r g b a   (0x80 = 1.0)
```
Packet size is the four UNPACK words (16 B), the 32 B of constants and 8n + 8n + 4n bytes of vertex data,
padded to 16: `(48 + 20 n + 15) & ~15` bytes (1488 bytes = 93 quadwords for 24 triangles).
VU1 memory: addresses 0..3 are `a)`, vertices follow; each UNPACKed element occupies one quadword, so a batch needs 4 + 3 x 72 = **220 quadwords** of input buffer. The engine appends `MSCAL`.

* Position: `q = round((p - centre) / scale)` as s16, decoded as `p = centre + q * scale`; the error is
  at most `radius / 65534` per axis, independent of the map size.
* Texture uv: s16 = uv x 2^shift of the material (the largest power of two that fits every |uv| <= 64;
  retail has a few garbage uv beyond 1e6 that clamp). Lightmap uv: s16 = uv x 32767.
* Colours are neutral `0x80808080` for now. `vertex_colour(material, vertex)` in `bspconv.py` is the
  hook for the later lightmap-chroma bake (per vertex of the material, returns r, g, b, a); packet
  layout and reader do not change when it is filled in.

## Reader validation (`bsp_open`)
Everything is checked once, so accessors never read out of bounds: size field and signature; table
offsets (16-aligned, >= 144, inside) and counts (no overflow, fit); counts within u16 range
(materials, batches, portals <= 65535, clusters <= 65534); every batch (1..24 triangles, material in
range, exact packet size, the four UNPACK words, vertex count lane and padding verified word for word
so the VIF can never be fed another command, radius not NaN/negative); triangle and batch sums against
the header and materials; cluster batch/portal lists (index range, ranges inside the lists); portals
(cluster indices, vertex range); nodes (plane range, children forward or leaf in range); leaf clusters.
`engine/tests/test_bsp.c` corrupts every table, every directory entry, targeted fields and then every
byte of a section; a corrupt section is either rejected or safe to walk completely (ASan/UBSan).

## Sizes (raw section bytes, budget 7 MiB; docs/02-specs.md)
| Map | BSPs | Largest | Total |
|---|---|---|---|
| a10 | 9 | 3.83 MiB (a10g, 57532 triangles) | 24.5 |
| a30 | 2 | 2.89 MiB (a30_b) | 5.4 |
| a50 | 4 | 3.15 MiB (a50_control) | 11.1 |
| b30 | 2 | 2.93 MiB (b30a) | 4.7 |
| b40 | 13 | 2.47 MiB (b40_b) | 18.0 |
| c10 | 6 | 2.50 MiB (c10_interiord) | 13.2 |
| c20 | 4 | 3.69 MiB (c20_3) | 12.8 |
| c40 | 13 | 2.67 MiB (c40_b) | 18.2 |
| d20 | 5 | 3.70 MiB (d20_hangar) | 13.8 |
| d40 | 10 | 3.50 MiB (d40b) | 23.3 |
| ui | 1 | 0.00 MiB (empty: 1 cluster, 6 nodes) | 0.0 |

About 60 bytes per triangle (positions 24, uv 24, colours 12) plus 1..3% for tables; the tree is
12 B x nodes + 16 B x planes (about 0.9 MiB for a30_b). The largest is 3.83 MiB, 55% of the budget.
