/* CMOD section reader (docs/16-cmod-section.md). Pure C11, no heap, no double. cmod_open validates every table once
 * (sizes, alignment, ranges, sort order, cross references, string terminators, finite floats, unit axes), so the
 * accessors can never read out of bounds whatever the bytes. Records are decoded with byte loads: no alignment
 * assumptions on the section buffer. */
#include "world/cmod.h"
#include <string.h>

#define HEADER_SIZE 128u
#define MODEL_REC 96u
#define OBJECT_REC 48u
#define MATERIAL_REC 48u
#define REGION_REC 48u
#define PERM_REC 32u
#define NODE_REC 8u
#define SHAPE_REC 96u
#define CMOD_VERSION 1u
#define MAX_MODELS 0xFFFFu
#define MAX_SHAPES_PER_MODEL 0xFFFFu
#define TABLES 7u        /* models, objects, materials, regions, perms, nodes, shapes: counts at 0x0C, offsets at 0x28 */

static const uint32_t rec_size[TABLES] = { MODEL_REC, OBJECT_REC, MATERIAL_REC, REGION_REC, PERM_REC, NODE_REC, SHAPE_REC };

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

static const uint8_t *model_rec(const cmod_section *s, uint32_t i) { return s->models + (size_t)i * MODEL_REC; }
static const uint8_t *region_rec(const cmod_section *s, uint32_t i) { return s->regions + (size_t)i * REGION_REC; }
static const uint8_t *node_rec(const cmod_section *s, uint32_t i) { return s->nodes + (size_t)i * NODE_REC; }
static const uint8_t *shape_rec(const cmod_section *s, uint32_t i) { return s->shapes + (size_t)i * SHAPE_REC; }

