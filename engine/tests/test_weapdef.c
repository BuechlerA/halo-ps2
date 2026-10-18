/* Host test of the WEAP reader and the weapon state machine (sim/weapdef.c).
 *
 *   test_weapdef <fixture.hp2> [-t] [real.weap.hp2 ...]
 *
 * 1. The fixture is written by tools/tests/test_weapconv.py (fixture_set(): a rifle, a charging battery weapon with
 *    two triggers and a shotgun with a shell-by-shell reload). Every accessor is checked against its numbers, then
 *    the state machine is driven through ready, a burst, a full reload, an empty magazine, melee, put away, heat,
 *    charge and zoom with exact expectations. Then the section is truncated at every length and every byte is
 *    corrupted: it is rejected or safe to walk completely and to simulate (ASan / UBSan).
 * 2. Every further argument is a real pack (assets/hp2/<map>.weap.hp2): EVERY weapon in it is run through ready, a
 *    burst per trigger (rate against the tag), a full and a partial reload, an empty magazine, charge, heat, melee,
 *    put away and zoom with ammunition conservation checked on every tick. -t prints the table of the docs. */
#include "stream/hp2.h"
#include "sim/weapdef.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define CHECKF(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static volatile uint32_t sink;
#define WALK_FAIL() do { fprintf(stderr, "FAIL %s:%d: walk found an inconsistency\n", __FILE__, __LINE__); abort(); } while (0)

/* ---- loading ---- */
static uint8_t *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	buf = malloc(*n);
	if (!buf || fread(buf, 1, *n, f) != *n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	return buf;
}

static int load_weap(const char *path, uint8_t **raw, size_t *rawn)
{
	size_t n;
	uint8_t *img = read_file(path, &n);
	static hp2_pack pk;
	const hp2_section *sec;
	CHECKF(img, "cannot read %s", path);
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	sec = hp2_find(&pk, "WEAP");
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(&pk, sec, *raw, sec->raw_size) == HP2_OK);
	*rawn = sec->raw_size;
	return 0;
}

/* ---- simulation helpers ---- */
static weap_input none_in;                       /* all zero */

static weap_events step(weap_state *st, const weap_weapon *w, const weap_input *in, uint32_t *flags_acc)
{
	weap_events ev;
	weapon_fire_step(st, w, in, &ev);
	if (flags_acc) *flags_acc |= ev.flags;
	return ev;
}

static int conserved(const weap_state *st, const weap_weapon *w, unsigned initial)
{
	unsigned m, sum = 0;
	for (m = 0; m < w->magazine_count; m++) sum += st->loaded[m] + st->reserve[m];
	return sum + st->wasted + st->rounds_fired == initial;
}
static unsigned total_rounds(const weap_state *st, const weap_weapon *w)
{
	unsigned m, sum = 0;
	for (m = 0; m < w->magazine_count; m++) sum += st->loaded[m] + st->reserve[m];
	return sum;
}

/* a ready weapon (steps through the ready animation, checking READY at exactly weap_ready_ticks and no shot before) */
static int make_ready(weap_state *st, const weap_weapon *w, int hold_triggers)
{
	weap_input in;
	unsigned i, t, n = weap_ready_ticks(w);
	memset(&in, 0, sizeof in);
	if (hold_triggers) for (t = 0; t < WEAP_MAX_TRIGGERS; t++) in.fire[t] = 1;
	weapon_state_init(st, w);
	CHECK(st->mode == WEAP_MODE_READYING && st->timer == n);
	for (i = 1; i <= n; i++) {
		weap_events ev = step(st, w, &in, NULL);
		for (t = 0; t < WEAP_MAX_TRIGGERS; t++) CHECKF(i == n || !ev.shot[t], "%s shot while readying", w->name);
		CHECKF(((ev.flags & WEAP_EV_READY) != 0) == (i == n), "%s READY at %u of %u", w->name, i, n);
	}
	CHECK(st->mode == WEAP_MODE_IDLE || st->mode == WEAP_MODE_RELOADING || st->mode == WEAP_MODE_OVERHEATED);
	return 0;
}

static unsigned firing_triggers(const weap_weapon *w, unsigned *list)
{
	unsigned t, n = 0;
	for (t = 0; t < w->trigger_count; t++)
		if (w->trigger[t].projectile || w->trigger[t].fx_count) list[n++] = t;
	return n;
}

/* ---- accessors on the fixture ---- */
#define W_RIFLE 0xE1760010u
#define W_PLASMA 0xE1770011u
#define W_SHOTGUN 0xE1780012u
#define P_BULLET 0xE1790020u
#define P_BOLT 0xE17A0021u
#define P_PELLET 0xE17B0022u
#define D_BULLET 0xE17C0030u
#define D_MELEE 0xE17D0031u
#define D_EXPLOSION 0xE17E0032u
#define Q_AMMO 0xE17F0040u
#define Q_GRENADE 0xE1800041u
#define C_BOX 0xE1810050u

