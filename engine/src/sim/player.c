#include "sim/player.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define EPS 0.002f          /* skin distance kept from surfaces */

void player_params_default(player_params_t *p)
{
	/* Placeholders until unitdef supplies the tag values (world units: 1 = 10 ft). */
	p->radius = 0.15f; p->height = 0.7f; p->crouch_height = 0.5f;
	p->eye_height = 0.62f; p->crouch_eye_height = 0.42f;
	p->walk_speed = 1.0f; p->run_speed = 2.2f; p->strafe_speed = 1.8f; p->back_speed = 1.4f; p->crouch_speed = 0.9f;
	p->accel = 14.0f; p->air_accel = 2.5f;
	p->jump_speed = 2.0f; p->gravity = 3.5f; p->terminal_speed = 8.0f;
	p->step_height = 0.17f; p->max_slope_cos = 0.55f;
	p->turn_rate = 2.8f; p->pitch_limit = 1.45f;
}

void player_init(player_t *pl, const float pos[3], float yaw)
{
	memset(pl, 0, sizeof *pl);
	pl->pos[0] = pos[0]; pl->pos[1] = pos[1]; pl->pos[2] = pos[2];
	pl->yaw = yaw;
	pl->ground_normal[2] = 1.0f;
}

void player_eye(const player_t *pl, const player_params_t *par, float eye[3])
{
	eye[0] = pl->pos[0]; eye[1] = pl->pos[1];
	eye[2] = pl->pos[2] + par->eye_height + (par->crouch_eye_height - par->eye_height) * pl->crouch;
}

