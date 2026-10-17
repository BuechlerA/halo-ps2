#include "mission_app.h"
#include "ai/ai.h"
#include "script/hs_host_ai.h"
#include "script/hs_host_misc.h"
#include "script/hs_host_obj.h"
#include "sim/bot.h"
#include "platform/timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static units_t        s_units;
static hs_machine     s_hs;
static devices_t      s_dev;
static cine_state     s_cine;
static mission_t      s_m;
static ai_t           s_ai;
static bot_t          s_bot;
static path_scratch  *s_pscratch;
#define PATH_SECTORS_MAX 12000u
static mission_checkpoint_t s_ck;
static mapp_env       s_env;
static int            s_active;
static int            s_use_bot;
static unsigned       s_status_tick;

static uint32_t clock_now(void) { return timer_cycles(); }
static float clock_ms(uint32_t a, uint32_t b) { return timer_ms(a, b); }
mission_t *mapp_mission(void) { return &s_m; }
int mapp_active(void) { return s_active; }
mission_checkpoint_t *mapp_checkpoint(void) { return &s_ck; }
/* Debug: what every sleeping script thread waits on (script name and the chain of built-ins of its innermost frames). */
void mapp_dump_threads(void)
{
	const hs_state *st = hs_state_ptr(&s_hs);
	const hs_section *sec = s_m.scripts;
	unsigned t;
	for (t = 0; t < HS_MAX_THREADS; t++) {
		const hs_thread *th = &st->threads[t];
		char chain[160];
		unsigned f, n = 0;
		if (th->state != HS_T_ACTIVE) continue;
		chain[0] = 0;
		for (f = 0; f < th->fsp && f < HS_MAX_FRAMES; f++) {
			const hs_node *nd = &sec->nodes[th->f[f].node];
			const char *nm = nd->kind == HS_N_CALL ? sec->strings + sec->builtins[nd->data].name : nd->kind == HS_N_SCRIPT_CALL ? sec->strings + sec->scripts[nd->data].name : "?";
			if (f + 4 >= th->fsp) n += (unsigned)snprintf(chain + n, sizeof chain - n, "%s%s", n ? ">" : "", nm);
			if (n > 120) break;
		}
		printf("[mission] thread %u script %s wake %s (in %d ticks, fsp %u, inner node kind %u phase %u deadline %u): %s\n", t, sec->strings + sec->scripts[th->script].name, th->wake == HS_FOREVER ? "never" : "later", th->wake == HS_FOREVER ? -1 : (int)(th->wake - st->tick), (unsigned)th->fsp, th->fsp ? (unsigned)sec->nodes[th->f[th->fsp - 1].node].kind : 0u, th->fsp ? (unsigned)th->f[th->fsp - 1].phase : 0u, th->fsp ? (unsigned)th->f[th->fsp - 1].aux : 0u, chain);
	}
}

void mapp_report(void)
{
	unsigned k = 0;
	uint32_t calls;
	const char *name;
	printf("[mission] script machine: %u threads, %u ticks, errors %u\n", (unsigned)hs_thread_count(&s_hs), s_m.tick, (unsigned)hs_state_ptr(&s_hs)->errors);
	while (hs_unregistered(&s_hs, k, &name, &calls)) { if (calls) printf("[mission] unregistered built-in %s: %u calls\n", name, (unsigned)calls); k++; }
	printf("[mission] ai: placed %u, deaths %u, noticed %u, shots %u; units spawned %u died %u\n", s_ai.st.st_placed, s_ai.st.st_deaths, s_ai.st.st_noticed, s_ai.st.st_shots_ticks, s_units.spawned, s_units.died);
}

const void *mapp_bot(void) { return &s_bot; }

/* ---- hooks ---- */
static void hook_play(void *user, uint32_t tag, const float *pos, float gain)
{
	camera_t cam;
	(void)user;
	if (!s_env.ga || !s_env.ga->snd) return;
	game_camera(s_env.game, &cam);
	gameaudio_play(s_env.ga, tag, pos, &cam, gain);
}

