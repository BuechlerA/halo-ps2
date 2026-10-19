/* AIDT section reader (docs/25-ai-section.md): the `actor` and `actor_variant` tags of a map and the scenario's AI data:
 * encounters with their squads, platoons, starting locations, move and firing positions, command lists (atoms and
 * points), AI conversations, the AI animation / script references and the name tables scripts use. Contract between
 * tools/halo2ps2/aiconv.py (writer), ai/aidef.c (reader, host-tested, pure C11, no heap, no double) and the AI runtime.
 *
 * Everything is stored as the Xbox tags have it; ids are Xbox TAG IDS (0 = none) that key the other packs (units in UNIT, weapons in
 * WEAP, models, sounds in SND0 / STRM). UNITS (the cached values of the tags prove them): one tick = 1/30 s; one world unit (wu) =
 * 10 ft = 3.048 m; times are seconds unless a field says ticks; distances are wu; angles are radians; chances and fractions are
 * 0..1. Indices into other tables are -1 when "none"; the reader refuses an index that does not exist.
 * Records that are out of range give a neutral (zero, -1 for indices) struct and a return of 0; a failed aidef_open leaves the
 * section zeroed, so every accessor works on it. The tag offsets named below ("tag +0x74") are the offsets of the Xbox tag and
 * of the raw record of the section; aidef_*_f32() / _u32() read any field by tag offset, also those that have no name here. */
#ifndef AI_AIDEF_H
#define AI_AIDEF_H
#include <stddef.h>
#include <stdint.h>

#define AIDEF_NAME_LEN 32
#define AIDEF_MAX_DIFFICULTY 3             /* 0 easy, 1 normal, 2 hard, 3 impossible (as unitdef) */
#define AIDEF_NO_INDEX 0xFFFFFFFFu         /* palette entries without a variant */
#define AIDEF_ACTOR_RAW_SIZE 1272u
#define AIDEF_VARIANT_RAW_SIZE 568u

typedef enum { AIDEF_OK = 0, AIDEF_ERR_TRUNCATED, AIDEF_ERR_MAGIC, AIDEF_ERR_LAYOUT, AIDEF_ERR_VERSION } aidef_status;

/* actor.type [V: the numbers of the tag paths on the ten maps] */
typedef enum {
	AIDEF_ACTOR_ELITE = 0, AIDEF_ACTOR_JACKAL, AIDEF_ACTOR_GRUNT, AIDEF_ACTOR_HUNTER, AIDEF_ACTOR_ENGINEER, AIDEF_ACTOR_ASSASSIN,
	AIDEF_ACTOR_PLAYER, AIDEF_ACTOR_MARINE, AIDEF_ACTOR_CREW, AIDEF_ACTOR_COMBAT_FORM, AIDEF_ACTOR_INFECTION_FORM,
	AIDEF_ACTOR_CARRIER_FORM, AIDEF_ACTOR_MONITOR, AIDEF_ACTOR_SENTINEL, AIDEF_ACTOR_NONE, AIDEF_ACTOR_MOUNTED_WEAPON,
	AIDEF_ACTOR_TYPES
} aidef_actor_type;

/* squad initial / return state [V: 0..11 on every one of the 2988 squads] */
typedef enum {
	AIDEF_STATE_NONE = 0, AIDEF_STATE_SLEEPING, AIDEF_STATE_ALERT, AIDEF_STATE_MOVING_REPEAT_SAME_POSITION,
	AIDEF_STATE_MOVING_LOOP, AIDEF_STATE_MOVING_LOOP_BACK_AND_FORTH, AIDEF_STATE_MOVING_LOOP_RANDOMLY,
	AIDEF_STATE_MOVING_RANDOMLY, AIDEF_STATE_GUARDING, AIDEF_STATE_GUARDING_AT_GUARD_POSITION, AIDEF_STATE_SEARCHING,
	AIDEF_STATE_FLEEING, AIDEF_STATES
} aidef_actor_state;

