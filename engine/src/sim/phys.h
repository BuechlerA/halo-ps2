/* Collision queries as the simulation sees them: a table of callbacks so the player controller, projectiles and tests
 * do not depend on the BSP reader (the engine plugs in world/coll.c, host tests plug in analytic worlds). Pure C11.
 * Conventions: world units, +z up; a hit's `t` is the fraction (0..1) of the segment travelled before contact and
 * `normal` faces the moving object. */
#ifndef SIM_PHYS_H
#define SIM_PHYS_H
#include <stdint.h>

typedef struct {
	float    t;
	float    normal[3];
	float    point[3];
	uint16_t material;
	uint16_t flags;
} phys_hit;

typedef struct {
	void *user;
	/* Segment a->b against the world; returns 1 and fills `h` when it hits. */
	int (*ray)(void *user, const float a[3], const float b[3], phys_hit *h);
	/* Sphere of `radius` whose centre moves a->b; returns 1 and fills `h` for the first contact (t == 0: already touching). */
	int (*sweep)(void *user, const float a[3], const float b[3], float radius, phys_hit *h);
	/* Optional: 1 if the point is inside solid space. */
	int (*solid)(void *user, const float p[3]);
} phys_world;

/* Capsule standing on `pos` (feet centre): spheres of `radius` stacked from pos.z + radius to pos.z + height - radius. Sweeps all of them
 * along `delta` and returns the earliest hit (1) or 0. `lift` raises the whole capsule first (step-up probing). */
int phys_sweep_capsule(const phys_world *w, const float pos[3], const float delta[3], float radius, float height, float lift, phys_hit *h);
/* The same with at most `max_spheres` spheres along the capsule (0 = as many as its height needs): actors (units.c) use 2, which halves their
 * collision cost; two spheres leave no gap while the capsule is not taller than about four radii. */
int phys_sweep_capsule_n(const phys_world *w, const float pos[3], const float delta[3], float radius, float height, float lift, unsigned max_spheres, phys_hit *h);

#endif
