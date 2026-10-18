/* Script host, part H2: camera / cinematic, sound, hud / ui, navigation points, game flow, scripted player effect and the debug / no-op family
 * (docs/34-host-misc.md). Every built-in is an `hs_builtin` registered by name; `host` is a `mission_t`. Never fails: an unresolved handle, a
 * missing module (stream player, HUD strings, cinematic state) makes the call a no-op. No built-in blocks (they return 0): waits are the scripts'
 * own `sleep_until` over `sound_impulse_time` / `camera_time` / `game_saving` / `hud_get_timer_ticks`, which all reach 0 on their own.
 * Pure C11, no heap, no double. */
#include "script/hs_host_misc.h"
#include "audio/stream.h"
#include <math.h>
#include <string.h>

/* The default easing of camera_set (UNVERIFIED: the original's curve is not documented). */
#define MISC_CAMERA_EASE CINE_EASE_SMOOTH
#define MISC_PI 3.14159265f

/* ---- the built-in list: one X entry per registered name; the counters and the table follow from it ------------------------------------- */
#define MISC_LIST(X) \
	X(camera_set) X(camera_set_relative) X(camera_control) X(camera_time) X(camera_set_first_person) X(camera_set_dead) \
	X(cinematic_start) X(cinematic_stop) X(cinematic_show_letterbox) X(cinematic_set_title) X(cinematic_set_near_clip_distance) \
	X(cinematic_skip_start_internal) X(cinematic_skip_stop_internal) X(fade_in) X(fade_out) \
	X(cinematic_screen_effect_start) X(cinematic_screen_effect_stop) X(cinematic_screen_effect_set_convolution) \
	X(cinematic_screen_effect_set_filter) X(cinematic_screen_effect_set_filter_desaturation_tint) X(cinematic_screen_effect_set_video) \
	X(cinematic_suppress_bsp_object_creation) \
	X(sound_impulse_start) X(sound_impulse_time) X(sound_impulse_stop) X(sound_impulse_predict) X(sound_looping_start) X(sound_looping_stop) \
	X(sound_looping_set_alternate) X(sound_looping_predict) X(sound_class_set_gain) \
	X(show_hud_help_text) X(hud_set_help_text) X(hud_set_objective_text) X(enable_hud_help_flash) X(display_scenario_help) X(show_hud) \
	X(hud_show_motion_sensor) X(hud_blink_health) X(hud_blink_motion_sensor) X(hud_blink_shield) X(hud_show_crosshair) X(hud_show_health) \
	X(hud_show_shield) X(show_hud_timer) X(pause_hud_timer) X(hud_get_timer_ticks) X(hud_set_timer_position) X(hud_set_timer_time) \
	X(hud_set_timer_warning_time) X(numeric_countdown_timer_set) X(numeric_countdown_timer_stop) X(time_code_show) X(time_code_start) \
	X(activate_team_nav_point_flag) X(deactivate_team_nav_point_flag) X(activate_team_nav_point_object) X(deactivate_team_nav_point_object) \
	X(game_save) X(game_save_no_timeout) X(game_save_totally_unsafe) X(game_save_cancel) X(game_saving) X(game_safe_to_save) X(game_all_quiet) \
	X(game_safe_to_speak) X(game_difficulty_get) X(game_difficulty_get_real) X(game_is_cooperative) X(game_won) X(game_lost) X(game_revert) \
	X(game_reverted) X(game_time) X(game_speed) X(game_skip_ticks) X(switch_bsp) X(scenario_switch_bsp) X(structure_bsp_index) \
	X(player_effect_set_max_translation) X(player_effect_set_max_rotation) X(player_effect_set_max_rumble) X(player_effect_start) X(player_effect_stop) \
	X(objects_predict) X(object_beautify) X(object_pvs_activate) X(object_type_predict) X(rasterizer_model_ambient_reflection_tint) \
	X(garbage_collect_now) X(object_pvs_set_camera) X(breakable_surfaces_enable) X(cls) X(breakable_surfaces_reset) X(object_pvs_clear) \
	X(object_pvs_set_object) X(render_lights) X(rasterizer_lights_reset_for_new_map)

enum {
#define X(n) ID_##n,
	MISC_LIST(X)
#undef X
	MISC_COUNT
};
static const char *const k_names[MISC_COUNT] = {
#define X(n) #n,
	MISC_LIST(X)
#undef X
};
static uint32_t k_calls[MISC_COUNT];

#define BI(nm) static int bi_##nm(hs_machine *hm, void *host, const hs_value *a, unsigned n, hs_value *r)
#define ENTER(nm) mission_t *m = (mission_t *)host; (void)hm; (void)a; (void)n; (void)r; (void)m; k_calls[ID_##nm]++; if (m) m->builtin_calls++
#define NOOP(nm) BI(nm) { ENTER(nm); return 0; }

/* ---- argument and result helpers ------------------------------------------------------------------------------------------------------- */
static float argf(const hs_value *a, unsigned n, unsigned i, float def)
{
	if (i >= n) return def;
	if (a[i].type == HS_REAL) return a[i].v.f == a[i].v.f ? a[i].v.f : def;
	return (float)a[i].v.i;
}
static int32_t argi(const hs_value *a, unsigned n, unsigned i, int32_t def)
{
	if (i >= n) return def;
	if (a[i].type == HS_REAL) {
		float f = a[i].v.f;
		if (!(f == f)) return def;
		if (f > 2147483000.0f) return 2147483000;
		if (f < -2147483000.0f) return -2147483000;
		return (int32_t)f;
	}
	return a[i].v.i;
}
static int argb(const hs_value *a, unsigned n, unsigned i, int def) { return i < n ? (a[i].type == HS_REAL ? a[i].v.f != 0.0f : a[i].v.i != 0) : def; }
/* A handle argument as an index (HS_HANDLE_NONE and negative values are -1). */
static int64_t argh(const hs_value *a, unsigned n, unsigned i)
{
	if (i >= n || a[i].v.h == HS_HANDLE_NONE) return -1;
	return (int64_t)a[i].v.h;
}
static uint32_t argtag(const hs_value *a, unsigned n, unsigned i) { return i < n ? a[i].v.h : HS_HANDLE_NONE; }

static void ret_int(hs_value *r, int32_t i)
{
	if (r->type == HS_REAL) r->v.f = (float)i;
	else if (r->type == HS_BOOL) r->v.i = i != 0;
	else r->v.i = i;
}
static void ret_bool(hs_value *r, int b) { r->type = HS_BOOL; r->v.i = b != 0; }

