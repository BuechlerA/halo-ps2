/* CINE section reader (docs/28-cine-section.md). Pure C11, no heap, no double. cinedef_open validates every table, string and
 * event stream once, so the accessors can never read out of bounds, whatever the bytes. Records are decoded with byte loads:
 * no alignment assumptions on the section buffer. */
#include "world/cinedef.h"
#include <string.h>

#define HEADER_SIZE 128u
#define REC_SIZE 80u
#define TRACK_SIZE 32u
#define POINT_SIZE 32u
#define TITLE_SIZE 64u
#define AREF_SIZE 48u
#define STR_SIZE 8u
#define CINE_VERSION 1u
#define MAX_TABLE (1u << 22)

/* header: +0 "CINE", +4 u16 version, +8 size, +0x0C recs, +0x10 recs off, +0x14 pool off, +0x18 pool bytes, +0x1C tracks,
 * +0x20 tracks off, +0x24 points, +0x28 points off, +0x2C titles, +0x30 titles off, +0x34 strings, +0x38 strings off,
 * +0x3C char units, +0x40 chars off, +0x44 help tag, +0x48 anim refs, +0x4C refs off, +0x50 graphs, +0x54 graphs off */
#define H_SIZE 0x08
#define H_RECS 0x0C
#define H_O_RECS 0x10
#define H_O_POOL 0x14
#define H_POOL 0x18
#define H_TRACKS 0x1C
#define H_O_TRACKS 0x20
#define H_POINTS 0x24
#define H_O_POINTS 0x28
#define H_TITLES 0x2C
#define H_O_TITLES 0x30
#define H_STRINGS 0x34
#define H_O_STRINGS 0x38
#define H_CHARS 0x3C
#define H_O_CHARS 0x40
#define H_HELP 0x44
#define H_AREFS 0x48
#define H_O_AREFS 0x4C
#define H_GRAPHS 0x50
#define H_O_GRAPHS 0x54

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
static void copy_name(char *dst, const uint8_t *src, unsigned n)
{
	memcpy(dst, src, n);
	dst[n - 1] = 0;
}

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

/* ---- event stream ------------------------------------------------------------------------------------------------ */
static int payload_size(unsigned type)
{
	switch (type) {
	case CINEDEF_EV_WAIT: return 0;
	case CINEDEF_EV_MODE: return 1;
	case CINEDEF_EV_FLAGS: return 2;
	case CINEDEF_EV_WEAPON: return 2;
	case CINEDEF_EV_THROTTLE: return 8;
	case CINEDEF_EV_AIM_SMALL: return 2;
	case CINEDEF_EV_AIM_LARGE: return 4;
	default: return -1;
	}
}

int cinedef_next_event(const uint8_t *stream, uint32_t bytes, uint32_t *pos, cinedef_event *ev)
{
	uint32_t p = *pos;
	unsigned op, type, kind;
	int pl;
	if (!stream || p >= bytes) return 0;
	op = stream[p++];
	type = op >> 2;
	kind = op & 3u;
	pl = payload_size(type);
	if (pl < 0) return 0;
	ev->wait = 0;
	if (kind == 1) {
		ev->wait = 1;
	} else if (kind == 2) {
		if (bytes - p < 1) return 0;
		ev->wait = stream[p++];
	} else if (kind == 3) {
		if (bytes - p < 2) return 0;
		ev->wait = rd16(stream + p);
		p += 2;
	}
	if (bytes - p < (uint32_t)pl) return 0;
	ev->type = (uint8_t)type;
	ev->kind = (uint8_t)kind;
	ev->payload = stream + p;
	*pos = p + (uint32_t)pl;
	return 1;
}

