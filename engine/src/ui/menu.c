#include "ui/menu.h"
#include <string.h>

/* ---- layout, read off the Xbox widget tags of ui.map (positions are absolute on the 640 x 480 screen; docs/39-main-menu.md) ---------------------------------- */
#define LOGO_X 0
#define LOGO_Y 28
static const struct { uint8_t s0, s1; int16_t x, y; int8_t cam; } k_items[MENU_MAIN_ITEMS] = {
	{ MS_CAMPAIGN0, MS_CAMPAIGN1, 194, 247, MENU_CAM_CAMPAIGN },
	{ MS_MULTI0, MS_MULTI1, 192, 283, MENU_CAM_MULTIPLAYER },
	{ MS_SETTINGS0, MS_SETTINGS1, 192, 319, MENU_CAM_SETTINGS },
	{ MS_DEMOS0, MS_DEMOS1, 192, 355, MENU_CAM_HOME },
};
#define DIFF_ITEM_X 51
#define DIFF_ITEM_Y 78
#define DIFF_ITEM_PITCH 33
#define DIFF_ITEM_W 232
#define DIFF_PANEL_X 275
#define DIFF_PANEL_Y 78
#define DIFF_EMBLEM_X 333
#define DIFF_EMBLEM_Y 88
#define DIFF_TEXT_X 285
#define DIFF_TEXT_Y 292
#define HEADER_X 35
#define HEADER_Y 11
#define KEY_Y 414
#define KEY_B_X 371
#define KEY_A_X 471
static const int16_t k_card_x[3] = { 72, 239, 406 };
#define CARD_Y 75
#define CARD_PIC_DX 13
#define CARD_PIC_Y 87
#define CARD_PIC_W 140
#define CARD_PIC_H 114
#define CARD_NAME_Y 204
#define CARD_DATA_Y 258
#define CARD_TEXT_DX 11
#define CARD_TEXT_W 144

#define COL_FULL 0x80808080u
#define COL_SELECTED 0x80808080u
#define COL_DIM 0x80684c30u          /* R low byte: a blue-grey for the unselected rows */
#define COL_BLUE 0x80d09050u
#define COL_ALERT_TEXT 0x80f0e0c0u

static const char *const k_notice[3][MENU_NOTICE_LINES] = {
	{ "MULTIPLAYER IS NOT", "AVAILABLE IN THIS", "VERSION.", "" },
	{ "SETTINGS ARE NOT", "AVAILABLE IN THIS", "VERSION.", "" },
	{ "NO GAME DEMOS ARE", "ON THIS DISC.", "", "" },
};

/* ---- helpers ------------------------------------------------------------------------------------------------------------------------------------------- */
static int in_range(const menu_t *m) { return m->def && m->def->data; }

unsigned menu_string(const menu_t *m, unsigned list, unsigned index, char *out, unsigned cap)
{
	uint16_t buf[192];
	uint32_t n, i;
	int li;
	if (!cap) return 0;
	out[0] = 0;
	if (!m->strs || !in_range(m)) return 0;
	li = huddef_find_list(m->strs, menudef_list_tag(m->def, list));
	if (li < 0) return 0;
	n = huddef_get_string(m->strs, (uint32_t)li, index, buf, 191);
	if (n == HUDDEF_STR_NONE) return 0;
	for (i = 0; i < n && i + 1 < cap; i++) out[i] = buf[i] < 256 ? (char)buf[i] : '?';
	out[i] = 0;
	return i;
}

static void sprite_part(hud_frame_t *f, const menudef_t *d, unsigned s, float x, float y, float w, float h, uint32_t rgba)
{
	menudef_sprite sp = menudef_get_sprite(d, s);
	hud_quad_t *q;
	float ku, kv;
	if (!sp.w || !sp.h || f->n >= HUD_MAX_QUADS) return;
	if (x >= (float)MENUDEF_VIRT_W || y >= (float)MENUDEF_VIRT_H) return;
	if (w > (float)sp.w) w = (float)sp.w;
	if (h > (float)sp.h) h = (float)sp.h;
	if (x + w > (float)MENUDEF_VIRT_W) w = (float)MENUDEF_VIRT_W - x;            /* the logo is a 1024 wide bitmap on a 640 wide screen: its right part is empty and off screen */
	if (y + h > (float)MENUDEF_VIRT_H) h = (float)MENUDEF_VIRT_H - y;
	ku = (float)(sp.u1 - sp.u0) / (float)sp.w; kv = (float)(sp.v1 - sp.v0) / (float)sp.h;
	q = &f->q[f->n++];
	q->x0 = x; q->y0 = y; q->x1 = x + w; q->y1 = y + h;
	q->tex = sp.tex; q->u0 = sp.u0; q->v0 = sp.v0;
	q->u1 = (uint16_t)(sp.u0 + (uint16_t)(w * ku + 0.5f)); q->v1 = (uint16_t)(sp.v0 + (uint16_t)(h * kv + 0.5f));
	if (q->u1 <= q->u0) q->u1 = (uint16_t)(q->u0 + 1u);
	if (q->v1 <= q->v0) q->v1 = (uint16_t)(q->v0 + 1u);
	q->rgba = rgba; q->blend = HUDDEF_BLEND_ALPHA;
}

