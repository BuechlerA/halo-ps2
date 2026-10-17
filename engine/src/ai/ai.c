/* AI runtime, see ai/ai.h and docs/32-ai-runtime.md. */
#include "ai/ai.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TICKS 30.0f
#define PI_F 3.14159265f
#define PERC_PERIOD 4u                 /* ticks between two looks of one actor (staggered by actor index) */
#define LOSE_TICKS 60u                 /* a target not seen for this long makes the actor search */
#define SEARCH_TICKS 360u              /* how long a search for a lost target lasts */
#define ALERT_TICKS 75u
#define ACTIVE_DIST 120.0f             /* actors further than this from the player (and not forced / fighting) idle */
#define HUNT_DIST 70.0f                /* searching squads hunt the nearest enemy within this distance */
#define ARRIVE_R 0.55f
#define WP_R 0.45f
#define NOISE_R 1.6f                   /* a gunshot event this close to a unit comes from it */
#define MAX_ATOMS_PER_TICK 8u

/* ---- small helpers -------------------------------------------------------------------------------------------------------------------- */
static uint32_t rnd(ai_t *ai)
{
	uint32_t x = ai->st.rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	ai->st.rng = x;
	return x;
}
static float rf(ai_t *ai) { return (float)(rnd(ai) >> 8) * (1.0f / 16777216.0f); }
static float rrange(ai_t *ai, float lo, float hi) { return hi <= lo ? lo : lo + (hi - lo) * rf(ai); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float wrap_pi(float a)
{
	while (a > PI_F) a -= 2.0f * PI_F;
	while (a < -PI_F) a += 2.0f * PI_F;
	return a;
}
static float dist2d2(const float a[3], const float b[3]) { float dx = a[0] - b[0], dy = a[1] - b[1]; return dx * dx + dy * dy; }
static float dist3(const float a[3], const float b[3])
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}
static void cp3(float d[3], const float s[3]) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
static unsigned secs(float s) { return s <= 0.0f ? 0u : (unsigned)(s * TICKS + 0.5f); }
static void logmsg(ai_t *ai, const char *m) { if (ai->log) ai->log(ai->log_user, m); }

static const scen_section *scen_of(const ai_t *ai) { return ai->env.game ? ai->env.game->ow.scen : NULL; }
static unit_t *unit_at(const ai_t *ai, int unit) { return &ai->env.units->u[unit]; }
static int obj_live(const ai_t *ai, int obj)
{
	const object_table_t *t = ai->env.game ? ai->env.game->objects : NULL;
	return t && obj >= 0 && (unsigned)obj < t->count && !(t->obj[obj].flags & OBJ_F_FREE);
}
static int is_friend_team(int team) { return team == AIDEF_TEAM_HUMAN || team == AIDEF_TEAM_PLAYER; }

int ai_teams_hostile(const ai_t *ai, int a, int b)
{
	if (a < 0 || b < 0 || a >= (int)AIDEF_TEAMS || b >= (int)AIDEF_TEAMS) return 0;
	if (ai->st.rel[a][b] == 1) return 0;
	if (ai->st.rel[a][b] == 2) return 1;
	return units_teams_hostile(a, b);
}

/* ---- references ----------------------------------------------------------------------------------------------------------------------- */
int ai_ref_decode(const ai_t *ai, uint32_t h, ai_ref *out)
{
	uint32_t enc = h & 0xFFFFu, idx = (h >> 16) & 0x3FFFu;
	memset(out, 0, sizeof *out);
	out->squad = out->platoon = -1;
	if (h == 0xFFFFFFFFu || !ai->env.aidef || enc >= aidef_encounter_count(ai->env.aidef) || enc >= AI_MAX_ENCOUNTERS) return 0;
	out->enc = (int16_t)enc;
	if (h & 0x80000000u) {
		idx = (h >> 16) & 0x7FFFu;
		if (idx >= ai->st.enc[enc].squad_count) return 0;
		out->kind = 2; out->squad = (int16_t)idx;
	} else if (h & 0x40000000u) {
		aidef_encounter_def e;
		if (!aidef_get_encounter(ai->env.aidef, enc, &e) || idx >= e.platoon_count) return 0;
		out->kind = 3; out->platoon = (int16_t)idx;
	} else {
		if ((h >> 16) != 0) return 0;
		out->kind = 1;
	}
	return 1;
}

uint32_t ai_ref_encode(const ai_ref *r)
{
	if (r->kind == 2) return 0x80000000u | ((uint32_t)r->squad << 16) | (uint32_t)r->enc;
	if (r->kind == 3) return 0x40000000u | ((uint32_t)r->platoon << 16) | (uint32_t)r->enc;
	return (uint32_t)r->enc;
}

int ai_ref_squad_range(const ai_t *ai, const ai_ref *r, unsigned *first, unsigned *count)
{
	const ai_enc_rt *e;
	if (r->kind == 0 || r->enc < 0 || (unsigned)r->enc >= AI_MAX_ENCOUNTERS) return 0;
	e = &ai->st.enc[r->enc];
	if (r->kind == 2) { *first = e->first_squad + (unsigned)r->squad; *count = 1; }
	else { *first = e->first_squad; *count = e->squad_count; }
	return 1;
}

int ai_ref_has_squad(const ai_t *ai, const ai_ref *r, unsigned gsq)
{
	unsigned first, count;
	if (!ai_ref_squad_range(ai, r, &first, &count) || gsq < first || gsq >= first + count || gsq >= AI_MAX_SQUADS) return 0;
	if (r->kind == 3) return ai->st.squad[gsq].platoon == (uint8_t)r->platoon;
	return 1;
}

int ai_actor_in_ref(const ai_t *ai, const ai_actor *a, const ai_ref *r)
{
	if (!a->active || r->kind == 0) return 0;
	if (a->enc != (uint16_t)r->enc) return 0;
	if (r->kind == 2) return a->squad == ai->st.enc[r->enc].first_squad + (unsigned)r->squad;
	if (r->kind == 3) return a->platoon == (uint16_t)r->platoon;
	return 1;
}

/* ---- difficulty ----------------------------------------------------------------------------------------------------------------------- */
static float dif(const ai_t *ai, unsigned group, int level, float neutral)
{
	const unitdef_section *u = ai->env.game ? ai->env.game->unit : NULL;
	float v;
	if (!u || !u->data) return neutral;
	v = unitdef_difficulty(u, group, (uint32_t)level);
	return v;
}
static float dif_mul(const ai_t *ai, unsigned group, int level)
{
	float v = dif(ai, group, level, 1.0f);
	return v > 0.0f ? v : 1.0f;
}

void ai_set_difficulty(ai_t *ai, int d)
{
	ai_diff *f = &ai->st.diff;
	if (d < 0) d = 0;
	if (d > 3) d = 3;
	ai->st.difficulty = (uint8_t)d;
	f->vit_enemy = dif_mul(ai, UNITDEF_D_ENEMY_VITALITY, d);
	f->shield_enemy = dif_mul(ai, UNITDEF_D_ENEMY_SHIELD, d);
	f->recharge_enemy = dif_mul(ai, UNITDEF_D_ENEMY_RECHARGE, d);
	f->vit_friend = dif_mul(ai, UNITDEF_D_FRIEND_VITALITY, d);
	f->shield_friend = dif_mul(ai, UNITDEF_D_FRIEND_SHIELD, d);
	f->recharge_friend = dif_mul(ai, UNITDEF_D_FRIEND_RECHARGE, d);
	f->rof = dif_mul(ai, UNITDEF_D_RATE_OF_FIRE, d);
	f->error = dif_mul(ai, UNITDEF_D_PROJECTILE_ERROR, d);
	f->burstiness = dif_mul(ai, UNITDEF_D_BURSTINESS, d);
	f->new_target = dif_mul(ai, UNITDEF_D_NEW_TARGET_DELAY, d);
	f->burst_sep = dif_mul(ai, UNITDEF_D_BURST_SEPARATION, d);
	f->track_add = dif(ai, UNITDEF_D_TARGET_TRACKING, d, 0.0f);
	f->lead_add = dif(ai, UNITDEF_D_TARGET_LEADING, d, 0.0f);
	f->gren_chance = dif_mul(ai, UNITDEF_D_GRENADE_CHANCE_SCALE, d);
	f->gren_timer = dif_mul(ai, UNITDEF_D_GRENADE_TIMER_SCALE, d);
	f->melee_delay = dif_mul(ai, UNITDEF_D_MELEE_DELAY_SCALE, d);
}

/* ---- profiles: a digest of variant + actor + weapon ---------------------------------------------------------------------------------- */
static void profile_fill(ai_t *ai, ai_profile *p, uint32_t variant_tag)
{
	aidef_variant_def v;
	aidef_actor_def a;
	unitdef_biped_def bd;
	weap_weapon w;
	weap_projectile pr;
	const game_t *g = ai->env.game;
	int has_v, has_a, has_b = 0;
	memset(p, 0, sizeof *p);
	p->variant_tag = variant_tag;
	p->weapon = -1;
	has_v = aidef_actor_variant(ai->env.aidef, variant_tag, &v);
	if (!has_v) memset(&v, 0, sizeof v);
	has_a = has_v && aidef_variant_actor(ai->env.aidef, &v, &a);
	if (!has_a) memset(&a, 0, sizeof a);
	if (g && g->unit && v.unit_tag) has_b = unitdef_biped(g->unit, v.unit_tag, &bd);
	if (!has_b) memset(&bd, 0, sizeof bd);
	p->type = (uint8_t)(has_a ? a.type : AIDEF_ACTOR_NONE);
	p->movement_type = (uint8_t)v.movement_type;
	p->vision_dist = has_a && a.max_vision_distance > 0.0f ? a.max_vision_distance : 20.0f;
	p->cos_central = cosf(has_a ? a.central_vision_angle : 0.873f);
	p->cos_max = cosf(has_a ? a.max_vision_angle : 1.222f);
	p->cos_periph = cosf(has_a ? a.peripheral_vision_angle : 2.094f);
	p->periph_dist = has_a ? a.peripheral_distance : 3.0f;
	p->hearing = has_a && a.hearing_distance > 0.0f ? a.hearing_distance : 30.0f;
	p->t_combat = has_a ? a.combat_perception_time : 0.2f;
	p->t_guard = has_a ? a.guard_perception_time : 0.6f;
	p->t_idle = has_a ? a.non_combat_perception_time : 1.5f;
	p->fire_dist = v.max_firing_distance > 0.0f ? v.max_firing_distance : 12.0f;
	p->rof = v.rate_of_fire;
	p->error = v.projectile_error;
	p->rmin = v.desired_combat_range[0];
	p->rmax = v.desired_combat_range[1];
	if (p->rmax < 1.0f) p->rmax = p->fire_dist * 0.6f;
	if (p->rmin > p->rmax - 0.5f) p->rmin = p->rmax * 0.5f;
	p->first_delay[0] = v.first_burst_delay[0]; p->first_delay[1] = v.first_burst_delay[1];
	p->burst_dur[0] = v.burst_duration[0]; p->burst_dur[1] = v.burst_duration[1];
	p->burst_sep[0] = v.burst_separation[0]; p->burst_sep[1] = v.burst_separation[1];
	if (p->burst_dur[1] <= 0.0f) { p->burst_dur[0] = 0.8f; p->burst_dur[1] = 1.6f; }
	p->burst_radius = v.burst_origin_radius;
	p->return_len = 0.5f * (v.burst_return_length[0] + v.burst_return_length[1]);
	p->track = v.target_tracking;
	p->lead = v.target_leading;
	p->crouch_chance = v.initial_crouch_chance;
	p->crouch_t[0] = v.crouch_time[0]; p->crouch_t[1] = v.crouch_time[1];
	p->run_t[0] = v.run_time[0]; p->run_t[1] = v.run_time[1];
	p->gren_chance = v.grenade_chance;
	p->gren_check = v.grenade_check_time > 0.0f ? v.grenade_check_time : 1.0f;
	p->gren_range[0] = v.grenade_range[0]; p->gren_range[1] = v.grenade_range[1];
	p->gren_timeout = v.encounter_grenade_timeout;
	p->grenade_type = (uint8_t)(v.grenade_type & 1u);
	p->grenade_min_enemies = (uint8_t)v.grenade_min_enemies;
	p->gren_count[0] = v.grenade_count[0]; p->gren_count[1] = v.grenade_count[1];
	p->body = v.body_vitality;
	p->shield = v.shield_vitality;
	p->recharge = has_b ? bd.shield_recharge_rate : 1.0f / 120.0f;
	p->stun = has_b ? bd.shield_stun_time : 4.0f;
	if (p->body <= 0.0f && has_b) { p->body = bd.health; p->shield = bd.shield; }
	/* weapon: the variant's, else the biped's first default */
	p->weapon_tag = v.weapon_tag ? v.weapon_tag : (has_b && bd.weapon_count ? bd.weapon_tags[0] : 0);
	p->duty = 1.0f;
	if (g && g->weap && p->weapon_tag) {
		p->weapon = (int16_t)weap_find_weapon(g->weap, p->weapon_tag);
		if (p->weapon >= 0 && weap_get_weapon(g->weap, (uint32_t)p->weapon, &w)) {
			p->armed = 1;
			/* ammunition accounting only for weapons that spend rounds (the energy weapons list a magazine with 0 rounds per shot and 0 capacity) */
			p->mag = 0xFFu;
			if (w.trigger_count > 0 && w.trigger[0].rounds_per_shot > 0 && w.trigger[0].magazine < w.magazine_count && w.trigger[0].magazine < WEAP_MAX_MAGAZINES &&
			    w.magazine[w.trigger[0].magazine].rounds_loaded_max > 0) p->mag = (uint8_t)w.trigger[0].magazine;
			if (w.trigger_count > 0 && w.trigger[0].projectile) {
				int pi = weap_find_projectile(g->weap, w.trigger[0].projectile);
				if (pi >= 0 && weap_get_projectile(g->weap, (uint32_t)pi, &pr)) p->proj_speed = pr.initial_velocity;
				if (w.trigger[0].rps_max > 0.0f && p->rof > 0.0f) {
					float duty = p->rof / w.trigger[0].rps_max;
					p->duty = duty < 1.0f ? duty : 1.0f;
				}
			}
		}
	}
}

static int profile_get(ai_t *ai, uint32_t variant_tag)
{
	unsigned i;
	for (i = 0; i < ai->st.prof_n; i++) if (ai->st.prof[i].variant_tag == variant_tag) return (int)i;
	if (ai->st.prof_n >= AI_MAX_PROFILES) return -1;
	profile_fill(ai, &ai->st.prof[ai->st.prof_n], variant_tag);
	return (int)ai->st.prof_n++;
}

/* ---- lifecycle ------------------------------------------------------------------------------------------------------------------------ */
static void cache_encounters(ai_t *ai)
{
	const aidef_section *s = ai->env.aidef;
	unsigned i, n = s ? aidef_encounter_count(s) : 0, sq = 0;
	if (n > AI_MAX_ENCOUNTERS) n = AI_MAX_ENCOUNTERS;
	for (i = 0; i < n; i++) {
		aidef_encounter_def e;
		ai_enc_rt *r = &ai->st.enc[i];
		unsigned k;
		if (!aidef_get_encounter(s, i, &e)) continue;
		r->first_squad = (uint16_t)e.first_squad;
		r->squad_count = (uint16_t)(e.squad_count);
		r->first_firing = (uint16_t)e.first_firing;
		r->firing_count = (uint16_t)(e.firing_count > 0xFFFFu ? 0xFFFFu : e.firing_count);
		r->bsp = (uint16_t)((e.flags & AIDEF_ENC_MANUAL_BSP_INDEX) ? e.manual_bsp : e.precomputed_bsp);
		if (e.first_squad + e.squad_count > AI_MAX_SQUADS) r->squad_count = (uint16_t)(e.first_squad < AI_MAX_SQUADS ? AI_MAX_SQUADS - e.first_squad : 0);
		for (k = 0; k < r->squad_count; k++) {
			aidef_squad_def sd;
			ai_squad_rt *q = &ai->st.squad[e.first_squad + k];
			q->enc = (uint16_t)i;
			q->platoon = 0xFFu;
			q->follow = -1;
			if (aidef_get_squad(s, e.first_squad + k, &sd) && sd.platoon >= 0 && sd.platoon < 255) q->platoon = (uint8_t)sd.platoon;
		}
		sq += r->squad_count;
	}
	(void)sq;
}

