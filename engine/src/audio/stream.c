/* Streamed audio player: pure part (voices, read-ahead planner, decoder driver, resampling mixer, fades, loops) and, at the end of the
 * file under `#ifdef _EE`, the EE glue (fio + audsrv). The pure part is compiled and tested on the host (engine/tests/test_stream.c);
 * it has no ps2sdk dependency, no heap, no double, and never blocks: file reads are started and polled through `stream_io`.
 * Design notes live in docs/29-strm-section.md. */
#include "audio/stream.h"
#include <stddef.h>
#include <string.h>

#define NONE_REF 0xFFFFFFFFu
#define Q23_ONE (1 << 23)
#define PCM_MASK (STREAM_PCM_FRAMES - 1u)
#define RING_MASK (STREAM_MIX_RING - 1u)
#define MIX_BLOCK 128u
#define PUMP_MAX_FRAMES 3072u            /* one stream_pump renders at most this many: never more than a voice's decoded ring (4096) can feed at 1:1 */
#define IDLE_TAIL_FRAMES 24000u          /* keep mixing silence this long after the last voice ended (0.5 s) */
#define DEFAULT_STOP_FADE 0.05f
#define MIN_FADE_FRAMES 64u
#define MAX_READ_ERRORS 4u
#define MAX_READS_PER_TICK 4             /* chunks one tick may complete when the backend answers at once (the console completes about one per tick) */

enum { PLAN_NONE = 0, PLAN_ONESHOT, PLAN_START, PLAN_LOOP, PLAN_END, PLAN_DONE };
enum { CS_FREE = 0, CS_FLIGHT = 1, CS_READY = 2, CS_ABANDONED = 3 };

/* ---- helpers --------------------------------------------------------------------------------------------------------------- */
static int32_t gain_q23(float g)
{
	if (!(g > 0.0f)) return 0;                       /* also NaN */
	if (g > 1.99f) g = 1.99f;
	return (int32_t)(g * (float)Q23_ONE);
}

static uint32_t seconds_to_frames(float s)
{
	if (!(s > 0.0f)) return 0;
	if (s > 600.0f) s = 600.0f;
	return (uint32_t)(s * (float)STREAM_MIX_RATE);
}

static void pan_gains(float pan, int32_t *l, int32_t *r)
{
	if (!(pan > -1.0f)) pan = pan != pan ? 0.0f : -1.0f;
	if (pan > 1.0f) pan = 1.0f;
	*l = (int32_t)((pan > 0.0f ? 1.0f - pan : 1.0f) * 32768.0f);
	*r = (int32_t)((pan < 0.0f ? 1.0f + pan : 1.0f) * 32768.0f);
}

static uint32_t rng_next(stream_player *p)
{
	uint32_t x = p->rng ? p->rng : 0x2545F491u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	p->rng = x;
	return x;
}

static int ref_valid(const stream_player *p, uint32_t ref) { return p->sec && ref != NONE_REF && ref < p->sec->stream_count; }

static uint32_t pcm_avail(const stream_voice *v) { return v->pcm_w - v->pcm_r; }

static uint32_t chunk_frames_left(const stream_chunk *c)
{
	uint32_t unit = strm_unit_bytes(c->channels ? c->channels : 1u);
	return (c->bytes - c->pos) / unit * STRM_BLOCK_FRAMES;
}

/* Decoded frames waiting plus everything read or being read and not yet decoded (before the limit/skip trimming). */
uint32_t stream_voice_buffered(const stream_player *p, int slot)
{
	const stream_voice *v;
	uint32_t n, i;
	if (slot < 0 || slot >= STREAM_VOICES) return 0;
	v = &p->voice[slot];
	n = pcm_avail(v);
	for (i = 0; i < STREAM_CHUNKS; i++)
		if (v->chunk[i].state == CS_READY || v->chunk[i].state == CS_FLIGHT) n += chunk_frames_left(&v->chunk[i]);
	return n;
}

/* ---- voice lifecycle ------------------------------------------------------------------------------------------------------ */
/* Drops everything buffered. An in-flight read keeps its slot until the read ends (the I/O layer owns the buffer meanwhile). */
static void voice_flush(stream_voice *v)
{
	unsigned i;
	for (i = 0; i < STREAM_CHUNKS; i++) {
		if (v->chunk[i].state == CS_FLIGHT) v->chunk[i].state = CS_ABANDONED;
		else if (v->chunk[i].state == CS_READY) v->chunk[i].state = CS_FREE;
	}
	v->seq_dec = v->seq_plan;
	v->pcm_r = v->pcm_w = 0;
	v->phase = 0;
	v->seg_open = 0;
	v->starving = 0;
	v->primed = 0;
	v->age_ticks = 0;
}

static void voice_reset(stream_voice *v)
{
	voice_flush(v);
	memset(v, 0, offsetof(stream_voice, chunk));
	v->ref_start = v->ref_loop = v->ref_alt = v->ref_end = v->cur_ref = NONE_REF;
}

static void voice_finish(stream_player *p, stream_voice *v)
{
	voice_reset(v);
	p->stats.finished++;
}

/* ---- planner: which bytes to read next ------------------------------------------------------------------------------------ */
/* Opens a pass over stream `ref`: the first pass of a segment starts at frame 0, later passes of a loop at loop_start; a looping stream's
 * pass ends at loop_end. Returns 0 (and counts it) when the stream's format differs from what the voice already plays. */
