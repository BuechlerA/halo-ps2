/* Host test of the AI runtime (docs/32-ai-runtime.md) and of the `ai` script built-ins on the real a10 packs (skipped when assets/ is missing).
 *   test_ai <assets/hp2 dir> <path pack with PAT0..PAT2>
 * Everything picks its data by properties (an encounter of the humans with armed squads on a BSP, a covenant one, a command list with go-to atoms ...),
 * never by name. Needs the real unit layer (ai/units.c): without it the fixture's stand-in cannot move or shoot and the behavioural tests are skipped.
 *   1. references, spawning (counts of the difficulty, variants, vitals by the difficulty table, pending placement for another BSP)
 *   2. hostile pairs in sight of each other on BSP 0..2: they notice, approach, shoot, take damage and die within 30 s
 *   3. the player as a target; marines following the player; fleeing unarmed actors
 *   4. every command list of the map run on a test actor (atoms advance, counted), a squad following a list to its point
 *   5. conversations through the sound hook
 *   6. cost of 16 actors (host microseconds per tick and per phase), determinism, snapshot / restore
 *   7. the built-ins through the registered script host (return values, effects), random-argument fuzz, a soak with the real a10 script machine */
#include "ai/ai.h"
#include "script/hs_host_ai.h"
#include "script/hs_host_obj.h"
#pragma GCC diagnostic ignored "-Wunused-function"      /* the shared fixture has more helpers than this test uses */
#include "mission_fixture.h"
#include <time.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

#ifdef AI_REAL_UNITS
#define HAVE_UNITS 1
#else
#define HAVE_UNITS 0
#endif

static mission_fixture F;
static ai_t A;
static coll_bsp g_coll[3];
static path_bsp g_pbsp[3];
static path_scratch *g_scratch;
static void *g_scratch_mem;
static int g_nbsp;

static uint32_t hash_bytes(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

/* ---- loading ------------------------------------------------------------------------------------------------------------------------- */
static int load_extra(const char *dir, const char *path_pack)
{
	char p[256];
	unsigned b;
	uint32_t maxs = 0;
	snprintf(p, sizeof p, "%s/a10.coll.hp2", dir);
	for (b = 0; b < 3; b++) {
		char tag[5] = { 'C', 'O', 'L', (char)('0' + b), 0 };
		size_t n;
		uint8_t *d = mfix_section(p, tag, &n);
		if (!d || coll_open(d, n, &g_coll[b]) != COLL_OK) return 0;
	}
	for (b = 0; b < 3; b++) {
		char tag[5] = { 'P', 'A', 'T', (char)('0' + b), 0 };
		size_t n;
		uint8_t *d = mfix_section(path_pack, tag, &n);
		if (!d || path_open(d, n, &g_pbsp[b]) != PATH_OK) return 0;
		if (g_pbsp[b].sector_count > maxs) maxs = g_pbsp[b].sector_count;
	}
	g_scratch = (path_scratch *)malloc(sizeof *g_scratch);
	g_scratch_mem = malloc(path_scratch_bytes(maxs) + 16);
	if (!g_scratch || !g_scratch_mem || !path_scratch_init(g_scratch, g_scratch_mem, path_scratch_bytes(maxs) + 16, maxs)) return 0;
	g_nbsp = 3;
	return 1;
}

/* a clean scene on BSP `bsp`: only the player's body object, no other unit, the AI bound to that BSP's collision and navigation */
static void scene(int bsp, uint32_t seed, int difficulty)
{
	unsigned i;
	ai_env env;
	mfix_start(&F, seed);
	for (i = 0; i < F.units.count; i++) if (F.units.u[i].active) units_detach(&F.units, (int)i);
	for (i = 0; i < F.objects.count; i++) {
		if ((int)i == F.mission.player_obj || (F.objects.obj[i].flags & OBJ_F_FREE)) continue;
		{ /* every placed object goes: the AI's sight sees the BSP only, a crate between two actors would eat the shots */ objects_destroy(&F.objects, i); F.game.unit_state[i].model = -1; F.game.unit_state[i].has_vitals = 0; }
	}
	game_set_collision(&F.game, &g_coll[bsp]);
	F.game.difficulty = difficulty;
	F.mission.cur_bsp = bsp;
	F.mission.difficulty = difficulty;
	memset(&env, 0, sizeof env);
	env.aidef = &F.ai; env.game = &F.game; env.units = &F.units; env.path = &g_pbsp[bsp]; env.scratch = g_scratch; env.bsp = bsp;
	ai_init(&A, &env);
	F.mission.ai = &A;
}

static void world_tick(int with_ai)
{
	game_input_t none;
	memset(&none, 0, sizeof none);
	mission_sync_player(&F.mission);
	if (with_ai) ai_tick(&A);
	units_tick(&F.units, &F.game);
	game_tick(&F.game, &none);
	objects_tick(&F.objects, &F.ow);
	F.mission.tick++;
}

static void run(unsigned ticks) { while (ticks--) world_tick(1); }

/* ---- picking data by properties ------------------------------------------------------------------------------------------------------ */
typedef struct { int enc, bsp; unsigned humans, covenant, armed, total; } einfo;

static unsigned enc_bsp_of(unsigned e)
{
	aidef_encounter_def d;
	if (!aidef_get_encounter(&F.ai, e, &d)) return 99;
	return (d.flags & AIDEF_ENC_MANUAL_BSP_INDEX) ? (unsigned)d.manual_bsp : (unsigned)d.precomputed_bsp;
}

static void enc_info(unsigned e, einfo *o)
{
	aidef_encounter_def d;
	unsigned k;
	memset(o, 0, sizeof *o);
	o->enc = (int)e; o->bsp = (int)enc_bsp_of(e);
	if (!aidef_get_encounter(&F.ai, e, &d)) return;
	for (k = 0; k < d.squad_count; k++) {
		aidef_squad_def sq;
		aidef_variant_def v;
		unitdef_biped_def bd;
		unsigned n;
		if (!aidef_encounter_squad(&F.ai, e, k, &sq) || !aidef_squad_variant(&F.ai, &sq, &v) || !v.unit_tag || sq.delay_time > 0.0f) continue;
		n = sq.start_count && aidef_squad_spawn_count(&sq, 3) > 0 ? aidef_squad_spawn_count(&sq, 3) : 0;
		if (scen_find_def(&F.scen, v.unit_tag) < 0 || !unitdef_biped(&F.unit, v.unit_tag, &bd)) continue;
		o->total += n;
		if (bd.default_team == AIDEF_TEAM_HUMAN) o->humans += n;
		if (bd.default_team == AIDEF_TEAM_COVENANT) o->covenant += n;
		if (v.weapon_tag && v.rate_of_fire >= 0.0f) o->armed += n;
	}
}

/* an armed human encounter and a covenant one on `bsp`, the smallest that fit */
static int pick_pair(int bsp, int *ea, int *eb)
{
	unsigned e, n = aidef_encounter_count(&F.ai);
	int besta = -1, bestb = -1;
	einfo ia, ib, cur;
	for (e = 0; e < n; e++) {
		enc_info(e, &cur);
		if (cur.bsp != bsp) continue;
		if (cur.humans >= 2 && cur.armed >= 2 && cur.humans == cur.total && (besta < 0 || cur.total < ia.total)) { ia = cur; besta = (int)e; }
		if (cur.covenant >= 2 && cur.covenant == cur.total && (bestb < 0 || cur.total < ib.total)) { ib = cur; bestb = (int)e; }
	}
	if (besta < 0 || bestb < 0 || besta == bestb) return 0;
	*ea = besta; *eb = bestb;
	return 1;
}

static ai_ref enc_ref(int e) { ai_ref r; memset(&r, 0, sizeof r); r.kind = 1; r.enc = (int16_t)e; r.squad = r.platoon = -1; return r; }
static ai_ref squad_ref(int e, int s) { ai_ref r = enc_ref(e); r.kind = 2; r.squad = (int16_t)s; return r; }

static int first_actor(const ai_ref *r)
{
	unsigned i;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai_actor_in_ref(&A, &A.st.actor[i], r)) return (int)i;
	return -1;
}

