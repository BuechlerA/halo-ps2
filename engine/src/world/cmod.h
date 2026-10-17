/* CMOD section reader (docs/16-cmod-section.md): collision models of objects for hit detection and damage
 * regions. Contract between tools/halo2ps2/cmodconv.py (writer), world/cmod.c (reader, host-tested, pure C11,
 * no heap, no double) and the engine's damage code.
 *
 * Keyed by the Xbox tag id of the collision_model (`cmod_find`) and of the object tag that uses it
 * (`cmod_find_object`: scenery, biped, vehicle, weapon, equipment, device machine / control, light fixture,
 * garbage). Units: world units (1 wu = 10 ft), vitality in the tag's own units (a Master Chief has 75 body,
 * 75 shield), times in seconds, rates per 1/30 s tick.
 *
 * Geometry: per (region, permutation, node, surface material) one conservative oriented box in the NODE's own
 * frame at the model's default pose, built from the collision model's own geometry (see the doc: every source
 * vertex lies inside its box). A posed shape is that box moved with the node's WORLD matrix (node-local -> world,
 * what `skel_world` produces: the pose chain including the object's placement, NOT multiplied by the inverse bind
 * pose). The collision model has the same nodes, in the same order, as the object's render model.
 * Boxes: u_k = axis[k] . (p - center) for p in node-local space; p is inside when |u_k| <= half[k] for k = 0..2. */
#ifndef WORLD_CMOD_H
#define WORLD_CMOD_H
#include <stddef.h>
#include <stdint.h>
#include "core/mat.h"

#define CMOD_NAME_LEN 32
#define CMOD_MAX_NODES 64          /* = SKEL_MAX_NODES: a node matrix array of this size always suffices */
#define CMOD_MAX_REGIONS 64        /* a `uint8_t perm_sel[CMOD_MAX_REGIONS]` and a uint64_t region mask suffice */
#define CMOD_MAX_PERMS 64          /* per region */
#define CMOD_MAX_MATERIALS 256

typedef enum { CMOD_OK = 0, CMOD_ERR_TRUNCATED, CMOD_ERR_MAGIC, CMOD_ERR_LAYOUT, CMOD_ERR_VERSION } cmod_status;

/* model flags (names from the public tag documentation; the values are the tag's) */
#define CMOD_F_SHIELD_FOR_CHILDREN  0x01u
#define CMOD_F_BODY_FOR_CHILDREN    0x02u
#define CMOD_F_ALWAYS_SHIELD_FRIEND 0x04u
#define CMOD_F_AREA_TO_CHILDREN     0x08u
#define CMOD_F_PARENT_NO_BODY       0x10u
#define CMOD_F_ONLY_EXPLOSIVES      0x20u
#define CMOD_F_ONLY_WHEN_OCCUPIED   0x40u
/* region flags */
#define CMOD_RF_LIVES_UNTIL_DEATH   0x001u
#define CMOD_RF_FORCES_DEATH        0x002u
#define CMOD_RF_DIES_WITH_OBJECT    0x004u
#define CMOD_RF_DIES_WHEN_DAMAGED   0x008u
#define CMOD_RF_VANISH_SHIELD_OFF   0x010u
#define CMOD_RF_NO_MELEE            0x020u
#define CMOD_RF_NO_WEAPON           0x040u
#define CMOD_RF_NO_WALKING          0x080u
#define CMOD_RF_DROPS_WEAPON        0x100u
#define CMOD_RF_HEAD_SCREAM         0x200u
/* material flags / shape flags */
#define CMOD_MF_HEAD 1u
#define CMOD_SF_HEAD 1u            /* the shape's material has CMOD_MF_HEAD */

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       model_count, object_count, material_count, region_count, perm_count, node_count, shape_count;
	const uint8_t *models, *objects, *materials, *regions, *perms, *nodes, *shapes;
} cmod_section;

/* One collision model; a plain value, valid as long as the section buffer lives. */
typedef struct {
	const cmod_section *sec;
	uint32_t index;                    /* index in the section's model table */
	uint32_t tag_id;                   /* Xbox collision_model tag id */
	uint32_t flags;                    /* CMOD_F_* */
	int16_t  indirect_damage_material; /* -1 none, else an index into globals' material table */
	uint16_t shield_material;          /* index into globals' material table */
	uint16_t node_count, region_count, material_count;
	uint32_t shape_count;
	uint32_t first_material, first_region, first_node, first_shape;   /* absolute indices into the section's tables */
	float    max_body_vitality;        /* 0 where the tag leaves it unset (vehicles, props) */
	float    body_system_shock;
	float    body_destroyed_threshold;
	float    max_shield_vitality;      /* 0 = no shield; 1e10 = effectively invulnerable (the monitor) */
	float    shield_stun_time;         /* seconds after damage before the shield recharges (inferred, see doc) */
	float    shield_recharge_time;     /* seconds to refill */
	float    shield_recharge_rate;     /* fraction of the maximum per 1/30 s tick = 1 / (30 x recharge time); 1.0 with no shield */
	uint32_t source_vertices;          /* statistics */
} cmod_model;

