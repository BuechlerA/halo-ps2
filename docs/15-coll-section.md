# `COLn` section format, version 1 (world collision)

Output of `tools/halo2ps2/collconv.py`, stored as sections of `assets/hp2/<map>.coll.hp2`, read by
`engine/src/world/coll.c` (contract: `engine/src/world/coll.h`). One section per `structure_bsp`, in the order of
the `BSPn` sections of the same map: `COL0`..`COL9`, then `COLA`..`COLZ` (b40 and c40 have 13 BSPs). Little-endian,
every table 16-byte aligned, offsets relative to the section start. Workstream A of [Phase 3](14-phase3-plan.md).

The section holds exactly what the engine needs to answer, without heap, without recursion and in bounded time:
segment hits (`coll_ray`), swept-sphere contacts (`coll_sphere_sweep`), "is this point in solid space"
(`coll_point_in_solid`) and depenetration (`coll_sphere_overlap`). The Xbox collision BSP is converted without
loss of anything those queries use; the winged-edge structure (the biggest block of the source) becomes plain
vertex loops.

## Source data (Xbox `structure_bsp`, every statement verified on all 69 BSPs of the 10 campaign maps and `ui.map`)

`structure_bsp` +0x0B0 is the *collision bsp* reflexive (always one element, 96 bytes = eight reflexives of its
own). Element sizes come from the address gaps between the blocks; nothing below is taken on trust, `collconv.py
--verify <map>` re-runs every check (2,264,007 2D lookups, all polygons, every tree property; 0 problems on the 69
BSPs).

| # | Block | Elem | Fields |
|---|---|---|---|
| 0 | bsp3d nodes | 12 | u32 plane, u32 back child, u32 front child |
| 1 | planes | 16 | f32 nx ny nz d (`n.p = d`; front is `n.p - d >= 0`; unit normals, 1 +- 1.5e-7) |
| 2 | leaves | 8 | u16 flags, u16 reference count, u32 first reference |
| 3 | bsp2d references | 8 | u32 plane (bit 31 = flip), u32 root (bit 31: a surface, else a bsp2d node) |
| 4 | bsp2d nodes | 20 | f32 a, b, d (2D line), u32 child, u32 child |
| 5 | surfaces | 12 | u32 plane (bit 31 = flip), i32 first edge, u8 flags, i8 breakable group, i16 material |
| 6 | edges | 24 | i32 start vertex, end vertex, forward edge, reverse edge, left surface, right surface |
| 7 | vertices | 16 | f32 x y z, i32 first edge |

Facts (all checked):

* **3D tree.** Root = node 0, every child index is greater than its parent, every node and leaf has exactly one
  parent, depth at most **66** (c10_exteriora; a10: 22..52). A child of `0xFFFFFFFF` is **solid space** (and
  everything outside the world: terrain BSPs end in such children without any surface there), bit 31 a leaf,
  otherwise a node. Leaf space is empty; the surfaces lie on the boundary planes of the leaves that reference them.
* **Leaf flags.** Bit 0 = "contains two-sided surfaces": set exactly when a surface reachable from the leaf is
  two-sided (0 exceptions on 69 BSPs); no other bit occurs.
* **References are not consecutive in leaf order** on 40 of the 69 BSPs: 3,565 of the 1.74 M references are owned
  by no leaf (orphans), 5,013 by several leaves. 26 surfaces in the whole game no leaf can reach (14 are named only by
  orphan references, 12 by no reference at all). The converter re-emits the references leaf by leaf and drops the dead data.
