/* WEAP section reader and weapon state machine (docs/18-weap-section.md): weapons (magazines, triggers, firing
 * effects), projectiles (physics, guidance, arming, material responses), damage effects, equipment, grenades and
 * item collections of one map. Contract between tools/halo2ps2/weapconv.py (writer), sim/weapdef.c (reader and
 * simulation, host-tested, pure C11, no heap, no double) and the engine's weapon, projectile and damage systems.
 *
 * Units: seconds in the data are converted by the helpers below to ticks of 1/30 s (WEAP_TICKS_PER_SEC); world
 * units; angles in radians; projectile velocities are WORLD UNITS PER TICK exactly as the tags store them
 * (x 30 = world units per second); damage amounts are in the unit of the units' health and shield (docs/18).
 * Cross references are Xbox tag ids (u32, 0 = none): they key the MODL, ANIM, SND0, FXD0 and HUD0 sections and
 * this section's own tables (weap_find_*). Records are decoded with byte loads: no alignment assumptions. */
#ifndef SIM_WEAPDEF_H
#define SIM_WEAPDEF_H
#include <stddef.h>
#include <stdint.h>

#define WEAP_TICKS_PER_SEC 30
#define WEAP_NAME_LEN 32
#define WEAP_LABEL_LEN 16
#define WEAP_MAX_MAGAZINES 4
#define WEAP_MAX_TRIGGERS 4
#define WEAP_MAX_FX 16
#define WEAP_MAX_MATERIALS 33
#define WEAP_NO_MAGAZINE 0xFFFFu
#define WEAP_DEFAULT_READY_TICKS 15        /* weapons without a first person ready animation */
#define WEAP_DEFAULT_PUT_AWAY_TICKS 10
#define WEAP_DEFAULT_RELOAD_TICKS 60
#define WEAP_DEFAULT_MELEE_TICKS 20

typedef enum { WEAP_OK = 0, WEAP_ERR_TRUNCATED, WEAP_ERR_MAGIC, WEAP_ERR_LAYOUT, WEAP_ERR_VERSION } weap_status;

/* weapon flags (weap_weapon.flags) */
#define WEAP_F_VERTICAL_HEAT        0x0001u
#define WEAP_F_MUTUALLY_EXCLUSIVE   0x0002u   /* only one trigger at a time */
#define WEAP_F_MUST_BE_READIED      0x0008u
#define WEAP_F_AIM_ASSIST_ZOOM_ONLY 0x0020u
#define WEAP_F_NO_GRENADES          0x0040u
#define WEAP_F_NO_MELEE             0x0200u
#define WEAP_F_DETONATES_DROPPED    0x0400u
#define WEAP_F_MAX_AGE_DEAD         0x0800u   /* cannot fire at maximum age (battery) */
#define WEAP_F_NIGHT_VISION         0x4000u
/* magazine flags */
#define WEAP_MAG_WASTES_ROUNDS      0x0001u   /* unused rounds are lost on reload */
#define WEAP_MAG_CHAMBER_EACH       0x0002u   /* every round must be chambered */
/* trigger flags */
#define WEAP_T_PARTIAL_AMMO         0x0004u   /* can fire with less than rounds-per-shot loaded */
#define WEAP_T_NO_REPEAT            0x0008u   /* one shot per trigger pull */
#define WEAP_T_ANALOG_RATE          0x0200u
#define WEAP_T_ERROR_UNZOOMED       0x0400u
#define WEAP_T_WEAPON_ORIGIN        0x0020u
#define WEAP_T_IDENTICAL_ERROR      0x1000u
/* projectile flags */
#define WEAP_P_ORIENTED             0x0001u
#define WEAP_P_BALLISTIC_AIM        0x0002u
#define WEAP_P_SUPER_COMBINE        0x0008u
/* projectile material response */
enum { WEAP_RESP_DISAPPEAR = 0, WEAP_RESP_DETONATE, WEAP_RESP_REFLECT, WEAP_RESP_OVERPENETRATE, WEAP_RESP_ATTACH };
/* equipment powerup types / grenade types */
enum { WEAP_POWERUP_NONE = 0, WEAP_POWERUP_DOUBLE_SPEED, WEAP_POWERUP_OVER_SHIELD, WEAP_POWERUP_ACTIVE_CAMO,
       WEAP_POWERUP_FULL_SPECTRUM, WEAP_POWERUP_HEALTH, WEAP_POWERUP_GRENADE };
