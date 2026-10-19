/* AI runtime (docs/32-ai-runtime.md): the brain of every non-player actor. The AI *decides*, the unit layer (sim/units.h) *executes*: this
 * module only reads unit / object / player state and writes `unit_control_t` through units_set_control, plus the handful of direct writes
 * that spawning, command-list animations and erasing need (documented where they happen). Pure C11, no heap, no `double`, no ps2sdk, no
 * recursion; host-tested against the real a10 packs (tests/test_ai.c). Data: ai/aidef (encounters, squads, variants, command lists,
 * conversations), ai/path (navigation meshes of the current BSP), world/coll (line of sight through game_t.coll), sim/unitdef (difficulty).
 *
 * Shape
 *   ai_init()      clear the state, bind the data (ai_env)
 *   ai_set_bsp()   the BSP the path section / collision belong to changed: pending placements of that BSP happen, other BSPs' actors freeze
 *   ai_place_*()   spawn squads / encounters for the difficulty (also what the `ai_place` built-in calls)
 *   ai_tick()      one 30 Hz step: perception (staggered), behaviour decisions, movement, fire control; writes the units' controls
 *   ai_*()         script-facing operations and queries (hs_host_ai.c maps the built-ins onto them)
 *
 * State and saves: everything mutable is `ai_state` (plain old data, NO pointers, fixed size ai_state_size()); `ai_t` adds the borrowed data
 * pointers and the hooks (not saved). Checkpoint: memcpy ai_state_size() bytes out of ai_state_ptr(); restore: ai_init() with the same
 * environment, copy the bytes into ai_state_ptr(), ai_rebind() (re-derives the profile cache and validates every index; -1 = unusable).
 * The object table, the units and the game's vitals are saved by their owners and must be restored with it (actors name units and objects
 * by index).
 *
 * Script references (`ai` values of the scripts): bits 0..15 encounter, bit 31 + bits 16..30 = squad of the encounter, bit 30 + bits 16..29 =
 * platoon of the encounter (verified against every constant of a10). */
#ifndef AI_AI_H
#define AI_AI_H
#include <stddef.h>
#include <stdint.h>
#include "ai/aidef.h"
#include "ai/path.h"
#include "sim/game.h"
#include "sim/units.h"

#define AI_STATE_VERSION 1u
#define AI_MAX_ACTORS 64u
#define AI_MAX_ENCOUNTERS 128u
#define AI_MAX_SQUADS 1024u
#define AI_MAX_CONVS 64u
#define AI_MAX_PROFILES 48u
#define AI_MAX_NOISES 8u
#define AI_MAX_LINKS 32u
#define AI_WAYPOINTS 6u
#define AI_CONV_PARTS 8u
#define AI_TARGET_PLAYER UNIT_MAX          /* target index of the player (units are 0..UNIT_MAX-1) */
#define AI_NO_TARGET (-1)
#define AI_NONE16 0xFFFFu

/* Actor behaviour (the per-actor state machine) */
typedef enum {
	AIS_FREE = 0, AIS_SLEEP, AIS_IDLE, AIS_PATROL, AIS_ALERT, AIS_COMBAT, AIS_SEARCH, AIS_RETREAT, AIS_FLEE, AIS_FOLLOW, AIS_CHARGE, AIS_STATES
} ai_behavior;

/* Orders a script gives to an encounter / squad (ai_attack, ai_defend, ...) */
typedef enum { AIO_NONE = 0, AIO_ATTACK, AIO_DEFEND, AIO_RETREAT, AIO_FOLLOW_PLAYER, AIO_FOLLOW_SQUAD, AIO_MANEUVER, AIO_COUNT } ai_orders;

/* Flags of actors / squads / encounters (a flag set on a squad or encounter applies to its actors, present and future) */
enum {
	AIF_BRAINDEAD = 1, AIF_BLIND = 2, AIF_DEAF = 4, AIF_BERSERK = 8, AIF_FORCE_ACTIVE = 16, AIF_PLAYFIGHT = 32,
	AIF_IGNORE_ALL = 64,               /* ai_try_to_fight_nothing: no hostile is ever a target */
	AIF_IGNORE_PLAYER = 128            /* ai_try_to_fight (a, b) with a squad: the player is not preferred */
};

