/* Overlay pass: the HUD (ui/hud.c quads) and screen effects, drawn as GS sprites after the world and objects. */
#ifndef RENDER_HUD_RENDER_H
#define RENDER_HUD_RENDER_H
#include "render/render_internal.h"
#include "render/sprite.h"
#include "sim/cine.h"
#include "sim/game.h"
#include "ui/hud.h"
#include "world/tex.h"

typedef struct {
	const huddef_section *hud;
	const huddef_fonts   *fonts;
	const hud_state_t    *state;
	const tex_source_t   *tex;          /* HUDT textures */
	unsigned              key_base;     /* bsp_render_reserve_keys result */
	const game_t         *game;
	const hud_frame_t    *overlay;      /* extra quads drawn over the HUD with the same texture source (the main menu: ui/menu.c); NULL: none */
	const cine_state     *cine;         /* letterbox, fades, titles and messages (NULL: none) */
	uint32_t              tick;
	int                   enabled;
	/* statistics */
	unsigned              st_quads, st_skipped;
	float                 st_ms;
	hud_frame_t           frame;
} hud_render_t;

void hud_render_pass(void *user, struct render_ctx *rc);
/* Claims the HUD's textures (the font, the meters) early in the frame, before the world takes every page of the texture window: the pass itself runs last and found the
 * font atlas without a page for hundreds of frames in the hangar (no help text, no "Checkpoint... done"). */
void hud_render_prefetch(hud_render_t *h);

#endif
