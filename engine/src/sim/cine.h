/* Cinematics and camera runtime (docs/28-cine-section.md). Pure C11, no heap, no double, no platform dependency: host-tested.
 * Everything advances in 30 Hz ticks driven by the host (`cine_tick` once per game tick, last in the tick order: script
 * machine -> devices -> AI -> player -> weapons -> cinematics) and is deterministic: the same calls give the same state.
 *
 * Parts, one section each below:
 *   1. recorded animation playback (`recording_play` and friends): time -> the control state of a unit and the events of
 *      that tick; the HOST feeds the control state to a unit (game_input_t for the player biped, the AI unit controller
 *      for the rest). Recordings carry no positions: the unit's own physics makes the motion.
 *   2. camera track sampler (the unit chase camera: control points -> camera pose from the look pitch);
 *   3. the cinematic camera (`camera_control`, `camera_set`, `camera_set_relative`, `camera_time`, field of view);
 *   4. screen state (`cinematic_start/stop`, letterbox, `fade_in` / `fade_out`, skip flags);
 *   5. a timed title queue (`cinematic_set_title`) and a message queue (help, objective, scenario help, subtitles) the HUD draws.
 *
 * Conventions are Halo's, as render/camera.h: world +z up, yaw counter-clockwise seen from above from +x, positive pitch looks
 * up, +y is left; angles in radians; a roll is positive when the camera banks to the right (matrix Rz(yaw) Ry(-pitch) Rx(roll)).
 * `fov` is the horizontal field of view in radians like camera_t.hfov (the cutscene camera points of the maps hold 70 degrees
 * = 1.2217 there; whether the Xbox value is horizontal or vertical is unverified).
 *
 * Constants the engine needs that no tag carries (verified: none of the tags read for this section has a fade, letterbox or
 * transition field; the colour and time of a fade are arguments of the script call) are defined here and are UNVERIFIED
 * choices (marked): letterbox size and speed, default easing, message durations. */
#ifndef SIM_CINE_H
#define SIM_CINE_H
#include "render/camera.h"
#include "world/cinedef.h"
#include <stdint.h>

#define CINE_TICKS_PER_SECOND 30
#define CINE_COUNT_RAD 0.00314159265f        /* one aim count in radians (pi / 1000) */
#define CINE_DEFAULT_FOV 1.2217305f          /* 70 degrees */
#define CINE_LETTERBOX_FRACTION 0.125f       /* UNVERIFIED: each bar covers this fraction of the screen height (16:9 window on 4:3) */
#define CINE_LETTERBOX_TICKS 15              /* UNVERIFIED: bars slide in or out in this many ticks */
#define CINE_TITLE_SLOTS 4
#define CINE_MSG_SLOTS 8
#define CINE_TEXT_UNITS 128                  /* longest title / message kept, UTF-16 units (longer text is cut) */
#define CINE_MAX_PLAYS 32
#define CINE_SCENARIO_HELP_TICKS 300         /* UNVERIFIED default duration of display_scenario_help text (10 s) */

/* ---- 1. recorded animation playback ------------------------------------------------------------------------------- */
enum { CINE_CH_FLAGS = 1, CINE_CH_THROTTLE = 2, CINE_CH_AIM = 4, CINE_CH_WEAPON = 8, CINE_CH_MODE = 16, CINE_CH_ALL = 31 };

typedef struct {
	const cinedef_section *sec;
	uint32_t    rec;                 /* record index */
	const uint8_t *stream;
	uint32_t    bytes, pos;          /* the stream and the offset of the next event */
	uint32_t    length;              /* ticks of the recording */
	uint32_t    tick;                /* ticks played so far (recording_time); state is that of tick min(tick, length - 1) */
	uint32_t    next_time;           /* tick at which the event at `pos` applies */
	uint32_t    changed;             /* CINE_CH_* bits touched by the last start / tick / seek */
	uint8_t     done;                /* tick >= length: the recording has run out (the state holds its last value) */
	cinedef_ctl ctl;
} cine_play;

/* Starts record `rec` of `sec` at tick 0 (the events of tick 0 are applied). Returns 0 for a bad index. */
int      cine_play_start(cine_play *p, const cinedef_section *sec, uint32_t rec);
/* Advances one tick; once `tick` reaches `length` the playback is done and the state stays. */
void     cine_play_tick(cine_play *p);
/* Jumps to `tick` (clamped to `length`) replaying from the start; `changed` = CINE_CH_ALL. */
void     cine_play_seek(cine_play *p, uint32_t tick);
/* Aim as radians / unit vector (cos p cos y, cos p sin y, sin p). */
float    cine_ctl_yaw(const cinedef_ctl *c);
float    cine_ctl_pitch(const cinedef_ctl *c);
void     cine_ctl_aim_vector(const cinedef_ctl *c, float out[3]);

