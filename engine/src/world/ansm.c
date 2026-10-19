/* ASTB section reader (docs/19-ansm-section.md, contract in world/ansm.h). Pure C11, no heap, no double.
 * ansm_open validates the whole section once: header, graph order, every table reference against the section size and
 * the first byte of the body, every string (inside the section, NUL within ANSM_NAME_MAX), every animation record
 * (variant links, sound index, frame fields, name index sorted and a permutation) and every slot list entry. The
 * accessors then only index with the counts they were given. */
#include "world/ansm.h"
#include <string.h>

#define HEADER_SIZE 64u
#define GRAPH_REC 96u
#define ANIM_REC 32u
#define UNIT_REC 64u
#define CLASS_REC 64u
#define TYPE_REC 16u
#define IK_REC 8u
#define VEH_REC 32u
#define SUSP_REC 16u
#define LIST_REC 16u
#define OBJ_REC 8u
#define NONE16 0xFFFFu
#define MAX_GRAPHS (1u << 16)

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static int      rds16(const uint8_t *p) { return (int16_t)(uint16_t)rd16(p); }
static float    rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* graph record: refs (offset, count) at these offsets */
enum { G_TAG = 0, G_ANIMS = 8, G_NIDX = 16, G_OBJECTS = 24, G_UNITS = 32, G_VEHICLES = 40, G_DEVICES = 48, G_WOBJS = 56,
       G_FPS = 64, G_DAMAGE = 72, G_SOUNDS = 80 };
/* animation record */
enum { A_NAME = 0, A_FRAMES = 4, A_TICKS = 6, A_FLAGS = 8, A_TYPE = 10, A_FPS = 11, A_NEXT = 12, A_MAIN = 14, A_LOOP = 16,
       A_KEY = 18, A_KEY2 = 20, A_SOUND = 22, A_SOUND_FRAME = 24, A_FOOT_L = 26, A_FOOT_R = 27, A_WEIGHT = 28 };
/* unit, class, type, vehicle records */
enum { U_NAME = 0, U_CLASSES = 4, U_SLOTS = 12, U_IK = 20, U_AIM = 28 };
enum { C_NAME = 0, C_GRIP = 4, C_HOLD = 8, C_SLOTS = 12, C_IK = 20, C_TYPES = 28, C_AIM = 36 };
enum { T_LABEL = 0, T_SLOTS = 4 };
enum { V_YAW_R = 0, V_YAW_L = 4, V_RC = 8, V_LC = 10, V_SLOTS = 12, V_SUSP = 20 };

/* ---- slot name tables (built from the X-macros of ansm.h) ------------------------------------------------------------- */
#define X_NAME(n, s, str) [s] = str,
static const char *const type_names[] = { ANSM_TYPE_SLOTS(X_NAME) };
static const char *const class_names[] = { ANSM_CLASS_SLOTS(X_NAME) };
static const char *const unit_names[] = { ANSM_UNIT_SLOTS(X_NAME) };
static const char *const fp_names[] = { ANSM_FP_SLOTS(X_NAME) };
static const char *const wobj_names[] = { ANSM_WOBJ_SLOTS(X_NAME) };
static const char *const vehicle_names[] = { ANSM_VEHICLE_SLOTS(X_NAME) };
static const char *const device_names[] = { ANSM_DEVICE_SLOTS(X_NAME) };
#undef X_NAME

#define N(a) (sizeof(a) / sizeof((a)[0]))
static const struct { const char *const *names; unsigned count; } LEVELS[ANSM_LEVELS] = {
	{ type_names, (unsigned)N(type_names) }, { class_names, (unsigned)N(class_names) },
	{ unit_names, (unsigned)N(unit_names) }, { fp_names, (unsigned)N(fp_names) },
	{ wobj_names, (unsigned)N(wobj_names) }, { vehicle_names, (unsigned)N(vehicle_names) },
	{ device_names, (unsigned)N(device_names) },
};

unsigned ansm_slot_count(unsigned level) { return level < ANSM_LEVELS ? LEVELS[level].count : 0u; }

const char *ansm_slot_name(unsigned level, unsigned slot)
{
	if (level >= ANSM_LEVELS || slot >= LEVELS[level].count) return NULL;
	return LEVELS[level].names[slot] ? LEVELS[level].names[slot] : "";
}