cmod_status cmod_open(cmod_section *s, const uint8_t *data, size_t size)
{
	uint32_t cnt[TABLES], off[TABLES], t, i, k;
	uint32_t next_mat = 0, next_reg = 0, next_node = 0, next_shape = 0, next_perm = 0;
	uint32_t prev_tag = 0;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return CMOD_ERR_TRUNCATED;
	if (memcmp(data, "CMOD", 4) != 0) return CMOD_ERR_MAGIC;
	if (rd16(data + 4) != CMOD_VERSION) return CMOD_ERR_VERSION;
	if (rd32(data + 8) > size) return CMOD_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return CMOD_ERR_LAYOUT;
	if (!finite_at(data + 0x44)) return CMOD_ERR_LAYOUT;                     /* the fit margin */
	for (t = 0; t < TABLES; t++) {
		cnt[t] = rd32(data + 0x0C + 4 * t);
		off[t] = rd32(data + 0x28 + 4 * t);
		if (!table_ok(off[t], cnt[t], rec_size[t], size)) return CMOD_ERR_LAYOUT;
	}
	if (cnt[0] > MAX_MODELS || cnt[3] > 0xFFFFu || cnt[4] > 0xFFFFu) return CMOD_ERR_LAYOUT;   /* region / permutation numbers are u16 */
	s->data = data;
	s->size = size;
	s->model_count = cnt[0];
	s->object_count = cnt[1];
	s->material_count = cnt[2];
	s->region_count = cnt[3];
	s->perm_count = cnt[4];
	s->node_count = cnt[5];
	s->shape_count = cnt[6];
	s->models = data + off[0];
	s->objects = data + off[1];
	s->materials = data + off[2];
	s->regions = data + off[3];
	s->perms = data + off[4];
	s->nodes = data + off[5];
	s->shapes = data + off[6];

	/* leaf tables first: names terminated, floats finite */
	for (i = 0; i < s->material_count; i++) {
		const uint8_t *p = s->materials + (size_t)i * MATERIAL_REC;
		if (!terminated(p, CMOD_NAME_LEN) || !finite_at(p + 0x24) || !finite_at(p + 0x28) || !finite_at(p + 0x2C)) goto bad;
	}
	for (i = 0; i < s->region_count; i++) {
		const uint8_t *p = region_rec(s, i);
		if (!terminated(p, CMOD_NAME_LEN) || !finite_at(p + 0x24)) goto bad;
		if ((uint32_t)rd16(p + 0x2C) + rd16(p + 0x2E) > s->perm_count || rd16(p + 0x2E) > CMOD_MAX_PERMS) goto bad;
	}
	for (i = 0; i < s->perm_count; i++)
		if (!terminated(s->perms + (size_t)i * PERM_REC, CMOD_NAME_LEN)) goto bad;
	for (i = 0; i < s->shape_count; i++) {
		const uint8_t *p = shape_rec(s, i);
		for (k = 0; k < 3; k++) {
			unsigned c;
			float len2 = 0.0f;
			if (!finite_at(p + 0x10 + 4 * k) || !finite_at(p + 0x20 + 4 * k)) goto bad;
			if (rd32(p + 0x20 + 4 * k) & 0x80000000u) goto bad;                     /* half extent >= 0 */
			for (c = 0; c < 3; c++) {
				float v;
				if (!finite_at(p + 0x30 + 16 * k + 4 * c)) goto bad;
				v = rdf(p + 0x30 + 16 * k + 4 * c);
				len2 += v * v;
			}
			if (!(len2 > 0.99f && len2 < 1.01f)) goto bad;                           /* unit axes */
		}
	}

	/* models: sorted, partitioning the tables in order, internally consistent */
	for (i = 0; i < s->model_count; i++) {
		const uint8_t *m = model_rec(s, i);
		uint32_t tag = rd32(m);
		uint32_t nn = rd16(m + 0x0C), nr = rd16(m + 0x0E), nm = rd16(m + 0x10);
		uint32_t f_mat = rd32(m + 0x14), f_reg = rd32(m + 0x18), f_node = rd32(m + 0x1C), f_shape = rd32(m + 0x20), ns = rd32(m + 0x24);
		if (i && tag <= prev_tag) goto bad;
		prev_tag = tag;
		if (nn > CMOD_MAX_NODES || nr > CMOD_MAX_REGIONS || nm > CMOD_MAX_MATERIALS || ns > MAX_SHAPES_PER_MODEL) goto bad;
		if (f_mat != next_mat || f_reg != next_reg || f_node != next_node || f_shape != next_shape) goto bad;
		if ((uint64_t)f_mat + nm > s->material_count || (uint64_t)f_reg + nr > s->region_count ||
		    (uint64_t)f_node + nn > s->node_count || (uint64_t)f_shape + ns > s->shape_count) goto bad;
		for (k = 0; k < 7; k++)
			if (!finite_at(m + 0x28 + 4 * k)) goto bad;
		for (k = 0; k < nr; k++) {
			const uint8_t *rg = region_rec(s, f_reg + k);
			if (rd16(rg + 0x2C) != next_perm) goto bad;                              /* permutations partition in region order */
			next_perm += rd16(rg + 0x2E);
		}
		for (k = 0; k < nn; k++) {
			const uint8_t *nd = node_rec(s, f_node + k);
			int16_t reg = (int16_t)rd16(nd), link;
			unsigned j;
			if (reg < -1 || reg >= (int)nr) goto bad;
			for (j = 1; j < 4; j++) {
				link = (int16_t)rd16(nd + 2 * j);
				if (link < -1 || link >= (int)nn) goto bad;
			}
		}
		for (k = 0; k < ns; k++) {
			const uint8_t *sh = shape_rec(s, f_shape + k);
			uint32_t node = rd16(sh), reg = rd16(sh + 2), perm = rd16(sh + 4), mat = rd16(sh + 6);
			if (node >= nn || reg >= nr || mat >= nm) goto bad;
			if ((int16_t)rd16(node_rec(s, f_node + node)) != (int)reg) goto bad;     /* a shape's region is its node's region */
			if (perm >= rd16(region_rec(s, f_reg + reg) + 0x2E)) goto bad;
		}
		next_mat += nm;
		next_reg += nr;
		next_node += nn;
		next_shape += ns;
	}
	if (next_mat != s->material_count || next_reg != s->region_count || next_node != s->node_count ||
	    next_shape != s->shape_count || next_perm != s->perm_count)
		goto bad;

	prev_tag = 0;
	for (i = 0; i < s->object_count; i++) {
		const uint8_t *o = s->objects + (size_t)i * OBJECT_REC;
		uint32_t tag = rd32(o), mi = rd16(o + 8);
		if (i && tag <= prev_tag) goto bad;
		prev_tag = tag;
		if (mi >= s->model_count || rd32(model_rec(s, mi)) != rd32(o + 4)) goto bad;
		for (k = 0; k < 8; k++)
			if (!finite_at(o + 0x0C + 4 * k)) goto bad;
		if (rd32(o + 0x0C) & 0x80000000u) goto bad;                                 /* radius >= 0 */
	}
	return CMOD_OK;
bad:
	memset(s, 0, sizeof *s);
	return CMOD_ERR_LAYOUT;
}

