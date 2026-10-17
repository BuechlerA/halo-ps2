/* Host test: open the SCEN / SCE1 sections of a pack written by tools/tests/test_scenconv.py and check them
 * against the synthetic world that test builds (6 definitions, 8 placements in 3 BSPs of which BSP 1 has no
 * lighting probes, a player block; SCE1 is an empty section). Then truncate at every length, corrupt every
 * table and field, and flip every byte: a corrupt section is either rejected or safe to walk completely
 * (ASan/UBSan). */
#include "stream/hp2.h"
#include "world/scen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static volatile uint32_t sink;

/* Touch everything an opened section hands out. */
static void walk(const scen_section *s)
{
	uint32_t acc = 0, i, b, c;
	for (i = 0; i < s->def_count; i++) {
		scen_def d = scen_get_def(s, i);
		acc += d.tag_id + d.model_tag + d.anim_tag + d.type + d.idle_anim + (uint32_t)strlen(d.name);
		if (d.radius < 0.0f || d.radius != d.radius) abort();
		if (scen_find_def(s, d.tag_id) != (int)i) abort();
	}
	for (i = 0; i < s->placement_count; i++) {
		scen_placement p = scen_get_placement(s, i);
		if (p.def >= s->def_count) abort();
		acc += p.flags + p.bsp_mask + (uint32_t)strlen(p.variant) + (uint32_t)p.name_index;
	}
	for (b = 0; b < SCEN_MAX_BSPS + 2; b++) {
		acc += scen_placements_in_bsp(s, b);
		for (c = 0; c < 6; c++) {
			scen_light l;
			scen_cluster_light(s, b, c, &l);
			acc += l.ambient[0] + l.ambient[1] + l.ambient[2] + l.sun[0] + l.sun[1] + l.sun[2];
			if (fabsf(l.sun_dir[0]) > 1.0001f || fabsf(l.sun_dir[1]) > 1.0001f || fabsf(l.sun_dir[2]) > 1.0001f) abort();
		}
	}
	{
		scen_player p = scen_get_player(s);
		if (p.weapon_def > s->def_count || p.biped_def > s->def_count) abort();
		acc += (uint32_t)strlen(p.weapon_hand_marker) + p.arms_idle_anim;
	}
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

static int near3(const float *a, float x, float y, float z)
{
	return fabsf(a[0] - x) < 1e-4f && fabsf(a[1] - y) < 1e-4f && fabsf(a[2] - z) < 1e-4f;
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
	uint8_t *raw, *raw1;
	size_t rawn, rawn1;
	if (load_section(&pk, "SCEN", &raw, &rawn) || load_section(&pk, "SCE1", &raw1, &rawn1)) return 1;

	scen_section s;
	CHECK(scen_open(&s, raw, rawn) == SCEN_OK);
	CHECK(s.def_count == 6 && s.placement_count == 8 && s.bsp_count == 3);
	CHECK(((uintptr_t)s.defs | (uintptr_t)s.placements | (uintptr_t)s.lights | (uintptr_t)s.tables) % 16 == (uintptr_t)raw % 16);

	/* definitions: sorted by tag id, found by binary search */
	static const uint32_t tags[6] = { 0xE1760002u, 0xE1770003u, 0xE1780004u, 0xE1790005u, 0xE17A0006u, 0xE17B0007u };
	static const char *const names[6] = { "crate", "cyborg", "marine", "chair", "pistol", "door" };
	static const uint8_t types[6] = { SCEN_SCENERY, SCEN_BIPED, SCEN_BIPED, SCEN_VEHICLE, SCEN_WEAPON, SCEN_MACHINE };
	static const uint16_t idle[6] = { 1, 2, 2, SCEN_NO_ANIM, SCEN_NO_ANIM, SCEN_NO_ANIM };
	for (uint32_t i = 0; i < 6; i++) {
		scen_def d = scen_get_def(&s, i);
		CHECK(d.tag_id == tags[i] && d.type == types[i] && d.idle_anim == idle[i] && strcmp(d.name, names[i]) == 0);
		CHECK(scen_find_def(&s, tags[i]) == (int)i);
	}
	CHECK(scen_find_def(&s, 0) == -1 && scen_find_def(&s, 0xFFFFFFFFu) == -1 && scen_find_def(&s, tags[2] + 1) == -1);
	scen_def crate = scen_get_def(&s, 0), cyborg = scen_get_def(&s, 1), marine = scen_get_def(&s, 2), chair = scen_get_def(&s, 3),
	         pistol = scen_get_def(&s, 4), door = scen_get_def(&s, 5);
	CHECK(fabsf(crate.radius - 1.5f) < 1e-6f && fabsf(door.radius - 0.9f) < 1e-6f && fabsf(pistol.radius - 0.1f) < 1e-6f);
	CHECK(crate.model_tag && crate.anim_tag && cyborg.model_tag == marine.model_tag && cyborg.anim_tag == marine.anim_tag);
	CHECK(chair.model_tag == 0 && chair.anim_tag && pistol.model_tag && pistol.anim_tag == 0 && door.anim_tag);
	scen_def none = scen_get_def(&s, 6);                      /* out of range: a neutral, static record */
	CHECK(none.tag_id == 0 && none.idle_anim == SCEN_NO_ANIM && none.name[0] == 0);

	/* placements, in scenario order */
	static const uint16_t pdef[8] = { 0, 0, 0, 0, 2, 3, 4, 5 };
	static const uint16_t pflags[8] = { 1, 0, 11, 1, 0, 1, 1, 1 };
	static const uint32_t pmask[8] = { 1, 1, 2, 2, 1, 1, 0, 1 };
	static const int16_t pname[8] = { 0, -1, 1, -1, -1, -1, -1, -1 };
	static const float px[8] = { -5.0f, 5.0f, 25.0f, 15.0f, 10.05f, 10.8f, 40.0f, -3.0f };
	for (uint32_t i = 0; i < 8; i++) {
		scen_placement p = scen_get_placement(&s, i);
		CHECK(p.def == pdef[i] && p.flags == pflags[i] && p.bsp_mask == pmask[i] && p.name_index == pname[i]);
		CHECK(fabsf(p.pos[0] - px[i]) < 1e-5f && p.variant[0] == 0);
	}
	scen_placement p0 = scen_get_placement(&s, 0), p2 = scen_get_placement(&s, 2), p7 = scen_get_placement(&s, 7);
	CHECK(p0.pos[1] == 100.0f && p0.pos[2] == 0.5f && p0.rot[0] == 0.5f);
	CHECK(p2.rot[0] == 1.0f && p2.rot[1] == 0.25f && p2.rot[2] == -0.25f && fabsf(p7.rot[0] - 3.14159f) < 1e-5f);
	scen_placement pn = scen_get_placement(&s, 8);            /* out of range */
	CHECK(pn.name_index == -1 && pn.bsp_mask == 0);
	CHECK(scen_placements_in_bsp(&s, 0) == 5 && scen_placements_in_bsp(&s, 1) == 2 && scen_placements_in_bsp(&s, 2) == 0);
	CHECK(scen_placements_in_bsp(&s, 31) == 0 && scen_placements_in_bsp(&s, 32) == 0 && scen_placements_in_bsp(&s, 0xFFFFFFFFu) == 0);

	/* cluster lighting: BSP 0 has two probes, BSP 1 none (neutral), BSP 2 three */
	scen_light l;
	scen_cluster_light(&s, 0, 0, &l);
	CHECK(l.ambient[0] == 20 && l.ambient[1] == 30 && l.ambient[2] == 40 && l.sun[0] == 50 && l.sun[1] == 60 && l.sun[2] == 70);
	CHECK(near3(l.sun_dir, 0.0f, 0.0f, 1.0f));
	scen_cluster_light(&s, 0, 1, &l);
	CHECK(l.ambient[0] == 255 && l.ambient[1] == 0 && l.ambient[2] == 128 && l.sun[0] == 1 && l.sun[1] == 2 && l.sun[2] == 3);
	CHECK(near3(l.sun_dir, -0.6f, 0.8f, 0.0f));
	scen_cluster_light(&s, 2, 2, &l);
	CHECK(l.ambient[0] == 9 && l.sun[0] == 0 && near3(l.sun_dir, 0.0f, 0.6f, -0.8f));
	static const struct { uint32_t bsp, cluster; } missing[] = { { 0, 2 }, { 1, 0 }, { 2, 3 }, { 3, 0 }, { 31, 0 }, { 0xFFFFFFFFu, 0 }, { 0, 0xFFFFFFFFu } };
	for (size_t i = 0; i < sizeof missing / sizeof missing[0]; i++) {
		scen_cluster_light(&s, missing[i].bsp, missing[i].cluster, &l);
		CHECK(l.ambient[0] == 64 && l.ambient[1] == 64 && l.ambient[2] == 64 && l.sun[0] == 64 && l.sun[2] == 64);
		CHECK(near3(l.sun_dir, 0.0f, 0.0f, 1.0f));
	}

	/* player */
	scen_player pl = scen_get_player(&s);
	CHECK(pl.fp_arms_model_tag && pl.fp_arms_anim_tag && pl.fp_arms_anim_tag == pl.weapon_fp_anim_tag && pl.weapon_fp_model_tag);
	CHECK(pl.weapon_def == 5 && pl.biped_def == 2 && pl.arms_idle_anim == 0 && pl.weapon_idle_anim == 0);
	CHECK(pl.fp_offset[0] == 0.0f && pl.fp_offset[1] == 0.0f && pl.fp_offset[2] == 0.0f);
	CHECK(strcmp(pl.weapon_hand_marker, "frame r wriste") == 0);
	walk(&s);

	/* SCE1: empty section */
	scen_section e;
	CHECK(scen_open(&e, raw1, rawn1) == SCEN_OK);
	CHECK(e.def_count == 0 && e.placement_count == 0 && e.bsp_count == 0);
	CHECK(scen_find_def(&e, tags[0]) == -1 && scen_placements_in_bsp(&e, 0) == 0);
	scen_cluster_light(&e, 0, 0, &l);
	CHECK(l.ambient[1] == 64 && near3(l.sun_dir, 0.0f, 0.0f, 1.0f));
	pl = scen_get_player(&e);
	CHECK(pl.weapon_def == 0 && pl.biped_def == 0 && pl.arms_idle_anim == SCEN_NO_ANIM && pl.weapon_hand_marker[0] == 0);
	walk(&e);
	scen_section unopened;
	CHECK(scen_open(&unopened, raw, 10) == SCEN_ERR_TRUNCATED);
	CHECK(unopened.def_count == 0 && scen_get_player(&unopened).weapon_def == 0);   /* a failed open leaves a safe, empty view */
	walk(&unopened);

	/* ---- hostile input ---- */
	scen_section bad;
	for (size_t cut = 0; cut < rawn; cut++) CHECK(scen_open(&bad, raw, cut) != SCEN_OK);
	CHECK(scen_open(&bad, NULL, rawn) != SCEN_OK);
	uint8_t *copy = malloc(rawn + 16);
	CHECK(copy);
	memcpy(copy, raw, rawn);
	memset(copy + rawn, 0, 16);
	CHECK(scen_open(&bad, copy, rawn + 16) == SCEN_ERR_LAYOUT);       /* size field no longer matches */
	memcpy(copy, raw, rawn);
	copy[4] = 2;
	CHECK(scen_open(&bad, copy, rawn) == SCEN_ERR_VERSION);
	memcpy(copy, raw, rawn);
	copy[0] = 'X';
	CHECK(scen_open(&bad, copy, rawn) == SCEN_ERR_MAGIC);

	/* every counted table of the header (offset, count): moved, misaligned, oversized, counted wrong */
	static const struct { const char *name; size_t count_at, off_at; } tabs[] = {
		{ "defs", 0x0C, 0x18 }, { "placements", 0x10, 0x1C }, { "bsps", 0x14, 0x24 }, { "lights", 0x2C, 0x20 } };
	for (size_t t = 0; t < sizeof tabs / sizeof tabs[0]; t++) {
		uint32_t off = rd32(raw + tabs[t].off_at), cnt = rd32(raw + tabs[t].count_at);
		const struct { uint32_t off, cnt; } tries[] = { { off + 4, cnt }, { (uint32_t)rawn + 16, cnt }, { off, 0xFFFFFFFFu },
		                                                { off, (uint32_t)rawn }, { 0, cnt }, { 0xFFFFFFF0u, cnt } };
		for (size_t k = 0; k < sizeof tries / sizeof tries[0]; k++) {
			memcpy(copy, raw, rawn);
			wr32(copy + tabs[t].off_at, tries[k].off);
			wr32(copy + tabs[t].count_at, tries[k].cnt);
			if (scen_open(&bad, copy, rawn) == SCEN_OK) { fprintf(stderr, "table %s try %zu accepted\n", tabs[t].name, k); return 1; }
		}
		for (int d = -1; d <= 1; d += 2) {                              /* merely wrong counts must never read out of bounds */
			memcpy(copy, raw, rawn);
			wr32(copy + tabs[t].count_at, cnt + (uint32_t)d);
			if (scen_open(&bad, copy, rawn) == SCEN_OK) walk(&bad);
		}
	}
	{
		memcpy(copy, raw, rawn);
		wr32(copy + 0x28, 0);                                            /* player block before the header end */
		CHECK(scen_open(&bad, copy, rawn) != SCEN_OK);
		memcpy(copy, raw, rawn);
		wr32(copy + 0x28, (uint32_t)rawn - 16);                         /* player block runs off the end */
		CHECK(scen_open(&bad, copy, rawn) != SCEN_OK);
		memcpy(copy, raw, rawn);
		wr32(copy + 0x14, SCEN_MAX_BSPS + 1);                           /* more BSPs than a mask has bits */
		CHECK(scen_open(&bad, copy, rawn) != SCEN_OK);
	}

	/* targeted fields: each of these must be refused */
	uint32_t od = rd32(raw + 0x18), op = rd32(raw + 0x1C), ot = rd32(raw + 0x24), opl = rd32(raw + 0x28);
	const struct { const char *what; size_t at; uint32_t val; int bytes; } hit[] = {
		{ "defs out of order", od + 64, tags[0], 4 }, { "defs equal tags", od + 128, tags[1], 4 },
		{ "def type 10", od + 12, 10, 1 }, { "def name unterminated", od + 16 + 31, 'x', 1 },
		{ "def radius NaN", od + 48, 0x7FC00000u, 4 }, { "def radius negative", od + 48, 0xBF800000u, 4 },
		{ "def radius inf", od + 48, 0x7F800000u, 4 },
		{ "placement def out of range", op, 6, 2 }, { "placement mask bit 3", op + 4, 8, 4 },
		{ "placement mask bit 31", op + 4, 0x80000000u, 4 }, { "placement pos NaN", op + 8, 0x7FC00000u, 4 },
		{ "placement rot inf", op + 28, 0x7F800000u, 4 }, { "variant unterminated", op + 32 + 15, 'v', 1 },
		{ "table first out of range", ot, 100, 4 }, { "table count out of range", ot + 4, 100, 4 },
		{ "table first + count wraps", ot + 8, 0xFFFFFFFFu, 4 }, { "table count wraps", ot + 12, 0xFFFFFFFFu, 4 },
		{ "weapon_def past the defs", opl + 8, 7, 4 }, { "biped_def past the defs", opl + 20, 7, 4 },
		{ "marker unterminated", opl + 40 + 31, 'm', 1 }, { "fp offset NaN", opl + 28, 0x7FC00000u, 4 },
		{ "header size", 8, 1, 4 } };
	for (size_t k = 0; k < sizeof hit / sizeof hit[0]; k++) {
		memcpy(copy, raw, rawn);
		if (hit[k].bytes == 4) wr32(copy + hit[k].at, hit[k].val);
		else if (hit[k].bytes == 2) { copy[hit[k].at] = (uint8_t)hit[k].val; copy[hit[k].at + 1] = (uint8_t)(hit[k].val >> 8); }
		else copy[hit[k].at] = (uint8_t)hit[k].val;
		if (scen_open(&bad, copy, rawn) == SCEN_OK) { fprintf(stderr, "accepted: %s\n", hit[k].what); return 1; }
	}

	/* no single-byte change anywhere may let an out-of-bounds read through (both sections) */
	for (size_t at = 4; at < rawn; at++)
		for (int pat = 0; pat < 3; pat++) {
			memcpy(copy, raw, rawn);
			copy[at] = pat == 0 ? (uint8_t)(copy[at] ^ 0xFF) : pat == 1 ? (uint8_t)(copy[at] ^ 0x01) : 0x7F;
			if (scen_open(&bad, copy, rawn) == SCEN_OK) walk(&bad);
		}
	for (size_t cut = 0; cut < rawn1; cut++) CHECK(scen_open(&bad, raw1, cut) != SCEN_OK);
	for (size_t at = 4; at < rawn1; at++) {
		uint8_t *c1 = malloc(rawn1);
		CHECK(c1);
		memcpy(c1, raw1, rawn1);
		c1[at] ^= 0xFF;
		if (scen_open(&bad, c1, rawn1) == SCEN_OK) walk(&bad);
		free(c1);
	}
	free(copy);
	free(raw);
	free(raw1);
	free(img);
	puts("ok");
	return 0;
}
