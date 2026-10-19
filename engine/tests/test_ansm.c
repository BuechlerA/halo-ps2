/* Host test for world/ansm.c.
 *
 *   test_ansm <pack.hp2>                    pack written by tools/tests/test_ansmconv.py (ANSM_FIXTURE): sections ASTB and
 *                                           AEXP (digest of the Python reader's walk, graph and animation counts)
 *   test_ansm <pack.hp2> <digest-hex>       any pack with an ASTB section (e.g. a real map's assets/hp2/<map>.ansm.hp2) and
 *                                           the digest `ansmconv.py <pack> --digest` prints
 *
 * 1. the walk below visits everything ansm.h exposes in the order of ansmconv.digest(); equal numbers mean the C and the
 *    Python readers see the same data in every table;
 * 2. invariants that must hold for any valid pack (names find themselves, slots in range, variant chains terminate,
 *    lookups agree with direct slot access, events sorted);
 * 3. for the synthetic fixture: hand-written expectations of the world test_ansmconv.py builds (lookups, variants,
 *    events, damage, aim ranges, first person, vehicle, device, slot names);
 * 4. every byte of the section is changed four ways and the section truncated at every length: whatever ansm_open accepts
 *    is walked completely (ASan / UBSan watch every access). */
#include "stream/hp2.h"
#include "world/ansm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* ---- the digest walk (mirror of ansmconv.digest) -------------------------------------------------------------------- */
static uint32_t H;
static void fb(uint8_t c) { H = (H ^ c) * 16777619u; }
static void fu(uint32_t v) { fb((uint8_t)v); fb((uint8_t)(v >> 8)); fb((uint8_t)(v >> 16)); fb((uint8_t)(v >> 24)); }
static void fi(int v) { fu((uint32_t)(int32_t)v); }
static void ff(float v) { uint32_t u; memcpy(&u, &v, 4); fu(u); }
static void fs(const char *s) { while (*s) fb((uint8_t)*s++); fb(0); }

static void faim(const ansm_aim *a)
{
	ff(a->yaw_right_per_frame); ff(a->yaw_left_per_frame); fu(a->yaw_right_frames); fu(a->yaw_left_frames);
	ff(a->pitch_down_per_frame); ff(a->pitch_up_per_frame); fu(a->pitch_down_frames); fu(a->pitch_up_frames);
}

#define DIGEST_SLOTS 64u

