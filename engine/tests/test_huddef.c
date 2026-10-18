/* Host test: open the HUD0 / FONT / STRS / HUDT sections of the pack written by tools/tests/test_hudconv.py (HUD_FIXTURE)
 * and check them against the synthetic world that test builds (7 HUD tags: 2 unit, 3 weapon, 2 grenade; 27 elements,
 * 6 meters, 4 numbers, 4 crosshairs, 26 sprites, 2 fonts of 5 glyphs, 2 string lists, 2 message sets). Then truncate
 * every section at every length, corrupt every table field and flip every byte: a corrupt section is either rejected or
 * safe to walk completely (ASan / UBSan watch for reads outside the buffer). */
#include "stream/hp2.h"
#include "ui/huddef.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
/* tag ids of the synthetic map: index i of the tag list, salt 0xE174 + i (tagindex.TagIndex.build) */
#define TID(i) (((uint32_t)(0xE174u + (i)) << 16) | (uint32_t)(i))
enum { T_HUDG = 2, T_UNIT = 4, T_VEH = 5, T_RIFLE = 6, T_MASTER = 7, T_JUNK = 8, T_GREN = 9, T_PLASMA = 10, T_FONT_L = 11,
       T_FONT_S = 12, T_ITEMS = 13, T_ICONS = 14, T_HMT_UI = 15, T_HMT_LEVEL = 16, T_WEAP_RIFLE = 21, T_BIPD = 23, T_VEHI = 24,
       T_SND = 25, T_LSND = 26 };

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static volatile uint32_t sink;

/* Touch everything an opened HUD0 hands out. */
static void walk0(const huddef_section *s)
{
	uint32_t acc = 0, i, k, ty;
	huddef_globals g = huddef_get_globals(s);
	if (isnan(g.item_scale_x) || isnan(g.motion_scale)) abort();
	acc += g.font_sp + g.digit_count + g.icon_count + g.blip_sprite;
	for (i = 0; i < s->sprite_count; i++) {
		huddef_sprite sp = huddef_get_sprite(s, i);
		if (sp.u0 >= sp.u1 || sp.v0 >= sp.v1 || sp.w == 0 || sp.h == 0) abort();
		acc += sp.tex + sp.w + sp.h;
	}
	for (i = 0; i < s->hud_count; i++) {
		huddef_hud h = huddef_get_hud(s, i);
		if (huddef_find_hud(s, h.tag_id) < 0) abort();
		for (k = 0; k < h.elem_count; k++) {
			huddef_elem e = huddef_get_elem(s, (uint32_t)h.elem_first + k);
			if (e.sprite != HUDDEF_NONE) {
				uint32_t f;
				if ((uint32_t)e.sprite + e.frames > s->sprite_count || e.frames == 0) abort();
				for (f = 0; f < e.frames; f++) acc += huddef_get_sprite(s, e.sprite + f).tex;
			}
			if (isnan(e.scale_x) || isnan(e.scale_y)) abort();
			acc += e.role + e.x + e.y + e.w + e.h + e.colour;
		}
		for (k = 0; k < h.meter_count; k++) {
			huddef_meter m = huddef_get_meter(s, (uint32_t)h.meter_first + k);
			if (m.sprite != HUDDEF_NONE && m.sprite >= s->sprite_count) abort();
			if (isnan(m.opacity) || isnan(m.translucency) || isnan(m.min_fraction)) abort();
			acc += m.role + m.x + m.colour_min;
		}
		for (k = 0; k < h.number_count; k++) {
			huddef_number n = huddef_get_number(s, (uint32_t)h.number_first + k);
			if (n.max_digits == 0 || n.max_digits > 12) abort();
			acc += n.x + n.w + n.flags;
		}
		for (k = 0; k < h.cross_count; k++) {
			huddef_crosshair c = huddef_get_crosshair(s, (uint32_t)h.cross_first + k);
			uint32_t m;
			if ((uint32_t)c.elem_first + c.elem_count > s->elem_count || c.type > HUDDEF_X_MAX) abort();
			for (m = 0; m < c.elem_count; m++) acc += huddef_get_elem(s, (uint32_t)c.elem_first + m).sprite;
		}
		for (k = 0; k < h.sound_count; k++) acc += huddef_get_sound(s, (uint32_t)h.sound_first + k).latched_to;
		for (ty = 0; ty <= 33; ty++) {
			huddef_crosshair c;
			if (huddef_find_crosshair(s, h.tag_id, ty, &c)) {
				if (c.type != ty || c.hud_index >= s->hud_count) abort();
				acc += c.elem_count;
			}
		}
	}
	for (i = 0; i < s->icon_count; i++) acc += huddef_get_icon(s, i).sprite;
	for (i = 0; i < s->link_count; i++) {
		huddef_link l = huddef_get_link(s, i);
		uint32_t tag = 0;
		if (!huddef_find_link(s, l.kind, l.owner, l.order, &tag)) abort();
		acc += l.hud_tag + tag;
	}
	sink = acc;
}

