#include "world/bsp.h"
#include <string.h>

#define HEADER_SIZE 144u
#define DIR_AT 0x30u
#define MATERIAL_REC 32u
#define BATCH_REC 32u
#define CLUSTER_REC 64u
#define PORTAL_REC 48u
#define VERTEX_REC 12u
#define NODE_REC 12u
#define PLANE_REC 16u
#define NO_CHILD 0xFFFFFFFFu
#define LEAF_FLAG 0x80000000u
#define VIF_UNPACK_V4_32 0x6Cu
#define VIF_UNPACK_V4_16 0x6Du
#define VIF_UNPACK_V4_8 0x6Eu
#define TOPS 0x8000u
#define USN 0x4000u
#define VU_POS 4u
#define VU_UV (VU_POS + BSP_MAX_BATCH_VERTS)
#define VU_COL (VU_UV + BSP_MAX_BATCH_VERTS)

/* Directory order: materials, batches, vif, clusters, cluster batches, cluster portals,
 * portals, portal vertices, nodes, planes, leaves. */
enum { T_MATERIALS, T_BATCHES, T_VIF, T_CLUSTERS, T_CBATCHES, T_CPORTALS, T_PORTALS, T_PVERTS, T_NODES,
       T_PLANES, T_LEAVES, T_COUNT };
static const uint32_t rec_size[T_COUNT] = { MATERIAL_REC, BATCH_REC, 1, CLUSTER_REC, 2, 2, PORTAL_REC,
                                            VERTEX_REC, NODE_REC, PLANE_REC, 2 };

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static int rds16(const uint8_t *p) { return (int)(int16_t)(uint16_t)rd16(p); }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* A batch packet is exactly: UNPACK V4-32 (addr 0, 2 qw), UNPACK V4-16 (addr 4, n), UNPACK V4-16
 * (addr 4+72, n), UNPACK V4-8 unsigned (addr 4+144, n), zero padding to a quadword. Anything else
 * could make the VIF run arbitrary commands, so the packet is verified word for word. */
static uint32_t packet_bytes(uint32_t tris)
{
	uint32_t n = tris * 3;
	return (4 + 32 + 4 + n * 8 + 4 + n * 8 + 4 + n * 4 + 15) & ~15u;
}

static int packet_ok(const uint8_t *p, uint32_t tris)
{
	uint32_t n = tris * 3, o = 0;
	if (rd32(p) != (((uint32_t)VIF_UNPACK_V4_32 << 24) | (2u << 16) | TOPS)) return 0;
	if (rd32(p + 20) != n || rd32(p + 24) != 0 || rd32(p + 28) != 0 || rd32(p + 32) != 0) return 0;
	o = 36;
	if (rd32(p + o) != (((uint32_t)VIF_UNPACK_V4_16 << 24) | (n << 16) | TOPS | VU_POS)) return 0;
	o += 4 + n * 8;
	if (rd32(p + o) != (((uint32_t)VIF_UNPACK_V4_16 << 24) | (n << 16) | TOPS | VU_UV)) return 0;
	o += 4 + n * 8;
	if (rd32(p + o) != (((uint32_t)VIF_UNPACK_V4_8 << 24) | (n << 16) | TOPS | USN | VU_COL)) return 0;
	o += 4 + n * 4;
	for (; o < packet_bytes(tris); o++)
		if (p[o]) return 0;
	return 1;
}