void ai_init(ai_t *ai, const ai_env *env)
{
	memset(ai, 0, sizeof *ai);
	ai->env = *env;
	ai->max_routes_per_tick = 1;
	ai->st.magic = 0x54494148u;          /* "HAIT" */
	ai->st.version = AI_STATE_VERSION;
	ai->st.rng = 0x9E3779B9u;
	ai->st.enabled = 1; ai->st.follow_default = 1; ai->st.grenades = 1; ai->st.dialogue = 1;
	if (env->aidef) {
		ai->st.enc_count = aidef_encounter_count(env->aidef);
		ai->st.squad_count = aidef_squad_count(env->aidef);
		ai->st.conv_count = aidef_conversation_count(env->aidef);
	}
	{
		unsigned i;
		for (i = 0; i < AI_MAX_SQUADS; i++) ai->st.squad[i].follow = -1;
		for (i = 0; i < AI_MAX_ACTORS; i++) ai->st.actor[i].tgt = AI_NO_TARGET;
	}
	ai_set_difficulty(ai, env->game ? env->game->difficulty : 1);
	cache_encounters(ai);
}

ai_state *ai_state_ptr(ai_t *ai) { return &ai->st; }
size_t ai_state_size(void) { return sizeof(ai_state); }

int ai_rebind(ai_t *ai, const ai_env *env)
{
	unsigned i;
	int ok = 1;
	ai->env = *env;
	if (ai->st.magic != 0x54494148u || ai->st.version != AI_STATE_VERSION) ok = 0;
	if (ok && env->aidef && (ai->st.enc_count != aidef_encounter_count(env->aidef) || ai->st.squad_count != aidef_squad_count(env->aidef) ||
	                         ai->st.conv_count != aidef_conversation_count(env->aidef))) ok = 0;
	if (ok && ai->st.prof_n > AI_MAX_PROFILES) ok = 0;
	for (i = 0; ok && i < AI_MAX_ACTORS; i++) {
		const ai_actor *a = &ai->st.actor[i];
		if (!a->active) continue;
		if (a->unit < 0 || a->unit >= UNIT_MAX || a->obj < 0 || a->obj >= OBJ_MAX || a->enc >= AI_MAX_ENCOUNTERS || a->squad >= AI_MAX_SQUADS ||
		    a->prof >= ai->st.prof_n || a->st >= AIS_STATES || a->wp_n > AI_WAYPOINTS || a->wp_i > AI_WAYPOINTS || a->tgt < AI_NO_TARGET ||
		    a->tgt > (int)UNIT_MAX) ok = 0;
	}
	if (!ok) {
		ai_init(ai, env);
		return -1;
	}
	for (i = 0; i < ai->st.prof_n; i++) {            /* the cache is derived data: rebuild it from the tags */
		uint32_t tag = ai->st.prof[i].variant_tag;
		profile_fill(ai, &ai->st.prof[i], tag);
	}
	cache_encounters(ai);
	return 0;
}

void ai_set_sound_hook(ai_t *ai, ai_sound_fn fn, void *user) { ai->sound = fn; ai->sound_user = user; }
void ai_set_script_hook(ai_t *ai, ai_script_fn fn, void *user) { ai->script = fn; ai->script_user = user; }
void ai_set_object_hook(ai_t *ai, ai_object_fn fn, void *user) { ai->object = fn; ai->object_user = user; }
void ai_set_log_hook(ai_t *ai, ai_log_fn fn, void *user) { ai->log = fn; ai->log_user = user; }

/* ---- targets: the player and every live unit, built once per tick -------------------------------------------------------------------- */
static void build_targets(ai_t *ai)
{
	const units_t *us = ai->env.units;
	game_t *g = ai->env.game;
	unsigned i;
	for (i = 0; i <= UNIT_MAX; i++) ai->tgt[i].valid = 0;
	ai->tgt_n = 0;
	for (i = 0; i < us->count && i < UNIT_MAX; i++) {
		const unit_t *u = &us->u[i];
		const object_t *o;
		if (!u->active || (u->flags & (UF_PLAYER | UF_HIDE_BODY)) || !units_alive(us, g, (int)i) || !obj_live(ai, u->obj)) continue;
		o = &g->objects->obj[u->obj];
		if (o->flags & (OBJ_F_HIDDEN | OBJ_F_INACTIVE)) continue;
		cp3(ai->tgt[i].pos, u->body.pos);
		cp3(ai->tgt[i].chest, u->body.pos);
		ai->tgt[i].chest[2] += 0.55f * (u->pp.height > 0.1f ? u->pp.height : 0.7f);
		ai->tgt[i].vel[0] = u->body.vel[0] / TICKS; ai->tgt[i].vel[1] = u->body.vel[1] / TICKS; ai->tgt[i].vel[2] = 0.0f;
		ai->tgt[i].team = u->team;
		ai->tgt[i].obj = u->obj;
		ai->tgt[i].valid = 1;
		ai->tgt_n++;
	}
	if (!vitals_dead(&g->vitals)) {
		cp3(ai->tgt[UNIT_MAX].pos, g->player.pos);
		cp3(ai->tgt[UNIT_MAX].chest, g->player.pos);
		ai->tgt[UNIT_MAX].chest[2] += 0.55f * (g->pp.height > 0.1f ? g->pp.height : 0.7f);
		ai->tgt[UNIT_MAX].vel[0] = g->player.vel[0] / TICKS; ai->tgt[UNIT_MAX].vel[1] = g->player.vel[1] / TICKS; ai->tgt[UNIT_MAX].vel[2] = 0.0f;
		ai->tgt[UNIT_MAX].team = AIDEF_TEAM_PLAYER;
		ai->tgt[UNIT_MAX].obj = (int16_t)g->player_obj;
		ai->tgt[UNIT_MAX].valid = 1;
		ai->tgt_n++;
	}
}

static int disregarded(const ai_t *ai, int t)
{
	return t >= 0 && t < (int)UNIT_MAX && (ai->st.disregard[t >> 5] >> (t & 31)) & 1u;
}
void ai_disregard(ai_t *ai, int unit, int on)
{
	if (unit < 0 || unit >= (int)UNIT_MAX) return;
	if (on) ai->st.disregard[unit >> 5] |= 1u << (unit & 31); else ai->st.disregard[unit >> 5] &= ~(1u << (unit & 31));
}

/* a target the actor may act against */
static int hostile_target(const ai_t *ai, const ai_actor *a, int t)
{
	if (t < 0 || t > (int)UNIT_MAX || !ai->tgt[t].valid) return 0;
	if (t < (int)UNIT_MAX && t == a->unit) return 0;
	if (a->flags & AIF_IGNORE_ALL) return 0;
	if (t == (int)UNIT_MAX && (a->flags & AIF_IGNORE_PLAYER)) return 0;
	if (disregarded(ai, t)) return 0;
	return ai_teams_hostile(ai, a->team, ai->tgt[t].team);
}

/* line of sight between two points through the collision BSP */
static int los_clear(ai_t *ai, const float a[3], const float b[3])
{
	coll_hit h;
	if (!ai->env.game || !ai->env.game->coll) return 1;
	ai->st.st_los_rays++;
	return !coll_ray(ai->env.game->coll, a, b, 0, &h);
}

static void eye_of(const unit_t *u, float eye[3])
{
	float h = u->body.crouch > 0.5f ? u->pp.crouch_eye_height : u->pp.eye_height;
	if (h < 0.2f) h = 0.55f;
	eye[0] = u->body.pos[0]; eye[1] = u->body.pos[1]; eye[2] = u->body.pos[2] + h;
}

/* ---- actors --------------------------------------------------------------------------------------------------------------------------- */
static int actor_alloc(ai_t *ai)
{
	unsigned i;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (!ai->st.actor[i].active) return (int)i;
	return -1;
}

static void set_state(ai_t *ai, ai_actor *a, unsigned st)
{
	if (a->st == st) return;
	a->st = (uint8_t)st;
	a->state_tick = ai->st.tick;
	a->wander_tick = 0; a->dwell_until = 0;
	if (st < AIS_STATES) ai->st.st_state_changes[st]++;
}

/* the behaviour a squad state (initial / return) stands for */
static unsigned state_for(unsigned aidef_state)
{
	switch (aidef_state) {
	case AIDEF_STATE_SLEEPING: return AIS_SLEEP;
	case AIDEF_STATE_MOVING_REPEAT_SAME_POSITION: return AIS_IDLE;
	case AIDEF_STATE_MOVING_LOOP: case AIDEF_STATE_MOVING_LOOP_BACK_AND_FORTH: case AIDEF_STATE_MOVING_LOOP_RANDOMLY:
	case AIDEF_STATE_MOVING_RANDOMLY: return AIS_PATROL;
	case AIDEF_STATE_SEARCHING: return AIS_SEARCH;
	case AIDEF_STATE_FLEEING: return AIS_FLEE;
	default: return AIS_IDLE;
	}
}

static void actor_free(ai_t *ai, ai_actor *a)
{
	(void)ai;
	memset(a, 0, sizeof *a);
	a->tgt = AI_NO_TARGET;
	a->cl_list = -1;
	a->fp = -1;
}

static void actor_init(ai_t *ai, ai_actor *a, int unit, int obj, unsigned enc, unsigned gsq, int platoon, unsigned init_state, unsigned ret_state,
                       int prof, const float pos[3], float yaw, unsigned slot)
{
	const ai_squad_rt *q = &ai->st.squad[gsq < AI_MAX_SQUADS ? gsq : 0];
	memset(a, 0, sizeof *a);
	a->active = 1;
	a->unit = (int16_t)unit; a->obj = (int16_t)obj;
	a->team = ai->env.units->u[unit].team;
	a->prof = (uint8_t)prof;
	a->enc = (uint16_t)enc; a->squad = (uint16_t)gsq;
	a->platoon = platoon >= 0 ? (uint16_t)platoon : AI_NONE16;
	a->bsp = (uint8_t)ai->env.bsp;
	a->slot = (uint8_t)slot;
	a->init_state = (uint8_t)init_state; a->ret_state = (uint8_t)ret_state;
	a->flags = (uint8_t)(q->flags | ai->st.enc[enc < AI_MAX_ENCOUNTERS ? enc : 0].flags);
	a->orders = gsq < AI_MAX_SQUADS ? q->orders : 0;
	a->spawn_tick = ai->st.tick;
	cp3(a->home, pos); a->home_yaw = yaw;
	cp3(a->goal, pos); cp3(a->prog_pos, pos); cp3(a->tgt_pos, pos);
	a->tgt = AI_NO_TARGET;
	a->cl_list = -1;
	a->fp = -1;
	a->aim[0] = yaw;
	a->strafe_dir = 1.0f;
	a->st = (uint8_t)state_for(init_state);
	a->state_tick = ai->st.tick;
	a->last_hp = 0.0f;
	if (a->orders == AIO_NONE && gsq < AI_MAX_SQUADS && q->state_override) a->st = (uint8_t)state_for(q->state_override);
}

/* ---- spawning ------------------------------------------------------------------------------------------------------------------------- */
static void destroy_object(ai_t *ai, int obj)
{
	game_t *g = ai->env.game;
	if (!obj_live(ai, obj)) return;
	units_detach(ai->env.units, units_of_object(ai->env.units, obj));
	objects_destroy(g->objects, (unsigned)obj);
	g->unit_state[obj].model = -1; g->unit_state[obj].has_vitals = 0;
}

/* Creates the object + unit + actor of one start location. Returns the actor index or -1. */
static int spawn_actor(ai_t *ai, unsigned enc, unsigned gsq, const aidef_squad_def *sq, const aidef_encounter_def *e,
                       const aidef_variant_def *var, const aidef_start_def *st, unsigned slot, float off)
{
	game_t *g = ai->env.game;
	const scen_section *scen = scen_of(ai);
	float pos[3];
	int def, obj, unit, ai_i, prof;
	unsigned team;
	ai_actor *a;
	unit_t *u;
	if (!scen || !g || !g->objects || !ai->env.units) return -1;
	def = scen_find_def(scen, var->unit_tag);
	if (def < 0) {
		ai->st.st_skipped_no_def++;
		logmsg(ai, "ai: actor variant unit has no object definition in SCEN (extend the objpack roots)");
		return -1;
	}
	ai_i = actor_alloc(ai);
	if (ai_i < 0) { ai->st.st_spawn_fail++; return -1; }
	prof = profile_get(ai, var->tag_id);
	if (prof < 0) { ai->st.st_spawn_fail++; return -1; }
	pos[0] = st->pos[0] + off; pos[1] = st->pos[1] + off * 0.5f; pos[2] = st->pos[2];
	if (ai->env.path && enc < AI_MAX_ENCOUNTERS && ai->st.enc[enc].bsp == (uint16_t)ai->env.bsp) {
		float fz;
		if (path_locate_ex(ai->env.path, pos, 0.6f, 2.0f, &fz) >= 0) pos[2] = fz + 0.01f;
	}
	obj = objects_spawn_def(g->objects, &g->ow, (unsigned)def, pos, st->facing);
	if (obj < 0) { ai->st.st_spawn_fail++; return -1; }
	g->objects->obj[obj].bsp_mask = 1u << (ai->env.bsp & 31);
	g->objects->obj[obj].flags &= (uint8_t)~OBJ_F_INACTIVE;
	game_object_spawned(g, (unsigned)obj);
	team = e->team;
	unit = units_attach(ai->env.units, g, obj, (int)team, var->weapon_tag ? (int32_t)var->weapon_tag : 0);
	if (unit < 0) { destroy_object(ai, obj); ai->st.st_spawn_fail++; return -1; }
	u = unit_at(ai, unit);
	units_apply_variant(ai->env.units, g, unit, var);          /* health / shield by the difficulty table, weapon, grenades, projectile error */
	if (!ai->st.grenades) u->grenades[0] = u->grenades[1] = 0;
	units_teleport(ai->env.units, g, unit, pos, st->facing);
	a = &ai->st.actor[ai_i];
	actor_init(ai, a, unit, obj, enc, gsq, sq->platoon, sq->initial_state, sq->return_state, prof, pos, st->facing, slot);
	if (st->command_list >= 0) { a->cl_list = st->command_list; a->cl_status = 1; a->cl_forced = 0; a->cl_radius = ARRIVE_R; }
	a->last_hp = g->unit_state[obj].vitals.health + g->unit_state[obj].vitals.shield;
	ai->st.st_placed++;
	return ai_i;
}

static unsigned enc_bsp(const ai_t *ai, unsigned enc) { return enc < AI_MAX_ENCOUNTERS ? ai->st.enc[enc].bsp : 0u; }

/* spawn the actors of one squad now; `count` 0 = the difficulty's count. Returns actors created. */
static int place_squad_now(ai_t *ai, unsigned enc, unsigned gsq, unsigned count)
{
	aidef_squad_def sq;
	aidef_encounter_def e;
	aidef_variant_def var;
	ai_squad_rt *q = &ai->st.squad[gsq];
	unsigned n, k, made = 0, nstart;
	uint16_t order[64];
	const aidef_section *s = ai->env.aidef;
	if (!aidef_get_squad(s, gsq, &sq) || !aidef_get_encounter(s, enc, &e)) return 0;
	q->placed = 1;
	q->spawn_at = 0;
	if (!aidef_squad_variant(s, &sq, &var) || !var.unit_tag) { if (sq.actor_type >= 0) ai->st.st_skipped_no_variant++; return 0; }
	n = count ? count : aidef_squad_spawn_count(&sq, ai->st.difficulty);
	nstart = sq.start_count;
	if (n == 0 || nstart == 0) return 0;
	if (n > 32) n = 32;
	/* which start locations: a deterministic random subset when the squad has more than it spawns, cyclic reuse otherwise */
	{
		unsigned m = nstart < 64 ? nstart : 64;
		for (k = 0; k < m; k++) order[k] = (uint16_t)k;
		for (k = 0; k + 1 < m && k < n; k++) {
			unsigned j = k + rnd(ai) % (m - k);
			uint16_t t = order[k]; order[k] = order[j]; order[j] = t;
		}
		for (k = 0; k < n; k++) {
			aidef_start_def st;
			unsigned slot = order[k % m];
			if (count) slot = (q->next_start++) % m;
			if (aidef_get_start(s, &sq, slot, &st) && spawn_actor(ai, enc, gsq, &sq, &e, &var, &st, slot, k >= m ? 0.4f * (float)(k / m) : 0.0f) >= 0) made++;
		}
	}
	q->spawned = (uint16_t)(q->spawned + made);
	ai->st.enc[enc].spawned = (uint16_t)(ai->st.enc[enc].spawned + made);
	return (int)made;
}

