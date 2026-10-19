/* DEVS section reader (docs/27-dev-section.md). Pure C11, no heap, no double. devdef_open validates every table once
 * (sizes, alignment, ranges, sort orders, string terminators, finite floats, cross indices), so the accessors can never
 * read out of bounds, whatever the bytes. Records are decoded with byte loads: no alignment assumptions. */
#include "world/devdef.h"
#include <string.h>

#define HEADER_SIZE 128u
#define DEVDEF_VERSION 1u
#define MAX_RECORDS (1u << 20)

#define DEF_REC 160u
#define DEVICE_REC 80u
#define GROUP_REC 48u
#define VOLUME_REC 96u
#define START_REC 32u
#define CFLAG_REC 64u
#define CAMERA_REC 64u
#define NAME_REC 48u
#define AMBIENT_REC 48u
#define DECAL_REC 32u
#define BSPSW_REC 8u

/* header: +0 "DEVS", +4 u16 version, +8 size, +0x0C twelve counts, +0x3C bsp count, +0x40 thirteen table offsets,
 * +0x74 dropped devices, +0x78 dangling names */
#define H_SIZE 0x08
#define H_COUNTS 0x0C
#define H_BSPS 0x3C
#define H_OFFSETS 0x40
#define H_DROPPED 0x74
#define H_DANGLING 0x78

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
static int finite_n(const uint8_t *p, unsigned n)
{
	unsigned k;
	for (k = 0; k < n; k++)
		if (!finite_at(p + 4u * k)) return 0;
	return 1;
}
static int terminated(const uint8_t *p, unsigned n) { return p[n - 1] == 0; }

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

/* name `a` of a name record vs. a C string / another record: strcmp on the NUL-terminated 32 bytes */
static int name_cmp(const uint8_t *a, const uint8_t *b) { return strcmp((const char *)a, (const char *)b); }

