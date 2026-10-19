/* Device runtime (docs/27-dev-section.md): doors, platforms, gears, controls and light fixtures as the DEVS data
 * describes them, plus the trigger-volume helpers the script machine needs. Pure C11 (no heap, no double, no engine
 * dependencies): everything the world supplies arrives through function arguments, everything the world must react
 * to leaves through an event queue, so the whole file is host-tested (engine/tests/test_devices.c).
 *
 * Model (30 Hz, one devices_tick per game tick):
 *   - Every device has a POWER and a POSITION, both 0..1 (Q32 fixed point internally, so a transition of T ticks takes
 *     exactly T ticks), and a target for each. Power follows the power group's value (or `devices_set_power`) with the
 *     tag's power transition time; position follows its position group's value (reversed when the placement says so),
 *     a script's `devices_set_position`, or - doors - its own activator logic, with the tag's position transition time
 *     (linear; the acceleration times are carried in the def for the host's easing, not modelled here).
 *   - A powered device moves; an unpowered one is frozen, except that when power DROPS to 0 and the tag has a depowered
 *     transition time the device drifts to position 0 in that time (once); a position a SCRIPT asked for moves
 *     regardless of power.
 *   - Doors (machine subtype door, activation radius > 0, placement not "does not operate automatically") open when the
 *     host reports an activator within the radius (one-sided doors only for activators on their front side) and close
 *     `open_ticks + 1` ticks after the last tick with an activator in reach.
 *   - Controls are used by the host (`devices_use`): the control sets its position group (toggle: flips it, on: 1,
 *     off: 0, call: the tag's call value or 1); machines sharing that group follow it. A control needs power, and the
 *     user must be on the front side unless the placement is usable from both sides.
 *   - "Can change only once" (device placement flag, group flag) is honoured: the second change is refused.
 * The model is data-driven; the details the data does not settle (what power loss does, one-sided orientation, call
 * buttons, change-once timing) are listed as unverified in docs/27-dev-section.md. No collision with objects (doors do
 * not push, block or crush anything). */
#ifndef SIM_DEVICES_H
#define SIM_DEVICES_H
#include <stddef.h>
#include <stdint.h>
#include "world/devdef.h"

#define DEV_MAX_DEVICES 640            /* retail maps place at most 497 devices (a50); sizeof(devices_t) is about 70 KB */
#define DEV_MAX_GROUPS 64
#define DEV_EVENT_QUEUE 256

typedef enum {
	DEV_EV_NONE = 0,
	DEV_EV_OPENING,        /* position starts to rise (also a reversal) */
	DEV_EV_OPENED,         /* position reached 1 */
	DEV_EV_CLOSING,        /* position starts to fall */
	DEV_EV_CLOSED,         /* position reached 0 */
	DEV_EV_ACTIVATED,      /* control used (value 1 = now on, 0 = off) / door triggered by an activator */
	DEV_EV_DEPOWERED,      /* power reached 0 */
	DEV_EV_REPOWERED,      /* power left 0 */
	DEV_EV_DENIED          /* control used without power or after its one change */
} dev_event_type;

typedef struct {
	uint8_t  type;         /* dev_event_type */
	uint8_t  value;
	uint16_t device;       /* device index */
	uint32_t tick;         /* devices_t.tick when it happened */
} dev_event;

/* devices_use results */
#define DEV_USE_OK 1
#define DEV_USE_INVALID 0        /* not a control, or no such device */
#define DEV_USE_NO_POWER (-1)
#define DEV_USE_WRONG_SIDE (-2)
#define DEV_USE_LOCKED (-3)

/* dev_state.flags */
#define DEV_S_NEAR 1u        /* an activator is in reach this tick */
#define DEV_S_AUTO 2u        /* the open target was raised by the door's own logic: it closes again by itself */
#define DEV_S_FORCED 4u      /* the position target came from a script: moves even when unpowered */
#define DEV_S_CHANGED 8u     /* position or power changed in the last tick */
#define DEV_S_LOCKED 16u     /* "can change only once": the change is done */
#define DEV_S_DEPOWERING 32u /* power just dropped to 0 and the tag has a depowered time: drifting to position 0 */

