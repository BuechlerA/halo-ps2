/* A mission_t built from the real a10 packs for the host tests of the script host (H1, H2) and of anything else that needs a whole
 * mission: objects of BSP 0, the game (player, weapons, vitals), units, devices, cinematics, effects, AI data and the script machine.
 * Header only, static data (a fixture is ~1.2 MB: declare it `static`). Needs assets/hp2/a10.{obj,coll,cmod,unit,weap,ansm,fx,dev,ai,script}.hp2
 * and, optionally, a CINE pack (the Makefile converts $(HOST_TMP)/halo-cine-a10.hp2). mfix_load() returns 0 when something is missing: the test skips.
 *
 *   static mission_fixture F;
 *   if (!mfix_load(&F, "../../assets/hp2", cine_pack_or_NULL)) { puts("skip"); return 0; }
 *   mfix_start(&F, seed);                       // fresh scene: objects, game, units, devices, cinematics, effects, mission, player body
 *   mfix_hs_start(&F, hs_host_register_objects); // script machine with the host's built-ins (init runs the startup scripts)
 *   mfix_tick(&F, &input);                      // one mission tick in the stage-2 order (hooks: F.pre / F.post, see below)
 *   hs_value r = mfix_call(&F, "object_destroy", HS_VOID, args, 1);   // call a registered built-in directly
 *
 * The units module (ai/units.c, workstream U) may not exist yet: the weak definitions below (a minimal unit layer: one unit per placed
 * biped, controls stored, custom actions counted down, damage and kill through the game's vitals) are replaced by the real ones as soon as the
 * test links ai/units.c. */
