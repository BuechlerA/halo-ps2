/* Overlay pass for effects: particles and decals of sim/fx.c as depth-tested GS sprites (billboards). */
#ifndef RENDER_FX_RENDER_H
#define RENDER_FX_RENDER_H
#include "render/render_internal.h"
#include "render/sprite.h"
#include "sim/fx.h"
#include "world/tex.h"

typedef struct {
	const fx_t         *fx;
	const tex_source_t *tex;          /* FXTX */
	unsigned            key_base;
	int                 enabled, decals, particles;
	unsigned            st_particles, st_decals, st_skipped;
	float               st_ms;
} fx_render_t;

void fx_render_pass(void *user, struct render_ctx *rc);

#endif
