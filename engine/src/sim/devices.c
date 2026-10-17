/* Device runtime (docs/27-dev-section.md, header sim/devices.h). Pure C11, no heap, no double, no engine dependencies.
 * Position and power are Q32 fixed point (0 .. 2^32), so a transition of T ticks takes exactly T ticks: the step is
 * ceil(2^32 / T) and the last step is clamped. */
#include "sim/devices.h"
#include <math.h>
#include <string.h>

#define Q_ONE (1ull << 32)
#define MAX_TICKS 65535u                  /* (T - 1) * ceil(2^32 / T) < 2^32 holds up to T = 65536: exact */

static uint64_t q_from_f(float v)
{
	if (!(v > 0.0f)) return 0;            /* also NaN */
	if (v >= 1.0f) return Q_ONE;
	return (uint64_t)(v * 4294967296.0f);
}

static float f_from_q(uint64_t q) { return (float)q / 4294967296.0f; }

static uint64_t step_of(uint32_t ticks)
{
	if (ticks == 0) return Q_ONE;         /* instant */
	if (ticks > MAX_TICKS) ticks = MAX_TICKS;
	return (Q_ONE + ticks - 1u) / ticks;
}

/* moves *v towards target by at most `step`; returns the direction moved (+1 / -1 / 0) */
static int move_q(uint64_t *v, uint64_t target, uint64_t step)
{
	if (*v < target) {
		*v = target - *v <= step ? target : *v + step;
		return 1;
	}
	if (*v > target) {
		*v = *v - target <= step ? target : *v - step;
		return -1;
	}
	return 0;
}

static void push_event(devices_t *d, uint8_t type, uint8_t value, unsigned device)
{
	dev_event *e;
	if (d->event_count >= DEV_EVENT_QUEUE) {
		d->events_dropped++;
		return;
	}
	e = &d->events[(d->event_head + d->event_count) % DEV_EVENT_QUEUE];
	e->type = type;
	e->value = value;
	e->device = (uint16_t)device;
	e->tick = d->tick;
	d->event_count++;
}

int devices_pop_event(devices_t *d, dev_event *out)
{
	if (d->event_count == 0) return 0;
	*out = d->events[d->event_head];
	d->event_head = (d->event_head + 1u) % DEV_EVENT_QUEUE;
	d->event_count--;
	return 1;
}

static int16_t usable_group(const devices_t *d, int16_t g) { return (g >= 0 && (uint32_t)g < d->group_count) ? g : (int16_t)-1; }

void devices_init(devices_t *d, const devdef_section *s)
{
	uint32_t i;
	memset(d, 0, sizeof *d);
	d->sec = s;
	d->count = s->device_count < DEV_MAX_DEVICES ? s->device_count : DEV_MAX_DEVICES;
	d->group_count = s->group_count < DEV_MAX_GROUPS ? s->group_count : DEV_MAX_GROUPS;
	d->truncated = (s->device_count - d->count) + (s->group_count - d->group_count);
	for (i = 0; i < d->group_count; i++) d->group_value[i] = devdef_get_group(s, i).initial;
	for (i = 0; i < d->count; i++) {
		devdef_device dv = devdef_get_device(s, i);
		devdef_def df = devdef_get_def(s, dv.def);
		dev_state *st = &d->dev[i];
		uint64_t pos, power;
		st->kind = df.kind;
		st->subtype = df.subtype;
		st->pflags = dv.device_flags;
		st->mflags = df.kind == DEVDEF_MACHINE ? dv.kind_flags : 0;
		st->power_group = usable_group(d, dv.power_group);
		st->position_group = usable_group(d, dv.position_group);
		st->pos_ticks = df.position_ticks > MAX_TICKS ? MAX_TICKS : df.position_ticks;
		st->power_ticks = df.power_ticks > MAX_TICKS ? MAX_TICKS : df.power_ticks;
		st->depowered_ticks = df.depowered_ticks > MAX_TICKS ? MAX_TICKS : df.depowered_ticks;
		st->open_ticks = df.open_ticks;
		st->radius = df.auto_radius;
		st->pos_frames = df.pos_frames;
		st->origin[0] = dv.pos[0];
		st->origin[1] = dv.pos[1];
		st->origin[2] = dv.pos[2];
		st->fwd[0] = cosf(dv.rot[0]);
		st->fwd[1] = sinf(dv.rot[0]);
		pos = (dv.device_flags & DEVDEF_PF_INITIALLY_OPEN) ? Q_ONE : 0;
		if (st->position_group >= 0) {
			float v = d->group_value[st->position_group];
			pos = q_from_f((dv.device_flags & DEVDEF_PF_REVERSED) ? 1.0f - v : v);
		}
		power = (dv.device_flags & DEVDEF_PF_INITIALLY_OFF) ? 0 : Q_ONE;
		if (st->power_group >= 0) power = q_from_f(d->group_value[st->power_group]);
		st->pos = st->target = pos;
		st->power = st->power_target = power;
		st->initial_open = pos == Q_ONE;
		/* doors the logic must visit: those that open for activators and those a melee blow opens (which close again) */
		if (st->kind == DEVDEF_MACHINE && st->subtype == DEVDEF_MACH_DOOR &&
		    ((st->radius > 0.0f && !(st->mflags & DEVDEF_MPF_NO_AUTO)) || (st->mflags & DEVDEF_MPF_MELEE)) &&
		    d->door_count < DEV_MAX_DEVICES)
			d->doors[d->door_count++] = (uint16_t)i;
	}
}