int ansm_slot_find(unsigned level, const char *name)
{
	unsigned i;
	if (level >= ANSM_LEVELS || !name || !name[0]) return -1;
	for (i = 0; i < LEVELS[level].count; i++)
		if (LEVELS[level].names[i] && strcmp(LEVELS[level].names[i], name) == 0) return (int)i;
	return -1;
}

/* ---- validation ----------------------------------------------------------------------------------------------------------- */
static int ref_ok(const ansm_section *s, uint32_t off, uint32_t count, uint32_t rec, uint32_t align)
{
	if (count == 0) return 1;
	if (off < s->body || off % align != 0 || off > s->size) return 0;
	return count <= (s->size - off) / rec;
}

static int str_ok(const ansm_section *s, uint32_t off)
{
	uint32_t i;
	if (off == 0) return 1;
	if (off < s->body || off >= s->size) return 0;
	for (i = 0; i < ANSM_NAME_MAX && off + i < s->size; i++)
		if (s->data[off + i] == 0) return 1;
	return 0;
}

static const char *str(const ansm_section *s, uint32_t off) { return off ? (const char *)(s->data + off) : ""; }

static int slots_ok(const ansm_section *s, uint32_t off, uint32_t count, uint32_t na)
{
	uint32_t i;
	if (!ref_ok(s, off, count, 2, 2)) return 0;
	for (i = 0; i < count; i++) {
		uint32_t v = rd16(s->data + off + 2u * i);
		if (v != NONE16 && v >= na) return 0;
	}
	return 1;
}

static int ik_ok(const ansm_section *s, uint32_t off, uint32_t count)
{
	uint32_t i;
	if (!ref_ok(s, off, count, IK_REC, 4)) return 0;
	for (i = 0; i < count; i++)
		if (!str_ok(s, rd32(s->data + off + IK_REC * i)) || !str_ok(s, rd32(s->data + off + IK_REC * i + 4))) return 0;
	return 1;
}

static void graph_load(const ansm_section *s, uint32_t gi, ansm_graph *g)
{
	const uint8_t *r = s->data + HEADER_SIZE + (size_t)GRAPH_REC * gi;
	g->tag_id = rd32(r + G_TAG);
	g->o_anims = rd32(r + G_ANIMS);       g->anim_count = rd32(r + G_ANIMS + 4);
	g->o_nidx = rd32(r + G_NIDX);
	g->o_objects = rd32(r + G_OBJECTS);   g->object_count = rd32(r + G_OBJECTS + 4);
	g->o_units = rd32(r + G_UNITS);       g->unit_count = rd32(r + G_UNITS + 4);
	g->o_vehicles = rd32(r + G_VEHICLES); g->vehicle_count = rd32(r + G_VEHICLES + 4);
	g->o_devices = rd32(r + G_DEVICES);   g->device_count = rd32(r + G_DEVICES + 4);
	g->o_wobjs = rd32(r + G_WOBJS);       g->wobj_count = rd32(r + G_WOBJS + 4);
	g->o_fps = rd32(r + G_FPS);           g->fp_count = rd32(r + G_FPS + 4);
	g->o_damage = rd32(r + G_DAMAGE);     g->damage_count = rd32(r + G_DAMAGE + 4);
	g->o_sounds = rd32(r + G_SOUNDS);     g->sound_count = rd32(r + G_SOUNDS + 4);
}

static int lists_ok(const ansm_section *s, uint32_t off, uint32_t count, uint32_t na)
{
	uint32_t i;
	if (!ref_ok(s, off, count, LIST_REC, 4)) return 0;
	for (i = 0; i < count; i++) {
		const uint8_t *r = s->data + off + LIST_REC * i;
		if (!slots_ok(s, rd32(r), rd32(r + 4), na)) return 0;
	}
	return 1;
}

