/* Host test: open the ANIM section of the pack written by tools/tests/test_animconv.py (ANIM_FIXTURE) and
 * check it against the world that test baked (a biped graph with eight animations, a three-node door graph
 * and an orphan graph) and against AEXP, the Python reference sampler's poses at 12 frames of every
 * animation. Then check loop/clamp behaviour, shortest-path blending and root motion, corrupt every byte of
 * the section and truncate it at every length: whatever anim_open accepts must be completely safe to walk
 * and sample (ASan / UBSan watch every access). */
#include "stream/hp2.h"
#include "world/anim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

static const uint32_t TAG_BIPED = 0xE1760002u, TAG_DOOR = 0xE1790005u, TAG_ORPHAN = 0xE17B0007u;

static volatile uint32_t sink;
static uint32_t f2u(float x) { x = fabsf(x); return x < 1e9f ? (uint32_t)x : 0; }

/* Everything an opened section hands out. */
static void walk(const anim_section *s, int deep)
{
	static const float frames[] = { 0.0f, 0.4f, 1.0f, 2.5f, 7.75f, 29.5f, 100.0f, 1.0e9f, -5.0f, 1.0e30f, NAN, INFINITY };
	const unsigned nframes = deep ? sizeof frames / sizeof frames[0] : 5;
	uint32_t acc = 0;
	for (uint32_t gi = 0; gi < s->graph_count; gi++) {
		anim_graph g = anim_get_graph(s, gi);
		acc += (uint32_t)anim_find_graph(s, g.tag_id) + g.node_count;
		for (uint32_t k = 0; k < g.anim_count; k++) {
			anim_info in = anim_get(s, &g, k);
			float rm[4];
			acc += (uint32_t)anim_find_name(s, &g, in.name) + in.frame_count + in.bytes + (uint32_t)strlen(in.name);
			for (unsigned fi = 0; fi < nframes; fi++) {
				anim_pose p;
				anim_sample(s, &g, k, frames[fi], &p);
				if (p.count != g.node_count) abort();
				for (uint32_t n = 0; n < p.count; n++) acc += f2u(p.node[n].rot[0] * 100.0f) + f2u(p.node[n].trans[2]);
			}
			for (uint32_t f = 0; f < (deep ? in.frame_count + 2u : 3u); f++) { anim_root_motion(s, &g, k, f, rm); acc += f2u(rm[0] * 1e4f); }
		}
	}
	sink = acc;
}

static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static int check_expected(const anim_section *s, const uint8_t *e, size_t en)
{
	CHECK(en >= 4);
	uint32_t count = rd32(e), seen = 0;
	size_t at = 4;
	CHECK(count > 100);
	for (uint32_t r = 0; r < count; r++) {
		CHECK(en - at >= 12);
		uint32_t gi = rd32(e + at), ai = rd32(e + at + 4);
		float frame = rdf(e + at + 8);
		at += 12;
		CHECK(gi < s->graph_count);
		anim_graph g = anim_get_graph(s, gi);
		CHECK(ai < g.anim_count && en - at >= 32u * g.node_count);
		anim_pose p;
		anim_sample(s, &g, ai, frame, &p);
		CHECK(p.count == g.node_count);
		for (uint32_t n = 0; n < g.node_count; n++, at += 32) {
			float want[8], dot = 0.0f, sg;
			for (int c = 0; c < 8; c++) want[c] = rdf(e + at + 4u * (uint32_t)c);
			for (int c = 0; c < 4; c++) dot += p.node[n].rot[c] * want[c];
			sg = dot < 0.0f ? -1.0f : 1.0f;
			for (int c = 0; c < 4; c++) CHECK(near(sg * p.node[n].rot[c], want[c], 2e-4f));
			for (int c = 0; c < 3; c++) CHECK(near(p.node[n].trans[c], want[4 + c], 1e-4f + 1e-5f * fabsf(want[4 + c])));
			CHECK(near(p.node[n].scale, want[7], 1e-4f));
			float len = 0.0f;
			for (int c = 0; c < 4; c++) len += p.node[n].rot[c] * p.node[n].rot[c];
			CHECK(near(len, 1.0f, 1e-4f));
		}
		seen++;
	}
	CHECK(seen == count && at == en);
	return 0;
}