/* Conversation status (what `ai_conversation_status` answers; scripts test `< 4` while it runs and `> 4` when it is over) */
enum { AI_CONV_NONE = 0, AI_CONV_STARTING = 1, AI_CONV_PLAYING = 2, AI_CONV_ENDING = 4, AI_CONV_DONE = 5, AI_CONV_ABORTED = 6 };

/* Atom kinds counted when unsupported (index = aidef_atom_type) are in ai_state.atom_skipped[] */

typedef struct {
	uint8_t  kind;                     /* 1 encounter, 2 squad, 3 platoon, 0 = not a valid reference */
	int16_t  enc;                      /* encounter index */
	int16_t  squad;                    /* squad index inside the encounter, -1 */
	int16_t  platoon;                  /* platoon index inside the encounter, -1 */
} ai_ref;

/* ---- mutable state (pointer-free) ---------------------------------------------------------------------------------------------------- */
typedef struct {                       /* a cached, difficulty-independent digest of an actor variant (+ its actor, biped and weapon) */
	uint32_t variant_tag;
	uint8_t  type, movement_type, armed, grenade_type;
	uint8_t  grenade_min_enemies, mag, pad[2];      /* mag: ammunition magazine of trigger 0, 0xFF = energy weapon without ammunition */
	float    vision_dist, cos_central, cos_max, cos_periph, periph_dist;
	float    hearing, t_combat, t_guard, t_idle;                /* seconds to notice */
	float    fire_dist, rof, error, rmin, rmax;
	float    first_delay[2], burst_dur[2], burst_sep[2], burst_radius, return_len, track, lead;
	float    crouch_chance, crouch_t[2], run_t[2];
	float    gren_chance, gren_check, gren_range[2], gren_timeout;
	float    proj_speed, duty;                                   /* wu per tick of the weapon's projectile; fraction of ticks UC_FIRE may be held */
	float    body, shield, recharge, stun;                       /* vitals at spawn before the difficulty (stun in seconds) */
	uint16_t gren_count[2];
	int16_t  weapon;                                             /* WEAP index, -1 none */
	uint32_t weapon_tag;
} ai_profile;

typedef struct {
	uint8_t  active, st, team, prof;
	uint8_t  flags, orders, init_state, ret_state;
	int16_t  unit, obj;
	uint16_t enc, squad;               /* encounter, global squad index */
	uint16_t platoon;                  /* platoon inside the encounter, AI_NONE16 */
	uint8_t  bsp, slot;                /* BSP the actor lives in; start location it was placed at */
	uint32_t spawn_tick, state_tick;
	float    home[3], home_yaw;
	/* perception */
	int16_t  tgt;                      /* AI_NO_TARGET, unit index, AI_TARGET_PLAYER */
	uint8_t  aware;                    /* 0 none, 1 suspicious (noise, damage, told), 2 target acquired */
	uint8_t  tgt_vis;
	float    notice;                   /* 0..1 accumulated sighting */
	float    tgt_pos[3];               /* last known position of the target */
	uint32_t tgt_seen_tick, perc_tick, hurt_tick;
	float    last_hp;                  /* health + shield at the last look, for damage detection */
	/* navigation */
	float    goal[3];
	uint8_t  goal_valid, wp_n, wp_i, stuck;
	float    wp[AI_WAYPOINTS][3];
	uint8_t  wp_kind[AI_WAYPOINTS], pad0[2];
	uint32_t route_tick;               /* tick of the last route */
	float    route_goal[3];
	float    prog_pos[3];
	uint32_t prog_tick;
	uint16_t stuck_ticks, pad1;
	/* combat */
	int16_t  fp;                       /* firing position (index inside the encounter), -1 */
	uint16_t fp_scan;                  /* rotating scan offset */
	uint32_t fp_tick;
	uint8_t  bstate, crouching, pad2[2];
	uint16_t btimer, blen;
	uint16_t crouch_timer, pad3;
	float    boff[2], rof_acc, aim[2];
	uint32_t gren_tick, melee_tick, strafe_tick, retreat_until;
	float    strafe_dir;
	/* search / patrol / follow */
	uint32_t search_until, wander_tick;
	uint16_t patrol_i, patrol_dir;
	uint32_t dwell_until;
	/* command list */
	int16_t  cl_list;                  /* -1 none */
	uint16_t cl_pc;
	uint8_t  cl_forced, cl_status, cl_phase, cl_crouch;   /* status 0 none, 1 running, 2 finished */
	uint32_t cl_timer;
	float    cl_look[3], cl_radius;
	uint8_t  cl_look_kind, cl_aim, cl_skip, cl_loops;
	/* last control written (the AI writes every tick; kept for stuck / debugging) */
	float    yaw_goal;
} ai_actor;

