/* Cinematics and camera runtime (docs/28-cine-section.md). Pure C11, no heap, no double. See cine.h. */
#include "sim/cine.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define TWO_PI_F 6.28318531f
#define MAX_PITCH_CAMERA 1.5533430f      /* 89 degrees, as camera_look */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lerpf(float a, float b, float u) { return a + (b - a) * u; }

static float wrap_pi(float a)
{
	int i;
	if (!(a == a)) return 0.0f;
	for (i = 0; i < 64 && a > PI_F; i++) a -= TWO_PI_F;
	for (i = 0; i < 64 && a <= -PI_F; i++) a += TWO_PI_F;
	return a;
}


/* ---- 1. recorded animation playback ------------------------------------------------------------------------------ */
static uint32_t change_bit(unsigned type)
{
	switch (type) {
	case CINEDEF_EV_MODE: return CINE_CH_MODE;
	case CINEDEF_EV_FLAGS: return CINE_CH_FLAGS;
	case CINEDEF_EV_WEAPON: return CINE_CH_WEAPON;
	case CINEDEF_EV_THROTTLE: return CINE_CH_THROTTLE;
	case CINEDEF_EV_AIM_SMALL:
	case CINEDEF_EV_AIM_LARGE: return CINE_CH_AIM;
	default: return 0;
	}
}

/* applies every event due at or before tick `t` */
static void play_advance(cine_play *p, uint32_t t)
{
	cinedef_event ev;
	while (p->pos < p->bytes && p->next_time <= t) {
		if (!cinedef_next_event(p->stream, p->bytes, &p->pos, &ev)) {
			p->pos = p->bytes;                 /* malformed: a validated section never gets here */
			break;
		}
		cinedef_apply_event(&p->ctl, &ev);
		p->changed |= change_bit(ev.type);
		p->next_time += ev.wait;
	}
}

int cine_play_start(cine_play *p, const cinedef_section *sec, uint32_t rec)
{
	cinedef_rec r;
	memset(p, 0, sizeof *p);
	if (!sec || !sec->data || rec >= sec->rec_count) return 0;
	r = cinedef_get_rec(sec, rec);
	p->sec = sec;
	p->rec = rec;
	p->stream = cinedef_rec_stream(sec, rec, &p->bytes);
	p->length = r.length;
	p->ctl = r.init;
	p->changed = CINE_CH_ALL;
	play_advance(p, 0);
	return 1;
}

void cine_play_tick(cine_play *p)
{
	p->changed = 0;
	if (!p->sec || p->done) return;
	p->tick++;
	if (p->tick >= p->length) {
		p->done = 1;
		return;
	}
	play_advance(p, p->tick);
}

void cine_play_seek(cine_play *p, uint32_t tick)
{
	cinedef_rec r;
	uint32_t t;
	if (!p->sec) return;
	r = cinedef_get_rec(p->sec, p->rec);
	p->ctl = r.init;
	p->pos = 0;
	p->next_time = 0;
	t = tick < p->length ? tick : p->length;
	p->changed = CINE_CH_ALL;
	play_advance(p, t < p->length ? t : p->length - 1);
	p->tick = t;
	p->done = t >= p->length;
}

float cine_ctl_yaw(const cinedef_ctl *c) { return (float)c->yaw * CINE_COUNT_RAD; }
float cine_ctl_pitch(const cinedef_ctl *c) { return (float)c->pitch * CINE_COUNT_RAD; }

void cine_ctl_aim_vector(const cinedef_ctl *c, float out[3])
{
	float y = cine_ctl_yaw(c), p = cine_ctl_pitch(c), cp = cosf(p);
	out[0] = cp * cosf(y);
	out[1] = cp * sinf(y);
	out[2] = sinf(p);
}

