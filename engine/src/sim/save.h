/* Checkpoint snapshot and revert (docs/03 §6): the mutable simulation state (game, object table, first-person view, effects) is copied
 * into a preallocated block; revert copies it back. All data pointers stay valid because the snapshot is restored in the same session.
 * ~300 KB, within the 512 KB budget. Pure C11, host-tested. */
#ifndef SIM_SAVE_H
#define SIM_SAVE_H
#include "sim/fx.h"
#include "sim/game.h"

typedef struct {
	int           valid;
	uint32_t      tick;
	game_t        game;
	object_table_t objects;
	fpview_t      fp;
	fx_t          fx;
	int           has_fx;
} checkpoint_t;

/* Saves the state of `g` (its object table and first-person view are the ones g points at); `fx` may be NULL. */
void checkpoint_save(checkpoint_t *c, const game_t *g, const fpview_t *fp, const fx_t *fx);
/* Restores it; returns 0 if there is no snapshot. */
int  checkpoint_revert(const checkpoint_t *c, game_t *g, object_table_t *objects, fpview_t *fp, fx_t *fx);

#endif
