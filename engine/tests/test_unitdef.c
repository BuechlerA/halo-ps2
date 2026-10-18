/* Host test: open the UNIT / UNI1 sections of a pack written by tools/tests/test_unitconv.py and check them against
 * the synthetic world that test builds (2 bipeds: a player-like cyborg and a marine; 2 vehicles: a warthog with 2
 * seats and a chair with 1; 3 materials; UNI1 is an empty section). Then truncate at every length, corrupt every
 * table and field, and flip every byte: a corrupt section is either rejected or safe to walk completely
 * (ASan/UBSan). With a second argument (a real assets/hp2/<map>.unit.hp2) it also checks plausibility ranges of the
 * real data. */
#include "stream/hp2.h"
#include "sim/unitdef.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define TAG(i) ((((uint32_t)0xE174u + (i)) << 16) | (uint32_t)(i))   /* tag ids of the synthetic map: index i of its tag list */
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-5f)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static volatile uint32_t sink;

/* Touch everything an opened section hands out; abort on anything the reader promised not to deliver. */
static void walk(const unitdef_section *s)
{
	uint32_t acc = 0, i, g, l;
	unitdef_globals_def gl;
	unitdef_biped_def b;
	unitdef_vehicle_def v;
	unitdef_seat_def seat;
	unitdef_material_def m;

	unitdef_globals(s, &gl);
	if (!isfinite(gl.gravity) || !isfinite(gl.run_forward) || !isfinite(gl.look_function[5]) || !isfinite(gl.walk_speed)) abort();
	acc += gl.default_biped_tag + gl.damage_tags[5] + gl.grenades[1].projectile + gl.min_weapon_swap_ticks;
	for (g = 0; g < UNITDEF_DIFFICULTY_GROUPS + 2; g++)
		for (l = 0; l < 6; l++) {
			float d = unitdef_difficulty(s, g, l);
			if (!isfinite(d)) abort();
		}
	for (i = 0; i < (uint32_t)s->biped_count + 2u; i++) {
		int got = unitdef_get_biped(s, i, &b);
		if (got != (i < s->biped_count)) abort();
		if (got && unitdef_find_biped(s, b.tag_id) != (int)i) abort();
		if (!isfinite(b.radius) || !isfinite(b.stand_height) || !isfinite(b.shield) || b.radius < 0.0f || b.health < 0.0f) abort();
		if (b.weapon_count > UNITDEF_MAX_WEAPONS || b.dialogue_count > UNITDEF_MAX_DIALOGUE) abort();
		acc += b.tag_id + b.model_tag + (uint32_t)strlen(b.name) + (uint32_t)strlen(b.hold_marker) + (uint32_t)strlen(b.foot_marker[1]) +
		       (uint32_t)strlen(b.camera_marker) + (uint32_t)strlen(b.aux_marker) + b.pelvis_node + b.dialogue_variant[7] + b.dialogue_tag[7];
	}
	for (i = 0; i < (uint32_t)s->vehicle_count + 2u; i++) {
		int got = unitdef_get_vehicle(s, i, &v);
		uint32_t k;
		if (got != (i < s->vehicle_count)) abort();
		if (got && unitdef_find_vehicle(s, v.tag_id) != (int)i) abort();
		if (v.type > UNITDEF_VEH_TURRET || !isfinite(v.max_forward)) abort();
		for (k = 0; k < (uint32_t)v.seat_count + 2u; k++) {
			int ok = unitdef_vehicle_seat(s, &v, k, &seat);
			if (ok != (k < v.seat_count)) abort();
			acc += seat.flags + (uint32_t)strlen(seat.label) + (uint32_t)strlen(seat.marker) + (uint32_t)strlen(seat.camera_marker);
			if (!isfinite(seat.pitch_min) || !isfinite(seat.pitch_max)) abort();
		}
		acc += v.tag_id + (uint32_t)strlen(v.name);
	}
	for (i = 0; i < UNITDEF_MAX_MATERIALS + 2; i++) {
		unitdef_material(s, i, &m);
		if (!isfinite(m.param_a) || !isfinite(m.param_b)) abort();
		acc += m.melee_sound + m.particle_count + (uint32_t)strlen(unitdef_material_name(i));
	}
	if (unitdef_player_biped(s, &b)) acc += b.tag_id;
	sink = acc;
}