static uint32_t walk(const ansm_section *s)
{
	uint32_t gi;
	H = 2166136261u;
	fu(s->graph_count);
	for (gi = 0; gi < s->graph_count; gi++) {
		ansm_graph g;
		uint32_t k, u, c, t, i;
		unsigned slot, cat, dir, reg;
		if (ansm_get_graph(s, gi, &g)) abort();
		fu(g.tag_id);
		fu(g.anim_count);
		for (k = 0; k < g.anim_count; k++) {
			ansm_anim a;
			if (ansm_anim_info(s, &g, k, &a)) abort();
			fs(a.name);
			fu(a.frame_count); fu(a.ticks); fu(a.flags); fu(a.type); fu(a.fps); fi(a.next); fi(a.main); fu(a.loop_frame);
			fi(a.key_frame[0]); fi(a.key_frame[1]); fi(a.sound); fi(a.sound_frame); fu(a.sound_tag);
			fu(a.foot_left); fu(a.foot_right); fu((uint32_t)(a.weight * 65535.0f + 0.5f));
			fi(ansm_find_anim(s, &g, a.name));
		}
		fu(g.sound_count);
		for (i = 0; i < g.sound_count; i++) fu(ansm_sound_tag(s, &g, i));
		fu(g.object_count);
		for (i = 0; i < g.object_count; i++) {
			ansm_object_overlay o;
			if (ansm_object_overlay_get(s, &g, i, &o)) abort();
			fi(o.anim); fu(o.function); fu(o.controls);
		}
		fu(g.unit_count);
		for (u = 0; u < g.unit_count; u++) {
			ansm_unit un;
			ansm_ik ik;
			if (ansm_unit_info(s, &g, u, &un)) abort();
			fs(un.name);
			faim(&un.aim);
			fu(un.slot_count);
			if (un.slot_count > DIGEST_SLOTS) abort();
			for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_unit_slot(s, &g, u, slot));
			fu(un.ik_count);
			for (i = 0; i < un.ik_count; i++) { if (ansm_unit_ik(s, &g, u, i, &ik)) abort(); fs(ik.marker); fs(ik.attach); }
			fu(un.class_count);
			for (c = 0; c < un.class_count; c++) {
				ansm_class cl;
				if (ansm_class_info(s, &g, u, c, &cl)) abort();
				fs(cl.name); fs(cl.grip); fs(cl.hold);
				faim(&cl.aim);
				fu(cl.slot_count);
				if (cl.slot_count > DIGEST_SLOTS) abort();
				for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_class_slot(s, &g, u, c, slot));
				fu(cl.ik_count);
				for (i = 0; i < cl.ik_count; i++) { if (ansm_class_ik(s, &g, u, c, i, &ik)) abort(); fs(ik.marker); fs(ik.attach); }
				fu(cl.type_count);
				for (t = 0; t < cl.type_count; t++) {
					ansm_type ty;
					if (ansm_type_info(s, &g, u, c, t, &ty)) abort();
					fs(ty.label);
					fu(ty.slot_count);
					if (ty.slot_count > DIGEST_SLOTS) abort();
					for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_type_slot(s, &g, u, c, t, slot));
				}
			}
		}
		fu(g.vehicle_count);
		for (u = 0; u < g.vehicle_count; u++) {
			ansm_vehicle v;
			ansm_suspension su;
			if (ansm_vehicle_info(s, &g, u, &v)) abort();
			ff(v.steer_right_per_frame); ff(v.steer_left_per_frame); fu(v.steer_right_frames); fu(v.steer_left_frames);
			fu(v.slot_count);
			if (v.slot_count > DIGEST_SLOTS) abort();
			for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_vehicle_slot(s, &g, u, slot));
			fu(v.suspension_count);
			for (i = 0; i < v.suspension_count; i++) {
				if (ansm_vehicle_suspension(s, &g, u, i, &su)) abort();
				fu(su.mass_point); fi(su.anim); ff(su.full_extension); ff(su.full_compression);
			}
		}
		fu(g.device_count);
		for (u = 0; u < g.device_count; u++) for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_device_slot(s, &g, u, slot));
		fu(g.wobj_count);
		for (u = 0; u < g.wobj_count; u++) for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_wobj_slot(s, &g, u, slot));
		fu(g.fp_count);
		for (u = 0; u < g.fp_count; u++) for (slot = 0; slot < DIGEST_SLOTS; slot++) fi(ansm_fp_slot(s, &g, u, slot));
		fu(g.damage_count);
		for (cat = 0; cat < 4; cat++)
			for (dir = 0; dir < 4; dir++)
				for (reg = 0; reg < ansm_damage_regions(&g); reg++) fi(ansm_damage_anim(s, &g, cat, dir, reg));
	}
	return H;
}

