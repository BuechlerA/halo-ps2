#include <stdio.h>
/* Script host H1: object, unit, player, device and effect built-ins (docs/33-host-objects.md). Pure C11, no heap, no double.
 * Conventions: sim/mission.h (handle encodings: runtime object handles carry MISSION_OBJ_BIT, constants are object-name indices).
 * A built-in never fails: an unresolved handle makes it a no-op, and none of these block (the original blocks in none of them: the
 * scripts wait with sleep / sleep_until on the values they return). */
#include "script/hs_host_obj.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

/* ---- the built-ins of this module (X-macro: enum ids, functions and the registration table come from one list) ---- */
#define OBJ_LIST(X) \
	X(players) X(list_get) X(list_count) X(volume_test_objects) X(volume_test_objects_all) X(volume_test_object) \
	X(volume_return_objects) X(volume_teleport_players_not_inside) \
	X(object_create) X(object_create_anew) X(object_create_containing) X(object_create_anew_containing) \
	X(object_destroy) X(object_destroy_containing) X(object_destroy_all) X(object_teleport) X(object_set_facing) \
	X(object_beautify) X(objects_attach) X(objects_detach) X(objects_can_see_object) X(objects_can_see_flag) \
	X(objects_distance_to_object) X(objects_distance_to_flag) X(object_get_health) X(object_set_scale) \
	X(object_set_permutation) X(object_set_collideable) X(object_cannot_take_damage) X(object_can_take_damage) \
	X(object_set_melee_attack_inhibited) X(object_set_ranged_attack_inhibited)
#define UNIT_LIST(X) \
	X(unit) X(unit_get_health) X(unit_get_shield) X(unit_get_total_grenade_count) X(unit_set_current_vitality) \
	X(units_set_current_vitality) X(unit_set_maximum_vitality) X(unit_kill) X(unit_kill_silent) X(unit_open) X(unit_close) \
	X(unit_set_emotion) X(unit_set_enterable_by_player) X(unit_doesnt_drop_items) X(unit_impervious) X(unit_suspended) \
	X(unit_set_seat) X(unit_enter_vehicle) X(unit_exit_vehicle) X(vehicle_load_magic) X(vehicle_unload) X(vehicle_riders) \
	X(vehicle_test_seat_list) X(vehicle_hover) X(unit_stop_custom_animation) X(unit_get_custom_animation_time) X(custom_animation) \
	X(unit_custom_animation_at_frame) X(custom_animation_list) X(unit_has_weapon_readied) \
	X(unit_solo_player_integrated_night_vision_is_active) X(units_set_desired_flashlight_state)
#define PLAYER_LIST(X) \
	X(player_enable_input) X(player_camera_control) X(player_action_test_reset) X(player_action_test_accept) \
	X(player_action_test_primary_trigger) X(player_action_test_grenade_trigger) X(player_action_test_action) \
	X(player_action_test_back) X(player_action_test_look_relative_up) X(player_action_test_look_relative_down) \
	X(player_action_test_look_relative_left) X(player_action_test_look_relative_right) \
	X(player_action_test_look_relative_all_directions) X(player0_joystick_set_is_normal) X(player0_look_invert_pitch) \
	X(player0_look_pitch_is_inverted) X(player_add_equipment) X(players_unzoom_all)
#define DEVICE_LIST(X) \
	X(device_get_position) X(device_get_power) X(device_set_position) X(device_set_position_immediate) X(device_set_power) \
	X(device_group_set) X(device_group_set_immediate) X(device_group_get) X(device_one_sided_set) \
	X(device_operates_automatically_set) X(device_set_never_appears_locked) X(device_group_change_only_once_more_set)
#define FX_LIST(X) \
	X(effect_new) X(effect_new_on_object_marker) X(damage_new) X(damage_object) \
	X(recording_play) X(recording_play_and_delete) X(recording_play_and_hover) X(recording_kill) X(recording_time)
#define ALL_LIST(X) OBJ_LIST(X) UNIT_LIST(X) PLAYER_LIST(X) DEVICE_LIST(X) FX_LIST(X)

#define X_ID(name) ID_##name,
enum { ALL_LIST(X_ID) ID_COUNT };
static uint32_t g_calls[ID_COUNT];

#define BI(name) static int bi_##name(hs_machine *hm, void *host, const hs_value *a, unsigned n, hs_value *r)
#define ENTER(name) mission_t *M = (mission_t *)host; (void)M; (void)hm; (void)a; (void)n; (void)r; g_calls[ID_##name]++

/* ---- argument and result helpers ---------------------------------------------------------------------------------------- */
static const hs_value NIL = { HS_VOID, 0, 0, { 0 } };
#define A(i) ((i) < n ? &a[i] : &NIL)

static int f2i(float f)
{
	if (!(f == f)) return 0;
	if (f >= 2147483520.0f) return 2147483647;
	if (f <= -2147483648.0f) return (-2147483647 - 1);
	return (int)f;
}
static float argf(const hs_value *v) { return v->type == HS_REAL ? v->v.f : (float)v->v.i; }
static int argi(const hs_value *v) { return v->type == HS_REAL ? f2i(v->v.f) : v->v.i; }
static int argb(const hs_value *v) { return v->type == HS_REAL ? v->v.f != 0.0f : v->v.i != 0; }

static void ret_num(hs_value *r, float f)
{
	if (r->type == HS_REAL) r->v.f = f;
	else if (r->type == HS_BOOL) r->v.i = f != 0.0f;
	else if (r->type == HS_SHORT || r->type == HS_LONG) r->v.i = f2i(f);
	else { r->type = HS_REAL; r->v.f = f; }
}
static void ret_bool(hs_value *r, int b) { if (r->type == HS_REAL) r->v.f = b ? 1.0f : 0.0f; else r->v.i = b != 0; }
static void ret_obj(hs_value *r, int slot) { r->v.h = mission_obj_handle(slot); }

static const char *str_arg(hs_machine *hm, const hs_value *v)
{
	if (v->type != HS_STRING || !hm->sec) return "";
	return hs_string(hm->sec, v->v.i);
}

static uint16_t hash16(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
	return (uint16_t)(h ^ (h >> 16));
}

/* bit sets over object slots */
static int bit_get(const uint32_t *b, int i) { return (b[i >> 5] >> (i & 31)) & 1u; }
static void bit_set(uint32_t *b, int i, int v) { if (v) b[i >> 5] |= 1u << (i & 31); else b[i >> 5] &= ~(1u << (i & 31)); }

/* ---- object helpers ----------------------------------------------------------------------------------------------------- */
static object_t *obj_of(mission_t *M, int slot) { return mission_object_live(M, slot) ? &M->objects->obj[slot] : NULL; }
static int unit_of(const mission_t *M, int slot) { return (M->units && slot >= 0) ? units_of_object(M->units, slot) : -1; }
static int is_player(const mission_t *M, int slot) { return slot >= 0 && slot == M->player_obj; }

static void ensure_unit(mission_t *M, int slot)
{
	const object_t *o = obj_of(M, slot);
	if (o && o->type == SCEN_BIPED && M->units && M->game && units_of_object(M->units, slot) < 0 && !is_player(M, slot))
		units_attach(M->units, M->game, slot, 0, 0);
}

/* The object an argument names (a runtime handle, a name, a list's first element); `create` spawns a named placement on demand. */
static int obj_arg(mission_t *M, const hs_value *v, int create)
{
	int slot = mission_resolve_object(M, v, create);
	if (slot >= 0) ensure_unit(M, slot);
	return slot;
}

static float obj_yaw(const mission_t *M, int slot)
{
	const object_t *o = &M->objects->obj[slot];
	int ui = unit_of(M, slot);
	if (is_player(M, slot) && M->game) return M->game->player.yaw;
	if (ui >= 0) return M->units->u[ui].body.yaw;
	return atan2f(o->root.m[0][1], o->root.m[0][0]);
}

static void obj_set_transform(mission_t *M, int slot, const float pos[3], float yaw)
{
	object_t *o = obj_of(M, slot);
	int ui;
	if (!o) return;
	if (is_player(M, slot) && M->game) { game_teleport(M->game, pos, yaw); mission_sync_player(M); return; }
	ui = unit_of(M, slot);
	if (ui >= 0 && M->game) units_teleport(M->units, M->game, ui, pos, yaw);
	o->pos[0] = pos[0]; o->pos[1] = pos[1]; o->pos[2] = pos[2];
	mat4_from_ypr(&o->root, yaw, 0.0f, 0.0f, o->pos);
}

static void list_of(mission_t *M, hs_value *r, mission_list **out)
{
	uint32_t h;
	*out = mission_list_new(M, &h);
	r->v.h = h;
}

/* A mobile thing a script may address as a unit: biped / vehicle with a unit, or the player. */
static int is_mobile(const object_t *o) { return o->type == SCEN_BIPED || o->type == SCEN_VEHICLE; }

static void forget_cine(mission_t *M, int slot)
{
	int ps;
	if (!M->cine) return;
	while ((ps = cine_play_find(M->cine, (uint32_t)slot)) >= 0) cine_play_kill(M->cine, ps);
}

