/* MODL section reader (docs/11-model-section.md): object models converted from Xbox `model` tags.
 * Contract between tools/halo2ps2/modelconv.py (writer), world/model.c (reader, host-tested, pure
 * C11, no heap, no double, bounds-validating) and render/model_render.c (consumer, VU1 model.vsm).
 *
 * A model has a skeleton (nodes), markers, and renderable geometry organised as
 * region x permutation x LOD -> parts -> batches. A batch is one VIF packet (<= 24 triangles,
 * <= MODEL_MAX_PALETTE distinct nodes) laid out for vu1/model.vsm:
 *
 *   UNPACK V4-32  addr 0,    2 qw   qw0 = f32 centre.xyz, scale (decoded position = centre + s16 * scale,
 *                                    in model space); qw1 = s32 vertex count n (= 3 * triangles) in x
 *   UNPACK V4-16  addr 4,    n      x y z, w = palette slot of the vertex's first node   (signed s16)
 *   UNPACK V4-16  addr 4+72, n      s t (texture uv * 2^uv_shift, signed), slot of the second node,
 *                                   weight of the FIRST node (0..32767 = 0..1); rigid: slot1 = slot0, weight 32767
 *   UNPACK V4-8   addr 4+144,n      normal x y z (signed, 127 = 1.0), w = 0     (cmd 0x6E with the signed flag)
 *   (all TOPS-relative, then zero padding to a quadword; the engine appends MSCAL)
 *
 * Triangles are expanded lists like BSPS batches. `uv_shift` of a part is in the part record.
 * Palette slots index `model_batch.palette[]` (node indices of the model); the renderer uploads the
 * world-space matrices of exactly those nodes before the batch is drawn. */
#ifndef WORLD_MODEL_H
#define WORLD_MODEL_H
#include <stddef.h>
#include <stdint.h>

#define MODEL_LODS 5                  /* Halo's super low .. super high; index 0 = highest detail here */
#define MODEL_MAX_PALETTE 24
#define MODEL_MAX_BATCH_TRIS 24
#define MODEL_MAX_NODES 64
#define MODEL_NAME_LEN 32
#define MODEL_NO_NODE (-1)

typedef enum { MODEL_OK = 0, MODEL_ERR_TRUNCATED, MODEL_ERR_MAGIC, MODEL_ERR_LAYOUT, MODEL_ERR_VERSION } model_status;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       model_count, part_count, batch_count;
	const uint8_t *models, *parts, *batches, *vif, *nodes, *markers, *ranges;
	size_t         vif_size;
	uint32_t       generation;                  /* counts model_open calls: caches keyed on a section (the bind-pose inverses) drop when it is reopened */
} model_section;

typedef struct {
	uint32_t tag_id;                      /* Xbox tag id of the `model` tag (key used by the scenario) */
	uint16_t node_count, marker_count;
	uint16_t region_count, permutation_count;   /* permutation_count = total (region, permutation) pairs */
	uint32_t first_node, first_marker, first_range;   /* into the section's tables */
	float    centre[3], radius;           /* bounding sphere in model space (default pose) */
	float    lod_cutoff[MODEL_LODS];      /* as stored in the tag; semantics: see model_pick_lod() */
	uint32_t triangles_lod0;              /* statistics: triangles of one full set at LOD 0 */
} model_info;

typedef struct {
	char    name[MODEL_NAME_LEN];
	int16_t parent, first_child, next_sibling, pad;   /* MODEL_NO_NODE = none */
	float   translation[3], rotation[4], scale;       /* default pose (rotation: quaternion x y z w) */
} model_node;

typedef struct {
	char    name[MODEL_NAME_LEN];
	uint16_t region, permutation;         /* indices the marker belongs to */
	int16_t node, pad;
	float   translation[3], rotation[4];
} model_marker;

typedef struct {
	uint16_t first_part, part_count;      /* parts of one (region, permutation) at one LOD */
} model_part_range;

typedef struct {
	uint32_t shader_id;                   /* Xbox shader tag id: binding key in the OTEX section */
	uint16_t first_batch, batch_count;
	uint16_t triangles, uv_shift;
	int16_t  rigid_node;                  /* >= 0: whole part follows this node; MODEL_NO_NODE: skinned */
	uint16_t flags;                       /* bit 0 alpha-tested, bit 1 blended */
	float    centre[3], radius;
} model_part;

typedef struct {
	uint16_t part, triangles;
	uint8_t  palette_count, pad[3];
	uint32_t vif_offset, vif_qwords;      /* into the VIF data; 16-byte aligned */
	uint8_t  palette[MODEL_MAX_PALETTE];  /* model node index per palette slot */
} model_batch;

model_status model_open(model_section *s, const uint8_t *data, size_t size);
/* Index of the model with this Xbox tag id, or -1. Binary search (models are sorted by tag id). */
int          model_find(const model_section *s, uint32_t tag_id);
model_info   model_get(const model_section *s, uint32_t index);
model_node   model_get_node(const model_section *s, const model_info *m, uint32_t node);
model_marker model_get_marker(const model_section *s, const model_info *m, uint32_t marker);
/* First marker named `name`, or -1. */
int          model_find_marker(const model_section *s, const model_info *m, const char *name);
/* The model's permutations are numbered 0..m->permutation_count-1 in region order; this gives the parts
 * of permutation `perm` (a flat index) at `lod`; region_of_perm gives its region. */
model_part_range model_parts(const model_section *s, const model_info *m, uint32_t perm, uint32_t lod);
uint32_t     model_perm_region(const model_section *s, const model_info *m, uint32_t perm);
/* Name of permutation `perm` (e.g. "base", "damaged"). */
void         model_perm_name(const model_section *s, const model_info *m, uint32_t perm, char out[MODEL_NAME_LEN]);
/* Choose one permutation per region for an object placed with `variant` (may be NULL or ""): in each
 * region the permutation whose name equals the variant, else the first one. out[region_count] = flat
 * permutation index. Documented in docs/11-model-section.md. */
void         model_select_permutations(const model_section *s, const model_info *m, const char *variant, uint16_t *out);
/* Permutations by number (docs/13): a permutation's name may end in "-<number>" (`sgt_johnson-100`, `head_charles_helmet-10`, `head_marcus_cap-101`, `damaged torso-102`).
 * Per region: with `desired` != 0 the permutation whose number matches, if the region has one; otherwise a deterministic pick (from `seed`) among the permutations numbered
 * 1 .. 99 (the random pool: the marines' and the crew's heads 10 .. 14); a region without such permutations takes its first (`__base`). Numbers from 100 up are only ever
 * chosen by a matching `desired`. */
void         model_select_variant(const model_section *s, const model_info *m, unsigned desired, uint32_t seed, uint16_t *out);
/* The number a name ends in ("head_matt_helmet-12" -> 12), 0 when it has none. */
unsigned     model_name_number(const char *name);
model_part   model_get_part(const model_section *s, uint32_t part_index);
model_batch  model_get_batch(const model_section *s, uint32_t batch_index);
const uint8_t *model_batch_vif(const model_section *s, const model_batch *b);
/* LOD for an object whose bounding sphere projects to `diameter_px` pixels (of a 640-wide frame): the
 * highest-detail LOD whose cutoff semantics are satisfied (semantics verified and documented by the
 * converter; the result is clamped to LODs that exist for the model). */
uint32_t     model_pick_lod(const model_info *m, float diameter_px);

#endif
