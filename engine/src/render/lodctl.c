#include "render/lodctl.h"

#define OVER 1.0f        /* a frame above this fraction of the budget counts as over budget */
#define COMFORT 0.75f    /* smoothed time below this fraction counts as slack */
#define SLACK_FRAMES 90  /* slack needed before stepping up */

void lodctl_init(lodctl_t *c, float budget_ms, int fps)
{
	c->budget_ms = budget_ms;
	c->smooth = 0.0f;
	c->level = c->hold = c->slack = 0;
	c->hold_frames = 2 * (fps > 0 ? fps : 30);
	c->downs = c->ups = 0;
}

void lodctl_frame(lodctl_t *c, float frame_ms, int dropped)
{
	c->smooth = c->smooth == 0.0f ? frame_ms : c->smooth * 0.9f + frame_ms * 0.1f;
	if (c->hold > 0) c->hold--;
	if (dropped || frame_ms > OVER * c->budget_ms) {
		c->slack = 0;
		if (c->level < LODCTL_MAX_LEVEL) { c->level++; c->downs++; }
		c->hold = c->hold_frames;               /* at the cap too: keep waiting before recovering */
		return;
	}
	if (c->smooth < COMFORT * c->budget_ms) c->slack++; else c->slack = 0;
	if (c->level > 0 && c->hold == 0 && c->slack >= SLACK_FRAMES) {
		c->level--;
		c->ups++;
		c->slack = 0;
		c->hold = c->hold_frames / 2;           /* recover one step at a time */
	}
}

float lodctl_distance_scale(const lodctl_t *c)
{
	static const float k[LODCTL_MAX_LEVEL + 1] = { 1.0f, 0.85f, 0.7f, 0.55f, 0.4f };
	return k[c->level];
}

int lodctl_lod_bias(const lodctl_t *c)
{
	static const int k[LODCTL_MAX_LEVEL + 1] = { 0, 0, 1, 1, 2 };
	return k[c->level];
}

float lodctl_particle_scale(const lodctl_t *c)
{
	static const float k[LODCTL_MAX_LEVEL + 1] = { 1.0f, 1.0f, 0.75f, 0.5f, 0.25f };
	return k[c->level];
}
