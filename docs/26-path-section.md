# `PATn` section format, version 1 (pathfinding: navigation mesh and queries)

Output of `tools/halo2ps2/pathconv.py`, stored as sections of `assets/hp2/<map>.path.hp2`, read by `engine/src/ai/path.c` (contract:
`engine/src/ai/path.h`). One section per `structure_bsp`, in the order of the `BSPn` / `COLn` sections of the same map: `PAT0`..`PAT9`,
then `PATA`..`PATZ` (b40 and c40 have 13 BSPs). Little-endian, every table 16-byte aligned, offsets relative to the section start.
Workstream P of [Phase 4](23-phase4-plan.md).

**The most important finding first: the Xbox data holds no sectors, links, vertices, object references or hints.** The plan assumed Halo 2
style pathfinding blocks; the Halo CE `structure_bsp` has one byte per collision surface and nothing else (next section). Everything the
section contains beyond that byte is **derived by the converter from the collision surfaces** (sectors, links, hints, costs, widths,
headroom). The derivation rules and thresholds are mine, validated against the collision BSP, not read from the data; "what is data" and
"what is derived" are separated throughout this document.

## Source data (Xbox; every statement verified on all 69 BSPs of the ten campaign maps and `ui.map`, `pathconv.py --verify` re-runs it)

`structure_bsp` +0x1E4 is a reflexive named "pathfinding surfaces": **one byte per collision-BSP surface**. Its count equals the surface
count of the collision BSP on every BSP (`ui.map`: 6 surfaces, 6 bytes). The neighbouring reflexive at +0x1F0 (the "pathfinding edges" that
would hold edge data) is empty on all 69 BSPs and the words around it are zero; no other block of the BSP or of the scenario is
pathfinding data (the scenario has no hint blocks: jump / climb / vault hints are Halo 2 constructs).

The byte takes three values on the retail maps (1,059,523 surfaces):

| value | surfaces | meaning (verified) |
|---|---|---|
| `0x00` | 858,121 | not walkable |
| `0x40` | 201,203 | **walkable**: bit 6 is set exactly when the surface's effective normal has `z >= 0.71` |
| `0xC0` | 199 | walkable **and breakable**: bit 7 is set exactly on the walkable surfaces whose collision flags say breakable (glass floors) |

* The `z >= 0.71` rule has **no exception on any surface**: the lowest flagged surface has `nz = 0.71002`, the highest unflagged upward
  one `0.70998` (so 45 degrees, `cos = 0.7071`, is *not* walkable: a 45 degree ramp is excluded by 0.003).
* The flag is independent of every other surface flag: two-sided, invisible, climbable and breakable surfaces are flagged by slope alone
  (539 invisible+two-sided+climbable surfaces are flagged walkable, for instance), so the byte carries no information beyond the
  geometry. It is nevertheless the engine's own definition of "walkable for the AI", and the converter honours it exactly.
* No other bits occur (`verify_mesh` reports unknown bits, there are none).

The original engine therefore navigates over the flagged collision surfaces; what it does with them at run time is not in the data. Its
actors vault and jump obstacles through their own movement behaviour (actor tags), not through hints.

## Derived navigation mesh

* **Sectors** are the flagged surfaces (convex polygons, 3..8 vertices, counter-clockwise seen from above because `nz >= 0.71`),
  merged greedily: two neighbours across a shared edge run merge when the union is convex in plan (3e-4 of the longer edge), planar to
  `PLANE_TOL = 0.02` wu (vertical deviation from the area-weighted plane), has at most `MAX_VERTS = 12` vertices, and the two have the
  same *key* (breakable bit, invisible / climbable flags, material type). Surfaces the collision section drops (no leaf reaches them)
  are skipped (none of them is walkable on the retail maps: `dropped = 0`). 201,402 flagged surfaces become 143,292 sectors (71%); the
  largest sector merged 16 surfaces. Merging stops mostly at convexity (terrain triangle fans), not at planarity: on the largest terrain BSP (10,869
  surfaces) raising the tolerance from 0.02 to 0.2 wu only takes 7,976 sectors down to 5,226.
  A sector keeps the COLn index of its largest source surface (`surface`) and that surface's material type.
