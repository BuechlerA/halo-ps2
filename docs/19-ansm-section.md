# `ASTB` section: animation state tables, format version 1

Workstream E of [phase 3](14-phase3-plan.md). Converter `tools/halo2ps2/ansmconv.py` (writer, reference reader, digest),
reader `engine/src/world/ansm.c` (header `engine/src/world/ansm.h`), tests `tools/tests/test_ansmconv.py` and
`engine/tests/test_ansm.c`. All integers are little endian. `ansmconv.py <map> -o assets/hp2/<map>.ansm.hp2` writes a pack
holding one section, `ASTB` (see [07-hp2-format.md](07-hp2-format.md)).

[12-anim-section.md](12-anim-section.md) explains why `ANIM` has no state: the animation names carry it, and `anim_find_name`
searches names. `ASTB` is the other half: the tables of the `model_animations` tag that say *which animation plays in which
state* (unit posture x weapon class x weapon type x action), plus the per-animation data that is not pose data (variant
chains, sound / key / foot frames, aiming ranges). Animation index *k* of a graph in `ASTB` is animation *k* of the same graph
(same Xbox tag id) in `ANIM`: `anim_sample(s, g, ansm_lookup(...), frame, out)` plays what a lookup returned. Contents of the Xbox data are
not reproduced here: layouts and derived statistics only.

## 1. What the Xbox tag holds (verified on all ten campaign maps)

A `model_animations` tag has, besides nodes and animations (12-anim-section.md), eight state blocks. Strides and fields below were
confirmed by name statistics on **8219 animations in 387 graphs** (every `antr` tag of the ten maps) and by 0 out-of-range indices
and 0 unprintable names (the converter drops and reports an out-of-range index, `check_graph` flags it: none occurs).

| Block (tag +offset) | Element | Content |
|---|---|---|
| objects +0x00 | 20 B | u16 animation, u16 function (0..3 seen), u16 function controls: overlay animations driven by an object function (sky wind/scale/rotation, the warthog's `firing`, a monitor's talking loop) |
| units +0x0C | 100 B | char[32] label (`stand`, `crouch`, `alert`, `flee`, seat names such as `W-driver`, `P-riderLF`, `GT-gunner`, `CD-passenger01`: 56 distinct over the maps), aiming range (24 B), reflexive of unit slots, reflexive of IK points, reflexive of class blocks |
| class block (unit +0x58) | 188 B | char[32] **weapon class** (`rifle`, `pistol`, `unarmed`, `sword`, `fixed`, `missile`, `support`: some are misspelt in the data, `missle` 90 of 160 blocks, `unuarmed` 16), grip marker, hold marker (`right hand`), aiming range, reflexive of 51 class slots, IK points, reflexive of type blocks |
| type block (class +0xB0) | 60 B | char[32] **weapon type** label (`ar`, `sg`, `sr`, `pr`, `pp`, `hp`, `ne`, `rl`, `ft`, `fr`, `fb`, ...), reflexive of up to 9 type slots |
| weapons +0x18 | 28 B | the weapon *object's* own graph: reflexive of slots (`fire-1`, `reload-2`, `chamber-1` of the pistol, sniper rifle, shotgun) |
| vehicles +0x24 | 116 B | steering range (right / left yaw per frame, frame counts), reflexive of vehicle slots, reflexive of suspension (20 B: mass point, animation, full extension and compression ground depth) |
| devices +0x30 | 96 B | reflexive of device slots (`position`) |
| unit damage +0x3C | 2 B x 176 | flat table of animation indices: 4 categories x 4 directions x 11 hit regions |
| first person weapons +0x48 | 28 B | reflexive of up to 28 fp slots |
| sound references +0x54 | 20 B | tag reference to a `sound` tag (+ 4 B zero) |

Slot lists are reflexives of 16-bit animation indices (`-1` / `0xFFFF` = none, slots past the end of a list are none).

### 1.1 Slot names (the answer to "what is in there")

