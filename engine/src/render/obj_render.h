/* Object render pass: draws the object table and the first-person view model through the VU1 model program
 * (vu1/model.vsm). Installed with bsp_render_set_object_pass(obj_render_pass, &obj_render, tex source). */
#ifndef RENDER_OBJ_RENDER_H
#define RENDER_OBJ_RENDER_H
#include "sim/fx.h"
#include "render/lodctl.h"
#include "render/render_internal.h"
#include "sim/fpview.h"
#include "sim/objects.h"
#include "world/bsp.h"
#include "world/cchg.h"
#include "world/hires.h"

typedef struct {
	const object_table_t *table;
	object_world_t        world;
	const bsp_section    *bsp;
	int                   bsp_index;
	const tex_source_t   *textures;      /* OTEX binding source (NULL: objects draw flat) */
	fpview_t             *fp;            /* first-person view (NULL: none) */
	lodctl_t             *lod;           /* dynamic LOD controller (NULL: fixed quality) */
	float                 max_distance;  /* draw distance in world units at quality level 0 */
	float                 far_px;        /* objects past the draw distance are still drawn when they are at least this many pixels across (640 px scale; cfg objfarpx) */
	unsigned              max_batches;   /* per-frame cap */
	int                   draw_objects, draw_fp;
	int                   hide_fp;             /* this frame: a cinematic runs or a script camera renders (set by main) */
	const cchg_t         *cchg;          /* colour change records (NULL: none) */
	int                   sky_model;     /* MODL index of the sky model, -1 = none */
	int                   sky_model2;    /* the scenario's second sky (a cluster with sky index 1 draws it), -1 = none */
	float                 sky_ms;        /* EE time of the sky pass */
	float                 sky_rot[3];    /* extra sky rotation yaw, pitch, roll in radians (debug / calibration: config key skyrot) */
	float                 sky_yaw0;                           /* extra yaw (radians) of the palette-0 sky only: cfg skyyaw in degrees */
	unsigned              frame_no, trace_lo, trace_hi, trace_step;   /* cfg objtrace: log every object's cull inputs for frames [lo, hi) every `step` frames */
	int                   dbg_obj;                           /* debug (cfg objdbg): log the visibility test of this object index each frame (-1 none) */
	float                 sky_planet_gain;                   /* colour gain (1 .. 1.99) of the sky's non-additive parts (the planet): cfg skyplanet */
	uint8_t               cc_perm[1280];                      /* per object index: the permutation of change colour a (0xFF = pick from the seed): cfg ccperm "obj:perm ..." */
	const fx_t            *fx;                               /* dynamic lights of attached fixtures (alert lights, flames): objects near them take their colour */
	float                 dl_gain, light_gain[3], light_chroma, amb_gain;             /* cfg dlobj (weight of dynamic lights on objects, 1.0) / objlight (rgb multiplier of the cluster lighting, 1 1 1) */
	hires_t              *hires;                             /* streamed high resolution variants (NULL: none), their texcache key range, and the on-screen diameter (640 px scale) from which an object asks for them */
	unsigned              hires_key_base;
	float                 hires_px;
	int                   want_hi;                            /* set per object by obj_render_pass */
	int                   unlit_add;                          /* additive parts ignore the object lighting (cfg unlitadd, default 1) */
	unsigned              sky_hide;                           /* debug (cfg skyhide): bit r hides region r of the sky model */
	/* statistics of the last pass, copied into render_stats by the caller */
	float                 st_ms;         /* EE time of the pass */
	unsigned              st_visible, st_culled, st_batches, st_triangles, st_skipped;
} obj_render_t;

void obj_render_pass(void *user, struct render_ctx *rc);
/* Sky: the sky model drawn on the EE (projected and clipped like the BSP's near triangles), centred on the camera,
 * Z test always and no Z write, before the BSP. Installed with bsp_render_set_sky_pass. */
void obj_render_sky(void *user, struct render_ctx *rc);

#endif
