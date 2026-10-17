/* UNIT section reader (docs/17-unit-section.md). Pure C11, no heap, no double. unitdef_open validates every table
 * once (sizes, alignment, ranges, sort order, indices, counts, string terminators, finite floats), so the accessors
 * can never read out of bounds, whatever the bytes. Records are decoded with byte loads: no alignment assumptions on
 * the section buffer. */
#include "sim/unitdef.h"
#include <string.h>

#define HEADER_SIZE 128u
#define UNITDEF_VERSION 1u
#define GLOBALS_REC 832u
#define BIPED_REC 576u
#define VEHICLE_REC 256u
#define SEAT_REC 128u
#define MATERIAL_REC 48u
#define MAX_BIPEDS 255u
#define MAX_VEHICLES 255u
#define MAX_SEATS 1024u
#define NUM_LEVELS 4u

/* header: +0 "UNIT", +4 u16 version, +8 size, +0x0C bipeds, +0x10 vehicles, +0x14 materials, +0x18 seats, +0x1C globals
 * offset, +0x20 bipeds, +0x24 vehicles, +0x28 materials, +0x2C seats, +0x30 player biped tag, +0x34 its index + 1 */

/* globals record */
#define G_DEFAULT_BIPED 0x000
#define G_COOP_EFFECT 0x004
#define G_CAMERA_TRACK 0x008
#define G_WATER_ENTER 0x00C
#define G_WATER_EXIT 0x010
#define G_PLAYER_INFO 0x020          /* 12 + 5 + 3 floats up to 0x70 */
#define G_CONTROL 0x070
#define G_CTRL_TICKS 0x094
#define G_FLIP_ANGLE 0x098
#define G_LOOK_FUNCTION 0x09C
#define G_FALLING 0x0B4
#define G_DAMAGE_TAGS 0x0D0
#define G_GRENADES 0x0F0
#define G_DIFFICULTY 0x130
#define G_FLOATS_A_BEGIN 0x020
#define G_FLOATS_A_END 0x094
#define G_FLOATS_B_BEGIN 0x098
#define G_FLOATS_B_END 0x0D0
#define G_DIFF_END 0x320

/* biped record */
#define B_TAG 0x000
#define B_WEAPONS 0x020
#define B_DIALOGUE 0x030
#define B_FLOATS 0x070
#define B_FLOATS_END 0x150
#define B_UNIT_FLAGS 0x150
#define B_U16S 0x15C
#define B_WEAPON_COUNT 0x16C
#define B_DIALOGUE_COUNT 0x16D
#define B_NAME 0x170
#define B_HOLD 0x190
#define B_AUX 0x1B0
#define B_CAMERA 0x1D0
#define B_FOOT 0x1F0

/* vehicle record */
#define V_FLOATS 0x028
#define V_FLOATS_END 0x06C
#define V_FLAGS 0x06C
#define V_TYPE 0x078
#define V_WEAPON_COUNT 0x084
#define V_NAME 0x086

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p)
{
	uint32_t u = rd32(p);
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}
static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }
static int nonneg_at(const uint8_t *p) { return finite_at(p) && !(rd32(p) & 0x80000000u); }
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
	memcpy(dst, p, UNITDEF_NAME_LEN);
	dst[UNITDEF_NAME_LEN - 1] = 0;
}

static void load_floats(float *dst, const uint8_t *p, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) dst[i] = rdf(p + 4 * i);
}

/* a table [off, off + count * rec) must be 16-aligned, past the header and inside the section */
static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

static int globals_ok(const uint8_t *g)
{
	if (!finites(g, G_FLOATS_A_BEGIN, G_FLOATS_A_END) || !finites(g, G_FLOATS_B_BEGIN, G_FLOATS_B_END)) return 0;
	if (!finites(g, G_DIFFICULTY, G_DIFF_END)) return 0;
	/* gravity must be a positive, finite number: it divides nothing here but the controller integrates with it */
	if (!finite_at(g + 0x0C0) || (rd32(g + 0x0C0) & 0x80000000u) || rdf(g + 0x0C0) == 0.0f) return 0;
	return 1;
}

