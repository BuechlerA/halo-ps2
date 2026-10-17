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
	int                   all_weapons;      /* debug: switch cycles through every weapon of the map */
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
/* One 30 Hz tick with this input. */
void game_tick(game_t *g, const game_input_t *in);
void game_camera(const game_t *g, camera_t *cam);
const game_event_t *game_events(const game_t *g, unsigned *count);
/* Damage (tag units) from `src` position to object `obj` with the given multipliers; returns the damage dealt. Used by tests and scripts too. */
float game_damage_object(game_t *g, int obj, float amount, const cmod_hit *hit);
/* Fire direction helper for tests. */
void game_eye(const game_t *g, float eye[3], float fwd[3]);
const game_slot_t *game_cur_slot(const game_t *g);

#endif
