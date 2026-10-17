/* State of the "misc" script host (script/hs_host_misc.c, docs/34-host-misc.md): camera / cinematic, sound, HUD / UI, navigation points, game flow
 * and the scripted player effect. Embedded in `mission_t` as `misc` (plain data, no pointers: it can be copied into a checkpoint as bytes and
 * a zeroed struct is the initial state) and `misc_env` (borrowed pointers and hooks the integrator wires after every restore).
 * Pure C11. Ticks are mission ticks (mission_t.tick, 30 Hz). */
#ifndef SIM_MISSION_MISC_H
#define SIM_MISSION_MISC_H
#include <stdint.h>
#include "audio/snddef.h"
#include "audio/strmdef.h"

#define MISC_IMPULSES 16        /* sound_impulse_start records kept (for sound_impulse_time / _stop) */
#define MISC_LOOPS 8            /* sound_looping_start records */
#define MISC_NAVPOINTS 16       /* active navigation points */
#define MISC_SAVE_TICKS 60      /* game_saving reports true for at most this long after a save request that nobody served */

/* mission_misc_t.save_requested bits (mission_t.save_requested is the same word: main serves it, then clears it) */
#define MISC_SAVE_NORMAL 1u
#define MISC_SAVE_NO_TIMEOUT 2u
#define MISC_SAVE_UNSAFE 4u

/* hud_hidden / hud_blink bits */
#define MISC_HUD_ALL 1u
#define MISC_HUD_CROSSHAIR 2u
#define MISC_HUD_HEALTH 4u
#define MISC_HUD_SHIELD 8u
#define MISC_HUD_MOTION 16u

enum { MISC_NAV_NONE = 0, MISC_NAV_FLAG = 1, MISC_NAV_OBJECT = 2 };
enum { MISC_GAIN_MUSIC = 0, MISC_GAIN_AMBIENCE, MISC_GAIN_DIALOGUE, MISC_GAIN_OTHER, MISC_GAINS };

typedef struct {                /* one sound_impulse_start */
	uint32_t tag;               /* snd! tag id */
	uint32_t end_tick;          /* mission tick the sound ends (natural length); remaining = end - tick */
	int16_t  obj;               /* source object slot or -1 */
	int8_t   slot;              /* stream slot it plays in (STREAM_SLOT_*), -1 none */
	uint8_t  used;
} misc_impulse;

typedef struct {                /* one sound_looping_start */
	uint32_t tag;               /* lsnd tag id */
	int8_t   slot;              /* stream slot, -1 = not streamed (resident loop, goes through the hook) */
	uint8_t  running, alt;      /* alt: sound_looping_set_alternate state */
	uint8_t  used;
} misc_loop;

typedef struct {                /* an active navigation point */
	uint8_t  active, kind;      /* MISC_NAV_* */
	uint8_t  team, type;        /* team enumeration value, navpoint type (the script's navpoint constant) */
	int32_t  ref;               /* kind FLAG: cutscene flag index; OBJECT: object slot */
	float    height;            /* vertical offset the script passes */
	uint32_t tick;              /* mission tick it was activated */
	uint32_t serial;            /* activation counter: the largest is the most recent */
	float    pos[3];            /* where it points (refreshed by mission_navpoint_target / misc_nav_refresh; objects move) */
	uint8_t  live;              /* pos is valid (an object navpoint whose object is gone is not live) */
	uint8_t  pad[3];
} misc_nav;

typedef struct {
	/* sound */
	misc_impulse imp[MISC_IMPULSES];
	misc_loop    loops[MISC_LOOPS];
	float        class_gain[MISC_GAINS];     /* sound_class_set_gain per group, valid where class_gain_set is 1 (else 1.0) */
	uint8_t      class_gain_set[MISC_GAINS];
	uint32_t     rng;                        /* variant picker (xorshift32), 0 = unseeded */
	uint32_t     impulses_started, loops_started;
	/* hud */
	uint32_t     hud_hidden, hud_blink;      /* MISC_HUD_* bits (a zero struct shows everything) */
	uint8_t      help_flash, screen_effect, suppress_bsp_objects, time_code_shown, time_code_running;
	int16_t      help_msg1, objective_msg1;  /* last message index + 1 (0 none) */
	uint32_t     help_shown, objectives_shown, scenario_help_shown;   /* messages posted: the mission bot's progress counters */
	uint8_t      timer_shown, timer_paused, timer_set, ncd_active;
	int16_t      timer_x, timer_y;
	uint32_t     timer_end, timer_left, timer_warning;   /* running: tick it reaches 0; paused: ticks left; warning threshold in ticks */
	uint32_t     ncd_end;
	/* navigation points */
	misc_nav     nav[MISC_NAVPOINTS];
	uint32_t     nav_serial;
	int16_t      cam_anchor_obj1;            /* camera_set_relative: object slot + 1 the camera hangs on (0 none); misc_tick refreshes its frame */
	/* game flow */
	uint32_t     save_tick;                  /* tick of the last save request */
	uint8_t      reverted, revert_requested; /* reverted: set by main after a checkpoint revert (game_reverted); revert_requested: game_revert, main serves it */
	uint8_t      pad0[2];
	float        game_speed;                 /* game_speed argument (informational: main may scale its clock; 0 = never set) */
	uint32_t     skip_ticks;                 /* game_skip_ticks accumulated, main may fast forward and clear it */
	/* scripted player effect (player_effect_*): evaluate with misc_player_effect_get */
	float        pfx_trans[3], pfx_rot[3];   /* maxima: world units, radians */
	float        pfx_rumble[2];              /* 0..1 low / high motor */
	float        pfx_intensity, pfx_stop_level;
	uint32_t     pfx_start, pfx_attack, pfx_stop, pfx_decay;   /* ticks */
	uint8_t      pfx_active, pfx_stopping;
	uint8_t      pad1[2];
} mission_misc_t;

/* Borrowed data and hooks; wired by the integrator (pointers: not part of a snapshot). All optional. */
typedef struct {
	const strm_section *strm;   /* the STRM index when mission.stream is NULL: the lengths of dialogue lines still come from it */
	const snd_section  *snd;    /* SND0 (resident sounds): lengths of resident impulses, resident-versus-streamed decision */
	void *user;
	/* resident impulse: gameaudio_play(tag, pos or NULL, camera, gain) */
	void (*play)(void *user, uint32_t tag, const float *pos, float gain);
	/* resident looping sound: start / stop of the sound_looping tag (the engine plays the looping part of its tracks) */
	void (*loop_start)(void *user, uint32_t tag, const float *pos, float gain);
	void (*loop_stop)(void *user, uint32_t tag);
} mission_misc_env_t;

#endif