/* encounter.team and the `default_team` of unitdef bipeds [V: bipeds of the ten maps use 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel] */
typedef enum {
	AIDEF_TEAM_DEFAULT_BY_UNIT = 0, AIDEF_TEAM_PLAYER, AIDEF_TEAM_HUMAN, AIDEF_TEAM_COVENANT, AIDEF_TEAM_FLOOD,
	AIDEF_TEAM_SENTINEL, AIDEF_TEAM_UNUSED6, AIDEF_TEAM_UNUSED7, AIDEF_TEAM_UNUSED8, AIDEF_TEAM_UNUSED9, AIDEF_TEAMS
} aidef_team;

/* encounter flags [V: bits 0, 5, 6 appear with these meanings, bit 1 never; bits 2 and 3 appear together with them] */
enum {
	AIDEF_ENC_NOT_INITIALLY_CREATED = 1, AIDEF_ENC_RESPAWN_ENABLED = 2, AIDEF_ENC_INITIALLY_BLIND = 4,
	AIDEF_ENC_INITIALLY_DEAF = 8, AIDEF_ENC_INITIALLY_BRAINDEAD = 16, AIDEF_ENC_3D_FIRING_POSITIONS = 32,
	AIDEF_ENC_MANUAL_BSP_INDEX = 64
};
/* squad flags [P: the public tag names; bit 5 on 1057 of 2988 squads] */
enum {
	AIDEF_SQUAD_UNUSED = 1, AIDEF_SQUAD_NEVER_SEARCH = 2, AIDEF_SQUAD_START_TIMER_IMMEDIATELY = 4,
	AIDEF_SQUAD_NO_TIMER_DELAY = 8, AIDEF_SQUAD_MAGIC_SIGHT_AFTER_TIMER = 16, AIDEF_SQUAD_AUTOMATIC_MIGRATION = 32
};
/* squad.unique_leader: 0 normal, 1 none, 2 random, 3 and 4 the two named unique leaders [P]; squad.major_upgrade: 0 normal, 1 few,
 * 2 many, 3 none, 4 all [P] */

/* command atom types [V: loop = 20 carries command indices, animate = 13 animation indices, script = 12 a script index, recording =
 * 14 never occurs (no recordings on any map); the rest by the order of the public tag documentation] */
typedef enum {
	AIDEF_ATOM_PAUSE = 0, AIDEF_ATOM_GO_TO, AIDEF_ATOM_GO_TO_AND_FACE, AIDEF_ATOM_MOVE_IN_DIRECTION, AIDEF_ATOM_LOOK,
	AIDEF_ATOM_ANIMATION_MODE, AIDEF_ATOM_CROUCH, AIDEF_ATOM_SHOOT, AIDEF_ATOM_GRENADE, AIDEF_ATOM_VEHICLE,
	AIDEF_ATOM_RUNNING_JUMP, AIDEF_ATOM_TARGETED_JUMP, AIDEF_ATOM_SCRIPT, AIDEF_ATOM_ANIMATE, AIDEF_ATOM_RECORDING,
	AIDEF_ATOM_ACTION, AIDEF_ATOM_VOCALIZE, AIDEF_ATOM_TARGETING, AIDEF_ATOM_INITIATIVE, AIDEF_ATOM_WAIT, AIDEF_ATOM_LOOP,
	AIDEF_ATOM_DIE, AIDEF_ATOM_MOVE_IMMEDIATE, AIDEF_ATOM_LOOK_RANDOM, AIDEF_ATOM_LOOK_PLAYER, AIDEF_ATOM_LOOK_OBJECT,
	AIDEF_ATOM_SET_RADIUS, AIDEF_ATOM_TELEPORT, AIDEF_ATOMS
} aidef_atom_type;