void devices_rebind(devices_t *d, const devdef_section *s) { d->sec = s; }

/* ---- targets -------------------------------------------------------------------------------------------------- */
static void set_target(dev_state *s, uint64_t q, int forced)
{
	if (s->flags & DEV_S_LOCKED) return;
	s->target = q;
	s->flags = (uint8_t)(s->flags & ~(DEV_S_AUTO | DEV_S_FORCED));
	if (forced) s->flags = (uint8_t)((s->flags | DEV_S_FORCED) & ~DEV_S_DEPOWERING);
}

static void follow_group(dev_state *s, float v)
{
	set_target(s, q_from_f((s->pflags & DEVDEF_PF_REVERSED) ? 1.0f - v : v), 0);
}

/* returns 0 when the group refused the change ("can change only once") */
static int group_set(devices_t *d, unsigned g, float v)
{
	uint32_t i;
	if (g >= d->group_count) return 0;
	if (!(v > 0.0f)) v = 0.0f;
	if (v > 1.0f) v = 1.0f;
	if (v == d->group_value[g]) return 1;
	if ((devdef_get_group(d->sec, g).flags & DEVDEF_GF_CHANGE_ONCE) && d->group_changed[g]) return 0;
	d->group_changed[g] = 1;
	d->group_value[g] = v;
	for (i = 0; i < d->count; i++) {
		dev_state *s = &d->dev[i];
		if (s->position_group == (int16_t)g) follow_group(s, v);
		if (s->power_group == (int16_t)g) s->power_target = q_from_f(v);
	}
	return 1;
}

void devices_group_set(devices_t *d, unsigned group, float v) { (void)group_set(d, group, v); }
float devices_group_get(const devices_t *d, unsigned group) { return group < d->group_count ? d->group_value[group] : 0.0f; }

void devices_set_position(devices_t *d, unsigned device, float v)
{
	if (device < d->count) set_target(&d->dev[device], q_from_f(v), 1);
}

void devices_set_position_immediate(devices_t *d, unsigned device, float v)
{
	dev_state *s;
	if (device >= d->count) return;
	s = &d->dev[device];
	s->pos = s->target = q_from_f(v);
	s->dir = 0;
	s->flags = (uint8_t)(s->flags & ~(DEV_S_AUTO | DEV_S_FORCED));
	s->flags |= DEV_S_CHANGED;
}

void devices_set_power(devices_t *d, unsigned device, float v)
{
	if (device < d->count) d->dev[device].power_target = q_from_f(v);
}

