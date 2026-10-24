/* Host test of the units runtime (docs/31-units.md) against the real a10 packs: skipped when assets/ is missing. Covers attach and the tag
 * numbers (speeds, turn rate, vitals x difficulty), scripted walks through BSP 0 against COL0, animation selection per state, weapons (shots,
 * ammo, reload, grenades, melee), damage, flinch, death, the cinedef control mapping, custom animations, a 96 unit tick benchmark (with
 * UNITS_BENCH=1, build without sanitizers) and snapshot / restore determinism. No content names appear in this file: species are chosen by
 * what the tags say (team, speeds, weapons). */
#define _POSIX_C_SOURCE 200809L
#include "sim/units.h"
#include "sim/playerdef.h"
#include "stream/hp2.h"
#include "world/anim.h"
#include "world/model.h"
#include "world/scen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
static game_t g_game;
static units_t g_units;
static object_world_t g_ow;
static game_init_t g_gi;
static game_input_t g_in0;
static float g_pstart[3];
static float g_lod;                      /* units_t.lod_distance of the next scenes */

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
	return 1;
}

/* ---- scene helpers ---------------------------------------------------------------------------------------------------------- */
static void new_scene(uint32_t seed, int difficulty)
{
	objects_clear(&g_objects);
	objects_spawn_bsp(&g_objects, &g_ow, 0, 0);
	g_gi.objects = &g_objects; g_gi.seed = seed;
	game_init(&g_game, &g_gi, g_pstart, 0.0f);
	g_game.difficulty = difficulty;
	g_game.vitals.health = g_game.vitals.max_health = 1e6f;          /* the player is only a bystander unless a test says otherwise */
	g_game.vitals.shield = g_game.vitals.max_shield = 1e6f;
	units_init(&g_units);
	g_units.lod_distance = g_lod;
}

/* an object table with nothing in it but what the test spawns (shots then cannot hit a placed crate on the way) */
static void new_scene_empty(uint32_t seed, int difficulty)
{
	objects_clear(&g_objects);
	g_gi.objects = &g_objects; g_gi.seed = seed;
	game_init(&g_game, &g_gi, g_pstart, 0.0f);
	g_game.difficulty = difficulty;
	g_game.vitals.health = g_game.vitals.max_health = 1e6f;
	g_game.vitals.shield = g_game.vitals.max_shield = 1e6f;
	units_init(&g_units);
	g_units.lod_distance = g_lod;
}

static void park_player(void)
{
	float hi[3] = { g_pstart[0], g_pstart[1], 400.0f };
	game_teleport(&g_game, hi, 0.0f);
}

static unsigned g_evtotal[8];                           /* unit events seen so far, by type */
static void count_events(const units_t *s)
{
	unsigned n, i;
	const unit_event_t *ev = units_events(s, &n);
	for (i = 0; i < n; i++) if (ev[i].type < 8) g_evtotal[ev[i].type]++;
}

static void step_with(units_t *s, int n)
{
	int i;
	for (i = 0; i < n; i++) { units_tick(s, &g_game); count_events(s); game_tick(&g_game, &g_in0); objects_tick(&g_objects, &g_ow); }
}
static void step(int n) { step_with(&g_units, n); }

static int spawn_unit(unsigned def, const float pos[3], float yaw, int team, int32_t weapon)
{
	int slot = objects_spawn_def(&g_objects, &g_ow, def, pos, yaw);
	if (slot < 0) return -1;
	game_object_spawned(&g_game, (unsigned)slot);
	return units_attach(&g_units, &g_game, slot, team, weapon);
}

static object_t *obj_of(int unit) { return &g_objects.obj[g_units.u[unit].obj]; }

static const char *anim_name(const object_t *o)
{
	static char buf[ANIM_NAME_LEN + 1];
	anim_graph gr;
	if (o->graph < 0 || o->anim == SCEN_NO_ANIM) return "-";
	gr = anim_get_graph(&g_anims, (uint32_t)o->graph);
	memcpy(buf, anim_get(&g_anims, &gr, o->anim).name, ANIM_NAME_LEN);
	buf[ANIM_NAME_LEN] = 0;
	return buf;
}

static const char *lookup_name(int unit, unit_action a, unsigned dir, int *overlay)
{
	static char buf[ANIM_NAME_LEN + 1];
	ansm_graph gr;
	ansm_anim ai;
	int k = units_lookup_anim(&g_units, &g_game, unit, a, dir, overlay);
	if (k < 0 || ansm_find_graph(&g_ansm, g_units.u[unit].anim_tag, &gr) != 0 || ansm_anim_info(&g_ansm, &gr, (uint32_t)k, &ai) != 0) return "-";
	snprintf(buf, sizeof buf, "%.*s", ANIM_NAME_LEN - 1, ai.name);
	return buf;
}

static int in_solid(const unit_t *u)
{
	float h = u->body.crouch > 0.5f ? u->pp.crouch_height : u->pp.height, p[3];
	p[0] = u->body.pos[0]; p[1] = u->body.pos[1];
	p[2] = u->body.pos[2] + 0.5f * h;
	if (coll_point_in_solid(&g_coll, p)) return 1;
	p[2] = u->body.pos[2] + 0.2f * h + 0.02f;
	return coll_point_in_solid(&g_coll, p);
}

static uint32_t hash_u32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static float hrand(uint32_t a, uint32_t b) { return (float)(hash_u32(a * 2654435761u + b * 40503u + 12345u) >> 8) * (1.0f / 16777216.0f); }

/* species: one biped definition per kind of unit the tags describe */
typedef struct { unsigned def; unitdef_biped_def b; int widx; } species_t;
static species_t g_sp[16];
static unsigned g_nsp;

static void find_species(void)
{
	unsigned d;
	g_nsp = 0;
	for (d = 0; d < g_scen.def_count && g_nsp < 16; d++) {
		scen_def sd = scen_get_def(&g_scen, d);
		unitdef_biped_def b;
		int widx;
		if (sd.type != SCEN_BIPED || !unitdef_biped(&g_unit, sd.tag_id, &b)) continue;
		if (!(b.anim_speed[0] > 1.0f) || !(b.health > 0.0f) || b.weapon_count < 1 || b.turn_speed <= 0.0f) continue;     /* walkers that carry a weapon */
		widx = weap_find_weapon(&g_weap, b.weapon_tags[0]);
		if (widx < 0) continue;
		g_sp[g_nsp].def = d; g_sp[g_nsp].b = b; g_sp[g_nsp].widx = widx;
		g_nsp++;
	}
}
static int sp_by_team(int team) { unsigned i; for (i = 0; i < g_nsp; i++) if (g_sp[i].b.default_team == team) return (int)i; return -1; }

/* a start with a long clear and level run in direction `heading`: returns 1 and the start / heading */
static int find_run(float len, float side, float start[3], float *heading, unsigned *cursor)
{
	unsigned i;
	for (i = *cursor; i < g_objects.count; i++) {
		const object_t *o = &g_objects.obj[i];
		unsigned k;
		float base[3];
		coll_hit h;
		if ((o->flags & OBJ_F_FREE) || o->pos[2] < -50.0f) continue;
		base[0] = o->pos[0]; base[1] = o->pos[1]; base[2] = o->pos[2];
		{
			float a[3] = { base[0], base[1], base[2] + 0.5f }, b[3] = { base[0], base[1], base[2] - 0.6f };
			if (coll_point_in_solid(&g_coll, a) || !coll_ray(&g_coll, a, b, 0, &h)) continue;
			base[2] = h.point[2];
		}
		for (k = 0; k < 36; k++) {
			float ang = (float)k * 0.174533f, t;
			int ok = 1;
			for (t = 0.0f; t <= len && ok; t += 0.25f) {
				float p[3] = { base[0] + cosf(ang) * t, base[1] + sinf(ang) * t, base[2] }, a[3], b[3], w0[3], w1[3];
				a[0] = b[0] = p[0]; a[1] = b[1] = p[1];
				a[2] = p[2] + 0.4f; b[2] = p[2] - 0.4f;
				if (coll_point_in_solid(&g_coll, a) || !coll_ray(&g_coll, a, b, 0, &h) || fabsf(h.point[2] - base[2]) > 0.05f) ok = 0;
				/* walls ahead at two heights and room to each side */
				w0[0] = p[0]; w0[1] = p[1]; w0[2] = base[2] + 0.25f;
				w1[0] = p[0] + cosf(ang) * 0.3f; w1[1] = p[1] + sinf(ang) * 0.3f; w1[2] = w0[2];
				if (coll_ray(&g_coll, w0, w1, 0, &h)) ok = 0;
				w1[0] = p[0] - sinf(ang) * side; w1[1] = p[1] + cosf(ang) * side;
				if (coll_ray(&g_coll, w0, w1, 0, &h)) ok = 0;
				w1[0] = p[0] + sinf(ang) * side; w1[1] = p[1] - cosf(ang) * side;
				if (coll_ray(&g_coll, w0, w1, 0, &h)) ok = 0;
				w0[2] = w1[2] = base[2] + 0.6f;
				w1[0] = p[0] + cosf(ang) * 0.3f; w1[1] = p[1] + sinf(ang) * 0.3f;
				if (coll_ray(&g_coll, w0, w1, 0, &h)) ok = 0;
			}
			if (ok) { start[0] = base[0]; start[1] = base[1]; start[2] = base[2]; *heading = ang; *cursor = i + 1; return 1; }
		}
	}
	return 0;
}

/* a point on open floor of BSP 0 near placement i, for a unit */
static int floor_point(unsigned i, float out[3])
{
	const object_t *o = &g_objects.obj[i];
	coll_hit h;
	float a[3] = { o->pos[0], o->pos[1], o->pos[2] + 0.5f }, b[3] = { o->pos[0], o->pos[1], o->pos[2] - 0.6f };
	unsigned k;
	if ((o->flags & OBJ_F_FREE) || coll_point_in_solid(&g_coll, a) || !coll_ray(&g_coll, a, b, 0, &h)) return 0;
	for (k = 0; k < 5; k++) {                                   /* open space above the floor: no thin ledge or brush edge */
		float q[3] = { h.point[0], h.point[1], h.point[2] + 0.06f + 0.15f * (float)k };
		if (coll_point_in_solid(&g_coll, q)) return 0;
	}
	out[0] = h.point[0]; out[1] = h.point[1]; out[2] = h.point[2] + 0.03f;
	return 1;
}
static int first_floor(float out[3]) { unsigned k; for (k = 0; k < g_objects.count; k++) if (floor_point(k, out)) return 1; return 0; }

static float wrap_diff(float a, float b) { float d = a - b; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f; return d; }
static float xy_dist(const float a[3], const float b[3]) { return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])); }

static unit_control_t ctl0(float yaw) { unit_control_t c; memset(&c, 0, sizeof c); c.aim_yaw = yaw; c.weapon_slot = -1; return c; }

