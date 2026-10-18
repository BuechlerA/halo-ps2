/* Object render pass: draws the object table and the first-person view model through the VU1 model program
 * (vu1/model.vsm). Installed with bsp_render_set_object_pass(obj_render_pass, &obj_render, tex source). */
#ifndef RENDER_OBJ_RENDER_H
#define RENDER_OBJ_RENDER_H
#include "render/lodctl.h"
#include "render/render_internal.h"
#include "sim/fpview.h"
#include "sim/objects.h"
#include "world/bsp.h"
#include "world/cchg.h"

typedef struct {
	const object_table_t *table;
	object_world_t        world;
	const bsp_section    *bsp;
	int                   bsp_index;
	const tex_source_t   *textures;      /* OTEX binding source (NULL: objects draw flat) */
	fpview_t             *fp;            /* first-person view (NULL: none) */
	lodctl_t             *lod;           /* dynamic LOD controller (NULL: fixed quality) */
	float                 max_distance;  /* draw distance in world units at quality level 0 */
	unsigned              max_batches;   /* per-frame cap */
	int                   draw_objects, draw_fp;
	int                   hide_fp;             /* this frame: a cinematic runs or a script camera renders (set by main) */
	const cchg_t         *cchg;          /* colour change records (NULL: none) */
	int                   sky_model;     /* MODL index of the sky model, -1 = none */
	int                   sky_model2;    /* the scenario's second sky (a cluster with sky index 1 draws it), -1 = none */
	float                 sky_ms;        /* EE time of the sky pass */
	/* statistics of the last pass, copied into render_stats by the caller */
	float                 st_ms;         /* EE time of the pass */
	unsigned              st_visible, st_culled, st_batches, st_triangles, st_skipped;
} obj_render_t;

void obj_render_pass(void *user, struct render_ctx *rc);
/* Sky: the sky model drawn on the EE (projected and clipped like the BSP's near triangles), centred on the camera,
 * Z test always and no Z write, before the BSP. Installed with bsp_render_set_sky_pass. */
void obj_render_sky(void *user, struct render_ctx *rc);

#endif
