/* Host test of the game simulation against the real a10 packs (skipped when assets/ is missing). */
#include "sim/game.h"
#include "sim/playerdef.h"
#include "sim/fx.h"
#include "sim/save.h"
#include "ui/hud.h"
#include "stream/hp2.h"
#include "world/anim.h"
#include "world/model.h"
#include "world/scen.h"
#include <math.h>
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
	static hp2_pack pk;
	const hp2_section *sec;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	img = malloc((size_t)n);
	if (fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(img); return NULL; }
	fclose(f);
	if (hp2_open(&pk, img, (size_t)n) != HP2_OK) { free(img); return NULL; }
	sec = hp2_find(&pk, tag);
	if (!sec) { free(img); return NULL; }
	raw = malloc(sec->raw_size + 64);
	if (hp2_load(&pk, sec, raw, sec->raw_size) != HP2_OK) { free(img); free(raw); return NULL; }
	*size = sec->raw_size;
	free(img);
	return raw;
}

static model_section g_models; static scen_section g_scen; static anim_section g_anims;
static coll_bsp g_coll; static cmod_section g_cmod; static unitdef_section g_unit; static weap_section g_weap;
static ansm_section g_ansm; static fxdef_section g_fx;
static object_table_t g_objects;
static fpview_t g_fp;
static game_t g_game;
static huddef_section g_hud;
static int g_has_hud;
static huddef_fonts g_fonts;
static int g_has_fonts;
static checkpoint_t g_ckpt;
static fx_t g_fxrt;

