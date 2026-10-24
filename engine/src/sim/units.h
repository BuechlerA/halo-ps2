/* Units: every biped that is not the player (AI actors, scripted units, crew) is a `unit_t` attached to an object-table slot. A unit has a
 * body (the Phase 3 capsule controller: collision against the BSP, gravity, stairs), a control input (what an AI, a recorded animation or a
 * script wants it to do this tick), a weapon (the Phase 3 weapon state machine) and an animation state. Health comes from the game's
 * `unit_state` (vitals) of the same object slot. Pure C11, host-tested against the real packs (tests/test_units.c). Implemented by the
 * Phase 4 runtime workstream (ai/units.c, docs/31-units.md). The API below is the contract of everything that drives units: the AI runtime,
 * recorded animation playback (cinedef_ctl maps onto unit_control_t), the script built-ins (unit_*, object_*, ai_*), the mission bot. */
#ifndef SIM_UNITS_H
#define SIM_UNITS_H
#include <stddef.h>
#include <stdint.h>
#include "ai/aidef.h"
#include "sim/game.h"
#include "sim/player.h"
#include "sim/weapdef.h"
#include "world/cinedef.h"

#define UNIT_MAX 96
#define UNIT_NONE (-1)

/* control flags */
#define UC_CROUCH   0x0001u
#define UC_JUMP     0x0002u
#define UC_FIRE     0x0004u       /* primary trigger held */
#define UC_FIRE2    0x0008u       /* secondary trigger held */
#define UC_GRENADE  0x0010u       /* throw a grenade (edge: starts a throw) */
#define UC_MELEE    0x0020u       /* edge: starts a melee blow */
#define UC_RELOAD   0x0040u       /* edge: starts a reload */
#define UC_WALK     0x0080u       /* walk instead of run (AI "moving at walking pace") */
#define UC_FLASHLIGHT 0x0100u
#define UC_ZOOM     0x0200u

typedef struct {
	float    throttle_fwd, throttle_left;   /* -1..1: fractions of the unit's run speed, relative to the body yaw (cinedef_ctl x forward, y left) */
	float    aim_yaw, aim_pitch;            /* world radians the unit wants to face / aim at; the body turns towards aim_yaw at the biped's turn speed */
	uint32_t flags;                         /* UC_* */
	int8_t   weapon_slot;                   /* -1 keep (units have one weapon: other values are ignored) */
} unit_control_t;

/* what the unit is doing, for animation selection and for the AI's own logic */
typedef enum {
	UA_IDLE = 0, UA_MOVE, UA_AIRBORNE, UA_LAND, UA_FIRE, UA_RELOAD, UA_MELEE, UA_THROW, UA_FLINCH, UA_DEAD, UA_CUSTOM /* script / command-list animation */
} unit_action;