static int graph_ok(const ansm_section *s, const ansm_graph *g)
{
	uint32_t na = g->anim_count, i, j, k;
	if (na > 0xFFFFu) return 0;
	if (!ref_ok(s, g->o_anims, na, ANIM_REC, 4) || !ref_ok(s, g->o_nidx, na, 2, 2) ||
	    !ref_ok(s, g->o_objects, g->object_count, OBJ_REC, 4) || !ref_ok(s, g->o_units, g->unit_count, UNIT_REC, 4) ||
	    !ref_ok(s, g->o_vehicles, g->vehicle_count, VEH_REC, 4) || !ref_ok(s, g->o_damage, g->damage_count, 2, 2) ||
	    !ref_ok(s, g->o_sounds, g->sound_count, 4, 4))
		return 0;
	for (i = 0; i < na; i++) {
		const uint8_t *a = s->data + g->o_anims + ANIM_REC * i;
		int next = rds16(a + A_NEXT), snd = rds16(a + A_SOUND);
		uint32_t main = rd16(a + A_MAIN), fc = rd16(a + A_FRAMES);
		if (!str_ok(s, rd32(a + A_NAME))) return 0;
		if (next < -1 || (next >= 0 && (uint32_t)next >= na) || main >= na) return 0;
		if (snd < -1 || (snd >= 0 && (uint32_t)snd >= g->sound_count)) return 0;
		if (fc < 1 || a[A_TYPE] > 2 || (rd16(a + A_FLAGS) & ~3u) || rd16(a + A_LOOP) >= fc || rds16(a + A_SOUND_FRAME) >= (int)fc) return 0;
	}
	for (i = 0; i < na; i++) {                       /* name index: in range, strictly ascending by (name, index) */
		uint32_t k1 = rd16(s->data + g->o_nidx + 2u * i);
		if (k1 >= na) return 0;
		if (i > 0) {
			uint32_t k0 = rd16(s->data + g->o_nidx + 2u * (i - 1));
			int c = strcmp(str(s, rd32(s->data + g->o_anims + ANIM_REC * k0)), str(s, rd32(s->data + g->o_anims + ANIM_REC * k1)));
			if (c > 0 || (c == 0 && k0 >= k1)) return 0;
		}
	}
	for (i = 0; i < g->object_count; i++) {
		int an = rds16(s->data + g->o_objects + OBJ_REC * i);
		if (an < -1 || (an >= 0 && (uint32_t)an >= na)) return 0;
	}
	for (i = 0; i < g->unit_count; i++) {
		const uint8_t *u = s->data + g->o_units + UNIT_REC * i;
		uint32_t cn = rd32(u + U_CLASSES + 4), co = rd32(u + U_CLASSES);
		if (!str_ok(s, rd32(u + U_NAME)) || !slots_ok(s, rd32(u + U_SLOTS), rd32(u + U_SLOTS + 4), na) ||
		    !ik_ok(s, rd32(u + U_IK), rd32(u + U_IK + 4)) || !ref_ok(s, co, cn, CLASS_REC, 4))
			return 0;
		for (j = 0; j < cn; j++) {
			const uint8_t *c = s->data + co + CLASS_REC * j;
			uint32_t tn = rd32(c + C_TYPES + 4), to = rd32(c + C_TYPES);
			if (!str_ok(s, rd32(c + C_NAME)) || !str_ok(s, rd32(c + C_GRIP)) || !str_ok(s, rd32(c + C_HOLD)) ||
			    !slots_ok(s, rd32(c + C_SLOTS), rd32(c + C_SLOTS + 4), na) || !ik_ok(s, rd32(c + C_IK), rd32(c + C_IK + 4)) ||
			    !ref_ok(s, to, tn, TYPE_REC, 4))
				return 0;
			for (k = 0; k < tn; k++) {
				const uint8_t *t = s->data + to + TYPE_REC * k;
				if (!str_ok(s, rd32(t + T_LABEL)) || !slots_ok(s, rd32(t + T_SLOTS), rd32(t + T_SLOTS + 4), na)) return 0;
			}
		}
	}
	for (i = 0; i < g->vehicle_count; i++) {
		const uint8_t *v = s->data + g->o_vehicles + VEH_REC * i;
		uint32_t sn = rd32(v + V_SUSP + 4), so = rd32(v + V_SUSP);
		if (!slots_ok(s, rd32(v + V_SLOTS), rd32(v + V_SLOTS + 4), na) || !ref_ok(s, so, sn, SUSP_REC, 4)) return 0;
		for (j = 0; j < sn; j++) {
			int an = rds16(s->data + so + SUSP_REC * j + 2);
			if (an < -1 || (an >= 0 && (uint32_t)an >= na)) return 0;
		}
	}
	if (!lists_ok(s, g->o_devices, g->device_count, na) || !lists_ok(s, g->o_wobjs, g->wobj_count, na) ||
	    !lists_ok(s, g->o_fps, g->fp_count, na))
		return 0;
	for (i = 0; i < g->damage_count; i++) {
		uint32_t v = rd16(s->data + g->o_damage + 2u * i);
		if (v != NONE16 && v >= na) return 0;
	}
	return 1;
}

