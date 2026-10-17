#include "sim/phys.h"

int phys_sweep_capsule_n(const phys_world *w, const float pos[3], const float delta[3], float radius, float height, float lift, unsigned max_spheres, phys_hit *h)
{
	float zlo = pos[2] + lift + radius, zhi = pos[2] + lift + height - radius;
	int n, i, found = 0;
	if (zhi < zlo) zhi = zlo;
	n = (int)((zhi - zlo) / radius) + 1;          /* spheres overlap by at least half a radius */
	if (n > 5) n = 5;
	if (max_spheres && (unsigned)n + 1u > max_spheres) n = (int)max_spheres - 1;
	for (i = 0; i <= n; i++) {
		float a[3], b[3], z = n ? zlo + (zhi - zlo) * (float)i / (float)n : zlo;
		phys_hit t;
		a[0] = pos[0]; a[1] = pos[1]; a[2] = z;
		b[0] = a[0] + delta[0]; b[1] = a[1] + delta[1]; b[2] = a[2] + delta[2];
		if (w->sweep(w->user, a, b, radius, &t) && (!found || t.t < h->t)) { *h = t; found = 1; }
		if (n == 0) break;
	}
	return found;
}

int phys_sweep_capsule(const phys_world *w, const float pos[3], const float delta[3], float radius, float height, float lift, phys_hit *h)
{
	return phys_sweep_capsule_n(w, pos, delta, radius, height, lift, 0, h);
}