static void walk_font(const huddef_fonts *f)
{
	uint32_t acc = 0, i, c;
	static const uint16_t text[5] = { 'A', 'B', ' ', 'z', '?' };
	for (i = 0; i < f->font_count; i++) {
		huddef_font fi = huddef_get_font(f, i);
		if (huddef_find_font(f, fi.tag_id) < 0) abort();
		for (c = 0; c < 200; c++) {
			huddef_glyph g;
			if (huddef_find_glyph(f, i, c, &g)) {
				if (g.code != c) abort();
				if (g.w * g.h != 0 && (g.tex == HUDDEF_NONE || g.u1 - g.u0 != g.w || g.v1 - g.v0 != g.h)) abort();
				acc += g.advance + g.w + g.tex;
			}
		}
		acc += (uint32_t)huddef_text_width(f, i, text, 5);
	}
	sink = acc;
}

static void walk_strings(const huddef_strings *st)
{
	uint32_t acc = 0, l, i, k, m;
	uint16_t buf[8], big[64];
	for (l = 0; l < st->list_count + 1; l++) {
		uint32_t n = huddef_list_size(st, l);
		for (i = 0; i < n + 1; i++) {
			uint32_t len = huddef_get_string(st, l, i, buf, 8);
			if (len != HUDDEF_STR_NONE) {
				if (huddef_get_string(st, l, i, big, 64) != len) abort();
				acc += len + buf[0];
			}
			acc += huddef_get_string(st, l, i, NULL, 0) == len ? 1u : 0u;
		}
	}
	for (l = 0; l < st->set_count + 1; l++)
		for (m = 0; m < huddef_set_size(st, l) + 1; m++) {
			uint32_t n = huddef_segment_count(st, l, m);
			for (k = 0; k < n + 1; k++) {
				huddef_segment sg;
				int ok = huddef_get_segment(st, l, m, k, &sg, buf, 8);
				if (ok != (k < n)) abort();
				if (ok) acc += sg.kind + sg.icon + sg.length + buf[0];
			}
		}
	sink = acc;
}

static int load_section(const hp2_pack *pk, const char *tag, uint8_t **raw, size_t *n)
{
	const hp2_section *sec = hp2_find(pk, tag);
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(pk, sec, *raw, sec->raw_size) == HP2_OK);
	*n = sec->raw_size;
	return 0;
}

static int near1(float a, float b) { return fabsf(a - b) < 1e-5f; }

