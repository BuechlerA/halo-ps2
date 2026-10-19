# `ANIM` section: converted `model_animations`, format version 2

Converter `tools/halo2ps2/animconv.py` (writer, reference sampler), reader and sampler
`engine/src/world/anim.c` (header `engine/src/world/anim.h`), tests `tools/tests/test_animconv.py` and
`engine/tests/test_anim.c`. All integers are little endian. The section lives in the mission pack under the tag
`ANIM` (see [07-hp2-format.md](07-hp2-format.md)); `animconv.py ... -o assets/hp2/a10.anim.hp2` writes a pack
holding only this section, like the `TEXS` packs.

Contents of the Xbox data are not reproduced anywhere in this repository: this document and the module
docstring of `animconv.py` describe layouts and quote only derived statistics.

## 1. What a graph is

An `antr` (`model_animations`) tag is an *animation graph*: a node list (names, hierarchy), up to a few hundred
animations, and state tables (which animation plays in which unit/weapon/vehicle state). Node *i* of a graph is
node *i* of every model it can animate: each animation carries the node list checksum of its model
(`model.node_list_checksum`), and the node names/counts match on every pair checked (all ten campaign maps).
Object tags reference model (+0x28) and graph (+0x38); one graph is shared by many objects (`marine` by all marine
variants, `door small` by three door models).

The section holds, per graph (sorted by Xbox tag id, key of `anim_find_graph`): the node count, a rest pose per
node, and its animations in the tag's order. Animation time is in frames at 30 Hz; `fps` is 30 for every
animation (the 25 Hz PAL flag never occurs).

## 2. Verified and unverified facts about the Xbox data

Full layouts are in the docstring of `tools/halo2ps2/animconv.py`. Summary:

| Fact | Status |
|---|---|
| Tag struct: nodes reflexive +0x68 (64 B each), animations reflexive +0x74 (180 B each); object tags reference graph at +0x38, model at +0x28 | verified on all maps |
| Animation block fields (name, type, frame count/size, frame info type, checksum, node count, loop frame, flags, node flag words, three tag-data blocks) | verified: data sizes equal what the flags imply for **all 8219 animations of the ten campaign maps** (decoding raises otherwise) |
| Channel flags are 64-bit, one bit per node, per channel (translation +0x5C, rotation +0x6C, scale +0x7C); a set bit means *animated*; bits at or above the node count are garbage and ignored | verified |
| Uncompressed: default data holds the static channels, frame data the animated ones, interleaved per node (rotation 4 x s16 / 32767, translation 3 x f32, scale f32); all rotations are unit length | verified |
| Frame info = per-frame **motion deltas** (dx dy [dz] [dyaw], f32): a walk cycle's dx sums to one stride, a 90 degree turn's dyaw to 1.571 | verified on cyborg idle/walk/turn |
| Compressed data (186 animations: cinematics etc.): header of 11 offsets, per-channel (first key << 12 \| count) tables, u16 key frame lists (never frame 0, last = frame count - 1), a default value for every node, linear keys; rotation = four signed 12-bit components packed into 48 bits (bit layout in the docstring) | verified: offsets and key lists are checked on every compressed animation, the quaternions of 2812 keys have sum of squares 1.0005 +- 0.0003, hinge nodes have exactly two non-zero components, and a compressed cinematic idle equals the uncompressed biped idle to 0.048 degrees mean / 0.186 max |
| **Halo stores the conjugate of the Hamilton quaternion**, in animations as in model nodes. The converter conjugates, so ANIM rotations are in the engine's convention, like MODL | verified (the std reading blows the bind pose apart, the conjugate stands the skeleton up and the walks walk; see `docs/11-model-section.md`) |
| Node-less graphs (vehicles, devices, trees): the node block is empty, animations carry the model's node count | verified |
| Rotation interpolation between compressed keys is linear/nlerp | assumed (differences are inside the 0.19 degree compression error) |
| `ANIM_LOOPS` | **derived, unverified**: the tag has no loop flag (Halo's unit state machine loops idle/move states). The converter sets it for base animations named `... idle`, `move-front/back/left/right`, `airborne`. `loop_frame` is the tag's loop frame index (non-zero only for `airborne`, where it is a real loop point, and `land-*`, where it is a marker); it is only used with `ANIM_LOOPS`. The engine's state machine should override the flag |
| `ANIM_WORLD_RELATIVE` | tag flag bit 1 (the c10 cinematics), informational |

## 3. Not converted

* The state tables of the graph (`units`, `weapons`, `vehicles`, `devices`, `unit damage`, `fp weapons`
  reflexives): `anim.h` has no place for them. The animation names carry the state
  (`<posture> <weapon class> <action>`, e.g. `stand rifle idle`, `alert unarmed move-front`, `G-driver enter`), and
  `anim_find_name` finds them; variants are numbered `name%1`, `name%2`. A later contract revision should add the
  state table (state -> animation index, variant chain `next animation`/`relative weight`).
* `sound`, `sound frame`, foot frames, `key frame`/`second key frame`, `weight`, `next animation`: read, not stored.
* Sound references block.

## 4. Section layout

```
0      header (64 B)   "ANIM" | u16 version = 2 | u16 0 | u32 graph_count | u32 anim_count | u32 graphs_off (64) |
                       u32 anims_off | u32 pool_off (16-aligned) | u32 total size | zero
64     graph  x G (32 B)   u32 tag_id | u16 node_count | u16 anim_count | u32 first_anim | u32 default_off |
                       u32 model_tag (the model the rest pose came from, 0 = derived from the animations) |
                       u32 names_off (version 2: node_count x 32 B node names in the pool, section 8) | zero
       anim   x A (96 B)   char[48] name | u16 type | u16 frame_count | u16 loop_frame | u16 flags | u16 node_count |
                       u16 root_type | u16 graph_nodes | 2 pad | f32 fps | u32 data_off | u32 data_bytes |
                       u32 root_off | u32 default_off | zero
pool   (offsets are pool-relative; the first 16 bytes are reserved so offset 0 means "none")
       graph rest pose   node_count x 32 B = anim_xform floats (x y z w, tx ty tz, scale)
       animation blobs   deduplicated: identical animations (the tag repeats some) share one blob
       root motion blocks
```

Graphs are sorted by tag id and own consecutive animation records (`first_anim` runs on from the previous graph);
`anim_open` rejects anything else.

### Animation blob

```
u8 mode[node_count], padded to 4:  bits 0-1 rotation, 2-3 translation, 4-5 scale
        0 = the graph's rest pose value   1 = static (value follows)   2 = animated (track follows)
then, node by node, rotation / translation / scale, skipping mode 0:
   static rotation      4 x s16 (x y z w) / 32767                          8 B
   static translation   3 x f32                                            12 B
   static scale         f32                                                4 B
   track (4-byte aligned, a multiple of 4 bytes long):
        u16 key_count K (>= 2) | u16 flags
              1 dense: K == frame_count, no index list
              2 u8 indices (frame_count <= 256) instead of u16
              4 translation values are f32, not quantised
              8 rotation about one axis (hinge): axis = flags >> 4 & 3
        translation, unless flag 4:  f32 base[3] | f32 step[3]
        index list unless dense: K ascending frame numbers, first 0, last frame_count - 1, padded to 4
        values: rotation K x 6 B (or 4 B with flag 8) | translation K x 6 B (or 12 B) | scale K x f32
```

* Rotation key (6 B): the unit quaternion is sign-flipped so its largest component is positive, that component is
  dropped, the other three are stored as 15-bit signed values of `round(c * 16383 * sqrt 2)` in the high 15 bits
  of three s16; the dropped index (0..3 = x y z w) is bit 0 of the first word | bit 0 of the second << 1. The
  dropped component is rebuilt as `sqrt(1 - sum of squares)`. Quantisation error alone: 0.008 degrees max.
* Hinge key (4 B): two s16, the axis component and w (x = y = 0 for the hinge nodes of the biped skeletons: 30 % of the
  rotation tracks, 25 % of the keys). Detected when the other components stay below 1e-4.
* Translation key (6 B): u16 x 3, `base + u * step` per axis with the track's min and range / 65535. A track whose
  quantisation error would exceed 40 % of the tolerance (range above ~2.4 wu) keeps f32 keys.
* Keys are the output of a closed-loop reduction: a split-at-worst-frame search over the **quantised** key values,
  so that linear interpolation (nlerp for rotations) reproduces every source frame within the tolerance
  (rotation 0.18 degrees, translation 0.35 mm = 1.15e-4 world units, scale 1e-3). A track needing (almost) every
  frame is stored dense, which drops the index list.
* A channel within 0.05 degrees / 0.08 mm of a single value is static; a static value equal to the graph's rest pose
  is mode 0 (all the bone-offset translations of a skeleton, hence only a few percent of the bytes are static
  values).

### Root motion block

`f32 scale[4]`, then `frame_count x k` s16 (k = 2, 3, 4 for `root_type` 1, 2, 3: dx dy [dz] dyaw), padded to 4. Frame
info that is zero everywhere is dropped (`root_type` 0). `anim_root_motion` returns the *motion made during a
frame*, in the object's frame, world units and radians; the engine integrates it.

### Rest pose

The rest pose of a graph is the default pose (translation, Hamilton rotation) of a model whose node list
checksum equals the graph's animations', preferring a model used together with the graph by an object tag; `model_tag`
names it. Graphs without such a model (weapon first-person graphs, whose node list is assembled at runtime, and
the `jenkins` cinematic) take the first animation's frame 0. The converter prints warnings for graphs without a
model, with differing checksums, node names or counts between graph and the models used with it.

## 5. Reader semantics (`anim.h`)

* `anim_open` validates the whole section once (tables, ordering, every animation blob: mode bytes, track headers,
  flags, key counts, key frame lists strictly ascending from 0 to frame_count - 1, exact lengths). A section it accepts
  can be walked and sampled without further checks; the host test corrupts every byte and truncates at every length
  under ASan/UBSan. Opening a 2 MiB section takes 0.3 ms on a fast host (~10 ms estimated on the EE).
* `anim_sample(s, g, k, frame, out)`: `out->count = g->node_count`; nodes at or beyond the animation's `node_count`
  and mode 0 channels get the rest pose. `frame` is clamped to >= 0 (NaN counts as 0). Non-looping animations clamp
  at the last frame. Looping ones (`ANIM_LOOPS`) wrap `frame >= frame_count` to
  `loop_frame + fmod(frame - loop_frame, frame_count - loop_frame)` (`loop_frame` 0 unless it is below the last
  frame), and a fractional frame between the last frame and `frame_count` blends the last frame with the loop frame.
* Rotations interpolate along the shortest path (`dot < 0` flips) and are normalised; `anim_blend` does the same per
  node and has the same aliasing guarantees as the header promises.
* No heap, no `double`; the only libm calls are `sqrtf` (inlined) and `fmodf` (only when a looping frame number runs
  past the end).
* Additions to the fixed header (see section 7): `anim_info.root_motion` and `anim_root_motion`.

## 6. Numbers

All graphs of a map, `animconv.py <map> --verify` (Python 3.14, one core):

| map | graphs | animations | Xbox source | ANIM | share of the 9 MiB resident budget | convert | rotation error max / rms | translation error max / rms |
|---|---:|---:|---:|---:|---:|---:|---|---|
| a10 | 49 | 827 | 4.09 MiB | 1.87 MiB | 20.8 % | 4.3 s | 0.180 / 0.038 degrees | 0.350 / 0.017 mm |
| a30 | 29 | 803 | 4.03 MiB | 1.83 MiB | 20.3 % | 3.5 s | 0.180 / 0.036 | 0.350 / 0.020 |
| b40 | 40 | 905 | 4.70 MiB | 2.11 MiB | 23.4 % | 4.3 s | 0.180 / 0.038 | 0.350 / 0.019 |
| c10 | 39 | 880 | 5.31 MiB | 2.34 MiB | 26.0 % | 4.8 s | 0.180 / 0.039 | 0.350 / 0.020 |

The errors compare the section's samples at every source frame with the decoded Xbox frames (quantisation, key
reduction and static detection included). The largest a10 graphs: `marine` 437 KiB (176 animations), `elite`
360 KiB, `cyborg` 342 KiB (195), `grunt` 180 KiB; a scenario normally needs a subset (cinematic and fp graphs, vehicles
and devices are small: a door is about 200 B). About 85 % of the bytes are rotation tracks, 7 % translation tracks, 2 % static values, the rest
records, rest poses and root motion. The sampler costs 0.22 microseconds per 19-node pose on an Apple core at -O2
(1 microsecond with sanitizers); the R5900 is 20-30 times slower per operation, i.e. about 5-7 microseconds per
character per tick.