static int load_all(const char *dir)
{
	char p[256];
	size_t n;
	uint8_t *d;
	snprintf(p, sizeof p, "%s/a10.obj.hp2", dir);
	if (!(d = load_section(p, "MODL", &n)) || model_open(&g_models, d, n) != MODEL_OK) return 0;
	if (!(d = load_section(p, "SCEN", &n)) || scen_open(&g_scen, d, n) != SCEN_OK) return 0;
	if (!(d = load_section(p, "ANIM", &n)) || anim_open(&g_anims, d, n) != ANIM_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.coll.hp2", dir);
	if (!(d = load_section(p, "COL0", &n)) || coll_open(d, n, &g_coll) != COLL_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.cmod.hp2", dir);
	if (!(d = load_section(p, "CMOD", &n)) || cmod_open(&g_cmod, d, n) != CMOD_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.unit.hp2", dir);
	if (!(d = load_section(p, "UNIT", &n)) || unitdef_open(&g_unit, d, n) != UNITDEF_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.weap.hp2", dir);
	if (!(d = load_section(p, "WEAP", &n)) || weap_open(&g_weap, d, n) != WEAP_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.ansm.hp2", dir);
	if (!(d = load_section(p, "ASTB", &n)) || ansm_open(&g_ansm, d, n) != ANSM_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.fx.hp2", dir);
	if (!(d = load_section(p, "FXD0", &n)) || fxdef_open(&g_fx, d, n) != FXD_OK) return 0;
	snprintf(p, sizeof p, "%s/a10.hud.hp2", dir);
	if ((d = load_section(p, "HUD0", &n)) && huddef_open(&g_hud, d, n) == HUDDEF_OK) g_has_hud = 1;
	if ((d = load_section(p, "FONT", &n)) && huddef_fonts_open(&g_fonts, d, n) == HUDDEF_OK) g_has_fonts = 1;
	return 1;
}

static void run_ticks(game_t *g, game_input_t *in, int n)
{
	int i;
	unsigned prev = g->prev_held;
	for (i = 0; i < n; i++) {
		unsigned held = in->held;
		in->pressed = held & ~prev;
		game_tick(g, in);
		prev = held;
		in->pressed = 0;
	}
}


static object_world_t g_ow;
static game_init_t g_gi;
static int g_target = -1;                        /* the first placed crewman */
static float g_start[3];

static void new_scene(uint32_t seed)
{
	objects_clear(&g_objects);
	objects_spawn_bsp(&g_objects, &g_ow, 0, 0);
	{ scen_player pl = scen_get_player(&g_scen); fpview_init(&g_fp, &g_ow, &pl); }
	g_gi.objects = &g_objects; g_gi.fp = &g_fp; g_gi.seed = seed;
	game_init(&g_game, &g_gi, g_start, -1.5707963f);
}

static void aim_at(game_t *g, const object_t *b)
{
	float dx = b->pos[0] - g->player.pos[0], dy = b->pos[1] - g->player.pos[1], dz = (b->pos[2] + 0.35f) - (g->player.pos[2] + g->pp.eye_height);
	g->player.yaw = atan2f(dy, dx);
	g->player.pitch = atan2f(dz, sqrtf(dx * dx + dy * dy));
}

static int count_events(const game_t *g, int type)
{
	unsigned i, n, c = 0;
	const game_event_t *ev = game_events(g, &n);
	for (i = 0; i < n; i++) if (ev[i].type == type) c++;
	return (int)c;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "../../assets/hp2";
	game_input_t in;
	unsigned i;
	int t, w, found = 0, k;
	if (!load_all(dir)) { printf("skip: no a10 packs in %s\n", dir); return 0; }
	g_ow.scen = &g_scen; g_ow.models = &g_models; g_ow.anims = &g_anims;
	objects_clear(&g_objects);
	objects_spawn_bsp(&g_objects, &g_ow, 0, 0);
	for (i = 0; i < g_objects.count; i++) if (g_objects.obj[i].type == SCEN_BIPED) { g_target = (int)i; break; }
	CHECK(g_target >= 0);
	/* a start a few units from the crewman with a clear line of sight to its chest */
	{
		const object_t *bo = &g_objects.obj[g_target];
		for (k = 0; k < 72 && !found; k++) {
			float ang = (float)k * 0.0872665f, dist = 2.5f + (float)(k / 24), eye[3], tgt[3];
			coll_hit h;
			g_start[0] = bo->pos[0] + cosf(ang) * dist; g_start[1] = bo->pos[1] + sinf(ang) * dist; g_start[2] = bo->pos[2] + 0.05f;
			eye[0] = g_start[0]; eye[1] = g_start[1]; eye[2] = g_start[2] + 0.62f;
			tgt[0] = bo->pos[0]; tgt[1] = bo->pos[1]; tgt[2] = bo->pos[2] + 0.35f;
			if (coll_point_in_solid(&g_coll, eye) || coll_point_in_solid(&g_coll, g_start)) continue;
			if (!coll_ray(&g_coll, eye, tgt, 0, &h)) found = 1;
		}
		CHECK(found);
	}
	memset(&g_gi, 0, sizeof g_gi);
	g_gi.unit = &g_unit; g_gi.weap = &g_weap; g_gi.cmod = &g_cmod; g_gi.ansm = &g_ansm; g_gi.fx = &g_fx; g_gi.coll = &g_coll; g_gi.ow = g_ow;
	memset(&in, 0, sizeof in);

	/* 1. the player lands, stands, the data is what the tags say */
	new_scene(7);
	run_ticks(&g_game, &in, 60);
	CHECK(g_game.player.on_ground && fabsf(g_game.player.pos[2]) < 0.05f);
	CHECK(g_game.vitals.max_shield == 75.0f && g_game.vitals.max_health == 75.0f);
	CHECK(g_game.slot[0].weapon >= 0 && g_game.slot[1].weapon >= 0);
	CHECK(g_game.pp.radius > 0.1f && g_game.pp.radius < 0.3f);

	/* 2. walking forward moves the player and collision keeps it inside the world */
	{
		float x0 = g_game.player.pos[0], y0 = g_game.player.pos[1], eye[3];
		in.move.move_y = 1.0f;
		run_ticks(&g_game, &in, 90);
		in.move.move_y = 0.0f;
		CHECK(fabsf(g_game.player.pos[0] - x0) + fabsf(g_game.player.pos[1] - y0) > 0.5f);
		player_eye(&g_game.player, &g_game.pp, eye);
		CHECK(!coll_point_in_solid(&g_coll, eye));
	}

	/* 3. firing at the crewman: shots, projectiles, hits and a kill; the checkpoint undoes it */
	new_scene(7);
	run_ticks(&g_game, &in, 45);
	checkpoint_save(&g_ckpt, &g_game, &g_fp, NULL);
	{
		float px = g_game.player.pos[0], py = g_game.player.pos[1];
		uint32_t tick0 = g_game.tick;
		aim_at(&g_game, &g_objects.obj[g_target]);
		in.held = IN_FIRE;
		run_ticks(&g_game, &in, 120);
		in.held = 0;
		CHECK(g_game.st_shots > 0 && g_game.st_projectiles >= g_game.st_shots);
		CHECK(g_game.st_hits > 0);
		CHECK(g_game.st_kills >= 1);
		CHECK(g_game.unit_state[g_target].dead);
		CHECK(g_game.slot[g_game.cur].st.loaded[0] < g_game.slot[g_game.cur].def.magazine[0].rounds_loaded_max || g_game.st_reloads > 0);
		CHECK(checkpoint_revert(&g_ckpt, &g_game, &g_objects, &g_fp, NULL));
		CHECK(g_game.tick == tick0 && g_game.st_kills == 0 && g_game.st_shots == 0 && !g_game.unit_state[g_target].dead);
		CHECK(g_game.player.pos[0] == px && g_game.player.pos[1] == py);
		CHECK(g_game.objects == &g_objects && g_game.fp == &g_fp);
	}

	/* 4. every weapon of every map fires, spends ammo, reloads and spawns its projectiles: the a10 scene (world, targets) is reused
	 *    with each map's WEAP pack as the weapon definitions (the models of other maps' weapons are not needed by the simulation) */
	{
		static const char *maps[] = { "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40" };
		static char tested[64][WEAP_NAME_LEN];
		static weap_section ws;
		unsigned ntested = 0, mi;
		for (mi = 0; mi < sizeof maps / sizeof maps[0]; mi++) {
			char p[256];
			size_t n;
			uint8_t *d;
			snprintf(p, sizeof p, "%s/%s.weap.hp2", dir, maps[mi]);
			if (!(d = load_section(p, "WEAP", &n)) || weap_open(&ws, d, n) != WEAP_OK) continue;
			for (w = 0; w < (int)ws.n_weapons; w++) {
				weap_weapon wd;
				unsigned shots0, proj0, loaded0, reloads0, seen = 0, q;
				weap_get_weapon(&ws, (uint32_t)w, &wd);
				for (q = 0; q < ntested; q++) if (strcmp(tested[q], wd.name) == 0) seen = 1;
				if (seen || ntested >= 64) continue;
				strncpy(tested[ntested++], wd.name, WEAP_NAME_LEN - 1);
				g_gi.weap = &ws;
				new_scene(11 + (uint32_t)w);
				run_ticks(&g_game, &in, 30);
				g_game.slot[0].weapon = -1; g_game.cur = -1;
				game_give_weapon(&g_game, 0, wd.tag_id);
				CHECK(g_game.cur == 0 && g_game.slot[0].weapon == w);
				run_ticks(&g_game, &in, 60);                  /* ready animation */
				aim_at(&g_game, &g_objects.obj[g_target]);
				shots0 = (unsigned)g_game.st_shots; proj0 = (unsigned)g_game.st_projectiles; loaded0 = g_game.slot[0].st.loaded[0]; reloads0 = (unsigned)g_game.st_reloads;
				in.held = IN_FIRE;
				run_ticks(&g_game, &in, 75);
				in.held = 0;
				run_ticks(&g_game, &in, 12);                  /* charge weapons fire on release */
				printf("  %s weapon %-16s shots %3u projectiles %3u loaded %u -> %u reloads %u hits %u\n", maps[mi], wd.name, (unsigned)g_game.st_shots - shots0,
				       (unsigned)g_game.st_projectiles - proj0, loaded0, g_game.slot[0].st.loaded[0], (unsigned)g_game.st_reloads - reloads0, (unsigned)g_game.st_hits);
				if (wd.trigger[0].projectile == 0) {                   /* melee-only weapon (energy sword): the melee button works instead */
					unsigned m0 = (unsigned)g_game.st_melee;
					in.held = IN_MELEE; run_ticks(&g_game, &in, 1); in.held = 0; run_ticks(&g_game, &in, 40);
					CHECK(g_game.st_melee > m0);
					continue;
				}
				CHECK(g_game.st_shots > shots0);
				CHECK(g_game.st_projectiles > proj0);
				CHECK(wd.magazine_count == 0 || wd.magazine[0].rounds_loaded_max <= 1 || g_game.slot[0].st.loaded[0] < loaded0 || g_game.st_reloads > reloads0 ||
				      wd.trigger[0].heat_per_round > 0.0f || wd.trigger[0].age_per_round > 0.0f);
			}
		}
		g_gi.weap = &g_weap;
		printf("  %u distinct weapons exercised\n", ntested);
		CHECK(ntested >= 6);
	}

	/* 5. a thrown grenade detonates and hurts (frag grenade: type 0) */
	new_scene(5);
	run_ticks(&g_game, &in, 45);
	aim_at(&g_game, &g_objects.obj[g_target]);
	g_game.player.pitch += 0.12f;
	in.held = IN_GRENADE;
	{
		int detonated = 0;
		run_ticks(&g_game, &in, 1);
		in.held = 0;
		for (t = 0; t < 200 && !detonated; t++) { run_ticks(&g_game, &in, 1); detonated += count_events(&g_game, GE_DETONATE); }
		CHECK(g_game.st_throws == 1);
		CHECK(detonated > 0);
	}

	/* 5b. an AI unit's shot hits the player and hurts the shield */
	new_scene(21);
	run_ticks(&g_game, &in, 45);
	{
		float eye[3], fwd[3], from[3], dir[3];
		int pi = weap_find_projectile(&g_weap, g_game.slot[0].def.trigger[0].projectile);
		float s0 = g_game.vitals.shield;
		game_eye(&g_game, eye, fwd);
		from[0] = eye[0] + 1.5f; from[1] = eye[1]; from[2] = eye[2] - 0.2f;
		dir[0] = eye[0] - from[0]; dir[1] = eye[1] - from[1]; dir[2] = (eye[2] - 0.2f) - from[2];
		CHECK(pi >= 0 && game_fire_projectile(&g_game, 0, (unsigned)pi, from, dir, 0.0f));
		run_ticks(&g_game, &in, 6);
		CHECK(g_game.vitals.shield < s0 || g_game.vitals.health < 75.0f);
		CHECK(g_game.hurt_flash > 0.0f || g_game.vitals.shield < s0);
	}

	/* 6. pickup: the unarmed placement of a weapon object swaps it in (if the map places one in BSP 0, skipped otherwise) */
	/* 7. the HUD lays out inside the screen and shows something */
	if (g_has_hud) {
		static hud_frame_t hf;
		new_scene(3);
		run_ticks(&g_game, &in, 45);
		hud_build(&hf, &g_hud, NULL, &g_game, 45, NULL);
		printf("  hud: %u quads (%u elements, %u meters, %u digits)\n", hf.n, hf.elements, hf.meters, hf.digits);
		CHECK(hf.n > 8 && hf.meters >= 1);
		for (i = 0; i < hf.n; i++) CHECK(hf.q[i].x1 > hf.q[i].x0 && hf.q[i].y1 > hf.q[i].y0 && hf.q[i].x1 > -64.0f && hf.q[i].x0 < 704.0f && hf.q[i].y1 > -64.0f && hf.q[i].y0 < 544.0f);
		CHECK(hf.digits >= 2);                    /* ammo counters */
		if (g_has_fonts) {
			hud_state_t hs;
			static hud_frame_t hf2;
			unsigned n0 = hf.n;
			hud_state_init(&hs);
			hud_message(&hs, "Picked up a pistol", 90);
			hud_build(&hf2, &g_hud, &g_fonts, &g_game, 45, &hs);
			printf("  hud with a message: %u quads\n", hf2.n);
			CHECK(hf2.n >= n0 + 10);              /* one quad per glyph */
			hud_tick(&hs);
			CHECK(hs.ticks[0] == 89);
		}
	}

	/* 8. effects: an impact effect makes particles that live and die */
	{
		float pos[3] = { 0, 0, 1 }, nrm[3] = { 0, 0, 1 };
		unsigned made = 0, ei;
		const game_event_t *ev;
		unsigned nev;
		new_scene(9);
		fx_init(&g_fxrt, &g_fx, g_game.globals.gravity * 900.0f, 4);
		run_ticks(&g_game, &in, 45);
		aim_at(&g_game, &g_objects.obj[g_target]);
		in.held = IN_FIRE;
		for (t = 0; t < 40; t++) {
			run_ticks(&g_game, &in, 1);
			ev = game_events(&g_game, &nev);
			for (ei = 0; ei < nev; ei++) if (ev[ei].type == GE_IMPACT && ev[ei].tag) made += fx_spawn(&g_fxrt, ev[ei].tag, ev[ei].pos, ev[ei].normal, 1);
			fx_tick(&g_fxrt);
		}
		in.held = 0;
		(void)pos; (void)nrm;
		printf("  fx: %u particles/decals created, %u active\n", made, fx_active_particles(&g_fxrt) + fx_active_decals(&g_fxrt));
		CHECK(made > 0);
		for (t = 0; t < 4000; t++) fx_tick(&g_fxrt);
		CHECK(fx_active_particles(&g_fxrt) == 0);
	}

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