* **WALK links**: two sectors share an edge run. The collision mesh is *not* a manifold where two-sided / invisible surfaces overlay a floor
  (1,002 directed edges of a10 BSP 0 have two owners), so adjacency is taken from the final sector loops (every owner is a neighbour);
  the portal is the run (end points are loop vertices of both sectors).
* **STEP / VAULT / DROP links** cross a *riser*: a chain of at most 3 non-walkable surfaces with `-0.35 <= nz < 0.71` (walls, steep
  slopes, walls leaning out by up to 20 degrees) between a boundary edge of sector A and a boundary edge of sector B that are anti-parallel (cos <=
  -0.9), overlap by at least 0.08 wu in plan, lie at most `0.45 + 1.5 x rise` apart, and where the chain stays inside the vertical band of the two
  edges (+0.05): a wall taller than both floors is a wall, not a step. The rise classifies the pair: `|rise| <= 0.20` STEP (both ways);
  rise up to 0.55 VAULT upward and DROP downward; a higher rise only DROP downward, down to 3 wu. (Player step height in the engine is
  0.17, the jump apex about 0.57.) The portal is the plan overlap.
* **CLIMB links** (ladders): a component of climbable collision surfaces (connected through shared vertices, at least 0.6 wu tall) whose
  footprint (+0.4 wu) touches a walkable, non-climbable sector near its bottom and another near its top (within 0.35 wu): bottom <-> top.
  A flat patch of climbable surfaces (the data has some) is not a ladder. The portal is a point (end points equal).
* **JUMP** (gap hops) is **not generated**: the converter has no sweep query to prove that a gap is open, and a false hop through a wall is
  worse than none. The kind is reserved (`PATH_LINK_JUMP`); the integrator can generate hops at level load with `coll_sphere_sweep`.
* **Costs**: centroid(A) -> portal middle(s) -> centroid(B) plus a penalty (STEP 0.03, VAULT 0.6 + 0.5 rise, DROP 0.3 + 0.5 drop,
  CLIMB 1.0 + 0.5 rise), stored as a u16 in `1/cost_scale` wu (64 on 67 BSPs, 32 on the one with 600 wu triangles), **rounded up**, so the
  straight centroid distance never exceeds a link's cost: the A* heuristic is admissible *and consistent*, and the A* is optimal (tested
  against Dijkstra, below).
* **Components** over the symmetric kinds (WALK, STEP, CLIMB): union-find ids, for O(1) negative reachability.
* **Headroom** per sector (u8, 1/32 wu): the lowest overhead surface (`nz < -0.3`) above any part of the sector, from clipping every
  overhead polygon against the sector polygon in plan (both planes are linear: the minimum is at a corner of the overlap). Coincident
  twins (< 1 cm) are ignored. 22% of the sectors (31,345) have less than 0.5 wu: hidden voids between layered floors, crawl spaces, the
  lower layer under a walkable sheet. a10 BSP 0 has 864 of 1,630.
* **Thickness** per sector (u8, 1/64 wu): the smallest extent of the polygon across any of its edges (slivers: 49% of the sectors are thinner
  than 0.4 wu; floors are mosaics of strips, grooves and bevels).
* **Link width** (byte 3 of the link record, 1/16 wu, 255 = unlimited for ladders): the plan length of the portal.

Nothing above is hint data; it is the best nav mesh the flagged surfaces and the surrounding collision geometry support.

## Section layout

Header, 128 bytes:

| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `PATH` |
| 0x04 | u16, u16 | version (**1**), reserved |
| 0x08 | u32 | total section size |
| 0x0C | u32 | component count |
| 0x10 | f32[6] | bounds min xyz, max xyz of the sector vertices |
| 0x28 | f32, f32, f32 | grid cell size, grid origin x, y |
| 0x34 | u16, u16 | grid nx, ny |
| 0x38 | f32, f32, f32 | thresholds the links were classified with: step max (0.2), vault max (0.55), drop max (3.0) |
| 0x44 | u32 | cost units per wu (64 or 32) |
| 0x48 | 6 x {u32 offset, u32 count} | directory: sectors, points, indices, links, cell start, cell items |
| 0x78 | u8[8] | reserved |

Every offset is 16-aligned, >= 128 and inside the section. Records:

| Table | Element | Layout |
|---|---|---|
| sectors | 32 B | `f32` centroid x y z (the plan-area centroid lifted on the sector plane); `u32` first index; `u32` first link; `u16` link count; `u8` vertex count (3..15); `u8` flags (1 breakable, 2 invisible, 4 climbable, 8 two-sided); `u16` component; `u16` COLn surface (0xFFFF none); `u8` material type (255 none); `u8` headroom; `u8` thickness; `u8` reserved |
| points | 12 B | `f32` x y z: polygon vertices and clipped portal end points, shared |
| indices | u16 | point indices of the sector polygons, counter-clockwise seen from above (collinear vertices that carry no portal end are dropped) |
| links | 12 B | `u16` destination; `u8` kind (0 walk, 1 step, 2 vault, 3 drop, 4 climb, 5 jump); `u8` width; `u16` right point, `u16` left point (the portal on the *source* side, as seen walking from the source to the destination: the end of the edge run is the left end); `u16` cost; `s16` dz in 1/256 wu (destination minus source at the portal middles). Links are grouped by source sector, sorted by (destination, kind). |
| cell start | u32, nx*ny + 1 entries | CSR offsets into the cell items |
| cell items | u16 | sectors whose polygon touches the cell (polygon against cell, conservative by 0.2 mm); the cell size is the best of 1..4 x the median sector extent (fewest bytes), at most 96 cells per axis |

The destination side of a chain link is not stored: the landing point of a vault / drop / climb is the closest point of the destination
sector to the takeoff point (the reader computes it).

## Reader (`ai/path.[ch]`)

Pure C11, no heap, no `double`, no recursion; compiles with the EE gcc (`-O2`: 17.1 KB of code, only `memset` unresolved; the largest frame
is 336 bytes). Records are decoded with byte loads. `path_open` validates everything once (size field, signature, version, tables, counts,
ids in u16 range, finite and sane floats, polygon vertex counts, every index of every table, link kinds and destinations, grid offsets
monotone and ending at the item count, items < sectors, component ids < component count, thresholds, cost scale). Every search has an
expansion budget and every loop is bounded by a table size; corrupt data that survives `path_open` gives wrong answers, never a crash or an
endless loop. Memory of a query is a caller-owned `path_scratch` (a 73 KB struct plus 22 B per sector, 175 KB for the largest BSP): queries run one at a
time, so one scratch serves every actor.

```c
path_status path_open(const uint8_t *data, size_t size, path_bsp *out);

/* sectors */
int      path_sector_get(const path_bsp*, uint32_t sector, path_sector_info*);      /* centroid, comp, surface, flags, material, headroom, thickness, links */
uint32_t path_sector_polygon(const path_bsp*, uint32_t sector, float out[][3]);
float    path_sector_height(const path_bsp*, uint32_t sector, float x, float y);
/* locating and snapping */
int32_t  path_locate(const path_bsp*, const float pos[3]);                           /* the sector under pos (highest floor within 0.3 above .. 4 below), -1 */
int32_t  path_locate_ex(const path_bsp*, const float pos[3], float below, float above, float *floor_z);
int32_t  path_nearest_sector(const path_bsp*, const float pos[3], float max_dist, float *dist, float point[3]);
int      path_snap(const path_bsp*, uint32_t sector, const float pos[3], float margin, float out[3]);   /* spawn / firing position snapping */
/* links and hints */
uint32_t path_link_count(...); int path_link_get(...);
uint32_t path_sector_hints(const path_bsp*, uint32_t sector, uint32_t kind_mask, path_link *out, uint32_t max);   /* the vault / drop / climb links of a sector */
/* queries */
size_t path_scratch_bytes(uint32_t sector_count); int path_scratch_init(path_scratch*, void *mem, size_t bytes, uint32_t sector_count);
void path_query_default(path_query*);       /* radius 0.2, height 0.5, kinds WALK|STEP, max_drop 1.5, no blocked set, snap_dist 8 */
int  path_find(const path_bsp*, path_scratch*, const path_query*, uint32_t from, uint32_t to, path_route_info*);
int  path_sector_reachable(...); int path_reachable(...);
int  path_route(const path_bsp*, path_scratch*, const path_query*, const float from[3], const float to[3], path_waypoint *out, int max, path_route_info*);
```

