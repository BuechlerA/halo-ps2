/* Host test: open the MODL / OTEX sections of a pack written by tools/tests/test_modelconv.py and check
 * them against the world that test baked (3 models: a 4-node biped with 2 regions / 4 permutations and 5
 * geometries, a 40-node ribbon that needs palette-limited batches, a rigid two-part prop; 9 parts,
 * 14 batches, 126 triangles). Then corrupt every table and byte: a corrupt section must be rejected, or
 * - if it still opens - be safe to walk completely (accessors included, with forged argument structs). */
#include "stream/hp2.h"
#include "world/model.h"
#include "world/tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

#define TAG_BIPED 0xE17C0008u
#define TAG_SPIDER 0xE17D0009u
#define TAG_PROP 0xE17E000Au

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int rd16s(const uint8_t *p) { return (int)(int16_t)(uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8); }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

static volatile uint32_t sink;

static uint32_t f2u(float x) { x = fabsf(x); return x < 1e9f ? (uint32_t)x : 0; }

/* Touch everything an opened section hands out: with ASan this proves the validation in model_open
 * covers every access the accessors make. Returns a checksum so the loop cannot be optimised away. */
static void walk(const model_section *s)
{
	uint32_t acc = 0;
	for (uint32_t i = 0; i < s->model_count; i++) {
		model_info m = model_get(s, i);
		acc += m.node_count + f2u(m.radius) + m.triangles_lod0;
		for (uint32_t k = 0; k < m.node_count; k++) {
			model_node n = model_get_node(s, &m, k);
			acc += (uint32_t)(n.parent + 2) + (uint32_t)n.name[0] + f2u(n.translation[0]);
			if (n.parent >= (int)m.node_count || n.first_child >= (int)m.node_count || n.next_sibling >= (int)m.node_count) abort();
		}
		for (uint32_t k = 0; k < m.marker_count; k++) {
			model_marker mk = model_get_marker(s, &m, k);
			acc += (uint32_t)mk.name[0] + mk.region + f2u(mk.translation[1]);
			if (model_find_marker(s, &m, mk.name) < 0) abort();
			if (mk.node >= (int)m.node_count) abort();
		}
		uint16_t sel[64];
		if (m.region_count > 64) abort();                           /* model_open caps the regions */
		model_select_permutations(s, &m, NULL, sel);
		model_select_permutations(s, &m, "base", sel);
		for (uint32_t p = 0; p < m.permutation_count; p++) {
			char name[MODEL_NAME_LEN];
			model_perm_name(s, &m, p, name);
			if (name[MODEL_NAME_LEN - 1]) abort();
			acc += model_perm_region(s, &m, p);
			for (uint32_t lod = 0; lod < MODEL_LODS; lod++) {
				model_part_range r = model_parts(s, &m, p, lod);
				if ((uint32_t)r.first_part + r.part_count > s->part_count) abort();
				for (uint32_t k = 0; k < r.part_count; k++) {
					model_part pt = model_get_part(s, r.first_part + k);
					if (pt.rigid_node >= (int)m.node_count) abort();
					acc += pt.triangles + f2u(pt.radius);
					for (uint32_t b = 0; b < pt.batch_count; b++) {
						model_batch bt = model_get_batch(s, pt.first_batch + b);
						const uint8_t *v = model_batch_vif(s, &bt);
						if (!v || bt.part != r.first_part + k) abort();
						for (uint32_t q = 0; q < bt.palette_count; q++)
							if (bt.palette[q] >= m.node_count) abort();
						for (uint32_t q = 0; q < bt.vif_qwords * 16; q++) acc += v[q];
					}
				}
			}
		}
		acc += model_pick_lod(&m, 50.0f);
	}
	for (uint32_t i = 0; i < s->part_count; i++) acc += model_get_part(s, i).triangles;
	for (uint32_t i = 0; i < s->batch_count; i++) acc += model_get_batch(s, i).vif_offset;
	sink = acc;
}

/* The accessors must survive arguments the caller forged (a model_info with oversized counts, a part
 * index past the table, a batch with a wild offset). */
