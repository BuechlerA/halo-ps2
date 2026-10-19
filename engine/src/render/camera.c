#include "render/camera.h"
#include <math.h>

#define PI_F 3.14159265f
#define PITCH_LIMIT (89.0f * PI_F / 180.0f)

void camera_init(camera_t *c)
{
	c->pos[0] = c->pos[1] = c->pos[2] = 0.0f;
	c->yaw = c->pitch = c->roll = 0.0f;
	c->hfov = 70.0f * PI_F / 180.0f;
	c->aspect = 4.0f / 3.0f;
	c->near_z = CAMERA_NEAR_DEFAULT;
	c->far_z = 2000.0f;
}

void camera_look(camera_t *c, float dyaw, float dpitch)
{
	c->yaw += dyaw;
	if (c->yaw > PI_F) c->yaw -= 2.0f * PI_F;
	if (c->yaw < -PI_F) c->yaw += 2.0f * PI_F;
	c->pitch += dpitch;
	if (c->pitch > PITCH_LIMIT) c->pitch = PITCH_LIMIT;
	if (c->pitch < -PITCH_LIMIT) c->pitch = -PITCH_LIMIT;
}

void camera_basis(const camera_t *c, float right[3], float up[3], float fwd[3])
{
	float cy = cosf(c->yaw), sy = sinf(c->yaw), cp = cosf(c->pitch), sp = sinf(c->pitch);
	fwd[0] = cp * cy;   fwd[1] = cp * sy;   fwd[2] = sp;
	right[0] = sy;      right[1] = -cy;     right[2] = 0.0f;     /* +y is left in Halo */
	up[0] = -sp * cy;   up[1] = -sp * sy;   up[2] = cp;
	if (c->roll != 0.0f) {                                   /* bank about the view axis: Rz(yaw) Ry(-pitch) Rx(roll), x forward */
		float cr = cosf(c->roll), sr = sinf(c->roll), r0[3] = { right[0], right[1], right[2] };
		int i;
		for (i = 0; i < 3; i++) { right[i] = cr * r0[i] - sr * up[i]; up[i] = sr * r0[i] + cr * up[i]; }
	}
}

void camera_move(camera_t *c, float forward, float strafe, float rise)
{
	float r[3], u[3], f[3];
	int i;
	camera_basis(c, r, u, f);
	for (i = 0; i < 3; i++) c->pos[i] += f[i] * forward + r[i] * strafe;
	c->pos[2] += rise;
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void camera_clip_matrix(const camera_t *c, float m[4][4])
{
	float r[3], u[3], f[3];
	float kx = 1.0f / tanf(c->hfov * 0.5f);
	float ky = kx * c->aspect;      /* tan(vfov/2) = tan(hfov/2) / aspect */
	float rc, uc, fc;
	int i;
	camera_basis(c, r, u, f);
	rc = dot3(r, c->pos);
	uc = dot3(u, c->pos);
	fc = dot3(f, c->pos);
	for (i = 0; i < 3; i++) {
		m[i][0] = kx * r[i];
		m[i][1] = -ky * u[i];
		m[i][2] = 0.0f;
		m[i][3] = f[i];
	}
	m[3][0] = -kx * rc;
	m[3][1] = ky * uc;
	m[3][2] = c->near_z;
	m[3][3] = -fc;
}

static void set_plane(float p[4], const float n[3], const float through[3])
{
	float len = sqrtf(dot3(n, n));
	p[0] = n[0] / len; p[1] = n[1] / len; p[2] = n[2] / len;
	p[3] = -(p[0] * through[0] + p[1] * through[1] + p[2] * through[2]);
}

void camera_frustum(const camera_t *c, frustum_t *fr)
{
	float r[3], u[3], f[3], n[3], far_pt[3];
	float tx = tanf(c->hfov * 0.5f), ty = tx / c->aspect;
	int i;
	camera_basis(c, r, u, f);
	fr->count = 6;
	/* Side planes pass through the eye; inward normals. */
	for (i = 0; i < 3; i++) n[i] = r[i] + f[i] * tx;         /* left:   x_v >= -tx z_v */
	set_plane(fr->plane[0], n, c->pos);
	for (i = 0; i < 3; i++) n[i] = -r[i] + f[i] * tx;        /* right */
	set_plane(fr->plane[1], n, c->pos);
	for (i = 0; i < 3; i++) n[i] = u[i] + f[i] * ty;         /* bottom */
	set_plane(fr->plane[2], n, c->pos);
	for (i = 0; i < 3; i++) n[i] = -u[i] + f[i] * ty;        /* top */
	set_plane(fr->plane[3], n, c->pos);
	for (i = 0; i < 3; i++) far_pt[i] = c->pos[i] + f[i] * c->near_z;
	set_plane(fr->plane[4], f, far_pt);                       /* near */
	for (i = 0; i < 3; i++) { n[i] = -f[i]; far_pt[i] = c->pos[i] + f[i] * c->far_z; }
	set_plane(fr->plane[5], n, far_pt);                       /* far */
}

int frustum_sphere(const frustum_t *f, const float centre[3], float radius)
{
	int i;
	for (i = 0; i < f->count; i++) {
		const float *p = f->plane[i];
		if (p[0] * centre[0] + p[1] * centre[1] + p[2] * centre[2] + p[3] < -radius) return 0;
	}
	return 1;
}

int frustum_aabb(const frustum_t *f, const float bmin[3], const float bmax[3])
{
	int i;
	for (i = 0; i < f->count; i++) {
		const float *p = f->plane[i];
		/* Positive vertex: the box corner furthest along the plane normal. */
		float x = p[0] >= 0 ? bmax[0] : bmin[0];
		float y = p[1] >= 0 ? bmax[1] : bmin[1];
		float z = p[2] >= 0 ? bmax[2] : bmin[2];
		if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return 0;
	}
	return 1;
}
