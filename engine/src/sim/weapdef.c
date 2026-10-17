/* WEAP section reader and weapon state machine (docs/18-weap-section.md). Pure C11, no heap, no double.
 * weap_open validates every table once (sizes, alignment, ranges, sort order, index ranges, string terminators,
 * finite floats), so the accessors can never read out of bounds and the simulation never sees a NaN, whatever the
 * bytes. Records are decoded with byte loads: no alignment assumptions on the section buffer. */
#include "sim/weapdef.h"
#include <string.h>

#define HEADER_SIZE 128u
#define WEAP_VERSION 1u
#define REC_WEAPON 256u
#define REC_MAGAZINE 64u
#define REC_TRIGGER 128u
#define REC_FX 32u
#define REC_PROJECTILE 192u
#define REC_MATERIAL 64u
#define REC_DAMAGE 384u
#define REC_EQUIPMENT 96u
#define REC_GRENADE 32u
#define REC_COLLECTION 32u
#define REC_PERMUTATION 32u
#define MAX_RECORDS (1u << 20)
#define N_TABLES 11u
#define COUNTS_AT 0x0Cu
#define OFFSETS_AT 0x38u

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
static int terminated(const uint8_t *p, unsigned n) { return p[n - 1] == 0; }

/* every one of n floats starting at p + off, step 4, is finite and |x| <= 1e7 (so that tick conversions cannot overflow) */
static int floats_ok(const uint8_t *p, unsigned off, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		float f;
		if (!finite_at(p + off + 4 * i)) return 0;
		f = rdf(p + off + 4 * i);
		if (f > 1e7f || f < -1e7f) return 0;
	}
	return 1;
}

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

