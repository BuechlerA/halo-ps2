/* Host test of the cinematics runtime (sim/cine.c).
 *   test_cine <fixture.hp2> [<real.cin.hp2> <golden.txt>]...
 * Recorded animation playback (timing against the tag lengths, deterministic replay, seek == sequential play, change masks),
 * the camera track sampler (passes through the control points, continuity, pitch lookup, quaternion round trips), the cinematic
 * camera (transitions, easing, shortest yaw arc, relative cameras, field of view, control off), screen state (letterbox, fade,
 * skip), the title queue and the message queue. The fixture is the synthetic world of tools/tests/test_cinconv.py; the real packs
 * are played in full and every recording's per tick state hash must equal the one the Python reference player wrote. */
#include "sim/cine.h"
#include "stream/hp2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static int load_section(const char *path, uint8_t **raw, size_t *n)
{
	FILE *f = fopen(path, "rb");
	size_t fn;
	uint8_t *img;
	hp2_pack pk;
	const hp2_section *sec;
	CHECK(f);
	fseek(f, 0, SEEK_END);
	fn = (size_t)ftell(f);
	rewind(f);
	img = malloc(fn);
	CHECK(img && fread(img, 1, fn, f) == fn);
	fclose(f);
	CHECK(hp2_open(&pk, img, fn) == HP2_OK);
	sec = hp2_find(&pk, "CINE");
	CHECK(sec);
	*raw = malloc(sec->raw_size);
	CHECK(*raw && hp2_load(&pk, sec, *raw, sec->raw_size) == HP2_OK);
	*n = sec->raw_size;
	free(img);
	return 0;
}

static uint32_t fnv_byte(uint32_t h, uint8_t b) { return (h ^ b) * 0x01000193u; }
static uint32_t fnv_u16(uint32_t h, uint16_t v) { return fnv_byte(fnv_byte(h, (uint8_t)v), (uint8_t)(v >> 8)); }
static uint32_t fnv_f32(uint32_t h, float f)
{
	uint32_t u;
	int i;
	memcpy(&u, &f, 4);
	for (i = 0; i < 4; i++) h = fnv_byte(h, (uint8_t)(u >> (8 * i)));
	return h;
}
static uint32_t hash_state(uint32_t h, const cinedef_ctl *c)
{
	h = fnv_u16(h, c->flags);
	h = fnv_u16(h, (uint16_t)c->weapon);
	h = fnv_byte(h, c->mode);
	h = fnv_u16(h, (uint16_t)c->yaw);
	h = fnv_u16(h, (uint16_t)c->pitch);
	h = fnv_f32(h, c->throttle[0]);
	return fnv_f32(h, c->throttle[1]);
}

static int ctl_equal(const cinedef_ctl *a, const cinedef_ctl *b) { return memcmp(a, b, sizeof *a) == 0; }

