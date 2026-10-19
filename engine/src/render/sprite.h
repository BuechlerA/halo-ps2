/* Textured GS sprites drawn through the VIF1 chain (PATH2 DIRECT), for the HUD and for particles/decals. Coordinates are framebuffer
 * pixels (origin top left); depth is a 24-bit Z for particles (HUD uses z = 0xFFFFFF with the Z test off). Quads are collected per
 * texture state and emitted when the state changes or on spr_flush. */
#ifndef RENDER_SPRITE_H
#define RENDER_SPRITE_H
#include <tamtypes.h>
#include "render/chain.h"

#define SPR_MAX 192
enum { SPR_BLEND_ALPHA = 0, SPR_BLEND_ADD = 1, SPR_BLEND_MULTIPLY = 2 };

typedef struct {
	float x0, y0, x1, y1, u0, v0, u1, v1;
	u32   z;
	u8    r, g, b, a;               /* GS scale: 0x80 = 1.0 */
} spr_quad_t;

typedef struct {
	chain_t   *chain;
	u64        tex0;                /* 0 = untextured (flat colour) */
	int        blend, ztest;        /* ztest: 0 off (always), 1 GEQUAL read-only */
	u32        zpage;
	int        screen_w, screen_h;
	unsigned   n, flushes, quads;
	spr_quad_t q[SPR_MAX];
} spr_t;

void spr_begin(spr_t *s, chain_t *c, u32 zbuf_page, int screen_w, int screen_h);
/* Selects the texture state; flushes the pending quads if it differs. */
void spr_state(spr_t *s, u64 tex0, int blend, int ztest);
void spr_quad(spr_t *s, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 z, u32 rgba);
void spr_flush(spr_t *s);

#endif