typedef struct {
	uint16_t spawned;                  /* actors placed since the last erase */
	uint8_t  placed, orders, flags, state_override;   /* state_override: aidef_actor_state set by ai_migrate (0 none) */
	uint8_t  platoon, pad;             /* platoon of the encounter (0xFF none) */
	uint16_t enc;                      /* encounter the squad belongs to */
	uint16_t next_start;               /* rotating start-location counter */
	uint32_t spawn_at;                 /* delayed squad: tick it spawns (0 = none pending) */
	int16_t  follow;                   /* follow-the-leader target: global squad index, -1 */
	int16_t  pad2;
} ai_squad_rt;

typedef struct {
	uint8_t  placed, pending, flags, orders;
	uint16_t spawned, first_squad;
	uint16_t squad_count, first_firing, firing_count, bsp;
	uint8_t  activated, pad;
	uint32_t grenade_tick, activated_tick;
} ai_enc_rt;

typedef struct {
	uint8_t  status, line, nparts, pad;
	uint32_t timer;                    /* tick the current line ends / the next starts */
	uint32_t started;
	int16_t  part_obj[AI_CONV_PARTS];  /* object slot of each participant, -1 */
} ai_conv_rt;

typedef struct { float pos[3]; uint32_t tick; uint8_t team, pad[3]; int16_t src, pad2; } ai_noise;

typedef struct {                       /* difficulty scales (ai_set_difficulty) */
	float vit_enemy, shield_enemy, recharge_enemy, vit_friend, shield_friend, recharge_friend;
	float rof, error, burstiness, new_target, burst_sep, track_add, lead_add, gren_chance, gren_timer, melee_delay;
} ai_diff;

typedef struct {
	uint32_t magic, version;
	uint32_t enc_count, squad_count, conv_count;        /* of the section it was built for (ai_rebind checks them) */
	uint32_t tick, rng;
	uint8_t  enabled, difficulty, follow_default, grenades, dialogue, conv_proximity, pad[2];
	ai_diff  diff;
	uint32_t prof_n, noise_head, link_n;
	ai_profile prof[AI_MAX_PROFILES];
	ai_actor   actor[AI_MAX_ACTORS];
	ai_squad_rt squad[AI_MAX_SQUADS];
	ai_enc_rt   enc[AI_MAX_ENCOUNTERS];
	ai_conv_rt  conv[AI_MAX_CONVS];
	ai_noise    noise[2 * AI_MAX_NOISES];               /* [0, AI_MAX_NOISES): read this tick; [AI_MAX_NOISES, +noise_head): made this tick */
	int16_t     link[AI_MAX_LINKS][2];                  /* link_activation pairs (encounter a activates encounter b) */
	uint8_t     rel[AIDEF_TEAMS][AIDEF_TEAMS];          /* 0 default, 1 allied (ai_allegiance), 2 broken (hostile) */
	uint32_t    disregard[(UNIT_MAX + 31) / 32];         /* ai_disregard: units no actor targets */
	/* statistics (deterministic, saved with the state) */
	uint32_t    st_placed, st_skipped_no_def, st_skipped_no_variant, st_spawn_fail, st_noticed, st_alerts, st_shots_ticks, st_routes,
	            st_route_expanded, st_route_partial, st_route_fail, st_replans_deferred, st_stuck, st_assists, st_los_rays, st_rays_fp,
	            st_deaths, st_state_changes[AIS_STATES], st_atoms_run, st_atoms_instant, st_conv_lines, st_dormant_ticks, st_melee,
	            st_grenades, st_fp_picks, st_noises, st_think, st_climbs;
	uint32_t    atom_skipped[AIDEF_ATOMS + 4];
	uint32_t    atom_seen[AIDEF_ATOMS + 4];
} ai_state;

