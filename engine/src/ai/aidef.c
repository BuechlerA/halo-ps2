/* AIDT section reader (docs/25-ai-section.md). Pure C11, no heap, no double. aidef_open validates every table once (sizes,
 * alignment, ranges, sort order, indices, parent / child ranges, string terminators, finite floats), so the accessors can never
 * read out of bounds, whatever the bytes. Records are decoded with byte loads: no alignment assumptions on the section buffer. */
#include "ai/aidef.h"
#include <string.h>

#define HEADER_SIZE 256u
#define AIDEF_VERSION 1u
#define DIR_OFFSET 16u

/* record sizes (tools/halo2ps2/aiconv.py TABLES) and maximum counts */
static const struct { uint32_t rec, max; } TAB[AIDEF_TABLES] = {
	{ 1272, 255 },  { 568, 1024 },  { 4, 255 },     { 4, 1024 },    { 8, 255 },     { 80, 1024 },   { 112, 16384 },
	{ 28, 65536 },  { 40, 65536 },  { 20, 65536 },  { 48, 8192 },   { 32, 4096 },   { 64, 1024 },   { 32, 65536 },
	{ 20, 65536 },  { 64, 512 },    { 64, 4096 },   { 48, 8192 },   { 48, 256 },    { 32, 256 },    { 4, 1024 },
	{ 4, 1024 },    { 4, 512 }
};

/* squad record */
#define SQ_NAME 0
#define SQ_ACTOR_TYPE 32
#define SQ_STATES 36
#define SQ_FLAGS 40
#define SQ_LEADER 44
#define SQ_DELAY 48
#define SQ_MASKS 52
#define SQ_COUNTS 80
#define SQ_FIRST_START 92

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }
static int terminated(const uint8_t *p, unsigned n) { return p[n - 1] == 0; }

static int finites(const uint8_t *rec, unsigned from, unsigned to)
{
	unsigned o;
	for (o = from; o < to; o += 4)
		if (!finite_at(rec + o)) return 0;
	return 1;
}

static void load_str(char *dst, const uint8_t *p)
{
	memcpy(dst, p, AIDEF_NAME_LEN);
	dst[AIDEF_NAME_LEN - 1] = 0;
}

static void load_floats(float *dst, const uint8_t *p, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) dst[i] = rdf(p + 4 * i);
}

static const uint8_t *rec_at(const aidef_section *s, aidef_table t, uint32_t i)
{
	return s->tab[t] + (size_t)i * TAB[t].rec;
}

/* ---- validation ---------------------------------------------------------------------------------------------- */
static int actor_nonfloat(unsigned off)
{
	switch (off) {
	case 0x00: case 0x04: case 0x08: case 0x0C: case 0x10: case 0x14: case 0x24:
	case 0x13C: case 0x140: case 0x144: case 0x148: case 0x258: case 0x25C: case 0x260: case 0x264:
	case 0x268: case 0x26C: case 0x278: case 0x2A4: case 0x2F8: case 0x354:
	case 0x4B8: case 0x4BC: case 0x4C0: case 0x4C4:
		return 1;
	default:
		return 0;
	}
}

static int variant_nonfloat(unsigned off)
{
	switch (off) {
	case 0x00: case 0x04: case 0x08: case 0x0C: case 0x10: case 0x14: case 0x18: case 0x1C: case 0x20: case 0x24: case 0x28:
	case 0x2C: case 0x30: case 0x34: case 0x4C: case 0x64: case 0x68: case 0x6C: case 0x70: case 0x154: case 0x180: case 0x184:
	case 0x18C: case 0x1C0: case 0x1C4: case 0x1C8: case 0x1CC: case 0x1D0: case 0x1E0: case 0x20C: case 0x22C: case 0x230:
	case 0x234:
		return 1;
	default:
		return 0;
	}
}

static int actor_ok(const uint8_t *a)
{
	unsigned o;
	if (rd16(a + 0x14) > AIDEF_ACTOR_MOUNTED_WEAPON) return 0;
	for (o = 0; o < AIDEF_ACTOR_RAW_SIZE; o += 4)
		if (!actor_nonfloat(o) && !finite_at(a + o)) return 0;
	return 1;
}

static int variant_ok(const uint8_t *v)
{
	unsigned o;
	for (o = 0; o < AIDEF_VARIANT_RAW_SIZE; o += 4)
		if (!variant_nonfloat(o) && !finite_at(v + o)) return 0;
	if (rd16(v + 0x4C) > 7 || rd16(v + 0x154) > 7 || rd16(v + 0x156) > 7) return 0;
	if (rd16(v + 0x180) > 7 || rd16(v + 0x182) > 7 || rd16(v + 0x184) > 7 || rd16(v + 0x186) > 255) return 0;
	if (rd32(v + 0x22C) > 255) return 0;
	return 1;
}

/* case-insensitive (ASCII) name order, as scripts compare names */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_cmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = lower((unsigned char)*a), cb = lower((unsigned char)*b);
		if (ca != cb) return ca < cb ? -1 : 1;
		if (!ca) return 0;
	}
}

static int printable_name(const uint8_t *p)
{
	unsigned i;
	if (!terminated(p, AIDEF_NAME_LEN)) return 0;
	for (i = 0; p[i]; i++)
		if (p[i] < 0x20 || p[i] > 0x7E) return 0;
	return 1;
}