/* ---- invariants of any valid pack ------------------------------------------------------------------------------------ */
static int check_invariants(const ansm_section *s)
{
	uint32_t gi, total = 0;
	unsigned level, slot;
	for (level = 0; level < ANSM_LEVELS; level++) {
		CHECK(ansm_slot_count(level) > 0 && ansm_slot_name(level, ansm_slot_count(level)) == NULL);
		for (slot = 0; slot < ansm_slot_count(level); slot++) {
			const char *n = ansm_slot_name(level, slot);
			CHECK(n);
			CHECK(n[0] ? ansm_slot_find(level, n) == (int)slot : 1);
		}
	}
	CHECK(ansm_slot_name(ANSM_LEVELS, 0) == NULL && ansm_slot_find(ANSM_L_CLASS, "no such slot") == -1 && ansm_slot_find(ANSM_L_CLASS, NULL) == -1);
	for (gi = 0; gi < s->graph_count; gi++) {
		ansm_graph g, again;
		uint32_t k, u, c, t;
		CHECK(ansm_get_graph(s, gi, &g) == 0 && ansm_find_graph(s, g.tag_id, &again) == 0 && again.tag_id == g.tag_id &&
		      again.o_anims == g.o_anims);
		total += g.anim_count;
		for (k = 0; k < g.anim_count; k++) {
			ansm_anim a, b;
			ansm_event ev[ANSM_EVENTS_MAX];
			unsigned n, e;
			int f;
			char base[ANSM_NAME_MAX];
			CHECK(ansm_anim_info(s, &g, k, &a) == 0 && a.frame_count >= 1 && strlen(a.name) < ANSM_NAME_MAX);
			f = ansm_find_anim(s, &g, a.name);
			CHECK(f >= 0 && (uint32_t)f <= k);
			CHECK(ansm_anim_info(s, &g, (uint32_t)f, &b) == 0 && strcmp(a.name, b.name) == 0);
			CHECK(a.main >= 0 && (uint32_t)a.main < g.anim_count && a.next < (int)g.anim_count);
			CHECK(ansm_pick_variant(s, &g, k, 0.0f) >= 0 && ansm_pick_variant(s, &g, k, 0.9999f) >= 0);
			CHECK(ansm_pick_variant(s, &g, k, -1.0f) == a.main);
			{
				int last = ansm_pick_variant(s, &g, k, 1.0f);
				ansm_anim l;
				CHECK(ansm_anim_info(s, &g, (uint32_t)last, &l) == 0);
				CHECK(l.next < 0 || l.weight > 0.0f);
			}
			if (strchr(a.name, '%')) {                      /* a name without its variant suffix finds a chain head */
				size_t len = (size_t)(strchr(a.name, '%') - a.name);
				memcpy(base, a.name, len);
				base[len] = 0;
				f = ansm_find_anim(s, &g, base);
				CHECK(f >= 0 && ansm_anim_info(s, &g, (uint32_t)f, &b) == 0);
				if (strcmp(b.name, base) != 0) CHECK(b.main == (int16_t)f || strncmp(b.name, base, len) == 0);
			}
			n = ansm_anim_events(s, &g, k, ev);
			CHECK(n <= ANSM_EVENTS_MAX);
			for (e = 1; e < n; e++) CHECK(ev[e - 1].frame < ev[e].frame || (ev[e - 1].frame == ev[e].frame && ev[e - 1].kind <= ev[e].kind));
			for (e = 0; e < n; e++) CHECK(ev[e].kind >= ANSM_EV_SOUND && ev[e].kind <= ANSM_EV_FOOT_RIGHT);
			CHECK(a.sound < 0 ? a.sound_tag == 0 : a.sound_tag == ansm_sound_tag(s, &g, (uint32_t)a.sound));
		}
		CHECK(ansm_anim_info(s, &g, g.anim_count, &(ansm_anim){ 0 }) == -1 && ansm_find_anim(s, &g, NULL) == -1);
		for (u = 0; u < g.unit_count; u++) {
			ansm_unit un;
			CHECK(ansm_unit_info(s, &g, u, &un) == 0);
			CHECK(ansm_find_unit(s, &g, un.name) >= 0 && (uint32_t)ansm_find_unit(s, &g, un.name) <= u);
			for (slot = 0; slot < un.slot_count; slot++) {
				int a = ansm_unit_slot(s, &g, u, slot);
				CHECK(a >= -1 && a < (int)g.anim_count);
				if (ansm_find_unit(s, &g, un.name) == (int)u && ansm_slot_name(ANSM_L_UNIT, slot)[0])
					CHECK(ansm_lookup(s, &g, un.name, NULL, NULL, ANSM_ACTION(ANSM_L_UNIT, slot)) == a);
			}
			for (c = 0; c < un.class_count; c++) {
				ansm_class cl;
				CHECK(ansm_class_info(s, &g, u, c, &cl) == 0);
				for (slot = 0; slot < cl.slot_count; slot++) {
					int a = ansm_class_slot(s, &g, u, c, slot);
					CHECK(a >= -1 && a < (int)g.anim_count);
					if (ansm_find_unit(s, &g, un.name) == (int)u && ansm_find_class(s, &g, u, cl.name) == (int)c)
						CHECK(ansm_lookup(s, &g, un.name, cl.name, NULL, ANSM_ACTION(ANSM_L_CLASS, slot)) == a);
				}
				for (t = 0; t < cl.type_count; t++) {
					ansm_type ty;
					int best;
					CHECK(ansm_type_info(s, &g, u, c, t, &ty) == 0);
					best = ansm_find_class_for_type(s, &g, u, ty.label);
					if (ty.label[0] && best >= 0) CHECK((uint32_t)best < un.class_count);
					for (slot = 0; slot < ty.slot_count; slot++) {
						int a = ansm_type_slot(s, &g, u, c, t, slot);
						CHECK(a >= -1 && a < (int)g.anim_count);
						if (ansm_find_unit(s, &g, un.name) == (int)u && ansm_find_class(s, &g, u, cl.name) == (int)c &&
						    ansm_find_type(s, &g, u, c, ty.label) == (int)t)
							CHECK(ansm_lookup(s, &g, un.name, cl.name, ty.label, ANSM_ACTION(ANSM_L_TYPE, slot)) == a);
					}
				}
			}
		}
		CHECK(ansm_unit_info(s, &g, g.unit_count, &(ansm_unit){ 0 }) == -1 && ansm_unit_slot(s, &g, g.unit_count, 0) == -1);
		CHECK(ansm_class_slot(s, &g, 0, 1000, 0) == -1 && ansm_type_slot(s, &g, 0, 0, 1000, 0) == -1);
		CHECK(ansm_fp_slot(s, &g, g.fp_count, 0) == -1 && ansm_wobj_slot(s, &g, g.wobj_count, 0) == -1);
		CHECK(ansm_device_slot(s, &g, g.device_count, 0) == -1 && ansm_vehicle_slot(s, &g, g.vehicle_count, 0) == -1);
		CHECK(ansm_damage_anim(s, &g, 4, 0, 0) == -1 && ansm_damage_anim(s, &g, 0, 4, 0) == -1 &&
		      ansm_damage_anim(s, &g, 0, 0, ansm_damage_regions(&g)) == -1);
		CHECK(ansm_lookup(s, &g, "stand", "rifle", "ar", 0x7F00u) == -1);
	}
	CHECK(total == s->anim_total);
	{
		ansm_graph g;
		CHECK(ansm_find_graph(s, 0x12345u, &g) == -1 && ansm_get_graph(s, s->graph_count, &g) == -1);
	}
	return 0;
}

