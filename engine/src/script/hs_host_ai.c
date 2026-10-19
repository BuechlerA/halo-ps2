/* Script host A: the `ai` built-ins (docs/32-ai-runtime.md). Pure C11, no heap, no double. Conventions: sim/mission.h (object arguments are resolved with
 * mission_resolve_object, results that are object lists are mission lists of object slots). A built-in never fails and none of them blocks: the
 * scripts wait with sleep / sleep_until on the values they return (counts, fractions, command-list and conversation statuses). */
#include "script/hs_host_ai.h"
#include "ai/ai.h"
#include "sim/mission.h"
#include <string.h>

/* ---- the built-ins of this module (X-macro: ids, functions and the registration table come from one list) ---- */
#define AI_LIST(X) \
	X(ai) X(ai_place) X(ai_actors) X(ai_living_count) X(ai_living_fraction) X(ai_nonswarm_count) X(ai_strength) X(ai_status) \
	X(ai_erase) X(ai_erase_all) X(ai_kill) X(ai_kill_silent) X(ai_migrate) X(ai_migrate_and_speak) X(ai_migrate_by_unit) \
	X(ai_spawn_actor) X(ai_renew) X(ai_timer_expire) X(ai_free) X(ai_free_units) X(ai_attach) X(ai_attach_free) X(ai_detach) \
	X(ai_teleport_to_starting_location) X(ai_teleport_to_starting_location_if_unsupported) \
	X(ai_attack) X(ai_defend) X(ai_retreat) X(ai_maneuver) X(ai_maneuver_enable) X(ai_follow_target_players) X(ai_follow_target_ai) \
	X(ai_follow_target_disable) X(ai_follow_distance) X(ai_berserk) X(ai_braindead) X(ai_braindead_by_unit) X(ai_set_blind) X(ai_set_deaf) \
	X(ai_disregard) X(ai_prefer_target) X(ai_try_to_fight) X(ai_try_to_fight_player) X(ai_try_to_fight_nothing) X(ai_force_active) \
	X(ai_allow_dormant) X(ai_playfight) X(ai_grenades) X(ai_dialogue_triggers) X(ai_magically_see_players) X(ai_magically_see_encounter) \
	X(ai_link_activation) X(ai_automatic_migration_target) X(ai_allegiance) X(ai_allegiance_remove) X(ai_allegiance_broken) \
	X(ai_command_list) X(ai_command_list_by_unit) X(ai_command_list_advance) X(ai_command_list_status) \
	X(ai_conversation) X(ai_conversation_stop) X(ai_conversation_advance) X(ai_conversation_status) X(ai_conversation_line) \
	X(ai_go_to_vehicle) X(ai_going_to_vehicle) X(ai_exit_vehicle) X(ai_vehicle_encounter) X(ai_vehicle_enterable_distance) \
	X(ai_vehicle_enterable_disable) X(ai_vehicle_enterable_actor_type) X(ai_reconnect)

#define X_ID(name) ID_##name,
enum { AI_LIST(X_ID) ID_COUNT };
static uint32_t g_calls[ID_COUNT];

#define BI(name) static int bi_##name(hs_machine *hm, void *host, const hs_value *a, unsigned n, hs_value *r)
#define ENTER(name) mission_t *M = (mission_t *)host; ai_t *AI = M ? (ai_t *)M->ai : NULL; (void)hm; (void)a; (void)n; (void)r; (void)M; (void)AI; g_calls[ID_##name]++

static const hs_value NIL = { HS_VOID, 0, 0, { 0 } };
#define A(i) ((i) < n ? &a[i] : &NIL)

static int f2i(float f)
{
	if (!(f == f)) return 0;
	if (f >= 2147483520.0f) return 2147483647;
	if (f <= -2147483648.0f) return (-2147483647 - 1);
	return (int)f;
}
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

/* an `ai` argument (encounter / squad / platoon reference): 1 when it names something of the section */
static int ref_of(const ai_t *AI, const hs_value *v, ai_ref *r)
{
	memset(r, 0, sizeof *r);
	r->squad = r->platoon = -1;
	return AI && v->type == HS_HANDLE && ai_ref_decode(AI, v->v.h, r);
}

/* the AI unit of an object argument, -1 when it is none */
static int unit_arg(mission_t *M, const hs_value *v)
{
	int obj = mission_resolve_object(M, v, 0);
	return (M->units && obj >= 0) ? units_of_object(M->units, obj) : -1;
}

