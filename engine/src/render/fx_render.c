#include "render/fx_render.h"
#include "platform/timer.h"
#include "platform/video.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { float depth, cx, cy, half; unsigned sprite_tex; float u0, v0, u1, v1; uint32_t rgba; uint8_t blend; uint8_t decal; } item_t;

static item_t s_items[FX_MAX_PARTICLES + FX_MAX_DECALS];
static uint16_t s_order[FX_MAX_PARTICLES + FX_MAX_DECALS];

static int blend_of(unsigned fxblend)
{
	if (fxblend == FXD_BLEND_ADD) return SPR_BLEND_ADD;
	if (fxblend == FXD_BLEND_MULTIPLY || fxblend == FXD_BLEND_DOUBLE_MULTIPLY) return SPR_BLEND_MULTIPLY;
	return SPR_BLEND_ALPHA;
}

static int project(render_ctx_t *rc, const float pos[3], float half_world, float *cx, float *cy, float *half, float *depth)
{
	cvert v;
	float kx;
	clip_project(rc->clip, pos, &v);
	if (v.c[3] < rc->cam->near_z * 1.5f) return 0;
	kx = 1.0f / tanf(rc->cam->hfov * 0.5f);
	*cx = (v.c[0] / v.c[3] * 0.5f + 0.5f) * (float)VIDEO_WIDTH;
	*cy = (v.c[1] / v.c[3] * 0.5f + 0.5f) * (float)VIDEO_HEIGHT;
	*half = half_world * kx / v.c[3] * (float)VIDEO_WIDTH * 0.5f;
	*depth = v.c[3];
	return 1;
}