void devices_set_power_immediate(devices_t *d, unsigned device, float v)
{
	if (device < d->count) {
		dev_state *s = &d->dev[device];
		uint64_t before = s->power;
		s->power = s->power_target = q_from_f(v);
		s->flags |= DEV_S_CHANGED;
		if (before > 0 && s->power == 0 && s->depowered_ticks) s->flags |= DEV_S_DEPOWERING;
		if (s->power > 0) s->flags = (uint8_t)(s->flags & ~DEV_S_DEPOWERING);
	}
}

float devices_position(const devices_t *d, unsigned device) { return device < d->count ? f_from_q(d->dev[device].pos) : 0.0f; }
float devices_power(const devices_t *d, unsigned device) { return device < d->count ? f_from_q(d->dev[device].power) : 0.0f; }
float devices_position_target(const devices_t *d, unsigned device) { return device < d->count ? f_from_q(d->dev[device].target) : 0.0f; }
int devices_moving(const devices_t *d, unsigned device) { return device < d->count && d->dev[device].dir != 0; }
int devices_changed(const devices_t *d, unsigned device) { return device < d->count && (d->dev[device].flags & DEV_S_CHANGED) != 0; }

float devices_anim_frame(const devices_t *d, unsigned device)
{
	if (device >= d->count || d->dev[device].pos_frames < 2) return 0.0f;
	return f_from_q(d->dev[device].pos) * (float)(d->dev[device].pos_frames - 1u);
}

/* ---- activation ------------------------------------------------------------------------------------------------ */
void devices_set_activators(devices_t *d, const dev_activator *a, unsigned n)
{
	uint32_t k;
	unsigned j;
	for (k = 0; k < d->door_count; k++) {
		dev_state *s = &d->dev[d->doors[k]];
		float r2 = s->radius * s->radius;
		s->flags = (uint8_t)(s->flags & ~DEV_S_NEAR);
		if ((s->mflags & DEVDEF_MPF_NO_AUTO) || !(s->radius > 0.0f)) continue;      /* melee-only door */
		for (j = 0; j < n; j++) {
			float dx = a[j].pos[0] - s->origin[0], dy = a[j].pos[1] - s->origin[1], dz = a[j].pos[2] - s->origin[2];
			int one_sided = (s->mflags & DEVDEF_MPF_ONE_SIDED) || ((s->mflags & DEVDEF_MPF_ONE_SIDED_PLAYER) && a[j].player);
			if (!(dx * dx + dy * dy + dz * dz <= r2)) continue;
			if (one_sided && !(dx * s->fwd[0] + dy * s->fwd[1] >= 0.0f)) continue;
			s->flags |= DEV_S_NEAR;
			break;
		}
	}
}

int devices_use(devices_t *d, unsigned device, const float user_pos[3])
{
	dev_state *s;
	devdef_device dv;
	devdef_def df;
	float value, cur;
	int g;

	if (device >= d->count || d->dev[device].kind != DEVDEF_CONTROL) return DEV_USE_INVALID;
	s = &d->dev[device];
	if (s->power == 0) {
		push_event(d, DEV_EV_DENIED, 0, device);
		return DEV_USE_NO_POWER;
	}
	if (!(s->pflags & DEVDEF_PF_BOTH_SIDES)) {
		float side = (user_pos[0] - s->origin[0]) * s->fwd[0] + (user_pos[1] - s->origin[1]) * s->fwd[1];
		if (!(side >= 0.0f)) return DEV_USE_WRONG_SIDE;
	}
	if (s->flags & DEV_S_LOCKED) {
		push_event(d, DEV_EV_DENIED, 1, device);
		return DEV_USE_LOCKED;
	}
	dv = devdef_get_device(d->sec, device);
	df = devdef_get_def(d->sec, dv.def);
	g = s->position_group;
	cur = g >= 0 ? d->group_value[g] : (s->pos >= Q_ONE / 2 ? 1.0f : 0.0f);
	switch (df.subtype) {
	case DEVDEF_CTRL_ON: value = 1.0f; break;
	case DEVDEF_CTRL_OFF: value = 0.0f; break;
	case DEVDEF_CTRL_CALL: value = df.call_value > 0.0f ? (df.call_value > 1.0f ? 1.0f : df.call_value) : 1.0f; break;
	default: value = cur >= 0.5f ? 0.0f : 1.0f; break;
	}
	if (g >= 0) {
		if (!group_set(d, (unsigned)g, value)) {
			push_event(d, DEV_EV_DENIED, 1, device);
			return DEV_USE_LOCKED;
		}
	} else {
		set_target(s, q_from_f(value), 0);
	}
	push_event(d, DEV_EV_ACTIVATED, value >= 0.5f, device);
	return DEV_USE_OK;
}

