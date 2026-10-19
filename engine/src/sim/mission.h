/* The mission: everything a script, the AI and the level flow share. One `mission_t` is the `host` pointer of the script machine; the
 * script built-ins (script/hs_host_*.c) reach every subsystem through it. Handle encodings (hs_value.v.h) of the script machine:
 *   - object / unit / vehicle / weapon / device / scenery handle: the object-table slot (0..OBJ_MAX-1); HS_HANDLE_NONE = no object
 *   - object list: MISSION_LIST_BIT | pool slot (a ring of MISSION_LISTS lists, valid for roughly a second)
 *   - scenario names (trigger volume, cutscene flag, camera point, title, recording, device group, command list, conversation, navpoint,
 *     hud message, starting profile, ai encounter/squad references): the index into the matching table of DEVS / CINE / AIDT / HUD / SCEN
 *   - object *names* (the constants of raw value types 39..43 and the object/unit values a script writes as a name): the index in the
 *     scenario's object-name table (DEVS names); `mission_resolve_object` turns any of these into an object slot
 *   - tag references (sound, effect, damage, looping sound, animation graph, actor variant, object definition): the Xbox tag id
 * Conventions for built-ins: arguments arrive evaluated; `ret` is pre-set to the neutral value of the call's type; return 1 to block (retry
 * next tick). Never fail: an unresolved handle makes the call a no-op (log with mission_note_missing once per built-in).
 * The player is also an object (a hidden biped placed at the player's feet every tick) so scripts can address it like any unit:
 * `mission.player_obj`. Pure C11 where possible, host-tested (tests/test_mission.c); the engine glue lives in main.c. */
#ifndef SIM_MISSION_H
#define SIM_MISSION_H
#include <stdint.h>
#include "script/hs.h"
#include "sim/devices.h"
#include "sim/fx.h"
#include "sim/game.h"
#include "sim/units.h"
#include "world/cinedef.h"
#include "sim/cine.h"
#include "ai/aidef.h"
#include "ui/hud.h"
#include "ui/huddef.h"
#include "world/devdef.h"
#include "sim/mission_misc.h"

#define MISSION_LIST_BIT 0x80000000u
/* Runtime object / unit / device handles (values computed by built-ins: list_get, unit, ...) carry MISSION_OBJ_BIT | slot. A handle WITHOUT the
 * bit is a scenario object NAME index: constants of the compiled scripts of value types 23 / 37..43 hold name indices (the data cannot tell a
 * constant "unit" from a computed one, so the two must be told apart by the value). Every producer uses mission_obj_handle(). */
#define MISSION_OBJ_BIT 0x40000000u
static inline uint32_t mission_obj_handle(int slot) { return slot < 0 ? 0xFFFFFFFFu : (MISSION_OBJ_BIT | (uint32_t)slot); }
#define MISSION_LISTS 64
#define MISSION_LIST_MAX 96

typedef struct { uint16_t count; int16_t obj[MISSION_LIST_MAX]; } mission_list;

/* trigger volumes the scripts are waiting for the player to enter (filled by the volume built-ins, read by the bot) */
typedef struct { uint16_t volume; uint8_t active; uint8_t pad; uint32_t last_tick; uint32_t hits; } mission_awaited;

/* State of the object / unit / player / device script built-ins (script/hs_host_obj.c, docs/33-host-objects.md). Plain data without pointers
 * (the integrator embeds it in checkpoints: sizeof is about 10 KB). Zero = default (input and look enabled, nothing attached). */
#define MISSION_ATTACH_MAX 48
#define MISSION_SUSPEND_MAX 16
#define MISSION_BITS(n) (((n) + 31) / 32)
typedef struct { int16_t parent, child, marker, pad; } mission_attach;        /* marker: model marker index of the parent, -1 = its origin */
typedef struct { int16_t obj, pad; float pos[3]; } mission_suspend;
typedef struct {
	uint8_t  player_input_disabled;    /* player_enable_input false: the main loop feeds neutral input */
	uint8_t  player_look_disabled;     /* player_camera_control false: the main loop ignores the look stick */
	uint8_t  pitch_inverted, joystick_not_normal;
	uint16_t action_latched;           /* HOBJ_PA_* since player_action_test_reset */
	uint16_t pad;
	mission_attach attach[MISSION_ATTACH_MAX];
	uint32_t attach_count;
	mission_suspend suspend[MISSION_SUSPEND_MAX];
	uint32_t suspend_count;
	uint32_t cannot_take_damage[MISSION_BITS(OBJ_MAX)];   /* object_cannot_take_damage / unit_impervious */
	uint32_t hid_vitals[MISSION_BITS(OBJ_MAX)];           /* ... objects whose game vitals are parked (has_vitals was 1) */
	uint32_t melee_inhibited[MISSION_BITS(OBJ_MAX)];      /* for the AI: object_set_melee_attack_inhibited */
	uint32_t ranged_inhibited[MISSION_BITS(OBJ_MAX)];
	uint32_t no_drop[MISSION_BITS(OBJ_MAX)];              /* unit_doesnt_drop_items: units.c must not drop the weapon */
	uint32_t not_enterable[MISSION_BITS(OBJ_MAX)];        /* unit_set_enterable_by_player false */
	int16_t  seat_vehicle[OBJ_MAX];    /* unit slot -> vehicle slot + 1 (0 = not seated) */
	uint16_t seat_hash[OBJ_MAX];       /* FNV-1a 16 of the seat label */
	int16_t  held[UNIT_MAX];           /* unit index -> object slot of the weapon in its hand + 1 (0 = none) */
	int16_t  saved_graph[OBJ_MAX];     /* custom animation: the object's own graph + 2 while a foreign graph plays (0 = none) */
} mission_hostobj;

