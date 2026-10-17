/* PATn section reader and navigation queries (docs/26-path-section.md). Pure C11, no heap, no double, no recursion.
 * path_open validates every table once (sizes, alignment, ranges, finite floats, grid, links), so the queries can never read
 * out of bounds, whatever the bytes; every search has an expansion budget and every loop is bounded by a table size:
 * corrupt data gives wrong answers, never a crash or an endless loop. Records are decoded with byte loads. */
#include "ai/path.h"
#include <math.h>
#include <string.h>

#define HEADER_SIZE 128u
#define DIR_AT 0x48u
#define SECTOR_REC 32u
#define POINT_REC 12u
#define INDEX_REC 2u
#define LINK_REC 12u
#define CELLSTART_REC 4u
#define CELLITEM_REC 2u
#define MAX_COORD 65536.0f
#define DZ_SCALE 256.0f
#define HEUR_MARGIN 0.9995f      /* the heuristic runs a hair under the link costs: float noise cannot make it inconsistent */

enum { T_SECTORS, T_POINTS, T_INDICES, T_LINKS, T_CELLSTART, T_CELLITEMS, T_COUNT };

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }
static int sane_at(const uint8_t *p) { float f = rdf(p); return finite_at(p) && f >= -MAX_COORD && f <= MAX_COORD; }

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

/* ---- opening ------------------------------------------------------------------------------------------------ */
path_status path_open(const uint8_t *data, size_t size, path_bsp *out)
{
	static const uint32_t rec[T_COUNT] = { SECTOR_REC, POINT_REC, INDEX_REC, LINK_REC, CELLSTART_REC, CELLITEM_REC };
	uint32_t off[T_COUNT], cnt[T_COUNT], i, k;
	const uint8_t *t[T_COUNT];

	memset(out, 0, sizeof *out);
	if (!data || size < HEADER_SIZE) return PATH_ERR_TRUNCATED;
	if (memcmp(data, "PATH", 4) != 0) return PATH_ERR_MAGIC;
	if (rd16(data + 4) != PATH_VERSION) return PATH_ERR_VERSION;
	if (rd32(data + 8) > size) return PATH_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return PATH_ERR_LAYOUT;
	for (i = 0; i < 6; i++)
		if (!sane_at(data + 0x10 + 4 * i)) return PATH_ERR_LAYOUT;
	for (i = 0; i < 7; i++)                                /* cell, origin, thresholds are finite floats (+0x34 holds nx, ny) */
		if (i != 3 && !sane_at(data + 0x28 + 4 * i)) return PATH_ERR_LAYOUT;
	for (i = 0; i < T_COUNT; i++) {
		off[i] = rd32(data + DIR_AT + 8 * i);
		cnt[i] = rd32(data + DIR_AT + 8 * i + 4);
		if (!table_ok(off[i], cnt[i], rec[i], size)) return PATH_ERR_LAYOUT;
		t[i] = data + off[i];
	}
	out->data = data; out->size = size;
	out->sector_count = cnt[T_SECTORS]; out->point_count = cnt[T_POINTS]; out->index_count = cnt[T_INDICES];
	out->link_count = cnt[T_LINKS]; out->cellstart_count = cnt[T_CELLSTART]; out->item_count = cnt[T_CELLITEMS];
	out->comp_count = rd32(data + 0x0C);
	for (i = 0; i < 6; i++) out->bounds[i] = rdf(data + 0x10 + 4 * i);
	out->cell = rdf(data + 0x28); out->gx0 = rdf(data + 0x2C); out->gy0 = rdf(data + 0x30);
	out->nx = rd16(data + 0x34); out->ny = rd16(data + 0x36);
	out->step_max = rdf(data + 0x38); out->vault_max = rdf(data + 0x3C); out->drop_max = rdf(data + 0x40);
	out->cost_scale = rd32(data + 0x44);
	out->sectors = t[T_SECTORS]; out->points = t[T_POINTS]; out->indices = t[T_INDICES]; out->links = t[T_LINKS];
	out->cellstart = t[T_CELLSTART]; out->cellitems = t[T_CELLITEMS];
#define FAIL do { memset(out, 0, sizeof *out); return PATH_ERR_LAYOUT; } while (0)
	if (out->sector_count > PATH_MAX_SECTORS || out->point_count > 0xFFFFu) FAIL;
	if (!(out->cell > 0.0f) || out->nx < 1 || out->ny < 1) FAIL;
	if ((uint64_t)out->nx * out->ny + 1u != out->cellstart_count) FAIL;
	if (out->cost_scale < 1u || out->cost_scale > 1024u) FAIL;
	if (out->step_max < 0.0f || out->vault_max < 0.0f || out->drop_max < 0.0f) FAIL;
	if (out->comp_count > out->sector_count) FAIL;
	for (i = 0; i < out->point_count; i++)
		for (k = 0; k < 3; k++)
			if (!sane_at(out->points + i * POINT_REC + 4 * k)) FAIL;
	for (i = 0; i < out->index_count; i++)
		if (rd16(out->indices + i * INDEX_REC) >= out->point_count) FAIL;
	for (i = 0; i < out->sector_count; i++) {
		const uint8_t *s = out->sectors + i * SECTOR_REC;
		uint32_t nv = s[22];
		for (k = 0; k < 3; k++)
			if (!sane_at(s + 4 * k)) FAIL;
		if (nv < 3u || nv > PATH_MAX_POLY) FAIL;
		if ((uint64_t)rd32(s + 12) + nv > out->index_count) FAIL;
		if ((uint64_t)rd32(s + 16) + rd16(s + 20) > out->link_count) FAIL;
		if (rd16(s + 24) >= out->comp_count) FAIL;
	}
	for (i = 0; i < out->link_count; i++) {
		const uint8_t *l = out->links + i * LINK_REC;
		if (rd16(l) >= out->sector_count || l[2] >= PATH_LINK_KINDS) FAIL;
		if (rd16(l + 4) >= out->point_count || rd16(l + 6) >= out->point_count) FAIL;
	}
	if (rd32(out->cellstart) != 0u) FAIL;
	for (i = 0; i + 1 < out->cellstart_count; i++)
		if (rd32(out->cellstart + (i + 1) * 4u) < rd32(out->cellstart + i * 4u)) FAIL;
	if (rd32(out->cellstart + (out->cellstart_count - 1u) * 4u) != out->item_count) FAIL;
	for (i = 0; i < out->item_count; i++)
		if (rd16(out->cellitems + i * CELLITEM_REC) >= out->sector_count) FAIL;
#undef FAIL
	return PATH_OK;
}

