/* Host test of the H2 script host (script/hs_host_misc.c, docs/34-host-misc.md) against the real a10 packs: every built-in of the camera /
 * cinematic, sound, hud / ui, navigation, game flow, player effect and debug domains is called with real arguments and its effect checked on
 * a mission_t built from the a10 data (test_host_misc_fixture.h), every call site of the a10 script runs once, a fuzz run with hostile
 * arguments on a full and on an empty mission must never crash, and the soak runs the a10 script machine for 9000 ticks with the player idle.
 *   test_host_misc ASSET_DIR CINE_PACK       (skips with a message when the packs are missing) */
#include "audio/stream.h"
#include "script/hs_host_misc.h"
#include "world/mouth.h"
#include "test_host_misc_fixture.h"
#ifdef HM_WITH_OBJ
#include "script/hs_host_obj.h"
static void register_both(hs_machine *hm) { hs_host_register_objects(hm); hs_host_register_misc(hm); }
#define HM_REGISTER register_both
#else
#define HM_REGISTER hs_host_register_misc
#endif
#include <math.h>
#include <time.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b, e) (fabsf((float)(a) - (float)(b)) <= (e))

static mfx_t X;
static stream_player g_sp;

/* ---- helpers ------------------------------------------------------------------------------------------------------------------------- */
static hs_value hv(uint32_t h, int vt) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_HANDLE; v.vtype = (uint8_t)vt; v.v.h = h; return v; }
static hs_value rv(float f) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_REAL; v.vtype = 6; v.v.f = f; return v; }
static hs_value sv(int i) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_SHORT; v.vtype = 7; v.v.i = i; return v; }
static hs_value bv(int b) { hs_value v; memset(&v, 0, sizeof v); v.type = HS_BOOL; v.vtype = 5; v.v.i = b != 0; return v; }

static hs_builtin find_fn(const hs_machine *hm, const char *name)
{
	unsigned i;
	for (i = 0; i < hm->nreg; i++) if (strcmp(hm->reg[i].name, name) == 0) return hm->reg[i].fn;
	return NULL;
}

static hs_value call(const char *name, const hs_value *a, unsigned n, unsigned rtype)
{
	hs_value r = hs_neutral(rtype, 0);
	hs_builtin fn = find_fn(&X.hs, name);
	if (!fn) { printf("FAIL: %s not registered\n", name); failures++; return r; }
	if (fn(&X.hs, &X.m, a, n, &r) != 0) { printf("FAIL: %s blocked\n", name); failures++; }
	return r;
}
#define CALL0(name, rt) call(name, NULL, 0, rt)
#define CALL1(name, rt, a0) do { hs_value _a[1]; _a[0] = (a0); (void)call(name, _a, 1, rt); } while (0)

static void step(unsigned n)
{
	while (n--) { X.m.tick++; misc_tick(&X.m); cine_tick(&X.cine); }
}

static void fresh(void) { mfx_start(&X, HM_REGISTER, 1); hs_host_misc_reset_stats(); }

static const cine_msg *find_msg(int kind)
{
	unsigned i;
	for (i = 0; i < CINE_MSG_SLOTS; i++) if (X.cine.msgs[i].active && X.cine.msgs[i].kind == kind) return &X.cine.msgs[i];
	return NULL;
}

/* ---- file backed stream_io (the real pack) ------------------------------------------------------------------------------------------ */
typedef struct { FILE *f; int busy; void *dst; uint32_t off, bytes; } fio_t;
static int io_start(void *u, uint32_t off, void *dst, uint32_t bytes)
{
	fio_t *i = u;
	if (i->busy) return -1;
	i->busy = 1; i->dst = dst; i->off = off; i->bytes = bytes;
	return 0;
}
static int io_poll(void *u)
{
	fio_t *i = u;
	if (!i->busy) return -1;
	i->busy = 0;
	if (fseek(i->f, (long)i->off, SEEK_SET) != 0 || fread(i->dst, 1, i->bytes, i->f) != i->bytes) return -1;
	return 1;
}

/* hooks */
typedef struct { unsigned plays, loop_starts, loop_stops, with_pos; uint32_t last_tag; float last_gain; } hooks_t;
static hooks_t g_hooks;
static void hk_play(void *u, uint32_t tag, const float *pos, float gain) { hooks_t *h = u; h->plays++; h->last_tag = tag; h->last_gain = gain; if (pos) h->with_pos++; }
static void hk_loop_start(void *u, uint32_t tag, const float *pos, float gain) { hooks_t *h = u; h->loop_starts++; h->last_tag = tag; (void)pos; (void)gain; }
static void hk_loop_stop(void *u, uint32_t tag) { hooks_t *h = u; h->loop_stops++; h->last_tag = tag; }

static int first_object(void)
{
	int i;
	for (i = 0; i < (int)X.objects.count; i++) if (mission_object_live(&X.m, i) && i != X.m.player_obj) return i;
	return -1;
}

