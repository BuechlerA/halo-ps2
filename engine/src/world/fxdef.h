/* FXD0 section reader (docs/22-fx-section.md): the effects a map's gameplay data reaches, as compact tables for an
 * EE-side billboard / sprite renderer. Contract between tools/halo2ps2/fxconv.py (writer), world/fxdef.c (reader,
 * host-tested, pure C11, no heap, no double) and the engine's effect system. The bitmaps are the FXTX section of the
 * same pack, a TEXS blob read by world/tex.c.
 *
 * Units: seconds for every time (lifespans, delays, durations, fades; 30 ticks = 1 s), world units for distances and
 * sizes, radians for angles and angular rates, colours 0..1 floats unless stated (tints are RGBA8). Every record is
 * keyed by its Xbox tag id (u32); the definition tables (effects, particles, physics, decals, lights, particle systems,
 * bitmaps) are sorted by tag id and found by binary search. "Index" values are indices into the table named; 0xFFFF
 * (FXD_NONE) means none. Bounds are stored (min, max) with min <= max (the converter swapped retail data that had them
 * the other way round); a random value is lerp(min, max, u). Whatever fxdef_open accepts can be walked through the
 * accessors without leaving the buffer.
 *
 * Structure: effect -> events (skip fraction, delay, duration) -> parts (a damage effect / sound / light / decal /
 * particle system / object created at a marker) and emitters (a particle tag spawned `count` times in a distribution). */
#ifndef WORLD_FXDEF_H
#define WORLD_FXDEF_H
#include <stddef.h>
#include <stdint.h>
#include "world/tex.h"

#define FXD_NONE 0xFFFFu
#define FXD_NO_TAG 0xFFFFFFFFu
#define FXD_NAME_LEN 32
#define FXD_SHORT_NAME 16
#define FXD_TICKS_PER_SEC 30

typedef enum { FXD_OK = 0, FXD_ERR_TRUNCATED, FXD_ERR_MAGIC, FXD_ERR_LAYOUT, FXD_ERR_VERSION } fxdef_status;

/* part / reference kinds */
enum { FXD_K_NONE = 0, FXD_K_DAMAGE, FXD_K_OBJECT, FXD_K_LIGHT, FXD_K_SOUND, FXD_K_DECAL, FXD_K_PARTICLE_SYSTEM, FXD_K_EFFECT };

/* create-in environment (part, emitter) */
enum { FXD_ENV_ANY = 0, FXD_ENV_AIR, FXD_ENV_WATER, FXD_ENV_SPACE };
/* violence mode */
enum { FXD_VIOLENCE_EITHER = 0, FXD_VIOLENCE_ONLY, FXD_VIOLENCE_NONVIOLENT };
/* emitter camera mode (`create`) */
enum { FXD_CAM_ANY = 0, FXD_CAM_FIRST, FXD_CAM_THIRD, FXD_CAM_FIRST_IF_POSSIBLE };
/* emitter distribution of the spawn times over the event's duration */
enum { FXD_DIST_START = 0, FXD_DIST_END, FXD_DIST_CONSTANT, FXD_DIST_BUILDUP, FXD_DIST_FALLOFF, FXD_DIST_BUILDUP_FALLOFF };

