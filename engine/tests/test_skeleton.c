/* Host tests for core/mat and sim/skeleton. */
#include "core/mat.h"
#include "render/camera.h"
#include "sim/skeleton.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

static void quat_axis(float q[4], float x, float y, float z, float angle)
{
	float s = sinf(angle * 0.5f);
	q[0] = x * s; q[1] = y * s; q[2] = z * s; q[3] = cosf(angle * 0.5f);
}

int main(void)
{
	const float PI_F = 3.14159265f;
	float q[4], t[3] = { 1, 2, 3 }, p[3] = { 1, 0, 0 }, o[3];
	mat4 a, b, c, id;

	/* identity, translation, rotation about z by 90 degrees: +x -> +y */
	mat4_identity(&id);
	mat4_transform_point(&id, p, o);
	CHECK(o[0] == 1 && o[1] == 0 && o[2] == 0);
	quat_axis(q, 0, 0, 1, PI_F * 0.5f);
	mat4_from_trs(&a, q, t, 1.0f);
	mat4_transform_point(&a, p, o);
	CHECK(NEAR(o[0], 1, 1e-5f) && NEAR(o[1], 3, 1e-5f) && NEAR(o[2], 3, 1e-5f));     /* (0,1,0) + (1,2,3) */
	mat4_transform_dir(&a, p, o);
	CHECK(NEAR(o[0], 0, 1e-5f) && NEAR(o[1], 1, 1e-5f));
	mat4_from_trs(&a, q, t, 2.0f);                                                     /* scale applies before the translation */
	mat4_transform_point(&a, p, o);
	CHECK(NEAR(o[0], 1, 1e-5f) && NEAR(o[1], 4, 1e-5f));
	{ float zero[4] = { 0, 0, 0, 0 }; mat4_from_trs(&a, zero, t, 1.0f); CHECK(a.m[3][0] == 1); }   /* degenerate quaternion: no NaN */

	/* composition order: a then b */
	{
		float t1[3] = { 1, 0, 0 }, t2[3] = { 0, 5, 0 };
		quat_axis(q, 0, 0, 1, PI_F * 0.5f);
		mat4_from_trs(&a, q, t1, 1.0f);            /* rotate 90 deg, move +x by 1 */
		mat4_from_trs(&b, (float[4]){ 0, 0, 0, 1 }, t2, 1.0f);   /* move +y by 5 */
		mat4_mul(&c, &a, &b);
		mat4_transform_point(&c, p, o);              /* (1,0,0) -> (0,1,0)+(1,0,0) = (1,1,0) -> +5y = (1,6,0) */
		CHECK(NEAR(o[0], 1, 1e-5f) && NEAR(o[1], 6, 1e-5f));
		mat4_mul(&a, &a, &b);                        /* aliasing */
		CHECK(memcmp(&a, &c, sizeof a) == 0);
	}

	/* placement: yaw 90 turns +x forward into +y; pitch 90 points forward at +z; roll rotates up about forward */
	{
		float zero[3] = { 0, 0, 0 }, f[3] = { 1, 0, 0 }, up[3] = { 0, 0, 1 };
		mat4_from_ypr(&a, PI_F * 0.5f, 0, 0, zero);
		mat4_transform_dir(&a, f, o);
		CHECK(NEAR(o[0], 0, 1e-5f) && NEAR(o[1], 1, 1e-5f));
		mat4_from_ypr(&a, 0, PI_F * 0.5f, 0, zero);
		mat4_transform_dir(&a, f, o);
		CHECK(NEAR(o[2], 1, 1e-5f));
		mat4_from_ypr(&a, 0, 0, PI_F * 0.5f, zero);
		mat4_transform_dir(&a, up, o);
		CHECK(NEAR(o[1], -1, 1e-5f) || NEAR(o[1], 1, 1e-5f));         /* up swings sideways about the forward axis */
		CHECK(NEAR(o[0], 0, 1e-5f));
		/* rows of a placement matrix are orthonormal */
		mat4_from_ypr(&a, 0.7f, -0.3f, 0.4f, t);
		CHECK(NEAR(a.m[0][0] * a.m[1][0] + a.m[0][1] * a.m[1][1] + a.m[0][2] * a.m[1][2], 0, 1e-5f));
		CHECK(NEAR(a.m[1][0] * a.m[1][0] + a.m[1][1] * a.m[1][1] + a.m[1][2] * a.m[1][2], 1, 1e-5f));
	}

	/* rigid inverse: m * inverse = identity, with scale and translation */
	{
		mat4 inv, prod;
		int i, j, ok = 1;
		quat_axis(q, 0.2f, -0.7f, 0.4f, 0.8f);
		mat4_from_trs(&a, q, t, 1.5f);
		mat4_rigid_inverse(&inv, &a);
		mat4_mul(&prod, &a, &inv);
		for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) if (!NEAR(prod.m[i][j], i == j ? 1.0f : 0.0f, 1e-4f)) ok = 0;
		CHECK(ok);
	}

	/* world->local light direction round trip */
	{
		float w[3] = { 0.3f, -0.5f, 0.8f }, l[3], back[3];
		quat_axis(q, 0.3f, 0.5f, 0.8f, 1.1f);
		mat4_from_trs(&a, q, t, 1.7f);
		mat4_world_dir_to_local(&a, w, l);
		mat4_transform_dir(&a, l, back);
		CHECK(NEAR(back[0], w[0], 1e-4f) && NEAR(back[1], w[1], 1e-4f) && NEAR(back[2], w[2], 1e-4f));
	}

	/* skeleton: three-node chain with parents out of order */
	{
		skel_xform local[3];
		int16_t parent[3] = { 2, -1, 1 };          /* node 1 root, node 2 child of 1, node 0 child of 2 */
		mat4 world[3], root;
		float one[4] = { 0, 0, 0, 1 }, tr[3] = { 0, 0, 0 };
		int i;
		for (i = 0; i < 3; i++) { memcpy(local[i].rot, one, sizeof one); local[i].scale = 1.0f; }
		local[1].trans[0] = 10; local[1].trans[1] = 0; local[1].trans[2] = 0;
		local[2].trans[0] = 0;  local[2].trans[1] = 2;  local[2].trans[2] = 0;
		local[0].trans[0] = 0;  local[0].trans[1] = 0;  local[0].trans[2] = 3;
		mat4_from_ypr(&root, PI_F * 0.5f, 0, 0, tr);      /* object yaw 90: local +x -> world +y */
		CHECK(skel_world(world, local, parent, 3, &root) == 0);
		/* node 0 origin = (0,0,3) + (0,2,0) + (10,0,0) in root space, then yawed: (x,y,z)->(-y,x,z) */
		{
			float origin[3] = { 0, 0, 0 };
			mat4_transform_point(&world[0], origin, o);
			CHECK(NEAR(o[0], -2, 1e-4f) && NEAR(o[1], 10, 1e-4f) && NEAR(o[2], 3, 1e-4f));
		}
		parent[1] = 0;                                  /* cycle 0 -> 2 -> 1 -> 0 */
		CHECK(skel_world(world, local, parent, 3, &root) == -1);
		parent[1] = 7;                                  /* out of range */
		CHECK(skel_world(world, local, parent, 3, &root) == -1);
		CHECK(skel_world(world, local, parent, SKEL_MAX_NODES + 1, &root) == -1);
	}

	/* palette entry: projecting a node-local point through the palette equals world then clip */
	{
		camera_t cam;
		float clip[4][4], pal[5][4], lw[3] = { 0, 0, 1 }, pt[3] = { 0.5f, -0.25f, 1.0f }, pw[3], expect[4], got[4];
		mat4 w;
		int k;
		camera_init(&cam);
		cam.pos[0] = -5.0f;
		camera_clip_matrix(&cam, clip);
		quat_axis(q, 0, 0, 1, 0.9f);
		mat4_from_trs(&w, q, t, 1.0f);
		skel_palette_entry(pal, &w, clip, lw);
		mat4_transform_point(&w, pt, pw);
		for (k = 0; k < 4; k++) {
			expect[k] = clip[0][k] * pw[0] + clip[1][k] * pw[1] + clip[2][k] * pw[2] + clip[3][k];
			got[k] = pt[0] * pal[0][k] + pt[1] * pal[1][k] + pt[2] * pal[2][k] + pal[3][k];
			CHECK(NEAR(got[k], expect[k], 1e-3f * (1.0f + fabsf(expect[k]))));
		}
		/* local light dir is unit for a unit-scale node and rotates opposite to the node */
		CHECK(NEAR(pal[4][0] * pal[4][0] + pal[4][1] * pal[4][1] + pal[4][2] * pal[4][2], 1.0f, 1e-4f) && pal[4][3] == 0.0f);
	}

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
