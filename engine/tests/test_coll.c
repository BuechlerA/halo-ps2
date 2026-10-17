/* Host test of the COLn reader (docs/15-coll-section.md).
 *   test_coll <synthetic.hp2> [<real a10 coll pack> [<a10 BSPn pack>]]
 * 1. primitives (sphere / edge / polygon / projection helpers; this file includes coll.c to reach its statics),
 * 2. the synthetic two-room world of tools/tests/test_collconv.py (numbers hard-coded here): rays, sweeps, overlap,
 *    point location, flags, the two-sided pane, an empty section,
 * 3. hostile input on the synthetic section (every truncation, every byte flipped three ways),
 * 4. with a real pack (the converted a10): every BSP opens and its polygons are planar and wound to their planes; on
 *    BSP 0 coll_ray against a brute force over every surface polygon (agreement rate, every disagreement class),
 *    sphere sweeps that must never end (or pass through) solid space, depenetration, point-in-solid against the
 *    render tree of the BSPn section, random corruption of a real section;
 * 5. COLL_BENCH=1: queries per millisecond (build this file with -O2 and without the sanitizers for meaningful numbers). */
#include "stream/hp2.h"
#include "world/bsp.h"
#include "world/coll.c"      /* the reader itself: the statics are unit-tested below */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

static volatile float sink;

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frand(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }
static float frange(float lo, float hi) { return lo + (hi - lo) * frand(); }
static void rdir(float d[3])
{
	float l;
	do {
		d[0] = frange(-1, 1); d[1] = frange(-1, 1); d[2] = frange(-1, 1);
		l = sqrtf(dot3(d, d));
	} while (l < 0.1f || l > 1.0f);
	d[0] /= l; d[1] /= l; d[2] /= l;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ---- loading ------------------------------------------------------------------------------------------- */
static uint8_t *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *img;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	img = malloc(*n);
	if (!img || fread(img, 1, *n, f) != *n) { fclose(f); free(img); return NULL; }
	fclose(f);
	return img;
}

static uint8_t *load_section(const hp2_pack *pk, const char *tag, size_t *n)
{
	const hp2_section *sec = hp2_find(pk, tag);
	uint8_t *raw;
	if (!sec) return NULL;
	raw = malloc(sec->raw_size);
	if (!raw || hp2_load(pk, sec, raw, sec->raw_size) != HP2_OK) { free(raw); return NULL; }
	*n = sec->raw_size;
	return raw;
}

/* ---- touching everything an opened section hands out ------------------------------------------------------- */
static void walk(const coll_bsp *b)
{
	float acc = 0.0f, P[COLL_MAX_POLY][3], pl[4], lo[3], hi[3];
	uint32_t s, i, lim = b->surface_count < 4000 ? b->surface_count : 4000;
	uint16_t m, f, br;
	for (s = 0; s < lim; s++) {
		uint32_t n = coll_surface_polygon(b, (int32_t)s, P);
		if (n < 3 || n > COLL_MAX_POLY) abort();
		for (i = 0; i < n; i++) acc += P[i][0] + P[i][1] + P[i][2];
		if (!coll_surface_info(b, (int32_t)s, pl, &m, &f, &br)) abort();
		acc += pl[3] + (float)m + (float)f + (float)br;
	}
	for (i = 0; i < b->leaf_count && i < 20000; i++) acc += (float)coll_leaf_ref_count(b, i);
	for (i = 0; i < 3; i++) { lo[i] = b->bounds[i] - 1.0f; hi[i] = b->bounds[3 + i] + 1.0f; }
	for (i = 0; i < 60; i++) {
		float a[3], c[3], d[3], push[3];
		coll_hit h;
		int k;
		for (k = 0; k < 3; k++) { a[k] = frange(lo[k], hi[k]); c[k] = a[k] + frange(-6, 6); }
		rdir(d);
		acc += (float)coll_point_in_solid(b, a) + (float)coll_locate_leaf(b, a);
		if (coll_ray(b, a, c, 0, &h)) acc += h.t + h.normal[0] + h.point[1];
		if (coll_sphere_sweep(b, a, c, 0.3f, &h)) acc += h.t + h.normal[2] + h.point[0];
		if (coll_sphere_sweep_flags(b, a, c, 0.1f, COLL_F_INVISIBLE, &h)) acc += h.t;
		if (coll_sphere_overlap(b, a, 0.4f, push)) acc += push[0] + push[1] + push[2];
	}
	sink = acc;
}

/* ---- 1. primitives ---------------------------------------------------------------------------------------- */
static int test_primitives(void)
{
	float t, Q[3], d[3];

	/* sphere path: centre moves along +x from the origin, sphere of radius 0.5 around (3, 0, 0) is touched at x = 2.5 */
	{ float V[3] = { 3, 0, 0 }; d[0] = 4; d[1] = 0; d[2] = 0;
	  CHECK(path_vs_sphere(V, d, 0.5f, 1.0f, &t) && NEAR(t, 2.5f / 4.0f, 1e-6f));
	  CHECK(!path_vs_sphere(V, d, 0.5f, 0.5f, &t));                       /* not reached within tmax */
	  { float W[3] = { 3, 0.6f, 0 }; CHECK(!path_vs_sphere(W, d, 0.5f, 1.0f, &t)); }     /* passes by */
	  { float W[3] = { 3, 0.3f, 0 }; CHECK(path_vs_sphere(W, d, 0.5f, 1.0f, &t) && NEAR(3.0f - 0.4f, t * 4.0f, 1e-5f)); }
	  { float W[3] = { -3, 0, 0 }; CHECK(!path_vs_sphere(W, d, 0.5f, 1.0f, &t)); }       /* behind: moving away */
	  { float W[3] = { 0.2f, 0, 0 }; CHECK(path_vs_sphere(W, d, 0.5f, 1.0f, &t) && t == 0.0f); }   /* already inside, moving closer */
	  { float W[3] = { -0.2f, 0, 0 }; CHECK(!path_vs_sphere(W, d, 0.5f, 1.0f, &t)); } }          /* already inside, moving away */
	/* edge: the line through (2, -1, 0) and (2, 1, 0) (along y), sphere centre moving along +x; contact at x = 1.5 */
	{ float V0[3] = { 2, -1, 0 }, V1[3] = { 2, 1, 0 };
	  CHECK(path_vs_edge(V0, V1, d, 0.5f, 1.0f, &t, Q) && NEAR(t, 1.5f / 4.0f, 1e-6f) && NEAR(Q[0], 2, 1e-6f) && NEAR(Q[1], 0, 1e-6f));
	  { float W0[3] = { 2, 0.2f, 0 }, W1[3] = { 2, 1, 0 };                /* the contact (y = 0) is past the end of the segment */
	    CHECK(!path_vs_edge(W0, W1, d, 0.5f, 1.0f, &t, Q)); }
	  { float W0[3] = { 2, -1, 0.6f }, W1[3] = { 2, 1, 0.6f }; CHECK(!path_vs_edge(W0, W1, d, 0.5f, 1.0f, &t, Q)); }
	  { float E0[3] = { 0, -1, 1 }, E1[3] = { 5, -1, 1 }; CHECK(!path_vs_edge(E0, E1, d, 0.5f, 1.0f, &t, Q)); }   /* parallel */
	  { float W0[3] = { 1.8f, -1, 0 }, W1[3] = { 1.8f, 1, 0 };            /* starts 0.2 from the edge line, inside the cylinder */
	    float st[3] = { 0, 0, 0 }; (void)st;
	    CHECK(path_vs_edge(W0, W1, d, 0.5f, 1.0f, &t, Q) && NEAR(t, 1.3f / 4.0f, 1e-5f)); } }
	/* polygon containment, counter-clockwise about +z, with a tolerance band */
	{ float sq[4][3] = { { 0, 0, 0 }, { 2, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 } }, n[3] = { 0, 0, 1 };
	  float in[3] = { 1, 1, 0 }, edge[3] = { 2.00005f, 1, 0 }, out[3] = { 2.001f, 1, 0 }, corner[3] = { -0.00005f, -0.00005f, 0 };
	  CHECK(poly_contains(sq, 4, n, in, 0.0f) && poly_contains(sq, 4, n, edge, 1e-4f) && !poly_contains(sq, 4, n, out, 1e-4f));
	  CHECK(poly_contains(sq, 4, n, corner, 1e-4f) && !poly_contains(sq, 4, n, edge, 0.0f)); }
	/* closest point of a segment */
	{ float a[3] = { 0, 0, 0 }, b[3] = { 4, 0, 0 }, x[3] = { 5, 3, 0 }, q[3];
	  CHECK(NEAR(seg_closest(a, b, x, q), 10.0f, 1e-5f) && q[0] == 4.0f);
	  { float y[3] = { 1, 2, 0 }; CHECK(NEAR(seg_closest(a, b, y, q), 4.0f, 1e-5f) && q[0] == 1.0f); } }
	/* 2D projection axes: cyclic when the oriented dominant component is positive, later axis wins ties */
	{ int u, v; const float s = 0.70710678f;
	  float nx[3] = { 1, 0, 0 }, ny[3] = { 0, 1, 0 }, nz[3] = { 0, 0, 1 }, e1[3] = { s, s, 0 }, e2[3] = { s, 0, s }, e3[3] = { 0, s, s };
	  proj_axes(nx, 0, &u, &v); CHECK(u == 1 && v == 2);
	  proj_axes(nx, 1, &u, &v); CHECK(u == 2 && v == 1);
	  proj_axes(ny, 0, &u, &v); CHECK(u == 2 && v == 0);
	  proj_axes(nz, 0, &u, &v); CHECK(u == 0 && v == 1);
	  proj_axes(nz, 1, &u, &v); CHECK(u == 1 && v == 0);
	  proj_axes(e1, 0, &u, &v); CHECK(u == 2 && v == 0);        /* x/y tie: y */
	  proj_axes(e2, 0, &u, &v); CHECK(u == 0 && v == 1);        /* x/z tie: z */
	  proj_axes(e3, 0, &u, &v); CHECK(u == 0 && v == 1); }      /* y/z tie: z */
	return 0;
}

