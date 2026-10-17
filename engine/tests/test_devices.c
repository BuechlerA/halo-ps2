/* Host test of the device runtime (sim/devices.c): the synthetic DEVS section of tools/tests/test_devconv.py (hand-computed
 * expectations for doors, platforms, gears, controls, groups, change-once, power loss, events, snapshots, volumes) and,
 * for every real pack given (assets/hp2/<map>.dev.hp2), the transitions of EVERY real device against the tag's own times:
 * a full position change takes exactly position_ticks ticks, a power change exactly power_ticks, a depowered device drifts
 * home in depowered_ticks, a door held open by an activator closes open_ticks + 1 ticks after the last tick it was in reach.
 * Volumes are tested against an independent point-in-box solver on random points. Built with ASan/UBSan. */
#include "sim/devices.h"
#include "stream/hp2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define TAGID(i) (((0xE174u + (i)) << 16) | (i))

static int load_file(const char *path, uint8_t **img, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	*img = malloc(*n);
	if (!*img || fread(*img, 1, *n, f) != *n) return 1;
	fclose(f);
	return 0;
}

static int load_section(const char *path, uint8_t **raw, size_t *rawn, uint8_t **img)
{
	hp2_pack pk;
	const hp2_section *sec;
	size_t n;
	if (load_file(path, img, &n)) return 1;
	if (hp2_open(&pk, *img, n) != HP2_OK) return 1;
	sec = hp2_find(&pk, "DEVS");
	if (!sec) return 1;
	*raw = malloc(sec->raw_size);
	if (!*raw || hp2_load(&pk, sec, *raw, sec->raw_size) != HP2_OK) return 1;
	*rawn = sec->raw_size;
	return 0;
}

/* ---- event log of one device ---------------------------------------------------------------------------------- */
typedef struct { int32_t first[10]; int count[10]; uint32_t t0; } evlog;

static void log_start(evlog *l, const devices_t *d)
{
	int k;
	for (k = 0; k < 10; k++) { l->first[k] = -1; l->count[k] = 0; }
	l->t0 = d->tick;
}

/* run n ticks, record the events of `dev` (relative to log_start), drop the rest */
static void run(devices_t *d, unsigned n, unsigned dev, evlog *l)
{
	dev_event ev;
	while (n--) {
		devices_tick(d);
		while (devices_pop_event(d, &ev)) {
			if (ev.device != dev || !l) continue;
			if (l->first[ev.type] < 0) l->first[ev.type] = (int32_t)(ev.tick - l->t0);
			l->count[ev.type]++;
		}
	}
}