static unsigned squad_alive_n(const ai_t *ai, unsigned gsq)
{
	unsigned i, n = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai->st.actor[i].active && ai->st.actor[i].squad == gsq) n++;
	return n;
}

static int place_encounter(ai_t *ai, unsigned enc, int only_squad, int only_platoon)
{
	aidef_encounter_def e;
	ai_enc_rt *r;
	unsigned k, made = 0;
	if (enc >= AI_MAX_ENCOUNTERS || !aidef_get_encounter(ai->env.aidef, enc, &e)) return 0;
	r = &ai->st.enc[enc];
	if (enc_bsp(ai, enc) != (unsigned)ai->env.bsp) {          /* another BSP is loaded: place when it is */
		r->pending = 1;
		return 0;
	}
	r->pending = 0;
	r->placed = 1;
	for (k = 0; k < r->squad_count; k++) {
		unsigned gsq = (unsigned)r->first_squad + k;
		ai_squad_rt *q = &ai->st.squad[gsq];
		aidef_squad_def sq;
		if (only_squad >= 0 && (int)k != only_squad) continue;
		if (only_platoon >= 0 && q->platoon != (uint8_t)only_platoon) continue;
		if (q->placed && (squad_alive_n(ai, gsq) > 0 || q->spawn_at)) continue;      /* already out there */
		if (q->placed) { q->spawned = 0; }                                          /* wiped out: a fresh set */
		if (!aidef_get_squad(ai->env.aidef, gsq, &sq)) continue;
		if (sq.delay_time > 0.0f && !(sq.flags & AIDEF_SQUAD_NO_TIMER_DELAY)) {
			q->placed = 1;
			q->spawn_at = ai->st.tick + secs(sq.delay_time) + 1u;
			continue;
		}
		made += (unsigned)place_squad_now(ai, enc, gsq, 0);
	}
	return (int)made;
}

int ai_place(ai_t *ai, const ai_ref *r)
{
	if (r->kind == 0) return 0;
	return place_encounter(ai, (unsigned)r->enc, r->kind == 2 ? r->squad : -1, r->kind == 3 ? r->platoon : -1);
}

int ai_place_all_initial(ai_t *ai)
{
	unsigned i, made = 0;
	for (i = 0; i < ai->st.enc_count && i < AI_MAX_ENCOUNTERS; i++) {
		aidef_encounter_def e;
		if (!aidef_get_encounter(ai->env.aidef, i, &e) || (e.flags & AIDEF_ENC_NOT_INITIALLY_CREATED)) continue;
		made += (unsigned)place_encounter(ai, i, -1, -1);
	}
	return (int)made;
}

void ai_set_bsp(ai_t *ai, int bsp, const path_bsp *path, path_scratch *scratch)
{
	unsigned i;
	ai->env.bsp = bsp;
	ai->env.path = path;
	ai->env.scratch = scratch;
	for (i = 0; i < AI_MAX_ACTORS; i++) {          /* routes of the old mesh are meaningless */
		ai->st.actor[i].wp_n = 0; ai->st.actor[i].wp_i = 0; ai->st.actor[i].goal_valid = 0;
	}
	for (i = 0; i < AI_MAX_ENCOUNTERS; i++)
		if (ai->st.enc[i].pending && enc_bsp(ai, i) == (unsigned)bsp) place_encounter(ai, i, -1, -1);
}

int ai_spawn_one(ai_t *ai, const ai_ref *r)
{
	unsigned gsq;
	int before, made;
	if (r->kind != 2 && r->kind != 1) return -1;
	gsq = ai->st.enc[r->enc].first_squad + (unsigned)(r->kind == 2 ? r->squad : 0);
	if (enc_bsp(ai, (unsigned)r->enc) != (unsigned)ai->env.bsp) return -1;
	before = (int)squad_alive_n(ai, gsq);
	made = place_squad_now(ai, (unsigned)r->enc, gsq, 1);
	ai->st.enc[r->enc].placed = 1;
	(void)before;
	if (made <= 0) return -1;
	{
		unsigned i;
		for (i = AI_MAX_ACTORS; i-- > 0;) if (ai->st.actor[i].active && ai->st.actor[i].squad == gsq) return (int)i;
	}
	return -1;
}

int ai_attach_unit(ai_t *ai, int unit, const ai_ref *r, uint32_t variant_tag)
{
	unit_t *u;
	int ai_i, prof;
	float yaw;
	if (!ai->env.units || unit < 0 || unit >= (int)UNIT_MAX || !ai->env.units->u[unit].active) return -1;
	if (ai_actor_of_unit(ai, unit) >= 0) return -1;
	u = unit_at(ai, unit);
	ai_i = actor_alloc(ai);
	if (ai_i < 0) return -1;
	if (!variant_tag) variant_tag = u->variant_tag;
	prof = profile_get(ai, variant_tag);
	if (prof < 0) {                                  /* no variant: the first one of the palette that uses this biped, else the first variant */
		prof = ai->st.prof_n ? 0 : -1;
		if (prof < 0) return -1;
	}
	yaw = u->body.yaw;
	{
		unsigned enc = r && r->kind ? (unsigned)r->enc : 0u;
		unsigned gsq = r && r->kind ? ai->st.enc[enc].first_squad + (unsigned)(r->kind == 2 ? r->squad : 0) : 0u;
		actor_init(ai, &ai->st.actor[ai_i], unit, u->obj, enc, gsq, -1, AIDEF_STATE_NONE, AIDEF_STATE_NONE, prof, u->body.pos, yaw, 0);
	}
	u->flags &= (uint8_t)~UF_NO_AI;
	return ai_i;
}

void ai_detach_unit(ai_t *ai, int unit)
{
	int i = ai_actor_of_unit(ai, unit);
	if (i < 0) return;
	actor_free(ai, &ai->st.actor[i]);
	if (ai->env.units && unit >= 0 && unit < (int)UNIT_MAX) {
		unit_control_t c;
		memset(&c, 0, sizeof c);
		c.aim_yaw = ai->env.units->u[unit].body.yaw;
		c.weapon_slot = -1;
		units_set_control(ai->env.units, unit, &c);
	}
}

int ai_actor_of_unit(const ai_t *ai, int unit)
{
	unsigned i;
	if (unit < 0) return -1;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai->st.actor[i].active && ai->st.actor[i].unit == unit) return (int)i;
	return -1;
}
int ai_actor_of_object(const ai_t *ai, int obj)
{
	unsigned i;
	if (obj < 0) return -1;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai->st.actor[i].active && ai->st.actor[i].obj == obj) return (int)i;
	return -1;
}
unsigned ai_active_actors(const ai_t *ai)
{
	unsigned i, n = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) n += ai->st.actor[i].active;
	return n;
}

void ai_kill(ai_t *ai, const ai_ref *r, int silent)
{
	unsigned i;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, r)) continue;
		units_kill(ai->env.units, ai->env.game, a->unit, silent);
		ai->st.st_deaths++;
		actor_free(ai, a);
	}
}

void ai_erase(ai_t *ai, const ai_ref *r)
{
	unsigned i, first, count;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, r)) continue;
		destroy_object(ai, a->obj);
		actor_free(ai, a);
	}
	if (ai_ref_squad_range(ai, r, &first, &count)) {
		for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) {
			if (!ai_ref_has_squad(ai, r, i)) continue;
			ai->st.squad[i].placed = 0; ai->st.squad[i].spawned = 0; ai->st.squad[i].spawn_at = 0;
		}
		if (r->kind == 1) { ai->st.enc[r->enc].placed = 0; ai->st.enc[r->enc].spawned = 0; ai->st.enc[r->enc].pending = 0; ai->st.enc[r->enc].activated = 0; }
	}
}

void ai_erase_all(ai_t *ai)
{
	unsigned e;
	for (e = 0; e < ai->st.enc_count && e < AI_MAX_ENCOUNTERS; e++) {
		ai_ref r;
		memset(&r, 0, sizeof r);
		r.kind = 1; r.enc = (int16_t)e; r.squad = r.platoon = -1;
		ai_erase(ai, &r);
	}
}

void ai_timer_expire(ai_t *ai, const ai_ref *r)
{
	unsigned i, first, count;
	if (!ai_ref_squad_range(ai, r, &first, &count)) return;
	for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) {
		ai_squad_rt *q = &ai->st.squad[i];
		if (!ai_ref_has_squad(ai, r, i) || !q->spawn_at) continue;
		q->spawn_at = 0;
		if (enc_bsp(ai, q->enc) == (unsigned)ai->env.bsp) place_squad_now(ai, q->enc, i, 0);
	}
}

void ai_renew(ai_t *ai, const ai_ref *r)
{
	unsigned i, first, count;
	if (!ai_ref_squad_range(ai, r, &first, &count)) return;
	for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) {
		ai_squad_rt *q = &ai->st.squad[i];
		if (!ai_ref_has_squad(ai, r, i) || !q->placed || q->spawn_at) continue;
		if (enc_bsp(ai, q->enc) != (unsigned)ai->env.bsp) continue;
		if (squad_alive_n(ai, i) == 0) { q->spawned = 0; place_squad_now(ai, q->enc, i, 0); }
	}
}

void ai_teleport_home(ai_t *ai, const ai_ref *r)
{
	unsigned i;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, r)) continue;
		units_teleport(ai->env.units, ai->env.game, a->unit, a->home, a->home_yaw);
		a->wp_n = 0; a->goal_valid = 0;
	}
}

/* ---- noises, alerts, perception ------------------------------------------------------------------------------------------------------- */
static void push_noise(ai_t *ai, const float pos[3], int team, int src)
{
	ai_noise *n;
	if (ai->st.noise_head >= AI_MAX_NOISES) return;
	n = &ai->st.noise[AI_MAX_NOISES + ai->st.noise_head++];
	cp3(n->pos, pos);
	n->tick = ai->st.tick; n->team = (uint8_t)team; n->src = (int16_t)src;
	ai->st.st_noises++;
}

/* Start of a tick: last tick's noises become readable, the shots of the player (and of units not driven by the AI) in the game's event
 * list are added. AI actors push their own noise when they fire, so their GE_FIRE events are skipped. */
static void begin_noises(ai_t *ai)
{
	unsigned n = ai->st.noise_head, i, ne = 0;
	const game_event_t *ev;
	for (i = 0; i < n; i++) ai->st.noise[i] = ai->st.noise[AI_MAX_NOISES + i];
	ai->st.noise_head = 0;
	ai->noise_read = n;
	ev = game_events(ai->env.game, &ne);
	for (i = 0; i < ne && ai->noise_read < AI_MAX_NOISES; i++) {
		unsigned t;
		int best = -1;
		float bd = NOISE_R * NOISE_R;
		ai_noise *nz;
		if (ev[i].type != GE_FIRE) continue;
		for (t = 0; t <= UNIT_MAX; t++) {
			float dx, dy, dz, d2;
			if (!ai->tgt[t].valid) continue;
			dx = ev[i].pos[0] - ai->tgt[t].chest[0]; dy = ev[i].pos[1] - ai->tgt[t].chest[1]; dz = ev[i].pos[2] - ai->tgt[t].chest[2];
			d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < bd) { bd = d2; best = (int)t; }
		}
		if (best >= 0 && best < (int)UNIT_MAX && ai_actor_of_unit(ai, best) >= 0) continue;
		nz = &ai->st.noise[ai->noise_read++];
		cp3(nz->pos, ev[i].pos);
		nz->tick = ai->st.tick; nz->team = (uint8_t)(best >= 0 ? ai->tgt[best].team : AIDEF_TEAM_PLAYER); nz->src = (int16_t)best;
		ai->st.st_noises++;
	}
}

static void alert_actor(ai_t *ai, ai_actor *b, const float pos[3])
{
	if (b->aware >= 2 || (b->flags & (AIF_BRAINDEAD | AIF_IGNORE_ALL))) return;
	b->aware = 1;
	cp3(b->tgt_pos, pos);
	b->tgt_seen_tick = ai->st.tick;
	if (b->notice < 0.5f) b->notice = 0.5f;
	if (b->st == AIS_SLEEP) set_state(ai, b, AIS_ALERT);
}

/* an actor of the squad's encounter noticed something: its squad mates and the actors near it learn where */
static void alert_others(ai_t *ai, const ai_actor *src, const float pos[3])
{
	unsigned i;
	const float *sp = unit_at(ai, src->unit)->body.pos;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *b = &ai->st.actor[i];
		float d;
		if (!b->active || b == src || b->bsp != src->bsp) continue;
		d = dist3(unit_at(ai, b->unit)->body.pos, sp);
		if ((b->enc == src->enc && (b->squad == src->squad || d < 30.0f)) || (b->team == src->team && d < 10.0f)) alert_actor(ai, b, pos);
	}
	ai->st.st_alerts++;
}

static void activate_encounter(ai_t *ai, unsigned enc, const float pos[3], unsigned depth)
{
	unsigned i;
	ai_enc_rt *e;
	if (enc >= AI_MAX_ENCOUNTERS || depth > 3) return;
	e = &ai->st.enc[enc];
	if (e->activated) return;
	e->activated = 1; e->activated_tick = ai->st.tick;
	for (i = 0; i < ai->st.link_n; i++) {
		if (ai->st.link[i][0] != (int16_t)enc) continue;
		{
			unsigned k, b = (unsigned)ai->st.link[i][1];
			for (k = 0; k < AI_MAX_ACTORS; k++) if (ai->st.actor[k].active && ai->st.actor[k].enc == b) alert_actor(ai, &ai->st.actor[k], pos);
			activate_encounter(ai, b, pos, depth + 1u);
		}
	}
}

static void acquire(ai_t *ai, ai_actor *a, int t)
{
	const ai_profile *p = &ai->st.prof[a->prof];
	int newly = a->aware < 2 || a->tgt != t;
	a->tgt = (int16_t)t;
	a->aware = 2;
	a->notice = 1.0f;
	if (!newly) return;
	ai->st.st_noticed++;
	a->bstate = 0;
	a->btimer = (uint16_t)(10u + secs(rrange(ai, p->first_delay[0], p->first_delay[1]) * ai->st.diff.new_target));
	a->fp = -1; a->fp_tick = 0;
	a->crouching = (uint8_t)(rf(ai) < p->crouch_chance || p->movement_type == 1);
	a->crouch_timer = (uint16_t)secs(rrange(ai, p->crouch_t[0], p->crouch_t[1]));
	alert_others(ai, a, ai->tgt[t].chest);
	activate_encounter(ai, a->enc, ai->tgt[t].chest, 0);
	{
		aidef_squad_def sq;
		if (a->squad < AI_MAX_SQUADS && aidef_get_squad(ai->env.aidef, a->squad, &sq) && sq.maneuver_squad >= 0 &&
		    (unsigned)sq.maneuver_squad < ai->st.enc[a->enc].squad_count) {
			unsigned g2 = ai->st.enc[a->enc].first_squad + (unsigned)sq.maneuver_squad, k;
			for (k = 0; k < AI_MAX_ACTORS; k++) if (ai->st.actor[k].active && ai->st.actor[k].squad == g2) alert_actor(ai, &ai->st.actor[k], ai->tgt[t].chest);
		}
	}
}

