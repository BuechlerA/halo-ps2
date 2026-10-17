#include "render/upload.h"
#include "render/gs.h"

int gs_upload(chain_t *c, const void *pixels, uint32_t bytes, uint32_t dst_block, uint32_t dst_bw,
              uint32_t psm, uint32_t w, uint32_t h)
{
	uint64_t *q;
	if (bytes == 0 || (bytes & 15) || bytes / 16 > 0x7FFF) return -1;
	q = chain_cnt(c, 6, 0);
	if (!q) return -1;
	q[0] = GIF_TAG(4, 0, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
	q[2] = GS_BITBLTBUF(0, 0, 0, dst_block, dst_bw, psm); q[3] = GS_A_BITBLTBUF;
	q[4] = 0;                                              q[5] = GS_A_TRXPOS;   /* source and dest origin 0, DIR 0 */
	q[6] = GS_TRXREG(w, h);                                q[7] = GS_A_TRXREG;
	q[8] = 0;                                              q[9] = GS_A_TRXDIR;   /* host -> local */
	q[10] = GIF_TAG(bytes / 16, 1, 0, 0, GIFX_FLG_IMAGE, 0); q[11] = 0;
	chain_ref(c, pixels, bytes / 16, 0);
	return c->overflow ? -1 : 0;
}