typedef struct {
	uint8_t   active;
	uint8_t   team;                         /* aidef_team: 1 player 2 human 3 covenant 4 flood 5 sentinel */
	uint8_t   flags;                        /* UF_* */
	uint8_t   action;                       /* unit_action */
	int16_t   obj;                          /* object-table slot */
	int16_t   biped;                        /* unitdef biped index */
	uint32_t  variant_tag;                  /* actor variant it was spawned from (0 none) */
	player_t  body;                         /* pos = feet; the object's pos / root follow it */
	player_params_t pp;                     /* from the biped tag (+ the actor's movement type) */
	unit_control_t ctl;
	int16_t   weapon;                       /* WEAP weapon index, -1 unarmed */
	weap_state wst;
	uint16_t  grenades[2];
	uint16_t  action_ticks;                 /* ticks left of a one-shot action animation */
	uint16_t  anim_class;                   /* resolved animation class index (ASTB) of the current posture block */
	int16_t   custom_anim;                  /* script / command-list animation index in the unit's graph, -1 none */
	float     custom_frame_scale;
	int16_t   rm_frame;                     /* last frame of the custom animation whose root motion has been applied (-1 none) */
	uint32_t  spawn_tick;
	float     stun;                         /* ticks of flinch left */
	/* ---- internals of units.c (pointer-free; the AI may read them) ---- */
	uint32_t  prev_flags;                   /* control flags of the previous tick (edge detection) */
	uint32_t  rng;                          /* per-unit xorshift state (spread, variants) */
	uint32_t  anim_tag;                     /* animation graph tag id of the biped */
	float     proj_error;                   /* radians added to the weapon's own spread (variant x difficulty) */
	float     rate_mult;                    /* rate-of-fire multiplier (difficulty table, hostile teams) */
	float     hp_prev;                      /* health at the end of the last tick (flinch detection) */
	float     anim_rate;                    /* playback rate of the current animation (1 = as authored) */
	float     turn_speed;                   /* rad/s */
	float     turn_threshold;               /* rad: a stationary scripted unit's body turns only once its aim is this far off its facing (the biped tag's stationary turning threshold); below it the head and arms look (ADR-086) */
	float     aim_range[4];                 /* the aiming screen of the class block the aim-still overlay belongs to (yaw right, left, pitch down, up), radians */
	uint32_t  aim_key;                      /* posture / weapon / graph aim_k and aim_range were looked up for (0xFFFFFFFF: look again) */
	int16_t   aim_k;                        /* animation index of that overlay in the unit's graph, -1 none */
	float     look_range[4];                /* the unit block's aiming screen in radians: yaw right, yaw left, pitch down, pitch up (marines 60 / 20 / 30 / 60, Keyes 70 / 70 / 45 / 45); look_range[0] < 0 = not read yet */
	uint8_t   turning;                      /* a stationary turn animation is playing: 0 none, 1 turn-left, 2 turn-right (the body follows its root yaw, 90 degrees per play, ADR-086) */
	int16_t   turn_k;                       /* its animation index in the unit's graph */
	uint16_t  turn_f;                       /* frames of it applied to the body */
	float     hit_dir[2];                   /* unit vector (x, y) towards the last attacker, 0 0 unknown */
	uint16_t  anim_key;                     /* what the current animation was chosen for (0xFFFF: choose again) */
	uint16_t  event_at;                     /* melee blow / grenade release: action_ticks value at which it happens, 0 done */
	uint16_t  fire_hold;                    /* ticks left of UA_FIRE after a shot */
	uint16_t  flinch_cd;                    /* ticks until the next flinch is allowed */
	int16_t   custom_graph;                 /* ANIM graph index of a custom animation, -1 the unit's own */
	int16_t   base_graph;                   /* the unit's own ANIM graph index */
	int16_t   anim_cur;                     /* animation index of the base animation being played, -1 none */
	uint8_t   grenade_type;                 /* current grenade type, 0 frag 1 plasma */
	uint8_t   posture;                      /* the animation block: 0 stand, 1 crouch, 2 alert (an unarmed non-player unit: the Xbox shows the captain and the crew walking and standing with their hands behind their backs, the `alert unarmed` animations, ADR-080) */
	uint8_t   alert_state;                  /* 0 not looked up, 1 the graph has an `alert` block, 2 it has none */
	uint8_t   mv_have;                      /* mv_base holds the biped tag's speeds (the unit has adopted its move animations' ground speeds at least once) */
	uint32_t  mv_key;                       /* posture / weapon / graph the speeds in pp were taken for */
	float     mv_base[5];                   /* the speeds of the biped tag (run, back, strafe, crouch, walk): what pp falls back to when a move animation has no root motion */
	uint8_t   move_dir;                     /* 0 front 1 back 2 left 3 right (sticky) */
	uint8_t   settle;                       /* ticks since the physics last ran on a settled unit */
	uint8_t   air;                          /* ticks without ground under the feet */
	uint8_t   dying;                        /* death handled */
} unit_t;
#define UF_NO_AI 1u          /* controlled from outside (script, recording): the AI must not drive it */
#define UF_INVULNERABLE 2u
#define UF_HIDE_BODY 4u      /* the player's own body in first person */
#define UF_PLAYER 8u         /* the player's body object */
#define UF_NO_DROP 16u       /* does not drop its weapon when it dies */
#define UF_ALERT 32u         /* a script put the unit in its alert posture (`unit_set_seat unit "alert"`): it stands in the graph's `alert` block, armed or not (ADR-083) */