weap_status weap_open(weap_section *s, const uint8_t *data, size_t size)
{
	static const uint32_t rec[N_TABLES] = { REC_WEAPON, REC_MAGAZINE, REC_TRIGGER, REC_FX, REC_PROJECTILE, REC_MATERIAL,
	                                        REC_DAMAGE, REC_EQUIPMENT, REC_GRENADE, REC_COLLECTION, REC_PERMUTATION };
	uint32_t cnt[N_TABLES], off[N_TABLES], i, prev;
	const uint8_t *tab[N_TABLES];

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return WEAP_ERR_TRUNCATED;
	if (memcmp(data, "WEAP", 4) != 0) return WEAP_ERR_MAGIC;
	if (rd16(data + 4) != WEAP_VERSION) return WEAP_ERR_VERSION;
	if (rd32(data + 8) > size) return WEAP_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return WEAP_ERR_LAYOUT;
	for (i = 0; i < N_TABLES; i++) {
		cnt[i] = rd32(data + COUNTS_AT + 4 * i);
		off[i] = rd32(data + OFFSETS_AT + 4 * i);
		if (cnt[i] > MAX_RECORDS || !table_ok(off[i], cnt[i], rec[i], size)) return WEAP_ERR_LAYOUT;
		tab[i] = data + off[i];
	}
	/* u16 indices: weapons, magazines, triggers, fx, projectiles, materials, damages, equipment */
	for (i = 0; i < 8; i++)
		if (cnt[i] > 0xFFFFu) return WEAP_ERR_LAYOUT;

	for (i = 0, prev = 0; i < cnt[0]; i++) {                     /* weapons */
		const uint8_t *w = tab[0] + (size_t)i * REC_WEAPON;
		uint32_t fm = rd16(w + 0xA8), nm = rd16(w + 0xAA), ft = rd16(w + 0xAC), nt = rd16(w + 0xAE), k;
		if (i && rd32(w) <= prev) return WEAP_ERR_LAYOUT;
		prev = rd32(w);
		if (!floats_ok(w, 0x50, 21)) return WEAP_ERR_LAYOUT;
		if (nm > WEAP_MAX_MAGAZINES || nt > WEAP_MAX_TRIGGERS) return WEAP_ERR_LAYOUT;
		if ((uint64_t)fm + nm > cnt[1] || (uint64_t)ft + nt > cnt[2]) return WEAP_ERR_LAYOUT;
		if (!terminated(w + 0xC0, WEAP_LABEL_LEN) || !terminated(w + 0xD0, WEAP_NAME_LEN)) return WEAP_ERR_LAYOUT;
		for (k = 0; k < nt; k++) {
			uint32_t mg = rd16(tab[2] + (size_t)(ft + k) * REC_TRIGGER + 0x18);
			if (mg != WEAP_NO_MAGAZINE && mg >= nm) return WEAP_ERR_LAYOUT;
		}
	}
	for (i = 0; i < cnt[1]; i++) {                               /* magazines */
		const uint8_t *m = tab[1] + (size_t)i * REC_MAGAZINE;
		if (!floats_ok(m, 0x10, 2) || rdf(m + 0x10) < 0.0f || rdf(m + 0x14) < 0.0f) return WEAP_ERR_LAYOUT;
		if (rd16(m + 0x0E) > 2) return WEAP_ERR_LAYOUT;
	}
	for (i = 0; i < cnt[2]; i++) {                               /* triggers */
		const uint8_t *t = tab[2] + (size_t)i * REC_TRIGGER;
		uint32_t ff = rd16(t + 0x74), nf = rd16(t + 0x76);
		if (!floats_ok(t, 0x04, 5) || !floats_ok(t, 0x28, 9) || !floats_ok(t, 0x4C, 8)) return WEAP_ERR_LAYOUT;
		if (rdf(t + 0x4C) < 0.0f || rdf(t + 0x50) < 0.0f || rdf(t + 0x64) < 0.0f || rdf(t + 0x68) < 0.0f) return WEAP_ERR_LAYOUT;
		if (rdf(t + 0x04) < 0.0f || rdf(t + 0x08) < 0.0f || rdf(t + 0x04) > 1000.0f || rdf(t + 0x08) > 1000.0f) return WEAP_ERR_LAYOUT;
		if (nf > WEAP_MAX_FX || (uint64_t)ff + nf > cnt[3]) return WEAP_ERR_LAYOUT;
	}
	for (i = 0; i < cnt[3]; i++)                                 /* fx */
		if (tab[3][(size_t)i * REC_FX + 28] > 7) return WEAP_ERR_LAYOUT;
	for (i = 0, prev = 0; i < cnt[4]; i++) {                     /* projectiles */
		const uint8_t *p = tab[4] + (size_t)i * REC_PROJECTILE;
		if (i && rd32(p) <= prev) return WEAP_ERR_LAYOUT;
		prev = rd32(p);
		if (!floats_ok(p, 0x10, 17) || !floats_ok(p, 0x74, 1)) return WEAP_ERR_LAYOUT;
		if (rd16(p + 0x72) > WEAP_MAX_MATERIALS || (uint64_t)rd16(p + 0x70) + rd16(p + 0x72) > cnt[5]) return WEAP_ERR_LAYOUT;
		if (!terminated(p + 0x80, WEAP_NAME_LEN)) return WEAP_ERR_LAYOUT;
	}
	for (i = 0; i < cnt[5]; i++) {                               /* material responses */
		const uint8_t *m = tab[5] + (size_t)i * REC_MATERIAL;
		if (!floats_ok(m, 0x0C, 1) || !floats_ok(m, 0x10, 4) || !floats_ok(m, 0x24, 2) || !floats_ok(m, 0x30, 4)) return WEAP_ERR_LAYOUT;
	}
	for (i = 0, prev = 0; i < cnt[6]; i++) {                     /* damage effects */
		const uint8_t *d = tab[6] + (size_t)i * REC_DAMAGE;
		if (i && rd32(d) <= prev) return WEAP_ERR_LAYOUT;
		prev = rd32(d);
		if (!floats_ok(d, 0x04, 3) || !floats_ok(d, 0x18, 1) || !floats_ok(d, 0x20, 5) || !floats_ok(d, 0x34, 2) ||
		    !floats_ok(d, 0x40, 2) || !floats_ok(d, 0x4C, 1) || !floats_ok(d, 0x54, 5) || !floats_ok(d, 0x68, 1) ||
		    !floats_ok(d, 0x70, 2) || !floats_ok(d, 0x7C, 2) || !floats_ok(d, 0x88, 6) || !floats_ok(d, 0xA8, 9) ||
		    !floats_ok(d, 0xD0, WEAP_MAX_MATERIALS))
			return WEAP_ERR_LAYOUT;
		if (!terminated(d + 0x154, WEAP_NAME_LEN)) return WEAP_ERR_LAYOUT;
	}
	for (i = 0, prev = 0; i < cnt[7]; i++) {                     /* equipment */
		const uint8_t *q = tab[7] + (size_t)i * REC_EQUIPMENT;
		if (i && rd32(q) <= prev) return WEAP_ERR_LAYOUT;
		prev = rd32(q);
		if (!floats_ok(q, 0x14, 1) || !floats_ok(q, 0x28, 3)) return WEAP_ERR_LAYOUT;
		if (!terminated(q + 0x40, WEAP_NAME_LEN)) return WEAP_ERR_LAYOUT;
	}
	for (i = 0, prev = 0; i < cnt[9]; i++) {                     /* item collections */
		const uint8_t *c = tab[9] + (size_t)i * REC_COLLECTION;
		if (i && rd32(c) <= prev) return WEAP_ERR_LAYOUT;
		prev = rd32(c);
		if ((uint64_t)rd32(c + 4) + rd32(c + 8) > cnt[10]) return WEAP_ERR_LAYOUT;
	}
	for (i = 0; i < cnt[10]; i++)
		if (!floats_ok(tab[10] + (size_t)i * REC_PERMUTATION, 0, 1)) return WEAP_ERR_LAYOUT;

	s->data = data;
	s->size = size;
	s->n_weapons = cnt[0]; s->n_magazines = cnt[1]; s->n_triggers = cnt[2]; s->n_fx = cnt[3];
	s->n_projectiles = cnt[4]; s->n_materials = cnt[5]; s->n_damages = cnt[6]; s->n_equipment = cnt[7];
	s->n_grenades = cnt[8]; s->n_collections = cnt[9]; s->n_permutations = cnt[10];
	s->weapons = tab[0]; s->magazines = tab[1]; s->triggers = tab[2]; s->fx = tab[3]; s->projectiles = tab[4];
	s->materials = tab[5]; s->damages = tab[6]; s->equipment = tab[7]; s->grenades = tab[8]; s->collections = tab[9];
	s->permutations = tab[10];
	return WEAP_OK;
}

static int find_tag(const uint8_t *table, uint32_t count, uint32_t rec, uint32_t tag_id)
{
	uint32_t lo = 0, hi = count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(table + (size_t)mid * rec);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}
int weap_find_weapon(const weap_section *s, uint32_t tag_id) { return find_tag(s->weapons, s->n_weapons, REC_WEAPON, tag_id); }
int weap_find_projectile(const weap_section *s, uint32_t tag_id) { return find_tag(s->projectiles, s->n_projectiles, REC_PROJECTILE, tag_id); }
int weap_find_damage(const weap_section *s, uint32_t tag_id) { return find_tag(s->damages, s->n_damages, REC_DAMAGE, tag_id); }
int weap_find_equipment(const weap_section *s, uint32_t tag_id) { return find_tag(s->equipment, s->n_equipment, REC_EQUIPMENT, tag_id); }
int weap_find_collection(const weap_section *s, uint32_t tag_id) { return find_tag(s->collections, s->n_collections, REC_COLLECTION, tag_id); }

static void rd_floats(float *out, const uint8_t *p, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) out[i] = rdf(p + 4 * i);
}