/* minutes * 60 + seconds as ticks, clamped (hostile arguments must not overflow) */
static uint32_t ticks_of_min_sec(int32_t mins, int32_t secs)
{
	int64_t t = ((int64_t)mins * 60 + secs) * 30;
	return t < 0 ? 0u : t > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)t;
}

static uint32_t ticks_of_seconds(float s)
{
	if (!(s > 0.0f)) return 0;
	if (s > 1.0e6f) s = 1.0e6f;
	return (uint32_t)(s * 30.0f + 0.5f);
}

static uint32_t misc_rand(mission_t *m)
{
	uint32_t x = m->misc.rng;
	if (!x) x = 0x9E3779B9u ^ (m->tick * 2654435761u + 1u);
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	m->misc.rng = x ? x : 1u;
	return x;
}

/* ---- camera ---------------------------------------------------------------------------------------------------------------------------- */
static int cam_point_pose(const mission_t *m, int64_t idx, cine_pose *p)
{
	devdef_camera c;
	if (idx < 0 || !m->dev || (uint64_t)idx >= m->dev->camera_count) return 0;
	c = devdef_get_camera(m->dev, (uint32_t)idx);
	p->pos[0] = c.pos[0]; p->pos[1] = c.pos[1]; p->pos[2] = c.pos[2];
	cine_halo_euler_to_ypr(c.ori[0], c.ori[1], c.ori[2], &p->yaw, &p->pitch, &p->roll);       /* Halo's euler convention (ADR-063) */
	p->fov = c.fov > 0.05f && c.fov < 3.0f ? c.fov : CINE_DEFAULT_FOV;
	return 1;
}

static void refresh_player_pose(mission_t *m)
{
	camera_t c;
	cine_pose p;
	if (!m->game || !m->cine) return;
	memset(&c, 0, sizeof c);
	game_camera(m->game, &c);
	p.pos[0] = c.pos[0]; p.pos[1] = c.pos[1]; p.pos[2] = c.pos[2];
	p.yaw = c.yaw; p.pitch = c.pitch; p.roll = 0.0f; p.fov = c.hfov;
	cine_cam_set_player_pose(&m->cine->cam, &p);
}

static int object_frame(const mission_t *m, int obj, cine_frame *f)
{
	const object_t *o;
	if (!mission_object_live(m, obj)) return 0;
	o = &m->objects->obj[obj];
	f->pos[0] = o->pos[0]; f->pos[1] = o->pos[1]; f->pos[2] = o->pos[2];
	f->yaw = atan2f(o->root.m[0][1], o->root.m[0][0]);
	return 1;
}

BI(camera_set)
{
	cine_pose p;
	ENTER(camera_set);
	if (!m || !m->cine || !cam_point_pose(m, argh(a, n, 0), &p)) { if (m) mission_note_missing(m, "camera_set"); return 0; }
	refresh_player_pose(m);
	m->misc.cam_anchor_obj1 = 0;
	cine_cam_set(&m->cine->cam, &p, (uint32_t)(argi(a, n, 1, 0) > 0 ? argi(a, n, 1, 0) : 0), MISC_CAMERA_EASE);
	return 0;
}

BI(camera_set_relative)
{
	cine_pose p;
	cine_frame f;
	int obj;
	ENTER(camera_set_relative);
	if (!m || !m->cine || n < 3 || !cam_point_pose(m, argh(a, n, 0), &p)) return 0;
	obj = mission_resolve_object(m, &a[2], 0);
	if (!object_frame(m, obj, &f)) { mission_note_missing(m, "camera_set_relative"); return 0; }
	refresh_player_pose(m);
	m->misc.cam_anchor_obj1 = (int16_t)(obj + 1);
	cine_cam_set_relative(&m->cine->cam, &p, (uint32_t)(argi(a, n, 1, 0) > 0 ? argi(a, n, 1, 0) : 0), MISC_CAMERA_EASE, &f);
	return 0;
}

BI(camera_control)
{
	ENTER(camera_control);
	if (!m || !m->cine) return 0;
	refresh_player_pose(m);
	if (!argb(a, n, 0, 0)) m->misc.cam_anchor_obj1 = 0;
	cine_cam_control(&m->cine->cam, argb(a, n, 0, 0));
	return 0;
}

BI(camera_time)
{
	ENTER(camera_time);
	ret_int(r, m && m->cine ? (int32_t)cine_cam_time(&m->cine->cam) : 0);
	return 0;
}

BI(camera_set_first_person)
{
	ENTER(camera_set_first_person);
	if (m && m->cine) cine_cam_set_first_person(&m->cine->cam);
	return 0;
}

BI(camera_set_dead)
{
	ENTER(camera_set_dead);
	if (m && m->cine) cine_cam_set_dead(&m->cine->cam);
	return 0;
}

BI(cinematic_start)
{
	ENTER(cinematic_start);
	if (!m) return 0;
	if (m->cine) cine_start(m->cine);
	m->cinematic = 1;
	return 0;
}

BI(cinematic_stop)
{
	ENTER(cinematic_stop);
	if (!m) return 0;
	if (m->cine) cine_stop(m->cine);
	m->cinematic = 0;
	return 0;
}

BI(cinematic_show_letterbox)
{
	ENTER(cinematic_show_letterbox);
	if (m && m->cine) cine_letterbox(m->cine, argb(a, n, 0, 0));
	return 0;
}

BI(cinematic_set_title)
{
	int64_t t = argh(a, n, 0);
	ENTER(cinematic_set_title);
	if (!m || !m->cine || !m->cin || t < 0) return 0;
	if (cine_title_show(m->cine, m->cin, (uint32_t)t, 0) < 0) mission_note_missing(m, "cinematic_set_title");
	return 0;
}

BI(cinematic_set_near_clip_distance)
{
	ENTER(cinematic_set_near_clip_distance);
	if (m && m->cine) cine_cam_set_near_clip(&m->cine->cam, argf(a, n, 0, 0.0f));
	return 0;
}

BI(cinematic_skip_start_internal)
{
	ENTER(cinematic_skip_start_internal);
	if (m && m->cine) cine_skip_begin(m->cine);
	return 0;
}

BI(cinematic_skip_stop_internal)
{
	ENTER(cinematic_skip_stop_internal);
	if (m && m->cine) cine_skip_end(m->cine);
	return 0;
}

BI(fade_out)
{
	ENTER(fade_out);
	if (m && m->cine) cine_fade_out(m->cine, argf(a, n, 0, 0.0f), argf(a, n, 1, 0.0f), argf(a, n, 2, 0.0f), (uint32_t)(argi(a, n, 3, 0) > 0 ? argi(a, n, 3, 0) : 0));
	return 0;
}