bsp_status bsp_open(bsp_section *s, const uint8_t *d, size_t size)
{
	memset(s, 0, sizeof *s);
	if (size < HEADER_SIZE) return BSP_ERR_TRUNCATED;
	if (memcmp(d, "BSPS", 4) != 0) return BSP_ERR_MAGIC;
	if (rd16(d + 4) != BSP_VERSION) return BSP_ERR_VERSION;
	if (rd32(d + 8) != size) return BSP_ERR_LAYOUT;
	s->triangle_count = rd32(d + 12);
	for (int i = 0; i < 6; i++) s->bounds[i] = rdf(d + 16 + 4 * i);

	const uint8_t *tab[T_COUNT];
	uint32_t cnt[T_COUNT];
	for (int t = 0; t < T_COUNT; t++) {
		uint32_t off = rd32(d + DIR_AT + 8 * t), n = rd32(d + DIR_AT + 8 * t + 4);
		if (off % 16 || off < HEADER_SIZE || off > size) return BSP_ERR_LAYOUT;
		if ((uint64_t)n * rec_size[t] > size - off) return BSP_ERR_TRUNCATED;
		tab[t] = d + off;
		cnt[t] = n;
	}
	if (cnt[T_MATERIALS] > 0xFFFF || cnt[T_BATCHES] > 0xFFFF || cnt[T_CLUSTERS] > 0xFFFE ||
	    cnt[T_PORTALS] > 0xFFFF)
		return BSP_ERR_LAYOUT;
	s->data = d;
	s->size = size;
	s->material_count = cnt[T_MATERIALS];
	s->batch_count = cnt[T_BATCHES];
	s->cluster_count = cnt[T_CLUSTERS];
	s->cluster_batch_count = cnt[T_CBATCHES];
	s->cluster_portal_count = cnt[T_CPORTALS];
	s->portal_count = cnt[T_PORTALS];
	s->portal_vertex_count = cnt[T_PVERTS];
	s->node_count = cnt[T_NODES];
	s->plane_count = cnt[T_PLANES];
	s->leaf_count = cnt[T_LEAVES];
	s->materials = tab[T_MATERIALS];
	s->batches = tab[T_BATCHES];
	s->vif = tab[T_VIF];
	s->vif_size = cnt[T_VIF];
	s->clusters = tab[T_CLUSTERS];
	s->cluster_batches = tab[T_CBATCHES];
	s->cluster_portals = tab[T_CPORTALS];
	s->portals = tab[T_PORTALS];
	s->portal_vertices = tab[T_PVERTS];
	s->nodes = tab[T_NODES];
	s->planes = tab[T_PLANES];
	s->leaves = tab[T_LEAVES];

	uint64_t tris = 0, bsum = 0, mtris = 0;
	for (uint32_t i = 0; i < s->batch_count; i++) {
		bsp_batch b = bsp_get_batch(s, i);
		if (b.triangles < 1 || b.triangles > BSP_MAX_BATCH_TRIS || b.material >= s->material_count)
			return BSP_ERR_LAYOUT;
		if (b.vif_offset % 16 || b.vif_offset > s->vif_size || b.vif_qwords * 16ull != packet_bytes(b.triangles) ||
		    (uint64_t)b.vif_qwords * 16 > s->vif_size - b.vif_offset)
			return BSP_ERR_LAYOUT;
		if (!(b.radius >= 0.0f) || !packet_ok(s->vif + b.vif_offset, b.triangles)) return BSP_ERR_LAYOUT;
		tris += b.triangles;
	}
	for (uint32_t i = 0; i < s->material_count; i++) {
		bsp_material m = bsp_get_material(s, i);
		bsum += m.batch_count;
		mtris += m.triangles;
	}
	if (tris != s->triangle_count || mtris != tris || bsum != s->batch_count) return BSP_ERR_LAYOUT;

	for (uint32_t i = 0; i < s->cluster_batch_count; i++)
		if (rd16(s->cluster_batches + 2 * (size_t)i) >= s->batch_count) return BSP_ERR_LAYOUT;
	for (uint32_t i = 0; i < s->cluster_portal_count; i++)
		if (rd16(s->cluster_portals + 2 * (size_t)i) >= s->portal_count) return BSP_ERR_LAYOUT;
	for (uint32_t i = 0; i < s->cluster_count; i++) {
		bsp_cluster c = bsp_get_cluster(s, i);
		if ((uint64_t)c.first_batch + c.batch_count > s->cluster_batch_count ||
		    (uint64_t)c.first_portal + c.portal_count > s->cluster_portal_count)
			return BSP_ERR_LAYOUT;
	}
	for (uint32_t i = 0; i < s->portal_count; i++) {
		bsp_portal p = bsp_get_portal(s, i);
		if ((p.cluster_a != BSP_NO_CLUSTER && p.cluster_a >= s->cluster_count) ||
		    (p.cluster_b != BSP_NO_CLUSTER && p.cluster_b >= s->cluster_count) ||
		    (uint64_t)p.first_vertex + p.vertex_count > s->portal_vertex_count)
			return BSP_ERR_LAYOUT;
	}
	/* children must point forward (strictly later node) or to a leaf: the tree is acyclic, so a
	 * walk from the root ends in at most node_count steps */
	for (uint32_t i = 0; i < s->node_count; i++) {
		const uint8_t *n = s->nodes + (size_t)i * NODE_REC;
		if (rd32(n) >= s->plane_count) return BSP_ERR_LAYOUT;
		for (int k = 1; k <= 2; k++) {
			uint32_t c = rd32(n + 4 * k);
			if (c == NO_CHILD) continue;
			if (c & LEAF_FLAG) {
				if ((c & ~LEAF_FLAG) >= s->leaf_count) return BSP_ERR_LAYOUT;
			} else if (c <= i || c >= s->node_count) {
				return BSP_ERR_LAYOUT;
			}
		}
	}
	if (s->node_count == 0 && s->leaf_count > 1) return BSP_ERR_LAYOUT;
	for (uint32_t i = 0; i < s->leaf_count; i++) {
		uint32_t c = rd16(s->leaves + 2 * (size_t)i);
		if (c != BSP_NO_CLUSTER && c >= s->cluster_count) return BSP_ERR_LAYOUT;
	}
	return BSP_OK;
}

