/* Host test of the CINE section reader (world/cinedef.c).
 *   test_cinedef <fixture.hp2> [<real.cin.hp2> <golden.txt>]...
 * The fixture is the synthetic world of tools/tests/test_cinconv.py (4 recordings of control data versions 4, 3, 0 and 4, two
 * camera tracks of 9 and 3 points, two titles, five help strings, two animation references, two graphs): every field is checked
 * against the numbers that test hard-codes. The real packs (optional; converted by tools/halo2ps2/cinconv.py) are walked
 * completely and every recording is played tick by tick and hashed; the hash must equal the one the Python reference player
 * wrote into the golden file. Then every truncation, and a flip of (nearly) every byte, of each section is opened: a corrupt
 * section is either rejected or safe to walk completely (ASan/UBSan). */
#include "stream/hp2.h"
#include "world/cinedef.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static volatile uint32_t sink;
static uint32_t fuzz_accepted, fuzz_total;

static uint8_t *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	buf = malloc(*n ? *n : 1);
	if (!buf || fread(buf, 1, *n, f) != *n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	return buf;
}

static int load_section(const char *path, uint8_t **raw, size_t *n)
{
	size_t fn;
	uint8_t *img = read_file(path, &fn);
	hp2_pack pk;
	const hp2_section *sec;
	CHECK(img);
	CHECK(hp2_open(&pk, img, fn) == HP2_OK);
	sec = hp2_find(&pk, "CINE");
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(&pk, sec, *raw, sec->raw_size) == HP2_OK);
	*n = sec->raw_size;
	free(img);
	return 0;
}

static uint32_t fnv_byte(uint32_t h, uint8_t b) { return (h ^ b) * 0x01000193u; }
static uint32_t fnv_u16(uint32_t h, uint16_t v) { return fnv_byte(fnv_byte(h, (uint8_t)v), (uint8_t)(v >> 8)); }
static uint32_t fnv_f32(uint32_t h, float f)
{
	uint32_t u;
	int i;
	memcpy(&u, &f, 4);
	for (i = 0; i < 4; i++) h = fnv_byte(h, (uint8_t)(u >> (8 * i)));
	return h;
}

/* Independent of sim/cine.c: replays a recording with nothing but the reader's event decoder and hashes the state of each tick
 * the way tools/halo2ps2/cinconv.py state_hash does. */
static uint32_t play_hash(const cinedef_section *s, uint32_t rec)
{
	cinedef_rec r = cinedef_get_rec(s, rec);
	uint32_t bytes, pos = 0, next = 0, tick, h = 0x811C9DC5u;
	const uint8_t *st = cinedef_rec_stream(s, rec, &bytes);
	cinedef_ctl c = r.init;
	cinedef_event ev;
	int have = cinedef_next_event(st, bytes, &pos, &ev);
	for (tick = 0; tick < r.length; tick++) {
		while (have && next + ev.wait <= tick) {                   /* a wait is the delay before its event */
			cinedef_apply_event(&c, &ev);
			next += ev.wait;
			have = cinedef_next_event(st, bytes, &pos, &ev);
		}
		h = fnv_u16(h, c.flags);
		h = fnv_u16(h, (uint16_t)c.weapon);
		h = fnv_byte(h, c.mode);
		h = fnv_u16(h, (uint16_t)c.yaw);
		h = fnv_u16(h, (uint16_t)c.pitch);
		h = fnv_f32(h, c.throttle[0]);
		h = fnv_f32(h, c.throttle[1]);
	}
	return h;
}