static int load_section(const hp2_pack *pk, const char *tag, uint8_t **raw, size_t *n)
{
	const hp2_section *sec = hp2_find(pk, tag);
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(pk, sec, *raw, sec->raw_size) == HP2_OK);
	*n = sec->raw_size;
	return 0;
}

static int check_real(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f);
	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	rewind(f);
	uint8_t *img = malloc(n);
	CHECK(img && fread(img, 1, n, f) == n);
	fclose(f);
	hp2_pack pk;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	uint8_t *raw;
	size_t rn;
	if (load_section(&pk, "UNIT", &raw, &rn)) return 1;
	unitdef_section s;
	CHECK(unitdef_open(&s, raw, rn) == UNITDEF_OK);
	CHECK(s.biped_count >= 8 && s.material_count == 32);
	unitdef_globals_def g;
	unitdef_globals(&s, &g);
	CHECK(g.gravity > 0.00356f && g.gravity < 0.00357f);                 /* 3.2085 wu/s^2 */
	CHECK(NEAR(g.run_forward, 2.25f) && NEAR(g.run_backward, 2.0f) && NEAR(g.sneak_forward, 0.9f));
	unitdef_biped_def p;
	CHECK(unitdef_player_biped(&s, &p) && p.tag_id == g.default_biped_tag && strcmp(p.name, "cyborg") == 0);
	CHECK(NEAR(p.radius, 0.2f) && NEAR(p.stand_height, 0.7f) && NEAR(p.jump_velocity, 0.07f) && p.health == 75.0f && p.shield == 75.0f);
	CHECK(fabsf(p.anim_speed[0] - g.run_forward) < 0.01f && strcmp(p.hold_marker, "right hand") == 0);
	CHECK(p.model_tag && p.anim_tag && p.collision_tag && p.head_node == 12);
	CHECK(NEAR(unitdef_difficulty(&s, UNITDEF_D_ENEMY_DAMAGE, 1), 1.0f) && NEAR(unitdef_difficulty(&s, UNITDEF_D_ENEMY_DAMAGE, 3), 1.8f));
	for (uint32_t i = 0; i < s.biped_count; i++) {
		unitdef_biped_def b;
		CHECK(unitdef_get_biped(&s, i, &b) && b.radius >= 0.0f && b.radius <= 1.0f && b.health <= 1000.0f && b.stand_height <= 2.0f);
	}
	walk(&s);
	free(raw);
	free(img);
	printf("real pack %s: %u bipeds, %u vehicles, %u seats\n", path, (unsigned)s.biped_count, (unsigned)s.vehicle_count, (unsigned)s.seat_count);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc >= 2);
	FILE *f = fopen(argv[1], "rb");
	CHECK(f);
	fseek(f, 0, SEEK_END);
	size_t n = (size_t)ftell(f);
	rewind(f);
	uint8_t *img = malloc(n);
	CHECK(img && fread(img, 1, n, f) == n);
	fclose(f);

	hp2_pack pk;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	uint8_t *raw, *raw1;
	size_t rawn, rawn1;
	if (load_section(&pk, "UNIT", &raw, &rawn) || load_section(&pk, "UNI1", &raw1, &rawn1)) return 1;

	unitdef_section s;
	CHECK(unitdef_open(&s, raw, rawn) == UNITDEF_OK);
	CHECK(s.biped_count == 2 && s.vehicle_count == 2 && s.material_count == 3 && s.seat_count == 3);
	CHECK(s.player_biped_tag == TAG(2) && s.player_biped_index1 == 1);
	CHECK(((uintptr_t)s.globals | (uintptr_t)s.bipeds | (uintptr_t)s.vehicles | (uintptr_t)s.materials | (uintptr_t)s.seats) % 16 == (uintptr_t)raw % 16);

	/* globals */
	unitdef_globals_def g;
	unitdef_globals(&s, &g);
	CHECK(g.default_biped_tag == TAG(2) && g.coop_effect_tag == TAG(31) && g.camera_track_tag == TAG(25));
	CHECK(g.water_enter_sound == TAG(26) && g.water_exit_sound == TAG(27));
	CHECK(NEAR(g.walk_speed, 1.25f) && NEAR(g.run_forward, 2.25f) && NEAR(g.run_backward, 2.0f) && NEAR(g.run_sideways, 2.0f));
	CHECK(NEAR(g.run_accel, 0.32f) && NEAR(g.sneak_forward, 0.9f) && NEAR(g.sneak_backward, 0.65f) && NEAR(g.sneak_sideways, 0.6f));
	CHECK(NEAR(g.sneak_accel, 0.16f) && NEAR(g.airborne_accel, 0.035f) && NEAR(g.pi_unk_2c, 0.512f) && NEAR(g.pi_unk_6c, 0.05f));
	CHECK(NEAR(g.stun_move_penalty, 0.8f) && NEAR(g.stun_turn_penalty, 0.8f) && NEAR(g.stun_jump_penalty, 1.0f));
	CHECK(NEAR(g.stun_min_time, 0.0f) && NEAR(g.stun_max_time, 1.25f) && NEAR(g.fp_idle_min, 10.0f) && NEAR(g.fp_idle_max, 15.0f) && NEAR(g.fp_skip_fraction, 0.25f));
	CHECK(NEAR(g.magnetism_friction, 0.6f) && NEAR(g.magnetism_adhesion, 0.7f) && NEAR(g.inconsequential_scale, 0.5f));
	CHECK(NEAR(g.look_a, 1.0f) && NEAR(g.look_b, 3.0f) && NEAR(g.look_c, 0.85f) && NEAR(g.look_pitch_rate_deg, 60.0f) && NEAR(g.look_yaw_rate_deg, 120.0f) && NEAR(g.look_f, 0.5f));
	CHECK(g.min_weapon_swap_ticks == 7 && g.min_autolevel_ticks == 15 && NEAR(g.min_vehicle_flip_angle, 0.7853982f));
	CHECK(NEAR(g.look_function[0], 0.0f) && NEAR(g.look_function[3], 0.25f) && NEAR(g.look_function[4], 0.58f) && NEAR(g.look_function[5], 1.0f));
	CHECK(NEAR(g.harmful_fall_min, 3.0f) && NEAR(g.harmful_fall_max, 6.0f) && NEAR(g.max_fall_distance, 16.5f));
	CHECK(fabsf(g.gravity - 0.0035651791f) < 1e-7f);
	CHECK(fabsf(g.fall_velocity_harm_min * g.fall_velocity_harm_min / (2.0f * g.harmful_fall_min) - g.gravity) < 1e-6f);
	CHECK(g.damage_tags[0] == TAG(33) && g.damage_tags[1] == TAG(34) && g.damage_tags[5] == TAG(38));
	CHECK(g.grenades[0].max_count == 4 && g.grenades[0].throw_effect == TAG(46) && g.grenades[0].hud_interface == TAG(40) &&
	      g.grenades[0].equipment == TAG(42) && g.grenades[0].projectile == TAG(44));
	CHECK(g.grenades[1].throw_effect == TAG(47) && g.grenades[1].hud_interface == TAG(41) && g.grenades[1].equipment == TAG(43) && g.grenades[1].projectile == TAG(45));
	CHECK(NEAR(unitdef_difficulty(&s, 0, 0), 0.0f) && NEAR(unitdef_difficulty(&s, 7, 2), 7.2f) && NEAR(unitdef_difficulty(&s, 30, 3), 30.3f));
	CHECK(unitdef_difficulty(&s, 31, 0) == 0.0f && unitdef_difficulty(&s, 0, 4) == 0.0f && unitdef_difficulty(&s, 0xFFFFFFFFu, 0xFFFFFFFFu) == 0.0f);

	/* bipeds: sorted by tag id, found by binary search */
	CHECK(unitdef_find_biped(&s, TAG(2)) == 0 && unitdef_find_biped(&s, TAG(3)) == 1);
	CHECK(unitdef_find_biped(&s, 0) == -1 && unitdef_find_biped(&s, 0xFFFFFFFFu) == -1 && unitdef_find_biped(&s, TAG(2) + 1) == -1);
	unitdef_biped_def cy, ma, none;
	CHECK(unitdef_biped(&s, TAG(2), &cy) == 1 && unitdef_biped(&s, TAG(3), &ma) == 1);
	CHECK(strcmp(cy.name, "cyborg") == 0 && cy.model_tag == TAG(6) && cy.anim_tag == TAG(9) && cy.collision_tag == TAG(12) && cy.physics_tag == TAG(16));
	CHECK(cy.camera_track_tag == TAG(24) && cy.footsteps_tag == TAG(17) && cy.melee_damage_tag == TAG(39));
	CHECK(cy.weapon_count == 2 && cy.weapon_tags[0] == TAG(19) && cy.weapon_tags[1] == TAG(18) && cy.weapon_tags[2] == 0);
	CHECK(cy.dialogue_count == 1 && cy.dialogue_tag[0] == TAG(21) && cy.dialogue_variant[0] == 0 && cy.dialogue_tag[1] == 0);
	CHECK(NEAR(cy.radius, 0.2f) && NEAR(cy.stand_height, 0.7f) && NEAR(cy.crouch_height, 0.5f) && NEAR(cy.bounding_radius, 0.45f) && NEAR(cy.render_radius, 0.45f));
	CHECK(NEAR(cy.bounding_offset[0], -0.02f) && NEAR(cy.bounding_offset[1], 0.0f) && NEAR(cy.bounding_offset[2], 0.1f));
	CHECK(NEAR(cy.stand_camera, 0.62f) && NEAR(cy.crouch_camera, 0.35f) && NEAR(cy.crouch_time, 0.2f) && NEAR(cy.jump_velocity, 0.07f));
	CHECK(NEAR(cy.accel_scale, 0.7f) && NEAR(cy.camera_fov, 1.2217f));
	CHECK(NEAR(cy.max_slope, 0.7854f) && NEAR(cy.downhill_falloff, 0.34907f) && NEAR(cy.downhill_cutoff, 0.7854f) && NEAR(cy.downhill_scale, 1.25f));
	CHECK(NEAR(cy.uphill_falloff, 0.34907f) && NEAR(cy.uphill_cutoff, 0.7854f) && NEAR(cy.uphill_scale, 0.65f));
	CHECK(NEAR(cy.turn_speed, 5.8939f) && NEAR(cy.turn_threshold, 1.3963f) && cy.biped_flags == 0x202 && cy.unit_flags == 0x00448008);
	CHECK(NEAR(cy.soft_landing_time, 0.6f) && NEAR(cy.hard_landing_time, 1.0f) && NEAR(cy.min_soft_landing_velocity, 1.5f) &&
	      NEAR(cy.min_hard_landing_velocity, 5.0f) && NEAR(cy.max_hard_landing_velocity, 8.0f) && NEAR(cy.death_landing_velocity, 0.0f));
	CHECK(cy.max_velocity == 0.0f && cy.acceleration == 0.0f && cy.aiming_velocity_max == 0.0f && cy.looking_acceleration_max == 0.0f);
	CHECK(NEAR(cy.health, 75.0f) && NEAR(cy.shield, 75.0f) && NEAR(cy.shield_recharge_time, 4.0f) && NEAR(cy.shield_stun_time, 6.0f));
	CHECK(fabsf(cy.shield_recharge_rate - 1.0f / 120.0f) < 1e-6f && cy.shield_material == 22 && cy.coll_flags == 7 && cy.body_material == 1);
	CHECK(NEAR(cy.grenade_velocity, 10.0f) && cy.grenade_type == 0 && cy.grenade_count == 4);
	CHECK(cy.pelvis_node == 0 && cy.head_node == 12 && cy.object_flags == 2 && cy.default_team == 1);
	CHECK(strcmp(cy.hold_marker, "right hand") == 0 && strcmp(cy.aux_marker, "left hand gun grip") == 0 && cy.camera_marker[0] == 0);
	CHECK(strcmp(cy.foot_marker[0], "left foot") == 0 && strcmp(cy.foot_marker[1], "right foot") == 0);
	CHECK(fabsf(cy.anim_speed[0] - 2.25f) < 1e-3f && fabsf(cy.anim_speed[1] - 2.01f) < 1e-3f && fabsf(cy.anim_speed[2] - 2.01f) < 1e-3f && fabsf(cy.anim_speed[3] - 0.9f) < 1e-3f);
	CHECK(ma.health == 50.0f && ma.shield_recharge_time == 1.0f && ma.weapon_count == 0 && ma.camera_track_tag == 0 && ma.physics_tag == 0);
	CHECK(ma.dialogue_count == 2 && ma.dialogue_variant[0] == 0 && ma.dialogue_variant[1] == 100 && ma.dialogue_tag[0] == TAG(22) && ma.dialogue_tag[1] == TAG(23));
	CHECK(ma.anim_tag == TAG(9) && NEAR(ma.radius, 0.18f) && NEAR(ma.crouch_camera, 0.44f));
	CHECK(unitdef_biped(&s, TAG(9), &none) == 0 && none.tag_id == 0 && none.pelvis_node == UNITDEF_NO_NODE && none.head_node == UNITDEF_NO_NODE && none.name[0] == 0);
	CHECK(unitdef_get_biped(&s, 2, &none) == 0 && none.health == 0.0f);
	unitdef_biped_def pl;
	CHECK(unitdef_player_biped(&s, &pl) == 1 && pl.tag_id == TAG(2) && NEAR(pl.radius, 0.2f));

	/* vehicles */
	CHECK(unitdef_find_vehicle(&s, TAG(4)) == 0 && unitdef_find_vehicle(&s, TAG(5)) == 1 && unitdef_find_vehicle(&s, TAG(2)) == -1);
	unitdef_vehicle_def wh, ch, nv;
	CHECK(unitdef_vehicle(&s, TAG(4), &wh) == 1 && unitdef_vehicle(&s, TAG(5), &ch) == 1 && unitdef_vehicle(&s, TAG(6), &nv) == 0 && nv.tag_id == 0);
	CHECK(strcmp(wh.name, "warthog") == 0 && wh.type == UNITDEF_VEH_JEEP && wh.model_tag == TAG(8) && wh.anim_tag == TAG(10) && wh.collision_tag == TAG(14) && wh.physics_tag == TAG(15));
	CHECK(NEAR(wh.max_forward, 0.255f) && NEAR(wh.max_reverse, 0.1f) && NEAR(wh.accel, 0.00275f) && NEAR(wh.decel, 0.011f));
	CHECK(NEAR(wh.max_left_turn, 30.0f) && NEAR(wh.max_right_turn, -30.0f) && NEAR(wh.wheel_circumference, 1.0f) && NEAR(wh.turn_rate, 180.0f));
	CHECK(wh.vehicle_flags == 0xCD1 && wh.weapon_count == 1 && wh.weapon_tags[0] == TAG(20) && wh.seat_count == 2 && wh.first_seat == 0);
	CHECK(wh.health == 0.0f && wh.shield == 0.0f && wh.unit_flags == 0x00448008 && NEAR(wh.camera_fov, 1.2217f));
	unitdef_seat_def st;
	CHECK(unitdef_vehicle_seat(&s, &wh, 0, &st) == 1 && strcmp(st.label, "W-driver") == 0 && strcmp(st.marker, "driver") == 0 &&
	      strcmp(st.camera_marker, "camera driver") == 0 && st.flags == 0x14 && NEAR(st.pitch_min, -0.5236f) && NEAR(st.pitch_max, 0.2618f));
	CHECK(unitdef_vehicle_seat(&s, &wh, 1, &st) == 1 && strcmp(st.label, "W-gunner") == 0 && st.flags == 0x198 && NEAR(st.pitch_max, 0.6109f));
	CHECK(unitdef_vehicle_seat(&s, &wh, 2, &st) == 0 && st.label[0] == 0 && st.flags == 0);
	CHECK(ch.type == UNITDEF_VEH_TURRET && ch.seat_count == 1 && ch.first_seat == 2 && ch.model_tag == 0 && ch.max_forward == 0.0f);
	CHECK(unitdef_vehicle_seat(&s, &ch, 0, &st) == 1 && strcmp(st.label, "pchair-driver") == 0 && strcmp(st.marker, "seat") == 0);
	{
		unitdef_vehicle_def fake = ch;                          /* a def that does not come from this section */
		fake.first_seat = 3;
		CHECK(unitdef_vehicle_seat(&s, &fake, 0, &st) == 0);
		fake.first_seat = 0xFFFF;
		fake.seat_count = 0xFFFF;
		CHECK(unitdef_vehicle_seat(&s, &fake, 0xFFFFFFFFu, &st) == 0 && unitdef_vehicle_seat(&s, &fake, 0xFFFE, &st) == 0);
	}

	/* materials */
	unitdef_material_def mt;
	unitdef_material(&s, 0, &mt);
	CHECK(mt.melee_sound == TAG(28) && mt.particle_count == 0 && mt.impact_sound == 0 && mt.effect == 0);
	unitdef_material(&s, 1, &mt);
	CHECK(mt.melee_sound == TAG(29) && mt.impact_sound == TAG(30) && mt.particle_tag == TAG(32) && mt.particle_count == 1 && NEAR(mt.param_b, 10.0f));
	unitdef_material(&s, 2, &mt);
	CHECK(NEAR(mt.param_a, 0.125f));
	unitdef_material(&s, 3, &mt);
	CHECK(mt.melee_sound == 0 && mt.param_a == 0.0f);
	unitdef_material(&s, 0xFFFFFFFFu, &mt);
	CHECK(mt.melee_sound == 0);
	CHECK(strcmp(unitdef_material_name(0), "dirt") == 0 && strcmp(unitdef_material_name(22), "cyborg energy shield") == 0 &&
	      strcmp(unitdef_material_name(30), "elite energy shield") == 0 && strcmp(unitdef_material_name(1000), "?") == 0);
	walk(&s);

	/* UNI1: empty section */
	unitdef_section e;
	CHECK(unitdef_open(&e, raw1, rawn1) == UNITDEF_OK);
	CHECK(e.biped_count == 0 && e.vehicle_count == 0 && e.material_count == 0 && e.seat_count == 0 && e.player_biped_index1 == 0);
	CHECK(unitdef_find_biped(&e, TAG(2)) == -1 && unitdef_find_vehicle(&e, TAG(4)) == -1);
	unitdef_globals(&e, &g);
	CHECK(fabsf(g.gravity - 0.0035651791f) < 1e-7f && g.default_biped_tag == 0);
	CHECK(unitdef_player_biped(&e, &none) == 0 && none.head_node == UNITDEF_NO_NODE);
	walk(&e);
	unitdef_section unopened;
	CHECK(unitdef_open(&unopened, raw, 10) == UNITDEF_ERR_TRUNCATED);
	CHECK(unopened.biped_count == 0 && unitdef_find_biped(&unopened, TAG(2)) == -1);   /* a failed open leaves a safe, empty view */
	unitdef_globals(&unopened, &g);
	CHECK(g.gravity == 0.0f && unitdef_difficulty(&unopened, 0, 0) == 0.0f);
	CHECK(unitdef_player_biped(&unopened, &none) == 0);
	walk(&unopened);

	/* ---- hostile input ---- */
	unitdef_section bad;
	for (size_t cut = 0; cut < rawn; cut++) CHECK(unitdef_open(&bad, raw, cut) != UNITDEF_OK);
	CHECK(unitdef_open(&bad, NULL, rawn) != UNITDEF_OK);
	uint8_t *copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(unitdef_open(&bad, copy, rawn + 16) == UNITDEF_ERR_LAYOUT);       /* size field no longer matches */
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(unitdef_open(&bad, copy, rawn) == UNITDEF_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(unitdef_open(&bad, copy, rawn) == UNITDEF_ERR_MAGIC);

	/* every counted table of the header (count at, offset at): moved, misaligned, oversized, counted wrong */
	static const struct { const char *name; size_t count_at, off_at; } tabs[] = {
		{ "bipeds", 0x0C, 0x20 }, { "vehicles", 0x10, 0x24 }, { "materials", 0x14, 0x28 }, { "seats", 0x18, 0x2C } };
	for (size_t t = 0; t < sizeof tabs / sizeof tabs[0]; t++) {
		uint32_t off = rd32(raw + tabs[t].off_at), cnt = rd32(raw + tabs[t].count_at);
		const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)rawn + 16, cnt }, { off, 0xFFFFFFFFu },
		                                                { off, (uint32_t)rawn }, { 0, cnt }, { 0xFFFFFFF0u, cnt } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw, rawn);
			wr32(copy + tabs[t].off_at, tries[k].off);
			wr32(copy + tabs[t].count_at, tries[k].cnt);
			if (unitdef_open(&bad, copy, rawn) == UNITDEF_OK) { fprintf(stderr, "table %s try %zu accepted\n", tabs[t].name, k); return 1; }
		}
		for (int d = -1; d <= 1; d += 2) {                              /* merely wrong counts must never read out of bounds */
			memcpy(copy, raw, rawn);
			wr32(copy + tabs[t].count_at, cnt + (uint32_t)d);
			if (unitdef_open(&bad, copy, rawn) == UNITDEF_OK) walk(&bad);
		}
	}
	memcpy(copy, raw, rawn);
	wr32(copy + 0x1C, 0);                                                   /* globals before the header end */
	CHECK(unitdef_open(&bad, copy, rawn) != UNITDEF_OK);
	memcpy(copy, raw, rawn);
	wr32(copy + 0x1C, (uint32_t)rawn - 16);                                 /* globals run off the end */
	CHECK(unitdef_open(&bad, copy, rawn) != UNITDEF_OK);

	/* targeted fields: each of these must be refused */
	uint32_t og = rd32(raw + 0x1C), ob = rd32(raw + 0x20), ov = rd32(raw + 0x24), om = rd32(raw + 0x28), os = rd32(raw + 0x2C);
	const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
		{ "globals gravity NaN", og + 0xC0, 0x7FC00000u, 4 }, { "globals gravity zero", og + 0xC0, 0, 4 },
		{ "globals gravity negative", og + 0xC0, 0xBB000000u, 4 }, { "globals run speed inf", og + 0x24, 0x7F800000u, 4 },
		{ "globals look function NaN", og + 0xA0, 0x7FC00000u, 4 }, { "globals difficulty NaN", og + 0x130 + 40, 0x7FC00000u, 4 },
		{ "globals difficulty last NaN", og + 0x130 + 123 * 4, 0x7FC00000u, 4 },
		{ "biped order", ob + 576, rd32(raw + ob), 4 }, { "biped radius NaN", ob + 0xA4, 0x7FC00000u, 4 },
		{ "biped radius negative", ob + 0xA4, 0xBF800000u, 4 }, { "biped stand height negative", ob + 0x9C, 0xBF800000u, 4 },
		{ "biped bounding radius negative", ob + 0x70, 0xBF800000u, 4 }, { "biped health negative", ob + 0x13C, 0xBF800000u, 4 },
		{ "biped shield inf", ob + 0x140, 0x7F800000u, 4 }, { "biped last float NaN", ob + 0x14C, 0x7FC00000u, 4 },
		{ "biped first float NaN", ob + 0x074, 0x7FC00000u, 4 }, { "biped weapon count 5", ob + 0x16C, 5, 1 },
		{ "biped dialogue count 9", ob + 0x16D, 9, 1 }, { "biped dialogue variant 0x10000", ob + 0x30, 0x10000, 4 },
		{ "biped name unterminated", ob + 0x170 + 31, 'x', 1 }, { "biped hold marker unterminated", ob + 0x190 + 31, 'x', 1 },
		{ "biped aux unterminated", ob + 0x1B0 + 31, 'x', 1 }, { "biped camera marker unterminated", ob + 0x1D0 + 31, 'x', 1 },
		{ "biped foot 0 unterminated", ob + 0x1F0 + 31, 'x', 1 }, { "biped foot 1 unterminated", ob + 0x1F0 + 63, 'x', 1 },
		{ "vehicle order", ov + 256, rd32(raw + ov), 4 }, { "vehicle speed NaN", ov + 0x40, 0x7FC00000u, 4 },
		{ "vehicle bounding radius negative", ov + 0x28, 0xBF800000u, 4 }, { "vehicle type 7", ov + 0x78, 7, 2 },
		{ "vehicle seats past table", ov + 0x7C, 4, 2 }, { "vehicle first seat past table", ov + 0x7A, 3, 2 },
		{ "vehicle weapon count 5", ov + 0x84, 5, 1 }, { "vehicle name unterminated", ov + 0x86 + 31, 'x', 1 },
		{ "seat pitch NaN", os + 4, 0x7FC00000u, 4 }, { "seat label unterminated", os + 0x10 + 31, 'x', 1 },
		{ "seat marker unterminated", os + 0x30 + 31, 'x', 1 }, { "seat camera unterminated", os + 0x50 + 31, 'x', 1 },
		{ "material param NaN", om + 0x10, 0x7FC00000u, 4 }, { "material param_b NaN", om + 0x14, 0x7FC00000u, 4 },
		{ "player index past the bipeds", 0x34, 3, 4 }, { "player tag differs", 0x30, 1, 4 }, { "header size", 8, 1, 4 } };
	for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
		memcpy(copy, raw, rawn);
		if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
		else if (hit[k].bytes == 2) { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
		else copy[hit[k].at] = (uint8_t)hit[k].val;
		if (unitdef_open(&bad, copy, rawn) == UNITDEF_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
	}

	/* no single-byte change anywhere may let an out-of-bounds read through (both sections) */
	for (size_t at = 4; at < rawn; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (unitdef_open(&bad, copy, rawn) == UNITDEF_OK) walk(&bad);
		}
	for (size_t cut = 0; cut < rawn1; cut++) CHECK(unitdef_open(&bad, raw1, cut) != UNITDEF_OK);
	for (size_t at = 4; at < rawn1; at++) {
		uint8_t *c1 = malloc(rawn1);
		CHECK(c1);
		memcpy(c1, raw1, rawn1);
		c1[at] ^= 0xFF;
		if (unitdef_open(&bad, c1, rawn1) == UNITDEF_OK) walk(&bad);
		free(c1);
	}
	free(copy);
	free(raw);
	free(raw1);
	free(img);
	if (argc >= 3 && check_real(argv[2])) return 1;
	puts("ok");
	return 0;
}