static int check_fixture_data(const weap_section *s)
{
	weap_weapon w, p, g;
	weap_projectile pr;
	weap_material_response mr;
	weap_damage d;
	weap_equipment q;
	weap_grenade gr;
	weap_fx fx;
	uint32_t spawn, count, item;
	float weight;

	CHECK(s->n_weapons == 3 && s->n_magazines == 2 && s->n_triggers == 4 && s->n_fx == 1 && s->n_projectiles == 3);
	CHECK(s->n_materials == 3 && s->n_damages == 3 && s->n_equipment == 2 && s->n_grenades == 2 &&
	      s->n_collections == 1 && s->n_permutations == 2);
	CHECK(weap_find_weapon(s, W_RIFLE) == 0 && weap_find_weapon(s, W_PLASMA) == 1 && weap_find_weapon(s, W_SHOTGUN) == 2);
	CHECK(weap_find_weapon(s, 0) == -1 && weap_find_weapon(s, W_RIFLE + 1) == -1 && weap_find_weapon(s, 0xFFFFFFFFu) == -1);
	CHECK(weap_find_projectile(s, P_PELLET) == 2 && weap_find_damage(s, D_EXPLOSION) == 2 && weap_find_equipment(s, Q_GRENADE) == 1);
	CHECK(weap_find_collection(s, C_BOX) == 0 && weap_find_collection(s, 5) == -1);

	CHECK(weap_get_weapon(s, 0, &w));
	CHECK(w.tag_id == W_RIFLE && w.model_tag == 0xE1900001u && w.fp_model_tag == 0xE1900002u && w.fp_anim_tag == 0xE1910001u);
	CHECK(w.hud_tag == 0xE1920001u && w.pickup_sound == 0xE1930001u && w.melee_damage == D_MELEE);
	CHECK(strcmp(w.label, "tr") == 0 && strcmp(w.name, "rifle") == 0 && w.flags == 0 && w.hud_message == 7 && w.sort_order == 1);
	CHECK(near(w.autoaim_angle, 0.1f, 1e-6f) && near(w.autoaim_range, 25.0f, 1e-6f) && near(w.item_scale, 1.5f, 1e-6f));
	CHECK(near(w.near_reticle, 4.0f, 0) && near(w.far_reticle, 16.0f, 0) && near(w.intersection_reticle, 80.0f, 0));
	CHECK(w.fp_ticks[WEAP_FP_READY] == 20 && w.fp_ticks[WEAP_FP_PUT_AWAY] == 8 && w.fp_ticks[WEAP_FP_RELOAD_EMPTY] == 50 &&
	      w.fp_ticks[WEAP_FP_RELOAD_FULL] == 45 && w.fp_ticks[WEAP_FP_MELEE] == 18 && w.fp_ticks[WEAP_FP_THROW] == 0);
	CHECK(w.magazine_count == 1 && w.trigger_count == 1);
	CHECK(w.magazine[0].rounds_total_initial == 90 && w.magazine[0].rounds_total_max == 300 && w.magazine[0].rounds_loaded_max == 30 &&
	      w.magazine[0].rounds_reloaded == 30 && near(w.magazine[0].reload_time, 2.0f, 0) && w.magazine[0].equipment_count == 1 &&
	      w.magazine[0].equipment_tag[0] == Q_AMMO && w.magazine[0].equipment_rounds[0] == 60);
	CHECK(near(w.trigger[0].rps_min, 10.0f, 0) && near(w.trigger[0].rps_max, 10.0f, 0) && w.trigger[0].magazine == 0 &&
	      w.trigger[0].rounds_per_shot == 1 && w.trigger[0].tracer_interval == 3 && w.trigger[0].firing_noise == 2 &&
	      w.trigger[0].projectile == P_BULLET && w.trigger[0].fx_count == 1 && w.trigger[0].first_fx == 0);
	CHECK(near(w.trigger[0].error_min, 0.02f, 1e-6f) && near(w.trigger[0].error_max, 0.06f, 1e-6f) &&
	      near(w.trigger[0].minimum_error, 0.01f, 1e-6f) && near(w.trigger[0].error_accel_time, 1.0f, 0) && near(w.trigger[0].error_decel_time, 0.5f, 0));
	CHECK(weap_get_fx(s, &w.trigger[0], 0, &fx));
	CHECK(fx.shots_lo == 1 && fx.shots_hi == 1 && fx.effects[0] == 0xE1A00001u && fx.effects[1] == 0 && fx.effects[2] == 0xE1930002u &&
	      fx.sound_mask == 4 && fx.damages[0] == D_BULLET && fx.damages[1] == 0);
	CHECK(!weap_get_fx(s, &w.trigger[0], 1, &fx) && fx.effects[0] == 0);

	CHECK(weap_get_weapon(s, 1, &p));
	CHECK(p.flags == 0x800 && strcmp(p.label, "pp") == 0 && p.weapon_type == 3 && p.zoom_levels == 2 && near(p.zoom_min, 2, 0) && near(p.zoom_max, 8, 0));
	CHECK(near(p.heat_recovery, 0.25f, 0) && near(p.overheated, 1.0f, 0) && near(p.heat_loss, 0.5f, 0) && p.magazine_count == 0 && p.trigger_count == 2);
	CHECK(near(p.trigger[0].charging_time, 0.5f, 0) && p.trigger[0].magazine == WEAP_NO_MAGAZINE && p.trigger[0].projectile == P_BOLT);
	CHECK(near(p.trigger[1].rps_min, 4, 0) && near(p.trigger[1].rps_max, 8, 0) && near(p.trigger[1].accel_time, 1, 0) && near(p.trigger[1].heat_per_round, 0.25f, 0));
	CHECK(weap_get_weapon(s, 2, &g));
	CHECK(strcmp(g.label, "sg") == 0 && g.weapon_type == 1 && g.magazine[0].flags == WEAP_MAG_CHAMBER_EACH && g.magazine[0].rounds_reloaded == 1 &&
	      near(g.magazine[0].reload_time, 0.4f, 1e-6f) && near(g.magazine[0].chamber_time, 0.2f, 1e-6f) &&
	      g.trigger[0].flags == WEAP_T_NO_REPEAT && g.trigger[0].projectiles_per_shot == 12 && g.trigger[0].projectile == P_PELLET);
	CHECK(!weap_get_weapon(s, 3, &g) && g.tag_id == 0 && g.magazine_count == 0);

	CHECK(weap_get_projectile(s, 0, &pr));
	CHECK(pr.tag_id == P_BULLET && pr.flags == WEAP_P_ORIENTED && near(pr.initial_velocity, 10, 0) && near(pr.final_velocity, 10, 0) &&
	      near(pr.air_gravity, 1.0f, 0) && near(pr.air_range[0], 5, 0) && near(pr.air_range[1], 15, 0) && near(pr.water_gravity, 0.4f, 1e-6f) &&
	      near(pr.max_range, 40, 0) && pr.impact_damage == D_BULLET && near(pr.object_radius, 0.1f, 1e-6f) && strcmp(pr.name, "bullet") == 0);
	CHECK(pr.first_material == 0 && pr.material_count == 3);
	CHECK(weap_get_material(s, &pr, 0, &mr) && mr.response == WEAP_RESP_DETONATE && mr.effect == 0xE1A00002u);
	CHECK(weap_get_material(s, &pr, 1, &mr) && mr.response == WEAP_RESP_REFLECT);
	CHECK(weap_get_material(s, &pr, 2, &mr) && mr.response == WEAP_RESP_OVERPENETRATE && mr.flags == 1 && near(mr.angle[1], 3.14159f, 1e-5f) &&
	      near(mr.initial_friction, 0.7f, 1e-6f) && near(mr.max_distance, 100, 0) && mr.effect == 0xE1A00003u);
	CHECK(!weap_get_material(s, &pr, 3, &mr) && mr.response == 0);
	{
		weap_projectile bolt;
		CHECK(weap_get_projectile(s, 1, &bolt));
		CHECK(bolt.tag_id == P_BOLT && bolt.first_material == 3 && bolt.material_count == 0 && !weap_get_material(s, &bolt, 0, &mr));
		CHECK(near(bolt.guided_angular_velocity, 1.5f, 0) && bolt.detonation_effect == 0xE1A00004u && bolt.impact_damage == D_EXPLOSION &&
		      near(bolt.arming_time, 0.5f, 0) && near(bolt.timer[0], 1, 0) && near(bolt.timer[1], 2, 0));
		CHECK(near(weap_projectile_speed(&bolt, 0, 30), 1.5f, 1e-6f) && near(weap_projectile_speed(&bolt, 15, 30), 1.125f, 1e-6f) &&
		      near(weap_projectile_speed(&bolt, 30, 30), 0.75f, 1e-6f) && near(weap_projectile_speed(&bolt, 99, 30), 0.75f, 1e-6f) &&
		      near(weap_projectile_speed(&bolt, 5, 0), 0.75f, 1e-6f));
	}

	CHECK(weap_get_damage(s, 0, &d));
	CHECK(d.tag_id == D_BULLET && d.category == WEAP_CAT_BULLET && near(d.damage_lower, 20, 0) && near(d.damage_upper[0], 25, 0) &&
	      d.damage_flags == 2 && near(d.flash_color[1], 1.0f, 1e-6f) && near(d.flash_color[2], 0.5f, 1e-6f) && near(d.low_rumble[0], 0.5f, 0) &&
	      near(d.low_rumble[1], 0.25f, 0) && d.low_rumble_fade == 4 && near(d.shake_duration, 0.05f, 1e-6f) && near(d.shake_rotation, 0.01f, 1e-6f) &&
	      near(d.impulse_rotation, 0.03f, 1e-6f) && near(d.modifiers[7], 0.7f, 1e-6f) && near(d.modifiers[32], 3.2f, 1e-5f) && strcmp(d.name, "bullet") == 0);
	CHECK(weap_get_damage(s, 1, &d) && d.side_effect == 2 && d.category == WEAP_CAT_MELEE && near(d.radius[0], 0.5f, 0) && near(d.damage_upper[1], 60, 0));
	CHECK(weap_get_damage(s, 2, &d) && d.category == WEAP_CAT_GRENADE && near(d.aoe_core_radius, 0.4f, 1e-6f) && near(d.break_forward[0], 15, 0) && d.damage_flags == 0x20);
	CHECK(!weap_get_damage(s, 3, &d) && d.tag_id == 0);

	/* damage helpers: bullet 25 up to 5 wu, 20 from 15 wu on, linear between; explosion falls from its radius */
	{
		weap_damage bd, ex;
		CHECK(weap_get_damage(s, 0, &bd) && weap_get_damage(s, 2, &ex));
		CHECK(near(weap_damage_at_range(&pr, &bd, 1, 0.5f), 25.0f, 1e-4f) && near(weap_damage_at_range(&pr, &bd, 10, 0), 22.5f, 1e-4f) &&
		      near(weap_damage_at_range(&pr, &bd, 15, 0), 20.0f, 1e-4f) && near(weap_damage_at_range(&pr, &bd, 99, 0), 20.0f, 1e-4f));
		CHECK(near(weap_damage_at_radius(&ex, 0.0f, 0.0f), 100.0f, 1e-3f) && near(weap_damage_at_radius(&ex, 1.5f, 1.0f), 120.0f, 1e-3f) &&
		      near(weap_damage_at_radius(&ex, 2.0f, 0.0f), 90.0f, 1e-3f) && near(weap_damage_at_radius(&ex, 2.5f, 0.0f), 0.0f, 0) &&
		      near(weap_damage_at_radius(&ex, 9.0f, 0.0f), 0.0f, 0) && near(weap_damage_at_radius(&bd, 3.0f, 0.0f), 25.0f, 1e-4f));
	}

	CHECK(weap_get_equipment(s, 0, &q) && q.tag_id == Q_AMMO && q.hud_message == 9 && near(q.item_scale, 1.0f, 0) && strcmp(q.name, "ammo") == 0);
	CHECK(weap_get_equipment(s, 1, &q) && q.powerup_type == WEAP_POWERUP_GRENADE && q.grenade_type == WEAP_GRENADE_PLASMA &&
	      near(q.detonation_delay[0], 0.4f, 1e-6f) && near(q.detonation_delay[1], 0.8f, 1e-6f));
	CHECK(!weap_get_equipment(s, 2, &q) && q.tag_id == 0);
	CHECK(weap_get_grenade(s, 0, &gr) && gr.max_count == 4 && gr.throw_effect == 0xE1A00005u && gr.hud_tag == 0xE1B00001u && gr.equipment == Q_GRENADE && gr.projectile == P_BOLT);
	CHECK(weap_get_grenade(s, 1, &gr) && gr.max_count == 3 && gr.mp_spawn_default == 1 && gr.equipment == Q_AMMO && gr.projectile == P_PELLET);
	CHECK(!weap_get_grenade(s, 2, &gr) && gr.max_count == 0);
	CHECK(weap_collection(s, 0, &spawn, &count) && spawn == 5 && count == 2);
	CHECK(weap_collection_entry(s, 0, 0, &weight, &item) && near(weight, 2.0f, 0) && item == W_RIFLE);
	CHECK(weap_collection_entry(s, 0, 1, &weight, &item) && near(weight, 1.0f, 0) && item == Q_AMMO);
	CHECK(!weap_collection_entry(s, 0, 2, &weight, &item) && item == 0 && !weap_collection_entry(s, 1, 0, &weight, &item));
	CHECK(!weap_collection(s, 1, &spawn, &count) && count == 0);
	return 0;
}