/* ---- quaternions ------------------------------------------------------------------------------------------------------ */
static void quat_mul(const float a[4], const float b[4], float o[4])
{
	o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void cine_ypr_to_quat(float yaw, float pitch, float roll, float q[4])
{
	float qz[4] = {0.0f, 0.0f, sinf(yaw * 0.5f), cosf(yaw * 0.5f)};
	float qy[4] = {0.0f, -sinf(pitch * 0.5f), 0.0f, cosf(pitch * 0.5f)};
	float qx[4] = {sinf(roll * 0.5f), 0.0f, 0.0f, cosf(roll * 0.5f)};
	float t[4];
	quat_mul(qz, qy, t);
	quat_mul(t, qx, q);
}

void cine_halo_euler_to_ypr(float yaw, float pitch, float roll, float *oy, float *op, float *orr)
{
	float c1 = cosf(yaw), s1 = sinf(yaw), c2 = cosf(pitch), s2 = sinf(pitch), c3 = cosf(roll), s3 = sinf(roll);
	float fx = c1 * c2, fy = s1 * c3 - s2 * s3 * c1, fz = s1 * s3 + s2 * c3 * c1;       /* forward */
	float ux = -s2, uy = -c2 * s3, uz = c2 * c3;                                          /* up */
	float y = atan2f(fy, fx), p = asinf(clampf(fz, -1.0f, 1.0f)), sy = sinf(y), cy = cosf(y), sp = sinf(p), cp = cosf(p);
	float u0x = -sp * cy, u0y = -sp * sy, u0z = cp, r0x = sy, r0y = -cy;                  /* level up and right of that heading */
	*oy = y;
	*op = p;
	*orr = atan2f(ux * r0x + uy * r0y, ux * u0x + uy * u0y + uz * u0z);                   /* camera_t banks right: up' = sin r right + cos r up */
}

void cine_quat_to_ypr(const float q[4], float *yaw, float *pitch, float *roll)
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float fx = 1.0f - 2.0f * (y * y + z * z), fy = 2.0f * (x * y + w * z), fz = 2.0f * (x * z - w * y);
	float lx = 2.0f * (x * y - w * z), ly = 1.0f - 2.0f * (x * x + z * z), lz = 2.0f * (y * z + w * x);
	float yw = atan2f(fy, fx), pt = asinf(clampf(fz, -1.0f, 1.0f));
	float sy = sinf(yw), cy = cosf(yw);
	/* level left axis L = (-sin yaw, cos yaw, 0); up0 = forward x L */
	float ux = -fz * cy, uy = -fz * sy, uz = fx * cy + fy * sy;
	*yaw = yw;
	*pitch = pt;
	*roll = atan2f(lx * ux + ly * uy + lz * uz, -lx * sy + ly * cy);
}

static void quat_forward(const float q[4], float f[3])
{
	f[0] = 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]);
	f[1] = 2.0f * (q[0] * q[1] + q[3] * q[2]);
	f[2] = 2.0f * (q[0] * q[2] - q[3] * q[1]);
}

static void quat_nlerp(const float a[4], const float b[4], float u, float o[4])
{
	float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	float s = dot < 0.0f ? -1.0f : 1.0f, len2 = 0.0f, inv;
	int i;
	for (i = 0; i < 4; i++) {
		o[i] = a[i] * (1.0f - u) + b[i] * s * u;
		len2 += o[i] * o[i];
	}
	if (len2 < 1e-12f) {
		o[0] = o[1] = o[2] = 0.0f;
		o[3] = 1.0f;
		return;
	}
	inv = 1.0f / sqrtf(len2);
	for (i = 0; i < 4; i++) o[i] *= inv;
}

