#include "sim/game.h"
#include "sim/playerdef.h"
#include <math.h>
#include <string.h>

#define TICKS 30.0f
#define MELEE_RANGE 1.1f
#define PICKUP_RANGE 1.6f

#define OBJ_OFF(o) ((o)->model < 0 || ((o)->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)))

/* ---- small helpers ---------------------------------------------------------------------------------------------------- */
static float rnd01(game_t *g)
{
	uint32_t x = g->rng ? g->rng : 0x9E3779B9u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	g->rng = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static game_event_t *push(game_t *g, int type, uint32_t tag, const float pos[3], const float normal[3])
{
	game_event_t *e;
	if (g->ev_count >= GAME_MAX_EVENTS) return NULL;
	e = &g->ev[g->ev_count++];
	memset(e, 0, sizeof *e);
	e->type = (uint8_t)type; e->tag = tag; e->obj = -1;
	if (pos) { e->pos[0] = pos[0]; e->pos[1] = pos[1]; e->pos[2] = pos[2]; }
	if (normal) { e->normal[0] = normal[0]; e->normal[1] = normal[1]; e->normal[2] = normal[2]; }
	return e;
}

static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void game_eye(const game_t *g, float eye[3], float fwd[3])
{
	float cp = cosf(g->player.pitch);
	player_eye(&g->player, &g->pp, eye);
	fwd[0] = cp * cosf(g->player.yaw); fwd[1] = cp * sinf(g->player.yaw); fwd[2] = sinf(g->player.pitch);
}

const game_slot_t *game_cur_slot(const game_t *g) { return g->cur >= 0 ? &g->slot[g->cur] : NULL; }
const game_event_t *game_events(const game_t *g, unsigned *count) { *count = g->ev_count; return g->ev; }

/* ---- first person ----------------------------------------------------------------------------------------------------- */
static int fp_anim_for(const game_t *g, const game_slot_t *s, unsigned action)
{
	ansm_graph gr;
	if (!g->ansm || s->fp_graph < 0 || s->weapon < 0) return -1;
	if (ansm_find_graph(g->ansm, s->def.fp_anim_tag, &gr) != 0) return -1;
	return ansm_fp_slot(g->ansm, &gr, 0, ANSM_ACTION_SLOT(action));
}

static void fp_set(game_t *g, int anim)
{
	fpview_t *f = g->fp;
	if (!f || !f->active) return;
	f->arms_anim = f->weapon_anim = anim >= 0 ? (unsigned)anim : SCEN_NO_ANIM;
	f->arms_frame = f->weapon_frame = 0.0f;
}

static void fp_play(game_t *g, unsigned action)
{
	const game_slot_t *s = game_cur_slot(g);
	int a;
	ansm_graph gr;
	ansm_anim info;
	if (!s) return;
	a = fp_anim_for(g, s, action);
	if (a < 0) return;
	fp_set(g, a);
	g->fp_action = (int)action;
	g->fp_timer = 0;
	if (ansm_find_graph(g->ansm, s->def.fp_anim_tag, &gr) == 0 && ansm_anim_info(g->ansm, &gr, (uint32_t)a, &info) == 0) g->fp_timer = info.ticks;
}

static void fp_apply_slot(game_t *g)
{
	fpview_t *f = g->fp;
	const game_slot_t *s = game_cur_slot(g);
	if (!f || !f->active) return;
	if (!s || s->weapon < 0 || s->fp_model < 0) { f->weapon_model = -1; fp_set(g, -1); return; }
	f->weapon_model = s->fp_model;
	f->weapon_graph = s->fp_graph;
	if (s->fp_graph >= 0) f->arms_graph = s->fp_graph;     /* arms and gun share the weapon's graph (nodes mapped by name) */
	fp_set(g, s->fp_anim_idle);
	g->fp_timer = 0; g->fp_action = -1;
}

/* ---- weapons ----------------------------------------------------------------------------------------------------------- */
void game_give_weapon(game_t *g, int slot, uint32_t weapon_tag)
{
	game_slot_t *s;
	int idx;
	if (slot < 0 || slot >= GAME_SLOTS || !g->weap) return;
	s = &g->slot[slot];
	idx = weap_find_weapon(g->weap, weapon_tag);
	if (idx < 0) return;
	memset(s, 0, sizeof *s);
	s->weapon = idx;
	weap_get_weapon(g->weap, (uint32_t)idx, &s->def);
	weapon_state_init(&s->st, &s->def);
	s->fp_model = s->def.fp_model_tag ? model_find(g->ow.models, s->def.fp_model_tag) : -1;
	s->fp_graph = (s->def.fp_anim_tag && g->ow.anims) ? anim_find_graph(g->ow.anims, s->def.fp_anim_tag) : -1;
	s->fp_anim_idle = fp_anim_for(g, s, ANSM_FP_IDLE);
	if (g->cur < 0) { g->cur = slot; }
	if (g->cur == slot) {
		fp_apply_slot(g);
		fp_play(g, ANSM_FP_READY);
		g->slot[slot].st.mode = WEAP_MODE_READYING;
		g->slot[slot].st.timer = weap_ready_ticks(&g->slot[slot].def);
	}
}

static const weap_weapon *cur_def(const game_t *g) { return g->cur >= 0 && g->slot[g->cur].weapon >= 0 ? &g->slot[g->cur].def : NULL; }

/* ---- object helpers ---------------------------------------------------------------------------------------------------- */
static uint32_t object_tag(const game_t *g, const object_t *o)
{
	return g->ow.scen ? scen_get_def(g->ow.scen, (uint32_t)o->def).tag_id : 0;
}

static void unit_init_one(game_t *g, unsigned i)
{
	const object_t *o = &g->objects->obj[i];
	cmod_object co;
	cmod_model m;
	game_unit_t *u = &g->unit_state[i];
	memset(u, 0, sizeof *u);
	u->model = -1;
	if (g->unit && !(o->flags & OBJ_F_FREE)) {                 /* the unit tag's default team (the HUD tracker colours blips by it) */
		uint32_t tag = object_tag(g, o);
		int k;
		if (o->type == SCEN_BIPED && (k = unitdef_find_biped(g->unit, tag)) >= 0) {
			unitdef_biped_def bd;
			if (unitdef_get_biped(g->unit, (uint32_t)k, &bd)) u->team = (uint8_t)(bd.default_team < 255u ? bd.default_team : 0u);
		} else if (o->type == SCEN_VEHICLE && (k = unitdef_find_vehicle(g->unit, tag)) >= 0) {
			unitdef_vehicle_def vd;
			if (unitdef_get_vehicle(g->unit, (uint32_t)k, &vd)) u->team = (uint8_t)(vd.default_team < 255u ? vd.default_team : 0u);
		}
	}
	if (!g->cmod || o->model < 0 || (o->flags & OBJ_F_FREE) || !cmod_find_object(g->cmod, object_tag(g, o), &co, &m)) return;
	u->model = (int32_t)m.index;
	if (m.max_body_vitality > 0.0f && m.max_body_vitality < 1e9f) {
		float shield = m.max_shield_vitality < 1e9f ? m.max_shield_vitality : 0.0f;
		vitals_init(&u->vitals, m.max_body_vitality, shield, m.shield_recharge_rate, (uint16_t)(m.shield_stun_time * TICKS));
		u->has_vitals = 1;
	}
}

static void unit_init_all(game_t *g)
{
	unsigned i;
	for (i = 0; i < OBJ_MAX; i++) g->unit_state[i].model = -1;
	if (!g->objects) return;
	for (i = 0; i < g->objects->count; i++) unit_init_one(g, i);
}

void game_teleport(game_t *g, const float pos[3], float yaw)
{
	g->player.pos[0] = pos[0]; g->player.pos[1] = pos[1]; g->player.pos[2] = pos[2];
	g->player.vel[0] = g->player.vel[1] = g->player.vel[2] = 0.0f;
	g->player.yaw = yaw; g->player.on_ground = 0;
}

void game_set_collision(game_t *g, const coll_bsp *coll)
{
	g->coll = coll;
	g->has_phys = coll != NULL;
	if (coll) phys_world_from_coll(&g->phys, coll);
}

void game_object_spawned(game_t *g, unsigned index) { if (g->objects && index < g->objects->count) unit_init_one(g, index); }

static const game_unit_t *unit_of(const game_t *g, int obj) { return (obj >= 0 && obj < OBJ_MAX) ? &g->unit_state[obj] : NULL; }

static int pose_object(const game_t *g, const object_t *o, const cmod_model *m, mat4 *out, unsigned max_nodes)
{
	object_nodes_t nodes;
	unsigned n = objects_pose_world(o, &g->ow, &nodes), i;
	for (i = 0; i < m->node_count && i < max_nodes; i++) out[i] = i < n ? nodes.world[i] : o->root;
	return 1;
}

/* Nearest segment hit among object collision models. Returns the object index or -1. */
static int ray_objects(game_t *g, const float a[3], const float b[3], int ignore, cmod_hit *best, float *best_t)
{
	unsigned i;
	int found = -1;
	float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, dl2 = dot3(d, d);
	static mat4 nodes[64];
	if (!g->cmod || !g->objects) return -1;
	for (i = 0; i < g->objects->count; i++) {
		const object_t *o = &g->objects->obj[i];
		const game_unit_t *u = &g->unit_state[i];
		cmod_model m;
		cmod_segment seg;
		cmod_hit h;
		float rel[3], t, c[3], r;
		if ((int)i == ignore || u->model < 0 || OBJ_OFF(o)) continue;
		/* broad phase: the segment against the object's bounding sphere (padded: the tag radius is not a hull) */
		r = o->radius * 1.5f + 0.3f;
		rel[0] = o->pos[0] - a[0]; rel[1] = o->pos[1] - a[1]; rel[2] = o->pos[2] - a[2];
		t = dl2 > 0.0f ? dot3(rel, d) / dl2 : 0.0f;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		c[0] = a[0] + d[0] * t - o->pos[0]; c[1] = a[1] + d[1] * t - o->pos[1]; c[2] = a[2] + d[2] * t - o->pos[2];
		if (dot3(c, c) > r * r) continue;
		if (!cmod_get_model(g->cmod, (uint32_t)u->model, &m) || m.node_count > 64) continue;
		pose_object(g, o, &m, nodes, 64);
		seg.a[0] = a[0]; seg.a[1] = a[1]; seg.a[2] = a[2]; seg.b[0] = b[0]; seg.b[1] = b[1]; seg.b[2] = b[2];
		if (cmod_ray_ex(&m, nodes, &seg, NULL, &h) && (found < 0 || h.t < *best_t)) { *best = h; *best_t = h.t; found = (int)i; }
	}
	return found;
}

float game_damage_object(game_t *g, int obj, float amount, const cmod_hit *hit)
{
	game_unit_t *u;
	float dealt, sm = 1.0f, bm = 1.0f, leak = 0.0f;
	game_event_t *e;
	if (obj < 0 || obj >= (int)OBJ_MAX) return 0.0f;
	u = &g->unit_state[obj];
	if (!u->has_vitals || u->dead) return 0.0f;
	if (hit) {
		if (hit->shield_multiplier > 0.0f) sm = hit->shield_multiplier;
		if (hit->body_multiplier > 0.0f) bm = hit->body_multiplier;
		leak = hit->shield_leak;
	}
	dealt = vitals_damage(&u->vitals, amount, sm, bm, leak);
	g->st_damage += dealt;
	e = push(g, GE_HIT, 0, g->objects->obj[obj].pos, NULL);
	if (e) { e->obj = (int16_t)obj; e->value = dealt; }
	if (vitals_dead(&u->vitals)) {
		object_t *o = &g->objects->obj[obj];
		u->dead = 1;
		g->st_kills++;
		e = push(g, GE_KILL, 0, o->pos, NULL);
		if (e) e->obj = (int16_t)obj;
		/* death animation from the unit's damage table (health kill, from the front, chest), else stop animating */
		if (g->ansm && g->ow.scen) {
			ansm_graph gr;
			scen_def d = scen_get_def(g->ow.scen, (uint32_t)o->def);
			if (d.anim_tag && ansm_find_graph(g->ansm, d.anim_tag, &gr) == 0) {
				int a = ansm_damage_anim(g->ansm, &gr, 3, 0, 1);
				if (a < 0) a = ansm_damage_anim(g->ansm, &gr, 2, 0, 1);
				if (a >= 0) { o->anim = (uint16_t)a; o->frame = 0.0f; o->flags |= OBJ_F_ANIMATED; }
				else o->flags &= ~OBJ_F_ANIMATED;
			}
		}
	}
	return dealt;
}

/* ---- damage effects ---------------------------------------------------------------------------------------------------- */
static void apply_damage_to_player(game_t *g, float amount, const float from[3])
{
	float dealt;
	(void)from;
	if (vitals_dead(&g->vitals) || g->player_frozen) return;      /* the player is out of the fight during a cinematic */
	dealt = vitals_damage(&g->vitals, amount, 1.0f, 1.0f, 0.0f);
	if (dealt > 0.0f) {
		game_event_t *e = push(g, GE_PLAYER_HURT, 0, g->player.pos, NULL);
		if (e) e->value = dealt;
		g->hurt_flash = 1.0f;
		if (g->vitals.flags & VITALS_F_SHIELD_DOWN) push(g, GE_SHIELD_DOWN, 0, g->player.pos, NULL);
	}
	if (vitals_dead(&g->vitals)) push(g, GE_PLAYER_DEAD, 0, g->player.pos, NULL);
}

static void camera_shake_from(game_t *g, const weap_damage *d)
{
	if (d->shake_rotation > 0.0f && d->shake_rotation * 0.017453f > g->shake) g->shake = d->shake_rotation * 0.017453f;
}

/* Area damage of a damage effect centred on `c`: every object and the player inside its outer radius. */
static void area_damage(game_t *g, const weap_damage *d, const float c[3], int ignore_obj)
{
	unsigned i;
	float outer = d->radius[1];
	if (outer <= 0.0f) return;
	for (i = 0; g->objects && i < g->objects->count; i++) {
		const object_t *o = &g->objects->obj[i];
		float dv[3] = { o->pos[0] - c[0], o->pos[1] - c[1], o->pos[2] - c[2] };
		float dist = len3(dv), amount;
		if (!g->unit_state[i].has_vitals || g->unit_state[i].dead || OBJ_OFF(o)) continue;
		if ((int)i == ignore_obj) continue;
		/* distance to the object's surface, approximated by its centre minus the bounding radius */
		dist -= o->radius * 0.5f;
		if (dist < 0.0f) dist = 0.0f;
		if (dist > outer) continue;
		amount = weap_damage_at_radius(d, dist, rnd01(g));
		if (amount > 0.0f) game_damage_object(g, (int)i, amount, NULL);
	}
	{
		float eye[3], dv[3], dist, amount;
		player_eye(&g->player, &g->pp, eye);
		dv[0] = eye[0] - c[0]; dv[1] = eye[1] - c[1]; dv[2] = eye[2] - c[2];
		dist = len3(dv);
		if (dist <= outer) {
			amount = weap_damage_at_radius(d, dist, rnd01(g));
			if (amount > 0.0f) apply_damage_to_player(g, amount, c);
			camera_shake_from(g, d);
		}
	}
}

static void run_effect_damage(game_t *g, uint32_t effect_tag, const float pos[3], int ignore_obj)
{
	int ei;
	fxd_effect e;
	unsigned k, p;
	if (!g->fx || !g->weap || !effect_tag) return;
	ei = fxdef_find_effect(g->fx, effect_tag);
	if (ei < 0) return;
	e = fxdef_get_effect(g->fx, (uint32_t)ei);
	for (k = 0; k < e.event_count; k++) {
		fxd_event ev = fxdef_get_event(g->fx, &e, k);
		for (p = 0; p < ev.part_count; p++) {
			fxd_part part = fxdef_get_part(g->fx, &ev, p);
			if (part.kind == FXD_K_DAMAGE) {
				int di = weap_find_damage(g->weap, part.tag_id);
				weap_damage d;
				if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) {
					push(g, GE_DAMAGE, part.tag_id, pos, NULL);
					area_damage(g, &d, pos, ignore_obj);
				}
			}
		}
	}
}

