/* Host test of the PATn reader (docs/26-path-section.md).
 *   test_path <synthetic.hp2> [<path pack> <coll pack>]...
 * 1. the synthetic world of tools/tests/test_pathconv.py (numbers hard-coded here): open, locate, heights, links with their costs,
 *    A* costs, funnel corners (hand computed), partial routes, blocked sectors, kind masks, radius clamping, reachability,
 *    nearest sector, snapping, hints, an empty section,
 * 2. hostile input on the synthetic section (every truncation, every byte flipped three ways) and on a real one,
 * 3. per real map (a path pack and the coll pack of the same map): every BSP opens; sector polygons planar, convex and counter-
 *    clockwise; links symmetric with consistent portals; points inside the collision bounds; sectors against the collision
 *    surfaces (coll_ray from above, coll_surface_info of the source surface); locate; path_find against a brute-force Dijkstra
 *    on the sector graph (costs equal, completeness, partial end sector); reachability symmetry; every leg of the routes
 *    against coll_sphere_sweep; PATH_BENCH=1 (PATH_BENCH_BSP=<n>): queries per millisecond of the first pack's BSP n;
 *    PATH_VERBOSE=1 / PATH_VERBOSE2=1: describe / list the legs the capsule cannot follow. */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c11 on glibc */
#include "ai/path.h"
#include "stream/hp2.h"
#include "world/coll.h"
#include <math.h>
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
static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

/* ---- loading -------------------------------------------------------------------------------------------- */
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
	raw = malloc(sec->raw_size ? sec->raw_size : 1);
	if (!raw || hp2_load(pk, sec, raw, sec->raw_size) != HP2_OK) { free(raw); return NULL; }
	*n = sec->raw_size;
	return raw;
}
static char tag_char(uint32_t i) { return (char)(i < 10 ? '0' + i : 'A' + (i - 10)); }

typedef struct {
	path_bsp b;
	path_scratch *sc;
	void *mem;
	path_query q;
} pw;

static int pw_init(pw *w, const uint8_t *data, size_t size)
{
	memset(w, 0, sizeof *w);
	if (path_open(data, size, &w->b) != PATH_OK) return 0;
	w->sc = malloc(sizeof *w->sc);
	w->mem = malloc(path_scratch_bytes(w->b.sector_count) + 8);
	if (!w->sc || !w->mem || !path_scratch_init(w->sc, w->mem, path_scratch_bytes(w->b.sector_count) + 8, w->b.sector_count)) return 0;
	path_query_default(&w->q);
	return 1;
}
static void pw_free(pw *w) { free(w->sc); free(w->mem); memset(w, 0, sizeof *w); }

/* ---- touching everything an opened section hands out ---------------------------------------------------- */
static void walk_all(const path_bsp *b)
{
	float acc = 0.0f, lo[3], hi[3];
	uint32_t i, k, n = b->sector_count < 3000 ? b->sector_count : 3000;
	path_scratch *sc = malloc(sizeof *sc);
	void *mem = malloc(path_scratch_bytes(b->sector_count) + 8);
	path_query q;
	path_waypoint wp[64];
	path_link lk[8];
	if (!sc || !mem || !path_scratch_init(sc, mem, path_scratch_bytes(b->sector_count) + 8, b->sector_count)) abort();
	for (i = 0; i < 3; i++) { lo[i] = b->bounds[i] - 1.0f; hi[i] = b->bounds[3 + i] + 1.0f; }
	path_query_default(&q);
	for (i = 0; i < n; i++) {
		path_sector_info si;
		float P[PATH_MAX_POLY][3];
		uint32_t m, j;
		if (!path_sector_get(b, i, &si)) abort();
		m = path_sector_polygon(b, i, P);
		if (m < 3 || m > PATH_MAX_POLY) abort();
		for (j = 0; j < m; j++) acc += P[j][0] + P[j][1] + P[j][2];
		acc += si.centroid[2] + (float)si.comp + path_sector_height(b, i, si.centroid[0], si.centroid[1]);
		for (j = 0; j < si.link_count && j < 64; j++) { path_link l; if (!path_link_get(b, i, j, &l)) abort(); acc += l.dz + (float)l.cost + l.right[0]; }
		acc += (float)path_sector_hints(b, i, ~PATH_KINDS_WALKING, lk, 8);
	}
	for (i = 0; i < 60; i++) {
		float a[3], c[3], out[3], d;
		int32_t s;
		int k2;
		for (k = 0; k < 3; k++) { a[k] = frange(lo[k], hi[k]); c[k] = frange(lo[k], hi[k]); }
		s = path_locate(b, a);
		acc += (float)s;
		s = path_nearest_sector(b, a, 5.0f, &d, out);
		if (s >= 0) acc += d + out[0];
		if (b->sector_count) {
			uint32_t a_s = rnd() % b->sector_count, b_s = rnd() % b->sector_count;
			q.kinds = rnd() & PATH_KINDS_ALL;
			q.height = frange(0.0f, 10.0f);
			q.max_expand = (i & 1u) ? 40u : 0u;
			if (path_find(b, sc, &q, a_s, b_s, NULL)) acc += 1.0f;
			acc += (float)path_sector_reachable(b, sc, &q, a_s, b_s);
			path_snap(b, a_s, a, 0.2f, out);
			acc += out[0];
		}
		k2 = path_route(b, sc, &q, a, c, wp, 64, NULL);
		for (k = 0; k < (uint32_t)k2; k++) acc += wp[k].pos[0];
		if (b->sector_count) {
			float pa[3], pb[3];
			path_sector_info s1, s2;
			uint32_t x = rnd() % b->sector_count, y = rnd() % b->sector_count;
			path_sector_get(b, x, &s1); path_sector_get(b, y, &s2);
			pa[0] = s1.centroid[0]; pa[1] = s1.centroid[1]; pa[2] = s1.centroid[2];
			pb[0] = s2.centroid[0]; pb[1] = s2.centroid[1]; pb[2] = s2.centroid[2];
			path_query_default(&q);
			q.kinds = rnd() & PATH_KINDS_ALL;
			k2 = path_route(b, sc, &q, pa, pb, wp, 64, NULL);
			for (k = 0; k < (uint32_t)k2; k++) acc += wp[k].pos[1];
		}
	}
	sink = acc;
	free(sc); free(mem);
}

/* ---- 1. the synthetic world ------------------------------------------------------------------------- */
enum { A1, A2, SC_, B, D, E, F, H, LT, G, NSEC };
static int32_t ID[NSEC];

static int32_t at(const pw *w, float x, float y, float z) { float p[3]; p[0] = x; p[1] = y; p[2] = z; return path_locate(&w->b, p); }

static int route_to(pw *w, const path_query *q, float fx, float fy, float fz, float tx, float ty, float tz, path_waypoint *out, int max, path_route_info *inf)
{
	float a[3], c[3];
	a[0] = fx; a[1] = fy; a[2] = fz; c[0] = tx; c[1] = ty; c[2] = tz;
	return path_route(&w->b, w->sc, q, a, c, out, max, inf);
}
static int wp_is(const path_waypoint *p, float x, float y, float z, uint32_t kind, uint32_t flags)
{
	return NEAR(p->pos[0], x, 2e-3f) && NEAR(p->pos[1], y, 2e-3f) && NEAR(p->pos[2], z, 2e-3f) && p->kind == kind && p->flags == flags;
}

