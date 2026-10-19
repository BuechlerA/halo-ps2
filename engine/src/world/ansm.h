/* ASTB section reader (docs/19-ansm-section.md): the state tables of the Xbox `model_animations` graphs, converted
 * by tools/halo2ps2/ansmconv.py. Named animation lookup for bipeds, vehicles, weapons, first-person arms and devices:
 *
 *     graph tag id -> (unit label, weapon class, weapon type label, action) -> animation index
 *
 * plus the per-animation info (frame count, 30 Hz ticks, derived loop flag, variant chain, sound / key / foot
 * events) and the aiming ranges. Keys are Xbox tag ids, exactly as in the ANIM section (world/anim.h): animation
 * index k of a graph here is animation k of the same graph there, so `anim_sample(s, g, k, ...)` plays what a lookup
 * returned. Pure C11, no heap, no `double`, host-tested (engine/tests/test_ansm.c).
 *
 * Units: angles in radians, time in frames/ticks of 1/30 s. Every animation index returned is either -1 (none) or
 * below the graph's animation count (ansm_open validates all of them). Strings are NUL terminated, inside the section.
 *
 * An opened section never changes; every function is a pure read and returns -1 / 0 / NULL on bad arguments. */
#ifndef WORLD_ANSM_H
#define WORLD_ANSM_H
#include <stddef.h>
#include <stdint.h>

typedef enum { ANSM_OK = 0, ANSM_ERR_TRUNCATED, ANSM_ERR_MAGIC, ANSM_ERR_VERSION, ANSM_ERR_LAYOUT } ansm_status;

#define ANSM_NAME_MAX 64            /* longest string in the section, NUL included */
#define ANSM_EVENTS_MAX 5           /* ansm_anim_events: 1 sound + 2 key frames + 2 foot frames */

/* ansm_anim.flags */
#define ANSM_A_LOOPS 1u             /* same derived rule as ANIM_LOOPS (idle, move-*, airborne); the tag has no loop flag */
#define ANSM_A_WORLD_RELATIVE 2u
/* ansm_anim.type */
#define ANSM_TYPE_BASE 0
#define ANSM_TYPE_OVERLAY 1
#define ANSM_TYPE_REPLACEMENT 2

/* ---- slots and actions ------------------------------------------------------------------------------------------------
 * Every table of the graph is a list of animation indices addressed by a fixed slot number; slot numbers and names are
 * those of the Xbox tag. An ACTION is level << 8 | slot. The X-macros give "<identifier suffix>, <slot>, <name>"; the
 * names are those found on the real maps (a few "assumed" ones are listed in the doc). */
#define ANSM_L_TYPE 0               /* (unit, class, weapon type) block: reload, chamber, fire, melee */
#define ANSM_L_CLASS 1              /* (unit, class) block: idle, turn, move, airborne, land, throw, ready, aim, ... */
#define ANSM_L_UNIT 2               /* unit block: airborne-dead, acc-*, enter, exit, look, ... */
#define ANSM_L_FP 3                 /* first person weapon block (graph of weapon "fp" tags) */
#define ANSM_L_WOBJ 4               /* weapon object block (the weapon's own graph: bolt, magazine, ...) */
#define ANSM_L_VEHICLE 5            /* vehicle block: steering, velocity, ground-speed, ... */
#define ANSM_L_DEVICE 6             /* device block: position, power */
#define ANSM_LEVELS 7

#define ANSM_ACTION(level, slot) (((unsigned)(level) << 8) | (unsigned)(slot))
#define ANSM_ACTION_LEVEL(a) ((unsigned)(a) >> 8)
#define ANSM_ACTION_SLOT(a) ((unsigned)(a) & 255u)

#define ANSM_TYPE_SLOTS(X) \
	X(RELOAD_1, 0, "reload-1") X(RELOAD_2, 1, "reload-2") X(CHAMBER_1, 2, "chamber-1") X(CHAMBER_2, 3, "chamber-2") \
	X(FIRE_1, 4, "fire-1") X(FIRE_2, 5, "fire-2") X(MELEE, 8, "melee")