/* effect.flags */
#define FXD_EF_DELETE_ON_DETACH 1u
#define FXD_EF_DETERMINISTIC_XBOX 2u
#define FXD_EF_DETERMINISTIC_PC 4u
#define FXD_EF_BLOOD_SETTING 8u
/* part.flags */
#define FXD_PF_FACE_DOWN 1u                 /* decals: face down regardless of the location */
/* emitter.flags */
#define FXD_EMF_ATTACHED 1u                 /* stay attached to the marker */
#define FXD_EMF_RANDOM_ANGLE 2u
#define FXD_EMF_TINT_FROM_OBJECT 4u
#define FXD_EMF_TINT_HSV 8u
#define FXD_EMF_LONG_HUE 16u
/* particle.flags */
#define FXD_PAF_ANIM_BACKWARDS 1u
#define FXD_PAF_STOPS_AT_REST 2u
#define FXD_PAF_RANDOM_START_FRAME 4u
#define FXD_PAF_ONCE_PER_FRAME 8u
#define FXD_PAF_DIES_AT_REST 0x10u
#define FXD_PAF_DIES_ON_STRUCTURE 0x20u
#define FXD_PAF_TINT_FROM_DIFFUSE 0x40u
#define FXD_PAF_DIES_IN_WATER 0x80u
#define FXD_PAF_DIES_IN_AIR 0x100u
#define FXD_PAF_SELF_ILLUMINATED 0x200u
#define FXD_PAF_MIRROR_H 0x400u
#define FXD_PAF_MIRROR_V 0x800u
/* particle.orientation */
enum { FXD_ORIENT_SCREEN = 0, FXD_ORIENT_PARALLEL, FXD_ORIENT_PERPENDICULAR };
/* framebuffer blend functions (particle, decal, particle state) */
enum { FXD_BLEND_ALPHA = 0, FXD_BLEND_MULTIPLY, FXD_BLEND_DOUBLE_MULTIPLY, FXD_BLEND_ADD, FXD_BLEND_SUBTRACT,
       FXD_BLEND_MIN, FXD_BLEND_MAX, FXD_BLEND_ALPHA_MULTIPLY_ADD };
/* particle fade mode */
enum { FXD_FADE_NONE = 0, FXD_FADE_PERPENDICULAR, FXD_FADE_PARALLEL };
/* point physics flags */
#define FXD_PP_FLAMETHROWER 1u
#define FXD_PP_COLLIDES_STRUCTURE 2u
#define FXD_PP_COLLIDES_WATER 4u
#define FXD_PP_SIMPLE_WIND 8u
#define FXD_PP_DAMPED_WIND 0x10u
#define FXD_PP_NO_GRAVITY 0x20u
/* decal.type / layer */
enum { FXD_DECAL_SCRATCH = 0, FXD_DECAL_SPLATTER, FXD_DECAL_BURN, FXD_DECAL_PAINTED_SIGN };
enum { FXD_LAYER_PRIMARY = 0, FXD_LAYER_SECONDARY, FXD_LAYER_LIGHT, FXD_LAYER_ALPHA_TESTED, FXD_LAYER_WATER };
/* decal.flags */
#define FXD_DF_NEXT_INHERITS 1u
#define FXD_DF_HSV 2u
#define FXD_DF_NO_RANDOM_ROTATION 8u
#define FXD_DF_WATER 0x10u
#define FXD_DF_ANIM_LOOP 0x80u
#define FXD_DF_PRESERVE_ASPECT 0x100u
/* light.flags: bit 0 dynamic (the rest as the tag documents them: see the doc) */
/* particle system */
enum { FXD_PCTL_SIMPLE = 0, FXD_PCTL_ROTATIONAL };
enum { FXD_PCTL_PHYS_DEFAULT = 0, FXD_PCTL_PHYS_EXPLOSION, FXD_PCTL_PHYS_JET };

enum { FXD_T_EFFECTS = 0, FXD_T_LOCATIONS, FXD_T_EVENTS, FXD_T_PARTS, FXD_T_EMITTERS, FXD_T_PARTICLES, FXD_T_PHYSICS,
       FXD_T_DECALS, FXD_T_LIGHTS, FXD_T_PCTL, FXD_T_PCTL_TYPES, FXD_T_PCTL_STATES, FXD_T_PCTL_PSTATES, FXD_T_CONSTS,
       FXD_T_BITMAPS, FXD_T_SEQUENCES, FXD_T_SPRITES, FXD_T_COUNT };

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       count[FXD_T_COUNT];
	const uint8_t *table[FXD_T_COUNT];
} fxdef_section;

