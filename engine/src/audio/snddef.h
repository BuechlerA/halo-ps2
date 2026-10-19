/* SND0 / SNDD section reader (docs/21-snd-section.md): the gameplay sounds of a map converted to SPU2 ADPCM.
 * Contract between tools/halo2ps2/soundconv.py (writer), audio/snddef.c (reader, host-tested, pure C11, no heap,
 * no double) and the engine's audio client.
 *
 * SND0 holds the tables, SNDD one ready-made audsrv sample per resident permutation, 16-byte aligned: a 16-byte header
 * ('APCM', 1, 1, loop, 0, u32 pitch = floor(rate * 4096 / 48000), u32 frames, little-endian), the SPU2 ADPCM blocks (16 bytes =
 * 28 samples, tools/halo2ps2/vag.py) and a terminator block (flags 7), the layout of ps2sdk's adpenc. The engine passes
 * `sndd + sample_offset` straight to audsrv_load_adpcm; audsrv uploads everything after the header, so the SPU2 resident set
 * (blocks + terminators = sum of size - 16) is at most 1,467,968 bytes (1.4 MiB) per map; SNDD itself is larger by the headers. Sounds are keyed by the Xbox tag id of the `sound` (snd!) tag,
 * looping sounds by the id of the `sound_looping` (lsnd) tag; every tag id inside the records (promotion sound,
 * loop tracks, detail sounds) is such a key, 0xFFFFFFFF = none.
 * Units: distances in world units, angles in radians, rates in Hz, durations in seconds, gains linear (1.0 = unity). */
#ifndef AUDIO_SNDDEF_H
#define AUDIO_SNDDEF_H
#include <stddef.h>
#include <stdint.h>

#define SND_NAME_LEN 32
#define SND_SMALL_NAME_LEN 16
#define SND_NO_TAG 0xFFFFFFFFu
#define SND_NO_PERM 0xFFFFu
#define SND_BLOCK_BYTES 16u
#define SND_BLOCK_SAMPLES 28u
#define SND_BUDGET_BYTES 1467968u        /* the converter's default: 1.4 MiB of SPU2 RAM per map, a multiple of 64 */

typedef enum { SND_OK = 0, SND_ERR_TRUNCATED, SND_ERR_MAGIC, SND_ERR_VERSION, SND_ERR_LAYOUT } snd_status;

/* snd_sound.flags */
#define SND_F_LOOP        1u   /* used as the loop (or alternate loop) of a sound_looping track: its permutations loop */
#define SND_F_DROPPED     2u   /* resident set was over budget: no permutation kept, the sound is silent */
#define SND_F_STREAMED    4u   /* dialogue / music: not resident, Phase 4 streaming plays it */
#define SND_F_DEGRADED    8u   /* rate halved (11025 Hz) to fit the budget */
#define SND_F_UNSUPPORTED 16u  /* source format the converter cannot read: no data */

/* snd_perm.flags */
#define SND_P_LOOP   1u        /* voice loops over the whole permutation (flags in the blocks say so too) */
#define SND_P_FOLDED 2u        /* the source was stereo and has been folded to mono */

/* sound classes as stored in the tag (verified by tag path, see the doc): voice priority and routing hints */
enum {
	SND_CLASS_PROJECTILE_IMPACT = 0, SND_CLASS_PROJECTILE_DETONATION = 1,
	SND_CLASS_WEAPON_FIRE = 4, SND_CLASS_WEAPON_READY = 5, SND_CLASS_WEAPON_RELOAD = 6, SND_CLASS_WEAPON_EMPTY = 7,
	SND_CLASS_WEAPON_CHARGE = 8, SND_CLASS_WEAPON_OVERHEAT = 9, SND_CLASS_WEAPON_IDLE = 10,
	SND_CLASS_OBJECT_IMPACTS = 13, SND_CLASS_PARTICLE_IMPACTS = 14, SND_CLASS_SLOW_PARTICLE_IMPACTS = 15,
	SND_CLASS_UNIT_FOOTSTEPS = 18, SND_CLASS_UNIT_DIALOG = 19, SND_CLASS_VEHICLE_COLLISION = 22,
	SND_CLASS_VEHICLE_ENGINE = 23, SND_CLASS_DEVICE_DOOR = 26, SND_CLASS_DEVICE_FORCE_FIELD = 27,
	SND_CLASS_DEVICE_MACHINERY = 28, SND_CLASS_DEVICE_COMPUTERS = 30, SND_CLASS_MUSIC = 32,
	SND_CLASS_AMBIENT_NATURE = 33, SND_CLASS_AMBIENT_MACHINERY = 34, SND_CLASS_FIRST_PERSON_DAMAGE = 39,
	SND_CLASS_GAME_EVENT = 50
};

typedef struct {
	const uint8_t *data;               /* SND0 bytes, not owned */
	size_t         size;
	uint32_t       sndd_size;          /* bytes of the SNDD section the tables describe */
	uint32_t       sound_count, range_count, perm_count, loop_count, track_count, detail_count;
	uint32_t       budget, dropped_sounds, dropped_perms, degraded_sounds, streamed_sounds;
	const uint8_t *sounds, *ranges, *perms, *loops, *tracks, *details;
} snd_section;