static float dist2(const float a[3], const float b[3]) { float dx = a[0] - b[0], dy = a[1] - b[1]; return sqrtf(dx * dx + dy * dy); }
static const float *upos(int actor) { return F.units.u[A.st.actor[actor].unit].body.pos; }

/* a floor point 5..9 units from `a` with a clear line of sight (eye to eye) and a walkable route; 0 if none found */
static int sight_spot(int bsp, const float a[3], float out[3])
{
	unsigned k;
	path_query q;
	path_query_default(&q);
	for (k = 0; k < 64; k++) {
		float ang = (float)k * 0.83f, d = 5.0f + (float)(k % 5), cand[3], pt[3], e1[3], e2[3], dd;
		coll_hit h;
		cand[0] = a[0] + d * cosf(ang); cand[1] = a[1] + d * sinf(ang); cand[2] = a[2];
		if (path_nearest_sector(&g_pbsp[bsp], cand, 2.0f, &dd, pt) < 0) continue;
		if (dist2(pt, a) < 4.0f || dist2(pt, a) > 10.0f || fabsf(pt[2] - a[2]) > 1.5f) continue;
		e1[0] = a[0]; e1[1] = a[1]; e1[2] = a[2] + 0.6f; e2[0] = pt[0]; e2[1] = pt[1]; e2[2] = pt[2] + 0.6f;
		if (coll_ray(&g_coll[bsp], e1, e2, 0, &h)) continue;
		if (!path_reachable(&g_pbsp[bsp], g_scratch, &q, a, pt)) continue;
		pt[2] += 0.02f;
		memcpy(out, pt, 3 * sizeof(float));
		return 1;
	}
	return 0;
}

/* ================================================================================================================================================= */
static void test_refs(void)
{
	unsigned e, n = aidef_encounter_count(&F.ai), checked = 0, k;
	ai_ref r, r2;
	scene(0, 1, 3);
	for (e = 0; e < n && e < AI_MAX_ENCOUNTERS; e++) {
		aidef_encounter_def d;
		CHECK(aidef_get_encounter(&F.ai, e, &d));
		r = enc_ref((int)e);
		CHECK(ai_ref_decode(&A, ai_ref_encode(&r), &r2) && r2.kind == 1 && r2.enc == (int16_t)e);
		for (k = 0; k < d.squad_count; k++) {
			unsigned gs;
			r = squad_ref((int)e, (int)k);
			CHECK(ai_ref_decode(&A, ai_ref_encode(&r), &r2) && r2.kind == 2 && r2.squad == (int16_t)k && r2.enc == (int16_t)e);
			CHECK(ai_ref_squad_range(&A, &r2, &gs, &checked) && checked == 1 && gs == d.first_squad + k);
			CHECK(ai_ref_has_squad(&A, &r2, gs) && !ai_ref_has_squad(&A, &r2, gs + 1u));
		}
		r = squad_ref((int)e, (int)d.squad_count);                    /* one past the end */
		CHECK(!ai_ref_decode(&A, ai_ref_encode(&r), &r2));
		if (d.platoon_count) {
			r = enc_ref((int)e); r.kind = 3; r.platoon = (int16_t)(d.platoon_count - 1);
			CHECK(ai_ref_decode(&A, ai_ref_encode(&r), &r2) && r2.kind == 3);
			r.platoon = (int16_t)d.platoon_count;
			CHECK(!ai_ref_decode(&A, ai_ref_encode(&r), &r2));
		}
	}
	CHECK(!ai_ref_decode(&A, 0xFFFFFFFFu, &r2));
	CHECK(!ai_ref_decode(&A, n, &r2));
	CHECK(!ai_ref_decode(&A, 0x00010000u, &r2));                       /* a stray bit pattern */
	printf("  refs: %u encounters round-trip\n", n);
}

static void test_spawn(void)
{
	int bsp, ea, eb, any = 0;
	for (bsp = 0; bsp < g_nbsp; bsp++) {
		einfo ia;
		unsigned i;
		int made, es;
		float st_hp = 0.0f;
		ai_ref r;
		if (!pick_pair(bsp, &ea, &eb)) continue;
		any = 1;
		scene(bsp, 7, 3);
		enc_info((unsigned)ea, &ia);
		r = enc_ref(ea);
		CHECK(ai_living_count(&A, &r) == 0 && ai_living_fraction(&A, &r) == 0.0f);
		made = ai_place(&A, &r);
		CHECK(made == (int)ia.total);                                      /* the counts of Legendary, placed squads without delay */
		CHECK(ai_living_count(&A, &r) == (unsigned)made);
		CHECK(ai_living_fraction(&A, &r) == 1.0f);
		CHECK(ai_place(&A, &r) == 0);                                      /* placing again does nothing while they live */
		es = 0;
		for (i = 0; i < AI_MAX_ACTORS; i++) {
			const ai_actor *a = &A.st.actor[i];
			const unit_t *u;
			const vitals_t *v;
			aidef_variant_def var;
			if (!a->active) continue;
			u = &F.units.u[a->unit];
			v = &F.game.unit_state[a->obj].vitals;
			CHECK(u->active && u->obj == a->obj && !(u->flags & UF_NO_AI));
			CHECK(aidef_actor_variant(&F.ai, u->variant_tag, &var));
			CHECK(v->max_health > 0.0f && v->health == v->max_health);
			CHECK(F.game.unit_state[a->obj].has_vitals && !F.game.unit_state[a->obj].dead);
			/* Legendary (difficulty 3) scales the vitality of enemies / friends by the table's factor */
			{
				float f = unitdef_difficulty(&F.unit, (u->team == AIDEF_TEAM_HUMAN) ? UNITDEF_D_FRIEND_VITALITY : UNITDEF_D_ENEMY_VITALITY, 3);
				if (var.body_vitality > 0.0f) CHECK(fabsf(v->max_health - var.body_vitality * f) < 0.01f * var.body_vitality * f + 0.01f);
			}
			CHECK(dist2(u->body.pos, F.objects.obj[a->obj].pos) < 0.01f);
			CHECK(g_pbsp[bsp].sector_count && path_locate(&g_pbsp[bsp], u->body.pos) >= 0);   /* placed on the navigation mesh */
			CHECK(a->bsp == bsp);
			st_hp += v->max_health;
			es++;
		}
		CHECK(es == made);
		/* kill + erase, then they can be placed again */
		ai_kill(&A, &r, 0);
		CHECK(ai_living_count(&A, &r) == 0);
		ai_erase(&A, &r);
		CHECK(ai_place(&A, &r) == made);
		ai_erase(&A, &r);
		CHECK(ai_living_count(&A, &r) == 0 && ai_living_fraction(&A, &r) == 0.0f);
		printf("  spawn bsp %d: %d actors, %.0f total health (Legendary)\n", bsp, made, (double)st_hp);
		/* the difficulty counts */
		{
			int d;
			for (d = 0; d < 4; d++) {
				int cnt;
				scene(bsp, 7, d);
				cnt = ai_place(&A, &r);
				CHECK(cnt > 0);
				if (d == 0) CHECK(cnt <= made + 2);
			}
		}
	}
	CHECK(any);
}