/* ---- 1. camera and cinematic ----------------------------------------------------------------------------------------------------------- */
static void test_camera(void)
{
	hs_value a[4], r;
	devdef_camera c;
	unsigned k;
	int o;
	fresh();
	CHECK(X.dev.camera_count > 4 && X.dev.flag_count > 4);
	c = devdef_get_camera(&X.dev, 3);
	a[0] = hv(3, 13); a[1] = sv(100);                 /* camera_set lasts 0.9 of its ticks unless measured otherwise (hs_host_misc.c camera_ticks) */
	call("camera_set", a, 2, HS_VOID);
	CHECK(X.cine.cam.control && X.cine.cam.mode == CINE_CAM_POSE);
	r = CALL0("camera_time", HS_SHORT); CHECK(r.v.i == 90);
	step(45);
	r = CALL0("camera_time", HS_SHORT); CHECK(r.v.i == 45);
	r = CALL0("camera_time", HS_REAL); CHECK(NEAR(r.v.f, 45.0f, 0.01f));
	step(45);
	r = CALL0("camera_time", HS_SHORT); CHECK(r.v.i == 0);
	{
		cine_pose p = cine_cam_pose(&X.cine.cam);
		CHECK(NEAR(p.pos[0], c.pos[0], 1e-3f) && NEAR(p.pos[1], c.pos[1], 1e-3f) && NEAR(p.pos[2], c.pos[2], 1e-3f));
		float ey, ep, er;
		cine_halo_euler_to_ypr(c.ori[0], c.ori[1], c.ori[2], &ey, &ep, &er);       /* camera points use Halo's euler convention */
		CHECK(NEAR(p.yaw, ey, 1e-3f) && NEAR(p.pitch, ep, 1e-3f) && NEAR(p.roll, er, 1e-3f));
	}
	/* a cut: zero ticks arrives at once; an unknown point is a no-op */
	a[0] = hv(5, 13); a[1] = sv(0);
	call("camera_set", a, 2, HS_VOID);
	r = CALL0("camera_time", HS_SHORT); CHECK(r.v.i == 0);
	{
		cine_pose before = cine_cam_pose(&X.cine.cam), after;
		a[0] = hv(99999, 13); a[1] = sv(10);
		call("camera_set", a, 2, HS_VOID);
		a[0] = hv(HS_HANDLE_NONE, 13);
		call("camera_set", a, 2, HS_VOID);
		after = cine_cam_pose(&X.cine.cam);
		CHECK(memcmp(&before, &after, sizeof before) == 0);
		CHECK(CALL0("camera_time", HS_SHORT).v.i == 0);
	}
	/* camera_control off hands the camera back */
	CALL1("camera_control", HS_VOID, bv(0));
	CHECK(!X.cine.cam.control && X.cine.cam.mode == CINE_CAM_PLAYER);
	CALL1("camera_control", HS_VOID, bv(1));
	CHECK(X.cine.cam.control);
	/* relative to an object */
	o = first_object();
	CHECK(o >= 0);
	a[0] = hv(2, 13); a[1] = sv(20); a[2] = hv(mission_obj_handle(o), 37);
	call("camera_set_relative", a, 3, HS_VOID);
	CHECK(X.cine.cam.relative && X.m.misc.cam_anchor_obj1 == o + 1);
	step(20);
	{
		cine_pose rel, w = cine_cam_pose(&X.cine.cam);
		cine_frame f;
		devdef_camera c2 = devdef_get_camera(&X.dev, 2);
		f.pos[0] = X.objects.obj[o].pos[0]; f.pos[1] = X.objects.obj[o].pos[1]; f.pos[2] = X.objects.obj[o].pos[2];
		f.yaw = atan2f(X.objects.obj[o].root.m[0][1], X.objects.obj[o].root.m[0][0]);
		rel.pos[0] = c2.pos[0]; rel.pos[1] = c2.pos[1]; rel.pos[2] = c2.pos[2]; rel.yaw = c2.ori[0]; rel.pitch = c2.ori[1]; rel.roll = c2.ori[2]; rel.fov = c2.fov;
		cine_pose_compose(&rel, &f, &rel);
		CHECK(NEAR(w.pos[0], rel.pos[0], 1e-2f) && NEAR(w.pos[1], rel.pos[1], 1e-2f) && NEAR(w.pos[2], rel.pos[2], 1e-2f));
	}
	/* first person / dead camera */
	CALL1("camera_set_first_person", HS_VOID, hv(mission_obj_handle(o), 38));
	CHECK(X.cine.cam.mode == CINE_CAM_FIRST_PERSON);
	CALL1("camera_set_dead", HS_VOID, hv(mission_obj_handle(o), 38));
	CHECK(X.cine.cam.mode == CINE_CAM_DEAD);
	CALL1("cinematic_set_near_clip_distance", HS_VOID, rv(0.25f));
	CHECK(NEAR(X.cine.cam.near_clip, 0.25f, 1e-6f));

	/* cinematic start / stop, letterbox, skip section */
	call("cinematic_start", NULL, 0, HS_VOID);
	CHECK(X.m.cinematic == 1 && X.cine.screen.cinematic);
	CALL1("cinematic_show_letterbox", HS_VOID, bv(1));
	step(CINE_LETTERBOX_TICKS + 2);
	CHECK(NEAR(X.cine.screen.letterbox, 1.0f, 1e-4f));
	call("cinematic_skip_start_internal", NULL, 0, HS_VOID);
	CHECK(X.cine.screen.skippable);
	cine_skip_request(&X.cine);
	CHECK(cine_skip_pending(&X.cine));
	call("cinematic_skip_stop_internal", NULL, 0, HS_VOID);
	CHECK(!X.cine.screen.skippable);
	CALL1("cinematic_show_letterbox", HS_VOID, bv(0));
	step(CINE_LETTERBOX_TICKS + 2);
	CHECK(NEAR(X.cine.screen.letterbox, 1.0f, 1e-4f));          /* cinematic mode keeps the bars by itself */
	call("cinematic_stop", NULL, 0, HS_VOID);
	CHECK(X.m.cinematic == 0 && !X.cine.screen.cinematic);
	step(CINE_LETTERBOX_TICKS + 2);
	CHECK(NEAR(X.cine.screen.letterbox, 0.0f, 1e-4f));

	/* fades: colour and time are the arguments */
	a[0] = rv(1.0f); a[1] = rv(0.0f); a[2] = rv(0.5f); a[3] = sv(30);
	call("fade_out", a, 4, HS_VOID);
	step(15);
	CHECK(X.cine.screen.fade_alpha > 0.3f && X.cine.screen.fade_alpha < 0.7f);
	step(15);
	CHECK(NEAR(X.cine.screen.fade_alpha, 1.0f, 1e-4f) && NEAR(X.cine.screen.fade_rgb[0], 1.0f, 1e-4f) && NEAR(X.cine.screen.fade_rgb[2], 0.5f, 1e-4f));
	call("fade_in", a, 4, HS_VOID);
	step(30);
	CHECK(NEAR(X.cine.screen.fade_alpha, 0.0f, 1e-4f));
	a[3] = sv(0);
	call("fade_out", a, 4, HS_VOID);
	CHECK(NEAR(X.cine.screen.fade_alpha, 1.0f, 1e-4f));

	/* titles */
	CHECK(X.cin.title_count > 0);
	for (k = 0; k < CINE_TITLE_SLOTS; k++) CHECK(!X.cine.titles[k].active);
	CALL1("cinematic_set_title", HS_VOID, hv(0, 14));
	{
		int any = 0;
		for (k = 0; k < CINE_TITLE_SLOTS; k++) any |= X.cine.titles[k].active;
		CHECK(any);
	}
	CALL1("cinematic_set_title", HS_VOID, hv(9999, 14));          /* unknown: no-op */

	/* screen effects: state only, typed no-ops */
	CALL1("cinematic_screen_effect_start", HS_VOID, bv(1));
	CHECK(X.m.misc.screen_effect == 1);
	call("cinematic_screen_effect_stop", NULL, 0, HS_VOID);
	CHECK(X.m.misc.screen_effect == 0);
	CALL1("cinematic_suppress_bsp_object_creation", HS_VOID, bv(1));
	CHECK(X.m.misc.suppress_bsp_objects == 1);
}

/* ---- 2. sound ----------------------------------------------------------------------------------------------------------------------------- */
static int find_dialogue(unsigned skip, uint32_t *tag, uint32_t *tmin, uint32_t *tmax)
{
	uint32_t i;
	for (i = 0; i < X.strm.sound_count; i++) {
		strm_sound s = strm_get_sound(&X.strm, i);
		uint32_t v, lo = 0xFFFFFFFFu, hi = 0;
		if (s.kind != STRM_KIND_DIALOGUE || (X.snd.sound_count && snd_find(&X.snd, s.tag_id) >= 0 && !(snd_get_sound(&X.snd, (uint32_t)snd_find(&X.snd, s.tag_id)).flags & SND_F_STREAMED))) continue;
		if (skip) { skip--; continue; }
		for (v = 0; v < s.stream_count; v++) {
			strm_stream st = strm_get_stream(&X.strm, s.stream_first + v);
			uint32_t t = (uint32_t)(((uint64_t)st.frames * 30u + st.rate - 1u) / st.rate);
			if (t < lo) lo = t;
			if (t > hi) hi = t;
		}
		if (s.max_play_ms) lo = hi = (uint32_t)((uint64_t)s.max_play_ms * 30u / 1000u);      /* the declared length (tag +0x84) is what the Xbox waits for */
		*tag = s.tag_id; *tmin = lo; *tmax = hi;
		return 1;
	}
	return 0;
}

