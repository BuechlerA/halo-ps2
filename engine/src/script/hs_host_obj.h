/* Script host, part H1 (docs/33-host-objects.md): the object, unit, player, device and effect built-ins of the script machine
 * (plus recording_* / custom_animation*). The built-ins are registered by name; the host is a `mission_t *` (sim/mission.h) and
 * the persistent state of the module lives in `mission_t.hobj` (plain data, saved with the checkpoint).
 *
 * Wiring (the integrator's mission_tick): after the script machine and devices call hs_host_obj_pre() (it feeds recorded animations
 * to their units and latches the player's input for player_action_test_*), after units_tick / game_tick call hs_host_obj_post()
 * (attachments follow their parents, custom animations hand the graph back, suspended units stay put). Everything is NULL tolerant:
 * a mission without units, game, devices, cine or fx still runs, the affected built-ins do nothing. */
#ifndef SCRIPT_HS_HOST_OBJ_H
#define SCRIPT_HS_HOST_OBJ_H
#include "script/hs.h"
#include "sim/input.h"
#include "sim/mission.h"

/* mission_hostobj.action_latched bits (player_action_test_*) */
#define HOBJ_PA_ACCEPT   0x0001u
#define HOBJ_PA_PRIMARY  0x0002u
#define HOBJ_PA_GRENADE  0x0004u
#define HOBJ_PA_ACTION   0x0008u
#define HOBJ_PA_BACK     0x0010u
#define HOBJ_PA_LOOK_UP    0x0020u
#define HOBJ_PA_LOOK_DOWN  0x0040u
#define HOBJ_PA_LOOK_LEFT  0x0080u
#define HOBJ_PA_LOOK_RIGHT 0x0100u

void hs_host_register_objects(hs_machine *m);

/* Once per mission tick, after the script machine and the devices, before the AI / units. `in` may be NULL. */
void hs_host_obj_pre(mission_t *m, const game_input_t *in);
/* Once per mission tick, after units_tick and game_tick. */
void hs_host_obj_post(mission_t *m);
/* An object is going away (also called by object_destroy): drops attachments, flags and recordings that refer to the slot. */
void hs_host_obj_forget(mission_t *m, int obj);

/* Diagnostics: calls per registered built-in since the last reset (process wide, the module keeps no other global state). */
unsigned    hs_host_obj_count(void);
const char *hs_host_obj_name(unsigned i);
uint32_t    hs_host_obj_calls(unsigned i);
void        hs_host_obj_stats_reset(void);

#endif
