#include "ui/menudef.h"
#include <string.h>

#define HEADER 64u
#define SPRITE_REC 28u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *const k_names[MS_COUNT] = {
	"logo", "campaign0", "campaign1", "multi0", "multi1", "settings0", "settings1", "demos0", "demos1", "gradient",
	"itemtop0", "itemtop1", "item0", "item1", "hdr_diff", "emblem0", "emblem1", "emblem2", "emblem3", "listfield",
	"x_butn", "o_butn", "hdr_level", "alert", "white", "arrow_l", "arrow_r", "option0", "option1", "card0", "card1", "ind0", "ind1", "ind2", "ind3",
	"level0", "level1", "level2", "level3", "level4", "level5", "level6", "level7", "level8", "level9", "level10"
};

const char *menudef_sprite_name(uint32_t i) { return i < MS_COUNT ? k_names[i] : NULL; }

menudef_status menudef_open(menudef_t *m, const uint8_t *data, size_t size)
{
	uint32_t total, i, so, lo, fo;
	memset(m, 0, sizeof *m);
	if (!data || size < HEADER) return MENUDEF_ERR_TRUNCATED;
	if (memcmp(data, "MENU", 4) != 0) return MENUDEF_ERR_MAGIC;
	if (rd16(data + 4) != 1) return MENUDEF_ERR_VERSION;
	total = rd32(data + 8);
	m->sprite_count = rd32(data + 12); m->list_count = rd32(data + 16); m->font_count = rd32(data + 20);
	so = rd32(data + 24); lo = rd32(data + 28); fo = rd32(data + 32);
	if (total > size || m->sprite_count != MS_COUNT || m->list_count != ML_COUNT || m->font_count != MF_COUNT) { memset(m, 0, sizeof *m); return MENUDEF_ERR_LAYOUT; }
	if (so < HEADER || so + m->sprite_count * SPRITE_REC > total || lo + m->list_count * 8u > total || fo + m->font_count * 8u > total || (so & 3u) || (lo & 3u) || (fo & 3u)) { memset(m, 0, sizeof *m); return MENUDEF_ERR_LAYOUT; }
	for (i = 0; i < m->sprite_count; i++) {
		const uint8_t *r = data + so + i * SPRITE_REC;
		char name[13];
		memcpy(name, r + 16, 12); name[12] = 0;
		if (strcmp(name, k_names[i]) != 0) { memset(m, 0, sizeof *m); return MENUDEF_ERR_NAMES; }
		if (rd16(r + 6) <= rd16(r + 2) || rd16(r + 8) <= rd16(r + 4) || !rd16(r + 10) || !rd16(r + 12)) { memset(m, 0, sizeof *m); return MENUDEF_ERR_LAYOUT; }
	}
	m->data = data; m->size = size;
	m->sprites = data + so; m->lists = data + lo; m->fonts = data + fo;
	return MENUDEF_OK;
}

menudef_sprite menudef_get_sprite(const menudef_t *m, uint32_t i)
{
	menudef_sprite s;
	const uint8_t *r;
	memset(&s, 0, sizeof s);
	if (!m->data || i >= m->sprite_count) return s;
	r = m->sprites + i * SPRITE_REC;
	s.tex = rd16(r); s.u0 = rd16(r + 2); s.v0 = rd16(r + 4); s.u1 = rd16(r + 6); s.v1 = rd16(r + 8); s.w = rd16(r + 10); s.h = rd16(r + 12); s.flags = rd16(r + 14);
	return s;
}

uint32_t menudef_list_tag(const menudef_t *m, uint32_t i) { return m->data && i < m->list_count ? rd32(m->lists + i * 8u) : 0u; }
uint32_t menudef_font_tag(const menudef_t *m, uint32_t i) { return m->data && i < m->font_count ? rd32(m->fonts + i * 8u) : 0u; }
