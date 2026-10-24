/* Host test for the pure part of audio/stream.c: the streamed audio player (voices, read-ahead planner, decoder driver, resampling mixer,
 * fades, loops, underrun handling, statistics).
 *   test_stream FIXTURE.hp2 [REAL.strm.hp2 ...]
 * Two kinds of data: packs built in memory by this test (streams whose decoded content is known in closed form: a ramp of +1 per sample
 * starting at a block-dependent base, or a constant, so loop points, skips, sequencing and gains are checked exactly), and the pack
 * tools/tests/test_streamconv.py writes (the real converter's output, played through the real container); real packs from assets/ are
 * played for a while with a file backed stream_io. ASan/UBSan clean. */
#include "audio/stream.h"
#include "strm_testlib.h"
#include <math.h>
#include <time.h>

/* ---- in-memory packs ------------------------------------------------------------------------------------------------------------- */
typedef struct {
	uint32_t tag;                  /* the sound's tag id (one variant, one stream) */
	uint8_t  channels;
	uint32_t rate, blocks;
	int      dc;                   /* 1: constant `base` on every sample, 0: ramp (+1 per sample) */
	int32_t  base, base_r;         /* ramp: sample n = (base + (n / 64) * 64) % 30000 + n % 64; r = same with base_r (stereo) */
	uint32_t loop_start, loop_end; /* frames; loop_end 0 = no loop flag */
	float    gain;
} sspec;

typedef struct { uint32_t tag, start, loop, end, alt_loop; float gain, fade_in, fade_out; uint32_t flags; } lspec;

typedef struct {
	uint8_t *strm, *strd;
	size_t   strm_size, strd_size;
	strm_section sec;
} mempack;

static void wr16le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wrf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); wr32le(p, u); }

static int32_t ramp_at(int32_t base, uint32_t n) { return (base + (int32_t)(n / 64) * 64) % 30000 + (int32_t)(n % 64); }
static int32_t sample_at(const sspec *s, unsigned ch, uint32_t n)
{
	if (s->dc) return ch ? s->base_r : s->base;
	return ramp_at(ch ? s->base_r : s->base, n);
}

static int build_pack(mempack *mp, const sspec *st, int nst, const lspec *lp, int nlp)
{
	size_t strd_cap = 0, strm_cap, off = 0;
	uint8_t *hdr;
	uint32_t sounds_off, streams_off, loops_off, tracks_off, align = 64;
	int s, l;
	memset(mp, 0, sizeof *mp);
	for (s = 0; s < nst; s++) strd_cap += (size_t)st[s].blocks * 36 * st[s].channels + 64;
	mp->strd = calloc(1, strd_cap);
	strm_cap = 128 + (size_t)nst * 128 + (size_t)nlp * 128 + 256;
	mp->strm = calloc(1, strm_cap);
	if (!mp->strd || !mp->strm) return -1;
	/* data */
	hdr = mp->strm;
	sounds_off = 128;
	streams_off = sounds_off + (uint32_t)nst * 64;
	loops_off = streams_off + (uint32_t)nst * 64;
	tracks_off = loops_off + (uint32_t)nlp * 64;
	for (s = 0; s < nst; s++) {
		uint8_t *rec = hdr + streams_off + (size_t)s * 64, *snd = hdr + sounds_off + (size_t)s * 64;
		uint32_t unit = 36u * st[s].channels, b;
		off = (off + align - 1) & ~(size_t)(align - 1);
		for (b = 0; b < st[s].blocks; b++) {
			uint8_t *blk = mp->strd + off + (size_t)b * unit;
			uint32_t pl = st[s].dc ? (uint32_t)st[s].base : (uint32_t)((st[s].base + (int32_t)b * 64) % 30000);
			uint32_t pr = st[s].dc ? (uint32_t)st[s].base_r : (uint32_t)((st[s].base_r + (int32_t)b * 64) % 30000);
			wr16le(blk, pl);
			if (st[s].channels == 1) {
				if (!st[s].dc) memset(blk + 4, 0x11, 32);
			} else {
				wr16le(blk + 4, pr);
				if (!st[s].dc) memset(blk + 8, 0x11, 64);
			}
		}
		wr32le(rec, (uint32_t)off);
		wr32le(rec + 4, st[s].blocks * unit);
		wr32le(rec + 8, st[s].blocks * 64);
		wr32le(rec + 12, st[s].rate);
		wr32le(rec + 16, st[s].loop_end ? st[s].loop_start : 0);
		wr32le(rec + 20, st[s].loop_end);
		wrf(rec + 24, st[s].gain > 0 ? st[s].gain : 1.0f);
		rec[32] = st[s].channels;
		rec[33] = STRM_FMT_XBOX_IMA;
		wr16le(rec + 34, st[s].loop_end ? STRM_F_LOOP : 0);
		wr16le(rec + 36, 1);
		wr32le(rec + 40, st[s].tag);
		wr32le(snd, st[s].tag);
		snd[4] = 32;
		wr16le(snd + 8, 1);
		wr32le(snd + 12, (uint32_t)s);
		off += (size_t)st[s].blocks * unit;
	}
	for (l = 0; l < nlp; l++) {
		uint8_t *rec = hdr + loops_off + (size_t)l * 64, *tr = hdr + tracks_off + (size_t)l * 48;
		wr32le(rec, lp[l].tag);
		wr32le(rec + 4, lp[l].flags);
		wrf(rec + 8, 5.0f);
		wr16le(rec + 12, 1);
		wr32le(rec + 16, (uint32_t)l);
		wr32le(tr, 0);
		wrf(tr + 4, lp[l].gain);
		wrf(tr + 8, lp[l].fade_in);
		wrf(tr + 12, lp[l].fade_out);
		wr32le(tr + 16, lp[l].start);
		wr32le(tr + 20, lp[l].loop);
		wr32le(tr + 24, lp[l].end);
		wr32le(tr + 28, lp[l].alt_loop);
		wr32le(tr + 32, STRM_NO_TAG);
	}
	mp->strd_size = (off + 63) & ~(size_t)63;
	mp->strm_size = tracks_off + (size_t)nlp * 48;
	memcpy(hdr, "STRM", 4);
	wr16le(hdr + 4, 1);
	wr32le(hdr + 8, (uint32_t)mp->strm_size);
	wr32le(hdr + 0x0C, (uint32_t)nst); wr32le(hdr + 0x10, (uint32_t)nst); wr32le(hdr + 0x14, (uint32_t)nlp); wr32le(hdr + 0x18, (uint32_t)nlp);
	wr32le(hdr + 0x1C, sounds_off); wr32le(hdr + 0x20, streams_off); wr32le(hdr + 0x24, loops_off); wr32le(hdr + 0x28, tracks_off);
	wr32le(hdr + 0x2C, (uint32_t)mp->strd_size);
	wr32le(hdr + 0x30, align);
	if (strm_open(&mp->sec, mp->strm, mp->strm_size, (uint32_t)mp->strd_size) != STRM_OK) return -2;
	return 0;
}

static void free_pack(mempack *mp) { free(mp->strm); free(mp->strd); memset(mp, 0, sizeof *mp); }

