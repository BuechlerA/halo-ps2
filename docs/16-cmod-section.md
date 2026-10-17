# `CMOD` section, version 1 (collision models of objects: hit shapes and damage regions)

Output of `tools/halo2ps2/cmodconv.py`, read by `engine/src/world/cmod.c` (contract: `engine/src/world/cmod.h`). Stored as
the `CMOD` section of `assets/hp2/<map>.cmod.hp2` ([`.hp2` pack](07-hp2-format.md)). Workstream B of
[Phase 3](14-phase3-plan.md). Little-endian, every table 16-byte aligned, all offsets relative to the start of the
section. Tests: `tools/tests/test_cmodconv.py` (synthetic map + a skip-if-missing check on a10),
`engine/tests/test_cmod.c` (reader, rays, spheres, corruption; ASan/UBSan).

The goal is **hit detection and damage regions, not physics**. The world collision of a level is workstream A's `COLn`; this
section answers "which part of this unit / vehicle / prop did the shot hit, and what does that part do to the damage".

## What goes in
* **Models** (`cmod_model`): every `collision_model` (`coll`) tag of the map, sorted by Xbox tag id: damage flags and
  constants (maximum body vitality, shield vitality and its recharge fields), the materials (damage multipliers, head
  flag), the regions (name, damage-region flags, permutations), the node tree, and the shapes.
* **Shapes** (`cmod_shape`): per (region, permutation, node, surface material) **one conservative oriented box in the
  node's own frame**, built from the model's collision geometry (below). A model has at most 65535, real ones 1 to 31.
* **Objects** (`cmod_object`): every object tag of the map (`scen bipd vehi weap eqip garb mach ctrl lifi`, also `ssce plac proj`
  where they carry one) whose collision-model reference is a `coll` tag, sorted by tag id, with the object's own bounds
  (radius, bounding offset, origin offset, render radius). `cmod_find_object(object_tag)` returns the object and its model.
  Several objects can share one model (the many cosmetic bipeds, door variants).