/* ---- small geometry --------------------------------------------------------------------------------------------- */
static const uint8_t *sector_rec(const path_bsp *b, uint32_t s) { return b->sectors + s * SECTOR_REC; }
static void point_at(const path_bsp *b, uint32_t p, float o[3])
{
	const uint8_t *q = b->points + p * POINT_REC;
	o[0] = rdf(q); o[1] = rdf(q + 4); o[2] = rdf(q + 8);
}
static void centroid_of(const path_bsp *b, uint32_t s, float c[3])
{
	const uint8_t *r = sector_rec(b, s);
	c[0] = rdf(r); c[1] = rdf(r + 4); c[2] = rdf(r + 8);
}
static uint32_t load_poly(const path_bsp *b, uint32_t s, float P[][3])
{
	const uint8_t *r = sector_rec(b, s);
	uint32_t n = r[22], first = rd32(r + 12), i;
	for (i = 0; i < n; i++) point_at(b, rd16(b->indices + (first + i) * INDEX_REC), P[i]);
	return n;
}
/* area vector of the polygon about the centroid c: its z component is positive for a counter-clockwise polygon seen from above */
static void plane_normal(const float P[][3], uint32_t n, const float c[3], float nrm[3])
{
	uint32_t i;
	nrm[0] = nrm[1] = nrm[2] = 0.0f;
	for (i = 0; i < n; i++) {
		const float *p = P[i], *q = P[(i + 1) % n];
		float ax = p[0] - c[0], ay = p[1] - c[1], az = p[2] - c[2];
		float bx = q[0] - c[0], by = q[1] - c[1], bz = q[2] - c[2];
		nrm[0] += ay * bz - az * by;
		nrm[1] += az * bx - ax * bz;
		nrm[2] += ax * by - ay * bx;
	}
}
static float plane_z(const float nrm[3], const float c[3], float x, float y)
{
	if (nrm[2] < 1e-12f) return c[2];
	return c[2] - (nrm[0] * (x - c[0]) + nrm[1] * (y - c[1])) / nrm[2];
}
/* signed distance (plan, positive inside) of (x, y) from edge i of the polygon, times the edge length (cheap form) */
static int inside_plan(const float P[][3], uint32_t n, float x, float y)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		const float *p = P[i], *q = P[(i + 1) % n];
		float ex = q[0] - p[0], ey = q[1] - p[1];
		float cr = ex * (y - p[1]) - ey * (x - p[0]);
		if (cr < -1e-4f * (fabsf(ex) + fabsf(ey))) return 0;
	}
	return 1;
}

int path_sector_get(const path_bsp *b, uint32_t sector, path_sector_info *out)
{
	const uint8_t *r;
	if (!b->data || sector >= b->sector_count) return 0;
	r = sector_rec(b, sector);
	centroid_of(b, sector, out->centroid);
	out->first_link = rd32(r + 16); out->link_count = (uint16_t)rd16(r + 20);
	out->nverts = r[22]; out->flags = r[23]; out->comp = (uint16_t)rd16(r + 24); out->surface = (uint16_t)rd16(r + 26);
	out->material = r[28];
	out->headroom = (float)r[29] / PATH_HEADROOM_SCALE;
	out->thickness = (float)r[30] / PATH_THICK_SCALE;
	return 1;
}

uint32_t path_sector_polygon(const path_bsp *b, uint32_t sector, float out[][3])
{
	if (!b->data || sector >= b->sector_count) return 0;
	return load_poly(b, sector, out);
}

float path_sector_height(const path_bsp *b, uint32_t sector, float x, float y)
{
	float P[PATH_MAX_POLY][3], c[3], n[3];
	uint32_t cnt;
	if (!b->data || sector >= b->sector_count) return 0.0f;
	cnt = load_poly(b, sector, P);
	centroid_of(b, sector, c);
	plane_normal(P, cnt, c, n);
	return plane_z(n, c, x, y);
}

/* ---- locating ----------------------------------------------------------------------------------------------- */
static int grid_cell(const path_bsp *b, float x, float y, uint32_t *gx, uint32_t *gy)
{
	float fx = (x - b->gx0) / b->cell, fy = (y - b->gy0) / b->cell;
	if (!(fx >= 0.0f) || !(fy >= 0.0f) || fx >= (float)b->nx || fy >= (float)b->ny) return 0;
	*gx = (uint32_t)fx; *gy = (uint32_t)fy;
	if (*gx >= b->nx) *gx = b->nx - 1u;
	if (*gy >= b->ny) *gy = b->ny - 1u;
	return 1;
}

int32_t path_locate_ex(const path_bsp *b, const float pos[3], float below, float above, float *floor_z)
{
	uint32_t gx, gy, cell, i, i0, i1;
	int32_t best = -1;
	float best_h = 0.0f;
	if (!b->data || !grid_cell(b, pos[0], pos[1], &gx, &gy)) return -1;
	cell = gy * b->nx + gx;
	i0 = rd32(b->cellstart + cell * 4u);
	i1 = rd32(b->cellstart + (cell + 1u) * 4u);
	for (i = i0; i < i1; i++) {
		uint32_t s = rd16(b->cellitems + i * CELLITEM_REC), n;
		float P[PATH_MAX_POLY][3], c[3], nr[3], h;
		n = load_poly(b, s, P);
		if (!inside_plan(P, n, pos[0], pos[1])) continue;
		centroid_of(b, s, c);
		plane_normal(P, n, c, nr);
		h = plane_z(nr, c, pos[0], pos[1]);
		if (h > pos[2] + below || pos[2] - h > above) continue;
		if (best < 0 || h > best_h) { best = (int32_t)s; best_h = h; }
	}
	if (best >= 0 && floor_z) *floor_z = best_h;
	return best;
}