/* ---- 2. camera track sampler ---------------------------------------------------------------------------------------- */
typedef struct { float pos[3]; float quat[4]; } cine_track_pose;       /* in the unit's frame (x forward, y left, z up) */

/* Pose at parameter t in [0, 1] over the control points (uniform; clamped): Catmull-Rom through the positions with the end
 * points repeated, normalised lerp of the quaternions along the shortest arc. Interpolation is UNVERIFIED (the original is
 * not documented; the control points lie on an arc of 22.5 degree steps, where any smooth choice agrees within a few
 * percent of the radius). Returns 0 for a bad track. */
int   cine_track_sample(const cinedef_section *s, uint32_t track, float t, cine_track_pose *out);
/* Look pitch (radians, positive up) of a control point. */
float cine_track_point_pitch(const cinedef_section *s, uint32_t track, uint32_t k);
/* Pose for a look pitch: the segment whose control point pitches bracket `pitch` (clamped to the first and last point). This is
 * how a chase camera follows the aim; it does not assume the points are evenly spaced (one track is not). */
int   cine_track_sample_pitch(const cinedef_section *s, uint32_t track, float pitch, cine_track_pose *out);

/* ---- 3. cinematic camera -------------------------------------------------------------------------------------------- */
typedef struct { float pos[3]; float yaw, pitch, roll, fov; } cine_pose;
typedef struct { float pos[3]; float yaw; } cine_frame;                /* the object a relative camera hangs on */

enum { CINE_CAM_PLAYER = 0, CINE_CAM_POSE, CINE_CAM_FIRST_PERSON, CINE_CAM_DEAD };
enum { CINE_EASE_LINEAR = 0, CINE_EASE_SMOOTH };                       /* accepted for compatibility; the observer ignores it (see cine_cam_set) */

typedef struct {
	uint8_t   control;               /* camera_control: scripts own the camera */
	uint8_t   mode;                  /* CINE_CAM_*: what the host should render from */
	uint8_t   relative, ease;
	uint8_t   fov_active;
	uint32_t  ticks, elapsed;        /* the running camera_set transition */
	uint32_t  fov_ticks, fov_elapsed;
	float     fov_from, fov_to;
	cine_pose from, to;              /* transition ends (relative to `anchor` when `relative`) */
	cine_pose cur;                   /* evaluated pose, world space */
	cine_pose rel_cur;               /* the same in the transition's space (relative to the anchor for a relative camera) */
	cine_pose player;                /* the player camera pose, kept up to date by the host (start of a transition) */
	cine_frame anchor;
	/* Halo's observer (XBE 0x11dd30): the state it keeps between updates, in the transition's space. The camera_set pass and every tick run obs_update. */
	uint8_t   os_valid, os_rel;      /* the state holds a pose; whether it is in the anchor's space */
	float     os_pos[3], os_fov;     /* position and field of view (the channels the scripted camera moves) */
	float     os_fwd[3], os_up[3];   /* orientation as forward and up vectors (the observer's state; rotated by a rotation vector each update) */
	float     os_v[7], os_a[7];      /* d/dt and d2/dt2 of the offset from the target (remaining time t) at the next step: position 0..2, fov 3, rotation vector 4..6 (0x2715c4 / 0x2715fc) */
	float     tgt_fwd[3], tgt_up[3]; /* the target orientation */
	float     rem;                   /* seconds left of the running camera_set, counted down like the director does (0x120a90) */
	float     near_clip;             /* cinematic_set_near_clip_distance, 0 = the engine's default */
} cine_camera;

void cine_cam_init(cine_camera *c);
/* The player's own camera pose, to be refreshed by the host every tick: the camera_set transition starts from it. */
void cine_cam_set_player_pose(cine_camera *c, const cine_pose *p);
/* camera_control on / off. On: the camera stays where it is (mode POSE holding the player's pose the first time); off: back to
 * the player camera. */
void cine_cam_control(cine_camera *c, int on);
/* camera_set: move to `to` in `ticks` ticks (0 = cut). Implies control on. The motion is Halo's observer, ported from the executable (XBE 0x11c500 .. 0x11cda0, ADR-084): a quintic from the
 * current pose to `to`, refitted every tick with the velocity and acceleration the camera has at that instant, so a retarget in mid flight continues the motion. */
