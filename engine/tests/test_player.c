/* Host tests for sim/player (capsule controller) against an analytic world of axis-aligned boxes. */
#include "sim/player.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

typedef struct { float mn[3], mx[3]; } box_t;
typedef struct { box_t box[8]; int n; } boxworld_t;

static int sweep_box(const box_t *bx, const float a[3], const float b[3], float r, phys_hit *h)
{
	float t0 = 0.0f, t1 = 1.0f, n[3] = { 0, 0, 0 };
	int k, inside = 1;
	for (k = 0; k < 3; k++) {
		float lo = bx->mn[k] - r, hi = bx->mx[k] + r, d = b[k] - a[k];
		if (a[k] < lo || a[k] > hi) inside = 0;
		if (fabsf(d) < 1e-9f) { if (a[k] < lo || a[k] > hi) return 0; continue; }
		{
			float ta = (lo - a[k]) / d, tb = (hi - a[k]) / d, tn, tf;
			float sgn = d > 0 ? -1.0f : 1.0f;     /* entering through lo when moving +, normal points -axis */
			if (ta > tb) { float t = ta; ta = tb; tb = t; }
			tn = ta; tf = tb;
			if (tn > t0) { t0 = tn; n[0] = n[1] = n[2] = 0; n[k] = sgn; }
			if (tf < t1) t1 = tf;
			if (t0 > t1) return 0;
		}
	}
	if (inside) { h->t = 0; h->normal[0] = 0; h->normal[1] = 0; h->normal[2] = 1; h->material = 0; h->flags = 0; return 1; }
	if (t0 <= 0.0f && n[0] == 0 && n[1] == 0 && n[2] == 0) return 0;
	h->t = t0; memcpy(h->normal, n, sizeof n); h->material = 0; h->flags = 0;
	for (k = 0; k < 3; k++) h->point[k] = a[k] + (b[k] - a[k]) * t0;
	return 1;
}

static int w_sweep(void *u, const float a[3], const float b[3], float r, phys_hit *h)
{
	const boxworld_t *w = u;
	int i, found = 0;
	for (i = 0; i < w->n; i++) {
		phys_hit t;
		if (sweep_box(&w->box[i], a, b, r, &t) && (!found || t.t < h->t)) { *h = t; found = 1; }
	}
	return found;
}
static int w_ray(void *u, const float a[3], const float b[3], phys_hit *h) { return w_sweep(u, a, b, 0.0f, h); }

static phys_world mk(boxworld_t *w) { phys_world p; p.user = w; p.ray = w_ray; p.sweep = w_sweep; p.solid = NULL; return p; }

static void add(boxworld_t *w, float x0, float y0, float z0, float x1, float y1, float z1)
{
	box_t *b = &w->box[w->n++];
	b->mn[0] = x0; b->mn[1] = y0; b->mn[2] = z0; b->mx[0] = x1; b->mx[1] = y1; b->mx[2] = z1;
}

static void run(player_t *pl, const player_params_t *par, const phys_world *w, player_cmd_t c, int ticks)
{
	int i;
	for (i = 0; i < ticks; i++) player_tick(pl, par, w, &c, 1.0f / 30.0f);
}

static void test_fall_and_walk(void)
{
	boxworld_t bw = { .n = 0 };
	phys_world w;
	player_params_t par;
	player_t pl;
	player_cmd_t c;
	float start[3] = { 0, 0, 1.0f };
	memset(&c, 0, sizeof c);
	add(&bw, -50, -50, -1, 50, 50, 0);
	w = mk(&bw);
	player_params_default(&par);
	player_init(&pl, start, 0.0f);
	run(&pl, &par, &w, c, 30);
	CHECK(pl.on_ground);
	CHECK(NEAR(pl.pos[2], 0.0f, 0.01f));
	CHECK(pl.landed_speed > 1.0f);
	/* run forward along +x (yaw 0) */
	c.move_y = 1.0f;
	run(&pl, &par, &w, c, 30);
	CHECK(pl.pos[0] > par.run_speed * 0.7f && pl.pos[0] < par.run_speed * 1.05f);
	CHECK(NEAR(pl.pos[1], 0.0f, 1e-3f) && pl.on_ground);
	CHECK(NEAR(pl.pos[2], 0.0f, 0.01f));
	/* walking is slower */
	player_init(&pl, (float[3]){ 0, 0, 0 }, 0.0f);
	c.walk = 1;
	run(&pl, &par, &w, c, 30);
	CHECK(pl.pos[0] < par.walk_speed * 1.05f && pl.pos[0] > par.walk_speed * 0.6f);
}

static void test_wall_slide(void)
{
	boxworld_t bw = { .n = 0 };
	phys_world w;
	player_params_t par;
	player_t pl;
	player_cmd_t c;
	memset(&c, 0, sizeof c);
	add(&bw, -50, -50, -1, 50, 50, 0);
	add(&bw, 2, -50, 0, 3, 50, 5);                    /* wall at x = 2 */
	w = mk(&bw);
	player_params_default(&par);
	player_init(&pl, (float[3]){ 0, 0, 0 }, 0.0f);
	c.move_y = 1.0f;
	run(&pl, &par, &w, c, 90);
	CHECK(pl.pos[0] < 2.0f - par.radius + 0.01f && pl.pos[0] > 2.0f - par.radius - 0.05f);
	/* diagonal into the wall keeps sliding along it */
	player_init(&pl, (float[3]){ 0, 0, 0 }, 0.5f);      /* looking up-left of +x */
	run(&pl, &par, &w, c, 120);
	CHECK(pl.pos[0] < 2.0f - par.radius + 0.01f);
	CHECK(pl.pos[1] > 1.0f);                           /* slid along +y */
}

