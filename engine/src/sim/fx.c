#include "sim/fx.h"
#include <math.h>
#include <stdio.h>
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

/* ---- object attachments: particle systems and looping effects ------------------------------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int fx_set_attachments(fx_t *f, const uint8_t *data, size_t size)
{
	uint32_t n;
	f->attach = NULL; f->attach_count = 0; f->glow_tag = 0xFFFFFFFFu;
	if (!data || size < 16 || memcmp(data, "FXAT", 4) != 0 || rd32(data + 4) != 2u) return 0;
	n = rd32(data + 8);
	if ((size - 16) / 32u < n) return 0;
	f->attach = data + 16; f->attach_count = n; f->glow_tag = rd32(data + 12);
	return 1;
}

fx_attach_row fx_attachment(const fx_t *f, uint32_t row)
{
	fx_attach_row r;
	memset(&r, 0, sizeof r);
	if (row >= f->attach_count) return r;
	r.object_tag = rd32(f->attach + row * 32u);
	r.tag = rd32(f->attach + row * 32u + 4);
	r.kind = f->attach[row * 32u + 8];
	memcpy(r.marker, f->attach + row * 32u + 12, 19);
	return r;
}

unsigned fx_attachments_of(const fx_t *f, uint32_t object_tag, uint32_t *first)
{
	uint32_t lo = 0, hi = f->attach_count, n = 0;
	while (lo < hi) {                                  /* first row with object tag >= the wanted one */
		uint32_t mid = (lo + hi) / 2;
		if (rd32(f->attach + mid * 32u) < object_tag) lo = mid + 1; else hi = mid;
	}
	*first = lo;
	while (lo + n < f->attach_count && rd32(f->attach + (lo + n) * 32u) == object_tag) n++;
	return n;
}

static float range_mid(const float b[2]) { return 0.5f * (b[0] + b[1]); }

/* One particle of a particle system type. The tag's durations, multipliers and physics are not decoded (ADR-066): the particle lives the sum of the transition times
 * of its particle states but the last (the death state), grows from the first state's scale to the middle one's (x 80 world units per tag unit, measured against the
 * steam puff of the Xbox cryo bay), takes the middle state's colours and drifts along the marker with the system's jet speed, rising slowly. */