devdef_status devdef_open(devdef_section *s, const uint8_t *data, size_t size)
{
	static const uint32_t recs[13] = { DEF_REC, DEVICE_REC, GROUP_REC, VOLUME_REC, START_REC, CFLAG_REC, CAMERA_REC, NAME_REC, 2u,
	                                   AMBIENT_REC, DECAL_REC, 4u, BSPSW_REC };
	uint32_t c[12], off[13], nbsp, valid, i, k;
	uint64_t counts[13];

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return DEVDEF_ERR_TRUNCATED;
	if (memcmp(data, "DEVS", 4) != 0) return DEVDEF_ERR_MAGIC;
	if (rd16(data + 4) != DEVDEF_VERSION) return DEVDEF_ERR_VERSION;
	if (rd32(data + H_SIZE) > size) return DEVDEF_ERR_TRUNCATED;
	if (rd32(data + H_SIZE) != size) return DEVDEF_ERR_LAYOUT;
	for (i = 0; i < 12; i++) {
		c[i] = rd32(data + H_COUNTS + 4 * i);
		if (c[i] > MAX_RECORDS) return DEVDEF_ERR_LAYOUT;
	}
	nbsp = rd32(data + H_BSPS);
	if (nbsp > DEVDEF_MAX_BSPS) return DEVDEF_ERR_LAYOUT;
	if (c[0] > 0xFFFFu) return DEVDEF_ERR_LAYOUT;                    /* devices index their def with a u16 */
	for (i = 0; i < 13; i++) off[i] = rd32(data + H_OFFSETS + 4 * i);
	/* counts of the 13 tables: the name order table has one u16 per name */
	for (i = 0; i < 8; i++) counts[i] = c[i];
	counts[8] = c[7];
	for (i = 9; i < 13; i++) counts[i] = c[i - 1];
	for (i = 0; i < 13; i++)
		if (!table_ok(off[i], counts[i], recs[i], size)) return DEVDEF_ERR_LAYOUT;
	valid = nbsp >= 32u ? 0xFFFFFFFFu : (1u << nbsp) - 1u;

	{	/* defs */
		uint32_t prev = 0;
		for (i = 0; i < c[0]; i++) {
			const uint8_t *d = data + off[0] + (size_t)i * DEF_REC;
			uint32_t tag = rd32(d);
			if (i && tag <= prev) return DEVDEF_ERR_LAYOUT;       /* strictly ascending: binary search */
			prev = tag;
			if (d[12] > DEVDEF_LIGHT) return DEVDEF_ERR_LAYOUT;
			if (d[12] == DEVDEF_MACHINE && d[13] > DEVDEF_MACH_GEAR) return DEVDEF_ERR_LAYOUT;
			if (d[12] == DEVDEF_CONTROL && d[13] > DEVDEF_CTRL_CALL) return DEVDEF_ERR_LAYOUT;
			if (d[12] == DEVDEF_LIGHT && d[13] != 0) return DEVDEF_ERR_LAYOUT;
			if (!finite_at(d + 44) || (rd32(d + 44) & 0x80000000u)) return DEVDEF_ERR_LAYOUT;      /* radius >= 0 */
			if (!finite_at(d + 116)) return DEVDEF_ERR_LAYOUT;
			if (rd16(d + 100) >> DEVDEF_FX_COUNT) return DEVDEF_ERR_LAYOUT;
			for (k = 0; k < DEVDEF_FX_COUNT; k++)
				if ((rd16(d + 100) >> k & 1u) && rd32(d + 60 + 4 * k) == 0) return DEVDEF_ERR_LAYOUT;   /* mask bit without a tag */
			if (!terminated(d + 120, DEVDEF_NAME_LEN)) return DEVDEF_ERR_LAYOUT;
		}
	}
	{	/* devices */
		uint32_t prev = 0;
		for (i = 0; i < c[1]; i++) {
			const uint8_t *p = data + off[1] + (size_t)i * DEVICE_REC;
			int16_t pg = (int16_t)rd16(p + 32), qg = (int16_t)rd16(p + 34), ni = (int16_t)rd16(p + 44);
			uint32_t scen = rd32(p + 48);
			if (rd16(p) >= c[0]) return DEVDEF_ERR_LAYOUT;
			if (rd32(p + 4) & ~valid) return DEVDEF_ERR_LAYOUT;
			if (!finite_n(p + 8, 6) || !finite_n(p + 52, 6)) return DEVDEF_ERR_LAYOUT;
			if (pg < -1 || (pg >= 0 && (uint32_t)pg >= c[2])) return DEVDEF_ERR_LAYOUT;
			if (qg < -1 || (qg >= 0 && (uint32_t)qg >= c[2])) return DEVDEF_ERR_LAYOUT;
			if (ni < -1 || (ni >= 0 && (uint32_t)ni >= c[7])) return DEVDEF_ERR_LAYOUT;
			if (scen == DEVDEF_NONE || (i && scen <= prev)) return DEVDEF_ERR_LAYOUT;   /* strictly ascending: binary search */
			prev = scen;
		}
	}
	for (i = 0; i < c[2]; i++) {     /* groups */
		const uint8_t *g = data + off[2] + (size_t)i * GROUP_REC;
		float v = rdf(g + 32);
		if (!terminated(g, DEVDEF_NAME_LEN) || !finite_at(g + 32) || v < 0.0f || v > 1.0f) return DEVDEF_ERR_LAYOUT;
	}
	for (i = 0; i < c[3]; i++) {     /* volumes */
		const uint8_t *v = data + off[3] + (size_t)i * VOLUME_REC;
		if (!terminated(v, DEVDEF_NAME_LEN) || !finite_n(v + 32, 15)) return DEVDEF_ERR_LAYOUT;
		for (k = 0; k < 3; k++)
			if (rdf(v + 80 + 4 * k) < 0.0f) return DEVDEF_ERR_LAYOUT;                 /* extents >= 0 */
	}
	for (i = 0; i < c[4]; i++) {     /* starting locations */
		const uint8_t *p = data + off[4] + (size_t)i * START_REC;
		if (!finite_n(p, 4) || (rd32(p + 28) & ~valid)) return DEVDEF_ERR_LAYOUT;
	}
	for (i = 0; i < c[5]; i++) {     /* cutscene flags */
		const uint8_t *p = data + off[5] + (size_t)i * CFLAG_REC;
		if (!terminated(p, DEVDEF_NAME_LEN) || !finite_n(p + 32, 5) || (rd32(p + 52) & ~valid)) return DEVDEF_ERR_LAYOUT;
	}
	for (i = 0; i < c[6]; i++) {     /* camera points */
		const uint8_t *p = data + off[6] + (size_t)i * CAMERA_REC;
		if (!terminated(p, DEVDEF_NAME_LEN) || !finite_n(p + 32, 7) || (rd32(p + 60) & ~valid)) return DEVDEF_ERR_LAYOUT;
	}
	for (i = 0; i < c[7]; i++) {     /* names */
		const uint8_t *p = data + off[7] + (size_t)i * NAME_REC;
		uint8_t fl = p[34];
		if (!terminated(p, DEVDEF_NAME_LEN) || p[33] > 9u || fl > 7u) return DEVDEF_ERR_LAYOUT;
		if ((fl & 2u) && ((fl & 4u) || !(fl & 1u) || rd32(p + 44) >= c[1])) return DEVDEF_ERR_LAYOUT;
		if ((fl & 4u) && (!(fl & 1u) || rd32(p + 44) >= c[8])) return DEVDEF_ERR_LAYOUT;
		if (!(fl & 6u) && rd32(p + 44) != DEVDEF_NONE) return DEVDEF_ERR_LAYOUT;
	}
	{	/* name order: strictly increasing (name, index) keys over valid indices, hence a permutation */
		uint32_t prev = 0;
		for (i = 0; i < c[7]; i++) {
			uint32_t idx = rd16(data + off[8] + 2 * i);
			if (idx >= c[7]) return DEVDEF_ERR_LAYOUT;
			if (i) {
				int cmp = name_cmp(data + off[7] + (size_t)prev * NAME_REC, data + off[7] + (size_t)idx * NAME_REC);
				if (cmp > 0 || (cmp == 0 && idx <= prev)) return DEVDEF_ERR_LAYOUT;
			}
			prev = idx;
		}
	}
	for (i = 0; i < c[8]; i++) {     /* sound scenery */
		const uint8_t *p = data + off[9] + (size_t)i * AMBIENT_REC;
		int16_t ni = (int16_t)rd16(p + 32);
		if (!finite_n(p + 8, 6) || (rd32(p + 36) & ~valid)) return DEVDEF_ERR_LAYOUT;
		if (ni < -1 || (ni >= 0 && (uint32_t)ni >= c[7])) return DEVDEF_ERR_LAYOUT;
	}
	for (i = 0; i < c[9]; i++) {     /* decals */
		const uint8_t *p = data + off[10] + (size_t)i * DECAL_REC;
		if (rd16(p) >= c[10] || !finite_n(p + 4, 5) || (rd32(p + 24) & ~valid)) return DEVDEF_ERR_LAYOUT;
	}
	for (i = 0; i < c[11]; i++) {    /* BSP switches */
		const uint8_t *p = data + off[12] + (size_t)i * BSPSW_REC;
		if (rd16(p) >= c[3]) return DEVDEF_ERR_LAYOUT;
		if (nbsp && (rd16(p + 2) >= nbsp || rd16(p + 4) >= nbsp)) return DEVDEF_ERR_LAYOUT;
	}
	s->data = data;
	s->size = size;
	s->def_count = c[0];
	s->device_count = c[1];
	s->group_count = c[2];
	s->volume_count = c[3];
	s->start_count = c[4];
	s->flag_count = c[5];
	s->camera_count = c[6];
	s->name_count = c[7];
	s->ambient_count = c[8];
	s->decal_count = c[9];
	s->decal_type_count = c[10];
	s->bspsw_count = c[11];
	s->bsp_count = nbsp;
	s->dropped_devices = rd32(data + H_DROPPED);
	s->dangling_names = rd32(data + H_DANGLING);
	s->defs = data + off[0];
	s->devices = data + off[1];
	s->groups = data + off[2];
	s->volumes = data + off[3];
	s->starts = data + off[4];
	s->flags = data + off[5];
	s->cameras = data + off[6];
	s->names = data + off[7];
	s->name_order = data + off[8];
	s->ambients = data + off[9];
	s->decals = data + off[10];
	s->decal_types = data + off[11];
	s->bspsw = data + off[12];
	return DEVDEF_OK;
}