/* ---- state machine on the fixture ---- */
static int check_rifle(const weap_section *s)
{
	weap_weapon w;
	weap_state st;
	weap_input in;
	weap_events ev;
	unsigned i, shots = 0, tracers = 0, shot_tick[64];
	float last_error = 0.0f;
	uint32_t flags = 0;

	CHECK(weap_get_weapon(s, 0, &w));
	CHECK(weap_ready_ticks(&w) == 20 && weap_put_away_ticks(&w) == 8 && weap_melee_ticks(&w) == 18 && weap_reload_ticks(&w, 0, 1) == 60);
	CHECK(near(weap_shot_interval(&w, 0, 10.0f), 3.0f, 1e-6f) && near(weap_shot_interval(&w, 0, 0.0f), 0.0f, 0) && weap_shot_interval(&w, 5, 1.0f) == 0.0f);
	CHECK(!make_ready(&st, &w, 1));
	CHECK(st.reserve[0] == 90 && st.mode == WEAP_MODE_IDLE);
	/* the trigger was held through the whole ready animation: the first shot is on the READY tick itself */
	CHECK(st.shots == 1 && st.loaded[0] == 29 && st.rounds_fired == 1);

	/* burst: held for 30 ticks from the READY tick (already counted one) -> 10 shots, 3 ticks apart */
	weapon_state_init(&st, &w);
	memset(&in, 0, sizeof in);
	for (i = 0; i < 20; i++) step(&st, &w, &none_in, NULL);
	CHECK(st.mode == WEAP_MODE_IDLE);
	in.fire[0] = 1;
	for (i = 0; i < 30; i++) {
		ev = step(&st, &w, &in, &flags);
		if (ev.shot[0]) {
			CHECK(shots < 64 && ev.projectiles[0] == 1 && ev.rounds_used == 1 && near(ev.charge[0], 1.0f, 0));
			CHECK(ev.error[0] >= 0.02f - 1e-6f && ev.error[0] <= 0.06f + 1e-6f && ev.error[0] >= last_error - 1e-6f);   /* spread blooms while held */
			last_error = ev.error[0];
			shot_tick[shots++] = i;
			tracers += ev.tracer[0];
		}
	}
	CHECK(shots == 10 && tracers == 3 && st.loaded[0] == 20 && st.reserve[0] == 90 && !(flags & WEAP_EV_DRY_FIRE));
	for (i = 1; i < shots; i++) CHECK(shot_tick[i] - shot_tick[i - 1] == 3);
	CHECK(shot_tick[0] == 0 && conserved(&st, &w, 120));
	CHECK(st.error[0] > 0.9f);                                   /* error accel 1 s: nearly fully bloomed after 1 s */
	in.fire[0] = 0;
	for (i = 0; i < 20; i++) step(&st, &w, &in, NULL);
	CHECK(st.error[0] < 0.01f);                                  /* 0.5 s to relax */

	/* full reload: 60 ticks (reload time 2 s), 30 rounds back */
	in.reload = 1;
	ev = step(&st, &w, &in, NULL);
	in.reload = 0;
	CHECK((ev.flags & WEAP_EV_RELOAD_START) && st.mode == WEAP_MODE_RELOADING && st.timer == 60 && !(ev.flags & WEAP_EV_AUTO_RELOAD));
	for (i = 1; i < 60; i++) {
		ev = step(&st, &w, &in, NULL);
		CHECK(st.mode == WEAP_MODE_RELOADING && !(ev.flags & WEAP_EV_RELOAD_DONE));
	}
	ev = step(&st, &w, &in, NULL);
	CHECK((ev.flags & WEAP_EV_RELOAD_DONE) && st.mode == WEAP_MODE_IDLE && st.loaded[0] == 30 && st.reserve[0] == 80 && conserved(&st, &w, 120));
	/* a reload of a full magazine does nothing */
	in.reload = 1;
	ev = step(&st, &w, &in, NULL);
	in.reload = 0;
	CHECK(!(ev.flags & WEAP_EV_RELOAD_START) && st.mode == WEAP_MODE_IDLE);

	/* empty magazine with reserve: pulling the trigger clicks and starts an automatic reload, no shot */
	weapon_state_set_ammo(&st, &w, 0, 0, 5);
	st.rounds_fired = 0; st.wasted = 0;
	in.fire[0] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK((ev.flags & WEAP_EV_DRY_FIRE) && (ev.flags & WEAP_EV_RELOAD_START) && (ev.flags & WEAP_EV_AUTO_RELOAD) && !ev.shot[0]);
	CHECK(st.mode == WEAP_MODE_RELOADING && st.timer == 60);                            /* reload_time 2 s = 60 ticks */
	for (i = 0; i < 60; i++) ev = step(&st, &w, &in, NULL);
	CHECK(st.reserve[0] == 0 && st.loaded[0] == 4 && ev.shot[0]);                       /* 5 rounds in, the held trigger fired one on the same tick */
	/* empty magazine, no reserve: dry fire once per pull (not every tick) */
	weapon_state_set_ammo(&st, &w, 0, 0, 0);
	st.mode = WEAP_MODE_IDLE; st.prev_fire = 0; st.cooldown[0] = 0;
	in.fire[0] = 0;
	step(&st, &w, &in, NULL);
	in.fire[0] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK((ev.flags & WEAP_EV_DRY_FIRE) && !ev.shot[0] && st.mode == WEAP_MODE_IDLE);
	for (i = 0; i < 10; i++) { ev = step(&st, &w, &in, NULL); CHECK(!(ev.flags & WEAP_EV_DRY_FIRE) && !ev.shot[0]); }
	in.fire[0] = 0;
	step(&st, &w, &in, NULL);
	in.fire[0] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK(ev.flags & WEAP_EV_DRY_FIRE);

	/* last round: the weapon starts reloading by itself right after the shot */
	weapon_state_init(&st, &w);
	for (i = 0; i < 20; i++) step(&st, &w, &none_in, NULL);
	weapon_state_set_ammo(&st, &w, 0, 1, 40);
	in.fire[0] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK(ev.shot[0] && (ev.flags & WEAP_EV_RELOAD_START) && (ev.flags & WEAP_EV_AUTO_RELOAD) && st.mode == WEAP_MODE_RELOADING && st.loaded[0] == 0);
	in.fire[0] = 0;

	/* melee: 18 ticks, the hit lands half way, no shooting meanwhile */
	weapon_state_init(&st, &w);
	for (i = 0; i < 20; i++) step(&st, &w, &none_in, NULL);
	memset(&in, 0, sizeof in);
	in.melee = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK((ev.flags & WEAP_EV_MELEE_START) && st.mode == WEAP_MODE_MELEE && st.timer == 18);
	in.melee = 0; in.fire[0] = 1;
	flags = 0;
	for (i = 1; i <= 18; i++) {
		ev = step(&st, &w, &in, &flags);
		CHECK(!ev.shot[0] || i == 18);
	}
	CHECK((flags & WEAP_EV_MELEE_HIT) && st.mode == WEAP_MODE_IDLE && st.melee_hit_at == 0);

	/* put away: 8 ticks, then nothing happens any more */
	memset(&in, 0, sizeof in);
	weapon_state_init(&st, &w);
	for (i = 0; i < 20; i++) step(&st, &w, &none_in, NULL);
	in.switch_out = 1;
	ev = step(&st, &w, &in, NULL);
	in.switch_out = 0; in.fire[0] = 1;
	CHECK(st.mode == WEAP_MODE_PUTTING_AWAY && st.timer == 8);
	for (i = 1; i < 8; i++) { ev = step(&st, &w, &in, NULL); CHECK(!ev.shot[0] && !(ev.flags & WEAP_EV_PUT_AWAY)); }
	ev = step(&st, &w, &in, NULL);
	CHECK((ev.flags & WEAP_EV_PUT_AWAY) && st.mode == WEAP_MODE_STOWED);
	for (i = 0; i < 20; i++) { ev = step(&st, &w, &in, NULL); CHECK(ev.flags == 0 && !ev.shot[0]); }
	return 0;
}

