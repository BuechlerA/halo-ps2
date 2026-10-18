/* FXD0 section reader (docs/22-fx-section.md). Pure C11, no heap, no double. fxdef_open validates every table once
 * (sizes, alignment, ranges, sort order, cross references, enums, string terminators, finite floats), so the accessors
 * can never read out of bounds, whatever the bytes. Records are decoded with byte loads: no alignment assumptions on
 * the section buffer. */
#include "world/fxdef.h"
#include <string.h>

#define HEADER_SIZE 192u
#define FXD_VERSION 1u
#define MAX_RECORDS (1u << 22)

/* record sizes in table order (fxconv.REC_SIZE) */
static const uint8_t rec_size[FXD_T_COUNT] = { 48, 32, 48, 80, 128, 112, 48, 96, 80, 32, 64, 96, 112, 4, 32, 8, 32 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }
static const uint8_t *rec(const fxdef_section *s, int t, uint32_t i) { return s->table[t] + (size_t)i * rec_size[t]; }
static int in_range(const fxdef_section *s, int t, uint32_t i) { return i < s->count[t]; }

/* all `n` floats starting at p are finite */
static int finite_run(const uint8_t *p, unsigned n)
{
	unsigned k;
	for (k = 0; k < n; k++)
		if (!finite_at(p + 4 * k)) return 0;
	return 1;
}

/* a range [first, first + count) inside table t */
static int range_ok(const fxdef_section *s, int t, uint32_t first, uint32_t count)
{
	return (uint64_t)first + count <= s->count[t];
}
static int idx16_ok(const fxdef_section *s, int t, uint32_t v) { return v == FXD_NONE || v < s->count[t]; }

static int sorted_ids(const fxdef_section *s, int t)
{
	uint32_t i, prev = 0;
	for (i = 0; i < s->count[t]; i++) {
		uint32_t id = rd32(rec(s, t, i));
		if (i && id <= prev) return 0;
		prev = id;
	}
	return 1;
}