static int seg_open_ref(stream_player *p, stream_voice *v, uint32_t ref, int first_pass)
{
	strm_stream st;
	uint32_t start = 0, end;
	if (!ref_valid(p, ref)) return 0;
	st = strm_get_stream(p->sec, ref);
	if (!st.channels) return 0;
	if (v->has_format && (st.channels != v->channels || st.rate != v->rate)) {
		p->stats.format_mismatch++;
		return 0;
	}
	if (!v->has_format) {
		v->channels = st.channels;
		v->rate = st.rate;
		v->step = (uint32_t)(((uint64_t)st.rate << 16) / STREAM_MIX_RATE);
		v->has_format = 1;
	}
	end = st.frames;
	if ((st.flags & STRM_F_LOOP) && !first_pass) {
		start = st.loop_start;
		end = st.loop_end;
	}
	v->cur_ref = ref;
	v->seg_block = start / STRM_BLOCK_FRAMES;
	v->seg_skip = start % STRM_BLOCK_FRAMES;
	v->seg_end_block = (end + STRM_BLOCK_FRAMES - 1u) / STRM_BLOCK_FRAMES;
	v->seg_frames_left = end - start;
	v->seg_first = 1;
	v->seg_open = 1;
	return 1;
}

/* The current segment has been completely planned: move on to the next one (or to PLAN_DONE). */
static void plan_advance(stream_player *p, stream_voice *v)
{
	v->seg_open = 0;
	switch (v->plan) {
	case PLAN_START:
		if (ref_valid(p, v->ref_loop) && seg_open_ref(p, v, v->ref_loop, 1)) { v->plan = PLAN_LOOP; return; }
		break;
	case PLAN_LOOP: {
		uint32_t ref;
		if (v->once) break;                                           /* "not a loop": the pass just planned was the only one */
		ref = (v->alt_on && ref_valid(p, v->ref_alt)) ? v->ref_alt : v->ref_loop;
		if (seg_open_ref(p, v, ref, ref != v->cur_ref)) { p->stats.loop_passes++; return; }
		break;
	}
	default:
		break;
	}
	v->plan = PLAN_DONE;
}

/* Describes the next chunk to read and commits the plan past it. Returns 0 when the plan is exhausted. */
static int plan_next_chunk(stream_player *p, stream_voice *v, uint32_t *offset, uint32_t *bytes, uint32_t *skip, uint32_t *limit, uint32_t *channels)
{
	strm_stream st;
	uint32_t unit, blocks, left, per_chunk;
	uint64_t off;
	if (!v->seg_open) return 0;
	st = strm_get_stream(p->sec, v->cur_ref);
	unit = strm_unit_bytes(st.channels);
	per_chunk = STREAM_CHUNK_BYTES / unit;
	left = v->seg_end_block - v->seg_block;
	blocks = left < per_chunk ? left : per_chunk;
	off = (uint64_t)p->strd_base + st.data_offset + (uint64_t)v->seg_block * unit;
	if (blocks == 0 || off > 0xFFFFFFFFu) {                       /* cannot happen for a validated stream; give up cleanly */
		v->plan = PLAN_DONE;
		v->seg_open = 0;
		return 0;
	}
	*offset = (uint32_t)off;
	*bytes = blocks * unit;
	*skip = v->seg_first ? v->seg_skip : 0;
	{
		uint32_t avail = blocks * STRM_BLOCK_FRAMES - *skip;
		*limit = v->seg_frames_left < avail ? v->seg_frames_left : avail;
		v->seg_frames_left -= *limit;
	}
	*channels = st.channels;
	v->seg_first = 0;
	v->seg_block += blocks;
	if (v->seg_block >= v->seg_end_block || v->seg_frames_left == 0) plan_advance(p, v);
	return 1;
}

/* ---- I/O scheduling ------------------------------------------------------------------------------------------------------- */
static int free_chunk(const stream_voice *v)
{
	unsigned i;
	for (i = 0; i < STREAM_CHUNKS; i++)
		if (v->chunk[i].state == CS_FREE) return (int)i;
	return -1;
}

static int voice_wants_data(const stream_voice *v)
{
	return v->active && v->seg_open && v->errors < MAX_READ_ERRORS && free_chunk(v) >= 0;
}

static void schedule_reads(stream_player *p)
{
	int best = -1, vi, ci;
	uint64_t best_ms = ~(uint64_t)0;
	stream_voice *v;
	stream_chunk *c;
	uint32_t offset, bytes, skip, limit, channels;
	if (p->io_voice >= 0 || !p->io.start) return;
	for (vi = 0; vi < STREAM_VOICES; vi++) {
		uint64_t ms;
		v = &p->voice[vi];
		if (!voice_wants_data(v)) continue;
		ms = (uint64_t)stream_voice_buffered(p, vi) * 1000u / (v->rate ? v->rate : 1u);
		if (ms < best_ms) { best_ms = ms; best = vi; }
	}
	if (best < 0) return;
	v = &p->voice[best];
	ci = free_chunk(v);
	c = &v->chunk[ci];
	if (!plan_next_chunk(p, v, &offset, &bytes, &skip, &limit, &channels)) return;
	c->bytes = bytes;
	c->pos = 0;
	c->skip = skip;
	c->limit = limit;
	c->channels = channels;
	c->seq = v->seq_plan++;
	c->state = CS_FLIGHT;
	if (p->io.start(p->io.user, offset, c->data, bytes) != 0) {
		c->state = CS_FREE;                                       /* the I/O layer is broken: give this voice up */
		p->stats.read_errors++;
		voice_finish(p, v);
		return;
	}
	p->io_voice = best;
	p->io_chunk = ci;
	p->stats.reads++;
	p->stats.read_bytes += bytes;
}

/* Returns 1 if a read finished (ok or failed) and was processed. */
static int poll_complete(stream_player *p)
{
	int r;
	stream_voice *v;
	stream_chunk *c;
	if (p->io_voice < 0) return 0;
	r = p->io.poll ? p->io.poll(p->io.user) : -1;
	if (r == 0) return 0;
	v = &p->voice[p->io_voice];
	c = &v->chunk[p->io_chunk];
	p->io_voice = p->io_chunk = -1;
	if (c->state == CS_ABANDONED) {
		c->state = CS_FREE;
		return 1;
	}
	if (r > 0) {
		c->state = CS_READY;
		v->errors = 0;
	} else {
		p->stats.read_errors++;
		v->errors++;
		c->bytes = c->pos = 0;                                    /* a lost chunk: the decoder skips it */
		c->state = CS_READY;
		if (v->errors >= MAX_READ_ERRORS) voice_finish(p, v);
	}
	return 1;
}