static int biped_ok(const uint8_t *b)
{
	unsigned i;
	if (!finites(b, B_FLOATS, B_FLOATS_END)) return 0;
	/* radii, heights, vitals and times are never negative */
	if (!nonneg_at(b + 0x070) || !nonneg_at(b + 0x09C) || !nonneg_at(b + 0x0A0) || !nonneg_at(b + 0x0A4)) return 0;
	if (!nonneg_at(b + 0x13C) || !nonneg_at(b + 0x140) || !nonneg_at(b + 0x144)) return 0;
	if (b[B_WEAPON_COUNT] > UNITDEF_MAX_WEAPONS || b[B_DIALOGUE_COUNT] > UNITDEF_MAX_DIALOGUE) return 0;
	for (i = 0; i < UNITDEF_MAX_DIALOGUE; i++)
		if (rd32(b + B_DIALOGUE + 8 * i) > 0xFFFFu) return 0;          /* variant numbers are u16 in the tag */
	if (!terminated(b + B_NAME, UNITDEF_NAME_LEN) || !terminated(b + B_HOLD, UNITDEF_NAME_LEN) ||
	    !terminated(b + B_AUX, UNITDEF_NAME_LEN) || !terminated(b + B_CAMERA, UNITDEF_NAME_LEN) ||
	    !terminated(b + B_FOOT, UNITDEF_NAME_LEN) || !terminated(b + B_FOOT + UNITDEF_NAME_LEN, UNITDEF_NAME_LEN))
		return 0;
	return 1;
}

static int vehicle_ok(const uint8_t *v, uint32_t total_seats)
{
	uint32_t first, count;
	if (!finites(v, V_FLOATS, V_FLOATS_END)) return 0;
	if (!nonneg_at(v + 0x028)) return 0;                                  /* bounding radius */
	if (rd16(v + V_TYPE) > UNITDEF_VEH_TURRET) return 0;
	first = rd16(v + V_TYPE + 2);
	count = rd16(v + V_TYPE + 4);
	if ((uint64_t)first + count > total_seats) return 0;
	if (v[V_WEAPON_COUNT] > UNITDEF_MAX_WEAPONS) return 0;
	if (!terminated(v + V_NAME, UNITDEF_NAME_LEN)) return 0;
	return 1;
}

static int seat_ok(const uint8_t *s)
{
	if (!finite_at(s + 4) || !finite_at(s + 8)) return 0;
	return terminated(s + 0x10, UNITDEF_NAME_LEN) && terminated(s + 0x30, UNITDEF_NAME_LEN) &&
	       terminated(s + 0x50, UNITDEF_NAME_LEN);
}

static int material_ok(const uint8_t *m)
{
	return finite_at(m + 0x10) && finite_at(m + 0x14);
}