static int find_id(const fxdef_section *s, int t, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->count[t];
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, v = rd32(rec(s, t, mid));
		if (v == tag_id) return (int)mid;
		if (v < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

/* ---- validation of the individual tables ------------------------------------------------------------------------ */
static int check_effects(const fxdef_section *s)
{
	uint32_t i, e, k;
	for (i = 0; i < s->count[FXD_T_EFFECTS]; i++) {
		const uint8_t *r = rec(s, FXD_T_EFFECTS, i);
		uint32_t nloc = rd16(r + 20), nev = rd16(r + 22), fev = rd32(r + 24);
		if (!finite_at(r + 12)) return 0;
		if (!range_ok(s, FXD_T_LOCATIONS, rd32(r + 16), nloc)) return 0;
		if (!range_ok(s, FXD_T_EVENTS, fev, nev)) return 0;
		for (e = 0; e < nev; e++) {
			const uint8_t *ev = rec(s, FXD_T_EVENTS, fev + e);
			uint32_t np = rd16(ev + 24), nc = rd16(ev + 26), fp = rd32(ev + 20), fc = rd32(ev + 28);
			if (!range_ok(s, FXD_T_PARTS, fp, np) || !range_ok(s, FXD_T_EMITTERS, fc, nc)) return 0;
			for (k = 0; k < np; k++) {
				uint32_t loc = rd16(rec(s, FXD_T_PARTS, fp + k) + 2);
				if (loc != FXD_NONE && loc >= nloc) return 0;
			}
			for (k = 0; k < nc; k++) {
				uint32_t loc = rd16(rec(s, FXD_T_EMITTERS, fc + k) + 4);
				if (loc != FXD_NONE && loc >= nloc) return 0;
			}
		}
	}
	return 1;
}

static int check_tables(const fxdef_section *s)
{
	uint32_t i, k;
	static const int sorted[] = { FXD_T_EFFECTS, FXD_T_PARTICLES, FXD_T_PHYSICS, FXD_T_DECALS, FXD_T_LIGHTS, FXD_T_PCTL, FXD_T_BITMAPS };

	for (i = 0; i < sizeof sorted / sizeof sorted[0]; i++)
		if (!sorted_ids(s, sorted[i])) return 0;
	for (i = 0; i < s->count[FXD_T_LOCATIONS]; i++)
		if (rec(s, FXD_T_LOCATIONS, i)[FXD_NAME_LEN - 1] != 0) return 0;
	if (!check_effects(s)) return 0;
	for (i = 0; i < s->count[FXD_T_EVENTS]; i++) {            /* event ranges, whether or not an effect lists them */
		const uint8_t *ev = rec(s, FXD_T_EVENTS, i);
		if (!finite_run(ev, 5)) return 0;
		if (!range_ok(s, FXD_T_PARTS, rd32(ev + 20), rd16(ev + 24)) || !range_ok(s, FXD_T_EMITTERS, rd32(ev + 28), rd16(ev + 26))) return 0;
	}
	for (i = 0; i < s->count[FXD_T_PARTS]; i++) {
		const uint8_t *r = rec(s, FXD_T_PARTS, i);
		uint8_t kind = r[6];
		static const int table_of_kind[8] = { -1, -1, -1, FXD_T_LIGHTS, -1, FXD_T_DECALS, FXD_T_PCTL, -1 };
		if (r[0] > FXD_ENV_SPACE || r[1] > FXD_VIOLENCE_NONVIOLENT || kind > FXD_K_EFFECT || kind == FXD_K_NONE || r[7] != 0) return 0;
		if (r[12] == 0) return 0;                                          /* group must not be empty */
		if (table_of_kind[kind] >= 0 && rd32(r + 16) >= s->count[table_of_kind[kind]]) return 0;
		if (!finite_run(r + 20, 7)) return 0;
	}
	for (i = 0; i < s->count[FXD_T_EMITTERS]; i++) {
		const uint8_t *r = rec(s, FXD_T_EMITTERS, i);
		if (r[0] > FXD_ENV_SPACE || r[1] > FXD_VIOLENCE_NONVIOLENT || r[2] > FXD_CAM_FIRST_IF_POSSIBLE) return 0;
		if (!idx16_ok(s, FXD_T_PARTICLES, rd16(r + 6)) || rd16(r + 6) == FXD_NONE) return 0;
		if (rd32(r + 92) != rd32(rec(s, FXD_T_PARTICLES, rd16(r + 6)))) return 0;       /* tag id matches the particle index */
		if (rd16(r + 32) > FXD_DIST_BUILDUP_FALLOFF) return 0;
		if (rds16(r + 36) < 0 || rds16(r + 38) < 0) return 0;
		if (!finite_run(r + 8, 5) || !finite_run(r + 40, 9)) return 0;
	}
	for (i = 0; i < s->count[FXD_T_PARTICLES]; i++) {
		const uint8_t *r = rec(s, FXD_T_PARTICLES, i);
		if (!idx16_ok(s, FXD_T_BITMAPS, rd16(r + 8)) || !idx16_ok(s, FXD_T_PHYSICS, rd16(r + 10))) return 0;
		if (!finite_run(r + 20, 12) || !finite_at(r + 76)) return 0;
		if (r[80] > FXD_ORIENT_PERPENDICULAR || r[81] > FXD_BLEND_ALPHA_MULTIPLY_ADD || r[82] > FXD_FADE_PARALLEL || r[83] > 7) return 0;
		if (r[84] > FXD_K_EFFECT || r[85] > FXD_K_EFFECT) return 0;
	}
	for (i = 0; i < s->count[FXD_T_PHYSICS]; i++)
		if (!finite_run(rec(s, FXD_T_PHYSICS, i) + 8, 7)) return 0;
	for (i = 0; i < s->count[FXD_T_DECALS]; i++) {
		const uint8_t *r = rec(s, FXD_T_DECALS, i);
		if (r[6] > FXD_DECAL_PAINTED_SIGN || r[7] > FXD_LAYER_WATER || !idx16_ok(s, FXD_T_BITMAPS, rd16(r + 12)) || rd16(r + 14) > FXD_BLEND_ALPHA_MULTIPLY_ADD) return 0;
		if (!finite_run(r + 16, 14) || !finite_at(r + 76)) return 0;
	}
	for (i = 0; i < s->count[FXD_T_LIGHTS]; i++) {
		const uint8_t *r = rec(s, FXD_T_LIGHTS, i);
		if (!finite_run(r + 8, 5) || !finite_run(r + 32, 8) || !finite_at(r + 64) || rd16(r + 68) > 15) return 0;
	}
	for (i = 0; i < s->count[FXD_T_PCTL]; i++) {
		const uint8_t *r = rec(s, FXD_T_PCTL, i);
		if (!idx16_ok(s, FXD_T_PHYSICS, rd16(r + 4)) || rd16(r + 6) > 15) return 0;
		if (!range_ok(s, FXD_T_PCTL_TYPES, rd32(r + 8), rd16(r + 12))) return 0;
	}
	for (i = 0; i < s->count[FXD_T_PCTL_TYPES]; i++) {
		const uint8_t *r = rec(s, FXD_T_PCTL_TYPES, i);
		if (r[FXD_SHORT_NAME - 1] != 0 || r[22] > FXD_PCTL_ROTATIONAL || r[23] > FXD_PCTL_PHYS_JET || !finite_at(r + 24)) return 0;
		if (!range_ok(s, FXD_T_CONSTS, rd32(r + 28), rd16(r + 32)) || !range_ok(s, FXD_T_PCTL_STATES, rd32(r + 36), rd16(r + 40)) ||
		    !range_ok(s, FXD_T_PCTL_PSTATES, rd32(r + 44), rd16(r + 48)))
			return 0;
	}
	for (i = 0; i < s->count[FXD_T_PCTL_STATES]; i++) {
		const uint8_t *r = rec(s, FXD_T_PCTL_STATES, i);
		if (r[FXD_SHORT_NAME - 1] != 0 || !finite_run(r + 16, 14) || rd16(r + 72) > 15 || rd16(r + 74) > 15) return 0;
		if (!range_ok(s, FXD_T_CONSTS, rd32(r + 76), rd16(r + 80))) return 0;
	}
	for (i = 0; i < s->count[FXD_T_PCTL_PSTATES]; i++) {
		const uint8_t *r = rec(s, FXD_T_PCTL_PSTATES, i);
		if (r[FXD_SHORT_NAME - 1] != 0 || !finite_run(r + 16, 4) || !finite_run(r + 36, 15)) return 0;
		if (!idx16_ok(s, FXD_T_BITMAPS, rd16(r + 32)) || !idx16_ok(s, FXD_T_PHYSICS, rd16(r + 96))) return 0;
		if (r[98] > FXD_BLEND_ALPHA_MULTIPLY_ADD || r[99] > FXD_FADE_PARALLEL || rd16(r + 100) > 7) return 0;
		if (!range_ok(s, FXD_T_CONSTS, rd32(r + 102), rd16(r + 106))) return 0;
	}
	for (i = 0; i < s->count[FXD_T_CONSTS]; i++)
		if (!finite_at(rec(s, FXD_T_CONSTS, i))) return 0;
	for (i = 0; i < s->count[FXD_T_BITMAPS]; i++) {
		const uint8_t *r = rec(s, FXD_T_BITMAPS, i);
		uint32_t first = rd32(r + 4), n = rd16(r + 8), images = rd16(r + 14);
		if (!range_ok(s, FXD_T_SEQUENCES, first, n)) return 0;
		for (k = 0; k < n; k++) {
			const uint8_t *q = rec(s, FXD_T_SEQUENCES, first + k);
			uint32_t j, ns = rd16(q + 4);
			if (!range_ok(s, FXD_T_SPRITES, rd32(q), ns)) return 0;
			for (j = 0; j < ns; j++)
				if (rd16(rec(s, FXD_T_SPRITES, rd32(q) + j) + 2) >= images) return 0;
		}
	}
	for (i = 0; i < s->count[FXD_T_SEQUENCES]; i++) {
		const uint8_t *q = rec(s, FXD_T_SEQUENCES, i);
		if (!range_ok(s, FXD_T_SPRITES, rd32(q), rd16(q + 4))) return 0;
	}
	for (i = 0; i < s->count[FXD_T_SPRITES]; i++) {
		const uint8_t *r = rec(s, FXD_T_SPRITES, i);
		unsigned c;
		if (!finite_run(r + 4, 6)) return 0;
		for (c = 0; c < 4; c++) {
			float v = rdf(r + 4 + 4 * c);
			if (!(v >= 0.0f && v <= 1.0f)) return 0;
		}
	}
	return 1;
}

fxdef_status fxdef_open(fxdef_section *s, const uint8_t *data, size_t size)
{
	int t;
	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return FXD_ERR_TRUNCATED;
	if (memcmp(data, "FXD0", 4) != 0) return FXD_ERR_MAGIC;
	if (rd16(data + 4) != FXD_VERSION) return FXD_ERR_VERSION;
	if (rd32(data + 8) > size) return FXD_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return FXD_ERR_LAYOUT;
	for (t = 0; t < FXD_T_COUNT; t++) {
		uint32_t off = rd32(data + 16 + 8 * t), cnt = rd32(data + 20 + 8 * t);
		if (cnt > MAX_RECORDS || off % 16u || off < HEADER_SIZE || off > size) return FXD_ERR_LAYOUT;
		if ((uint64_t)cnt * rec_size[t] > (uint64_t)size - off) return FXD_ERR_LAYOUT;
		s->count[t] = cnt;
		s->table[t] = data + off;
	}
	s->data = data;
	s->size = size;
	if (!check_tables(s)) {
		memset(s, 0, sizeof *s);
		return FXD_ERR_LAYOUT;
	}
	return FXD_OK;
}

/* ---- accessors -------------------------------------------------------------------------------------------------- */
int fxdef_find_effect(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_EFFECTS, tag_id); }
int fxdef_find_particle(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_PARTICLES, tag_id); }
int fxdef_find_physics(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_PHYSICS, tag_id); }
int fxdef_find_decal(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_DECALS, tag_id); }
int fxdef_find_light(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_LIGHTS, tag_id); }
int fxdef_find_pctl(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_PCTL, tag_id); }
int fxdef_find_bitmap(const fxdef_section *s, uint32_t tag_id) { return find_id(s, FXD_T_BITMAPS, tag_id); }

