/* SND0 reader (docs/21-snd-section.md). Pure C11, no heap, no double. snd_open validates every table once (sizes,
 * alignment, ranges, sort order, cross indices, SNDD bounds, finite floats, string terminators), so the accessors can
 * never read out of bounds, whatever the bytes. Records are decoded with byte loads: no alignment assumption. */
#include "audio/snddef.h"
#include <string.h>

#define HEADER_SIZE 128u
#define SOUND_REC 128u
#define RANGE_REC 32u
#define PERM_REC 64u
#define LOOP_REC 64u
#define TRACK_REC 48u
#define DETAIL_REC 48u
#define SND_VERSION 1u
#define MAX_RECORDS (1u << 20)
#define MAX_RATE 48000u
#define KNOWN_SOUND_FLAGS (SND_F_LOOP | SND_F_DROPPED | SND_F_STREAMED | SND_F_DEGRADED | SND_F_UNSUPPORTED)
#define KNOWN_PERM_FLAGS (SND_P_LOOP | SND_P_FOLDED)

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
static int finite_run(const uint8_t *p, unsigned count)
{
	unsigned i;
	for (i = 0; i < count; i++)
		if (!finite_at(p + 4 * i)) return 0;
	return 1;
}

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

snd_status snd_open(snd_section *s, const uint8_t *data, size_t size, uint32_t sndd_size)
{
	uint32_t ns, nr, np, nl, nt, nd, o[6], hdr_sndd, i, k;
	uint32_t prev = 0;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return SND_ERR_TRUNCATED;
	if (memcmp(data, "SND0", 4) != 0) return SND_ERR_MAGIC;
	if (rd16(data + 4) != SND_VERSION) return SND_ERR_VERSION;
	if (rd32(data + 8) > size) return SND_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return SND_ERR_LAYOUT;
	ns = rd32(data + 0x0C);
	nr = rd32(data + 0x10);
	np = rd32(data + 0x14);
	nl = rd32(data + 0x18);
	nt = rd32(data + 0x1C);
	nd = rd32(data + 0x20);
	hdr_sndd = rd32(data + 0x24);
	for (i = 0; i < 6; i++) o[i] = rd32(data + 0x28 + 4 * i);
	if (ns > MAX_RECORDS || nr > MAX_RECORDS || np > MAX_RECORDS || nl > MAX_RECORDS || nt > MAX_RECORDS || nd > MAX_RECORDS)
		return SND_ERR_LAYOUT;
	if (hdr_sndd != sndd_size || sndd_size % 16u) return SND_ERR_LAYOUT;
	if (!table_ok(o[0], ns, SOUND_REC, size) || !table_ok(o[1], nr, RANGE_REC, size) || !table_ok(o[2], np, PERM_REC, size) ||
	    !table_ok(o[3], nl, LOOP_REC, size) || !table_ok(o[4], nt, TRACK_REC, size) || !table_ok(o[5], nd, DETAIL_REC, size))
		return SND_ERR_LAYOUT;

	for (i = 0; i < ns; i++) {
		const uint8_t *r = data + o[0] + (size_t)i * SOUND_REC;
		uint32_t tag = rd32(r), range_first = rd32(r + 12), perm_first = rd32(r + 20);
		uint32_t range_count = rd16(r + 16), perm_count = rd16(r + 18), run = 0;
		uint8_t flags = r[9];
		if (i && tag <= prev) return SND_ERR_LAYOUT;                     /* strictly ascending: binary search */
		prev = tag;
		if (flags & ~KNOWN_SOUND_FLAGS) return SND_ERR_LAYOUT;
		if (rd16(r + 10) > MAX_RATE) return SND_ERR_LAYOUT;
		if ((uint64_t)range_first + range_count > nr || (uint64_t)perm_first + perm_count > np) return SND_ERR_LAYOUT;
		if ((perm_count == 0) != (range_count == 0)) return SND_ERR_LAYOUT;   /* data and ranges come together */
		if (perm_count && (flags & (SND_F_DROPPED | SND_F_STREAMED | SND_F_UNSUPPORTED))) return SND_ERR_LAYOUT;
		if (!finite_run(r + 32, 15) || !terminated(r + 96, SND_NAME_LEN)) return SND_ERR_LAYOUT;   /* 9 + 6 floats */
		for (k = 0; k < range_count; k++) {                              /* ranges partition the permutations in order */
			const uint8_t *rg = data + o[1] + (size_t)(range_first + k) * RANGE_REC;
			if (rd16(rg + 12) != run) return SND_ERR_LAYOUT;
			run += rd16(rg + 14);
		}
		if (run != perm_count) return SND_ERR_LAYOUT;
		for (k = 0; k < range_count; k++) {
			const uint8_t *rg = data + o[1] + (size_t)(range_first + k) * RANGE_REC;
			uint32_t pf = rd16(rg + 12), pc = rd16(rg + 14), j;
			for (j = 0; j < pc; j++) {
				const uint8_t *p = data + o[2] + (size_t)(perm_first + pf + j) * PERM_REC;
				uint32_t off = rd32(p), sz = rd32(p + 4), frames = rd32(p + 8), rate = rd32(p + 12);
				uint32_t ls = rd32(p + 16), le = rd32(p + 20), nxt = rd16(p + 32), pfl = rd16(p + 34);
				uint32_t nblk;
				if (off % 16u || sz % 16u || sz < 48u || off > sndd_size || sz > sndd_size - off) return SND_ERR_LAYOUT;
				nblk = sz / 16u - 2u;                                         /* header and terminator are not blocks */
				if (frames > nblk * 28u || frames <= (nblk - 1u) * 28u) return SND_ERR_LAYOUT;
				if (rate == 0 || rate > MAX_RATE) return SND_ERR_LAYOUT;
				if (pfl & ~KNOWN_PERM_FLAGS) return SND_ERR_LAYOUT;
				if (pfl & SND_P_LOOP) {
					if (ls != 0 || le != nblk * 16u) return SND_ERR_LAYOUT;
				} else if (ls || le) {
					return SND_ERR_LAYOUT;
				}
				if (!finite_at(p + 24) || !finite_at(p + 28)) return SND_ERR_LAYOUT;
				if (rd16(p + 36) != k) return SND_ERR_LAYOUT;                       /* range index matches the run */
				if (nxt != SND_NO_PERM && nxt >= pc) return SND_ERR_LAYOUT;
				if (!terminated(p + 40, SND_SMALL_NAME_LEN)) return SND_ERR_LAYOUT;
			}
		}
	}
	for (i = 0; i < nr; i++) {
		const uint8_t *rg = data + o[1] + (size_t)i * RANGE_REC;
		if (!finite_run(rg, 3) || !terminated(rg + 16, SND_SMALL_NAME_LEN)) return SND_ERR_LAYOUT;
	}
	prev = 0;
	for (i = 0; i < nl; i++) {
		const uint8_t *r = data + o[3] + (size_t)i * LOOP_REC;
		if (i && rd32(r) <= prev) return SND_ERR_LAYOUT;
		prev = rd32(r);
		if (!finite_at(r + 8) || !terminated(r + 32, SND_NAME_LEN)) return SND_ERR_LAYOUT;
		if ((uint64_t)rd32(r + 16) + rd16(r + 12) > nt || (uint64_t)rd32(r + 20) + rd16(r + 14) > nd) return SND_ERR_LAYOUT;
	}
	for (i = 0; i < nt; i++)
		if (!finite_run(data + o[4] + (size_t)i * TRACK_REC + 4, 3)) return SND_ERR_LAYOUT;
	for (i = 0; i < nd; i++) {
		const uint8_t *r = data + o[5] + (size_t)i * DETAIL_REC;
		if (!finite_run(r + 4, 3) || !finite_run(r + 20, 6)) return SND_ERR_LAYOUT;
	}
	s->data = data;
	s->size = size;
	s->sndd_size = sndd_size;
	s->sound_count = ns;
	s->range_count = nr;
	s->perm_count = np;
	s->loop_count = nl;
	s->track_count = nt;
	s->detail_count = nd;
	s->budget = rd32(data + 0x40);
	s->dropped_sounds = rd32(data + 0x44);
	s->dropped_perms = rd32(data + 0x48);
	s->degraded_sounds = rd32(data + 0x4C);
	s->streamed_sounds = rd32(data + 0x50);
	s->sounds = data + o[0];
	s->ranges = data + o[1];
	s->perms = data + o[2];
	s->loops = data + o[3];
	s->tracks = data + o[4];
	s->details = data + o[5];
	return SND_OK;
}