typedef struct {
	uint32_t tag_id;
	uint32_t tag_flags;                /* the tag's own flags (bit 0 fit to ADPCM block, bit 1 split long sound) */
	uint8_t  klass;                    /* SND_CLASS_* */
	uint8_t  flags;                    /* SND_F_* */
	uint16_t rate;                     /* nominal rate of the permutations (22050 or 11025), 0 when none */
	uint32_t range_first;              /* internal: index into the pitch range table */
	uint16_t range_count;              /* pitch ranges (0 for sounds without resident data) */
	uint16_t perm_count;               /* resident permutations over all ranges, ranges are consecutive runs of them */
	uint32_t perm_first;               /* internal */
	uint16_t perms_dropped;            /* permutations the budget removed */
	uint16_t promotion_count;
	uint32_t promotion_tag;            /* sound to play instead when too many of these play at once, SND_NO_TAG = none */
	float    min_dist, max_dist;       /* full volume inside min, silent beyond max (0 = default distances) */
	float    skip_fraction;            /* probability of not playing at all */
	float    pitch_lo, pitch_hi;       /* random pitch bounds (multipliers, 1.0 = none) */
	float    inner_cone, outer_cone, outer_gain;   /* directional sounds: radians, radians, gain outside the outer cone */
	float    gain_mod;                 /* "random gain modifier" (name unverified) */
	float    scale_mod[6];             /* skip fraction, gain, pitch when the impact scale is 0, then when it is 1 (unverified) */
	uint32_t max_play_ms;              /* the tag's declared maximum play length */
	char     name[SND_NAME_LEN];       /* tail of the tag path, debugging only */
} snd_sound;

typedef struct {
	float    natural_pitch, bend_lo, bend_hi;
	uint16_t perm_first;               /* index into the sound's permutation list (snd_get_perm) */
	uint16_t perm_count;
	char     name[SND_SMALL_NAME_LEN];
} snd_range;

typedef struct {
	uint32_t sample_offset;            /* byte offset of the audsrv sample (its APCM header) inside SNDD, 16-aligned */
	uint32_t size;                     /* bytes of the whole sample: 16 header + blocks + 16 terminator, a multiple of 16, >= 48 */
	uint32_t frames;                   /* decoded samples (also in the header); the last block may hold padding up to blocks * 28 */
	uint32_t rate;                     /* playback rate in Hz for the original pitch (loops: slightly below the sound's nominal) */
	uint32_t loop_start, loop_end;     /* byte offsets relative to the first block (sample_offset + 16), valid with SND_P_LOOP: 0 .. size - 32 */
	float    gain, skip;               /* permutation gain (linear) and skip fraction */
	uint16_t next;                     /* permutation (index within the same pitch range) to play after this one, SND_NO_PERM = none */
	uint16_t flags;                    /* SND_P_* */
	uint16_t range;                    /* pitch range it belongs to (index within the sound) */
	uint16_t src_index;                /* position in the tag's own permutation order, debugging */
	char     name[SND_SMALL_NAME_LEN];
	uint32_t src_bytes;                /* size of the Xbox ADPCM source, statistics */
} snd_perm;

typedef struct {
	uint32_t tag_id, flags;
	float    param;                    /* +0x20 of the tag (3.0 or 5.0 on the retail maps), meaning unverified */
	uint16_t track_count, detail_count;
	uint32_t track_first, detail_first;   /* internal */
	char     name[SND_NAME_LEN];
} snd_loop;

typedef struct {
	uint32_t flags;
	float    gain, fade_in, fade_out;
	uint32_t start, loop, end, alt_loop, alt_end;   /* sound tag ids, SND_NO_TAG = none; `loop` and `alt_loop` loop */
} snd_track;

typedef struct {
	uint32_t sound;
	float    period_lo, period_hi;     /* seconds between random plays */
	float    gain;
	uint32_t flags;
	float    yaw_lo, yaw_hi, pitch_lo, pitch_hi, dist_lo, dist_hi;
} snd_detail;

/* Validate every table (sizes, alignment, ranges, sort order, indices, finite floats, terminated names, SNDD bounds).
 * `sndd_size` is the raw size of the SNDD section. On failure *s is left empty and safe to query. */
snd_status snd_open(snd_section *s, const uint8_t *snd0, size_t size, uint32_t sndd_size);

int        snd_find(const snd_section *s, uint32_t tag_id);              /* sound index, -1 if absent */
snd_sound  snd_get_sound(const snd_section *s, uint32_t index);          /* neutral zero record if out of range */
snd_range  snd_get_range(const snd_section *s, const snd_sound *snd, uint32_t k);
snd_perm   snd_get_perm(const snd_section *s, const snd_sound *snd, uint32_t k);   /* k < snd->perm_count */

int        snd_find_loop(const snd_section *s, uint32_t tag_id);        /* sound_looping index, -1 if absent */
snd_loop   snd_get_loop(const snd_section *s, uint32_t index);
snd_track  snd_get_track(const snd_section *s, const snd_loop *lp, uint32_t k);
snd_detail snd_get_detail(const snd_section *s, const snd_loop *lp, uint32_t k);

/* Check SNDD against the tables: every sample has the APCM header (loop byte = SND_P_LOOP, pitch = floor(rate * 4096 / 48000), frames),
 * every block's predictor/shift in range, the flag pattern of wrap_audsrv (one-shot: 0 .. 0, 1; loop: 6, 0 .. 0, 3 on the first and
 * last block; terminator 7 with zero nibbles). Returns 0 if all fine, otherwise 1 + the index of the first bad permutation.
 * `sndd` has `n` bytes (= sndd_size). */
uint32_t   snd_check_vag(const snd_section *s, const uint8_t *sndd, size_t n);

#endif
