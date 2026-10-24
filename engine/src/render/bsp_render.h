/* Static-world renderer: visibility -> material sort -> texture residency -> one VIF1 chain per
 * frame (docs/03 §5.1), drawn by the VU1 programs in vu1/bsp.vsm in two passes (base texture ×
 * vertex colour, then lightmap multiply). Frames are pipelined: bsp_render_build() runs on the EE
 * while the GS still draws the previous frame; bsp_render_kick() starts the built frame. */
#pragma once
#include <tamtypes.h>
#include "core/mem.h"
#include "platform/video.h"
#include "render/camera.h"
#include "render/text.h"
#include "render/texcache.h"
#include "render/vis.h"
#include "world/bsp.h"
#include "world/hires.h"

#define MAT_ALPHA_TEST 1u
#define MAT_BLEND      2u
#define MAT_TWO_SIDED  4u
#define MAT_SKY        8u

typedef struct {
	int16_t  tex;               /* texture index, -1 = untextured (flat) */
	uint8_t  flags;             /* MAT_* */
	uint8_t  add;               /* 1: additive blend (colour + framebuffer); the other blends all draw as alpha blends */
	float    uv_scale[2];       /* multiplies the material's baked uv (shader tiling) */
	uint8_t  cc;                /* colour change 1..4 (objects only): a second pass with `tex2` and the object's change colour */
	int16_t  tex2;              /* that pass's texture (base map with the mask as alpha), -1 = none */
} mat_binding_t;

/* A texture or lightmap page ready for upload. `pixels` and `clut` must be flushed from the
 * D-cache and 16-byte aligned. psm: GS_PSMT8 (T4 is not drawn yet). */
typedef struct {
	const void *pixels, *clut;
	uint32_t    pixel_bytes, clut_bytes;
	uint16_t    w, h;
	uint8_t     psm;
} tex_image_t;

typedef struct {
	/* shader tag id -> binding; returns 0 if known, non-zero = untextured */
	int (*binding)(void *user, uint32_t shader_id, mat_binding_t *out);
	int (*texture)(void *user, int tex, tex_image_t *out);          /* 0 on success */
	int (*lightmap)(void *user, unsigned page, tex_image_t *out);   /* 0 on success */
	void *user;
	unsigned texture_count;
} tex_source_t;

struct render_ctx;
typedef void (*render_pass_fn)(void *user, struct render_ctx *rc);

typedef struct {
	u32 obj_visible, obj_batches, obj_triangles, obj_culled;
	u32 batches, triangles, clusters_visible, clusters_total, materials_drawn;
	u32 portals_tested, portals_passed, camera_cluster_valid;
	u32 uploads, upload_bytes, tex_hits, tex_misses, tex_deferred, tex_failed, pages_free;
	u32 chain_qw, chain_overflow, untextured_batches;
	u32 clip_batches, clip_tris, clip_polys, clip_dropped, clip_skipped_batches, clip_culled_batches, clip_culled_back;     /* triangles clipped on the EE, polygons drawn, dropped for lack of room */
	float vis_ms, build_ms, clip_ms, emit_clip_ms, clip_scan_ms, clip_gather_ms;
} render_stats_t;

typedef struct {
	int   textures, lightmaps, fog;
	int   glow;                  /* draw the self illumination maps (screens, lamps, windows) as an additive pass after the lightmap: cfg glow, default 1 (ADR-079) */
	int   only_batch, only_batch_hi;     /* debug: draw only batches lo..hi (only_batch -1 = all) */
	int   unused_only;                    /* debug: draw only this batch index (-1 = all) */
	int   cull_sign;                    /* back face culling: 0 off, +1 / -1 = which orientation is the back */
	int   clip_debug;                    /* debug: log full-screen clipped polygons */
	float clip_budget_ms;                /* EE time allowed for clipping near triangles per frame */
	int   debug;                         /* bit 0: skip pass-1 batches, 1: skip pass 2, 2: skip overlay */
	float fog_near, fog_far;
	u8    fog_rgb[3];
	u8    sky_rgb[3];                    /* sky colour at the top of the screen */
	u8    horizon_rgb[3];                /* sky colour at the horizon (and below it) */
} render_options_t;