#define ANSM_CLASS_SLOTS(X) \
	X(IDLE, 0, "idle") X(TURN_LEFT, 2, "turn-left") X(TURN_RIGHT, 3, "turn-right") X(DIVE_FRONT, 4, "dive-front") \
	X(DIVE_LEFT, 6, "dive-left") X(DIVE_RIGHT, 7, "dive-right") X(MOVE_FRONT, 8, "move-front") \
	X(MOVE_BACK, 9, "move-back") X(MOVE_LEFT, 10, "move-left") X(MOVE_RIGHT, 11, "move-right") \
	X(AIRBORNE, 16, "airborne") X(LAND_SOFT, 17, "land-soft") X(LAND_HARD, 18, "land-hard") \
	X(THROW_GRENADE, 20, "throw-grenade") X(READY, 23, "ready") X(AIM_STILL, 25, "aim-still") \
	X(AIM_MOVE, 26, "aim-move") X(SURPRISE_FRONT, 27, "surprise-front") X(SURPRISE_BACK, 28, "surprise-back") \
	X(BERSERK, 29, "berserk") X(EVADE_LEFT, 30, "evade-left") X(EVADE_RIGHT, 31, "evade-right") \
	X(SIGNAL_MOVE, 32, "signal-move") X(SIGNAL_ATTACK, 33, "signal-attack") X(WARN, 34, "warn") \
	X(STUNNED_FRONT, 35, "stunned-front") X(STUNNED_BACK, 36, "stunned-back") X(STUNNED_LEFT, 37, "stunned-left") \
	X(STUNNED_RIGHT, 38, "stunned-right") X(MELEE, 39, "melee") X(CELEBRATE, 40, "celebrate") X(PANIC, 41, "panic") \
	X(MELEE_AIRBORNE, 42, "melee-airborne") X(RESURRECT_FRONT, 44, "resurrect-front") \
	X(RESURRECT_BACK, 45, "resurrect-back") X(MELEE_CONTINUOUS, 46, "melee-continuous") X(FEEDING, 47, "feeding") \
	X(LEAP_START, 48, "leap-start") X(LEAP_AIRBORNE, 49, "leap-airborne") X(LEAP_MELEE, 50, "leap-melee")
#define ANSM_UNIT_SLOTS(X) \
	X(AIRBORNE_DEAD, 0, "airborne-dead") X(LANDING_DEAD, 1, "landing-dead") X(ACC_FRONT_BACK, 2, "acc-front-back") \
	X(ACC_LEFT_RIGHT, 3, "acc-left-right") X(ACC_UP_DOWN, 4, "acc-up-down") X(PUSH, 5, "push") X(ENTER, 7, "enter") \
	X(EXIT, 8, "exit") X(LOOK, 9, "look") X(TALK, 10, "talk") X(EMOTIONS, 11, "emotions") X(OPENING, 27, "opening") \
	X(CLOSING, 28, "closing") X(HOVERING, 29, "hovering")
#define ANSM_FP_SLOTS(X) \
	X(IDLE, 0, "idle") X(POSING, 1, "posing") X(FIRE_1, 2, "fire-1") X(MOVING, 3, "moving") X(OVERLAYS, 4, "overlays") \
	X(LIGHT_OFF, 5, "light-off") X(LIGHT_ON, 6, "light-on") X(RELOAD_EMPTY, 7, "reload-empty") \
	X(RELOAD_FULL, 8, "reload-full") X(OVERHEATED, 9, "overheated") X(READY, 10, "ready") X(PUT_AWAY, 11, "put-away") \
	X(OVERCHARGED, 12, "overcharged") X(MELEE, 13, "melee") X(FIRE_2, 14, "fire-2") \
	X(OVERCHARGED_JITTER, 15, "overcharged-jitter") X(THROW_GRENADE, 16, "throw-grenade") \
	X(AMMUNITION, 17, "ammunition") X(MISFIRE_1, 18, "misfire-1") X(MISFIRE_2, 19, "misfire-2") \
	X(THROW_OVERHEATED, 20, "throw-overheated") X(OVERHEATING, 21, "overheating") \
	X(OVERHEATING_AGAIN, 22, "overheating-again") X(ENTER, 23, "enter") X(EXIT_EMPTY, 24, "exit-empty") \
	X(EXIT_FULL, 25, "exit-full") X(OH_EXIT, 26, "o-h-exit") X(OH_S_ENTER, 27, "o-h-s-enter")
