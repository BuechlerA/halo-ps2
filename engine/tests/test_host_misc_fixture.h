/* A mission_t built from the real a10 packs for the H2 host tests (script host: camera, sound, hud, flow). Header only, static data.
 * Needs: assets/hp2/a10.{obj,coll,cmod,unit,weap,ansm,fx,hud,dev,ai,script,snd,strm}.hp2 and the CINE pack the Makefile converts into
 * $(HOST_TMP)/halo-cine-a10.hp2. mfx_load() returns 0 when something is missing (the test then skips). The units module (ai/units.c, workstream U)
 * is not needed: mission.units stays NULL; weak stubs satisfy the linker. */
#ifndef TEST_HOST_MISC_FIXTURE_H
#define TEST_HOST_MISC_FIXTURE_H
#include "ai/aidef.h"
#include "audio/snddef.h"
#include "audio/strmdef.h"
#include "script/hs.h"
#include "sim/devices.h"
#include "sim/fx.h"
#include "sim/game.h"
#include "sim/mission.h"
#include "stream/hp2.h"
#include "ui/hud.h"
#include "ui/huddef.h"
#include "world/anim.h"
#include "world/cinedef.h"
#include "world/devdef.h"
#include "world/model.h"
#include "world/scen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HM_WITH_OBJ
/* opt-in integration run (-DHM_WITH_OBJ -Wno-unused-function, see docs/34): H1's built-ins next to ours, with H1's stand-in unit layer */
#include "mission_fixture.h"
#else
/* the units module is a separate workstream; the mission's object helpers reference these, the fixture never attaches units */
__attribute__((weak)) int  units_attach(units_t *s, game_t *g, int obj, int team, int32_t weapon_tag) { (void)s; (void)g; (void)obj; (void)team; (void)weapon_tag; return -1; }
__attribute__((weak)) void units_detach(units_t *s, int unit) { (void)s; (void)unit; }
__attribute__((weak)) int  units_of_object(const units_t *s, int obj) { (void)s; (void)obj; return -1; }
__attribute__((weak)) void units_tick(units_t *s, game_t *g) { (void)s; (void)g; }

#endif

typedef struct {
	model_section models; scen_section scen; anim_section anims;
	coll_bsp coll; cmod_section cmod; unitdef_section unit; weap_section weap; ansm_section ansm; fxdef_section fx;
	huddef_section hud; huddef_strings strs;
	devdef_section dev; cinedef_section cin; aidef_section ai; hs_section scr; snd_section snd; strm_section strm;
	uint8_t *strm_bytes; size_t strm_size; uint32_t strd_size, strd_offset;
	object_table_t objects;
	units_t units;
	fpview_t fp;
	game_t game;
	devices_t devices;
	cine_state cine;
	hud_state_t hudst;
	hs_machine hs;
	mission_t m;
	game_init_t gi;
	char strm_path[300];
} mfx_t;

static uint8_t *mfx_section(const char *path, const char *tag, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;
	uint8_t *img, *raw;
	static hp2_pack pk;
	const hp2_section *sec;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	img = malloc((size_t)n);
	if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(img); return NULL; }
	fclose(f);
	if (hp2_open(&pk, img, (size_t)n) != HP2_OK) { free(img); return NULL; }
	sec = hp2_find(&pk, tag);
	if (!sec) { free(img); return NULL; }
	raw = malloc(sec->raw_size + 64);
	if (!raw || hp2_load(&pk, sec, raw, sec->raw_size) != HP2_OK) { free(img); free(raw); return NULL; }
	*size = sec->raw_size;
	free(img);
	return raw;
}

/* STRM index + sizes of the stream pack (the stream data itself stays in the file, played by a file backed stream_io) */
static int mfx_strm(mfx_t *x, const char *path)
{
	uint8_t hdr[64 + 32 * HP2_MAX_SECTIONS];
	hp2_pack pk;
	const hp2_section *sm, *sd;
	size_t n, fsize;
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END); fsize = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	n = fsize < sizeof hdr ? fsize : sizeof hdr;
	if (fread(hdr, 1, n, f) != n || hp2_open_header(&pk, hdr, n, fsize) != HP2_OK) { fclose(f); return 0; }
	sm = hp2_find(&pk, "STRM"); sd = hp2_find(&pk, "STRD");
	if (!sm || !sd || sm->codec != HP2_CODEC_STORED || sd->codec != HP2_CODEC_STORED) { fclose(f); return 0; }
	x->strm_bytes = malloc(sm->stored_size);
	if (!x->strm_bytes) { fclose(f); return 0; }
	fseek(f, (long)sm->offset, SEEK_SET);
	if (fread(x->strm_bytes, 1, sm->stored_size, f) != sm->stored_size) { fclose(f); return 0; }
	fclose(f);
	x->strm_size = sm->stored_size; x->strd_size = sd->raw_size; x->strd_offset = sd->offset;
	snprintf(x->strm_path, sizeof x->strm_path, "%s", path);
	return strm_open(&x->strm, x->strm_bytes, x->strm_size, x->strd_size) == STRM_OK;
}