/* ---- projectiles ------------------------------------------------------------------------------------------------------- */
static void proj_free(game_proj_t *p) { p->active = 0; }

static game_proj_t *proj_alloc(game_t *g)
{
	unsigned i;
	for (i = 0; i < GAME_MAX_PROJ; i++) if (!g->proj[i].active) return &g->proj[i];
	return NULL;
}

static void proj_spawn(game_t *g, unsigned pidx, const float pos[3], const float dir[3], float speed_override, int grenade)
{
	game_proj_t *p = proj_alloc(g);
	weap_projectile def;
	float s, dl = len3(dir);
	if (!p || !weap_get_projectile(g->weap, pidx, &def) || dl < 1e-6f) return;
	memset(p, 0, sizeof *p);
	p->active = 1; p->proj = (uint16_t)pidx; p->owner = -1; p->grenade = (uint8_t)grenade;
	s = speed_override > 0.0f ? speed_override : def.initial_velocity;
	p->pos[0] = pos[0]; p->pos[1] = pos[1]; p->pos[2] = pos[2];
	p->vel[0] = dir[0] / dl * s; p->vel[1] = dir[1] / dl * s; p->vel[2] = dir[2] / dl * s;
	if (def.initial_velocity > 0.0f && !grenade) {
		float range = def.max_range > 0.0f ? def.max_range : 100.0f;
		p->life = (uint16_t)(range / def.initial_velocity + 1.0f);
	} else {
		float t = def.timer[0] > 0.0f ? def.timer[0] + (def.timer[1] - def.timer[0]) * rnd01(g) : 3.0f;
		p->life = (uint16_t)(t * TICKS);
	}
	if (p->life < 2) p->life = 2;
	g->st_projectiles++;
}