BI(fade_in)
{
	ENTER(fade_in);
	if (m && m->cine) cine_fade_in(m->cine, argf(a, n, 0, 0.0f), argf(a, n, 1, 0.0f), argf(a, n, 2, 0.0f), (uint32_t)(argi(a, n, 3, 0) > 0 ? argi(a, n, 3, 0) : 0));
	return 0;
}

/* Screen effects (blur / filter / convolution of the cinematic renderer) have no PS2 counterpart: only the on / off state is kept. */
BI(cinematic_screen_effect_start) { ENTER(cinematic_screen_effect_start); if (m) m->misc.screen_effect = (uint8_t)argb(a, n, 0, 0); return 0; }
BI(cinematic_screen_effect_stop) { ENTER(cinematic_screen_effect_stop); if (m) m->misc.screen_effect = 0; return 0; }
NOOP(cinematic_screen_effect_set_convolution)
NOOP(cinematic_screen_effect_set_filter)
NOOP(cinematic_screen_effect_set_filter_desaturation_tint)
NOOP(cinematic_screen_effect_set_video)
BI(cinematic_suppress_bsp_object_creation) { ENTER(cinematic_suppress_bsp_object_creation); if (m) m->misc.suppress_bsp_objects = (uint8_t)argb(a, n, 0, 0); return 0; }

/* ---- sound ----------------------------------------------------------------------------------------------------------------------------- */
static const strm_section *strm_of(const mission_t *m)
{
	if (m->stream && m->stream->sec && m->stream->sec->data) return m->stream->sec;
	return m->misc_env.strm && m->misc_env.strm->data ? m->misc_env.strm : NULL;
}

static int snd_resident(const snd_sound *s)
{
	return s->perm_count > 0 && !(s->flags & (SND_F_DROPPED | SND_F_STREAMED | SND_F_UNSUPPORTED));
}

static float class_gain(const mission_t *m, unsigned group)
{
	return group < MISC_GAINS && m->misc.class_gain_set[group] ? m->misc.class_gain[group] : 1.0f;
}

static unsigned kind_group(unsigned kind) { return kind == STRM_KIND_MUSIC ? MISC_GAIN_MUSIC : kind == STRM_KIND_AMBIENCE ? MISC_GAIN_AMBIENCE : MISC_GAIN_DIALOGUE; }
static int kind_slot(unsigned kind) { return kind == STRM_KIND_MUSIC ? STREAM_SLOT_MUSIC : kind == STRM_KIND_AMBIENCE ? STREAM_SLOT_AMBIENCE : STREAM_SLOT_DIALOGUE; }

/* Gain and pan of a mono dialogue or effect source `obj` heard by the player: linear distance attenuation between the sound's min and max
 * distance (0 = none) and the bearing against the player's yaw. The player's own body and "no object" are centred at full gain. */
static void spatial(const mission_t *m, int obj, float min_d, float max_d, float scale, float *gain, float *pan)
{
	*gain = scale; *pan = 0.0f;
	if (obj < 0 || obj == m->player_obj || !m->game || !mission_object_live(m, obj)) return;
	{
		const object_t *o = &m->objects->obj[obj];
		float dx = o->pos[0] - m->game->player.pos[0], dy = o->pos[1] - m->game->player.pos[1];
		float d = sqrtf(dx * dx + dy * dy), rel;
		if (max_d > 0.0f && max_d > min_d) {
			if (d >= max_d) *gain = 0.0f;
			else if (d > min_d) *gain = scale * (1.0f - (d - min_d) / (max_d - min_d));
		}
		if (d > 0.01f) {
			rel = atan2f(dy, dx) - m->game->player.yaw;
			*pan = -sinf(rel);               /* +y is left */
			if (*pan > 1.0f) *pan = 1.0f;
			if (*pan < -1.0f) *pan = -1.0f;
		}
	}
}

static misc_impulse *imp_alloc(mission_t *m, uint32_t tag)
{
	unsigned i, best = 0;
	for (i = 0; i < MISC_IMPULSES; i++) if (m->misc.imp[i].used && m->misc.imp[i].tag == tag) return &m->misc.imp[i];
	for (i = 0; i < MISC_IMPULSES; i++) if (!m->misc.imp[i].used) return &m->misc.imp[i];
	for (i = 1; i < MISC_IMPULSES; i++) if (m->misc.imp[i].end_tick < m->misc.imp[best].end_tick) best = i;
	return &m->misc.imp[best];
}

static const misc_impulse *imp_find(const mission_t *m, uint32_t tag)
{
	unsigned i;
	for (i = 0; i < MISC_IMPULSES; i++) if (m->misc.imp[i].used && m->misc.imp[i].tag == tag) return &m->misc.imp[i];
	return NULL;
}

uint32_t misc_impulse_ticks(const mission_t *m, uint32_t tag)
{
	const misc_impulse *p = imp_find(m, tag);
	return p && p->end_tick > m->tick ? p->end_tick - m->tick : 0;
}

static uint32_t frames_to_ticks(uint32_t frames, uint32_t rate)
{
	uint64_t t;
	if (!rate) return 0;
	t = ((uint64_t)frames * 30u + rate - 1u) / rate;
	return t > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)t;
}

