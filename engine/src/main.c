/* halo-ps2 — Phase 0 boot: 512x448 @ 60 Hz, VU1 triangle, 30 Hz tick / 60 Hz render
 * scheduler skeleton with a cycle-accurate profiler printed to TTY. */
#include <kernel.h>
#include <stdio.h>
#include <dma.h>
#include <packet2.h>
#include <draw.h>
#include <graph.h>

#include "platform/video.h"
#include "platform/timer.h"
#include "render/vu1_tri.h"

static video_t s_video;

typedef struct {
	u32 frames, ticks, dropped;
	float ee_ms_max, ee_ms_acc;
} profile_t;

static void sim_tick(u32 tick)
{
	/* Placeholder for the 30 Hz simulation: rotate the test triangle. */
	vu1_tri_update((float)tick * 0.02f);
}

static void render_prep(void)
{
	/* NORMAL mode: draw_* helpers emit raw GIF data, no DMA tags (chain mode would misread them). */
	packet2_t *p = packet2_create(64, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);
	video_add_setup_env(&s_video, p);
	video_add_clear(&s_video, p, 20, 24, 40);
	packet2_update(p, draw_finish(p->next));
	dma_channel_send_packet2(p, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	packet2_free(p);

	vu1_tri_draw();
}

int main(int argc, char *argv[])
{
	profile_t prof = {0};
	u32 tick = 0;
	u32 prev = 0;

	setbuf(stdout, NULL);   /* ps2sdk stdout is not a tty: unbuffered so TTY logs appear immediately */
	printf("halo-ps2 phase 0 boot\n");

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);

	video_init(&s_video);
	vu1_tri_init();

	prev = timer_cycles();
	for (;;) {
		u32 t0 = timer_cycles();
		int tick_frame = (prof.frames & 1) == 0;   /* ADR-002: sim every other vblank */
		float ee_ms;

		if (tick_frame) {
			sim_tick(tick++);
			prof.ticks++;
		}
		render_prep();

		ee_ms = timer_ms(t0, timer_cycles());
		prof.ee_ms_acc += ee_ms;
		if (ee_ms > prof.ee_ms_max) prof.ee_ms_max = ee_ms;

		video_flip(&s_video);

		/* Frame pacing check: anything over ~1.5 vblanks means a missed vsync. */
		{
			u32 now = timer_cycles();
			if (timer_ms(prev, now) > 25.0f) prof.dropped++;
			prev = now;
		}
		prof.frames++;

		if ((prof.frames % 300) == 0) {
			printf("[prof] frames=%u ticks=%u ee avg=%.2fms max=%.2fms dropped=%u\n",
			       prof.frames, prof.ticks, prof.ee_ms_acc / 300.0f, prof.ee_ms_max, prof.dropped);
			prof.ee_ms_acc = 0.0f;
			prof.ee_ms_max = 0.0f;
		}
	}
	return 0;
}