void hs_host_obj_forget(mission_t *M, int slot)
{
	mission_hostobj *h = &M->hobj;
	uint32_t i, k;
	if (slot < 0 || slot >= OBJ_MAX) return;
	for (i = 0; i < h->attach_count;) {
		if (h->attach[i].parent == slot || h->attach[i].child == slot) h->attach[i] = h->attach[--h->attach_count];
		else i++;
	}
	for (i = 0; i < h->suspend_count;) {
		if (h->suspend[i].obj == slot) h->suspend[i] = h->suspend[--h->suspend_count]; else i++;
	}
	bit_set(h->cannot_take_damage, slot, 0); bit_set(h->hid_vitals, slot, 0); bit_set(h->melee_inhibited, slot, 0);
	bit_set(h->ranged_inhibited, slot, 0); bit_set(h->no_drop, slot, 0); bit_set(h->not_enterable, slot, 0);
	h->seat_vehicle[slot] = 0; h->seat_hash[slot] = 0; h->saved_graph[slot] = 0;
	for (k = 0; k < OBJ_MAX; k++) if (h->seat_vehicle[k] == slot + 1) h->seat_vehicle[k] = 0;
	forget_cine(M, slot);
}

static void destroy_obj(mission_t *M, int slot)
{
	if (!mission_object_live(M, slot) || is_player(M, slot)) return;
	hs_host_obj_forget(M, slot);
	mission_destroy_object(M, slot);
}

/* ---- trigger volumes ---------------------------------------------------------------------------------------------------- */
static int vol_arg(const mission_t *M, const hs_value *v, devdef_volume *out, unsigned *index)
{
	if (!M->dev || v->type != HS_HANDLE || v->v.h >= M->dev->volume_count) return 0;
	*out = devdef_get_volume(M->dev, v->v.h);
	*index = v->v.h;
	return 1;
}

/* mode 0: any object inside, 1: all objects inside (empty: false). Reports the awaited volume when the player's body is in the list. */
static int volume_test(mission_t *M, const hs_value *vol, const mission_list *l, int all)
{
	devdef_volume v;
	unsigned vi, i, inside = 0, live = 0;
	int player_in_list = 0, player_inside = 0;
	if (!l || !vol_arg(M, vol, &v, &vi)) return 0;
	for (i = 0; i < l->count; i++) {
		float p[3];
		int slot = l->obj[i], in;
		if (!mission_object_live(M, slot)) continue;
		mission_object_pos(M, slot, p);
		in = volume_contains(&v, p);
		live++;
		inside += (unsigned)in;
		if (is_player(M, slot)) { player_in_list = 1; player_inside = in; }
	}
	if (player_in_list) mission_note_awaited_volume(M, vi, player_inside);
	return all ? (live > 0 && inside == live) : inside > 0;
}

BI(players)
{
	mission_list *l;
	ENTER(players);
	list_of(M, r, &l);
	if (M->player_obj >= 0 && mission_object_live(M, M->player_obj)) mission_list_add(l, M->player_obj);
	return 0;
}

BI(list_get)
{
	mission_list tmp;
	const mission_list *l;
	int i;
	ENTER(list_get);
	r->v.h = HS_HANDLE_NONE;
	l = mission_arg_list(M, A(0), &tmp);
	i = argi(A(1));
	if (l && i >= 0 && (unsigned)i < l->count && mission_object_live(M, l->obj[i])) ret_obj(r, l->obj[i]);
	return 0;
}

BI(list_count)
{
	mission_list tmp;
	const mission_list *l;
	unsigned i, c = 0;
	ENTER(list_count);
	l = mission_arg_list(M, A(0), &tmp);
	for (i = 0; l && i < l->count; i++) c += (unsigned)mission_object_live(M, l->obj[i]);
	ret_num(r, (float)c);
	return 0;
}

BI(volume_test_objects)
{
	mission_list tmp;
	ENTER(volume_test_objects);
	ret_bool(r, volume_test(M, A(0), mission_arg_list(M, A(1), &tmp), 0));
	return 0;
}

BI(volume_test_objects_all)
{
	mission_list tmp;
	ENTER(volume_test_objects_all);
	ret_bool(r, volume_test(M, A(0), mission_arg_list(M, A(1), &tmp), 1));
	return 0;
}

BI(volume_test_object)
{
	mission_list one;
	int slot;
	ENTER(volume_test_object);
	one.count = 0;
	slot = obj_arg(M, A(1), 0);
	if (slot >= 0) mission_list_add(&one, slot);
	ret_bool(r, volume_test(M, A(0), &one, 0));
	return 0;
}

BI(volume_return_objects)
{
	devdef_volume v;
	unsigned vi, i;
	mission_list *l;
	ENTER(volume_return_objects);
	list_of(M, r, &l);
	if (!vol_arg(M, A(0), &v, &vi)) return 0;
	for (i = 0; M->objects && i < M->objects->count; i++) {
		const object_t *o = &M->objects->obj[i];
		float p[3];
		if (o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) continue;
		if (!is_mobile(o) && o->type != SCEN_WEAPON && o->type != SCEN_EQUIPMENT) continue;
		mission_object_pos(M, (int)i, p);
		if (volume_contains(&v, p)) mission_list_add(l, (int)i);
	}
	return 0;
}

static int flag_arg(const mission_t *M, const hs_value *v, devdef_cflag *out)
{
	if (!M->dev || v->type != HS_HANDLE || v->v.h >= M->dev->flag_count) return 0;
	*out = devdef_get_cflag(M->dev, v->v.h);
	return 1;
}

BI(volume_teleport_players_not_inside)
{
	devdef_volume v;
	devdef_cflag f;
	unsigned vi;
	float p[3];
	ENTER(volume_teleport_players_not_inside);
	if (!M->game || !vol_arg(M, A(0), &v, &vi) || !flag_arg(M, A(1), &f)) return 0;
	p[0] = M->game->player.pos[0]; p[1] = M->game->player.pos[1]; p[2] = M->game->player.pos[2] + 0.3f;
	if (!volume_contains(&v, p)) { game_teleport(M->game, f.pos, f.yaw); mission_sync_player(M); }
	return 0;
}

/* ---- creating and destroying objects ------------------------------------------------------------------------------------ */
BI(object_create)
{
	ENTER(object_create);
	(void)obj_arg(M, A(0), 1);
	return 0;
}

BI(object_create_anew)
{
	int slot;
	ENTER(object_create_anew);
	slot = obj_arg(M, A(0), 0);
	if (slot >= 0) destroy_obj(M, slot);
	(void)obj_arg(M, A(0), 1);
	return 0;
}

/* Every object name that contains `sub`: mode 0 create, 1 destroy, 2 destroy and create again. */
static void by_containing(mission_t *M, const char *sub, int mode)
{
	uint32_t i;
	if (!M->dev || !sub[0]) return;
	for (i = 0; i < M->dev->name_count; i++) {
		devdef_name nm = devdef_get_name(M->dev, i);
		int slot;
		if (!(nm.flags & DEVDEF_NF_RESOLVED) || !strstr(nm.name, sub)) continue;
		slot = mission_object_by_name(M, i, 0);
		if (mode >= 1 && slot >= 0) destroy_obj(M, slot);
		if (mode != 1) { slot = mission_object_by_name(M, i, 1); if (slot >= 0) ensure_unit(M, slot); }
	}
}

BI(object_create_containing) { ENTER(object_create_containing); by_containing(M, str_arg(hm, A(0)), 0); return 0; }
BI(object_destroy_containing) { ENTER(object_destroy_containing); by_containing(M, str_arg(hm, A(0)), 1); return 0; }
BI(object_create_anew_containing) { ENTER(object_create_anew_containing); by_containing(M, str_arg(hm, A(0)), 2); return 0; }

BI(object_destroy)
{
	ENTER(object_destroy);
	destroy_obj(M, obj_arg(M, A(0), 0));
	return 0;
}

BI(object_destroy_all)
{
	unsigned i;
	ENTER(object_destroy_all);
	/* the mobile things and items; scenery, devices and lights belong to the level and stay (docs/33) */
	for (i = 0; M->objects && i < M->objects->count; i++) {
		const object_t *o = &M->objects->obj[i];
		if (o->flags & OBJ_F_FREE) continue;
		if (o->type == SCEN_BIPED || o->type == SCEN_VEHICLE || o->type == SCEN_WEAPON || o->type == SCEN_EQUIPMENT || o->type == SCEN_PROJECTILE)
			destroy_obj(M, (int)i);
	}
	return 0;
}

BI(object_teleport)
{
	int slot;
	devdef_cflag f;
	ENTER(object_teleport);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0 || !flag_arg(M, A(1), &f)) return 0;
	{
		const object_t *o = &M->objects->obj[slot];
		if (o->type == SCEN_MACHINE || o->type == SCEN_CONTROL) return 0;      /* devices do not move */
	}
	obj_set_transform(M, slot, f.pos, f.yaw);
	return 0;
}

BI(object_set_facing)
{
	int slot;
	devdef_cflag f;
	ENTER(object_set_facing);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0 || !flag_arg(M, A(1), &f)) return 0;
	{
		const object_t *o = &M->objects->obj[slot];
		float p[3] = { o->pos[0], o->pos[1], o->pos[2] };
		float dx = f.pos[0] - p[0], dy = f.pos[1] - p[1];
		/* "face the flag": turn towards the flag's position, which is how scripts use it (the flag's own yaw is not a target) */
		obj_set_transform(M, slot, p, (dx * dx + dy * dy > 1e-6f) ? atan2f(dy, dx) : f.yaw);
	}
	return 0;
}