static void hook_loop_start(void *user, uint32_t tag, const float *pos, float gain) { (void)user; (void)pos; (void)gain; (void)tag; /* resident loops: Phase 5 */ }
static void hook_loop_stop(void *user, uint32_t tag) { (void)user; (void)tag; }

static int ai_sound(void *user, const ai_sound_event *ev)
{
	(void)user;
	if (ev->kind != AI_SND_LINE_START || !ev->sound) return 0;
	if (s_env.ga && s_env.ga->snd && snd_find(s_env.ga->snd, ev->sound) >= 0) {
		snd_sound sd = snd_get_sound(s_env.ga->snd, (uint32_t)snd_find(s_env.ga->snd, ev->sound));
		hook_play(NULL, ev->sound, ev->pos, 1.0f);
		if (sd.perm_count) { snd_perm p = snd_get_perm(s_env.ga->snd, &sd, 0); return p.rate ? (int)((float)p.frames * 30.0f / (float)p.rate) : 0; }
		return 0;
	}
	if (s_env.sp && s_env.sp->sec) {
		int idx = stream_play_sound(s_env.sp, STREAM_SLOT_DIALOGUE, ev->sound, -1, 0, 1.0f, 0.0f, -1.0f);
		if (idx >= 0) { strm_stream st = strm_get_stream(s_env.sp->sec, (uint32_t)idx); return st.rate ? (int)((float)st.frames * 30.0f / (float)st.rate) : 0; }
	}
	return 0;
}

static void ai_script(void *user, uint32_t name_index)
{
	char name[AIDEF_NAME_LEN];
	(void)user;
	if (aidef_script_ref_name(&s_env.gd->ai, name_index, name)) hs_wake_script(&s_hs, name);
}

static int ai_object(void *user, uint32_t name_index) { (void)user; return mission_object_by_name(&s_m, name_index, 0); }

/* Persistent allocation (before the BSP arena mark): the route scratch for the largest BSP. */
void mapp_reserve(arena_t *arena)
{
	size_t bytes = sizeof(path_scratch) + path_scratch_bytes(PATH_SECTORS_MAX) + 64u;
	s_pscratch = arena_alloc(arena, bytes, 64);
	if (s_pscratch && !path_scratch_init(s_pscratch, (uint8_t *)s_pscratch + sizeof(path_scratch), bytes - sizeof(path_scratch), PATH_SECTORS_MAX)) s_pscratch = NULL;
}

