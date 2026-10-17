# `STRM` / `STRD` sections, version 1 (streamed audio: dialogue, music, long ambience)

Output of `tools/halo2ps2/streamconv.py` (parsers and the Xbox ADPCM decoder are `soundconv.py`'s, [doc 21](21-snd-section.md)), read by
`engine/src/audio/strmdef.c` (contract `strmdef.h`), played by `engine/src/audio/stream.c` (contract `stream.h`). Stored as the two sections of a
[`.hp2` pack](07-hp2-format.md), `assets/hp2/<map>.strm.hp2`, both **stored** (codec 0: ADPCM does not compress, and the EE reads ranges):

* `STRM`: the index (little-endian tables, 16-byte aligned, offsets relative to the section start). Loaded into RAM, 19-393 KiB per map.
* `STRD`: the stream data. Starts on a 2048-byte boundary of the pack; every stream starts on a `data_align` (2048) boundary **inside** it, so a stream's
  first byte is the first byte of a disc sector and `pack file offset = STRD section offset + stream.data_offset (+ byte position in the stream)`.
  The reader needs only `STRM` and the *size* of `STRD`; the player reads ranges of `STRD` from the file.

Cross references are Xbox tag ids, as in the other packs: `snd!` ids key the sounds, `lsnd` ids the loops. The records name no model or animation, so
the converter exports no `referenced_models()` / `referenced_anims()`; the sound roots go the other way (`--roots`, below).

## What goes in
* **Every streamed `snd!` of the map** (the sounds `soundconv.py` marks `STREAMED`: classes 19 32 44 45 46 47 48 and the dialog/music paths), by `kind`:
  `DIALOGUE` (19, 44-48), `MUSIC` (32 and the music paths) or `AMBIENCE` (any other streamed one).
* **Long ambience**: every other sound that a `sound_looping` track plays as its loop or alternate loop and whose longest chain lasts at least
  `--ambience-seconds` (default 5 s). Flag `STRM_S_EXTRA`. SND0 may hold a resident copy of the same sound (or have dropped it for the 1.4 MiB budget); the
  engine plays the resident one when there is one and streams otherwise.
* **`sound_looping` tags** that name at least one sound of the pack get a loop record with the track structure; the sounds a track names that are *not* in
  the pack (resident gameplay sounds) stay plain tag ids for the SND0 path.
* `--no-dialogue` and `--roots TAG...|@file` narrow the dialogue: `--roots` keeps only the listed dialogue sounds (ids, exact paths, or `@file` of ids), music,
  ambience and `sound_looping` are always kept. Workstreams I, D and C export `referenced_sounds(map_path)` for this; **without roots a10 .. c10 carry 900-1,040 dialogue
  sounds each** (70-76 MiB; c20 44, c40 about 390, d20 and d40 about 250): the maps ship far more lines than their own scripts and conversations reference.

### Permutations, chains, variants
A streamed `snd!` is split two ways in the source (verified on all ten maps). **Dialogue** has *alternate takes*: flat permutations, `next = none`, each a complete
line: every take is a stream of its own and a **variant** of the sound (4.4 variants per sound on average; the player picks one at random). **Long music** is a
*chain* of permutations of 65,520 bytes (910 stereo blocks) linked by `next` (the tag's "split long sound into permutations" flag, tens of permutations): the chain
is one continuous signal, so the converter concatenates it (block aligned, nothing to re-synchronise) into **one stream**; every chain head (a permutation no `next`
points to) is one variant. A cycle or two chains running into the same permutation is a conversion error.

## Layout
Header, 128 bytes: `char[4] "STRM"`, `u16 version (1)`, `u16 reserved`, `u32 total size` (= section size), counts at `0x0C` sounds, `0x10` streams, `0x14` loops,
`0x18` tracks, table offsets at `0x1C` sounds, `0x20` streams, `0x24` loops, `0x28` tracks, `u32` at `0x2C` STRD size (the reader is given the real size and they
must agree), `0x30` data alignment (power of two >= 16; 2048), statistics at `0x34` Xbox source bytes represented, `0x38` streams copied byte for byte, `0x3C` streams
re-encoded, `0x40` streams sharing data with an earlier one, `0x44` dialogue streams, `0x48` music streams, `0x4C` ambience streams; the rest is zero.

| Table | Element | Order |
|---|---|---|
| sounds | 64 B | strictly ascending tag id (binary search) |
| streams | 64 B | per sound consecutive: its variants are `stream_first .. stream_first + stream_count - 1` |
| loops | 64 B | strictly ascending `lsnd` tag id |
| tracks | 48 B | per loop consecutive |

**Sound, 64 B**: `u32 tag_id`, `u8 class` (the tag's sound class, [doc 21](21-snd-section.md)), `u8 kind` (0 dialogue, 1 music, 2 ambience), `u16 flags`
(`STRM_S_LOOPING 1` some track plays it as loop / alternate loop, `STRM_S_EXTRA 2` long loop, not a streamed class), `u16 stream_count` (>= 1), `u16` reserved,
`u32 stream_first`, `f32` at `0x10` min distance, max distance, skip fraction, gain modifier (as in `snd_sound`, unverified names), `u32 max_play_ms` at `0x20`
(the tag's declared maximum play length; the decoded length is 8/9 of it), `u32 tag_flags` at `0x24`, `char[24] name` at `0x28` (tail of the tag path, debugging only).

**Stream, 64 B**: `u32 data_offset` (inside STRD, multiple of `data_align`), `u32 data_size` (bytes: a whole number of blocks, > 0), `u32 frames` (= blocks x 64:
decoded sample frames per channel, **including up to 63 padding frames** of the source encoder: the tag has no sample count), `u32 rate` (Hz, 1..48000),
`u32 loop_start`, `u32 loop_end` (frames, `loop_start < loop_end <= frames` with the loop flag, else 0 / 0), `f32 gain`, `f32 skip` (the chain's first
permutation's), `u8 channels` (1 or 2), `u8 format` (1 = Xbox IMA container), `u16 flags` (`STRM_F_LOOP 1`, `FOLDED 2` stereo folded to mono, `RESAMPLED 4` decoded /
resampled / re-encoded, `SHARED 8` data shared with an earlier stream), `u16 chunks` (source permutations concatenated), `u16 range` (pitch range of the source),
`u32 owner` (the sound's tag id), `u32 src_bytes` (the Xbox source, statistics), `i32 snr10` (encoder SNR against its PCM input in tenths of dB, `0x7FFFFFFF` for a byte
copy), 12 reserved.

**Loop, 64 B** (the layout of SND0's loop record): `u32 tag_id`, `u32 flags`, `f32 param`, `u16 track_count`, `u16` 0, `u32 track_first`, `u32` 0, 8 zero bytes,
`char[32] name` at `0x20`. **Track, 48 B**: `u32 flags`, `f32 gain, fade_in, fade_out` (linear, seconds), `u32 start, loop, end, alt_loop, alt_end` (sound tag ids,
`0xFFFFFFFF` none), 12 reserved. Detail sounds of a `sound_looping` are not recorded (SND0 has them).

## Stream data: IMA ADPCM in the Xbox container
**IMA ADPCM, standard 89-entry step table**, laid out as the source stores it (verified in doc 21 against 39.4 M block boundaries): a **block holds 64 sample frames
per channel**. Mono block, 36 bytes: `s16` predictor (= sample 0, output as is), `u8` step index, `u8` 0, 32 bytes of nibbles (low nibble first; nibble k gives sample
k + 1, the 64th nibble is padding). Stereo block, 72 bytes: left header (4), right header (4), then eight groups of 4 bytes left / 4 bytes right (the MS layout).
Decode of a nibble: `diff = step>>3 (+ step>>2 if bit0) (+ step>>1 if bit1) (+ step if bit2)`, negated by bit 3; `pred = clamp16(pred + diff)`;
`index = clamp(index + (-1 -1 -1 -1 2 4 6 8)[nibble & 7], 0, 88)`. A block restarts from its header, so concatenated chains and any block-aligned seek are exact.

Why the source container: **a source at or below its cap is copied byte for byte**, no second generation of quantisation noise and no conversion time. That is all of
the dialogue (22050 Hz mono on all ten maps), the ambience at 22050 Hz and the few music pieces at 22050 Hz. What is above the cap is **decoded, resampled and
re-encoded in the same container**:

| kind | cap (`--*-rate`) | channels | source | stored |
|---|---|---|---|---|
| dialogue | 22050 Hz | mono (stereo folded `(l + r) >> 1`) | 22050 mono | byte copy |
| music | 32000 Hz | as source (stereo) | 44100 stereo (almost all) | resampled 32000 stereo |
| ambience | 22050 Hz | as source | 22050 mono / stereo | byte copy (44100 sources: resampled) |

* **Resampler** (`streamconv.resample`): windowed sinc, 12 taps on each side, Blackman-Harris, 1024 phases, cutoff 0.46 of the *output* rate, DC gain 1 per
  phase, output sample j at input position `j * n_in / n_out`, zero padded at the ends (a loop wraps around instead, so its seam stays continuous). Measured: passband
  gain within 0.09 dB, 1 kHz tone keeps its frequency, 20 kHz at 44.1 kHz is more than 22 dB down at a 32 kHz output.
* **Encoder** (`streamconv.encode_xbox`): closed loop, the header predictor is the block's source sample 0, the nibble is the nearest `(|d| * 4) // step` (capped at 7),
  the step index carries over from the previous block (the source's continuity rule). Mean SNR against its own PCM input on the ten maps: **34.5-41.7 dB** (it is the
  ADPCM noise only; the resampler's own error is not in it).
* **Loops**: a looping stream has `loop_start = 0`, `loop_end = frames` (the sound *is* the loop). A resampled loop is shrunk to a whole number of blocks (fewer than 64
  frames) with the periodic filter and **keeps the nominal rate** (pitch error below 64 / length: 0.4 % for a half second loop, 0.002 % for a two minute track), because the player
  renders every segment of a `sound_looping` track through one voice and needs one rate. One-shots are padded to a whole block by holding the last sample. A byte-copied loop
  keeps the source's up to 63 padding frames (a gap of at most 2.9 ms at the seam).
* **Deduplication**: streams with identical bytes, channels and rate share one copy in STRD (`SHARED`): 46-105 per map. **Across maps** the ten packs hold 993 MiB of
  stream data of which only 430 MiB (43 %) is unique: dialogue and music repeat from map to map. Nothing shares them yet (one pool pack on the disc would); that is
  a disc-layout decision for the integrator.
* Sector alignment pads about 1 KiB per stream (4 % of STRD).

## Sizes and conversion time (ten maps, measured)
`python3 -I tools/halo2ps2/streamconv.py assets/decompressed/<map>.map -o assets/hp2/<map>.strm.hp2 --cache assets/hp2/.strmcache`, 14 cores, a shared cache that started empty,
maps converted one after the other:

| map | sounds | streams (copied / re-encoded / shared) | loops / tracks | dialogue MiB (streams) | music MiB (streams) | ambience MiB | STRM KiB | STRD MiB | pack MiB | Xbox source MiB | SNR dB | time s (cpu s) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a10 | 1085 | 4749 (4671 / 78 / 105) | 35 / 35 | 72.04 (4658) | 41.45 (72) | 2.25 | 368.6 | 118.85 | 119.2 | 132.33 | 36.9 | 11.2 (115) |
| a30 | 1067 | 5121 (5052 / 69 / 104) | 37 / 45 | 75.97 (5030) | 31.88 (55) | 3.78 | 391.3 | 115.07 | 115.5 | 125.61 | 34.5 | 7.5 (91) |
| a50 | 1068 | 5151 (5058 / 93 / 104) | 37 / 44 | 75.93 (5036) | 48.77 (76) | 4.49 | 393.2 | 132.70 | 133.1 | 150.26 | 36.8 | 12.1 (103) |
| b30 | 983 | 5058 (4971 / 87 / 103) | 31 / 39 | 72.55 (4954) | 39.97 (64) | 5.21 | 381.5 | 121.35 | 121.7 | 136.64 | 35.7 | 8.8 (103) |
| b40 | 1023 | 5115 (5015 / 100 / 103) | 36 / 45 | 73.76 (4981) | 42.56 (83) | 7.67 | 388.1 | 127.65 | 128.0 | 143.33 | 36.2 | 12.9 (103) |
| c10 | 952 | 4837 (4748 / 89 / 46) | 34 / 42 | 70.24 (4725) | 37.32 (69) | 5.94 | 366.0 | 117.56 | 117.9 | 131.78 | 37.6 | 9.6 (106) |
| c20 | 75 | 198 (146 / 52 / 22) | 15 / 17 | 4.09 (130) | 27.48 (49) | 1.69 | 18.9 | 33.13 | 33.2 | 44.35 | 41.7 | 7.0 (49) |
| c40 | 416 | 2050 (1951 / 99 / 98) | 37 / 45 | 30.60 (1917) | 56.08 (83) | 7.35 | 158.7 | 94.79 | 94.9 | 118.46 | 37.0 | 12.9 (93) |
| d20 | 255 | 1365 (1252 / 113 / 94) | 34 / 40 | 20.31 (1233) | 48.63 (92) | 5.10 | 105.4 | 74.19 | 74.3 | 96.20 | 38.1 | 9.8 (54) |
| d40 | 269 | 1024 (908 / 116 / 94) | 58 / 71 | 19.14 (883) | 62.95 (92) | 8.22 | 87.9 | 90.12 | 90.2 | 120.07 | 39.1 | 15.5 (99) |

Total **1,025 MiB** of STRD (1,028 MiB of packs) for the ten maps, 107 s of wall time for all of them cold (915 s of CPU); a rerun from the cache is 0.6 s per map and writes
byte-identical packs; one core takes about 60-120 s per map (c20: 60 s); the pack is assembled in memory, peak resident size 645 MB for the largest (a50). "Tens of MiB" turned out to be 33-133 MiB per map (the Xbox maps hold 44-150 MiB of the same material). The DVD budget (4.7 GB) is not the constraint; the **disc read rate** is not either
(the player reads at most 36 KiB/s of music + 12 KiB/s of dialogue + 12 KiB/s of ambience). Music re-encodes 44100 -> 32000 Hz (52-116 streams per map) shrink 57 -> 41 MiB (a10).
"Xbox source MiB" counts the Xbox bytes each stream represents (duplicates included).

## Reader API (`audio/strmdef.h`)
```c
strm_status strm_open(strm_section *s, const uint8_t *strm, size_t size, uint32_t strd_size);   /* validates everything */
int         strm_find(const strm_section *s, uint32_t tag_id);              /* sound index or -1 */
strm_sound  strm_get_sound(const strm_section *s, uint32_t index);          /* kind, class, flags, stream_first / stream_count, distances ... */
strm_stream strm_get_stream(const strm_section *s, uint32_t index);         /* global stream index = stream_first + variant */
int         strm_find_loop(const strm_section *s, uint32_t tag_id);         /* sound_looping index or -1 */
strm_loop   strm_get_loop(...);  strm_track strm_get_track(const strm_section *s, const strm_loop *lp, uint32_t k);
void        strm_decode_blocks(const uint8_t *src, unsigned channels, uint32_t nblocks, int16_t *out);   /* interleaved frames, 64 per block */
```
`strm_open` checks sizes and alignment of every table, sort order, sound -> stream ranges, every stream (format, channels, size a whole number of blocks, frames = blocks x 64,
offset aligned and inside STRD, rate 1..48000, flags, loop points consistent with the flag), finite floats, terminated names; on failure the section is left empty and every accessor
returns a neutral record. The decoder is safe on arbitrary bytes (an index above 88 is read as 88; the converter rejects such a stream). Host tests:
* `tools/tests/test_streamconv.py`: encoder / decoder round trips, the resampler (gain, frequency, alias rejection, periodic mode), chains, planning options, a synthetic map
  with dialogue variants, a stereo fold, chains, loops, long ambience, deduplication, byte-for-byte copies, cache and process pool giving identical bytes, section corruption (20 targeted
  fields), the command line, and (with `assets/`) the real a10 conversion. `STRM_FIXTURE=<path>` also writes the pack and `<path>.crc` (reference CRC-32 of every stream's PCM).
* `engine/tests/test_strmdef.c`: every field of the fixture; **bit-exact decode against the Python reference** (CRC of every stream of the fixture and of 60 streams of each real pack,
  `--verify --crc-out`); the table decoder against a formula decoder on 20,000 random blocks (mono and stereo, headers included); extremes (saturation); truncation at every length; every
  byte of the index flipped three ways; real packs opened, walked, decoded on a sample and byte-flipped. ASan/UBSan clean.

## Player (`audio/stream.h`, `stream.c`)
Two parts in one file: the **pure part** (no ps2sdk, no heap, no `double`, host-tested by `engine/tests/test_stream.c`) and the **EE glue** under `#ifdef _EE` at the end of
`stream.c`.

```c
stream_init(&p, &sec, &io, strd_base);                  /* io = { start, poll, user }: one asynchronous read at a time */
stream_play_sound(&p, slot, tag, variant|-1, STREAM_PLAY_LOOP?, gain, pan, fade_in_s);   /* a snd! of the pack */
stream_play_loop(&p, slot, lsnd_tag, track, gain, pan, fade_in_s|-1);                    /* start -> loop (alt) -> stop -> end */
stream_stop(&p, slot, fade_out_s|-1);  stream_kill(&p, slot);  stream_set_gain(&p, slot, gain, ramp_s);  stream_loop_alternate(&p, slot, on);
stream_tick(&p);                                        /* 30 Hz: reads, decode, planner. Never blocks. */
stream_pump(&p, queued_frames);                         /* mix up to the target into the output ring */
stream_ring_peek / stream_ring_consume / stream_ring_read, stream_get_stats, stream_slot_busy
```
* **Slots**: three voices, `STREAM_SLOT_DIALOGUE / MUSIC / AMBIENCE`; one sound per slot, a new request replaces it (`steals`). A stereo source ignores `pan`; for a mono source
  `pan` -1 .. 1 attenuates the far side linearly (centre = both channels at full level).
* **Data path**: per voice `STREAM_CHUNKS` = 3 chunk buffers of 32,760 bytes (910 mono or 455 stereo blocks: 2.64 s of 22050 mono, 0.91 s of 32000 stereo; 32 KiB buffers),
  read ahead one chunk at a time (**one read outstanding for the whole player**, the voice with the least buffered time goes first); the decoder turns whole blocks into the voice's
  ring of 4096 decoded frames; the mixer reads that ring with a Q16 phase (`step = rate * 65536 / 48000`), linear interpolation, Q23 gain (ramped per frame), Q15 pan, 32-bit accumulators,
  clamped into the **output ring** of 8192 stereo 16-bit frames at **48000 Hz**. `stream_pump(queued)` renders `target - queued - ring` frames (at most 3072 per call, so one call never
  outruns a voice's decoded ring even at 1:1), in blocks of 128. All arithmetic is integer except the float arguments of the control calls.
* **Timeline of a sound_looping track**: the start segment plays once, the loop repeats (at `loop_start..loop_end`, or the whole stream), `stream_loop_alternate(on)` swaps the loop for
  the alternate loop at the next pass boundary *that has not been planned yet* (read-ahead: at most 3 chunks plus the decoded ring, so a short loop switches within about 2 s and a
  loop that is minutes long switches when its pass ends), `stream_stop` fades the loop out over the track's `fade_out`, flushes, and plays the end sound once at the track gain (about one
  tick of silence in between: the end sound is read and decoded after the flush). Fades are linear in gain (minimum 64 frames); `fade_in` starts when the voice becomes audible. Segments of one
  track must share channels and rate: a mismatch is counted (`format_mismatch`) and the segment skipped; `streamconv --verify` reports it (none on the ten maps).
* **Loops are gapless**: a pass is just more chunks in the same FIFO. A loop point inside a block is handled with a skip count on the first chunk of the pass and a frame limit on the last.
* **Priming and underruns**: a voice is audible once 1024 source frames are decoded (or the whole stream is). A playing voice that has fewer than two decoded frames while more data is coming
  outputs silence and counts one `voice_underruns` event (and the missing output frames in `voice_underrun_frames`); it resumes where it paused. `out_underruns` (glue) counts ticks that found
  audsrv's ring empty while the player was still supposed to be feeding it. A failed read loses its chunk (counted); four failures in a row end the voice; a failing `start` ends it at once.
* **Idle**: after the last voice ended the pump renders 0.5 s of silence and then stops (returns 0), so a game with nothing playing pays no RPCs.
* **Statistics** (`stream_stats`): ticks, reads, bytes read, read errors, ticks that began with a read still running, frames decoded / mixed / silent, voice and output underruns,
  plays / steals / finished / loop passes / format mismatches, ring high water, worst start latency in ticks, and (glue) the EE time of every service call: total, in `stream_tick`, in `stream_pump`
  and in the audsrv RPCs, worst case, the audsrv ring fill seen at each call, pushed frames.
* **Memory**: `sizeof(stream_player)` = 378 KB (static, no heap) + the `STRM` index (19-393 KiB, from the arena at open).
* **Host tests** (`test_stream.c`, synthetic packs whose decoded content is known in closed form: a ramp of +1 per sample or a constant, so loop points, skips, sequences, gains and fades are
  exact, plus the converter's fixture and the real a10 pack): one-shot output equal to the source sample for sample; a loop with the loop start inside a block and a loop end that is not a block
  end; interpolation; stereo; three voices summing and clamping; gain, pan, fade in, fade out, `set_gain`; start -> loop -> alternate -> back -> stop -> end with exact levels and a bounded switching latency;
  a loop-only track; a format mismatch; a 3.3-chunk stream read in 4 reads of at most 32,760 bytes inside the stream's range, never two reads at once; a slow disc (150 polls per read: underruns counted, order kept,
  data intact); read errors and a failing start; a play that abandons an in-flight read; the ring (partial consumption, wrap, target limits, idle stop); every stream of the fixture against a
  double-precision reference render (error <= 3 LSB, bit-identical decode); the fixture's music track by zero-crossing frequency (400 / 1000 / 2000 / 3000 Hz segments) and level; noise in the stream data
  and 300 single-bit flips of the index through the whole player; the real pack played for 20 s of game time with a file-backed `stream_io` (0 underruns). ASan/UBSan clean.

## EE glue and what was measured on PCSX2
### audsrv (read from `audsrv.h`, measured with test programs on PCSX2, `PCSX2 -nogui -batch`)
* **The ring holds 53 ms, whatever the format**: `audsrv_available()` right after `audsrv_set_format` is 10,240 bytes at 48000 Hz, 9,400 at 44100, 6,820 at 32000, 5,120 at 24000, 4,700 at 22050,
  2,340 at 11025 (always 53.3 ms of audio). 16000 Hz is not supported (`set_format` returns `AUDSRV_ERR_FORMAT_NOT_SUPPORTED`). A lower mix rate therefore buys no latency margin, so the player mixes at 48000
  (nothing converted on the IOP; music 32000 -> 48000 and dialogue 22050 -> 48000 by linear interpolation on the EE). **A 30 Hz tick leaves 20 ms of margin: a late frame runs the ring dry.**
  The glue keeps the ring within 128 frames of full (target = ring - 128 = 2432 frames) and offers `stream_ee_pump`, a mix-and-push that can run between ticks (e.g. every vsync of a 30 fps game: 36 ms of margin).
  A dedicated audio thread would remove the dependency on the frame time altogether; it needs a lock around the control calls and is the integrator's call.
* What blocks and what does not: `audsrv_wait_audio` blocks on a semaphore until there is space and is **never used**; the fill-buffer callback is not used; `audsrv_play_audio` returns the bytes it could take
  (**0 after 0.07 ms when the ring is full**: it neither waits nor loops) and the glue keeps what was not taken for the next call; `audsrv_available` is one RPC round trip (0.05 ms); `audsrv_play_audio` of 10 KiB 0.34 ms.
  RPCs wait for the IOP: on hardware their latency grows when the IOP is busy; the glue makes two per service call.
* The audsrv ADPCM voices (`audio.c`, SPU2 RAM) and this PCM stream do not interact (separate paths of the same server); `audio_init()` must have loaded audsrv first.

### File reads: an IO thread, not `FIO_NOWAIT`
`fioSetBlockMode(FIO_NOWAIT)` + `fioRead` + `fioSync(FIO_NOWAIT)` was tried first and works on PCSX2 (a 32,768-byte read from `host:` completes about 1.6 ms later while the EE keeps running, data bit-identical to a
blocking read, 64-multiple and 32,760-byte sizes alike) **as long as the mode stays NOWAIT until the poll reports completion** (switching back in between: it never completes). But fio has one RPC client:
in NOWAIT mode every other fio call runs while the read is in flight and steals its completion. Measured: a status line printed (printf goes through fio) during a read made `fioSync` report "complete" with
186 bytes, the length of the line. The engine prints, loads and uses the memory card; so the glue uses **an IO thread making ordinary blocking `fioLseek` + `fioRead` calls** (the library's semaphore
serialises threads: a printf during a read waits for it, and nothing is ever corrupted). `stream_io.start` posts the request (a semaphore), `poll` reads a flag. The thread runs at a higher priority than the
game thread (the game spins in `graph_wait_vsync` and a lower priority thread would starve); it runs only between "RPC finished" and "asleep again". **The glue lowers the calling thread's priority to 2 if it
was below 2 (the crt0 main thread runs at 1, measured) and starts the IO thread at 1 (one better than the game thread).** Reads are rounded up to whole 64-byte lines (the buffer has room), and the destination is not touched until the flag says done.
On a disc the read is a seek plus 32 KiB: tens of ms in the IO thread, none in the game thread.

### Verified on PCSX2
`engine/tests/ee_strm` builds a test ELF (`make -C engine/tests/ee_strm`; not part of CI or the engine build; keys in `main.c`): a music loop, a dialogue line and a looping ambience from a10's pack for 20 s,
tick every 33.3 ms and a pump in between, real time (PCSX2's speed limiter):
* 25 reads / 785,088 bytes, **0 read errors, 0 voice underruns, 0 output underruns**, worst start latency 2 ticks; the same with the recompilers off (`--interpreter`) and with a blocking file read and
  printing in the middle (`strm_stress 1`).
* `pushed frames / seconds = 48,019`: audsrv consumes in real time, the ring fill seen at the start of a service call stays at or below 1,920 frames (the first call sees 0).
* **Bit-exact with the host**: the first 16,000 audible frames of a dialogue stream (22050 mono) and of a music stream (32000 stereo) played alone have CRC-32 `5a787b59` and `60a81a87` on the console build and
  on the host build (same arithmetic, same fio data path, the file's bytes through DMA and cache).
* PCM statistics of what was pushed (headless audio cannot be listened to; counters and PCM statistics stand in for ears): peak 32,551 / 32,559, rms 4,028 / 4,010, mean -8 / -8 (L / R; DC is nil, nothing
  wraps, the music is hot).
* **EE time** (emulated cycles; PCSX2 does not model cache misses, GS or DMA contention: treat as a lower bound, keep margin): `stream_tick` (read start, decode of ~1,000 frames per tick, planner) **0.28 ms per
  tick**; `stream_pump` (mixing 800 frames, two to three voices) **0.41 ms per call**; the audsrv RPCs **0.22 ms per call**. A 30 Hz tick plus one pump in between: 0.28 + 2 x 0.41 + 2 x 0.22 = **1.5 ms per 33.3 ms frame (4.5 %)**;
  without the extra pump 1.0 ms. Worst single service call 2.3-2.5 ms (a read start plus a decode burst when a voice starts). Same numbers with the interpreter (the EE cycle counter is emulated either way).
  The host build runs the whole 20 s scenario at 0.2 ms per tick under ASan.
* The one thing not run: a DVD. `file_size_of` / `file_read_at` / `fioOpen` take `cdrom0:` paths like the rest of the engine, but nothing here has read from a disc image (ADR-022 defers it).

## What the integrator wires
* **Pack and cfg**: `scripts/build-world.sh`: `python3 -I tools/halo2ps2/streamconv.py assets/decompressed/$m.map -o assets/hp2/$m.strm.hp2 --cache assets/hp2/.strmcache [--roots @ids.txt]` (about 10 s per
  map cold with all cores). `game <prefix>` loads `<prefix>.strm.hp2` when it exists (the same `<prefix>.<kind>.hp2` convention); suggested cfg keys: `stream 0/1` (default 1) and `streamlog 1`.
* **Init** (after `audio_init()`): `static stream_player s_stream; static stream_ee s_stream_ee;` then `stream_ee_open(&s_stream_ee, &s_stream, &arena, "<prefix>.strm.hp2", log)`; on failure it returns -1 and the player
  stays inert (every call a no-op). **Per 30 Hz game tick**: `stream_ee_tick(&s_stream_ee)` after the game tick (it starts reads, decodes, mixes, pushes); optionally `stream_ee_pump(&s_stream_ee)` once per vsync in between.
  On a level change: `stream_kill` the slots, `stream_ee_close`, open the new pack. Pausing: stop calling; audsrv drains in 53 ms.
* **Script built-ins** (names are the engine's vocabulary): `sound_impulse_start(sound, object, scale)` / dialogue lines of AI conversations: if `snd_find` says resident, `gameaudio_play`, else if `strm_find(tag) >= 0`
  `stream_play_sound(slot by kind: dialogue -> DIALOGUE, music -> MUSIC, else AMBIENCE, tag, -1, 0, gain, pan, -1)` (gain and pan from the distance / bearing as `gameaudio` does; `skip_fraction` is the caller's);
  `sound_impulse_time` / "is this line still playing" -> `stream_slot_busy`; `sound_impulse_stop` -> `stream_stop` / `stream_kill`; `sound_looping_start(lsnd, object, scale)` -> `stream_play_loop(&p, slot, tag, 0, scale, 0.0f, -1.0f)` when
  `strm_find_loop` finds it (a loop whose sounds are resident goes to SND0); `sound_looping_stop` -> `stream_stop(slot, -1)`; `sound_looping_set_alternate` -> `stream_loop_alternate`; `sound_looping_set_scale` ->
  `stream_set_gain`. `sound_looping` tags with several tracks are independent layers (on the ten maps: 284 one-track, 65 two-track, 3 three-track, all ambience; no music has more than one):
  tracks whose sounds are resident play through SND0, the streamed ones through `stream_play_loop(..., track, ...)`; there is **one ambience voice**, so when two tracks of one tag are both
  streamed (41 two-track tags are) only one can play: the integrator picks (the first, or the one the object is nearest to).
* **Mission flow**: a checkpoint reload should `stream_kill` all slots (the data position of a stream is not part of the save).

## Debugging aids (`streamconv.py`)
* `PACK --info`: one-line summary (counts, bytes per kind, track sounds that only SND0 can resolve).
* `PACK --verify [--samples N] [--crc-out FILE]`: parses the pack, checks every stream's block headers, checks that every segment of every track has one channel count and rate, and decodes N streams with the
  reference decoder (CRC-32 of the interleaved PCM written to `FILE`, which `test_strmdef` compares with its decoder).
* `MAP -o PACK --roots ... / --no-dialogue / --music-rate / --dialogue-rate / --ambience-rate / --ambience-seconds / --jobs / --cache`.

## Not verified, approximated
* **Alternate loop / end / fade semantics are the engine's reading of the tag**, not checked against Halo: the track fields (`start loop end alt_loop alt_end`, `gain`, `fade_in`, `fade_out`) are verified as data
  (doc 21); what the original does with them at a stop (crossfade of the end sound? fade length?) is not known. The player fades the loop out over `fade_out`, then plays the end sound.
* **Sample counts**: frames include the source encoder's padding (up to 63 frames, 2.9 ms at 22050 Hz): a line can end with a few ms of near silence, a byte-copied loop has that gap at its seam.
* **Audio quality was measured, not heard** (no ears, ADR-022): SNR, frequency, level and DC are checked; the resampler (24 taps) and the linear interpolation on the EE are not what a mastering tool would do.
* **Hardware**: audsrv on the real IOP (RPC latency under disc load, SIF), `cdrom0:` read rates, the IO thread's behaviour next to the engine's own threads (the engine creates none today), and the cost of the
  mixer with real cache behaviour are unmeasured. The 53 ms ring is the weak point: the first thing to check on a console is `out_underruns` while the frame time varies.
* **Not modelled**: positional audio (the caller computes gain and pan), dialogue lip sync data, the `sound_environment` reverb, pitch ranges of streamed sounds (the converter records the range index, the player ignores it), the
  promotion sound of a streamed sound.
* The order of variants (`rand`): uniform; Halo's selection (weights from the permutation records?) is not known.