/* ---- the synthetic world of test_ansmconv.py ----------------------------------------------------------------------------- */
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static int check_world(const ansm_section *s)
{
	enum { BIPED = 0xE1760002u, FP = 0xE1780004u, VEH = 0xE17A0006u, DEV = 0xE17C0008u, WOBJ = 0xE17E000Au, SCEN = 0xE17F000Bu,
	       CINE = 0xE181000Du };
	ansm_graph g, fp, veh, dev, wo;
	ansm_anim a;
	ansm_event ev[ANSM_EVENTS_MAX];
	ansm_unit un;
	ansm_class cl;
	ansm_type ty;
	ansm_ik ik;
	ansm_aim aim;
	float rng[4];
	CHECK(s->graph_count == 7);
	CHECK(ansm_find_graph(s, BIPED, &g) == 0 && ansm_find_graph(s, FP, &fp) == 0 && ansm_find_graph(s, VEH, &veh) == 0 &&
	      ansm_find_graph(s, DEV, &dev) == 0 && ansm_find_graph(s, WOBJ, &wo) == 0);
	CHECK(ansm_find_graph(s, SCEN, &(ansm_graph){ 0 }) == 0 && ansm_find_graph(s, CINE, &(ansm_graph){ 0 }) == 0);
	CHECK(g.anim_count == 19 && g.unit_count == 2 && g.sound_count == 2 && g.damage_count == 176 && g.object_count == 1);
	/* animation info */
	CHECK(ansm_anim_info(s, &g, 0, &a) == 0 && strcmp(a.name, "stand rifle idle") == 0 && a.frame_count == 40 && a.ticks == 40 &&
	      a.fps == 30 && (a.flags & ANSM_A_LOOPS) && a.type == ANSM_TYPE_BASE);
	CHECK(ansm_anim_info(s, &g, 5, &a) == 0 && a.type == ANSM_TYPE_OVERLAY && !(a.flags & ANSM_A_LOOPS) && a.frame_count == 9);
	CHECK(ansm_anim_info(s, &g, 1, &a) == 0 && a.sound == 0 && a.sound_tag == 0xE0010001u && a.foot_left == 3 && a.foot_right == 15);
	CHECK(ansm_anim_events(s, &g, 1, ev) == 3 && ev[0].kind == ANSM_EV_SOUND && ev[0].frame == 0 && ev[0].arg == 0xE0010001u &&
	      ev[1].kind == ANSM_EV_FOOT_LEFT && ev[1].frame == 3 && ev[2].kind == ANSM_EV_FOOT_RIGHT && ev[2].frame == 15);
	CHECK(ansm_anim_events(s, &g, 4, ev) == 2 && ev[0].kind == ANSM_EV_KEY && ev[0].frame == 6 && ev[0].arg == 0 &&
	      ev[1].kind == ANSM_EV_KEY && ev[1].frame == 11 && ev[1].arg == 1);
	CHECK(ansm_anim_events(s, &g, 3, ev) == 1 && ev[0].kind == ANSM_EV_SOUND && ev[0].frame == 4 && ev[0].arg == 0xE0020002u);
	CHECK(ansm_anim_events(s, &g, 0, ev) == 0 && ansm_anim_events(s, &g, 99, ev) == 0);
	CHECK(ansm_sound_tag(s, &g, 1) == 0xE0020002u && ansm_sound_tag(s, &g, 2) == 0);
	/* names and variants */
	CHECK(ansm_find_anim(s, &g, "stand rifle ar reload-1") == 3 && ansm_find_anim(s, &g, "alert unarmed idle") == 7);
	CHECK(ansm_find_anim(s, &g, "alert unarmed idle%2") == 8 && ansm_find_anim(s, &g, "stand rifle") == -1 &&
	      ansm_find_anim(s, &g, "zzz") == -1 && ansm_find_anim(s, &g, "pilot_fidget01") == 14);
	CHECK(ansm_pick_variant(s, &g, 8, 0.0f) == 7 && ansm_pick_variant(s, &g, 8, 0.32f) == 7 && ansm_pick_variant(s, &g, 9, 0.34f) == 8 &&
	      ansm_pick_variant(s, &g, 7, 0.66f) == 8 && ansm_pick_variant(s, &g, 7, 0.68f) == 9 && ansm_pick_variant(s, &g, 7, 0.99f) == 9);
	CHECK(ansm_pick_variant(s, &g, 0, 0.5f) == 0 && ansm_pick_variant(s, &g, 99, 0.5f) == -1);
	CHECK(ansm_anim_info(s, &g, 8, &a) == 0 && a.main == 7 && a.next == 9 && near(a.weight, 0.67f, 0.001f));
	/* units, classes, types */
	CHECK(ansm_find_unit(s, &g, "stand") == 0 && ansm_find_unit(s, &g, "crouch") == 1 && ansm_find_unit(s, &g, "nope") == -1);
	CHECK(ansm_unit_info(s, &g, 0, &un) == 0 && strcmp(un.name, "stand") == 0 && un.class_count == 2 && un.ik_count == 1 && un.slot_count == 1);
	CHECK(near(un.aim.yaw_right_per_frame, 1.5f, 1e-6f) && un.aim.yaw_left_frames == 1 && un.aim.pitch_up_frames == 1);
	CHECK(ansm_unit_ik(s, &g, 0, 0, &ik) == 0 && strcmp(ik.marker, "right hand") == 0 && strcmp(ik.attach, "grip") == 0 &&
	      ansm_unit_ik(s, &g, 0, 1, &ik) == -1);
	CHECK(ansm_find_class(s, &g, 0, "rifle") == 0 && ansm_find_class(s, &g, 0, "unarmed") == 1 && ansm_find_class(s, &g, 0, "pistol") == -1);
	CHECK(ansm_class_info(s, &g, 0, 0, &cl) == 0 && strcmp(cl.name, "rifle") == 0 && cl.grip[0] == 0 && strcmp(cl.hold, "right hand") == 0 &&
	      cl.type_count == 3 && cl.ik_count == 1);
	CHECK(ansm_class_ik(s, &g, 0, 0, 0, &ik) == 0 && strcmp(ik.attach, "gun left hand") == 0);
	aim = cl.aim;
	ansm_aim_ranges(&aim, rng);
	CHECK(near(rng[0], 1.2f, 1e-5f) && near(rng[1], 1.2f, 1e-5f) && near(rng[2], 0.5f, 1e-5f) && near(rng[3], 0.5f, 1e-5f));
	CHECK(ansm_find_type(s, &g, 0, 0, "ar") == 0 && ansm_find_type(s, &g, 0, 0, "sg") == 1 && ansm_find_type(s, &g, 0, 0, "xx") == -1 &&
	      ansm_find_type(s, &g, 0, 1, "") == 0);
	CHECK(ansm_type_info(s, &g, 0, 0, 0, &ty) == 0 && strcmp(ty.label, "ar") == 0 && ty.slot_count == 9);
	CHECK(ansm_lookup(s, &g, "stand", "rifle", "ar", ANSM_T_RELOAD_1) == 3 && ansm_lookup(s, &g, "stand", "rifle", "ar", ANSM_T_FIRE_1) == 2 &&
	      ansm_lookup(s, &g, "stand", "rifle", "ar", ANSM_T_MELEE) == 4 && ansm_lookup(s, &g, "stand", "rifle", "sg", ANSM_T_CHAMBER_1) == 17);
	CHECK(ansm_lookup(s, &g, "stand", "rifle", "ar", ANSM_T_RELOAD_2) == -1 && ansm_lookup(s, &g, "stand", "rifle", "zz", ANSM_T_FIRE_1) == -1 &&
	      ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_T_FIRE_1) == -1);
	CHECK(ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_IDLE) == 0 && ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_MOVE_FRONT) == 1 &&
	      ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_AIM_STILL) == 5 && ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_THROW_GRENADE) == 6 &&
	      ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_READY) == 16 && ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_LAND_HARD) == 18 &&
	      ansm_lookup(s, &g, "stand", "rifle", NULL, ANSM_C_LAND_SOFT) == 18);
	CHECK(ansm_lookup(s, &g, "stand", "unarmed", NULL, ANSM_C_IDLE) == 7 && ansm_lookup(s, &g, "crouch", "rifle", NULL, ANSM_C_IDLE) == 13 &&
	      ansm_lookup(s, &g, "crouch", "rifle", "ar", ANSM_T_FIRE_1) == 2);
	CHECK(ansm_lookup(s, &g, "stand", NULL, NULL, ANSM_U_AIRBORNE_DEAD) == 10 && ansm_lookup(s, &g, "stand", NULL, NULL, ANSM_U_LOOK) == -1 &&
	      ansm_lookup(s, &g, "nope", NULL, NULL, ANSM_U_AIRBORNE_DEAD) == -1 && ansm_lookup(s, &g, NULL, NULL, NULL, ANSM_U_ENTER) == -1);
	CHECK(ansm_find_class_for_type(s, &g, 0, "ar") == 0 && ansm_find_class_for_type(s, &g, 0, "pb") == -1 &&
	      ansm_find_class_for_type(s, &g, 0, "") == -1 && ansm_find_class_for_type(s, &g, 0, NULL) == -1);
	/* slot names and action codes */
	CHECK(ANSM_C_MOVE_FRONT == ANSM_ACTION(ANSM_L_CLASS, 8) && strcmp(ansm_slot_name(ANSM_L_CLASS, 8), "move-front") == 0);
	CHECK(ansm_slot_find(ANSM_L_TYPE, "fire-1") == 4 && ansm_slot_find(ANSM_L_FP, "reload-full") == 8 && ansm_slot_count(ANSM_L_CLASS) == 51 &&
	      ansm_slot_count(ANSM_L_TYPE) == 9 && ansm_slot_count(ANSM_L_UNIT) == 30 && ansm_slot_count(ANSM_L_FP) == 28 &&
	      ansm_slot_count(ANSM_L_WOBJ) == 10 && ansm_slot_count(ANSM_L_VEHICLE) == 8 && ansm_slot_count(ANSM_L_DEVICE) == 2);
	CHECK(ANSM_ACTION_LEVEL(ANSM_FP_RELOAD_FULL) == ANSM_L_FP && ANSM_ACTION_SLOT(ANSM_FP_RELOAD_FULL) == 8);
	/* damage */
	CHECK(ansm_damage_regions(&g) == 11 && ansm_damage_anim(s, &g, 0, 0, 5) == 11 && ansm_damage_anim(s, &g, 0, 1, 1) == 12 &&
	      ansm_damage_anim(s, &g, 3, 0, 10) == 11 && ansm_damage_anim(s, &g, 1, 0, 0) == -1);
	{
		ansm_object_overlay o;
		CHECK(ansm_object_overlay_get(s, &g, 0, &o) == 0 && o.anim == 14 && o.function == 1 && o.controls == 0 &&
		      ansm_object_overlay_get(s, &g, 1, &o) == -1);
	}
	/* first person, weapon object, vehicle, device */
	CHECK(fp.fp_count == 1 && ansm_fp_slot(s, &fp, 0, 8) == 3 && ansm_fp_slot(s, &fp, 0, 2) == 1 && ansm_fp_slot(s, &fp, 0, 27) == -1 &&
	      ansm_lookup(s, &fp, NULL, NULL, NULL, ANSM_FP_RELOAD_FULL) == 3 && ansm_lookup(s, &fp, NULL, NULL, NULL, ANSM_FP_READY) == 2 &&
	      ansm_lookup(s, &fp, NULL, NULL, NULL, ANSM_FP_IDLE) == 0);
	CHECK(ansm_lookup(s, &wo, NULL, NULL, NULL, ANSM_WO_FIRE_1) == 0 && ansm_lookup(s, &wo, NULL, NULL, NULL, ANSM_WO_RELOAD_2) == 1);
	{
		ansm_vehicle v;
		ansm_suspension su;
		CHECK(ansm_vehicle_info(s, &veh, 0, &v) == 0 && near(v.steer_right_per_frame, 0.6f, 1e-6f) && v.steer_right_frames == 1 &&
		      v.slot_count == 6 && v.suspension_count == 2);
		CHECK(ansm_vehicle_slot(s, &veh, 0, 0) == 0 && ansm_vehicle_slot(s, &veh, 0, 5) == 1 && ansm_vehicle_slot(s, &veh, 0, 6) == -1 &&
		      ansm_lookup(s, &veh, NULL, NULL, NULL, ANSM_V_GROUND_SPEED) == 1);
		CHECK(ansm_vehicle_suspension(s, &veh, 0, 1, &su) == 0 && su.mass_point == 12 && su.anim == 2 && near(su.full_extension, -0.3f, 1e-6f) &&
		      near(su.full_compression, -0.12f, 1e-6f) && ansm_vehicle_suspension(s, &veh, 0, 2, &su) == -1);
	}
	CHECK(ansm_device_slot(s, &dev, 0, 0) == 0 && ansm_device_slot(s, &dev, 0, 1) == -1 && ansm_lookup(s, &dev, NULL, NULL, NULL, ANSM_D_POSITION) == 0);
	return 0;
}

