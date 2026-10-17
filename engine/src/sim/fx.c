#include "sim/fx.h"
#include <math.h>
#include <string.h>

#define TICKS 30.0f

static float rnd01(fx_t *f)
{
	uint32_t x = f->rng ? f->rng : 0xA341316Cu;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	f->rng = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}
static float lerp(float a, float b, float t) { return a + (b - a) * t; }

void fx_init(fx_t *f, const fxdef_section *def, float gravity_per_s2, uint32_t seed)
{
	memset(f, 0, sizeof *f);
	f->def = def; f->rng = seed ? seed : 1u; f->gravity = gravity_per_s2;
}

static void basis(const float n[3], float l[3], float u[3])
{
	float a[3] = { 0, 0, 1 }, d;
	if (fabsf(n[2]) > 0.95f) { a[0] = 1; a[1] = 0; a[2] = 0; }
	/* u = a - (a.n) n, then l = u x n */
	d = a[0] * n[0] + a[1] * n[1] + a[2] * n[2];
	u[0] = a[0] - n[0] * d; u[1] = a[1] - n[1] * d; u[2] = a[2] - n[2] * d;
	d = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	if (d < 1e-6f) { u[0] = 0; u[1] = 0; u[2] = 1; d = 1; }
	u[0] /= d; u[1] /= d; u[2] /= d;
	l[0] = u[1] * n[2] - u[2] * n[1]; l[1] = u[2] * n[0] - u[0] * n[2]; l[2] = u[0] * n[1] - u[1] * n[0];
}

static fx_particle *alloc_particle(fx_t *f)
{
	unsigned i;
	for (i = 0; i < FX_MAX_PARTICLES; i++) if (!f->p[i].active) return &f->p[i];
	f->st_dropped++;
	return NULL;
}

static unsigned colour_byte(float v)
{
	int c = (int)(v * 128.0f + 0.5f);
	return (unsigned)(c < 0 ? 0 : c > 255 ? 255 : c);
}

static unsigned spawn_emitter(fx_t *f, const fxd_emitter *em, const fxd_event *ev, const float pos[3], const float n[3], const float l[3], const float u[3])
{
	fxd_particle pd;
	fxd_physics ph;
	unsigned count, k, made = 0;
	int pi = em->particle < 0xFFFFu ? (int)em->particle : -1;
	float cy, sy, cp, sp, dir[3];
	if (pi < 0) return 0;
	pd = fxdef_get_particle(f->def, (uint32_t)pi);
	memset(&ph, 0, sizeof ph);
	if (pd.physics != 0xFFFFu) ph = fxdef_get_physics(f->def, pd.physics);
	count = (unsigned)em->count[0];
	if (em->count[1] > em->count[0]) count += (unsigned)(rnd01(f) * (float)(em->count[1] - em->count[0] + 1));
	if (count > 24) count = 24;
	cy = cosf(em->yaw); sy = sinf(em->yaw); cp = cosf(em->pitch); sp = sinf(em->pitch);
	for (k = 0; k < 3; k++) dir[k] = n[k] * cp * cy + l[k] * cp * sy + u[k] * sp;
	for (k = 0; k < count; k++) {
		fx_particle *p = alloc_particle(f);
		float speed, d[3], cone, a, r, rad, life_s, t, off[3];
		unsigned i;
		fxd_sprite spr;
		if (!p) break;
		memset(p, 0, sizeof *p);
		p->active = 1;
		/* direction inside the emitter's cone */
		cone = em->cone > 0.0f ? em->cone * sqrtf(rnd01(f)) : 0.0f;
		a = rnd01(f) * 6.2831853f; r = sinf(cone);
		{
			float bl[3], bu[3];
			basis(dir, bl, bu);
			for (i = 0; i < 3; i++) d[i] = dir[i] * cosf(cone) + (bl[i] * cosf(a) + bu[i] * sinf(a)) * r;
		}
		speed = lerp(em->velocity[0], em->velocity[1], rnd01(f));
		for (i = 0; i < 3; i++) off[i] = n[i] * em->offset[0] + l[i] * em->offset[1] + u[i] * em->offset[2];
		rad = lerp(em->dist_radius[0], em->dist_radius[1], rnd01(f));
		a = rnd01(f) * 6.2831853f;
		for (i = 0; i < 3; i++) p->pos[i] = pos[i] + off[i] + (l[i] * cosf(a) + u[i] * sinf(a)) * rad;
		for (i = 0; i < 3; i++) p->vel[i] = d[i] * speed / TICKS;
		life_s = lerp(pd.lifespan[0], pd.lifespan[1], rnd01(f));
		if (life_s <= 0.0f) life_s = 0.3f;
		p->life = (uint16_t)(life_s * TICKS + 1.0f);
		p->age = (int16_t)(-(int)(lerp(ev->delay[0], ev->delay[1], rnd01(f)) * TICKS));
		p->fade_in = (uint16_t)(pd.fade_in * TICKS); p->fade_out = (uint16_t)(pd.fade_out * TICKS);
		t = lerp(em->radius[0], em->radius[1], rnd01(f));
		if (t <= 0.0f) t = 0.05f;
		p->size0 = t * (pd.radius_anim[0] > 0.0f ? pd.radius_anim[0] : 1.0f);
		p->size1 = t * (pd.radius_anim[1] > 0.0f ? pd.radius_anim[1] : 1.0f);
		p->bitmap = pd.bitmap; p->seq = (uint16_t)(pd.seq_first > 0 ? pd.seq_first : 0);
		p->anim_rate = lerp(pd.anim_rate[0], pd.anim_rate[1], rnd01(f)) / TICKS;
		p->blend = pd.blend; p->fade_mode = pd.fade_mode;
		p->gravity = ph.air_gravity_scale < 0.0f ? -ph.air_gravity_scale * f->gravity / (TICKS * TICKS) : -ph.air_gravity_scale * f->gravity / (TICKS * TICKS);
		p->friction = ph.air_friction > 0.0f && ph.air_friction < 5.0f ? 1.0f - ph.air_friction / TICKS : 1.0f;
		if (p->friction < 0.5f) p->friction = 0.5f;
		t = rnd01(f);
		for (i = 0; i < 4; i++) {
			p->rgba[i] = (uint8_t)(em->tint_lo[i] + (em->tint_hi[i] - em->tint_lo[i]) * t);
			p->rgba_end[i] = p->rgba[i];
		}
		/* tints are 0..255 bytes on the tag's scale; the GS wants 0x80 = 1.0 */
		for (i = 0; i < 4; i++) { p->rgba[i] = (uint8_t)(p->rgba[i] / 2); p->rgba_end[i] = p->rgba[i]; }
		if (!p->rgba[3]) p->rgba[3] = 0x80;
		if (!fxdef_sprite(f->def, p->bitmap, p->seq, 0, &spr)) { p->active = 0; continue; }
		made++;
	}
	return made;
}