BI(object_beautify) { ENTER(object_beautify); return 0; }

/* ---- attachments ---------------------------------------------------------------------------------------------------------- */
static int marker_index(const mission_t *M, const object_t *o, const char *name)
{
	model_info mi;
	if (!name[0] || o->model < 0 || !M->ow.models) return -1;
	mi = model_get(M->ow.models, (uint32_t)o->model);
	return model_find_marker(M->ow.models, &mi, name);
}

/* World matrix of marker `marker` (-1 = origin) of an object in its current pose. */
static int marker_world(const mission_t *M, const object_t *o, int marker, mat4 *out)
{
	static object_nodes_t nodes;
	*out = o->root;
	if (marker < 0 || o->model < 0 || !M->ow.models) return marker < 0;
	{
		model_info mi = model_get(M->ow.models, (uint32_t)o->model);
		model_marker mk;
		unsigned cnt = objects_pose_world(o, &M->ow, &nodes);
		mat4 local;
		if ((unsigned)marker >= mi.marker_count) return 0;
		mk = model_get_marker(M->ow.models, &mi, (uint32_t)marker);
		if (mk.node < 0 || (unsigned)mk.node >= cnt) return 0;
		mat4_from_trs(&local, mk.rotation, mk.translation, 1.0f);
		mat4_mul(out, &local, &nodes.world[mk.node]);
	}
	return 1;
}

BI(objects_attach)
{
	int parent, child, mk;
	uint32_t i;
	mission_hostobj *h;
	ENTER(objects_attach);
	parent = obj_arg(M, A(0), 0);
	child = obj_arg(M, A(2), 0);
	if (parent < 0 || child < 0 || parent == child) return 0;
	h = &M->hobj;
	mk = marker_index(M, &M->objects->obj[parent], str_arg(hm, A(1)));
	for (i = 0; i < h->attach_count; i++) if (h->attach[i].child == child) break;     /* an object hangs on one parent */
	if (i == h->attach_count) {
		if (h->attach_count >= MISSION_ATTACH_MAX) { mission_note_missing(M, "objects_attach"); return 0; }
		h->attach_count++;
	}
	h->attach[i].parent = (int16_t)parent; h->attach[i].child = (int16_t)child; h->attach[i].marker = (int16_t)mk; h->attach[i].pad = 0;
	return 0;
}

BI(objects_detach)
{
	int parent, child;
	uint32_t i;
	mission_hostobj *h;
	ENTER(objects_detach);
	h = &M->hobj;
	parent = obj_arg(M, A(0), 0);
	child = obj_arg(M, A(1), 0);
	for (i = 0; i < h->attach_count;) {
		if ((child < 0 || h->attach[i].child == child) && (parent < 0 || h->attach[i].parent == parent) && (parent >= 0 || child >= 0))
			h->attach[i] = h->attach[--h->attach_count];
		else i++;
	}
	return 0;
}

/* Weapons in hands: an armed unit gets a weapon object (the weapon's own model) whose "right hand" marker (else its origin) sits on the unit's
 * "right hand" marker in the unit's current pose, so a marine in a cinematic holds his rifle like the Xbox's. The object is hidden for a dead
 * unit and destroyed when the weapon is gone; OBJ_F_HELD keeps the player from picking it up. */
static int marker_by_names(const mission_t *M, const object_t *o, const char *a, const char *b)
{
	int mk = marker_index(M, o, a);
	return mk >= 0 || !b ? mk : marker_index(M, o, b);
}

/* What a held weapon needs per tick, kept across ticks: the marker indices (a string search each) and, for a weapon with no animation of its own, the inverse of its grip
 * marker in its own pose (a constant). Rebuilt when the unit's weapon slot or either model changes. */
typedef struct { int slot; int32_t pmodel, cmodel; uint32_t serial; int pm, cm, have_inv; mat4 inv; } held_cache_t;
static held_cache_t s_held_cache[UNIT_MAX];

static void update_held_weapons(mission_t *M)
{
	mission_hostobj *h = &M->hobj;
	unsigned ui;
	if (!M->units || !M->game || !M->game->weap || !M->ow.scen) return;
	for (ui = 0; ui < UNIT_MAX; ui++) {
		const unit_t *u = &M->units->u[ui];
		int slot = h->held[ui] - 1;
		object_t *po, *co;
		weap_weapon wd;
		mat4 hand, out;
		held_cache_t *hc = &s_held_cache[ui];
		if (!u->active || u->weapon < 0 || u->obj < 0 || is_player(M, u->obj) || !(po = obj_of(M, u->obj)) || po->model < 0
		    || !weap_get_weapon(M->game->weap, (uint32_t)u->weapon, &wd)) {
			if (slot >= 0 && mission_object_live(M, slot)) { M->objects->obj[slot].flags |= OBJ_F_HIDDEN; objects_destroy(M->objects, (unsigned)slot); }
			h->held[ui] = 0;
			continue;
		}
		if (slot < 0 || !mission_object_live(M, slot) || !(M->objects->obj[slot].flags & OBJ_F_HELD)) {
			int def = scen_find_def(M->ow.scen, wd.tag_id);
			if (def < 0) { h->held[ui] = 0; continue; }
			slot = objects_spawn_def(M->objects, &M->ow, (unsigned)def, po->pos, 0.0f);
			if (slot < 0) { h->held[ui] = 0; continue; }
			M->objects->obj[slot].flags |= OBJ_F_HELD;
			h->held[ui] = (int16_t)(slot + 1);
		}
		co = &M->objects->obj[slot];
		if (M->game->unit_state[u->obj].dead || (po->flags & OBJ_F_HIDDEN)) co->flags |= OBJ_F_HIDDEN; else co->flags &= ~OBJ_F_HIDDEN;
		if (hc->slot != slot + 1 || hc->pmodel != po->model || hc->cmodel != co->model || hc->serial != co->serial) {
			hc->slot = slot + 1; hc->pmodel = po->model; hc->cmodel = co->model; hc->serial = co->serial;
			hc->pm = marker_by_names(M, po, "right hand", NULL);
			hc->cm = marker_by_names(M, co, "right hand", "primary trigger");
			hc->have_inv = 0;
			if (hc->cm >= 0 && !(co->flags & (OBJ_F_ANIMATED | OBJ_F_POSED))) {
				mat4 gl;
				co->root = (mat4){ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };
				if (marker_world(M, co, hc->cm, &gl)) { mat4_rigid_inverse(&hc->inv, &gl); hc->have_inv = 1; }
			}
		}
		if (hc->pm < 0 || !marker_world(M, po, hc->pm, &hand)) continue;
		if (hc->have_inv) {
			mat4_mul(&out, &hc->inv, &hand);
		} else if (hc->cm >= 0) {                                    /* the weapon's grip marker (in its own pose) lands on the hand */
			mat4 gl;
			co->root = (mat4){ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };
			if (marker_world(M, co, hc->cm, &gl)) { mat4 inv; mat4_rigid_inverse(&inv, &gl); mat4_mul(&out, &inv, &hand); } else out = hand;
		} else out = hand;
		co->root = out;
		co->pos[0] = out.m[3][0]; co->pos[1] = out.m[3][1]; co->pos[2] = out.m[3][2];
	}
}

/* Particle systems and looping effects attached to objects (scenery steam jets, smoke and fire emitters): the FXAT rows of the object's tag, at the row's marker
 * (the object's origin when the marker name is empty or unknown), along the marker's forward axis. */
static void update_object_fx(mission_t *M)
{
	unsigned i;
	if (!M->fx || !M->fx->attach_count || !M->ow.scen) return;
	for (i = 0; i < M->objects->count && i < 1280u; i++) {
		object_t *o = &M->objects->obj[i];
		uint32_t first, k, n;
		scen_def d;
		if (o->model < 0 || (o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE | OBJ_F_HIDDEN))) continue;
		d = scen_get_def(M->ow.scen, (uint32_t)o->def);
		n = fx_attachments_of(M->fx, d.tag_id, &first);
		for (k = 0; k < n && k < 8; k++) {
			fx_attach_row r = fx_attachment(M->fx, first + k);
			float pos[3] = { o->pos[0], o->pos[1], o->pos[2] }, dir[3] = { o->root.m[0][0], o->root.m[0][1], o->root.m[0][2] };
			int mk = r.marker[0] ? marker_index(M, o, r.marker) : -1;
			mat4 w;
			if (mk >= 0 && marker_world(M, o, mk, &w)) {
				pos[0] = w.m[3][0]; pos[1] = w.m[3][1]; pos[2] = w.m[3][2];
				dir[0] = w.m[0][0]; dir[1] = w.m[0][1]; dir[2] = w.m[0][2];
			}
			fx_attach_update(M->fx, (int32_t)((i * 8u + k) | (uint32_t)o->serial << 16), first + k, pos, dir);
		}
	}
}

