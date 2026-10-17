/* Host tests for render/lodctl. */
#include "render/lodctl.h"
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	lodctl_t c;
	int i;
	lodctl_init(&c, 33.3f, 30);
	CHECK(c.level == 0 && lodctl_distance_scale(&c) == 1.0f && lodctl_lod_bias(&c) == 0 && lodctl_particle_scale(&c) == 1.0f);

	/* comfortable frames never change anything */
	for (i = 0; i < 500; i++) lodctl_frame(&c, 20.0f, 0);
	CHECK(c.level == 0 && c.downs == 0);

	/* a dropped frame steps down at once; scales shrink monotonically; the level is capped */
	lodctl_frame(&c, 20.0f, 1);
	CHECK(c.level == 1 && lodctl_distance_scale(&c) < 1.0f);
	for (i = 0; i < 10; i++) lodctl_frame(&c, 40.0f, 0);
	CHECK(c.level == LODCTL_MAX_LEVEL);
	CHECK(lodctl_distance_scale(&c) == 0.4f && lodctl_lod_bias(&c) == 2 && lodctl_particle_scale(&c) == 0.25f);
	for (i = 0; i < 100; i++) lodctl_frame(&c, 45.0f, 1);
	CHECK(c.level == LODCTL_MAX_LEVEL);

	/* hold: even with plenty of slack the level does not recover for ~2 s (60 frames) after a miss */
	lodctl_init(&c, 33.3f, 30);
	lodctl_frame(&c, 50.0f, 0);
	CHECK(c.level == 1);
	for (i = 0; i < 100; i++) lodctl_frame(&c, 15.0f, 0);
	CHECK(c.level == 1);                                   /* slack needs 90 frames, hold 60: not yet at 100 incl. smoothing */
	for (i = 0; i < 400; i++) lodctl_frame(&c, 15.0f, 0);
	CHECK(c.level == 0 && c.ups == 1);

	/* recovery is one level at a time */
	lodctl_init(&c, 33.3f, 30);
	for (i = 0; i < 4; i++) lodctl_frame(&c, 50.0f, 0);
	CHECK(c.level == 4);
	{
		int prev = c.level, steps = 0;
		for (i = 0; i < 3000; i++) {
			lodctl_frame(&c, 10.0f, 0);
			if (c.level != prev) { CHECK(prev - c.level == 1); prev = c.level; steps++; }
		}
		CHECK(c.level == 0 && steps == 4);
	}

	/* hovering just under budget never recovers (no slack) and never degrades */
	lodctl_init(&c, 33.3f, 30);
	lodctl_frame(&c, 50.0f, 0);
	for (i = 0; i < 1000; i++) lodctl_frame(&c, 31.0f, 0);
	CHECK(c.level == 1);

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