int devices_melee(devices_t *d, unsigned device)
{
	dev_state *s;
	if (device >= d->count) return 0;
	s = &d->dev[device];
	if (s->kind != DEVDEF_MACHINE || !(s->mflags & DEVDEF_MPF_MELEE) || s->power == 0 || (s->flags & DEV_S_LOCKED)) return 0;
	if (s->target != Q_ONE) push_event(d, DEV_EV_ACTIVATED, 1, device);
	set_target(s, Q_ONE, 0);
	s->flags |= DEV_S_AUTO;
	s->hold = s->open_ticks;
	return 1;
}

/* ---- the tick ---------------------------------------------------------------------------------------------------- */
static void tick_power(devices_t *d, unsigned i)
{
	dev_state *s = &d->dev[i];
	uint64_t prev = s->power;
	if (move_q(&s->power, s->power_target, step_of(s->power_ticks))) {
		s->flags |= DEV_S_CHANGED;
		if (prev > 0 && s->power == 0) {
			push_event(d, DEV_EV_DEPOWERED, 0, i);
			if (s->depowered_ticks) s->flags |= DEV_S_DEPOWERING;
		} else if (prev == 0 && s->power > 0) {
			push_event(d, DEV_EV_REPOWERED, 0, i);
		}
		if (s->power > 0) s->flags = (uint8_t)(s->flags & ~DEV_S_DEPOWERING);
	}
}

static void tick_door(devices_t *d, unsigned i)
{
	dev_state *s = &d->dev[i];
	if (s->power > 0) {
		if (s->flags & DEV_S_NEAR) {
			if (s->target != Q_ONE && !(s->flags & DEV_S_LOCKED)) push_event(d, DEV_EV_ACTIVATED, 1, i);
			if (!(s->flags & DEV_S_LOCKED)) {
				set_target(s, Q_ONE, 0);
				s->flags |= DEV_S_AUTO;
				s->hold = s->open_ticks;
			}
		} else if (s->flags & DEV_S_AUTO) {
			if (s->hold > 0) {
				s->hold--;
			} else if (!(s->mflags & DEVDEF_MPF_NO_AUTO_CLOSE)) {
				uint64_t back = 0;
				if (s->position_group >= 0) {
					float v = d->group_value[s->position_group];
					back = q_from_f((s->pflags & DEVDEF_PF_REVERSED) ? 1.0f - v : v);
				}
				set_target(s, back, 0);
			}
		}
	}
	s->flags = (uint8_t)(s->flags & ~DEV_S_NEAR);
}

static void tick_position(devices_t *d, unsigned i)
{
	dev_state *s = &d->dev[i];
	uint64_t eff = s->target;
	uint32_t ticks = s->pos_ticks;
	int newdir;

	if (s->power == 0 && !(s->flags & DEV_S_FORCED)) {
		if ((s->flags & DEV_S_DEPOWERING) && s->depowered_ticks) {
			eff = 0;                       /* power just dropped: the device drifts to position 0 ... */
			ticks = s->depowered_ticks;
		} else {
			eff = s->pos;                  /* ... and an unpowered device otherwise stays where it is */
		}
	}
	newdir = eff > s->pos ? 1 : eff < s->pos ? -1 : 0;
	if (newdir != 0 && newdir != s->dir) push_event(d, newdir > 0 ? DEV_EV_OPENING : DEV_EV_CLOSING, 0, i);
	if (newdir != 0) {
		move_q(&s->pos, eff, step_of(ticks));
		s->flags |= DEV_S_CHANGED;
		if (s->pos == eff) {               /* arrived */
			if (s->pos == Q_ONE) push_event(d, DEV_EV_OPENED, 0, i);
			else if (s->pos == 0) push_event(d, DEV_EV_CLOSED, 0, i);
			if (s->pos == s->target) s->flags = (uint8_t)(s->flags & ~DEV_S_FORCED);
			s->flags = (uint8_t)(s->flags & ~DEV_S_DEPOWERING);
			if (s->pflags & DEVDEF_PF_CHANGE_ONCE) s->flags |= DEV_S_LOCKED;
			newdir = 0;
		}
	}
	s->dir = (int8_t)newdir;
}