/* ---- 1. playback ---------------------------------------------------------------------------------------------------- */
static int check_playback(const cinedef_section *s)
{
	cine_play p, q;
	uint32_t t, rec;
	CHECK(!cine_play_start(&p, s, 4) && p.sec == NULL);
	CHECK(cine_play_start(&p, s, 0));
	CHECK(p.length == 326 && p.tick == 0 && !p.done && p.changed == CINE_CH_ALL);
	CHECK(p.ctl.throttle[0] == 1.0f && p.ctl.yaw == 100 && p.ctl.flags == 0x0300 && p.ctl.weapon == -1);
	cine_play_tick(&p);                                          /* tick 1: aim and flags */
	CHECK(p.tick == 1 && p.ctl.yaw == 103 && p.ctl.pitch == -2 && p.ctl.flags == 0x0800 && p.changed == (CINE_CH_AIM | CINE_CH_FLAGS));
	for (t = 2; t <= 4; t++) {
		cine_play_tick(&p);
		CHECK(p.tick == t && p.changed == 0 && p.ctl.yaw == 103);
	}
	cine_play_tick(&p);                                          /* tick 5: the large aim change wraps */
	CHECK(p.ctl.yaw == 53 && p.ctl.pitch == 3 && p.changed == CINE_CH_AIM);
	for (t = 6; t <= 304; t++) cine_play_tick(&p);
	CHECK(p.tick == 304 && p.ctl.weapon == -1 && !p.done);
	cine_play_tick(&p);
	CHECK(p.tick == 305 && p.ctl.weapon == 1 && p.changed == CINE_CH_WEAPON);
	for (t = 306; t <= 325; t++) cine_play_tick(&p);
	CHECK(p.tick == 325 && !p.done && p.ctl.weapon == 1);
	cine_play_tick(&p);                                          /* ran out: done, the state holds */
	CHECK(p.tick == 326 && p.done && p.ctl.weapon == 1 && p.ctl.yaw == 53 && p.changed == 0);
	cine_play_tick(&p);
	CHECK(p.tick == 326 && p.done);
	/* aim helpers */
	{
		float v[3];
		cinedef_ctl c = p.ctl;
		cine_ctl_aim_vector(&c, v);
		CHECK(near(v[0] * v[0] + v[1] * v[1] + v[2] * v[2], 1.0f, 1e-5f));
		CHECK(near(cine_ctl_yaw(&c), 53 * 0.00314159265f, 1e-6f) && near(cine_ctl_pitch(&c), 3 * 0.00314159265f, 1e-6f));
		c.yaw = -500;
		c.pitch = 0;
		cine_ctl_aim_vector(&c, v);
		CHECK(near(v[0], 0.0f, 1e-5f) && near(v[1], -1.0f, 1e-5f) && near(v[2], 0.0f, 1e-6f));
	}
	/* second recording: wrap and clamp, 11 ticks */
	CHECK(cine_play_start(&p, s, 1));
	CHECK(p.length == 11 && p.ctl.yaw == 999 && p.ctl.pitch == -500);
	for (t = 1; t < 11; t++) cine_play_tick(&p);
	CHECK(!p.done && p.tick == 10);
	cine_play_tick(&p);
	CHECK(p.done);
	/* third: version 0 header; the mode and throttle events (the throttle one waits 2 ticks AFTER applying) are both tick 0 */
	CHECK(cine_play_start(&p, s, 2));
	CHECK(p.length == 3 && p.ctl.mode == 3 && p.ctl.flags == 0x12 && p.ctl.throttle[0] == 0.5f && p.ctl.throttle[1] == -0.5f &&
	      (p.changed & CINE_CH_MODE) && (p.changed & CINE_CH_THROTTLE));
	cine_play_tick(&p);
	CHECK(p.ctl.throttle[0] == 0.5f && p.changed == 0 && !p.done);
	cine_play_tick(&p);
	CHECK(p.changed == 0 && !p.done && p.tick == 2);
	cine_play_tick(&p);
	CHECK(p.done && p.tick == 3);

	/* determinism and seek == sequential play, for every recording */
	for (rec = 0; rec < s->rec_count; rec++) {
		uint32_t len = cinedef_get_rec(s, rec).length, k;
		CHECK(cine_play_start(&p, s, rec) && cine_play_start(&q, s, rec));
		for (t = 0; t < len; t++) {
			CHECK(p.tick == t && ctl_equal(&p.ctl, &q.ctl));
			if (t % 37 == 0 || t == len - 1) {
				cine_play seek;
				CHECK(cine_play_start(&seek, s, rec));
				cine_play_seek(&seek, t);
				CHECK(seek.tick == t && ctl_equal(&seek.ctl, &p.ctl) && !seek.done);
			}
			cine_play_tick(&p);
			cine_play_tick(&q);
		}
		CHECK(p.done && q.done && p.tick == len);
		cine_play_seek(&p, len + 100);                            /* clamps to the end */
		CHECK(p.done && p.tick == len);
		cine_play_seek(&p, 0);
		CHECK(!p.done && p.tick == 0 && p.changed == CINE_CH_ALL);
		(void)k;
	}
	return 0;
}