/* the units of an object list (or a single object) argument, call fn on each */
static unsigned units_of_list(mission_t *M, const hs_value *v, int *out, unsigned max)
{
	mission_list tmp;
	const mission_list *l = mission_arg_list(M, v, &tmp);
	unsigned i, k = 0;
	if (!l) return 0;
	for (i = 0; i < l->count && k < max; i++) {
		int u = M->units ? units_of_object(M->units, l->obj[i]) : -1;
		if (u >= 0) out[k++] = u;
	}
	return k;
}

/* ---- queries ---- */
BI(ai_actors)
{
	ai_ref ref;
	uint32_t h;
	mission_list *l;
	ENTER(ai_actors);
	l = mission_list_new(M, &h);
	if (ref_of(AI, A(0), &ref)) {
		int16_t objs[MISSION_LIST_MAX];
		unsigned i, k = ai_actor_objects(AI, &ref, objs, MISSION_LIST_MAX);
		for (i = 0; i < k; i++) mission_list_add(l, objs[i]);
	}
	r->v.h = h;
	return 0;
}
BI(ai_living_count) { ai_ref ref; ENTER(ai_living_count); ret_num(r, ref_of(AI, A(0), &ref) ? (float)ai_living_count(AI, &ref) : 0.0f); return 0; }
BI(ai_living_fraction) { ai_ref ref; ENTER(ai_living_fraction); ret_num(r, ref_of(AI, A(0), &ref) ? ai_living_fraction(AI, &ref) : 0.0f); return 0; }
BI(ai_nonswarm_count) { ai_ref ref; ENTER(ai_nonswarm_count); ret_num(r, ref_of(AI, A(0), &ref) ? (float)ai_nonswarm_count(AI, &ref) : 0.0f); return 0; }
BI(ai_strength) { ai_ref ref; ENTER(ai_strength); ret_num(r, ref_of(AI, A(0), &ref) ? ai_strength(AI, &ref) : 0.0f); return 0; }
BI(ai_status) { ai_ref ref; ENTER(ai_status); ret_num(r, ref_of(AI, A(0), &ref) ? (float)ai_status(AI, &ref) : 0.0f); return 0; }

/* ---- the AI switch, spawning, removal ---- */
BI(ai) { ENTER(ai); if (AI) AI->st.enabled = (uint8_t)argb(A(0)); return 0; }
BI(ai_place) { ai_ref ref; ENTER(ai_place); if (ref_of(AI, A(0), &ref)) ai_place(AI, &ref); return 0; }
BI(ai_spawn_actor) { ai_ref ref; ENTER(ai_spawn_actor); if (ref_of(AI, A(0), &ref)) ai_spawn_one(AI, &ref); return 0; }
BI(ai_renew) { ai_ref ref; ENTER(ai_renew); if (ref_of(AI, A(0), &ref)) ai_renew(AI, &ref); return 0; }
BI(ai_timer_expire) { ai_ref ref; ENTER(ai_timer_expire); if (ref_of(AI, A(0), &ref)) ai_timer_expire(AI, &ref); return 0; }
BI(ai_erase) { ai_ref ref; ENTER(ai_erase); if (ref_of(AI, A(0), &ref)) ai_erase(AI, &ref); return 0; }
BI(ai_erase_all) { ENTER(ai_erase_all); if (AI) ai_erase_all(AI); return 0; }
BI(ai_kill) { ai_ref ref; ENTER(ai_kill); if (ref_of(AI, A(0), &ref)) ai_kill(AI, &ref, 0); return 0; }
BI(ai_kill_silent) { ai_ref ref; ENTER(ai_kill_silent); if (ref_of(AI, A(0), &ref)) ai_kill(AI, &ref, 1); return 0; }
BI(ai_free) { ai_ref ref; ENTER(ai_free); if (ref_of(AI, A(0), &ref)) ai_free(AI, &ref); return 0; }
BI(ai_free_units)
{
	int u[MISSION_LIST_MAX];
	unsigned i, k;
	ENTER(ai_free_units);
	if (!AI) return 0;
	k = units_of_list(M, A(0), u, MISSION_LIST_MAX);
	for (i = 0; i < k; i++) ai_detach_unit(AI, u[i]);
	return 0;
}
BI(ai_attach)
{
	ai_ref ref;
	int u;
	ENTER(ai_attach);
	if (AI && (u = unit_arg(M, A(0))) >= 0 && ref_of(AI, A(1), &ref)) ai_attach_unit(AI, u, &ref, 0);
	return 0;
}
BI(ai_attach_free)
{
	int u;
	ENTER(ai_attach_free);
	if (AI && (u = unit_arg(M, A(0))) >= 0) ai_attach_unit(AI, u, NULL, A(1)->type == HS_HANDLE && A(1)->v.h != HS_HANDLE_NONE ? A(1)->v.h : 0u);
	return 0;
}
BI(ai_detach) { int u; ENTER(ai_detach); if (AI && (u = unit_arg(M, A(0))) >= 0) ai_detach_unit(AI, u); return 0; }
BI(ai_migrate) { ai_ref f, t; ENTER(ai_migrate); if (ref_of(AI, A(0), &f) && ref_of(AI, A(1), &t)) ai_migrate(AI, &f, &t); return 0; }
BI(ai_migrate_and_speak) { ai_ref f, t; ENTER(ai_migrate_and_speak); if (ref_of(AI, A(0), &f) && ref_of(AI, A(1), &t)) ai_migrate(AI, &f, &t); return 0; }
BI(ai_migrate_by_unit)
{
	int u[MISSION_LIST_MAX];
	unsigned i, k;
	ai_ref t;
	ENTER(ai_migrate_by_unit);
	if (!AI || !ref_of(AI, A(1), &t)) return 0;
	k = units_of_list(M, A(0), u, MISSION_LIST_MAX);
	for (i = 0; i < k; i++) ai_migrate_unit(AI, u[i], &t);
	return 0;
}
BI(ai_teleport_to_starting_location) { ai_ref ref; ENTER(ai_teleport_to_starting_location); if (ref_of(AI, A(0), &ref)) ai_teleport_home(AI, &ref); return 0; }
BI(ai_teleport_to_starting_location_if_unsupported) { ai_ref ref; ENTER(ai_teleport_to_starting_location_if_unsupported); if (ref_of(AI, A(0), &ref)) ai_teleport_home(AI, &ref); return 0; }