static void rd_magazine(weap_magazine *g, const uint8_t *m)
{
	memset(g, 0, sizeof *g);
	g->flags = rd32(m);
	g->rounds_recharged = rd16(m + 4);
	g->rounds_total_initial = rd16(m + 6);
	g->rounds_total_max = rd16(m + 8);
	g->rounds_loaded_max = rd16(m + 0x0A);
	g->rounds_reloaded = rd16(m + 0x0C);
	g->equipment_count = rd16(m + 0x0E);
	g->reload_time = rdf(m + 0x10);
	g->chamber_time = rdf(m + 0x14);
	g->reload_effect = rd32(m + 0x18);
	g->chamber_effect = rd32(m + 0x1C);
	g->equipment_tag[0] = rd32(m + 0x20);
	g->equipment_tag[1] = rd32(m + 0x24);
	g->equipment_rounds[0] = rd16(m + 0x28);
	g->equipment_rounds[1] = rd16(m + 0x2A);
}

static void rd_trigger(weap_trigger *g, const uint8_t *t)
{
	memset(g, 0, sizeof *g);
	g->flags = rd32(t);
	g->rps_min = rdf(t + 4); g->rps_max = rdf(t + 8); g->accel_time = rdf(t + 0x0C); g->decel_time = rdf(t + 0x10);
	g->blurred_rate = rdf(t + 0x14);
	g->magazine = rd16(t + 0x18); g->rounds_per_shot = rd16(t + 0x1A); g->min_rounds_loaded = rd16(t + 0x1C);
	g->tracer_interval = rd16(t + 0x1E); g->firing_noise = rd16(t + 0x20); g->projectiles_per_shot = rd16(t + 0x22);
	g->distribution_function = rd16(t + 0x24); g->overcharged_action = rd16(t + 0x26);
	g->error_accel_time = rdf(t + 0x28); g->error_decel_time = rdf(t + 0x2C); g->minimum_error = rdf(t + 0x30);
	g->error_min = rdf(t + 0x34); g->error_max = rdf(t + 0x38); g->distribution_angle = rdf(t + 0x3C);
	rd_floats(g->fp_offset, t + 0x40, 3);
	g->charging_time = rdf(t + 0x4C); g->charged_time = rdf(t + 0x50); g->charged_illumination = rdf(t + 0x54);
	g->spew_time = rdf(t + 0x58); g->ejection_recovery = rdf(t + 0x5C); g->illumination_recovery = rdf(t + 0x60);
	g->heat_per_round = rdf(t + 0x64); g->age_per_round = rdf(t + 0x68);
	g->projectile = rd32(t + 0x6C); g->charging_effect = rd32(t + 0x70);
	g->first_fx = rd16(t + 0x74); g->fx_count = rd16(t + 0x76);
}

int weap_get_weapon(const weap_section *s, uint32_t index, weap_weapon *g)
{
	const uint8_t *w;
	unsigned k;
	memset(g, 0, sizeof *g);
	if (index >= s->n_weapons) return 0;
	w = s->weapons + (size_t)index * REC_WEAPON;
	g->tag_id = rd32(w); g->model_tag = rd32(w + 4); g->anim_tag = rd32(w + 8); g->fp_model_tag = rd32(w + 0x0C);
	g->fp_anim_tag = rd32(w + 0x10); g->hud_tag = rd32(w + 0x14); g->pickup_sound = rd32(w + 0x18);
	g->zoom_in_sound = rd32(w + 0x1C); g->zoom_out_sound = rd32(w + 0x20); g->collision_sound = rd32(w + 0x24);
	g->ready_effect = rd32(w + 0x28); g->overheated_effect = rd32(w + 0x2C); g->detonation_effect = rd32(w + 0x30);
	g->melee_damage = rd32(w + 0x34); g->melee_response = rd32(w + 0x38);
	g->flags = rd32(w + 0x40); g->item_flags = rd32(w + 0x44);
	g->secondary_mode = rd16(w + 0x48); g->max_alternate_shots = rd16(w + 0x4A); g->weapon_type = rd16(w + 0x4C);
	g->zoom_levels = rd16(w + 0x4E);
	g->zoom_min = rdf(w + 0x50); g->zoom_max = rdf(w + 0x54);
	g->heat_recovery = rdf(w + 0x58); g->overheated = rdf(w + 0x5C); g->heat_detonation = rdf(w + 0x60);
	g->heat_detonation_fraction = rdf(w + 0x64); g->heat_loss = rdf(w + 0x68); g->heat_illumination = rdf(w + 0x6C);
	g->near_reticle = rdf(w + 0x70); g->far_reticle = rdf(w + 0x74); g->intersection_reticle = rdf(w + 0x78);
	g->autoaim_angle = rdf(w + 0x7C); g->autoaim_range = rdf(w + 0x80); g->magnetism_angle = rdf(w + 0x84);
	g->magnetism_range = rdf(w + 0x88); g->deviation_angle = rdf(w + 0x8C);
	g->camo_ding = rdf(w + 0x90); g->camo_regrowth = rdf(w + 0x94); g->item_scale = rdf(w + 0x98);
	g->detonation_delay[0] = rdf(w + 0x9C); g->detonation_delay[1] = rdf(w + 0xA0);
	g->hud_message = rd16(w + 0xA4); g->sort_order = rd16(w + 0xA6);
	for (k = 0; k < 8; k++) g->fp_ticks[k] = rd16(w + 0xB0 + 2 * k);
	g->magazine_count = (uint8_t)rd16(w + 0xAA);
	g->trigger_count = (uint8_t)rd16(w + 0xAE);
	memcpy(g->label, w + 0xC0, WEAP_LABEL_LEN);
	g->label[WEAP_LABEL_LEN - 1] = 0;
	memcpy(g->name, w + 0xD0, WEAP_NAME_LEN);
	g->name[WEAP_NAME_LEN - 1] = 0;
	for (k = 0; k < g->magazine_count; k++)
		rd_magazine(&g->magazine[k], s->magazines + ((size_t)rd16(w + 0xA8) + k) * REC_MAGAZINE);
	for (k = 0; k < g->trigger_count; k++)
		rd_trigger(&g->trigger[k], s->triggers + ((size_t)rd16(w + 0xAC) + k) * REC_TRIGGER);
	return 1;
}