/* ---- the synthetic world -------------------------------------------------------------------------------------- */
static int synthetic(const char *path)
{
	uint8_t *raw, *img;
	size_t rawn;
	devdef_section s;
	static devices_t d, snap;
	evlog l;
	dev_event ev;
	dev_activator act;
	unsigned t;
	float ctl_front[3] = { 3.0f, 100.5f, 0.0f }, ctl_back[3] = { 1.0f, 99.5f, 0.0f };

	CHECK(!load_section(path, &raw, &rawn, &img));
	CHECK(devdef_open(&s, raw, rawn) == DEVDEF_OK);

	/* initial state */
	devices_init(&d, &s);
	CHECK(d.count == 9 && d.group_count == 3 && d.truncated == 0 && d.event_count == 0);
	CHECK(d.group_value[0] == 0.0f && d.group_value[1] == 1.0f && d.group_value[2] == 0.5f);
	CHECK(devices_position(&d, 0) == 0.0f && devices_power(&d, 0) == 1.0f && !devices_moving(&d, 0));
	CHECK(devices_power(&d, 3) == 0.5f && devices_position(&d, 3) == 0.0f);        /* group 2 = 0.5; group 1 = 1.0 reversed */
	CHECK(devices_power(&d, 4) == 0.0f && devices_position(&d, 2) == 0.0f);        /* power group 0 = 0 */
	CHECK(devices_power(&d, 5) == 0.5f && devices_position(&d, 6) == 1.0f && devices_position(&d, 7) == 0.5f);
	CHECK(devices_power(&d, 8) == 1.0f && devices_position(&d, 99) == 0.0f && devices_power(&d, 99) == 0.0f);
	CHECK(d.door_count == 3 && d.doors[0] == 0 && d.doors[1] == 1 && d.doors[2] == 4);   /* two activator doors, one melee door */

	/* a group drives every device bound to it: the platform opens in exactly its 60 ticks */
	log_start(&l, &d);
	devices_group_set(&d, 0, 1.0f);
	CHECK(devices_position_target(&d, 2) == 1.0f && devices_position_target(&d, 5) == 1.0f);
	run(&d, 70, 2, &l);
	CHECK(l.first[DEV_EV_OPENING] == 0 && l.first[DEV_EV_OPENED] == 59 && l.count[DEV_EV_OPENED] == 1 && l.count[DEV_EV_OPENING] == 1);
	CHECK(devices_position(&d, 2) == 1.0f && !devices_moving(&d, 2) && devices_position(&d, 5) == 1.0f);
	CHECK(devices_power(&d, 4) == 1.0f);                                             /* its power group is group 0 as well: 15 ticks */
	devices_group_set(&d, 0, 0.0f);
	log_start(&l, &d);
	run(&d, 70, 2, &l);
	CHECK(l.first[DEV_EV_CLOSING] == 0 && l.first[DEV_EV_CLOSED] == 59 && devices_position(&d, 2) == 0.0f);
	/* a half-way reversal: 20 ticks up (1/3), then down again: 20 ticks back, one OPENING and one CLOSING */
	devices_group_set(&d, 0, 1.0f);
	run(&d, 20, 2, NULL);
	CHECK(fabsf(devices_position(&d, 2) - 20.0f / 60.0f) < 1e-6f && devices_moving(&d, 2));
	log_start(&l, &d);
	devices_group_set(&d, 0, 0.0f);
	run(&d, 25, 2, &l);
	CHECK(l.first[DEV_EV_CLOSING] == 0 && l.first[DEV_EV_CLOSED] == 19 && l.count[DEV_EV_OPENED] == 0 && devices_position(&d, 2) == 0.0f);

	/* an activator opens a door: 30 ticks, it stays while somebody is in reach, closes open_ticks + 1 ticks after the last */
	devices_init(&d, &s);
	act.player = 1;
	act.pos[0] = -5.5f; act.pos[1] = 100.0f; act.pos[2] = 0.0f;
	log_start(&l, &d);
	for (t = 0; t < 33; t++) {
		devices_set_activators(&d, &act, 1);
		devices_tick(&d);
		while (devices_pop_event(&d, &ev))
			if (ev.device == 0) {
				if (l.first[ev.type] < 0) l.first[ev.type] = (int32_t)(ev.tick - l.t0);
				l.count[ev.type]++;
				if (ev.type == DEV_EV_ACTIVATED) CHECK(ev.value == 1 && devices_event_tag(&d, &ev) == 0);
				if (ev.type == DEV_EV_OPENING) CHECK(devices_event_tag(&d, &ev) == TAGID(11));
				if (ev.type == DEV_EV_OPENED) CHECK(devices_event_tag(&d, &ev) == TAGID(14));
				if (ev.type == DEV_EV_CLOSED) CHECK(devices_event_tag(&d, &ev) == 0);
			}
	}
	CHECK(l.first[DEV_EV_ACTIVATED] == 0 && l.first[DEV_EV_OPENING] == 0 && l.first[DEV_EV_OPENED] == 29 && devices_position(&d, 0) == 1.0f);
	CHECK(l.count[DEV_EV_ACTIVATED] == 1 && l.count[DEV_EV_CLOSING] == 0);
	run(&d, 200, 0, &l);
	CHECK(l.first[DEV_EV_CLOSING] == 32 + 90 + 1 && l.first[DEV_EV_CLOSED] == 32 + 90 + 1 + 29 && devices_position(&d, 0) == 0.0f);
	CHECK(l.count[DEV_EV_CLOSING] == 1 && l.count[DEV_EV_OPENED] == 1);
	/* reach is a sphere of the activation radius (2.0) around the door origin */
	devices_init(&d, &s);
	act.pos[0] = -5.0f + 1.99f; act.pos[1] = 100.0f; act.pos[2] = 0.0f;
	devices_set_activators(&d, &act, 1);
	CHECK(d.dev[0].flags & DEV_S_NEAR);
	act.pos[0] = -5.0f + 2.01f;
	devices_set_activators(&d, &act, 1);
	CHECK(!(d.dev[0].flags & DEV_S_NEAR));
	act.pos[0] = -5.0f; act.pos[2] = 2.01f;
	devices_set_activators(&d, &act, 1);
	CHECK(!(d.dev[0].flags & DEV_S_NEAR));
	{	/* a list: any one activator in reach is enough, none is none */
		dev_activator two[2] = { { { 50.0f, 0.0f, 0.0f }, 0 }, { { -5.0f, 100.5f, 0.0f }, 0 } };
		devices_set_activators(&d, two, 2);
		CHECK(d.dev[0].flags & DEV_S_NEAR);
		devices_set_activators(&d, two, 1);
		CHECK(!(d.dev[0].flags & DEV_S_NEAR));
		devices_set_activators(&d, NULL, 0);
		CHECK(!(d.dev[0].flags & DEV_S_NEAR));
	}
	/* a one-sided door (front = yaw direction, here -x) ignores activators behind it */
	devices_init(&d, &s);
	act.pos[0] = 6.5f; act.pos[1] = 101.0f; act.pos[2] = 0.0f;                         /* behind */
	for (t = 0; t < 10; t++) { devices_set_activators(&d, &act, 1); devices_tick(&d); }
	CHECK(devices_position(&d, 1) == 0.0f && devices_position_target(&d, 1) == 0.0f);
	act.pos[0] = 3.5f;                                                                  /* in front */
	devices_set_activators(&d, &act, 1);
	devices_tick(&d);
	CHECK(devices_position_target(&d, 1) == 1.0f && devices_moving(&d, 1));
	/* a door without power does not react; with power it does */
	devices_init(&d, &s);
	devices_set_power_immediate(&d, 0, 0.0f);
	act.pos[0] = -5.5f; act.pos[1] = 100.0f;
	for (t = 0; t < 10; t++) { devices_set_activators(&d, &act, 1); devices_tick(&d); }
	CHECK(devices_position(&d, 0) == 0.0f);
	devices_set_power_immediate(&d, 0, 1.0f);
	devices_set_activators(&d, &act, 1);
	devices_tick(&d);
	CHECK(devices_moving(&d, 0));

	/* power loss: a platform with a depowered time drifts home in exactly that time; a gear without one stays put */
	devices_init(&d, &s);
	devices_set_position_immediate(&d, 2, 1.0f);
	devices_set_power_immediate(&d, 2, 0.0f);
	devices_group_set(&d, 0, 0.0f);                                                     /* no change: group 0 is 0 */
	log_start(&l, &d);
	run(&d, 40, 2, &l);
	CHECK(l.first[DEV_EV_CLOSING] == 0 && l.first[DEV_EV_CLOSED] == 29 && devices_position(&d, 2) == 0.0f);
	devices_group_set(&d, 1, 0.0f);                                                     /* gear's position group (reversed): target 1 */
	CHECK(devices_position_target(&d, 3) == 1.0f);
	devices_set_power_immediate(&d, 3, 0.0f);
	run(&d, 20, 3, NULL);
	CHECK(devices_position(&d, 3) == 0.0f && !devices_moving(&d, 3));                   /* unpowered: frozen */
	devices_set_power_immediate(&d, 3, 1.0f);
	log_start(&l, &d);
	run(&d, 20, 3, &l);
	CHECK(l.first[DEV_EV_OPENED] == 14 && devices_position(&d, 3) == 1.0f);            /* 15 ticks once powered */
	/* a script's position moves a device even without power */
	devices_init(&d, &s);
	devices_set_power_immediate(&d, 3, 0.0f);
	devices_set_position(&d, 3, 1.0f);
	log_start(&l, &d);
	run(&d, 20, 3, &l);
	CHECK(l.first[DEV_EV_OPENED] == 14 && devices_position(&d, 3) == 1.0f);
	/* power transitions: the lamp takes 8 ticks both ways, DEPOWERED when it reaches 0, REPOWERED when it leaves 0 */
	devices_init(&d, &s);
	devices_set_power(&d, 8, 0.0f);
	log_start(&l, &d);
	run(&d, 12, 8, &l);
	CHECK(l.first[DEV_EV_DEPOWERED] == 7 && devices_power(&d, 8) == 0.0f);
	devices_set_power(&d, 8, 1.0f);
	log_start(&l, &d);
	run(&d, 1, 8, &l);
	CHECK(l.first[DEV_EV_REPOWERED] == 0 && fabsf(devices_power(&d, 8) - 0.125f) < 1e-6f);
	run(&d, 10, 8, &l);
	CHECK(devices_power(&d, 8) == 1.0f && l.count[DEV_EV_REPOWERED] == 1);
	CHECK(devices_changed(&d, 0) == 0 && devices_changed(&d, 99) == 0);

	/* controls */
	devices_init(&d, &s);
	CHECK(devices_use(&d, 5, ctl_back) == DEV_USE_OK);                                 /* both sides: from behind is fine */
	CHECK(devices_pop_event(&d, &ev) && ev.type == DEV_EV_ACTIVATED && ev.device == 5 && ev.value == 1);
	CHECK(devices_event_tag(&d, &ev) == TAGID(14) && d.group_value[0] == 1.0f && devices_position_target(&d, 2) == 1.0f);
	CHECK(devices_use(&d, 5, ctl_back) == DEV_USE_OK);                                 /* toggled back */
	CHECK(devices_pop_event(&d, &ev) && ev.value == 0 && devices_event_tag(&d, &ev) == 0 && d.group_value[0] == 0.0f);
	CHECK(devices_use(&d, 6, ctl_back) == DEV_USE_WRONG_SIDE && d.event_count == 0);   /* a one-sided control, used from behind */
	CHECK(devices_use(&d, 6, ctl_front) == DEV_USE_OK);                                /* the "on" button: group 1 is already 1 */
	CHECK(devices_pop_event(&d, &ev) && ev.type == DEV_EV_ACTIVATED && ev.value == 1);
	CHECK(devices_use(&d, 0, ctl_front) == DEV_USE_INVALID && devices_use(&d, 99, ctl_front) == DEV_USE_INVALID);
	/* group flagged "can change only once": the first change goes through, the second is refused */
	devices_group_set(&d, 1, 0.0f);
	CHECK(d.group_value[1] == 0.0f && devices_position_target(&d, 3) == 1.0f && devices_position_target(&d, 6) == 0.0f);
	devices_group_set(&d, 1, 1.0f);
	CHECK(d.group_value[1] == 0.0f);
	while (devices_pop_event(&d, &ev)) {}
	CHECK(devices_use(&d, 6, ctl_front) == DEV_USE_LOCKED);                            /* "on" would change group 1 again */
	CHECK(devices_pop_event(&d, &ev) && ev.type == DEV_EV_DENIED && ev.device == 6 && devices_event_tag(&d, &ev) == TAGID(15));
	/* no power: denied with the deny effect */
	devices_set_power_immediate(&d, 5, 0.0f);
	CHECK(devices_use(&d, 5, ctl_front) == DEV_USE_NO_POWER);
	CHECK(devices_pop_event(&d, &ev) && ev.type == DEV_EV_DENIED && devices_event_tag(&d, &ev) == TAGID(15));
	/* a call button writes its call value (0.75) into its group: the gear and the toggle (power group 2) follow in 8 ticks */
	devices_init(&d, &s);
	CHECK(devices_use(&d, 7, ctl_front) == DEV_USE_OK && d.group_value[2] == 0.75f);
	run(&d, 4, 99, NULL);
	CHECK(fabsf(devices_power(&d, 3) - (0.5f + 4.0f / 30.0f)) < 1e-5f);
	run(&d, 4, 99, NULL);
	CHECK(fabsf(devices_power(&d, 3) - 0.75f) < 1e-6f && fabsf(devices_position(&d, 7) - 0.75f) < 1e-6f);
	/* a control cannot be used by a user at NaN (not on the front side) */
	{
		float nan3[3] = { NAN, 0.0f, 0.0f };
		CHECK(devices_use(&d, 6, nan3) == DEV_USE_WRONG_SIDE);
	}

	/* melee door: no power, no reaction; powered, it opens, and "can change only once" locks it open */
	devices_init(&d, &s);
	CHECK(devices_melee(&d, 4) == 0 && devices_melee(&d, 0) == 0 && devices_melee(&d, 99) == 0);
	devices_group_set(&d, 0, 1.0f);
	run(&d, 2, 4, NULL);
	log_start(&l, &d);
	CHECK(devices_melee(&d, 4) == 1);
	run(&d, 400, 4, &l);
	CHECK(l.first[DEV_EV_ACTIVATED] == 0 && l.first[DEV_EV_OPENED] == 29 && devices_position(&d, 4) == 1.0f);
	CHECK(l.count[DEV_EV_CLOSING] == 0 && (d.dev[4].flags & DEV_S_LOCKED));
	devices_set_position(&d, 4, 0.0f);
	run(&d, 40, 4, NULL);
	CHECK(devices_position(&d, 4) == 1.0f);                                            /* locked */

	/* instant set, animation frame, changed flag */
	devices_init(&d, &s);
	devices_set_position_immediate(&d, 0, 0.5f);
	CHECK(devices_position(&d, 0) == 0.5f && fabsf(devices_anim_frame(&d, 0) - 4.5f) < 1e-6f && devices_changed(&d, 0));
	devices_tick(&d);
	CHECK(!devices_changed(&d, 0) && devices_anim_frame(&d, 99) == 0.0f && devices_anim_frame(&d, 2) == 0.0f);
	devices_set_position(&d, 0, 2.5f);                                                  /* clamped */
	CHECK(devices_position_target(&d, 0) == 1.0f);
	devices_set_position(&d, 0, NAN);
	CHECK(devices_position_target(&d, 0) == 0.0f);
	devices_group_set(&d, 99, 1.0f);                                                    /* out of range: ignored */
	devices_set_power(&d, 99, 1.0f);
	devices_set_position(&d, 99, 1.0f);
	devices_set_position_immediate(&d, 99, 1.0f);
	devices_set_power_immediate(&d, 99, 1.0f);
	CHECK(devices_group_get(&d, 99) == 0.0f && !devices_moving(&d, 99));

	/* event queue: overflow is counted, order is kept, the queue drains */
	devices_init(&d, &s);
	for (t = 0; t < 140; t++) {
		devices_group_set(&d, 0, (t & 1u) ? 0.0f : 1.0f);                              /* the instant control flips every tick: 2 events */
		devices_tick(&d);
	}
	CHECK(d.event_count == DEV_EVENT_QUEUE && d.events_dropped > 0);
	{
		uint32_t last = 0, n = 0;
		while (devices_pop_event(&d, &ev)) {
			CHECK(ev.tick >= last);
			last = ev.tick;
			n++;
		}
		CHECK(n == DEV_EVENT_QUEUE && !devices_pop_event(&d, &ev));
	}

	/* a snapshot is plain data: memcpy it, run on, restore, run again: the same state */
	devices_init(&d, &s);
	devices_group_set(&d, 0, 1.0f);
	act.pos[0] = -5.5f; act.pos[1] = 100.0f; act.pos[2] = 0.0f;
	run(&d, 17, 99, NULL);
	memcpy(&snap, &d, sizeof d);
	for (t = 0; t < 50; t++) { devices_set_activators(&d, &act, 1); devices_tick(&d); }
	{
		static devices_t again;
		uint64_t pos2 = d.dev[2].pos, pos0 = d.dev[0].pos;
		memcpy(&again, &snap, sizeof snap);
		devices_rebind(&again, &s);
		for (t = 0; t < 50; t++) { devices_set_activators(&again, &act, 1); devices_tick(&again); }
		CHECK(again.dev[2].pos == pos2 && again.dev[0].pos == pos0 && again.tick == d.tick && again.event_count == d.event_count);
		CHECK(memcmp(again.dev, d.dev, sizeof d.dev[0] * d.count) == 0);
	}

	/* trigger volumes of the synthetic section */
	{
		devdef_volume v0 = devdef_get_volume(&s, 0), v1 = devdef_get_volume(&s, 1), v3 = devdef_get_volume(&s, 3);
		float in0[3] = { 0.0f, 100.0f, 2.0f }, out0[3] = { -6.0f, 100.0f, 2.0f }, corner[3] = { -5.0f, 95.0f, 0.0f }, far[3] = { 5.0f, 105.0f, 5.0f };
		float in1[3] = { 19.0f, 97.0f, 1.0f }, out1[3] = { 21.0f, 97.0f, 1.0f }, nanp[3] = { NAN, 100.0f, 2.0f }, ctr[3];
		CHECK(volume_contains(&v0, in0) && !volume_contains(&v0, out0) && volume_contains(&v0, corner) && volume_contains(&v0, far));
		CHECK(volume_contains(&v1, in1) && !volume_contains(&v1, out1) && !volume_contains(&v0, nanp));
		CHECK(volume_contains(&v3, (const float[3]){ 0, 0, 0 }) && !volume_contains(&v3, in0));   /* the invalid volume is an empty box at the origin */
		{
			const float pts[4][3] = { { 0, 100, 2 }, { -6, 100, 2 }, { -5, 95, 0 }, { 50, 50, 50 } };
			CHECK(volume_count_inside(&v0, pts, 4) == 2 && volume_any_inside(&v0, pts, 4) && !volume_all_inside(&v0, pts, 4));
			CHECK(volume_all_inside(&v0, pts, 1) && !volume_all_inside(&v0, pts, 0) && !volume_any_inside(&v0, pts, 0));
			CHECK(volume_count_inside(&v0, pts + 3, 1) == 0 && volume_count_inside(&v0, pts, 0) == 0);
		}
		volume_center(&v0, ctr);
		CHECK(fabsf(ctr[0] - 0.0f) < 1e-5f && fabsf(ctr[1] - 100.0f) < 1e-5f && fabsf(ctr[2] - 2.5f) < 1e-5f);
		volume_center(&v1, ctr);
		CHECK(fabsf(ctr[0] - 19.0f) < 1e-5f && fabsf(ctr[1] - 97.0f) < 1e-5f && fabsf(ctr[2] - 1.5f) < 1e-5f && volume_contains(&v1, ctr));
	}
	free(raw);
	free(img);
	return 0;
}

