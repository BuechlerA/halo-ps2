# `DEVS` section: devices, groups, trigger volumes and scenario markup, format version 1

Workstream D of [phase 4](23-phase4-plan.md). Converter `tools/halo2ps2/devconv.py` (writer, reference parser, verification
helpers), reader `engine/src/world/devdef.[ch]`, pure device runtime `engine/src/sim/devices.[ch]`, tests
`tools/tests/test_devconv.py`, `engine/tests/test_devdef.c`, `engine/tests/test_devices.c`. `devconv.py <map> -o
assets/hp2/<map>.dev.hp2` writes a pack with one section, `DEVS` (see [07-hp2-format.md](07-hp2-format.md)). All integers little
endian, every table 16-byte aligned, offsets from the section start, times in **ticks of 1/30 s** (the tags hold seconds;
the converter multiplies by 30 and rounds), cross references are Xbox tag ids. Names of the game content are not
reproduced here: layouts and aggregate statistics only.

What the section holds, and who uses it:

| Table | What | Used by |
|---|---|---|
| device tags (defs) | one per `device_machine` / `device_control` / `device_light_fixture` tag that is placed: times, flags, radius, effect and sound references, animation indices | `sim/devices` |
| devices | every placed machine, control and light fixture: position, rotation, groups, flags, SCEN index | `sim/devices`, object spawn |
| device groups | name, initial value, flags | `sim/devices`, scripts |
| trigger volumes | name, oriented box | scripts (`volume_test_objects`), level manager |
| player starting locations | position, facing, team, game type, BSPs it lies in | game start, respawn |
| cutscene flags, cutscene camera points | name, position, orientation (field of view) | cinematics (workstream C), scripts |
| object names | name -> (type, placement) -> SCEN placement / device / sound scenery | script machine (`object_create`, `device_*`) |
| sound scenery | looping sound placements | audio (ambience) |
| decals | placements and the decal palette | effects |
| BSP switch volumes | trigger volume, source BSP, destination BSP | level manager |

## 1. Source data (Xbox, engine version 5)

Layout facts below are checked on all ten campaign maps (a10 a30 a50 b30 b40 c10 c20 c40 d20 d40) by an assertion in
`devconv.py` / `test_devconv.py` or by the statistics named; "unverified" fields are read as the data has them and carried raw.
The full docstring of `devconv.py` lists every offset; this is the summary.

### 1.1 Device tags (`mach`, `ctrl`, `lifi`)

The object part is the one `scenconv` reads (type +0x00, model +0x28, animation graph +0x38) and ends at +0x17C. The device
part follows (verified on the 120 machine, 17 control and 19 light fixture tags of the ten maps; struct sizes 0x324, 0x318
and 0x2D0, the rest of a tag block is attachment and function sub-blocks):

| Field | Offset | Notes |
|---|---|---|
| flags | +0x17C u32 | bit 0 position loops, bit 1 position not interpolated; only 0..2 seen |
| times | +0x180 6 x f32 seconds | power transition, power acceleration, position transition, position acceleration, depowered position transition, depowered acceleration. **verified**: the engine's own derived rates at +0x274..+0x28C equal `1 / (30 t)` of these (so ticks = 30 t), and a machine's +0x320 equals `round(30 x door open time)` on all 120 machines |
| function inputs A B C D | +0x198 4 x u16 | enum 0 none, 1 power, 2 change in power, 3 position, 4 change in position, 5 locked, 6 delay (values 0..6 seen; the names are **unverified**) |
| effects / sounds | +0x1A0 6 x tag reference | open (up), close (down), opened, closed, depowered, repowered. **verified**: every non-null reference is group `snd!` or `effe` |
| delay time, delay effect | +0x200 f32, +0x20C reference | |
| activation radius | +0x21C f32 | world units; **non-zero only on door-type machines** (verified on every tag): radius 0 means the door is driven by scripts / groups, not by proximity |
| machine | +0x290 u16 type (0 door, 1 platform, 2 gear), +0x292 u16 flags, +0x294 f32 door open time (s), +0x2E8 u16 collision response, +0x2EA u16 elevator node | the flag bit 2 occurs only on platforms (= elevator); names of bits 0 and 1 (pathfinding obstacle, but not when open) are unverified |
| control | +0x290 u16 type (0 toggle, 1 on, 2 off, 3 call), +0x292 u16 triggers-when, +0x294 f32 call value, +0x2E8 / +0x2F8 / +0x308 references (on, off, deny) | **all 17 control tags hold type 0 and call value 0**: only the zero is verified; the other values follow the public tag documentation |
| light fixture | nothing beyond the device part | the light is an attachment (a `ligh` tag at a marker) in the attachments block; not read here |
| animation graph | `antr` +0x30 devices (96 B), +0x54 reflexive of i16 slots | slot 0 = position: a one-animation **overlay** of 2..n frames (verified by animation type and name on 88 machine and 5 control graphs); slot 1 = power, never present on the maps. Position `p` plays frame `p x (frames - 1)` |