BI(sound_impulse_start)
{
	uint32_t tag = argtag(a, n, 0), dur = 0;
	int obj, slot = -1, si, ni, resident = 0;
	float scale = argf(a, n, 2, 1.0f), pos[3] = { 0.0f, 0.0f, 0.0f };
	const strm_section *ss;
	const snd_section *sn = NULL;
	snd_sound snd;
	misc_impulse *im;
	unsigned i;
	ENTER(sound_impulse_start);
	if (!m || tag == HS_HANDLE_NONE || tag == 0) return 0;
	obj = n > 1 ? mission_resolve_object(m, &a[1], 0) : -1;
	if (obj == m->player_obj) obj = -1;                              /* the player's own body is not a positional source */
	if (obj >= 0) mission_object_pos(m, obj, pos);
	ss = strm_of(m);
	sn = m->misc_env.snd;
	si = ss ? strm_find(ss, tag) : -1;
	ni = sn ? snd_find(sn, tag) : -1;
	memset(&snd, 0, sizeof snd);
	if (ni >= 0) { snd = snd_get_sound(sn, (uint32_t)ni); resident = snd_resident(&snd); }
	if (!resident && si >= 0) {                                     /* streamed: dialogue, music, long ambience */
		strm_sound ssn = strm_get_sound(ss, (uint32_t)si);
		unsigned variant = ssn.stream_count ? misc_rand(m) % ssn.stream_count : 0;
		strm_stream st = strm_get_stream(ss, ssn.stream_first + variant);
		float gain, pan, cg = class_gain(m, kind_group(ssn.kind));
		slot = kind_slot(ssn.kind);
		dur = frames_to_ticks(st.frames, st.rate);
		spatial(m, obj, ssn.min_dist, ssn.max_dist, scale * cg, &gain, &pan);
		for (i = 0; i < MISC_IMPULSES; i++)                         /* the slot's previous sound is replaced: its time is over */
			if (m->misc.imp[i].used && m->misc.imp[i].slot == slot && m->misc.imp[i].tag != tag) { m->misc.imp[i].slot = -1; m->misc.imp[i].end_tick = m->tick; }
		if (m->stream && gain > 0.001f) stream_play_sound(m->stream, slot, tag, (int)variant, 0, gain, pan, -1.0f);
	} else {                                                         /* resident (SPU2) sound: length of its longest permutation */
		if (ni >= 0) {
			for (i = 0; i < snd.perm_count; i++) {
				snd_perm pm = snd_get_perm(sn, &snd, i);
				uint32_t t = frames_to_ticks(pm.frames, pm.rate);
				if (t > dur) dur = t;
			}
			if (!dur && snd.max_play_ms) dur = (snd.max_play_ms * 30u + 999u) / 1000u;
		}
		if (m->misc_env.play) {
			float gain, pan;
			spatial(m, obj, snd.min_dist, snd.max_dist, scale, &gain, &pan);
			if (gain > 0.001f) m->misc_env.play(m->misc_env.user, tag, obj >= 0 ? pos : NULL, gain);
		}
	}
	im = imp_alloc(m, tag);
	im->used = 1; im->tag = tag; im->obj = (int16_t)obj; im->slot = (int8_t)slot; im->end_tick = m->tick + dur;
	m->misc.impulses_started++;
	return 0;
}

BI(sound_impulse_time)
{
	ENTER(sound_impulse_time);
	ret_int(r, m ? (int32_t)misc_impulse_ticks(m, argtag(a, n, 0)) : 0);
	return 0;
}

BI(sound_impulse_stop)
{
	uint32_t tag = argtag(a, n, 0);
	unsigned i;
	ENTER(sound_impulse_stop);
	if (!m) return 0;
	for (i = 0; i < MISC_IMPULSES; i++) {
		misc_impulse *p = &m->misc.imp[i];
		if (!p->used || p->tag != tag) continue;
		if (p->slot >= 0 && m->stream && p->end_tick > m->tick) stream_stop(m->stream, p->slot, -1.0f);
		p->slot = -1;
		p->end_tick = m->tick;
	}
	return 0;
}

NOOP(sound_impulse_predict)
NOOP(sound_looping_predict)

static misc_loop *loop_find(mission_t *m, uint32_t tag)
{
	unsigned i;
	for (i = 0; i < MISC_LOOPS; i++) if (m->misc.loops[i].used && m->misc.loops[i].tag == tag) return &m->misc.loops[i];
	return NULL;
}

/* The stream slot a sound_looping tag plays in, -1 when it has no streamed track (a resident loop: goes through the hook). */
static int loop_route(const mission_t *m, uint32_t tag)
{
	const strm_section *ss = strm_of(m);
	int li, si;
	strm_loop lp;
	strm_track tr;
	uint32_t snd_tag;
	if (!ss) return -1;
	li = strm_find_loop(ss, tag);
	if (li < 0) return -1;
	lp = strm_get_loop(ss, (uint32_t)li);
	if (!lp.track_count) return -1;
	tr = strm_get_track(ss, &lp, 0);
	snd_tag = tr.loop != STRM_NO_TAG ? tr.loop : tr.start;
	si = strm_find(ss, snd_tag);
	if (si < 0) return -1;
	if (m->misc_env.snd) {
		int ni = snd_find(m->misc_env.snd, snd_tag);
		if (ni >= 0) { snd_sound s = snd_get_sound(m->misc_env.snd, (uint32_t)ni); if (snd_resident(&s)) return -1; }
	}
	return kind_slot(strm_get_sound(ss, (uint32_t)si).kind == STRM_KIND_MUSIC ? STRM_KIND_MUSIC : STRM_KIND_AMBIENCE);
}

BI(sound_looping_start)
{
	uint32_t tag = argtag(a, n, 0);
	float scale = argf(a, n, 2, 1.0f), pos[3] = { 0.0f, 0.0f, 0.0f };
	int obj, slot;
	unsigned i;
	misc_loop *lp;
	ENTER(sound_looping_start);
	if (!m || tag == HS_HANDLE_NONE || tag == 0) return 0;
	obj = n > 1 ? mission_resolve_object(m, &a[1], 0) : -1;
	if (obj == m->player_obj) obj = -1;
	if (obj >= 0) mission_object_pos(m, obj, pos);
	lp = loop_find(m, tag);
	if (!lp) {
		for (i = 0; i < MISC_LOOPS && m->misc.loops[i].used; i++) {}
		if (i == MISC_LOOPS) {                                       /* full: drop a stopped one, else the first */
			for (i = 0; i < MISC_LOOPS && m->misc.loops[i].running; i++) {}
			if (i == MISC_LOOPS) i = 0;
		}
		lp = &m->misc.loops[i];
		memset(lp, 0, sizeof *lp);
	}
	slot = loop_route(m, tag);
	if (slot >= 0) {
		unsigned group = slot == STREAM_SLOT_MUSIC ? MISC_GAIN_MUSIC : MISC_GAIN_AMBIENCE;
		for (i = 0; i < MISC_LOOPS; i++)                             /* one voice per slot: its previous loop stops */
			if (m->misc.loops[i].used && m->misc.loops[i].slot == slot && m->misc.loops[i].tag != tag) m->misc.loops[i].running = 0;
		if (m->stream) {
			if (stream_play_loop(m->stream, slot, tag, 0, scale * class_gain(m, group), 0.0f, -1.0f) != 0) slot = -1;
			else if (lp->alt) stream_loop_alternate(m->stream, slot, 1);
		}
	}
	if (slot < 0 && m->misc_env.loop_start) m->misc_env.loop_start(m->misc_env.user, tag, obj >= 0 ? pos : NULL, scale);
	lp->used = 1; lp->tag = tag; lp->slot = (int8_t)slot; lp->running = 1;
	m->misc.loops_started++;
	return 0;
}

BI(sound_looping_stop)
{
	uint32_t tag = argtag(a, n, 0);
	misc_loop *lp;
	ENTER(sound_looping_stop);
	if (!m || !(lp = loop_find(m, tag)) || !lp->running) return 0;
	if (lp->slot >= 0) { if (m->stream) stream_stop(m->stream, lp->slot, -1.0f); }
	else if (m->misc_env.loop_stop) m->misc_env.loop_stop(m->misc_env.user, tag);
	lp->running = 0;
	return 0;
}