Choices that were measured and rejected: 10.10.10.2 smallest-three keys (4 B) have 0.25 degrees max error, over the
0.2 budget; per-track relative encoding with 8-bit components saves 4 % because 30 % of the tracks swing through more than
50 degrees; greedy forward key selection instead of the split search saves 3 %.

## 7. Header additions

`anim.h` is the lead's fixed contract. Two additive changes, both documented there: a field `uint16_t root_motion`
at the end of `anim_info` and the function `anim_root_motion` (frame info is the only way to move a unit with its
animation; the Xbox stores it separately from the node tracks). Nothing else was changed. The hp2 section tag
table in [07-hp2-format.md](07-hp2-format.md) should list `ANIM`.

## 8. Version 2: graph node names and the model-to-graph node map

Version 1 assumed graph node i = model node i. That holds for every graph that has a model with the same node-list checksum
(bipeds, vehicles, devices, scenery), but the first-person weapon graphs are assembled at runtime from the hands model and the
weapon's first-person model, ordered breadth-first over the combined rig: the rifle graph (42 nodes) keeps the five gun nodes at
indices 7, 18, 24, 25 and 31 between the hand nodes. Playing it with the version-1 rule gave every hand bone another bone's
transform (the 2026-10-17 playtest review). Version 2 therefore stores the graph's node names: the graph record's bytes 20..23
hold `names_off`, a pool offset of `node_count` x 32 B NUL-terminated names in graph order (graphs without a node block write
empty names). The reader exposes `anim_node_name` / `anim_find_node`; `sim/objects.c` maps a model's nodes onto a graph by name
once per (model, graph) pair (a 32-entry cache; a graph without names, or one whose order equals the model's, maps 1:1; a model
node the graph does not name keeps its bind pose). The first-person weapon hangs from the hand node the graph names as the gun
root's parent. Cost: about 60 KB more ANIM data on a10. ADR-056.