static void update_attachments(mission_t *M)
{
	mission_hostobj *h = &M->hobj;
	uint32_t i;
	for (i = 0; i < h->attach_count;) {
		mission_attach at = h->attach[i];
		object_t *po = obj_of(M, at.parent), *co = obj_of(M, at.child);
		mat4 w;
		if (!po || !co) { h->attach[i] = h->attach[--h->attach_count]; continue; }
		if (marker_world(M, po, at.marker, &w)) {
			float pos[3] = { w.m[3][0], w.m[3][1], w.m[3][2] };
			int ui = unit_of(M, at.child);
			if (ui >= 0 && M->game) units_pin(M->units, M->game, ui, pos, atan2f(w.m[0][1], w.m[0][0]));      /* carried: no body integration, nothing to fall */
			else if (is_player(M, at.child) && M->game) { game_teleport(M->game, pos, atan2f(w.m[0][1], w.m[0][0])); }
			co->pos[0] = pos[0]; co->pos[1] = pos[1]; co->pos[2] = pos[2];
			co->root = w;
		}
		i++;
	}
	update_held_weapons(M);
}

/* ---- visibility and distance --------------------------------------------------------------------------------------------- */
static void eye_of(const mission_t *M, int slot, float eye[3], float dir[3])
{
	const object_t *o = &M->objects->obj[slot];
	int ui = unit_of(M, slot);
	if (is_player(M, slot) && M->game) {
		const game_t *g = M->game;
		player_eye(&g->player, &g->pp, eye);
		dir[0] = cosf(g->player.pitch) * cosf(g->player.yaw); dir[1] = cosf(g->player.pitch) * sinf(g->player.yaw); dir[2] = sinf(g->player.pitch);
	} else if (ui >= 0) {
		units_eye(M->units, ui, eye, dir);
	} else {
		float yaw = obj_yaw(M, slot);
		eye[0] = o->pos[0]; eye[1] = o->pos[1]; eye[2] = o->pos[2] + 0.6f;
		dir[0] = cosf(yaw); dir[1] = sinf(yaw); dir[2] = 0.0f;
	}
}

static int sees_point(const mission_t *M, int slot, const float p[3], float half_angle_deg)
{
	float eye[3], dir[3], v[3], d, c;
	coll_hit hit;
	eye_of(M, slot, eye, dir);
	v[0] = p[0] - eye[0]; v[1] = p[1] - eye[1]; v[2] = p[2] - eye[2];
	d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (d < 1e-4f) return 1;
	c = (v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / d;
	if (c < cosf(half_angle_deg * PI_F / 180.0f)) return 0;
	if (M->game && M->game->coll && coll_ray(M->game->coll, eye, p, 0, &hit)) return 0;
	return 1;
}

static int any_sees(mission_t *M, const mission_list *l, const float p[3], float deg, int skip)
{
	unsigned i;
	for (i = 0; l && i < l->count; i++) {
		int slot = l->obj[i];
		if (slot == skip || !mission_object_live(M, slot)) continue;
		if (sees_point(M, slot, p, deg)) return 1;
	}
	return 0;
}

BI(objects_can_see_object)
{
	mission_list tmp;
	int target;
	float p[3];
	ENTER(objects_can_see_object);
	target = obj_arg(M, A(1), 0);
	if (target < 0) return 0;
	mission_object_pos(M, target, p);
	ret_bool(r, any_sees(M, mission_arg_list(M, A(0), &tmp), p, argf(A(2)), target));
	return 0;
}

BI(objects_can_see_flag)
{
	mission_list tmp;
	devdef_cflag f;
	ENTER(objects_can_see_flag);
	if (!flag_arg(M, A(1), &f)) return 0;
	ret_bool(r, any_sees(M, mission_arg_list(M, A(0), &tmp), f.pos, argf(A(2)), -1));
	return 0;
}

static float min_distance(mission_t *M, const mission_list *l, const float p[3])
{
	unsigned i;
	float best = 1.0e9f;
	for (i = 0; l && i < l->count; i++) {
		float q[3], d;
		if (!mission_object_live(M, l->obj[i])) continue;
		mission_object_pos(M, l->obj[i], q);
		d = sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]));
		if (d < best) best = d;
	}
	return best;
}

BI(objects_distance_to_object)
{
	mission_list tmp;
	int target;
	float p[3];
	ENTER(objects_distance_to_object);
	target = obj_arg(M, A(1), 0);
	ret_num(r, 1.0e9f);
	if (target < 0) return 0;
	mission_object_pos(M, target, p);
	ret_num(r, min_distance(M, mission_arg_list(M, A(0), &tmp), p));
	return 0;
}

BI(objects_distance_to_flag)
{
	mission_list tmp;
	devdef_cflag f;
	ENTER(objects_distance_to_flag);
	ret_num(r, 1.0e9f);
	if (!flag_arg(M, A(1), &f)) return 0;
	ret_num(r, min_distance(M, mission_arg_list(M, A(0), &tmp), f.pos));
	return 0;
}

/* ---- health ---------------------------------------------------------------------------------------------------------------- */
static const vitals_t *vitals_of(const mission_t *M, int slot)
{
	if (!M->game || slot < 0) return NULL;
	if (is_player(M, slot)) return &M->game->vitals;
	return &M->game->unit_state[slot].vitals;
}
static vitals_t *vitals_rw(mission_t *M, int slot) { return (vitals_t *)vitals_of(M, slot); }

static int slot_has_vitals(const mission_t *M, int slot)
{
	return M->game && slot >= 0 && (is_player(M, slot) || M->game->unit_state[slot].has_vitals || bit_get(M->hobj.hid_vitals, slot));
}

/* health / shield as fractions of their maximum; 1 for an object without vitals, 0 when dead */
static float health_fraction(const mission_t *M, int slot, int shield)
{
	const vitals_t *v;
	float cur, mx;
	if (!M->game) return 1.0f;
	if (!is_player(M, slot) && M->game->unit_state[slot].dead) return 0.0f;
	if (!slot_has_vitals(M, slot)) return 1.0f;
	v = vitals_of(M, slot);
	if (shield) { cur = v->shield; mx = v->max_shield; } else { cur = v->health; mx = v->max_health; }
	return mx > 0.0f ? cur / mx : (shield ? 0.0f : 1.0f);
}

BI(object_get_health)
{
	int slot;
	ENTER(object_get_health);
	slot = obj_arg(M, A(0), 0);
	ret_num(r, slot >= 0 ? health_fraction(M, slot, 0) : 0.0f);
	return 0;
}

BI(unit_get_health)
{
	int slot;
	ENTER(unit_get_health);
	slot = obj_arg(M, A(0), 0);
	ret_num(r, slot >= 0 ? health_fraction(M, slot, 0) : 0.0f);
	return 0;
}

BI(unit_get_shield)
{
	int slot;
	ENTER(unit_get_shield);
	slot = obj_arg(M, A(0), 0);
	ret_num(r, slot >= 0 ? health_fraction(M, slot, 1) : 0.0f);
	return 0;
}

BI(unit_get_total_grenade_count)
{
	int slot, ui, c = 0;
	ENTER(unit_get_total_grenade_count);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0) return 0;
	ui = unit_of(M, slot);
	if (is_player(M, slot) && M->game) c = M->game->grenades[0] + M->game->grenades[1];
	else if (ui >= 0) c = M->units->u[ui].grenades[0] + M->units->u[ui].grenades[1];
	ret_num(r, (float)c);
	return 0;
}

/* Absolute values in tag units (the tags' health / shield numbers). A health of 0 or less kills the unit. */
static void set_vitality(mission_t *M, int slot, const hs_value *hv, const hs_value *sv, int maximum)
{
	vitals_t *v;
	float hp = argf(hv), sp = argf(sv);
	int ui;
	if (slot < 0 || !slot_has_vitals(M, slot)) return;
	v = vitals_rw(M, slot);
	if (maximum) {
		if (hp > 0.0f) v->max_health = hp;
		if (sp >= 0.0f) v->max_shield = sp;
		if (v->health > v->max_health) v->health = v->max_health;
		if (v->shield > v->max_shield) v->shield = v->max_shield;
		return;
	}
	ui = unit_of(M, slot);
	if (hp <= 0.0f && ui >= 0 && M->game) { units_kill(M->units, M->game, ui, 0); return; }
	v->health = hp > v->max_health && v->max_health > 0.0f ? v->max_health : hp;
	v->shield = sp > v->max_shield ? v->max_shield : sp;
	if (v->health > 0.0f) v->flags &= ~VITALS_F_DEAD;
}

BI(unit_set_current_vitality) { ENTER(unit_set_current_vitality); set_vitality(M, obj_arg(M, A(0), 0), A(1), A(2), 0); return 0; }
BI(unit_set_maximum_vitality) { ENTER(unit_set_maximum_vitality); set_vitality(M, obj_arg(M, A(0), 0), A(1), A(2), 1); return 0; }

BI(units_set_current_vitality)
{
	mission_list tmp;
	const mission_list *l;
	unsigned i;
	ENTER(units_set_current_vitality);
	l = mission_arg_list(M, A(0), &tmp);
	for (i = 0; l && i < l->count; i++) set_vitality(M, l->obj[i], A(1), A(2), 0);
	return 0;
}