/* ---- 2. tracks and quaternions ------------------------------------------------------------------------------------------- */
static int check_tracks(const cinedef_section *s)
{
	cine_track_pose tp;
	cinedef_point pt;
	uint32_t k;
	int i;
	float prev[3] = { 0, 0, 0 }, maxstep = 0.0f;
	CHECK(cine_track_sample(s, 0, 0.0f, &tp));
	pt = cinedef_get_point(s, 0, 0);
	CHECK(near(tp.pos[0], pt.pos[0], 1e-6f) && near(tp.pos[2], pt.pos[2], 1e-6f) && near(tp.quat[1], pt.quat[1], 1e-6f));
	CHECK(cine_track_sample(s, 0, 1.0f, &tp));
	pt = cinedef_get_point(s, 0, 8);
	CHECK(near(tp.pos[0], pt.pos[0], 1e-5f) && near(tp.pos[2], pt.pos[2], 1e-5f) && near(tp.quat[1], pt.quat[1], 1e-5f));
	for (k = 0; k < 9; k++) {                                     /* through every control point */
		CHECK(cine_track_sample(s, 0, (float)k / 8.0f, &tp));
		pt = cinedef_get_point(s, 0, k);
		CHECK(near(tp.pos[0], pt.pos[0], 1e-4f) && near(tp.pos[2], pt.pos[2], 1e-4f) && near(tp.quat[1], pt.quat[1], 1e-4f) &&
		      near(tp.quat[3], pt.quat[3], 1e-4f));
	}
	for (i = 0; i <= 2000; i++) {                                 /* continuity, unit quaternions */
		float t = (float)i / 2000.0f, len;
		CHECK(cine_track_sample(s, 0, t, &tp));
		len = tp.quat[0] * tp.quat[0] + tp.quat[1] * tp.quat[1] + tp.quat[2] * tp.quat[2] + tp.quat[3] * tp.quat[3];
		CHECK(near(len, 1.0f, 1e-4f));
		if (i) {
			float d = sqrtf((tp.pos[0] - prev[0]) * (tp.pos[0] - prev[0]) + (tp.pos[2] - prev[2]) * (tp.pos[2] - prev[2]));
			if (d > maxstep) maxstep = d;
		}
		memcpy(prev, tp.pos, sizeof prev);
	}
	CHECK(maxstep < 0.01f);                                       /* the arc of radius 2 is 6.3 long: 0.003 per step */
	/* the midpoint of a segment lies near the circle of the control points (radius 2 around (0, 0, 0.5)) */
	CHECK(cine_track_sample(s, 0, 0.5f / 8.0f, &tp));
	CHECK(near(sqrtf(tp.pos[0] * tp.pos[0] + (tp.pos[2] - 0.5f) * (tp.pos[2] - 0.5f)), 2.0f, 0.02f));
	/* out of range parameters clamp, bad tracks are refused */
	CHECK(cine_track_sample(s, 0, -3.0f, &tp) && near(tp.pos[2], 2.5f, 1e-4f));
	CHECK(cine_track_sample(s, 0, 7.0f, &tp) && near(tp.pos[2], -1.5f, 1e-4f));
	CHECK(cine_track_sample(s, 0, NAN, &tp) && near(tp.pos[2], 2.5f, 1e-4f));
	CHECK(!cine_track_sample(s, 2, 0.5f, &tp) && !cine_track_sample(s, 0xFFFFFFFFu, 0.5f, &tp) && !cine_track_sample(NULL, 0, 0.5f, &tp));

	/* look pitch: control point k of the first track looks -90 + 22.5 k degrees */
	for (k = 0; k < 9; k++) CHECK(near(cine_track_point_pitch(s, 0, k), (-90.0f + 22.5f * (float)k) * 0.0174532925f, 2e-3f));
	for (i = -85; i <= 85; i += 5) {
		float yaw, pitch, roll, want = (float)i * 0.0174532925f;
		CHECK(cine_track_sample_pitch(s, 0, want, &tp));
		cine_quat_to_ypr(tp.quat, &yaw, &pitch, &roll);
		CHECK(near(pitch, want, 0.01f) && near(yaw, 0.0f, 1e-3f) && near(roll, 0.0f, 1e-3f));
	}
	CHECK(cine_track_sample_pitch(s, 0, -2.0f, &tp) && near(tp.pos[2], 2.5f, 1e-4f));      /* below the first point: clamped */
	CHECK(cine_track_sample_pitch(s, 0, 2.0f, &tp) && near(tp.pos[2], -1.5f, 1e-4f));
	/* the second track: three points at -90, -20 and +80 degrees, one position; pitch lookup does not assume even spacing */
	CHECK(cine_track_sample_pitch(s, 1, -20.0f * 0.0174532925f, &tp));
	pt = cinedef_get_point(s, 1, 1);
	CHECK(near(tp.quat[1], pt.quat[1], 1e-4f) && near(tp.pos[0], 0.34f, 1e-5f) && near(tp.pos[2], 0.16f, 1e-5f));
	for (i = -85; i <= 75; i += 10) {
		float yaw, pitch, roll, want = (float)i * 0.0174532925f;
		CHECK(cine_track_sample_pitch(s, 1, want, &tp));
		cine_quat_to_ypr(tp.quat, &yaw, &pitch, &roll);
		CHECK(near(pitch, want, 0.05f));                               /* nlerp over a 70 / 100 degree segment: within 3 degrees */
		CHECK(near(tp.pos[0], 0.34f, 1e-4f));
	}
	/* round trips of yaw / pitch / roll through quaternions */
	for (i = -6; i <= 6; i++) {
		int j, l;
		for (j = -7; j <= 7; j++)
			for (l = -4; l <= 4; l++) {
				float y = (float)i * 0.5f, p = (float)j * 0.2f, r = (float)l * 0.25f, q[4], y2, p2, r2;
				float yw = y > 3.14159f ? y - 6.28318f : y < -3.14159f ? y + 6.28318f : y;
				cine_ypr_to_quat(y, p, r, q);
				CHECK(near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f, 1e-5f));
				cine_quat_to_ypr(q, &y2, &p2, &r2);
				CHECK(near(p2, p, 2e-4f) && near(r2, r, 5e-4f));
				CHECK(near(cosf(y2), cosf(yw), 2e-4f) && near(sinf(y2), sinf(yw), 2e-4f));
			}
	}
	{                                                              /* conventions: positive pitch looks up, positive roll banks right */
		float q[4], y, p, r;
		cine_ypr_to_quat(0.0f, 0.5f, 0.0f, q);
		cine_quat_to_ypr(q, &y, &p, &r);
		CHECK(p > 0.49f);
		cine_ypr_to_quat(1.0f, 0.0f, 0.0f, q);                     /* forward = (cos 1, sin 1, 0) */
		CHECK(near(1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]), cosf(1.0f), 1e-5f) && near(2.0f * (q[0] * q[1] + q[3] * q[2]), sinf(1.0f), 1e-5f));
	}
	/* a track pose in the world: origin + yaw, the aim yaw rotates the offset */
	{
		cine_pose w;
		float origin[3] = { 10.0f, 20.0f, 1.0f };
		CHECK(cine_track_sample(s, 0, 0.5f, &tp));                 /* the middle point: level, two units behind */
		cine_track_world(&tp, origin, 0.0f, 1.0f, &w);
		CHECK(near(w.pos[0], 8.0f, 1e-3f) && near(w.pos[1], 20.0f, 1e-3f) && near(w.pos[2], 1.5f, 1e-3f) && near(w.pitch, 0.0f, 1e-3f) && w.fov == 1.0f);
		cine_track_world(&tp, origin, 1.5707963f, 1.0f, &w);       /* facing +y: behind is -y */
		CHECK(near(w.pos[0], 10.0f, 1e-3f) && near(w.pos[1], 18.0f, 1e-3f) && near(w.yaw, 1.5707963f, 1e-3f));
	}
	return 0;
}