int game_fire_projectile(game_t *g, int owner, unsigned proj, const float pos[3], const float dir[3], float speed_per_tick)
{
	unsigned i;
	for (i = 0; i < GAME_MAX_PROJ && g->proj[i].active; i++) {}
	if (i == GAME_MAX_PROJ || !g->weap) return 0;
	proj_spawn(g, proj, pos, dir, speed_per_tick, 0);
	g->proj[i].owner = (int16_t)owner;
	return 1;
}

int game_throw_grenade(game_t *g, int owner, int type, const float pos[3], const float dir[3], float speed)
{
	weap_grenade gr;
	int pi;
	unsigned i;
	if (!g->weap || type < 0 || !weap_get_grenade(g->weap, (uint32_t)type, &gr) || !gr.projectile) return 0;
	pi = weap_find_projectile(g->weap, gr.projectile);
	if (pi < 0) return 0;
	for (i = 0; i < GAME_MAX_PROJ && g->proj[i].active; i++) {}
	if (i == GAME_MAX_PROJ) return 0;
	proj_spawn(g, (unsigned)pi, pos, dir, speed > 0.0f ? speed / TICKS : 0.0f, 1);
	if (!g->proj[i].active) return 0;
	g->proj[i].owner = (int16_t)owner;
	g->st_throws++;
	return 1;
}