bsp_material bsp_get_material(const bsp_section *s, uint32_t i)
{
	const uint8_t *p = s->materials + (size_t)i * MATERIAL_REC;
	bsp_material m = { rd32(p), (uint16_t)rd16(p + 4), (uint16_t)rd16(p + 10), rd16(p + 8), rd32(p + 12), 0.0f };
	uint32_t bits = rd32(p + 16);
	memcpy(&m.uv_world, &bits, 4);
	if (!(m.uv_world >= 0.0f && m.uv_world < 1.0e6f)) m.uv_world = 0.0f;   /* NaN, negative or absurd: unknown */
	return m;
}

bsp_batch bsp_get_batch(const bsp_section *s, uint32_t i)
{
	const uint8_t *p = s->batches + (size_t)i * BATCH_REC;
	bsp_batch b = { (uint16_t)rd16(p), (uint16_t)rd16(p + 2), rd32(p + 4), rd32(p + 8),
	                { rdf(p + 12), rdf(p + 16), rdf(p + 20) }, rdf(p + 24) };
	return b;
}

const uint8_t *bsp_batch_vif(const bsp_section *s, const bsp_batch *b)
{
	return s->vif + b->vif_offset;
}

void bsp_batch_vertex(const bsp_section *s, const bsp_batch *b, uint32_t k, bsp_vertex *out)
{
	/* packet: [UNPACK a][32 B constants][UNPACK b][n x 8 pos][UNPACK c][n x 8 uv][UNPACK d][n x 4 colour] */
	const uint8_t *v = s->vif + b->vif_offset;
	uint32_t n = 3u * b->triangles;
	const uint8_t *c = v + 4, *pos = v + 40 + 8u * k, *uv = v + 44 + 8u * n + 8u * k, *col = v + 48 + 16u * n + 4u * k;
	float scale = rdf(c + 12);
	int i;
	for (i = 0; i < 3; i++) out->pos[i] = rdf(c + 4 * i) + (float)rds16(pos + 2 * i) * scale;
	for (i = 0; i < 4; i++) out->uv[i] = (int16_t)rds16(uv + 2 * i);
	for (i = 0; i < 4; i++) out->rgba[i] = col[i];
}