enum { WEAP_GRENADE_FRAG = 0, WEAP_GRENADE_PLASMA };
/* damage categories */
enum { WEAP_CAT_NONE = 0, WEAP_CAT_FALLING, WEAP_CAT_BULLET, WEAP_CAT_GRENADE, WEAP_CAT_EXPLOSION, WEAP_CAT_SNIPER,
       WEAP_CAT_MELEE, WEAP_CAT_FLAME, WEAP_CAT_MOUNTED, WEAP_CAT_VEHICLE, WEAP_CAT_PLASMA, WEAP_CAT_NEEDLE,
       WEAP_CAT_SHOTGUN };

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       n_weapons, n_magazines, n_triggers, n_fx, n_projectiles, n_materials, n_damages, n_equipment,
	               n_grenades, n_collections, n_permutations;
	const uint8_t *weapons, *magazines, *triggers, *fx, *projectiles, *materials, *damages, *equipment, *grenades,
	              *collections, *permutations;
} weap_section;

typedef struct {
	uint32_t flags;
	uint16_t rounds_recharged, rounds_total_initial, rounds_total_max, rounds_loaded_max, rounds_reloaded;
	uint16_t equipment_count;
	float    reload_time, chamber_time;         /* seconds */
	uint32_t reload_effect, chamber_effect;
	uint32_t equipment_tag[2];                  /* ammunition pickups of this magazine ... */
	uint16_t equipment_rounds[2];               /* ... and the rounds each gives */
} weap_magazine;

typedef struct {
	uint32_t flags;
	float    rps_min, rps_max, accel_time, decel_time, blurred_rate;   /* rounds per second, seconds */
	uint16_t magazine;                          /* WEAP_NO_MAGAZINE: no ammunition accounting */
	uint16_t rounds_per_shot, min_rounds_loaded, tracer_interval;
	uint16_t firing_noise, projectiles_per_shot, distribution_function, overcharged_action;
	float    error_accel_time, error_decel_time, minimum_error, error_min, error_max, distribution_angle;   /* radians */
	float    fp_offset[3];
	float    charging_time, charged_time, charged_illumination, spew_time;
	float    ejection_recovery, illumination_recovery;
	float    heat_per_round, age_per_round;     /* 0..1 of the weapon's heat / battery scale per round */
	uint32_t projectile, charging_effect;
	uint16_t first_fx, fx_count;                /* weap_get_fx(): firing effects of this trigger */
} weap_trigger;

typedef struct {
	uint16_t shots_lo, shots_hi;
	uint32_t effects[3];                        /* firing, misfire, empty: effect or sound tag (see sound_mask) */
	uint8_t  sound_mask;                        /* bit i: effects[i] is a sound tag */
	uint32_t damages[3];                        /* firing, misfire, empty: damage effect tag (camera shake, rumble) */
} weap_fx;

