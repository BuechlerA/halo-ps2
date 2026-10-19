/* UNIT section reader (docs/17-unit-section.md): the player constants of the `globals` tag, the material table, every
 * biped of the map and the basic data of its vehicles. Contract between tools/halo2ps2/unitconv.py (writer),
 * sim/unitdef.c (reader, host-tested, pure C11, no heap, no double) and the player / unit controllers.
 *
 * Everything is stored as the Xbox tags have it; ids are Xbox TAG IDS (0 = none) that key the other packs (MODL, ANIM,
 * the weapon, sound and effect packs). UNITS (proved against real data in the doc):
 *   - one tick = 1/30 s; one world unit (wu) = 10 ft = 3.048 m
 *   - `gravity` is wu/tick^2 (0.003565 on every map = 3.2085 wu/s^2); `jump_velocity` and every vehicle speed /
 *     acceleration are wu/tick (wu/tick^2); the speeds of `unitdef_globals` (run_forward, ...) are wu/SECOND; the
 *     biped's landing velocities are wu/second; times marked (s) are seconds; angles are radians except where a
 *     field says degrees.
 * Missing records are neutral (zero), never garbage: a failed unitdef_open leaves the section zeroed. */
#ifndef SIM_UNITDEF_H
#define SIM_UNITDEF_H
#include <stddef.h>
#include <stdint.h>

#define UNITDEF_NAME_LEN 32
#define UNITDEF_MAX_WEAPONS 4
#define UNITDEF_MAX_DIALOGUE 8
#define UNITDEF_DIFFICULTY_GROUPS 31
#define UNITDEF_MAX_MATERIALS 64
#define UNITDEF_NO_NODE 0xFFFFu            /* pelvis_node / head_node when the biped has none */
#define UNITDEF_TICKS_PER_SECOND 30

typedef enum { UNITDEF_OK = 0, UNITDEF_ERR_TRUNCATED, UNITDEF_ERR_MAGIC, UNITDEF_ERR_LAYOUT, UNITDEF_ERR_VERSION } unitdef_status;

typedef enum {
	UNITDEF_VEH_TANK = 0, UNITDEF_VEH_JEEP, UNITDEF_VEH_BOAT, UNITDEF_VEH_PLANE, UNITDEF_VEH_SCOUT, UNITDEF_VEH_FIGHTER,
	UNITDEF_VEH_TURRET
} unitdef_vehicle_type;

/* difficulty groups: unitdef_difficulty(s, group, level) with level 0 easy, 1 normal, 2 hard, 3 impossible */
enum {
	UNITDEF_D_ENEMY_DAMAGE = 0, UNITDEF_D_ENEMY_VITALITY, UNITDEF_D_ENEMY_SHIELD, UNITDEF_D_ENEMY_RECHARGE,
	UNITDEF_D_FRIEND_DAMAGE, UNITDEF_D_FRIEND_VITALITY, UNITDEF_D_FRIEND_SHIELD, UNITDEF_D_FRIEND_RECHARGE,
	UNITDEF_D_INFECTION_FORMS, /* 9 is zero */ UNITDEF_D_RATE_OF_FIRE = 10, UNITDEF_D_PROJECTILE_ERROR, UNITDEF_D_BURSTINESS,
	UNITDEF_D_NEW_TARGET_DELAY, UNITDEF_D_BURST_SEPARATION, UNITDEF_D_TARGET_TRACKING, UNITDEF_D_TARGET_LEADING,
	UNITDEF_D_OVERCHARGE_CHANCE, UNITDEF_D_SPECIAL_FIRE_DELAY, UNITDEF_D_GUIDANCE_VS_PLAYER, UNITDEF_D_MELEE_DELAY_BASE,
	UNITDEF_D_MELEE_DELAY_SCALE, /* 22 is zero */ UNITDEF_D_GRENADE_CHANCE_SCALE = 23, UNITDEF_D_GRENADE_TIMER_SCALE
};

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       biped_count, vehicle_count, material_count, seat_count;
	const uint8_t *globals, *bipeds, *vehicles, *materials, *seats;
	uint32_t       player_biped_tag;          /* the globals' default player biped (0 none) */
	uint32_t       player_biped_index1;       /* its index + 1 in the section (0: not present) */
} unitdef_section;

typedef struct { uint16_t max_count, unk; uint32_t throw_effect, hud_interface, equipment, projectile; } unitdef_grenade;