unsigned fx_spawn(fx_t *f, uint32_t effect_tag, const float pos[3], const float dir[3], int surface)
{
	int ei;
	fxd_effect e;
	float n[3] = { 0, 0, 1 }, l[3], u[3], len;
	unsigned k, j, made = 0;
	if (!f->def || !effect_tag) return 0;
	ei = fxdef_find_effect(f->def, effect_tag);
	if (ei < 0) return 0;
	if (dir) { len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]); if (len > 1e-6f) { n[0] = dir[0] / len; n[1] = dir[1] / len; n[2] = dir[2] / len; } }
	basis(n, l, u);
	e = fxdef_get_effect(f->def, (uint32_t)ei);
	f->st_effects++;
	for (k = 0; k < e.event_count; k++) {
		fxd_event ev = fxdef_get_event(f->def, &e, k);
		if (ev.skip > 0.0f && rnd01(f) < ev.skip) continue;
		for (j = 0; j < ev.emitter_count; j++) {
			fxd_emitter em = fxdef_get_emitter(f->def, &ev, j);
			if (em.create_in == FXD_ENV_WATER) continue;
			made += spawn_emitter(f, &em, &ev, pos, n, l, u);
		}
		for (j = 0; j < ev.part_count && surface; j++) {
			fxd_part part = fxdef_get_part(f->def, &ev, j);
			if (part.kind == FXD_K_DECAL && part.index != 0xFFFFFFFFu) {
				fxd_decal dd = fxdef_get_decal(f->def, part.index);
				fx_decal *d = &f->d[f->next_decal++ % FX_MAX_DECALS];
				unsigned i;
				memset(d, 0, sizeof *d);
				d->active = 1; d->blend = (uint8_t)dd.blend; d->bitmap = dd.bitmap;
				for (i = 0; i < 3; i++) { d->pos[i] = pos[i] + n[i] * 0.01f; d->normal[i] = n[i]; }
				d->radius = lerp(dd.radius[0], dd.radius[1], rnd01(f)); if (d->radius <= 0.0f) d->radius = 0.05f;
				d->life = (uint16_t)(lerp(dd.lifetime[0], dd.lifetime[1], rnd01(f)) * TICKS + 30.0f);
				d->fade = (uint16_t)(lerp(dd.decay[0], dd.decay[1], rnd01(f)) * TICKS + 1.0f);
				d->rgba[0] = colour_byte(dd.color_hi[0]); d->rgba[1] = colour_byte(dd.color_hi[1]); d->rgba[2] = colour_byte(dd.color_hi[2]);
				d->rgba[3] = colour_byte(lerp(dd.intensity[0], dd.intensity[1], 0.5f)); if (d->rgba[3] < 0x40) d->rgba[3] = 0x70;
				made++; f->st_decals++;
			}
		}
	}
	f->st_particles += made;
	return made;
}

unsigned fx_particle_alpha(const fx_particle *p)
{
	float a = (float)p->rgba[3];
	if (p->age < 0) return 0;
	if (p->fade_in && p->age < (int)p->fade_in) a *= (float)p->age / (float)p->fade_in;
	if (p->fade_out && p->life > p->age && (unsigned)(p->life - p->age) < p->fade_out) a *= (float)(p->life - p->age) / (float)p->fade_out;
	return (unsigned)a;
}

void fx_tick(fx_t *f)
{
	unsigned i;
	for (i = 0; i < FX_MAX_PARTICLES; i++) {
		fx_particle *p = &f->p[i];
		if (!p->active) continue;
		p->age++;
		if (p->age < 0) continue;
		if (p->age >= (int)p->life) { p->active = 0; continue; }
		p->vel[2] -= p->gravity;
		p->vel[0] *= p->friction; p->vel[1] *= p->friction; p->vel[2] *= p->friction;
		p->pos[0] += p->vel[0]; p->pos[1] += p->vel[1]; p->pos[2] += p->vel[2];
	}
	for (i = 0; i < FX_MAX_DECALS; i++) {
		fx_decal *d = &f->d[i];
		if (!d->active) continue;
		d->age++;
		if (d->age >= d->life + d->fade) d->active = 0;
	}
}

unsigned fx_active_particles(const fx_t *f) { unsigned i, n = 0; for (i = 0; i < FX_MAX_PARTICLES; i++) n += f->p[i].active; return n; }
unsigned fx_active_decals(const fx_t *f) { unsigned i, n = 0; for (i = 0; i < FX_MAX_DECALS; i++) n += f->d[i].active; return n; }