static void sprite(hud_frame_t *f, const menudef_t *d, unsigned s, float x, float y, uint32_t rgba)
{
	menudef_sprite sp = menudef_get_sprite(d, s);
	sprite_part(f, d, s, x, y, (float)sp.w, (float)sp.h, rgba);
}

static void sprite_stretch(hud_frame_t *f, const menudef_t *d, unsigned s, float x0, float y0, float x1, float y1, uint32_t rgba)
{
	menudef_sprite sp = menudef_get_sprite(d, s);
	hud_quad_t *q;
	if (!sp.w || f->n >= HUD_MAX_QUADS) return;
	q = &f->q[f->n++];
	q->x0 = x0; q->y0 = y0; q->x1 = x1; q->y1 = y1;
	q->tex = sp.tex; q->u0 = sp.u0; q->v0 = sp.v0; q->u1 = sp.u1; q->v1 = sp.v1; q->rgba = rgba; q->blend = HUDDEF_BLEND_ALPHA;
}

static unsigned text_width(const huddef_fonts *fonts, uint32_t font_tag, const char *s, unsigned n)
{
	uint16_t u[96];
	unsigned i;
	int fi;
	if (!fonts || !fonts->font_count) return 0;
	for (i = 0; i < n && i < 95; i++) u[i] = (uint8_t)s[i];
	fi = huddef_find_font(fonts, font_tag);
	return (unsigned)huddef_text_width(fonts, (uint32_t)(fi < 0 ? 0 : fi), u, i);
}

/* Lines of `text` (split at "\r\n" / "\n"), left aligned at x or centred in [x, x + w) when w > 0; returns the number of lines. */
static unsigned text_block(hud_frame_t *f, const huddef_fonts *fonts, uint32_t font_tag, float x, float y, float w, const char *text, uint32_t rgba)
{
	huddef_font fd;
	int fi;
	float lh;
	unsigned lines = 0;
	if (!fonts || !fonts->font_count) return 0;
	fi = huddef_find_font(fonts, font_tag);
	fd = huddef_get_font(fonts, (uint32_t)(fi < 0 ? 0 : fi));
	lh = (float)(fd.ascent + fd.descent + fd.leading_h);
	while (*text) {
		char line[96];
		unsigned n = 0;
		while (*text && *text != '\n' && *text != '\r') { if (n < sizeof line - 1) line[n++] = *text; text++; }
		line[n] = 0;
		while (*text == '\r' || *text == '\n') text++;
		hud_text(f, fonts, font_tag, w > 0.0f ? x + (w - (float)text_width(fonts, font_tag, line, n)) * 0.5f : x, y + lh * (float)lines, line, rgba);
		lines++;
	}
	return lines;
}

static void button_key(hud_frame_t *f, const menu_t *m, int accept_text)
{
	char t[40];
	huddef_font fd;
	int fi = m->fonts && m->fonts->font_count ? huddef_find_font(m->fonts, menudef_font_tag(m->def, MF_LARGE)) : -1;
	float ty = (float)KEY_Y;
	if (fi >= 0) { fd = huddef_get_font(m->fonts, (uint32_t)fi); ty += (26.0f - (float)(fd.ascent + fd.descent)) * 0.5f; }
	sprite(f, m->def, MS_B_BUTN, (float)KEY_B_X, (float)KEY_Y, COL_FULL);
	menu_string(m, ML_BUTTON_KEY, MK_BACK, t, sizeof t);
	hud_text(f, m->fonts, menudef_font_tag(m->def, MF_LARGE), (float)KEY_B_X + 21.0f, ty, t[0] == '=' ? t + 1 : t, COL_BLUE);
	sprite(f, m->def, MS_A_BUTN, (float)KEY_A_X, (float)KEY_Y, COL_FULL);
	menu_string(m, ML_BUTTON_KEY, accept_text, t, sizeof t);
	hud_text(f, m->fonts, menudef_font_tag(m->def, MF_LARGE), (float)KEY_A_X + 21.0f, ty, t[0] == '=' ? t + 1 : t, COL_BLUE);
}