static void test_sound(void)
{
	uint32_t d1 = 0, d2 = 0, lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0, t;
	hs_value a[3];
	unsigned i;
	int o;
	fresh();
	CHECK(find_dialogue(0, &d1, &lo1, &hi1) && find_dialogue(1, &d2, &lo2, &hi2));
	CHECK(lo1 > 0);
	o = first_object();

	/* a dialogue line without a stream player takes its natural length: sound_impulse_time counts it down to zero */
	CHECK(X.m.stream == NULL);
	a[0] = hv(d1, 24); a[1] = hv(HS_HANDLE_NONE, 37); a[2] = rv(1.0f);
	CHECK(CALL0("game_safe_to_speak", HS_BOOL).v.i == 1);
	call("sound_impulse_start", a, 3, HS_VOID);
	{
		uint32_t t0 = (uint32_t)call("sound_impulse_time", a, 1, HS_SHORT).v.i;
		CHECK(t0 >= lo1 && t0 <= hi1);
		CHECK((uint32_t)call("sound_impulse_time", a, 1, HS_LONG).v.i == t0);
		CHECK(NEAR(call("sound_impulse_time", a, 1, HS_REAL).v.f, (float)t0, 0.01f));
		CHECK(CALL0("game_safe_to_speak", HS_BOOL).v.i == 0);
		step(t0 / 2);
		t = (uint32_t)call("sound_impulse_time", a, 1, HS_SHORT).v.i;
		CHECK(t == t0 - t0 / 2);
		step(t0);                                                  /* past the end: stays 0 */
		CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i == 0);
		CHECK(CALL0("game_safe_to_speak", HS_BOOL).v.i == 1);
	}
	/* a second line in the dialogue slot replaces the first: its time is over */
	call("sound_impulse_start", a, 3, HS_VOID);
	CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i > 0);
	{
		hs_value b[3];
		b[0] = hv(d2, 24); b[1] = a[1]; b[2] = a[2];
		call("sound_impulse_start", b, 3, HS_VOID);
		CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i == 0);
		t = (uint32_t)call("sound_impulse_time", b, 1, HS_SHORT).v.i;
		CHECK(t >= lo2 && t <= hi2);
		call("sound_impulse_stop", b, 1, HS_VOID);
		CHECK(call("sound_impulse_time", b, 1, HS_SHORT).v.i == 0);
	}
	/* lip sync: a line a unit speaks drives its face's talk byte with the line's mouth data, one byte per tick, closing the mouth when it ends */
	{
		uint8_t blob[16 + 12 * 8 + 16], *q = blob;
		strm_sound ss = strm_get_sound(&X.strm, (uint32_t)strm_find(&X.strm, d1));
		mouth_section ms;
		unsigned v, k;
		uint32_t t0;
		static const uint8_t line[8] = { 0, 40, 255, 130, 5, 0, 90, 200 };
		CHECK(ss.stream_count >= 1 && ss.stream_count <= 8);
		memset(blob, 0, sizeof blob);
		memcpy(q, "MOUT", 4); q[4] = 1; q[8] = (uint8_t)ss.stream_count; q[12] = (uint8_t)(16 + 12 * ss.stream_count);
		for (v = 0; v < ss.stream_count; v++) {                      /* every take has the same mouth data (the picked take is random) */
			uint8_t *r = blob + 16 + 12 * v;
			r[0] = (uint8_t)d1; r[1] = (uint8_t)(d1 >> 8); r[2] = (uint8_t)(d1 >> 16); r[3] = (uint8_t)(d1 >> 24);
			r[4] = (uint8_t)v; r[6] = 8; r[8] = 0;
		}
		memcpy(blob + 16 + 12 * ss.stream_count, line, 8);
		CHECK(mouth_open(&ms, blob, 16 + 12 * ss.stream_count + 8) == 0);
		X.m.ow.mouth = &ms;
		a[0] = hv(d1, 24); a[1] = hv(mission_obj_handle(o), 37); a[2] = rv(1.0f);
		X.objects.obj[o].face.talk = 77;
		call("sound_impulse_start", a, 3, HS_VOID);
		t0 = (uint32_t)call("sound_impulse_time", a, 1, HS_SHORT).v.i;
		CHECK(t0 > 10);
		for (k = 1; k < 8; k++) {                                               /* tick k after the start plays byte k */
			step(1);
			CHECK(X.objects.obj[o].face.talk == line[k]);
		}
		step(1);
		CHECK(X.objects.obj[o].face.talk == 0);                                 /* past the data: closed */
		call("sound_impulse_stop", a, 1, HS_VOID);
		X.m.ow.mouth = NULL;
		step(1);
		CHECK(X.objects.obj[o].face.talk == 0);
	}
	/* unknown sounds and none: no-ops with time 0 */
	a[0] = hv(0xE0001234u, 24);
	call("sound_impulse_start", a, 3, HS_VOID);
	CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i == 0);
	a[0] = hv(HS_HANDLE_NONE, 24);
	call("sound_impulse_start", a, 3, HS_VOID);
	CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i == 0);
	call("sound_impulse_predict", a, 1, HS_VOID);

	/* resident sounds go to the hook with the object's position, and their time is their longest permutation */
	{
		uint32_t rt = 0, rtime = 0;
		float maxd = 0.0f;
		for (i = 0; i < X.snd.sound_count && !rt; i++) {
			snd_sound s = snd_get_sound(&X.snd, i);
			if (s.perm_count > 0 && !(s.flags & (SND_F_DROPPED | SND_F_STREAMED | SND_F_UNSUPPORTED)) && s.klass != SND_CLASS_UNIT_DIALOG) {
				uint32_t k;
				for (k = 0; k < s.perm_count; k++) {
					snd_perm p = snd_get_perm(&X.snd, &s, k);
					uint32_t tt = (uint32_t)(((uint64_t)p.frames * 30u + p.rate - 1u) / p.rate);
					if (tt > rtime) rtime = tt;
				}
				if (s.max_play_ms) rtime = (uint32_t)((uint64_t)s.max_play_ms * 30u / 1000u);
				rt = s.tag_id; maxd = s.max_dist;
			}
		}
		CHECK(rt != 0);
		memset(&g_hooks, 0, sizeof g_hooks);
		X.m.misc_env.user = &g_hooks; X.m.misc_env.play = hk_play; X.m.misc_env.loop_start = hk_loop_start; X.m.misc_env.loop_stop = hk_loop_stop;
		a[0] = hv(rt, 24); a[1] = hv(HS_HANDLE_NONE, 37); a[2] = rv(0.75f);
		call("sound_impulse_start", a, 3, HS_VOID);
		CHECK(g_hooks.plays == 1 && g_hooks.last_tag == rt && g_hooks.with_pos == 0 && NEAR(g_hooks.last_gain, 0.75f, 1e-5f));
		CHECK((uint32_t)call("sound_impulse_time", a, 1, HS_SHORT).v.i == rtime);
		a[1] = hv(mission_obj_handle(o), 37);
		{
			float home[3];
			memcpy(home, X.objects.obj[o].pos, sizeof home);
			X.objects.obj[o].pos[0] = X.game.player.pos[0] + 0.5f; X.objects.obj[o].pos[1] = X.game.player.pos[1]; X.objects.obj[o].pos[2] = X.game.player.pos[2];
			call("sound_impulse_start", a, 3, HS_VOID);
			CHECK(g_hooks.plays == 2 && g_hooks.with_pos == 1 && g_hooks.last_gain <= 0.75f + 1e-5f && g_hooks.last_gain > 0.0f);
			memcpy(X.objects.obj[o].pos, home, sizeof home);
		}
		if (maxd > 0.0f) {                                          /* an object beyond the max distance is inaudible */
			float save = X.objects.obj[o].pos[0];
			X.objects.obj[o].pos[0] = X.game.player.pos[0] + maxd + 1000.0f;
			call("sound_impulse_start", a, 3, HS_VOID);
			CHECK(g_hooks.plays == 2);
			CHECK((uint32_t)call("sound_impulse_time", a, 1, HS_SHORT).v.i == rtime);   /* the script still waits as long */
			X.objects.obj[o].pos[0] = save;
		}
		/* the player's own body is not positional */
		a[1] = hv(mission_obj_handle(X.m.player_obj), 38);
		call("sound_impulse_start", a, 3, HS_VOID);
		CHECK(g_hooks.with_pos == 1);
	}

	/* looping sounds: streamed ones keep a slot, resident ones go through the hook, alternate is remembered */
	{
		uint32_t li, stream_loop = 0, music_loop = 0, res_loop = 0;
		for (li = 0; li < X.strm.loop_count; li++) {
			strm_loop lp = strm_get_loop(&X.strm, li);
			strm_track tr;
			int si;
			if (!lp.track_count) continue;
			tr = strm_get_track(&X.strm, &lp, 0);
			si = strm_find(&X.strm, tr.loop != STRM_NO_TAG ? tr.loop : tr.start);
			if (si < 0) continue;
			if (snd_find(&X.snd, tr.loop != STRM_NO_TAG ? tr.loop : tr.start) >= 0 && snd_get_sound(&X.snd, (uint32_t)snd_find(&X.snd, tr.loop != STRM_NO_TAG ? tr.loop : tr.start)).perm_count > 0 &&
			    !(snd_get_sound(&X.snd, (uint32_t)snd_find(&X.snd, tr.loop != STRM_NO_TAG ? tr.loop : tr.start)).flags & (SND_F_DROPPED | SND_F_STREAMED))) continue;
			if (!stream_loop) stream_loop = lp.tag_id;
			if (!music_loop && strm_get_sound(&X.strm, (uint32_t)si).kind == STRM_KIND_MUSIC) music_loop = lp.tag_id;
		}
		for (li = 0; li < X.snd.loop_count && !res_loop; li++) {
			snd_loop lp = snd_get_loop(&X.snd, li);
			if (strm_find_loop(&X.strm, lp.tag_id) < 0) res_loop = lp.tag_id;
		}
		CHECK(stream_loop != 0 && music_loop != 0 && res_loop != 0);
		memset(&g_hooks, 0, sizeof g_hooks);
		a[0] = hv(music_loop, 27); a[1] = hv(HS_HANDLE_NONE, 37); a[2] = rv(1.0f);
		call("sound_looping_start", a, 3, HS_VOID);
		{
			unsigned running = 0, slot_ok = 0;
			for (i = 0; i < MISC_LOOPS; i++)
				if (X.m.misc.loops[i].used && X.m.misc.loops[i].tag == music_loop) { running = X.m.misc.loops[i].running; slot_ok = X.m.misc.loops[i].slot == STREAM_SLOT_MUSIC; }
			CHECK(running && slot_ok);
		}
		CHECK(g_hooks.loop_starts == 0);
		a[0] = hv(music_loop, 27); a[1] = bv(1);
		call("sound_looping_set_alternate", a, 2, HS_VOID);
		for (i = 0; i < MISC_LOOPS; i++) if (X.m.misc.loops[i].used && X.m.misc.loops[i].tag == music_loop) CHECK(X.m.misc.loops[i].alt == 1);
		call("sound_looping_stop", a, 1, HS_VOID);
		for (i = 0; i < MISC_LOOPS; i++) if (X.m.misc.loops[i].used && X.m.misc.loops[i].tag == music_loop) CHECK(X.m.misc.loops[i].running == 0);
		a[0] = hv(res_loop, 27); a[1] = hv(HS_HANDLE_NONE, 37); a[2] = rv(1.0f);
		call("sound_looping_start", a, 3, HS_VOID);
		CHECK(g_hooks.loop_starts == 1 && g_hooks.last_tag == res_loop);
		call("sound_looping_stop", a, 1, HS_VOID);
		CHECK(g_hooks.loop_stops == 1);
		call("sound_looping_stop", a, 1, HS_VOID);                    /* already stopped: nothing again */
		CHECK(g_hooks.loop_stops == 1);
		call("sound_looping_predict", a, 1, HS_VOID);
		/* set_alternate before the start is remembered for it */
		a[0] = hv(stream_loop, 27); a[1] = bv(1);
		call("sound_looping_set_alternate", a, 2, HS_VOID);
		for (i = 0; i < MISC_LOOPS; i++) if (X.m.misc.loops[i].used && X.m.misc.loops[i].tag == stream_loop) CHECK(X.m.misc.loops[i].alt == 1 && !X.m.misc.loops[i].running);
	}

	/* the real stream player: a dialogue line and a music loop actually play, the script's timing does not depend on it */
	{
		fio_t io;
		uint32_t music_loop = 0, li;
		io.f = fopen(X.strm_path, "rb"); io.busy = 0;
		CHECK(io.f != NULL);
		if (io.f) {
			stream_io sio;
			sio.start = io_start; sio.poll = io_poll; sio.user = &io;
			memset(&g_sp, 0, sizeof g_sp);
			stream_init(&g_sp, &X.strm, &sio, X.strd_offset);
			X.m.stream = (struct stream_player *)&g_sp;
			fresh();
			X.m.stream = (struct stream_player *)&g_sp;
			a[0] = hv(d1, 24); a[1] = hv(HS_HANDLE_NONE, 37); a[2] = rv(1.0f);
			call("sound_impulse_start", a, 3, HS_VOID);
			CHECK(stream_slot_busy(&g_sp, STREAM_SLOT_DIALOGUE));
			CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i > 0);
			for (i = 0; i < 30; i++) stream_tick(&g_sp);
			CHECK(stream_get_stats(&g_sp)->frames_decoded > 0 && stream_get_stats(&g_sp)->plays >= 1);
			{
				hs_value b[3];
				b[0] = hv(d2, 24); b[1] = a[1]; b[2] = a[2];
				call("sound_impulse_start", b, 3, HS_VOID);
				CHECK(stream_get_stats(&g_sp)->steals >= 1);
				CHECK(call("sound_impulse_time", a, 1, HS_SHORT).v.i == 0);
				call("sound_impulse_stop", b, 1, HS_VOID);
			}
			for (li = 0; li < X.strm.loop_count && !music_loop; li++) {
				strm_loop lp = strm_get_loop(&X.strm, li);
				strm_track tr;
				int si;
				if (!lp.track_count) continue;
				tr = strm_get_track(&X.strm, &lp, 0);
				si = strm_find(&X.strm, tr.loop != STRM_NO_TAG ? tr.loop : tr.start);
				if (si >= 0 && strm_get_sound(&X.strm, (uint32_t)si).kind == STRM_KIND_MUSIC) music_loop = lp.tag_id;
			}
			a[0] = hv(music_loop, 27); a[1] = hv(HS_HANDLE_NONE, 37); a[2] = rv(1.0f);
			call("sound_looping_start", a, 3, HS_VOID);
			CHECK(stream_slot_busy(&g_sp, STREAM_SLOT_MUSIC));
			for (i = 0; i < 60; i++) stream_tick(&g_sp);
			CHECK(stream_voice_buffered(&g_sp, STREAM_SLOT_MUSIC) > 0);
			a[1] = bv(1);
			call("sound_looping_set_alternate", a, 2, HS_VOID);
			call("sound_looping_stop", a, 1, HS_VOID);
			for (i = 0; i < 120; i++) { stream_tick(&g_sp); (void)stream_pump(&g_sp, 0); stream_ring_consume(&g_sp, stream_ring_count(&g_sp)); }
			CHECK(stream_get_stats(&g_sp)->read_errors == 0);
			X.m.stream = NULL;
			fclose(io.f);
		}
	}
	/* sound_class_set_gain: the class name is a string constant; the test runs the real call sites of the script below (test_call_sites) */
}

