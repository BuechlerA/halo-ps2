/* Skeleton pose -> node matrices, and the per-node VU1 palette (docs/03 §6). Pure C11, host-tested.
 * Works on plain arrays so it does not depend on the section readers. */
#ifndef SIM_SKELETON_H
#define SIM_SKELETON_H
#include <stdint.h>
#include "core/mat.h"

#define SKEL_MAX_NODES 64

typedef struct { float rot[4]; float trans[3]; float scale; } skel_xform;   /* same layout as anim_xform */

/* world[i] = local(i) * world[parent(i)] (parent < 0: root, multiplied by `root`). Parents need not precede
 * children in the array (resolved iteratively). Returns 0, or -1 for a cycle / out-of-range parent. */
int skel_world(mat4 *world, const skel_xform *local, const int16_t *parent, unsigned count, const mat4 *root);

/* VU1 palette entry for one node (docs: vu1/model.vsm): five quadwords =
 *   rows 0..3: world-space node matrix composed with the camera clip matrix (clip = x*r0 + y*r1 + z*r2 + r3),
 *   row 4:     the world-space light direction expressed in the node's local frame (xyz), w = 0. */
void skel_palette_entry(float out[5][4], const mat4 *node_world, const float clip[4][4], const float light_world[3]);

#endif
