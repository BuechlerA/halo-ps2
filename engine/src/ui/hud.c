#include "ui/hud.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t scale_colour(uint32_t c, float k)
{
	unsigned r = (unsigned)((float)(c & 255u) * k), gg = (unsigned)((float)((c >> 8) & 255u) * k), b = (unsigned)((float)((c >> 16) & 255u) * k);
	if (r > 255) r = 255;
	if (gg > 255) gg = 255;
	if (b > 255) b = 255;
	return r | gg << 8 | b << 16 | (c & 0xFF000000u);
}

static uint32_t lerp_colour(uint32_t a, uint32_t b, float t)
{
	unsigned i, out = 0;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	for (i = 0; i < 4; i++) {
		float x = (float)((a >> (8 * i)) & 255u), y = (float)((b >> (8 * i)) & 255u);
		out |= (unsigned)(x + (y - x) * t) << (8 * i);
	}
	return out;
}

static void add_quad(hud_frame_t *f, const huddef_sprite *sp, float x, float y, float w, float h, float frac_x, uint32_t rgba, int blend)
{
	hud_quad_t *q;
	float u1 = (float)sp->u1;
	if (f->n >= HUD_MAX_QUADS || sp->tex == HUDDEF_NONE || w <= 0.0f || h <= 0.0f) return;
	q = &f->q[f->n++];
	q->x0 = x; q->y0 = y; q->x1 = x + w * frac_x; q->y1 = y + h;
	q->tex = sp->tex; q->u0 = sp->u0; q->v0 = sp->v0; q->v1 = sp->v1;
	q->u1 = (uint16_t)((float)sp->u0 + ((float)sp->u1 - (float)sp->u0) * frac_x + 0.5f);
	(void)u1;
	q->rgba = rgba ? rgba : 0x80808080u;
	q->blend = (uint8_t)blend;
}

int hud_team_hostile(unsigned team) { return team == 3 || team == 4 || team == 5; }     /* covenant, flood, sentinel (ai/units.c teams) */
int hud_team_friendly(unsigned team) { return team == 1 || team == 2; }                  /* player, human */

/* motion tracker blips: every living unit within the tracker range, around the radar centre (cx, cy) of radius `radius`
 * pixels, the player's facing up. The sprite is the retail blip bitmap scaled to HUD_BLIP_SIZE. */
static void draw_blips(hud_frame_t *f, const huddef_section *s, const huddef_globals *gl, const game_t *g, float cx, float cy, float radius)
{
	huddef_sprite sp;
	float range = gl->motion_range > 0.0f ? gl->motion_range : 20.0f;
	float c = cosf(g->player.yaw), sn = sinf(g->player.yaw);
	unsigned i, n;
	if (gl->blip_sprite == HUDDEF_NONE || !g->objects || radius <= 0.0f) return;
	sp = huddef_get_sprite(s, gl->blip_sprite);
	n = g->objects->count < OBJ_MAX ? g->objects->count : OBJ_MAX;
	for (i = 0; i < n; i++) {
		const object_t *o = &g->objects->obj[i];
		const game_unit_t *u = &g->unit_state[i];
		float dx, dy, fwd, right, d, size;
		uint32_t col;
		if ((o->type != SCEN_BIPED && o->type != SCEN_VEHICLE) || (o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE | OBJ_F_HIDDEN))) continue;
		if ((int)i == g->player_obj || u->dead || (u->has_vitals && vitals_dead(&u->vitals))) continue;
		dx = o->pos[0] - g->player.pos[0]; dy = o->pos[1] - g->player.pos[1];
		d = sqrtf(dx * dx + dy * dy);
		if (d > range) continue;
		fwd = dx * c + dy * sn;                        /* along the facing (x forward at yaw 0, z up) */
		right = dx * sn - dy * c;                      /* to the right of the facing */
		size = o->type == SCEN_VEHICLE ? HUD_BLIP_VEHICLE_SIZE : HUD_BLIP_SIZE;
		col = hud_team_hostile(u->team) ? HUD_BLIP_HOSTILE : hud_team_friendly(u->team) ? HUD_BLIP_FRIENDLY : HUD_BLIP_NEUTRAL;
		add_quad(f, &sp, cx + right / range * radius - size * 0.5f, cy - fwd / range * radius - size * 0.5f, size, size, 1.0f, col, HUDDEF_BLEND_ALPHA);
		f->blips++;
	}
}