static int check_plasma(const weap_section *s)
{
	weap_weapon w;
	weap_state st;
	weap_input in;
	weap_events ev;
	unsigned i, shots = 0, ticks_to_overheat = 0, cooled_at = 0;
	uint32_t flags;

	CHECK(weap_get_weapon(s, 1, &w));
	CHECK(weap_ready_ticks(&w) == WEAP_DEFAULT_READY_TICKS && weap_put_away_ticks(&w) == WEAP_DEFAULT_PUT_AWAY_TICKS && weap_melee_ticks(&w) == WEAP_DEFAULT_MELEE_TICKS);
	CHECK(!make_ready(&st, &w, 0));

	/* charge: 0.5 s = 15 ticks, CHARGED on tick 15, released: a full-charge shot; a tap: a partial one */
	memset(&in, 0, sizeof in);
	in.fire[0] = 1;
	flags = 0;
	for (i = 1; i <= 14; i++) { ev = step(&st, &w, &in, &flags); CHECK(!ev.shot[0] && !(ev.flags & WEAP_EV_CHARGED)); }
	CHECK(flags & WEAP_EV_CHARGING);
	ev = step(&st, &w, &in, NULL);
	CHECK((ev.flags & WEAP_EV_CHARGED) && !ev.shot[0]);
	for (i = 0; i < 10; i++) { ev = step(&st, &w, &in, NULL); CHECK(!ev.shot[0] && !(ev.flags & WEAP_EV_CHARGED)); }
	in.fire[0] = 0;
	ev = step(&st, &w, &in, NULL);
	CHECK(ev.shot[0] && near(ev.charge[0], 1.0f, 0) && ev.projectiles[0] == 1 && ev.rounds_used == 0);
	CHECK(near(st.heat, 0.25f, 0.02f) && near(st.age, 0.01f, 1e-6f));
	for (i = 0; i < 60; i++) step(&st, &w, &none_in, NULL);
	in.fire[0] = 1;
	for (i = 0; i < 5; i++) step(&st, &w, &in, NULL);
	in.fire[0] = 0;
	ev = step(&st, &w, &in, NULL);
	CHECK(ev.shot[0] && near(ev.charge[0], 5.0f / 15.0f, 0.01f));

	/* trigger 1: rate ramps from 4 to 8 per second over a second; heat 0.25 per shot, loss 0.5 per second:
	   it overheats, ignores the trigger while hot, cools to the recovery threshold and works again */
	weapon_state_init(&st, &w);
	for (i = 0; i < 15; i++) step(&st, &w, &none_in, NULL);
	memset(&in, 0, sizeof in);
	in.fire[1] = 1;
	flags = 0;
	for (i = 1; i <= 200 && st.mode != WEAP_MODE_OVERHEATED; i++) {
		ev = step(&st, &w, &in, &flags);
		shots += ev.shot[1];
		ticks_to_overheat = i;
	}
	CHECK(st.mode == WEAP_MODE_OVERHEATED && (flags & WEAP_EV_OVERHEATED) && shots >= 4 && shots <= 6 && ticks_to_overheat < 100);
	for (i = 0; i < 300; i++) {
		unsigned was_hot = st.mode == WEAP_MODE_OVERHEATED;
		ev = step(&st, &w, &in, &flags);
		CHECK(!(was_hot && ev.shot[1] && !(ev.flags & WEAP_EV_COOLED)));
		if ((ev.flags & WEAP_EV_COOLED) && !cooled_at) cooled_at = i + 1;
	}
	CHECK(cooled_at > 10 && cooled_at < 200 && (flags & WEAP_EV_COOLED));
	in.fire[1] = 0;
	for (i = 0; i < 100; i++) step(&st, &w, &in, NULL);
	CHECK(st.mode == WEAP_MODE_IDLE && st.heat < 0.001f && st.age > 0.0f && st.age <= 1.0f);

	/* battery: at age 1 the weapon cannot fire (flag "cannot fire at maximum age") */
	st.age = 0.995f;
	in.fire[1] = 1;
	shots = 0;
	for (i = 0; i < 30; i++) { ev = step(&st, &w, &in, &flags); shots += ev.shot[1]; if (st.mode == WEAP_MODE_OVERHEATED) break; }
	in.fire[1] = 0;
	for (i = 0; i < 120; i++) step(&st, &w, &in, NULL);
	st.age = 1.0f;
	in.fire[1] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK(!ev.shot[1] && (ev.flags & WEAP_EV_DRY_FIRE) && (ev.flags & WEAP_EV_BATTERY_DEAD));

	/* zoom: 2 levels, the third press returns to the plain view */
	memset(&in, 0, sizeof in);
	st.zoom = 0;
	in.zoom = 1;
	for (i = 1; i <= 3; i++) { ev = step(&st, &w, &in, NULL); CHECK((ev.flags & WEAP_EV_ZOOM) && st.zoom == i % 3); }
	return 0;
}

static int check_shotgun(const weap_section *s)
{
	weap_weapon w;
	weap_state st;
	weap_input in;
	weap_events ev;
	unsigned i, shots = 0, dones = 0, cycles_ticks = 0;
	uint32_t flags;

	CHECK(weap_get_weapon(s, 2, &w));
	CHECK(weap_reload_ticks(&w, 0, 1) == 12 && near(weap_shot_interval(&w, 0, 2.0f), 15.0f, 1e-6f));
	CHECK(!make_ready(&st, &w, 0));
	CHECK(st.loaded[0] == 6 && st.reserve[0] == 24);
	/* one shot per pull, 12 pellets, a held trigger does nothing more */
	memset(&in, 0, sizeof in);
	in.fire[0] = 1;
	for (i = 0; i < 60; i++) {
		ev = step(&st, &w, &in, NULL);
		if (ev.shot[0]) { CHECK(ev.projectiles[0] == 12 && i == 0); shots++; }
	}
	CHECK(shots == 1 && st.loaded[0] == 5);
	/* releasing and pulling again before the interval has passed does not fire; after it does */
	in.fire[0] = 0;
	step(&st, &w, &in, NULL);
	in.fire[0] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK(ev.shot[0] || st.cooldown[0] > 0.0f);
	/* shell by shell: from 1 loaded, 5 more shells at 12 ticks each, RELOAD_DONE after each */
	weapon_state_init(&st, &w);
	for (i = 0; i < 15; i++) step(&st, &w, &none_in, NULL);
	weapon_state_set_ammo(&st, &w, 0, 1, 24);
	st.rounds_fired = 0; st.wasted = 0;
	memset(&in, 0, sizeof in);
	in.reload = 1;
	flags = 0;
	step(&st, &w, &in, &flags);
	in.reload = 0;
	CHECK(st.mode == WEAP_MODE_RELOADING);
	cycles_ticks = 1;
	while (st.mode == WEAP_MODE_RELOADING && cycles_ticks < 500) {
		ev = step(&st, &w, &in, NULL);
		cycles_ticks++;
		dones += (ev.flags & WEAP_EV_RELOAD_DONE) != 0;
	}
	CHECK(st.loaded[0] == 6 && st.reserve[0] == 24 - 5 && dones == 5 && cycles_ticks == 1 + 5 * 12);
	CHECK(conserved(&st, &w, 25));
	/* firing interrupts it after two shells */
	weapon_state_set_ammo(&st, &w, 0, 1, 24);
	st.rounds_fired = 0; st.wasted = 0; st.cooldown[0] = 0; st.prev_fire = 0;
	in.reload = 1;
	step(&st, &w, &in, NULL);
	in.reload = 0;
	for (i = 0; i < 24; i++) step(&st, &w, &in, NULL);
	CHECK(st.loaded[0] == 3);
	in.fire[0] = 1;
	flags = 0;
	ev = step(&st, &w, &in, &flags);
	CHECK((flags & WEAP_EV_RELOAD_CANCEL) && st.mode == WEAP_MODE_IDLE && !ev.shot[0]);       /* the chamber is still working */
	in.fire[0] = 0;                                              /* one shot per pull: release, wait out the chamber, pull again */
	for (i = 0; i < 5; i++) step(&st, &w, &in, NULL);
	in.fire[0] = 1;
	ev = step(&st, &w, &in, NULL);
	CHECK(ev.shot[0] && st.loaded[0] == 2 && conserved(&st, &w, 25));
	return 0;
}