static void test_pending_bsp(void)
{
	int ea, eb;
	ai_ref r;
	einfo ia;
	if (!pick_pair(1, &ea, &eb)) { printf("  pending: no pair on BSP 1, skipped\n"); return; }
	scene(0, 3, 3);
	r = enc_ref(ea);
	enc_info((unsigned)ea, &ia);
	CHECK(ai_place(&A, &r) == 0);                                          /* another BSP is loaded: nothing now, remembered */
	CHECK(A.st.enc[ea].pending && ai_living_count(&A, &r) == 0);
	game_set_collision(&F.game, &g_coll[1]);
	ai_set_bsp(&A, 1, &g_pbsp[1], g_scratch);
	CHECK(!A.st.enc[ea].pending && ai_living_count(&A, &r) == ia.total);   /* placed when its BSP arrives */
	/* actors of the other BSP freeze instead of being driven */
	{
		int ai_i = first_actor(&r);
		float p0[3];
		CHECK(ai_i >= 0);
		memcpy(p0, upos(ai_i), sizeof p0);
		game_set_collision(&F.game, &g_coll[0]);
		ai_set_bsp(&A, 0, &g_pbsp[0], g_scratch);
		run(30);
		CHECK(dist2(upos(ai_i), p0) < 0.05f && A.st.actor[ai_i].active);
	}
	/* initial placement: the encounters not flagged "not initially created" of the loaded BSP */
	scene(0, 3, 3);
	{
		unsigned e, expect = 0, n = aidef_encounter_count(&F.ai);
		for (e = 0; e < n && e < AI_MAX_ENCOUNTERS; e++) {
			aidef_encounter_def d;
			einfo ie;
			if (!aidef_get_encounter(&F.ai, e, &d) || (d.flags & AIDEF_ENC_NOT_INITIALLY_CREATED)) continue;
			enc_info(e, &ie);
			if (ie.bsp == 0) expect += ie.total;
		}
		CHECK((unsigned)ai_place_all_initial(&A) == expect);
		{
			unsigned pend = 0, q;
			for (q = 0; q < AI_MAX_ENCOUNTERS; q++) pend += A.st.enc[q].pending;
			printf("  initial placement on BSP 0: %u actors, %u encounters pending for other BSPs\n", expect, pend);
		}
	}
}

/* ---- fights --------------------------------------------------------------------------------------------------------------------------- */
typedef struct { unsigned first_aware, first_shot, first_damage, first_death, deaths_a, deaths_b, alive_a, alive_b; float moved; float dmg; } fight_stats;

static int run_fight(int bsp, int seconds, fight_stats *fs)
{
	int ea, eb, a0, b0;
	ai_ref ra, rb;
	float spot[3], start_a[AI_MAX_ACTORS][3];
	unsigned t, i, n_a, n_b;
	memset(fs, 0, sizeof *fs);
	if (!pick_pair(bsp, &ea, &eb)) return 0;
	scene(bsp, 11, 3);
	ra = enc_ref(ea); rb = enc_ref(eb);
	ai_set_flag(&A, &ra, AIF_IGNORE_PLAYER | AIF_FORCE_ACTIVE, 1);
	ai_set_flag(&A, &rb, AIF_IGNORE_PLAYER | AIF_FORCE_ACTIVE, 1);
	ai_place(&A, &ra); ai_place(&A, &rb);
	n_a = ai_living_count(&A, &ra); n_b = ai_living_count(&A, &rb);
	a0 = first_actor(&ra); b0 = first_actor(&rb);
	CHECK(a0 >= 0 && b0 >= 0);
	if (a0 < 0 || b0 < 0) return 1;
	CHECK(sight_spot(bsp, upos(a0), spot));
	units_teleport(&F.units, &F.game, A.st.actor[b0].unit, spot, atan2f(upos(a0)[1] - spot[1], upos(a0)[0] - spot[0]));
	{
		float d = dist2(upos(a0), upos(b0));
		float yaw = atan2f(spot[1] - upos(a0)[1], spot[0] - upos(a0)[0]);
		units_teleport(&F.units, &F.game, A.st.actor[a0].unit, upos(a0), yaw);          /* the human faces the other one */
		(void)d;
	}
	for (i = 0; i < AI_MAX_ACTORS; i++) if (A.st.actor[i].active) memcpy(start_a[i], upos((int)i), 3 * sizeof(float));
	for (t = 1; t <= (unsigned)seconds * 30u; t++) {
		unsigned aw = 0;
		world_tick(1);
		for (i = 0; i < AI_MAX_ACTORS; i++) aw += A.st.actor[i].active && A.st.actor[i].aware == 2;
		if (!fs->first_aware && aw) fs->first_aware = t;
		if (!fs->first_shot && A.st.st_shots_ticks) fs->first_shot = t;
		if (!fs->first_damage && F.game.st_damage > 0.0f) fs->first_damage = t;
		if (!fs->first_death && A.st.st_deaths) fs->first_death = t;
	}
	fs->alive_a = ai_living_count(&A, &ra); fs->alive_b = ai_living_count(&A, &rb);
	fs->deaths_a = n_a - fs->alive_a; fs->deaths_b = n_b - fs->alive_b;
	fs->dmg = F.game.st_damage;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (A.st.actor[i].active) { float d = dist2(upos((int)i), start_a[i]); if (d > fs->moved) fs->moved = d; }
	return 1;
}

static void test_fights(void)
{
	int bsp, any = 0;
	for (bsp = 0; bsp < g_nbsp; bsp++) {
		fight_stats fs;
		if (!run_fight(bsp, 30, &fs)) { printf("  fight bsp %d: no pair, skipped\n", bsp); continue; }
		any++;
		printf("  fight bsp %d: routes %u (partial %u, failed %u, deferred %u), stuck %u, assists %u, atoms %u, noises %u, alerts %u, grenades %u, melee %u, firing positions picked %u\n", bsp,
		       (unsigned)A.st.st_routes, (unsigned)A.st.st_route_partial, (unsigned)A.st.st_route_fail, (unsigned)A.st.st_replans_deferred, (unsigned)A.st.st_stuck,
		       (unsigned)A.st.st_assists, (unsigned)A.st.st_atoms_run, (unsigned)A.st.st_noises, (unsigned)A.st.st_alerts, (unsigned)A.st.st_grenades, (unsigned)A.st.st_melee,
		       (unsigned)A.st.st_fp_picks);
		printf("  fight bsp %d (30 s): aware at tick %u, first shot %u, first damage %u, first death %u; deaths %u + %u, alive %u + %u, damage %.0f, farthest move %.1f wu\n",
		       bsp, fs.first_aware, fs.first_shot, fs.first_damage, fs.first_death, fs.deaths_a, fs.deaths_b, fs.alive_a, fs.alive_b, (double)fs.dmg, (double)fs.moved);
		CHECK(fs.first_aware > 0 && fs.first_aware < 90);                 /* they were put in sight: noticed within 3 s */
		CHECK(fs.first_shot > 0);
		CHECK(fs.first_damage > 0);
		CHECK(fs.first_death > 0);
		CHECK(fs.deaths_a + fs.deaths_b > 0);
		CHECK(A.st.st_noticed > 0);
	}
	CHECK(any > 0);
}

/* ---- the player ----------------------------------------------------------------------------------------------------------------------- */
static int ground_near(int bsp, const float a[3], float out[3]) { return sight_spot(bsp, a, out); }

