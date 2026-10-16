/* Dynamic LOD controller (docs/02-specs.md §5.1, roadmap Phase 2): trades draw distance, model LOD and
 * particle counts for frame time. Pure C11, host-tested. Feed it the measured frame time (or the
 * dropped-frame event) once per frame; read the three scales when building the next frame.
 *
 * Quality level 0 = full; each step down lowers the draw distance, raises the LOD bias and shrinks
 * the particle cap. A missed frame steps down at once and holds the level for ~2 s; sustained slack steps up. */
#ifndef RENDER_LODCTL_H
#define RENDER_LODCTL_H

#define LODCTL_MAX_LEVEL 4

typedef struct {
	float budget_ms;           /* frame budget (33.3 at 30 fps) */
	float smooth;              /* smoothed frame time, ms */
	int   level;               /* 0..LODCTL_MAX_LEVEL */
	int   hold;                /* frames the level may not step up */
	int   slack;               /* consecutive frames comfortably under budget */
	int   hold_frames;         /* hold length after a miss (2 s worth of frames) */
	unsigned downs, ups;       /* statistics */
} lodctl_t;

void  lodctl_init(lodctl_t *c, float budget_ms, int frames_per_second);
/* `frame_ms`: the frame's cost (the larger of EE build and GS time when known, else the flip interval);
 * `dropped`: the frame missed its slot. */
void  lodctl_frame(lodctl_t *c, float frame_ms, int dropped);
float lodctl_distance_scale(const lodctl_t *c);   /* multiplies object draw distances: 1.0 .. 0.4 */
int   lodctl_lod_bias(const lodctl_t *c);         /* added to the chosen model LOD index: 0 .. 2 */
float lodctl_particle_scale(const lodctl_t *c);   /* multiplies particle caps: 1.0 .. 0.25 */

#endif