/* ---- 2. the synthetic world -------------------------------------------------------------------------------- */
static int ray(const coll_bsp *b, float ax, float ay, float az, float bx, float by, float bz, uint32_t ign, coll_hit *h)
{
	float a[3] = { ax, ay, az }, e[3] = { bx, by, bz };
	return coll_ray(b, a, e, ign, h);
}

static int sweep(const coll_bsp *b, float ax, float ay, float az, float bx, float by, float bz, float r, uint32_t ign, coll_hit *h)
{
	float a[3] = { ax, ay, az }, e[3] = { bx, by, bz };
	return coll_sphere_sweep_flags(b, a, e, r, ign, h);
}

static int is_hit(const coll_hit *h, float t, float nx, float ny, float nz, unsigned flags, unsigned material)
{
	if (!NEAR(h->t, t, 2e-5f) || !NEAR(h->normal[0], nx, 1e-5f) || !NEAR(h->normal[1], ny, 1e-5f) || !NEAR(h->normal[2], nz, 1e-5f)) {
		fprintf(stderr, "  hit t=%.6f n=(%.4f %.4f %.4f) p=(%.4f %.4f %.4f) flags=%u material=%u surface=%d; expected t=%.6f n=(%g %g %g) flags=%u material=%u\n",
		        (double)h->t, (double)h->normal[0], (double)h->normal[1], (double)h->normal[2], (double)h->point[0],
		        (double)h->point[1], (double)h->point[2], h->flags, h->material, h->surface, (double)t, (double)nx, (double)ny, (double)nz, flags, material);
		return 0;
	}
	return h->flags == flags && h->material == material;
}