/* ---- 2. camera track sampler ---------------------------------------------------------------------------------------------- */
static float catmull(float p0, float p1, float p2, float p3, float u)
{
	return 0.5f * ((2.0f * p1) + (-p0 + p2) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u * u +
	               (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u * u * u);
}

static void track_segment(const cinedef_section *s, uint32_t track, uint32_t n, uint32_t seg, float u, cine_track_pose *out)
{
	cinedef_point p0 = cinedef_get_point(s, track, seg ? seg - 1 : 0), p1 = cinedef_get_point(s, track, seg);
	cinedef_point p2 = cinedef_get_point(s, track, seg + 1), p3 = cinedef_get_point(s, track, seg + 2 < n ? seg + 2 : n - 1);
	int i;
	for (i = 0; i < 3; i++) out->pos[i] = catmull(p0.pos[i], p1.pos[i], p2.pos[i], p3.pos[i], u);
	quat_nlerp(p1.quat, p2.quat, u, out->quat);
}

int cine_track_sample(const cinedef_section *s, uint32_t track, float t, cine_track_pose *out)
{
	uint32_t n, seg;
	float f;
	if (!s || !s->data || track >= s->track_count) return 0;
	n = cinedef_get_track(s, track).point_count;
	if (n < 2) return 0;
	if (!(t == t)) t = 0.0f;
	t = clampf(t, 0.0f, 1.0f);
	f = t * (float)(n - 1);
	seg = (uint32_t)f;
	if (seg > n - 2) seg = n - 2;
	track_segment(s, track, n, seg, clampf(f - (float)seg, 0.0f, 1.0f), out);
	return 1;
}

float cine_track_point_pitch(const cinedef_section *s, uint32_t track, uint32_t k)
{
	cinedef_point p = cinedef_get_point(s, track, k);
	float f[3];
	quat_forward(p.quat, f);
	return asinf(clampf(f[2], -1.0f, 1.0f));
}

int cine_track_sample_pitch(const cinedef_section *s, uint32_t track, float pitch, cine_track_pose *out)
{
	uint32_t n, k, seg = 0;
	float u = 0.0f, a, b, first, last;
	if (!s || !s->data || track >= s->track_count) return 0;
	n = cinedef_get_track(s, track).point_count;
	if (n < 2) return 0;
	if (!(pitch == pitch)) pitch = 0.0f;
	first = cine_track_point_pitch(s, track, 0);
	last = cine_track_point_pitch(s, track, n - 1);
	if ((first <= last && pitch <= first) || (first > last && pitch >= first)) {
		seg = 0;
		u = 0.0f;
	} else if ((first <= last && pitch >= last) || (first > last && pitch <= last)) {
		seg = n - 2;
		u = 1.0f;
	} else {
		seg = n - 2;
		u = 1.0f;
		for (k = 0; k + 1 < n; k++) {
			a = cine_track_point_pitch(s, track, k);
			b = cine_track_point_pitch(s, track, k + 1);
			if ((pitch >= a && pitch <= b) || (pitch <= a && pitch >= b)) {
				seg = k;
				u = b != a ? clampf((pitch - a) / (b - a), 0.0f, 1.0f) : 0.0f;
				break;
			}
		}
	}
	track_segment(s, track, n, seg, u, out);
	return 1;
}

/* ---- 3. cinematic camera --------------------------------------------------------------------------------------------------- */
void cine_pose_compose(const cine_pose *rel, const cine_frame *anchor, cine_pose *world)
{
	float cy = cosf(anchor->yaw), sy = sinf(anchor->yaw), x = rel->pos[0], y = rel->pos[1], z = rel->pos[2];
	cine_pose o = *rel;
	o.pos[0] = anchor->pos[0] + cy * x - sy * y;
	o.pos[1] = anchor->pos[1] + sy * x + cy * y;
	o.pos[2] = anchor->pos[2] + z;
	o.yaw = wrap_pi(rel->yaw + anchor->yaw);
	*world = o;
}

void cine_pose_relative(const cine_pose *world, const cine_frame *anchor, cine_pose *rel)
{
	float cy = cosf(anchor->yaw), sy = sinf(anchor->yaw);
	float dx = world->pos[0] - anchor->pos[0], dy = world->pos[1] - anchor->pos[1], dz = world->pos[2] - anchor->pos[2];
	cine_pose o = *world;
	o.pos[0] = cy * dx + sy * dy;
	o.pos[1] = -sy * dx + cy * dy;
	o.pos[2] = dz;
	o.yaw = wrap_pi(world->yaw - anchor->yaw);
	*rel = o;
}

/* Halo's observer: every channel (position, orientation as a rotation vector from the target, field of view) follows
 * x(s) = c3 s^3 + c4 s^4 + c5 s^5 with s = remaining / total ticks, the offset from the target. The coefficients make the offset,
 * velocity and acceleration at the retarget equal to what the camera had (a quintic: no jump in speed or acceleration when a
 * script retargets in mid-flight). */
enum { CAM_CH = 7 };

static void quat_conj(const float q[4], float o[4])
{
	o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3];
}

