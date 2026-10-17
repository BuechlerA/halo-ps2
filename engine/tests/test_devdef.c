/* Host test: open the DEVS section of a pack written by tools/tests/test_devconv.py and check it against the synthetic
 * world that test builds (7 device tags, 9 placed devices, 3 groups, 4 volumes, 2 starts, 2 flags, 2 cameras, 6 object
 * names, 2 sound scenery, 2 decals, 2 BSP switches, 2 BSPs). Then truncate at every length, corrupt every table and
 * field, and flip every byte: a corrupt section is either rejected or safe to walk completely (ASan/UBSan).
 * Further arguments are real packs (assets/hp2/<map>.dev.hp2): opened, walked and cross-checked. */
#include "stream/hp2.h"
#include "world/devdef.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define TAGID(i) (((0xE174u + (i)) << 16) | (i))     /* tag ids of tools/tests/test_devconv.py's TAGS, by position */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static volatile uint32_t sink;

static uint32_t fbits(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

/* Touch everything an opened section hands out and check the invariants devdef_open promises. */
static void walk(const devdef_section *s)
{
	uint32_t acc = 0, i, k;
	for (i = 0; i < s->def_count; i++) {
		devdef_def d = devdef_get_def(s, i);
		if (devdef_find_def(s, d.tag_id) != (int)i) abort();
		if (d.kind > DEVDEF_LIGHT || strlen(d.name) >= DEVDEF_NAME_LEN || d.auto_radius < 0.0f || d.auto_radius != d.auto_radius) abort();
		for (k = 0; k < DEVDEF_FX_COUNT; k++)
			if ((d.fx_snd_mask >> k & 1u) && !d.fx[k]) abort();
		acc += d.tag_id + d.position_ticks + d.open_ticks + d.pos_anim + d.pos_frames + fbits(d.call_value);
	}
	for (i = 0; i < s->device_count; i++) {
		devdef_device v = devdef_get_device(s, i);
		if (v.def >= s->def_count || v.power_group >= (int)s->group_count || v.position_group >= (int)s->group_count) abort();
		if (v.power_group < -1 || v.position_group < -1 || v.name_index >= (int)s->name_count || v.name_index < -1) abort();
		if (devdef_find_device_by_scen(s, v.scen_index) != (int)i) abort();
		if (s->bsp_count < 32 && (v.bsp_mask >> s->bsp_count)) abort();
		acc += v.flags + v.bsp_mask + v.extra + fbits(v.pos[0]);
	}
	for (i = 0; i < s->group_count; i++) {
		devdef_group g = devdef_get_group(s, i);
		if (g.initial < 0.0f || g.initial > 1.0f || devdef_find_group(s, g.name) < 0) abort();
		acc += g.flags;
	}
	for (i = 0; i < s->volume_count; i++) {
		devdef_volume v = devdef_get_volume(s, i);
		if (v.extents[0] < 0.0f || v.extents[1] < 0.0f || v.extents[2] < 0.0f || devdef_find_volume(s, v.name) < 0) abort();
		acc += fbits(v.position[0]);
	}
	for (i = 0; i < s->start_count; i++) acc += devdef_get_start(s, i).bsp_mask;
	for (i = 0; i < s->flag_count; i++) {
		devdef_cflag f = devdef_get_cflag(s, i);
		if (devdef_find_cflag(s, f.name) < 0) abort();
		acc += f.bsp_mask;
	}
	for (i = 0; i < s->camera_count; i++) {
		devdef_camera c = devdef_get_camera(s, i);
		if (devdef_find_camera(s, c.name) < 0) abort();
		acc += c.bsp_mask;
	}
	for (i = 0; i < s->name_count; i++) {
		devdef_name n = devdef_get_name(s, i);
		int found = devdef_find_name(s, n.name);
		if (found < 0 || strcmp(devdef_get_name(s, (uint32_t)found).name, n.name) != 0) abort();
		if (n.flags & DEVDEF_NF_DEVICE) {
			if (n.aux_index >= s->device_count || devdef_name_to_device(s, i) != (int)n.aux_index) abort();
		}
		acc += n.scen_index + n.aux_index + n.table_index;
	}
	for (i = 0; i < s->ambient_count; i++) {
		devdef_ambient a = devdef_get_ambient(s, i);
		if (a.name_index >= (int)s->name_count || a.name_index < -1) abort();
		acc += a.sound_tag + a.bsp_mask;
	}
	for (i = 0; i < s->decal_count; i++) {
		devdef_decal d = devdef_get_decal(s, i);
		if (d.type >= s->decal_type_count) abort();
		acc += devdef_decal_type(s, d.type) + d.bsp_mask;
	}
	for (i = 0; i < s->bspsw_count; i++) {
		devdef_bspsw b = devdef_get_bspsw(s, i);
		if (b.volume >= s->volume_count || devdef_find_bspsw_by_volume(s, b.volume) < 0) abort();
		acc += b.source + b.dest;
	}
	sink = acc;
}

static int load_file(const char *path, uint8_t **img, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	*img = malloc(*n);
	if (!*img || fread(*img, 1, *n, f) != *n) return 1;
	fclose(f);
	return 0;
}

static int load_section(const uint8_t *img, size_t n, uint8_t **raw, size_t *rawn)
{
	hp2_pack pk;
	const hp2_section *sec;
	CHECK(hp2_open(&pk, img, n) == HP2_OK);
	sec = hp2_find(&pk, "DEVS");
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(&pk, sec, *raw, sec->raw_size) == HP2_OK);
	*rawn = sec->raw_size;
	return 0;
}