/* ---- 3. camera ------------------------------------------------------------------------------------------------------------- */
static int check_camera(void)
{
	cine_camera c;
	cine_pose p0 = { { 1, 2, 3 }, 0.5f, 0.1f, 0.0f, 1.2f }, t = { { 11, 2, 3 }, 0.5f, 0.1f, 0.0f, 1.2f }, now;
	uint32_t i;
	camera_t cam;
	cine_frame fr = { { 100, 0, 0 }, 1.5707963f };
	cine_pose rel = { { 10, 0, 0 }, 0.0f, 0.0f, 0.0f, 1.0f };
	cine_cam_init(&c);
	CHECK(!c.control && c.mode == CINE_CAM_PLAYER && near(cine_cam_pose(&c).fov, CINE_DEFAULT_FOV, 1e-6f));
	cine_cam_set_player_pose(&c, &p0);
	now = cine_cam_pose(&c);
	CHECK(now.pos[0] == 1.0f && cine_cam_time(&c) == 0);
	cine_cam_control(&c, 1);
	CHECK(c.control && c.mode == CINE_CAM_POSE && cine_cam_pose(&c).pos[0] == 1.0f);
	/* linear transition of 30 ticks */
	cine_cam_set(&c, &t, 30, CINE_EASE_LINEAR);
	CHECK(cine_cam_time(&c) == 30 && cine_cam_pose(&c).pos[0] == 1.0f);
	for (i = 1; i <= 30; i++) {
		cine_cam_tick(&c);
		now = cine_cam_pose(&c);
		CHECK(near(now.pos[0], 1.0f + 10.0f * (float)i / 30.0f, 1e-4f) && cine_cam_time(&c) == 30 - i);
	}
	CHECK(now.pos[0] == 11.0f && near(now.yaw, 0.5f, 1e-6f));
	cine_cam_tick(&c);
	CHECK(cine_cam_pose(&c).pos[0] == 11.0f && cine_cam_time(&c) == 0);                      /* arrived: holds */
	/* smooth: slower at the ends, equal at the middle, monotone */
	cine_cam_set(&c, &p0, 30, CINE_EASE_SMOOTH);
	{
		float last = c.cur.pos[0], x7 = 0, x15 = 0;
		for (i = 1; i <= 30; i++) {
			cine_cam_tick(&c);
			CHECK(cine_cam_pose(&c).pos[0] <= last + 1e-6f);
			last = cine_cam_pose(&c).pos[0];
			if (i == 7) x7 = last;
			if (i == 15) x15 = last;
		}
		CHECK(near(x15, 6.0f, 1e-4f) && x7 > 11.0f - 10.0f * (7.0f / 30.0f));      /* started slower than linear */
		CHECK(last == 1.0f);
	}
	/* a cut */
	cine_cam_set(&c, &t, 0, CINE_EASE_LINEAR);
	CHECK(cine_cam_pose(&c).pos[0] == 11.0f && cine_cam_time(&c) == 0);
	/* a transition starts from where the camera is, mid transition too */
	cine_cam_set(&c, &p0, 20, CINE_EASE_LINEAR);
	for (i = 0; i < 10; i++) cine_cam_tick(&c);
	cine_cam_set(&c, &t, 10, CINE_EASE_LINEAR);
	CHECK(near(cine_cam_pose(&c).pos[0], 6.0f, 1e-4f));
	/* the shortest way round: yaw 3.0 to -3.0 passes through pi, never through 0 */
	{
		cine_pose a = p0, b = p0;
		a.yaw = 3.0f;
		b.yaw = -3.0f;
		cine_cam_set(&c, &a, 0, CINE_EASE_LINEAR);
		cine_cam_set(&c, &b, 10, CINE_EASE_LINEAR);
		for (i = 1; i <= 10; i++) {
			cine_cam_tick(&c);
			now = cine_cam_pose(&c);
			CHECK(cosf(now.yaw) < -0.98f);                              /* stays near the back */
		}
		CHECK(near(cosf(now.yaw), cosf(-3.0f), 1e-4f) && near(sinf(now.yaw), sinf(-3.0f), 1e-4f));
	}
	/* a relative camera follows its anchor */
	cine_cam_control(&c, 0);
	CHECK(!c.control && c.mode == CINE_CAM_PLAYER && cine_cam_pose(&c).pos[0] == 1.0f);       /* the player pose again */
	cine_cam_set_relative(&c, &rel, 0, CINE_EASE_LINEAR, &fr);
	now = cine_cam_pose(&c);
	CHECK(near(now.pos[0], 100.0f, 1e-3f) && near(now.pos[1], 10.0f, 1e-3f) && near(now.yaw, 1.5707963f, 1e-5f));
	fr.pos[0] = 200.0f;
	cine_cam_set_anchor(&c, &fr);
	CHECK(near(cine_cam_pose(&c).pos[0], 200.0f, 1e-3f) && near(cine_cam_pose(&c).pos[1], 10.0f, 1e-3f));
	cine_cam_tick(&c);
	CHECK(near(cine_cam_pose(&c).pos[0], 200.0f, 1e-3f));
	{                                                              /* compose / relative are inverses */
		cine_pose w, r2;
		cine_frame f2 = { { -7, 3, 2 }, -2.0f };
		cine_pose r1 = { { 1, -2, 3 }, 0.3f, 0.2f, 0.1f, 1.0f };
		cine_pose_compose(&r1, &f2, &w);
		cine_pose_relative(&w, &f2, &r2);
		CHECK(near(r2.pos[0], 1.0f, 1e-4f) && near(r2.pos[1], -2.0f, 1e-4f) && near(r2.pos[2], 3.0f, 1e-4f) && near(r2.yaw, 0.3f, 1e-4f));
		CHECK(near(w.pitch, 0.2f, 1e-6f) && near(w.roll, 0.1f, 1e-6f));
	}
	/* transition to a relative pose over time: ends at the anchor's frame */
	cine_cam_control(&c, 0);
	cine_cam_set_player_pose(&c, &p0);
	cine_cam_set_relative(&c, &rel, 10, CINE_EASE_LINEAR, &fr);
	for (i = 0; i < 10; i++) cine_cam_tick(&c);
	CHECK(near(cine_cam_pose(&c).pos[0], 200.0f, 1e-2f) && near(cine_cam_pose(&c).pos[1], 10.0f, 1e-2f));
	/* field of view */
	cine_cam_control(&c, 0);
	cine_cam_control(&c, 1);
	cine_cam_set_fov(&c, 0.6f, 10);
	for (i = 1; i <= 10; i++) {
		cine_cam_tick(&c);
		CHECK(near(cine_cam_pose(&c).fov, 1.2f + (0.6f - 1.2f) * (float)i / 10.0f, 1e-4f));
	}
	cine_cam_set(&c, &t, 0, CINE_EASE_LINEAR);                      /* the next camera_set takes the pose's own field of view */
	CHECK(near(cine_cam_pose(&c).fov, 1.2f, 1e-6f));
	cine_cam_set_fov(&c, -1.0f, 5);                                  /* nonsense is ignored */
	CHECK(!c.fov_active);
	cine_cam_set_near_clip(&c, 0.05f);
	CHECK(c.near_clip == 0.05f);
	cine_cam_set_near_clip(&c, -1.0f);
	CHECK(c.near_clip == 0.0f);
	/* the other modes */
	cine_cam_set_first_person(&c);
	CHECK(c.mode == CINE_CAM_FIRST_PERSON && cine_cam_pose(&c).pos[0] == p0.pos[0]);
	cine_cam_set_dead(&c);
	CHECK(c.mode == CINE_CAM_DEAD);
	cine_cam_tick(&c);
	/* a pose becomes a render camera: position, yaw, clamped pitch, field of view */
	memset(&cam, 0, sizeof cam);
	{
		cine_pose e = { { 5, 6, 7 }, 1.0f, 2.0f, 0.4f, 0.9f };
		cine_pose_to_camera(&e, &cam);
		CHECK(cam.pos[0] == 5.0f && cam.pos[2] == 7.0f && cam.yaw == 1.0f && cam.pitch < 1.56f && cam.pitch > 1.5f && cam.hfov == 0.9f);
	}
	return 0;
}

