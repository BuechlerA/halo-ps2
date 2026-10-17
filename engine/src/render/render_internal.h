/* Shared between bsp_render.c and the object pass (obj_render.c): the frame's chain and the helpers that
 * touch bsp_render.c's file-local state. Not part of any other module's interface. */
#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H
#include <tamtypes.h>
#include "render/bsp_render.h"
#include "render/camera.h"
#include "render/chain.h"
#include "render/clip.h"

typedef struct render_ctx {
	chain_t        *chain;
	const camera_t *cam;
	const float   (*clip)[4];
	const frustum_t *frustum;
	u32             frame;
	const render_options_t *opt;
} render_ctx_t;

/* MSCAL (or other VIF pair) that must ride on the next tag; the object pass takes it with its first chunk. */
u64  rb_take_pending(void);
void rb_set_pending(u64 pair);
/* GS TEX0 value for object texture `index` of the object tex source (uploading it if needed), 0 if it cannot
 * be drawn this frame. `tfx` 0 = modulate. */
u64  rb_object_tex0(unsigned index, const tex_image_t *img, u64 tfx);
/* GS TEX0 of texture `index` of a reserved key range (bsp_render_reserve_keys), 0 if it cannot be drawn this frame. */
u64  rb_extra_tex0(unsigned key_base, unsigned index, const tex_image_t *img, u64 tfx);
/* Fog constants in the F16 form of bsp.vsm (abs 9 / model abs 4): k, b. */
void rb_fog_params(float *k, float *b);
/* VU1 entry address (MSCAL operand) of the model program. */
u32  rb_model_entry(void);
const u32 *rb_zbuf_page_ptr(void);

#endif