ansm_status ansm_open(ansm_section *out, const uint8_t *data, size_t size)
{
	ansm_section s;
	uint32_t total, graphs_off, gi, anims = 0, prev = 0;
	memset(out, 0, sizeof *out);                      /* a failed open leaves an empty section */
	if (!data || size < HEADER_SIZE) return ANSM_ERR_TRUNCATED;
	if (memcmp(data, "ASTB", 4) != 0) return ANSM_ERR_MAGIC;
	if (rd16(data + 4) != 1u) return ANSM_ERR_VERSION;
	s.data = data;
	s.size = size;
	s.graph_count = rd32(data + 8);
	graphs_off = rd32(data + 12);
	s.body = rd32(data + 16);
	total = rd32(data + 20);
	s.anim_total = rd32(data + 24);
	if (total > size) return ANSM_ERR_TRUNCATED;
	if (total != size || graphs_off != HEADER_SIZE || s.graph_count > MAX_GRAPHS) return ANSM_ERR_LAYOUT;
	if (s.body < HEADER_SIZE + GRAPH_REC * s.graph_count || s.body > size || s.body % 4u) return ANSM_ERR_LAYOUT;
	for (gi = 0; gi < s.graph_count; gi++) {
		ansm_graph g;
		graph_load(&s, gi, &g);
		if (gi > 0 && g.tag_id <= prev) return ANSM_ERR_LAYOUT;
		prev = g.tag_id;
		if (!graph_ok(&s, &g)) return ANSM_ERR_LAYOUT;
		anims += g.anim_count;
	}
	if (anims != s.anim_total) return ANSM_ERR_LAYOUT;
	*out = s;
	return ANSM_OK;
}

int ansm_get_graph(const ansm_section *s, uint32_t index, ansm_graph *out)
{
	if (index >= s->graph_count) return -1;
	graph_load(s, index, out);
	return 0;
}

int ansm_find_graph(const ansm_section *s, uint32_t tag_id, ansm_graph *out)
{
	uint32_t lo = 0, hi = s->graph_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, t = rd32(s->data + HEADER_SIZE + (size_t)GRAPH_REC * mid);
		if (t < tag_id) lo = mid + 1; else hi = mid;
	}
	if (lo >= s->graph_count || rd32(s->data + HEADER_SIZE + (size_t)GRAPH_REC * lo) != tag_id) return -1;
	graph_load(s, lo, out);
	return 0;
}

/* ---- animations ---------------------------------------------------------------------------------------------------------- */
static const uint8_t *anim_rec(const ansm_section *s, const ansm_graph *g, uint32_t k)
{
	return k < g->anim_count ? s->data + g->o_anims + ANIM_REC * k : NULL;
}

static const char *anim_name(const ansm_section *s, const ansm_graph *g, uint32_t k)
{
	return str(s, rd32(s->data + g->o_anims + ANIM_REC * k + A_NAME));
}

uint32_t ansm_sound_tag(const ansm_section *s, const ansm_graph *g, uint32_t index)
{
	return index < g->sound_count ? rd32(s->data + g->o_sounds + 4u * index) : 0u;
}