/* flashing: on for `length` ticks every `period`, `count` times after `delay` (period 0 = never) */
static int flash_on(uint32_t tick, uint16_t period, uint16_t length)
{
	if (!period) return 1;
	return (tick % period) < (length ? length : period / 2u);
}

static void draw_elem(hud_frame_t *f, const huddef_section *s, const huddef_elem *e, uint32_t colour, uint32_t tick)
{
	huddef_sprite sp;
	unsigned frame = 0;
	if (e->sprite == HUDDEF_NONE || !e->frames) return;
	if (e->frame_rate && e->frames > 1) frame = (tick / (unsigned)(e->frame_rate > 0 ? 30u / (e->frame_rate > 30 ? 30 : e->frame_rate) : 1u)) % e->frames;
	sp = huddef_get_sprite(s, (uint32_t)e->sprite + frame);
	add_quad(f, &sp, (float)e->x, (float)e->y, (float)e->w, (float)e->h, 1.0f, colour, e->blend ? e->blend : HUDDEF_BLEND_ALPHA);
	f->elements++;
}

static void draw_meter(hud_frame_t *f, const huddef_section *s, const huddef_meter *m, float frac, uint32_t tick, int low)
{
	huddef_sprite sp;
	uint32_t col;
	if (m->sprite == HUDDEF_NONE) return;
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;
	sp = huddef_get_sprite(s, m->sprite);
	if (frac <= 0.0f && m->colour_empty) { add_quad(f, &sp, (float)m->x, (float)m->y, (float)m->w, (float)m->h, 1.0f, m->colour_empty, HUDDEF_BLEND_ALPHA); f->meters++; return; }
	col = lerp_colour(m->colour_min, m->colour_max, frac);
	if (low && ((tick / 4u) & 1u)) col = m->colour_flash ? m->colour_flash : scale_colour(col, 0.5f);
	add_quad(f, &sp, (float)m->x, (float)m->y, (float)m->w, (float)m->h, frac, col, HUDDEF_BLEND_ALPHA);
	f->meters++;
}

static void draw_number(hud_frame_t *f, const huddef_section *s, const huddef_globals *gl, const huddef_number *n, unsigned value, uint32_t colour)
{
	char buf[12];
	unsigned len = 0, i, digits = n->max_digits ? n->max_digits : 3;
	float dw, dh, x;
	huddef_sprite sp;
	if (gl->digit_count < 10) return;
	{ unsigned v = value, k; unsigned cap = digits < 10 ? digits : 10; for (k = 0; k < cap; k++) { buf[cap - 1 - k] = (char)('0' + v % 10); v /= 10; } len = cap; }
	if (!(n->flags & HUDDEF_NUM_LEADING_ZEROS)) {
		unsigned lead = 0;
		while (lead + 1 < len && buf[lead] == '0') lead++;
		memmove(buf, buf + lead, len - lead); len -= lead;
	}
	sp = huddef_get_sprite(s, gl->digit_first);
	dw = (float)(gl->screen_digit_width > 0 ? gl->screen_digit_width : sp.w);
	dh = (float)n->h;
	if (dh <= 0.0f) dh = (float)sp.h * (n->scale_y > 0.0f ? n->scale_y : 1.0f);
	x = (float)n->x + (float)n->w - dw * (float)len;       /* right aligned inside the number's box */
	for (i = 0; i < len; i++) {
		huddef_sprite d = huddef_get_sprite(s, gl->digit_first + (uint32_t)(buf[i] - '0'));
		/* a digit sprite is a cell about twice the glyph's width with the glyph in its middle (the sheet's pitch, not the
		 * on-screen advance): draw the cell at its own size, centred on the advance slot, never squeezed into it */
		float cw = (float)d.w * (n->scale_x > 0.0f ? n->scale_x : 1.0f);
		add_quad(f, &d, x + dw * (float)i + (dw - cw) * 0.5f, (float)n->y, cw, dh, 1.0f, colour, HUDDEF_BLEND_ALPHA);
		f->digits++;
	}
}