/* ---- 4. screen --------------------------------------------------------------------------------------------------------------- */
static int check_screen(void)
{
	cine_state s;
	uint32_t i;
	float last;
	cine_init(&s);
	CHECK(!s.screen.cinematic && s.screen.letterbox == 0.0f && s.screen.fade_alpha == 0.0f && cine_letterbox_bar(&s, 480) == 0);
	cine_start(&s);
	CHECK(s.screen.cinematic);
	cine_letterbox(&s, 1);
	last = 0.0f;
	for (i = 1; i <= CINE_LETTERBOX_TICKS; i++) {
		cine_tick(&s);
		CHECK(s.screen.letterbox > last && s.screen.letterbox <= 1.0f);
		last = s.screen.letterbox;
	}
	CHECK(s.screen.letterbox == 1.0f && cine_letterbox_bar(&s, 480) == 60 && cine_letterbox_bar(&s, 0) == 0);
	cine_tick(&s);
	CHECK(s.screen.letterbox == 1.0f);
	cine_letterbox(&s, 0);
	for (i = 1; i <= CINE_LETTERBOX_TICKS; i++) cine_tick(&s);
	CHECK(s.screen.letterbox == 1.0f);                               /* cinematic mode keeps the bars by itself */
	cine_stop(&s);
	for (i = 1; i <= CINE_LETTERBOX_TICKS; i++) cine_tick(&s);
	CHECK(s.screen.letterbox == 0.0f);
	cine_letterbox(&s, 1);
	for (i = 1; i <= 5; i++) cine_tick(&s);
	cine_letterbox(&s, 0);                                           /* reverses from where it is */
	cine_tick(&s);
	CHECK(s.screen.letterbox > 0.2f && s.screen.letterbox < 0.4f);
	/* fades */
	cine_fade_out(&s, 1.0f, 0.5f, 0.0f, 30);
	CHECK(s.screen.fade_alpha == 0.0f && s.screen.fade_rgb[0] == 1.0f && s.screen.fade_rgb[1] == 0.5f);
	for (i = 1; i <= 15; i++) cine_tick(&s);
	CHECK(near(s.screen.fade_alpha, 0.5f, 1e-5f));
	for (i = 16; i <= 30; i++) cine_tick(&s);
	CHECK(s.screen.fade_alpha == 1.0f);
	cine_tick(&s);
	CHECK(s.screen.fade_alpha == 1.0f);
	cine_fade_in(&s, 0.0f, 0.0f, 0.0f, 30);
	for (i = 1; i <= 15; i++) cine_tick(&s);
	CHECK(near(s.screen.fade_alpha, 0.5f, 1e-5f));
	cine_fade_out(&s, 0.0f, 0.0f, 0.0f, 10);                         /* reverses from the current alpha */
	for (i = 1; i <= 5; i++) cine_tick(&s);
	CHECK(near(s.screen.fade_alpha, 0.75f, 1e-5f));
	cine_fade_in(&s, 9.0f, -1.0f, 0.0f, 0);                          /* instant, colour clamped */
	CHECK(s.screen.fade_alpha == 0.0f && s.screen.fade_rgb[0] == 1.0f && s.screen.fade_rgb[1] == 0.0f);
	cine_fade_out(&s, 0.0f, 0.0f, 0.0f, 0);
	CHECK(s.screen.fade_alpha == 1.0f);
	/* skip */
	cine_skip_request(&s);
	CHECK(!cine_skip_pending(&s));                                   /* not inside a skippable section */
	cine_skip_begin(&s);
	CHECK(!cine_skip_pending(&s));
	cine_skip_request(&s);
	CHECK(cine_skip_pending(&s));
	cine_skip_end(&s);
	CHECK(!cine_skip_pending(&s) && !s.screen.skippable);
	cine_skip_begin(&s);
	cine_skip_request(&s);
	cine_stop(&s);
	CHECK(!s.screen.cinematic && !cine_skip_pending(&s));
	return 0;
}