/* ---- corruption ------------------------------------------------------------------------------------------------------ */
static int check_corruption(const uint8_t *good, size_t n)
{
	uint8_t *buf = malloc(n);
	size_t step = n > 20000 ? n / 400 : 1, i, len;      /* the 10 KB synthetic section: every byte; real maps: sampled */
	uint32_t accepted = 0, tried = 0;
	ansm_section s;
	unsigned d;
	CHECK(buf);
	for (i = 0; i < n; i += step) {
		for (d = 0; d < 4; d++) {
			memcpy(buf, good, n);
			buf[i] = d == 0 ? 0 : d == 1 ? (uint8_t)(good[i] + 1) : d == 2 ? (uint8_t)(good[i] ^ 0x55) : (uint8_t)(good[i] ^ 0x80);
			if (buf[i] == good[i]) continue;
			tried++;
			if (ansm_open(&s, buf, n) == ANSM_OK) { accepted++; (void)walk(&s); }
		}
	}
	free(buf);
	CHECK(tried > 1000 && accepted > 0);                           /* value bytes change freely; tables and indices cannot */
	for (len = 0; len < n; len += (n > 20000 ? n / 1000 : 1)) {
		uint8_t *t = malloc(len ? len : 1);
		CHECK(t);
		memcpy(t, good, len);
		CHECK(ansm_open(&s, t, len) != ANSM_OK);
		CHECK(s.graph_count == 0 && s.anim_total == 0);
		free(t);
	}
	CHECK(ansm_open(&s, good, n + 1) == ANSM_ERR_LAYOUT && ansm_open(&s, good, 63) == ANSM_ERR_TRUNCATED);
	buf = malloc(n);
	CHECK(buf);
	memcpy(buf, good, n);
	buf[0] = 'X';
	CHECK(ansm_open(&s, buf, n) == ANSM_ERR_MAGIC);
	buf[0] = 'A';
	buf[4] = 2;
	CHECK(ansm_open(&s, buf, n) == ANSM_ERR_VERSION);
	free(buf);
	CHECK(ansm_open(&s, NULL, 0) == ANSM_ERR_TRUNCATED);
	printf("corruption: %u changes tried, %u accepted and walked\n", tried, accepted);
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f;
	size_t n;
	uint8_t *img, *raw, *exp = NULL;
	hp2_pack pk;
	const hp2_section *as, *es;
	ansm_section s;
	uint32_t digest, want = 0;

	CHECK(argc == 2 || argc == 3);
	f = fopen(argv[1], "rb");
	CHECK(f);
	fseek(f, 0, SEEK_END);
	n = (size_t)ftell(f);
	rewind(f);
	img = malloc(n);
	CHECK(img && fread(img, 1, n, f) == n);
	fclose(f);
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	as = hp2_find(&pk, "ASTB");
	CHECK(as);
	raw = malloc(as->raw_size);
	CHECK(raw && hp2_load(&pk, as, raw, as->raw_size) == HP2_OK);
	CHECK(ansm_open(&s, raw, as->raw_size) == ANSM_OK);
	digest = walk(&s);
	if (argc == 3) {
		want = (uint32_t)strtoul(argv[2], NULL, 16);
	} else {
		es = hp2_find(&pk, "AEXP");
		CHECK(es && es->raw_size == 12);
		exp = malloc(es->raw_size);
		CHECK(exp && hp2_load(&pk, es, exp, es->raw_size) == HP2_OK);
		want = (uint32_t)exp[0] | (uint32_t)exp[1] << 8 | (uint32_t)exp[2] << 16 | (uint32_t)exp[3] << 24;
		CHECK(s.graph_count == (uint32_t)exp[4] && s.anim_total == (uint32_t)exp[8]);
	}
	if (digest != want) { fprintf(stderr, "FAIL digest %08x, the Python reader says %08x\n", digest, want); return 1; }
	if (check_invariants(&s)) return 1;
	if (argc == 2 && check_world(&s)) return 1;
	if (check_corruption(raw, as->raw_size)) return 1;
	printf("ok: %u graphs, %u animations, digest %08x\n", s.graph_count, s.anim_total, digest);
	free(exp);
	free(raw);
	free(img);
	return 0;
}