/* ---- hooks (not saved) ---------------------------------------------------------------------------------------------------------------- */
typedef enum { AI_SND_LINE_START = 0, AI_SND_LINE_STOP, AI_SND_CONV_END } ai_snd_kind;
typedef struct {
	uint8_t  kind;                     /* ai_snd_kind */
	uint8_t  participant;              /* participant index of the speaker (line events) */
	uint16_t conv, line;               /* conversation index, line index inside it */
	int16_t  obj;                      /* object slot of the speaker, -1 when its actor is gone */
	uint32_t sound;                    /* snd! tag id of the take (0 = silent line) */
	float    pos[3];                   /* where the speaker stands */
	float    run_to_player;            /* the conversation's "run to player" distance (wu), 0 = none */
} ai_sound_event;
/* The integrator plays the line (stream player, positional impulse) and returns its length in ticks (0 = unknown: the AI assumes 3 s). */
typedef int (*ai_sound_fn)(void *user, const ai_sound_event *ev);
/* `script` atoms: wake the script with that name of the AIDT script references (name_index) */
typedef void (*ai_script_fn)(void *user, uint32_t name_index);
/* look-at-object atoms: scenario object-name index -> object slot or -1 */
typedef int (*ai_object_fn)(void *user, uint32_t name_index);
typedef void (*ai_log_fn)(void *user, const char *msg);

typedef struct {
	const aidef_section *aidef;
	game_t              *game;                  /* player, objects, weapons, vitals, difficulty, collision */
	units_t             *units;
	const path_bsp      *path;                  /* navigation of the current BSP (NULL: straight lines) */
	path_scratch        *scratch;               /* route memory sized for `path` (path_scratch_bytes) */
	int                  bsp;                   /* index of the BSP `path` and game->coll belong to */
} ai_env;

typedef struct ai {
	ai_state st;
	ai_env   env;
	ai_sound_fn  sound;  void *sound_user;
	ai_script_fn script; void *script_user;
	ai_object_fn object; void *object_user;
	ai_log_fn    log;    void *log_user;
	unsigned     routes_left;                   /* route budget of the running tick */
	unsigned     max_routes_per_tick;           /* default 1 */
	unsigned     tgt_n, noise_read;             /* transient: target table and readable noises of the running tick */
	struct { float pos[3], chest[3], vel[3]; uint8_t team, valid; int16_t obj; } tgt[UNIT_MAX + 1];
} ai_t;

/* ---- lifecycle ------------------------------------------------------------------------------------------------------------------------ */
void   ai_init(ai_t *ai, const ai_env *env);               /* clears the state, difficulty from env->game->difficulty */
int    ai_rebind(ai_t *ai, const ai_env *env);             /* after restoring ai_state: 0 ok, -1 inconsistent (the state is cleared) */
ai_state *ai_state_ptr(ai_t *ai);
size_t ai_state_size(void);
void   ai_set_bsp(ai_t *ai, int bsp, const path_bsp *path, path_scratch *scratch);
void   ai_set_difficulty(ai_t *ai, int difficulty);        /* 0 easy .. 3 legendary: scales of the difficulty table */
void   ai_set_sound_hook(ai_t *ai, ai_sound_fn fn, void *user);
void   ai_set_script_hook(ai_t *ai, ai_script_fn fn, void *user);
void   ai_set_object_hook(ai_t *ai, ai_object_fn fn, void *user);
void   ai_set_log_hook(ai_t *ai, ai_log_fn fn, void *user);
void   ai_tick(ai_t *ai);                                   /* after the script machine, before units_tick */

/* ---- references ------------------------------------------------------------------------------------------------------------------------ */
int      ai_ref_decode(const ai_t *ai, uint32_t handle, ai_ref *out);   /* 1 when the handle names an encounter / squad / platoon of the section */
uint32_t ai_ref_encode(const ai_ref *r);
int      ai_ref_squad_range(const ai_t *ai, const ai_ref *r, unsigned *first, unsigned *count);   /* global squads covered (a platoon: filter with ai_ref_has_squad) */
int      ai_ref_has_squad(const ai_t *ai, const ai_ref *r, unsigned global_squad);
int      ai_actor_in_ref(const ai_t *ai, const ai_actor *a, const ai_ref *r);