/* ---- decoding ------------------------------------------------------------------------------------------------------------- */
static stream_chunk *decode_chunk(stream_voice *v)
{
	unsigned i;
	for (i = 0; i < STREAM_CHUNKS; i++)
		if (v->chunk[i].state == CS_READY && v->chunk[i].seq == v->seq_dec) return &v->chunk[i];
	return NULL;
}

static void voice_decode(stream_player *p, stream_voice *v)
{
	int16_t tmp[STRM_BLOCK_FRAMES * 2];
	stream_chunk *c;
	while (pcm_avail(v) + STRM_BLOCK_FRAMES <= STREAM_PCM_FRAMES && (c = decode_chunk(v)) != NULL) {
		uint32_t unit = strm_unit_bytes(c->channels), first, count, ch = c->channels, i;
		if (c->pos >= c->bytes || c->limit == 0) {                 /* done with this chunk (or a lost one) */
			c->state = CS_FREE;
			v->seq_dec++;
			continue;
		}
		strm_decode_blocks(c->data + c->pos, ch, 1, tmp);
		first = c->skip;
		c->skip = 0;
		count = STRM_BLOCK_FRAMES - first;
		if (count > c->limit) count = c->limit;
		c->limit -= count;
		c->pos += unit;
		for (i = 0; i < count; i++) {
			uint32_t w = (v->pcm_w + i) & PCM_MASK;
			if (ch == 1) {
				v->pcm[w] = tmp[first + i];
			} else {
				v->pcm[2 * w] = tmp[2 * (first + i)];
				v->pcm[2 * w + 1] = tmp[2 * (first + i) + 1];
			}
		}
		v->pcm_w += count;
		p->stats.frames_decoded += count;
		if (c->pos >= c->bytes || c->limit == 0) {
			c->state = CS_FREE;
			v->seq_dec++;
		}
	}
}

static void decode_all(stream_player *p)
{
	int vi;
	for (vi = 0; vi < STREAM_VOICES; vi++)
		if (p->voice[vi].active && p->voice[vi].has_format) voice_decode(p, &p->voice[vi]);
}

/* True when nothing more will ever be decoded for the voice: plan exhausted and every chunk consumed. */
static int source_done(const stream_voice *v)
{
	unsigned i;
	if (v->seg_open || v->plan != PLAN_DONE) return 0;
	for (i = 0; i < STREAM_CHUNKS; i++)
		if (v->chunk[i].state == CS_READY || v->chunk[i].state == CS_FLIGHT) return 0;
	return 1;
}

static void voice_check_primed(stream_player *p, stream_voice *v)
{
	if (v->primed) return;
	if (pcm_avail(v) >= STREAM_PRIME_FRAMES || (source_done(v) && pcm_avail(v) > 0)) {
		v->primed = 1;
		if (v->age_ticks > p->stats.start_latency_max) p->stats.start_latency_max = v->age_ticks;
	}
}

/* ---- tick ------------------------------------------------------------------------------------------------------------------ */
void stream_tick(stream_player *p)
{
	int vi, n;
	p->stats.ticks++;
	if (p->io_voice >= 0 && p->io.poll) {
		/* a read still running at the start of the tick means the disc is slower than the planner */
		if (!poll_complete(p)) p->stats.read_wait_ticks++;
	}
	decode_all(p);
	schedule_reads(p);
	for (n = 0; n < MAX_READS_PER_TICK - 1 && p->io_voice >= 0 && poll_complete(p); n++) {   /* fast backends (memory, host) finish inside the tick */
		decode_all(p);
		schedule_reads(p);
	}
	for (vi = 0; vi < STREAM_VOICES; vi++) {
		stream_voice *v = &p->voice[vi];
		if (!v->active) continue;
		v->age_ticks++;
		voice_check_primed(p, v);
		if (v->has_format && !v->seg_open && v->plan == PLAN_DONE && source_done(v) && pcm_avail(v) == 0) voice_finish(p, v);
	}
}

/* ---- mixing --------------------------------------------------------------------------------------------------------------- */
static void begin_end(stream_player *p, stream_voice *v);

static void end_of_fade(stream_player *p, stream_voice *v)
{
	if (v->plan != PLAN_END && ref_valid(p, v->ref_end)) {
		begin_end(p, v);
		return;
	}
	voice_finish(p, v);
}