* **2D BSP.** The point is projected onto the reference's plane by dropping the dominant axis `d` of the plane
  normal (`|n[1]| >= |n[0]|` picks y, then `|n[2]| >= |n[d]|` picks z: **on exact ties the later axis wins**, found
  from 33 failing lookups of 33,064 on a10a, none with this rule); `(u, v) = ((d+1) % 3, (d+2) % 3)`, swapped when
  `sigma = (n[d] > 0) xor flip` is false (so the 2D basis is counter-clockwise seen from the surface front). Then
  `side = a*u + b*v - dd`: `side >= 0` takes the **second** child, otherwise the first; a child with bit 31 is a
  surface. The flip bit of a reference equals the flip bit of every surface its tree reaches (never differs). Every 2D
  tree is a tree (one parent per node, all reachable), at most 8 deep. **No 2D child is ever null: the 2D BSP has no
  empty leaves.** A point of the plane that lies on no surface at all (a crossing in the open part of a leaf's face)
  still gets a surface, so the lookup is a *candidate*, not a decision (see "Rays").
* **Surfaces.** The polygon is walked from the first edge: if `left(e) == s` take `start(e)` and continue with
  `forward(e)`, else take `end(e)` and continue with `reverse(e)`, until the first edge comes back. Every loop
  closes, every edge side is used exactly once, polygons have 3..8 vertices, are **convex** (0 concave, 0
  degenerate in 1,059,523 surfaces), planar to 4.1e-4 of their plane and wound counter-clockwise about
  `n * (flip ? -1 : 1)`, the surface normal. Vertex indices stay below 28,752 (they fit u16).
* **Flags** (bit meanings from the public tag documentation): 1 two-sided, 2 invisible, 4 climbable, 8 breakable. The
  1,059,523 surfaces: flags 0: 1,042,705; 1: 9,684; 3: 4,248; 5: 48; 7: 1,358; 9: 1,480; nothing else. The breakable
  byte (0..31) is non-zero only on breakable surfaces.
* **Two-sided = twin pair.** All 16,818 two-sided surfaces come in pairs: two coincident polygons (same vertex
  positions) with opposite orientation, both flagged two-sided, each listed by its own front leaf. Hence a
  surface is only ever hit from its front; the back of a two-sided surface is the front of its twin (this is why a
  two-sided surface is "listed by its front leaf only" when looked at alone). The reader has no back-face logic.
* **Material.** The surface's i16 indexes the BSP's collision-material palette (`structure_bsp` +0x0A4, 20 B: a
  shader tag reference, then at +0x12 a u16). That u16 is the shader's material type (shader +0x22 in `senv`, `sotr`,
  `sgla`: equal in 1,565 of the 1,573 palette entries of the ten maps; the other 8 are the `soso` "invisible" shader whose
  value is 0) and indexes the `globals` material table (0 dirt, 1 sand, 2 stone, 3 snow, 4 wood, 5 metal hollow, 6
  metal thin, 7 metal thick, 9 glass, 10 force field, 28 water, 31 ice occur; all consistent with the shader names).
  The converter stores that type per surface (255 = surface without material, 2,199 surfaces).

## Section layout

Header, 128 bytes:

| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `COLL` |
| 0x04 | u16, u16 | version (**1**), reserved |
| 0x08 | u32 | total section size |
| 0x0C | u32 | depth of the 3D tree in nodes (converter-measured, at most 128) |
| 0x10 | f32[6] | bounds min xyz, max xyz of the surface vertices |
| 0x28 | u32 | stack need of the 2D subtree walk (at most 64) |
| 0x2C | u32 | reserved |
| 0x30 | 8 x {u32 offset, u32 count} | directory: nodes, planes, leaves, refs, nodes2d, surfaces, indices, vertices |
| 0x70 | u8[16] | reserved |

Every offset is 16-aligned, >= 128 and inside the section. Records:

