#include "render/vis.h"
#include <math.h>
#include <string.h>

#define EYE_EPS 0.02f      /* world units: closer than this to a portal plane counts as "in" the portal */

int vis_init(vis_t *v, arena_t *a, const bsp_section *s)
{
	memset(v, 0, sizeof *v);
	v->cap_batches = s->batch_count;
	v->cap_clusters = s->cluster_count;
	v->max_pushes = s->cluster_count * 4u + 16u;
	v->batches = arena_alloc(a, sizeof(uint32_t) * (s->batch_count + 1), 16);
	v->batch_stamp = arena_alloc(a, sizeof(uint32_t) * (s->batch_count + 1), 16);
	v->clusters = arena_alloc(a, sizeof(uint16_t) * (s->cluster_count + 1), 16);
	v->window = arena_alloc(a, sizeof(float) * 4 * (s->cluster_count + 1), 16);
	v->stack = arena_alloc(a, sizeof(uint16_t) * (v->max_pushes + 1), 16);
	if (!v->batches || !v->batch_stamp || !v->clusters || !v->window || !v->stack) return -1;
	memset(v->batch_stamp, 0, sizeof(uint32_t) * (s->batch_count + 1));
	return 0;
}

static void project(const float m[4][4], const float p[3], float out[4])
{
	int k;
	for (k = 0; k < 4; k++) out[k] = m[0][k] * p[0] + m[1][k] * p[1] + m[2][k] * p[2] + m[3][k];
}

static float *win(vis_t *v, unsigned c) { return v->window + 4 * c; }
static int win_empty(const float *w) { return w[0] > w[2] || w[1] > w[3]; }

/* Window through portal `p` seen from the eye, intersected with `parent`. Returns 0 if the portal
 * cannot be seen at all. */
static int portal_window(const bsp_section *s, const bsp_portal *p, const float clip[4][4], float near_z,
                         const float *parent, float out[4])
{
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	int front = 0, behind = 0;
	uint32_t i;
	for (i = 0; i < p->vertex_count; i++) {
		float v[3], c[4];
		bsp_portal_vertex(s, p, i, v);
		project(clip, v, c);
		if (c[3] <= near_z) { behind++; continue; }
		front++;
		{
			float nx = c[0] / c[3], ny = c[1] / c[3];
			if (nx < x0) x0 = nx;
			if (nx > x1) x1 = nx;
			if (ny < y0) y0 = ny;
			if (ny > y1) y1 = ny;
		}
	}
	if (front == 0) return 0;                       /* entirely behind the camera */
	if (behind > 0) { x0 = y0 = -1.0f; x1 = y1 = 1.0f; }   /* crosses the near plane: do not tighten */
	out[0] = x0 > parent[0] ? x0 : parent[0];
	out[1] = y0 > parent[1] ? y0 : parent[1];
	out[2] = x1 < parent[2] ? x1 : parent[2];
	out[3] = y1 < parent[3] ? y1 : parent[3];
	return !win_empty(out);
}

static int win_contains(const float *outer, const float *inner)
{
	return inner[0] >= outer[0] && inner[1] >= outer[1] && inner[2] <= outer[2] && inner[3] <= outer[3];
}

/* Does the sphere (centre/radius) touch the NDC window? Conservative: NDC half-extent of the
 * sphere is estimated from the projection of its centre and the matrix scale. */
static int sphere_in_window(const float clip[4][4], float kx, float ky, const float c[3], float r, const float *w)
{
	float h[4], ex, ey;
	project(clip, c, h);
	if (h[3] <= 0.0f) return 1;                     /* centre behind or at the eye: the frustum test decides */
	if (h[3] <= r) return 1;                        /* the eye is inside the sphere */
	ex = r * kx / (h[3] - r);
	ey = r * ky / (h[3] - r);
	{
		float nx = h[0] / h[3], ny = h[1] / h[3];
		return !(nx + ex < w[0] || nx - ex > w[2] || ny + ey < w[1] || ny - ey > w[3]);
	}
}

static void add_cluster(vis_t *v, unsigned c)
{
	if (v->cluster_count < v->cap_clusters) v->clusters[v->cluster_count++] = (uint16_t)c;
}

