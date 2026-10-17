#include "world/gamedata.h"
#include "stream/file.h"
#include <stdio.h>
#include <string.h>

static uint8_t *load(arena_t *a, const char *prefix, const char *kind, const char *tag, size_t *size, int optional)
{
	char path[160];
	snprintf(path, sizeof path, "%s.%s.hp2", prefix, kind);
	if (file_size_of(path) < 0) return NULL;
	return world_load_section(a, path, tag, size, optional);
}

int gamedata_load(gamedata_t *g, arena_t *a, const char *prefix, int bsp_index)
{
	size_t n;
	uint8_t *p;
	char tag[8];
	int loaded = 0;
	memset(g, 0, sizeof *g);

	snprintf(tag, sizeof tag, "COL%c", bsp_index < 10 ? '0' + bsp_index : 'A' + (bsp_index - 10));
	if ((p = load(a, prefix, "coll", tag, &n, 1)) != NULL) {
		if (coll_open(p, n, &g->coll) == COLL_OK) { g->has_coll = 1; loaded++; printf("[game] collision %s: %u surfaces, %u KB\n", tag, (unsigned)g->coll.surface_count, (unsigned)(n / 1024)); }
		else printf("[err] %s: bad collision section\n", tag);
	}
	if ((p = load(a, prefix, "cmod", "CMOD", &n, 1)) != NULL) {
		if (cmod_open(&g->cmod, p, n) == CMOD_OK) { g->has_cmod = 1; loaded++; printf("[game] object collision: %u KB\n", (unsigned)(n / 1024)); }
		else printf("[err] bad CMOD\n");
	}
	if ((p = load(a, prefix, "unit", "UNIT", &n, 1)) != NULL) {
		if (unitdef_open(&g->unit, p, n) == UNITDEF_OK) { g->has_unit = 1; loaded++; printf("[game] units: %u bipeds, %u vehicles, %u KB\n", (unsigned)g->unit.biped_count, (unsigned)g->unit.vehicle_count, (unsigned)(n / 1024)); }
		else printf("[err] bad UNIT\n");
	}
	if ((p = load(a, prefix, "weap", "WEAP", &n, 1)) != NULL) {
		if (weap_open(&g->weap, p, n) == WEAP_OK) { g->has_weap = 1; loaded++; printf("[game] weapons: %u weapons, %u projectiles, %u damage effects, %u KB\n", (unsigned)g->weap.n_weapons, (unsigned)g->weap.n_projectiles, (unsigned)g->weap.n_damages, (unsigned)(n / 1024)); }
		else printf("[err] bad WEAP\n");
	}
	if ((p = load(a, prefix, "ansm", "ASTB", &n, 1)) != NULL) {
		if (ansm_open(&g->ansm, p, n) == ANSM_OK) { g->has_ansm = 1; loaded++; printf("[game] animation state tables: %u KB\n", (unsigned)(n / 1024)); }
		else printf("[err] bad ASTB\n");
	}
	if ((p = load(a, prefix, "hud", "HUD0", &n, 1)) != NULL) {
		if (huddef_open(&g->hud, p, n) == HUDDEF_OK) {
			g->has_hud = 1; loaded++;
			if ((p = load(a, prefix, "hud", "FONT", &n, 1)) != NULL && huddef_fonts_open(&g->fonts, p, n) == HUDDEF_OK) g->has_fonts = 1;
			if ((p = load(a, prefix, "hud", "STRS", &n, 1)) != NULL && huddef_strings_open(&g->strings, p, n) == HUDDEF_OK) g->has_strings = 1;
			if ((p = load(a, prefix, "hud", "HUDT", &n, 1)) != NULL && tex_open(&g->hud_tex.tex, p, n) == TEX_OK) g->hud_tex.has = 1;
			world_texset_source(&g->hud_source, &g->hud_tex);
			printf("[game] hud: fonts %d strings %d textures %u\n", g->has_fonts, g->has_strings, g->hud_tex.has ? (unsigned)g->hud_tex.tex.texture_count : 0u);
		} else printf("[err] bad HUD0\n");
	}
	if ((p = load(a, prefix, "snd", "SND0", &n, 1)) != NULL) {
		size_t dn = 0;
		const uint8_t *d = load(a, prefix, "snd", "SNDD", &dn, 1);
		if (d && snd_open(&g->snd, p, n, (uint32_t)dn) == SND_OK) { g->has_snd = 1; g->sndd = d; g->sndd_size = dn; loaded++; printf("[game] sound: %u sounds, %u KB of ADPCM\n", (unsigned)g->snd.sound_count, (unsigned)(dn / 1024)); }
		else printf("[err] bad SND0/SNDD\n");
	}
	if ((p = load(a, prefix, "fx", "FXD0", &n, 1)) != NULL) {
		if (fxdef_open(&g->fx, p, n) == FXD_OK) {
			g->has_fx = 1; loaded++;
			if ((p = load(a, prefix, "fx", "FXTX", &n, 1)) != NULL && tex_open(&g->fx_tex.tex, p, n) == TEX_OK) g->fx_tex.has = 1;
			world_texset_source(&g->fx_source, &g->fx_tex);
			printf("[game] effects: textures %u\n", g->fx_tex.has ? (unsigned)g->fx_tex.tex.texture_count : 0u);
		} else printf("[err] bad FXD0\n");
	}
	return loaded;
}