static void kill_slot(mission_t *M, int slot, int silent)
{
	int ui = unit_of(M, slot);
	if (slot < 0 || is_player(M, slot)) return;
	if (ui >= 0 && M->game) units_kill(M->units, M->game, ui, silent);
}
BI(unit_kill) { ENTER(unit_kill); kill_slot(M, obj_arg(M, A(0), 0), 0); return 0; }
BI(unit_kill_silent) { ENTER(unit_kill_silent); kill_slot(M, obj_arg(M, A(0), 0), 1); return 0; }

/* ---- damage immunity and per-object flags ------------------------------------------------------------------------------- */
static void set_invulnerable(mission_t *M, int slot, int on)
{
	mission_hostobj *h = &M->hobj;
	int ui;
	if (slot < 0 || is_player(M, slot)) return;
	bit_set(h->cannot_take_damage, slot, on);
	ui = unit_of(M, slot);
	if (ui >= 0) { if (on) M->units->u[ui].flags |= UF_INVULNERABLE; else M->units->u[ui].flags &= (uint8_t)~UF_INVULNERABLE; }
	if (!M->game) return;
	/* park the vitals: game_damage_object ignores an object without vitals, whoever the attacker is */
	if (on && M->game->unit_state[slot].has_vitals) { M->game->unit_state[slot].has_vitals = 0; bit_set(h->hid_vitals, slot, 1); }
	else if (!on && bit_get(h->hid_vitals, slot)) { M->game->unit_state[slot].has_vitals = 1; bit_set(h->hid_vitals, slot, 0); }
}

static void for_list(mission_t *M, const hs_value *v, void (*fn)(mission_t *, int, int), int arg)
{
	mission_list tmp;
	const mission_list *l = mission_arg_list(M, v, &tmp);
	unsigned i;
	for (i = 0; l && i < l->count; i++) fn(M, l->obj[i], arg);
}

BI(object_cannot_take_damage) { ENTER(object_cannot_take_damage); for_list(M, A(0), set_invulnerable, 1); return 0; }
BI(object_can_take_damage) { ENTER(object_can_take_damage); for_list(M, A(0), set_invulnerable, 0); return 0; }
BI(unit_impervious) { ENTER(unit_impervious); for_list(M, A(0), set_invulnerable, argb(A(1))); return 0; }

static void set_bit_obj(uint32_t *bits, int slot, int on) { if (slot >= 0 && slot < OBJ_MAX) bit_set(bits, slot, on); }
BI(object_set_melee_attack_inhibited) { ENTER(object_set_melee_attack_inhibited); set_bit_obj(M->hobj.melee_inhibited, obj_arg(M, A(0), 0), argb(A(1))); return 0; }
BI(object_set_ranged_attack_inhibited) { ENTER(object_set_ranged_attack_inhibited); set_bit_obj(M->hobj.ranged_inhibited, obj_arg(M, A(0), 0), argb(A(1))); return 0; }
BI(unit_set_enterable_by_player) { ENTER(unit_set_enterable_by_player); set_bit_obj(M->hobj.not_enterable, obj_arg(M, A(0), 0), !argb(A(1))); return 0; }

static void set_no_drop(mission_t *M, int slot, int on)
{
	int ui = unit_of(M, slot);
	set_bit_obj(M->hobj.no_drop, slot, on);
	if (ui >= 0) { if (on) M->units->u[ui].flags |= UF_NO_DROP; else M->units->u[ui].flags &= (uint8_t)~UF_NO_DROP; }
}
BI(unit_doesnt_drop_items) { ENTER(unit_doesnt_drop_items); for_list(M, A(0), set_no_drop, 1); return 0; }

BI(object_set_collideable) { ENTER(object_set_collideable); return 0; }
BI(unit_open) { ENTER(unit_open); return 0; }
BI(unit_close) { ENTER(unit_close); return 0; }
/* A frame of the unit's "stand emotions" overlay (-1 = none, "other values depend on unit", XBE 0x220df0): its face's eyelids, brows and lips. */
BI(unit_set_emotion)
{
	int slot, e;
	ENTER(unit_set_emotion);
	slot = obj_arg(M, A(0), 0);
	e = argi(A(1));
	if (slot >= 0 && (unsigned)slot < M->objects->count) {
		object_face *fc = &M->objects->obj[slot].face;
		uint8_t ne = (uint8_t)(e < 0 ? 0 : e > 254 ? 255 : e + 1);
		if (ne != fc->emotion) { fc->emo_from = fc->emotion; fc->emo_blend = 0; fc->emotion = ne; }      /* eased in by misc_tick (EMO_EASE_TICKS) */
	}
	return 0;
}
BI(unit_solo_player_integrated_night_vision_is_active) { ENTER(unit_solo_player_integrated_night_vision_is_active); ret_bool(r, 0); return 0; }
BI(units_set_desired_flashlight_state) { ENTER(units_set_desired_flashlight_state); return 0; }
BI(vehicle_hover) { ENTER(vehicle_hover); return 0; }

BI(unit)
{
	int slot;
	ENTER(unit);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0) { r->v.h = HS_HANDLE_NONE; return 0; }
	ret_obj(r, slot);
	return 0;
}

BI(unit_suspended)
{
	int slot, on;
	mission_hostobj *h;
	uint32_t i;
	ENTER(unit_suspended);
	h = &M->hobj;
	slot = obj_arg(M, A(0), 0);
	on = argb(A(1));
	if (slot < 0) return 0;
	for (i = 0; i < h->suspend_count; i++) if (h->suspend[i].obj == slot) break;
	if (!on) { if (i < h->suspend_count) h->suspend[i] = h->suspend[--h->suspend_count]; return 0; }
	if (i == h->suspend_count) {
		if (h->suspend_count >= MISSION_SUSPEND_MAX) return 0;
		h->suspend_count++;
	}
	h->suspend[i].obj = (int16_t)slot;
	h->suspend[i].pos[0] = M->objects->obj[slot].pos[0]; h->suspend[i].pos[1] = M->objects->obj[slot].pos[1]; h->suspend[i].pos[2] = M->objects->obj[slot].pos[2];
	return 0;
}

/* ---- vehicles and seats (stubs with plausible answers) -------------------------------------------------------------------- */
/* A seated unit hangs on the vehicle's origin (an attachment, so it travels with the vehicle); the seat label is only remembered. */
static void seat_unit(mission_t *M, int unit_slot, int veh, const char *seat)
{
	mission_hostobj *h = &M->hobj;
	uint32_t i;
	if (unit_slot < 0 || veh < 0 || unit_slot == veh || is_player(M, unit_slot)) return;
	h->seat_vehicle[unit_slot] = (int16_t)(veh + 1);
	h->seat_hash[unit_slot] = seat[0] ? hash16(seat) : 0;
	for (i = 0; i < h->attach_count; i++) if (h->attach[i].child == unit_slot) break;
	if (i == h->attach_count) {
		if (h->attach_count >= MISSION_ATTACH_MAX) { mission_note_missing(M, "seat"); return; }
		h->attach_count++;
	}
	h->attach[i].parent = (int16_t)veh; h->attach[i].child = (int16_t)unit_slot; h->attach[i].marker = -1; h->attach[i].pad = 0;
}

static void unseat_unit(mission_t *M, int unit_slot)
{
	mission_hostobj *h = &M->hobj;
	uint32_t i;
	if (unit_slot < 0) return;
	h->seat_vehicle[unit_slot] = 0; h->seat_hash[unit_slot] = 0;
	for (i = 0; i < h->attach_count; i++) if (h->attach[i].child == unit_slot) { h->attach[i] = h->attach[--h->attach_count]; break; }
}

BI(unit_set_seat) { ENTER(unit_set_seat); return 0; }     /* no vehicle given: nothing to sit in */

BI(unit_enter_vehicle)
{
	ENTER(unit_enter_vehicle);
	seat_unit(M, obj_arg(M, A(0), 0), obj_arg(M, A(1), 0), str_arg(hm, A(2)));
	return 0;
}

BI(unit_exit_vehicle) { ENTER(unit_exit_vehicle); unseat_unit(M, obj_arg(M, A(0), 0)); return 0; }

BI(vehicle_load_magic)
{
	mission_list tmp;
	const mission_list *l;
	int veh;
	unsigned i;
	ENTER(vehicle_load_magic);
	veh = obj_arg(M, A(0), 0);
	l = mission_arg_list(M, A(2), &tmp);
	for (i = 0; veh >= 0 && l && i < l->count; i++) seat_unit(M, l->obj[i], veh, str_arg(hm, A(1)));
	return 0;
}

BI(vehicle_unload)
{
	int veh, k;
	uint16_t hash;
	const char *seat;
	ENTER(vehicle_unload);
	veh = obj_arg(M, A(0), 0);
	seat = str_arg(hm, A(1));
	hash = seat[0] ? hash16(seat) : 0;
	if (veh < 0) return 0;
	for (k = 0; k < OBJ_MAX; k++)
		if (M->hobj.seat_vehicle[k] == veh + 1 && (!hash || M->hobj.seat_hash[k] == hash)) unseat_unit(M, k);
	return 0;
}