fxd_effect fxdef_get_effect(const fxdef_section *s, uint32_t index)
{
	fxd_effect e;
	memset(&e, 0, sizeof e);
	e.loop_start = e.loop_stop = -1;
	if (in_range(s, FXD_T_EFFECTS, index)) {
		const uint8_t *r = rec(s, FXD_T_EFFECTS, index);
		e.tag_id = rd32(r);
		e.flags = rd32(r + 4);
		e.loop_start = rds16(r + 8);
		e.loop_stop = rds16(r + 10);
		e.damage_radius = rdf(r + 12);
		e.first_location = rd32(r + 16);
		e.location_count = rd16(r + 20);
		e.event_count = rd16(r + 22);
		e.first_event = rd32(r + 24);
	}
	return e;
}

fxd_event fxdef_get_event(const fxdef_section *s, const fxd_effect *e, uint32_t i)
{
	fxd_event v;
	memset(&v, 0, sizeof v);
	if (i < e->event_count && in_range(s, FXD_T_EVENTS, e->first_event + i)) {
		const uint8_t *r = rec(s, FXD_T_EVENTS, e->first_event + i);
		v.skip = rdf(r);
		v.delay[0] = rdf(r + 4);
		v.delay[1] = rdf(r + 8);
		v.duration[0] = rdf(r + 12);
		v.duration[1] = rdf(r + 16);
		v.first_part = rd32(r + 20);
		v.part_count = rd16(r + 24);
		v.emitter_count = rd16(r + 26);
		v.first_emitter = rd32(r + 28);
	}
	return v;
}