### 1.2 Scenario blocks

Strides are verified as (distance to the next non-empty block) / count on all ten maps.

| Block | Offset | Element | Fields (verified unless marked) |
|---|---|---|---|
| object names | +0x204 | 36 B | char[32] name, u16 object type, u16 index into that type's placement block |
| device groups | +0x288 | 52 B | char[32] name, f32 initial value (0..1), u32 flags (bit 0 "can change only once"; 0 / 1 seen) |
| machines, controls, light fixtures | +0x294 / +0x2AC / +0x2C4 | 64 / 64 / 88 B | placement header as in scenconv; +0x28 i16 power group, +0x2A i16 position group (-1 none; **always < the group count**), +0x2C u32 device flags (bit 0 initially open, 1 initially off, 2 can change only once, 3 position reversed, 4 usable from both sides; only bits 0..4 occur), machines +0x30 u32 flags (bit 0 does not operate automatically, 1 one-sided, 2 never appears locked, 3 opened by melee; only 0..8 occur, **names unverified**), controls +0x30 u32 (0 / 1) and +0x34 u16 (0..4) **meaning unknown, carried raw**, light fixtures +0x30 colour, +0x3C intensity, +0x40 falloff angle, +0x44 cutoff angle |
| sound scenery | +0x2DC | 40 B | placement header, +0x20 8 bytes (all zero); the palette tag (type 11) references a `sound_looping` tag at +0x1FC (all 51 do) |
| player starting locations | +0x354 | 52 B | f32 position[3], f32 facing (yaw), i16 team, i16 BSP index, i16 game type x 4 |
| trigger volumes | +0x360 | 96 B | u32 (always 1), char[32] name at +4, f32 forward[3] +0x30, up[3] +0x3C, position[3] +0x48, extents[3] +0x54 |
| BSP switch volumes | +0x39C | 8 B | u16 trigger volume, u16 source BSP, u16 destination BSP, u16 unknown (0xFFFF or small, carried raw) |
| decals, decal palette | +0x3A8, +0x3B4 | 16 B | u16 palette index, i8 yaw, i8 pitch, f32 position[3]; palette entry = `deca` reference |
| cutscene flags | +0x4E4 | 92 B | u32 0, char[32] name at +4, f32 position[3] +0x24, f32 yaw, pitch +0x30 |
| cutscene camera points | +0x4F0 | 104 B | u32 0, char[32] name at +4, u32 0, f32 position[3] +0x28, f32 yaw, pitch, roll +0x34, f32 field of view +0x40 |

Checks behind the table, all on the ten maps: group indices of every placement valid (1 596 machines, 77 controls, 840
fixtures); times and ticks as above; the device palette tags are of the right group and object type; trigger volume `forward`
and `up` are unit and orthogonal on all 1 060 volumes and the extents positive; 1 012 of 1 025 cutscene flags and 926 of 962
camera points are exactly inside a BSP (of the rest: 9 / 11 within 0.5 wu, 3 / 17 inside a BSP's bounds, 1 / 8 outside every BSP: seams
and the unplayable space), all 20 starting locations are inside a BSP; camera fields of view are 70 degrees on 959 of 962 points.

## 2. `DEVS` layout

Header, 128 B: `"DEVS"`, `u16 version (1)`, `u16 0`, `u32 total size` (must equal the section size), twelve `u32` counts at
0x0C (defs, devices, groups, volumes, starts, cutscene flags, cameras, names, ambients, decals, decal types, BSP switches), `u32
BSP count` at 0x3C (<= 32), thirteen `u32` table offsets at 0x40 (the same order with the name order table after the names), `u32 dropped
devices` 0x74, `u32 dangling names` 0x78, zero.

