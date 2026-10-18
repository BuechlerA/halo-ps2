/* Host test: open the AIDT / AID1 sections of a pack written by tools/tests/test_aiconv.py and check them against the
 * synthetic world that test builds (3 actors, 4 actor variants, a 3-entry palette, 3 encounters with 3 squads, 4 start locations,
 * 2 move positions, 3 firing positions, 1 platoon, 1 player start, 4 command lists, 2 conversations, 2 AI animation references,
 * 1 script reference; AID1 is an empty section). Then truncate at every length, corrupt every table and about 80 fields, and
 * flip every byte: a corrupt section is either rejected or safe to walk completely (ASan/UBSan). With further arguments (real
 * assets/hp2/<map>.ai.hp2 packs) it also checks ranges, indices, names and units of the real data. */
#include "stream/hp2.h"
#include "ai/aidef.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-5f)
/* tag ids of the synthetic map: index i of its tag list (tools/tests/test_aiconv.py NAMES) */
#define TAG(i) ((((uint32_t)0xE174u + (i)) << 16) | (uint32_t)(i))
enum { T_SCN, T_ACT_A, T_ACT_B, T_ACT_T, T_VAR_A, T_VAR_B, T_VAR_C, T_VAR_S, T_UNIT_A, T_UNIT_B, T_WPN_A, T_WPN_B, T_GR_N1 = 19, T_GR_N2, T_S1 = 22, T_S2, T_S3 };

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static volatile uint32_t sink;

/* Touch everything an opened section hands out; abort on anything the reader promised not to deliver. */
static void walk(const aidef_section *s)
{
	uint32_t acc = 0, i, k, j;
	aidef_actor_def a;
	aidef_variant_def v, v2;
	aidef_encounter_def e;
	aidef_squad_def q;
	aidef_start_def st;
	aidef_move_def mv;
	aidef_firing_def f;
	aidef_platoon_def pl;
	aidef_pstart_def ps;
	aidef_command_list_def cl;
	aidef_command_def cm;
	aidef_point_def pt;
	aidef_conversation_def cv;
	aidef_participant_def pa;
	aidef_line_def ln;
	aidef_anim_ref_def ar;
	char nm[AIDEF_NAME_LEN];

	for (i = 0; i < s->count[AIDEF_T_ACTORS] + 2; i++) {
		int got = aidef_get_actor(s, i, &a);
		if (got != (i < s->count[AIDEF_T_ACTORS])) abort();
		if (got && aidef_find_actor(s, a.tag_id) != (int)i) abort();
		if (!isfinite(a.max_vision_distance) || !isfinite(a.idle_times[9][1]) || !isfinite(a.cos_looking_deviation[1]) || a.type > AIDEF_ACTOR_MOUNTED_WEAPON) abort();
		if (got && (!a.raw || !isfinite(aidef_actor_f32(&a, 0x300)))) abort();
		acc += a.tag_id + a.flags + (uint32_t)strlen(aidef_actor_type_name(a.type)) + aidef_actor_u32(&a, 0x2F8) + aidef_actor_u16(&a, 0x354) +
		       aidef_actor_u32(&a, 0xFFFFFFF0u) + aidef_actor_u32(&a, 0x4F7);
	}
	for (i = 0; i < s->count[AIDEF_T_VARIANTS] + 2; i++) {
		int got = aidef_get_variant(s, i, &v);
		if (got != (i < s->count[AIDEF_T_VARIANTS])) abort();
		if (got && aidef_find_variant(s, v.tag_id) != (int)i) abort();
		if (!isfinite(v.rate_of_fire) || !isfinite(v.body_vitality) || !isfinite(v.drop_weapon_loaded[1]) || v.grenade_type > 7) abort();
		acc += v.tag_id + v.flags + v.weapon_tag + aidef_variant_u32(&v, 0x74) + aidef_variant_u16(&v, 0x180) + aidef_variant_u32(&v, 0x234);
		if (aidef_variant_actor(s, &v, &a)) acc += a.tag_id;
		if (aidef_variant_major(s, &v, &v2)) acc += v2.tag_id;
	}
	for (i = 0; i < aidef_palette_count(s) + 2; i++) {
		acc += aidef_palette_tag(s, i);
		if (aidef_palette_variant(s, i, &v)) acc += v.tag_id;
	}
	for (i = 0; i < aidef_encounter_count(s) + 2; i++) {
		int got = aidef_get_encounter(s, i, &e);
		if (got != (i < aidef_encounter_count(s))) abort();
		if (got && aidef_find_encounter(s, e.name) < 0) abort();
		acc += e.flags + (uint32_t)strlen(e.name) + e.team;
		for (k = 0; k < (uint32_t)e.squad_count + 1u; k++) {
			if (aidef_encounter_squad(s, i, k, &q) != (got && k < e.squad_count)) abort();
			if (!got || k >= e.squad_count) continue;
			if (aidef_find_squad(s, i, q.name) < 0) abort();
			if (q.actor_type < -1 || q.actor_type >= (int)aidef_palette_count(s) || q.maneuver_squad >= (int)e.squad_count) abort();
			if (q.initial_state >= AIDEF_STATES || q.return_state >= AIDEF_STATES || !isfinite(q.delay_time)) abort();
			acc += q.flags + aidef_squad_spawn_count(&q, k) + (uint32_t)strlen(aidef_state_name(q.initial_state));
			if (aidef_squad_variant(s, &q, &v)) acc += v.tag_id;
			for (j = 0; j < (uint32_t)q.start_count + 1u; j++) {
				if (aidef_get_start(s, &q, j, &st) != (j < q.start_count)) abort();
				if (!isfinite(st.pos[0]) || !isfinite(st.facing) || st.command_list < -1 || st.command_list >= (int)aidef_command_list_count(s)) abort();
				acc += (uint32_t)st.cluster;
			}
			for (j = 0; j < (uint32_t)q.move_count + 1u; j++) {
				if (aidef_get_move(s, &q, j, &mv) != (j < q.move_count)) abort();
				if (!isfinite(mv.weight) || !isfinite(mv.pos[2])) abort();
				acc += (uint32_t)mv.surface;
			}
		}
		for (k = 0; k < (uint32_t)e.firing_count + 1u; k++) {
			if (aidef_get_firing(s, &e, k, &f) != (got && k < e.firing_count)) abort();
			if (!isfinite(f.pos[1])) abort();
			acc += (uint32_t)f.surface;
		}
		for (k = 0; k < (uint32_t)e.platoon_count + 1u; k++) {
			if (aidef_get_platoon(s, &e, k, &pl) != (got && k < e.platoon_count)) abort();
			if (got && k < e.platoon_count && aidef_find_platoon(s, &e, pl.name) < 0) abort();
			acc += (uint32_t)strlen(pl.name) + pl.w20;
		}
		for (k = 0; k < (uint32_t)e.pstart_count + 1u; k++) {
			if (aidef_get_pstart(s, &e, k, &ps) != (got && k < e.pstart_count)) abort();
			if (!isfinite(ps.facing)) abort();
			acc += ps.team;
		}
	}
	for (i = 0; i < aidef_squad_count(s) + 2; i++)
		if (aidef_get_squad(s, i, &q) != (i < aidef_squad_count(s))) abort();
	for (i = 0; i < aidef_command_list_count(s) + 2; i++) {
		int got = aidef_get_command_list(s, i, &cl);
		if (got != (i < aidef_command_list_count(s))) abort();
		if (got && aidef_find_command_list(s, cl.name) < 0) abort();
		for (k = 0; k < (uint32_t)cl.command_count + 1u; k++) {
			if (aidef_get_command(s, &cl, k, &cm) != (got && k < cl.command_count)) abort();
			if (cm.type >= AIDEF_ATOMS || !isfinite(cm.param1) || !isfinite(cm.param2)) abort();
			acc += cm.type + (uint32_t)strlen(aidef_atom_name(cm.type)) + (uint32_t)cm.animation;
		}
		for (k = 0; k < (uint32_t)cl.point_count + 1u; k++) {
			if (aidef_get_point(s, &cl, k, &pt) != (got && k < cl.point_count)) abort();
			if (!isfinite(pt.pos[0])) abort();
			acc += (uint32_t)pt.surface;
		}
	}
	for (i = 0; i < aidef_conversation_count(s) + 2; i++) {
		int got = aidef_get_conversation(s, i, &cv);
		if (got != (i < aidef_conversation_count(s))) abort();
		if (got && aidef_find_conversation(s, cv.name) < 0) abort();
		if (!isfinite(cv.trigger_distance)) abort();
		for (k = 0; k < (uint32_t)cv.participant_count + 1u; k++) {
			if (aidef_get_participant(s, &cv, k, &pa) != (got && k < cv.participant_count)) abort();
			if (pa.encounter_index < -1 || pa.encounter_index >= (int)aidef_encounter_count(s)) abort();
			acc += pa.actor_type + (uint32_t)strlen(pa.encounter_name) + (uint32_t)pa.variants[5];
		}
		for (k = 0; k < (uint32_t)cv.line_count + 1u; k++) {
			if (aidef_get_line(s, &cv, k, &ln) != (got && k < cv.line_count)) abort();
			if (ln.participant < -1 || ln.participant >= (int)cv.participant_count || !isfinite(ln.delay)) abort();
			acc += ln.sounds[0] + ln.sounds[5];
		}
	}
	for (i = 0; i < aidef_anim_ref_count(s) + 2; i++)
		if (aidef_get_anim_ref(s, i, &ar)) acc += ar.graph_tag + (uint32_t)strlen(ar.name);
	for (i = 0; i < aidef_script_ref_count(s) + 2; i++)
		if (aidef_script_ref_name(s, i, nm)) acc += (uint32_t)strlen(nm);
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

static uint8_t *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *img;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	img = malloc(*n);
	if (!img || fread(img, 1, *n, f) != *n) return NULL;
	fclose(f);
	return img;
}

