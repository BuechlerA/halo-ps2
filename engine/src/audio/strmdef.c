/* STRM reader and IMA block decoder (docs/29-strm-section.md). Pure C11, no heap, no double. strm_open validates every table once
 * (sizes, alignment, ranges, sort order, stream bounds, finite floats, string terminators), so the accessors can never read out of
 * bounds whatever the bytes. Records are decoded with byte loads: no alignment assumption. */
#include "audio/strmdef.h"
#include <string.h>

#define HEADER_SIZE 128u
#define SOUND_REC 64u
#define STREAM_REC 64u
#define LOOP_REC 64u
#define TRACK_REC 48u
#define STRM_VERSION 1u
#define MAX_RECORDS (1u << 20)
#define KNOWN_STREAM_FLAGS (STRM_F_LOOP | STRM_F_FOLDED | STRM_F_RESAMPLED | STRM_F_SHARED)
#define KNOWN_SOUND_FLAGS (STRM_S_LOOPING | STRM_S_EXTRA)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }
static int terminated(const uint8_t *p, unsigned n) { return p[n - 1] == 0; }

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

strm_status strm_open(strm_section *s, const uint8_t *data, size_t size, uint32_t strd_size)
{
	uint32_t ns, nv, nl, nt, o[4], hdr_data, align, i, k;
	uint32_t prev = 0;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return STRM_ERR_TRUNCATED;
	if (memcmp(data, "STRM", 4) != 0) return STRM_ERR_MAGIC;
	if (rd16(data + 4) != STRM_VERSION) return STRM_ERR_VERSION;
	if (rd32(data + 8) > size) return STRM_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return STRM_ERR_LAYOUT;
	ns = rd32(data + 0x0C);
	nv = rd32(data + 0x10);
	nl = rd32(data + 0x14);
	nt = rd32(data + 0x18);
	for (i = 0; i < 4; i++) o[i] = rd32(data + 0x1C + 4 * i);
	hdr_data = rd32(data + 0x2C);
	align = rd32(data + 0x30);
	if (ns > MAX_RECORDS || nv > MAX_RECORDS || nl > MAX_RECORDS || nt > MAX_RECORDS) return STRM_ERR_LAYOUT;
	if (hdr_data != strd_size || align < 16u || (align & (align - 1u)) || align > (1u << 20)) return STRM_ERR_LAYOUT;
	if (!table_ok(o[0], ns, SOUND_REC, size) || !table_ok(o[1], nv, STREAM_REC, size) || !table_ok(o[2], nl, LOOP_REC, size) ||
	    !table_ok(o[3], nt, TRACK_REC, size))
		return STRM_ERR_LAYOUT;

	for (i = 0; i < ns; i++) {
		const uint8_t *r = data + o[0] + (size_t)i * SOUND_REC;
		uint32_t tag = rd32(r), cnt = rd16(r + 8), first = rd32(r + 12);
		if (i && tag <= prev) return STRM_ERR_LAYOUT;                      /* strictly ascending: binary search */
		prev = tag;
		if (r[5] > STRM_KIND_AMBIENCE || (rd16(r + 6) & ~KNOWN_SOUND_FLAGS)) return STRM_ERR_LAYOUT;
		if (cnt == 0 || (uint64_t)first + cnt > nv) return STRM_ERR_LAYOUT;
		for (k = 16; k < 32; k += 4)
			if (!finite_at(r + k)) return STRM_ERR_LAYOUT;
		if (!terminated(r + 40, STRM_NAME_LEN)) return STRM_ERR_LAYOUT;
	}
	for (i = 0; i < nv; i++) {
		const uint8_t *r = data + o[1] + (size_t)i * STREAM_REC;
		uint32_t off = rd32(r), sz = rd32(r + 4), frames = rd32(r + 8), rate = rd32(r + 12), ls = rd32(r + 16), le = rd32(r + 20);
		uint32_t ch = r[32], flags = rd16(r + 34), unit;
		if (ch != 1u && ch != 2u) return STRM_ERR_LAYOUT;
		unit = strm_unit_bytes(ch);
		if (r[33] != STRM_FMT_XBOX_IMA || sz == 0 || sz % unit) return STRM_ERR_LAYOUT;
		if ((uint64_t)(sz / unit) * STRM_BLOCK_FRAMES != frames) return STRM_ERR_LAYOUT;
		if (off % align || off > strd_size || sz > strd_size - off) return STRM_ERR_LAYOUT;
		if (rate == 0 || rate > STRM_MAX_RATE || (flags & ~KNOWN_STREAM_FLAGS)) return STRM_ERR_LAYOUT;
		if (((flags & STRM_F_LOOP) != 0) != (le != 0)) return STRM_ERR_LAYOUT;
		if (le && !(ls < le && le <= frames)) return STRM_ERR_LAYOUT;
		if (!le && ls) return STRM_ERR_LAYOUT;
		if (!finite_at(r + 24) || !finite_at(r + 28)) return STRM_ERR_LAYOUT;
	}
	prev = 0;
	for (i = 0; i < nl; i++) {
		const uint8_t *r = data + o[2] + (size_t)i * LOOP_REC;
		if (i && rd32(r) <= prev) return STRM_ERR_LAYOUT;
		prev = rd32(r);
		if (!finite_at(r + 8) || !terminated(r + 32, STRM_LOOP_NAME_LEN)) return STRM_ERR_LAYOUT;
		if ((uint64_t)rd32(r + 16) + rd16(r + 12) > nt) return STRM_ERR_LAYOUT;
	}
	for (i = 0; i < nt; i++) {
		const uint8_t *r = data + o[3] + (size_t)i * TRACK_REC;
		for (k = 4; k < 16; k += 4)
			if (!finite_at(r + k)) return STRM_ERR_LAYOUT;
	}
	s->data = data;
	s->size = size;
	s->data_size = strd_size;
	s->data_align = align;
	s->sound_count = ns;
	s->stream_count = nv;
	s->loop_count = nl;
	s->track_count = nt;
	s->src_bytes = rd32(data + 0x34);
	s->passthrough_streams = rd32(data + 0x38);
	s->transcoded_streams = rd32(data + 0x3C);
	s->shared_streams = rd32(data + 0x40);
	s->dialogue_streams = rd32(data + 0x44);
	s->music_streams = rd32(data + 0x48);
	s->ambience_streams = rd32(data + 0x4C);
	s->sounds = data + o[0];
	s->streams = data + o[1];
	s->loops = data + o[2];
	s->tracks = data + o[3];
	return STRM_OK;
}

