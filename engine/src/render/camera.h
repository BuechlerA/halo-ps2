/* Free-fly camera, projection and frustum. Pure C11 (host-tested).
 *
 * World space is Halo's: right-handed, +z up, yaw counter-clockwise from +x seen from above.
 * Camera space: +x right, +y up, +z forward, so view-space z is also clip w.
 *
 * Projection is *reverse-Z*: clip.z = near (constant), clip.w = view z, hence the post-divide
 * depth is near/z in (0, 1], larger = nearer, which is what the GS GEQUAL test and Z24 want and
 * keeps precision at distance (docs/03 §5, ADR-017). Screen scale is applied by the VU1 programs. */
#ifndef RENDER_CAMERA_H
#define RENDER_CAMERA_H

#define FRUSTUM_MAX_PLANES 16
#define CAMERA_NEAR_DEFAULT 0.05f

typedef struct {
	float pos[3];
	float yaw, pitch;          /* radians; pitch clamped to +-89 degrees by camera_look */
	float roll;                /* radians, positive banks right (cinematic cameras; 0 for the player) */
	float hfov;                /* horizontal field of view, radians (Halo default 70 degrees) */
	float aspect;              /* display aspect, width/height (4/3: the buffer is 512x448 anamorphic) */
	float near_z, far_z;
} camera_t;

typedef struct {
	int   count;
	float plane[FRUSTUM_MAX_PLANES][4];   /* nx ny nz d: inside when n.p + d >= 0 */
} frustum_t;

void camera_init(camera_t *c);
void camera_look(camera_t *c, float dyaw, float dpitch);
/* Move relative to the view: forward/strafe along the horizontal-agnostic view axes, rise along world z. */
void camera_move(camera_t *c, float forward, float strafe, float rise);
void camera_basis(const camera_t *c, float right[3], float up[3], float fwd[3]);

/* Row i of `m` is what input component i (x, y, z, 1 for i = 3) contributes to clip xyzw, i.e.
 * clip = m[0]*x + m[1]*y + m[2]*z + m[3]. This is the layout the VU1 programs consume
 * (four consecutive quadwords). The y axis is flipped: GS screen y points down. */
void camera_clip_matrix(const camera_t *c, float m[4][4]);

/* View frustum with 6 planes (left, right, bottom, top, near, far); returns plane count. */
void camera_frustum(const camera_t *c, frustum_t *f);

/* 0 = completely outside, 1 = inside or intersecting. */
int frustum_sphere(const frustum_t *f, const float centre[3], float radius);
/* Same for an axis-aligned box (min xyz, max xyz). */
int frustum_aabb(const frustum_t *f, const float bmin[3], const float bmax[3]);

#endif