int32_t path_locate(const path_bsp *b, const float pos[3])
{
	return path_locate_ex(b, pos, PATH_LOCATE_BELOW, PATH_LOCATE_ABOVE, NULL);
}

/* closest point of sector `s` to pos (plan, then lifted on the plane); returns the squared 3D distance */
static float sector_closest(const path_bsp *b, uint32_t s, const float pos[3], float out[3])
{
	float P[PATH_MAX_POLY][3], c[3], nr[3], bx = pos[0], by = pos[1], bd = 1e30f, dz;
	uint32_t n = load_poly(b, s, P), i;
	centroid_of(b, s, c);
	plane_normal(P, n, c, nr);
	if (!inside_plan(P, n, pos[0], pos[1])) {
		for (i = 0; i < n; i++) {
			const float *p = P[i], *q = P[(i + 1) % n];
			float ex = q[0] - p[0], ey = q[1] - p[1], l2 = ex * ex + ey * ey, t = 0.0f, px, py, d;
			if (l2 > 1e-18f) {
				t = ((pos[0] - p[0]) * ex + (pos[1] - p[1]) * ey) / l2;
				t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
			}
			px = p[0] + ex * t; py = p[1] + ey * t;
			d = (px - pos[0]) * (px - pos[0]) + (py - pos[1]) * (py - pos[1]);
			if (d < bd) { bd = d; bx = px; by = py; }
		}
	} else {
		bd = 0.0f;
	}
	out[0] = bx; out[1] = by; out[2] = plane_z(nr, c, bx, by);
	dz = pos[2] - out[2];
	return bd + dz * dz;
}

int32_t path_nearest_sector(const path_bsp *b, const float pos[3], float max_dist, float *dist, float point[3])
{
	float best = 1e30f, bp[3] = { 0, 0, 0 }, lim = max_dist * max_dist;
	int32_t bs = -1;
	int32_t cx, cy, r, rmax;
	float fx, fy;
	if (!b->data || b->sector_count == 0u) return -1;
	fx = (pos[0] - b->gx0) / b->cell; fy = (pos[1] - b->gy0) / b->cell;          /* the cell under pos, clamped into the grid */
	fx = fx < 0.0f ? 0.0f : fx > (float)(b->nx - 1u) ? (float)(b->nx - 1u) : fx;
	fy = fy < 0.0f ? 0.0f : fy > (float)(b->ny - 1u) ? (float)(b->ny - 1u) : fy;
	if (!(fx == fx) || !(fy == fy)) return -1;
	cx = (int32_t)fx; cy = (int32_t)fy;
	rmax = (int32_t)(b->nx > b->ny ? b->nx : b->ny);
	for (r = 0; r <= rmax; r++) {
		int32_t gx, gy;
		float ring_min = (float)(r - 1) * b->cell;
		if (bs >= 0 && ring_min * ring_min > best) break;
		if (ring_min > max_dist) break;
		for (gy = cy - r; gy <= cy + r; gy++) {
			if (gy < 0 || gy >= (int32_t)b->ny) continue;
			for (gx = cx - r; gx <= cx + r; gx += (gy == cy - r || gy == cy + r) ? 1 : (2 * r > 0 ? 2 * r : 1)) {
				uint32_t cell, i, i0, i1;
				if (gx < 0 || gx >= (int32_t)b->nx) continue;
				cell = (uint32_t)gy * b->nx + (uint32_t)gx;
				i0 = rd32(b->cellstart + cell * 4u);
				i1 = rd32(b->cellstart + (cell + 1u) * 4u);
				for (i = i0; i < i1; i++) {
					uint32_t s = rd16(b->cellitems + i * CELLITEM_REC);
					float q[3], d = sector_closest(b, s, pos, q);
					if (d < best) { best = d; bs = (int32_t)s; bp[0] = q[0]; bp[1] = q[1]; bp[2] = q[2]; }
				}
			}
		}
	}
	if (bs < 0 || best > lim) return -1;
	if (dist) *dist = sqrtf(best);
	if (point) { point[0] = bp[0]; point[1] = bp[1]; point[2] = bp[2]; }
	return bs;
}

/* smallest distance (plan) from (x, y) to the polygon's edge lines, positive inside */
static float inset_of(const float P[][3], uint32_t n, float x, float y)
{
	float m = 1e30f;
	uint32_t i;
	for (i = 0; i < n; i++) {
		const float *p = P[i], *q = P[(i + 1) % n];
		float ex = q[0] - p[0], ey = q[1] - p[1], l = sqrtf(ex * ex + ey * ey), d;
		if (l < 1e-9f) continue;
		d = (ex * (y - p[1]) - ey * (x - p[0])) / l;
		if (d < m) m = d;
	}
	return m;
}