fxd_part fxdef_get_part(const fxdef_section *s, const fxd_event *ev, uint32_t i)
{
	fxd_part p;
	memset(&p, 0, sizeof p);
	p.location = FXD_NONE;
	p.index = FXD_NO_TAG;
	if (i < ev->part_count && in_range(s, FXD_T_PARTS, ev->first_part + i)) {
		const uint8_t *r = rec(s, FXD_T_PARTS, ev->first_part + i);
		p.create_in = r[0];
		p.violence = r[1];
		p.location = rd16(r + 2);
		p.flags = rd16(r + 4);
		p.kind = r[6];
		p.tag_id = rd32(r + 8);
		memcpy(p.group, r + 12, 4);
		p.group[4] = 0;
		p.index = rd32(r + 16);
		p.velocity[0] = rdf(r + 20);
		p.velocity[1] = rdf(r + 24);
		p.cone = rdf(r + 28);
		p.angvel[0] = rdf(r + 32);
		p.angvel[1] = rdf(r + 36);
		p.radius_mod[0] = rdf(r + 40);
		p.radius_mod[1] = rdf(r + 44);
		p.a_scales = rd32(r + 48);
		p.b_scales = rd32(r + 52);
	}
	return p;
}

fxd_emitter fxdef_get_emitter(const fxdef_section *s, const fxd_event *ev, uint32_t i)
{
	fxd_emitter c;
	unsigned k;
	memset(&c, 0, sizeof c);
	c.location = c.particle = FXD_NONE;
	if (i < ev->emitter_count && in_range(s, FXD_T_EMITTERS, ev->first_emitter + i)) {
		const uint8_t *r = rec(s, FXD_T_EMITTERS, ev->first_emitter + i);
		c.create_in = r[0];
		c.violence = r[1];
		c.create = r[2];
		c.location = rd16(r + 4);
		c.particle = rd16(r + 6);
		c.yaw = rdf(r + 8);
		c.pitch = rdf(r + 12);
		for (k = 0; k < 3; k++) c.offset[k] = rdf(r + 16 + 4 * k);
		c.flags = rd32(r + 28);
		c.distribution = rd16(r + 32);
		c.count[0] = rds16(r + 36);
		c.count[1] = rds16(r + 38);
		c.dist_radius[0] = rdf(r + 40);
		c.dist_radius[1] = rdf(r + 44);
		c.velocity[0] = rdf(r + 48);
		c.velocity[1] = rdf(r + 52);
		c.cone = rdf(r + 56);
		c.angvel[0] = rdf(r + 60);
		c.angvel[1] = rdf(r + 64);
		c.radius[0] = rdf(r + 68);
		c.radius[1] = rdf(r + 72);
		for (k = 0; k < 4; k++) {
			c.tint_lo[k] = r[76 + k];
			c.tint_hi[k] = r[80 + k];
		}
		c.a_scales = rd32(r + 84);
		c.b_scales = rd32(r + 88);
		c.particle_tag = rd32(r + 92);
	}
	return c;
}