/* what happened in the last units_tick, for sound, muzzle flashes and the AI's hearing (the game's own event list is cleared by game_tick, which
 * runs after units_tick): shots, reloads, blows, grenades, deaths, flinches and landings. */
#define UNIT_EVENTS 32
typedef enum { UE_NONE = 0, UE_FIRE, UE_RELOAD, UE_MELEE, UE_THROW, UE_DEATH, UE_FLINCH, UE_LAND } unit_event_type;
typedef struct {
	uint8_t  type;                          /* unit_event_type */
	uint8_t  arg;                           /* UE_FIRE: trigger; UE_MELEE: 1 if it hit; UE_THROW: grenade type */
	int16_t  obj;                           /* object slot of the unit */
	uint32_t tag;                           /* UE_FIRE, UE_RELOAD: weapon tag id */
	float    pos[3], dir[3];                /* UE_FIRE: muzzle and direction; others: feet (UE_LAND: dir[0] = landing speed) */
} unit_event_t;

typedef struct {
	unit_t   u[UNIT_MAX];
	int16_t  by_obj[OBJ_MAX];               /* object slot -> unit index, -1 */
	unsigned count;                          /* high-water mark of u[] */
	uint32_t spawned, died;                  /* statistics */
	float    lod_distance;                   /* wu from the player beyond which a unit's body is integrated every other tick (0 = never; set by the host) */
	uint8_t  lod_cine;                       /* host: allow lod_all during cinematics (the mission sets lod_all from the cinematic flag) */
	uint8_t  tag_speeds;                     /* debug (cfg unitspeeds 1): units keep the speeds of their biped tag instead of the ground speed of the move animation they play (ADR-083) */
	uint8_t  lod_all;                        /* every unit's body is integrated every other tick (a cinematic: the mission sets it from the cinematic flag) */
	unsigned ev_count;                       /* events of the last units_tick (dropped beyond UNIT_EVENTS) */
	unit_event_t ev[UNIT_EVENTS];
} units_t;

extern int g_units_turn_gate, g_units_look;           /* debug switches (cfg turngate, lookoverlay) */
void units_init(units_t *s);
/* Creates a unit for object slot `obj` (an existing biped object: spawned from a placement or objects_spawn_def). Reads the biped tag
 * (unitdef) of its definition, sets team (0 = the biped's default), weapon (weapon tag id, 0 = the biped's first default weapon, -1 =
 * unarmed), grenades. Health and shield: the game's vitals of the object (collision model) scaled by the difficulty table of the globals
 * (enemy / friend vitality, shield, recharge). Returns the unit index or -1; an object that already is a unit returns its index. */
int  units_attach(units_t *s, game_t *g, int obj, int team, int32_t weapon_tag);
void units_detach(units_t *s, int unit);                 /* the object stays, the unit stops being simulated */
int  units_of_object(const units_t *s, int obj);         /* unit index or -1 */
/* Applies an actor variant to a unit (AI spawn, after units_attach): health / shield (absolute values of the variant, scaled by the
 * difficulty table), weapon, grenade type and count, projectile error (x the difficulty's projectile error scale), damage modifier; sets
 * variant_tag. Returns 1 (0 for a bad unit). */
int  units_apply_variant(units_t *s, game_t *g, int unit, const aidef_variant_def *v);
/* One 30 Hz tick: for every active unit apply its control (turn, move with collision, jump, crouch), run the weapon, shoot via
 * game_fire_projectile, update animation (object anim/frame), handle death (death animation, weapon drop), flinch on damage.
 * Call it BEFORE game_tick (the hit events of the previous game tick give the direction of a hit) and keep objects_tick after it. */