void game_set_vitals(game_t *g, int obj, float health, float shield, float recharge_per_tick, uint16_t stun_ticks)
{
	game_unit_t *u;
	if (obj < 0 || obj >= (int)OBJ_MAX) return;
	u = &g->unit_state[obj];
	if (u->dead) return;
	vitals_init(&u->vitals, health, shield, recharge_per_tick, stun_ticks);
	u->has_vitals = health > 0.0f;
}

float game_damage_player(game_t *g, float amount, const float from[3])
{
	g->vitals.last_damage = 0.0f;
	apply_damage_to_player(g, amount, from);
	return g->vitals.last_damage;
}

static void detonate(game_t *g, game_proj_t *p, const weap_projectile *def, const float normal[3], int hit_obj)
{
	game_event_t *e;
	e = push(g, GE_DETONATE, def->detonation_effect, p->pos, normal);
	(void)e;
	run_effect_damage(g, def->detonation_effect, p->pos, hit_obj);
	if (def->super_detonation) run_effect_damage(g, def->super_detonation, p->pos, hit_obj);
	proj_free(p);
}

/* Segment a->b against the player's capsule (feet centre + radius .. + height - radius, radius `pr` plus the projectile's). Returns the
 * fraction of the segment at the first contact, or 2 for none (sampled: capsule and segment are tiny compared with a tick's flight). */
static float segment_vs_player(const game_t *g, const float a[3], const float b[3], float pr)
{
	float lo = g->player.pos[2] + g->pp.radius, hi = g->player.pos[2] + (g->player.crouch > 0.5f ? g->pp.crouch_height : g->pp.height) - g->pp.radius;
	float r = g->pp.radius + pr, d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, dl2 = dot3(d, d), t = 0.0f;
	int step, steps = dl2 > 4.0f ? 32 : 8;
	if (hi < lo) hi = lo;
	for (step = 0; step <= steps; step++) {
		float p[3], cz, dx, dy, dz;
		t = (float)step / (float)steps;
		p[0] = a[0] + d[0] * t; p[1] = a[1] + d[1] * t; p[2] = a[2] + d[2] * t;
		cz = p[2] < lo ? lo : p[2] > hi ? hi : p[2];
		dx = p[0] - g->player.pos[0]; dy = p[1] - g->player.pos[1]; dz = p[2] - cz;
		if (dx * dx + dy * dy + dz * dz <= r * r) return t;
	}
	return 2.0f;
}