static void quat_log(const float q[4], float rv[3])
{
	float s = q[3] < 0.0f ? -1.0f : 1.0f, x = q[0] * s, y = q[1] * s, z = q[2] * s, w = q[3] * s;
	float n = sqrtf(x * x + y * y + z * z), k;
	k = n > 1e-6f ? 2.0f * atan2f(n, w) / n : 2.0f;
	rv[0] = x * k; rv[1] = y * k; rv[2] = z * k;
}

static void quat_exp(const float rv[3], float q[4])
{
	float a = sqrtf(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]), k = a > 1e-6f ? sinf(a * 0.5f) / a : 0.5f;
	q[0] = rv[0] * k; q[1] = rv[1] * k; q[2] = rv[2] * k; q[3] = cosf(a * 0.5f);
}

static void pose_offset(const cine_pose *p, const cine_pose *tgt, const float qt[4], float ch[CAM_CH])
{
	float q[4], qc[4], r[4];
	int i;
	cine_ypr_to_quat(p->yaw, p->pitch, p->roll, q);
	quat_conj(qt, qc);
	quat_mul(q, qc, r);
	for (i = 0; i < 3; i++) ch[i] = p->pos[i] - tgt->pos[i];
	quat_log(r, ch + 3);
	ch[6] = p->fov - tgt->fov;
}

static void cam_retarget(cine_camera *c, uint32_t ticks, const cine_pose *to, int relative, const cine_frame *anchor)
{
	cine_pose h[3];
	float ch[3][CAM_CH], T = (float)ticks;
	int i, k, n;
	h[0] = c->from;                                         /* the pose now (set by the caller, in the new space) */
	for (i = 1; i < 3; i++) {
		h[i] = c->hist[i];
		if (c->hist_n > (uint8_t)i && relative != c->hist_rel) {                                       /* history from the other space */
			cine_pose o;
			if (relative) cine_pose_relative(&h[i], anchor, &o);
			else cine_pose_compose(&h[i], anchor, &o);
			h[i] = o;
		}
	}
	n = c->hist_n;
	c->to = *to;
	cine_ypr_to_quat(to->yaw, to->pitch, to->roll, c->qt);
	c->hist_rel = (uint8_t)relative;
	c->hist[0] = h[0];
	c->hist[1] = n > 1 ? h[1] : h[0];
	c->hist[2] = n > 2 ? h[2] : c->hist[1];
	c->hist_n = 3;
	memset(c->coef, 0, sizeof c->coef);
	if (!ticks) return;
	for (i = 0; i < 3; i++) pose_offset(&c->hist[i], to, c->qt, ch[i]);
	for (k = 0; k < CAM_CH; k++) {
		float D = ch[0][k], v = ch[0][k] - ch[1][k], a = ch[0][k] - 2.0f * ch[1][k] + ch[2][k];
		float V = -v * T, A = a * T * T;                    /* d/ds and d2/ds2 at s = 1 */
		c->coef[k][0] = 10.0f * D - 4.0f * V + 0.5f * A;
		c->coef[k][1] = 7.0f * V - 15.0f * D - A;
		c->coef[k][2] = 6.0f * D - 3.0f * V + 0.5f * A;
	}
}

static void cam_eval(cine_camera *c)
{
	cine_pose t = c->to;
	float s = c->ticks && c->elapsed < c->ticks ? (float)(c->ticks - c->elapsed) / (float)c->ticks : 0.0f, ch[CAM_CH], q[4], r[4];
	int k;
	for (k = 0; k < CAM_CH; k++) ch[k] = ((c->coef[k][2] * s + c->coef[k][1]) * s + c->coef[k][0]) * s * s * s;
	for (k = 0; k < 3; k++) t.pos[k] = c->to.pos[k] + ch[k];
	t.fov = c->to.fov + ch[6];
	quat_exp(ch + 3, r);
	quat_mul(r, c->qt, q);
	cine_quat_to_ypr(q, &t.yaw, &t.pitch, &t.roll);
	if (s == 0.0f) {                                        /* arrived: exactly the target, no Euler round trip */
		t.yaw = c->to.yaw; t.pitch = c->to.pitch; t.roll = c->to.roll;
	}
	if (c->relative) cine_pose_compose(&t, &c->anchor, &c->cur);
	else c->cur = t;
	if (c->fov_active) c->cur.fov = lerpf(c->fov_from, c->fov_to, c->fov_ticks ? clampf((float)c->fov_elapsed / (float)c->fov_ticks, 0.0f, 1.0f) : 1.0f);
	c->rel_cur = t;
}