static int check_tables(const anim_section *s)
{
	CHECK(s->graph_count == 3 && s->anim_count == 10);
	int gb = anim_find_graph(s, TAG_BIPED), gd = anim_find_graph(s, TAG_DOOR), go = anim_find_graph(s, TAG_ORPHAN);
	CHECK(gb == 0 && gd == 1 && go == 2);
	CHECK(anim_find_graph(s, 0) == -1 && anim_find_graph(s, TAG_BIPED + 1) == -1 && anim_find_graph(s, 0xFFFFFFFFu) == -1);
	anim_graph b = anim_get_graph(s, 0), d = anim_get_graph(s, 1), o = anim_get_graph(s, 2), none = anim_get_graph(s, 3);
	CHECK(b.tag_id == TAG_BIPED && b.node_count == 5 && b.anim_count == 8 && b.first_anim == 0);
	CHECK(d.tag_id == TAG_DOOR && d.node_count == 3 && d.anim_count == 1 && d.first_anim == 8);
	CHECK(o.tag_id == TAG_ORPHAN && o.node_count == 3 && o.anim_count == 1 && o.first_anim == 9);
	CHECK(none.node_count == 0 && none.anim_count == 0 && none.tag_id == 0);
	CHECK(anim_get_graph(s, 0xFFFFFFFFu).anim_count == 0);

	static const char *names[8] = { "stand idle", "stand move-front", "stand aim-still", "stand long", "cine walk",
	                                "stand idle%1", "stand airborne", "idle" };
	static const uint16_t frames[8] = { 40, 30, 9, 300, 60, 40, 30, 40 }, types[8] = { 0, 0, 1, 0, 0, 0, 0, 0 };
	static const uint16_t flags[8] = { ANIM_LOOPS, ANIM_LOOPS, 0, 0, 0, ANIM_LOOPS, ANIM_LOOPS, ANIM_LOOPS };
	static const uint16_t root[8] = { 0, 3, 0, 0, 2, 0, 3, 0 };
	for (uint32_t k = 0; k < 8; k++) {
		anim_info in = anim_get(s, &b, k);
		CHECK(strcmp(in.name, names[k]) == 0 && in.frame_count == frames[k] && in.type == types[k]);
		CHECK(in.flags == flags[k] && in.node_count == 5 && in.fps == 30.0f && in.bytes > 4);
		CHECK(in.root_motion == root[k] && in.loop_frame == (k == 6 ? 10 : 0));
	}
	anim_info dd = anim_get(s, &d, 0), oo = anim_get(s, &o, 0);
	CHECK(strcmp(dd.name, "device position") == 0 && dd.type == 1 && dd.frame_count == 3 && dd.node_count == 3 && dd.flags == 0);
	CHECK(strcmp(oo.name, "t") == 0 && oo.type == 0 && oo.frame_count == 3);
	CHECK(anim_get(s, &b, 8).frame_count == 0 && anim_get(s, &b, 8).name[0] == 0 && anim_get(s, &b, 0xFFFFFFFFu).bytes == 0);
	/* identical animations share their data: 'stand idle', 'stand idle%1' and 'idle' */
	CHECK(anim_get(s, &b, 0).bytes == anim_get(s, &b, 5).bytes && anim_get(s, &b, 0).bytes == anim_get(s, &b, 7).bytes);

	/* name search: exact match first, then the first substring match */
	CHECK(anim_find_name(s, &b, "stand idle") == 0 && anim_find_name(s, &b, "idle") == 7);
	CHECK(anim_find_name(s, &b, "idle%1") == 5 && anim_find_name(s, &b, "stand idle%1") == 5);
	CHECK(anim_find_name(s, &b, "move") == 1 && anim_find_name(s, &b, "aim") == 2 && anim_find_name(s, &b, "stand") == 0);
	CHECK(anim_find_name(s, &b, "Stand") == -1 && anim_find_name(s, &b, "zzz") == -1 && anim_find_name(s, &b, "") == 0);
	CHECK(anim_find_name(s, &d, "position") == 0 && anim_find_name(s, &d, "idle") == -1 && anim_find_name(s, &none, "x") == -1);
	CHECK(anim_find_name(s, &b, NULL) == -1);
	return 0;
}

