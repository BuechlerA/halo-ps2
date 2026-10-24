/* Units (sim/units.h, docs/31-units.md): bipeds that are not the player. Locomotion reuses the player's capsule controller (player_tick) with a
 * player_cmd_t built from the unit's control; weapons are the Phase 3 weapon state machine, shots go through game_fire_projectile; health is
 * the game's vitals of the object slot; animation is chosen from the ASTB tables and written into the object's anim / frame. Pure C11, no heap,
 * no double. */
#include "sim/units.h"
#include "sim/playerdef.h"
#include <math.h>
#include <string.h>

#define TICKS 30.0f
#define DT (1.0f / 30.0f)
#define PI_F 3.14159265f
#define CINE_COUNT_RAD 0.00314159265f      /* one recorded aim count (pi / 1000) in radians */
#define AIM_YAW_LIMIT 1.2217305f           /* 70 degrees: the aiming screen of the animation graphs (docs/19 section 1.4) */
#define AIM_PITCH_LIMIT 0.7853982f         /* 45 degrees */
#define MELEE_RANGE 1.1f                   /* centre to centre, horizontal (the player's blow reaches 1.1 from the eye) */
#define MELEE_CONE_COS 0.5f                /* +-60 degrees in front of the body */
#define MELEE_HEIGHT 0.75f
#define MOVING_SPEED 0.25f                 /* wu/s above which a unit counts as moving (animation, UA_MOVE) */
int g_units_turn_gate = 1;               /* debug (cfg turngate 0): scripted units turn their bodies to every aim change, as before ADR-086 */
int g_units_look = 1;                    /* debug (cfg lookoverlay 0): no head / arm overlays */
#define TURN_DONE_ANGLE 0.05f              /* radians: a threshold turn ends when the body is this close to the aim */
#define TURN_ANIM_ANGLE 0.12f              /* radians the body must still have to turn before the turn animation plays */
#define FLINCH_FRACTION 0.12f              /* a hit takes this fraction of the maximum health for a flinch */
#define FLINCH_COOLDOWN 120                /* ticks between two flinches */
#define FLINCH_MAX_TICKS 40
#define FIRE_HOLD_DEFAULT 6
#define DEFAULT_MELEE_TICKS 20
#define DEFAULT_THROW_TICKS 20
#define HOLD_TICKS 0xFFFFu                 /* action_ticks of a held custom animation */
#define SETTLE_PERIOD 8                    /* a settled unit re-checks its ground every this many ticks */
#define DROP_RESERVE 48                    /* object slots kept free of dropped weapons */

enum { K_IDLE = 1, K_MOVE, K_TURN, K_AIR, K_LAND, K_RELOAD, K_MELEE, K_THROW, K_FLINCH };

/* ---- small helpers ---------------------------------------------------------------------------------------------------- */
static float wrap_pi(float a)
{
	if (!(a == a)) return 0.0f;
	while (a > PI_F) a -= 2.0f * PI_F;
	while (a < -PI_F) a += 2.0f * PI_F;
	return a;
}