static void cam_reset_history(cine_camera *c)
{
	cine_ypr_to_quat(c->to.yaw, c->to.pitch, c->to.roll, c->qt);
	c->hist[0] = c->hist[1] = c->hist[2] = c->rel_cur;
	c->hist_n = 1;
	c->hist_rel = c->relative;
	memset(c->coef, 0, sizeof c->coef);
}

void cine_cam_init(cine_camera *c)
{
	memset(c, 0, sizeof *c);
	c->player.fov = c->cur.fov = c->from.fov = c->to.fov = CINE_DEFAULT_FOV;
	c->mode = CINE_CAM_PLAYER;
}

void cine_cam_set_player_pose(cine_camera *c, const cine_pose *p) { c->player = *p; }

void cine_cam_control(cine_camera *c, int on)
{
	if (on) {
		if (!c->control) {
			c->control = 1;
			c->mode = CINE_CAM_POSE;
			c->relative = 0;
			c->ticks = c->elapsed = 0;
			c->from = c->to = c->cur = c->rel_cur = c->player;
			cam_reset_history(c);
		}
	} else {
		c->control = 0;
		c->mode = CINE_CAM_PLAYER;
		c->fov_active = 0;
		c->relative = 0;
	}
}

static cine_pose cam_start_pose(const cine_camera *c) { return c->control && c->mode == CINE_CAM_POSE ? c->cur : c->player; }

void cine_cam_set(cine_camera *c, const cine_pose *to, uint32_t ticks, int ease)
{
	cine_pose start = cam_start_pose(c);
	c->control = 1;
	c->mode = CINE_CAM_POSE;
	c->relative = 0;
	c->fov_active = 0;
	c->ease = (uint8_t)ease;
	c->from = start;
	c->ticks = ticks;
	c->elapsed = 0;
	cam_retarget(c, ticks, to, 0, &c->anchor);
	cam_eval(c);
	if (!ticks) { c->hist_n = 1; cam_reset_history(c); }
}

void cine_cam_set_relative(cine_camera *c, const cine_pose *rel, uint32_t ticks, int ease, const cine_frame *anchor)
{
	cine_pose start = cam_start_pose(c);
	c->control = 1;
	c->mode = CINE_CAM_POSE;
	c->fov_active = 0;
	c->ease = (uint8_t)ease;
	c->anchor = *anchor;
	cine_pose_relative(&start, anchor, &c->from);
	c->relative = 1;
	c->ticks = ticks;
	c->elapsed = 0;
	cam_retarget(c, ticks, rel, 1, anchor);
	cam_eval(c);
	if (!ticks) cam_reset_history(c);
}

void cine_cam_set_anchor(cine_camera *c, const cine_frame *anchor)
{
	c->anchor = *anchor;
	if (c->control && c->mode == CINE_CAM_POSE && c->relative) cam_eval(c);
}

void cine_cam_set_first_person(cine_camera *c)
{
	c->control = 1;
	c->mode = CINE_CAM_FIRST_PERSON;
}

void cine_cam_set_dead(cine_camera *c)
{
	c->control = 1;
	c->mode = CINE_CAM_DEAD;
}

void cine_cam_set_fov(cine_camera *c, float fov, uint32_t ticks)
{
	cine_pose now = cine_cam_pose(c);
	if (!(fov == fov) || fov <= 0.0f) return;
	c->fov_active = 1;
	c->fov_from = now.fov;
	c->fov_to = fov;
	c->fov_ticks = ticks;
	c->fov_elapsed = 0;
	if (c->control && c->mode == CINE_CAM_POSE) cam_eval(c);
}

void cine_cam_set_near_clip(cine_camera *c, float dist) { c->near_clip = (dist == dist && dist > 0.0f) ? dist : 0.0f; }