/* Touches everything an opened section hands out. Returns 0 if an invariant is violated. */
static int walk(const cinedef_section *s)
{
	uint32_t acc = 0, i, k, n;
	uint16_t buf[8];
	for (i = 0; i < s->rec_count; i++) {
		cinedef_rec r = cinedef_get_rec(s, i);
		uint32_t bytes, pos = 0, count = 0;
		uint64_t total = 0;
		const uint8_t *st = cinedef_rec_stream(s, i, &bytes);
		cinedef_event ev;
		cinedef_ctl c = r.init;
		if (!st || bytes != r.stream_bytes || r.name[CINEDEF_NAME_LEN - 1] != 0) return 0;
		if (r.length == 0 || r.length > CINEDEF_MAX_LENGTH) return 0;
		while (cinedef_next_event(st, bytes, &pos, &ev)) {
			cinedef_apply_event(&c, &ev);
			total += ev.wait;
			count++;
			if (c.yaw < -CINEDEF_YAW_HALF || c.yaw > CINEDEF_YAW_HALF || c.pitch < -CINEDEF_PITCH_LIMIT || c.pitch > CINEDEF_PITCH_LIMIT) return 0;
			if (c.throttle[0] != c.throttle[0] || c.throttle[1] != c.throttle[1]) return 0;
		}
		if (pos != bytes || count != r.events || total + 1 != r.length) return 0;
		acc += r.length + c.flags + (uint32_t)(c.yaw + 2000);
		if (cinedef_find_rec(s, r.name) < 0 || cinedef_find_rec(s, r.name) > (int)i) return 0;
	}
	for (i = 0; i < s->track_count; i++) {
		cinedef_track t = cinedef_get_track(s, i);
		if (cinedef_find_track(s, t.tag_id) != (int)i || t.point_count < 2) return 0;
		for (k = 0; k < t.point_count; k++) {
			cinedef_point p = cinedef_get_point(s, i, k);
			if (p.quat[0] != p.quat[0] || p.pos[2] != p.pos[2]) return 0;
			acc += (uint32_t)fminf(fabsf(p.pos[0]), 1000000.0f);
		}
		if (cinedef_get_point(s, i, t.point_count).quat[3] != 0.0f) return 0;      /* out of range: zeroed */
	}
	for (i = 0; i < s->title_count; i++) {
		cinedef_title t = cinedef_get_title(s, i);
		if (t.string != CINEDEF_NONE16 && t.string >= s->string_count) return 0;
		if (t.string != CINEDEF_NONE16) {
			n = cinedef_get_string(s, t.string, buf, 8);
			if (n == 0xFFFFFFFFu) return 0;
			acc += n + buf[0];
		}
		if (cinedef_find_title(s, t.name) < 0) return 0;
		acc += t.fade_in + t.up + t.fade_out + t.top;
	}
	for (i = 0; i < s->string_count; i++) acc += cinedef_get_string(s, i, buf, 8);
	for (i = 0; i < s->anim_ref_count; i++) {
		cinedef_anim_ref a = cinedef_get_anim_ref(s, i);
		acc += a.graph_tag + a.resolved + (uint32_t)strlen(a.name);
	}
	for (i = 0; i < s->graph_count; i++)
		if (!cinedef_has_graph(s, cinedef_get_graph(s, i))) return 0;
	sink = acc;
	return 1;
}

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static int check_synthetic(const uint8_t *raw, size_t rawn)
{
	cinedef_section s;
	cinedef_status st = cinedef_open(&s, raw, rawn);
	uint32_t i, bytes, pos;
	cinedef_event ev;
	cinedef_ctl c;
	uint16_t buf[16];
	CHECK(st == CINEDEF_OK);
	CHECK(s.rec_count == 4 && s.track_count == 2 && s.point_count == 12 && s.title_count == 2 && s.string_count == 5 &&
	      s.anim_ref_count == 2 && s.graph_count == 2 && s.help_tag != 0);
	CHECK(walk(&s));

	/* recordings */
	static const char *const names[4] = { "rec_a", "rec_b", "rec_c", "rec_a" };
	static const uint32_t lens[4] = { 326, 11, 3, 326 };
	static const uint8_t vers[4] = { 4, 3, 0, 4 };
	static const uint32_t events[4] = { 6, 2, 3, 6 };
	for (i = 0; i < 4; i++) {
		cinedef_rec r = cinedef_get_rec(&s, i);
		CHECK(strcmp(r.name, names[i]) == 0 && r.length == lens[i] && r.ctl_version == vers[i] && r.events == events[i]);
		CHECK(r.version == 4 && r.raw == 0);
	}
	CHECK(cinedef_find_rec(&s, "rec_a") == 0 && cinedef_find_rec(&s, "rec_c") == 2 && cinedef_find_rec(&s, "rec_x") == -1 &&
	      cinedef_find_rec(&s, "") == -1 && cinedef_find_rec(&s, NULL) == -1);
	cinedef_rec ra = cinedef_get_rec(&s, 0), rb = cinedef_get_rec(&s, 1), rc = cinedef_get_rec(&s, 2);
	CHECK(ra.init.mode == 3 && ra.init.flags == 0x0300 && ra.init.weapon == -1 && ra.init.extra_a == -1 && ra.init.extra_b == -1 &&
	      ra.init.yaw == 100 && ra.init.pitch == 0 && ra.init.throttle[0] == 0.0f);
	CHECK(rb.init.weapon == 0 && rb.init.extra_a == 1 && rb.init.yaw == -1000 && rb.init.pitch == -500);
	CHECK(rc.init.mode == 0 && rc.init.flags == 0x12 && rc.init.weapon == -1);
	{
		cinedef_rec none = cinedef_get_rec(&s, 4);
		CHECK(none.name[0] == 0 && none.length == 0);
		CHECK(cinedef_rec_stream(&s, 4, &bytes) == NULL && bytes == 0);
	}
	/* the duplicated recording shares its stream */
	{
		uint32_t b0, b3;
		CHECK(cinedef_rec_stream(&s, 0, &b0) == cinedef_rec_stream(&s, 3, &b3) && b0 == b3);
	}

	/* the events of the first recording */
	static const uint8_t types[6] = { CINEDEF_EV_THROTTLE, CINEDEF_EV_AIM_SMALL, CINEDEF_EV_FLAGS, CINEDEF_EV_AIM_LARGE,
	                                  CINEDEF_EV_WEAPON, CINEDEF_EV_WAIT };
	static const uint32_t waits[6] = { 0, 1, 0, 4, 300, 20 };
	const uint8_t *stream = cinedef_rec_stream(&s, 0, &bytes);
	c = ra.init;
	pos = 0;
	for (i = 0; i < 6; i++) {
		CHECK(cinedef_next_event(stream, bytes, &pos, &ev));
		CHECK(ev.type == types[i] && ev.wait == waits[i]);
		cinedef_apply_event(&c, &ev);
		if (i == 0) CHECK(c.throttle[0] == 1.0f && c.throttle[1] == 0.0f);
		if (i == 1) CHECK(c.yaw == 103 && c.pitch == -2);
		if (i == 2) CHECK(c.flags == 0x0800);
		if (i == 3) CHECK(c.yaw == 53 && c.pitch == 3);             /* 103 + 1950 wraps */
		if (i == 4) CHECK(c.weapon == 1);
	}
	CHECK(!cinedef_next_event(stream, bytes, &pos, &ev) && pos == bytes);
	/* yaw wrap and pitch clamp of the second recording */
	stream = cinedef_rec_stream(&s, 1, &bytes);
	c = rb.init;
	pos = 0;
	CHECK(cinedef_next_event(stream, bytes, &pos, &ev) && ev.type == CINEDEF_EV_AIM_SMALL && ev.wait == 0);
	cinedef_apply_event(&c, &ev);
	CHECK(c.yaw == 999 && c.pitch == -500);
	/* malformed events are refused, not read */
	{
		static const uint8_t cut[] = { CINEDEF_EV_FLAGS << 2 | 2, 5, 0 };           /* payload short by one byte */
		static const uint8_t bad[] = { 3 << 2 };                                     /* unknown type */
		uint32_t p = 0;
		CHECK(!cinedef_next_event(cut, sizeof cut, &p, &ev) && p == 0);
		CHECK(!cinedef_next_event(bad, sizeof bad, &p, &ev) && p == 0);
		CHECK(!cinedef_next_event(NULL, 0, &p, &ev));
	}

	/* camera tracks: sorted by tag id, 9 and 3 points */
	cinedef_track t0 = cinedef_get_track(&s, 0), t1 = cinedef_get_track(&s, 1);
	CHECK(t0.tag_id < t1.tag_id && t0.point_count == 9 && t1.point_count == 3 && t0.first_point == 0 && t1.first_point == 9);
	CHECK(strcmp(t0.name, "small") == 0 && strcmp(t1.name, "gunner") == 0);
	CHECK(cinedef_find_track(&s, t0.tag_id) == 0 && cinedef_find_track(&s, t1.tag_id) == 1 && cinedef_find_track(&s, t0.tag_id + 1) == -1);
	for (i = 0; i < 9; i++) {
		cinedef_point p = cinedef_get_point(&s, 0, i);
		float a = (90.0f - 22.5f * (float)i) * 3.14159265f / 180.0f;
		CHECK(near(p.pos[0], -2.0f * cosf(a), 1e-5f) && near(p.pos[1], 0.0f, 1e-6f) && near(p.pos[2], 0.5f + 2.0f * sinf(a), 1e-5f));
		/* engine convention: the conjugate of the stored quaternion, rotation a about +y */
		CHECK(near(p.quat[0], 0.0f, 1e-6f) && near(p.quat[1], sinf(a * 0.5f), 1e-6f) && near(p.quat[2], 0.0f, 1e-6f) && near(p.quat[3], cosf(a * 0.5f), 1e-6f));
	}
	CHECK(near(cinedef_get_point(&s, 1, 2).pos[2], 0.16f, 1e-6f) && near(cinedef_get_point(&s, 1, 0).pos[0], 0.34f, 1e-6f));
	CHECK(cinedef_get_track(&s, 2).tag_id == 0 && cinedef_get_point(&s, 7, 0).quat[3] == 0.0f);

	/* titles and strings */
	cinedef_title a = cinedef_get_title(&s, 0), b = cinedef_get_title(&s, 1);
	CHECK(strcmp(a.name, "t_first") == 0 && a.top == 425 && a.left == 0 && a.bottom == 446 && a.right == 597 && a.string == 1 && a.style == 1 &&
	      a.justify == 0);
	CHECK(a.colour == 0x80808080u && a.shadow == 0x80000000u && a.fade_in == 30 && a.up == 120 && a.fade_out == 30);
	CHECK(strcmp(b.name, "t_bad") == 0 && b.string == CINEDEF_NONE16 && b.left == 50 && b.fade_in == 0 && b.up == 90 && b.fade_out == 0);
	CHECK(cinedef_find_title(&s, "t_bad") == 1 && cinedef_find_title(&s, "nothing") == -1);
	CHECK(cinedef_get_title(&s, 2).string == CINEDEF_NONE16);
	CHECK(cinedef_get_string(&s, 1, buf, 16) == 6 && buf[0] == 'B' && buf[4] == ' ' && buf[5] == 0xE9 && buf[6] == 0);
	CHECK(cinedef_get_string(&s, 2, buf, 16) == 0 && buf[0] == 0);
	CHECK(cinedef_get_string(&s, 0, buf, 3) == 5 && buf[0] == 'A' && buf[2] == 'p');                   /* truncated copy reports the real length */
	CHECK(cinedef_get_string(&s, 5, buf, 16) == 0xFFFFFFFFu && cinedef_get_string(&s, 0, NULL, 0) == 5);

	/* references */
	cinedef_anim_ref r0 = cinedef_get_anim_ref(&s, 0), r1 = cinedef_get_anim_ref(&s, 1);
	CHECK(strcmp(r0.name, "idle_a") == 0 && r0.resolved == 1 && strcmp(r1.name, "zzz") == 0 && r1.resolved == 0);
	CHECK(r0.graph_tag == cinedef_get_graph(&s, 0) && r1.graph_tag == cinedef_get_graph(&s, 1) && r0.graph_tag < r1.graph_tag);
	CHECK(cinedef_has_graph(&s, r0.graph_tag) && !cinedef_has_graph(&s, 1) && cinedef_get_graph(&s, 2) == 0);
	CHECK(cinedef_get_anim_ref(&s, 2).graph_tag == 0);

	/* a section that failed to open hands out neutral records */
	{
		cinedef_section z;
		CHECK(cinedef_open(&z, raw, 20) == CINEDEF_ERR_TRUNCATED);
		CHECK(cinedef_get_rec(&z, 0).length == 0 && cinedef_find_rec(&z, "rec_a") == -1 && cinedef_find_track(&z, 1) == -1 &&
		      cinedef_get_string(&z, 0, buf, 4) == 0xFFFFFFFFu && !cinedef_has_graph(&z, 1) && cinedef_find_title(&z, "t") == -1);
	}
	return 0;
}