static void proj_step(game_t *g, game_proj_t *p)
{
	weap_projectile def;
	float a[3], b[3], hp[3], t_world = 2.0f, t_obj = 2.0f, normal[3] = { 0, 0, 1 };
	phys_hit wh;
	cmod_hit oh;
	int obj = -1, hit_world = 0, i;
	uint16_t mat = COLL_NO_MATERIAL;
	if (!weap_get_projectile(g->weap, p->proj, &def)) { proj_free(p); return; }
	/* speed profile and gravity */
	if (!p->grenade && def.initial_velocity > 0.0f) {
		float s = weap_projectile_speed(&def, p->age, p->life), vl = len3(p->vel);
		if (vl > 1e-6f) { p->vel[0] *= s / vl; p->vel[1] *= s / vl; p->vel[2] *= s / vl; }
	}
	{
		float grav = def.air_gravity != 0.0f ? def.air_gravity : (p->grenade ? 1.0f : 0.0f);
		p->vel[2] -= grav * g->globals.gravity;
	}
	a[0] = p->pos[0]; a[1] = p->pos[1]; a[2] = p->pos[2];
	for (i = 0; i < 3; i++) b[i] = a[i] + p->vel[i];
	if (g->has_phys && g->phys.ray(g->phys.user, a, b, &wh)) { hit_world = 1; t_world = wh.t; mat = wh.material; }
	obj = ray_objects(g, a, b, p->owner, &oh, &t_obj);
	if (obj >= 0 && (!hit_world || t_obj <= t_world)) hit_world = 0;
	else obj = -1;
	if (p->owner >= 0 && !vitals_dead(&g->vitals) && !(p->owner < (int)OBJ_MAX && g->unit_state[p->owner].ally)) {   /* shots of hostile AI units can hit the player */
		float tp = segment_vs_player(g, a, b, def.collision_radius > 0.0f ? def.collision_radius : 0.02f);
		float tbest = obj >= 0 ? t_obj : (hit_world ? t_world : 2.0f);
		if (tp <= 1.0f && tp <= tbest) {
			int di = def.impact_damage ? weap_find_damage(g->weap, def.impact_damage) : -1;
			weap_damage d;
			weap_material_response resp;
			unsigned mt = g->vitals.shield > 0.0f ? g->biped.shield_material : g->biped.body_material;
			float hp[3], amount = 0.0f, diff = g->unit ? unitdef_difficulty(g->unit, UNITDEF_D_ENEMY_DAMAGE, (unsigned)g->difficulty) : 1.0f;
			for (i = 0; i < 3; i++) hp[i] = a[i] + (b[i] - a[i]) * tp;
			if (diff <= 0.0f) diff = 1.0f;
			if (p->owner < (int)OBJ_MAX && g->unit_state[p->owner].dmg_scale > 0.0f) diff = g->unit_state[p->owner].dmg_scale;   /* units.c: difficulty x variant */
			p->pos[0] = hp[0]; p->pos[1] = hp[1]; p->pos[2] = hp[2];
			if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) { amount = weap_damage_at_range(&def, &d, p->dist, rnd01(g)) * diff; camera_shake_from(g, &d); }
			if (amount > 0.0f) apply_damage_to_player(g, amount, hp);
			{ float nrm[3] = { -p->vel[0], -p->vel[1], -p->vel[2] }; float nl = len3(nrm); if (nl > 0) { nrm[0] /= nl; nrm[1] /= nl; nrm[2] /= nl; }
			  if (weap_get_material(g->weap, &def, mt, &resp) && resp.response == WEAP_RESP_DETONATE) { detonate(g, p, &def, nrm, -1); return; } }
			proj_free(p);
			return;
		}
	}
	if (hit_world || obj >= 0) {
		float t = obj >= 0 ? t_obj : t_world;
		const weap_material_response *rp = NULL;
		weap_material_response resp;
		game_event_t *e;
		unsigned material_type = 0;
		for (i = 0; i < 3; i++) hp[i] = a[i] + (b[i] - a[i]) * t;
		if (obj >= 0) {
			cmod_model m;
			const game_unit_t *u = unit_of(g, obj);
			normal[0] = -p->vel[0]; normal[1] = -p->vel[1]; normal[2] = -p->vel[2];
			{ float nl = len3(normal); if (nl > 0) { normal[0] /= nl; normal[1] /= nl; normal[2] /= nl; } }
			if (u && cmod_get_model(g->cmod, (uint32_t)u->model, &m)) {
				cmod_material cm = cmod_get_material(&m, oh.material);
				material_type = cm.material_type;
				if (u->has_vitals && u->vitals.shield > 0.0f) material_type = m.shield_material;
			}
		} else {
			normal[0] = wh.normal[0]; normal[1] = wh.normal[1]; normal[2] = wh.normal[2];
			material_type = mat == COLL_NO_MATERIAL ? 0u : mat;
		}
		if (weap_get_material(g->weap, &def, material_type, &resp)) rp = &resp;
		p->pos[0] = hp[0]; p->pos[1] = hp[1]; p->pos[2] = hp[2];
		p->dist += len3(p->vel) * t;
		e = push(g, GE_IMPACT, rp ? rp->effect : 0, hp, normal);
		if (e) { e->material = (uint8_t)material_type; e->obj = (int16_t)obj; }
		/* direct damage of the projectile */
		if (def.impact_damage) {
			int di = weap_find_damage(g->weap, def.impact_damage);
			weap_damage d;
			if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) {
				float amount = weap_damage_at_range(&def, &d, p->dist, rnd01(g));
				camera_shake_from(g, &d);
				if (obj >= 0 && amount > 0.0f) {
					float dealt;
					if (p->owner >= 0 && p->owner < (int)OBJ_MAX && g->unit_state[p->owner].dmg_scale > 0.0f) amount *= g->unit_state[p->owner].dmg_scale;
					if (d.modifiers[material_type < WEAP_MAX_MATERIALS ? material_type : 0] > 0.0f) amount *= d.modifiers[material_type < WEAP_MAX_MATERIALS ? material_type : 0];
					dealt = game_damage_object(g, obj, amount, &oh);
					if (dealt > 0.0f) g->st_hits++;
				}
				if (obj < 0) g->st_world_hits++;
			}
		}
		if (obj >= 0 && !rp) { proj_free(p); return; }
		switch (rp ? rp->response : WEAP_RESP_DISAPPEAR) {
		case WEAP_RESP_DETONATE: detonate(g, p, &def, normal, obj); return;
		case WEAP_RESP_REFLECT: {
			float vn = dot3(p->vel, normal);
			float fr = 1.0f - (rp->perpendicular_friction > 0.0f ? rp->perpendicular_friction : 0.0f);
			p->vel[0] -= 2.0f * vn * normal[0]; p->vel[1] -= 2.0f * vn * normal[1]; p->vel[2] -= 2.0f * vn * normal[2];
			p->vel[0] *= fr; p->vel[1] *= fr; p->vel[2] *= fr;
			p->pos[0] += normal[0] * 0.01f; p->pos[1] += normal[1] * 0.01f; p->pos[2] += normal[2] * 0.01f;
			if (++p->bounces > 6) { if (def.detonation_effect) detonate(g, p, &def, normal, obj); else proj_free(p); }
			return;
		}
		case WEAP_RESP_OVERPENETRATE:
			p->pos[0] += p->vel[0] * 0.02f; p->pos[1] += p->vel[1] * 0.02f; p->pos[2] += p->vel[2] * 0.02f;
			return;
		case WEAP_RESP_ATTACH:
			p->vel[0] = p->vel[1] = p->vel[2] = 0.0f; p->grenade = 1;
			p->age++;
			if (p->age >= p->life) detonate(g, p, &def, normal, obj);
			return;
		default: proj_free(p); return;
		}
	}
	p->pos[0] = b[0]; p->pos[1] = b[1]; p->pos[2] = b[2];
	p->dist += len3(p->vel);
	p->age++;
	if (p->age >= p->life) {
		if (def.detonation_effect || def.timer[0] > 0.0f) detonate(g, p, &def, normal, -1);
		else proj_free(p);
	}
}

/* ---- init ---------------------------------------------------------------------------------------------------------------- */
void game_init(game_t *g, const game_init_t *in, const float pos[3], float yaw)
{
	int i;
	memset(g, 0, sizeof *g);
	g->unit = in->unit; g->weap = in->weap; g->cmod = in->cmod; g->ansm = in->ansm; g->fx = in->fx; g->coll = in->coll;
	g->ow = in->ow; g->objects = in->objects; g->fp = in->fp;
	g->rng = in->seed ? in->seed : 12345u;
	g->cur = -1; g->switch_to = -1; g->fp_action = -1; g->difficulty = 1; g->autoaim = 1; g->nearby_weapon = -1; g->player_obj = -1;
	if (g->coll) { phys_world_from_coll(&g->phys, g->coll); g->has_phys = 1; }
	if (g->unit) {
		unitdef_globals(g->unit, &g->globals);
		g->has_biped = unitdef_player_biped(g->unit, &g->biped);
	}
	player_params_from_tags(&g->pp, g->unit ? &g->globals : NULL, g->has_biped ? &g->biped : NULL);
	player_init(&g->player, pos, yaw);
	if (g->has_biped)
		vitals_init(&g->vitals, g->biped.health > 0 ? g->biped.health : 75.0f, g->biped.shield,
		            g->biped.shield_recharge_rate, (uint16_t)(g->biped.shield_stun_time * TICKS));
	else vitals_init(&g->vitals, 75.0f, 75.0f, 1.0f / 120.0f, 120);
	unit_init_all(g);
	for (i = 0; i < GAME_SLOTS; i++) g->slot[i].weapon = -1;
	if (g->has_biped) {
		for (i = 0; i < g->biped.weapon_count && i < GAME_SLOTS; i++) game_give_weapon(g, i, g->biped.weapon_tags[i]);
		g->grenades[0] = 2; g->grenades[1] = 2;
	}
}

