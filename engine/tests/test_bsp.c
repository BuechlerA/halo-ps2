/* Host test: open the BSP0 / BSP1 sections of a pack written by tools/tests/test_bspconv.py and check
 * them against the world that test baked (2 clusters joined by one portal, a 3-node tree, 6 batches,
 * 58 triangles; BSP1 is a one-triangle section without a tree). Then corrupt every table and byte:
 * a corrupt section must be rejected, or - if it still opens - be safe to walk completely. */
#include "stream/hp2.h"
#include "world/bsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static volatile uint32_t sink;

static uint32_t f2u(float x) { x = fabsf(x); return x < 1e9f ? (uint32_t)x : 0; }

/* Touch everything an opened section hands out: with ASan this proves the validation in bsp_open
 * covers every access the accessors make. */
static void walk(const bsp_section *s)
{
	uint32_t acc = 0;
	for (uint32_t i = 0; i < s->material_count; i++) acc += bsp_get_material(s, i).triangles;
	for (uint32_t i = 0; i < s->batch_count; i++) {
		bsp_batch b = bsp_get_batch(s, i);
		const uint8_t *p = bsp_batch_vif(s, &b);
		for (uint32_t q = 0; q < b.vif_qwords * 16; q++) acc += p[q];
		acc += f2u(b.radius);
	}
	for (uint32_t i = 0; i < s->cluster_count; i++) {
		bsp_cluster c = bsp_get_cluster(s, i);
		for (uint32_t k = 0; k < c.batch_count; k++) {
			uint32_t bi = bsp_cluster_batch(s, &c, k);
			acc += bsp_get_batch(s, bi).triangles;
		}
		for (uint32_t k = 0; k < c.portal_count; k++) acc += bsp_get_portal(s, bsp_cluster_portal(s, &c, k)).vertex_count;
	}
	for (uint32_t i = 0; i < s->portal_count; i++) {
		bsp_portal p = bsp_get_portal(s, i);
		float v[3];
		for (uint32_t k = 0; k < p.vertex_count; k++) { bsp_portal_vertex(s, &p, k, v); acc += f2u(v[0]); }
	}
	for (int x = -30; x <= 30; x += 3)
		for (int y = -30; y <= 30; y += 15) {
			float pos[3] = { (float)x * 0.7f, (float)y, 0.5f };
			int c = bsp_find_cluster(s, pos);
			acc += (uint32_t)(c + 1);
			if (c >= (int)s->cluster_count || c < -1) abort();
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
	if (load_section(&pk, "BSP0", &raw, &rawn) || load_section(&pk, "BSP1", &raw1, &rawn1)) return 1;

	bsp_section b;
	CHECK(bsp_open(&b, raw, rawn) == BSP_OK);
	CHECK(b.material_count == 2 && b.batch_count == 6 && b.triangle_count == 58);
	CHECK(b.cluster_count == 2 && b.portal_count == 1 && b.portal_vertex_count == 4);
	CHECK(b.node_count == 3 && b.plane_count == 3 && b.leaf_count == 2);
	CHECK(b.cluster_batch_count == 7 && b.cluster_portal_count == 2);
	CHECK(fabsf(b.bounds[0] + 11.0f) < 1e-6f && fabsf(b.bounds[4] - 117.5f) < 1e-6f);
	CHECK(((uintptr_t)b.materials | (uintptr_t)b.batches | (uintptr_t)b.vif | (uintptr_t)b.clusters |
	       (uintptr_t)b.portals | (uintptr_t)b.nodes | (uintptr_t)b.planes) % 16 == (uintptr_t)raw % 16);
	CHECK(BSP_VU_QWORDS == 220 && BSP_MAX_BATCH_TRIS == 24 && BSP_MAX_BATCH_VERTS == 72);

	/* batch vertex accessors: the slow accessor and the raw streams agree, and every vertex lies
	 * within the batch radius of its centre (the encoding is centre-relative) */
	{
		uint32_t bi, v;
		for (bi = 0; bi < b.batch_count; bi++) {
			bsp_batch bb = bsp_get_batch(&b, bi);
			bsp_batch_raw raw_s = bsp_batch_streams(&b, &bb);
			CHECK(raw_s.vertices == 3u * bb.triangles && raw_s.scale > 0.0f);
			CHECK(fabsf(raw_s.scale * 32767.0f - bb.radius) <= 1e-3f * (1.0f + bb.radius));
			CHECK(((uintptr_t)raw_s.pos & 7) == 0 && ((uintptr_t)raw_s.uv & 3) == 0);
			for (v = 0; v < raw_s.vertices; v++) {
				bsp_vertex bv;
				int k;
				float d2 = 0.0f;
				bsp_batch_vertex(&b, &bb, v, &bv);
				for (k = 0; k < 3; k++) {
					float p = raw_s.centre[k] + (float)raw_s.pos[4 * v + k] * raw_s.scale;
					float d = bv.pos[k] - bb.centre[k];
					CHECK(fabsf(bv.pos[k] - p) <= 1e-5f * (1.0f + fabsf(p)));
					d2 += d * d;
				}
				for (k = 0; k < 4; k++) CHECK(bv.uv[k] == raw_s.uv[4 * v + k] && bv.rgba[k] == raw_s.col[4 * v + k]);
				CHECK(d2 <= (bb.radius * 1.001f + 1e-3f) * (bb.radius * 1.001f + 1e-3f));
			}
		}
	}

	/* materials: batches are grouped by cluster set, so the material records only count them */
	bsp_material m0 = bsp_get_material(&b, 0), m1 = bsp_get_material(&b, 1);
	CHECK(m0.shader_id == 0xE1740042u && m0.lightmap_page == 3 && m0.batch_count == 4 && m0.triangles == 40);
	CHECK(m1.shader_id == 0xE1740043u && m1.batch_count == 2 && m1.triangles == 18);
	static const uint16_t tris[6] = { 24, 6, 4, 8, 6, 10 }, mat[6] = { 0, 0, 0, 1, 0, 1 };
	for (uint32_t i = 0; i < 6; i++) {
		bsp_batch bt = bsp_get_batch(&b, i);
		CHECK(bt.triangles == tris[i] && bt.material == mat[i] && bt.radius > 0.0f);
		CHECK(bt.vif_offset % 16 == 0 && bt.vif_qwords * 16 >= 4 + 32 + 3 * bt.triangles * 20);
	}
	/* batch 0 packet: UNPACK V4-32 addr 0 (2 qw), V4-16 addr 4 and 76, V4-8 unsigned addr 148 */
	bsp_batch b0 = bsp_get_batch(&b, 0);
	const uint8_t *p = bsp_batch_vif(&b, &b0);
	CHECK(rd32(p) == (0x6Cu << 24 | 2u << 16 | 0x8000u));
	CHECK(rd32(p + 20) == 72 && rd32(p + 24) == 0 && rd32(p + 28) == 0 && rd32(p + 32) == 0);
	CHECK(rd32(p + 36) == (0x6Du << 24 | 72u << 16 | 0x8000u | 4u));
	CHECK(rd32(p + 36 + 4 + 72 * 8) == (0x6Du << 24 | 72u << 16 | 0x8000u | 76u));
	CHECK(rd32(p + 36 + 8 + 2 * 72 * 8) == (0x6Eu << 24 | 72u << 16 | 0xC000u | 148u));
	CHECK(b0.vif_qwords == 93);
	const uint8_t *col = p + 36 + 12 + 2 * 72 * 8;
	CHECK(rd32(col) == 0x80808080u && rd32(col + 71 * 4) == 0x80808080u);   /* neutral colours */

	/* clusters: batch lists (ordered by material, batch 2 shared), portal list, attributes */
	bsp_cluster c0 = bsp_get_cluster(&b, 0), c1 = bsp_get_cluster(&b, 1);
	CHECK(c0.batch_count == 4 && c1.batch_count == 3 && c0.portal_count == 1 && c1.portal_count == 1);
	static const uint32_t l0[4] = { 0, 1, 2, 3 }, l1[3] = { 2, 4, 5 };
	for (uint32_t k = 0; k < 4; k++) CHECK(bsp_cluster_batch(&b, &c0, k) == l0[k]);
	for (uint32_t k = 0; k < 3; k++) CHECK(bsp_cluster_batch(&b, &c1, k) == l1[k]);
	CHECK(bsp_cluster_portal(&b, &c0, 0) == 0 && bsp_cluster_portal(&b, &c1, 0) == 0);
	CHECK(c0.triangles == 42 && c1.triangles == 20);
	CHECK(c0.flags == BSP_CLUSTER_SKY && c0.fog == -1 && c0.sky == 0 && c0.sound_environment == 1);
	CHECK(c1.flags == (BSP_CLUSTER_FOG | BSP_CLUSTER_FOG_BIT15 | BSP_CLUSTER_WEATHER | BSP_CLUSTER_MIRROR));
	CHECK(c1.fog == 1 && c1.sky == -1 && c1.weather == 0 && c1.background_sound == 2);
	for (int k = 0; k < 3; k++) CHECK(c0.bounds_min[k] <= c0.bounds_max[k] && c1.bounds_min[k] <= c1.bounds_max[k]);

	/* the portal: cluster 1 is in front of x = 0, cluster 0 behind; CCW square of half-size 1 */
	bsp_portal po = bsp_get_portal(&b, 0);
	CHECK(po.cluster_a == 1 && po.cluster_b == 0 && po.vertex_count == 4 && po.first_vertex == 0);
	CHECK(po.plane[0] == 1.0f && po.plane[1] == 0.0f && po.plane[3] == 0.0f);
	CHECK(fabsf(po.radius - 1.41421356f) < 1e-5f);
	float v[3];
	bsp_portal_vertex(&b, &po, 2, v);
	CHECK(v[0] == 0.0f && v[1] == 1.0f && v[2] == 1.0f);

	/* point location */
	static const struct { float p[3]; int want; } pts[] = {
		{ { -5, 0, 0 }, 0 }, { { 5, 3, -2 }, 1 }, { { 15, 0, 0 }, -1 }, { { -15, 0, 0 }, -1 },
		{ { 0, 1, 1 }, 1 }, { { -0.001f, 0, 0 }, 0 }, { { 9.999f, 0, 0 }, 1 }, { { 10, 0, 0 }, -1 },
		{ { -10, 5, 5 }, 0 }, { { -10.001f, 0, 0 }, -1 } };
	for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) CHECK(bsp_find_cluster(&b, pts[i].p) == pts[i].want);
	walk(&b);

	/* BSP1: no tree, one leaf -> every position is cluster 0 */
	bsp_section flat;
	CHECK(bsp_open(&flat, raw1, rawn1) == BSP_OK);
	CHECK(flat.node_count == 0 && flat.leaf_count == 1 && flat.cluster_count == 1 && flat.batch_count == 1);
	float far[3] = { 1e6f, -1e6f, 0.0f };
	CHECK(bsp_find_cluster(&flat, far) == 0);
	walk(&flat);

	/* ---- hostile input ---- */
	bsp_section bad;
	for (size_t cut = 0; cut < 144; cut++) CHECK(bsp_open(&bad, raw, cut) != BSP_OK);
	CHECK(bsp_open(&bad, raw, rawn - 16) != BSP_OK);
	uint8_t *copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(bsp_open(&bad, copy, rawn + 16) == BSP_ERR_LAYOUT);          /* size field no longer matches */

	/* version 1 (and any other version) is refused with its own error */
	memcpy(copy, raw, rawn);
	copy[4] = 1;
	CHECK(bsp_open(&bad, copy, rawn) == BSP_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(bsp_open(&bad, copy, rawn) == BSP_ERR_MAGIC);

	/* every table of the directory: moved, misaligned, oversized, counted wrong */
	static const char *const names[11] = { "materials", "batches", "vif", "clusters", "cluster_batches",
		"cluster_portals", "portals", "portal_vertices", "nodes", "planes", "leaves" };
	for (int t = 0; t < 11; t++) {
		size_t at = 0x30 + 8 * (size_t)t;
		uint32_t off = rd32(raw + at), cnt = rd32(raw + at + 4);
		const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)rawn + 16, cnt }, { off, 0xFFFFFFFFu },
		                                                { off, (uint32_t)rawn }, { 0, cnt }, { 0xFFFFFFF0u, cnt } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw, rawn);
			wr32(copy + at, tries[k].off);
			wr32(copy + at + 4, tries[k].cnt);
			if (bsp_open(&bad, copy, rawn) == BSP_OK) { fprintf(stderr, "table %s try %zu accepted\n", names[t], k); return 1; }
		}
		/* a count that is merely wrong (one more / one less) must never open into out-of-bounds reads */
		for (int d = -1; d <= 1; d += 2) {
			memcpy(copy, raw, rawn);
			wr32(copy + at + 4, cnt + (uint32_t)d);
			if (bsp_open(&bad, copy, rawn) == BSP_OK) walk(&bad);
		}
	}

	/* targeted: each of these must be refused */
	uint32_t clus = rd32(raw + 0x30 + 8 * 3), cbl = rd32(raw + 0x30 + 8 * 4), por = rd32(raw + 0x30 + 8 * 6),
	         nod = rd32(raw + 0x30 + 8 * 8), lea = rd32(raw + 0x30 + 8 * 10), bat = rd32(raw + 0x30 + 8 * 1),
	         mat_ = rd32(raw + 0x30 + 8 * 0), vif = rd32(raw + 0x30 + 8 * 2);
	const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
		{ "node plane", nod, 3, 4 }, { "node child backwards", nod + 12 + 4, 1, 4 }, { "node child past end", nod + 4, 3, 4 },
		{ "node leaf child", nod + 12 + 4, 0x80000002u, 4 }, { "leaf cluster", lea, 2, 2 },
		{ "cluster batch ref", cbl, 6, 2 }, { "cluster batch range", clus + 36, 100, 4 },
		{ "cluster portal range", clus + 44, 3, 4 }, { "portal cluster", por, 2, 2 },
		{ "portal vertex range", por + 4, 5, 2 }, { "batch triangles 25", bat + 2, 25, 2 },
		{ "batch triangles 0", bat + 2, 0, 2 }, { "batch material", bat, 2, 2 }, { "batch offset", bat + 4, 8, 4 },
		{ "batch qwords", bat + 8, 3, 4 }, { "batch radius NaN", bat + 24, 0x7FC00000u, 4 },
		{ "material batch sum", mat_ + 8, 9, 2 },
		{ "vif unpack cmd", vif, 0x6B000000u, 4 }, { "vif not TOPS-relative", vif + 1, 0, 1 },
		{ "vif vertex lane", vif + 20, 71, 4 }, { "vif pos addr", vif + 36, 0x6D480005u, 4 },
		{ "vif non-zero padding", vif + 1488 + 412, 0x17000000u, 4 }, { "header size", 8, 1, 4 } };
	for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
		memcpy(copy, raw, rawn);
		if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
		else if (hit[k].bytes == 2) { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
		else copy[hit[k].at] = (uint8_t)hit[k].val;
		if (bsp_open(&bad, copy, rawn) == BSP_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
	}

	/* no single-byte change anywhere may let an out-of-bounds read through */
	for (size_t at = 4; at < rawn; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (bsp_open(&bad, copy, rawn) == BSP_OK) walk(&bad);
		}
	free(copy);
	free(raw);
	free(raw1);
	free(img);
	puts("ok");
	return 0;
}