/* ---- state ------------------------------------------------------------------------------------------------------------------------------------------- */
static void want_camera(menu_t *m, int point, int hold)
{
	m->cam_point = (int8_t)point; m->cam_hold = (uint8_t)hold; m->cam_dirty = 1;
}

static unsigned level_count_playable(const menu_t *m)
{
	unsigned n = 0, i;
	for (i = 0; i < MENU_LEVELS; i++) n += (m->level_mask >> i) & 1u;
	return n;
}

void menu_init(menu_t *m, const menudef_t *def, const huddef_fonts *fonts, const huddef_strings *strs, uint16_t level_mask, int skip_title)
{
	memset(m, 0, sizeof *m);
	m->def = def; m->fonts = fonts; m->strs = strs; m->level_mask = level_mask;
	m->screen = skip_title ? MENU_SCREEN_MAIN : MENU_SCREEN_TITLE;
	m->launch_level = m->launch_diff = -1;
	m->cam_point = -1; m->fade_in = 30;
	m->sel_diff = 1;                                           /* NORMAL is the default highlight */
	m->sel_level = 0;
}

static void enter_main(menu_t *m)
{
	m->snd |= MENU_SND_BACK;
	m->screen = MENU_SCREEN_MAIN;
	want_camera(m, k_items[(int)m->sel_main].cam, 1);
}

void menu_key(menu_t *m, unsigned keys)
{
	if (!keys) return;
	m->since_key = 0;
	switch (m->screen) {
	case MENU_SCREEN_TITLE:
		if (keys & (MENU_KEY_START | MENU_KEY_ACCEPT)) { enter_main(m); m->snd = MENU_SND_FORWARD; }
		break;
	case MENU_SCREEN_MAIN:
		if (keys & MENU_KEY_UP) { m->snd |= MENU_SND_CURSOR; m->sel_main = (int8_t)((m->sel_main + MENU_MAIN_ITEMS - 1) % MENU_MAIN_ITEMS); want_camera(m, k_items[(int)m->sel_main].cam, 1); }
		else if (keys & MENU_KEY_DOWN) { m->snd |= MENU_SND_CURSOR; m->sel_main = (int8_t)((m->sel_main + 1) % MENU_MAIN_ITEMS); want_camera(m, k_items[(int)m->sel_main].cam, 1); }
		else if (keys & (MENU_KEY_ACCEPT | MENU_KEY_START)) {
			m->snd |= MENU_SND_FORWARD;
			if (m->sel_main == 0) {
				m->from = MENU_SCREEN_MAIN;
				if (level_count_playable(m) > 1u) { m->screen = MENU_SCREEN_LEVELS; want_camera(m, MENU_CAM_NEW_CAMPAIGN, 1); }
				else { m->sel_level = 0; m->screen = MENU_SCREEN_DIFFICULTY; want_camera(m, MENU_CAM_NEW_CAMPAIGN, 1); }
			} else { m->snd = MENU_SND_FAIL; m->from = MENU_SCREEN_MAIN; m->notice = (uint8_t)(m->sel_main - 1); m->screen = MENU_SCREEN_NOTICE; }
		} else if (keys & MENU_KEY_BACK) { m->snd |= MENU_SND_BACK; m->screen = MENU_SCREEN_TITLE; want_camera(m, -1, 0); }
		break;
	case MENU_SCREEN_LEVELS:
		if (keys & MENU_KEY_LEFT) { if (m->sel_level > 0) { m->sel_level--; m->snd |= MENU_SND_CURSOR; } }
		else if (keys & MENU_KEY_RIGHT) { if (m->sel_level < MENU_LEVELS - 1) { m->sel_level++; m->snd |= MENU_SND_CURSOR; } }
		else if (keys & (MENU_KEY_ACCEPT | MENU_KEY_START)) { if ((m->level_mask >> m->sel_level) & 1u) { m->snd |= MENU_SND_FORWARD; m->from = MENU_SCREEN_LEVELS; m->screen = MENU_SCREEN_DIFFICULTY; } else m->snd |= MENU_SND_FAIL; }
		else if (keys & MENU_KEY_BACK) { enter_main(m); }
		break;
	case MENU_SCREEN_DIFFICULTY:
		if (keys & MENU_KEY_UP) { if (m->sel_diff > 0) { m->sel_diff--; m->snd |= MENU_SND_CURSOR; } }
		else if (keys & MENU_KEY_DOWN) { if (m->sel_diff < 3) { m->sel_diff++; m->snd |= MENU_SND_CURSOR; } }
		else if (keys & (MENU_KEY_ACCEPT | MENU_KEY_START)) { m->launch_level = m->sel_level; m->launch_diff = m->sel_diff; m->snd |= MENU_SND_FORWARD; }
		else if (keys & MENU_KEY_BACK) { if (m->from == MENU_SCREEN_LEVELS) { m->screen = MENU_SCREEN_LEVELS; m->snd |= MENU_SND_BACK; } else enter_main(m); }
		break;
	case MENU_SCREEN_NOTICE:
		if (keys & (MENU_KEY_ACCEPT | MENU_KEY_BACK | MENU_KEY_START)) { m->screen = MENU_SCREEN_MAIN; m->snd |= MENU_SND_BACK; }
		break;
	default: break;
	}
}