static int test_world(const uint8_t *raw0, size_t n0, const uint8_t *raw1, size_t n1)
{
	coll_bsp b, e;
	coll_hit h;
	float push[3];
	float P[COLL_MAX_POLY][3], pl[4];
	uint16_t m, f, br;
	uint32_t i;

	CHECK(coll_open(raw0, n0, &b) == COLL_OK);
	CHECK(b.node_count == 11 && b.plane_count == 13 && b.leaf_count == 2 && b.ref_count == 12 && b.node2d_count == 15 &&
	      b.surface_count == 26 && b.index_count == 104 && b.vertex_count == 30 && b.depth == 6);
	CHECK(NEAR(b.bounds[0], -10, 0) && NEAR(b.bounds[3], 10, 0) && NEAR(b.bounds[2], 0, 0) && NEAR(b.bounds[5], 5, 0));
	CHECK(coll_leaf_ref_count(&b, 0) == 6 && coll_leaf_ref_count(&b, 1) == 6 && coll_leaf_ref_count(&b, 2) == 0);

	/* point location: leaf 0 = left room, leaf 1 = right room, ties on x = 0 go to the front (right) */
	{ static const struct { float p[3]; int leaf; } pts[] = {
		{ { 5, 0, 2.5f }, 1 }, { { -5, 0, 2.5f }, 0 }, { { 0, 0, 2.5f }, 1 }, { { -0.0001f, 0, 2.5f }, 0 }, { { -20, 0, 2.5f }, -1 },
		{ { 5, 0, -1 }, -1 }, { { 5, 0, 6 }, -1 }, { { 11, 0, 2.5f }, -1 }, { { 5, 11, 2.5f }, -1 }, { { 5, -11, 2.5f }, -1 },
		{ { 10, 0, 2.5f }, -1 }, { { 9.999f, 0, 2.5f }, 1 }, { { -9.999f, 9.999f, 0.001f }, 0 } };
	  for (i = 0; i < sizeof pts / sizeof pts[0]; i++) {
		  CHECK(coll_locate_leaf(&b, pts[i].p) == pts[i].leaf);
		  CHECK(coll_point_in_solid(&b, pts[i].p) == (pts[i].leaf < 0));
	  } }

	/* rays: floor, ceiling (breakable, flipped plane, swapped 2D axes), the pane from both sides, flags, solid start */
	CHECK(ray(&b, 5, 0, 2.5f, 5, 0, -2.5f, 0, &h) && is_hit(&h, 0.5f, 0, 0, 1, 0, 2) && NEAR(h.point[0], 5, 1e-5f) && NEAR(h.point[2], 0, 1e-5f));
	CHECK(ray(&b, -5, 0, 2.5f, -5, 0, 10, 0, &h) && is_hit(&h, 1.0f / 3.0f, 0, 0, -1, COLL_F_BREAKABLE, 2) && h.breakable == 5);
	CHECK(ray(&b, -5, 0, 2.5f, 5, 0, 2.5f, 0, &h) && is_hit(&h, 0.5f, -1, 0, 0, COLL_F_TWO_SIDED, 9) && NEAR(h.point[0], 0, 1e-5f) && h.surface >= 10 && h.surface <= 11);   /* the twin */
	CHECK(!ray(&b, -5, 0, 2.5f, 5, 0, 2.5f, COLL_F_TWO_SIDED, &h));
	CHECK(ray(&b, 5, 0, 2.5f, -5, 0, 2.5f, 0, &h) && is_hit(&h, 0.5f, 1, 0, 0, COLL_F_TWO_SIDED, 9) && h.surface <= 1);
	CHECK(ray(&b, -5, 0, 2.5f, 15, 0, 2.5f, 0, &h) && is_hit(&h, 0.25f, -1, 0, 0, COLL_F_TWO_SIDED, 9));
	CHECK(ray(&b, 5, 5, 2.5f, 5, 15, 2.5f, 0, &h) && is_hit(&h, 0.5f, 0, -1, 0, COLL_F_INVISIBLE, 7));
	CHECK(!ray(&b, 5, 5, 2.5f, 5, 15, 2.5f, COLL_F_INVISIBLE, &h));
	CHECK(ray(&b, 5, -5, 2.5f, 5, -15, 2.5f, 0, &h) && is_hit(&h, 0.5f, 0, 1, 0, COLL_F_CLIMBABLE, 7));
	CHECK(ray(&b, 5, 0, 2.5f, 15, 0, 2.5f, 0, &h) && is_hit(&h, 0.5f, -1, 0, 0, 0, 2));
	CHECK(ray(&b, -7, 5, 2.5f, -7, 15, 2.5f, 0, &h) && is_hit(&h, 0.5f, 0, -1, 0, 0, 2));        /* the shared reference */
	CHECK(ray(&b, -4, 3, 2, -4, 3, -1, 0, &h) && is_hit(&h, 2.0f / 3.0f, 0, 0, 1, 0, 2));        /* a cell corner of the 2D BSP */
	CHECK(ray(&b, -1, 9.99f, 0.01f, 1, 9.99f, 0.01f, 0, &h) && is_hit(&h, 0.5f, -1, 0, 0, COLL_F_TWO_SIDED, 9));
	CHECK(!ray(&b, -20, 0, 2.5f, -5, 0, 2.5f, 0, &h));                 /* starts in solid space: the wall is hit from behind */
	CHECK(!ray(&b, 1, 1, 1, 9, 1, 4, 0, &h));
	CHECK(!ray(&b, 1, 1, 1, 1, 1, 1, 0, &h));                          /* no length */
	CHECK(!ray(&b, 1, 0, 0, 9, 0, 0, 0, &h));                          /* slides along the floor plane */
	CHECK(!ray(&b, 5, 0, 5, 5, 0, 9, 0, &h));                          /* leaves through the ceiling plane from the front... */
	CHECK(ray(&b, 5, 0, 2.5f, 5, 0, 9, 0, &h) && is_hit(&h, 2.5f / 6.5f, 0, 0, -1, 0, 2));

	/* sphere sweeps, radius 0.5 */
	CHECK(sweep(&b, 5, 0, 2.5f, 5, 0, -1, 0.5f, 0, &h) && is_hit(&h, 2.0f / 3.5f, 0, 0, 1, 0, 2) && NEAR(h.point[2], 0, 1e-4f));
	CHECK(sweep(&b, 5, 0, 2.5f, 5, 20, 2.5f, 0.5f, 0, &h) && is_hit(&h, 0.475f, 0, -1, 0, COLL_F_INVISIBLE, 7));
	CHECK(!sweep(&b, 5, 0, 2.5f, 5, 9.4f, 2.5f, 0.5f, 0, &h));
	CHECK(!sweep(&b, 5, 0, 2.5f, 5, 20, 2.5f, 0.5f, COLL_F_INVISIBLE, &h));                  /* ignored: out through the wall */
	CHECK(sweep(&b, -5, 0, 2.5f, 5, 0, 2.5f, 0.5f, 0, &h) && is_hit(&h, 0.45f, -1, 0, 0, COLL_F_TWO_SIDED, 9));
	CHECK(sweep(&b, 5, 0, 2.5f, -5, 0, 2.5f, 0.5f, 0, &h) && is_hit(&h, 0.45f, 1, 0, 0, COLL_F_TWO_SIDED, 9));
	CHECK(!sweep(&b, -5, 0, 2.5f, 5, 0, 2.5f, 0.5f, COLL_F_TWO_SIDED, &h));
	CHECK(sweep(&b, 5, 0, 2.5f, 15, 0, 2.5f, 0.5f, 0, &h) && is_hit(&h, 0.45f, -1, 0, 0, 0, 2));
	CHECK(sweep(&b, 9, 9, 2.5f, 12, 12, 2.5f, 0.5f, 0, &h) && NEAR(h.t, 1.0f / 6.0f, 1e-5f) && h.surface >= 0);
	CHECK(NEAR(h.normal[0], -1, 1e-5f) + NEAR(h.normal[1], -1, 1e-5f) == 1);                 /* one of the two walls */
	CHECK(sweep(&b, 5, 0, 0.5f, 5, 0, -1, 0.5f, 0, &h) && is_hit(&h, 0.0f, 0, 0, 1, 0, 2));  /* touching and moving in */
	CHECK(!sweep(&b, 5, 0, 0.5f, 8, 0, 0.5f, 0.5f, 0, &h));                                   /* sliding along it */
	CHECK(!sweep(&b, 3, 0, 2.5f, 7, 3, 3, 0.5f, 0, &h));
	CHECK(sweep(&b, 5, 0, 2.5f, 5, 0, -1, 0.0f, 0, &h) && is_hit(&h, 2.5f / 3.5f, 0, 0, 1, 0, 2));   /* radius 0: a ray */
	CHECK(sweep(&b, -5, 0, 2.5f, -5, 0, 10, 0.25f, 0, &h) && h.flags == COLL_F_BREAKABLE && h.breakable == 5 && NEAR(h.t, 2.25f / 7.5f, 1e-5f));
	{ float a[3] = { 5, 0, 2.5f }, c[3] = { 5, 0, -1 };                                        /* the plain entry point */
	  CHECK(coll_sphere_sweep(&b, a, c, 0.5f, &h) && NEAR(h.t, 2.0f / 3.5f, 2e-5f)); }

	/* depenetration */
	{ float c1[3] = { 5, 0, 0.2f }, c2[3] = { 5, 0, 1 }, c3[3] = { 5, 0, -0.2f }, c4[3] = { 9.8f, 9.8f, 2.5f }, c5[3] = { 0.2f, 0, 2.5f },
	        c6[3] = { -0.2f, 0, 2.5f }, c7[3] = { 5, 0, -0.7f };
	  CHECK(coll_sphere_overlap(&b, c1, 0.5f, push) && NEAR(push[0], 0, 1e-5f) && NEAR(push[1], 0, 1e-5f) && NEAR(push[2], 0.3f, 1e-5f));
	  CHECK(!coll_sphere_overlap(&b, c2, 0.5f, push) && push[0] == 0 && push[1] == 0 && push[2] == 0);
	  CHECK(coll_sphere_overlap(&b, c3, 0.5f, push) && NEAR(push[2], 0.7f, 1e-5f));              /* centre behind the face: out through it */
	  CHECK(coll_sphere_overlap(&b, c4, 0.5f, push) && NEAR(push[0], -0.3f, 1e-4f) && NEAR(push[1], -0.3f, 1e-4f) && NEAR(push[2], 0, 1e-5f));
	  CHECK(coll_sphere_overlap(&b, c5, 0.5f, push) && NEAR(push[0], 0.3f, 1e-5f));
	  CHECK(coll_sphere_overlap(&b, c6, 0.5f, push) && NEAR(push[0], -0.3f, 1e-5f));            /* two-sided pane: pushed to its own side */
	  CHECK(!coll_sphere_overlap(&b, c7, 0.5f, push));                                          /* too far behind a one-sided face */
	  CHECK(!coll_sphere_overlap(&b, c1, 0.0f, push)); }

	/* surfaces */
	CHECK(coll_surface_polygon(&b, 0, P) == 4 && P[0][0] == 0 && P[1][0] == 0 && P[2][0] == 0 && P[3][0] == 0);
	CHECK(coll_surface_info(&b, 0, pl, &m, &f, &br) && pl[0] == 1 && pl[3] == 0 && m == 9 && f == COLL_F_TWO_SIDED && br == 0);
	CHECK(coll_surface_info(&b, 2, pl, &m, &f, &br) && pl[0] == -1 && pl[3] == -10 && m == 2 && f == 0);     /* x = 10, flipped */
	CHECK(coll_surface_info(&b, 10, pl, &m, &f, &br) && pl[0] == -1 && pl[3] == 0 && m == 9 && f == COLL_F_TWO_SIDED);   /* PL, the twin of PR */
	CHECK(!coll_surface_info(&b, 26, pl, &m, &f, &br) && !coll_surface_info(&b, -1, pl, &m, &f, &br));
	CHECK(coll_surface_polygon(&b, 26, P) == 0 && coll_surface_polygon(&b, -1, P) == 0);
	walk(&b);

	/* the empty section */
	CHECK(coll_open(raw1, n1, &e) == COLL_OK && e.node_count == 0 && e.leaf_count == 0 && e.surface_count == 0);
	CHECK(!ray(&e, 0, 0, 0, 5, 5, 5, 0, &h) && !sweep(&e, 0, 0, 0, 5, 5, 5, 0.5f, 0, &h));
	{ float p[3] = { 1, 2, 3 }; CHECK(coll_point_in_solid(&e, p) == 1 && coll_locate_leaf(&e, p) == -1 && !coll_sphere_overlap(&e, p, 1.0f, push)); }
	walk(&e);

	/* hostile input: every truncation, every byte flipped three ways; a section that still opens must be safe to walk */
	for (i = 0; i < n0; i++) CHECK(coll_open(raw0, i, &e) != COLL_OK);
	CHECK(coll_open(NULL, n0, &e) != COLL_OK);
	{
		uint8_t *copy = malloc(n0 + 16);
		uint32_t k, opened = 0;
		static const uint8_t flips[3] = { 0xFF, 0x01, 0x80 };
		CHECK(copy);
		memcpy(copy, raw0, n0);
		memset(copy + n0, 0, 16);
		CHECK(coll_open(copy, n0 + 16, &e) == COLL_ERR_LAYOUT);                     /* the size field no longer matches */
		put32(copy + 8, (uint32_t)n0 + 16);
		CHECK(coll_open(copy, n0 + 16, &e) == COLL_OK);                             /* trailing padding is fine if declared */
		for (i = 0; i < n0; i++)
			for (k = 0; k < 3; k++) {
				memcpy(copy, raw0, n0);
				copy[i] ^= flips[k];
				if (coll_open(copy, n0, &e) == COLL_OK) { walk(&e); opened++; }
			}
		fprintf(stdout, "[coll] synthetic: %u of %zu byte flips still open (all walked clean)\n", opened, n0 * 3);
		free(copy);
	}
	{
		uint8_t *copy = malloc(n0);
		CHECK(copy);
		memcpy(copy, raw0, n0);
		copy[4] = 2;
		CHECK(coll_open(copy, n0, &e) == COLL_ERR_VERSION);
		copy[4] = 1;
		copy[0] = 'X';
		CHECK(coll_open(copy, n0, &e) == COLL_ERR_MAGIC);
		free(copy);
	}
	return 0;
}