/* ---- 3. hud ---------------------------------------------------------------------------------------------------------------------------- */
static void test_hud(void)
{
	hs_value a[2];
	uint32_t n, set;
	const cine_msg *mg;
	fresh();
	set = (uint32_t)huddef_find_set(&X.strs, huddef_get_globals(&X.hud).level_messages);
	n = huddef_set_size(&X.strs, set);
	CHECK(n > 10);
	CALL1("hud_set_help_text", HS_VOID, hv(0, 22));
	mg = find_msg(CINE_MSG_HELP);
	CHECK(mg && mg->units > 0 && mg->duration == 0);
	CHECK(X.m.misc.help_shown == 1 && X.m.misc.help_msg1 == 1);
	CHECK(cine_msg_slot(&X.cine, (unsigned)(mg - X.cine.msgs)) != NULL);
	CALL1("show_hud_help_text", HS_VOID, bv(0));
	CHECK(cine_msg_slot(&X.cine, (unsigned)(mg - X.cine.msgs)) == NULL);
	CALL1("show_hud_help_text", HS_VOID, bv(1));
	CHECK(cine_msg_slot(&X.cine, (unsigned)(mg - X.cine.msgs)) != NULL);
	CALL1("hud_set_help_text", HS_VOID, hv(n - 1, 22));
	CHECK(find_msg(CINE_MSG_HELP) && X.m.misc.help_shown == 2 && X.m.misc.help_msg1 == (int)n);
	CALL1("hud_set_help_text", HS_VOID, hv(n, 22));               /* out of the set: no-op */
	CALL1("hud_set_help_text", HS_VOID, hv(HS_HANDLE_NONE, 22));
	CHECK(X.m.misc.help_shown == 2);
	/* objectives */
	CALL1("hud_set_objective_text", HS_VOID, hv(2, 22));
	mg = find_msg(CINE_MSG_OBJECTIVE);
	CHECK(mg && mg->units > 0);
	CHECK(X.m.misc.objectives_shown == 1 && (X.m.objective_flags & (1u << 2)) && X.m.misc.objective_msg1 == 3);
	CALL1("hud_set_objective_text", HS_VOID, hv(4, 22));
	CHECK(X.m.misc.objectives_shown == 2 && (X.m.objective_flags & (1u << 4)));
	/* scenario help (numbered strings of the scenario) */
	CHECK(X.cin.string_count > 2);
	CALL1("display_scenario_help", HS_VOID, sv(1));
	mg = find_msg(CINE_MSG_SCENARIO_HELP);
	CHECK(mg && mg->units > 0 && mg->duration == CINE_SCENARIO_HELP_TICKS && X.m.misc.scenario_help_shown == 1);
	CALL1("display_scenario_help", HS_VOID, sv(500));
	CALL1("display_scenario_help", HS_VOID, sv(-1));
	CHECK(X.m.misc.scenario_help_shown == 1);
	step(CINE_SCENARIO_HELP_TICKS + 1);
	CHECK(find_msg(CINE_MSG_SCENARIO_HELP) == NULL);
	CALL1("enable_hud_help_flash", HS_VOID, bv(1));
	CHECK(X.m.misc.help_flash == 1);
	/* hud element switches */
	CALL1("show_hud", HS_VOID, bv(0)); CHECK(X.m.misc.hud_hidden & MISC_HUD_ALL);
	CALL1("show_hud", HS_VOID, bv(1)); CHECK(!(X.m.misc.hud_hidden & MISC_HUD_ALL));
	CALL1("hud_show_crosshair", HS_VOID, bv(0)); CALL1("hud_show_health", HS_VOID, bv(0)); CALL1("hud_show_shield", HS_VOID, bv(0)); CALL1("hud_show_motion_sensor", HS_VOID, bv(0));
	CHECK(X.m.misc.hud_hidden == (MISC_HUD_CROSSHAIR | MISC_HUD_HEALTH | MISC_HUD_SHIELD | MISC_HUD_MOTION));
	CALL1("hud_blink_health", HS_VOID, bv(1)); CALL1("hud_blink_shield", HS_VOID, bv(1)); CALL1("hud_blink_motion_sensor", HS_VOID, bv(1));
	CHECK(X.m.misc.hud_blink == (MISC_HUD_HEALTH | MISC_HUD_SHIELD | MISC_HUD_MOTION));
	CALL1("hud_blink_health", HS_VOID, bv(0)); CHECK(!(X.m.misc.hud_blink & MISC_HUD_HEALTH));
	/* countdown timer */
	CHECK(CALL0("hud_get_timer_ticks", HS_SHORT).v.i == 0);
	a[0] = sv(1); a[1] = sv(30);
	call("hud_set_timer_time", a, 2, HS_VOID);
	CHECK(CALL0("hud_get_timer_ticks", HS_SHORT).v.i == 2700);
	step(100);
	CHECK(CALL0("hud_get_timer_ticks", HS_SHORT).v.i == 2600);
	CALL1("pause_hud_timer", HS_VOID, bv(1));
	step(50);
	CHECK(CALL0("hud_get_timer_ticks", HS_SHORT).v.i == 2600);
	CALL1("pause_hud_timer", HS_VOID, bv(0));
	step(100);
	CHECK(CALL0("hud_get_timer_ticks", HS_SHORT).v.i == 2500);
	step(3000);
	CHECK(CALL0("hud_get_timer_ticks", HS_SHORT).v.i == 0);
	CALL1("show_hud_timer", HS_VOID, bv(1)); CHECK(X.m.misc.timer_shown);
	a[0] = sv(0); a[1] = sv(20);
	call("hud_set_timer_warning_time", a, 2, HS_VOID); CHECK(X.m.misc.timer_warning == 600);
	a[0] = sv(10); a[1] = sv(20);
	call("hud_set_timer_position", a, 2, HS_VOID); CHECK(X.m.misc.timer_x == 10 && X.m.misc.timer_y == 20);
	{
		hs_value b[2];
		b[0].type = HS_LONG; b[0].vtype = 8; b[0].v.i = 900; b[0].aux = 0; b[1] = bv(1);
		call("numeric_countdown_timer_set", b, 2, HS_VOID);
		CHECK(X.m.misc.ncd_active && X.m.misc.ncd_end == X.m.tick + 900);
		call("numeric_countdown_timer_stop", NULL, 0, HS_VOID);
		CHECK(!X.m.misc.ncd_active);
	}
	CALL1("time_code_show", HS_VOID, bv(1)); CALL1("time_code_start", HS_VOID, bv(1));
	CHECK(X.m.misc.time_code_shown && X.m.misc.time_code_running);
}