int fxdef_location_name(const fxdef_section *s, const fxd_effect *e, uint32_t i, char out[FXD_NAME_LEN])
{
	out[0] = 0;
	if (i >= e->location_count || !in_range(s, FXD_T_LOCATIONS, e->first_location + i)) return 0;
	memcpy(out, rec(s, FXD_T_LOCATIONS, e->first_location + i), FXD_NAME_LEN);
	out[FXD_NAME_LEN - 1] = 0;
	return 1;
}

fxd_particle fxdef_get_particle(const fxdef_section *s, uint32_t index)
{
	fxd_particle p;
	memset(&p, 0, sizeof p);
	p.bitmap = p.physics = FXD_NONE;
	p.collision_effect = p.death_effect = FXD_NO_TAG;
	if (in_range(s, FXD_T_PARTICLES, index)) {
		const uint8_t *r = rec(s, FXD_T_PARTICLES, index);
		p.tag_id = rd32(r);
		p.flags = rd32(r + 4);
		p.bitmap = rd16(r + 8);
		p.physics = rd16(r + 10);
		p.collision_effect = rd32(r + 12);
		p.death_effect = rd32(r + 16);
		p.lifespan[0] = rdf(r + 20);
		p.lifespan[1] = rdf(r + 24);
		p.fade_in = rdf(r + 28);
		p.fade_out = rdf(r + 32);
		p.min_size = rdf(r + 36);
		p.radius_anim[0] = rdf(r + 40);
		p.radius_anim[1] = rdf(r + 44);
		p.anim_rate[0] = rdf(r + 48);
		p.anim_rate[1] = rdf(r + 52);
		p.contact_deterioration = rdf(r + 56);
		p.fade_start_size = rdf(r + 60);
		p.fade_end_size = rdf(r + 64);
		p.seq_first = rds16(r + 68);
		p.seq_initial = rds16(r + 70);
		p.seq_loop = rds16(r + 72);
		p.seq_final = rds16(r + 74);
		p.sprite_size = rdf(r + 76);
		p.orientation = r[80];
		p.blend = r[81];
		p.fade_mode = r[82];
		p.shader_flags = r[83];
		p.collision_kind = r[84];
		p.death_kind = r[85];
	}
	return p;
}