/* ---- setup ---- */
int mapp_init(const mapp_env *env)
{
	gamedata_t *gd = env->gd;
	ai_env aenv;
	unsigned i;
	s_env = *env;
	if (!gd->has_script || !gd->has_dev || !gd->has_ai) { printf("[mission] needs the script, device and AI packs\n"); return -1; }
	mission_init(&s_m);
	s_m.game = env->game; s_m.objects = env->objects; s_m.ow = env->ow; s_m.units = &s_units; s_m.hs = &s_hs; s_m.scripts = &gd->script;
	s_m.devices = &s_dev; s_m.dev = &gd->dev; s_m.cine = &s_cine; s_m.cin = gd->has_cin ? &gd->cin : NULL; s_m.aidef = &gd->ai; s_m.ai = &s_ai;
	s_m.hud = env->hud; s_m.huddef = gd->has_hud ? &gd->hud : NULL; s_m.strings = gd->has_strings ? &gd->strings : NULL; s_m.fx = env->fx;
	s_m.stream = env->sp; s_m.cur_bsp = env->bsp; s_m.difficulty = env->game->difficulty;
	s_m.misc_env.strm = (env->sp && env->sp->sec) ? env->sp->sec : NULL;
	s_m.misc_env.snd = gd->has_snd ? &gd->snd : NULL;
	s_m.misc_env.play = hook_play; s_m.misc_env.loop_start = hook_loop_start; s_m.misc_env.loop_stop = hook_loop_stop;
	units_init(&s_units);
	devices_init(&s_dev, &gd->dev);
	cine_init(&s_cine);
	mission_attach_player(&s_m);
	/* placed bipeds are units (scripts and encounters drive them) */
	for (i = 0; i < env->objects->count; i++) {
		const object_t *o = &env->objects->obj[i];
		if (o->type == SCEN_BIPED && !(o->flags & OBJ_F_FREE) && (int)i != s_m.player_obj) units_attach(&s_units, env->game, (int)i, 0, 0);
	}
	memset(&aenv, 0, sizeof aenv);
	aenv.aidef = &gd->ai; aenv.game = env->game; aenv.units = &s_units; aenv.path = gd->has_path ? &gd->path : NULL; aenv.scratch = s_pscratch; aenv.bsp = env->bsp;
	ai_init(&s_ai, &aenv);
	ai_set_sound_hook(&s_ai, ai_sound, NULL);
	ai_set_script_hook(&s_ai, ai_script, NULL);
	ai_set_object_hook(&s_ai, ai_object, NULL);
	hs_setup(&s_hs);
	hs_host_register_objects(&s_hs);
	hs_host_register_ai(&s_hs);
	hs_host_register_misc(&s_hs);                          /* last: it wins where two domains register the same name */
	if (hs_init(&s_hs, &gd->script, &s_m) != HS_OK) { printf("[mission] hs_init failed\n"); return -1; }
	s_use_bot = (int)cfg_int(env->cfg, "bot", 0);
	s_m.log = (int)cfg_int(env->cfg, "missionlog", 0);
	if (s_m.log) { s_m.clock = clock_now; s_m.clock_ms = clock_ms; }
	if (s_use_bot) bot_init(&s_bot);
	ai_place_all_initial(&s_ai);
	s_active = 1;
	printf("[mission] ready: %u scripts, %u units, %u encounters; bot %d\n", (unsigned)gd->script.script_count, s_units.count, (unsigned)aidef_encounter_count(&gd->ai), s_use_bot);
	return 0;
}

void mapp_set_bsp(int bsp)
{
	if (!s_active) return;
	s_m.cur_bsp = bsp;
	s_ai.env.path = s_env.gd->has_path ? &s_env.gd->path : NULL;
	ai_set_bsp(&s_ai, bsp, s_ai.env.path, s_pscratch);
	s_bot.wp_n = s_bot.wp_i = 0;
	if (s_env.gd->has_path && cfg_int(s_env.cfg, "pathprobe", 0)) {
		const path_bsp *pb = &s_env.gd->path;
		float p[3] = { -47.4f, -3.2f, 1.5f };
		float fz = 0;
		printf("[bot] probe: sectors %u, locate %d (floor z %.2f), nearest %d\n", (unsigned)pb->sector_count, (int)path_locate_ex(pb, p, 1.0f, 0.5f, &fz), (double)fz, (int)path_nearest_sector(pb, p, 8.0f, NULL, NULL));
	}
}

int mapp_start_location(float pos[3], float *yaw, int *bsp)
{
	devdef_start st;
	unsigned b;
	if (!s_env.gd || !s_env.gd->has_dev || s_env.gd->dev.start_count == 0) return 0;
	st = devdef_get_start(&s_env.gd->dev, 0);
	pos[0] = st.pos[0]; pos[1] = st.pos[1]; pos[2] = st.pos[2];
	*yaw = st.facing;
	*bsp = 0;
	for (b = 0; b < 32; b++) if (st.bsp_mask & (1u << b)) { *bsp = (int)b; break; }
	return 1;
}