/* ---- 4. real data: brute force reference ----------------------------------------------------------------- */
typedef struct {
	const coll_bsp *b;
	uint32_t ns;
	float (*pl)[4];                      /* effective planes */
	uint8_t *nv;
	uint16_t *fl;
	float (*P)[COLL_MAX_POLY][3];
	float (*lo)[3], (*hi)[3];
	float cell, org[3];
	int dim[3];
	uint32_t *start, *items;             /* grid: items of cell c are items[start[c] .. start[c+1]) */
	uint32_t *stamp, tick;
} refw;

static int cell_of(const refw *w, int x, int y, int z) { return (z * w->dim[1] + y) * w->dim[0] + x; }

static void cell_range(const refw *w, uint32_t s, int r[6])
{
	int k;
	for (k = 0; k < 3; k++) {
		r[2 * k] = (int)((w->lo[s][k] - w->org[k]) / w->cell);
		r[2 * k + 1] = (int)((w->hi[s][k] - w->org[k]) / w->cell);
	}
}

static int refw_build(refw *w, const coll_bsp *b)
{
	uint32_t s, i, total = 0, c;
	int k;
	memset(w, 0, sizeof *w);
	w->b = b;
	w->ns = b->surface_count;
	w->pl = malloc(sizeof *w->pl * w->ns);
	w->nv = malloc(w->ns);
	w->fl = malloc(sizeof *w->fl * w->ns);
	w->P = malloc(sizeof *w->P * w->ns);
	w->lo = malloc(sizeof *w->lo * w->ns);
	w->hi = malloc(sizeof *w->hi * w->ns);
	w->stamp = calloc(w->ns, sizeof *w->stamp);
	if (!w->pl || !w->nv || !w->fl || !w->P || !w->lo || !w->hi || !w->stamp) return 0;
	w->cell = 8.0f;
	for (k = 0; k < 3; k++) {
		w->org[k] = b->bounds[k] - 1.0f;
		w->dim[k] = (int)((b->bounds[3 + k] - b->bounds[k] + 2.0f) / w->cell) + 1;
	}
	for (s = 0; s < w->ns; s++) {
		uint16_t m, f, br;
		coll_surface_info(b, (int32_t)s, w->pl[s], &m, &f, &br);
		w->fl[s] = f;
		w->nv[s] = (uint8_t)coll_surface_polygon(b, (int32_t)s, w->P[s]);
		for (k = 0; k < 3; k++) { w->lo[s][k] = 1e30f; w->hi[s][k] = -1e30f; }
		for (i = 0; i < w->nv[s]; i++)
			for (k = 0; k < 3; k++) {
				if (w->P[s][i][k] < w->lo[s][k]) w->lo[s][k] = w->P[s][i][k];
				if (w->P[s][i][k] > w->hi[s][k]) w->hi[s][k] = w->P[s][i][k];
			}
	}
	/* grid: count the cells of every surface's box, prefix-sum, fill */
	{
		uint32_t cells = (uint32_t)(w->dim[0] * w->dim[1] * w->dim[2]), *cur;
		int r[6], x, y, z;
		w->start = calloc((size_t)cells + 1, sizeof *w->start);
		if (!w->start) return 0;
		for (s = 0; s < w->ns; s++) {
			cell_range(w, s, r);
			for (z = r[4]; z <= r[5]; z++)
				for (y = r[2]; y <= r[3]; y++)
					for (x = r[0]; x <= r[1]; x++) { w->start[cell_of(w, x, y, z) + 1]++; total++; }
		}
		for (c = 0; c < cells; c++) w->start[c + 1] += w->start[c];
		w->items = malloc(sizeof *w->items * (total + 1));
		cur = malloc(sizeof *cur * (cells + 1));
		if (!w->items || !cur) return 0;
		memcpy(cur, w->start, sizeof *cur * (cells + 1));
		for (s = 0; s < w->ns; s++) {
			cell_range(w, s, r);
			for (z = r[4]; z <= r[5]; z++)
				for (y = r[2]; y <= r[3]; y++)
					for (x = r[0]; x <= r[1]; x++) w->items[cur[cell_of(w, x, y, z)]++] = s;
		}
		free(cur);
	}
	return 1;
}

static void refw_free(refw *w)
{
	free(w->pl); free(w->nv); free(w->fl); free(w->P); free(w->lo); free(w->hi); free(w->start); free(w->items); free(w->stamp);
}

/* brute force ray: every surface polygon, front to back */
static int ref_ray(const refw *w, const float a[3], const float e[3], uint32_t ign, float *t_out, int32_t *s_out)
{
	float d[3], best = 2.0f;
	int32_t bs = -1;
	uint32_t s;
	sub3(d, e, a);
	for (s = 0; s < w->ns; s++) {
		const float *pl = w->pl[s];
		float ea = dot3(pl, a) - pl[3], eb = ea + dot3(pl, d), t, X[3];
		if (w->fl[s] & ign) continue;
		if (!(ea >= 0.0f && eb < 0.0f)) continue;                       /* front to back only: a two-sided surface has a twin */
		t = ea / (ea - eb);
		if (t >= best) continue;
		X[0] = a[0] + t * d[0]; X[1] = a[1] + t * d[1]; X[2] = a[2] + t * d[2];
		if (!poly_contains((float (*)[3])w->P[s], w->nv[s], pl, X, EPS_RAY_INSIDE)) continue;
		best = t;
		bs = (int32_t)s;
	}
	*t_out = best;
	*s_out = bs;
	return bs >= 0;
}