The tag stores *numbers*; the names of the slots are fixed by the engine. Verified = the last word of the animation name referenced
from the slot equals the slot name (or one of the aliases of section 1.2) for **every** reference on the ten maps, 0 exceptions;
the numbers in brackets are the references found (ten maps, all graphs). Slots in the tables of `ansmconv.py` / `ansm.h` that no animation of the
ten maps uses are marked *assumed* (taken from the public tag documentation, neighbours verified) or are unnamed.

* **type** (9): 0 reload-1 (340), 1 reload-2 *assumed*, 2 chamber-1 (10), 3 chamber-2 *assumed*, 4 fire-1 (807), 5 fire-2 (18), 6, 7 unnamed, 8 melee (200).
* **class** (51): 0 idle (1216), 2 turn-left, 3 turn-right (403), 4 dive-front, 6 dive-left, 7 dive-right (1249), 8 move-front (445), 9 move-back (204),
  10 move-left, 11 move-right (185), 16 airborne, 17 land-soft, 18 land-hard (1874), 20 throw-grenade (1543), 23 ready (10), 25 aim-still (656),
  26 aim-move (201), 27 surprise-front, 28 surprise-back, 29 berserk, 30 evade-left, 31 evade-right, 32 signal-move, 33 signal-attack, 34 warn,
  35..38 stunned-front/back/left/right (54), 39 melee (618), 40 celebrate, 41 panic, 42 melee-airborne, 44 resurrect-front, 45 resurrect-back,
  46 melee-continuous, 47 feeding, 48 leap-start, 49 leap-airborne, 50 leap-melee. Unnamed and never used: 1, 5, 12..15, 19, 21, 22, 24, 43
  (in public documents 5 is dive-back, 24 put-away: not asserted here).
* **unit** (30): 0 airborne-dead, 1 landing-dead (1217), 2 acc-front-back, 3 acc-left-right, 4 acc-up-down (155), 5 push (10), 7 enter (273), 8 exit (535),
  9 look (1122), 10 talk (199), 11 emotions (33), 27 opening (21), 28 closing (20), 29 hovering (7). Unnamed and never used: 6, 12..26.
* **first person** (28): 0 idle, 1 posing, 2 fire-1, 3 moving, 4 overlays, 5 light-off, 6 light-on *(10 refs, the animations of two weapons are named `light-off`)*,
  7 reload-empty, 8 reload-full (the pistol has both; the assault rifle points both at `reload-full`), 9 overheated, 10 ready, 11 put-away (10), 12 overcharged,
  13 melee, 14 fire-2, 15 overcharged-jitter, 16 throw-grenade, 17 ammunition (9), 18 misfire-1, 19 misfire-2 *assumed*, 20 throw-overheated,
  21 overheating, 22 overheating-again, 23 enter, 24 exit-empty, 25 exit-full, 26 o-h-exit, 27 o-h-s-enter.
* **weapon object** (10): 4 reload-2 (5), 5 chamber-1 (5), 9 fire-1 (15); the other slots are unused (the labels of 4 and 5 are as the data names them).
* **vehicle** (8): 0 steering (26), 3 velocity (11), 5 ground-speed (6); 1 roll, 2 throttle, 4 braking, 6 occupied, 7 unoccupied *assumed*.
* **device** (2): 0 position (76); 1 power *assumed*.
* **damage**: index = (category x 4 + direction) x 11 + region. Categories `s-ping`, `h-ping`, `s-kill`, `h-kill` (shield / health, hit / killing blow),
  directions `front`, `left`, `right`, `back`, regions `gut chest head l-arm l-hand l-leg l-foot r-arm r-hand r-leg r-foot` (the word of the animations at
  each region index; unconfirmed names fall back to `gut`). Verified: all 11336 entries of the ten maps: the category word of the animation name equals the
  table's category for 10170, the direction word for 7186 (the others reuse a neighbouring animation: a single front hit animation for every direction, `s-kill`
  where there is no `h-kill`).