static void test_player_target(void)
{
	int bsp, ea, eb, done = 0;
	for (bsp = 0; bsp < g_nbsp && !done; bsp++) {
		ai_ref rb;
		int b0;
		float spot[3];
		unsigned t, aware = 0, shots_before;
		float hp0;
		if (!pick_pair(bsp, &ea, &eb)) continue;
		scene(bsp, 5, 3);
		rb = enc_ref(eb);
		ai_place(&A, &rb);
		b0 = first_actor(&rb);
		if (b0 < 0 || !ground_near(bsp, upos(b0), spot)) continue;
		game_teleport(&F.game, spot, atan2f(upos(b0)[1] - spot[1], upos(b0)[0] - spot[0]));
		F.game.player.pos[2] = spot[2];
		hp0 = F.game.vitals.health + F.game.vitals.shield;
		shots_before = A.st.st_shots_ticks;
		for (t = 0; t < 30u * 30u && !aware; t++) { world_tick(1); aware = A.st.actor[b0].active && A.st.actor[b0].aware == 2 && A.st.actor[b0].tgt == (int)AI_TARGET_PLAYER; }
		CHECK(aware);
		printf("  player target bsp %d: noticed after %u ticks\n", bsp, t);
		for (t = 0; t < 30u * 20u; t++) {
			world_tick(1);
		}
		CHECK(A.st.st_shots_ticks > shots_before);
		CHECK(F.game.vitals.health + F.game.vitals.shield < hp0 || vitals_dead(&F.game.vitals));   /* it hit the player */
		/* the player dies: nobody targets it any more */
		F.game.vitals.health = 0.0f; F.game.vitals.flags |= VITALS_F_DEAD;
		run(3);
		{
			unsigned i, tgt = 0;
			for (i = 0; i < AI_MAX_ACTORS; i++) tgt += A.st.actor[i].active && A.st.actor[i].tgt == (int)AI_TARGET_PLAYER && A.st.actor[i].aware == 2;
			CHECK(tgt == 0);
		}
		done = 1;
	}
	CHECK(done);
}

static void test_follow(void)
{
	int bsp, ea, eb, done = 0;
	for (bsp = 0; bsp < g_nbsp && !done; bsp++) {
		ai_ref ra;
		int a0;
		float spot[3], far_spot[3], dbefore, dafter;
		unsigned i, n;
		if (!pick_pair(bsp, &ea, &eb)) continue;
		scene(bsp, 9, 3);
		ra = enc_ref(ea);
		ai_place(&A, &ra);
		a0 = first_actor(&ra);
		if (a0 < 0 || !ground_near(bsp, upos(a0), spot)) continue;
		ai_set_flag(&A, &ra, AIF_IGNORE_ALL, 1);                            /* no fighting: just follow */
		ai_set_follow(&A, &ra, NULL);
		n = ai_living_count(&A, &ra);
		game_teleport(&F.game, spot, 0.0f);
		F.game.player.pos[2] = spot[2];
		run(30 * 8);
		/* the player walks (teleports) away along the mesh: the followers come after it */
		memcpy(far_spot, spot, sizeof far_spot);
		{
			unsigned k;
			for (k = 0; k < 64; k++) {                                     /* a point 12..25 wu away on the same component */
				float cand[3], pt[3], dd, ang = (float)k * 0.9f;
				path_query q;
				path_query_default(&q);
				cand[0] = spot[0] + (12.0f + (float)(k % 7) * 2.0f) * cosf(ang); cand[1] = spot[1] + (12.0f + (float)(k % 7) * 2.0f) * sinf(ang); cand[2] = spot[2];
				if (path_nearest_sector(&g_pbsp[bsp], cand, 3.0f, &dd, pt) >= 0 && path_reachable(&g_pbsp[bsp], g_scratch, &q, spot, pt) && dist2(pt, spot) > 10.0f) { memcpy(far_spot, pt, sizeof pt); break; }
			}
		}
		if (dist2(far_spot, spot) < 10.0f) continue;
		dbefore = 0.0f;
		for (i = 0; i < AI_MAX_ACTORS; i++) if (A.st.actor[i].active) dbefore += dist2(upos((int)i), far_spot);
		game_teleport(&F.game, far_spot, 0.0f);
		F.game.player.pos[2] = far_spot[2];
		run(30 * 25);
		dafter = 0.0f;
		for (i = 0; i < AI_MAX_ACTORS; i++) {
			if (!A.st.actor[i].active) continue;
			dafter += dist2(upos((int)i), far_spot);
			CHECK(A.st.actor[i].st == AIS_FOLLOW);
		}
		printf("  follow bsp %d: %u followers, mean distance to the player %.1f -> %.1f wu\n", bsp, n, (double)(dbefore / (float)n), (double)(dafter / (float)n));
		CHECK(dafter / (float)n < dbefore / (float)n * 0.6f || dafter / (float)n < 8.0f);
		done = 1;
	}
	CHECK(done);
}

/* ---- command lists --------------------------------------------------------------------------------------------------------------------- */
static void test_command_lists(void)
{
	unsigned nl = aidef_command_list_count(&F.ai), l, tested = 0, arrived = 0, finished = 0, looping = 0;
	uint32_t seen0[AIDEF_ATOMS + 4], skipped0[AIDEF_ATOMS + 4];
	int bsp;
	for (bsp = 0; bsp < g_nbsp; bsp++) {
		int ea, eb;
		ai_ref r;
		if (!pick_pair(bsp, &ea, &eb)) continue;
		for (l = 0; l < nl; l++) {
			aidef_command_list_def cl;
			unsigned k, goto_k = 0xFFFFu;
			int actor;
			float spot[3];
			aidef_point_def pt;
			unsigned t;
			if (!aidef_get_command_list(&F.ai, l, &cl)) continue;
			if (((cl.flags & 32u) ? cl.manual_bsp : cl.precomputed_bsp) != bsp) continue;
			for (k = 0; k < cl.command_count; k++) {
				aidef_command_def c;
				aidef_get_command(&F.ai, &cl, k, &c);
				if ((c.type == AIDEF_ATOM_GO_TO || c.type == AIDEF_ATOM_GO_TO_AND_FACE) && c.point1 >= 0 && aidef_get_point(&F.ai, &cl, (unsigned)c.point1, &pt) &&
				    path_locate(&g_pbsp[bsp], pt.pos) >= 0) { goto_k = k; break; }
			}
			scene(bsp, 21u + l, 3);
			r = squad_ref(ea, 0);
			ai_place(&A, &r);
			actor = first_actor(&r);
			if (actor < 0) continue;
			ai_set_flag(&A, &r, AIF_IGNORE_ALL, 1);
			if (goto_k != 0xFFFFu) {
				aidef_command_def c;
				aidef_get_command(&F.ai, &cl, goto_k, &c);
				aidef_get_point(&F.ai, &cl, (unsigned)c.point1, &pt);
				if (!sight_spot(bsp, pt.pos, spot)) continue;
				units_teleport(&F.units, &F.game, A.st.actor[actor].unit, spot, 0.0f);
			}
			memcpy(seen0, A.st.atom_seen, sizeof seen0); memcpy(skipped0, A.st.atom_skipped, sizeof skipped0);
			CHECK(ai_command_list(&A, &r, (int)l, 1) >= 1);
			CHECK(ai_command_list_status(&A, A.st.actor[actor].unit) == 1);
			for (t = 0; t < 30u * 25u; t++) {
				world_tick(1);
				if (!A.st.actor[actor].active) break;
				if (A.st.actor[actor].cl_status != 1) break;
				if (goto_k != 0xFFFFu && A.st.actor[actor].cl_pc > goto_k && !arrived) arrived++;
			}
			tested++;
			if (A.st.actor[actor].active && A.st.actor[actor].cl_status == 2) { finished++; CHECK(ai_command_list_status(&A, A.st.actor[actor].unit) == 2); }
			else looping++;
			CHECK(A.st.st_atoms_run > 0 || cl.command_count == 0);
			(void)seen0; (void)skipped0;
		}
	}
	printf("  command lists: %u lists run on a test actor (%u finished, %u still looping after 25 s); atoms run %u, unsupported atoms counted:", tested,
	       finished, looping, (unsigned)A.st.st_atoms_run);
	{
		unsigned k;
		for (k = 0; k < AIDEF_ATOMS; k++) if (A.st.atom_skipped[k]) printf(" %s=%u", aidef_atom_name(k), (unsigned)A.st.atom_skipped[k]);
	}
	printf("\n");
	CHECK(tested > 0);
}