#ifndef TEST_MISSION_FIXTURE_H
#define TEST_MISSION_FIXTURE_H
#include "ai/aidef.h"
#include "script/hs.h"
#include "sim/devices.h"
#include "sim/fx.h"
#include "sim/game.h"
#include "sim/mission.h"
#include "sim/playerdef.h"
#include "stream/hp2.h"
#include "world/anim.h"
#include "world/cinedef.h"
#include "world/devdef.h"
#include "world/model.h"
#include "world/scen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- weak stand-in for the unit layer ---- */
__attribute__((weak)) void units_init(units_t *s)
{
	int i;
	memset(s, 0, sizeof *s);
	for (i = 0; i < OBJ_MAX; i++) s->by_obj[i] = -1;
}
__attribute__((weak)) int units_attach(units_t *s, game_t *g, int obj, int team, int32_t weapon_tag)
{
	int i;
	const object_t *o;
	(void)weapon_tag;
	if (!g->objects || obj < 0 || obj >= OBJ_MAX) return -1;
	if (s->by_obj[obj] >= 0) return s->by_obj[obj];
	for (i = 0; i < UNIT_MAX; i++) if (!s->u[i].active) break;
	if (i == UNIT_MAX) return -1;
	o = &g->objects->obj[obj];
	memset(&s->u[i], 0, sizeof s->u[i]);
	s->u[i].active = 1; s->u[i].team = (uint8_t)(team ? team : 2); s->u[i].obj = (int16_t)obj; s->u[i].weapon = -1; s->u[i].custom_anim = -1;
	memcpy(s->u[i].body.pos, o->pos, sizeof o->pos);
	s->u[i].body.yaw = atan2f(o->root.m[0][1], o->root.m[0][0]);
	s->by_obj[obj] = (int16_t)i;
	if ((unsigned)i >= s->count) s->count = (unsigned)i + 1;
	s->spawned++;
	return i;
}
__attribute__((weak)) void units_detach(units_t *s, int unit)
{
	if (unit < 0 || unit >= UNIT_MAX || !s->u[unit].active) return;
	s->by_obj[s->u[unit].obj] = -1;
	s->u[unit].active = 0;
}
__attribute__((weak)) int units_of_object(const units_t *s, int obj) { return (obj >= 0 && obj < OBJ_MAX) ? s->by_obj[obj] : -1; }
__attribute__((weak)) void units_tick(units_t *s, game_t *g)
{
	unsigned i;
	(void)g;
	for (i = 0; i < s->count; i++) {
		unit_t *u = &s->u[i];
		if (!u->active) continue;
		if (u->action == UA_CUSTOM) { if (u->action_ticks > 0) u->action_ticks--; if (u->action_ticks == 0) { u->action = UA_IDLE; u->custom_anim = -1; } }
	}
}
__attribute__((weak)) void units_set_control(units_t *s, int unit, const unit_control_t *c) { if (unit >= 0 && unit < UNIT_MAX) s->u[unit].ctl = *c; }
__attribute__((weak)) float units_damage(units_t *s, game_t *g, int unit, float amount)
{
	if (unit < 0 || unit >= UNIT_MAX || !s->u[unit].active) return 0.0f;
	return game_damage_object(g, s->u[unit].obj, amount, NULL);
}
__attribute__((weak)) void units_kill(units_t *s, game_t *g, int unit, int silent) { (void)silent; (void)units_damage(s, g, unit, 1.0e9f); }
__attribute__((weak)) int units_alive(const units_t *s, const game_t *g, int unit)
{
	return unit >= 0 && unit < UNIT_MAX && s->u[unit].active && !g->unit_state[s->u[unit].obj].dead;
}
__attribute__((weak)) void units_teleport(units_t *s, game_t *g, int unit, const float pos[3], float yaw)
{
	object_t *o;
	if (unit < 0 || unit >= UNIT_MAX || !s->u[unit].active) return;
	o = &g->objects->obj[s->u[unit].obj];
	memcpy(s->u[unit].body.pos, pos, 3 * sizeof(float));
	s->u[unit].body.yaw = yaw;
	memcpy(o->pos, pos, 3 * sizeof(float));
	mat4_from_ypr(&o->root, yaw, 0.0f, 0.0f, o->pos);
}
__attribute__((weak)) void units_give_weapon(units_t *s, game_t *g, int unit, uint32_t weapon_tag) { (void)s; (void)g; (void)unit; (void)weapon_tag; }
__attribute__((weak)) void units_eye(const units_t *s, int unit, float eye[3], float dir[3])
{
	const unit_t *u = &s->u[unit];
	eye[0] = u->body.pos[0]; eye[1] = u->body.pos[1]; eye[2] = u->body.pos[2] + 0.6f;
	dir[0] = cosf(u->ctl.aim_yaw); dir[1] = sinf(u->ctl.aim_yaw); dir[2] = 0.0f;
	if (u->ctl.aim_yaw == 0.0f) { dir[0] = cosf(u->body.yaw); dir[1] = sinf(u->body.yaw); }
}
__attribute__((weak)) int units_teams_hostile(int a, int b) { return a != b && (a == 3 || b == 3); }
__attribute__((weak)) void units_apply_cinedef(units_t *s, int unit, const cinedef_ctl *c)
{
	unit_control_t k;
	if (unit < 0 || unit >= UNIT_MAX || !s->u[unit].active || !c) return;
	memset(&k, 0, sizeof k);
	k.throttle_fwd = c->throttle[0]; k.throttle_left = c->throttle[1];
	k.aim_yaw = (float)c->yaw * 0.00314159265f; k.aim_pitch = (float)c->pitch * 0.00314159265f; k.weapon_slot = -1;
	s->u[unit].ctl = k;
}
__attribute__((weak)) unsigned units_custom_animation(units_t *s, game_t *g, int unit, int graph, int anim, float frame_scale, int hold)
{
	object_t *o;
	anim_graph ag;
	anim_info ai;
	unsigned ticks;
	(void)hold;
	if (unit < 0 || unit >= UNIT_MAX || !s->u[unit].active || graph < 0 || !g->ow.anims) return 0;
	o = &g->objects->obj[s->u[unit].obj];
	ag = anim_get_graph(g->ow.anims, (uint32_t)graph);
	if (anim < 0 || (unsigned)anim >= ag.anim_count) return 0;
	ai = anim_get(g->ow.anims, &ag, (uint32_t)anim);
	ticks = (unsigned)((float)ai.frame_count / (frame_scale * (ai.fps > 0.0f ? ai.fps / 30.0f : 1.0f)) + 0.5f);
	if (!ticks) ticks = 1;
	o->graph = (int16_t)graph; o->anim = (uint16_t)anim; o->frame = 0.0f; o->flags |= OBJ_F_ANIMATED;
	s->u[unit].action = UA_CUSTOM; s->u[unit].custom_anim = (int16_t)anim; s->u[unit].action_ticks = (uint16_t)ticks;
	return ticks;
}
__attribute__((weak)) void units_stop_custom(units_t *s, game_t *g, int unit)
{
	(void)g;
	if (unit < 0 || unit >= UNIT_MAX || !s->u[unit].active || s->u[unit].action != UA_CUSTOM) return;
	s->u[unit].action = UA_IDLE; s->u[unit].action_ticks = 0; s->u[unit].custom_anim = -1;
}