BI(sound_looping_set_alternate)
{
	uint32_t tag = argtag(a, n, 0);
	misc_loop *lp;
	unsigned i;
	ENTER(sound_looping_set_alternate);
	if (!m || tag == HS_HANDLE_NONE) return 0;
	lp = loop_find(m, tag);
	if (!lp) {                                                       /* set before the start: remember it */
		for (i = 0; i < MISC_LOOPS && m->misc.loops[i].used; i++) {}
		if (i == MISC_LOOPS) return 0;
		lp = &m->misc.loops[i];
		memset(lp, 0, sizeof *lp);
		lp->used = 1; lp->tag = tag; lp->slot = -1;
	}
	lp->alt = (uint8_t)argb(a, n, 1, 0);
	if (lp->running && lp->slot >= 0 && m->stream) stream_loop_alternate(m->stream, lp->slot, lp->alt);
	return 0;
}

BI(sound_class_set_gain)
{
	const char *name = hm && hm->sec && n > 0 ? hs_string(hm->sec, a[0].v.i) : "";
	float g = argf(a, n, 1, 1.0f);
	uint32_t ticks = (uint32_t)(argi(a, n, 2, 0) > 0 ? argi(a, n, 2, 0) : 0);
	unsigned grp = MISC_GAIN_OTHER;
	ENTER(sound_class_set_gain);
	if (!m) return 0;
	if (g < 0.0f) g = 0.0f;
	if (g > 4.0f) g = 4.0f;
	if (strstr(name, "music")) grp = MISC_GAIN_MUSIC;
	else if (strstr(name, "ambient")) grp = MISC_GAIN_AMBIENCE;
	else if (strstr(name, "dialog")) grp = MISC_GAIN_DIALOGUE;
	m->misc.class_gain[grp] = g;
	m->misc.class_gain_set[grp] = 1;
	if (m->stream && grp != MISC_GAIN_OTHER) {
		int slot = grp == MISC_GAIN_MUSIC ? STREAM_SLOT_MUSIC : grp == MISC_GAIN_AMBIENCE ? STREAM_SLOT_AMBIENCE : STREAM_SLOT_DIALOGUE;
		if (stream_slot_busy(m->stream, slot)) stream_set_gain(m->stream, slot, g, (float)ticks / 30.0f);
	}
	return 0;
}

/* ---- hud / ui -------------------------------------------------------------------------------------------------------------------------- */
/* Text of message `idx` of the level's message set (the constant of a `hud message` argument is the index into that set): the text segments
 * concatenated, an icon segment as the private-use unit U+E000 + icon. Returns the length in units or HUDDEF_STR_NONE. */
static uint32_t hud_message_text(const mission_t *m, uint32_t idx, uint16_t *out, uint32_t cap)
{
	huddef_globals g;
	int set;
	uint32_t k, nseg, pos = 0;
	if (!m->strings || !m->huddef || !m->strings->data) return HUDDEF_STR_NONE;
	g = huddef_get_globals(m->huddef);
	set = huddef_find_set(m->strings, g.level_messages);
	if (set < 0 || idx >= huddef_set_size(m->strings, (uint32_t)set)) return HUDDEF_STR_NONE;
	nseg = huddef_segment_count(m->strings, (uint32_t)set, idx);
	for (k = 0; k < nseg && pos < cap; k++) {
		huddef_segment seg;
		uint16_t tmp[CINE_TEXT_UNITS + 1];
		uint32_t len, j;
		if (!huddef_get_segment(m->strings, (uint32_t)set, idx, k, &seg, tmp, CINE_TEXT_UNITS + 1)) continue;
		if (seg.kind == HUDDEF_SEG_ICON) { out[pos++] = (uint16_t)(0xE000u + seg.icon); continue; }
		len = seg.length < CINE_TEXT_UNITS ? seg.length : CINE_TEXT_UNITS;
		for (j = 0; j < len && pos < cap; j++) out[pos++] = tmp[j];
	}
	return pos;
}

BI(show_hud_help_text) { ENTER(show_hud_help_text); if (m && m->cine) cine_msg_set_visible(m->cine, CINE_MSG_HELP, argb(a, n, 0, 1)); return 0; }
BI(enable_hud_help_flash) { ENTER(enable_hud_help_flash); if (m) m->misc.help_flash = (uint8_t)argb(a, n, 0, 0); return 0; }

BI(hud_set_help_text)
{
	uint16_t t[CINE_TEXT_UNITS];
	int64_t idx = argh(a, n, 0);
	uint32_t units;
	ENTER(hud_set_help_text);
	if (!m || !m->cine || idx < 0) return 0;
	units = hud_message_text(m, (uint32_t)idx, t, CINE_TEXT_UNITS);
	if (units == HUDDEF_STR_NONE) { mission_note_missing(m, "hud_set_help_text"); return 0; }
	cine_msg_post(m->cine, CINE_MSG_HELP, t, units, 0);
	m->misc.help_shown++;
	m->misc.help_msg1 = (int16_t)(idx + 1 > 32767 ? 32767 : idx + 1);
	return 0;
}

BI(hud_set_objective_text)
{
	uint16_t t[CINE_TEXT_UNITS];
	int64_t idx = argh(a, n, 0);
	uint32_t units;
	ENTER(hud_set_objective_text);
	if (!m || !m->cine || idx < 0) return 0;
	units = hud_message_text(m, (uint32_t)idx, t, CINE_TEXT_UNITS);
	if (units == HUDDEF_STR_NONE) { mission_note_missing(m, "hud_set_objective_text"); return 0; }
	cine_msg_post(m->cine, CINE_MSG_OBJECTIVE, t, units, 0);
	m->misc.objectives_shown++;
	m->misc.objective_msg1 = (int16_t)(idx + 1 > 32767 ? 32767 : idx + 1);
	if (idx < 32) m->objective_flags |= 1u << idx;
	return 0;
}

BI(display_scenario_help)
{
	uint16_t t[CINE_TEXT_UNITS + 1];
	int32_t idx = argi(a, n, 0, -1);
	uint32_t units;
	ENTER(display_scenario_help);
	if (!m || !m->cine || !m->cin || idx < 0) return 0;
	units = cinedef_get_string(m->cin, (uint32_t)idx, t, CINE_TEXT_UNITS + 1);
	if (units == 0xFFFFFFFFu) { mission_note_missing(m, "display_scenario_help"); return 0; }
	if (units > CINE_TEXT_UNITS) units = CINE_TEXT_UNITS;
	cine_msg_post(m->cine, CINE_MSG_SCENARIO_HELP, t, units, CINE_SCENARIO_HELP_TICKS);
	m->misc.scenario_help_shown++;
	return 0;
}

