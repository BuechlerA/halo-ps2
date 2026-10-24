/* BSPS section reader, format version 2 (docs/08-bsp-section.md). Validates every table of a
 * section already decompressed into memory and gives bounds-checked access to batches, clusters,
 * portals and the point-location tree. Pure C11, no ps2sdk dependency, no heap, no double.
 * Version 1 sections (no cluster data) are rejected with BSP_ERR_VERSION. */
#ifndef WORLD_BSP_H
#define WORLD_BSP_H
#include <stddef.h>
#include <stdint.h>

#define BSP_VERSION 2
#define BSP_MAX_BATCH_TRIS 24
#define BSP_MAX_BATCH_VERTS (BSP_MAX_BATCH_TRIS * 3)
#define BSP_VU_QWORDS (4 + 3 * BSP_MAX_BATCH_VERTS)   /* VU1 input buffer per batch: 220 */
#define BSP_NO_LIGHTMAP 0xFFFFu
#define BSP_NO_CLUSTER 0xFFFFu                         /* portal side without a cluster */

/* bsp_cluster.flags */
#define BSP_CLUSTER_SKY 1u         /* sky visible from the cluster (draw the sky when it is visible) */
#define BSP_CLUSTER_FOG 2u         /* bsp_cluster.fog is valid */
#define BSP_CLUSTER_WEATHER 4u
#define BSP_CLUSTER_MIRROR 8u      /* has mirror surfaces (not converted) */
#define BSP_CLUSTER_FOG_BIT15 16u  /* bit 15 of the source fog field (meaning unknown) */

typedef struct {
	uint32_t shader_id;
	uint16_t lightmap_page, uv_shift;
	uint32_t batch_count, triangles;
	float    uv_world;            /* world units per texture uv unit (+0x10; 0 = unknown): the streamed texture variants' texel density test, docs/37 */
} bsp_material;

typedef struct {
	uint16_t material, triangles;
	uint32_t vif_offset, vif_qwords;   /* into the VIF data; 16-byte aligned */
	float    centre[3], radius;
} bsp_batch;

typedef struct {
	float    bounds_min[3], bounds_max[3];
	uint16_t flags;
	int16_t  fog, sky, weather, background_sound, sound_environment;   /* -1 = none */
	uint32_t first_batch, batch_count;     /* range in the cluster batch list */
	uint32_t first_portal, portal_count;   /* range in the cluster portal list */
	uint32_t triangles;                    /* sum over the cluster's batches */
} bsp_cluster;

/* Portal polygon between two clusters. plane: n.p = d, n unit length, cluster_a lies on the
 * positive side (n.p - d >= 0), cluster_b on the negative side; BSP_NO_CLUSTER if there is none.
 * The polygon is convex, counter-clockwise seen from the positive side. */
typedef struct {
	uint16_t cluster_a, cluster_b, vertex_count, flags;
	uint32_t first_vertex;
	float    plane[4], centre[3], radius;
} bsp_portal;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       material_count, batch_count, triangle_count;
	uint32_t       cluster_count, portal_count, portal_vertex_count;
	uint32_t       node_count, plane_count, leaf_count;
	uint32_t       cluster_batch_count, cluster_portal_count;
	float          bounds[6];          /* min xyz, max xyz */
	const uint8_t *materials, *batches, *vif, *clusters, *cluster_batches, *cluster_portals,
	              *portals, *portal_vertices, *nodes, *planes, *leaves;
	size_t         vif_size;
} bsp_section;

typedef enum { BSP_OK = 0, BSP_ERR_TRUNCATED, BSP_ERR_MAGIC, BSP_ERR_LAYOUT, BSP_ERR_VERSION } bsp_status;

bsp_status bsp_open(bsp_section *s, const uint8_t *data, size_t size);
bsp_material bsp_get_material(const bsp_section *s, uint32_t i);   /* i < material_count */
bsp_batch bsp_get_batch(const bsp_section *s, uint32_t i);         /* i < batch_count */
/* Pointer to the batch's VIF packet (batch.vif_qwords * 16 bytes): four TOPS-relative UNPACKs
 * (constants, positions, uv, colours), see the doc. The engine appends MSCAL. */
const uint8_t *bsp_batch_vif(const bsp_section *s, const bsp_batch *b);

/* One decoded vertex of a batch: world position, uv exactly as baked (texture s t scaled by the
 * material's 2^uv_shift, lightmap u v = value / 32767) and colour (0x80 = 1.0). */
typedef struct {
	float   pos[3];
	int16_t uv[4];
	uint8_t rgba[4];
} bsp_vertex;
/* Vertex k (< 3 * b->triangles) of batch b; the three vertices of triangle t are 3t .. 3t+2. */
void bsp_batch_vertex(const bsp_section *s, const bsp_batch *b, uint32_t k, bsp_vertex *out);

/* Direct pointers into a batch packet for hot loops (little-endian host only): positions are
 * s16 x y z 0 per vertex (world = centre + value * scale), uv s16 s t lmu lmv, colour u8 rgba. */
typedef struct {
	const int16_t *pos, *uv;
	const uint8_t *col;
	float          centre[3], scale;
	uint32_t       vertices;
} bsp_batch_raw;
bsp_batch_raw bsp_batch_streams(const bsp_section *s, const bsp_batch *b);

bsp_cluster bsp_get_cluster(const bsp_section *s, uint32_t i);     /* i < cluster_count */
/* k-th batch / portal of a cluster (k < c->batch_count / c->portal_count). A batch shared by
 * several clusters is listed by each of them; de-duplicate with a per-frame stamp. */
uint32_t bsp_cluster_batch(const bsp_section *s, const bsp_cluster *c, uint32_t k);
uint32_t bsp_cluster_portal(const bsp_section *s, const bsp_cluster *c, uint32_t k);
bsp_portal bsp_get_portal(const bsp_section *s, uint32_t i);       /* i < portal_count */
void bsp_portal_vertex(const bsp_section *s, const bsp_portal *p, uint32_t k, float out[3]);

/* Cluster containing pos, or -1 outside the map (and for positions in solid space). */
int bsp_find_cluster(const bsp_section *s, const float pos[3]);

#endif