**Locating.** `path_locate` looks into the grid cell of `(x, y)`, keeps the sectors whose polygon contains the point (0.1 mm tolerance) and
whose floor (plane at the point) is at most `below` (0.3) above and at most `above` (4) under it, and returns the one with the highest
floor. Stacked floors therefore resolve to the upper one. `path_nearest_sector` walks the grid in rings (bounded by `max_dist`) and takes the
smallest distance (plan distance to the polygon, then the vertical gap). `path_snap` returns a point of the sector next to the request that
keeps `margin` from every edge: a few alternating projections along the inward normals of the edges that are too close, bisection towards
the centroid as the fallback; a sector narrower than the margin returns its centroid and 0.

**A\* (`path_find`).** Nodes are sectors, edge costs are the stored integers, the heuristic is the floor of 0.9995 x the straight centroid
distance in cost units (consistent), the open list is an indexed binary heap with decrease-key (sectors are expanded once). A link is used when
its kind is in `q->kinds`, its destination is not in `q->blocked` (a bit set over sector ids; `path_block_set` helpers), its portal is at
least `2 x radius` wide, its destination has at least `height` of headroom, and a DROP is not deeper than `max_drop`. The start sector is
always allowed; a goal that is blocked or too low is unreachable. When the goal is unreachable, or the budget (`max_expand`, default
`8 x sectors + 64`) runs out, the route to the **expanded sector closest to the goal** (smallest heuristic, then cost, among sectors at least
`2 x radius` thick) is returned with `PATH_ROUTE_PARTIAL`; `path_find` returns 1 only for a complete route. Routes longer than
`PATH_ROUTE_MAX = 1024` sectors are cut and flagged.

**Route (`path_route`).** Locates both ends (`path_locate`, else the nearest sector within `snap_dist`; `NO_START` / `NO_GOAL` otherwise),
runs `path_find`, then pulls the sector route tight with the simple-stupid funnel over the portals in plan, with these radius rules:
* every portal end is pulled in along the portal by `r`; where a boundary edge of the source or destination sector leaves the end at an
  acute angle `theta` to the portal, by `r / sin theta` (capped at 4 r) so the point keeps `r` from that wall's line, not only from its
  corner; a portal narrower than the two pulls collapses to its middle;
* each corner then **slides** along its portal, away from the obstacle vertex, until both adjacent legs keep `r` from that vertex (or the
  middle is reached): a taut path through shrunk ends otherwise grazes the vertex when it arrives at a shallow angle.
Walk and step links are smoothed across; a vault / drop / climb link ends a smoothing run: the **takeoff** point (the portal middle, `kind`
set to the action) and the **landing** point (closest point of the destination sector, pulled in by `r`, `PATH_WPF_LANDING`) follow each other, the
funnel restarts at the landing. Waypoints exclude the start, the last one has `PATH_WPF_GOAL` (the goal projected on its sector, or the
best point of the end sector for a partial route), each carries its sector and the z of its sector's plane.

## Verification

`tools/tests/test_pathconv.py` (no game data): a synthetic world with 13 sectors (merged pairs, a doorway with T-junction vertices, a step, a
vault, a ledge dropped from, a ladder volume, an island, a dropped surface, a steep surface, a ceiling over one sector), the exact link
counts / kinds / portals / costs / components / headroom / thickness, merge rules (convexity, keys, bent pairs, 16-gon), chain rules (rises
0.15 / 0.45 / 2 / 5, overlap and offset limits, a thin tall wall is not stepped over), the section writer and its parser with attacks on
every directory entry, the pathfinding byte read out of the collision test's structure_bsp block (size rules, edges present, outside the
block), `verify_mesh` on deliberately wrong inputs, and with `assets/decompressed/a10.map` every BSP of a10 converted and verified
(`PATH_ALL_MAPS=1`: all 68 BSPs of the ten maps, 38 s).
`verify_mesh` checks per BSP: the pathfinding byte rules above, every walkable surface in exactly one sector, plan area conserved,
polygons convex / counter-clockwise / planar to 2 cm / at most 12 vertices, sector bounds inside the BSP's vertex bounds, walk portals are
loop vertices of both sectors with the reverse link, step / climb reverse links, vault <-> drop, costs not below the centroid distance,
component ids consistent. 0 problems on all 68 BSPs.