/* ---- 1. attach, tag numbers --------------------------------------------------------------------------------------------------- */
static void test_attach(void)
{
	unsigned i, placed = 0, attached = 0, refused = 0;
	new_scene(1, 3);
	for (i = 0; i < g_objects.count; i++) {
		const object_t *o = &g_objects.obj[i];
		int u;
		if (o->flags & OBJ_F_FREE) continue;
		u = units_attach(&g_units, &g_game, (int)i, 0, 0);
		if (o->type == SCEN_BIPED) {
			placed++;
			if (u >= 0) {
				attached++;
				CHECK(units_of_object(&g_units, (int)i) == u);
				CHECK(units_attach(&g_units, &g_game, (int)i, 0, 0) == u);              /* idempotent */
				CHECK(g_units.u[u].active && g_units.u[u].obj == (int)i);
				CHECK(g_units.u[u].team >= 1 && g_units.u[u].team <= 5);
				CHECK(g_units.u[u].action == UA_IDLE);
			}
		} else if (u < 0) refused++;
		else CHECK(0);
	}
	printf("  attach: %u placed bipeds in BSP 0 attached %u, %u other objects refused\n", placed, attached, refused);
	CHECK(placed >= 1 && attached == placed);
	CHECK(units_attach(&g_units, &g_game, -1, 0, 0) == -1 && units_attach(&g_units, &g_game, OBJ_MAX + 5, 0, 0) == -1);
	CHECK(units_attach(NULL, &g_game, 0, 0, 0) == -1);
	if (attached) {                                                                  /* detach and attach again */
		int obj = -1, u;
		for (i = 0; i < g_units.count; i++) if (g_units.u[i].active) { obj = g_units.u[i].obj; break; }
		u = units_of_object(&g_units, obj);
		units_detach(&g_units, u);
		CHECK(units_of_object(&g_units, obj) == -1 && !units_alive(&g_units, &g_game, u));
		CHECK(units_attach(&g_units, &g_game, obj, 0, 0) >= 0);
		units_detach(&g_units, -1); units_detach(&g_units, UNIT_MAX); units_detach(NULL, 0);
	}
	/* the table fills up and refuses cleanly */
	if (g_nsp) {
		float p[3];
		unsigned made = 0;
		new_scene(1, 3);
		first_floor(p);
		for (i = 0; i < UNIT_MAX + 8; i++) {
			int obj = objects_spawn_def(&g_objects, &g_ow, g_sp[0].def, p, 0.0f);
			if (obj < 0) break;
			if (units_attach(&g_units, &g_game, obj, 0, 0) >= 0) made++;
		}
		CHECK(made == UNIT_MAX);
	}
}

static void test_tags(void)
{
	unsigned i, level;
	float p[3] = { 0, 0, 0 };
	first_floor(p);
	for (level = 0; level < 4; level++) {
		new_scene(2, (int)level);
		for (i = 0; i < g_nsp; i++) {
			const unitdef_biped_def *b = &g_sp[i].b;
			int u = spawn_unit(g_sp[i].def, p, 0.0f, 0, 0);
			const unit_t *ut;
			const game_unit_t *gu;
			float hs, ss;
			CHECK(u >= 0);
			if (u < 0) continue;
			ut = &g_units.u[u]; gu = &g_game.unit_state[ut->obj];
			CHECK(ut->team == b->default_team);
			CHECK(fabsf(ut->pp.run_speed - b->anim_speed[0]) < 1e-5f);
			CHECK(b->anim_speed[1] <= 0.0f || fabsf(ut->pp.back_speed - b->anim_speed[1]) < 1e-5f);
			CHECK(b->anim_speed[2] <= 0.0f || fabsf(ut->pp.strafe_speed - b->anim_speed[2]) < 1e-5f);
			CHECK(b->anim_speed[3] <= 0.0f || fabsf(ut->pp.crouch_speed - b->anim_speed[3]) < 1e-5f);
			CHECK(fabsf(ut->turn_speed - b->turn_speed) < 1e-5f);
			CHECK(b->radius <= 0.0f || fabsf(ut->pp.radius - b->radius) < 1e-5f);
			CHECK(ut->weapon == g_sp[i].widx);
			CHECK(ut->wst.mode == WEAP_MODE_IDLE);
			if (units_teams_hostile(ut->team, 1)) {
				hs = unitdef_difficulty(&g_unit, UNITDEF_D_ENEMY_VITALITY, level); ss = unitdef_difficulty(&g_unit, UNITDEF_D_ENEMY_SHIELD, level);
			} else {
				hs = unitdef_difficulty(&g_unit, UNITDEF_D_FRIEND_VITALITY, level); ss = unitdef_difficulty(&g_unit, UNITDEF_D_FRIEND_SHIELD, level);
			}
			CHECK(gu->has_vitals);
			CHECK(fabsf(gu->vitals.max_health - b->health * hs) < 0.01f * b->health * hs + 1e-3f);
			CHECK(fabsf(gu->vitals.max_shield - b->shield * ss) < 0.01f * b->shield * ss + 1e-3f);
			CHECK(gu->vitals.health == gu->vitals.max_health && gu->vitals.shield == gu->vitals.max_shield);
			CHECK(gu->ally == (ut->team == 1 || ut->team == 2));
			if (level == 3)
				printf("  tags (team %u, legendary): run %.2f back %.2f side %.2f crouch %.2f wu/s, turn %.2f rad/s, health %.0f (tag %.0f x%.2f), shield %.0f (tag %.0f x%.2f), damage x%.2f, rate x%.2f\n",
				       ut->team, (double)ut->pp.run_speed, (double)ut->pp.back_speed, (double)ut->pp.strafe_speed, (double)ut->pp.crouch_speed, (double)ut->turn_speed,
				       (double)gu->vitals.max_health, (double)b->health, (double)hs, (double)gu->vitals.max_shield, (double)b->shield, (double)ss, (double)gu->dmg_scale, (double)ut->rate_mult);
		}
	}
	printf("  units_t: %zu bytes (%zu per unit), UNIT_MAX %d\n", units_state_size(), sizeof(unit_t), UNIT_MAX);
	CHECK(units_state_size() == sizeof(units_t) && units_state_size() <= 80u * 1024u);
	{                                                                               /* the team table */
		int a, b;
		for (a = -1; a <= 7; a++) for (b = -1; b <= 7; b++) {
			CHECK(units_teams_hostile(a, b) == units_teams_hostile(b, a));
			if (a == b || a <= 0 || b <= 0) CHECK(!units_teams_hostile(a, b));
		}
		CHECK(!units_teams_hostile(1, 2) && units_teams_hostile(3, 1) && units_teams_hostile(3, 2) && units_teams_hostile(4, 1) && units_teams_hostile(4, 2) && units_teams_hostile(4, 3));
		CHECK(!units_teams_hostile(5, 3) && units_teams_hostile(5, 4) && !units_teams_hostile(9, 3));
	}
}

/* ---- 2. speeds and turn rate ----------------------------------------------------------------------------------------------------- */
/* Ground speed (wu/s) of animation k of the unit's graph: its root motion over one cycle, 0 when it has none. The tags' `anim_speed` are the standing ones; a unit in the alert
 * posture, crouching or holding another weapon moves at the pace of the animation it plays (ADR-083). */
static float anim_ground(int unit, int k)
{
	const object_t *o = obj_of(unit);
	anim_graph gr;
	anim_info in;
	float sx = 0.0f, sy = 0.0f;
	uint32_t f;
	if (k < 0 || o->graph < 0) return 0.0f;
	gr = anim_get_graph(&g_anims, (uint32_t)o->graph);
	if ((unsigned)k >= gr.anim_count) return 0.0f;
	in = anim_get(&g_anims, &gr, (uint32_t)k);
	if (!in.root_motion || in.frame_count < 2) return 0.0f;
	for (f = 0; f < in.frame_count; f++) { float d[4]; anim_root_motion(&g_anims, &gr, (uint32_t)k, f, d); sx += d[0]; sy += d[1]; }
	return sqrtf(sx * sx + sy * sy) * 30.0f / (float)in.frame_count;
}

/* `*expect` goes in as the tag's speed and comes out as the ground speed of the move animation `dir` (0 front 1 back 2 left 3 right) the unit plays once it has settled in its posture */
static float speed_test(unsigned sp, float fwd, float left, int crouch, float *expect, float offset, unsigned dir)
{
	float start[3], heading, measured = 0.0f;
	unsigned cursor = 0;
	int tries = 0, ok = 0;
	while (!ok && tries < 12 && find_run(6.0f, 0.5f, start, &heading, &cursor)) {
		int u, t, ground = 1;
		unit_control_t c;
		float p0[3], p1[3], yaw = heading - offset;
		tries++;
		new_scene(3, 3);
		start[2] += 0.03f;
		u = spawn_unit(g_sp[sp].def, start, yaw, 0, -1);
		if (u < 0) return 0.0f;
		c = ctl0(yaw);
		c.flags = crouch ? UC_CROUCH : 0u;
		units_set_control(&g_units, u, &c);
		step(15);                                                       /* settle and crouch */
		{ float gs = anim_ground(u, units_lookup_anim(&g_units, &g_game, u, UA_MOVE, dir, NULL)); if (gs > 0.05f) *expect = gs; }
		c.throttle_fwd = fwd; c.throttle_left = left;
		units_set_control(&g_units, u, &c);
		step(12);                                                       /* accelerate */
		memcpy(p0, g_units.u[u].body.pos, sizeof p0);
		for (t = 0; t < 45; t++) { step(1); if (!g_units.u[u].body.on_ground) ground = 0; }
		memcpy(p1, g_units.u[u].body.pos, sizeof p1);
		measured = xy_dist(p0, p1) / (45.0f / 30.0f);
		ok = ground && measured > 0.5f * *expect && !in_solid(&g_units.u[u]);   /* a blocked run is not a measurement: try another start */
	}
	return ok ? measured : -1.0f;
}

static void test_speeds(void)
{
	unsigned i;
	for (i = 0; i < g_nsp; i++) {
		const unitdef_biped_def *b = &g_sp[i].b;
		struct { float fwd, left; int crouch; float expect, offset; unsigned dir; const char *name; } m[5] = {
			{ 1.0f, 0.0f, 0, b->anim_speed[0], 0.0f, 0, "run front" }, { -1.0f, 0.0f, 0, b->anim_speed[1], 3.14159265f, 1, "run back" },
			{ 0.0f, 1.0f, 0, b->anim_speed[2], 1.5707963f, 2, "strafe left" }, { 0.0f, -1.0f, 0, b->anim_speed[2], -1.5707963f, 3, "strafe right" },
			{ 1.0f, 0.0f, 1, b->anim_speed[3], 0.0f, 0, "crouch front" } };
		unsigned k;
		printf("  speeds (team %u, measured/ground speed of the animation played wu/s):", b->default_team);
		for (k = 0; k < 5; k++) {
			float v;
			if (m[k].expect <= 0.0f) { printf(" %s n/a;", m[k].name); continue; }
			v = speed_test(i, m[k].fwd, m[k].left, m[k].crouch, &m[k].expect, m[k].offset, m[k].dir);
			printf(" %s %.2f/%.2f;", m[k].name, (double)v, (double)m[k].expect);
			CHECK(v > 0.0f && fabsf(v / m[k].expect - 1.0f) < 0.10f);
		}
		printf("\n");
	}
}

