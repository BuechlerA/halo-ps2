# `SND0` / `SNDD` sections, version 1 (gameplay sounds as SPU2 ADPCM)

Output of `tools/halo2ps2/soundconv.py` (SPU2 ADPCM codec: `tools/halo2ps2/vag.py`), read by `engine/src/audio/snddef.c`
(contract: `engine/src/audio/snddef.h`). Stored as the `SND0` and `SNDD` sections of a [`.hp2` pack](07-hp2-format.md),
`assets/hp2/<map>.snd.hp2`. `SND0` is a table section (little-endian, every table 16-byte aligned, offsets relative to the
section start); `SNDD` holds one ready-made **audsrv sample** per permutation (below). Cross references are Xbox tag ids, as in the other Phase 3 packs.
Nothing in this section refers to models or animations, so the converter exports no `referenced_models()` /
`referenced_anims()` roots.

## What goes in
* **Sounds** (`snd_sound`): every `sound` (`snd!`) tag reachable from the gameplay tags (below), keyed by tag id, sorted
  ascending (binary search): class, tag flags, distances, random pitch bounds, cone angles, gain modifiers, promotion
  sound, name, and its pitch ranges and permutations. A sound that does not make it into SPU2 RAM stays in the table with a
  flag saying why (`DROPPED` by the budget, `STREAMED` dialogue/music, `UNSUPPORTED` source format) and no data, so the
  engine can tell "silent on purpose" from "unknown id".
* **Pitch ranges** (`snd_range`) and **permutations** (`snd_perm`): per permutation its place in `SNDD` (byte offset, size),
  decoded frame count, playback rate, loop bounds, gain, skip fraction, the next-permutation link.
* **Looping sounds** (`sound_looping`): per `lsnd` tag its tracks (start / loop / end / alternate loop / alternate end sound
  ids, gain, fades) and detail sounds (sound id, random period, gain, yaw/pitch/distance bounds). The converter records
  the structure; the engine plays the `loop` sounds. The sounds a track names are ordinary sounds of the table; those used as
  `loop` or `alt_loop` carry `SND_F_LOOP` and their permutations loop.