/* Dynamic point lights (the red alert lamps, the command display): `n` rows of x y z r g b reach, r g b 0..1. The vertex colours of the batches they reach are rewritten
 * in place (the packets are static, so a patched batch stays patched until the light set changes) to blend toward the light's colour with a (1 - d / reach)^2 falloff:
 * walls are lit by baked lightmaps, this is their only dynamic term. n = 0 restores every batch. */
void bsp_render_set_dynamic_lights(const float (*rows)[7], unsigned n);
void bsp_render_set_dl_gain(float g);   /* default 0.33 */

/* One-time: VU1 microprogram, double buffer, chain memory (the single boot-time malloc). */
int  bsp_render_init(const video_t *v);
/* Per BSP (level arena): visibility buffers, material bindings, texture cache. */
int  bsp_render_set_world(arena_t *a, const bsp_section *s, const tex_source_t *src);
void bsp_render_options(render_options_t *o);                    /* defaults */
void bsp_render_set_options(const render_options_t *o);
/* While set, a camera that lies in no BSP cluster draws no world instead of every cluster in view (the player fell out of the map: ADR-062). */
void bsp_render_set_void_view(int on);
/* Builds frame `frame` into the idle chain. `overlay` may be NULL. */
void bsp_render_build(u32 frame, const camera_t *cam, const text_batch_t *overlay);
/* Sends env + clear + uploads + the geometry chain; returns immediately after starting VIF1. */
void bsp_render_kick(video_t *v);
const render_stats_t *bsp_render_stats(void);
/* Poll point for the profiler: records when the VIF1 chain and GS finish were first seen. */
void bsp_render_poll(void);
/* ms from kick to VIF1 chain done / GS FINISH as last observed by bsp_render_poll. */
void bsp_render_timing(float *vif_ms, float *gs_ms);
/* Pages in the GS texture window available to the texture cache (after CLUT pool and font). */
u32 bsp_render_cache_pages(void);
/* Spin until the VIF1 chain and the GS FINISH of the last kicked frame have been observed (bounded), so
 * bsp_render_timing reports the true GS time; the flip would wait for it anyway. */
void bsp_render_wait_gpu(void);

/* Object pass (render/obj_render.c): called inside bsp_render_build after the BSP passes, before the
 * overlay, with the frame's chain; `obj_src` supplies the object textures (OTEX), may be NULL. */
void bsp_render_set_object_pass(render_pass_fn fn, void *user, const tex_source_t *obj_src);
/* Sky pass: called right after the frame constants, before the BSP passes (depth writes are the pass's business). */
void bsp_render_set_sky_pass(render_pass_fn fn, void *user);
/* Overlay pass: after the object pass, before the profiler text (HUD, particles, decals). */
void bsp_render_set_overlay_pass(render_pass_fn fn, void *user);
/* Reserves `count` texture residency keys for another texture set (HUD, effects); returns the key base to pass to rb_extra_tex0. Call before the first frame. */
unsigned bsp_render_reserve_keys(unsigned count);
/* Streamed high resolution variants of the BSP's textures (world/hires.h, docs/37). `key_base` is a bsp_render_reserve_keys range of the base texture count. A surface
 * asks for the variant of its texture when one screen pixel covers at least `want_ratio` of a texel of the resident copy (0.7: the resident texture starts to blur), and
 * starts the read from half of that ratio on; NULL switches the feature off. Needs the materials' uv_world (BSP section, tools/halo2ps2/bspconv.py). */
void bsp_render_set_hires(hires_t *h, unsigned key_base, float want_ratio);