static void copy_name(char *dst, const uint8_t *src, unsigned n)
{
	memcpy(dst, src, n);
	dst[n - 1] = 0;
}

int snd_find(const snd_section *s, uint32_t tag_id)
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

snd_sound snd_get_sound(const snd_section *s, uint32_t index)
{
	snd_sound o;
	unsigned k;
	memset(&o, 0, sizeof o);
	o.promotion_tag = SND_NO_TAG;
	if (index >= s->sound_count) return o;
	{
		const uint8_t *r = s->sounds + (size_t)index * SOUND_REC;
		o.tag_id = rd32(r);
		o.tag_flags = rd32(r + 4);
		o.klass = r[8];
		o.flags = r[9];
		o.rate = rd16(r + 10);
		o.range_first = rd32(r + 12);
		o.range_count = rd16(r + 16);
		o.perm_count = rd16(r + 18);
		o.perm_first = rd32(r + 20);
		o.perms_dropped = rd16(r + 24);
		o.promotion_count = rd16(r + 26);
		o.promotion_tag = rd32(r + 28);
		o.min_dist = rdf(r + 32);
		o.max_dist = rdf(r + 36);
		o.skip_fraction = rdf(r + 40);
		o.pitch_lo = rdf(r + 44);
		o.pitch_hi = rdf(r + 48);
		o.inner_cone = rdf(r + 52);
		o.outer_cone = rdf(r + 56);
		o.outer_gain = rdf(r + 60);
		o.gain_mod = rdf(r + 64);
		for (k = 0; k < 6; k++) o.scale_mod[k] = rdf(r + 68 + 4 * k);
		o.max_play_ms = rd32(r + 92);
		copy_name(o.name, r + 96, SND_NAME_LEN);
	}
	return o;
}