* **Not included**: dialogue and music (classes 19 32 44 45 46 47 48, paths below `sound\dialog\` and `sound\music\`) are
  Phase 4 streaming; they appear as `STREAMED` entries without data. `sound_environment` (reverb) is not converted.

## Layout
Header, 128 bytes: `char[4] "SND0"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (must equal the section size),
`u32` counts at `0x0C` sounds, `0x10` ranges, `0x14` permutations, `0x18` loops, `0x1C` tracks, `0x20` details,
`0x24` SNDD size in bytes (the reader is given the real size and they must agree), table offsets at `0x28` sounds, `0x2C` ranges,
`0x30` perms, `0x34` loops, `0x38` tracks, `0x3C` details, statistics at `0x40` budget, `0x44` dropped sounds, `0x48` dropped
permutations of kept sounds, `0x4C` sounds at half rate, `0x50` streamed sounds; the rest is zero.

| Table | Element | Order |
|---|---|---|
| sounds | 128 B | strictly ascending tag id |
| ranges | 32 B | per sound consecutive, in the tag's order |
| perms | 64 B | per sound consecutive, range by range |
| loops | 64 B | strictly ascending tag id |
| tracks | 48 B | per loop consecutive |
| details | 48 B | per loop consecutive |

**Sound, 128 B**: `u32 tag_id`, `u32 tag_flags`, `u8 class`, `u8 flags` (`SND_F_LOOP 1`, `DROPPED 2`, `STREAMED 4`,
`DEGRADED 8` = rate halved to fit, `UNSUPPORTED 16`), `u16 rate` (nominal Hz of the permutations, 0 without data),
`u32 range_first`, `u16 range_count`, `u16 perm_count`, `u32 perm_first`, `u16 perms_dropped`, `u16 promotion_count`,
`u32 promotion_tag` (0xFFFFFFFF none), `f32` at `0x20`: min distance, max distance, skip fraction, pitch lo, pitch hi, inner cone,
outer cone, outer cone gain, gain modifier; `f32[6]` at `0x44` scale modifiers (skip, gain, pitch at scale 0, then at scale 1),
`u32 max_play_ms` at `0x5C`, `char[32] name` at `0x60` (the last 31 characters of the tag path). Sounds with data have
`range_count > 0` and `perm_count > 0`, the ranges partition the permutations in order.

**Range, 32 B**: `f32 natural_pitch, bend_lo, bend_hi`, `u16 perm_first` (relative to the sound's first permutation), `u16 perm_count`
(0 is allowed when a budget drop emptied the range), `char[16] name`.

**Permutation, 64 B**: `u32 offset` (of the audsrv sample, i.e. of its `APCM` header, inside `SNDD`, 16-aligned), `u32 size` (bytes of the whole sample: 16 header +
blocks + 16 terminator, a multiple of 16, >= 48), `u32 frames` (decoded samples, also in the header; the last block may hold padding up to `(size / 16 - 2) * 28`),
`u32 rate` (Hz at original pitch), `u32 loop_start`, `u32 loop_end` (bytes relative to the first block, `0 .. size - 32` with `SND_P_LOOP`, else both 0), `f32 gain`, `f32 skip`, `u16 next` (index within the
same pitch range of the permutation to play after this one, 0xFFFF none), `u16 flags` (`SND_P_LOOP 1`, `SND_P_FOLDED 2` = stereo source folded),
`u16 range`, `u16 src_index` (the tag's own position, debugging), `char[16] name`, `u32 src_bytes` (Xbox size), 4 reserved.

**Loop, 64 B**: `u32 tag_id`, `u32 flags`, `f32 param`, `u16 track_count`, `u16 detail_count`, `u32 track_first`, `u32 detail_first`,
`char[32] name` at `0x20`. **Track, 48 B**: `u32 flags`, `f32 gain, fade_in, fade_out`, `u32 start, loop, end, alt_loop, alt_end` (sound tag ids), 12 reserved.
**Detail, 48 B**: `u32 sound`, `f32 period_lo, period_hi, gain`, `u32 flags`, `f32 yaw_lo, yaw_hi, pitch_lo, pitch_hi, dist_lo, dist_hi`, 4 reserved.

**SNDD**: the permutations as audsrv samples back to back, each starting at a 16-byte boundary, the section padded to 64 bytes. The engine passes
`sndd + offset` and `size` straight to `audsrv_load_adpcm`. Sample layout (little-endian), **compared byte for byte with ps2sdk's `adpenc` output**:

| bytes | content |
|---|---|
| 0-3 | `'APCM'` |
| 4, 5 | `1`, `1` |
| 6 | loop flag: 1 looping, 0 one-shot (= `SND_P_LOOP`) |
| 7 | 0 |
| 8-11 | `u32` pitch word = **floor**(rate * 4096 / 48000) (22050 Hz -> 0x759 = 1881; 1881.6 is truncated, not rounded) |
| 12-15 | `u32` sample count (= `frames`) |
| 16 .. | SPU2 ADPCM blocks (16 B each), then one terminator block: the predictor/shift byte of the last block, flags `7`, 14 zero bytes |

Block flags: all 0 except the first block of a loop (`6` = loop start) and the last block (`1` for a one-shot; `3` = end + repeat for a loop, so the jump to the
loop start happens on the last block and the terminator is never played). adpenc writes `0` on the last block of a sample whose length is a multiple of 28 (the terminator
then does the stop/jump, a loop plays 28 extra samples of ringing before it wraps) and `1` otherwise; its first block is `6` for a loop. A single-block loop carries `7`.
The SPU2 resident set is the blocks plus the terminators (sum of `size - 16`, the header is not uploaded) and is <= 1.4 MiB rounded down to 64 bytes (1,467,968 bytes) on every
map; `SNDD` is larger by 16 bytes per permutation and padding.

## Source tag layouts (verified)
Everything the converter reads is documented with its checks in the `soundconv.py` docstring; the short version, all confirmed on the
11,559 `snd!` and 548 `lsnd` tags of the ten campaign maps (`soundconv.py MAP --check` repeats the structural checks):

* `snd!` header 0xA4 bytes (class `+0x04`, rate code `+0x06`, distances `+0x08/0x0C`, pitch bounds, cones, channels `+0x6C`, format
  `+0x6E`, promotion sound `+0x70`, max play ms `+0x84`, pitch ranges `+0x98`), pitch range 72 B, permutation 124 B (name, skip, gain,
  compression, next index, owner id at `+0x34/+0x3C`, sample bytes `+0x40`, sample offset in the map file `+0x48`). Every permutation passes:
  owner id equals the sound, compression equals the tag format (always Xbox ADPCM, no PCM, no Ogg), sample range inside the map file before the
  tag region, size a whole number of blocks, no two permutations share a range. 1 / 2 / 4 pitch ranges: 11,545 / 2 / 12 tags.
* The **class values** were checked against tag paths class by class (weapon fire/ready/reload/empty/charge/overheat/idle = 4..10, footsteps =
  18, unit dialog = 19, music = 32, first person damage = 39, game events = 50, ...); 44 and 47 are scripted dialogue.
* `lsnd`: 0x54 byte header, `tracks` (160 B) at `+0x3C`, `details` (104 B) at `+0x48`; the tag extent equals `0x54 + 160 * tracks + 104 * details`
  on all 548 tags. Track sound references at `+0x30 / +0x40 / +0x50 / +0x80 / +0x90`.
* Tag references are 16 bytes (`group, path pointer, 0, tag id`); the path pointer always equals the tag index entry's path address, which
  is what makes the reference scan below free of false positives.

## Xbox ADPCM (verified)
IMA ADPCM with the standard 89-entry step table. Mono block = 36 bytes: `s16` predictor, `u8` step index, `u8` 0, 32 bytes of nibbles (low nibble
first). A block decodes to **64 samples**: the header predictor is sample 0, nibbles 0..62 give samples 1..63, nibble 63 is always 0 (padding).
Stereo block = 72 bytes in the MS IMA layout: left header, right header, then eight groups of 4 bytes left / 4 bytes right.

How this was established, without trusting any description: (1) the step index stored in block *k+1* must equal the index the decoder reaches
after processing block *k*; it matches on **39,421,073 of 39,421,073** block boundaries of all **60,181** permutations of the ten maps (19.9 M
of them in the 11,440 stereo permutations) only for "63 nibbles, low nibble first, MS-style stereo groups" (a 62- or 64-nibble rule matches
2 %, the "36 + 36 bytes" stereo layout 2-58 %). (2) The predictor in block *k+1* is the *source's* sample 64 (k+1); our decode of block *k* ends
within a few LSBs of it: `boundary_ratio` (mean |header(k+1) - decoded sample 63| / mean |decoded 63 - decoded 62|) has median 1.00, 99th
percentile 1.45 over the 13,118 permutations of the 3,955 gameplay sounds of all maps; the single worst (3.40) is a step-like onset in the
source at a block edge (472 -> 5777 within one sample), the neighbouring 100 boundaries of that sound agree to tens of LSBs.
(3) Waveform sanity on those 13,118 permutations (9,967 s of audio): no permutation clips more than 1 % of its samples (1,074 touch full scale
on a few samples, hot masters). The plausibility check (clipping > 1 %, DC offset > 2 % of full scale and > 20 % of the RMS, boundary ratio > 3)
flags exactly 7 distinct permutations, which recur in 55 sound/map instances: the three plasma-hit sounds (mean +3,100..+3,300 on an RMS of
~7,800), the three flood infector death sounds (mean -1,550..-2,600) and the footstep above; all have boundary ratios near 1, i.e. they are
asymmetric transients in the source, not decoder drift. Median first-difference energy ratio 0.11 (white noise would be 1.0; 1,458 permutations, 11 %, are noise-like because they *are*
broadband: hiss, static, dry-fire clicks, footstep scuffs).
(4) Durations: the tag's `max play ms` equals `floor(longest permutation bytes * 2000 / (rate * channels))` on 98 % of sounds, i.e. the tag counts
2 samples per byte; the decoded duration (64 samples per 36 bytes) is therefore 64/72 = 0.8889 of the declared value, measured 0.8892 median and
within 0.5 % of 8/9 on 99.4 % of the 3,955 sounds. The Xbox tag has no sample-count field of its own.

## SPU2 ADPCM (`vag.py`)
16-byte blocks of 28 samples: `byte 0` = predictor (high nibble 0..4) and shift (low nibble 0..12), `byte 1` = flags (`1` loop end/stop, `2` repeat,
`4` loop start), 14 bytes of nibbles, low nibble first. (`adpenc`'s blocks decode with `vag.py` at 33-35 dB on a synthetic tone and our encoder gets 35-39 dB on the same
input: same block format; its first block uses filter 1, ours filter 0, see below.) Decode: `s = clamp16(((nib << 12) >> shift) + ((f0*s1 + f1*s2 + 32) >> 6))` with `(f0, f1)` =
`(0,0) (60,0) (115,-52) (98,-55) (122,-60)`.
* **Encoder**: closed loop (it tracks the decoded values), per block the (filter, shift) with the smallest squared error. The fast search
  tries, for each filter, the shift that covers the open-loop residual and its two neighbours (15 trials instead of 65); on 42 real sounds
  (856,896 samples) it produced exactly the same SNR as the exhaustive 65-trial search (difference 0.000 dB; `--exhaustive` is available).
* **Block flags**: `vag.encode` writes one-shots as 0 everywhere except 1 on the last block and loops as 4 on the first, 3 on the last; `wrap_audsrv` rewrites them to the
  audsrv pattern above (first block of a loop 6).
* **First block uses filter 0.** Filter 0 ignores the history, so a voice that jumps back to the loop start from any decoder state decodes block 0
  exactly like a fresh voice. Tested: the second pass of every one of the 65 resident loop permutations of the ten maps is bit-identical to the first.
* **Loop length**: the SPU2 loops whole blocks, so a looping sound needs a multiple of 28 samples. The converter shrinks the (already decimated) loop
  periodically (wrap-around windowed-sinc, less than 28 samples, < 0.15 % on every real loop: rate / nominal >= 0.9988) and lowers the recorded `rate`
  by the same factor, so the seam stays continuous and the pitch is unchanged. A loop whose source is not periodic has a seam jump of its own: 40 of 65 real
  loops start and end at silence (jump 0), the worst is a flamethrower "fire" sound whose *source* jumps by 9677 at the wrap (6709 after conversion).
* **SNR** against the PCM fed to the encoder, over all 1,999 permutations that went into the ten packs (22.05 kHz and 11.025 kHz): min 17.1 dB,
  median 24.2 dB, mean 25.4 dB; at full rate on 42 sampled sounds mean 30.3 dB (hiss-like sounds 19.7 dB, voices and impacts 29-40 dB). 4-bit ADPCM
  is a ~25-30 dB codec; the numbers are what the codec gives, not a bug.
* The rounding term (`+ 32`) and the shift > 12 behaviour are taken from the hardware references; they have not been compared with a console (ADR-022
  defers that), the encoder uses the same decoder so a different rounding would only change the output by an LSB per sample.

## Conversion pipeline
1. Parse and validate the tag (`read_sound`), find the sample range in the decompressed map file.
2. Xbox ADPCM -> PCM; stereo is folded to mono as `(l + r) >> 1`.
3. Sample rate: sources are 22050 or 44100 Hz. 44100 Hz sources are decimated by 2 (49-tap Blackman-Harris windowed sinc, cutoff 0.92 of the output
   Nyquist, DC gain 1, no time shift; >40 dB down above the output Nyquist, tested). A sound the budget degrades is decimated once more (11025 Hz).
4. Looping sounds are shrunk to a multiple of 28 (above) with periodic (wrap-around) filtering.
5. SPU2 ADPCM encode, then `wrap_audsrv` (header, flags, terminator). The size is known before encoding (`ceil(frames / 28) * 16 + 16`), which is what the budget planner uses.
Encoded permutations can be cached (`--cache DIR`, keyed by the sha1 of the source bytes and the conversion parameters; maps share most weapon and
effect sounds). Output is deterministic: a cold run and a cached run write byte-identical packs.

## Reachability: what "gameplay" means
The tag index has no usage information, so references are found by scanning the extent of a tag (from its data address to the next tag's) for 16-byte
tag references, accepting one only when its group, tag id **and path pointer** all agree with the index entry (0 false positives: every reference
found has a matching path pointer on all maps). The traversal starts at **every `weap proj jpt! effe bipd matg eqip unhi` tag** of the map and
follows references through those groups plus `antr` (footsteps, reload and melee sounds are animation frame events), `lsnd`, `snd!` (promotion sounds)
and `foot` (material effects); models, bitmaps, actors, dialogue (`udlg`), vehicles, devices, scenery and sound scenery are leaves. `unhi`
(unit HUD interface) and `eqip` carry the shield/health feedback (class 39) and pickup (class 50) sounds; vehicles, doors, sound scenery and
ambient `lsnd` can be added with `--roots <tag ids or exact paths>` (a root is always entered, whatever its group). `--roots` also narrows the set:
for example the player biped + weapon tags + globals give a footstep set for one species only and leave most of the budget to weapons.

Reached per map (all ten): 269-468 gameplay sounds with 795-1,585 permutations, 9.0-15.7 MiB at the base rate (the Xbox maps hold 55-165 MiB
of ADPCM in total, almost all dialogue and music).

## Budget (1.4 MiB per map)
`plan_budget` removes, one step at a time, the option with the smallest *loss per byte saved* until the set fits:
* dropping a permutation of a sound that keeps *n* >= 2 loses `weight * 2^-n` (the largest permutation of the sound goes first);
* dropping the last permutation loses the sound's whole `weight` (it becomes silent, flag `DROPPED`);
* halving a sound's rate (all permutations once, 22050 -> 11025 Hz, flag `DEGRADED`) loses 15 % of its weight (40 % for weapons) and saves about half.
`weight` = class weight / sqrt(number of gameplay sounds of that class), except the *critical* classes (weight >= 90: weapon fire, ready, reload,
empty, charge, overheat, idle and first-person damage) which keep their full weight on every sound. Class weights: weapons 100, first person damage 90,
projectile detonation 80, game events 60, projectile impact 45, footsteps 40, vehicles 25, object / particle impacts 20, doors and machinery 10, ambience 8,
anything else 10 (`CLASS_WEIGHT`; `--weight 18=60 ...` overrides).
Ties break on the lower tag id, so the result is deterministic. `--drops` lists every dropped sound, every dropped permutation, every degraded and
every streamed sound; `--json FILE` writes the same plus the per-permutation encoder statistics.

| map | gameplay sounds / permutations | all perms at base rate | SPU2 resident (blocks + terminators) | SNDD (with headers) | kept sounds / perms | dropped sounds | dropped perms of kept sounds | half rate | loops resident / total | streamed | SND0 | pack |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a10 | 333 / 1124 | 9.23 MiB | 1,461,632 B (99.6 %) | 1,466,240 B | 226 / 285 | 107 | 670 | 185 | 6 / 6 | 2 | 67.5 KiB | 1322 KiB |
| a30 | 360 / 1169 | 9.54 MiB | 1,461,552 B (99.6 %) | 1,465,152 B | 208 / 224 | 152 | 634 | 158 | 5 / 6 | 1 | 66.4 KiB | 1256 KiB |
| a50 | 416 / 1404 | 11.90 MiB | 1,465,584 B (99.8 %) | 1,469,248 B | 213 / 227 | 203 | 668 | 164 | 6 / 7 | 1 | 73.9 KiB | 1258 KiB |
| b30 | 418 / 1409 | 11.91 MiB | 1,466,976 B (99.9 %) | 1,470,528 B | 204 / 219 | 214 | 618 | 151 | 6 / 7 | 1 | 73.3 KiB | 1274 KiB |
| b40 | 427 / 1426 | 12.39 MiB | 1,465,696 B (99.8 %) | 1,468,672 B | 170 / 183 | 257 | 466 | 114 | 7 / 8 | 2 | 71.4 KiB | 1228 KiB |
| c10 | 353 / 1085 | 10.79 MiB | 1,465,808 B (99.9 %) | 1,468,928 B | 175 / 192 | 178 | 470 | 118 | 6 / 11 | 3 | 63.3 KiB | 1274 KiB |
| c20 | 269 / 795 | 9.04 MiB | 1,451,792 B (98.9 %) | 1,454,272 B | 138 / 152 | 131 | 279 | 80 | 8 / 13 | 2 | 49.2 KiB | 1242 KiB |
| c40 | 466 / 1585 | 15.75 MiB | 1,447,664 B (98.6 %) | 1,450,304 B | 149 / 164 | 317 | 372 | 87 | 7 / 12 | 3 | 75.0 KiB | 1216 KiB |
| d20 | 468 / 1568 | 14.63 MiB | 1,463,808 B (99.7 %) | 1,466,176 B | 133 / 146 | 335 | 277 | 73 | 8 / 9 | 3 | 73.3 KiB | 1230 KiB |
| d40 | 445 / 1553 | 14.87 MiB | 1,462,656 B (99.6 %) | 1,465,728 B | 170 / 189 | 275 | 462 | 111 | 6 / 11 | 2 | 74.3 KiB | 1246 KiB |

What the budget dropped, over the ten maps (2,169 whole sounds in total, listed per map by `--drops`): object impacts (the scripted-animation sounds
of riders, drivers and body falls, class 13: 1,369 of the dropped sounds), unit footsteps (601: surface variants of Elite / Grunt / Flood, then
Marine, on the maps with many species), projectile impacts (94), doors (44), particle impacts (36), vehicle engines (16) and projectile detonations
(9, the large carrier / explosion sounds). **No weapon sound (classes 4-10), no first-person damage sound and no game-event sound is dropped on any map**;
a few weapon sounds lose permutations (a10: weapon fire keeps 15 of 18 permutations) or play at 11025 Hz. On a10 all 111 footstep sounds survive (129 of their 590
permutations, all at 11025 Hz); on d20 only about 22 of 164 do and all object impacts are gone, because d20's weapon set alone takes about 1.2 MiB.
Per-class tables for every map are printed by the converter.

The SND0 section is 49-75 KiB per map (dominated by the 128-byte sound records: dropped sounds keep a record).

## Reader API (`audio/snddef.h`)
```c
snd_status snd_open(snd_section *s, const uint8_t *snd0, size_t size, uint32_t sndd_size);   /* validates everything */
int        snd_find(const snd_section *s, uint32_t tag_id);                  /* sound index or -1 */
snd_sound  snd_get_sound(const snd_section *s, uint32_t index);              /* class, flags, distances, pitch, cones, ... */
snd_range  snd_get_range(const snd_section *s, const snd_sound *snd, uint32_t k);
snd_perm   snd_get_perm(const snd_section *s, const snd_sound *snd, uint32_t k);   /* offset, size, frames, rate, loop, gain, skip, next */
int        snd_find_loop(const snd_section *s, uint32_t tag_id);             /* sound_looping index or -1 */
snd_loop   snd_get_loop(...);  snd_track snd_get_track(...);  snd_detail snd_get_detail(...);
uint32_t   snd_check_vag(const snd_section *s, const uint8_t *sndd, size_t n);   /* APCM headers, flags, terminators vs tables: 0 ok, else 1 + bad permutation */
```
`snd_open` checks sizes and alignment of every table, sort order (binary search), range/permutation partitions, `next` and `range` indices,
every permutation inside `SNDD` and block aligned (>= header + 1 block + terminator), frame count consistent with the size, loop bounds, flags, rates (1..48000), finite floats,
terminated names; on failure the section is left empty and every accessor returns a neutral record. The host test (`engine/tests/test_snddef.c`)
walks a fixture written by the Python tests, truncates it at every length, flips every byte three ways, mangles every table and about 60 targeted fields, flips
every `SNDD` byte for the checker, and also opens, walks, ADPCM-checks and byte-flips (every 61st byte) every real `assets/hp2/*.snd.hp2` it finds; ASan/UBSan clean.

Playing a permutation: pick a permutation of the sound (skip fraction and gain are per permutation, `next` chains within the pitch range), upload or address
`SNDD + offset` (an audsrv sample: pitch and loop are in its header), volume/pan from the distance fields.

## Debugging aids (`soundconv.py`)
* `MAP --check`: structural checks of every `snd!` / `lsnd` tag (0 problems on all ten maps).
* `MAP --survey N`: decode N gameplay sounds, print duration, peak/RMS/99th percentile in dB, DC offset %, clipping %, noise ratio, block-boundary ratio
  and flag implausible ones (no ears needed).
* `MAP --inspect TEXT [--wav-dir DIR] [--converted]`: statistics for every permutation of the sounds whose path contains TEXT, the decoded/declared duration
  ratio, and with `--wav-dir` the decoded source as a (stereo-preserving) 16-bit `.xbox.wav`, with `--converted` also the re-decoded SPU2 data as `.spu2.wav`
  plus its SNR against the PCM that was encoded.
* `PACK --info`: one-line summary of a `.snd.hp2`.
* `vag.py wav in.vag out.wav` decodes any VAGp / raw block file; `vag.py selftest`.

## Not verified, approximated
* Field **names** `+0x28` "random gain modifier", `+0x2C` "maximum bend per second", the six scale modifiers and the `lsnd` float at `+0x20` come from public tag
  documentation; the data is consistent with them (value ranges above) but their runtime use is not confirmed. They are stored and exposed verbatim.
* `next permutation index` is treated as an index within the same pitch range (all in-range on every map); what the engine does with chains is Halo
  behaviour not checked here. The promotion sound is stored, not interpreted.
* PCM16 sources are handled by the converter but no retail sound uses them (all 11,559 are Xbox ADPCM): that path is unit-untested against real data.
* Stereo folding is an average; out-of-phase content would cancel (not observed to matter in the gameplay set).
* Looping uses the whole permutation; Halo's loop sounds may be crossfaded by the original engine (unknown).
* The budget weights are a heuristic; nothing was listened to (no ears, ADR-022); the sizes, drop lists and SNRs are measured.
* SPU2 decode rounding, pitch register scaling and voice behaviour at loop ends have not been checked on hardware or PCSX2's SPU2.
* Source frame counts include the up to 63 padding samples of the Xbox encoder (the tag has no sample count), so a sound can end with a few ms of near-silence.

## What the integrator wires
* `scripts/build-world.sh`: `python3 -I tools/halo2ps2/soundconv.py assets/decompressed/<map>.map -o assets/hp2/<map>.snd.hp2 --cache assets/hp2/.sndcache`
  (about 7 s per map cold, 1 s from the cache, 21 s for all ten maps with a shared cache). Pass `--roots` to narrow the set (player biped, weapons) or
  add vehicles; `--budget` changes the resident size. cfg key `snd` (ADR-031) names the pack; no other keys.
* Keep `SNDD` in RAM (about 1.45 MB per map) and, for each sound the game plays for the first time (or all at level load), call
  `audsrv_load_adpcm(&sample, sndd + perm.offset, perm.size)`; the sample is already in audsrv's format, nothing has to be prepended. The per-sample header
  was verified against `adpenc` output (see above) but **not** against audsrv's IOP source (not in the toolchain tree); the coordinator's description (bytes 0-15) matched
  `adpenc` on every field except the pitch word, which `adpenc` truncates (1881 for 22050 Hz, `round()` would give 1882). The SPU2 set that audsrv ends up holding is
  `sum(size - 16)` <= 1,467,968 bytes. Loops have their flags set (6 ... 3) and header loop byte 1: pass `loop` accordingly when playing.
* `snd_find(tag id)` with the ids the other packs carry: weapon / projectile / effect / biped / globals sound references are Xbox tag ids of `snd!`
  and `lsnd` tags; an id that `snd_find` does not know was out of the gameplay set, `SND_F_STREAMED` / `SND_F_DROPPED` sounds exist but have no data.