/* ---- hostile input ---- */
static void walk(const weap_section *s)
{
	uint32_t acc = 0, i, k, m;
	for (i = 0; i < s->n_weapons; i++) {
		weap_weapon w;
		weap_state st;
		weap_input in;
		weap_events ev;
		uint32_t seed = i * 2654435761u + 7u;
		unsigned t;
		if (!weap_get_weapon(s, i, &w)) WALK_FAIL();
		acc += w.tag_id + (uint32_t)strlen(w.name) + (uint32_t)strlen(w.label);
		if (weap_find_weapon(s, w.tag_id) < 0) WALK_FAIL();
		for (t = 0; t < w.trigger_count; t++)
			for (k = 0; k < w.trigger[t].fx_count; k++) {
				weap_fx fx;
				if (!weap_get_fx(s, &w.trigger[t], k, &fx)) WALK_FAIL();
				acc += fx.effects[0] + fx.damages[0];
			}
		weapon_state_init(&st, &w);
		memset(&in, 0, sizeof in);
		for (k = 0; k < 400; k++) {
			seed = seed * 1664525u + 1013904223u;
			for (t = 0; t < WEAP_MAX_TRIGGERS; t++) in.fire[t] = (seed >> (8 + t)) & 1u;
			in.reload = ((seed >> 14) & 31u) == 0;
			in.melee = ((seed >> 19) & 63u) == 0;
			in.zoom = ((seed >> 21) & 63u) == 0;
			in.switch_out = ((seed >> 25) & 255u) == 0;
			weapon_fire_step(&st, &w, &in, &ev);
			for (m = 0; m < w.magazine_count; m++)
				if (st.loaded[m] > w.magazine[m].rounds_loaded_max) WALK_FAIL();
			if (!(st.heat >= 0.0f) || !(st.age >= 0.0f && st.age <= 1.0f)) WALK_FAIL();
			acc += ev.flags + st.timer;
		}
	}
	for (i = 0; i < s->n_projectiles; i++) {
		weap_projectile p;
		if (!weap_get_projectile(s, i, &p) || weap_find_projectile(s, p.tag_id) != (int)i) WALK_FAIL();
		for (k = 0; k < p.material_count; k++) {
			weap_material_response mr;
			if (!weap_get_material(s, &p, k, &mr)) WALK_FAIL();
			acc += mr.effect;
		}
		acc += (uint32_t)weap_projectile_speed(&p, 3, 9);
	}
	for (i = 0; i < s->n_damages; i++) {
		weap_damage d;
		if (!weap_get_damage(s, i, &d) || weap_find_damage(s, d.tag_id) != (int)i) WALK_FAIL();
		acc += (uint32_t)weap_damage_at_radius(&d, 1.0f, 0.5f) + (uint32_t)strlen(d.name);
	}
	for (i = 0; i < s->n_equipment; i++) {
		weap_equipment q;
		if (!weap_get_equipment(s, i, &q) || weap_find_equipment(s, q.tag_id) != (int)i) WALK_FAIL();
		acc += q.tag_id;
	}
	for (i = 0; i < s->n_grenades; i++) {
		weap_grenade g;
		if (!weap_get_grenade(s, i, &g)) WALK_FAIL();
		acc += g.equipment;
	}
	for (i = 0; i < s->n_collections; i++) {
		uint32_t spawn, count, tag;
		float wt;
		if (!weap_collection(s, i, &spawn, &count)) WALK_FAIL();
		for (k = 0; k < count; k++)
			if (!weap_collection_entry(s, i, k, &wt, &tag)) WALK_FAIL();
		acc += spawn + count;
	}
	sink = acc;
}

static int check_corruption(const uint8_t *raw, size_t n)
{
	uint8_t *buf = malloc(n);
	size_t i;
	unsigned v, opened = 0;
	weap_section s;
	static const uint8_t vals[] = { 0x00, 0xFF, 0x7F, 0x80, 0x01 };
	CHECK(buf);
	for (i = 0; i < n; i++) {                                   /* every truncation */
		memcpy(buf, raw, i);
		if (weap_open(&s, buf, i) == WEAP_OK) walk(&s);
	}
	for (i = 0; i < n; i++)                                     /* every byte corrupted */
		for (v = 0; v < sizeof vals; v++) {
			memcpy(buf, raw, n);
			buf[i] = vals[v] == raw[i] ? (uint8_t)(raw[i] ^ 0x55) : vals[v];
			if (weap_open(&s, buf, n) == WEAP_OK) { walk(&s); opened++; }
		}
	CHECK(opened > 0);                                          /* many corruptions are harmless numbers */
	/* targeted layout corruptions are rejected */
	memcpy(buf, raw, n);
	buf[0] = 'X';
	CHECK(weap_open(&s, buf, n) == WEAP_ERR_MAGIC && s.data == NULL);
	memcpy(buf, raw, n); buf[4] = 9;
	CHECK(weap_open(&s, buf, n) == WEAP_ERR_VERSION);
	memcpy(buf, raw, n);
	CHECK(weap_open(&s, buf, 100) == WEAP_ERR_TRUNCATED && weap_open(&s, NULL, 0) == WEAP_ERR_TRUNCATED);
	{
		uint32_t v = (uint32_t)n + 16u;                         /* size field no longer the real size */
		buf[8] = (uint8_t)v; buf[9] = (uint8_t)(v >> 8); buf[10] = (uint8_t)(v >> 16); buf[11] = (uint8_t)(v >> 24);
		CHECK(weap_open(&s, buf, n) == WEAP_ERR_TRUNCATED);
		v = (uint32_t)n - 16u;
		buf[8] = (uint8_t)v; buf[9] = (uint8_t)(v >> 8); buf[10] = (uint8_t)(v >> 16); buf[11] = (uint8_t)(v >> 24);
		CHECK(weap_open(&s, buf, n) == WEAP_ERR_LAYOUT);
	}
	memcpy(buf, raw, n);
	buf[0x38] = 0x10;                                           /* weapons table not aligned to... inside the header */
	CHECK(weap_open(&s, buf, n) == WEAP_ERR_LAYOUT);
	memcpy(buf, raw, n);
	buf[0x0C + 3] = 0x7F;                                       /* absurd weapon count */
	CHECK(weap_open(&s, buf, n) == WEAP_ERR_LAYOUT);
	free(buf);
	return 0;
}

/* ---- every weapon of a real pack ---- */
typedef struct { unsigned weapons, triggers, shots; } totals;

