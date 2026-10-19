#include "sim/playerdef.h"
#include <math.h>

#define TICKS 30.0f
#define DEG2RAD 0.01745329252f

void player_params_from_tags(player_params_t *p, const unitdef_globals_def *g, const unitdef_biped_def *b)
{
	player_params_default(p);
	if (g) {
		if (g->run_forward > 0.0f) p->run_speed = g->run_forward;
		if (g->run_backward > 0.0f) p->back_speed = g->run_backward;
		if (g->run_sideways > 0.0f) p->strafe_speed = g->run_sideways;
		if (g->walk_speed > 0.0f) p->walk_speed = g->walk_speed;
		if (g->sneak_forward > 0.0f) p->crouch_speed = g->sneak_forward;
		if (g->gravity > 0.0f) p->gravity = g->gravity * TICKS * TICKS;
		if (g->look_yaw_rate_deg > 0.0f) p->turn_rate = g->look_yaw_rate_deg * DEG2RAD;
	}
	if (b) {
		if (b->radius > 0.0f) p->radius = b->radius;
		if (b->stand_height > 0.0f) p->height = b->stand_height;
		if (b->crouch_height > 0.0f) p->crouch_height = b->crouch_height;
		if (b->stand_camera > 0.0f) p->eye_height = b->stand_camera;
		if (b->crouch_camera > 0.0f) p->crouch_eye_height = b->crouch_camera;
		if (b->jump_velocity > 0.0f) p->jump_speed = b->jump_velocity * TICKS;
		if (b->max_slope > 0.0f) p->max_slope_cos = cosf(b->max_slope);
		p->step_height = 0.25f * p->height;
	}
	if (p->crouch_height > p->height) p->crouch_height = p->height;
	if (p->radius * 2.0f > p->crouch_height) p->radius = 0.5f * p->crouch_height;
}

static int w_ray(void *u, const float a[3], const float b[3], phys_hit *h)
{
	coll_hit c;
	if (!coll_ray((const coll_bsp *)u, a, b, 0, &c)) return 0;
	h->t = c.t; h->normal[0] = c.normal[0]; h->normal[1] = c.normal[1]; h->normal[2] = c.normal[2];
	h->point[0] = c.point[0]; h->point[1] = c.point[1]; h->point[2] = c.point[2];
	h->material = c.material; h->flags = c.flags;
	return 1;
}

static int w_sweep(void *u, const float a[3], const float b[3], float r, phys_hit *h)
{
	coll_hit c;
	if (!coll_sphere_sweep((const coll_bsp *)u, a, b, r, &c)) return 0;
	h->t = c.t; h->normal[0] = c.normal[0]; h->normal[1] = c.normal[1]; h->normal[2] = c.normal[2];
	h->point[0] = c.point[0]; h->point[1] = c.point[1]; h->point[2] = c.point[2];
	h->material = c.material; h->flags = c.flags;
	return 1;
}

static int w_solid(void *u, const float p[3]) { return coll_point_in_solid((const coll_bsp *)u, p); }

void phys_world_from_coll(phys_world *w, const coll_bsp *bsp)
{
	w->user = (void *)bsp;
	w->ray = w_ray; w->sweep = w_sweep; w->solid = w_solid;
}
