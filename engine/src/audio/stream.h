/* Streamed audio player (docs/29-strm-section.md): dialogue, music and long ambience from `<map>.strm.hp2`, mixed on the EE and
 * handed to audsrv as one 48 kHz stereo 16-bit PCM stream.
 *
 * Two parts in stream.c:
 *  - the pure part (no ps2sdk; host-tested by engine/tests/test_stream.c): voices, read-ahead planner, block decoder driver, PCM rings,
 *    resampling mixer, fades, loops, statistics. It never blocks: file reads are *started* and *polled* through `stream_io`, mixing is
 *    called with exactly as many frames as the consumer can take.
 *  - the EE glue (`#ifdef _EE`, at the end of stream.c): a stream_io on top of an IO thread that makes blocking fio calls, the audsrv output
 *    (what blocks and what does not is documented there), and the entry points `stream_ee_tick` (30 Hz) and `stream_ee_pump` (in between).
 *
 * Voices: three slots, one per kind (STREAM_SLOT_*). A slot plays one sound at a time; starting another in a busy slot replaces it.
 * The tick (30 Hz) does the I/O, the decoding and the state machine; mixing is sample accurate and happens in stream_pump.
 *
 * Timeline of a looping sound (sound_looping track): start (once) -> loop (repeats; alt_loop replaces it from the next pass on after
 * stream_loop_alternate) -> on stop: the sound fades out over its fade_out, then the end sound (if any) plays once at the track gain.
 * Units: seconds for fades, linear gains (1.0 = unity), pan -1 (left) .. 1 (right) for mono sources (stereo sources ignore it). */
#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H
#include "audio/strmdef.h"
#include <stddef.h>
#include <stdint.h>

#define STREAM_VOICES 3
enum { STREAM_SLOT_DIALOGUE = 0, STREAM_SLOT_MUSIC = 1, STREAM_SLOT_AMBIENCE = 2 };

#define STREAM_MIX_RATE 48000u            /* output rate; stereo, 16-bit */
#define STREAM_CHUNK_BYTES 32760u         /* read size: 910 mono or 455 stereo blocks (a whole number of both), 31.99 KiB */
#define STREAM_CHUNK_BUFFER 32768u        /* buffer behind a chunk: the EE glue rounds reads up to whole 64-byte lines */
#define STREAM_CHUNKS 3u                  /* chunk buffers per voice (read-ahead depth) */
#define STREAM_PCM_FRAMES 4096u           /* decoded source frames per voice (power of two) */
#define STREAM_MIX_RING 8192u             /* output frames the player keeps ahead (power of two) */
#define STREAM_DEFAULT_TARGET 4800u       /* frames the pump keeps queued: 100 ms */
#define STREAM_PRIME_FRAMES 1024u         /* decoded source frames before a voice becomes audible */

/* stream_play_* flags */
#define STREAM_PLAY_LOOP 1u               /* repeat loop_start..loop_end (or the whole stream) until stopped */

/* Asynchronous reads of the stream data. Exactly one read is outstanding at any time. `offset` is absolute in the pack file. */
typedef struct {
	int  (*start)(void *user, uint32_t offset, void *dst, uint32_t bytes);   /* begin reading; 0 = started, else error */
	int  (*poll)(void *user);                                                /* 1 = finished ok, 0 = still running, < 0 = failed */
	void *user;
} stream_io;

typedef struct {
	uint32_t ticks;
	uint32_t reads, read_bytes, read_errors;
	uint32_t read_wait_ticks;             /* ticks that began with a read still running (disc slower than the planner) */
	uint32_t frames_decoded, frames_mixed, frames_silence;
	uint32_t voice_underruns;             /* times a playing voice ran out of decoded data */
	uint32_t voice_underrun_frames;       /* output frames mixed without that voice's data */
	uint32_t out_underruns;               /* glue: audsrv ran dry between two ticks */
	uint32_t plays, steals, finished, loop_passes, format_mismatch, ring_high;
	uint32_t start_latency_max;           /* ticks from a play request to the voice becoming audible, worst case */
	/* filled in by the glue */
	float    ms_last, ms_max, ms_sum;     /* EE time of one stream_ee_tick */
	uint32_t pushed_frames, push_calls, push_partial;
	uint32_t queued_min, queued_max;      /* audsrv ring fill (frames) sampled at the start of each tick */
	uint32_t service_calls;               /* glue: stream_ee_tick + stream_ee_pump calls (ms_sum / service_calls = average) */
	float    ms_tick_sum, ms_pump_sum, ms_aud_sum;   /* glue: time in stream_tick (I/O, decode), stream_pump (mix) and the audsrv RPCs; ticks / service_calls give the divisors */
	float    ms_audsrv_max;               /* worst audsrv_available + audsrv_play_audio time inside a tick */
} stream_stats;