What the hoped-for names map to in Halo CE's vocabulary: there is **no separate walk and run** (`move-front` / `move-back` / `move-left` / `move-right`, speed
comes from the biped's tuning and the animation's root motion); `crouch` is a **unit label** (posture), not an action; `jump` and `fall` are both `airborne`;
`land` is `land-soft` / `land-hard`; `ready` exists for the class block (10 refs) and the first-person block (68), `put-away` only first person (10); `fire-1` /
`fire-2` / `reload-1` / `melee` are *type* slots (so they depend on the weapon label), `throw-grenade` and the `aim-*` overlays are class slots, `melee` also
exists as a class slot (generic melee, 618 refs).

### 1.2 Aliases (animations authored under a neighbouring slot)

0 of 32 088 references name something unexpected; 847 point at the animation of a neighbouring slot, all of them these: class `land-soft` <- `land-hard` (276) and
the reverse (16), `dive-left` <- `evade-left` and `dive-right` <- `evade-right` (222 each: the elite graphs name the dodge `evade`), `aim-move` <- `aim-still` (25),
`stunned-back/left/right` <- `stunned-front` (10 each); first person `reload-empty` <- `reload-full` (19, and 7 the other way), `light-on` <- `light-off` (10),
`reload-*` <- `overheated` (20). They are in `ansmconv.ALIASES`.

### 1.3 Variants, loops, events

* `next animation` is **never** a loop link and never the animation itself: it is -1 or the next *variant* (`idle%1` -> `idle%2` -> `idle%3`; 403 links, all
  inside a chain). `main animation` (+0x42) is the first variant of the chain (the animation itself otherwise) and `relative weight` (+0x44) is the **cumulative**
  probability of the chain position (0.33, 0.67, 1.00): pick `r` in [0, 1) and walk from the head until `r < weight`. The slots always point at the head. Names
  without a suffix (`stand rifle idle`) and variant heads (`alert unarmed idle%1`) both occur; 808 of 8219 animations have a `%n` suffix.
* **Loops**: the tag has no loop flag (loop frame index is non-zero for 319 animations, flags are only compressed / world-relative / 25 Hz; none is 25 Hz). The
  ASTB `LOOPS` flag is the same derived rule as `ANIM_LOOPS` (base animation named ... `idle`, `move-*`, `airborne`: 1983 animations; the test checks that the two
  sections agree on every animation of the ten maps). The engine's state machine decides what loops; overlays (`aim-still`, damage, `look`) are pose sets
  (1821 overlay animations) and are indexed by aim angle, not played.
* **Events** an animation carries: **one sound** (index into the graph's sound references, frame; 1783 animations), **two key frames** (non-zero on 195 / 11 animations: the
  frame where a melee blow lands, a grenade leaves the hand), **left / right foot frames** (975 animations, 0 = unset; 146 exceed the frame count, up to twice
  as large, e.g. a 7 frame `turn-left` has 7 and 14: the raw tag values are passed through). **There are no effect or marker events in the tag**: muzzle flashes, shell
  ejection and blood come from the weapon, projectile and damage effect tags. Sound frame < frame count and key frames < frame count hold on all 8219 animations.
* Time: every animation is 30 Hz (`ticks` == `frame_count`); a 25 Hz flag would give `ticks = round(frames x 30 / 25)` (never set).

### 1.4 Aiming ranges

The units block and each class block carry the aiming ranges: yaw per frame x frame count to the right / left, pitch per frame x frame count down / up (radians).
For every class block the **`aim-still` / `aim-move` overlay has (right + left + 1) x (up + down + 1) frames** (810 of 857 overlays; 30 blocks carry no counts but
have 9 frames, the other 17 are the cyborg's gunner block (16 frames for 5 x 3) and the dropship gun's 2-frame overlays). The cyborg's unit blocks all say 70 degrees yaw / 45 degrees pitch (the aiming screen `unitconv` reports), its rifle, pistol and support class blocks +-90 / +-90
(one frame of 90 degrees each way: the overlay frames are authored at the extremes and the engine interpolates), the missile class +-90 / +-90 with two pitch frames of 45 degrees, a gunner's `fixed`
weapon +-180 yaw (2 x 90) / 20 down / 35 up. Whether the clamp is the unit's 70 / 45 or the class block's range is not decided here (unverified); both are exported. The order of the frames inside the overlay is not read.

### 1.5 Which weapon uses which block