/* section tables (order of the directory in the header) */
typedef enum {
	AIDEF_T_ACTORS = 0, AIDEF_T_VARIANTS, AIDEF_T_ACTOR_IDS, AIDEF_T_VARIANT_IDS, AIDEF_T_PALETTE, AIDEF_T_ENCOUNTERS,
	AIDEF_T_SQUADS, AIDEF_T_STARTS, AIDEF_T_MOVES, AIDEF_T_FIRINGS, AIDEF_T_PLATOONS, AIDEF_T_PSTARTS, AIDEF_T_CMDLISTS,
	AIDEF_T_COMMANDS, AIDEF_T_POINTS, AIDEF_T_CONVS, AIDEF_T_PARTS, AIDEF_T_LINES, AIDEF_T_ANIMS, AIDEF_T_SCRIPTS,
	AIDEF_T_ENC_NAMES, AIDEF_T_CMD_NAMES, AIDEF_T_CONV_NAMES, AIDEF_TABLES
} aidef_table;

typedef struct {
	const uint8_t *data;
	size_t         size;
	const uint8_t *tab[AIDEF_TABLES];
	uint32_t       count[AIDEF_TABLES];
} aidef_section;

/* ---- actor (tag `actor`) ---- */
typedef struct {
	const uint8_t *raw;                          /* the record: AIDEF_ACTOR_RAW_SIZE bytes in the layout of the tag, references flattened (valid while the section is) */
	uint32_t tag_id;
	uint32_t flags, more_flags;                  /* tag +0x00, +0x04 (bits unnamed; the one mounted-weapon actor carries stray text in more_flags) */
	uint16_t type;                               /* aidef_actor_type, tag +0x14 */
	/* perception: distances in wu, angles in radians (the mounted weapon uses wide-open sentinel values above pi), tag +0x18..+0x2C */
	float max_vision_distance, central_vision_angle, max_vision_angle, peripheral_vision_angle, peripheral_distance;
	float standing_gun_offset[3], crouching_gun_offset[3];                 /* +0x34, +0x40 (the second is zero on every actor) */
	float hearing_distance, notice_projectile_chance, notice_vehicle_chance;   /* wu, fractions; +0x4C..+0x54 */
	float combat_perception_time, guard_perception_time, non_combat_perception_time;   /* seconds, +0x60..+0x68 */
	float inv_combat_perception_time, inv_guard_perception_time, inv_non_combat_perception_time;   /* cached 1 / (t * 30) per tick, +0x6C..+0x74 [V] */
	float fractions_80[4], values_90[3], angle_9c, angle_9c_cos;           /* +0x80..+0xA0: behaviour thresholds [U]; the angle (0.698 = 40 degrees) and its cosine [V] */
	float max_aiming_deviation[2], max_looking_deviation[2];               /* yaw, pitch; +0xA4..+0xB0 */
	float looking_angles[8];                                               /* +0xB4..+0xD0 look deltas and idle ranges [P] */
	float idle_times[10][2];                                               /* +0xD4..+0x120 (min, max) seconds [P] */
	float cos_aiming_deviation[2], cos_looking_deviation[2];               /* cached cosines of the four deviations, +0x12C..+0x138 [V] */
	uint32_t weapon_tag, projectile_tag, actor_tag;                        /* null on every actor of the ten maps */
} aidef_actor_def;

