/* PATn section reader and navigation queries (docs/26-path-section.md). Contract between tools/halo2ps2/pathconv.py (writer),
 * ai/path.c (reader, host-tested, pure C11, no heap, no double, no recursion) and the AI runtime.
 *
 * What the section is: the navigation mesh of one structure_bsp, derived by the converter from the BSP's "pathfinding
 * surfaces" (the collision surfaces flagged walkable: normal z >= 0.71). A *sector* is a convex polygon (3..15 vertices,
 * counter-clockwise seen from above, planar to 2 cm) lying on those floors; a *link* leads from a sector to a neighbour:
 *   WALK   shared edge run (the portal is the run),
 *   STEP   across a riser of at most `step_max` (0.2 wu): walk it, the physics steps up,
 *   VAULT  upward over a riser of at most `vault_max` (0.55 wu): needs a jump / vault action,
 *   DROP   downward off a ledge of at most `drop_max` (3 wu): falls (one way),
 *   CLIMB  between the foot and the top of a ladder volume (both ways),
 * with a cost (centroid -> portal -> centroid plus a per-kind penalty; the straight centroid distance never exceeds it, so
 * the heuristic of the A* is consistent) and a height change. The data has no hint blocks: jump / climb / vault links are
 * derived from geometry (see the doc). Sector ids are the converter's order; every id fits u16 (0xFFFF = none).
 *
 * Units: world units (1 wu = 10 ft), the BSP's own space. Costs are integers in 1/`cost_scale` wu.
 *
 * Every query is bounded in time (explicit heaps / stacks, expansion budgets), never recurses and never allocates: the
 * caller owns a `path_scratch` (a few bytes per sector, see path_scratch_bytes). path_open validates every table once, so the
 * queries never check an index again; corrupt data gives wrong answers, never a crash or an endless loop. */
#ifndef AI_PATH_H
#define AI_PATH_H
#include <stddef.h>
#include <stdint.h>

#define PATH_VERSION 1u
#define PATH_MAX_POLY 15u            /* vertices per sector (converter limit 12) */
#define PATH_NONE 0xFFFFu            /* "no sector" in the 16-bit fields */
#define PATH_ROUTE_MAX 1024u         /* sectors of one route (longer ones are cut and flagged) */
#define PATH_MAX_SECTORS 0xFFFEu

typedef enum { PATH_OK = 0, PATH_ERR_TRUNCATED, PATH_ERR_MAGIC, PATH_ERR_LAYOUT, PATH_ERR_VERSION } path_status;

/* link kinds (bit k of a kind mask) */
enum { PATH_LINK_WALK = 0, PATH_LINK_STEP, PATH_LINK_VAULT, PATH_LINK_DROP, PATH_LINK_CLIMB, PATH_LINK_JUMP, PATH_LINK_KINDS };
#define PATH_KIND(k) (1u << (k))
#define PATH_KINDS_WALKING (PATH_KIND(PATH_LINK_WALK) | PATH_KIND(PATH_LINK_STEP))
#define PATH_KINDS_SYMMETRIC (PATH_KINDS_WALKING | PATH_KIND(PATH_LINK_CLIMB))     /* every link has its reverse */
#define PATH_KINDS_ALL ((1u << PATH_LINK_KINDS) - 1u)

/* sector flags */
#define PATH_SF_BREAKABLE 1u         /* breakable floor (glass): the host may block it */
#define PATH_SF_INVISIBLE 2u         /* a collision-only floor */
#define PATH_SF_CLIMBABLE 4u         /* floor part of a climbable (ladder) volume */
#define PATH_SF_TWO_SIDED 8u

#define PATH_HEADROOM_SCALE 32.0f    /* sector headroom is stored in 1/32 wu; 255 means "nothing within 7.97 wu" */
#define PATH_LINK_WIDTH_SCALE 16.0f          /* link width units per wu (byte 3 of the link record) */
#define PATH_THICK_SCALE 64.0f               /* sector thickness is stored in 1/64 wu; 255 means 3.98 wu or more */
#define PATH_NO_SURFACE 0xFFFFu
#define PATH_NO_MATERIAL 255u

/* defaults of path_locate */
#define PATH_LOCATE_BELOW 0.30f      /* a point this far under a floor still stands on it (feet sunk into the skin) */
#define PATH_LOCATE_ABOVE 4.00f      /* a point up to this far above a floor (jumping, falling) still belongs to it */

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       sector_count, point_count, index_count, link_count, cellstart_count, item_count, comp_count;
	float          bounds[6];                /* min xyz, max xyz of the sector vertices */
	float          cell, gx0, gy0;           /* point-location grid: cell size, origin */
	uint32_t       nx, ny;
	float          step_max, vault_max, drop_max;   /* thresholds the converter classified links with (wu) */
	uint32_t       cost_scale;               /* cost units per wu */
	const uint8_t *sectors, *points, *indices, *links, *cellstart, *cellitems;
} path_bsp;