void menu_tick(menu_t *m)
{
	m->tick++;
	if (m->fade_in) m->fade_in--;
	if (m->since_key < 0x7FFFFFFFu) m->since_key++;
	if (m->since_key == MENU_IDLE_TICKS && m->cam_hold && m->screen == MENU_SCREEN_MAIN) want_camera(m, -1, 0);       /* the ring tour resumes */
}

int menu_take_camera(menu_t *m, int *point, unsigned *ticks, int *hold)
{
	if (!m->cam_dirty) return 0;
	m->cam_dirty = 0;
	*point = m->cam_point; *ticks = MENU_CAM_MOVE_TICKS; *hold = m->cam_hold;
	return 1;
}

unsigned menu_take_sounds(menu_t *m) { unsigned s = m->snd; m->snd = 0; return s; }

int menu_launch(const menu_t *m, int *level, int *difficulty)
{
	if (m->launch_level < 0 || m->launch_diff < 0) return 0;
	*level = m->launch_level; *difficulty = m->launch_diff;
	return 1;
}

/* ---- drawing ------------------------------------------------------------------------------------------------------------------------------------------- */
static void build_title(const menu_t *m, hud_frame_t *f)
{
	char t[48];
	sprite(f, m->def, MS_LOGO, (float)LOGO_X, (float)LOGO_Y, COL_FULL);
	if ((m->tick / 20u) & 1u) return;                                       /* blinks */
	menu_string(m, ML_BUTTON_KEY, MK_PRESS_START, t, sizeof t);
	text_block(f, m->fonts, menudef_font_tag(m->def, MF_LARGE), 0.0f, 360.0f, (float)MENUDEF_VIRT_W, t, COL_FULL);
}

static void build_main(const menu_t *m, hud_frame_t *f)
{
	unsigned i;
	sprite(f, m->def, MS_LOGO, (float)LOGO_X, (float)LOGO_Y, COL_FULL);
	for (i = 0; i < MENU_MAIN_ITEMS; i++)
		sprite(f, m->def, i == (unsigned)m->sel_main ? k_items[i].s1 : k_items[i].s0, (float)k_items[i].x, (float)k_items[i].y, COL_FULL);
}

