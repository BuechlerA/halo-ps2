/* COLn section reader (docs/15-coll-section.md). Pure C11, no heap, no double, no recursion.
 * coll_open validates every table once (sizes, alignment, ranges, tree order, finite and sane floats), so the
 * queries can never read out of bounds, whatever the bytes. Records are decoded with byte loads (no alignment
 * assumption on the buffer) unless COLL_ALIGNED_LOADS is defined. Every walk uses a fixed stack and a visit budget:
 * corrupt data gives wrong answers, never a crash or an endless loop. */
#include "world/coll.h"
#include <math.h>
#include <string.h>

#define HEADER_SIZE 128u
#define DIR_AT 0x30u
#define NODE_REC 8u
#define PLANE_REC 16u
#define LEAF_REC 4u
#define REF_REC 4u
#define N2_REC 16u
#define SURF_REC 8u
#define INDEX_REC 2u
#define VERT_REC 12u
#define CHILD_NONE 0xFFFFFFu
#define CHILD_LEAF 0x800000u
#define MAX_NODES 0x7FFFFFu
#define MAX_COORD 65536.0f

enum { T_NODES, T_PLANES, T_LEAVES, T_REFS, T_NODES2D, T_SURFACES, T_INDICES, T_VERTICES, T_COUNT };

/* tolerances, world units */
#define EPS_RAY 4e-3f          /* a ray hit may lie this far outside its leaf's part of the segment (planes vs polygons: mm) */
#define EPS_INSIDE 1e-4f       /* polygon containment of a sphere contact point */
#define EPS_BEHIND 1e-3f       /* a sphere centre this far behind a face still counts as in front */
#define EPS_WALK 4e-3f         /* margin of the capsule-versus-plane tests while walking the tree (planes vs polygons: mm) */
#define EPS_RAY_INSIDE 5e-4f   /* a ray hit may lie this far outside the polygon of the surface the 2D BSP names */
#define EPS_PROBE 1e-3f        /* how far past a face along the ray a hit looks for solid space (a sliver leaf is thinner) */
#define EPS_TIE 1e-6f          /* a later contact must beat the best by this much (faces win ties against edges) */

#ifdef COLL_ALIGNED_LOADS
static uint32_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
#else
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
#endif