/* ---- the tick ------------------------------------------------------------------------------------------------------------ */
/* Aim assist (weapon autoaim cone and range): bends the fire direction towards the live, visible unit nearest to the crosshair. */
static void autoaim(game_t *g, const weap_weapon *w, const float eye[3], const float fwd[3], float out[3])
{
	unsigned i;
	float best = w->autoaim_angle > 0.0f ? w->autoaim_angle : 0.0f, bd[3] = { 0, 0, 0 };
	int found = 0;
	out[0] = fwd[0]; out[1] = fwd[1]; out[2] = fwd[2];
	if (best <= 0.0f || !g->objects) return;
	for (i = 0; i < g->objects->count; i++) {
		const object_t *o = &g->objects->obj[i];
		const game_unit_t *u = &g->unit_state[i];
		float d[3], dist, cosang, ang;
		phys_hit h;
		if (!u->has_vitals || u->dead || OBJ_OFF(o)) continue;
		d[0] = o->pos[0] - eye[0]; d[1] = o->pos[1] - eye[1]; d[2] = o->pos[2] + 0.35f - eye[2];
		dist = len3(d);
		if (dist < 0.3f || (w->autoaim_range > 0.0f && dist > w->autoaim_range)) continue;
		cosang = dot3(d, fwd) / dist;
		if (cosang < 0.0f) continue;
		ang = acosf(cosang > 1.0f ? 1.0f : cosang);
		if (ang >= best) continue;
		if (g->has_phys) {
			float t[3] = { o->pos[0], o->pos[1], o->pos[2] + 0.35f };
			if (g->phys.ray(g->phys.user, eye, t, &h)) continue;      /* something in the way */
		}
		best = ang; bd[0] = d[0] / dist; bd[1] = d[1] / dist; bd[2] = d[2] / dist; found = 1;
	}
	if (found) {
		float k = 0.75f, l;
		out[0] = fwd[0] + (bd[0] - fwd[0]) * k; out[1] = fwd[1] + (bd[1] - fwd[1]) * k; out[2] = fwd[2] + (bd[2] - fwd[2]) * k;
		l = len3(out);
		if (l > 1e-6f) { out[0] /= l; out[1] /= l; out[2] /= l; }
	}
}

static void fire_shots(game_t *g, game_slot_t *s, const weap_events *wev)
{
	unsigned t, n, k;
	float eye[3], fwd[3], aim[3];
	game_eye(g, eye, fwd);
	if (g->autoaim) autoaim(g, &s->def, eye, fwd, aim); else { aim[0] = fwd[0]; aim[1] = fwd[1]; aim[2] = fwd[2]; }
	for (t = 0; t < s->def.trigger_count; t++) {
		const weap_trigger *tr = &s->def.trigger[t];
		if (!wev->shot[t]) continue;
		g->st_shots++;
		{
			uint32_t shots = s->st.shots > 0 ? s->st.shots - 1 : 0;
			game_event_t *e = push(g, GE_FIRE, s->def.tag_id, eye, fwd);
			if (e) e->arg = (uint16_t)t;
			for (k = 0; k < tr->fx_count; k++) {
				weap_fx fx;
				if (!weap_get_fx(g->weap, tr, k, &fx)) continue;
				(void)shots;                               /* the shot-count bounds (1..1 on every retail weapon) do not restrict the first entry */
				if (k > 0 && fx.shots_lo > 1) continue;
				if (fx.effects[0]) { game_event_t *e2 = push(g, GE_FIRE, fx.effects[0], eye, fwd); if (e2) { e2->arg = (uint16_t)t; e2->material = (fx.sound_mask & 1u) ? 1 : 0; e2->value = 1.0f; } }
				if (fx.damages[0]) {
					int di = weap_find_damage(g->weap, fx.damages[0]);
					weap_damage d;
					if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) camera_shake_from(g, &d);
				}
				break;
			}
		}
		if (!tr->projectile) continue;
		{
			int pi = weap_find_projectile(g->weap, tr->projectile);
			if (pi < 0) continue;
			n = wev->projectiles[t] ? wev->projectiles[t] : 1;
			for (k = 0; k < n; k++) {
				float dir[3], r[3], u[3], ang = wev->error[t], rr = sqrtf(rnd01(g)) * ang, th = rnd01(g) * 6.2831853f;
				float cy = cosf(g->player.yaw), sy = sinf(g->player.yaw), sp = sinf(g->player.pitch), cp = cosf(g->player.pitch);
				float muzzle[3];
				r[0] = sy; r[1] = -cy; r[2] = 0.0f;
				u[0] = -sp * cy; u[1] = -sp * sy; u[2] = cp;
				dir[0] = aim[0] + (r[0] * cosf(th) + u[0] * sinf(th)) * rr;
				dir[1] = aim[1] + (r[1] * cosf(th) + u[1] * sinf(th)) * rr;
				dir[2] = aim[2] + (r[2] * cosf(th) + u[2] * sinf(th)) * rr;
				muzzle[0] = eye[0] + fwd[0] * 0.12f; muzzle[1] = eye[1] + fwd[1] * 0.12f; muzzle[2] = eye[2] + fwd[2] * 0.12f - 0.02f;
				proj_spawn(g, (unsigned)pi, muzzle, dir, 0.0f, 0);
			}
		}
	}
}

static void melee_strike(game_t *g, const weap_weapon *w)
{
	float eye[3], fwd[3], b[3];
	cmod_hit oh;
	float t_obj = 2.0f, t_world = 2.0f;
	phys_hit wh;
	int obj, i, hw = 0;
	game_eye(g, eye, fwd);
	for (i = 0; i < 3; i++) b[i] = eye[i] + fwd[i] * MELEE_RANGE;
	if (g->has_phys && g->phys.ray(g->phys.user, eye, b, &wh)) { hw = 1; t_world = wh.t; }
	obj = ray_objects(g, eye, b, -1, &oh, &t_obj);
	if (obj >= 0 && (!hw || t_obj <= t_world)) {
		int di = weap_find_damage(g->weap, w->melee_damage);
		weap_damage d;
		float hp[3];
		for (i = 0; i < 3; i++) hp[i] = eye[i] + (b[i] - eye[i]) * t_obj;
		if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) {
			float amount = d.damage_upper[0] + (d.damage_upper[1] - d.damage_upper[0]) * rnd01(g);
			if (amount < d.damage_lower) amount = d.damage_lower;
			if (game_damage_object(g, obj, amount, &oh) > 0.0f) g->st_hits++;
			push(g, GE_DAMAGE, w->melee_damage, hp, NULL);
		}
		{ game_event_t *e = push(g, GE_IMPACT, w->melee_response, hp, NULL); if (e) e->obj = (int16_t)obj; }
	} else if (hw) {
		float hp[3];
		for (i = 0; i < 3; i++) hp[i] = eye[i] + (b[i] - eye[i]) * t_world;
		push(g, GE_IMPACT, w->melee_response, hp, wh.normal);
	}
}