static void draw_weapon_hud(hud_frame_t *f, const huddef_section *s, uint32_t hud_tag, const game_t *g, uint32_t tick, int depth)
{
	int hi = huddef_find_hud(s, hud_tag);
	huddef_hud h;
	const game_slot_t *sl = game_cur_slot(g);
	huddef_globals gl = huddef_get_globals(s);
	unsigned i, loaded = 0, reserve = 0, mag = 0;
	float heat = 0.0f;
	int low_ammo;
	if (hi < 0 || depth > HUDDEF_MAX_CHILD_DEPTH) return;
	h = huddef_get_hud(s, (uint32_t)hi);
	if (sl && sl->weapon >= 0) {
		mag = sl->st.reload_mag < sl->def.magazine_count ? sl->st.reload_mag : 0;
		if (sl->def.trigger_count && sl->def.trigger[0].magazine < sl->def.magazine_count) mag = sl->def.trigger[0].magazine;
		loaded = sl->st.loaded[mag]; reserve = sl->st.reserve[mag]; heat = sl->st.heat;
	}
	low_ammo = (h.loaded_ammo_cutoff && loaded <= h.loaded_ammo_cutoff) || (h.total_ammo_cutoff && reserve <= h.total_ammo_cutoff);
	for (i = 0; i < h.elem_count; i++) {
		huddef_elem e = huddef_get_elem(s, (uint32_t)h.elem_first + i);
		uint32_t col;
		/* statics and overlay sub elements (the "x" and clip icon after the ammo count are the latter) */
		if ((e.role != HUDDEF_R_STATIC && e.role != HUDDEF_R_OVERLAY) || e.allowed == HUDDEF_MAP_MULTIPLAYER) continue;
		col = e.colour;
		if (!col) { if (!low_ammo || !flash_on(tick, e.flash_period, e.flash_length)) continue; col = e.flash_colour; }
		draw_elem(f, s, &e, col, tick);
	}
	for (i = 0; i < h.meter_count; i++) {
		huddef_meter m = huddef_get_meter(s, (uint32_t)h.meter_first + i);
		float frac = 0.0f;
		if (m.role != HUDDEF_M_WEAPON || m.allowed == HUDDEF_MAP_MULTIPLAYER) continue;
		if (!sl || sl->weapon < 0) continue;
		switch (m.state) {
		case HUDDEF_S_LOADED_AMMO: frac = sl->def.magazine[mag].rounds_loaded_max ? (float)loaded / (float)sl->def.magazine[mag].rounds_loaded_max : 0.0f; break;
		case HUDDEF_S_TOTAL_AMMO: frac = sl->def.magazine[mag].rounds_total_max ? (float)reserve / (float)sl->def.magazine[mag].rounds_total_max : 0.0f; break;
		case HUDDEF_S_HEAT: frac = heat; break;
		case HUDDEF_S_AGE: frac = 1.0f - sl->st.age; break;
		default: continue;
		}
		draw_meter(f, s, &m, frac, tick, 0);
	}
	for (i = 0; i < h.number_count; i++) {
		huddef_number n = huddef_get_number(s, (uint32_t)h.number_first + i);
		unsigned v = 0;
		uint32_t col;
		if (n.role != HUDDEF_N_WEAPON || n.allowed == HUDDEF_MAP_MULTIPLAYER || !sl || sl->weapon < 0) continue;
		if ((n.flags & HUDDEF_NUM_ONLY_ZOOMED) && !g->zoomed) continue;
		if (n.state == HUDDEF_S_TOTAL_AMMO) v = reserve; else if (n.state == HUDDEF_S_LOADED_AMMO) v = loaded; else continue;
		col = n.colour ? n.colour : 0x80808080u;
		if (low_ammo && n.flash_colour && flash_on(tick, n.flash_period, n.flash_length) && (n.state == HUDDEF_S_LOADED_AMMO ? (h.loaded_ammo_cutoff && loaded <= h.loaded_ammo_cutoff) : 1)) col = n.flash_colour;
		draw_number(f, s, &gl, &n, v, col);
	}
	/* crosshair: the zoom reticle when zoomed, else the aim reticle */
	if (depth == 0 && sl && sl->weapon >= 0) {
		huddef_crosshair c;
		uint32_t type = g->zoomed ? HUDDEF_X_ZOOM : HUDDEF_X_AIM;
		if (!huddef_find_crosshair(s, hud_tag, type, &c) && type != HUDDEF_X_AIM) { type = HUDDEF_X_AIM; huddef_find_crosshair(s, hud_tag, type, &c); }
		for (i = 0; i < c.elem_count; i++) {
			huddef_elem e = huddef_get_elem(s, (uint32_t)c.elem_first + i);
			uint32_t col = e.colour ? e.colour : 0x80808080u;
			if (e.role != HUDDEF_R_CROSSHAIR) continue;
			if ((e.flags & HUDDEF_CF_ONLY_ZOOMED) && !g->zoomed) continue;
			if (low_ammo && loaded == 0) col = scale_colour(col, 0.6f);
			draw_elem(f, s, &e, col, tick);
		}
	}
	if (h.child_tag && h.child_tag != hud_tag) draw_weapon_hud(f, s, h.child_tag, g, tick, depth + 1);
}