/* ---- 5. titles and messages --------------------------------------------------------------------------------------------------- */
static int check_titles(const cinedef_section *sec)
{
	cine_state s;
	uint32_t i;
	int slot;
	cine_init(&s);
	CHECK(cine_title_show(&s, sec, 5, 0) == -1 && cine_title_show(&s, NULL, 0, 0) == -1);
	slot = cine_title_show(&s, sec, 0, 10);
	CHECK(slot == 0 && s.titles[0].active && s.titles[0].units == 6 && s.titles[0].text[0] == 'B' && s.titles[0].text[5] == 0xE9);
	CHECK(s.titles[0].def.fade_in == 30 && s.titles[0].def.up == 120 && s.titles[0].def.fade_out == 30);
	for (i = 1; i <= 10; i++) {
		CHECK(cine_title_alpha(&s.titles[0]) == 0.0f);               /* the delay */
		cine_tick(&s);
	}
	CHECK(cine_title_alpha(&s.titles[0]) == 0.0f && s.titles[0].elapsed == 0);
	for (i = 1; i <= 15; i++) cine_tick(&s);
	CHECK(near(cine_title_alpha(&s.titles[0]), 0.5f, 1e-5f));         /* half way up */
	for (i = 16; i <= 30; i++) cine_tick(&s);
	CHECK(cine_title_alpha(&s.titles[0]) == 1.0f);
	for (i = 31; i <= 150; i++) {
		cine_tick(&s);
		CHECK(cine_title_alpha(&s.titles[0]) == 1.0f && s.titles[0].active);
	}
	for (i = 151; i <= 165; i++) cine_tick(&s);
	CHECK(near(cine_title_alpha(&s.titles[0]), 0.5f, 1e-5f));
	for (i = 166; i <= 179; i++) cine_tick(&s);
	CHECK(s.titles[0].active && cine_title_alpha(&s.titles[0]) > 0.0f);
	cine_tick(&s);
	CHECK(!s.titles[0].active && cine_title_alpha(&s.titles[0]) == 0.0f);
	/* a title without a string, no fades: shows at once for `up` ticks */
	slot = cine_title_show(&s, sec, 1, 0);
	CHECK(slot == 0 && s.titles[0].units == 0 && cine_title_alpha(&s.titles[0]) == 1.0f);
	for (i = 1; i < 90; i++) cine_tick(&s);
	CHECK(s.titles[0].active);
	cine_tick(&s);
	CHECK(!s.titles[0].active);
	/* the queue: four slots, the fifth replaces the title that has been shown longest */
	{
		cinedef_title d = cinedef_get_title(sec, 0);
		uint16_t text[3] = { 'a', 'b', 'c' };
		cine_title_clear(&s);
		CHECK(cine_title_post(&s, &d, text, 3, 0) == 0);
		cine_tick(&s);
		CHECK(cine_title_post(&s, &d, text, 3, 0) == 1);
		cine_tick(&s);
		CHECK(cine_title_post(&s, &d, text, 3, 0) == 2 && cine_title_post(&s, &d, text, 3, 5) == 3);
		cine_tick(&s);
		CHECK(cine_title_post(&s, &d, text, 3, 0) == 0);              /* slot 0 was shown longest */
		CHECK(s.titles[0].elapsed == 0 && s.titles[1].elapsed == 2);
		/* text longer than the buffer is cut */
		{
			uint16_t big[CINE_TEXT_UNITS + 40];
			for (i = 0; i < CINE_TEXT_UNITS + 40; i++) big[i] = 'x';
			cine_title_clear(&s);
			CHECK(cine_title_post(&s, &d, big, CINE_TEXT_UNITS + 40, 0) == 0 && s.titles[0].units == CINE_TEXT_UNITS);
		}
	}
	return 0;
}