unitdef_status unitdef_open(unitdef_section *s, const uint8_t *data, size_t size)
{
	uint32_t nb, nv, nm, ns, o_g, o_b, o_v, o_m, o_s, i, prev = 0;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return UNITDEF_ERR_TRUNCATED;
	if (memcmp(data, "UNIT", 4) != 0) return UNITDEF_ERR_MAGIC;
	if (rd16(data + 4) != UNITDEF_VERSION) return UNITDEF_ERR_VERSION;
	if (rd32(data + 8) > size) return UNITDEF_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return UNITDEF_ERR_LAYOUT;
	nb = rd32(data + 0x0C);
	nv = rd32(data + 0x10);
	nm = rd32(data + 0x14);
	ns = rd32(data + 0x18);
	o_g = rd32(data + 0x1C);
	o_b = rd32(data + 0x20);
	o_v = rd32(data + 0x24);
	o_m = rd32(data + 0x28);
	o_s = rd32(data + 0x2C);
	if (nb > MAX_BIPEDS || nv > MAX_VEHICLES || nm > UNITDEF_MAX_MATERIALS || ns > MAX_SEATS) return UNITDEF_ERR_LAYOUT;
	if (!table_ok(o_g, 1, GLOBALS_REC, size) || !table_ok(o_b, nb, BIPED_REC, size) || !table_ok(o_v, nv, VEHICLE_REC, size) ||
	    !table_ok(o_m, nm, MATERIAL_REC, size) || !table_ok(o_s, ns, SEAT_REC, size))
		return UNITDEF_ERR_LAYOUT;
	if (!globals_ok(data + o_g)) return UNITDEF_ERR_LAYOUT;
	for (i = 0; i < nb; i++) {
		const uint8_t *b = data + o_b + (size_t)i * BIPED_REC;
		uint32_t tag = rd32(b + B_TAG);
		if (i && tag <= prev) return UNITDEF_ERR_LAYOUT;                  /* strictly ascending: binary search */
		prev = tag;
		if (!biped_ok(b)) return UNITDEF_ERR_LAYOUT;
	}
	prev = 0;
	for (i = 0; i < nv; i++) {
		const uint8_t *v = data + o_v + (size_t)i * VEHICLE_REC;
		uint32_t tag = rd32(v);
		if (i && tag <= prev) return UNITDEF_ERR_LAYOUT;
		prev = tag;
		if (!vehicle_ok(v, ns)) return UNITDEF_ERR_LAYOUT;
	}
	for (i = 0; i < ns; i++)
		if (!seat_ok(data + o_s + (size_t)i * SEAT_REC)) return UNITDEF_ERR_LAYOUT;
	for (i = 0; i < nm; i++)
		if (!material_ok(data + o_m + (size_t)i * MATERIAL_REC)) return UNITDEF_ERR_LAYOUT;
	{
		uint32_t ptag = rd32(data + 0x30), pidx = rd32(data + 0x34);
		if (pidx > nb) return UNITDEF_ERR_LAYOUT;
		if (pidx && rd32(data + o_b + (size_t)(pidx - 1) * BIPED_REC + B_TAG) != ptag) return UNITDEF_ERR_LAYOUT;
		s->player_biped_tag = ptag;
		s->player_biped_index1 = pidx;
	}
	s->data = data;
	s->size = size;
	s->biped_count = nb;
	s->vehicle_count = nv;
	s->material_count = nm;
	s->seat_count = ns;
	s->globals = data + o_g;
	s->bipeds = data + o_b;
	s->vehicles = data + o_v;
	s->materials = data + o_m;
	s->seats = data + o_s;
	return UNITDEF_OK;
}

void unitdef_globals(const unitdef_section *s, unitdef_globals_def *out)
{
	const uint8_t *g = s->globals;
	unsigned i;
	memset(out, 0, sizeof *out);
	if (!g) return;
	out->default_biped_tag = rd32(g + G_DEFAULT_BIPED);
	out->coop_effect_tag = rd32(g + G_COOP_EFFECT);
	out->camera_track_tag = rd32(g + G_CAMERA_TRACK);
	out->water_enter_sound = rd32(g + G_WATER_ENTER);
	out->water_exit_sound = rd32(g + G_WATER_EXIT);
	out->walk_speed = rdf(g + 0x20);
	out->run_forward = rdf(g + 0x24);
	out->run_backward = rdf(g + 0x28);
	out->run_sideways = rdf(g + 0x2C);
	out->run_accel = rdf(g + 0x30);
	out->sneak_forward = rdf(g + 0x34);
	out->sneak_backward = rdf(g + 0x38);
	out->sneak_sideways = rdf(g + 0x3C);
	out->sneak_accel = rdf(g + 0x40);
	out->airborne_accel = rdf(g + 0x44);
	out->pi_unk_2c = rdf(g + 0x48);
	out->pi_unk_6c = rdf(g + 0x4C);
	out->stun_move_penalty = rdf(g + 0x50);
	out->stun_turn_penalty = rdf(g + 0x54);
	out->stun_jump_penalty = rdf(g + 0x58);
	out->stun_min_time = rdf(g + 0x5C);
	out->stun_max_time = rdf(g + 0x60);
	out->fp_idle_min = rdf(g + 0x64);
	out->fp_idle_max = rdf(g + 0x68);
	out->fp_skip_fraction = rdf(g + 0x6C);
	out->magnetism_friction = rdf(g + 0x70);
	out->magnetism_adhesion = rdf(g + 0x74);
	out->inconsequential_scale = rdf(g + 0x78);
	out->look_a = rdf(g + 0x7C);
	out->look_b = rdf(g + 0x80);
	out->look_c = rdf(g + 0x84);
	out->look_pitch_rate_deg = rdf(g + 0x88);
	out->look_yaw_rate_deg = rdf(g + 0x8C);
	out->look_f = rdf(g + 0x90);
	out->min_weapon_swap_ticks = rd16(g + G_CTRL_TICKS);
	out->min_autolevel_ticks = rd16(g + G_CTRL_TICKS + 2);
	out->min_vehicle_flip_angle = rdf(g + G_FLIP_ANGLE);
	load_floats(out->look_function, g + G_LOOK_FUNCTION, 6);
	out->harmful_fall_min = rdf(g + G_FALLING);
	out->harmful_fall_max = rdf(g + G_FALLING + 4);
	out->max_fall_distance = rdf(g + G_FALLING + 8);
	out->gravity = rdf(g + G_FALLING + 12);
	out->fall_velocity_max = rdf(g + G_FALLING + 16);
	out->fall_velocity_harm_min = rdf(g + G_FALLING + 20);
	out->fall_velocity_harm_max = rdf(g + G_FALLING + 24);
	for (i = 0; i < 6; i++) out->damage_tags[i] = rd32(g + G_DAMAGE_TAGS + 4 * i);
	for (i = 0; i < 2; i++) {
		const uint8_t *e = g + G_GRENADES + 32 * i;
		out->grenades[i].max_count = rd16(e);
		out->grenades[i].unk = rd16(e + 2);
		out->grenades[i].throw_effect = rd32(e + 4);
		out->grenades[i].hud_interface = rd32(e + 8);
		out->grenades[i].equipment = rd32(e + 12);
		out->grenades[i].projectile = rd32(e + 16);
	}
}

