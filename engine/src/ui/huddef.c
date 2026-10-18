/* HUD0 / FONT / STRS readers (docs/20-hud-section.md). Pure C11, no heap, no double. The *_open functions validate every
 * table once, so the accessors can never read out of bounds, whatever the bytes. Records are decoded with byte loads. */
#include "ui/huddef.h"
#include <string.h>

#define HUD0_HEADER 128u
#define FONT_HEADER 64u
#define STRS_HEADER 64u
#define SPRITE_REC 24u
#define ELEM_REC 64u
#define METER_REC 80u
#define NUMBER_REC 64u
#define CROSS_REC 16u
#define HUD_REC 64u
#define SOUND_REC 32u
#define ICON_REC 32u
#define LINK_REC 16u
#define GLOBALS_REC 192u
#define FONT_REC 48u
#define GLYPH_REC 24u
#define LIST_REC 32u
#define SET_REC 32u
#define MSG_REC 48u
#define SEG_REC 8u
#define STR_REC 8u
#define MAX_HUDS 255u
#define MAX_ELEMS 4096u
#define MAX_SPRITES 65534u
#define MAX_TABLE (1u << 20)
#define VERSION 1u          /* FONT, STRS */
#define HUD0_VERSION 2u     /* 2: anchors on the title-safe frame, motion sensor bottom left, blip sprite at globals +0x64 */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }

