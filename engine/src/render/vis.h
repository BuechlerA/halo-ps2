/* Cluster/portal visibility (docs/03 §5.1 step 1). Pure C11, host-tested; no heap (buffers come
 * from an arena at BSP load).
 *
 * Starting in the camera's cluster, portals are followed whose polygon is in front of the eye and
 * inside the current screen-space window (a Quake-style portal scissor in NDC; the window of a
 * cluster reached by several routes is the union). Batches of visible clusters are then culled
 * against the view frustum and the cluster's window. A camera outside the map (or in solid
 * space) falls back to a frustum test of every cluster's bounds. */
#ifndef RENDER_VIS_H
#define RENDER_VIS_H
#include "core/mem.h"
#include "render/camera.h"
#include "world/bsp.h"

typedef struct {
	/* output of vis_run */
	uint32_t *batches;           /* visible batch indices (de-duplicated, unordered) */
	uint32_t  batch_count;
	uint16_t *clusters;          /* visible clusters */
	uint32_t  cluster_count;
	int       camera_cluster;    /* -1: outside the map */
	int       sky_visible;
	int       sky_index;         /* sky palette entry of the camera cluster (else of the first visible cluster with a sky); 0 when none */
	int       force_all;         /* debug: skip portal traversal, test every cluster against the frustum */
	/* statistics */
	uint32_t  triangles, batches_tested, batches_culled, portals_tested, portals_passed;
	/* internal */
	uint32_t *batch_stamp;
	uint32_t  stamp, cap_batches, cap_clusters;
	uint32_t  max_pushes;
	float    *window;            /* 4 floats per cluster: x0 y0 x1 y1 in NDC; x0 > x1 = not visited */
	uint16_t *stack;
} vis_t;

/* Allocate buffers for `s` from `a`. Returns 0 on success. */
int  vis_init(vis_t *v, arena_t *a, const bsp_section *s);
/* `clip` is camera_clip_matrix(cam). */
void vis_run(vis_t *v, const bsp_section *s, const camera_t *cam, const frustum_t *fr, const float clip[4][4]);

#endif
