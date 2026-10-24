/* The main menu (docs/39-main-menu.md): title screen, main menu (CAMPAIGN / MULTIPLAYER / SETTINGS / GAME DEMOS), difficulty select, level select and a notice box,
 * laid out on the Xbox's 640 x 480 screen from the widget positions of ui.map, and the camera moves of the scenario's named camera points that go with them.
 * Pure C11, host-tested (engine/tests/test_menu.c): input in, a quad list (render/hud_render.c draws it) and camera / launch requests out. */
#ifndef UI_MENU_H
#define UI_MENU_H
#include "ui/hud.h"
#include "ui/huddef.h"
#include "ui/menudef.h"
#include "sim/settings.h"

typedef enum { MENU_SCREEN_TITLE = 0, MENU_SCREEN_MAIN, MENU_SCREEN_DIFFICULTY, MENU_SCREEN_LEVELS, MENU_SCREEN_NOTICE, MENU_SCREEN_SETTINGS } menu_screen;

/* Keys the platform layer maps its pad onto. */
#define MENU_KEY_UP 1u
#define MENU_KEY_DOWN 2u
#define MENU_KEY_LEFT 4u
#define MENU_KEY_RIGHT 8u
#define MENU_KEY_ACCEPT 16u
#define MENU_KEY_BACK 32u
#define MENU_KEY_START 64u

/* The scenario's camera points (levels\ui\ui, the order of the camera point table): 0 uicam, 1 load_campaign, 2 settings, 3 multiplayer, 4 new_campaign, 5 .. 14 point_2 .. point_11. */
#define MENU_CAM_HOME 0
#define MENU_CAM_CAMPAIGN 1
#define MENU_CAM_SETTINGS 2
#define MENU_CAM_MULTIPLAYER 3
#define MENU_CAM_NEW_CAMPAIGN 4
#define MENU_CAM_MOVE_TICKS 60u               /* a move to an item's point */
#define MENU_IDLE_TICKS 450u                  /* no key for this long: the scenario's own tour of the ring (ring_loop) runs again */

/* Sounds of the ui scenario: sound\\sfx\\ui\\cursor, forward, back, flag_failure. */
#define MENU_SND_CURSOR 1u
#define MENU_SND_FORWARD 2u
#define MENU_SND_BACK 4u
#define MENU_SND_FAIL 8u
#define MENU_MAIN_ITEMS 4
#define MENU_LEVELS 10
#define MENU_NOTICE_LINES 4
#define MENU_SET_ROWS 3                       /* invert thumbstick, look sensitivity, volume */

typedef struct {
	const menudef_t     *def;
	const huddef_fonts  *fonts;
	const huddef_strings *strs;
	uint8_t  screen, from;               /* current screen; the one a BACK returns to */
	int8_t   sel_main, sel_diff, sel_level, sel_set;
	settings_t set;                      /* the player settings the SETTINGS screen edits (a copy: the host stores it when set_dirty) */
	uint8_t  set_dirty;
	uint8_t  notice;                     /* which notice box */
	uint16_t level_mask;                 /* bit n: campaign level n can be played (its packs exist) */
	uint32_t tick, since_key;
	uint8_t  fade_in;                    /* ticks left of the fade from black at the start */
	/* requests the host takes (menu_take_camera / the launch fields) */
	int8_t   cam_point;                  /* point to move to, -1 none */
	uint8_t  cam_hold;                   /* 1: the menu owns the camera (the script tour is held), 0: the tour runs */
	uint8_t  cam_dirty;
	int8_t   launch_level, launch_diff;  /* >= 0 when a campaign was chosen */
	uint8_t  snd;                        /* MENU_SND_* bits of this tick's keys (menu_take_sounds) */
} menu_t;

/* `level_mask`: playable campaign levels (bit 0 the Pillar of Autumn). `skip_title` starts at the main menu. */
void menu_init(menu_t *m, const menudef_t *def, const huddef_fonts *fonts, const huddef_strings *strs, uint16_t level_mask, int skip_title, const settings_t *set);
void menu_key(menu_t *m, unsigned keys);                 /* edge triggered MENU_KEY_* bits since the last call */
void menu_tick(menu_t *m);                               /* once per 30 Hz tick */
void menu_build(const menu_t *m, hud_frame_t *f);        /* the quads of this tick, 640 x 480 virtual pixels */
/* The camera move the menu wants now: returns 1 and fills *point / *ticks / *hold once per request. */
int  menu_take_camera(menu_t *m, int *point, unsigned *ticks, int *hold);
/* 1 (once per change) and fills *out when the SETTINGS screen changed a value: the host applies and stores it. */
int  menu_take_settings(menu_t *m, settings_t *out);
/* The MENU_SND_* bits the last menu_key calls produced; clears them. */
unsigned menu_take_sounds(menu_t *m);
/* 1 when a level and difficulty were chosen (and fills them); the host starts the game. */
int  menu_launch(const menu_t *m, int *level, int *difficulty);
/* The widget text (a string list entry as ASCII) for tests and the builder; returns the length. */
unsigned menu_string(const menu_t *m, unsigned list, unsigned index, char *out, unsigned cap);

#endif
