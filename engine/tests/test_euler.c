/* Halo's euler convention (core/mat.h mat4_from_euler_halo, sim/cine.h cine_halo_euler_to_ypr): orthonormal, the yaw-only case is the usual one, and
 * pitch / roll act about the world axes. */
#include "core/mat.h"
#include "sim/cine.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int near(float a, float b, float e) { return fabsf(a - b) <= e; }

int main(void)
{
	static const float z[3] = { 1.0f, 2.0f, 3.0f };
	mat4 m, n;
	unsigned i;
	for (i = 0; i < 200; i++) {
		float y = (float)i * 0.37f - 3.0f, p = (float)((i * 7) % 29) * 0.1f - 1.4f, r = (float)((i * 13) % 31) * 0.2f - 3.0f;
		mat4_from_euler_halo(&m, y, p, r, z);
		assert(near(dot(m.m[0], m.m[0]), 1.0f, 1e-5f) && near(dot(m.m[1], m.m[1]), 1.0f, 1e-5f) && near(dot(m.m[2], m.m[2]), 1.0f, 1e-5f));
		assert(near(dot(m.m[0], m.m[1]), 0.0f, 1e-5f) && near(dot(m.m[0], m.m[2]), 0.0f, 1e-5f) && near(dot(m.m[1], m.m[2]), 0.0f, 1e-5f));
		{ float c[3] = { m.m[0][1] * m.m[1][2] - m.m[0][2] * m.m[1][1], m.m[0][2] * m.m[1][0] - m.m[0][0] * m.m[1][2], m.m[0][0] * m.m[1][1] - m.m[0][1] * m.m[1][0] };
		  assert(near(c[0], m.m[2][0], 1e-5f) && near(c[1], m.m[2][1], 1e-5f) && near(c[2], m.m[2][2], 1e-5f)); }          /* right handed: forward x left = up */
		assert(m.m[3][0] == 1.0f && m.m[3][1] == 2.0f && m.m[3][2] == 3.0f);
	}
	mat4_from_euler_halo(&m, 0.7f, 0.0f, 0.0f, z);              /* yaw only: the engine's convention */
	mat4_from_ypr(&n, 0.7f, 0.0f, 0.0f, z);
	for (i = 0; i < 12; i++) assert(near(m.m[i / 4][i % 4], n.m[i / 4][i % 4], 1e-6f));
	/* the a10 intro's first camera point (yaw 264, pitch 1, roll 21 degrees): it looks 21 degrees DOWN, not 1 */
	{
		float y, p, r, d = 3.14159265f / 180.0f;
		cine_halo_euler_to_ypr(264.0f * d, 1.0f * d, 21.0f * d, &y, &p, &r);
		assert(near(p / d, -21.0f, 0.6f) && near(fmodf(y / d + 360.0f, 360.0f), 263.7f, 0.5f));
		cine_halo_euler_to_ypr(0.3f, 0.0f, 0.0f, &y, &p, &r);
		assert(near(y, 0.3f, 1e-6f) && near(p, 0.0f, 1e-6f) && near(r, 0.0f, 1e-6f));
	}
	puts("ok");
	return 0;
}