static int check_messages(void)
{
	cine_state s;
	uint32_t i;
	int a, b, c;
	const cine_msg *m;
	cine_init(&s);
	a = cine_msg_post_ascii(&s, CINE_MSG_HELP, "first", 0);
	b = cine_msg_post_ascii(&s, CINE_MSG_OBJECTIVE, "goal", 0);
	CHECK(a == 0 && b == 1 && (m = cine_msg_slot(&s, 0)) && m->units == 5 && m->text[0] == 'f' && m->kind == CINE_MSG_HELP);
	CHECK(cine_msg_post_ascii(&s, CINE_MSG_HELP, "second", 0) == 0);        /* replaces its predecessor in place */
	CHECK(cine_msg_slot(&s, 0)->units == 6 && cine_msg_slot(&s, 0)->elapsed == 0);
	CHECK(cine_msg_post_ascii(&s, CINE_MSG_OBJECTIVE, "goal 2", 0) == 1);
	c = cine_msg_post_ascii(&s, CINE_MSG_SCENARIO_HELP, "tip", 5);
	CHECK(c == 2);
	for (i = 1; i < 5; i++) cine_tick(&s);
	CHECK(cine_msg_slot(&s, 2) != NULL);
	cine_tick(&s);
	CHECK(cine_msg_slot(&s, 2) == NULL && cine_msg_slot(&s, 0) != NULL && cine_msg_slot(&s, 1) != NULL);   /* duration 0 never expires */
	/* visibility of a kind (show_hud_help_text) */
	cine_msg_set_visible(&s, CINE_MSG_HELP, 0);
	CHECK(cine_msg_slot(&s, 0) == NULL && cine_msg_slot(&s, 1) != NULL);
	cine_msg_set_visible(&s, CINE_MSG_HELP, 1);
	CHECK(cine_msg_slot(&s, 0) != NULL);
	cine_msg_set_visible(&s, 99, 1);
	CHECK(cine_msg_slot(&s, CINE_MSG_SLOTS) == NULL);
	cine_msg_clear(&s, CINE_MSG_HELP);
	CHECK(cine_msg_slot(&s, 0) == NULL && cine_msg_slot(&s, 1) != NULL);
	CHECK(cine_msg_post_ascii(&s, 77, "x", 0) == -1 && cine_msg_post_ascii(&s, -1, "x", 0) == -1);
	/* subtitles queue; a full queue drops the oldest */
	cine_init(&s);
	for (i = 0; i < CINE_MSG_SLOTS; i++) {
		CHECK(cine_msg_post_ascii(&s, CINE_MSG_SUBTITLE, "line", 100) == (int)i);
		cine_tick(&s);
	}
	CHECK(cine_msg_post_ascii(&s, CINE_MSG_SUBTITLE, "new", 100) == 0);   /* slot 0 is the oldest */
	CHECK(cine_msg_slot(&s, 0)->units == 3 && cine_msg_slot(&s, 0)->elapsed == 0 && cine_msg_slot(&s, 1)->elapsed == CINE_MSG_SLOTS - 1);
	/* cut at the buffer size, empty and NULL text */
	{
		char big[CINE_TEXT_UNITS + 50];
		memset(big, 'y', sizeof big);
		big[sizeof big - 1] = 0;
		cine_init(&s);
		CHECK(cine_msg_post_ascii(&s, CINE_MSG_HELP, big, 0) == 0 && s.msgs[0].units == CINE_TEXT_UNITS);
		CHECK(cine_msg_post_ascii(&s, CINE_MSG_SUBTITLE, "", 0) == 1 && s.msgs[1].units == 0);
		CHECK(cine_msg_post_ascii(&s, CINE_MSG_SUBTITLE, NULL, 0) == 2 && s.msgs[2].units == 0);
		CHECK(cine_msg_post(&s, CINE_MSG_SUBTITLE, NULL, 7, 0) == 3 && s.msgs[3].units == 0);
	}
	return 0;
}