void cinedef_apply_event(cinedef_ctl *c, const cinedef_event *ev)
{
	int32_t y, p;
	switch (ev->type) {
	case CINEDEF_EV_MODE:
		c->mode = ev->payload[0];
		break;
	case CINEDEF_EV_FLAGS:
		c->flags = rd16(ev->payload);
		break;
	case CINEDEF_EV_WEAPON:
		c->weapon = rds16(ev->payload);
		break;
	case CINEDEF_EV_THROTTLE:
		c->throttle[0] = rdf(ev->payload);
		c->throttle[1] = rdf(ev->payload + 4);
		break;
	case CINEDEF_EV_AIM_SMALL:
	case CINEDEF_EV_AIM_LARGE:
		if (ev->type == CINEDEF_EV_AIM_SMALL) {
			y = (int8_t)ev->payload[0];
			p = (int8_t)ev->payload[1];
		} else {
			y = rds16(ev->payload);
			p = rds16(ev->payload + 2);
		}
		y += c->yaw;
		p += c->pitch;
		/* wrap into [-1000, 1000): +- 32768 + 1000 fits easily in int32 */
		y = (y + CINEDEF_YAW_HALF) % (2 * CINEDEF_YAW_HALF);
		if (y < 0) y += 2 * CINEDEF_YAW_HALF;
		c->yaw = (int16_t)(y - CINEDEF_YAW_HALF);
		if (p > CINEDEF_PITCH_LIMIT) p = CINEDEF_PITCH_LIMIT;
		if (p < -CINEDEF_PITCH_LIMIT) p = -CINEDEF_PITCH_LIMIT;
		c->pitch = (int16_t)p;
		break;
	default:
		break;
	}
}

/* walks a stream: 1 if it is exactly `length` ticks (1 + the sum of the waits) with `events` events and finite throttles */
static int stream_ok(const uint8_t *stream, uint32_t bytes, uint32_t length, uint32_t events)
{
	uint32_t pos = 0, n = 0;
	uint64_t total = 0;
	cinedef_event ev;
	while (pos < bytes) {
		if (!cinedef_next_event(stream, bytes, &pos, &ev)) return 0;
		if (ev.type == CINEDEF_EV_THROTTLE && (!finite_at(ev.payload) || !finite_at(ev.payload + 4))) return 0;
		total += ev.wait;
		if (++n > CINEDEF_MAX_EVENTS) return 0;
	}
	return n == events && total + 1 == length;
}