/* ---- orders and flags ---- */
BI(ai_attack) { ai_ref ref; ENTER(ai_attack); if (ref_of(AI, A(0), &ref)) ai_set_orders(AI, &ref, AIO_ATTACK); return 0; }
BI(ai_defend) { ai_ref ref; ENTER(ai_defend); if (ref_of(AI, A(0), &ref)) ai_set_orders(AI, &ref, AIO_DEFEND); return 0; }
BI(ai_retreat) { ai_ref ref; ENTER(ai_retreat); if (ref_of(AI, A(0), &ref)) ai_set_orders(AI, &ref, AIO_RETREAT); return 0; }
BI(ai_maneuver) { ai_ref ref; ENTER(ai_maneuver); if (ref_of(AI, A(0), &ref)) { ai_set_orders(AI, &ref, AIO_MANEUVER); ai_magically_see_players(AI, &ref); } return 0; }
BI(ai_maneuver_enable) { ENTER(ai_maneuver_enable); return 0; }
BI(ai_follow_target_players) { ai_ref ref; ENTER(ai_follow_target_players); if (ref_of(AI, A(0), &ref)) ai_set_follow(AI, &ref, NULL); return 0; }
BI(ai_follow_target_ai) { ai_ref f, t; ENTER(ai_follow_target_ai); if (ref_of(AI, A(0), &f) && ref_of(AI, A(1), &t)) ai_set_follow(AI, &f, &t); return 0; }
BI(ai_follow_target_disable) { ai_ref ref; ENTER(ai_follow_target_disable); if (ref_of(AI, A(0), &ref)) ai_set_orders(AI, &ref, AIO_DEFEND); return 0; }
BI(ai_follow_distance) { ENTER(ai_follow_distance); return 0; }
BI(ai_berserk) { ai_ref ref; ENTER(ai_berserk); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_BERSERK, argb(A(1))); return 0; }
BI(ai_braindead) { ai_ref ref; ENTER(ai_braindead); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_BRAINDEAD, argb(A(1))); return 0; }
BI(ai_braindead_by_unit)
{
	int u[MISSION_LIST_MAX];
	unsigned i, k;
	ENTER(ai_braindead_by_unit);
	if (!AI) return 0;
	k = units_of_list(M, A(0), u, MISSION_LIST_MAX);
	for (i = 0; i < k; i++) ai_set_flag_unit(AI, u[i], AIF_BRAINDEAD, argb(A(1)));
	return 0;
}
BI(ai_set_blind) { ai_ref ref; ENTER(ai_set_blind); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_BLIND, argb(A(1))); return 0; }
BI(ai_set_deaf) { ai_ref ref; ENTER(ai_set_deaf); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_DEAF, argb(A(1))); return 0; }
BI(ai_disregard)
{
	int u[MISSION_LIST_MAX];
	unsigned i, k;
	ENTER(ai_disregard);
	if (!AI) return 0;
	k = units_of_list(M, A(0), u, MISSION_LIST_MAX);
	for (i = 0; i < k; i++) ai_disregard(AI, u[i], argb(A(1)));
	return 0;
}
BI(ai_prefer_target) { ENTER(ai_prefer_target); return 0; }
BI(ai_try_to_fight)
{
	ai_ref f, t;
	ENTER(ai_try_to_fight);
	if (ref_of(AI, A(0), &f) && ref_of(AI, A(1), &t)) { ai_set_orders(AI, &f, AIO_ATTACK); ai_magically_see_encounter(AI, &f, &t); }
	return 0;
}
BI(ai_try_to_fight_player)
{
	ai_ref ref;
	ENTER(ai_try_to_fight_player);
	if (ref_of(AI, A(0), &ref)) { ai_set_orders(AI, &ref, AIO_ATTACK); ai_magically_see_players(AI, &ref); }
	return 0;
}
BI(ai_try_to_fight_nothing)
{
	ai_ref ref;
	ENTER(ai_try_to_fight_nothing);
	if (ref_of(AI, A(0), &ref)) { ai_set_flag(AI, &ref, AIF_IGNORE_ALL, 1); ai_set_orders(AI, &ref, AIO_DEFEND); }
	return 0;
}
BI(ai_force_active) { ai_ref ref; ENTER(ai_force_active); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_FORCE_ACTIVE, argb(A(1))); return 0; }
BI(ai_allow_dormant) { ai_ref ref; ENTER(ai_allow_dormant); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_FORCE_ACTIVE, !argb(A(1))); return 0; }
BI(ai_playfight) { ai_ref ref; ENTER(ai_playfight); if (ref_of(AI, A(0), &ref)) ai_set_flag(AI, &ref, AIF_PLAYFIGHT, argb(A(1))); return 0; }
BI(ai_grenades) { ENTER(ai_grenades); if (AI) AI->st.grenades = (uint8_t)argb(A(0)); return 0; }
BI(ai_dialogue_triggers) { ENTER(ai_dialogue_triggers); if (AI) AI->st.dialogue = (uint8_t)argb(A(0)); return 0; }
BI(ai_magically_see_players) { ai_ref ref; ENTER(ai_magically_see_players); if (ref_of(AI, A(0), &ref)) ai_magically_see_players(AI, &ref); return 0; }
BI(ai_magically_see_encounter) { ai_ref f, t; ENTER(ai_magically_see_encounter); if (ref_of(AI, A(0), &f) && ref_of(AI, A(1), &t)) ai_magically_see_encounter(AI, &f, &t); return 0; }
BI(ai_link_activation) { ai_ref f, t; ENTER(ai_link_activation); if (ref_of(AI, A(0), &f) && ref_of(AI, A(1), &t)) ai_link_activation(AI, &f, &t); return 0; }
BI(ai_automatic_migration_target) { ENTER(ai_automatic_migration_target); return 0; }
BI(ai_allegiance) { ENTER(ai_allegiance); if (AI) ai_allegiance(AI, argi(A(0)), argi(A(1)), 1); return 0; }
BI(ai_allegiance_remove) { ENTER(ai_allegiance_remove); if (AI) ai_allegiance(AI, argi(A(0)), argi(A(1)), 0); return 0; }
BI(ai_allegiance_broken)
{
	ENTER(ai_allegiance_broken);
	ret_bool(r, AI && argi(A(0)) >= 0 && argi(A(0)) < (int)AIDEF_TEAMS && argi(A(1)) >= 0 && argi(A(1)) < (int)AIDEF_TEAMS &&
	            AI->st.rel[argi(A(0))][argi(A(1))] == 2);
	return 0;
}

