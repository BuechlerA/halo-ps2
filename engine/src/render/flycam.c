#include "render/flycam.h"
#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265f

int campath_parse(cam_path *p, const cfg_t *cfg)
{
	int i;
	const char *v;
	p->count = 0;
	for (i = 0; (v = cfg_nth(cfg, "pathkey", i)) != NULL && p->count < CAMPATH_MAX_KEYS; i++) {
		float f[6];
		int n = 0;
		while (v && *v && n < 6) {
			char *end;
			f[n] = strtof(v, &end);
			if (end == v) break;
			n++;
			v = end;
		}
		if (n != 6) continue;
		if (p->count > 0 && f[0] <= p->key[p->count - 1].t) continue;   /* time must strictly increase */
		{
			cam_key *k = &p->key[p->count++];
			k->t = f[0];
			k->pos[0] = f[1]; k->pos[1] = f[2]; k->pos[2] = f[3];
			k->yaw = f[4] * PI_F / 180.0f;
			k->pitch = f[5] * PI_F / 180.0f;
		}
	}
	return p->count;
}

float campath_duration(const cam_path *p)
{
	return p->count ? p->key[p->count - 1].t : 0.0f;
}

static float lerp_angle(float a, float b, float u)
{
	float d = fmodf(b - a, 2.0f * PI_F);
	if (d > PI_F) d -= 2.0f * PI_F;
	if (d < -PI_F) d += 2.0f * PI_F;
	return a + d * u;
}

void campath_eval(const cam_path *p, float t, camera_t *cam)
{
	int i, k;
	float dur = campath_duration(p), u;
	const cam_key *a, *b;
	if (p->count == 0) return;
	if (p->count == 1 || dur <= 0.0f) { a = b = &p->key[0]; u = 0.0f; }
	else {
		t = fmodf(t, dur);
		if (t < 0.0f) t += dur;
		for (i = 0; i + 2 < p->count && t >= p->key[i + 1].t; i++) {}
		a = &p->key[i];
		b = &p->key[i + 1];
		u = (t - a->t) / (b->t - a->t);
		if (u < 0.0f) u = 0.0f;
		if (u > 1.0f) u = 1.0f;
	}
	for (k = 0; k < 3; k++) cam->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * u;
	cam->yaw = lerp_angle(a->yaw, b->yaw, u);
	cam->pitch = a->pitch + (b->pitch - a->pitch) * u;
}

float fly_deadzone(float v)
{
	float m = fabsf(v);
	if (m < FLY_DEADZONE) return 0.0f;
	m = (m - FLY_DEADZONE) / (1.0f - FLY_DEADZONE);
	if (m > 1.0f) m = 1.0f;
	return v < 0.0f ? -m : m;
}

void flycam_update(camera_t *cam, const fly_input *in, float dt)
{
	float speed = FLY_SPEED * (in->fast ? 4.0f : 1.0f) * dt;
	camera_look(cam, -fly_deadzone(in->look_x) * FLY_TURN * dt, fly_deadzone(in->look_y) * FLY_TURN * dt);
	/* Yaw is counter-clockwise: stick right must turn right (clockwise), hence the negation. */
	camera_move(cam, fly_deadzone(in->move_y) * speed, fly_deadzone(in->move_x) * speed, in->rise * speed);
}