static void test_speeds_lod(void)
{
	unsigned i;
	g_lod = 0.001f;                                        /* every unit is beyond the LOD distance: bodies integrate every other tick */
	for (i = 0; i < g_nsp; i++) {
		const unitdef_biped_def *b = &g_sp[i].b;
		float ef = b->anim_speed[0], es = b->anim_speed[2];
		float v = speed_test(i, 1.0f, 0.0f, 0, &ef, 0.0f, 0), vs = speed_test(i, 0.0f, 1.0f, 0, &es, 1.5707963f, 2);
		printf("  speeds with the distance LOD (team %u): run front %.2f/%.2f, strafe %.2f/%.2f wu/s\n", b->default_team, (double)v, (double)ef, (double)vs, (double)es);
		CHECK(v > 0.0f && fabsf(v / ef - 1.0f) < 0.10f);
		CHECK(es <= 0.0f || (vs > 0.0f && fabsf(vs / es - 1.0f) < 0.10f));
	}
	g_lod = 0.0f;
}

static void test_turn(void)
{
	unsigned i;
	float p[3] = { 0, 0, 0 };
	first_floor(p);
	for (i = 0; i < g_nsp; i++) {
		int u, t, n = 0;
		unit_control_t c;
		new_scene(4, 3);
		u = spawn_unit(g_sp[i].def, p, 0.0f, 0, -1);
		CHECK(u >= 0);
		if (u < 0) continue;
		step(10);
		c = ctl0(3.1f);
		units_set_control(&g_units, u, &c);
		for (t = 0; t < 200; t++) {
			step(1); n++;
			if (fabsf(fabsf(g_units.u[u].body.yaw) - 3.1f) < 0.002f) break;
		}
		{
			float rate = 3.1f / ((float)n / 30.0f), tag = g_sp[i].b.turn_speed;
			printf("  turn (team %u): 3.1 rad in %d ticks = %.2f rad/s, tag %.2f\n", g_sp[i].b.default_team, n, (double)rate, (double)tag);
			CHECK(fabsf(rate / tag - 1.0f) < 0.10f);
		}
	}
}

/* ---- 3. scripted walks through BSP 0 ------------------------------------------------------------------------------------------------ */
static void script_controls(unsigned unit, uint32_t tick, unit_control_t *c)
{
	uint32_t phase = tick / 40u + unit * 7u;
	float r = hrand(phase, unit);
	memset(c, 0, sizeof *c);
	c->weapon_slot = -1;
	c->aim_yaw = (hrand(phase, unit + 100u) * 2.0f - 1.0f) * 3.1f;
	c->throttle_fwd = r < 0.75f ? 1.0f : r < 0.85f ? -1.0f : 0.0f;
	c->throttle_left = hrand(phase, unit + 200u) < 0.25f ? (hrand(phase, unit + 300u) < 0.5f ? 1.0f : -1.0f) : 0.0f;
	if (hrand(phase, unit + 400u) < 0.15f) c->flags |= UC_CROUCH;
	if (hrand(phase, unit + 500u) < 0.05f && (tick % 40u) < 2u) c->flags |= UC_JUMP;
	if (hrand(phase, unit + 600u) < 0.2f) c->flags |= UC_WALK;
}

static void test_walk(void)
{
	unsigned i, j, nu = 0, k;
	int ids[24];
	float start_pos[24][3], travelled[24];
	unsigned solid_ticks = 0;
	new_scene(5, 3);
	k = 0;
	for (i = 0; i < g_nsp && nu < 24; i++)
		for (j = 0; j < 3 && nu < 24; j++) {
			float p[3];
			while (k < g_objects.count && !floor_point(k, p)) k++;
			if (k >= g_objects.count) break;
			ids[nu] = spawn_unit(g_sp[i].def, p, (float)j, 0, 0);
			CHECK(ids[nu] >= 0);
			if (ids[nu] < 0) continue;
			memcpy(start_pos[nu], g_units.u[ids[nu]].body.pos, sizeof start_pos[nu]);
			travelled[nu] = 0.0f;
			nu++; k += 5;
		}
	{
		uint32_t t;
		for (t = 0; t < 3000; t++) {
			unsigned a;
			for (a = 0; a < nu; a++) {
				unit_control_t c;
				if (ids[a] < 0) continue;
				script_controls(a, t, &c);
				units_set_control(&g_units, ids[a], &c);
			}
			step(1);
			for (a = 0; a < nu; a++) {
				const unit_t *u = &g_units.u[ids[a]];
				if (ids[a] < 0) continue;
				if (in_solid(u)) { if (solid_ticks++ < 6) printf("    in solid: tick %u unit %u pos %.3f %.3f %.3f action %u crouch %.2f ground %d\n", (unsigned)t, a, (double)u->body.pos[0], (double)u->body.pos[1], (double)u->body.pos[2], u->action, (double)u->body.crouch, u->body.on_ground); }
				CHECK(u->body.pos[0] == u->body.pos[0] && u->body.pos[2] == u->body.pos[2] && fabsf(u->body.pos[2]) < 1000.0f);
			}
		}
	}
	{
		float total = 0.0f;
		unsigned a, moved = 0;
		for (a = 0; a < nu; a++) {
			travelled[a] = xy_dist(start_pos[a], g_units.u[ids[a]].body.pos);
			total += travelled[a];
			if (travelled[a] > 1.0f) moved++;
		}
		printf("  walk: %u units x 3000 ticks of scripted controls, %u ticks inside solid space, %u units ended more than 1 wu from their start (mean %.1f wu)\n", nu, solid_ticks, moved, (double)(total / (float)(nu ? nu : 1)));
		CHECK(solid_ticks == 0);
		CHECK(moved >= nu / 2);
	}
}

/* ---- 4. animation selection ------------------------------------------------------------------------------------------------------- */
static void tour(unsigned spi)
{
	float start[3], heading;
	unsigned cursor = 0;
	int u, ov = 0, objs_before;
	unit_control_t c;
	const unit_t *ut;
	object_t *o;
	char fire_note[64];
	if (!find_run(5.0f, 1.6f, start, &heading, &cursor)) { CHECK(0); return; }
	new_scene(9, 3);
	start[2] += 0.03f;
	u = spawn_unit(g_sp[spi].def, start, heading, 0, 0);
	CHECK(u >= 0);
	if (u < 0) return;
	ut = &g_units.u[u]; o = obj_of(u);
	g_units.u[u].grenades[g_units.u[u].grenade_type] = 3;
	c = ctl0(heading);
	units_set_control(&g_units, u, &c);
	printf("  animations of a team %u unit (default weapon):\n", ut->team);
	step(8);
	printf("    idle        %-34s action %d\n", anim_name(o), ut->action);
	CHECK(ut->action == UA_IDLE && strstr(anim_name(o), "idle"));
	c.throttle_fwd = 1.0f; units_set_control(&g_units, u, &c); step(20);
	printf("    move front  %-34s action %d speed %.2f\n", anim_name(o), ut->action, (double)sqrtf(ut->body.vel[0] * ut->body.vel[0] + ut->body.vel[1] * ut->body.vel[1]));
	CHECK(ut->action == UA_MOVE && strstr(anim_name(o), "move-front"));
	c.throttle_fwd = -1.0f; units_set_control(&g_units, u, &c); step(24);
	printf("    move back   %-34s\n", anim_name(o)); CHECK(strstr(anim_name(o), "move-back") != NULL);
	c.throttle_fwd = 0.0f; c.throttle_left = 1.0f; units_set_control(&g_units, u, &c); step(24);
	printf("    move left   %-34s\n", anim_name(o)); CHECK(strstr(anim_name(o), "move-left") != NULL);
	c.throttle_left = -1.0f; units_set_control(&g_units, u, &c); step(24);
	printf("    move right  %-34s\n", anim_name(o)); CHECK(strstr(anim_name(o), "move-right") != NULL);
	c.throttle_left = 0.0f; c.flags = UC_CROUCH; units_set_control(&g_units, u, &c); step(30);
	printf("    crouch idle %-34s posture %u\n", anim_name(o), ut->posture);
	CHECK(ut->posture == 1 && strstr(anim_name(o), "idle"));
	c.throttle_fwd = 1.0f; units_set_control(&g_units, u, &c); step(24);
	printf("    crouch move %-34s\n", anim_name(o)); CHECK(strstr(anim_name(o), "move") != NULL);
	c.throttle_fwd = 0.0f; c.flags = 0; units_set_control(&g_units, u, &c); step(30);
	/* in place turn */
	c.aim_yaw = heading + 2.0f; units_set_control(&g_units, u, &c); step(3);
	printf("    turn        %-34s\n", anim_name(o));
	step(30);
	/* fire: overlay animations stay overlays (the base animation keeps playing) */
	c.aim_yaw = ut->body.yaw; c.flags = UC_FIRE; units_set_control(&g_units, u, &c);
	{
		unsigned shots0 = (unsigned)g_game.st_shots + (unsigned)g_game.st_projectiles;
		int saw_fire = 0, t;
		for (t = 0; t < 30; t++) { step(1); if (ut->action == UA_FIRE) saw_fire = 1; }
		snprintf(fire_note, sizeof fire_note, "%s", lookup_name(u, UA_FIRE, 0, &ov));
		printf("    fire        %-34s overlay %d, base meanwhile '%s', action seen %d, projectiles %u\n", fire_note, ov, anim_name(o), saw_fire, (unsigned)g_game.st_projectiles);
		CHECK(saw_fire && (unsigned)g_game.st_projectiles + (unsigned)g_game.st_shots > shots0);
	}
	c.flags = 0; units_set_control(&g_units, u, &c); step(10);
	/* reload */
	{
		weap_weapon w;
		weap_get_weapon(&g_weap, (uint32_t)ut->weapon, &w);
		printf("    reload      lookup %-27s ", lookup_name(u, UA_RELOAD, 0, &ov));
		if (w.magazine_count > 0 && ut->wst.loaded[0] < w.magazine[0].rounds_loaded_max) {
			c.flags = UC_RELOAD; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c);
			step(2);
			printf("played '%s' action %d rate %.2f\n", anim_name(o), ut->action, (double)ut->anim_rate);
			CHECK(ut->action == UA_RELOAD && ut->wst.mode == WEAP_MODE_RELOADING);
			step(200);
			CHECK(ut->wst.mode == WEAP_MODE_IDLE && ut->wst.loaded[0] == w.magazine[0].rounds_loaded_max && ut->action != UA_RELOAD);
		} else printf("(weapon has no magazine to reload)\n");
	}
	/* melee */
	c.flags = UC_MELEE; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c);
	step(2);
	printf("    melee       %-34s action %d (lookup '%s')\n", anim_name(o), ut->action, lookup_name(u, UA_MELEE, 0, &ov));
	CHECK(ut->action == UA_MELEE);
	step(80); CHECK(ut->action == UA_IDLE);
	/* grenade */
	{
		unsigned thr0 = (unsigned)g_game.st_throws, n0 = ut->grenades[ut->grenade_type];
		c.aim_pitch = 0.4f; c.flags = UC_GRENADE; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c);
		step(2);
		printf("    throw       %-34s action %d\n", anim_name(o), ut->action);
		CHECK(ut->action == UA_THROW);
		step(90);
		CHECK((unsigned)g_game.st_throws == thr0 + 1 && ut->grenades[ut->grenade_type] == n0 - 1 && ut->action == UA_IDLE);
		c.aim_pitch = 0.0f; units_set_control(&g_units, u, &c);
	}
	/* land: a fall from a height */
	{
		float hi[3] = { ut->body.pos[0], ut->body.pos[1], ut->body.pos[2] + 1.2f };
		int t, saw = 0;
		units_teleport(&g_units, &g_game, u, hi, ut->body.yaw);
		for (t = 0; t < 60; t++) { step(1); if (ut->action == UA_AIRBORNE && !saw) { saw = 1; printf("    airborne    %-34s\n", anim_name(o)); } if (ut->action == UA_LAND) break; }
		CHECK(saw);
		if (ut->action == UA_LAND) printf("    land        %-34s\n", anim_name(o));
		step(80);
	}
	/* flinch: a big (not lethal) health hit */
	{
		game_unit_t *gu = &g_game.unit_state[ut->obj];
		float big;
		gu->vitals.health = gu->vitals.max_health; gu->vitals.shield = gu->vitals.max_shield; gu->vitals.flags = 0; gu->dead = 0;     /* the grenade hurt it */
		step(2);
		big = gu->vitals.max_shield + 0.2f * gu->vitals.max_health;
		units_damage(&g_units, &g_game, u, big);
		step(2);
		printf("    flinch      %-34s action %d health %.1f/%.1f\n", anim_name(o), ut->action, (double)gu->vitals.health, (double)gu->vitals.max_health);
		CHECK(ut->action == UA_FLINCH || gu->vitals.health >= gu->vitals.max_health * 0.8f || strcmp(lookup_name(u, UA_FLINCH, 0, &ov), "-") == 0);
		step(100);
	}
	/* death */
	objs_before = (int)g_objects.count;
	units_kill(&g_units, &g_game, u, 0);
	CHECK(!units_alive(&g_units, &g_game, u) && ut->action == UA_DEAD);
	step(2);
	printf("    death       %-34s action %d (lookup front '%s')\n", anim_name(o), ut->action, lookup_name(u, UA_DEAD, 0, &ov));
	CHECK(strstr(anim_name(o), "kill") || strstr(anim_name(o), "dead") || strstr(anim_name(o), "death"));
	{
		float p0[3], f0;
		int dropped = (int)g_objects.count - objs_before;
		step(60);
		memcpy(p0, ut->body.pos, sizeof p0); f0 = o->frame;
		step(60);
		CHECK(xy_dist(p0, ut->body.pos) < 1e-4f && fabsf(p0[2] - ut->body.pos[2]) < 1e-4f);         /* the corpse stays */
		CHECK(o->frame >= f0 && ut->action == UA_DEAD);
		{
			weap_weapon wd;
			int has_def = weap_get_weapon(&g_weap, (uint32_t)ut->weapon, &wd) && scen_find_def(&g_scen, wd.tag_id) >= 0;
			printf("    corpse stays (frame %.0f); weapon pickup dropped: %d (the map %s an object definition for it)\n", (double)o->frame, dropped, has_def ? "has" : "has no");
			CHECK(dropped == (has_def ? 1 : 0));
			if (has_def) {
				const object_t *dr = &g_objects.obj[g_objects.count - 1];
				CHECK(dr->type == SCEN_WEAPON && xy_dist(dr->pos, ut->body.pos) < 0.01f);
			}
		}
	}
}