int ansm_anim_info(const ansm_section *s, const ansm_graph *g, uint32_t k, ansm_anim *out)
{
	const uint8_t *a = anim_rec(s, g, k);
	if (!a) return -1;
	out->name = str(s, rd32(a + A_NAME));
	out->frame_count = (uint16_t)rd16(a + A_FRAMES);
	out->ticks = (uint16_t)rd16(a + A_TICKS);
	out->flags = (uint16_t)rd16(a + A_FLAGS);
	out->loop_frame = (uint16_t)rd16(a + A_LOOP);
	out->type = a[A_TYPE];
	out->fps = a[A_FPS];
	out->next = (int16_t)rds16(a + A_NEXT);
	out->main = (int16_t)rds16(a + A_MAIN);
	out->key_frame[0] = (int16_t)rds16(a + A_KEY);
	out->key_frame[1] = (int16_t)rds16(a + A_KEY2);
	out->sound = (int16_t)rds16(a + A_SOUND);
	out->sound_frame = (int16_t)rds16(a + A_SOUND_FRAME);
	out->sound_tag = out->sound >= 0 ? ansm_sound_tag(s, g, (uint32_t)out->sound) : 0u;
	out->foot_left = a[A_FOOT_L];
	out->foot_right = a[A_FOOT_R];
	out->weight = (float)rd16(a + A_WEIGHT) / 65535.0f;
	return 0;
}

int ansm_find_anim(const ansm_section *s, const ansm_graph *g, const char *name)
{
	uint32_t lo = 0, hi = g->anim_count;
	size_t len;
	if (!name) return -1;
	len = strlen(name);
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, k = rd16(s->data + g->o_nidx + 2u * mid);
		if (strcmp(anim_name(s, g, k), name) < 0) lo = mid + 1; else hi = mid;
	}
	for (; lo < g->anim_count; lo++) {
		uint32_t k = rd16(s->data + g->o_nidx + 2u * lo);
		const char *found = anim_name(s, g, k);
		if (strncmp(found, name, len) != 0) break;
		if (found[len] == 0) return (int)k;
		if (found[len] == '%') return (int)rd16(s->data + g->o_anims + ANIM_REC * k + A_MAIN);
	}
	return -1;
}

int ansm_pick_variant(const ansm_section *s, const ansm_graph *g, uint32_t k, float r)
{
	const uint8_t *a = anim_rec(s, g, k);
	uint32_t cur, n;
	if (!a) return -1;
	cur = rd16(a + A_MAIN);
	for (n = 0; n < g->anim_count; n++) {
		const uint8_t *c = s->data + g->o_anims + ANIM_REC * cur;
		int next = rds16(c + A_NEXT);
		if (next < 0 || r < (float)rd16(c + A_WEIGHT) / 65535.0f) return (int)cur;
		cur = (uint32_t)next;
	}
	return (int)cur;
}

unsigned ansm_anim_events(const ansm_section *s, const ansm_graph *g, uint32_t k, ansm_event out[ANSM_EVENTS_MAX])
{
	const uint8_t *a = anim_rec(s, g, k);
	ansm_event ev[ANSM_EVENTS_MAX];
	unsigned n = 0, i, j;
	int snd, key0, key1;
	if (!a) return 0;
	snd = rds16(a + A_SOUND);
	if (snd >= 0) { ev[n].frame = (uint16_t)rds16(a + A_SOUND_FRAME); ev[n].kind = ANSM_EV_SOUND; ev[n].arg = ansm_sound_tag(s, g, (uint32_t)snd); n++; }
	key0 = rds16(a + A_KEY);
	key1 = rds16(a + A_KEY2);
	if (key0 > 0) { ev[n].frame = (uint16_t)key0; ev[n].kind = ANSM_EV_KEY; ev[n].arg = 0; n++; }
	if (key1 > 0) { ev[n].frame = (uint16_t)key1; ev[n].kind = ANSM_EV_KEY; ev[n].arg = 1; n++; }
	if (a[A_FOOT_L]) { ev[n].frame = a[A_FOOT_L]; ev[n].kind = ANSM_EV_FOOT_LEFT; ev[n].arg = 0; n++; }
	if (a[A_FOOT_R]) { ev[n].frame = a[A_FOOT_R]; ev[n].kind = ANSM_EV_FOOT_RIGHT; ev[n].arg = 0; n++; }
	for (i = 1; i < n; i++) {                           /* insertion sort by (frame, kind): at most five entries */
		ansm_event e = ev[i];
		for (j = i; j > 0 && (ev[j - 1].frame > e.frame || (ev[j - 1].frame == e.frame && ev[j - 1].kind > e.kind)); j--) ev[j] = ev[j - 1];
		ev[j] = e;
	}
	for (i = 0; i < n; i++) out[i] = ev[i];
	return n;
}