static int finite_at(const uint8_t *p) { return (rd32(p) & 0x7F800000u) != 0x7F800000u; }
static int sane_at(const uint8_t *p) { float f = rdf(p); return finite_at(p) && f >= -MAX_COORD && f <= MAX_COORD; }

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(float o[3], const float a[3], const float b[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void cross3(float o[3], const float a[3], const float b[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static int table_ok(uint32_t off, uint64_t count, uint32_t rec, size_t size)
{
	if (off % 16u || off < HEADER_SIZE || off > size) return 0;
	return count * rec <= (uint64_t)size - off;
}

/* ---- opening ------------------------------------------------------------------------------------------- */
coll_status coll_open(const uint8_t *data, size_t size, coll_bsp *out)
{
	static const uint32_t rec[T_COUNT] = { NODE_REC, PLANE_REC, LEAF_REC, REF_REC, N2_REC, SURF_REC, INDEX_REC, VERT_REC };
	uint32_t off[T_COUNT], cnt[T_COUNT], i, k;
	const uint8_t *t[T_COUNT];

	memset(out, 0, sizeof *out);
	if (!data || size < HEADER_SIZE) return COLL_ERR_TRUNCATED;
	if (memcmp(data, "COLL", 4) != 0) return COLL_ERR_MAGIC;
	if (rd16(data + 4) != COLL_VERSION) return COLL_ERR_VERSION;
	if (rd32(data + 8) > size) return COLL_ERR_TRUNCATED;
	if (rd32(data + 8) != size) return COLL_ERR_LAYOUT;
	if (rd32(data + 0x0C) > COLL_STACK_3D || rd32(data + 0x28) > COLL_STACK_2D) return COLL_ERR_LAYOUT;
	for (i = 0; i < 6; i++)
		if (!sane_at(data + 0x10 + 4 * i)) return COLL_ERR_LAYOUT;
	for (i = 0; i < T_COUNT; i++) {
		off[i] = rd32(data + DIR_AT + 8 * i);
		cnt[i] = rd32(data + DIR_AT + 8 * i + 4);
		if (!table_ok(off[i], (uint64_t)cnt[i] + (i == T_LEAVES ? 1u : 0u), rec[i], size)) return COLL_ERR_LAYOUT;
		t[i] = data + off[i];
	}
	if (cnt[T_NODES] >= MAX_NODES || cnt[T_LEAVES] >= MAX_NODES || cnt[T_PLANES] > 0x8000u || cnt[T_NODES2D] > 0x8000u ||
	    cnt[T_SURFACES] > 0x8000u || cnt[T_VERTICES] > 0x10000u || cnt[T_INDICES] > 0xFFFFFu)
		return COLL_ERR_LAYOUT;

	for (k = 0; k < cnt[T_PLANES]; k++) {
		const uint8_t *p = t[T_PLANES] + (size_t)k * PLANE_REC;
		float n[3], l2;
		if (!sane_at(p) || !sane_at(p + 4) || !sane_at(p + 8) || !sane_at(p + 12)) return COLL_ERR_LAYOUT;
		n[0] = rdf(p); n[1] = rdf(p + 4); n[2] = rdf(p + 8);
		l2 = dot3(n, n);
		if (!(l2 > 0.98f && l2 < 1.02f)) return COLL_ERR_LAYOUT;       /* unit normals (retail: 1 +- 1.5e-7) */
	}
	for (k = 0; k < cnt[T_VERTICES]; k++) {
		const uint8_t *p = t[T_VERTICES] + (size_t)k * VERT_REC;
		if (!sane_at(p) || !sane_at(p + 4) || !sane_at(p + 8)) return COLL_ERR_LAYOUT;
	}
	for (k = 0; k < cnt[T_NODES]; k++) {
		const uint8_t *p = t[T_NODES] + (size_t)k * NODE_REC;
		uint32_t w0 = rd32(p), w1 = rd32(p + 4), plane = (w0 >> 24) | (w1 >> 24) << 8, c[2];
		if (plane >= cnt[T_PLANES]) return COLL_ERR_LAYOUT;
		c[0] = w0 & 0xFFFFFFu;
		c[1] = w1 & 0xFFFFFFu;
		for (i = 0; i < 2; i++) {
			if (c[i] == CHILD_NONE) continue;
			if (c[i] & CHILD_LEAF) {
				if ((c[i] & ~CHILD_LEAF) >= cnt[T_LEAVES]) return COLL_ERR_LAYOUT;
			} else if (!(c[i] > k && c[i] < cnt[T_NODES])) {
				return COLL_ERR_LAYOUT;                                 /* children come later: no cycles */
			}
		}
	}
	{
		uint32_t prev = 0;
		for (k = 0; k <= cnt[T_LEAVES]; k++) {
			uint32_t v = rd32(t[T_LEAVES] + (size_t)k * LEAF_REC) & 0x7FFFFFFFu;
			if (v < prev || v > cnt[T_REFS]) return COLL_ERR_LAYOUT;
			prev = v;
		}
		if (prev != cnt[T_REFS]) return COLL_ERR_LAYOUT;
	}
	for (k = 0; k < cnt[T_REFS]; k++) {
		const uint8_t *p = t[T_REFS] + (size_t)k * REF_REC;
		uint32_t root = rd16(p + 2);
		if ((rd16(p) & 0x7FFFu) >= cnt[T_PLANES]) return COLL_ERR_LAYOUT;
		if (root & 0x8000u ? (root & 0x7FFFu) >= cnt[T_SURFACES] : root >= cnt[T_NODES2D]) return COLL_ERR_LAYOUT;
	}
	for (k = 0; k < cnt[T_NODES2D]; k++) {
		const uint8_t *p = t[T_NODES2D] + (size_t)k * N2_REC;
		if (!sane_at(p) || !sane_at(p + 4) || !sane_at(p + 8)) return COLL_ERR_LAYOUT;
		for (i = 0; i < 2; i++) {
			uint32_t c = rd16(p + 12 + 2 * i);
			if (c & 0x8000u) {
				if ((c & 0x7FFFu) >= cnt[T_SURFACES]) return COLL_ERR_LAYOUT;
			} else if (!(c > k && c < cnt[T_NODES2D])) {
				return COLL_ERR_LAYOUT;
			}
		}
	}
	for (k = 0; k < cnt[T_SURFACES]; k++) {
		const uint8_t *p = t[T_SURFACES] + (size_t)k * SURF_REC;
		uint32_t w = rd32(p), first = w & 0xFFFFFu, nv = (w >> 20) & 15u;
		if ((w >> 29) != 0 || p[6] > 15u) return COLL_ERR_LAYOUT;        /* reserved bits stay zero */
		if (nv < 3 || first + nv > cnt[T_INDICES] || (rd16(p + 4) & 0x7FFFu) >= cnt[T_PLANES]) return COLL_ERR_LAYOUT;
	}
	for (k = 0; k < cnt[T_INDICES]; k++)
		if (rd16(t[T_INDICES] + (size_t)k * INDEX_REC) >= cnt[T_VERTICES]) return COLL_ERR_LAYOUT;

	out->data = data;
	out->size = size;
	out->node_count = cnt[T_NODES];
	out->plane_count = cnt[T_PLANES];
	out->leaf_count = cnt[T_LEAVES];
	out->ref_count = cnt[T_REFS];
	out->node2d_count = cnt[T_NODES2D];
	out->surface_count = cnt[T_SURFACES];
	out->index_count = cnt[T_INDICES];
	out->vertex_count = cnt[T_VERTICES];
	out->depth = rd32(data + 0x0C);
	for (i = 0; i < 6; i++) out->bounds[i] = rdf(data + 0x10 + 4 * i);
	out->nodes = t[T_NODES];
	out->planes = t[T_PLANES];
	out->leaves = t[T_LEAVES];
	out->refs = t[T_REFS];
	out->nodes2d = t[T_NODES2D];
	out->surfaces = t[T_SURFACES];
	out->indices = t[T_INDICES];
	out->vertices = t[T_VERTICES];
	return COLL_OK;
}

/* ---- decoding (valid after coll_open) ------------------------------------------------------------------- */
static void load_plane(const coll_bsp *b, uint32_t i, float n[3], float *d)
{
	const uint8_t *p = b->planes + (size_t)i * PLANE_REC;
	n[0] = rdf(p); n[1] = rdf(p + 4); n[2] = rdf(p + 8);
	*d = rdf(p + 12);
}

static void node_get(const coll_bsp *b, uint32_t i, uint32_t *plane, uint32_t *front, uint32_t *back)
{
	const uint8_t *p = b->nodes + (size_t)i * NODE_REC;
	uint32_t w0 = rd32(p), w1 = rd32(p + 4);
	*plane = (w0 >> 24) | (w1 >> 24) << 8;
	*front = w0 & 0xFFFFFFu;
	*back = w1 & 0xFFFFFFu;
}

static void leaf_range(const coll_bsp *b, uint32_t leaf, uint32_t *r0, uint32_t *r1, int *two_sided)
{
	uint32_t w = rd32(b->leaves + (size_t)leaf * LEAF_REC);
	*r0 = w & 0x7FFFFFFFu;
	*r1 = rd32(b->leaves + (size_t)(leaf + 1) * LEAF_REC) & 0x7FFFFFFFu;
	*two_sided = (int)(w >> 31);
}

typedef struct { uint32_t first, nverts, plane, flip, flags, material, breakable; } surf;

static void surf_get(const coll_bsp *b, uint32_t s, surf *o)
{
	const uint8_t *p = b->surfaces + (size_t)s * SURF_REC;
	uint32_t w = rd32(p), pf = rd16(p + 4);
	o->first = w & 0xFFFFFu;
	o->nverts = (w >> 20) & 15u;
	o->breakable = (w >> 24) & 31u;
	o->plane = pf & 0x7FFFu;
	o->flip = pf >> 15;
	o->flags = p[6];
	o->material = p[7];
}

/* the plane of a surface oriented to its front: n.p = d, n points to the empty side */
static void surf_plane(const coll_bsp *b, const surf *s, float n[3], float *d)
{
	load_plane(b, s->plane, n, d);
	if (s->flip) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; *d = -*d; }
}

static void surf_poly(const coll_bsp *b, const surf *s, float P[][3])
{
	uint32_t i;
	for (i = 0; i < s->nverts; i++) {
		const uint8_t *v = b->vertices + (size_t)rd16(b->indices + (size_t)(s->first + i) * INDEX_REC) * VERT_REC;
		P[i][0] = rdf(v); P[i][1] = rdf(v + 4); P[i][2] = rdf(v + 8);
	}
}

/* 2D projection of a reference (docs/15): drop the dominant axis of the plane as stored (later axis wins ties),
 * swap the remaining two when the oriented dominant component is negative */
static void proj_axes(const float n[3], uint32_t flip, int *ua, int *va)
{
	int d = fabsf(n[1]) >= fabsf(n[0]) ? 1 : 0, u, v;
	if (fabsf(n[2]) >= fabsf(n[d])) d = 2;
	u = (d + 1) % 3;
	v = (d + 2) % 3;
	if ((n[d] > 0.0f) == (flip != 0)) { int tmp = u; u = v; v = tmp; }
	*ua = u;
	*va = v;
}

/* surface that the 2D BSP below `root` assigns to (u, v); -1 only for corrupt data */
static int32_t lookup2d(const coll_bsp *b, uint32_t root, float u, float v)
{
	uint32_t c = root, steps = 0;
	while (!(c & 0x8000u)) {
		const uint8_t *p;
		if (c >= b->node2d_count || ++steps > b->node2d_count) return -1;
		p = b->nodes2d + (size_t)c * N2_REC;
		c = rd16(p + 12 + (rdf(p) * u + rdf(p + 4) * v - rdf(p + 8) >= 0.0f ? 0 : 2));
	}
	return (int32_t)(c & 0x7FFFu);
}

static uint32_t material_of(const surf *s) { return s->material == 0xFFu ? COLL_NO_MATERIAL : s->material; }

/* ---- public helpers -------------------------------------------------------------------------------------- */
int32_t coll_locate_leaf(const coll_bsp *b, const float p[3])
{
	uint32_t c = 0, steps = 0;
	if (b->node_count == 0) return b->leaf_count ? 0 : -1;
	for (;;) {
		uint32_t plane, front, back, next;
		float n[3], d;
		if (++steps > b->node_count) return -1;
		node_get(b, c, &plane, &front, &back);
		load_plane(b, plane, n, &d);
		next = dot3(n, p) - d >= 0.0f ? front : back;
		if (next == CHILD_NONE) return -1;
		if (next & CHILD_LEAF) return (int32_t)(next & ~CHILD_LEAF);
		c = next;
	}
}

int coll_point_in_solid(const coll_bsp *b, const float p[3]) { return coll_locate_leaf(b, p) < 0; }

uint32_t coll_surface_polygon(const coll_bsp *b, int32_t surface, float out[][3])
{
	surf s;
	if (surface < 0 || (uint32_t)surface >= b->surface_count) return 0;
	surf_get(b, (uint32_t)surface, &s);
	surf_poly(b, &s, out);
	return s.nverts;
}

int coll_surface_info(const coll_bsp *b, int32_t surface, float plane[4], uint16_t *material, uint16_t *flags,
                      uint16_t *breakable)
{
	surf s;
	if (surface < 0 || (uint32_t)surface >= b->surface_count) return 0;
	surf_get(b, (uint32_t)surface, &s);
	surf_plane(b, &s, plane, &plane[3]);
	if (material) *material = (uint16_t)material_of(&s);
	if (flags) *flags = (uint16_t)s.flags;
	if (breakable) *breakable = (uint16_t)s.breakable;
	return 1;
}

uint32_t coll_leaf_ref_count(const coll_bsp *b, uint32_t leaf)
{
	uint32_t r0, r1;
	int two;
	if (leaf >= b->leaf_count) return 0;
	leaf_range(b, leaf, &r0, &r1, &two);
	return r1 - r0;
}

/* ---- polygon helpers -------------------------------------------------------------------------------------- */
/* X (in the polygon's plane) inside a convex polygon wound counter-clockwise about n, within eps of its boundary */
static int poly_contains(float P[][3], uint32_t nv, const float n[3], const float X[3], float eps)
{
	uint32_t i;
	for (i = 0; i < nv; i++) {
		const float *p0 = P[i], *p1 = P[i + 1 == nv ? 0 : i + 1];
		float e[3], w[3], c[3], cd;
		sub3(e, p1, p0);
		sub3(w, X, p0);
		cross3(c, e, w);
		cd = dot3(c, n);
		if (cd < 0.0f && cd * cd > eps * eps * dot3(e, e)) return 0;
	}
	return 1;
}

/* closest point of segment V0 V1 to X: writes it, returns the squared distance */
static float seg_closest(const float V0[3], const float V1[3], const float X[3], float out[3])
{
	float e[3], w[3], l2, s, dx, dy, dz;
	sub3(e, V1, V0);
	sub3(w, X, V0);
	l2 = dot3(e, e);
	s = l2 > 1e-12f ? dot3(w, e) / l2 : 0.0f;
	if (s < 0.0f) s = 0.0f;
	if (s > 1.0f) s = 1.0f;
	out[0] = V0[0] + s * e[0]; out[1] = V0[1] + s * e[1]; out[2] = V0[2] + s * e[2];
	dx = X[0] - out[0]; dy = X[1] - out[1]; dz = X[2] - out[2];
	return dx * dx + dy * dy + dz * dz;
}

/* ---- rays ------------------------------------------------------------------------------------------------ */
typedef struct { uint32_t child; float t0, t1; } ray_entry;

static void fill_hit(const coll_bsp *b, coll_hit *h, uint32_t s, float t, const float normal[3], const float point[3])
{
	surf sf;
	surf_get(b, s, &sf);
	h->t = t;
	h->normal[0] = normal[0]; h->normal[1] = normal[1]; h->normal[2] = normal[2];
	h->point[0] = point[0]; h->point[1] = point[1]; h->point[2] = point[2];
	h->material = (uint16_t)material_of(&sf);
	h->flags = (uint16_t)sf.flags;
	h->surface = (int32_t)s;
	h->breakable = (uint16_t)sf.breakable;
	h->pad = 0;
}

/* The references of a leaf: the segment a + t d, t in [t0, t1] is the part inside the leaf. A front-to-back crossing
 * of a reference's plane is a hit when it lies in that part (within te) and the 2D BSP gives a surface that is not
 * ignored and is a boundary of solid space or lies on the surface polygon. Returns 1 when `best` was set. */
static int ray_leaf(const coll_bsp *b, uint32_t leaf, const float a[3], const float d[3], const float dir[3], float t0,
                    float t1, float te, uint32_t ignore, coll_hit *best)
{
	uint32_t r0, r1, r;
	int two, found = 0;
	leaf_range(b, leaf, &r0, &r1, &two);
	(void)two;                      /* the leaf's two-sided flag is not needed: both halves of a pair are listed */
	for (r = r0; r < r1; r++) {
		const uint8_t *p = b->refs + (size_t)r * REF_REC;
		uint32_t pf = rd16(p), root = rd16(p + 2), flip = pf >> 15;
		float n[3], dd, sg = flip ? -1.0f : 1.0f, ea, eb, ts, x[3], nn[3];
		int ua, va;
		int32_t s;
		surf sf;
		load_plane(b, pf & 0x7FFFu, n, &dd);
		ea = sg * (dot3(n, a) - dd);
		eb = ea + sg * dot3(n, d);
		if (!(ea >= 0.0f && eb < 0.0f)) continue;
		ts = ea / (ea - eb);
		if (ts < t0 - te || ts > t1 + te) continue;
		if (ts < 0.0f) ts = 0.0f;
		if (ts > 1.0f) ts = 1.0f;
		if (found && ts >= best->t) continue;
		x[0] = a[0] + ts * d[0]; x[1] = a[1] + ts * d[1]; x[2] = a[2] + ts * d[2];
		proj_axes(n, flip, &ua, &va);
		s = lookup2d(b, root, x[ua], x[va]);
		if (s < 0) continue;
		surf_get(b, (uint32_t)s, &sf);
		if (sf.flags & ignore) continue;
		/* The 2D BSP has no empty leaves (every leaf is a surface), so a crossing of the plane at a point of the leaf's
		 * face that is open to the next leaf still gets a surface from it. The crossing is a hit when it lies in that
		 * surface's polygon, or when solid space follows it along the ray (the face of the leaf then is a solid boundary
		 * even where the polygon is a hair away: the tree's planes and the surface planes of the Xbox data differ by
		 * up to a millimetre in places). */
		nn[0] = sg * n[0]; nn[1] = sg * n[1]; nn[2] = sg * n[2];                                /* front normal */
		{
			float P[COLL_MAX_POLY][3];
			surf_poly(b, &sf, P);
			if (!poly_contains(P, sf.nverts, nn, x, EPS_RAY_INSIDE)) {
				float ahead[3] = { x[0] + dir[0] * EPS_PROBE, x[1] + dir[1] * EPS_PROBE, x[2] + dir[2] * EPS_PROBE };
				if (!coll_point_in_solid(b, ahead)) continue;
			}
		}
		fill_hit(b, best, (uint32_t)s, ts, nn, x);
		found = 1;
	}
	return found;
}

int coll_ray(const coll_bsp *b, const float a[3], const float b_[3], uint32_t ignore_flags, coll_hit *h)
{
	ray_entry st[COLL_STACK_3D];
	uint32_t sp = 0, child = 0, budget = 2 * (b->node_count + b->leaf_count) + 16;
	float d[3], dir[3], t0 = 0.0f, t1 = 1.0f, len, te;
	coll_hit best;

	if (b->node_count == 0) return 0;
	sub3(d, b_, a);
	len = sqrtf(dot3(d, d));
	if (!(len > 1e-9f)) return 0;
	te = EPS_RAY / len;
	dir[0] = d[0] / len; dir[1] = d[1] / len; dir[2] = d[2] / len;
	for (;;) {
		if (child != CHILD_NONE && budget) {
			budget--;
			if (child & CHILD_LEAF) {
				if (ray_leaf(b, child & ~CHILD_LEAF, a, d, dir, t0, t1, te, ignore_flags, &best)) {
					*h = best;
					return 1;
				}
			} else {
				uint32_t plane, front, back;
				float n[3], dd, da, dn, s0, s1;
				node_get(b, child, &plane, &front, &back);
				load_plane(b, plane, n, &dd);
				da = dot3(n, a) - dd;
				dn = dot3(n, d);
				s0 = da + t0 * dn;
				s1 = da + t1 * dn;
				if (s0 >= 0.0f && s1 >= 0.0f) { child = front; continue; }
				if (s0 < 0.0f && s1 < 0.0f) { child = back; continue; }
				{
					float tc = t0 + (t1 - t0) * (s0 / (s0 - s1));
					uint32_t near_c = s0 >= 0.0f ? front : back, far_c = s0 >= 0.0f ? back : front;
					if (tc < t0) tc = t0;
					if (tc > t1) tc = t1;
					if (far_c != CHILD_NONE && sp < COLL_STACK_3D) {
						st[sp].child = far_c; st[sp].t0 = tc; st[sp].t1 = t1;
						sp++;
					}
					child = near_c;
					t1 = tc;
					continue;
				}
			}
		}
		if (!sp || !budget) return 0;
		sp--;
		child = st[sp].child; t0 = st[sp].t0; t1 = st[sp].t1;
	}
}

/* ---- capsule walk: every surface of every leaf the swept sphere can touch ---------------------------------- */
typedef void (*surface_visit)(void *ctx, uint32_t surface);

/* Calls `visit` for the surfaces of the leaves that the capsule (centre a + t d, t in [0, *tmax], radius r) may touch.
 * *tmax may shrink while walking (sweeps): the walk re-reads it. A surface can be offered more than once; callers
 * dedupe. The references whose plane is farther than r from the whole capsule are skipped without opening their
 * 2D trees. */
static void walk_capsule(const coll_bsp *b, const float a[3], const float d[3], float r, const float *tmax,
                         surface_visit visit, void *ctx)
{
	uint32_t stack[COLL_STACK_3D], sp = 0, child = 0, budget = 2 * (b->node_count + b->leaf_count) + 16;
	float rm = r + EPS_WALK;

	if (b->node_count == 0) return;
	for (;;) {
		if (child != CHILD_NONE && budget) {
			budget--;
			if (child & CHILD_LEAF) {
				uint32_t r0, r1, ri;
				int two;
				leaf_range(b, child & ~CHILD_LEAF, &r0, &r1, &two);
				for (ri = r0; ri < r1; ri++) {
					const uint8_t *p = b->refs + (size_t)ri * REF_REC;
					uint32_t pf = rd16(p), root = rd16(p + 2);
					float n[3], dd, da, de, lo, hi;
					load_plane(b, pf & 0x7FFFu, n, &dd);
					da = dot3(n, a) - dd;
					de = da + *tmax * dot3(n, d);
					lo = da < de ? da : de;
					hi = da < de ? de : da;
					if (lo > rm || hi < -rm) continue;           /* the plane is out of reach of the capsule */
					if (root & 0x8000u) {
						visit(ctx, root & 0x7FFFu);
					} else {
						uint32_t st2[COLL_STACK_2D], sp2 = 0, steps = 0;
						st2[sp2++] = root;
						while (sp2 && steps++ <= b->node2d_count) {
							const uint8_t *q = b->nodes2d + (size_t)st2[--sp2] * N2_REC;
							uint32_t k;
							for (k = 0; k < 2; k++) {
								uint32_t c = rd16(q + 12 + 2 * k);
								if (c & 0x8000u) visit(ctx, c & 0x7FFFu);
								else if (sp2 < COLL_STACK_2D) st2[sp2++] = c;
							}
						}
					}
				}
			} else {
				uint32_t plane, front, back;
				float n[3], dd, da, de, lo, hi;
				int go_front, go_back;
				node_get(b, child, &plane, &front, &back);
				load_plane(b, plane, n, &dd);
				da = dot3(n, a) - dd;
				de = da + *tmax * dot3(n, d);
				lo = da < de ? da : de;
				hi = da < de ? de : da;
				go_front = hi >= -rm;
				go_back = lo < rm;
				if (go_front && go_back) {
					if (back != CHILD_NONE && sp < COLL_STACK_3D) stack[sp++] = back;
					child = front;
				} else {
					child = go_front ? front : back;
				}
				continue;
			}
		}
		if (!sp || !budget) return;
		child = stack[--sp];
	}
}

/* ---- sphere sweeps ---------------------------------------------------------------------------------------- */
typedef struct {
	const coll_bsp *b;
	float a[3], d[3], r;
	uint32_t ignore;
	float tmax;                    /* best contact so far (1 until something is found) */
	int found;
	float normal[3], point[3];     /* world space */
	uint32_t surface;
	uint32_t seen[64];             /* direct-mapped "already swept" cache, surface + 1 */
} sweep_q;

static void sweep_offer(sweep_q *q, float t, const float n[3], const float p_rel[3], uint32_t s)
{
	if (q->found ? !(t < q->tmax - EPS_TIE) : !(t <= q->tmax)) return;
	q->found = 1;
	q->tmax = t;
	q->normal[0] = n[0]; q->normal[1] = n[1]; q->normal[2] = n[2];
	q->point[0] = q->a[0] + p_rel[0]; q->point[1] = q->a[1] + p_rel[1]; q->point[2] = q->a[2] + p_rel[2];
	q->surface = s;
}

/* first contact of the point-sphere path c(t) = t d with the sphere of radius r around V (all relative to the start) */
static int path_vs_sphere(const float V[3], const float d[3], float r, float tmax, float *t)
{
	float A = dot3(d, d), B = -dot3(V, d), C = dot3(V, V) - r * r, disc, tt;
	if (C <= 0.0f) {                                  /* already touching or inside: contact now if moving closer */
		if (!(B < 0.0f)) return 0;
		*t = 0.0f;
		return 1;
	}
	if (B >= 0.0f || A < 1e-12f) return 0;
	disc = B * B - A * C;
	if (disc < 0.0f) return 0;
	tt = (-B - sqrtf(disc)) / A;
	if (tt > tmax) return 0;
	*t = tt < 0.0f ? 0.0f : tt;
	return 1;
}

/* first contact with the cylinder of radius r around the line V0 V1 (the contact must lie within the segment) */
static int path_vs_edge(const float V0[3], const float V1[3], const float d[3], float r, float tmax, float *t, float Q[3])
{
	float E[3], m[3], mp[3], dp[3], l2, k, k2, A, B, C, disc, tt, s;
	int i;
	sub3(E, V1, V0);
	l2 = dot3(E, E);
	if (l2 < 1e-12f) return 0;
	for (i = 0; i < 3; i++) m[i] = -V0[i];
	k = dot3(m, E) / l2;
	k2 = dot3(d, E) / l2;
	for (i = 0; i < 3; i++) { mp[i] = m[i] - E[i] * k; dp[i] = d[i] - E[i] * k2; }
	A = dot3(dp, dp);
	B = dot3(mp, dp);
	C = dot3(mp, mp) - r * r;
	if (A < 1e-12f || B >= 0.0f) return 0;
	if (C <= 0.0f) {
		tt = 0.0f;
	} else {
		disc = B * B - A * C;
		if (disc < 0.0f) return 0;
		tt = (-B - sqrtf(disc)) / A;
		if (tt > tmax) return 0;
		if (tt < 0.0f) tt = 0.0f;
	}
	s = k + tt * k2;
	if (s < 0.0f || s > 1.0f) return 0;
	for (i = 0; i < 3; i++) Q[i] = V0[i] + E[i] * s;
	*t = tt;
	return 1;
}

/* One edge or vertex contact candidate: the centre at the contact must be in front of the surface's plane (or on it) */
static void sweep_offer_feature(sweep_q *q, float t, const float Q[3], float sad, float dn, uint32_t s)
{
	float c[3], nc[3], len, sd = sad + dn * t;
	int k;
	for (k = 0; k < 3; k++) { c[k] = t * q->d[k]; nc[k] = c[k] - Q[k]; }
	len = sqrtf(dot3(nc, nc));
	if (sd < -EPS_BEHIND || !(len > 1e-9f)) return;
	nc[0] /= len; nc[1] /= len; nc[2] /= len;
	sweep_offer(q, t, nc, Q, s);
}

/* Surfaces are hit from the front only; a two-sided surface is a coincident pair of opposite surfaces (docs/15), each
 * listed by its own front leaf, so the back is covered by the twin. */
static void sweep_surface(sweep_q *q, uint32_t s)
{
	const coll_bsp *b = q->b;
	surf sf;
	float n[3], D, P[COLL_MAX_POLY][3], sad, dn, send, r = q->r;
	uint32_t i;

	surf_get(b, s, &sf);
	if (sf.flags & q->ignore) return;
	surf_plane(b, &sf, n, &D);
	sad = dot3(n, q->a) - D;                                             /* distance of the start to the plane */
	dn = dot3(n, q->d);
	send = sad + dn * q->tmax;
	if (sad > r + EPS_INSIDE && send > r + EPS_INSIDE) return;           /* never within reach of the plane */
	if (sad < -EPS_BEHIND && send < -EPS_BEHIND) return;                 /* always behind it */
	surf_poly(b, &sf, P);
	for (i = 0; i < sf.nverts; i++) sub3(P[i], P[i], q->a);            /* relative to the sphere's start */
	/* face: the centre reaches the plane offset by r while the contact point lies in the polygon */
	if (dn < -1e-9f) {
		float ts = sad >= r ? (r - sad) / dn : 0.0f;
		if (ts <= q->tmax) {
			float c[3] = { ts * q->d[0], ts * q->d[1], ts * q->d[2] }, sd = sad + dn * ts, foot[3];
			foot[0] = c[0] - n[0] * sd; foot[1] = c[1] - n[1] * sd; foot[2] = c[2] - n[2] * sd;
			if (poly_contains(P, sf.nverts, n, foot, EPS_INSIDE)) sweep_offer(q, ts, n, foot, s);
		}
	}
	/* edges and vertices */
	for (i = 0; i < sf.nverts; i++) {
		float t, Q[3];
		if (path_vs_sphere(P[i], q->d, r, q->tmax, &t)) sweep_offer_feature(q, t, P[i], sad, dn, s);
		if (path_vs_edge(P[i], P[i + 1 == sf.nverts ? 0 : i + 1], q->d, r, q->tmax, &t, Q)) sweep_offer_feature(q, t, Q, sad, dn, s);
	}
}

static void sweep_visit(void *ctx, uint32_t s)
{
	sweep_q *q = ctx;
	uint32_t slot = s & 63u;
	if (q->seen[slot] == s + 1u) return;
	q->seen[slot] = s + 1u;
	sweep_surface(q, s);
}

int coll_sphere_sweep_flags(const coll_bsp *b, const float a[3], const float b_[3], float radius, uint32_t ignore_flags,
                            coll_hit *h)
{
	sweep_q q;
	if (!(radius > 0.0f)) return coll_ray(b, a, b_, ignore_flags, h);
	memset(&q, 0, sizeof q);
	q.b = b;
	q.a[0] = a[0]; q.a[1] = a[1]; q.a[2] = a[2];
	sub3(q.d, b_, a);
	q.r = radius;
	q.ignore = ignore_flags;
	q.tmax = 1.0f;
	walk_capsule(b, q.a, q.d, radius, &q.tmax, sweep_visit, &q);
	if (!q.found) return 0;
	fill_hit(b, h, q.surface, q.tmax, q.normal, q.point);
	return 1;
}

int coll_sphere_sweep(const coll_bsp *b, const float a[3], const float b_[3], float radius, coll_hit *h)
{
	return coll_sphere_sweep_flags(b, a, b_, radius, 0, h);
}

/* ---- depenetration ---------------------------------------------------------------------------------------- */
typedef struct {
	const coll_bsp *b;
	float pos[3], r;
	uint32_t seen[64];
	int moved;
} overlap_q;

static void overlap_surface(overlap_q *q, uint32_t s)
{
	const coll_bsp *b = q->b;
	surf sf;
	float n0[3], D0, P[COLL_MAX_POLY][3], sd, r = q->r, n[3], dist, foot[3], push[3];
	uint32_t i;
	int face;

	surf_get(b, s, &sf);
	surf_plane(b, &sf, n0, &D0);
	sd = dot3(n0, q->pos) - D0;
	if (sd >= 0.0f || (sd > -r && !(sf.flags & COLL_F_TWO_SIDED))) {
		/* in front of the face; or (one-sided only) the centre sits in the solid behind it: out through the face. The
		 * twin of a two-sided surface faces the other way and takes care of that side. */
		n[0] = n0[0]; n[1] = n0[1]; n[2] = n0[2];
		dist = sd;
	} else {
		return;
	}
	if (dist >= r) return;
	surf_poly(b, &sf, P);
	for (i = 0; i < 3; i++) foot[i] = q->pos[i] - n0[i] * sd;           /* projection onto the plane */
	face = poly_contains(P, sf.nverts, n0, foot, 0.0f);
	if (face) {
		float depth = r - dist;
		push[0] = n[0] * depth; push[1] = n[1] * depth; push[2] = n[2] * depth;
	} else {
		float best = 1e30f, Q[3], bq[3] = { 0, 0, 0 }, dm, dir[3], len;
		if (sd < 0.0f) return;                                           /* behind a face, beside its edge */
		for (i = 0; i < sf.nverts; i++) {
			dm = seg_closest(P[i], P[i + 1 == sf.nverts ? 0 : i + 1], q->pos, Q);
			if (dm < best) { best = dm; bq[0] = Q[0]; bq[1] = Q[1]; bq[2] = Q[2]; }
		}
		if (!(best < r * r)) return;
		len = sqrtf(best);
		sub3(dir, q->pos, bq);
		if (len > 1e-6f) { dir[0] /= len; dir[1] /= len; dir[2] /= len; }
		else { dir[0] = n[0]; dir[1] = n[1]; dir[2] = n[2]; }
		push[0] = dir[0] * (r - len); push[1] = dir[1] * (r - len); push[2] = dir[2] * (r - len);
	}
	q->pos[0] += push[0]; q->pos[1] += push[1]; q->pos[2] += push[2];
	q->moved = 1;
}

static void overlap_visit(void *ctx, uint32_t s)
{
	overlap_q *q = ctx;
	uint32_t slot = s & 63u;
	if (q->seen[slot] == s + 1u) return;
	q->seen[slot] = s + 1u;
	overlap_surface(q, s);
}

int coll_sphere_overlap(const coll_bsp *b, const float c[3], float r, float push_out[3])
{
	overlap_q q;
	static const float zero[3] = { 0.0f, 0.0f, 0.0f };
	float tmax = 0.0f;
	int pass;

	push_out[0] = push_out[1] = push_out[2] = 0.0f;
	if (!(r > 0.0f)) return 0;
	memset(&q, 0, sizeof q);
	q.b = b;
	q.r = r;
	q.pos[0] = c[0]; q.pos[1] = c[1]; q.pos[2] = c[2];
	for (pass = 0; pass < 2; pass++) {
		float at[3] = { q.pos[0], q.pos[1], q.pos[2] };
		memset(q.seen, 0, sizeof q.seen);
		walk_capsule(b, at, zero, r, &tmax, overlap_visit, &q);
	}
	if (!q.moved) return 0;
	push_out[0] = q.pos[0] - c[0]; push_out[1] = q.pos[1] - c[1]; push_out[2] = q.pos[2] - c[2];
	return 1;
}