bsp_batch_raw bsp_batch_streams(const bsp_section *s, const bsp_batch *b)
{
	const uint8_t *v = s->vif + b->vif_offset;
	uint32_t n = 3u * b->triangles;
	bsp_batch_raw r;
	r.pos = (const int16_t *)(const void *)(v + 40);
	r.uv = (const int16_t *)(const void *)(v + 44 + 8u * n);
	r.col = v + 48 + 16u * n;
	r.centre[0] = rdf(v + 4); r.centre[1] = rdf(v + 8); r.centre[2] = rdf(v + 12); r.scale = rdf(v + 16);
	r.vertices = n;
	return r;
}

bsp_cluster bsp_get_cluster(const bsp_section *s, uint32_t i)
{
	const uint8_t *p = s->clusters + (size_t)i * CLUSTER_REC;
	bsp_cluster c = { { rdf(p), rdf(p + 4), rdf(p + 8) }, { rdf(p + 16), rdf(p + 20), rdf(p + 24) },
	                  (uint16_t)rd16(p + 12),
	                  (int16_t)rds16(p + 14), (int16_t)rds16(p + 28), (int16_t)rds16(p + 30),
	                  (int16_t)rds16(p + 48), (int16_t)rds16(p + 50),
	                  rd32(p + 32), rd32(p + 36), rd32(p + 40), rd32(p + 44), rd32(p + 52) };
	return c;
}

uint32_t bsp_cluster_batch(const bsp_section *s, const bsp_cluster *c, uint32_t k)
{
	return rd16(s->cluster_batches + 2 * ((size_t)c->first_batch + k));
}

uint32_t bsp_cluster_portal(const bsp_section *s, const bsp_cluster *c, uint32_t k)
{
	return rd16(s->cluster_portals + 2 * ((size_t)c->first_portal + k));
}

bsp_portal bsp_get_portal(const bsp_section *s, uint32_t i)
{
	const uint8_t *p = s->portals + (size_t)i * PORTAL_REC;
	bsp_portal r = { (uint16_t)rd16(p), (uint16_t)rd16(p + 2), (uint16_t)rd16(p + 4), (uint16_t)rd16(p + 6),
	                 rd32(p + 8), { rdf(p + 16), rdf(p + 20), rdf(p + 24), rdf(p + 28) },
	                 { rdf(p + 32), rdf(p + 36), rdf(p + 40) }, rdf(p + 44) };
	return r;
}

void bsp_portal_vertex(const bsp_section *s, const bsp_portal *p, uint32_t k, float out[3])
{
	const uint8_t *v = s->portal_vertices + ((size_t)p->first_vertex + k) * VERTEX_REC;
	out[0] = rdf(v);
	out[1] = rdf(v + 4);
	out[2] = rdf(v + 8);
}

int bsp_find_cluster(const bsp_section *s, const float pos[3])
{
	if (s->node_count == 0) {
		uint32_t c = s->leaf_count == 1 ? rd16(s->leaves) : BSP_NO_CLUSTER;
		return c == BSP_NO_CLUSTER ? -1 : (int)c;
	}
	uint32_t n = 0;
	for (uint32_t step = 0; step < s->node_count; step++) {
		const uint8_t *node = s->nodes + (size_t)n * NODE_REC;
		const uint8_t *pl = s->planes + (size_t)rd32(node) * PLANE_REC;
		float dist = rdf(pl) * pos[0] + rdf(pl + 4) * pos[1] + rdf(pl + 8) * pos[2] - rdf(pl + 12);
		uint32_t c = rd32(node + (dist >= 0.0f ? 4 : 8));      /* front, back */
		if (c == NO_CHILD) return -1;
		if (c & LEAF_FLAG) {
			uint32_t cl = rd16(s->leaves + 2 * (size_t)(c & ~LEAF_FLAG));
			return cl == BSP_NO_CLUSTER ? -1 : (int)cl;
		}
		n = c;
	}
	return -1;
}
