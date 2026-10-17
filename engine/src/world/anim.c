/* ANIM section reader and sampler (docs/12-anim-section.md). Pure C11, no heap, no double. anim_open
 * validates the whole section once (tables, every animation blob down to its track headers and key
 * frame lists), so the accessors and the samplers do no bounds checks on section data. */
#include "world/anim.h"
#include <math.h>
#include <string.h>

#define HEADER_SIZE 64u
#define GRAPH_REC 32u
#define ANIM_REC 96u
#define POOL_RESERVED 16u        /* pool offset 0 means "none": the first 16 bytes are never a block */
#define MAX_ANIMS (1u << 20)

enum { M_DEFAULT = 0, M_STATIC = 1, M_ANIMATED = 2 };
enum { T_DENSE = 1, T_IDX8 = 2, T_F32 = 4, T_AXIS = 8, T_AXIS_SHIFT = 4 };
enum { CH_ROT = 0, CH_TRANS = 1, CH_SCALE = 2 };

_Static_assert(sizeof(anim_xform) == 32, "anim_xform is stored as 8 packed floats");

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static int      rds16(const uint8_t *p) { return (int16_t)(uint16_t)rd16(p); }
static float    rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
static uint32_t pad4(uint32_t v) { return (v + 3u) & ~3u; }

/* anim record layout (ANIM_REC bytes) */
#define A_NAME 0
#define A_TYPE 48
#define A_FRAMES 50
#define A_LOOP 52
#define A_FLAGS 54
#define A_NODES 56
#define A_ROOT_TYPE 58
#define A_GRAPH_NODES 60
#define A_FPS 64
#define A_DATA_OFF 68
#define A_DATA_BYTES 72
#define A_ROOT_OFF 76
#define A_DEFAULT_OFF 80

/* ---- validation ------------------------------------------------------------------------------ */
static uint32_t root_components(uint32_t root_type) { return root_type + 1u; }
static uint32_t root_block_bytes(uint32_t root_type, uint32_t frames)
{
	return 16u + pad4(2u * root_components(root_type) * frames);
}

/* One animation blob (docs/12-anim-section.md): mode bytes, then static values and tracks. */
static int blob_ok(const uint8_t *b, uint32_t size, uint32_t nn, uint32_t fc)
{
	uint32_t pos = pad4(nn), n, ch, j;
	if (pos > size) return 0;
	for (n = 0; n < nn; n++) {
		if (b[n] & 0xC0u) return 0;
		for (ch = 0; ch < 3; ch++) {
			uint32_t mode = (b[n] >> (2 * ch)) & 3u;
			if (mode == M_DEFAULT) continue;
			if (mode == M_STATIC) {
				uint32_t sz = ch == CH_ROT ? 8u : ch == CH_TRANS ? 12u : 4u;
				if (size - pos < sz) return 0;
				pos += sz;
				continue;
			}
			if (mode != M_ANIMATED) return 0;
			{
				uint32_t k, flags, per, mask, bytes;
				if (size - pos < 4u) return 0;
				k = rd16(b + pos);
				flags = rd16(b + pos + 2);
				pos += 4;
				mask = T_DENSE | T_IDX8;
				if (ch == CH_ROT) mask |= T_AXIS | (3u << T_AXIS_SHIFT);
				if (ch == CH_TRANS) mask |= T_F32;
				if (flags & ~mask) return 0;
				if (ch == CH_ROT && !(flags & T_AXIS) && (flags >> T_AXIS_SHIFT)) return 0;
				if (ch == CH_ROT && (flags & T_AXIS) && ((flags >> T_AXIS_SHIFT) & 3u) > 2u) return 0;
				if (k < 2u || k > fc) return 0;
				if (ch == CH_TRANS && !(flags & T_F32)) {
					if (size - pos < 24u) return 0;
					pos += 24;
				}
				if (flags & T_DENSE) {
					if (k != fc || (flags & T_IDX8)) return 0;
				} else {
					uint32_t w = (flags & T_IDX8) ? 1u : 2u, prev = 0, v = 0;
					if (w == 1u && fc > 256u) return 0;
					bytes = pad4(w * k);
					if (size - pos < bytes) return 0;
					for (j = 0; j < k; j++) {
						v = w == 1u ? b[pos + j] : rd16(b + pos + 2u * j);
						if (j == 0 ? v != 0 : v <= prev) return 0;
						prev = v;
					}
					if (v != fc - 1u) return 0;
					pos += bytes;
				}
				per = ch == CH_ROT ? ((flags & T_AXIS) ? 4u : 6u) : ch == CH_TRANS ? ((flags & T_F32) ? 12u : 6u) : 4u;
				bytes = pad4(per * k);
				if (size - pos < bytes) return 0;
				pos += bytes;
			}
		}
	}
	return pos == size;
}