void devices_tick(devices_t *d)
{
	uint32_t i;
	for (i = 0; i < d->count; i++) d->dev[i].flags = (uint8_t)(d->dev[i].flags & ~DEV_S_CHANGED);
	for (i = 0; i < d->count; i++) tick_power(d, i);
	for (i = 0; i < d->door_count; i++) tick_door(d, d->doors[i]);
	for (i = 0; i < d->count; i++) tick_position(d, i);
	d->tick++;
}

uint32_t devices_event_tag(const devices_t *d, const dev_event *ev)
{
	devdef_def df;
	devdef_device dv;
	if (ev->device >= d->sec->device_count) return 0;
	dv = devdef_get_device(d->sec, ev->device);
	df = devdef_get_def(d->sec, dv.def);
	switch (ev->type) {
	case DEV_EV_OPENING: return df.fx[DEVDEF_FX_OPEN];
	case DEV_EV_CLOSING: return df.fx[DEVDEF_FX_CLOSE];
	case DEV_EV_OPENED: return df.fx[DEVDEF_FX_OPENED];
	case DEV_EV_CLOSED: return df.fx[DEVDEF_FX_CLOSED];
	case DEV_EV_DEPOWERED: return df.fx[DEVDEF_FX_DEPOWERED];
	case DEV_EV_REPOWERED: return df.fx[DEVDEF_FX_REPOWERED];
	case DEV_EV_ACTIVATED: return df.kind == DEVDEF_CONTROL ? df.fx[ev->value ? DEVDEF_FX_CTL_ON : DEVDEF_FX_CTL_OFF] : 0;
	case DEV_EV_DENIED: return df.fx[DEVDEF_FX_CTL_DENY];
	default: return 0;
	}
}

/* ---- trigger volumes -------------------------------------------------------------------------------------------- */
int volume_contains(const devdef_volume *v, const float p[3])
{
	float dx = p[0] - v->position[0], dy = p[1] - v->position[1], dz = p[2] - v->position[2];
	float s0 = dx * v->forward[0] + dy * v->forward[1] + dz * v->forward[2];
	float s1 = dx * v->left[0] + dy * v->left[1] + dz * v->left[2];
	float s2 = dx * v->up[0] + dy * v->up[1] + dz * v->up[2];
	return (s0 >= 0.0f && s0 <= v->extents[0]) && (s1 >= 0.0f && s1 <= v->extents[1]) && (s2 >= 0.0f && s2 <= v->extents[2]);
}

unsigned volume_count_inside(const devdef_volume *v, const float (*pts)[3], unsigned n)
{
	unsigned i, c = 0;
	for (i = 0; i < n; i++)
		if (volume_contains(v, pts[i])) c++;
	return c;
}

int volume_any_inside(const devdef_volume *v, const float (*pts)[3], unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++)
		if (volume_contains(v, pts[i])) return 1;
	return 0;
}

int volume_all_inside(const devdef_volume *v, const float (*pts)[3], unsigned n)
{
	unsigned i;
	if (n == 0) return 0;
	for (i = 0; i < n; i++)
		if (!volume_contains(v, pts[i])) return 0;
	return 1;
}

void volume_center(const devdef_volume *v, float out[3])
{
	unsigned k;
	for (k = 0; k < 3; k++)
		out[k] = v->position[k] + 0.5f * (v->extents[0] * v->forward[k] + v->extents[1] * v->left[k] + v->extents[2] * v->up[k]);
}