static void decode_model(const cmod_section *s, uint32_t index, cmod_model *out)
{
	const uint8_t *m = model_rec(s, index);
	out->sec = s;
	out->index = index;
	out->tag_id = rd32(m);
	out->flags = rd32(m + 4);
	out->indirect_damage_material = (int16_t)rd16(m + 8);
	out->shield_material = rd16(m + 0x0A);
	out->node_count = rd16(m + 0x0C);
	out->region_count = rd16(m + 0x0E);
	out->material_count = rd16(m + 0x10);
	out->first_material = rd32(m + 0x14);
	out->first_region = rd32(m + 0x18);
	out->first_node = rd32(m + 0x1C);
	out->first_shape = rd32(m + 0x20);
	out->shape_count = rd32(m + 0x24);
	out->max_body_vitality = rdf(m + 0x28);
	out->body_system_shock = rdf(m + 0x2C);
	out->body_destroyed_threshold = rdf(m + 0x30);
	out->max_shield_vitality = rdf(m + 0x34);
	out->shield_stun_time = rdf(m + 0x38);
	out->shield_recharge_time = rdf(m + 0x3C);
	out->shield_recharge_rate = rdf(m + 0x40);
	out->source_vertices = rd32(m + 0x44);
}

int cmod_get_model(const cmod_section *s, uint32_t index, cmod_model *out)
{
	if (!s->data || index >= s->model_count) {
		memset(out, 0, sizeof *out);
		return 0;
	}
	decode_model(s, index, out);
	return 1;
}

int cmod_find(const cmod_section *s, uint32_t coll_tag_id, cmod_model *out)
{
	uint32_t lo = 0, hi = s->model_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(model_rec(s, mid));
		if (t == coll_tag_id) return cmod_get_model(s, mid, out);
		if (t < coll_tag_id) lo = mid + 1;
		else hi = mid;
	}
	memset(out, 0, sizeof *out);
	return 0;
}

int cmod_get_object(const cmod_section *s, uint32_t index, cmod_object *out)
{
	unsigned k;
	memset(out, 0, sizeof *out);
	if (!s->data || index >= s->object_count) return 0;
	{
		const uint8_t *o = s->objects + (size_t)index * OBJECT_REC;
		out->tag_id = rd32(o);
		out->coll_tag = rd32(o + 4);
		out->model_index = rd16(o + 8);
		out->type = o[0x0A];
		out->radius = rdf(o + 0x0C);
		for (k = 0; k < 3; k++) {
			out->offset[k] = rdf(o + 0x10 + 4 * k);
			out->origin_offset[k] = rdf(o + 0x1C + 4 * k);
		}
		out->render_radius = rdf(o + 0x28);
	}
	return 1;
}

