/* Mission checkpoints: the Phase 3 game checkpoint plus everything Phase 4 added (units, AI, script machine, devices, cinematics, host state).
 * Raw copies of plain-data structs, rebound to the live data after a restore; the packed form goes to the memory card (relocatable between runs of
 * the same build). ~650 KB. The caller restores the BSP first when `msave_bsp` differs from the loaded one. Pure C11, host-tested. */
#ifndef SIM_MSAVE_H
#define SIM_MSAVE_H
#include "ai/ai.h"
#include "sim/mission.h"
#include "sim/save.h"

typedef struct {
	int32_t         valid;
	int32_t         cur_bsp;
	uint32_t        tick, difficulty;
	int32_t         won, lost, cinematic, player_obj;
	uint32_t        objective_flags;
	checkpoint_t    base;
	units_t         units;
	ai_state        ai;
	hs_state        hs;
	devices_t       devices;
	cine_state      cine;
	mission_misc_t  misc;
	mission_hostobj hobj;
} mission_checkpoint_t;

/* `ai` / `hs` / `devices` / `cine` come from the mission (any may be NULL when the mission does not use it). */
void msave_save(mission_checkpoint_t *c, mission_t *m, const fx_t *fx);
/* Restores everything and rebinds; returns 0 when `c` holds no snapshot or a rebind failed. The object table, game and first-person view are those the
 * mission points at. */
int  msave_revert(const mission_checkpoint_t *c, mission_t *m, fx_t *fx);
int  msave_bsp(const mission_checkpoint_t *c);
size_t msave_pack(const mission_checkpoint_t *c, uint8_t *out, size_t cap);
int    msave_unpack(mission_checkpoint_t *c, const uint8_t *in, size_t n);
size_t msave_pack_max(void);

#endif