void vis_run(vis_t *v, const bsp_section *s, const camera_t *cam, const frustum_t *fr, const float clip[4][4])
{
	uint32_t i, k, pushes = 0, sp = 0;
	float kx = sqrtf(clip[0][0] * clip[0][0] + clip[1][0] * clip[1][0] + clip[2][0] * clip[2][0]);
	float ky = sqrtf(clip[0][1] * clip[0][1] + clip[1][1] * clip[1][1] + clip[2][1] * clip[2][1]);
	int cam_cluster = bsp_find_cluster(s, cam->pos);
	float full[4] = { -1.0f, -1.0f, 1.0f, 1.0f };

	v->batch_count = v->cluster_count = 0;
	v->triangles = v->batches_tested = v->batches_culled = v->portals_tested = v->portals_passed = 0;
	v->sky_visible = 0;
	v->camera_cluster = cam_cluster;
	v->stamp++;
	if (v->stamp == 0) { memset(v->batch_stamp, 0, sizeof(uint32_t) * (s->batch_count + 1)); v->stamp = 1; }

	for (i = 0; i < s->cluster_count; i++) { float *w = win(v, i); w[0] = w[1] = 1.0f; w[2] = w[3] = -1.0f; }

	if (!v->force_all && cam_cluster >= 0 && (uint32_t)cam_cluster < s->cluster_count) {
		memcpy(win(v, (unsigned)cam_cluster), full, sizeof full);
		v->stack[sp++] = (uint16_t)cam_cluster;
		pushes = 1;
		while (sp) {
			unsigned c = v->stack[--sp];
			bsp_cluster cl = bsp_get_cluster(s, c);
			float parent[4];
			memcpy(parent, win(v, c), sizeof parent);
			for (k = 0; k < cl.portal_count; k++) {
				bsp_portal p = bsp_get_portal(s, bsp_cluster_portal(s, &cl, k));
				unsigned other;
				float side, w[4];
				if (p.cluster_a == c) other = p.cluster_b;
				else if (p.cluster_b == c) other = p.cluster_a;
				else continue;                             /* inconsistent data: ignore */
				if (other == BSP_NO_CLUSTER || other >= s->cluster_count) continue;
				v->portals_tested++;
				side = p.plane[0] * cam->pos[0] + p.plane[1] * cam->pos[1] + p.plane[2] * cam->pos[2] - p.plane[3];
				/* a is on the positive side: looking out of a needs the eye on the positive side. */
				if (p.cluster_a == c ? side < -EYE_EPS : side > EYE_EPS) continue;
				if (!portal_window(s, &p, clip, cam->near_z, parent, w)) continue;
				{
					float *cur = win(v, other);
					if (!win_empty(cur)) {
						if (win_contains(cur, w)) continue;          /* nothing new */
						if (w[0] > cur[0]) w[0] = cur[0];                  /* union */
						if (w[1] > cur[1]) w[1] = cur[1];
						if (w[2] < cur[2]) w[2] = cur[2];
						if (w[3] < cur[3]) w[3] = cur[3];
					}
					memcpy(cur, w, sizeof w);
				}
				v->portals_passed++;
				if (pushes < v->max_pushes) { v->stack[sp++] = (uint16_t)other; pushes++; }
			}
		}
		for (i = 0; i < s->cluster_count; i++) if (!win_empty(win(v, i))) add_cluster(v, i);
	} else {
		for (i = 0; i < s->cluster_count; i++) {
			bsp_cluster cl = bsp_get_cluster(s, i);
			if (frustum_aabb(fr, cl.bounds_min, cl.bounds_max)) {
				memcpy(win(v, i), full, sizeof full);
				add_cluster(v, i);
			}
		}
	}

	for (i = 0; i < v->cluster_count; i++) {
		unsigned c = v->clusters[i];
		bsp_cluster cl = bsp_get_cluster(s, c);
		const float *w = win(v, c);
		if (cl.flags & BSP_CLUSTER_SKY) v->sky_visible = 1;
		if (!frustum_aabb(fr, cl.bounds_min, cl.bounds_max)) continue;
		for (k = 0; k < cl.batch_count; k++) {
			uint32_t bi = bsp_cluster_batch(s, &cl, k);
			bsp_batch b;
			if (bi >= s->batch_count || v->batch_stamp[bi] == v->stamp) continue;
			v->batches_tested++;
			b = bsp_get_batch(s, bi);
			if (!frustum_sphere(fr, b.centre, b.radius) || !sphere_in_window(clip, kx, ky, b.centre, b.radius, w)) {
				v->batches_culled++;
				continue;
			}
			v->batch_stamp[bi] = v->stamp;
			v->batches[v->batch_count++] = bi;
			v->triangles += b.triangles;
		}
	}
}