/* a table of u32 indices into a table of `n` records whose names start at name_off, sorted by name (then index) */
static int name_table_ok(const aidef_section *s, aidef_table nt, aidef_table rt, unsigned name_off)
{
	uint32_t i, n = s->count[rt];
	if (s->count[nt] != n) return 0;
	for (i = 0; i < n; i++) {
		uint32_t idx = rd32(s->tab[nt] + 4u * i);
		if (idx >= n) return 0;
		if (i) {
			uint32_t pi = rd32(s->tab[nt] + 4u * (i - 1));
			int c = name_cmp((const char *)rec_at(s, rt, pi) + name_off, (const char *)rec_at(s, rt, idx) + name_off);
			if (c > 0 || (c == 0 && pi >= idx)) return 0;
		}
	}
	return 1;
}

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

/* the child ranges of consecutive parents must tile the child table exactly (first = running sum) */
static int tile_ok(const aidef_section *s, aidef_table parents, unsigned first_off, unsigned count_off, aidef_table children)
{
	uint64_t run = 0;
	uint32_t i;
	for (i = 0; i < s->count[parents]; i++) {
		const uint8_t *p = rec_at(s, parents, i);
		if (rd32(p + first_off) != run) return 0;
		run += rd32(p + count_off);
		if (run > s->count[children]) return 0;
	}
	return run == s->count[children];
}

static int validate(const aidef_section *s)
{
	uint32_t i, k, prev = 0;
	uint32_t npal = s->count[AIDEF_T_PALETTE], ncl = s->count[AIDEF_T_CMDLISTS], nenc = s->count[AIDEF_T_ENCOUNTERS];

	if (s->count[AIDEF_T_ACTOR_IDS] != s->count[AIDEF_T_ACTORS] || s->count[AIDEF_T_VARIANT_IDS] != s->count[AIDEF_T_VARIANTS]) return 0;
	for (i = 0; i < s->count[AIDEF_T_ACTORS]; i++) {
		uint32_t id = rd32(s->tab[AIDEF_T_ACTOR_IDS] + 4u * i);
		if (i && id <= prev) return 0;                           /* strictly ascending: binary search */
		prev = id;
		if (!actor_ok(rec_at(s, AIDEF_T_ACTORS, i))) return 0;
	}
	for (i = 0; i < s->count[AIDEF_T_VARIANTS]; i++) {
		uint32_t id = rd32(s->tab[AIDEF_T_VARIANT_IDS] + 4u * i);
		if (i && id <= prev) return 0;
		prev = id;
		if (!variant_ok(rec_at(s, AIDEF_T_VARIANTS, i))) return 0;
	}
	for (i = 0; i < npal; i++) {
		const uint8_t *p = rec_at(s, AIDEF_T_PALETTE, i);
		uint32_t vi = rd32(p + 4);
		if (vi != AIDEF_NO_INDEX && (vi >= s->count[AIDEF_T_VARIANTS] || rd32(s->tab[AIDEF_T_VARIANT_IDS] + 4u * vi) != rd32(p)))
			return 0;
	}

	/* encounters: their children tile the child tables */
	if (!tile_ok(s, AIDEF_T_ENCOUNTERS, 48, 52, AIDEF_T_SQUADS) || !tile_ok(s, AIDEF_T_ENCOUNTERS, 56, 60, AIDEF_T_PLATOONS) ||
	    !tile_ok(s, AIDEF_T_ENCOUNTERS, 64, 68, AIDEF_T_FIRINGS) || !tile_ok(s, AIDEF_T_ENCOUNTERS, 72, 76, AIDEF_T_PSTARTS))
		return 0;
	if (!tile_ok(s, AIDEF_T_SQUADS, 92, 96, AIDEF_T_STARTS) || !tile_ok(s, AIDEF_T_SQUADS, 100, 104, AIDEF_T_MOVES)) return 0;
	if (!tile_ok(s, AIDEF_T_CMDLISTS, 40, 44, AIDEF_T_COMMANDS) || !tile_ok(s, AIDEF_T_CMDLISTS, 48, 52, AIDEF_T_POINTS)) return 0;
	if (!tile_ok(s, AIDEF_T_CONVS, 44, 48, AIDEF_T_PARTS) || !tile_ok(s, AIDEF_T_CONVS, 52, 56, AIDEF_T_LINES)) return 0;

	for (i = 0; i < nenc; i++) {
		const uint8_t *e = rec_at(s, AIDEF_T_ENCOUNTERS, i);
		uint32_t nsq = rd32(e + 52), npl = rd32(e + 60), fsq = rd32(e + 48);
		if (!printable_name(e) || rd16(e + 36) >= AIDEF_TEAMS) return 0;
		for (k = 0; k < nsq; k++) {
			const uint8_t *q = rec_at(s, AIDEF_T_SQUADS, fsq + k);
			int a = rds16(q + SQ_ACTOR_TYPE), pl = rds16(q + 34), mv = rds16(q + 46);
			if (!printable_name(q)) return 0;
			if (a < -1 || a >= (int)npal || pl < -1 || pl >= (int)npl || mv < -1 || mv >= (int)nsq) return 0;
			if (rd16(q + SQ_STATES) >= AIDEF_STATES || rd16(q + SQ_STATES + 2) >= AIDEF_STATES) return 0;
			if (rd16(q + SQ_LEADER) > 4 || rd16(q + SQ_COUNTS + 4) > 4) return 0;
			if (rd16(q + SQ_COUNTS) > 1000 || rd16(q + SQ_COUNTS + 2) > 1000) return 0;
			if (!finite_at(q + SQ_DELAY)) return 0;
		}
	}
	for (i = 0; i < s->count[AIDEF_T_STARTS]; i++) {
		const uint8_t *p = rec_at(s, AIDEF_T_STARTS, i);
		int c = rds16(p + 26);
		if (!finites(p, 0, 16) || c < -1 || c >= (int)ncl) return 0;
	}
	for (i = 0; i < s->count[AIDEF_T_MOVES]; i++)
		if (!finites(rec_at(s, AIDEF_T_MOVES, i), 0, 28)) return 0;
	for (i = 0; i < s->count[AIDEF_T_FIRINGS]; i++)
		if (!finites(rec_at(s, AIDEF_T_FIRINGS, i), 0, 12)) return 0;
	for (i = 0; i < s->count[AIDEF_T_PLATOONS]; i++)
		if (!printable_name(rec_at(s, AIDEF_T_PLATOONS, i))) return 0;
	for (i = 0; i < s->count[AIDEF_T_PSTARTS]; i++)
		if (!finites(rec_at(s, AIDEF_T_PSTARTS, i), 0, 16)) return 0;
	for (i = 0; i < ncl; i++)
		if (!printable_name(rec_at(s, AIDEF_T_CMDLISTS, i))) return 0;
	for (i = 0; i < s->count[AIDEF_T_COMMANDS]; i++) {
		const uint8_t *p = rec_at(s, AIDEF_T_COMMANDS, i);
		if (rd16(p) >= AIDEF_ATOMS || !finite_at(p + 4) || !finite_at(p + 8)) return 0;
	}
	for (i = 0; i < s->count[AIDEF_T_POINTS]; i++)
		if (!finites(rec_at(s, AIDEF_T_POINTS, i), 0, 12)) return 0;
	for (i = 0; i < s->count[AIDEF_T_CONVS]; i++) {
		const uint8_t *c = rec_at(s, AIDEF_T_CONVS, i);
		uint32_t np = rd32(c + 48);
		if (!printable_name(c) || !finite_at(c + 36) || !finite_at(c + 40)) return 0;
		for (k = 0; k < rd32(c + 56); k++) {
			const uint8_t *l = rec_at(s, AIDEF_T_LINES, rd32(c + 52) + k);
			int part = rds16(l + 2);
			if (part < -1 || part >= (int)np || !finite_at(l + 8)) return 0;
		}
	}
	for (i = 0; i < s->count[AIDEF_T_PARTS]; i++) {
		const uint8_t *p = rec_at(s, AIDEF_T_PARTS, i);
		int32_t e = rds32(p + 24);
		if (!terminated(p + 28, AIDEF_NAME_LEN) || e < -1 || e >= (int32_t)nenc) return 0;
	}
	for (i = 0; i < s->count[AIDEF_T_ANIMS]; i++)
		if (!printable_name(rec_at(s, AIDEF_T_ANIMS, i))) return 0;
	for (i = 0; i < s->count[AIDEF_T_SCRIPTS]; i++)
		if (!printable_name(rec_at(s, AIDEF_T_SCRIPTS, i))) return 0;
	if (!name_table_ok(s, AIDEF_T_ENC_NAMES, AIDEF_T_ENCOUNTERS, 0) || !name_table_ok(s, AIDEF_T_CMD_NAMES, AIDEF_T_CMDLISTS, 0) ||
	    !name_table_ok(s, AIDEF_T_CONV_NAMES, AIDEF_T_CONVS, 0))
		return 0;
	return 1;
}

