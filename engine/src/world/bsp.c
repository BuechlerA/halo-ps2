#include "world/bsp.h"
#include <string.h>

#define HEADER_SIZE 64u
#define MATERIAL_REC 32u
#define BATCH_REC 32u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

bsp_status bsp_open(bsp_section *s, const uint8_t *d, size_t size)
{
	memset(s, 0, sizeof *s);
	if (size < HEADER_SIZE) return BSP_ERR_TRUNCATED;
	if (memcmp(d, "BSPS", 4) != 0 || rd16(d + 4) != 1) return BSP_ERR_MAGIC;
	s->material_count = rd32(d + 8);
	s->batch_count = rd32(d + 12);
	s->triangle_count = rd32(d + 16);
	uint32_t mo = rd32(d + 20), bo = rd32(d + 24), vo = rd32(d + 28);
	for (int i = 0; i < 6; i++) s->bounds[i] = rdf(d + 32 + 4 * i);
	if (rd32(d + 56) != size) return BSP_ERR_LAYOUT;
	if (s->material_count > 0xFFFF || s->batch_count > 0xFFFF) return BSP_ERR_LAYOUT;
	if (mo % 16 || bo % 16 || vo % 16 || vo > size) return BSP_ERR_LAYOUT;
	if (mo > size || (size_t)s->material_count * MATERIAL_REC > size - mo) return BSP_ERR_TRUNCATED;
	if (bo > size || (size_t)s->batch_count * BATCH_REC > size - bo) return BSP_ERR_TRUNCATED;
	s->data = d;
	s->size = size;
	s->materials = d + mo;
	s->batches = d + bo;
	s->vif = d + vo;
	s->vif_size = size - vo;

	uint64_t tris = 0;
	for (uint32_t i = 0; i < s->batch_count; i++) {
		bsp_batch b = bsp_get_batch(s, i);
		if (b.triangles > BSP_MAX_BATCH_TRIS || b.material >= s->material_count) return BSP_ERR_LAYOUT;
		if (b.vif_offset % 16 || b.vif_offset > s->vif_size ||
		    (uint64_t)b.vif_qwords * 16 > s->vif_size - b.vif_offset) return BSP_ERR_LAYOUT;
		tris += b.triangles;
	}
	uint64_t mtris = 0;
	for (uint32_t i = 0; i < s->material_count; i++) {
		bsp_material m = bsp_get_material(s, i);
		if ((uint32_t)m.first_batch + m.batch_count > s->batch_count) return BSP_ERR_LAYOUT;
		mtris += m.triangles;
	}
	return tris == s->triangle_count && mtris == tris ? BSP_OK : BSP_ERR_LAYOUT;
}

bsp_material bsp_get_material(const bsp_section *s, uint32_t i)
{
	const uint8_t *p = s->materials + (size_t)i * MATERIAL_REC;
	bsp_material m = { rd32(p), (uint16_t)rd16(p + 4), (uint16_t)rd16(p + 6), (uint16_t)rd16(p + 8),
	                   (uint16_t)rd16(p + 10), rd32(p + 12) };
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
