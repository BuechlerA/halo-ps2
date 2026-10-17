/* Host -> VRAM image transfers (GS PATH3) as GIF-channel source chains. Pure C11, host-tested.
 * Image data must already be flushed from the D-cache (sections are flushed once after load). */
#ifndef RENDER_UPLOAD_H
#define RENDER_UPLOAD_H
#include "render/chain.h"
#include <stdint.h>

/* Queue a transfer of `bytes` (multiple of 16) of `psm` pixels, `w` x `h`, into VRAM block
 * `dst_block` (64-word units) with buffer width `dst_bw` (64-pixel units). Costs 2 chain tags and
 * 7 quadwords. Returns 0, or -1 if the chain is full. */
int gs_upload(chain_t *c, const void *pixels, uint32_t bytes, uint32_t dst_block, uint32_t dst_bw,
              uint32_t psm, uint32_t w, uint32_t h);

#endif