| Table | Element | Layout |
|---|---|---|
| nodes | 8 B | `u32 w0`: front child (bits 0..23) \| plane bits 0..7 << 24; `u32 w1`: back child \| plane bits 8..15 << 24. A 24-bit child: `0xFFFFFF` solid, bit 23 set a leaf (index in 0..22), else a node index **greater than the node's own**. |
| planes | 16 B | f32 nx ny nz d (as the Xbox stores them: `n.p - d >= 0` is the front) |
| leaves | u32, **count + 1** entries | first reference of the leaf; the entry after the last leaf equals the reference count (so a leaf's references are `leaves[i] .. leaves[i+1]`); bit 31 = "has two-sided surfaces" |
| refs | 4 B | `u16`: plane (bits 0..14), flip (bit 15); `u16` root: bit 15 set = surface index (low 15 bits), else 2D node index |
| nodes2d | 16 B | f32 a, b, d; `u16 front` (`a*u + b*v - d >= 0`), `u16 back`; each child: bit 15 = surface, else a 2D node index **greater than the node's own** (a root may be shared by several references) |
| surfaces | 8 B | `u32`: first index (bits 0..19) \| vertex count (bits 20..23, 3..15) \| breakable group (bits 24..28), bits 29..31 zero; `u16` plane (bits 0..14) \| flip (bit 15); `u8` flags (0..15); `u8` material type (255 none) |
| indices | u16 | vertex indices of the polygons, consecutive per surface, counter-clockwise about the surface normal |
| vertices | 12 B | f32 x y z |

Limits the converter checks (an error, not a silent loss; the retail data never gets near them): nodes and
leaves < 2^23, planes / 2D nodes / surfaces <= 32768, vertices <= 65536, polygon indices < 2^20, 3..15 vertices
per polygon, convex polygons, breakable group <= 31, tree depth <= 128, 2D stack need <= 64.

What the converter drops or rewrites: the **edges** (the loops carry what a polygon needs; 2.0 M edges in the
ten maps, 37% of the source bytes), the vertex `first_edge`, the leaf's reference count (implicit in `leaves[i+1]`), orphan references, surfaces no
leaf can reach (26), vertices only they used (0 on retail), the 2D nodes of dropped references. References owned by
several leaves are duplicated per owner. 2D nodes are renumbered in preorder, so a child always follows its
parent (the Xbox order does not guarantee it). The planes and the 3D tree keep their Xbox numbering.

## Reader (`world/coll.[ch]`)

Pure C11, no heap, no `double`, no recursion; compiles with the EE gcc (11.3 KB of code, only `memset` and `__muldi3`
unresolved). Records are decoded with byte loads (no alignment assumption); `-DCOLL_ALIGNED_LOADS` switches to plain 4-byte
loads for a 16-byte aligned buffer (EE, little-endian only; measure before using it on the console).

```c
coll_status coll_open(const uint8_t *data, size_t size, coll_bsp *out);
int coll_ray(const coll_bsp*, const float a[3], const float b[3], uint32_t ignore_flags, coll_hit*);
int coll_sphere_sweep(const coll_bsp*, const float a[3], const float b[3], float radius, coll_hit*);
int coll_sphere_sweep_flags(..., float radius, uint32_t ignore_flags, coll_hit*);     /* helper */
int coll_point_in_solid(const coll_bsp*, const float p[3]);
int coll_sphere_overlap(const coll_bsp*, const float c[3], float r, float push_out[3]);
int32_t coll_locate_leaf(const coll_bsp*, const float p[3]);                           /* helpers */
uint32_t coll_surface_polygon(const coll_bsp*, int32_t surface, float out[][3]);
int coll_surface_info(const coll_bsp*, int32_t surface, float plane[4], uint16_t *material, uint16_t *flags, uint16_t *breakable);
```

`coll_hit` is the contract's `{t, normal[3], point[3], material, flags, surface}` plus `breakable` (the breakable
group, valid with `COLL_F_BREAKABLE`) and a pad. `material` is the globals material type (`COLL_NO_MATERIAL` =
0xFFFF when the surface has none), `flags` are `COLL_F_TWO_SIDED/INVISIBLE/CLIMBABLE/BREAKABLE`, `surface` indexes
this section's surface table. The normal faces the mover.

**`coll_open`** validates everything once (size field, signature, version, directory, table bounds, counts and
index widths, finite and sane floats (|x| <= 65536, plane normals within 1 +- 2%), node planes and children
(later nodes only, leaves in range), leaf table monotone and ending at the reference count, reference planes and
roots, 2D node children (later nodes only), surface planes / vertex ranges / reserved bits, polygon indices). After
that no query checks an index. A corrupt section is either rejected or safe to query: queries use explicit
stacks (128 3D entries, 64 2D entries, bounds-checked pushes) and a visit budget of `2 * (nodes + leaves) + 16`, so
shared children in corrupt data give wrong answers, never an endless loop. Host time: 0.11 ms (a10a, 1.05 MiB) to 0.26 ms (c20_3, 1.66 MiB).

**Point location / solid.** Standard descent, ties (`n.p - d == 0`) go to the front child, the same rule as
`bsp_find_cluster`. A `none` child is solid, so is everything outside the world. Cross-check: 200,000 random points
of a10, a30, b40 and c10 against the render tree of their `BSP0` section (`bsp_find_cluster < 0`), 0 differences.