/* globals: player information, player control, falling damage, grenades, difficulty */
typedef struct {
	uint32_t default_biped_tag, coop_effect_tag, camera_track_tag, water_enter_sound, water_exit_sound;
	/* player information. Speeds are wu/SECOND (run_forward 2.25 = the cyborg's run animation); the accelerations
	 * are in the tag's own unit (not proved); `pi_unk_*` are fields of unknown meaning (0.512 and 0.05). */
	float walk_speed, run_forward, run_backward, run_sideways, run_accel;
	float sneak_forward, sneak_backward, sneak_sideways, sneak_accel, airborne_accel, pi_unk_2c, pi_unk_6c;
	float stun_move_penalty, stun_turn_penalty, stun_jump_penalty, stun_min_time, stun_max_time;   /* times (s) */
	float fp_idle_min, fp_idle_max, fp_skip_fraction;                                              /* (s), (s), fraction */
	/* player control. look_*: degrees per second for the two rates; look_a/b/c/f are the acceleration group (unit and
	 * order unverified: 1.0, 3.0, 0.85, 0.5) */
	float magnetism_friction, magnetism_adhesion, inconsequential_scale;
	float look_a, look_b, look_c, look_pitch_rate_deg, look_yaw_rate_deg, look_f;
	uint16_t min_weapon_swap_ticks, min_autolevel_ticks;
	float min_vehicle_flip_angle;           /* radians */
	float look_function[6];                 /* response curve of the stick deflection, 0..1 */
	/* falling damage */
	float harmful_fall_min, harmful_fall_max, max_fall_distance;   /* wu */
	float gravity;                          /* wu/tick^2, derived from the three cached falling velocities */
	float fall_velocity_max, fall_velocity_harm_min, fall_velocity_harm_max;   /* wu/tick at 16.5 / 3 / 6 wu */
	uint32_t damage_tags[6];                /* jpt!: falling, distance, vehicle environment, vehicle killed unit, vehicle collision, flaming death */
	unitdef_grenade grenades[2];            /* 0 fragmentation, 1 plasma */
} unitdef_globals_def;

typedef struct {
	uint32_t tag_id, model_tag, anim_tag, collision_tag, physics_tag, camera_track_tag, footsteps_tag, melee_damage_tag;
	uint32_t weapon_tags[UNITDEF_MAX_WEAPONS];           /* default weapons (weap tag ids), weapon_count valid */
	uint16_t dialogue_variant[UNITDEF_MAX_DIALOGUE];     /* dialogue_count valid */
	uint32_t dialogue_tag[UNITDEF_MAX_DIALOGUE];         /* udlg tag ids */
	float    bounding_radius, bounding_offset[3], render_radius, accel_scale;   /* accel_scale: "scales gravity" (unverified) */
	float    camera_fov, camera_stiffness, pitch_auto_level, pitch_range[2];     /* fov radians; the look limits are not in the tags */
	/* collision pill: radius, standing / crouching height measured from the feet (model height 0.7 on the player) */
	float    stand_height, crouch_height, radius, autoaim_width;
	float    stand_camera, crouch_camera, crouch_time;                          /* camera heights above the feet (wu), (s) */
	float    jump_velocity;                                                    /* wu/tick */
	float    max_slope, downhill_falloff, downhill_cutoff, downhill_scale, uphill_falloff, uphill_cutoff, uphill_scale;  /* radians, scales */
	float    turn_speed, turn_threshold;                                       /* moving turning speed (rad/s), stationary threshold (rad) */
	float    soft_landing_time, hard_landing_time;                             /* (s) */
	float    min_soft_landing_velocity, min_hard_landing_velocity, max_hard_landing_velocity, death_landing_velocity;   /* wu/s */
	/* the tag's own motion block: zero on every walking biped (player speeds are in globals, AI speeds are root motion),
	 * non-zero on the monitor, sentinels and engineers (flyers) */
	float    max_velocity, max_sidestep_velocity, acceleration, deceleration, angular_velocity_max, angular_acceleration_max, crouch_velocity_modifier;
	float    aiming_velocity_max, aiming_acceleration_max, casual_aiming_modifier, looking_velocity_max, looking_acceleration_max;
	float    anim_speed[4];                                                    /* wu/s: stand run front / back / side, crouch front; from root motion, 0 = none */
	float    grenade_velocity;
	float    health, shield, shield_recharge_time, shield_stun_time, shield_recharge_rate;   /* vitals; (s); per tick */
	uint32_t unit_flags, biped_flags, coll_flags;
	uint16_t object_flags, default_team, pelvis_node, head_node, shield_material, body_material, grenade_type, grenade_count;
	uint8_t  weapon_count, dialogue_count;
	char     name[UNITDEF_NAME_LEN], hold_marker[UNITDEF_NAME_LEN], aux_marker[UNITDEF_NAME_LEN], camera_marker[UNITDEF_NAME_LEN];
	char     foot_marker[2][UNITDEF_NAME_LEN];
} unitdef_biped_def;