/* every truncation, and a flip of every `step`-th byte with three patterns */
static int fuzz(const uint8_t *raw, size_t n, size_t step)
{
	uint8_t *copy = malloc(n);
	size_t len, i;
	static const uint8_t flips[3] = { 0xFF, 0x01, 0x80 };
	unsigned f;
	cinedef_section s;
	CHECK(copy);
	for (len = 0; len < n; len += (n > 20000 ? 7 : 1)) {
		cinedef_status st;
		memcpy(copy, raw, len);
		st = cinedef_open(&s, copy, len);
		CHECK(st != CINEDEF_OK);
	}
	for (i = 0; i < n; i += step)
		for (f = 0; f < 3; f++) {
			memcpy(copy, raw, n);
			copy[i] ^= flips[f];
			fuzz_total++;
			if (cinedef_open(&s, copy, n) == CINEDEF_OK) {
				CHECK(walk(&s));
				fuzz_accepted++;
			}
		}
	free(copy);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t *raw;
	size_t rawn;
	int a;
	CHECK(argc >= 2);
	if (load_section(argv[1], &raw, &rawn)) return 1;
	if (check_synthetic(raw, rawn)) return 1;
	if (fuzz(raw, rawn, 1)) return 1;
	free(raw);

	for (a = 2; a + 1 < argc; a += 2) {
		cinedef_section s;
		FILE *g;
		char line[128];
		uint32_t total_ticks = 0, i;
		unsigned idx, len, want;
		CHECK(load_section(argv[a], &raw, &rawn) == 0);
		CHECK(cinedef_open(&s, raw, rawn) == CINEDEF_OK);
		CHECK(walk(&s));
		g = fopen(argv[a + 1], "r");
		CHECK(g);
		i = 0;
		while (fgets(line, sizeof line, g)) {
			CHECK(sscanf(line, "%u %u %x", &idx, &len, &want) == 3);
			CHECK(idx == i && idx < s.rec_count && cinedef_get_rec(&s, idx).length == len);
			CHECK(play_hash(&s, idx) == want);
			total_ticks += len;
			i++;
		}
		fclose(g);
		CHECK(i == s.rec_count);
		printf("%s: %u recordings, %u ticks played and hashed, %u tracks, %u titles\n", argv[a], s.rec_count, total_ticks,
		       s.track_count, s.title_count);
		if (fuzz(raw, rawn, rawn > 20000 ? 97 : 11)) return 1;
		free(raw);
	}
	printf("fuzz: %u flipped sections opened, %u accepted and walked\n", fuzz_total, fuzz_accepted);
	printf("ok\n");
	return 0;
}