typedef struct {
	uint32_t tag_id;
	uint32_t flags;                    /* FXD_EF_* */
	int16_t  loop_start, loop_stop;    /* event indices or -1 */
	float    damage_radius;            /* "maximum damage radius" of the tag (0 on most, 4..60 on explosions) */
	uint32_t first_location, first_event;
	uint16_t location_count, event_count;
} fxd_effect;

typedef struct {
	float    skip;                     /* probability that the event is skipped, 0..1 */
	float    delay[2];                 /* seconds from the effect's start (min, max) */
	float    duration[2];              /* seconds over which emitters spread their particles */
	uint32_t first_part, first_emitter;
	uint16_t part_count, emitter_count;
} fxd_event;

typedef struct {
	uint8_t  create_in, violence;      /* FXD_ENV_*, FXD_VIOLENCE_* */
	uint16_t location;                 /* marker index of the effect (< location_count) or FXD_NONE */
	uint16_t flags;                    /* FXD_PF_* */
	uint8_t  kind;                     /* FXD_K_*: what the tag is */
	uint32_t tag_id;                   /* the created tag (damage effect, sound, light, decal, particle system, object) */
	char     group[5];                 /* its group, e.g. "garb", "proj", "scen" for objects */
	uint32_t index;                    /* decal / light / particle system table index (kind), else 0xFFFFFFFF */
	float    velocity[2], cone, angvel[2], radius_mod[2];
	uint32_t a_scales, b_scales;       /* which effect-time scales apply (raw bit fields, see the doc) */
} fxd_part;

typedef struct {
	uint8_t  create_in, violence, create;     /* FXD_ENV_*, FXD_VIOLENCE_*, FXD_CAM_* */
	uint16_t location;                 /* marker index or FXD_NONE */
	uint16_t particle;                 /* index into the particle table */
	float    yaw, pitch;               /* relative direction, radians */
	float    offset[3];                /* relative offset, world units */
	uint32_t flags;                    /* FXD_EMF_* */
	uint16_t distribution;             /* FXD_DIST_* */
	int16_t  count[2];                 /* particles to spawn (min, max) */
	float    dist_radius[2];           /* spawn distribution radius */
	float    velocity[2], cone, angvel[2], radius[2];
	uint8_t  tint_lo[4], tint_hi[4];   /* RGBA8 bounds of the particle tint */
	uint32_t a_scales, b_scales;
	uint32_t particle_tag;             /* tag id of the particle (== particles[particle].tag_id) */
} fxd_emitter;

typedef struct {
	uint32_t tag_id;
	uint32_t flags;                    /* FXD_PAF_* */
	uint16_t bitmap, physics;          /* bitmap / physics table indices or FXD_NONE */
	uint32_t collision_effect, death_effect;   /* tag ids (effect, damage effect or sound per the kind) or FXD_NO_TAG */
	uint8_t  collision_kind, death_kind;       /* FXD_K_EFFECT / FXD_K_DAMAGE / FXD_K_SOUND / FXD_K_NONE */
	float    lifespan[2];              /* seconds */
	float    fade_in, fade_out;        /* seconds */
	float    min_size;                 /* pixels */
	float    radius_anim[2];           /* multiple of the emitter radius at birth and at death (lerp over the life) */
	float    anim_rate[2];             /* sprite frames per second */
	float    contact_deterioration, fade_start_size, fade_end_size;
	int16_t  seq_first, seq_initial, seq_loop, seq_final;   /* sequence selection, see the doc (clamp when walking) */
	float    sprite_size;              /* world units per sprite pixel */
	uint8_t  orientation, blend, fade_mode, shader_flags;   /* FXD_ORIENT_*, FXD_BLEND_*, FXD_FADE_*, bit 0 sort bias, 1 nonlinear tint */
} fxd_particle;

