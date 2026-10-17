/* VIF1 source-chain builder (docs/03 §5.1 step 4). Pure C11, host-tested.
 *
 * A chain is a flat array of 16-byte quadwords holding DMA tags and the data that follows them.
 * `ref` tags point at data elsewhere (the baked geometry packets); `cnt` tags carry their data
 * inline (per-draw state). The 64 bits above each tag are VIF codes: the channel runs with TTE
 * set, so they are sent before the tag's data, which is how MSCAL for batch i rides on the tag
 * of batch i+1 and a batch costs a single 16-byte tag. */
#ifndef RENDER_CHAIN_H
#define RENDER_CHAIN_H
#include <stdint.h>

typedef struct {
	uint8_t *base;
	uint32_t cap_qw, qw;
	int      overflow;
} chain_t;

enum { DMA_ID_CNT = 1, DMA_ID_REF = 3, DMA_ID_END = 7 };

#define CHAIN_PHYS(p) ((uint32_t)((uintptr_t)(p) & 0x0FFFFFFFu))

void chain_init(chain_t *c, void *mem, uint32_t cap_qw);
/* Reference `qw` quadwords at `data` (must be flushed from the D-cache). */
void chain_ref(chain_t *c, const void *data, uint32_t qw, uint64_t vif_pair);
/* Inline data: returns a pointer to `data_qw` quadwords to fill in, or NULL on overflow. */
void *chain_cnt(chain_t *c, uint32_t data_qw, uint64_t vif_pair);
/* Terminate. The pair runs after all previous data. */
void chain_end(chain_t *c, uint64_t vif_pair);
uint32_t chain_qwords(const chain_t *c);

#endif
