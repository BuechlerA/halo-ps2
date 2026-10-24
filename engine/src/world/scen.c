/* SCEN section reader (docs/13-scen-section.md). Pure C11, no heap, no double. scen_open validates every table
 * once (sizes, alignment, ranges, sort order, indices, string terminators, finite floats), so the accessors
 * can never read out of bounds, whatever the bytes. Records are decoded with byte loads: no alignment
 * assumptions on the section buffer. */
#include "world/scen.h"
#include <string.h>

#define HEADER_SIZE 128u
#define DEF_REC 64u
#define PLACEMENT_REC 64u
#define LIGHT_REC 16u
#define BSPTAB_REC 8u
#define PLAYER_REC 80u
#define SCEN_VERSION 1u
#define MAX_PLACEMENTS (1u << 20)
#define MAX_LIGHTS (1u << 22)

/* header: +0 "SCEN", +4 u16 version, +8 size, +0x0C defs, +0x10 placements, +0x14 bsps, +0x18 defs offset,
 * +0x1C placements offset, +0x20 lights offset, +0x24 tables offset, +0x28 player offset, +0x2C light count */
#define H_SIZE 0x08
#define H_DEFS 0x0C
#define H_PLACEMENTS 0x10
#define H_BSPS 0x14
#define H_O_DEFS 0x18
#define H_O_PLACEMENTS 0x1C
#define H_O_LIGHTS 0x20
#define H_O_TABLES 0x24
#define H_O_PLAYER 0x28
#define H_LIGHTS 0x2C

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

/* a table [off, off + count * rec) must be 16-aligned, past the header and inside the section */
static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

scen_status scen_open(scen_section *s, const uint8_t *data, size_t size)
{
	uint32_t nd, npl, nb, nl, o_def, o_pl, o_li, o_tab, o_pla, valid, i;
	uint32_t prev_tag = 0;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return SCEN_ERR_TRUNCATED;
	if (memcmp(data, "SCEN", 4) != 0) return SCEN_ERR_MAGIC;
	if (rd16(data + 4) != SCEN_VERSION) return SCEN_ERR_VERSION;
	if (rd32(data + H_SIZE) > size) return SCEN_ERR_TRUNCATED;
	if (rd32(data + H_SIZE) != size) return SCEN_ERR_LAYOUT;
	nd = rd32(data + H_DEFS);
	npl = rd32(data + H_PLACEMENTS);
	nb = rd32(data + H_BSPS);
	nl = rd32(data + H_LIGHTS);
	o_def = rd32(data + H_O_DEFS);
	o_pl = rd32(data + H_O_PLACEMENTS);
	o_li = rd32(data + H_O_LIGHTS);
	o_tab = rd32(data + H_O_TABLES);
	o_pla = rd32(data + H_O_PLAYER);
	if (nd > 0xFFFFu || npl > MAX_PLACEMENTS || nb > SCEN_MAX_BSPS || nl > MAX_LIGHTS) return SCEN_ERR_LAYOUT;
	if (!table_ok(o_def, nd, DEF_REC, size) || !table_ok(o_pl, npl, PLACEMENT_REC, size) ||
	    !table_ok(o_li, nl, LIGHT_REC, size) || !table_ok(o_tab, nb, BSPTAB_REC, size) ||
	    !table_ok(o_pla, 1, PLAYER_REC, size))
		return SCEN_ERR_LAYOUT;
	valid = nb >= 32u ? 0xFFFFFFFFu : (1u << nb) - 1u;

	for (i = 0; i < nd; i++) {
		const uint8_t *d = data + o_def + (size_t)i * DEF_REC;
		uint32_t tag = rd32(d);
		if (i && tag <= prev_tag) return SCEN_ERR_LAYOUT;          /* strictly ascending: binary search */
		prev_tag = tag;
		if (d[12] >= (uint8_t)(SCEN_OTHER + 1)) return SCEN_ERR_LAYOUT;
		if (!terminated(d + 16, SCEN_NAME_LEN)) return SCEN_ERR_LAYOUT;
		if (!finite_at(d + 48) || (rd32(d + 48) & 0x80000000u)) return SCEN_ERR_LAYOUT;   /* radius >= 0 */
	}
	for (i = 0; i < npl; i++) {
		const uint8_t *p = data + o_pl + (size_t)i * PLACEMENT_REC;
		unsigned k;
		if (rd16(p) >= nd) return SCEN_ERR_LAYOUT;
		if (rd32(p + 4) & ~valid) return SCEN_ERR_LAYOUT;
		for (k = 0; k < 6; k++)
			if (!finite_at(p + 8 + 4 * k)) return SCEN_ERR_LAYOUT;
		if (!terminated(p + 32, 16)) return SCEN_ERR_LAYOUT;
	}
	for (i = 0; i < nb; i++) {
		const uint8_t *t = data + o_tab + (size_t)i * BSPTAB_REC;
		if ((uint64_t)rd32(t) + rd32(t + 4) > nl) return SCEN_ERR_LAYOUT;
	}
	{
		const uint8_t *pl = data + o_pla;
		unsigned k;
		if (rd32(pl + 8) > nd || rd32(pl + 20) > nd) return SCEN_ERR_LAYOUT;   /* weapon_def, biped_def */
		for (k = 0; k < 3; k++)
			if (!finite_at(pl + 28 + 4 * k)) return SCEN_ERR_LAYOUT;
		if (!terminated(pl + 40, SCEN_NAME_LEN)) return SCEN_ERR_LAYOUT;
	}
	s->data = data;
	s->size = size;
	s->def_count = nd;
	s->placement_count = npl;
	s->bsp_count = nb;
	s->defs = data + o_def;
	s->placements = data + o_pl;
	s->lights = data + o_li;
	s->tables = data + o_tab;
	return SCEN_OK;
}

