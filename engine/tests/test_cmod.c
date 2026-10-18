/* Host test: open the CMOD / CMD1 sections of a pack written by tools/tests/test_cmodconv.py and check them against
 * the synthetic world that test builds (3 collision models: a hero with 4 nodes, 3 regions, 3 materials, an arm with two
 * permutations; a cart with a wheel region that has a "~blur" permutation; a one-node box; 4 object tags, two of
 * which share the hero). Hand-checked rays, spheres and permutation selection, then 20000 random posed segments
 * against an independent sampling reference, then truncation at every length and a flip of every byte: a corrupt
 * section is either rejected or safe to walk completely (ASan/UBSan). */
#include "stream/hp2.h"
#include "world/cmod.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

#define HERO_TAG 0xE1750001u
#define CART_TAG 0xE1760002u
#define BOX_TAG  0xE1770003u
#define BIPD_HERO 0xE1780004u
#define BIPD_HERO2 0xE1790005u
#define SCEN_BOX 0xE17A0006u
#define SCEN_PLAIN 0xE17B0007u        /* no collision model: absent */
#define VEHI_CART 0xE17C0008u

static volatile uint32_t sink;

/* float -> uint32 without undefined behaviour for negative or huge values (corrupt sections may carry them) */
static uint32_t usafe(float v) { return v > 0.0f && v < 1e9f ? (uint32_t)v : 0u; }
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
static float frand(float lo, float hi) { return lo + (hi - lo) * ((float)(rnd() >> 8) / 16777216.0f); }

static void ident(mat4 *m, float x, float y, float z)
{
	memset(m, 0, sizeof *m);
	m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
	m->m[3][0] = x; m->m[3][1] = y; m->m[3][2] = z;
}

/* rows = images of the basis vectors (core/mat.h); a random rotation times a uniform scale s, then a translation */
static void random_pose(mat4 *m, float s)
{
	float q[4], n = 0.0f, x, y, z, w;
	unsigned i;
	for (i = 0; i < 4; i++) { q[i] = frand(-1.0f, 1.0f); n += q[i] * q[i]; }
	n = sqrtf(n);
	if (n < 0.1f) { q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = n = 1.0f; }
	x = q[0] / n; y = q[1] / n; z = q[2] / n; w = q[3] / n;
	memset(m, 0, sizeof *m);
	m->m[0][0] = s * (1 - 2 * (y * y + z * z)); m->m[0][1] = s * 2 * (x * y + z * w); m->m[0][2] = s * 2 * (x * z - y * w);
	m->m[1][0] = s * 2 * (x * y - z * w); m->m[1][1] = s * (1 - 2 * (x * x + z * z)); m->m[1][2] = s * 2 * (y * z + x * w);
	m->m[2][0] = s * 2 * (x * z + y * w); m->m[2][1] = s * 2 * (y * z - x * w); m->m[2][2] = s * (1 - 2 * (x * x + y * y));
	m->m[3][0] = frand(-1.0f, 1.0f); m->m[3][1] = frand(-1.0f, 1.0f); m->m[3][2] = frand(-1.0f, 1.0f); m->m[3][3] = 1.0f;
}