/* ---- a stream_io over memory, with latency and error injection and checks of the I/O discipline -------------------------------------- */
typedef struct {
	const uint8_t *image;                 /* the bytes `offset` counts into (STRD for in-memory packs, the whole pack for the fixture) */
	size_t         size;
	const strm_section *sec;
	uint32_t       base;                  /* image offset of STRD */
	int            busy;
	int            latency, countdown;    /* polls that answer "still running" */
	void          *dst;
	uint32_t       off, bytes;
	unsigned       reads, fail_every, fail_start_at, violations;
	unsigned long long total;
	FILE          *file;                  /* if set, read from the file instead (real packs) */
} membe;

static int range_ok(const membe *m, uint32_t off, uint32_t bytes)
{
	uint32_t i;
	for (i = 0; i < m->sec->stream_count; i++) {
		strm_stream t = strm_get_stream(m->sec, i);
		if (off >= m->base + t.data_offset && (uint64_t)off + bytes <= (uint64_t)m->base + t.data_offset + t.data_size) return 1;
	}
	return 0;
}

static int be_start(void *u, uint32_t off, void *dst, uint32_t bytes)
{
	membe *m = u;
	if (m->busy) { m->violations++; return -1; }                 /* two reads at once */
	if (bytes == 0 || bytes > STREAM_CHUNK_BYTES || (m->sec && !range_ok(m, off, bytes))) { m->violations++; return -1; }
	if (!m->file && (size_t)off + bytes > m->size) { m->violations++; return -1; }
	m->reads++;
	if (m->fail_start_at && m->reads == m->fail_start_at) return -1;
	m->busy = 1; m->dst = dst; m->off = off; m->bytes = bytes; m->countdown = m->latency;
	return 0;
}

static int be_poll(void *u)
{
	membe *m = u;
	if (!m->busy) { m->violations++; return -1; }
	if (m->countdown > 0) { m->countdown--; return 0; }
	m->busy = 0;
	if (m->fail_every && m->reads % m->fail_every == 0) return -1;
	if (m->file) {
		if (fseek(m->file, (long)m->off, SEEK_SET) != 0 || fread(m->dst, 1, m->bytes, m->file) != m->bytes) return -1;
	} else {
		memcpy(m->dst, m->image + m->off, m->bytes);
	}
	m->total += m->bytes;
	return 1;
}

static void be_init(membe *m, const uint8_t *image, size_t size, const strm_section *sec, uint32_t base)
{
	memset(m, 0, sizeof *m);
	m->image = image; m->size = size; m->sec = sec; m->base = base;
}

/* ---- harness ---------------------------------------------------------------------------------------------------------------------- */
typedef struct {
	stream_player *p;
	membe         *be;
	int16_t       *out;                   /* interleaved L R */
	size_t         frames, cap;
	unsigned       per_tick;              /* frames the consumer takes per tick (1600) */
} harness;

static void h_init(harness *h, stream_player *p, membe *be, size_t cap_frames)
{
	stream_io io;
	memset(h, 0, sizeof *h);
	io.start = be_start; io.poll = be_poll; io.user = be;
	stream_init(p, p->sec, &io, be->base);
	h->p = p; h->be = be; h->cap = cap_frames; h->per_tick = 1600;
	stream_set_target(p, 1600);                                  /* mix exactly what the consumer takes each tick: no look-ahead to reason about */
	h->out = calloc(cap_frames * 2, sizeof(int16_t));
}

static void h_free(harness *h) { free(h->out); }

static void h_run(harness *h, unsigned ticks)
{
	unsigned t;
	for (t = 0; t < ticks; t++) {
		stream_tick(h->p);
		stream_pump(h->p, 0);
		if (h->frames + h->per_tick <= h->cap) h->frames += stream_ring_read(h->p, h->out + 2 * h->frames, h->per_tick);
		else stream_ring_consume(h->p, h->per_tick);                  /* past the capture buffer: the consumer still takes its frames */
	}
}

static int abs_i(int v) { return v < 0 ? -v : v; }

/* max |out[from + j] - expect(j)| over [0, n) for channel ch */
typedef int32_t (*expect_fn)(void *ctx, uint32_t j, unsigned ch);
static int max_err(const harness *h, size_t from, size_t n, unsigned ch, expect_fn f, void *ctx)
{
	size_t j;
	int worst = 0;
	for (j = 0; j < n && from + j < h->frames; j++) {
		int e = abs_i((int)h->out[2 * (from + j) + ch] - (int)f(ctx, (uint32_t)j, ch));
		if (e > worst) worst = e;
	}
	return worst;
}

/* ---- the in-memory world: streams and what they are ------------------------------------------------------------------------------ */
enum { T_RAMP = 0x1000, T_LOOPSKIP, T_START, T_LOOP, T_ALT, T_END, T_STEREO, T_HALF, T_R22, T_LONG, T_STEREO_LOOP, T_MONO_LOOP, T_BAD_LOOP, T_DC, T_DC2, T_DC3 };
static const sspec k_streams[] = {
	{ T_RAMP,        1, 48000, 40,   0, 0,     0,     0,    0,    0 },
	{ T_LOOPSKIP,    1, 48000, 40,   0, 0,     0,     100,  1000, 0 },
	{ T_START,       1, 48000, 10,   1, 1000,  0,     0,    0,    0 },
	{ T_LOOP,        1, 48000, 20,   1, 5000,  0,     0,    1280, 0 },
	{ T_ALT,         1, 48000, 20,   1, 9000,  0,     0,    1280, 0 },
	{ T_END,         1, 48000, 10,   1, 13000, 0,     0,    0,    0 },
	{ T_STEREO,      2, 48000, 20,   0, 0,     20000, 0,    0,    0 },
	{ T_HALF,        1, 24000, 20,   0, 0,     0,     0,    0,    0 },
	{ T_R22,         1, 22050, 40,   0, 0,     0,     0,    0,    0 },
	{ T_LONG,        1, 48000, 3000, 0, 0,     0,     0,    0,    0 },
	{ T_STEREO_LOOP, 2, 48000, 20,   1, 3000,  3000,  0,    1280, 0 },
	{ T_MONO_LOOP,   1, 48000, 20,   1, 3000,  0,     0,    1280, 0 },
	{ T_BAD_LOOP,    2, 48000, 20,   1, 3000,  3000,  0,    1280, 0 },
	{ T_DC,          1, 48000, 50,   1, 10000, 0,     0,    0,    0 },
	{ T_DC2,         1, 48000, 50,   1, 20000, 0,     0,    0,    0 },
	{ T_DC3,         1, 48000, 50,   1, 12000, 0,     0,    0,    0 },
};
#define NST ((int)(sizeof k_streams / sizeof k_streams[0]))
#define LT_SEQ 0x2000u      /* start -> loop (+ alternate) -> end, gain 0.5, fade in 0.1 s, fade out 0.1 s */
#define LT_MISMATCH 0x2001u /* mono start, stereo loop: the loop cannot join the voice's format */
#define LT_LOOPONLY 0x2002u
#define LT_ONCE 0x2003u     /* start -> loop, flagged "not a loop": the loop segment plays once and the voice ends */
static const lspec k_loops[] = {
	{ LT_SEQ,      T_START, T_LOOP, T_END, T_ALT, 0.5f, 0.1f, 0.1f, 0 },
	{ LT_MISMATCH, T_START, T_STEREO_LOOP, STRM_NO_TAG, STRM_NO_TAG, 1.0f, 0.0f, 0.0f, 0 },
	{ LT_LOOPONLY, STRM_NO_TAG, T_LOOP, STRM_NO_TAG, STRM_NO_TAG, 1.0f, 0.0f, 0.5f, 0 },
	{ LT_ONCE,     T_START, T_LOOP, STRM_NO_TAG, STRM_NO_TAG, 1.0f, 0.0f, 0.0f, STRM_LOOP_NOT_A_LOOP },
};
#define NLP ((int)(sizeof k_loops / sizeof k_loops[0]))