void cine_cam_tick(cine_camera *c)
{
	if (!c->control || c->mode != CINE_CAM_POSE) return;
	if (c->elapsed < c->ticks) c->elapsed++;
	if (c->fov_active && c->fov_elapsed < c->fov_ticks) c->fov_elapsed++;
	cam_eval(c);
	c->hist[2] = c->hist[1];
	c->hist[1] = c->hist[0];
	c->hist[0] = c->rel_cur;
	if (c->hist_n < 3) c->hist_n++;
}

uint32_t cine_cam_time(const cine_camera *c)
{
	if (!c->control || c->mode != CINE_CAM_POSE || c->elapsed >= c->ticks) return 0;
	return c->ticks - c->elapsed;
}

cine_pose cine_cam_pose(const cine_camera *c) { return c->control && c->mode == CINE_CAM_POSE ? c->cur : c->player; }

void cine_pose_to_camera(const cine_pose *p, camera_t *cam)
{
	int i;
	for (i = 0; i < 3; i++) cam->pos[i] = p->pos[i];
	cam->yaw = p->yaw;
	cam->pitch = clampf(p->pitch, -MAX_PITCH_CAMERA, MAX_PITCH_CAMERA);
	cam->roll = p->roll;
	cam->hfov = p->fov;
}

void cine_track_world(const cine_track_pose *tp, const float origin[3], float yaw, float fov, cine_pose *out)
{
	float qz[4] = {0.0f, 0.0f, sinf(yaw * 0.5f), cosf(yaw * 0.5f)}, q[4];
	float cy = cosf(yaw), sy = sinf(yaw);
	out->pos[0] = origin[0] + cy * tp->pos[0] - sy * tp->pos[1];
	out->pos[1] = origin[1] + sy * tp->pos[0] + cy * tp->pos[1];
	out->pos[2] = origin[2] + tp->pos[2];
	quat_mul(qz, tp->quat, q);
	cine_quat_to_ypr(q, &out->yaw, &out->pitch, &out->roll);
	out->fov = fov;
}

/* ---- 4. screen state --------------------------------------------------------------------------------------------------------- */
void cine_start(cine_state *s) { s->screen.cinematic = 1; }

void cine_stop(cine_state *s)
{
	s->screen.cinematic = 0;
	s->screen.skippable = 0;
	s->screen.skip_requested = 0;
}

void cine_skip_begin(cine_state *s)
{
	s->screen.skippable = 1;
	s->screen.skip_requested = 0;
}

void cine_skip_end(cine_state *s)
{
	s->screen.skippable = 0;
	s->screen.skip_requested = 0;
}

void cine_skip_request(cine_state *s)
{
	if (s->screen.skippable) s->screen.skip_requested = 1;
}

int cine_skip_pending(const cine_state *s) { return s->screen.skippable && s->screen.skip_requested; }

void cine_letterbox(cine_state *s, int on) { s->screen.letterbox_on = on ? 1 : 0; }

int cine_letterbox_bar(const cine_state *s, int screen_h)
{
	if (screen_h <= 0) return 0;
	return (int)(s->screen.letterbox * CINE_LETTERBOX_FRACTION * (float)screen_h + 0.5f);
}

static void fade_to(cine_state *s, float r, float g, float b, uint32_t ticks, float target)
{
	cine_screen *sc = &s->screen;
	sc->fade_rgb[0] = clampf(r, 0.0f, 1.0f);
	sc->fade_rgb[1] = clampf(g, 0.0f, 1.0f);
	sc->fade_rgb[2] = clampf(b, 0.0f, 1.0f);
	sc->fade_from = sc->fade_alpha;
	sc->fade_to = target;
	sc->fade_ticks = ticks;
	sc->fade_elapsed = 0;
	if (!ticks) sc->fade_alpha = target;
}

void cine_fade_out(cine_state *s, float r, float g, float b, uint32_t ticks) { fade_to(s, r, g, b, ticks, 1.0f); }
void cine_fade_in(cine_state *s, float r, float g, float b, uint32_t ticks) { fade_to(s, r, g, b, ticks, 0.0f); }