int path_snap(const path_bsp *b, uint32_t sector, const float pos[3], float margin, float out[3])
{
	float P[PATH_MAX_POLY][3], c[3], nr[3], q[3], lo, hi;
	uint32_t n, it;
	if (!b->data || sector >= b->sector_count) return 0;
	n = load_poly(b, sector, P);
	centroid_of(b, sector, c);
	plane_normal(P, n, c, nr);
	sector_closest(b, sector, pos, q);
	if (margin <= 0.0f || inset_of(P, n, q[0], q[1]) >= margin) {
		out[0] = q[0]; out[1] = q[1]; out[2] = plane_z(nr, c, q[0], q[1]);
		return 1;
	}
	if (inset_of(P, n, c[0], c[1]) < margin) {                /* too narrow for the margin: the best we have is the centre */
		out[0] = c[0]; out[1] = c[1]; out[2] = plane_z(nr, c, c[0], c[1]);
		return 0;
	}
	/* push the point along the inward normals of the edges that are too close (a few alternating projections: it moves the
	 * least in the usual case of one wall), then bisect towards the centroid should that not settle */
	for (it = 0; it < 8u; it++) {
		uint32_t e;
		int moved = 0;
		for (e = 0; e < n; e++) {
			const float *p0 = P[e], *p1 = P[(e + 1) % n];
			float ex = p1[0] - p0[0], ey = p1[1] - p0[1], l = sqrtf(ex * ex + ey * ey), d;
			if (l < 1e-9f) continue;
			d = (ex * (q[1] - p0[1]) - ey * (q[0] - p0[0])) / l;
			if (d < margin) { q[0] += -ey / l * (margin - d); q[1] += ex / l * (margin - d); moved = 1; }
		}
		if (!moved) break;
	}
	if (inset_of(P, n, q[0], q[1]) < margin - 1e-3f) {
		sector_closest(b, sector, pos, q);
		lo = 0.0f; hi = 1.0f;                                   /* the inset is concave along q -> centroid: bisect */
		for (it = 0; it < 12u; it++) {
			float mid = 0.5f * (lo + hi);
			if (inset_of(P, n, q[0] + (c[0] - q[0]) * mid, q[1] + (c[1] - q[1]) * mid) >= margin) hi = mid; else lo = mid;
		}
		q[0] = q[0] + (c[0] - q[0]) * hi; q[1] = q[1] + (c[1] - q[1]) * hi;
	}
	out[0] = q[0]; out[1] = q[1];
	out[2] = plane_z(nr, c, out[0], out[1]);
	return 1;
}

/* ---- links -------------------------------------------------------------------------------------------------- */
static void link_decode(const path_bsp *b, uint32_t li, path_link *o)
{
	const uint8_t *l = b->links + li * LINK_REC;
	int16_t dz = (int16_t)rd16(l + 10);
	o->to = (uint16_t)rd16(l); o->kind = l[2]; o->width = (float)l[3] / PATH_LINK_WIDTH_SCALE;
	point_at(b, rd16(l + 4), o->right);
	point_at(b, rd16(l + 6), o->left);
	o->dz = (float)dz / DZ_SCALE;
	o->cost = rd16(l + 8);
}

uint32_t path_link_count(const path_bsp *b, uint32_t sector)
{
	if (!b->data || sector >= b->sector_count) return 0;
	return rd16(sector_rec(b, sector) + 20);
}

int path_link_get(const path_bsp *b, uint32_t sector, uint32_t index, path_link *out)
{
	if (!b->data || sector >= b->sector_count || index >= rd16(sector_rec(b, sector) + 20)) return 0;
	link_decode(b, rd32(sector_rec(b, sector) + 16) + index, out);
	return 1;
}

uint32_t path_sector_hints(const path_bsp *b, uint32_t sector, uint32_t kind_mask, path_link *out, uint32_t max)
{
	uint32_t n = 0, first, cnt, i;
	if (!b->data || sector >= b->sector_count) return 0;
	first = rd32(sector_rec(b, sector) + 16);
	cnt = rd16(sector_rec(b, sector) + 20);
	for (i = 0; i < cnt && n < max; i++)
		if (kind_mask & PATH_KIND(b->links[(first + i) * LINK_REC + 2])) link_decode(b, first + i, &out[n++]);
	return n;
}

/* ---- scratch ------------------------------------------------------------------------------------------------ */
size_t path_scratch_bytes(uint32_t sector_count) { return (size_t)sector_count * 22u + 8u; }

int path_scratch_init(path_scratch *s, void *mem, size_t bytes, uint32_t n)
{
	uint8_t *p = (uint8_t *)mem;
	memset(s, 0, sizeof *s);
	if (!mem || ((uintptr_t)mem & 3u) || bytes < path_scratch_bytes(n)) return 0;
	memset(mem, 0, path_scratch_bytes(n));
	s->cap = n;
	s->g = (uint32_t *)(void *)p; p += 4u * n;
	s->f = (uint32_t *)(void *)p; p += 4u * n;
	s->seen = (uint32_t *)(void *)p; p += 4u * n;
	s->plink = (uint32_t *)(void *)p; p += 4u * n;
	s->parent = (uint16_t *)(void *)p; p += 2u * n;
	s->hpos = (uint16_t *)(void *)p; p += 2u * n;
	s->heap = (uint16_t *)(void *)p;
	return 1;
}

void path_query_default(path_query *q)
{
	q->radius = 0.2f; q->height = 0.5f; q->kinds = PATH_KINDS_WALKING; q->max_drop = 1.5f; q->blocked = NULL; q->max_expand = 0; q->snap_dist = 8.0f;
}

/* ---- A* ---------------------------------------------------------------------------------------------------- */
static int heap_less(const path_scratch *s, uint16_t a, uint16_t c)
{
	return s->f[a] < s->f[c] || (s->f[a] == s->f[c] && s->g[a] > s->g[c]);
}
static void heap_up(path_scratch *s, uint32_t i)
{
	uint16_t v = s->heap[i];
	while (i > 0) {
		uint32_t p = (i - 1u) / 2u;
		if (!heap_less(s, v, s->heap[p])) break;
		s->heap[i] = s->heap[p]; s->hpos[s->heap[i]] = (uint16_t)i;
		i = p;
	}
	s->heap[i] = v; s->hpos[v] = (uint16_t)i;
}
static void heap_down(path_scratch *s, uint32_t i)
{
	uint16_t v = s->heap[i];
	for (;;) {
		uint32_t c = 2u * i + 1u;
		if (c >= s->heap_n) break;
		if (c + 1u < s->heap_n && heap_less(s, s->heap[c + 1u], s->heap[c])) c++;
		if (!heap_less(s, s->heap[c], v)) break;
		s->heap[i] = s->heap[c]; s->hpos[s->heap[i]] = (uint16_t)i;
		i = c;
	}
	s->heap[i] = v; s->hpos[v] = (uint16_t)i;
}
static void heap_push(path_scratch *s, uint16_t v)
{
	s->heap[s->heap_n] = v;
	s->hpos[v] = (uint16_t)s->heap_n;
	s->heap_n++;
	heap_up(s, s->heap_n - 1u);
}
static uint16_t heap_pop(path_scratch *s)
{
	uint16_t top = s->heap[0];
	s->heap_n--;
	s->hpos[top] = PATH_NONE;
	if (s->heap_n) { s->heap[0] = s->heap[s->heap_n]; s->hpos[s->heap[0]] = 0; heap_down(s, 0); }
	return top;
}

