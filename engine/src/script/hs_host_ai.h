/* Script host, part A (docs/32-ai-runtime.md): the `ai` domain built-ins of the script machine, on top of the AI runtime (ai/ai.h). The host of
 * every built-in is a `mission_t *` (sim/mission.h); the AI runtime is `mission_t.ai` (a `struct ai` = ai_t, bound by the integrator with ai_init),
 * and every built-in is a no-op returning the neutral value when it is NULL. */
#ifndef SCRIPT_HS_HOST_AI_H
#define SCRIPT_HS_HOST_AI_H
#include "script/hs.h"

/* Registers every ai built-in (by name). Call after hs_setup, before or after hs_init. */
void hs_host_register_ai(hs_machine *m);

/* Diagnostics: calls per registered built-in since the last reset (process wide). */
unsigned    hs_host_ai_count(void);
const char *hs_host_ai_name(unsigned i);
uint32_t    hs_host_ai_calls(unsigned i);
void        hs_host_ai_stats_reset(void);

#endif