/* distance from c to the polygon of surface s (and the side: signed distance to the plane) */
static float poly_distance(const refw *w, uint32_t s, const float c[3], float *sd_out)
{
	const float *pl = w->pl[s];
	float sd = dot3(pl, c) - pl[3], foot[3], best = 1e30f, Q[3];
	uint32_t i;
	foot[0] = c[0] - pl[0] * sd; foot[1] = c[1] - pl[1] * sd; foot[2] = c[2] - pl[2] * sd;
	*sd_out = sd;
	if (poly_contains((float (*)[3])w->P[s], w->nv[s], pl, foot, 0.0f)) return fabsf(sd);
	for (i = 0; i < w->nv[s]; i++) {
		float dd = seg_closest(w->P[s][i], w->P[s][i + 1 == w->nv[s] ? 0 : i + 1], c, Q);
		if (dd < best) best = dd;
	}
	return sqrtf(best);
}

/* smallest distance from c to a surface it can collide with (c in front of it);
 * everything farther than `cap` is reported as cap */
static float ref_clearance(refw *w, const float c[3], float cap, int32_t *which)
{
	int lo[3], hi[3], k, x, y, z;
	float best = cap;
	for (k = 0; k < 3; k++) {
		lo[k] = (int)((c[k] - cap - w->org[k]) / w->cell);
		hi[k] = (int)((c[k] + cap - w->org[k]) / w->cell);
		if (lo[k] < 0) lo[k] = 0;
		if (hi[k] >= w->dim[k]) hi[k] = w->dim[k] - 1;
	}
	w->tick++;
	if (which) *which = -1;
	for (z = lo[2]; z <= hi[2]; z++)
		for (y = lo[1]; y <= hi[1]; y++)
			for (x = lo[0]; x <= hi[0]; x++) {
				uint32_t cc = (uint32_t)cell_of(w, x, y, z), i;
				for (i = w->start[cc]; i < w->start[cc + 1]; i++) {
					uint32_t s = w->items[i];
					float sd, dist;
					if (w->stamp[s] == w->tick) continue;
					w->stamp[s] = w->tick;
					for (k = 0; k < 3; k++)
						if (c[k] < w->lo[s][k] - cap || c[k] > w->hi[s][k] + cap) break;
					if (k < 3) continue;
					dist = poly_distance(w, s, c, &sd);
					if (sd < -1e-3f) continue;
					if (dist < best) { best = dist; if (which) *which = (int32_t)s; }
				}
			}
	return best;
}

static void random_point(const coll_bsp *b, float p[3])
{
	int k;
	for (k = 0; k < 3; k++) p[k] = frange(b->bounds[k], b->bounds[3 + k]);
}

/* random point in empty space whose sphere of radius r does not touch anything (by the brute force) */
static int clear_start(refw *w, float r, float p[3])
{
	int tries;
	for (tries = 0; tries < 4000; tries++) {
		random_point(w->b, p);
		if (coll_point_in_solid(w->b, p)) continue;
		if (ref_clearance(w, p, r + 0.02f, NULL) >= r + 0.02f) return 1;
	}
	return 0;
}

/* distance from p to the boundary of a surface polygon */
static float edge_dist(const coll_bsp *b, int32_t surface, const float p[3])
{
	float P[COLL_MAX_POLY][3], best = 1e30f, Q[3];
	uint32_t nv = coll_surface_polygon(b, surface, P), i;
	for (i = 0; i < nv; i++) {
		float dd = seg_closest(P[i], P[i + 1 == nv ? 0 : i + 1], p, Q);
		if (dd < best) best = dd;
	}
	return sqrtf(best);
}

/* where a tree hit lies relative to its own surface polygon: for the report of a disagreement */
static void explain_hit(const coll_bsp *b, const coll_hit *h, const float a[3], const float e[3])
{
	float P[COLL_MAX_POLY][3], pl[4], best = 1e30f, Q[3], d[3];
	uint32_t nv = coll_surface_polygon(b, h->surface, P), i;
	uint16_t m, f, br;
	if (!nv || !coll_surface_info(b, h->surface, pl, &m, &f, &br)) return;
	for (i = 0; i < nv; i++) {
		float dd = seg_closest(P[i], P[i + 1 == nv ? 0 : i + 1], h->point, Q);
		if (dd < best) best = dd;
	}
	sub3(d, e, a);
	{
		float before[3], after[3], len = sqrtf(dot3(d, d));
		for (i = 0; i < 3; i++) { before[i] = h->point[i] - d[i] / len * 2e-3f; after[i] = h->point[i] + d[i] / len * 2e-3f; }
		fprintf(stderr, "    leaf 2 mm before the hit %d, 2 mm after %d; surface flags %u\n", coll_locate_leaf(b, before), coll_locate_leaf(b, after), f);
	}
	fprintf(stderr, "    hit point %.4f %.4f %.4f: inside its polygon %d, %.4f from the nearest edge, plane distance %.5f, "
	        "start side %.4f end side %.4f, start leaf %d end leaf %d\n", (double)h->point[0], (double)h->point[1], (double)h->point[2],
	        poly_contains(P, nv, pl, h->point, 0.0f), (double)sqrtf(best), (double)(dot3(pl, h->point) - pl[3]),
	        (double)(dot3(pl, a) - pl[3]), (double)(dot3(pl, e) - pl[3]), coll_locate_leaf(b, a), coll_locate_leaf(b, e));
}

/* where the brute-force reference crosses a surface: leaves around the crossing, the surface's flags */
static void explain_ref(const coll_bsp *b, int32_t sr, const float a[3], const float e[3], float tr)
{
	float pl[4], X[3], d[3], len, q[3];
	uint16_t m, f, br;
	int k;
	coll_surface_info(b, sr, pl, &m, &f, &br);
	sub3(d, e, a);
	len = sqrtf(dot3(d, d));
	for (k = 0; k < 3; k++) X[k] = a[k] + tr * d[k];
	fprintf(stderr, "    ray a = %.9g %.9g %.9g, b = %.9g %.9g %.9g\n", (double)a[0], (double)a[1], (double)a[2], (double)e[0], (double)e[1], (double)e[2]);
	fprintf(stderr, "    reference surface %d flags %u material %u, crossing %.4f %.4f %.4f", sr, f, m, (double)X[0], (double)X[1], (double)X[2]);
	for (k = 0; k < 3; k++) q[k] = X[k] - d[k] / len * 2e-3f;
	fprintf(stderr, ", leaf 2 mm before %d", coll_locate_leaf(b, q));
	for (k = 0; k < 3; k++) q[k] = X[k] + d[k] / len * 2e-3f;
	fprintf(stderr, ", 2 mm after %d", coll_locate_leaf(b, q));
	for (k = 0; k < 3; k++) q[k] = X[k] - pl[k] * 1e-3f;
	fprintf(stderr, ", 1 mm behind the surface %d, 1 mm in front %d\n", coll_locate_leaf(b, q), ({ for (k = 0; k < 3; k++) q[k] = X[k] + pl[k] * 1e-3f; coll_locate_leaf(b, q); }));
}

/* does the leaf that holds the point just before a reference crossing list the surface, and what does its 2D BSP say */
static void explain_leaf(const coll_bsp *b, int32_t sr, const float a[3], const float e[3], float tr)
{
	float d[3], len, X[3], before[3], pl[4];
	uint32_t r0, r1, r, k;
	int two;
	int32_t leaf;
	sub3(d, e, a);
	len = sqrtf(dot3(d, d));
	for (k = 0; k < 3; k++) { X[k] = a[k] + tr * d[k]; before[k] = X[k] - d[k] / len * 2e-3f; }
	coll_surface_info(b, sr, pl, NULL, NULL, NULL);
	leaf = coll_locate_leaf(b, before);
	if (leaf < 0) return;
	leaf_range(b, (uint32_t)leaf, &r0, &r1, &two);
	fprintf(stderr, "    leaf %d has %u references:", leaf, r1 - r0);
	for (r = r0; r < r1; r++) {
		const uint8_t *p = b->refs + (size_t)r * REF_REC;
		uint32_t pf = rd16(p), root = rd16(p + 2);
		float n[3], dd, sg = (pf >> 15) ? -1.0f : 1.0f, ea, eb;
		int ua, va;
		int32_t s;
		load_plane(b, pf & 0x7FFFu, n, &dd);
		ea = sg * (dot3(n, a) - dd);
		eb = ea + sg * dot3(n, d);
		proj_axes(n, pf >> 15, &ua, &va);
		s = lookup2d(b, root, X[ua], X[va]);
		fprintf(stderr, " [plane %u flip %u sides %.4f %.4f -> surface %d%s]", pf & 0x7FFFu, pf >> 15, (double)ea, (double)eb, s, s == sr ? " (the reference surface)" : "");
	}
	fprintf(stderr, "\n    reference plane (%.5f %.5f %.5f | %.5f)\n", (double)pl[0], (double)pl[1], (double)pl[2], (double)pl[3]);
}