static void test_follow_list(void)
{
	unsigned nl = aidef_command_list_count(&F.ai), l;
	int bsp, done = 0;
	for (bsp = 0; bsp < g_nbsp && !done; bsp++) {
		int ea, eb;
		if (!pick_pair(bsp, &ea, &eb)) continue;
		for (l = 0; l < nl && !done; l++) {
			aidef_command_list_def cl;
			unsigned k, n_go = 0, goals = 0;
			aidef_point_def pts[3];
			float spot[3], dmin;
			int actor;
			ai_ref r;
			unsigned t;
			if (!aidef_get_command_list(&F.ai, l, &cl) || ((cl.flags & 32u) ? cl.manual_bsp : cl.precomputed_bsp) != bsp) continue;
			{
				path_query q0;
				unsigned all_go = 0, bad = 0;
				path_query_default(&q0);
				for (k = 0; k < cl.command_count; k++) {
					aidef_command_def cc;
					aidef_point_def pp;
					aidef_get_command(&F.ai, &cl, k, &cc);
					if ((cc.type != AIDEF_ATOM_GO_TO && cc.type != AIDEF_ATOM_GO_TO_AND_FACE) || cc.point1 < 0 || !aidef_get_point(&F.ai, &cl, (unsigned)cc.point1, &pp)) continue;
					if (path_locate(&g_pbsp[bsp], pp.pos) < 0 || (all_go && !path_reachable(&g_pbsp[bsp], g_scratch, &q0, pts[0].pos, pp.pos))) { bad = 1; break; }
					if (n_go < 2) { pts[n_go] = pp; n_go++; }
					all_go++;
				}
				if (bad || n_go < 2 || dist2(pts[0].pos, pts[1].pos) < 3.0f) continue;
			}
			scene(bsp, 77, 3);
			r = squad_ref(ea, 0);
			ai_place(&A, &r);
			actor = first_actor(&r);
			if (actor < 0 || !sight_spot(bsp, pts[0].pos, spot)) continue;
			ai_set_flag(&A, &r, AIF_IGNORE_ALL, 1);
			units_teleport(&F.units, &F.game, A.st.actor[actor].unit, spot, 0.0f);
			ai_command_list(&A, &r, (int)l, 1);
			dmin = 1e9f;
			for (t = 0; t < 30u * 40u && A.st.actor[actor].active; t++) {
				float d0, d1;
				world_tick(1);
				d0 = dist2(upos(actor), pts[0].pos); d1 = dist2(upos(actor), pts[1].pos);
				if (d0 < 1.0f && !(goals & 1u)) goals |= 1u;
				if (d1 < 1.0f && (goals & 1u)) { goals |= 2u; break; }
				if (d1 < dmin) dmin = d1;
			}
			printf("  follow list: both go-to points reached (%u) after %u ticks, %u atoms run, ladder hops %u, stuck assists %u\n", goals, t, (unsigned)A.st.st_atoms_run, (unsigned)A.st.st_climbs, (unsigned)A.st.st_assists);
			if ((goals & 3u) != 3u) {
				path_query q;
				const ai_actor *aa = &A.st.actor[actor];
				path_query_default(&q);
				printf("    list %u: pc %u status %u d1 %.1f assists %u stuck %u reachable p0->p1 %d wp_n %u wp_i %u stuck_ticks %u\n", l, aa->cl_pc, aa->cl_status, (double)dist2(upos(actor), pts[1].pos),
				       (unsigned)A.st.st_assists, (unsigned)A.st.st_stuck, path_reachable(&g_pbsp[bsp], g_scratch, &q, pts[0].pos, pts[1].pos), aa->wp_n, aa->wp_i, aa->stuck_ticks);
			}
			CHECK((goals & 3u) == 3u);
			done = 1;
		}
	}
	CHECK(done);
}

/* ---- conversations --------------------------------------------------------------------------------------------------------------------- */
typedef struct { unsigned starts, stops, ends, last_conv; } sndlog;
static int snd_hook(void *user, const ai_sound_event *ev)
{
	sndlog *l = (sndlog *)user;
	if (ev->kind == AI_SND_LINE_START) { l->starts++; l->last_conv = ev->conv; return 45; }
	if (ev->kind == AI_SND_LINE_STOP) l->stops++;
	if (ev->kind == AI_SND_CONV_END) l->ends++;
	return 0;
}

static void test_conversations(void)
{
	unsigned nc = aidef_conversation_count(&F.ai), c;
	int done = 0;
	sndlog log;
	for (c = 0; c < nc && !done; c++) {
		aidef_conversation_def cd;
		unsigned k, ok = 1, bsp = 99, t;
		unsigned encs[8];
		if (!aidef_get_conversation(&F.ai, c, &cd) || cd.participant_count == 0 || cd.participant_count > 8 || cd.line_count == 0) continue;
		for (k = 0; k < cd.participant_count && ok; k++) {
			aidef_participant_def pd;
			aidef_get_participant(&F.ai, &cd, k, &pd);
			if (pd.encounter_index < 0) { ok = 0; break; }
			encs[k] = (unsigned)pd.encounter_index;
			if (bsp == 99) bsp = enc_bsp_of(encs[k]);
			else if (enc_bsp_of(encs[k]) != bsp) ok = 0;
		}
		if (!ok || bsp >= (unsigned)g_nbsp) continue;
		scene((int)bsp, 31, 3);
		for (k = 0; k < cd.participant_count; k++) { ai_ref r = enc_ref((int)encs[k]); ai_set_flag(&A, &r, AIF_IGNORE_ALL, 1); ai_place(&A, &r); }
		{
			unsigned alive = 0, i;
			for (i = 0; i < AI_MAX_ACTORS; i++) alive += A.st.actor[i].active;
			if (!alive) continue;
		}
		memset(&log, 0, sizeof log);
		ai_set_sound_hook(&A, snd_hook, &log);
		CHECK(ai_conversation_status(&A, (int)c) == AI_CONV_NONE);
		CHECK(ai_conversation_start(&A, (int)c));
		CHECK(ai_conversation_status(&A, (int)c) == AI_CONV_STARTING);
		for (t = 0; t < 30u * 120u && ai_conversation_status(&A, (int)c) < AI_CONV_DONE; t++) world_tick(1);
		printf("  conversation: %u lines spoken, status %d after %u ticks, hook ends %u\n", log.starts, ai_conversation_status(&A, (int)c), t, log.ends);
		CHECK(ai_conversation_status(&A, (int)c) >= AI_CONV_DONE);
		CHECK(log.starts > 0 && log.ends == 1);
		/* stop in the middle of another one */
		memset(&log, 0, sizeof log);
		CHECK(ai_conversation_start(&A, (int)c));
		run(20);
		ai_conversation_stop(&A, (int)c);
		CHECK(ai_conversation_status(&A, (int)c) > AI_CONV_ENDING && log.ends == 1);
		done = 1;
	}
	if (!done) printf("  conversations: none with resolvable participants on BSP 0..2, skipped\n");
}

/* ---- cost, determinism, snapshot -------------------------------------------------------------------------------------------------------- */
/* about 16+ actors of every kind on one BSP, the player among them */
static unsigned crowd(int bsp, uint32_t seed)
{
	unsigned e, n = aidef_encounter_count(&F.ai), total = 0;
	scene(bsp, seed, 3);
	for (e = 0; e < n && total < 20; e++) {
		einfo ie;
		ai_ref r;
		enc_info(e, &ie);
		if (ie.bsp != bsp || ie.total == 0) continue;
		r = enc_ref((int)e);
		total += (unsigned)ai_place(&A, &r);
	}
	return total;
}

