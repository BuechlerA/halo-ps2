/* STRM section reader and block decoder (docs/29-strm-section.md): the streamed sounds of a map (dialogue, music, long ambience) as
 * IMA ADPCM in the Xbox container. Contract between tools/halo2ps2/streamconv.py (writer), audio/strmdef.c (reader, host-tested, pure
 * C11, no heap, no double) and audio/stream.c (the player).
 *
 * `<map>.strm.hp2` holds two stored (uncompressed) sections: STRM, the index below, and STRD, the stream data. STRD starts on a
 * 2048-byte boundary of the pack and every stream starts on a `data_align` boundary inside it, so a range of STRD is a range of
 * whole disc sectors. The reader needs only the STRM bytes and the *size* of STRD; the player reads STRD ranges from the pack file.
 * Sounds are keyed by the Xbox tag id of the `snd!` tag, sound_looping tags by the id of the `lsnd` tag; every tag id in a track
 * is such a key (0xFFFFFFFF = none), and may name a sound the SPU2 pack (snddef.h) holds instead.
 *
 * Stream data, container: blocks of 64 sample frames. A mono block is 36 bytes: s16 predictor (= sample 0), u8 step index, u8 0, then
 * 32 bytes of nibbles (low nibble first; nibble k gives sample k + 1, nibble 63 is padding). A stereo block is 72 bytes: left header,
 * right header, then eight groups of 4 bytes left / 4 bytes right. IMA step and index tables of the standard (89 steps).
 * Units: rates in Hz, frame counts in sample frames (per channel), gains linear, times in seconds. */
#ifndef AUDIO_STRMDEF_H
#define AUDIO_STRMDEF_H
#include <stddef.h>
#include <stdint.h>

#define STRM_NAME_LEN 24
#define STRM_LOOP_NAME_LEN 32
#define STRM_NO_TAG 0xFFFFFFFFu
#define STRM_FMT_XBOX_IMA 1u
#define STRM_BLOCK_FRAMES 64u
#define STRM_MAX_RATE 48000u

typedef enum { STRM_OK = 0, STRM_ERR_TRUNCATED, STRM_ERR_MAGIC, STRM_ERR_VERSION, STRM_ERR_LAYOUT } strm_status;

/* strm_stream.flags */
#define STRM_F_LOOP      1u    /* a sound_looping track plays it as loop / alternate loop: loop_start..loop_end repeats */
#define STRM_F_FOLDED    2u    /* the source was stereo, folded to mono */
#define STRM_F_RESAMPLED 4u    /* decoded, resampled and re-encoded; otherwise a byte copy of the source */
#define STRM_F_SHARED    8u    /* data shared with an earlier stream of this pack (identical bytes) */
/* strm_sound.kind */
enum { STRM_KIND_DIALOGUE = 0, STRM_KIND_MUSIC = 1, STRM_KIND_AMBIENCE = 2 };
/* strm_sound.flags */
#define STRM_S_LOOPING 1u      /* some sound_looping track uses it as loop / alternate loop */
#define STRM_S_EXTRA   2u      /* not a streamed class: a long loop sound; SND0 may hold a resident copy (check its SND_F_DROPPED) */

typedef struct {
	const uint8_t *data;               /* STRM bytes, not owned */
	size_t         size;
	uint32_t       data_size;          /* bytes of STRD */
	uint32_t       data_align;         /* stream offsets are multiples of this (2048) */
	uint32_t       sound_count, stream_count, loop_count, track_count;
	uint32_t       src_bytes, passthrough_streams, transcoded_streams, shared_streams;
	uint32_t       dialogue_streams, music_streams, ambience_streams;
	const uint8_t *sounds, *streams, *loops, *tracks;
} strm_section;