/* smallest u8 not below v (v <= 0: 0) */
static uint32_t ceil_u8(float v)
{
	uint32_t u;
	if (!(v > 0.0f)) return 0u;
	if (v >= 255.0f) return 255u;
	u = (uint32_t)v;
	return (float)u < v ? u + 1u : u;
}

static int link_usable(const path_bsp *b, const path_query *q, uint32_t need_head, uint32_t need_width, const uint8_t *l)
{
	uint32_t kind = l[2], to = rd16(l);
	if (!(q->kinds & PATH_KIND(kind))) return 0;
	if (l[3] < need_width) return 0;
	if (b->sectors[to * SECTOR_REC + 29u] < need_head) return 0;
	if (q->blocked && (q->blocked[to >> 5] >> (to & 31u) & 1u)) return 0;
	if (kind == PATH_LINK_DROP) {
		float dz = (float)(int16_t)rd16(l + 10) / DZ_SCALE;
		if (-dz > q->max_drop + 1e-4f) return 0;
	}
	return 1;
}

int path_find(const path_bsp *b, path_scratch *s, const path_query *q, uint32_t from, uint32_t to, path_route_info *info)
{
	path_route_info local;
	float gc[3], hscale;
	uint32_t n, best = from, best_h = 0xFFFFFFFFu, best_g = 0, budget, expanded = 0, goal_g = 0, need_head, need_width, need_thick;
	int reached = 0;
	path_route_info *inf = info ? info : &local;

	memset(inf, 0, sizeof *inf);
	inf->start_sector = (int32_t)from; inf->goal_sector = (int32_t)to; inf->end_sector = -1;
	inf->status = PATH_ROUTE_NO_START;
	s->route_links = 0;
	if (!b->data || from >= b->sector_count || to >= b->sector_count || s->cap < b->sector_count) return 0;
	n = b->sector_count;
	budget = q->max_expand ? q->max_expand : 8u * n + 64u;
	need_width = q->radius <= 0.0f ? 0u : 2.0f * q->radius * PATH_LINK_WIDTH_SCALE >= 255.0f ? 255u : (uint32_t)(2.0f * q->radius * PATH_LINK_WIDTH_SCALE);
	need_thick = q->radius <= 0.0f ? 0u : 2.0f * q->radius * PATH_THICK_SCALE >= 255.0f ? 255u : (uint32_t)(2.0f * q->radius * PATH_THICK_SCALE);
	need_head = ceil_u8(q->height * PATH_HEADROOM_SCALE);
	if (++s->stamp == 0u) { memset(s->seen, 0, 4u * n); s->stamp = 1u; }
	s->heap_n = 0;
	centroid_of(b, to, gc);
	hscale = (float)b->cost_scale * HEUR_MARGIN;
#define HEUR(sec, out) do { float c_[3], dx_, dy_, dz_; centroid_of(b, (sec), c_); dx_ = c_[0] - gc[0]; dy_ = c_[1] - gc[1]; \
	dz_ = c_[2] - gc[2]; (out) = (uint32_t)(sqrtf(dx_ * dx_ + dy_ * dy_ + dz_ * dz_) * hscale); } while (0)
	{ uint32_t h; HEUR(from, h); s->seen[from] = s->stamp; s->g[from] = 0; s->f[from] = h; s->parent[from] = PATH_NONE; s->plink[from] = 0;
	  s->hpos[from] = PATH_NONE; heap_push(s, (uint16_t)from); best_h = h; }
	while (s->heap_n) {
		uint16_t u = heap_pop(s);
		const uint8_t *r;
		uint32_t first, cnt, i, hu;
		if (u == to) { reached = 1; goal_g = s->g[u]; best = u; break; }
		if (expanded >= budget) break;
		expanded++;
		hu = s->f[u] - s->g[u];
		if ((hu < best_h || (hu == best_h && s->g[u] < best_g)) && sector_rec(b, u)[30] >= need_thick) { best = u; best_h = hu; best_g = s->g[u]; }
		r = sector_rec(b, u);
		first = rd32(r + 16); cnt = rd16(r + 20);
		for (i = 0; i < cnt; i++) {
			const uint8_t *l = b->links + (first + i) * LINK_REC;
			uint32_t v = rd16(l), ng = s->g[u] + rd16(l + 8);
			if (!link_usable(b, q, need_head, need_width, l)) continue;
			if (s->seen[v] != s->stamp) {
				uint32_t h;
				HEUR(v, h);
				s->seen[v] = s->stamp; s->g[v] = ng; s->f[v] = ng + h; s->parent[v] = u; s->plink[v] = first + i;
				s->hpos[v] = PATH_NONE;
				heap_push(s, (uint16_t)v);
			} else if (ng < s->g[v]) {
				uint32_t h = s->f[v] - s->g[v];
				s->g[v] = ng; s->f[v] = ng + h; s->parent[v] = u; s->plink[v] = first + i;
				if (s->hpos[v] != PATH_NONE) heap_up(s, s->hpos[v]); else heap_push(s, (uint16_t)v);
			}
		}
	}
