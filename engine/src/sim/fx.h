/* Effects runtime: particles and decals created from FXD0 effects (docs/22-fx-section.md). Pure C11, no heap, host-tested; render/fx_render.c
 * draws the pools as sprites. Approximations (documented in docs/14 and ADR-032): effect events fire at creation with their delay folded
 * into the particle's start; particle systems, lights and object parts are not created; billboards only; no particle collision. */
#ifndef SIM_FX_H
#define SIM_FX_H
#include <stdint.h>
#include "world/fxdef.h"

#define FX_MAX_PARTICLES 320
#define FX_MAX_DECALS 48

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

typedef struct {
	const fxdef_section *def;
	fx_particle p[FX_MAX_PARTICLES];
	fx_decal    d[FX_MAX_DECALS];
	uint32_t    rng, next_decal;
	unsigned    st_effects, st_particles, st_decals, st_dropped;
	float       gravity;             /* world gravity, units/s^2 */
} fx_t;

void fx_init(fx_t *f, const fxdef_section *def, float gravity_per_s2, uint32_t seed);
/* Creates effect `tag` at `pos`; `dir` is the effect's forward axis (the surface normal for impacts, the firing direction for muzzle effects),
 * `surface` = 1 allows decals. Unknown tags are ignored. Returns the number of particles + decals created. */
unsigned fx_spawn(fx_t *f, uint32_t effect_tag, const float pos[3], const float dir[3], int surface);
/* One 30 Hz tick. */
void fx_tick(fx_t *f);
/* Current tint alpha (0..0x80) of a particle: fade in / out applied. */
unsigned fx_particle_alpha(const fx_particle *p);
unsigned fx_active_particles(const fx_t *f);
unsigned fx_active_decals(const fx_t *f);

#endif