void cine_cam_set(cine_camera *c, const cine_pose *to, uint32_t ticks, int ease);
/* camera_set_relative: `rel` is in the frame of an object (x forward of `anchor`); the host refreshes the anchor every tick with
 * cine_cam_set_anchor. Yaw is added to the anchor yaw, the position rotated by it about z (the object's pitch and roll are
 * ignored: UNVERIFIED approximation). */
void cine_cam_set_relative(cine_camera *c, const cine_pose *rel, uint32_t ticks, int ease, const cine_frame *anchor);
void cine_cam_set_anchor(cine_camera *c, const cine_frame *anchor);
void cine_cam_set_first_person(cine_camera *c);           /* camera_set_first_person: the host renders the unit's view */
void cine_cam_set_dead(cine_camera *c);                   /* camera_set_dead: the host renders the death camera */
/* Field of view change over `ticks` (0 = at once); lasts until the next camera_set. */
void cine_cam_set_fov(cine_camera *c, float fov, uint32_t ticks);
/* cinematic_set_near_clip_distance (world units, 0 = the engine's default); cine_cam_near_clip returns it. */
void cine_cam_set_near_clip(cine_camera *c, float dist);
void cine_cam_tick(cine_camera *c);
/* camera_time: ticks until the running transition arrives (0 = there). */
uint32_t cine_cam_time(const cine_camera *c);
/* Current pose in world space (the player pose when the camera is not controlled). */
cine_pose cine_cam_pose(const cine_camera *c);
/* Writes position, yaw, pitch and hfov into a render camera (roll cannot be expressed in camera_t; aspect and clip stay). */
void cine_pose_to_camera(const cine_pose *p, camera_t *cam);
/* Pose in a frame: world = anchor + Rz(anchor.yaw) rel, and the inverse. */
void cine_pose_compose(const cine_pose *rel, const cine_frame *anchor, cine_pose *world);
void cine_pose_relative(const cine_pose *world, const cine_frame *anchor, cine_pose *rel);
/* Pose from a track pose: `origin` the unit's camera origin, `yaw` the heading of the camera (the aim yaw for a chase camera). */
void cine_track_world(const cine_track_pose *tp, const float origin[3], float yaw, float fov, cine_pose *out);
/* Quaternion (x y z w, Hamilton, engine convention) helpers. */
void cine_ypr_to_quat(float yaw, float pitch, float roll, float q[4]);
void cine_quat_to_ypr(const float q[4], float *yaw, float *pitch, float *roll);
/* A scenario / camera-point euler triple (Halo's euler_angles3d_to_matrix, see core/mat.h) as the yaw, pitch and bank of camera_t: forward is its first row,
 * up its third. */
void cine_halo_euler_to_ypr(float yaw, float pitch, float roll, float *oy, float *op, float *orr);

/* ---- 4. screen state ------------------------------------------------------------------------------------------------- */
typedef struct {
	uint8_t  cinematic;              /* between cinematic_start and cinematic_stop: no HUD, no player control */
	uint8_t  skippable, skip_requested;
	uint8_t  letterbox_on;
	float    letterbox;              /* 0 .. 1 of CINE_LETTERBOX_FRACTION */
	uint32_t letterbox_pos;          /* 0 .. CINE_LETTERBOX_TICKS: the exact slide position */
	float    fade_rgb[3];            /* fade colour, 0 .. 1 */
	float    fade_alpha;             /* 0 clear .. 1 covered */
	float    fade_from, fade_to;
	uint32_t fade_ticks, fade_elapsed;
	uint8_t  effect;                 /* cinematic_screen_effect_start / _stop: the hologram look of the briefing (hud_render draws a teal wash and scan bars) */
	uint32_t effect_ticks;
	uint8_t  filter;                 /* cinematic_screen_effect_set_filter was called: the veil of the tutorial's pod (a pale wash standing in for the Xbox's blur and desaturation) */
	float    veil;                   /* 0 .. 1 its strength: lerp(filter_from, filter_to) over filter_ticks, advanced by misc_tick */
	float    filter_from, filter_to;
	uint32_t filter_ticks, filter_elapsed;
} cine_screen;

/* ---- 5. titles and messages ------------------------------------------------------------------------------------------ */
typedef struct {
	uint8_t       active;
	uint32_t      delay, elapsed;    /* ticks before it starts, ticks shown */
	cinedef_title def;               /* box, style, colours, fade times */
	uint16_t      units;
	uint16_t      text[CINE_TEXT_UNITS];
} cine_title;

