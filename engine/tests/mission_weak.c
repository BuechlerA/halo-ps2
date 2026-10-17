/* Weak stand-ins for the parts of the mission tick a host test does not link (mission_tick calls all of them). */
#include "sim/mission.h"
#include "script/hs_host_obj.h"
#include "script/hs_host_misc.h"
#include "ai/ai.h"

__attribute__((weak)) void ai_tick(ai_t *ai) { (void)ai; }
__attribute__((weak)) void hs_host_obj_pre(mission_t *m, const game_input_t *in) { (void)m; (void)in; }
__attribute__((weak)) void hs_host_obj_post(mission_t *m) { (void)m; }
__attribute__((weak)) void misc_tick(mission_t *m) { (void)m; }