static void screen_tick(cine_screen *sc)
{
	int want = sc->letterbox_on || sc->cinematic;            /* cinematic mode shows the bars by itself; the built-in adds them outside one */
	if (want && sc->letterbox_pos < CINE_LETTERBOX_TICKS) sc->letterbox_pos++;
	else if (!want && sc->letterbox_pos > 0) sc->letterbox_pos--;
	sc->letterbox = (float)sc->letterbox_pos / (float)CINE_LETTERBOX_TICKS;
	if (sc->fade_elapsed < sc->fade_ticks) {
		sc->fade_elapsed++;
		sc->fade_alpha = lerpf(sc->fade_from, sc->fade_to, (float)sc->fade_elapsed / (float)sc->fade_ticks);
	}
}

/* ---- 5. titles and messages ------------------------------------------------------------------------------------------------------- */
int cine_title_post(cine_state *s, const cinedef_title *def, const uint16_t *text, uint32_t units, uint32_t delay)
{
	int slot = -1, i;
	uint32_t best = 0;
	cine_title *t;
	for (i = 0; i < CINE_TITLE_SLOTS; i++)
		if (!s->titles[i].active) {
			slot = i;
			break;
		}
	if (slot < 0) {
		slot = 0;
		for (i = 0; i < CINE_TITLE_SLOTS; i++)
			if (s->titles[i].elapsed > best) {
				best = s->titles[i].elapsed;
				slot = i;
			}
	}
	t = &s->titles[slot];
	memset(t, 0, sizeof *t);
	t->active = 1;
	t->delay = delay;
	t->def = *def;
	if (units > CINE_TEXT_UNITS) units = CINE_TEXT_UNITS;
	if (text && units) memcpy(t->text, text, (size_t)units * sizeof t->text[0]);
	t->units = (uint16_t)(text ? units : 0);
	return slot;
}

int cine_title_show(cine_state *s, const cinedef_section *sec, uint32_t index, uint32_t delay)
{
	cinedef_title def;
	uint16_t text[CINE_TEXT_UNITS];
	uint32_t units = 0;
	if (!sec || !sec->data || index >= sec->title_count) return -1;
	def = cinedef_get_title(sec, index);
	if (def.string != CINEDEF_NONE16) {
		units = cinedef_get_string(sec, def.string, text, CINE_TEXT_UNITS);
		if (units == 0xFFFFFFFFu) units = 0;
		if (units > CINE_TEXT_UNITS) units = CINE_TEXT_UNITS;
	}
	return cine_title_post(s, &def, text, units, delay);
}

float cine_title_alpha(const cine_title *t)
{
	uint32_t e = t->elapsed, fi = t->def.fade_in, up = t->def.up, fo = t->def.fade_out;
	if (!t->active || t->delay) return 0.0f;
	if (e < fi) return (float)e / (float)fi;
	if (e < fi + up) return 1.0f;
	if (e < fi + up + fo) return 1.0f - (float)(e - fi - up) / (float)fo;
	return 0.0f;
}

void cine_title_clear(cine_state *s) { memset(s->titles, 0, sizeof s->titles); }

static void titles_tick(cine_state *s)
{
	int i;
	for (i = 0; i < CINE_TITLE_SLOTS; i++) {
		cine_title *t = &s->titles[i];
		if (!t->active) continue;
		if (t->delay) {
			t->delay--;
			continue;
		}
		t->elapsed++;
		if (t->elapsed >= (uint32_t)t->def.fade_in + t->def.up + t->def.fade_out) t->active = 0;
	}
}

int cine_msg_post(cine_state *s, int kind, const uint16_t *text, uint32_t units, uint32_t duration)
{
	int slot = -1, i;
	uint32_t best = 0;
	cine_msg *m;
	if (kind < 0 || kind >= CINE_MSG_KINDS) return -1;
	if (kind == CINE_MSG_HELP || kind == CINE_MSG_OBJECTIVE) {
		for (i = 0; i < CINE_MSG_SLOTS; i++)
			if (s->msgs[i].active && s->msgs[i].kind == kind) {
				slot = i;
				break;
			}
	}
	if (slot < 0)
		for (i = 0; i < CINE_MSG_SLOTS; i++)
			if (!s->msgs[i].active) {
				slot = i;
				break;
			}
	if (slot < 0) {
		slot = 0;
		for (i = 0; i < CINE_MSG_SLOTS; i++)
			if (s->msgs[i].elapsed > best) {
				best = s->msgs[i].elapsed;
				slot = i;
			}
	}
	m = &s->msgs[slot];
	memset(m, 0, sizeof *m);
	m->active = 1;
	m->kind = (uint8_t)kind;
	m->duration = duration;
	if (units > CINE_TEXT_UNITS) units = CINE_TEXT_UNITS;
	if (text && units) memcpy(m->text, text, (size_t)units * sizeof m->text[0]);
	m->units = (uint16_t)(text ? units : 0);
	return slot;
}