static void set_hud_bit(mission_t *m, uint32_t bit, int shown) { if (shown) m->misc.hud_hidden &= ~bit; else m->misc.hud_hidden |= bit; }
static void set_blink_bit(mission_t *m, uint32_t bit, int on) { if (on) m->misc.hud_blink |= bit; else m->misc.hud_blink &= ~bit; }
BI(show_hud) { ENTER(show_hud); if (m) set_hud_bit(m, MISC_HUD_ALL, argb(a, n, 0, 1)); return 0; }
BI(hud_show_motion_sensor) { ENTER(hud_show_motion_sensor); if (m) set_hud_bit(m, MISC_HUD_MOTION, argb(a, n, 0, 1)); return 0; }
BI(hud_show_crosshair) { ENTER(hud_show_crosshair); if (m) set_hud_bit(m, MISC_HUD_CROSSHAIR, argb(a, n, 0, 1)); return 0; }
BI(hud_show_health) { ENTER(hud_show_health); if (m) set_hud_bit(m, MISC_HUD_HEALTH, argb(a, n, 0, 1)); return 0; }
BI(hud_show_shield) { ENTER(hud_show_shield); if (m) set_hud_bit(m, MISC_HUD_SHIELD, argb(a, n, 0, 1)); return 0; }
BI(hud_blink_health) { ENTER(hud_blink_health); if (m) set_blink_bit(m, MISC_HUD_HEALTH, argb(a, n, 0, 0)); return 0; }
BI(hud_blink_motion_sensor) { ENTER(hud_blink_motion_sensor); if (m) set_blink_bit(m, MISC_HUD_MOTION, argb(a, n, 0, 0)); return 0; }
BI(hud_blink_shield) { ENTER(hud_blink_shield); if (m) set_blink_bit(m, MISC_HUD_SHIELD, argb(a, n, 0, 0)); return 0; }

/* The countdown timer: set by hud_set_timer_time (minutes, seconds), runs down by itself, pause_hud_timer freezes it. */
uint32_t misc_timer_ticks(const mission_t *m)
{
	if (!m->misc.timer_set) return 0;
	if (m->misc.timer_paused) return m->misc.timer_left;
	return m->misc.timer_end > m->tick ? m->misc.timer_end - m->tick : 0;
}
BI(show_hud_timer) { ENTER(show_hud_timer); if (m) m->misc.timer_shown = (uint8_t)argb(a, n, 0, 0); return 0; }
BI(pause_hud_timer)
{
	int p = argb(a, n, 0, 0);
	ENTER(pause_hud_timer);
	if (!m || !m->misc.timer_set) return 0;
	if (p && !m->misc.timer_paused) { m->misc.timer_left = misc_timer_ticks(m); m->misc.timer_paused = 1; }
	else if (!p && m->misc.timer_paused) { m->misc.timer_end = m->tick + m->misc.timer_left; m->misc.timer_paused = 0; }
	return 0;
}
BI(hud_get_timer_ticks) { ENTER(hud_get_timer_ticks); ret_int(r, m ? (int32_t)misc_timer_ticks(m) : 0); return 0; }
BI(hud_set_timer_position)
{
	ENTER(hud_set_timer_position);
	if (m) { m->misc.timer_x = (int16_t)argi(a, n, 0, 0); m->misc.timer_y = (int16_t)argi(a, n, 1, 0); }
	return 0;
}
BI(hud_set_timer_time)
{
	uint32_t t = ticks_of_min_sec(argi(a, n, 0, 0), argi(a, n, 1, 0));
	ENTER(hud_set_timer_time);
	if (!m) return 0;
	m->misc.timer_set = 1;
	m->misc.timer_left = t;
	m->misc.timer_end = m->tick + t;
	return 0;
}
BI(hud_set_timer_warning_time)
{
	ENTER(hud_set_timer_warning_time);
	if (m) m->misc.timer_warning = ticks_of_min_sec(argi(a, n, 0, 0), argi(a, n, 1, 0));
	return 0;
}
BI(numeric_countdown_timer_set)
{
	int32_t t = argi(a, n, 0, 0);
	ENTER(numeric_countdown_timer_set);
	if (!m) return 0;
	m->misc.ncd_end = m->tick + (uint32_t)(t > 0 ? t : 0);
	m->misc.ncd_active = (uint8_t)argb(a, n, 1, 1);
	return 0;
}
BI(numeric_countdown_timer_stop) { ENTER(numeric_countdown_timer_stop); if (m) m->misc.ncd_active = 0; return 0; }
BI(time_code_show) { ENTER(time_code_show); if (m) m->misc.time_code_shown = (uint8_t)argb(a, n, 0, 0); return 0; }
BI(time_code_start) { ENTER(time_code_start); if (m) m->misc.time_code_running = (uint8_t)argb(a, n, 0, 0); return 0; }

/* ---- navigation points -------------------------------------------------------------------------------------------------------------------- */
static void nav_resolve(const mission_t *m, misc_nav *e)
{
	if (e->kind == MISC_NAV_FLAG) {
		devdef_cflag f;
		if (!m->dev || e->ref < 0 || (uint32_t)e->ref >= m->dev->flag_count) { e->live = 0; return; }
		f = devdef_get_cflag(m->dev, (uint32_t)e->ref);
		e->pos[0] = f.pos[0]; e->pos[1] = f.pos[1]; e->pos[2] = f.pos[2] + e->height;
		e->live = 1;
	} else if (e->kind == MISC_NAV_OBJECT) {
		if (!mission_object_live(m, e->ref)) { e->live = 0; return; }
		mission_object_pos(m, e->ref, e->pos);
		e->pos[2] += e->height;
		e->live = 1;
	}
}

void misc_nav_refresh(mission_t *m)
{
	unsigned i;
	for (i = 0; i < MISC_NAVPOINTS; i++) if (m->misc.nav[i].active) nav_resolve(m, &m->misc.nav[i]);
}

