/* Host test of the main menu (docs/39-main-menu.md): the MENU / FONT / STRS / HUDT sections of the real `ui.menu.hp2` (skipped without it) open, the menu's screens
 * answer keys the way the Xbox's do (title -> main -> difficulty -> launch, the notice box, the level select), and every quad a screen builds lies on the 640 x 480
 * screen and inside its texture. */
#include "stream/hp2.h"
#include "ui/menu.h"
#include "world/tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint8_t *load_section(const char *path, const char *tag, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;
	uint8_t *img, *raw;
	hp2_pack pk;
	const hp2_section *sec;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	img = malloc((size_t)n);
	if (fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(img); return NULL; }
	fclose(f);
	if (hp2_open(&pk, img, (size_t)n) != HP2_OK || !(sec = hp2_find(&pk, tag))) { free(img); return NULL; }
	raw = malloc(sec->raw_size + 64);
	if (hp2_load(&pk, sec, raw, sec->raw_size) != HP2_OK) { free(img); free(raw); return NULL; }
	*size = sec->raw_size;
	free(img);                                       /* the section is decoded: the pack image is not needed */
	return raw;
}

static menudef_t g_def;
static huddef_fonts g_fonts;
static huddef_strings g_strs;
static tex_section g_tex;

static void check_frame(const menu_t *m, unsigned min_quads)
{
	static hud_frame_t f;
	unsigned i;
	menu_build(m, &f);
	CHECK(f.n >= min_quads);
	CHECK(f.n <= HUD_MAX_QUADS);
	for (i = 0; i < f.n; i++) {
		const hud_quad_t *q = &f.q[i];
		tex_info ti;
		CHECK(q->x1 > q->x0 && q->y1 > q->y0);
		CHECK(q->x0 >= -0.5f && q->y0 >= -0.5f && q->x1 <= 640.5f && q->y1 <= 480.5f);
		CHECK(q->tex < g_tex.texture_count);
		CHECK(q->u1 > q->u0 && q->v1 > q->v0);
		if (q->tex < g_tex.texture_count) { ti = tex_get(&g_tex, q->tex); CHECK(q->u1 <= ti.width && q->v1 <= ti.height); }
	}
}

static void flow(void)
{
	menu_t m;
	int pt, hold, level, diff;
	unsigned ticks, i;
	char t[64];
	menu_init(&m, &g_def, &g_fonts, &g_strs, 0x0001u, 0);
	CHECK(m.screen == MENU_SCREEN_TITLE);
	check_frame(&m, 2);
	menu_key(&m, MENU_KEY_DOWN);
	CHECK(m.screen == MENU_SCREEN_TITLE);                    /* only START / accept leave the title */
	menu_key(&m, MENU_KEY_START);
	CHECK(m.screen == MENU_SCREEN_MAIN && (menu_take_sounds(&m) & MENU_SND_FORWARD));
	CHECK(menu_take_camera(&m, &pt, &ticks, &hold) && pt == MENU_CAM_CAMPAIGN && hold == 1 && ticks == MENU_CAM_MOVE_TICKS);
	CHECK(!menu_take_camera(&m, &pt, &ticks, &hold));        /* one request, taken once */
	check_frame(&m, 5);
	menu_key(&m, MENU_KEY_UP);                               /* wraps to GAME DEMOS */
	CHECK(m.sel_main == 3 && (menu_take_sounds(&m) & MENU_SND_CURSOR));
	menu_key(&m, MENU_KEY_DOWN);
	CHECK(m.sel_main == 0);
	for (i = 1; i < 4; i++) {                                /* every item moves the camera to its own point */
		menu_key(&m, MENU_KEY_DOWN);
		CHECK(menu_take_camera(&m, &pt, &ticks, &hold) && hold == 1);
	}
	menu_key(&m, MENU_KEY_ACCEPT);                           /* GAME DEMOS: a notice box */
	CHECK(m.screen == MENU_SCREEN_NOTICE && (menu_take_sounds(&m) & MENU_SND_FAIL));
	check_frame(&m, 5);
	menu_key(&m, MENU_KEY_BACK);
	CHECK(m.screen == MENU_SCREEN_MAIN);
	menu_key(&m, MENU_KEY_DOWN);                             /* CAMPAIGN */
	menu_key(&m, MENU_KEY_ACCEPT);
	CHECK(m.screen == MENU_SCREEN_DIFFICULTY && m.sel_diff == 1);                 /* one playable level: straight to the difficulty select, NORMAL highlighted */
	(void)menu_take_camera(&m, &pt, &ticks, &hold);
	check_frame(&m, 20);
	menu_key(&m, MENU_KEY_DOWN); menu_key(&m, MENU_KEY_DOWN); menu_key(&m, MENU_KEY_DOWN);
	CHECK(m.sel_diff == 3);
	menu_key(&m, MENU_KEY_UP);
	CHECK(m.sel_diff == 2 && !menu_launch(&m, &level, &diff));
	menu_key(&m, MENU_KEY_BACK);
	CHECK(m.screen == MENU_SCREEN_MAIN);
	menu_key(&m, MENU_KEY_ACCEPT);
	menu_key(&m, MENU_KEY_ACCEPT);
	CHECK(menu_launch(&m, &level, &diff) && level == 0 && diff == 2);
	/* strings */
	CHECK(menu_string(&m, ML_DIFF_OPTIONS, 3, t, sizeof t) == 9 && strcmp(t, "LEGENDARY") == 0);
	CHECK(menu_string(&m, ML_MAIN_OPTIONS, 0, t, sizeof t) > 0);
	CHECK(menu_string(&m, ML_COUNT, 0, t, sizeof t) == 0);
	/* idle: the tour of the ring resumes */
	menu_init(&m, &g_def, &g_fonts, &g_strs, 0x0001u, 1);
	menu_key(&m, MENU_KEY_DOWN);
	(void)menu_take_camera(&m, &pt, &ticks, &hold);
	for (i = 0; i < MENU_IDLE_TICKS + 2; i++) menu_tick(&m);
	CHECK(menu_take_camera(&m, &pt, &ticks, &hold) && pt == -1 && hold == 0);
}