`engine/tests/test_path.c` (`make -C engine/tests path`, ASan/UBSan, `PATH_MAPS=all` for the ten maps):
* the synthetic world with hand-computed numbers: locate (ties, corners, slack, stacking limits), heights, links and their costs, A* costs,
  funnel corners (computed by hand, including the corner slide: the doorway's corner moves 0.0351 wu along its portal), partial routes,
  blocked sectors (the start may be blocked, the goal may not), kind masks, drop limits, headroom and width filters, radius clamping (0.45
  shrinks the doorway, 0.5 passes exactly, 0.6 does not), vault / drop / climb takeoff and landing, snapping, nearest sector, hints,
  reachability symmetry, an empty section, a short waypoint buffer;
* hostile input: every truncation and every byte of the synthetic section flipped three ways (4,560 flips, 3,134 still open, all walked with
  the whole API), plus 400 random byte flips and a truncation every 260..510 bytes of a real section;
* on every BSP of the given maps, against the collision pack of the same map:
  * sector polygons inside the collision bounds, convex, planar, `nz >= 0.7`; every link consistent (above);
  * **sectors against the collision BSP**: the source surface of every sector (`coll_surface_info`) is walkable (`nz >= 0.7099`), has the
    sector's material and breakable flag, and its polygon lies inside the sector's plan within 2 mm and within 2.5 cm of its plane;
    `coll_ray` from 0.15 above a random interior point down (sectors with at least 0.2 wu of headroom) hits a walkable surface at the
    sector's height (0.03) for 99.7% of the 33,894 samples (83 start inside solid: a ceiling closer than 0.15; 29 hit another floor at
    another height: stacked layers closer than 0.15; none miss);
  * `path_locate` of a random interior point (z + 0.05) returns that sector, or a stacked sector whose floor is within reach: 54,400
    points, 1,742 stacked, **0 wrong**;
  * `path_find` against a brute-force Dijkstra (lazy heap) over the same link table with random kind masks, drop limits, radii, heights
    and blocked sets (3% of the sectors): **36,500 random sector pairs, 0 differences** in completeness, in cost (integers, equal), and
    in the partial end sector (minimal heuristic, then cost, among the reachable sectors thick enough); every route's links exist, honour
    the mask, the blocked set and the headroom, and their costs add up to the reported cost;
  * reachability with symmetric kinds is symmetric and equals component equality (18,930 pairs, 0 asymmetric); with a drop allowed, a search
    agrees with the components inside a component;
  * **every leg of 21,078 routes** (17,145 complete, 3,933 partial; start and goal on sectors a capsule can stand on, free of walls, in the
    same component for three of four routes) checked against `coll_sphere_sweep` with a sphere of radius 0.15 resting on the floor
    (centre 0.18 above the waypoint): see the next section.

### Leg validation (ten maps, sphere radius 0.15, route radius 0.2, route height 0.5)

186,720 leg traversals (a leg is a straight segment between consecutive waypoints; action legs of vault / drop / climb are not walked and
there were none in the default walking mode). A leg is judged three ways:

| check | result |
|---|---|
| straight sweep at a constant height clear | 148,925 (79.8%) |
| straight sweep clear, or clear when lifted by the step height 0.17 (what the controller does at a step) | 172,960 (92.6%) |
| a **walker** that follows the leg like the player controller (plan steps of 0.1, step up 0.17 when blocked, settle on the ground with a sweep down) reaches the end | 181,140 (**97.0%**) |
| the same, counted over **distinct** legs (the same chokepoints are traversed by many routes) | 68,636 of 70,233 (**97.7%**) |

Per map, distinct legs followed: a10 96.4%, a30 99.5%, a50 98.8%, b30 97.7%, b40 97.5%, c10 98.8%, c20 98.8%, c40 97.2%, d20 98.5%, d40 96.0%
(the test requires 95% per map). The remaining 2.3% fall in a few groups, found by listing the failing legs:
* **throats**: two big sectors that touch at a vertex are connected only through a sliver along a wall (a door threshold with a wedge-shaped
  step on each side; the 45 degree wedge is excluded from the mesh by the 0.71 rule, so the real passage over it is not in the graph); every
  portal is wide enough, but the pinch at the shared vertex is narrower than the capsule. Portal widths cannot see a pinch between two
  consecutive portals.