typedef struct {
	uint32_t tag_id, model_tag, anim_tag, fp_model_tag, fp_anim_tag, hud_tag;
	uint32_t pickup_sound, zoom_in_sound, zoom_out_sound, collision_sound;
	uint32_t ready_effect, overheated_effect, detonation_effect, melee_damage, melee_response;
	uint32_t flags, item_flags;
	uint16_t secondary_mode, max_alternate_shots, weapon_type, zoom_levels;
	float    zoom_min, zoom_max;
	float    heat_recovery, overheated, heat_detonation, heat_detonation_fraction, heat_loss, heat_illumination;
	float    near_reticle, far_reticle, intersection_reticle;
	float    autoaim_angle, autoaim_range, magnetism_angle, magnetism_range, deviation_angle;
	float    camo_ding, camo_regrowth, item_scale, detonation_delay[2];
	uint16_t hud_message, sort_order;
	uint16_t fp_ticks[8];                       /* ready, put away, reload empty, reload full, melee, overheated, throw, fire
	                                               (0 = the first person graph has no such animation) */
	uint8_t  magazine_count, trigger_count;
	char     label[WEAP_LABEL_LEN];             /* animation / weapon-type code ("ar", "pr", "hp", ...) */
	char     name[WEAP_NAME_LEN];               /* tag path tail */
	weap_magazine magazine[WEAP_MAX_MAGAZINES];
	weap_trigger  trigger[WEAP_MAX_TRIGGERS];
} weap_weapon;
enum { WEAP_FP_READY, WEAP_FP_PUT_AWAY, WEAP_FP_RELOAD_EMPTY, WEAP_FP_RELOAD_FULL, WEAP_FP_MELEE, WEAP_FP_OVERHEATED,
       WEAP_FP_THROW, WEAP_FP_FIRE };

typedef struct {
	uint16_t flags, response;                   /* WEAP_RESP_* */
	uint32_t effect;
	uint16_t potential_response, potential_flags;
	float    skip_fraction, angle[2], velocity[2];
	uint32_t potential_effect;
	float    angular_noise, velocity_noise;
	uint32_t detonation_effect;
	float    initial_friction, max_distance, parallel_friction, perpendicular_friction;
} weap_material_response;

typedef struct {
	uint32_t tag_id, model_tag, flags;
	uint16_t timer_starts, impact_noise;
	float    ai_perception_radius, collision_radius, arming_time, danger_radius;
	float    timer[2];                          /* detonation timer, seconds */
	float    min_velocity, max_range, air_gravity, air_range[2], water_gravity, water_range[2];
	float    initial_velocity, final_velocity;  /* world units per TICK */
	float    guided_angular_velocity;           /* radians per second */
	uint16_t detonation_noise;
	uint32_t super_detonation, detonation_effect, detonation_started, flyby_sound, attached_damage, impact_damage;
	uint16_t first_material, material_count;
	float    object_radius;
	char     name[WEAP_NAME_LEN];
} weap_projectile;

typedef struct {
	uint32_t tag_id;
	float    radius[2], cutoff_scale;
	uint32_t effect_flags;
	uint16_t flash_type, flash_priority;
	float    flash_duration;
	uint16_t flash_fade;
	float    flash_intensity, flash_color[4];   /* a, r, g, b */
	float    low_rumble[2], high_rumble[2];     /* frequency, duration (s) */
	uint16_t low_rumble_fade, high_rumble_fade;
	float    impulse_duration;
	uint16_t impulse_fade;
	float    impulse_rotation, impulse_pushback, impulse_jitter[2], permanent_angle;
	float    shake_duration;
	uint16_t shake_falloff, shake_wobble_function;
	float    shake_translation, shake_rotation, shake_wobble_period, shake_wobble_weight;
	uint32_t sound;
	float    break_forward[3], break_outward[3];
	uint16_t side_effect, category;
	uint32_t damage_flags;
	float    aoe_core_radius, damage_lower, damage_upper[2], vehicle_passthrough, active_camo, stun, max_stun, stun_time;
	float    modifiers[WEAP_MAX_MATERIALS];
	char     name[WEAP_NAME_LEN];
} weap_damage;