static void spawn_pctl_particle(fx_t *f, const fxd_pctl *pc, const fxd_pctl_type *ty, const fxd_pctl_state *st, const float pos[3], const float dir[3])
{
	fx_particle *p = alloc_particle(f);
	fxd_pctl_pstate first, mid;
	fxd_sprite spr;
	float life_s = 0.0f, bl[3], bu[3], d[3], cone, a, speed, alpha;
	unsigned i, n = ty->pstate_count, k;
	(void)pc;
	if (!p || n == 0) return;
	first = fxdef_get_pctl_pstate(f->def, ty, 0);
	mid = fxdef_get_pctl_pstate(f->def, ty, n > 2 ? n - 2 : 0);
	for (k = 0; k + 1 < n || k == 0; k++) {
		fxd_pctl_pstate ps = fxdef_get_pctl_pstate(f->def, ty, k);
		life_s += lerp(ps.transition[0], ps.transition[1], rnd01(f));
		if (k + 1 >= n) break;
	}
	if (life_s < 0.4f) life_s = 0.4f;
	life_s *= 1.6f;                                            /* particle lifetime = the states' transition times: the death state's fade-out adds to what is seen */
	memset(p, 0, sizeof *p);
	p->active = 1;
	p->life = (uint16_t)(life_s * TICKS + 1.0f);
	p->fade_in = 3; p->fade_out = (uint16_t)(p->life / 2u);
	p->size0 = 80.0f * range_mid(first.scale); p->size1 = 80.0f * range_mid(mid.scale);
	if (p->size0 < 0.01f) p->size0 = 0.01f;
	if (p->size1 < p->size0 * 3.5f) p->size1 = p->size0 * 3.5f;                 /* a puff expands as it cools */
	p->bitmap = mid.bitmap; p->seq = mid.sequence; p->blend = mid.blend; p->fade_mode = mid.fade_mode;
	basis(dir, bl, bu);
	{                                                          /* creation physics "jet" (XBE 0x10f150): v = (c1 x random unit vector + (1 - c1) x direction) x c0 / 30 per second, c0 = 45, c1 = 0.0355 for the steam */
		float c0 = ty->const_count > 0 ? fxdef_const(f->def, ty->first_const) : 45.0f, c1 = ty->const_count > 1 ? fxdef_const(f->def, ty->first_const + 1) : 0.0355f;
		float rv[3], rl, spd = c0 * (1.0f / 30.0f);
		for (i = 0; i < 3; i++) rv[i] = rnd01(f) * 2.0f - 1.0f;
		rl = sqrtf(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
		if (rl < 1e-3f) { rv[0] = 1.0f; rv[1] = rv[2] = 0.0f; rl = 1.0f; }
		cone = 0.0f; a = 0.0f; (void)bl; (void)bu;
		for (i = 0; i < 3; i++) { d[i] = rv[i] / rl * c1 + dir[i] * (1.0f - c1); p->pos[i] = pos[i]; p->vel[i] = d[i] * spd / TICKS; }
		speed = spd;
	}
	alpha = range_mid((float[2]){ mid.color1[0], mid.color2[0] }) * st->color_mult[0] * 3.4f;       /* the tag's alpha is a density: steam at 0.15 is a visible puff */
	if (alpha < 0.3f) alpha = 0.3f;
	if (alpha > 1.0f) alpha = 1.0f;
	p->rgba[0] = (uint8_t)colour_byte(mid.color1[1] * st->color_mult[1] * 0.5f * 2.0f);
	p->rgba[1] = (uint8_t)colour_byte(mid.color1[2] * st->color_mult[2] * 0.5f * 2.0f);
	p->rgba[2] = (uint8_t)colour_byte(mid.color1[3] * st->color_mult[3] * 0.5f * 2.0f);
	p->rgba[3] = (uint8_t)colour_byte(alpha);
	for (i = 0; i < 4; i++) p->rgba_end[i] = p->rgba[i];
	p->anim_rate = 0.0f;
	if (!fxdef_sprite(f->def, p->bitmap, p->seq, 0, &spr)) { p->active = 0; return; }
	f->st_particles++;
}

static fx_system *find_system(fx_t *f, int key)
{
	unsigned i;
	fx_system *free_s = NULL;
	for (i = 0; i < FX_MAX_SYSTEMS; i++) {
		if (f->sys[i].active && f->sys[i].key == (int16_t)key) return &f->sys[i];
		if (!f->sys[i].active && !free_s) free_s = &f->sys[i];
	}
	if (free_s) { memset(free_s, 0, sizeof *free_s); free_s->key = (int16_t)key; }
	return free_s;
}

void fx_attach_update(fx_t *f, int key, uint32_t row, const float pos[3], const float dir[3])
{
	fx_attach_row r;
	fx_system *s;
	if (!f->def || row >= f->attach_count) return;
	r = fx_attachment(f, row);
	s = find_system(f, key);
	if (!s) return;
	if (!s->active) {
		s->active = 1; s->kind = r.kind; s->tag = r.tag;
		if (r.kind == FX_ATT_PCTL) {
			int pi = fxdef_find_pctl(f->def, r.tag);
			if (pi < 0) { s->active = 0; return; }
			s->pctl = (uint16_t)pi;
		} else if (r.kind == FX_ATT_LIGH) {
			int li = fxdef_find_light(f->def, r.tag);
			if (li < 0) { s->active = 0; return; }
			s->pctl = (uint16_t)li;
		}
	}
	s->seen = 1;
	if (s->kind == FX_ATT_LIGH) {                          /* a light has no geometry: its glow is a soft additive disc at the marker (the Xbox adds a bloom pass) */
		fxd_light L = fxdef_get_light(f->def, s->pctl);
		fx_particle *p;
		int gb = f->glow_tag != 0xFFFFFFFFu ? fxdef_find_bitmap(f->def, f->glow_tag) : -1;
		float c[3], size;
		unsigned i;
		if ((L.flags & 1u) && L.radius >= 0.75f && f->dyn_next_n < FX_MAX_DYNLIGHTS) {      /* a held baton (radius 0.5) would colour its holder */      /* dynamic: it lights the objects near it (obj_render light_at); the tag's radius is in the engine's coarse units */
			fx_dynlight *dl = &f->dyn_next[f->dyn_next_n++];
			unsigned q;
			for (q = 0; q < 3; q++) { dl->pos[q] = pos[q]; dl->rgb[q] = L.color_hi[1 + q]; }
			dl->radius = (L.radius > 0.0f ? L.radius : 1.0f) * 2.5f;
			if (f->dbg == 3) printf("[fx] dyn light tag %08x key %d at %.1f %.1f %.1f rgb %.2f %.2f %.2f r %.1f\n", (unsigned)r.tag, key, (double)pos[0], (double)pos[1], (double)pos[2], (double)dl->rgb[0], (double)dl->rgb[1], (double)dl->rgb[2], (double)dl->radius);
		}
		if ((L.flags & 1u) && L.radius >= 0.75f) {           /* dynamic: no bloom; objects take it in obj_render, the walls' vertex colours in bsp_render (dl_apply) */
			return;
		}
		if (gb < 0 || !(p = alloc_particle(f))) return;     /* the others bloom */
		for (i = 0; i < 3; i++) c[i] = 0.55f * L.color_hi[1 + i] + 0.45f;           /* a bright light blooms towards white */
		size = L.radius > 0.0f ? L.radius : 1.0f;
		size = 0.05f * (size > 8.0f ? 8.0f : size) + 0.03f;       /* world units of half size: the light's radius is the reach of its light, the bloom is a small part of it */
		{                                                          /* a bloom is a screen space effect: at a distance it keeps an apparent size (about 3 % of the distance, times the light's reach) */
			float dx = pos[0] - f->cam[0], dy = pos[1] - f->cam[1], dz = pos[2] - f->cam[2], dist = sqrtf(dx * dx + dy * dy + dz * dz);
			float apparent = 0.13f * dist * (0.6f + 0.4f * (L.radius > 4.0f ? 4.0f : L.radius));
			if (dist < 12.0f) apparent = 0.0f;                  /* close up it is a lamp, not a bloom */
			if (apparent > size) size = apparent;
			if (size > 90.0f) size = 90.0f;
		}
		memset(p, 0, sizeof *p);
		p->active = 1; p->life = 2; p->age = 0;
		{                                                          /* in front of the fixture by its own size, or the glow is half inside the nozzle's depth */
			float dx = f->cam[0] - pos[0], dy = f->cam[1] - pos[1], dz = f->cam[2] - pos[2], dl = sqrtf(dx * dx + dy * dy + dz * dz), sh = size < 0.5f * dl ? size : 0.5f * dl;
			float inv = dl > 1e-4f ? sh / dl : 0.0f;
			p->pos[0] = pos[0] + dx * inv; p->pos[1] = pos[1] + dy * inv; p->pos[2] = pos[2] + dz * inv;
		}
		for (i = 0; i < 3; i++) { p->rgba[i] = (uint8_t)colour_byte(c[i] * 0.9f); p->rgba_end[i] = p->rgba[i]; }
		p->rgba[3] = p->rgba_end[3] = f->dbg == 2 ? 0x80 : 0x80;
		if (f->dbg == 2) { p->rgba[0] = p->rgba[1] = p->rgba[2] = 0x80; size = 20.0f; }
		p->size0 = p->size1 = size;
		p->bitmap = (uint16_t)gb; p->seq = 0; p->blend = 3;
		return;
	}
	if (s->kind == FX_ATT_EFFE) {
		if (s->cool > 0) s->cool--;
		else { fx_spawn(f, s->tag, pos, dir, 0); s->cool = 75; }
		return;
	}
	{
		fxd_pctl pc = fxdef_get_pctl(f->def, s->pctl);
		unsigned t;
		for (t = 0; t < pc.type_count && t < FX_SYS_TYPES; t++) {
			fxd_pctl_type ty = fxdef_get_pctl_type(f->def, &pc, t);
			fxd_pctl_state st;
			if (ty.state_count == 0) continue;
			if (!s->init) {
				unsigned k;
				st = fxdef_get_pctl_state(f->def, &ty, 0);
				s->state[t] = 0; s->timer[t] = lerp(st.duration[0], st.duration[1], rnd01(f)); s->acc[t] = 0.0f;
				for (k = 0; k < ty.initial_count && k < 16; k++) spawn_pctl_particle(f, &pc, &ty, &st, pos, dir);
			}
			st = fxdef_get_pctl_state(f->def, &ty, s->state[t]);
			s->timer[t] -= 1.0f / TICKS;
			if (s->timer[t] <= 0.0f) {                     /* the next state (ON -> OFF -> ON ...) */
				s->state[t] = (uint8_t)((s->state[t] + 1u) % ty.state_count);
				st = fxdef_get_pctl_state(f->def, &ty, s->state[t]);
				s->timer[t] = lerp(st.duration[0], st.duration[1], rnd01(f));
				if (s->timer[t] <= 0.0f) s->timer[t] = 1.0f;
			}
			s->acc[t] += st.creation_rate / TICKS;
			while (s->acc[t] >= 1.0f) { s->acc[t] -= 1.0f; spawn_pctl_particle(f, &pc, &ty, &st, pos, dir); }
		}
		s->init = 1;
	}
}

unsigned fx_particle_alpha(const fx_particle *p)
{
	float a = (float)p->rgba[3];
	if (p->age < 0) return 0;
	if (p->fade_in && p->age < (int)p->fade_in) a *= (float)p->age / (float)p->fade_in;
	if (p->fade_out && p->life > p->age && (unsigned)(p->life - p->age) < p->fade_out) a *= (float)(p->life - p->age) / (float)p->fade_out;
	return (unsigned)a;
}

unsigned fx_dynamic_lights(const fx_t *f, const fx_dynlight **out) { *out = f->dyn; return f->dyn_n; }

void fx_tick(fx_t *f)
{
	unsigned i;
	memcpy(f->dyn, f->dyn_next, f->dyn_next_n * sizeof f->dyn[0]); f->dyn_n = f->dyn_next_n; f->dyn_next_n = 0;
	for (i = 0; i < FX_MAX_SYSTEMS; i++) {                 /* an attachment that was not updated this tick is gone */
		if (!f->sys[i].active) continue;
		if (!f->sys[i].seen) f->sys[i].active = 0; else f->sys[i].seen = 0;
	}
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