aidef_status aidef_open(aidef_section *s, const uint8_t *data, size_t size)
{
	unsigned t;
	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return AIDEF_ERR_TRUNCATED;
	if (memcmp(data, "AIDT", 4) != 0) return AIDEF_ERR_MAGIC;
	if (rd16(data + 4) != AIDEF_VERSION) return AIDEF_ERR_VERSION;
	if (rd32(data + 8) > size) return AIDEF_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return AIDEF_ERR_LAYOUT;
	for (t = 0; t < AIDEF_TABLES; t++) {
		uint32_t off = rd32(data + DIR_OFFSET + 8u * t), cnt = rd32(data + DIR_OFFSET + 8u * t + 4);
		if (cnt > TAB[t].max || !table_ok(off, cnt, TAB[t].rec, size)) {
			memset(s, 0, sizeof *s);
			return AIDEF_ERR_LAYOUT;
		}
		s->tab[t] = data + off;
		s->count[t] = cnt;
	}
	s->data = data;
	s->size = size;
	if (!validate(s)) {
		memset(s, 0, sizeof *s);
		return AIDEF_ERR_LAYOUT;
	}
	return AIDEF_OK;
}

/* ---- actors and variants -------------------------------------------------------------------------------------- */
static int find_id(const aidef_section *s, aidef_table ids, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->count[ids];
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->tab[ids] + 4u * mid);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

int aidef_find_actor(const aidef_section *s, uint32_t tag_id) { return find_id(s, AIDEF_T_ACTOR_IDS, tag_id); }
int aidef_find_variant(const aidef_section *s, uint32_t tag_id) { return find_id(s, AIDEF_T_VARIANT_IDS, tag_id); }