static int test_synthetic(const uint8_t *data, size_t size, const uint8_t *edata, size_t esize)
{
	pw w, ew;
	path_waypoint wp[16];
	path_route_info inf;
	path_query q;
	path_link lk[8];
	path_sector_info si;
	float P[PATH_MAX_POLY][3], pt[3], dist;
	uint32_t i, j, n;
	int cnt;

	CHECK(pw_init(&w, data, size));
	CHECK(w.b.sector_count == 13 && w.b.link_count == 18 && w.b.comp_count == 6 && w.b.cost_scale == 64);
	CHECK(NEAR(w.b.step_max, 0.2f, 1e-6f) && NEAR(w.b.vault_max, 0.55f, 1e-6f) && NEAR(w.b.drop_max, 3.0f, 1e-6f));
	CHECK(NEAR(w.b.bounds[0], 0.0f, 1e-6f) && NEAR(w.b.bounds[3], 56.0f, 1e-6f) && NEAR(w.b.bounds[4], 13.0f, 1e-6f) && NEAR(w.b.bounds[2], -1.4f, 1e-5f) && NEAR(w.b.bounds[5], 1.0f, 1e-6f));
	/* locate */
	ID[A1] = at(&w, 1.5f, 2, 0); ID[A2] = at(&w, 4.5f, 2, 0); ID[SC_] = at(&w, 7, 2, 0); ID[B] = at(&w, 10, 2, 0); ID[D] = at(&w, 13, 2, 0.15f);
	ID[E] = at(&w, 15, 2, 0.6f); ID[F] = at(&w, 17, 2, -1.4f); ID[H] = at(&w, 20, 2, 1.0f); ID[LT] = at(&w, 18.9f, 1.15f, 1.0f); ID[G] = at(&w, 1.5f, 11.5f, 0);
	for (i = 0; i < NSEC; i++) { CHECK(ID[i] >= 0); for (j = 0; j < i; j++) CHECK(ID[i] != ID[j]); }
	CHECK(at(&w, 31, 1, 0) < 0 && at(&w, 41, 1, 1) < 0 && at(&w, -1, 1, 0) < 0 && at(&w, 1.5f, 6, 0) < 0 && at(&w, 1.5f, 2, -0.5f) < 0);
	CHECK(at(&w, 1.5f, 2, 0.35f) == ID[A1] && at(&w, 1.5f, 2, -0.29f) == ID[A1] && at(&w, 1.5f, 2, -0.31f) < 0 && at(&w, 1.5f, 2, 3.9f) == ID[A1] && at(&w, 1.5f, 2, 4.1f) < 0);
	{ float fz = 0; pt[0] = 13; pt[1] = 2; pt[2] = 5;
	  CHECK(path_locate_ex(&w.b, pt, 0.3f, 6.0f, &fz) == ID[D] && NEAR(fz, 0.15f, 1e-5f));
	  pt[0] = 3.0f; pt[1] = 2;                                                    /* exactly on the shared edge of A1 and A2 */
	  pt[2] = 0;  CHECK(path_locate(&w.b, pt) == ID[A1] || path_locate(&w.b, pt) == ID[A2]);
	  pt[0] = 0.0f; pt[1] = 0.0f; CHECK(path_locate(&w.b, pt) == ID[A1]);     /* a corner counts as inside */ }
	/* sector data */
	CHECK(path_sector_get(&w.b, (uint32_t)ID[D], &si) && NEAR(si.centroid[0], 13, 1e-5f) && NEAR(si.centroid[2], 0.15f, 1e-5f) && si.nverts == 4 && si.flags == 0);
	CHECK(path_sector_get(&w.b, (uint32_t)ID[A2], &si) && si.nverts == 6 && si.material == 5 && si.link_count == 2);
	CHECK(path_sector_get(&w.b, (uint32_t)ID[A1], &si) && si.nverts == 4 && si.material == 2 && si.surface == 0);
	CHECK(path_sector_get(&w.b, (uint32_t)ID[LT], &si) && (si.flags & PATH_SF_CLIMBABLE) && si.link_count == 0);
	CHECK(!path_sector_get(&w.b, 13, &si) && !path_sector_get(&w.b, 0xFFFFFFFFu, &si));
	{ path_sector_info a, b2, c; path_sector_get(&w.b, (uint32_t)ID[A1], &a); path_sector_get(&w.b, (uint32_t)ID[D], &b2); path_sector_get(&w.b, (uint32_t)ID[E], &c);
	  CHECK(a.comp == b2.comp && c.comp != a.comp); }
	n = path_sector_polygon(&w.b, (uint32_t)ID[A2], P);
	CHECK(n == 6 && NEAR(P[0][0], 3, 1e-6f));
	{ float area = 0; for (i = 0; i < n; i++) area += P[i][0] * P[(i + 1) % n][1] - P[(i + 1) % n][0] * P[i][1]; CHECK(NEAR(area * 0.5f, 12.0f, 1e-4f)); }   /* counter-clockwise, 3 x 4 */
	CHECK(path_sector_polygon(&w.b, 13, P) == 0);
	CHECK(NEAR(path_sector_height(&w.b, (uint32_t)ID[D], 13, 2), 0.15f, 1e-5f) && NEAR(path_sector_height(&w.b, (uint32_t)ID[F], 100, -50), -1.4f, 1e-4f));
	{ int32_t q1 = at(&w, 51, 2, 0), q2 = at(&w, 53, 2, 0), q3 = at(&w, 55, 2, 0);
	  path_sector_info a, b2, c;
	  CHECK(q1 >= 0 && q2 >= 0 && q3 >= 0 && q1 != q2 && q2 != q3);
	  path_sector_get(&w.b, (uint32_t)q1, &a); path_sector_get(&w.b, (uint32_t)q2, &b2); path_sector_get(&w.b, (uint32_t)q3, &c);
	  CHECK(NEAR(a.headroom, 255.0f / 32.0f, 1e-6f) && NEAR(b2.headroom, 9.0f / 32.0f, 1e-6f) && NEAR(c.headroom, 255.0f / 32.0f, 1e-6f));
	  CHECK(a.comp == b2.comp && b2.comp == c.comp);
	  path_sector_get(&w.b, (uint32_t)ID[A1], &a); CHECK(NEAR(a.headroom, 255.0f / 32.0f, 1e-6f));
	  /* the headroom filter: a 0.5 wu actor does not enter Q2, a 0.25 one does */
	  { path_query hq; path_route_info hi;
	    path_query_default(&hq);
	    CHECK(hq.height == 0.5f);
	    CHECK(path_find(&w.b, w.sc, &hq, (uint32_t)q1, (uint32_t)q3, &hi) == 0 && hi.status == PATH_ROUTE_PARTIAL && hi.end_sector == q1 && hi.cost == 0);
	    CHECK(path_find(&w.b, w.sc, &hq, (uint32_t)q2, (uint32_t)q3, &hi) == 1 && hi.cost == 128u);               /* the start may be low */
	    CHECK(path_find(&w.b, w.sc, &hq, (uint32_t)q1, (uint32_t)q2, &hi) == 0);                                  /* the goal may not */
	    hq.height = 0.25f;
	    CHECK(path_find(&w.b, w.sc, &hq, (uint32_t)q1, (uint32_t)q3, &hi) == 1 && hi.cost == 256u && hi.sector_count == 3);
	    hq.height = 0.0f;
	    CHECK(path_find(&w.b, w.sc, &hq, (uint32_t)q3, (uint32_t)q1, &hi) == 1 && hi.cost == 256u);
	    hq.height = 9.0f;                                                                                          /* taller than anything measurable: only unobstructed sectors */
	    CHECK(path_find(&w.b, w.sc, &hq, (uint32_t)ID[A1], (uint32_t)ID[A2], &hi) == 1);
	    hq.height = 0.5f;
	    CHECK(!path_sector_reachable(&w.b, w.sc, &hq, (uint32_t)q1, (uint32_t)q3) && path_sector_reachable(&w.b, w.sc, &hq, (uint32_t)q2, (uint32_t)q3));
	    hq.height = 0.0f;
	    CHECK(path_sector_reachable(&w.b, w.sc, &hq, (uint32_t)q1, (uint32_t)q3) && path_sector_reachable(&w.b, w.sc, &hq, (uint32_t)q3, (uint32_t)q1)); } }
	/* links */
	CHECK(path_link_count(&w.b, (uint32_t)ID[A1]) == 1 && path_link_count(&w.b, (uint32_t)ID[G]) == 0 && path_link_count(&w.b, 99) == 0);
	CHECK(path_link_get(&w.b, (uint32_t)ID[A1], 0, &lk[0]) && lk[0].to == ID[A2] && lk[0].kind == PATH_LINK_WALK && lk[0].cost == 192u && lk[0].dz == 0.0f && NEAR(lk[0].width, 4.0f, 1e-6f));
	CHECK(NEAR(lk[0].right[0], 3, 1e-6f) && NEAR(lk[0].right[1], 0, 1e-6f) && NEAR(lk[0].left[1], 4, 1e-6f));
	CHECK(!path_link_get(&w.b, (uint32_t)ID[A1], 1, &lk[0]));
	CHECK(path_link_get(&w.b, (uint32_t)ID[A2], 1, &lk[0]) && lk[0].to == ID[SC_] && lk[0].cost == 160u && NEAR(lk[0].width, 1.0f, 1e-6f));
	CHECK(NEAR(lk[0].right[1], 1.5f, 1e-6f) && NEAR(lk[0].left[1], 2.5f, 1e-6f));                  /* the run is the doorway only */
	n = path_sector_hints(&w.b, (uint32_t)ID[B], PATH_KIND(PATH_LINK_STEP), lk, 8);
	CHECK(n == 1 && lk[0].to == ID[D] && lk[0].cost == 204u && NEAR(lk[0].dz, 0.1484375f, 1e-6f) && NEAR(lk[0].right[2], 0.0f, 1e-6f));
	n = path_sector_hints(&w.b, (uint32_t)ID[D], ~PATH_KINDS_WALKING, lk, 8);
	CHECK(n == 1 && lk[0].kind == PATH_LINK_VAULT && lk[0].to == ID[E] && lk[0].cost == 210u && NEAR(lk[0].dz, 0.44921875f, 1e-6f));
	n = path_sector_hints(&w.b, (uint32_t)ID[E], ~PATH_KINDS_WALKING, lk, 8);
	CHECK(n == 2 && lk[0].kind == PATH_LINK_DROP && lk[1].kind == PATH_LINK_DROP && lk[0].to == ID[D] && lk[0].cost == 191u && lk[1].to == ID[F] && lk[1].cost == 372u && NEAR(lk[1].dz, -2.0f, 1e-6f));
	CHECK(path_sector_hints(&w.b, (uint32_t)ID[E], ~PATH_KINDS_WALKING, lk, 1) == 1 && path_sector_hints(&w.b, (uint32_t)ID[E], PATH_KIND(PATH_LINK_VAULT), lk, 8) == 0);
	n = path_sector_hints(&w.b, (uint32_t)ID[F], PATH_KINDS_ALL, lk, 8);
	CHECK(n == 1 && lk[0].kind == PATH_LINK_CLIMB && lk[0].to == ID[H] && lk[0].cost == 510u && NEAR(lk[0].right[0], 18.9f, 1e-5f) && NEAR(lk[0].right[2], -1.4f, 1e-5f) && lk[0].right[1] == lk[0].left[1] && lk[0].width == 255.0f / 16.0f);
	CHECK(path_sector_hints(&w.b, (uint32_t)ID[H], PATH_KINDS_ALL, lk, 8) == 2 && lk[0].kind == PATH_LINK_DROP && lk[1].kind == PATH_LINK_CLIMB);

	/* A*: costs of the sector route, with and without kinds */
	path_query_default(&q);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[B], &inf) == 1);
	CHECK(inf.status == PATH_ROUTE_COMPLETE && inf.cost == 544u && inf.sector_count == 4 && w.sc->route_sector[0] == ID[A1] && w.sc->route_sector[1] == ID[A2] &&
	      w.sc->route_sector[2] == ID[SC_] && w.sc->route_sector[3] == ID[B] && w.sc->route_links == 3 && inf.end_sector == ID[B]);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[A1], &inf) == 1 && inf.cost == 0 && inf.sector_count == 1);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[D], &inf) == 1 && inf.cost == 544u + 204u);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[E], &inf) == 0 && inf.status == PATH_ROUTE_PARTIAL && inf.end_sector == ID[D] && inf.cost == 544u + 204u);
	q.kinds |= PATH_KIND(PATH_LINK_VAULT);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[E], &inf) == 1 && inf.cost == 544u + 204u + 210u && inf.sector_count == 6);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[E], (uint32_t)ID[A1], &inf) == 0);              /* DROP not allowed yet */
	q.kinds |= PATH_KIND(PATH_LINK_DROP);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[E], (uint32_t)ID[F], &inf) == 0);               /* 2.0 down > max_drop 1.5 */
	q.max_drop = 2.5f;
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[E], (uint32_t)ID[F], &inf) == 1 && inf.cost == 372u);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[E], (uint32_t)ID[A1], &inf) == 1 && inf.cost == 191u + 204u + 192u + 160u + 192u);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[H], &inf) == 0);              /* no climb */
	q.kinds = PATH_KINDS_ALL;
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[H], &inf) == 1 && inf.cost == 544u + 204u + 210u + 372u + 510u);
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[LT], &inf) == 0 && inf.status == PATH_ROUTE_PARTIAL);   /* the ladder's cap is an island */
	CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[G], &inf) == 0 && inf.status == PATH_ROUTE_PARTIAL && inf.end_sector != ID[G]);
	CHECK(path_find(&w.b, w.sc, &q, 99, (uint32_t)ID[G], &inf) == 0 && inf.status == PATH_ROUTE_NO_START);
	/* blocked sectors */
	path_query_default(&q);
	{ uint32_t bl[1] = { 0 };
	  CHECK(path_block_words(13) == 1 && path_block_words(32) == 1 && path_block_words(33) == 2 && path_block_words(0) == 0);
	  path_block_set(bl, (uint32_t)ID[SC_], 1);
	  CHECK(bl[0] == 1u << ID[SC_]);
	  path_block_set(bl, (uint32_t)ID[B], 1); path_block_set(bl, (uint32_t)ID[B], 0);
	  CHECK(bl[0] == 1u << ID[SC_]);
	  q.blocked = bl;
	  CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[B], &inf) == 0 && inf.end_sector == ID[A2]);
	  CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[SC_], (uint32_t)ID[B], &inf) == 1 && inf.cost == 192u);          /* the start itself may be blocked */
	  CHECK(path_find(&w.b, w.sc, &q, (uint32_t)ID[B], (uint32_t)ID[SC_], &inf) == 0);                              /* the goal may not */
	  CHECK(!path_sector_reachable(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[B]) && path_sector_reachable(&w.b, w.sc, &q, (uint32_t)ID[A1], (uint32_t)ID[A2]));
	  q.blocked = NULL; }
	/* funnel: hand-computed corners. The shrunk portal end (8, 2.3) is 0.2 from the doorway's corner (8, 2.5), but the leg to the goal
	 * would pass it at 0.171: the corner slides down the portal until both legs keep 0.2 from the corner (3.96 s^2 - 0.08 s - 0.2 = 0,
	 * s = 0.2351 from the corner: y = 2.2649) */
	path_query_default(&q);
	cnt = route_to(&w, &q, 1.0f, 0.5f, 0, 10.0f, 3.5f, 0, wp, 16, &inf);
	CHECK(cnt == 2 && inf.status == PATH_ROUTE_COMPLETE && inf.cost == 544u && wp_is(&wp[0], 8.0f, 2.265f, 0, 0, 0) && wp_is(&wp[1], 10.0f, 3.5f, 0, 0, PATH_WPF_GOAL));
	CHECK(wp[0].sector == ID[SC_] && wp[1].sector == ID[B]);
	cnt = route_to(&w, &q, 1.0f, 3.5f, 0, 10.0f, 0.5f, 0, wp, 16, &inf);                  /* mirrored: the same at the south end of the doorway at x = 8 (y = 1.7351) */
	CHECK(cnt == 2 && wp_is(&wp[0], 8.0f, 1.7351f, 0, 0, 0) && wp_is(&wp[1], 10.0f, 0.5f, 0, 0, PATH_WPF_GOAL));
	cnt = route_to(&w, &q, 1.5f, 2, 0, 10, 2, 0, wp, 16, &inf);                           /* straight through the doorway */
	CHECK(cnt == 1 && wp_is(&wp[0], 10, 2, 0, 0, PATH_WPF_GOAL));
	cnt = route_to(&w, &q, 1.5f, 2, 0, 13, 2, 0.15f, wp, 16, &inf);                       /* across the step */
	CHECK(cnt == 1 && inf.status == PATH_ROUTE_COMPLETE && wp_is(&wp[0], 13, 2, 0.15f, 0, PATH_WPF_GOAL) && inf.cost == 748u);
	cnt = route_to(&w, &q, 1.5f, 2, 0, 1.5f, 2, 0, wp, 16, &inf);                         /* start = goal */
	CHECK(cnt == 1 && wp_is(&wp[0], 1.5f, 2, 0, 0, PATH_WPF_GOAL) && inf.cost == 0);
	{ path_query r6 = q; r6.radius = 0.45f;                                               /* the doorway (1.0 wide) shrinks to 0.1 around its middle */
	  cnt = route_to(&w, &r6, 1.0f, 0.5f, 0, 10.0f, 3.5f, 0, wp, 16, &inf);
	  CHECK(cnt == 3 && inf.status == PATH_ROUTE_COMPLETE && wp_is(&wp[0], 6, 1.9689f, 0, 0, 0) && wp_is(&wp[1], 8, 2.0f, 0, 0, 0) && wp_is(&wp[2], 10, 3.5f, 0, 0, PATH_WPF_GOAL));
	  r6.radius = 0.6f;                                                                     /* wider than the doorway: the route stops in A2 (and is pulled in from its walls) */
	  cnt = route_to(&w, &r6, 1.0f, 0.5f, 0, 10.0f, 3.5f, 0, wp, 16, &inf);
	  CHECK(cnt == 1 && inf.status == PATH_ROUTE_PARTIAL && inf.end_sector == ID[A2] && wp_is(&wp[0], 5.4f, 3.4f, 0, 0, PATH_WPF_GOAL));
	  CHECK(!path_sector_reachable(&w.b, w.sc, &r6, (uint32_t)ID[A1], (uint32_t)ID[B]));
	  r6.radius = 0.5f;                                                                     /* exactly the doorway: still passes (1.0 >= 2 * 0.5) */
	  CHECK(path_find(&w.b, w.sc, &r6, (uint32_t)ID[A1], (uint32_t)ID[B], &inf) == 1);
	  cnt = route_to(&w, &r6, 1.0f, 0.5f, 0, 10.0f, 3.5f, 0, wp, 16, &inf);
	  CHECK(cnt == 3 && wp_is(&wp[0], 6, 2, 0, 0, 0) && wp_is(&wp[1], 8, 2, 0, 0, 0)); }          /* the doorway collapses to its middle */
	cnt = route_to(&w, &q, 1.0f, 0.5f, 0, 10.0f, 3.5f, 0, wp, 1, &inf);                    /* a short waypoint buffer */
	CHECK(cnt == 1 && inf.truncated && wp_is(&wp[0], 8.0f, 2.3f, 0, 0, 0));
	/* partial route: the vault is not allowed; the best point is on D next to E */
	cnt = route_to(&w, &q, 1.5f, 2, 0, 15, 2, 0.6f, wp, 16, &inf);
	CHECK(cnt == 1 && inf.status == PATH_ROUTE_PARTIAL && inf.end_sector == ID[D] && wp_is(&wp[0], 13.8f, 2, 0.15f, 0, PATH_WPF_GOAL));
	cnt = route_to(&w, &q, 1.5f, 2, 0, 1.5f, 11.5f, 0, wp, 16, &inf);                      /* island: partial, ends on the A1 sector's side towards G */
	CHECK(cnt == 1 && inf.status == PATH_ROUTE_PARTIAL && wp_is(&wp[0], 1.5f, 3.8f, 0, 0, PATH_WPF_GOAL));
	/* actions */
	{ path_query v = q; v.kinds |= PATH_KIND(PATH_LINK_VAULT);
	  cnt = route_to(&w, &v, 13, 2, 0.15f, 15, 2, 0.6f, wp, 16, &inf);
	  CHECK(cnt == 3 && inf.status == PATH_ROUTE_COMPLETE && wp_is(&wp[0], 14, 2, 0.15f, PATH_LINK_VAULT, 0) && wp_is(&wp[1], 14.2f, 2, 0.6f, 0, PATH_WPF_LANDING) &&
	        wp_is(&wp[2], 15, 2, 0.6f, 0, PATH_WPF_GOAL));
	  CHECK(wp[0].sector == ID[D] && wp[1].sector == ID[E] && wp[2].sector == ID[E]); }
	{ path_query d2 = q; d2.kinds |= PATH_KIND(PATH_LINK_DROP); d2.max_drop = 3.0f;
	  cnt = route_to(&w, &d2, 15, 2, 0.6f, 17, 2, -1.4f, wp, 16, &inf);
	  CHECK(cnt == 3 && wp_is(&wp[0], 16, 2, 0.6f, PATH_LINK_DROP, 0) && wp_is(&wp[1], 16.2f, 2, -1.4f, 0, PATH_WPF_LANDING) && wp_is(&wp[2], 17, 2, -1.4f, 0, PATH_WPF_GOAL));
	  d2.max_drop = 1.5f;
	  cnt = route_to(&w, &d2, 15, 2, 0.6f, 17, 2, -1.4f, wp, 16, &inf);
	  CHECK(inf.status == PATH_ROUTE_PARTIAL && cnt >= 1); }
	{ path_query c2 = q; c2.kinds |= PATH_KIND(PATH_LINK_CLIMB);
	  cnt = route_to(&w, &c2, 17, 2, -1.4f, 20, 2, 1.0f, wp, 16, &inf);
	  CHECK(cnt == 3 && wp_is(&wp[0], 18.9f, 1.15f, -1.4f, PATH_LINK_CLIMB, 0) && wp_is(&wp[1], 19.2f, 1.15f, 1.0f, 0, PATH_WPF_LANDING) && wp_is(&wp[2], 20, 2, 1.0f, 0, PATH_WPF_GOAL)); }
	/* start / goal off the mesh snap to the nearest sector, or fail */
	cnt = route_to(&w, &q, 1.5f, 2, 0, 1.5f, 5.0f, 0, wp, 16, &inf);                       /* goal off the mesh, 1 wu north of A1 */
	CHECK(cnt == 1 && inf.status == PATH_ROUTE_COMPLETE && inf.goal_sector == ID[A1] && wp_is(&wp[0], 1.5f, 3.8f, 0, 0, PATH_WPF_GOAL));
	cnt = route_to(&w, &q, 1.5f, 50.0f, 0, 1.5f, 2, 0, wp, 16, &inf);
	CHECK(cnt == 0 && inf.status == PATH_ROUTE_NO_START);
	cnt = route_to(&w, &q, 1.5f, 2, 0, 1.5f, 50.0f, 0, wp, 16, &inf);
	CHECK(cnt == 0 && inf.status == PATH_ROUTE_NO_GOAL);
	/* reachability: symmetric kinds are symmetric; components agree */
	{ path_query s2 = q; s2.kinds = PATH_KINDS_SYMMETRIC; s2.height = 0.0f;
	  for (i = 0; i < NSEC; i++)
		  for (j = 0; j < NSEC; j++) {
			  int ab = path_sector_reachable(&w.b, w.sc, &s2, (uint32_t)ID[i], (uint32_t)ID[j]), ba = path_sector_reachable(&w.b, w.sc, &s2, (uint32_t)ID[j], (uint32_t)ID[i]);
			  path_sector_info x, y;
			  path_sector_get(&w.b, (uint32_t)ID[i], &x); path_sector_get(&w.b, (uint32_t)ID[j], &y);
			  CHECK(ab == ba && ab == (x.comp == y.comp));
		  }
	  CHECK(path_sector_reachable(&w.b, w.sc, &s2, (uint32_t)ID[F], (uint32_t)ID[H]) && !path_sector_reachable(&w.b, w.sc, &s2, (uint32_t)ID[A1], (uint32_t)ID[F]));
	  s2.kinds = PATH_KINDS_ALL; s2.max_drop = 3.0f; s2.height = 0.5f;
	  CHECK(path_sector_reachable(&w.b, w.sc, &s2, (uint32_t)ID[A1], (uint32_t)ID[H]) && !path_sector_reachable(&w.b, w.sc, &s2, (uint32_t)ID[H], (uint32_t)ID[A1]));   /* one-way drops */
	  s2.kinds = PATH_KINDS_WALKING;
	  { float f[3] = { 1.5f, 2, 0 }, t[3] = { 13, 2, 0.15f }, u[3] = { 15, 2, 0.6f };
	    CHECK(path_reachable(&w.b, w.sc, &s2, f, t) && !path_reachable(&w.b, w.sc, &s2, f, u)); } }
	/* nearest sector and snapping */
	pt[0] = 23; pt[1] = 1; pt[2] = 1.0f;
	{ float cp[3];
	  CHECK(path_nearest_sector(&w.b, pt, 5.0f, &dist, cp) == ID[H] && NEAR(dist, 1.0f, 1e-5f) && NEAR(cp[0], 22, 1e-5f) && NEAR(cp[1], 1, 1e-5f) && NEAR(cp[2], 1.0f, 1e-5f));
	  pt[0] = 30; CHECK(path_nearest_sector(&w.b, pt, 5.0f, &dist, cp) < 0);
	  CHECK(path_nearest_sector(&w.b, pt, 12.0f, NULL, NULL) == ID[H]);
	  pt[0] = 1.5f; pt[1] = 2; pt[2] = 2.0f; CHECK(path_nearest_sector(&w.b, pt, 5.0f, &dist, cp) == ID[A1] && NEAR(dist, 2.0f, 1e-5f)); }
	{ float o[3];
	  pt[0] = 0.05f; pt[1] = 0.05f; pt[2] = 0;
	  CHECK(path_snap(&w.b, (uint32_t)ID[A1], pt, 0.2f, o) == 1 && NEAR(o[0], 0.2f, 2e-3f) && NEAR(o[1], 0.2f, 2e-3f) && NEAR(o[2], 0, 1e-5f));
	  pt[0] = -3; pt[1] = 2; CHECK(path_snap(&w.b, (uint32_t)ID[A1], pt, 0.2f, o) == 1 && NEAR(o[0], 0.2f, 2e-3f) && NEAR(o[1], 2, 1e-4f));
	  pt[0] = 1.5f; pt[1] = 2; CHECK(path_snap(&w.b, (uint32_t)ID[A1], pt, 0.2f, o) == 1 && NEAR(o[0], 1.5f, 1e-5f) && NEAR(o[1], 2, 1e-5f));
	  pt[0] = 18.95f; pt[1] = 1.2f; pt[2] = 1.0f;
	  CHECK(path_snap(&w.b, (uint32_t)ID[LT], pt, 0.2f, o) == 0 && NEAR(o[0], 18.9f, 1e-5f) && NEAR(o[1], 1.15f, 1e-5f));          /* too narrow: centre */
	  CHECK(path_snap(&w.b, (uint32_t)ID[LT], pt, 0.0f, o) == 1 && NEAR(o[0], 18.95f, 1e-5f));
	  CHECK(path_snap(&w.b, 99, pt, 0.2f, o) == 0); }
	/* the scratch is a plain caller buffer */
	{ path_scratch s3; uint8_t tiny[16] __attribute__((aligned(4)));
	  CHECK(!path_scratch_init(&s3, tiny, sizeof tiny, 10) && !path_scratch_init(&s3, NULL, 0, 10) && path_scratch_bytes(0) >= 8); }

	/* an empty section */
	CHECK(pw_init(&ew, edata, esize) && ew.b.sector_count == 0 && ew.b.link_count == 0 && ew.b.comp_count == 0);
	pt[0] = 0; pt[1] = 0; pt[2] = 0;
	CHECK(path_locate(&ew.b, pt) < 0 && path_nearest_sector(&ew.b, pt, 100.0f, NULL, NULL) < 0);
	cnt = path_route(&ew.b, ew.sc, &ew.q, pt, pt, wp, 16, &inf);
	CHECK(cnt == 0 && inf.status == PATH_ROUTE_NO_START);
	CHECK(!path_find(&ew.b, ew.sc, &ew.q, 0, 0, &inf) && !path_sector_reachable(&ew.b, ew.sc, &ew.q, 0, 0));
	walk_all(&ew.b);
	pw_free(&ew);
	walk_all(&w.b);
	pw_free(&w);
	printf("[path] synthetic world: ok\n");
	return 0;
}

