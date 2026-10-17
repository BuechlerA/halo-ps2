/* Phase 3 data packs of a map (docs/14-phase3-plan.md): collision, object collision models, units, weapons, animation state
 * tables, HUD, sound and effects. Every pack is optional: the game runs with whatever is present. */
#ifndef WORLD_GAMEDATA_H
#define WORLD_GAMEDATA_H
#include "core/config.h"
#include "core/mem.h"
#include "ui/huddef.h"
#include "audio/snddef.h"
#include "sim/unitdef.h"
#include "sim/weapdef.h"
#include "world/ansm.h"
#include "world/cmod.h"
#include "world/coll.h"
#include "world/fxdef.h"
#include "world/world.h"

typedef struct {
	coll_bsp        coll;   int has_coll;
	cmod_section    cmod;   int has_cmod;
	unitdef_section unit;   int has_unit;
	weap_section    weap;   int has_weap;
	ansm_section    ansm;   int has_ansm;
	huddef_section  hud;    int has_hud;
	huddef_fonts    fonts;  int has_fonts;
	huddef_strings  strings; int has_strings;
	texset_t        hud_tex;
	tex_source_t    hud_source;
	snd_section     snd;    int has_snd;
	const uint8_t  *sndd;   size_t sndd_size;   /* SNDD, kept in the arena (uploaded sample by sample on demand) */
	fxdef_section   fx;     int has_fx;
	texset_t        fx_tex;
	tex_source_t    fx_source;
} gamedata_t;

/* Loads the packs `<prefix>.<kind>.hp2` (kind: coll cmod unit weap ansm hud snd fx) for BSP `bsp_index`; missing packs are skipped
 * with a message. Returns the number of packs loaded. */
int gamedata_load(gamedata_t *g, arena_t *a, const char *prefix, int bsp_index);

#endif