static void test_animations(void)
{
	int a = sp_by_team(2), b = sp_by_team(3);
	CHECK(a >= 0 || b >= 0);
	if (a >= 0) tour((unsigned)a);
	if (b >= 0) tour((unsigned)b);
	else if (g_nsp > 1) tour(g_nsp - 1);
}

/* ---- 5. weapons and damage ---------------------------------------------------------------------------------------------------------- */
static void aim_at_unit(unit_control_t *c, const unit_t *from, const unit_t *to)
{
	float ex = from->body.pos[0], ey = from->body.pos[1], ez = from->body.pos[2] + from->pp.eye_height;
	float tx = to->body.pos[0], ty = to->body.pos[1], tz = to->body.pos[2] + 0.35f * to->pp.height;
	float dx = tx - ex, dy = ty - ey, dz = tz - ez;
	c->aim_yaw = atan2f(dy, dx);
	c->aim_pitch = atan2f(dz, sqrtf(dx * dx + dy * dy));
}

static void test_combat(void)
{
	float start[3], heading;
	unsigned cursor = 0;
	int a = sp_by_team(2), b = sp_by_team(3), u, v, t, killed = 0;
	unit_control_t c;
	if (a < 0) a = 0;
	if (b < 0) b = (int)g_nsp - 1;
	if (!find_run(5.5f, 0.8f, start, &heading, &cursor)) { CHECK(0); return; }
	new_scene_empty(7, 3);
	park_player();
	{
		float far_pos[3] = { start[0] + cosf(heading) * 4.0f, start[1] + sinf(heading) * 4.0f, start[2] };
		start[2] += 0.03f; far_pos[2] += 0.03f;
		u = spawn_unit(g_sp[a].def, start, heading, 0, 0);
		v = spawn_unit(g_sp[b].def, far_pos, heading + 3.14159f, 0, -1);
	}
	CHECK(u >= 0 && v >= 0);
	if (u < 0 || v < 0) return;
	step(5);
	CHECK(units_teams_hostile(g_units.u[u].team, g_units.u[v].team) || g_units.u[u].team == g_units.u[v].team);
	/* one unit shoots the other until it dies: shots, projectiles, hits, damage, kill */
	{
		const game_unit_t *gv = &g_game.unit_state[g_units.u[v].obj];
		float h0 = gv->vitals.health + gv->vitals.shield;
		uint32_t shots0 = g_game.st_shots, hits0 = g_game.st_hits, proj0 = g_game.st_projectiles, fired0 = g_units.u[u].wst.rounds_fired;
		unsigned fire_ev0 = g_evtotal[UE_FIRE], death_ev0 = g_evtotal[UE_DEATH];
		c = ctl0(heading);
		for (t = 0; t < 900 && !killed; t++) {
			aim_at_unit(&c, &g_units.u[u], &g_units.u[v]);
			c.flags = UC_FIRE;
			units_set_control(&g_units, u, &c);
			step(1);
			if (!units_alive(&g_units, &g_game, v)) killed = 1;
		}
		step(2);
		printf("  combat: %u shots %u projectiles %u hits, target %.0f -> %.0f health+shield, %s in %d ticks, rounds fired %u\n", (unsigned)(g_game.st_shots - shots0), (unsigned)(g_game.st_projectiles - proj0),
		       (unsigned)(g_game.st_hits - hits0), (double)h0, (double)(gv->vitals.health + gv->vitals.shield), killed ? "killed" : "alive", t, (unsigned)(g_units.u[u].wst.rounds_fired - fired0));
		CHECK(g_game.st_shots > shots0 && g_game.st_projectiles >= proj0 + (g_game.st_shots - shots0) && g_game.st_hits > hits0);
		CHECK(killed && gv->dead && g_units.u[v].action == UA_DEAD && g_units.u[u].wst.rounds_fired > fired0);
		CHECK(g_game.st_kills >= 1);
		CHECK(g_evtotal[UE_FIRE] - fire_ev0 == g_game.st_shots - shots0 && g_evtotal[UE_DEATH] - death_ev0 == 1);       /* one UE_FIRE per shot, one UE_DEATH */
	}
	/* endless fight against an invulnerable target: the magazine empties, the unit reloads by itself and keeps firing */
	{
		weap_weapon w;
		int reloads = 0, prev_mode = 0, was_empty = 0;
		weap_get_weapon(&g_weap, (uint32_t)g_units.u[u].weapon, &w);
		new_scene_empty(8, 3);
	park_player();
		u = spawn_unit(g_sp[a].def, start, heading, 0, 0);
		{
			float far_pos[3] = { start[0] + cosf(heading) * 4.0f, start[1] + sinf(heading) * 4.0f, start[2] + 0.03f };
			v = spawn_unit(g_sp[b].def, far_pos, heading, 0, -1);
		}
		CHECK(u >= 0 && v >= 0);
		if (u >= 0 && v >= 0) {
			uint32_t fired0, hits0 = g_game.st_hits;
			unsigned reload_ev0 = g_evtotal[UE_RELOAD];
			g_units.u[v].flags |= UF_INVULNERABLE;
			c = ctl0(heading);
			fired0 = g_units.u[u].wst.rounds_fired;
			for (t = 0; t < 700; t++) {
				aim_at_unit(&c, &g_units.u[u], &g_units.u[v]);
				c.flags = UC_FIRE;
				units_set_control(&g_units, u, &c);
				step(1);
				if (g_units.u[u].wst.mode == WEAP_MODE_RELOADING && prev_mode != WEAP_MODE_RELOADING) { reloads++; if (g_units.u[u].action == UA_RELOAD) was_empty++; }
				prev_mode = g_units.u[u].wst.mode;
			}
			printf("  endless fight: %u rounds fired (magazine %u), %d reloads, %d with action UA_RELOAD, %u hits, target alive %d\n", (unsigned)(g_units.u[u].wst.rounds_fired - fired0),
			       w.magazine_count ? (unsigned)w.magazine[0].rounds_loaded_max : 0u, reloads, was_empty, (unsigned)(g_game.st_hits - hits0), units_alive(&g_units, &g_game, v));
			CHECK(units_alive(&g_units, &g_game, v) && g_game.st_hits > hits0);
			CHECK(g_evtotal[UE_RELOAD] - reload_ev0 == (unsigned)reloads);
			if (w.magazine_count && w.magazine[0].rounds_loaded_max > 0 && w.magazine[0].rounds_loaded_max < 150) {
				CHECK(g_units.u[u].wst.rounds_fired - fired0 > w.magazine[0].rounds_loaded_max);
				CHECK(reloads >= 1 && was_empty == reloads);
			}
		}
	}
	/* the player is hit by a hostile unit's shots (and not by an ally's) */
	{
		float far_pos[3] = { start[0] + cosf(heading) * 4.0f, start[1] + sinf(heading) * 4.0f, start[2] + 0.03f };
		int team_for[2] = { 3, 2 }, k;
		for (k = 0; k < 2; k++) {
			float v0, v1;
			new_scene_empty(10, 3);
	park_player();
			g_game.vitals.max_health = g_game.vitals.health = 75.0f; g_game.vitals.max_shield = g_game.vitals.shield = 75.0f;
			game_teleport(&g_game, far_pos, heading + 3.14159f);
			u = spawn_unit(g_sp[a].def, start, heading, team_for[k], 0);
			CHECK(u >= 0);
			if (u < 0) continue;
			step(30);                                              /* the player settles on the floor */
			v0 = g_game.vitals.health + g_game.vitals.shield;
			c = ctl0(heading);
			for (t = 0; t < 180; t++) {
				float eye[3], dx, dy, dz;
				player_eye(&g_game.player, &g_game.pp, eye);
				dx = eye[0] - g_units.u[u].body.pos[0]; dy = eye[1] - g_units.u[u].body.pos[1]; dz = eye[2] - (g_units.u[u].body.pos[2] + g_units.u[u].pp.eye_height);
				c.aim_yaw = atan2f(dy, dx); c.aim_pitch = atan2f(dz, sqrtf(dx * dx + dy * dy));
				c.flags = UC_FIRE;
				units_set_control(&g_units, u, &c);
				step(1);
			}
			v1 = g_game.vitals.health + g_game.vitals.shield;
			printf("  player target: shooter team %d, player health+shield %.1f -> %.1f\n", team_for[k], (double)v0, (double)v1);
			if (team_for[k] == 3) CHECK(v1 < v0 - 1.0f);
			else CHECK(v1 >= v0 - 0.01f);
		}
	}
}

