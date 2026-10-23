# `SCEN` section, version 1 (placed objects, definitions, player data, object lighting)

Output of `tools/halo2ps2/scenconv.py`, read by `engine/src/world/scen.c` (contract: `engine/src/world/scen.h`).
Stored as the `SCEN` section of a [`.hp2` pack](07-hp2-format.md). Little-endian, every table 16-byte aligned,
all offsets relative to the start of the section. `scenconv.collect(map)` also returns the Xbox tag ids of the
`model` and `model_animations` tags the placed objects and the player need (`ObjectSet.model_tags`,
`.anim_tags`), so the packer can hand them to the `MODL` and `ANIM` converters; the section itself only refers to
them by tag id.

## What goes in
* **Definitions** (`scen_def`): one per object tag that is placed (or that the player data needs), sorted by
  Xbox tag id: type, model tag, animation graph tag, idle animation index, radius, name.
* **Placements** (`scen_placement`): every placement of scenery, bipeds, vehicles, equipment, weapons,
  device machines, device controls and light fixtures, in scenario order (kind by kind), with position, rotation,
  flags, scenario object name index and the BSPs it lies in. Sound scenery is counted and skipped (no model); so
  are the other palette-less things of the scenario (netgame flags, decals, encounters: not objects).
  Placements whose palette entry is deleted (`0xFFFF`) or null, or with a non-finite position, are dropped and counted
  (`ObjectSet.unresolved`; a10 has one).
* **Player** (`scen_player`): first-person hands, default weapon, player biped.
* **Cluster lights** (`scen_light`): ambient + one directional light per BSP cluster.