static void copy_name(char *dst, const uint8_t *src, unsigned n)
{
	memcpy(dst, src, n);
	dst[n - 1] = 0;
}

int strm_find(const strm_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->sound_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->sounds + (size_t)mid * SOUND_REC);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

strm_sound strm_get_sound(const strm_section *s, uint32_t index)
{
	strm_sound d;
	const uint8_t *r;
	memset(&d, 0, sizeof d);
	if (index >= s->sound_count) return d;
	r = s->sounds + (size_t)index * SOUND_REC;
	d.tag_id = rd32(r);
	d.klass = r[4];
	d.kind = r[5];
	d.flags = rd16(r + 6);
	d.stream_count = rd16(r + 8);
	d.stream_first = rd32(r + 12);
	d.min_dist = rdf(r + 16);
	d.max_dist = rdf(r + 20);
	d.skip_fraction = rdf(r + 24);
	d.gain_mod = rdf(r + 28);
	d.max_play_ms = rd32(r + 32);
	d.tag_flags = rd32(r + 36);
	copy_name(d.name, r + 40, STRM_NAME_LEN);
	return d;
}

strm_stream strm_get_stream(const strm_section *s, uint32_t index)
{
	strm_stream d;
	const uint8_t *r;
	memset(&d, 0, sizeof d);
	if (index >= s->stream_count) return d;
	r = s->streams + (size_t)index * STREAM_REC;
	d.data_offset = rd32(r);
	d.data_size = rd32(r + 4);
	d.frames = rd32(r + 8);
	d.rate = rd32(r + 12);
	d.loop_start = rd32(r + 16);
	d.loop_end = rd32(r + 20);
	d.gain = rdf(r + 24);
	d.skip = rdf(r + 28);
	d.channels = r[32];
	d.format = r[33];
	d.flags = rd16(r + 34);
	d.chunks = rd16(r + 36);
	d.range = rd16(r + 38);
	d.owner = rd32(r + 40);
	d.src_bytes = rd32(r + 44);
	d.snr10 = (int32_t)rd32(r + 48);
	return d;
}

int strm_find_loop(const strm_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->loop_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->loops + (size_t)mid * LOOP_REC);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