static int forged(const model_section *s)
{
	model_info m = model_get(s, 0);
	char name[MODEL_NAME_LEN];
	m.node_count = m.marker_count = m.permutation_count = 0xFFFF;
	m.first_node = m.first_marker = m.first_range = 0xFFFFFFF0u;
	model_node n = model_get_node(s, &m, 0xFFFFFFFFu);
	CHECK(n.parent == MODEL_NO_NODE && n.scale == 1.0f);
	n = model_get_node(s, &m, 3);
	CHECK(n.parent == MODEL_NO_NODE && n.name[0] == 0);
	model_marker mk = model_get_marker(s, &m, 1);
	CHECK(mk.node == MODEL_NO_NODE && mk.name[0] == 0 && mk.region == 0xFFFF);
	CHECK(model_find_marker(s, &m, "primary trigger") == -1 && model_find_marker(s, &m, NULL) == -1);
	model_part_range r = model_parts(s, &m, 2, 1);
	CHECK(r.part_count == 0 && r.first_part == 0);
	model_perm_name(s, &m, 1, name);
	CHECK(name[0] == 0 && model_perm_region(s, &m, 1) == 0);
	m.region_count = 0;
	model_select_permutations(s, &m, "x", NULL);                          /* nothing to write */
	model_info none = model_get(s, 0xFFFFFFFFu);
	CHECK(none.tag_id == 0 && none.node_count == 0 && none.radius == 0.0f);
	CHECK(model_get_part(s, s->part_count).triangles == 0 && model_get_part(s, 0xFFFFFFFFu).rigid_node == MODEL_NO_NODE);
	model_batch bb = model_get_batch(s, s->batch_count);
	CHECK(bb.triangles == 0 && model_batch_vif(s, &bb) != NULL);          /* zeroed batch: offset 0, zero length */
	bb = model_get_batch(s, 0);
	bb.vif_offset = 0xFFFFFFF0u;
	CHECK(model_batch_vif(s, &bb) == NULL);
	bb.vif_offset = 0;
	bb.vif_qwords = 0x7FFFFFFFu;
	CHECK(model_batch_vif(s, &bb) == NULL && model_batch_vif(s, NULL) == NULL);
	return 0;
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

/* Open a copy that is exactly `n` bytes in its own allocation so ASan sees any read past it. */
static model_status open_copy(const uint8_t *raw, size_t n, int walk_it)
{
	uint8_t *c = malloc(n ? n : 1);
	model_section s;
	model_status st;
	if (!c) abort();
	memcpy(c, raw, n);
	st = model_open(&s, c, n);
	if (st == MODEL_OK && walk_it) walk(&s);
	free(c);
	return st;
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
	uint8_t *raw, *traw;
	size_t rawn, trawn;
	if (load_section(&pk, "MODL", &raw, &rawn) || load_section(&pk, "OTEX", &traw, &trawn)) return 1;

	model_section s;
	CHECK(model_open(&s, raw, rawn) == MODEL_OK);
	CHECK(s.model_count == 3 && s.part_count == 9 && s.batch_count == 14);
	CHECK(MODEL_LODS == 5 && MODEL_MAX_PALETTE == 24 && MODEL_MAX_BATCH_TRIS == 24 && MODEL_MAX_NODES == 64);
	CHECK(((uintptr_t)s.models | (uintptr_t)s.nodes | (uintptr_t)s.markers | (uintptr_t)s.ranges | (uintptr_t)s.parts |
	       (uintptr_t)s.batches | (uintptr_t)s.vif) % 16 == (uintptr_t)raw % 16);

	/* ---- lookup (sorted by tag id) ---- */
	CHECK(model_find(&s, TAG_BIPED) == 0 && model_find(&s, TAG_SPIDER) == 1 && model_find(&s, TAG_PROP) == 2);
	CHECK(model_find(&s, TAG_BIPED - 1) == -1 && model_find(&s, TAG_PROP + 1) == -1 && model_find(&s, 0) == -1 &&
	      model_find(&s, 0xFFFFFFFFu) == -1);

	/* ---- model records ---- */
	model_info a = model_get(&s, 0), sp = model_get(&s, 1), pr = model_get(&s, 2);
	CHECK(a.tag_id == TAG_BIPED && a.node_count == 4 && a.marker_count == 3 && a.region_count == 2 && a.permutation_count == 4);
	CHECK(a.first_node == 0 && a.first_marker == 0 && a.first_range == 0 && a.triangles_lod0 == 80);
	CHECK(fabsf(a.centre[0] - 5.5f) < 1e-6f && a.centre[1] == 0.0f && fabsf(a.centre[2] - 5.5f) < 1e-6f && fabsf(a.radius - 7.778176f) < 1e-5f);
	CHECK(a.lod_cutoff[0] == 70.0f && a.lod_cutoff[1] == 35.0f && a.lod_cutoff[2] == 18.0f && a.lod_cutoff[3] == 9.0f && a.lod_cutoff[4] == 0.0f);
	CHECK(sp.tag_id == TAG_SPIDER && sp.node_count == 40 && sp.marker_count == 0 && sp.permutation_count == 1 && sp.first_node == 4);
	CHECK(sp.first_range == 4 && sp.triangles_lod0 == 58 && sp.lod_cutoff[0] == 300.0f && sp.lod_cutoff[1] == 175.0f && sp.lod_cutoff[2] == 0.0f);
	CHECK(pr.tag_id == TAG_PROP && pr.node_count == 1 && pr.first_node == 44 && pr.first_range == 5 && pr.triangles_lod0 == 4);
	CHECK(pr.lod_cutoff[0] == 400.0f && pr.lod_cutoff[4] == 5.0f);

	/* ---- skeleton ---- */
	static const char *names[4] = { "root", "spine", "head", "arm" };
	static const int parent[4] = { -1, 0, 1, 1 }, child[4] = { 1, 2, -1, -1 }, sib[4] = { -1, -1, 3, -1 };
	for (uint32_t i = 0; i < 4; i++) {
		model_node nd = model_get_node(&s, &a, i);
		CHECK(strcmp(nd.name, names[i]) == 0 && nd.parent == parent[i] && nd.first_child == child[i] && nd.next_sibling == sib[i]);
		CHECK(nd.scale == 1.0f);
	}
	model_node spine = model_get_node(&s, &a, 1);
	CHECK(spine.translation[2] == 0.5f && fabsf(spine.rotation[2] + 0.70710678f) < 1e-6f && fabsf(spine.rotation[3] - 0.70710678f) < 1e-6f);
	CHECK(fabsf(spine.rotation[0]) < 1e-9f);
	model_node head = model_get_node(&s, &a, 2);
	CHECK(fabsf(head.translation[0] - 0.1f) < 1e-6f && fabsf(head.translation[2] - 0.4f) < 1e-6f && head.rotation[3] == 1.0f);
	model_node last = model_get_node(&s, &sp, 39);
	CHECK(strcmp(last.name, "bone39") == 0 && last.parent == MODEL_NO_NODE && last.next_sibling == MODEL_NO_NODE);
	CHECK(strcmp(model_get_node(&s, &sp, 0).name, "bone0") == 0 && model_get_node(&s, &sp, 0).next_sibling == 1);
	CHECK(model_get_node(&s, &a, 4).name[0] == 0 && model_get_node(&s, &a, 4).parent == MODEL_NO_NODE);   /* past the model's nodes */

	/* ---- markers ---- */
	model_marker m0 = model_get_marker(&s, &a, 0), m1 = model_get_marker(&s, &a, 1), m2 = model_get_marker(&s, &a, 2);
	CHECK(strcmp(m0.name, "primary trigger") == 0 && m0.region == 0 && m0.permutation == 0 && m0.node == 3);
	CHECK(fabsf(m0.translation[0] - 0.1f) < 1e-6f && fabsf(m0.translation[1] - 0.2f) < 1e-6f && fabsf(m0.rotation[3] - 0.70710678f) < 1e-6f);
	CHECK(strcmp(m1.name, "head cam") == 0 && m1.region == 1 && m1.permutation == 3 && m1.node == 2);
	CHECK(strcmp(m2.name, "head cam") == 0 && m2.region == 0xFFFF && m2.permutation == 0xFFFF && m2.node == 0);
	CHECK(m2.translation[0] == 1.0f && m2.translation[1] == 2.0f && m2.translation[2] == 3.0f);
	CHECK(model_find_marker(&s, &a, "primary trigger") == 0 && model_find_marker(&s, &a, "head cam") == 1);
	CHECK(model_find_marker(&s, &a, "head") == -1 && model_find_marker(&s, &a, "") == -1 && model_find_marker(&s, &sp, "head cam") == -1);
	CHECK(model_get_marker(&s, &a, 3).node == MODEL_NO_NODE);

	/* ---- permutations ---- */
	char nm[MODEL_NAME_LEN];
	static const char *pn[4] = { "base", "damaged", "base", "helmet" };
	static const uint32_t preg[4] = { 0, 0, 1, 1 };
	for (uint32_t p = 0; p < 4; p++) {
		model_perm_name(&s, &a, p, nm);
		CHECK(strcmp(nm, pn[p]) == 0 && model_perm_region(&s, &a, p) == preg[p]);
	}
	uint16_t sel[2];
	model_select_permutations(&s, &a, NULL, sel);
	CHECK(sel[0] == 0 && sel[1] == 2);
	model_select_permutations(&s, &a, "", sel);
	CHECK(sel[0] == 0 && sel[1] == 2);
	model_select_permutations(&s, &a, "damaged", sel);                    /* only region 0 has it; region 1 keeps its first */
	CHECK(sel[0] == 1 && sel[1] == 2);
	model_select_permutations(&s, &a, "helmet", sel);
	CHECK(sel[0] == 0 && sel[1] == 3);
	model_select_permutations(&s, &a, "base", sel);
	CHECK(sel[0] == 0 && sel[1] == 2);
	model_select_permutations(&s, &a, "Damaged", sel);                    /* names compare exactly */
	CHECK(sel[0] == 0 && sel[1] == 2);
	model_select_permutations(&s, &a, "nothing", sel);
	CHECK(sel[0] == 0 && sel[1] == 2);
	uint16_t one[1];
	model_select_permutations(&s, &pr, "x", one);
	CHECK(one[0] == 0);
	model_perm_name(&s, &pr, 0, nm);
	CHECK(strcmp(nm, "__base") == 0);

	/* ---- (permutation, LOD) -> parts: shared geometry shares parts ---- */
	static const model_part_range want_a[4][5] = {
		{ { 0, 2 }, { 2, 1 }, { 2, 1 }, { 3, 1 }, { 3, 1 } },
		{ { 4, 1 }, { 4, 1 }, { 4, 1 }, { 4, 1 }, { 4, 1 } },
		{ { 0, 2 }, { 2, 1 }, { 2, 1 }, { 3, 1 }, { 3, 1 } },
		{ { 5, 1 }, { 5, 1 }, { 5, 1 }, { 5, 1 }, { 5, 1 } } };
	for (uint32_t p = 0; p < 4; p++)
		for (uint32_t l = 0; l < 5; l++) {
			model_part_range r = model_parts(&s, &a, p, l);
			CHECK(r.first_part == want_a[p][l].first_part && r.part_count == want_a[p][l].part_count);
		}
	CHECK(model_parts(&s, &a, 4, 0).part_count == 0 && model_parts(&s, &a, 0, 5).part_count == 0);
	CHECK(model_parts(&s, &sp, 0, 4).first_part == 6 && model_parts(&s, &sp, 0, 4).part_count == 1);
	CHECK(model_parts(&s, &pr, 0, 2).first_part == 7 && model_parts(&s, &pr, 0, 2).part_count == 2);

	/* ---- parts ---- */
	static const struct { uint32_t shader; uint16_t fb, bc, tris, shift; int rigid; uint16_t flags; } parts[9] = {
		{ 0xE1770003u, 0, 2, 38, 13, -1, 0 }, { 0xE1780004u, 2, 1, 2, 14, 3, 2 }, { 0xE1770003u, 3, 1, 14, 14, 1, 0 },
		{ 0xE1770003u, 4, 1, 2, 14, 0, 0 },   { 0xE1770003u, 5, 1, 6, 14, 1, 0 },  { 0xE1790005u, 6, 1, 2, 14, 2, 0 },
		{ 0xE1770003u, 7, 5, 58, 14, -1, 0 }, { 0xE1780004u, 12, 1, 2, 14, 0, 2 }, { 0xE17A0006u, 13, 1, 2, 14, 0, 0 } };
	for (uint32_t i = 0; i < 9; i++) {
		model_part pt = model_get_part(&s, i);
		CHECK(pt.shader_id == parts[i].shader && pt.first_batch == parts[i].fb && pt.batch_count == parts[i].bc);
		CHECK(pt.triangles == parts[i].tris && pt.uv_shift == parts[i].shift && pt.rigid_node == parts[i].rigid && pt.flags == parts[i].flags);
		CHECK(pt.radius > 0.0f);
	}
	model_part p0 = model_get_part(&s, 0);
	CHECK(fabsf(p0.centre[0] - 4.75f) < 1e-6f && fabsf(p0.centre[2] - 0.25f) < 1e-6f && fabsf(p0.radius - 4.756575f) < 1e-5f);

	/* ---- batches: limits, palettes, packets ---- */
	static const uint16_t btris[14] = { 24, 14, 2, 14, 2, 6, 2, 14, 13, 12, 12, 7, 2, 2 };
	static const uint16_t bpart[14] = { 0, 0, 1, 2, 3, 4, 5, 6, 6, 6, 6, 6, 7, 8 };
	static const uint8_t bpal[14] = { 3, 2, 1, 1, 1, 1, 1, 23, 24, 24, 24, 17, 1, 1 };
	uint32_t vif_at = 0;
	for (uint32_t i = 0; i < 14; i++) {
		model_batch b = model_get_batch(&s, i);
		CHECK(b.triangles == btris[i] && b.part == bpart[i] && b.palette_count == bpal[i]);
		CHECK(b.vif_offset == vif_at && b.vif_offset % 16 == 0 && b.vif_qwords * 16u >= 4u + 32u + 3u * b.triangles * 20u);
		vif_at += b.vif_qwords * 16;
		for (uint32_t q = b.palette_count; q < MODEL_MAX_PALETTE; q++) CHECK(b.palette[q] == 0);
		for (uint32_t q = 0; q < b.palette_count; q++)
			for (uint32_t r = q + 1; r < b.palette_count; r++) CHECK(b.palette[q] != b.palette[r]);
	}
	CHECK(vif_at == s.vif_size);
	model_batch b0 = model_get_batch(&s, 0);
	CHECK(b0.palette[0] == 0 && b0.palette[1] == 1 && b0.palette[2] == 3 && b0.vif_qwords == 93);
	const uint8_t *p = model_batch_vif(&s, &b0);
	CHECK(rd32(p) == (0x6Cu << 24 | 2u << 16 | 0x8000u));
	CHECK(rd32(p + 20) == 72 && rd32(p + 24) == 0 && rd32(p + 28) == 0 && rd32(p + 32) == 0);
	CHECK(rd32(p + 36) == (0x6Du << 24 | 72u << 16 | 0x8000u | 4u));                          /* positions */
	CHECK(rd32(p + 36 + 4 + 72 * 8) == (0x6Du << 24 | 72u << 16 | 0x8000u | 76u));          /* uv, slot 1, weight */
	CHECK(rd32(p + 36 + 8 + 2 * 72 * 8) == (0x6Eu << 24 | 72u << 16 | 0x8000u | 148u));    /* normals: signed (no 0x4000) */

	/* batch 3 is the rigid 14-triangle ribbon on node 1: slots are all 0, weight 1.0; first triangle is
	 * (0,0,0) (0,0,0.5) (0.5,0,0); uv = (-1,-0.75) at shift 14; normals -y */
	model_batch b3 = model_get_batch(&s, 3);
	const uint8_t *q3 = model_batch_vif(&s, &b3);
	float c3[3] = { rdf(q3 + 4), rdf(q3 + 8), rdf(q3 + 12) }, sc3 = rdf(q3 + 16);
	const uint32_t n3 = 14 * 3;
	CHECK(rd32(q3 + 20) == n3 && fabsf(c3[0] - 1.75f) < 1e-6f && fabsf(c3[2] - 0.25f) < 1e-6f);
	CHECK(sc3 > 0.0f && fabsf(sc3 * 32767.0f - 1.75f) < 1e-4f);                  /* largest half extent / 32767 */
	static const float want[3][3] = { { 0, 0, 0 }, { 0, 0, 0.5f }, { 0.5f, 0, 0 } };
	for (int v = 0; v < 3; v++) {
		const uint8_t *pos = q3 + 40 + v * 8, *uv = q3 + 40 + n3 * 8 + 4 + v * 8, *nr = q3 + 40 + n3 * 8 + 4 + n3 * 8 + 4 + v * 4;
		for (int k = 0; k < 3; k++) CHECK(fabsf(c3[k] + (float)rd16s(pos + 2 * k) * sc3 - want[v][k]) < 1e-3f);
		CHECK(rd16s(pos + 6) == 0 && rd16s(uv + 4) == 0 && rd16s(uv + 6) == 32767);
		CHECK(nr[0] == 0 && (int8_t)nr[1] == -127 && nr[2] == 0 && nr[3] == 0);
	}
	CHECK(abs(rd16s(q3 + 40 + n3 * 8 + 4) + 16384) <= 2 && abs(rd16s(q3 + 40 + n3 * 8 + 6) + 12288) <= 2);

	/* the 40-node ribbon: every batch obeys the limits and one closes before 24 triangles because of the palette */
	uint32_t early = 0;
	for (uint32_t i = 7; i < 12; i++) {
		model_batch b = model_get_batch(&s, i);
		CHECK(b.palette_count <= MODEL_MAX_PALETTE && b.triangles <= MODEL_MAX_BATCH_TRIS);
		if (b.triangles < MODEL_MAX_BATCH_TRIS && i < 11) early++;
	}
	CHECK(early == 4);

	/* ---- LOD choice (semantics: the highest LOD whose cutoff the projected diameter reaches) ---- */
	CHECK(model_pick_lod(&a, 1000.0f) == 0 && model_pick_lod(&a, 70.0f) == 0 && model_pick_lod(&a, 69.9f) == 1);
	CHECK(model_pick_lod(&a, 35.0f) == 1 && model_pick_lod(&a, 20.0f) == 2 && model_pick_lod(&a, 18.0f) == 2);
	CHECK(model_pick_lod(&a, 9.0f) == 3 && model_pick_lod(&a, 8.99f) == 4 && model_pick_lod(&a, 0.0f) == 4);
	CHECK(model_pick_lod(&a, -5.0f) == 4 && model_pick_lod(&a, NAN) == 4 && model_pick_lod(&a, INFINITY) == 0);
	CHECK(model_pick_lod(&sp, 10.0f) == 2 && model_pick_lod(&sp, 175.0f) == 1 && model_pick_lod(&sp, 299.0f) == 1);
	CHECK(model_pick_lod(&pr, 4.0f) == 4 && model_pick_lod(&pr, 5.0f) == 4 && model_pick_lod(&pr, 120.0f) == 2);
	walk(&s);
	if (forged(&s)) return 1;

	/* ---- OTEX: same layout as TEXS, bound by the Xbox shader tag id the parts carry ---- */
	tex_section ts;
	CHECK(tex_open(&ts, traw, trawn) == TEX_OK && ts.binding_count == 4);
	tex_binding tb;
	CHECK(tex_find_binding(&ts, 0xE1770003u, &tb) && tb.texture != TEX_NONE && tb.blend == TEX_BLEND_OPAQUE);
	CHECK(tex_find_binding(&ts, 0xE1780004u, &tb) && tb.texture != TEX_NONE && tb.blend == TEX_BLEND_ALPHA && (tb.flags & TEX_B_DECAL));
	CHECK(tex_find_binding(&ts, 0xE1790005u, &tb) && tb.texture == TEX_NONE && (tb.flags & TEX_B_UNTEXTURED) && (tb.flags & TEX_B_TWO_SIDED));
	CHECK(tex_find_binding(&ts, 0xE17A0006u, &tb) && tb.blend == TEX_BLEND_OPAQUE);
	CHECK(!tex_find_binding(&ts, 0xE17B0007u, &tb));                       /* the meter shader no part uses */
	for (uint32_t i = 0; i < s.part_count; i++) CHECK(tex_find_binding(&ts, model_get_part(&s, i).shader_id, &tb));

	/* ---- hostile input ---- */
	model_section bad;
	for (size_t cut = 0; cut < rawn; cut++) CHECK(open_copy(raw, cut, 0) != MODEL_OK);       /* every truncation */
	uint8_t *copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(model_open(&bad, copy, rawn + 16) == MODEL_ERR_LAYOUT);          /* size field no longer matches */
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(model_open(&bad, copy, rawn) == MODEL_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(model_open(&bad, copy, rawn) == MODEL_ERR_MAGIC);
	CHECK(model_open(&bad, raw, 0) == MODEL_ERR_TRUNCATED);

	/* every table of the directory: moved, misaligned, oversized, counted wrong */
	for (int t = 0; t < 7; t++) {
		static const uint32_t vals[] = { 1, 17, 0xFFFFFFF0u, 0xFFFFFFFFu, 0x1000000u };
		for (int field = 0; field < 2; field++)
			for (size_t k = 0; k < sizeof vals / sizeof vals[0]; k++) {
				memcpy(copy, raw, rawn);
				uint32_t v = vals[k];
				for (int b = 0; b < 4; b++) copy[0x20 + 8 * t + 4 * field + b] = (uint8_t)(v >> (8 * b));
				CHECK(open_copy(copy, rawn, 0) != MODEL_OK);
			}
	}
	/* every byte of the header and the tables flipped three ways; the VIF data (free-form vertices) every 3rd byte */
	uint32_t vif_off = (uint32_t)(s.vif - raw), rejected = 0, accepted = 0;
	static const uint8_t xors[3] = { 0xFF, 0x80, 0x01 };
	for (size_t i = 0; i < rawn; i++) {
		if (i >= vif_off && (i - vif_off) % 3) continue;
		for (int x = 0; x < 3; x++) {
			memcpy(copy, raw, rawn);
			copy[i] ^= xors[x];
			model_status st = open_copy(copy, rawn, 1);                  /* walk() on every section that still opens */
			if (st == MODEL_OK) accepted++;
			else rejected++;
		}
	}
	CHECK(rejected > 2000 && accepted > 0);

	/* targeted table corruptions the reader must refuse */
	{
		const uint8_t *tab_models = s.models, *tab_parts = s.parts, *tab_batches = s.batches;
		struct { size_t at; uint8_t v; } cases[] = {
			{ (size_t)(tab_models - raw) + 4, 65 },                         /* node count over the maximum */
			{ (size_t)(tab_models - raw) + 8, 65 },                         /* more than 64 regions */
			{ (size_t)(tab_models - raw) + 12, 1 },                         /* first node not contiguous */
			{ (size_t)(tab_models - raw) + 64, 1 },                         /* first part not contiguous */
			{ (size_t)(tab_parts - raw) + 4, 7 },                           /* first batch not contiguous */
			{ (size_t)(tab_parts - raw) + 12, 0x7F },                       /* rigid node beyond the model */
			{ (size_t)(tab_parts - raw) + 14, 4 },                          /* unknown part flag */
			{ (size_t)(tab_parts - raw) + 10, 15 },                         /* uv shift > 14 */
			{ (size_t)(tab_batches - raw) + 2, 25 },                        /* 25 triangles */
			{ (size_t)(tab_batches - raw) + 4, 25 },                        /* palette of 25 */
			{ (size_t)(tab_batches - raw) + 4, 0 },                         /* empty palette */
			{ (size_t)(tab_batches - raw) + 16, 9 },                        /* palette node beyond the model (biped has 4) */
			{ (size_t)(tab_batches - raw) + 0, 9 },                         /* batch of another part */
			{ (size_t)(tab_batches - raw) + 8, 16 },                        /* vif offset out of order */
			{ (size_t)(s.vif - raw) + 3, 0 },                               /* first UNPACK command destroyed */
		};
		for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
			memcpy(copy, raw, rawn);
			CHECK(copy[cases[k].at] != cases[k].v);
			copy[cases[k].at] = cases[k].v;
			CHECK(open_copy(copy, rawn, 0) != MODEL_OK);
		}
	}
	free(copy);
	free(traw);
	free(raw);
	free(img);
	printf("test_model: ok (%u rejected, %u accepted and walked)\n", rejected, accepted);
	return 0;
}
