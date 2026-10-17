#include "core/mat.h"
#include <math.h>
#include <string.h>

void mat4_identity(mat4 *o)
{
	memset(o, 0, sizeof *o);
	o->m[0][0] = o->m[1][1] = o->m[2][2] = o->m[3][3] = 1.0f;
}

void mat4_from_trs(mat4 *o, const float q[4], const float t[3], float s)
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float n = x * x + y * y + z * z + w * w;
	float k = n > 0.0f ? 2.0f / n : 0.0f;
	float xx = x * x * k, yy = y * y * k, zz = z * z * k, xy = x * y * k, xz = x * z * k, yz = y * z * k;
	float wx = w * x * k, wy = w * y * k, wz = w * z * k;
	/* rows are the images of the basis vectors (column-vector rotation matrix transposed) */
	o->m[0][0] = s * (1.0f - yy - zz); o->m[0][1] = s * (xy + wz);        o->m[0][2] = s * (xz - wy);        o->m[0][3] = 0.0f;
	o->m[1][0] = s * (xy - wz);        o->m[1][1] = s * (1.0f - xx - zz); o->m[1][2] = s * (yz + wx);        o->m[1][3] = 0.0f;
	o->m[2][0] = s * (xz + wy);        o->m[2][1] = s * (yz - wx);        o->m[2][2] = s * (1.0f - xx - yy); o->m[2][3] = 0.0f;
	o->m[3][0] = t[0]; o->m[3][1] = t[1]; o->m[3][2] = t[2]; o->m[3][3] = 1.0f;
}

void mat4_from_ypr(mat4 *o, float yaw, float pitch, float roll, const float t[3])
{
	float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
	/* forward = (cp cy, cp sy, sp): the image of +x; left = +y image; up = +z image, rolled about forward */
	float fx = cp * cy, fy = cp * sy, fz = sp;
	float lx0 = -sy, ly0 = cy, lz0 = 0.0f;                       /* left before roll */
	float ux0 = -sp * cy, uy0 = -sp * sy, uz0 = cp;               /* up before roll */
	o->m[0][0] = fx; o->m[0][1] = fy; o->m[0][2] = fz; o->m[0][3] = 0.0f;
	o->m[1][0] = cr * lx0 + sr * ux0; o->m[1][1] = cr * ly0 + sr * uy0; o->m[1][2] = cr * lz0 + sr * uz0; o->m[1][3] = 0.0f;
	o->m[2][0] = cr * ux0 - sr * lx0; o->m[2][1] = cr * uy0 - sr * ly0; o->m[2][2] = cr * uz0 - sr * lz0; o->m[2][3] = 0.0f;
	o->m[3][0] = t[0]; o->m[3][1] = t[1]; o->m[3][2] = t[2]; o->m[3][3] = 1.0f;
}

void mat4_mul(mat4 *o, const mat4 *a, const mat4 *b)
{
	mat4 r;
	int i, k;
	for (i = 0; i < 4; i++)
		for (k = 0; k < 4; k++)
			r.m[i][k] = a->m[i][0] * b->m[0][k] + a->m[i][1] * b->m[1][k] + a->m[i][2] * b->m[2][k] + a->m[i][3] * b->m[3][k];
	*o = r;
}

void mat4_transform_point(const mat4 *m, const float p[3], float out[3])
{
	int k;
	for (k = 0; k < 3; k++) out[k] = p[0] * m->m[0][k] + p[1] * m->m[1][k] + p[2] * m->m[2][k] + m->m[3][k];
}

void mat4_transform_dir(const mat4 *m, const float d[3], float out[3])
{
	int k;
	for (k = 0; k < 3; k++) out[k] = d[0] * m->m[0][k] + d[1] * m->m[1][k] + d[2] * m->m[2][k];
}

void mat4_world_dir_to_local(const mat4 *m, const float w[3], float out[3])
{
	int j;
	/* local = w * M^-1; for M = s * R (rows orthogonal, length s) M^-1 rows = columns / s^2: out[j] = (w . row_j) / |row_j|^2 */
	for (j = 0; j < 3; j++) {
		float d = w[0] * m->m[j][0] + w[1] * m->m[j][1] + w[2] * m->m[j][2];
		float l2 = m->m[j][0] * m->m[j][0] + m->m[j][1] * m->m[j][1] + m->m[j][2] * m->m[j][2];
		out[j] = l2 > 1e-12f ? d / l2 : 0.0f;
	}
}

void mat4_rigid_inverse(mat4 *o, const mat4 *m)
{
	/* rows of the 3x3 are s*R's rows: inverse rows are the columns divided by s^2 */
	float s2 = m->m[0][0] * m->m[0][0] + m->m[0][1] * m->m[0][1] + m->m[0][2] * m->m[0][2];
	float k = s2 > 1e-12f ? 1.0f / s2 : 0.0f;
	mat4 r;
	int i, j;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) r.m[i][j] = m->m[j][i] * k;
	for (j = 0; j < 3; j++) r.m[i = 3][j] = -(m->m[3][0] * r.m[0][j] + m->m[3][1] * r.m[1][j] + m->m[3][2] * r.m[2][j]);
	r.m[0][3] = r.m[1][3] = r.m[2][3] = 0.0f;
	r.m[3][3] = 1.0f;
	*o = r;
}