**Rays.** The segment is walked front to back through the 3D tree with its current part `[t0, t1]` inside the node.
At a leaf, every reference whose plane the segment crosses front-to-back (`n.a - d >= 0`, `n.b - d < 0`, `n` the
flip-corrected normal) at a `t*` inside the leaf's part (plus 4 mm: planes and polygons of the Xbox data differ
by up to a few mm) gives the **2D lookup** at the crossing point, a candidate surface. The candidate is a hit
when (1) the crossing lies in the candidate's own polygon (0.5 mm tolerance), or (2) solid space follows the
crossing 1 mm along the ray. Why both: the 2D BSP has no empty leaves, so the open part of a leaf's face (the
ray goes on into the next leaf) also gets a surface; polygon containment rejects that. And the tree's planes
and the surface planes disagree by up to ~1 mm in places (a sliver between the plane of the leaf and the polygon);
solid space right behind the face still makes it a hit (the 2D lookup then names a neighbour of the real polygon at the
same point). One-sided surfaces with empty space behind them exist (thin sheets), (2) alone would lose them, (1) alone
would lose the sliver cases. The first leaf that yields a hit ends the walk; among its references the smallest `t`
wins. `ignore_flags` skip surfaces (the ray goes on). A ray that starts in solid space hits nothing until it has left it
and meets a front face. There is no back-face logic: two-sided surfaces are pairs.

**Sphere sweeps.** The sphere's centre moves `a -> b` with radius `r`. The tree walk is a *capsule walk*: at every
node the capsule (the centre segment, shrunk to the best contact found so far, plus `r` and 4 mm) decides whether
it touches the front, the back or both sides; at a leaf, every reference whose plane the capsule can reach is
expanded (its 2D subtree is enumerated with the 64-entry stack) into its surfaces. Each surface goes through a
64-entry direct-mapped cache (a surface is typically reached by several leaves) and one test:

* **face**: the centre reaches the plane offset by `r` (`dist = r`), the contact point (centre minus `r n`) lies in the
  polygon (1e-4 tolerance); a sphere that already touches or penetrates (`0 <= dist < r`) and moves toward the plane
  reports `t = 0` with the foot point;
* **edge**: first contact of the centre path with the cylinder of radius `r` around each polygon edge whose contact point
  lies within the segment;
* **vertex**: first contact with the sphere of radius `r` around each vertex.

An edge or vertex contact counts only when the centre is in front of the surface's plane at the contact (or within 1 mm
behind it). The earliest contact wins, a face wins ties against edges (1e-6). The hit normal for edges/vertices is the
direction from the contact point to the centre, so the sphere slides around corners. `t` is the exact contact
fraction; the integrator should stop ~1e-3 wu short. `radius <= 0` is a ray.