void mapp_tick(const game_input_t *in)
{
	game_input_t bot_in;
	if (!s_active) return;
	if (s_use_bot) {
		bot_tick(&s_bot, &s_m, s_env.gd->has_path ? &s_env.gd->path : NULL, s_pscratch, &bot_in);
		in = &bot_in;
	}
	mission_tick(&s_m, in);
	if (cfg_int(s_env.cfg, "missionlog", 0) && (s_m.tick - s_status_tick) >= 300) { s_status_tick = s_m.tick; mapp_log_status(); }
}

void mapp_log_status(void)
{
	unsigned i, alive = 0;
	for (i = 0; i < s_units.count; i++) if (s_units.u[i].active && units_alive(&s_units, s_env.game, (int)i)) alive++;
	if (s_bot.has_goal) printf("[bot] goal kind %d id %u at %.1f %.1f %.1f (%.1f away), route status %d start %d goal %d, %d waypoints, stuck %u unreach %u\n", s_bot.goal_kind, (unsigned)s_bot.goal_id, s_bot.goal[0], s_bot.goal[1], s_bot.goal[2], (double)sqrtf((s_bot.goal[0] - s_env.game->player.pos[0]) * (s_bot.goal[0] - s_env.game->player.pos[0]) + (s_bot.goal[1] - s_env.game->player.pos[1]) * (s_bot.goal[1] - s_env.game->player.pos[1])), s_bot.route_status, s_bot.route_start, s_bot.route_goal, s_bot.wp_n, (unsigned)s_bot.stuck_ticks, (unsigned)s_bot.unreach_ticks);
	if (s_bot.target_unit >= 0) { const unit_t *tu = &s_units.u[s_bot.target_unit]; const game_unit_t *gu = &s_env.game->unit_state[tu->obj]; printf("[bot] engaging unit %d obj %d team %d biped %d at %.1f %.1f %.1f hp %.1f/%.1f dead %d\n", s_bot.target_unit, (int)tu->obj, (int)tu->team, (int)tu->biped, tu->body.pos[0], tu->body.pos[1], tu->body.pos[2], gu->vitals.health, gu->vitals.shield, gu->dead); }
	if (s_m.clock && s_m.zone_n) {
		unsigned z;
		printf("[mission] ms per tick (avg/peak) over %u ticks:", s_m.zone_n);
		for (z = 0; z < 8; z++) { printf(" %s %.2f/%.2f", z == 0 ? "script" : z == 1 ? "dev" : z == 2 ? "pre" : z == 3 ? "ai" : z == 4 ? "units" : z == 5 ? "game" : z == 6 ? "post" : "cine", (double)(s_m.zone_sum[z] / (float)s_m.zone_n), (double)s_m.zone_peak[z]); s_m.zone_sum[z] = 0.0f; s_m.zone_peak[z] = 0.0f; }
		printf("\n");
		s_m.zone_n = 0;
	}
	printf("[mission] t=%us bsp %d pos %.1f %.1f %.1f hp %.0f/%.0f units %u alive %u threads %u won %d lost %d cin %d bot: goal %d assists %u replans %u enemies %u\n", s_m.tick / 30, s_m.cur_bsp,
	       s_env.game->player.pos[0], s_env.game->player.pos[1], s_env.game->player.pos[2], s_env.game->vitals.health, s_env.game->vitals.shield, s_units.count, alive,
	       (unsigned)hs_thread_count(&s_hs), s_m.won, s_m.lost, (int)s_m.cinematic, s_bot.goal_kind, s_bot.assists, s_bot.replans, s_bot.enemies_seen);
}

void mapp_checkpoint_save(void) { if (s_active) msave_save(&s_ck, &s_m, s_env.fx && s_env.fx->def ? s_env.fx : NULL); }
int  mapp_checkpoint_bsp(void) { return msave_bsp(&s_ck); }
int  mapp_checkpoint_restore(void) { return s_active ? msave_revert(&s_ck, &s_m, s_env.fx && s_env.fx->def ? s_env.fx : NULL) : 0; }