#undef HEUR
	inf->expanded = expanded;
	/* route: best -> ... -> from through the parents (bounded: a chain is never longer than the sectors) */
	{
		uint32_t len = 0, v = best, k;
		while (v != from && len <= n) { v = s->parent[v]; len++; if (v == PATH_NONE) { len = n + 1u; break; } }
		if (len > n) { inf->status = PATH_ROUTE_PARTIAL; return 0; }   /* cannot happen with validated data */
		inf->end_sector = (int32_t)best;
		inf->cost = reached ? goal_g : s->g[best];
		inf->truncated = len > PATH_ROUTE_MAX;
		inf->sector_count = (len > PATH_ROUTE_MAX ? PATH_ROUTE_MAX : len) + 1u;
		/* fill from the end; entries beyond the buffer are skipped (the start of the route is kept) */
		v = best;
		for (k = len; ; k--) {
			if (k <= PATH_ROUTE_MAX) {
				s->route_sector[k] = (uint16_t)v;
				if (k > 0) s->route_link[k - 1u] = s->plink[v];
			}
			if (k == 0) break;
			v = s->parent[v];
		}
		s->route_links = inf->sector_count - 1u;
		if (inf->truncated) inf->end_sector = (int32_t)s->route_sector[inf->sector_count - 1u];
	}
	inf->status = reached && !inf->truncated ? PATH_ROUTE_COMPLETE : PATH_ROUTE_PARTIAL;
	return reached && !inf->truncated;
}

int path_sector_reachable(const path_bsp *b, path_scratch *s, const path_query *q, uint32_t from, uint32_t to)
{
	uint32_t cf, ct;
	if (!b->data || from >= b->sector_count || to >= b->sector_count) return 0;
	if (from == to) return 1;
	cf = rd16(sector_rec(b, from) + 24); ct = rd16(sector_rec(b, to) + 24);
	/* the components only know the symmetric links: different components can never be joined by them (blocking and headroom
	 * only remove links), the same component is certain only when nothing is removed */
	if (cf != ct && !(q->kinds & ~PATH_KINDS_SYMMETRIC)) return 0;
	if (cf == ct && !q->blocked && q->height <= 0.0f && (q->kinds & PATH_KINDS_SYMMETRIC) == PATH_KINDS_SYMMETRIC) return 1;
	return path_find(b, s, q, from, to, NULL);
}

static int locate_or_snap(const path_bsp *b, const float p[3], float snap, int32_t *out)
{
	int32_t sec = path_locate(b, p);
	if (sec < 0) sec = path_nearest_sector(b, p, snap, NULL, NULL);
	*out = sec;
	return sec >= 0;
}

int path_reachable(const path_bsp *b, path_scratch *s, const path_query *q, const float from[3], const float to[3])
{
	int32_t a, c;
	if (!locate_or_snap(b, from, q->snap_dist, &a) || !locate_or_snap(b, to, q->snap_dist, &c)) return 0;
	return path_sector_reachable(b, s, q, (uint32_t)a, (uint32_t)c);
}

/* ---- funnel ------------------------------------------------------------------------------------------------- */
typedef struct { path_waypoint *out; path_scratch *s; int max, n, trunc; } wpbuf;
#define CORNER_CAP (PATH_ROUTE_MAX + 4u)

static void wp_add(wpbuf *w, const float p[3], uint32_t sector, uint32_t kind, uint32_t flags)
{
	path_waypoint *o;
	if (w->n > 0) {                                           /* a repeat of the previous plain point merges into it */
		path_waypoint *l = &w->out[w->n - 1];
		float dx = l->pos[0] - p[0], dy = l->pos[1] - p[1], dz = l->pos[2] - p[2];
		if (dx * dx + dy * dy < 1e-8f && fabsf(dz) < 1e-3f && l->kind == PATH_LINK_WALK && !(l->flags & PATH_WPF_GOAL)) {
			l->kind = (uint8_t)(l->kind | kind);
			l->flags = (uint8_t)(l->flags | flags);
			l->sector = (uint16_t)sector;
			return;
		}
	}
	if (w->n >= w->max) { w->trunc = 1; return; }
	o = &w->out[w->n++];
	o->pos[0] = p[0]; o->pos[1] = p[1]; o->pos[2] = p[2];
	o->sector = (uint16_t)sector; o->kind = (uint8_t)kind; o->flags = (uint8_t)flags;
}

/* a corner of the taut path: the point wraps the portal end `c` (the unshrunk one) at distance r along the portal direction `u`
 * (per unit of plan length); s0 is the shrink it starts at, `smax` half the portal's plan width, both measured from `c` */
static void wp_corner(wpbuf *w, const float p[3], uint32_t sector, const float c[3], const float u[3], float s0, float smax)
{
	int before = w->n;
	wp_add(w, p, sector, PATH_LINK_WALK, 0);
	if (w->n > before && (uint32_t)before < CORNER_CAP) {
		float *a = w->s->corner[before];
		a[0] = c[0]; a[1] = c[1]; a[2] = c[2]; a[3] = u[0]; a[4] = u[1]; a[5] = u[2]; a[6] = s0; a[7] = smax;
		w->s->corner_valid[before] = 1;
	}
}

static float seg_dist_plan(const float a[3], const float b[3], const float p[3])
{
	float ex = b[0] - a[0], ey = b[1] - a[1], l2 = ex * ex + ey * ey, t = 0.0f, dx, dy;
	if (l2 > 1e-18f) {
		t = ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	}
	dx = a[0] + ex * t - p[0]; dy = a[1] + ey * t - p[1];
	return sqrtf(dx * dx + dy * dy);
}

/* The funnel pulls the path through the *shrunk* portal ends, which keeps a corner point r from the obstacle vertex but lets the
 * legs arriving at it graze the vertex when they come in at a shallow angle. Slide every corner along its portal, away from the
 * vertex, until both adjacent legs keep r from it (or the middle of the portal is reached). */
