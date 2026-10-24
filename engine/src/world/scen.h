/* SCEN section reader (docs/13-scen-section.md): the objects a scenario places, their definitions,
 * the first-person (player) data and per-cluster object lighting. Contract between
 * tools/halo2ps2/scenconv.py (writer), world/scen.c (reader, host-tested, pure C11, no heap, no
 * double) and the engine's object table.
 *
 * Definitions are keyed by the Xbox tag id of the object tag (scenery, biped, vehicle, weapon, ...).
 * `model_tag` / `anim_tag` are Xbox tag ids that key the MODL and ANIM sections of the same pack. */
#ifndef WORLD_SCEN_H
#define WORLD_SCEN_H
#include <stddef.h>
#include <stdint.h>

#define SCEN_NAME_LEN 32
#define SCEN_NO_ANIM 0xFFFFu
#define SCEN_MAX_BSPS 32

typedef enum { SCEN_OK = 0, SCEN_ERR_TRUNCATED, SCEN_ERR_MAGIC, SCEN_ERR_LAYOUT, SCEN_ERR_VERSION } scen_status;

typedef enum {
	SCEN_SCENERY = 0, SCEN_BIPED, SCEN_VEHICLE, SCEN_WEAPON, SCEN_EQUIPMENT,
	SCEN_MACHINE, SCEN_CONTROL, SCEN_LIGHT_FIXTURE, SCEN_PROJECTILE, SCEN_OTHER
} scen_type;

/* placement.flags */
#define SCEN_P_INITIALLY_VISIBLE 1u   /* "not placed" objects (spawned by scripts) have this clear */

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       def_count, placement_count, bsp_count;
	const uint8_t *defs, *placements, *lights, *tables;
} scen_section;

typedef struct {
	uint32_t tag_id;                   /* object tag id */
	uint32_t model_tag;                /* MODL key, 0 = no model */
	uint32_t anim_tag;                 /* ANIM key, 0 = none */
	uint8_t  type;                     /* scen_type */
	uint8_t  pad;
	uint16_t idle_anim;                /* animation index in the graph to loop when placed (SCEN_NO_ANIM = static) */
	char     name[SCEN_NAME_LEN];      /* tag path tail, debugging only */
	float    radius;                   /* bounding radius (world units) of the default model, for culling */
} scen_def;

typedef struct {
	uint16_t def;                      /* index into the definitions */
	uint16_t flags;                    /* SCEN_P_* */
	uint32_t bsp_mask;                 /* bit n set: the placement lies inside BSP n of the map */
	float    pos[3];                   /* world units */
	float    rot[3];                   /* yaw, pitch, roll in radians (Halo order; see the doc) */
	char     variant[16];              /* permutation name, "" = default */
	int16_t  name_index;               /* scenario object-name index or -1 */
	uint16_t permutation;              /* the "desired permutation": selects the model permutations whose name ends in -<number> (Sergeant Johnson 100, the lab crew 10 / 12); 0 = random */
} scen_placement;

/* Object lighting: Halo samples the BSP lightmap for objects; here a baked ambient + one directional
 * term per BSP cluster (the "BSP lighting probe" of docs/03 §5.3). Colours are 0..255 (128 = 1.0). */
typedef struct {
	uint8_t ambient[3], pad0;
	uint8_t sun[3], pad1;              /* directional light colour */
	float   sun_dir[3];                /* unit vector TOWARDS the light, world space */
} scen_light;

/* The player's first-person set: arms and the default weapon. 0 = not found. */
typedef struct {
	uint32_t fp_arms_model_tag, fp_arms_anim_tag;
	uint32_t weapon_def;               /* definition index + 1 of the default weapon, 0 = none */
	uint32_t weapon_fp_model_tag, weapon_fp_anim_tag;
	uint32_t biped_def;                /* definition index + 1 of the player's biped (Master Chief), 0 = none */
	uint16_t arms_idle_anim, weapon_idle_anim;   /* animation indices (SCEN_NO_ANIM = none) */
	uint8_t  start_grenades[2], equip_grenades[2];   /* frag, plasma: of the first starting profile (the level's start) and of the profile that gives the weapon (player_add_equipment); old packs: 0 0 */
	uint16_t weapon_loaded, weapon_total;        /* rounds the starting profile that gives the weapon loads / carries (0 0: the weapon's own defaults; packs before ADR-085 have zeros) */
	char     weapon_hand_marker[SCEN_NAME_LEN];  /* marker on the arms' model the weapon attaches to */
	float    fp_offset[3];             /* camera-space offset (x right, y up, z forward) of the arms origin */
} scen_player;

scen_status scen_open(scen_section *s, const uint8_t *data, size_t size);
scen_def    scen_get_def(const scen_section *s, uint32_t index);
int         scen_find_def(const scen_section *s, uint32_t tag_id);        /* -1 if absent */
scen_placement scen_get_placement(const scen_section *s, uint32_t index);
/* Placements of BSP `bsp` are not contiguous; iterate all and test bsp_mask, or use this to count. */
uint32_t    scen_placements_in_bsp(const scen_section *s, uint32_t bsp);
/* Lighting of cluster `cluster` of BSP `bsp`; out is filled with a neutral default if the table has no entry. */
void        scen_cluster_light(const scen_section *s, uint32_t bsp, uint32_t cluster, scen_light *out);
scen_player scen_get_player(const scen_section *s);

#endif