scen_def scen_get_def(const scen_section *s, uint32_t index)
{
	scen_def d;
	memset(&d, 0, sizeof d);
	d.idle_anim = SCEN_NO_ANIM;
	d.type = SCEN_OTHER;
	if (index < s->def_count) {
		const uint8_t *p = s->defs + (size_t)index * DEF_REC;
		d.tag_id = rd32(p);
		d.model_tag = rd32(p + 4);
		d.anim_tag = rd32(p + 8);
		d.type = p[12];
		d.idle_anim = rd16(p + 14);
		memcpy(d.name, p + 16, SCEN_NAME_LEN);
		d.name[SCEN_NAME_LEN - 1] = 0;
		d.radius = rdf(p + 48);
	}
	return d;
}

int scen_find_def(const scen_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->def_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->defs + (size_t)mid * DEF_REC);
		if (t == tag_id) return (int)mid;
		if (t < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

scen_placement scen_get_placement(const scen_section *s, uint32_t index)
{
	scen_placement r;
	unsigned k;
	memset(&r, 0, sizeof r);
	r.name_index = -1;
	if (index < s->placement_count) {
		const uint8_t *p = s->placements + (size_t)index * PLACEMENT_REC;
		r.def = rd16(p);
		r.flags = rd16(p + 2);
		r.bsp_mask = rd32(p + 4);
		for (k = 0; k < 3; k++) {
			r.pos[k] = rdf(p + 8 + 4 * k);
			r.rot[k] = rdf(p + 20 + 4 * k);
		}
		memcpy(r.variant, p + 32, sizeof r.variant);
		r.variant[sizeof r.variant - 1] = 0;
		r.name_index = (int16_t)rd16(p + 48);
		r.permutation = (uint16_t)rd16(p + 50);
	}
	return r;
}

uint32_t scen_placements_in_bsp(const scen_section *s, uint32_t bsp)
{
	uint32_t i, n = 0;
	if (bsp >= SCEN_MAX_BSPS) return 0;
	for (i = 0; i < s->placement_count; i++)
		if (rd32(s->placements + (size_t)i * PLACEMENT_REC + 4) >> bsp & 1u) n++;
	return n;
}

void scen_cluster_light(const scen_section *s, uint32_t bsp, uint32_t cluster, scen_light *out)
{
	static const uint8_t neutral = 64;       /* 0.5 ambient + 0.5 sun from above (docs/13) */
	unsigned k;
	for (k = 0; k < 3; k++) {
		out->ambient[k] = neutral;
		out->sun[k] = neutral;
		out->sun_dir[k] = k == 2 ? 1.0f : 0.0f;
	}
	out->pad0 = out->pad1 = 0;
	if (bsp < s->bsp_count) {
		const uint8_t *t = s->tables + (size_t)bsp * BSPTAB_REC;
		uint32_t first = rd32(t), count = rd32(t + 4);
		if (cluster < count) {
			const uint8_t *l = s->lights + ((size_t)first + cluster) * LIGHT_REC;
			for (k = 0; k < 3; k++) {
				out->ambient[k] = l[k];
				out->sun[k] = l[4 + k];
				out->sun_dir[k] = (float)(int16_t)rd16(l + 8 + 2 * k) / 32767.0f;
			}
		}
	}
}

scen_player scen_get_player(const scen_section *s)
{
	scen_player r;
	const uint8_t *p;
	unsigned k;
	memset(&r, 0, sizeof r);
	r.arms_idle_anim = r.weapon_idle_anim = SCEN_NO_ANIM;
	if (!s->data) return r;
	p = s->data + rd32(s->data + H_O_PLAYER);
	r.fp_arms_model_tag = rd32(p);
	r.fp_arms_anim_tag = rd32(p + 4);
	r.weapon_def = rd32(p + 8);
	r.weapon_fp_model_tag = rd32(p + 12);
	r.weapon_fp_anim_tag = rd32(p + 16);
	r.biped_def = rd32(p + 20);
	r.arms_idle_anim = rd16(p + 24);
	r.weapon_idle_anim = rd16(p + 26);
	for (k = 0; k < 3; k++) r.fp_offset[k] = rdf(p + 28 + 4 * k);
	memcpy(r.weapon_hand_marker, p + 40, SCEN_NAME_LEN);
	r.weapon_hand_marker[SCEN_NAME_LEN - 1] = 0;
	r.weapon_loaded = rd16(p + 72);
	r.weapon_total = rd16(p + 74);
	for (k = 0; k < 2; k++) { r.start_grenades[k] = p[76 + k]; r.equip_grenades[k] = p[78 + k]; }
	return r;
}