int main(int argc, char **argv)
{
	CHECK(argc == 2 || (argc == 3 && strcmp(argv[2], "smoke") == 0));
	FILE *f = fopen(argv[1], "rb");
	CHECK(f);
	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	rewind(f);
	uint8_t *img = malloc(n);
	CHECK(img && fread(img, 1, n, f) == n);
	fclose(f);

	hp2_pack pk;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	uint8_t *raw0, *rawf, *raws, *rawt;
	size_t n0, nf, ns, nt;
	if (load_section(&pk, "HUD0", &raw0, &n0) || load_section(&pk, "FONT", &rawf, &nf) ||
	    load_section(&pk, "STRS", &raws, &ns) || load_section(&pk, "HUDT", &rawt, &nt))
		return 1;

	if (argc == 3) {                                  /* smoke mode: any real pack, open and walk everything */
		huddef_section rs;
		huddef_fonts rf;
		huddef_strings rst;
		tex_section rts;
		CHECK(huddef_open(&rs, raw0, n0) == HUDDEF_OK && huddef_fonts_open(&rf, rawf, nf) == HUDDEF_OK);
		CHECK(huddef_strings_open(&rst, raws, ns) == HUDDEF_OK && tex_open(&rts, rawt, nt) == TEX_OK);
		CHECK(huddef_check_textures(&rs, &rts) == HUDDEF_OK && huddef_check_font_textures(&rf, &rts) == HUDDEF_OK);
		walk0(&rs); walk_font(&rf); walk_strings(&rst);
		printf("ok: %u huds %u elems %u meters %u numbers %u crosshairs %u sprites, %u fonts %u glyphs, %u lists %u sets %u messages, %u textures\n",
		       rs.hud_count, rs.elem_count, rs.meter_count, rs.number_count, rs.cross_count, rs.sprite_count, rf.font_count,
		       rf.glyph_count, rst.list_count, rst.set_count, rst.message_count, rts.texture_count);
		return 0;
	}

	/* ---- HUD0 ---- */
	huddef_section s;
	CHECK(huddef_open(&s, raw0, n0) == HUDDEF_OK);
	CHECK(s.hud_count == 7 && s.elem_count == 27 && s.meter_count == 6 && s.number_count == 4 && s.cross_count == 4);
	CHECK(s.sound_count == 2 && s.sprite_count == 27 && s.icon_count == 3 && s.link_count == 6);   /* 26 + the motion sensor blip */
	CHECK(((uintptr_t)s.huds | (uintptr_t)s.elems | (uintptr_t)s.sprites) % 16 == (uintptr_t)raw0 % 16);
	tex_section ts;
	CHECK(tex_open(&ts, rawt, nt) == TEX_OK && ts.texture_count == 12);
	CHECK(huddef_check_textures(&s, &ts) == HUDDEF_OK);

	huddef_globals g = huddef_get_globals(&s);
	CHECK(g.anchor == HUDDEF_A_TL && g.off_x == 0 && g.off_y == 60);
	CHECK(g.font_sp == TID(T_FONT_L) && g.font_mp == TID(T_FONT_S) && near1(g.item_scale_x, 2.0f) && near1(g.item_scale_y, 2.0f));
	CHECK(g.default_weapon_hud == TID(T_MASTER) && g.item_strings == TID(T_ITEMS) && g.icon_strings == TID(T_ICONS));
	CHECK(g.messages == TID(T_HMT_UI) && g.level_messages == TID(T_HMT_LEVEL));
	CHECK(near1(g.motion_range, 20.0f) && near1(g.motion_scale, 32.0f) && fabsf(g.motion_velocity - 0.0002f) < 1e-8f);
	CHECK(g.digit_first == 0 && g.digit_count == 15 && g.bitmap_digit_width == 12 && g.screen_digit_width == 9);
	CHECK(g.digit_x_offset == 1 && g.digit_y_offset == 0 && g.decimal_width == 6 && g.colon_width == 6 && g.icon_count == 3);
	CHECK(g.damage_sprite == 17 && g.damage_mp_sprite == 16 && g.damage_colour == 0x64000080u);
	CHECK(g.blip_sprite == 26 && huddef_get_sprite(&s, g.blip_sprite).w == 32);      /* the blip bitmap found by path, last sprite */
	CHECK((g.text_colour_a >> 24) == 0x40);                                    /* float alpha 0.5 -> GS 0x40 */

	/* sprites: digit 9 (12 x 20 px at texel 16,32), the 100 x 20 padded image, the halved 512 x 16 image */
	huddef_sprite sp = huddef_get_sprite(&s, 9);
	CHECK(sp.w == 12 && sp.h == 20 && sp.u0 == 16 && sp.v0 == 32 && sp.u1 == 28 && sp.v1 == 52 && sp.flags == 0);
	sp = huddef_get_sprite(&s, 19);
	CHECK(sp.w == 100 && sp.h == 20 && sp.u1 == 100 && sp.flags == HUDDEF_SF_PADDED);
	sp = huddef_get_sprite(&s, 20);
	CHECK(sp.w == 512 && sp.h == 16 && sp.u1 == 256 && sp.v1 == 8 && sp.flags == HUDDEF_SF_SCALED);
	sp = huddef_get_sprite(&s, 21);
	CHECK(sp.w == 32 && sp.h == 16 && sp.reg_x == 16 && sp.reg_y == 8);
	sp = huddef_get_sprite(&s, 27);                                            /* out of range: neutral */
	CHECK(sp.tex == HUDDEF_NONE && sp.w == 0);

	/* HUD records */
	CHECK(huddef_find_hud(&s, TID(T_UNIT)) == 0 && huddef_find_hud(&s, TID(T_VEH)) == 1 && huddef_find_hud(&s, TID(T_RIFLE)) == 2);
	CHECK(huddef_find_hud(&s, TID(T_GREN)) == 5 && huddef_find_hud(&s, 0) == -1 && huddef_find_hud(&s, TID(T_HUDG)) == -1);
	huddef_hud h = huddef_get_hud(&s, 0);
	CHECK(h.kind == HUDDEF_UNIT && h.anchor == HUDDEF_A_TR && h.elem_first == 0 && h.elem_count == 7 && h.meter_count == 3);
	CHECK(h.sound_first == 0 && h.sound_count == 2 && h.icon_seq == HUDDEF_NONE);
	huddef_hud rifle = huddef_get_hud(&s, 2), master = huddef_get_hud(&s, 3), junk = huddef_get_hud(&s, 4);
	CHECK(rifle.kind == HUDDEF_WEAPON && rifle.anchor == HUDDEF_A_BL && rifle.child_tag == TID(T_MASTER) && rifle.crosshair_mask == 3);
	CHECK(rifle.total_ammo_cutoff == 61 && rifle.loaded_ammo_cutoff == 10 && rifle.heat_cutoff == 0 && rifle.age_cutoff == 0);
	CHECK(rifle.icon_seq == 2 && rifle.icon_width_offset == 2 && rifle.icon_sprite == 25 && rifle.cross_count == 2);
	CHECK(master.child_tag == 0 && master.flags == 1 && master.crosshair_mask == 0x108u && master.cross_count == 2);
	CHECK(junk.icon_seq == HUDDEF_NONE && junk.icon_sprite == HUDDEF_NONE && junk.elem_count == 0);

	/* unit HUD elements, resolved for the top right anchor */
	huddef_elem e = huddef_get_elem(&s, 1);
	CHECK(e.role == HUDDEF_R_SHIELD_BG && e.w == 128 && e.h == 32 && e.x == 464 && e.y == 37 && e.sprite == 18 && e.frames == 1);
	CHECK(e.colour == 0x6E806B55u && e.flash_period == 90 && e.flash_delay == 7 && e.flash_count == 3 && e.flash_length == 7);
	CHECK(e.anchor == HUDDEF_A_TR && e.blend == HUDDEF_BLEND_ALPHA && near1(e.scale_x, 1.0f));
	e = huddef_get_elem(&s, 2);
	CHECK(e.role == HUDDEF_R_HEALTH_BG && e.x == 497 && e.y == 54 && e.off_x == -5 && e.off_y == 18 && e.w == 100);
	e = huddef_get_elem(&s, 3);
	CHECK(e.role == HUDDEF_R_MOTION_BG && e.w == 512 && e.h == 16 && e.x == 48 && e.y == 444 - 16 && e.anchor == HUDDEF_A_BL && e.seq == 2);
	e = huddef_get_elem(&s, 0);
	CHECK(e.role == HUDDEF_R_HUD_BG && e.sprite == HUDDEF_NONE && e.frames == 0 && e.state == HUDDEF_NO_STATE);
	e = huddef_get_elem(&s, 5);
	CHECK(e.role == HUDDEF_R_MOTION_CENTER && e.off_x == 41 && e.off_y == 41 && e.x == 89 && e.y == 403 && e.anchor == HUDDEF_A_BL);
	e = huddef_get_elem(&s, 6);
	CHECK(e.role == HUDDEF_R_AUX_BG && e.frames == 3 && e.w == 32 && e.x == 567 && e.sprite == 22);
	e = huddef_get_elem(&s, 7);                                                /* vehicle HUD: centre anchor, width scale 2 */
	CHECK(e.anchor == HUDDEF_A_C && near1(e.scale_x, 2.0f) && near1(e.scale_y, 1.0f) && e.w == 64 && e.h == 16 && e.x == 288 && e.y == 192);
	e = huddef_get_elem(&s, 27);
	CHECK(e.sprite == HUDDEF_NONE && e.state == HUDDEF_NO_STATE && near1(e.scale_x, 1.0f));

	/* meters */
	huddef_meter m = huddef_get_meter(&s, 0);
	CHECK(m.role == HUDDEF_M_SHIELD && m.w == 32 && m.h == 16 && m.x == 553 && m.y == 36 && m.alpha_mult == 1 && m.alpha_bias == 55);
	CHECK(m.value_scale == 198 && m.flags == 0 && m.colour_empty == 0 && near1(m.translucency, 0.5f) && near1(m.opacity, 0.0f));
	CHECK(m.colour_min == 0x80502800u);                                        /* alpha 0 in the tag -> opaque (0x80) */
	m = huddef_get_meter(&s, 1);
	CHECK(m.role == HUDDEF_M_HEALTH && m.flags == 1 && m.min_value == 31 && m.alpha_mult == 30 && m.alpha_bias == 15 && m.off_x == 36);
	m = huddef_get_meter(&s, 2);
	CHECK(m.role == HUDDEF_M_AUX && near1(m.min_fraction, 0.25f) && m.aux_flags == 1 && m.y == 69);
	m = huddef_get_meter(&s, 5);                                               /* rifle: the weapon meter, bottom left */
	CHECK(m.role == HUDDEF_M_WEAPON && m.state == HUDDEF_S_LOADED_AMMO && m.allowed == HUDDEF_MAP_ANY && m.y == 444 + 15 - 16);
	CHECK(huddef_get_meter(&s, 6).sprite == HUDDEF_NONE && huddef_get_meter(&s, 6).role == 0);

	/* numbers */
	huddef_number nb = huddef_get_number(&s, 0);
	CHECK(nb.role == HUDDEF_N_WEAPON && nb.state == HUDDEF_S_TOTAL_AMMO && nb.allowed == HUDDEF_MAP_SOLO && nb.max_digits == 3);
	CHECK(nb.flags == HUDDEF_NUM_LEADING_ZEROS && nb.fraction_digits == 0 && nb.w == 27 && nb.h == 20 && nb.x == 48 + 29 && nb.y == 444 + 3 - 20);
	CHECK(near1(nb.scale_x, 1.0f) && nb.flash_period == 30 && nb.flash_length == 30);
	nb = huddef_get_number(&s, 1);
	CHECK(nb.allowed == HUDDEF_MAP_MULTIPLAYER && nb.max_digits == 2 && nb.flags == 3 && nb.fraction_digits == 1 && near1(nb.scale_x, 1.5f));
	CHECK(nb.w == 36);
	nb = huddef_get_number(&s, 2);
	CHECK(nb.role == HUDDEF_N_GRENADE && nb.max_digits == 1 && nb.x == 48 + 107);

	/* crosshairs: the rifle's own, the master HUD's through the child chain */
	huddef_crosshair c;
	CHECK(huddef_find_crosshair(&s, TID(T_RIFLE), HUDDEF_X_AIM, &c) && c.type == 0 && c.elem_first == 15 && c.elem_count == 1 && c.hud_index == 2);
	e = huddef_get_elem(&s, c.elem_first);
	CHECK(e.role == HUDDEF_R_CROSSHAIR && e.anchor == HUDDEF_A_C && e.flags == HUDDEF_CF_FLASHES && e.sprite == 21);
	CHECK(e.colour == 0x80804B14u && e.x == 305 && e.y == 230 && e.w == 32 && e.h == 16);
	CHECK(huddef_find_crosshair(&s, TID(T_RIFLE), HUDDEF_X_ZOOM, &c) && c.elem_first == 16 && (huddef_get_elem(&s, 16).flags & HUDDEF_CF_ONLY_ZOOMED));
	CHECK(huddef_get_elem(&s, 16).frames == 3);
	CHECK(huddef_find_crosshair(&s, TID(T_RIFLE), HUDDEF_X_SHOULD_RELOAD, &c) && c.type == 3 && c.hud_index == 3 && c.elem_first == 17);
	CHECK(huddef_find_crosshair(&s, TID(T_RIFLE), HUDDEF_X_FIRING_NO_AMMO, &c) && c.hud_index == 3 && c.elem_first == 18);
	CHECK(!huddef_find_crosshair(&s, TID(T_RIFLE), HUDDEF_X_CHARGE, &c) && c.elem_count == 0);
	CHECK(!huddef_find_crosshair(&s, TID(T_MASTER), HUDDEF_X_AIM, &c));          /* the child does not borrow from the parent */
	CHECK(!huddef_find_crosshair(&s, 0xDEAD, HUDDEF_X_AIM, &c) && !huddef_find_crosshair(&s, TID(T_UNIT), 0, &c));
	CHECK(huddef_get_crosshair(&s, 2).type == 3 && huddef_get_crosshair(&s, 4).elem_count == 0);

	/* weapon statics / overlays / grenade elements */
	e = huddef_get_elem(&s, 13);
	CHECK(e.role == HUDDEF_R_STATIC && e.state == 1 && e.allowed == 1 && e.x == 60 && e.y == 372 && e.w == 128);
	e = huddef_get_elem(&s, 14);
	CHECK(e.role == HUDDEF_R_OVERLAY && e.type == 16 && e.flags == 1 && e.seq == 2 && e.x == 101 && e.y == 432 && e.allowed == 1);
	e = huddef_get_elem(&s, 21);
	CHECK(e.role == HUDDEF_R_GRENADE_OVERLAY && e.seq == 2 && e.type == 1 && huddef_get_elem(&s, 22).type == 4);

	/* sounds, icons, links */
	huddef_sound so = huddef_get_sound(&s, 0);
	CHECK(so.sound_tag == TID(T_SND) && so.group == 0x21646E73u && so.latched_to == 2 && near1(so.scale, 0.5f) && so.hud_tag == TID(T_UNIT));
	so = huddef_get_sound(&s, 1);
	CHECK(so.sound_tag == TID(T_LSND) && so.group == 0x646E736Cu && so.latched_to == 1);
	huddef_icon ic = huddef_get_icon(&s, 0);
	CHECK(ic.seq == 0 && ic.width_offset == -6 && ic.x == -2 && ic.y == -5 && ic.sprite == 15 && ic.b13 == 2 && ic.index == 0);
	CHECK(huddef_get_icon(&s, 2).seq == 3 && huddef_get_icon(&s, 2).index == 2 && huddef_get_icon(&s, 2).sprite == 17);
	uint32_t tag = 0;
	CHECK(huddef_find_link(&s, HUDDEF_L_WEAPON, TID(T_WEAP_RIFLE), 0, &tag) && tag == TID(T_RIFLE));
	CHECK(huddef_find_link(&s, HUDDEF_L_UNIT, TID(T_BIPD), 0, &tag) && tag == TID(T_UNIT));
	CHECK(huddef_find_link(&s, HUDDEF_L_UNIT, TID(T_BIPD), 1, &tag) && tag == TID(T_VEH));
	CHECK(huddef_find_link(&s, HUDDEF_L_UNIT, TID(T_VEHI), 0, &tag) && tag == TID(T_VEH) && !huddef_find_link(&s, HUDDEF_L_UNIT, TID(T_VEHI), 1, &tag));
	CHECK(huddef_find_link(&s, HUDDEF_L_GRENADE, 1, 0, &tag) && tag == TID(T_PLASMA) && !huddef_find_link(&s, HUDDEF_L_WEAPON, TID(T_BIPD), 0, NULL));
	walk0(&s);
	huddef_section none;
	CHECK(huddef_open(&none, raw0, 10) == HUDDEF_ERR_TRUNCATED);
	CHECK(none.hud_count == 0 && huddef_get_hud(&none, 0).tag_id == 0 && huddef_get_globals(&none).digit_first == HUDDEF_NONE);
	walk0(&none);

	/* ---- FONT ---- */
	huddef_fonts fo;
	CHECK(huddef_fonts_open(&fo, rawf, nf) == HUDDEF_OK && fo.font_count == 2 && fo.glyph_count == 10);
	CHECK(huddef_find_font(&fo, TID(T_FONT_L)) == 0 && huddef_find_font(&fo, TID(T_FONT_S)) == 1 && huddef_find_font(&fo, 5) == -1);
	huddef_font fi = huddef_get_font(&fo, 0);
	CHECK(fi.ascent == 17 && fi.descent == 4 && fi.leading_h == 4 && fi.leading_w == 0 && fi.glyph_first == 0 && fi.glyph_count == 5 && fi.tex_first == 10 && fi.tex_count == 1);
	CHECK(huddef_get_font(&fo, 1).ascent == 8 && huddef_get_font(&fo, 1).glyph_first == 5 && huddef_get_font(&fo, 1).tex_first == 11);
	huddef_glyph gl;
	CHECK(huddef_find_glyph(&fo, 0, 'A', &gl) && gl.advance == 10 && gl.w == 6 && gl.h == 8 && gl.ox == 1 && gl.oy == 8 && gl.tex == 10);
	CHECK(gl.u1 - gl.u0 == 6 && gl.v1 - gl.v0 == 8);
	CHECK(huddef_find_glyph(&fo, 0, ' ', &gl) && gl.advance == 6 && gl.w == 0 && gl.tex == HUDDEF_NONE);
	CHECK(huddef_find_glyph(&fo, 1, 'B', &gl) && gl.w == 12 && gl.tex == 11);
	CHECK(!huddef_find_glyph(&fo, 0, 'z', &gl) && gl.tex == HUDDEF_NONE && !huddef_find_glyph(&fo, 2, 'A', &gl) && !huddef_find_glyph(&fo, 0, 0x10000u, &gl));
	{
		static const uint16_t txt[5] = { 'A', 'B', ' ', 'z', '?' };
		CHECK(huddef_text_width(&fo, 0, txt, 5) == 10 + 9 + 6 + 7 + 7);       /* 'z' counts as '?' */
		CHECK(huddef_text_width(&fo, 0, txt, 0) == 0 && huddef_text_width(&fo, 7, txt, 5) == 0);
	}
	CHECK(huddef_check_font_textures(&fo, &ts) == HUDDEF_OK);
	walk_font(&fo);

	/* ---- STRS ---- */
	huddef_strings st;
	CHECK(huddef_strings_open(&st, raws, ns) == HUDDEF_OK && st.list_count == 2 && st.set_count == 2);
	int l0 = huddef_find_list(&st, TID(T_ITEMS)), l1 = huddef_find_list(&st, TID(T_ICONS));
	CHECK(l0 == 0 && l1 == 1 && huddef_find_list(&st, 1) == -1 && huddef_list_size(&st, 0) == 3 && huddef_list_size(&st, 1) == 2);
	uint16_t buf[32];
	CHECK(huddef_get_string(&st, 0, 1, buf, 32) == 18 && buf[0] == 'P' && buf[10] == '%' && buf[11] == 'd' && buf[18] == 0);
	CHECK(huddef_get_string(&st, 0, 2, buf, 32) == 0 && buf[0] == 0);          /* empty string */
	CHECK(huddef_get_string(&st, 1, 1, buf, 3) == 5 && buf[0] == 'w' && buf[1] == 'h' && buf[2] == 0);      /* truncated copy, still NUL terminated */
	CHECK(huddef_get_string(&st, 1, 2, buf, 32) == HUDDEF_STR_NONE && huddef_get_string(&st, 2, 0, buf, 32) == HUDDEF_STR_NONE);
	CHECK(huddef_get_string(&st, 0, 0, NULL, 0) == 17);
	int s0 = huddef_find_set(&st, TID(T_HMT_UI)), s1 = huddef_find_set(&st, TID(T_HMT_LEVEL));
	CHECK(s0 == 0 && s1 == 1 && huddef_find_set(&st, 9) == -1 && huddef_set_size(&st, 0) == 2 && huddef_set_size(&st, 1) == 1);
	int mp = huddef_find_message(&st, 0, "pickup"), me = huddef_find_message(&st, 0, "exit");
	CHECK(mp == 0 && me == 1 && huddef_find_message(&st, 0, "tip") == -1 && huddef_find_message(&st, 1, "tip") == 0 && huddef_find_message(&st, 5, "x") == -1);
	CHECK(huddef_segment_count(&st, 0, 0) == 5 && huddef_segment_count(&st, 0, 1) == 3 && huddef_segment_count(&st, 0, 2) == 0);
	huddef_segment sg;
	CHECK(huddef_get_segment(&st, 0, 0, 0, &sg, buf, 32) && sg.kind == HUDDEF_SEG_TEXT && sg.length == 5 && buf[0] == 'H' && buf[4] == ' ' && buf[5] == 0);
	CHECK(huddef_get_segment(&st, 0, 0, 1, &sg, buf, 32) && sg.kind == HUDDEF_SEG_ICON && sg.icon == 18 && buf[0] == 0);
	CHECK(huddef_get_segment(&st, 0, 0, 3, &sg, NULL, 0) && sg.kind == HUDDEF_SEG_ICON && sg.icon == 32);
	CHECK(huddef_get_segment(&st, 0, 0, 4, &sg, buf, 32) && sg.kind == HUDDEF_SEG_TEXT && sg.length == 0);
	CHECK(!huddef_get_segment(&st, 0, 0, 5, &sg, buf, 32) && sg.length == 0 && !huddef_get_segment(&st, 3, 0, 0, &sg, buf, 32));
	CHECK(huddef_get_segment(&st, 1, 0, 0, &sg, buf, 32) && sg.length == 7 && buf[0] == 'L');
	walk_strings(&st);

	/* ---- hostile input: HUD0 ---- */
	huddef_section bad;
	uint8_t *copy = malloc(n0 + 16);
	CHECK(copy);
	for (size_t cut = 0; cut < n0; cut++) CHECK(huddef_open(&bad, raw0, cut) != HUDDEF_OK);
	CHECK(huddef_open(&bad, NULL, n0) != HUDDEF_OK);
	memcpy(copy, raw0, n0);
	memset(copy + n0, 0, 16);
	CHECK(huddef_open(&bad, copy, n0 + 16) == HUDDEF_ERR_LAYOUT);
	memcpy(copy, raw0, n0);
	copy[4] = 1;                                                               /* HUD0 version 1 packs are rejected (ADR-060) */
	CHECK(huddef_open(&bad, copy, n0) == HUDDEF_ERR_VERSION);
	memcpy(copy, raw0, n0);
	copy[0] = 'X';
	CHECK(huddef_open(&bad, copy, n0) == HUDDEF_ERR_MAGIC);
	/* every counted table: moved, misaligned, oversized, counted wrong */
	for (int t = 0; t < 9; t++) {
		uint32_t cnt = rd32(raw0 + 0x0C + 4 * t), off = rd32(raw0 + 0x38 + 4 * t);
		const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)n0 + 16, cnt }, { off, 0xFFFFFFFFu }, { off, (uint32_t)n0 },
		                                                { 0, cnt }, { 0xFFFFFFF0u, cnt } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw0, n0);
			wr32(copy + 0x38 + 4 * t, tries[k].off);
			wr32(copy + 0x0C + 4 * t, tries[k].cnt);
			if (huddef_open(&bad, copy, n0) == HUDDEF_OK) { fprintf(stderr, "table %d try %zu accepted\n", t, k); return 1; }
		}
		for (int d = -1; d <= 1; d += 2) {                                 /* merely wrong counts must never read out of bounds */
			memcpy(copy, raw0, n0);
			wr32(copy + 0x0C + 4 * t, cnt + (uint32_t)d);
			if (huddef_open(&bad, copy, n0) == HUDDEF_OK) walk0(&bad);
		}
	}
	memcpy(copy, raw0, n0);
	wr32(copy + 0x34, 0);
	CHECK(huddef_open(&bad, copy, n0) != HUDDEF_OK);
	/* targeted fields: each must be refused */
	{
		uint32_t oh = rd32(raw0 + 0x38), oe = rd32(raw0 + 0x3C), om = rd32(raw0 + 0x40), on = rd32(raw0 + 0x44), oc = rd32(raw0 + 0x48);
		uint32_t os = rd32(raw0 + 0x50), oi = rd32(raw0 + 0x54), ol = rd32(raw0 + 0x58), og = rd32(raw0 + 0x34);
		const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
			{ "sprite empty rect", os + 12, 0, 2 }, { "sprite zero width", os + 4, 0, 2 }, { "sprite flags", os + 2, 7, 2 },
			{ "elem role", oe, 14, 1 }, { "elem anchor", oe + 0x22, 5, 1 }, { "elem blend", oe + 0x23, 7, 1 }, { "elem state", oe + 1, 8, 1 },
			{ "elem allowed", oe + 2, 3, 1 }, { "elem sprite past the table", oe + 64 + 4, 27, 2 }, { "elem frames past the table", oe + 64 + 3, 10, 1 },
			{ "elem frames without sprite", oe + 3, 1, 1 }, { "elem scale NaN", oe + 0x18, 0x7FC00000u, 4 }, { "elem scale inf", oe + 0x1C, 0x7F800000u, 4 },
			{ "meter role 0", om, 0, 1 }, { "meter role 5", om, 5, 1 }, { "meter anchor", om + 7, 5, 1 }, { "meter sprite", om + 4, 27, 2 },
			{ "meter opacity NaN", om + 0x38, 0x7FC00000u, 4 }, { "meter min fraction inf", om + 0x40, 0x7F800000u, 4 },
			{ "number role", on, 0, 1 }, { "number digits 0", on + 3, 0, 1 }, { "number digits 13", on + 3, 13, 1 }, { "number flags", on + 4, 8, 1 },
			{ "number fraction", on + 5, 5, 1 }, { "number anchor", on + 6, 5, 1 }, { "number scale NaN", on + 0x14, 0x7FC00000u, 4 },
			{ "crosshair type", oc, 32, 1 }, { "crosshair elems past the table", oc + 2, 27, 2 }, { "crosshair count", oc + 4, 28, 2 },
			{ "sound scale NaN", rd32(raw0 + 0x4C) + 0x0C, 0x7FC00000u, 4 }, { "icon sprite", oi, 27, 2 }, { "link kind", ol + 4, 3, 4 },
			{ "hud kind", oh + 4, 3, 1 }, { "hud anchor", oh + 5, 5, 1 }, { "hud elems past the table", oh + 0x16, 28, 2 },
			{ "hud meters past the table", oh + 0x1A, 7, 2 }, { "hud numbers past", oh + 0x1E, 5, 2 }, { "hud crosshairs past", oh + 0x22, 5, 2 },
			{ "hud sounds past", oh + 0x26, 3, 2 }, { "hud icon sprite", oh + 0x34, 27, 2 },
			{ "globals anchor", og, 5, 1 }, { "globals item scale NaN", og + 0x18, 0x7FC00000u, 4 }, { "globals motion inf", og + 0x44, 0x7F800000u, 4 },
			{ "globals digit range", og + 0x5A, 30, 2 }, { "globals icon count", og + 0x62, 4, 2 }, { "globals damage sprite", og + 0x50, 27, 2 }, { "globals blip sprite", og + 0x64, 27, 2 },
			{ "header size", 8, 1, 4 } };
		(void)om;
		for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
			memcpy(copy, raw0, n0);
			if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
			else if (hit[k].bytes == 2) { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
			else copy[hit[k].at] = (uint8_t)hit[k].val;
			if (huddef_open(&bad, copy, n0) == HUDDEF_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
		}
	}
	/* sprite rectangles vs the real textures: a rectangle past its texture is caught by the texture check */
	memcpy(copy, raw0, n0);
	copy[rd32(raw0 + 0x50) + 12] = 0xFF;
	copy[rd32(raw0 + 0x50) + 13] = 0x7F;
	CHECK(huddef_open(&bad, copy, n0) == HUDDEF_OK && huddef_check_textures(&bad, &ts) == HUDDEF_ERR_LAYOUT);
	memcpy(copy, raw0, n0);
	copy[rd32(raw0 + 0x50)] = 0xFF;                                          /* texture index past the HUDT table */
	CHECK(huddef_open(&bad, copy, n0) == HUDDEF_OK && huddef_check_textures(&bad, &ts) == HUDDEF_ERR_LAYOUT);
	/* no single-byte change anywhere may let an out-of-bounds read through */
	for (size_t at = 4; at < n0; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw0, n0);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (huddef_open(&bad, copy, n0) == HUDDEF_OK) walk0(&bad);
		}
	free(copy);

	/* ---- hostile input: FONT ---- */
	huddef_fonts fbad;
	copy = malloc(nf + 16);
	CHECK(copy);
	for (size_t cut = 0; cut < nf; cut++) CHECK(huddef_fonts_open(&fbad, rawf, cut) != HUDDEF_OK);
	memcpy(copy, rawf, nf);
	memset(copy + nf, 0, 16);
	CHECK(huddef_fonts_open(&fbad, copy, nf + 16) == HUDDEF_ERR_LAYOUT);
	{
		uint32_t of = rd32(rawf + 0x14), og = rd32(rawf + 0x18);
		const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
			{ "glyph range past the table", of + 20, 99, 4 }, { "glyph codes out of order", og + 24, 65, 2 }, { "glyph rect width", og + 24 * 2 + 18, 5, 2 },
			{ "glyph rect height", og + 24 * 2 + 20, 0, 2 }, { "glyph without texture", og + 24 * 2 + 12, 0xFFFF, 2 }, { "font count", 0x0C, 65, 4 },
			{ "glyph count", 0x10, 0xFFFFFFFFu, 4 }, { "fonts offset", 0x14, 4, 4 }, { "header size", 8, 1, 4 } };
		for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
			memcpy(copy, rawf, nf);
			if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
			else { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
			if (huddef_fonts_open(&fbad, copy, nf) == HUDDEF_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
		}
	}
	for (size_t at = 4; at < nf; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, rawf, nf);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (huddef_fonts_open(&fbad, copy, nf) == HUDDEF_OK) walk_font(&fbad);
		}
	free(copy);

	/* ---- hostile input: STRS ---- */
	huddef_strings sbad;
	copy = malloc(ns + 16);
	CHECK(copy);
	for (size_t cut = 0; cut < ns; cut++) CHECK(huddef_strings_open(&sbad, raws, cut) != HUDDEF_OK);
	memcpy(copy, raws, ns);
	memset(copy + ns, 0, 16);
	CHECK(huddef_strings_open(&sbad, copy, ns + 16) == HUDDEF_ERR_LAYOUT);
	{
		uint32_t ol = rd32(raws + 0x24), osets = rd32(raws + 0x28), om = rd32(raws + 0x2C), oseg = rd32(raws + 0x30), otab = rd32(raws + 0x34);
		const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
			{ "list range past the strings", ol + 8, 90, 4 }, { "list count", ol + 4, 90, 4 }, { "set range past the messages", osets + 8, 9, 4 },
			{ "message name unterminated", om + 31, 'x', 1 }, { "message segments past the table", om + 0x22, 90, 2 },
			{ "segment kind", oseg, 2, 1 }, { "segment text past the characters", oseg + 4, 0x7FFFFFFFu, 4 }, { "string offset", otab, 0x7FFFFFFFu, 4 },
			{ "string length", otab + 4, 9999, 4 }, { "string not NUL terminated", otab + 4, 1, 4 }, { "string table offset", 0x34, 4, 4 },
			{ "character units", 0x20, 0xFFFFFFFFu, 4 }, { "header size", 8, 1, 4 } };
		for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
			memcpy(copy, raws, ns);
			if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
			else if (hit[k].bytes == 2) { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
			else copy[hit[k].at] = (uint8_t)hit[k].val;
			if (huddef_strings_open(&sbad, copy, ns) == HUDDEF_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
		}
	}
	for (size_t at = 4; at < ns; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raws, ns);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (huddef_strings_open(&sbad, copy, ns) == HUDDEF_OK) walk_strings(&sbad);
		}
	free(copy);
	free(raw0); free(rawf); free(raws); free(rawt); free(img);
	puts("ok");
	return 0;
}