static void rd_name(char *dst, const uint8_t *p)
{
	memcpy(dst, p, DEVDEF_NAME_LEN);
	dst[DEVDEF_NAME_LEN - 1] = 0;
}

static void rd_vec(float *dst, const uint8_t *p, unsigned n)
{
	unsigned k;
	for (k = 0; k < n; k++) dst[k] = rdf(p + 4u * k);
}

devdef_def devdef_get_def(const devdef_section *s, uint32_t index)
{
	devdef_def d;
	unsigned k;
	memset(&d, 0, sizeof d);
	d.pos_anim = d.power_anim = DEVDEF_NO_ANIM;
	d.kind = DEVDEF_LIGHT;
	if (index < s->def_count) {
		const uint8_t *p = s->defs + (size_t)index * DEF_REC;
		d.tag_id = rd32(p);
		d.model_tag = rd32(p + 4);
		d.anim_tag = rd32(p + 8);
		d.kind = p[12];
		d.subtype = p[13];
		d.dev_flags = rd16(p + 14);
		d.kind_flags = rd16(p + 16);
		d.triggers = rd16(p + 18);
		d.power_ticks = rd32(p + 20);
		d.power_accel_ticks = rd32(p + 24);
		d.position_ticks = rd32(p + 28);
		d.position_accel_ticks = rd32(p + 32);
		d.depowered_ticks = rd32(p + 36);
		d.depowered_accel_ticks = rd32(p + 40);
		d.auto_radius = rdf(p + 44);
		d.delay_ticks = rd32(p + 48);
		d.open_ticks = rd32(p + 52);
		for (k = 0; k < 4; k++) d.fn_in[k] = p[56 + k];
		for (k = 0; k < DEVDEF_FX_COUNT; k++) d.fx[k] = rd32(p + 60 + 4 * k);
		d.fx_snd_mask = rd16(p + 100);
		d.pos_anim = rd16(p + 104);
		d.power_anim = rd16(p + 106);
		d.pos_frames = rd16(p + 108);
		d.power_frames = rd16(p + 110);
		d.collision = p[112];
		d.elevator_node = p[113];
		d.call_value = rdf(p + 116);
		rd_name(d.name, p + 120);
	}
	return d;
}