/* ---- 4. navigation points ------------------------------------------------------------------------------------------------------------ */
static void test_nav(void)
{
	hs_value a[4];
	float p[3], f0[3], f1[3];
	devdef_cflag c0, c1;
	int o;
	fresh();
	CHECK(mission_navpoint_target(&X.m, p) == 0 && misc_nav_count(&X.m) == 0);
	c0 = devdef_get_cflag(&X.dev, 1); c1 = devdef_get_cflag(&X.dev, 3);
	f0[0] = c0.pos[0]; f0[1] = c0.pos[1]; f0[2] = c0.pos[2] + 0.5f;
	f1[0] = c1.pos[0]; f1[1] = c1.pos[1]; f1[2] = c1.pos[2];
	a[0] = hv(1, 21); a[1] = hv(2, 33); a[2] = hv(1, 12); a[3] = rv(0.5f);
	step(7);
	call("activate_team_nav_point_flag", a, 4, HS_VOID);
	CHECK(misc_nav_count(&X.m) == 1);
	CHECK(mission_navpoint_target(&X.m, p) == 1 && NEAR(p[0], f0[0], 1e-4f) && NEAR(p[1], f0[1], 1e-4f) && NEAR(p[2], f0[2], 1e-4f));
	CHECK(misc_nav_get(&X.m, 0)->kind == MISC_NAV_FLAG && misc_nav_get(&X.m, 0)->tick == 7 && misc_nav_get(&X.m, 0)->team == 2);
	step(5);
	a[2] = hv(3, 12); a[3] = rv(0.0f);
	call("activate_team_nav_point_flag", a, 4, HS_VOID);
	CHECK(misc_nav_count(&X.m) == 2);
	CHECK(mission_navpoint_target(&X.m, p) == 1 && NEAR(p[0], f1[0], 1e-4f) && NEAR(p[2], f1[2], 1e-4f));
	call("activate_team_nav_point_flag", a, 4, HS_VOID);          /* the same again: still two, still the newest */
	CHECK(misc_nav_count(&X.m) == 2);
	{
		hs_value d[2];
		d[0] = hv(2, 33); d[1] = hv(3, 12);
		call("deactivate_team_nav_point_flag", d, 2, HS_VOID);
		CHECK(misc_nav_count(&X.m) == 1);
		CHECK(mission_navpoint_target(&X.m, p) == 1 && NEAR(p[0], f0[0], 1e-4f));
		d[0] = hv(1, 33);                                          /* another team's: nothing happens */
		d[1] = hv(1, 12);
		call("deactivate_team_nav_point_flag", d, 2, HS_VOID);
		CHECK(misc_nav_count(&X.m) == 1);
	}
	/* an object: the point follows it every query; a destroyed object's point is not live */
	o = first_object();
	a[0] = hv(1, 21); a[1] = hv(2, 33); a[2] = hv(mission_obj_handle(o), 37); a[3] = rv(1.0f);
	call("activate_team_nav_point_object", a, 4, HS_VOID);
	CHECK(misc_nav_count(&X.m) == 2);
	CHECK(mission_navpoint_target(&X.m, p) == 1 && NEAR(p[2], X.objects.obj[o].pos[2] + 0.5f * X.objects.obj[o].radius + 1.0f, 1e-4f));
	X.objects.obj[o].pos[0] += 12.5f;
	CHECK(mission_navpoint_target(&X.m, p) == 1 && NEAR(p[0], X.objects.obj[o].pos[0], 1e-4f));
	X.objects.obj[o].flags |= OBJ_F_FREE;
	CHECK(mission_navpoint_target(&X.m, p) == 1 && NEAR(p[0], f0[0], 1e-4f));      /* falls back to the live flag point */
	X.objects.obj[o].flags &= ~OBJ_F_FREE;
	{
		hs_value d[2];
		d[0] = hv(2, 33); d[1] = hv(mission_obj_handle(o), 37);
		call("deactivate_team_nav_point_object", d, 2, HS_VOID);
		CHECK(misc_nav_count(&X.m) == 1);
		d[1] = hv(1, 12);
		call("deactivate_team_nav_point_flag", d, 2, HS_VOID);
		CHECK(misc_nav_count(&X.m) == 0 && mission_navpoint_target(&X.m, p) == 0);
	}
	/* unknown flag / object: no-ops; a full table drops the oldest */
	a[2] = hv(9999, 12);
	call("activate_team_nav_point_flag", a, 4, HS_VOID);
	a[2] = hv(HS_HANDLE_NONE, 37);
	call("activate_team_nav_point_object", a, 4, HS_VOID);
	CHECK(misc_nav_count(&X.m) == 0);
	{
		unsigned i;
		for (i = 0; i < MISC_NAVPOINTS + 5; i++) {
			a[1] = hv(1 + (i & 1), 33); a[2] = hv(i % X.dev.flag_count, 12);
			step(1);
			call("activate_team_nav_point_flag", a, 4, HS_VOID);
		}
		CHECK(misc_nav_count(&X.m) <= MISC_NAVPOINTS && misc_nav_count(&X.m) > 0);
		CHECK(mission_navpoint_target(&X.m, p) == 1);
	}
}