/* a start next to a random surface (in front of it, within 2 wu): moves from here meet geometry often, also on the
 * open outdoor BSPs where a random point of the bounds is far from everything */
static int near_surface_start(refw *w, float r, float p[3])
{
	int tries;
	for (tries = 0; tries < 2000; tries++) {
		uint32_t s = rnd() % w->ns;
		float c[3], off = frange(r + 0.02f, 2.0f);
		int k;
		for (k = 0; k < 3; k++) c[k] = (w->P[s][0][k] + w->P[s][1][k] + w->P[s][2][k]) / 3.0f + w->pl[s][k] * off + frange(-0.3f, 0.3f);
		if (coll_point_in_solid(w->b, c)) continue;
		if (ref_clearance(w, c, r + 0.02f, NULL) < r + 0.02f) continue;
		p[0] = c[0]; p[1] = c[1]; p[2] = c[2];
		return 1;
	}
	return 0;
}

static int test_real_bsp(const coll_bsp *b, const char *label, uint32_t RAYS, uint32_t SWEEPS)
{
	refw w;
	uint32_t i, n, solid_start_diff = 0, edge_tol = 0;
	/* counters */
	uint32_t both_none = 0, same_surface = 0, seam = 0, tree_only = 0, ref_only = 0, diff_t = 0, hits = 0;

	CHECK(refw_build(&w, b));
	/* rays: half start in empty space (any direction, log-uniform length), a quarter anywhere (solid starts too),
	 * a quarter short (about the length of one tick of motion or one bullet step) */
	for (n = 0; n < RAYS; n++) {
		float a[3], d[3], e[3], len, tt, tr;
		int32_t sr;
		coll_hit h;
		int ht, hr;
		random_point(b, a);
		if (n % 4 == 2) near_surface_start(&w, 0.05f, a);
		else if (n % 4 != 1) {
			uint32_t g;
			for (g = 0; g < 200 && coll_point_in_solid(b, a); g++) random_point(b, a);
		}
		rdir(d);
		len = n % 4 == 3 ? frange(0.05f, 2.0f) : expf(frange(logf(0.5f), logf(150.0f)));
		e[0] = a[0] + d[0] * len; e[1] = a[1] + d[1] * len; e[2] = a[2] + d[2] * len;
		ht = coll_ray(b, a, e, 0, &h);
		hr = ref_ray(&w, a, e, 0, &tr, &sr);
		tt = ht ? h.t : 2.0f;
		if (!ht && !hr) { both_none++; continue; }
		if (coll_point_in_solid(b, a) && (ht != hr || (ht && fabsf(tt - tr) * len > 5e-3f))) { solid_start_diff++; continue; }   /* a ray that starts inside solid space */
		if (ht) hits++;
		if (ht && !hr && edge_dist(b, h.surface, h.point) <= 2e-3f) { edge_tol++; continue; }       /* within 2 mm of a polygon edge */
		if (ht && hr && fabsf(tt - tr) * len > 5e-3f && edge_dist(b, h.surface, h.point) <= 2e-3f) { edge_tol++; continue; }
		if (!ht && hr) {
			float X[3], dd[3];
			sub3(dd, e, a);
			for (int k = 0; k < 3; k++) X[k] = a[k] + tr * dd[k];
			if (edge_dist(b, sr, X) <= 2e-3f) { edge_tol++; continue; }
		}
		if (ht && !hr) { tree_only++; if (tree_only <= 3) { fprintf(stderr, "  [%s] tree-only ray %u: t=%.6f len=%.3f surface %d\n", label, n, (double)h.t, (double)len, h.surface); explain_hit(b, &h, a, e); } continue; }
		if (!ht && hr) { ref_only++; if (ref_only <= 3) { fprintf(stderr, "  [%s] reference-only ray %u: t=%.6f len=%.3f surface %d start solid %d\n", label, n, (double)tr, (double)len, sr, coll_point_in_solid(b, a)); explain_ref(b, sr, a, e, tr); explain_leaf(b, sr, a, e, tr); } continue; }
		if (fabsf(tt - tr) * len > 5e-3f) { diff_t++; if (diff_t <= 3) { fprintf(stderr, "  [%s] ray %u t differs: tree %.6f (surface %d) reference %.6f (surface %d) len %.3f\n", label, n, (double)tt, h.surface, (double)tr, sr, (double)len); explain_hit(b, &h, a, e); explain_ref(b, sr, a, e, tr); explain_leaf(b, sr, a, e, tr); } continue; }
		if (h.surface == sr) same_surface++;
		else seam++;       /* same point, another surface: the two share an edge there, or coincide */
	}
	{
		uint32_t bad = tree_only + ref_only + diff_t + edge_tol;
		fprintf(stdout, "[coll] %s rays: %u, %u hit; both none %u, same surface %u, same point other surface (shared edge or coincident) %u, "
		        "tree-only %u, reference-only %u, different t %u (within 2 mm of a polygon edge, the tolerance zone: %u; rays that start in solid space and differ: %u) -> agreement %.3f%%\n",
		        label, RAYS, hits, both_none, same_surface, seam, tree_only, ref_only, diff_t, edge_tol, solid_start_diff,
		        100.0 * (double)(RAYS - bad) / (double)RAYS);
		CHECK(bad * 1000u <= RAYS);          /* at most 0.1% (the tolerance zone and real data inconsistencies, see the doc) */
	}

	/* sphere sweeps: never end in, or pass through, solid space */
	{
		static const float radii[5] = { 0.05f, 0.1f, 0.2f, 0.35f, 0.5f };
		uint32_t done = 0, hit_n = 0, world_edge = 0, passes_solid = 0, penetrates = 0, false_stop = 0, face = 0, other = 0, no_start = 0;
		for (n = 0; n < SWEEPS; n++) {
			float r = radii[n % 5], a[3], d[3], e[3], len, tt, c[3], prev[3];
			coll_hit h;
			int ht, k;
			uint32_t steps, st;
			if (!(n & 1 ? near_surface_start(&w, r, a) : clear_start(&w, r, a))) { no_start++; continue; }
			rdir(d);
			len = n % 3 == 0 ? frange(0.05f, 1.0f) : expf(frange(logf(0.2f), logf(40.0f)));
			for (k = 0; k < 3; k++) e[k] = a[k] + d[k] * len;
			ht = coll_sphere_sweep(b, a, e, r, &h);
			tt = ht ? h.t : 1.0f;
			steps = (uint32_t)(len * tt / (0.5f * r)) + 2;
			if (steps > 300) steps = 300;
			prev[0] = a[0]; prev[1] = a[1]; prev[2] = a[2];
			for (st = 1; st <= steps; st++) {
				float f = tt * (float)st / (float)steps, cl;
				for (k = 0; k < 3; k++) c[k] = a[k] + (e[k] - a[k]) * f;
				if (coll_point_in_solid(b, c)) {
					/* solid space entered through a surface polygon is a wall that was passed; solid space entered without
					 * crossing any is the open edge of the world (terrain BSPs end in `none` children without surfaces) */
					float tr_; int32_t sr_;
					if (!ref_ray(&w, prev, c, 0, &tr_, &sr_)) { world_edge++; break; }
					passes_solid++;
					if (passes_solid <= 3) fprintf(stderr, "  [%s] sweep %u r=%.2f: the centre is in solid space %.3f of the way (hit=%d t=%.4f), a = %.9g %.9g %.9g, b = %.9g %.9g %.9g\n", label, n,
					                               (double)r, (double)f, ht, (double)tt, (double)a[0], (double)a[1], (double)a[2], (double)e[0], (double)e[1], (double)e[2]);
					break;
				}
				cl = ref_clearance(&w, c, r + 0.01f, NULL);
				prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
				if (cl < r - 3e-3f) { penetrates++; if (penetrates <= 3) fprintf(stderr, "  [%s] sweep %u r=%.2f len=%.3f: clearance %.4f at %.3f of the way (hit=%d t=%.4f)\n", label, n, (double)r, (double)len, (double)cl, (double)f, ht, (double)tt); break; }
			}
			if (ht) {
				float cl;
				int32_t which;
				float n0[4];
				uint16_t m, f2, br;
				hit_n++;
				for (k = 0; k < 3; k++) c[k] = a[k] + (e[k] - a[k]) * tt;
				cl = ref_clearance(&w, c, r + 0.05f, &which);
				if (cl > r + 3e-3f) { false_stop++; if (false_stop <= 3) fprintf(stderr, "  [%s] sweep %u stopped at t=%.4f but clearance is %.4f (r %.2f)\n", label, n, (double)tt, (double)cl, (double)r); }
				coll_surface_info(b, h.surface, n0, &m, &f2, &br);
				if (fabsf(h.normal[0] - n0[0]) + fabsf(h.normal[1] - n0[1]) + fabsf(h.normal[2] - n0[2]) < 1e-3f ||
				    fabsf(h.normal[0] + n0[0]) + fabsf(h.normal[1] + n0[1]) + fabsf(h.normal[2] + n0[2]) < 1e-3f) face++; else other++;
				CHECK(h.normal[0] * (e[0] - a[0]) + h.normal[1] * (e[1] - a[1]) + h.normal[2] * (e[2] - a[2]) <= 1e-4f * len);
			}
			done++;
		}
		fprintf(stdout, "[coll] %s sweeps: %u (%u without a clear start), %u stopped (%u face contacts, %u edge/vertex), "
		        "passed through solid %u, penetrated %u, stopped short of touching %u (ran off the open edge of the world: %u)\n", label, done, no_start, hit_n, face, other,
		        passes_solid, penetrates, false_stop, world_edge);
		CHECK(done > SWEEPS * 9 / 10 && passes_solid == 0 && penetrates == 0 && false_stop == 0);
		if (hit_n >= 300) CHECK(other > 0 && face > 0);
	}

	/* depenetration: push a touching sphere 0.6 r into the surface it hit and push it back out */
	{
		uint32_t tried = 0, pushed = 0, resolved = 0, again = 0;
		for (n = 0; n < 1500 && tried < 400; n++) {
			float r = 0.2f + 0.1f * (float)(n % 4), a[3], d[3], e[3], len = 3.0f, c[3], push[3], p2[3];
			coll_hit h;
			int k;
			if (!(n & 1 ? near_surface_start(&w, r, a) : clear_start(&w, r, a))) continue;
			rdir(d);
			for (k = 0; k < 3; k++) e[k] = a[k] + d[k] * len;
			if (!coll_sphere_sweep(b, a, e, r, &h)) continue;
			tried++;
			for (k = 0; k < 3; k++) c[k] = a[k] + (e[k] - a[k]) * h.t - h.normal[k] * 0.6f * r;
			if (!coll_sphere_overlap(b, c, r, push)) continue;
			pushed++;
			for (k = 0; k < 3; k++) p2[k] = c[k] + push[k];
			if (ref_clearance(&w, p2, r + 0.01f, NULL) >= r - 0.02f && !coll_point_in_solid(b, p2)) resolved++;
			else if (pushed - resolved <= 3)
				fprintf(stderr, "  [%s] overlap not resolved: r %.2f, start %.4f %.4f %.4f (%d solid), push %.4f %.4f %.4f, clearance after %.4f, solid after %d, hit surface %d t %.4f\n", label, (double)r,
				        (double)c[0], (double)c[1], (double)c[2], coll_point_in_solid(b, c), (double)push[0], (double)push[1], (double)push[2],
				        (double)ref_clearance(&w, p2, r + 0.01f, NULL), coll_point_in_solid(b, p2), h.surface, (double)h.t);
			if (coll_sphere_overlap(b, p2, r - 0.02f, push)) again++;
		}
		fprintf(stdout, "[coll] %s overlap: %u sweeps hit a surface, %u pushed back in and out again, %u resolved (clearance >= r - 0.02), "
		        "%u still overlapping at r - 0.02\n", label, tried, pushed, resolved, again);
		CHECK(tried >= 1u && pushed * 10 >= tried * 9 && (pushed < 20u || resolved * 100 >= pushed * 90));      /* the rest: crevices narrower than the sphere, no solution */
	}
	/* a point 1 mm in front of the interior of any surface is almost always empty space (not where two surfaces are
	 * closer than that) */
	{
		uint32_t blocked = 0;
		for (i = 0; i < b->surface_count; i++) {
			float P[COLL_MAX_POLY][3], pl[4], c[3];
			uint32_t k;
			coll_surface_polygon(b, (int32_t)i, P);
			coll_surface_info(b, (int32_t)i, pl, NULL, NULL, NULL);
			for (k = 0; k < 3; k++) c[k] = (P[0][k] + P[1][k] + P[2][k]) / 3.0f + pl[k] * 1e-3f;
			blocked += (uint32_t)coll_point_in_solid(b, c);
		}
		fprintf(stdout, "[coll] %s: %u of %u surfaces have solid space 1 mm in front of their interior\n", label, blocked, b->surface_count);
		CHECK(blocked * 500u <= b->surface_count);
	}
	refw_free(&w);
	return 0;
}

