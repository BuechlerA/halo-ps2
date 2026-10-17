#include "render/hud_render.h"
#include "platform/timer.h"
#include "platform/video.h"

void hud_render_pass(void *user, render_ctx_t *rc)
{
	hud_render_t *h = user;
	spr_t spr;
	unsigned i;
	u32 t0 = timer_cycles();
	const float sx = (float)VIDEO_WIDTH / (float)HUDDEF_VIRT_W, sy = (float)VIDEO_HEIGHT / (float)HUDDEF_VIRT_H;
	h->st_quads = h->st_skipped = 0;
	if (!h->enabled || !h->game) return;
	spr_begin(&spr, rc->chain, *rb_zbuf_page_ptr(), VIDEO_WIDTH, VIDEO_HEIGHT);
	/* damage wash */
	if (h->game->hurt_flash > 0.0f) {
		unsigned a = (unsigned)(h->game->hurt_flash * 40.0f);
		spr_state(&spr, 0, SPR_BLEND_ALPHA, 0);
		spr_quad(&spr, 0.0f, 0.0f, (float)VIDEO_WIDTH, (float)VIDEO_HEIGHT, 0, 0, 0, 0, 0xFFFFFFu, 0x80u | 0x10u << 8 | 0x10u << 16 | (a > 0x60 ? 0x60u : a) << 24);
	}
	if (h->hud) {
		hud_build(&h->frame, h->hud, h->fonts, h->game, h->tick, h->state);
		for (i = 0; i < h->frame.n; i++) {
			const hud_quad_t *q = &h->frame.q[i];
			tex_image_t img;
			u64 tex0 = 0;
			if (h->tex && h->tex->texture && h->tex->texture(h->tex->user, q->tex, &img) == 0) tex0 = rb_extra_tex0(h->key_base, q->tex, &img, 0);
			if (!tex0) { h->st_skipped++; continue; }
			spr_state(&spr, tex0, q->blend == HUDDEF_BLEND_ADD ? SPR_BLEND_ADD : SPR_BLEND_ALPHA, 0);
			spr_quad(&spr, q->x0 * sx, q->y0 * sy, q->x1 * sx, q->y1 * sy, (float)q->u0, (float)q->v0, (float)q->u1, (float)q->v1, 0xFFFFFFu, q->rgba);
			h->st_quads++;
		}
	}
	spr_flush(&spr);
	h->st_ms = timer_ms(t0, timer_cycles());
}