anim_status anim_open(anim_section *s, const uint8_t *d, size_t size)
{
	uint32_t ng, na, goff, aoff, poff, total, i, k, acc = 0, prev_tag = 0;
	size_t pool_size;
	memset(s, 0, sizeof *s);
	if (!d || size < HEADER_SIZE) return ANIM_ERR_TRUNCATED;
	if (memcmp(d, "ANIM", 4) != 0) return ANIM_ERR_MAGIC;
	if (rd16(d + 4) != 1u) return ANIM_ERR_VERSION;
	ng = rd32(d + 8);
	na = rd32(d + 12);
	goff = rd32(d + 16);
	aoff = rd32(d + 20);
	poff = rd32(d + 24);
	total = rd32(d + 28);
	if (total > size) return ANIM_ERR_TRUNCATED;
	if (total != size || rd16(d + 6) != 0) return ANIM_ERR_LAYOUT;
	if (ng > 0xFFFFu || na > MAX_ANIMS) return ANIM_ERR_LAYOUT;
	if (goff != HEADER_SIZE || aoff != goff + GRAPH_REC * ng) return ANIM_ERR_LAYOUT;
	if (poff % 16u || (uint64_t)poff < (uint64_t)aoff + (uint64_t)ANIM_REC * na || poff > size) return ANIM_ERR_LAYOUT;
	pool_size = size - poff;
	if (pool_size < POOL_RESERVED) return ANIM_ERR_LAYOUT;

	for (i = 0; i < ng; i++) {
		const uint8_t *g = d + goff + GRAPH_REC * i;
		uint32_t tag = rd32(g), nn = rd16(g + 4), cnt = rd16(g + 6), first = rd32(g + 8), doff = rd32(g + 12);
		if (i > 0 && tag <= prev_tag) return ANIM_ERR_LAYOUT;      /* sorted: anim_find_graph bisects */
		prev_tag = tag;
		if (nn < 1u || nn > ANIM_MAX_NODES || first != acc || cnt > na - acc) return ANIM_ERR_LAYOUT;
		if (doff % 16u || doff < POOL_RESERVED || doff > pool_size || (size_t)nn * 32u > pool_size - doff) return ANIM_ERR_LAYOUT;
		for (k = 0; k < cnt; k++) {
			const uint8_t *a = d + aoff + ANIM_REC * (first + k);
			uint32_t type = rd16(a + A_TYPE), fc = rd16(a + A_FRAMES), loop = rd16(a + A_LOOP), fl = rd16(a + A_FLAGS);
			uint32_t anodes = rd16(a + A_NODES), rtype = rd16(a + A_ROOT_TYPE), gnodes = rd16(a + A_GRAPH_NODES);
			uint32_t boff = rd32(a + A_DATA_OFF), bbytes = rd32(a + A_DATA_BYTES), roff = rd32(a + A_ROOT_OFF);
			float fps = rdf(a + A_FPS);
			if (a[A_NAME + ANIM_NAME_LEN - 1] != 0) return ANIM_ERR_LAYOUT;
			if (type > 2u || fc < 1u || loop >= fc || (fl & ~(ANIM_LOOPS | ANIM_WORLD_RELATIVE))) return ANIM_ERR_LAYOUT;
			if (anodes < 1u || anodes > nn || gnodes != nn || rtype > 3u || !(fps > 0.0f && fps < 1000.0f)) return ANIM_ERR_LAYOUT;
			if (rd32(a + A_DEFAULT_OFF) != doff) return ANIM_ERR_LAYOUT;
			if (boff % 16u || boff < POOL_RESERVED || boff > pool_size || bbytes > pool_size - boff) return ANIM_ERR_LAYOUT;
			if (!blob_ok(d + poff + boff, bbytes, anodes, fc)) return ANIM_ERR_LAYOUT;
			if (rtype == 0) {
				if (roff != 0) return ANIM_ERR_LAYOUT;
			} else {
				uint32_t rb = root_block_bytes(rtype, fc);
				if (roff % 16u || roff < POOL_RESERVED || roff > pool_size || rb > pool_size - roff) return ANIM_ERR_LAYOUT;
			}
		}
		acc += cnt;
	}
	if (acc != na) return ANIM_ERR_LAYOUT;
	s->data = d;
	s->size = size;
	s->graph_count = ng;
	s->anim_count = na;
	s->graphs = d + goff;
	s->anims = d + aoff;
	s->pool = d + poff;
	return ANIM_OK;
}