int cine_msg_post_ascii(cine_state *s, int kind, const char *text, uint32_t duration)
{
	uint16_t buf[CINE_TEXT_UNITS];
	uint32_t n = 0;
	if (text)
		while (text[n] && n < CINE_TEXT_UNITS) {
			buf[n] = (uint8_t)text[n];
			n++;
		}
	return cine_msg_post(s, kind, buf, n, duration);
}

void cine_msg_clear(cine_state *s, int kind)
{
	int i;
	for (i = 0; i < CINE_MSG_SLOTS; i++)
		if (s->msgs[i].active && s->msgs[i].kind == kind) s->msgs[i].active = 0;
}

void cine_msg_set_visible(cine_state *s, int kind, int visible)
{
	if (kind >= 0 && kind < CINE_MSG_KINDS) s->msg_visible[kind] = visible ? 1 : 0;
}

const cine_msg *cine_msg_slot(const cine_state *s, unsigned i)
{
	if (i >= CINE_MSG_SLOTS || !s->msgs[i].active || !s->msg_visible[s->msgs[i].kind]) return NULL;
	return &s->msgs[i];
}

static void msgs_tick(cine_state *s)
{
	int i;
	for (i = 0; i < CINE_MSG_SLOTS; i++) {
		cine_msg *m = &s->msgs[i];
		if (!m->active) continue;
		m->elapsed++;
		if (m->duration && m->elapsed >= m->duration) m->active = 0;
	}
}

/* ---- recording pool, state ----------------------------------------------------------------------------------------------------------- */
int cine_play_begin(cine_state *s, const cinedef_section *sec, uint32_t rec, uint32_t owner, unsigned flags)
{
	int i, slot = cine_play_find(s, owner);
	if (slot >= 0) cine_play_kill(s, slot);
	for (i = 0; i < CINE_MAX_PLAYS; i++)
		if (!s->plays[i].used) {
			if (!cine_play_start(&s->plays[i].play, sec, rec)) return -1;
			s->plays[i].used = 1;
			s->plays[i].flags = (uint8_t)flags;
			s->plays[i].owner = owner;
			return i;
		}
	return -1;
}

cine_play *cine_play_get(cine_state *s, int slot)
{
	if (slot < 0 || slot >= CINE_MAX_PLAYS || !s->plays[slot].used) return NULL;
	return &s->plays[slot].play;
}

int cine_play_find(const cine_state *s, uint32_t owner)
{
	int i;
	for (i = 0; i < CINE_MAX_PLAYS; i++)
		if (s->plays[i].used && s->plays[i].owner == owner) return i;
	return -1;
}

void cine_play_kill(cine_state *s, int slot)
{
	if (slot >= 0 && slot < CINE_MAX_PLAYS) memset(&s->plays[slot], 0, sizeof s->plays[slot]);
}

void cine_init(cine_state *s)
{
	int i;
	memset(s, 0, sizeof *s);
	cine_cam_init(&s->cam);
	for (i = 0; i < CINE_MSG_KINDS; i++) s->msg_visible[i] = 1;
}

void cine_tick(cine_state *s)
{
	int i;
	s->tick++;
	cine_cam_tick(&s->cam);
	screen_tick(&s->screen);
	titles_tick(s);
	msgs_tick(s);
	for (i = 0; i < CINE_MAX_PLAYS; i++) {
		cine_play_slot *p = &s->plays[i];
		if (!p->used) continue;
		if (p->play.done && (p->flags & CINE_PLAY_DELETE)) {
			cine_play_kill(s, i);
			continue;
		}
		cine_play_tick(&p->play);
	}
}