static float notice_time(const ai_actor *a, const ai_profile *p)
{
	float t = p->t_idle;
	if (a->aware == 2 || a->st == AIS_COMBAT || a->st == AIS_CHARGE) t = p->t_combat;
	else if (a->aware == 1 || a->st == AIS_ALERT || a->st == AIS_SEARCH || a->init_state == AIDEF_STATE_ALERT || a->st == AIS_FOLLOW) t = p->t_guard;
	if (a->st == AIS_SLEEP) t *= 3.0f;
	return t < 0.05f ? 0.05f : t;
}

static void perceive(ai_t *ai, ai_actor *a, const unit_t *u)
{
	const ai_profile *p = &ai->st.prof[a->prof];
	float eye[3], fx, fy, vd, vd2, pd2, bestd2 = 0.0f, bestf = 0.0f, dt = (float)PERC_PERIOD / TICKS;
	int best = -1, t;
	if (a->flags & (AIF_BLIND | AIF_BRAINDEAD | AIF_IGNORE_ALL)) { a->tgt_vis = 0; return; }
	eye_of(u, eye);
	fx = cosf(u->body.yaw); fy = sinf(u->body.yaw);
	vd = p->vision_dist * (a->st == AIS_SLEEP ? 0.5f : 1.0f);
	vd2 = vd * vd;
	pd2 = p->periph_dist * p->periph_dist;
	{                                                      /* the nearest few candidates inside the cones, then rays in that order: a bounded number */
		int cand[4];
		float cs[4], cd2[4], cf[4];
		unsigned cn = 0, k;
		for (t = 0; t <= (int)UNIT_MAX; t++) {
			const float *c;
			float dx, dy, dz, d2, dh, cosang, f, score;
			if (!hostile_target(ai, a, t)) continue;
			c = ai->tgt[t].chest;
			dx = c[0] - eye[0]; dy = c[1] - eye[1]; dz = c[2] - eye[2];
			d2 = dx * dx + dy * dy + dz * dz;
			if (d2 > vd2) continue;
			dh = sqrtf(dx * dx + dy * dy);
			cosang = dh > 1e-4f ? (dx * fx + dy * fy) / dh : 1.0f;
			if (cosang >= p->cos_central || (a->aware == 2 && a->tgt == t && d2 < 144.0f)) f = 1.0f;
			else if (cosang >= p->cos_max) f = 0.5f;
			else if (cosang >= p->cos_periph && d2 <= pd2) f = 0.25f;
			else continue;
			score = d2 * ((a->aware == 2 && a->tgt == t) ? 0.5f : 1.0f);
			if (cn < 4 || score < cs[cn - 1]) {
				unsigned pos = cn < 4 ? cn : 3u;
				while (pos > 0 && cs[pos - 1] > score) { cs[pos] = cs[pos - 1]; cand[pos] = cand[pos - 1]; cd2[pos] = cd2[pos - 1]; cf[pos] = cf[pos - 1]; pos--; }
				cs[pos] = score; cand[pos] = t; cd2[pos] = d2; cf[pos] = f;
				if (cn < 4) cn++;
			}
		}
		for (k = 0; k < cn; k++) {
			if (!los_clear(ai, eye, ai->tgt[cand[k]].chest)) continue;
			best = cand[k]; bestd2 = cd2[k]; bestf = cf[k];
			break;
		}
	}
	if (best >= 0) {
		a->notice += dt * bestf * (1.0f - 0.4f * sqrtf(bestd2) / vd) / notice_time(a, p);
		if (a->aware == 2 && a->tgt == best) a->notice = 1.0f;
		if (a->notice >= 1.0f) acquire(ai, a, best);
		if (a->aware == 2 && a->tgt == best) {
			a->tgt_vis = 1;
			cp3(a->tgt_pos, ai->tgt[best].chest);
			a->tgt_seen_tick = ai->st.tick;
		} else a->tgt_vis = 0;
	} else {
		a->tgt_vis = 0;
		a->notice -= dt * 0.5f;
		if (a->notice < 0.0f) a->notice = 0.0f;
	}
}

static void hear_noises(ai_t *ai, ai_actor *a, const unit_t *u)
{
	const ai_profile *p = &ai->st.prof[a->prof];
	unsigned i;
	float h = p->hearing * (a->st == AIS_SLEEP ? 0.5f : 1.0f);
	if (a->flags & (AIF_DEAF | AIF_BRAINDEAD | AIF_IGNORE_ALL) || a->aware >= 2) return;
	for (i = 0; i < ai->noise_read; i++) {
		const ai_noise *n = &ai->st.noise[i];
		if (n->src == a->unit || !ai_teams_hostile(ai, a->team, n->team)) continue;
		if (dist3(u->body.pos, n->pos) > h) continue;
		alert_actor(ai, a, n->pos);
		a->notice = a->notice < 0.7f ? 0.7f : a->notice;
	}
}

/* health lost since the last tick: the shooter is somewhere near; the actor goes for it */
static void check_damage(ai_t *ai, ai_actor *a, const unit_t *u)
{
	const game_unit_t *gu = &ai->env.game->unit_state[a->obj];
	float hp = gu->vitals.health + gu->vitals.shield;
	if (a->last_hp > 0.0f && hp < a->last_hp - 0.01f) {
		a->hurt_tick = ai->st.tick;
		if (a->aware < 2 && !(a->flags & (AIF_BRAINDEAD | AIF_IGNORE_ALL))) {
			int t, best = -1;
			float bd = 60.0f * 60.0f;
			for (t = 0; t <= (int)UNIT_MAX; t++) {
				float d2;
				if (!hostile_target(ai, a, t)) continue;
				d2 = dist2d2(ai->tgt[t].pos, u->body.pos);
				if (d2 < bd) { bd = d2; best = t; }
			}
			if (best >= 0) {
				acquire(ai, a, best);
				a->tgt_vis = 0;
				cp3(a->tgt_pos, ai->tgt[best].chest);
				a->tgt_seen_tick = ai->st.tick;
			} else alert_actor(ai, a, u->body.pos);
			if (a->st == AIS_SLEEP) set_state(ai, a, AIS_ALERT);
		}
	}
	a->last_hp = hp;
}

/* ---- navigation ----------------------------------------------------------------------------------------------------------------------- */
typedef struct { float dir[2]; float speed; int jump; int arrived; } nav_out;

static int sweep_clear(ai_t *ai, const float from[3], const float to[3])
{
	coll_hit h;
	float a[3], b[3];
	if (!ai->env.game->coll) return 1;
	cp3(a, from); cp3(b, to);
	a[2] += 0.35f; b[2] += 0.35f;
	return !coll_sphere_sweep(ai->env.game->coll, a, b, 0.2f, &h);
}

static void nav_route(ai_t *ai, ai_actor *a, const unit_t *u, const float goal[3])
{
	path_waypoint wp[AI_WAYPOINTS + 2];
	path_query q;
	path_route_info info;
	int n = 0, i;
	a->route_tick = ai->st.tick;
	cp3(a->route_goal, goal);
	a->goal_valid = 1; a->wp_i = 0; a->wp_n = 0;
	if (!ai->env.path || !ai->env.scratch || (dist3(u->body.pos, goal) < 5.0f && sweep_clear(ai, u->body.pos, goal))) {
		cp3(a->wp[0], goal); a->wp_kind[0] = PATH_LINK_WALK; a->wp_n = 1;
		return;
	}
	if (ai->routes_left) ai->routes_left--;
	ai->st.st_routes++;
	path_query_default(&q);
	q.radius = u->pp.radius > 0.1f ? u->pp.radius : 0.2f;
	q.height = u->pp.crouch_height > 0.3f ? u->pp.crouch_height : 0.5f;
	q.kinds = PATH_KIND(PATH_LINK_WALK) | PATH_KIND(PATH_LINK_STEP) | PATH_KIND(PATH_LINK_VAULT) | PATH_KIND(PATH_LINK_DROP) | PATH_KIND(PATH_LINK_CLIMB);
	q.max_drop = 1.5f;
	q.max_expand = 400;
	n = path_route(ai->env.path, ai->env.scratch, &q, u->body.pos, goal, wp, (int)AI_WAYPOINTS, &info);
	ai->st.st_route_expanded += info.expanded;
	if (n <= 0 || info.status == PATH_ROUTE_NO_START || info.status == PATH_ROUTE_NO_GOAL) {
		ai->st.st_route_fail++;
		cp3(a->wp[0], goal); a->wp_kind[0] = PATH_LINK_WALK; a->wp_n = 1;
		return;
	}
	if (info.status == PATH_ROUTE_PARTIAL) ai->st.st_route_partial++;
	for (i = 0; i < n && i < (int)AI_WAYPOINTS; i++) {
		cp3(a->wp[i], wp[i].pos);
		a->wp_kind[i] = wp[i].kind;
	}
	a->wp_n = (uint8_t)(n < (int)AI_WAYPOINTS ? n : (int)AI_WAYPOINTS);
}

/* One step towards `goal`: fills the desired direction; returns 1 when within `radius` of it. */
static int nav_move(ai_t *ai, ai_actor *a, const unit_t *u, const float goal[3], float radius, nav_out *o)
{
	const float *pos = u->body.pos;
	float dx = goal[0] - pos[0], dy = goal[1] - pos[1], tx, ty, len;
	unsigned tick = ai->st.tick, guard;
	int need;
	o->dir[0] = o->dir[1] = 0.0f; o->speed = 1.0f; o->jump = 0; o->arrived = 0;
	if (dx * dx + dy * dy <= radius * radius && fabsf(goal[2] - pos[2]) < 2.5f) {
		o->arrived = 1; a->goal_valid = 0; a->wp_n = a->wp_i = 0; a->stuck_ticks = 0;
		return 1;
	}
	need = !a->goal_valid || a->wp_i >= a->wp_n || dist2d2(goal, a->route_goal) > 4.0f;
	if (need && (!a->goal_valid || tick - a->route_tick >= 10u)) {
		if (ai->routes_left > 0 || !ai->env.path || !ai->env.scratch) nav_route(ai, a, u, goal);
		else ai->st.st_replans_deferred++;
	}
	tx = goal[0]; ty = goal[1];
	for (guard = 0; guard < 3; guard++) {
		float wx, wy, wd2, r;
		if (a->wp_i >= a->wp_n) break;
		wx = a->wp[a->wp_i][0] - pos[0]; wy = a->wp[a->wp_i][1] - pos[1];
		wd2 = wx * wx + wy * wy;
		r = a->wp_i + 1u == a->wp_n ? (radius < WP_R ? radius : WP_R) : WP_R;
		if (wd2 > r * r) { tx = a->wp[a->wp_i][0]; ty = a->wp[a->wp_i][1]; break; }
		if (a->wp_kind[a->wp_i] == PATH_LINK_CLIMB && a->wp_i + 1u < a->wp_n) {          /* ladders: put the actor at the top */
			units_teleport(ai->env.units, ai->env.game, a->unit, a->wp[a->wp_i + 1u], u->body.yaw);
			ai->st.st_climbs++;
			a->wp_i = (uint8_t)(a->wp_i + 2u);
			break;
		}
		a->wp_i++;
	}
	if (a->wp_i < a->wp_n) {
		unsigned k = a->wp_i;
		if (k > 0 && (a->wp_kind[k - 1] == PATH_LINK_VAULT || a->wp_kind[k - 1] == PATH_LINK_JUMP) && u->body.on_ground) o->jump = 1;
	}
	dx = tx - pos[0]; dy = ty - pos[1];
	len = sqrtf(dx * dx + dy * dy);
	if (len > 1e-4f) { o->dir[0] = dx / len; o->dir[1] = dy / len; }
	/* keep apart from other actors */
	{
		unsigned i;
		float px = 0.0f, py = 0.0f;
		for (i = 0; i < AI_MAX_ACTORS; i++) {
			const ai_actor *b = &ai->st.actor[i];
			float ex, ey, d2;
			if (!b->active || b == a || b->bsp != a->bsp) continue;
			ex = pos[0] - ai->env.units->u[b->unit].body.pos[0]; ey = pos[1] - ai->env.units->u[b->unit].body.pos[1];
			d2 = ex * ex + ey * ey;
			if (d2 < 0.64f && d2 > 1e-6f) { float d = sqrtf(d2), k = (0.8f - d) / 0.8f; px += ex / d * k; py += ey / d * k; }
		}
		o->dir[0] += px * 0.8f; o->dir[1] += py * 0.8f;
		len = sqrtf(o->dir[0] * o->dir[0] + o->dir[1] * o->dir[1]);
		if (len > 1e-4f) { o->dir[0] /= len; o->dir[1] /= len; }
	}
	/* progress check: not moving for a second re-plans, for three seconds hops to the next waypoint (an assist, counted) */
	if (tick - a->prog_tick > 40u) { cp3(a->prog_pos, pos); a->prog_tick = tick; a->stuck_ticks = 0; }
	else if (tick - a->prog_tick >= 15u) {
		a->stuck_ticks = (uint16_t)(dist2d2(pos, a->prog_pos) < 0.04f ? a->stuck_ticks + 15u : 0u);
		cp3(a->prog_pos, pos); a->prog_tick = tick;
		if (a->stuck_ticks == 30u) { ai->st.st_stuck++; a->route_tick = 0; a->goal_valid = 0; }
		if (a->stuck_ticks >= 90u) {
			const float *hop = a->wp_i < a->wp_n ? a->wp[a->wp_i] : goal;
			units_teleport(ai->env.units, ai->env.game, a->unit, hop, u->body.yaw);
			ai->st.st_assists++;
			a->stuck_ticks = 0;
			if (a->wp_i < a->wp_n) a->wp_i++;
		}
	}
	return 0;
}

static void apply_move(unit_control_t *c, const unit_t *u, const nav_out *o, float face_yaw, int have_face, int walk)
{
	float yaw = u->body.yaw, cy = cosf(yaw), sy = sinf(yaw);
	if (o->dir[0] * o->dir[0] + o->dir[1] * o->dir[1] < 1e-8f) { if (have_face) c->aim_yaw = face_yaw; return; }
	if (have_face) {
		c->aim_yaw = face_yaw;
		c->throttle_fwd = (o->dir[0] * cy + o->dir[1] * sy) * o->speed;
		c->throttle_left = (-o->dir[0] * sy + o->dir[1] * cy) * o->speed;
	} else {
		float head = atan2f(o->dir[1], o->dir[0]), diff = fabsf(wrap_pi(head - yaw));
		c->aim_yaw = head;
		c->throttle_fwd = diff > 1.3f ? 0.0f : o->speed * clampf(cosf(diff), 0.15f, 1.0f);
		c->throttle_left = 0.0f;
	}
	if (o->jump) c->flags |= UC_JUMP;
	if (walk) c->flags |= UC_WALK;
}

/* ---- species -------------------------------------------------------------------------------------------------------------------------- */
typedef struct { float retreat; uint8_t flee_unarmed, flee_alone, always_charge, melee_only; } species_t;
static species_t species_of(unsigned type)
{
	species_t s = { 0.20f, 0, 0, 0, 0 };
	switch (type) {
	case AIDEF_ACTOR_ELITE: s.retreat = 0.18f; break;
	case AIDEF_ACTOR_GRUNT: s.retreat = 0.45f; s.flee_alone = 1; break;
	case AIDEF_ACTOR_JACKAL: s.retreat = 0.35f; break;
	case AIDEF_ACTOR_HUNTER: s.retreat = 0.0f; s.always_charge = 1; break;
	case AIDEF_ACTOR_MARINE: s.retreat = 0.25f; break;
	case AIDEF_ACTOR_CREW: s.retreat = 0.30f; s.flee_unarmed = 1; break;
	case AIDEF_ACTOR_ENGINEER: s.flee_unarmed = 1; s.retreat = 0.5f; break;
	case AIDEF_ACTOR_COMBAT_FORM: case AIDEF_ACTOR_INFECTION_FORM: case AIDEF_ACTOR_CARRIER_FORM:
		s.retreat = 0.0f; s.always_charge = 1; s.melee_only = 1; break;
	case AIDEF_ACTOR_SENTINEL: case AIDEF_ACTOR_MONITOR: s.retreat = 0.0f; break;
	default: break;
	}
	return s;
}

