#include "platform/fbdump.h"
#include "platform/video.h"
#include <screenshot.h>
#include <stdio.h>

/* 16-bit pixels: 2 bytes each. Uncached-friendly alignment for the DMA readback. */
static u8 s_pixels[VIDEO_WIDTH * VIDEO_HEIGHT * 2] __attribute__((aligned(64)));

void fbdump_frame(const framebuffer_t *fb, u32 frame)
{
	static const char hex[] = "0123456789abcdef";
	char row[VIDEO_WIDTH * 2 * 2 + 1];
	int ret;
	u32 x, y;

	ret = ps2_screenshot(s_pixels, fb->address / 64, 0, 0, fb->width, fb->height, fb->psm);
	/* The readback leaves VIF1 in FIFO-reverse mode (FDR); clear it or the next VIF1 DMA never completes. */
	*(volatile u32 *)0x10003C00 = 0;   /* VIF1_STAT */
	printf("[fb] begin frame=%u w=%d h=%d psm=%d addr=%u ret=%d\n", frame, fb->width, fb->height, fb->psm, fb->address, ret);
	for (y = 0; y < (u32)fb->height; y++) {
		const u8 *src = s_pixels + y * fb->width * 2;
		for (x = 0; x < (u32)fb->width * 2; x++) {
			row[2 * x] = hex[src[x] >> 4];
			row[2 * x + 1] = hex[src[x] & 15];
		}
		row[fb->width * 4] = 0;
		printf("[fb] %d %s\n", y, row);
	}
	printf("[fb] end frame=%u\n", frame);
}