int weap_get_fx(const weap_section *s, const weap_trigger *t, uint32_t k, weap_fx *g)
{
	const uint8_t *f;
	unsigned i;
	memset(g, 0, sizeof *g);
	if (k >= t->fx_count || (uint64_t)t->first_fx + k >= s->n_fx) return 0;
	f = s->fx + ((size_t)t->first_fx + k) * REC_FX;
	g->shots_lo = rd16(f); g->shots_hi = rd16(f + 2);
	for (i = 0; i < 3; i++) {
		g->effects[i] = rd32(f + 4 + 4 * i);
		g->damages[i] = rd32(f + 16 + 4 * i);
	}
	g->sound_mask = f[28];
	return 1;
}

int weap_get_projectile(const weap_section *s, uint32_t index, weap_projectile *g)
{
	const uint8_t *p;
	memset(g, 0, sizeof *g);
	if (index >= s->n_projectiles) return 0;
	p = s->projectiles + (size_t)index * REC_PROJECTILE;
	g->tag_id = rd32(p); g->model_tag = rd32(p + 4); g->flags = rd32(p + 8);
	g->timer_starts = rd16(p + 0x0C); g->impact_noise = rd16(p + 0x0E);
	g->ai_perception_radius = rdf(p + 0x10); g->collision_radius = rdf(p + 0x14); g->arming_time = rdf(p + 0x18);
	g->danger_radius = rdf(p + 0x1C); g->timer[0] = rdf(p + 0x20); g->timer[1] = rdf(p + 0x24);
	g->min_velocity = rdf(p + 0x28); g->max_range = rdf(p + 0x2C); g->air_gravity = rdf(p + 0x30);
	g->air_range[0] = rdf(p + 0x34); g->air_range[1] = rdf(p + 0x38); g->water_gravity = rdf(p + 0x3C);
	g->water_range[0] = rdf(p + 0x40); g->water_range[1] = rdf(p + 0x44);
	g->initial_velocity = rdf(p + 0x48); g->final_velocity = rdf(p + 0x4C); g->guided_angular_velocity = rdf(p + 0x50);
	g->detonation_noise = rd16(p + 0x54);
	g->super_detonation = rd32(p + 0x58); g->detonation_effect = rd32(p + 0x5C); g->detonation_started = rd32(p + 0x60);
	g->flyby_sound = rd32(p + 0x64); g->attached_damage = rd32(p + 0x68); g->impact_damage = rd32(p + 0x6C);
	g->first_material = rd16(p + 0x70); g->material_count = rd16(p + 0x72);
	g->object_radius = rdf(p + 0x74);
	memcpy(g->name, p + 0x80, WEAP_NAME_LEN);
	g->name[WEAP_NAME_LEN - 1] = 0;
	return 1;
}

int weap_get_material(const weap_section *s, const weap_projectile *p, uint32_t material, weap_material_response *g)
{
	const uint8_t *m;
	memset(g, 0, sizeof *g);
	if (material >= p->material_count || (uint64_t)p->first_material + material >= s->n_materials) return 0;
	m = s->materials + ((size_t)p->first_material + material) * REC_MATERIAL;
	g->flags = rd16(m); g->response = rd16(m + 2); g->effect = rd32(m + 4);
	g->potential_response = rd16(m + 8); g->potential_flags = rd16(m + 0x0A); g->skip_fraction = rdf(m + 0x0C);
	g->angle[0] = rdf(m + 0x10); g->angle[1] = rdf(m + 0x14); g->velocity[0] = rdf(m + 0x18); g->velocity[1] = rdf(m + 0x1C);
	g->potential_effect = rd32(m + 0x20); g->angular_noise = rdf(m + 0x24); g->velocity_noise = rdf(m + 0x28);
	g->detonation_effect = rd32(m + 0x2C); g->initial_friction = rdf(m + 0x30); g->max_distance = rdf(m + 0x34);
	g->parallel_friction = rdf(m + 0x38); g->perpendicular_friction = rdf(m + 0x3C);
	return 1;
}

int weap_get_damage(const weap_section *s, uint32_t index, weap_damage *g)
{
	const uint8_t *d;
	memset(g, 0, sizeof *g);
	if (index >= s->n_damages) return 0;
	d = s->damages + (size_t)index * REC_DAMAGE;
	g->tag_id = rd32(d);
	g->radius[0] = rdf(d + 4); g->radius[1] = rdf(d + 8); g->cutoff_scale = rdf(d + 0x0C); g->effect_flags = rd32(d + 0x10);
	g->flash_type = rd16(d + 0x14); g->flash_priority = rd16(d + 0x16); g->flash_duration = rdf(d + 0x18);
	g->flash_fade = rd16(d + 0x1C); g->flash_intensity = rdf(d + 0x20); rd_floats(g->flash_color, d + 0x24, 4);
	g->low_rumble[0] = rdf(d + 0x34); g->low_rumble[1] = rdf(d + 0x38); g->low_rumble_fade = rd16(d + 0x3C);
	g->high_rumble[0] = rdf(d + 0x40); g->high_rumble[1] = rdf(d + 0x44); g->high_rumble_fade = rd16(d + 0x48);
	g->impulse_duration = rdf(d + 0x4C); g->impulse_fade = rd16(d + 0x50); g->impulse_rotation = rdf(d + 0x54);
	g->impulse_pushback = rdf(d + 0x58); g->impulse_jitter[0] = rdf(d + 0x5C); g->impulse_jitter[1] = rdf(d + 0x60);
	g->permanent_angle = rdf(d + 0x64);
	g->shake_duration = rdf(d + 0x68); g->shake_falloff = rd16(d + 0x6C); g->shake_translation = rdf(d + 0x70);
	g->shake_rotation = rdf(d + 0x74); g->shake_wobble_function = rd16(d + 0x78); g->shake_wobble_period = rdf(d + 0x7C);
	g->shake_wobble_weight = rdf(d + 0x80);
	g->sound = rd32(d + 0x84);
	rd_floats(g->break_forward, d + 0x88, 3); rd_floats(g->break_outward, d + 0x94, 3);
	g->side_effect = rd16(d + 0xA0); g->category = rd16(d + 0xA2); g->damage_flags = rd32(d + 0xA4);
	g->aoe_core_radius = rdf(d + 0xA8); g->damage_lower = rdf(d + 0xAC); g->damage_upper[0] = rdf(d + 0xB0);
	g->damage_upper[1] = rdf(d + 0xB4); g->vehicle_passthrough = rdf(d + 0xB8); g->active_camo = rdf(d + 0xBC);
	g->stun = rdf(d + 0xC0); g->max_stun = rdf(d + 0xC4); g->stun_time = rdf(d + 0xC8);
	rd_floats(g->modifiers, d + 0xD0, WEAP_MAX_MATERIALS);
	memcpy(g->name, d + 0x154, WEAP_NAME_LEN);
	g->name[WEAP_NAME_LEN - 1] = 0;
	return 1;
}