static void test_steps_jump(void)
{
	boxworld_t bw = { .n = 0 };
	phys_world w;
	player_params_t par;
	player_t pl;
	player_cmd_t c;
	float peak = 0;
	int i;
	memset(&c, 0, sizeof c);
	add(&bw, -50, -50, -1, 50, 50, 0);
	add(&bw, 2, -50, 0, 4, 50, 0.1f);                 /* a low step (climbable) */
	add(&bw, 6, -50, 0, 8, 50, 0.5f);                 /* a high one (blocks) */
	w = mk(&bw);
	player_params_default(&par);
	player_init(&pl, (float[3]){ 0, 0, 0 }, 0.0f);
	c.move_y = 1.0f;
	run(&pl, &par, &w, c, 80);
	CHECK(pl.pos[0] > 3.0f);
	CHECK(pl.on_ground);
	run(&pl, &par, &w, c, 90);
	CHECK(pl.pos[0] < 6.0f - par.radius + 0.01f);     /* stopped by the high step */
	/* jump on flat ground */
	player_init(&pl, (float[3]){ -20, 0, 0 }, 0.0f);
	run(&pl, &par, &w, (player_cmd_t){ 0 }, 5);
	c.move_y = 0; c.jump = 1;
	for (i = 0; i < 40; i++) { player_tick(&pl, &par, &w, &c, 1.0f / 30.0f); if (pl.pos[2] > peak) peak = pl.pos[2]; }
	{
		float expect = par.jump_speed * par.jump_speed / (2.0f * par.gravity);
		CHECK(peak > expect * 0.8f && peak < expect * 1.2f);
	}
	CHECK(pl.on_ground);                               /* landed again, holding jump does not rebound */
	CHECK(pl.pos[2] < 0.05f);
}

static void test_crouch_headroom(void)
{
	boxworld_t bw = { .n = 0 };
	phys_world w;
	player_params_t par;
	player_t pl;
	player_cmd_t c;
	memset(&c, 0, sizeof c);
	add(&bw, -50, -50, -1, 50, 50, 0);
	add(&bw, -50, -50, 0.6f, 50, 50, 2);              /* ceiling at 0.6: crouch height is 0.5, standing 0.7 */
	w = mk(&bw);
	player_params_default(&par);
	player_init(&pl, (float[3]){ 0, 0, 0 }, 0.0f);
	c.crouch = 1;
	run(&pl, &par, &w, c, 20);
	CHECK(pl.crouch > 0.99f);
	c.crouch = 0;
	run(&pl, &par, &w, c, 20);
	CHECK(pl.crouch > 0.9f);                           /* cannot stand under the ceiling */
	{
		float eye[3];
		player_eye(&pl, &par, eye);
		CHECK(eye[2] < 0.6f);
	}
}

static void test_ledge(void)
{
	boxworld_t bw = { .n = 0 };
	phys_world w;
	player_params_t par;
	player_t pl;
	player_cmd_t c;
	memset(&c, 0, sizeof c);
	add(&bw, -50, -50, -1, 3, 50, 0);                 /* floor ends at x = 3 */
	add(&bw, 3, -50, -10, 50, 50, -3);                /* lower floor 3 units down */
	w = mk(&bw);
	player_params_default(&par);
	player_init(&pl, (float[3]){ 0, 0, 0 }, 0.0f);
	c.move_y = 1.0f;
	run(&pl, &par, &w, c, 120);
	CHECK(pl.pos[0] > 3.0f);
	CHECK(pl.pos[2] < -2.5f);
	CHECK(pl.landed_speed > 1.0f);
}

static void test_look(void)
{
	boxworld_t bw = { .n = 0 };
	phys_world w = mk(&bw);
	player_params_t par;
	player_t pl;
	player_cmd_t c;
	memset(&c, 0, sizeof c);
	player_params_default(&par);
	player_init(&pl, (float[3]){ 0, 0, 5 }, 0.0f);
	c.look_y = 1.0f;
	run(&pl, &par, &w, c, 300);
	CHECK(NEAR(pl.pitch, par.pitch_limit, 1e-4f));
	c.look_y = 0; c.look_x = 1.0f;
	run(&pl, &par, &w, c, 10);
	CHECK(pl.yaw < -0.5f && pl.yaw > -3.0f);                             /* stick right turned clockwise */
	c.look_x = 0.05f;                                  /* inside the deadzone */
	{
		float y = pl.yaw;
		run(&pl, &par, &w, c, 10);
		CHECK(pl.yaw == y);
	}
}

int main(void)
{
	test_fall_and_walk();
	test_wall_slide();
	test_steps_jump();
	test_crouch_headroom();
	test_ledge();
	test_look();
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