The weapon tag's **label** (the string at +0x30C, `ar`, `pp`, `hp`, `cannon`; followed by stale bytes of an older label, see `weapconv.py`) is the *type label*:
**verified: all 20 weapon tags of the ten maps** have a label that occurs as a type label in a graph. The tag does **not** name the class (`rifle`, `pistol`, ...): the
plasma pistol (`pp`) sits under `pistol`, the rocket launcher (`rl`) under `missile` / `missle`, and a 32-byte string at +0x320 of most weapons says `rifle` regardless
(stale text). The engine therefore resolves the class by label, per unit: `ansm_find_class_for_type(unit, label)` returns the class block whose type block `label`
has the most animations (ties: the first), e.g. `pp` -> `pistol`, `ar` -> `rifle`, `ne` (needler, present under both) -> the block with animations.

## 2. Section layout

```
0      header (64 B)  "ASTB" | u16 version = 1 | u16 flags = 0 | u32 graph_count | u32 graphs_off (64) | u32 body_off | u32 total size |
                      u32 anim_count (all graphs) | zero
64     graph x G (96 B, strictly ascending tag id)
         u32 tag_id | u32 0 | refs (u32 offset, u32 count) x 10, in this order:
         animations (32 B), name index (u16), objects (8 B), units (64 B), vehicles (32 B), devices (16 B), weapon objects (16 B),
         first person (16 B), damage (u16), sounds (u32 tag id) | zero
body   written bottom-up, every table 4- or 16-aligned; offset body_off + 0 is never a table or a string ("" is offset 0):
         strings (NUL terminated, <= 63 chars, deduplicated), slot lists (u16, deduplicated, trailing "none" trimmed),
         then per graph its tables
anim   (32 B) u32 name_off | u16 frame_count | u16 ticks | u16 flags (1 LOOPS, 2 WORLD_RELATIVE) | u8 type | u8 fps | i16 next | i16 main |
         u16 loop_frame | i16 key_frame | i16 key_frame2 | i16 sound | i16 sound_frame | u8 foot_left | u8 foot_right |
         u16 variant_weight (cumulative probability x 65535) | 2 pad
unit   (64 B) u32 name_off | ref classes | ref slots | ref ik | aim 24 B | pad
class  (64 B) u32 name_off | u32 grip_off | u32 hold_off | ref slots | ref ik | ref types | aim 24 B | pad
type   (16 B) u32 label_off | ref slots | pad          ik (8 B) u32 marker_off | u32 attach_off
aim    (24 B) f32 yaw_right_per_frame | f32 yaw_left_per_frame | u16 right_frames | u16 left_frames |
              f32 pitch_down_per_frame | f32 pitch_up_per_frame | u16 down_frames | u16 up_frames
vehicle (32 B) f32 steer_right | f32 steer_left | u16 right_frames | u16 left_frames | ref slots | ref suspension | pad
         suspension (16 B) u16 mass_point | i16 anim | f32 full_extension | f32 full_compression | pad
device, weapon object, first person (16 B) ref slots | pad          object overlay (8 B) i16 anim | u16 function | u16 controls | pad
```

Every animation index stored anywhere is below the graph's animation count or "none". The name index is a permutation of the graph's animation numbers sorted by
(name bytes, index): `ansm_find_anim` is a binary search. The default graph set of a pack is `referenced_anims(map)` (section 4); `--all` converts every `antr` tag.

## 3. Reader (`ansm.h`)

* `ansm_open` validates the whole section once: header and graph order, every reference against the section size and the start of the body, every string (inside the
  section, NUL within 64 bytes), every animation record (variant links, sound index, frame fields, flags), the name index (in range, strictly ascending, so a permutation) and
  every slot of every list and of the damage table. After `ANSM_OK` every accessor can be used without further checks; a failed open leaves an empty section. No heap, no `double`,
  no ps2sdk. Cost: 24 microseconds for a10's 79 KiB on a fast host (about 0.7 ms estimated on the EE at 20-30x), 14 KiB of code at -O2 for the EE. A lookup
  `ansm_lookup(s, g, "stand", "rifle", "ar", ANSM_T_RELOAD_1)` is three string compares, 22 ns on the host.