/* ---- the fixture ---- */
typedef struct mission_fixture {
	model_section models; scen_section scen; anim_section anims;
	coll_bsp coll; cmod_section cmod; unitdef_section unit; weap_section weap; ansm_section ansm; fxdef_section fx;
	devdef_section dev; cinedef_section cin; int has_cin; aidef_section ai; int has_ai; hs_section scr; int has_scr;
	object_table_t objects;
	fpview_t fp;
	game_t game;
	units_t units;
	devices_t devices;
	cine_state cine;
	fx_t fxrt;
	hs_machine hs;
	mission_t mission;
	object_world_t ow;
	game_init_t gi;
	char *strpool; uint32_t strpool_cap;       /* the script's strings plus room for the test's own */
	/* hooks of the tick (the integrator's mission_tick wiring): after scripts + devices / after units + game */
	void (*pre)(mission_t *, const game_input_t *);
	void (*post)(mission_t *);
} mission_fixture;

static uint8_t *mfix_section(const char *path, const char *tag, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;
	uint8_t *img, *raw;
	static hp2_pack pk;
	const hp2_section *sec;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	img = (uint8_t *)malloc((size_t)n);
	if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(img); return NULL; }
	fclose(f);
	if (hp2_open(&pk, img, (size_t)n) != HP2_OK) { free(img); return NULL; }
	sec = hp2_find(&pk, tag);
	if (!sec) { free(img); return NULL; }
	raw = (uint8_t *)malloc(sec->raw_size + 64);
	if (!raw || hp2_load(&pk, sec, raw, sec->raw_size) != HP2_OK) { free(img); free(raw); return NULL; }
	*size = sec->raw_size;
	free(img);
	return raw;
}