float unitdef_difficulty(const unitdef_section *s, uint32_t group, uint32_t level)
{
	if (!s->globals || group >= UNITDEF_DIFFICULTY_GROUPS || level >= NUM_LEVELS) return 0.0f;
	return rdf(s->globals + G_DIFFICULTY + 16u * group + 4u * level);
}

static void neutral_biped(unitdef_biped_def *o)
{
	memset(o, 0, sizeof *o);
	o->pelvis_node = o->head_node = UNITDEF_NO_NODE;
}

int unitdef_find_biped(const unitdef_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->biped_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->bipeds + (size_t)mid * BIPED_REC + B_TAG);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

int unitdef_get_biped(const unitdef_section *s, uint32_t index, unitdef_biped_def *o)
{
	const uint8_t *b;
	unsigned i;
	neutral_biped(o);
	if (index >= s->biped_count) return 0;
	b = s->bipeds + (size_t)index * BIPED_REC;
	o->tag_id = rd32(b);
	o->model_tag = rd32(b + 0x04);
	o->anim_tag = rd32(b + 0x08);
	o->collision_tag = rd32(b + 0x0C);
	o->physics_tag = rd32(b + 0x10);
	o->camera_track_tag = rd32(b + 0x14);
	o->footsteps_tag = rd32(b + 0x18);
	o->melee_damage_tag = rd32(b + 0x1C);
	for (i = 0; i < UNITDEF_MAX_WEAPONS; i++) o->weapon_tags[i] = rd32(b + B_WEAPONS + 4 * i);
	for (i = 0; i < UNITDEF_MAX_DIALOGUE; i++) {
		o->dialogue_variant[i] = (uint16_t)rd32(b + B_DIALOGUE + 8 * i);
		o->dialogue_tag[i] = rd32(b + B_DIALOGUE + 8 * i + 4);
	}
	o->bounding_radius = rdf(b + 0x070);
	load_floats(o->bounding_offset, b + 0x074, 3);
	o->render_radius = rdf(b + 0x080);
	o->accel_scale = rdf(b + 0x084);
	o->camera_fov = rdf(b + 0x088);
	o->camera_stiffness = rdf(b + 0x08C);
	o->pitch_auto_level = rdf(b + 0x090);
	load_floats(o->pitch_range, b + 0x094, 2);
	o->stand_height = rdf(b + 0x09C);
	o->crouch_height = rdf(b + 0x0A0);
	o->radius = rdf(b + 0x0A4);
	o->autoaim_width = rdf(b + 0x0A8);
	o->stand_camera = rdf(b + 0x0AC);
	o->crouch_camera = rdf(b + 0x0B0);
	o->crouch_time = rdf(b + 0x0B4);
	o->jump_velocity = rdf(b + 0x0B8);
	o->max_slope = rdf(b + 0x0BC);
	o->downhill_falloff = rdf(b + 0x0C0);
	o->downhill_cutoff = rdf(b + 0x0C4);
	o->downhill_scale = rdf(b + 0x0C8);
	o->uphill_falloff = rdf(b + 0x0CC);
	o->uphill_cutoff = rdf(b + 0x0D0);
	o->uphill_scale = rdf(b + 0x0D4);
	o->turn_speed = rdf(b + 0x0D8);
	o->turn_threshold = rdf(b + 0x0DC);
	o->soft_landing_time = rdf(b + 0x0E0);
	o->hard_landing_time = rdf(b + 0x0E4);
	o->min_soft_landing_velocity = rdf(b + 0x0E8);
	o->min_hard_landing_velocity = rdf(b + 0x0EC);
	o->max_hard_landing_velocity = rdf(b + 0x0F0);
	o->death_landing_velocity = rdf(b + 0x0F4);
	o->max_velocity = rdf(b + 0x0F8);
	o->max_sidestep_velocity = rdf(b + 0x0FC);
	o->acceleration = rdf(b + 0x100);
	o->deceleration = rdf(b + 0x104);
	o->angular_velocity_max = rdf(b + 0x108);
	o->angular_acceleration_max = rdf(b + 0x10C);
	o->crouch_velocity_modifier = rdf(b + 0x110);
	o->aiming_velocity_max = rdf(b + 0x114);
	o->aiming_acceleration_max = rdf(b + 0x118);
	o->casual_aiming_modifier = rdf(b + 0x11C);
	o->looking_velocity_max = rdf(b + 0x120);
	o->looking_acceleration_max = rdf(b + 0x124);
	load_floats(o->anim_speed, b + 0x128, 4);
	o->grenade_velocity = rdf(b + 0x138);
	o->health = rdf(b + 0x13C);
	o->shield = rdf(b + 0x140);
	o->shield_recharge_time = rdf(b + 0x144);
	o->shield_stun_time = rdf(b + 0x148);
	o->shield_recharge_rate = rdf(b + 0x14C);
	o->unit_flags = rd32(b + B_UNIT_FLAGS);
	o->biped_flags = rd32(b + B_UNIT_FLAGS + 4);
	o->coll_flags = rd32(b + B_UNIT_FLAGS + 8);
	o->object_flags = rd16(b + B_U16S);
	o->default_team = rd16(b + B_U16S + 2);
	o->pelvis_node = rd16(b + B_U16S + 4);
	o->head_node = rd16(b + B_U16S + 6);
	o->shield_material = rd16(b + B_U16S + 8);
	o->body_material = rd16(b + B_U16S + 10);
	o->grenade_type = rd16(b + B_U16S + 12);
	o->grenade_count = rd16(b + B_U16S + 14);
	o->weapon_count = b[B_WEAPON_COUNT];
	o->dialogue_count = b[B_DIALOGUE_COUNT];
	load_str(o->name, b + B_NAME);
	load_str(o->hold_marker, b + B_HOLD);
	load_str(o->aux_marker, b + B_AUX);
	load_str(o->camera_marker, b + B_CAMERA);
	load_str(o->foot_marker[0], b + B_FOOT);
	load_str(o->foot_marker[1], b + B_FOOT + UNITDEF_NAME_LEN);
	return 1;
}