strm_loop strm_get_loop(const strm_section *s, uint32_t index)
{
	strm_loop d;
	const uint8_t *r;
	memset(&d, 0, sizeof d);
	if (index >= s->loop_count) return d;
	r = s->loops + (size_t)index * LOOP_REC;
	d.tag_id = rd32(r);
	d.flags = rd32(r + 4);
	d.param = rdf(r + 8);
	d.track_count = rd16(r + 12);
	d.track_first = rd32(r + 16);
	copy_name(d.name, r + 32, STRM_LOOP_NAME_LEN);
	return d;
}

strm_track strm_get_track(const strm_section *s, const strm_loop *lp, uint32_t k)
{
	strm_track d;
	const uint8_t *r;
	memset(&d, 0, sizeof d);
	if (k >= lp->track_count || (uint64_t)lp->track_first + k >= s->track_count) return d;
	r = s->tracks + (size_t)(lp->track_first + k) * TRACK_REC;
	d.flags = rd32(r);
	d.gain = rdf(r + 4);
	d.fade_in = rdf(r + 8);
	d.fade_out = rdf(r + 12);
	d.start = rd32(r + 16);
	d.loop = rd32(r + 20);
	d.end = rd32(r + 24);
	d.alt_loop = rd32(r + 28);
	d.alt_end = rd32(r + 32);
	return d;
}

/* ---- decoder ---------------------------------------------------------------------------------------------------------------- */
static const int16_t k_steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
	107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
	4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
	22385, 24623, 27086, 29794, 32767
};
static const int8_t k_adjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* Index (idx * 16 + nibble) -> sample delta, and -> next (index * 16): the tables the reference decoder builds. */
static int32_t s_diff[89 * 16];
static uint16_t s_next[89 * 16];
static int s_ready;

static void build_tables(void)
{
	int idx, nib;
	for (idx = 0; idx < 89; idx++) {
		int st = k_steps[idx];
		for (nib = 0; nib < 16; nib++) {
			int d = st >> 3, ni;
			if (nib & 1) d += st >> 2;
			if (nib & 2) d += st >> 1;
			if (nib & 4) d += st;
			s_diff[idx * 16 + nib] = (nib & 8) ? -d : d;
			ni = idx + k_adjust[nib & 7];
			ni = ni < 0 ? 0 : ni > 88 ? 88 : ni;
			s_next[idx * 16 + nib] = (uint16_t)(ni * 16);
		}
	}
	s_ready = 1;
}

/* One channel of one block: `body` = 32 bytes of nibbles (63 are used), writes samples 0..63 at out[0], out[stride], ... */
static void decode_channel(const uint8_t *hdr, const uint8_t *body, int16_t *out, unsigned stride)
{
	int32_t pred = (int16_t)rd16(hdr);
	unsigned idx = hdr[2], i;
	uint32_t s16;
	if (idx > 88u) idx = 88u;
	s16 = idx * 16u;
	out[0] = (int16_t)pred;
	out += stride;
	for (i = 0; i < 31; i++) {
		unsigned b = body[i], k;
		k = s16 + (b & 15u);
		pred += s_diff[k];
		pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
		s16 = s_next[k];
		*out = (int16_t)pred;
		out += stride;
		k = s16 + (b >> 4);
		pred += s_diff[k];
		pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
		s16 = s_next[k];
		*out = (int16_t)pred;
		out += stride;
	}
	{
		unsigned k = s16 + (body[31] & 15u);                   /* nibble 62 gives sample 63; nibble 63 is padding */
		pred += s_diff[k];
		pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
		*out = (int16_t)pred;
	}
}

void strm_decode_blocks(const uint8_t *src, unsigned channels, uint32_t nblocks, int16_t *out)
{
	uint32_t b;
	if (!s_ready) build_tables();
	if (channels == 1) {
		for (b = 0; b < nblocks; b++) {
			decode_channel(src, src + 4, out, 1);
			src += 36;
			out += STRM_BLOCK_FRAMES;
		}
		return;
	}
	for (b = 0; b < nblocks; b++) {
		uint8_t l[32], r[32];
		unsigned g;
		for (g = 0; g < 8; g++) {
			memcpy(l + g * 4, src + 8 + g * 8, 4);
			memcpy(r + g * 4, src + 12 + g * 8, 4);
		}
		decode_channel(src, l, out, 2);
		decode_channel(src + 4, r, out + 1, 2);
		src += 72;
		out += 2 * STRM_BLOCK_FRAMES;
	}
}