static int check_real(const char *path)
{
	size_t n, rn;
	uint8_t *img = read_file(path, &n), *raw;
	uint32_t i, k, j, pairs = 0;
	hp2_pack pk;
	aidef_section s;
	aidef_encounter_def e;
	aidef_squad_def q;
	aidef_variant_def v;
	aidef_actor_def a;
	CHECK(img && hp2_open(&pk, img, n) == HP2_OK);
	if (load_section(&pk, "AIDT", &raw, &rn)) return 1;
	CHECK(aidef_open(&s, raw, rn) == AIDEF_OK);
	CHECK(s.count[AIDEF_T_ACTORS] >= 7 && s.count[AIDEF_T_VARIANTS] >= 16 && aidef_encounter_count(&s) >= 22 && aidef_squad_count(&s) >= 115);
	for (i = 0; i < s.count[AIDEF_T_ACTORS]; i++) {
		CHECK(aidef_get_actor(&s, i, &a));
		CHECK(a.max_vision_distance > 0.0f && a.max_vision_distance <= 100.0f && a.hearing_distance > 0.0f && a.hearing_distance <= 100.0f);
		CHECK(a.central_vision_angle > 0.0f && a.central_vision_angle <= a.max_vision_angle + 1e-5f);
		if (a.central_vision_angle < 1.99f) CHECK(a.peripheral_vision_angle <= 3.1416f);
		CHECK(a.notice_projectile_chance >= 0.0f && a.notice_projectile_chance <= 1.0f);
		if (a.combat_perception_time > 0.0f) CHECK(fabsf(a.inv_combat_perception_time * a.combat_perception_time * 30.0f - 1.0f) < 1e-5f);
		CHECK(fabsf(cosf(a.max_aiming_deviation[0]) - a.cos_aiming_deviation[0]) < 1e-6f && fabsf(cosf(a.max_looking_deviation[1]) - a.cos_looking_deviation[1]) < 1e-6f);
	}
	for (i = 0; i < s.count[AIDEF_T_VARIANTS]; i++) {
		CHECK(aidef_get_variant(&s, i, &v));
		CHECK(v.rate_of_fire >= 0.0f && v.rate_of_fire <= 30.0f && v.max_firing_distance >= 0.0f && v.max_firing_distance <= 150.0f);
		CHECK(v.desired_combat_range[0] <= v.desired_combat_range[1] && v.desired_combat_range[1] <= 100.0f);
		CHECK(v.grenade_type <= 1 && v.target_tracking >= 0.0f && v.target_tracking <= 1.0f && v.body_vitality >= 0.0f && v.body_vitality <= 1000.0f);
		if (v.actor_tag) CHECK(aidef_variant_actor(&s, &v, &a));
		if (v.major_variant_tag) CHECK(aidef_variant_major(&s, &v, &v));
	}
	for (i = 0; i < aidef_palette_count(&s); i++) CHECK(aidef_palette_tag(&s, i) && aidef_palette_variant(&s, i, &v));
	for (i = 0; i < aidef_encounter_count(&s); i++) {
		int found;
		CHECK(aidef_get_encounter(&s, i, &e));
		found = aidef_find_encounter(&s, e.name);                          /* names are unique on the real maps, or the first one wins */
		CHECK(found >= 0);
		if ((uint32_t)found != i) {
			aidef_encounter_def o;
			CHECK(aidef_get_encounter(&s, (uint32_t)found, &o) && strcmp(o.name, e.name) == 0 && (uint32_t)found < i);
		}
		CHECK(e.precomputed_bsp >= -1 && e.precomputed_bsp < 32 && e.team < AIDEF_TEAMS && !(e.flags & AIDEF_ENC_RESPAWN_ENABLED));
		for (k = 0; k < e.squad_count; k++) {
			char path_[2 * AIDEF_NAME_LEN];
			int g;
			CHECK(aidef_encounter_squad(&s, i, k, &q));
			CHECK(q.initial_state <= AIDEF_STATE_FLEEING && q.normal_count <= 100 && q.insane_count <= 100);
			CHECK(aidef_squad_spawn_count(&q, 0) == q.normal_count && aidef_squad_spawn_count(&q, 3) == q.insane_count && aidef_squad_spawn_count(&q, 99) == q.insane_count);
			if (q.insane_count >= q.normal_count)
				CHECK(aidef_squad_spawn_count(&q, 2) >= q.normal_count && aidef_squad_spawn_count(&q, 2) <= q.insane_count);
			CHECK(strlen(e.name) + strlen(q.name) + 2 < sizeof path_);
			snprintf(path_, sizeof path_, "%s/%s", e.name, q.name);
			g = aidef_find_squad_path(&s, path_);
			CHECK(g >= 0);
			pairs++;
			for (j = 0; j < q.start_count; j++) {
				aidef_start_def st;
				CHECK(aidef_get_start(&s, &q, j, &st) && fabsf(st.facing) <= 3.1416f && st.command_list >= -1 && st.command_list < (int)aidef_command_list_count(&s));
			}
		}
	}
	for (i = 0; i < aidef_command_list_count(&s); i++) {
		aidef_command_list_def cl;
		CHECK(aidef_get_command_list(&s, i, &cl) && aidef_find_command_list(&s, cl.name) >= 0);
		for (k = 0; k < cl.command_count; k++) {
			aidef_command_def cm;
			CHECK(aidef_get_command(&s, &cl, k, &cm) && cm.type <= AIDEF_ATOM_TELEPORT && cm.type != AIDEF_ATOM_RECORDING);
			if (cm.type == AIDEF_ATOM_ANIMATE) CHECK(cm.animation >= -1 && cm.animation < (int)aidef_anim_ref_count(&s));
		}
	}
	walk(&s);
	printf("real pack %s: %u actors, %u variants, %u encounters, %u squads (%u paths found), %u command lists, %u conversations\n", path,
	       (unsigned)s.count[AIDEF_T_ACTORS], (unsigned)s.count[AIDEF_T_VARIANTS], (unsigned)aidef_encounter_count(&s), (unsigned)aidef_squad_count(&s),
	       (unsigned)pairs, (unsigned)aidef_command_list_count(&s), (unsigned)aidef_conversation_count(&s));
	/* a sample of single-byte changes of the real section: rejected or safe */
	{
		uint8_t *copy = malloc(rn);
		size_t at, step = rn / 500 + 1;
		aidef_section bad;
		CHECK(copy);
		for (at = 4; at < rn; at += step) {
			memcpy(copy, raw, rn);
			copy[at] ^= 0xFF;
			if (aidef_open(&bad, copy, rn) == AIDEF_OK) walk(&bad);
		}
		free(copy);
	}
	free(raw);
	free(img);
	return 0;
}

