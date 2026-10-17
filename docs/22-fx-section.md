# `FXD0` and `FXTX` sections, version 1 (effects, particles, decals, lights, FX textures)

Output of `tools/halo2ps2/fxconv.py`, read by `engine/src/world/fxdef.c` (contract: `fxdef.h`; the bitmaps are read
by `world/tex.c`). Both are sections of one [`.hp2` pack](07-hp2-format.md), `assets/hp2/<map>.fx.hp2`:

| Section | Content |
|---|---|
| `FXD0` | the effect tables below, one per map |
| `FXTX` | a [`TEXS` blob](09-tex-section.md) (read by `tex_open`) with the textures of the sprite sheets and decal bitmaps; **bindings are keyed by bitmap tag id** |

Little-endian, every table 16-byte aligned, offsets relative to the start of the section, cross references are
Xbox tag ids (u32) or indices into the tables of the same section. Effects reference sounds (`snd!`), damage effects
(`jpt!`) and spawned objects (`garb`, `proj`, ...) **by tag id only**: the sound, weapon/damage and object converters
(workstreams G, D and the object pack) own those. No VU programs: the tables feed an EE-side billboard renderer.

## What goes in

`effect` tags, and what their events create: `particle` tags (sprite parameters, through the effect's particle
emitters), `decal` tags (with their "next decal in chain"), `light` tags, `particle_system` tags, `point_physics` tags of
particles and particle systems, and the `bitmap` tags of all of them (sprite sheets and sequences). Spawned object tags
(debris, projectiles) are not converted, only listed (`referenced_models`).