static void refine_corners(path_scratch *s, const float start[3], wpbuf *w, float r)
{
	int pass, i;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i + 1 < w->n; i++) {
			const float *a, *A, *B = w->out[i + 1].pos;
			float best, W[3], sv, s0, smax;
			int k;
			if ((uint32_t)i >= CORNER_CAP || !s->corner_valid[i]) continue;
			a = s->corner[i];
			A = i ? w->out[i - 1].pos : start;
			s0 = a[6]; smax = a[7] > a[6] ? a[7] : a[6];
			best = smax;
			for (k = 0; k <= 32; k++) {
				sv = s0 + (smax - s0) * (float)k / 32.0f;
				W[0] = a[0] + a[3] * sv; W[1] = a[1] + a[4] * sv; W[2] = a[2] + a[5] * sv;
				if (seg_dist_plan(A, W, a) >= r - 1e-4f && seg_dist_plan(W, B, a) >= r - 1e-4f) {
					float lo = sv - (smax - s0) / 32.0f, hi = sv;
					int it;
					if (k > 0) {                                        /* the first step that is clear: bisect back to the exact edge */
						for (it = 0; it < 12; it++) {
							float mid = 0.5f * (lo + hi), M[3];
							M[0] = a[0] + a[3] * mid; M[1] = a[1] + a[4] * mid; M[2] = a[2] + a[5] * mid;
							if (seg_dist_plan(A, M, a) >= r - 1e-4f && seg_dist_plan(M, B, a) >= r - 1e-4f) hi = mid; else lo = mid;
						}
					}
					best = hi;
					break;
				}
			}
			w->out[i].pos[0] = a[0] + a[3] * best; w->out[i].pos[1] = a[1] + a[4] * best; w->out[i].pos[2] = a[2] + a[5] * best;
		}
}

static float cr2(const float o[3], const float a[3], const float c[3])    /* cross of (a - o) and (c - o) in plan */
{
	return (a[0] - o[0]) * (c[1] - o[1]) - (a[1] - o[1]) * (c[0] - o[0]);
}
static int same2(const float a[3], const float c[3])
{
	float dx = a[0] - c[0], dy = a[1] - c[1];
	return dx * dx + dy * dy < 1e-10f;
}
/* How far a capsule of radius r must stay from the end `P` of a portal along the portal (unit plan direction d, pointing from P into
 * the portal): the boundary edges of `sec` that leave P at an acute angle theta to the portal need r / sin(theta) to keep r from
 * their line, anything else r from the vertex (capped at 4 r: a wedge that sharp is a collapsed portal for any actor). */
static float end_shrink(const path_bsp *b, uint32_t sec, const float P[3], const float d[2], float r)
{
	float poly[PATH_MAX_POLY][3], best = r;
	uint32_t n = load_poly(b, sec, poly), i, k;
	for (i = 0; i < n; i++)
		if (poly[i][0] == P[0] && poly[i][1] == P[1] && poly[i][2] == P[2]) break;
	if (i == n) return r;
	for (k = 0; k < 2u; k++) {
		const float *Q = poly[k ? (i + 1u) % n : (i + n - 1u) % n];
		float ex = Q[0] - P[0], ey = Q[1] - P[1], le = sqrtf(ex * ex + ey * ey), c, sn;
		if (le < 1e-6f) continue;
		c = (ex * d[0] + ey * d[1]) / le;
		if (c <= 0.0f || c > 0.9999f) continue;               /* obtuse, or the portal edge itself */
		sn = sqrtf(1.0f - c * c);
		if (sn < 0.25f) sn = 0.25f;
		if (r / sn > best) best = r / sn;
	}
	return best;
}

static void shrink_portal(const float R[3], const float L[3], float sr, float sl, float oR[3], float oL[3])
{
	float dx = L[0] - R[0], dy = L[1] - R[1], w = sqrtf(dx * dx + dy * dy), tr, tl;
	int k;
	if (w <= sr + sl + 1e-4f) {
		for (k = 0; k < 3; k++) { oR[k] = 0.5f * (R[k] + L[k]); oL[k] = oR[k]; }
		return;
	}
	tr = sr / w; tl = sl / w;
	for (k = 0; k < 3; k++) {
		oR[k] = R[k] + (L[k] - R[k]) * tr;
		oL[k] = L[k] - (L[k] - R[k]) * tl;
	}
}

/* Simple-stupid-funnel over s->pl / s->pr [1..np] between start and goal; corners go to w (the goal itself is the caller's). */
static void corner_at(path_scratch *s, wpbuf *w, uint32_t runstart, uint32_t idx, int left_end)
{
	const float *Ls = s->pl[idx], *Rs = s->pr[idx];
	float dx = Rs[0] - Ls[0], dy = Rs[1] - Ls[1], ls = sqrtf(dx * dx + dy * dy), u[3], c[3], sgn = left_end ? 1.0f : -1.0f;
	float s0 = s->shr[idx][left_end ? 1 : 0], other = s->shr[idx][left_end ? 0 : 1];
	const float *P = left_end ? Ls : Rs;
	uint32_t sec = s->route_sector[runstart + idx - 1u];
	if (ls < 1e-6f) { wp_add(w, P, sec, PATH_LINK_WALK, 0); return; }          /* a collapsed portal: its middle, nothing to slide */
	u[0] = sgn * (Rs[0] - Ls[0]) / ls; u[1] = sgn * (Rs[1] - Ls[1]) / ls; u[2] = sgn * (Rs[2] - Ls[2]) / ls;
	c[0] = P[0] - u[0] * s0; c[1] = P[1] - u[1] * s0; c[2] = P[2] - u[2] * s0;
	wp_corner(w, P, sec, c, u, s0, 0.5f * (ls + s0 + other));
}

