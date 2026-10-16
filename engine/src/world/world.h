/* Loading a map's BSP (and its textures) from .hp2 packs and wiring them to the renderer. */
#pragma once
#include "core/mem.h"
#include "render/bsp_render.h"
#include "world/bsp.h"
#include "world/anim.h"
#include "world/model.h"
#include "world/scen.h"
#include "world/tex.h"

typedef struct { tex_section tex; int has; } texset_t;

/* SKY0 section (tools/halo2ps2/skyconv.py): the scenario's sky model and its fog/ambient parameters */
typedef struct {
	int      valid;
	uint32_t model_tag;              /* 0 = fog only */
	float    fog_rgb[3], fog_near, fog_far, fog_density, outdoor[3], indoor[3];
} sky_info_t;

typedef struct {
	bsp_section bsp;
	int         bsp_index;
	texset_t    bsp_tex;
	lm_section  lm;
	int         has_lm;
	tex_source_t source;          /* BSP textures + lightmaps */
	/* object data (world_load_objects): models, animations, scenario, object textures */
	model_section models;
	anim_section  anims;
	scen_section  scen;
	texset_t      obj_tex;
	sky_info_t    sky;
	int           has_objects;
	tex_source_t  obj_source;
} world_t;

/* Decompresses section `tag` of the pack at `path` into `a` (64-byte aligned) without reading the
 * rest of the pack, and flushes it from the D-cache for DMA. Returns the buffer, or NULL; a missing
 * section is only reported when `optional` is 0. */
uint8_t *world_load_section(arena_t *a, const char *path, const char *tag, size_t *raw_size, int optional);

/* Loads BSP `bsp_index` from `pack_path`, and TEXS + LMn from `tex_path` when given. Returns 0 on success;
 * missing or broken texture data is reported and the world draws flat. */
int world_load(world_t *w, arena_t *a, const char *pack_path, const char *tex_path, int bsp_index);

/* Loads MODL, OTEX, ANIM and SCEN from the object pack `obj_path` (tools/halo2ps2/objpack.py). Returns 0 when the
 * models and the scenario are usable; animations and object textures are optional (static / flat objects). */
int world_load_objects(world_t *w, arena_t *a, const char *obj_path);
