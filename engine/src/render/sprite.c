#include "render/sprite.h"
#include "render/gs.h"
#include "render/render_internal.h"
#include <string.h>

void spr_begin(spr_t *s, chain_t *c, u32 zbuf_page, int screen_w, int screen_h)
{
	memset(s, 0, sizeof *s);
	s->chain = c; s->zpage = zbuf_page; s->screen_w = screen_w; s->screen_h = screen_h;
}

void spr_flush(spr_t *s)
{
	u32 n = s->n, i, payload, *w;
	u64 *q;
	if (n == 0) return;
	payload = 1 + 6 + 1 + n * 6;                       /* state tag + 6 A+D, sprite tag, 2 vertices x 3 qw per quad */
	w = chain_cnt(s->chain, 1 + payload, rb_take_pending());
	if (!w) { s->n = 0; return; }
	w[0] = VIF_CODE(VIF_NOP, 0, 0); w[1] = VIF_CODE(VIF_NOP, 0, 0);
	w[2] = VIF_CODE(VIF_FLUSHA, 0, 0); w[3] = VIF_CODE(VIF_DIRECT, 0, payload);
	q = (u64 *)(w + 4);
	q[0] = GIF_TAG(6, 0, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
	q[2] = s->tex0; q[3] = GS_A_TEX0_1;
	q[4] = GS_TEX1(1, 0, 1, 1, 0, 0, 0);               q[5] = GS_A_TEX1_1;
	q[6] = GS_CLAMP(1, 1, 0, 0, 0, 0);                 q[7] = GS_A_CLAMP_1;
	if (s->blend == SPR_BLEND_ADD) q[8] = GS_ALPHA(0, 2, 0, 1, 0);          /* Cs * As + Cd */
	else if (s->blend == SPR_BLEND_MULTIPLY) q[8] = GS_ALPHA(1, 2, 0, 0, 0);   /* Cd * As: darken */
	else q[8] = GS_ALPHA(0, 1, 0, 1, 0);                                    /* (Cs - Cd) * As + Cd */
	q[9] = GS_A_ALPHA_1;
	q[10] = GS_TEST(0, 1, 0, 0, 0, 0, 1, s->ztest ? 2 : 1); q[11] = GS_A_TEST_1;
	q[12] = GS_ZBUF(s->zpage, GS_PSMZ24, 1);           q[13] = GS_A_ZBUF_1;                  /* Z read-only */
	q[14] = GIF_TAG(n * 2, 1, 1, GS_PRIM(6, 0, s->tex0 ? 1 : 0, 0, 1, 0, 1, 0, 0), GIFX_FLG_PACKED, 3);
	q[15] = 0x531;                                     /* RGBAQ, UV, XYZ2 */
	q += 16;
	for (i = 0; i < n; i++) {
		const spr_quad_t *p = &s->q[i];
		int k;
		for (k = 0; k < 2; k++) {
			float u = k ? p->u1 : p->u0, v = k ? p->v1 : p->v0, x = k ? p->x1 : p->x0, y = k ? p->y1 : p->y0;
			q[0] = (u64)p->r | (u64)p->g << 32;
			q[1] = (u64)p->b | (u64)p->a << 32;
			q[2] = (u64)(u32)(int)(u * 16.0f) | (u64)(u32)(int)(v * 16.0f) << 32;
			q[3] = 0;
			q[4] = (u64)(u32)(int)((2048.0f - (float)s->screen_w * 0.5f + x) * 16.0f) | (u64)(u32)(int)((2048.0f - (float)s->screen_h * 0.5f + y) * 16.0f) << 32;
			q[5] = (u64)p->z;
			q += 6;
		}
	}
	s->flushes++;
	s->n = 0;
}

void spr_state(spr_t *s, u64 tex0, int blend, int ztest)
{
	if (s->n && (s->tex0 != tex0 || s->blend != blend || s->ztest != ztest)) spr_flush(s);
	s->tex0 = tex0; s->blend = blend; s->ztest = ztest;
}

void spr_quad(spr_t *s, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 z, u32 rgba)
{
	spr_quad_t *p;
	if (s->n >= SPR_MAX) spr_flush(s);
	p = &s->q[s->n++];
	p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1; p->u0 = u0; p->v0 = v0; p->u1 = u1; p->v1 = v1; p->z = z;
	p->r = (u8)rgba; p->g = (u8)(rgba >> 8); p->b = (u8)(rgba >> 16); p->a = (u8)(rgba >> 24);
	s->quads++;
}