static const sspec *spec_of(uint32_t tag)
{
	int i;
	for (i = 0; i < NST; i++) if (k_streams[i].tag == tag) return &k_streams[i];
	return NULL;
}

/* one-shot stream at 48 kHz: out[j] = source[j] */
static int32_t exp_identity(void *c, uint32_t j, unsigned ch) { const sspec *s = c; return j < s->blocks * 64 ? sample_at(s, ch, j) : 0; }
/* looped stream with first pass over everything: 0 .. frames-1, then loop_start .. loop_end-1 forever */
static int32_t exp_loopskip(void *c, uint32_t j, unsigned ch)
{
	const sspec *s = c;
	uint32_t n = s->blocks * 64, len = s->loop_end - s->loop_start;
	if (j < n) return sample_at(s, ch, j);
	return sample_at(s, ch, s->loop_start + (j - n) % len);
}

#define NEW_PLAYER() ((stream_player *)calloc(1, sizeof(stream_player)))

static int run_basic(void)
{
	mempack mp;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	const sspec *ramp = spec_of(T_RAMP), *ls = spec_of(T_LOOPSKIP), *half = spec_of(T_HALF), *st = spec_of(T_STEREO);
	size_t j;
	int e;
	CHECK(build_pack(&mp, k_streams, NST, k_loops, NLP) == 0);
	p->sec = &mp.sec;
	be_init(&be, mp.strd, mp.strd_size, &mp.sec, 0);
	h_init(&h, p, &be, 60000);

	/* one-shot at 48 kHz: the output is the source, sample for sample, then silence; the slot frees itself */
	CHECK(stream_play_sound(p, STREAM_SLOT_DIALOGUE, T_RAMP, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	CHECK(stream_slot_busy(p, STREAM_SLOT_DIALOGUE));
	h_run(&h, 4);
	e = max_err(&h, 0, 2560 + 3000, 0, exp_identity, (void *)ramp);
	CHECK(e == 0);
	CHECK(max_err(&h, 0, 2560 + 3000, 1, exp_identity, (void *)ramp) == 0);        /* mono source: both channels (pan 0) */
	CHECK(!stream_slot_busy(p, STREAM_SLOT_DIALOGUE) && p->stats.finished == 1 && p->stats.plays == 1);
	CHECK(p->stats.read_bytes == 40u * 36 && p->stats.reads == 1 && be.violations == 0 && p->stats.voice_underruns == 0);
	CHECK(p->stats.frames_decoded == 2560 && be.total == 40u * 36);

	/* looped stream with a loop point inside a block and a loop end that is not a block end: exact sequence, skip and limit at work */
	h.frames = 0;
	memset(h.out, 0, h.cap * 2 * sizeof(int16_t));
	CHECK(stream_play_sound(p, STREAM_SLOT_MUSIC, T_LOOPSKIP, 0, STREAM_PLAY_LOOP, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 40);                                                                  /* 64000 frames, 3 passes of the loop ... */
	e = max_err(&h, 0, 2560 + 900 * 60, 0, exp_loopskip, (void *)ls);
	if (e) fprintf(stderr, "loop sequence error %d\n", e);
	CHECK(e == 0);
	CHECK(stream_slot_busy(p, STREAM_SLOT_MUSIC) && p->stats.loop_passes >= 60 && p->stats.voice_underruns == 0 && be.violations == 0);
	stream_kill(p, STREAM_SLOT_MUSIC);
	CHECK(!stream_slot_busy(p, STREAM_SLOT_MUSIC));

	/* 24 kHz: every output frame in between is the midpoint (the ramp makes interpolation exact up to the floor) */
	h.frames = 0;
	CHECK(stream_play_stream(p, STREAM_SLOT_AMBIENCE, 7, 0, 1.0f, 0.0f, -1.0f) == 7);
	h_run(&h, 5);
	for (j = 0; j < 1280 * 2 - 4; j++) {
		int want2 = (int)j;                                                         /* 2 * source position */
		int got2 = 2 * h.out[2 * j] + 0;
		if (abs_i(got2 - want2) > 2) { fprintf(stderr, "lerp at %zu: %d vs %d (out %d %d %d %d, frames %zu)\n", j, got2, want2, h.out[0], h.out[2], h.out[4], h.out[6], h.frames); return 1; }
	}
	(void)half;

	/* stereo source: independent channels, pan ignored */
	h.frames = 0;
	CHECK(stream_play_stream(p, STREAM_SLOT_DIALOGUE, 6, 0, 1.0f, 1.0f, -1.0f) == 6);
	h_run(&h, 3);
	CHECK(max_err(&h, 0, 1280, 0, exp_identity, (void *)st) == 0 && max_err(&h, 0, 1280, 1, exp_identity, (void *)st) == 0);

	/* unknown things fail cleanly */
	CHECK(stream_play_sound(p, 0, 0xDEAD, 0, 0, 1, 0, 0) == -1 && stream_play_sound(p, 7, T_RAMP, 0, 0, 1, 0, 0) == -1);
	CHECK(stream_play_sound(p, -1, T_RAMP, 0, 0, 1, 0, 0) == -1 && stream_play_stream(p, 0, 9999, 0, 1, 0, 0) == -1);
	CHECK(stream_play_loop(p, 0, 0xBEEF, 0, 1, 0, 0) == -1 && stream_play_loop(p, 0, LT_SEQ, 5, 1, 0, 0) == -1);
	stream_stop(p, 9, 0); stream_kill(p, -3); stream_set_gain(p, 17, 1, 0); stream_loop_alternate(p, 4, 1);
	CHECK(!stream_slot_busy(p, 5) && !stream_slot_busy(p, -1));
	h_free(&h);
	free(p);
	free_pack(&mp);
	return 0;
}

/* ---- gain, pan, fades, mixing ---------------------------------------------------------------------------------------------------- */
static int run_mix(void)
{
	mempack mp;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	size_t j;
	CHECK(build_pack(&mp, k_streams, NST, k_loops, NLP) == 0);
	p->sec = &mp.sec;
	be_init(&be, mp.strd, mp.strd_size, &mp.sec, 0);
	h_init(&h, p, &be, 60000);

	/* three voices at once: the mix is the sum (exact: unity gains), and clamps at full scale */
	CHECK(stream_play_sound(p, 0, T_DC, 0, STREAM_PLAY_LOOP, 1.0f, 0.0f, -1.0f) >= 0);   /* these streams carry no loop flag: one-shot */
	CHECK(stream_play_sound(p, 1, T_DC2, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 1);
	for (j = 0; j < 1000; j++) CHECK(h.out[2 * j] == 30000 && h.out[2 * j + 1] == 30000);
	CHECK(stream_play_sound(p, 2, T_DC3, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h.frames = 0;
	h_run(&h, 1);
	for (j = 0; j < 1000; j++) CHECK(h.out[2 * j] == 32767 && h.out[2 * j + 1] == 32767);   /* 10000 + 20000 + 12000 clamps */
	stream_kill(p, 0); stream_kill(p, 1); stream_kill(p, 2);

	/* gain 0.5 and a hard pan: left = gain * source, right = silent */
	h.frames = 0;
	CHECK(stream_play_sound(p, 0, T_DC, 0, 0, 0.5f, -1.0f, -1.0f) >= 0);
	h_run(&h, 1);
	for (j = 0; j < 1000; j++) CHECK(h.out[2 * j] == 5000 && h.out[2 * j + 1] == 0);
	stream_kill(p, 0);
	h.frames = 0;
	CHECK(stream_play_sound(p, 0, T_DC, 0, 0, 1.0f, 1.0f, -1.0f) >= 0);
	h_run(&h, 1);
	for (j = 0; j < 1000; j++) CHECK(h.out[2 * j] == 0 && h.out[2 * j + 1] == 10000);
	stream_kill(p, 0);
	h.frames = 0;
	CHECK(stream_play_sound(p, 0, T_DC, 0, 0, 1.0f, 0.5f, -1.0f) >= 0);
	h_run(&h, 1);
	CHECK(h.out[100] == 5000 && h.out[101] == 10000);                                         /* pan 0.5: left halved, right full */
	stream_kill(p, 0);

	/* fade in over 0.05 s (2400 frames): linear from 0 to the gain, then constant */
	h.frames = 0;
	{
		int prev = -1;
		CHECK(stream_play_sound(p, 0, T_DC, 0, 0, 1.0f, 0.0f, 0.02f) >= 0);                   /* 0.02 s = 960 frames, the stream lasts 3200 */
		h_run(&h, 2);
		CHECK(h.out[0] < 100 && abs_i(h.out[2 * 480] - 5000) < 120 && h.out[2 * 1200] == 10000 && h.out[2 * 2000] == 10000);
		for (j = 0; j < 960; j++) { CHECK(h.out[2 * j] >= prev); prev = h.out[2 * j]; }
		CHECK(abs_i(h.out[2 * 959] - 10000) < 40);
	}
	stream_kill(p, 0);

	/* stop with a fade out: linear to zero over 0.01 s (480 frames) of a ramp source, then the slot is free and only silence follows */
	h.frames = 0;
	CHECK(stream_play_sound(p, 0, T_LONG, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 2);
	{
		size_t at = h.frames, k, lastnz = 0;
		stream_stop(p, 0, 0.01f);
		h_run(&h, 3);
		CHECK(!stream_slot_busy(p, 0) && p->stats.finished >= 1);
		for (k = 0; k < h.frames; k++) if (h.out[2 * k]) lastnz = k;
		CHECK(lastnz >= at + 470 && lastnz <= at + 480);
		for (k = lastnz + 1; k < h.frames; k++) CHECK(h.out[2 * k] == 0 && h.out[2 * k + 1] == 0);
		for (k = 0; k < 480; k += 40) {                                                       /* the envelope is linear: source (a ramp, value = index) x (1 - (k + 1) / 480) */
			double want = (double)(at + k) * (1.0 - (double)(k + 1) / 480.0);
			CHECK(fabs((double)h.out[2 * (at + k)] - want) <= 2.0);
		}
	}

	/* set_gain: ramps to the new gain; DC source makes the envelope visible */
	h.frames = 0;
	p->stats.finished = 0;
	CHECK(stream_play_sound(p, 0, T_DC, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 1);
	stream_set_gain(p, 0, 0.25f, 0.01f);                                                       /* 480 frames from here */
	h_run(&h, 2);
	{
		int prev = 100000;
		size_t k, first = 0;
		for (k = 0; k < h.frames && h.out[2 * k] == 10000; k++) first = k;
		for (k = first; k < first + 600 && k < h.frames; k++) { CHECK(h.out[2 * k] <= prev); prev = h.out[2 * k]; }
		CHECK(h.out[2 * (first + 700)] == 2500);
	}
	stream_kill(p, 0);
	h_free(&h);
	free(p);
	free_pack(&mp);
	return 0;
}

/* ---- sound_looping: start -> loop -> alternate -> stop -> end ---------------------------------------------------------------------- */
/* Longest run of equal values around out[at] */
static size_t run_len(const harness *h, size_t at)
{
	size_t a = at, b = at;
	while (a > 0 && h->out[2 * (a - 1)] == h->out[2 * at]) a--;
	while (b + 1 < h->frames && h->out[2 * (b + 1)] == h->out[2 * at]) b++;
	return b - a + 1;
}

/* ---- "not a loop" tracks and layered voices (a cinematic's score over its foley and a spoken line) ---------------------------------- */
static int run_once_layers(void)
{
	mempack mp;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	size_t k;
	CHECK(build_pack(&mp, k_streams, NST, k_loops, NLP) == 0);
	p->sec = &mp.sec;
	be_init(&be, mp.strd, mp.strd_size, &mp.sec, 0);
	h_init(&h, p, &be, 60000);

	/* one voice: the start (640 frames, DC 1000) and the loop segment once (1280 frames, DC 5000), then it is over */
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, LT_ONCE, 0, 1.0f, 0.0f, 0.0f) == 0);
	CHECK(stream_slot_owner(p, STREAM_SLOT_MUSIC) == LT_ONCE && stream_slot_owner(p, STREAM_SLOT_LAYER0) == 0);
	CHECK(stream_free_slot(p, STREAM_SLOT_MUSIC, 1) == -1 && stream_free_slot(p, STREAM_SLOT_LAYER0, STREAM_LAYERS) == STREAM_SLOT_LAYER0);
	h_run(&h, 20);
	for (k = 0; k < 640; k++) CHECK(h.out[2 * k] == 1000);
	for (k = 640; k < 640 + 1280; k++) CHECK(h.out[2 * k] == 5000);
	for (k = 640 + 1280; k < h.frames; k++) CHECK(h.out[2 * k] == 0);
	CHECK(!stream_slot_busy(p, STREAM_SLOT_MUSIC) && p->stats.finished == 1 && p->stats.loop_passes == 0);
	CHECK(stream_slot_owner(p, STREAM_SLOT_MUSIC) == 0);

	/* two voices of the same track, started together on a kind slot and a layer: they add, each ends on its own, a stop reaches one only */
	h.frames = 0;
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, LT_ONCE, 0, 1.0f, 0.0f, 0.0f) == 0);
	CHECK(stream_play_loop(p, STREAM_SLOT_LAYER0, LT_ONCE, 0, 0.5f, 0.0f, 0.0f) == 0);
	CHECK(stream_free_slot(p, STREAM_SLOT_LAYER0, STREAM_LAYERS) == STREAM_SLOT_LAYER0 + 1);
	h_run(&h, 20);
	for (k = 0; k < 640; k++) CHECK(h.out[2 * k] == 1500);
	for (k = 640; k < 640 + 1280; k++) CHECK(h.out[2 * k] == 7500);
	for (k = 640 + 1280; k < h.frames; k++) CHECK(h.out[2 * k] == 0);
	CHECK(!stream_slot_busy(p, STREAM_SLOT_MUSIC) && !stream_slot_busy(p, STREAM_SLOT_LAYER0));

	/* a sound on a layer names its tag as owner and does not disturb the other voices */
	h.frames = 0;
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, LT_LOOPONLY, 0, 1.0f, 0.0f, 0.0f) == 0);
	CHECK(stream_play_sound(p, STREAM_SLOT_LAYER0 + 2, T_RAMP, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	CHECK(stream_slot_owner(p, STREAM_SLOT_LAYER0 + 2) == T_RAMP && stream_slot_owner(p, STREAM_SLOT_MUSIC) == LT_LOOPONLY);
	stream_kill(p, STREAM_SLOT_MUSIC);
	CHECK(stream_slot_busy(p, STREAM_SLOT_LAYER0 + 2) && !stream_slot_busy(p, STREAM_SLOT_MUSIC));
	CHECK(stream_slot_owner(p, STREAM_VOICES) == 0 && stream_slot_owner(p, -1) == 0 && stream_free_slot(p, STREAM_VOICES, 2) == -1);
	free(p);
	free_pack(&mp);
	return 0;
}

static int run_loops(void)
{
	mempack mp;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	size_t k, i, alt_at = 0, end_at = 0, zero_at = 0, fade_from = 0;
	CHECK(build_pack(&mp, k_streams, NST, k_loops, NLP) == 0);
	p->sec = &mp.sec;
	be_init(&be, mp.strd, mp.strd_size, &mp.sec, 0);
	h_init(&h, p, &be, 120000);

	/* gain 0.5 x track gain 0.5 would be 0.25: stream_play_loop's gain is multiplied with the track's, so ask for 1.0 and get 0.5 */
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, LT_SEQ, 0, 1.0f, 0.0f, 0.0f) == 0);       /* fade in 0 */
	h_run(&h, 6);                                                                           /* 9600 frames */
	for (k = 0; k < 640; k++) CHECK(h.out[2 * k] == 500);                                   /* start: DC 1000 x 0.5, 10 blocks */
	for (k = 640; k < 640 + 1280 * 5; k++) CHECK(h.out[2 * k] == 2500);                     /* then the loop, DC 5000 x 0.5, seamless */
	CHECK(p->stats.voice_underruns == 0 && stream_slot_busy(p, STREAM_SLOT_MUSIC));
	/* alternate: switches at a pass boundary (whole passes of 1280 frames), within the read-ahead */
	stream_loop_alternate(p, STREAM_SLOT_MUSIC, 1);
	h.frames = 0;
	h_run(&h, 30);
	for (k = 0; k < h.frames && h.out[2 * k] == 2500; k++) {}
	alt_at = k;
	CHECK(alt_at < h.frames && h.out[2 * alt_at] == 4500);
	CHECK(alt_at < 3 * 1280 + STREAM_PCM_FRAMES + 3 * 1600);                                 /* bounded switching latency */
	CHECK(run_len(&h, 0) == alt_at && alt_at > 0);
	/* a switch back */
	stream_loop_alternate(p, STREAM_SLOT_MUSIC, 0);
	h.frames = 0;
	h_run(&h, 30);
	for (k = 0; k < h.frames && h.out[2 * k] == 4500; k++) {}
	CHECK(k < h.frames && h.out[2 * k] == 2500);
	/* stop: the loop fades out over the track's 0.1 s (4800 frames), then the end sound (DC 13000 x 0.5, 640 frames) plays and the voice ends */
	h.frames = 0;
	h_run(&h, 1);
	stream_stop(p, STREAM_SLOT_MUSIC, -1.0f);
	h_run(&h, 12);
	for (k = 1; k < h.frames; k++) {
		if (h.out[2 * k] == 6500 && h.out[2 * (k - 1)] != 6500) { end_at = k; break; }
	}
	CHECK(end_at > 0 && run_len(&h, end_at) == 640);                                         /* the end sound, whole */
	for (i = end_at; i < end_at + 640; i++) CHECK(h.out[2 * i] == 6500);
	for (i = end_at + 640; i < h.frames; i++) CHECK(h.out[2 * i] == 0);
	CHECK(!stream_slot_busy(p, STREAM_SLOT_MUSIC));
	/* the fade before it: monotonic down to zero over about 4800 frames */
	for (k = end_at; k > 0; k--) if (h.out[2 * (k - 1)] != 0) break;
	zero_at = k;
	CHECK(zero_at < end_at);                                                                 /* silent gap(s) between fade and end sound: flush */
	{
		int prev = 100000;
		size_t kk;
		for (kk = 0; kk < zero_at; kk++) if (h.out[2 * kk] != 2500) { fade_from = kk; break; }
		for (kk = fade_from; kk < zero_at; kk++) { CHECK(h.out[2 * kk] <= prev); prev = h.out[2 * kk]; }
		CHECK(zero_at - fade_from > 4400 && zero_at - fade_from < 5200);
	}
	/* fade in from the track (0.1 s) on a second play, and loop-only tracks: no start, no end; fade out without an end sound frees the slot */
	h.frames = 0;
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, LT_SEQ, 0, 1.0f, 0.0f, -1.0f) == 0);
	h_run(&h, 5);
	CHECK(h.out[0] < 3 && abs_i(h.out[2 * 600] - 62) < 5 && abs_i(h.out[2 * 2400] - 1250) < 40 && h.out[2 * 4900] == 2500);   /* the track's 0.1 s fade in */
	stream_kill(p, STREAM_SLOT_MUSIC);
	h.frames = 0;
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, LT_LOOPONLY, 0, 1.0f, 0.0f, 0.0f) == 0);
	h_run(&h, 3);
	for (k = 0; k < 4000; k++) if (h.out[2 * k] != 5000) { { size_t q; fprintf(stderr, "k=%zu v=%d frames %zu:", k, h.out[2*k], h.frames); for (q = k - 3; q < k + 8; q++) fprintf(stderr, " %d", h.out[2*q]); fprintf(stderr, " mism %u under %u pcm %u/%u\n", p->stats.format_mismatch, p->stats.voice_underruns, p->voice[1].pcm_r, p->voice[1].pcm_w); return 1; } }
	stream_stop(p, STREAM_SLOT_MUSIC, -1.0f);                                                /* the track's 0.5 s */
	h_run(&h, 25);
	CHECK(!stream_slot_busy(p, STREAM_SLOT_MUSIC));
	{
		size_t lastnz = 0;
		for (k = 0; k < h.frames; k++) if (h.out[2 * k]) lastnz = k;
		CHECK(lastnz > 4000 + 20000 && lastnz < 4000 + 4800 + 30000);
	}
	/* a track whose loop has another channel count: the start plays, the mismatch is counted, the voice ends */
	h.frames = 0;
	p->stats.format_mismatch = 0;
	CHECK(stream_play_loop(p, STREAM_SLOT_AMBIENCE, LT_MISMATCH, 0, 1.0f, 0.0f, 0.0f) == 0);
	h_run(&h, 4);
	for (k = 0; k < 640; k++) CHECK(h.out[2 * k] == 1000);
	for (k = 640; k < 4000; k++) CHECK(h.out[2 * k] == 0);
	CHECK(p->stats.format_mismatch == 1 && !stream_slot_busy(p, STREAM_SLOT_AMBIENCE));
	h_free(&h);
	free(p);
	free_pack(&mp);
	return 0;
}

