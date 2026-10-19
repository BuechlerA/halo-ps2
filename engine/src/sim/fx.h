/* Effects runtime: particles and decals created from FXD0 effects (docs/22-fx-section.md). Pure C11, no heap, host-tested; render/fx_render.c
 * draws the pools as sprites. Approximations (documented in docs/14 and ADR-032): effect events fire at creation with their delay folded
 * into the particle's start; particle systems exist only as object attachments (fx_attach_update; an approximation, ADR-066), lights and object parts are not created; billboards only; no particle collision. */
#ifndef SIM_FX_H
#define SIM_FX_H
#include <stdint.h>
#include "world/fxdef.h"

#define FX_MAX_PARTICLES 560
#define FX_MAX_DECALS 48
#define FX_MAX_SYSTEMS 128
#define FX_SYS_TYPES 4

typedef struct {
	uint8_t  active, blend, fade_mode;
	uint16_t bitmap, seq;            /* fxdef bitmap table index, sequence */
	int16_t  age;                    /* ticks; negative = waiting for the event delay */
	uint16_t life;
	uint16_t fade_in, fade_out;      /* ticks */
	float    pos[3], vel[3];
	float    size0, size1;           /* half size in world units at birth / death */
	float    anim_rate;              /* frames per tick */
	float    gravity, friction;      /* units per tick^2 (down positive), velocity multiplier per tick */
	uint8_t  rgba[4];                /* GS scale tint */
	uint8_t  rgba_end[4];
} fx_particle;

typedef struct {
	uint8_t  active, blend;
	uint16_t bitmap;
	uint16_t age, life, fade;        /* ticks */
	float    pos[3], normal[3];
	float    radius;
	uint8_t  rgba[4];
} fx_decal;

/* An object's attached particle system or looping effect (FXAT rows: object tag, attached tag, kind, marker). One instance per (object, row). */
enum { FX_ATT_PCTL = 1, FX_ATT_EFFE = 2, FX_ATT_LIGH = 3 };
typedef struct { uint32_t object_tag, tag; uint8_t kind; char marker[20]; } fx_attach_row;
typedef struct {
	uint8_t  active, seen, kind, init;
	int32_t  key;                    /* (object slot * 8 + row within the object) | serial << 16 */
	uint16_t pctl;                   /* fxdef pctl index (a light's table index for FX_ATT_LIGH) */
	uint32_t tag;                    /* effect tag (FX_ATT_EFFE) */
	uint8_t  state[FX_SYS_TYPES];
	float    timer[FX_SYS_TYPES], acc[FX_SYS_TYPES];
	int16_t  cool;
} fx_system;

#define FX_MAX_DYNLIGHTS 16
typedef struct { float pos[3], rgb[3], radius; } fx_dynlight;     /* a dynamic point light (flag bit 0 of the light tag) of an attached fixture */

typedef struct {
	const fxdef_section *def;
	fx_particle p[FX_MAX_PARTICLES];
	fx_decal    d[FX_MAX_DECALS];
	fx_system   sys[FX_MAX_SYSTEMS];
	const uint8_t *attach; uint32_t attach_count;      /* FXAT rows (32 bytes each) */
	fx_dynlight dyn[FX_MAX_DYNLIGHTS], dyn_next[FX_MAX_DYNLIGHTS];
	unsigned    dyn_n, dyn_next_n;   /* the lights registered by the attachments last tick / this tick */
	int         dbg;                 /* debug (cfg fxdbg 1): log light glows */
	float       cam[3];              /* camera position of the last frame: light glows keep a minimum apparent size */
	uint32_t    glow_tag;            /* tag id of the soft disc bitmap light attachments are drawn with (0xFFFFFFFF none) */
	uint32_t    rng, next_decal;
	unsigned    st_effects, st_particles, st_decals, st_dropped;
	float       gravity;             /* world gravity, units/s^2 */
} fx_t;

void fx_init(fx_t *f, const fxdef_section *def, float gravity_per_s2, uint32_t seed);
/* Creates effect `tag` at `pos`; `dir` is the effect's forward axis (the surface normal for impacts, the firing direction for muzzle effects),
 * `surface` = 1 allows decals. Unknown tags are ignored. Returns the number of particles + decals created. */
unsigned fx_spawn(fx_t *f, uint32_t effect_tag, const float pos[3], const float dir[3], int surface);
/* FXAT section: 'FXAT', u32 version 2, u32 count, u32 glow bitmap tag, rows sorted by object tag. Returns 0 when it is malformed (no attachments then). */
int fx_set_attachments(fx_t *f, const uint8_t *data, size_t size);
/* Attachments of an object tag: returns how many and the index of the first row. */
unsigned fx_attachments_of(const fx_t *f, uint32_t object_tag, uint32_t *first);
fx_attach_row fx_attachment(const fx_t *f, uint32_t row);
/* One tick of the attachment `row` of the object slot `key`: creates the instance on first call, emits (a particle system's particles, an effect now and then)
 * at `pos` along `dir`. An instance that is not updated for a tick ends. */
void fx_attach_update(fx_t *f, int32_t key, uint32_t row, const float pos[3], const float dir[3]);
/* The dynamic lights of the attachments (objects near them are lit by them): the set of the last tick. */
unsigned fx_dynamic_lights(const fx_t *f, const fx_dynlight **out);
/* One 30 Hz tick. */
void fx_tick(fx_t *f);
/* Current tint alpha (0..0x80) of a particle: fade in / out applied. */
unsigned fx_particle_alpha(const fx_particle *p);
unsigned fx_active_particles(const fx_t *f);
unsigned fx_active_decals(const fx_t *f);

#endif