int weap_get_equipment(const weap_section *s, uint32_t index, weap_equipment *g)
{
	const uint8_t *q;
	memset(g, 0, sizeof *g);
	if (index >= s->n_equipment) return 0;
	q = s->equipment + (size_t)index * REC_EQUIPMENT;
	g->tag_id = rd32(q); g->model_tag = rd32(q + 4); g->anim_tag = rd32(q + 8); g->item_flags = rd32(q + 0x0C);
	g->powerup_type = rd16(q + 0x10); g->grenade_type = rd16(q + 0x12); g->powerup_time = rdf(q + 0x14);
	g->pickup_sound = rd32(q + 0x18); g->collision_sound = rd32(q + 0x1C); g->detonating_effect = rd32(q + 0x20);
	g->detonation_effect = rd32(q + 0x24); g->detonation_delay[0] = rdf(q + 0x28); g->detonation_delay[1] = rdf(q + 0x2C);
	g->item_scale = rdf(q + 0x30); g->hud_message = rd16(q + 0x34);
	memcpy(g->name, q + 0x40, WEAP_NAME_LEN);
	g->name[WEAP_NAME_LEN - 1] = 0;
	return 1;
}

int weap_get_grenade(const weap_section *s, uint32_t type, weap_grenade *g)
{
	const uint8_t *p;
	memset(g, 0, sizeof *g);
	if (type >= s->n_grenades) return 0;
	p = s->grenades + (size_t)type * REC_GRENADE;
	g->max_count = rd16(p); g->mp_spawn_default = rd16(p + 2); g->throw_effect = rd32(p + 4); g->hud_tag = rd32(p + 8);
	g->equipment = rd32(p + 12); g->projectile = rd32(p + 16);
	return 1;
}

int weap_collection(const weap_section *s, uint32_t index, uint32_t *spawn_time, uint32_t *count)
{
	const uint8_t *c;
	*spawn_time = *count = 0;
	if (index >= s->n_collections) return 0;
	c = s->collections + (size_t)index * REC_COLLECTION;
	*count = rd32(c + 8);
	*spawn_time = rd32(c + 12);
	return 1;
}

int weap_collection_entry(const weap_section *s, uint32_t index, uint32_t k, float *weight, uint32_t *item_tag)
{
	const uint8_t *c;
	const uint8_t *e;
	*weight = 0.0f;
	*item_tag = 0;
	if (index >= s->n_collections) return 0;
	c = s->collections + (size_t)index * REC_COLLECTION;
	if (k >= rd32(c + 8)) return 0;
	e = s->permutations + ((size_t)rd32(c + 4) + k) * REC_PERMUTATION;
	*weight = rdf(e);
	*item_tag = rd32(e + 4);
	return 1;
}

/* ---- helpers ---- */
static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

float weap_damage_at_range(const weap_projectile *p, const weap_damage *d, float dist, float rand01)
{
	float upper = d->damage_upper[0] + clamp01(rand01) * (d->damage_upper[1] - d->damage_upper[0]);
	float lo = p->air_range[0], hi = p->air_range[1];
	if (dist <= lo) return upper;
	if (hi <= lo || dist >= hi) return d->damage_lower;
	return upper + (d->damage_lower - upper) * ((dist - lo) / (hi - lo));
}

float weap_damage_at_radius(const weap_damage *d, float dist, float rand01)
{
	float upper = d->damage_upper[0] + clamp01(rand01) * (d->damage_upper[1] - d->damage_upper[0]);
	float outer = d->radius[1], inner = d->aoe_core_radius > d->radius[0] ? d->aoe_core_radius : d->radius[0];
	if (outer <= 0.0f) return upper;
	if (dist >= outer) return 0.0f;
	if (inner > outer) inner = outer;
	if (dist <= inner) return upper;
	return upper + (d->damage_lower - upper) * ((dist - inner) / (outer - inner));
}

float weap_projectile_speed(const weap_projectile *p, uint32_t age_ticks, uint32_t lifetime_ticks)
{
	float t;
	if (lifetime_ticks == 0 || age_ticks >= lifetime_ticks) return p->final_velocity;
	t = (float)age_ticks / (float)lifetime_ticks;
	return p->initial_velocity + (p->final_velocity - p->initial_velocity) * t;
}

/* ---- weapon state machine ---- */
static uint16_t ticks_from(float seconds)
{
	float t;
	if (!(seconds > 0.0f)) return 0;
	t = seconds * (float)WEAP_TICKS_PER_SEC + 0.5f;
	if (t >= 65535.0f) return 65535u;
	if (t < 1.0f) return 1u;                 /* a positive time lasts at least one tick */
	return (uint16_t)t;
}