static int mfx_load(mfx_t *x, const char *dir, const char *cin_pack)
{
	char p[300];
	size_t n;
	uint8_t *d;
	memset(x, 0, sizeof *x);
	snprintf(p, sizeof p, "%s/a10.obj.hp2", dir);
	if (!(d = mfx_section(p, "MODL", &n)) || model_open(&x->models, d, n) != MODEL_OK) return 0;
	if (!(d = mfx_section(p, "SCEN", &n)) || scen_open(&x->scen, d, n) != SCEN_OK) return 0;
	if (!(d = mfx_section(p, "ANIM", &n)) || anim_open(&x->anims, d, n) != ANIM_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.coll.hp2", dir);
	if (!(d = mfx_section(p, "COL0", &n)) || coll_open(d, n, &x->coll) != COLL_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.cmod.hp2", dir);
	if (!(d = mfx_section(p, "CMOD", &n)) || cmod_open(&x->cmod, d, n) != CMOD_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.unit.hp2", dir);
	if (!(d = mfx_section(p, "UNIT", &n)) || unitdef_open(&x->unit, d, n) != UNITDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.weap.hp2", dir);
	if (!(d = mfx_section(p, "WEAP", &n)) || weap_open(&x->weap, d, n) != WEAP_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.ansm.hp2", dir);
	if (!(d = mfx_section(p, "ASTB", &n)) || ansm_open(&x->ansm, d, n) != ANSM_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.fx.hp2", dir);
	if (!(d = mfx_section(p, "FXD0", &n)) || fxdef_open(&x->fx, d, n) != FXD_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.hud.hp2", dir);
	if (!(d = mfx_section(p, "HUD0", &n)) || huddef_open(&x->hud, d, n) != HUDDEF_OK) return 0;
	if (!(d = mfx_section(p, "STRS", &n)) || huddef_strings_open(&x->strs, d, n) != HUDDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.dev.hp2", dir);
	if (!(d = mfx_section(p, "DEVS", &n)) || devdef_open(&x->dev, d, n) != DEVDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.ai.hp2", dir);
	if (!(d = mfx_section(p, "AIDT", &n)) || aidef_open(&x->ai, d, n) != AIDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.script.hp2", dir);
	if (!(d = mfx_section(p, "SCRP", &n)) || hs_open(&x->scr, d, n) != HS_OK) return 0;
	if (!(d = mfx_section(cin_pack, "CINE", &n)) || cinedef_open(&x->cin, d, n) != CINEDEF_OK) return 0;
	{
		size_t sndd = 0;
		uint8_t *dd;
		snprintf(p, sizeof p, "%s/a10.snd.hp2", dir);
		if (!(d = mfx_section(p, "SND0", &n)) || !(dd = mfx_section(p, "SNDD", &sndd))) return 0;
		free(dd);
		if (snd_open(&x->snd, d, n, (uint32_t)sndd) != SND_OK) return 0;
	}
	snprintf(p, sizeof p, "%s/a10.strm.hp2", dir);
	if (!mfx_strm(x, p)) return 0;
	return 1;
}

/* Builds the world: BSP 0 objects, the player at the first starting location, devices, cinematics, script machine with `register_fn` registered. */
static void mfx_start(mfx_t *x, void (*register_fn)(hs_machine *), int with_machine)
{
	mission_t *m = &x->m;
	devdef_start st = devdef_get_start(&x->dev, 0);
	float pos[3];
	scen_player pl = scen_get_player(&x->scen);
	memset(&x->gi, 0, sizeof x->gi);
	memset(&x->hs, 0, sizeof x->hs);
	x->gi.ow.scen = &x->scen; x->gi.ow.models = &x->models; x->gi.ow.anims = &x->anims;
	objects_clear(&x->objects);
	objects_spawn_bsp(&x->objects, &x->gi.ow, 0, 0);
	fpview_init(&x->fp, &x->gi.ow, &pl);
	x->gi.unit = &x->unit; x->gi.weap = &x->weap; x->gi.cmod = &x->cmod; x->gi.ansm = &x->ansm; x->gi.fx = &x->fx; x->gi.coll = &x->coll;
	x->gi.objects = &x->objects; x->gi.fp = &x->fp; x->gi.seed = 5;
	pos[0] = st.pos[0]; pos[1] = st.pos[1]; pos[2] = st.pos[2] + 0.05f;
	game_init(&x->game, &x->gi, pos, st.facing);
	game_set_collision(&x->game, &x->coll);
	devices_init(&x->devices, &x->dev);
	cine_init(&x->cine);
	hud_state_init(&x->hudst);
	mission_init(m);
#ifdef HM_WITH_OBJ
	units_init(&x->units);
	m->units = &x->units;
	m->fx = NULL;
#endif
	m->game = &x->game; m->objects = &x->objects; m->ow = x->gi.ow;
	m->scripts = &x->scr; m->devices = &x->devices; m->dev = &x->dev; m->cine = &x->cine; m->cin = &x->cin; m->aidef = &x->ai;
	m->hud = &x->hudst; m->huddef = &x->hud; m->strings = &x->strs;
	m->difficulty = 3;
	m->cur_bsp = 0;
	m->misc_env.strm = &x->strm;
	m->misc_env.snd = &x->snd;
	mission_attach_player(m);
#ifdef HM_WITH_OBJ
	{
		unsigned i;
		for (i = 0; i < x->objects.count; i++)
			if (!(x->objects.obj[i].flags & OBJ_F_FREE) && x->objects.obj[i].type == SCEN_BIPED && (int)i != m->player_obj) units_attach(&x->units, &x->game, (int)i, 0, 0);
	}
#endif
	if (with_machine) {
		hs_setup(&x->hs);
		if (register_fn) register_fn(&x->hs);
		m->hs = &x->hs;
		hs_init(&x->hs, &x->scr, m);
	}
}

#endif