/* every surface of every BSP: planar polygon, wound counter-clockwise about its plane normal, leaf table sane */
static int test_real_geometry(const coll_bsp *b, const char *label)
{
	uint32_t s;
	float worst = 0.0f;
	for (s = 0; s < b->surface_count; s++) {
		float P[COLL_MAX_POLY][3], pl[4], nw[3] = { 0, 0, 0 };
		uint32_t nv = coll_surface_polygon(b, (int32_t)s, P), i, j;
		CHECK(nv >= 3 && coll_surface_info(b, (int32_t)s, pl, NULL, NULL, NULL));
		for (i = 0; i < nv; i++) {
			float dev = fabsf(dot3(pl, P[i]) - pl[3]);
			j = i + 1 == nv ? 0 : i + 1;
			if (dev > worst) worst = dev;
			nw[0] += (P[i][1] - P[j][1]) * (P[i][2] + P[j][2]);
			nw[1] += (P[i][2] - P[j][2]) * (P[i][0] + P[j][0]);
			nw[2] += (P[i][0] - P[j][0]) * (P[i][1] + P[j][1]);
		}
		CHECK(dot3(nw, pl) > 0.0f);
	}
	CHECK(worst < 1e-3f);
	(void)label;
	return 0;
}

static int cross_check_bsp(const coll_bsp *b, const uint8_t *bsp_raw, size_t bsp_n)
{
	bsp_section s;
	uint32_t n, differ = 0, solid = 0;
	const uint32_t N = 200000;
	CHECK(bsp_open(&s, bsp_raw, bsp_n) == BSP_OK);
	for (n = 0; n < N; n++) {
		float p[3];
		int a, c;
		random_point(b, p);
		if (n % 2) { p[0] += frange(-30, 30); p[1] += frange(-30, 30); p[2] += frange(-10, 10); }
		a = coll_point_in_solid(b, p);
		c = bsp_find_cluster(&s, p) < 0;
		solid += (uint32_t)a;
		if (a != c) differ++;
	}
	fprintf(stdout, "[coll] point-in-solid against the render tree of BSP0: %u points (%u solid), %u differ\n", N, solid, differ);
	CHECK(differ == 0 && solid > N / 10 && N - solid > N / 20);          /* both outcomes well represented */
	return 0;
}

