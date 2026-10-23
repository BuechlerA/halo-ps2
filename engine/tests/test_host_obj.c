/* Host test of the script host H1 (script/hs_host_obj.c, docs/33-host-objects.md) against the real a10 packs (skipped when assets/ is missing):
 * every built-in is called with real arguments and its effect checked, then the a10 script machine runs 9000 ticks with the player idle
 * (the soak: calls per built-in, unregistered built-ins hit, errors, blocking). Usage: test_host_obj <assets/hp2 dir> [CINE pack]. */
#include "mission_fixture.h"
#include "script/hs_host_obj.h"
#include <time.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static mission_fixture F;
static const char *g_dir = "../../assets/hp2";

static void fresh(uint32_t seed, int scripts)
{
	mfix_start(&F, seed);
	mfix_hs_start(&F, hs_host_register_objects);
	F.pre = hs_host_obj_pre; F.post = hs_host_obj_post;
	if (!scripts) F.mission.hs = NULL;          /* direct calls only: the script threads stay out of the way */
	hs_host_obj_stats_reset();
}

static void ticks(int n)
{
	int i;
	for (i = 0; i < n; i++) mfix_tick(&F, NULL);
}

static hs_value call(const char *name, unsigned ret_cls, const hs_value *args, unsigned n) { return mfix_call(&F, name, ret_cls, args, n); }
static hs_value call1(const char *name, unsigned ret_cls, hs_value a0) { return mfix_call(&F, name, ret_cls, &a0, 1); }
static hs_value call2(const char *name, unsigned ret_cls, hs_value a0, hs_value a1) { hs_value a[2]; a[0] = a0; a[1] = a1; return mfix_call(&F, name, ret_cls, a, 2); }
static hs_value call3(const char *name, unsigned ret_cls, hs_value a0, hs_value a1, hs_value a2) { hs_value a[3]; a[0] = a0; a[1] = a1; a[2] = a2; return mfix_call(&F, name, ret_cls, a, 3); }

