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
	float               halo;                 /* big additive sprites get a wider blue copy of themselves at this strength (the Xbox bloom pass; cfg fxhalo, cinematics only) */
	float               add_gain, add_size;   /* additive particles: colour and size multipliers (cfg fxadd, fxaddsize; 1.0) */
} fx_render_t;

void fx_render_pass(void *user, struct render_ctx *rc);

#endif