static void test_grenade_melee(void)
{
	float start[3], heading;
	unsigned cursor = 0;
	int a = sp_by_team(3), u, v, t, detonated = 0;
	unit_control_t c;
	if (a < 0) a = 0;
	if (!find_run(5.0f, 0.6f, start, &heading, &cursor)) { CHECK(0); return; }
	/* grenade: thrown along the aim, owner = the unit, it detonates */
	new_scene_empty(11, 3);
	park_player();
	start[2] += 0.03f;
	u = spawn_unit(g_sp[a].def, start, heading, 0, 0);
	CHECK(u >= 0);
	if (u >= 0) {
		unsigned n0, th0 = (unsigned)g_game.st_throws, throw_ev0 = g_evtotal[UE_THROW];
		g_units.u[u].grenades[0] = 2; g_units.u[u].grenade_type = 0;
		n0 = g_units.u[u].grenades[0];
		c = ctl0(heading); c.aim_pitch = 0.5f;
		units_set_control(&g_units, u, &c);
		step(2);
		c.flags = UC_GRENADE; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c);
		for (t = 0; t < 300 && !detonated; t++) {
			unsigned i, n;
			const game_event_t *ev;
			step(1);
			ev = game_events(&g_game, &n);
			for (i = 0; i < n; i++) if (ev[i].type == GE_DETONATE) detonated = 1;
		}
		printf("  grenade: throws %u, grenades %u -> %u, detonated %d after %d ticks\n", (unsigned)(g_game.st_throws - th0), n0, g_units.u[u].grenades[0], detonated, t);
		CHECK(g_game.st_throws == th0 + 1 && g_units.u[u].grenades[0] == n0 - 1 && detonated && g_evtotal[UE_THROW] == throw_ev0 + 1);
		/* no grenades left: nothing is thrown */
		g_units.u[u].grenades[0] = 0;
		step(80);
		c.flags = UC_GRENADE; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c); step(2);
		CHECK(g_units.u[u].action != UA_THROW && g_units.u[u].grenades[0] == 0);
	}
	/* melee: a blow lands in front of the unit and hurts a target unit; a hostile one hurts the player */
	{
		weap_weapon w;
		new_scene_empty(12, 3);
	park_player();
		{
			float tgt[3] = { start[0] + cosf(heading) * 0.75f, start[1] + sinf(heading) * 0.75f, start[2] };
			u = spawn_unit(g_sp[a].def, start, heading, 0, 0);
			v = spawn_unit(g_sp[a].def, tgt, heading + 3.14159f, 2, -1);
		}
		CHECK(u >= 0 && v >= 0);
		if (u >= 0 && v >= 0) {
			float h0;
			unsigned melee_ev0 = g_evtotal[UE_MELEE];
			weap_get_weapon(&g_weap, (uint32_t)g_units.u[u].weapon, &w);
			step(10);
			h0 = g_game.unit_state[g_units.u[v].obj].vitals.health + g_game.unit_state[g_units.u[v].obj].vitals.shield;
			c = ctl0(heading);
			c.flags = UC_MELEE; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c);
			CHECK(g_units.u[u].action == UA_MELEE);
			step(70);
			{
				float h1 = g_game.unit_state[g_units.u[v].obj].vitals.health + g_game.unit_state[g_units.u[v].obj].vitals.shield;
				printf("  melee: target health+shield %.1f -> %.1f (weapon melee damage tag %s)\n", (double)h0, (double)h1, w.melee_damage ? "present" : "absent");
				if (w.melee_damage) CHECK(h1 < h0);
				CHECK(g_evtotal[UE_MELEE] == melee_ev0 + 1);
			}
			/* unarmed units hit with the biped's own melee damage; friends are not hit */
			g_game.vitals.max_health = g_game.vitals.health = 75.0f; g_game.vitals.max_shield = g_game.vitals.shield = 75.0f;
			{
				float pp[3] = { g_units.u[u].body.pos[0] + cosf(heading) * 0.7f, g_units.u[u].body.pos[1] + sinf(heading) * 0.7f, g_units.u[u].body.pos[2] };
				float p0;
				units_kill(&g_units, &g_game, v, 1);
				game_teleport(&g_game, pp, heading + 3.14159f);
				step(20);
				p0 = g_game.vitals.health + g_game.vitals.shield;
				c.flags = UC_MELEE; units_set_control(&g_units, u, &c); step(1); c.flags = 0; units_set_control(&g_units, u, &c);
				step(70);
				printf("  melee on the player: %.1f -> %.1f\n", (double)p0, (double)(g_game.vitals.health + g_game.vitals.shield));
				if (w.melee_damage) CHECK(g_game.vitals.health + g_game.vitals.shield < p0);
			}
		}
	}
}

