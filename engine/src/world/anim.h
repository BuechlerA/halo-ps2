/* ANIM section reader (docs/12-anim-section.md): animation graphs converted from Xbox
 * `model_animations` tags, quantised for the EE. Contract between tools/halo2ps2/animconv.py (writer),
 * world/anim.c (reader + sampler, host-tested, pure C11, no heap, no double) and the engine's object
 * animation player.
 *
 * Version 2 carries the graph's node NAMES: a model's node is driven by the graph node of the same name
 * (sim/objects.c builds that map), because a graph may order its nodes differently from a model it animates
 * (the first-person weapon graphs interleave hand and gun nodes breadth-first). Graphs whose node list matches
 * the model's in order (every biped and its own graph) map 1:1.
 * Animation time is in frames at 30 Hz (the original tick rate, R3). */
#ifndef WORLD_ANIM_H
#define WORLD_ANIM_H
#include <stddef.h>
#include <stdint.h>

#define ANIM_MAX_NODES 64
#define ANIM_NAME_LEN 48
#define ANIM_NODE_NAME_LEN 32

typedef enum { ANIM_OK = 0, ANIM_ERR_TRUNCATED, ANIM_ERR_MAGIC, ANIM_ERR_LAYOUT, ANIM_ERR_VERSION } anim_status;

/* flags */
#define ANIM_LOOPS 1u                 /* wraps at frame_count (otherwise clamps at the last frame) */
#define ANIM_WORLD_RELATIVE 2u        /* root motion is world space (vehicles); informational */

typedef struct { float rot[4]; float trans[3]; float scale; } anim_xform;   /* rot: quaternion x y z w */
typedef struct { anim_xform node[ANIM_MAX_NODES]; uint16_t count; } anim_pose;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       graph_count, anim_count;
	const uint8_t *graphs, *anims, *pool;
} anim_section;

typedef struct {
	uint32_t tag_id;                   /* Xbox tag id of the `model_animations` tag */
	uint16_t node_count, anim_count;
	uint32_t first_anim;               /* into the section's animation table */
	uint32_t names_off;                /* pool offset of node_count x ANIM_NODE_NAME_LEN names */
} anim_graph;

typedef struct {
	char     name[ANIM_NAME_LEN];      /* e.g. "stand unarmed idle" */
	uint16_t type, frame_count;
	uint16_t loop_frame, flags;        /* ANIM_* */
	uint16_t node_count;
	float    fps;                      /* playback rate in frames per second (30 unless the tag says otherwise) */
	uint32_t bytes;                    /* size of the sampled data in the pool (statistics) */
	/* ADDED by animconv (docs/12-anim-section.md, section "Header additions"): */
	uint16_t root_motion;              /* 0 none, 1 dx dy, 2 dx dy dyaw, 3 dx dy dz dyaw (see anim_root_motion) */
} anim_info;

anim_status anim_open(anim_section *s, const uint8_t *data, size_t size);
int         anim_find_graph(const anim_section *s, uint32_t tag_id);       /* binary search, -1 if absent */
anim_graph  anim_get_graph(const anim_section *s, uint32_t index);
anim_info   anim_get(const anim_section *s, const anim_graph *g, uint32_t k);
/* Name of graph node i (NUL-terminated, at most ANIM_NODE_NAME_LEN - 1 characters; "" out of range or unnamed). */
const char *anim_node_name(const anim_section *s, const anim_graph *g, uint32_t i);
/* Index of the graph node called `name`, or -1. */
int         anim_find_node(const anim_section *s, const anim_graph *g, const char *name);
/* First animation whose name contains `token` (case-sensitive), or -1. Prefer exact matches. */
int         anim_find_name(const anim_section *s, const anim_graph *g, const char *token);
/* Sample animation k at fractional frame `frame` (loops or clamps per its flags) into `out`: all
 * g->node_count nodes get a transform (nodes the animation does not drive get the graph's default). */
void        anim_sample(const anim_section *s, const anim_graph *g, uint32_t k, float frame, anim_pose *out);
/* out = lerp(a, b, t) with normalised quaternion interpolation; a, b, out may alias. */
void        anim_blend(const anim_pose *a, const anim_pose *b, float t, anim_pose *out);

/* ADDED by animconv: root motion (the tag's "frame info"). out = { dx, dy, dz, dyaw } of the given frame:
 * the displacement the animation makes during that frame, in the object's frame, world units and radians
 * (a walk cycle's dx sums to one stride, a 90 degree turn's dyaw to 1.5708). Components the animation does
 * not have, and animations without frame info (anim_info.root_motion == 0), give 0. Frames past the end clamp. */
void        anim_root_motion(const anim_section *s, const anim_graph *g, uint32_t k, uint32_t frame, float out[4]);

#endif