**Which effects** (`fxconv.effect_closure`): the gameplay set. Every tag of the groups `matg foot weap proj jpt! bipd vehi
eqip garb itmc` is scanned inside its own memory (a tag's root struct and all its nested blocks are contiguous in tag
space: the data block runs up to the next tag's data block, verified by the effect tags tiling exactly) for 16-byte tag
references of group `effe` that resolve to an effect tag. On a10 that finds 212 distinct effects (weapons 18 references, the 12
projectiles' material responses 169, `foot` material effects 26, globals 5, equipment 3, bipeds / vehicles / garbage 1 each); the
closure then adds the collision and death effects of the particles and the effects of the objects effects create: **219 effects
on a10 (of 255)**. `fxconv.py --all` converts every effect tag of the map (devices, scenery emitters, cinematics: +10..20 % effects,
+9 % FXTX on a10, and the object parts become reachable).

The gameplay set creates **no object** (`referenced_models(map)` therefore defaults to the whole-map set: 1..10 models per
map, `garb` debris and a few projectiles; keyed by `model` tag id as `modelconv` keys them).

## Source layouts (all verified on the ten campaign maps, see the docstring of `fxconv.py`)

Offsets are inside the tag's own data block; `ref` = 16-byte tag reference, `refl` = reflexive (count, address, pad).

| Tag | Size | Fields read |
|---|---:|---|
| `effe` | 64 | +0 flags, +4 s16 loop start event, +6 s16 loop stop event, +8 f32 maximum damage radius, +0x28 locations refl (32 B names), +0x34 events refl (68 B) |
| event | 68 | +4 skip fraction, +8 delay bounds, +0x10 duration bounds, +0x2C parts refl (104 B), +0x38 particles refl (232 B) |
| part | 104 | +0 create in, +2 violence mode, +4 location, +6 flags, +0x14 type class, +0x18 ref, +0x40 velocity bounds, +0x48 cone, +0x4C angular velocity bounds, +0x54 radius modifier bounds, +0x60 / +0x64 a / b scales |
| effect particle | 232 | +0 create in, +2 violence, +4 create (camera mode), +8 location, +0x0C yaw, +0x10 pitch, +0x14 offset, +0x20 direction vector (derived, not kept), +0x54 ref `part`, +0x64 flags, +0x68 distribution, +0x6C count bounds (s16), +0x70 distribution radius, +0x84 velocity, +0x8C cone, +0x90 angular velocity, +0xA0 radius, +0xB0 / +0xC0 tint ARGB lower / upper, +0xE0 / +0xE4 a / b scales |
| `part` | 356 | +0 flags, +4 bitmap, +0x14 physics, +0x38 lifespan, +0x40 fade in, +0x44 fade out, +0x48 collision effect, +0x58 death effect, +0x68 minimum size, +0x74 radius animation, +0x80 animation rate, +0x88 contact deterioration, +0x8C / +0x90 fade start / end size, +0x98 s16 first sequence index, +0x9A / +0x9C / +0x9E initial / looping / final sequence count, +0xA8 sprite size, +0xAC orientation, +0xD8 shader flags, +0xDA blend, +0xDC fade mode |
| `pphy` | 64 | +0 flags, +8 water gravity scale, +0xC air gravity scale, +0x20 density, +0x24 air friction, +0x28 water friction, +0x2C surface friction, +0x30 elasticity |
| `deca` | 268 | +0 u16 flags, +2 type, +4 layer, +8 next decal, +0x18 radius, +0x2C intensity, +0x34 / +0x40 colour lower / upper (RGB), +0x58 loop frame, +0x5A speed, +0x78 lifetime, +0x80 decay time, +0xC0 blend, +0xD8 bitmap, +0xFC maximum sprite extent |
| `ligh` | 352 | +0 flags, +4 radius, +8 radius modifier bounds, +0x10 falloff angle, +0x14 cutoff angle, +0x30 interpolation flags, +0x34 / +0x44 colour lower / upper (ARGB), +0xF4 duration, +0xFA falloff function |
| `pctl` | 104 + blocks | +0x38 physics, +0x48 update physics, +0x50 constants, +0x5C types (128 B): name, +0x20 flags, +0x24 initial count, +0x28 render mode, +0x2C radius, +0x54 creation physics, +0x5C constants, +0x68 states (192 B), +0x74 particle states (376 B) |
| `bitm` | 108 | +0 type, +0x54 sequences refl (64 B: name, first bitmap, count, sprites refl +0x34 of 32 B: bitmap index, +8 left right top bottom, +0x18 registration), +0x60 bitmap data (48 B, see `bitmaps.py`) |

Public tag documentation (Invader's definitions) was used as a cross-check of the field order; every offset above was then
confirmed on the data: strides by the exact tiling of the tags' memory (3134 effect tags, 108 particle systems), the effect
particle direction vector against yaw / pitch on 17 000+ emitters, enum values by every tag converting without an out-of-range
value, references by resolving to tags of the right group.

## `FXD0` layout

Header, 192 bytes: `char[4] "FXD0"`, `u16 version (1)`, `u16 reserved`, `u32 total size`, `u32 reserved`, then 17 pairs
`(u32 offset, u32 count)` at +0x10 in the order of the table below, the rest reserved. A table is `count` records
of the stated size at `offset`; counts are limited to 2^22.

| # | Table | Record | Sorted by tag id |
|---:|---|---:|:-:|
| 0 | effects | 48 | yes |
| 1 | locations (marker names, char[32]) | 32 | |
| 2 | events | 48 | |
| 3 | parts | 80 | |
| 4 | emitters (effect particles) | 128 | |
| 5 | particles (`part` tags) | 112 | yes |
| 6 | physics (`pphy`) | 48 | yes |
| 7 | decals | 96 | yes |
| 8 | lights | 80 | yes |
| 9 | pctl (particle systems) | 32 | yes |
| 10 | pctl types | 64 | |
| 11 | pctl states (system states) | 96 | |
| 12 | pctl particle states | 112 | |
| 13 | constants (f32 pool) | 4 | |
| 14 | bitmaps | 32 | yes |
| 15 | sequences | 8 | |
| 16 | sprites | 32 | |

(Only the tables marked sorted are looked up by tag id, binary search; `fxdef_open` rejects non-ascending or equal ids.)
Units: **seconds** for all times (30 ticks = 1 s, `FXD_TICKS_PER_SEC`), world units, radians, f32 colours 0..1 except the
RGBA8 emitter tints. Bounds are `(min, max)` with min <= max; retail data stores 128..245 bounds per map the wrong way
round (delay 0.75 / 0.70, angular velocity 6.28 / -6.28), the converter swaps them (Halo draws between the two whichever is
larger) and counts them.

**effect, 48 B**: `+0 u32 tag_id`, `+4 u32 flags` (1 deleted when attachment deactivates, 2 deterministic Xbox, 4 deterministic
PC, 8 disabled by blood setting), `+8 s16 loop start event`, `+0x0A s16 loop stop event` (-1 none), `+0x0C f32 damage radius`,
`+0x10 u32 first location`, `+0x14 u16 location count`, `+0x16 u16 event count`, `+0x18 u32 first event`.

**event, 48 B**: `+0 f32 skip` (0..1: probability the event is skipped), `+4 f32 delay[2]`, `+0x0C f32 duration[2]`, `+0x14 u32 first
part`, `+0x18 u16 part count`, `+0x1A u16 emitter count`, `+0x1C u32 first emitter`. Delay is the time from the effect's
start, duration the time over which the event's emitters spread their particles.

**part, 80 B**: `+0 u8 create in` (0 any, 1 air, 2 water, 3 space), `+1 u8 violence` (0 either, 1 violent only, 2 non-violent only),
`+2 u16 location` (marker index within the effect or 0xFFFF), `+4 u16 flags` (1 face down: decals), `+6 u8 kind` (1 damage
effect, 2 object, 3 light, 4 sound, 5 decal, 6 particle system), `+8 u32 tag id` of the created tag, `+0x0C char[4] group`
(`jpt!`, `ligh`, `snd!`, `deca`, `pctl`, or the object's group `garb`, `proj`, ...), `+0x10 u32 index` (decals / lights /
pctl table index for those kinds, else 0xFFFFFFFF), `+0x14 f32 velocity[2]`, `+0x1C f32 cone`, `+0x20 f32 angular velocity[2]`,
`+0x28 f32 radius modifier[2]`, `+0x30 u32 a scales`, `+0x34 u32 b scales` (raw bit fields: which effect scales apply;
zero on every retail part).

**emitter, 128 B**: `+0 u8 create in`, `+1 u8 violence`, `+2 u8 create` (0 any camera mode, 1 first person only, 2 third person
only, 3 first person if possible), `+4 u16 location`, `+6 u16 particle` (index into table 5), `+8 f32 yaw`, `+0x0C f32 pitch`
(the emitter's direction relative to the location), `+0x10 f32 offset[3]`, `+0x1C u32 flags` (1 stay attached to the
marker, 2 random initial angle, 4 tint from object colour, 8 interpolate tint as HSV, 0x10 across the long hue path),
`+0x20 u16 distribution` (0 start, 1 end, 2 constant, 3 buildup, 4 falloff, 5 buildup and falloff), `+0x24 s16 count[2]`,
`+0x28 f32 distribution radius[2]`, `+0x30 f32 velocity[2]`, `+0x38 f32 cone`, `+0x3C f32 angular velocity[2]`, `+0x44 f32
radius[2]`, `+0x4C u8 tint_lo[4]`, `+0x50 u8 tint_hi[4]` (RGBA8; Halo's ARGB floats), `+0x54 u32 a scales`, `+0x58 u32 b scales`,
`+0x5C u32 particle tag id` (== `particles[particle].tag_id`, checked on open). A random particle count is `count[0]..count[1]`
(retail 0..2000, almost always 1..30).

**particle, 112 B**: `+0 u32 tag_id`, `+4 u32 flags` (1 animate backwards, 2 animation stops at rest, 4 starts on random frame,
8 animate once per frame, 0x10 dies at rest, 0x20 dies on structure, 0x40 tint from diffuse, 0x80 dies in water, 0x100 dies in
air, 0x200 self illuminated, 0x400 / 0x800 random horizontal / vertical mirroring), `+8 u16 bitmap` (table 14 or 0xFFFF),
`+0x0A u16 physics` (table 6 or 0xFFFF), `+0x0C u32 collision effect`, `+0x10 u32 death effect` (tag ids or 0xFFFFFFFF; the
kinds at +0x54 / +0x55 say what they are: 7 effect (a table 0 entry), 1 damage effect, 4 sound), `+0x14 f32 lifespan[2]`,
`+0x1C f32 fade in`, `+0x20 f32 fade out`, `+0x24 f32 minimum size` (pixels), `+0x28 f32 radius animation[2]` (multiple of the
emitter radius at birth and at death, lerped over the life), `+0x30 f32 animation rate[2]` (sprite frames per second),
`+0x38 f32 contact deterioration`, `+0x3C f32 fade start size`, `+0x40 f32 fade end size` (pixels), `+0x44 s16 sequence
first / initial / looping / final` (see below), `+0x4C f32 sprite size` (world units per sprite pixel), `+0x50 u8 orientation`
(0 screen facing, 1 parallel to direction, 2 perpendicular), `+0x51 u8 blend` (0 alpha, 1 multiply, 2 double multiply, 3 add,
4 subtract, 5 min, 6 max, 7 alpha-multiply add), `+0x52 u8 fade mode` (0 none, 1 fade when perpendicular, 2 when parallel),
`+0x53 u8 shader flags` (bit 0 sort bias, 1 nonlinear tint, 2 don't overdraw first person weapon).

**physics, 48 B**: `+0 tag_id`, `+4 flags` (1 flamethrower collision, 2 collides with structures, 4 collides with the water
surface, 8 simple wind, 0x10 damped wind, 0x20 no gravity), `+8 f32 density`, air friction, water friction, surface friction,
elasticity (`+0x0C .. +0x18`), `+0x1C f32 air gravity scale`, `+0x20 f32 water gravity scale` (the file's values: **negative
pulls down**, -1 = full gravity: stones -0.999, cold smoke -0.02, hot smoke +0.47 which rises; sign convention unverified, argued
from those samples; the engine multiplies world gravity by `-scale`).

**decal, 96 B**: `+0 tag_id`, `+4 u16 flags` (1 geometry inherited by the next decal, 2 HSV colour, 8 no random rotation, 0x10 water
effect, 0x80 animation loop, 0x100 preserve aspect), `+6 u8 type` (0 scratch, 1 splatter, 2 burn, 3 painted sign), `+7 u8 layer`
(0 primary, 1 secondary, 2 light, 3 alpha tested, 4 water), `+8 u32 next decal tag` (chain; also in the table) , `+0x0C u16 bitmap`,
`+0x0E u16 blend` (as particles), `+0x10 f32 radius[2]`, `+0x18 f32 intensity[2]`, `+0x20 f32 colour_lo[3]`, `+0x2C f32 colour_hi[3]` (RGB),
`+0x38 f32 lifetime[2]` (seconds), `+0x40 f32 decay[2]`, `+0x48 s16 loop frame`, `+0x4A s16 animation speed`, `+0x4C f32 maximum
sprite extent`. A decal's bitmap has several sequences (retail: 1..4 single-sprite sequences, one per variant): the engine
picks a sequence at random.

**light, 80 B**: `+0 tag_id`, `+4 flags` (bit 0 dynamic), `+8 f32 radius`, `+0x0C f32 radius modifier[2]`, `+0x14 f32 falloff
angle`, `+0x18 f32 cutoff angle` (pi = omnidirectional), `+0x1C u32 interpolation flags`, `+0x20 f32 colour_lo[4]`,
`+0x30 f32 colour_hi[4]` (A R G B), `+0x40 f32 duration` (unit unverified: 3..180 on explosion lights, 0 else), `+0x44 u16
falloff function`. The colour runs from lower to upper bound over the light's life.

**particle system**: `pctl` record 32 B (`tag_id`, `u16 physics`, `u16 update physics` (0 default, 1 explosion), `u32 first type`, `u16 type
count`), **type** 64 B (`char[16] name`, `u32 flags` (bit 0 type states loop, 1 forward/backward, 2 particle states loop,
4 particles die in water, 5 in air, 6 on ground, 7 rotational sprites animate sideways, 8 disabled, 9 tint by effect colour,
10..15 the "scales with effect" bits, 16 / 17 not in first / third person), `u16 initial particle count`, `u8 render mode` (0
simple, 1 rotational), `u8 creation physics` (0 default, 1 explosion, 2 jet), `f32 radius`, `u32 first const`, `u16 const count`,
`u32 first state`, `u16 state count`, `u32 first particle state`, `u16 particle state count`), **state** 96 B (`name`,
`f32 duration[2]`, `transition[2]`, scale / animation rate / rotation rate multipliers, colour multiplier ARGB, radius multiplier, minimum particle
count, creation rate, `u16 creation physics`, `u16 update physics`, constants), **particle state** 112 B (`name`, `duration[2]`, `transition[2]`,
`u16 bitmap`, `u16 sequence`, `scale[2]`, `animation rate[2]`, `rotation rate[2]` (radians per second), `colour1[4]`, `colour2[4]` (ARGB),
`radius multiplier`, `u16 physics`, `u8 blend`, `u8 fade mode`, `u16 shader flags`, constants). Constants are slices of table 13.
This is **the explosion renderer's input** (grenades, rocket, needler, fuel rod, wraith, plasma): the structure is verified
(exact tiling, names, references), the *dynamics* are not: how Halo's simulation consumes duration, transition, scale and
creation rate is not recoverable from the data alone, so the values are exported verbatim for the effect renderer to approximate
(spawn `initial count` sprites of the first particle state with its scale / rotation rate / colour1 -> colour2, then the next
state after its duration; blend, bitmap and sequence per state).

**bitmap, 32 B**: `+0 tag_id`, `+4 u32 first sequence`, `+8 u16 sequence count`, `+0x0A u8 kind` (bitmap tag type: 0 2D, 3
sprites), `+0x0C u16 texture` (FXTX texture of the first converted image or 0xFFFF), `+0x0E u16 image count`. **sequence, 8 B**:
`u32 first sprite`, `u16 count`. **sprite, 32 B**: `+0 u16 texture` (FXTX index, 0xFFFF if the image was not converted), `+2 u16
image` (index in the bitmap tag), `+4 f32 u0 (left), v0 (top), u1 (right), v1 (bottom)` (0..1 of the texture, v0 = first row;
sprite sheets keep the sheet's texture, **uv are valid at any texture size**), `+0x14 f32 registration x, y` (as stored, units
unverified, -0.17..0.5). For plain 2D bitmaps (bullet holes, tread marks, a flare sheet: type 0) every sequence lists whole images
as sprites with the full rectangle (0, 0, 1, 1).

**Sequence selection of particles** is the one unverified field family: `first / initial / looping / final` are stored as the
tag has them. Read as sequence indices (initial sequence(s) once, looping ones repeatedly, final ones once; every sequence a list
of sprites played at the animation rate) they fit most retail particles, but 10 of 914 particles have a first index past the
bitmap's sequences and 38 have first + counts beyond them. `fxdef_sprite(bitmap, sequence, frame)` therefore clamps the sequence
(to the last sequence that has sprites) and takes the frame modulo the sprite count, so every combination draws something.

## `FXTX` (textures)

A [`TEXS` blob](09-tex-section.md) (PSMT8 / PSMT4 + CLUT, `texconv.convert_rgba`'s quantiser). **One texture per image**
a sprite points to, so a sprite sheet with two pages is two textures; `sprite.texture` is the index, a **binding per bitmap tag**
(`shader_id` = the bitmap tag id, `tex_find_binding`) names the bitmap's first converted image and carries the framebuffer
blend of its first user (TEXS blend codes) for convenience. Policy:

* sheets (bitmap type 3) keep **one mip** (a mip would blend neighbouring sprites); plain 2D bitmaps keep the usual chain;
* sheets up to 64 px keep their size, larger ones are halved once (Xbox mip 1 when present: 256 -> 128, 128 -> 64), clamped to
  `max side` 128, min 8; uv are normalised so a different texture size is invisible to the sprite tables;
* alpha: the pixels decide (opaque / punch-through / blend classes of `texconv`), whatever the blend function: the renderer picks
  the GS blend from the particle's `blend`; additive sprites with alpha in the source keep it;
* budget `--budget` (default 1 MiB of pixels + CLUTs, the 6 MiB texture pool also holds BSP, lightmap and object textures): if
  the estimate (PSMT8, sheets one mip) exceeds it the side cap drops to 64, then 32. Not needed on any campaign map.

The second map of particles (`secondary bitmap`: noise / distortion masks), cube maps of lights and lens flares are dropped
(ADR-004/005: no detail maps; lens flares and glow / contrail / light volume tags are not part of this workstream).

## Measured, all ten campaign maps (`fxconv.py`, default gameplay set)

"effects" is gameplay set / whole map (`--all`). Section sizes are raw; pack = `.fx.hp2` on disk (LZ4).

| map | effects | events | parts | emitters | particles | decals | lights | pctl | physics | bitmaps / sprites | textures (PSMT4) | FXD0 KiB | FXTX KiB | pack KiB | s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|---:|---:|
| a10 | 219 / 255 | 389 | 323 | 1330 | 90 | 32 | 7 | 5 | 23 | 75 / 712 | 112 (46) | 280 | 637 | 318 | 0.8 |
| a30 | 274 / 298 | 487 | 398 | 1639 | 89 | 33 | 6 | 5 | 18 | 74 / 709 | 112 (45) | 335 | 637 | 322 | 0.8 |
| a50 | 262 / 291 | 458 | 405 | 1464 | 93 | 33 | 7 | 8 | 18 | 78 / 721 | 117 (46) | 315 | 680 | 344 | 0.8 |
| b30 | 259 / 288 | 449 | 377 | 1469 | 89 | 32 | 6 | 6 | 19 | 75 / 713 | 113 (46) | 310 | 639 | 322 | 0.8 |
| b40 | 314 / 340 | 557 | 475 | 1835 | 102 | 33 | 9 | 9 | 23 | 82 / 737 | 121 (46) | 381 | 700 | 366 | 0.9 |
| c10 | 253 / 300 | 442 | 367 | 1407 | 87 | 32 | 6 | 4 | 16 | 74 / 707 | 112 (45) | 298 | 637 | 322 | 0.8 |
| c20 | 232 / 261 | 414 | 361 | 1349 | 83 | 31 | 6 | 4 | 16 | 73 / 700 | 110 (44) | 286 | 632 | 316 | 0.8 |
| c40 | 349 / 394 | 624 | 519 | 2069 | 98 | 33 | 7 | 8 | 19 | 81 / 785 | 121 (46) | 421 | 724 | 378 | 0.9 |
| d20 | 297 / 350 | 523 | 429 | 1715 | 93 | 36 | 6 | 5 | 18 | 79 / 780 | 119 (49) | 354 | 684 | 350 | 0.8 |
| d40 | 297 / 357 | 524 | 428 | 1734 | 90 | 32 | 6 | 5 | 18 | 76 / 770 | 115 (45) | 356 | 676 | 346 | 0.8 |

* **Largest**: c40, FXD0 421 KiB + FXTX 724 KiB raw (378 KiB packed); a10: 280 + 637 KiB. The FXTX pixels + CLUTs are 632..724 KiB
  (a10 631 KiB), well under the 1 MiB default budget (the whole-map set adds up to ~120 KiB: 806 KiB at most, d20); the largest
  texture side is 128. About 60 % of FXD0 is emitters (128 B each).
* Parts of the gameplay set: sounds ~ 50 %, decals ~ 40 %, damage effects, lights and particle systems the rest (a10:
  155 sounds, 147 decals, 8 damage effects, 8 lights, 5 particle systems).
* Conversion: 0.8..0.9 s per map for the gameplay set (including all textures), whole-map sets the same.
* Notes the converter reports: 1..2 parts and 1..2 emitters per map with a null reference are dropped (they create nothing); 128..245
  bounds per map are swapped; one retail effect (Scorpion secondary fire bullet) names marker 1 of 1 (the reference is
  cleared, the part drawn at the object's origin); one sprite rectangle overhangs the image by 1/16 (clamped).

## What is verified and what is not

| Fact | Status |
|---|---|
| Strides and reflexives of effect (64 / 68 / 104 / 232), `part` 356, `deca` 268, `ligh` 352, `pphy` 64, `pctl` (104 / 128 / 192 / 376) | verified: the memory of every tag tiles exactly (3134 effects, 108 particle systems, 1052 particles, 428 decals, 507 lights, 260 physics) |
| Event delay / duration / skip positions | verified by plausibility (0.8..0.9 s delays, 2..4 s durations on a rocket impact) and the public definition |
| Emitter yaw / pitch / vector, count, distribution, tint | verified (the vector equals the yaw-pitch direction on every emitter) |
| Particle fields up to the sprite size and the shader block | verified by value ranges and enum bounds |
| Sequence selection, `radius animation` semantics, `fade start / end size` units, `minimum size` units | **unverified** (see above) |
| Physics gravity sign, units of friction values | unverified (argued from samples); the engine multiplies world gravity by `-air gravity scale` |
| Light `duration` unit, the `falloff function` meaning | unverified |
| Particle system dynamics (durations, transitions, multipliers, creation rate) | structure verified, **dynamics unverified** |
| Decal sequence = random variant | unverified (4 single-sprite sequences on blood and bullet holes) |
| FXTX alpha handling per blend, PSMT8 CLUT swap | as `docs/09`; CLUT swap confirmed on console earlier, the rest needs a render |

## Reader

`engine/src/world/fxdef.h`: `fxdef_open` validates every table (record ranges, 16-byte alignment, ascending unique ids, every
index and range of the hierarchy effect -> event -> part / emitter and bitmap -> sequence -> sprite, enum values, string
terminators, finite floats, sprite rectangles 0..1, emitter particle index = tag id, locations below the effect's count, sprite
image below the bitmap's image count), so the accessors cannot read out of bounds. API: `fxdef_find_effect` /
`fxdef_get_effect` / `fxdef_get_event` / `fxdef_get_part` / `fxdef_get_emitter` / `fxdef_location_name`;
`fxdef_find_*` and `fxdef_get_*` for particles, physics, decals, lights, particle systems (types, states, particle states,
`fxdef_const`) and bitmaps (`fxdef_get_sequence`, `fxdef_get_sprite`, the total `fxdef_sprite`); `fxdef_bitmap_binding(tex_section, bitmap tag id)`
(the FXTX binding) and `fxdef_check_textures(section, texture count)` (cross-check after both sections are open).
The Python twin is `fxconv.parse_section`. Tests: `tools/tests/test_fxconv.py` (synthetic map with every tag kind, closure,
corruption of every layout field, textures, serialisation, plus a10 numbers when the map is present) and
`engine/tests/test_fxdef.c` (hand-checked values of the same world, every table header moved / misaligned / oversized, 100+
targeted field corruptions each of which must be refused, truncation at every length, every byte flipped three ways with the
accepted sections walked completely under ASan/UBSan; also run by hand over the real a10 pack, 15 000 flips).

## For the integrator

* Pack per map: `python3 -I tools/halo2ps2/fxconv.py assets/decompressed/<map>.map -o assets/hp2/<map>.fx.hp2` (add to
  `scripts/build-world.sh`; `--all` for the device / scenery effects, `--budget` for the texture bytes).
* Load `FXD0` and `FXTX` from the pack (`hp2_find`), `fxdef_open` and `tex_open`, then `fxdef_check_textures(&fx, tex.texture_count)`.
  The texture pool: FXTX is at most 0.8 MiB of the 6 MiB; upload sprite sheets on demand through the texture cache like the others.
* Cross references that leave this pack are Xbox tag ids: sounds (`snd!`, `SND0`), damage effects (`jpt!`, `WEAP`), objects (`garb`, `proj`, ...
  in `SCEN`/`OBJ`); look them up in the owning pack. Add `fxconv.referenced_models(map)` to the roots of `objpack.py`.
* Effects name markers (`fxdef_location_name`: "" is the object's origin, others are model markers by name).
* What the renderer needs from `fxd_particle` + `fxd_emitter`: spawn `count` sprites over the event's duration (`distribution`), each
  with velocity / cone / angular velocity bounds, radius `lerp(radius_anim[0], radius_anim[1], age / life)` times the emitter's radius,
  tint `lerp(tint_lo, tint_hi)`, fade in / out times, a sprite from `fxdef_sprite(bitmap, sequence, frame)` at `anim_rate` frames per second,
  the GS blend from `blend`, gravity from the physics record.