BI(vehicle_riders)
{
	mission_list *l;
	int veh, k;
	ENTER(vehicle_riders);
	list_of(M, r, &l);
	veh = obj_arg(M, A(0), 0);
	for (k = 0; veh >= 0 && k < OBJ_MAX; k++) if (M->hobj.seat_vehicle[k] == veh + 1 && mission_object_live(M, k)) mission_list_add(l, k);
	return 0;
}

BI(vehicle_test_seat_list)
{
	mission_list tmp;
	const mission_list *l;
	int veh;
	unsigned i;
	uint16_t hash;
	const char *seat;
	ENTER(vehicle_test_seat_list);
	veh = obj_arg(M, A(0), 0);
	seat = str_arg(hm, A(1));
	hash = seat[0] ? hash16(seat) : 0;
	l = mission_arg_list(M, A(2), &tmp);
	for (i = 0; veh >= 0 && l && i < l->count; i++) {
		int s = l->obj[i];
		if (s >= 0 && s < OBJ_MAX && M->hobj.seat_vehicle[s] == veh + 1 && (!hash || M->hobj.seat_hash[s] == hash)) { ret_bool(r, 1); return 0; }
	}
	ret_bool(r, 0);
	return 0;
}

/* ---- custom animations ---------------------------------------------------------------------------------------------------- */
/* saved_graph = the object's own graph index + 2 while a foreign graph plays (non-unit objects only; units hand the graph back themselves) */

static void restore_graph(mission_t *M, int slot)
{
	object_t *o = obj_of(M, slot);
	int16_t sg = M->hobj.saved_graph[slot];
	if (!o || !sg) return;
	o->graph = (int16_t)(sg - 2);
	M->hobj.saved_graph[slot] = 0;
	{
		scen_def d = scen_get_def(M->ow.scen, (uint32_t)o->def);
		if (o->graph >= 0 && d.idle_anim != SCEN_NO_ANIM) { o->anim = d.idle_anim; o->flags |= OBJ_F_ANIMATED; }
		else { o->anim = SCEN_NO_ANIM; o->flags &= ~OBJ_F_ANIMATED; }
		o->frame = 0.0f;
	}
}

static int start_custom(mission_t *M, int slot, uint32_t graph_tag, const char *name, int frame)
{
	object_t *o = obj_of(M, slot);
	int gi, idx, ui;
	anim_graph g;
	anim_info info;
	float tps, ticks;
	if (!o || !M->ow.anims) return 0;
	gi = anim_find_graph(M->ow.anims, graph_tag);
	if (gi < 0) { mission_note_missing(M, "custom_animation graph"); if (M->log_anim) printf("[anim] t%u obj %d graph %08x \"%s\": graph NOT in the pack\n", (unsigned)M->tick, slot, graph_tag, name); return 0; }
	g = anim_get_graph(M->ow.anims, (uint32_t)gi);
	idx = anim_find_name(M->ow.anims, &g, name);
	if (idx < 0) { mission_note_missing(M, "custom_animation name"); if (M->log_anim) printf("[anim] t%u obj %d graph %08x \"%s\": name NOT in the graph (%u animations)\n", (unsigned)M->tick, slot, graph_tag, name, (unsigned)g.anim_count); return 0; }
	if (M->log_anim) printf("[anim] t%u obj %d graph %08x \"%s\" ok (anim %d of %u, frames %u)\n", (unsigned)M->tick, slot, graph_tag, name, idx, (unsigned)g.anim_count, (unsigned)anim_get(M->ow.anims, &g, (uint32_t)idx).frame_count);
	info = anim_get(M->ow.anims, &g, (uint32_t)idx);
	if (frame < 0) frame = 0;
	if (frame >= (int)info.frame_count) frame = info.frame_count ? (int)info.frame_count - 1 : 0;
	tps = info.fps > 0.0f ? info.fps / 30.0f : 1.0f;
	ui = unit_of(M, slot);
	if (ui >= 0 && M->game) {                       /* a unit: the unit layer plays it (and hands the graph back by itself) */
		unsigned t = units_custom_animation(M->units, M->game, ui, gi, idx, 1.0f, 0);
		if (!t) return 0;
		if (frame > 0) {
			unit_t *u = &M->units->u[ui];
			unsigned skip = (unsigned)((float)frame / tps);
			o->frame = (float)frame;
			u->rm_frame = (int16_t)frame;                 /* the skipped frames do not move the unit */
			u->action_ticks = (uint16_t)(skip < t ? t - skip : 1u);
		}
		return 1;
	}
	if (o->graph != gi) {                           /* any other object: swap its graph for the duration */
		if (!M->hobj.saved_graph[slot]) M->hobj.saved_graph[slot] = (int16_t)(o->graph + 2);
		o->graph = (int16_t)gi;
	}
	ticks = ((float)info.frame_count - (float)frame) / tps;
	(void)ticks;
	o->anim = (uint16_t)idx; o->frame = (float)frame; o->flags |= OBJ_F_ANIMATED;
	return 1;
}

/* Ticks left of the unit's custom animation (0 when none). */
static int custom_time_left(const mission_t *M, int slot)
{
	int ui = unit_of(M, slot);
	const object_t *o = &M->objects->obj[slot];
	if (ui >= 0) return M->units->u[ui].action == UA_CUSTOM ? (int)M->units->u[ui].action_ticks : 0;
	if (M->hobj.saved_graph[slot] && M->ow.anims && o->graph >= 0) {
		anim_graph g = anim_get_graph(M->ow.anims, (uint32_t)o->graph);
		anim_info info = anim_get(M->ow.anims, &g, o->anim);
		float left = (float)info.frame_count - o->frame;
		return left > 0.0f ? (int)left : 0;
	}
	return 0;
}

BI(custom_animation)
{
	ENTER(custom_animation);
	(void)start_custom(M, obj_arg(M, A(0), 0), A(1)->v.h, str_arg(hm, A(2)), 0);
	return 0;
}

BI(unit_custom_animation_at_frame)
{
	ENTER(unit_custom_animation_at_frame);
	(void)start_custom(M, obj_arg(M, A(0), 0), A(1)->v.h, str_arg(hm, A(2)), argi(A(4)));
	return 0;
}

BI(custom_animation_list)
{
	mission_list tmp;
	const mission_list *l;
	unsigned i;
	ENTER(custom_animation_list);
	l = mission_arg_list(M, A(0), &tmp);
	for (i = 0; l && i < l->count; i++) (void)start_custom(M, l->obj[i], A(1)->v.h, str_arg(hm, A(2)), 0);
	return 0;
}

BI(unit_stop_custom_animation)
{
	int slot, ui;
	ENTER(unit_stop_custom_animation);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0) return 0;
	ui = unit_of(M, slot);
	if (ui >= 0 && M->game) units_stop_custom(M->units, M->game, ui);
	restore_graph(M, slot);
	return 0;
}

BI(unit_get_custom_animation_time)
{
	int slot;
	ENTER(unit_get_custom_animation_time);
	slot = obj_arg(M, A(0), 0);
	ret_num(r, slot >= 0 ? (float)custom_time_left(M, slot) : 0.0f);
	return 0;
}

BI(unit_has_weapon_readied)
{
	int slot, ui;
	uint32_t tag = 0;
	ENTER(unit_has_weapon_readied);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0 || !M->game || !M->game->weap) return 0;
	ui = unit_of(M, slot);
	if (is_player(M, slot)) {
		const game_slot_t *gs = game_cur_slot(M->game);
		if (gs && gs->weapon >= 0) tag = gs->def.tag_id;
	} else if (ui >= 0 && M->units->u[ui].weapon >= 0) {
		weap_weapon w;
		if (weap_get_weapon(M->game->weap, (uint32_t)M->units->u[ui].weapon, &w)) tag = w.tag_id;
	}
	ret_bool(r, tag != 0 && tag == A(1)->v.h);
	return 0;
}

/* ---- object appearance --------------------------------------------------------------------------------------------------- */
BI(object_set_scale)
{
	object_t *o;
	float s;
	int i, j;
	ENTER(object_set_scale);
	o = obj_of(M, obj_arg(M, A(0), 0));
	s = argf(A(1));
	if (!o || !(s > 0.0f)) return 0;
	/* immediate (the transition time of the third argument is not modelled): the placement matrix is rebuilt unscaled, then scaled */
	{
		float yaw = atan2f(o->root.m[0][1], o->root.m[0][0]);
		mat4_from_ypr(&o->root, yaw, 0.0f, 0.0f, o->pos);
	}
	for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) o->root.m[i][j] *= s;
	return 0;
}

BI(object_set_permutation)
{
	object_t *o;
	const char *perm;
	model_info mi;
	unsigned p;
	ENTER(object_set_permutation);
	o = obj_of(M, obj_arg(M, A(0), 0));
	perm = str_arg(hm, A(2));
	if (!o || o->model < 0 || !M->ow.models) return 0;
	mi = model_get(M->ow.models, (uint32_t)o->model);
	/* the region label of the call is not resolvable (regions carry no names in MODL): the permutation is set in every region that has it */
	for (p = 0; p < mi.permutation_count; p++) {
		char nm[MODEL_NAME_LEN];
		uint32_t reg;
		model_perm_name(M->ow.models, &mi, p, nm);
		if (strcmp(nm, perm) != 0) continue;
		reg = model_perm_region(M->ow.models, &mi, p);
		if (reg < o->region_count && reg < OBJ_MAX_REGIONS) o->perm[reg] = (uint16_t)p;
	}
	return 0;
}