/* ---- 5. game flow ----------------------------------------------------------------------------------------------------------------------- */
static void test_flow(void)
{
	hs_value r, a[1];
	fresh();
	r = CALL0("game_difficulty_get", HS_HANDLE); CHECK(r.v.h == 3);
	r = CALL0("game_difficulty_get_real", HS_REAL); CHECK(NEAR(r.v.f, 3.0f, 1e-6f));
	X.m.difficulty = 0; r = CALL0("game_difficulty_get", HS_HANDLE); CHECK(r.v.h == 0);
	CHECK(CALL0("game_is_cooperative", HS_BOOL).v.i == 0);
	CHECK(CALL0("game_all_quiet", HS_BOOL).v.i == 1);
	CHECK(CALL0("game_safe_to_save", HS_BOOL).v.i == 1);
	/* saving: a request that main serves, or lapses after MISC_SAVE_TICKS */
	CHECK(CALL0("game_saving", HS_BOOL).v.i == 0);
	call("game_save", NULL, 0, HS_VOID);
	CHECK(X.m.save_requested & MISC_SAVE_NORMAL);
	CHECK(CALL0("game_saving", HS_BOOL).v.i == 1);
	step(MISC_SAVE_TICKS - 1);
	CHECK(CALL0("game_saving", HS_BOOL).v.i == 1);
	X.m.save_requested = 0;                                        /* main served it */
	CHECK(CALL0("game_saving", HS_BOOL).v.i == 0);
	call("game_save_no_timeout", NULL, 0, HS_VOID);
	CHECK(X.m.save_requested & MISC_SAVE_NO_TIMEOUT);
	step(MISC_SAVE_TICKS + 1);
	CHECK(CALL0("game_saving", HS_BOOL).v.i == 0 && X.m.save_requested == 0);
	call("game_save_totally_unsafe", NULL, 0, HS_VOID);
	CHECK(X.m.save_requested & MISC_SAVE_UNSAFE);
	call("game_save_cancel", NULL, 0, HS_VOID);
	CHECK(X.m.save_requested == 0 && CALL0("game_saving", HS_BOOL).v.i == 0);
	/* a cinematic is not a safe moment */
	X.m.cinematic = 1; CHECK(CALL0("game_safe_to_save", HS_BOOL).v.i == 0); X.m.cinematic = 0;
	/* revert */
	CHECK(CALL0("game_reverted", HS_BOOL).v.i == 0);
	X.m.misc.reverted = 1; CHECK(CALL0("game_reverted", HS_BOOL).v.i == 1);
	call("game_revert", NULL, 0, HS_VOID); CHECK(X.m.misc.revert_requested == 1);
	/* time and speed */
	step(40);
	CHECK((uint32_t)CALL0("game_time", HS_LONG).v.i == X.m.tick);
	CALL1("game_speed", HS_VOID, rv(0.5f)); CHECK(NEAR(X.m.misc.game_speed, 0.5f, 1e-6f));
	CALL1("game_skip_ticks", HS_VOID, sv(7)); CALL1("game_skip_ticks", HS_VOID, sv(-3)); CHECK(X.m.misc.skip_ticks == 7);
	/* bsp switching goes through want_bsp, a switch to the current or an unknown bsp does nothing */
	CHECK(CALL0("structure_bsp_index", HS_SHORT).v.i == 0);
	CHECK(X.dev.bsp_count >= 3);
	CALL1("switch_bsp", HS_VOID, sv(0)); CHECK(X.m.want_bsp == -1);
	CALL1("switch_bsp", HS_VOID, sv(99)); CHECK(X.m.want_bsp == -1);
	CALL1("switch_bsp", HS_VOID, sv(-1)); CHECK(X.m.want_bsp == -1);
	CALL1("switch_bsp", HS_VOID, sv(2)); CHECK(X.m.want_bsp == 2 && X.m.want_teleport == 0);
	X.m.want_bsp = -1;
	CALL1("scenario_switch_bsp", HS_VOID, sv(1)); CHECK(X.m.want_bsp == 1);
	X.m.cur_bsp = 1;
	CHECK(CALL0("structure_bsp_index", HS_SHORT).v.i == 1);
	CHECK(NEAR(CALL0("structure_bsp_index", HS_REAL).v.f, 1.0f, 1e-6f));
	/* won / lost */
	CHECK(!X.m.won && !X.m.lost);
	call("game_won", NULL, 0, HS_VOID); CHECK(X.m.won == 1);
	call("game_lost", NULL, 0, HS_VOID); CHECK(X.m.lost == 1);
	CHECK(CALL0("game_safe_to_save", HS_BOOL).v.i == 0);
	(void)a;
}