static void run_funnel(path_scratch *s, uint32_t np, uint32_t runstart, const float start[3], const float goal[3], wpbuf *w)
{
	float apex[3], left[3], right[3];
	uint32_t apexI = 0, leftI = 0, rightI = 0, i, guard = 0, limit = (np + 2u) * (np + 2u) + 16u;
	memcpy(s->pl[0], start, 12); memcpy(s->pr[0], start, 12);
	memcpy(s->pl[np + 1u], goal, 12); memcpy(s->pr[np + 1u], goal, 12);
	memcpy(apex, start, 12); memcpy(left, start, 12); memcpy(right, start, 12);
	for (i = 1; i <= np + 1u; i++) {
		const float *nl = s->pl[i], *nr = s->pr[i];
		if (++guard > limit) break;
		if (cr2(apex, right, nr) >= 0.0f) {                     /* the right edge tightens */
			if (same2(apex, right) || cr2(apex, left, nr) < 0.0f) {
				memcpy(right, nr, 12); rightI = i;
			} else {                                            /* it crossed the left edge: the left end is a corner */
				if (leftI >= 1u) corner_at(s, w, runstart, leftI, 1);
				memcpy(apex, left, 12); apexI = leftI;
				memcpy(right, apex, 12); rightI = apexI; leftI = apexI;
				i = apexI;
				continue;
			}
		}
		if (cr2(apex, left, nl) <= 0.0f) {                      /* the left edge tightens */
			if (same2(apex, left) || cr2(apex, right, nl) > 0.0f) {
				memcpy(left, nl, 12); leftI = i;
			} else {
				if (rightI >= 1u) corner_at(s, w, runstart, rightI, 0);
				memcpy(apex, right, 12); apexI = rightI;
				memcpy(left, apex, 12); leftI = apexI; rightI = apexI;
				i = apexI;
				continue;
			}
		}
	}
}

int path_route(const path_bsp *b, path_scratch *s, const path_query *q, const float from[3], const float to[3],
               path_waypoint *out, int max, path_route_info *info)
{
	path_route_info local;
	path_route_info *inf = info ? info : &local;
	wpbuf w;
	int32_t sa, sb;
	float cur[3], cur0[3], goal[3], gfloor;
	uint32_t i, np = 0, runstart = 0, last;

	memset(inf, 0, sizeof *inf);
	inf->start_sector = inf->goal_sector = inf->end_sector = -1;
	w.out = out; w.s = s; w.max = max; w.n = 0; w.trunc = 0;
	if (!b->data || max <= 0 || !s || s->cap < b->sector_count) { inf->status = PATH_ROUTE_NO_START; return 0; }
	if (!locate_or_snap(b, from, q->snap_dist, &sa)) { inf->status = PATH_ROUTE_NO_START; return 0; }
	if (!locate_or_snap(b, to, q->snap_dist, &sb)) { inf->status = PATH_ROUTE_NO_GOAL; inf->start_sector = sa; return 0; }
	memset(s->corner_valid, 0, sizeof s->corner_valid);
	path_find(b, s, q, (uint32_t)sa, (uint32_t)sb, inf);
	inf->start_sector = sa; inf->goal_sector = sb;
	{ float z0;
	  cur[0] = from[0]; cur[1] = from[1];
	  z0 = path_sector_height(b, (uint32_t)sa, from[0], from[1]);
	  cur[2] = z0;
	  if (path_locate(b, from) != sa) { float pt[3]; sector_closest(b, (uint32_t)sa, from, pt); memcpy(cur, pt, 12); } }
	memcpy(cur0, cur, 12);
	for (i = 0; i < s->route_links; i++) {
		path_link L;
		uint32_t kind;
		float R[3], Lf[3];
		link_decode(b, s->route_link[i], &L);
		kind = L.kind;
		{ float dxp = L.left[0] - L.right[0], dyp = L.left[1] - L.right[1], wp_ = sqrtf(dxp * dxp + dyp * dyp), dd[2], sr = q->radius, sl = q->radius;
		  if (wp_ > 1e-6f) {
			  dd[0] = dxp / wp_; dd[1] = dyp / wp_;
			  sr = end_shrink(b, s->route_sector[i], L.right, dd, q->radius);
			  { float e2 = end_shrink(b, s->route_sector[i + 1u], L.right, dd, q->radius); if (e2 > sr) sr = e2; }
			  dd[0] = -dd[0]; dd[1] = -dd[1];
			  sl = end_shrink(b, s->route_sector[i], L.left, dd, q->radius);
			  { float e2 = end_shrink(b, s->route_sector[i + 1u], L.left, dd, q->radius); if (e2 > sl) sl = e2; }
		  }
		  shrink_portal(L.right, L.left, sr, sl, R, Lf);
		  s->shr[np + 1u][0] = sr; s->shr[np + 1u][1] = sl; }
		if (kind == PATH_LINK_WALK || kind == PATH_LINK_STEP) {
			np++;
			memcpy(s->pr[np], R, 12); memcpy(s->pl[np], Lf, 12);
			continue;
		}
		{ float take[3], land[3];                               /* an action ends the smoothing run */
		  take[0] = 0.5f * (R[0] + Lf[0]); take[1] = 0.5f * (R[1] + Lf[1]); take[2] = 0.5f * (R[2] + Lf[2]);
		  run_funnel(s, np, runstart, cur, take, &w);
		  wp_add(&w, take, s->route_sector[i], kind, 0);
		  path_snap(b, s->route_sector[i + 1u], take, q->radius, land);
		  wp_add(&w, land, s->route_sector[i + 1u], PATH_LINK_WALK, PATH_WPF_LANDING);
		  memcpy(cur, land, 12);
		  np = 0; runstart = i + 1u; }
	}
	last = s->route_sector[inf->sector_count - 1u];
	if (!inf->truncated && inf->status == PATH_ROUTE_COMPLETE && path_locate(b, to) == sb) {
		goal[0] = to[0]; goal[1] = to[1];
		goal[2] = path_locate_ex(b, to, PATH_LOCATE_BELOW, PATH_LOCATE_ABOVE, &gfloor) >= 0 ? gfloor : path_sector_height(b, last, to[0], to[1]);
	} else {
		path_snap(b, last, to, q->radius, goal);
	}
	run_funnel(s, np, runstart, cur, goal, &w);
	wp_add(&w, goal, last, PATH_LINK_WALK, PATH_WPF_GOAL);
	refine_corners(s, cur0, &w, q->radius);
	if (w.trunc) inf->truncated = 1;
	return w.n;
}
