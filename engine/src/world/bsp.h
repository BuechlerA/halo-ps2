/* BSPS section reader (docs/08-bsp-section.md). Validates the tables of a section already
 * decompressed into memory and gives bounds-checked access to batches. No ps2sdk dependency. */
#ifndef WORLD_BSP_H
#define WORLD_BSP_H
#include <stddef.h>
#include <stdint.h>

#define BSP_MAX_BATCH_TRIS 32
#define BSP_NO_LIGHTMAP 0xFFFFu

typedef struct {
	uint32_t shader_id;
	uint16_t lightmap_page, first_batch, batch_count, uv_shift;
	uint32_t triangles;
} bsp_material;

typedef struct {
	uint16_t material, triangles;
	uint32_t vif_offset, vif_qwords;   /* into the VIF data; 16-byte aligned */
	float    centre[3], radius;
} bsp_batch;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       material_count, batch_count, triangle_count;
	float          bounds[6];          /* min xyz, max xyz */
	const uint8_t *materials, *batches, *vif;
	size_t         vif_size;
} bsp_section;

typedef enum { BSP_OK = 0, BSP_ERR_TRUNCATED, BSP_ERR_MAGIC, BSP_ERR_LAYOUT } bsp_status;

bsp_status bsp_open(bsp_section *s, const uint8_t *data, size_t size);
bsp_material bsp_get_material(const bsp_section *s, uint32_t i);   /* i < material_count */
bsp_batch bsp_get_batch(const bsp_section *s, uint32_t i);         /* i < batch_count */
/* Pointer to the batch's VIF packet (batch.vif_qwords * 16 bytes). */
const uint8_t *bsp_batch_vif(const bsp_section *s, const bsp_batch *b);

#endif