snd_range snd_get_range(const snd_section *s, const snd_sound *snd, uint32_t k)
{
	snd_range o;
	memset(&o, 0, sizeof o);
	if (k >= snd->range_count || (uint64_t)snd->range_first + k >= s->range_count) return o;
	{
		const uint8_t *r = s->ranges + (size_t)(snd->range_first + k) * RANGE_REC;
		o.natural_pitch = rdf(r);
		o.bend_lo = rdf(r + 4);
		o.bend_hi = rdf(r + 8);
		o.perm_first = rd16(r + 12);
		o.perm_count = rd16(r + 14);
		copy_name(o.name, r + 16, SND_SMALL_NAME_LEN);
	}
	return o;
}

snd_perm snd_get_perm(const snd_section *s, const snd_sound *snd, uint32_t k)
{
	snd_perm o;
	memset(&o, 0, sizeof o);
	o.next = SND_NO_PERM;
	if (k >= snd->perm_count || (uint64_t)snd->perm_first + k >= s->perm_count) return o;
	{
		const uint8_t *r = s->perms + (size_t)(snd->perm_first + k) * PERM_REC;
		o.sample_offset = rd32(r);
		o.size = rd32(r + 4);
		o.frames = rd32(r + 8);
		o.rate = rd32(r + 12);
		o.loop_start = rd32(r + 16);
		o.loop_end = rd32(r + 20);
		o.gain = rdf(r + 24);
		o.skip = rdf(r + 28);
		o.next = rd16(r + 32);
		o.flags = rd16(r + 34);
		o.range = rd16(r + 36);
		o.src_index = rd16(r + 38);
		copy_name(o.name, r + 40, SND_SMALL_NAME_LEN);
		o.src_bytes = rd32(r + 56);
	}
	return o;
}

int snd_find_loop(const snd_section *s, uint32_t tag_id)
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