/* ---- 2. hostile input ------------------------------------------------------------------------------------- */
static int test_hostile(const uint8_t *data, size_t size, int full)
{
	uint8_t *buf = malloc(size + 16), *copy;
	size_t cut, i;
	uint32_t opened = 0, flips = 0, k;
	path_bsp b;
	CHECK(buf);
	/* every truncation (the real section: a sample) */
	for (cut = 0; cut < size; cut += full ? 1 : 257u + (rnd() & 255u)) {
		memcpy(buf, data, cut);
		if (path_open(buf, cut, &b) == PATH_OK) abort();           /* the size field must catch it */
	}
	copy = malloc(size);
	CHECK(copy);
	if (full) {
		for (i = 0; i < size; i++) {
			static const uint8_t xors[3] = { 0xFF, 0x01, 0x80 };
			for (k = 0; k < 3; k++) {
				memcpy(copy, data, size);
				copy[i] ^= xors[k];
				flips++;
				if (path_open(copy, size, &b) == PATH_OK) { opened++; walk_all(&b); }
			}
		}
	} else {
		for (i = 0; i < 400; i++) {
			uint32_t at_ = rnd() % (uint32_t)size;
			memcpy(copy, data, size);
			copy[at_] ^= (uint8_t)(1u << (rnd() & 7u));
			flips++;
			if (path_open(copy, size, &b) == PATH_OK) { opened++; if (i % 4 == 0) walk_all(&b); }
		}
	}
	printf("[path] hostile input: %zu bytes, %u flips, %u still open (all walked)\n", size, flips, opened);
	free(buf); free(copy);
	return 0;
}