int aidef_get_actor(const aidef_section *s, uint32_t index, aidef_actor_def *o)
{
	const uint8_t *a;
	memset(o, 0, sizeof *o);
	if (index >= s->count[AIDEF_T_ACTORS]) return 0;
	a = rec_at(s, AIDEF_T_ACTORS, index);
	o->raw = a;
	o->tag_id = rd32(s->tab[AIDEF_T_ACTOR_IDS] + 4u * index);
	o->flags = rd32(a + 0x00);
	o->more_flags = rd32(a + 0x04);
	o->type = rd16(a + 0x14);
	o->max_vision_distance = rdf(a + 0x18);
	o->central_vision_angle = rdf(a + 0x1C);
	o->max_vision_angle = rdf(a + 0x20);
	o->peripheral_vision_angle = rdf(a + 0x28);
	o->peripheral_distance = rdf(a + 0x2C);
	load_floats(o->standing_gun_offset, a + 0x34, 3);
	load_floats(o->crouching_gun_offset, a + 0x40, 3);
	o->hearing_distance = rdf(a + 0x4C);
	o->notice_projectile_chance = rdf(a + 0x50);
	o->notice_vehicle_chance = rdf(a + 0x54);
	o->combat_perception_time = rdf(a + 0x60);
	o->guard_perception_time = rdf(a + 0x64);
	o->non_combat_perception_time = rdf(a + 0x68);
	o->inv_combat_perception_time = rdf(a + 0x6C);
	o->inv_guard_perception_time = rdf(a + 0x70);
	o->inv_non_combat_perception_time = rdf(a + 0x74);
	load_floats(o->fractions_80, a + 0x80, 4);
	load_floats(o->values_90, a + 0x90, 3);
	o->angle_9c = rdf(a + 0x9C);
	o->angle_9c_cos = rdf(a + 0xA0);
	load_floats(o->max_aiming_deviation, a + 0xA4, 2);
	load_floats(o->max_looking_deviation, a + 0xAC, 2);
	load_floats(o->looking_angles, a + 0xB4, 8);
	load_floats(&o->idle_times[0][0], a + 0xD4, 20);
	load_floats(o->cos_aiming_deviation, a + 0x12C, 2);
	load_floats(o->cos_looking_deviation, a + 0x134, 2);
	o->weapon_tag = rd32(a + 0x148);
	o->projectile_tag = rd32(a + 0x264);
	o->actor_tag = rd32(a + 0x4C4);
	return 1;
}

int aidef_actor(const aidef_section *s, uint32_t tag_id, aidef_actor_def *out)
{
	int i = aidef_find_actor(s, tag_id);
	if (i < 0) {
		memset(out, 0, sizeof *out);
		return 0;
	}
	return aidef_get_actor(s, (uint32_t)i, out);
}

int aidef_get_variant(const aidef_section *s, uint32_t index, aidef_variant_def *o)
{
	const uint8_t *v;
	memset(o, 0, sizeof *o);
	if (index >= s->count[AIDEF_T_VARIANTS]) return 0;
	v = rec_at(s, AIDEF_T_VARIANTS, index);
	o->raw = v;
	o->tag_id = rd32(s->tab[AIDEF_T_VARIANT_IDS] + 4u * index);
	o->flags = rd32(v + 0x00);
	o->actor_tag = rd32(v + 0x10);
	o->unit_tag = rd32(v + 0x20);
	o->major_variant_tag = rd32(v + 0x30);
	o->weapon_tag = rd32(v + 0x70);
	o->equipment_tag = rd32(v + 0x1CC);
	o->movement_type = rd16(v + 0x4C);
	o->initial_crouch_chance = rdf(v + 0x50);
	load_floats(o->crouch_time, v + 0x54, 2);
	load_floats(o->run_time, v + 0x5C, 2);
	o->max_firing_distance = rdf(v + 0x74);
	o->rate_of_fire = rdf(v + 0x78);
	o->projectile_error = rdf(v + 0x7C);
	load_floats(o->first_burst_delay, v + 0x80, 2);
	o->new_target_firing_pattern_time = rdf(v + 0x88);
	o->surprise_delay_time = rdf(v + 0x8C);
	o->surprise_fire_wildly_time = rdf(v + 0x90);
	o->death_fire_wildly_chance = rdf(v + 0x94);
	o->death_fire_wildly_time = rdf(v + 0x98);
	load_floats(o->desired_combat_range, v + 0x9C, 2);
	load_floats(o->custom_stand_gun_offset, v + 0xA4, 3);
	load_floats(o->custom_crouch_gun_offset, v + 0xB0, 3);
	o->target_tracking = rdf(v + 0xBC);
	o->target_leading = rdf(v + 0xC0);
	o->weapon_damage_modifier = rdf(v + 0xC4);
	o->damage_per_second = rdf(v + 0xC8);
	o->burst_origin_radius = rdf(v + 0xCC);
	o->burst_origin_angle = rdf(v + 0xD0);
	load_floats(o->burst_return_length, v + 0xD4, 2);
	o->burst_return_angle = rdf(v + 0xDC);
	load_floats(o->burst_duration, v + 0xE0, 2);
	load_floats(o->burst_separation, v + 0xE8, 2);
	o->burst_angular_velocity = rdf(v + 0xF0);
	o->special_damage_modifier = rdf(v + 0xF8);
	o->special_projectile_error = rdf(v + 0xFC);
	o->special_fire_mode = rd16(v + 0x154);
	o->special_fire_situation = rd16(v + 0x156);
	o->special_fire_chance = rdf(v + 0x158);
	o->special_fire_delay = rdf(v + 0x15C);
	o->grenade_type = rd16(v + 0x180);
	o->grenade_trajectory = rd16(v + 0x182);
	o->grenade_stimulus = rd16(v + 0x184);
	o->grenade_min_enemies = rd16(v + 0x186);
	o->grenade_enemy_radius = rdf(v + 0x188);
	o->grenade_velocity = rdf(v + 0x190);
	load_floats(o->grenade_range, v + 0x194, 2);
	o->grenade_collateral_radius = rdf(v + 0x19C);
	o->grenade_chance = rdf(v + 0x1A0);
	o->grenade_check_time = rdf(v + 0x1A4);
	o->encounter_grenade_timeout = rdf(v + 0x1A8);
	o->grenade_count[0] = rd16(v + 0x1D0);
	o->grenade_count[1] = rd16(v + 0x1D2);
	o->dont_drop_grenades_chance = rdf(v + 0x1D4);
	load_floats(o->drop_weapon_loaded, v + 0x1D8, 2);
	o->drop_weapon_ammo[0] = rd16(v + 0x1E0);
	o->drop_weapon_ammo[1] = rd16(v + 0x1E2);
	o->body_vitality = rdf(v + 0x200);
	o->shield_vitality = rdf(v + 0x204);
	o->shield_sapping_radius = rdf(v + 0x208);
	o->forced_shader_permutation = rd16(v + 0x20C);
	o->change_color_count = (uint16_t)rd32(v + 0x22C);
	return 1;
}