/* ---- recording pool ----------------------------------------------------------------------------------------------------------- */
static int check_pool(const cinedef_section *sec)
{
	cine_state s;
	int a, b, i;
	cine_play *p;
	cine_init(&s);
	CHECK(cine_play_begin(&s, sec, 9, 1, 0) == -1);                      /* no such recording */
	a = cine_play_begin(&s, sec, 1, 7, CINE_PLAY_DELETE);                /* 11 ticks */
	b = cine_play_begin(&s, sec, 0, 8, 0);
	CHECK(a == 0 && b == 1 && cine_play_find(&s, 7) == 0 && cine_play_find(&s, 8) == 1 && cine_play_find(&s, 9) == -1);
	p = cine_play_get(&s, a);
	CHECK(p && p->length == 11 && cine_play_get(&s, 2) == NULL && cine_play_get(&s, -1) == NULL && cine_play_get(&s, CINE_MAX_PLAYS) == NULL);
	for (i = 1; i <= 10; i++) cine_tick(&s);
	CHECK(!cine_play_get(&s, a)->done && cine_play_get(&s, a)->tick == 10);
	cine_tick(&s);
	CHECK(cine_play_get(&s, a) && cine_play_get(&s, a)->done);           /* done: the host can read the final state */
	cine_tick(&s);
	CHECK(cine_play_get(&s, a) == NULL && cine_play_find(&s, 7) == -1);  /* then deleted */
	CHECK(cine_play_get(&s, b)->tick == 12);
	/* a unit has one recording: starting another replaces it (in the first free slot) */
	CHECK(cine_play_begin(&s, sec, 2, 8, 0) == 0 && cine_play_find(&s, 8) == 0 && cine_play_get(&s, 1) == NULL);
	CHECK(cine_play_get(&s, 0)->rec == 2 && cine_play_get(&s, 0)->tick == 0);
	for (i = 0; i < 100; i++) cine_tick(&s);
	CHECK(cine_play_get(&s, 0) && cine_play_get(&s, 0)->done);           /* without the delete flag it stays */
	cine_play_kill(&s, 0);
	CHECK(cine_play_get(&s, 0) == NULL);
	cine_play_kill(&s, -5);
	cine_play_kill(&s, 1000);
	/* a full pool refuses */
	for (i = 0; i < CINE_MAX_PLAYS; i++) CHECK(cine_play_begin(&s, sec, 0, 100 + (uint32_t)i, 0) == i);
	CHECK(cine_play_begin(&s, sec, 0, 500, 0) == -1);
	return 0;
}

/* ---- real packs ------------------------------------------------------------------------------------------------------------------ */
static int check_real(const char *pack, const char *golden)
{
	uint8_t *raw;
	size_t rawn;
	cinedef_section s;
	FILE *g;
	char line[128];
	unsigned idx, len, want;
	uint32_t total = 0, n = 0, longest = 0;
	CHECK(load_section(pack, &raw, &rawn) == 0);
	CHECK(cinedef_open(&s, raw, rawn) == CINEDEF_OK);
	g = fopen(golden, "r");
	CHECK(g);
	while (fgets(line, sizeof line, g)) {
		cine_play p;
		uint32_t h = 0x811C9DC5u, t, ticks = 0;
		CHECK(sscanf(line, "%u %u %x", &idx, &len, &want) == 3 && idx == n && idx < s.rec_count);
		CHECK(cine_play_start(&p, &s, idx) && p.length == len);
		for (t = 0; t < len; t++) {
			CHECK(!p.done && p.tick == t);
			h = hash_state(h, &p.ctl);
			cine_play_tick(&p);
			ticks++;
		}
		CHECK(p.done && p.tick == len && ticks == len);                       /* timing against the tag length */
		CHECK(h == want);
		total += len;
		if (len > longest) longest = len;
		n++;
	}
	fclose(g);
	CHECK(n == s.rec_count);
	{                                                                   /* camera tracks of the real maps: usable by both samplers */
		uint32_t i;
		for (i = 0; i < s.track_count; i++) {
			cine_track_pose tp;
			uint32_t k;
			float prev = -1e30f;
			CHECK(cine_track_sample(&s, i, 0.37f, &tp) && cine_track_sample_pitch(&s, i, 0.2f, &tp));
			for (k = 0; k < cinedef_get_track(&s, i).point_count; k++) {
				float pt = cine_track_point_pitch(&s, i, k);
				CHECK(pt >= prev - 1e-4f);                                      /* look pitch never decreases along a track */
				prev = pt;
			}
			CHECK(near(cine_track_point_pitch(&s, i, 0), -1.5708f, 2e-3f));    /* the first point looks straight down */
		}
	}
	printf("%s: %u recordings, %u ticks (longest %u) played and hashed, %u tracks\n", pack, n, total, longest, s.track_count);
	free(raw);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t *raw;
	size_t rawn;
	cinedef_section s;
	int a;
	CHECK(argc >= 2);
	CHECK(load_section(argv[1], &raw, &rawn) == 0);
	CHECK(cinedef_open(&s, raw, rawn) == CINEDEF_OK);
	if (check_playback(&s)) return 1;
	if (check_tracks(&s)) return 1;
	if (check_camera()) return 1;
	if (check_screen()) return 1;
	if (check_titles(&s)) return 1;
	if (check_messages()) return 1;
	if (check_pool(&s)) return 1;
	free(raw);
	for (a = 2; a + 1 < argc; a += 2)
		if (check_real(argv[a], argv[a + 1])) return 1;
	printf("ok\n");
	return 0;
}
