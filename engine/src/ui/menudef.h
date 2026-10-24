/* The main menu's data (docs/39-main-menu.md): the MENU section of `ui.menu.hp2` written by tools/halo2ps2/menuconv.py (sprites, string list and font
 * tag ids), next to the pack's FONT / STRS / HUDT sections that huddef.c and world/tex.c read. Pure C11, no heap, no double; menudef_open validates
 * every table once (ranges, the sprite names against the enum order below), the accessors then never read outside the buffer. */
#ifndef UI_MENUDEF_H
#define UI_MENUDEF_H
#include <stddef.h>
#include <stdint.h>

typedef enum { MENUDEF_OK = 0, MENUDEF_ERR_TRUNCATED, MENUDEF_ERR_MAGIC, MENUDEF_ERR_LAYOUT, MENUDEF_ERR_VERSION, MENUDEF_ERR_NAMES } menudef_status;

/* Sprite slots: the order of SPRITES in tools/halo2ps2/menuconv.py (checked by name when the pack opens). */
enum {
	MS_LOGO = 0, MS_CAMPAIGN0, MS_CAMPAIGN1, MS_MULTI0, MS_MULTI1, MS_SETTINGS0, MS_SETTINGS1, MS_DEMOS0, MS_DEMOS1, MS_GRADIENT,
	MS_ITEMTOP0, MS_ITEMTOP1, MS_ITEM0, MS_ITEM1, MS_HDR_DIFF, MS_EMBLEM0, MS_EMBLEM1, MS_EMBLEM2, MS_EMBLEM3, MS_LISTFIELD,
	MS_X_BUTN, MS_O_BUTN, MS_HDR_LEVEL, MS_ALERT, MS_WHITE, MS_ARROW_L, MS_ARROW_R, MS_OPTION0, MS_OPTION1, MS_CARD0, MS_CARD1, MS_IND0, MS_LEVEL0 = MS_IND0 + 4, MS_COUNT = MS_LEVEL0 + 11
};
/* String lists (ustr tags of the Xbox UI) and fonts. */
enum { ML_MAIN_OPTIONS = 0, ML_BUTTON_KEY, ML_DIFF_OPTIONS, ML_DIFF_DESC, ML_WARNING, ML_MAP_LIST, ML_MAP_DATA, ML_SET_LABELS, ML_SET_INVERT, ML_SET_SENS, ML_COUNT };
enum { MF_SMALL = 0, MF_LARGE, MF_COUNT };
/* Button key strings (ML_BUTTON_KEY indices of the Xbox list). */
enum { MK_BACK = 0, MK_SELECT = 1, MK_START_GAME = 10, MK_PRESS_START = 13, MK_ACCEPT = 14 };

#define MENUDEF_VIRT_W 640
#define MENUDEF_VIRT_H 480

typedef struct {
	uint16_t tex, u0, v0, u1, v1;    /* HUDT texture and texel rectangle (u1 / v1 exclusive) */
	uint16_t w, h;                   /* source pixels: the size it is drawn at */
	uint16_t flags;
} menudef_sprite;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       sprite_count, list_count, font_count;
	const uint8_t *sprites, *lists, *fonts;
} menudef_t;

menudef_status menudef_open(menudef_t *m, const uint8_t *data, size_t size);
menudef_sprite menudef_get_sprite(const menudef_t *m, uint32_t i);                 /* zeroed for a bad index */
uint32_t       menudef_list_tag(const menudef_t *m, uint32_t i);                   /* ustr tag id, 0 for a bad index */
uint32_t       menudef_font_tag(const menudef_t *m, uint32_t i);                   /* font tag id, 0 for a bad index */
const char    *menudef_sprite_name(uint32_t i);                                    /* the expected name of slot i, NULL past MS_COUNT */

#endif