/* ---- 6. scripted player effect ----------------------------------------------------------------------------------------------------------- */
static void test_player_effect(void)
{
	hs_value a[3];
	float rot[3], tr[3], rb[2], l;
	fresh();
	a[0] = rv(0.0f); a[1] = rv(5.0f); a[2] = rv(0.0f);
	call("player_effect_set_max_rotation", a, 3, HS_VOID);
	a[0] = rv(0.01f); a[1] = rv(0.02f); a[2] = rv(0.04f);
	call("player_effect_set_max_translation", a, 3, HS_VOID);
	a[0] = rv(0.4f); a[1] = rv(1.0f);
	call("player_effect_set_max_rumble", a, 2, HS_VOID);
	l = misc_player_effect_get(&X.m, rot, tr, rb);
	CHECK(l == 0.0f && rot[1] == 0.0f && rb[1] == 0.0f);
	a[0] = rv(1.0f); a[1] = rv(0.5f);                                /* intensity 1, attack half a second */
	call("player_effect_start", a, 2, HS_VOID);
	CHECK(X.game.shake > 0.0f && X.game.shake <= 0.1f);
	step(8);
	l = misc_player_effect_get(&X.m, rot, tr, rb);
	CHECK(l > 0.4f && l < 0.65f && NEAR(rot[1], 5.0f * 3.14159265f / 180.0f * l, 1e-5f) && NEAR(tr[2], 0.04f * l, 1e-6f) && NEAR(rb[0], 0.4f * l, 1e-6f));
	step(10);
	l = misc_player_effect_get(&X.m, rot, tr, rb);
	CHECK(NEAR(l, 1.0f, 1e-6f) && NEAR(rb[1], 1.0f, 1e-6f));
	CALL1("player_effect_stop", HS_VOID, rv(1.0f));                  /* decays over one second */
	step(15);
	l = misc_player_effect_get(&X.m, rot, tr, rb);
	CHECK(l > 0.4f && l < 0.6f);
	step(15);
	l = misc_player_effect_get(&X.m, rot, tr, rb);
	CHECK(l == 0.0f && rot[1] == 0.0f);
	step(100);
	CHECK(misc_player_effect_get(&X.m, rot, tr, rb) == 0.0f);
}

/* ---- 7. debug family: typed no-ops that change nothing ----------------------------------------------------------------------------------- */
static void test_noops(void)
{
	static const char *const names[] = { "objects_predict", "object_beautify", "object_pvs_activate", "object_type_predict", "rasterizer_model_ambient_reflection_tint",
		"garbage_collect_now", "object_pvs_set_camera", "breakable_surfaces_enable", "cls", "breakable_surfaces_reset", "object_pvs_clear", "object_pvs_set_object",
		"render_lights", "rasterizer_lights_reset_for_new_map", "cinematic_screen_effect_set_convolution", "cinematic_screen_effect_set_filter",
		"cinematic_screen_effect_set_filter_desaturation_tint", "cinematic_screen_effect_set_video", "sound_impulse_predict", "sound_looping_predict" };
	unsigned i;
	hs_value a[5];
	fresh();
	for (i = 0; i < 5; i++) a[i] = rv(0.5f);
	for (i = 0; i < sizeof names / sizeof names[0]; i++) {
		mission_misc_t before = X.m.misc;
		cine_state *cs = malloc(sizeof *cs);
		unsigned want = X.m.want_bsp, ob = X.objects.count;
		memcpy(cs, &X.cine, sizeof *cs);
		(void)call(names[i], a, 5, HS_VOID);
		CHECK(memcmp(&before, &X.m.misc, sizeof before) == 0);
		CHECK(memcmp(cs, &X.cine, sizeof *cs) == 0 && want == (unsigned)X.m.want_bsp && ob == X.objects.count);
		free(cs);
	}
}

/* ---- 8. every call site of the a10 script, once, with the script's own arguments ---------------------------------------------------------- */
static struct { char name[48]; unsigned n; } k_unres[8];
static void test_call_sites(void)
{
	uint32_t i, mine = 0, ran = 0;
	unsigned k;
	fresh();
	for (i = 0; i < X.scr.node_count; i++) {
		const hs_node *nd = &X.scr.nodes[i];
		const char *name;
		int is_mine = 0;
		if (nd->kind != HS_N_CALL) continue;
		name = hs_builtin_name(&X.scr, nd->data);
		for (k = 0; k < hs_host_misc_stats(0, NULL, NULL); k++) {
			const char *nm;
			hs_host_misc_stats(k, &nm, NULL);
			if (strcmp(nm, name) == 0) { is_mine = 1; break; }
		}
		if (!is_mine) continue;
		mine++;
		{
			uint32_t miss0 = X.m.missing_noted[0];
			(void)hs_run_expression(&X.hs, i);
			if (X.m.missing_noted[0] != miss0) { unsigned q; for (q = 0; q < 8; q++) if (!strcmp(name, k_unres[q].name) || !k_unres[q].name[0]) { snprintf(k_unres[q].name, sizeof k_unres[q].name, "%s", name); k_unres[q].n++; break; } }
		}
		ran++;
		if ((ran % 50) == 0) {                                     /* keep the world sane for later call sites */
			X.m.won = X.m.lost = 0; X.m.want_bsp = -1; X.m.save_requested = 0; X.m.cinematic = 0;
		}
		step(1);
	}
	printf("  call sites of this domain on a10: %u run synchronously (script errors %u, unresolved handles noted %u, hud messages shown %u + %u objectives)\n", (unsigned)mine,
	       (unsigned)hs_state_ptr(&X.hs)->errors, (unsigned)X.m.missing_noted[0], X.m.misc.help_shown, X.m.misc.objectives_shown);
	{
		unsigned q;
		printf("  unresolved by built-in:");
		for (q = 0; q < 8 && k_unres[q].name[0]; q++) printf(" %s=%u", k_unres[q].name, k_unres[q].n);
		printf("\n");
	}
	printf("  sound_class_set_gain groups set (music ambience dialogue other): %u %u %u %u\n", X.m.misc.class_gain_set[0], X.m.misc.class_gain_set[1], X.m.misc.class_gain_set[2], X.m.misc.class_gain_set[3]);
	CHECK(X.m.misc.class_gain_set[0] | X.m.misc.class_gain_set[1] | X.m.misc.class_gain_set[2] | X.m.misc.class_gain_set[3]);
	CHECK(mine > 1000 && ran == mine);
	CHECK(hs_state_ptr(&X.hs)->errors == 0);
	{
		uint32_t total = 0;
		for (k = 0; k < hs_host_misc_stats(0, NULL, NULL); k++) { uint32_t c; hs_host_misc_stats(k, NULL, &c); total += c; }
		CHECK(total >= mine);
	}
}