static void test_damage_death(void)
{
	float p[3];
	int u, a = sp_by_team(3);
	unit_control_t c;
	if (a < 0) a = 0;
	first_floor(p);
	new_scene(13, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	CHECK(u >= 0);
	if (u < 0) return;
	step(5);
	{
		const game_unit_t *gu = &g_game.unit_state[g_units.u[u].obj];
		float total = gu->vitals.health + gu->vitals.shield, dealt;
		CHECK(units_alive(&g_units, &g_game, u));
		dealt = units_damage(&g_units, &g_game, u, 10.0f);
		CHECK(dealt > 9.9f && fabsf(gu->vitals.health + gu->vitals.shield - (total - dealt)) < 0.01f);
		CHECK(units_damage(&g_units, &g_game, u, -5.0f) == 0.0f && units_damage(&g_units, &g_game, -1, 5.0f) == 0.0f && units_damage(NULL, &g_game, 0, 5.0f) == 0.0f);
		/* invulnerable: a lethal hit does nothing */
		g_units.u[u].flags |= UF_INVULNERABLE;
		CHECK(units_damage(&g_units, &g_game, u, 1e5f) == 0.0f);
		game_damage_object(&g_game, g_units.u[u].obj, 1e5f, NULL);                      /* the game itself kills: units.c revives the invulnerable */
		step(2);
		CHECK(units_alive(&g_units, &g_game, u) && g_units.u[u].action != UA_DEAD);
		g_units.u[u].flags &= (uint8_t)~UF_INVULNERABLE;
		/* killed by the game (a projectile): the unit notices, plays the death animation and holds it */
		game_damage_object(&g_game, g_units.u[u].obj, 1e5f, NULL);
		CHECK(gu->dead);
		step(2);
		CHECK(g_units.u[u].action == UA_DEAD && g_units.u[u].dying && !units_alive(&g_units, &g_game, u));
		CHECK(g_units.died == 1);
		units_kill(&g_units, &g_game, u, 0); units_kill(&g_units, &g_game, u, 1);        /* again: harmless */
		CHECK(g_units.died == 1);
		c = ctl0(1.0f); c.throttle_fwd = 1.0f; c.flags = UC_FIRE | UC_JUMP;
		units_set_control(&g_units, u, &c);
		{
			float p0[3];
			memcpy(p0, g_units.u[u].body.pos, sizeof p0);
			step(60);
			CHECK(xy_dist(p0, g_units.u[u].body.pos) < 1e-4f);                          /* a corpse does not walk */
		}
	}
	/* a silent kill and a kill without vitals */
	new_scene(14, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	if (u >= 0) {
		step(3);
		{
			unsigned hits0 = (unsigned)g_game.ev_count;
			units_kill(&g_units, &g_game, u, 1);
			CHECK(!units_alive(&g_units, &g_game, u) && g_game.unit_state[g_units.u[u].obj].dead && g_game.ev_count == hits0);
		}
	}
	/* fall damage: a long fall hurts or kills, the landing is soft or hard */
	new_scene(15, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	if (u >= 0) {
		float hi[3] = { p[0], p[1], p[2] + 8.0f }, h0;
		const game_unit_t *gu = &g_game.unit_state[g_units.u[u].obj];
		int t;
		step(10);
		h0 = gu->vitals.health + gu->vitals.shield;
		units_teleport(&g_units, &g_game, u, hi, 0.0f);
		for (t = 0; t < 150; t++) step(1);
		printf("  fall of 8 wu: health+shield %.1f -> %.1f, dead %d\n", (double)h0, (double)(gu->vitals.health + gu->vitals.shield), gu->dead);
		CHECK(gu->dead || gu->vitals.health + gu->vitals.shield < h0);
		CHECK(g_units.u[u].body.on_ground);
	}
}

/* ---- 5b. permutations and postures of the cutscene's actors (ADR-083) ----------------------------------------------------------------- */
static void test_permutation_pool(void)
{
	uint32_t m, found = 0;
	CHECK(model_name_number("sgt_johnson-100") == 100 && model_name_number("head_charles_helmet-10") == 10 && model_name_number("damaged torso-102") == 102);
	CHECK(model_name_number("__base") == 0 && model_name_number("base") == 0 && model_name_number("x-") == 0);
	CHECK(model_name_number("a-12345") == 0 && model_name_number("a-007") == 7 && model_name_number("") == 0 && model_name_number("head2") == 0);
	for (m = 0; m < g_models.model_count; m++) {
		model_info mi = model_get(&g_models, m);
		uint16_t sel[16], first[16];
		uint32_t p, r, seed, heads[16], nheads = 0, johnson = UINT32_MAX;
		char nm[MODEL_NAME_LEN];
		if (mi.region_count == 0 || mi.region_count > 16) continue;
		for (p = 0; p < mi.permutation_count; p++) { model_perm_name(&g_models, &mi, p, nm); if (!strcmp(nm, "sgt_johnson-100")) johnson = p; }
		if (johnson == UINT32_MAX) continue;
		found++;
		model_select_variant(&g_models, &mi, 0, 0, first);                                /* the first permutation of every region: the base pieces */
		for (r = 0; r < mi.region_count; r++) { model_perm_name(&g_models, &mi, first[r], nm); CHECK(model_name_number(nm) == 0 || model_name_number(nm) < 100); }
		model_select_variant(&g_models, &mi, 100, 7, sel);                                 /* Sergeant Johnson: the scenario's desired permutation 100 */
		for (r = 0, p = 0; r < mi.region_count; r++) { model_perm_name(&g_models, &mi, sel[r], nm); if (model_name_number(nm) == 100) p++; }
		CHECK(p >= 2);                                                                     /* his arms (region 0) and his head */
		model_select_variant(&g_models, &mi, 101, 7, sel);                                 /* the cap: region 1 only */
		for (r = 0, p = 0; r < mi.region_count; r++) { model_perm_name(&g_models, &mi, sel[r], nm); if (model_name_number(nm) == 101) p++; }
		CHECK(p == 1);
		for (seed = 0; seed < 300; seed++) {                                               /* the random pool (1..99): never Johnson, the cap or the damaged torso */
			uint16_t again[16];
			model_select_variant(&g_models, &mi, 0, seed * 2654435761u, sel);
			model_select_variant(&g_models, &mi, 0, seed * 2654435761u, again);
			for (r = 0; r < mi.region_count; r++) {
				model_perm_name(&g_models, &mi, sel[r], nm);
				CHECK(sel[r] == again[r] && model_name_number(nm) < 100);                  /* stable per seed */
				if (r == 1 && sel[r] != first[r]) { uint32_t k = 0; while (k < nheads && heads[k] != sel[r]) k++; if (k == nheads && nheads < 16) heads[nheads++] = sel[r]; }
			}
		}
		printf("  permutations of the marine model: %u distinct heads over 300 seeds, Johnson is permutation %u\n", nheads, johnson);
		CHECK(nheads >= 3);
	}
	CHECK(found >= 1);
}

/* An actor stands in the alert block when a script says so (unit_set_seat unit "alert" sets UF_ALERT), and then it moves at the pace of the alert move animation */
static void test_alert_posture(void)
{
	unsigned i, tried = 0, named = 0;
	for (i = 0; i < g_nsp; i++) {
		float p[3], start[3], heading;
		unsigned cursor = 0;
		int u, k, t, ground = 1;
		unit_control_t c;
		float p0[3], p1[3], measured, expect;
		ansm_graph gr;
		if (!find_run(6.0f, 0.5f, start, &heading, &cursor)) break;
		memcpy(p, start, sizeof p); p[2] += 0.03f;
		new_scene(17, 3);
		u = spawn_unit(g_sp[i].def, p, heading, 0, 0);                                      /* armed: stands at the ready */
		if (u < 0) continue;
		step(10);
		CHECK(g_units.u[u].posture == 0);
		if (ansm_find_graph(&g_ansm, g_units.u[u].anim_tag, &gr) != 0 || ansm_find_unit(&g_ansm, &gr, "alert") < 0) continue;       /* a graph without an alert block */
		tried++;
		g_units.u[u].flags |= UF_ALERT;
		c = ctl0(heading);
		units_set_control(&g_units, u, &c);
		step(15);
		CHECK(g_units.u[u].posture == 2);
		k = units_lookup_anim(&g_units, &g_game, u, UA_IDLE, 0, NULL);
		if (k >= 0 && !strncmp(lookup_name(u, UA_IDLE, 0, NULL), "alert", 5)) named++;
		k = units_lookup_anim(&g_units, &g_game, u, UA_MOVE, 0, NULL);
		expect = anim_ground(u, k);
		c.throttle_fwd = 1.0f;
		units_set_control(&g_units, u, &c);
		step(12);
		memcpy(p0, g_units.u[u].body.pos, sizeof p0);
		for (t = 0; t < 45; t++) { step(1); if (!g_units.u[u].body.on_ground) ground = 0; }
		memcpy(p1, g_units.u[u].body.pos, sizeof p1);
		measured = xy_dist(p0, p1) / (45.0f / 30.0f);
		printf("  alert posture (team %u): '%s', %.2f wu/s measured, %.2f wu/s from the animation, %.2f wu/s tag run speed\n", g_sp[i].b.default_team, lookup_name(u, UA_MOVE, 0, NULL),
		       (double)measured, (double)expect, (double)g_sp[i].b.anim_speed[0]);
		if (ground && !in_solid(&g_units.u[u]) && expect > 0.05f) CHECK(fabsf(measured / expect - 1.0f) < 0.10f);
		if (expect > 0.05f && g_sp[i].b.anim_speed[0] > 0.05f) CHECK(measured < g_sp[i].b.anim_speed[0] * 1.1f);                   /* never faster than the standing run it replaces here */
		g_units.u[u].flags &= (uint8_t)~UF_ALERT;
		c.throttle_fwd = 0.0f;
		units_set_control(&g_units, u, &c);
		step(15);
		CHECK(g_units.u[u].posture == 0 && !strncmp(lookup_name(u, UA_IDLE, 0, NULL), "stand", 5));
	}
	CHECK(tried >= 1 && named >= 1);                                                       /* the marines' graph has an alert idle */
}

/* ---- 6. variants, cinedef, custom animations, rebind ------------------------------------------------------------------------------------- */
static void test_variant(void)
{
	float p[3];
	int u, a = sp_by_team(3);
	aidef_variant_def v;
	if (a < 0) a = 0;
	first_floor(p);
	new_scene(16, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 3, 0);
	CHECK(u >= 0);
	if (u < 0) return;
	memset(&v, 0, sizeof v);
	v.tag_id = 0x1234u; v.body_vitality = 50.0f; v.shield_vitality = 30.0f; v.projectile_error = 0.04f; v.weapon_damage_modifier = 1.5f;
	v.grenade_type = 1; v.grenade_count[0] = 2; v.grenade_count[1] = 4;
	CHECK(units_apply_variant(&g_units, &g_game, u, &v) == 1);
	{
		const game_unit_t *gu = &g_game.unit_state[g_units.u[u].obj];
		float hs = unitdef_difficulty(&g_unit, UNITDEF_D_ENEMY_VITALITY, 3), ss = unitdef_difficulty(&g_unit, UNITDEF_D_ENEMY_SHIELD, 3);
		float es = unitdef_difficulty(&g_unit, UNITDEF_D_PROJECTILE_ERROR, 3), dm = unitdef_difficulty(&g_unit, UNITDEF_D_ENEMY_DAMAGE, 3);
		CHECK(g_units.u[u].variant_tag == 0x1234u);
		CHECK(fabsf(gu->vitals.max_health - 50.0f * hs) < 0.01f && fabsf(gu->vitals.max_shield - 30.0f * ss) < 0.01f && gu->vitals.health == gu->vitals.max_health);
		CHECK(fabsf(g_units.u[u].proj_error - 0.04f * es) < 1e-6f);
		CHECK(fabsf(gu->dmg_scale - 1.5f * dm) < 1e-5f);
		CHECK(g_units.u[u].grenade_type == 1 && g_units.u[u].grenades[1] >= 2 && g_units.u[u].grenades[1] <= 4 && g_units.u[u].grenades[0] == 0);
		printf("  variant: health %.1f shield %.1f, projectile error %.4f rad, damage x%.2f, %u plasma grenades\n", (double)gu->vitals.max_health, (double)gu->vitals.max_shield, (double)g_units.u[u].proj_error, (double)gu->dmg_scale, g_units.u[u].grenades[1]);
	}
	CHECK(units_apply_variant(&g_units, &g_game, -1, &v) == 0 && units_apply_variant(&g_units, &g_game, u, NULL) == 0);
	/* an explicit weapon of the variant */
	{
		unsigned w;
		for (w = 0; w < g_weap.n_weapons; w++) {
			weap_weapon wd;
			weap_get_weapon(&g_weap, w, &wd);
			if ((int)w != g_units.u[u].weapon && wd.trigger_count > 0 && wd.trigger[0].projectile) {
				v.weapon_tag = wd.tag_id;
				units_apply_variant(&g_units, &g_game, u, &v);
				CHECK(g_units.u[u].weapon == (int)w);
				break;
			}
		}
	}
	units_give_weapon(&g_units, &g_game, u, 0u);                                       /* unknown weapon tag: unchanged */
	units_give_weapon(&g_units, &g_game, -1, g_weap.n_weapons ? 1u : 0u);
}

static void test_cinedef(void)
{
	float p[3];
	int u, a = sp_by_team(2), t;
	int16_t cine_yaw = 0;
	cinedef_ctl c;
	if (a < 0) a = 0;
	{
		float heading;
		unsigned cursor = 0;
		if (!find_run(4.0f, 0.6f, p, &heading, &cursor)) { CHECK(0); return; }
		new_scene(17, 3);
		p[2] += 0.03f;
		u = spawn_unit(g_sp[a].def, p, heading, 0, 0);
		cine_yaw = (int16_t)(heading / 0.00314159265f);
		if (cine_yaw >= 1000) cine_yaw -= 2000;
	}
	CHECK(u >= 0);
	if (u < 0) return;
	memset(&c, 0, sizeof c);
	c.throttle[0] = 1.0f; c.throttle[1] = 0.5f; c.yaw = 500; c.pitch = -100; c.weapon = -1;
	c.flags = CINEDEF_CF_CROUCH | CINEDEF_CF_PRIMARY_FIRE | CINEDEF_CF_GRENADE | CINEDEF_CF_ACTION | CINEDEF_CF_FORCE_ALERT;
	units_apply_cinedef(&g_units, u, &c);
	{
		const unit_control_t *k = &g_units.u[u].ctl;
		CHECK(k->throttle_fwd == 1.0f && k->throttle_left == 0.5f);
		CHECK(fabsf(k->aim_yaw - 500.0f * 0.00314159265f) < 1e-5f && fabsf(k->aim_pitch + 100.0f * 0.00314159265f) < 1e-5f);
		CHECK(k->flags == (UC_CROUCH | UC_FIRE | UC_GRENADE));
	}
	c.flags = CINEDEF_CF_SECONDARY_FIRE | CINEDEF_CF_JUMP | CINEDEF_CF_MELEE | CINEDEF_CF_RELOAD | CINEDEF_CF_LOOK_DONT_TURN | CINEDEF_CF_EXCHANGE_WEAPON;
	c.weapon = 1;
	units_apply_cinedef(&g_units, u, &c);
	CHECK(g_units.u[u].ctl.flags == (UC_FIRE2 | UC_JUMP | UC_MELEE | UC_RELOAD) && g_units.u[u].ctl.weapon_slot == 1);
	units_apply_cinedef(&g_units, -1, &c); units_apply_cinedef(NULL, 0, &c); units_apply_cinedef(&g_units, u, NULL);
	/* a recording that walks: the aim yaw stays (500 counts = 90 degrees), the unit walks that way */
	{
		float p0[3];
		c.flags = 0; c.throttle[0] = 1.0f; c.throttle[1] = 0.0f; c.pitch = 0; c.yaw = cine_yaw;
		step(10);
		memcpy(p0, g_units.u[u].body.pos, sizeof p0);
		for (t = 0; t < 60; t++) { units_apply_cinedef(&g_units, u, &c); step(1); }
		printf("  cinedef: walked %.2f wu along the recorded aim (yaw %.2f, recorded %.2f)\n", (double)xy_dist(p0, g_units.u[u].body.pos), (double)g_units.u[u].body.yaw, (double)cine_yaw * 0.00314159265);
		CHECK(fabsf(wrap_diff(g_units.u[u].body.yaw, (float)cine_yaw * 0.00314159265f)) < 0.05f && g_units.u[u].action == UA_MOVE && xy_dist(p0, g_units.u[u].body.pos) > 1.0f);
	}
	/* NaN and absurd controls are sanitised */
	{
		unit_control_t k = ctl0(0.0f);
		k.throttle_fwd = NAN; k.throttle_left = 1e30f; k.aim_yaw = INFINITY; k.aim_pitch = -1e9f;
		units_set_control(&g_units, u, &k);
		CHECK(g_units.u[u].ctl.throttle_fwd == 0.0f && g_units.u[u].ctl.throttle_left == 0.0f && g_units.u[u].ctl.aim_yaw == g_units.u[u].ctl.aim_yaw && fabsf(g_units.u[u].ctl.aim_pitch) < 1.0f);
		step(30);
		CHECK(g_units.u[u].body.pos[0] == g_units.u[u].body.pos[0] && !in_solid(&g_units.u[u]));
	}
}

static void test_custom(void)
{
	float p[3];
	int u, a = sp_by_team(2), k;
	object_t *o;
	if (a < 0) a = 0;
	first_floor(p);
	new_scene(18, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	CHECK(u >= 0);
	if (u < 0) return;
	o = obj_of(u);
	step(10);
	k = units_lookup_anim(&g_units, &g_game, u, UA_MELEE, 0, NULL);
	if (k < 0) k = units_lookup_anim(&g_units, &g_game, u, UA_THROW, 0, NULL);
	CHECK(k >= 0);
	if (k >= 0) {
		anim_graph gr = anim_get_graph(&g_anims, (uint32_t)o->graph);
		unsigned frames = anim_get(&g_anims, &gr, (uint32_t)k).frame_count, ticks, t = 0;
		ticks = units_custom_animation(&g_units, &g_game, u, -1, k, 2.0f, 0);
		printf("  custom animation '%s': %u frames at 2x = %u ticks\n", anim_name(o), frames, ticks);
		CHECK(ticks >= frames / 2 - 1 && ticks <= frames / 2 + 1 && g_units.u[u].action == UA_CUSTOM && o->anim == k);
		while (g_units.u[u].action == UA_CUSTOM && t < 400) { step(1); t++; }
		CHECK(t >= ticks - 1 && t <= ticks + 2 && g_units.u[u].action != UA_CUSTOM);
		step(3);
		CHECK(o->anim != k && (strstr(anim_name(o), "idle") || strstr(anim_name(o), "airborne")));       /* the melee lunge's root motion may carry the unit off the test floor */
		/* held until stopped */
		ticks = units_custom_animation(&g_units, &g_game, u, -1, k, 1.0f, 1);
		step((int)ticks + 40);
		CHECK(g_units.u[u].action == UA_CUSTOM && o->anim == k);
		units_stop_custom(&g_units, &g_game, u);
		step(3);
		CHECK(g_units.u[u].action != UA_CUSTOM && strstr(anim_name(o), "idle"));
		/* bad arguments */
		CHECK(units_custom_animation(&g_units, &g_game, u, -1, 30000, 1.0f, 0) == 0);
		CHECK(units_custom_animation(&g_units, &g_game, u, 30000, 0, 1.0f, 0) == 0);
		CHECK(units_custom_animation(&g_units, &g_game, -1, -1, 0, 1.0f, 0) == 0);
		CHECK(units_custom_animation(&g_units, &g_game, u, -1, -3, 1.0f, 0) == 0);
		units_stop_custom(&g_units, &g_game, -1);
		/* a custom animation of another graph of the map and back */
		if (g_anims.graph_count > 1) {
			unsigned gi;
			for (gi = 0; gi < g_anims.graph_count; gi++) {
				anim_graph og = anim_get_graph(&g_anims, gi);
				if ((int)gi == o->graph || og.anim_count == 0) continue;
				{
					int own = o->graph;
					unsigned tk = units_custom_animation(&g_units, &g_game, u, (int)gi, 0, 1.0f, 0);
					CHECK(tk > 0 && o->graph == (int)gi);
					step((int)tk + 3);
					CHECK(o->graph == own && g_units.u[u].action != UA_CUSTOM);
				}
				break;
			}
		}
		/* death ends a custom animation */
		units_custom_animation(&g_units, &g_game, u, -1, k, 1.0f, 1);
		units_kill(&g_units, &g_game, u, 0);
		step(2);
		CHECK(g_units.u[u].action == UA_DEAD && g_units.u[u].custom_graph == -1);
	}
}

static void test_flags(void)
{
	float p[3], q[3] = { 0, 0, 0 }, hs[3];
	int u, v, w, a = sp_by_team(3), t;
	unsigned k, n = 0;
	unit_control_t c;
	object_t *o;
	if (a < 0) a = 0;
	first_floor(p);
	for (k = 0; k < g_objects.count; k++) if (floor_point(k, hs) && xy_dist(hs, p) > 8.0f) { memcpy(q, hs, sizeof q); n = 1; break; }
	new_scene(25, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	v = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	w = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	CHECK(u >= 0 && v >= 0 && w >= 0);
	if (u < 0 || v < 0 || w < 0) return;
	step(5);
	/* UF_PLAYER units are not simulated (the player's body is placed by the mission), UF_HIDE_BODY hides the object */
	g_units.u[u].flags |= UF_PLAYER;
	g_units.u[v].flags |= UF_HIDE_BODY;
	c = ctl0(0.0f); c.throttle_fwd = 1.0f;
	units_set_control(&g_units, u, &c); units_set_control(&g_units, v, &c);
	{
		float u0[3];
		memcpy(u0, g_units.u[u].body.pos, sizeof u0);
		step(30);
		CHECK(xy_dist(u0, g_units.u[u].body.pos) < 1e-6f && g_units.u[u].action == UA_IDLE);
		CHECK((obj_of(v)->flags & OBJ_F_HIDDEN) != 0 && (obj_of(u)->flags & OBJ_F_HIDDEN) == 0);
	}
	/* teleport: body and object move at once, the unit falls to the floor there and stands */
	if (n) {
		o = obj_of(w);
		q[2] += 0.5f;
		units_teleport(&g_units, &g_game, w, q, 1.0f);
		CHECK(fabsf(o->pos[0] - q[0]) < 1e-5f && fabsf(o->pos[2] - q[2]) < 1e-5f && fabsf(g_units.u[w].body.yaw - 1.0f) < 1e-5f && g_units.u[w].ctl.aim_yaw == g_units.u[w].body.yaw);
		for (t = 0; t < 40; t++) step(1);
		CHECK(g_units.u[w].body.on_ground && xy_dist(g_units.u[w].body.pos, q) < 0.01f && g_units.u[w].body.pos[2] < q[2] && !in_solid(&g_units.u[w]));
		CHECK(fabsf(o->pos[2] - g_units.u[w].body.pos[2]) < 1e-5f);
	}
	/* UF_NO_DROP: no pickup object */
	{
		unsigned before = g_objects.count;
		g_units.u[w].flags |= UF_NO_DROP;
		units_kill(&g_units, &g_game, w, 0);
		step(3);
		CHECK(g_objects.count == before);
	}
	/* the unit table is rebuilt from the units: a lost by_obj entry comes back */
	{
		int obj = g_units.u[v].obj;
		g_units.by_obj[obj] = -1;
		CHECK(units_rebind(&g_units, &g_game) >= 2 && units_of_object(&g_units, obj) == v);
	}
}

static void test_robust(void)
{
	float p[3];
	int u, i, a = sp_by_team(3);
	if (a < 0) a = 0;
	first_floor(p);
	new_scene(19, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	CHECK(u >= 0);
	if (u < 0) return;
	/* the object disappears under a unit: the unit detaches, ticks never touch it */
	step(5);
	{
		int obj = g_units.u[u].obj;
		objects_destroy(&g_objects, (unsigned)obj);
		units_rebind(&g_units, &g_game);
		CHECK(!g_units.u[u].active && units_of_object(&g_units, obj) == -1);
		step(3);
	}
	new_scene(20, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	{
		int obj = g_units.u[u].obj;
		objects_destroy(&g_objects, (unsigned)obj);
		step(3);                                                                       /* units_tick notices by itself */
		CHECK(!g_units.u[u].active);
		units_set_control(&g_units, u, &(unit_control_t){ 0 });
		units_teleport(&g_units, &g_game, u, p, 0.0f);
		units_give_weapon(&g_units, &g_game, u, g_weap.n_weapons ? 1u : 0u);
		units_kill(&g_units, &g_game, u, 0);
		CHECK(units_damage(&g_units, &g_game, u, 5.0f) == 0.0f && !units_alive(&g_units, &g_game, u));
		{ float e[3], d[3]; units_eye(&g_units, u, e, d); units_eye(&g_units, -5, e, d); units_eye(NULL, 0, e, d); }
		CHECK(units_lookup_anim(&g_units, &g_game, u, UA_IDLE, 0, NULL) == -1 && units_lookup_anim(&g_units, &g_game, 9999, UA_IDLE, 0, NULL) == -1);
	}
	/* a game without animation tables, weapons or world collision still ticks */
	new_scene(21, 3);
	u = spawn_unit(g_sp[a].def, p, 0.0f, 0, 0);
	if (u >= 0) {
		const ansm_section *sav = g_game.ansm;
		g_game.ansm = NULL;
		for (i = 0; i < 40; i++) {
			unit_control_t c = ctl0((float)i * 0.3f);
			c.throttle_fwd = 1.0f; c.flags = UC_FIRE | UC_MELEE | UC_GRENADE | UC_RELOAD | UC_CROUCH;
			units_set_control(&g_units, u, &c);
			step(1);
		}
		CHECK(g_units.u[u].body.pos[0] == g_units.u[u].body.pos[0]);
		g_game.ansm = sav;
		game_set_collision(&g_game, NULL);
		for (i = 0; i < 30; i++) { units_tick(&g_units, &g_game); objects_tick(&g_objects, &g_ow); }       /* (game_tick itself needs a world) */
		game_set_collision(&g_game, &g_coll);
		step(30);
	}
	/* random API calls, mixed with ticks, never crash */
	new_scene(22, 3);
	for (i = 0; i < 400; i++) {
		unsigned r = hash_u32((uint32_t)i * 977u);
		int target = (int)(r % 14u) - 2;
		unit_control_t c = ctl0(hrand((uint32_t)i, 1u) * 6.0f - 3.0f);
		switch (r >> 8 & 7u) {
		case 0: if (g_nsp) { u = spawn_unit(g_sp[r % g_nsp].def, p, 0.0f, (int)(r >> 12 & 7u), (r & 1u) ? 0 : -1); (void)u; } break;
		case 1: c.throttle_fwd = hrand((uint32_t)i, 2u) * 2.0f - 1.0f; c.flags = r >> 16 & 0x3FFu; units_set_control(&g_units, target, &c); break;
		case 2: units_damage(&g_units, &g_game, target, hrand((uint32_t)i, 3u) * 60.0f); break;
		case 3: units_kill(&g_units, &g_game, target, (int)(r >> 20 & 1u)); break;
		case 4: units_teleport(&g_units, &g_game, target, p, hrand((uint32_t)i, 4u) * 6.0f); break;
		case 5: units_custom_animation(&g_units, &g_game, target, -1, (int)(r % 40u), 1.0f, (int)(r >> 3 & 1u)); break;
		case 6: units_stop_custom(&g_units, &g_game, target); break;
		default: units_detach(&g_units, target); break;
		}
		step(2);
	}
	CHECK(1);
}

/* ---- 7. 96 units: tick cost, then snapshot and restore ------------------------------------------------------------------------------------ */
static unsigned populate(unsigned want, unsigned teams_mask)
{
	unsigned n = 0, k = 0, ring = 0;
	while (n < want && g_nsp) {
		float p[3];
		int u;
		if (k >= g_objects.count) { k = 0; ring++; if (ring > 6) break; }
		if (floor_point(k, p)) {
			p[0] += 0.4f * (float)ring * cosf((float)(n * 2)); p[1] += 0.4f * (float)ring * sinf((float)(n * 2));
			u = spawn_unit(g_sp[n % g_nsp].def, p, (float)n, (int)(2 + (n & 1u) + (teams_mask & 1u)), 0);
			if (u >= 0) n++;
		}
		k++;
	}
	return n;
}

static void drive_all(units_t *s, uint32_t tick, int fight)
{
	unsigned i;
	for (i = 0; i < s->count; i++) {
		unit_control_t c;
		if (!s->u[i].active) continue;
		script_controls(i, tick, &c);
		if (fight && hrand(tick / 20u, i + 900u) < 0.5f) c.flags |= UC_FIRE;
		if (fight && hrand(tick / 90u, i + 950u) < 0.1f && (tick % 90u) < 2u) c.flags |= UC_MELEE;
		units_set_control(s, (int)i, &c);
	}
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void drive_mixed(units_t *s, uint32_t tick)
{
	unsigned i;
	for (i = 0; i < s->count; i++) {
		unit_control_t c;
		if (!s->u[i].active) continue;
		memset(&c, 0, sizeof c);
		c.weapon_slot = -1; c.aim_yaw = s->u[i].body.yaw;
		if (i % 6u == 0) script_controls(i, tick, &c);                                   /* a sixth walks */
		else if (i % 6u == 1) { c.aim_yaw = (float)(i % 7u); if (hrand(tick / 15u, i) < 0.6f) c.flags |= UC_FIRE; }   /* a sixth stands and shoots */
		units_set_control(s, (int)i, &c);
	}
}

static double run_ticks_timed(int mode, uint32_t t0, uint32_t n)
{
	uint32_t t;
	double acc = 0.0;
	for (t = t0; t < t0 + n; t++) {
		double a;
		if (mode == 0) { /* idle: controls untouched */ }
		else if (mode == 3) drive_mixed(&g_units, t);
		else drive_all(&g_units, t, mode == 2);
		a = now_ms();
		units_tick(&g_units, &g_game);
		acc += now_ms() - a;
		game_tick(&g_game, &g_in0); objects_tick(&g_objects, &g_ow);
	}
	return acc / (double)n;
}

static void test_perf(void)
{
	unsigned n, i;
	double idle_ms, moving_ms, fight_ms, mixed_ms, lod_ms;
	int bench = getenv("UNITS_BENCH") != NULL;
	new_scene_empty(23, 3);
	park_player();
	{                                                                                  /* populate on the placements of BSP 0 */
		unsigned keep = g_objects.count;
		(void)keep;
	}
	objects_clear(&g_objects);
	objects_spawn_bsp(&g_objects, &g_ow, 0, 0);
	game_init(&g_game, &g_gi, g_pstart, 0.0f);
	g_game.difficulty = 3;
	g_game.vitals.health = g_game.vitals.max_health = 1e6f; g_game.vitals.shield = g_game.vitals.max_shield = 1e6f;
	park_player();
	units_init(&g_units);
	n = populate(UNIT_MAX, 0);
	printf("  perf: %u units attached\n", n);
	CHECK(n == UNIT_MAX);
	step(60);
	idle_ms = run_ticks_timed(0, 0, 300);
	moving_ms = run_ticks_timed(1, 0, 600);
	fight_ms = run_ticks_timed(2, 600, 600);
	mixed_ms = run_ticks_timed(3, 1200, 600);
	g_units.lod_distance = 30.0f;                                                      /* the parked player is far from everyone */
	lod_ms = run_ticks_timed(1, 1800, 600);
	g_units.lod_distance = 0.0f;
	{
		unsigned solid = 0, alive = 0, dead = 0;
		for (i = 0; i < g_units.count; i++) { if (!g_units.u[i].active) continue; if (!units_alive(&g_units, &g_game, (int)i)) dead++; else alive++; if (in_solid(&g_units.u[i])) solid++; }
		printf("  perf (%s build), units_tick ms per tick for %u units: all idle %.3f; a sixth walking, a sixth shooting %.3f; all walking %.3f, with the distance LOD %.3f; all walking and shooting %.3f\n",
		       bench ? "optimised" : "sanitizer", n, idle_ms, mixed_ms, moving_ms, lod_ms, fight_ms);
		printf("  perf: %u alive, %u dead after the fights, %u in solid\n", alive, dead, solid);
		CHECK(solid == 0);
	}
	if (bench) {
		CHECK(idle_ms < 0.1 && mixed_ms < 0.5 && lod_ms < 0.5);
		CHECK(moving_ms < 0.8 && fight_ms < 0.8);                                        /* the worst case: reported, bounded */
	} else {
		CHECK(idle_ms < 5.0 && mixed_ms < 25.0 && moving_ms < 40.0 && fight_ms < 60.0 && lod_ms < 40.0);   /* generous: ASan / UBSan instrumented */
	}
}

static uint32_t hash_state(const units_t *s)
{
	uint32_t h = 2166136261u;
	unsigned i;
	const float *f;
#define MIX(v) do { uint32_t w_; memcpy(&w_, &(v), sizeof w_); h = (h ^ w_) * 16777619u; } while (0)
	for (i = 0; i < s->count; i++) {
		const unit_t *u = &s->u[i];
		const object_t *o;
		const game_unit_t *gu;
		unsigned k;
		if (!u->active) continue;
		o = &g_objects.obj[u->obj]; gu = &g_game.unit_state[u->obj];
		for (k = 0; k < 3; k++) { MIX(u->body.pos[k]); MIX(u->body.vel[k]); }
		MIX(u->body.yaw); MIX(u->body.crouch);
		MIX(gu->vitals.health); MIX(gu->vitals.shield);
		f = &o->frame; MIX(*f);
		{ uint32_t w = (uint32_t)o->anim | ((uint32_t)u->action << 16) | ((uint32_t)u->wst.loaded[0] << 24); h = (h ^ w) * 16777619u; }
		{ uint32_t w = u->wst.rounds_fired; h = (h ^ w) * 16777619u; }
		{ uint32_t w = u->rng; h = (h ^ w) * 16777619u; }
	}
	{ uint32_t w = g_game.st_shots + g_game.st_hits * 31u + g_game.st_kills * 1031u + g_game.st_projectiles * 7u; h = (h ^ w) * 16777619u; }
	MIX(g_game.st_damage);
	{ uint32_t w = g_game.rng; h = (h ^ w) * 16777619u; }
	{ uint32_t w = (uint32_t)g_objects.count; h = (h ^ w) * 16777619u; }
#undef MIX
	return h;
}

static units_t s_saved_units, s_copy_units;
static game_t s_saved_game;
static object_table_t s_saved_objects;

static void test_snapshot(void)
{
	unsigned n;
	uint32_t t, tick0, h1, h2, h3;
	new_scene(24, 3);
	n = populate(32, 1);
	CHECK(n == 32);
	g_units.lod_distance = 12.0f;                                                     /* part of the crowd runs at the LOD rate */
	for (t = 0; t < 200; t++) { drive_all(&g_units, t, 1); step(1); }
	/* save */
	memcpy(&s_saved_units, &g_units, sizeof g_units);
	memcpy(&s_saved_game, &g_game, sizeof g_game);
	memcpy(&s_saved_objects, &g_objects, sizeof g_objects);
	tick0 = 200;
	for (t = tick0; t < tick0 + 300; t++) { drive_all(&g_units, t, 1); step(1); }
	h1 = hash_state(&g_units);
	/* restore into the same units_t and play the same 300 ticks */
	memcpy(&g_units, &s_saved_units, sizeof g_units);
	memcpy(&g_game, &s_saved_game, sizeof g_game);
	memcpy(&g_objects, &s_saved_objects, sizeof g_objects);
	CHECK(g_game.objects == &g_objects);
	units_rebind(&g_units, &g_game);
	for (t = tick0; t < tick0 + 300; t++) { drive_all(&g_units, t, 1); step(1); }
	h2 = hash_state(&g_units);
	/* restore into a different, fresh units_t (copied bytes, rebound) */
	memcpy(&g_game, &s_saved_game, sizeof g_game);
	memcpy(&g_objects, &s_saved_objects, sizeof g_objects);
	memset(&s_copy_units, 0xAB, sizeof s_copy_units);
	memcpy(&s_copy_units, &s_saved_units, sizeof s_copy_units);
	CHECK(units_rebind(&s_copy_units, &g_game) == n);
	for (t = tick0; t < tick0 + 300; t++) { drive_all(&s_copy_units, t, 1); step_with(&s_copy_units, 1); }
	h3 = hash_state(&s_copy_units);
	{
		unsigned dead = 0, i;
		for (i = 0; i < g_units.count; i++) if (g_units.u[i].active && !units_alive(&g_units, &g_game, (int)i)) dead++;
		printf("  snapshot: hash after 300 ticks %08x, after restore %08x, in a fresh units_t %08x (%u units, %u dead, %u shots)\n", h1, h2, h3, n, dead, (unsigned)g_game.st_shots);
	}
	CHECK(h1 == h2 && h1 == h3);
	CHECK(g_game.st_shots > 0);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "../../assets/hp2";
	unsigned k;
	if (!load_all(dir)) { printf("skip: no a10 packs in %s\n", dir); return 0; }
	g_ow.scen = &g_scen; g_ow.models = &g_models; g_ow.anims = &g_anims;
	memset(&g_gi, 0, sizeof g_gi);
	g_gi.unit = &g_unit; g_gi.weap = &g_weap; g_gi.cmod = &g_cmod; g_gi.ansm = &g_ansm; g_gi.fx = &g_fx; g_gi.coll = &g_coll; g_gi.ow = g_ow;
	memset(&g_in0, 0, sizeof g_in0);
	objects_clear(&g_objects);
	objects_spawn_bsp(&g_objects, &g_ow, 0, 0);
	for (k = 0; k < g_objects.count; k++) if (g_objects.obj[k].type == SCEN_BIPED) { g_pstart[0] = g_objects.obj[k].pos[0]; g_pstart[1] = g_objects.obj[k].pos[1]; g_pstart[2] = g_objects.obj[k].pos[2] + 0.05f; break; }
	find_species();
	printf("  %u walking species with weapons in the a10 packs\n", g_nsp);
	CHECK(g_nsp >= 3);
	if (g_nsp == 0) { printf("%d failure(s)\n", failures + 1); return 1; }
	test_attach();
	test_tags();
	test_speeds();
	test_speeds_lod();
	test_turn();
	test_walk();
	test_animations();
	test_combat();
	test_grenade_melee();
	test_damage_death();
	test_permutation_pool();
	test_alert_posture();
	test_variant();
	test_cinedef();
	test_custom();
	test_flags();
	test_robust();
	test_perf();
	test_snapshot();
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