/* ---- tables ---------------------------------------------------------------------------------- */
int anim_find_graph(const anim_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->graph_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->graphs + GRAPH_REC * mid);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1; else hi = mid;
	}
	return -1;
}

anim_graph anim_get_graph(const anim_section *s, uint32_t index)
{
	anim_graph g;
	memset(&g, 0, sizeof g);
	if (index < s->graph_count) {
		const uint8_t *p = s->graphs + GRAPH_REC * index;
		g.tag_id = rd32(p);
		g.node_count = (uint16_t)rd16(p + 4);
		g.anim_count = (uint16_t)rd16(p + 6);
		g.first_anim = rd32(p + 8);
	}
	return g;
}

/* The record of animation k of g, or NULL when g does not belong to this section / k is out of range. */
static const uint8_t *anim_rec(const anim_section *s, const anim_graph *g, uint32_t k)
{
	uint64_t at;
	if (!s || !s->anims || !g || k >= g->anim_count) return NULL;
	at = (uint64_t)g->first_anim + k;
	return at < s->anim_count ? s->anims + ANIM_REC * (size_t)at : NULL;
}

anim_info anim_get(const anim_section *s, const anim_graph *g, uint32_t k)
{
	anim_info in;
	const uint8_t *a = anim_rec(s, g, k);
	memset(&in, 0, sizeof in);
	if (!a) return in;
	memcpy(in.name, a + A_NAME, ANIM_NAME_LEN);
	in.name[ANIM_NAME_LEN - 1] = 0;
	in.type = (uint16_t)rd16(a + A_TYPE);
	in.frame_count = (uint16_t)rd16(a + A_FRAMES);
	in.loop_frame = (uint16_t)rd16(a + A_LOOP);
	in.flags = (uint16_t)rd16(a + A_FLAGS);
	in.node_count = (uint16_t)rd16(a + A_NODES);
	in.fps = rdf(a + A_FPS);
	in.root_motion = (uint16_t)rd16(a + A_ROOT_TYPE);
	in.bytes = rd32(a + A_DATA_BYTES) + (in.root_motion ? root_block_bytes(in.root_motion, in.frame_count) : 0u);
	return in;
}

int anim_find_name(const anim_section *s, const anim_graph *g, const char *token)
{
	uint32_t k;
	if (!g || !token) return -1;
	for (k = 0; k < g->anim_count; k++) {
		const uint8_t *a = anim_rec(s, g, k);
		if (a && strcmp((const char *)a + A_NAME, token) == 0) return (int)k;
	}
	for (k = 0; k < g->anim_count; k++) {
		const uint8_t *a = anim_rec(s, g, k);
		if (a && strstr((const char *)a + A_NAME, token)) return (int)k;
	}
	return -1;
}

/* ---- sampling ---------------------------------------------------------------------------------- */
typedef struct {
	const uint8_t *idx;     /* NULL when dense */
	const uint8_t *val;
	const uint8_t *aux;     /* translation: base[3], step[3] */
	uint32_t       k, flags;
} track;

/* Parse the track at p (validated by anim_open); returns the byte after it. */
static const uint8_t *track_parse(const uint8_t *p, int ch, track *t)
{
	uint32_t per;
	t->k = rd16(p);
	t->flags = rd16(p + 2);
	p += 4;
	t->aux = NULL;
	if (ch == CH_TRANS && !(t->flags & T_F32)) { t->aux = p; p += 24; }
	t->idx = NULL;
	if (!(t->flags & T_DENSE)) {
		t->idx = p;
		p += pad4(((t->flags & T_IDX8) ? 1u : 2u) * t->k);
	}
	t->val = p;
	per = ch == CH_ROT ? ((t->flags & T_AXIS) ? 4u : 6u) : ch == CH_TRANS ? ((t->flags & T_F32) ? 12u : 6u) : 4u;
	return p + pad4(per * t->k);
}

/* Segment of the track holding frame f in [0, last]: keys lo and lo + 1, blend factor u. */
static void track_segment(const track *t, float f, uint32_t *lo_out, float *u)
{
	uint32_t i = (uint32_t)f, lo;
	if (!t->idx) {
		lo = i < t->k - 1u ? i : t->k - 2u;
		*u = f - (float)lo;
	} else {
		uint32_t a = 0, b = t->k - 1u, f0, f1;
		int w1 = (t->flags & T_IDX8) != 0;
		while (b - a > 1u) {
			uint32_t mid = (a + b) >> 1, v = w1 ? t->idx[mid] : rd16(t->idx + 2u * mid);
			if (v <= i) a = mid; else b = mid;
		}
		lo = a;
		f0 = w1 ? t->idx[lo] : rd16(t->idx + 2u * lo);
		f1 = w1 ? t->idx[lo + 1u] : rd16(t->idx + 2u * (lo + 1u));
		*u = (f - (float)f0) / (float)(f1 - f0);
	}
	*lo_out = lo;
}

