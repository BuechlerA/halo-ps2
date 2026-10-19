#include "render/hud_render.h"
#include "platform/timer.h"
#include "platform/video.h"
#include <string.h>

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
	if (h->game->hurt_flash > 0.0f && !(h->cine && h->cine->screen.cinematic)) {
		unsigned a = (unsigned)(h->game->hurt_flash * 40.0f);
		spr_state(&spr, 0, SPR_BLEND_ALPHA, 0);
		spr_quad(&spr, 0.0f, 0.0f, (float)VIDEO_WIDTH, (float)VIDEO_HEIGHT, 0, 0, 0, 0, 0xFFFFFFu, 0x80u | 0x10u << 8 | 0x10u << 16 | (a > 0x60 ? 0x60u : a) << 24);
	}
	if (h->hud && !(h->cine && h->cine->screen.cinematic)) {
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
	if (h->cine) {
		const cine_state *cs = h->cine;
		static hud_frame_t tf;
		unsigned i;
		/* letterbox bars and the fade wash (flat quads) */
		if (cs->screen.letterbox > 0.0f) {
			float bar = (float)cine_letterbox_bar(cs, VIDEO_HEIGHT);
			spr_state(&spr, 0, SPR_BLEND_ALPHA, 0);
			spr_quad(&spr, 0.0f, 0.0f, (float)VIDEO_WIDTH, bar, 0, 0, 0, 0, 0xFFFFFFu, 0x80000000u | 0u);
			spr_quad(&spr, 0.0f, (float)VIDEO_HEIGHT - bar, (float)VIDEO_WIDTH, (float)VIDEO_HEIGHT, 0, 0, 0, 0, 0xFFFFFFu, 0x80000000u);
		}
		/* text: messages and titles through the HUD font */
		memset(&tf, 0, sizeof tf);
		if (h->hud && h->fonts) {
			uint32_t font = huddef_get_globals(h->hud).font_sp;
			for (i = 0; i < CINE_MSG_SLOTS; i++) {
				const cine_msg *mg = cine_msg_slot(cs, i);
				char line[CINE_TEXT_UNITS + 1];
				unsigned k;
				float y;
				if (!mg || !mg->units) continue;
				for (k = 0; k < mg->units && k < CINE_TEXT_UNITS; k++) line[k] = mg->text[k] < 128 ? (char)mg->text[k] : '?';
				line[k] = 0;
				y = mg->kind == CINE_MSG_SUBTITLE ? 392.0f : mg->kind == CINE_MSG_OBJECTIVE ? 90.0f : 60.0f;
				if (cs->screen.cinematic && mg->kind != CINE_MSG_SUBTITLE) continue;
				hud_text(&tf, h->fonts, font, 60.0f, y, line, 0x80808080u);
			}
			for (i = 0; i < CINE_TITLE_SLOTS; i++) {
				const cine_title *tt = &cs->titles[i];
				float a;
				char line[CINE_TEXT_UNITS + 1];
				unsigned k;
				if (!tt->active) continue;
				a = cine_title_alpha(tt);
				if (a <= 0.01f) continue;
				for (k = 0; k < tt->units && k < CINE_TEXT_UNITS; k++) line[k] = tt->text[k] < 128 ? (char)tt->text[k] : '?';
				line[k] = 0;
				hud_text(&tf, h->fonts, font, 120.0f, 330.0f + 24.0f * (float)i, line, 0x00808080u | ((uint32_t)(a * 128.0f) << 24));
			}
		}
		for (i = 0; i < tf.n; i++) {
			const hud_quad_t *q = &tf.q[i];
			tex_image_t img;
			u64 tex0 = 0;
			if (h->tex && h->tex->texture && h->tex->texture(h->tex->user, q->tex, &img) == 0) tex0 = rb_extra_tex0(h->key_base, q->tex, &img, 0);
			if (!tex0) continue;
			spr_state(&spr, tex0, SPR_BLEND_ALPHA, 0);
			spr_quad(&spr, q->x0 * sx, q->y0 * sy, q->x1 * sx, q->y1 * sy, (float)q->u0, (float)q->v0, (float)q->u1, (float)q->v1, 0xFFFFFFu, q->rgba);
		}
		if (cs->screen.fade_alpha > 0.0f) {
			unsigned a = (unsigned)(cs->screen.fade_alpha * 128.0f);
			spr_state(&spr, 0, SPR_BLEND_ALPHA, 0);
			spr_quad(&spr, 0.0f, 0.0f, (float)VIDEO_WIDTH, (float)VIDEO_HEIGHT, 0, 0, 0, 0, 0xFFFFFFu,
			         (unsigned)(cs->screen.fade_rgb[0] * 128.0f) | (unsigned)(cs->screen.fade_rgb[1] * 128.0f) << 8 | (unsigned)(cs->screen.fade_rgb[2] * 128.0f) << 16 | (a > 128u ? 128u : a) << 24);
		}
	}
	spr_flush(&spr);
	h->st_ms = timer_ms(t0, timer_cycles());
}