static void test_perf(void)
{
	int bsp;
	for (bsp = 0; bsp < g_nbsp; bsp++) {
		unsigned total = crowd(bsp, 5), t;
		double ai_ms = 0.0, ai_max = 0.0, all_ms = 0.0, t0;
		unsigned routes0, dormant0, worst_routes = 0, worst_rays = 0, worst_fp = 0;
		if (total < 6) continue;
		/* the player stands among them */
		{
			int a0 = -1;
			unsigned i;
			for (i = 0; i < AI_MAX_ACTORS; i++) if (A.st.actor[i].active) { a0 = (int)i; break; }
			if (a0 >= 0) { float p[3]; memcpy(p, upos(a0), sizeof p); p[0] += 3.0f; game_teleport(&F.game, p, 0.0f); F.game.player.pos[2] = p[2]; }
		}
		run(60);
		routes0 = A.st.st_routes; dormant0 = A.st.st_dormant_ticks;
		for (t = 0; t < 600; t++) {
			game_input_t none;
			double a0, a1;
			memset(&none, 0, sizeof none);
			mission_sync_player(&F.mission);
			{
				unsigned r0 = A.st.st_routes, y0 = A.st.st_los_rays, f0 = A.st.st_rays_fp;
				a0 = now_ms();
				ai_tick(&A);
				a1 = now_ms();
				if (a1 - a0 > ai_max) { worst_routes = A.st.st_routes - r0; worst_rays = A.st.st_los_rays - y0; worst_fp = A.st.st_rays_fp - f0; }
			}
			units_tick(&F.units, &F.game);
			game_tick(&F.game, &none);
			objects_tick(&F.objects, &F.ow);
			ai_ms += a1 - a0;
			if (a1 - a0 > ai_max) ai_max = a1 - a0;
			all_ms += now_ms() - a0;
		}
		t0 = ai_ms / 600.0;
		printf("  perf bsp %d: %u actors (%u alive at the end): ai_tick %.1f us avg, %.0f us worst; whole tick (ai + units + game + objects) %.1f us avg; routes %u, dormant ticks %u, rays %u; worst tick had %u routes, %u sight rays, %u firing-position rays\n",
		       bsp, total, ai_active_actors(&A), t0 * 1000.0, ai_max * 1000.0, all_ms / 600.0 * 1000.0, (unsigned)(A.st.st_routes - routes0), (unsigned)(A.st.st_dormant_ticks - dormant0),
		       (unsigned)A.st.st_los_rays, worst_routes, worst_rays, worst_fp);
		CHECK(t0 < 2.0);                                                    /* the host budget: the EE is 30..100x slower, target 2.5 ms there */
	}
}

static uint32_t world_hash(void)
{
	uint32_t h = 2166136261u;
	unsigned i;
	h = hash_bytes(h, ai_state_ptr(&A), ai_state_size());
	for (i = 0; i < F.units.count; i++) {
		const unit_t *u = &F.units.u[i];
		if (!u->active) continue;
		h = hash_bytes(h, u->body.pos, sizeof u->body.pos);
		h = hash_bytes(h, &F.game.unit_state[u->obj].vitals.health, sizeof(float));
	}
	h = hash_bytes(h, &F.game.st_shots, sizeof F.game.st_shots);
	return h;
}

static void test_determinism(void)
{
	uint32_t h1 = 0, h2 = 0;
	unsigned total, pass;
	for (pass = 0; pass < 2; pass++) {
		total = crowd(1, 99);
		if (total < 4) { printf("  determinism: no crowd on BSP 1, skipped\n"); return; }
		run(450);
		if (pass == 0) h1 = world_hash(); else h2 = world_hash();
	}
	printf("  determinism: two runs of 450 ticks hash %08x / %08x\n", h1, h2);
	CHECK(h1 == h2 && h1 != 0);
}

static ai_state g_snap_ai;
static units_t g_snap_units;
static object_table_t g_snap_objects;
static game_t g_snap_game;

static void test_snapshot(void)
{
	uint32_t hb, hc;
	ai_t B;
	ai_env env;
	unsigned total = crowd(1, 123), t;
	if (total < 4) { printf("  snapshot: no crowd on BSP 1, skipped\n"); return; }
	run(200);
	memcpy(&g_snap_ai, ai_state_ptr(&A), ai_state_size());
	memcpy(&g_snap_units, &F.units, sizeof F.units);
	memcpy(&g_snap_objects, &F.objects, sizeof F.objects);
	memcpy(&g_snap_game, &F.game, sizeof F.game);
	run(300);
	hb = world_hash();
	/* restore everything into the same world, the AI into a fresh ai_t: init + copy + rebind */
	memcpy(&F.units, &g_snap_units, sizeof F.units);
	memcpy(&F.objects, &g_snap_objects, sizeof F.objects);
	memcpy(&F.game, &g_snap_game, sizeof F.game);
	memset(&env, 0, sizeof env);
	env.aidef = &F.ai; env.game = &F.game; env.units = &F.units; env.path = &g_pbsp[1]; env.scratch = g_scratch; env.bsp = 1;
	ai_init(&B, &env);
	memcpy(ai_state_ptr(&B), &g_snap_ai, ai_state_size());
	CHECK(ai_rebind(&B, &env) == 0);
	memcpy(&A.st, &B.st, sizeof A.st);
	A.env = B.env;
	for (t = 0; t < 300; t++) world_tick(1);
	hc = world_hash();
	printf("  snapshot: sizeof(ai_state) = %u bytes, sizeof(ai_t) = %u; hash after restore %08x vs %08x\n", (unsigned)ai_state_size(), (unsigned)sizeof(ai_t), hb, hc);
	CHECK(hb == hc);
	/* a corrupt state is refused and leaves a clean AI */
	{
		ai_t C;
		ai_init(&C, &env);
		memcpy(ai_state_ptr(&C), &g_snap_ai, ai_state_size());
		C.st.actor[0].unit = 500; C.st.actor[0].active = 1;
		CHECK(ai_rebind(&C, &env) == -1 && ai_active_actors(&C) == 0);
		ai_init(&C, &env);
		memcpy(ai_state_ptr(&C), &g_snap_ai, ai_state_size());
		C.st.magic = 0;
		CHECK(ai_rebind(&C, &env) == -1);
		ai_init(&C, &env);
		memcpy(ai_state_ptr(&C), &g_snap_ai, ai_state_size());
		C.st.enc_count++;
		CHECK(ai_rebind(&C, &env) == -1);
	}
	CHECK(ai_state_size() <= 160u * 1024u);
}

/* ---- the built-ins --------------------------------------------------------------------------------------------------------------------- */
static void reg_ai(hs_machine *m) { hs_host_register_ai(m); }

static hs_value aiv(const ai_ref *r) { return mfix_handle(ai_ref_encode(r), 17); }

