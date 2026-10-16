#include "world/model.h"
#include <string.h>

/* MODL section reader; layout in docs/11-model-section.md (writer: tools/halo2ps2/modelconv.py).
 * Everything model_open accepts can be read through the accessors without leaving the buffer. The
 * accessors also re-check their indices against the tables (read from the section header), so a
 * model_info / model_part / model_batch edited by the caller cannot make them read out of bounds. */

#define HEADER_SIZE 96u
#define DIR_AT 0x20u
#define MODEL_REC 80u
#define NODE_REC 72u
#define MARKER_REC 68u
#define PERM_REC 64u
#define PART_REC 32u
#define BATCH_REC 40u
#define VIF_UNPACK_V4_32 0x6Cu
#define VIF_UNPACK_V4_16 0x6Du
#define VIF_UNPACK_V4_8 0x6Eu
#define TOPS 0x8000u
#define VU_POS 4u
#define VU_UV (VU_POS + MODEL_MAX_BATCH_TRIS * 3u)
#define VU_NRM (VU_UV + MODEL_MAX_BATCH_TRIS * 3u)
#define MAX_UV_SHIFT 14u
#define ANY 0xFFFFu
#define MODEL_MAX_REGIONS 64u

enum { T_MODELS, T_NODES, T_MARKERS, T_PERMS, T_PARTS, T_BATCHES, T_VIF, T_COUNT };
static const uint32_t rec_size[T_COUNT] = { MODEL_REC, NODE_REC, MARKER_REC, PERM_REC, PART_REC, BATCH_REC, 1 };

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static int rds16(const uint8_t *p) { return (int)(int16_t)(uint16_t)rd16(p); }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
/* false for NaN and infinities: every float the renderer sees is finite */
static int fin(float f) { return f > -1e30f && f < 1e30f; }

static uint32_t tab_count(const model_section *s, int t) { return rd32(s->data + DIR_AT + 8 * t + 4); }

/* A batch packet is exactly: UNPACK V4-32 (addr 0, 2 qw), UNPACK V4-16 (addr 4, n), UNPACK V4-16
 * (addr 4+72, n), UNPACK V4-8 signed (addr 4+144, n), zero padding to a quadword. Anything else
 * could make the VIF run arbitrary commands, so the packet is verified word for word. */
static uint32_t packet_bytes(uint32_t tris)
{
	uint32_t n = tris * 3;
	return (4 + 32 + 4 + n * 8 + 4 + n * 8 + 4 + n * 4 + 15) & ~15u;
}

static int packet_ok(const uint8_t *p, uint32_t tris)
{
	uint32_t n = tris * 3, o;
	float centre[3], scale;
	if (rd32(p) != (((uint32_t)VIF_UNPACK_V4_32 << 24) | (2u << 16) | TOPS)) return 0;
	for (int i = 0; i < 3; i++) centre[i] = rdf(p + 4 + 4 * i);
	scale = rdf(p + 16);
	if (!fin(centre[0]) || !fin(centre[1]) || !fin(centre[2]) || !fin(scale) || scale < 0.0f) return 0;
	if (rd32(p + 20) != n || rd32(p + 24) != 0 || rd32(p + 28) != 0 || rd32(p + 32) != 0) return 0;
	o = 36;
	if (rd32(p + o) != (((uint32_t)VIF_UNPACK_V4_16 << 24) | (n << 16) | TOPS | VU_POS)) return 0;
	o += 4 + n * 8;
	if (rd32(p + o) != (((uint32_t)VIF_UNPACK_V4_16 << 24) | (n << 16) | TOPS | VU_UV)) return 0;
	o += 4 + n * 8;
	if (rd32(p + o) != (((uint32_t)VIF_UNPACK_V4_8 << 24) | (n << 16) | TOPS | VU_NRM)) return 0;
	o += 4 + n * 4;
	for (; o < packet_bytes(tris); o++)
		if (p[o]) return 0;
	return 1;
}

static int name_ok(const uint8_t *p) { return p[MODEL_NAME_LEN - 1] == 0; }

