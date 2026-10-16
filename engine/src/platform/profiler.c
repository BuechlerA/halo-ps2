#include "platform/profiler.h"
#include "platform/timer.h"
#include <stdio.h>
#include <string.h>

#define WINDOW 60

void profiler_init(profiler_t *p)
{
	memset(p, 0, sizeof *p);
	p->overlay = 1;
	p->last_flip = timer_cycles();
}

void profiler_frame(profiler_t *p, float prep_ms, float wait_ms, const render_stats_t *rs, u32 now)
{
	float interval = timer_ms(p->last_flip, now);
	p->last_flip = now;
	bsp_render_timing(&p->vif_ms, &p->gs_ms);
	if (p->frames > 0 && interval > 25.0f) {
		if (p->drop_n < 6) {
			p->drop_frame[p->drop_n] = p->frames; p->drop_prep[p->drop_n] = prep_ms;
			p->drop_gs[p->drop_n] = p->gs_ms; p->drop_upload_kb[p->drop_n] = (float)(rs->upload_bytes / 1024);
			p->drop_n++;
		}
		p->dropped++; p->win_dropped++;
	}
	p->frames++;
	p->window_frames++;
	p->interval_acc += interval;
	if (interval > p->interval_max) p->interval_max = interval;
	p->prep_acc += prep_ms;
	if (prep_ms > p->prep_max) p->prep_max = prep_ms;
	p->wait_acc += wait_ms;
	p->last = *rs;
	if (p->gs_ms > p->gs_max) p->gs_max = p->gs_ms;
	if (p->vif_ms > p->vif_max) p->vif_max = p->vif_ms;
	p->gs_acc += p->gs_ms > 0.0f ? p->gs_ms : 0.0f;
	if (prep_ms > p->prep_win_max) p->prep_win_max = prep_ms;
	if (p->window_frames >= WINDOW) {
		p->gs_avg = p->gs_acc / (float)p->window_frames; p->gs_peak = p->gs_max; p->vif_peak = p->vif_max;
		p->gs_acc = p->gs_max = p->vif_max = p->prep_win_max = 0.0f;
		p->fps = 1000.0f * (float)p->window_frames / p->interval_acc;
		p->prep_avg = p->prep_acc / (float)p->window_frames;
		p->prep_peak = p->prep_max;
		p->wait_avg = p->wait_acc / (float)p->window_frames;
		p->window_frames = 0;
		p->interval_acc = p->interval_max = p->prep_acc = p->prep_max = p->wait_acc = 0.0f;
		p->win_dropped = 0;
	}
}

void profiler_overlay(const profiler_t *p, text_batch_t *b)
{
	char line[96];
	const render_stats_t *r = &p->last;
	text_batch_clear(b);
	if (!p->overlay) return;
	snprintf(line, sizeof line, "FPS %.1f  FRAME %u  DROPPED %u", p->fps, p->frames, p->dropped);
	text_batch_print(b, 8, 8, p->dropped ? 0xFF6060 : 0xFFFFFF, line);
	snprintf(line, sizeof line, "EE %.2f/%.2f MS  VIS %.2f  BUILD %.2f  WAIT %.2f", p->prep_avg, p->prep_peak, r->vis_ms, r->build_ms, p->wait_avg);
	text_batch_print(b, 8, 16, 0xC0FFC0, line);
	snprintf(line, sizeof line, "VIF1 %.2f/%.2f  GS AVG %.2f MAX %.2f MS", p->vif_ms, p->vif_peak, p->gs_avg, p->gs_peak);
	text_batch_print(b, 8, 24, 0xC0FFC0, line);
	snprintf(line, sizeof line, "TRIS %u  BATCHES %u  MATS %u  CLUSTERS %u/%u%s", r->triangles, r->batches, r->materials_drawn,
	         r->clusters_visible, r->clusters_total, r->camera_cluster_valid ? "" : " (OUTSIDE)");
	text_batch_print(b, 8, 32, 0xFFFFC0, line);
	snprintf(line, sizeof line, "TEX HIT %u UP %u (%u KB) DEFER %u FAIL %u FREE %u PG", r->tex_hits, r->uploads, r->upload_bytes / 1024,
	         r->tex_deferred, r->tex_failed, r->pages_free);
	text_batch_print(b, 8, 40, 0xFFFFC0, line);
	if (p->drop_n) {
		unsigned i;
		for (i = 0; i < p->drop_n && i < 3; i++) {
			snprintf(line, sizeof line, "DROP F%u EE %.1f GS %.1f UP %.0fKB", (unsigned)p->drop_frame[i], p->drop_prep[i], p->drop_gs[i], p->drop_upload_kb[i]);
			text_batch_print(b, 8, 64 + 8 * (int)i, 0xFF8080, line);
		}
	}
	snprintf(line, sizeof line, "CHAIN %u QW%s  FLAT %u  CLIP %u/%u", r->chain_qw, r->chain_overflow ? " OVERFLOW" : "", r->untextured_batches,
	         (unsigned)r->clip_polys, (unsigned)r->clip_tris);
	text_batch_print(b, 8, 48, r->chain_overflow ? 0xFF6060 : 0xFFFFC0, line);
}

void profiler_report(const profiler_t *p)
{
	const render_stats_t *r = &p->last;
	printf("[prof] frames=%u fps=%.1f dropped=%u (%u this window) interval max=%.1fms | ee prep avg=%.2f peak=%.2f vis=%.2f wait=%.2f | "
	       "vif1=%.2f gs=%.2f | tris=%u batches=%u mats=%u clusters=%u/%u portals=%u/%u | tex hit=%u up=%u/%uKB defer=%u fail=%u free=%upg flat=%u | chain=%uqw%s clip=%ub/%u/%u skip=%u drop=%u (%.2fms, polygon %.2fms) build=%.2f\n",
	       p->frames, p->fps, p->dropped, p->win_dropped, p->interval_max, p->prep_avg, p->prep_peak, r->vis_ms, p->wait_avg,
	       p->vif_ms, p->gs_ms, r->triangles, r->batches, r->materials_drawn, r->clusters_visible, r->clusters_total,
	       r->portals_passed, r->portals_tested, r->tex_hits, r->uploads, r->upload_bytes / 1024, r->tex_deferred, r->tex_failed,
	       r->pages_free, r->untextured_batches, r->chain_qw, r->chain_overflow ? " OVERFLOW" : "",
	       (unsigned)r->clip_batches, (unsigned)r->clip_polys, (unsigned)r->clip_tris, (unsigned)r->clip_skipped_batches, (unsigned)r->clip_dropped, r->clip_ms, r->emit_clip_ms, r->build_ms);
}