int main(int argc, char **argv)
{
	size_t n, rawn, rawn1, at, cut;
	uint8_t *img, *raw, *raw1, *copy;
	hp2_pack pk;
	aidef_section s, bad, e0;
	int a;
	CHECK(argc >= 2);
	img = read_file(argv[1], &n);
	CHECK(img && hp2_open(&pk, img, n) == HP2_OK);
	if (load_section(&pk, "AIDT", &raw, &rawn) || load_section(&pk, "AID1", &raw1, &rawn1)) return 1;
	CHECK(aidef_open(&s, raw, rawn) == AIDEF_OK);
	CHECK(s.count[AIDEF_T_ACTORS] == 3 && s.count[AIDEF_T_VARIANTS] == 4 && aidef_palette_count(&s) == 3 && aidef_encounter_count(&s) == 3);
	CHECK(aidef_squad_count(&s) == 3 && s.count[AIDEF_T_STARTS] == 4 && s.count[AIDEF_T_MOVES] == 2 && s.count[AIDEF_T_FIRINGS] == 3);
	CHECK(s.count[AIDEF_T_PLATOONS] == 1 && s.count[AIDEF_T_PSTARTS] == 1 && aidef_command_list_count(&s) == 4 && s.count[AIDEF_T_COMMANDS] == 5);
	CHECK(s.count[AIDEF_T_POINTS] == 2 && aidef_conversation_count(&s) == 2 && s.count[AIDEF_T_PARTS] == 3 && s.count[AIDEF_T_LINES] == 2);
	CHECK(aidef_anim_ref_count(&s) == 2 && aidef_script_ref_count(&s) == 1);
	for (a = 0; a < AIDEF_TABLES; a++) CHECK((((uintptr_t)s.tab[a]) - (uintptr_t)raw) % 16 == 0);

	/* ---- actors ---- */
	{
		aidef_actor_def A, B, T, none;
		CHECK(aidef_find_actor(&s, TAG(T_ACT_A)) == 0 && aidef_find_actor(&s, TAG(T_ACT_B)) == 1 && aidef_find_actor(&s, TAG(T_ACT_T)) == 2);
		CHECK(aidef_find_actor(&s, 0) == -1 && aidef_find_actor(&s, TAG(T_ACT_A) + 1) == -1 && aidef_find_actor(&s, 0xFFFFFFFFu) == -1);
		CHECK(aidef_actor(&s, TAG(T_ACT_A), &A) == 1 && aidef_actor(&s, TAG(T_ACT_B), &B) == 1 && aidef_actor(&s, TAG(T_ACT_T), &T) == 1);
		CHECK(aidef_actor(&s, TAG(T_VAR_A), &none) == 0 && none.tag_id == 0 && none.raw == NULL && none.max_vision_distance == 0.0f);
		CHECK(A.tag_id == TAG(T_ACT_A) && A.flags == 0x1234 && A.more_flags == 0x40 && A.type == AIDEF_ACTOR_MARINE);
		CHECK(NEAR(A.max_vision_distance, 40.0f) && NEAR(A.central_vision_angle, 0.8726646f) && NEAR(A.max_vision_angle, 1.2217305f));
		CHECK(NEAR(A.peripheral_vision_angle, 2.0943952f) && NEAR(A.peripheral_distance, 6.0f) && NEAR(A.hearing_distance, 40.0f));
		CHECK(NEAR(A.standing_gun_offset[0], 0.2f) && NEAR(A.standing_gun_offset[1], -0.1f) && NEAR(A.standing_gun_offset[2], 0.51f));
		CHECK(A.crouching_gun_offset[0] == 0.0f && NEAR(A.notice_projectile_chance, 0.5f) && NEAR(A.notice_vehicle_chance, 0.75f));
		CHECK(NEAR(A.combat_perception_time, 0.6f) && NEAR(A.guard_perception_time, 1.4f) && NEAR(A.non_combat_perception_time, 2.0f));
		CHECK(fabsf(A.inv_combat_perception_time - 1.0f / 18.0f) < 1e-6f && fabsf(A.inv_guard_perception_time - 1.0f / 42.0f) < 1e-6f && fabsf(A.inv_non_combat_perception_time - 1.0f / 60.0f) < 1e-6f);
		CHECK(NEAR(A.fractions_80[0], 0.25f) && NEAR(A.fractions_80[2], 0.75f) && NEAR(A.fractions_80[3], 0.2f) && NEAR(A.values_90[1], 5.0f) && NEAR(A.values_90[2], 2.0f));
		CHECK(NEAR(A.angle_9c, 0.6981317f) && NEAR(A.angle_9c_cos, cosf(0.6981317f)));
		CHECK(NEAR(A.max_aiming_deviation[0], 1.5707964f) && NEAR(A.max_looking_deviation[1], 1.0471976f) && NEAR(A.cos_looking_deviation[1], 0.5f));
		CHECK(NEAR(A.looking_angles[0], 1.5f) && NEAR(A.looking_angles[7], 0.8f) && NEAR(A.idle_times[0][0], 0.5f) && NEAR(A.idle_times[9][1], 10.5f));
		CHECK(A.weapon_tag == 0 && A.projectile_tag == 0 && A.actor_tag == 0);
		CHECK(NEAR(aidef_actor_f32(&A, 0x18), 40.0f) && aidef_actor_u32(&A, 0x2F8) == 3 && NEAR(aidef_actor_f32(&A, 0x2FC), 0.6f));
		CHECK(aidef_actor_u16(&A, 0x268) == 4 && aidef_actor_u16(&A, 0x26A) == 3 && aidef_actor_u16(&A, 0x354) == 6 && aidef_actor_u16(&A, 0x356) == 7);
		CHECK(aidef_actor_u32(&A, 0x2F9) == 0 && aidef_actor_u16(&A, 0x2F9) == 0 && aidef_actor_u32(&A, AIDEF_ACTOR_RAW_SIZE) == 0 && aidef_actor_u32(&A, 0xFFFFFFFCu) == 0);
		CHECK(aidef_actor_u32(&A, AIDEF_ACTOR_RAW_SIZE - 4) == 0 && aidef_actor_u16(&A, AIDEF_ACTOR_RAW_SIZE - 2) == 0 && aidef_actor_f32(&none, 0x18) == 0.0f);
		CHECK(B.type == AIDEF_ACTOR_ELITE && NEAR(B.max_vision_distance, 25.0f) && NEAR(B.hearing_distance, 30.0f) && NEAR(B.standing_gun_offset[1], 0.095f));
		CHECK(T.type == AIDEF_ACTOR_MOUNTED_WEAPON && NEAR(T.central_vision_angle, 2.0f) && NEAR(T.peripheral_vision_angle, 40.0f) && T.more_flags == 0x62697064u);
		CHECK(NEAR(T.max_aiming_deviation[0], 3.1415927f));
		CHECK(aidef_get_actor(&s, 3, &none) == 0 && none.tag_id == 0 && aidef_get_actor(&s, 0xFFFFFFFFu, &none) == 0);
	}

	/* ---- variants ---- */
	{
		aidef_variant_def VA, VB, VC, VS, none, major;
		aidef_actor_def act;
		CHECK(aidef_find_variant(&s, TAG(T_VAR_A)) == 0 && aidef_find_variant(&s, TAG(T_VAR_S)) == 3 && aidef_find_variant(&s, TAG(T_ACT_A)) == -1);
		CHECK(aidef_actor_variant(&s, TAG(T_VAR_A), &VA) == 1 && aidef_actor_variant(&s, TAG(T_VAR_B), &VB) == 1);
		CHECK(aidef_actor_variant(&s, TAG(T_VAR_C), &VC) == 1 && aidef_actor_variant(&s, TAG(T_VAR_S), &VS) == 1);
		CHECK(aidef_actor_variant(&s, 5, &none) == 0 && none.tag_id == 0 && none.raw == NULL);
		CHECK(VA.tag_id == TAG(T_VAR_A) && VA.flags == 8 && VA.actor_tag == TAG(T_ACT_A) && VA.unit_tag == TAG(T_UNIT_A) && VA.major_variant_tag == TAG(T_VAR_B));
		CHECK(VA.weapon_tag == TAG(T_WPN_A) && VA.equipment_tag == 0 && VA.movement_type == 1 && VA.change_color_count == 1);
		CHECK(NEAR(VA.initial_crouch_chance, 0.7f) && NEAR(VA.crouch_time[0], 6.0f) && NEAR(VA.crouch_time[1], 8.0f) && NEAR(VA.run_time[1], 4.0f));
		CHECK(NEAR(VA.max_firing_distance, 13.0f) && NEAR(VA.rate_of_fire, 10.0f) && NEAR(VA.projectile_error, 0.0261799f));
		CHECK(NEAR(VA.first_burst_delay[0], 0.25f) && NEAR(VA.first_burst_delay[1], 1.25f) && NEAR(VA.new_target_firing_pattern_time, 5.0f));
		CHECK(NEAR(VA.surprise_delay_time, 0.6f) && NEAR(VA.surprise_fire_wildly_time, 0.3f) && NEAR(VA.death_fire_wildly_chance, 0.1f) && NEAR(VA.death_fire_wildly_time, 3.0f));
		CHECK(NEAR(VA.desired_combat_range[0], 4.0f) && NEAR(VA.desired_combat_range[1], 8.0f) && VA.custom_stand_gun_offset[2] == 0.0f);
		CHECK(NEAR(VA.target_tracking, 0.9f) && NEAR(VA.target_leading, 0.5f) && NEAR(VA.weapon_damage_modifier, 0.5f) && VA.damage_per_second == 0.0f);
		CHECK(NEAR(VA.burst_origin_radius, 0.3f) && NEAR(VA.burst_origin_angle, 0.0872665f) && NEAR(VA.burst_return_length[0], 0.275f) && NEAR(VA.burst_return_angle, 0.1745329f));
		CHECK(NEAR(VA.burst_duration[1], 1.1f) && NEAR(VA.burst_separation[1], 1.5f) && NEAR(VA.burst_angular_velocity, 0.0698132f) && NEAR(VA.special_damage_modifier, 0.4f));
		CHECK(VA.special_fire_mode == 1 && VA.special_fire_situation == 2 && NEAR(VA.special_fire_chance, 0.35f) && NEAR(VA.special_fire_delay, 10.0f));
		CHECK(VA.grenade_type == 0 && VA.grenade_trajectory == 0 && VA.grenade_stimulus == 1 && VA.grenade_min_enemies == 3);
		CHECK(NEAR(VA.grenade_enemy_radius, 3.5f) && NEAR(VA.grenade_velocity, 7.0f / 30.0f) && NEAR(VA.grenade_range[0], 3.0f) && NEAR(VA.grenade_range[1], 14.0f));
		CHECK(NEAR(VA.grenade_collateral_radius, 2.5f) && NEAR(VA.grenade_chance, 0.7f) && NEAR(VA.grenade_check_time, 0.5f) && NEAR(VA.encounter_grenade_timeout, 3.0f));
		CHECK(VA.grenade_count[0] == 1 && VA.grenade_count[1] == 2 && NEAR(VA.dont_drop_grenades_chance, 0.7f) && NEAR(VA.drop_weapon_loaded[0], 0.7f) && NEAR(VA.drop_weapon_loaded[1], 0.9f));
		CHECK(VA.drop_weapon_ammo[0] == 90 && VA.drop_weapon_ammo[1] == 140 && NEAR(VA.body_vitality, 100.0f) && NEAR(VA.shield_vitality, 45.0f) && VA.forced_shader_permutation == 2);
		CHECK(VB.flags == 0 && VB.major_variant_tag == 0 && VB.weapon_tag == TAG(T_WPN_B) && NEAR(VB.body_vitality, 25.0f) && VB.shield_vitality == 0.0f && NEAR(VB.rate_of_fire, 5.0f));
		CHECK(VC.actor_tag == 0 && VC.unit_tag == 0 && VC.weapon_tag == 0 && VC.grenade_type == 1 && VC.grenade_trajectory == 2 && NEAR(VC.body_vitality, 225.0f));
		CHECK(VS.grenade_type == 0 && VS.grenade_stimulus == 0 && VS.grenade_min_enemies == 0 && VS.grenade_enemy_radius == 0.0f && VS.unit_tag == TAG(T_UNIT_B));  /* stale bytes were zeroed */
		CHECK(aidef_variant_actor(&s, &VA, &act) == 1 && act.tag_id == TAG(T_ACT_A) && act.type == AIDEF_ACTOR_MARINE);
		CHECK(aidef_variant_actor(&s, &VC, &act) == 0 && act.tag_id == 0);
		CHECK(aidef_variant_actor(&s, &VS, &act) == 1 && act.type == AIDEF_ACTOR_MOUNTED_WEAPON);
		CHECK(aidef_variant_major(&s, &VA, &major) == 1 && major.tag_id == TAG(T_VAR_B) && aidef_variant_major(&s, &VB, &major) == 0);
		CHECK(NEAR(aidef_variant_f32(&VA, 0x74), 13.0f) && aidef_variant_u16(&VA, 0x4C) == 1 && aidef_variant_u32(&VA, 0x70) == TAG(T_WPN_A) && aidef_variant_u32(&VA, 0x22C) == 1);
		CHECK(aidef_variant_u32(&VA, AIDEF_VARIANT_RAW_SIZE) == 0 && aidef_variant_f32(&VA, 0xFFFFFFF0u) == 0.0f && aidef_variant_u16(&VA, 0x4D) == 0);
		CHECK(aidef_get_variant(&s, 4, &none) == 0 && none.tag_id == 0);
	}

	/* ---- palette ---- */
	{
		aidef_variant_def v;
		CHECK(aidef_palette_tag(&s, 0) == TAG(T_VAR_B) && aidef_palette_tag(&s, 1) == TAG(T_VAR_A) && aidef_palette_tag(&s, 2) == TAG(T_VAR_C) && aidef_palette_tag(&s, 3) == 0);
		CHECK(aidef_palette_variant(&s, 1, &v) == 1 && v.tag_id == TAG(T_VAR_A) && aidef_palette_variant(&s, 3, &v) == 0 && v.tag_id == 0);
		CHECK(aidef_palette_variant(&s, 0xFFFFFFFFu, &v) == 0);
	}

	/* ---- encounters and names (case-insensitive) ---- */
	{
		aidef_encounter_def E0, E1, E2, none;
		CHECK(aidef_find_encounter(&s, "enc_b") == 0 && aidef_find_encounter(&s, "ENC_B") == 0 && aidef_find_encounter(&s, "Enc_a") == 1 && aidef_find_encounter(&s, "enc_c") == 2);
		CHECK(aidef_find_encounter(&s, "enc") == -1 && aidef_find_encounter(&s, "enc_bb") == -1 && aidef_find_encounter(&s, "") == -1 && aidef_find_encounter(&s, NULL) == -1);
		CHECK(aidef_get_encounter(&s, 0, &E0) && aidef_get_encounter(&s, 1, &E1) && aidef_get_encounter(&s, 2, &E2));
		CHECK(strcmp(E0.name, "enc_b") == 0 && strcmp(E1.name, "Enc_A") == 0 && E0.flags == 0x61 && E0.team == AIDEF_TEAM_DEFAULT_BY_UNIT && E1.team == AIDEF_TEAM_FLOOD);
		CHECK((E0.flags & AIDEF_ENC_NOT_INITIALLY_CREATED) && (E0.flags & AIDEF_ENC_3D_FIRING_POSITIONS) && (E0.flags & AIDEF_ENC_MANUAL_BSP_INDEX));
		CHECK(E0.search_behavior == 1 && E0.w28 == 2 && E0.manual_bsp == 1 && E0.precomputed_bsp == 1 && E2.precomputed_bsp == -1);
		CHECK(E0.first_squad == 0 && E0.squad_count == 2 && E0.platoon_count == 1 && E0.firing_count == 3 && E0.pstart_count == 1);
		CHECK(E1.first_squad == 2 && E1.squad_count == 1 && E1.first_firing == 3 && E2.squad_count == 0 && E2.first_squad == 3);
		CHECK(aidef_get_encounter(&s, 3, &none) == 0 && none.name[0] == 0 && none.squad_count == 0 && aidef_get_encounter(&s, 0xFFFFFFFFu, &none) == 0);
		CHECK(strcmp(aidef_team_name(AIDEF_TEAM_COVENANT), "covenant") == 0 && strcmp(aidef_team_name(99), "?") == 0);
	}

	/* ---- squads and children ---- */
	{
		aidef_squad_def Q0, Q1, Q2, none;
		aidef_start_def st;
		aidef_move_def mv;
		aidef_firing_def f;
		aidef_platoon_def pl;
		aidef_pstart_def ps;
		aidef_encounter_def E0, E1;
		aidef_variant_def v;
		CHECK(aidef_get_encounter(&s, 0, &E0) && aidef_get_encounter(&s, 1, &E1));
		CHECK(aidef_find_squad(&s, 0, "sq_one") == 0 && aidef_find_squad(&s, 0, "SQ_TWO") == 1 && aidef_find_squad(&s, 1, "sq_one") == 2 && aidef_find_squad(&s, 2, "sq_one") == -1);
		CHECK(aidef_find_squad(&s, 0, "nope") == -1 && aidef_find_squad(&s, 9, "sq_one") == -1 && aidef_find_squad(&s, 0, NULL) == -1);
		CHECK(aidef_find_squad_path(&s, "enc_b/sq_one") == 0 && aidef_find_squad_path(&s, "enc_a/sq_one") == 2 && aidef_find_squad_path(&s, "ENC_B/sq_two") == 1);
		CHECK(aidef_find_squad_path(&s, "enc_b") == -1 && aidef_find_squad_path(&s, "/sq_one") == -1 && aidef_find_squad_path(&s, "enc_b/nope") == -1 && aidef_find_squad_path(&s, "nope/sq_one") == -1);
		CHECK(aidef_find_squad_path(&s, "0123456789012345678901234567890123456789/sq_one") == -1 && aidef_find_squad_path(&s, NULL) == -1);
		CHECK(aidef_get_squad(&s, 0, &Q0) && aidef_get_squad(&s, 1, &Q1) && aidef_get_squad(&s, 2, &Q2));
		CHECK(strcmp(Q0.name, "sq_one") == 0 && strcmp(Q2.name, "SQ_ONE") == 0);
		CHECK(Q0.actor_type == 1 && Q0.platoon == 0 && Q0.initial_state == AIDEF_STATE_SEARCHING && Q0.return_state == AIDEF_STATE_GUARDING_AT_GUARD_POSITION);
		CHECK(Q0.flags == AIDEF_SQUAD_AUTOMATIC_MIGRATION && Q0.unique_leader == 0 && Q0.maneuver_squad == 1 && NEAR(Q0.delay_time, 2.5f));
		CHECK(Q0.masks[0] == 0x03FFFFFFu && Q0.masks[2] == 1 && Q0.masks[6] == 0x100 && Q0.normal_count == 2 && Q0.insane_count == 4 && Q0.major_upgrade == 1);
		CHECK(Q0.w82 == 3 && Q0.w84 == 5 && Q0.w86 == 7 && Q0.start_count == 3 && Q0.move_count == 2 && Q0.first_start == 0 && Q0.first_move == 0);
		CHECK(Q1.actor_type == -1 && Q1.platoon == -1 && Q1.unique_leader == 4 && Q1.start_count == 0 && Q1.first_start == 3 && Q2.start_count == 1 && Q2.first_start == 3);
		CHECK(Q2.initial_state == AIDEF_STATE_ALERT && Q2.return_state == 3 && Q2.flags == AIDEF_SQUAD_START_TIMER_IMMEDIATELY && Q2.actor_type == 0);
		CHECK(aidef_encounter_squad(&s, 0, 1, &none) == 1 && strcmp(none.name, "sq_two") == 0 && aidef_encounter_squad(&s, 0, 2, &none) == 0 && none.actor_type == -1);
		CHECK(aidef_encounter_squad(&s, 5, 0, &none) == 0 && aidef_encounter_squad(&s, 2, 0, &none) == 0);
		CHECK(aidef_get_squad(&s, 3, &none) == 0 && none.maneuver_squad == -1 && none.name[0] == 0);
		/* spawn counts per difficulty: easy and normal 2, hard 3, impossible 4; clamped beyond */
		CHECK(aidef_squad_spawn_count(&Q0, 0) == 2 && aidef_squad_spawn_count(&Q0, 1) == 2 && aidef_squad_spawn_count(&Q0, 2) == 3 && aidef_squad_spawn_count(&Q0, 3) == 4);
		CHECK(aidef_squad_spawn_count(&Q0, 4) == 4 && aidef_squad_spawn_count(&Q0, 0xFFFFFFFFu) == 4 && aidef_squad_spawn_count(&Q1, 3) == 0 && aidef_squad_spawn_count(&Q2, 2) == 1);
		/* palette variant of a squad */
		CHECK(aidef_squad_variant(&s, &Q0, &v) == 1 && v.tag_id == TAG(T_VAR_A) && aidef_squad_variant(&s, &Q2, &v) == 1 && v.tag_id == TAG(T_VAR_B));
		CHECK(aidef_squad_variant(&s, &Q1, &v) == 0 && v.tag_id == 0);
		/* starting locations */
		CHECK(aidef_get_start(&s, &Q0, 0, &st) == 1 && st.pos[0] == 1.0f && st.pos[1] == 2.0f && st.pos[2] == 3.0f && NEAR(st.facing, 0.5f) && st.cluster == 4 && st.command_list == 0);
		CHECK(aidef_get_start(&s, &Q0, 1, &st) == 1 && st.cluster == 5 && st.w12 == 3 && st.w14 == 9 && st.w16 == 7 && st.w18 == 2 && st.command_list == -1 && NEAR(st.facing, -3.1f));
		CHECK(aidef_get_start(&s, &Q0, 2, &st) == 1 && st.pos[0] == -7.0f && st.command_list == 2 && aidef_get_start(&s, &Q0, 3, &st) == 0 && st.command_list == -1);
		CHECK(aidef_get_start(&s, &Q2, 0, &st) == 1 && st.pos[2] == 22.0f && aidef_get_start(&s, &Q1, 0, &st) == 0);
		{
			aidef_squad_def fake = Q2;                        /* a def that does not come from this section */
			fake.first_start = 4;
			CHECK(aidef_get_start(&s, &fake, 0, &st) == 0);
			fake.first_start = 0xFFFFFFFFu;
			fake.start_count = 0xFFFFFFFFu;
			CHECK(aidef_get_start(&s, &fake, 0, &st) == 0 && aidef_get_start(&s, &fake, 0xFFFFFFFEu, &st) == 0);
			fake.first_move = 0xFFFFFFFFu;
			fake.move_count = 0xFFFFFFFFu;
			CHECK(aidef_get_move(&s, &fake, 1, &mv) == 0);
		}
		/* move positions */
		CHECK(aidef_get_move(&s, &Q0, 0, &mv) == 1 && mv.pos[1] == 11.0f && NEAR(mv.facing, 1.5f) && NEAR(mv.weight, 0.001f) && mv.time_min == 0.0f && mv.animation == -1 && mv.sequence == 0 && mv.cluster == 3 && mv.surface == -1);
		CHECK(aidef_get_move(&s, &Q0, 1, &mv) == 1 && NEAR(mv.facing, -1.5f) && NEAR(mv.weight, 2.0f) && NEAR(mv.time_min, 1.0f) && NEAR(mv.time_max, 4.0f) && mv.sequence == 3 && mv.cluster == 4 && mv.surface == 77);
		CHECK(aidef_get_move(&s, &Q0, 2, &mv) == 0 && mv.surface == -1 && aidef_get_move(&s, &Q2, 0, &mv) == 0);
		/* firing positions, platoons, player starts of the first encounter */
		CHECK(aidef_get_firing(&s, &E0, 0, &f) == 1 && f.pos[2] == 2.5f && f.group == 0 && f.cluster == 4 && f.surface == 48);
		CHECK(aidef_get_firing(&s, &E0, 1, &f) == 1 && f.group == 1 && f.cluster == 5 && f.surface == -1 && f.pos[0] == 3.5f);
		CHECK(aidef_get_firing(&s, &E0, 2, &f) == 1 && f.group == 25 && f.cluster == -1 && f.surface == 12345);
		CHECK(aidef_get_firing(&s, &E0, 3, &f) == 0 && f.surface == -1 && aidef_get_firing(&s, &E1, 0, &f) == 0);
		CHECK(aidef_get_platoon(&s, &E0, 0, &pl) == 1 && strcmp(pl.name, "plt_one") == 0 && pl.w20 == 6 && pl.w30 == 0 && pl.w32 == -1 && pl.w3c == 0 && pl.w3e == -1);
		CHECK(aidef_find_platoon(&s, &E0, "PLT_ONE") == 0 && aidef_find_platoon(&s, &E0, "x") == -1 && aidef_find_platoon(&s, &E1, "plt_one") == -1 && aidef_get_platoon(&s, &E0, 1, &pl) == 0);
		CHECK(aidef_get_pstart(&s, &E0, 0, &ps) == 1 && ps.pos[0] == 9.0f && ps.pos[2] == 7.0f && NEAR(ps.facing, 1.25f) && ps.team == 2 && ps.bsp == 1 && ps.types[3] == 0);
		CHECK(aidef_get_pstart(&s, &E0, 1, &ps) == 0 && aidef_get_pstart(&s, &E1, 0, &ps) == 0);
		CHECK(strcmp(aidef_state_name(AIDEF_STATE_FLEEING), "fleeing") == 0 && strcmp(aidef_state_name(12), "?") == 0 && strcmp(aidef_actor_type_name(7), "marine") == 0);
	}

	/* ---- command lists ---- */
	{
		aidef_command_list_def L0, L1, L2, L3, none;
		aidef_command_def c;
		aidef_point_def p;
		CHECK(aidef_find_command_list(&s, "cmd_b") == 0 && aidef_find_command_list(&s, "CMD_A") == 1 && aidef_find_command_list(&s, "cmd_a") == 1 && aidef_find_command_list(&s, "cmd_c") == 2);
		CHECK(aidef_find_command_list(&s, "CMD_B") == 0);                    /* a repeated name: the first (lowest index) one wins */
		CHECK(aidef_find_command_list(&s, "cmd") == -1 && aidef_find_command_list(&s, NULL) == -1);
		CHECK(aidef_get_command_list(&s, 0, &L0) && aidef_get_command_list(&s, 1, &L1) && aidef_get_command_list(&s, 2, &L2) && aidef_get_command_list(&s, 3, &L3));
		CHECK(L0.flags == 0x20 && L0.manual_bsp == 0 && L0.command_count == 4 && L0.point_count == 2 && L1.manual_bsp == -1 && L1.first_command == 4 && L1.command_count == 1);
		CHECK(L2.manual_bsp == 2 && L2.flags == 0x24 && L2.command_count == 0 && L2.first_command == 5 && L2.first_point == 2);
		CHECK(L0.precomputed_bsp == 1 && L1.precomputed_bsp == -1 && L2.precomputed_bsp == 3);
		CHECK(strcmp(L3.name, "CMD_B") == 0 && L3.command_count == 0 && L3.first_command == 5 && L3.first_point == 2);
		CHECK(aidef_get_command_list(&s, 4, &none) == 0 && none.name[0] == 0);
		CHECK(aidef_get_command(&s, &L0, 0, &c) == 1 && c.type == AIDEF_ATOM_ANIMATION_MODE && c.animation == -1 && c.script == -1);
		CHECK(aidef_get_command(&s, &L0, 1, &c) == 1 && c.type == AIDEF_ATOM_ANIMATE && c.animation == 1 && NEAR(c.param1, 1.0f));
		CHECK(aidef_get_command(&s, &L0, 2, &c) == 1 && c.type == AIDEF_ATOM_LOOP && c.modifier == 1 && c.command == 1 && c.point1 == 0 && c.point2 == 1 && NEAR(c.param1, 0.6f) && NEAR(c.param2, 2.6f));
		CHECK(aidef_get_command(&s, &L0, 3, &c) == 1 && c.type == AIDEF_ATOM_SCRIPT && c.script == 0 && c.object_name == 3 && c.recording == -1);
		CHECK(aidef_get_command(&s, &L0, 4, &c) == 0 && c.command == -1 && aidef_get_command(&s, &L1, 0, &c) == 1 && c.type == AIDEF_ATOM_MOVE_IN_DIRECTION && NEAR(c.param2, 90.0f));
		CHECK(aidef_get_command(&s, &L2, 0, &c) == 0);
		CHECK(aidef_get_point(&s, &L0, 0, &p) == 1 && p.pos[0] == 1.0f && p.pos[2] == 3.0f && p.surface == 7 && aidef_get_point(&s, &L0, 1, &p) == 1 && p.pos[1] == 5.5f && p.surface == -1);
		CHECK(aidef_get_point(&s, &L0, 2, &p) == 0 && aidef_get_point(&s, &L1, 0, &p) == 0);
		CHECK(strcmp(aidef_atom_name(AIDEF_ATOM_LOOP), "loop") == 0 && strcmp(aidef_atom_name(99), "?") == 0);
	}

	/* ---- conversations, animation and script references ---- */
	{
		aidef_conversation_def V0, V1, none;
		aidef_participant_def p;
		aidef_line_def l;
		aidef_anim_ref_def ar;
		char nm[AIDEF_NAME_LEN];
		CHECK(aidef_find_conversation(&s, "conv_b") == 0 && aidef_find_conversation(&s, "CONV_A") == 1 && aidef_find_conversation(&s, "conv") == -1 && aidef_find_conversation(&s, NULL) == -1);
		CHECK(aidef_get_conversation(&s, 0, &V0) && aidef_get_conversation(&s, 1, &V1));
		CHECK(V0.flags == 0xF2 && NEAR(V0.trigger_distance, 4.0f) && NEAR(V0.run_to_player_distance, 4.5f) && V0.participant_count == 2 && V0.line_count == 2 && V1.first_participant == 2 && V1.line_count == 0);
		CHECK(aidef_get_conversation(&s, 2, &none) == 0 && none.name[0] == 0);
		CHECK(aidef_get_participant(&s, &V0, 0, &p) == 1 && p.actor_type == AIDEF_ACTOR_MARINE && p.encounter_index == 0 && strcmp(p.encounter_name, "enc_b") == 0 && p.use_object == -1 && p.variants[0] == -1);
		CHECK(aidef_get_participant(&s, &V0, 1, &p) == 1 && p.actor_type == AIDEF_ACTOR_NONE && p.encounter_index == -1 && p.encounter_name[0] == 0 && p.variants[0] == 100 && p.variants[1] == 101 && p.variants[2] == -1);
		CHECK(p.use_object == 12 && p.new_name_object == 13 && p.w02 == 1 && p.selection_type == 2 && aidef_get_participant(&s, &V0, 2, &p) == 0 && p.encounter_index == -1 && p.variants[5] == -1);
		CHECK(aidef_get_participant(&s, &V1, 0, &p) == 1 && p.encounter_index == 2 && p.actor_type == AIDEF_ACTOR_CREW);
		CHECK(aidef_get_line(&s, &V0, 0, &l) == 1 && l.participant == 0 && l.addressee == 1 && l.addressee_participant == -1 && NEAR(l.delay, 1.5f) && l.sounds[0] == TAG(T_S1) && l.sounds[1] == TAG(T_S2) && l.sounds[2] == 0);
		CHECK(aidef_get_line(&s, &V0, 1, &l) == 1 && l.flags == 4 && l.participant == 1 && l.addressee_participant == 0 && l.sounds[0] == TAG(T_S3));
		CHECK(aidef_get_line(&s, &V0, 2, &l) == 0 && l.participant == -1 && aidef_get_line(&s, &V1, 0, &l) == 0);
		CHECK(aidef_get_anim_ref(&s, 0, &ar) == 1 && strcmp(ar.name, "anim_one") == 0 && ar.graph_tag == TAG(T_GR_N1) && aidef_get_anim_ref(&s, 1, &ar) == 1 && ar.graph_tag == TAG(T_GR_N2));
		CHECK(aidef_get_anim_ref(&s, 2, &ar) == 0 && ar.graph_tag == 0 && aidef_script_ref_name(&s, 0, nm) == 1 && strcmp(nm, "script_one") == 0 && aidef_script_ref_name(&s, 1, nm) == 0 && nm[0] == 0);
	}
	walk(&s);

	/* ---- AID1: an empty section ---- */
	CHECK(aidef_open(&e0, raw1, rawn1) == AIDEF_OK);
	CHECK(aidef_encounter_count(&e0) == 0 && aidef_squad_count(&e0) == 0 && aidef_palette_count(&e0) == 0 && e0.count[AIDEF_T_ACTORS] == 0);
	{
		aidef_actor_def a0;
		aidef_encounter_def en;
		CHECK(aidef_find_actor(&e0, TAG(T_ACT_A)) == -1 && aidef_find_encounter(&e0, "enc_b") == -1 && aidef_find_command_list(&e0, "x") == -1 && aidef_find_squad_path(&e0, "a/b") == -1);
		CHECK(aidef_actor(&e0, TAG(T_ACT_A), &a0) == 0 && aidef_get_encounter(&e0, 0, &en) == 0);
	}
	walk(&e0);
	{
		aidef_section unopened;
		CHECK(aidef_open(&unopened, raw, 10) == AIDEF_ERR_TRUNCATED && unopened.count[AIDEF_T_ACTORS] == 0 && unopened.tab[AIDEF_T_ACTORS] == NULL);
		CHECK(aidef_find_actor(&unopened, TAG(T_ACT_A)) == -1 && aidef_encounter_count(&unopened) == 0);   /* a failed open leaves a safe, empty view */
		walk(&unopened);
	}

	/* ---- hostile input ---- */
	for (cut = 0; cut < rawn; cut++) CHECK(aidef_open(&bad, raw, cut) != AIDEF_OK);
	CHECK(aidef_open(&bad, NULL, rawn) != AIDEF_OK);
	copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(aidef_open(&bad, copy, rawn + 16) == AIDEF_ERR_LAYOUT);          /* size field no longer matches */
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(aidef_open(&bad, copy, rawn) == AIDEF_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(aidef_open(&bad, copy, rawn) == AIDEF_ERR_MAGIC);
	memcpy(copy, raw, rawn);
	CHECK(aidef_open(&bad, copy, rawn) == AIDEF_OK);

	/* every table of the directory: moved, misaligned, oversized, counted wrong */
	for (a = 0; a < AIDEF_TABLES; a++) {
		size_t dir = 16u + 8u * (size_t)a;
		uint32_t off = rd32(raw + dir), cnt = rd32(raw + dir + 4);
		const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)rawn + 16, cnt }, { off, 0xFFFFFFFFu }, { off, (uint32_t)rawn },
		                                                { 0, cnt }, { 0xFFFFFFF0u, cnt }, { off, 70000 } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw, rawn);
			wr32(copy + dir, tries[k].off);
			wr32(copy + dir + 4, tries[k].cnt);
			if (aidef_open(&bad, copy, rawn) == AIDEF_OK) { fprintf(stderr, "table %d try %zu accepted\n", a, k); return 1; }
		}
		for (int d = -1; d <= 1; d += 2) {                                  /* merely wrong counts must never read out of bounds */
			memcpy(copy, raw, rawn);
			wr32(copy + dir + 4, cnt + (uint32_t)d);
			if (aidef_open(&bad, copy, rawn) == AIDEF_OK) walk(&bad);
		}
	}

	/* targeted fields: each of these must be refused */
	{
		uint32_t t_act = rd32(raw + 16 + 8 * AIDEF_T_ACTORS), t_var = rd32(raw + 16 + 8 * AIDEF_T_VARIANTS), t_aid = rd32(raw + 16 + 8 * AIDEF_T_ACTOR_IDS);
		uint32_t t_vid = rd32(raw + 16 + 8 * AIDEF_T_VARIANT_IDS), t_pal = rd32(raw + 16 + 8 * AIDEF_T_PALETTE), t_enc = rd32(raw + 16 + 8 * AIDEF_T_ENCOUNTERS);
		uint32_t t_sq = rd32(raw + 16 + 8 * AIDEF_T_SQUADS), t_st = rd32(raw + 16 + 8 * AIDEF_T_STARTS), t_mv = rd32(raw + 16 + 8 * AIDEF_T_MOVES);
		uint32_t t_fi = rd32(raw + 16 + 8 * AIDEF_T_FIRINGS), t_pl = rd32(raw + 16 + 8 * AIDEF_T_PLATOONS), t_ps = rd32(raw + 16 + 8 * AIDEF_T_PSTARTS);
		uint32_t t_cl = rd32(raw + 16 + 8 * AIDEF_T_CMDLISTS), t_cm = rd32(raw + 16 + 8 * AIDEF_T_COMMANDS), t_pt = rd32(raw + 16 + 8 * AIDEF_T_POINTS);
		uint32_t t_cv = rd32(raw + 16 + 8 * AIDEF_T_CONVS), t_pa = rd32(raw + 16 + 8 * AIDEF_T_PARTS), t_ln = rd32(raw + 16 + 8 * AIDEF_T_LINES);
		uint32_t t_an = rd32(raw + 16 + 8 * AIDEF_T_ANIMS), t_sc = rd32(raw + 16 + 8 * AIDEF_T_SCRIPTS), t_en = rd32(raw + 16 + 8 * AIDEF_T_ENC_NAMES);
		uint32_t t_cn = rd32(raw + 16 + 8 * AIDEF_T_CMD_NAMES), t_vn = rd32(raw + 16 + 8 * AIDEF_T_CONV_NAMES);
		const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
			{ "actor type 16", t_act + 0x14, 16, 2 }, { "actor float inf", t_act + 0x18, 0x7F800000u, 4 }, { "actor first float NaN", t_act + 0x1C, 0x7FC00000u, 4 },
			{ "actor unnamed float NaN", t_act + 0x300, 0x7FC00000u, 4 }, { "actor last float NaN", t_act + 0x4B4, 0x7FC00000u, 4 },
			{ "actor cached cosine NaN", t_act + 0x138, 0x7FC00000u, 4 }, { "actor idle time NaN", t_act + 0x120, 0x7FC00000u, 4 },
			{ "actor ids order", t_aid + 4, rd32(raw + t_aid), 4 }, { "actor ids descending", t_aid, 0xFFFFFFF0u, 4 },
			{ "variant movement type 8", t_var + 0x4C, 8, 2 }, { "variant special fire mode 8", t_var + 0x154, 8, 2 }, { "variant special situation 8", t_var + 0x156, 8, 2 },
			{ "variant grenade type 8", t_var + 0x180, 8, 2 }, { "variant grenade trajectory 8", t_var + 0x182, 8, 2 }, { "variant grenade stimulus 8", t_var + 0x184, 8, 2 },
			{ "variant min enemies 256", t_var + 0x186, 256, 2 }, { "variant change colors 256", t_var + 0x22C, 256, 4 }, { "variant float NaN", t_var + 0x74, 0x7FC00000u, 4 },
			{ "variant last float NaN", t_var + 0x228, 0x7FC00000u, 4 }, { "variant body vitality NaN", t_var + 0x200, 0x7FC00000u, 4 },
			{ "variant ids order", t_vid + 4, rd32(raw + t_vid), 4 },
			{ "palette index past the variants", t_pal + 4, 4, 4 }, { "palette tag differs", t_pal, 1, 4 }, { "palette index of another variant", t_pal + 4, 2, 4 },
			{ "encounter name unterminated", t_enc + 31, 'x', 1 }, { "encounter name unprintable", t_enc, 1, 1 }, { "encounter team 10", t_enc + 36, 10, 2 },
			{ "encounter squads shifted", t_enc + 48, 1, 4 }, { "encounter squad count + 1", t_enc + 52, 3, 4 }, { "encounter platoons shifted", t_enc + 56, 1, 4 },
			{ "encounter firing count 4", t_enc + 68, 4, 4 }, { "encounter pstart shifted", t_enc + 72, 1, 4 },
			{ "squad name unterminated", t_sq + 31, 'x', 1 }, { "squad name unprintable", t_sq + 3, 0x7F, 1 }, { "squad actor type 3", t_sq + 32, 3, 2 },
			{ "squad actor type -2", t_sq + 32, 0xFFFE, 2 }, { "squad platoon 1", t_sq + 34, 1, 2 }, { "squad maneuver 2", t_sq + 46, 2, 2 },
			{ "squad initial state 12", t_sq + 36, 12, 2 }, { "squad return state 12", t_sq + 38, 12, 2 }, { "squad leader 5", t_sq + 44, 5, 2 },
			{ "squad upgrade 5", t_sq + 84, 5, 2 }, { "squad normal count 1001", t_sq + 80, 1001, 2 }, { "squad insane count 1001", t_sq + 82, 1001, 2 },
			{ "squad delay NaN", t_sq + 48, 0x7FC00000u, 4 }, { "squad starts shifted", t_sq + 92, 1, 4 }, { "squad move count 3", t_sq + 104, 3, 4 },
			{ "start command list 4", t_st + 26, 4, 2 }, { "start command list -2", t_st + 26, 0xFFFE, 2 }, { "start facing NaN", t_st + 12, 0x7FC00000u, 4 },
			{ "start position inf", t_st, 0x7F800000u, 4 }, { "move facing NaN", t_mv + 12, 0x7FC00000u, 4 }, { "move weight inf", t_mv + 16, 0x7F800000u, 4 },
			{ "move last float inf", t_mv + 24, 0x7F800000u, 4 }, { "firing position NaN", t_fi + 4, 0x7FC00000u, 4 },
			{ "platoon name unterminated", t_pl + 31, 'x', 1 }, { "pstart facing NaN", t_ps + 12, 0x7FC00000u, 4 },
			{ "command list name unterminated", t_cl + 31, 'x', 1 }, { "command list commands shifted", t_cl + 40, 1, 4 }, { "command list points shifted", t_cl + 48, 1, 4 },
			{ "command type 28", t_cm, 28, 2 }, { "command param NaN", t_cm + 4, 0x7FC00000u, 4 }, { "command param2 inf", t_cm + 8, 0x7F800000u, 4 },
			{ "point NaN", t_pt, 0x7FC00000u, 4 }, { "conversation name unterminated", t_cv + 31, 'x', 1 }, { "conversation trigger NaN", t_cv + 36, 0x7FC00000u, 4 },
			{ "conversation run distance NaN", t_cv + 40, 0x7FC00000u, 4 }, { "conversation participants shifted", t_cv + 44, 1, 4 }, { "conversation lines shifted", t_cv + 52, 1, 4 },
			{ "participant encounter 3", t_pa + 24, 3, 4 }, { "participant encounter -2", t_pa + 24, 0xFFFFFFFEu, 4 }, { "participant name unterminated", t_pa + 28 + 31, 'x', 1 },
			{ "line participant 2", t_ln + 2, 2, 2 }, { "line participant -2", t_ln + 2, 0xFFFE, 2 }, { "line delay NaN", t_ln + 8, 0x7FC00000u, 4 },
			{ "anim name unterminated", t_an + 31, 'x', 1 }, { "script name unterminated", t_sc + 31, 'x', 1 },
			{ "encounter name table index 3", t_en, 3, 4 }, { "encounter name table order", t_en, 0, 4 }, { "command list name table index 4", t_cn, 4, 4 },
			{ "command list name table order", t_cn + 4, 1, 4 }, { "conversation name table index 2", t_vn, 2, 4 }, { "conversation name table repeats", t_vn + 4, 1, 4 },
			{ "header size", 8, 1, 4 } };
		for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
			memcpy(copy, raw, rawn);
			if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
			else if (hit[k].bytes == 2) wr16(copy + hit[k].at, hit[k].val);
			else copy[hit[k].at] = (uint8_t)hit[k].val;
			if (aidef_open(&bad, copy, rawn) == AIDEF_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
		}
		/* words that are not floats may hold any bit pattern (flags, enums, raw u16 pairs): still accepted */
		{
			const struct { const char *what; size_t at; } ok[] = { { "actor flags", t_act }, { "actor more flags", t_act + 4 }, { "actor raw pair", t_act + 0x354 },
				{ "actor raw enum", t_act + 0x2F8 }, { "variant flags", t_var }, { "variant garbage word", t_var + 0x18C }, { "variant raw pair", t_var + 0x1D0 },
				{ "variant ammo pair", t_var + 0x1E0 }, { "encounter flags", t_enc + 32 }, { "squad flags", t_sq + 40 }, { "squad masks", t_sq + 52 } };
			for (size_t k = 0; k < sizeof ok / sizeof ok[0]; k++) {
				memcpy(copy, raw, rawn);
				wr32(copy + ok[k].at, 0xFFFFFFFFu);
				if (aidef_open(&bad, copy, rawn) != AIDEF_OK) { fprintf(stderr, "refused: %s\n", ok[k].what); return 1; }
				walk(&bad);
			}
		}
	}

	/* no single-byte change anywhere may let an out-of-bounds read through (both sections) */
	for (at = 4; at < rawn; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (aidef_open(&bad, copy, rawn) == AIDEF_OK) walk(&bad);
		}
	for (cut = 0; cut < rawn1; cut++) CHECK(aidef_open(&bad, raw1, cut) != AIDEF_OK);
	for (at = 4; at < rawn1; at++) {
		uint8_t *c1 = malloc(rawn1);
		CHECK(c1);
		memcpy(c1, raw1, rawn1);
		c1[at] ^= 0xFF;
		if (aidef_open(&bad, c1, rawn1) == AIDEF_OK) walk(&bad);
		free(c1);
	}
	free(copy);
	free(raw);
	free(raw1);
	free(img);
	for (a = 2; a < argc; a++)
		if (check_real(argv[a])) return 1;
	puts("ok");
	return 0;
}