static void test_builtins(void)
{
	int ea, eb, bsp = 0, a0;
	ai_ref ra, rb;
	hs_value arg[3], v;
	mission_list *lst;
	einfo ia, ib;
	unsigned i;
	for (bsp = 0; bsp < g_nbsp; bsp++) if (pick_pair(bsp, &ea, &eb)) break;
	if (bsp == g_nbsp) { printf("  built-ins: no pair, skipped\n"); return; }
	scene(bsp, 17, 3);
	mfix_hs_start(&F, reg_ai);
	hs_host_ai_stats_reset();
	ra = enc_ref(ea); rb = enc_ref(eb);
	enc_info((unsigned)ea, &ia); enc_info((unsigned)eb, &ib);
	/* every built-in of the table is registered */
	for (i = 0; i < hs_host_ai_count(); i++) {
		unsigned k;
		int found = 0;
		for (k = 0; k < F.hs.nreg; k++) if (!strcmp(F.hs.reg[k].name, hs_host_ai_name(i))) found = 1;
		CHECK(found);
	}
	arg[0] = aiv(&ra);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.type == HS_SHORT && v.v.i == 0);
	v = mfix_call(&F, "ai_living_count", HS_REAL, arg, 1); CHECK(v.type == HS_REAL && v.v.f == 0.0f);
	v = mfix_call(&F, "ai_living_fraction", HS_REAL, arg, 1); CHECK(v.v.f == 0.0f);
	mfix_call(&F, "ai_place", HS_VOID, arg, 1);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == (int)ia.total);
	v = mfix_call(&F, "ai_living_fraction", HS_REAL, arg, 1); CHECK(v.v.f == 1.0f);
	v = mfix_call(&F, "ai_nonswarm_count", HS_SHORT, arg, 1); CHECK(v.v.i == (int)ia.total);
	v = mfix_call(&F, "ai_strength", HS_REAL, arg, 1); CHECK(v.v.f > 0.99f && v.v.f <= 1.0f);
	v = mfix_call(&F, "ai_status", HS_SHORT, arg, 1); CHECK(v.v.i >= 1);
	v = mfix_call(&F, "ai_actors", HS_HANDLE, arg, 1);
	lst = mission_list_get(&F.mission, v.v.h);
	CHECK(lst && lst->count == ia.total);
	if (lst) for (i = 0; i < lst->count; i++) CHECK(mission_object_live(&F.mission, lst->obj[i]) && units_of_object(&F.units, lst->obj[i]) >= 0);
	/* command list status of the actors: none yet (0), running (1) after ai_command_list, finished (2) once the unit is gone */
	{
		hs_value l2;
		unsigned nl = aidef_command_list_count(&F.ai), l;
		int pick = -1;
		for (l = 0; l < nl && pick < 0; l++) {
			aidef_command_list_def cl;
			aidef_get_command_list(&F.ai, l, &cl);
			if (cl.command_count > 1 && ((cl.flags & 32u) ? cl.manual_bsp : cl.precomputed_bsp) == bsp) pick = (int)l;
		}
		v = mfix_call(&F, "ai_command_list_status", HS_SHORT, &(hs_value){ .type = HS_HANDLE, .vtype = 23, .v.h = (lst ? (MISSION_LIST_BIT | (uint32_t)(lst - F.mission.lists)) : 0) }, 1);
		CHECK(v.v.i == 0);
		if (pick >= 0) {
			arg[0] = aiv(&ra); arg[1] = mfix_handle((uint32_t)pick, 18);
			mfix_call(&F, "ai_command_list", HS_VOID, arg, 2);
			v = mfix_call(&F, "ai_command_list_status", HS_SHORT, &(hs_value){ .type = HS_HANDLE, .vtype = 23, .v.h = (MISSION_LIST_BIT | (uint32_t)(lst - F.mission.lists)) }, 1);
			CHECK(v.v.i == 1);
			l2 = mfix_handle(0, 18); (void)l2;
		}
	}
	/* flags and orders reach the actors */
	arg[0] = aiv(&ra); arg[1] = mfix_bool(1);
	mfix_call(&F, "ai_braindead", HS_VOID, arg, 2);
	a0 = first_actor(&ra);
	CHECK(a0 >= 0 && (A.st.actor[a0].flags & AIF_BRAINDEAD));
	arg[1] = mfix_bool(0);
	mfix_call(&F, "ai_braindead", HS_VOID, arg, 2);
	CHECK(!(A.st.actor[a0].flags & AIF_BRAINDEAD));
	mfix_call(&F, "ai_set_blind", HS_VOID, (arg[1] = mfix_bool(1), arg), 2); CHECK(A.st.actor[a0].flags & AIF_BLIND);
	mfix_call(&F, "ai_set_blind", HS_VOID, (arg[1] = mfix_bool(0), arg), 2); CHECK(!(A.st.actor[a0].flags & AIF_BLIND));
	mfix_call(&F, "ai_defend", HS_VOID, arg, 1); CHECK(A.st.actor[a0].orders == AIO_DEFEND);
	mfix_call(&F, "ai_attack", HS_VOID, arg, 1); CHECK(A.st.actor[a0].orders == AIO_ATTACK);
	mfix_call(&F, "ai_retreat", HS_VOID, arg, 1); CHECK(A.st.actor[a0].orders == AIO_RETREAT);
	mfix_call(&F, "ai_follow_target_players", HS_VOID, arg, 1); CHECK(A.st.actor[a0].orders == AIO_FOLLOW_PLAYER);
	mfix_call(&F, "ai_follow_target_disable", HS_VOID, arg, 1); CHECK(A.st.actor[a0].orders == AIO_DEFEND);
	/* kill / erase / migrate */
	arg[0] = aiv(&rb);
	mfix_call(&F, "ai_place", HS_VOID, arg, 1);
	arg[0] = aiv(&ra); arg[1] = aiv(&rb);
	mfix_call(&F, "ai_migrate", HS_VOID, arg, 2);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == 0);
	arg[0] = aiv(&rb);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == (int)(ia.total + ib.total));
	mfix_call(&F, "ai_kill", HS_VOID, arg, 1);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == 0);
	v = mfix_call(&F, "ai_living_fraction", HS_REAL, arg, 1); CHECK(v.v.f == 0.0f || v.v.f == 0.0f);
	mfix_call(&F, "ai_erase", HS_VOID, arg, 1);
	mfix_call(&F, "ai_erase_all", HS_VOID, NULL, 0);
	/* queries on garbage never fail */
	arg[0] = mfix_handle(0xFFFFFFFFu, 17);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == 0);
	arg[0] = mfix_handle(0x7FFFu, 17);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == 0);
	/* allegiance: broken -> hostile, allied -> not */
	arg[0] = mfix_handle(AIDEF_TEAM_PLAYER, 33); arg[1] = mfix_handle(AIDEF_TEAM_COVENANT, 33);
	CHECK(ai_teams_hostile(&A, AIDEF_TEAM_PLAYER, AIDEF_TEAM_COVENANT));
	mfix_call(&F, "ai_allegiance", HS_VOID, arg, 2);
	CHECK(!ai_teams_hostile(&A, AIDEF_TEAM_PLAYER, AIDEF_TEAM_COVENANT) && !ai_teams_hostile(&A, AIDEF_TEAM_COVENANT, AIDEF_TEAM_PLAYER));
	mfix_call(&F, "ai_allegiance_remove", HS_VOID, arg, 2);
	CHECK(ai_teams_hostile(&A, AIDEF_TEAM_PLAYER, AIDEF_TEAM_COVENANT));
	v = mfix_call(&F, "ai_allegiance_broken", HS_BOOL, arg, 2); CHECK(v.v.i == 0);
	ai_allegiance(&A, AIDEF_TEAM_PLAYER, AIDEF_TEAM_HUMAN, 2);
	v = mfix_call(&F, "ai_allegiance_broken", HS_BOOL, (arg[0] = mfix_handle(AIDEF_TEAM_PLAYER, 33), arg[1] = mfix_handle(AIDEF_TEAM_HUMAN, 33), arg), 2); CHECK(v.v.i == 1);
	ai_allegiance(&A, AIDEF_TEAM_PLAYER, AIDEF_TEAM_HUMAN, 0);
	/* conversation status of an unknown conversation */
	arg[0] = mfix_handle(0, 20);
	v = mfix_call(&F, "ai_conversation_status", HS_SHORT, arg, 1); CHECK(v.v.i == AI_CONV_NONE);
	arg[0] = mfix_handle(0xFFFFu, 20);
	v = mfix_call(&F, "ai_conversation_status", HS_SHORT, arg, 1); CHECK(v.v.i == AI_CONV_NONE);
	/* with no AI bound everything is a no-op returning the neutral value */
	F.mission.ai = NULL;
	arg[0] = aiv(&ra);
	v = mfix_call(&F, "ai_living_count", HS_SHORT, arg, 1); CHECK(v.v.i == 0);
	mfix_call(&F, "ai_place", HS_VOID, arg, 1);
	v = mfix_call(&F, "ai_actors", HS_HANDLE, arg, 1); CHECK(mission_list_get(&F.mission, v.v.h) && mission_list_get(&F.mission, v.v.h)->count == 0);
	F.mission.ai = &A;
	{
		unsigned called = 0;
		for (i = 0; i < hs_host_ai_count(); i++) called += hs_host_ai_calls(i) > 0;
		printf("  built-ins: %u of %u distinct built-ins exercised directly\n", called, hs_host_ai_count());
	}
}