snd_loop snd_get_loop(const snd_section *s, uint32_t index)
{
	snd_loop o;
	memset(&o, 0, sizeof o);
	if (index >= s->loop_count) return o;
	{
		const uint8_t *r = s->loops + (size_t)index * LOOP_REC;
		o.tag_id = rd32(r);
		o.flags = rd32(r + 4);
		o.param = rdf(r + 8);
		o.track_count = rd16(r + 12);
		o.detail_count = rd16(r + 14);
		o.track_first = rd32(r + 16);
		o.detail_first = rd32(r + 20);
		copy_name(o.name, r + 32, SND_NAME_LEN);
	}
	return o;
}

snd_track snd_get_track(const snd_section *s, const snd_loop *lp, uint32_t k)
{
	snd_track o;
	memset(&o, 0, sizeof o);
	o.start = o.loop = o.end = o.alt_loop = o.alt_end = SND_NO_TAG;
	if (k >= lp->track_count || (uint64_t)lp->track_first + k >= s->track_count) return o;
	{
		const uint8_t *r = s->tracks + (size_t)(lp->track_first + k) * TRACK_REC;
		o.flags = rd32(r);
		o.gain = rdf(r + 4);
		o.fade_in = rdf(r + 8);
		o.fade_out = rdf(r + 12);
		o.start = rd32(r + 16);
		o.loop = rd32(r + 20);
		o.end = rd32(r + 24);
		o.alt_loop = rd32(r + 28);
		o.alt_end = rd32(r + 32);
	}
	return o;
}

snd_detail snd_get_detail(const snd_section *s, const snd_loop *lp, uint32_t k)
{
	snd_detail o;
	memset(&o, 0, sizeof o);
	o.sound = SND_NO_TAG;
	if (k >= lp->detail_count || (uint64_t)lp->detail_first + k >= s->detail_count) return o;
	{
		const uint8_t *r = s->details + (size_t)(lp->detail_first + k) * DETAIL_REC;
		o.sound = rd32(r);
		o.period_lo = rdf(r + 4);
		o.period_hi = rdf(r + 8);
		o.gain = rdf(r + 12);
		o.flags = rd32(r + 16);
		o.yaw_lo = rdf(r + 20);
		o.yaw_hi = rdf(r + 24);
		o.pitch_lo = rdf(r + 28);
		o.pitch_hi = rdf(r + 32);
		o.dist_lo = rdf(r + 36);
		o.dist_hi = rdf(r + 40);
	}
	return o;
}

uint32_t snd_check_vag(const snd_section *s, const uint8_t *sndd, size_t n)
{
	uint32_t i, b;
	if (!sndd || n != s->sndd_size) return 1;
	for (i = 0; i < s->perm_count; i++) {
		const uint8_t *r = s->perms + (size_t)i * PERM_REC;
		uint32_t off = rd32(r), sz = rd32(r + 4), frames = rd32(r + 8), rate = rd32(r + 12), nb = sz / 16u - 2u;
		uint32_t loop = rd16(r + 34) & SND_P_LOOP;
		const uint8_t *h, *blocks, *term;
		if (off > n || sz > n - off || sz < 48u) return i + 1;
		h = sndd + off;
		blocks = h + 16;
		term = h + sz - 16;
		if (memcmp(h, "APCM", 4) != 0 || h[4] != 1 || h[5] != 1 || h[6] != (loop ? 1u : 0u) || h[7] != 0) return i + 1;
		if (rd32(h + 8) != (uint32_t)((uint64_t)rate * 4096u / 48000u) || rd32(h + 12) != frames) return i + 1;
		for (b = 0; b < nb; b++) {
			const uint8_t *blk = blocks + (size_t)b * 16u;
			uint8_t want;
			if ((blk[0] >> 4) > 4u || (blk[0] & 15u) > 12u) return i + 1;
			if (loop) want = b == 0 ? (nb == 1 ? 7u : 6u) : (b == nb - 1u ? 3u : 0u);
			else want = b == nb - 1u ? 1u : 0u;
			if (blk[1] != want) return i + 1;
		}
		if (term[0] != blocks[(size_t)(nb - 1u) * 16u] || term[1] != 7u) return i + 1;
		for (b = 2; b < 16; b++)
			if (term[b]) return i + 1;
	}
	return 0;
}