model_status model_open(model_section *s, const uint8_t *d, size_t size)
{
	const uint8_t *tab[T_COUNT];
	uint32_t cnt[T_COUNT];
	memset(s, 0, sizeof *s);
	if (size < HEADER_SIZE) return MODEL_ERR_TRUNCATED;
	if (memcmp(d, "MODL", 4) != 0) return MODEL_ERR_MAGIC;
	if (rd16(d + 4) != 1) return MODEL_ERR_VERSION;
	if (rd32(d + 8) != size) return MODEL_ERR_LAYOUT;
	for (int t = 0; t < T_COUNT; t++) {
		uint32_t off = rd32(d + DIR_AT + 8 * t), n = rd32(d + DIR_AT + 8 * t + 4);
		if (off % 16 || off < HEADER_SIZE || off > size) return MODEL_ERR_LAYOUT;
		if ((uint64_t)n * rec_size[t] > size - off) return MODEL_ERR_TRUNCATED;
		tab[t] = d + off;
		cnt[t] = n;
	}
	if (cnt[T_PARTS] > 0xFFFF || cnt[T_BATCHES] > 0xFFFF || cnt[T_PERMS] > 0xFFFF) return MODEL_ERR_LAYOUT;

	/* nodes, markers, permutation names */
	for (uint32_t i = 0; i < cnt[T_NODES]; i++) {
		const uint8_t *p = tab[T_NODES] + i * NODE_REC;
		if (!name_ok(p)) return MODEL_ERR_LAYOUT;
		for (int k = 0; k < 8; k++)
			if (!fin(rdf(p + 40 + 4 * k))) return MODEL_ERR_LAYOUT;      /* translation[3], rotation[4], scale */
	}
	for (uint32_t i = 0; i < cnt[T_MARKERS]; i++) {
		const uint8_t *p = tab[T_MARKERS] + i * MARKER_REC;
		if (!name_ok(p)) return MODEL_ERR_LAYOUT;
		for (int k = 0; k < 7; k++)
			if (!fin(rdf(p + 40 + 4 * k))) return MODEL_ERR_LAYOUT;
	}
	for (uint32_t i = 0; i < cnt[T_PERMS]; i++)
		if (!name_ok(tab[T_PERMS] + i * PERM_REC)) return MODEL_ERR_LAYOUT;

	/* batches: each packet verified; the VIF data is covered exactly, in order */
	uint64_t vif_at = 0;
	for (uint32_t i = 0; i < cnt[T_BATCHES]; i++) {
		const uint8_t *p = tab[T_BATCHES] + i * BATCH_REC;
		uint32_t part = rd16(p), tris = rd16(p + 2), pc = p[4], off = rd32(p + 8), qw = rd32(p + 12);
		if (part >= cnt[T_PARTS] || tris < 1 || tris > MODEL_MAX_BATCH_TRIS || pc < 1 || pc > MODEL_MAX_PALETTE)
			return MODEL_ERR_LAYOUT;
		if (p[5] | p[6] | p[7]) return MODEL_ERR_LAYOUT;
		for (uint32_t k = pc; k < MODEL_MAX_PALETTE; k++)
			if (p[16 + k]) return MODEL_ERR_LAYOUT;
		if (off != vif_at || off % 16 || qw != packet_bytes(tris) / 16) return MODEL_ERR_LAYOUT;
		if ((uint64_t)off + (uint64_t)qw * 16 > cnt[T_VIF]) return MODEL_ERR_TRUNCATED;
		if (!packet_ok(tab[T_VIF] + off, tris)) return MODEL_ERR_LAYOUT;
		vif_at += (uint64_t)qw * 16;
	}
	if (vif_at != cnt[T_VIF]) return MODEL_ERR_LAYOUT;

	/* models partition the node, marker, permutation, part and batch tables in order */
	uint32_t run_node = 0, run_marker = 0, run_perm = 0, run_part = 0, run_batch = 0, last_tag = 0;
	uint64_t sum_tris = 0;
	for (uint32_t i = 0; i < cnt[T_MODELS]; i++) {
		const uint8_t *m = tab[T_MODELS] + i * MODEL_REC;
		uint32_t tag = rd32(m), ncnt = rd16(m + 4), mcnt = rd16(m + 6), rcnt = rd16(m + 8), pcnt = rd16(m + 10);
		uint32_t fn = rd32(m + 12), fm = rd32(m + 16), fp = rd32(m + 20), fpart = rd32(m + 64), npart = rd32(m + 68);
		if (i > 0 && tag <= last_tag) return MODEL_ERR_LAYOUT;
		last_tag = tag;
		if (ncnt > MODEL_MAX_NODES || rcnt > MODEL_MAX_REGIONS || fn != run_node || fm != run_marker || fp != run_perm || fpart != run_part)
			return MODEL_ERR_LAYOUT;
		if ((uint64_t)fn + ncnt > cnt[T_NODES] || (uint64_t)fm + mcnt > cnt[T_MARKERS] ||
		    (uint64_t)fp + pcnt > cnt[T_PERMS] || (uint64_t)fpart + npart > cnt[T_PARTS])
			return MODEL_ERR_LAYOUT;
		if (!fin(rdf(m + 24)) || !fin(rdf(m + 28)) || !fin(rdf(m + 32)) || !(rdf(m + 36) >= 0.0f) || !fin(rdf(m + 36)))
			return MODEL_ERR_LAYOUT;
		for (int k = 0; k < MODEL_LODS; k++)
			if (!fin(rdf(m + 40 + 4 * k))) return MODEL_ERR_LAYOUT;
		for (uint32_t k = 0; k < ncnt; k++) {
			const uint8_t *n = tab[T_NODES] + (fn + k) * NODE_REC;
			for (int f = 0; f < 3; f++) {
				int v = rds16(n + 32 + 2 * f);
				if (v < -1 || v >= (int)ncnt) return MODEL_ERR_LAYOUT;
			}
		}
		for (uint32_t k = 0; k < mcnt; k++) {
			const uint8_t *mk = tab[T_MARKERS] + (fm + k) * MARKER_REC;
			uint32_t reg = rd16(mk + 32), perm = rd16(mk + 34);
			int node = rds16(mk + 36);
			if (node < -1 || node >= (int)ncnt || (reg != ANY && reg >= rcnt) || (perm != ANY && perm >= pcnt))
				return MODEL_ERR_LAYOUT;
		}
		uint32_t last_region = 0;
		for (uint32_t k = 0; k < pcnt; k++) {
			const uint8_t *pm = tab[T_PERMS] + (fp + k) * PERM_REC;
			uint32_t reg = rd16(pm + 32);
			if (reg >= rcnt || reg < last_region) return MODEL_ERR_LAYOUT;
			last_region = reg;
			for (int l = 0; l < MODEL_LODS; l++) {
				uint32_t first = rd16(pm + 36 + 4 * l), n = rd16(pm + 38 + 4 * l);
				if (first < fpart || (uint64_t)first + n > (uint64_t)fpart + npart) return MODEL_ERR_LAYOUT;
			}
		}
		for (uint32_t k = fpart; k < fpart + npart; k++) {
			const uint8_t *pt = tab[T_PARTS] + k * PART_REC;
			uint32_t fb = rd16(pt + 4), bc = rd16(pt + 6), tris = rd16(pt + 8), shift = rd16(pt + 10), flags = rd16(pt + 14);
			int rigid = rds16(pt + 12);
			if (fb != run_batch || bc < 1 || tris < 1 || (uint64_t)run_batch + bc > cnt[T_BATCHES]) return MODEL_ERR_LAYOUT;
			if (shift > MAX_UV_SHIFT || flags > 3 || rigid < -1 || rigid >= (int)ncnt) return MODEL_ERR_LAYOUT;
			if (!fin(rdf(pt + 16)) || !fin(rdf(pt + 20)) || !fin(rdf(pt + 24)) || !fin(rdf(pt + 28)) || rdf(pt + 28) < 0.0f)
				return MODEL_ERR_LAYOUT;
			uint32_t t = 0;
			for (uint32_t b = run_batch; b < run_batch + bc; b++) {
				const uint8_t *bt = tab[T_BATCHES] + b * BATCH_REC;
				if (rd16(bt) != k) return MODEL_ERR_LAYOUT;
				for (uint32_t q = 0; q < bt[4]; q++)
					if (bt[16 + q] >= ncnt) return MODEL_ERR_LAYOUT;
				t += rd16(bt + 2);
			}
			if (t != tris) return MODEL_ERR_LAYOUT;
			sum_tris += t;
			run_batch += bc;
		}
		run_node += ncnt;
		run_marker += mcnt;
		run_perm += pcnt;
		run_part += npart;
	}
	if (run_node != cnt[T_NODES] || run_marker != cnt[T_MARKERS] || run_perm != cnt[T_PERMS] ||
	    run_part != cnt[T_PARTS] || run_batch != cnt[T_BATCHES] || sum_tris != rd32(d + 12))
		return MODEL_ERR_LAYOUT;

	s->data = d;
	s->size = size;
	s->model_count = cnt[T_MODELS];
	s->part_count = cnt[T_PARTS];
	s->batch_count = cnt[T_BATCHES];
	s->models = tab[T_MODELS];
	s->nodes = tab[T_NODES];
	s->markers = tab[T_MARKERS];
	s->ranges = tab[T_PERMS];
	s->parts = tab[T_PARTS];
	s->batches = tab[T_BATCHES];
	s->vif = tab[T_VIF];
	s->vif_size = cnt[T_VIF];
	return MODEL_OK;
}