/* ---- aiming, slot lists ------------------------------------------------------------------------------------------------ */
static void aim_read(const uint8_t *p, ansm_aim *a)
{
	a->yaw_right_per_frame = rdf(p);
	a->yaw_left_per_frame = rdf(p + 4);
	a->yaw_right_frames = (uint16_t)rd16(p + 8);
	a->yaw_left_frames = (uint16_t)rd16(p + 10);
	a->pitch_down_per_frame = rdf(p + 12);
	a->pitch_up_per_frame = rdf(p + 16);
	a->pitch_down_frames = (uint16_t)rd16(p + 20);
	a->pitch_up_frames = (uint16_t)rd16(p + 22);
}

void ansm_aim_ranges(const ansm_aim *a, float out[4])
{
	out[0] = a->yaw_right_per_frame * (float)a->yaw_right_frames;
	out[1] = a->yaw_left_per_frame * (float)a->yaw_left_frames;
	out[2] = a->pitch_down_per_frame * (float)a->pitch_down_frames;
	out[3] = a->pitch_up_per_frame * (float)a->pitch_up_frames;
}

/* A slot list given as a (offset, count) pair at `ref`. */
static int list_slot(const ansm_section *s, const uint8_t *ref, unsigned slot)
{
	uint32_t off = rd32(ref), count = rd32(ref + 4);
	uint32_t v;
	if (slot >= count) return -1;
	v = rd16(s->data + off + 2u * slot);
	return v == NONE16 ? -1 : (int)v;
}

/* ---- units, classes, types ------------------------------------------------------------------------------------------ */
static const uint8_t *unit_rec(const ansm_section *s, const ansm_graph *g, uint32_t u)
{
	return u < g->unit_count ? s->data + g->o_units + UNIT_REC * u : NULL;
}

static const uint8_t *class_rec(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c)
{
	const uint8_t *ur = unit_rec(s, g, u);
	return ur && c < rd32(ur + U_CLASSES + 4) ? s->data + rd32(ur + U_CLASSES) + CLASS_REC * c : NULL;
}

static const uint8_t *type_rec(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t t)
{
	const uint8_t *cr = class_rec(s, g, u, c);
	return cr && t < rd32(cr + C_TYPES + 4) ? s->data + rd32(cr + C_TYPES) + TYPE_REC * t : NULL;
}

static int ik_get(const ansm_section *s, const uint8_t *ref, uint32_t i, ansm_ik *out)
{
	if (i >= rd32(ref + 4)) return -1;
	out->marker = str(s, rd32(s->data + rd32(ref) + IK_REC * i));
	out->attach = str(s, rd32(s->data + rd32(ref) + IK_REC * i + 4));
	return 0;
}

int ansm_unit_info(const ansm_section *s, const ansm_graph *g, uint32_t u, ansm_unit *out)
{
	const uint8_t *r = unit_rec(s, g, u);
	if (!r) return -1;
	out->name = str(s, rd32(r + U_NAME));
	aim_read(r + U_AIM, &out->aim);
	out->class_count = rd32(r + U_CLASSES + 4);
	out->slot_count = rd32(r + U_SLOTS + 4);
	out->ik_count = rd32(r + U_IK + 4);
	return 0;
}

int ansm_find_unit(const ansm_section *s, const ansm_graph *g, const char *label)
{
	uint32_t u;
	if (!label) return -1;
	for (u = 0; u < g->unit_count; u++)
		if (strcmp(str(s, rd32(s->data + g->o_units + UNIT_REC * u + U_NAME)), label) == 0) return (int)u;
	return -1;
}

int ansm_unit_slot(const ansm_section *s, const ansm_graph *g, uint32_t u, unsigned slot)
{
	const uint8_t *r = unit_rec(s, g, u);
	return r ? list_slot(s, r + U_SLOTS, slot) : -1;
}

int ansm_unit_ik(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t i, ansm_ik *out)
{
	const uint8_t *r = unit_rec(s, g, u);
	return r ? ik_get(s, r + U_IK, i, out) : -1;
}