typedef struct {
	uint64_t pos, target, power, power_target;       /* Q32: 0 .. 2^32 = 0.0 .. 1.0 */
	uint32_t hold;                                   /* door: ticks left before it closes */
	uint32_t pos_ticks, power_ticks, depowered_ticks, open_ticks;
	float    radius;                                 /* door activation radius */
	float    origin[3], fwd[2];                      /* device position; (cos yaw, sin yaw) of its front */
	uint32_t pflags;                                 /* DEVDEF_PF_* */
	uint32_t mflags;                                 /* DEVDEF_MPF_* (machines) */
	int16_t  power_group, position_group;
	uint16_t pos_frames;
	uint8_t  flags;                                  /* DEV_S_* */
	int8_t   dir;                                    /* position motion: -1 closing, 0 idle, +1 opening */
	uint8_t  kind, subtype;
	uint8_t  initial_open;
	uint8_t  pad[3];
} dev_state;

typedef struct {
	float pos[3];
	int   player;          /* the activator is a player (matters for one-sided-for-player doors) */
} dev_activator;

typedef struct {
	const devdef_section *sec;
	uint32_t   count, group_count;
	uint32_t   tick;
	uint32_t   truncated;                       /* devices / groups of the section beyond the caps */
	uint32_t   event_head, event_count, events_dropped;
	uint16_t   doors[DEV_MAX_DEVICES];          /* doors the logic visits: activator and melee doors */
	uint32_t   door_count;
	float      group_value[DEV_MAX_GROUPS];
	uint8_t    group_changed[DEV_MAX_GROUPS];
	dev_state  dev[DEV_MAX_DEVICES];
	dev_event  events[DEV_EVENT_QUEUE];
} devices_t;

/* Instantiate every device of `s` in its initial state (groups at their initial values, power from its group or the
 * "initially off" flag, position from its group or "initially open"). No events. The section must outlive `d`. */
void devices_init(devices_t *d, const devdef_section *s);
/* After restoring a devices_t from a checkpoint (it is plain data: memcpy is a snapshot) point it at the section again. */
void devices_rebind(devices_t *d, const devdef_section *s);

/* One tick: power, door logic, position, events. Call after devices_set_activators of the same tick. */
void devices_tick(devices_t *d);
/* The positions of everything that can open doors this tick (players, actors): replaces the NEAR flags. */
void devices_set_activators(devices_t *d, const dev_activator *a, unsigned n);
/* A control was used by someone at `user_pos` (the host decided it is within reach). DEV_USE_* result. */
int  devices_use(devices_t *d, unsigned device, const float user_pos[3]);
/* A melee blow hit a door (machine flag "opened by melee attack"): returns 1 when it triggered. */
int  devices_melee(devices_t *d, unsigned device);

/* Script interface (device_set_position, device_set_power, device_group_*). Values are clamped to 0..1. */
void  devices_set_position(devices_t *d, unsigned device, float v);            /* a transition to v */
void  devices_set_position_immediate(devices_t *d, unsigned device, float v);  /* no transition, no events */
void  devices_set_power(devices_t *d, unsigned device, float v);
void  devices_set_power_immediate(devices_t *d, unsigned device, float v);
float devices_position(const devices_t *d, unsigned device);                   /* 0 for a bad index */
float devices_power(const devices_t *d, unsigned device);
float devices_position_target(const devices_t *d, unsigned device);
int   devices_moving(const devices_t *d, unsigned device);                     /* position is not at its target */
int   devices_changed(const devices_t *d, unsigned device);                    /* moved or re-powered in the last tick */
float devices_anim_frame(const devices_t *d, unsigned device);                 /* position * (frames - 1) of the position animation */
void  devices_group_set(devices_t *d, unsigned group, float v);
float devices_group_get(const devices_t *d, unsigned group);

/* Events for the host (sound / effect playback, script triggers). */
int      devices_pop_event(devices_t *d, dev_event *out);                      /* 1 when one was returned */
/* The sound / effect tag the def attaches to an event (devdef fx slots), 0 when none. */
uint32_t devices_event_tag(const devices_t *d, const dev_event *ev);

/* ---- trigger volumes ---------------------------------------------------------------------------------------------- */
int      volume_contains(const devdef_volume *v, const float p[3]);            /* NaN is outside */
unsigned volume_count_inside(const devdef_volume *v, const float (*pts)[3], unsigned n);   /* objects of a list inside */
int      volume_any_inside(const devdef_volume *v, const float (*pts)[3], unsigned n);
int      volume_all_inside(const devdef_volume *v, const float (*pts)[3], unsigned n);     /* 0 for an empty list */
void     volume_center(const devdef_volume *v, float out[3]);

#endif