* Lookup chain: `ansm_find_graph(tag)` -> `ansm_find_unit("stand")` -> `ansm_find_class(u, "rifle")` -> `ansm_find_type(u, c, "ar")` -> `ansm_type_slot(.., ANSM_T_RELOAD_1 & 255)`,
  or in one call `ansm_lookup(s, g, unit, class, type, action)`. Actions are `level << 8 | slot` (`ANSM_T_FIRE_1`, `ANSM_C_MOVE_FRONT`, `ANSM_U_ENTER`, `ANSM_FP_RELOAD_FULL`,
  `ANSM_WO_FIRE_1`, `ANSM_V_STEERING`, `ANSM_D_POSITION`, generated from the X-macros that also fill the name tables, `ansm_slot_name` / `ansm_slot_find`, checked against
  `ansmconv.SLOT_NAMES` by the test). Fp / weapon object / vehicle / device actions use block 0 of the graph (`ansm_fp_slot(s, g, block, slot)` etc. reach the others).
* `ansm_anim_info` (name, frames, ticks, flags, type, next, main, key frames, sound index and tag, foot frames, cumulative weight), `ansm_find_anim` (exact name, lowest index; a name
  without `%n` finds the head of its chain), `ansm_pick_variant(k, r)`, `ansm_anim_events(k)` (sorted by frame: sound with its `sound` tag id, key frames, feet; at most 5),
  `ansm_aim_ranges`, `ansm_unit_ik` / `ansm_class_ik`, `ansm_vehicle_info` / `_suspension`, `ansm_object_overlay_get`, `ansm_damage_anim(category, direction, region)`.
* Integrator notes: (1) look up by *unit label*: `stand` / `crouch` / `alert` for the player and AI bipeds; the player's cyborg also has `W-driver`, `P-riderLF` seat blocks. (2) The player
  biped's weapon in hand maps to a class through `ansm_find_class_for_type(unit, weapon_label)`; `unarmed` is the class without a weapon. (3) The player's first-person arms: ONE graph animates hands
  and gun (`scenconv`: weapon tag +0x46C), its first-person block gives `idle / ready / fire-1 / reload-empty / reload-full / melee / throw-grenade / overheated ...` by slot. (4)
  `ANIM`'s `anim_find_name` and `ansm_find_anim` agree on names (same graph, same index).

## 4. Roots, `animconv` hook and numbers

`ansmconv.referenced_anims(map_path) -> set[int]`: the `model_animations` tag ids that gameplay objects use: the graph (+0x38) of every object tag (biped, vehicle, weapon, equipment,
projectile, scenery, device, light fixture, ...) and the first person graph (+0x46C) of every weapon. a10: 39 graphs (49 `antr` tags exist; the rest are cinematics referenced by the
scenario). The integrator adds them to `objpack.py`'s animation roots (`animconv.build_section(map, sorted(roots))`). `ansmconv.referenced_sounds(map_path)` returns the `sound` tag ids named by
those graphs (footsteps, weapon handling, melee, ...) for the sound workstream.

`animconv` converts **every animation of a selected graph, in tag order**: ANIM index = antr index = ASTB index (checked on all 8219 animations of the ten maps: same names, frame counts,
loop flags and `next` links). It drops nothing. Optional hook, additive and API compatible: `animconv.build_section(map, tags, animations_needed={tag_id: indices})` writes the animations of
those graphs that are not in the set as **stubs** (same name, type, frame count, loop flags; no pose data, all channels the rest pose, no root motion; `ANIM_STUB` = 4 in `flags`, ignored by `anim.c`).
`ansmconv.needed_animations(map)` computes the sets: every animation except those no table references although the name says they belong in one (`stand rifle dive-left` in the marine graph;
`dead_anims`); graphs without any slot (scenery, machines) and free-form names that scripts play by name (`pilot_fidget01`, `think_1`) stay. The saving is small because the tables name
nearly every animation: a10 1,708,704 -> 1,678,912 bytes of ANIM (-1.7 %, 12 stubs), c10 2,070,848 -> 2,034,848. The big lever is converting only the referenced graphs.