/* ---- open ---------------------------------------------------------------------------------------------------------- */
cinedef_status cinedef_open(cinedef_section *s, const uint8_t *data, size_t size)
{
	uint32_t i, prev = 0, nrec, ntrk, npt, ntit, nstr, nch, nar, ngr;
	uint32_t o_rec, o_pool, npool, o_trk, o_pt, o_tit, o_str, o_ch, o_ar, o_gr;

	memset(s, 0, sizeof *s);
	if (!data || size < HEADER_SIZE) return CINEDEF_ERR_TRUNCATED;
	if (memcmp(data, "CINE", 4) != 0) return CINEDEF_ERR_MAGIC;
	if (rd16(data + 4) != CINE_VERSION) return CINEDEF_ERR_VERSION;
	if (rd32(data + H_SIZE) > size) return CINEDEF_ERR_TRUNCATED;
	if (rd32(data + H_SIZE) != size) return CINEDEF_ERR_LAYOUT;
	nrec = rd32(data + H_RECS);
	o_rec = rd32(data + H_O_RECS);
	o_pool = rd32(data + H_O_POOL);
	npool = rd32(data + H_POOL);
	ntrk = rd32(data + H_TRACKS);
	o_trk = rd32(data + H_O_TRACKS);
	npt = rd32(data + H_POINTS);
	o_pt = rd32(data + H_O_POINTS);
	ntit = rd32(data + H_TITLES);
	o_tit = rd32(data + H_O_TITLES);
	nstr = rd32(data + H_STRINGS);
	o_str = rd32(data + H_O_STRINGS);
	nch = rd32(data + H_CHARS);
	o_ch = rd32(data + H_O_CHARS);
	nar = rd32(data + H_AREFS);
	o_ar = rd32(data + H_O_AREFS);
	ngr = rd32(data + H_GRAPHS);
	o_gr = rd32(data + H_O_GRAPHS);
	if (nrec > MAX_TABLE || ntrk > MAX_TABLE || npt > MAX_TABLE || ntit > MAX_TABLE || nstr > MAX_TABLE || nch > MAX_TABLE ||
	    nar > MAX_TABLE || ngr > MAX_TABLE)
		return CINEDEF_ERR_LAYOUT;
	if (!table_ok(o_rec, nrec, REC_SIZE, size) || !table_ok(o_pool, 1, npool, size) || !table_ok(o_trk, ntrk, TRACK_SIZE, size) ||
	    !table_ok(o_pt, npt, POINT_SIZE, size) || !table_ok(o_tit, ntit, TITLE_SIZE, size) || !table_ok(o_str, nstr, STR_SIZE, size) ||
	    !table_ok(o_ch, 1, (uint64_t)nch * 2 > 0xFFFFFFFFu ? 0xFFFFFFFFu : nch * 2u, size) || !table_ok(o_ar, nar, AREF_SIZE, size) ||
	    !table_ok(o_gr, ngr, 4, size))
		return CINEDEF_ERR_LAYOUT;

	s->data = data;
	s->size = size;
	s->rec_count = nrec;
	s->track_count = ntrk;
	s->point_count = npt;
	s->title_count = ntit;
	s->string_count = nstr;
	s->char_units = nch;
	s->anim_ref_count = nar;
	s->graph_count = ngr;
	s->pool_bytes = npool;
	s->help_tag = rd32(data + H_HELP);
	s->recs = data + o_rec;
	s->pool = data + o_pool;
	s->tracks = data + o_trk;
	s->points = data + o_pt;
	s->titles = data + o_tit;
	s->strtab = data + o_str;
	s->chars = data + o_ch;
	s->arefs = data + o_ar;
	s->graphs = data + o_gr;

#define FAIL do { memset(s, 0, sizeof *s); return CINEDEF_ERR_LAYOUT; } while (0)

	for (i = 0; i < nrec; i++) {
		const uint8_t *r = s->recs + (size_t)i * REC_SIZE;
		uint32_t soff = rd32(r + 0x20), sbytes = rd32(r + 0x24), length = rd32(r + 0x28);
		unsigned cver = r[0x2E];
		int16_t yaw = rds16(r + 0x44), pitch = rds16(r + 0x46);
		if (r[CINEDEF_NAME_LEN - 1] != 0) FAIL;
		if (soff % 4u || sbytes == 0 || soff > npool || sbytes > npool - soff) FAIL;
		if (length == 0 || length > CINEDEF_MAX_LENGTH) FAIL;
		if (cver != 0 && cver != 3 && cver != 4) FAIL;
		if (yaw < -CINEDEF_YAW_HALF || yaw > CINEDEF_YAW_HALF || pitch < -CINEDEF_PITCH_LIMIT || pitch > CINEDEF_PITCH_LIMIT) FAIL;
		if (!finite_at(r + 0x3C) || !finite_at(r + 0x40)) FAIL;
		if (!stream_ok(s->pool + soff, sbytes, length, rd32(r + 0x48))) FAIL;
	}
	prev = 0;
	for (i = 0; i < ntrk; i++) {
		const uint8_t *t = s->tracks + (size_t)i * TRACK_SIZE;
		uint32_t tag = rd32(t), first = rd32(t + 8), cnt = rd32(t + 12), k;
		if (i && tag <= prev) FAIL;
		prev = tag;
		if (cnt < 2 || first > npt || cnt > npt - first) FAIL;
		if (t[TRACK_SIZE - 1] != 0) FAIL;
		for (k = 0; k < cnt; k++) {
			const uint8_t *pt = s->points + (size_t)(first + k) * POINT_SIZE;
			unsigned c;
			for (c = 0; c < 7; c++)
				if (!finite_at(pt + 4 * c)) FAIL;
		}
	}
	for (i = 0; i < nstr; i++) {
		const uint8_t *e = s->strtab + (size_t)i * STR_SIZE;
		uint32_t first = rd32(e), units = rd32(e + 4);
		if (first > nch || units >= nch - first) FAIL;                       /* room for the NUL */
		if (rd16(s->chars + 2 * (size_t)(first + units)) != 0) FAIL;
	}
	for (i = 0; i < ntit; i++) {
		const uint8_t *t = s->titles + (size_t)i * TITLE_SIZE;
		uint32_t str = rd16(t + 0x28);
		if (t[CINEDEF_NAME_LEN - 1] != 0) FAIL;
		if (str != CINEDEF_NONE16 && str >= nstr) FAIL;
	}
	for (i = 0; i < nar; i++)
		if (s->arefs[(size_t)i * AREF_SIZE + CINEDEF_NAME_LEN - 1] != 0) FAIL;
	for (i = 0; i < ngr; i++) {
		uint32_t g = rd32(s->graphs + 4 * (size_t)i);
		if (i && g <= prev) FAIL;
		prev = g;
	}
#undef FAIL
	return CINEDEF_OK;
}