/* independent point-in-box: solve p - position = s0 f + s1 l + s2 u by Cramer's rule */
static double det3(const float *a, const float *b, const float *c)
{
	return (double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) - (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0]) +
	       (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0]);
}

/* returns -1 when the box is degenerate; 1 / 0 inside / outside; `margin` is the smallest distance to a face along an axis */
static int box_inside(const devdef_volume *v, const float p[3], double *margin)
{
	float d[3] = { p[0] - v->position[0], p[1] - v->position[1], p[2] - v->position[2] };
	double den = det3(v->forward, v->left, v->up), s[3];
	int k, in = 1;
	if (fabs(den) < 1e-6) return -1;
	s[0] = det3(d, v->left, v->up) / den;
	s[1] = det3(v->forward, d, v->up) / den;
	s[2] = det3(v->forward, v->left, d) / den;
	*margin = 1e30;
	for (k = 0; k < 3; k++) {
		double m = fmin(fabs(s[k]), fabs(s[k] - v->extents[k]));
		if (m < *margin) *margin = m;
		if (s[k] < 0.0 || s[k] > v->extents[k]) in = 0;
	}
	return in;
}

/* the geometry helper the volume tests of test_devices.c use as well */
static int check_real(const char *path)
{
	uint8_t *img, *raw;
	size_t n, rawn, i;
	devdef_section s;
	unsigned long agree = 0, total = 0, inside = 0, fx_tags = 0;
	uint32_t rng = 12345;
	CHECK(!load_file(path, &img, &n));
	CHECK(!load_section(img, n, &raw, &rawn));
	CHECK(devdef_open(&s, raw, rawn) == DEVDEF_OK);
	walk(&s);
	/* volumes: random points against the independent solver (points within 1e-3 of a face are not judged) */
	for (i = 0; i < s.volume_count; i++) {
		devdef_volume v = devdef_get_volume(&s, (uint32_t)i);
		int t;
		for (t = 0; t < 40; t++) {
			float p[3];
			double margin;
			int ref, k;
			if (t & 1) {                                                  /* odd: a point inside by construction */
				float sc[3];
				for (k = 0; k < 3; k++) {
					rng = rng * 1664525u + 1013904223u;
					sc[k] = (float)(rng >> 8) / 16777216.0f * v.extents[k];
				}
				for (k = 0; k < 3; k++) p[k] = v.position[k] + sc[0] * v.forward[k] + sc[1] * v.left[k] + sc[2] * v.up[k];
			} else {                                                       /* even: anywhere around the box */
				for (k = 0; k < 3; k++) {
					rng = rng * 1664525u + 1013904223u;
					p[k] = v.position[k] + ((float)(rng >> 8) / 16777216.0f - 0.5f) * (v.extents[0] + v.extents[1] + v.extents[2] + 2.0f);
				}
			}
			ref = box_inside(&v, p, &margin);
			CHECK(ref >= 0);
			if (margin < 1e-3) continue;
			total++;
			inside += (unsigned long)ref;
			agree++;
			/* local coordinates via the dot products of the converter's frame: same answer */
			{
				float dx = p[0] - v.position[0], dy = p[1] - v.position[1], dz = p[2] - v.position[2];
				int in2 = 1;
				const float *ax[3] = { v.forward, v.left, v.up };
				for (k = 0; k < 3; k++) {
					float sk = dx * ax[k][0] + dy * ax[k][1] + dz * ax[k][2];
					if (!(sk >= 0.0f && sk <= v.extents[k])) in2 = 0;
				}
				CHECK(in2 == ref);
			}
		}
	}
	CHECK(agree == total);
	/* devices: tags consistent, names resolve to the device carrying them */
	for (i = 0; i < s.device_count; i++) {
		devdef_device v = devdef_get_device(&s, (uint32_t)i);
		devdef_def d = devdef_get_def(&s, v.def);
		if (d.kind == DEVDEF_MACHINE && d.auto_radius > 0.0f) CHECK(d.subtype == DEVDEF_MACH_DOOR);
		if (v.name_index >= 0) {
			devdef_name nm = devdef_get_name(&s, (uint32_t)v.name_index);
			CHECK((nm.flags & DEVDEF_NF_DEVICE) && nm.aux_index == i && nm.scen_index == v.scen_index);
		}
		for (int k = 0; k < DEVDEF_FX_COUNT; k++) fx_tags += d.fx[k] != 0;
	}
	/* every start lies in a BSP of the map (the converter located it) */
	for (i = 0; i < s.start_count; i++) CHECK(devdef_get_start(&s, (uint32_t)i).bsp_mask != 0 || s.bsp_count == 0);
	printf("  %s: %u defs, %u devices, %u groups, %u volumes (%lu / %lu points inside), %u names, %u decals, %lu effect refs\n", path,
	       s.def_count, s.device_count, s.group_count, s.volume_count, inside, total, s.name_count, s.decal_count, fx_tags);
	free(raw);
	free(img);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t *img, *raw, *copy;
	size_t n, rawn;
	devdef_section s, bad;
	CHECK(argc >= 2);
	CHECK(!load_file(argv[1], &img, &n));
	CHECK(!load_section(img, n, &raw, &rawn));
	CHECK(devdef_open(&s, raw, rawn) == DEVDEF_OK);
	CHECK(s.def_count == 7 && s.device_count == 9 && s.group_count == 3 && s.volume_count == 4 && s.start_count == 2);
	CHECK(s.flag_count == 2 && s.camera_count == 2 && s.name_count == 6 && s.ambient_count == 2 && s.decal_count == 2);
	CHECK(s.decal_type_count == 2 && s.bspsw_count == 2 && s.bsp_count == 2 && s.dropped_devices == 1 && s.dangling_names == 1);
	CHECK(((uintptr_t)s.defs | (uintptr_t)s.devices | (uintptr_t)s.groups | (uintptr_t)s.volumes | (uintptr_t)s.names) % 16 == (uintptr_t)raw % 16);

	/* device tags: sorted by tag id, found by binary search */
	{
		static const char *const names[7] = { "door", "platform", "gear", "toggle", "button on", "call", "lamp" };
		for (uint32_t i = 0; i < 7; i++) {
			devdef_def d = devdef_get_def(&s, i);
			CHECK(d.tag_id == TAGID(2 + i) && strcmp(d.name, names[i]) == 0 && devdef_find_def(&s, d.tag_id) == (int)i);
		}
		CHECK(devdef_find_def(&s, 0) == -1 && devdef_find_def(&s, 0xFFFFFFFFu) == -1 && devdef_find_def(&s, TAGID(2) + 1) == -1);
		devdef_def door = devdef_get_def(&s, 0), plat = devdef_get_def(&s, 1), gear = devdef_get_def(&s, 2), tog = devdef_get_def(&s, 3),
		           bon = devdef_get_def(&s, 4), call = devdef_get_def(&s, 5), lamp = devdef_get_def(&s, 6);
		CHECK(door.kind == DEVDEF_MACHINE && door.subtype == DEVDEF_MACH_DOOR && door.model_tag == TAGID(16) && door.anim_tag == TAGID(18));
		CHECK(door.power_ticks == 15 && door.power_accel_ticks == 3 && door.position_ticks == 30 && door.position_accel_ticks == 6);
		CHECK(door.depowered_ticks == 60 && door.depowered_accel_ticks == 0 && door.open_ticks == 90 && door.delay_ticks == 15);
		CHECK(door.auto_radius == 2.0f && door.kind_flags == 3 && door.collision == 1 && door.dev_flags == 0);
		CHECK(door.fn_in[0] == 3 && door.fn_in[1] == 1 && door.fn_in[2] == 0 && door.fn_in[3] == 0);
		CHECK(door.fx[DEVDEF_FX_OPEN] == TAGID(11) && door.fx[DEVDEF_FX_CLOSE] == TAGID(12) && door.fx[DEVDEF_FX_OPENED] == TAGID(14));
		CHECK(door.fx[DEVDEF_FX_CLOSED] == 0 && door.fx[DEVDEF_FX_DELAY] == TAGID(14) && door.fx_snd_mask == 3);
		CHECK(door.pos_anim == 0 && door.power_anim == DEVDEF_NO_ANIM && door.pos_frames == 10 && door.power_frames == 0);
		CHECK(plat.subtype == DEVDEF_MACH_PLATFORM && plat.kind_flags == DEVDEF_MF_ELEVATOR && plat.elevator_node == 2 && plat.position_ticks == 60);
		CHECK(plat.depowered_ticks == 30 && plat.auto_radius == 0.0f && plat.pos_anim == DEVDEF_NO_ANIM && plat.anim_tag == 0);
		CHECK(gear.subtype == DEVDEF_MACH_GEAR && gear.dev_flags == DEVDEF_DF_POSITION_LOOPS && gear.power_ticks == 30 && gear.position_ticks == 15);
		CHECK(gear.pos_anim == 0 && gear.power_anim == 1 && gear.pos_frames == 12 && gear.power_frames == 5 && gear.anim_tag == TAGID(19));
		CHECK(tog.kind == DEVDEF_CONTROL && tog.subtype == DEVDEF_CTRL_TOGGLE && tog.fx[DEVDEF_FX_CTL_ON] == TAGID(14) && tog.fx[DEVDEF_FX_CTL_OFF] == 0);
		CHECK(tog.fx[DEVDEF_FX_CTL_DENY] == TAGID(15));
		CHECK(bon.subtype == DEVDEF_CTRL_ON && call.subtype == DEVDEF_CTRL_CALL && fabsf(call.call_value - 0.75f) < 1e-6f && call.position_ticks == 15);
		CHECK(lamp.kind == DEVDEF_LIGHT && lamp.power_ticks == 8 && lamp.model_tag == 0 && lamp.pos_anim == DEVDEF_NO_ANIM);
		devdef_def none = devdef_get_def(&s, 7);                 /* out of range: a neutral record */
		CHECK(none.tag_id == 0 && none.pos_anim == DEVDEF_NO_ANIM && none.name[0] == 0);
	}

	/* placed devices, scenario order, SCEN indices */
	{
		static const uint16_t def[9] = { 0, 0, 1, 2, 0, 3, 4, 5, 6 };
		static const uint16_t flags[9] = { 1, 0, 0x0B, 1, 1, 1, 1, 1, 1 };
		static const uint32_t mask[9] = { 1, 1, 2, 1, 1, 1, 1, 1, 3 };
		static const int16_t pg[9] = { -1, -1, -1, 2, 0, 2, -1, -1, -1 };
		static const int16_t qg[9] = { -1, -1, 0, 1, -1, 0, 1, 2, -1 };
		static const uint32_t dfl[9] = { 0, 0, 0, 9, 6, 16, 0, 0, 0 };
		static const uint32_t kfl[9] = { 0, 2, 0, 0, 9, 1, 0, 0, 0 };
		static const int16_t nameidx[9] = { -1, 0, -1, -1, -1, 2, -1, -1, 5 };
		for (uint32_t i = 0; i < 9; i++) {
			devdef_device v = devdef_get_device(&s, i);
			CHECK(v.def == def[i] && v.flags == flags[i] && v.bsp_mask == mask[i] && v.power_group == pg[i] && v.position_group == qg[i]);
			CHECK(v.device_flags == dfl[i] && v.kind_flags == kfl[i] && v.name_index == nameidx[i] && v.scen_index == 2 + i);
			CHECK(devdef_find_device_by_scen(&s, 2 + i) == (int)i);
		}
		CHECK(devdef_find_device_by_scen(&s, 0) == -1 && devdef_find_device_by_scen(&s, 1) == -1 && devdef_find_device_by_scen(&s, 11) == -1);
		CHECK(devdef_find_device_by_scen(&s, DEVDEF_NONE) == -1);
		devdef_device d1 = devdef_get_device(&s, 1), d2 = devdef_get_device(&s, 2), d5 = devdef_get_device(&s, 5), d8 = devdef_get_device(&s, 8);
		CHECK(fabsf(d1.rot[0] - 3.14159f) < 1e-5f && d2.pos[0] == 25.0f && d2.pos[1] == 100.0f && d5.extra == 3);
		CHECK(fabsf(d8.light_color[0] - 0.5f) < 1e-6f && fabsf(d8.light_color[2] - 0.75f) < 1e-6f && fabsf(d8.light_intensity - 2.0f) < 1e-6f);
		CHECK(fabsf(d8.light_falloff - 1.2f) < 1e-6f && fabsf(d8.light_cutoff - 0.8f) < 1e-6f);
		devdef_device dn = devdef_get_device(&s, 9);             /* out of range */
		CHECK(dn.power_group == -1 && dn.name_index == -1 && dn.scen_index == DEVDEF_NONE);
	}

	/* groups, volumes, markup */
	{
		devdef_group g = devdef_get_group(&s, 1);
		CHECK(strcmp(g.name, "grp_b") == 0 && g.initial == 1.0f && g.flags == DEVDEF_GF_CHANGE_ONCE);
		CHECK(devdef_get_group(&s, 2).initial == 0.5f && devdef_find_group(&s, "grp_c") == 2 && devdef_find_group(&s, "grp_x") == -1);
		CHECK(devdef_find_group(&s, "") == -1 && devdef_find_group(&s, NULL) == -1);
		devdef_volume v1 = devdef_get_volume(&s, 1), v3 = devdef_get_volume(&s, 3);
		CHECK(v1.forward[1] == 1.0f && v1.left[0] == -1.0f && v1.up[2] == 1.0f && v1.position[0] == 20.0f && v1.extents[0] == 4.0f);
		CHECK(v3.extents[0] == 0.0f && v3.extents[1] == 0.0f && devdef_find_volume(&s, "vol_b") == 1 && devdef_find_volume(&s, "vol_z") == -1);
		devdef_volume vn = devdef_get_volume(&s, 4);
		CHECK(vn.forward[0] == 1.0f && vn.up[2] == 1.0f && vn.left[1] == 1.0f && vn.extents[0] == 0.0f);
		devdef_start s0 = devdef_get_start(&s, 0), s1 = devdef_get_start(&s, 1);
		CHECK(s0.pos[0] == -5.0f && s0.facing == 1.5f && s0.bsp_mask == 1 && s1.team == 100 && s1.bsp == 1 && s1.game_type[0] == 1 && s1.game_type[3] == 2);
		CHECK(s1.bsp_mask == 2 && devdef_get_start(&s, 2).bsp_mask == 0);
		devdef_cflag f0 = devdef_get_cflag(&s, 0), f1 = devdef_get_cflag(&s, 1);
		CHECK(strcmp(f0.name, "flag_a") == 0 && f0.pitch == 0.25f && f0.bsp_mask == 1 && f1.bsp_mask == 0 && devdef_find_cflag(&s, "flag_far") == 1);
		devdef_camera c0 = devdef_get_camera(&s, 0);
		CHECK(strcmp(c0.name, "cam_a") == 0 && fabsf(c0.fov - 1.2217f) < 1e-5f && c0.ori[0] == 0.5f && c0.bsp_mask == 2);
		CHECK(devdef_find_camera(&s, "cam_b") == 1 && devdef_get_camera(&s, 1).bsp_mask == 1 && devdef_find_camera(&s, "cam_c") == -1);
	}

	/* object names: sorted lookup, device / ambient / SCEN resolution, dangling entry */
	{
		static const char *const names[6] = { "door_a", "crate_a", "ctl_a", "hum_a", "ghost", "lamp_a" };
		static const uint8_t flags[6] = { 3, 1, 3, 5, 0, 3 };
		static const uint32_t scen[6] = { 3, 0, 7, DEVDEF_NONE, DEVDEF_NONE, 10 };
		static const uint32_t aux[6] = { 1, DEVDEF_NONE, 5, 0, DEVDEF_NONE, 8 };
		for (uint32_t i = 0; i < 6; i++) {
			devdef_name nm = devdef_get_name(&s, i);
			CHECK(strcmp(nm.name, names[i]) == 0 && nm.flags == flags[i] && nm.scen_index == scen[i] && nm.aux_index == aux[i]);
			CHECK(devdef_find_name(&s, names[i]) == (int)i);
		}
		CHECK(devdef_get_name(&s, 0).obj_type == 7 && devdef_get_name(&s, 0).table_index == 1 && devdef_get_name(&s, 1).obj_type == 6);
		CHECK(devdef_get_name(&s, 3).obj_type == 11 && devdef_get_name(&s, 5).obj_type == 9);
		CHECK(devdef_name_to_device(&s, 0) == 1 && devdef_name_to_device(&s, 1) == -1 && devdef_name_to_device(&s, 2) == 5 && devdef_name_to_device(&s, 4) == -1);
		CHECK(devdef_name_to_scen(&s, 0) == 3 && devdef_name_to_scen(&s, 1) == 0 && devdef_name_to_scen(&s, 3) == -1 && devdef_name_to_scen(&s, 4) == -1);
		CHECK(devdef_name_to_scen(&s, 99) == -1 && devdef_name_to_device(&s, 99) == -1);
		CHECK(devdef_find_name(&s, "door") == -1 && devdef_find_name(&s, "door_a ") == -1 && devdef_find_name(&s, "") == -1);
		CHECK(devdef_find_name(&s, NULL) == -1 && devdef_find_name(&s, "0123456789012345678901234567890123456789") == -1);
		devdef_name nn = devdef_get_name(&s, 6);
		CHECK(nn.scen_index == DEVDEF_NONE && nn.flags == 0);
	}

	/* sound scenery, decals, BSP switches */
	{
		devdef_ambient a0 = devdef_get_ambient(&s, 0), a1 = devdef_get_ambient(&s, 1);
		CHECK(a0.ssce_tag == TAGID(9) && a0.sound_tag == TAGID(10) && a0.name_index == 3 && a0.flags == 1 && a0.bsp_mask == 1);
		CHECK(a1.name_index == -1 && a1.flags == 0 && a1.pos[0] == 4.0f && devdef_get_ambient(&s, 2).name_index == -1);
		devdef_decal d0 = devdef_get_decal(&s, 0), d1 = devdef_get_decal(&s, 1);
		CHECK(d0.type == 0 && d0.yaw_raw == 64 && d0.pitch_raw == -64 && fabsf(d0.yaw - 1.5707964f) < 1e-6f && fabsf(d0.pitch + 1.5707964f) < 1e-6f);
		CHECK(d1.type == 1 && d1.yaw_raw == -32 && d1.pitch_raw == 16 && d1.bsp_mask == 2);
		CHECK(devdef_decal_type(&s, 0) == TAGID(20) && devdef_decal_type(&s, 1) == TAGID(21) && devdef_decal_type(&s, 2) == 0);
		devdef_bspsw b0 = devdef_get_bspsw(&s, 0), b1 = devdef_get_bspsw(&s, 1);
		CHECK(b0.volume == 0 && b0.source == 0 && b0.dest == 1 && b0.unknown == 0xFFFF && b1.volume == 1 && b1.source == 1 && b1.dest == 0 && b1.unknown == 5);
		CHECK(devdef_find_bspsw_by_volume(&s, 1) == 1 && devdef_find_bspsw_by_volume(&s, 2) == -1);
	}
	walk(&s);

	/* empty / failed views are safe */
	devdef_section unopened;
	CHECK(devdef_open(&unopened, raw, 10) == DEVDEF_ERR_TRUNCATED);
	CHECK(unopened.def_count == 0 && devdef_get_def(&unopened, 0).tag_id == 0 && devdef_find_name(&unopened, "x") == -1);
	walk(&unopened);

	/* ---- hostile input ---- */
	for (size_t cut = 0; cut < rawn; cut++) CHECK(devdef_open(&bad, raw, cut) != DEVDEF_OK);
	CHECK(devdef_open(&bad, NULL, rawn) != DEVDEF_OK);
	copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(devdef_open(&bad, copy, rawn + 16) == DEVDEF_ERR_LAYOUT);
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(devdef_open(&bad, copy, rawn) == DEVDEF_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(devdef_open(&bad, copy, rawn) == DEVDEF_ERR_MAGIC);

	/* every table of the header (count slot, offset slot): moved, misaligned, oversized, counted wrong */
	{
		static const char *const tn[13] = { "defs", "devices", "groups", "volumes", "starts", "flags", "cameras", "names", "name_order",
		                                    "ambients", "decals", "decal_types", "bspsw" };
		static const int count_slot[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 7, 8, 9, 10, 11 };
		for (size_t t = 0; t < 13; t++) {
			size_t cat = 0x0C + 4u * (size_t)count_slot[t], oat = 0x40 + 4 * t;
			uint32_t off = rd32(raw + oat), cnt = rd32(raw + cat);
			const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)rawn + 16, cnt }, { off, 0xFFFFFFFFu },
			                                                { off, (uint32_t)rawn }, { 0, cnt }, { 0xFFFFFFF0u, cnt } };
			for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
				if (t == 8 && k == 2) continue;                  /* the name order shares the count of the names: covered by "names" */
				memcpy(copy, raw, rawn);
				wr32(copy + oat, tries[k].off);
				wr32(copy + cat, tries[k].cnt);
				if (devdef_open(&bad, copy, rawn) == DEVDEF_OK) { fprintf(stderr, "table %s try %zu accepted\n", tn[t], k); return 1; }
			}
			for (int d = -1; d <= 1; d += 2) {                  /* merely wrong counts must never read out of bounds */
				memcpy(copy, raw, rawn);
				wr32(copy + cat, cnt + (uint32_t)d);
				if (devdef_open(&bad, copy, rawn) == DEVDEF_OK) walk(&bad);
			}
		}
		memcpy(copy, raw, rawn);
		wr32(copy + 0x3C, 33);                                   /* more BSPs than a mask has bits */
		CHECK(devdef_open(&bad, copy, rawn) != DEVDEF_OK);
	}

	/* targeted fields: each of these must be refused */
	{
		uint32_t od = rd32(raw + 0x40), ov = rd32(raw + 0x44), og = rd32(raw + 0x48), oo = rd32(raw + 0x4C), os = rd32(raw + 0x50);
		uint32_t of = rd32(raw + 0x54), oc = rd32(raw + 0x58), on = rd32(raw + 0x5C), oord = rd32(raw + 0x60), oa = rd32(raw + 0x64);
		uint32_t odc = rd32(raw + 0x68), ob = rd32(raw + 0x70);
		const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
			{ "defs out of order", od + 160, TAGID(2), 4 }, { "defs equal tags", od + 320, TAGID(3), 4 },
			{ "def kind 3", od + 12, 3, 1 }, { "machine subtype 3", od + 13, 3, 1 }, { "control subtype 4", od + 3 * 160 + 13, 4, 1 },
			{ "light subtype 1", od + 6 * 160 + 13, 1, 1 }, { "def radius NaN", od + 44, 0x7FC00000u, 4 },
			{ "def radius negative", od + 44, 0xBF800000u, 4 }, { "def call value inf", od + 116, 0x7F800000u, 4 },
			{ "def fx mask past 10 bits", od + 100, 0x400, 2 }, { "def fx mask bit without a tag", od + 100, 0x08, 2 },
			{ "def name unterminated", od + 120 + 31, 'x', 1 },
			{ "device def out of range", ov, 7, 2 }, { "device mask bit 2", ov + 4, 4, 4 }, { "device pos NaN", ov + 8, 0x7FC00000u, 4 },
			{ "device light inf", ov + 52, 0x7F800000u, 4 }, { "device power group 3", ov + 32, 3, 2 }, { "device power group -2", ov + 32, 0xFFFE, 2 },
			{ "device position group 3", ov + 34, 3, 2 }, { "device name index 6", ov + 44, 6, 2 },
			{ "device scen not ascending", ov + 80 + 48, 2, 4 }, { "device scen none", ov + 48, DEVDEF_NONE, 4 },
			{ "group name unterminated", og + 31, 'g', 1 }, { "group initial 2", og + 32, 0x40000000u, 4 }, { "group initial NaN", og + 32, 0x7FC00000u, 4 },
			{ "group initial negative", og + 32, 0xBF800000u, 4 },
			{ "volume name unterminated", oo + 31, 'v', 1 }, { "volume extent negative", oo + 80, 0xBF800000u, 4 },
			{ "volume forward NaN", oo + 32, 0x7FC00000u, 4 }, { "volume extent NaN", oo + 84, 0x7FC00000u, 4 },
			{ "start pos NaN", os, 0x7FC00000u, 4 }, { "start mask bit 5", os + 28, 32, 4 },
			{ "flag name unterminated", of + 31, 'f', 1 }, { "flag yaw inf", of + 44, 0x7F800000u, 4 },
			{ "camera fov NaN", oc + 56, 0x7FC00000u, 4 }, { "camera mask bit 7", oc + 60, 128, 4 },
			{ "name unterminated", on + 31, 'n', 1 }, { "name scen type 10", on + 33, 10, 1 }, { "name flags 8", on + 34, 8, 1 },
			{ "name device without resolved", on + 48 + 34, 2, 1 }, { "name device index out of range", on + 44, 9, 4 },
			{ "name aux set without device", on + 48 * 4 + 44, 0, 4 }, { "name order index out of range", oord, 6, 2 },
			{ "name order duplicate", oord + 2, 0, 2 }, { "ambient pos NaN", oa + 8, 0x7FC00000u, 4 },
			{ "ambient name index 6", oa + 32, 6, 2 }, { "decal type 2", odc, 2, 2 }, { "decal yaw NaN", odc + 16, 0x7FC00000u, 4 },
			{ "bspsw volume 4", ob, 4, 2 }, { "bspsw dest 2", ob + 4, 2, 2 }, { "header size", 8, 1, 4 } };
		for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
			memcpy(copy, raw, rawn);
			if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
			else if (hit[k].bytes == 2) { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
			else copy[hit[k].at] = (uint8_t)hit[k].val;
			if (devdef_open(&bad, copy, rawn) == DEVDEF_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
		}
	}

	/* no single-byte change anywhere may let an out-of-bounds read through */
	for (size_t at = 4; at < rawn; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (devdef_open(&bad, copy, rawn) == DEVDEF_OK) walk(&bad);
		}
	free(copy);
	free(raw);
	free(img);
	for (int a = 2; a < argc; a++)
		if (check_real(argv[a])) return 1;
	puts("ok");
	return 0;
}