uint16_t weap_ready_ticks(const weap_weapon *w)
{
	return w->fp_ticks[WEAP_FP_READY] ? w->fp_ticks[WEAP_FP_READY] : WEAP_DEFAULT_READY_TICKS;
}
uint16_t weap_put_away_ticks(const weap_weapon *w)
{
	return w->fp_ticks[WEAP_FP_PUT_AWAY] ? w->fp_ticks[WEAP_FP_PUT_AWAY] : WEAP_DEFAULT_PUT_AWAY_TICKS;
}
uint16_t weap_melee_ticks(const weap_weapon *w)
{
	return w->fp_ticks[WEAP_FP_MELEE] ? w->fp_ticks[WEAP_FP_MELEE] : WEAP_DEFAULT_MELEE_TICKS;
}
uint16_t weap_reload_ticks(const weap_weapon *w, unsigned mag, int empty)
{
	uint16_t t = 0, a;
	if (mag < w->magazine_count) t = ticks_from(w->magazine[mag].reload_time);
	if (t) return t;
	a = empty ? w->fp_ticks[WEAP_FP_RELOAD_EMPTY] : w->fp_ticks[WEAP_FP_RELOAD_FULL];
	if (!a) a = empty ? w->fp_ticks[WEAP_FP_RELOAD_FULL] : w->fp_ticks[WEAP_FP_RELOAD_EMPTY];
	return a ? a : WEAP_DEFAULT_RELOAD_TICKS;
}

static int has_ammo(const weap_weapon *w, const weap_trigger *t)
{
	return t->magazine != WEAP_NO_MAGAZINE && t->magazine < w->magazine_count && w->magazine[t->magazine].rounds_loaded_max > 0;
}

/* is there enough in the magazine for one shot of this trigger (always true without ammunition accounting) */
static int ammo_ok(const weap_state *st, const weap_weapon *w, const weap_trigger *t)
{
	unsigned need = t->rounds_per_shot ? t->rounds_per_shot : 1u, floor_ = t->min_rounds_loaded > need ? t->min_rounds_loaded : need;
	unsigned have;
	if (!has_ammo(w, t)) return 1;
	have = st->loaded[t->magazine];
	return have >= floor_ || ((t->flags & WEAP_T_PARTIAL_AMMO) && have > 0);
}

static float chamber_ticks(const weap_weapon *w, const weap_trigger *t)
{
	if (t->magazine < w->magazine_count && (w->magazine[t->magazine].flags & WEAP_MAG_CHAMBER_EACH))
		return w->magazine[t->magazine].chamber_time * (float)WEAP_TICKS_PER_SEC;
	return 0.0f;
}

float weap_shot_interval(const weap_weapon *w, unsigned t, float rate)
{
	if (t >= w->trigger_count) return 0.0f;
	if (rate > 0.0f) {
		float iv = (float)WEAP_TICKS_PER_SEC / rate;
		float ch = chamber_ticks(w, &w->trigger[t]);
		return iv > ch ? iv : ch;
	}
	return chamber_ticks(w, &w->trigger[t]);
}

void weapon_state_set_ammo(weap_state *st, const weap_weapon *w, unsigned mag, unsigned loaded, unsigned reserve)
{
	const weap_magazine *m;
	if (mag >= w->magazine_count) return;
	m = &w->magazine[mag];
	if (loaded > m->rounds_loaded_max) loaded = m->rounds_loaded_max;
	if (m->rounds_total_max && reserve > m->rounds_total_max) reserve = m->rounds_total_max;
	if (reserve > 0xFFFFu) reserve = 0xFFFFu;
	st->loaded[mag] = (uint16_t)loaded;
	st->reserve[mag] = (uint16_t)reserve;
}

void weapon_state_init(weap_state *st, const weap_weapon *w)
{
	unsigned i;
	memset(st, 0, sizeof *st);
	st->mode = WEAP_MODE_READYING;
	st->timer = weap_ready_ticks(w);
	for (i = 0; i < w->magazine_count; i++)
		weapon_state_set_ammo(st, w, i, w->magazine[i].rounds_loaded_max, w->magazine[i].rounds_total_initial);
	for (i = 0; i < w->trigger_count; i++) st->rate[i] = w->trigger[i].rps_min;
}

static void start_reload(weap_state *st, const weap_weapon *w, unsigned mag, uint32_t extra_flags, weap_events *ev)
{
	st->mode = WEAP_MODE_RELOADING;
	st->reload_mag = (uint8_t)mag;
	st->timer = weap_reload_ticks(w, mag, st->loaded[mag] == 0);
	ev->flags |= WEAP_EV_RELOAD_START | extra_flags;
}

static int can_reload(const weap_state *st, const weap_weapon *w, unsigned mag)
{
	return mag < w->magazine_count && w->magazine[mag].rounds_loaded_max > 0 &&
	       st->loaded[mag] < w->magazine[mag].rounds_loaded_max && st->reserve[mag] > 0;
}

static void set_overheated(weap_state *st, const weap_weapon *w, weap_events *ev)
{
	unsigned t;
	st->mode = WEAP_MODE_OVERHEATED;
	for (t = 0; t < w->trigger_count; t++) st->charge[t] = 0.0f;
	st->charged_sent = 0;
	ev->flags |= WEAP_EV_OVERHEATED;
}

static void finish_reload(weap_state *st, const weap_weapon *w, weap_events *ev)
{
	unsigned mag = st->reload_mag, t, take, space, n;
	const weap_magazine *m = &w->magazine[mag];
	n = m->rounds_reloaded ? m->rounds_reloaded : m->rounds_loaded_max;
	if (m->flags & WEAP_MAG_WASTES_ROUNDS) {
		st->wasted += st->loaded[mag];
		st->loaded[mag] = 0;
	}
	space = (unsigned)m->rounds_loaded_max - st->loaded[mag];
	take = n < space ? n : space;
	if (take > st->reserve[mag]) take = st->reserve[mag];
	st->loaded[mag] = (uint16_t)(st->loaded[mag] + take);
	st->reserve[mag] = (uint16_t)(st->reserve[mag] - take);
	ev->flags |= WEAP_EV_RELOAD_DONE;
	for (t = 0; t < w->trigger_count; t++)
		if (w->trigger[t].magazine == mag) {
			float c = chamber_ticks(w, &w->trigger[t]);
			if (c > st->cooldown[t]) st->cooldown[t] = c;
		}
	if (n < m->rounds_loaded_max && can_reload(st, w, mag))      /* one shell at a time: the next cycle */
		start_reload(st, w, mag, 0, ev);
	else
		st->mode = WEAP_MODE_IDLE;
}