static float hp_fraction(const ai_t *ai, const ai_actor *a)
{
	const vitals_t *v = &ai->env.game->unit_state[a->obj].vitals;
	float mx = v->max_health + v->max_shield;
	return mx > 0.0f ? clampf((v->health + v->shield) / mx, 0.0f, 1.0f) : 1.0f;
}

/* ---- combat helpers ------------------------------------------------------------------------------------------------------------------- */
static int ammo_ok(const ai_profile *p, const unit_t *u)
{
	if (!p->armed || u->weapon < 0) return 0;
	if (p->mag == 0xFFu) return 1;
	return u->wst.loaded[p->mag] > 0 || u->wst.reserve[p->mag] > 0 || u->wst.mode == WEAP_MODE_RELOADING;
}

static int burst_step(ai_t *ai, ai_actor *a, const ai_profile *p, int active)
{
	const ai_diff *d = &ai->st.diff;
	if (!active) return 0;
	if (a->btimer > 0) a->btimer--;
	switch (a->bstate) {
	case 1:
		a->boff[0] *= 0.96f; a->boff[1] *= 0.96f;
		if (a->btimer == 0) {
			a->bstate = 2;
			a->btimer = (uint16_t)(5u + secs(rrange(ai, p->burst_sep[0], p->burst_sep[1]) * d->burst_sep));
			return 0;
		}
		return 1;
	default:
		if (a->btimer == 0) {
			float th = rf(ai) * 2.0f * PI_F, r0 = p->burst_radius * d->burstiness;
			a->bstate = 1;
			a->blen = a->btimer = (uint16_t)(6u + secs(rrange(ai, p->burst_dur[0], p->burst_dur[1])));
			a->boff[0] = cosf(th) * r0; a->boff[1] = sinf(th) * r0 * 0.6f;
			return 1;
		}
		return 0;
	}
}

/* firing positions of the encounter: look at a window of them for one with line of sight to the target, in range and not taken */
static void pick_fp(ai_t *ai, ai_actor *a, const unit_t *u, const ai_profile *p, const float target[3])
{
	const ai_enc_rt *e = &ai->st.enc[a->enc];
	aidef_encounter_def ed;
	unsigned n = e->firing_count, window, k, i, cand_n = 0;
	int cand[3];
	float cand_s[3];
	float mid = 0.5f * (p->rmin + p->rmax), maxd = p->fire_dist * 0.85f;
	a->fp_tick = ai->st.tick;
	if (n == 0) return;
	memset(&ed, 0, sizeof ed);
	ed.first_firing = e->first_firing; ed.firing_count = n;
	window = n < 48u ? n : 48u;
	for (i = 0; i < window; i++) {
		aidef_firing_def f;
		float dt2, da2, dtd, score;
		unsigned j;
		k = (a->fp_scan + i) % n;
		if (!aidef_get_firing(ai->env.aidef, &ed, k, &f)) continue;
		dt2 = dist2d2(f.pos, target); da2 = dist2d2(f.pos, u->body.pos);
		if (fabsf(f.pos[2] - target[2]) > 4.0f || da2 > 35.0f * 35.0f || dt2 > maxd * maxd || dt2 < 1.0f) continue;
		dtd = sqrtf(dt2);
		score = fabsf(dtd - mid) + 0.35f * sqrtf(da2);
		for (j = 0; j < AI_MAX_ACTORS; j++)
			if (ai->st.actor[j].active && &ai->st.actor[j] != a && ai->st.actor[j].enc == a->enc && ai->st.actor[j].fp == (int16_t)k) score += 8.0f;
		if (a->fp == (int16_t)k) score -= 1.5f;
		/* keep the three best */
		if (cand_n < 3 || score < cand_s[cand_n - 1]) {
			unsigned pos = cand_n < 3 ? cand_n : 2u;
			while (pos > 0 && cand_s[pos - 1] > score) { cand_s[pos] = cand_s[pos - 1]; cand[pos] = cand[pos - 1]; pos--; }
			cand_s[pos] = score; cand[pos] = (int)k;
			if (cand_n < 3) cand_n++;
		}
	}
	a->fp_scan = (uint16_t)((a->fp_scan + window) % n);
	for (i = 0; i < cand_n; i++) {
		aidef_firing_def f;
		float from[3];
		if (!aidef_get_firing(ai->env.aidef, &ed, (unsigned)cand[i], &f)) continue;
		cp3(from, f.pos); from[2] += 0.55f;
		ai->st.st_rays_fp++;
		if (los_clear(ai, from, target)) {
			if (a->fp != (int16_t)cand[i]) { a->fp = (int16_t)cand[i]; ai->st.st_fp_picks++; }
			return;
		}
	}
}

/* ---- leaders and followers ------------------------------------------------------------------------------------------------------------ */
static int leader_pos(const ai_t *ai, const ai_actor *a, float pos[3], float *yaw)
{
	unsigned i;
	if (a->orders == AIO_FOLLOW_SQUAD && a->squad < AI_MAX_SQUADS && ai->st.squad[a->squad].follow >= 0) {
		int fs = ai->st.squad[a->squad].follow;
		for (i = 0; i < AI_MAX_ACTORS; i++) {
			const ai_actor *b = &ai->st.actor[i];
			if (b->active && b != a && b->squad == (uint16_t)fs) { const unit_t *ub = unit_at(ai, b->unit); cp3(pos, ub->body.pos); *yaw = ub->body.yaw; return 1; }
		}
	}
	if (ai->tgt[UNIT_MAX].valid) { cp3(pos, ai->tgt[UNIT_MAX].pos); *yaw = ai->env.game->player.yaw; return 1; }
	return 0;
}

static unsigned follow_rank(const ai_t *ai, const ai_actor *a, unsigned *total)
{
	unsigned i, rank = 0, n = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		const ai_actor *b = &ai->st.actor[i];
		if (!b->active || b->orders != a->orders || b->squad >= AI_MAX_SQUADS) continue;
		if (a->orders == AIO_FOLLOW_SQUAD && ai->st.squad[b->squad].follow != ai->st.squad[a->squad].follow) continue;
		if (b == a) rank = n;
		n++;
	}
	*total = n ? n : 1u;
	return rank;
}

/* ---- behaviours ----------------------------------------------------------------------------------------------------------------------- */
static void face_point(const unit_t *u, const float p[3], unit_control_t *c)
{
	float eye[3], dx, dy;
	eye_of(u, eye);
	dx = p[0] - eye[0]; dy = p[1] - eye[1];
	c->aim_yaw = atan2f(dy, dx);
	c->aim_pitch = atan2f(p[2] - eye[2], sqrtf(dx * dx + dy * dy));
}

static int fp_pos(const ai_t *ai, const ai_actor *a, int idx, float out[3])
{
	aidef_encounter_def ed;
	aidef_firing_def f;
	const ai_enc_rt *e = &ai->st.enc[a->enc];
	if (idx < 0 || (unsigned)idx >= e->firing_count) return 0;
	memset(&ed, 0, sizeof ed);
	ed.first_firing = e->first_firing; ed.firing_count = e->firing_count;
	if (!aidef_get_firing(ai->env.aidef, &ed, (unsigned)idx, &f)) return 0;
	cp3(out, f.pos);
	return 1;
}

static void do_idle(ai_t *ai, ai_actor *a, const unit_t *u, unit_control_t *c)
{
	nav_out o;
	c->aim_yaw = a->home_yaw;
	if (dist2d2(u->body.pos, a->home) > 1.4f * 1.4f || (a->goal_valid && a->wp_n)) {
		if (!nav_move(ai, a, u, a->home, 0.6f, &o)) { o.speed = 0.8f; apply_move(c, u, &o, 0.0f, 0, 1); }
	}
}

static void patrol_pick(ai_t *ai, ai_actor *a)
{
	aidef_squad_def sq;
	const aidef_section *s = ai->env.aidef;
	unsigned n, idx;
	float dwell = rrange(ai, 2.0f, 5.0f);
	cp3(a->goal, a->home);
	if (a->squad >= AI_MAX_SQUADS || !aidef_get_squad(s, a->squad, &sq)) return;
	n = sq.move_count ? sq.move_count : sq.start_count;
	if (n == 0) return;
	switch (a->init_state) {
	case AIDEF_STATE_MOVING_LOOP: idx = (a->patrol_i + 1u) % n; break;
	case AIDEF_STATE_MOVING_LOOP_BACK_AND_FORTH:
		if (a->patrol_dir == 0) { idx = a->patrol_i + 1u; if (idx >= n) { idx = n >= 2 ? n - 2u : 0u; a->patrol_dir = 1; } }
		else { if (a->patrol_i == 0) { idx = n >= 2 ? 1u : 0u; a->patrol_dir = 0; } else idx = a->patrol_i - 1u; }
		break;
	default: idx = rnd(ai) % n; break;
	}
	if (sq.move_count) {
		aidef_move_def m;
		if (aidef_get_move(s, &sq, idx, &m)) { cp3(a->goal, m.pos); if (m.time_max > 0.0f) dwell = rrange(ai, m.time_min, m.time_max); }
	} else {
		aidef_start_def st;
		if (aidef_get_start(s, &sq, idx, &st)) cp3(a->goal, st.pos);
	}
	a->patrol_i = (uint16_t)idx;
	a->cl_timer = secs(dwell);                               /* dwell time at this point (cl_timer is free outside command lists) */
}

static void do_patrol(ai_t *ai, ai_actor *a, const unit_t *u, unit_control_t *c)
{
	nav_out o;
	unsigned tick = ai->st.tick;
	if (a->dwell_until && tick < a->dwell_until) { c->aim_yaw = u->body.yaw; return; }
	if (a->dwell_until) { a->dwell_until = 0; a->wander_tick = 0; }
	if (a->wander_tick == 0) { patrol_pick(ai, a); a->wander_tick = 1; }
	if (nav_move(ai, a, u, a->goal, 0.6f, &o)) a->dwell_until = tick + a->cl_timer + 1u;
	else { o.speed = 0.8f; apply_move(c, u, &o, 0.0f, 0, 1); }
}

static int nearest_hostile(ai_t *ai, const ai_actor *a, const unit_t *u, float maxd, float out[3])
{
	int t, best = -1;
	float bd = maxd * maxd;
	for (t = 0; t <= (int)UNIT_MAX; t++) {
		float d2;
		if (!hostile_target(ai, a, t)) continue;
		d2 = dist2d2(ai->tgt[t].pos, u->body.pos);
		if (d2 < bd) { bd = d2; best = t; }
	}
	if (best >= 0) cp3(out, ai->tgt[best].pos);
	return best;
}

static void do_search(ai_t *ai, ai_actor *a, const unit_t *u, unit_control_t *c)
{
	nav_out o;
	float goal[3];
	int hunting = a->aware < 1 && a->search_until == 0;
	if (hunting) {
		if (a->wander_tick == 0 || ai->st.tick - a->wander_tick >= 45u) {            /* re-sniff the hunted position every 1.5 s */
			if (nearest_hostile(ai, a, u, HUNT_DIST, a->goal) < 0) { do_idle(ai, a, u, c); return; }
			a->wander_tick = ai->st.tick;
		}
		cp3(goal, a->goal);
	} else cp3(goal, a->tgt_pos);
	if (nav_move(ai, a, u, goal, hunting ? 2.5f : 1.2f, &o)) {
		a->yaw_goal += 0.06f;                                                         /* look around */
		c->aim_yaw = a->yaw_goal;
	} else {
		apply_move(c, u, &o, 0.0f, 0, 0);
		a->yaw_goal = c->aim_yaw;
	}
}

static void flee_goal(ai_t *ai, ai_actor *a, const unit_t *u, const float thr[3])
{
	unsigned k;
	float best = -1.0f, bp[3];
	cp3(bp, u->body.pos);
	for (k = 0; k < 8; k++) {
		float ang = (float)k * (PI_F * 0.25f) + a->yaw_goal, cand[3], pt[3], sc, d;
		cand[0] = u->body.pos[0] + 12.0f * cosf(ang); cand[1] = u->body.pos[1] + 12.0f * sinf(ang); cand[2] = u->body.pos[2];
		cp3(pt, cand);
		if (ai->env.path) {
			if (path_nearest_sector(ai->env.path, cand, 5.0f, &d, pt) < 0) continue;
		}
		sc = sqrtf(dist2d2(pt, thr)) - 0.4f * sqrtf(dist2d2(pt, u->body.pos));
		if (sc > best) { best = sc; cp3(bp, pt); }
	}
	cp3(a->goal, bp);
	a->yaw_goal += 0.7f;
	a->wander_tick = ai->st.tick;
}

static void do_flee(ai_t *ai, ai_actor *a, const unit_t *u, unit_control_t *c)
{
	nav_out o;
	float thr[3];
	int has = 0;
	if (a->aware == 2 && a->tgt >= 0 && ai->tgt[a->tgt].valid) { cp3(thr, ai->tgt[a->tgt].pos); has = 1; }
	else if (nearest_hostile(ai, a, u, 25.0f, thr) >= 0) has = 1;
	if (!has) { c->aim_yaw = u->body.yaw; return; }
	if (a->wander_tick == 0 || ai->st.tick - a->wander_tick >= 45u || dist2d2(a->goal, u->body.pos) < 1.0f) flee_goal(ai, a, u, thr);
	if (!nav_move(ai, a, u, a->goal, 1.0f, &o)) apply_move(c, u, &o, 0.0f, 0, 0);
}

static void do_follow(ai_t *ai, ai_actor *a, const unit_t *u, unit_control_t *c)
{
	float lp[3], lyaw, goal[3], ang, r, d;
	unsigned rank, total;
	nav_out o;
	if (!leader_pos(ai, a, lp, &lyaw)) { do_idle(ai, a, u, c); return; }
	rank = follow_rank(ai, a, &total);
	ang = lyaw + PI_F + ((float)rank - 0.5f * (float)(total - 1u)) * 0.8f;
	r = 2.2f + 0.35f * (float)rank;
	goal[0] = lp[0] + r * cosf(ang); goal[1] = lp[1] + r * sinf(ang); goal[2] = lp[2];
	d = sqrtf(dist2d2(u->body.pos, lp));
	if (d > 3.5f && !nav_move(ai, a, u, goal, 0.8f, &o)) {
		o.speed = 1.0f;
		apply_move(c, u, &o, 0.0f, 0, d <= 6.0f);
	} else c->aim_yaw = lyaw;
}