/* a real section hit by random byte flips and truncations: rejected, or safe to walk */
static int corrupt_real(const uint8_t *raw, size_t n)
{
	uint8_t *copy = malloc(n);
	uint32_t k, opened = 0, trials = 400;
	coll_bsp e;
	CHECK(copy);
	for (k = 0; k < trials; k++) {
		uint32_t at = k < 160 ? k : rnd() % (uint32_t)n;           /* the header and directory first, then anywhere */
		memcpy(copy, raw, n);
		copy[at] ^= (uint8_t)(1u << (rnd() & 7));
		if (k % 8 == 7) copy[(rnd() % (uint32_t)n)] = (uint8_t)rnd();
		if (coll_open(copy, n, &e) == COLL_OK) { walk(&e); opened++; }
	}
	for (k = 0; k < 60; k++) CHECK(coll_open(raw, rnd() % (uint32_t)n, &e) != COLL_OK);
	fprintf(stdout, "[coll] real section: %u of %u random flips still open (all walked clean), truncations rejected\n", opened, trials);
	free(copy);
	return 0;
}

/* ---- 5. benchmark ----------------------------------------------------------------------------------------- */
static void bench(const coll_bsp *b, const char *label)
{
	enum { N = 20000 };
	static float A[N][3], E[N][3];
	uint32_t i;
	double t0, t1;
	coll_hit h;
	float push[3], acc = 0.0f;
	uint32_t hits;
	int k;

	for (i = 0; i < N; i++) {
		float d[3];
		do random_point(b, A[i]); while (coll_point_in_solid(b, A[i]));
		rdir(d);
		for (k = 0; k < 3; k++) E[i][k] = A[i][k] + d[k] * frange(5.0f, 40.0f);
	}
	t0 = now_ms();
	for (i = 0, hits = 0; i < N; i++) hits += (uint32_t)coll_ray(b, A[i], E[i], 0, &h);
	t1 = now_ms();
	printf("[coll] bench %s: coll_ray, 5..40 wu segments from empty space: %.1f queries/ms (%u of %d hit)\n", label, N / (t1 - t0), hits, N);
	for (i = 0; i < N; i++) {
		float d[3];
		rdir(d);
		for (k = 0; k < 3; k++) E[i][k] = A[i][k] + d[k] * frange(0.05f, 1.0f);
	}
	t0 = now_ms();
	for (i = 0, hits = 0; i < N; i++) hits += (uint32_t)coll_ray(b, A[i], E[i], 0, &h);
	t1 = now_ms();
	printf("[coll] bench %s: coll_ray, 0.05..1 wu segments: %.1f queries/ms (%u hit)\n", label, N / (t1 - t0), hits);
	t0 = now_ms();
	for (i = 0, hits = 0; i < N; i++) hits += (uint32_t)coll_sphere_sweep(b, A[i], E[i], 0.2f, &h);
	t1 = now_ms();
	printf("[coll] bench %s: coll_sphere_sweep r=0.2, 0.05..1 wu moves (one tick of walking): %.1f queries/ms (%u hit)\n", label, N / (t1 - t0), hits);
	for (i = 0; i < N; i++) {
		float d[3];
		rdir(d);
		for (k = 0; k < 3; k++) E[i][k] = A[i][k] + d[k] * frange(5.0f, 20.0f);
	}
	t0 = now_ms();
	for (i = 0, hits = 0; i < N / 4; i++) hits += (uint32_t)coll_sphere_sweep(b, A[i], E[i], 0.2f, &h);
	t1 = now_ms();
	printf("[coll] bench %s: coll_sphere_sweep r=0.2, 5..20 wu moves (vehicle, grenade): %.1f queries/ms (%u hit)\n", label, (N / 4) / (t1 - t0), hits);
	t0 = now_ms();
	for (i = 0; i < N; i++) acc += (float)coll_point_in_solid(b, A[i]);
	t1 = now_ms();
	printf("[coll] bench %s: coll_point_in_solid: %.1f queries/ms\n", label, N / (t1 - t0));
	t0 = now_ms();
	for (i = 0, hits = 0; i < N; i++) hits += (uint32_t)coll_sphere_overlap(b, A[i], 0.3f, push);
	t1 = now_ms();
	printf("[coll] bench %s: coll_sphere_overlap r=0.3 at free points: %.1f queries/ms (%u overlapping)\n", label, N / (t1 - t0), hits);
	sink = acc;
}

int main(int argc, char **argv)
{
	size_t n, rn0, rn1;
	uint8_t *img, *raw0, *raw1;
	hp2_pack pk;
	int rc;

	if (argc < 2) { fprintf(stderr, "usage: test_coll synthetic.hp2 [real.hp2 [bsp.hp2]]\n"); return 2; }
	if (test_primitives()) return 1;
	img = read_file(argv[1], &n);
	CHECK(img && hp2_open(&pk, img, n) == HP2_OK);
	raw0 = load_section(&pk, "COL0", &rn0);
	raw1 = load_section(&pk, "COL1", &rn1);
	CHECK(raw0 && raw1);
	rc = test_world(raw0, rn0, raw1, rn1);
	if (rc) return rc;
	fprintf(stdout, "[coll] synthetic world: ok\n");
	free(raw0); free(raw1); free(img);

	if (argc >= 3) {
		static const char tags[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		uint32_t i, total = 0, biggest = 0;
		uint8_t *first = NULL;
		size_t first_n = 0;
		coll_bsp b0;
		img = read_file(argv[2], &n);
		CHECK(img && hp2_open(&pk, img, n) == HP2_OK);
		for (i = 0; i < 36; i++) {
			char tag[5] = { 'C', 'O', 'L', tags[i], 0 };
			size_t sn;
			coll_bsp b;
			uint8_t *raw = load_section(&pk, tag, &sn);
			if (!raw) break;
			CHECK(coll_open(raw, sn, &b) == COLL_OK);
			if (test_real_geometry(&b, tag)) return 1;
			fprintf(stdout, "[coll] %s: %.2f MiB, %u nodes, %u leaves, %u 2D nodes, %u surfaces, %u vertices\n", tag,
			        (double)sn / (1024.0 * 1024.0), b.node_count, b.leaf_count, b.node2d_count, b.surface_count, b.vertex_count);
			total += (uint32_t)sn;
			if (sn > biggest) biggest = (uint32_t)sn;
			if (i == 0) { first = raw; first_n = sn; }
			else {
				if ((i == 1 || i == 7 || i == 8) && test_real_bsp(&b, tag, 1500, 400)) return 1;     /* a few more BSPs, lighter */
				free(raw);
			}
		}
		CHECK(i > 0 && first);
		fprintf(stdout, "[coll] %u BSPs open, %.2f MiB raw in total, largest %.2f MiB\n", i, (double)total / (1024.0 * 1024.0), (double)biggest / (1024.0 * 1024.0));
		CHECK(coll_open(first, first_n, &b0) == COLL_OK);
		if (test_real_bsp(&b0, "COL0", 6000, 1500)) return 1;
		if (corrupt_real(first, first_n)) return 1;
		if (argc >= 4) {
			size_t bn, braw_n;
			uint8_t *bimg = read_file(argv[3], &bn), *braw;
			hp2_pack bpk;
			CHECK(bimg && hp2_open(&bpk, bimg, bn) == HP2_OK);
			braw = load_section(&bpk, "BSP0", &braw_n);
			CHECK(braw);
			if (cross_check_bsp(&b0, braw, braw_n)) return 1;
			free(braw); free(bimg);
		}
		if (getenv("COLL_BENCH")) { char label[40]; snprintf(label, sizeof label, "%s BSP0", pk.name); bench(&b0, label); }
		free(first);
		free(img);
	}
	printf("test_coll: ok\n");
	return 0;
}