/* one shot of trigger t; returns 1 when a shot was fired */
static int try_shot(weap_state *st, const weap_weapon *w, unsigned t, float charge, int edge, weap_events *ev)
{
	const weap_trigger *tr = &w->trigger[t];
	unsigned need = tr->rounds_per_shot ? tr->rounds_per_shot : 1u, mag = tr->magazine, projectiles;
	float frac;

	if ((w->flags & WEAP_F_MAX_AGE_DEAD) && st->age >= 1.0f) {
		if (edge) ev->flags |= WEAP_EV_DRY_FIRE | WEAP_EV_BATTERY_DEAD;
		return 0;
	}
	if (has_ammo(w, tr)) {
		unsigned have = st->loaded[mag], floor_ = tr->min_rounds_loaded > need ? tr->min_rounds_loaded : need;
		if (have < floor_) {
			if (!((tr->flags & WEAP_T_PARTIAL_AMMO) && have > 0)) {
				if (edge) ev->flags |= WEAP_EV_DRY_FIRE;
				if (edge && can_reload(st, w, mag)) start_reload(st, w, mag, WEAP_EV_AUTO_RELOAD, ev);
				return 0;
			}
			need = have;
		}
		st->loaded[mag] = (uint16_t)(have - need);
		st->rounds_fired += need;
		ev->rounds_used = (uint16_t)(ev->rounds_used + need);
	}
	st->heat += tr->heat_per_round * (float)need;
	if (st->heat > 8.0f) st->heat = 8.0f;
	st->age += tr->age_per_round * (float)need;
	if (st->age > 1.0f) st->age = 1.0f;
	projectiles = tr->projectiles_per_shot ? tr->projectiles_per_shot : 1u;
	if (projectiles > 255u) projectiles = 255u;
	frac = st->error[t];
	ev->shot[t] = 1;
	ev->projectiles[t] = (uint8_t)projectiles;
	ev->charge[t] = charge;
	{
		float e = tr->error_min + (tr->error_max - tr->error_min) * clamp01(frac);
		ev->error[t] = e > tr->minimum_error ? e : tr->minimum_error;
	}
	st->shots++;
	ev->tracer[t] = (uint8_t)(tr->tracer_interval && st->shots % tr->tracer_interval == 0u);
	/* the rate ramps up with use, the error angle grows while firing */
	return 1;
}

