#include "render/selftest.h"
#include "render/chain.h"
#include "render/gs.h"
#include "render/upload.h"
#include <kernel.h>
#include <dma.h>
#include <draw.h>
#include <packet2.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define GS_CSR ((volatile u64 *)0x12001000)

static u8       t4_pixels[64 * 64 / 2] __attribute__((aligned(64)));
static u8       t8_pixels[64 * 64] __attribute__((aligned(64)));
static uint32_t clut16[16] __attribute__((aligned(64)));
static uint32_t clut256[256] __attribute__((aligned(64)));
static u64      mem[512 * 2] __attribute__((aligned(64)));

/* Draw a 256x256 sprite (4x magnified 64x64 texture) at (x, y). */
static void sprite(chain_t *c, u64 tex0, int x, int y, int sw, int sh, video_t *v)
{
	u64 *q = chain_cnt(c, 1 + 3 + 6 + 1 + 4, 0);
	int ox = 2048 - VIDEO_WIDTH / 2, oy = 2048 - VIDEO_HEIGHT / 2;
	(void)v;
	q[0] = GIF_TAG(3, 0, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
	q[2] = tex0;                                    q[3] = GS_A_TEX0_1;
	q[4] = GS_TEX1(1, 0, 0, 0, 0, 0, 0);            q[5] = GS_A_TEX1_1;          /* nearest */
	q[6] = GS_TEST(0, 1, 0, 0, 0, 0, 1, 1);         q[7] = GS_A_TEST_1;
	q[8] = GIF_TAG(2, 1, 1, GS_PRIM(6, 0, 1, 0, 0, 0, 1, 0, 0), GIFX_FLG_PACKED, 2);
	q[9] = 0x53;                                    /* UV, XYZ2 */
	q[10] = 0;                                      q[11] = 0;
	q[12] = (u64)((ox + x) * 16) | (u64)((oy + y) * 16) << 32; q[13] = 0;
	q[14] = (u64)(64 * 16) | (u64)(64 * 16) << 32;  q[15] = 0;
	q[16] = (u64)((ox + x + sw) * 16) | (u64)((oy + y + sh) * 16) << 32; q[17] = 0;
}

void selftest_draw(video_t *v)
{
	chain_t c;
	packet2_t *p;
	u32 base = v->tex_base;
	u32 t4a = (base + 32 * 2048) / 64, t4b = (base + 34 * 2048) / 64, t8 = (base + 36 * 2048) / 64;
	u32 ca = base / 64, cb = base / 64 + 4, c8 = base / 64 + 8;
	int x, y, i;

	/* index of a 4x4-pixel cell: 16 cells over the texture; low nibble = even pixel */
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) {
			int idx = ((x / 16) & 3) + 4 * ((y / 16) & 3);
			if (x & 1) t4_pixels[(y * 64 + x) / 2] |= (u8)(idx << 4); else t4_pixels[(y * 64 + x) / 2] = (u8)idx;
			t8_pixels[y * 64 + x] = (u8)(((x / 4) & 15) + 16 * ((y / 4) & 15));
		}
	for (i = 0; i < 16; i++) clut16[i] = (uint32_t)(i * 17) | (uint32_t)(255 - i * 17) << 8 | (uint32_t)((i & 1) * 255) << 16 | 0x80u << 24;
	for (i = 0; i < 256; i++) {
		int slot = (i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1);          /* CSM1 swap of bits 3 and 4 */
		clut256[slot] = (uint32_t)i | (uint32_t)(255 - i) << 8 | (uint32_t)((i & 15) * 16) << 16 | 0x80u << 24;
	}
	SyncDCache(t4_pixels, t4_pixels + sizeof t4_pixels); SyncDCache(t8_pixels, t8_pixels + sizeof t8_pixels);
	SyncDCache(clut16, clut16 + 16); SyncDCache(clut256, clut256 + 256);

	*GS_CSR = 2;
	p = packet2_create(64, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	video_add_setup_env(v, p);
	video_add_clear(v, p, 40, 40, 40);
	packet2_add_u64(p, GIF_TAG(1, 1, 0, 0, GIFX_FLG_PACKED, 1)); packet2_add_u64(p, GIF_REGS_AD);
	packet2_add_u64(p, 0); packet2_add_u64(p, 0x7F);
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	packet2_free(p);

	chain_init(&c, mem, 512);
	gs_upload(&c, t4_pixels, sizeof t4_pixels, t4a, 1, GS_PSMT4, 64, 64);
	gs_upload(&c, clut16, 64, ca, 1, GS_PSMCT32, 8, 2);                   /* variant A: 8x2 */
	gs_upload(&c, t4_pixels, sizeof t4_pixels, t4b, 1, GS_PSMT4, 64, 64);
	gs_upload(&c, clut16, 64, cb, 1, GS_PSMCT32, 16, 1);                  /* variant B: 16x1 */
	gs_upload(&c, t8_pixels, sizeof t8_pixels, t8, 1, GS_PSMT8, 64, 64);
	gs_upload(&c, clut256, 1024, c8, 1, GS_PSMCT32, 16, 16);
	sprite(&c, GS_TEX0(t4a, 1, GS_PSMT4, 6, 6, 1, 1, ca, GS_PSMCT32, 0, 0, 1), 8, 8, 192, 192, v);
	sprite(&c, GS_TEX0(t4b, 1, GS_PSMT4, 6, 6, 1, 1, cb, GS_PSMCT32, 0, 0, 1), 208, 8, 192, 192, v);
	sprite(&c, GS_TEX0(t8, 1, GS_PSMT8, 6, 6, 1, 1, c8, GS_PSMCT32, 0, 0, 1), 8, 208, 192, 192, v);
	{
		u64 *q = chain_cnt(&c, 2, 0);
		q[0] = GIF_TAG(1, 1, 0, 0, GIFX_FLG_PACKED, 1); q[1] = GIF_REGS_AD;
		q[2] = 0; q[3] = GS_A_FINISH;
	}
	chain_end(&c, 0);
	SyncDCache(mem, mem + 512 * 2);
	dma_channel_send_chain(DMA_CHANNEL_GIF, mem, 0, 0, 0);
	dma_wait_fast();
	draw_wait_finish();
	printf("[tex] selftest drawn (chain %u qw)\n", (unsigned)chain_qwords(&c));
}