int cmod_find_object(const cmod_section *s, uint32_t object_tag_id, cmod_object *obj, cmod_model *model)
{
	uint32_t lo = 0, hi = s->object_count;
	cmod_object tmp;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->objects + (size_t)mid * OBJECT_REC);
		if (t == object_tag_id) {
			cmod_get_object(s, mid, &tmp);
			if (obj) *obj = tmp;
			if (model) return cmod_get_model(s, tmp.model_index, model);
			return 1;
		}
		if (t < object_tag_id) lo = mid + 1;
		else hi = mid;
	}
	if (obj) memset(obj, 0, sizeof *obj);
	if (model) memset(model, 0, sizeof *model);
	return 0;
}

cmod_material cmod_get_material(const cmod_model *m, uint32_t index)
{
	cmod_material r;
	memset(&r, 0, sizeof r);
	if (m->sec && m->sec->data && index < m->material_count) {
		const uint8_t *p = m->sec->materials + ((size_t)m->first_material + index) * MATERIAL_REC;
		memcpy(r.name, p, CMOD_NAME_LEN);
		r.name[CMOD_NAME_LEN - 1] = 0;
		r.flags = rd16(p + 0x20);
		r.material_type = rd16(p + 0x22);
		r.shield_leak = rdf(p + 0x24);
		r.shield_multiplier = rdf(p + 0x28);
		r.body_multiplier = rdf(p + 0x2C);
	}
	return r;
}

cmod_region cmod_get_region(const cmod_model *m, uint32_t index)
{
	cmod_region r;
	memset(&r, 0, sizeof r);
	if (m->sec && m->sec->data && index < m->region_count) {
		const uint8_t *p = region_rec(m->sec, m->first_region + index);
		memcpy(r.name, p, CMOD_NAME_LEN);
		r.name[CMOD_NAME_LEN - 1] = 0;
		r.flags = rd32(p + 0x20);
		r.damage_threshold = rdf(p + 0x24);
		r.destroyed_effect = rd32(p + 0x28);
		r.first_perm = rd16(p + 0x2C);
		r.perm_count = rd16(p + 0x2E);
	}
	return r;
}

cmod_node cmod_get_node(const cmod_model *m, uint32_t index)
{
	cmod_node r;
	memset(&r, 0, sizeof r);
	r.region = r.parent = r.first_child = r.next_sibling = -1;
	if (m->sec && m->sec->data && index < m->node_count) {
		const uint8_t *p = node_rec(m->sec, m->first_node + index);
		r.region = (int16_t)rd16(p);
		r.parent = (int16_t)rd16(p + 2);
		r.first_child = (int16_t)rd16(p + 4);
		r.next_sibling = (int16_t)rd16(p + 6);
	}
	return r;
}

static void decode_shape(const uint8_t *p, cmod_shape *r)
{
	unsigned k, c;
	r->node = rd16(p);
	r->region = rd16(p + 2);
	r->perm = rd16(p + 4);
	r->material = rd16(p + 6);
	r->flags = rd16(p + 8);
	r->source_vertices = rd32(p + 0x0C);
	for (k = 0; k < 3; k++) {
		r->center[k] = rdf(p + 0x10 + 4 * k);
		r->half[k] = rdf(p + 0x20 + 4 * k);
		for (c = 0; c < 3; c++) r->axes[k][c] = rdf(p + 0x30 + 16 * k + 4 * c);
	}
}

cmod_shape cmod_get_shape(const cmod_model *m, uint32_t index)
{
	cmod_shape r;
	memset(&r, 0, sizeof r);
	if (m->sec && m->sec->data && index < m->shape_count) decode_shape(shape_rec(m->sec, m->first_shape + index), &r);
	return r;
}

