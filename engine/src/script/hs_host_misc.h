/* Script host, part H2 (docs/34-host-misc.md): the built-ins of the camera / cinematic, sound, hud / ui, game / flow, scripted player effect and
 * debug / no-op domains. `host` of the script machine is a `mission_t` (sim/mission.h); the state lives in `mission_t.misc`. Pure C11. */
#ifndef SCRIPT_HS_HOST_MISC_H
#define SCRIPT_HS_HOST_MISC_H
#include "sim/mission.h"

/* Registers every built-in of the domains above (by name). Call after hs_setup, before or after hs_init. */
void hs_host_register_misc(hs_machine *m);

/* Optional once-per-tick service for the integrator (call before cine_tick, any time after the script machine): refreshes the player camera pose the
 * camera_set transitions start from, the frame of a relative camera's object, and the navigation points' positions. */
void misc_tick(mission_t *m);
void misc_nav_refresh(mission_t *m);

/* Navigation points: the number active, entry i of the active ones (0 = oldest). Returns NULL past the end. */
unsigned         misc_nav_count(const mission_t *m);
const misc_nav  *misc_nav_get(const mission_t *m, unsigned i);

/* The scripted player effect at the mission's current tick: shake amplitudes (radians pitch / yaw / roll, world units x y z) and rumble motors
 * (0..1) already scaled by the envelope level (0..1, returned). The integrator adds noise of that size to the player's camera and drives the pad. */
float misc_player_effect_get(const mission_t *m, float rot[3], float trans[3], float rumble[2]);

/* HUD timer (hud_set_timer_time / pause_hud_timer): ticks left now. */
uint32_t misc_timer_ticks(const mission_t *m);

/* Remaining ticks of the most recent sound_impulse_start of `tag` (what sound_impulse_time answers). */
uint32_t misc_impulse_ticks(const mission_t *m, uint32_t tag);

/* Statistics: calls per registered built-in since start (or the last reset), in registration order. Returns the number of built-ins. */
unsigned hs_host_misc_stats(unsigned k, const char **name, uint32_t *calls);
void     hs_host_misc_reset_stats(void);

#endif
