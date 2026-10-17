/* Host tests for core/mem and render/camera. */
#include "core/mem.h"
#include "render/camera.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static void project(const float m[4][4], const float p[3], float out[4])
{
	int k;
	for (k = 0; k < 4; k++) out[k] = m[0][k] * p[0] + m[1][k] * p[1] + m[2][k] * p[2] + m[3][k];
}

static void test_arena(void)
{
	static uint8_t pool[256];
	arena_t a, sub;
	void *p, *q;
	arena_init(&a, "t", pool + 1, 200);          /* deliberately misaligned base */
	p = arena_alloc(&a, 10, 16);
	CHECK(p && ((uintptr_t)p & 15) == 0);
	q = arena_alloc(&a, 1, 1);
	CHECK(q == (uint8_t *)p + 10);
	CHECK(arena_alloc(&a, 1000, 4) == NULL);
	CHECK(arena_alloc(&a, 4, 3) == NULL);        /* alignment must be a power of two */
	CHECK(arena_sub(&sub, &a, "sub", 64) == 0);
	CHECK(((uintptr_t)sub.base & 15) == 0 && sub.cap == 64);
	CHECK(arena_alloc(&sub, 65, 1) == NULL);
	CHECK(arena_alloc(&sub, 64, 1) != NULL && arena_free(&sub) == 0);
	CHECK(a.high >= 74);
	{
		size_t m = arena_mark(&a);
		void *keep, *tmp;
		arena_reset(&a);
		keep = arena_alloc(&a, 32, 16);
		m = arena_mark(&a);
		tmp = arena_alloc(&a, 64, 16);
		CHECK(keep && tmp && a.used > m);
		arena_release(&a, m);
		CHECK(a.used == m);
		CHECK(arena_alloc(&a, 64, 16) == tmp);          /* the scratch space is reusable */
		arena_release(&a, a.used + 100);                /* a mark beyond used never grows the arena */
	}
	arena_reset(&a);
	CHECK(a.used == 0 && a.high >= 74);
	CHECK(arena_alloc(&a, 200, 1) != NULL);      /* exact fit */
	CHECK(arena_alloc(&a, 1, 1) == NULL);
}

static void test_projection(void)
{
	camera_t c;
	float m[4][4], out[4], p[3];
	float kx;
	camera_init(&c);
	c.pos[0] = 10; c.pos[1] = -4; c.pos[2] = 2;
	c.yaw = 0.7f; c.pitch = -0.2f;
	camera_clip_matrix(&c, m);

	/* A point straight ahead lands in the screen centre; depth is near/z (reverse-Z). */
	{
		float r[3], u[3], f[3];
		int i;
		camera_basis(&c, r, u, f);
		for (i = 0; i < 3; i++) p[i] = c.pos[i] + f[i] * 25.0f;
		project(m, p, out);
		CHECK(NEAR(out[3], 25.0f, 1e-3f));
		CHECK(NEAR(out[0] / out[3], 0.0f, 1e-4f));
		CHECK(NEAR(out[1] / out[3], 0.0f, 1e-4f));
		CHECK(NEAR(out[2] / out[3], c.near_z / 25.0f, 1e-6f));
		/* The edge of the horizontal field of view maps to ndc x = +-1; +y is Halo-left, so
		 * a point to the right of the view axis has positive ndc x. */
		for (i = 0; i < 3; i++) p[i] = c.pos[i] + f[i] * 10.0f + r[i] * 10.0f * tanf(c.hfov * 0.5f);
		project(m, p, out);
		CHECK(NEAR(out[0] / out[3], 1.0f, 1e-4f));
		for (i = 0; i < 3; i++) p[i] = c.pos[i] + f[i] * 10.0f + u[i] * 10.0f * tanf(c.hfov * 0.5f) / c.aspect;
		project(m, p, out);
		CHECK(NEAR(out[1] / out[3], -1.0f, 1e-4f));   /* screen y points down: up in the world is -1 */
	}

	/* Straight down +x with yaw 0: +y is left, so a point at y>0 has negative ndc x. */
	camera_init(&c);
	camera_clip_matrix(&c, m);
	p[0] = 10; p[1] = 3; p[2] = 0;
	project(m, p, out);
	CHECK(out[0] < 0 && out[3] == 10.0f);
	kx = 1.0f / tanf(c.hfov * 0.5f);
	CHECK(NEAR(out[0], -kx * 3.0f, 1e-4f));
	/* Behind the camera: negative w. */
	p[0] = -5; p[1] = 0; p[2] = 0;
	project(m, p, out);
	CHECK(out[3] < 0);
}