/* ---- 9. hostile arguments: never a crash, never a block, on a full and on an empty mission -------------------------------------------------- */
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static hs_value hostile(void)
{
	hs_value v;
	static const uint32_t special[] = { 0u, 1u, 2u, 0xFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x7FC00000u, 0xFF800000u, 0x7F800000u };
	memset(&v, 0, sizeof v);
	v.type = (uint8_t)(HS_BOOL + rnd() % 6);          /* bool real short long string handle */
	if (v.type == HS_SCRIPT) v.type = HS_HANDLE;
	v.vtype = (uint8_t)(rnd() % 44);
	switch (rnd() % 4) {
	case 0: v.v.h = special[rnd() % (sizeof special / sizeof special[0])]; break;
	case 1: v.v.h = rnd() % 200; break;
	case 2: v.v.h = rnd(); break;
	default: v.v.h = (rnd() % 8) | MISSION_LIST_BIT; break;
	}
	return v;
}

static void fuzz_one(mission_t *m, unsigned calls)
{
	unsigned c, k, nb = hs_host_misc_stats(0, NULL, NULL);
	for (c = 0; c < calls; c++) {
		const char *name;
		hs_value a[6], r;
		hs_builtin fn;
		unsigned n = rnd() % 7, i;
		hs_host_misc_stats(rnd() % nb, &name, NULL);
		fn = find_fn(&X.hs, name);
		for (i = 0; i < 6; i++) a[i] = hostile();
		r = hs_neutral(rnd() % 8, 0);
		CHECK(fn != NULL);
		if (fn) CHECK(fn(&X.hs, m, a, n, &r) == 0);
		if (rnd() % 50 == 0) m->tick += rnd() % 500;
		(void)k;
	}
}

static void test_fuzz(void)
{
	static mission_t bare;
	fresh();
	fuzz_one(&X.m, 60000);
	mission_init(&bare);
	fuzz_one(&bare, 30000);
	/* the structures stayed well formed */
	CHECK(misc_nav_count(&X.m) <= MISC_NAVPOINTS);
	{
		float p[3];
		(void)mission_navpoint_target(&X.m, p);
		(void)mission_navpoint_target(&bare, p);
	}
}

/* one mission tick; with the H1 built-ins registered its pre / post hooks run in the order of the integrator's tick */
static void tick_once(const game_input_t *in)
{
#ifdef HM_WITH_OBJ
	mission_t *m = &X.m;
	mission_sync_player(m);
	hs_tick(m->hs);
	devices_tick(m->devices);
	hs_host_obj_pre(m, in);
	units_tick(m->units, m->game);
	game_tick(m->game, m->cinematic ? &(game_input_t){ 0 } : in);
	hs_host_obj_post(m);
	cine_tick(m->cine);
	m->tick++;
#else
	mission_tick(&X.m, in);
#endif
}

/* ---- 10. soak: the a10 script machine, the player idle ----------------------------------------------------------------------------------------- */
static void soak(const char *label, int wake_dormant)
{
	static const uint32_t wake_at[] = { 0, 300, 900, 1800 };
	game_input_t in;
	unsigned t, k, nb, i, unreg_n, maxcalls_k = 0;
	uint32_t maxrem = 0, impulses_seen = 0, last_end = 0;
	clock_t c0;
	double secs;
	const hs_state *st;
	memset(&in, 0, sizeof in);
	fresh();
	c0 = clock();
	for (t = 0; t < 9000; t++) {
		if (wake_dormant)
			for (i = 0; i < sizeof wake_at / sizeof wake_at[0]; i++)
				if (wake_at[i] == t) {
					unsigned s;
					for (s = 0; s < X.scr.script_count; s++) if (X.scr.scripts[s].type == HS_S_DORMANT) hs_wake_script_index(&X.hs, s);
				}
		misc_tick(&X.m);
		tick_once(&in);
		for (i = 0; i < MISC_IMPULSES; i++)
			if (X.m.misc.imp[i].used && X.m.misc.imp[i].end_tick > X.m.tick) {
				uint32_t rem = X.m.misc.imp[i].end_tick - X.m.tick;
				if (rem > maxrem) maxrem = rem;
				if (X.m.misc.imp[i].end_tick != last_end) { impulses_seen++; last_end = X.m.misc.imp[i].end_tick; }
			}
		if (X.m.want_bsp >= 0) X.m.want_bsp = -1;                     /* nobody performs the switch here */
	}
	secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
	st = hs_state_ptr(&X.hs);
	nb = hs_host_misc_stats(0, NULL, NULL);
	printf("soak %s: 9000 ticks, %.2f s host (mission_tick incl. game), threads %u, errors %u, blocked calls %u, thread overflow %u, tick budget hits %u\n",
	       label, secs, hs_thread_count(&X.hs), st->errors, st->blocked_calls, st->thread_overflow, st->tick_budget_hits);
	printf("  longest impulse remaining %u ticks; impulses %u, loops %u; won %d lost %d save req %u; navpoints active %u; help %u objectives %u; bsp want %d\n",
	       maxrem, X.m.misc.impulses_started, X.m.misc.loops_started, X.m.won, X.m.lost, X.m.save_requested, misc_nav_count(&X.m),
	       X.m.misc.help_shown, X.m.misc.objectives_shown, X.m.want_bsp);
	{
		uint32_t total = 0;
		printf("  calls per built-in:");
		for (k = 0; k < nb; k++) {
			const char *nm;
			uint32_t c;
			hs_host_misc_stats(k, &nm, &c);
			total += c;
			if (c) printf(" %s=%u", nm, (unsigned)c);
			if (c) maxcalls_k++;
		}
		printf("\n  %u of %u built-ins of the domain called, %u calls in total\n", maxcalls_k, nb, (unsigned)total);
	}
	unreg_n = hs_unregistered(&X.hs, 0, NULL, NULL);
	printf("  unregistered built-ins hit (%u):", unreg_n);
	for (k = 0; k < unreg_n; k++) {
		const char *nm;
		uint32_t c;
		hs_unregistered(&X.hs, k, &nm, &c);
		printf(" %s=%u", nm, (unsigned)c);
	}
	printf("\n");
	CHECK(st->errors == 0 && st->blocked_calls == 0 && st->thread_overflow == 0 && !X.m.lost);
	CHECK(maxrem < 3000);                                           /* no dialogue wait longer than 100 s */
	/* nothing of this domain is still counting down at the end of an idle run unless it started recently */
	CHECK(impulses_seen <= X.m.misc.impulses_started);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "../../assets/hp2";
	const char *cin = argc > 2 ? argv[2] : "/tmp/halo-cine-a10.hp2";
	if (!mfx_load(&X, dir, cin)) { printf("skip: no complete a10 packs in %s (or no CINE pack %s)\n", dir, cin); return 0; }
	printf("mission_misc_t %u bytes, mission_t %u bytes, hs built-ins of the domain %u\n", (unsigned)sizeof(mission_misc_t), (unsigned)sizeof(mission_t), hs_host_misc_stats(0, NULL, NULL));
	test_camera();
	test_sound();
	test_hud();
	test_nav();
	test_flow();
	test_player_effect();
	test_noops();
	test_call_sites();
	test_fuzz();
	soak("idle", 0);
	soak("idle + every dormant script woken at 0/300/900/1800", 1);
	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("ok\n");
	return 0;
}