/* ---- 3. real maps ---------------------------------------------------------------------------------------- */
static void sample_in(const path_bsp *b, uint32_t s, float shrink, float p[3])
{
	float P[PATH_MAX_POLY][3], c[3], area[PATH_MAX_POLY], tot = 0.0f, r;
	path_sector_info si;
	uint32_t n = path_sector_polygon(b, s, P), i, t = 1;
	float u, v;
	path_sector_get(b, s, &si);
	c[0] = si.centroid[0]; c[1] = si.centroid[1]; c[2] = si.centroid[2];
	for (i = 1; i + 1 < n; i++) {                                    /* fan triangles by area */
		float ar = fabsf((P[i][0] - P[0][0]) * (P[i + 1][1] - P[0][1]) - (P[i + 1][0] - P[0][0]) * (P[i][1] - P[0][1]));
		area[i] = ar; tot += ar;
	}
	r = frand() * tot;
	for (i = 1; i + 1 < n; i++) { if (r <= area[i]) { t = i; break; } r -= area[i]; t = i; }
	u = frand(); v = frand();
	if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
	p[0] = P[0][0] + u * (P[t][0] - P[0][0]) + v * (P[t + 1][0] - P[0][0]);
	p[1] = P[0][1] + u * (P[t][1] - P[0][1]) + v * (P[t + 1][1] - P[0][1]);
	p[0] = c[0] + (p[0] - c[0]) * shrink; p[1] = c[1] + (p[1] - c[1]) * shrink;
	p[2] = path_sector_height(b, s, p[0], p[1]);
}

/* the capsule (a sphere of radius r resting on the floor) follows a leg like the player controller does: plan steps of about 0.1 wu,
 * a step up of at most 0.17 when blocked, then settle on the ground with a sweep down (up to 0.45); a drop or a wall ends it */
static int walk_why;
static int walk_leg(const coll_bsp *cb, const float a[3], const float c[3], float r)
{
	walk_why = 0;
	float pos[3], dir[2], len = hypotf(c[0] - a[0], c[1] - a[1]), done = 0.0f, ds = fmaxf(0.1f, len / 400.0f);
	coll_hit h;
	pos[0] = a[0]; pos[1] = a[1]; pos[2] = a[2] + r + 0.01f;
	if (len < 1e-4f) return 1;
	dir[0] = (c[0] - a[0]) / len; dir[1] = (c[1] - a[1]) / len;
	while (done < len - 1e-4f) {
		float step = fminf(ds, len - done), tgt[3], lift[3], l2[3], down[3];
		tgt[0] = pos[0] + dir[0] * step; tgt[1] = pos[1] + dir[1] * step; tgt[2] = pos[2];
		if (!coll_sphere_sweep(cb, pos, tgt, r, &h)) {
			pos[0] = tgt[0]; pos[1] = tgt[1];
		} else {
			lift[0] = pos[0]; lift[1] = pos[1]; lift[2] = pos[2] + 0.17f;
			l2[0] = tgt[0]; l2[1] = tgt[1]; l2[2] = lift[2];
			if (coll_sphere_sweep(cb, pos, lift, r, &h) && h.t < 0.5f) { walk_why = 1; return 0; }
			if (coll_sphere_sweep(cb, lift, l2, r, &h)) { walk_why = 2; return 0; }
			pos[0] = l2[0]; pos[1] = l2[1]; pos[2] = l2[2];
		}
		down[0] = pos[0]; down[1] = pos[1]; down[2] = pos[2] - 0.45f;
		if (!coll_sphere_sweep(cb, pos, down, r, &h)) { walk_why = 3; return 0; }
		pos[2] = pos[2] + (down[2] - pos[2]) * h.t + 0.005f;
		done += step;
	}
	if (fabsf(pos[2] - (c[2] + r + 0.01f)) >= 0.2f) { walk_why = 4; return 0; }
	return 1;
}

typedef struct { uint32_t first, count; } adj_t;
typedef struct { uint16_t to; uint8_t kind; uint8_t width; uint32_t cost; float dz; } edge_t;

static uint32_t heuristic(const path_bsp *b, const float gc[3], uint32_t s)
{
	path_sector_info si;
	float dx, dy, dz;
	path_sector_get(b, s, &si);
	dx = si.centroid[0] - gc[0]; dy = si.centroid[1] - gc[1]; dz = si.centroid[2] - gc[2];
	return (uint32_t)(sqrtf(dx * dx + dy * dy + dz * dz) * ((float)b->cost_scale * 0.9995f));
}