static void levels(void)
{
	menu_t m;
	int level, diff, pt, hold;
	unsigned ticks;
	menu_init(&m, &g_def, &g_fonts, &g_strs, 0x0007u, 1);
	menu_key(&m, MENU_KEY_ACCEPT);                           /* CAMPAIGN with three playable levels: the level select first */
	CHECK(m.screen == MENU_SCREEN_LEVELS);
	check_frame(&m, 30);
	menu_key(&m, MENU_KEY_LEFT);
	CHECK(m.sel_level == 0);
	menu_key(&m, MENU_KEY_RIGHT); menu_key(&m, MENU_KEY_RIGHT);
	CHECK(m.sel_level == 2);
	menu_key(&m, MENU_KEY_RIGHT);                            /* level 3 is locked: it can be looked at, not entered */
	CHECK(m.sel_level == 3);
	(void)menu_take_sounds(&m);
	menu_key(&m, MENU_KEY_ACCEPT);
	CHECK(m.screen == MENU_SCREEN_LEVELS && (menu_take_sounds(&m) & MENU_SND_FAIL));
	check_frame(&m, 20);
	menu_key(&m, MENU_KEY_LEFT);
	menu_key(&m, MENU_KEY_ACCEPT);
	CHECK(m.screen == MENU_SCREEN_DIFFICULTY);
	menu_key(&m, MENU_KEY_BACK);
	CHECK(m.screen == MENU_SCREEN_LEVELS);                   /* back goes to where it came from */
	menu_key(&m, MENU_KEY_ACCEPT);
	menu_key(&m, MENU_KEY_ACCEPT);
	CHECK(menu_launch(&m, &level, &diff) && level == 2 && diff == 1);
	(void)menu_take_camera(&m, &pt, &ticks, &hold);
	for (level = 0; level < 10; level++) {                   /* every level card builds inside the screen */
		m.sel_level = (int8_t)level; m.screen = MENU_SCREEN_LEVELS;
		check_frame(&m, 20);
	}
}

static void bad_data(void)
{
	menudef_t d;
	static uint8_t copy[8192];
	size_t n;
	uint8_t *p = load_section("../../assets/hp2/ui.menu.hp2", "MENU", &n);
	size_t i;
	CHECK(p != NULL && n < sizeof copy);
	if (!p || n >= sizeof copy) return;
	for (i = 0; i < n; i += 7) {                                 /* a flipped byte is rejected or harmless */
		memcpy(copy, p, n);
		copy[i] ^= 0xFF;
		if (menudef_open(&d, copy, n) == MENUDEF_OK) { uint32_t k, acc = 0; for (k = 0; k < d.sprite_count; k++) acc += menudef_get_sprite(&d, k).tex; (void)acc; }
	}
	for (i = 0; i < 64; i++) CHECK(menudef_open(&d, p, i) != MENUDEF_OK);
	CHECK(menudef_open(&d, p, n) == MENUDEF_OK);
	free(p);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "../../assets/hp2/ui.menu.hp2";
	size_t n;
	uint8_t *p;
	uint32_t i;
	if (!(p = load_section(path, "MENU", &n))) { printf("skip: no %s\n", path); return 0; }
	CHECK(menudef_open(&g_def, p, n) == MENUDEF_OK);
	CHECK(g_def.sprite_count == MS_COUNT && g_def.list_count == ML_COUNT && g_def.font_count == MF_COUNT);
	for (i = 0; i < MS_COUNT; i++) { menudef_sprite s = menudef_get_sprite(&g_def, i); CHECK(s.w > 0 && s.h > 0 && s.u1 > s.u0 && s.v1 > s.v0); }
	CHECK(menudef_get_sprite(&g_def, MS_COUNT).w == 0 && menudef_list_tag(&g_def, ML_COUNT) == 0 && menudef_font_tag(&g_def, MF_COUNT) == 0);
	CHECK((p = load_section(path, "FONT", &n)) && huddef_fonts_open(&g_fonts, p, n) == HUDDEF_OK);
	CHECK((p = load_section(path, "STRS", &n)) && huddef_strings_open(&g_strs, p, n) == HUDDEF_OK);
	CHECK((p = load_section(path, "HUDT", &n)) && tex_open(&g_tex, p, n) == TEX_OK);
	CHECK(huddef_check_font_textures(&g_fonts, &g_tex) == HUDDEF_OK);
	for (i = 0; i < MF_COUNT; i++) CHECK(huddef_find_font(&g_fonts, menudef_font_tag(&g_def, i)) >= 0);
	for (i = 0; i < ML_COUNT; i++) CHECK(huddef_find_list(&g_strs, menudef_list_tag(&g_def, i)) >= 0);
	flow();
	levels();
	bad_data();
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