#define Q15_SCALE (0.70710678118654752f / 16383.0f)

static void quat_set4(float q[4], int dropped, float x, float y, float z)
{
	float w = 1.0f - x * x - y * y - z * z;
	w = w > 0.0f ? sqrtf(w) : 0.0f;
	switch (dropped) {
	case 0:  q[0] = w; q[1] = x; q[2] = y; q[3] = z; break;
	case 1:  q[0] = x; q[1] = w; q[2] = y; q[3] = z; break;
	case 2:  q[0] = x; q[1] = y; q[2] = w; q[3] = z; break;
	default: q[0] = x; q[1] = y; q[2] = z; q[3] = w; break;
	}
}

/* Rotation key j of the track: three 15-bit smallest components + 2-bit index, or a 2-component key. */
static void rot_key(const track *t, uint32_t j, float q[4])
{
	if (t->flags & T_AXIS) {
		const uint8_t *p = t->val + 4u * j;
		int ax = (int)((t->flags >> T_AXIS_SHIFT) & 3u);
		q[0] = q[1] = q[2] = 0.0f;
		q[ax] = (float)rds16(p) * (1.0f / 32767.0f);
		q[3] = (float)rds16(p + 2) * (1.0f / 32767.0f);
	} else {
		const uint8_t *p = t->val + 6u * j;
		int a = rds16(p), b = rds16(p + 2), c = rds16(p + 4);
		int idx = (a & 1) | ((b & 1) << 1);
		quat_set4(q, idx, (float)((a - (a & 1)) / 2) * Q15_SCALE, (float)((b - (b & 1)) / 2) * Q15_SCALE,
		          (float)(c / 2) * Q15_SCALE);
	}
}

static void norm4(float q[4])
{
	float l = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	if (l > 1e-24f) {
		float inv = 1.0f / sqrtf(l);
		q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
	} else {
		q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f;
	}
}

/* out = normalised lerp of a and b along the shortest arc; out may alias a or b. */
static void nlerp4(const float a[4], const float b[4], float t, float out[4])
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0.0f ? -1.0f : 1.0f, r[4];
	int i;
	for (i = 0; i < 4; i++) r[i] = a[i] + (s * b[i] - a[i]) * t;
	norm4(r);
	memcpy(out, r, sizeof r);
}

static void eval_rot(const track *t, float f, float out[4])
{
	uint32_t lo;
	float u, a[4], b[4];
	track_segment(t, f, &lo, &u);
	rot_key(t, lo, a);
	rot_key(t, lo + 1u, b);
	nlerp4(a, b, u, out);
}

static void trans_key(const track *t, uint32_t j, float v[3])
{
	int c;
	if (t->flags & T_F32) {
		for (c = 0; c < 3; c++) v[c] = rdf(t->val + 12u * j + 4u * (uint32_t)c);
	} else {
		for (c = 0; c < 3; c++)
			v[c] = rdf(t->aux + 4 * c) + (float)rd16(t->val + 6u * j + 2u * (uint32_t)c) * rdf(t->aux + 12 + 4 * c);
	}
}

static void eval_trans(const track *t, float f, float out[3])
{
	uint32_t lo;
	float u, a[3], b[3];
	int c;
	track_segment(t, f, &lo, &u);
	trans_key(t, lo, a);
	trans_key(t, lo + 1u, b);
	for (c = 0; c < 3; c++) out[c] = a[c] + (b[c] - a[c]) * u;
}

static float eval_scale(const track *t, float f)
{
	uint32_t lo;
	float u, a, b;
	track_segment(t, f, &lo, &u);
	a = rdf(t->val + 4u * lo);
	b = rdf(t->val + 4u * (lo + 1u));
	return a + (b - a) * u;
}

static void static_rot(const uint8_t *p, float q[4])
{
	int i;
	for (i = 0; i < 4; i++) q[i] = (float)rds16(p + 2 * i) * (1.0f / 32767.0f);
	norm4(q);
}