/* Touch everything an opened section hands out and run rays / spheres at it. */
static void walk(const cmod_section *s, unsigned rays)
{
	uint32_t acc = 0, i, k;
	mat4 poses[CMOD_MAX_NODES];
	uint8_t sel[CMOD_MAX_REGIONS];
	for (i = 0; i < CMOD_MAX_NODES; i++) ident(&poses[i], 0.1f * (float)i, 0.0f, 0.0f);
	for (i = 0; i < CMOD_MAX_REGIONS; i++) sel[i] = (uint8_t)(i * 37);
	for (i = 0; i < s->model_count; i++) {
		cmod_model m;
		uint32_t j;
		if (!cmod_get_model(s, i, &m)) abort();
		if (m.node_count > CMOD_MAX_NODES || m.region_count > CMOD_MAX_REGIONS) abort();
		{
			cmod_model f;
			if (!cmod_find(s, m.tag_id, &f) || f.index != i) abort();
		}
		acc += m.tag_id + m.flags + m.shape_count + usafe(m.max_body_vitality);
		for (j = 0; j < (uint32_t)m.material_count + 1u; j++) { cmod_material mt = cmod_get_material(&m, j); acc += (uint32_t)strlen(mt.name) + mt.flags; }
		for (j = 0; j < (uint32_t)m.region_count + 1u; j++) {
			cmod_region r = cmod_get_region(&m, j);
			acc += (uint32_t)strlen(r.name) + r.flags;
			for (k = 0; k < r.perm_count + 1u; k++) {
				char nm[CMOD_NAME_LEN];
				if (cmod_perm_name(&m, j, k, nm)) { acc += (uint32_t)strlen(nm); if (cmod_find_permutation(&m, j, nm) < 0) abort(); }
			}
		}
		for (j = 0; j < (uint32_t)m.node_count + 1u; j++) { cmod_node n = cmod_get_node(&m, j); acc += (uint32_t)(n.region + 2) + (uint32_t)(n.parent + 2); }
		for (j = 0; j < (uint32_t)m.shape_count + 1u; j++) {
			cmod_shape sh = cmod_get_shape(&m, j);
			if (j < m.shape_count && (sh.node >= m.node_count || sh.region >= m.region_count || sh.material >= m.material_count)) abort();
			acc += sh.node + sh.region;
		}
		for (j = 0; j < rays; j++) {
			cmod_segment seg;
			cmod_hit h;
			uint64_t mask;
			uint16_t reg;
			float t, c[3];
			for (k = 0; k < 3; k++) { seg.a[k] = frand(-3, 3); seg.b[k] = frand(-3, 3); c[k] = frand(-2, 2); }
			if (cmod_ray_ex(&m, poses, &seg, j & 1 ? sel : NULL, &h)) {
				if (!(h.t >= 0.0f && h.t <= 1.0f) || h.region >= m.region_count || h.node >= m.node_count) abort();
				acc += h.region + h.node + h.material;
			}
			if (cmod_ray(&m, poses, &seg, &reg, &t) && !(t >= 0.0f && t <= 1.0f)) abort();
			acc += cmod_sphere(&m, poses, c, frand(0.0f, 1.0f), j & 1 ? sel : NULL, &mask);
		}
	}
	for (i = 0; i < (uint32_t)s->object_count + 1u; i++) {
		cmod_object o;
		cmod_model m;
		if (cmod_get_object(s, i, &o)) {
			if (!cmod_find_object(s, o.tag_id, NULL, &m) || m.tag_id != o.coll_tag) abort();
			acc += o.type + usafe(o.radius);
		}
	}
	sink = acc;
}

static int load_section(const hp2_pack *pk, const char *tag, uint8_t **raw, size_t *n)
{
	const hp2_section *sec = hp2_find(pk, tag);
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(pk, sec, *raw, sec->raw_size) == HP2_OK);
	*n = sec->raw_size;
	return 0;
}