/* brute-force Dijkstra (lazy binary heap) over the link table with the same usability rules as the reader */
typedef struct { uint32_t d; uint32_t v; } hitem;
static void dijkstra(const path_bsp *b, const adj_t *adj, const edge_t *ed, const uint8_t *hr, uint32_t need, uint32_t need_w, const path_query *q, uint32_t from, uint32_t *dist, hitem *heap)
{
	uint32_t n = b->sector_count, i, hn = 0;
	for (i = 0; i < n; i++) dist[i] = 0xFFFFFFFFu;
	dist[from] = 0;
	heap[hn].d = 0; heap[hn].v = from; hn++;
	while (hn) {
		hitem top = heap[0], x;
		uint32_t u, j;
		hn--;
		if (hn) {
			uint32_t c, p = 0;
			x = heap[hn];
			for (;;) {
				c = 2 * p + 1;
				if (c >= hn) break;
				if (c + 1 < hn && heap[c + 1].d < heap[c].d) c++;
				if (heap[c].d >= x.d) break;
				heap[p] = heap[c]; p = c;
			}
			heap[p] = x;
		}
		u = top.v;
		if (top.d != dist[u]) continue;
		for (j = 0; j < adj[u].count; j++) {
			const edge_t *e = &ed[adj[u].first + j];
			uint32_t nd = dist[u] + e->cost;
			if (!(q->kinds & PATH_KIND(e->kind))) continue;
			if (hr[e->to] < need || e->width < need_w) continue;
			if (q->blocked && (q->blocked[e->to >> 5] >> (e->to & 31u) & 1u)) continue;
			if (e->kind == PATH_LINK_DROP && -e->dz > q->max_drop + 1e-4f) continue;
			if (nd < dist[e->to]) {
				uint32_t c = hn++, p;
				dist[e->to] = nd;
				while (c > 0) {
					p = (c - 1) / 2;
					if (heap[p].d <= nd) break;
					heap[c] = heap[p]; c = p;
				}
				heap[c].d = nd; heap[c].v = e->to;
			}
		}
	}
}

typedef struct {
	uint32_t routes, complete, partial, legs, legs_skipped, clear_plain, clear_lifted, fail, route_skipped, walk_ok, walk_fail;
	uint32_t fail_start, fail_end, fail_mid, fail_floor_hit, why[5];
	uint32_t ulegs, ufail, link_overhang;
	uint32_t pairs, mism_cost, mism_complete, mism_end;
	uint32_t locate_n, locate_bad, locate_stacked;
	uint32_t ray_n, ray_miss, ray_overhead, ray_bad;
	uint32_t link_walk, link_step, link_vault, link_drop, link_climb;
	uint32_t sec_n, sec_nonplanar, sec_concave;
	uint32_t reach_n, reach_asym, reach_bad;
	double route_ms_max, route_ms_sum; uint32_t route_n;
} stats_t;

static int dist_point_segment_ok(const float p[3], const float a[3], const float c[3], float tol)
{
	float ex = c[0] - a[0], ey = c[1] - a[1], l2 = ex * ex + ey * ey, t = 0, px, py;
	if (l2 > 1e-18f) { t = ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2; t = t < 0 ? 0 : t > 1 ? 1 : t; }
	px = a[0] + ex * t; py = a[1] + ey * t;
	return (p[0] - px) * (p[0] - px) + (p[1] - py) * (p[1] - py) <= tol * tol && fabsf(p[2] - (a[2] + (c[2] - a[2]) * t)) <= tol + 0.0f;
}
static int point_eq(const float a[3], const float c[3]) { return a[0] == c[0] && a[1] == c[1] && a[2] == c[2]; }

static uint64_t leg_key(const float a[3], const float c[3])
{
	uint64_t h = 1469598103934665603ull;
	int k;
	for (k = 0; k < 6; k++) {
		int32_t v = (int32_t)lroundf((k < 3 ? a[k] : c[k - 3]) * 1000.0f);
		h ^= (uint64_t)(uint32_t)v; h *= 1099511628211ull;
	}
	return h | 1ull;
}