int aidef_actor_variant(const aidef_section *s, uint32_t tag_id, aidef_variant_def *out)
{
	int i = aidef_find_variant(s, tag_id);
	if (i < 0) {
		memset(out, 0, sizeof *out);
		return 0;
	}
	return aidef_get_variant(s, (uint32_t)i, out);
}

int aidef_variant_actor(const aidef_section *s, const aidef_variant_def *v, aidef_actor_def *out)
{
	return aidef_actor(s, v->actor_tag, out);
}

int aidef_variant_major(const aidef_section *s, const aidef_variant_def *v, aidef_variant_def *out)
{
	return aidef_actor_variant(s, v->major_variant_tag, out);
}

static uint32_t raw_u32(const uint8_t *raw, uint32_t size, uint32_t off)
{
	if (!raw || off % 4u || off > size - 4u) return 0;
	return rd32(raw + off);
}
static uint32_t raw_u16(const uint8_t *raw, uint32_t size, uint32_t off)
{
	if (!raw || off % 2u || off > size - 2u) return 0;
	return rd16(raw + off);
}
static float raw_f32(const uint8_t *raw, uint32_t size, uint32_t off)
{
	uint32_t u = raw_u32(raw, size, off);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
float aidef_actor_f32(const aidef_actor_def *a, uint32_t off) { return raw_f32(a->raw, AIDEF_ACTOR_RAW_SIZE, off); }
uint32_t aidef_actor_u32(const aidef_actor_def *a, uint32_t off) { return raw_u32(a->raw, AIDEF_ACTOR_RAW_SIZE, off); }
uint32_t aidef_actor_u16(const aidef_actor_def *a, uint32_t off) { return raw_u16(a->raw, AIDEF_ACTOR_RAW_SIZE, off); }
float aidef_variant_f32(const aidef_variant_def *v, uint32_t off) { return raw_f32(v->raw, AIDEF_VARIANT_RAW_SIZE, off); }
uint32_t aidef_variant_u32(const aidef_variant_def *v, uint32_t off) { return raw_u32(v->raw, AIDEF_VARIANT_RAW_SIZE, off); }
uint32_t aidef_variant_u16(const aidef_variant_def *v, uint32_t off) { return raw_u16(v->raw, AIDEF_VARIANT_RAW_SIZE, off); }

/* ---- palette ----------------------------------------------------------------------------------------------------- */
uint32_t aidef_palette_count(const aidef_section *s) { return s->count[AIDEF_T_PALETTE]; }

uint32_t aidef_palette_tag(const aidef_section *s, uint32_t index)
{
	return index < s->count[AIDEF_T_PALETTE] ? rd32(rec_at(s, AIDEF_T_PALETTE, index)) : 0;
}

int aidef_palette_variant(const aidef_section *s, uint32_t index, aidef_variant_def *out)
{
	uint32_t vi;
	if (index >= s->count[AIDEF_T_PALETTE]) return aidef_get_variant(s, s->count[AIDEF_T_VARIANTS], out);   /* neutral */
	vi = rd32(rec_at(s, AIDEF_T_PALETTE, index) + 4);
	return aidef_get_variant(s, vi, out);          /* AIDEF_NO_INDEX is out of range: neutral, 0 */
}

/* ---- encounters, squads and their children --------------------------------------------------------------------------- */
uint32_t aidef_encounter_count(const aidef_section *s) { return s->count[AIDEF_T_ENCOUNTERS]; }
uint32_t aidef_squad_count(const aidef_section *s) { return s->count[AIDEF_T_SQUADS]; }

int aidef_get_encounter(const aidef_section *s, uint32_t index, aidef_encounter_def *o)
{
	const uint8_t *e;
	memset(o, 0, sizeof *o);
	if (index >= s->count[AIDEF_T_ENCOUNTERS]) return 0;
	e = rec_at(s, AIDEF_T_ENCOUNTERS, index);
	load_str(o->name, e);
	o->flags = rd32(e + 32);
	o->team = rd16(e + 36);
	o->search_behavior = rd16(e + 38);
	o->w28 = rd16(e + 40);
	o->manual_bsp = rds16(e + 42);
	o->precomputed_bsp = rds16(e + 44);
	o->first_squad = rd32(e + 48);
	o->squad_count = rd32(e + 52);
	o->first_platoon = rd32(e + 56);
	o->platoon_count = rd32(e + 60);
	o->first_firing = rd32(e + 64);
	o->firing_count = rd32(e + 68);
	o->first_pstart = rd32(e + 72);
	o->pstart_count = rd32(e + 76);
	return 1;
}

/* binary search of a name table (case-insensitive); returns the record index or -1 */
static int find_name(const aidef_section *s, aidef_table nt, aidef_table rt, const char *name)
{
	uint32_t lo = 0, hi = s->count[nt];
	if (!name) return -1;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, idx = rd32(s->tab[nt] + 4u * mid);
		int c = name_cmp(name, (const char *)rec_at(s, rt, idx));
		if (c == 0) {                                   /* equal names are sorted by index: the first one wins */
			while (mid > 0 && name_cmp(name, (const char *)rec_at(s, rt, rd32(s->tab[nt] + 4u * (mid - 1)))) == 0) mid--;
			return (int)rd32(s->tab[nt] + 4u * mid);
		}
		if (c > 0) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

int aidef_find_encounter(const aidef_section *s, const char *name) { return find_name(s, AIDEF_T_ENC_NAMES, AIDEF_T_ENCOUNTERS, name); }

int aidef_get_squad(const aidef_section *s, uint32_t index, aidef_squad_def *o)
{
	const uint8_t *q;
	unsigned i;
	memset(o, 0, sizeof *o);
	o->actor_type = o->platoon = o->maneuver_squad = -1;
	if (index >= s->count[AIDEF_T_SQUADS]) return 0;
	q = rec_at(s, AIDEF_T_SQUADS, index);
	load_str(o->name, q);
	o->actor_type = rds16(q + 32);
	o->platoon = rds16(q + 34);
	o->initial_state = rd16(q + 36);
	o->return_state = rd16(q + 38);
	o->flags = rd32(q + 40);
	o->unique_leader = rd16(q + 44);
	o->maneuver_squad = rds16(q + 46);
	o->delay_time = rdf(q + 48);
	for (i = 0; i < 7; i++) o->masks[i] = rd32(q + SQ_MASKS + 4 * i);
	o->normal_count = rd16(q + 80);
	o->insane_count = rd16(q + 82);
	o->major_upgrade = rd16(q + 84);
	o->w82 = rd16(q + 86);
	o->w84 = rd16(q + 88);
	o->w86 = rd16(q + 90);
	o->first_start = rd32(q + 92);
	o->start_count = rd32(q + 96);
	o->first_move = rd32(q + 100);
	o->move_count = rd32(q + 104);
	return 1;
}

int aidef_encounter_squad(const aidef_section *s, uint32_t encounter, uint32_t k, aidef_squad_def *out)
{
	aidef_encounter_def e;
	if (!aidef_get_encounter(s, encounter, &e) || k >= e.squad_count) return aidef_get_squad(s, s->count[AIDEF_T_SQUADS], out);
	return aidef_get_squad(s, e.first_squad + k, out);
}

int aidef_find_squad(const aidef_section *s, uint32_t encounter, const char *name)
{
	aidef_encounter_def e;
	uint32_t k;
	if (!name || !aidef_get_encounter(s, encounter, &e)) return -1;
	for (k = 0; k < e.squad_count; k++)
		if (name_cmp(name, (const char *)rec_at(s, AIDEF_T_SQUADS, e.first_squad + k)) == 0) return (int)(e.first_squad + k);
	return -1;
}

int aidef_find_squad_path(const aidef_section *s, const char *path)
{
	char enc[AIDEF_NAME_LEN];
	size_t n = 0;
	int e;
	if (!path) return -1;
	while (path[n] && path[n] != '/') n++;
	if (!path[n] || n == 0 || n >= AIDEF_NAME_LEN) return -1;
	memcpy(enc, path, n);
	enc[n] = 0;
	e = aidef_find_encounter(s, enc);
	return e < 0 ? -1 : aidef_find_squad(s, (uint32_t)e, path + n + 1);
}

int aidef_squad_variant(const aidef_section *s, const aidef_squad_def *sq, aidef_variant_def *out)
{
	if (sq->actor_type < 0) return aidef_get_variant(s, s->count[AIDEF_T_VARIANTS], out);
	return aidef_palette_variant(s, (uint32_t)sq->actor_type, out);
}

int aidef_get_start(const aidef_section *s, const aidef_squad_def *sq, uint32_t k, aidef_start_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	o->command_list = -1;
	if (k >= sq->start_count) return 0;
	idx = (uint64_t)sq->first_start + k;
	if (idx >= s->count[AIDEF_T_STARTS]) return 0;       /* a def not taken from this section */
	p = rec_at(s, AIDEF_T_STARTS, (uint32_t)idx);
	load_floats(o->pos, p, 3);
	o->facing = rdf(p + 12);
	o->cluster = rds16(p + 16);
	o->w12 = rds16(p + 18);
	o->w14 = rd16(p + 20);
	o->w16 = rd16(p + 22);
	o->w18 = rds16(p + 24);
	o->command_list = rds16(p + 26);
	return 1;
}

int aidef_get_move(const aidef_section *s, const aidef_squad_def *sq, uint32_t k, aidef_move_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	o->animation = o->surface = -1;
	if (k >= sq->move_count) return 0;
	idx = (uint64_t)sq->first_move + k;
	if (idx >= s->count[AIDEF_T_MOVES]) return 0;
	p = rec_at(s, AIDEF_T_MOVES, (uint32_t)idx);
	load_floats(o->pos, p, 3);
	o->facing = rdf(p + 12);
	o->weight = rdf(p + 16);
	o->time_min = rdf(p + 20);
	o->time_max = rdf(p + 24);
	o->animation = rds16(p + 28);
	o->sequence = rds16(p + 30);
	o->cluster = rds16(p + 32);
	o->surface = rds32(p + 36);
	return 1;
}

int aidef_get_firing(const aidef_section *s, const aidef_encounter_def *e, uint32_t k, aidef_firing_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	o->surface = -1;
	if (k >= e->firing_count) return 0;
	idx = (uint64_t)e->first_firing + k;
	if (idx >= s->count[AIDEF_T_FIRINGS]) return 0;
	p = rec_at(s, AIDEF_T_FIRINGS, (uint32_t)idx);
	load_floats(o->pos, p, 3);
	o->group = rd16(p + 12);
	o->cluster = rds16(p + 14);
	o->surface = rds32(p + 16);
	return 1;
}

int aidef_get_platoon(const aidef_section *s, const aidef_encounter_def *e, uint32_t k, aidef_platoon_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	if (k >= e->platoon_count) return 0;
	idx = (uint64_t)e->first_platoon + k;
	if (idx >= s->count[AIDEF_T_PLATOONS]) return 0;
	p = rec_at(s, AIDEF_T_PLATOONS, (uint32_t)idx);
	load_str(o->name, p);
	o->w20 = rd32(p + 32);
	o->w30 = rd16(p + 36);
	o->w32 = rds16(p + 38);
	o->w3c = rd16(p + 40);
	o->w3e = rds16(p + 42);
	return 1;
}

int aidef_find_platoon(const aidef_section *s, const aidef_encounter_def *e, const char *name)
{
	uint32_t k;
	if (!name) return -1;
	for (k = 0; k < e->platoon_count; k++) {
		uint64_t idx = (uint64_t)e->first_platoon + k;
		if (idx >= s->count[AIDEF_T_PLATOONS]) return -1;
		if (name_cmp(name, (const char *)rec_at(s, AIDEF_T_PLATOONS, (uint32_t)idx)) == 0) return (int)k;
	}
	return -1;
}

int aidef_get_pstart(const aidef_section *s, const aidef_encounter_def *e, uint32_t k, aidef_pstart_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	unsigned i;
	memset(o, 0, sizeof *o);
	if (k >= e->pstart_count) return 0;
	idx = (uint64_t)e->first_pstart + k;
	if (idx >= s->count[AIDEF_T_PSTARTS]) return 0;
	p = rec_at(s, AIDEF_T_PSTARTS, (uint32_t)idx);
	load_floats(o->pos, p, 3);
	o->facing = rdf(p + 12);
	o->team = rd16(p + 16);
	o->bsp = rd16(p + 18);
	for (i = 0; i < 4; i++) o->types[i] = rd16(p + 20 + 2 * i);
	return 1;
}

uint32_t aidef_squad_spawn_count(const aidef_squad_def *sq, uint32_t difficulty)
{
	uint32_t n = sq->normal_count, ins = sq->insane_count;
	if (difficulty > AIDEF_MAX_DIFFICULTY) difficulty = AIDEF_MAX_DIFFICULTY;
	switch (difficulty) {
	case 0: case 1: return n;
	case 2: return (n + ins + 1u) / 2u;
	default: return ins;
	}
}

/* ---- command lists ----------------------------------------------------------------------------------------------------- */
uint32_t aidef_command_list_count(const aidef_section *s) { return s->count[AIDEF_T_CMDLISTS]; }

int aidef_get_command_list(const aidef_section *s, uint32_t index, aidef_command_list_def *o)
{
	const uint8_t *l;
	memset(o, 0, sizeof *o);
	if (index >= s->count[AIDEF_T_CMDLISTS]) return 0;
	l = rec_at(s, AIDEF_T_CMDLISTS, index);
	load_str(o->name, l);
	o->flags = rd32(l + 32);
	o->manual_bsp = rds16(l + 36);
	o->precomputed_bsp = rds16(l + 38);
	o->first_command = rd32(l + 40);
	o->command_count = rd32(l + 44);
	o->first_point = rd32(l + 48);
	o->point_count = rd32(l + 52);
	return 1;
}

int aidef_find_command_list(const aidef_section *s, const char *name) { return find_name(s, AIDEF_T_CMD_NAMES, AIDEF_T_CMDLISTS, name); }

int aidef_get_command(const aidef_section *s, const aidef_command_list_def *l, uint32_t k, aidef_command_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	o->point1 = o->point2 = o->animation = o->script = o->recording = o->command = o->object_name = -1;
	if (k >= l->command_count) return 0;
	idx = (uint64_t)l->first_command + k;
	if (idx >= s->count[AIDEF_T_COMMANDS]) return 0;
	p = rec_at(s, AIDEF_T_COMMANDS, (uint32_t)idx);
	o->type = rd16(p);
	o->modifier = rd16(p + 2);
	o->param1 = rdf(p + 4);
	o->param2 = rdf(p + 8);
	o->point1 = rds16(p + 12);
	o->point2 = rds16(p + 14);
	o->animation = rds16(p + 16);
	o->script = rds16(p + 18);
	o->recording = rds16(p + 20);
	o->command = rds16(p + 22);
	o->object_name = rds16(p + 24);
	return 1;
}

int aidef_get_point(const aidef_section *s, const aidef_command_list_def *l, uint32_t k, aidef_point_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	o->surface = -1;
	if (k >= l->point_count) return 0;
	idx = (uint64_t)l->first_point + k;
	if (idx >= s->count[AIDEF_T_POINTS]) return 0;
	p = rec_at(s, AIDEF_T_POINTS, (uint32_t)idx);
	load_floats(o->pos, p, 3);
	o->surface = rds32(p + 12);
	return 1;
}

/* ---- conversations ------------------------------------------------------------------------------------------------------- */
uint32_t aidef_conversation_count(const aidef_section *s) { return s->count[AIDEF_T_CONVS]; }

int aidef_get_conversation(const aidef_section *s, uint32_t index, aidef_conversation_def *o)
{
	const uint8_t *c;
	memset(o, 0, sizeof *o);
	if (index >= s->count[AIDEF_T_CONVS]) return 0;
	c = rec_at(s, AIDEF_T_CONVS, index);
	load_str(o->name, c);
	o->flags = rd32(c + 32);
	o->trigger_distance = rdf(c + 36);
	o->run_to_player_distance = rdf(c + 40);
	o->first_participant = rd32(c + 44);
	o->participant_count = rd32(c + 48);
	o->first_line = rd32(c + 52);
	o->line_count = rd32(c + 56);
	return 1;
}

int aidef_find_conversation(const aidef_section *s, const char *name) { return find_name(s, AIDEF_T_CONV_NAMES, AIDEF_T_CONVS, name); }

int aidef_get_participant(const aidef_section *s, const aidef_conversation_def *c, uint32_t k, aidef_participant_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	unsigned i;
	memset(o, 0, sizeof *o);
	o->use_object = o->new_name_object = -1;
	o->encounter_index = -1;
	for (i = 0; i < 6; i++) o->variants[i] = -1;
	if (k >= c->participant_count) return 0;
	idx = (uint64_t)c->first_participant + k;
	if (idx >= s->count[AIDEF_T_PARTS]) return 0;
	p = rec_at(s, AIDEF_T_PARTS, (uint32_t)idx);
	o->flags = rd16(p);
	o->w02 = rd16(p + 2);
	o->selection_type = rd16(p + 4);
	o->actor_type = rd16(p + 6);
	o->use_object = rds16(p + 8);
	o->new_name_object = rds16(p + 10);
	for (i = 0; i < 6; i++) o->variants[i] = rds16(p + 12 + 2 * i);
	o->encounter_index = rds32(p + 24);
	load_str(o->encounter_name, p + 28);
	return 1;
}

int aidef_get_line(const aidef_section *s, const aidef_conversation_def *c, uint32_t k, aidef_line_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	unsigned i;
	memset(o, 0, sizeof *o);
	o->participant = o->addressee_participant = -1;
	if (k >= c->line_count) return 0;
	idx = (uint64_t)c->first_line + k;
	if (idx >= s->count[AIDEF_T_LINES]) return 0;
	p = rec_at(s, AIDEF_T_LINES, (uint32_t)idx);
	o->flags = rd16(p);
	o->participant = rds16(p + 2);
	o->addressee = rd16(p + 4);
	o->addressee_participant = rds16(p + 6);
	o->delay = rdf(p + 8);
	for (i = 0; i < 6; i++) o->sounds[i] = rd32(p + 12 + 4 * i);
	return 1;
}

/* ---- references -------------------------------------------------------------------------------------------------------------- */
uint32_t aidef_anim_ref_count(const aidef_section *s) { return s->count[AIDEF_T_ANIMS]; }

int aidef_get_anim_ref(const aidef_section *s, uint32_t index, aidef_anim_ref_def *o)
{
	const uint8_t *p;
	memset(o, 0, sizeof *o);
	if (index >= s->count[AIDEF_T_ANIMS]) return 0;
	p = rec_at(s, AIDEF_T_ANIMS, index);
	load_str(o->name, p);
	o->graph_tag = rd32(p + 32);
	return 1;
}

uint32_t aidef_script_ref_count(const aidef_section *s) { return s->count[AIDEF_T_SCRIPTS]; }

int aidef_script_ref_name(const aidef_section *s, uint32_t index, char out[AIDEF_NAME_LEN])
{
	memset(out, 0, AIDEF_NAME_LEN);
	if (index >= s->count[AIDEF_T_SCRIPTS]) return 0;
	load_str(out, rec_at(s, AIDEF_T_SCRIPTS, index));
	return 1;
}

/* ---- names ----------------------------------------------------------------------------------------------------------------------- */
const char *aidef_actor_type_name(uint32_t type)
{
	static const char *const n[] = { "elite", "jackal", "grunt", "hunter", "engineer", "assassin", "player", "marine", "crew",
		"combat form", "infection form", "carrier form", "monitor", "sentinel", "none", "mounted weapon" };
	return type < sizeof n / sizeof n[0] ? n[type] : "?";
}

const char *aidef_state_name(uint32_t state)
{
	static const char *const n[] = { "none", "sleeping", "alert", "moving (repeat same position)", "moving (loop)",
		"moving (loop back and forth)", "moving (loop randomly)", "moving (randomly)", "guarding",
		"guarding (at guard position)", "searching", "fleeing" };
	return state < sizeof n / sizeof n[0] ? n[state] : "?";
}

const char *aidef_team_name(uint32_t team)
{
	static const char *const n[] = { "default by unit", "player", "human", "covenant", "flood", "sentinel", "unused 6", "unused 7",
		"unused 8", "unused 9" };
	return team < sizeof n / sizeof n[0] ? n[team] : "?";
}

const char *aidef_atom_name(uint32_t atom)
{
	static const char *const n[] = { "pause", "go to", "go to and face", "move in direction", "look", "animation mode", "crouch",
		"shoot", "grenade", "vehicle", "running jump", "targeted jump", "script", "animate", "recording", "action", "vocalize",
		"targeting", "initiative", "wait", "loop", "die", "move immediate", "look random", "look player", "look object",
		"set radius", "teleport" };
	return atom < sizeof n / sizeof n[0] ? n[atom] : "?";
}