static void nav_activate(mission_t *m, uint8_t kind, int team, int type, int32_t ref, float height)
{
	unsigned i, slot = MISC_NAVPOINTS, oldest = 0;
	misc_nav *e;
	for (i = 0; i < MISC_NAVPOINTS; i++) {
		const misc_nav *p = &m->misc.nav[i];
		if (p->active && p->kind == kind && p->ref == ref && p->team == (uint8_t)team) { slot = i; break; }
	}
	if (slot == MISC_NAVPOINTS)
		for (i = 0; i < MISC_NAVPOINTS; i++) if (!m->misc.nav[i].active) { slot = i; break; }
	if (slot == MISC_NAVPOINTS) {                                    /* full: the oldest activation goes */
		for (i = 1; i < MISC_NAVPOINTS; i++) if (m->misc.nav[i].serial < m->misc.nav[oldest].serial) oldest = i;
		slot = oldest;
	}
	e = &m->misc.nav[slot];
	memset(e, 0, sizeof *e);
	e->active = 1; e->kind = kind; e->team = (uint8_t)team; e->type = (uint8_t)type; e->ref = ref; e->height = height;
	e->tick = m->tick; e->serial = ++m->misc.nav_serial;
	nav_resolve(m, e);
}

static void nav_deactivate(mission_t *m, uint8_t kind, int team, int32_t ref)
{
	unsigned i;
	for (i = 0; i < MISC_NAVPOINTS; i++) {
		misc_nav *p = &m->misc.nav[i];
		if (p->active && p->kind == kind && p->ref == ref && p->team == (uint8_t)team) memset(p, 0, sizeof *p);
	}
}

BI(activate_team_nav_point_flag)
{
	int64_t flag = argh(a, n, 2);
	ENTER(activate_team_nav_point_flag);
	if (!m || n < 3 || flag < 0 || !m->dev || (uint64_t)flag >= m->dev->flag_count) { if (m) mission_note_missing(m, "activate_team_nav_point_flag"); return 0; }
	nav_activate(m, MISC_NAV_FLAG, argi(a, n, 1, 0), argi(a, n, 0, 0), (int32_t)flag, argf(a, n, 3, 0.0f));
	return 0;
}
BI(deactivate_team_nav_point_flag)
{
	int64_t flag = argh(a, n, 1);
	ENTER(deactivate_team_nav_point_flag);
	if (m && flag >= 0) nav_deactivate(m, MISC_NAV_FLAG, argi(a, n, 0, 0), (int32_t)flag);
	return 0;
}
BI(activate_team_nav_point_object)
{
	int obj;
	ENTER(activate_team_nav_point_object);
	if (!m || n < 3) return 0;
	obj = mission_resolve_object(m, &a[2], 0);
	if (obj < 0) { mission_note_missing(m, "activate_team_nav_point_object"); return 0; }
	nav_activate(m, MISC_NAV_OBJECT, argi(a, n, 1, 0), argi(a, n, 0, 0), obj, argf(a, n, 3, 0.0f));
	return 0;
}
BI(deactivate_team_nav_point_object)
{
	int obj;
	ENTER(deactivate_team_nav_point_object);
	if (!m || n < 2) return 0;
	obj = mission_resolve_object(m, &a[1], 0);
	if (obj >= 0) nav_deactivate(m, MISC_NAV_OBJECT, argi(a, n, 0, 0), obj);
	return 0;
}

unsigned misc_nav_count(const mission_t *m)
{
	unsigned i, c = 0;
	for (i = 0; i < MISC_NAVPOINTS; i++) c += m->misc.nav[i].active;
	return c;
}

const misc_nav *misc_nav_get(const mission_t *m, unsigned k)
{
	unsigned i, c = 0;
	for (i = 0; i < MISC_NAVPOINTS; i++)
		if (m->misc.nav[i].active) { if (c == k) return &m->misc.nav[i]; c++; }
	return NULL;
}

int mission_navpoint_target(mission_t *m, float out_pos[3])
{
	const misc_nav *best = NULL;
	unsigned i;
	misc_nav_refresh(m);
	for (i = 0; i < MISC_NAVPOINTS; i++) {
		const misc_nav *e = &m->misc.nav[i];
		if (e->active && e->live && (!best || e->serial > best->serial)) best = e;
	}
	if (!best) return 0;
	out_pos[0] = best->pos[0]; out_pos[1] = best->pos[1]; out_pos[2] = best->pos[2];
	return 1;
}

/* ---- game flow --------------------------------------------------------------------------------------------------------------------------- */
static void request_save(mission_t *m, uint32_t bits)
{
	m->save_requested |= bits;
	m->misc.save_tick = m->tick;
}
BI(game_save) { ENTER(game_save); if (m) request_save(m, MISC_SAVE_NORMAL); return 0; }
BI(game_save_no_timeout) { ENTER(game_save_no_timeout); if (m) request_save(m, MISC_SAVE_NO_TIMEOUT); return 0; }
BI(game_save_totally_unsafe) { ENTER(game_save_totally_unsafe); if (m) request_save(m, MISC_SAVE_UNSAFE); return 0; }
BI(game_save_cancel) { ENTER(game_save_cancel); if (m) m->save_requested = 0; return 0; }
BI(game_saving)
{
	int saving = 0;
	ENTER(game_saving);
	if (m && m->save_requested) {
		if (m->tick < m->misc.save_tick + MISC_SAVE_TICKS) saving = 1;
		else m->save_requested = 0;          /* nobody served it: the request lapses, the script's wait ends */
	}
	ret_bool(r, saving);
	return 0;
}
BI(game_safe_to_save)
{
	ENTER(game_safe_to_save);
	ret_bool(r, m && !m->cinematic && !m->lost && !m->won && !(m->game && m->game->dead_ticks));
	return 0;
}
BI(game_all_quiet) { ENTER(game_all_quiet); ret_bool(r, 1); return 0; }
BI(game_safe_to_speak)
{
	unsigned i, busy = 0;
	ENTER(game_safe_to_speak);
	if (m)
		for (i = 0; i < MISC_IMPULSES; i++)
			if (m->misc.imp[i].used && m->misc.imp[i].slot == STREAM_SLOT_DIALOGUE && m->misc.imp[i].end_tick > m->tick) busy = 1;
	ret_bool(r, !busy);
	return 0;
}
BI(game_difficulty_get) { ENTER(game_difficulty_get); ret_int(r, m ? m->difficulty : 0); return 0; }
BI(game_difficulty_get_real) { ENTER(game_difficulty_get_real); ret_int(r, m ? m->difficulty : 0); return 0; }
BI(game_is_cooperative) { ENTER(game_is_cooperative); ret_bool(r, 0); return 0; }
BI(game_won) { ENTER(game_won); if (m) m->won = 1; return 0; }
BI(game_lost) { ENTER(game_lost); if (m) m->lost = 1; return 0; }
BI(game_revert) { ENTER(game_revert); if (m) m->misc.revert_requested = 1; return 0; }
BI(game_reverted) { ENTER(game_reverted); ret_bool(r, m && m->misc.reverted); return 0; }
BI(game_time) { ENTER(game_time); ret_int(r, m ? (int32_t)(m->tick & 0x7FFFFFFFu) : 0); return 0; }
BI(game_speed) { ENTER(game_speed); if (m) m->misc.game_speed = argf(a, n, 0, 1.0f); return 0; }
BI(game_skip_ticks)
{
	int32_t t = argi(a, n, 0, 0);
	ENTER(game_skip_ticks);
	if (m && t > 0) m->misc.skip_ticks += (uint32_t)t;
	return 0;
}
static void do_switch_bsp(mission_t *m, int32_t i)
{
	if (i < 0 || (m->dev && m->dev->bsp_count && (uint32_t)i >= m->dev->bsp_count) || i == m->cur_bsp) return;
	m->want_bsp = i;                         /* main performs it between ticks; the player keeps its position (the BSPs overlap at the switch volume) */
	m->want_teleport = 0;
}
BI(switch_bsp) { ENTER(switch_bsp); if (m) do_switch_bsp(m, argi(a, n, 0, -1)); return 0; }
BI(scenario_switch_bsp) { ENTER(scenario_switch_bsp); if (m) do_switch_bsp(m, argi(a, n, 0, -1)); return 0; }
BI(structure_bsp_index) { ENTER(structure_bsp_index); ret_int(r, m ? m->cur_bsp : 0); return 0; }