static int check_real_weapon(const weap_section *s, const weap_weapon *w, int table, totals *tot)
{
	weap_state st;
	weap_input in;
	weap_events ev;
	unsigned list[WEAP_MAX_TRIGGERS], nfire = firing_triggers(w, list), k, t, i, m, initial;
	uint32_t flags;

	/* data sanity that the section reader promises */
	CHECKF(w->trigger_count <= WEAP_MAX_TRIGGERS && w->magazine_count <= WEAP_MAX_MAGAZINES, "%s", w->name);
	for (t = 0; t < w->trigger_count; t++) {
		const weap_trigger *tr = &w->trigger[t];
		weap_projectile p;
		int pi;
		if (!tr->projectile) continue;
		pi = weap_find_projectile(s, tr->projectile);
		CHECKF(pi >= 0 && weap_get_projectile(s, (uint32_t)pi, &p), "%s: projectile missing", w->name);
		CHECKF(p.initial_velocity >= 0.0f && p.initial_velocity * 30.0f <= 1500.0f, "%s: speed", w->name);
		CHECKF(p.material_count == 0 || p.material_count >= 30, "%s: materials %u", w->name, p.material_count);
		for (k = 0; k < tr->fx_count; k++) {
			weap_fx fx;
			CHECK(weap_get_fx(s, tr, k, &fx));
			CHECKF(fx.damages[0] == 0 || weap_find_damage(s, fx.damages[0]) >= 0, "%s: firing damage", w->name);
		}
	}

	/* ready */
	CHECK(!make_ready(&st, w, 1));
	CHECKF(weap_ready_ticks(w) >= 1, "%s", w->name);

	/* per trigger: burst and rate; every trigger on a fresh weapon */
	for (k = 0; k < nfire; k++) {
		const weap_trigger *tr;
		unsigned shot_ticks[256], nshots = 0, hold;
		float iv, ivmin, ivmax;
		int once, ammo;
		unsigned need;

		t = list[k];
		tr = &w->trigger[t];
		need = tr->rounds_per_shot ? tr->rounds_per_shot : 1u;
		weapon_state_init(&st, w);
		for (m = 0; m < w->magazine_count; m++)
			weapon_state_set_ammo(&st, w, m, w->magazine[m].rounds_loaded_max, w->magazine[m].rounds_total_max);
		initial = total_rounds(&st, w);
		memset(&in, 0, sizeof in);
		for (i = 0; i < weap_ready_ticks(w); i++) step(&st, w, &in, NULL);
		CHECKF(st.mode == WEAP_MODE_IDLE, "%s", w->name);
		ammo = tr->magazine != WEAP_NO_MAGAZINE && tr->magazine < w->magazine_count && w->magazine[tr->magazine].rounds_loaded_max > 0;

		if (tr->charging_time > 0.0f) {
			unsigned want = (unsigned)(tr->charging_time * 30.0f + 0.5f), charged_at = 0, shot_at = 0;
			in.fire[t] = 1;
			flags = 0;
			for (i = 1; i <= want + 3; i++) {
				ev = step(&st, w, &in, &flags);
				if ((ev.flags & WEAP_EV_CHARGED) && !charged_at) charged_at = i;
				CHECKF(!ev.shot[t] || (tr->charged_time > 0.0f), "%s shot while charging", w->name);
			}
			CHECKF(charged_at >= want && charged_at <= want + 1 && (flags & WEAP_EV_CHARGING), "%s charged at %u want %u", w->name, charged_at, want);
			in.fire[t] = 0;
			ev = step(&st, w, &in, NULL);
			CHECKF(ev.shot[t] && near(ev.charge[t], 1.0f, 0.001f), "%s: no full charge shot", w->name);
			(void)shot_at;
			tot->shots++;
			CHECK(!ammo || conserved(&st, w, initial));
			continue;
		}

		/* repeat rate against the tag */
		once = (tr->flags & WEAP_T_NO_REPEAT) != 0 || (tr->rps_min <= 0.0f && tr->rps_max <= 0.0f &&
		       weap_shot_interval(w, t, 0.0f) <= 0.0f);
		hold = 200;
		in.fire[t] = 1;
		for (i = 0; i < hold; i++) {
			ev = step(&st, w, &in, &flags);
			CHECKF(!ammo || conserved(&st, w, initial), "%s: ammunition not conserved at tick %u", w->name, i);
			if (ev.shot[t] && nshots < 256) shot_ticks[nshots++] = i;
			CHECKF(!ev.shot[t] || ev.projectiles[t] == (tr->projectiles_per_shot ? tr->projectiles_per_shot : 1u), "%s projectiles", w->name);
			CHECKF(!ev.shot[t] || (ev.error[t] >= tr->minimum_error - 1e-6f && ev.error[t] <= (tr->error_max > tr->minimum_error ? tr->error_max : tr->minimum_error) + 1e-6f),
			       "%s: error %f", w->name, (double)ev.error[t]);
			if (st.mode != WEAP_MODE_IDLE) break;               /* reload or overheat ended the burst */
		}
		CHECKF(nshots >= 1 && shot_ticks[0] == 0, "%s: no shot on the first tick", w->name);
		tot->shots += nshots;
		if (once) {
			CHECKF(nshots == 1, "%s: %u shots from one pull", w->name, nshots);
		} else if (nshots >= 3) {
			/* intervals stay between the tag's slowest and fastest rate (minus the chamber time floor) */
			ivmin = tr->rps_max > 0.0f ? 30.0f / tr->rps_max : 0.0f;
			ivmax = tr->rps_min > 0.0f ? 30.0f / tr->rps_min : 1.0f;
			{
				float ch = weap_shot_interval(w, t, 0.0f);
				if (ivmin < ch) ivmin = ch;
				if (ivmax < ch) ivmax = ch;
			}
			if (tr->rps_max <= 0.0f) { ivmin = weap_shot_interval(w, t, 0.0f); ivmax = ivmin; }
			for (i = 1; i < nshots; i++) {
				float d = (float)(shot_ticks[i] - shot_ticks[i - 1]);
				CHECKF(d >= ivmin - 1.0f && d <= ivmax + 1.0f, "%s: interval %.0f ticks outside %.2f .. %.2f", w->name, (double)d, (double)ivmin, (double)ivmax);
			}
			if (tr->accel_time <= 0.0f || tr->rps_min == tr->rps_max) {      /* constant rate: the mean interval is the tag's */
				iv = weap_shot_interval(w, t, tr->rps_max);
				CHECKF(fabsf((float)(shot_ticks[nshots - 1] - shot_ticks[0]) - (float)(nshots - 1) * iv) < 1.0f,
				       "%s: %u shots span %u ticks, interval %.3f", w->name, nshots, shot_ticks[nshots - 1] - shot_ticks[0], (double)iv);
			}
		}
		if (ammo) CHECKF(st.rounds_fired == nshots * need, "%s: %u rounds for %u shots", w->name, (unsigned)st.rounds_fired, nshots);
	}

	/* reloads: empty (full cycles), partial, and a reserve smaller than a magazine */
	for (m = 0; m < w->magazine_count; m++) {
		const weap_magazine *mg = &w->magazine[m];
		unsigned cyc, n, ticks, rt, reserve, loaded, dones;
		if (mg->rounds_loaded_max == 0) continue;
		n = mg->rounds_reloaded ? mg->rounds_reloaded : mg->rounds_loaded_max;
		cyc = (mg->rounds_loaded_max + n - 1) / n;
		for (i = 0; i < 3; i++) {
			weapon_state_init(&st, w);
			memset(&in, 0, sizeof in);
			for (k = 0; k < weap_ready_ticks(w); k++) step(&st, w, &in, NULL);
			reserve = i == 2 ? mg->rounds_loaded_max / 2 : mg->rounds_loaded_max * 2;
			if (mg->rounds_total_max && reserve > mg->rounds_total_max) reserve = mg->rounds_total_max;
			loaded = i == 1 ? mg->rounds_loaded_max / 2 : 0;
			weapon_state_set_ammo(&st, w, m, loaded, reserve);
			st.rounds_fired = 0; st.wasted = 0;
			initial = total_rounds(&st, w);
			loaded = st.loaded[m]; reserve = st.reserve[m];
			if (loaded >= mg->rounds_loaded_max || !reserve) continue;
			in.reload = 1;
			ev = step(&st, w, &in, &flags);
			in.reload = 0;
			CHECKF(ev.flags & WEAP_EV_RELOAD_START, "%s: no reload", w->name);
			rt = weap_reload_ticks(w, m, loaded == 0);
			ticks = 0;
			dones = (ev.flags & WEAP_EV_RELOAD_DONE) != 0;
			while (st.mode == WEAP_MODE_RELOADING && ticks < 20000) {
				ev = step(&st, w, &in, NULL);
				ticks++;
				dones += (ev.flags & WEAP_EV_RELOAD_DONE) != 0;
				CHECK(conserved(&st, w, initial));
			}
			if (mg->flags & WEAP_MAG_WASTES_ROUNDS) {
				CHECKF(st.loaded[m] == (reserve < mg->rounds_loaded_max ? reserve : mg->rounds_loaded_max), "%s: wasteful reload loaded %u", w->name, st.loaded[m]);
			} else {
				unsigned want = loaded + reserve;
				if (want > mg->rounds_loaded_max) want = mg->rounds_loaded_max;
				CHECKF(st.loaded[m] == want, "%s: reload i=%u loaded %u -> %u, want %u", w->name, i, loaded, st.loaded[m], want);
				CHECKF(st.reserve[m] == loaded + reserve - want, "%s: reserve %u", w->name, st.reserve[m]);
			}
			CHECKF(dones >= 1 && ticks >= rt, "%s: reload took %u ticks for %u per cycle", w->name, ticks, rt);
			if (loaded == 0 && reserve >= mg->rounds_loaded_max) {
				CHECKF(ticks == cyc * rt && dones == cyc, "%s: full reload %u ticks in %u cycles, want %u x %u", w->name, ticks, dones, cyc, rt);
			}
			CHECKF(st.mode == WEAP_MODE_IDLE, "%s: reload left mode %u", w->name, st.mode);
		}
	}

	/* empty magazine: nothing fires, one click per pull, ammunition untouched */
	for (k = 0; k < nfire; k++) {
		const weap_trigger *tr;
		int ammo;
		t = list[k];
		tr = &w->trigger[t];
		ammo = tr->magazine != WEAP_NO_MAGAZINE && tr->magazine < w->magazine_count && w->magazine[tr->magazine].rounds_loaded_max > 0;
		if (!ammo || (tr->flags & WEAP_T_PARTIAL_AMMO)) continue;
		weapon_state_init(&st, w);
		memset(&in, 0, sizeof in);
		for (i = 0; i < weap_ready_ticks(w); i++) step(&st, w, &in, NULL);
		weapon_state_set_ammo(&st, w, tr->magazine, 0, 0);
		in.fire[t] = 1;
		ev = step(&st, w, &in, NULL);
		CHECKF(!ev.shot[t] && (ev.flags & WEAP_EV_DRY_FIRE) && st.mode == WEAP_MODE_IDLE && st.rounds_fired == 0, "%s: empty magazine", w->name);
		for (i = 0; i < 40; i++) { ev = step(&st, w, &in, NULL); CHECKF(!ev.shot[t] && !(ev.flags & WEAP_EV_DRY_FIRE), "%s: empty magazine repeats", w->name); }
		weapon_state_set_ammo(&st, w, tr->magazine, 0, 7);              /* with reserve: automatic reload, then it fires again */
		in.fire[t] = 0;
		step(&st, w, &in, NULL);
		in.fire[t] = 1;
		ev = step(&st, w, &in, NULL);
		CHECKF((ev.flags & WEAP_EV_AUTO_RELOAD) && st.mode == WEAP_MODE_RELOADING && !ev.shot[t], "%s: no automatic reload", w->name);
		for (i = 0; i < 20000 && st.mode == WEAP_MODE_RELOADING; i++) step(&st, w, &in, NULL);
		CHECKF(st.loaded[tr->magazine] > 0 || st.rounds_fired > 0, "%s: automatic reload did nothing", w->name);
	}

	/* battery weapons: a dead battery is a click */
	if ((w->flags & WEAP_F_MAX_AGE_DEAD) && nfire) {
		t = list[0];
		weapon_state_init(&st, w);
		memset(&in, 0, sizeof in);
		for (i = 0; i < weap_ready_ticks(w); i++) step(&st, w, &in, NULL);
		st.age = 1.0f;
		in.fire[t] = 1;
		ev = step(&st, w, &in, NULL);
		CHECKF(!ev.shot[t] && (ev.flags & WEAP_EV_BATTERY_DEAD), "%s: dead battery fired", w->name);
	}

	/* heat: a weapon that can overheat does so within 30 s of continuous fire, refuses to fire until cooled, recovers */
	if (w->overheated > 0.0f) {
		for (k = 0; k < nfire; k++) {
			const weap_trigger *tr;
			unsigned hot_at = 0, cool_at = 0;
			t = list[k];
			tr = &w->trigger[t];
			if (tr->heat_per_round <= 0.0f || tr->charging_time > 0.0f) continue;
			weapon_state_init(&st, w);
			memset(&in, 0, sizeof in);
			for (i = 0; i < weap_ready_ticks(w); i++) step(&st, w, &in, NULL);
			in.fire[t] = 1;
			for (i = 1; i <= 900 && !hot_at; i++) {
				ev = step(&st, w, &in, NULL);
				if (ev.flags & WEAP_EV_OVERHEATED) hot_at = i;
			}
			/* a weapon with a magazine reloads (and cools) before it gets hot: the flamethrower never overheats in practice */
			{
				int ammo_trigger = tr->magazine != WEAP_NO_MAGAZINE && tr->magazine < w->magazine_count && w->magazine[tr->magazine].rounds_loaded_max > 0;
				CHECKF(hot_at || st.age >= 1.0f || ammo_trigger, "%s: never overheats", w->name);
			}
			if (!hot_at) continue;
			for (i = 1; i <= 900 && !cool_at; i++) {
				unsigned was_hot = st.mode == WEAP_MODE_OVERHEATED;
				ev = step(&st, w, &in, NULL);
				CHECKF(!(was_hot && ev.shot[t] && !(ev.flags & WEAP_EV_COOLED)), "%s fired while overheated", w->name);
				if (ev.flags & WEAP_EV_COOLED) cool_at = i;
			}
			CHECKF(cool_at, "%s: never cools down", w->name);
		}
	}

	/* melee and put away */
	{
		weapon_state_init(&st, w);
		memset(&in, 0, sizeof in);
		for (i = 0; i < weap_ready_ticks(w); i++) step(&st, w, &in, NULL);
		if (!(w->flags & WEAP_F_NO_MELEE)) {
			unsigned n = weap_melee_ticks(w), hits = 0;
			in.melee = 1;
			ev = step(&st, w, &in, NULL);
			CHECKF((ev.flags & WEAP_EV_MELEE_START) && st.mode == WEAP_MODE_MELEE && st.timer == n, "%s melee", w->name);
			in.melee = 0;
			for (t = 0; t < WEAP_MAX_TRIGGERS; t++) in.fire[t] = 1;
			for (i = 1; i <= n; i++) {
				ev = step(&st, w, &in, NULL);
				hits += (ev.flags & WEAP_EV_MELEE_HIT) != 0;
				for (t = 0; t < WEAP_MAX_TRIGGERS; t++) CHECKF(i == n || !ev.shot[t], "%s shot during melee", w->name);
			}
			CHECKF(hits == 1 && st.mode != WEAP_MODE_MELEE, "%s melee hits %u", w->name, hits);
		}
		weapon_state_init(&st, w);
		for (i = 0; i < weap_ready_ticks(w); i++) step(&st, w, &none_in, NULL);
		memset(&in, 0, sizeof in);
		in.switch_out = 1;
		step(&st, w, &in, NULL);
		in.switch_out = 0;
		CHECK(st.mode == WEAP_MODE_PUTTING_AWAY && st.timer == weap_put_away_ticks(w));
		for (i = 1; i < weap_put_away_ticks(w); i++) { ev = step(&st, w, &in, NULL); CHECK(!(ev.flags & WEAP_EV_PUT_AWAY)); }
		ev = step(&st, w, &in, NULL);
		CHECK((ev.flags & WEAP_EV_PUT_AWAY) && st.mode == WEAP_MODE_STOWED);
	}

	/* zoom */
	if (w->zoom_levels) {
		weapon_state_init(&st, w);
		memset(&in, 0, sizeof in);
		in.zoom = 1;
		for (i = 1; i <= (unsigned)w->zoom_levels + 1u; i++) {
			ev = step(&st, w, &in, NULL);
			CHECKF((ev.flags & WEAP_EV_ZOOM) && st.zoom == i % ((unsigned)w->zoom_levels + 1u), "%s zoom", w->name);
		}
	}

	/* hostile play: 5000 ticks of random inputs keep every invariant */
	{
		uint32_t seed = w->tag_id * 2654435761u + 3u;
		unsigned lm[WEAP_MAX_MAGAZINES];
		weapon_state_init(&st, w);
		initial = total_rounds(&st, w);
		for (m = 0; m < w->magazine_count; m++) lm[m] = w->magazine[m].rounds_loaded_max;
		memset(&in, 0, sizeof in);
		for (i = 0; i < 5000; i++) {
			unsigned prev_mode = st.mode;
			seed = seed * 1664525u + 1013904223u;
			for (t = 0; t < WEAP_MAX_TRIGGERS; t++) in.fire[t] = ((seed >> (6 + t)) & 3u) != 0;
			in.reload = ((seed >> 12) & 63u) == 0;
			in.melee = ((seed >> 17) & 127u) == 0;
			in.zoom = ((seed >> 20) & 127u) == 0;
			in.switch_out = ((seed >> 24) & 1023u) == 0;
			weapon_fire_step(&st, w, &in, &ev);
			CHECKF(conserved(&st, w, initial), "%s: ammunition not conserved at random tick %u", w->name, i);
			for (m = 0; m < w->magazine_count; m++) CHECK(st.loaded[m] <= lm[m]);
			CHECK(st.heat >= 0.0f && st.age >= 0.0f && st.age <= 1.0f);
			if (prev_mode == WEAP_MODE_STOWED) CHECK(ev.flags == 0);
			for (t = 0; t < WEAP_MAX_TRIGGERS; t++)
				if (ev.shot[t]) {
					CHECKF(prev_mode != WEAP_MODE_STOWED && prev_mode != WEAP_MODE_PUTTING_AWAY, "%s shot in mode %u", w->name, prev_mode);
					CHECK(t < w->trigger_count && ev.projectiles[t] >= 1);
				}
		}
	}

	if (table) {
		const weap_trigger *tr = nfire ? &w->trigger[list[0]] : (w->trigger_count ? &w->trigger[0] : NULL);
		char rate[64] = "-", mag[24] = "-", rel[16] = "-", spd[16] = "-", dmg[64] = "-";
		weap_projectile p;
		weap_damage d;
		int pi;
		if (tr && (tr->rps_max > 0.0f || tr->charging_time > 0.0f)) {
			if (tr->charging_time > 0.0f) snprintf(rate, sizeof rate, "charge %.2fs", (double)tr->charging_time);
			else if (tr->rps_min == tr->rps_max) snprintf(rate, sizeof rate, "%g/s", (double)tr->rps_max);
			else snprintf(rate, sizeof rate, "%g-%g/s", (double)tr->rps_min, (double)tr->rps_max);
		} else if (tr && weap_shot_interval(w, list[0], 0.0f) > 0.0f) {
			snprintf(rate, sizeof rate, "1/%.1fs", (double)weap_shot_interval(w, list[0], 0.0f) / 30.0);
		} else if (tr) {
			snprintf(rate, sizeof rate, "single");
		}
		if (w->magazine_count && w->magazine[0].rounds_loaded_max && w->magazine[0].rounds_total_max >= 9999) {
			snprintf(mag, sizeof mag, "%u / unlimited", w->magazine[0].rounds_loaded_max);
		} else if (w->magazine_count && w->magazine[0].rounds_loaded_max) {
			snprintf(mag, sizeof mag, "%u / %u", w->magazine[0].rounds_loaded_max, w->magazine[0].rounds_total_max);
			snprintf(rel, sizeof rel, "%.2f s", (double)weap_reload_ticks(w, 0, 0) / 30.0);
		} else if (w->overheated > 0.0f) {
			snprintf(mag, sizeof mag, "battery");
		} else if (nfire) {
			snprintf(mag, sizeof mag, "unlimited");
		}
		pi = tr && tr->projectile ? weap_find_projectile(s, tr->projectile) : -1;
		if (pi >= 0 && weap_get_projectile(s, (uint32_t)pi, &p)) {
			snprintf(spd, sizeof spd, "%.0f", (double)(p.initial_velocity * 30.0f));
			if (p.impact_damage && weap_find_damage(s, p.impact_damage) >= 0 && weap_get_damage(s, (uint32_t)weap_find_damage(s, p.impact_damage), &d)) {
				if (d.damage_upper[0] == d.damage_upper[1] && d.damage_lower == d.damage_upper[0]) snprintf(dmg, sizeof dmg, "%g", (double)d.damage_upper[0]);
				else snprintf(dmg, sizeof dmg, "%g-%g (min %g)", (double)d.damage_upper[0], (double)d.damage_upper[1], (double)d.damage_lower);
			} else if (p.detonation_effect) snprintf(dmg, sizeof dmg, "explosion");
		}
		printf("| %-18s | %-12s | %-11s | %-7s | %-5s | %-18s |\n", w->name, rate, mag, rel, spd, dmg);
	}
	tot->weapons++;
	tot->triggers += w->trigger_count;
	return 0;
}