#define ANSM_WOBJ_SLOTS(X) X(RELOAD_2, 4, "reload-2") X(CHAMBER_1, 5, "chamber-1") X(FIRE_1, 9, "fire-1")
#define ANSM_VEHICLE_SLOTS(X) \
	X(STEERING, 0, "steering") X(ROLL, 1, "roll") X(THROTTLE, 2, "throttle") X(VELOCITY, 3, "velocity") \
	X(BRAKING, 4, "braking") X(GROUND_SPEED, 5, "ground-speed") X(OCCUPIED, 6, "occupied") X(UNOCCUPIED, 7, "unoccupied")
#define ANSM_DEVICE_SLOTS(X) X(POSITION, 0, "position") X(POWER, 1, "power")

/* ANSM_T_FIRE_1, ANSM_C_MOVE_FRONT, ANSM_U_ENTER, ANSM_FP_RELOAD_FULL, ANSM_WO_FIRE_1, ANSM_V_STEERING, ANSM_D_POSITION, ... */
#define ANSM_X_ENUM_T(n, s, str) ANSM_T_##n = ANSM_ACTION(ANSM_L_TYPE, s),
#define ANSM_X_ENUM_C(n, s, str) ANSM_C_##n = ANSM_ACTION(ANSM_L_CLASS, s),
#define ANSM_X_ENUM_U(n, s, str) ANSM_U_##n = ANSM_ACTION(ANSM_L_UNIT, s),
#define ANSM_X_ENUM_FP(n, s, str) ANSM_FP_##n = ANSM_ACTION(ANSM_L_FP, s),
#define ANSM_X_ENUM_WO(n, s, str) ANSM_WO_##n = ANSM_ACTION(ANSM_L_WOBJ, s),
#define ANSM_X_ENUM_V(n, s, str) ANSM_V_##n = ANSM_ACTION(ANSM_L_VEHICLE, s),
#define ANSM_X_ENUM_D(n, s, str) ANSM_D_##n = ANSM_ACTION(ANSM_L_DEVICE, s),
enum {
	ANSM_TYPE_SLOTS(ANSM_X_ENUM_T)
	ANSM_CLASS_SLOTS(ANSM_X_ENUM_C)
	ANSM_UNIT_SLOTS(ANSM_X_ENUM_U)
	ANSM_FP_SLOTS(ANSM_X_ENUM_FP)
	ANSM_WOBJ_SLOTS(ANSM_X_ENUM_WO)
	ANSM_VEHICLE_SLOTS(ANSM_X_ENUM_V)
	ANSM_DEVICE_SLOTS(ANSM_X_ENUM_D)
};

/* Slot tables (debugging, config files): name of a slot ("" when it has none, NULL for an invalid level or slot), slot
 * number by name, number of
 * slots a level can have (type 9, class 51, unit 30, fp 28, wobj 10, vehicle 8, device 2). */
const char *ansm_slot_name(unsigned level, unsigned slot);
int         ansm_slot_find(unsigned level, const char *name);          /* -1 unknown */
unsigned    ansm_slot_count(unsigned level);

/* ---- section and graphs ----------------------------------------------------------------------------------------------- */
typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       graph_count, anim_total;
	uint32_t       body;                  /* first byte after the graph table */
} ansm_section;

typedef struct {
	uint32_t tag_id;                       /* Xbox tag id of the `model_animations` tag (key of anim_find_graph too) */
	uint32_t anim_count, unit_count, object_count, vehicle_count, device_count;
	uint32_t wobj_count, fp_count, damage_count, sound_count;
	uint32_t o_anims, o_nidx, o_objects, o_units, o_vehicles, o_devices, o_wobjs, o_fps, o_damage, o_sounds;   /* internal */
} ansm_graph;

/* Validates the whole section once (header, every table, string, index and slot); a section it accepts can be walked
 * with every function below. */