static void voice_mix(stream_player *p, stream_voice *v, int32_t *acc, unsigned n)
{
	unsigned i, done;
	uint32_t ch = v->channels, phase, step = v->step, r;
	int32_t g = v->gain, tgt = v->target, gstep = v->gain_step, gl = 0, gr = 0;
	int ramping;
	if (!v->active || !v->has_format) return;
	voice_check_primed(p, v);
	if (!v->primed) return;
	done = source_done(v);
	phase = v->phase;
	r = v->pcm_r;
	ramping = g != tgt;
	if (!ramping) {
		int32_t gq = g >> 8;
		gl = (int32_t)(((int64_t)gq * v->pan_l) >> 15);
		gr = (int32_t)(((int64_t)gq * v->pan_r) >> 15);
	}
	/* Fast path: steady gain and enough decoded source for the whole block (the common case): no per-frame checks. Same arithmetic as below. */
	if (!ramping && (uint64_t)(v->pcm_w - r) >= (((uint64_t)phase + (uint64_t)step * (n - 1u)) >> 16) + 2u) {
		const int16_t *pcm = v->pcm;
		if (ch == 1) {
			for (i = 0; i < n; i++) {
				int32_t l0 = pcm[r & PCM_MASK], l1 = pcm[(r + 1u) & PCM_MASK];
				int32_t o = l0 + (((l1 - l0) * (int32_t)(phase >> 1)) >> 15);
				acc[2 * i] += (o * gl) >> 15;
				acc[2 * i + 1] += (o * gr) >> 15;
				phase += step;
				r += phase >> 16;
				phase &= 0xFFFFu;
			}
		} else {
			int32_t gc = gl > gr ? gl : gr;
			for (i = 0; i < n; i++) {
				uint32_t a = (r & PCM_MASK) * 2u, b = ((r + 1u) & PCM_MASK) * 2u;
				int32_t f = (int32_t)(phase >> 1);
				int32_t ol = pcm[a] + (((pcm[b] - pcm[a]) * f) >> 15);
				int32_t or_ = pcm[a + 1] + (((pcm[b + 1] - pcm[a + 1]) * f) >> 15);
				acc[2 * i] += (ol * gc) >> 15;
				acc[2 * i + 1] += (or_ * gc) >> 15;
				phase += step;
				r += phase >> 16;
				phase &= 0xFFFFu;
			}
		}
		p->stats.frames_mixed += n;
		v->starving = 0;
		v->phase = phase;
		v->pcm_r = r;
		if (v->stopping && v->gain == 0) end_of_fade(p, v);
		return;
	}
	for (i = 0; i < n; i++) {
		uint32_t avail = v->pcm_w - r, i0 = r & PCM_MASK, i1 = (r + 1u) & PCM_MASK, adv;
		int32_t l0, r0, l1, r1, ol, or_, f;
		if (avail < 2u && !(avail == 1u && done)) {
			if (avail == 0u && done) { v->phase = phase; v->pcm_r = r; voice_finish(p, v); return; }
			if (!v->starving) { v->starving = 1; p->stats.voice_underruns++; }
			p->stats.voice_underrun_frames += n - i;
			break;
		}
		v->starving = 0;
		if (ch == 1) {
			l0 = r0 = v->pcm[i0];
			l1 = r1 = avail >= 2u ? v->pcm[i1] : l0;
		} else {
			l0 = v->pcm[2 * i0]; r0 = v->pcm[2 * i0 + 1];
			l1 = avail >= 2u ? v->pcm[2 * i1] : l0;
			r1 = avail >= 2u ? v->pcm[2 * i1 + 1] : r0;
		}
		f = (int32_t)(phase >> 1);
		ol = l0 + (((l1 - l0) * f) >> 15);
		or_ = ch == 1 ? ol : r0 + (((r1 - r0) * f) >> 15);
		if (ramping) {
			int32_t gq;
			g += gstep;
			if (gstep > 0 ? g >= tgt : g <= tgt) { g = tgt; ramping = 0; }
			gq = g >> 8;
			gl = (int32_t)(((int64_t)gq * v->pan_l) >> 15);
			gr = (int32_t)(((int64_t)gq * v->pan_r) >> 15);
		}
		if (ch == 1) {
			acc[2 * i] += (ol * gl) >> 15;
			acc[2 * i + 1] += (or_ * gr) >> 15;
		} else {                                              /* stereo sources: no pan, gain on both sides */
			int32_t gc = gl > gr ? gl : gr;
			acc[2 * i] += (ol * gc) >> 15;
			acc[2 * i + 1] += (or_ * gc) >> 15;
		}
		phase += step;
		adv = phase >> 16;
		phase &= 0xFFFFu;
		r += adv;
		p->stats.frames_mixed++;
	}
	v->phase = phase;
	v->pcm_r = r;
	v->gain = g;
	if (v->stopping && v->gain == 0) end_of_fade(p, v);
}