static int test_real_bsp(const path_bsp *b, const coll_bsp *cb, const char *label, uint32_t pairs, uint32_t legs_routes, stats_t *st)
{
	uint32_t n = b->sector_count, i, j, k;
	adj_t *adj = calloc(n + 1, sizeof *adj);
	edge_t *ed = calloc(b->link_count + 1, sizeof *ed);
	uint32_t *dist = malloc((n + 1) * sizeof *dist);
	hitem *heap = malloc((b->link_count + n + 2) * sizeof *heap);
	uint8_t *hr = malloc(n + 1);
	pw w;
	uint32_t e = 0;
	float lo[3], hi[3];
	CHECK(adj && ed && dist && heap && hr);
	if (n == 0) { free(adj); free(ed); free(dist); free(heap); free(hr); return 0; }
	CHECK(pw_init(&w, b->data, b->size));
	/* ---- structure: polygons, links ---- */
	for (i = 0; i < 3; i++) { lo[i] = cb->bounds[i] - 1e-3f; hi[i] = cb->bounds[3 + i] + 1e-3f; }
	for (i = 0; i < n; i++) {
		path_sector_info si;
		float P[PATH_MAX_POLY][3], nr[3] = { 0, 0, 0 }, dmax = 0;
		uint32_t m;
		CHECK(path_sector_get(b, i, &si));
		hr[i] = (uint8_t)(si.headroom * PATH_HEADROOM_SCALE + 0.5f);
		m = path_sector_polygon(b, i, P);
		CHECK(m >= 3 && m <= PATH_MAX_POLY);
		adj[i].first = e; adj[i].count = si.link_count;
		for (j = 0; j < m; j++) {                                                     /* inside the collision bounds, convex, counter-clockwise */
			const float *p = P[j], *p1 = P[(j + 1) % m], *p2 = P[(j + 2) % m];
			float cr = (p1[0] - p[0]) * (p2[1] - p1[1]) - (p1[1] - p[1]) * (p2[0] - p1[0]);
			float sc = fmaxf(hypotf(p1[0] - p[0], p1[1] - p[1]), hypotf(p2[0] - p1[0], p2[1] - p1[1]));
			for (k = 0; k < 3; k++) CHECK(p[k] >= lo[k] && p[k] <= hi[k]);
			if (cr < -1e-3f * sc - 1e-6f) st->sec_concave++;          /* the converter allows 3e-4 of the longer edge; compression and float rounding get 3x that */
			nr[0] += (p[1] - si.centroid[1]) * (p1[2] - si.centroid[2]) - (p[2] - si.centroid[2]) * (p1[1] - si.centroid[1]);
			nr[1] += (p[2] - si.centroid[2]) * (p1[0] - si.centroid[0]) - (p[0] - si.centroid[0]) * (p1[2] - si.centroid[2]);
			nr[2] += (p[0] - si.centroid[0]) * (p1[1] - si.centroid[1]) - (p[1] - si.centroid[1]) * (p1[0] - si.centroid[0]);
		}
		{ float l = sqrtf(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
		  CHECK(l > 0.0f);
		  nr[0] /= l; nr[1] /= l; nr[2] /= l;
		  CHECK(nr[2] >= 0.70f);                                                      /* walkable slope (the converter merges planar, nz >= 0.7) */
		  for (j = 0; j < m; j++) { float dd = fabsf((P[j][0] - si.centroid[0]) * nr[0] + (P[j][1] - si.centroid[1]) * nr[1] + (P[j][2] - si.centroid[2]) * nr[2]); if (dd > dmax) dmax = dd; }
		  if (dmax > 0.0215f) st->sec_nonplanar++; }
		st->sec_n++;
		for (j = 0; j < si.link_count; j++) {
			path_link l;
			CHECK(path_link_get(b, i, j, &l));
			ed[e].to = l.to; ed[e].kind = l.kind; ed[e].cost = l.cost; ed[e].dz = l.dz; ed[e].width = (uint8_t)(l.width * PATH_LINK_WIDTH_SCALE + 0.5f); e++;
		}
	}
	CHECK(e == b->link_count);
	{
		/* link consistency: costs not below the centroid distance; walk links have the swapped reverse and their end points are polygon
		 * vertices of both sectors; step / climb have their reverse, vault has its drop back; every other portal lies on one
		 * boundary edge of the source and the destination is within a riser's run of its middle */
		for (i = 0; i < n; i++) {
			path_sector_info si;
			float P[PATH_MAX_POLY][3];
			uint32_t m;
			path_sector_get(b, i, &si);
			m = path_sector_polygon(b, i, P);
			for (j = 0; j < si.link_count; j++) {
				path_link l, r;
				path_sector_info ti;
				float dd, Q[PATH_MAX_POLY][3];
				uint32_t mq, a, c0;
				int on_r = 0, on_l = 0, found = 0;
				path_link_get(b, i, j, &l);
				path_sector_get(b, l.to, &ti);
				CHECK(l.to != i);
				dd = sqrtf((si.centroid[0] - ti.centroid[0]) * (si.centroid[0] - ti.centroid[0]) + (si.centroid[1] - ti.centroid[1]) * (si.centroid[1] - ti.centroid[1]) +
				           (si.centroid[2] - ti.centroid[2]) * (si.centroid[2] - ti.centroid[2]));
				CHECK((float)l.cost >= dd * (float)b->cost_scale - 1e-3f);               /* the heuristic is admissible and consistent */
				for (c0 = 0; c0 < path_link_count(b, l.to); c0++) {
					path_link_get(b, l.to, c0, &r);
					if (r.to != i) continue;
					if (l.kind == PATH_LINK_WALK && r.kind == PATH_LINK_WALK && point_eq(r.right, l.left) && point_eq(r.left, l.right)) found = 1;
					if ((l.kind == PATH_LINK_STEP || l.kind == PATH_LINK_CLIMB) && r.kind == l.kind) found = 1;
					if (l.kind == PATH_LINK_VAULT && r.kind == PATH_LINK_DROP) found = 1;
				}
				if (l.kind == PATH_LINK_DROP) {                                          /* its reverse is a vault (low ledge) or absent (high ledge) */
					found = 1;
					CHECK(l.dz < -b->step_max + 1e-3f && -l.dz <= b->drop_max + 1e-2f);
				}
				if (l.kind == PATH_LINK_VAULT) CHECK(l.dz > b->step_max - 1e-3f && l.dz <= b->vault_max + 1e-2f);
				if (l.kind == PATH_LINK_STEP) CHECK(fabsf(l.dz) <= b->step_max + 1e-2f);
				CHECK(found);
				if (l.kind == PATH_LINK_WALK) {
					mq = path_sector_polygon(b, l.to, Q);
					for (a = 0; a < m; a++) { on_r += point_eq(P[a], l.right); on_l += point_eq(P[a], l.left); }
					CHECK(on_r == 1 && on_l == 1 && l.dz == 0.0f);
					on_r = on_l = 0;
					for (a = 0; a < mq; a++) { on_r += point_eq(Q[a], l.right); on_l += point_eq(Q[a], l.left); }
					CHECK(on_r == 1 && on_l == 1);
					st->link_walk++;
				} else if (l.kind != PATH_LINK_CLIMB) {
					int on_edge = 0;
					float mid[3], cp[3], off;
					mid[0] = 0.5f * (l.right[0] + l.left[0]); mid[1] = 0.5f * (l.right[1] + l.left[1]); mid[2] = 0.5f * (l.right[2] + l.left[2]);
					for (a = 0; a < m; a++)
						if (dist_point_segment_ok(l.right, P[a], P[(a + 1) % m], 2e-3f) && dist_point_segment_ok(l.left, P[a], P[(a + 1) % m], 2e-3f)) on_edge = 1;
					CHECK(on_edge);
					path_snap(b, l.to, mid, 0.0f, cp);
					off = hypotf(cp[0] - mid[0], cp[1] - mid[1]);
					CHECK(off <= 0.45f + 1.5f * fabsf(l.dz) + 0.05f);
					/* the destination plane at the closest point must agree with the rise, unless the middle lies inside the destination's
					 * footprint (an overhanging riser: the plane is then evaluated over the portal, not at its edge) */
					if (off > 1e-3f) {
						if (!(fabsf((cp[2] - mid[2]) - l.dz) <= 0.08f + 0.9f * off))
							fprintf(stderr, "link %u -> %u kind %u dz %.3f: closest point of the destination (%.3f %.3f %.3f) to the middle (%.3f %.3f %.3f) off %.3f\n", i, l.to, l.kind, (double)l.dz, (double)cp[0], (double)cp[1],
							        (double)cp[2], (double)mid[0], (double)mid[1], (double)mid[2], (double)off);
						CHECK(fabsf((cp[2] - mid[2]) - l.dz) <= 0.08f + 0.9f * off);
					} else {
						st->link_overhang++;
					}
					if (l.kind == PATH_LINK_STEP) st->link_step++; else if (l.kind == PATH_LINK_VAULT) st->link_vault++; else st->link_drop++;
				} else {
					st->link_climb++;
				}
			}
		}
	}
	/* ---- sectors against the collision surfaces ---- */
	for (k = 0; k < 600 && k < n * 3u; k++) {
		uint32_t s = k < n ? k : rnd() % n;
		path_sector_info si;
		float pl[4], poly[COLL_MAX_POLY][3], p[3], from[3], to[3];
		uint16_t m, f, br;
		uint32_t pn, a, bad = 0;
		coll_hit h;
		path_sector_get(b, s, &si);
		CHECK(si.surface != PATH_NO_SURFACE && si.surface < cb->surface_count);
		CHECK(coll_surface_info(cb, si.surface, pl, &m, &f, &br));
		CHECK(pl[2] >= 0.7099f);                                                       /* the source surface is walkable */
		CHECK((m == si.material) || (m == COLL_NO_MATERIAL && si.material == PATH_NO_MATERIAL));
		CHECK(!!(f & COLL_F_BREAKABLE) == !!(si.flags & PATH_SF_BREAKABLE));
		pn = coll_surface_polygon(cb, si.surface, poly);
		CHECK(pn >= 3);
		for (a = 0; a < pn; a++) {                                                     /* every vertex of the source surface lies in the sector's plan */
			float x = poly[a][0], y = poly[a][1], zz = path_sector_height(b, s, x, y);
			float P2[PATH_MAX_POLY][3];
			uint32_t mm = path_sector_polygon(b, s, P2), q2;
			for (q2 = 0; q2 < mm; q2++) {
				const float *p0 = P2[q2], *p1 = P2[(q2 + 1) % mm];
				float cr = (p1[0] - p0[0]) * (y - p0[1]) - (p1[1] - p0[1]) * (x - p0[0]);
				if (cr < -2e-3f * (fabsf(p1[0] - p0[0]) + fabsf(p1[1] - p0[1]))) bad = 1;
			}
			if (fabsf(zz - poly[a][2]) > 0.025f) bad = 1;
		}
		CHECK(!bad);
		/* a ray from 0.15 above a random interior point down: it hits a walkable surface at the sector's height */
		if (si.headroom < 0.2f) continue;                                              /* a ray from 0.15 above must start under the sheet / ceiling above */
		sample_in(b, s, 0.8f, p);
		from[0] = p[0]; from[1] = p[1]; from[2] = p[2] + 0.15f;
		to[0] = p[0]; to[1] = p[1]; to[2] = p[2] - 0.15f;
		st->ray_n++;
		if (!coll_ray(cb, from, to, 0, &h)) st->ray_miss++;
		else if (coll_point_in_solid(cb, from)) st->ray_overhead++;
		else if (fabsf(h.point[2] - p[2]) > 0.03f || h.normal[2] < 0.7f) st->ray_bad++;
	}
	/* ---- locate ---- */
	for (k = 0; k < 800; k++) {
		uint32_t s = rnd() % n;
		float p[3];
		int32_t r;
		sample_in(b, s, 0.9f, p);
		p[2] += 0.05f;
		r = path_locate(b, p);
		st->locate_n++;
		if (r == (int32_t)s) continue;
		if (r >= 0) {                                                                  /* a stacked sector over the same point, higher but within reach */
			float zr = path_sector_height(b, (uint32_t)r, p[0], p[1]);
			if (zr >= p[2] - 0.05f && zr <= p[2] + PATH_LOCATE_BELOW) { st->locate_stacked++; continue; }
		}
		st->locate_bad++;
	}
	/* ---- A* against Dijkstra ---- */
	{
		static const uint32_t masks[5] = { PATH_KINDS_WALKING, PATH_KINDS_WALKING | PATH_KIND(PATH_LINK_DROP), PATH_KINDS_ALL, PATH_KINDS_SYMMETRIC,
		                                   PATH_KIND(PATH_LINK_WALK) };
		uint32_t *blocked = calloc((n + 31) / 32 + 1, 4);
		CHECK(blocked);
		for (k = 0; k < pairs; k++) {
			path_query q = w.q;
			uint32_t from = rnd() % n, to = (k & 7u) == 0 ? from : rnd() % n, g, reach_best_h = 0xFFFFFFFFu, reach_best_g = 0xFFFFFFFFu;
			path_route_info inf;
			float gc[3];
			path_sector_info gi;
			int r;
			q.kinds = masks[k % 5u];
			q.max_drop = (k & 1u) ? 1.5f : 3.0f;
			q.height = (k % 4u == 0u) ? 0.0f : (k % 4u == 1u) ? 0.25f : (k % 4u == 2u) ? 0.5f : 0.9f;
			q.radius = (k % 5u == 0u) ? 0.0f : (k % 5u == 1u) ? 0.1f : (k % 5u == 2u) ? 0.2f : (k % 5u == 3u) ? 0.35f : 0.6f;
			memset(blocked, 0, ((n + 31) / 32) * 4);
			if (k % 3u == 2u) for (g = 0; g < n / 30u + 1u; g++) { uint32_t x = rnd() % n; if (x != from) blocked[x >> 5] |= 1u << (x & 31u); }
			q.blocked = (k % 3u == 2u) ? blocked : NULL;
			dijkstra(b, adj, ed, hr, q.height <= 0.0f ? 0u : q.height * PATH_HEADROOM_SCALE >= 255.0f ? 255u : (uint32_t)ceilf(q.height * PATH_HEADROOM_SCALE),
			         q.radius <= 0.0f ? 0u : 2.0f * q.radius * PATH_LINK_WIDTH_SCALE >= 255.0f ? 255u : (uint32_t)(2.0f * q.radius * PATH_LINK_WIDTH_SCALE), &q, from, dist, heap);
			r = path_find(b, w.sc, &q, from, to, &inf);
			st->pairs++;
			if ((dist[to] != 0xFFFFFFFFu) != (r == 1)) st->mism_complete++;
			else if (r == 1 && inf.cost != dist[to]) st->mism_cost++;
			if (r == 1) {
				uint32_t sum = 0;
				CHECK(inf.sector_count == w.sc->route_links + 1 && w.sc->route_sector[0] == from && w.sc->route_sector[inf.sector_count - 1] == to);
				for (j = 0; j < w.sc->route_links; j++) {                              /* the route's links exist and its costs add up */
					path_link l; path_sector_info si2;
					uint32_t u = w.sc->route_sector[j], v2 = w.sc->route_sector[j + 1], f2 = 0, kind_ok = 0;
					path_sector_get(b, u, &si2);
					CHECK(w.sc->route_link[j] >= si2.first_link && w.sc->route_link[j] < si2.first_link + si2.link_count);
					for (i = 0; i < si2.link_count; i++) { path_link_get(b, u, i, &l); if (si2.first_link + i == w.sc->route_link[j]) { CHECK(l.to == v2); sum += l.cost; f2 = 1; kind_ok = (q.kinds & PATH_KIND(l.kind)) != 0; } }
					CHECK(f2 && kind_ok);
					CHECK(hr[v2] >= (q.height <= 0.0f ? 0u : q.height * PATH_HEADROOM_SCALE >= 255.0f ? 255u : (uint32_t)ceilf(q.height * PATH_HEADROOM_SCALE)));
					if (q.blocked) CHECK(!(blocked[v2 >> 5] >> (v2 & 31u) & 1u));
				}
				CHECK(sum == inf.cost);
			} else {
				/* partial: the end sector minimises (heuristic, cost) over everything reachable */
				path_sector_get(b, to, &gi);
				gc[0] = gi.centroid[0]; gc[1] = gi.centroid[1]; gc[2] = gi.centroid[2];
				for (g = 0; g < n; g++) {
					uint32_t h;
					path_sector_info ti;
					if (dist[g] == 0xFFFFFFFFu) continue;
					path_sector_get(b, g, &ti);
					if (g != from && (uint32_t)(ti.thickness * PATH_THICK_SCALE + 0.5f) < (q.radius <= 0.0f ? 0u : 2.0f * q.radius * PATH_THICK_SCALE >= 255.0f ? 255u : (uint32_t)(2.0f * q.radius * PATH_THICK_SCALE))) continue;
					h = heuristic(b, gc, g);
					if (h < reach_best_h || (h == reach_best_h && dist[g] < reach_best_g)) { reach_best_h = h; reach_best_g = dist[g]; }
				}
				CHECK(inf.end_sector >= 0 && dist[inf.end_sector] != 0xFFFFFFFFu);
				if (heuristic(b, gc, (uint32_t)inf.end_sector) != reach_best_h || dist[inf.end_sector] != reach_best_g) st->mism_end++;
				if (inf.cost != dist[inf.end_sector]) st->mism_cost++;
			}
		}
		free(blocked);
	}
	/* ---- reachability symmetry, components ---- */
	for (k = 0; k < pairs / 2 + 10; k++) {
		path_query q = w.q;
		uint32_t a = rnd() % n, c = rnd() % n;
		path_sector_info x, y;
		int ab, ba;
		q.kinds = PATH_KINDS_SYMMETRIC; q.height = 0.0f;
		ab = path_sector_reachable(b, w.sc, &q, a, c);
		ba = path_sector_reachable(b, w.sc, &q, c, a);
		path_sector_get(b, a, &x); path_sector_get(b, c, &y);
		st->reach_n++;
		if (ab != ba) st->reach_asym++;
		if (ab != (x.comp == y.comp)) st->reach_bad++;
		q.kinds = PATH_KINDS_SYMMETRIC | PATH_KIND(PATH_LINK_DROP);                    /* a search must agree with the component test in the symmetric part */
		if (x.comp == y.comp && !path_sector_reachable(b, w.sc, &q, a, c)) st->reach_bad++;
	}
	/* ---- routes: every leg against the collision BSP ---- */
	uint64_t *seen = calloc(1u << 16, sizeof *seen);
	CHECK(seen);
	for (k = 0; k < legs_routes; k++) {
		uint32_t sa = 0, sb = 0, tries;
		float pa[3], pb[3], ca[3], cb2[3];
		path_waypoint wp[PATH_ROUTE_MAX + 4];
		path_route_info inf;
		path_query q = w.q;
		float prev[3], push[3];
		int cnt, w_i, ok = 0, ulegs_new = 0;
		double t0, t1;
		path_sector_info sia, sib;
		/* endpoints a capsule can stand on: headroom for its height, a sector wide enough for its radius, a start free of the walls
		 * (a quarter of the routes ask for a goal in another component: those end partial) */
		for (tries = 0; tries < 3000 && !ok; tries++) {
			sa = rnd() % n; sb = rnd() % n;
			path_sector_get(b, sa, &sia); path_sector_get(b, sb, &sib);
			if (k % 4u != 3u && (sia.comp != sib.comp || !path_sector_reachable(b, w.sc, &q, sa, sb))) continue;
			if (sia.headroom < q.height || sib.headroom < q.height) continue;                           /* a floor under a ceiling a capsule does not fit under */
			if (!path_snap(b, sa, sia.centroid, 0.2f, pa) || !path_snap(b, sb, sib.centroid, 0.2f, pb)) continue;   /* too narrow for the radius */
			ca[0] = pa[0]; ca[1] = pa[1]; ca[2] = pa[2] + 0.18f; cb2[0] = pb[0]; cb2[1] = pb[1]; cb2[2] = pb[2] + 0.18f;
			if (coll_sphere_overlap(cb, ca, 0.15f, push) || coll_sphere_overlap(cb, cb2, 0.15f, push)) continue;   /* a start that is itself in a wall */
			ok = 1;
		}
		if (!ok) { st->route_skipped++; continue; }
		t0 = now_ms();
		cnt = path_route(b, w.sc, &q, pa, pb, wp, PATH_ROUTE_MAX + 4, &inf);
		t1 = now_ms();
		st->route_ms_sum += t1 - t0; st->route_n++;
		if (t1 - t0 > st->route_ms_max) st->route_ms_max = t1 - t0;
		CHECK(cnt > 0 && inf.status != PATH_ROUTE_NO_START && inf.status != PATH_ROUTE_NO_GOAL);
		CHECK(wp[cnt - 1].flags & PATH_WPF_GOAL);
		st->routes++;
		if (inf.status == PATH_ROUTE_COMPLETE) st->complete++; else st->partial++;
		prev[0] = pa[0]; prev[1] = pa[1]; prev[2] = pa[2];
		for (w_i = 0; w_i < cnt; w_i++) {
			float A2[3], B2[3], A3[3], B3[3];
			coll_hit h;
			path_sector_info wi;
			float floorz;
			CHECK(wp[w_i].sector < n && path_sector_get(b, wp[w_i].sector, &wi));
			floorz = path_sector_height(b, wp[w_i].sector, wp[w_i].pos[0], wp[w_i].pos[1]);
			CHECK(fabsf(floorz - wp[w_i].pos[2]) < 0.03f);                              /* the waypoint is on its sector's floor */
			if (w_i > 0 && wp[w_i - 1].kind != PATH_LINK_WALK) {                       /* an action leg (vault, drop, climb): not walked */
				st->legs_skipped++;
				prev[0] = wp[w_i].pos[0]; prev[1] = wp[w_i].pos[1]; prev[2] = wp[w_i].pos[2];
				continue;
			}
			A2[0] = prev[0]; A2[1] = prev[1]; A2[2] = prev[2] + 0.18f;
			B2[0] = wp[w_i].pos[0]; B2[1] = wp[w_i].pos[1]; B2[2] = wp[w_i].pos[2] + 0.18f;
			A3[0] = A2[0]; A3[1] = A2[1]; A3[2] = A2[2] + 0.17f; B3[0] = B2[0]; B3[1] = B2[1]; B3[2] = B2[2] + 0.17f;
			st->legs++;
			{ uint64_t key = leg_key(prev, wp[w_i].pos), idx = key & 0xFFFFu;
			  int fresh = 1;
			  while (seen[idx]) { if (seen[idx] == key) { fresh = 0; break; } idx = (idx + 1) & 0xFFFFu; }
			  if (fresh) { seen[idx] = key; st->ulegs++; ulegs_new = 1; } else ulegs_new = 0; }
			if (!coll_sphere_sweep(cb, A2, B2, 0.15f, &h)) st->clear_plain++;
			else if (!coll_sphere_sweep(cb, A3, B3, 0.15f, &h)) st->clear_lifted++;
			else st->fail++;
			if (walk_leg(cb, prev, wp[w_i].pos, 0.15f)) st->walk_ok++;
			else {
				st->walk_fail++; st->why[walk_why]++; if (ulegs_new) st->ufail++;
				{ coll_hit hh; float len = hypotf(B2[0] - A2[0], B2[1] - A2[1]);
				  if (coll_sphere_sweep(cb, A2, B2, 0.15f, &hh)) {
					  if (hh.t * len < 0.1f) st->fail_start++; else if ((1.0f - hh.t) * len < 0.1f) st->fail_end++; else st->fail_mid++;
					  if (hh.normal[2] > 0.5f) st->fail_floor_hit++;
				  } }
				if (getenv("PATH_VERBOSE2")) fprintf(stderr, "FAILLEG %s %.3f %.3f %.3f %.3f %.3f %.3f sectors %u %u why %d\n", label, (double)prev[0], (double)prev[1], (double)prev[2], (double)wp[w_i].pos[0], (double)wp[w_i].pos[1],
				                                     (double)wp[w_i].pos[2], w_i ? wp[w_i - 1].sector : sa, wp[w_i].sector, walk_why);
				if (getenv("PATH_VERBOSE") && st->walk_fail <= 12) {
					path_sector_info a_, b_;
					path_sector_get(b, w_i ? wp[w_i - 1].sector : sa, &a_); path_sector_get(b, wp[w_i].sector, &b_);
					fprintf(stderr, "  route from (%.4f %.4f %.4f) to (%.4f %.4f %.4f), waypoint %d of %d (flags %u kind %u); sectors %u -> %u: nverts %u %u, headroom %.2f %.2f\n", (double)pa[0], (double)pa[1], (double)pa[2],
					        (double)pb[0], (double)pb[1], (double)pb[2], w_i, cnt, wp[w_i].flags, wp[w_i].kind, w_i ? wp[w_i - 1].sector : sa, wp[w_i].sector, a_.nverts, b_.nverts, (double)a_.headroom, (double)b_.headroom);
					fprintf(stderr, "  [%s] leg the walker cannot follow: (%.3f %.3f %.3f) -> (%.3f %.3f %.3f) straight sweep hit t=%.3f normal %.2f %.2f %.2f material %u flags %u\n", label, (double)prev[0], (double)prev[1],
					        (double)prev[2], (double)wp[w_i].pos[0], (double)wp[w_i].pos[1], (double)wp[w_i].pos[2], (double)h.t, (double)h.normal[0], (double)h.normal[1], (double)h.normal[2], h.material, h.flags);
				}
			}
			prev[0] = wp[w_i].pos[0]; prev[1] = wp[w_i].pos[1]; prev[2] = wp[w_i].pos[2];
		}
	}
	free(seen);
	pw_free(&w);
	free(adj); free(ed); free(dist); free(heap); free(hr);
	return 0;
}

static int test_pack(const char *pathfile, const char *collfile, int first, stats_t *tot, int bench)
{
	size_t pn, cn, i;
	uint8_t *pimg = read_file(pathfile, &pn), *cimg = read_file(collfile, &cn);
	hp2_pack ppk, cpk;
	uint32_t bi, pk_u = 0, pk_ufail = 0;
	CHECK(pimg && cimg && hp2_open(&ppk, pimg, pn) == HP2_OK && hp2_open(&cpk, cimg, cn) == HP2_OK);
	(void)i;
	for (bi = 0; bi < 36; bi++) {
		char tp[5] = { 'P', 'A', 'T', tag_char(bi), 0 }, tc[5] = { 'C', 'O', 'L', tag_char(bi), 0 };
		size_t psz, csz;
		uint8_t *pd = load_section(&ppk, tp, &psz), *cd = load_section(&cpk, tc, &csz);
		path_bsp b;
		coll_bsp cb;
		char label[48];
		stats_t st;
		if (!pd) { CHECK(!cd); break; }
		CHECK(cd && path_open(pd, psz, &b) == PATH_OK && coll_open(cd, csz, &cb) == COLL_OK);
		snprintf(label, sizeof label, "%s %s", ppk.name, tp);
		memset(&st, 0, sizeof st);
		CHECK(!test_real_bsp(&b, &cb, label, first && bi == 0 ? 3000 : 500, first && bi == 0 ? 1200 : 300, &st));
		printf("[path] %-9s %5u sectors %6u links (walk %u step %u vault %u drop %u climb %u) %4u comps %6.1f KiB | planar>0.0215: %u concave: %u | "
		       "A* vs Dijkstra %u pairs: complete-mismatch %u cost-mismatch %u partial-end-mismatch %u | locate %u: stacked %u bad %u | rays %u: miss %u overhead %u bad %u | "
		       "reach %u asym %u bad %u | routes %u (%u complete, %u partial, %u starts in walls): legs %u: straight sweep clear %u, with step-up %u, blocked %u; walker follows %u, stuck %u (%.1f%% of the traversals; %u of %u distinct legs; first contact of the straight sweep: within 0.1 of the leg start %u, of its end %u, between %u; on a floor-like surface %u; stopped by: blocked above a step-up %u, blocked at the lifted height %u, no ground within 0.45 %u, ended at the wrong height %u); action legs %u\n",
		       label, b.sector_count, b.link_count, st.link_walk, st.link_step, st.link_vault, st.link_drop, st.link_climb, b.comp_count, (double)psz / 1024.0, st.sec_nonplanar, st.sec_concave,
		       st.pairs, st.mism_complete, st.mism_cost, st.mism_end, st.locate_n, st.locate_stacked, st.locate_bad, st.ray_n, st.ray_miss, st.ray_overhead, st.ray_bad,
		       st.reach_n, st.reach_asym, st.reach_bad, st.routes, st.complete, st.partial, st.route_skipped, st.legs, st.clear_plain, st.clear_lifted, st.fail,
		       st.walk_ok, st.walk_fail, st.legs ? 100.0 * (double)st.walk_fail / (double)st.legs : 0.0, st.ufail, st.ulegs, st.fail_start, st.fail_end, st.fail_mid, st.fail_floor_hit, st.why[1], st.why[2], st.why[3], st.why[4], st.legs_skipped);
		CHECK(st.mism_complete == 0 && st.mism_cost == 0 && st.mism_end == 0 && st.reach_asym == 0 && st.reach_bad == 0);
		CHECK(st.sec_nonplanar == 0 && st.sec_concave == 0 && st.locate_bad == 0);
		CHECK(st.ray_bad * 50u <= st.ray_n);
		CHECK(st.ulegs < 50u || st.ufail * 100u <= st.ulegs * 12u);                    /* a regression guard per BSP: at most 12% of the distinct legs */
		pk_u += st.ulegs; pk_ufail += st.ufail;
		tot->pairs += st.pairs; tot->legs += st.legs; tot->clear_plain += st.clear_plain; tot->clear_lifted += st.clear_lifted; tot->fail += st.fail; tot->routes += st.routes;
		tot->legs_skipped += st.legs_skipped; tot->ulegs += st.ulegs; tot->ufail += st.ufail; tot->walk_ok += st.walk_ok; tot->walk_fail += st.walk_fail; tot->sec_n += st.sec_n; tot->locate_n += st.locate_n; tot->locate_stacked += st.locate_stacked; tot->ray_n += st.ray_n;
		tot->ray_miss += st.ray_miss; tot->ray_overhead += st.ray_overhead; tot->ray_bad += st.ray_bad; tot->route_skipped += st.route_skipped; tot->complete += st.complete;
		tot->partial += st.partial; tot->reach_n += st.reach_n;
		if (st.route_ms_max > tot->route_ms_max) tot->route_ms_max = st.route_ms_max;
		tot->route_ms_sum += st.route_ms_sum; tot->route_n += st.route_n;
		if (bench && first && bi == (getenv("PATH_BENCH_BSP") ? (uint32_t)atoi(getenv("PATH_BENCH_BSP")) : 0u)) {
			/* host speed on this BSP */
			pw w;
			uint32_t n = b.sector_count, k, cnt;
			double t0, t1, worst = 0, sum = 0;
			float p[3];
			uint32_t N = 20000, hits = 0;
			CHECK(pw_init(&w, pd, psz));
			t0 = now_ms();
			for (k = 0; k < N; k++) { sample_in(&b, rnd() % n, 0.9f, p); p[2] += 0.05f; hits += path_locate(&b, p) >= 0; }
			t1 = now_ms();
			printf("[path bench] %s locate (point inside a random sector): %.0f per ms (%u/%u found; includes the sampling)\n", label, (double)N / (t1 - t0), hits, N);
			t0 = now_ms();
			for (k = 0; k < 5000; k++) { float d; sample_in(&b, rnd() % n, 0.9f, p); p[0] += 3.0f; hits += path_nearest_sector(&b, p, 6.0f, &d, NULL) >= 0; }
			t1 = now_ms();
			printf("[path bench] %s nearest sector (3 wu off, <= 6 wu): %.0f per ms\n", label, 5000.0 / (t1 - t0));
			{
				uint32_t cs, cstep;
				double longest = 0; uint32_t exp_sum = 0, runs = 0, expmax = 0;
				path_route_info inf;
				path_query q = w.q;
				/* path_find between random sectors of the same component, timed one by one */
				for (k = 0, cs = 0; cs < 3000 && k < 20000; k++) {
					uint32_t a = rnd() % n, c = rnd() % n;
					path_sector_info x, y;
					path_sector_get(&b, a, &x); path_sector_get(&b, c, &y);
					if (x.comp != y.comp) continue;
					cs++;
					t0 = now_ms();
					path_find(&b, w.sc, &q, a, c, &inf);
					t1 = now_ms();
					sum += t1 - t0; if (t1 - t0 > worst) worst = t1 - t0;
					exp_sum += inf.expanded; runs++; if (inf.expanded > expmax) expmax = inf.expanded;
					if ((double)inf.sector_count > longest) longest = (double)inf.sector_count;
				}
				(void)cstep;
				printf("[path bench] %s path_find, %u random same-component pairs: mean %.4f ms (%.0f per ms), worst %.3f ms; expansions mean %.0f max %u, longest route %.0f sectors\n",
				       label, runs, sum / (double)runs, (double)runs / sum, worst, (double)exp_sum / (double)runs, expmax, longest);
				/* unreachable goals flood a component */
				worst = 0; sum = 0; runs = 0;
				for (k = 0; k < 400; k++) {
					uint32_t a = rnd() % n, c = rnd() % n;
					path_sector_info x, y;
					path_sector_get(&b, a, &x); path_sector_get(&b, c, &y);
					if (x.comp == y.comp) continue;
					t0 = now_ms();
					path_find(&b, w.sc, &q, a, c, &inf);
					t1 = now_ms();
					sum += t1 - t0; if (t1 - t0 > worst) worst = t1 - t0; runs++;
				}
				if (runs) printf("[path bench] %s path_find to an unreachable sector (floods the start's component): mean %.4f ms, worst %.3f ms (%u runs)\n", label, sum / (double)runs, worst, runs);
				/* full path_route with the funnel */
				{
					path_waypoint wp[PATH_ROUTE_MAX + 4];
					double rs = 0, rw = 0; uint32_t rn = 0;
					for (k = 0; k < 4000 && rn < 2000; k++) {
						uint32_t a = rnd() % n, c = rnd() % n;
						float pa[3], pb[3];
						path_sector_info x, y;
						path_sector_get(&b, a, &x); path_sector_get(&b, c, &y);
						if (x.comp != y.comp) continue;
						sample_in(&b, a, 0.8f, pa); sample_in(&b, c, 0.8f, pb);
						t0 = now_ms();
						cnt = (uint32_t)path_route(&b, w.sc, &q, pa, pb, wp, PATH_ROUTE_MAX + 4, &inf);
						t1 = now_ms();
						(void)cnt;
						rs += t1 - t0; if (t1 - t0 > rw) rw = t1 - t0; rn++;
					}
					printf("[path bench] %s path_route (locate, A*, funnel), %u random same-component pairs: mean %.4f ms (%.0f per ms), worst %.3f ms\n", label, rn, rs / (double)rn, (double)rn / rs, rw);
				}
			}
			pw_free(&w);
		}
		if (first && bi == 0) test_hostile(pd, psz, 0);
		free(pd); free(cd);
	}
	printf("[path] %s: distinct legs the capsule follows: %u of %u (%.2f%% stuck)\n", ppk.name, pk_u - pk_ufail, pk_u, pk_u ? 100.0 * (double)pk_ufail / (double)pk_u : 0.0);
	CHECK(pk_u < 100u || pk_ufail * 20u <= pk_u);                                       /* at least 95% of the distinct legs of a map clear */
	free(pimg); free(cimg);
	return 0;
}

int main(int argc, char **argv)
{
	size_t n = 0;
	uint8_t *img;
	hp2_pack pk;
	uint8_t *s0, *s1;
	size_t n0, n1;
	int a, bench = getenv("PATH_BENCH") != NULL;
	stats_t tot;
	if (argc < 2) { fprintf(stderr, "usage: test_path <synthetic.hp2> [<path pack> <coll pack>]...\n"); return 2; }
	img = read_file(argv[1], &n);
	CHECK(img && hp2_open(&pk, img, n) == HP2_OK);
	s0 = load_section(&pk, "PAT0", &n0);
	s1 = load_section(&pk, "PAT1", &n1);
	CHECK(s0 && s1);
	if (test_synthetic(s0, n0, s1, n1)) return 1;
	if (test_hostile(s0, n0, 1)) return 1;
	memset(&tot, 0, sizeof tot);
	for (a = 2; a + 1 < argc; a += 2)
		if (test_pack(argv[a], argv[a + 1], a == 2, &tot, bench)) return 1;
	if (argc > 3)
		printf("[path] all packs: %u sectors, %u A*-vs-Dijkstra pairs, %u routes (%u complete, %u partial; %u starts inside walls skipped), legs %u: straight sweep clear %u, with step-up %u, blocked %u; walker follows %u, stuck %u (%.2f%% of the traversals; %u of %u distinct legs = %.2f%%); %u action legs skipped; "
		       "locate %u (stacked sectors %u); rays %u (miss %u, overhead %u, bad %u); route time mean %.4f ms worst %.3f ms\n",
		       tot.sec_n, tot.pairs, tot.routes, tot.complete, tot.partial, tot.route_skipped, tot.legs, tot.clear_plain, tot.clear_lifted, tot.fail,
		       tot.walk_ok, tot.walk_fail, tot.legs ? 100.0 * (double)tot.walk_fail / (double)tot.legs : 0.0, tot.ufail, tot.ulegs, tot.ulegs ? 100.0 * (double)tot.ufail / (double)tot.ulegs : 0.0, tot.legs_skipped, tot.locate_n, tot.locate_stacked, tot.ray_n, tot.ray_miss, tot.ray_overhead, tot.ray_bad,
		       tot.route_n ? tot.route_ms_sum / (double)tot.route_n : 0.0, tot.route_ms_max);
	free(s0); free(s1); free(img);
	printf("test_path: ok\n");
	return 0;
}