struct ai;                 /* ai/ai.h (Phase 4 AI runtime) */
struct stream_player;      /* audio/stream.h */

typedef struct mission {
	/* data (borrowed) */
	game_t               *game;
	object_table_t       *objects;
	object_world_t        ow;
	units_t              *units;
	hs_machine           *hs;
	const hs_section     *scripts;
	devices_t            *devices;
	const devdef_section *dev;
	cine_state           *cine;
	const cinedef_section *cin;
	const aidef_section  *aidef;
	struct ai            *ai;
	hud_state_t          *hud;
	const huddef_section *huddef;
	const huddef_strings *strings;
	fx_t                 *fx;
	struct stream_player *stream;
	/* flow */
	uint32_t tick;
	int      cur_bsp;
	int      want_bsp;               /* set by switch_bsp: main performs it between ticks (-1 none) */
	float    want_pos[3], want_yaw;  /* where the player appears after the switch */
	int      want_teleport;
	int      won, lost, quit;
	int      cinematic;              /* a cinematic is running: player input off, HUD hidden */
	int      player_obj;             /* object slot of the player's body (-1 before init) */
	int      difficulty;             /* 0 easy .. 3 legendary */
	uint32_t objective_flags;
	uint32_t save_requested;
	/* object lists */
	mission_list lists[MISSION_LISTS];
	unsigned     list_next;
	/* diagnostics */
	uint32_t     missing_noted[8];
	unsigned     builtin_calls;
	int          log;
	int          log_anim;                    /* cfg animlog: print every custom_animation call and whether its graph and name resolve */
	/* optional profiling of mission_tick: set clock/clock_ms (platform timer), zone sums and peaks in ms since the last reset */
	uint32_t   (*clock)(void);
	float      (*clock_ms)(uint32_t, uint32_t);
	float        zone_sum[8], zone_peak[8];
	unsigned     zone_n;
	mission_awaited awaited_volume[8];
	mission_hostobj hobj;
	/* H2 script host (script/hs_host_misc.c, docs/34): sound / hud / navpoint / flow state and its borrowed pointers + hooks */
	mission_misc_t     misc;
	mission_misc_env_t misc_env;
	/* scratch for AI/script exchange */
	void        *user;
} mission_t;

void mission_init(mission_t *m);
/* Spawns the hidden player body object (needs a biped definition in SCEN: the player's) and keeps it at the player's position. */
void mission_attach_player(mission_t *m);
void mission_sync_player(mission_t *m);        /* call every tick before scripts run */
/* Device objects show their device's position: the position overlay animation of the device graph at the frame
 * devices_anim_frame gives (docs/27). Called by mission_tick after devices_tick. */
void mission_sync_devices(mission_t *m);

/* ---- object lists ---- */
mission_list *mission_list_new(mission_t *m, uint32_t *handle_out);   /* a cleared list from the ring */
mission_list *mission_list_get(mission_t *m, uint32_t handle);        /* NULL if not a list handle */
void          mission_list_add(mission_list *l, int obj);
/* An object-typed argument as a list: a list handle gives its list, an object handle gives a temporary one-element list. */
const mission_list *mission_arg_list(mission_t *m, const hs_value *a, mission_list *tmp);

/* ---- objects ---- */
/* Object slot of an argument: an object / unit handle, an object name (spawning the placement on demand when `create`), the player. -1 if none. */
int  mission_resolve_object(mission_t *m, const hs_value *a, int create);
int  mission_object_by_name(mission_t *m, uint32_t name_index, int create);
int  mission_object_live(const mission_t *m, int obj);                 /* exists, not free */
void mission_object_pos(const mission_t *m, int obj, float out[3]);    /* centre-ish position (feet + half the radius up) */
/* Creates the placement the name refers to (script object_create); returns the slot or -1. Attaches a unit when the object is a biped, and
 * initialises game state (health) through game_object_spawned. */
int  mission_spawn_named(mission_t *m, uint32_t name_index);
int  mission_spawn_def_at(mission_t *m, uint32_t def, const float pos[3], float yaw);
void mission_destroy_object(mission_t *m, int obj);

/* ---- one tick ---- */
/* Order: sync player -> script machine -> devices -> AI -> units -> game (player, weapons, projectiles) -> cinematics. `in` is the player's input
 * (ignored during a cinematic). */
void mission_tick(mission_t *m, const game_input_t *in);

void mission_note_missing(mission_t *m, const char *what);
/* The volume tests of the script built-ins report here (player_inside = the player's body was in the tested list and inside the volume). */
void mission_note_awaited_volume(mission_t *m, unsigned volume, int player_inside);
/* Position of the most recently activated active navpoint (HUD navigation), 0 if none. Provided by the hud built-ins (H2). */
int  mission_navpoint_target(mission_t *m, float out_pos[3]);

#endif