static int find_pickup(const game_t *g)
{
	unsigned i;
	int best = -1;
	float bestd = PICKUP_RANGE;
	for (i = 0; g->objects && g->weap && i < g->objects->count; i++) {
		const object_t *o = &g->objects->obj[i];
		float d[3] = { o->pos[0] - g->player.pos[0], o->pos[1] - g->player.pos[1], o->pos[2] - g->player.pos[2] }, dist;
		if (o->type != SCEN_WEAPON || OBJ_OFF(o) || (o->flags & OBJ_F_HELD)) continue;
		dist = len3(d);
		if (dist < bestd && weap_find_weapon(g->weap, object_tag(g, o)) >= 0) { bestd = dist; best = (int)i; }
	}
	return best;
}

static void switch_weapons(game_t *g, const game_input_t *in)
{
	if (!(in->pressed & IN_SWITCH) || g->switch_to >= 0) return;
	/* pick up a weapon lying nearby (replaces the current one) */
	{
		int best = find_pickup(g);
		if (best >= 0) {
			object_t *o = &g->objects->obj[best];
			uint32_t tag = object_tag(g, o);
			int slot = g->cur >= 0 ? g->cur : (g->slot[0].weapon < 0 ? 0 : 1);
			o->model = -1;                                       /* taken: no longer drawn */
			push(g, GE_PICKUP, tag, o->pos, NULL);
			game_give_weapon(g, slot, tag);
			return;
		}
	}
	if (g->all_weapons && g->weap && g->weap->n_weapons) {
		g->cycle_index = (g->cycle_index + 1) % (int)g->weap->n_weapons;
		g->switch_to = g->cur >= 0 ? g->cur : 0;
		g->slot[g->switch_to].weapon = -2;                       /* marker: load the cycled weapon after putting away */
	} else if (g->cur >= 0) {
		int other = 1 - g->cur;
		if (g->slot[other].weapon >= 0) g->switch_to = other;
	}
}