* **overhanging geometry that is not above the floor of any sector**: hanging wall segments with a 0.15..0.3 wu gap under them whose
  footprint starts a few centimetres outside the sector (headroom is exact inside the sector polygon, not outside it) and the sphere top
  touches them;
* corners where the leg passes a vertex closer than 0.15 after the slide could not get far enough (portal middle reached).
All of them are places where the actor's own collision pushes it, which every path follower needs anyway; the route is a guide, not a proof.
Before the headroom filter, the width filter, the thickness rule for partial routes, the corner rules and the slide, 23% of the legs of a10
BSP 0 were blocked (plain and lifted sweeps).

### Numbers (ten maps, the pack of each map converted by `pathconv.py`; raw section KiB, budget 300 KiB per BSP, part of the 7 MiB current-BSP budget)

| Map | BSPs | KiB per BSP (index order) | Total MiB | Largest KiB |
|---|---|---|---|---|
| a10 | 9 | 184, 287, 168, 154, 260, 210, 229, 79, 97 | 1.63 | 287 |
| a30 | 2 | 584, 799 | 1.35 | **799** |
| a50 | 4 | 510, 227, 285, 231 | 1.22 | 510 |
| b30 | 2 | 503, 164 | 0.65 | 503 |
| b40 | 13 | 359, 378, 197, 84, 151, 52, 52, 106, 149, 114, 115, 148, 52 | 1.91 | 378 |
| c10 | 6 | 323, 142, 165, 133, 168, 308 | 1.21 | 323 |
| c20 | 4 | 292, 334, 348, 273 | 1.22 | 348 |
| c40 | 13 | 359, 428, 197, 86, 151, 52, 52, 106, 149, 114, 115, 148, 52 | 1.96 | 428 |
| d20 | 5 | 220, 575, 276, 340, 133 | 1.51 | 575 |
| d40 | 10 | 137, 293, 208, 148, 208, 357, 204, 347, 301, 0.2 | 2.15 | 357 |
| total | 68 | | 14.81 | |

**17 of 68 BSPs exceed the 300 KiB target** (the terrain-like BSPs: 7,976 sectors and 24,088 links for a30 BSP 1; no sector merging is possible across
triangle fans, and 84% of the links are WALK links at 12 bytes); the worst is 799 KiB, and even then BSP + COL + PAT of the same BSP stay far under 7 MiB
(a30 BSP 1: 2.89 + 1.58 + 0.78 = 5.25 MiB; the sum of the three largest sections of the game, 3.83 + 1.66 + 0.78 = 6.27 MiB, is under the 7 MiB too). On a10 every BSP is under 300 KiB (the whole mission 1.63 MiB). Totals over the 68 BSPs: 143,292
sectors (4.4 MiB), 375,383 links (4.3 MiB: 314,800 walk, 23,706 step, 12,147 vault, 24,656 drop, 74 climb), 341,957 points (3.9 MiB), 591,797
indices (1.1 MiB), grids 1.1 MiB; 25,725 components. Conversion takes 1.6 s (b30) to 5.8 s (d40) per map, 37 s for the ten. On disc the packs are 11.8 MB (the LZ4 of the pack container is modest on this data).
Possible further packing, not done: quantised points (6 B), links without cost (computed from centroids and portal, 8 B), implicit walk
links from the loop edges; together about 45%.

### Speed (host, Apple M4 Max, `-O2`, single thread; `PATH_BENCH=1`, printed by `make path`)

| Query | a10 BSP 0 (1,630 sectors) | a30 BSP 1 (7,976 sectors, the largest) |
|---|---|---|
| `path_locate` | ~2,100 per ms | ~2,400 per ms |
| `path_nearest_sector`, 3 wu off, <= 6 wu | ~200 per ms | ~270 per ms |
| `path_find`, random pairs in one component | 0.0039 ms mean (97 expansions), worst 0.029 ms | 0.082 ms mean (1,405 expansions), worst 0.38 ms (5,586) |
| `path_find` flooding a component (unreachable goal) | 0.0014 ms mean, 0.013 worst | 0.117 ms mean, 0.37 worst |
| `path_route` (locate, A*, funnel) | 0.0071 ms mean, 0.021 worst | 0.092 ms mean, 0.38 worst |