int ansm_class_info(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, ansm_class *out)
{
	const uint8_t *r = class_rec(s, g, u, c);
	if (!r) return -1;
	out->name = str(s, rd32(r + C_NAME));
	out->grip = str(s, rd32(r + C_GRIP));
	out->hold = str(s, rd32(r + C_HOLD));
	aim_read(r + C_AIM, &out->aim);
	out->type_count = rd32(r + C_TYPES + 4);
	out->slot_count = rd32(r + C_SLOTS + 4);
	out->ik_count = rd32(r + C_IK + 4);
	return 0;
}

int ansm_find_class(const ansm_section *s, const ansm_graph *g, uint32_t u, const char *name)
{
	const uint8_t *ur = unit_rec(s, g, u);
	uint32_t c;
	if (!ur || !name) return -1;
	for (c = 0; c < rd32(ur + U_CLASSES + 4); c++)
		if (strcmp(str(s, rd32(s->data + rd32(ur + U_CLASSES) + CLASS_REC * c + C_NAME)), name) == 0) return (int)c;
	return -1;
}

int ansm_class_slot(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, unsigned slot)
{
	const uint8_t *r = class_rec(s, g, u, c);
	return r ? list_slot(s, r + C_SLOTS, slot) : -1;
}

int ansm_class_ik(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t i, ansm_ik *out)
{
	const uint8_t *r = class_rec(s, g, u, c);
	return r ? ik_get(s, r + C_IK, i, out) : -1;
}

int ansm_type_info(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t t, ansm_type *out)
{
	const uint8_t *r = type_rec(s, g, u, c, t);
	if (!r) return -1;
	out->label = str(s, rd32(r + T_LABEL));
	out->slot_count = rd32(r + T_SLOTS + 4);
	return 0;
}

int ansm_find_type(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, const char *label)
{
	const uint8_t *cr = class_rec(s, g, u, c);
	uint32_t t;
	if (!cr || !label) return -1;
	for (t = 0; t < rd32(cr + C_TYPES + 4); t++)
		if (strcmp(str(s, rd32(s->data + rd32(cr + C_TYPES) + TYPE_REC * t + T_LABEL)), label) == 0) return (int)t;
	return -1;
}

int ansm_type_slot(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t t, unsigned slot)
{
	const uint8_t *r = type_rec(s, g, u, c, t);
	return r ? list_slot(s, r + T_SLOTS, slot) : -1;
}

static unsigned used_slots(const ansm_section *s, const uint8_t *ref)
{
	uint32_t i, n = 0;
	for (i = 0; i < rd32(ref + 4); i++)
		if (rd16(s->data + rd32(ref) + 2u * i) != NONE16) n++;
	return n;
}

int ansm_find_class_for_type(const ansm_section *s, const ansm_graph *g, uint32_t u, const char *label)
{
	const uint8_t *ur = unit_rec(s, g, u);
	uint32_t c, t, score = 0;
	int best = -1;
	if (!ur || !label) return -1;
	for (c = 0; c < rd32(ur + U_CLASSES + 4); c++) {
		const uint8_t *cr = s->data + rd32(ur + U_CLASSES) + CLASS_REC * c;
		for (t = 0; t < rd32(cr + C_TYPES + 4); t++) {
			const uint8_t *tr = s->data + rd32(cr + C_TYPES) + TYPE_REC * t;
			uint32_t n;
			if (strcmp(str(s, rd32(tr + T_LABEL)), label) != 0) continue;
			n = used_slots(s, tr + T_SLOTS);
			if (n > score) { score = n; best = (int)c; }
		}
	}
	return best;
}

int ansm_lookup(const ansm_section *s, const ansm_graph *g, const char *unit, const char *cls, const char *type,
                unsigned action)
{
	unsigned level = ANSM_ACTION_LEVEL(action), slot = ANSM_ACTION_SLOT(action);
	int u, c, t;
	switch (level) {
	case ANSM_L_FP: return ansm_fp_slot(s, g, 0, slot);
	case ANSM_L_WOBJ: return ansm_wobj_slot(s, g, 0, slot);
	case ANSM_L_VEHICLE: return ansm_vehicle_slot(s, g, 0, slot);
	case ANSM_L_DEVICE: return ansm_device_slot(s, g, 0, slot);
	case ANSM_L_UNIT: case ANSM_L_CLASS: case ANSM_L_TYPE: break;
	default: return -1;
	}
	u = ansm_find_unit(s, g, unit);
	if (u < 0) return -1;
	if (level == ANSM_L_UNIT) return ansm_unit_slot(s, g, (uint32_t)u, slot);
	c = ansm_find_class(s, g, (uint32_t)u, cls);
	if (c < 0) return -1;
	if (level == ANSM_L_CLASS) return ansm_class_slot(s, g, (uint32_t)u, (uint32_t)c, slot);
	t = ansm_find_type(s, g, (uint32_t)u, (uint32_t)c, type);
	return t < 0 ? -1 : ansm_type_slot(s, g, (uint32_t)u, (uint32_t)c, (uint32_t)t, slot);
}