static void do_combat(ai_t *ai, ai_actor *a, const unit_t *u, unit_control_t *c, int mode)   /* mode 0 fight, 1 charge, 2 fighting withdrawal to home */
{
	const ai_profile *p = &ai->st.prof[a->prof];
	const ai_diff *df = &ai->st.diff;
	species_t sp = species_of(p->type);
	unsigned tick = ai->st.tick;
	float eye[3], to[3], dist, dh, aim[3], k, rmin = p->rmin, rmax = p->rmax, goal[3], gr = 0.8f, yaw_d, pitch_d, dh2;
	int vis = a->tgt_vis, armed = ammo_ok(p, u), moving = 0, have_goal = 0, backoff = 0, tvalid = a->tgt >= 0 && ai->tgt[a->tgt].valid;
	const float *tp = tvalid ? ai->tgt[a->tgt].chest : a->tgt_pos;
	nav_out o;
	eye_of(u, eye);
	to[0] = tp[0] - eye[0]; to[1] = tp[1] - eye[1]; to[2] = tp[2] - eye[2];
	dh = sqrtf(to[0] * to[0] + to[1] * to[1]);
	dist = sqrtf(dh * dh + to[2] * to[2]);
	/* aim point: lead for moving targets, burst offset */
	cp3(aim, tp);
	if (tvalid && p->proj_speed > 0.05f) {
		float lk = clampf(p->lead + df->lead_add, 0.0f, 1.0f), tf = dist / p->proj_speed;
		if (tf > 45.0f) tf = 45.0f;
		aim[0] += ai->tgt[a->tgt].vel[0] * tf * lk; aim[1] += ai->tgt[a->tgt].vel[1] * tf * lk;
	}
	{
		float y0 = atan2f(to[1], to[0]);
		aim[0] += -sinf(y0) * a->boff[0]; aim[1] += cosf(y0) * a->boff[0]; aim[2] += a->boff[1];
	}
	yaw_d = atan2f(aim[1] - eye[1], aim[0] - eye[0]);
	dh2 = sqrtf((aim[0] - eye[0]) * (aim[0] - eye[0]) + (aim[1] - eye[1]) * (aim[1] - eye[1]));
	pitch_d = atan2f(aim[2] - eye[2], dh2);
	k = clampf(0.30f + 0.6f * (p->track + df->track_add), 0.15f, 1.0f);
	a->aim[0] = wrap_pi(a->aim[0] + wrap_pi(yaw_d - a->aim[0]) * k);
	a->aim[1] += (pitch_d - a->aim[1]) * k;
	c->aim_yaw = a->aim[0]; c->aim_pitch = a->aim[1];

	if (mode == 1 || sp.melee_only || !armed) { rmin = 0.0f; rmax = 0.9f; }
	/* where to stand */
	if (mode == 0 && armed && a->tgt >= 0 && ai->st.enc[a->enc].firing_count && tick - a->fp_tick >= 40u + (unsigned)(a - ai->st.actor) % 8u) pick_fp(ai, a, u, p, tp);
	if (mode == 2) { cp3(goal, a->home); gr = 0.8f; have_goal = 1; }
	else if (mode == 0 && armed && a->fp >= 0 && fp_pos(ai, a, a->fp, goal)) { gr = 0.7f; have_goal = 1; }
	else if (!vis || dist > rmax) { cp3(goal, tp); goal[2] = tvalid ? ai->tgt[a->tgt].pos[2] : tp[2]; gr = rmax * 0.7f > 0.8f ? rmax * 0.7f : 0.8f; have_goal = 1; }
	else if (dist < rmin * 0.75f && armed) backoff = 1;
	if (have_goal) {
		if (!nav_move(ai, a, u, goal, gr, &o)) { apply_move(c, u, &o, c->aim_yaw, 1, 0); moving = 1; }
		else if (mode == 2 && a->orders == AIO_RETREAT) a->orders = AIO_DEFEND;          /* arrived: hold here */
	} else if (backoff && dh > 1e-3f) {
		o.dir[0] = -to[0] / dh; o.dir[1] = -to[1] / dh; o.speed = 0.6f; o.jump = 0; o.arrived = 0;
		apply_move(c, u, &o, c->aim_yaw, 1, 0);
		moving = 1;
	}
	if (!moving) {
		/* holding: strafe a little, crouch per the variant's movement type */
		if (vis && armed && a->fp < 0 && mode == 0 && dh > 1e-3f) {
			if (tick >= a->strafe_tick) {
				float side[3] = { -to[1] / dh, to[0] / dh, 0.0f }, probe[3];
				a->strafe_dir = rf(ai) < 0.4f ? 0.0f : (rf(ai) < 0.5f ? -1.0f : 1.0f);
				a->strafe_tick = tick + (a->strafe_dir == 0.0f ? 30u + rnd(ai) % 30u : 20u + rnd(ai) % 25u);
				if (a->strafe_dir != 0.0f) {
					probe[0] = u->body.pos[0] + side[0] * a->strafe_dir * 0.8f; probe[1] = u->body.pos[1] + side[1] * a->strafe_dir * 0.8f; probe[2] = u->body.pos[2];
					if (!sweep_clear(ai, u->body.pos, probe)) a->strafe_dir = -a->strafe_dir;
				}
			}
			if (a->strafe_dir != 0.0f) {
				o.dir[0] = -to[1] / dh * a->strafe_dir; o.dir[1] = to[0] / dh * a->strafe_dir; o.speed = 0.5f; o.jump = 0; o.arrived = 0;
				apply_move(c, u, &o, c->aim_yaw, 1, 0);
			}
		}
		if (p->movement_type != 0 || p->crouch_chance > 0.0f) {
			if (p->movement_type == 2 || p->crouch_chance > 0.0f) {
				if (a->crouch_timer) a->crouch_timer--;
				else {
					a->crouching = (uint8_t)!a->crouching;
					a->crouch_timer = (uint16_t)(30u + secs(a->crouching ? rrange(ai, p->crouch_t[0], p->crouch_t[1]) : rrange(ai, p->run_t[0], p->run_t[1])));
				}
			}
			if (a->crouching || p->movement_type == 1) c->flags |= UC_CROUCH;
		}
	}
	/* fire control */
	{
		int fire_ok = armed && vis && !sp.melee_only && dist <= p->fire_dist && u->wst.mode != WEAP_MODE_RELOADING;
		int facing = fabsf(wrap_pi(c->aim_yaw - u->body.yaw)) < 0.4f;
		if (burst_step(ai, a, p, fire_ok && facing)) {
			float duty = p->duty;                      /* the unit layer applies the difficulty's rate-of-fire scale itself */
			a->rof_acc += duty;
			if (a->rof_acc >= 1.0f) {
				a->rof_acc -= 1.0f;
				c->flags |= UC_FIRE;
				ai->st.st_shots_ticks++;
				if (a->btimer % 8u == 0) push_noise(ai, u->body.pos, a->team, a->unit);
			}
		}
		if (armed && p->mag != 0xFFu && u->wst.loaded[p->mag] == 0 && u->wst.reserve[p->mag] > 0 && u->wst.mode != WEAP_MODE_RELOADING) c->flags |= UC_RELOAD;
		/* grenades */
		if (vis && ai->st.grenades && (u->grenades[0] + u->grenades[1]) > 0 && tick >= a->gren_tick && p->gren_range[1] > 0.0f) {
			a->gren_tick = tick + 1u + secs(p->gren_check);
			if (dist >= p->gren_range[0] && dist <= p->gren_range[1] && tick >= ai->st.enc[a->enc].grenade_tick &&
			    rf(ai) < p->gren_chance * df->gren_chance) {
				c->flags |= UC_GRENADE;
				ai->st.enc[a->enc].grenade_tick = tick + secs(p->gren_timeout * df->gren_timer);
				ai->st.st_grenades++;
			}
		}
		/* melee */
		if (dh < 1.15f && fabsf(to[2]) < 1.3f && tick >= a->melee_tick && (!armed || sp.melee_only || mode == 1 || dh < 0.7f)) {
			c->flags |= UC_MELEE;
			a->melee_tick = tick + 1u + secs(1.2f * df->melee_delay);
			ai->st.st_melee++;
		}
	}
}

/* ---- command lists -------------------------------------------------------------------------------------------------------------------- */
static void cl_finish(ai_t *ai, ai_actor *a, const unit_t *u)
{
	a->cl_status = 2; a->cl_list = -1; a->cl_phase = 0;
	cp3(a->home, u->body.pos); a->home_yaw = u->body.yaw;                /* the actor stays where the list left it */
	a->goal_valid = 0; a->wp_n = 0;
	if (a->st != AIS_SLEEP) set_state(ai, a, AIS_IDLE);
}

/* returns 0 while the atom runs (control written), 1 when it completed, 2 when it completed and set the program counter */
static int atom_run(ai_t *ai, ai_actor *a, unit_t *u, unit_control_t *c, const aidef_command_list_def *l, const aidef_command_def *k)
{
	unsigned tick = ai->st.tick;
	const aidef_section *s = ai->env.aidef;
	aidef_point_def pt, pt2;
	int has1, has2;
	memset(&pt, 0, sizeof pt); memset(&pt2, 0, sizeof pt2);
	has1 = k->point1 >= 0 && aidef_get_point(s, l, (unsigned)k->point1, &pt);
	has2 = k->point2 >= 0 && aidef_get_point(s, l, (unsigned)k->point2, &pt2);
	unsigned ty = k->type < AIDEF_ATOMS ? k->type : AIDEF_ATOMS;
	if (a->cl_phase == 0) ai->st.atom_seen[ty]++;
	switch (k->type) {
	case AIDEF_ATOM_PAUSE: case AIDEF_ATOM_WAIT:
		if (a->cl_phase == 0) { a->cl_timer = tick + (secs(k->param1) > 1u ? secs(k->param1) : 1u); a->cl_phase = 1; }
		return tick >= a->cl_timer;
	case AIDEF_ATOM_GO_TO: case AIDEF_ATOM_GO_TO_AND_FACE: {
		nav_out o;
		if (!has1) { ai->st.atom_skipped[ty]++; return 1; }
		if (a->cl_phase == 0) { a->cl_timer = tick + 40u * 30u; a->cl_phase = 1; }
		if (a->cl_phase == 1) {
			if (!nav_move(ai, a, u, pt.pos, a->cl_radius, &o)) {
				apply_move(c, u, &o, 0.0f, 0, 0);
				a->cl_aim = 1;                                 /* this atom owns the aim while it walks (or turns to walk) */
				if (tick > a->cl_timer) { ai->st.atom_skipped[ty]++; return 1; }
				return 0;
			}
			if (k->type == AIDEF_ATOM_GO_TO || !has2) return 1;
			a->cl_phase = 2; a->cl_timer = tick + 45u;
		}
		{
			float yaw = atan2f(pt2.pos[1] - u->body.pos[1], pt2.pos[0] - u->body.pos[0]);
			c->aim_yaw = yaw;
			a->cl_look_kind = 3; a->cl_look[0] = yaw;
			return fabsf(wrap_pi(yaw - u->body.yaw)) < 0.2f || tick >= a->cl_timer;
		}
	}
	case AIDEF_ATOM_LOOK: case AIDEF_ATOM_LOOK_PLAYER: case AIDEF_ATOM_LOOK_OBJECT: case AIDEF_ATOM_LOOK_RANDOM:
		if (a->cl_phase == 0) {
			float lo = k->param1, hi = k->param2;
			a->cl_phase = 1;
			if (k->type == AIDEF_ATOM_LOOK) {
				if (has1) { cp3(a->cl_look, pt.pos); a->cl_look_kind = 1; }
				lo = hi = k->param1;
			} else if (k->type == AIDEF_ATOM_LOOK_PLAYER) { a->cl_look_kind = 2; lo = hi = k->param1; }
			else if (k->type == AIDEF_ATOM_LOOK_OBJECT) {
				int obj = ai->object && k->object_name >= 0 ? ai->object(ai->object_user, (unsigned)k->object_name) : -1;
				if (obj_live(ai, obj)) { cp3(a->cl_look, ai->env.game->objects->obj[obj].pos); a->cl_look[2] += 0.5f; a->cl_look_kind = 1; }
				lo = hi = k->param1;
			} else {
				int p1 = k->point1, p2 = k->point2, lo_i = p1 < p2 ? p1 : p2, hi_i = p1 < p2 ? p2 : p1;
				aidef_point_def rp;
				if (lo > hi) { float t = lo; lo = hi; hi = t; }
				if (lo_i >= 0 && aidef_get_point(s, l, (unsigned)(lo_i + (int)(rnd(ai) % (unsigned)(hi_i - lo_i + 1))), &rp)) { cp3(a->cl_look, rp.pos); a->cl_look_kind = 1; }
				else { a->cl_look[0] = wrap_pi(a->home_yaw + rrange(ai, -0.9f, 0.9f)); a->cl_look_kind = 3; }
			}
			a->cl_timer = tick + (secs(rrange(ai, lo, hi)) > 3u ? secs(rrange(ai, lo, hi)) : 3u);
		}
		return tick >= a->cl_timer;
	case AIDEF_ATOM_SHOOT:
		if (a->cl_phase == 0) { a->cl_timer = tick + (secs(k->param1) > 9u ? secs(k->param1) : 30u); a->cl_phase = 1; }
		if (has1) face_point(u, pt.pos, c);
		c->flags |= UC_FIRE;
		return tick >= a->cl_timer;
	case AIDEF_ATOM_GRENADE:
		if (a->cl_phase == 0) { a->cl_timer = tick + 30u; a->cl_phase = 1; c->flags |= UC_GRENADE; if (has1) face_point(u, pt.pos, c); }
		return tick >= a->cl_timer;
	case AIDEF_ATOM_CROUCH:
		if (a->cl_phase == 0) { a->cl_timer = tick + (secs(k->param1) > 9u ? secs(k->param1) : 9u); a->cl_phase = 1; }
		c->flags |= UC_CROUCH;
		return tick >= a->cl_timer;
	case AIDEF_ATOM_ANIMATE: {
		if (a->cl_phase == 0) {
			aidef_anim_ref_def ref;
			unsigned dur = secs(k->param1);
			const anim_section *an = ai->env.game->ow.anims;
			int played = 0;
			a->cl_phase = 1;
			if (an && k->animation >= 0 && aidef_get_anim_ref(s, (unsigned)k->animation, &ref) && ref.graph_tag) {
				int gi = anim_find_graph(an, ref.graph_tag);
				if (gi >= 0) {
					anim_graph g = anim_get_graph(an, (uint32_t)gi);
					int idx = anim_find_name(an, &g, ref.name);
					if (idx >= 0) {
						unsigned t = units_custom_animation(ai->env.units, ai->env.game, a->unit, gi, idx, 1.0f, 0);
						if (t) { dur = t; played = 1; }
					}
				}
			}
			if (!played) ai->st.atom_skipped[ty]++;
			a->cl_timer = tick + (dur > 1u ? dur : 1u);
		}
		return tick >= a->cl_timer;
	}
	case AIDEF_ATOM_LOOP:
		if (k->command >= 0 && (unsigned)k->command < l->command_count) { a->cl_pc = (uint16_t)k->command; a->cl_loops++; return 2; }
		return 1;
	case AIDEF_ATOM_DIE:
		units_kill(ai->env.units, ai->env.game, a->unit, k->modifier == 1);
		return 1;
	case AIDEF_ATOM_MOVE_IMMEDIATE: case AIDEF_ATOM_TELEPORT:
		if (has1) { units_teleport(ai->env.units, ai->env.game, a->unit, pt.pos, u->body.yaw); a->goal_valid = 0; a->wp_n = 0; }
		else ai->st.atom_skipped[ty]++;
		return 1;
	case AIDEF_ATOM_SET_RADIUS:
		a->cl_radius = clampf(k->param1, 0.2f, 3.0f);
		return 1;
	case AIDEF_ATOM_SCRIPT:
		if (ai->script && k->script >= 0) ai->script(ai->script_user, (unsigned)k->script);
		else ai->st.atom_skipped[ty]++;
		return 1;
	case AIDEF_ATOM_ANIMATION_MODE: case AIDEF_ATOM_ACTION: case AIDEF_ATOM_VOCALIZE: case AIDEF_ATOM_TARGETING: case AIDEF_ATOM_INITIATIVE:
	case AIDEF_ATOM_VEHICLE:
		ai->st.st_atoms_instant++;
		return 1;
	default:                                              /* move in direction, jumps, recording: counted, complete at once */
		ai->st.atom_skipped[ty]++;
		return 1;
	}
}

static void cl_step(ai_t *ai, ai_actor *a, unit_t *u, unit_control_t *c)
{
	aidef_command_list_def l;
	unsigned budget = MAX_ATOMS_PER_TICK;
	if (a->cl_list < 0 || !aidef_get_command_list(ai->env.aidef, (unsigned)a->cl_list, &l)) { cl_finish(ai, a, u); return; }
	a->cl_aim = 0;
	while (budget-- > 0) {
		aidef_command_def k;
		int r;
		if (a->cl_pc >= l.command_count) { cl_finish(ai, a, u); return; }
		if (!aidef_get_command(ai->env.aidef, &l, a->cl_pc, &k)) { cl_finish(ai, a, u); return; }
		if (a->cl_skip) { a->cl_skip = 0; a->cl_pc++; a->cl_phase = 0; continue; }
		r = atom_run(ai, a, u, c, &l, &k);
		if (r == 0) break;
		ai->st.st_atoms_run++;
		a->cl_phase = 0;
		if (r == 1) a->cl_pc++;
	}
	/* where the actor looks while no atom moves or aims it */
	if (!a->cl_aim && c->throttle_fwd == 0.0f && c->throttle_left == 0.0f && !(c->flags & UC_FIRE)) {
		if (a->cl_look_kind == 1) face_point(u, a->cl_look, c);
		else if (a->cl_look_kind == 2 && ai->tgt[UNIT_MAX].valid) face_point(u, ai->tgt[UNIT_MAX].chest, c);
		else if (a->cl_look_kind == 3) c->aim_yaw = a->cl_look[0];
	}
}