fxd_physics fxdef_get_physics(const fxdef_section *s, uint32_t index)
{
	fxd_physics p;
	memset(&p, 0, sizeof p);
	if (in_range(s, FXD_T_PHYSICS, index)) {
		const uint8_t *r = rec(s, FXD_T_PHYSICS, index);
		p.tag_id = rd32(r);
		p.flags = rd32(r + 4);
		p.density = rdf(r + 8);
		p.air_friction = rdf(r + 12);
		p.water_friction = rdf(r + 16);
		p.surface_friction = rdf(r + 20);
		p.elasticity = rdf(r + 24);
		p.air_gravity_scale = rdf(r + 28);
		p.water_gravity_scale = rdf(r + 32);
	}
	return p;
}

fxd_decal fxdef_get_decal(const fxdef_section *s, uint32_t index)
{
	fxd_decal d;
	unsigned k;
	memset(&d, 0, sizeof d);
	d.bitmap = FXD_NONE;
	d.next_tag = FXD_NO_TAG;
	if (in_range(s, FXD_T_DECALS, index)) {
		const uint8_t *r = rec(s, FXD_T_DECALS, index);
		d.tag_id = rd32(r);
		d.flags = rd16(r + 4);
		d.type = r[6];
		d.layer = r[7];
		d.next_tag = rd32(r + 8);
		d.bitmap = rd16(r + 12);
		d.blend = rd16(r + 14);
		d.radius[0] = rdf(r + 16);
		d.radius[1] = rdf(r + 20);
		d.intensity[0] = rdf(r + 24);
		d.intensity[1] = rdf(r + 28);
		for (k = 0; k < 3; k++) {
			d.color_lo[k] = rdf(r + 32 + 4 * k);
			d.color_hi[k] = rdf(r + 44 + 4 * k);
		}
		d.lifetime[0] = rdf(r + 56);
		d.lifetime[1] = rdf(r + 60);
		d.decay[0] = rdf(r + 64);
		d.decay[1] = rdf(r + 68);
		d.loop_frame = rds16(r + 72);
		d.anim_speed = rds16(r + 74);
		d.max_extent = rdf(r + 76);
	}
	return d;
}

fxd_light fxdef_get_light(const fxdef_section *s, uint32_t index)
{
	fxd_light l;
	unsigned k;
	memset(&l, 0, sizeof l);
	if (in_range(s, FXD_T_LIGHTS, index)) {
		const uint8_t *r = rec(s, FXD_T_LIGHTS, index);
		l.tag_id = rd32(r);
		l.flags = rd32(r + 4);
		l.radius = rdf(r + 8);
		l.radius_mod[0] = rdf(r + 12);
		l.radius_mod[1] = rdf(r + 16);
		l.falloff_angle = rdf(r + 20);
		l.cutoff_angle = rdf(r + 24);
		l.interp_flags = rd32(r + 28);
		for (k = 0; k < 4; k++) {
			l.color_lo[k] = rdf(r + 32 + 4 * k);
			l.color_hi[k] = rdf(r + 48 + 4 * k);
		}
		l.duration = rdf(r + 64);
		l.falloff_fn = rd16(r + 68);
	}
	return l;
}

fxd_pctl fxdef_get_pctl(const fxdef_section *s, uint32_t index)
{
	fxd_pctl p;
	memset(&p, 0, sizeof p);
	p.physics = FXD_NONE;
	if (in_range(s, FXD_T_PCTL, index)) {
		const uint8_t *r = rec(s, FXD_T_PCTL, index);
		p.tag_id = rd32(r);
		p.physics = rd16(r + 4);
		p.update_physics = rd16(r + 6);
		p.first_type = rd32(r + 8);
		p.type_count = rd16(r + 12);
	}
	return p;
}