int model_find(const model_section *s, uint32_t tag_id)
{
	uint32_t lo = 0, hi = s->model_count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, v = rd32(s->models + mid * MODEL_REC);
		if (v == tag_id) return (int)mid;
		if (v < tag_id) lo = mid + 1;
		else hi = mid;
	}
	return -1;
}

model_info model_get(const model_section *s, uint32_t index)
{
	model_info m;
	memset(&m, 0, sizeof m);
	if (index >= s->model_count) return m;
	const uint8_t *p = s->models + index * MODEL_REC;
	m.tag_id = rd32(p);
	m.node_count = (uint16_t)rd16(p + 4);
	m.marker_count = (uint16_t)rd16(p + 6);
	m.region_count = (uint16_t)rd16(p + 8);
	m.permutation_count = (uint16_t)rd16(p + 10);
	m.first_node = rd32(p + 12);
	m.first_marker = rd32(p + 16);
	m.first_range = rd32(p + 20);
	for (int i = 0; i < 3; i++) m.centre[i] = rdf(p + 24 + 4 * i);
	m.radius = rdf(p + 36);
	for (int i = 0; i < MODEL_LODS; i++) m.lod_cutoff[i] = rdf(p + 40 + 4 * i);
	m.triangles_lod0 = rd32(p + 60);
	return m;
}

