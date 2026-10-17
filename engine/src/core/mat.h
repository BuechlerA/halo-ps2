/* 4x4 transforms in the same row layout as render/camera.h's clip matrix, pure C11 (host-tested):
 * a point p maps to  p' = x*m[0] + y*m[1] + z*m[2] + m[3]  (m[0..2] are the images of the basis
 * vectors, m[3] the translation; the last column of a pure transform is 0 0 0 1). Composition
 * mat4_mul(a, b) applies `a` first, then `b`. */
#ifndef CORE_MAT_H
#define CORE_MAT_H

typedef struct { float m[4][4]; } mat4;

void mat4_identity(mat4 *o);
/* Local transform of a skeleton node: uniform scale, then rotation by quaternion (x y z w), then translation. */
void mat4_from_trs(mat4 *o, const float q[4], const float t[3], float s);
/* Object placement: yaw about +z (counter-clockwise from +x), then pitch about the object's y axis
 * (positive pitch tips +x up), then roll about its x axis, then translate. Radians. */
void mat4_from_ypr(mat4 *o, float yaw, float pitch, float roll, const float t[3]);
void mat4_mul(mat4 *o, const mat4 *a, const mat4 *b);      /* o may alias a or b */
/* Inverse of a rotation (times uniform scale s: pass the matrix as is) plus translation: exact for what skeletons use. */
void mat4_rigid_inverse(mat4 *o, const mat4 *m);
void mat4_transform_point(const mat4 *m, const float p[3], float out[3]);
void mat4_transform_dir(const mat4 *m, const float d[3], float out[3]);   /* rotation/scale part only */
/* Direction `world_dir` expressed in the local frame of `m` (inverse of its rotation/scale part;
 * exact for rotation times uniform scale, which is all skeletons use). */
void mat4_world_dir_to_local(const mat4 *m, const float world_dir[3], float out[3]);

#endif