/* ---- first person, weapon objects, devices, vehicles, overlays, damage ------------------------------------------------- */
static int block_slot(const ansm_section *s, uint32_t off, uint32_t count, uint32_t block, unsigned slot)
{
	return block < count ? list_slot(s, s->data + off + LIST_REC * block, slot) : -1;
}

int ansm_fp_slot(const ansm_section *s, const ansm_graph *g, uint32_t block, unsigned slot)
{
	return block_slot(s, g->o_fps, g->fp_count, block, slot);
}

int ansm_wobj_slot(const ansm_section *s, const ansm_graph *g, uint32_t block, unsigned slot)
{
	return block_slot(s, g->o_wobjs, g->wobj_count, block, slot);
}

int ansm_device_slot(const ansm_section *s, const ansm_graph *g, uint32_t block, unsigned slot)
{
	return block_slot(s, g->o_devices, g->device_count, block, slot);
}

int ansm_vehicle_info(const ansm_section *s, const ansm_graph *g, uint32_t v, ansm_vehicle *out)
{
	const uint8_t *r;
	if (v >= g->vehicle_count) return -1;
	r = s->data + g->o_vehicles + VEH_REC * v;
	out->steer_right_per_frame = rdf(r + V_YAW_R);
	out->steer_left_per_frame = rdf(r + V_YAW_L);
	out->steer_right_frames = (uint16_t)rd16(r + V_RC);
	out->steer_left_frames = (uint16_t)rd16(r + V_LC);
	out->slot_count = rd32(r + V_SLOTS + 4);
	out->suspension_count = rd32(r + V_SUSP + 4);
	return 0;
}

int ansm_vehicle_slot(const ansm_section *s, const ansm_graph *g, uint32_t v, unsigned slot)
{
	return v < g->vehicle_count ? list_slot(s, s->data + g->o_vehicles + VEH_REC * v + V_SLOTS, slot) : -1;
}

int ansm_vehicle_suspension(const ansm_section *s, const ansm_graph *g, uint32_t v, uint32_t i, ansm_suspension *out)
{
	const uint8_t *r, *e;
	if (v >= g->vehicle_count) return -1;
	r = s->data + g->o_vehicles + VEH_REC * v;
	if (i >= rd32(r + V_SUSP + 4)) return -1;
	e = s->data + rd32(r + V_SUSP) + SUSP_REC * i;
	out->mass_point = (uint16_t)rd16(e);
	out->anim = (int16_t)rds16(e + 2);
	out->full_extension = rdf(e + 4);
	out->full_compression = rdf(e + 8);
	return 0;
}

int ansm_object_overlay_get(const ansm_section *s, const ansm_graph *g, uint32_t i, ansm_object_overlay *out)
{
	const uint8_t *r;
	if (i >= g->object_count) return -1;
	r = s->data + g->o_objects + OBJ_REC * i;
	out->anim = (int16_t)rds16(r);
	out->function = (uint16_t)rd16(r + 2);
	out->controls = (uint16_t)rd16(r + 4);
	return 0;
}

unsigned ansm_damage_regions(const ansm_graph *g) { return g->damage_count / 16u; }

int ansm_damage_anim(const ansm_section *s, const ansm_graph *g, unsigned category, unsigned direction, unsigned region)
{
	unsigned regions = ansm_damage_regions(g);
	uint32_t v;
	if (category >= 4 || direction >= 4 || region >= regions) return -1;
	v = rd16(s->data + g->o_damage + 2u * ((category * 4u + direction) * regions + region));
	return v == NONE16 ? -1 : (int)v;
}