static int near1(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/* Reference: is the world point inside a posed selected shape (own code path: explicit local transform)? Returns the
 * number of shapes containing it and the region of the last one. */
static int ref_inside(const cmod_model *m, const mat4 *w, const float p[3], int *region)
{
	uint32_t i;
	int n = 0;
	for (i = 0; i < m->shape_count; i++) {
		cmod_shape s = cmod_get_shape(m, i);
		const mat4 *M = &w[s.node];
		float d[3], l[3], len2;
		unsigned k, c;
		int in = 1;
		if (s.perm != 0) continue;
		for (k = 0; k < 3; k++) d[k] = p[k] - M->m[3][k];
		for (k = 0; k < 3; k++) {
			len2 = M->m[k][0] * M->m[k][0] + M->m[k][1] * M->m[k][1] + M->m[k][2] * M->m[k][2];
			l[k] = (d[0] * M->m[k][0] + d[1] * M->m[k][1] + d[2] * M->m[k][2]) / len2;
		}
		for (k = 0; k < 3 && in; k++) {
			float u = 0.0f;
			for (c = 0; c < 3; c++) u += s.axes[k][c] * (l[c] - s.center[c]);
			if (fabsf(u) > s.half[k]) in = 0;
		}
		if (in) { n++; *region = s.region; }
	}
	return n;
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
	uint8_t *raw, *raw1;
	size_t rawn, rawn1;
	if (load_section(&pk, "CMOD", &raw, &rawn) || load_section(&pk, "CMD1", &raw1, &rawn1)) return 1;

	cmod_section s;
	CHECK(cmod_open(&s, raw, rawn) == CMOD_OK);
	CHECK(s.model_count == 3 && s.object_count == 4 && s.material_count == 6 && s.region_count == 6 && s.perm_count == 8);
	CHECK(s.node_count == 7 && s.shape_count == 9);
	CHECK(((uintptr_t)s.models | (uintptr_t)s.objects | (uintptr_t)s.materials | (uintptr_t)s.regions | (uintptr_t)s.perms |
	       (uintptr_t)s.nodes | (uintptr_t)s.shapes) % 16 == (uintptr_t)raw % 16);

	/* lookups: by collision tag id and by object tag id */
	cmod_model hero, cart, box, m2;
	cmod_object o;
	CHECK(cmod_find(&s, HERO_TAG, &hero) && cmod_find(&s, CART_TAG, &cart) && cmod_find(&s, BOX_TAG, &box));
	CHECK(hero.index == 0 && cart.index == 1 && box.index == 2);
	CHECK(!cmod_find(&s, 0, &m2) && m2.tag_id == 0 && !cmod_find(&s, 0xFFFFFFFFu, &m2) && !cmod_find(&s, HERO_TAG + 1, &m2));
	CHECK(cmod_find_object(&s, BIPD_HERO, &o, &m2) && m2.tag_id == HERO_TAG && o.coll_tag == HERO_TAG && o.model_index == 0 && o.type == 0);
	CHECK(near1(o.radius, 0.45f, 1e-6f) && near1(o.offset[0], -0.02f, 1e-6f) && near1(o.render_radius, 0.45f, 1e-6f));
	CHECK(cmod_find_object(&s, BIPD_HERO2, &o, &m2) && m2.tag_id == HERO_TAG && near1(o.origin_offset[2], -0.38f, 1e-6f) && near1(o.offset[2], 0.1f, 1e-6f));
	CHECK(near1(o.render_radius, 2.0f, 1e-6f));
	CHECK(cmod_find_object(&s, SCEN_BOX, &o, NULL) && o.type == 6 && o.model_index == 2 && near1(o.radius, 1.5f, 1e-6f));
	CHECK(cmod_find_object(&s, VEHI_CART, NULL, &m2) && m2.tag_id == CART_TAG);
	CHECK(!cmod_find_object(&s, SCEN_PLAIN, &o, &m2) && o.tag_id == 0 && m2.tag_id == 0);
	CHECK(!cmod_find_object(&s, 0, NULL, NULL) && cmod_find_object(&s, VEHI_CART, NULL, NULL));
	CHECK(!cmod_get_object(&s, 4, &o) && o.tag_id == 0 && !cmod_get_model(&s, 3, &m2));

	/* damage constants */
	CHECK(hero.flags == (CMOD_F_SHIELD_FOR_CHILDREN | CMOD_F_BODY_FOR_CHILDREN | CMOD_F_ALWAYS_SHIELD_FRIEND));
	CHECK(hero.indirect_damage_material == 1 && hero.shield_material == 22);
	CHECK(hero.max_body_vitality == 75.0f && hero.max_shield_vitality == 75.0f && hero.shield_stun_time == 6.0f && hero.shield_recharge_time == 4.0f);
	CHECK(near1(hero.shield_recharge_rate, 1.0f / 120.0f, 1e-9f) && hero.source_vertices == 40);
	CHECK(hero.node_count == 4 && hero.region_count == 3 && hero.material_count == 3 && hero.shape_count == 5);
	CHECK(cart.max_body_vitality == 0.0f && cart.max_shield_vitality == 0.0f && cart.shield_recharge_rate == 1.0f && cart.indirect_damage_material == -1);
	CHECK(cart.flags == (CMOD_F_AREA_TO_CHILDREN | CMOD_F_ONLY_EXPLOSIVES) && cart.shape_count == 3);
	CHECK(box.node_count == 1 && box.shape_count == 1 && box.first_shape == 8);

	/* materials, regions, permutations, nodes, shapes */
	cmod_material mt = cmod_get_material(&hero, 1);
	CHECK(strcmp(mt.name, "head") == 0 && mt.flags == CMOD_MF_HEAD && mt.material_type == 21 && mt.body_multiplier == 2.0f && mt.shield_multiplier == 1.0f);
	mt = cmod_get_material(&hero, 2);
	CHECK(strcmp(mt.name, "limb") == 0 && near1(mt.shield_leak, 0.2f, 1e-6f) && near1(mt.body_multiplier, 0.8f, 1e-6f));
	mt = cmod_get_material(&hero, 3);                                       /* out of range: zeroed */
	CHECK(mt.name[0] == 0 && mt.flags == 0 && mt.body_multiplier == 0.0f);
	cmod_region rg = cmod_get_region(&hero, 1);
	CHECK(strcmp(rg.name, "arm") == 0 && rg.flags == (CMOD_RF_NO_MELEE | CMOD_RF_NO_WEAPON) && rg.perm_count == 2 && rg.destroyed_effect == 0);
	rg = cmod_get_region(&hero, 2);
	CHECK(strcmp(rg.name, "head") == 0 && rg.flags == (CMOD_RF_FORCES_DEATH | CMOD_RF_HEAD_SCREAM) && rg.damage_threshold == 0.25f && rg.perm_count == 1);
	CHECK(cmod_get_region(&hero, 3).name[0] == 0 && cmod_get_region(&hero, 3).perm_count == 0);
	char nm[CMOD_NAME_LEN];
	CHECK(cmod_perm_name(&hero, 1, 1, nm) && strcmp(nm, "~damaged") == 0 && cmod_perm_name(&hero, 1, 0, nm) && strcmp(nm, "__base") == 0);
	CHECK(!cmod_perm_name(&hero, 1, 2, nm) && nm[0] == 0 && !cmod_perm_name(&hero, 3, 0, nm));
	CHECK(cmod_find_permutation(&hero, 1, "~damaged") == 1 && cmod_find_permutation(&hero, 1, "__base") == 0 &&
	      cmod_find_permutation(&hero, 1, "~blur") == -1 && cmod_find_permutation(&hero, 0, "~damaged") == -1 &&
	      cmod_find_permutation(&hero, 9, "__base") == -1 && cmod_find_permutation(&hero, 1, NULL) == -1);
	CHECK(cmod_find_permutation(&cart, 1, "~blur") == 1);
	cmod_node nd = cmod_get_node(&hero, 0);
	CHECK(nd.region == 0 && nd.parent == -1 && nd.first_child == 1 && nd.next_sibling == -1);
	nd = cmod_get_node(&hero, 3);
	CHECK(nd.region == -1 && nd.parent == 0);
	nd = cmod_get_node(&hero, 4);
	CHECK(nd.region == -1 && nd.parent == -1 && nd.first_child == -1);
	static const uint16_t shape_expect[5][4] = { { 0, 0, 0, 0 }, { 1, 1, 0, 2 }, { 1, 1, 1, 0 }, { 2, 2, 0, 1 }, { 2, 2, 0, 2 } };   /* node, region, perm, material */
	for (uint32_t i = 0; i < 5; i++) {
		cmod_shape sh = cmod_get_shape(&hero, i);
		CHECK(sh.node == shape_expect[i][0] && sh.region == shape_expect[i][1] && sh.perm == shape_expect[i][2] && sh.material == shape_expect[i][3]);
		CHECK(sh.source_vertices == 8 && sh.flags == (i == 3 ? CMOD_SF_HEAD : 0u));
		for (unsigned k = 0; k < 3; k++) {
			CHECK(sh.half[k] > 0.0f);
			CHECK(near1(sh.axes[k][0] * sh.axes[k][0] + sh.axes[k][1] * sh.axes[k][1] + sh.axes[k][2] * sh.axes[k][2], 1.0f, 1e-5f));
		}
	}
	{
		cmod_shape root = cmod_get_shape(&hero, 0), arm1 = cmod_get_shape(&hero, 2);
		CHECK(root.axes[0][0] == 1.0f && root.axes[1][1] == 1.0f && near1(root.half[0], 0.102f, 1e-6f) && near1(root.half[1], 0.202f, 1e-6f));
		CHECK(near1(arm1.center[0], 0.05f, 1e-6f) && near1(arm1.half[0], 0.052f, 1e-6f));
		CHECK(cmod_get_shape(&hero, 5).half[0] == 0.0f);                      /* out of range: zeroed */
	}

	/* rays, hand checked: hero posed with the root at the origin, the arm 1 wu along +x, the head 0.5 wu up */
	mat4 w[CMOD_MAX_NODES];
	for (unsigned i = 0; i < CMOD_MAX_NODES; i++) ident(&w[i], 0, 0, 0);
	ident(&w[1], 1.0f, 0.0f, 0.0f);
	ident(&w[2], 0.0f, 0.0f, 0.5f);
	cmod_segment seg = { { -1, 0, 0 }, { 1, 0, 0 } };
	cmod_hit h;
	uint16_t reg = 99;
	float t = -1.0f;
	CHECK(cmod_ray_ex(&hero, w, &seg, NULL, &h) && h.region == 0 && h.node == 0 && h.material == 0 && near1(h.t, 0.449f, 2e-4f));
	CHECK(h.region_flags == 0 && h.body_multiplier == 1.0f && h.shape == 0 && h.perm == 0);
	CHECK(cmod_ray(&hero, w, &seg, &reg, &t) && reg == 0 && near1(t, 0.449f, 2e-4f));
	{
		cmod_segment hs = { { 0, -1, 0.5f }, { 0, 1, 0.5f } };                 /* through the head */
		CHECK(cmod_ray_ex(&hero, w, &hs, NULL, &h) && h.region == 2 && h.node == 2 && h.material == 1 && near1(h.t, 0.474f, 2e-4f));
		CHECK((h.shape_flags & CMOD_SF_HEAD) && h.body_multiplier == 2.0f && h.region_flags == (CMOD_RF_FORCES_DEATH | CMOD_RF_HEAD_SCREAM));
		CHECK(h.shape == 3);
		cmod_segment ins = { { 0, 0, 0 }, { 1, 0, 0 } };                       /* starts inside the root box */
		CHECK(cmod_ray_ex(&hero, w, &ins, NULL, &h) && h.t == 0.0f && h.region == 0);
		cmod_segment pt = { { 0, 0, 0 }, { 0, 0, 0 } };                        /* a point inside; a point outside */
		CHECK(cmod_ray_ex(&hero, w, &pt, NULL, &h) && h.t == 0.0f);
		pt.a[0] = pt.b[0] = 5.0f;
		CHECK(!cmod_ray_ex(&hero, w, &pt, NULL, &h));
		cmod_segment miss = { { -1, 5, 0 }, { 1, 5, 0 } };
		CHECK(!cmod_ray_ex(&hero, w, &miss, NULL, &h) && !cmod_ray(&hero, w, &miss, &reg, &t));
		cmod_segment short_ = { { -1, 0, 0 }, { -0.5f, 0, 0 } };               /* stops before the box */
		CHECK(!cmod_ray_ex(&hero, w, &short_, NULL, &h));
		cmod_segment bad = { { NAN, 0, 0 }, { 1, 0, 0 } };
		CHECK(!cmod_ray_ex(&hero, w, &bad, NULL, &h));
		bad.a[0] = 0.0f; bad.b[2] = INFINITY;
		CHECK(!cmod_ray_ex(&hero, w, &bad, NULL, &h));
		CHECK(!cmod_ray_ex(&hero, NULL, &seg, NULL, &h) && !cmod_ray_ex(&hero, w, NULL, NULL, &h));
		CHECK(cmod_ray_ex(&hero, w, &seg, NULL, NULL));                       /* hit may be NULL */
	}
	{
		/* the arm: a vertical ray through the middle of the turned box; permutation 1 is a small box, the ray misses it */
		cmod_segment down = { { 1.1299f, 0.075f, 1.0f }, { 1.1299f, 0.075f, -1.0f } };
		uint8_t sel[CMOD_MAX_REGIONS];
		memset(sel, 0, sizeof sel);
		CHECK(cmod_ray_ex(&hero, w, &down, NULL, &h) && h.region == 1 && h.node == 1 && h.perm == 0 && h.material == 2);
		CHECK(h.t > 0.45f && h.t < 0.48f && near1(h.shield_leak, 0.2f, 1e-6f) && near1(h.body_multiplier, 0.8f, 1e-6f));
		CHECK(h.region_flags == (CMOD_RF_NO_MELEE | CMOD_RF_NO_WEAPON));
		CHECK(cmod_ray_ex(&hero, w, &down, sel, &h) && h.perm == 0);
		sel[1] = 1;
		CHECK(!cmod_ray_ex(&hero, w, &down, sel, &h));
		sel[1] = 7;                                                             /* past the region's permutations: 0 */
		CHECK(cmod_ray_ex(&hero, w, &down, sel, &h) && h.perm == 0);
		cmod_segment small = { { 1.05f, 0.0f, 1.0f }, { 1.05f, 0.0f, -1.0f } };  /* local (0.05, 0, z): inside the small box too */
		sel[1] = 1;
		CHECK(cmod_ray_ex(&hero, w, &small, sel, &h) && h.perm == 1 && h.material == 0 && near1(h.t, 0.474f, 2e-4f));
	}
	{
		/* a non-uniform pose: scale 2 about the node origin halves the t of the first hit */
		mat4 w2[CMOD_MAX_NODES];
		memcpy(w2, w, sizeof w2);
		for (unsigned k = 0; k < 3; k++) for (unsigned c = 0; c < 3; c++) w2[0].m[k][c] *= 2.0f;
		CHECK(cmod_ray_ex(&hero, w2, &seg, NULL, &h) && h.node == 0 && near1(h.t, 0.398f, 2e-4f));
		{
			cmod_segment s2 = { { -1, 0, 0 }, { 0.5f, 0, 0 } };                /* meets only the root box */
			memcpy(w2, w, sizeof w2);
			CHECK(cmod_ray_ex(&hero, w2, &s2, NULL, &h) && h.node == 0);
			memset(&w2[0], 0, sizeof w2[0]);                                   /* degenerate matrix: the node's shapes vanish */
			CHECK(!cmod_ray_ex(&hero, w2, &s2, NULL, &h));
		}
	}
	/* spheres */
	{
		const float c[3] = { 0, 0, 0.5f };
		uint64_t mask = 77;
		CHECK(cmod_sphere(&hero, w, c, 0.01f, NULL, &mask) == 1 && mask == (1ull << 2));
		CHECK(cmod_sphere(&hero, w, c, 0.3f, NULL, &mask) == 2 && mask == (1ull << 2));
		CHECK(cmod_sphere(&hero, w, c, 0.45f, NULL, &mask) == 3 && mask == ((1ull << 0) | (1ull << 2)));
		CHECK(cmod_sphere(&hero, w, c, 5.0f, NULL, &mask) == 4 && mask == 7);        /* all permutation 0 shapes: 4 (not the arm's 2nd) */
		CHECK(cmod_sphere(&hero, w, c, -1.0f, NULL, &mask) == 0 && mask == 0);
		CHECK(cmod_sphere(&hero, w, c, 0.45f, NULL, NULL) == 3);
	}
	/* cart and box */
	{
		mat4 cw[CMOD_MAX_NODES];
		uint8_t sel[CMOD_MAX_REGIONS];
		for (unsigned i = 0; i < CMOD_MAX_NODES; i++) ident(&cw[i], 0, 0, 0);
		ident(&cw[1], 0.0f, 0.7f, 0.0f);
		cmod_segment a = { { 0, -2, 0.3f }, { 0, 2, 0.3f } };
		CHECK(cmod_ray_ex(&cart, cw, &a, NULL, &h) && h.region == 0 && near1(h.t, 0.3745f, 2e-4f) && h.region_flags == CMOD_RF_DIES_WITH_OBJECT);
		CHECK(h.body_multiplier == 0.0f);                                       /* not authored on vehicles */
		cmod_segment b = { { -2, 0.7f, 0 }, { 2, 0.7f, 0 } };
		memset(sel, 0, sizeof sel);
		CHECK(cmod_ray_ex(&cart, cw, &b, sel, &h) && h.region == 1 && h.perm == 0 && near1(h.t, 0.4495f, 2e-4f));
		sel[1] = 1;
		CHECK(cmod_ray_ex(&cart, cw, &b, sel, &h) && h.perm == 1 && near1(h.t, 0.4370f, 2e-4f));
		cmod_segment top = { { 0, 0, 5 }, { 0, 0, -5 } };
		mat4 bw[CMOD_MAX_NODES];
		ident(&bw[0], 0, 0, 0);
		CHECK(cmod_ray(&box, bw, &top, &reg, &t) && reg == 0 && near1(t, 0.2998f, 2e-4f));
		ident(&bw[0], 0, 0, 10.0f);                                             /* moved up: the box now spans z 10 .. 12 */
		CHECK(!cmod_ray(&box, bw, &top, &reg, &t));
		top.a[2] = 20.0f;
		CHECK(cmod_ray(&box, bw, &top, &reg, &t) && near1(t, (20.0f - 12.002f) / 25.0f, 2e-4f));
	}

	/* random posed segments against an independent reference: where dense sampling first finds a point inside a posed
	 * shape, the ray must have hit no later (no false negative beyond a sample); every reported hit point must lie on
	 * the boundary of the shape reported (no false positive), or inside it for a ray that starts inside */
	{
		enum { SAMPLES = 600, RUNS = 4000 };
		int mismatch = 0, hits = 0, region_checked = 0, graze = 0, boundary_checked = 0;
		for (int run = 0; run < RUNS; run++) {
			mat4 pw[CMOD_MAX_NODES];
			cmod_segment sg;
			float scale = (run & 1) ? 1.0f : 1.3f;
			for (unsigned i = 0; i < hero.node_count; i++) random_pose(&pw[i], scale);
			{
				/* aim at a random point near a random node, from a random point, and run past it */
				const mat4 *tn = &pw[rnd() % hero.node_count];
				float ext = frand(0.8f, 2.0f);
				for (unsigned k = 0; k < 3; k++) {
					float target = tn->m[3][k] + frand(-0.3f, 0.3f);
					sg.a[k] = frand(-2.0f, 2.0f);
					sg.b[k] = sg.a[k] + (target - sg.a[k]) * ext;
				}
			}
			int found = cmod_ray_ex(&hero, pw, &sg, NULL, &h);
			int first = -1, region = -1;
			for (int i = 0; i <= SAMPLES; i++) {
				float p[3], u = (float)i / SAMPLES;
				int r2 = -1;
				for (unsigned k = 0; k < 3; k++) p[k] = sg.a[k] + (sg.b[k] - sg.a[k]) * u;
				int cnt = ref_inside(&hero, pw, p, &r2);
				if (cnt) { first = i; region = cnt == 1 ? r2 : -1; break; }
			}
			if (first >= 0) {
				float tm = (float)first / SAMPLES;
				hits++;
				if (!found || h.t > tm + 1e-4f) { mismatch++; fprintf(stderr, "run %d: found %d t %f sampled %f\n", run, found, found ? h.t : -1.0f, tm); }
				else if (region >= 0 && first > 0 && h.t >= tm - 1.0f / SAMPLES && h.region != region) {
					mismatch++;
					fprintf(stderr, "run %d: region %d, sampled %d\n", run, h.region, region);
				} else if (region >= 0 && first > 0 && h.t >= tm - 1.0f / SAMPLES) region_checked++;
			} else if (found) {
				graze++;                                                          /* a chord shorter than a sample */
			}
			if (found) {
				cmod_shape sh = cmod_get_shape(&hero, h.shape);
				const mat4 *M = &pw[sh.node];
				float p[3], d[3], worst = -1e30f;
				for (unsigned k = 0; k < 3; k++) p[k] = sg.a[k] + (sg.b[k] - sg.a[k]) * h.t;
				for (unsigned k = 0; k < 3; k++) d[k] = p[k] - M->m[3][k];
				for (unsigned k = 0; k < 3; k++) {
					float l[3], u = 0.0f;
					for (unsigned a = 0; a < 3; a++) {
						float l2 = M->m[a][0] * M->m[a][0] + M->m[a][1] * M->m[a][1] + M->m[a][2] * M->m[a][2];
						l[a] = (d[0] * M->m[a][0] + d[1] * M->m[a][1] + d[2] * M->m[a][2]) / l2;
					}
					for (unsigned a = 0; a < 3; a++) u += sh.axes[k][a] * (l[a] - sh.center[a]);
					if (fabsf(u) - sh.half[k] > worst) worst = fabsf(u) - sh.half[k];
				}
				boundary_checked++;
				/* worst < 0: inside; the hit point is on the surface (worst ~ 0) unless the segment starts inside */
				if (worst > 1e-3f || (h.t > 0.0f && worst < -1e-3f)) { mismatch++; fprintf(stderr, "run %d: hit point %f outside/inside the box by %f (t %f)\n", run, worst, worst, h.t); }
			}
		}
		fprintf(stderr, "random rays: %d sampled hits, %d regions compared, %d boundary checks, %d chords below a sample, %d mismatches\n",
		        hits, region_checked, boundary_checked, graze, mismatch);
		CHECK(hits > 500 && mismatch == 0 && graze < 40);
	}

	/* an empty section opens and answers "not found" */
	{
		cmod_section e;
		cmod_model m;
		CHECK(cmod_open(&e, raw1, rawn1) == CMOD_OK && e.model_count == 0 && e.object_count == 0 && e.shape_count == 0);
		CHECK(!cmod_find(&e, HERO_TAG, &m) && !cmod_find_object(&e, BIPD_HERO, NULL, NULL));
		walk(&e, 40);
	}
	walk(&s, 40);

	/* a failed open leaves the section zeroed: every accessor reports "not found" */
	{
		cmod_section z;
		cmod_model m;
		cmod_object ob;
		uint8_t bad[64] = { 0 };
		CHECK(cmod_open(&z, bad, sizeof bad) == CMOD_ERR_TRUNCATED && z.data == NULL && z.model_count == 0);
		CHECK(cmod_open(&z, NULL, 1000) == CMOD_ERR_TRUNCATED);
		CHECK(!cmod_find(&z, HERO_TAG, &m) && !cmod_find_object(&z, BIPD_HERO, &ob, &m) && !cmod_get_model(&z, 0, &m));
		walk(&z, 40);
		CHECK(cmod_open(&z, raw, 4) == CMOD_ERR_TRUNCATED && cmod_open(&z, raw, 127) == CMOD_ERR_TRUNCATED);
	}

	/* header corruption: each is rejected with the right status */
	{
		cmod_section z;
		uint8_t *c = malloc(rawn);
		CHECK(c);
		memcpy(c, raw, rawn);
		c[0] = 'X';
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_MAGIC);
		memcpy(c, raw, rawn);
		c[4] = 2;
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_VERSION);
		memcpy(c, raw, rawn);
		c[8]++;                                                                  /* size field larger than the buffer */
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_TRUNCATED);
		memcpy(c, raw, rawn);
		c[8] -= 2;                                                               /* smaller */
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_LAYOUT);
		memcpy(c, raw, rawn);
		c[0x0C + 3] = 0x40;                                                      /* a huge model count */
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_LAYOUT);
		memcpy(c, raw, rawn);
		c[0x28 + 5] = 0xFF;                                                      /* a table offset beyond the section */
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_LAYOUT);
		memcpy(c, raw, rawn);
		c[0x28]++;                                                               /* a misaligned table */
		CHECK(cmod_open(&z, c, rawn) == CMOD_ERR_LAYOUT);
		free(c);
	}

	/* truncation at every length, then every byte flipped three ways: rejected, or safe to walk */
	{
		uint8_t *c = malloc(rawn);
		size_t i;
		int opened = 0, rejected = 0;
		CHECK(c);
		for (i = 0; i < rawn; i++) {
			cmod_section z;
			memcpy(c, raw, i);
			CHECK(cmod_open(&z, c, i) != CMOD_OK);
		}
		for (i = 0; i < rawn; i++) {
			static const uint8_t flips[3] = { 0x00, 0xFF, 0x80 };
			for (unsigned v = 0; v < 3; v++) {
				cmod_section z;
				memcpy(c, raw, rawn);
				c[i] = v == 2 ? (uint8_t)(c[i] ^ 0x80) : flips[v];
				if (cmod_open(&z, c, rawn) == CMOD_OK) { opened++; walk(&z, 3); } else rejected++;
			}
		}
		fprintf(stderr, "byte flips: %d rejected, %d opened and walked\n", rejected, opened);
		CHECK(rejected > 200 && opened > 0);
		free(c);
	}
	free(raw);
	free(raw1);
	free(img);
	printf("ok\n");
	return 0;
}