fxd_pctl_type fxdef_get_pctl_type(const fxdef_section *s, const fxd_pctl *p, uint32_t i)
{
	fxd_pctl_type t;
	memset(&t, 0, sizeof t);
	if (i < p->type_count && in_range(s, FXD_T_PCTL_TYPES, p->first_type + i)) {
		const uint8_t *r = rec(s, FXD_T_PCTL_TYPES, p->first_type + i);
		memcpy(t.name, r, FXD_SHORT_NAME);
		t.name[FXD_SHORT_NAME - 1] = 0;
		t.flags = rd32(r + 16);
		t.initial_count = rd16(r + 20);
		t.render_mode = r[22];
		t.creation_physics = r[23];
		t.radius = rdf(r + 24);
		t.first_const = rd32(r + 28);
		t.const_count = rd16(r + 32);
		t.first_state = rd32(r + 36);
		t.state_count = rd16(r + 40);
		t.first_pstate = rd32(r + 44);
		t.pstate_count = rd16(r + 48);
	}
	return t;
}

fxd_pctl_state fxdef_get_pctl_state(const fxdef_section *s, const fxd_pctl_type *t, uint32_t i)
{
	fxd_pctl_state v;
	unsigned k;
	memset(&v, 0, sizeof v);
	if (i < t->state_count && in_range(s, FXD_T_PCTL_STATES, t->first_state + i)) {
		const uint8_t *r = rec(s, FXD_T_PCTL_STATES, t->first_state + i);
		memcpy(v.name, r, FXD_SHORT_NAME);
		v.name[FXD_SHORT_NAME - 1] = 0;
		v.duration[0] = rdf(r + 16);
		v.duration[1] = rdf(r + 20);
		v.transition[0] = rdf(r + 24);
		v.transition[1] = rdf(r + 28);
		v.scale_mult = rdf(r + 32);
		v.anim_mult = rdf(r + 36);
		v.rot_mult = rdf(r + 40);
		for (k = 0; k < 4; k++) v.color_mult[k] = rdf(r + 44 + 4 * k);
		v.radius_mult = rdf(r + 60);
		v.min_count = rdf(r + 64);
		v.creation_rate = rdf(r + 68);
		v.creation_physics = rd16(r + 72);
		v.update_physics = rd16(r + 74);
		v.first_const = rd32(r + 76);
		v.const_count = rd16(r + 80);
	}
	return v;
}

fxd_pctl_pstate fxdef_get_pctl_pstate(const fxdef_section *s, const fxd_pctl_type *t, uint32_t i)
{
	fxd_pctl_pstate v;
	unsigned k;
	memset(&v, 0, sizeof v);
	v.bitmap = v.physics = FXD_NONE;
	if (i < t->pstate_count && in_range(s, FXD_T_PCTL_PSTATES, t->first_pstate + i)) {
		const uint8_t *r = rec(s, FXD_T_PCTL_PSTATES, t->first_pstate + i);
		memcpy(v.name, r, FXD_SHORT_NAME);
		v.name[FXD_SHORT_NAME - 1] = 0;
		v.duration[0] = rdf(r + 16);
		v.duration[1] = rdf(r + 20);
		v.transition[0] = rdf(r + 24);
		v.transition[1] = rdf(r + 28);
		v.bitmap = rd16(r + 32);
		v.sequence = rd16(r + 34);
		for (k = 0; k < 2; k++) {
			v.scale[k] = rdf(r + 36 + 4 * k);
			v.anim_rate[k] = rdf(r + 44 + 4 * k);
			v.rot_rate[k] = rdf(r + 52 + 4 * k);
		}
		for (k = 0; k < 4; k++) {
			v.color1[k] = rdf(r + 60 + 4 * k);
			v.color2[k] = rdf(r + 76 + 4 * k);
		}
		v.radius_mult = rdf(r + 92);
		v.physics = rd16(r + 96);
		v.blend = r[98];
		v.fade_mode = r[99];
		v.shader_flags = rd16(r + 100);
		v.first_const = rd32(r + 102);
		v.const_count = rd16(r + 106);
	}
	return v;
}