static float finite_or(float v, float d) { return (v == v && v < 1e9f && v > -1e9f) ? v : d; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float rnd01(unit_t *u)
{
	uint32_t x = u->rng ? u->rng : 0x9E3779B9u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	u->rng = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static unit_t *U(units_t *s, int unit) { return (s && unit >= 0 && unit < UNIT_MAX && s->u[unit].active) ? &s->u[unit] : NULL; }
static const unit_t *CU(const units_t *s, int unit) { return (s && unit >= 0 && unit < UNIT_MAX && s->u[unit].active) ? &s->u[unit] : NULL; }

static object_t *OBJ(const game_t *g, const unit_t *u)
{
	if (!g->objects || u->obj < 0 || (unsigned)u->obj >= g->objects->count) return NULL;
	return (g->objects->obj[u->obj].flags & OBJ_F_FREE) ? NULL : &g->objects->obj[u->obj];
}

static int get_biped(const game_t *g, const unit_t *u, unitdef_biped_def *b) { return g->unit && unitdef_get_biped(g->unit, (uint32_t)u->biped, b); }

static unsigned diff_level(const game_t *g) { return g->difficulty < 0 ? 0u : g->difficulty > 3 ? 3u : (unsigned)g->difficulty; }
static float diff_value(const game_t *g, unsigned group, float def)
{
	float v;
	if (!g->unit) return def;
	v = unitdef_difficulty(g->unit, group, diff_level(g));
	return v > 0.0f ? v : def;
}

/* ---- teams ---------------------------------------------------------------------------------------------------------------- */
int units_teams_hostile(int a, int b)
{
	if (a <= 0 || b <= 0 || a == b || a > 5 || b > 5) return 0;
	if (a > b) { int t = a; a = b; b = t; }
	switch (a) {
	case 1: return b == 3 || b == 4 || b == 5;          /* player: covenant, flood, sentinels */
	case 2: return b == 3 || b == 4 || b == 5;          /* human */
	case 3: return b == 4;                              /* covenant: flood (sentinels are neutral to them) */
	case 4: return b == 5;                              /* flood: sentinels */
	default: return 0;
	}
}

static int is_ally_team(int t) { return t == 1 || t == 2; }

static void emit(units_t *s, unit_event_type type, const unit_t *u, unsigned arg, uint32_t tag, const float pos[3], const float dir[3])
{
	unit_event_t *e;
	if (s->ev_count >= UNIT_EVENTS) return;
	e = &s->ev[s->ev_count++];
	memset(e, 0, sizeof *e);
	e->type = (uint8_t)type; e->arg = (uint8_t)arg; e->obj = u->obj; e->tag = tag;
	if (pos) { e->pos[0] = pos[0]; e->pos[1] = pos[1]; e->pos[2] = pos[2]; }
	if (dir) { e->dir[0] = dir[0]; e->dir[1] = dir[1]; e->dir[2] = dir[2]; }
}

const unit_event_t *units_events(const units_t *s, unsigned *count) { *count = s->ev_count; return s->ev; }

/* ---- weapons -------------------------------------------------------------------------------------------------------------- */
static int weapon_decode(const game_t *g, int widx, weap_weapon *w) { return g->weap && widx >= 0 && weap_get_weapon(g->weap, (uint32_t)widx, w); }

/* ---- animation lookup ------------------------------------------------------------------------------------------------------- */
static int get_graph(const game_t *g, const unit_t *u, ansm_graph *gr) { return g->ansm && u->anim_tag && ansm_find_graph(g->ansm, u->anim_tag, gr) == 0; }

/* The class block of unit block `ui` that holds the weapon's animations. A weapon names its *type* ("ar", "pp"), and only the `stand` block (and `crouch`) lists type blocks; the
 * `alert` block has class level animations only (`alert rifle idle%0`), so a miss there is answered with the class the `stand` block uses for the weapon, found again by name. -1: none. */
static int class_for_weapon(const ansm_section *a, const ansm_graph *gr, int ui, const char *wlabel)
{
	int ci = (wlabel && wlabel[0]) ? ansm_find_class_for_type(a, gr, (uint32_t)ui, wlabel) : -1;
	if (ci < 0 && wlabel && wlabel[0]) {
		int su = ansm_find_unit(a, gr, "stand"), sc;
		ansm_class info;
		if (su >= 0 && su != ui && (sc = ansm_find_class_for_type(a, gr, (uint32_t)su, wlabel)) >= 0 && ansm_class_info(a, gr, (uint32_t)su, (uint32_t)sc, &info) == 0 && info.name)
			ci = ansm_find_class(a, gr, (uint32_t)ui, info.name);
	}
	return ci;
}

static int pick_class(const ansm_section *a, const ansm_graph *gr, int ui, const char *wlabel)
{
	int ci = class_for_weapon(a, gr, ui, wlabel);
	ansm_unit info;
	if (ci < 0) ci = ansm_find_class(a, gr, (uint32_t)ui, "unarmed");
	if (ci < 0) ci = ansm_find_class(a, gr, (uint32_t)ui, "unuarmed");     /* misspelt in some graphs */
	if (ci < 0 && ansm_unit_info(a, gr, (uint32_t)ui, &info) == 0 && info.class_count > 0) ci = 0;
	return ci;
}

/* animation of `action` in the block of unit label `ulabel` with the weapon of type label `wlabel` */
static int block_slot(const ansm_section *a, const ansm_graph *gr, const char *ulabel, const char *wlabel, unsigned action, int *class_out)
{
	int ui = ansm_find_unit(a, gr, ulabel), ci, ti;
	unsigned lvl = ANSM_ACTION_LEVEL(action), slot = ANSM_ACTION_SLOT(action);
	if (ui < 0) return -1;
	if (lvl == ANSM_L_UNIT) return ansm_unit_slot(a, gr, (uint32_t)ui, slot);
	if (wlabel && wlabel[0] && strcmp(ulabel, "stand") != 0 && class_for_weapon(a, gr, ui, wlabel) < 0) return -1;    /* an armed unit never takes another weapon's (the pistol's) animations from a posture block: the caller falls back to `stand` */
	ci = pick_class(a, gr, ui, wlabel);
	if (class_out) *class_out = ci;
	if (ci < 0) return -1;
	if (lvl == ANSM_L_CLASS) return ansm_class_slot(a, gr, (uint32_t)ui, (uint32_t)ci, slot);
	ti = (wlabel && wlabel[0]) ? ansm_find_type(a, gr, (uint32_t)ui, (uint32_t)ci, wlabel) : -1;
	return ti < 0 ? -1 : ansm_type_slot(a, gr, (uint32_t)ui, (uint32_t)ci, (uint32_t)ti, slot);
}

/* posture block first, then the standing block */
static int find_action(const ansm_section *a, const ansm_graph *gr, unsigned posture, const char *wlabel, unsigned action, int *class_out)
{
	int k = block_slot(a, gr, posture == 2 ? "alert" : posture ? "crouch" : "stand", wlabel, action, class_out);
	if (k < 0 && posture) k = block_slot(a, gr, "stand", wlabel, action, class_out);
	return k;
}

static int is_overlay(const ansm_section *a, const ansm_graph *gr, int k)
{
	ansm_anim info;
	return k >= 0 && ansm_anim_info(a, gr, (uint32_t)k, &info) == 0 && info.type == ANSM_TYPE_OVERLAY;
}

static const char *weapon_label(const game_t *g, const unit_t *u, weap_weapon *w)
{
	if (u->weapon < 0 || !weapon_decode(g, u->weapon, w)) return NULL;
	return w->label;
}

/* The animation of a unit action; dir: UA_MOVE 0 front 1 back 2 left 3 right, UA_LAND 0 soft 1 hard, UA_FLINCH / UA_DEAD the damage table's
 * 0 front 1 left 2 right 3 back. `r` in [0, 1) picks among variants. */
static int lookup_action(const game_t *g, const unit_t *u, const ansm_graph *gr, unit_action act, unsigned dir, float r, int *overlay)
{
	const ansm_section *a = g->ansm;
	weap_weapon w;
	const char *wl = weapon_label(g, u, &w);
	int k = -1, cls = -1;
	unsigned posture = u->posture;
	if (overlay) *overlay = 0;
	switch (act) {
	case UA_IDLE: k = find_action(a, gr, posture, wl, ANSM_C_IDLE, &cls); break;
	case UA_MOVE: {
		static const unsigned mv[4] = { ANSM_C_MOVE_FRONT, ANSM_C_MOVE_BACK, ANSM_C_MOVE_LEFT, ANSM_C_MOVE_RIGHT };
		k = find_action(a, gr, posture, wl, mv[dir & 3u], &cls);
		if (k < 0) k = find_action(a, gr, posture, wl, ANSM_C_MOVE_FRONT, &cls);
		break;
	}
	case UA_AIRBORNE: k = find_action(a, gr, posture, wl, ANSM_C_AIRBORNE, &cls); break;
	case UA_LAND:
		k = find_action(a, gr, posture, wl, dir ? ANSM_C_LAND_HARD : ANSM_C_LAND_SOFT, &cls);
		if (k < 0) k = find_action(a, gr, posture, wl, dir ? ANSM_C_LAND_SOFT : ANSM_C_LAND_HARD, &cls);
		break;
	case UA_FIRE:
		k = find_action(a, gr, posture, wl, (u->ctl.flags & UC_FIRE2) && !(u->ctl.flags & UC_FIRE) ? ANSM_T_FIRE_2 : ANSM_T_FIRE_1, &cls);
		if (k < 0) k = find_action(a, gr, posture, wl, ANSM_T_FIRE_1, &cls);
		break;
	case UA_RELOAD: k = find_action(a, gr, posture, wl, ANSM_T_RELOAD_1, &cls); break;
	case UA_MELEE:
		k = find_action(a, gr, posture, wl, ANSM_T_MELEE, &cls);
		if (k < 0) k = find_action(a, gr, posture, wl, ANSM_C_MELEE, &cls);
		break;
	case UA_THROW: k = find_action(a, gr, posture, wl, ANSM_C_THROW_GRENADE, &cls); break;
	case UA_FLINCH: {
		/* health ping (damage table category 1) when it is a full-body animation, else the stunned class animations */
		int d = (int)(dir & 3u), p;
		p = ansm_damage_anim(a, gr, 1, (unsigned)d, 1);
		if (p < 0 || is_overlay(a, gr, p)) p = ansm_damage_anim(a, gr, 1, (unsigned)d, 0);
		if (p >= 0 && !is_overlay(a, gr, p)) { k = p; break; }
		k = find_action(a, gr, posture, wl, ANSM_C_STUNNED_FRONT, &cls);
		break;
	}
	case UA_DEAD:
	default: break;
	}
	if (k >= 0 && is_overlay(a, gr, k)) { if (overlay) *overlay = 1; }
	else if (k >= 0 && act != UA_FLINCH) k = ansm_pick_variant(a, gr, (uint32_t)k, r);
	return k;
}

int units_lookup_anim(const units_t *s, const game_t *g, int unit, unit_action action, unsigned dir, int *is_overlay_out)
{
	const unit_t *u = CU(s, unit);
	ansm_graph gr;
	if (is_overlay_out) *is_overlay_out = 0;
	if (!u || !get_graph(g, u, &gr)) return -1;
	if (action == UA_DEAD) {
		/* the same choice as the death handler, with its first variant */
		const ansm_section *a = g->ansm;
		int k = -1;
		unsigned cat, reg;
		static const unsigned cats[2] = { 3, 2 }, regs[3] = { 1, 0, 2 };
		for (cat = 0; cat < 2 && k < 0; cat++)
			for (reg = 0; reg < 3 && k < 0; reg++) {
				int p = ansm_damage_anim(a, &gr, cats[cat], dir & 3u, regs[reg]);
				if (p >= 0 && !is_overlay(a, &gr, p)) k = p;
			}
		return k;
	}
	return lookup_action(g, u, &gr, action, dir, 0.0f, is_overlay_out);
}

static unsigned anim_ticks(const game_t *g, const ansm_graph *gr, int k, unsigned *key)
{
	ansm_anim info;
	if (key) *key = 0;
	if (k < 0 || ansm_anim_info(g->ansm, gr, (uint32_t)k, &info) != 0) return 0;
	if (key && info.key_frame[0] > 0) *key = (unsigned)info.key_frame[0];
	return info.ticks;
}

static int anim_loops(const game_t *g, const ansm_graph *gr, int k)
{
	ansm_anim info;
	return k >= 0 && ansm_anim_info(g->ansm, gr, (uint32_t)k, &info) == 0 && (info.flags & ANSM_A_LOOPS);
}

static unsigned anim_frames(const game_t *g, const ansm_graph *gr, int k)
{
	ansm_anim info;
	return (k >= 0 && ansm_anim_info(g->ansm, gr, (uint32_t)k, &info) == 0) ? info.frame_count : 0u;
}

/* Starts animation k on the object: restarts at frame 0, or keeps the phase between two looping animations (move directions). */
static void play_anim(const game_t *g, unit_t *u, object_t *o, const ansm_graph *gr, int k, int keep_phase)
{
	if (k < 0 || u->base_graph < 0 || o->graph < 0) return;
	if (keep_phase && u->anim_cur >= 0 && anim_loops(g, gr, u->anim_cur) && anim_loops(g, gr, k)) {
		unsigned fo = anim_frames(g, gr, u->anim_cur), fn = anim_frames(g, gr, k);
		if (fo > 0 && fn > 0) o->frame = o->frame / (float)fo * (float)fn;
		if (o->frame < 0.0f) o->frame = 0.0f;
	} else o->frame = 0.0f;
	o->anim = (uint16_t)k;
	o->flags |= OBJ_F_ANIMATED;
	u->anim_cur = (int16_t)k;
}

/* ---- body, object sync ----------------------------------------------------------------------------------------------------- */
/* A scripted animation moves its unit by the animation's own root motion (the tag's frame info, per frame, in the unit's frame): Halo's cinematic actors
 * walk across the set that way (Keyes steps up to the hologram table in "well_thats_it_then"). The frames since the last tick are applied. */
static void custom_root_motion(unit_t *u, const object_t *o, game_t *g)
{
	anim_graph ag;
	int gi = o->graph, cur;
	if (u->custom_anim < 0 || !g->ow.anims || gi < 0 || (unsigned)gi >= g->ow.anims->graph_count) return;
	ag = anim_get_graph(g->ow.anims, (uint32_t)gi);
	if ((unsigned)u->custom_anim >= ag.anim_count) return;
	cur = (int)o->frame;
	while (u->rm_frame < cur && u->rm_frame < 30000) {
		float d[4], cy, sy;
		u->rm_frame++;
		anim_root_motion(g->ow.anims, &ag, (uint32_t)u->custom_anim, (uint32_t)u->rm_frame, d);
		cy = cosf(u->body.yaw); sy = sinf(u->body.yaw);
		u->body.pos[0] += cy * d[0] - sy * d[1];
		u->body.pos[1] += sy * d[0] + cy * d[1];
		/* d[2] (vertical root motion) is ignored: the body's physics keeps the unit on the floor */
		u->body.yaw += d[3];
		u->body.vel[0] = u->body.vel[1] = 0.0f;                  /* the animation moves the body: no leftover velocity to coast on afterwards */
	}
}

/* Ground speed of animation k of ANIM graph gi in wu/s: its root motion over one cycle (the tag's frame info). 0 when it has none. */
static float anim_ground_speed(const game_t *g, int gi, int k)
{
	anim_graph ag;
	anim_info in;
	float sx = 0.0f, sy = 0.0f;
	uint32_t f;
	if (k < 0 || gi < 0 || !g->ow.anims || (unsigned)gi >= g->ow.anims->graph_count) return 0.0f;
	ag = anim_get_graph(g->ow.anims, (uint32_t)gi);
	if ((unsigned)k >= ag.anim_count) return 0.0f;
	in = anim_get(g->ow.anims, &ag, (uint32_t)k);
	if (!in.root_motion || in.frame_count < 2) return 0.0f;
	for (f = 0; f < in.frame_count; f++) {
		float d[4];
		anim_root_motion(g->ow.anims, &ag, (uint32_t)k, f, d);
		sx += d[0]; sy += d[1];
	}
	return sqrtf(sx * sx + sy * sy) * (in.fps > 0.0f ? in.fps : 30.0f) / (float)in.frame_count;
}

/* An actor moves as fast as the move animation its posture and weapon select: the marines' `stand rifle move-front` covers 2.1 wu/s, their `alert rifle move-front` 0.42 and the
 * pistol's 2.25. The speeds of the biped tag (params_for) are the standing ones, so the other postures and weapons take the ground speed of their own animation, else the unit would
 * slide over the floor at four times the pace of its feet (Johnson's slow walk into the hangar, ADR-083). Recomputed when the posture, weapon or graph changes. */
static void refresh_move_speeds(unit_t *u, const game_t *g)
{
	ansm_graph gr;
	uint32_t key;
	float s[3], scale = 1.0f;
	unsigned d;
	if (u->base_graph < 0 || !get_graph(g, u, &gr)) return;
	if (!u->mv_have) {
		u->mv_base[0] = u->pp.run_speed; u->mv_base[1] = u->pp.back_speed; u->mv_base[2] = u->pp.strafe_speed; u->mv_base[3] = u->pp.crouch_speed; u->mv_base[4] = u->pp.walk_speed;
		u->mv_have = 1; u->mv_key = 0xFFFFFFFFu;
	}
	key = (uint32_t)u->posture | (((uint32_t)(u->weapon + 1) & 0xFFFu) << 2) | (((uint32_t)u->base_graph & 0x3FFu) << 14);
	if (key == u->mv_key) return;
	u->mv_key = key;
	u->pp.run_speed = u->mv_base[0]; u->pp.back_speed = u->mv_base[1]; u->pp.strafe_speed = u->mv_base[2]; u->pp.crouch_speed = u->mv_base[3]; u->pp.walk_speed = u->mv_base[4];
	for (d = 0; d < 3; d++) {
		int k = lookup_action(g, u, &gr, UA_MOVE, d, 0.0f, NULL);
		s[d] = (k >= 0 && !is_overlay(g->ansm, &gr, k)) ? anim_ground_speed(g, u->base_graph, k) : 0.0f;
	}
	if (s[0] < 0.05f) return;                                       /* no root motion to follow: the tag's speeds stay */
	if (u->posture == 1) { u->pp.crouch_speed = s[0]; return; }
	if (u->mv_base[0] > 0.05f) scale = s[0] / u->mv_base[0];
	u->pp.run_speed = s[0];
	if (s[1] > 0.05f) u->pp.back_speed = s[1];                         /* a direction whose animation carries no root motion keeps the tag's speed (the player's biped: its animations have none) */
	if (s[2] > 0.05f) u->pp.strafe_speed = s[2];
	u->pp.walk_speed = u->mv_base[4] * scale;
}

static void sync_object(const unit_t *u, object_t *o)
{
	o->pos[0] = u->body.pos[0]; o->pos[1] = u->body.pos[1]; o->pos[2] = u->body.pos[2];
	mat4_from_ypr(&o->root, u->body.yaw, 0.0f, 0.0f, o->pos);
}

static float speed2d(const unit_t *u) { return sqrtf(u->body.vel[0] * u->body.vel[0] + u->body.vel[1] * u->body.vel[1]); }

/* player_cmd_t input for a throttle fraction: player_tick applies a deadzone and rescales, so invert it */
static float throttle_input(float t)
{
	float m = fabsf(t);
	if (m < 1e-4f) return 0.0f;
	if (m > 1.0f) m = 1.0f;
	m = 0.15f + 0.85f * m;
	return t < 0.0f ? -m : m;
}

static void aim_vector(const unit_t *u, float dir[3])
{
	float rel = clampf(wrap_pi(u->ctl.aim_yaw - u->body.yaw), -AIM_YAW_LIMIT, AIM_YAW_LIMIT), yaw = u->body.yaw + rel;
	float pitch = clampf(u->ctl.aim_pitch, -AIM_PITCH_LIMIT, AIM_PITCH_LIMIT), cp = cosf(pitch);
	dir[0] = cp * cosf(yaw); dir[1] = cp * sinf(yaw); dir[2] = sinf(pitch);
}

void units_eye(const units_t *s, int unit, float eye[3], float dir[3])
{
	const unit_t *u = CU(s, unit);
	if (!u) { eye[0] = eye[1] = eye[2] = 0.0f; dir[0] = 1.0f; dir[1] = dir[2] = 0.0f; return; }
	player_eye(&u->body, &u->pp, eye);
	aim_vector(u, dir);
}

/* ---- params from tags ---------------------------------------------------------------------------------------------------- */
static void params_for(const game_t *g, const unitdef_biped_def *b, player_params_t *pp, float *turn_speed)
{
	float front, back, side, crouch;
	player_params_from_tags(pp, g->unit ? &g->globals : NULL, b);
	/* the walking speeds of an actor are the root motion of its animations (UNIT section, docs/17), not the player's globals */
	front = b->anim_speed[0];
	if (front <= 0.0f) front = g->globals.walk_speed > 0.0f ? g->globals.walk_speed : 1.0f;     /* a unit without move animations */
	back = b->anim_speed[1] > 0.0f ? b->anim_speed[1] : front * 0.75f;
	side = b->anim_speed[2] > 0.0f ? b->anim_speed[2] : front * 0.8f;
	crouch = b->anim_speed[3] > 0.0f ? b->anim_speed[3] : front * 0.5f;
	pp->run_speed = front; pp->back_speed = back; pp->strafe_speed = side; pp->crouch_speed = crouch;
	/* walking pace (UC_WALK) keeps the player's ratio of walk to run speed */
	pp->walk_speed = front * ((g->globals.walk_speed > 0.0f && g->globals.run_forward > 0.0f) ? g->globals.walk_speed / g->globals.run_forward : 0.55f);
	*turn_speed = b->turn_speed > 0.5f ? b->turn_speed : 3.0f;
	/* actors: two collision spheres (no gap while the capsule is at most four radii tall) and a one sphere ground probe; the player keeps the full capsule */
	pp->sweep_spheres = pp->height <= 4.0f * pp->radius + 0.002f ? 2u : 0u;
}

/* ---- vitals ---------------------------------------------------------------------------------------------------------------- */
static void set_vitals_scaled(game_t *g, const unit_t *u, float health, float shield, float recharge, uint16_t stun)
{
	int enemy = units_teams_hostile(u->team, 1), ally = is_ally_team(u->team);
	float hs = 1.0f, ss = 1.0f, rs = 1.0f;
	if (enemy) { hs = diff_value(g, UNITDEF_D_ENEMY_VITALITY, 1.0f); ss = diff_value(g, UNITDEF_D_ENEMY_SHIELD, 1.0f); rs = diff_value(g, UNITDEF_D_ENEMY_RECHARGE, 1.0f); }
	else if (ally) { hs = diff_value(g, UNITDEF_D_FRIEND_VITALITY, 1.0f); ss = diff_value(g, UNITDEF_D_FRIEND_SHIELD, 1.0f); rs = diff_value(g, UNITDEF_D_FRIEND_RECHARGE, 1.0f); }
	game_set_vitals(g, u->obj, health * hs, shield * ss, recharge * rs, stun);
}

static void set_damage_scale(game_t *g, const unit_t *u, float modifier)
{
	game_unit_t *gu = &g->unit_state[u->obj];
	float scale = 1.0f;
	if (units_teams_hostile(u->team, 1)) scale = diff_value(g, UNITDEF_D_ENEMY_DAMAGE, 1.0f);
	else if (is_ally_team(u->team)) scale = diff_value(g, UNITDEF_D_FRIEND_DAMAGE, 1.0f);
	if (modifier > 0.0f) scale *= modifier;
	gu->dmg_scale = scale;
	gu->ally = is_ally_team(u->team) ? 1 : 0;
	gu->team = (uint8_t)(u->team > 0 && u->team < 255 ? u->team : 0);
}

/* ---- attach / detach ------------------------------------------------------------------------------------------------------ */
void units_init(units_t *s)
{
	unsigned i;
	memset(s, 0, sizeof *s);
	for (i = 0; i < OBJ_MAX; i++) s->by_obj[i] = -1;
}

size_t units_state_size(void) { return sizeof(units_t); }

int units_of_object(const units_t *s, int obj) { return (s && obj >= 0 && obj < OBJ_MAX) ? s->by_obj[obj] : -1; }

void units_detach(units_t *s, int unit)
{
	unit_t *u = U(s, unit);
	if (!u) return;
	if (u->obj >= 0 && u->obj < OBJ_MAX && s->by_obj[u->obj] == unit) s->by_obj[u->obj] = -1;
	u->active = 0;
	while (s->count && !s->u[s->count - 1].active) s->count--;
}

unsigned units_rebind(units_t *s, const game_t *g)
{
	unsigned i, n = 0;
	for (i = 0; i < OBJ_MAX; i++) s->by_obj[i] = -1;
	for (i = 0; i < UNIT_MAX; i++) {
		unit_t *u = &s->u[i];
		if (!u->active) continue;
		if (u->obj < 0 || u->obj >= OBJ_MAX || !OBJ(g, u) || s->by_obj[u->obj] >= 0) { u->active = 0; continue; }
		s->by_obj[u->obj] = (int16_t)i;
		n++;
	}
	s->count = 0;
	for (i = 0; i < UNIT_MAX; i++) if (s->u[i].active) s->count = i + 1;
	return n;
}

static void apply_weapon(const game_t *g, unit_t *u, int widx)
{
	weap_weapon w;
	u->weapon = (int16_t)widx;
	memset(&u->wst, 0, sizeof u->wst);
	if (widx >= 0 && weapon_decode(g, widx, &w)) {
		weapon_state_init(&u->wst, &w);
		u->wst.mode = WEAP_MODE_IDLE;               /* units are ready the moment they hold it */
		u->wst.timer = 0;
	} else u->weapon = -1;
	u->anim_key = 0xFFFFu;
}

int units_attach(units_t *s, game_t *g, int obj, int team, int32_t weapon_tag)
{
	object_t *o;
	unit_t *u;
	scen_def d;
	unitdef_biped_def b;
	int idx = -1, bi, widx = -1;
	unsigned i;
	float yaw;
	if (!s || !g || !g->objects || !g->unit || !g->ow.scen || obj < 0 || (unsigned)obj >= g->objects->count || obj >= OBJ_MAX) return -1;
	if (s->by_obj[obj] >= 0 && s->u[s->by_obj[obj]].active) return s->by_obj[obj];
	o = &g->objects->obj[obj];
	if ((o->flags & OBJ_F_FREE) || o->type != SCEN_BIPED) return -1;
	d = scen_get_def(g->ow.scen, (uint32_t)o->def);
	bi = unitdef_find_biped(g->unit, d.tag_id);
	if (bi < 0 || !unitdef_get_biped(g->unit, (uint32_t)bi, &b)) return -1;
	for (i = 0; i < UNIT_MAX; i++) if (!s->u[i].active) { idx = (int)i; break; }
	if (idx < 0) return -1;
	u = &s->u[idx];
	memset(u, 0, sizeof *u);
	u->active = 1;
	u->team = (uint8_t)(team > 0 ? team : (b.default_team <= 9 ? b.default_team : 0));
	u->obj = (int16_t)obj;
	u->biped = (int16_t)bi;
	u->anim_tag = b.anim_tag;
	u->base_graph = o->graph;
	objects_set_face_graph(o, &g->ow, o->graph);
	u->custom_graph = -1; u->custom_anim = -1; u->custom_frame_scale = 1.0f; u->anim_cur = -1;
	u->anim_key = 0xFFFFu; u->anim_rate = 1.0f; u->rate_mult = 1.0f; u->weapon = -1;
	u->rng = (uint32_t)obj * 2654435761u ^ g->tick * 40503u ^ 0x9E3779B9u;
	yaw = atan2f(o->root.m[0][1], o->root.m[0][0]);
	player_init(&u->body, o->pos, yaw);
	u->body.pos[2] += 0.02f;                                    /* a placement stands exactly on the floor: start a hair above it */
	params_for(g, &b, &u->pp, &u->turn_speed);
	u->turn_threshold = b.turn_threshold; u->turning = 0; u->look_range[0] = -1.0f; u->aim_key = 0xFFFFFFFFu; u->aim_k = -1;
	u->mv_have = 0;                                                  /* the speeds of the new tag are the base of refresh_move_speeds */
	u->ctl.aim_yaw = yaw; u->ctl.weapon_slot = -1;
	u->spawn_tick = g->tick;
	/* weapon: -1 unarmed, 0 the biped's first default weapon */
	if (weapon_tag != -1) {
		uint32_t tag = weapon_tag ? (uint32_t)weapon_tag : (b.weapon_count > 0 ? b.weapon_tags[0] : 0u);
		if (tag && g->weap) widx = weap_find_weapon(g->weap, tag);
	}
	apply_weapon(g, u, widx);
	/* grenades */
	u->grenade_type = (uint8_t)(b.grenade_type > 1 ? 0 : b.grenade_type);
	u->grenades[u->grenade_type] = b.grenade_count;
	/* difficulty: the vitals of the collision model (already in the game) or of the biped */
	{
		game_unit_t *gu = &g->unit_state[obj];
		float hp = gu->has_vitals ? gu->vitals.max_health : b.health, sh = gu->has_vitals ? gu->vitals.max_shield : b.shield;
		float rc = gu->has_vitals ? gu->vitals.recharge : b.shield_recharge_rate;
		uint16_t st = gu->has_vitals ? gu->vitals.stun_ticks : (uint16_t)(b.shield_stun_time * TICKS);
		if (!(o->flags & OBJ_F_FREE) && hp > 0.0f && !gu->dead) set_vitals_scaled(g, u, hp, sh, rc, st);
	}
	set_damage_scale(g, u, 0.0f);
	if (units_teams_hostile(u->team, 1)) u->rate_mult = diff_value(g, UNITDEF_D_RATE_OF_FIRE, 1.0f);
	u->hp_prev = g->unit_state[obj].vitals.health;
	s->by_obj[obj] = (int16_t)idx;
	if ((unsigned)idx >= s->count) s->count = (unsigned)idx + 1;
	s->spawned++;
	sync_object(u, o);
	return idx;
}

int units_apply_variant(units_t *s, game_t *g, int unit, const aidef_variant_def *v)
{
	unit_t *u = U(s, unit);
	unitdef_biped_def b;
	game_unit_t *gu;
	if (!u || !v || !g->objects) return 0;
	gu = &g->unit_state[u->obj];
	u->variant_tag = v->tag_id;
	if (!gu->dead && get_biped(g, u, &b)) {
		float hp = v->body_vitality > 0.0f ? v->body_vitality : (b.health > 0.0f ? b.health : gu->vitals.max_health);
		float sh = v->body_vitality > 0.0f ? v->shield_vitality : b.shield;
		if (hp > 0.0f) set_vitals_scaled(g, u, hp, sh, b.shield_recharge_rate, (uint16_t)(b.shield_stun_time * TICKS));
		u->hp_prev = gu->vitals.health;
	}
	set_damage_scale(g, u, v->weapon_damage_modifier);
	u->proj_error = v->projectile_error > 0.0f ? v->projectile_error : 0.0f;
	if (units_teams_hostile(u->team, 1)) u->proj_error *= diff_value(g, UNITDEF_D_PROJECTILE_ERROR, 1.0f);
	if (v->grenade_count[1] > 0 || v->grenade_count[0] > 0) {
		unsigned lo = v->grenade_count[0], hi = v->grenade_count[1] >= lo ? v->grenade_count[1] : lo;
		unsigned n = lo + (unsigned)(rnd01(u) * (float)(hi - lo + 1u));
		u->grenade_type = (uint8_t)(v->grenade_type > 1 ? 0 : v->grenade_type);
		u->grenades[0] = u->grenades[1] = 0;
		u->grenades[u->grenade_type] = (uint16_t)(n > hi ? hi : n);
	}
	if (v->weapon_tag && g->weap) {
		int widx = weap_find_weapon(g->weap, v->weapon_tag);
		if (widx >= 0 && widx != u->weapon) apply_weapon(g, u, widx);
	}
	return 1;
}

void units_set_control(units_t *s, int unit, const unit_control_t *c)
{
	unit_t *u = U(s, unit);
	if (!u || !c) return;
	u->ctl.throttle_fwd = clampf(finite_or(c->throttle_fwd, 0.0f), -1.0f, 1.0f);
	u->ctl.throttle_left = clampf(finite_or(c->throttle_left, 0.0f), -1.0f, 1.0f);
	u->ctl.aim_yaw = wrap_pi(finite_or(c->aim_yaw, u->body.yaw));
	u->ctl.aim_pitch = clampf(finite_or(c->aim_pitch, 0.0f), -AIM_PITCH_LIMIT, AIM_PITCH_LIMIT);
	u->ctl.flags = c->flags;
	u->ctl.weapon_slot = c->weapon_slot;
}

void units_apply_cinedef(units_t *s, int unit, const cinedef_ctl *c)
{
	unit_control_t k;
	unit_t *u = U(s, unit);
	uint32_t f = 0;
	if (!u || !c) return;
	k = u->ctl;
	k.throttle_fwd = c->throttle[0]; k.throttle_left = c->throttle[1];
	k.aim_yaw = (float)c->yaw * CINE_COUNT_RAD;
	k.aim_pitch = (float)c->pitch * CINE_COUNT_RAD;
	if (c->flags & CINEDEF_CF_CROUCH) f |= UC_CROUCH;
	if (c->flags & CINEDEF_CF_JUMP) f |= UC_JUMP;
	if (c->flags & CINEDEF_CF_PRIMARY_FIRE) f |= UC_FIRE;
	if (c->flags & CINEDEF_CF_SECONDARY_FIRE) f |= UC_FIRE2;
	if (c->flags & CINEDEF_CF_GRENADE) f |= UC_GRENADE;
	if (c->flags & CINEDEF_CF_MELEE) f |= UC_MELEE;
	if (c->flags & CINEDEF_CF_RELOAD) f |= UC_RELOAD;
	k.flags = f;
	k.weapon_slot = (int8_t)(c->weapon >= 0 && c->weapon < 127 ? c->weapon : -1);
	units_set_control(s, unit, &k);
}

void units_teleport(units_t *s, game_t *g, int unit, const float pos[3], float yaw)
{
	unit_t *u = U(s, unit);
	object_t *o;
	if (!u || !pos || !(o = OBJ(g, u))) return;
	u->body.pos[0] = pos[0]; u->body.pos[1] = pos[1]; u->body.pos[2] = pos[2];
	u->body.vel[0] = u->body.vel[1] = u->body.vel[2] = 0.0f;
	u->body.yaw = wrap_pi(yaw); u->body.on_ground = 0;
	u->ctl.aim_yaw = u->body.yaw;
	u->settle = 0;
	sync_object(u, o);
}

void units_pin(units_t *s, game_t *g, int unit, const float pos[3], float yaw)
{
	unit_t *u = U(s, unit);
	object_t *o;
	if (!u || !pos || !(o = OBJ(g, u))) return;
	u->body.pos[0] = pos[0]; u->body.pos[1] = pos[1]; u->body.pos[2] = pos[2];
	u->body.vel[0] = u->body.vel[1] = u->body.vel[2] = 0.0f;
	u->body.yaw = wrap_pi(yaw); u->ctl.aim_yaw = u->body.yaw;
	if (!u->body.on_ground) {                                                 /* stands where it was put: no fall, and the physics may rest */
		u->body.on_ground = 1;
		u->body.ground_normal[0] = u->body.ground_normal[1] = 0.0f; u->body.ground_normal[2] = 1.0f;
	}
	sync_object(u, o);
}

void units_give_weapon(units_t *s, game_t *g, int unit, uint32_t weapon_tag)
{
	unit_t *u = U(s, unit);
	int widx;
	if (!u || !g->weap) return;
	widx = weapon_tag ? weap_find_weapon(g->weap, weapon_tag) : -1;
	if (widx < 0) return;
	apply_weapon(g, u, widx);
}

/* ---- vitals API -------------------------------------------------------------------------------------------------------------- */
int units_alive(const units_t *s, const game_t *g, int unit)
{
	const unit_t *u = CU(s, unit);
	const game_unit_t *gu;
	if (!u || u->dying || u->obj < 0 || u->obj >= OBJ_MAX) return 0;
	gu = &g->unit_state[u->obj];
	return !gu->dead && !(gu->has_vitals && vitals_dead(&gu->vitals));
}

float units_damage(units_t *s, game_t *g, int unit, float amount)
{
	unit_t *u = U(s, unit);
	if (!u || (u->flags & UF_INVULNERABLE) || !(amount > 0.0f)) return 0.0f;
	return game_damage_object(g, u->obj, amount, NULL);
}

static void begin_death(units_t *s, unit_t *u, game_t *g, object_t *o);

void units_kill(units_t *s, game_t *g, int unit, int silent)
{
	unit_t *u = U(s, unit);
	game_unit_t *gu;
	object_t *o;
	if (!u || !(o = OBJ(g, u))) return;
	gu = &g->unit_state[u->obj];
	if (gu->dead) { if (!u->dying) begin_death(s, u, g, o); return; }
	u->flags &= (uint8_t)~UF_INVULNERABLE;
	if (!gu->has_vitals) game_set_vitals(g, u->obj, 1.0f, 0.0f, 0.0f, 0);
	if (silent) {
		gu->vitals.health = 0.0f; gu->vitals.shield = 0.0f; gu->vitals.overshield = 0.0f;
		gu->vitals.flags |= VITALS_F_DEAD;
		gu->dead = 1;
	} else {
		game_damage_object(g, u->obj, gu->vitals.health + gu->vitals.shield + gu->vitals.overshield + 1.0f, NULL);
		if (!gu->dead) { gu->vitals.health = 0.0f; gu->vitals.flags |= VITALS_F_DEAD; gu->dead = 1; }
	}
	begin_death(s, u, g, o);
}

/* ---- death -------------------------------------------------------------------------------------------------------------------- */
static int death_anim(const game_t *g, const unit_t *u, const ansm_graph *gr, int airborne, unsigned dir, float r)
{
	const ansm_section *a = g->ansm;
	int k = -1;
	unsigned cat, reg, di;
	static const unsigned cats[2] = { 3, 2 };             /* health kill, then shield kill */
	static const unsigned regs[4] = { 1, 0, 2, 5 };       /* chest, gut, head, leg */
	if (airborne) k = block_slot(a, gr, "stand", NULL, ANSM_U_AIRBORNE_DEAD, NULL);
	if (k >= 0 && is_overlay(a, gr, k)) k = -1;
	for (cat = 0; cat < 2 && k < 0; cat++)
		for (di = 0; di < 2 && k < 0; di++)
			for (reg = 0; reg < 4 && k < 0; reg++) {
				unsigned region = regs[(reg + (r < 0.5f ? 0u : 1u)) & 3u];
				int p = ansm_damage_anim(a, gr, cats[cat], di ? 0u : (dir & 3u), region);
				if (p >= 0 && !is_overlay(a, gr, p)) k = p;
			}
	if (k < 0) k = block_slot(a, gr, "stand", NULL, ANSM_U_LANDING_DEAD, NULL);
	if (k < 0) {                                           /* any animation whose name says it is a death */
		uint32_t i;
		for (i = 0; i < gr->anim_count && k < 0; i++) {
			ansm_anim info;
			if (ansm_anim_info(a, gr, i, &info) == 0 && info.type != ANSM_TYPE_OVERLAY &&
			    (strstr(info.name, "kill") || strstr(info.name, "dead") || strstr(info.name, "death"))) k = (int)i;
		}
	}
	(void)u;
	return k >= 0 ? ansm_pick_variant(a, gr, (uint32_t)k, r) : -1;
}

static void drop_weapon(game_t *g, const unit_t *u, const object_t *src)
{
	weap_weapon w;
	int def, slot;
	float pos[3];
	if (u->weapon < 0 || !g->objects || !g->ow.scen || !weapon_decode(g, u->weapon, &w)) return;
	def = scen_find_def(g->ow.scen, w.tag_id);
	if (def < 0 || g->objects->count + DROP_RESERVE >= OBJ_MAX) return;
	pos[0] = u->body.pos[0]; pos[1] = u->body.pos[1]; pos[2] = u->body.pos[2] + 0.05f;
	slot = objects_spawn_def(g->objects, &g->ow, (unsigned)def, pos, u->body.yaw);
	if (slot < 0) return;
	g->objects->obj[slot].bsp_mask = src->bsp_mask;
	game_object_spawned(g, (unsigned)slot);
}

static void finish_custom(unit_t *u, object_t *o)
{
	if (u->custom_graph >= 0) o->graph = u->base_graph;
	u->custom_graph = -1; u->custom_anim = -1; u->custom_frame_scale = 1.0f;
	u->anim_key = 0xFFFFu; u->anim_cur = -1;
}

static void begin_death(units_t *s, unit_t *u, game_t *g, object_t *o)
{
	ansm_graph gr;
	int k;
	float r;
	unsigned dir = 0;
	u->dying = 1;
	u->action = UA_DEAD;
	u->action_ticks = 0; u->event_at = 0; u->fire_hold = 0;
	memset(&u->ctl, 0, sizeof u->ctl);
	u->ctl.aim_yaw = u->body.yaw; u->ctl.weapon_slot = -1;
	if (u->custom_graph >= 0 || u->custom_anim >= 0) finish_custom(u, o);
	r = rnd01(u);
	/* direction of the killing hit relative to the body: front, left, right, back (the damage table's order) */
	if (u->hit_dir[0] != 0.0f || u->hit_dir[1] != 0.0f) {
		float fx = cosf(u->body.yaw), fy = sinf(u->body.yaw);
		float fwd = u->hit_dir[0] * fx + u->hit_dir[1] * fy, left = -u->hit_dir[0] * fy + u->hit_dir[1] * fx;   /* left = (-fy, fx) */
		if (fwd >= 0.7f) dir = 0; else if (fwd <= -0.7f) dir = 3; else dir = left > 0.0f ? 1u : 2u;
	}
	if (get_graph(g, u, &gr) && u->base_graph >= 0 && o->graph >= 0) {
		k = death_anim(g, u, &gr, u->body.on_ground ? 0 : (u->body.vel[2] < -0.5f), dir, r);
		if (k >= 0) { o->anim = (uint16_t)k; o->frame = 0.0f; o->flags |= OBJ_F_ANIMATED; u->anim_cur = (int16_t)k; }
		else o->flags &= ~OBJ_F_ANIMATED;
	}
	if (!(u->flags & UF_NO_DROP)) drop_weapon(g, u, o);
	emit(s, UE_DEATH, u, 0, 0, u->body.pos, NULL);
	s->died++;
	sync_object(u, o);
}

/* ---- the tick ------------------------------------------------------------------------------------------------------------------ */
static int busy_action(unsigned a) { return a == UA_MELEE || a == UA_THROW || a == UA_FLINCH || a == UA_LAND || a == UA_CUSTOM || a == UA_DEAD; }

/* the nearest living target in front of the unit: an object with vitals (not a friendly unit) or the player; applies the blow */
static void melee_blow(units_t *s, unit_t *u, game_t *g)
{
	unitdef_biped_def b;
	weap_weapon w;
	weap_damage dmg;
	float fx = cosf(u->body.yaw), fy = sinf(u->body.yaw), best = 1e9f, amount;
	int target = -1, di = -1, hit_player = 0;
	unsigned i;
	uint32_t dtag = 0;
	phys_hit ph;
	float eye[3];
	player_eye(&u->body, &u->pp, eye);
	if (u->weapon >= 0 && weapon_decode(g, u->weapon, &w)) dtag = w.melee_damage;
	if (!dtag && get_biped(g, u, &b)) dtag = b.melee_damage_tag;
	if (!dtag || !g->weap || (di = weap_find_damage(g->weap, dtag)) < 0 || !weap_get_damage(g->weap, (uint32_t)di, &dmg)) return;
	for (i = 0; g->objects && i < g->objects->count; i++) {
		const object_t *t = &g->objects->obj[i];
		const game_unit_t *gu = &g->unit_state[i];
		float dx, dy, dist, c[3];
		int ui;
		if ((int)i == u->obj || !gu->has_vitals || gu->dead || t->model < 0 || (t->flags & (OBJ_F_FREE | OBJ_F_INACTIVE))) continue;
		ui = s->by_obj[i];
		if (ui >= 0 && s->u[ui].active && !units_teams_hostile(u->team, s->u[ui].team)) continue;      /* no blows at friends */
		dx = t->pos[0] - u->body.pos[0]; dy = t->pos[1] - u->body.pos[1];
		dist = sqrtf(dx * dx + dy * dy);
		if (dist > MELEE_RANGE || fabsf(t->pos[2] - u->body.pos[2]) > MELEE_HEIGHT) continue;
		if (dist > 1e-4f && (dx * fx + dy * fy) / dist < MELEE_CONE_COS) continue;
		c[0] = t->pos[0]; c[1] = t->pos[1]; c[2] = t->pos[2] + 0.3f;
		if (g->has_phys && g->phys.ray(g->phys.user, eye, c, &ph)) continue;                    /* a wall in between */
		if (dist < best) { best = dist; target = (int)i; }
	}
	if (units_teams_hostile(u->team, 1) && !vitals_dead(&g->vitals)) {
		float dx = g->player.pos[0] - u->body.pos[0], dy = g->player.pos[1] - u->body.pos[1], dist = sqrtf(dx * dx + dy * dy);
		if (dist <= MELEE_RANGE && fabsf(g->player.pos[2] - u->body.pos[2]) <= MELEE_HEIGHT && dist < best &&
		    (dist < 1e-4f || (dx * fx + dy * fy) / dist >= MELEE_CONE_COS)) { hit_player = 1; target = -1; }
	}
	if (target < 0 && !hit_player) { emit(s, UE_MELEE, u, 0, 0, u->body.pos, NULL); return; }
	emit(s, UE_MELEE, u, 1, 0, u->body.pos, NULL);
	amount = dmg.damage_upper[0] + (dmg.damage_upper[1] - dmg.damage_upper[0]) * rnd01(u);
	if (amount < dmg.damage_lower) amount = dmg.damage_lower;
	if (g->unit_state[u->obj].dmg_scale > 0.0f) amount *= g->unit_state[u->obj].dmg_scale;
	if (hit_player) game_damage_player(g, amount, u->body.pos);
	else game_damage_object(g, target, amount, NULL);
}

static void throw_grenade(units_t *s, unit_t *u, game_t *g)
{
	unitdef_biped_def b;
	float eye[3], dir[3], speed = 10.0f;
	if (u->grenades[u->grenade_type] == 0) return;
	if (get_biped(g, u, &b) && b.grenade_velocity > 0.0f) speed = b.grenade_velocity;
	player_eye(&u->body, &u->pp, eye);
	aim_vector(u, dir);
	eye[0] += dir[0] * 0.3f; eye[1] += dir[1] * 0.3f;
	if (game_throw_grenade(g, u->obj, u->grenade_type, eye, dir, speed)) { u->grenades[u->grenade_type]--; emit(s, UE_THROW, u, u->grenade_type, 0, eye, dir); }
}

static void shoot(units_t *s, unit_t *u, game_t *g, const weap_weapon *w, const weap_events *wev)
{
	unsigned t, k, n;
	float eye[3], fwd[3];
	player_eye(&u->body, &u->pp, eye);
	aim_vector(u, fwd);
	for (t = 0; t < w->trigger_count; t++) {
		const weap_trigger *tr = &w->trigger[t];
		int pi;
		if (!wev->shot[t]) continue;
		g->st_shots++;
		emit(s, UE_FIRE, u, t, w->tag_id, eye, fwd);
		if (!tr->projectile) continue;
		pi = weap_find_projectile(g->weap, tr->projectile);
		if (pi < 0) continue;
		n = wev->projectiles[t] ? wev->projectiles[t] : 1u;
		for (k = 0; k < n; k++) {
			float ang = wev->error[t] + u->proj_error, rr = sqrtf(rnd01(u)) * ang, th = rnd01(u) * 2.0f * PI_F;
			float cy = cosf(u->body.yaw), sy = sinf(u->body.yaw);
			float r[3] = { fwd[1], -fwd[0], 0.0f }, up[3], rl = sqrtf(r[0] * r[0] + r[1] * r[1]), dir[3], muzzle[3];
			if (rl < 1e-4f) { r[0] = sy; r[1] = -cy; rl = 1.0f; }       /* aiming straight up or down */
			r[0] /= rl; r[1] /= rl;
			up[0] = r[1] * fwd[2] - r[2] * fwd[1];                       /* up = right x fwd */
			up[1] = r[2] * fwd[0] - r[0] * fwd[2];
			up[2] = r[0] * fwd[1] - r[1] * fwd[0];
			dir[0] = fwd[0] + (r[0] * cosf(th) + up[0] * sinf(th)) * rr;
			dir[1] = fwd[1] + (r[1] * cosf(th) + up[1] * sinf(th)) * rr;
			dir[2] = fwd[2] + (r[2] * cosf(th) + up[2] * sinf(th)) * rr;
			muzzle[0] = eye[0] + fwd[0] * 0.25f; muzzle[1] = eye[1] + fwd[1] * 0.25f; muzzle[2] = eye[2] + fwd[2] * 0.25f - 0.08f;
			game_fire_projectile(g, u->obj, (unsigned)pi, muzzle, dir, 0.0f);
		}
	}
}

static void start_oneshot(unit_t *u, const game_t *g, unit_action act, unsigned dir, unsigned def_ticks, int key_event)
{
	ansm_graph gr;
	unsigned ticks = 0, key = 0;
	if (get_graph(g, u, &gr)) {
		int k = lookup_action(g, u, &gr, act, dir, 0.0f, NULL);
		ticks = anim_ticks(g, &gr, k, &key);
	}
	if (!ticks) ticks = def_ticks;
	if (act == UA_FLINCH && ticks > FLINCH_MAX_TICKS) ticks = FLINCH_MAX_TICKS;
	if (ticks > 200u) ticks = 200u;
	u->action = (uint8_t)act;
	u->action_ticks = (uint16_t)ticks;
	u->event_at = 0;
	if (key_event) u->event_at = (uint16_t)(key > 0 && key < ticks ? ticks - key : ticks / 2u);
	if (key_event && !u->event_at) u->event_at = 1;
	u->anim_key = 0xFFFFu;
	u->anim_rate = 1.0f;
}

static void anim_update(unit_t *u, object_t *o, const game_t *g, float speed, float turn_left)
{
	ansm_graph gr;
	unsigned kind = K_IDLE, sub = 0;
	unit_action act = UA_IDLE;
	int k, keep = 0;
	uint16_t key;
	float rate = 1.0f;
	if (u->base_graph < 0 || o->graph < 0 || !get_graph(g, u, &gr)) return;
	switch (u->action) {
	case UA_AIRBORNE: kind = K_AIR; act = UA_AIRBORNE; break;
	case UA_LAND: kind = K_LAND; act = UA_LAND; sub = u->move_dir & 1u; break;
	case UA_MELEE: kind = K_MELEE; act = UA_MELEE; break;
	case UA_THROW: kind = K_THROW; act = UA_THROW; break;
	case UA_FLINCH: kind = K_FLINCH; act = UA_FLINCH; sub = u->move_dir & 3u; break;
	default: break;
	}
	if (kind == K_IDLE) {
		if (speed > MOVING_SPEED) {
			float fx = cosf(u->body.yaw), fy = sinf(u->body.yaw);
			float vf = u->body.vel[0] * fx + u->body.vel[1] * fy, vl = -u->body.vel[0] * fy + u->body.vel[1] * fx, af = fabsf(vf), al = fabsf(vl);
			unsigned dir = u->move_dir & 3u;
			/* sticky direction: the other axis must clearly dominate before the animation switches */
			if (dir < 2u) { if (al > af * 1.3f) dir = vl > 0.0f ? 2u : 3u; else dir = vf >= 0.0f ? 0u : 1u; }
			else { if (af > al * 1.3f) dir = vf >= 0.0f ? 0u : 1u; else dir = vl > 0.0f ? 2u : 3u; }
			u->move_dir = (uint8_t)dir;
			kind = K_MOVE; sub = dir; act = UA_MOVE;
			{
				float ref = dir == 0u ? u->pp.run_speed : dir == 1u ? u->pp.back_speed : u->pp.strafe_speed;
				if (u->posture == 1 && dir == 0u) ref = u->pp.crouch_speed;
				else if (u->posture == 1) ref *= u->pp.run_speed > 0.0f ? u->pp.crouch_speed / u->pp.run_speed : 1.0f;
				rate = ref > 0.05f ? clampf(speed / ref, 0.35f, 1.8f) : 1.0f;
			}
		} else if (u->action == UA_RELOAD) {
			kind = K_RELOAD; act = UA_RELOAD;
			rate = u->anim_rate;
		} else if (fabsf(turn_left) > TURN_ANIM_ANGLE) {
			kind = K_TURN; sub = turn_left > 0.0f ? 0u : 1u; act = UA_IDLE;
		}
	}
	key = (uint16_t)(kind | (sub << 4) | ((unsigned)u->posture << 8));
	if (key != u->anim_key || u->anim_cur == -1) {
		unsigned dir = sub;
		float r = rnd01(u);
		unsigned prev_kind = u->anim_key == 0xFFFFu ? 0u : (u->anim_key & 15u);
		u->anim_key = key;
		if (kind == K_TURN) {                       /* turn-left / turn-right class slots */
			const ansm_section *a = g->ansm;
			weap_weapon w;
			const char *wl = weapon_label(g, u, &w);
			int cls = -1;
			k = find_action(a, &gr, u->posture, wl, sub ? ANSM_C_TURN_RIGHT : ANSM_C_TURN_LEFT, &cls);
			if (k < 0) k = lookup_action(g, u, &gr, UA_IDLE, 0, r, NULL);
		} else k = lookup_action(g, u, &gr, act, dir, r, NULL);
		if (k >= 0 && is_overlay(g->ansm, &gr, k)) k = -1;
		if (k < 0 && act != UA_IDLE) k = lookup_action(g, u, &gr, UA_IDLE, 0, r, NULL);
		if (k >= 0) {
			keep = (kind == K_MOVE && prev_kind == K_MOVE) && u->anim_cur >= 0;
			if (k != u->anim_cur || kind == K_LAND || kind == K_MELEE || kind == K_THROW || kind == K_FLINCH || kind == K_RELOAD) play_anim(g, u, o, &gr, k, keep);
			else { o->anim = (uint16_t)k; o->flags |= OBJ_F_ANIMATED; }
			if (kind == K_IDLE) { int cls = -1; weap_weapon w; const char *wl = weapon_label(g, u, &w); (void)find_action(g->ansm, &gr, u->posture, wl, ANSM_C_IDLE, &cls); if (cls >= 0) u->anim_class = (uint16_t)cls; }
		} else u->anim_cur = -2;                  /* nothing to play for this state: keep what the object has, do not look again */
	} else if (turn_left != 0.0f && kind == K_TURN && u->anim_cur >= 0) {
		/* a turn animation that ran out while the body still turns: play it again */
		if (!anim_loops(g, &gr, u->anim_cur) && anim_frames(g, &gr, u->anim_cur) > 0 && o->frame >= (float)anim_frames(g, &gr, u->anim_cur) - 1.5f) o->frame = 0.0f;
	}
	if (u->anim_cur >= 0 && (o->flags & OBJ_F_ANIMATED) && rate != 1.0f) {
		o->frame += rate - 1.0f;
		if (o->frame < 0.0f) o->frame = 0.0f;
	}
}

/* a world with nothing in it (a game without collision: units fall and keep going) */
static int no_ray(void *u, const float a[3], const float b[3], phys_hit *h) { (void)u; (void)a; (void)b; (void)h; return 0; }
static int no_sweep(void *u, const float a[3], const float b[3], float r, phys_hit *h) { (void)u; (void)a; (void)b; (void)r; (void)h; return 0; }
static const phys_world k_empty_world = { NULL, no_ray, no_sweep, NULL };

/* The aim-still overlay of an armed unit's posture / weapon block and the aiming screen of that class block (looked up when posture, weapon or graph change). */
static void refresh_aim_overlay(unit_t *u, const game_t *g)
{
	ansm_graph gr;
	weap_weapon w;
	const char *wl;
	uint32_t key = (uint32_t)u->posture | (((uint32_t)(u->weapon + 1) & 0xFFFu) << 2) | (((uint32_t)u->base_graph & 0x3FFu) << 14);
	unsigned i;
	if (key == u->aim_key) return;
	u->aim_key = key; u->aim_k = -1;
	if (u->weapon < 0 || u->base_graph < 0 || !get_graph(g, u, &gr)) return;
	wl = weapon_label(g, u, &w);
	for (i = 0; i < 2 && u->aim_k < 0; i++) {
		const char *lbl = i == 0 ? (u->posture == 2 ? "alert" : u->posture ? "crouch" : "stand") : "stand";
		int cls = -1, ui, k = block_slot(g->ansm, &gr, lbl, wl, ANSM_C_AIM_STILL, &cls);
		ansm_class ci;
		if (k < 0 || !is_overlay(g->ansm, &gr, k) || cls < 0 || (ui = ansm_find_unit(g->ansm, &gr, lbl)) < 0 || ansm_class_info(g->ansm, &gr, (uint32_t)ui, (uint32_t)cls, &ci) != 0) continue;
		u->aim_k = (int16_t)k;
		ansm_aim_ranges(&ci.aim, u->aim_range);
		if (u->aim_range[0] < 1e-3f && u->aim_range[1] < 1e-3f && u->aim_range[2] < 1e-3f && u->aim_range[3] < 1e-3f) u->aim_k = -1;
	}
}

/* the animation of a stationary turn (turn-left / turn-right class slot of the unit's posture block), -1 when the graph has none */
static int turn_anim_for(const game_t *g, const unit_t *u, int right)
{
	ansm_graph gr;
	weap_weapon w;
	const char *wl;
	int cls = -1, k;
	if (!get_graph(g, u, &gr)) return -1;
	wl = weapon_label(g, u, &w);
	k = find_action(g->ansm, &gr, u->posture, wl, right ? ANSM_C_TURN_RIGHT : ANSM_C_TURN_LEFT, &cls);
	return (k >= 0 && !is_overlay(g->ansm, &gr, k)) ? k : -1;
}

/* the physics step of a living or dying body: turn towards the aim, then player_tick with the throttle as a stick */
static unsigned s_sweeps;                                           /* player_tick calls since the last units_take_sweeps (diagnostics) */
unsigned units_take_sweeps(void) { unsigned n = s_sweeps; s_sweeps = 0; return n; }

static void step_body(unit_t *u, const game_t *g, int turn, float throttle_scale, int mul, float *turn_left)
{
	player_cmd_t cmd;
	float tf = u->ctl.throttle_fwd * throttle_scale, tl = u->ctl.throttle_left * throttle_scale, mag;
	int goal_crouch = (u->ctl.flags & UC_CROUCH) != 0, settled;
	*turn_left = 0.0f;
	if (turn) {
		float delta = wrap_pi(u->ctl.aim_yaw - u->body.yaw), step = u->turn_speed * DT;
		if (u->action == UA_FLINCH) step *= 0.5f;
		/* a scripted or recorded unit that stands still keeps its body until the aim is past the biped's stationary turning threshold (the Xbox requests a turn animation only then,
		 * XBE 0x488a0 against the cosine cached at +0x4c8); closer aims are the head's and the arms' (apply_grid). The turn is its animation: the body follows the root yaw of
		 * `turn-left` / `turn-right` (90 degrees over 14 frames standing, 20 alert), not the turning speed, which is the moving one (+0x2f0). */
		if ((u->flags & UF_NO_AI) && !(u->flags & UF_PLAYER) && u->turn_threshold > 0.0f && g_units_turn_gate && tf * tf + tl * tl < 1e-4f && speed2d(u) <= MOVING_SPEED) {
			if (!u->turning && fabsf(delta) > u->turn_threshold) {
				int k = turn_anim_for(g, u, delta < 0.0f);
				if (k >= 0) { u->turning = delta < 0.0f ? 2u : 1u; u->turn_k = (int16_t)k; u->turn_f = 0; }
				else goto fallback_turn;                      /* a graph without turn animations: the turning speed */
			}
			if (u->turning) {
				anim_graph ag;
				float d[4] = { 0, 0, 0, 0 };
				uint32_t frames = 0;
				if (u->base_graph >= 0 && g->ow.anims && (unsigned)u->base_graph < g->ow.anims->graph_count) {
					ag = anim_get_graph(g->ow.anims, (uint32_t)u->base_graph);
					if ((unsigned)u->turn_k < ag.anim_count) {
						frames = anim_get(g->ow.anims, &ag, (uint32_t)u->turn_k).frame_count;
						anim_root_motion(g->ow.anims, &ag, (uint32_t)u->turn_k, (uint32_t)u->turn_f, d);
					}
				}
				u->body.yaw = wrap_pi(u->body.yaw + d[3]);
				if (++u->turn_f >= frames) u->turning = 0;
				*turn_left = u->turning ? (u->turning == 1u ? 1.0f : -1.0f) : 0.0f;
			}
			goto no_turn;
		}
		u->turning = 0;
fallback_turn:
		*turn_left = delta;
		u->body.yaw = wrap_pi(u->body.yaw + clampf(delta, -step, step));
	}
no_turn:
	mag = sqrtf(tf * tf + tl * tl);
	if (mag > 1.0f) { tf /= mag; tl /= mag; }
	memset(&cmd, 0, sizeof cmd);
	cmd.move_y = throttle_input(tf);
	cmd.move_x = throttle_input(-tl);                 /* player_cmd: x is strafe RIGHT, throttle_left is positive to the left */
	cmd.jump = (u->ctl.flags & UC_JUMP) != 0 && !u->dying;
	cmd.crouch = goal_crouch && !u->dying;
	cmd.walk = (u->ctl.flags & UC_WALK) != 0;
	settled = u->body.on_ground && cmd.move_x == 0.0f && cmd.move_y == 0.0f && !cmd.jump &&
	          fabsf(u->body.vel[0]) + fabsf(u->body.vel[1]) + fabsf(u->body.vel[2]) < 1e-3f &&
	          fabsf(u->body.crouch - (cmd.crouch ? 1.0f : 0.0f)) < 0.01f;
	if (settled && u->settle < SETTLE_PERIOD) { u->settle++; return; }      /* nothing moves: skip the sweeps, re-check the ground now and then */
	if (mul == 0) return;                                                 /* distance LOD: the other tick of the pair */
	u->settle = 0;
	u->body.landed_speed = 0.0f;
	s_sweeps++;
	player_tick(&u->body, &u->pp, (g->has_phys && g->phys.sweep) ? &g->phys : &k_empty_world, &cmd, DT * (float)mul);
}

/* fall damage and landing animation (globals / biped landing thresholds) */
static void landed(units_t *s, unit_t *u, game_t *g)
{
	unitdef_biped_def b;
	float v = u->body.landed_speed, vt = v / TICKS, lo = g->globals.fall_velocity_harm_min, hi = g->globals.fall_velocity_harm_max;
	if (!(v > 0.0f)) return;
	{ float d[3] = { v, 0.0f, 0.0f }; emit(s, UE_LAND, u, 0, 0, u->body.pos, d); }
	if (hi > lo && vt > lo && g->globals.damage_tags[0] && g->weap && !(u->flags & UF_INVULNERABLE)) {
		int di = weap_find_damage(g->weap, g->globals.damage_tags[0]);
		weap_damage d;
		if (di >= 0 && weap_get_damage(g->weap, (uint32_t)di, &d)) {
			float f = clampf((vt - lo) / (hi - lo), 0.0f, 1.0f);
			game_damage_object(g, u->obj, d.damage_lower + (d.damage_upper[1] - d.damage_lower) * f, NULL);
		}
	}
	if (!busy_action(u->action) && get_biped(g, u, &b) && b.min_soft_landing_velocity > 0.0f && v >= b.min_soft_landing_velocity) {
		unsigned hard = (b.min_hard_landing_velocity > 0.0f && v >= b.min_hard_landing_velocity) ? 1u : 0u;
		start_oneshot(u, g, UA_LAND, hard, 30, 0);
		u->move_dir = (uint8_t)hard;
	}
}

/* Distance LOD (or every unit while `lod_all` is set, in a cinematic): a unit farther than `lod_distance` from the player integrates its body every other tick over a double step (staggered by
 * unit index), halving the sweeps of the crowd that nobody looks at. Returns 1 full rate, 2 the double step, 0 skip this tick. */
static int phys_divider(const units_t *s, const unit_t *u, const game_t *g)
{
	float dx, dy, dz;
	if (u->ctl.flags & UC_JUMP) return 1;
	if (!s->lod_all) {
		if (!(s->lod_distance > 0.0f)) return 1;
		dx = u->body.pos[0] - g->player.pos[0]; dy = u->body.pos[1] - g->player.pos[1]; dz = u->body.pos[2] - g->player.pos[2];
		if (dx * dx + dy * dy + dz * dz <= s->lod_distance * s->lod_distance) return 1;
	}
	return ((g->tick + (uint32_t)(u - s->u)) & 1u) ? 0 : 2;
}

static void tick_unit(units_t *s, unit_t *u, game_t *g, object_t *o)
{
	game_unit_t *gu = &g->unit_state[u->obj];
	unit_control_t *c = &u->ctl;
	uint32_t edge = c->flags & ~u->prev_flags;
	float throttle_scale = 1.0f, turn_left = 0.0f, spd;
	weap_weapon w;
	int have_w = 0;
	unsigned busy;

	if (u->flags & UF_INVULNERABLE) {
		if (gu->dead) { gu->dead = 0; gu->vitals.flags &= ~VITALS_F_DEAD; u->dying = 0; if (u->action == UA_DEAD) { u->action = UA_IDLE; u->anim_key = 0xFFFFu; } }
		if (gu->has_vitals) { gu->vitals.health = gu->vitals.max_health; gu->vitals.shield = gu->vitals.max_shield; gu->vitals.flags &= ~VITALS_F_SHIELD_DOWN; }
	}
	if (u->flags & UF_HIDE_BODY) o->flags |= OBJ_F_HIDDEN;

	/* death */
	if (gu->dead || (gu->has_vitals && vitals_dead(&gu->vitals))) {
		if (!u->dying) begin_death(s, u, g, o);
		if (gu->has_vitals && !gu->dead) gu->dead = 1;
		if (!(u->body.on_ground && fabsf(u->body.vel[0]) + fabsf(u->body.vel[1]) < 1e-3f && u->settle >= SETTLE_PERIOD)) {
			u->ctl.throttle_fwd = u->ctl.throttle_left = 0.0f;
			step_body(u, g, 0, 0.0f, 1, &turn_left);
			if (u->body.on_ground && speed2d(u) < 1e-3f) { if (u->settle < 255) u->settle++; }
			sync_object(u, o);
		}
		u->prev_flags = c->flags;
		return;
	}

	/* damage taken: flinch on a big health hit */
	if (u->flinch_cd) u->flinch_cd--;
	if (gu->has_vitals) {
		float drop = u->hp_prev - gu->vitals.health;
		if (drop > 0.0f && drop >= FLINCH_FRACTION * gu->vitals.max_health && !u->flinch_cd && !busy_action(u->action) && u->action != UA_AIRBORNE) {
			unsigned dir = 0;
			if (u->hit_dir[0] != 0.0f || u->hit_dir[1] != 0.0f) {
				float fx = cosf(u->body.yaw), fy = sinf(u->body.yaw);
				float fwd = u->hit_dir[0] * fx + u->hit_dir[1] * fy, left = -u->hit_dir[0] * fy + u->hit_dir[1] * fx;
				dir = fwd >= 0.7f ? 0u : fwd <= -0.7f ? 3u : left > 0.0f ? 1u : 2u;
			}
			start_oneshot(u, g, UA_FLINCH, dir, 12, 0);
			u->move_dir = (uint8_t)dir;
			u->hit_dir[0] = u->hit_dir[1] = 0.0f;
			emit(s, UE_FLINCH, u, dir, 0, u->body.pos, NULL);
			u->flinch_cd = (uint16_t)(FLINCH_COOLDOWN + u->action_ticks);
			u->stun = (float)u->action_ticks;
		}
		u->hp_prev = gu->vitals.health;
	}
	if (u->stun > 0.0f) u->stun -= 1.0f;

	/* one-shot action countdown: blow, grenade release, end */
	if (u->action == UA_CUSTOM) {
		if (u->action_ticks != HOLD_TICKS) {
			if (u->action_ticks) u->action_ticks--;
			if (!u->action_ticks) { finish_custom(u, o); u->action = UA_IDLE; }
		}
	} else if (u->action == UA_MELEE || u->action == UA_THROW || u->action == UA_LAND || u->action == UA_FLINCH) {
		if (u->action_ticks) u->action_ticks--;
		if (u->event_at && u->action_ticks <= u->event_at) {
			u->event_at = 0;
			if (u->action == UA_MELEE) melee_blow(s, u, g);
			else if (u->action == UA_THROW) throw_grenade(s, u, g);
		}
		if (!u->action_ticks) { u->action = UA_IDLE; u->anim_key = 0xFFFFu; }
	}

	have_w = u->weapon >= 0 && weapon_decode(g, u->weapon, &w);
	busy = busy_action(u->action);

	/* new one-shot actions from the control edges */
	if (!busy && u->action != UA_AIRBORNE) {
		if ((edge & UC_MELEE) && (!have_w || !(w.flags & WEAP_F_NO_MELEE)) && (!have_w || u->wst.mode == WEAP_MODE_IDLE)) {
			start_oneshot(u, g, UA_MELEE, 0, DEFAULT_MELEE_TICKS, 1);
		} else if ((edge & UC_GRENADE) && u->grenades[u->grenade_type] > 0 && g->weap) {
			start_oneshot(u, g, UA_THROW, 0, DEFAULT_THROW_TICKS, 1);
		}
		busy = busy_action(u->action);
	}

	/* movement */
	if (u->action == UA_CUSTOM || u->action == UA_MELEE || u->action == UA_THROW) throttle_scale = 0.0f;
	else if (u->action == UA_LAND) throttle_scale = u->move_dir ? 0.2f : 0.6f;
	else if (u->action == UA_FLINCH) throttle_scale = 0.3f;
	u->posture = u->body.crouch > 0.5f ? 1u : 0u;
	if (!u->posture && (u->weapon < 0 || (u->flags & UF_ALERT)) && !(u->flags & UF_PLAYER)) {   /* an unarmed non-player unit, and any unit a script put in the alert posture (the marines at port arms, ADR-083), stands in the graph's `alert` block when it has one */
		if (!u->alert_state) { ansm_graph agr; u->alert_state = (get_graph(g, u, &agr) && ansm_find_unit(g->ansm, &agr, "alert") >= 0) ? 1u : 2u; }
		if (u->alert_state == 1) u->posture = 2;
	}
	if (!(u->flags & UF_PLAYER) && !s->tag_speeds) refresh_move_speeds(u, g);
	if (u->action == UA_CUSTOM) {
		/* a script's animation owns the body: it stands exactly where object_teleport put it (cryo bay crew: the flag is 7 cm below the floor the ground physics found, they stood on tiptoe
		 * and their forearms went through the console); the animation's root motion moves it, the ground check resumes when the animation ends (ADR-089) */
		turn_left = 0.0f;
		u->body.vel[0] = u->body.vel[1] = u->body.vel[2] = 0.0f;
		if (!u->body.on_ground) { u->body.on_ground = 1; u->body.ground_normal[0] = u->body.ground_normal[1] = 0.0f; u->body.ground_normal[2] = 1.0f; }
		u->air = 0;
	} else step_body(u, g, u->action != UA_CUSTOM, throttle_scale, phys_divider(s, u, g), &turn_left);
	if (u->body.landed_speed > 0.0f) { landed(s, u, g); u->body.landed_speed = 0.0f; }
	spd = speed2d(u);
	if (u->body.on_ground) { if (u->settle > 0 && spd > MOVING_SPEED) u->settle = 0; }
	if (gu->dead || (gu->has_vitals && vitals_dead(&gu->vitals))) { u->prev_flags = c->flags; sync_object(u, o); return; }   /* died of the fall */

	/* weapon */
	if (have_w) {
		weap_input wi;
		weap_events wev;
		unsigned m;
		memset(&wi, 0, sizeof wi);
		for (m = 0; m < w.magazine_count && m < WEAP_MAX_MAGAZINES; m++)       /* an actor never runs out of magazines */
			if (u->wst.reserve[m] < w.magazine[m].rounds_loaded_max) u->wst.reserve[m] = w.magazine[m].rounds_loaded_max;
		busy = busy_action(u->action);
		wi.fire[0] = (c->flags & UC_FIRE) && !busy;
		wi.fire[1] = (c->flags & UC_FIRE2) && !busy;
		wi.reload = (edge & UC_RELOAD) != 0;
		if (u->rate_mult != 1.0f && (wi.fire[0] || wi.fire[1])) {          /* rate of fire of the difficulty table */
			unsigned t;
			for (t = 0; t < w.trigger_count; t++) { w.trigger[t].rps_min *= u->rate_mult; w.trigger[t].rps_max *= u->rate_mult; }
		}
		weapon_fire_step(&u->wst, &w, &wi, &wev);
		{
			unsigned t, any = 0;
			for (t = 0; t < w.trigger_count; t++) any |= wev.shot[t];
			if (any) {
				shoot(s, u, g, &w, &wev);
				{
					ansm_graph gr;
					unsigned ticks = FIRE_HOLD_DEFAULT;
					if (get_graph(g, u, &gr)) { unsigned tk = anim_ticks(g, &gr, lookup_action(g, u, &gr, UA_FIRE, 0, 0.0f, NULL), NULL); if (tk > 0 && tk < 20) ticks = tk; }
					u->fire_hold = (uint16_t)ticks;
				}
			}
		}
		if (wev.flags & WEAP_EV_RELOAD_START) {
			ansm_graph gr;
			emit(s, UE_RELOAD, u, 0, w.tag_id, u->body.pos, NULL);
			unsigned ticks = 0;
			u->action_ticks = u->wst.timer;
			if (get_graph(g, u, &gr)) ticks = anim_ticks(g, &gr, lookup_action(g, u, &gr, UA_RELOAD, 0, 0.0f, NULL), NULL);
			u->anim_rate = (ticks > 0 && u->wst.timer > 0) ? clampf((float)ticks / (float)u->wst.timer, 0.5f, 2.0f) : 1.0f;
			u->anim_key = 0xFFFFu;
		}
	}
	if (u->fire_hold) u->fire_hold--;

	/* the base action */
	if (u->body.on_ground) u->air = 0; else if (u->air < 255) u->air++;
	if (!busy_action(u->action)) {
		if (u->air >= 3) u->action = UA_AIRBORNE;
		else if (have_w && u->wst.mode == WEAP_MODE_RELOADING) u->action = UA_RELOAD;
		else if (u->fire_hold) u->action = UA_FIRE;
		else u->action = spd > MOVING_SPEED ? UA_MOVE : UA_IDLE;
	}
	if (u->action == UA_RELOAD) u->action_ticks = u->wst.timer;

	/* animation */
	if (u->action == UA_CUSTOM) {
		if (u->custom_frame_scale != 1.0f && u->custom_anim >= 0 && (o->flags & OBJ_F_ANIMATED)) {
			o->frame += u->custom_frame_scale - 1.0f;
			if (o->frame < 0.0f) o->frame = 0.0f;
		}
		custom_root_motion(u, o, g);
	} else anim_update(u, o, g, spd, turn_left);
	{                                                  /* the aim relative to the body, as a fraction of the aiming screen: the aim-still overlay of an armed unit (spine, arms, head), the `look` overlay of the head for the others (apply_grid) */
		int live = g_units_look && !(u->flags & UF_PLAYER) && u->action != UA_CUSTOM && u->action != UA_DEAD && !u->dying;
		float ry = wrap_pi(u->ctl.aim_yaw - u->body.yaw), rp = u->ctl.aim_pitch;
		o->face.look_yaw = o->face.look_pitch = o->face.aim_yaw = o->face.aim_pitch = 0;
		if (live) refresh_aim_overlay(u, g);
		if (live && u->aim_k >= 0) {
			float fy = ry >= 0.0f ? (u->aim_range[1] > 1e-3f ? ry / u->aim_range[1] : 0.0f) : (u->aim_range[0] > 1e-3f ? ry / u->aim_range[0] : 0.0f);
			float fp = rp >= 0.0f ? (u->aim_range[3] > 1e-3f ? rp / u->aim_range[3] : 0.0f) : (u->aim_range[2] > 1e-3f ? rp / u->aim_range[2] : 0.0f);
			if (!o->face.graph) o->face.graph = (int16_t)(o->graph + 1);
			o->face.aim_anim = (int16_t)(u->aim_k + 1);
			o->face.aim_yaw = (int8_t)(clampf(fy, -1.0f, 1.0f) * 127.0f); o->face.aim_pitch = (int8_t)(clampf(fp, -1.0f, 1.0f) * 127.0f);
		} else if (live && o->face.look_anim > 0) {
			float fy, fp;
			if (u->look_range[0] < 0.0f) {                  /* the unit block's aiming screen (marines: 60 right, 20 left, 30 down, 60 up), read once */
				ansm_graph lg;
				ansm_unit ui;
				int ux;
				u->look_range[0] = u->look_range[1] = AIM_YAW_LIMIT; u->look_range[2] = u->look_range[3] = AIM_PITCH_LIMIT;
				if (get_graph(g, u, &lg) && (ux = ansm_find_unit(g->ansm, &lg, "stand")) >= 0 && ansm_unit_info(g->ansm, &lg, (uint32_t)ux, &ui) == 0) ansm_aim_ranges(&ui.aim, u->look_range);
				if (u->look_range[0] < 0.0f) u->look_range[0] = 0.0f;
			}
			fy = ry >= 0.0f ? (u->look_range[1] > 1e-3f ? ry / u->look_range[1] : 0.0f) : (u->look_range[0] > 1e-3f ? ry / u->look_range[0] : 0.0f);
			fp = rp >= 0.0f ? (u->look_range[3] > 1e-3f ? rp / u->look_range[3] : 0.0f) : (u->look_range[2] > 1e-3f ? rp / u->look_range[2] : 0.0f);
			o->face.look_yaw = (int8_t)(clampf(fy, -1.0f, 1.0f) * 127.0f); o->face.look_pitch = (int8_t)(clampf(fp, -1.0f, 1.0f) * 127.0f);
		}
	}

	u->prev_flags = c->flags;
	sync_object(u, o);
}

void units_tick(units_t *s, game_t *g)
{
	unsigned i, nev;
	const game_event_t *ev;
	if (!s || !g || !g->objects) return;
	s->ev_count = 0;
	/* attackers of the last game tick: the impact of a shot carries the way back to the shooter */
	ev = game_events(g, &nev);
	for (i = 0; i < nev; i++) {
		if (ev[i].type == GE_IMPACT && ev[i].obj >= 0 && ev[i].obj < OBJ_MAX && s->by_obj[ev[i].obj] >= 0) {
			unit_t *t = &s->u[s->by_obj[ev[i].obj]];
			float nx = ev[i].normal[0], ny = ev[i].normal[1], l = sqrtf(nx * nx + ny * ny);
			if (l > 1e-4f) { t->hit_dir[0] = nx / l; t->hit_dir[1] = ny / l; }
		}
	}
	for (i = 0; i < s->count; i++) {
		unit_t *u = &s->u[i];
		object_t *o;
		if (!u->active) continue;
		o = OBJ(g, u);
		if (!o) { units_detach(s, (int)i); continue; }
		if ((o->flags & OBJ_F_INACTIVE) || (u->flags & UF_PLAYER)) continue;
		tick_unit(s, u, g, o);
	}
}

/* ---- custom animations -------------------------------------------------------------------------------------------------------- */
unsigned units_custom_animation(units_t *s, game_t *g, int unit, int graph, int anim, float frame_scale, int hold)
{
	unit_t *u = U(s, unit);
	object_t *o;
	anim_graph ag;
	anim_info ai;
	int gi;
	unsigned ticks;
	if (!u || u->dying || !(o = OBJ(g, u)) || !g->ow.anims || anim < 0) return 0;
	gi = graph >= 0 ? graph : u->base_graph;
	if (gi < 0 || (unsigned)gi >= g->ow.anims->graph_count) return 0;
	ag = anim_get_graph(g->ow.anims, (uint32_t)gi);
	if ((unsigned)anim >= ag.anim_count) return 0;
	ai = anim_get(g->ow.anims, &ag, (uint32_t)anim);
	if (!(frame_scale > 0.0f) || frame_scale > 16.0f) frame_scale = 1.0f;
	ticks = (unsigned)((float)ai.frame_count / (frame_scale * (ai.fps > 0.0f ? ai.fps / TICKS : 1.0f)) + 0.5f);
	if (!ticks) ticks = 1;
	if (ticks > 65000u) ticks = 65000u;
	if ((ai.flags & ANIM_LOOPS) && strstr(ai.name, "driver") && strstr(ai.name, "idle")) hold = 1;       /* a seat idle ("CT-driver unarmed idle": the Chief in his cryo tube) goes on until the script changes it */
	o->graph = (int16_t)gi;
	o->anim = (uint16_t)anim; o->frame = 0.0f; o->flags |= OBJ_F_ANIMATED;
	u->custom_graph = (int16_t)(gi == u->base_graph ? -1 : gi);
	u->custom_anim = (int16_t)anim;
	u->custom_frame_scale = frame_scale;
	u->rm_frame = 0;
	u->action = UA_CUSTOM;
	u->action_ticks = hold ? HOLD_TICKS : (uint16_t)ticks;
	u->event_at = 0; u->fire_hold = 0;
	u->anim_key = 0xFFFFu; u->anim_cur = (int16_t)anim;
	return ticks;
}

void units_stop_custom(units_t *s, game_t *g, int unit)
{
	unit_t *u = U(s, unit);
	object_t *o;
	if (!u || u->action != UA_CUSTOM || !(o = OBJ(g, u))) return;
	finish_custom(u, o);
	u->action = UA_IDLE;
	u->action_ticks = 0;
}
