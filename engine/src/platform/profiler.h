/* Frame profiler: rolling statistics over the last second, TTY report, on-screen overlay.
 * Budgets are in docs/02-specs.md §5; R2 counts missed vsyncs ("dropped"). */
#pragma once
#include <tamtypes.h>
#include "render/bsp_render.h"
#include "render/text.h"

typedef struct {
	u32   frames, dropped, window_frames;
	float drop_ms;
	u32   last_flip;                       /* timer_cycles() at the previous flip */
	float interval_acc, interval_max;      /* ms between flips */
	float prep_acc, prep_max;              /* EE time to build a frame */
	float wait_acc;                        /* EE idle time waiting for GS + vsync */
	float vif_ms, gs_ms;                   /* last observed kick -> VIF1 done / GS FINISH */
	float fps;                             /* over the last full window */
	float prep_avg, prep_peak, wait_avg;
	u32   win_dropped;
	float gs_max, gs_acc, vif_max, prep_win_max;  /* per window: GS/VIF1 times and worst EE build */
	float gs_avg, gs_peak, vif_peak;
	u32   drop_frame[6], drop_n;                  /* first dropped frames, with the times around them */
	float drop_prep[6], drop_gs[6], drop_upload_kb[6];
	float last_interval;                          /* ms between the last two flips */
	float obj_ms;
	unsigned obj_visible, obj_culled, obj_batches, obj_tris, lod_level;   /* object pass, set by the caller */
	int   overlay;                         /* draw the text overlay */
	render_stats_t last;
} profiler_t;

/* `drop_ms`: a flip interval above this counts as a dropped frame (1.5 x the nominal frame time). */
void profiler_init(profiler_t *p, float drop_ms);
/* Call once per frame after the flip. `prep_ms`/`wait_ms` are this frame's EE times. */
void profiler_frame(profiler_t *p, float prep_ms, float wait_ms, const render_stats_t *rs, u32 now_cycles);
/* Fills `b` with the overlay for the most recent statistics. */
void profiler_overlay(const profiler_t *p, text_batch_t *b);
/* One "[prof]" TTY line (called every 60 frames). */
void profiler_report(const profiler_t *p);