model_node model_get_node(const model_section *s, const model_info *m, uint32_t node)
{
	model_node n;
	memset(&n, 0, sizeof n);
	n.parent = n.first_child = n.next_sibling = MODEL_NO_NODE;
	n.rotation[3] = n.scale = 1.0f;
	if (node >= m->node_count || (uint64_t)m->first_node + node >= tab_count(s, T_NODES)) return n;
	const uint8_t *p = s->nodes + (m->first_node + node) * NODE_REC;
	memcpy(n.name, p, MODEL_NAME_LEN);
	n.name[MODEL_NAME_LEN - 1] = 0;
	n.parent = (int16_t)rds16(p + 32);
	n.first_child = (int16_t)rds16(p + 34);
	n.next_sibling = (int16_t)rds16(p + 36);
	for (int i = 0; i < 3; i++) n.translation[i] = rdf(p + 40 + 4 * i);
	for (int i = 0; i < 4; i++) n.rotation[i] = rdf(p + 52 + 4 * i);
	n.scale = rdf(p + 68);
	return n;
}

model_marker model_get_marker(const model_section *s, const model_info *m, uint32_t marker)
{
	model_marker k;
	memset(&k, 0, sizeof k);
	k.region = k.permutation = ANY;
	k.node = MODEL_NO_NODE;
	k.rotation[3] = 1.0f;
	if (marker >= m->marker_count || (uint64_t)m->first_marker + marker >= tab_count(s, T_MARKERS)) return k;
	const uint8_t *p = s->markers + (m->first_marker + marker) * MARKER_REC;
	memcpy(k.name, p, MODEL_NAME_LEN);
	k.name[MODEL_NAME_LEN - 1] = 0;
	k.region = (uint16_t)rd16(p + 32);
	k.permutation = (uint16_t)rd16(p + 34);
	k.node = (int16_t)rds16(p + 36);
	for (int i = 0; i < 3; i++) k.translation[i] = rdf(p + 40 + 4 * i);
	for (int i = 0; i < 4; i++) k.rotation[i] = rdf(p + 52 + 4 * i);
	return k;
}

int model_find_marker(const model_section *s, const model_info *m, const char *name)
{
	if (!name) return -1;
	uint32_t total = tab_count(s, T_MARKERS);
	for (uint32_t i = 0; i < m->marker_count; i++) {
		if ((uint64_t)m->first_marker + i >= total) break;
		const uint8_t *p = s->markers + (m->first_marker + i) * MARKER_REC;
		if (strncmp((const char *)p, name, MODEL_NAME_LEN) == 0) return (int)i;
	}
	return -1;
}