**Depenetration.** `coll_sphere_overlap` walks the capsule of the centre (a point, radius `r`) twice; for each surface in
reach it pushes the centre out by `r - distance` along the face normal (when the centre's projection is inside the polygon)
or along the direction from the closest edge/vertex point; a centre inside the solid behind a *one-sided* face (up to `r`
behind it) is pushed out through that face. The pushes are applied in sequence (two passes), so a corner resolves to a
diagonal push. It cannot resolve a sphere inside a crevice narrower than its diameter (it ends close to the best of the
bad positions): 0.3% of the test pushes in the 10-map run.

## Verification

`tools/tests/test_collconv.py` (no game data): a synthetic Xbox `structure_bsp` block (two closed, subdivided cuboid rooms
joined by a two-sided pane that exists as a twin pair, winged edges built from polygons, 2D BSPs with both orientations of
the projection rule, a shared reference, an orphan reference, a dead cuboid, breakable and invisible and climbable surfaces,
three materials): reader, polygons, every check of `verify_source`, compaction (counts of dropped data), serialisation, the
section parser, 2D lookups through the section against the source, and attacks on the source (outside tables, edge chains,
planes/winding/flags/leaf flag/twin/parent counts), on the builder (16-gon, concave polygon, material type, breakable id) and on the section
(short, magic, version, size, every table offset/count/alignment). With `assets/decompressed/a10.map` it converts all 9
BSPs of a10 and checks them against their source (polygons, flags, counts); `COLL_ALL_MAPS=1` does the ten maps.
`engine/tests/test_coll.c` (run by `make -C engine/tests coll`, part of `make -C engine/tests`), under ASan/UBSan:

* the primitives (sphere, edge, polygon, closest point, projection rule incl. ties);
* the synthetic world with hand-computed numbers: point location (ties, solid), rays (floor, ceiling with flipped plane and
  swapped axes, both sides of the pane, flags and `ignore_flags`, solid start, the shared reference, a 2D cell corner),
  sweeps (face contacts with exact `t`, flags, the pane from both sides, touching start, sliding, diagonal into a corner,
  radius 0), depenetration (front, behind a one-sided face, corner, both halves of the pane, too far behind), surfaces,
  the empty section;
* hostile input: every truncation, every byte of the synthetic section flipped three ways (4,560 flips, 2,779 still
  open, all walked clean), plus 400 random flips and 60 truncations of a real a10 section;
* on a real pack (the converted a10, built by the test target from `assets/decompressed/a10.map` when it exists): every BSP opens,
  every polygon is planar and wound to its plane; on BSP 0 (6,000 rays, 1,500 sweeps, 400 depenetrations) and on a few
  more BSPs (1,500 / 400 each) the checks of the next section.

### Numbers (ten maps, the pack of each map converted by `collconv.py`, `test_coll` per pack, ASan/UBSan, -O1)

Brute-force reference for rays: every surface polygon of the BSP (front-to-back crossing inside the polygon, 0.5 mm
tolerance). Rays start in empty space (half), anywhere (a quarter, solid starts included), next to a random surface (a
quarter); lengths 0.05..150 wu. Sweeps: radii 0.05..0.5, half of the starts next to a random surface, lengths 0.05..40 wu;
they must never end in solid space and never penetrate (a brute-force clearance function, the distance to the polygons that the
sphere is in front of, checked at steps of `r / 2` along the path up to the contact), the contact must be a touch (clearance
within 3 mm of `r`), the contact normal must oppose the motion. Depenetration: a touching sphere is pushed 0.6 r into the surface it hit,
`coll_sphere_overlap` must bring its clearance back to `r - 0.02` and out of solid space.

| Map | rays (hit) | same surface | same point, other surface | disagree | sweeps (stopped: face / edge+vertex) | violations | depenetration resolved |
|---|---|---|---|---|---|---|---|
| a10 | 10500 (4978) | 4942 | 36 | 0 | 2700 (1227: 897 / 330) | 0 | 1566 / 1568 |
| a30 | 7500 (1303) | 1296 | 7 | 0 | 1900 (311: 283 / 28) | 0 | 703 / 703 |
| a50 | 7500 (2150) | 2111 | 39 | 0 | 1900 (454: 403 / 51) | 0 | 751 / 761 |
| b30 | 7500 (1664) | 1649 | 15 | 0 | 1900 (392: 336 / 56) | 0 | 784 / 784 |
| b40 | 10500 (3339) | 3301 | 37 | 1 | 2700 (771: 684 / 87) | 0 | 1584 / 1591 |
| c10 | 7500 (2294) | 2271 | 23 | 0 | 1900 (484: 407 / 77) | 0 | 799 / 800 |
| c20 | 7500 (2683) | 2619 | 64 | 0 | 1900 (616: 543 / 73) | 0 | 800 / 800 |
| c40 | 10500 (3355) | 3317 | 37 | 1 | 2700 (755: 668 / 87) | 0 | 1582 / 1588 |
| d20 | 7500 (2150) | 2132 | 18 | 0 | 1900 (519: 430 / 89) | 0 | 794 / 800 |
| d40 | 10500 (3397) | 3349 | 48 | 0 | 2700 (936: 756 / 180) | 0 | 1516 / 1519 |
| total | 87000 (27313) | 26987 | 324 | 2 | 22200 (6465: 5407 / 1058) | 0 | 10879 / 10914 |

* **Rays**: 98.8% of the hits name the same surface as the brute force at the same `t`; 324 (1.2%) hit the same point but name another
  surface: the two share an edge at that point, or the tree's plane and the polygons disagree by a millimetre (the 2D lookup then
  names a neighbour on the same plane; same material, nearly the same normal). **Disagreements: 2 of 87,000**, the same geometry
  in b40 and c40 BSP 8 (the same BSP): the brute force crosses a glass pane (two-sided, breakable) 0.137 wu before the tree does;
  the pane's front leaf is a 1.6 mm sliver whose references do not list the pane's plane, the pane is listed by a leaf the
  ray never enters, so the tree reaches the next surface (a plane 0.137 wu further) instead. Also 3 rays that start inside solid space
  differ (the polygon reference crosses a front face from a point in solid space that lies in front of its plane; the tree has no
  empty leaf to start from): tree and reference are not comparable there. Rays that start in empty space and
  stay within 4 mm of the tree's planes agree exactly.