int cmod_perm_name(const cmod_model *m, uint32_t region, uint32_t perm, char *out)
{
	out[0] = 0;
	if (m->sec && m->sec->data && region < m->region_count) {
		cmod_region rg = cmod_get_region(m, region);
		if (perm < rg.perm_count) {
			memcpy(out, m->sec->perms + ((size_t)rg.first_perm + perm) * PERM_REC, CMOD_NAME_LEN);
			out[CMOD_NAME_LEN - 1] = 0;
			return 1;
		}
	}
	return 0;
}

int cmod_find_permutation(const cmod_model *m, uint32_t region, const char *name)
{
	uint32_t k;
	char buf[CMOD_NAME_LEN];
	cmod_region rg;
	if (!name || !m->sec || !m->sec->data || region >= m->region_count) return -1;
	rg = cmod_get_region(m, region);
	for (k = 0; k < rg.perm_count; k++) {
		cmod_perm_name(m, region, k, buf);
		if (strcmp(buf, name) == 0) return (int)k;
	}
	return -1;
}

/* ---- posed shapes ------------------------------------------------------------------------------------------------- */

/* The matrix rows are the images of the basis vectors (core/mat.h): world = x*m[0] + y*m[1] + z*m[2] + m[3]. For a
 * rotation times a uniform scale the node-local point is (p - m[3]) . m[i] / |m[i]|^2 (inv_lengths: 0 for a degenerate matrix). */
static void to_local(const mat4 *m, const float p[3], float out[3], const float inv2[3])
{
	float d[3];
	unsigned i;
	for (i = 0; i < 3; i++) d[i] = p[i] - m->m[3][i];
	for (i = 0; i < 3; i++) out[i] = (d[0] * m->m[i][0] + d[1] * m->m[i][1] + d[2] * m->m[i][2]) * inv2[i];
}

static int inv_lengths(const mat4 *m, float inv2[3])
{
	unsigned i;
	for (i = 0; i < 3; i++) {
		float l2 = m->m[i][0] * m->m[i][0] + m->m[i][1] * m->m[i][1] + m->m[i][2] * m->m[i][2];
		if (!(l2 > 1e-12f)) return 0;                 /* also rejects NaN */
		inv2[i] = 1.0f / l2;
	}
	return 1;
}

static int selected(const cmod_section *s, const cmod_model *m, uint32_t region, uint32_t perm, const uint8_t *perm_sel)
{
	uint32_t want = perm_sel ? perm_sel[region] : 0u;
	if (want >= rd16(region_rec(s, m->first_region + region) + 0x2E)) want = 0;
	return perm == want;
}

/* Segment a + t d (t in [0,1]) against a box given by centre, half extents and unit axes, all in the same frame. */
static int seg_box(const float a[3], const float d[3], const cmod_shape *s, float *t_out)
{
	float tmin = 0.0f, tmax = 1.0f;
	unsigned k;
	for (k = 0; k < 3; k++) {
		const float *ax = s->axes[k];
		float oa = ax[0] * (a[0] - s->center[0]) + ax[1] * (a[1] - s->center[1]) + ax[2] * (a[2] - s->center[2]);
		float od = ax[0] * d[0] + ax[1] * d[1] + ax[2] * d[2];
		float h = s->half[k];
		if (od > -1e-12f && od < 1e-12f) {
			if (oa > h || oa < -h) return 0;
		} else {
			float inv = 1.0f / od, t1 = (-h - oa) * inv, t2 = (h - oa) * inv, tmp;
			if (t1 > t2) { tmp = t1; t1 = t2; t2 = tmp; }
			if (t1 > tmin) tmin = t1;
			if (t2 < tmax) tmax = t2;
			if (tmin > tmax) return 0;
		}
	}
	*t_out = tmin;
	return 1;
}

static int finite_f(float v) { return v - v == 0.0f; }