int devdef_find_def(const devdef_section *s, uint32_t tag_id)
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

devdef_device devdef_get_device(const devdef_section *s, uint32_t index)
{
	devdef_device r;
	memset(&r, 0, sizeof r);
	r.power_group = r.position_group = r.name_index = -1;
	r.scen_index = DEVDEF_NONE;
	if (index < s->device_count) {
		const uint8_t *p = s->devices + (size_t)index * DEVICE_REC;
		r.def = rd16(p);
		r.flags = rd16(p + 2);
		r.bsp_mask = rd32(p + 4);
		rd_vec(r.pos, p + 8, 3);
		rd_vec(r.rot, p + 20, 3);
		r.power_group = (int16_t)rd16(p + 32);
		r.position_group = (int16_t)rd16(p + 34);
		r.device_flags = rd32(p + 36);
		r.kind_flags = rd32(p + 40);
		r.name_index = (int16_t)rd16(p + 44);
		r.extra = rd16(p + 46);
		r.scen_index = rd32(p + 48);
		rd_vec(r.light_color, p + 52, 3);
		r.light_intensity = rdf(p + 64);
		r.light_falloff = rdf(p + 68);
		r.light_cutoff = rdf(p + 72);
	}
	return r;
}

int devdef_find_device_by_scen(const devdef_section *s, uint32_t scen_index)
{
	uint32_t lo = 0, hi = s->device_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, v = rd32(s->devices + (size_t)mid * DEVICE_REC + 48);
		if (v == scen_index) return (int)mid;
		if (v < scen_index) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

devdef_group devdef_get_group(const devdef_section *s, uint32_t index)
{
	devdef_group g;
	memset(&g, 0, sizeof g);
	if (index < s->group_count) {
		const uint8_t *p = s->groups + (size_t)index * GROUP_REC;
		rd_name(g.name, p);
		g.initial = rdf(p + 32);
		g.flags = rd32(p + 36);
	}
	return g;
}

devdef_volume devdef_get_volume(const devdef_section *s, uint32_t index)
{
	devdef_volume v;
	memset(&v, 0, sizeof v);
	v.forward[0] = v.up[2] = 1.0f;       /* an empty box at the origin */
	v.left[1] = 1.0f;
	if (index < s->volume_count) {
		const uint8_t *p = s->volumes + (size_t)index * VOLUME_REC;
		rd_name(v.name, p);
		rd_vec(v.forward, p + 32, 3);
		rd_vec(v.left, p + 44, 3);
		rd_vec(v.up, p + 56, 3);
		rd_vec(v.position, p + 68, 3);
		rd_vec(v.extents, p + 80, 3);
	}
	return v;
}

devdef_start devdef_get_start(const devdef_section *s, uint32_t index)
{
	devdef_start r;
	unsigned k;
	memset(&r, 0, sizeof r);
	if (index < s->start_count) {
		const uint8_t *p = s->starts + (size_t)index * START_REC;
		rd_vec(r.pos, p, 3);
		r.facing = rdf(p + 12);
		r.team = (int16_t)rd16(p + 16);
		r.bsp = (int16_t)rd16(p + 18);
		for (k = 0; k < 4; k++) r.game_type[k] = (int16_t)rd16(p + 20 + 2 * k);
		r.bsp_mask = rd32(p + 28);
	}
	return r;
}

devdef_cflag devdef_get_cflag(const devdef_section *s, uint32_t index)
{
	devdef_cflag r;
	memset(&r, 0, sizeof r);
	if (index < s->flag_count) {
		const uint8_t *p = s->flags + (size_t)index * CFLAG_REC;
		rd_name(r.name, p);
		rd_vec(r.pos, p + 32, 3);
		r.yaw = rdf(p + 44);
		r.pitch = rdf(p + 48);
		r.bsp_mask = rd32(p + 52);
	}
	return r;
}

devdef_camera devdef_get_camera(const devdef_section *s, uint32_t index)
{
	devdef_camera r;
	memset(&r, 0, sizeof r);
	if (index < s->camera_count) {
		const uint8_t *p = s->cameras + (size_t)index * CAMERA_REC;
		rd_name(r.name, p);
		rd_vec(r.pos, p + 32, 3);
		rd_vec(r.ori, p + 44, 3);
		r.fov = rdf(p + 56);
		r.bsp_mask = rd32(p + 60);
	}
	return r;
}

devdef_name devdef_get_name(const devdef_section *s, uint32_t index)
{
	devdef_name r;
	memset(&r, 0, sizeof r);
	r.scen_type = 9;
	r.scen_index = r.aux_index = DEVDEF_NONE;
	if (index < s->name_count) {
		const uint8_t *p = s->names + (size_t)index * NAME_REC;
		rd_name(r.name, p);
		r.obj_type = p[32];
		r.scen_type = p[33];
		r.flags = p[34];
		r.table_index = rd16(p + 36);
		r.scen_index = rd32(p + 40);
		r.aux_index = rd32(p + 44);
	}
	return r;
}

devdef_ambient devdef_get_ambient(const devdef_section *s, uint32_t index)
{
	devdef_ambient r;
	memset(&r, 0, sizeof r);
	r.name_index = -1;
	if (index < s->ambient_count) {
		const uint8_t *p = s->ambients + (size_t)index * AMBIENT_REC;
		r.ssce_tag = rd32(p);
		r.sound_tag = rd32(p + 4);
		rd_vec(r.pos, p + 8, 3);
		rd_vec(r.rot, p + 20, 3);
		r.name_index = (int16_t)rd16(p + 32);
		r.flags = rd16(p + 34);
		r.bsp_mask = rd32(p + 36);
		r.raw[0] = rd32(p + 40);
		r.raw[1] = rd32(p + 44);
	}
	return r;
}

devdef_decal devdef_get_decal(const devdef_section *s, uint32_t index)
{
	devdef_decal r;
	memset(&r, 0, sizeof r);
	if (index < s->decal_count) {
		const uint8_t *p = s->decals + (size_t)index * DECAL_REC;
		r.type = rd16(p);
		r.yaw_raw = (int8_t)p[2];
		r.pitch_raw = (int8_t)p[3];
		rd_vec(r.pos, p + 4, 3);
		r.yaw = rdf(p + 16);
		r.pitch = rdf(p + 20);
		r.bsp_mask = rd32(p + 24);
	}
	return r;
}

uint32_t devdef_decal_type(const devdef_section *s, uint32_t type)
{
	return type < s->decal_type_count ? rd32(s->decal_types + 4u * type) : 0;
}

devdef_bspsw devdef_get_bspsw(const devdef_section *s, uint32_t index)
{
	devdef_bspsw r;
	memset(&r, 0, sizeof r);
	if (index < s->bspsw_count) {
		const uint8_t *p = s->bspsw + (size_t)index * BSPSW_REC;
		r.volume = rd16(p);
		r.source = rd16(p + 2);
		r.dest = rd16(p + 4);
		r.unknown = rd16(p + 6);
	}
	return r;
}

/* Linear search over fixed 32-byte name fields at `stride` from `base`; the C string is compared exactly. */
static int find_linear(const uint8_t *base, uint32_t count, uint32_t stride, const char *name)
{
	uint32_t i;
	if (!name || strlen(name) >= DEVDEF_NAME_LEN) return -1;
	for (i = 0; i < count; i++)
		if (strcmp((const char *)(base + (size_t)i * stride), name) == 0) return (int)i;
	return -1;
}

int devdef_find_name(const devdef_section *s, const char *name)
{
	uint32_t lo = 0, hi = s->name_count;
	if (!name || strlen(name) >= DEVDEF_NAME_LEN) return -1;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, idx = rd16(s->name_order + 2u * mid);
		int cmp = strcmp((const char *)(s->names + (size_t)idx * NAME_REC), name);
		if (cmp == 0) return (int)idx;
		if (cmp < 0) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

int devdef_find_group(const devdef_section *s, const char *name) { return find_linear(s->groups, s->group_count, GROUP_REC, name); }
int devdef_find_volume(const devdef_section *s, const char *name) { return find_linear(s->volumes, s->volume_count, VOLUME_REC, name); }
int devdef_find_cflag(const devdef_section *s, const char *name) { return find_linear(s->flags, s->flag_count, CFLAG_REC, name); }
int devdef_find_camera(const devdef_section *s, const char *name) { return find_linear(s->cameras, s->camera_count, CAMERA_REC, name); }

int devdef_name_to_device(const devdef_section *s, uint32_t name_index)
{
	devdef_name n = devdef_get_name(s, name_index);
	return (n.flags & DEVDEF_NF_DEVICE) ? (int)n.aux_index : -1;
}

int devdef_name_to_scen(const devdef_section *s, uint32_t name_index)
{
	devdef_name n = devdef_get_name(s, name_index);
	return n.scen_index != DEVDEF_NONE ? (int)n.scen_index : -1;
}

int devdef_find_bspsw_by_volume(const devdef_section *s, uint32_t volume)
{
	uint32_t i;
	for (i = 0; i < s->bspsw_count; i++)
		if (rd16(s->bspsw + (size_t)i * BSPSW_REC) == volume) return (int)i;
	return -1;
}
