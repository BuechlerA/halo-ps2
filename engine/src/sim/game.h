/* The game simulation (docs/14-phase3-plan.md): fixed 30 Hz tick over the player, inventory, projectiles, damage and the object
 * table. Pure C11 over the data readers, so it is host-tested against the real a10 packs (tests/test_game.c); rendering, audio
 * and effects consume the event list it produces (game_events). Units: world units, radians, ticks of 1/30 s. */
#ifndef SIM_GAME_H
#define SIM_GAME_H
#include "render/camera.h"
#include "sim/fpview.h"
#include "sim/input.h"
#include "sim/objects.h"
#include "sim/phys.h"
#include "sim/player.h"
#include "sim/unitdef.h"
#include "sim/vitals.h"
#include "sim/weapdef.h"
#include "ui/huddef.h"
#include "world/ansm.h"
#include "world/cmod.h"
#include "world/coll.h"
#include "world/fxdef.h"

#define GAME_MAX_PROJ 64
#define GAME_MAX_EVENTS 160
#define GAME_SLOTS 2

typedef enum {
	GE_NONE = 0,
	GE_FIRE,          /* a trigger fired: tag = weapon tag, arg = trigger, pos/dir = muzzle */
	GE_EMPTY,         /* dry fire */
	GE_RELOAD,        /* reload started */
	GE_READY,         /* weapon ready / switched in */
	GE_OVERHEAT,
	GE_MELEE,
	GE_IMPACT,        /* projectile or bullet hit: tag = effect to create, pos/normal, arg = material */
	GE_DETONATE,      /* projectile detonated: tag = detonation effect, pos */
	GE_DAMAGE,        /* damage effect applied (camera shake, rumble): tag = damage effect tag, pos */
	GE_HIT,           /* an object took damage: obj = object index, amount in `value` */
	GE_KILL,          /* an object died */
	GE_PLAYER_HURT,   /* the player took damage: value */
	GE_PLAYER_DEAD,
	GE_PICKUP,        /* tag = weapon tag */
	GE_THROW,         /* grenade thrown: arg = grenade type */
	GE_LAND,          /* player landed: value = speed */
	GE_FOOTSTEP,
	GE_ZOOM,
	GE_SHIELD_DOWN,
	GE_SHIELD_UP,
	GE_SWITCH
} game_event_type;

typedef struct {
	uint8_t  type, material;
	uint16_t arg;
	uint32_t tag;
	int16_t  obj;
	float    pos[3], normal[3];
	float    value;
} game_event_t;

typedef struct {
	uint8_t   active;
	uint16_t  proj;                 /* WEAP projectile index */
	uint16_t  age, life;            /* ticks */
	int16_t   owner;                /* object index of the shooter, -1 = the player */
	uint8_t   grenade;              /* thrown (speed from the throw, gravity world) */
	uint8_t   bounces;
	float     pos[3], vel[3];       /* velocity in world units per tick */
	float     dist;                 /* distance travelled (damage falloff) */
} game_proj_t;

typedef struct {
	vitals_t vitals;
	int32_t  model;                 /* cmod model index, -1 none */
	uint8_t  has_vitals, dead, fading;
	uint16_t death_ticks;
	uint8_t  ally;                  /* set by units.c: an ally of the player (its projectiles never hurt the player) */
	uint8_t  team;                  /* the unit tag's default team (0 none, 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel), overwritten by units.c */
	float    dmg_scale;             /* set by units.c: multiplier on the damage of the projectiles this object owns (0 = 1) */
} game_unit_t;

typedef struct {
	int           weapon;           /* WEAP weapon index, -1 empty slot */
	weap_weapon   def;
	weap_state    st;
	int           fp_model, fp_graph;     /* MODL / ANIM indices of the first-person model and graph (-1 none) */
	int           fp_anim_idle;
} game_slot_t;