/* ---- the decision of one actor -------------------------------------------------------------------------------------------------------- */
static unsigned entry_state(ai_t *ai, ai_actor *a, const unit_t *u)
{
	unsigned want = a->ret_state == AIDEF_STATE_NONE ? AIS_IDLE : state_for(a->ret_state);
	if (want == AIS_SLEEP) want = AIS_IDLE;
	if (a->ret_state != AIDEF_STATE_GUARDING_AT_GUARD_POSITION && a->ret_state != AIDEF_STATE_MOVING_LOOP &&
	    a->ret_state != AIDEF_STATE_MOVING_LOOP_BACK_AND_FORTH && a->ret_state != AIDEF_STATE_MOVING_LOOP_RANDOMLY &&
	    a->ret_state != AIDEF_STATE_MOVING_RANDOMLY) {                              /* stay where the fight ended */
		cp3(a->home, u->body.pos);
		a->home_yaw = u->body.yaw;
	}
	if (want == AIS_SEARCH) a->search_until = 0;
	(void)ai;
	return want;
}

static unsigned orders_state(ai_actor *a)
{
	switch (a->orders) {
	case AIO_FOLLOW_PLAYER: case AIO_FOLLOW_SQUAD: return AIS_FOLLOW;
	case AIO_ATTACK: case AIO_MANEUVER: a->search_until = 0; return AIS_SEARCH;
	case AIO_RETREAT: return AIS_RETREAT;
	default: return AIS_IDLE;
	}
}

static unsigned enc_alive_n(const ai_t *ai, unsigned enc)
{
	unsigned i, n = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) n += ai->st.actor[i].active && ai->st.actor[i].enc == enc;
	return n;
}

static void think(ai_t *ai, ai_actor *a, unit_t *u, unit_control_t *c)
{
	const ai_profile *p = &ai->st.prof[a->prof];
	species_t sp = species_of(p->type);
	unsigned tick = ai->st.tick;
	int tvalid;
	if (a->flags & AIF_BRAINDEAD) return;
	if (a->cl_list >= 0 && a->cl_status == 1 && (a->cl_forced || a->aware < 2)) { cl_step(ai, a, u, c); return; }
	tvalid = a->tgt >= 0 && ai->tgt[a->tgt].valid;
	if (a->aware == 2 && !tvalid) {                          /* the target died or left: back to normal unless somebody else is in view */
		a->aware = 0; a->tgt = AI_NO_TARGET; a->notice = 0.0f; a->tgt_vis = 0;
		if (a->st != AIS_SLEEP) set_state(ai, a, entry_state(ai, a, u));
	}
	if (a->st == AIS_SLEEP) {
		if (a->aware == 0) return;
		set_state(ai, a, AIS_ALERT);
	}
	if (a->aware == 2) {
		unsigned since = tick - a->tgt_seen_tick;
		if (a->tgt_vis || since < LOSE_TICKS) {
			unsigned want = AIS_COMBAT;
			int armed = ammo_ok(p, u), follower = a->orders == AIO_FOLLOW_PLAYER || a->orders == AIO_FOLLOW_SQUAD;
			float frac = hp_fraction(ai, a), lp[3], lyaw;
			if (a->orders == AIO_RETREAT) want = AIS_RETREAT;
			else if (sp.flee_unarmed && !armed) want = AIS_FLEE;
			else if (tick < a->retreat_until) want = AIS_FLEE;
			else if (!(a->flags & AIF_BERSERK) && ((sp.retreat > 0.0f && frac < sp.retreat) || (sp.flee_alone && frac < 0.75f && enc_alive_n(ai, a->enc) == 1u)) &&
			         (a->retreat_until == 0 || tick >= a->retreat_until + 150u)) {
				a->retreat_until = tick + 150u;
				want = AIS_FLEE;
			} else if ((a->flags & AIF_BERSERK) || sp.always_charge) want = AIS_CHARGE;
			if (follower && want != AIS_FLEE && leader_pos(ai, a, lp, &lyaw) && dist2d2(u->body.pos, lp) > 16.0f * 16.0f &&
			    dist2d2(u->body.pos, a->tgt_pos) > 8.0f * 8.0f) want = AIS_FOLLOW;
			set_state(ai, a, want);
		} else {
			aidef_squad_def sq;
			if (a->st != AIS_SEARCH) {
				if (a->squad < AI_MAX_SQUADS && aidef_get_squad(ai->env.aidef, a->squad, &sq) && (sq.flags & AIDEF_SQUAD_NEVER_SEARCH)) {
					a->aware = 0; a->tgt = AI_NO_TARGET; a->notice = 0.0f;
					set_state(ai, a, entry_state(ai, a, u));
				} else { a->search_until = tick + SEARCH_TICKS; set_state(ai, a, AIS_SEARCH); }
			} else if (a->search_until && tick >= a->search_until) {
				a->aware = 0; a->tgt = AI_NO_TARGET; a->notice = 0.0f; a->search_until = 0;
				set_state(ai, a, entry_state(ai, a, u));
			}
		}
	} else if (a->aware == 1) {
		if (a->st != AIS_ALERT && a->st != AIS_SEARCH) set_state(ai, a, AIS_ALERT);
		if (a->st == AIS_ALERT && tick - a->state_tick > ALERT_TICKS) { a->search_until = tick + SEARCH_TICKS; set_state(ai, a, AIS_SEARCH); }
		else if (a->st == AIS_SEARCH && a->search_until && tick >= a->search_until) {
			a->aware = 0; a->search_until = 0;
			set_state(ai, a, entry_state(ai, a, u));
		}
	} else {
		int calm = a->st == AIS_IDLE || a->st == AIS_PATROL || a->st == AIS_FOLLOW || (a->st == AIS_SEARCH && a->search_until == 0) ||
		           (a->st == AIS_RETREAT && a->orders == AIO_RETREAT);
		if (a->orders != AIO_NONE) { unsigned w = orders_state(a); if (a->st != w) set_state(ai, a, w); }
		else if (!calm) set_state(ai, a, entry_state(ai, a, u));
		else if (a->st == AIS_IDLE && ai->st.follow_default && is_friend_team(a->team) && ai->tgt[UNIT_MAX].valid && a->init_state != AIDEF_STATE_SLEEPING &&
		         dist2d2(u->body.pos, ai->tgt[UNIT_MAX].pos) < 15.0f * 15.0f) {
			a->orders = AIO_FOLLOW_PLAYER;
			set_state(ai, a, AIS_FOLLOW);
		}
	}
	switch (a->st) {
	case AIS_IDLE: do_idle(ai, a, u, c); break;
	case AIS_PATROL: do_patrol(ai, a, u, c); break;
	case AIS_ALERT: face_point(u, a->tgt_pos, c); break;
	case AIS_SEARCH: do_search(ai, a, u, c); break;
	case AIS_COMBAT: do_combat(ai, a, u, c, 0); break;
	case AIS_CHARGE: do_combat(ai, a, u, c, 1); break;
	case AIS_RETREAT:
		if (a->aware == 2) do_combat(ai, a, u, c, 2);
		else {
			nav_out o;
			if (nav_move(ai, a, u, a->home, 0.8f, &o)) { if (a->orders == AIO_RETREAT) a->orders = AIO_DEFEND; c->aim_yaw = a->home_yaw; }
			else apply_move(c, u, &o, 0.0f, 0, 0);
		}
		break;
	case AIS_FLEE: do_flee(ai, a, u, c); break;
	case AIS_FOLLOW: do_follow(ai, a, u, c); break;
	default: break;
	}
}

static void actor_tick(ai_t *ai, unsigned idx)
{
	ai_actor *a = &ai->st.actor[idx];
	unit_t *u;
	unit_control_t c;
	unsigned tick = ai->st.tick;
	if (!a->active) return;
	u = unit_at(ai, a->unit);
	if (!u->active || !obj_live(ai, a->obj) || !units_alive(ai->env.units, ai->env.game, a->unit)) {
		ai->st.st_deaths++;
		actor_free(ai, a);
		return;
	}
	if (a->bsp != ai->env.bsp || (u->flags & UF_NO_AI)) return;
	memset(&c, 0, sizeof c);
	c.weapon_slot = -1;
	c.aim_yaw = u->body.yaw;
	if (!(a->flags & AIF_FORCE_ACTIVE) && a->aware < 2 && !(a->cl_list >= 0 && a->cl_forced) && ai->tgt[UNIT_MAX].valid &&
	    dist2d2(u->body.pos, ai->tgt[UNIT_MAX].pos) > ACTIVE_DIST * ACTIVE_DIST) {
		ai->st.st_dormant_ticks++;
		units_set_control(ai->env.units, a->unit, &c);
		return;
	}
	ai->st.st_think++;
	check_damage(ai, a, u);
	hear_noises(ai, a, u);
	if ((tick + idx) % (a->aware == 2 ? 3u : PERC_PERIOD) == 0) { perceive(ai, a, u); a->perc_tick = tick; }
	think(ai, a, u, &c);
	units_set_control(ai->env.units, a->unit, &c);
}

/* ---- conversations -------------------------------------------------------------------------------------------------------------------- */
static void conv_event(ai_t *ai, unsigned conv, unsigned line, unsigned part, int kind, uint32_t sound, int obj, float run_to)
{
	ai_sound_event ev;
	memset(&ev, 0, sizeof ev);
	ev.kind = (uint8_t)kind; ev.participant = (uint8_t)part; ev.conv = (uint16_t)conv; ev.line = (uint16_t)line;
	ev.obj = (int16_t)obj; ev.sound = sound; ev.run_to_player = run_to;
	if (obj_live(ai, obj)) cp3(ev.pos, ai->env.game->objects->obj[obj].pos);
	if (ai->sound) ai->sound(ai->sound_user, &ev);
}

static int conv_resolve(ai_t *ai, unsigned conv, const aidef_conversation_def *cd)
{
	ai_conv_rt *cr = &ai->st.conv[conv];
	unsigned n = cd->participant_count < AI_CONV_PARTS ? cd->participant_count : AI_CONV_PARTS, k, i, got = 0;
	uint8_t used[AI_MAX_ACTORS];
	memset(used, 0, sizeof used);
	cr->nparts = (uint8_t)n;
	for (k = 0; k < AI_CONV_PARTS; k++) cr->part_obj[k] = -1;
	for (k = 0; k < n; k++) {
		aidef_participant_def pd;
		int best = -1;
		float bd = 1e30f;
		if (!aidef_get_participant(ai->env.aidef, cd, k, &pd)) continue;
		for (i = 0; i < AI_MAX_ACTORS; i++) {
			const ai_actor *a = &ai->st.actor[i];
			float d = 0.0f, pen = 0.0f;
			if (!a->active || used[i]) continue;
			if (pd.encounter_index >= 0 && a->enc != (uint16_t)pd.encounter_index) continue;
			if (pd.actor_type < AIDEF_ACTOR_TYPES && ai->st.prof[a->prof].type != pd.actor_type) pen = pd.encounter_index >= 0 ? 500.0f : 1e6f;
			if (pen >= 1e6f) continue;
			if (ai->tgt[UNIT_MAX].valid) d = dist3(unit_at(ai, a->unit)->body.pos, ai->tgt[UNIT_MAX].pos);
			if (d + pen < bd) { bd = d + pen; best = (int)i; }
		}
		if (best >= 0) { used[best] = 1; cr->part_obj[k] = ai->st.actor[best].obj; got++; }
	}
	return (int)got;
}

static uint32_t conv_take(ai_t *ai, const aidef_participant_def *pd, const aidef_line_def *ln, int obj)
{
	unsigned k, first = 0;
	int au = obj >= 0 ? units_of_object(ai->env.units, obj) : -1;
	if (au >= 0 && ai->env.game->unit) {
		unitdef_biped_def bd;
		if (unitdef_get_biped(ai->env.game->unit, (uint32_t)ai->env.units->u[au].biped, &bd)) {
			unsigned j;
			for (k = 0; k < 6; k++) {
				if (pd->variants[k] < 0 || !ln->sounds[k]) continue;
				for (j = 0; j < bd.dialogue_count && j < UNITDEF_MAX_DIALOGUE; j++) if ((int)bd.dialogue_variant[j] == pd->variants[k]) return ln->sounds[k];
			}
		}
	}
	for (k = 0; k < 6; k++) if (ln->sounds[k]) { first = ln->sounds[k]; break; }
	return first;
}

static void conv_end(ai_t *ai, unsigned conv, int status)
{
	ai_conv_rt *cr = &ai->st.conv[conv];
	if (cr->status == AI_CONV_PLAYING) conv_event(ai, conv, cr->line, 0, AI_SND_LINE_STOP, 0, -1, 0.0f);
	conv_event(ai, conv, cr->line, 0, AI_SND_CONV_END, 0, -1, 0.0f);
	cr->status = (uint8_t)status;
}

static void conv_tick(ai_t *ai)
{
	unsigned c, tick = ai->st.tick;
	for (c = 0; c < ai->st.conv_count && c < AI_MAX_CONVS; c++) {
		ai_conv_rt *cr = &ai->st.conv[c];
		aidef_conversation_def cd;
		if (cr->status != AI_CONV_STARTING && cr->status != AI_CONV_PLAYING && cr->status != AI_CONV_ENDING) continue;
		if (!aidef_get_conversation(ai->env.aidef, c, &cd)) { cr->status = AI_CONV_ABORTED; continue; }
		if (cr->status == AI_CONV_STARTING) {
			if (conv_resolve(ai, c, &cd) > 0) { cr->status = AI_CONV_PLAYING; cr->line = 0; cr->timer = 0; cr->started = tick; }
			else if (tick - cr->started > 150u) cr->status = AI_CONV_ABORTED;
			continue;
		}
		if (cr->status == AI_CONV_ENDING) { cr->status = AI_CONV_DONE; continue; }
		if (cr->timer == 0) {                                       /* delay of the first line */
			aidef_line_def ln;
			cr->timer = tick;
			if (aidef_get_line(ai->env.aidef, &cd, 0, &ln)) cr->timer = tick + secs(ln.delay);
		}
		if (tick < cr->timer) continue;
		if (cr->line >= cd.line_count) { conv_end(ai, c, AI_CONV_ENDING); continue; }
		{
			aidef_line_def ln, nx;
			aidef_participant_def pd;
			unsigned part, dur = 0;
			int obj = -1;
			if (!aidef_get_line(ai->env.aidef, &cd, cr->line, &ln)) { cr->line++; continue; }
			part = ln.participant >= 0 ? (unsigned)ln.participant : 0u;
			if (part < cr->nparts) obj = cr->part_obj[part];
			if (obj >= 0 && !obj_live(ai, obj)) { obj = -1; cr->part_obj[part] = -1; }
			if (obj >= 0 && aidef_get_participant(ai->env.aidef, &cd, part, &pd)) {
				uint32_t snd = conv_take(ai, &pd, &ln, obj);
				int d = 0;
				if (ai->sound) {
					ai_sound_event ev;
					memset(&ev, 0, sizeof ev);
					ev.kind = AI_SND_LINE_START; ev.participant = (uint8_t)part; ev.conv = (uint16_t)c; ev.line = cr->line; ev.obj = (int16_t)obj; ev.sound = snd;
					ev.run_to_player = cd.run_to_player_distance;
					cp3(ev.pos, ai->env.game->objects->obj[obj].pos);
					d = ai->sound(ai->sound_user, &ev);
				}
				dur = d > 0 ? (unsigned)d : 90u;
				ai->st.st_conv_lines++;
			}
			cr->line++;
			if (cr->line < cd.line_count && aidef_get_line(ai->env.aidef, &cd, cr->line, &nx)) cr->timer = tick + dur + secs(nx.delay);
			else cr->timer = tick + dur;
		}
	}
}