## Layout
Header, 128 bytes: `char[4] "SCEN"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (must equal the
section size), `u32 def count` (<= 65535, placements index them with a `u16`), `u32 placement count`, `u32 BSP
count` (<= 32), then table offsets `defs 0x18`, `placements 0x1C`, `lights 0x20`, `bsp table 0x24`, `player 0x28`
and `u32 total cluster lights 0x2C`; `u32 x 10` placements per `scen_type` at `0x38` (statistics); the rest is zero.

| Table | Element | Count |
|---|---|---|
| defs | 64 B | defs, **strictly ascending** tag id (binary search) |
| placements | 64 B | placements |
| lights | 16 B | all clusters of all BSPs, BSP after BSP |
| bsp table | 8 B (`u32 first light, u32 cluster count`) | BSPs |
| player | 80 B | 1 |

**Def, 64 B**: `u32 tag_id`, `u32 model_tag` (0 none), `u32 anim_tag` (0 none), `u8 type` (`scen_type`), `u8 pad`,
`u16 idle_anim` (`0xFFFF` none), `char[32] name` (tag path tail, NUL terminated), `f32 radius`, 12 reserved.

**Placement, 64 B**: `u16 def`, `u16 flags`, `u32 bsp_mask`, `f32 pos[3]`, `f32 rot[3]` (yaw, pitch, roll),
`char[16] variant` (always empty, see below), `s16 name_index` (-1 none), `u16 permutation` (the scenario's desired permutation, ADR-083), 12 reserved.
`flags`: bit 0 = `SCEN_P_INITIALLY_VISIBLE` (the scenario's "not placed automatically" bit is *clear*: objects
the scenario creates itself; the cleared ones are created by scripts, e.g. `object_create`); bits 1..3 are the
scenario's raw "not placed on easy / normal / hard" bits (value 2, 4, 8) and bit 4 "use player appearance" (value
16), carried as they are (`scen.h` only defines bit 0; the engine can name the others locally).

**Cluster light, 16 B**: `u8 ambient[3], u8 pad, u8 sun[3], u8 pad, s16 dir[3]` (unit vector towards the light
x 32767, so the reader needs no square root: the length is within 1e-4 of 1), 2 pad. Colours use 128 = 1.0, the
scale of the lightmap pass (a texel of 0x80 modulates by 1.0), so object and BSP brightness match.
`scen_cluster_light` returns the **neutral default** when the BSP or cluster has no entry (a BSP converted without
probes, `bsp_filter`; a cluster past the table): ambient 64, sun 64 (both 0.5), direction (0, 0, 1).

**Player, 80 B**: `u32 fp_arms_model_tag, fp_arms_anim_tag, weapon_def, weapon_fp_model_tag, weapon_fp_anim_tag,
biped_def` (`*_def` = def index + 1, 0 none), `u16 arms_idle_anim, weapon_idle_anim`, `f32 fp_offset[3]`,
`char[32] weapon_hand_marker`, `u16 weapon_loaded`, `u16 weapon_total` (the rounds the starting profile that gives the default weapon loads / carries, 0 0 = none; ADR-085), 4 reserved.

## Reader validation (`scen_open`)
Size field (`SCEN_ERR_TRUNCATED` if larger than the buffer, `SCEN_ERR_LAYOUT` if smaller), signature
(`SCEN_ERR_MAGIC`), version (`SCEN_ERR_VERSION`); counts (defs <= 65535, BSPs <= 32, bounded placements and lights);
every table 16-aligned, past the header, inside the section (64-bit products, no overflow); defs: strictly ascending
tag ids, type <= 9, NUL-terminated name, radius finite and non-negative; placements: `def` in range, `bsp_mask` only
has bits of existing BSPs, finite position and rotation, NUL-terminated variant; BSP table ranges inside the light
table; player: `weapon_def` and `biped_def` <= def count, finite offset, NUL-terminated marker. A failed open leaves
`scen_section` zeroed, so accessors on it return empty records. `engine/tests/test_scen.c` truncates at every
length, corrupts each table, each field and then every byte of the section: a corrupt section is rejected or safe
to walk completely (ASan/UBSan). Accessors with an index out of range return a neutral record
(`idle_anim = SCEN_NO_ANIM`, `name_index = -1`).

## Source data (Xbox, engine version 5; verified unless marked)
The tag layouts are in the docstring of `tools/halo2ps2/scenconv.py` (offsets for the scenario, object tags, animation
graphs, `globals`, skies). Checked on all ten campaign maps for the placement and palette strides (each stride is
the distance to the next non-empty block divided by its count), in depth on a10 / a30 / b40.

| Fact | Status |
|---|---|
| Placement blocks and palettes (scenery 72 B, bipeds 120, vehicles 120, equipment 40, weapons 92, machines 64, controls 64, light fixtures 88, sound scenery 40; palette entries 48 B holding a tag reference) | verified |
| Common placement header: `u16 type, u16 name, u16 flags, u16 permutation, f32 position[3], f32 rotation[3]` | verified (every position lies in a BSP, see below) |
| Bit 0 of the flags = "not placed automatically" (the object is created by a script) | verified (a10: 90 of 92 scenery with it set are named script objects; 177 of 182 without it are unnamed set dressing) |
| Bits 1..3 = not placed on easy / normal / hard; bit 4 = use player appearance | **unverified** (public tag names; only the values 0, 1, 3 and 8 occur on scenery) |
| `desired permutation` (+0x06) | 0 on scenery and nearly all bipeds, 100 / 101 / 10 / 12 on 5 of 107 a10 bipeds; the variant string stays empty. Carried in the placement (`+50`, ADR-083) and matched to model permutation names ending in `-<number>` (`sgt_johnson-100`, `head_marcus_cap-101`); 0 draws from the random pool `-1 .. -99`. |
| Scenery and light fixtures carry "manual BSP flags" (+0x20, bit n = BSP n) | verified (a10 scenery: 270 of 274 equal the located set, 4 are supersets) |
| Rotation: yaw about +z counter-clockwise seen from above (x forward, y left, z up) | weakly verified (light fixtures on walls are more consistent across a kind with this sign, 0.58 against 0.52; the sun of a30 comes out right with it) |
| Rotation matrix `Rz(yaw) * Ry(-pitch) * Rx(roll)`, positive pitch = nose up | **unverified** for pitch and roll (a ground-alignment test over 2265 tilted objects of all maps was inconclusive: the objects are mostly not ground aligned) |
| Object tag: type at +0x00 (scen 6, bipd 0, vehi 1, weap 2, eqip 3, mach 7, ctrl 8, lifi 9, ssce 11), bounding radius +0x04, bounding offset +0x08, model +0x28, animation graph +0x38 | verified (types and paths) |
| Weapon tag: first person model +0x45C, first person animations +0x46C | verified (assault rifle, pistol, plasma pistol on a10 and a30) |
| `globals`: player information +0x170 (default biped, `characters\cyborg\cyborg`), first person interface +0x17C (hands model, `characters\cyborg\fp\fp`) | verified by path |
| Player starting profiles (scenario +0x348, 104 B: primary weapon at +0x28, secondary at +0x3C) | verified on a10 / a30 / a50 / b30 / b40 / c10 / c20 / c40 / d20 / d40 |

The converter also checks the object type value stored in every placed object tag against its group, and notes
a mismatch in `ObjectSet.problems`; none occurs on retail maps.

### Definition radius
`radius = bounding radius + |bounding offset|` of the object tag: a sphere around the object's origin that contains
the tag's bounding sphere (the offset is non-zero for 36 of the 98 object tags of a10: e.g. a chair's is 0.5 forward).
The model's own bounds are not used (they are the model converter's business).

### Idle animation (`scen_def.idle_anim`)
Biped, vehicle and scenery defs only (devices animate by position, weapons and equipment are static pickups).
The animation graph (`antr`) has `units` (+0x0C, 100 B: label, e.g. `stand`, `crouch`, `alert`, a seat label),
each with `weapons` (+0x58, 188 B: weapon name such as `rifle`, `pistol`, `unarmed`, `fixed`; +0x98 a `u16` list of
animation indices whose entry 0 is always the idle). Rule: unit `stand` (else the first unit), weapon `unarmed`
(else every weapon in order), `animations[0]`; the first candidate whose index is valid *and whose name contains
`idle`* wins, otherwise the object is static. Verified by name on the marine / cyborg / captain / wounded marine
graphs, the a10 chairs, lifepods and the Halo ring. The name check keeps a cryotube (whose only animation is
`stand opening`) static. a10 results: 18 of 82 defs get an idle (marines: `alert pistol idle`; the cyborg:
`alert unarmed idle`; chairs, lifepods, the ring: `stand fixed idle`); the Warthog, Pelican, tubewire and doors stay
static. The animation graph tag is stored for every object that has one, idle or not.

## BSP membership (`bsp_mask`)
Point location with the BSP tree of every BSP of the map (`bsp.find_cluster`, the converter's reference
implementation), in this order:
1. **exact**: the placement position lies in a cluster of BSP n: bit n. The manual BSP flags (scenery, light fixtures)
   are added (`flags` when they are the only source).
2. **probe**: if that gives nothing, the six axis neighbours at 0.02, 0.05, 0.1, 0.25 and 0.5 wu; the first radius
   where any neighbour is inside a BSP decides (all BSPs hit at that radius).
3. **bounds**: if still nothing, every BSP whose world bounds contain the position (doors sit on BSP seams and
   inside walls: a10 has 19 such, mostly `door blast` / `door small` at z = 0.01).
4. otherwise 0: the placement is in no BSP (a10: none; a50 17, c10 12, b40 6, c40 5: doors, ceiling lights and
   boulders outside the playable space).

Verified: of the 274 a10 scenery, point location alone equals the designer's manual flags on 270 (the other four are
placed in two BSPs). The tolerance covers vehicles resting 0.02 wu below the floor.

| map | placements | dropped | defs | models | graphs | idle | exact | flags | probe | bounds | none |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 742 | 1 | 82 | 63 | 34 | 18 | 717 | 0 | 6 | 19 | 0 |
| a30 | 838 | 1 | 37 | 34 | 18 | 6 | 821 | 1 | 0 | 16 | 0 |
| a50 | 1427 | 0 | 59 | 54 | 16 | 10 | 1366 | 1 | 6 | 37 | 17 |
| b30 | 1005 | 0 | 70 | 64 | 20 | 5 | 1001 | 0 | 4 | 0 | 0 |
| b40 | 1417 | 1 | 70 | 66 | 30 | 10 | 1268 | 9 | 42 | 92 | 6 |
| c10 | 2039 | 0 | 106 | 73 | 23 | 12 | 2023 | 0 | 4 | 0 | 12 |
| c20 | 568 | 0 | 33 | 29 | 15 | 7 | 567 | 0 | 0 | 1 | 0 |
| c40 | 1178 | 1 | 76 | 71 | 31 | 9 | 1043 | 13 | 28 | 89 | 5 |
| d20 | 1340 | 0 | 61 | 51 | 16 | 8 | 1333 | 1 | 0 | 6 | 0 |
| d40 | 736 | 0 | 83 | 68 | 32 | 14 | 736 | 0 | 0 | 0 | 0 |

(models and graphs include the player's first-person set.) a10 per type: scenery 273, bipeds 107, vehicles 56,
equipment 32, weapons 29, machines 245 (+ 39 sound scenery, skipped). a10 per BSP (a placement counts in every BSP
of its mask): 121 / 154 / 55 / 84 / 135 / 97 / 84 / 56 / 99; BSP 0 (`a10a`) has 121.
`bsp_filter` of `build_section` only restricts the lighting probes; placements and masks always cover every BSP.

## Player data
* `biped_def`: the default player biped from `globals` (`characters\cyborg\cyborg`; added to the defs although the
  scenario never places it).
* `weapon_def`: the primary weapon of the **first starting profile that gives one** (else the first secondary).
  a10's profile 0 is unarmed (the player wakes up without a weapon); the pistol comes from `bridge0_profile`, so a10's
  default weapon is the pistol, and a30 / b30 / b40 / c10 / c20 / d20 / d40 the assault rifle (a50: sniper rifle,
  c40: plasma pistol).
* `fp_arms_model_tag`: the first-person hands (37 nodes, no markers). `weapon_fp_model_tag`: the weapon's first-person
  model (the assault rifle's has 5 nodes and 6 markers such as `primary trigger`; the markers are the model
  converter's business).
* **One animation graph animates hands and gun**: the weapon's first-person graph has 42 = 37 + 5 nodes whose names
  are exactly the hands' and the gun's nodes (the converter checks this and records a note otherwise).
  `fp_arms_anim_tag == weapon_fp_anim_tag`, `arms_idle_anim == weapon_idle_anim` = entry 0 of the graph's
  `first person weapons` list (`first-person idle`, verified on the pistol, assault rifle and plasma pistol).
* `weapon_hand_marker`: the hands have no markers, so this is the NODE the gun's root hangs under in the graph's node
  tree: `frame r wriste` (right wrist; every weapon). The gun root node (`frame gun` for the assault rifle and pistol)
  is a child of it; the engine attaches the weapon model's node 0 there. (The *third-person* grip is the unit weapon
  block's hold marker, `right hand` on the cyborg's model.)
* `fp_offset` is zero: the hands' root node sits at the origin of the model and no tag has a camera-space offset;
  the arms' position in front of the camera is part of the animation frames (root node translation).

Not read: per-profile ammo and grenade counts (profiles carry rounds loaded / total at +0x38 / +0x3A), the biped's
third-person weapon markers, anything about weapon sway.

## Lighting probes
Object lighting per BSP cluster (docs/03 section 5.3): one ambient colour and one directional light, from the lightmap
the BSP is drawn with. Input per BSP: its lightmap materials (`bspconv.read_materials`), the lightmap pages
(`texconv.lightmap_pages_rgba`, full resolution RGBA) and the cluster -> surface map (`bsp.Structure.cluster_surfaces`,
the union of the leaf and the subcluster routes). For every lightmapped triangle:

* area `a`, normal `n = -(e1 x e2) / |e1 x e2|` (**BSP triangles are wound clockwise seen from the front**; with the
  geometric normal the terrain of a30 faces *away* from the sun, with the negated one it faces it: verified, mean
  cosine 0.82 on outdoor clusters), colour `c` = mean of four nearest-texel lightmap samples (the three vertices and
  the centroid) x `texconv.LM_DEFAULT_GAIN`, luminance `L = 0.299 r + 0.587 g + 0.114 b`.

Per cluster (a triangle counts for every cluster that lists its surface), with `A` = sum of areas:

```
C      = (sum a c + 24 * C_bsp) / (A + 24)         # 24 wu^2 prior: tiny / black clusters inherit the BSP mean
g      = sum a (L - Lmean) n / A                   # luminance-weighted normal gradient (Lmean = area mean of L)
f      = min(0.75, 3 |g| / L(C))                   # directional fraction (3 = the sphere factor of a cosine lobe)
dir    = brightest light of the cluster's sky tag, if the cluster has a sky (see below);
         else g / |g| if |g| >= 0.5; else (0, 0, 1) and f = 0