/* ---- actor variant (tag `actor_variant`) ---- */
typedef struct {
	const uint8_t *raw;                          /* AIDEF_VARIANT_RAW_SIZE bytes, tag layout, references flattened */
	uint32_t tag_id;
	uint32_t flags;                              /* tag +0x00 */
	uint32_t actor_tag, unit_tag, major_variant_tag, weapon_tag, equipment_tag;   /* actor / biped (0 on vehicle-seat variants) / the stronger variant / weapon (weap tag ids) */
	uint16_t movement_type;                      /* +0x4C [P]: 0 always run, 1 always crouch, 2 switch */
	float initial_crouch_chance, crouch_time[2], run_time[2];              /* fraction, seconds bounds; +0x50.. [P] */
	/* firing: distances in wu, times in seconds, angles in radians */
	float max_firing_distance, rate_of_fire /* rounds per second */, projectile_error;   /* +0x74.. */
	float first_burst_delay[2], new_target_firing_pattern_time, surprise_delay_time, surprise_fire_wildly_time;
	float death_fire_wildly_chance, death_fire_wildly_time, desired_combat_range[2];
	float custom_stand_gun_offset[3], custom_crouch_gun_offset[3];         /* zero on every variant */
	float target_tracking, target_leading, weapon_damage_modifier, damage_per_second;
	float burst_origin_radius, burst_origin_angle, burst_return_length[2], burst_return_angle;
	float burst_duration[2], burst_separation[2], burst_angular_velocity, special_damage_modifier, special_projectile_error;
	uint16_t special_fire_mode, special_fire_situation;                    /* +0x154 [P] */
	float special_fire_chance, special_fire_delay;
	/* grenades: type 0 fragmentation, 1 plasma [P]; velocity in wu per tick (x / 30 = whole wu per second) */
	uint16_t grenade_type, grenade_trajectory, grenade_stimulus, grenade_min_enemies;   /* +0x180 */
	float grenade_enemy_radius, grenade_velocity, grenade_range[2], grenade_collateral_radius, grenade_chance;
	float grenade_check_time, encounter_grenade_timeout;
	uint16_t grenade_count[2];                   /* min, max carried (+0x1D0) */
	float dont_drop_grenades_chance, drop_weapon_loaded[2];
	uint16_t drop_weapon_ammo[2];
	float body_vitality, shield_vitality;        /* hit points, absolute (not scales: the globals' difficulty block scales them) [V] */
	float shield_sapping_radius;
	uint16_t forced_shader_permutation, change_color_count;   /* the change-colors block itself is not converted */
} aidef_variant_def;

/* ---- scenario AI ---- */
typedef struct {
	char     name[AIDEF_NAME_LEN];
	uint32_t flags;                              /* AIDEF_ENC_* */
	uint16_t team;                               /* aidef_team */
	uint16_t search_behavior, w28;               /* tag +0x26 (always 1), +0x28 [U] */
	int16_t  manual_bsp, precomputed_bsp;        /* BSP indices [V]; manual_bsp counts when AIDEF_ENC_MANUAL_BSP_INDEX is set */
	uint32_t first_squad, squad_count, first_platoon, platoon_count, first_firing, firing_count, first_pstart, pstart_count;
} aidef_encounter_def;

typedef struct {
	char     name[AIDEF_NAME_LEN];
	int16_t  actor_type;                         /* index into the scenario actor palette, -1 none */
	int16_t  platoon;                            /* platoon of the encounter, -1 none */
	uint16_t initial_state, return_state;        /* aidef_actor_state */
	uint32_t flags;                              /* AIDEF_SQUAD_* */
	uint16_t unique_leader;
	int16_t  maneuver_squad;                     /* squad of the same encounter, -1 none */
	float    delay_time;                         /* seconds */
	uint32_t masks[7];                           /* attacking, attacking search, attacking guard, defending, defending search, defending guard, pursuing [U] */
	uint16_t normal_count, insane_count;         /* actors spawned on normal / legendary: see aidef_squad_spawn_count */
	uint16_t major_upgrade, w82, w84, w86;
	uint32_t first_start, start_count, first_move, move_count;
} aidef_squad_def;

typedef struct {                                 /* a spot where a squad's actor is placed */
	float   pos[3], facing;                      /* wu, radians (-pi..pi) */
	int16_t cluster;                             /* BSP cluster [V] */
	int16_t w12; uint16_t w14, w16; int16_t w18; /* [U] */
	int16_t command_list;                        /* command list index [V], -1 none */
} aidef_start_def;

typedef struct {                                 /* where a squad may maneuver to */
	float   pos[3], facing, weight, time_min, time_max;
	int16_t animation, sequence, cluster;        /* animation: -1 on every position */
	int32_t surface;                             /* pathfinding surface of the BSP, -1 none */
} aidef_move_def;

typedef struct {
	float    pos[3];
	uint16_t group;                              /* [U] */
	int16_t  cluster;                            /* BSP cluster [V] */
	int32_t  surface;                            /* pathfinding surface, -1 none */
} aidef_firing_def;

typedef struct {
	char     name[AIDEF_NAME_LEN];
	uint32_t w20; uint16_t w30; int16_t w32; uint16_t w3c; int16_t w3e;    /* [U] */
} aidef_platoon_def;