`path_open`: 0.011 ms (a10 BSP 0) to 0.046 ms (the largest BSP). About 58 ns per sector expansion on the host. The EE has no out-of-order core and
32 KB of data cache; assuming the 30..100x of docs/15 (not measured: ADR-022) an expansion costs 1.7..5.8 us. a10 BSP 0 (97 expansions on
average) then costs 0.17..0.56 ms per route: 16 actors replanning three times a second spend 8..27 ms per second (1..3%). The largest terrain BSP
(1,405 expansions on average, long routes) costs 2.4..8.1 ms per route and 0.12..0.39 s per second for the same 48 routes: too much for every
actor at 3 Hz, so give the query a budget (`max_expand = 400`: 0.7..2.3 ms on the EE, a partial route towards the goal that the actor
follows while it replans from the best point) and spread the replans over ticks. Locating is cheap everywhere (14..48 us on the EE).

## Not verified, approximations, known differences from the Xbox engine

* **There is no Xbox pathfinding data to compare with.** Sectors, links, hints, costs, widths, headroom and thickness are derived; the
  original engine's own nav behaviour (which surfaces it routes over, how it treats risers and ladders) is known only through the flag byte.
  The thresholds (0.2 step, 0.55 vault, 3.0 drop, 0.35 riser normal, 12 vertices, 2 cm planarity) are mine; the tuning for the player's
  controller is in the engine (`step_height = 0.17`), for other actors the integrator should re-check `max_drop` and `height`.
* Flag meanings of bits 6 and 7 are from the data (rule above), not from tag documentation.
* **No jump hints** (gap hops); vault / drop / climb are derived from riser geometry and ladder volumes. Ladders were found in four maps
  (a10, b40, c40, d40: 74 links in the whole game; a10 BSP 0 has 8 links from 4 ladder volumes); whether the retail game uses those volumes for AI is unknown.
* Sectors are floor patches without obstacle knowledge: props whose footprint is not a hole in the floor, hanging walls and door frames
  can sit inside or beside a sector; legs near them fail the sphere test (2.3% of the distinct legs, above). The runtime's capsule
  collision resolves them; the nav mesh does not claim a guaranteed clear path.
* Headroom and thickness are conservative summaries per sector: a big sector that is partly under a low overhang has the low headroom
  everywhere, a long strip has a small thickness; the first can hide a usable hall, the second is only used for partial route ends.
* A start sector is always allowed whatever its headroom or thickness, so reachability with a headroom filter is not symmetric when an end
  is a low sector (with `height = 0` and symmetric kinds it is, and equals component equality).
* Waypoint z is the z of the portal end on the source side: across a step the leg is not level (the controller steps; the lifted-sweep
  numbers above show what a straight sweep sees).
* Not measured on the EE or the PS2 (ADR-022); host numbers only.

## Using it

```
python3 -I tools/halo2ps2/pathconv.py assets/decompressed/a10.map -o assets/hp2/a10.path.hp2 [--bsp N]
python3 -I tools/halo2ps2/pathconv.py --info assets/hp2/a10.path.hp2
python3 -I tools/halo2ps2/pathconv.py --verify assets/decompressed/a10.map [--bsp N]    # every check, per BSP
python3 -I tools/tests/test_pathconv.py                                                   # PATH_ALL_MAPS=1 for the ten maps
make -C engine/tests path                                                                 # PATH_MAPS="a10 a30" or all
```

The engine loads `PATn` of the current BSP like `BSPn` and `COLn` (docs/07: tags `PAT0`..`PATZ`), `path_open`s it once (0.01..0.05 ms on the host) and keeps the buffer alive; the scratch is allocated once per level (73 KB + 22 B x the largest sector count of the
map). Nothing else to wire: no config keys, no tag ids exported. Typical use by the AI runtime: snap firing positions and spawns with
`path_locate` / `path_nearest_sector` + `path_snap(margin = radius)` at load, replan with `path_route` (radius = capsule radius + 0.05, height =
capsule height, `blocked` set for closed doors) and follow the waypoints with the unit controller; take the `kind` of a waypoint as the
action to perform to the next one; use `path_sector_reachable` for "can the player be reached" tests.