/* ---- accessors --------------------------------------------------------------------------------------------------- */
cinedef_rec cinedef_get_rec(const cinedef_section *s, uint32_t i)
{
	cinedef_rec out;
	const uint8_t *r;
	memset(&out, 0, sizeof out);
	if (!s->data || i >= s->rec_count) return out;
	r = s->recs + (size_t)i * REC_SIZE;
	copy_name(out.name, r, CINEDEF_NAME_LEN);
	out.stream_bytes = rd32(r + 0x24);
	out.length = rd32(r + 0x28);
	out.version = r[0x2C];
	out.raw = r[0x2D];
	out.ctl_version = r[0x2E];
	out.init.flags = rd16(r + 0x30);
	out.init.weapon = rds16(r + 0x32);
	out.init.extra_a = rds16(r + 0x34);
	out.init.extra_b = rds16(r + 0x36);
	out.init.mode = r[0x38];
	out.init.aux = r[0x39];
	out.init.word2 = rd16(r + 0x3A);
	out.init.throttle[0] = rdf(r + 0x3C);
	out.init.throttle[1] = rdf(r + 0x40);
	out.init.yaw = rds16(r + 0x44);
	out.init.pitch = rds16(r + 0x46);
	out.events = rd32(r + 0x48);
	return out;
}

int cinedef_find_rec(const cinedef_section *s, const char *name)
{
	uint32_t i;
	if (!s->data || !name) return -1;
	for (i = 0; i < s->rec_count; i++)
		if (strncmp((const char *)(s->recs + (size_t)i * REC_SIZE), name, CINEDEF_NAME_LEN) == 0) return (int)i;
	return -1;
}

const uint8_t *cinedef_rec_stream(const cinedef_section *s, uint32_t i, uint32_t *bytes)
{
	const uint8_t *r;
	if (!s->data || i >= s->rec_count) {
		if (bytes) *bytes = 0;
		return NULL;
	}
	r = s->recs + (size_t)i * REC_SIZE;
	if (bytes) *bytes = rd32(r + 0x24);
	return s->pool + rd32(r + 0x20);
}

cinedef_track cinedef_get_track(const cinedef_section *s, uint32_t i)
{
	cinedef_track out;
	const uint8_t *t;
	memset(&out, 0, sizeof out);
	if (!s->data || i >= s->track_count) return out;
	t = s->tracks + (size_t)i * TRACK_SIZE;
	out.tag_id = rd32(t);
	out.flags = rd32(t + 4);
	out.first_point = rd32(t + 8);
	out.point_count = rd32(t + 12);
	copy_name(out.name, t + 16, CINEDEF_TRACK_NAME_LEN);
	return out;
}