typedef struct {
	float    centroid[3];
	uint16_t comp;                           /* connected component over WALK / STEP / CLIMB links */
	uint16_t surface;                        /* COLn surface index of the largest source surface (PATH_NO_SURFACE none) */
	uint8_t  flags, material;                /* PATH_SF_*, globals material type (PATH_NO_MATERIAL none) */
	float    headroom;                       /* free height above the floor (wu): the nearest overhead surface, 7.97 = unobstructed */
	float    thickness;                      /* the smallest extent of the polygon across any of its edges (wu; 3.98 = that or more) */
	uint8_t  nverts;
	uint16_t link_count;
	uint32_t first_link;
} path_sector_info;

typedef struct {
	uint16_t to;
	uint8_t  kind;                           /* PATH_LINK_* */
	float    width;                          /* plan length of the portal (wu, 1/16 resolution, at most 15.9); 255/16 = unlimited (ladders) */
	float    right[3], left[3];              /* portal ends on the source sector, as seen walking from source to `to` */
	float    dz;                             /* destination minus source height at the portal (wu) */
	uint32_t cost;                           /* in 1/cost_scale wu */
} path_link;

/* Validates everything once (size, alignment, ranges, finite floats, grid, links); `out` is zeroed on failure. The buffer must
 * stay alive and unchanged; it needs no alignment. */
path_status path_open(const uint8_t *data, size_t size, path_bsp *out);

/* ---- sectors ------------------------------------------------------------------------------------------------------- */
int      path_sector_get(const path_bsp *b, uint32_t sector, path_sector_info *out);          /* 0 on a bad index */
uint32_t path_sector_polygon(const path_bsp *b, uint32_t sector, float out[][3]);              /* up to PATH_MAX_POLY; 0 bad */
/* height of the sector's plane at the plan position (x, y) (extrapolated outside the polygon) */
float    path_sector_height(const path_bsp *b, uint32_t sector, float x, float y);

/* ---- locating ------------------------------------------------------------------------------------------------------ */
/* The sector under `pos`: among the sectors whose polygon contains (x, y) in plan, whose floor is at most `below` above
 * pos[2] and at most `above` under it, the one with the highest floor. -1 when none; *floor_z (optional) gets the height. */
int32_t  path_locate_ex(const path_bsp *b, const float pos[3], float below, float above, float *floor_z);
int32_t  path_locate(const path_bsp *b, const float pos[3]);       /* PATH_LOCATE_BELOW / PATH_LOCATE_ABOVE */
/* Nearest sector (distance from `pos` to the polygon: plan distance, then the vertical gap to the plane) within max_dist;
 * -1 when none. *dist and point[3] (the closest point on the sector) are optional. */
int32_t  path_nearest_sector(const path_bsp *b, const float pos[3], float max_dist, float *dist, float point[3]);
/* A point on `sector` near `pos` that keeps `margin` (a capsule radius) from every edge when the sector is wide enough:
 * returns 1; a narrower sector yields its centroid and returns 0. The point is on the sector plane. */
int      path_snap(const path_bsp *b, uint32_t sector, const float pos[3], float margin, float out[3]);

/* ---- links and hints ----------------------------------------------------------------------------------------------- */
uint32_t path_link_count(const path_bsp *b, uint32_t sector);
int      path_link_get(const path_bsp *b, uint32_t sector, uint32_t index, path_link *out);   /* 0 on a bad index */
/* Outgoing links of `sector` whose kind is in `kind_mask` (use ~(WALK|STEP) for the jump / climb / vault / drop hints a route may
 * take); returns the number written (at most max). */
uint32_t path_sector_hints(const path_bsp *b, uint32_t sector, uint32_t kind_mask, path_link *out, uint32_t max);

/* ---- queries ------------------------------------------------------------------------------------------------------- */
/* Per-query caller memory: arrays of `cap` sectors carved from a buffer, plus the route buffers. */
typedef struct {
	uint32_t  cap, stamp, heap_n;
	uint32_t *g, *f, *seen, *plink;
	uint16_t *parent, *hpos, *heap;
	uint32_t  route_links;                   /* links of the last route (see path_route_sectors) */
	uint16_t  route_sector[PATH_ROUTE_MAX + 1];
	uint32_t  route_link[PATH_ROUTE_MAX];    /* absolute link indices */
	float     pl[PATH_ROUTE_MAX + 2][3];     /* funnel portals (left, right ends) */
	float     pr[PATH_ROUTE_MAX + 2][3];
	float     shr[PATH_ROUTE_MAX + 2][2];    /* per portal: how far its right / left end was pulled in */
	float     corner[PATH_ROUTE_MAX + 4][8]; /* per waypoint: the unshrunk portal end it wraps, the direction into the portal, first and last slide */
	uint8_t   corner_valid[PATH_ROUTE_MAX + 4];
} path_scratch;