int unitdef_biped(const unitdef_section *s, uint32_t tag_id, unitdef_biped_def *out)
{
	int i = unitdef_find_biped(s, tag_id);
	if (i < 0) {
		neutral_biped(out);
		return 0;
	}
	return unitdef_get_biped(s, (uint32_t)i, out);
}

int unitdef_player_biped(const unitdef_section *s, unitdef_biped_def *out)
{
	if (!s->player_biped_index1) {
		neutral_biped(out);
		return 0;
	}
	return unitdef_get_biped(s, s->player_biped_index1 - 1, out);
}

int unitdef_find_vehicle(const unitdef_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->vehicle_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->vehicles + (size_t)mid * VEHICLE_REC);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

int unitdef_get_vehicle(const unitdef_section *s, uint32_t index, unitdef_vehicle_def *o)
{
	const uint8_t *v;
	unsigned i;
	memset(o, 0, sizeof *o);
	if (index >= s->vehicle_count) return 0;
	v = s->vehicles + (size_t)index * VEHICLE_REC;
	o->tag_id = rd32(v);
	o->model_tag = rd32(v + 0x04);
	o->anim_tag = rd32(v + 0x08);
	o->collision_tag = rd32(v + 0x0C);
	o->physics_tag = rd32(v + 0x10);
	o->camera_track_tag = rd32(v + 0x14);
	for (i = 0; i < UNITDEF_MAX_WEAPONS; i++) o->weapon_tags[i] = rd32(v + 0x18 + 4 * i);
	o->bounding_radius = rdf(v + 0x28);
	load_floats(o->bounding_offset, v + 0x2C, 3);
	o->accel_scale = rdf(v + 0x38);
	o->camera_fov = rdf(v + 0x3C);
	o->max_forward = rdf(v + 0x40);
	o->max_reverse = rdf(v + 0x44);
	o->accel = rdf(v + 0x48);
	o->decel = rdf(v + 0x4C);
	o->max_left_turn = rdf(v + 0x50);
	o->max_right_turn = rdf(v + 0x54);
	o->wheel_circumference = rdf(v + 0x58);
	o->turn_rate = rdf(v + 0x5C);
	o->health = rdf(v + 0x60);
	o->shield = rdf(v + 0x64);
	o->shield_recharge_time = rdf(v + 0x68);
	o->unit_flags = rd32(v + V_FLAGS);
	o->vehicle_flags = rd32(v + V_FLAGS + 4);
	o->coll_flags = rd32(v + V_FLAGS + 8);
	o->type = rd16(v + V_TYPE);
	o->first_seat = rd16(v + V_TYPE + 2);
	o->seat_count = rd16(v + V_TYPE + 4);
	o->object_flags = rd16(v + V_TYPE + 6);
	o->default_team = rd16(v + V_TYPE + 8);
	o->shield_material = rd16(v + V_TYPE + 10);
	o->weapon_count = v[V_WEAPON_COUNT];
	load_str(o->name, v + V_NAME);
	return 1;
}