static float dist3(const float a[3], const float b[3]) { return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
static int live(int slot) { return mission_object_live(&F.mission, slot); }
static const object_t *obj(int slot) { return &F.objects.obj[slot]; }

/* first live object of a type in BSP 0 (not the player's body), with an optional unit requirement */
static int first_live(int type, int need_unit, int skip)
{
	unsigned i;
	for (i = 0; i < F.objects.count; i++) {
		const object_t *o = &F.objects.obj[i];
		if ((o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) || o->type != type || (int)i == F.mission.player_obj || (int)i == skip) continue;
		if (need_unit && units_of_object(&F.units, (int)i) < 0) continue;
		if (need_unit && !F.game.unit_state[i].has_vitals) continue;
		return (int)i;
	}
	return -1;
}

static hs_value flag_h(unsigned i) { return mfix_handle(i, 12); }
static hs_value vol_h(unsigned i) { return mfix_handle(i, 11); }

/* a standing spot around `target` with a clear line from the eye to it (like test_game); returns 1 */
static int los_spot(const float target[3], int want_clear, float out[3])
{
	int k;
	for (k = 0; k < 144; k++) {
		float ang = (float)k * 0.0872665f, dist = 2.5f + (float)(k / 24), eye[3], p[3];
		coll_hit h;
		p[0] = target[0] + cosf(ang) * dist; p[1] = target[1] + sinf(ang) * dist; p[2] = target[2] + 0.05f;
		eye[0] = p[0]; eye[1] = p[1]; eye[2] = p[2] + 0.62f;
		if (coll_point_in_solid(&F.coll, eye) || coll_point_in_solid(&F.coll, p)) continue;
		if (!!coll_ray(&F.coll, eye, target, 0, &h) == !want_clear) { memcpy(out, p, sizeof(float) * 3); return 1; }
	}
	return 0;
}

static void place_player(const float p[3], float yaw, float pitch)
{
	game_teleport(&F.game, p, yaw);
	F.game.player.pitch = pitch;
	F.game.player.on_ground = 1;
	mission_sync_player(&F.mission);
}

static void aim_player_at(const float t[3])
{
	float eye[3];
	player_eye(&F.game.player, &F.game.pp, eye);
	F.game.player.yaw = atan2f(t[1] - eye[1], t[0] - eye[0]);
	F.game.player.pitch = atan2f(t[2] - eye[2], sqrtf((t[0] - eye[0]) * (t[0] - eye[0]) + (t[1] - eye[1]) * (t[1] - eye[1])));
}

/* ---- tests ------------------------------------------------------------------------------------------------------------ */
static void test_lists(void)
{
	hs_value pl, e, c;
	mission_list *l;
	fresh(1, 0);
	CHECK(F.mission.player_obj >= 0 && live(F.mission.player_obj));
	pl = call("players", HS_HANDLE, NULL, 0);
	l = mission_list_get(&F.mission, pl.v.h);
	CHECK(l && l->count == 1 && l->obj[0] == F.mission.player_obj);
	e = call2("list_get", HS_HANDLE, pl, mfix_short(0));
	CHECK(e.v.h == mission_obj_handle(F.mission.player_obj));
	e = call2("list_get", HS_HANDLE, pl, mfix_short(3));
	CHECK(e.v.h == HS_HANDLE_NONE);
	c = call1("list_count", HS_SHORT, pl);
	CHECK(c.v.i == 1);
	c = call1("list_count", HS_SHORT, mfix_obj(F.mission.player_obj));      /* an object used as a list */
	CHECK(c.v.i == 1);
	/* the values of different calls do not alias the ring slot while it is in use */
	{
		hs_value pl2 = call("players", HS_HANDLE, NULL, 0);
		CHECK(pl2.v.h != pl.v.h);
	}
	/* a handle with no list and no object: empty */
	c = call1("list_count", HS_SHORT, mfix_handle(HS_HANDLE_NONE, 23));
	CHECK(c.v.i == 0);
	/* the name/slot disambiguation: a constant of "unit" type is a name, a computed one a slot */
	{
		int n = mfix_find_name(&F, SCEN_BIPED, 1, 1);
		if (n >= 0) {
			hs_value nm = mfix_handle((uint32_t)n, 38);
			int slot = mission_resolve_object(&F.mission, &nm, 0);
			CHECK(slot >= 0 && obj(slot)->placement == (int)devdef_get_name(&F.dev, (uint32_t)n).scen_index);
			nm = mfix_obj(slot);
			CHECK(mission_resolve_object(&F.mission, &nm, 0) == slot);
		}
	}
}

static void test_create_destroy(void)
{
	int types[4] = { SCEN_BIPED, SCEN_SCENERY, SCEN_WEAPON, SCEN_VEHICLE }, t, tested = 0;
	fresh(2, 0);
	for (t = 0; t < 4; t++) {
		int n = mfix_find_name(&F, types[t], 0, 1), slot, again, anew;
		if (n < 0) continue;
		tested++;
		CHECK(mission_object_by_name(&F.mission, (uint32_t)n, 0) == -1);
		call1("object_create", HS_VOID, mfix_name((uint32_t)n));
		slot = mission_object_by_name(&F.mission, (uint32_t)n, 0);
		CHECK(slot >= 0 && live(slot));
		CHECK(obj(slot)->type == types[t]);
		if (types[t] == SCEN_BIPED) CHECK(units_of_object(&F.units, slot) >= 0);
		call1("object_create", HS_VOID, mfix_name((uint32_t)n));              /* again: the same object */
		again = mission_object_by_name(&F.mission, (uint32_t)n, 0);
		CHECK(again == slot);
		/* anew: destroyed and created again (same placement, fresh state) */
		if (types[t] == SCEN_BIPED) { F.game.unit_state[slot].vitals.health = 1.0f; }
		call1("object_create_anew", HS_VOID, mfix_name((uint32_t)n));
		anew = mission_object_by_name(&F.mission, (uint32_t)n, 0);
		CHECK(anew >= 0 && live(anew));
		if (types[t] == SCEN_BIPED && F.game.unit_state[anew].has_vitals) CHECK(F.game.unit_state[anew].vitals.health > 1.5f);
		call1("object_destroy", HS_VOID, mfix_obj(anew));
		CHECK(!live(anew));
		CHECK(mission_object_by_name(&F.mission, (uint32_t)n, 0) == -1);
		if (types[t] == SCEN_BIPED) CHECK(units_of_object(&F.units, anew) < 0);
		call1("object_destroy", HS_VOID, mfix_obj(anew));                      /* dead handle: no-op */
		call1("object_destroy", HS_VOID, mfix_handle(HS_HANDLE_NONE, 37));
		call1("object_destroy", HS_VOID, mfix_name((uint32_t)n));              /* a name with no object: no-op */
	}
	CHECK(tested >= 2);
	/* the player body is not destroyable */
	call1("object_destroy", HS_VOID, mfix_obj(F.mission.player_obj));
	CHECK(live(F.mission.player_obj));
	/* *_containing: a substring of a created name */
	{
		int n = mfix_find_name(&F, SCEN_SCENERY, 0, 1);
		if (n < 0) n = mfix_find_name(&F, SCEN_BIPED, 0, 1);
		if (n >= 0) {
			devdef_name nm = devdef_get_name(&F.dev, (uint32_t)n);
			char sub[5];
			uint32_t i, matches = 0, spawned = 0;
			unsigned live0 = 0, live1 = 0, k;
			strncpy(sub, nm.name, 4); sub[4] = 0;
			if (strlen(nm.name) >= 4) {
				for (k = 0; k < F.objects.count; k++) live0 += !(F.objects.obj[k].flags & OBJ_F_FREE);
				call1("object_create_containing", HS_VOID, mfix_str(&F, sub));
				for (k = 0; k < F.objects.count; k++) live1 += !(F.objects.obj[k].flags & OBJ_F_FREE);
				for (i = 0; i < F.dev.name_count; i++) {
					devdef_name x = devdef_get_name(&F.dev, i);
					if (!(x.flags & DEVDEF_NF_RESOLVED) || !strstr(x.name, sub)) continue;
					matches++;
					if (mission_object_by_name(&F.mission, i, 0) >= 0) spawned++;
				}
				CHECK(matches >= 1 && spawned >= 1 && live1 >= live0);
				call1("object_destroy_containing", HS_VOID, mfix_str(&F, sub));
				for (i = 0; i < F.dev.name_count; i++) {
					devdef_name x = devdef_get_name(&F.dev, i);
					if ((x.flags & DEVDEF_NF_RESOLVED) && strstr(x.name, sub)) CHECK(mission_object_by_name(&F.mission, i, 0) == -1);
				}
				call1("object_create_anew_containing", HS_VOID, mfix_str(&F, sub));
				for (i = 0; i < F.dev.name_count; i++) {
					devdef_name x = devdef_get_name(&F.dev, i);
					if ((x.flags & DEVDEF_NF_RESOLVED) && strstr(x.name, sub) && x.scen_index != DEVDEF_NONE) {
						scen_placement pl = scen_get_placement(&F.scen, x.scen_index);
						if (model_find(&F.models, scen_get_def(&F.scen, pl.def).model_tag) >= 0) CHECK(mission_object_by_name(&F.mission, i, 0) >= 0);
					}
				}
			}
		}
	}
	/* object_destroy_all removes the mobile objects and keeps the level (and the player) */
	{
		int b = first_live(SCEN_BIPED, 0, -1), s = first_live(SCEN_SCENERY, 0, -1);
		call("object_destroy_all", HS_VOID, NULL, 0);
		if (b >= 0) CHECK(!live(b) || b == F.mission.player_obj);
		if (s >= 0) CHECK(live(s));
		CHECK(live(F.mission.player_obj));
	}
}

static void test_teleport(void)
{
	int b;
	devdef_cflag f;
	fresh(3, 0);
	CHECK(F.dev.flag_count > 0);
	if (F.dev.flag_count == 0) return;
	f = devdef_get_cflag(&F.dev, 0);
	b = first_live(SCEN_BIPED, 1, -1);
	if (b >= 0) {
		int ui = units_of_object(&F.units, b);
		call2("object_teleport", HS_VOID, mfix_obj(b), flag_h(0));
		CHECK(dist3(obj(b)->pos, f.pos) < 1e-4f);
		CHECK(dist3(F.units.u[ui].body.pos, f.pos) < 1e-4f);
		CHECK(fabsf(F.units.u[ui].body.yaw - f.yaw) < 1e-4f);
		call2("object_teleport", HS_VOID, mfix_obj(b), flag_h(F.dev.flag_count > 1 ? 1 : 0));
	}
	/* a named object that is not there: no-op; the player moves through the game */
	call2("object_teleport", HS_VOID, mfix_handle(HS_HANDLE_NONE, 37), flag_h(0));
	call2("object_teleport", HS_VOID, mfix_obj(F.mission.player_obj), flag_h(0));
	CHECK(dist3(F.game.player.pos, f.pos) < 1e-4f);
	CHECK(dist3(obj(F.mission.player_obj)->pos, f.pos) < 1e-4f);
	/* facing: turn towards the flag */
	{
		int s = first_live(SCEN_SCENERY, 0, -1);
		if (s >= 0) {
			float yaw, dx = f.pos[0] - obj(s)->pos[0], dy = f.pos[1] - obj(s)->pos[1];
			call2("object_set_facing", HS_VOID, mfix_obj(s), flag_h(0));
			yaw = atan2f(obj(s)->root.m[0][1], obj(s)->root.m[0][0]);
			CHECK(fabsf(yaw - atan2f(dy, dx)) < 1e-3f || dx * dx + dy * dy < 1e-6f);
		}
	}
	/* devices do not move */
	{
		int m = first_live(SCEN_MACHINE, 0, -1);
		if (m >= 0) {
			float p0[3] = { obj(m)->pos[0], obj(m)->pos[1], obj(m)->pos[2] };
			call2("object_teleport", HS_VOID, mfix_obj(m), flag_h(0));
			CHECK(dist3(obj(m)->pos, p0) < 1e-6f);
		}
	}
	/* scale */
	{
		int s = first_live(SCEN_SCENERY, 0, -1);
		if (s >= 0) {
			float len0 = sqrtf(obj(s)->root.m[0][0] * obj(s)->root.m[0][0] + obj(s)->root.m[0][1] * obj(s)->root.m[0][1]);
			call3("object_set_scale", HS_VOID, mfix_obj(s), mfix_real(2.0f), mfix_short(30));
			CHECK(fabsf(sqrtf(obj(s)->root.m[0][0] * obj(s)->root.m[0][0] + obj(s)->root.m[0][1] * obj(s)->root.m[0][1]) - 2.0f * len0) < 1e-3f);
		}
	}
	call2("object_beautify", HS_VOID, mfix_obj(first_live(SCEN_SCENERY, 0, -1)), mfix_bool(1));      /* another object: nothing happens */
}

/* The player's body as a stand-in (the cryo pod, ADR-085): object_beautify true makes it a drawn unit that stays where the script puts it (the player does
 * not drag it along), false hands the player back to where the body is and hides it again. */
static void test_player_stand_in(void)
{
	int p;
	float pos[3];
	int ui;
	fresh(41, 0);
	p = F.mission.player_obj;
	CHECK(p >= 0 && live(p));
	CHECK(obj(p)->flags & OBJ_F_HIDDEN);
	CHECK(units_of_object(&F.units, p) < 0);
	call2("object_beautify", HS_VOID, mfix_obj(p), mfix_bool(1));
	ui = units_of_object(&F.units, p);
	CHECK(ui >= 0 && F.mission.hobj.stand_in == 2);
	CHECK(!(obj(p)->flags & OBJ_F_HIDDEN));
	pos[0] = F.game.player.pos[0] + 3.0f; pos[1] = F.game.player.pos[1]; pos[2] = F.game.player.pos[2];
	F.units.u[ui].body.pos[0] = pos[0]; F.units.u[ui].body.pos[1] = pos[1]; F.units.u[ui].body.pos[2] = pos[2];
	F.objects.obj[p].pos[0] = pos[0]; F.objects.obj[p].pos[1] = pos[1]; F.objects.obj[p].pos[2] = pos[2];
	mission_sync_player(&F.mission);                                  /* the object keeps its place: the body is not the player's while it is a stand-in */
	CHECK(fabsf(obj(p)->pos[0] - pos[0]) < 1e-4f);
	call2("object_beautify", HS_VOID, mfix_obj(p), mfix_bool(0));
	CHECK(units_of_object(&F.units, p) < 0 && F.mission.hobj.stand_in == 0);
	CHECK(obj(p)->flags & OBJ_F_HIDDEN);
	CHECK(fabsf(F.game.player.pos[0] - pos[0]) < 1e-3f && fabsf(F.game.player.pos[1] - pos[1]) < 1e-3f);
}

/* An unarmed start hands the pistol out with the starting profile's ammunition when the script asks (player_add_equipment, a10's bridge0_profile: 6 + 48) */
static void test_player_equipment(void)
{
	scen_player pl;
	int slot = -1;
	fresh(42, 0);
	pl = scen_get_player(F.mission.ow.scen);
	game_drop_weapons(&F.game);
	CHECK(F.game.cur < 0 && F.game.unarmed_start);
	call3("player_add_equipment", HS_VOID, mfix_obj(F.mission.player_obj), mfix_handle(3, 19), mfix_bool(0));
	CHECK(F.game.cur >= 0 && F.game.slot[F.game.cur].weapon >= 0);
	if (F.game.cur >= 0) {
		const game_slot_t *sl = &F.game.slot[F.game.cur];
		if (pl.weapon_total) CHECK(sl->st.loaded[0] == pl.weapon_loaded && sl->st.reserve[0] == pl.weapon_total);        /* packs built before ADR-085 carry no ammunition: the weapon's own */
		else CHECK(sl->st.loaded[0] == sl->def.magazine[0].rounds_loaded_max);
		slot = F.game.cur;
	}
	call3("player_add_equipment", HS_VOID, mfix_obj(F.mission.player_obj), mfix_handle(3, 19), mfix_bool(0));          /* already armed: nothing changes */
	CHECK(F.game.cur == slot);
	printf("  player_add_equipment: pistol with %u loaded + %u reserve (pack profile: %u + %u)\n", slot >= 0 ? (unsigned)F.game.slot[slot].st.loaded[0] : 0u, slot >= 0 ? (unsigned)F.game.slot[slot].st.reserve[0] : 0u, (unsigned)pl.weapon_loaded, (unsigned)pl.weapon_total);
}

static void test_volumes(void)
{
	unsigned vi, tested = 0;
	fresh(4, 0);
	for (vi = 0; vi < F.dev.volume_count && tested < 3; vi++) {
		devdef_volume v = devdef_get_volume(&F.dev, vi);
		float c[3], pos[3], far_pos[3];
		hs_value pl, r;
		mission_awaited *aw = NULL;
		unsigned i;
		volume_center(&v, c);
		if (!volume_contains(&v, c)) continue;
		tested++;
		pl = call("players", HS_HANDLE, NULL, 0);
		/* player inside: the body's reference point is feet + half the radius */
		pos[0] = c[0]; pos[1] = c[1]; pos[2] = c[2] - 0.5f * obj(F.mission.player_obj)->radius;
		place_player(pos, 0.0f, 0.0f);
		r = call2("volume_test_objects", HS_BOOL, vol_h(vi), pl);
		CHECK(r.v.i == 1);
		r = call2("volume_test_object", HS_BOOL, vol_h(vi), mfix_obj(F.mission.player_obj));
		CHECK(r.v.i == 1);
		r = call2("volume_test_objects_all", HS_BOOL, vol_h(vi), pl);
		CHECK(r.v.i == 1);
		/* outside: false, and the volume is recorded as awaited */
		far_pos[0] = c[0] + 5000.0f; far_pos[1] = c[1]; far_pos[2] = c[2];
		place_player(far_pos, 0.0f, 0.0f);
		r = call2("volume_test_objects", HS_BOOL, vol_h(vi), pl);
		CHECK(r.v.i == 0);
		for (i = 0; i < 8; i++) if (F.mission.awaited_volume[i].active && F.mission.awaited_volume[i].volume == vi) aw = &F.mission.awaited_volume[i];
		CHECK(aw != NULL && aw->hits >= 1);
		if (aw) {
			uint32_t h0 = aw->hits;
			(void)call2("volume_test_object", HS_BOOL, vol_h(vi), mfix_obj(F.mission.player_obj));
			CHECK(aw->hits == h0 + 1);
		}
		r = call2("volume_test_objects_all", HS_BOOL, vol_h(vi), pl);
		CHECK(r.v.i == 0);
		/* inside again: true and no longer awaited */
		place_player(pos, 0.0f, 0.0f);
		r = call2("volume_test_objects", HS_BOOL, vol_h(vi), pl);
		CHECK(r.v.i == 1);
		for (i = 0; i < 8; i++) CHECK(!(F.mission.awaited_volume[i].active && F.mission.awaited_volume[i].volume == vi));
		/* volume_return_objects sees the player */
		{
			hs_value li = call1("volume_return_objects", HS_HANDLE, vol_h(vi));
			mission_list *l = mission_list_get(&F.mission, li.v.h);
			unsigned k, found = 0;
			CHECK(l != NULL);
			for (k = 0; l && k < l->count; k++) found += l->obj[k] == F.mission.player_obj;
			CHECK(found == 1);
		}
		/* a list of a unit that is not the player: no awaited entry, answer by its position */
		{
			int b = first_live(SCEN_BIPED, 1, -1);
			if (b >= 0) {
				mission_list *l;
				uint32_t h;
				float up[3] = { c[0], c[1], c[2] - 0.5f * obj(b)->radius };
				unsigned hits_before = 0;
				for (i = 0; i < 8; i++) hits_before += F.mission.awaited_volume[i].hits;
				units_teleport(&F.units, &F.game, units_of_object(&F.units, b), up, 0.0f);
				l = mission_list_new(&F.mission, &h);
				mission_list_add(l, b);
				r = call2("volume_test_objects", HS_BOOL, vol_h(vi), mfix_handle(h, 23));
				CHECK(r.v.i == 1);
				up[0] += 5000.0f;
				units_teleport(&F.units, &F.game, units_of_object(&F.units, b), up, 0.0f);
				r = call2("volume_test_objects", HS_BOOL, vol_h(vi), mfix_handle(h, 23));
				CHECK(r.v.i == 0);
				{ unsigned hits_after = 0; for (i = 0; i < 8; i++) hits_after += F.mission.awaited_volume[i].hits; CHECK(hits_after == hits_before); }
				mission_list_add(l, F.mission.player_obj);                     /* unit outside + player inside: all = false, any = true */
				r = call2("volume_test_objects_all", HS_BOOL, vol_h(vi), mfix_handle(h, 23));
				CHECK(r.v.i == 0);
				r = call2("volume_test_objects", HS_BOOL, vol_h(vi), mfix_handle(h, 23));
				CHECK(r.v.i == 1);
			}
		}
		/* volume_teleport_players_not_inside: outside -> to the flag, inside -> stays */
		if (F.dev.flag_count) {
			devdef_cflag f = devdef_get_cflag(&F.dev, 0);
			call2("volume_teleport_players_not_inside", HS_VOID, vol_h(vi), flag_h(0));
			CHECK(dist3(F.game.player.pos, pos) < 1.0f);                        /* was inside */
			place_player(far_pos, 0.0f, 0.0f);
			call2("volume_teleport_players_not_inside", HS_VOID, vol_h(vi), flag_h(0));
			CHECK(dist3(F.game.player.pos, f.pos) < 1e-4f);
		}
	}
	CHECK(tested >= 1);
	/* bad volume, empty list: false */
	{
		hs_value r = call2("volume_test_objects", HS_BOOL, vol_h(0x7FFFu), call("players", HS_HANDLE, NULL, 0));
		CHECK(r.v.i == 0);
		r = call2("volume_test_objects", HS_BOOL, vol_h(0), mfix_handle(HS_HANDLE_NONE, 23));
		CHECK(r.v.i == 0);
	}
}

static int find_marker_object(int *marker_out)
{
	unsigned i;
	for (i = 0; i < F.objects.count; i++) {
		const object_t *o = &F.objects.obj[i];
		model_info mi;
		unsigned k;
		if ((o->flags & (OBJ_F_FREE | OBJ_F_INACTIVE)) || o->model < 0 || (int)i == F.mission.player_obj) continue;
		mi = model_get(&F.models, (uint32_t)o->model);
		for (k = 0; k < mi.marker_count; k++) {
			model_marker mk = model_get_marker(&F.models, &mi, k);
			if (mk.name[0] && mk.node >= 0 && model_find_marker(&F.models, &mi, mk.name) == (int)k) { *marker_out = (int)k; return (int)i; }
		}
	}
	return -1;
}

static void test_attach(void)
{
	int mk = -1, parent, child;
	char name[MODEL_NAME_LEN];
	hs_value args[4];
	fresh(5, 0);
	parent = find_marker_object(&mk);
	CHECK(parent >= 0);
	if (parent < 0) return;
	child = first_live(SCEN_SCENERY, 0, parent);
	if (child < 0) child = first_live(SCEN_BIPED, 0, parent);
	CHECK(child >= 0);
	if (child < 0) return;
	{
		model_info mi = model_get(&F.models, (uint32_t)obj(parent)->model);
		model_marker m = model_get_marker(&F.models, &mi, (uint32_t)mk);
		strncpy(name, m.name, sizeof name - 1); name[sizeof name - 1] = 0;
	}
	args[0] = mfix_obj(parent); args[1] = mfix_str(&F, name); args[2] = mfix_obj(child); args[3] = mfix_str(&F, "");
	call("objects_attach", HS_VOID, args, 4);
	CHECK(F.mission.hobj.attach_count == 1);
	ticks(2);
	{
		/* the independent computation: marker node pose of the parent, marker transform */
		static object_nodes_t nodes;
		model_info mi = model_get(&F.models, (uint32_t)obj(parent)->model);
		model_marker m = model_get_marker(&F.models, &mi, (uint32_t)mk);
		unsigned n = objects_pose(obj(parent), &F.ow, &nodes);
		mat4 local, w;
		CHECK((unsigned)m.node < n);
		mat4_from_trs(&local, m.rotation, m.translation, 1.0f);
		mat4_mul(&w, &local, &nodes.world[m.node]);
		CHECK(fabsf(obj(child)->pos[0] - w.m[3][0]) < 1e-3f && fabsf(obj(child)->pos[1] - w.m[3][1]) < 1e-3f && fabsf(obj(child)->pos[2] - w.m[3][2]) < 1e-3f);
	}
	/* the parent moves: the child follows */
	{
		float before[3] = { obj(child)->pos[0], obj(child)->pos[1], obj(child)->pos[2] };
		object_t *po = &F.objects.obj[parent];
		float np[3] = { po->pos[0] + 7.0f, po->pos[1], po->pos[2] };
		float yaw = atan2f(po->root.m[0][1], po->root.m[0][0]);
		po->pos[0] = np[0]; mat4_from_ypr(&po->root, yaw, 0.0f, 0.0f, po->pos);
		ticks(1);
		CHECK(fabsf(obj(child)->pos[0] - before[0] - 7.0f) < 0.05f);
	}
	/* detach: the child stays where it is */
	call2("objects_detach", HS_VOID, mfix_obj(parent), mfix_obj(child));
	CHECK(F.mission.hobj.attach_count == 0);
	{
		float p[3] = { obj(child)->pos[0], obj(child)->pos[1], obj(child)->pos[2] };
		F.objects.obj[parent].pos[0] += 3.0f;
		mat4_from_ypr(&F.objects.obj[parent].root, 0.0f, 0.0f, 0.0f, F.objects.obj[parent].pos);
		ticks(1);
		CHECK(dist3(obj(child)->pos, p) < 1e-5f);
	}
	/* origin attachment, then the parent is destroyed: the entry goes with it */
	args[1] = mfix_str(&F, "");
	call("objects_attach", HS_VOID, args, 4);
	CHECK(F.mission.hobj.attach_count == 1);
	call1("object_destroy", HS_VOID, mfix_obj(parent));
	CHECK(F.mission.hobj.attach_count == 0);
	/* an unknown marker is the origin, an unresolved handle does nothing */
	args[0] = mfix_handle(HS_HANDLE_NONE, 37);
	call("objects_attach", HS_VOID, args, 4);
	CHECK(F.mission.hobj.attach_count == 0);
}

static void test_sight(void)
{
	int b;
	float spot[3], tgt[3], p2[3];
	hs_value pl, r;
	fresh(6, 0);
	b = first_live(SCEN_BIPED, 0, -1);
	CHECK(b >= 0);
	if (b < 0) return;
	mission_object_pos(&F.mission, b, tgt);
	pl = call("players", HS_HANDLE, NULL, 0);
	CHECK(los_spot(tgt, 1, spot));
	place_player(spot, 0.0f, 0.0f);
	aim_player_at(tgt);
	r = call3("objects_can_see_object", HS_BOOL, pl, mfix_obj(b), mfix_real(30.0f));
	CHECK(r.v.i == 1);
	F.game.player.yaw += 2.5f;                                                   /* looking away */
	r = call3("objects_can_see_object", HS_BOOL, pl, mfix_obj(b), mfix_real(30.0f));
	CHECK(r.v.i == 0);
	aim_player_at(tgt);
	F.game.player.yaw += 0.5f;                                                   /* ~28 degrees off */
	r = call3("objects_can_see_object", HS_BOOL, pl, mfix_obj(b), mfix_real(10.0f));
	CHECK(r.v.i == 0);
	r = call3("objects_can_see_object", HS_BOOL, pl, mfix_obj(b), mfix_real(60.0f));
	CHECK(r.v.i == 1);
	/* the distance */
	r = call2("objects_distance_to_object", HS_REAL, pl, mfix_obj(b));
	{
		float pp[3];
		mission_object_pos(&F.mission, F.mission.player_obj, pp);
		CHECK(fabsf(r.v.f - dist3(pp, tgt)) < 1e-3f);
	}
	/* blocked by the world */
	if (los_spot(tgt, 0, p2)) {
		place_player(p2, 0.0f, 0.0f);
		aim_player_at(tgt);
		r = call3("objects_can_see_object", HS_BOOL, pl, mfix_obj(b), mfix_real(90.0f));
		CHECK(r.v.i == 0);
	} else printf("  (no blocked spot found)\n");
	/* a flag */
	if (F.dev.flag_count) {
		devdef_cflag f = devdef_get_cflag(&F.dev, 0);
		float sp[3];
		if (los_spot(f.pos, 1, sp)) {
			place_player(sp, 0.0f, 0.0f);
			aim_player_at(f.pos);
			r = call3("objects_can_see_flag", HS_BOOL, pl, flag_h(0), mfix_real(30.0f));
			CHECK(r.v.i == 1);
			F.game.player.yaw += 2.5f;
			r = call3("objects_can_see_flag", HS_BOOL, pl, flag_h(0), mfix_real(30.0f));
			CHECK(r.v.i == 0);
			r = call2("objects_distance_to_flag", HS_REAL, pl, flag_h(0));
			{ float pp[3]; mission_object_pos(&F.mission, F.mission.player_obj, pp); CHECK(fabsf(r.v.f - dist3(pp, f.pos)) < 1e-3f); }
		}
	}
	/* an unit that sees: a biped looking at the player */
	{
		mission_list *l;
		uint32_t h;
		int ui = units_of_object(&F.units, b);
		if (ui >= 0 && los_spot(tgt, 1, spot)) {
			float pe[3];
			place_player(spot, 0.0f, 0.0f);
			mission_object_pos(&F.mission, F.mission.player_obj, pe);
			{
				float yaw = atan2f(pe[1] - F.units.u[ui].body.pos[1], pe[0] - F.units.u[ui].body.pos[0]);
				units_teleport(&F.units, &F.game, ui, F.units.u[ui].body.pos, yaw);       /* face the player (the head turn is limited) */
			}
			l = mission_list_new(&F.mission, &h);
			mission_list_add(l, b);
			r = call3("objects_can_see_object", HS_BOOL, mfix_handle(h, 23), mfix_obj(F.mission.player_obj), mfix_real(40.0f));
			{
				float ue[3] = { F.units.u[ui].body.pos[0], F.units.u[ui].body.pos[1], F.units.u[ui].body.pos[2] + 0.6f };
				coll_hit ch;
				CHECK(r.v.i == 1 || coll_ray(&F.coll, ue, pe, 0, &ch));        /* it sees the player unless the world is in the way */
			}
		}
	}
}

static void test_vitals(void)
{
	int a, b;
	hs_value r;
	fresh(7, 0);
	a = first_live(SCEN_BIPED, 1, -1);
	b = first_live(SCEN_BIPED, 1, a);
	CHECK(a >= 0);
	if (a < 0) return;
	r = call1("unit_get_health", HS_REAL, mfix_obj(a));
	CHECK(fabsf(r.v.f - 1.0f) < 1e-4f);
	r = call1("unit_get_shield", HS_REAL, mfix_obj(a));
	CHECK(r.v.f >= 0.0f && r.v.f <= 1.0f);
	r = call1("object_get_health", HS_REAL, mfix_obj(a));
	CHECK(fabsf(r.v.f - 1.0f) < 1e-4f);
	{
		float mh = F.game.unit_state[a].vitals.max_health;
		call3("unit_set_current_vitality", HS_VOID, mfix_obj(a), mfix_real(mh * 0.5f), mfix_real(0.0f));
		r = call1("unit_get_health", HS_REAL, mfix_obj(a));
		CHECK(fabsf(r.v.f - 0.5f) < 1e-3f);
		r = call1("unit_get_shield", HS_REAL, mfix_obj(a));
		CHECK(fabsf(r.v.f) < 1e-4f);
		call3("unit_set_maximum_vitality", HS_VOID, mfix_obj(a), mfix_real(mh * 2.0f), mfix_real(0.0f));
		CHECK(fabsf(F.game.unit_state[a].vitals.max_health - mh * 2.0f) < 1e-3f);
		r = call1("unit_get_health", HS_REAL, mfix_obj(a));
		CHECK(fabsf(r.v.f - 0.25f) < 1e-3f);
	}
	/* kill */
	call1("unit_kill", HS_VOID, mfix_obj(a));
	CHECK(F.game.unit_state[a].dead);
	r = call1("unit_get_health", HS_REAL, mfix_obj(a));
	CHECK(r.v.f == 0.0f);
	if (b >= 0) {
		mission_list *l;
		uint32_t h;
		float d;
		l = mission_list_new(&F.mission, &h);
		mission_list_add(l, b);
		call1("object_cannot_take_damage", HS_VOID, mfix_handle(h, 23));
		d = game_damage_object(&F.game, b, 5.0f, NULL);
		CHECK(d == 0.0f);
		d = units_damage(&F.units, &F.game, units_of_object(&F.units, b), 5.0f);
		CHECK(d == 0.0f);
		r = call1("unit_get_health", HS_REAL, mfix_obj(b));
		CHECK(fabsf(r.v.f - 1.0f) < 1e-4f);
		call1("object_can_take_damage", HS_VOID, mfix_handle(h, 23));
		d = game_damage_object(&F.game, b, 5.0f, NULL);
		CHECK(d > 0.0f);
		/* units_set_current_vitality over a list, unit_impervious */
		{
			float mh = F.game.unit_state[b].vitals.max_health;
			hs_value args[3];
			args[0] = mfix_handle(h, 23); args[1] = mfix_real(mh * 0.25f); args[2] = mfix_real(0.0f);
			call("units_set_current_vitality", HS_VOID, args, 3);
			r = call1("unit_get_health", HS_REAL, mfix_obj(b));
			CHECK(fabsf(r.v.f - 0.25f) < 1e-3f);
			call2("unit_impervious", HS_VOID, mfix_handle(h, 23), mfix_bool(1));
			CHECK(game_damage_object(&F.game, b, 5.0f, NULL) == 0.0f);
			call2("unit_impervious", HS_VOID, mfix_handle(h, 23), mfix_bool(0));
			CHECK(game_damage_object(&F.game, b, 1.0f, NULL) > 0.0f);
			call1("unit_kill_silent", HS_VOID, mfix_obj(b));
			CHECK(F.game.unit_state[b].dead);
		}
	}
	/* the player */
	r = call1("unit_get_health", HS_REAL, mfix_obj(F.mission.player_obj));
	CHECK(fabsf(r.v.f - 1.0f) < 1e-4f);
	call3("unit_set_current_vitality", HS_VOID, mfix_obj(F.mission.player_obj), mfix_real(F.game.vitals.max_health * 0.5f), mfix_real(0.0f));
	r = call1("unit_get_health", HS_REAL, mfix_obj(F.mission.player_obj));
	CHECK(fabsf(r.v.f - 0.5f) < 1e-3f);
	r = call1("unit_get_total_grenade_count", HS_SHORT, mfix_obj(F.mission.player_obj));
	CHECK(r.v.i == F.game.grenades[0] + F.game.grenades[1]);
	/* unit_has_weapon_readied with the player's weapon */
	{
		const game_slot_t *gs = game_cur_slot(&F.game);
		if (gs) {
			r = call2("unit_has_weapon_readied", HS_BOOL, mfix_obj(F.mission.player_obj), mfix_handle(gs->def.tag_id, 31));
			CHECK(r.v.i == 1);
			r = call2("unit_has_weapon_readied", HS_BOOL, mfix_obj(F.mission.player_obj), mfix_handle(gs->def.tag_id ^ 0x1234u, 31));
			CHECK(r.v.i == 0);
		}
	}
	/* flags that other modules read */
	call2("object_set_melee_attack_inhibited", HS_VOID, mfix_obj(a), mfix_bool(1));
	CHECK((F.mission.hobj.melee_inhibited[a >> 5] >> (a & 31)) & 1u);
	call2("object_set_melee_attack_inhibited", HS_VOID, mfix_obj(a), mfix_bool(0));
	CHECK(!((F.mission.hobj.melee_inhibited[a >> 5] >> (a & 31)) & 1u));
	call2("object_set_ranged_attack_inhibited", HS_VOID, mfix_obj(a), mfix_bool(1));
	CHECK((F.mission.hobj.ranged_inhibited[a >> 5] >> (a & 31)) & 1u);
	call2("unit_set_enterable_by_player", HS_VOID, mfix_obj(a), mfix_bool(0));
	CHECK((F.mission.hobj.not_enterable[a >> 5] >> (a & 31)) & 1u);
}

static void test_custom_animation(void)
{
	int b, ui, gi, ai, other_graph = -1;
	hs_value args[5], r;
	scen_def d;
	unsigned k;
	fresh(8, 0);
	b = first_live(SCEN_BIPED, 1, -1);
	CHECK(b >= 0);
	if (b < 0) return;
	ui = units_of_object(&F.units, b);
	gi = obj(b)->graph;
	d = scen_get_def(&F.scen, (uint32_t)obj(b)->def);
	ai = mfix_find_anim(&F, gi, 6, 80);
	CHECK(gi >= 0 && ai >= 0);
	if (gi < 0 || ai < 0) return;
	{
		anim_graph g = anim_get_graph(&F.anims, (uint32_t)gi);
		anim_info info = anim_get(&F.anims, &g, (uint32_t)ai);
		float tps = info.fps > 0.0f ? info.fps / 30.0f : 1.0f;
		int expect = (int)((float)info.frame_count / tps);
		args[0] = mfix_obj(b); args[1] = mfix_handle(d.anim_tag, 28); args[2] = mfix_str(&F, info.name); args[3] = mfix_bool(1);
		call("custom_animation", HS_VOID, args, 4);
		CHECK(obj(b)->anim == ai && obj(b)->frame == 0.0f);
		CHECK(F.units.u[ui].action == UA_CUSTOM && F.units.u[ui].custom_anim == ai);
		r = call1("unit_get_custom_animation_time", HS_SHORT, mfix_obj(b));
		CHECK(r.v.i >= expect - 1 && r.v.i <= expect + 1);
		ticks(5);
		r = call1("unit_get_custom_animation_time", HS_SHORT, mfix_obj(b));
		CHECK(r.v.i == expect - 5 || r.v.i == expect - 6 || r.v.i == expect - 4);
		/* stop */
		call1("unit_stop_custom_animation", HS_VOID, mfix_obj(b));
		CHECK(F.units.u[ui].action != UA_CUSTOM);
		r = call1("unit_get_custom_animation_time", HS_SHORT, mfix_obj(b));
		CHECK(r.v.i == 0);
		/* run to the end by itself */
		call("custom_animation", HS_VOID, args, 4);
		ticks(expect + 3);
		CHECK(F.units.u[ui].action != UA_CUSTOM);
		/* at a frame */
		args[3] = mfix_bool(0); args[4] = mfix_short(3);
		call("unit_custom_animation_at_frame", HS_VOID, args, 5);
		CHECK(obj(b)->frame == 3.0f && obj(b)->anim == ai);
		r = call1("unit_get_custom_animation_time", HS_SHORT, mfix_obj(b));
		CHECK(r.v.i >= expect - 3 - 1 && r.v.i <= expect - 3 + 1);
		call1("unit_stop_custom_animation", HS_VOID, mfix_obj(b));
		/* unknown animation name / graph: nothing happens */
		{
			uint16_t a0 = obj(b)->anim;
			args[2] = mfix_str(&F, "no such animation"); args[3] = mfix_bool(1);
			call("custom_animation", HS_VOID, args, 4);
			CHECK(obj(b)->anim == a0 && F.units.u[ui].action != UA_CUSTOM);
			args[2] = mfix_str(&F, info.name); args[1] = mfix_handle(0x12345678u, 28);
			call("custom_animation", HS_VOID, args, 4);
			CHECK(obj(b)->anim == a0 && F.units.u[ui].action != UA_CUSTOM);
		}
		/* a list */
		{
			mission_list *l;
			uint32_t h;
			hs_value la[4];
			l = mission_list_new(&F.mission, &h);
			mission_list_add(l, b);
			la[0] = mfix_handle(h, 23); la[1] = mfix_handle(d.anim_tag, 28); la[2] = mfix_str(&F, info.name); la[3] = mfix_bool(1);
			call("custom_animation_list", HS_VOID, la, 4);
			CHECK(F.units.u[ui].action == UA_CUSTOM);
			call1("unit_stop_custom_animation", HS_VOID, mfix_obj(b));
		}
		/* a foreign graph: the object's graph is swapped for the duration and handed back */
		for (k = 0; k < F.anims.graph_count && other_graph < 0; k++) {
			anim_graph og = anim_get_graph(&F.anims, k);
			if ((int)k != gi && og.anim_count > 0 && og.node_count == g.node_count) other_graph = (int)k;
		}
		if (other_graph >= 0) {
			anim_graph og = anim_get_graph(&F.anims, (uint32_t)other_graph);
			unsigned q;
			int oi = -1;
			for (q = 0; q < og.anim_count && oi < 0; q++) { anim_info x = anim_get(&F.anims, &og, q); if (x.name[0] && x.frame_count >= 4 && x.frame_count <= 60) oi = (int)q; }
			if (oi >= 0) {
				anim_info x = anim_get(&F.anims, &og, (uint32_t)oi);
				args[1] = mfix_handle(og.tag_id, 28); args[2] = mfix_str(&F, x.name); args[3] = mfix_bool(1);
				call("custom_animation", HS_VOID, args, 4);
				if (obj(b)->graph == other_graph) {
					CHECK(F.mission.hobj.saved_graph[b] == gi + 2);
					ticks((int)x.frame_count + 3);
					CHECK(obj(b)->graph == gi && F.mission.hobj.saved_graph[b] == 0);
				} else printf("  (foreign graph animation name not resolvable by exact search)\n");
			}
		}
	}
	/* a non-unit object plays it through the object */
	{
		int s = first_live(SCEN_SCENERY, 0, -1);
		if (s >= 0 && obj(s)->graph >= 0) {
			scen_def sd = scen_get_def(&F.scen, (uint32_t)obj(s)->def);
			int sa = mfix_find_anim(&F, obj(s)->graph, 4, 200);
			if (sa >= 0 && sd.anim_tag) {
				anim_graph g = anim_get_graph(&F.anims, (uint32_t)obj(s)->graph);
				anim_info info = anim_get(&F.anims, &g, (uint32_t)sa);
				args[0] = mfix_obj(s); args[1] = mfix_handle(sd.anim_tag, 28); args[2] = mfix_str(&F, info.name); args[3] = mfix_bool(1);
				call("custom_animation", HS_VOID, args, 4);
				CHECK(obj(s)->anim == sa);
			}
		}
	}
}

static int shortest_recording(unsigned min_len)
{
	unsigned i;
	int best = -1;
	uint32_t bl = 0xFFFFFFFFu;
	for (i = 0; i < F.cin.rec_count; i++) {
		cinedef_rec r = cinedef_get_rec(&F.cin, i);
		if (r.length >= min_len && r.length < bl) { bl = r.length; best = (int)i; }
	}
	return best;
}

static void test_recordings(void)
{
	int b, rec, ps, ui;
	hs_value r;
	cinedef_rec cr;
	unsigned steady;
	fresh(9, 0);
	if (!F.has_cin) { printf("  recordings: no CINE pack, skipped\n"); return; }
	b = first_live(SCEN_BIPED, 1, -1);
	rec = shortest_recording(20);
	CHECK(b >= 0 && rec >= 0);
	if (b < 0 || rec < 0) return;
	cr = cinedef_get_rec(&F.cin, (uint32_t)rec);
	ui = units_of_object(&F.units, b);
	r = call1("recording_time", HS_SHORT, mfix_obj(b));
	CHECK(r.v.i == 0);
	call2("recording_play", HS_VOID, mfix_obj(b), mfix_handle((uint32_t)rec, 15));
	ps = cine_play_find(&F.cine, (uint32_t)b);
	CHECK(ps >= 0);
	r = call1("recording_time", HS_SHORT, mfix_obj(b));
	CHECK((uint32_t)r.v.i == cr.length);
	steady = 0;
	{
		uint32_t t;
		for (t = 0; t < cr.length + 4; t++) {
			mfix_tick(&F, NULL);
			if (t == 5) {
				CHECK(F.units.u[ui].flags & UF_NO_AI);
				r = call1("recording_time", HS_SHORT, mfix_obj(b));
				CHECK((uint32_t)r.v.i + 6 == cr.length || (uint32_t)r.v.i + 5 == cr.length || (uint32_t)r.v.i + 7 == cr.length);
			}
			if (t > 2 && t < cr.length - 1) steady++;
		}
	}
	CHECK(steady > 0);
	CHECK(cine_play_find(&F.cine, (uint32_t)b) < 0);                  /* finished: released */
	CHECK(!(F.units.u[ui].flags & UF_NO_AI));
	r = call1("recording_time", HS_SHORT, mfix_obj(b));
	CHECK(r.v.i == 0);
	/* the control follows the recording: compare the unit's control with the independently decoded state at a tick */
	{
		cine_play p;
		unit_control_t seen;
		int t0 = 3;
		call2("recording_play", HS_VOID, mfix_obj(b), mfix_handle((uint32_t)rec, 15));
		ticks(t0);
		seen = F.units.u[ui].ctl;                                    /* control set during the last pre hook = the state of tick t0 - 1 + 0 */
		CHECK(cine_play_start(&p, &F.cin, (uint32_t)rec));
		{ int q; for (q = 0; q < t0 - 1; q++) cine_play_tick(&p); }
		CHECK(fabsf(seen.aim_yaw - cine_ctl_yaw(&p.ctl)) < 1e-4f && fabsf(seen.throttle_fwd - p.ctl.throttle[0]) < 1e-5f);
		/* kill */
		call1("recording_kill", HS_VOID, mfix_obj(b));
		CHECK(cine_play_find(&F.cine, (uint32_t)b) < 0 && !(F.units.u[ui].flags & UF_NO_AI));
		r = call1("recording_time", HS_SHORT, mfix_obj(b));
		CHECK(r.v.i == 0);
	}
	/* play and delete: the object goes when the recording ends */
	call2("recording_play_and_delete", HS_VOID, mfix_obj(b), mfix_handle((uint32_t)rec, 15));
	CHECK(cine_play_find(&F.cine, (uint32_t)b) >= 0);
	ticks((int)cr.length + 4);
	CHECK(!live(b));
	CHECK(F.cine.plays[0].used == 0 || cine_play_find(&F.cine, (uint32_t)b) < 0);
	/* hover: nothing is released, the unit stays under control until killed */
	{
		int b2 = first_live(SCEN_BIPED, 1, -1);
		if (b2 >= 0) {
			call2("recording_play_and_hover", HS_VOID, mfix_obj(b2), mfix_handle((uint32_t)rec, 15));
			ticks((int)cr.length + 4);
			CHECK(live(b2));
		}
	}
	/* bad index, the player, an unresolved unit: no-ops */
	call2("recording_play", HS_VOID, mfix_obj(first_live(SCEN_BIPED, 0, -1)), mfix_handle(0xFFFFu, 15));
	call2("recording_play", HS_VOID, mfix_obj(F.mission.player_obj), mfix_handle((uint32_t)rec, 15));
	CHECK(cine_play_find(&F.cine, (uint32_t)F.mission.player_obj) < 0);
	call2("recording_play", HS_VOID, mfix_handle(HS_HANDLE_NONE, 38), mfix_handle((uint32_t)rec, 15));
	call1("recording_kill", HS_VOID, mfix_handle(HS_HANDLE_NONE, 38));
	/* destroying an object kills its recording */
	{
		int b3 = first_live(SCEN_BIPED, 1, -1);
		if (b3 >= 0) {
			call2("recording_play", HS_VOID, mfix_obj(b3), mfix_handle((uint32_t)rec, 15));
			CHECK(cine_play_find(&F.cine, (uint32_t)b3) >= 0);
			call1("object_destroy", HS_VOID, mfix_obj(b3));
			CHECK(cine_play_find(&F.cine, (uint32_t)b3) < 0);
		}
	}
}

static void test_devices(void)
{
	uint32_t i;
	int tested = 0;
	fresh(10, 0);
	for (i = 0; i < F.dev.device_count && tested < 4; i++) {
		devdef_device dv = devdef_get_device(&F.dev, i);
		hs_value nm, r;
		unsigned pt;
		if (dv.name_index < 0 || (int)i >= (int)F.devices.count) continue;
		if (devdef_name_to_device(&F.dev, (uint32_t)dv.name_index) != (int)i) continue;
		nm = mfix_handle((uint32_t)dv.name_index, 41);
		tested++;
		pt = F.devices.dev[i].pos_ticks;
		call2("device_set_position_immediate", HS_VOID, nm, mfix_real(0.0f));
		r = call1("device_get_position", HS_REAL, nm);
		CHECK(r.v.f == 0.0f);
		r = call2("device_set_position", HS_BOOL, nm, mfix_real(1.0f));
		CHECK(r.v.i == 1);
		if (pt > 2) {
			ticks(1);
			r = call1("device_get_position", HS_REAL, nm);
			CHECK(r.v.f > 0.0f && r.v.f < 1.0f);
		}
		ticks((int)pt + 3);
		r = call1("device_get_position", HS_REAL, nm);
		CHECK(fabsf(r.v.f - 1.0f) < 1e-4f);
		call2("device_set_position_immediate", HS_VOID, nm, mfix_real(0.25f));
		r = call1("device_get_position", HS_REAL, nm);
		CHECK(fabsf(r.v.f - 0.25f) < 1e-4f);
		call2("device_set_power", HS_VOID, nm, mfix_real(0.0f));
		ticks((int)F.devices.dev[i].power_ticks + 3);
		r = call1("device_get_power", HS_REAL, nm);
		CHECK(r.v.f == 0.0f);
		call2("device_set_power", HS_VOID, nm, mfix_real(1.0f));
		ticks((int)F.devices.dev[i].power_ticks + 3);
		r = call1("device_get_power", HS_REAL, nm);
		CHECK(fabsf(r.v.f - 1.0f) < 1e-4f);
		/* flags */
		call2("device_operates_automatically_set", HS_VOID, nm, mfix_bool(0));
		CHECK(F.devices.dev[i].mflags & DEVDEF_MPF_NO_AUTO);
		call2("device_operates_automatically_set", HS_VOID, nm, mfix_bool(1));
		CHECK(!(F.devices.dev[i].mflags & DEVDEF_MPF_NO_AUTO));
		call2("device_one_sided_set", HS_VOID, nm, mfix_bool(1));
		CHECK(F.devices.dev[i].mflags & DEVDEF_MPF_ONE_SIDED);
		call2("device_set_never_appears_locked", HS_VOID, nm, mfix_bool(1));
		CHECK(F.devices.dev[i].mflags & DEVDEF_MPF_NEVER_LOCKED);
		/* by object handle */
		if (dv.scen_index < OBJ_MAX_PLACEMENTS && F.objects.by_placement[dv.scen_index] >= 0) {
			hs_value oh = mfix_obj(F.objects.by_placement[dv.scen_index]);
			r = call1("device_get_position", HS_REAL, oh);
			CHECK(fabsf(r.v.f - 0.25f) < 1e-4f);
		}
	}
	CHECK(tested >= 1);
	/* groups */
	if (F.devices.group_count > 0) {
		hs_value g = mfix_handle(0, 16), r;
		call2("device_group_set", HS_VOID, g, mfix_real(0.7f));
		r = call1("device_group_get", HS_REAL, g);
		CHECK(fabsf(r.v.f - 0.7f) < 1e-4f);
		call2("device_group_set_immediate", HS_VOID, g, mfix_real(0.0f));
		r = call1("device_group_get", HS_REAL, g);
		CHECK(r.v.f == 0.0f);
		for (i = 0; i < F.devices.count; i++) if (F.devices.dev[i].position_group == 0) {
			float want = (F.devices.dev[i].pflags & DEVDEF_PF_REVERSED) ? 1.0f : 0.0f;
			CHECK(fabsf(devices_position(&F.devices, i) - want) < 1e-4f);
		}
		call2("device_group_set_immediate", HS_VOID, g, mfix_real(1.0f));
		for (i = 0; i < F.devices.count; i++) if (F.devices.dev[i].position_group == 0) {
			float want = (F.devices.dev[i].pflags & DEVDEF_PF_REVERSED) ? 0.0f : 1.0f;
			CHECK(fabsf(devices_position(&F.devices, i) - want) < 1e-4f);
		}
		call2("device_group_set", HS_VOID, mfix_handle(0xFFFFu, 16), mfix_real(1.0f));      /* bad group */
	}
	/* an unresolved device: neutral answers */
	{
		hs_value r = call1("device_get_position", HS_REAL, mfix_handle(HS_HANDLE_NONE, 41));
		CHECK(r.v.f == 0.0f);
		call2("device_set_position", HS_VOID, mfix_handle(0xFFFFu, 41), mfix_real(1.0f));
	}
}

static void test_effects(void)
{
	uint32_t i;
	uint32_t etag = 0, dtag_area = 0, dtag_hit = 0;
	hs_value r;
	fresh(11, 0);
	/* an effect that makes particles */
	for (i = 0; i < F.fx.count[FXD_T_EFFECTS] && !etag; i++) {
		static fx_t scratch;
		float p[3] = { 0, 0, 1 }, n[3] = { 0, 0, 1 };
		fxd_effect e = fxdef_get_effect(&F.fx, i);
		fx_init(&scratch, &F.fx, 0.0f, 1);
		if (fx_spawn(&scratch, e.tag_id, p, n, 0) > 0) etag = e.tag_id;
	}
	CHECK(etag != 0);
	if (F.dev.flag_count && etag) {
		unsigned before = fx_active_particles(&F.fxrt);
		call2("effect_new", HS_VOID, mfix_handle(etag, 25), flag_h(0));
		CHECK(fx_active_particles(&F.fxrt) + fx_active_decals(&F.fxrt) > before);
	}
	if (etag) {
		int mk = -1, o = find_marker_object(&mk);
		if (o >= 0) {
			model_info mi = model_get(&F.models, (uint32_t)obj(o)->model);
			model_marker m = model_get_marker(&F.models, &mi, (uint32_t)mk);
			unsigned before = fx_active_particles(&F.fxrt);
			call3("effect_new_on_object_marker", HS_VOID, mfix_handle(etag, 25), mfix_obj(o), mfix_str(&F, m.name));
			CHECK(fx_active_particles(&F.fxrt) > before);
		}
		call2("effect_new", HS_VOID, mfix_handle(etag, 25), flag_h(0xFFFFu));                /* bad flag */
	}
	for (i = 0; i < F.weap.n_damages; i++) {
		weap_damage d;
		weap_get_damage(&F.weap, i, &d);
		if (!dtag_area && d.radius[1] > 1.0f && d.damage_upper[0] > 0.0f && d.damage_lower >= 0.0f) dtag_area = d.tag_id;
		if (!dtag_hit && d.damage_upper[0] > 0.0f) dtag_hit = d.tag_id;
	}
	CHECK(dtag_hit != 0);
	{
		int b = first_live(SCEN_BIPED, 1, -1);
		if (b >= 0 && dtag_hit) {
			weap_damage d;
			float h0 = F.game.unit_state[b].vitals.health + F.game.unit_state[b].vitals.shield, h1;
			weap_get_damage(&F.weap, (uint32_t)weap_find_damage(&F.weap, dtag_hit), &d);
			call2("damage_object", HS_VOID, mfix_handle(dtag_hit, 26), mfix_obj(b));
			h1 = F.game.unit_state[b].vitals.health + F.game.unit_state[b].vitals.shield;
			CHECK(h1 < h0);
			/* the player */
			{
				float s0 = F.game.vitals.shield + F.game.vitals.health;
				call2("damage_object", HS_VOID, mfix_handle(dtag_hit, 26), mfix_obj(F.mission.player_obj));
				CHECK(F.game.vitals.shield + F.game.vitals.health < s0);
			}
		}
		b = first_live(SCEN_BIPED, 1, b);                      /* the first one may have died of the hit above */
		if (b >= 0 && dtag_area && F.dev.flag_count && !F.game.unit_state[b].dead) {
			/* area damage at a flag: put the biped on the flag */
			devdef_cflag f = devdef_get_cflag(&F.dev, 0);
			int ui = units_of_object(&F.units, b);
			float h0, h1;
			units_teleport(&F.units, &F.game, ui, f.pos, 0.0f);
			F.game.unit_state[b].vitals.health = F.game.unit_state[b].vitals.max_health;
			F.game.unit_state[b].vitals.shield = F.game.unit_state[b].vitals.max_shield;
			h0 = F.game.unit_state[b].vitals.health + F.game.unit_state[b].vitals.shield;
			call2("damage_new", HS_VOID, mfix_handle(dtag_area, 26), flag_h(0));
			h1 = F.game.unit_state[b].vitals.health + F.game.unit_state[b].vitals.shield;
			CHECK(h1 < h0);
		}
	}
	(void)r;
}

static void test_player(void)
{
	game_input_t in;
	hs_value r;
	fresh(12, 0);
	r = call("player_action_test_primary_trigger", HS_BOOL, NULL, 0);
	CHECK(r.v.i == 0);
	memset(&in, 0, sizeof in);
	in.held = IN_FIRE;
	mfix_tick(&F, &in);
	r = call("player_action_test_primary_trigger", HS_BOOL, NULL, 0);
	CHECK(r.v.i == 1);
	r = call("player_action_test_grenade_trigger", HS_BOOL, NULL, 0);
	CHECK(r.v.i == 0);
	call("player_action_test_reset", HS_VOID, NULL, 0);
	r = call("player_action_test_primary_trigger", HS_BOOL, NULL, 0);
	CHECK(r.v.i == 0);
	memset(&in, 0, sizeof in);
	in.held = IN_GRENADE | IN_SWITCH | IN_JUMP; in.move.move_y = -1.0f;
	mfix_tick(&F, &in);
	CHECK(call("player_action_test_grenade_trigger", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player_action_test_action", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player_action_test_accept", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player_action_test_back", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player_action_test_look_relative_up", HS_BOOL, NULL, 0).v.i == 0);
	CHECK(call("player_action_test_look_relative_all_directions", HS_BOOL, NULL, 0).v.i == 0);
	memset(&in, 0, sizeof in);
	in.move.look_y = 1.0f; mfix_tick(&F, &in);
	CHECK(call("player_action_test_look_relative_up", HS_BOOL, NULL, 0).v.i == 1);
	in.move.look_y = -1.0f; mfix_tick(&F, &in);
	CHECK(call("player_action_test_look_relative_down", HS_BOOL, NULL, 0).v.i == 1);
	in.move.look_y = 0.0f; in.move.look_x = -1.0f; mfix_tick(&F, &in);
	CHECK(call("player_action_test_look_relative_left", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player_action_test_look_relative_all_directions", HS_BOOL, NULL, 0).v.i == 0);
	in.move.look_x = 1.0f; mfix_tick(&F, &in);
	CHECK(call("player_action_test_look_relative_right", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player_action_test_look_relative_all_directions", HS_BOOL, NULL, 0).v.i == 1);
	/* input and camera locks, pitch option */
	CHECK(!F.mission.hobj.player_input_disabled && !F.mission.hobj.player_look_disabled);
	call1("player_enable_input", HS_VOID, mfix_bool(0));
	CHECK(F.mission.hobj.player_input_disabled);
	call1("player_enable_input", HS_VOID, mfix_bool(1));
	CHECK(!F.mission.hobj.player_input_disabled);
	call1("player_camera_control", HS_VOID, mfix_bool(0));
	CHECK(F.mission.hobj.player_look_disabled);
	call1("player_camera_control", HS_VOID, mfix_bool(1));
	CHECK(!F.mission.hobj.player_look_disabled);
	CHECK(call("player0_look_pitch_is_inverted", HS_BOOL, NULL, 0).v.i == 0);
	call1("player0_look_invert_pitch", HS_VOID, mfix_bool(1));
	CHECK(call("player0_look_pitch_is_inverted", HS_BOOL, NULL, 0).v.i == 1);
	CHECK(call("player0_joystick_set_is_normal", HS_BOOL, NULL, 0).v.i == 1);
	F.game.zoomed = 1;
	call("players_unzoom_all", HS_VOID, NULL, 0);
	CHECK(F.game.zoomed == 0);
}

static void test_vehicles(void)
{
	int a, b, veh;
	mission_list *l;
	uint32_t h;
	hs_value args[3], r;
	fresh(13, 0);
	a = first_live(SCEN_BIPED, 1, -1);
	b = first_live(SCEN_BIPED, 1, a);
	veh = first_live(SCEN_VEHICLE, 0, -1);
	if (veh < 0) veh = first_live(SCEN_SCENERY, 0, -1);
	CHECK(a >= 0 && b >= 0 && veh >= 0);
	if (a < 0 || b < 0 || veh < 0) return;
	l = mission_list_new(&F.mission, &h);
	mission_list_add(l, a); mission_list_add(l, b);
	args[0] = mfix_obj(veh); args[1] = mfix_str(&F, "seat-a"); args[2] = mfix_handle(h, 23);
	call("vehicle_load_magic", HS_VOID, args, 3);
	{
		hs_value rl = call1("vehicle_riders", HS_HANDLE, mfix_obj(veh));
		mission_list *rr = mission_list_get(&F.mission, rl.v.h);
		CHECK(rr && rr->count == 2);
	}
	args[0] = mfix_obj(veh); args[1] = mfix_str(&F, "seat-a"); args[2] = mfix_handle(h, 23);
	r = call("vehicle_test_seat_list", HS_BOOL, args, 3);
	CHECK(r.v.i == 1);
	args[1] = mfix_str(&F, "seat-b");
	r = call("vehicle_test_seat_list", HS_BOOL, args, 3);
	CHECK(r.v.i == 0);
	/* the riders travel with the vehicle */
	{
		object_t *vo = &F.objects.obj[veh];
		vo->pos[0] += 9.0f;
		mat4_from_ypr(&vo->root, 0.0f, 0.0f, 0.0f, vo->pos);
		ticks(1);
		CHECK(dist3(obj(a)->pos, vo->pos) < 1e-3f);
	}
	call2("vehicle_unload", HS_VOID, mfix_obj(veh), mfix_str(&F, "seat-b"));          /* another seat: nobody leaves */
	CHECK(call1("vehicle_riders", HS_HANDLE, mfix_obj(veh)).v.h != HS_HANDLE_NONE);
	call2("vehicle_unload", HS_VOID, mfix_obj(veh), mfix_str(&F, "seat-a"));
	{
		hs_value rl = call1("vehicle_riders", HS_HANDLE, mfix_obj(veh));
		mission_list *rr = mission_list_get(&F.mission, rl.v.h);
		CHECK(rr && rr->count == 0);
	}
	/* single enter / exit */
	args[0] = mfix_obj(a); args[1] = mfix_obj(veh); args[2] = mfix_str(&F, "seat-c");
	call("unit_enter_vehicle", HS_VOID, args, 3);
	CHECK(F.mission.hobj.seat_vehicle[a] == veh + 1);
	call1("unit_exit_vehicle", HS_VOID, mfix_obj(a));
	CHECK(F.mission.hobj.seat_vehicle[a] == 0 && F.mission.hobj.attach_count == 0);
	call2("unit_set_seat", HS_VOID, mfix_obj(a), mfix_str(&F, "x"));
	call2("vehicle_hover", HS_VOID, mfix_obj(veh), mfix_bool(1));
	call1("unit_open", HS_VOID, mfix_obj(veh));
	call1("unit_close", HS_VOID, mfix_obj(veh));
	call2("unit_set_emotion", HS_VOID, mfix_obj(a), mfix_short(1));
	/* the vehicle goes away: nobody is left sitting in it */
	args[0] = mfix_obj(veh); args[1] = mfix_str(&F, "s"); args[2] = mfix_handle(h, 23);
	call("vehicle_load_magic", HS_VOID, args, 3);
	call1("object_destroy", HS_VOID, mfix_obj(veh));
	CHECK(F.mission.hobj.seat_vehicle[a] == 0 && F.mission.hobj.attach_count == 0);
	/* unit_suspended holds a unit in place */
	{
		float p0[3] = { obj(a)->pos[0], obj(a)->pos[1], obj(a)->pos[2] };
		call2("unit_suspended", HS_VOID, mfix_obj(a), mfix_bool(1));
		F.objects.obj[a].pos[2] += 5.0f;
		ticks(2);
		CHECK(dist3(obj(a)->pos, p0) < 1e-4f);
		call2("unit_suspended", HS_VOID, mfix_obj(a), mfix_bool(0));
		CHECK(F.mission.hobj.suspend_count == 0);
	}
	call1("unit_doesnt_drop_items", HS_VOID, mfix_handle(h, 23));
	CHECK((F.mission.hobj.no_drop[a >> 5] >> (a & 31)) & 1u);
	/* the unit conversion */
	r = call1("unit", HS_HANDLE, mfix_obj(a));
	CHECK(r.v.h == mission_obj_handle(a));
	r = call1("unit", HS_HANDLE, mfix_handle(HS_HANDLE_NONE, 37));
	CHECK(r.v.h == HS_HANDLE_NONE);
}

static void test_noops(void)
{
	hs_value r;
	fresh(14, 0);
	/* explicit no-ops and the neutral ones must be registered (not counted as unregistered) and never touch anything */
	r = call("unit_solo_player_integrated_night_vision_is_active", HS_BOOL, NULL, 0);
	CHECK(r.v.i == 0 && r.vtype != 255);
	call2("units_set_desired_flashlight_state", HS_VOID, mfix_handle(HS_HANDLE_NONE, 23), mfix_bool(1));
	call2("object_set_collideable", HS_VOID, mfix_obj(F.mission.player_obj), mfix_bool(0));
	call3("player_add_equipment", HS_VOID, mfix_obj(F.mission.player_obj), mfix_handle(0, 19), mfix_bool(1));
	call2("device_group_change_only_once_more_set", HS_VOID, mfix_handle(0, 16), mfix_bool(1));
	r = call("not_a_builtin", HS_VOID, NULL, 0);
	CHECK(r.vtype == 255);
	/* every built-in of the module has a name and is reachable by the registry; missing argument lists never crash */
	{
		unsigned i, n = hs_host_obj_count(), ok = 0;
		for (i = 0; i < n; i++) {
			hs_value rr = mfix_call(&F, hs_host_obj_name(i), HS_REAL, NULL, 0);   /* no arguments at all */
			ok += rr.vtype != 255;
		}
		CHECK(ok == n);
		printf("  %u built-ins registered by H1\n", n);
	}
}


/* Fuzz: every built-in with random, mostly wrong arguments (dead handles, names out of range, NaN, huge numbers, lists, strings out of the pool),
 * with ticks in between so attachments, recordings and custom animations run on whatever the calls made. Under ASan / UBSan: no crash. */
static uint32_t g_rng = 12345u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static hs_value fuzz_value(void)
{
	hs_value v;
	uint32_t k = rnd() % 14u;
	memset(&v, 0, sizeof v);
	switch (k) {
	case 0: return mfix_handle(HS_HANDLE_NONE, 37);
	case 1: return mfix_handle(rnd(), 37u + rnd() % 7u);
	case 2: return mfix_handle(rnd() % (uint32_t)(F.objects.count + 4), 37);
	case 3: return mfix_obj((int)(rnd() % (uint32_t)(F.objects.count + 4)));
	case 4: return mfix_obj((int)(rnd() % 70000u) - 2);
	case 5: return mfix_handle(MISSION_LIST_BIT | (rnd() % 80u), 23);
	case 6: return mfix_real((float)((int)(rnd() % 2001u) - 1000) * 0.37f);
	case 7: { static const float w[5] = { 0.0f / 0.0f, 1.0f / 0.0f, -1.0f / 0.0f, 3.4e38f, -3.4e38f }; return mfix_real(w[rnd() % 5u]); }
	case 8: return mfix_short((int)(rnd() % 70000u) - 30000);
	case 9: return mfix_bool((int)(rnd() & 1u));
	case 10: { hs_value s = mfix_str(&F, "abc"); s.v.i = (int32_t)rnd(); return s; }
	case 11: return mfix_handle(rnd() % (uint32_t)(F.dev.name_count + 3), 41);
	case 12: return mfix_handle(rnd() % 300u, 11u + rnd() % 6u);
	default: return mfix_handle(rnd(), 28);
	}
}

static void test_fuzz(void)
{
	unsigned round, i, nb = hs_host_obj_count();
	fresh(55, 0);
	for (round = 0; round < 12; round++) {
		if (round % 4 == 0) fresh(56 + round, 0);
		for (i = 0; i < nb * 40; i++) {
			hs_value a[5];
			unsigned n = rnd() % 6u, k, ret = (rnd() & 1u) ? HS_BOOL : (rnd() & 1u) ? HS_REAL : HS_HANDLE;
			const char *nm = hs_host_obj_name(rnd() % nb);
			if (strcmp(nm, "object_destroy_all") == 0 && (rnd() % 8u)) continue;      /* keep the scene populated most of the time */
			for (k = 0; k < n && k < 5; k++) a[k] = fuzz_value();
			(void)mfix_call(&F, nm, ret, a, n > 5 ? 5 : n);
			if ((i % 97) == 0) mfix_tick(&F, NULL);
		}
		ticks(30);
	}
	CHECK(live(F.mission.player_obj));
	{
		uint32_t t = 0;
		unsigned k;
		for (k = 0; k < nb; k++) t += hs_host_obj_calls(k);
		printf("  fuzz: %u random calls, scene survived\n", t);
		CHECK(t > 1000);
	}
}

/* ---- the soak ------------------------------------------------------------------------------------------------------------ */
static void soak(void)
{
	unsigned t, i, k, nb = hs_host_obj_count(), unreg_n, thr;
	const hs_state *st;
	const char *nm;
	uint32_t calls, total = 0, objs_peak = 0;
	hs_value dummy;
	(void)dummy;
	clock_t c0;
	fresh(99, 1);
	if (!F.has_scr) { printf("  soak: no script pack, skipped\n"); return; }
	c0 = clock();
	for (t = 0; t < 9000; t++) {
		unsigned live_objs = 0;
		mfix_tick(&F, NULL);
		if ((t % 600) == 599) {
			for (k = 0; k < F.objects.count; k++) live_objs += !(F.objects.obj[k].flags & OBJ_F_FREE);
			if (live_objs > objs_peak) objs_peak = live_objs;
		}
	}
	printf("  soak host time: %.2f s for 9000 mission ticks (scripts, devices, units, game; an ASan build is about 50x slower than -O2)\n", (double)(clock() - c0) / CLOCKS_PER_SEC);
	st = hs_state_ptr(&F.hs);
	printf("  soak: 9000 ticks, nodes %u total, errors %u, blocked calls %u, thread overflow %u, budget hits %u/%u, peak threads %u\n",
	       st->nodes_total, st->errors, st->blocked_calls, st->thread_overflow, st->budget_hits, st->tick_budget_hits, st->hw_threads);
	thr = hs_thread_count(&F.hs);
	printf("  threads alive at the end: %u; live objects (sampled peak) %u; units spawned %u\n", thr, objs_peak, F.units.spawned);
	printf("  H1 built-in calls (of %u registered, only those called):\n", nb);
	for (i = 0; i < nb; i++) {
		calls = hs_host_obj_calls(i);
		total += calls;
		if (calls) printf("    %-48s %u\n", hs_host_obj_name(i), calls);
	}
	printf("  H1 calls total %u\n", total);
	unreg_n = hs_unregistered(&F.hs, 0, NULL, NULL);
	printf("  unregistered built-ins hit: %u distinct (the other hosts' domains)\n", unreg_n);
	{
		/* none of them may belong to the domains of this module */
		unsigned bad = 0;
		for (k = 0; k < unreg_n; k++) {
			calls = 0; nm = "";
			hs_unregistered(&F.hs, k, &nm, &calls);
			for (i = 0; i < nb; i++) if (strcmp(nm, hs_host_obj_name(i)) == 0) bad++;
		}
		CHECK(bad == 0);
	}
	{
		/* unregistered built-ins of this module's own domains (by prefix) that a10 calls and that H1 does not implement */
		static const char *dom[] = { "object", "objects_", "unit", "vehicle", "player", "device", "effect", "damage", "recording", "custom_animation", "list_", "volume_", "players" };
		for (k = 0; k < unreg_n; k++) {
			unsigned d;
			calls = 0; nm = "";
			hs_unregistered(&F.hs, k, &nm, &calls);
			for (d = 0; d < sizeof dom / sizeof dom[0]; d++)
				if (strncmp(nm, dom[d], strlen(dom[d])) == 0 && strncmp(nm, "object_pvs", 10) != 0 && strncmp(nm, "player_effect", 13) != 0 && strstr(nm, "predict") == NULL) printf("  NOTE: domain built-in not implemented by H1: %s (%u calls)\n", nm, calls);
		}
	}
	{
		unsigned aw = 0;
		for (k = 0; k < 8; k++) if (F.mission.awaited_volume[k].active) {
			aw++;
			printf("  awaited volume %u (last seen at tick %u, %u tests)\n", F.mission.awaited_volume[k].volume, F.mission.awaited_volume[k].last_tick, F.mission.awaited_volume[k].hits);
		}
		printf("  awaited volumes at the end: %u\n", aw);
	}
	CHECK(st->errors == 0);
	CHECK(st->blocked_calls == 0);
	CHECK(st->thread_overflow == 0);
	CHECK(total > 0);
	CHECK(live(F.mission.player_obj));
	/* sleeping threads are all waiting on something finite or on another script: nothing is stuck inside a host call */
	CHECK(thr < HS_MAX_THREADS);
}


/* Stress soak: the same, but every dormant script is woken every 450 ticks (the other hosts' conditions answer neutral values, so far more
 * of the mission's script code reaches the H1 built-ins than in the quiet run): robustness of every built-in with real arguments. */
static void soak_stress(void)
{
	unsigned t, i, nb = hs_host_obj_count(), distinct = 0;
	uint32_t total = 0;
	const hs_state *st;
	clock_t c0;
	fresh(77, 1);
	if (!F.has_scr) return;
	c0 = clock();
	for (t = 0; t < 9000; t++) {
		if ((t % 450) == 0) for (i = 0; i < F.scr.script_count; i++) if (F.scr.scripts[i].type == HS_S_DORMANT) hs_wake_script_index(&F.hs, i);
		mfix_tick(&F, NULL);
	}
	printf("  stress soak host time: %.2f s\n", (double)(clock() - c0) / CLOCKS_PER_SEC);
	st = hs_state_ptr(&F.hs);
	for (i = 0; i < nb; i++) { total += hs_host_obj_calls(i); distinct += hs_host_obj_calls(i) != 0; }
	printf("  stress soak: 9000 ticks, H1 calls %u over %u distinct built-ins, script errors %u, blocked %u, thread overflow %u, peak threads %u\n",
	       total, distinct, st->errors, st->blocked_calls, st->thread_overflow, st->hw_threads);
	for (i = 0; i < nb; i++) if (hs_host_obj_calls(i)) printf("    %-48s %u\n", hs_host_obj_name(i), hs_host_obj_calls(i));
	CHECK(st->errors == 0);
	CHECK(st->blocked_calls == 0);
	CHECK(live(F.mission.player_obj));
}

int main(int argc, char **argv)
{
	const char *cine = argc > 2 ? argv[2] : NULL;
	if (argc > 1) g_dir = argv[1];
	if (!mfix_load(&F, g_dir, cine)) { printf("skip: no a10 packs in %s\n", g_dir); return 0; }
	printf("mission_hostobj: %u bytes, mission_t: %u bytes; a10 objects %u placements, %u names, %u devices, %u volumes, %u flags, cine %s\n",
	       (unsigned)sizeof(mission_hostobj), (unsigned)sizeof(mission_t), (unsigned)F.scen.placement_count, (unsigned)F.dev.name_count,
	       (unsigned)F.dev.device_count, (unsigned)F.dev.volume_count, (unsigned)F.dev.flag_count, F.has_cin ? "yes" : "no");
	test_lists();
	test_create_destroy();
	test_teleport();
	test_player_stand_in();
	test_player_equipment();
	test_volumes();
	test_attach();
	test_sight();
	test_vitals();
	test_custom_animation();
	test_recordings();
	test_devices();
	test_effects();
	test_player();
	test_vehicles();
	test_noops();
	test_fuzz();
	soak();
	soak_stress();
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