/* ---- every real device against its tag --------------------------------------------------------------------------- */
static uint32_t cap_ticks(uint32_t t) { return t > 65535u ? 65535u : t; }
static uint32_t at_least_one(uint32_t t) { return t ? t : 1u; }

static int real_pack(const char *path)
{
	uint8_t *raw, *img;
	size_t rawn;
	devdef_section s;
	static devices_t base, d;
	unsigned i, g, doors = 0, tested = 0;
	evlog l;
	dev_event ev;
	unsigned long volume_ok = 0;

	CHECK(!load_section(path, &raw, &rawn, &img));
	CHECK(devdef_open(&s, raw, rawn) == DEVDEF_OK);
	devices_init(&base, &s);
	CHECK(base.count == s.device_count && base.truncated == 0 && base.group_count == s.group_count);
	for (i = 0; i < base.count; i++) {
		devdef_device dv = devdef_get_device(&s, i);
		devdef_def df = devdef_get_def(&s, dv.def);
		uint32_t T = at_least_one(cap_ticks(df.position_ticks)), Tp = at_least_one(cap_ticks(df.power_ticks));
		int once = (dv.device_flags & DEVDEF_PF_CHANGE_ONCE) != 0;

		/* position: a scripted full change takes exactly position_ticks ticks, both ways */
		memcpy(&d, &base, sizeof d);
		devices_set_position_immediate(&d, i, 0.0f);
		devices_set_position(&d, i, 1.0f);
		log_start(&l, &d);
		run(&d, T + 3, i, &l);
		CHECK(l.first[DEV_EV_OPENING] == 0 && l.first[DEV_EV_OPENED] == (int32_t)T - 1 && l.count[DEV_EV_OPENED] == 1);
		CHECK(devices_position(&d, i) == 1.0f && !devices_moving(&d, i));
		devices_set_position(&d, i, 0.0f);
		log_start(&l, &d);
		run(&d, T + 3, i, &l);
		if (once) {
			CHECK(devices_position(&d, i) == 1.0f && l.count[DEV_EV_CLOSING] == 0);        /* the one change is used up */
		} else {
			CHECK(l.first[DEV_EV_CLOSING] == 0 && l.first[DEV_EV_CLOSED] == (int32_t)T - 1 && devices_position(&d, i) == 0.0f);
		}
		/* power: a full change takes exactly power_ticks; DEPOWERED at the end, REPOWERED when it leaves 0 */
		memcpy(&d, &base, sizeof d);
		devices_set_power_immediate(&d, i, 1.0f);
		devices_set_power(&d, i, 0.0f);
		log_start(&l, &d);
		run(&d, Tp + 2, i, &l);
		CHECK(l.first[DEV_EV_DEPOWERED] == (int32_t)Tp - 1 && devices_power(&d, i) == 0.0f);
		devices_set_power(&d, i, 1.0f);
		log_start(&l, &d);
		run(&d, Tp + 2, i, &l);
		CHECK(l.first[DEV_EV_REPOWERED] == 0 && devices_power(&d, i) == 1.0f);
		/* depowered: drifts to 0 in depowered_ticks, or stays when the tag has no such time */
		memcpy(&d, &base, sizeof d);
		devices_set_position_immediate(&d, i, 1.0f);
		devices_set_power_immediate(&d, i, 1.0f);
		devices_set_power_immediate(&d, i, 0.0f);                                           /* power DROPS to 0 */
		d.dev[i].target = d.dev[i].pos;
		log_start(&l, &d);
		if (df.depowered_ticks) {
			uint32_t D = cap_ticks(df.depowered_ticks);
			run(&d, D + 2, i, &l);
			CHECK(l.first[DEV_EV_CLOSING] == 0 && l.first[DEV_EV_CLOSED] == (int32_t)D - 1 && devices_position(&d, i) == 0.0f);
		} else {
			run(&d, 10, i, &l);
			CHECK(devices_position(&d, i) == 1.0f && l.count[DEV_EV_CLOSING] == 0);
		}
		/* doors that open for activators */
		if (df.kind == DEVDEF_MACHINE && df.subtype == DEVDEF_MACH_DOOR && df.auto_radius > 0.0f && !(dv.kind_flags & DEVDEF_MPF_NO_AUTO) &&
		    dv.position_group < 0 && !once) {
			dev_activator act;
			uint32_t K = T + 3, N = df.open_ticks, t;
			float yaw = dv.rot[0], off = df.auto_radius < 1.0f ? 0.5f * df.auto_radius : 0.5f;
			memcpy(&d, &base, sizeof d);
			devices_set_power_immediate(&d, i, 1.0f);
			devices_set_position_immediate(&d, i, 0.0f);
			act.player = 1;
			act.pos[0] = dv.pos[0] + off * cosf(yaw);                                     /* in front of a one-sided door too */
			act.pos[1] = dv.pos[1] + off * sinf(yaw);
			act.pos[2] = dv.pos[2];
			log_start(&l, &d);
			for (t = 0; t < K; t++) {
				devices_set_activators(&d, &act, 1);
				devices_tick(&d);
				while (devices_pop_event(&d, &ev))
					if (ev.device == i) {
						if (l.first[ev.type] < 0) l.first[ev.type] = (int32_t)(ev.tick - l.t0);
						l.count[ev.type]++;
					}
			}
			run(&d, N + T + 5, i, &l);
			CHECK(l.first[DEV_EV_ACTIVATED] == 0 && l.first[DEV_EV_OPENING] == 0 && l.first[DEV_EV_OPENED] == (int32_t)T - 1);
			CHECK(l.first[DEV_EV_CLOSING] == (int32_t)(K - 1 + N + 1) && l.first[DEV_EV_CLOSED] == (int32_t)(K - 1 + N + 1 + T - 1));
			CHECK(devices_position(&d, i) == 0.0f && l.count[DEV_EV_OPENED] == 1 && l.count[DEV_EV_CLOSED] == 1);
			doors++;
		}
		/* controls: usable with power, from the front (or both sides); toggles flip their group */
		if (df.kind == DEVDEF_CONTROL) {
			float fwd[3] = { cosf(dv.rot[0]), sinf(dv.rot[0]), 0.0f };
			float front[3] = { dv.pos[0] + fwd[0], dv.pos[1] + fwd[1], dv.pos[2] };
			float back[3] = { dv.pos[0] - fwd[0], dv.pos[1] - fwd[1], dv.pos[2] };
			memcpy(&d, &base, sizeof d);
			devices_set_power_immediate(&d, i, 0.0f);
			CHECK(devices_use(&d, i, front) == DEV_USE_NO_POWER && devices_pop_event(&d, &ev) && ev.type == DEV_EV_DENIED);
			devices_set_power_immediate(&d, i, 1.0f);
			CHECK(devices_use(&d, i, back) == ((dv.device_flags & DEVDEF_PF_BOTH_SIDES) ? DEV_USE_OK : DEV_USE_WRONG_SIDE));
			memcpy(&d, &base, sizeof d);
			devices_set_power_immediate(&d, i, 1.0f);
			{
				float before = dv.position_group >= 0 ? d.group_value[dv.position_group] : 0.0f;
				int r = devices_use(&d, i, front);
				CHECK(r == DEV_USE_OK || r == DEV_USE_LOCKED);
				if (r == DEV_USE_OK && dv.position_group >= 0) {
					float after = d.group_value[dv.position_group];
					if (df.subtype == DEVDEF_CTRL_TOGGLE) CHECK(after == (before >= 0.5f ? 0.0f : 1.0f));
					if (df.subtype == DEVDEF_CTRL_ON) CHECK(after == 1.0f);
					if (df.subtype == DEVDEF_CTRL_OFF) CHECK(after == 0.0f);
				}
			}
		}
		tested++;
	}
	/* groups: setting a group to 1 drives every bound device (reversed ones to 0) and every power group */
	for (g = 0; g < base.group_count; g++) {
		memcpy(&d, &base, sizeof d);
		devices_group_set(&d, g, 1.0f);
		for (i = 0; i < d.count; i++) {
			devdef_device dv = devdef_get_device(&s, i);
			if (dv.position_group == (int)g && !(d.dev[i].flags & DEV_S_LOCKED))
				CHECK(devices_position_target(&d, i) == ((dv.device_flags & DEVDEF_PF_REVERSED) ? 0.0f : 1.0f));
			if (dv.power_group == (int)g) CHECK(d.dev[i].power_target == (1ull << 32));
		}
		CHECK(d.group_value[g] == 1.0f);
	}
	/* volumes: the same helper agrees with an independent solve on interior points */
	for (i = 0; i < s.volume_count; i++) {
		devdef_volume v = devdef_get_volume(&s, i);
		float c[3];
		volume_center(&v, c);
		CHECK(volume_contains(&v, c));
		volume_ok++;
		{
			float corner[3] = { v.position[0], v.position[1], v.position[2] };
			float out[3] = { v.position[0] - 0.01f * v.forward[0], v.position[1] - 0.01f * v.forward[1], v.position[2] - 0.01f * v.forward[2] };
			CHECK(volume_contains(&v, corner) && !volume_contains(&v, out));
		}
	}
	/* initial devices: positions and powers are legal fractions */
	for (i = 0; i < base.count; i++) CHECK(devices_position(&base, i) >= 0.0f && devices_position(&base, i) <= 1.0f && devices_power(&base, i) <= 1.0f);
	printf("  %s: %u devices tested (%u activator doors), %u groups, %lu volumes\n", path, tested, doors, base.group_count, volume_ok);
	free(raw);
	free(img);
	return 0;
}