int unitdef_vehicle(const unitdef_section *s, uint32_t tag_id, unitdef_vehicle_def *out)
{
	int i = unitdef_find_vehicle(s, tag_id);
	if (i < 0) {
		memset(out, 0, sizeof *out);
		return 0;
	}
	return unitdef_get_vehicle(s, (uint32_t)i, out);
}

int unitdef_vehicle_seat(const unitdef_section *s, const unitdef_vehicle_def *v, uint32_t i, unitdef_seat_def *o)
{
	const uint8_t *p;
	uint64_t idx;
	memset(o, 0, sizeof *o);
	if (i >= v->seat_count) return 0;
	idx = (uint64_t)v->first_seat + i;
	if (idx >= s->seat_count) return 0;            /* a def not taken from this section */
	p = s->seats + (size_t)idx * SEAT_REC;
	o->flags = rd32(p);
	o->pitch_min = rdf(p + 4);
	o->pitch_max = rdf(p + 8);
	load_str(o->label, p + 0x10);
	load_str(o->marker, p + 0x30);
	load_str(o->camera_marker, p + 0x50);
	return 1;
}

void unitdef_material(const unitdef_section *s, uint32_t index, unitdef_material_def *o)
{
	const uint8_t *m;
	memset(o, 0, sizeof *o);
	if (index >= s->material_count) return;
	m = s->materials + (size_t)index * MATERIAL_REC;
	o->melee_sound = rd32(m);
	o->impact_sound = rd32(m + 4);
	o->effect = rd32(m + 8);
	o->particle_tag = rd32(m + 12);
	o->param_a = rdf(m + 0x10);
	o->param_b = rdf(m + 0x14);
	o->particle_count = rd16(m + 0x18);
}

const char *unitdef_material_name(uint32_t index)
{
	static const char *const names[] = {
		"dirt", "sand", "stone", "snow", "wood", "metal (hollow)", "metal (thin)", "metal (thick)", "rubber", "glass",
		"force field", "grunt", "hunter armor", "hunter skin", "elite", "jackal", "jackal energy shield", "engineer skin",
		"engineer force field", "flood combat form", "flood carrier form", "cyborg armor", "cyborg energy shield",
		"human armor", "human skin", "sentinel", "monitor", "plastic", "water", "leaves", "elite energy shield", "ice",
		"hunter shield"
	};
	return index < sizeof names / sizeof names[0] ? names[index] : "?";
}