void weapon_fire_step(weap_state *st, const weap_weapon *w, const weap_input *in, weap_events *ev)
{
	unsigned t;
	float dt = 1.0f / (float)WEAP_TICKS_PER_SEC;

	memset(ev, 0, sizeof *ev);
	for (t = 0; t < WEAP_MAX_TRIGGERS; t++) ev->charge[t] = 1.0f;
	if (st->mode == WEAP_MODE_STOWED) return;

	/* heat cools down continuously */
	if (st->heat > 0.0f) {
		float loss = w->heat_loss > 0.0f ? w->heat_loss : 0.5f;
		st->heat -= loss * dt;
		if (st->heat < 0.0f) st->heat = 0.0f;
	}

	/* timed modes */
	switch (st->mode) {
	case WEAP_MODE_READYING:
		if (st->timer) st->timer--;
		if (!st->timer) {
			st->mode = WEAP_MODE_IDLE;
			ev->flags |= WEAP_EV_READY;
		}
		break;
	case WEAP_MODE_RELOADING:
		if (st->timer) st->timer--;
		if (!st->timer) finish_reload(st, w, ev);
		break;
	case WEAP_MODE_MELEE:
		if (st->timer) st->timer--;
		if (st->melee_hit_at && st->timer <= st->melee_hit_at) {
			st->melee_hit_at = 0;
			ev->flags |= WEAP_EV_MELEE_HIT;
		}
		if (!st->timer) st->mode = WEAP_MODE_IDLE;
		break;
	case WEAP_MODE_PUTTING_AWAY:
		if (st->timer) st->timer--;
		if (!st->timer) {
			st->mode = WEAP_MODE_STOWED;
			ev->flags |= WEAP_EV_PUT_AWAY;
			st->prev_fire = 0;
			return;
		}
		break;
	case WEAP_MODE_OVERHEATED:
		if (st->heat <= w->heat_recovery) {
			st->mode = WEAP_MODE_IDLE;
			ev->flags |= WEAP_EV_COOLED;
		}
		break;
	default:
		break;
	}

	/* inputs that work in every live mode */
	if (in->switch_out && st->mode != WEAP_MODE_PUTTING_AWAY) {
		st->mode = WEAP_MODE_PUTTING_AWAY;
		st->timer = weap_put_away_ticks(w);
		st->zoom = 0;
		st->melee_hit_at = 0;
		for (t = 0; t < w->trigger_count; t++) st->charge[t] = 0.0f;
		st->charged_sent = 0;
		st->prev_fire = 0;
		return;
	}
	if (in->zoom && w->zoom_levels) {
		st->zoom = (uint8_t)((st->zoom + 1u) % ((unsigned)w->zoom_levels + 1u));
		ev->flags |= WEAP_EV_ZOOM;
	}

	/* a reload that loads one round or shell at a time yields to the trigger */
	if (st->mode == WEAP_MODE_RELOADING) {
		const weap_magazine *m = &w->magazine[st->reload_mag];
		if (m->rounds_reloaded && m->rounds_reloaded < m->rounds_loaded_max && st->loaded[st->reload_mag] > 0) {
			for (t = 0; t < w->trigger_count; t++)
				if (in->fire[t] && w->trigger[t].magazine == st->reload_mag) {
					st->mode = WEAP_MODE_IDLE;
					st->timer = 0;
					ev->flags |= WEAP_EV_RELOAD_CANCEL;
					break;
				}
		}
	}

	if (st->mode == WEAP_MODE_IDLE) {
		if (in->melee && !(w->flags & WEAP_F_NO_MELEE)) {
			st->mode = WEAP_MODE_MELEE;
			st->timer = weap_melee_ticks(w);
			st->melee_hit_at = (uint16_t)(st->timer / 2u);
			if (!st->melee_hit_at) st->melee_hit_at = 1;
			ev->flags |= WEAP_EV_MELEE_START;
			for (t = 0; t < w->trigger_count; t++) st->charge[t] = 0.0f;
			st->charged_sent = 0;
		} else if (in->reload) {
			unsigned mag;
			for (mag = 0; mag < w->magazine_count; mag++)
				if (can_reload(st, w, mag)) {
					start_reload(st, w, mag, 0, ev);
					break;
				}
		}
	}

	/* triggers */
	for (t = 0; t < w->trigger_count; t++) {
		const weap_trigger *tr = &w->trigger[t];
		int pressed = in->fire[t] != 0, was = (st->prev_fire >> t) & 1u, edge = pressed && !was;
		float span = tr->rps_max - tr->rps_min;

		/* rate and error angle ramp while the trigger is held, relax when it is released */
		if (pressed) {
			st->rate[t] = tr->accel_time > 0.0f ? st->rate[t] + span * dt / tr->accel_time : tr->rps_max;
			st->error[t] = tr->error_accel_time > 0.0f ? st->error[t] + dt / tr->error_accel_time : 1.0f;
		} else {
			st->rate[t] = tr->decel_time > 0.0f ? st->rate[t] - span * dt / tr->decel_time : tr->rps_min;
			st->error[t] = tr->error_decel_time > 0.0f ? st->error[t] - dt / tr->error_decel_time : 0.0f;
		}
		if (st->rate[t] > tr->rps_max) st->rate[t] = tr->rps_max;
		if (st->rate[t] < tr->rps_min) st->rate[t] = tr->rps_min;
		st->error[t] = clamp01(st->error[t]);
		if (st->cooldown[t] > 0.0f) st->cooldown[t] -= 1.0f;

		if (st->mode != WEAP_MODE_IDLE) {
			if (!pressed) { st->charge[t] = 0.0f; st->charged_sent &= (uint8_t)~(1u << t); }
			continue;
		}
		if ((w->flags & WEAP_F_MUTUALLY_EXCLUSIVE) && (st->prev_fire & ~(1u << t)) && !was && pressed) continue;

		if (pressed && (w->flags & WEAP_F_MAX_AGE_DEAD) && st->age >= 1.0f) {     /* dead battery: nothing starts */
			if (edge) ev->flags |= WEAP_EV_DRY_FIRE | WEAP_EV_BATTERY_DEAD;
			st->charge[t] = 0.0f;
			st->charged_sent &= (uint8_t)~(1u << t);
			continue;
		}
		if (tr->charging_time > 0.0f && pressed && st->charge[t] == 0.0f && !ammo_ok(st, w, tr)) {   /* nothing to charge */
			if (edge) {
				ev->flags |= WEAP_EV_DRY_FIRE;
				if (can_reload(st, w, tr->magazine)) start_reload(st, w, tr->magazine, WEAP_EV_AUTO_RELOAD, ev);
			}
			continue;
		}
		if (tr->charging_time > 0.0f) {
			float full = tr->charging_time * (float)WEAP_TICKS_PER_SEC;     /* charge counts whole ticks */
			if (pressed) {
				if (st->charge[t] == 0.0f) ev->flags |= WEAP_EV_CHARGING;
				st->charge[t] += 1.0f;
				if (st->charge[t] >= full && !((st->charged_sent >> t) & 1u)) {
					st->charged_sent |= (uint8_t)(1u << t);
					ev->flags |= WEAP_EV_CHARGED;
				}
				if (tr->charged_time > 0.0f && st->charge[t] >= full + tr->charged_time * (float)WEAP_TICKS_PER_SEC) {
					try_shot(st, w, t, 1.0f, 1, ev);                /* held too long: it goes off */
					st->charge[t] = 0.0f;
					st->charged_sent &= (uint8_t)~(1u << t);
				}
			} else if (was && st->charge[t] > 0.0f) {
				try_shot(st, w, t, clamp01(st->charge[t] / full), 1, ev);
				st->charge[t] = 0.0f;
				st->charged_sent &= (uint8_t)~(1u << t);
			}
		} else if (pressed) {
			float iv = weap_shot_interval(w, t, st->rate[t]);
			int once = (tr->flags & WEAP_T_NO_REPEAT) != 0 || iv <= 0.0f;
			if (once && was) {
				/* held: nothing until released */
			} else if (st->cooldown[t] <= 0.0f) {
				if (try_shot(st, w, t, 1.0f, edge, ev)) {
					st->cooldown[t] += iv > 0.0f ? iv : 1.0f;
					if (st->cooldown[t] < 0.0f) st->cooldown[t] = 0.0f;
				}
			}
		} else if (st->cooldown[t] < 0.0f) {
			st->cooldown[t] = 0.0f;
		}
		if (ev->shot[t] && has_ammo(w, tr) && st->loaded[tr->magazine] < (tr->rounds_per_shot ? tr->rounds_per_shot : 1u) &&
		    can_reload(st, w, tr->magazine) && st->mode == WEAP_MODE_IDLE)
			start_reload(st, w, tr->magazine, WEAP_EV_AUTO_RELOAD, ev);   /* the last round is gone */
	}

	if (st->mode != WEAP_MODE_OVERHEATED && w->overheated > 0.0f && st->heat >= w->overheated) set_overheated(st, w, ev);

	st->prev_fire = 0;
	for (t = 0; t < w->trigger_count; t++)
		if (in->fire[t]) st->prev_fire |= (uint8_t)(1u << t);
}