/* table [off, off + count * rec): 16-aligned, past the header, inside the section */
static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size, uint32_t header)
{
	if (off % 16u || off < header || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

/* ============================================================ HUD0 ============================================== */
/* header: +0 "HUD0", +4 u16 version, +8 size, +0x0C huds, +0x10 elems, +0x14 meters, +0x18 numbers, +0x1C crosshairs,
 * +0x20 sounds, +0x24 sprites, +0x28 icons, +0x2C links, +0x34 globals offset, then the table offsets from +0x38: huds,
 * elems, meters, numbers, crosshairs, sounds, sprites, icons, links */

static huddef_status check_range(uint32_t first, uint32_t count, uint32_t total) { return (uint64_t)first + count <= total ? HUDDEF_OK : HUDDEF_ERR_LAYOUT; }

huddef_status huddef_open(huddef_section *s, const uint8_t *d, size_t size)
{
	uint32_t n[9], off[9], og, i;
	static const uint32_t rec[9] = { HUD_REC, ELEM_REC, METER_REC, NUMBER_REC, CROSS_REC, SOUND_REC, SPRITE_REC, ICON_REC, LINK_REC };
	static const uint32_t limit[9] = { MAX_HUDS, MAX_ELEMS, MAX_ELEMS, MAX_ELEMS, MAX_ELEMS, MAX_ELEMS, MAX_SPRITES, MAX_ELEMS, MAX_ELEMS };

	memset(s, 0, sizeof *s);
	if (!d || size < HUD0_HEADER) return HUDDEF_ERR_TRUNCATED;
	if (memcmp(d, "HUD0", 4) != 0) return HUDDEF_ERR_MAGIC;
	if (rd16(d + 4) != HUD0_VERSION) return HUDDEF_ERR_VERSION;
	if (rd32(d + 8) > size) return HUDDEF_ERR_TRUNCATED;
	if (rd32(d + 8) != size) return HUDDEF_ERR_LAYOUT;
	for (i = 0; i < 9; i++) {
		n[i] = rd32(d + 0x0C + 4 * i);
		off[i] = rd32(d + 0x38 + 4 * i);
		if (n[i] > limit[i] || !table_ok(off[i], n[i], rec[i], size, HUD0_HEADER)) return HUDDEF_ERR_LAYOUT;
	}
	og = rd32(d + 0x34);
	if (!table_ok(og, 1, GLOBALS_REC, size, HUD0_HEADER)) return HUDDEF_ERR_LAYOUT;

	/* sprites */
	for (i = 0; i < n[6]; i++) {
		const uint8_t *p = d + off[6] + (size_t)i * SPRITE_REC;
		if (rd16(p + 8) >= rd16(p + 12) || rd16(p + 10) >= rd16(p + 14)) return HUDDEF_ERR_LAYOUT;    /* empty rectangle */
		if (rd16(p + 4) == 0 || rd16(p + 6) == 0 || rd16(p + 4) > 2048u || rd16(p + 6) > 2048u) return HUDDEF_ERR_LAYOUT;
		if (rd16(p + 2) > 3u) return HUDDEF_ERR_LAYOUT;
	}
	/* elements */
	for (i = 0; i < n[1]; i++) {
		const uint8_t *p = d + off[1] + (size_t)i * ELEM_REC;
		uint32_t sprite = rd16(p + 4), frames = p[3];
		if (p[0] > HUDDEF_R_GRENADE_OVERLAY || p[0x22] > HUDDEF_A_C || p[0x23] > TEX_BLEND_SUBTRACT) return HUDDEF_ERR_LAYOUT;
		if (p[1] != HUDDEF_NO_STATE && p[1] > HUDDEF_S_TARGET_ELEVATION) return HUDDEF_ERR_LAYOUT;
		if (p[2] > HUDDEF_MAP_MULTIPLAYER) return HUDDEF_ERR_LAYOUT;
		if (sprite != HUDDEF_NONE && (frames == 0 || (uint64_t)sprite + frames > n[6])) return HUDDEF_ERR_LAYOUT;
		if (sprite == HUDDEF_NONE && frames != 0) return HUDDEF_ERR_LAYOUT;
		if (!finite_at(p + 0x18) || !finite_at(p + 0x1C)) return HUDDEF_ERR_LAYOUT;
	}
	/* meters */
	for (i = 0; i < n[2]; i++) {
		const uint8_t *p = d + off[2] + (size_t)i * METER_REC;
		uint32_t sprite = rd16(p + 4);
		if (p[0] < HUDDEF_M_SHIELD || p[0] > HUDDEF_M_WEAPON || p[7] > HUDDEF_A_C) return HUDDEF_ERR_LAYOUT;
		if (sprite != HUDDEF_NONE && sprite >= n[6]) return HUDDEF_ERR_LAYOUT;
		if (!finite_at(p + 0x14) || !finite_at(p + 0x18) || !finite_at(p + 0x38) || !finite_at(p + 0x3C) || !finite_at(p + 0x40))
			return HUDDEF_ERR_LAYOUT;
	}
	/* numbers */
	for (i = 0; i < n[3]; i++) {
		const uint8_t *p = d + off[3] + (size_t)i * NUMBER_REC;
		if (p[0] < HUDDEF_N_WEAPON || p[0] > HUDDEF_N_GRENADE || p[6] > HUDDEF_A_C) return HUDDEF_ERR_LAYOUT;
		if (p[3] == 0 || p[3] > 12u || p[4] > 7u || p[5] > 4u) return HUDDEF_ERR_LAYOUT;
		if (!finite_at(p + 0x14) || !finite_at(p + 0x18)) return HUDDEF_ERR_LAYOUT;
	}
	/* crosshairs: type and the range of overlay elements */
	for (i = 0; i < n[4]; i++) {
		const uint8_t *p = d + off[4] + (size_t)i * CROSS_REC;
		if (p[0] > HUDDEF_X_MAX || check_range(rd16(p + 2), rd16(p + 4), n[1]) != HUDDEF_OK) return HUDDEF_ERR_LAYOUT;
	}
	for (i = 0; i < n[5]; i++)
		if (!finite_at(d + off[5] + (size_t)i * SOUND_REC + 0x0C)) return HUDDEF_ERR_LAYOUT;
	for (i = 0; i < n[7]; i++) {
		const uint8_t *p = d + off[7] + (size_t)i * ICON_REC;
		if (rd16(p) != HUDDEF_NONE && rd16(p) >= n[6]) return HUDDEF_ERR_LAYOUT;
	}
	for (i = 0; i < n[8]; i++)
		if (rd32(d + off[8] + (size_t)i * LINK_REC + 4) > HUDDEF_L_GRENADE) return HUDDEF_ERR_LAYOUT;
	/* HUDs: kind, anchor, ranges in every table */
	for (i = 0; i < n[0]; i++) {
		const uint8_t *p = d + off[0] + (size_t)i * HUD_REC;
		uint32_t isp = rd16(p + 0x34);
		if (p[4] > HUDDEF_GRENADE || p[5] > HUDDEF_A_C) return HUDDEF_ERR_LAYOUT;
		if (check_range(rd16(p + 0x14), rd16(p + 0x16), n[1]) || check_range(rd16(p + 0x18), rd16(p + 0x1A), n[2]) ||
		    check_range(rd16(p + 0x1C), rd16(p + 0x1E), n[3]) || check_range(rd16(p + 0x20), rd16(p + 0x22), n[4]) ||
		    check_range(rd16(p + 0x24), rd16(p + 0x26), n[5]))
			return HUDDEF_ERR_LAYOUT;
		if (isp != HUDDEF_NONE && isp >= n[6]) return HUDDEF_ERR_LAYOUT;
	}
	/* globals */
	{
		const uint8_t *g = d + og;
		uint32_t df = rd16(g + 0x58), dc = rd16(g + 0x5A);
		if (g[0] > HUDDEF_A_C) return HUDDEF_ERR_LAYOUT;
		for (i = 0; i < 2; i++)
			if (!finite_at(g + 0x18 + 4 * i)) return HUDDEF_ERR_LAYOUT;
		for (i = 0; i < 3; i++)
			if (!finite_at(g + 0x44 + 4 * i)) return HUDDEF_ERR_LAYOUT;
		if (dc && (df == HUDDEF_NONE || (uint64_t)df + dc > n[6])) return HUDDEF_ERR_LAYOUT;
		if (rd16(g + 0x62) != n[7]) return HUDDEF_ERR_LAYOUT;
		if ((rd16(g + 0x50) != HUDDEF_NONE && rd16(g + 0x50) >= n[6]) || (rd16(g + 0x52) != HUDDEF_NONE && rd16(g + 0x52) >= n[6]))
			return HUDDEF_ERR_LAYOUT;
		if (rd16(g + 0x64) != HUDDEF_NONE && rd16(g + 0x64) >= n[6]) return HUDDEF_ERR_LAYOUT;
	}
	s->data = d;
	s->size = size;
	s->hud_count = n[0]; s->elem_count = n[1]; s->meter_count = n[2]; s->number_count = n[3]; s->cross_count = n[4];
	s->sound_count = n[5]; s->sprite_count = n[6]; s->icon_count = n[7]; s->link_count = n[8];
	s->globals = d + og;
	s->huds = d + off[0]; s->elems = d + off[1]; s->meters = d + off[2]; s->numbers = d + off[3]; s->crosses = d + off[4];
	s->sounds = d + off[5]; s->sprites = d + off[6]; s->icons = d + off[7]; s->links = d + off[8];
	return HUDDEF_OK;
}

huddef_status huddef_check_textures(const huddef_section *s, const tex_section *t)
{
	uint32_t i;
	for (i = 0; i < s->sprite_count; i++) {
		const uint8_t *p = s->sprites + (size_t)i * SPRITE_REC;
		uint32_t tex = rd16(p);
		tex_info ti;
		if (tex >= t->texture_count) return HUDDEF_ERR_LAYOUT;
		ti = tex_get(t, tex);
		if (rd16(p + 12) > ti.width || rd16(p + 14) > ti.height) return HUDDEF_ERR_LAYOUT;
	}
	return HUDDEF_OK;
}

huddef_globals huddef_get_globals(const huddef_section *s)
{
	huddef_globals r;
	const uint8_t *g = s->globals;
	memset(&r, 0, sizeof r);
	r.damage_sprite = r.damage_mp_sprite = r.digit_first = r.blip_sprite = HUDDEF_NONE;
	if (!g) return r;
	r.anchor = g[0];
	r.off_x = rds16(g + 2); r.off_y = rds16(g + 4);
	r.font_sp = rd32(g + 0x10); r.font_mp = rd32(g + 0x14);
	r.item_scale_x = rdf(g + 0x18); r.item_scale_y = rdf(g + 0x1C);
	r.text_colour_a = rd32(g + 0x20); r.text_colour_b = rd32(g + 0x24);
	r.default_weapon_hud = rd32(g + 0x28);
	r.item_strings = rd32(g + 0x2C); r.icon_strings = rd32(g + 0x30);
	r.messages = rd32(g + 0x34); r.level_messages = rd32(g + 0x38);
	r.icon_bitmap = rd32(g + 0x3C); r.digit_tag = rd32(g + 0x40);
	r.motion_range = rdf(g + 0x44); r.motion_velocity = rdf(g + 0x48); r.motion_scale = rdf(g + 0x4C);
	r.damage_sprite = rd16(g + 0x50); r.damage_mp_sprite = rd16(g + 0x52);
	r.damage_colour = rd32(g + 0x54);
	r.digit_first = rd16(g + 0x58); r.digit_count = rd16(g + 0x5A);
	r.bitmap_digit_width = (int8_t)g[0x5C]; r.screen_digit_width = (int8_t)g[0x5D];
	r.digit_x_offset = (int8_t)g[0x5E]; r.digit_y_offset = (int8_t)g[0x5F];
	r.decimal_width = (int8_t)g[0x60]; r.colon_width = (int8_t)g[0x61];
	r.icon_count = rd16(g + 0x62);
	r.blip_sprite = rd16(g + 0x64);
	return r;
}

huddef_sprite huddef_get_sprite(const huddef_section *s, uint32_t i)
{
	huddef_sprite r;
	memset(&r, 0, sizeof r);
	r.tex = HUDDEF_NONE;
	if (i < s->sprite_count) {
		const uint8_t *p = s->sprites + (size_t)i * SPRITE_REC;
		r.tex = rd16(p); r.flags = rd16(p + 2); r.w = rd16(p + 4); r.h = rd16(p + 6);
		r.u0 = rd16(p + 8); r.v0 = rd16(p + 10); r.u1 = rd16(p + 12); r.v1 = rd16(p + 14);
		r.reg_x = rds16(p + 16); r.reg_y = rds16(p + 18);
		r.bitmap_tag = rd32(p + 20);
	}
	return r;
}

huddef_hud huddef_get_hud(const huddef_section *s, uint32_t i)
{
	huddef_hud r;
	memset(&r, 0, sizeof r);
	r.icon_seq = r.icon_sprite = HUDDEF_NONE;
	if (i < s->hud_count) {
		const uint8_t *p = s->huds + (size_t)i * HUD_REC;
		r.tag_id = rd32(p); r.kind = p[4]; r.anchor = p[5]; r.flags = rd16(p + 6); r.child_tag = rd32(p + 8);
		r.total_ammo_cutoff = rd16(p + 0x0C); r.loaded_ammo_cutoff = rd16(p + 0x0E);
		r.heat_cutoff = rd16(p + 0x10); r.age_cutoff = rd16(p + 0x12);
		r.elem_first = rd16(p + 0x14); r.elem_count = rd16(p + 0x16);
		r.meter_first = rd16(p + 0x18); r.meter_count = rd16(p + 0x1A);
		r.number_first = rd16(p + 0x1C); r.number_count = rd16(p + 0x1E);
		r.cross_first = rd16(p + 0x20); r.cross_count = rd16(p + 0x22);
		r.sound_first = rd16(p + 0x24); r.sound_count = rd16(p + 0x26);
		r.crosshair_mask = rd32(p + 0x28);
		r.icon_seq = rd16(p + 0x2C);
		r.icon_width_offset = rds16(p + 0x2E); r.icon_x = rds16(p + 0x30); r.icon_y = rds16(p + 0x32);
		r.icon_sprite = rd16(p + 0x34);
	}
	return r;
}

int huddef_find_hud(const huddef_section *s, uint32_t tag_id)
{
	uint32_t i;
	for (i = 0; i < s->hud_count; i++)
		if (rd32(s->huds + (size_t)i * HUD_REC) == tag_id) return (int)i;
	return -1;
}

huddef_elem huddef_get_elem(const huddef_section *s, uint32_t i)
{
	huddef_elem r;
	memset(&r, 0, sizeof r);
	r.state = HUDDEF_NO_STATE;
	r.sprite = HUDDEF_NONE;
	r.scale_x = r.scale_y = 1.0f;
	if (i < s->elem_count) {
		const uint8_t *p = s->elems + (size_t)i * ELEM_REC;
		r.role = p[0]; r.state = p[1]; r.allowed = p[2]; r.frames = p[3];
		r.sprite = rd16(p + 4);
		r.flags = rd32(p + 8);
		r.x = rds16(p + 0x0C); r.y = rds16(p + 0x0E); r.w = rds16(p + 0x10); r.h = rds16(p + 0x12);
		r.off_x = rds16(p + 0x14); r.off_y = rds16(p + 0x16);
		r.scale_x = rdf(p + 0x18); r.scale_y = rdf(p + 0x1C);
		r.scaling_flags = rd16(p + 0x20); r.anchor = p[0x22]; r.blend = p[0x23];
		r.colour = rd32(p + 0x24); r.flash_colour = rd32(p + 0x28); r.disabled_colour = rd32(p + 0x2C);
		r.flash_period = rd16(p + 0x30); r.flash_delay = rd16(p + 0x32); r.flash_count = rd16(p + 0x34);
		r.flash_flags = rd16(p + 0x36); r.flash_length = rd16(p + 0x38);
		r.frame_rate = rd16(p + 0x3A); r.seq = rd16(p + 0x3C); r.type = rd16(p + 0x3E);
	}
	return r;
}

huddef_meter huddef_get_meter(const huddef_section *s, uint32_t i)
{
	huddef_meter r;
	memset(&r, 0, sizeof r);
	r.state = HUDDEF_NO_STATE;
	r.sprite = HUDDEF_NONE;
	r.scale_x = r.scale_y = 1.0f;
	if (i < s->meter_count) {
		const uint8_t *p = s->meters + (size_t)i * METER_REC;
		r.role = p[0]; r.state = p[1]; r.allowed = p[2]; r.flags = p[3];
		r.sprite = rd16(p + 4); r.min_value = p[6]; r.anchor = p[7];
		r.x = rds16(p + 8); r.y = rds16(p + 10); r.w = rds16(p + 12); r.h = rds16(p + 14);
		r.off_x = rds16(p + 16); r.off_y = rds16(p + 18);
		r.scale_x = rdf(p + 0x14); r.scale_y = rdf(p + 0x18);
		r.scaling_flags = rd16(p + 0x1C); r.alpha_mult = p[0x1E]; r.alpha_bias = p[0x1F];
		r.colour_min = rd32(p + 0x20); r.colour_max = rd32(p + 0x24); r.colour_flash = rd32(p + 0x28);
		r.colour_empty = rd32(p + 0x2C); r.colour_disabled = rd32(p + 0x30);
		r.value_scale = rd16(p + 0x34); r.seq = rd16(p + 0x36);
		r.opacity = rdf(p + 0x38); r.translucency = rdf(p + 0x3C);
		r.min_fraction = rdf(p + 0x40); r.aux_flags = rd32(p + 0x44);
	}
	return r;
}

huddef_number huddef_get_number(const huddef_section *s, uint32_t i)
{
	huddef_number r;
	memset(&r, 0, sizeof r);
	r.state = HUDDEF_NO_STATE;
	r.scale_x = r.scale_y = 1.0f;
	if (i < s->number_count) {
		const uint8_t *p = s->numbers + (size_t)i * NUMBER_REC;
		r.role = p[0]; r.state = p[1]; r.allowed = p[2]; r.max_digits = p[3]; r.flags = p[4]; r.fraction_digits = p[5];
		r.anchor = p[6];
		r.x = rds16(p + 8); r.y = rds16(p + 10); r.w = rds16(p + 12); r.h = rds16(p + 14);
		r.off_x = rds16(p + 16); r.off_y = rds16(p + 18);
		r.scale_x = rdf(p + 0x14); r.scale_y = rdf(p + 0x18);
		r.scaling_flags = rd16(p + 0x1C);
		r.colour = rd32(p + 0x20); r.flash_colour = rd32(p + 0x24); r.disabled_colour = rd32(p + 0x28);
		r.flash_period = rd16(p + 0x2C); r.flash_delay = rd16(p + 0x2E); r.flash_count = rd16(p + 0x30);
		r.flash_flags = rd16(p + 0x32); r.flash_length = rd16(p + 0x34);
	}
	return r;
}

huddef_crosshair huddef_get_crosshair(const huddef_section *s, uint32_t i)
{
	huddef_crosshair r;
	memset(&r, 0, sizeof r);
	if (i < s->cross_count) {
		const uint8_t *p = s->crosses + (size_t)i * CROSS_REC;
		r.type = p[0]; r.elem_first = rd16(p + 2); r.elem_count = rd16(p + 4); r.bitmap_tag = rd32(p + 8);
	}
	return r;
}

huddef_sound huddef_get_sound(const huddef_section *s, uint32_t i)
{
	huddef_sound r;
	memset(&r, 0, sizeof r);
	if (i < s->sound_count) {
		const uint8_t *p = s->sounds + (size_t)i * SOUND_REC;
		r.sound_tag = rd32(p); r.group = rd32(p + 4); r.latched_to = rd32(p + 8); r.scale = rdf(p + 12); r.hud_tag = rd32(p + 16);
	}
	return r;
}

huddef_icon huddef_get_icon(const huddef_section *s, uint32_t i)
{
	huddef_icon r;
	memset(&r, 0, sizeof r);
	r.sprite = HUDDEF_NONE;
	if (i < s->icon_count) {
		const uint8_t *p = s->icons + (size_t)i * ICON_REC;
		r.sprite = rd16(p); r.seq = rd16(p + 2); r.width_offset = rds16(p + 4); r.x = rds16(p + 6); r.y = rds16(p + 8);
		r.colour = rd32(p + 12); r.b12 = p[16]; r.b13 = p[17]; r.index = rd16(p + 18);
	}
	return r;
}

huddef_link huddef_get_link(const huddef_section *s, uint32_t i)
{
	huddef_link r;
	memset(&r, 0, sizeof r);
	if (i < s->link_count) {
		const uint8_t *p = s->links + (size_t)i * LINK_REC;
		r.owner = rd32(p); r.kind = rd32(p + 4); r.hud_tag = rd32(p + 8); r.order = rd32(p + 12);
	}
	return r;
}

int huddef_find_crosshair(const huddef_section *s, uint32_t hud_tag, uint32_t type, huddef_crosshair *out)
{
	uint32_t depth;
	memset(out, 0, sizeof *out);
	for (depth = 0; depth < HUDDEF_MAX_CHILD_DEPTH && hud_tag; depth++) {
		int hi = huddef_find_hud(s, hud_tag);
		huddef_hud h;
		uint32_t k;
		if (hi < 0) return 0;
		h = huddef_get_hud(s, (uint32_t)hi);
		for (k = 0; k < h.cross_count; k++) {
			huddef_crosshair c = huddef_get_crosshair(s, (uint32_t)h.cross_first + k);
			if (c.type == type) {
				*out = c;
				out->hud_index = (uint32_t)hi;
				return 1;
			}
		}
		hud_tag = h.child_tag;
	}
	return 0;
}

int huddef_find_link(const huddef_section *s, uint32_t kind, uint32_t owner, uint32_t order, uint32_t *hud_tag)
{
	uint32_t i;
	for (i = 0; i < s->link_count; i++) {
		const uint8_t *p = s->links + (size_t)i * LINK_REC;
		if (rd32(p) == owner && rd32(p + 4) == kind && rd32(p + 12) == order) {
			if (hud_tag) *hud_tag = rd32(p + 8);
			return 1;
		}
	}
	return 0;
}

/* ============================================================ FONT ============================================== */
/* header: +0 "FONT", +4 u16 version, +8 size, +0x0C fonts, +0x10 glyphs, +0x14 fonts offset, +0x18 glyphs offset */
huddef_status huddef_fonts_open(huddef_fonts *f, const uint8_t *d, size_t size)
{
	uint32_t nf, ng, of, og, i, k;
	memset(f, 0, sizeof *f);
	if (!d || size < FONT_HEADER) return HUDDEF_ERR_TRUNCATED;
	if (memcmp(d, "FONT", 4) != 0) return HUDDEF_ERR_MAGIC;
	if (rd16(d + 4) != VERSION) return HUDDEF_ERR_VERSION;
	if (rd32(d + 8) > size) return HUDDEF_ERR_TRUNCATED;
	if (rd32(d + 8) != size) return HUDDEF_ERR_LAYOUT;
	nf = rd32(d + 0x0C); ng = rd32(d + 0x10); of = rd32(d + 0x14); og = rd32(d + 0x18);
	if (nf > 64u || ng > MAX_TABLE) return HUDDEF_ERR_LAYOUT;
	if (!table_ok(of, nf, FONT_REC, size, FONT_HEADER) || !table_ok(og, ng, GLYPH_REC, size, FONT_HEADER)) return HUDDEF_ERR_LAYOUT;
	for (i = 0; i < nf; i++) {
		const uint8_t *p = d + of + (size_t)i * FONT_REC;
		uint32_t gf = rd32(p + 16), gc = rd32(p + 20), prev = 0;
		if ((uint64_t)gf + gc > ng) return HUDDEF_ERR_LAYOUT;
		for (k = 0; k < gc; k++) {
			const uint8_t *g = d + og + ((size_t)gf + k) * GLYPH_REC;
			uint32_t code = rd16(g), w = rd16(g + 4), h = rd16(g + 6);
			if (k && code <= prev) return HUDDEF_ERR_LAYOUT;                      /* ascending: binary search */
			prev = code;
			if (w * h != 0) {                                                      /* blanks (space) carry no texture */
				if (rd16(g + 12) == HUDDEF_NONE) return HUDDEF_ERR_LAYOUT;
				if (rd16(g + 18) < rd16(g + 14) || rd16(g + 18) - rd16(g + 14) != w ||
				    rd16(g + 20) < rd16(g + 16) || rd16(g + 20) - rd16(g + 16) != h)
					return HUDDEF_ERR_LAYOUT;
			}
		}
	}
	f->data = d;
	f->size = size;
	f->font_count = nf;
	f->glyph_count = ng;
	f->fonts = d + of;
	f->glyphs = d + og;
	return HUDDEF_OK;
}

huddef_status huddef_check_font_textures(const huddef_fonts *f, const tex_section *t)
{
	uint32_t i;
	for (i = 0; i < f->glyph_count; i++) {
		const uint8_t *g = f->glyphs + (size_t)i * GLYPH_REC;
		uint32_t tex = rd16(g + 12);
		tex_info ti;
		if (tex == HUDDEF_NONE) continue;
		if (tex >= t->texture_count) return HUDDEF_ERR_LAYOUT;
		ti = tex_get(t, tex);
		if (rd16(g + 18) > ti.width || rd16(g + 20) > ti.height) return HUDDEF_ERR_LAYOUT;
	}
	return HUDDEF_OK;
}

huddef_font huddef_get_font(const huddef_fonts *f, uint32_t index)
{
	huddef_font r;
	memset(&r, 0, sizeof r);
	if (index < f->font_count) {
		const uint8_t *p = f->fonts + (size_t)index * FONT_REC;
		r.tag_id = rd32(p); r.flags = rd32(p + 4);
		r.ascent = rds16(p + 8); r.descent = rds16(p + 10); r.leading_h = rds16(p + 12); r.leading_w = rds16(p + 14);
		r.glyph_first = rd32(p + 16); r.glyph_count = rd32(p + 20); r.tex_first = rd16(p + 24); r.tex_count = rd16(p + 26);
	}
	return r;
}

int huddef_find_font(const huddef_fonts *f, uint32_t tag_id)
{
	uint32_t i;
	for (i = 0; i < f->font_count; i++)
		if (rd32(f->fonts + (size_t)i * FONT_REC) == tag_id) return (int)i;
	return -1;
}

int huddef_find_glyph(const huddef_fonts *f, uint32_t font, uint32_t code, huddef_glyph *out)
{
	uint32_t lo, hi;
	huddef_font fi;
	memset(out, 0, sizeof *out);
	out->tex = HUDDEF_NONE;
	if (font >= f->font_count || code > 0xFFFFu) return 0;
	fi = huddef_get_font(f, font);
	lo = 0;
	hi = fi.glyph_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const uint8_t *g = f->glyphs + ((size_t)fi.glyph_first + mid) * GLYPH_REC;
		uint32_t c = rd16(g);
		if (c == code) {
			out->code = (uint16_t)c; out->advance = rds16(g + 2); out->w = rd16(g + 4); out->h = rd16(g + 6);
			out->ox = rds16(g + 8); out->oy = rds16(g + 10); out->tex = rd16(g + 12);
			out->u0 = rd16(g + 14); out->v0 = rd16(g + 16); out->u1 = rd16(g + 18); out->v1 = rd16(g + 20);
			return 1;
		}
		if (c < code) lo = mid + 1;
		else hi = mid;
	}
	return 0;
}

int32_t huddef_text_width(const huddef_fonts *f, uint32_t font, const uint16_t *s, uint32_t n)
{
	huddef_glyph g, q;
	int32_t w = 0, qa = 0;
	uint32_t i;
	if (huddef_find_glyph(f, font, '?', &q)) qa = q.advance;
	for (i = 0; i < n; i++) w += huddef_find_glyph(f, font, s[i], &g) ? g.advance : qa;
	return w;
}

/* ============================================================ STRS ============================================== */
/* header: +0 "STRS", +4 u16 version, +8 size, +0x0C lists, +0x10 sets, +0x14 messages, +0x18 segments, +0x1C strings,
 * +0x20 character units, then offsets +0x24 lists, +0x28 sets, +0x2C messages, +0x30 segments, +0x34 string table,
 * +0x38 characters (UTF-16LE, each string followed by a NUL unit) */
huddef_status huddef_strings_open(huddef_strings *st, const uint8_t *d, size_t size)
{
	uint32_t nl, ns, nm, nseg, nstr, nch, ol, os, om, oseg, otab, och, i;
	memset(st, 0, sizeof *st);
	if (!d || size < STRS_HEADER) return HUDDEF_ERR_TRUNCATED;
	if (memcmp(d, "STRS", 4) != 0) return HUDDEF_ERR_MAGIC;
	if (rd16(d + 4) != VERSION) return HUDDEF_ERR_VERSION;
	if (rd32(d + 8) > size) return HUDDEF_ERR_TRUNCATED;
	if (rd32(d + 8) != size) return HUDDEF_ERR_LAYOUT;
	nl = rd32(d + 0x0C); ns = rd32(d + 0x10); nm = rd32(d + 0x14); nseg = rd32(d + 0x18); nstr = rd32(d + 0x1C); nch = rd32(d + 0x20);
	ol = rd32(d + 0x24); os = rd32(d + 0x28); om = rd32(d + 0x2C); oseg = rd32(d + 0x30); otab = rd32(d + 0x34); och = rd32(d + 0x38);
	if (nl > 1024u || ns > 1024u || nm > MAX_TABLE || nseg > MAX_TABLE || nstr > MAX_TABLE || nch > MAX_TABLE * 16u) return HUDDEF_ERR_LAYOUT;
	if (!table_ok(ol, nl, LIST_REC, size, STRS_HEADER) || !table_ok(os, ns, SET_REC, size, STRS_HEADER) ||
	    !table_ok(om, nm, MSG_REC, size, STRS_HEADER) || !table_ok(oseg, nseg, SEG_REC, size, STRS_HEADER) ||
	    !table_ok(otab, nstr, STR_REC, size, STRS_HEADER) || !table_ok(och, (uint64_t)nch * 2u, 1u, size, STRS_HEADER))
		return HUDDEF_ERR_LAYOUT;
	for (i = 0; i < nstr; i++) {
		const uint8_t *p = d + otab + (size_t)i * STR_REC;
		uint64_t off = rd32(p), len = rd32(p + 4);
		if (off + len + 1u > nch) return HUDDEF_ERR_LAYOUT;                       /* text plus its NUL unit */
		if (rd16(d + och + (size_t)(off + len) * 2u) != 0) return HUDDEF_ERR_LAYOUT;
	}
	for (i = 0; i < nl; i++) {
		const uint8_t *p = d + ol + (size_t)i * LIST_REC;
		if ((uint64_t)rd32(p + 8) + rd32(p + 4) > nstr) return HUDDEF_ERR_LAYOUT;
	}
	for (i = 0; i < ns; i++) {
		const uint8_t *p = d + os + (size_t)i * SET_REC;
		if ((uint64_t)rd32(p + 8) + rd32(p + 4) > nm) return HUDDEF_ERR_LAYOUT;
	}
	for (i = 0; i < nm; i++) {
		const uint8_t *p = d + om + (size_t)i * MSG_REC;
		if (p[31] != 0) return HUDDEF_ERR_LAYOUT;                                   /* name NUL terminated */
		if ((uint32_t)rd16(p + 0x20) + rd16(p + 0x22) > nseg) return HUDDEF_ERR_LAYOUT;
	}
	for (i = 0; i < nseg; i++) {
		const uint8_t *p = d + oseg + (size_t)i * SEG_REC;
		if (p[0] > HUDDEF_SEG_ICON) return HUDDEF_ERR_LAYOUT;
		if (p[0] == HUDDEF_SEG_TEXT && (uint64_t)rd32(p + 4) + rd16(p + 2) + 1u > nch) return HUDDEF_ERR_LAYOUT;
		if (p[0] == HUDDEF_SEG_TEXT && rd16(d + och + ((size_t)rd32(p + 4) + rd16(p + 2)) * 2u) != 0) return HUDDEF_ERR_LAYOUT;
	}
	st->data = d;
	st->size = size;
	st->list_count = nl; st->set_count = ns; st->message_count = nm; st->segment_count = nseg; st->string_count = nstr;
	st->char_units = nch;
	st->lists = d + ol; st->sets = d + os; st->messages = d + om; st->segments = d + oseg; st->table = d + otab; st->chars = d + och;
	return HUDDEF_OK;
}

static uint32_t copy_units(const huddef_strings *st, uint32_t off, uint32_t len, uint16_t *dst, uint32_t cap)
{
	uint32_t i;
	if (dst && cap) {
		uint32_t n = len < cap ? len : cap - 1;
		for (i = 0; i < n; i++) dst[i] = rd16(st->chars + (size_t)(off + i) * 2u);
		dst[n] = 0;
	}
	return len;
}

int huddef_find_list(const huddef_strings *st, uint32_t tag_id)
{
	uint32_t i;
	for (i = 0; i < st->list_count; i++)
		if (rd32(st->lists + (size_t)i * LIST_REC) == tag_id) return (int)i;
	return -1;
}

uint32_t huddef_list_size(const huddef_strings *st, uint32_t list)
{
	return list < st->list_count ? rd32(st->lists + (size_t)list * LIST_REC + 4) : 0;
}

uint32_t huddef_get_string(const huddef_strings *st, uint32_t list, uint32_t index, uint16_t *dst, uint32_t cap)
{
	const uint8_t *l, *t;
	if (list >= st->list_count) { if (dst && cap) dst[0] = 0; return HUDDEF_STR_NONE; }
	l = st->lists + (size_t)list * LIST_REC;
	if (index >= rd32(l + 4)) { if (dst && cap) dst[0] = 0; return HUDDEF_STR_NONE; }
	t = st->table + ((size_t)rd32(l + 8) + index) * STR_REC;
	return copy_units(st, rd32(t), rd32(t + 4), dst, cap);
}

int huddef_find_set(const huddef_strings *st, uint32_t tag_id)
{
	uint32_t i;
	for (i = 0; i < st->set_count; i++)
		if (rd32(st->sets + (size_t)i * SET_REC) == tag_id) return (int)i;
	return -1;
}

uint32_t huddef_set_size(const huddef_strings *st, uint32_t set)
{
	return set < st->set_count ? rd32(st->sets + (size_t)set * SET_REC + 4) : 0;
}

int huddef_find_message(const huddef_strings *st, uint32_t set, const char *name)
{
	uint32_t i, n, first;
	if (set >= st->set_count || !name) return -1;
	n = rd32(st->sets + (size_t)set * SET_REC + 4);
	first = rd32(st->sets + (size_t)set * SET_REC + 8);
	for (i = 0; i < n; i++)
		if (strcmp((const char *)(st->messages + ((size_t)first + i) * MSG_REC), name) == 0) return (int)i;
	return -1;
}

static const uint8_t *message_rec(const huddef_strings *st, uint32_t set, uint32_t message)
{
	if (set >= st->set_count || message >= rd32(st->sets + (size_t)set * SET_REC + 4)) return NULL;
	return st->messages + ((size_t)rd32(st->sets + (size_t)set * SET_REC + 8) + message) * MSG_REC;
}

uint32_t huddef_segment_count(const huddef_strings *st, uint32_t set, uint32_t message)
{
	const uint8_t *m = message_rec(st, set, message);
	return m ? rd16(m + 0x22) : 0;
}

int huddef_get_segment(const huddef_strings *st, uint32_t set, uint32_t message, uint32_t k, huddef_segment *out,
                       uint16_t *dst, uint32_t cap)
{
	const uint8_t *m = message_rec(st, set, message), *sg;
	memset(out, 0, sizeof *out);
	if (dst && cap) dst[0] = 0;
	if (!m || k >= rd16(m + 0x22)) return 0;
	sg = st->segments + ((size_t)rd16(m + 0x20) + k) * SEG_REC;
	out->kind = sg[0];
	if (sg[0] == HUDDEF_SEG_ICON) {
		out->icon = sg[1];
		return 1;
	}
	out->length = copy_units(st, rd32(sg + 4), rd16(sg + 2), dst, cap);
	return 1;
}