void hud_state_init(hud_state_t *s) { memset(s, 0, sizeof *s); }

void hud_message(hud_state_t *s, const char *text, unsigned ticks)
{
	int i, slot = -1;
	for (i = 0; i < HUD_MSG_COUNT; i++) if (!s->ticks[i]) { slot = i; break; }
	if (slot < 0) {                                  /* all busy: scroll up and reuse the last slot */
		for (i = 0; i + 1 < HUD_MSG_COUNT; i++) { memcpy(s->text[i], s->text[i + 1], HUD_MSG_LEN); s->ticks[i] = s->ticks[i + 1]; }
		slot = HUD_MSG_COUNT - 1;
	}
	strncpy(s->text[slot], text, HUD_MSG_LEN - 1); s->text[slot][HUD_MSG_LEN - 1] = 0;
	s->ticks[slot] = (uint16_t)ticks;
}

void hud_tick(hud_state_t *s)
{
	int i;
	for (i = 0; i < HUD_MSG_COUNT; i++) if (s->ticks[i]) s->ticks[i]--;
}

int hud_text(hud_frame_t *f, const huddef_fonts *fonts, uint32_t font_tag, float x, float y, const char *text, uint32_t rgba)
{
	int fi, pen = 0;
	huddef_font fd;
	if (!fonts || !fonts->font_count) return 0;
	fi = huddef_find_font(fonts, font_tag);
	if (fi < 0) fi = 0;
	fd = huddef_get_font(fonts, (uint32_t)fi);
	for (; *text; text++) {
		huddef_glyph gl;
		if (!huddef_find_glyph(fonts, (uint32_t)fi, (uint32_t)(unsigned char)*text, &gl)) { if (!huddef_find_glyph(fonts, (uint32_t)fi, '?', &gl)) continue; }
		if (gl.tex != HUDDEF_NONE && gl.w && gl.h && f->n < HUD_MAX_QUADS) {
			hud_quad_t *q = &f->q[f->n++];
			q->x0 = x + (float)(pen + gl.ox); q->y0 = y + (float)fd.ascent - (float)gl.oy;
			q->x1 = q->x0 + (float)gl.w; q->y1 = q->y0 + (float)gl.h;
			q->tex = gl.tex; q->u0 = gl.u0; q->v0 = gl.v0; q->u1 = gl.u1; q->v1 = gl.v1;
			q->rgba = rgba; q->blend = HUDDEF_BLEND_ALPHA;
		}
		pen += gl.advance;
	}
	return pen;
}