/* ---- I/O: latency (underruns), errors, abandoned reads, discipline ----------------------------------------------------------------- */
static int run_io(void)
{
	mempack mp;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	size_t k, nz;
	unsigned u0;
	CHECK(build_pack(&mp, k_streams, NST, k_loops, NLP) == 0);
	p->sec = &mp.sec;
	be_init(&be, mp.strd, mp.strd_size, &mp.sec, 0);
	h_init(&h, p, &be, 400000);

	/* a long stream (3000 blocks = 3.3 chunks): read in chunks of at most 32760 bytes, decoded exactly, no underrun with instant reads */
	CHECK(stream_play_sound(p, 0, T_LONG, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 125);
	{
		const sspec *lg = spec_of(T_LONG);
		CHECK(max_err(&h, 0, 192000, 0, exp_identity, (void *)lg) == 0);
	}
	CHECK(be.reads == 4 && be.total == 3000u * 36 && be.violations == 0 && p->stats.read_bytes == 3000u * 36);
	CHECK(p->stats.voice_underruns == 0 && p->stats.read_errors == 0 && !stream_slot_busy(p, 0) && p->stats.ticks == 125);
	CHECK(p->stats.start_latency_max <= 2);

	/* slow disc: every read takes 150 polls. The voice runs dry (counted), keeps its order, and the data that does come is intact */
	be.latency = 150;
	h.frames = 0;
	memset(h.out, 0, h.cap * 2 * sizeof(int16_t));
	be.reads = 0; be.total = 0;
	u0 = p->stats.voice_underruns;
	CHECK(stream_play_sound(p, 0, T_LONG, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 240);
	CHECK(p->stats.voice_underruns > u0 && p->stats.voice_underrun_frames > 0 && p->stats.read_wait_ticks > 50);
	{
		/* with gaps removed, the non-silent frames are the source in order */
		const sspec *lg = spec_of(T_LONG);
		uint32_t n = 0;
		for (k = 0; k < h.frames; k++) {
			if (h.out[2 * k] == 0 && sample_at(lg, 0, n) != 0) continue;
			CHECK(h.out[2 * k] == (int16_t)sample_at(lg, 0, n));
			n++;
			if (n == 192000) break;
		}
		CHECK(n > 100000);
		CHECK(be.violations == 0);
	}
	stream_kill(p, 0);
	be.latency = 0;
	be.countdown = 0;                                                                        /* let the read the killed voice left behind finish */
	h_run(&h, 2);
	CHECK(be.busy == 0 && p->io_voice == -1);

	/* a play on a busy slot while a read is in flight: the old read is abandoned and its buffer is not reused until it ends */
	be.latency = 3;
	h.frames = 0;
	CHECK(stream_play_sound(p, 1, T_LONG, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	h_run(&h, 1);
	CHECK(p->io_voice == 1);
	CHECK(stream_play_sound(p, 1, T_RAMP, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
	CHECK(p->stats.steals >= 1 && p->voice[1].chunk[p->io_chunk].state == 3);
	h.frames = 0;
	h_run(&h, 12);
	{
		const sspec *r = spec_of(T_RAMP);
		size_t start = 0;
		while (start < h.frames && h.out[2 * start] == 0 && start < 20000) start++;           /* sample 0 of a ramp is 0: the first non-zero is sample 1 */
		CHECK(start < h.frames && start < 20000);
		for (k = 0; k < 1000; k++) CHECK(h.out[2 * (start + k)] == (int16_t)ramp_at(0, (uint32_t)(1 + k)));
		(void)r;
	}
	CHECK(!stream_slot_busy(p, 1));
	CHECK(be.violations == 0);
	be.latency = 0;

	/* read errors: one failed chunk is skipped (counted), four in a row end the voice; a failing start ends it at once */
	{
		mempack m2;
		stream_player *q = NEW_PLAYER();
		membe b2;
		harness h2;
		CHECK(build_pack(&m2, k_streams, NST, k_loops, NLP) == 0);
		q->sec = &m2.sec;
		be_init(&b2, m2.strd, m2.strd_size, &m2.sec, 0);
		h_init(&h2, q, &b2, 400000);
		b2.fail_every = 2;                                                                   /* every second read fails */
		CHECK(stream_play_sound(q, 0, T_LONG, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
		h_run(&h2, 125);
		CHECK(q->stats.read_errors >= 1 && q->stats.reads == 4 && !stream_slot_busy(q, 0));
		b2.fail_every = 1;                                                                   /* all fail */
		b2.reads = 0;
		CHECK(stream_play_sound(q, 0, T_LONG, 0, STREAM_PLAY_LOOP, 1.0f, 0.0f, -1.0f) >= 0);
		h_run(&h2, 10);
		CHECK(!stream_slot_busy(q, 0));
		b2.fail_every = 0;
		b2.reads = 0;
		b2.fail_start_at = 1;
		h2.frames = 0;
		CHECK(stream_play_sound(q, 2, T_RAMP, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
		CHECK(!stream_slot_busy(q, 2));                                                      /* start() failed inside the play call */
		b2.fail_start_at = 0;
		CHECK(stream_play_sound(q, 2, T_RAMP, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
		h_run(&h2, 3);
		CHECK(max_err(&h2, 0, 2560, 0, exp_identity, (void *)spec_of(T_RAMP)) == 0);          /* the player recovered */
		h_free(&h2);
		free(q);
		free_pack(&m2);
	}

	/* the ring: partial consumption, wrap-around, queued frames limit what is mixed, idle tail */
	{
		unsigned n, run, total = 0;
		const int16_t *src;
		h.frames = 0;
		stream_set_target(p, STREAM_DEFAULT_TARGET);
		stream_kill(p, 0); stream_kill(p, 1); stream_kill(p, 2);
		be.fail_every = 0;
		be.reads = 0;
		CHECK(stream_play_sound(p, 0, T_LONG, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
		stream_tick(p);
		n = stream_pump(p, 0);
		CHECK(n == 3072 && stream_ring_count(p) == 3072);
		n = stream_pump(p, 0);
		CHECK(n == p->target_frames - 3072 && stream_ring_count(p) == p->target_frames);          /* at the target */
		CHECK(stream_pump(p, 0) == 0 && stream_pump(p, 100) == 0);
		stream_ring_consume(p, 3000);
		CHECK(stream_pump(p, 1000) == p->target_frames - 1800 - 1000 && stream_ring_count(p) == p->target_frames - 1000);
		stream_set_target(p, 100000);
		CHECK(p->target_frames == STREAM_MIX_RING);
		stream_set_target(p, 3);
		CHECK(p->target_frames == 256);
		stream_set_target(p, STREAM_DEFAULT_TARGET);
		while ((src = stream_ring_peek(p, &run)), run) {                                      /* drain in odd pieces across the wrap */
			unsigned take = run > 777 ? 777 : run;
			(void)src;
			stream_ring_consume(p, take);
			total += take;
			if (total > 20000) break;
		}
		CHECK(stream_ring_count(p) == 0);
		stream_ring_consume(p, 50);                                                           /* over-consume is harmless */
		CHECK(stream_ring_count(p) == 0 && stream_ring_peek(p, &run) != NULL && run == 0);
		stream_kill(p, 0);
		for (k = 0; k < 40; k++) { stream_tick(p); stream_pump(p, 0); stream_ring_consume(p, 1600); }
		nz = stream_pump(p, 0);
		CHECK(nz == 0);                                                                       /* nothing has played for 0.5 s: the pump stops */
		CHECK(stream_play_sound(p, 0, T_DC, 0, 0, 1.0f, 0.0f, -1.0f) >= 0);
		stream_tick(p);
		CHECK(stream_pump(p, 0) > 0);                                                         /* and starts again with the next sound */
	}
	h_free(&h);
	free(p);
	free_pack(&mp);
	return 0;
}

/* ---- the fixture written by the converter ------------------------------------------------------------------------------------------ */
/* Reference render of a decoded stream at the player's fixed-point step with doubles for the interpolation. */
static void ref_render(const int16_t *pcm, uint32_t frames, unsigned ch, uint32_t rate, uint32_t n_out, int16_t *out, unsigned och, double gain)
{
	uint32_t step = (uint32_t)(((uint64_t)rate << 16) / STREAM_MIX_RATE), j;
	uint64_t pos = 0;
	for (j = 0; j < n_out; j++) {
		uint32_t i0 = (uint32_t)(pos >> 16);
		double frac = (double)(pos & 0xFFFF) / 65536.0;
		double v;
		unsigned c = och < ch ? och : 0;
		if (i0 >= frames) { out[j] = 0; pos += step; continue; }
		{
			double a = pcm[i0 * ch + c], b = i0 + 1 < frames ? pcm[(i0 + 1) * ch + c] : a;
			v = a + (b - a) * frac;
		}
		out[j] = (int16_t)floor(v * gain + 0.5);
		pos += step;
	}
}

static int freq_of(const harness *h, size_t from, size_t n)
{
	size_t k;
	int crossings = 0;
	for (k = from + 1; k < from + n && k < h->frames; k++) if (h->out[2 * (k - 1)] < 0 && h->out[2 * k] >= 0) crossings++;
	return (int)((double)crossings * (double)STREAM_MIX_RATE / (double)n + 0.5);
}

static int run_fixture(const char *path)
{
	testpack tp;
	strm_section sec;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	uint8_t *img;
	int16_t *pcm, *ref;
	uint32_t i;
	size_t n;
	CHECK(testpack_open(&tp, path) == 0);
	CHECK(strm_open(&sec, tp.strm, tp.strm_size, tp.strd_size) == STRM_OK);
	img = malloc(tp.file_size);
	CHECK(img);
	fseek(tp.f, 0, SEEK_SET);
	CHECK(fread(img, 1, tp.file_size, tp.f) == tp.file_size);
	p->sec = &sec;
	be_init(&be, img, tp.file_size, &sec, tp.strd_offset);
	h_init(&h, p, &be, 400000);

	/* every stream, played alone from its slot, matches the reference render of the decoded stream */
	for (i = 0; i < sec.stream_count; i++) {
		strm_stream t = strm_get_stream(&sec, i);
		uint32_t unit = strm_unit_bytes(t.channels), nb = t.data_size / unit, nout = (uint32_t)((uint64_t)t.frames * STREAM_MIX_RATE / t.rate);
		int slot = (int)(i % 3), e0 = 0, e1 = 0;
		uint32_t j;
		pcm = malloc((size_t)t.frames * t.channels * sizeof(int16_t));
		ref = malloc(((size_t)nout + 8) * sizeof(int16_t));
		CHECK(pcm && ref);
		strm_decode_blocks(img + tp.strd_offset + t.data_offset, t.channels, nb, pcm);
		h.frames = 0;
		CHECK(stream_play_stream(p, slot, i, 0, 1.0f, 0.0f, -1.0f) == (int)i);
		h_run(&h, nout / 1600 + 6);
		CHECK(!stream_slot_busy(p, slot));
		for (j = 0; j < 2; j++) {
			unsigned c;
			ref_render(pcm, t.frames, t.channels, t.rate, nout, ref, j, t.gain > 0.0f ? (double)t.gain : 1.0);
			for (c = 0; c < nout - 2; c++) {
				int e = abs_i((int)h.out[2 * c + j] - (int)ref[c]);
				if (j == 0 && e > e0) e0 = e;
				if (j == 1 && e > e1) e1 = e;
			}
		}
		if (e0 > 3 || e1 > 3) { fprintf(stderr, "stream %u: error %d/%d against the reference render\n", i, e0, e1); return 1; }
		for (n = nout + 40; n < h.frames; n++) CHECK(h.out[2 * n] == 0 && h.out[2 * n + 1] == 0);
		free(pcm);
		free(ref);
	}
	CHECK(p->stats.voice_underruns == 0 && be.violations == 0 && p->stats.read_errors == 0);

	/* the music loop of the fixture: intro (400 Hz) -> loop (1000 Hz) -> alternate (2000 Hz) -> fade out -> end (3000 Hz) */
	h.frames = 0;
	CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, W_LOOP_M1, 0, 1.0f, 0.0f, -1.0f) == 0);
	h_run(&h, 40);                                                                          /* 64000 frames = 1.33 s */
	CHECK(abs_i(freq_of(&h, 6000, 12000) - 400) < 25);                                      /* intro: 16000 frames at 32 kHz = 24000 output frames */
	CHECK(h.out[2 * 2000] != 0);
	{ int f = freq_of(&h, 30000, 24000); if (abs_i(f - 1003) >= 30) { fprintf(stderr, "loop freq %d (intro %d)\n", f, freq_of(&h, 6000, 12000)); return 1; } }
	{
		int peak = 0;
		size_t k;
		for (k = 12000; k < 24000; k++) if (abs_i(h.out[2 * k]) > peak) peak = abs_i(h.out[2 * k]);
		CHECK(abs_i(peak - 6750) < 300);                                                    /* track gain 0.75 x 9000, after the 0.2 s fade in */
		peak = 0;
		for (k = 0; k < 2400; k++) if (abs_i(h.out[2 * k]) > peak) peak = abs_i(h.out[2 * k]);
		CHECK(peak < 3600);                                                                 /* the first 50 ms are still fading in */
	}
	stream_loop_alternate(p, STREAM_SLOT_MUSIC, 1);
	h.frames = 0;
	h_run(&h, 125);                                                                         /* the switch waits for the end of the current pass (+ read-ahead) */
	{
		size_t k, a = 0;
		for (k = 4800; k + 4800 < h.frames; k += 2400) if (abs_i(freq_of(&h, k, 2400) - 2000) < 60) { a = k; break; }
		CHECK(a > 0 && abs_i(freq_of(&h, a + 4800, 24000) - 2000) < 40);
	}
	stream_stop(p, STREAM_SLOT_MUSIC, -1.0f);                                               /* fade out 2 s */
	h.frames = 0;
	h_run(&h, 100);
	{
		size_t k, peak_early = 0, e = 0;
		for (k = 0; k < 4800; k++) if ((size_t)abs_i(h.out[2 * k]) > peak_early) peak_early = (size_t)abs_i(h.out[2 * k]);
		CHECK(peak_early > 4000);                                                           /* still loud at the start of the fade */
		for (k = 0; k < h.frames; k++) if (h.out[2 * k]) e = k;
		CHECK(e > 90000 && e < 120000 + 14000);                                             /* the 2 s fade (96000 frames) then the 0.25 s end sound */
		CHECK(abs_i(freq_of(&h, e - 9000, 8000) - 3000) < 90);
	}
	CHECK(!stream_slot_busy(p, STREAM_SLOT_MUSIC));
	h_free(&h);
	free(p);
	free(img);
	testpack_close(&tp);
	return 0;
}

/* corrupt stream data and index bytes through the whole player: nothing may crash */
static int run_fuzz(const char *path)
{
	testpack tp;
	strm_section sec;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	uint8_t *img, *strm_copy;
	uint32_t seed = 7, i, t;
	CHECK(testpack_open(&tp, path) == 0);
	img = malloc(tp.file_size);
	strm_copy = malloc(tp.strm_size);
	CHECK(img && strm_copy);
	fseek(tp.f, 0, SEEK_SET);
	CHECK(fread(img, 1, tp.file_size, tp.f) == tp.file_size);
	for (t = 0; t < 12; t++) {
		/* noise in the stream data */
		for (i = 0; i < 400; i++) { seed = seed * 1664525u + 1013904223u; img[tp.strd_offset + (seed >> 8) % tp.strd_size] = (uint8_t)(seed >> 3); }
		CHECK(strm_open(&sec, tp.strm, tp.strm_size, tp.strd_size) == STRM_OK);
		p->sec = &sec;
		be_init(&be, img, tp.file_size, &sec, tp.strd_offset);
		h_init(&h, p, &be, 100000);
		CHECK(stream_play_loop(p, 1, W_LOOP_M1, 0, 1.0f, 0.0f, 0.0f) == 0);
		stream_play_sound(p, 0, W_LINE1, -1, 0, 1.0f, -0.3f, 0.0f);
		stream_play_sound(p, 2, W_AIR, 0, STREAM_PLAY_LOOP, 1.0f, 0.3f, 0.1f);
		h_run(&h, 30);
		stream_loop_alternate(p, 1, 1);
		h_run(&h, 20);
		stream_stop(p, 1, -1.0f);
		stream_set_gain(p, 0, 0.5f, 0.0f);
		h_run(&h, 100);
		h_free(&h);
	}
	/* byte flips of the index that survive strm_open */
	for (t = 0; t < 300; t++) {
		memcpy(strm_copy, tp.strm, tp.strm_size);
		seed = seed * 1664525u + 1013904223u;
		strm_copy[(seed >> 8) % tp.strm_size] ^= (uint8_t)(1u << (seed & 7));
		if (strm_open(&sec, strm_copy, tp.strm_size, tp.strd_size) != STRM_OK) continue;
		p->sec = &sec;
		be_init(&be, img, tp.file_size, &sec, tp.strd_offset);
		h_init(&h, p, &be, 30000);
		for (i = 0; i < sec.sound_count && i < 8; i++) {
			strm_sound d = strm_get_sound(&sec, i);
			stream_play_sound(p, (int)(i % 3), d.tag_id, -1, STREAM_PLAY_LOOP, 1.0f, 0.0f, 0.0f);
		}
		for (i = 0; i < sec.loop_count && i < 2; i++) stream_play_loop(p, 1, strm_get_loop(&sec, i).tag_id, 0, 1.0f, 0.0f, -1.0f);
		h_run(&h, 12);
		h_free(&h);
	}
	free(p);
	free(img);
	free(strm_copy);
	testpack_close(&tp);
	return 0;
}

/* ---- real packs ---------------------------------------------------------------------------------------------------------------------- */
static int run_real(const char *path)
{
	testpack tp;
	strm_section sec;
	stream_player *p = NEW_PLAYER();
	membe be;
	harness h;
	uint32_t i, music = 0, dialog = 0, amb = 0;
	int have_music = 0, have_dialog = 0, have_amb = 0;
	unsigned ticks = 0;
	clock_t t0;
	double secs;
	const stream_stats *st;
	CHECK(testpack_open(&tp, path) == 0);
	CHECK(strm_open(&sec, tp.strm, tp.strm_size, tp.strd_size) == STRM_OK);
	for (i = 0; i < sec.loop_count; i++) {
		strm_loop l = strm_get_loop(&sec, i);
		strm_track tr = strm_get_track(&sec, &l, 0);
		int si = strm_find(&sec, tr.loop);
		if (si >= 0 && !have_music && strm_get_sound(&sec, (uint32_t)si).kind == STRM_KIND_MUSIC) { music = l.tag_id; have_music = 1; }
	}
	for (i = 0; i < sec.sound_count && !(have_dialog && have_amb); i++) {
		strm_sound d = strm_get_sound(&sec, i);
		if (d.kind == STRM_KIND_DIALOGUE && d.klass == 19 && !have_dialog) { dialog = d.tag_id; have_dialog = 1; }
		if (d.kind == STRM_KIND_AMBIENCE && !have_amb) { amb = d.tag_id; have_amb = 1; }
	}
	p->sec = &sec;
	be_init(&be, NULL, 0, &sec, tp.strd_offset);
	be.file = tp.f;
	h_init(&h, p, &be, 200000);
	t0 = clock();
	if (have_music) CHECK(stream_play_loop(p, STREAM_SLOT_MUSIC, music, 0, 1.0f, 0.0f, -1.0f) == 0);
	if (have_dialog) CHECK(stream_play_sound(p, STREAM_SLOT_DIALOGUE, dialog, -1, 0, 1.0f, 0.0f, -1.0f) >= 0);
	if (have_amb) CHECK(stream_play_sound(p, STREAM_SLOT_AMBIENCE, amb, -1, STREAM_PLAY_LOOP, 0.5f, 0.0f, -1.0f) >= 0);
	for (i = 0; i < 20 * 30; i++) {                                                         /* 20 s of game time */
		h.frames = 0;
		h_run(&h, 1);
		ticks++;
	}
	secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
	st = stream_get_stats(p);
	printf("  %s: music %s, dialogue %s, ambience %s: %u ticks, %u reads, %.2f MiB read, %u frames decoded, %u mixed, underruns %u/%u, %.3f ms/tick on the host\n",
	       path, have_music ? "yes" : "no", have_dialog ? "yes" : "no", have_amb ? "yes" : "no", ticks, st->reads, (double)st->read_bytes / 1048576.0,
	       st->frames_decoded, st->frames_mixed, st->voice_underruns, st->out_underruns, secs * 1000.0 / ticks);
	CHECK(st->voice_underruns == 0 && be.violations == 0 && st->read_errors == 0);
	CHECK(!have_music || stream_slot_busy(p, STREAM_SLOT_MUSIC));
	h_free(&h);
	free(p);
	testpack_close(&tp);
	return 0;
}

int main(int argc, char **argv)
{
	int a;
	if (argc < 2) { fprintf(stderr, "usage: %s fixture.hp2 [real.strm.hp2 ...]\n", argv[0]); return 2; }
	CHECK(sizeof(stream_chunk) % 64 == 0);
	CHECK(run_basic() == 0);
	printf("  basic: one-shot, loop points, interpolation, stereo: ok\n");
	CHECK(run_mix() == 0);
	printf("  mix: sums, clamp, gain, pan, fades: ok\n");
	CHECK(run_loops() == 0);
	CHECK(run_once_layers() == 0);
	printf("  sound_looping: start, loop, alternate, stop, end, format mismatch: ok\n");
	printf("  not-a-loop tracks play once, voices layer, slot owners: ok\n");
	CHECK(run_io() == 0);
	printf("  io: chunking, slow disc, errors, abandoned reads, ring: ok\n");
	CHECK(run_fixture(argv[1]) == 0);
	printf("  fixture: every converted stream and the music track play as the reference says: ok\n");
	CHECK(run_fuzz(argv[1]) == 0);
	printf("  fuzz: noise in data and index: ok\n");
	for (a = 2; a < argc; a++) CHECK(run_real(argv[a]) == 0);
	printf("test_stream: ok\n");
	return 0;
}