Not included: the collision BSPs themselves, pathfinding spheres, modifiers (empty on every model), the tag's bounding box (zero
on 155 of 157 models), the effect references (the destroyed-effect tag id of a region is kept; the rest is workstream H's).

## Layout
Header, 128 bytes: `char[4] "CMOD"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (must equal the section size),
then `u32 count x 7` at `0x0C` (models, objects, materials, regions, permutations, nodes, shapes), `u32 offset x 7` at
`0x28` (same order), `f32 margin` at `0x44`, `u32 source vertices` at `0x48`, `u32 empty bsps` at `0x4C`, the rest zero.

| Table | Element | Contents |
|---|---|---|
| models | 96 B | one per `coll` tag, **strictly ascending tag id** (binary search) |
| objects | 48 B | one per object tag with a collision model, **strictly ascending tag id** |
| materials | 48 B | the models' material tables, model after model |
| regions | 48 B | the models' region tables, model after model |
| permutations | 32 B | permutation names, region after region (`char[32]`) |
| nodes | 8 B | the models' node tables |
| shapes | 96 B | the models' shapes, sorted by (region, permutation, node, material) inside a model |

**Model, 96 B**: `u32 tag_id`, `u32 flags` (`CMOD_F_*`), `s16 indirect_damage_material` (-1 none), `u16 shield_material`,
`u16 node_count, region_count, material_count`, `u16 pad`, `u32 first_material, first_region, first_node, first_shape,
shape_count` (absolute indices; the models partition each table in order), `f32 max_body_vitality, body_system_shock,
body_destroyed_threshold, max_shield_vitality, shield_stun_time, shield_recharge_time, shield_recharge_rate`, `u32
source_vertices` (statistics), 24 reserved.

**Object, 48 B**: `u32 tag_id`, `u32 coll_tag`, `u16 model_index`, `u8 type` (the object tag's own type: biped 0, vehicle 1,
weapon 2, equipment 3, scenery 6, machine 7, control 8, light fixture 9), `u8 pad`, `f32 radius`, `f32 offset[3]`,
`f32 origin_offset[3]`, `f32 render_radius`, 4 reserved.

**Material, 48 B**: `char name[32]`, `u16 flags` (bit 0 = head), `u16 material_type` (index into the `globals` material
table: impact responses), `f32 shield_leak, shield_multiplier, body_multiplier`.

**Region, 48 B**: `char name[32]`, `u32 flags` (`CMOD_RF_*`), `f32 damage_threshold`, `u32 destroyed_effect` (Xbox effect
tag id, 0 none), `u16 first_perm, perm_count` (absolute indices into the permutation table, in order).

**Node, 8 B**: `s16 region` (-1: the node has no collision geometry), `s16 parent, first_child, next_sibling` (-1 none).

**Shape, 96 B**: `u16 node, region, perm, material`, `u16 flags` (bit 0 `CMOD_SF_HEAD`: the material has the head flag),
`u16 pad`, `u32 source_vertices`, `f32 center[3]` + pad, `f32 half[3]` + pad, `f32 axes[3][4]` (three unit rows, 4th
element 0). `perm` is the permutation index **within the region**.

## The shapes
Frame and convention: the vertices of a node's collision BSP are **node-local at the default pose** (bones point along +x). The
shape is `p` inside iff `|axes[k] . (p - center)| <= half[k]` for k = 0..2 for `p` in the node's frame. To pose it the engine
uses the node's **world matrix** (node-local to world), exactly what `skel_world` produces from the pose chain with the
object placement as root; the matrix is *not* multiplied by the inverse bind pose (that is the skin matrix of the renderer).
The collision model and the object's render `model` have the same nodes in the same order (527 of 529 object / model pairs
equal by name; the other two have the same count). `cmod_ray` takes `node_world[model.node_count]`.

Fit: for every (node, permutation, material) the vertices of the surfaces that carry the material (the polygons come from
the tag's edge rings) are boxed. Two candidates in float64: the axis-aligned box, and the box on the principal axes of the vertex
cloud (3x3 Jacobi eigen decomposition); the principal box is taken only when its volume is under 98 % of the axis-aligned one
(86 of 234 shapes on a10, 174 of 406 on c40). Axes, centre and half extents are rounded to float32 first and the half extents
are re-measured against the rounded numbers, rounded **up** and inflated by `margin` = 0.002 wu (6 mm, stored in the header).
Splitting by material keeps the head flag and the damage multipliers right: a head node's box is `material = head`, the odd
surface of another material (a shield generator on a spine, a hub on a tyre: 72 of 624 node bsps have two or three) gets its own
small box. Overlapping boxes of one node are fine: a ray reports the nearest entry.

## Reader (`world/cmod.h`)
```c
cmod_status cmod_open(cmod_section *s, const uint8_t *data, size_t size);
int cmod_find(const cmod_section *s, uint32_t coll_tag_id, cmod_model *out);               /* by collision model tag id */
int cmod_find_object(const cmod_section *s, uint32_t object_tag_id, cmod_object *obj, cmod_model *model);   /* by object tag id */
cmod_material cmod_get_material(const cmod_model*, uint32_t); cmod_region cmod_get_region(...); cmod_node cmod_get_node(...);
cmod_shape cmod_get_shape(...); int cmod_perm_name(...); int cmod_find_permutation(const cmod_model*, uint32_t region, const char *name);
int cmod_ray(const cmod_model *m, const mat4 *node_world, const cmod_segment *seg, uint16_t *out_region, float *out_t);
int cmod_ray_ex(const cmod_model *m, const mat4 *node_world, const cmod_segment *seg, const uint8_t *perm_sel, cmod_hit *hit);
uint32_t cmod_sphere(const cmod_model *m, const mat4 *node_world, const float c[3], float radius, const uint8_t *perm_sel, uint64_t *regions);
```
* `cmod_ray` is the contract form: world-space segment, permutation 0 in every region, nearest hit's region and `t` (0..1).
  `cmod_ray_ex` takes `perm_sel[region]` (NULL = all 0; a value past the region's permutations counts as 0) and fills
  `cmod_hit`: `t`, region, node, permutation, material, shape flags, **region flags and the material's body / shield multipliers
  and shield leak**, so the damage code needs no second lookup. A segment that starts inside a shape hits at `t = 0`. A
  non-finite segment or a degenerate matrix (a row of length 0) hits nothing / drops the node's shapes; rigid or uniformly
  scaled matrices work (the transform divides by the row lengths).
* `cmod_sphere`: area damage: the regions that have a shape within `radius` of a world point as a `uint64_t` mask (bit r =
  region r, at most 64 regions).
* **Permutations do not correspond** between the collision model and the render model: the collision model has "__base" and
  one or two damage states ("~damaged", "~shield_off", "~blur") per region, the render model many cosmetic ones (heads,
  helmets). To pick the collision permutation for a placement: for each region, `cmod_find_permutation(model, region,
  name_of_the_active_render_permutation)`, else 0.
* Accessors with an index out of range return a zeroed record (`region = -1` for nodes); `cmod_open` of a corrupt section
  fails and leaves `cmod_section` zeroed, after which `cmod_find` and friends report "not found".
* Pure C11, no heap, no `double`, records decoded with byte loads (no alignment assumptions). The ray and sphere tests need
  `core/mat.h` for the matrix type only (no link dependency).

## What `cmod_open` checks
Size (`CMOD_ERR_TRUNCATED` when larger than the buffer, `CMOD_ERR_LAYOUT` when smaller), signature (`CMOD_ERR_MAGIC`), version
(`CMOD_ERR_VERSION`); every table 16-aligned, past the header and inside the section (64-bit products); counts (models <= 65535,
regions and permutations u16); names NUL-terminated; every float finite, radii and half extents non-negative, shape axes of
length 1 +- 0.5 %; per model <= 64 nodes, <= 64 regions, <= 256 materials, <= 65535 shapes, the models strictly ascending
and partitioning the material / region / node / shape / permutation tables exactly in order, node regions and links in range,
every shape's node / region / material in range, its region equal to its node's region and its permutation below the region's
permutation count; objects strictly ascending, `model_index` in range and naming a model with the object's `coll_tag`.
`test_cmod.c` truncates at every length, corrupts the header, and flips every byte three ways (1116 rejected, 5988 opened and
walked with rays and spheres under ASan/UBSan); the Python twin is `cmodconv.parse_section`.

## Source data: the Xbox `collision_model` tag (engine version 5; verified unless marked)
All layouts are in the docstring of `cmodconv.py` (offsets, strides, every fact with its evidence). Checked on the **157 distinct
collision models of the ten campaign maps** (862 nodes, 652 node bsps, 16642 bsp vertices, 21678 surfaces).

| Fact | Status |
|---|---|
| Header 0x298 B; reflexives +0x234 materials (72 B), +0x240 regions (84 B), +0x24C modifiers (empty), +0x280 pathfinding spheres (32 B), +0x28C nodes (64 B); permutation 32 B; node bsps 0x60 B with eight reflexives of stride 12 / 16 / 8 / 8 / 20 / 12 / 24 / 16 | verified (the arrays follow each other with exactly these strides) |
| Object tags keep the collision model at **+0x70** (all groups; 52 vehicle, 140 biped, 71 equipment, 68 weapon, 73 machine, 11 control, 119 scenery, 26 garbage, 2 light fixture tags, counted per map); bounds: radius +0x04, bounding offset +0x08, origin offset +0x14, render radius +0x104 | verified (reference targets are all `coll`; the render radius equals +0x04 on 383 of 389 tags) |
| A node's bsp count equals the permutation count of its region (0 for region -1); one bsp per permutation, in order | verified (862 of 862 nodes) |
| Node tree (parent / first child / next sibling) is consistent | verified (705 of 705 nodes with a parent) |
| Node names / order equal the render model's | verified (527 of 529 pairs; 2 doors differ in names, same count) |
| **BSP vertices are in the node's own frame at the default pose** | verified: render vertices (dominant node) transformed into node-local space by the inverse default-pose chain give boxes within centimetres of the collision boxes on every node of the cyborg (e.g. head x -0.006 .. 0.095 against -0.028 .. 0.096), and an engine-side check (`skel_world` on the MODL default pose, rays through the cyborg's head) hits `head` at z = 0.707 wu, the top of a 2.1 m figure |
| Surface polygons close into edge rings; every vertex is on a ring; every surface material is in the table | verified (all 21678 surfaces) |
| Surface flags: 0 on 21648, two-sided on 26, two-sided + invisible on 4 (all geometry is kept) | verified |
| Material: head flag in bit 0 (only the "head" materials of characters); body multiplier +0x3C, shield multiplier +0x2C (0 = not authored on every prop and vehicle; 1.0 / 0.8 on characters: the cyborg's "legs" is 0.8) | strides and names verified; the positions of the two multipliers inferred from names and values |
| Region flags (`CMOD_RF_*`): bit values agree with the region names: head 0x2 (forces death) / 0x200 (head-maimed scream), shield regions 0x10 (disappears when shield is off), legs 0x80 (inhibits walking), left arm 0x20, right arm 0x140, vehicles 0x4 | names from the public tag documentation, plausibility checked against region names |
| Region damage threshold at +0x28 (0.1 .. 0.5 on flood regions, 0 elsewhere) | position by its values; **unverified** meaning |
| Model flags (`CMOD_F_*`) | names from the public documentation; **unverified**; only 0, 1, 3, 7, 8, 0x20, 0x28, 0x42, 0x48 occur |
| Max body vitality +0x08 (crewman 25, grunt 40, marine 50, captain 60, cyborg 75, elite 100, hunter 200, wraith 450 ...; 0 on 84 models) and max shield vitality +0xCC (cyborg 75, marine 50, elite and jackal 100, sentinel 80, monitor 1e10) | verified by values against the characters |
| Shield recharge time +0x110 seconds and rate +0x1C0 per tick | verified: rate = 1 / (30 x time) on all 6 models that have a shield; `shield_recharge_rate` is 1.0 where there is none |
| Shield stun time +0x10C (cyborg 6, marine 8, elite 12, jackal 15), shield material +0xD2, body destroyed threshold +0x94, body system shock +0x0C | inferred from values and the public documentation; **unverified** |
| The Warthog and most props have vitality 0 in the collision model (vehicle health lives elsewhere or is a default) | verified as a fact of the data; where the engine finds it is for workstream C / the integrator |

## Measured on the real maps
Section sizes (raw `CMOD` bytes, the `.hp2` is LZ4 compressed and sector aligned) and counts; conversion takes 0.1 s per map.

| map | models | objects | shapes | section | pack | source vertices |
|---|---:|---:|---:|---:|---:|---:|
| a10 | 47 | 58 | 234 | 40.2 KiB | 18 KiB | 4937 |
| a30 | 33 | 39 | 221 | 35.2 KiB | 18 KiB | 4330 |
| a50 | 49 | 56 | 269 | 44.2 KiB | 22 KiB | 5325 |
| b30 | 49 | 53 | 288 | 46.0 KiB | 22 KiB | 4662 |
| b40 | 54 | 60 | 345 | 53.6 KiB | 24 KiB | 6572 |
| c10 | 52 | 60 | 298 | 48.6 KiB | 24 KiB | 5301 |
| c20 | 34 | 36 | 150 | 26.6 KiB | 16 KiB | 3194 |
| **c40** | 61 | 68 | 406 | **62.8 KiB** | 30 KiB | 7535 |
| d20 | 49 | 57 | 292 | 46.9 KiB | 24 KiB | 5007 |
| d40 | 62 | 75 | 318 | 53.2 KiB | 26 KiB | 5554 |

`cmodconv.py --measure <map>` re-reads the written section (so after float32 rounding) and compares it with the data:

| Measure | a10 | all ten maps |
|---|---|---|
| **collision vertices inside their (node, permutation, material) box** | 4937 of 4937 (100.000 %) | **100.000 % on every map** (52 417 vertices) |
| box volume / axis-aligned box volume (median, min, mean) | 1.000, 0.306, 0.931 | min 0.105, mean 0.855 .. 0.931 |
| shapes on principal axes | 86 of 234 | 68 .. 174 per map |
| **render-model vertices** (dominant node, first permutation of each region, best LOD) inside a shape of their own node | 83.7 % of 61105 | 64.5 % (c20) .. 86.1 % (b30) |
| render vertices inside *some* shape of the model | 90.8 % | 85.2 % (c20) .. 96.4 % (a30) |
| render vertices whose node has no shape (clavicles, necks, shield and weapon nodes) | 7.2 % | 4.8 % .. 26.8 % |
| outside every shape: distance to the nearest box, median / p95 / max (wu) | 0.018 / 1.1 / 2.0 | median 0.005 .. 0.04; p95 0.09 .. 1.1 (30 on c20: two big platform devices) |
| object bounding sphere (tag radius around the bounding offset) contains all collision vertices at the default pose | 34 of 58 objects, 80.7 % of the vertices | 50.9 .. 84.4 % of the vertices per map |

Reading the numbers: the boxes are **conservative against the collision geometry** (100 %, the guarantee) but the collision
geometry is itself simpler than the render mesh: armour plates, visors, rotors and weapon parts stick out of it, so 9 % of the
render vertices of a10 are outside every box (by 2 cm at the median) and a shot at such a part passes through, exactly as in
the original game's collision model. Characters' clavicles and necks have no collision geometry at all: shots there hit the
neighbouring nodes' boxes. **The tag's bounding radius is not a bounding sphere of the collision geometry**: for characters it
is the collision-pill radius (0.45 wu around the feet, so the head at 0.6 wu is outside); do not use it for early-out of
`cmod_ray`. A box test per shape (at most 31 per model on the retail maps: the Warthog; 16 for a marine, 17 for the Master Chief) is cheap enough; an
engine that wants an early-out can use its own conservative sphere (for example the model's MODL `centre / radius`).

## What is unverified or approximated
* The meaning of `shield_stun_time`, `shield_material`, `body_destroyed_threshold`, `body_system_shock`, region
  `damage_threshold` and the model flags (see the table); they are carried as stored, named from the public documentation.
* Whether the material multipliers at +0x2C / +0x3C are what the original damage code uses (they are 0 on vehicles; the damage
  code decides what 0 means, 1.0 is the natural reading).
* The boxes are node-local and rigid: Halo's own collision model is a BSP per node, so concave parts (a chair's back, a hull
  with an opening) are filled in; shots can hit empty corners of a box.
* Pitch / roll conventions of object placement are SCEN's business (unverified there); `cmod_ray` takes whatever matrices the
  skeleton gives it.
* The `objects` table lists every object tag of the map with a collision model, not only the placed ones (a10: 58 tags,
  of which 13 bipeds, 9 vehicles).

## Wiring for the integrator
* Build: `python3 -I tools/halo2ps2/cmodconv.py assets/decompressed/<map>.map -o assets/hp2/<map>.cmod.hp2` (all ten maps convert,
  no problems reported); `scripts/build-world.sh` is the integrator's. The pack needs no other pack to be read; it refers to
  the object tag ids of `SCEN` (`scen_def.tag_id`) and has the same node order as `MODL`.
* Per placed object: `cmod_find_object(sec, scen_def.tag_id, &obj, &model)`; each tick build `node_world[]` with `skel_world`
  (the same matrices that feed `skel_palette_entry` before the inverse bind is applied) and call `cmod_ray_ex`.
* Nothing else is exported (no model or animation tag ids are needed from this converter).