typedef struct {
	uint32_t tag_id;
	uint8_t  klass;                    /* sound class as in snddef.h (19 dialogue, 32 music, ...) */
	uint8_t  kind;                     /* STRM_KIND_* */
	uint16_t flags;                    /* STRM_S_* */
	uint16_t stream_count;             /* variants: alternate takes (random pick) or alternate chains, >= 1 */
	uint32_t stream_first;             /* index of the first variant in the stream table */
	float    min_dist, max_dist;       /* the tag's distances (world units, 0 = default) */
	float    skip_fraction;            /* probability of not playing */
	float    gain_mod;                 /* "random gain modifier" (name unverified, as in snddef.h) */
	uint32_t max_play_ms;              /* the tag's declared maximum play length (decoded length is 8/9 of it) */
	uint32_t tag_flags;
	char     name[STRM_NAME_LEN];      /* tail of the tag path, debugging only */
} strm_sound;

typedef struct {
	uint32_t data_offset;              /* inside STRD, multiple of data_align */
	uint32_t data_size;                /* bytes: a whole number of blocks (36 mono / 72 stereo) */
	uint32_t frames;                   /* decoded sample frames = blocks * 64 (includes up to 63 padding frames of the source encoder) */
	uint32_t rate;                     /* Hz */
	uint32_t loop_start, loop_end;     /* frames, valid with STRM_F_LOOP: loop_start < loop_end <= frames; 0 / 0 without */
	float    gain, skip;               /* permutation gain (linear) and skip fraction of the chain's first permutation */
	uint8_t  channels;                 /* 1 or 2 */
	uint8_t  format;                   /* STRM_FMT_XBOX_IMA */
	uint16_t flags;                    /* STRM_F_* */
	uint16_t chunks;                   /* source permutations concatenated into this stream */
	uint16_t range;                    /* pitch range of the source sound */
	uint32_t owner;                    /* tag id of the sound it belongs to */
	uint32_t src_bytes;                /* size of the Xbox source in bytes (statistics) */
	int32_t  snr10;                    /* encoder SNR in tenths of dB, 0x7FFFFFFF for a byte copy */
} strm_stream;

typedef struct {
	uint32_t tag_id, flags;
	float    param;
	uint16_t track_count;
	uint32_t track_first;              /* internal */
	char     name[STRM_LOOP_NAME_LEN];
} strm_loop;

typedef struct {
	uint32_t flags;
	float    gain, fade_in, fade_out;  /* gain linear, fades in seconds */
	uint32_t start, loop, end, alt_loop, alt_end;   /* sound tag ids, STRM_NO_TAG = none; `loop` and `alt_loop` repeat */
} strm_track;

/* Validates every table (sizes, alignment, ranges, sort order, indices, stream bounds against `strd_size`, finite floats, terminated
 * names). On failure *s is left empty and safe to query. */
strm_status strm_open(strm_section *s, const uint8_t *strm, size_t size, uint32_t strd_size);

int         strm_find(const strm_section *s, uint32_t tag_id);              /* sound index, -1 if absent */
strm_sound  strm_get_sound(const strm_section *s, uint32_t index);          /* neutral zero record if out of range */
strm_stream strm_get_stream(const strm_section *s, uint32_t index);         /* global stream index (stream_first + variant) */
int         strm_find_loop(const strm_section *s, uint32_t tag_id);         /* sound_looping index, -1 if absent */
strm_loop   strm_get_loop(const strm_section *s, uint32_t index);
strm_track  strm_get_track(const strm_section *s, const strm_loop *lp, uint32_t k);

/* ---- decoder -------------------------------------------------------------------------------------------------------------- */
static inline uint32_t strm_unit_bytes(unsigned channels) { return 36u * channels; }

/* Decodes `nblocks` consecutive blocks of `channels` (1 or 2) into interleaved signed 16-bit frames: 64 * nblocks frames,
 * channels samples each. Safe on arbitrary bytes (a step index above 88 is treated as 88). Bit-exact with soundconv's reference
 * decoder (tools/halo2ps2/soundconv.py: xbox_adpcm_decode). */
void strm_decode_blocks(const uint8_t *src, unsigned channels, uint32_t nblocks, int16_t *out);

#endif