typedef struct {
	uint32_t tag_id;
	uint32_t flags;                    /* FXD_PP_* */
	float    density, air_friction, water_friction, surface_friction, elasticity;
	float    air_gravity_scale;        /* file value: NEGATIVE pulls down (-1 = full gravity), positive rises */
	float    water_gravity_scale;
} fxd_physics;

typedef struct {
	uint32_t tag_id;
	uint16_t flags;                    /* FXD_DF_* */
	uint8_t  type, layer;              /* FXD_DECAL_*, FXD_LAYER_* */
	uint32_t next_tag;                 /* next decal in chain or FXD_NO_TAG */
	uint16_t bitmap, blend;            /* bitmap table index or FXD_NONE, FXD_BLEND_* */
	float    radius[2], intensity[2];
	float    color_lo[3], color_hi[3]; /* RGB 0..1 */
	float    lifetime[2], decay[2];    /* seconds */
	int16_t  loop_frame, anim_speed;
	float    max_extent;               /* maximum sprite extent (pixels) */
} fxd_decal;

typedef struct {
	uint32_t tag_id;
	uint32_t flags;                    /* bit 0 dynamic, ... */
	float    radius, radius_mod[2];    /* world units */
	float    falloff_angle, cutoff_angle;   /* radians; ~3.1416 = omnidirectional */
	uint32_t interp_flags;
	float    color_lo[4], color_hi[4]; /* A R G B 0..1 */
	float    duration;                 /* the tag's unit (unverified: seconds) */
	uint16_t falloff_fn;
} fxd_light;

typedef struct {
	uint32_t tag_id;
	uint16_t physics, update_physics;  /* physics table index or FXD_NONE; system update physics (0 default, 1 explosion) */
	uint32_t first_type;
	uint16_t type_count;
} fxd_pctl;

typedef struct {
	char     name[FXD_SHORT_NAME];
	uint32_t flags;
	uint16_t initial_count;
	uint8_t  render_mode, creation_physics;   /* FXD_PCTL_SIMPLE/ROTATIONAL, FXD_PCTL_PHYS_* */
	float    radius;
	uint32_t first_const, first_state, first_pstate;
	uint16_t const_count, state_count, pstate_count;
} fxd_pctl_type;

typedef struct {
	char     name[FXD_SHORT_NAME];
	float    duration[2], transition[2];
	float    scale_mult, anim_mult, rot_mult;
	float    color_mult[4];            /* A R G B */
	float    radius_mult, min_count, creation_rate;
	uint16_t creation_physics, update_physics;
	uint32_t first_const;
	uint16_t const_count;
} fxd_pctl_state;

typedef struct {
	char     name[FXD_SHORT_NAME];
	float    duration[2], transition[2];
	uint16_t bitmap, sequence;         /* bitmap table index or FXD_NONE, sequence index (clamp when walking) */
	float    scale[2], anim_rate[2], rot_rate[2];
	float    color1[4], color2[4];     /* A R G B */
	float    radius_mult;
	uint16_t physics;
	uint8_t  blend, fade_mode;
	uint16_t shader_flags;
	uint32_t first_const;
	uint16_t const_count;
} fxd_pctl_pstate;

typedef struct {
	uint32_t tag_id;
	uint32_t first_sequence;
	uint16_t sequence_count;
	uint8_t  kind;                     /* bitmap tag type: 0 2d, 3 sprites */
	uint16_t texture;                  /* FXTX texture index of the first converted image, FXD_NONE if none */
	uint16_t image_count;
} fxd_bitmap;

typedef struct { uint32_t first_sprite; uint16_t sprite_count; } fxd_sequence;

typedef struct {
	uint16_t texture;                  /* FXTX texture index or FXD_NONE (image not converted) */
	uint16_t image;                    /* image index inside the bitmap tag */
	float    u0, v0, u1, v1;           /* rectangle of the image, 0..1, v0 = top row */
	float    reg[2];                   /* registration point as stored (fractions of the sprite) */
} fxd_sprite;

fxdef_status fxdef_open(fxdef_section *s, const uint8_t *data, size_t size);

