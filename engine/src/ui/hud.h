/* HUD layout for the current game state: turns the HUD0 records (unit HUD of the player's biped, weapon HUD of the held weapon with its
 * child chain, grenade HUD) plus the simulation state (shield, health, ammo, heat, zoom, grenades) into a list of textured quads on the
 * 640x480 virtual screen. Pure C11, host-tested; render/hud_render.c draws the list. The meter fill rule, flash rules and digit
 * placement are the engine's reading of the tag fields (docs/20, "unverified"). */
#ifndef UI_HUD_H
#define UI_HUD_H
#include "sim/game.h"
#include "ui/huddef.h"

#define HUD_MAX_QUADS 448                 /* the menu's level select writes about 400 glyphs and sprites (three cards of name and description) */

typedef struct {
	float    x0, y0, x1, y1;          /* virtual pixels */
	uint16_t tex, u0, v0, u1, v1;     /* HUDT texture and texel rectangle */
	uint32_t rgba;                    /* GS scale, R in the low byte */
	uint8_t  blend;                   /* HUDDEF_BLEND_* */
} hud_quad_t;

typedef struct {
	hud_quad_t q[HUD_MAX_QUADS];
	unsigned   n;
	unsigned   elements, meters, digits, blips;   /* statistics */
} hud_frame_t;

/* motion tracker: blips for every living unit (biped or vehicle, not the player's body) within the tracker range of the
 * player, laid out around the radar centre with the player's facing up; friendly units (teams player / human) yellow,
 * hostile ones (covenant / flood / sentinel) red, other teams grey. All units in range show (the Xbox hides still
 * enemies: not modelled, docs/20). */
#define HUD_BLIP_SIZE 7.0f                   /* virtual pixels (the Xbox blip is about 6 px wide on 640 x 480) */
#define HUD_BLIP_VEHICLE_SIZE 10.0f
#define HUD_BLIP_FRIENDLY 0x80507A80u        /* GS RGBA, R low byte: pale yellow */
#define HUD_BLIP_HOSTILE 0x80302880u         /* red */
#define HUD_BLIP_NEUTRAL 0x80606060u
int hud_team_hostile(unsigned team);         /* 1 for the teams hostile to the player, 0 otherwise */
int hud_team_friendly(unsigned team);

#define HUD_MSG_COUNT 3
#define HUD_MSG_LEN 80
typedef struct {
	char     text[HUD_MSG_COUNT][HUD_MSG_LEN];
	uint16_t ticks[HUD_MSG_COUNT];
	int      paused;
	uint8_t  pause_sel, can_quit;            /* the pause menu: 0 RESUME, 1 QUIT TO MAIN MENU (offered when can_quit) */
	uint32_t hidden;                         /* HUD_HIDE_* bits a script set (show_hud, hud_show_crosshair / health / shield / motion_sensor false) */
} hud_state_t;
#define HUD_HIDE_ALL 1u
#define HUD_HIDE_CROSSHAIR 2u
#define HUD_HIDE_HEALTH 4u
#define HUD_HIDE_SHIELD 8u
#define HUD_HIDE_MOTION 16u

void hud_state_init(hud_state_t *s);
/* Shows a message for `ticks` ticks (newest at the bottom); oldest is replaced when all slots are busy. */
void hud_message(hud_state_t *s, const char *text, unsigned ticks);
void hud_tick(hud_state_t *s);
/* ASCII text with the HUD font (`fonts` may be NULL: nothing is drawn); returns the pixel width. */
int  hud_text(hud_frame_t *f, const huddef_fonts *fonts, uint32_t font_tag, float x, float y, const char *text, uint32_t rgba);

/* `tick` drives flashing. `fonts` / `st` may be NULL (no messages, no prompt). */
void hud_build(hud_frame_t *f, const huddef_section *hud, const huddef_fonts *fonts, const game_t *g, uint32_t tick, const hud_state_t *st);

#endif