* **Sweeps**: 22,200 sweeps, 6,465 stopped by a surface (5,407 face contacts, 1,058 edge or vertex contacts); **none** ended or passed
  through solid space, none penetrated (> 3 mm), none stopped short of touching; one terrain sweep left the open edge of the world
  (solid space entered without crossing a surface) and is counted separately. a10 BSP 0 alone: 1,500 sweeps, 894 stopped (612 / 282).
* **Depenetration**: 10,879 of 10,914 pushed spheres (99.7%) end clear; the rest sit in crevices narrower than the sphere.
* Leaf, node and surface counts, point location against the render tree: 0 differences in 800,000 points (a10, a30, b40, c10 `BSP0`).

### Sizes (raw section bytes) and conversion time

Budget: the 7 MiB "current BSP" budget of docs/02-specs.md, target <= 1.5 MiB per BSP.

| Map | BSPs | Raw total | Largest BSP | On disc (LZ4) | Convert |
|---|---|---|---|---|---|
| a10 | 9 | 9.22 MiB | 1.40 (a10b) | 6.79 | 2.5 s |
| a30 | 2 | 2.83 | **1.58** (a30_b) | 2.44 | 0.9 s |
| a50 | 4 | 4.74 | 1.26 | 3.85 | 1.4 s |
| b30 | 2 | 2.06 | 1.40 | 1.70 | 0.7 s |
| b40 | 13 | 7.89 | 1.24 | 6.36 | 2.1 s |
| c10 | 6 | 6.05 | 1.27 | 4.99 | 1.7 s |
| c20 | 4 | 5.83 | **1.66** (c20_3), 1.57 (c20_2) | 4.48 | 1.7 s |
| c40 | 13 | 7.96 | 1.33 | 6.42 | 2.1 s |
| d20 | 5 | 5.93 | **1.51** (d20_hangar) | 4.84 | 1.7 s |
| d40 | 10 | 9.57 | 1.36 | 7.35 | 2.5 s |
| ui | 1 | 0.00 | 0 (6 surfaces) | 0.00 | 0.05 s |

Four BSPs exceed 1.5 MiB (by at most 11%; all well below the 7 MiB budget); the 8-byte nodes, 4-byte leaves and references
and the dropped edges already bring the Xbox's 124.6 MiB for the ten maps down to 62.1 MiB (about half). The rest is dominated by nodes (8 B), planes (16 B)
and 2D nodes (16 B) on the large outdoor BSPs; only a third of the 2D lines are axis aligned, so the obvious further packing saves
little (about 45 KB on c20_3).

a10 per BSP (raw MiB, counts as `nodes / leaves / 2D nodes / surfaces / vertices`; none of these has orphans or dead surfaces):

| BSP | MiB | nodes | leaves | 2D nodes | surfaces | vertices |
|---|---|---|---|---|---|---|
| a10a | 1.05 | 15436 | 6216 | 14625 | 20960 | 20047 |
| a10b | 1.40 | 19342 | 7262 | 20885 | 28314 | 27083 |
| a10c | 1.01 | 14611 | 4810 | 13899 | 20405 | 20297 |
| a10d | 0.91 | 13177 | 4312 | 12264 | 18263 | 18286 |
| a10e | 1.28 | 17307 | 5460 | 18237 | 26120 | 25807 |
| a10f | 1.40 | 19898 | 7632 | 18572 | 28341 | 27832 |
| a10g | 1.38 | 18825 | 6274 | 18428 | 28454 | 28751 |
| a10_space | 0.31 | 7815 | 4609 | 2120 | 5084 | 3838 |
| x10hangar | 0.48 | 7605 | 3799 | 7648 | 8920 | 8503 |