static float deadzone(float v)
{
	float m = fabsf(v);
	if (m < 0.15f) return 0.0f;
	m = (m - 0.15f) / 0.85f;
	if (m > 1.0f) m = 1.0f;
	return v < 0.0f ? -m : m;
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* Move the capsule by `delta`, sliding along surfaces (up to 4 plane clips). Returns the number of blocking hits;
 * `vel` is clipped like the motion. */
static int slide(const phys_world *w, const player_params_t *par, float height, float pos[3], float delta[3], float vel[3], float lift)
{
	int bumps = 0, it;
	for (it = 0; it < 4; it++) {
		phys_hit h;
		float len2 = dot3(delta, delta);
		if (len2 < 1e-10f) break;
		if (phys_sweep_capsule(w, pos, delta, par->radius, height, lift, &h)) {
			float tt = h.t > 0.0f ? h.t : 0.0f, back, into, n[3];
			float len = sqrtf(len2);
			/* stop short of the surface by a skin distance along the path */
			back = EPS / len;
			tt = tt > back ? tt - back : 0.0f;
			pos[0] += delta[0] * tt; pos[1] += delta[1] * tt; pos[2] += delta[2] * tt;
			bumps++;
			n[0] = h.normal[0]; n[1] = h.normal[1]; n[2] = h.normal[2];
			/* remaining motion and velocity lose their component into the surface */
			{
				float rem[3] = { delta[0] * (1.0f - tt), delta[1] * (1.0f - tt), delta[2] * (1.0f - tt) };
				into = dot3(rem, n);
				if (into < 0.0f) { rem[0] -= n[0] * into; rem[1] -= n[1] * into; rem[2] -= n[2] * into; }
				delta[0] = rem[0]; delta[1] = rem[1]; delta[2] = rem[2];
			}
			into = dot3(vel, n);
			if (into < 0.0f) { vel[0] -= n[0] * into; vel[1] -= n[1] * into; vel[2] -= n[2] * into; }
		} else {
			pos[0] += delta[0]; pos[1] += delta[1]; pos[2] += delta[2];
			break;
		}
	}
	return bumps;
}

static int find_ground(const phys_world *w, const player_params_t *par, float height, float pos[3], float dist, float normal[3])
{
	phys_hit h;
	float down[3] = { 0.0f, 0.0f, -dist };
	if (phys_sweep_capsule(w, pos, down, par->radius, height, 0.0f, &h) && h.normal[2] >= par->max_slope_cos) {
		pos[2] -= dist * h.t - EPS;
		if (h.t <= 0.0f) pos[2] += EPS;
		normal[0] = h.normal[0]; normal[1] = h.normal[1]; normal[2] = h.normal[2];
		return 1;
	}
	return 0;
}

void player_tick(player_t *pl, const player_params_t *par, const phys_world *w, const player_cmd_t *cmd, float dt)
{
	float height, fwd[2], right[2], wish[2], speed, mx, my, len, target[2], dv[2], dl, delta[3];
	int was_ground = pl->on_ground;
	float vz_before;

	/* look */
	{
		float lx = deadzone(cmd->look_x), ly = deadzone(cmd->look_y);
		float mag = fabsf(lx) > fabsf(ly) ? fabsf(lx) : fabsf(ly);
		/* turning ramps up while the stick is held at the edge (Halo's look acceleration) */
		pl->turn_accel += ((mag > 0.9f ? 1.0f : 0.0f) - pl->turn_accel) * 4.0f * dt;
		if (pl->turn_accel < 0.0f) pl->turn_accel = 0.0f;
		{
			float gain = par->turn_rate * (1.0f + 0.8f * pl->turn_accel);
			pl->yaw -= lx * gain * dt;                 /* stick right = clockwise = decreasing yaw */
			pl->pitch += ly * gain * 0.7f * dt;
		}
		if (pl->yaw > PI_F) pl->yaw -= 2.0f * PI_F;
		if (pl->yaw < -PI_F) pl->yaw += 2.0f * PI_F;
		if (pl->pitch > par->pitch_limit) pl->pitch = par->pitch_limit;
		if (pl->pitch < -par->pitch_limit) pl->pitch = -par->pitch_limit;
	}

	/* crouch: smoothly lower the eye, shrink the capsule once mostly down */
	{
		float goal = cmd->crouch ? 1.0f : 0.0f;
		float step = 6.0f * dt;
		if (pl->crouch < goal) { pl->crouch += step; if (pl->crouch > goal) pl->crouch = goal; }
		else if (pl->crouch > goal) {
			/* standing up needs headroom */
			float up[3] = { 0.0f, 0.0f, par->height - par->crouch_height };
			phys_hit h;
			if (!phys_sweep_capsule(w, pl->pos, up, par->radius, par->crouch_height, 0.0f, &h) || h.t >= 1.0f) {
				pl->crouch -= step; if (pl->crouch < goal) pl->crouch = goal;
			}
		}
	}
	height = pl->crouch > 0.5f ? par->crouch_height : par->height;

	/* wish velocity */
	fwd[0] = cosf(pl->yaw); fwd[1] = sinf(pl->yaw);
	right[0] = sinf(pl->yaw); right[1] = -cosf(pl->yaw);
	mx = deadzone(cmd->move_x); my = deadzone(cmd->move_y);
	len = sqrtf(mx * mx + my * my);
	if (len > 1.0f) { mx /= len; my /= len; }
	speed = my >= 0.0f ? (cmd->walk ? par->walk_speed : par->run_speed) : par->back_speed;
	if (pl->crouch > 0.5f && speed > par->crouch_speed) speed = par->crouch_speed;
	wish[0] = (fwd[0] * my * speed) + right[0] * mx * (cmd->walk ? par->walk_speed : par->strafe_speed);
	wish[1] = (fwd[1] * my * speed) + right[1] * mx * (cmd->walk ? par->walk_speed : par->strafe_speed);
	target[0] = wish[0]; target[1] = wish[1];

	{
		float a = (pl->on_ground ? par->accel : par->air_accel) * dt;
		dv[0] = target[0] - pl->vel[0]; dv[1] = target[1] - pl->vel[1];
		dl = sqrtf(dv[0] * dv[0] + dv[1] * dv[1]);
		if (dl > a && dl > 0.0f) { dv[0] *= a / dl; dv[1] *= a / dl; }
		if (!pl->on_ground && len < 0.01f) dv[0] = dv[1] = 0.0f;       /* no braking in the air */
		pl->vel[0] += dv[0]; pl->vel[1] += dv[1];
	}

	/* jump / gravity */
	if (cmd->jump && !pl->jump_held && pl->on_ground) { pl->vel[2] = par->jump_speed; pl->on_ground = 0; }
	pl->jump_held = cmd->jump;
	if (!pl->on_ground) {
		pl->vel[2] -= par->gravity * dt;
		if (pl->vel[2] < -par->terminal_speed) pl->vel[2] = -par->terminal_speed;
	} else {
		/* walking on a slope: keep the velocity in the ground plane */
		float into = dot3(pl->vel, pl->ground_normal);
		if (into < 0.0f) { pl->vel[0] -= pl->ground_normal[0] * into; pl->vel[1] -= pl->ground_normal[1] * into; pl->vel[2] -= pl->ground_normal[2] * into; }
		if (pl->vel[2] > 0.0f && pl->vel[0] * pl->vel[0] + pl->vel[1] * pl->vel[1] < 1e-6f) pl->vel[2] = 0.0f;
	}

	vz_before = pl->vel[2];
	/* move: horizontal-ish with step-up, then vertical */
	delta[0] = pl->vel[0] * dt; delta[1] = pl->vel[1] * dt; delta[2] = pl->vel[2] * dt;
	{
		float p0[3], v0[3], d0[3], p1[3], v1[3], d1[3];
		int blocked;
		memcpy(p0, pl->pos, sizeof p0); memcpy(v0, pl->vel, sizeof v0); memcpy(d0, delta, sizeof d0);
		blocked = slide(w, par, height, p0, d0, v0, 0.0f);
		if (blocked && pl->on_ground && (delta[0] != 0.0f || delta[1] != 0.0f)) {
			/* try again from a raised position, then settle back onto the ground */
			float up[3] = { 0.0f, 0.0f, par->step_height };
			phys_hit h;
			memcpy(p1, pl->pos, sizeof p1); memcpy(v1, pl->vel, sizeof v1);
			d1[0] = delta[0]; d1[1] = delta[1]; d1[2] = 0.0f;
			if (!phys_sweep_capsule(w, p1, up, par->radius, height, 0.0f, &h) || h.t >= 1.0f) {
				float n[3];
				p1[2] += par->step_height;
				slide(w, par, height, p1, d1, v1, 0.0f);
				if (find_ground(w, par, height, p1, par->step_height + 0.02f, n)) {
					float dx0 = p0[0] - pl->pos[0], dy0 = p0[1] - pl->pos[1], dx1 = p1[0] - pl->pos[0], dy1 = p1[1] - pl->pos[1];
					if (dx1 * dx1 + dy1 * dy1 > dx0 * dx0 + dy0 * dy0 + 1e-8f) { memcpy(p0, p1, sizeof p0); v0[0] = v1[0]; v0[1] = v1[1]; }
				}
			}
		}
		memcpy(pl->pos, p0, sizeof p0);
		pl->vel[0] = v0[0]; pl->vel[1] = v0[1];
		if (!pl->on_ground || pl->vel[2] > 0.0f) pl->vel[2] = v0[2];
	}

	/* ground contact */
	{
		float n[3];
		if (pl->vel[2] <= 0.0f && find_ground(w, par, height, pl->pos, was_ground ? par->step_height + 0.02f : 0.04f, n)) {
			if (!was_ground && vz_before < 0.0f) pl->landed_speed = -vz_before;
			pl->on_ground = 1;
			memcpy(pl->ground_normal, n, sizeof n);
			if (pl->vel[2] < 0.0f) pl->vel[2] = 0.0f;
		} else {
			pl->on_ground = 0;
			pl->ground_normal[0] = pl->ground_normal[1] = 0.0f; pl->ground_normal[2] = 1.0f;
		}
	}
}