typedef struct { float pos[3], facing; uint16_t team, bsp, types[4]; } aidef_pstart_def;

typedef struct {
	char     name[AIDEF_NAME_LEN];
	uint32_t flags;                              /* bit 5 (32): manual_bsp is specified [V] */
	int16_t  manual_bsp, precomputed_bsp;        /* BSP indices [V / P] */
	uint32_t first_command, command_count, first_point, point_count;
} aidef_command_list_def;

typedef struct {
	uint16_t type;                               /* aidef_atom_type */
	uint16_t modifier;
	float    param1, param2;
	int16_t  point1, point2;                     /* points of the list */
	int16_t  animation;                          /* AI animation reference */
	int16_t  script, recording;
	int16_t  command;                            /* loop / branch target */
	int16_t  object_name;
} aidef_command_def;

typedef struct { float pos[3]; int32_t surface; } aidef_point_def;

typedef struct {
	char     name[AIDEF_NAME_LEN];
	uint32_t flags;
	float    trigger_distance, run_to_player_distance;       /* wu */
	uint32_t first_participant, participant_count, first_line, line_count;
} aidef_conversation_def;

typedef struct {
	uint16_t flags, w02, selection_type, actor_type;          /* actor_type: aidef_actor_type */
	int16_t  use_object, new_name_object;                      /* scenario object names */
	int16_t  variants[6];                                      /* dialogue variant numbers (-1 unused) */
	int32_t  encounter_index;                                  /* the encounter of encounter_name, -1 none */
	char     encounter_name[AIDEF_NAME_LEN];
} aidef_participant_def;

typedef struct {
	uint16_t flags;
	int16_t  participant;                        /* of the conversation, -1 none */
	uint16_t addressee;
	int16_t  addressee_participant;
	float    delay;                              /* seconds */
	uint32_t sounds[6];                          /* snd! tag ids of the alternative takes, 0 unused */
} aidef_line_def;

typedef struct { char name[AIDEF_NAME_LEN]; uint32_t graph_tag; } aidef_anim_ref_def;     /* antr tag id and the animation's name in it */

aidef_status aidef_open(aidef_section *s, const uint8_t *data, size_t size);

/* ---- actors and variants (sorted by tag id: binary search) ---- */
int aidef_find_actor(const aidef_section *s, uint32_t tag_id);                      /* index or -1 */
int aidef_get_actor(const aidef_section *s, uint32_t index, aidef_actor_def *out);
int aidef_actor(const aidef_section *s, uint32_t tag_id, aidef_actor_def *out);     /* "actor by tag id" */
int aidef_find_variant(const aidef_section *s, uint32_t tag_id);
int aidef_get_variant(const aidef_section *s, uint32_t index, aidef_variant_def *out);
int aidef_actor_variant(const aidef_section *s, uint32_t tag_id, aidef_variant_def *out);   /* "actor variant by tag id" */
int aidef_variant_actor(const aidef_section *s, const aidef_variant_def *v, aidef_actor_def *out);    /* the actor a variant names */
int aidef_variant_major(const aidef_section *s, const aidef_variant_def *v, aidef_variant_def *out);  /* the stronger variant, if any */
/* any word of the raw record by tag offset (4-aligned for the 32-bit reads, 2-aligned for u16); 0 beyond the record */
float    aidef_actor_f32(const aidef_actor_def *a, uint32_t tag_off);
uint32_t aidef_actor_u32(const aidef_actor_def *a, uint32_t tag_off);
uint32_t aidef_actor_u16(const aidef_actor_def *a, uint32_t tag_off);
float    aidef_variant_f32(const aidef_variant_def *v, uint32_t tag_off);
uint32_t aidef_variant_u32(const aidef_variant_def *v, uint32_t tag_off);
uint32_t aidef_variant_u16(const aidef_variant_def *v, uint32_t tag_off);

/* ---- scenario actor palette: squad.actor_type indexes it ---- */
uint32_t aidef_palette_count(const aidef_section *s);
uint32_t aidef_palette_tag(const aidef_section *s, uint32_t index);                 /* actv tag id, 0 none */
int      aidef_palette_variant(const aidef_section *s, uint32_t index, aidef_variant_def *out);   /* 1 when the palette entry has a variant */