size_t path_scratch_bytes(uint32_t sector_count);                                   /* memory the arrays need */
int    path_scratch_init(path_scratch *s, void *mem, size_t bytes, uint32_t sector_count);   /* mem 4-aligned; 0 if too small */

typedef struct {
	float          radius;                   /* capsule radius: portal ends are pulled in by it (default 0.2) */
	float          height;                   /* capsule height: sectors with less headroom are not entered (default 0.5) */
	                                         /* (links with a portal narrower than 2 * radius are not used either) */
	uint32_t       kinds;                    /* PATH_KIND(k) mask of the link kinds a route may use (default WALK | STEP) */
	float          max_drop;                 /* DROP links further down than this are not used (default 1.5) */
	const uint32_t *blocked;                 /* optional bit set over sector ids (bit s: sector s is impassable), or NULL */
	uint32_t       max_expand;               /* sector expansions the search may spend; 0 = 8 * sector_count + 64 */
	float          snap_dist;                /* start / goal off the mesh snap to a sector within this distance (default 8) */
} path_query;

void path_query_default(path_query *q);

/* helpers for the blocked set: words needed for a BSP, and set / clear one sector (a door, a vehicle's footprint) */
static inline uint32_t path_block_words(uint32_t sector_count) { return (sector_count + 31u) >> 5; }
static inline void path_block_set(uint32_t *mask, uint32_t sector, int blocked)
{
	if (blocked) mask[sector >> 5] |= 1u << (sector & 31u); else mask[sector >> 5] &= ~(1u << (sector & 31u));
}

typedef enum { PATH_ROUTE_COMPLETE = 0, PATH_ROUTE_PARTIAL, PATH_ROUTE_NO_START, PATH_ROUTE_NO_GOAL } path_route_status;

typedef struct {
	path_route_status status;                /* COMPLETE: reaches the goal sector; PARTIAL: best route to the closest sector reached */
	uint32_t          cost;                  /* sector-graph cost of the sector route (cost units; 0 when no route) */
	uint32_t          expanded;              /* sectors expanded by the search */
	uint32_t          sector_count;          /* sectors on the route (scratch->route_sector[0..sector_count)) */
	int32_t           start_sector, goal_sector, end_sector;   /* located / snapped / last sector of the route */
	uint8_t           truncated;             /* the route was cut at PATH_ROUTE_MAX sectors or the waypoint buffer */
	uint8_t           pad[3];
} path_route_info;

/* Sector-level search: A* over the links (integer costs, consistent heuristic; optimal), honouring q->kinds, q->max_drop,
 * q->blocked. Fills s->route_sector / route_link and *info (cost, counts). With the goal unreachable it returns the route
 * to the expanded sector closest to the goal (PATH_ROUTE_PARTIAL). Returns 1 when the goal sector was reached. */
int path_find(const path_bsp *b, path_scratch *s, const path_query *q, uint32_t from, uint32_t to, path_route_info *info);

/* O(1) when `q` allows only symmetric kinds and has no blocked set (component ids); otherwise a bounded search. */
int path_sector_reachable(const path_bsp *b, path_scratch *s, const path_query *q, uint32_t from, uint32_t to);
int path_reachable(const path_bsp *b, path_scratch *s, const path_query *q, const float from[3], const float to[3]);

#define PATH_WP_PLAIN 0u
typedef struct {
	float    pos[3];
	uint16_t sector;                         /* the sector the point lies on */
	uint8_t  kind;                           /* PATH_LINK_* of the action to perform from this point to the next one; WALK = none */
	uint8_t  flags;                          /* PATH_WPF_* */
} path_waypoint;
#define PATH_WPF_LANDING 1u                  /* the point where a vault / drop / climb ends */
#define PATH_WPF_GOAL 2u                     /* the last point (goal or best partial point) */

/* Route from `from` to `to`: locates both (path_locate, else the nearest sector within snap_dist), runs path_find and pulls the
 * sector route tight (funnel over the portals, ends pulled in by q->radius). Walk and step links are smoothed across; a
 * vault / drop / climb link ends a smoothing run: the takeoff point (kind set), then the landing point (PATH_WPF_LANDING) follow.
 * Waypoints exclude the start; the last one has PATH_WPF_GOAL (the goal projected on its sector, or for a partial route the
 * closest point of the end sector to `to`). Returns the number written (<= max); *info (optional) tells complete / partial. */
int path_route(const path_bsp *b, path_scratch *s, const path_query *q, const float from[3], const float to[3],
               path_waypoint *out, int max, path_route_info *info);

#endif