static const uint8_t *perm_rec(const model_section *s, const model_info *m, uint32_t perm)
{
	if (perm >= m->permutation_count || (uint64_t)m->first_range + perm >= tab_count(s, T_PERMS)) return NULL;
	return s->ranges + (m->first_range + perm) * PERM_REC;
}

model_part_range model_parts(const model_section *s, const model_info *m, uint32_t perm, uint32_t lod)
{
	model_part_range r = { 0, 0 };
	const uint8_t *p = perm_rec(s, m, perm);
	if (!p || lod >= MODEL_LODS) return r;
	r.first_part = (uint16_t)rd16(p + 36 + 4 * lod);
	r.part_count = (uint16_t)rd16(p + 38 + 4 * lod);
	if ((uint32_t)r.first_part + r.part_count > s->part_count) { r.first_part = 0; r.part_count = 0; }
	return r;
}

uint32_t model_perm_region(const model_section *s, const model_info *m, uint32_t perm)
{
	const uint8_t *p = perm_rec(s, m, perm);
	return p ? rd16(p + 32) : 0;
}

void model_perm_name(const model_section *s, const model_info *m, uint32_t perm, char out[MODEL_NAME_LEN])
{
	const uint8_t *p = perm_rec(s, m, perm);
	memset(out, 0, MODEL_NAME_LEN);
	if (p) memcpy(out, p, MODEL_NAME_LEN - 1);
}

void model_select_permutations(const model_section *s, const model_info *m, const char *variant, uint16_t *out)
{
	int want = variant && variant[0];
	for (uint32_t r = 0; r < m->region_count; r++) {
		uint32_t first = ANY, hit = ANY;
		for (uint32_t p = 0; p < m->permutation_count; p++) {
			const uint8_t *rec = perm_rec(s, m, p);
			if (!rec || rd16(rec + 32) != r) continue;
			if (first == ANY) first = p;
			if (want && hit == ANY && strncmp((const char *)rec, variant, MODEL_NAME_LEN) == 0) hit = p;
		}
		out[r] = (uint16_t)(hit != ANY ? hit : first);
	}
}

model_part model_get_part(const model_section *s, uint32_t part_index)
{
	model_part p;
	memset(&p, 0, sizeof p);
	p.rigid_node = MODEL_NO_NODE;
	if (part_index >= s->part_count) return p;
	const uint8_t *r = s->parts + part_index * PART_REC;
	p.shader_id = rd32(r);
	p.first_batch = (uint16_t)rd16(r + 4);
	p.batch_count = (uint16_t)rd16(r + 6);
	p.triangles = (uint16_t)rd16(r + 8);
	p.uv_shift = (uint16_t)rd16(r + 10);
	p.rigid_node = (int16_t)rds16(r + 12);
	p.flags = (uint16_t)rd16(r + 14);
	for (int i = 0; i < 3; i++) p.centre[i] = rdf(r + 16 + 4 * i);
	p.radius = rdf(r + 28);
	return p;
}

model_batch model_get_batch(const model_section *s, uint32_t batch_index)
{
	model_batch b;
	memset(&b, 0, sizeof b);
	if (batch_index >= s->batch_count) return b;
	const uint8_t *r = s->batches + batch_index * BATCH_REC;
	b.part = (uint16_t)rd16(r);
	b.triangles = (uint16_t)rd16(r + 2);
	b.palette_count = r[4];
	b.vif_offset = rd32(r + 8);
	b.vif_qwords = rd32(r + 12);
	memcpy(b.palette, r + 16, MODEL_MAX_PALETTE);
	return b;
}

const uint8_t *model_batch_vif(const model_section *s, const model_batch *b)
{
	if (!b || (uint64_t)b->vif_offset + (uint64_t)b->vif_qwords * 16 > s->vif_size) return NULL;
	return s->vif + b->vif_offset;
}

uint32_t model_pick_lod(const model_info *m, float diameter_px)
{
	for (uint32_t lod = 0; lod < MODEL_LODS; lod++)
		if (diameter_px >= m->lod_cutoff[lod]) return lod;
	return MODEL_LODS - 1;                 /* smaller than every cutoff, or NaN: the cheapest */
}