void units_tick(units_t *s, game_t *g);
/* Body integrations (capsule sweeps) run by units_tick since the last call; resets the count. For the mission log. */
unsigned units_take_sweeps(void);
void units_set_control(units_t *s, int unit, const unit_control_t *c);
/* Kills / damages through the game's vitals so the usual events fire; returns damage dealt. */
float units_damage(units_t *s, game_t *g, int unit, float amount);
void  units_kill(units_t *s, game_t *g, int unit, int silent);
int   units_alive(const units_t *s, const game_t *g, int unit);
/* Teleports a unit (and its object) without physics. */
void units_teleport(units_t *s, game_t *g, int unit, const float pos[3], float yaw);
/* Holds a unit at `pos` every tick (a script's unit_suspended, an object carried by an attachment): like units_teleport but it keeps the unit on the ground and its settle count, so the body
 * is not swept every tick; call it every tick. */
void units_pin(units_t *s, game_t *g, int unit, const float pos[3], float yaw);
/* Gives a weapon (tag id) to a unit; resets the weapon state. */
void units_give_weapon(units_t *s, game_t *g, int unit, uint32_t weapon_tag);
/* Eye / aim origin of a unit and its current aim direction. */
void units_eye(const units_t *s, int unit, float eye[3], float dir[3]);
/* Hostility between teams (Halo's default relations: player+human friendly to each other, covenant hostile to both, flood hostile to all,
 * sentinels hostile to flood and covenant-neutral). */
int  units_teams_hostile(int a, int b);

/* ---- additions of the units workstream ---- */
/* Recorded animations (cinedef_ctl: throttle x forward / y left, aim in counts of pi/1000, control flags, weapon index) as the control of
 * a unit: call once per tick with the recording's `cine_play.ctl`. Flags map CROUCH, JUMP, PRIMARY_FIRE, SECONDARY_FIRE, GRENADE, MELEE and
 * RELOAD; the others are ignored. */
void units_apply_cinedef(units_t *s, int unit, const cinedef_ctl *c);
/* Script / command-list animation: plays animation `anim` of ANIM graph `graph` (-1 = the unit's own graph) once at `frame_scale` x speed
 * (action UA_CUSTOM, the unit stands still); at the end the unit goes back to its normal animation unless `hold` keeps the last frame
 * until units_stop_custom. Returns the ticks it lasts (0 for a bad animation: nothing happens). */
unsigned units_custom_animation(units_t *s, game_t *g, int unit, int graph, int anim, float frame_scale, int hold);
void units_stop_custom(units_t *s, game_t *g, int unit);
/* The animation (index in the unit's graph, -1 none) the unit uses for a state: UA_IDLE, UA_MOVE (dir 0 front 1 back 2 left 3 right), UA_AIRBORNE,
 * UA_LAND (dir 0 soft 1 hard), UA_FIRE, UA_RELOAD, UA_MELEE, UA_THROW, UA_FLINCH and UA_DEAD (dir 0 hit from the front 1 left 2 right 3 back, the
 * damage table's order), in its current posture (crouch) with its weapon.
 * `*is_overlay` (may be NULL) is set to 1 when the animation is an aim / fire overlay that is not played as the base animation. */
int  units_lookup_anim(const units_t *s, const game_t *g, int unit, unit_action action, unsigned dir, int *is_overlay);
/* Recomputes everything derived from other state after the units were copied in (checkpoint revert, memory-card load): the object slot
 * index; units whose object no longer exists are detached. Returns the number of active units. */
unsigned units_rebind(units_t *s, const game_t *g);
size_t   units_state_size(void);                          /* sizeof(units_t) for the checkpoint code */
/* The events of the last units_tick (valid until the next one). */
const unit_event_t *units_events(const units_t *s, unsigned *count);

#endif