typedef struct {
	uint32_t tag_id, model_tag, anim_tag, item_flags;
	uint16_t powerup_type, grenade_type;
	float    powerup_time;
	uint32_t pickup_sound, collision_sound, detonating_effect, detonation_effect;
	float    detonation_delay[2], item_scale;
	uint16_t hud_message;
	char     name[WEAP_NAME_LEN];
} weap_equipment;

typedef struct {
	uint16_t max_count, mp_spawn_default;
	uint32_t throw_effect, hud_tag, equipment, projectile;
} weap_grenade;

weap_status weap_open(weap_section *s, const uint8_t *data, size_t size);
int  weap_find_weapon(const weap_section *s, uint32_t tag_id);        /* index or -1 */
int  weap_find_projectile(const weap_section *s, uint32_t tag_id);
int  weap_find_damage(const weap_section *s, uint32_t tag_id);
int  weap_find_equipment(const weap_section *s, uint32_t tag_id);
int  weap_find_collection(const weap_section *s, uint32_t tag_id);
/* The getters return 1 and fill `out`, or return 0 (out zeroed) for an index out of range. */
int  weap_get_weapon(const weap_section *s, uint32_t index, weap_weapon *out);
int  weap_get_fx(const weap_section *s, const weap_trigger *t, uint32_t k, weap_fx *out);
int  weap_get_projectile(const weap_section *s, uint32_t index, weap_projectile *out);
int  weap_get_material(const weap_section *s, const weap_projectile *p, uint32_t material, weap_material_response *out);
int  weap_get_damage(const weap_section *s, uint32_t index, weap_damage *out);
int  weap_get_equipment(const weap_section *s, uint32_t index, weap_equipment *out);
int  weap_get_grenade(const weap_section *s, uint32_t type, weap_grenade *out);
/* Item collection: weight and tag of permutation k. */
int  weap_collection(const weap_section *s, uint32_t index, uint32_t *spawn_time, uint32_t *count);
int  weap_collection_entry(const weap_section *s, uint32_t index, uint32_t k, float *weight, uint32_t *item_tag);

/* ---- damage and projectile helpers (pure functions) ---- */
/* Damage of a projectile that has flown `dist` world units: `upper` (picked between damage_upper[0..1] with rand01 in
 * 0..1) up to air_range[0], falling linearly to damage_lower at air_range[1] (docs/18). */
float weap_damage_at_range(const weap_projectile *p, const weap_damage *d, float dist, float rand01);
/* Area damage at distance `dist` from the centre: full inside aoe_core_radius, falling linearly to damage_lower at
 * radius[1], 0 beyond it. A damage effect with radius[1] == 0 (bullets) does not spread. */
float weap_damage_at_radius(const weap_damage *d, float dist, float rand01);
/* Velocity along the flight: from initial to final velocity over the tick range [0, lifetime ticks]. */
float weap_projectile_speed(const weap_projectile *p, uint32_t age_ticks, uint32_t lifetime_ticks);

/* ---- weapon state machine (30 Hz, pure) ---- */
typedef enum {
	WEAP_MODE_READYING = 0, WEAP_MODE_IDLE, WEAP_MODE_RELOADING, WEAP_MODE_OVERHEATED, WEAP_MODE_MELEE,
	WEAP_MODE_PUTTING_AWAY, WEAP_MODE_STOWED
} weap_mode;