int cmod_ray_ex(const cmod_model *m, const mat4 *node_world, const cmod_segment *seg, const uint8_t *perm_sel, cmod_hit *hit)
{
	const cmod_section *s = m->sec;
	uint32_t i;
	int found = 0;
	float best = 2.0f;
	uint32_t best_i = 0;
	unsigned k;
	if (!s || !s->data || !node_world || !seg) return 0;
	for (k = 0; k < 3; k++)
		if (!finite_f(seg->a[k]) || !finite_f(seg->b[k])) return 0;
	for (i = 0; i < m->shape_count; i++) {
		const uint8_t *p = shape_rec(s, m->first_shape + i);
		uint32_t node = rd16(p), region = rd16(p + 2), perm = rd16(p + 4);
		float inv2[3], la[3], lb[3], d[3], t;
		cmod_shape sh;
		if (!selected(s, m, region, perm, perm_sel)) continue;
		if (!inv_lengths(&node_world[node], inv2)) continue;
		to_local(&node_world[node], seg->a, la, inv2);
		to_local(&node_world[node], seg->b, lb, inv2);
		for (k = 0; k < 3; k++) d[k] = lb[k] - la[k];
		decode_shape(p, &sh);
		if (seg_box(la, d, &sh, &t) && t < best) {
			best = t;
			best_i = i;
			found = 1;
		}
	}
	if (found && hit) {
		cmod_shape sh = cmod_get_shape(m, best_i);
		cmod_region rg = cmod_get_region(m, sh.region);
		cmod_material mt = cmod_get_material(m, sh.material);
		hit->t = best;
		hit->region = sh.region;
		hit->node = sh.node;
		hit->perm = sh.perm;
		hit->material = sh.material;
		hit->shape_flags = sh.flags;
		hit->shape = best_i;
		hit->region_flags = rg.flags;
		hit->body_multiplier = mt.body_multiplier;
		hit->shield_multiplier = mt.shield_multiplier;
		hit->shield_leak = mt.shield_leak;
	}
	return found;
}

int cmod_ray(const cmod_model *m, const mat4 *node_world, const cmod_segment *seg, uint16_t *out_region, float *out_t)
{
	cmod_hit h;
	if (!cmod_ray_ex(m, node_world, seg, NULL, &h)) return 0;
	if (out_region) *out_region = h.region;
	if (out_t) *out_t = h.t;
	return 1;
}

uint32_t cmod_sphere(const cmod_model *m, const mat4 *node_world, const float c[3], float radius,
                     const uint8_t *perm_sel, uint64_t *regions)
{
	const cmod_section *s = m->sec;
	uint32_t i, n = 0;
	unsigned k;
	uint64_t mask = 0;
	if (regions) *regions = 0;
	if (!s || !s->data || !node_world || !c || !finite_f(radius) || radius < 0.0f) return 0;
	for (k = 0; k < 3; k++)
		if (!finite_f(c[k])) return 0;
	for (i = 0; i < m->shape_count; i++) {
		const uint8_t *p = shape_rec(s, m->first_shape + i);
		uint32_t node = rd16(p), region = rd16(p + 2), perm = rd16(p + 4);
		float inv2[3], lc[3], r2, dist2 = 0.0f;
		cmod_shape sh;
		if (!selected(s, m, region, perm, perm_sel)) continue;
		if (!inv_lengths(&node_world[node], inv2)) continue;
		to_local(&node_world[node], c, lc, inv2);
		r2 = radius * radius * inv2[0];                  /* uniform scale: local radius^2 = r^2 / s^2 */
		decode_shape(p, &sh);
		for (k = 0; k < 3; k++) {
			const float *ax = sh.axes[k];
			float u = ax[0] * (lc[0] - sh.center[0]) + ax[1] * (lc[1] - sh.center[1]) + ax[2] * (lc[2] - sh.center[2]);
			float out = (u < 0.0f ? -u : u) - sh.half[k];
			if (out > 0.0f) dist2 += out * out;
		}
		if (dist2 <= r2) {
			n++;
			mask |= (uint64_t)1 << region;
		}
	}
	if (regions) *regions = mask;
	return n;
}