void fx_render_pass(void *user, render_ctx_t *rc)
{
	fx_render_t *r = user;
	const fx_t *fx = r->fx;
	spr_t spr;
	unsigned i, n = 0, j;
	u32 t0 = timer_cycles();
	r->st_particles = r->st_decals = r->st_skipped = 0;
	if (!r->enabled || !fx || !fx->def) return;
	for (i = 0; i < FX_MAX_PARTICLES; i++) {
		const fx_particle *p = &fx->p[i];
		item_t *it = &s_items[n];
		float life_t, size;
		fxd_sprite sp;
		unsigned a;
		if (!p->active || p->age < 0 || !r->particles) continue;
		a = fx_particle_alpha(p);
		if (a < 2) continue;
		life_t = p->life ? (float)p->age / (float)p->life : 0.0f;
		size = p->size0 + (p->size1 - p->size0) * life_t;
		if (p->blend == FXD_BLEND_ADD) size *= r->add_size;
		if (!project(rc, p->pos, size, &it->cx, &it->cy, &it->half, &it->depth) || it->depth < 0.35f) continue;
		if (it->half < 0.75f) it->half = 0.75f;
		if (it->half > 40.0f) it->half = 40.0f;             /* effects are tuned for a third-person-ish camera: cap the near-camera blow-up */
		if (it->cx + it->half < 0.0f || it->cx - it->half > (float)VIDEO_WIDTH || it->cy + it->half < 0.0f || it->cy - it->half > (float)VIDEO_HEIGHT) continue;   /* off screen: far coordinates wrap in the GS */
		if (!fxdef_sprite(fx->def, p->bitmap, p->seq, (uint32_t)((float)p->age * p->anim_rate), &sp) || sp.texture == 0xFFFFu) continue;
		it->sprite_tex = sp.texture; it->u0 = sp.u0; it->v0 = sp.v0; it->u1 = sp.u1; it->v1 = sp.v1;
		it->rgba = (uint32_t)p->rgba[0] | (uint32_t)p->rgba[1] << 8 | (uint32_t)p->rgba[2] << 16 | (uint32_t)a << 24;
		it->blend = (uint8_t)blend_of(p->blend); it->decal = 0;
		if (it->blend == SPR_BLEND_ADD && r->add_gain != 1.0f) {
			unsigned c;
			for (c = 0; c < 3; c++) { unsigned v = (unsigned)((float)((it->rgba >> (8 * c)) & 0xFFu) * r->add_gain); it->rgba = (it->rgba & ~(0xFFu << (8 * c))) | ((v > 255u ? 255u : v) << (8 * c)); }
		}
		if (it->blend == SPR_BLEND_ALPHA && it->half > 12.0f) it->rgba = (it->rgba & 0x00FFFFFFu) | (((it->rgba >> 24) * 2u / 5u) << 24);
		if (it->blend == SPR_BLEND_MULTIPLY) { it->blend = SPR_BLEND_ALPHA; it->rgba = 0x10u | 0x10u << 8 | 0x10u << 16 | (((it->rgba >> 24) * 3u / 5u) << 24); }
		n++;
	}
	for (i = 0; i < FX_MAX_DECALS; i++) {
		const fx_decal *d = &fx->d[i];
		item_t *it = &s_items[n];
		fxd_sprite sp;
		unsigned a;
		if (!d->active || !r->decals) continue;
		a = d->rgba[3];
		if (d->age > d->life) a = d->fade ? a * (unsigned)(d->life + d->fade - d->age) / d->fade : 0;
		if (a < 2) continue;
		if (!project(rc, d->pos, d->radius, &it->cx, &it->cy, &it->half, &it->depth)) continue;
		if (it->depth > 40.0f) continue;
		if (it->half < 0.75f) it->half = 0.75f;
		if (it->half > 80.0f) it->half = 80.0f;
		if (!fxdef_sprite(fx->def, d->bitmap, 0, 0, &sp) || sp.texture == 0xFFFFu) continue;
		it->sprite_tex = sp.texture; it->u0 = sp.u0; it->v0 = sp.v0; it->u1 = sp.u1; it->v1 = sp.v1;
		it->rgba = (uint32_t)d->rgba[0] | (uint32_t)d->rgba[1] << 8 | (uint32_t)d->rgba[2] << 16 | (uint32_t)a << 24;
		it->blend = (uint8_t)blend_of(d->blend); it->decal = 1;
		if (it->blend == SPR_BLEND_MULTIPLY) { it->blend = SPR_BLEND_ALPHA; it->rgba = 0x10u | 0x10u << 8 | 0x10u << 16 | (((it->rgba >> 24) * 3u / 5u) << 24); }   /* no texture-dependent multiply on the GS: soot */
		n++;
	}
	/* order: decals first, then by blend group, far to near inside a group (insertion sort, n <= 368) */
	for (i = 0; i < n; i++) s_order[i] = (uint16_t)i;
	for (i = 1; i < n; i++) {
		uint16_t k = s_order[i];
		const item_t *a = &s_items[k];
		for (j = i; j > 0; j--) {
			const item_t *b = &s_items[s_order[j - 1]];
			int before = a->decal != b->decal ? a->decal > b->decal : (a->blend != b->blend ? a->blend < b->blend : a->depth > b->depth);
			if (!before) break;
			s_order[j] = s_order[j - 1];
		}
		s_order[j] = k;
	}
	spr_begin(&spr, rc->chain, *rb_zbuf_page_ptr(), VIDEO_WIDTH, VIDEO_HEIGHT);
	for (i = 0; i < n; i++) {
		const item_t *it = &s_items[s_order[i]];
		tex_image_t img;
		u64 tex0 = 0;
		u32 z = (u32)(rc->cam->near_z / it->depth * 16777215.0f);
		if (r->tex && r->tex->texture && r->tex->texture(r->tex->user, (int)it->sprite_tex, &img) == 0) tex0 = rb_extra_tex0(r->key_base, it->sprite_tex, &img, 0);
		else img.w = img.h = 0;
		if (!tex0) { r->st_skipped++; continue; }
		if (z > 0xFFFFFFu) z = 0xFFFFFFu;
		spr_state(&spr, tex0, it->blend, 1);
		if (r->halo > 0.0f && it->blend == SPR_BLEND_ADD && it->half >= 16.0f && !it->decal) {      /* the bloom: a wider, bluer, fainter copy under the sprite */
			float h = it->half * 2.2f;
			unsigned c0 = (unsigned)((float)(it->rgba & 0xFFu) * 0.35f * r->halo), c1 = (unsigned)((float)((it->rgba >> 8) & 0xFFu) * 0.65f * r->halo), c2 = (unsigned)((float)((it->rgba >> 16) & 0xFFu) * r->halo);
			uint32_t hc = (c0 > 255u ? 255u : c0) | (c1 > 255u ? 255u : c1) << 8 | (c2 > 255u ? 255u : c2) << 16 | (it->rgba & 0xFF000000u);
			spr_quad(&spr, it->cx - h, it->cy - h, it->cx + h, it->cy + h, it->u0 * (float)img.w, it->v0 * (float)img.h, it->u1 * (float)img.w, it->v1 * (float)img.h, z, hc);
		}
		spr_quad(&spr, it->cx - it->half, it->cy - it->half, it->cx + it->half, it->cy + it->half,
		         it->u0 * (float)img.w, it->v0 * (float)img.h, it->u1 * (float)img.w, it->v1 * (float)img.h, z, it->rgba);
		if (it->decal) r->st_decals++; else r->st_particles++;
	}
	spr_flush(&spr);
	r->st_ms = timer_ms(t0, timer_cycles());
}