typedef struct {
	uint32_t tag_id, model_tag, anim_tag, collision_tag, physics_tag, camera_track_tag;
	uint32_t weapon_tags[UNITDEF_MAX_WEAPONS];
	float    bounding_radius, bounding_offset[3], accel_scale, camera_fov;
	float    max_forward, max_reverse, accel, decel;            /* wu/tick, wu/tick^2 */
	float    max_left_turn, max_right_turn;                      /* degrees, as the tag has them (fliers use other units) */
	float    wheel_circumference, turn_rate;
	float    health, shield, shield_recharge_time;
	uint32_t unit_flags, vehicle_flags, coll_flags;
	uint16_t type;                                               /* unitdef_vehicle_type */
	uint16_t first_seat, seat_count;                             /* range in the seat table */
	uint16_t object_flags, default_team, shield_material;
	uint8_t  weapon_count;
	char     name[UNITDEF_NAME_LEN];
} unitdef_vehicle_def;

typedef struct {
	uint32_t flags;                                              /* seat flags as the tag has them (driver, gunner, ...) */
	float    pitch_min, pitch_max;                               /* camera pitch limits, radians */
	char     label[UNITDEF_NAME_LEN], marker[UNITDEF_NAME_LEN], camera_marker[UNITDEF_NAME_LEN];
} unitdef_seat_def;

typedef struct {
	uint32_t melee_sound;                                        /* snd! tag id of the melee hit sound */
	uint32_t impact_sound, effect, particle_tag;                 /* sweetener sound, effect (null everywhere), first particle of the particle effects */
	float    param_a, param_b;                                   /* unknown floats (ice 0.125, glass 10) */
	uint16_t particle_count;
} unitdef_material_def;

unitdef_status unitdef_open(unitdef_section *s, const uint8_t *data, size_t size);

void unitdef_globals(const unitdef_section *s, unitdef_globals_def *out);        /* zeroed when the section is not open */
/* difficulty value of `group` (UNITDEF_D_*) for `level` 0..3; 0 out of range */
float unitdef_difficulty(const unitdef_section *s, uint32_t group, uint32_t level);

/* Bipeds are sorted by tag id. unitdef_find_biped: index or -1; unitdef_get_biped(index) and unitdef_biped(tag_id) fill
 * `out` (neutral when absent: tag_id 0, pelvis/head UNITDEF_NO_NODE) and return 1 when found. */
int  unitdef_find_biped(const unitdef_section *s, uint32_t tag_id);
int  unitdef_get_biped(const unitdef_section *s, uint32_t index, unitdef_biped_def *out);
int  unitdef_biped(const unitdef_section *s, uint32_t tag_id, unitdef_biped_def *out);
/* the globals' default player biped: 1 when the section has it */
int  unitdef_player_biped(const unitdef_section *s, unitdef_biped_def *out);

int  unitdef_find_vehicle(const unitdef_section *s, uint32_t tag_id);
int  unitdef_get_vehicle(const unitdef_section *s, uint32_t index, unitdef_vehicle_def *out);
int  unitdef_vehicle(const unitdef_section *s, uint32_t tag_id, unitdef_vehicle_def *out);
/* seat `i` (0 .. seat_count - 1) of a vehicle; 0 when out of range */
int  unitdef_vehicle_seat(const unitdef_section *s, const unitdef_vehicle_def *v, uint32_t i, unitdef_seat_def *out);

/* material type `index` (0 .. material_count - 1; 32 on the retail maps); neutral (zero) when out of range. */
void unitdef_material(const unitdef_section *s, uint32_t index, unitdef_material_def *out);
/* name of a standard material type ("dirt", ..., "cyborg energy shield", ...), "?" beyond the table. Engine knowledge,
 * not tag data (debug output only). */
const char *unitdef_material_name(uint32_t index);

#endif