typedef struct {
	uint32_t tag_id;                   /* object tag id */
	uint32_t coll_tag;                 /* its collision model */
	uint32_t model_index;              /* index of that model in the section */
	uint8_t  type;                     /* the object tag's own type value (scenery 6, biped 0, vehicle 1, weapon 2, equipment 3, machine 7, control 8, light fixture 9) */
	float    radius;                   /* tag bounding radius: NOT a bounding sphere of the collision geometry (see the doc) */
	float    offset[3];                /* tag bounding offset */
	float    origin_offset[3];
	float    render_radius;            /* tag render bounding radius */
} cmod_object;

typedef struct {
	char     name[CMOD_NAME_LEN];
	uint16_t flags;                    /* CMOD_MF_* */
	uint16_t material_type;            /* index into globals' material table (impact response) */
	float    shield_leak;              /* 0 where the tag does not author it */
	float    shield_multiplier;        /* damage multipliers; 0 = not authored (vehicles, devices, props) */
	float    body_multiplier;
} cmod_material;

typedef struct {
	char     name[CMOD_NAME_LEN];
	uint32_t flags;                    /* CMOD_RF_* */
	float    damage_threshold;
	uint32_t destroyed_effect;         /* Xbox effect tag id, 0 = none */
	uint16_t first_perm, perm_count;   /* permutations of this region: first_perm is an absolute index (cmod_perm_name) */
} cmod_region;

typedef struct {
	int16_t region;                    /* -1: the node has no collision geometry */
	int16_t parent, first_child, next_sibling;
} cmod_node;

typedef struct {
	uint16_t node, region, perm, material;   /* material: index into the model's materials; perm: index within the region */
	uint16_t flags;                    /* CMOD_SF_* */
	uint32_t source_vertices;
	float    center[3], half[3], axes[3][3];
} cmod_shape;

typedef struct { float a[3], b[3]; } cmod_segment;   /* world space, from a to b; t = 0 at a, 1 at b */

typedef struct {
	float    t;                        /* 0..1 along the segment; 0 when the segment starts inside the shape */
	uint16_t region, node, perm, material;
	uint16_t shape_flags;              /* CMOD_SF_* of the shape hit */
	uint32_t shape;                    /* index within the model's shapes */
	uint32_t region_flags;             /* CMOD_RF_* of the region hit */
	float    body_multiplier, shield_multiplier, shield_leak;    /* of the material hit */
} cmod_hit;

/* Validates every table and cross reference; a failed open leaves *s zeroed (every accessor then reports "not found"). */
cmod_status cmod_open(cmod_section *s, const uint8_t *data, size_t size);

/* Models: by index, by collision model tag id (binary search), by object tag id. 1 = found. */
int cmod_get_model(const cmod_section *s, uint32_t index, cmod_model *out);
int cmod_find(const cmod_section *s, uint32_t coll_tag_id, cmod_model *out);
int cmod_get_object(const cmod_section *s, uint32_t index, cmod_object *out);
/* `obj` and `model` may each be NULL. */
int cmod_find_object(const cmod_section *s, uint32_t object_tag_id, cmod_object *obj, cmod_model *model);

/* Tables of a model. An index out of range returns a zeroed record (region -1 for nodes). */
cmod_material cmod_get_material(const cmod_model *m, uint32_t index);
cmod_region   cmod_get_region(const cmod_model *m, uint32_t index);
cmod_node     cmod_get_node(const cmod_model *m, uint32_t index);
cmod_shape    cmod_get_shape(const cmod_model *m, uint32_t index);
/* Name of permutation `perm` of `region` into out[CMOD_NAME_LEN]; 0 if absent (out is then ""). */
int cmod_perm_name(const cmod_model *m, uint32_t region, uint32_t perm, char *out);
/* Index within `region` of the permutation called `name` (exact, case sensitive), -1 if none. A render permutation name
 * ("~damaged") that the collision model does not have means "use permutation 0". */
int cmod_find_permutation(const cmod_model *m, uint32_t region, const char *name);

/* Nearest hit of a world-space segment against the posed model. node_world[i] is node i's node-local -> world matrix
 * (at least m->node_count entries; rigid or uniformly scaled; a degenerate matrix drops its shapes). perm_sel[region]
 * picks the permutation of each region (NULL = permutation 0 everywhere; a value past the region's permutations
 * counts as 0). Returns 1 and fills *hit, or 0 for no hit (also for a non-finite segment). The ray starting inside a
 * shape hits at t = 0. */
int cmod_ray_ex(const cmod_model *m, const mat4 *node_world, const cmod_segment *seg, const uint8_t *perm_sel, cmod_hit *hit);
/* The contract form: permutation 0 everywhere, only the region and t. */
int cmod_ray(const cmod_model *m, const mat4 *node_world, const cmod_segment *seg, uint16_t *out_region, float *out_t);
/* Area damage: which regions have a shape within `radius` of world point `c` (bit r of *regions = region r). Returns the
 * number of shapes touched. */
uint32_t cmod_sphere(const cmod_model *m, const mat4 *node_world, const float c[3], float radius,
                     const uint8_t *perm_sel, uint64_t *regions);

#endif