int ai_conversation_start(ai_t *ai, int conv)
{
	ai_conv_rt *cr;
	if (conv < 0 || (unsigned)conv >= ai->st.conv_count || (unsigned)conv >= AI_MAX_CONVS) return 0;
	cr = &ai->st.conv[conv];
	if (cr->status == AI_CONV_STARTING || cr->status == AI_CONV_PLAYING) return 1;
	memset(cr, 0, sizeof *cr);
	cr->status = AI_CONV_STARTING; cr->started = ai->st.tick;
	return 1;
}
void ai_conversation_stop(ai_t *ai, int conv)
{
	if (conv < 0 || (unsigned)conv >= ai->st.conv_count || (unsigned)conv >= AI_MAX_CONVS) return;
	if (ai->st.conv[conv].status == AI_CONV_STARTING || ai->st.conv[conv].status == AI_CONV_PLAYING) conv_end(ai, (unsigned)conv, AI_CONV_DONE);
}
void ai_conversation_advance(ai_t *ai, int conv)
{
	if (conv < 0 || (unsigned)conv >= ai->st.conv_count || (unsigned)conv >= AI_MAX_CONVS) return;
	if (ai->st.conv[conv].status == AI_CONV_PLAYING) {
		conv_event(ai, (unsigned)conv, ai->st.conv[conv].line, 0, AI_SND_LINE_STOP, 0, -1, 0.0f);
		ai->st.conv[conv].timer = ai->st.tick;
	}
}
int ai_conversation_status(const ai_t *ai, int conv)
{
	if (conv < 0 || (unsigned)conv >= ai->st.conv_count || (unsigned)conv >= AI_MAX_CONVS) return AI_CONV_NONE;
	return ai->st.conv[conv].status;
}
int ai_conversation_line(const ai_t *ai, int conv)
{
	if (conv < 0 || (unsigned)conv >= ai->st.conv_count || (unsigned)conv >= AI_MAX_CONVS) return 0;
	return ai->st.conv[conv].line;
}

/* ---- orders ---------------------------------------------------------------------------------------------------------------------------- */
void ai_set_orders(ai_t *ai, const ai_ref *r, int orders)
{
	unsigned i, first, count;
	if (orders < 0 || orders >= AIO_COUNT) return;
	if (ai_ref_squad_range(ai, r, &first, &count))
		for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) if (ai_ref_has_squad(ai, r, i)) ai->st.squad[i].orders = (uint8_t)orders;
	if (r->kind == 1 && (unsigned)r->enc < AI_MAX_ENCOUNTERS) ai->st.enc[r->enc].orders = (uint8_t)orders;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, r)) continue;
		a->orders = (uint8_t)orders;
		if (a->st == AIS_SLEEP && orders != AIO_NONE) set_state(ai, a, AIS_IDLE);
		if (orders == AIO_DEFEND && a->aware < 2) { cp3(a->home, unit_at(ai, a->unit)->body.pos); a->home_yaw = unit_at(ai, a->unit)->body.yaw; }
	}
}

void ai_set_follow(ai_t *ai, const ai_ref *r, const ai_ref *target)
{
	unsigned i, first, count;
	int gs = -1;
	if (target && target->kind) {
		unsigned tf, tc;
		if (ai_ref_squad_range(ai, target, &tf, &tc)) {
			gs = (int)tf;
			for (i = tf; i < tf + tc && i < AI_MAX_SQUADS; i++) if (squad_alive_n(ai, i) > 0) { gs = (int)i; break; }
		}
	}
	if (ai_ref_squad_range(ai, r, &first, &count))
		for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) if (ai_ref_has_squad(ai, r, i)) ai->st.squad[i].follow = (int16_t)gs;
	ai_set_orders(ai, r, gs >= 0 ? AIO_FOLLOW_SQUAD : AIO_FOLLOW_PLAYER);
}

void ai_set_flag(ai_t *ai, const ai_ref *r, unsigned flag, int on)
{
	unsigned i, first, count;
	uint8_t f = (uint8_t)flag;
	if (ai_ref_squad_range(ai, r, &first, &count))
		for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) {
			if (!ai_ref_has_squad(ai, r, i)) continue;
			if (on) ai->st.squad[i].flags |= f; else ai->st.squad[i].flags &= (uint8_t)~f;
		}
	if (r->kind == 1 && (unsigned)r->enc < AI_MAX_ENCOUNTERS) { if (on) ai->st.enc[r->enc].flags |= f; else ai->st.enc[r->enc].flags &= (uint8_t)~f; }
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, r)) continue;
		if (on) a->flags |= f; else a->flags &= (uint8_t)~f;
	}
}

void ai_set_flag_unit(ai_t *ai, int unit, unsigned flag, int on)
{
	int i = ai_actor_of_unit(ai, unit);
	if (i < 0) return;
	if (on) ai->st.actor[i].flags |= (uint8_t)flag; else ai->st.actor[i].flags &= (uint8_t)~flag;
}

static void see_target(ai_t *ai, ai_actor *a, int t)
{
	if (t < 0 || !ai->tgt[t].valid) return;
	acquire(ai, a, t);
	a->tgt_vis = 1;
	cp3(a->tgt_pos, ai->tgt[t].chest);
	a->tgt_seen_tick = ai->st.tick;
	if (a->st == AIS_SLEEP) set_state(ai, a, AIS_ALERT);
}

void ai_magically_see_players(ai_t *ai, const ai_ref *r)
{
	unsigned i;
	build_targets(ai);
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai_actor_in_ref(ai, &ai->st.actor[i], r)) see_target(ai, &ai->st.actor[i], (int)UNIT_MAX);
}

void ai_magically_see_encounter(ai_t *ai, const ai_ref *observer, const ai_ref *seen)
{
	unsigned i, j;
	build_targets(ai);
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		int best = -1;
		float bd = 1e30f;
		if (!ai_actor_in_ref(ai, a, observer)) continue;
		for (j = 0; j < AI_MAX_ACTORS; j++) {
			const ai_actor *b = &ai->st.actor[j];
			float d;
			if (!ai_actor_in_ref(ai, b, seen) || !hostile_target(ai, a, b->unit)) continue;
			d = dist2d2(ai->tgt[b->unit].pos, unit_at(ai, a->unit)->body.pos);
			if (d < bd) { bd = d; best = b->unit; }
		}
		if (best >= 0) see_target(ai, a, best);
	}
}

void ai_link_activation(ai_t *ai, const ai_ref *a, const ai_ref *b)
{
	unsigned i;
	if (a->kind == 0 || b->kind == 0 || a->enc == b->enc) return;
	for (i = 0; i < ai->st.link_n; i++) if (ai->st.link[i][0] == a->enc && ai->st.link[i][1] == b->enc) return;
	if (ai->st.link_n < AI_MAX_LINKS) { ai->st.link[ai->st.link_n][0] = a->enc; ai->st.link[ai->st.link_n][1] = b->enc; ai->st.link_n++; }
}

void ai_allegiance(ai_t *ai, int a, int b, int mode)
{
	if (a < 0 || b < 0 || a >= (int)AIDEF_TEAMS || b >= (int)AIDEF_TEAMS || mode < 0 || mode > 2) return;
	ai->st.rel[a][b] = ai->st.rel[b][a] = (uint8_t)mode;
}

/* one actor joins squad `dgsq` (definition `dsq`): it takes its behaviour and one of its places */
static void migrate_one(ai_t *ai, ai_actor *a, unsigned enc, unsigned dgsq, const aidef_squad_def *dsq, unsigned k)
{
	a->enc = (uint16_t)enc; a->squad = (uint16_t)dgsq; a->platoon = dsq->platoon >= 0 ? (uint16_t)dsq->platoon : AI_NONE16;
	a->init_state = (uint8_t)dsq->initial_state; a->ret_state = (uint8_t)dsq->return_state;
	a->orders = ai->st.squad[dgsq].orders;
	if (a->aware < 2) a->aware = 0;
	a->fp = -1; a->wp_n = 0; a->goal_valid = 0;
	if (dsq->start_count) {                                    /* walk to one of its starting places */
		aidef_start_def st;
		if (aidef_get_start(ai->env.aidef, dsq, k % dsq->start_count, &st)) { cp3(a->home, st.pos); a->home_yaw = st.facing; }
	}
	if (a->aware < 2 && a->st != AIS_SLEEP) {
		unsigned w = state_for(dsq->initial_state);
		if (w == AIS_SLEEP || (w == AIS_PATROL && !dsq->start_count && !dsq->move_count)) w = AIS_IDLE;
		if (w == AIS_SEARCH) a->search_until = 0;
		set_state(ai, a, w);
		a->wander_tick = 0;
	}
}

/* the destination squad of a migration: the first squad of `to` that has places or a behaviour of its own */
static int migrate_target(const ai_t *ai, const ai_ref *to, unsigned *dgsq, aidef_squad_def *dsq)
{
	unsigned i, first, count;
	if (to->kind == 0 || !ai_ref_squad_range(ai, to, &first, &count) || count == 0) return 0;
	*dgsq = first;
	for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) if (ai_ref_has_squad(ai, to, i)) { *dgsq = i; break; }
	return aidef_get_squad(ai->env.aidef, *dgsq, dsq);
}

void ai_migrate(ai_t *ai, const ai_ref *from, const ai_ref *to)
{
	unsigned i, k = 0, dgsq;
	aidef_squad_def dsq;
	if (!migrate_target(ai, to, &dgsq, &dsq)) return;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, from)) continue;
		migrate_one(ai, a, (unsigned)to->enc, dgsq, &dsq, k++);
	}
}

void ai_migrate_unit(ai_t *ai, int unit, const ai_ref *to)
{
	int i = ai_actor_of_unit(ai, unit);
	unsigned dgsq;
	aidef_squad_def dsq;
	if (i < 0 || !migrate_target(ai, to, &dgsq, &dsq)) return;
	migrate_one(ai, &ai->st.actor[i], (unsigned)to->enc, dgsq, &dsq, ai->st.squad[dgsq].next_start++);
}

void ai_free(ai_t *ai, const ai_ref *r)
{
	unsigned i;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai_actor_in_ref(ai, &ai->st.actor[i], r)) ai_detach_unit(ai, ai->st.actor[i].unit);
}

/* ---- command list API ------------------------------------------------------------------------------------------------------------------ */
static void cl_assign(ai_t *ai, ai_actor *a, int list, int forced)
{
	a->cl_list = (int16_t)list; a->cl_pc = 0; a->cl_phase = 0; a->cl_forced = (uint8_t)forced; a->cl_look_kind = 0; a->cl_radius = ARRIVE_R; a->cl_skip = 0;
	a->cl_status = list >= 0 ? 1u : 0u;
	a->cl_loops = 0;
	a->goal_valid = 0; a->wp_n = 0;
	(void)ai;
}

int ai_command_list(ai_t *ai, const ai_ref *r, int list, int forced)
{
	unsigned i, n = 0;
	if (list >= 0 && (unsigned)list >= aidef_command_list_count(ai->env.aidef)) return 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai_actor_in_ref(ai, &ai->st.actor[i], r)) { cl_assign(ai, &ai->st.actor[i], list, forced); n++; }
	return (int)n;
}

int ai_command_list_unit(ai_t *ai, int unit, int list)
{
	int i = ai_actor_of_unit(ai, unit);
	if (i < 0 || (list >= 0 && (unsigned)list >= aidef_command_list_count(ai->env.aidef))) return 0;
	cl_assign(ai, &ai->st.actor[i], list, 1);
	return 1;
}

void ai_command_list_advance(ai_t *ai, const ai_ref *r)
{
	unsigned i;
	for (i = 0; i < AI_MAX_ACTORS; i++) if (ai_actor_in_ref(ai, &ai->st.actor[i], r) && ai->st.actor[i].cl_status == 1) ai->st.actor[i].cl_skip = 1;
}

int ai_command_list_status(const ai_t *ai, int unit)
{
	int i = ai_actor_of_unit(ai, unit);
	if (i < 0) return 2;
	return ai->st.actor[i].cl_status;
}

/* ---- queries ---------------------------------------------------------------------------------------------------------------------------- */
unsigned ai_living_count(const ai_t *ai, const ai_ref *r)
{
	unsigned i, n = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) n += ai_actor_in_ref(ai, &ai->st.actor[i], r);
	return n;
}

unsigned ai_nonswarm_count(const ai_t *ai, const ai_ref *r)
{
	unsigned i, n = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++)
		if (ai_actor_in_ref(ai, &ai->st.actor[i], r) && ai->st.prof[ai->st.actor[i].prof].type != AIDEF_ACTOR_INFECTION_FORM) n++;
	return n;
}

float ai_living_fraction(const ai_t *ai, const ai_ref *r)
{
	unsigned i, first, count, spawned = 0;
	if (!ai_ref_squad_range(ai, r, &first, &count)) return 0.0f;
	for (i = first; i < first + count && i < AI_MAX_SQUADS; i++) if (ai_ref_has_squad(ai, r, i)) spawned += ai->st.squad[i].spawned;
	return spawned ? (float)ai_living_count(ai, r) / (float)spawned : 0.0f;
}

float ai_strength(const ai_t *ai, const ai_ref *r)
{
	unsigned i, n = 0;
	float s = 0.0f;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		const ai_actor *a = &ai->st.actor[i];
		if (!ai_actor_in_ref(ai, a, r)) continue;
		s += hp_fraction(ai, a);
		n++;
	}
	return n ? s / (float)n : 0.0f;
}

int ai_status(const ai_t *ai, const ai_ref *r)
{
	unsigned i, n = 0, fighting = 0;
	int top = 0;
	for (i = 0; i < AI_MAX_ACTORS; i++) {
		const ai_actor *a = &ai->st.actor[i];
		int lv;
		if (!ai_actor_in_ref(ai, a, r)) continue;
		n++;
		switch (a->st) {
		case AIS_ALERT: lv = 2; break;
		case AIS_SEARCH: lv = a->search_until ? 3 : 1; break;
		case AIS_COMBAT: case AIS_CHARGE: case AIS_RETREAT: case AIS_FLEE: lv = 4; break;
		default: lv = 1; break;
		}
		if (lv >= 4) fighting++;
		if (lv > top) top = lv;
	}
	if (n == 0) return 0;
	if (top >= 4 && fighting * 2u >= n) top = fighting == n && n > 1u ? 6 : 5;
	return top;
}

unsigned ai_actor_objects(const ai_t *ai, const ai_ref *r, int16_t *out, unsigned max)
{
	unsigned i, n = 0;
	for (i = 0; i < AI_MAX_ACTORS && n < max; i++) if (ai_actor_in_ref(ai, &ai->st.actor[i], r)) out[n++] = ai->st.actor[i].obj;
	return n;
}

/* ---- the tick --------------------------------------------------------------------------------------------------------------------------- */
void ai_tick(ai_t *ai)
{
	unsigned k, start;
	if (!ai->env.game || !ai->env.units || !ai->env.aidef) return;
	ai->st.tick++;
	if (!ai->st.enabled) return;
	ai->routes_left = ai->max_routes_per_tick;
	build_targets(ai);
	begin_noises(ai);
	if (ai->st.tick % 10u == 0) {
		unsigned g;
		for (g = 0; g < ai->st.squad_count && g < AI_MAX_SQUADS; g++) {
			ai_squad_rt *q = &ai->st.squad[g];
			if (q->spawn_at && ai->st.tick >= q->spawn_at && enc_bsp(ai, q->enc) == (unsigned)ai->env.bsp) place_squad_now(ai, q->enc, g, 0);
		}
	}
	start = ai->st.tick % AI_MAX_ACTORS;                    /* rotate so the route budget is shared fairly */
	for (k = 0; k < AI_MAX_ACTORS; k++) actor_tick(ai, (start + k) % AI_MAX_ACTORS);
	conv_tick(ai);
}