void game_tick(game_t *g, const game_input_t *in)
{
	game_slot_t *s;
	const weap_weapon *w;
	g->ev_count = 0;
	g->tick++;

	if (vitals_dead(&g->vitals)) g->dead_ticks++;
	/* player */
	{
		player_cmd_t cmd = in->move;
		float speed0 = len3(g->player.vel);
		(void)speed0;
		if (vitals_dead(&g->vitals)) memset(&cmd, 0, sizeof cmd);
		g->player.landed_speed = 0.0f;
		if (!g->player_frozen) player_tick(&g->player, &g->pp, g->has_phys ? &g->phys : &(phys_world){ 0 }, &cmd, 1.0f / TICKS);
		if (g->player.landed_speed > 0.0f) {
			game_event_t *e = push(g, GE_LAND, 0, g->player.pos, NULL);
			if (e) e->value = g->player.landed_speed;
			/* falling damage: harmful above the globals' velocity (wu/tick) */
			{
				float v = g->player.landed_speed / TICKS, lo = g->globals.fall_velocity_harm_min, hi = g->globals.fall_velocity_harm_max;
				if (hi > lo && v > lo && g->globals.damage_tags[0]) {
					int di = weap_find_damage(g->weap, g->globals.damage_tags[0]);
					weap_damage d;
					if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) {
						float f = (v - lo) / (hi - lo);
						if (f > 1.0f) f = 1.0f;
						apply_damage_to_player(g, d.damage_lower + (d.damage_upper[1] - d.damage_lower) * f, g->player.pos);
					}
				}
			}
		}
	}
	{
		int was_down = (g->vitals.flags & VITALS_F_SHIELD_DOWN) != 0;
		vitals_tick(&g->vitals);
		if (was_down && !(g->vitals.flags & VITALS_F_SHIELD_DOWN)) push(g, GE_SHIELD_UP, 0, NULL, NULL);
	}
	if (g->hurt_flash > 0.0f) g->hurt_flash -= 0.1f;
	if (g->shake > 0.0f) { g->shake *= 0.85f; if (g->shake < 0.0005f) g->shake = 0.0f; }

	/* weapons */
	if (!vitals_dead(&g->vitals)) {
		g->nearby_weapon = find_pickup(g);
		switch_weapons(g, in);
		s = g->cur >= 0 ? &g->slot[g->cur] : NULL;
		if (s && s->weapon >= 0) {
			weap_input wi;
			weap_events wev;
			memset(&wi, 0, sizeof wi);
			w = &s->def;
			wi.fire[0] = (in->held & IN_FIRE) && !g->throwing;
			wi.reload = (in->pressed & IN_RELOAD) != 0;
			wi.melee = (in->pressed & IN_MELEE) != 0;
			wi.zoom = (in->pressed & IN_ZOOM) != 0;
			wi.switch_out = g->switch_to >= 0;
			weapon_fire_step(&s->st, w, &wi, &wev);
			if (wev.flags & WEAP_EV_ZOOM) { g->zoomed = s->st.zoom; push(g, GE_ZOOM, w->zoom_in_sound, NULL, NULL); }
			if (wev.flags & WEAP_EV_READY) { push(g, GE_READY, w->ready_effect, NULL, NULL); }
			if (wev.flags & WEAP_EV_DRY_FIRE) {
				weap_fx fx;
				if (weap_get_fx(g->weap, &w->trigger[0], 0, &fx) && fx.effects[2]) { game_event_t *e = push(g, GE_EMPTY, fx.effects[2], NULL, NULL); if (e) e->material = (fx.sound_mask & 4u) ? 1 : 0; }
				else push(g, GE_EMPTY, 0, NULL, NULL);
			}
			if (wev.flags & WEAP_EV_RELOAD_START) {
				g->st_reloads++;
				fp_play(g, s->st.loaded[s->st.reload_mag] == 0 ? ANSM_FP_RELOAD_EMPTY : ANSM_FP_RELOAD_FULL);
				if (!g->fp_timer) fp_play(g, ANSM_FP_RELOAD_FULL);
				push(g, GE_RELOAD, w->magazine[s->st.reload_mag].reload_effect, NULL, NULL);
			}
			if (wev.flags & WEAP_EV_OVERHEATED) { fp_play(g, ANSM_FP_OVERHEATED); push(g, GE_OVERHEAT, w->overheated_effect, NULL, NULL); }
			if (wev.flags & WEAP_EV_MELEE_START) { g->st_melee++; fp_play(g, ANSM_FP_MELEE); push(g, GE_MELEE, 0, NULL, NULL); }
			if (wev.flags & WEAP_EV_MELEE_HIT) melee_strike(g, w);
			{
				unsigned t, any = 0;
				for (t = 0; t < w->trigger_count; t++) any |= wev.shot[t];
				if (any) {
					fire_shots(g, s, &wev);
					fp_play(g, ANSM_FP_FIRE_1);
				}
			}
			if (wev.flags & WEAP_EV_PUT_AWAY) {
				int to = g->switch_to;
				g->switch_to = -1;
				if (to >= 0) {
					if (g->slot[to].weapon == -2 || (g->all_weapons && g->cycle_index >= 0 && to == g->cur)) {
						uint32_t tag;
						weap_weapon tmp;
						weap_get_weapon(g->weap, (uint32_t)g->cycle_index, &tmp);
						tag = tmp.tag_id;
						g->slot[to].weapon = -1;
						g->cur = to;
						game_give_weapon(g, to, tag);
					} else {
						g->cur = to;
						g->slot[to].st.mode = WEAP_MODE_READYING;
						g->slot[to].st.timer = weap_ready_ticks(&g->slot[to].def);
						fp_apply_slot(g);
						fp_play(g, ANSM_FP_READY);
					}
					push(g, GE_SWITCH, g->slot[g->cur].def.tag_id, NULL, NULL);
				}
			}
		}
		/* grenades */
		if (g->throwing > 0) {
			if (--g->throwing == 0) {
				weap_grenade gr;
				if (weap_get_grenade(g->weap, (uint32_t)g->grenade_type, &gr) && gr.projectile) {
					int pi = weap_find_projectile(g->weap, gr.projectile);
					if (pi >= 0) {
						float eye[3], fwd[3], dir[3], sp = g->has_biped && g->biped.grenade_velocity > 0.0f ? g->biped.grenade_velocity : 10.0f;
						game_eye(g, eye, fwd);
						dir[0] = fwd[0]; dir[1] = fwd[1]; dir[2] = fwd[2] + 0.25f;
						eye[0] += fwd[0] * 0.3f; eye[1] += fwd[1] * 0.3f;
						proj_spawn(g, (unsigned)pi, eye, dir, sp / TICKS, 1);
						g->st_throws++;
						{ game_event_t *e = push(g, GE_THROW, gr.throw_effect, eye, fwd); if (e) e->arg = (uint16_t)g->grenade_type; }
					}
				}
			}
		} else if ((in->pressed & IN_GRENADE) && g->grenades[g->grenade_type] > 0 && g->cur >= 0 && g->slot[g->cur].st.mode == WEAP_MODE_IDLE) {
			g->grenades[g->grenade_type]--;
			g->throwing = 12;
			fp_play(g, ANSM_FP_THROW_GRENADE);
			if (g->fp_timer > 3) g->throwing = g->fp_timer / 2;
		}
		if ((in->pressed & IN_SWAPGREN)) g->grenade_type ^= 1;
	}

	/* first-person animation: return to idle / moving when a one-shot animation ends */
	if (g->fp_timer > 0) { if (--g->fp_timer == 0) { g->fp_action = -1; fp_set(g, g->cur >= 0 ? g->slot[g->cur].fp_anim_idle : -1); } }
	else if (g->cur >= 0 && g->fp_action < 0) {
		/* walking sway animation when moving on the ground */
		const game_slot_t *cs = &g->slot[g->cur];
		float sp = sqrtf(g->player.vel[0] * g->player.vel[0] + g->player.vel[1] * g->player.vel[1]);
		int mv = fp_anim_for(g, cs, ANSM_FP_MOVING), idle = cs->fp_anim_idle;
		if (mv >= 0 && cs->fp_graph >= 0 && g->ow.anims) {                 /* 'moving' is an OVERLAY in the data: not a base pose (overlay blending: later) */
			anim_graph ag = anim_get_graph(g->ow.anims, (uint32_t)cs->fp_graph);
			if (anim_get(g->ow.anims, &ag, (uint32_t)mv).type != 0) mv = -1;
		}
		int want = (sp > 0.4f && g->player.on_ground && mv >= 0) ? mv : idle;
		if (g->fp && g->fp->active && want >= 0 && g->fp->arms_anim != (unsigned)want) fp_set(g, want);
	}

	/* projectiles */
	{
		unsigned i;
		for (i = 0; i < GAME_MAX_PROJ; i++) if (g->proj[i].active) proj_step(g, &g->proj[i]);
	}
	/* units: shields recharge, corpses */
	{
		unsigned i;
		for (i = 0; g->objects && i < g->objects->count; i++) {
			game_unit_t *u = &g->unit_state[i];
			if (u->has_vitals && !u->dead && !OBJ_OFF(&g->objects->obj[i])) vitals_tick(&u->vitals);
			if (u->dead && u->death_ticks < 65000) u->death_ticks++;
		}
	}
	g->prev_held = in->held;
}

void game_camera(const game_t *g, camera_t *cam)
{
	float eye[3];
	const weap_weapon *w = cur_def(g);
	player_eye(&g->player, &g->pp, eye);
	cam->pos[0] = eye[0]; cam->pos[1] = eye[1]; cam->pos[2] = eye[2];
	cam->yaw = g->player.yaw; cam->pitch = g->player.pitch; cam->roll = 0.0f;
	if (g->shake > 0.0f) {
		float t = (float)g->tick;
		cam->pitch += g->shake * sinf(t * 2.9f);
		cam->yaw += g->shake * sinf(t * 3.7f + 1.0f);
	}
	cam->hfov = (g->has_biped && g->biped.camera_fov > 0.1f) ? g->biped.camera_fov : 70.0f * 3.14159265f / 180.0f;
	if (w && g->zoomed && w->zoom_levels) {
		float mag = w->zoom_min + (w->zoom_max - w->zoom_min) * (w->zoom_levels > 1 ? (float)(g->zoomed - 1) / (float)(w->zoom_levels - 1) : 0.0f);
		if (mag < 1.0f) mag = 1.0f;
		cam->hfov = 2.0f * atanf(tanf(cam->hfov * 0.5f) / mag);
	}
}
