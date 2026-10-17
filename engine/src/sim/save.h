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

/* Relocatable byte image (zero-run compressed) for memory-card saves: header (magic, version, struct sizes, checksum) + payload. The
 * pointers inside the copied structs are NOT meaningful in another run: revert/unpack rebinds them to the live game's. Returns the size,
 * or 0 if `cap` is too small. */
size_t checkpoint_pack(const checkpoint_t *c, uint8_t *out, size_t cap);
/* Returns 1 on success; 0 for a damaged, truncated or incompatible image (nothing is modified). `scratch` is a checkpoint_t-sized buffer. */
int    checkpoint_unpack(checkpoint_t *scratch, const uint8_t *in, size_t n);
/* Generic zero-run packed image of any plain-data blob (magic identifies the kind; the header carries size and checksum). */
size_t blob_pack(const void *src, size_t n, uint32_t magic, uint8_t *out, size_t cap);
/* 1 on success: the image has exactly `n` bytes of payload for this magic and its checksum matches (dst untouched otherwise). */
int    blob_unpack(void *dst, size_t n, uint32_t magic, const uint8_t *in, size_t in_n);
size_t blob_pack_max(size_t n);
/* Worst-case packed size. */
size_t checkpoint_pack_max(void);

#endif