ansm_status ansm_open(ansm_section *s, const uint8_t *data, size_t size);
int         ansm_get_graph(const ansm_section *s, uint32_t index, ansm_graph *out);   /* 0 ok, -1 range */
int         ansm_find_graph(const ansm_section *s, uint32_t tag_id, ansm_graph *out); /* binary search; 0 ok, -1 absent */

/* ---- animations ---------------------------------------------------------------------------------------------------------- */
typedef struct {
	const char *name;                      /* "stand rifle idle", "alert unarmed idle%1" ('%n' = variant n) */
	uint16_t    frame_count, ticks;        /* frames at `fps`; ticks = duration in 1/30 s ticks (== frames at 30 Hz) */
	uint16_t    flags, loop_frame;         /* ANSM_A_*; loop_frame is the tag's value, only meaningful with LOOPS */
	uint8_t     type, fps;                 /* ANSM_TYPE_*; 30 (25 for the PAL flag, never set on the campaign maps) */
	int16_t     next;                      /* next VARIANT of the chain (idle%1 -> idle%2), -1 at the end; not a loop link */
	int16_t     main;                      /* first variant of the chain (itself without variants) */
	int16_t     key_frame[2];              /* 0 = unset (melee: the blow lands, throw-grenade: the grenade leaves) */
	int16_t     sound, sound_frame;        /* index into the graph's sound references (-1 none), frame it plays at */
	uint32_t    sound_tag;                 /* Xbox tag id of that `sound` tag, 0 when none */
	uint8_t     foot_left, foot_right;     /* footstep frames, 0 = unset; may exceed frame_count (raw tag values) */
	float       weight;                    /* cumulative probability of this variant within its chain (0.33, 0.67, 1.0) */
} ansm_anim;

int ansm_anim_info(const ansm_section *s, const ansm_graph *g, uint32_t k, ansm_anim *out);   /* 0 ok, -1 range */
/* Exact name (lowest index), else a name without '%n' finds the head of its variant chain. -1 if absent. */
int ansm_find_anim(const ansm_section *s, const ansm_graph *g, const char *name);
/* Variant of k's chain for r in [0, 1): walks from the head by cumulative weight. */
int ansm_pick_variant(const ansm_section *s, const ansm_graph *g, uint32_t k, float r);

#define ANSM_EV_SOUND 1                    /* arg = sound tag id */
#define ANSM_EV_KEY 2                      /* arg = 0 / 1: key frame / second key frame */
#define ANSM_EV_FOOT_LEFT 3
#define ANSM_EV_FOOT_RIGHT 4
typedef struct { uint16_t frame; uint8_t kind; uint32_t arg; } ansm_event;
/* The events of animation k sorted by frame (then kind); returns the count (<= ANSM_EVENTS_MAX). The tag has no effect or
 * marker events: weapon effects come from the weapon's own tag. */
unsigned ansm_anim_events(const ansm_section *s, const ansm_graph *g, uint32_t k, ansm_event out[ANSM_EVENTS_MAX]);
uint32_t ansm_sound_tag(const ansm_section *s, const ansm_graph *g, uint32_t index);        /* 0 if out of range */

/* ---- aiming ------------------------------------------------------------------------------------------------------------------ */
typedef struct {
	float    yaw_right_per_frame, yaw_left_per_frame;
	uint16_t yaw_right_frames, yaw_left_frames;
	float    pitch_down_per_frame, pitch_up_per_frame;
	uint16_t pitch_down_frames, pitch_up_frames;
} ansm_aim;
/* out = { yaw right, yaw left, pitch down, pitch up } in radians (per frame x frames): how far the aim-still / aim-move
 * overlay can turn; the overlay has (right + left + 1) x (up + down + 1) frames on 90 % of the campaign blocks. */
void ansm_aim_ranges(const ansm_aim *a, float out[4]);

/* ---- units, classes, types ------------------------------------------------------------------------------------------- */
typedef struct { const char *name; ansm_aim aim; uint32_t class_count, slot_count, ik_count; } ansm_unit;
typedef struct { const char *name, *grip, *hold; ansm_aim aim; uint32_t type_count, slot_count, ik_count; } ansm_class;
typedef struct { const char *label; uint32_t slot_count; } ansm_type;
typedef struct { const char *marker, *attach; } ansm_ik;