Sizes and times (`ansmconv.py <map> -o ...`, one core, Python 3.14; the default graph set and `--all`):

| map | graphs | animations | ASTB raw | `.ansm.hp2` | `--all`: graphs | raw |
|---|---:|---:|---:|---:|---:|---:|
| a10 | 39 | 768 | 78.7 KiB | 30 KiB | 49 | 83.1 KiB |
| a30 | 27 | 792 | 80.3 KiB | 30 KiB | 29 | 81.0 KiB |
| a50 | 25 | 847 | 84.1 KiB | 32 KiB | 34 | 92.7 KiB |
| b30 | 30 | 861 | 85.9 KiB | 32 KiB | 32 | 86.3 KiB |
| b40 | 37 | 882 | 87.8 KiB | 32 KiB | 40 | 89.3 KiB |
| c10 | 33 | 810 | 77.1 KiB | 30 KiB | 39 | 85.2 KiB |
| c20 | 28 | 471 | 43.8 KiB | 20 KiB | 30 | 44.1 KiB |
| c40 | 45 | 871 | 86.4 KiB | 32 KiB | 48 | 88.2 KiB |
| d20 | 32 | 829 | 81.8 KiB | 32 KiB | 34 | 82.4 KiB |
| d40 | 43 | 803 | 79.5 KiB | 32 KiB | 52 | 82.5 KiB |

About 0.1 s per map to convert. Largest: b40, 87.8 KiB (1 % of the 9 MiB resident budget); the biggest graphs are marine / cyborg / elite (a few KiB each of state, the animation names are most of the section).
Pack files are rounded up to the 2 KiB sector.

## 5. What is verified and what is not

Verified (data or tests): strides and field offsets of every block (name statistics above, zero out-of-range indices on 8219 animations); slot names (all referenced slots are named, 0 unexplained
disagreements); variant chains and cumulative weights; the overlay frame-count rule for the aiming ranges (810 / 857); the damage table layout (category / direction / region order); the weapon label
link (20 / 20); the sound index range and sound / key frames inside the animation; ANIM and ASTB index and name equality on every graph; that the C reader and the Python reader see the same
data in every table (digest, on a synthetic world and on all ten real packs); that every byte flip and truncation of the synthetic pack and sampled ones of a real pack are rejected or walk
safely under ASan/UBSan.

Not verified / approximated: the *names* of slots no animation uses (marked *assumed*); the loop rule (derived, as in ANIM); object `function` / `controls` values (kept raw); the meaning of the
foot frames beyond 0 = unset; which of the overlay's frames belongs to which aim angle; the order of the damage regions beyond the region word statistics; vehicle seat semantics (the seats are
unit blocks with labels like `W-driver`, their IK points are exported but not interpreted); `sound references` flags (+16, always 0); the weapon tag's class string (not found).

## 6. Verification

* `python3 -I tools/tests/test_ansmconv.py`: the slot tables against `ansm.h`, a synthetic map (units / classes / types, variants, events, damage, first person, vehicle, device,
  object overlays, sound references, stale and out-of-range data) converted and read back with the reference reader against hand-written expectations (lookups, find by name,
  variant picking, class by label, damage), damaged tags (dropped indices reported, structural damage raises), `referenced_anims` and the `animations_needed` hook (stubs, indices),
  the CLI; on the real maps (skipped without them) all ten convert, every slot is named, ANSM names / loop flags / frame counts / next links equal the ANIM source on every graph, the cyborg's
  `stand rifle ar reload-1` and friends resolve to the expected names, the pistol's first-person graph gives idle / fire / reload / ready / melee / throw by slot.
* `make -C engine/tests`: `test_ansm` opens the fixture, checks the digest of a full walk through the C API against the Python reader's, invariants of any pack (names find themselves,
  slots in range, variants end, lookups equal direct slot access, events sorted), hand-written expectations of the synthetic world, then corrupts every byte four ways and truncates at every length.
  On real packs: `test_ansm assets/hp2/a10.ansm.hp2 $(python3 -I tools/halo2ps2/ansmconv.py assets/hp2/a10.ansm.hp2 --digest)` (done for all ten maps).