/* ---- scripted player effect --------------------------------------------------------------------------------------------------------------- */
static float pfx_level(const mission_misc_t *x, uint32_t tick)
{
	if (!x->pfx_active) return 0.0f;
	if (!x->pfx_stopping) {
		float l = 1.0f;
		if (x->pfx_attack && tick < x->pfx_start + x->pfx_attack) l = (float)(tick - x->pfx_start) / (float)x->pfx_attack;
		return l * x->pfx_intensity;
	}
	if (!x->pfx_decay || tick >= x->pfx_stop + x->pfx_decay) return 0.0f;
	return x->pfx_stop_level * (1.0f - (float)(tick - x->pfx_stop) / (float)x->pfx_decay);
}

float misc_player_effect_get(const mission_t *m, float rot[3], float trans[3], float rumble[2])
{
	float l = pfx_level(&m->misc, m->tick);
	unsigned i;
	for (i = 0; i < 3; i++) { rot[i] = m->misc.pfx_rot[i] * l; trans[i] = m->misc.pfx_trans[i] * l; }
	rumble[0] = m->misc.pfx_rumble[0] * l; rumble[1] = m->misc.pfx_rumble[1] * l;
	return l;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
BI(player_effect_set_max_translation)
{
	unsigned i;
	ENTER(player_effect_set_max_translation);
	if (m) for (i = 0; i < 3; i++) m->misc.pfx_trans[i] = clampf(argf(a, n, i, 0.0f), 0.0f, 10.0f);
	return 0;
}
BI(player_effect_set_max_rotation)         /* the arguments are degrees: kept in radians */
{
	unsigned i;
	ENTER(player_effect_set_max_rotation);
	if (m) for (i = 0; i < 3; i++) m->misc.pfx_rot[i] = clampf(argf(a, n, i, 0.0f), 0.0f, 90.0f) * (MISC_PI / 180.0f);
	return 0;
}
BI(player_effect_set_max_rumble)
{
	ENTER(player_effect_set_max_rumble);
	if (m) { m->misc.pfx_rumble[0] = clampf(argf(a, n, 0, 0.0f), 0.0f, 1.0f); m->misc.pfx_rumble[1] = clampf(argf(a, n, 1, 0.0f), 0.0f, 1.0f); }
	return 0;
}
BI(player_effect_start)
{
	ENTER(player_effect_start);
	if (!m) return 0;
	m->misc.pfx_intensity = clampf(argf(a, n, 0, 1.0f), 0.0f, 4.0f);
	m->misc.pfx_attack = ticks_of_seconds(argf(a, n, 1, 0.0f));
	m->misc.pfx_start = m->tick;
	m->misc.pfx_active = 1;
	m->misc.pfx_stopping = 0;
	if (m->game) {                           /* an immediate kick through the camera shake the game already renders (it decays by itself) */
		float k = (m->misc.pfx_rot[0] > m->misc.pfx_rot[1] ? m->misc.pfx_rot[0] : m->misc.pfx_rot[1]) * m->misc.pfx_intensity;
		if (k > 0.1f) k = 0.1f;
		if (k > m->game->shake) m->game->shake = k;
	}
	return 0;
}
BI(player_effect_stop)
{
	ENTER(player_effect_stop);
	if (!m || !m->misc.pfx_active || m->misc.pfx_stopping) return 0;
	m->misc.pfx_stop_level = pfx_level(&m->misc, m->tick);
	m->misc.pfx_stopping = 1;
	m->misc.pfx_stop = m->tick;
	m->misc.pfx_decay = ticks_of_seconds(argf(a, n, 0, 0.0f));
	return 0;
}

/* ---- debug / hints: explicit no-ops (void results) ---------------------------------------------------------------------------------------- */
NOOP(objects_predict)
NOOP(object_beautify)
NOOP(object_pvs_activate)
NOOP(object_type_predict)
NOOP(rasterizer_model_ambient_reflection_tint)
NOOP(garbage_collect_now)
NOOP(object_pvs_set_camera)
NOOP(breakable_surfaces_enable)
NOOP(cls)
NOOP(breakable_surfaces_reset)
NOOP(object_pvs_clear)
NOOP(object_pvs_set_object)
NOOP(render_lights)
NOOP(rasterizer_lights_reset_for_new_map)

/* ---- per tick service, registration, statistics ------------------------------------------------------------------------------------------ */
void misc_tick(mission_t *m)
{
	if (!m) return;
	refresh_player_pose(m);
	if (m->misc.cam_anchor_obj1 > 0 && m->cine) {
		cine_frame f;
		if (object_frame(m, m->misc.cam_anchor_obj1 - 1, &f)) cine_cam_set_anchor(&m->cine->cam, &f);
	}
	misc_nav_refresh(m);
}

void hs_host_register_misc(hs_machine *m)
{
#define X(nm) hs_register(m, #nm, bi_##nm);
	MISC_LIST(X)
#undef X
}

unsigned hs_host_misc_stats(unsigned k, const char **name, uint32_t *calls)
{
	if (k < MISC_COUNT) {
		if (name) *name = k_names[k];
		if (calls) *calls = k_calls[k];
	}
	return MISC_COUNT;
}

void hs_host_misc_reset_stats(void) { memset(k_calls, 0, sizeof k_calls); }