int cinedef_find_track(const cinedef_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->data ? s->track_count : 0;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, v = rd32(s->tracks + (size_t)mid * TRACK_SIZE);
		if (v == tag_id) return (int)mid;
		if (v < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

cinedef_point cinedef_get_point(const cinedef_section *s, uint32_t track, uint32_t k)
{
	cinedef_point out;
	const uint8_t *t, *p;
	unsigned c;
	memset(&out, 0, sizeof out);
	if (!s->data || track >= s->track_count) return out;
	t = s->tracks + (size_t)track * TRACK_SIZE;
	if (k >= rd32(t + 12)) return out;
	p = s->points + (size_t)(rd32(t + 8) + k) * POINT_SIZE;
	for (c = 0; c < 3; c++) out.pos[c] = rdf(p + 4 * c);
	for (c = 0; c < 4; c++) out.quat[c] = rdf(p + 12 + 4 * c);
	return out;
}

cinedef_title cinedef_get_title(const cinedef_section *s, uint32_t i)
{
	cinedef_title out;
	const uint8_t *t;
	memset(&out, 0, sizeof out);
	out.string = CINEDEF_NONE16;
	if (!s->data || i >= s->title_count) return out;
	t = s->titles + (size_t)i * TITLE_SIZE;
	copy_name(out.name, t, CINEDEF_NAME_LEN);
	out.top = rds16(t + 0x20);
	out.left = rds16(t + 0x22);
	out.bottom = rds16(t + 0x24);
	out.right = rds16(t + 0x26);
	out.string = rd16(t + 0x28);
	out.style = rd16(t + 0x2A);
	out.justify = rd16(t + 0x2C);
	out.colour = rd32(t + 0x30);
	out.shadow = rd32(t + 0x34);
	out.fade_in = rd16(t + 0x38);
	out.up = rd16(t + 0x3A);
	out.fade_out = rd16(t + 0x3C);
	return out;
}

int cinedef_find_title(const cinedef_section *s, const char *name)
{
	uint32_t i;
	if (!s->data || !name) return -1;
	for (i = 0; i < s->title_count; i++)
		if (strncmp((const char *)(s->titles + (size_t)i * TITLE_SIZE), name, CINEDEF_NAME_LEN) == 0) return (int)i;
	return -1;
}

uint32_t cinedef_get_string(const cinedef_section *s, uint32_t index, uint16_t *dst, uint32_t cap)
{
	const uint8_t *e;
	uint32_t first, units, i, n;
	if (!s->data || index >= s->string_count) return 0xFFFFFFFFu;
	e = s->strtab + (size_t)index * STR_SIZE;
	first = rd32(e);
	units = rd32(e + 4);
	if (dst && cap) {
		n = units < cap ? units : cap;
		for (i = 0; i < n; i++) dst[i] = rd16(s->chars + 2 * (size_t)(first + i));
		if (n < cap) dst[n] = 0;
	}
	return units;
}

cinedef_anim_ref cinedef_get_anim_ref(const cinedef_section *s, uint32_t i)
{
	cinedef_anim_ref out;
	const uint8_t *a;
	memset(&out, 0, sizeof out);
	if (!s->data || i >= s->anim_ref_count) return out;
	a = s->arefs + (size_t)i * AREF_SIZE;
	copy_name(out.name, a, CINEDEF_NAME_LEN);
	out.graph_tag = rd32(a + 0x20);
	out.resolved = rd32(a + 0x24);
	return out;
}

uint32_t cinedef_get_graph(const cinedef_section *s, uint32_t i)
{
	if (!s->data || i >= s->graph_count) return 0;
	return rd32(s->graphs + 4 * (size_t)i);
}

int cinedef_has_graph(const cinedef_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->data ? s->graph_count : 0;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, v = rd32(s->graphs + 4 * (size_t)mid);
		if (v == tag_id) return 1;
		if (v < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return 0;
}