static void flush_acc(stream_player *p, const int32_t *acc, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		int32_t l = acc[2 * i], r = acc[2 * i + 1];
		uint32_t w = (p->ring_w + i) & RING_MASK;
		p->ring[2 * w] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
		p->ring[2 * w + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
	p->ring_w += n;
}

unsigned stream_ring_count(const stream_player *p) { return p->ring_w - p->ring_r; }

unsigned stream_pump(stream_player *p, unsigned queued_frames)
{
	unsigned have = stream_ring_count(p), need, rendered = 0;
	if ((uint64_t)have + queued_frames >= p->target_frames) return 0;
	need = p->target_frames - have - queued_frames;
	if (need > STREAM_MIX_RING - have) need = STREAM_MIX_RING - have;
	if (need > PUMP_MAX_FRAMES) need = PUMP_MAX_FRAMES;
	while (rendered < need) {
		int32_t acc[MIX_BLOCK * 2];
		unsigned n = need - rendered < MIX_BLOCK ? need - rendered : MIX_BLOCK, any = 0, vi;
		for (vi = 0; vi < STREAM_VOICES; vi++) any |= p->voice[vi].active;
		if (!any && p->idle_frames >= IDLE_TAIL_FRAMES) break;
		memset(acc, 0, n * 2u * sizeof acc[0]);
		for (vi = 0; vi < STREAM_VOICES; vi++) voice_mix(p, &p->voice[vi], acc, n);
		flush_acc(p, acc, n);
		if (any) p->idle_frames = 0;
		else {
			p->idle_frames += n;
			p->stats.frames_silence += n;
		}
		rendered += n;
	}
	if (stream_ring_count(p) > p->stats.ring_high) p->stats.ring_high = stream_ring_count(p);
	return rendered;
}

const int16_t *stream_ring_peek(const stream_player *p, unsigned *frames)
{
	unsigned count = stream_ring_count(p), r = p->ring_r & RING_MASK, run = STREAM_MIX_RING - r;
	*frames = count < run ? count : run;
	return &p->ring[2 * r];
}

void stream_ring_consume(stream_player *p, unsigned frames)
{
	unsigned count = stream_ring_count(p);
	p->ring_r += frames < count ? frames : count;
}

unsigned stream_ring_read(stream_player *p, int16_t *dst, unsigned max)
{
	unsigned total = 0;
	while (total < max) {
		unsigned run;
		const int16_t *src = stream_ring_peek(p, &run);
		if (!run) break;
		if (run > max - total) run = max - total;
		memcpy(dst + 2 * total, src, run * 2u * sizeof(int16_t));
		stream_ring_consume(p, run);
		total += run;
	}
	return total;
}

void stream_set_target(stream_player *p, unsigned frames)
{
	if (frames < 256u) frames = 256u;
	if (frames > STREAM_MIX_RING) frames = STREAM_MIX_RING;
	p->target_frames = frames;
}

const stream_stats *stream_get_stats(const stream_player *p) { return &p->stats; }

/* ---- control API ---------------------------------------------------------------------------------------------------------- */
void stream_init(stream_player *p, const strm_section *sec, const stream_io *io, uint32_t strd_base)
{
	int i;
	memset(p, 0, sizeof *p);
	p->sec = sec;
	if (io) p->io = *io;
	p->strd_base = strd_base;
	p->rng = 0x1F123BB5u;
	p->io_voice = p->io_chunk = -1;
	p->target_frames = STREAM_DEFAULT_TARGET;
	p->idle_frames = IDLE_TAIL_FRAMES;
	for (i = 0; i < STREAM_VOICES; i++) voice_reset(&p->voice[i]);
}

static stream_voice *claim_slot(stream_player *p, int slot)
{
	stream_voice *v;
	if (!p->sec || slot < 0 || slot >= STREAM_VOICES) return NULL;
	v = &p->voice[slot];
	if (v->active) p->stats.steals++;
	voice_reset(v);
	return v;
}

static void voice_start(stream_player *p, stream_voice *v, float gain, float pan, float fade_in_s, float track_gain, uint32_t fade_out_frames)
{
	uint32_t frames = seconds_to_frames(fade_in_s);
	v->active = 1;
	v->base_gain = gain_q23(gain);
	v->track_gain = gain_q23(track_gain);
	v->target = (int32_t)(((int64_t)v->base_gain * v->track_gain) >> 23);
	pan_gains(pan, &v->pan_l, &v->pan_r);
	v->fade_out_frames = fade_out_frames;
	if (frames) {
		v->gain = 0;
		v->gain_step = v->target / (int32_t)frames;
		if (v->gain_step < 1) v->gain_step = 1;
	} else {
		v->gain = v->target;
	}
	p->stats.plays++;
	schedule_reads(p);
}

int stream_play_stream(stream_player *p, int slot, uint32_t index, uint32_t flags, float gain, float pan, float fade_in_s)
{
	stream_voice *v;
	strm_stream st;
	if (!p->sec || index >= p->sec->stream_count) return -1;
	v = claim_slot(p, slot);
	if (!v) return -1;
	st = strm_get_stream(p->sec, index);
	v->plan = (flags & STREAM_PLAY_LOOP) ? PLAN_LOOP : PLAN_ONESHOT;
	v->ref_loop = (flags & STREAM_PLAY_LOOP) ? index : NONE_REF;
	if (!seg_open_ref(p, v, index, 1)) { voice_reset(v); return -1; }
	voice_start(p, v, gain, pan, fade_in_s, st.gain > 0.0f ? st.gain : 1.0f, 0);
	v->owner = st.owner;
	return (int)index;
}

int stream_play_sound(stream_player *p, int slot, uint32_t tag_id, int variant, uint32_t flags, float gain, float pan, float fade_in_s)
{
	int si;
	strm_sound sd;
	if (!p->sec) return -1;
	si = strm_find(p->sec, tag_id);
	if (si < 0) return -1;
	sd = strm_get_sound(p->sec, (uint32_t)si);
	if (!sd.stream_count) return -1;
	if (variant < 0) variant = (int)(rng_next(p) % sd.stream_count);
	if ((uint32_t)variant >= sd.stream_count) variant = (int)sd.stream_count - 1;
	si = stream_play_stream(p, slot, sd.stream_first + (uint32_t)variant, flags, gain, pan, fade_in_s);
	if (si >= 0) p->voice[slot].owner = tag_id;                          /* a deduplicated stream names its first owner, the caller asked for this sound */
	return si;
}

static uint32_t first_stream_of(const stream_player *p, uint32_t tag)
{
	int si;
	if (tag == STRM_NO_TAG) return NONE_REF;
	si = strm_find(p->sec, tag);
	return si < 0 ? NONE_REF : strm_get_sound(p->sec, (uint32_t)si).stream_first;
}

int stream_play_loop(stream_player *p, int slot, uint32_t loop_tag, unsigned track, float gain, float pan, float fade_in_s)
{
	stream_voice *v;
	strm_loop lp;
	strm_track tr;
	int li;
	uint32_t ref_start, ref_loop, ref_alt, ref_end, first;
	if (!p->sec) return -1;
	li = strm_find_loop(p->sec, loop_tag);
	if (li < 0) return -1;
	lp = strm_get_loop(p->sec, (uint32_t)li);
	if (track >= lp.track_count) return -1;
	tr = strm_get_track(p->sec, &lp, track);
	ref_start = first_stream_of(p, tr.start);
	ref_loop = first_stream_of(p, tr.loop);
	ref_alt = first_stream_of(p, tr.alt_loop);
	ref_end = first_stream_of(p, tr.end);
	if (ref_loop == NONE_REF && ref_alt != NONE_REF) { ref_loop = ref_alt; ref_alt = NONE_REF; }
	if (ref_start == NONE_REF && ref_loop == NONE_REF) return -1;
	v = claim_slot(p, slot);
	if (!v) return -1;
	v->ref_start = ref_start;
	v->ref_loop = ref_loop;
	v->ref_alt = ref_alt;
	v->ref_end = ref_end;
	v->once = (lp.flags & STRM_LOOP_NOT_A_LOOP) != 0;
	v->plan = ref_start != NONE_REF ? PLAN_START : PLAN_LOOP;
	first = ref_start != NONE_REF ? ref_start : ref_loop;
	if (!seg_open_ref(p, v, first, 1)) { voice_reset(v); return -1; }
	voice_start(p, v, gain, pan, fade_in_s < 0.0f ? tr.fade_in : fade_in_s, tr.gain > 0.0f ? tr.gain : 1.0f, seconds_to_frames(tr.fade_out));
	v->owner = loop_tag;
	return 0;
}

void stream_loop_alternate(stream_player *p, int slot, int on)
{
	if (slot >= 0 && slot < STREAM_VOICES && p->voice[slot].active) p->voice[slot].alt_on = on != 0;
}

void stream_set_gain(stream_player *p, int slot, float gain, float ramp_s)
{
	stream_voice *v;
	uint32_t frames;
	if (slot < 0 || slot >= STREAM_VOICES) return;
	v = &p->voice[slot];
	if (!v->active || v->stopping) return;
	v->base_gain = gain_q23(gain);
	v->target = (int32_t)(((int64_t)v->base_gain * v->track_gain) >> 23);
	frames = seconds_to_frames(ramp_s);
	if (frames == 0) { v->gain = v->target; v->gain_step = 0; return; }
	v->gain_step = (v->target - v->gain) / (int32_t)frames;
	if (v->gain_step == 0) v->gain_step = v->target > v->gain ? 1 : -1;
}

void stream_stop(stream_player *p, int slot, float fade_out_s)
{
	stream_voice *v;
	uint32_t frames;
	if (slot < 0 || slot >= STREAM_VOICES) return;
	v = &p->voice[slot];
	if (!v->active || v->stopping) return;
	frames = fade_out_s < 0.0f ? (v->fade_out_frames ? v->fade_out_frames : seconds_to_frames(DEFAULT_STOP_FADE)) : seconds_to_frames(fade_out_s);
	if (frames < MIN_FADE_FRAMES) frames = MIN_FADE_FRAMES;
	v->stopping = 1;
	v->target = 0;
	v->gain_step = -(v->gain / (int32_t)frames);
	if (v->gain_step > -1) v->gain_step = -1;
	if (v->gain == 0) end_of_fade(p, v);
}

void stream_kill(stream_player *p, int slot)
{
	if (slot < 0 || slot >= STREAM_VOICES || !p->voice[slot].active) return;
	voice_reset(&p->voice[slot]);
}

int stream_slot_busy(const stream_player *p, int slot)
{
	return slot >= 0 && slot < STREAM_VOICES && p->voice[slot].active;
}

uint32_t stream_slot_owner(const stream_player *p, int slot)
{
	return slot >= 0 && slot < STREAM_VOICES && p->voice[slot].active ? p->voice[slot].owner : 0;
}

int stream_free_slot(const stream_player *p, int first, int count)
{
	int i;
	for (i = first; i < first + count && i < STREAM_VOICES; i++)
		if (i >= 0 && !p->voice[i].active) return i;
	return -1;
}

/* The loop faded out: drop what is buffered and play the end sound once at the track gain. */
static void begin_end(stream_player *p, stream_voice *v)
{
	voice_flush(v);
	v->stopping = 0;
	v->plan = PLAN_END;
	v->gain = v->target = (int32_t)(((int64_t)v->base_gain * v->track_gain) >> 23);
	v->gain_step = 0;
	if (!seg_open_ref(p, v, v->ref_end, 1)) { voice_finish(p, v); return; }
	schedule_reads(p);
}

/* ====================================================================================================================================
 * EE glue: fio (non-blocking reads) and audsrv. Compiled only for the console; the host tests build the pure part above.
 * ================================================================================================================================== */
#ifdef _EE
#define NEWLIB_PORT_AWARE
#include "platform/timer.h"
#include "stream/file.h"
#include "stream/hp2.h"
#include <audsrv.h>
#include <fileio.h>
#include <kernel.h>
#include <tamtypes.h>
#include <stdio.h>

/* File I/O. The stream reads run in a small IO thread that makes ordinary *blocking* fio calls (lseek + read); stream_io.start posts the request,
 * stream_io.poll looks at a flag. Why a thread and not fioSetBlockMode(FIO_NOWAIT)? That was tried first (scratch test, docs/29): it works
 * (a 32 KB read from host: takes the IOP about 1.6 ms while the EE keeps running), but fio has ONE RPC client and in NOWAIT mode every other
 * fio call (printf goes through fio too) runs while a read is in flight and steals its completion: measured, a status line printed during a
 * read made fioSync report "complete" with the 186 bytes of the line. Blocking calls from several threads are serialised by the library's
 * semaphore, so a game that prints, loads or touches the memory card while a stream read is running just waits for it (a few ms; on a disc
 * a seek) in that thread, and the stream is never corrupted. The IO thread runs at a higher priority than the game thread (which spins in
 * graph_wait_vsync and would starve it otherwise); it only runs for the microseconds between "RPC finished" and "back to sleep".
 * The read destination is written back from the data cache by fioRead and is not touched until the flag says done. */
static void io_thread(void *arg)
{
	stream_ee *e = (stream_ee *)arg;
	for (;;) {
		int r = -1;
		WaitSema(e->req_sema);
		if (e->quit) break;
		if (fioLseek(e->fd, (int)e->req_off, 0 /* SEEK_SET */) >= 0) r = fioRead(e->fd, e->req_dst, (int)e->req_size);
		e->req_result = r;
		e->req_done = 1;
	}
	e->io_exited = 1;
	ExitDeleteThread();
}

static int ee_start(void *user, uint32_t offset, void *dst, uint32_t bytes)
{
	stream_ee *e = (stream_ee *)user;
	uint32_t want = (bytes + 63u) & ~63u;                              /* whole 64-byte lines: the DMA never shares a cache line with other data */
	if (e->fd < 0 || e->read_pending || want > STREAM_CHUNK_BUFFER || e->io_tid < 0) return -1;
	e->req_off = offset;
	e->req_dst = dst;
	e->req_size = want;
	e->req_done = 0;
	e->want = bytes;
	e->read_pending = 1;
	SignalSema(e->req_sema);
	return 0;
}

static int ee_poll(void *user)
{
	stream_ee *e = (stream_ee *)user;
	if (!e->read_pending) return -1;
	if (!e->req_done) return 0;
	e->read_pending = 0;
	if (e->req_result < (int)e->want && e->log) printf("[stream] read failed: fio returned %d, wanted %u\n", e->req_result, (unsigned)e->want);
	return e->req_result >= (int)e->want ? 1 : -1;                    /* a read past the end of the file may be short by the rounding only */
}

void stream_ee_sync(stream_ee *e)
{
	if (!e) return;
	while (e->read_pending && !e->req_done) {}                         /* the IO thread (higher priority) runs meanwhile; only needed before closing */
}

static const char *resolve_path(const char *path, char *buf, size_t cap)
{
	if (strchr(path, ':')) return path;
	snprintf(buf, cap, "host:/%s", path);
	return buf;
}

int stream_ee_open(stream_ee *e, stream_player *p, arena_t *a, const char *path, int log)
{
	uint8_t hdr[64 + 32 * HP2_MAX_SECTIONS];
	hp2_pack pk;
	const hp2_section *sm, *sd;
	char rb[160];
	uint8_t *raw, *stored;
	long fsize = file_size_of(path);
	size_t mark = arena_mark(a), hdr_len;
	audsrv_fmt_t fmt;
	int free_bytes;

	memset(e, 0, sizeof *e);
	e->fd = -1;
	e->io_tid = -1;
	e->player = p;
	e->log = log;
	snprintf(e->path, sizeof e->path, "%s", path);
	stream_init(p, NULL, NULL, 0);                                   /* inert until everything below worked */
	if (fsize < 64) { printf("[stream] cannot open %s\n", path); return -1; }
	hdr_len = (size_t)fsize < sizeof hdr ? (size_t)fsize : sizeof hdr;
	if (file_read_at(path, 0, hdr, hdr_len) != 0) return -1;
	if (hp2_open_header(&pk, hdr, hdr_len, (size_t)fsize) != HP2_OK) { printf("[stream] %s: bad pack header\n", path); return -1; }
	sm = hp2_find(&pk, "STRM");
	sd = hp2_find(&pk, "STRD");
	if (!sm || !sd || sm->codec != HP2_CODEC_STORED || sd->codec != HP2_CODEC_STORED) { printf("[stream] %s: no stored STRM/STRD\n", path); return -1; }
	raw = arena_alloc(a, sm->raw_size, 64);
	stored = raw;                                                     /* stored section: read straight into the destination */
	if (!raw || file_read_at(path, (long)sm->offset, stored, sm->stored_size) != 0) { arena_release(a, mark); return -1; }
	if (hp2_crc32(raw, sm->raw_size) != sm->crc32) { printf("[stream] %s: STRM crc mismatch\n", path); arena_release(a, mark); return -1; }
	if (strm_open(&e->sec, raw, sm->raw_size, sd->raw_size) != STRM_OK) { printf("[stream] %s: STRM rejected\n", path); arena_release(a, mark); return -1; }
	e->strm = raw;
	e->strd_offset = sd->offset;

	/* audsrv: 48 kHz stereo 16-bit; the ring size is whatever the IOP gives us, measured while it is empty */
	fmt.freq = (int)STREAM_MIX_RATE;
	fmt.bits = 16;
	fmt.channels = 2;
	if (audsrv_set_format(&fmt) != AUDSRV_ERR_NOERROR) { printf("[stream] audsrv_set_format failed: %s\n", audsrv_get_error_string()); return -1; }
	free_bytes = audsrv_available();
	if (free_bytes <= 0) { printf("[stream] audsrv reports no ring space (%d)\n", free_bytes); return -1; }
	e->ring_capacity = free_bytes;
	e->audsrv_ok = 1;

	fioInit();
	e->fd = fioOpen(resolve_path(path, rb, sizeof rb), FIO_O_RDONLY);
	if (e->fd < 0) { printf("[stream] fioOpen %s failed (%d)\n", rb, e->fd); return -1; }
	{
		ee_sema_t sema;
		ee_thread_t th;
		ee_thread_status_t me;
		int self = GetThreadId();
		extern void *_gp;
		sema.init_count = 0;
		sema.max_count = 1;
		sema.option = 0;
		e->req_sema = CreateSema(&sema);
		if (e->req_sema < 0) { printf("[stream] CreateSema failed\n"); fioClose(e->fd); e->fd = -1; return -1; }
		if (ReferThreadStatus(self, &me) < 0) me.current_priority = 1;
		e->main_priority = e->main_priority_orig = (int)me.current_priority;
		if (e->main_priority < 2) { ChangeThreadPriority(self, 2); e->main_priority = 2; e->main_demoted = 1; }
		memset(&th, 0, sizeof th);
		th.func = io_thread;
		th.stack = e->io_stack;
		th.stack_size = sizeof e->io_stack;
		th.gp_reg = &_gp;
		th.initial_priority = e->main_priority - 1;
		e->io_tid = CreateThread(&th);
		if (e->io_tid < 0 || StartThread(e->io_tid, e) < 0) {
			printf("[stream] cannot start the IO thread (%d)\n", e->io_tid);
			DeleteSema(e->req_sema);
			fioClose(e->fd);
			e->fd = -1;
			e->io_tid = -1;
			return -1;
		}
	}
	{
		stream_io io;
		io.start = ee_start;
		io.poll = ee_poll;
		io.user = e;
		stream_init(p, &e->sec, &io, e->strd_offset);
	}
	/* The ring holds 53 ms of audio whatever the format (measured: 10240 bytes at 48 kHz, 6820 at 32 kHz, ...): keep it nearly full. A tick every
	 * 33 ms then leaves 20 ms of margin, a pump every vsync (stream_ee_pump) 36 ms. */
	{
		int ring_frames = e->ring_capacity / 4, target = ring_frames - 128;
		if (target > (int)STREAM_DEFAULT_TARGET) target = (int)STREAM_DEFAULT_TARGET;
		if (target < 800) target = 800;
		stream_set_target(p, (unsigned)target);
	}
	if (log)
		printf("[stream] %s: %u sounds, %u streams, STRD at %u (%u bytes), audsrv ring %d bytes, target %u frames\n", path,
		       (unsigned)e->sec.sound_count, (unsigned)e->sec.stream_count, (unsigned)e->strd_offset, (unsigned)e->sec.data_size,
		       e->ring_capacity, (unsigned)p->target_frames);
	return 0;
}

/* Mix up to the target and push what audsrv has room for. `with_tick`: also run the player's tick (I/O, decode, planner). */
static void ee_service(stream_ee *e, int with_tick)
{
	stream_player *p = e->player;
	u32 t0, t1, ta, aud = 0;
	int free_bytes, queued_frames;
	if (!e->audsrv_ok || !p->sec) return;
	t0 = timer_cycles();
	ta = t0;
	free_bytes = audsrv_available();                                  /* short RPC: reads the ring counters, never waits for space */
	aud += timer_cycles() - ta;
	if (free_bytes < 0) free_bytes = 0;
	queued_frames = (e->ring_capacity - free_bytes) / 4;
	if (queued_frames < 0) queued_frames = 0;
	if (p->stats.ticks == 0 || (uint32_t)queued_frames < p->stats.queued_min) p->stats.queued_min = (uint32_t)queued_frames;
	if ((uint32_t)queued_frames > p->stats.queued_max) p->stats.queued_max = (uint32_t)queued_frames;
	if (queued_frames == 0 && p->stats.pushed_frames > 0 && stream_ring_count(p) == 0 && p->idle_frames < IDLE_TAIL_FRAMES)
		p->stats.out_underruns++;                                     /* audsrv ran dry while we were still supposed to be feeding it */
	{
		u32 tk0 = timer_cycles(), tk1, tk2;
		if (with_tick) stream_tick(p);
		tk1 = timer_cycles();
		stream_pump(p, (unsigned)queued_frames);
		tk2 = timer_cycles();
		p->stats.ms_tick_sum += timer_ms(tk0, tk1);
		p->stats.ms_pump_sum += timer_ms(tk1, tk2);
	}
	/* push what the ring holds and audsrv has room for. audsrv_play_audio copies into the IOP ring and returns the bytes it took: measured on
	 * PCSX2 it returns 0 after 0.07 ms when the ring is full (it neither waits nor loops), so asking for no more than audsrv_available() is a courtesy,
	 * not a necessity; a short count just leaves the rest in our ring for the next call. audsrv_wait_audio (blocks on a semaphore until there
	 * is space) and the fill-buffer callback are never used. audsrv_available is one RPC round trip (0.05 ms on PCSX2). */
	for (;;) {
		unsigned n;
		const int16_t *src = stream_ring_peek(p, &n);
		int sent;
		if (n == 0 || free_bytes < 4) break;
		if ((int)(n * 4) > free_bytes) n = (unsigned)free_bytes / 4;
		ta = timer_cycles();
		sent = audsrv_play_audio((const char *)src, (int)(n * 4));
		aud += timer_cycles() - ta;
		p->stats.push_calls++;
		if (sent <= 0) break;
		if ((unsigned)sent < n * 4) p->stats.push_partial++;
		stream_ring_consume(p, (unsigned)sent / 4);
		p->stats.pushed_frames += (unsigned)sent / 4;
		if (e->measure) {
			unsigned q, nf = (unsigned)sent / 4;
			for (q = 0; q < nf; q++) {
				int ch;
				for (ch = 0; ch < 2; ch++) {
					int32_t v = src[2 * q + (unsigned)ch];
					int32_t a = v < 0 ? -v : v;
					if (a > e->peak[ch]) e->peak[ch] = a;
					e->sum[ch] += v;
					e->sumsq[ch] += (int64_t)v * v;
				}
			}
			if (e->capture) {
				uint32_t room = e->capture_cap - e->capture_len, take = nf < room ? nf : room;
				memcpy(e->capture + 2u * e->capture_len, src, take * 4u);
				e->capture_len += take;
			}
			e->measured_frames += nf;
		}
		free_bytes -= sent;
	}
	t1 = timer_cycles();
	p->stats.ms_aud_sum += timer_ms(0, aud);
	if (timer_ms(0, aud) > p->stats.ms_audsrv_max) p->stats.ms_audsrv_max = timer_ms(0, aud);
	p->stats.ms_last = timer_ms(t0, t1);
	p->stats.ms_sum += p->stats.ms_last;
	p->stats.service_calls++;
	if (p->stats.ms_last > p->stats.ms_max) p->stats.ms_max = p->stats.ms_last;
}

void stream_ee_tick(stream_ee *e) { ee_service(e, 1); }
void stream_ee_pump(stream_ee *e) { ee_service(e, 0); }

void stream_ee_close(stream_ee *e)
{
	if (!e) return;
	stream_ee_sync(e);
	if (e->io_tid >= 0) {
		e->quit = 1;
		SignalSema(e->req_sema);
		while (!e->io_exited) {}                                       /* the thread ends itself (ExitDeleteThread) */
		DeleteSema(e->req_sema);
		e->io_tid = -1;
	}
	if (e->fd >= 0) fioClose(e->fd);
	e->fd = -1;
	if (e->audsrv_ok) audsrv_stop_audio();
	e->audsrv_ok = 0;
	if (e->player) stream_init(e->player, NULL, NULL, 0);
}
#endif /* _EE */