static void build_difficulty(const menu_t *m, hud_frame_t *f)
{
	unsigned i;
	char t[200];
	huddef_font fd;
	int fi = m->fonts && m->fonts->font_count ? huddef_find_font(m->fonts, menudef_font_tag(m->def, MF_LARGE)) : -1;
	sprite_stretch(f, m->def, MS_GRADIENT, 0.0f, 0.0f, (float)MENUDEF_VIRT_W, (float)MENUDEF_VIRT_H, COL_FULL);
	sprite(f, m->def, MS_HDR_DIFF, (float)HEADER_X, (float)HEADER_Y, COL_FULL);
	for (i = 0; i < 4; i++) {
		float y = (float)(DIFF_ITEM_Y + (int)i * DIFF_ITEM_PITCH), ty = y;
		int sel = (int)i == m->sel_diff;
		sprite(f, m->def, i == 0 ? (sel ? MS_ITEMTOP1 : MS_ITEMTOP0) : (sel ? MS_ITEM1 : MS_ITEM0), (float)DIFF_ITEM_X, y, COL_FULL);
		if (fi >= 0) { fd = huddef_get_font(m->fonts, (uint32_t)fi); ty += (32.0f - (float)(fd.ascent + fd.descent)) * 0.5f; }
		menu_string(m, ML_DIFF_OPTIONS, i, t, sizeof t);
		hud_text(f, m->fonts, menudef_font_tag(m->def, MF_LARGE), (float)DIFF_ITEM_X + 12.0f, ty, t, sel ? COL_SELECTED : COL_DIM);
	}
	sprite(f, m->def, MS_LISTFIELD, (float)DIFF_PANEL_X, (float)DIFF_PANEL_Y, COL_FULL);
	sprite(f, m->def, MS_EMBLEM0 + (unsigned)m->sel_diff, (float)DIFF_EMBLEM_X, (float)DIFF_EMBLEM_Y, COL_FULL);
	menu_string(m, ML_DIFF_DESC, (unsigned)m->sel_diff, t, sizeof t);
	text_block(f, m->fonts, menudef_font_tag(m->def, MF_SMALL), (float)DIFF_TEXT_X, (float)DIFF_TEXT_Y, 0.0f, t, COL_FULL);
	button_key(f, m, MK_ACCEPT);
}

static void build_levels(const menu_t *m, hud_frame_t *f)
{
	int slot;
	char t[200];
	sprite_stretch(f, m->def, MS_GRADIENT, 0.0f, 0.0f, (float)MENUDEF_VIRT_W, (float)MENUDEF_VIRT_H, COL_FULL);
	sprite(f, m->def, MS_HDR_LEVEL, (float)HEADER_X, (float)HEADER_Y, COL_FULL);
	for (slot = 0; slot < 3; slot++) {
		int li = m->sel_level + slot - 1;
		int open;
		float x = (float)k_card_x[slot];
		if (li < 0 || li >= MENU_LEVELS) continue;
		open = (m->level_mask >> li) & 1;
		sprite(f, m->def, slot == 1 ? MS_CARD1 : MS_CARD0, x, (float)CARD_Y, COL_FULL);
		sprite_part(f, m->def, MS_LEVEL0 + (unsigned)(open ? li : 10), x + (float)CARD_PIC_DX, (float)CARD_PIC_Y, (float)CARD_PIC_W, (float)CARD_PIC_H, slot == 1 ? COL_FULL : 0x80585858u);
		menu_string(m, ML_MAP_LIST, (unsigned)(open ? li : 10), t, sizeof t);
		text_block(f, m->fonts, menudef_font_tag(m->def, MF_LARGE), x + (float)CARD_TEXT_DX, (float)CARD_NAME_Y, (float)CARD_TEXT_W, t, slot == 1 ? COL_SELECTED : COL_DIM);
		menu_string(m, ML_MAP_DATA, (unsigned)(open ? li : 10), t, sizeof t);
		text_block(f, m->fonts, menudef_font_tag(m->def, MF_SMALL), x + (float)CARD_TEXT_DX, (float)CARD_DATA_Y, 0.0f, t, slot == 1 ? COL_FULL : COL_DIM);
	}
	button_key(f, m, MK_ACCEPT);
}

static void build_notice(const menu_t *m, hud_frame_t *f)
{
	unsigned i;
	sprite(f, m->def, MS_LOGO, (float)LOGO_X, (float)LOGO_Y, COL_FULL);
	sprite(f, m->def, MS_ALERT, 192.0f, 112.0f, COL_FULL);
	for (i = 0; i < MENU_NOTICE_LINES; i++)
		if (k_notice[m->notice % 3u][i][0]) text_block(f, m->fonts, menudef_font_tag(m->def, MF_LARGE), 192.0f, 172.0f + 26.0f * (float)i, 256.0f, k_notice[m->notice % 3u][i], COL_ALERT_TEXT);
}

void menu_build(const menu_t *m, hud_frame_t *f)
{
	memset(f, 0, sizeof *f);
	if (!in_range(m)) return;
	switch (m->screen) {
	case MENU_SCREEN_TITLE: build_title(m, f); break;
	case MENU_SCREEN_MAIN: build_main(m, f); break;
	case MENU_SCREEN_DIFFICULTY: build_difficulty(m, f); break;
	case MENU_SCREEN_LEVELS: build_levels(m, f); break;
	case MENU_SCREEN_NOTICE: build_notice(m, f); break;
	default: break;
	}
}