enum { CINE_MSG_HELP = 0, CINE_MSG_OBJECTIVE, CINE_MSG_SCENARIO_HELP, CINE_MSG_SUBTITLE, CINE_MSG_KINDS };
typedef struct {
	uint8_t  active, kind;
	uint32_t elapsed, duration;      /* duration 0 = until replaced or cleared */
	uint16_t units;
	uint16_t text[CINE_TEXT_UNITS];
} cine_msg;

enum { CINE_PLAY_DELETE = 1, CINE_PLAY_HOVER = 2 };    /* recording_play_and_delete / _and_hover: the host's policy, carried here */
typedef struct { uint8_t used, flags; uint32_t owner; cine_play play; } cine_play_slot;

typedef struct {
	uint32_t    tick;
	cine_camera cam;
	cine_screen screen;
	cine_title  titles[CINE_TITLE_SLOTS];
	cine_msg    msgs[CINE_MSG_SLOTS];
	uint8_t     msg_visible[CINE_MSG_KINDS];
	cine_play_slot plays[CINE_MAX_PLAYS];
} cine_state;

void cine_init(cine_state *s);
/* One game tick: camera, letterbox, fade, titles, messages and every running recording advance. */
void cine_tick(cine_state *s);

/* cinematic_start / cinematic_stop, cinematic_skip_start_internal / _stop_internal (skippable section), player skip input */
void cine_start(cine_state *s);
void cine_stop(cine_state *s);
void cine_skip_begin(cine_state *s);
void cine_skip_end(cine_state *s);
void cine_skip_request(cine_state *s);                  /* the player pressed skip; honoured only inside a skippable section */
int  cine_skip_pending(const cine_state *s);            /* skip requested and the section is still skippable */
/* cinematic_show_letterbox */
void cine_letterbox(cine_state *s, int on);
/* Height in pixels of each letterbox bar on a screen of `screen_h` pixels. */
int  cine_letterbox_bar(const cine_state *s, int screen_h);
/* fade_out / fade_in with a colour (0 .. 1 per channel) over `ticks` ticks: alpha runs from its current value to 1 / 0. */
void cine_fade_out(cine_state *s, float r, float g, float b, uint32_t ticks);
void cine_fade_in(cine_state *s, float r, float g, float b, uint32_t ticks);

/* Title `index` of the section starts after `delay` ticks (cinematic_set_title / _delayed); the text is copied. Returns the slot
 * or -1 when the title does not exist. A full queue replaces the title that has been shown longest. */
int   cine_title_show(cine_state *s, const cinedef_section *sec, uint32_t index, uint32_t delay);
/* Any title (a definition and UTF-16 text of `units` units). */
int   cine_title_post(cine_state *s, const cinedef_title *def, const uint16_t *text, uint32_t units, uint32_t delay);
/* 0 .. 1 opacity of a title now: ramps up over fade_in, holds for `up`, ramps down over fade_out; 0 before its delay. */
float cine_title_alpha(const cine_title *t);
void  cine_title_clear(cine_state *s);

/* Posts a message (text copied, cut at CINE_TEXT_UNITS). Help and objective messages replace their predecessor of the same
 * kind; the other kinds fill free slots, a full queue drops the oldest. Returns the slot. */
int   cine_msg_post(cine_state *s, int kind, const uint16_t *text, uint32_t units, uint32_t duration);
int   cine_msg_post_ascii(cine_state *s, int kind, const char *text, uint32_t duration);
void  cine_msg_clear(cine_state *s, int kind);
void  cine_msg_set_visible(cine_state *s, int kind, int visible);      /* show_hud_help_text: the HUD draws only visible kinds */
/* The HUD's view: slot i if it is active and its kind is visible, else NULL. */
const cine_msg *cine_msg_slot(const cine_state *s, unsigned i);

/* Recording pool (the playbacks the host started with recording_play): `owner` is an opaque host id (an object handle). */
int        cine_play_begin(cine_state *s, const cinedef_section *sec, uint32_t rec, uint32_t owner, unsigned flags);   /* slot or -1 */
cine_play *cine_play_get(cine_state *s, int slot);                  /* NULL if the slot is free */
int        cine_play_find(const cine_state *s, uint32_t owner);     /* slot of the owner's recording or -1 */
void       cine_play_kill(cine_state *s, int slot);                 /* recording_kill */

#endif
