/* Host tests for render/clip: planes, containment against an independent point-in-triangle test,
 * perspective-correct attribute interpolation, packing. */
#include "render/camera.h"
#include "render/clip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float frange(float a, float b) { return a + (b - a) * frand(); }

/* attributes 0..2 carry the world position so interpolation can be checked after clipping */
static void make_vertex(const float m[4][4], const float p[3], cvert *v)
{
	int k;
	memset(v, 0, sizeof *v);
	clip_project(m, p, v);
	for (k = 0; k < 3; k++) v->a[k] = p[k];
}

static int inside_screen_tri(const float *x, const float *y, float px, float py)
{
	float d0 = (x[1] - x[0]) * (py - y[0]) - (y[1] - y[0]) * (px - x[0]);
	float d1 = (x[2] - x[1]) * (py - y[1]) - (y[2] - y[1]) * (px - x[1]);
	float d2 = (x[0] - x[2]) * (py - y[2]) - (y[0] - y[2]) * (px - x[2]);
	return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

int main(void)
{
	camera_t cam;
	float m[4][4];
	int iter;

	camera_init(&cam);
	cam.pos[0] = 1.0f; cam.pos[1] = 2.0f; cam.pos[2] = 0.5f;
	cam.yaw = 0.4f; cam.pitch = -0.1f;
	camera_clip_matrix(&cam, m);

	/* --- clip_needs --- */
	{
		cvert v[3];
		float fwd[3], r[3], u[3], p[3];
		int i;
		camera_basis(&cam, r, u, fwd);
		for (i = 0; i < 3; i++) p[i] = cam.pos[i] + fwd[i] * 10.0f;
		make_vertex(m, p, &v[0]); v[1] = v[0]; v[2] = v[0];
		CHECK(!clip_needs(v, cam.near_z, 1.0f));
		for (i = 0; i < 3; i++) p[i] = cam.pos[i] - fwd[i] * 3.0f;       /* behind the eye */
		make_vertex(m, p, &v[1]);
		CHECK(clip_needs(v, cam.near_z, 1.0f));
		for (i = 0; i < 3; i++) p[i] = cam.pos[i] + fwd[i] * 1.0f + r[i] * 20.0f;   /* far to the side */
		make_vertex(m, p, &v[1]);
		CHECK(clip_needs(v, cam.near_z, 1.0f));
		/* just inside the band: needs no clip at margin 1, does at a tighter margin */
		for (i = 0; i < 3; i++) p[i] = cam.pos[i] + fwd[i] * 10.0f + r[i] * 10.0f * (CLIP_GUARD * 0.99f) / (1.0f / tanf(cam.hfov * 0.5f));
		make_vertex(m, p, &v[1]);
		CHECK(!clip_needs(v, cam.near_z, 1.0f) && clip_needs(v, cam.near_z, 0.9f));
	}

	srand(7);
	for (iter = 0; iter < 3000; iter++) {
		float p[3][3];
		cvert tri[3], out[CLIP_MAX_VERTS];
		int i, k, n, j;
		for (i = 0; i < 3; i++) {
			/* mostly in front of the camera, wildly large and sometimes behind it */
			float fwd[3], r[3], u[3], d = frange(-2.0f, 30.0f), lr = frange(-80.0f, 80.0f), lu = frange(-80.0f, 80.0f);
			camera_basis(&cam, r, u, fwd);
			for (k = 0; k < 3; k++) p[i][k] = cam.pos[k] + fwd[k] * d + r[k] * lr + u[k] * lu;
			make_vertex(m, p[i], &tri[i]);
		}
		n = clip_polygon(tri, cam.near_z, out);
		CHECK(n == 0 || (n >= 3 && n <= CLIP_MAX_VERTS));
		for (j = 0; j < n; j++) {
			float w = out[j].c[3], eps = 1e-3f * (1.0f + fabsf(w));
			CHECK(w >= cam.near_z - eps);
			CHECK(fabsf(out[j].c[0]) <= CLIP_GUARD * w + eps && fabsf(out[j].c[1]) <= CLIP_GUARD * w + eps);
			/* the attribute (world position) must reproject to the vertex's own clip coordinates */
			{
				cvert re;
				clip_project(m, out[j].a, &re);
				for (k = 0; k < 4; k++) CHECK(fabsf(re.c[k] - out[j].c[k]) <= 2e-3f * (1.0f + fabsf(re.c[k])));
			}
		}
		/* containment: only for triangles entirely in front of the near plane, so the screen-space
		 * triangle is the true image; random points inside the band must agree */
		if (tri[0].c[3] > cam.near_z && tri[1].c[3] > cam.near_z && tri[2].c[3] > cam.near_z) {
			float sx[3], sy[3];
			int s;
			for (i = 0; i < 3; i++) { sx[i] = tri[i].c[0] / tri[i].c[3]; sy[i] = tri[i].c[1] / tri[i].c[3]; }
			for (s = 0; s < 20; s++) {
				float px = frange(-CLIP_GUARD * 0.95f, CLIP_GUARD * 0.95f), py = frange(-CLIP_GUARD * 0.95f, CLIP_GUARD * 0.95f);
				int in_orig = inside_screen_tri(sx, sy, px, py), in_clip = 0;
				if (n >= 3) {
					float cx[CLIP_MAX_VERTS], cy[CLIP_MAX_VERTS];
					for (j = 0; j < n; j++) { cx[j] = out[j].c[0] / out[j].c[3]; cy[j] = out[j].c[1] / out[j].c[3]; }
					for (j = 1; j + 1 < n && !in_clip; j++) {
						float tx[3] = { cx[0], cx[j], cx[j + 1] }, ty[3] = { cy[0], cy[j], cy[j + 1] };
						in_clip = inside_screen_tri(tx, ty, px, py);
					}
				}
				if (in_orig != in_clip) {
					/* allow razor-thin disagreement exactly on an edge */
					float e[3] = { 0, 0, 0 };
					(void)e;
					CHECK(0);
				}
			}
		}
	}

	/* a triangle wholly outside one plane vanishes; one wholly inside is returned unchanged */
	{
		cvert tri[3], out[CLIP_MAX_VERTS];
		float p[3];
		float fwd[3], r[3], u[3];
		int i;
		camera_basis(&cam, r, u, fwd);
		for (i = 0; i < 3; i++) p[i] = cam.pos[i] - fwd[i] * 5.0f;
		make_vertex(m, p, &tri[0]); tri[1] = tri[0]; tri[2] = tri[0];
		CHECK(clip_polygon(tri, cam.near_z, out) == 0);
		for (i = 0; i < 3; i++) p[i] = cam.pos[i] + fwd[i] * 8.0f;
		make_vertex(m, p, &tri[0]);
		p[0] += r[0]; p[1] += r[1]; p[2] += r[2]; make_vertex(m, p, &tri[1]);
		p[0] += u[0]; p[1] += u[1]; p[2] += u[2]; make_vertex(m, p, &tri[2]);
		CHECK(clip_polygon(tri, cam.near_z, out) == 3);
		CHECK(memcmp(&out[0], &tri[0], sizeof(cvert)) == 0);
	}

	/* packing: centre of the screen, near depth, F saturates */
	{
		cvert v;
		uint32_t q[12];
		clip_gs g = { 0.05f, 16777215.0f, 256.0f, 224.0f, 2048.0f, 2048.0f, 0.0f, 0.0f };
		float one;
		memset(&v, 0, sizeof v);
		v.c[0] = 0; v.c[1] = 0; v.c[2] = 0.05f; v.c[3] = 0.05f;
		v.a[0] = 0.5f; v.a[1] = 0.25f; v.a[4] = 128; v.a[5] = 64; v.a[6] = 32; v.a[7] = 128;
		clip_pack_vertex(&v, 0, &g, q);
		memcpy(&one, &q[2], 4);
		CHECK(fabsf(one - 20.0f) < 1e-3f);                         /* Q = 1/w */
		CHECK(q[8] == 2048u * 16u && q[9] == 2048u * 16u);          /* 12.4 fixed */
		CHECK(q[10] == (uint32_t)(16777215.0f * 16.0f) || q[10] == 268435440u);
		CHECK(q[4] == 128 && q[5] == 64 && q[6] == 32 && q[7] == 128);
		g.fog_k = 200000.0f; g.fog_b = 0.0f;
		clip_pack_vertex(&v, 1, &g, q);
		CHECK(q[11] == 4080u);                                      /* clamped */
		{
			float s;
			memcpy(&s, &q[0], 4);
			CHECK(fabsf(s) < 1e-6f);                                /* pass 1 reads attributes 2,3 (zero here) */
		}
	}

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