static int check_sampling(const anim_section *s)
{
	anim_graph b = anim_get_graph(s, 0), d = anim_get_graph(s, 1);
	anim_pose p, q;

	/* the door: node 1 slides up 0.5 per frame, node 0 and 2 stay at the graph's rest pose (all zero) */
	anim_sample(s, &d, 0, 1.0f, &p);
	CHECK(p.count == 3 && near(p.node[1].trans[2], 0.5f, 1e-4f) && near(p.node[0].trans[2], 0.0f, 1e-6f));
	anim_sample(s, &d, 0, 1.5f, &p);
	CHECK(near(p.node[1].trans[2], 0.75f, 1e-4f));
	anim_sample(s, &d, 0, 2.0f, &p);
	CHECK(near(p.node[1].trans[2], 1.0f, 1e-4f) && near(p.node[1].rot[3], 1.0f, 1e-6f) && near(p.node[1].scale, 1.0f, 1e-6f));
	anim_sample(s, &d, 0, 99.0f, &p);                                    /* not looping: clamps at the last frame */
	CHECK(near(p.node[1].trans[2], 1.0f, 1e-4f));
	anim_sample(s, &d, 0, -3.0f, &p);
	CHECK(near(p.node[1].trans[2], 0.0f, 1e-6f));
	anim_sample(s, &d, 0, NAN, &p);
	CHECK(p.count == 3 && near(p.node[1].trans[2], 0.0f, 1e-6f));

	/* looping walk: wraps at frame_count and blends the last frame with the first */
	anim_sample(s, &b, 1, 29.0f, &p);
	anim_pose first, wrapped;
	anim_sample(s, &b, 1, 0.0f, &first);
	anim_sample(s, &b, 1, 29.5f, &q);
	anim_blend(&p, &first, 0.5f, &p);
	for (int n = 0; n < 5; n++) for (int c = 0; c < 4; c++) CHECK(near(p.node[n].rot[c], q.node[n].rot[c], 2e-4f) || near(p.node[n].rot[c], -q.node[n].rot[c], 2e-4f));
	anim_sample(s, &b, 1, 37.0f, &wrapped);
	anim_sample(s, &b, 1, 7.0f, &q);
	for (int n = 0; n < 5; n++) for (int c = 0; c < 3; c++) CHECK(near(wrapped.node[n].trans[c], q.node[n].trans[c], 1e-4f));
	/* the airborne animation loops back to frame 10, not 0 */
	anim_sample(s, &b, 6, 42.0f, &wrapped);
	anim_sample(s, &b, 6, 22.0f, &q);
	for (int n = 0; n < 5; n++) for (int c = 0; c < 3; c++) CHECK(near(wrapped.node[n].trans[c], q.node[n].trans[c], 1e-4f));
	/* a non-looping 300 frame animation clamps */
	anim_sample(s, &b, 3, 299.0f, &p);
	anim_sample(s, &b, 3, 5000.0f, &q);
	CHECK(memcmp(&p, &q, sizeof(float) * 8 * 5) == 0);
	CHECK(near(p.node[0].trans[0], 299.0f, 0.01f));       /* a 300 world unit range: stored as float32, not quantised */
	anim_sample(s, &b, 3, 150.0f, &p);
	CHECK(near(p.node[0].trans[0], 150.0f, 0.5f));

	/* a graph copy with fewer nodes samples that many; a bogus graph samples nothing and is safe */
	anim_graph small = b;
	small.node_count = 2;
	anim_sample(s, &small, 0, 3.0f, &p);
	CHECK(p.count == 2);
	small = b;
	small.first_anim = 0xFFFFFFF0u;
	p.count = 77;
	anim_sample(s, &small, 0, 1.0f, &p);
	CHECK(p.count == 0);
	small = b;
	small.node_count = 64;
	anim_sample(s, &small, 0, 1.0f, &p);
	CHECK(p.count == 5);
	anim_sample(s, &b, 99, 1.0f, &p);
	CHECK(p.count == 0);
	return 0;
}