/* random boxes against an independent solver (Cramer's rule in single precision-safe margins) */
static double det3(const float *a, const float *b, const float *c)
{
	return (double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) - (double)a[1] * ((double)b[0] * c[2] - (double)b[2] * c[0]) +
	       (double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0]);
}

static int volume_fuzz(void)
{
	uint32_t rng = 777;
	unsigned long total = 0, inside = 0;
	int b, t, k;
#define RND() (rng = rng * 1664525u + 1013904223u, (float)(rng >> 8) / 16777216.0f)
	for (b = 0; b < 200; b++) {
		devdef_volume v;
		float fw[3], up[3], dot = 0.0f, n = 0.0f;
		memset(&v, 0, sizeof v);
		for (k = 0; k < 3; k++) fw[k] = RND() * 2.0f - 1.0f;
		n = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
		if (n < 0.1f) continue;
		for (k = 0; k < 3; k++) { fw[k] /= n; up[k] = RND() * 2.0f - 1.0f; }
		for (k = 0; k < 3; k++) dot += up[k] * fw[k];
		for (k = 0; k < 3; k++) up[k] -= dot * fw[k];
		n = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		if (n < 0.1f) continue;
		for (k = 0; k < 3; k++) up[k] /= n;
		v.left[0] = up[1] * fw[2] - up[2] * fw[1];                                        /* up x forward */
		v.left[1] = up[2] * fw[0] - up[0] * fw[2];
		v.left[2] = up[0] * fw[1] - up[1] * fw[0];
		for (k = 0; k < 3; k++) {
			v.forward[k] = fw[k];
			v.up[k] = up[k];
			v.position[k] = (RND() - 0.5f) * 100.0f;
			v.extents[k] = 0.5f + RND() * 10.0f;
		}
		for (t = 0; t < 100; t++) {
			float p[3], d3[3], den, s[3], margin = 1e30f;
			int in = 1;
			if (t & 1) {
				float a = RND() * v.extents[0], bb = RND() * v.extents[1], c = RND() * v.extents[2];
				for (k = 0; k < 3; k++) p[k] = v.position[k] + a * v.forward[k] + bb * v.left[k] + c * v.up[k];
			} else {
				for (k = 0; k < 3; k++) p[k] = v.position[k] + (RND() - 0.5f) * 30.0f;
			}
			for (k = 0; k < 3; k++) d3[k] = p[k] - v.position[k];
			den = (float)det3(v.forward, v.left, v.up);
			s[0] = (float)(det3(d3, v.left, v.up) / den);
			s[1] = (float)(det3(v.forward, d3, v.up) / den);
			s[2] = (float)(det3(v.forward, v.left, d3) / den);
			for (k = 0; k < 3; k++) {
				float m = fminf(fabsf(s[k]), fabsf(s[k] - v.extents[k]));
				if (m < margin) margin = m;
				if (s[k] < 0.0f || s[k] > v.extents[k]) in = 0;
			}
			if (margin < 1e-3f) continue;                                                  /* rounding may decide on a face */
			total++;
			inside += (unsigned long)in;
			CHECK(volume_contains(&v, p) == in);
		}
	}
	CHECK(total > 15000 && inside > total / 4 && inside < total * 3 / 4);
	printf("  volume_contains: %lu random points (%lu inside) agree with the independent solver\n", total, inside);
	return 0;
}

int main(int argc, char **argv)
{
	int a;
	CHECK(argc >= 2);
	if (synthetic(argv[1])) return 1;
	if (volume_fuzz()) return 1;
	for (a = 2; a < argc; a++)
		if (real_pack(argv[a])) return 1;
	puts("ok");
	return 0;
}
