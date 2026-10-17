/* COLn section reader: the collision BSP of one structure_bsp (docs/15-coll-section.md). Contract between
 * tools/halo2ps2/collconv.py (writer), world/coll.c (reader, host-tested, pure C11, no heap, no double, no
 * recursion) and the player controller / projectile code.
 *
 * Units: world units (1 wu = 10 ft), the BSP's own space. Distances are float; every query is bounded (explicit
 * stacks, node-visit budgets), so corrupt data can give wrong answers but never a crash or an endless loop.
 *
 * Semantics (Halo's collision BSP, see the doc): the 3D tree splits space into convex leaves; a child of `none`
 * is solid space (and everything outside the world). Surfaces lie on the boundary planes of the leaves that
 * reference them; they face the empty side (front) and are hit from the front only. A "two-sided" surface is a pair
 * of coincident surfaces of opposite orientation (both flagged COLL_F_TWO_SIDED, each listed by its own front
 * leaf), so it is hit from either side without any special case here. The 2D BSP of a (leaf, plane) reference
 * names the surface of that plane at a point; a ray hit takes that surface when the crossing lies in its polygon
 * (the 2D BSP has no empty leaves). Sphere sweeps and depenetration use the surface polygons directly (convex on
 * every retail map: the converter refuses a concave one). */
#ifndef WORLD_COLL_H
#define WORLD_COLL_H
#include <stddef.h>
#include <stdint.h>

#define COLL_VERSION 1u
#define COLL_STACK_3D 128u     /* explicit stack of the tree walks (converter checks depth <= this; max on retail 66) */
#define COLL_STACK_2D 64u      /* stack of the 2D subtree walks (retail max 8) */
#define COLL_MAX_POLY 15u      /* vertices per surface polygon (retail max 8) */

typedef enum { COLL_OK = 0, COLL_ERR_TRUNCATED, COLL_ERR_MAGIC, COLL_ERR_LAYOUT, COLL_ERR_VERSION } coll_status;

/* surface flags (coll_hit.flags, and the `ignore_flags` of coll_ray) */
#define COLL_F_TWO_SIDED 1u    /* one of a coincident pair of opposite surfaces: collides from both sides */
#define COLL_F_INVISIBLE 2u    /* collision only, not drawn */
#define COLL_F_CLIMBABLE 4u    /* ladder / climbable surface */
#define COLL_F_BREAKABLE 8u    /* breakable (glass): coll_hit.breakable is its group id */
#define COLL_NO_MATERIAL 0xFFFFu

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       node_count, plane_count, leaf_count, ref_count, node2d_count, surface_count, index_count, vertex_count;
	uint32_t       depth;                    /* deepest 3D path (nodes), as measured by the converter */
	float          bounds[6];                /* min xyz, max xyz of the surface vertices */
	const uint8_t *nodes, *planes, *leaves, *refs, *nodes2d, *surfaces, *indices, *vertices;
} coll_bsp;

typedef struct {
	float    t;                /* 0..1 fraction along the move (sphere: where the sphere first touches) */
	float    normal[3];        /* unit normal at the hit, facing the mover (normal . move <= 0) */
	float    point[3];         /* contact point on the surface */
	uint16_t material;         /* globals material type (0 dirt, 1 sand, 2 stone, ... see the doc), COLL_NO_MATERIAL none */
	uint16_t flags;            /* COLL_F_* */
	int32_t  surface;          /* surface index of this section */
	uint16_t breakable;        /* breakable group id (valid when flags has COLL_F_BREAKABLE) */
	uint16_t pad;
} coll_hit;

/* Validates everything (sizes, alignment, ranges, tree order, finite floats) once; the queries never re-check. The
 * buffer must stay alive and unchanged; it needs no alignment (give 16 and define COLL_ALIGNED_LOADS to use plain
 * loads). `out` is zeroed on failure. */
coll_status coll_open(const uint8_t *data, size_t size, coll_bsp *out);

/* Segment a -> b. Returns 1 and fills *h for the nearest surface hit, 0 otherwise (*h untouched). Surfaces whose flags
 * intersect `ignore_flags` are skipped (the ray goes on). Only a crossing from the front of a surface to its back is a
 * hit, so a ray that starts inside solid space hits nothing until it has left it and meets another surface. */
int coll_ray(const coll_bsp *b, const float a[3], const float b_[3], uint32_t ignore_flags, coll_hit *h);

/* Sphere of `radius` whose centre moves a -> b. Returns 1 and fills *h at the first contact (face, edge or vertex of a
 * surface polygon), t = exact contact fraction: the integrator should stop slightly short (about 1e-3 wu). A sphere
 * that already touches or overlaps a surface and moves into it reports t = 0 (use coll_sphere_overlap to push it
 * out first). radius <= 0 falls back to coll_ray. */
int coll_sphere_sweep(const coll_bsp *b, const float a[3], const float b_[3], float radius, coll_hit *h);
int coll_sphere_sweep_flags(const coll_bsp *b, const float a[3], const float b_[3], float radius, uint32_t ignore_flags,
                            coll_hit *h);

/* 1 when p lies in solid space (a `none` child of the tree, or outside the world). Ties go to the front child. */
int coll_point_in_solid(const coll_bsp *b, const float p[3]);

/* Sphere (centre c, radius r) overlapping surfaces: returns 1 and the displacement `push_out` to add to c so that
 * it no longer overlaps (two passes over the surfaces within reach, so a corner resolves to a diagonal push), or 0
 * with push_out = 0. A centre that sits behind a one-sided face (in solid space, up to r behind it) is pushed out
 * through the face; between the two halves of a two-sided pair each half pushes to its own side. */
int coll_sphere_overlap(const coll_bsp *b, const float c[3], float r, float push_out[3]);

/* Helpers (also what the host tests use as their brute-force reference). */
int32_t  coll_locate_leaf(const coll_bsp *b, const float p[3]);              /* leaf index, -1 solid */
uint32_t coll_surface_polygon(const coll_bsp *b, int32_t surface, float out[][3]);   /* up to COLL_MAX_POLY; 0 invalid */
/* effective plane (n.p = d, n faces the front), flags, material, breakable id; 0 on a bad index */
int      coll_surface_info(const coll_bsp *b, int32_t surface, float plane[4], uint16_t *material, uint16_t *flags,
                           uint16_t *breakable);
/* Number of (plane) references of a leaf: for statistics only. */
uint32_t coll_leaf_ref_count(const coll_bsp *b, uint32_t leaf);

#endif
