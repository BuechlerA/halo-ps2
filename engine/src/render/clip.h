/* Homogeneous-space triangle clipper for the EE (docs/03 §5.2, ADR-017). Pure C11, host-tested.
 *
 * The VU1 programs drop any triangle with a vertex in front of the near plane or beyond the GS
 * guard band (|x|, |y| > guard * w) instead of clipping it. In small rooms that is most of the
 * walls, so the renderer finds those triangles on the EE, clips them here against the same planes
 * (attributes interpolated linearly in clip space, i.e. perspective-correct) and draws the
 * result through PATH2. The two paths use the same planes, so together they cover every pixel. */
#ifndef RENDER_CLIP_H
#define RENDER_CLIP_H
#include <stdint.h>

#define CLIP_ATTRS 8
#define CLIP_MAX_VERTS 12
#define CLIP_GUARD 7.0f      /* |x|, |y| <= CLIP_GUARD * w: 7 * 256 px < the 2048 px the GS XY range allows */

/* Attributes: [0,1] texture s t (pre-scaled), [2,3] lightmap s t, [4..7] colour r g b a (0..255). */
typedef struct {
	float c[4];                /* clip-space x y z w (z is the constant near plane value: unused here) */
	float a[CLIP_ATTRS];
} cvert;

typedef struct {
	float near_z;              /* clip.w must be >= near_z */
	float zmax;                /* 0xFFFFFF */
	float sx, sy, ox, oy;      /* screen scale and offset: 256, 224, 2048, 2048 */
	float fog_k, fog_b;        /* F16 = clamp(w * k + b, 0, 4080); GS: 4080 = no fog (k = 0, b = 4080 disables) */
} clip_gs;

/* clip = m[0]*x + m[1]*y + m[2]*z + m[3] (camera_clip_matrix layout). */
void clip_project(const float m[4][4], const float pos[3], cvert *out);
/* 1 if any vertex is behind the near plane or outside the band (scaled by `margin` <= 1). */
int  clip_needs(const cvert v[3], float near_z, float margin);
/* Clip a triangle to w >= near_z and |x|,|y| <= CLIP_GUARD * w. Returns the polygon's vertex
 * count (0 = nothing visible); the polygon is convex, draw it as a fan. */
int  clip_polygon(const cvert in[3], float near_z, cvert out[CLIP_MAX_VERTS]);
/* GS packed data for one vertex: 12 words = ST+Q, RGBAQ, XYZF2 quadwords. pass 0 uses attributes
 * 0,1 as texture coordinates, pass 1 uses 2,3. Matches vu1/bsp.vsm bit for bit (ftoi4, F in bits 4..11). */
void clip_pack_vertex(const cvert *v, int pass, const clip_gs *g, uint32_t q[12]);

#endif