typedef struct {
	/* data (borrowed) */
	const unitdef_section *unit;
	const weap_section    *weap;
	const cmod_section    *cmod;
	const ansm_section    *ansm;
	const fxdef_section   *fx;
	const coll_bsp        *coll;
	object_world_t         ow;
	object_table_t        *objects;
	fpview_t              *fp;
	/* player */
	phys_world            phys;
	int                   has_phys;
	player_t              player;
	player_params_t       pp;
	unitdef_globals_def   globals;
	unitdef_biped_def     biped;
	int                   has_biped;
	vitals_t              vitals;
	int                   difficulty;       /* 0 easy .. 3 impossible */
	/* weapons */
	game_slot_t           slot[GAME_SLOTS];
	int                   cur;              /* active slot, -1 none */
	int                   switch_to;        /* pending switch (put away first), -1 none */
	uint16_t              grenades[2];
	int                   grenade_type;
	int                   throwing;         /* ticks until the grenade leaves, 0 = not throwing */
	uint8_t               zoomed;
	uint16_t              fp_timer;         /* ticks left of a one-shot first-person animation (0 = idle) */
	int                   fp_action;        /* ANSM_FP_* of the running animation, -1 idle */
	int                   unarmed_start;    /* the level starts with empty hands (game_drop_weapons): the script hands the first weapon out, the mission bot does not hunt for one */
	int                   all_weapons;      /* debug: switch cycles through every weapon of the map */
	int                   player_obj;       /* object slot of the player's body (-1 none): excluded from hit shapes */
	int                   player_frozen;    /* cinematics: the player's body does not move (no gravity either) */
	int                   autoaim;          /* aim assist on (default) */
	int                   nearby_weapon;    /* object index of a weapon within pickup range, -1 none (HUD prompt) */
	int                   cycle_index;
	/* world */
	game_proj_t           proj[GAME_MAX_PROJ];
	game_unit_t           unit_state[OBJ_MAX];
	uint32_t              tick;
	uint32_t              rng;
	uint32_t              prev_held;
	uint32_t              dead_ticks;       /* ticks since the player died */
	/* events of the last tick */
	game_event_t          ev[GAME_MAX_EVENTS];
	unsigned              ev_count;
	/* statistics */
	uint32_t              st_shots, st_hits, st_kills, st_world_hits, st_projectiles, st_reloads, st_melee, st_throws;
	float                 st_damage;
	float                 shake;            /* camera shake amplitude (radians), decays */
	float                 hurt_flash;
} game_t;

typedef struct {
	const unitdef_section *unit;
	const weap_section    *weap;
	const cmod_section    *cmod;
	const ansm_section    *ansm;
	const fxdef_section   *fx;
	const coll_bsp        *coll;
	object_world_t         ow;
	object_table_t        *objects;
	fpview_t              *fp;
	uint32_t               seed;
} game_init_t;

void game_init(game_t *g, const game_init_t *in, const float pos[3], float yaw);
/* Gives the slots the weapons with these tag ids (0 = keep), e.g. the biped's defaults. */
void game_give_weapon(game_t *g, int slot, uint32_t weapon_tag);
void game_drop_weapons(game_t *g);          /* empty hands (cfg startunarmed) */
void game_give_default_weapons(game_t *g);  /* the biped's default weapons into empty hands (player_add_equipment) */
/* One 30 Hz tick with this input. */
void game_tick(game_t *g, const game_input_t *in);
/* Call after an object was created at runtime (script object_create, AI spawn) so it gets its health and hit shapes. */
void game_object_spawned(game_t *g, unsigned index);
/* Points the simulation at the collision BSP of the (new) current BSP; NULL = no world collision. */
void game_teleport(game_t *g, const float pos[3], float yaw);
void game_set_collision(game_t *g, const coll_bsp *coll);
void game_camera(const game_t *g, camera_t *cam);
const game_event_t *game_events(const game_t *g, unsigned *count);
/* An AI unit's shot (also used by tests): spawns projectile `proj` (WEAP projectile index) at `pos` towards `dir` with owner object `owner`;
 * speed 0 = the projectile's own initial speed. Returns 1 if a projectile was created. */
int game_fire_projectile(game_t *g, int owner, unsigned proj, const float pos[3], const float dir[3], float speed_per_tick);
/* A thrown grenade of `type` (0 fragmentation, 1 plasma) owned by object `owner` (-1 none): the projectile of the globals' grenade table with
 * grenade physics (gravity, bounces, detonation timer), launched along `dir` at `speed` world units per SECOND. Returns 1 if created. */
int game_throw_grenade(game_t *g, int owner, int type, const float pos[3], const float dir[3], float speed);
/* Replaces the vitals of object `obj` (units.c: variant and difficulty values); a dead object keeps its state. */
void game_set_vitals(game_t *g, int obj, float health, float shield, float recharge_per_tick, uint16_t stun_ticks);
/* Hurts the player (melee blows of units, scripts): `amount` in tag units, `from` the source position; returns the damage dealt. */
float game_damage_player(game_t *g, float amount, const float from[3]);
/* Damage (tag units) from `src` position to object `obj` with the given multipliers; returns the damage dealt. Used by tests and scripts too. */
float game_damage_object(game_t *g, int obj, float amount, const cmod_hit *hit);
/* Fire direction helper for tests. */
void game_eye(const game_t *g, float eye[3], float fwd[3]);
const game_slot_t *game_cur_slot(const game_t *g);

#endif