/* ---- encounters, by index and by name (case-insensitive, as scripts compare) ---- */
uint32_t aidef_encounter_count(const aidef_section *s);
int      aidef_get_encounter(const aidef_section *s, uint32_t index, aidef_encounter_def *out);
int      aidef_find_encounter(const aidef_section *s, const char *name);            /* index or -1 */
int      aidef_get_squad(const aidef_section *s, uint32_t squad_index, aidef_squad_def *out);       /* global squad index */
uint32_t aidef_squad_count(const aidef_section *s);
int      aidef_encounter_squad(const aidef_section *s, uint32_t encounter, uint32_t k, aidef_squad_def *out);   /* k-th squad of an encounter */
int      aidef_find_squad(const aidef_section *s, uint32_t encounter, const char *name);   /* global squad index or -1 */
int      aidef_find_squad_path(const aidef_section *s, const char *path);          /* "encounter/squad": global squad index or -1 */
int      aidef_squad_variant(const aidef_section *s, const aidef_squad_def *sq, aidef_variant_def *out);   /* palette[actor_type] */
int      aidef_get_start(const aidef_section *s, const aidef_squad_def *sq, uint32_t k, aidef_start_def *out);
int      aidef_get_move(const aidef_section *s, const aidef_squad_def *sq, uint32_t k, aidef_move_def *out);
int      aidef_get_firing(const aidef_section *s, const aidef_encounter_def *e, uint32_t k, aidef_firing_def *out);
int      aidef_get_platoon(const aidef_section *s, const aidef_encounter_def *e, uint32_t k, aidef_platoon_def *out);
int      aidef_find_platoon(const aidef_section *s, const aidef_encounter_def *e, const char *name);   /* k or -1 */
int      aidef_get_pstart(const aidef_section *s, const aidef_encounter_def *e, uint32_t k, aidef_pstart_def *out);
/* Actors a squad spawns on `difficulty` (0..3, clamped): easy and normal use normal_count, impossible insane_count, hard is the
 * midpoint rounded up [U: the tag has only the two counts; this interpolation is the usual reading and is not proved]. */
uint32_t aidef_squad_spawn_count(const aidef_squad_def *sq, uint32_t difficulty);

/* ---- command lists ---- */
uint32_t aidef_command_list_count(const aidef_section *s);
int      aidef_get_command_list(const aidef_section *s, uint32_t index, aidef_command_list_def *out);
int      aidef_find_command_list(const aidef_section *s, const char *name);
int      aidef_get_command(const aidef_section *s, const aidef_command_list_def *l, uint32_t k, aidef_command_def *out);
int      aidef_get_point(const aidef_section *s, const aidef_command_list_def *l, uint32_t k, aidef_point_def *out);

/* ---- conversations ---- */
uint32_t aidef_conversation_count(const aidef_section *s);
int      aidef_get_conversation(const aidef_section *s, uint32_t index, aidef_conversation_def *out);
int      aidef_find_conversation(const aidef_section *s, const char *name);
int      aidef_get_participant(const aidef_section *s, const aidef_conversation_def *c, uint32_t k, aidef_participant_def *out);
int      aidef_get_line(const aidef_section *s, const aidef_conversation_def *c, uint32_t k, aidef_line_def *out);

/* ---- AI animation and script references ---- */
uint32_t aidef_anim_ref_count(const aidef_section *s);
int      aidef_get_anim_ref(const aidef_section *s, uint32_t index, aidef_anim_ref_def *out);
uint32_t aidef_script_ref_count(const aidef_section *s);
int      aidef_script_ref_name(const aidef_section *s, uint32_t index, char out[AIDEF_NAME_LEN]);

/* debugging names of the enums (engine vocabulary, not tag data); "?" out of range */
const char *aidef_actor_type_name(uint32_t type);
const char *aidef_state_name(uint32_t state);
const char *aidef_team_name(uint32_t team);
const char *aidef_atom_name(uint32_t atom);

#endif