/* ---- spawning -------------------------------------------------------------------------------------------------------------------------- */
int  ai_place(ai_t *ai, const ai_ref *r);                  /* squads / encounters not placed yet (or wiped out): actors spawned; returns actors created now */
int  ai_place_all_initial(ai_t *ai);                       /* encounters not flagged "not initially created" of the current BSP */
int  ai_spawn_one(ai_t *ai, const ai_ref *r);              /* one more actor of that squad (ai_spawn_actor): actor index or -1 */
int  ai_attach_unit(ai_t *ai, int unit, const ai_ref *r, uint32_t variant_tag);   /* an existing unit becomes an actor (ai_attach / ai_attach_free): actor index or -1 */
void ai_detach_unit(ai_t *ai, int unit);                   /* the actor of a unit stops being driven (the unit stays) */
void ai_kill(ai_t *ai, const ai_ref *r, int silent);       /* units killed, actors removed */
void ai_erase(ai_t *ai, const ai_ref *r);                  /* objects destroyed; the squads can be placed again */
void ai_erase_all(ai_t *ai);
void ai_renew(ai_t *ai, const ai_ref *r);                  /* respawn what died */
void ai_timer_expire(ai_t *ai, const ai_ref *r);           /* delayed squads spawn now */
void ai_migrate(ai_t *ai, const ai_ref *from, const ai_ref *to);
void ai_migrate_unit(ai_t *ai, int unit, const ai_ref *to);
void ai_free(ai_t *ai, const ai_ref *r);                    /* the actors become plain units (ai_free) */
void ai_teleport_home(ai_t *ai, const ai_ref *r);

/* ---- orders ---------------------------------------------------------------------------------------------------------------------------- */
void ai_set_orders(ai_t *ai, const ai_ref *r, int orders);     /* AIO_* */
void ai_set_follow(ai_t *ai, const ai_ref *r, const ai_ref *target);   /* target NULL: follow the player */
void ai_set_flag(ai_t *ai, const ai_ref *r, unsigned flag, int on);
void ai_set_flag_unit(ai_t *ai, int unit, unsigned flag, int on);
void ai_magically_see_players(ai_t *ai, const ai_ref *r);
void ai_magically_see_encounter(ai_t *ai, const ai_ref *observer, const ai_ref *seen);
void ai_link_activation(ai_t *ai, const ai_ref *a, const ai_ref *b);
void ai_allegiance(ai_t *ai, int team_a, int team_b, int mode);       /* mode 0 remove, 1 allied, 2 broken */
int  ai_teams_hostile(const ai_t *ai, int team_a, int team_b);
void ai_disregard(ai_t *ai, int unit, int on);

/* ---- command lists --------------------------------------------------------------------------------------------------------------------- */
int  ai_command_list(ai_t *ai, const ai_ref *r, int list, int forced);   /* every actor of the reference; returns actors affected */
int  ai_command_list_unit(ai_t *ai, int unit, int list);
void ai_command_list_advance(ai_t *ai, const ai_ref *r);
int  ai_command_list_status(const ai_t *ai, int unit);       /* 0 none, 1 running, 2 finished (also when the unit is gone) */

/* ---- queries --------------------------------------------------------------------------------------------------------------------------- */
unsigned ai_living_count(const ai_t *ai, const ai_ref *r);
unsigned ai_nonswarm_count(const ai_t *ai, const ai_ref *r);
float    ai_living_fraction(const ai_t *ai, const ai_ref *r);   /* alive / actors placed (0 before anything was placed) */
float    ai_strength(const ai_t *ai, const ai_ref *r);          /* mean (health + shield) / maximum of the living actors, 0 when none */
int      ai_status(const ai_t *ai, const ai_ref *r);            /* 0 inactive .. 6 everybody fighting (docs/32) */
unsigned ai_actor_objects(const ai_t *ai, const ai_ref *r, int16_t *out, unsigned max);   /* object slots of the living actors */
int      ai_actor_of_unit(const ai_t *ai, int unit);            /* actor index or -1 */
int      ai_actor_of_object(const ai_t *ai, int obj);
unsigned ai_active_actors(const ai_t *ai);

/* ---- conversations --------------------------------------------------------------------------------------------------------------------- */
int  ai_conversation_start(ai_t *ai, int conv);              /* 1 when it was started (or is running) */
void ai_conversation_stop(ai_t *ai, int conv);
void ai_conversation_advance(ai_t *ai, int conv);
int  ai_conversation_status(const ai_t *ai, int conv);       /* AI_CONV_* */
int  ai_conversation_line(const ai_t *ai, int conv);

#endif