typedef struct {
	uint8_t  mode;                              /* weap_mode */
	uint8_t  prev_fire;                         /* bit t: trigger t was held last tick */
	uint8_t  zoom;                              /* current zoom level, 0 = not zoomed */
	uint8_t  reload_mag;                        /* magazine being reloaded */
	uint16_t timer;                             /* ticks left in READYING / RELOADING / MELEE / PUTTING_AWAY */
	uint16_t melee_hit_at;                      /* timer value when the melee damage lands (0 = landed) */
	uint16_t loaded[WEAP_MAX_MAGAZINES];
	uint16_t reserve[WEAP_MAX_MAGAZINES];
	uint32_t wasted;                            /* rounds thrown away by "wastes rounds when reloaded" */
	uint32_t rounds_fired;                      /* rounds consumed since init (ammo accounting) */
	uint32_t shots;                             /* shots fired since init (tracer interval) */
	float    cooldown[WEAP_MAX_TRIGGERS];       /* ticks until the trigger may fire again (fractional carry) */
	float    rate[WEAP_MAX_TRIGGERS];           /* current rounds per second (acceleration) */
	float    error[WEAP_MAX_TRIGGERS];          /* 0..1 position between error_min and error_max */
	float    charge[WEAP_MAX_TRIGGERS];         /* ticks the trigger has been charging (whole ticks) */
	uint8_t  charged_sent;                      /* bit t: WEAP_EV_CHARGED already reported for this pull */
	float    heat, age;                         /* 0..1 */
} weap_state;

typedef struct {
	uint8_t fire[WEAP_MAX_TRIGGERS];            /* trigger held */
	uint8_t reload, melee, zoom, switch_out;    /* pressed this tick */
} weap_input;

#define WEAP_EV_READY        0x0001u
#define WEAP_EV_DRY_FIRE     0x0002u            /* trigger pulled with nothing to fire (empty effect) */
#define WEAP_EV_RELOAD_START 0x0004u
#define WEAP_EV_RELOAD_DONE  0x0008u
#define WEAP_EV_OVERHEATED   0x0010u
#define WEAP_EV_COOLED       0x0020u
#define WEAP_EV_CHARGING     0x0040u            /* a trigger started charging */
#define WEAP_EV_CHARGED      0x0080u            /* a trigger reached its charging time */
#define WEAP_EV_MELEE_START  0x0100u
#define WEAP_EV_MELEE_HIT    0x0200u
#define WEAP_EV_PUT_AWAY     0x0400u
#define WEAP_EV_ZOOM         0x0800u
#define WEAP_EV_BATTERY_DEAD 0x1000u
#define WEAP_EV_AUTO_RELOAD  0x2000u            /* RELOAD_START came from an empty magazine */
#define WEAP_EV_RELOAD_CANCEL 0x4000u           /* a shell-by-shell reload was interrupted by firing */

typedef struct {
	uint32_t flags;                             /* WEAP_EV_* */
	uint8_t  shot[WEAP_MAX_TRIGGERS];           /* 1: the trigger fired this tick */
	uint8_t  projectiles[WEAP_MAX_TRIGGERS];    /* projectiles to spawn for that shot */
	uint8_t  tracer[WEAP_MAX_TRIGGERS];         /* this shot is a tracer */
	float    error[WEAP_MAX_TRIGGERS];          /* spread angle of the shot (radians) */
	float    charge[WEAP_MAX_TRIGGERS];         /* 0..1 charge of the shot (1 for triggers without charging) */
	uint16_t rounds_used;                       /* rounds consumed by all shots of this tick */
} weap_events;

uint16_t weap_ready_ticks(const weap_weapon *w);
uint16_t weap_put_away_ticks(const weap_weapon *w);
uint16_t weap_melee_ticks(const weap_weapon *w);
/* Duration of one reload cycle of magazine `mag` in ticks: the tag's reload time, else the first person animation. */
uint16_t weap_reload_ticks(const weap_weapon *w, unsigned mag, int empty);
/* Ticks between shots of trigger `t` at its current rate (fractional), 0 = not rate limited. */
float    weap_shot_interval(const weap_weapon *w, unsigned t, float rate);

/* Fresh weapon, being readied: every magazine loaded to its maximum from rounds_total_initial. */
void weapon_state_init(weap_state *st, const weap_weapon *w);
void weapon_state_set_ammo(weap_state *st, const weap_weapon *w, unsigned mag, unsigned loaded, unsigned reserve);
void weapon_fire_step(weap_state *st, const weap_weapon *w, const weap_input *in, weap_events *ev);

#endif