float fxdef_const(const fxdef_section *s, uint32_t index)
{
	return in_range(s, FXD_T_CONSTS, index) ? rdf(rec(s, FXD_T_CONSTS, index)) : 0.0f;
}

fxd_bitmap fxdef_get_bitmap(const fxdef_section *s, uint32_t index)
{
	fxd_bitmap b;
	memset(&b, 0, sizeof b);
	b.texture = FXD_NONE;
	if (in_range(s, FXD_T_BITMAPS, index)) {
		const uint8_t *r = rec(s, FXD_T_BITMAPS, index);
		b.tag_id = rd32(r);
		b.first_sequence = rd32(r + 4);
		b.sequence_count = rd16(r + 8);
		b.kind = r[10];
		b.texture = rd16(r + 12);
		b.image_count = rd16(r + 14);
	}
	return b;
}

fxd_sequence fxdef_get_sequence(const fxdef_section *s, const fxd_bitmap *b, uint32_t i)
{
	fxd_sequence q;
	memset(&q, 0, sizeof q);
	if (i < b->sequence_count && in_range(s, FXD_T_SEQUENCES, b->first_sequence + i)) {
		const uint8_t *r = rec(s, FXD_T_SEQUENCES, b->first_sequence + i);
		q.first_sprite = rd32(r);
		q.sprite_count = rd16(r + 4);
	}
	return q;
}

fxd_sprite fxdef_get_sprite(const fxdef_section *s, const fxd_sequence *q, uint32_t i)
{
	fxd_sprite v;
	memset(&v, 0, sizeof v);
	v.texture = FXD_NONE;
	if (i < q->sprite_count && in_range(s, FXD_T_SPRITES, q->first_sprite + i)) {
		const uint8_t *r = rec(s, FXD_T_SPRITES, q->first_sprite + i);
		v.texture = rd16(r);
		v.image = rd16(r + 2);
		v.u0 = rdf(r + 4);
		v.v0 = rdf(r + 8);
		v.u1 = rdf(r + 12);
		v.v1 = rdf(r + 16);
		v.reg[0] = rdf(r + 20);
		v.reg[1] = rdf(r + 24);
	}
	return v;
}

int fxdef_sprite(const fxdef_section *s, uint32_t bitmap, uint32_t sequence, uint32_t frame, fxd_sprite *out)
{
	fxd_bitmap b = fxdef_get_bitmap(s, bitmap);
	uint32_t seq, k;
	memset(out, 0, sizeof *out);
	out->texture = FXD_NONE;
	if (!in_range(s, FXD_T_BITMAPS, bitmap)) return 0;
	/* the last non-empty sequence at or before the requested one, else the first non-empty one */
	seq = sequence < b.sequence_count ? sequence : (b.sequence_count ? (uint32_t)b.sequence_count - 1u : 0u);
	for (k = seq + 1; k-- > 0;) {
		fxd_sequence q = fxdef_get_sequence(s, &b, k);
		if (q.sprite_count) {
			*out = fxdef_get_sprite(s, &q, frame % q.sprite_count);
			return 1;
		}
	}
	for (k = seq + 1; k < b.sequence_count; k++) {
		fxd_sequence q = fxdef_get_sequence(s, &b, k);
		if (q.sprite_count) {
			*out = fxdef_get_sprite(s, &q, frame % q.sprite_count);
			return 1;
		}
	}
	return 0;
}

int fxdef_bitmap_binding(const tex_section *fxtx, uint32_t bitmap_tag, tex_binding *out)
{
	return tex_find_binding(fxtx, bitmap_tag, out);
}

int fxdef_check_textures(const fxdef_section *s, uint32_t texture_count)
{
	uint32_t i;
	for (i = 0; i < s->count[FXD_T_SPRITES]; i++) {
		uint32_t t = rd16(rec(s, FXD_T_SPRITES, i));
		if (t != FXD_NONE && t >= texture_count) return 0;
	}
	for (i = 0; i < s->count[FXD_T_BITMAPS]; i++) {
		uint32_t t = rd16(rec(s, FXD_T_BITMAPS, i) + 12);
		if (t != FXD_NONE && t >= texture_count) return 0;
	}
	return 1;
}