static void test_frustum(void)
{
	camera_t c;
	frustum_t f;
	float centre[3], bmin[3], bmax[3];
	camera_init(&c);
	camera_frustum(&c, &f);
	CHECK(f.count == 6);
	centre[0] = 50; centre[1] = 0; centre[2] = 0;
	CHECK(frustum_sphere(&f, centre, 1.0f));
	centre[0] = -50;
	CHECK(!frustum_sphere(&f, centre, 1.0f));              /* behind */
	centre[0] = 50; centre[1] = 200;
	CHECK(!frustum_sphere(&f, centre, 1.0f));              /* far to the left */
	CHECK(frustum_sphere(&f, centre, 200.0f));             /* a big enough sphere still touches */
	centre[0] = 3000; centre[1] = 0;
	CHECK(!frustum_sphere(&f, centre, 1.0f));              /* beyond the far plane */
	bmin[0] = 10; bmin[1] = -1; bmin[2] = -1; bmax[0] = 12; bmax[1] = 1; bmax[2] = 1;
	CHECK(frustum_aabb(&f, bmin, bmax));
	bmin[0] = -12; bmax[0] = -10;
	CHECK(!frustum_aabb(&f, bmin, bmax));
	/* A box straddling the camera is visible. */
	bmin[0] = -5; bmax[0] = 5;
	CHECK(frustum_aabb(&f, bmin, bmax));
	/* Consistency with the projection: sphere test agrees with clip-space containment for points. */
	{
		float m[4][4], out[4], p[3];
		int i, agree = 0, total = 0;
		c.pos[0] = 3; c.pos[1] = 2; c.pos[2] = 1; c.yaw = 1.1f; c.pitch = 0.3f;
		camera_frustum(&c, &f);
		camera_clip_matrix(&c, m);
		srand(1);
		for (i = 0; i < 2000; i++) {
			int k;
			for (k = 0; k < 3; k++) p[k] = (float)(rand() % 4000 - 2000) * 0.05f;
			project(m, p, out);
			{
				int in_clip = out[3] > c.near_z && out[3] < c.far_z &&
				              fabsf(out[0]) <= out[3] && fabsf(out[1]) <= out[3];
				int in_f = frustum_sphere(&f, p, 0.0f);
				if (fabsf(fabsf(out[0]) - out[3]) < 1e-2f || fabsf(fabsf(out[1]) - out[3]) < 1e-2f) continue;
				total++;
				if (in_clip == in_f) agree++;
			}
		}
		CHECK(total > 500 && agree == total);
	}
}

static void test_camera_motion(void)
{
	camera_t c;
	camera_init(&c);
	camera_look(&c, 0.0f, 10.0f);
	CHECK(c.pitch < 1.57f && c.pitch > 1.5f);               /* clamped below straight up */
	camera_look(&c, 7.0f, -20.0f);
	CHECK(c.pitch > -1.57f && fabsf(c.yaw) <= 3.1416f);
	camera_init(&c);
	camera_move(&c, 2.0f, 1.0f, 0.5f);
	CHECK(NEAR(c.pos[0], 2.0f, 1e-5f) && NEAR(c.pos[1], -1.0f, 1e-5f) && NEAR(c.pos[2], 0.5f, 1e-5f));
}

int main(void)
{
	test_arena();
	test_projection();
	test_frustum();
	test_camera_motion();
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