/* random arguments, every built-in, many times: no crash (ASan / UBSan), the state stays rebindable */
static void test_fuzz(void)
{
	unsigned iter, i, nb = hs_host_ai_count(), nreg;
	uint32_t rng = 0x1234567u;
	int ea, eb, bsp;
	for (bsp = 0; bsp < g_nbsp; bsp++) if (pick_pair(bsp, &ea, &eb)) break;
	if (bsp == g_nbsp) return;
	scene(bsp, 19, 3);
	mfix_hs_start(&F, reg_ai);
	nreg = F.hs.nreg;
	{
		ai_ref r = enc_ref(ea);
		ai_place(&A, &r);
		r = enc_ref(eb);
		ai_place(&A, &r);
	}
	for (iter = 0; iter < 4000; iter++) {
		hs_value args[3];
		unsigned k;
		hs_value ret;
		const char *name = hs_host_ai_name(iter % nb);
		for (k = 0; k < 3; k++) {
			uint32_t pick;
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			pick = rng % 8u;
			memset(&args[k], 0, sizeof args[k]);
			args[k].type = HS_HANDLE; args[k].vtype = 17;
			switch (pick) {
			case 0: args[k].v.h = 0xFFFFFFFFu; break;
			case 1: args[k].v.h = rng >> 8; break;
			case 2: args[k].v.h = (uint32_t)(rng % aidef_encounter_count(&F.ai)); break;
			case 3: { ai_ref q = squad_ref((int)(rng % aidef_encounter_count(&F.ai)), (int)((rng >> 8) % 4u)); args[k].v.h = ai_ref_encode(&q); } break;
			case 4: args[k].type = HS_BOOL; args[k].vtype = 5; args[k].v.i = (int32_t)(rng & 1u); break;
			case 5: args[k].type = HS_REAL; args[k].vtype = 6; args[k].v.f = (float)(int)(rng % 100u) - 50.0f; break;
			case 6: args[k].v.h = MISSION_LIST_BIT | (rng % 70u); args[k].vtype = 23; break;
			default: args[k].v.h = mission_obj_handle((int)(rng % 200u)); args[k].vtype = 38; break;
			}
		}
		ret = mfix_call(&F, name, (iter & 1u) ? HS_SHORT : HS_REAL, args, 1u + (iter % 3u));
		(void)ret;
		if (iter % 7u == 0) world_tick(1);
	}
	(void)nreg;
	for (i = 0; i < 90; i++) world_tick(1);
	{
		ai_t C;
		ai_env env = A.env;
		ai_init(&C, &env);
		memcpy(ai_state_ptr(&C), ai_state_ptr(&A), ai_state_size());
		CHECK(ai_rebind(&C, &env) == 0);
	}
	printf("  fuzz: 4000 random built-in calls, then 90 ticks, state still rebinds\n");
}

/* ---- script soak: the real a10 script machine with the ai and the object built-ins registered ----------------------------------------------- */
static void reg_obj_ai(hs_machine *m) { hs_host_register_objects(m); hs_host_register_ai(m); }

static void test_soak(void)
{
	unsigned t, i, woken = 0;
	uint32_t calls_total = 0;
	unsigned peak = 0, max_expand = 0;
	if (!F.has_scr || !F.has_ai) { printf("  soak: no script pack, skipped\n"); return; }
	scene(0, 4, 3);
	mfix_hs_start(&F, reg_obj_ai);
	hs_host_ai_stats_reset();
	{
		double t0 = now_ms(), ai_ms = 0.0;
		for (t = 0; t < 3000; t++) {
			game_input_t none;
			double a0;
			memset(&none, 0, sizeof none);
			if (t == 90) for (i = 0; i < F.scr.script_count; i++) if (F.scr.scripts[i].type == HS_S_DORMANT && hs_wake_script_index(&F.hs, i) == 0) woken++;
			mission_sync_player(&F.mission);
			hs_tick(F.mission.hs);
			hs_host_obj_pre(&F.mission, &none);
			a0 = now_ms();
			ai_tick(&A);
			ai_ms += now_ms() - a0;
			units_tick(&F.units, &F.game);
			game_tick(&F.game, &none);
			hs_host_obj_post(&F.mission);
			objects_tick(&F.objects, &F.ow);
			F.mission.tick++;
			if (ai_active_actors(&A) > peak) peak = ai_active_actors(&A);
		}
		(void)max_expand;
		printf("  soak: 3000 ticks, a10 script machine with every dormant script woken at tick 90 (%u woken), ai + object built-ins registered, the rest neutral: %.1f ms (ai_tick %.1f ms), peak %u actors, %u placed, %u deaths, %u routes\n",
		       woken, now_ms() - t0, ai_ms, peak, (unsigned)A.st.st_placed, (unsigned)A.st.st_deaths, (unsigned)A.st.st_routes);
	}
	printf("  soak ai built-in calls:");
	for (i = 0; i < hs_host_ai_count(); i++) if (hs_host_ai_calls(i)) { printf(" %s=%u", hs_host_ai_name(i), (unsigned)hs_host_ai_calls(i)); calls_total += hs_host_ai_calls(i); }
	printf("\n");
	CHECK(F.hs.st.errors == 0);
	CHECK(calls_total > 0);
	CHECK(ai_rebind(&A, &A.env) == 0);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "../../assets/hp2";
	const char *pp = argc > 2 ? argv[2] : "";
	if (!mfix_load(&F, dir, NULL) || !F.has_ai || !load_extra(dir, pp)) { printf("skip: no a10 packs in %s (or no path pack)\n", dir); return 0; }
	printf("test_ai: %u encounters, %u squads, %u command lists, %u conversations\n", aidef_encounter_count(&F.ai), aidef_squad_count(&F.ai),
	       aidef_command_list_count(&F.ai), aidef_conversation_count(&F.ai));
	printf("sizeof(ai_state) = %u, sizeof(ai_t) = %u, sizeof(ai_actor) = %u, sizeof(ai_profile) = %u\n", (unsigned)sizeof(ai_state), (unsigned)sizeof(ai_t),
	       (unsigned)sizeof(ai_actor), (unsigned)sizeof(ai_profile));
	test_refs();
	test_spawn();
	test_pending_bsp();
#if HAVE_UNITS
	test_fights();
	test_player_target();
	test_follow();
	test_command_lists();
	test_follow_list();
	test_conversations();
	test_perf();
	test_determinism();
	test_snapshot();
#else
	printf("  no ai/units.c: behavioural tests skipped\n");
#endif
	test_builtins();
	test_fuzz();
	test_soak();
	if (failures) { printf("test_ai: %d FAILURES\n", failures); return 1; }
	printf("test_ai: ok\n");
	return 0;
}