static int check_blend(void)
{
	anim_pose a, b, out;
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	a.count = 3;
	b.count = 2;
	const float s5 = sinf(5.0f * 3.14159265f / 180.0f), c5 = cosf(5.0f * 3.14159265f / 180.0f);
	const float s15 = sinf(15.0f * 3.14159265f / 180.0f), c15 = cosf(15.0f * 3.14159265f / 180.0f);
	a.node[0].rot[3] = 1.0f; a.node[0].scale = 1.0f;
	b.node[0].rot[3] = -1.0f; b.node[0].scale = 3.0f;               /* antipodal: the same rotation */
	b.node[0].trans[1] = 4.0f;
	a.node[1].rot[2] = s5; a.node[1].rot[3] = c5; a.node[1].scale = 1.0f;
	b.node[1].rot[2] = -s15; b.node[1].rot[3] = -c15; b.node[1].scale = 1.0f;   /* -q of a 30 degree turn */
	anim_blend(&a, &b, 0.5f, &out);
	CHECK(out.count == 2);                                          /* the shorter pose limits the blend */
	CHECK(near(fabsf(out.node[0].rot[3]), 1.0f, 1e-6f) && near(out.node[0].rot[2], 0.0f, 1e-6f));
	CHECK(near(out.node[0].scale, 2.0f, 1e-6f) && near(out.node[0].trans[1], 2.0f, 1e-6f));
	/* shortest path: halfway between a 10 degree and a 30 degree turn is a 20 degree turn */
	CHECK(near(fabsf(out.node[1].rot[2]), sinf(10.0f * 3.14159265f / 180.0f), 1e-5f));
	CHECK(near(fabsf(out.node[1].rot[3]), cosf(10.0f * 3.14159265f / 180.0f), 1e-5f));
	anim_blend(&a, &b, 0.0f, &out);
	CHECK(near(out.node[1].rot[2] * out.node[1].rot[3], s5 * c5, 1e-6f) && near(out.node[0].scale, 1.0f, 1e-6f));
	anim_blend(&a, &b, 1.0f, &out);
	CHECK(near(fabsf(out.node[1].rot[3]), c15, 1e-6f) && near(out.node[0].scale, 3.0f, 1e-6f));
	/* aliasing: out == a, out == b */
	anim_pose a2 = a, b2 = b;
	anim_blend(&a2, &b, 0.25f, &a2);
	anim_blend(&a, &b2, 0.25f, &b2);
	CHECK(near(a2.node[0].scale, 1.5f, 1e-6f) && near(b2.node[0].scale, 1.5f, 1e-6f) && a2.count == 2 && b2.count == 2);
	return 0;
}

static int check_root_motion(const anim_section *s)
{
	anim_graph b = anim_get_graph(s, 0);
	float rm[4];
	for (uint32_t f = 0; f < 30; f++) {            /* walk: dx 0.0186 + 0.0005 sin f, dy -0.002, dz 0, dyaw 0.01 f / 30 */
		anim_root_motion(s, &b, 1, f, rm);
		CHECK(near(rm[0], 0.0186f + 0.0005f * sinf((float)f), 1e-5f) && near(rm[1], -0.002f, 1e-5f) && rm[2] == 0.0f);
		CHECK(near(rm[3], 0.01f * (float)f / 30.0f, 1e-5f));
	}
	anim_root_motion(s, &b, 1, 1000, rm);          /* past the end: the last frame */
	CHECK(near(rm[3], 0.01f * 29.0f / 30.0f, 1e-5f));
	anim_root_motion(s, &b, 4, 5, rm);             /* compressed cinematic: dx dy dyaw */
	CHECK(near(rm[0], 0.01f, 1e-5f) && rm[1] == 0.0f && near(rm[2], 0.002f, 1e-5f) && rm[3] == 0.0f);
	anim_root_motion(s, &b, 0, 5, rm);             /* idle: no root motion */
	CHECK(rm[0] == 0.0f && rm[1] == 0.0f && rm[2] == 0.0f && rm[3] == 0.0f);
	anim_root_motion(s, &b, 77, 5, rm);
	CHECK(rm[0] == 0.0f && rm[3] == 0.0f);
	return 0;
}