| Table | Element | Order / key |
|---|---|---|
| defs | 160 B | **strictly ascending** tag id (binary search) |
| devices | 80 B | scenario order: machines, then controls, then light fixtures; **strictly ascending `scen_index`** (binary search) |
| groups | 48 B | scenario order (scripts index them) |
| volumes | 96 B | scenario order (scripts index them) |
| starts | 32 B | scenario order (the first is the single player's) |
| cutscene flags, cameras | 64 B each | scenario order |
| names | 48 B | scenario order = object name index; `name_order` (u16 each) lists them sorted by name for binary search |
| ambients (sound scenery) | 48 B | scenario order |
| decals | 32 B | scenario order; decal types: u32 tag id per palette entry |
| BSP switches | 8 B | scenario order |

**def, 160 B**: `u32 tag, model_tag, anim_tag`, `u8 kind (0 machine, 1 control, 2 light fixture)`, `u8 subtype`, `u16 dev_flags`,
`u16 kind_flags`, `u16 triggers`, `u32 ticks x 6` (power, power acceleration, position, position acceleration, depowered,
depowered acceleration), `f32 auto_radius`, `u32 delay_ticks`, `u32 open_ticks`, `u8 fn_in x 4`, `u32 fx x 10` (open, close, opened,
closed, depowered, repowered, delay, control on, off, deny; 0 = none), `u16 fx_snd_mask` (bit i: `fx[i]` is a `snd!` tag, else an
`effe` tag), `u16 0`, `u16 pos_anim, power_anim` (0xFFFF none), `u16 pos_frames, power_frames`, `u8 collision, elevator_node`,
`u16 0`, `f32 call_value`, `char[32] name` (tag path tail, debugging), 8 reserved.

**device, 80 B**: `u16 def`, `u16 flags` (SCEN placement flags: bit 0 initially visible = *not* "not placed automatically", bits 1..3
not on easy / normal / hard, bit 4 player appearance: the per-difficulty / appearance flags), `u32 bsp_mask` (point location,
plus the light fixture's manual BSP flags), `f32 pos[3]`, `f32 rot[3]` (yaw, pitch, roll as in scenconv), `i16 power_group,
position_group`, `u32 device_flags`, `u32 kind_flags`, `i16 name_index`, `u16 extra`, `u32 scen_index`, `f32 colour[3],
intensity, falloff, cutoff` (light fixtures), 4 reserved.

**volume, 96 B**: `char[32] name`, `f32 forward[3], left[3] (= up x forward, derived), up[3], position[3], extents[3]`, 4 reserved.
A point `p` is inside when `0 <= dot(p - position, axis_i) <= extents[i]` for `i` = forward, left, up (inclusive). Invalid source
volumes (non-finite, negative extents: none on the maps) become empty boxes so that script indices stay stable.

**start, 32 B**: `f32 pos[3], facing`, `i16 team, bsp, game_type x 4`, `u32 bsp_mask` (**located**: the `bsp` field is 0 on every
map, also where the location lies only in another BSP). **cflag, 64 B**: `char[32] name`, `f32 pos[3], yaw, pitch`, `u32 bsp_mask`, 8
reserved. **camera, 64 B**: `char[32] name`, `f32 pos[3]`, `f32 yaw, pitch, roll`, `f32 fov` (radians), `u32 bsp_mask`.

**name, 48 B**: `char[32] name`, `u8 object type` (tag enum: 0 biped 1 vehicle 2 weapon 3 equipment 6 scenery 7 machine 8 control 9 light
fixture 11 sound scenery), `u8 scen_type` (`scen_type` of world/scen.h), `u8 flags` (bit 0 a placement carries this name, bit 1 it is a
device, bit 2 it is sound scenery), `u8 0`, `u16 table_index` (the tag's own index into the placement block of its kind), `u16 0`,
`u32 scen_index`, `u32 aux_index` (device index / ambient index; both 0xFFFFFFFF when none).
**ambient, 48 B**: `u32 ssce tag, looping sound tag`, `f32 pos[3], rot[3]`, `i16 name_index`, `u16 flags`, `u32 bsp_mask`, `u32 raw x 2`.
**decal, 32 B**: `u16 type`, `i8 yaw_raw, pitch_raw`, `f32 pos[3]`, `f32 yaw, pitch` (raw x 2 pi / 256), `u32 bsp_mask`, 4 reserved.

## 3. Script names -> placements (the object name table)

Scripts address placed objects by name (`object_create`, `device_*`, `volume_test_objects` ...). The scenario's name table
holds `(name, type, index in the scenario's placement block of that kind)`. SCEN does not keep the scenario's indices
(`scenconv` drops placements with a deleted palette entry or a non-finite transform and orders kind by kind), so DEVS carries
the mapping: for every name, the **SCEN placement index** (`scen_index`) computed by replaying scenconv's loop (the converter
cross-checks the count and, for every device, kind, position and tag against the real `scenconv.collect_objects` output).
Verified on all ten maps: a name's table entry and the placement carrying the name (`placement.name` field) agree for every
carried name (1 964 of 1 964), no name is carried twice, and 112 names are **dangling**: no placement carries them, their table
index points at some other placement (aliases / stale entries, 3 .. 49 per map). Dangling names keep `flags` bit 0 clear and
`scen_index` / `aux_index` 0xFFFFFFFF; the script machine should treat them as "object does not exist yet".
Names are unique per map. Lookup: `devdef_find_name` (binary search over `name_order`), then `scen_index` for the object table
(`scen_get_placement`), `aux_index` / `devdef_name_to_device` for a device. A placement that is "not placed automatically"
(`flags` bit 0 clear in SCEN) is exactly the kind a script creates by name.

## 4. Reader (`devdef.h`)

`devdef_open(s, data, size)` validates once: size, signature, version, counts (<= 2^20 per table, defs <= 65535, BSPs <= 32),
every table 16-aligned, past the header and inside the section (64-bit products), defs strictly ascending, kind / subtype ranges,
finite and non-negative radius, `fx_snd_mask` only on non-null refs, strings NUL terminated, devices: def in range, BSP mask only
existing BSPs, finite transform, group indices -1 or < the group count, name index in range, `scen_index` strictly ascending,
groups: initial value finite in 0..1, volumes: finite with extents >= 0, name table flags consistent with their indices, name order
a valid sorted permutation, decal types inside the palette, BSP switch volumes inside the volume table. A failed open leaves
`devdef_section` zeroed (accessors return neutral records). Accessors: `devdef_get_def / device / group / volume / start /
cflag / camera / name / ambient / decal / bspsw`, `devdef_decal_type`, `devdef_find_def` (binary), `devdef_find_device_by_scen`
(binary), `devdef_find_name` (binary), `devdef_find_group / volume / cflag / camera` (linear), `devdef_name_to_device /
name_to_scen`, `devdef_find_bspsw_by_volume`.

## 5. Device runtime (`devices.h`)

Pure C, no heap, no `double`, no dependency on the engine: the host feeds activators and control uses in, reads events out.
`devices_t` is plain data (about 70 KB for 640 devices; the busiest retail map places 497): `memcpy` is a checkpoint snapshot,
`devices_rebind` re-attaches the section. One `devices_tick` costs about 1 us on the host for the largest map (497 devices, 4
activators, a group change per tick), so well below 0.1 ms on the EE.

**State** per device: position and power in Q32 fixed point (0 .. 2^32), targets, a door hold timer, cached tag values (ticks,
radius, flags, origin, front direction). Fixed point makes a transition of T ticks last **exactly T ticks** (step = ceil(2^32 / T),
last step clamped, exact up to T = 65535), which the tests check against every real device.

**Rules** (30 Hz; the numbers are the tag's):
* **Power** follows its power group's value (or `devices_set_power`) with the power transition time. A device without a power
  group is powered unless "initially off". Events: `DEPOWERED` when it reaches 0, `REPOWERED` when it leaves 0.
* **Position** follows its position group's value (`1 - value` if "position reversed"), a script's `devices_set_position`, or, for
  doors, its own logic, linearly with the position transition time (a full change is `position_ticks` ticks, a partial one
  proportionally; a reversal mid-way is allowed). Events: `OPENING` / `CLOSING` when it starts to rise / fall, `OPENED` / `CLOSED`
  when it reaches 1 / 0. Initial position: "initially open" -> 1, else 0, overridden by the position group's initial value.
* **Power loss**: an unpowered device does not follow groups or activators (it stays where it is); when power DROPS to 0 and the
  tag has a depowered time, it drifts to position 0 once, in exactly that time. A position a script asked for moves regardless
  of power (so a script can drive an unpowered door).
* **Doors** (machine type door, radius > 0, placement not "does not operate automatically"): the host reports activator positions
  (`devices_set_activators`, called every tick before `devices_tick`); one within the radius (3-D sphere around the device origin;
  one-sided doors only for activators in front, `dot(offset, (cos yaw, sin yaw)) >= 0`) raises the target to 1, emits `ACTIVATED`
  on the first tick, and refreshes the hold timer to `open_ticks`; the door closes (target back to its group's value or 0)
  **`open_ticks + 1` ticks after the last tick with an activator in reach**. "Opened by melee" doors (`devices_melee`) behave the
  same without a radius. A door whose placement has a position group still follows it afterwards.
* **Controls** (`devices_use(device, user_pos)`, called when the host decides the control is within reach): needs power
  (`DEV_USE_NO_POWER`, `DENIED` event), the user in front of the control unless "usable from both sides"
  (`DEV_USE_WRONG_SIDE`), not locked (`DEV_USE_LOCKED`, `DENIED`). Then it writes its **position group**: toggle flips it, on sets 1, off
  sets 0, call sets the tag's call value (or 1); the machines sharing the group follow in the same tick; `ACTIVATED` carries the new
  value. A control without a position group just moves itself.
* **Can change only once**: the device flag locks a device after its first completed move (further targets are ignored); the group
  flag refuses a second change of the group's value.
* Script interface: `devices_set_position`, `_set_position_immediate`, `_set_power`, `_set_power_immediate`, `devices_group_set / get`,
  `devices_position / power / position_target / moving / changed / anim_frame` (the animation frame for the position overlay).
* **Events** go to a 256-entry ring (`devices_pop_event`; overflow is counted in `events_dropped`); `devices_event_tag(ev)` returns the
  sound or effect tag the def attaches to an event (open / close / opened / closed / depowered / repowered, control on / off /
  deny) so the host plays it.
* **Not modelled**: object collision of doors, platforms and gears (they never push, block or crush anything: documented
  limitation of this phase), the acceleration times (carried in the def for easing), position-loop and not-interpolated flags,
  device function outputs for shaders, the delay time / delay effect, the pathfinding obstacle flag (the AI workstream reads
  it from the machine flags if it wants it).

**Trigger volumes**: `volume_contains(volume, point)` (NaN is outside), `volume_count_inside / any_inside / all_inside(volume,
points[][3], n)` for an object list (all of an empty list is false, as the script function behaves), `volume_center`. They test
**object origins**; a sphere or bounding-box variant is a host decision.

## 6. Conventions the data does not state, and how they were settled

* **Trigger volume box.** `position` is the box's minimum corner and the extents run along `forward`, `left = up x forward` and
  `up`. Test (`devconv.py --verify`, run by `test_devconv.py` on a10): the BSP switch volumes straddle the seam of two BSPs, so
  random points inside them must lie inside the source and destination BSP. Fraction of 30 points per volume inside a BSP
  (a10): corner / `up x forward` 0.64 (c10 0.47), corner / mirrored left 0.33 (0.18), centred 0.20 and 0.17 (c10 0.23 and 0.21). The mirrored
  and centred conventions are clearly worse on every map checked.
* **Starting locations.** The tag's BSP index is 0 everywhere, but the locations of b40 (BSP 3), c40 (BSP 2), a10, c20, d20 and d40
  lie in other or additional BSPs, so the section carries the located `bsp_mask`: **the initial BSP of a map is a located one**, not
  BSP 0. First start of each map: a10 {0, 5}, a30 {0}, a50 {0}, b30 {0}, b40 {3}, c10 {0}, c20 {0, 1, 2, 3}, c40 {2}, d20 {0, 4},
  d40 {0, 9}. For masks with several bits the lowest bit is the natural guess; **unverified**.
* **Decals.** The direction `(cos p cos y, cos p sin y, sin p)` with angles = signed byte x 2 pi / 256 is compared with the normal of the
  nearest rendered BSP triangle (`--verify`): on a10 the median |dot| is 0.98 (1.00 without the most common raw pair, which is an
  editor default carrying no direction: 36 % of a10's decals), 52 % are within 18 degrees of the surface normal. On c10 and d40 the
  median is 0.96 / 0.76. Designers may project obliquely, and the sign of the pitch is not consistent on every map: **treat the angles
  as approximate and prefer the BSP surface under the decal when it is available.** `pitch_raw` near +-63 means "vertical" (floors
  and ceilings) everywhere.
* **Machine flag bits, control placement fields, function input names, one-sided front direction**: see section 8.

## 7. Numbers on the ten maps

| map | device tags | machines | controls | light fixtures | groups | volumes | flags | cameras | names (dangling) | sound scenery | decals | BSP switches | BSPs | DEVS size | convert |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| a10 | 19 | 245 | 0 | 0 | 9 | 126 | 221 | 292 | 379 (3) | 39 | 424 | 12 | 9 | 100.4 KiB | 0.26 s |
| a30 | 3 | 4 | 1 | 0 | 2 | 74 | 59 | 25 | 61 (17) | 124 | 117 | 2 | 2 | 25.8 KiB | 0.14 s |
| a50 | 14 | 208 | 8 | 281 | 26 | 89 | 66 | 75 | 176 (49) | 16 | 105 | 3 | 4 | 72.4 KiB | 0.15 s |
| b30 | 20 | 129 | 6 | 42 | 7 | 56 | 60 | 34 | 111 (10) | 18 | 27 | 2 | 2 | 35.7 KiB | 0.06 s |
| b40 | 15 | 72 | 22 | 97 | 12 | 122 | 70 | 72 | 448 (29) | 21 | 304 | 32 | 13 | 71.1 KiB | 0.27 s |
| c10 | 16 | 143 | 13 | 165 | 11 | 78 | 84 | 62 | 325 (3) | 51 | 2061 | 14 | 6 | 127.6 KiB | 0.25 s |
| c20 | 12 | 357 | 0 | 0 | 15 | 152 | 22 | 35 | 71 (0) | 119 | 46 | 6 | 4 | 59.0 KiB | 0.17 s |
| c40 | 20 | 79 | 22 | 93 | 14 | 121 | 52 | 126 | 129 (1) | 19 | 28 | 31 | 13 | 49.9 KiB | 0.29 s |
| d20 | 11 | 203 | 0 | 162 | 4 | 84 | 54 | 38 | 101 (0) | 50 | 218 | 4 | 5 | 58.4 KiB | 0.25 s |
| d40 | 24 | 156 | 5 | 0 | 13 | 158 | 337 | 203 | 275 (0) | 37 | 323 | 13 | 10 | 91.2 KiB | 0.33 s |

(sizes are the raw section; the pack is LZ4 compressed. No placement is dropped on any map: `dropped devices` is 0 everywhere; the
single unresolved placement of a10 is not a device.) Ranges over the tags of all maps: machine types door 30, platform 62, gear 28 tag
instances; position transition 0 .. 6000 ticks, power transition up to 150, depowered up to 6000, door open time up to 1800 ticks;
activation radii 0.85, 2.2, 2.6 and 5.0 wu (only on doors; the doors of a10 and d40 have radius 0: they are script-driven, so `a10` and
`d40` have no proximity doors, `a50` 46, `b40` 54, `c10` 50, `c40` 51, `d20` 26); group flags 0 (78 groups) / 1 (35).

## 8. Unverified or approximated

* Names of machine flag bits 0 / 1 and of the placement machine flags (public tag documentation; only the observed values
  are asserted), the meaning of the control placement fields `kind_flags` (0 / 1) and `extra` (0..4), the function input names,
  control types other than toggle, call value.
* Which side is "front" for one-sided doors and for controls: assumed the +x side of the rotation (yaw about +z), the same
  convention as every other placement (docs/13). Not checkable from the data alone.
* What exactly happens on power loss (depowered drift to 0 once), power gating of controls and doors, door close timing
  (`open_ticks + 1`), "can change only once" timing: modelled from the field names; the transition times themselves are verified.
* The decal angle convention (section 6), the initial BSP of maps whose first start lies in several BSPs.
* Position / power transitions are linear; the engine's acceleration times are not modelled.
* No object collision for doors, platforms and gears (nothing is pushed, blocked or crushed).

## 9. Tests

* `tools/tests/test_devconv.py`: a synthetic scenario (3 machine tags, 3 control tags, a light fixture, groups, volumes, starts,
  flags, cameras, sound scenery, decals, BSP switches, names with one dangling) checks every table and number, the SCEN placement
  order replica against `scenconv.collect_objects` (and detects a wrong count), tag problems (short block, wrong object type, time out
  of range, wrong reference group, door ticks mismatch), group index ranges, corrupt maps (every reflexive pointing outside the tag
  region raises `DevError`), NaN placements, the writer and reference parser (every truncation rejected), the independent volume
  reference (Cramer's rule against the converter's dot products on random boxes), the root helpers. With `assets/decompressed/*.map`
  it converts all ten maps and checks counts against the scenario blocks, orthonormal volumes, group ranges, tag ticks against the raw
  floats, SCEN indices, name resolution, serialisation, and the volume box convention on a10 (skipped without game data).
  `DEVS_FIXTURE=<path>` also writes the synthetic pack for the C tests.
* `engine/tests/test_devdef.c`: every field of the synthetic world, lookups, then truncation at every length, every table slot
  (moved, misaligned, oversized, miscounted), about 50 targeted field corruptions that must be refused, and a byte flip (three patterns) of
  every byte, each either rejected or walked completely under ASan/UBSan; real packs are opened, walked and checked against an
  independent point-in-box solver.
* `engine/tests/test_devices.c`: hand-computed synthetic expectations (initial states, group propagation, platform timing, mid-way
  reversal, door open / hold / close ticks, reach sphere, one-sided doors, power gating, depowered drift and frozen gears, scripted
  moves, controls of every kind including wrong side, no power, change-once groups and the call button, melee and change-once doors,
  anim frame, bad indices, event queue overflow, snapshot / restore) and, **for every real device of the ten maps**: a scripted full
  position change takes exactly `position_ticks` ticks both ways (OPENED / CLOSED event on tick T - 1), a power change exactly
  `power_ticks`, a depowered drift `depowered_ticks` (or none), 227 real doors that open for activators close `open_ticks + 1`
  ticks after the last activator tick, controls obey the power / side / toggle rules, every group drives its devices; plus
  19 767 random points of 200 random boxes against the independent solver.

## 10. Integration notes

* Pack: `<prefix>.dev.hp2` (cfg key `dev <prefix>`, per the phase 4 plan's pack keys); `scripts/build-world.sh` should run
  `devconv.py` for each map next to the other converters. `devconv.py` needs only the decompressed map.
* Object pack roots: `devconv.referenced_models(map)` / `referenced_anims(map)` (device models and their graphs; the placed devices
  are SCEN placements already, so `objpack` has them: the lists are the explicit roots), `referenced_sounds(map)` (tag ids of the
  `snd!` / `effe` references of the device tags and the `sound_looping` tags of the sound scenery, for `soundconv.py --roots` and
  `streamconv`; verified to resolve and close over their sounds with `soundconv.resolve_roots` / `closure`),
  `referenced_effects(map)` (the `effe` ids among them, for `fxconv` roots).
* Game tick order: script machine -> **devices** -> AI -> player. Per tick the host calls `devices_set_activators` (players, AI
  actors within reach of doors), runs `devices_tick`, drains events (play `devices_event_tag`, wake scripts waiting on a
  position), poses device objects with `devices_anim_frame` on the position overlay (`pos_anim` of the def's graph), and calls
  `devices_use` when the player presses use on a control. Script built-ins map to: `device_set_position(_immediate)`,
  `device_set_power`, `device_get_position / power`, `device_group_set / get`, `volume_test_object(s)` (`volume_*` helpers).
* Level manager: `bsp_switch` entries give the volume and the two BSPs; `devdef_get_start(0).bsp_mask` gives the BSP the map starts in
  (do not assume BSP 0); `bsp_mask` of devices, decals, cutscene flags and cameras selects what to keep per BSP.
* Memory: the section is 26 .. 128 KiB raw per map; `devices_t` is 70 KB static, or lower `DEV_MAX_DEVICES` (497 is the maximum seen).