/* ---- player ------------------------------------------------------------------------------------------------------------------ */
BI(player_enable_input) { ENTER(player_enable_input); M->hobj.player_input_disabled = !argb(A(0)); return 0; }
BI(player_camera_control) { ENTER(player_camera_control); M->hobj.player_look_disabled = !argb(A(0)); return 0; }
BI(player_action_test_reset) { ENTER(player_action_test_reset); M->hobj.action_latched = 0; return 0; }

#define ACTION_TEST(name, mask) BI(name) { ENTER(name); ret_bool(r, (M->hobj.action_latched & (mask)) != 0); return 0; }
#define ALL_LOOK (HOBJ_PA_LOOK_UP | HOBJ_PA_LOOK_DOWN | HOBJ_PA_LOOK_LEFT | HOBJ_PA_LOOK_RIGHT)
ACTION_TEST(player_action_test_accept, HOBJ_PA_ACCEPT)
ACTION_TEST(player_action_test_primary_trigger, HOBJ_PA_PRIMARY)
ACTION_TEST(player_action_test_grenade_trigger, HOBJ_PA_GRENADE)
ACTION_TEST(player_action_test_action, HOBJ_PA_ACTION)
ACTION_TEST(player_action_test_back, HOBJ_PA_BACK)
ACTION_TEST(player_action_test_look_relative_up, HOBJ_PA_LOOK_UP)
ACTION_TEST(player_action_test_look_relative_down, HOBJ_PA_LOOK_DOWN)
ACTION_TEST(player_action_test_look_relative_left, HOBJ_PA_LOOK_LEFT)
ACTION_TEST(player_action_test_look_relative_right, HOBJ_PA_LOOK_RIGHT)

BI(player_action_test_look_relative_all_directions)
{
	ENTER(player_action_test_look_relative_all_directions);
	ret_bool(r, (M->hobj.action_latched & ALL_LOOK) == ALL_LOOK);
	return 0;
}

BI(player0_joystick_set_is_normal) { ENTER(player0_joystick_set_is_normal); ret_bool(r, !M->hobj.joystick_not_normal); return 0; }
BI(player0_look_invert_pitch) { ENTER(player0_look_invert_pitch); M->hobj.pitch_inverted = argb(A(0)); return 0; }
BI(player0_look_pitch_is_inverted) { ENTER(player0_look_pitch_is_inverted); ret_bool(r, M->hobj.pitch_inverted); return 0; }
BI(player_add_equipment) { ENTER(player_add_equipment); return 0; }      /* starting profiles are not in SCEN: nothing to hand out */
BI(players_unzoom_all) { ENTER(players_unzoom_all); if (M->game) M->game->zoomed = 0; return 0; }

/* ---- devices ----------------------------------------------------------------------------------------------------------------- */
static int dev_arg(const mission_t *M, const hs_value *v)
{
	int d = -1;
	if (!M->dev || !M->devices || v->type != HS_HANDLE || v->v.h == HS_HANDLE_NONE || (v->v.h & MISSION_LIST_BIT)) return -1;
	if (v->v.h & MISSION_OBJ_BIT) {
		const object_t *o = mission_object_live(M, (int)(v->v.h & 0xFFFFu)) ? &M->objects->obj[v->v.h & 0xFFFFu] : NULL;
		if (o && o->placement >= 0) d = devdef_find_device_by_scen(M->dev, (uint32_t)o->placement);
	} else {
		d = devdef_name_to_device(M->dev, v->v.h);
	}
	return (d >= 0 && (uint32_t)d < M->devices->count) ? d : -1;
}

BI(device_get_position) { int d; ENTER(device_get_position); d = dev_arg(M, A(0)); ret_num(r, d >= 0 ? devices_position(M->devices, (unsigned)d) : 0.0f); return 0; }
BI(device_get_power) { int d; ENTER(device_get_power); d = dev_arg(M, A(0)); ret_num(r, d >= 0 ? devices_power(M->devices, (unsigned)d) : 0.0f); return 0; }

BI(device_set_position)
{
	int d;
	ENTER(device_set_position);
	d = dev_arg(M, A(0));
	if (d >= 0) devices_set_position(M->devices, (unsigned)d, argf(A(1)));
	if (r->type == HS_BOOL) r->v.i = 1;
	return 0;
}

BI(device_set_position_immediate)
{
	int d;
	ENTER(device_set_position_immediate);
	d = dev_arg(M, A(0));
	if (d >= 0) devices_set_position_immediate(M->devices, (unsigned)d, argf(A(1)));
	return 0;
}

BI(device_set_power)
{
	int d;
	ENTER(device_set_power);
	d = dev_arg(M, A(0));
	if (d >= 0) devices_set_power(M->devices, (unsigned)d, argf(A(1)));
	return 0;
}

static int group_arg(const mission_t *M, const hs_value *v)
{
	if (!M->devices || v->type != HS_HANDLE || v->v.h >= M->devices->group_count || v->v.h >= DEV_MAX_GROUPS) return -1;
	return (int)v->v.h;
}

BI(device_group_set)
{
	int g;
	ENTER(device_group_set);
	g = group_arg(M, A(0));
	if (g >= 0) devices_group_set(M->devices, (unsigned)g, argf(A(1)));
	return 0;
}

BI(device_group_set_immediate)
{
	int g;
	uint32_t i;
	float v;
	ENTER(device_group_set_immediate);
	g = group_arg(M, A(0));
	if (g < 0) return 0;
	v = argf(A(1));
	devices_group_set(M->devices, (unsigned)g, v);
	for (i = 0; i < M->devices->count; i++) {                  /* the members jump to where the group value sends them */
		const dev_state *s = &M->devices->dev[i];
		float x = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		if (s->position_group == g) devices_set_position_immediate(M->devices, i, (s->pflags & DEVDEF_PF_REVERSED) ? 1.0f - x : x);
		if (s->power_group == g) devices_set_power_immediate(M->devices, i, x);
	}
	return 0;
}

BI(device_group_get)
{
	int g;
	ENTER(device_group_get);
	g = group_arg(M, A(0));
	ret_num(r, g >= 0 ? devices_group_get(M->devices, (unsigned)g) : 0.0f);
	return 0;
}

static void mflag(mission_t *M, const hs_value *dv, uint32_t bit, int set)
{
	int d = dev_arg(M, dv);
	if (d < 0) return;
	if (set) M->devices->dev[d].mflags |= bit; else M->devices->dev[d].mflags &= ~bit;
}
BI(device_one_sided_set) { ENTER(device_one_sided_set); mflag(M, A(0), DEVDEF_MPF_ONE_SIDED, argb(A(1))); return 0; }
BI(device_operates_automatically_set) { ENTER(device_operates_automatically_set); mflag(M, A(0), DEVDEF_MPF_NO_AUTO, !argb(A(1))); return 0; }
BI(device_set_never_appears_locked) { ENTER(device_set_never_appears_locked); mflag(M, A(0), DEVDEF_MPF_NEVER_LOCKED, argb(A(1))); return 0; }
BI(device_group_change_only_once_more_set) { ENTER(device_group_change_only_once_more_set); return 0; }

/* ---- effects and damage ------------------------------------------------------------------------------------------------------ */
BI(effect_new)
{
	devdef_cflag f;
	float dir[3];
	ENTER(effect_new);
	if (!M->fx || !flag_arg(M, A(1), &f)) return 0;
	dir[0] = cosf(f.pitch) * cosf(f.yaw); dir[1] = cosf(f.pitch) * sinf(f.yaw); dir[2] = sinf(f.pitch);
	fx_spawn(M->fx, A(0)->v.h, f.pos, dir, 0);
	return 0;
}

BI(effect_new_on_object_marker)
{
	int slot;
	mat4 w;
	float dir[3], pos[3];
	ENTER(effect_new_on_object_marker);
	slot = obj_arg(M, A(1), 0);
	if (slot < 0 || !M->fx) return 0;
	if (!marker_world(M, &M->objects->obj[slot], marker_index(M, &M->objects->obj[slot], str_arg(hm, A(2))), &w)) w = M->objects->obj[slot].root;
	pos[0] = w.m[3][0]; pos[1] = w.m[3][1]; pos[2] = w.m[3][2];
	dir[0] = w.m[0][0]; dir[1] = w.m[0][1]; dir[2] = w.m[0][2];
	fx_spawn(M->fx, A(0)->v.h, pos, dir, 0);
	return 0;
}

static void hurt_player(mission_t *M, float amount)
{
	if (!M->game || vitals_dead(&M->game->vitals) || M->game->player_frozen || amount <= 0.0f) return;
	if (vitals_damage(&M->game->vitals, amount, 1.0f, 1.0f, 0.0f) > 0.0f) M->game->hurt_flash = 1.0f;
}

static void hurt_slot(mission_t *M, int slot, float amount)
{
	int ui;
	if (slot < 0 || amount <= 0.0f || !M->game) return;
	if (is_player(M, slot)) { hurt_player(M, amount); return; }
	ui = unit_of(M, slot);
	if (ui >= 0) units_damage(M->units, M->game, ui, amount);
	else game_damage_object(M->game, slot, amount, NULL);
}