/* ---- command lists ---- */
BI(ai_command_list)
{
	ai_ref ref;
	ENTER(ai_command_list);
	if (ref_of(AI, A(0), &ref) && A(1)->v.h != HS_HANDLE_NONE) ai_command_list(AI, &ref, (int)A(1)->v.h, 1);
	return 0;
}
BI(ai_command_list_by_unit)
{
	int u;
	ENTER(ai_command_list_by_unit);
	if (!AI || (u = unit_arg(M, A(0))) < 0 || A(1)->v.h == HS_HANDLE_NONE) return 0;
	if (ai_actor_of_unit(AI, u) < 0) ai_attach_unit(AI, u, NULL, 0);        /* a scripted unit gets a brain to run the list */
	ai_command_list_unit(AI, u, (int)A(1)->v.h);
	return 0;
}
BI(ai_command_list_advance) { ai_ref ref; ENTER(ai_command_list_advance); if (ref_of(AI, A(0), &ref)) ai_command_list_advance(AI, &ref); return 0; }
BI(ai_command_list_status)
{
	int u[MISSION_LIST_MAX];
	unsigned i, k, running = 0, done = 0;
	ENTER(ai_command_list_status);
	if (!AI) return 0;
	k = units_of_list(M, A(0), u, MISSION_LIST_MAX);
	if (k == 0) { ret_num(r, 2.0f); return 0; }          /* nobody left: finished */
	for (i = 0; i < k; i++) {
		int st = ai_command_list_status(AI, u[i]);
		if (st == 1) running++; else if (st == 2) done++;
	}
	ret_num(r, running ? 1.0f : (done ? 2.0f : 0.0f));
	return 0;
}