void hud_build(hud_frame_t *f, const huddef_section *s, const huddef_fonts *fonts, const game_t *g, uint32_t tick, const hud_state_t *st)
{
	uint32_t tag = 0;
	huddef_globals gl;
	memset(f, 0, sizeof *f);
	if (!s || !s->hud_count) return;
	gl = huddef_get_globals(s);
	/* unit HUD: backgrounds and the shield / health meters */
	if (g->has_biped && huddef_find_link(s, HUDDEF_L_UNIT, g->biped.tag_id, 0, &tag)) {
		int hi = huddef_find_hud(s, tag);
		if (hi >= 0) {
			huddef_hud h = huddef_get_hud(s, (uint32_t)hi);
			unsigned i;
			float shield = g->vitals.max_shield > 0.0f ? (g->vitals.shield + g->vitals.overshield) / g->vitals.max_shield : 0.0f;
			float health = g->vitals.max_health > 0.0f ? g->vitals.health / g->vitals.max_health : 0.0f;
			float cx = 0.0f, cy = 0.0f, radius = 0.0f;
			int have_sensor = 0;
			for (i = 0; i < h.elem_count; i++) {
				huddef_elem e = huddef_get_elem(s, (uint32_t)h.elem_first + i);
				if (e.role == HUDDEF_R_HUD_BG || e.role == HUDDEF_R_SHIELD_BG || e.role == HUDDEF_R_HEALTH_BG || e.role == HUDDEF_R_MOTION_BG ||
				    e.role == HUDDEF_R_MOTION_FG)
					draw_elem(f, s, &e, e.colour ? e.colour : 0x80808080u, tick);
				if (e.role == HUDDEF_R_MOTION_BG && e.sprite != HUDDEF_NONE) have_sensor = 1;
				if (e.role == HUDDEF_R_MOTION_CENTER) {   /* radar centre; the centre's distance from the sensor corner is the radar radius */
					int ox = e.off_x < 0 ? -e.off_x : e.off_x, oy = e.off_y < 0 ? -e.off_y : e.off_y;
					cx = (float)e.x; cy = (float)e.y; radius = (float)(ox < oy ? ox : oy);
				}
			}
			for (i = 0; i < h.meter_count; i++) {
				huddef_meter m = huddef_get_meter(s, (uint32_t)h.meter_first + i);
				if (m.role == HUDDEF_M_SHIELD) draw_meter(f, s, &m, shield, tick, shield < 0.25f && shield > 0.0f);
				else if (m.role == HUDDEF_M_HEALTH) draw_meter(f, s, &m, health, tick, health < 0.25f);
			}
			if (have_sensor) draw_blips(f, s, &gl, g, cx, cy, radius);
		}
	}
	/* weapon HUD (or the default one with only a crosshair when the hands are empty) */
	{
		const game_slot_t *sl = game_cur_slot(g);
		uint32_t wt = 0;
		if (sl && sl->weapon >= 0 && !(huddef_find_link(s, HUDDEF_L_WEAPON, sl->def.tag_id, 0, &wt))) wt = sl->def.hud_tag;
		if (!wt) wt = gl.default_weapon_hud;
		if (wt) draw_weapon_hud(f, s, wt, g, tick, 0);
	}
	/* grenades */
	if (huddef_find_link(s, HUDDEF_L_GRENADE, (uint32_t)g->grenade_type, 0, &tag)) {
		int hi = huddef_find_hud(s, tag);
		if (hi >= 0) {
			huddef_hud h = huddef_get_hud(s, (uint32_t)hi);
			unsigned i;
			for (i = 0; i < h.elem_count; i++) {
				huddef_elem e = huddef_get_elem(s, (uint32_t)h.elem_first + i);
				if (e.role == HUDDEF_R_GRENADE_BG || e.role == HUDDEF_R_GRENADE_COUNT_BG) draw_elem(f, s, &e, e.colour ? e.colour : 0x80808080u, tick);
			}
			for (i = 0; i < h.number_count; i++) {
				huddef_number n = huddef_get_number(s, (uint32_t)h.number_first + i);
				if (n.role == HUDDEF_N_GRENADE) draw_number(f, s, &gl, &n, g->grenades[g->grenade_type & 1], n.colour ? n.colour : 0x80808080u);
			}
		}
	}
	/* messages, the pickup prompt and the pause banner */
	if (fonts && fonts->font_count) {
		unsigned i;
		for (i = 0; st && i < HUD_MSG_COUNT; i++)
			if (st->ticks[i] && st->text[i][0]) hud_text(f, fonts, gl.font_sp, 24.0f, 372.0f + 22.0f * (float)i, st->text[i], 0x80808080u);
		if (g->nearby_weapon >= 0 && g->objects && g->ow.scen) {
			char line[HUD_MSG_LEN];
			scen_def d = scen_get_def(g->ow.scen, (uint32_t)g->objects->obj[g->nearby_weapon].def);
			snprintf(line, sizeof line, "Press TRIANGLE to pick up the %s", d.name);
			hud_text(f, fonts, gl.font_sp, 140.0f, 330.0f, line, 0x80808080u);
		}
		if (st && st->paused) hud_text(f, fonts, gl.font_sp, 270.0f, 220.0f, "PAUSED", 0x80808080u);
	}
}