typedef struct {
	uint8_t  state;                       /* 0 free, 1 read in flight, 2 loaded, 3 in flight but abandoned */
	uint32_t seq;                         /* order of the read within the voice's plan */
	uint32_t bytes;                       /* bytes read / being read (whole blocks) */
	uint32_t pos;                         /* decode cursor, bytes */
	uint32_t skip;                        /* frames to drop at the start (loop restarts at a non-block boundary) */
	uint32_t limit;                       /* frames still allowed out of this chunk (after skip) */
	uint32_t channels;
	uint8_t  data[STREAM_CHUNK_BUFFER] __attribute__((aligned(64)));
} stream_chunk;

/* Everything in front of `chunk` is cleared when a voice is reset (offsetof); keep the arrays last. */
typedef struct {
	uint8_t  active;
	uint8_t  plan;                        /* PLAN_* (stream.c) */
	uint8_t  primed, stopping, starving, alt_on, has_format;
	uint8_t  seg_open, seg_first;
	uint32_t channels;
	uint32_t rate;
	uint32_t step;                        /* source frames per output frame, Q16 */
	uint32_t phase;                       /* Q16 fraction between pcm_r and pcm_r + 1 */
	/* source plan */
	uint32_t ref_start, ref_loop, ref_alt, ref_end;     /* stream table indices, 0xFFFFFFFF = none */
	uint32_t cur_ref;
	uint32_t seg_block, seg_end_block, seg_skip, seg_frames_left;
	uint32_t seq_plan, seq_dec, errors;
	/* decoded ring: monotonic counters, ring index = counter & (STREAM_PCM_FRAMES - 1) */
	uint32_t pcm_r, pcm_w;
	/* gain: Q23 (1.0 = 1 << 23) ramping towards target; pan gains Q15 */
	int32_t  gain, target, gain_step, track_gain, base_gain, pan_l, pan_r;
	uint32_t fade_out_frames;
	uint32_t age_ticks;
	stream_chunk chunk[STREAM_CHUNKS];
	int16_t  pcm[STREAM_PCM_FRAMES * 2];  /* interleaved when stereo */
} stream_voice;

typedef struct stream_player {   /* tagged so sim/mission.h can forward declare it */
	const strm_section *sec;
	stream_io           io;
	uint32_t            strd_base;        /* file offset of STRD in the pack */
	uint32_t            rng;
	int                 io_voice, io_chunk;   /* the outstanding read, -1 none */
	uint32_t            target_frames;
	uint32_t            idle_frames;      /* frames mixed since the last voice ended */
	uint32_t            ring_r, ring_w;
	stream_voice        voice[STREAM_VOICES];
	int16_t             ring[STREAM_MIX_RING * 2];
	stream_stats        stats;
} stream_player;

void stream_init(stream_player *p, const strm_section *sec, const stream_io *io, uint32_t strd_base);

/* Plays variant `variant` (< 0: pick one at random) of the sound with tag `tag_id` in `slot`. Returns the stream index played, or -1
 * (unknown sound, bad slot, read plan impossible). `fade_in_s` < 0 means none. */
int  stream_play_sound(stream_player *p, int slot, uint32_t tag_id, int variant, uint32_t flags, float gain, float pan, float fade_in_s);
/* Plays a stream of the index table directly. */
int  stream_play_stream(stream_player *p, int slot, uint32_t stream_index, uint32_t flags, float gain, float pan, float fade_in_s);
/* Plays track `track` of the sound_looping tag `loop_tag` (start, loop, end, alt_loop of the track; ids that are not in the STRM table are
 * ignored). `fade_in_s` < 0 uses the track's. Returns 0 on success, -1 if nothing of the track can be streamed. */