static int check_corruption(const uint8_t *good, size_t n)
{
	uint8_t *buf = malloc(n);
	CHECK(buf);
	static const int deltas[] = { 0, 1, 2, 0x80 };                 /* set to zero, add one, xor 0x55, xor 0x80 */
	uint32_t accepted = 0, tried = 0;
	for (size_t i = 0; i < n; i++) {
		for (unsigned d = 0; d < sizeof deltas / sizeof deltas[0]; d++) {
			memcpy(buf, good, n);
			buf[i] = d == 0 ? 0 : d == 1 ? (uint8_t)(good[i] + 1) : d == 2 ? (uint8_t)(good[i] ^ 0x55) : (uint8_t)(good[i] ^ deltas[d]);
			if (buf[i] == good[i]) continue;
			anim_section s;
			anim_status st = anim_open(&s, buf, n);
			tried++;
			if (st == ANIM_OK) { accepted++; walk(&s, 0); }
		}
	}
	free(buf);
	CHECK(tried > 10000 && accepted > 0);                           /* value bytes can change freely; tables cannot */

	/* truncation at every length, into an exactly sized heap block */
	for (size_t len = 0; len < n; len++) {
		uint8_t *t = malloc(len ? len : 1);
		CHECK(t);
		memcpy(t, good, len);
		anim_section s;
		CHECK(anim_open(&s, t, len) != ANIM_OK);
		CHECK(s.graph_count == 0 && s.anim_count == 0);
		free(t);
	}
	/* wrong sizes, bad magic and version report their own status */
	anim_section s;
	CHECK(anim_open(&s, good, n + 1) == ANIM_ERR_LAYOUT && anim_open(&s, good, 63) == ANIM_ERR_TRUNCATED);
	buf = malloc(n);
	CHECK(buf);
	memcpy(buf, good, n);
	buf[0] = 'X';
	CHECK(anim_open(&s, buf, n) == ANIM_ERR_MAGIC);
	buf[0] = 'A';
	buf[4] = 2;
	CHECK(anim_open(&s, buf, n) == ANIM_ERR_VERSION);
	free(buf);
	CHECK(anim_open(&s, NULL, 0) == ANIM_ERR_TRUNCATED);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	FILE *f = fopen(argv[1], "rb");
	CHECK(f);
	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	rewind(f);
	uint8_t *img = malloc(n);
	CHECK(img && fread(img, 1, n, f) == n);
	fclose(f);

	hp2_pack pk;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	const hp2_section *as = hp2_find(&pk, "ANIM"), *es = hp2_find(&pk, "AEXP");
	CHECK(as && es);
	uint8_t *raw = malloc(as->raw_size), *exp = malloc(es->raw_size);
	CHECK(raw && exp && hp2_load(&pk, as, raw, as->raw_size) == HP2_OK && hp2_load(&pk, es, exp, es->raw_size) == HP2_OK);

	anim_section s;
	CHECK(anim_open(&s, raw, as->raw_size) == ANIM_OK);
	CHECK(((uintptr_t)s.pool - (uintptr_t)raw) % 16 == 0 && s.size == as->raw_size);
	if (check_tables(&s)) return 1;
	if (check_expected(&s, exp, es->raw_size)) return 1;
	if (check_sampling(&s)) return 1;
	if (check_blend()) return 1;
	if (check_root_motion(&s)) return 1;
	walk(&s, 1);
	if (check_corruption(raw, as->raw_size)) return 1;
	free(raw);
	free(exp);
	free(img);
	printf("ok\n");
	return 0;
}