/* ---- conversations ---- */
BI(ai_conversation) { ENTER(ai_conversation); if (AI && A(0)->v.h != HS_HANDLE_NONE) ai_conversation_start(AI, (int)A(0)->v.h); return 0; }
BI(ai_conversation_stop) { ENTER(ai_conversation_stop); if (AI && A(0)->v.h != HS_HANDLE_NONE) ai_conversation_stop(AI, (int)A(0)->v.h); return 0; }
BI(ai_conversation_advance) { ENTER(ai_conversation_advance); if (AI && A(0)->v.h != HS_HANDLE_NONE) ai_conversation_advance(AI, (int)A(0)->v.h); return 0; }
BI(ai_conversation_status) { ENTER(ai_conversation_status); ret_num(r, AI && A(0)->v.h != HS_HANDLE_NONE ? (float)ai_conversation_status(AI, (int)A(0)->v.h) : 0.0f); return 0; }
BI(ai_conversation_line) { ENTER(ai_conversation_line); ret_num(r, AI && A(0)->v.h != HS_HANDLE_NONE ? (float)ai_conversation_line(AI, (int)A(0)->v.h) : 0.0f); return 0; }

/* ---- vehicles: not supported, counted no-ops with the right result type ---- */
BI(ai_go_to_vehicle) { ENTER(ai_go_to_vehicle); return 0; }
BI(ai_going_to_vehicle) { ENTER(ai_going_to_vehicle); ret_num(r, 0.0f); return 0; }
BI(ai_exit_vehicle) { ENTER(ai_exit_vehicle); return 0; }
BI(ai_vehicle_encounter) { ENTER(ai_vehicle_encounter); return 0; }
BI(ai_vehicle_enterable_distance) { ENTER(ai_vehicle_enterable_distance); return 0; }
BI(ai_vehicle_enterable_disable) { ENTER(ai_vehicle_enterable_disable); return 0; }
BI(ai_vehicle_enterable_actor_type) { ENTER(ai_vehicle_enterable_actor_type); return 0; }
BI(ai_reconnect) { ENTER(ai_reconnect); return 0; }

/* ---- registration and diagnostics ---- */
#define X_NAME(name) #name,
static const char *const g_names[ID_COUNT] = { AI_LIST(X_NAME) };

void hs_host_register_ai(hs_machine *m)
{
#define X(nm) hs_register(m, #nm, bi_##nm);
	AI_LIST(X)
#undef X
}

unsigned hs_host_ai_count(void) { return ID_COUNT; }
const char *hs_host_ai_name(unsigned i) { return i < ID_COUNT ? g_names[i] : ""; }
uint32_t hs_host_ai_calls(unsigned i) { return i < ID_COUNT ? g_calls[i] : 0; }
void hs_host_ai_stats_reset(void) { memset(g_calls, 0, sizeof g_calls); }
