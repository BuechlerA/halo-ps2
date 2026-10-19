#include "render/clip.h"
#include <string.h>

void clip_project(const float m[4][4], const float p[3], cvert *out)
{
	int k;
	for (k = 0; k < 4; k++) out->c[k] = m[0][k] * p[0] + m[1][k] * p[1] + m[2][k] * p[2] + m[3][k];
}

int clip_needs(const cvert v[3], float near_z, float margin)
{
	float g = CLIP_GUARD * margin;
	int i;
	for (i = 0; i < 3; i++) {
		float w = v[i].c[3];
		if (w < near_z || v[i].c[0] > g * w || v[i].c[0] < -g * w || v[i].c[1] > g * w || v[i].c[1] < -g * w) return 1;
	}
	return 0;
}

int clip_outside(const cvert v[3], float near_z)
{
	float g = CLIP_GUARD;
	float w0 = v[0].c[3], w1 = v[1].c[3], w2 = v[2].c[3];
	if (w0 < near_z && w1 < near_z && w2 < near_z) return 1;
	if (v[0].c[0] > g * w0 && v[1].c[0] > g * w1 && v[2].c[0] > g * w2) return 1;
	if (v[0].c[0] < -g * w0 && v[1].c[0] < -g * w1 && v[2].c[0] < -g * w2) return 1;
	if (v[0].c[1] > g * w0 && v[1].c[1] > g * w1 && v[2].c[1] > g * w2) return 1;
	if (v[0].c[1] < -g * w0 && v[1].c[1] < -g * w1 && v[2].c[1] < -g * w2) return 1;
	return 0;
}

/* Signed distance to clip plane `p` (>= 0 is inside). */
static float dist(const cvert *v, int p, float near_z)
{
	switch (p) {
	case 0: return v->c[3] - near_z;
	case 1: return CLIP_GUARD * v->c[3] - v->c[0];
	case 2: return CLIP_GUARD * v->c[3] + v->c[0];
	case 3: return CLIP_GUARD * v->c[3] - v->c[1];
	default: return CLIP_GUARD * v->c[3] + v->c[1];
	}
}

static void lerp_vert(const cvert *a, const cvert *b, float t, cvert *o)
{
	int k;
	for (k = 0; k < 4; k++) o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
	for (k = 0; k < CLIP_ATTRS; k++) o->a[k] = a->a[k] + (b->a[k] - a->a[k]) * t;
}

int clip_polygon(const cvert in[3], float near_z, cvert out[CLIP_MAX_VERTS])
{
	cvert buf[2][CLIP_MAX_VERTS];
	int cur = 0, n = 3, p, i;
	unsigned any = 0, all = 0x1F;
	/* Outcodes: bit p set = vertex outside plane p. Wholly outside one plane: nothing to draw;
	 * only planes some vertex is outside of need a clipping pass. */
	for (i = 0; i < 3; i++) {
		unsigned c = 0;
		for (p = 0; p < 5; p++) if (dist(&in[i], p, near_z) < 0.0f) c |= 1u << p;
		any |= c;
		all &= c;
	}
	if (all) return 0;
	if (!any) { memcpy(out, in, 3 * sizeof(cvert)); return 3; }
	memcpy(buf[0], in, 3 * sizeof(cvert));
	for (p = 0; p < 5; p++) {
		int m = 0, nxt = cur ^ 1;
		if (!(any & (1u << p))) continue;
		for (i = 0; i < n; i++) {
			const cvert *a = &buf[cur][i], *b = &buf[cur][(i + 1) % n];
			float da = dist(a, p, near_z), db = dist(b, p, near_z);
			if (da >= 0.0f && m < CLIP_MAX_VERTS) buf[nxt][m++] = *a;
			if ((da >= 0.0f) != (db >= 0.0f) && m < CLIP_MAX_VERTS) {
				lerp_vert(a, b, da / (da - db), &buf[nxt][m]);
				m++;
			}
		}
		cur = nxt;
		n = m;
		if (n < 3) return 0;
	}
	memcpy(out, buf[cur], (size_t)n * sizeof(cvert));
	return n;
}

static uint32_t f32bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* float -> int like the VU1's FTOI4 (x16, truncate toward zero, saturate). */
static int32_t ftoi4(float v)
{
	v *= 16.0f;
	if (v >= 2147483520.0f) return 2147483647;
	if (v <= -2147483648.0f) return (-2147483647 - 1);
	return (int32_t)v;
}

static int32_t ftoi0(float v) { return (int32_t)v; }

void clip_pack_vertex(const cvert *v, int pass, const clip_gs *g, uint32_t q[12])
{
	float w = v->c[3], inv = 1.0f / w;
	float x = g->ox + g->sx * (v->c[0] * inv), y = g->oy + g->sy * (v->c[1] * inv), z = g->zmax * (v->c[2] * inv);
	float f = w * g->fog_k + g->fog_b;
	const float *st = &v->a[pass ? 2 : 0];
	if (f < 0.0f) f = 0.0f;
	if (f > 4080.0f) f = 4080.0f;
	q[0] = f32bits(st[0] * inv); q[1] = f32bits(st[1] * inv); q[2] = f32bits(inv); q[3] = 0;
	q[4] = (uint32_t)ftoi0(v->a[4]); q[5] = (uint32_t)ftoi0(v->a[5]); q[6] = (uint32_t)ftoi0(v->a[6]); q[7] = (uint32_t)ftoi0(v->a[7]);
	q[8] = (uint32_t)ftoi4(x) & 0xFFFFu; q[9] = (uint32_t)ftoi4(y) & 0xFFFFu; q[10] = (uint32_t)ftoi4(z); q[11] = (uint32_t)ftoi0(f);
}