int  stream_play_loop(stream_player *p, int slot, uint32_t loop_tag, unsigned track, float gain, float pan, float fade_in_s);
/* Fades the slot out over `fade_out_s` (< 0: the track's fade_out for a loop, else 0.05 s) and stops it; a loop then plays its end sound. */
void stream_stop(stream_player *p, int slot, float fade_out_s);
/* Hard stop: no fade, no end sound. */
void stream_kill(stream_player *p, int slot);
/* Switches a looping slot between its loop and its alternate loop at the next pass boundary that has not been read yet. */
void stream_loop_alternate(stream_player *p, int slot, int on);
/* Ramps the slot's gain to `gain` (times the track gain) over `ramp_s` seconds (0: next frame). */
void stream_set_gain(stream_player *p, int slot, float gain, float ramp_s);
int  stream_slot_busy(const stream_player *p, int slot);

/* The 30 Hz tick: completes and starts reads, decodes into the voices' rings, advances the planner. Never blocks, bounded work. */
void stream_tick(stream_player *p);
/* Renders mixed frames into the player's output ring until `queued_frames` (what the consumer still holds) plus the ring's content reach
 * the target. Returns the frames rendered; 0 when nothing has played for a while (the consumer may then go quiet). */
unsigned stream_pump(stream_player *p, unsigned queued_frames);
unsigned stream_ring_count(const stream_player *p);
/* Contiguous run of ready frames (interleaved L R) at the ring's read side; `*frames` is the run's length, may be 0. */
const int16_t *stream_ring_peek(const stream_player *p, unsigned *frames);
void     stream_ring_consume(stream_player *p, unsigned frames);
/* Copies up to `max` frames out (convenience for tests). */
unsigned stream_ring_read(stream_player *p, int16_t *dst, unsigned max);

void stream_set_target(stream_player *p, unsigned frames);
const stream_stats *stream_get_stats(const stream_player *p);
/* Frames of the voice's source that are decoded and waiting, plus unread chunks in flight (debug / tests). */
uint32_t stream_voice_buffered(const stream_player *p, int slot);

#ifdef _EE
/* ---- EE glue ------------------------------------------------------------------------------------------------------------------ */
#include "core/mem.h"
typedef struct {
	stream_player *player;
	strm_section   sec;
	uint8_t       *strm;                  /* STRM section bytes (arena) */
	int            fd;                    /* fio descriptor of the pack, -1 closed */
	int            read_pending;
	uint32_t       want;                  /* bytes the outstanding read must deliver at least */
	/* IO thread (see stream.c) */
	int            io_tid, req_sema, quit, main_priority, main_priority_orig, main_demoted;
	volatile int   req_done, req_result, io_exited;
	uint32_t       req_off, req_size;
	void          *req_dst;
	uint8_t        io_stack[16384] __attribute__((aligned(16)));
	int            audsrv_ok;
	int            ring_capacity;         /* audsrv's ring in bytes (measured right after set_format) */
	int            log, measure;          /* measure: also keep peak / sum / sum of squares of the pushed PCM (cost ~0.05 ms per tick) */
	int32_t        peak[2];
	int64_t        sum[2], sumsq[2];
	uint32_t       measured_frames;
	int16_t       *capture;               /* optional: also keep the first `capture_cap` pushed frames (tests) */
	uint32_t       capture_cap, capture_len;
	uint32_t       strd_offset;
	char           path[96];
} stream_ee;

/* Reads the pack's header and STRM section (blocking, load time), opens the pack for streaming and sets audsrv to 48 kHz stereo 16-bit.
 * Returns 0 on success; on failure the player stays inert (every call a no-op) and the game runs without streamed audio.
 * audio_init() must have run (it loads and starts audsrv). `path` as for file_load ("assets/hp2/a10.strm.hp2"). */
int  stream_ee_open(stream_ee *e, stream_player *p, arena_t *a, const char *path, int log);
/* Once per 30 Hz game tick. Non-blocking: starts/polls the fio read, decodes, mixes and pushes what audsrv has room for. */
void stream_ee_tick(stream_ee *e);
/* Between ticks, e.g. at every vsync of a 30 fps game running on a 60 Hz display: only mixes (from what the tick decoded) and pushes. audsrv's ring
 * holds just 53 ms, so a late 30 Hz tick runs it dry; a pump per vsync halves the exposure. */
void stream_ee_pump(stream_ee *e);
/* Waits for the outstanding read (needed before stream_ee_close; other fio / stdio users do not have to call it: the IO thread makes ordinary
 * blocking fio calls, which the library serialises). */
void stream_ee_sync(stream_ee *e);
void stream_ee_close(stream_ee *e);
#endif

#endif