/* effects */
int          fxdef_find_effect(const fxdef_section *s, uint32_t tag_id);        /* index or -1 */
fxd_effect   fxdef_get_effect(const fxdef_section *s, uint32_t index);
fxd_event    fxdef_get_event(const fxdef_section *s, const fxd_effect *e, uint32_t i);        /* i < event_count */
fxd_part     fxdef_get_part(const fxdef_section *s, const fxd_event *ev, uint32_t i);         /* i < part_count */
fxd_emitter  fxdef_get_emitter(const fxdef_section *s, const fxd_event *ev, uint32_t i);      /* i < emitter_count */
/* marker name of location i of the effect ("" = the object's origin); returns 0 when i is out of range */
int          fxdef_location_name(const fxdef_section *s, const fxd_effect *e, uint32_t i, char out[FXD_NAME_LEN]);

/* definitions: find by tag id (index or -1), get by index (zeroed record when out of range) */
int          fxdef_find_particle(const fxdef_section *s, uint32_t tag_id);
fxd_particle fxdef_get_particle(const fxdef_section *s, uint32_t index);
int          fxdef_find_physics(const fxdef_section *s, uint32_t tag_id);
fxd_physics  fxdef_get_physics(const fxdef_section *s, uint32_t index);
int          fxdef_find_decal(const fxdef_section *s, uint32_t tag_id);
fxd_decal    fxdef_get_decal(const fxdef_section *s, uint32_t index);
int          fxdef_find_light(const fxdef_section *s, uint32_t tag_id);
fxd_light    fxdef_get_light(const fxdef_section *s, uint32_t index);
int          fxdef_find_pctl(const fxdef_section *s, uint32_t tag_id);
fxd_pctl     fxdef_get_pctl(const fxdef_section *s, uint32_t index);
fxd_pctl_type   fxdef_get_pctl_type(const fxdef_section *s, const fxd_pctl *p, uint32_t i);              /* i < type_count */
fxd_pctl_state  fxdef_get_pctl_state(const fxdef_section *s, const fxd_pctl_type *t, uint32_t i);        /* i < state_count */
fxd_pctl_pstate fxdef_get_pctl_pstate(const fxdef_section *s, const fxd_pctl_type *t, uint32_t i);       /* i < pstate_count */
float        fxdef_const(const fxdef_section *s, uint32_t index);        /* physics constant pool, 0 when out of range */

/* bitmaps: sequences of sprites (a sheet's sprites, or the whole images of a 2D bitmap) */
int          fxdef_find_bitmap(const fxdef_section *s, uint32_t tag_id);
fxd_bitmap   fxdef_get_bitmap(const fxdef_section *s, uint32_t index);
fxd_sequence fxdef_get_sequence(const fxdef_section *s, const fxd_bitmap *b, uint32_t i);         /* i < sequence_count */
fxd_sprite   fxdef_get_sprite(const fxdef_section *s, const fxd_sequence *q, uint32_t i);         /* i < sprite_count */
/* Sprite `frame` (modulo the count) of sequence `sequence` (clamped to the last) of bitmap table index `bitmap`;
 * returns 0 and clears *out when the bitmap has no sprite. Total: retail particles carry sequence indices past the
 * end of their bitmap. */
int          fxdef_sprite(const fxdef_section *s, uint32_t bitmap, uint32_t sequence, uint32_t frame, fxd_sprite *out);

/* Texture of a bitmap tag in the FXTX section (a TEXS blob opened with tex_open); bindings are keyed by bitmap tag id.
 * Returns 1 and fills *out when the bitmap's first image was converted. */
int          fxdef_bitmap_binding(const tex_section *fxtx, uint32_t bitmap_tag, tex_binding *out);
/* 1 when every sprite and bitmap texture index is < texture_count (or FXD_NONE): cross-check against FXTX after both open. */
int          fxdef_check_textures(const fxdef_section *s, uint32_t texture_count);

#endif