static int damage_def(const mission_t *M, uint32_t tag, weap_damage *out)
{
	int di;
	if (!M->game || !M->game->weap || !tag) return 0;
	di = weap_find_damage(M->game->weap, tag);
	return di >= 0 && weap_get_damage(M->game->weap, (uint32_t)di, out);
}

BI(damage_object)
{
	weap_damage d;
	ENTER(damage_object);
	if (damage_def(M, A(0)->v.h, &d)) hurt_slot(M, obj_arg(M, A(1), 0), d.damage_upper[0]);
	return 0;
}

BI(damage_new)
{
	weap_damage d;
	devdef_cflag f;
	unsigned i;
	ENTER(damage_new);
	if (!damage_def(M, A(0)->v.h, &d) || !flag_arg(M, A(1), &f) || !(d.radius[1] > 0.0f)) return 0;
	for (i = 0; M->objects && i < M->objects->count; i++) {
		const object_t *o = &M->objects->obj[i];
		float dist, amt;
		if ((o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) || (int)i == M->player_obj || !M->game->unit_state[i].has_vitals) continue;
		dist = sqrtf((o->pos[0] - f.pos[0]) * (o->pos[0] - f.pos[0]) + (o->pos[1] - f.pos[1]) * (o->pos[1] - f.pos[1]) + (o->pos[2] - f.pos[2]) * (o->pos[2] - f.pos[2])) - o->radius * 0.5f;
		if (dist < 0.0f) dist = 0.0f;
		amt = weap_damage_at_radius(&d, dist, 0.5f);
		if (amt > 0.0f) hurt_slot(M, (int)i, amt);
	}
	if (M->game) {
		float eye[3], dist;
		player_eye(&M->game->player, &M->game->pp, eye);
		dist = sqrtf((eye[0] - f.pos[0]) * (eye[0] - f.pos[0]) + (eye[1] - f.pos[1]) * (eye[1] - f.pos[1]) + (eye[2] - f.pos[2]) * (eye[2] - f.pos[2]));
		hurt_player(M, weap_damage_at_radius(&d, dist, 0.5f));
	}
	return 0;
}

/* ---- recorded animations ------------------------------------------------------------------------------------------------------ */
static int start_recording(mission_t *M, int slot, uint32_t rec, unsigned flags)
{
	int ps;
	object_t *o = obj_of(M, slot);
	if (!o || !M->cine || !M->cin || rec >= M->cin->rec_count || is_player(M, slot)) return 0;
	forget_cine(M, slot);
	ps = cine_play_begin(M->cine, M->cin, rec, (uint32_t)slot, flags);
	if (ps < 0) {                                              /* the pool is full: finished playbacks are reclaimed */
		unsigned i;
		for (i = 0; i < CINE_MAX_PLAYS; i++) if (M->cine->plays[i].used && M->cine->plays[i].play.done) cine_play_kill(M->cine, (int)i);
		ps = cine_play_begin(M->cine, M->cin, rec, (uint32_t)slot, flags);
	}
	if (ps < 0) { mission_note_missing(M, "recording pool"); return 0; }
	{
		int ui = unit_of(M, slot);
		if (ui >= 0) M->units->u[ui].flags |= UF_NO_AI;
	}
	return 1;
}

BI(recording_play) { ENTER(recording_play); (void)start_recording(M, obj_arg(M, A(0), 0), A(1)->v.h, 0); return 0; }
BI(recording_play_and_delete) { ENTER(recording_play_and_delete); (void)start_recording(M, obj_arg(M, A(0), 0), A(1)->v.h, CINE_PLAY_DELETE); return 0; }
BI(recording_play_and_hover) { ENTER(recording_play_and_hover); (void)start_recording(M, obj_arg(M, A(0), 0), A(1)->v.h, CINE_PLAY_HOVER); return 0; }

static void release_unit(mission_t *M, int slot)
{
	int ui = unit_of(M, slot);
	if (ui >= 0) {
		unit_control_t c;
		memset(&c, 0, sizeof c);
		c.aim_yaw = M->units->u[ui].body.yaw; c.weapon_slot = -1;
		units_set_control(M->units, ui, &c);
		M->units->u[ui].flags &= (uint8_t)~UF_NO_AI;
	}
}

BI(recording_kill)
{
	int slot;
	ENTER(recording_kill);
	slot = obj_arg(M, A(0), 0);
	if (slot < 0) return 0;
	forget_cine(M, slot);
	release_unit(M, slot);
	return 0;
}

/* Remaining ticks of the unit's recording (scripts sleep on it: `sleep (recording_time unit)` waits for the end). */
BI(recording_time)
{
	int slot, ps;
	float left = 0.0f;
	ENTER(recording_time);
	slot = obj_arg(M, A(0), 0);
	ps = (slot >= 0 && M->cine) ? cine_play_find(M->cine, (uint32_t)slot) : -1;
	if (ps >= 0) {
		const cine_play *p = cine_play_get(M->cine, ps);
		if (p && p->length > p->tick) left = (float)(p->length - p->tick);
	}
	ret_num(r, left);
	return 0;
}

/* ---- per tick ---------------------------------------------------------------------------------------------------------------- */
void hs_host_obj_pre(mission_t *M, const game_input_t *in)
{
	unsigned i;
	if (in) {
		uint16_t l = 0;
		if (in->held & IN_JUMP) l |= HOBJ_PA_ACCEPT;
		if (in->held & IN_FIRE) l |= HOBJ_PA_PRIMARY;
		if (in->held & IN_GRENADE) l |= HOBJ_PA_GRENADE;
		if (in->held & IN_SWITCH) l |= HOBJ_PA_ACTION;
		if (in->move.move_y < -0.5f) l |= HOBJ_PA_BACK;
		if (in->move.look_y > 0.3f) l |= HOBJ_PA_LOOK_UP;
		if (in->move.look_y < -0.3f) l |= HOBJ_PA_LOOK_DOWN;
		if (in->move.look_x < -0.3f) l |= HOBJ_PA_LOOK_LEFT;
		if (in->move.look_x > 0.3f) l |= HOBJ_PA_LOOK_RIGHT;
		M->hobj.action_latched |= l;
	}
	if (!M->cine || !M->cin) return;
	for (i = 0; i < CINE_MAX_PLAYS; i++) {
		cine_play_slot *ps = &M->cine->plays[i];
		int slot, ui;
		if (!ps->used) continue;
		slot = (int)ps->owner;
		if (!mission_object_live(M, slot)) { cine_play_kill(M->cine, (int)i); continue; }
		ui = unit_of(M, slot);
		if (ps->play.done) {                                   /* finished: the unit is released (or deleted, or left hovering) */
			if (ps->flags & CINE_PLAY_DELETE) { destroy_obj(M, slot); continue; }
			if (!(ps->flags & CINE_PLAY_HOVER)) release_unit(M, slot);
			cine_play_kill(M->cine, (int)i);
			continue;
		}
		if (ui >= 0) {
			M->units->u[ui].flags |= UF_NO_AI;
			units_apply_cinedef(M->units, ui, &ps->play.ctl);
		}
	}
}

void hs_host_obj_post(mission_t *M)
{
	mission_hostobj *h = &M->hobj;
	uint32_t i;
	int slot;
	update_attachments(M);
	update_object_fx(M);
	for (i = 0; i < h->suspend_count; i++) {
		object_t *o = obj_of(M, h->suspend[i].obj);
		int ui = unit_of(M, h->suspend[i].obj);
		if (!o) continue;
		if (ui >= 0 && M->game) units_pin(M->units, M->game, ui, h->suspend[i].pos, M->units->u[ui].body.yaw);
		o->pos[0] = h->suspend[i].pos[0]; o->pos[1] = h->suspend[i].pos[1]; o->pos[2] = h->suspend[i].pos[2];
	}
	for (slot = 0; M->objects && slot < (int)M->objects->count; slot++) {          /* custom animations that have run out hand the graph back */
		int ui;
		if (!h->saved_graph[slot] || !mission_object_live(M, slot)) continue;
		ui = unit_of(M, slot);
		if (ui >= 0 || custom_time_left(M, slot) <= 1) restore_graph(M, slot);
	}
}

/* ---- registration and diagnostics ---------------------------------------------------------------------------------------------- */
#define X_REG(name) { #name, bi_##name },
static const struct { const char *name; hs_builtin fn; } g_table[] = { ALL_LIST(X_REG) };

void hs_host_register_objects(hs_machine *hm)
{
	unsigned i;
	for (i = 0; i < sizeof g_table / sizeof g_table[0]; i++) hs_register(hm, g_table[i].name, g_table[i].fn);
}

unsigned hs_host_obj_count(void) { return ID_COUNT; }
const char *hs_host_obj_name(unsigned i) { return i < ID_COUNT ? g_table[i].name : ""; }
uint32_t hs_host_obj_calls(unsigned i) { return i < ID_COUNT ? g_calls[i] : 0; }
void hs_host_obj_stats_reset(void) { memset(g_calls, 0, sizeof g_calls); }