int ansm_unit_info(const ansm_section *s, const ansm_graph *g, uint32_t u, ansm_unit *out);                       /* 0/-1 */
int ansm_find_unit(const ansm_section *s, const ansm_graph *g, const char *label);                                 /* index/-1 */
int ansm_unit_slot(const ansm_section *s, const ansm_graph *g, uint32_t u, unsigned slot);                         /* anim/-1 */
int ansm_unit_ik(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t i, ansm_ik *out);

int ansm_class_info(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, ansm_class *out);
int ansm_find_class(const ansm_section *s, const ansm_graph *g, uint32_t u, const char *name);
int ansm_class_slot(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, unsigned slot);
int ansm_class_ik(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t i, ansm_ik *out);
/* The tag of a weapon names its type label ("ar", "pp", "hp": 32 bytes at +0x30C) but not its class ("rifle", "pistol"),
 * so the class block is found by label: the one of unit u whose type block `label` holds the most animations. -1 if none. */
int ansm_find_class_for_type(const ansm_section *s, const ansm_graph *g, uint32_t u, const char *label);

int ansm_type_info(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t t, ansm_type *out);
int ansm_find_type(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, const char *label);
int ansm_type_slot(const ansm_section *s, const ansm_graph *g, uint32_t u, uint32_t c, uint32_t t, unsigned slot);

/* One-call lookup: (unit label, class name, type label, action) -> animation index or -1. Strings a level does not need
 * may be NULL (unit for FP/WOBJ/VEHICLE/DEVICE actions, which use block 0 of the graph; class and type for UNIT actions;
 * type for CLASS actions). Example: ansm_lookup(s, g, "stand", "rifle", "ar", ANSM_T_RELOAD_1). */
int ansm_lookup(const ansm_section *s, const ansm_graph *g, const char *unit, const char *cls, const char *type,
                unsigned action);

/* ---- first person, weapon objects, vehicles, devices, object overlays, damage ------------------------------------------ */
int ansm_fp_slot(const ansm_section *s, const ansm_graph *g, uint32_t block, unsigned slot);
int ansm_wobj_slot(const ansm_section *s, const ansm_graph *g, uint32_t block, unsigned slot);
int ansm_device_slot(const ansm_section *s, const ansm_graph *g, uint32_t block, unsigned slot);

typedef struct {
	float    steer_right_per_frame, steer_left_per_frame;     /* steering overlay: radians per frame x frames = range */
	uint16_t steer_right_frames, steer_left_frames;
	uint32_t slot_count, suspension_count;
} ansm_vehicle;
typedef struct { uint16_t mass_point; int16_t anim; float full_extension, full_compression; } ansm_suspension;
int ansm_vehicle_info(const ansm_section *s, const ansm_graph *g, uint32_t v, ansm_vehicle *out);
int ansm_vehicle_slot(const ansm_section *s, const ansm_graph *g, uint32_t v, unsigned slot);
int ansm_vehicle_suspension(const ansm_section *s, const ansm_graph *g, uint32_t v, uint32_t i, ansm_suspension *out);

/* Overlay animations driven by an object function (A..D output), e.g. a monitor's talking loop or the warthog's firing. */
typedef struct { int16_t anim; uint16_t function, controls; } ansm_object_overlay;
int ansm_object_overlay_get(const ansm_section *s, const ansm_graph *g, uint32_t i, ansm_object_overlay *out);

/* Unit damage animations: category 0..3 = shield ping, health ping, shield kill, health kill; direction 0..3 = front, left,
 * right, back; region 0..regions-1 = gut, chest, head, l-arm, l-hand, l-leg, l-foot, r-arm, r-hand, r-leg, r-foot (11).
 * Many entries reuse a neighbouring animation (a front hit for every direction). -1 if none or out of range. */
int ansm_damage_anim(const ansm_section *s, const ansm_graph *g, unsigned category, unsigned direction, unsigned region);
unsigned ansm_damage_regions(const ansm_graph *g);

#endif