ambient = max(20, C * (1 - f/2))     sun = C * f          (per channel, clamped 0..255, 128 = 1.0)
```
The gradient points towards where bright surfaces face, i.e. towards the light. **Sky light direction** (verified):
a sky tag's lights (`sky +0xC4`, 116 B: colour +0x50, power +0x5C, yaw +0x68, pitch +0x6C) point towards the light
along `(cos p cos y, cos p sin y, sin p)`; the brightest light (colour luminance x power; light 0 on every examined
sky) is "the sun". Outdoor clusters (`sky >= 0`, index into the scenario's sky palette, in range and with lights) use it
instead of the gradient: its agreement with the gradient of the outdoor clusters is 0.82 mean cosine on a30 (98..100 %
of the area above 0.5) and 0.91 / 0.95 on a10b / a10e (b40's snow is diffuse: 0.4). That also verifies the yaw sign of
the rotation convention. a10's sun is below the horizon (pitch -30 degrees, space), as the data says.

Numbers on a10 (ambient grey, 128 = 1.0, min / median / max): BSP 0 20 / 30 / 63, BSP 4 27 / 62 / 97, BSP 6
33 / 47 / 76; indoor clusters sit at 20..60 with a weak sun from above, the sky-facing ones brighter and lit from the
sky sun (a30 BSP 1: ambient 85..170, sun 20..76). Probes cost 2.3 s for a10 (9 BSPs, 301 clusters), 1.9 s for b40
(13 BSPs, 303 clusters). Clusters with no lightmapped surface, and BSPs without a lightmap page, get the BSP mean (or
the neutral colour) with the floor of 20. **Unverified**: that this ambient/sun split looks right on the VU1 model
program (it is a first approximation; `AMBIENT_FLOOR`, `PRIOR_AREA` and the 0.75 cap are tuning constants in
`scenconv.py`).

## Sky
`skyconv.py` writes a 64-byte `SKY0` section next to `SCEN` (object packs from `objpack.py` carry both). Layout (little-endian):
`'SKYS'`, u16 version (1), u16 reserved, u32 sky model tag id (0 = none), f32 fog rgb[3] (0..1), f32 fog near, f32 fog far (opaque
distance), f32 fog max density, f32 outdoor ambient rgb[3], f32 indoor ambient rgb[3], u32 model tag id of the scenario's second sky (+60; ADR-064), padded to 64. Sources in the scenario: the `skies`
block (+0x30) lists sky tags; the first is converted. The sky model is converted by `modelconv` (as a normal MODL entry, no placement).
The engine draws it camera-locked before the world (ADR-029). a30: ring and cloud dome (fog near 1, far 120, density 0.8);
a10: space. **Unverified**: the second a30 sky, the animation of the sky model, and the sky tag's fog values as a match for the Xbox's look.

## Tests
`tools/tests/test_scenconv.py` builds a complete synthetic scenario (palettes, placements of every converted kind,
object tags, animation graphs with unit blocks, globals, a sky, starting profiles) plus two synthetic BSPs, and checks
collection, the idle rule, every membership tier, the player variants (no weapon, secondary weapon), corrupt maps
(reflexives outside the region, NaN positions, wrong palette group), the lighting maths on hand-built rooms, the
writer and its reference parser; a skip-if-missing check converts a10. It writes the pack
`engine/tests/test_scen.c` reads (`make -C engine/tests`).