static int check_real_pack(const char *path, int table, totals *tot)
{
	uint8_t *raw;
	size_t n;
	weap_section s;
	uint32_t i, j;
	if (load_weap(path, &raw, &n)) return 1;
	CHECKF(weap_open(&s, raw, n) == WEAP_OK, "%s does not open", path);
	if (table) printf("\n%s\n", path);
	/* damage effects, equipment, grenades: relations that must hold on every map */
	for (i = 0; i < s.n_damages; i++) {
		weap_damage d;
		CHECK(weap_get_damage(&s, i, &d));
		CHECKF(d.damage_lower <= d.damage_upper[0] && d.damage_upper[0] <= d.damage_upper[1] && d.category <= WEAP_CAT_SHOTGUN, "%s", d.name);
	}
	for (i = 0; i < s.n_grenades; i++) {
		weap_grenade g;
		weap_equipment q;
		int qi;
		CHECK(weap_get_grenade(&s, i, &g) && g.max_count > 0);
		qi = weap_find_equipment(&s, g.equipment);
		CHECK(qi >= 0 && weap_get_equipment(&s, (uint32_t)qi, &q) && q.grenade_type == i && q.powerup_type == WEAP_POWERUP_GRENADE);
		CHECK(weap_find_projectile(&s, g.projectile) >= 0);
	}
	for (i = 0; i < s.n_weapons; i++) {
		weap_weapon w;
		CHECK(weap_get_weapon(&s, i, &w));
		for (j = 0; j < w.magazine_count; j++) {
			unsigned e;
			for (e = 0; e < w.magazine[j].equipment_count; e++) CHECK(weap_find_equipment(&s, w.magazine[j].equipment_tag[e]) >= 0);
		}
		if (check_real_weapon(&s, &w, table, tot)) return 1;
	}
	walk(&s);
	free(raw);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t *raw;
	size_t n;
	weap_section s;
	int i, table = 0;
	totals tot = { 0, 0, 0 };

	CHECK(argc >= 2);
	if (load_weap(argv[1], &raw, &n)) return 1;
	CHECK(weap_open(&s, raw, n) == WEAP_OK);
	CHECK(((uintptr_t)s.weapons | (uintptr_t)s.triggers | (uintptr_t)s.damages) % 16 == (uintptr_t)raw % 16);
	if (check_fixture_data(&s) || check_rifle(&s) || check_plasma(&s) || check_shotgun(&s)) return 1;
	walk(&s);
	if (check_corruption(raw, n)) return 1;
	free(raw);

	for (i = 2; i < argc; i++) if (strcmp(argv[i], "-t") == 0) table = 1;
	if (table) {
		printf("| weapon | rate | magazine (loaded / max) | reload | projectile speed (wu/s) | damage |\n");
		printf("|---|---|---|---|---|---|\n");
	}
	for (i = 2; i < argc; i++)
		if (strcmp(argv[i], "-t") != 0) {
			if (check_real_pack(argv[i], table, &tot)) return 1;
			printf("%s: all weapons ok\n", argv[i]);
		}
	if (tot.weapons) printf("%u weapons, %u triggers, %u burst shots simulated\n", tot.weapons, tot.triggers, tot.shots);
	printf("ok\n");
	return 0;
}
