/* Host test: open the FXD0 / FXD1 / FXTX sections of a pack written by tools/tests/test_fxconv.py and check them
 * against the synthetic world that test builds (3 effects, 2 particles, 1 point physics, 2 decals, 1 light, 1 particle
 * system, 2 bitmaps with 4 converted textures; FXD1 is an empty section). Then truncate at every length, corrupt every
 * table and field, and flip every byte: a corrupt section is either rejected or safe to walk completely (ASan/UBSan). */
#include "stream/hp2.h"
#include "world/fxdef.h"
#include "world/tex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
/* tag id of the i-th tag of the synthetic region (tagindex.TagIndex.build: salt 0xE174 + i) */
#define ID(i) ((((uint32_t)0xE174u + (i)) << 16) | (uint32_t)(i))

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static volatile uint32_t sink;
static int bad(const char *what) { fprintf(stderr, "walk: %s\n", what); abort(); return 1; }

static int near1(float a, float b) { return fabsf(a - b) < 1e-5f; }

/* Touch everything an opened section hands out; abort on anything an accepted section must not contain. */
static void walk(const fxdef_section *s)
{
	uint32_t acc = 0, i, e, k, t, q;
	for (i = 0; i < s->count[FXD_T_EFFECTS]; i++) {
		fxd_effect ef = fxdef_get_effect(s, i);
		char nm[FXD_NAME_LEN];
		if (fxdef_find_effect(s, ef.tag_id) != (int)i) bad("effect find");
		for (k = 0; k < ef.location_count; k++) {
			if (!fxdef_location_name(s, &ef, k, nm)) bad("location");
			acc += (uint32_t)strlen(nm);
		}
		if (fxdef_location_name(s, &ef, ef.location_count, nm)) bad("location past the end");
		for (e = 0; e < ef.event_count; e++) {
			fxd_event ev = fxdef_get_event(s, &ef, e);
			if (!(ev.skip == ev.skip)) bad("nan skip");
			for (k = 0; k < ev.part_count; k++) {
				fxd_part p = fxdef_get_part(s, &ev, k);
				if (p.location != FXD_NONE && p.location >= ef.location_count) bad("part location");
				if (p.kind == FXD_K_DECAL && p.index >= s->count[FXD_T_DECALS]) bad("part decal");
				if (p.kind == FXD_K_LIGHT && p.index >= s->count[FXD_T_LIGHTS]) bad("part light");
				if (p.kind == FXD_K_PARTICLE_SYSTEM && p.index >= s->count[FXD_T_PCTL]) bad("part pctl");
				acc += p.tag_id + (uint32_t)strlen(p.group) + p.kind;
			}
			for (k = 0; k < ev.emitter_count; k++) {
				fxd_emitter c = fxdef_get_emitter(s, &ev, k);
				fxd_particle pa = fxdef_get_particle(s, c.particle);
				if (c.particle >= s->count[FXD_T_PARTICLES] || pa.tag_id != c.particle_tag) bad("emitter particle");
				if (c.location != FXD_NONE && c.location >= ef.location_count) bad("emitter location");
				if (c.count[0] < 0 || c.count[1] < 0) bad("emitter count");
				acc += (uint32_t)c.count[1] + c.tint_hi[0];
			}
		}
	}
	for (i = 0; i < s->count[FXD_T_PARTICLES]; i++) {
		fxd_particle p = fxdef_get_particle(s, i);
		if (fxdef_find_particle(s, p.tag_id) != (int)i) bad("particle find");
		if (p.bitmap != FXD_NONE && p.bitmap >= s->count[FXD_T_BITMAPS]) bad("particle bitmap");
		if (p.physics != FXD_NONE && p.physics >= s->count[FXD_T_PHYSICS]) bad("particle physics");
		acc += p.flags + p.blend;
	}
	for (i = 0; i < s->count[FXD_T_PHYSICS]; i++) {
		fxd_physics p = fxdef_get_physics(s, i);
		if (fxdef_find_physics(s, p.tag_id) != (int)i) bad("physics find");
		acc += p.flags;
	}
	for (i = 0; i < s->count[FXD_T_DECALS]; i++) {
		fxd_decal d = fxdef_get_decal(s, i);
		if (fxdef_find_decal(s, d.tag_id) != (int)i) bad("decal find");
		if (d.bitmap != FXD_NONE && d.bitmap >= s->count[FXD_T_BITMAPS]) bad("decal bitmap");
		acc += d.flags;
	}
	for (i = 0; i < s->count[FXD_T_LIGHTS]; i++) {
		fxd_light l = fxdef_get_light(s, i);
		if (fxdef_find_light(s, l.tag_id) != (int)i) bad("light find");
		acc += l.flags;
	}
	for (i = 0; i < s->count[FXD_T_PCTL]; i++) {
		fxd_pctl p = fxdef_get_pctl(s, i);
		if (fxdef_find_pctl(s, p.tag_id) != (int)i) bad("pctl find");
		for (t = 0; t < p.type_count; t++) {
			fxd_pctl_type ty = fxdef_get_pctl_type(s, &p, t);
			acc += (uint32_t)strlen(ty.name) + ty.flags;
			for (k = 0; k < ty.const_count; k++) acc += fxdef_const(s, ty.first_const + k) > 0.0f;
			for (k = 0; k < ty.state_count; k++) {
				fxd_pctl_state st = fxdef_get_pctl_state(s, &ty, k);
				for (q = 0; q < st.const_count; q++) acc += fxdef_const(s, st.first_const + q) > 0.0f;
				acc += (uint32_t)strlen(st.name);
			}
			for (k = 0; k < ty.pstate_count; k++) {
				fxd_pctl_pstate ps = fxdef_get_pctl_pstate(s, &ty, k);
				if (ps.bitmap != FXD_NONE && ps.bitmap >= s->count[FXD_T_BITMAPS]) bad("pstate bitmap");
				for (q = 0; q < ps.const_count; q++) acc += fxdef_const(s, ps.first_const + q) > 0.0f;
				acc += (uint32_t)strlen(ps.name);
			}
		}
	}
	for (i = 0; i < s->count[FXD_T_BITMAPS]; i++) {
		fxd_bitmap b = fxdef_get_bitmap(s, i);
		fxd_sprite sp;
		if (fxdef_find_bitmap(s, b.tag_id) != (int)i) bad("bitmap find");
		for (k = 0; k < b.sequence_count; k++) {
			fxd_sequence sq = fxdef_get_sequence(s, &b, k);
			for (q = 0; q < sq.sprite_count; q++) {
				sp = fxdef_get_sprite(s, &sq, q);
				if (sp.image >= b.image_count) bad("sprite image");
				if (!(sp.u0 >= 0.0f && sp.u1 <= 1.0f && sp.v0 >= 0.0f && sp.v1 <= 1.0f)) bad("sprite rect");
				acc += sp.texture;
			}
		}
		for (k = 0; k < (uint32_t)b.sequence_count + 3u; k++) {
			if (fxdef_sprite(s, i, k, 7, &sp)) acc += sp.image;
		}
	}
	if (fxdef_sprite(s, s->count[FXD_T_BITMAPS], 0, 0, &(fxd_sprite){ 0 })) bad("sprite of a missing bitmap");
	acc += fxdef_check_textures(s, 0x10000u);
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

int main(int argc, char **argv)
{
	CHECK(argc == 2);
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
	uint8_t *raw, *raw1, *rawt;
	size_t rawn, rawn1, rawtn;
	if (load_section(&pk, "FXD0", &raw, &rawn) || load_section(&pk, "FXD1", &raw1, &rawn1) || load_section(&pk, "FXTX", &rawt, &rawtn)) return 1;

	fxdef_section s;
	CHECK(fxdef_open(&s, raw, rawn) == FXD_OK);
	CHECK(s.count[FXD_T_EFFECTS] == 3 && s.count[FXD_T_EVENTS] == 4 && s.count[FXD_T_PARTS] == 7 && s.count[FXD_T_EMITTERS] == 2);
	CHECK(s.count[FXD_T_LOCATIONS] == 3 && s.count[FXD_T_PARTICLES] == 2 && s.count[FXD_T_PHYSICS] == 1 && s.count[FXD_T_DECALS] == 2);
	CHECK(s.count[FXD_T_LIGHTS] == 1 && s.count[FXD_T_PCTL] == 1 && s.count[FXD_T_BITMAPS] == 2 && s.count[FXD_T_SEQUENCES] == 4 && s.count[FXD_T_SPRITES] == 6);
	CHECK(((uintptr_t)s.table[FXD_T_EFFECTS] | (uintptr_t)s.table[FXD_T_SPRITES] | (uintptr_t)s.table[FXD_T_EMITTERS]) % 16 == (uintptr_t)raw % 16);

	/* effects: tags 4 (fire), 5 (impact), 6 (splash); the device effect (7) is not in the gameplay set */
	CHECK(fxdef_find_effect(&s, ID(4)) == 0 && fxdef_find_effect(&s, ID(5)) == 1 && fxdef_find_effect(&s, ID(6)) == 2);
	CHECK(fxdef_find_effect(&s, ID(7)) == -1 && fxdef_find_effect(&s, 0) == -1 && fxdef_find_effect(&s, 0xFFFFFFFFu) == -1);
	fxd_effect fire = fxdef_get_effect(&s, 0), impact = fxdef_get_effect(&s, 1), splash = fxdef_get_effect(&s, 2);
	CHECK(fire.tag_id == ID(4) && fire.flags == 2 && fire.loop_start == -1 && fire.loop_stop == -1 && fire.damage_radius == 0.0f);
	CHECK(fire.location_count == 2 && fire.event_count == 2);
	CHECK(impact.flags == 0 && impact.loop_start == 1 && impact.loop_stop == -1 && near1(impact.damage_radius, 4.5f));
	CHECK(impact.location_count == 1 && impact.event_count == 1 && splash.flags == 1 && splash.location_count == 0 && splash.event_count == 1);
	char nm[FXD_NAME_LEN];
	CHECK(fxdef_location_name(&s, &fire, 0, nm) && nm[0] == 0);
	CHECK(fxdef_location_name(&s, &fire, 1, nm) && strcmp(nm, "muzzle") == 0);
	CHECK(!fxdef_location_name(&s, &fire, 2, nm) && !fxdef_location_name(&s, &splash, 0, nm));
	fxd_effect none = fxdef_get_effect(&s, 3);
	CHECK(none.tag_id == 0 && none.event_count == 0 && none.loop_start == -1);

	/* fire, event 0: a sound and a light at the muzzle, one spark emitter */
	fxd_event e0 = fxdef_get_event(&s, &fire, 0), e1 = fxdef_get_event(&s, &fire, 1);
	CHECK(e0.skip == 0.0f && e0.delay[0] == 0.0f && e0.duration[1] == 0.0f && e0.part_count == 2 && e0.emitter_count == 1);
	fxd_part p0 = fxdef_get_part(&s, &e0, 0), p1 = fxdef_get_part(&s, &e0, 1);
	CHECK(p0.kind == FXD_K_SOUND && p0.tag_id == ID(8) && strcmp(p0.group, "snd!") == 0 && p0.location == 0 && p0.index == 0xFFFFFFFFu);
	CHECK(p0.velocity[0] == 1.0f && p0.velocity[1] == 1.0f && p0.radius_mod[1] == 1.0f);
	CHECK(p1.kind == FXD_K_LIGHT && p1.tag_id == ID(10) && strcmp(p1.group, "ligh") == 0 && p1.location == 1 && p1.violence == 1 && p1.index == 0);
	fxd_emitter c = fxdef_get_emitter(&s, &e0, 0);
	CHECK(c.location == 1 && c.particle == 1 && c.particle_tag == ID(15) && c.create_in == 0 && c.create == 0);
	CHECK(near1(c.yaw, 0.5f) && near1(c.pitch, 0.25f) && near1(c.offset[0], 0.1f) && near1(c.offset[1], 0.2f) && near1(c.offset[2], 0.3f));
	CHECK(c.flags == FXD_EMF_ATTACHED && c.distribution == FXD_DIST_CONSTANT && c.count[0] == 1 && c.count[1] == 3);
	CHECK(c.dist_radius[0] == 0.0f && c.dist_radius[1] == 0.5f && c.velocity[0] == 1.0f && c.velocity[1] == 2.0f && near1(c.cone, 0.1f));
	CHECK(c.angvel[0] == -1.0f && c.angvel[1] == 1.0f && near1(c.radius[0], 0.05f) && near1(c.radius[1], 0.1f));
	CHECK(c.tint_lo[0] == 255 && c.tint_lo[1] == 128 && c.tint_lo[2] == 64 && c.tint_lo[3] == 255 && c.tint_hi[0] == 255 && c.tint_hi[3] == 255);
	/* event 1: a delayed, sometimes skipped damage effect and a particle system */
	CHECK(e1.skip == 0.25f && near1(e1.delay[0], 0.2f) && near1(e1.delay[1], 0.5f) && e1.duration[0] == 1.0f && e1.duration[1] == 2.0f);
	CHECK(e1.part_count == 2 && e1.emitter_count == 0);
	fxd_part p2 = fxdef_get_part(&s, &e1, 0), p3 = fxdef_get_part(&s, &e1, 1);
	CHECK(p2.kind == FXD_K_DAMAGE && p2.tag_id == ID(9) && strcmp(p2.group, "jpt!") == 0 && p2.create_in == FXD_ENV_AIR);
	CHECK(p3.kind == FXD_K_PARTICLE_SYSTEM && p3.tag_id == ID(13) && p3.index == 0);
	fxd_part pz = fxdef_get_part(&s, &e1, 2);                        /* out of range: a neutral record */
	CHECK(pz.kind == 0 && pz.location == FXD_NONE && pz.index == 0xFFFFFFFFu && pz.group[0] == 0);
	fxd_emitter cz = fxdef_get_emitter(&s, &e1, 0);
	CHECK(cz.particle == FXD_NONE && cz.count[1] == 0);

	/* impact: a face-down decal (chain of two), a sound without location, one dust emitter in air only */
	fxd_event ie = fxdef_get_event(&s, &impact, 0);
	fxd_part d0 = fxdef_get_part(&s, &ie, 0), d1 = fxdef_get_part(&s, &ie, 1);
	CHECK(d0.kind == FXD_K_DECAL && d0.tag_id == ID(11) && d0.flags == FXD_PF_FACE_DOWN && d0.index == 0 && near1(d0.radius_mod[0], 0.25f));
	CHECK(d1.kind == FXD_K_SOUND && d1.location == FXD_NONE);
	fxd_emitter dc = fxdef_get_emitter(&s, &ie, 0);
	CHECK(dc.create_in == FXD_ENV_AIR && dc.location == 0 && dc.particle == 0 && dc.particle_tag == ID(14) && dc.distribution == FXD_DIST_BUILDUP);
	CHECK(dc.count[0] == 2 && dc.count[1] == 5 && dc.angvel[0] == -6.0f && dc.angvel[1] == 6.0f && near1(dc.radius[1], 0.15f));
	CHECK(dc.tint_lo[0] == 128 && dc.tint_lo[1] == 102 && dc.tint_lo[2] == 77 && dc.tint_lo[3] == 255);

	/* particles: dust (14) and spark (15) */
	CHECK(fxdef_find_particle(&s, ID(14)) == 0 && fxdef_find_particle(&s, ID(15)) == 1 && fxdef_find_particle(&s, ID(16)) == -1);
	fxd_particle dust = fxdef_get_particle(&s, 0), spark = fxdef_get_particle(&s, 1);
	CHECK(dust.flags == 0x208 && dust.bitmap == 0 && dust.physics == 0 && dust.collision_kind == FXD_K_SOUND && dust.collision_effect == ID(8));
	CHECK(dust.death_kind == FXD_K_EFFECT && dust.death_effect == ID(6) && fxdef_find_effect(&s, dust.death_effect) == 2);
	CHECK(near1(dust.lifespan[0], 0.3f) && near1(dust.lifespan[1], 0.5f) && near1(dust.fade_in, 0.1f) && near1(dust.fade_out, 0.2f) && dust.min_size == 4.0f);
	CHECK(dust.radius_anim[0] == 1.0f && dust.radius_anim[1] == 2.0f && dust.anim_rate[0] == 30.0f && dust.fade_start_size == 5.0f && dust.fade_end_size == 4.0f);
	CHECK(dust.seq_first == 0 && dust.seq_initial == 1 && dust.seq_loop == 0 && dust.seq_final == 0 && near1(dust.sprite_size, 0.02f));
	CHECK(dust.orientation == FXD_ORIENT_PARALLEL && dust.blend == FXD_BLEND_ADD && dust.fade_mode == FXD_FADE_PERPENDICULAR && dust.shader_flags == 1);
	CHECK(spark.physics == FXD_NONE && spark.collision_kind == FXD_K_NONE && spark.collision_effect == FXD_NO_TAG && spark.death_effect == FXD_NO_TAG);
	CHECK(spark.blend == FXD_BLEND_ALPHA && near1(spark.lifespan[0], 0.2f) && near1(spark.lifespan[1], 0.2f));
	fxd_particle pn = fxdef_get_particle(&s, 2);
	CHECK(pn.tag_id == 0 && pn.bitmap == FXD_NONE && pn.collision_effect == FXD_NO_TAG);

	/* point physics (16), decals (11, 12), light (10), particle system (13) */
	CHECK(fxdef_find_physics(&s, ID(16)) == 0 && fxdef_find_physics(&s, ID(14)) == -1);
	fxd_physics ph = fxdef_get_physics(&s, 0);
	CHECK(ph.flags == (FXD_PP_COLLIDES_STRUCTURE | FXD_PP_FLAMETHROWER) && near1(ph.density, 2.4f) && ph.air_friction == 900.0f && ph.water_friction == 14000.0f);
	CHECK(near1(ph.surface_friction, 0.1f) && near1(ph.elasticity, 0.35f) && near1(ph.air_gravity_scale, -0.999f) && ph.water_gravity_scale == 0.5f);
	CHECK(fxdef_find_decal(&s, ID(11)) == 0 && fxdef_find_decal(&s, ID(12)) == 1 && fxdef_find_decal(&s, ID(13)) == -1);
	fxd_decal h = fxdef_get_decal(&s, 0), h2 = fxdef_get_decal(&s, 1);
	CHECK(h.flags == FXD_DF_NO_RANDOM_ROTATION && h.type == FXD_DECAL_BURN && h.layer == FXD_LAYER_PRIMARY && h.next_tag == ID(12) && h.bitmap == 1);
	CHECK(h.blend == FXD_BLEND_DOUBLE_MULTIPLY && near1(h.radius[0], 0.1f) && near1(h.radius[1], 0.15f) && h.intensity[0] == 0.5f && h.intensity[1] == 1.0f);
	CHECK(h.color_lo[0] == 0.25f && h.color_lo[2] == 0.75f && h.color_hi[0] == 1.0f && h.color_hi[2] == 0.0f);
	CHECK(h.lifetime[0] == 180.0f && h.decay[1] == 20.0f && h.loop_frame == 2 && h.anim_speed == 3 && h.max_extent == 16.0f);
	CHECK(h2.next_tag == FXD_NO_TAG && h2.blend == FXD_BLEND_MULTIPLY);
	CHECK(fxdef_find_light(&s, ID(10)) == 0 && fxdef_find_light(&s, ID(9)) == -1);
	fxd_light L = fxdef_get_light(&s, 0);
	CHECK(L.flags == 0x21 && L.radius == 2.0f && L.radius_mod[0] == 1.0f && fabsf(L.falloff_angle - 3.14159265f) < 1e-5f && fabsf(L.cutoff_angle - 3.14159265f) < 1e-5f);
	CHECK(L.interp_flags == 3 && L.color_lo[0] == 0.0f && L.color_hi[0] == 1.0f && near1(L.color_hi[1], 0.96f) && near1(L.color_hi[3], 0.69f));
	CHECK(L.duration == 30.0f && L.falloff_fn == 3);
	CHECK(fxdef_find_pctl(&s, ID(13)) == 0 && fxdef_find_pctl(&s, ID(0)) == -1);
	fxd_pctl pc = fxdef_get_pctl(&s, 0);
	CHECK(pc.physics == 0 && pc.update_physics == FXD_PCTL_PHYS_EXPLOSION && pc.type_count == 1);
	fxd_pctl_type ty = fxdef_get_pctl_type(&s, &pc, 0);
	CHECK(strcmp(ty.name, "fire") == 0 && ty.flags == 0x10 && ty.initial_count == 12 && ty.render_mode == FXD_PCTL_ROTATIONAL && ty.radius == 1.5f);
	CHECK(ty.creation_physics == FXD_PCTL_PHYS_EXPLOSION && ty.const_count == 2 && near1(fxdef_const(&s, ty.first_const), 0.1f) && near1(fxdef_const(&s, ty.first_const + 1), 0.2f));
	CHECK(fxdef_const(&s, 0xFFFFFFFFu) == 0.0f && ty.state_count == 2 && ty.pstate_count == 2);
	fxd_pctl_state st0 = fxdef_get_pctl_state(&s, &ty, 0), st1 = fxdef_get_pctl_state(&s, &ty, 1);
	CHECK(strcmp(st0.name, "birth") == 0 && strcmp(st1.name, "death") == 0 && st0.duration[0] == 1.0f && st0.duration[1] == 1.5f);
	CHECK(st0.transition[0] == 0.25f && st0.transition[1] == 0.5f && st0.radius_mult == 0.5f && st0.creation_rate == 15.0f && st0.creation_physics == 1);
	CHECK(st0.color_mult[0] == 1.0f && st0.scale_mult == 1.0f);
	fxd_pctl_pstate ps0 = fxdef_get_pctl_pstate(&s, &ty, 0), ps1 = fxdef_get_pctl_pstate(&s, &ty, 1);
	CHECK(strcmp(ps0.name, "fire") == 0 && strcmp(ps1.name, "smoke") == 0 && ps0.bitmap == 0 && ps0.sequence == 0 && ps1.sequence == 1 && ps0.physics == 0);
	CHECK(near1(ps0.duration[0], 0.1f) && near1(ps0.transition[1], 0.8f) && near1(ps0.scale[1], 0.03f) && ps0.rot_rate[0] == 3.5f && ps0.rot_rate[1] == 7.0f);
	CHECK(ps0.color1[0] == 1.0f && near1(ps0.color1[2], 0.92f) && near1(ps0.color2[3], 0.28f) && ps0.radius_mult == 1.0f);
	CHECK(ps0.blend == FXD_BLEND_ADD && ps1.blend == FXD_BLEND_ALPHA && ps0.shader_flags == 1 && ps0.fade_mode == 0);

	/* bitmaps: the sprite sheet (17) and the plain 2D bitmap (18) */
	CHECK(fxdef_find_bitmap(&s, ID(17)) == 0 && fxdef_find_bitmap(&s, ID(18)) == 1 && fxdef_find_bitmap(&s, ID(19)) == -1);
	fxd_bitmap bd = fxdef_get_bitmap(&s, 0), bh = fxdef_get_bitmap(&s, 1);
	CHECK(bd.kind == 3 && bd.sequence_count == 3 && bd.image_count == 2 && bd.texture == 0);
	CHECK(bh.kind == 0 && bh.sequence_count == 1 && bh.image_count == 2 && bh.texture == 2);
	fxd_sequence q0 = fxdef_get_sequence(&s, &bd, 0), q1 = fxdef_get_sequence(&s, &bd, 1), q2 = fxdef_get_sequence(&s, &bd, 2);
	CHECK(q0.sprite_count == 3 && q1.sprite_count == 1 && q2.sprite_count == 0 && fxdef_get_sequence(&s, &bd, 3).sprite_count == 0);
	fxd_sprite sp = fxdef_get_sprite(&s, &q0, 1);
	CHECK(sp.texture == 0 && sp.image == 0 && sp.u0 == 0.5f && sp.u1 == 1.0f && sp.v0 == 0.0f && sp.v1 == 0.5f && near1(sp.reg[0], 0.1f) && near1(sp.reg[1], -0.1f));
	sp = fxdef_get_sprite(&s, &q0, 2);
	CHECK(sp.texture == 1 && sp.image == 1 && sp.u0 == 0.0f && sp.u1 == 1.0f && sp.v1 == 1.0f);
	sp = fxdef_get_sprite(&s, &q0, 3);
	CHECK(sp.texture == FXD_NONE && sp.u1 == 0.0f);
	CHECK(fxdef_sprite(&s, 0, 0, 4, &sp) && sp.u0 == 0.5f);                    /* frame 4 of 3 sprites -> 1 */
	CHECK(fxdef_sprite(&s, 0, 2, 0, &sp) && sp.texture == 1 && sp.u0 == 0.25f); /* empty sequence: the sequence before */
	CHECK(fxdef_sprite(&s, 0, 99, 5, &sp) && sp.texture == 1 && sp.u0 == 0.25f); /* past the end: the last one that has sprites */
	CHECK(fxdef_sprite(&s, 1, 5, 3, &sp) && sp.texture == 3 && sp.image == 1);
	CHECK(!fxdef_sprite(&s, 7, 0, 0, &sp) && sp.texture == FXD_NONE);
	CHECK(fxdef_check_textures(&s, 4) && !fxdef_check_textures(&s, 3));
	walk(&s);

	/* FXTX: a TEXS blob keyed by bitmap tag id */
	tex_section tx;
	CHECK(tex_open(&tx, rawt, rawtn) == TEX_OK);
	CHECK(tx.texture_count == 4 && tx.binding_count == 2);
	tex_binding bind;
	CHECK(fxdef_bitmap_binding(&tx, ID(17), &bind) && bind.texture == 0 && bind.blend == TEX_BLEND_ADD);
	CHECK(fxdef_bitmap_binding(&tx, ID(18), &bind) && bind.texture == 2);
	CHECK(!fxdef_bitmap_binding(&tx, ID(19), &bind));
	tex_info t0 = tex_get(&tx, 0), t1 = tex_get(&tx, 1), t2 = tex_get(&tx, 2);
	CHECK(t0.width == 16 && t0.height == 16 && t0.mip_count == 1 && (t0.flags & TEX_F_BLEND));          /* sheets: one mip */
	CHECK(t1.width == 32 && t1.height == 16 && t1.mip_count == 1 && t2.width == 16 && t2.mip_count == 2);
	CHECK(fxdef_check_textures(&s, tx.texture_count));

	/* FXD1: empty section */
	fxdef_section e;
	CHECK(fxdef_open(&e, raw1, rawn1) == FXD_OK);
	for (int t = 0; t < FXD_T_COUNT; t++) CHECK(e.count[t] == 0);
	CHECK(fxdef_find_effect(&e, ID(4)) == -1 && fxdef_find_bitmap(&e, ID(17)) == -1);
	fxd_sprite none_sp;
	CHECK(!fxdef_sprite(&e, 0, 0, 0, &none_sp));
	walk(&e);
	fxdef_section unopened;
	CHECK(fxdef_open(&unopened, raw, 10) == FXD_ERR_TRUNCATED);
	CHECK(unopened.count[FXD_T_EFFECTS] == 0 && fxdef_get_effect(&unopened, 0).tag_id == 0);   /* a failed open leaves a safe, empty view */
	walk(&unopened);

	/* ---- hostile input ---- */
	fxdef_section bad_s;
	for (size_t cut = 0; cut < rawn; cut++) CHECK(fxdef_open(&bad_s, raw, cut) != FXD_OK);
	CHECK(fxdef_open(&bad_s, NULL, rawn) != FXD_OK);
	uint8_t *copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(fxdef_open(&bad_s, copy, rawn + 16) == FXD_ERR_LAYOUT);          /* size field no longer matches */
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(fxdef_open(&bad_s, copy, rawn) == FXD_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(fxdef_open(&bad_s, copy, rawn) == FXD_ERR_MAGIC);

	/* every table of the header: moved, misaligned, oversized, counted wrong */
	static const char *const tabn[FXD_T_COUNT] = { "effects", "locations", "events", "parts", "emitters", "particles", "physics", "decals",
	                                               "lights", "pctl", "pctl_types", "pctl_states", "pctl_pstates", "consts", "bitmaps",
	                                               "sequences", "sprites" };
	uint32_t tabofs[FXD_T_COUNT], tabcnt[FXD_T_COUNT];
	for (int t = 0; t < FXD_T_COUNT; t++) {
		tabofs[t] = rd32(raw + 16 + 8 * t);
		tabcnt[t] = rd32(raw + 20 + 8 * t);
		const struct { uint32_t off, cnt; } tries[] = { { tabofs[t] + 4, tabcnt[t] }, { (uint32_t)rawn + 16, tabcnt[t] }, { tabofs[t], 0xFFFFFFFFu },
		                                                { tabofs[t], (uint32_t)rawn }, { 0, tabcnt[t] }, { 0xFFFFFFF0u, tabcnt[t] },
		                                                { tabofs[t], (1u << 22) + 1u } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw, rawn);
			wr32(copy + 16 + 8 * t, tries[k].off);
			wr32(copy + 20 + 8 * t, tries[k].cnt);
			if (fxdef_open(&bad_s, copy, rawn) == FXD_OK) { fprintf(stderr, "table %s try %zu accepted\n", tabn[t], k); return 1; }
		}
		for (int d = -1; d <= 1; d += 2) {                                    /* merely wrong counts must never read out of bounds */
			memcpy(copy, raw, rawn);
			wr32(copy + 20 + 8 * t, tabcnt[t] + (uint32_t)d);
			if (fxdef_open(&bad_s, copy, rawn) == FXD_OK) walk(&bad_s);
		}
	}

	/* targeted fields: each of these must be refused */
#define REC(t, i, rs) (tabofs[t] + (uint32_t)(i) * (rs))
	const struct { const char *what; uint32_t at; uint32_t val; int bytes; } hit[] = {
		{ "effects out of order", REC(FXD_T_EFFECTS, 1, 48), ID(4), 4 }, { "effects equal ids", REC(FXD_T_EFFECTS, 2, 48), ID(5), 4 },
		{ "effect radius NaN", REC(FXD_T_EFFECTS, 0, 48) + 12, 0x7FC00000u, 4 },
		{ "effect locations out of range", REC(FXD_T_EFFECTS, 0, 48) + 16, 3, 4 }, { "effect locations count", REC(FXD_T_EFFECTS, 0, 48) + 20, 4, 2 },
		{ "effect events out of range", REC(FXD_T_EFFECTS, 0, 48) + 24, 4, 4 }, { "effect event count", REC(FXD_T_EFFECTS, 1, 48) + 22, 9, 2 },
		{ "location unterminated", REC(FXD_T_LOCATIONS, 1, 32) + 31, 'x', 1 },
		{ "event skip NaN", REC(FXD_T_EVENTS, 0, 48), 0x7FC00000u, 4 }, { "event delay inf", REC(FXD_T_EVENTS, 1, 48) + 4, 0x7F800000u, 4 },
		{ "event parts out of range", REC(FXD_T_EVENTS, 0, 48) + 20, 100, 4 }, { "event emitters out of range", REC(FXD_T_EVENTS, 0, 48) + 28, 100, 4 },
		{ "event part count", REC(FXD_T_EVENTS, 3, 48) + 24, 9, 2 },
		{ "part create in 4", REC(FXD_T_PARTS, 0, 80), 4, 1 }, { "part violence 3", REC(FXD_T_PARTS, 0, 80) + 1, 3, 1 },
		{ "part location past the effect's", REC(FXD_T_PARTS, 0, 80) + 2, 2, 2 }, { "part kind none", REC(FXD_T_PARTS, 0, 80) + 6, 0, 1 },
		{ "part kind 8", REC(FXD_T_PARTS, 0, 80) + 6, 8, 1 }, { "part pad", REC(FXD_T_PARTS, 0, 80) + 7, 1, 1 },
		{ "part group empty", REC(FXD_T_PARTS, 0, 80) + 12, 0, 1 }, { "part light index", REC(FXD_T_PARTS, 1, 80) + 16, 5, 4 },
		{ "part decal index", REC(FXD_T_PARTS, 4, 80) + 16, 2, 4 }, { "part velocity NaN", REC(FXD_T_PARTS, 0, 80) + 20, 0x7FC00000u, 4 },
		{ "emitter create in 4", REC(FXD_T_EMITTERS, 0, 128), 4, 1 }, { "emitter violence 3", REC(FXD_T_EMITTERS, 0, 128) + 1, 3, 1 },
		{ "emitter camera 4", REC(FXD_T_EMITTERS, 0, 128) + 2, 4, 1 }, { "emitter location past the effect's", REC(FXD_T_EMITTERS, 0, 128) + 4, 2, 2 },
		{ "emitter particle none", REC(FXD_T_EMITTERS, 0, 128) + 6, 0xFFFF, 2 }, { "emitter particle 2", REC(FXD_T_EMITTERS, 0, 128) + 6, 2, 2 },
		{ "emitter yaw NaN", REC(FXD_T_EMITTERS, 0, 128) + 8, 0x7FC00000u, 4 }, { "emitter distribution 6", REC(FXD_T_EMITTERS, 0, 128) + 32, 6, 2 },
		{ "emitter count negative", REC(FXD_T_EMITTERS, 0, 128) + 36, 0xFFFF, 2 }, { "emitter radius inf", REC(FXD_T_EMITTERS, 1, 128) + 68, 0x7F800000u, 4 },
		{ "particle bitmap 2", REC(FXD_T_PARTICLES, 0, 112) + 8, 2, 2 }, { "particle physics 1", REC(FXD_T_PARTICLES, 0, 112) + 10, 1, 2 },
		{ "particle lifespan NaN", REC(FXD_T_PARTICLES, 0, 112) + 20, 0x7FC00000u, 4 }, { "particle sprite size inf", REC(FXD_T_PARTICLES, 0, 112) + 76, 0x7F800000u, 4 },
		{ "particle orientation 3", REC(FXD_T_PARTICLES, 0, 112) + 80, 3, 1 }, { "particle blend 8", REC(FXD_T_PARTICLES, 0, 112) + 81, 8, 1 },
		{ "particle fade mode 3", REC(FXD_T_PARTICLES, 0, 112) + 82, 3, 1 }, { "particle shader flags 8", REC(FXD_T_PARTICLES, 0, 112) + 83, 8, 1 },
		{ "particle collision kind 8", REC(FXD_T_PARTICLES, 0, 112) + 84, 8, 1 }, { "particle death kind 8", REC(FXD_T_PARTICLES, 0, 112) + 85, 8, 1 },
		{ "particles out of order", REC(FXD_T_PARTICLES, 1, 112), ID(14), 4 },
		{ "physics density NaN", REC(FXD_T_PHYSICS, 0, 48) + 8, 0x7FC00000u, 4 }, { "physics gravity inf", REC(FXD_T_PHYSICS, 0, 48) + 28, 0x7F800000u, 4 },
		{ "decal type 4", REC(FXD_T_DECALS, 0, 96) + 6, 4, 1 }, { "decal layer 5", REC(FXD_T_DECALS, 0, 96) + 7, 5, 1 },
		{ "decal bitmap 2", REC(FXD_T_DECALS, 0, 96) + 12, 2, 2 }, { "decal blend 8", REC(FXD_T_DECALS, 0, 96) + 14, 8, 2 },
		{ "decal radius NaN", REC(FXD_T_DECALS, 0, 96) + 16, 0x7FC00000u, 4 }, { "decal extent NaN", REC(FXD_T_DECALS, 1, 96) + 76, 0x7FC00000u, 4 },
		{ "light radius NaN", REC(FXD_T_LIGHTS, 0, 80) + 8, 0x7FC00000u, 4 }, { "light colour inf", REC(FXD_T_LIGHTS, 0, 80) + 48, 0x7F800000u, 4 },
		{ "light duration NaN", REC(FXD_T_LIGHTS, 0, 80) + 64, 0x7FC00000u, 4 }, { "light falloff 16", REC(FXD_T_LIGHTS, 0, 80) + 68, 16, 2 },
		{ "pctl physics 1", REC(FXD_T_PCTL, 0, 32) + 4, 1, 2 }, { "pctl update physics 16", REC(FXD_T_PCTL, 0, 32) + 6, 16, 2 },
		{ "pctl types out of range", REC(FXD_T_PCTL, 0, 32) + 8, 1, 4 }, { "pctl type count", REC(FXD_T_PCTL, 0, 32) + 12, 2, 2 },
		{ "type name unterminated", REC(FXD_T_PCTL_TYPES, 0, 64) + 15, 'x', 1 }, { "type render mode 2", REC(FXD_T_PCTL_TYPES, 0, 64) + 22, 2, 1 },
		{ "type creation physics 3", REC(FXD_T_PCTL_TYPES, 0, 64) + 23, 3, 1 }, { "type radius NaN", REC(FXD_T_PCTL_TYPES, 0, 64) + 24, 0x7FC00000u, 4 },
		{ "type constants out of range", REC(FXD_T_PCTL_TYPES, 0, 64) + 28, 1, 4 }, { "type constant count", REC(FXD_T_PCTL_TYPES, 0, 64) + 32, 3, 2 },
		{ "type states out of range", REC(FXD_T_PCTL_TYPES, 0, 64) + 36, 1, 4 }, { "type state count", REC(FXD_T_PCTL_TYPES, 0, 64) + 40, 3, 2 },
		{ "type pstates out of range", REC(FXD_T_PCTL_TYPES, 0, 64) + 44, 1, 4 }, { "type pstate count", REC(FXD_T_PCTL_TYPES, 0, 64) + 48, 3, 2 },
		{ "state name unterminated", REC(FXD_T_PCTL_STATES, 0, 96) + 15, 'x', 1 }, { "state duration NaN", REC(FXD_T_PCTL_STATES, 0, 96) + 16, 0x7FC00000u, 4 },
		{ "state creation physics 16", REC(FXD_T_PCTL_STATES, 0, 96) + 72, 16, 2 }, { "state constants out of range", REC(FXD_T_PCTL_STATES, 0, 96) + 76, 3, 4 },
		{ "pstate name unterminated", REC(FXD_T_PCTL_PSTATES, 0, 112) + 15, 'x', 1 }, { "pstate bitmap 2", REC(FXD_T_PCTL_PSTATES, 0, 112) + 32, 2, 2 },
		{ "pstate scale NaN", REC(FXD_T_PCTL_PSTATES, 0, 112) + 36, 0x7FC00000u, 4 }, { "pstate colour inf", REC(FXD_T_PCTL_PSTATES, 1, 112) + 76, 0x7F800000u, 4 },
		{ "pstate physics 1", REC(FXD_T_PCTL_PSTATES, 0, 112) + 96, 1, 2 }, { "pstate blend 8", REC(FXD_T_PCTL_PSTATES, 0, 112) + 98, 8, 1 },
		{ "pstate fade 3", REC(FXD_T_PCTL_PSTATES, 0, 112) + 99, 3, 1 }, { "pstate shader flags 8", REC(FXD_T_PCTL_PSTATES, 0, 112) + 100, 8, 2 },
		{ "pstate constants out of range", REC(FXD_T_PCTL_PSTATES, 0, 112) + 102, 3, 4 },
		{ "constant NaN", REC(FXD_T_CONSTS, 0, 4), 0x7FC00000u, 4 },
		{ "bitmaps out of order", REC(FXD_T_BITMAPS, 1, 32), ID(17), 4 }, { "bitmap sequences out of range", REC(FXD_T_BITMAPS, 1, 32) + 4, 4, 4 },
		{ "bitmap sequence count", REC(FXD_T_BITMAPS, 0, 32) + 8, 5, 2 }, { "bitmap image count 1", REC(FXD_T_BITMAPS, 0, 32) + 14, 1, 2 },
		{ "sequence sprites out of range", REC(FXD_T_SEQUENCES, 0, 8), 5, 4 }, { "sequence sprite count", REC(FXD_T_SEQUENCES, 3, 8) + 4, 9, 2 },
		{ "sprite u0 NaN", REC(FXD_T_SPRITES, 0, 32) + 4, 0x7FC00000u, 4 }, { "sprite u0 above 1", REC(FXD_T_SPRITES, 0, 32) + 4, 0x40000000u, 4 },
		{ "sprite v1 negative", REC(FXD_T_SPRITES, 1, 32) + 16, 0xBF800000u, 4 }, { "sprite image 2", REC(FXD_T_SPRITES, 0, 32) + 2, 2, 2 },
		{ "sprite reg inf", REC(FXD_T_SPRITES, 1, 32) + 20, 0x7F800000u, 4 },
		{ "header size", 8, 1, 4 } };
	for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
		if (hit[k].bytes == 0) continue;                                   /* placeholder row */
		memcpy(copy, raw, rawn);
		if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
		else if (hit[k].bytes == 2) wr16(copy + hit[k].at, hit[k].val);
		else copy[hit[k].at] = (uint8_t)hit[k].val;
		if (fxdef_open(&bad_s, copy, rawn) == FXD_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
	}

	/* no single-byte change anywhere may let an out-of-bounds read through (both sections) */
	for (size_t at = 4; at < rawn; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (fxdef_open(&bad_s, copy, rawn) == FXD_OK) walk(&bad_s);
		}
	for (size_t cut = 0; cut < rawn1; cut++) CHECK(fxdef_open(&bad_s, raw1, cut) != FXD_OK);
	for (size_t at = 4; at < rawn1; at++) {
		uint8_t *c1 = malloc(rawn1);
		CHECK(c1);
		memcpy(c1, raw1, rawn1);
		c1[at] ^= 0xFF;
		if (fxdef_open(&bad_s, c1, rawn1) == FXD_OK) walk(&bad_s);
		free(c1);
	}
	free(copy);
	free(raw);
	free(raw1);
	free(rawt);
	free(img);
	puts("ok");
	return 0;
}