## 9a. Face overlays (`stand emotions`, `stand talk`) and the MOUT section

A talking unit's face is not in the cinematic animations (Keyes' `well_thats_it_then` animates 18 body nodes; its 16 `frame ...` face nodes are static at the rest pose): it is two OVERLAY animations (type 1) of the unit's own graph,
played at run time by the Xbox. Verified on `characters\captain\captain` (34 nodes):
* `stand emotions` (6 frames): the tag flags only the channels it drives as animated: rotation of the four eyelids and the jaw, translation of the brows, eyelids, lips and mouth corners; every other channel is the authoring pose and is not
  the overlay's. The values are DELTAS (brow translations of 0 .. 4 mm, eyelid rotations of 0 .. 41 degrees about the node's local x, frame 0 = neutral). `unit_set_emotion unit n` (XBE 0x220df0: "-1 is none, other values depend on unit") is the frame index.
  The a10 intro sets Keyes' emotion 3, then 5 (heavy lids: the stern stare of the briefing) and back to 3.
* `stand talk` (1 frame): only the jaw rotation (6.5 degrees about the jaw's local z) is driven; the speaking unit scales it by the loudness of the line, one byte (0..255) per 30 Hz tick: the `snd!` permutation's MOUTH DATA (tag-data
  reference at permutation +0x54; 293 bytes for the 9.76 s line `cor05b`, values in steps of 5).
* Composition (VERIFIED by eye against the video, frames 2830 .. 2910 of the a10 intro): the overlay rotation multiplies the base rotation on the right (`base * overlay`, the overlay turns in the bone's own frame: the jaw opens, the lids drop);
  `overlay * base` twists the lips sideways. Translations add. Nodes the overlay does not drive are untouched.

`animconv.encode_animation(..., overlay=True)` (for `is_face_overlay`: type 1 named `stand emotions` / `stand talk`) writes only the driven channels (a single-frame channel as a static one, never the rest shortcut) and leaves every other channel
mode 0 (default); `anim_overlay_modes()` reads the channel modes back as the mask (`ANIM_OV_ROT`, `ANIM_OV_TRANS`). Other type 1 animations (aim-still, device position, vehicle functions) are converted as before.
`sim/objects.c objects_pose_model_face` composes the overlays onto the posed skeleton for objects whose `object_t.face` is set (`objects_set_face_graph` at `units_attach`: the graph has an emotions and/or a talk animation).

MOUT (object pack, `tools/halo2ps2/mouthconv.py`, `engine/src/world/mouth.c`): `"MOUT"`, u16 version 1, u16 0, u32 count, u32 pool offset; count x (u32 snd! tag id, u16 variant, u16 length, u32 offset); the bytes. `variant` is the STRM variant
(`streamconv`: one per chain head of each pitch range). `sound_impulse_start` with a unit as the source records the take and the start tick; `misc_tick` sets the unit's `face.talk` to byte `tick - start` every tick (0 when the line is over).

## 9. Verification

* `python3 -I tools/tests/test_animconv.py`: the compressed quaternion bit layout, quantisers, key reduction, a
  synthetic map (uncompressed and compressed animations, hinge/static/animated/scale channels, a node-less device
  graph, models with matching, mismatching and absent checksums, frame info) converted and read back with the reference
  sampler against the decoded frames (< 0.2 degrees, < 0.5 mm), loop/clamp semantics, dedupe, error handling for
  corrupt tag data; on a10 (skipped without the map) a looping idle starts and ends within a few degrees, walks are
  continuous with unit rotations and bone-length translations, a compressed cinematic matches its uncompressed twin.
* `make -C engine/tests`: `test_anim` checks the C reader against the writer's fixture, sampling against the reference
  sampler (12 frames of every animation incl. loops, negative, NaN-free huge frames), hand-computed door/blend/root
  motion values, shortest-path blending with antipodal quaternions and aliasing, then corrupts every byte and truncates
  at every length.
* Posing the skeleton with the section (forward kinematics) for the cyborg, elite and grunt idle and walk animations
  gives standing, walking bodies with feet planted and swinging, hands at the rifle, no noise.