void anim_sample(const anim_section *s, const anim_graph *g, uint32_t k, float frame, anim_pose *out)
{
	const uint8_t *a = anim_rec(s, g, k), *b, *p, *rest;
	uint32_t fc, last, nn, nodes, graph_nodes, n, loops, lf = 0;
	float f = frame, wrap_t = 0.0f, f2 = 0.0f;

	out->count = 0;
	if (!a) return;
	fc = rd16(a + A_FRAMES);
	last = fc - 1u;
	nn = rd16(a + A_NODES);
	graph_nodes = rd16(a + A_GRAPH_NODES);
	nodes = g->node_count < graph_nodes ? g->node_count : graph_nodes;
	loops = rd16(a + A_FLAGS) & ANIM_LOOPS;
	b = s->pool + rd32(a + A_DATA_OFF);
	rest = s->pool + rd32(a + A_DEFAULT_OFF);

	if (!(f >= 0.0f)) f = 0.0f;
	if (f > 1.0e8f) f = 1.0e8f;
	if (fc <= 1u) {
		f = 0.0f;
	} else if (loops) {
		uint32_t lfr = rd16(a + A_LOOP);
		lf = lfr < last ? lfr : 0u;
		if (f >= (float)fc) f = (float)lf + fmodf(f - (float)lf, (float)(fc - lf));
		if (f > (float)last) { wrap_t = f - (float)last; f = (float)last; f2 = (float)lf; }
	} else if (f > (float)last) {
		f = (float)last;
	}

	p = b + pad4(nn);
	for (n = 0; n < nodes && n < nn; n++) {
		anim_xform *x = &out->node[n];
		uint32_t mode = b[n];
		const uint8_t *r = rest + 32u * n;
		track t;
		/* rotation */
		switch (mode & 3u) {
		case M_DEFAULT: memcpy(x->rot, r, 16); break;
		case M_STATIC: static_rot(p, x->rot); p += 8; break;
		default:
			p = track_parse(p, CH_ROT, &t);
			eval_rot(&t, f, x->rot);
			if (wrap_t > 0.0f) { float v[4]; eval_rot(&t, f2, v); nlerp4(x->rot, v, wrap_t, x->rot); }
			break;
		}
		/* translation */
		switch ((mode >> 2) & 3u) {
		case M_DEFAULT: memcpy(x->trans, r + 16, 12); break;
		case M_STATIC: x->trans[0] = rdf(p); x->trans[1] = rdf(p + 4); x->trans[2] = rdf(p + 8); p += 12; break;
		default:
			p = track_parse(p, CH_TRANS, &t);
			eval_trans(&t, f, x->trans);
			if (wrap_t > 0.0f) {
				float v[3];
				int c;
				eval_trans(&t, f2, v);
				for (c = 0; c < 3; c++) x->trans[c] += (v[c] - x->trans[c]) * wrap_t;
			}
			break;
		}
		/* scale */
		switch ((mode >> 4) & 3u) {
		case M_DEFAULT: memcpy(&x->scale, r + 28, 4); break;
		case M_STATIC: x->scale = rdf(p); p += 4; break;
		default:
			p = track_parse(p, CH_SCALE, &t);
			x->scale = eval_scale(&t, f);
			if (wrap_t > 0.0f) x->scale += (eval_scale(&t, f2) - x->scale) * wrap_t;
			break;
		}
	}
	for (; n < nodes; n++) memcpy(&out->node[n], rest + 32u * n, 32);
	out->count = (uint16_t)nodes;
}

void anim_blend(const anim_pose *a, const anim_pose *b, float t, anim_pose *out)
{
	uint32_t n, cnt = a->count < b->count ? a->count : b->count;
	if (cnt > ANIM_MAX_NODES) cnt = ANIM_MAX_NODES;
	for (n = 0; n < cnt; n++) {
		anim_xform x;
		int c;
		nlerp4(a->node[n].rot, b->node[n].rot, t, x.rot);
		for (c = 0; c < 3; c++) x.trans[c] = a->node[n].trans[c] + (b->node[n].trans[c] - a->node[n].trans[c]) * t;
		x.scale = a->node[n].scale + (b->node[n].scale - a->node[n].scale) * t;
		out->node[n] = x;
	}
	out->count = (uint16_t)cnt;
}

void anim_root_motion(const anim_section *s, const anim_graph *g, uint32_t k, uint32_t frame, float out[4])
{
	const uint8_t *a = anim_rec(s, g, k);
	uint32_t rtype, fc, comps, c;
	const uint8_t *blk;
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	if (!a) return;
	rtype = rd16(a + A_ROOT_TYPE);
	if (rtype == 0) return;
	fc = rd16(a + A_FRAMES);
	if (frame >= fc) frame = fc - 1u;
	comps = root_components(rtype);
	blk = s->pool + rd32(a + A_ROOT_OFF);
	for (c = 0; c < comps; c++)
		out[c] = (float)rds16(blk + 16u + 2u * (frame * comps + c)) * (1.0f / 32767.0f) * rdf(blk + 4u * c);
}