static int mfix_load(mission_fixture *f, const char *dir, const char *cine_pack)
{
	char p[256];
	size_t n;
	uint8_t *d;
	memset(f, 0, sizeof *f);
	snprintf(p, sizeof p, "%s/a10.obj.hp2", dir);
	if (!(d = mfix_section(p, "MODL", &n)) || model_open(&f->models, d, n) != MODEL_OK) return 0;
	if (!(d = mfix_section(p, "SCEN", &n)) || scen_open(&f->scen, d, n) != SCEN_OK) return 0;
	if (!(d = mfix_section(p, "ANIM", &n)) || anim_open(&f->anims, d, n) != ANIM_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.coll.hp2", dir);
	if (!(d = mfix_section(p, "COL0", &n)) || coll_open(d, n, &f->coll) != COLL_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.cmod.hp2", dir);
	if (!(d = mfix_section(p, "CMOD", &n)) || cmod_open(&f->cmod, d, n) != CMOD_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.unit.hp2", dir);
	if (!(d = mfix_section(p, "UNIT", &n)) || unitdef_open(&f->unit, d, n) != UNITDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.weap.hp2", dir);
	if (!(d = mfix_section(p, "WEAP", &n)) || weap_open(&f->weap, d, n) != WEAP_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.ansm.hp2", dir);
	if (!(d = mfix_section(p, "ASTB", &n)) || ansm_open(&f->ansm, d, n) != ANSM_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.fx.hp2", dir);
	if (!(d = mfix_section(p, "FXD0", &n)) || fxdef_open(&f->fx, d, n) != FXD_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.dev.hp2", dir);
	if (!(d = mfix_section(p, "DEVS", &n)) || devdef_open(&f->dev, d, n) != DEVDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.ai.hp2", dir);
	if ((d = mfix_section(p, "AIDT", &n)) && aidef_open(&f->ai, d, n) == AIDEF_OK) f->has_ai = 1;
	snprintf(p, sizeof p, "%s/a10.script.hp2", dir);
	if ((d = mfix_section(p, "SCRP", &n)) && hs_open(&f->scr, d, n) == HS_OK) {
		f->has_scr = 1;
		f->strpool_cap = f->scr.string_bytes + 16384u;
		f->strpool = (char *)malloc(f->strpool_cap);
		if (!f->strpool) return 0;
		memcpy(f->strpool, f->scr.strings, f->scr.string_bytes);
		f->scr.strings = f->strpool;
	}
	if (cine_pack && (d = mfix_section(cine_pack, "CINE", &n)) && cinedef_open(&f->cin, d, n) == CINEDEF_OK) f->has_cin = 1;
	f->ow.scen = &f->scen; f->ow.models = &f->models; f->ow.anims = &f->anims;
	return 1;
}

/* A fresh scene. The script machine is separate (mfix_hs_start). */
static void mfix_start(mission_fixture *f, uint32_t seed)
{
	float pos[3] = { 0, 0, 0 }, yaw = 0.0f;
	unsigned i;
	scen_player pl = scen_get_player(&f->scen);
	if (f->dev.start_count) { devdef_start st = devdef_get_start(&f->dev, 0); memcpy(pos, st.pos, sizeof pos); yaw = st.facing; }
	objects_clear(&f->objects);
	objects_spawn_bsp(&f->objects, &f->ow, 0, 0);
	fpview_init(&f->fp, &f->ow, &pl);
	memset(&f->gi, 0, sizeof f->gi);
	f->gi.unit = &f->unit; f->gi.weap = &f->weap; f->gi.cmod = &f->cmod; f->gi.ansm = &f->ansm; f->gi.fx = &f->fx; f->gi.coll = &f->coll;
	f->gi.ow = f->ow; f->gi.objects = &f->objects; f->gi.fp = &f->fp; f->gi.seed = seed;
	game_init(&f->game, &f->gi, pos, yaw);
	units_init(&f->units);
	devices_init(&f->devices, &f->dev);
	cine_init(&f->cine);
	fx_init(&f->fxrt, &f->fx, f->game.globals.gravity * 900.0f, seed);
	mission_init(&f->mission);
	f->mission.game = &f->game; f->mission.objects = &f->objects; f->mission.ow = f->ow; f->mission.units = &f->units;
	f->mission.scripts = f->has_scr ? &f->scr : NULL; f->mission.devices = &f->devices; f->mission.dev = &f->dev;
	f->mission.cine = &f->cine; f->mission.cin = f->has_cin ? &f->cin : NULL; f->mission.aidef = f->has_ai ? &f->ai : NULL;
	f->mission.fx = &f->fxrt; f->mission.cur_bsp = 0; f->mission.difficulty = 3;
	mission_attach_player(&f->mission);
	for (i = 0; i < f->objects.count; i++)
		if (!(f->objects.obj[i].flags & OBJ_F_FREE) && f->objects.obj[i].type == SCEN_BIPED && (int)i != f->mission.player_obj)
			units_attach(&f->units, &f->game, (int)i, 0, 0);
	f->mission.hs = NULL;
}

/* Registers `reg` (may be NULL) and starts the script machine on the fixture's SCRP; the host of every built-in is &f->mission. */
static void mfix_hs_start(mission_fixture *f, void (*reg)(hs_machine *))
{
	hs_setup(&f->hs);
	if (reg) reg(&f->hs);
	if (f->has_scr) hs_init(&f->hs, &f->scr, &f->mission);
	f->mission.hs = f->has_scr ? &f->hs : NULL;
}

/* One mission tick in the stage-2 order: sync player -> scripts -> devices -> [pre] -> units -> game -> [post] -> cinematics. */
static void mfix_tick(mission_fixture *f, const game_input_t *in)
{
	game_input_t none;
	mission_t *m = &f->mission;
	memset(&none, 0, sizeof none);
	if (!in) in = &none;
	mission_sync_player(m);
	if (m->hs) hs_tick(m->hs);
	if (m->devices) devices_tick(m->devices);
	if (f->pre) f->pre(m, in);
	if (m->units && m->game) units_tick(m->units, m->game);
	if (m->game) game_tick(m->game, m->cinematic ? &none : in);
	if (f->post) f->post(m);
	if (m->cine) cine_tick(m->cine);
	m->tick++;
}

/* A string argument: the text goes behind the script's own strings (offsets stay valid). */
static hs_value mfix_str(mission_fixture *f, const char *s)
{
	hs_value v;
	uint32_t len = (uint32_t)strlen(s) + 1u;
	memset(&v, 0, sizeof v);
	v.type = HS_STRING; v.vtype = 9;
	if (!f->strpool || f->scr.string_bytes + len > f->strpool_cap) return v;
	memcpy(f->strpool + f->scr.string_bytes, s, len);
	v.v.i = (int32_t)f->scr.string_bytes;
	f->scr.string_bytes += len;
	return v;
}

/* ---- argument constructors ---- */
static hs_value mfix_real(float x) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_REAL; v.vtype = 6; v.v.f = x; return v; }
static hs_value mfix_short(int x) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_SHORT; v.vtype = 7; v.v.i = x; return v; }
static hs_value mfix_bool(int x) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_BOOL; v.vtype = 5; v.v.i = x != 0; return v; }
static hs_value mfix_handle(uint32_t h, unsigned vtype) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_HANDLE; v.vtype = (uint8_t)vtype; v.v.h = h; return v; }
static hs_value mfix_obj(int slot) { return mfix_handle(mission_obj_handle(slot), 37); }       /* a runtime object / unit handle */
static hs_value mfix_name(uint32_t name_index) { return mfix_handle(name_index, 37); }        /* an object-name constant */

/* Calls the registered built-in `name` directly; `ret_cls` is the class the call site would expect (HS_VOID, HS_BOOL, HS_REAL, HS_HANDLE...).
 * Returns the result (type HS_VOID with vtype 255 when the built-in is not registered). */
static hs_value mfix_call(mission_fixture *f, const char *name, unsigned ret_cls, const hs_value *args, unsigned n)
{
	unsigned i;
	hs_value r = hs_neutral(ret_cls, ret_cls == HS_HANDLE ? 37u : 0u);
	for (i = 0; i < f->hs.nreg; i++) {
		if (strcmp(f->hs.reg[i].name, name) == 0 && f->hs.reg[i].fn) {
			f->hs.reg[i].fn(&f->hs, &f->mission, args, n, &r);
			return r;
		}
	}
	r.type = HS_VOID; r.vtype = 255;
	return r;
}

/* ---- queries over the real data (so tests need no content names) ---- */
/* first object name whose placement is a `scen_type` and (hidden: created by scripts / visible: placed at the start); -1 if none */
static int mfix_find_name(mission_fixture *f, int scen_type, int initially_visible, int want_model)
{
	uint32_t i;
	for (i = 0; i < f->dev.name_count; i++) {
		devdef_name nm = devdef_get_name(&f->dev, i);
		scen_placement pl;
		scen_def d;
		if (!(nm.flags & DEVDEF_NF_RESOLVED) || nm.scen_index == DEVDEF_NONE || nm.scen_index >= f->scen.placement_count) continue;
		pl = scen_get_placement(&f->scen, nm.scen_index);
		d = scen_get_def(&f->scen, pl.def);
		if (d.type != scen_type || !!(pl.flags & SCEN_P_INITIALLY_VISIBLE) != !!initially_visible) continue;
		if (want_model && model_find(&f->models, d.model_tag) < 0) continue;
		if (!(pl.bsp_mask & 1u)) continue;           /* BSP 0 only */
		return (int)i;
	}
	return -1;
}

/* A short animation of the graph of an object (index) with at least min_frames frames; -1 if none. */
static int mfix_find_anim(mission_fixture *f, int graph, unsigned min_frames, unsigned max_frames)
{
	anim_graph g;
	unsigned k;
	if (graph < 0) return -1;
	g = anim_get_graph(&f->anims, (uint32_t)graph);
	for (k = 0; k < g.anim_count; k++) {
		anim_info a = anim_get(&f->anims, &g, k);
		if (a.frame_count >= min_frames && a.frame_count <= max_frames && a.name[0]) return (int)k;
	}
	return -1;
}

#endif