### Speed (host, Apple M4 Max, `-O2`, a10 BSP 0, single thread; `COLL_BENCH=1`, printed by the test target)

| Query | per millisecond |
|---|---|
| `coll_ray`, 5..40 wu segments from free points (99.9% hit) | ~1,600 |
| `coll_ray`, 0.05..1 wu segments (a bullet step) | ~4,700 |
| `coll_sphere_sweep`, r = 0.2, 0.05..1 wu (one tick of walking) | ~460 |
| `coll_sphere_sweep`, r = 0.2, 5..20 wu (vehicle, grenade) | ~45 |
| `coll_point_in_solid` | ~9,000 |
| `coll_sphere_overlap`, r = 0.3 | ~450 |

The same loops on c20 BSP 0 (open terrain, fewer surfaces in reach) are 3..6x faster for sweeps and overlap. The EE has no
`double`, no out-of-order core and 32 KB of data cache; assume 30..100x slower than the host (not measured: ADR-022), i.e. one
walking sweep costs about 50..200 us, a 30 Hz tick of 10 sweeps plus a few rays under 2 ms. Stack use on the EE
(`-fstack-usage`): `coll_ray` 2 KB, a sweep about 1.7 KB, depenetration 1.6 KB.

## Not verified, approximations, known differences from the Xbox engine

* The reader reproduces the *data's* semantics, not the Xbox code. In particular the decision "polygon contains the point,
  or solid follows" for ray hits, the 4 mm and 1 mm tolerances, the capsule walk and the contact rules of sweeps are mine,
  validated against polygon brute forces (above), not against the original engine's results. Hits near polygon edges (within a
  few mm) may name a neighbour or differ by millimetres from the original.
* Flag meanings (invisible, climbable, breakable) are from the public tag documentation and consistent with the data
  (breakable groups only on flag 9; flag 7 and 3 surfaces are two-sided + invisible clip surfaces), not verified in play.
  Whether bullets should skip invisible or climbable surfaces is up to the integrator (`ignore_flags`).
* The material type per surface is the u16 in the BSP's collision-material palette, equal to the shader's material type field
  (1,565 of 1,573 palette entries; the other 8 are the 0 of the `soso` invisible shader). The names of the globals material table
  (index 0 dirt .. 31 ice) are from the public documentation; the engine's `unitdef_material` (workstream C) must index the same table.
* Rays that start inside solid space, and the sliver cases above, can differ from a polygon reference. Spheres that start
  inside solid space are not rescued by sweeps (use `coll_sphere_overlap` first; it handles up to `r` behind a one-sided
  face and cannot resolve crevices narrower than the sphere).
* The 2D BSP's behaviour for points outside every surface (the open part of a leaf face) is not modelled by the data (no empty
  leaves); the polygon/solid rule is this reader's solution.
* Terrain BSPs end in `none` children without surfaces at the world's edge: a sweep can leave the world there (point in solid, no hit).
* Not measured on the EE or the PS2 (ADR-022); host numbers only.
* `breakable` groups are exported but breaking is the integrator's business (damage, replacing the surface).

## Using it

```
python3 -I tools/halo2ps2/collconv.py assets/decompressed/a10.map -o assets/hp2/a10.coll.hp2 [--bsp N]
python3 -I tools/halo2ps2/collconv.py --info assets/hp2/a10.coll.hp2
python3 -I tools/halo2ps2/collconv.py --verify assets/decompressed/a10.map [--bsp N]   # every source check, per BSP
make -C engine/tests coll                                                              # synthetic + (with the maps) real tests
```

The engine loads `COLn` of the current BSP like `BSPn` (docs/07: tags `COL0`..`COLZ`), `coll_open` it once (about 0.1..0.3 ms on
the host), and keeps the buffer alive. Nothing else to wire: no config keys, no tag ids exported (surfaces carry the globals
material type directly).
