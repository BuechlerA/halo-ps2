/* CINE section reader (docs/28-cine-section.md): the cinematic data of a scenario converted by tools/halo2ps2/cinconv.py.
 *
 *   - recorded animations: Halo's `recording_play` data. NOT positions: the CONTROL INPUT of a unit (aim yaw / pitch,
 *     throttle, control flags, weapon index) as a delta coded event stream, played back through the unit's own physics.
 *     The stream is kept in its original byte coding (verified on all 349 recordings of the ten campaign maps); the reader
 *     validates every stream once, `cinedef_next_event` / `cinedef_apply_event` decode it, sim/cine.c plays it.
 *   - camera tracks (`camera_track` tags): control points of the path a unit's chase camera follows with the look pitch;
 *   - cutscene titles (box, style, colours, fade times in ticks) and the scenario's ingame help strings (title texts);
 *   - custom animation graph tag ids and ai animation references named by the scenario.
 *
 * Pure C11, no heap, no double. cinedef_open validates every table once (ranges, alignment, sort order, indices, string
 * terminators, finite floats, every event stream down to its tick count), so the accessors can never read outside the
 * buffer whatever the bytes. Records are decoded with byte loads: the section buffer needs no alignment.
 *
 * Units: time is in ticks of 1/30 s; aim angles are in "counts" of pi / 1000 radians (2000 per turn), yaw in
 * [-1000, 1000] at the start of a recording and [-1000, 1000) after any aim event, pitch in [-500, 500] (positive = up); positions in world units; rectangles are pixels of the 640 x 480
 * virtual screen of the HUD (huddef.h); colours are RGBA words, R in the low byte, GS scale (0x80 = 1.0), as in huddef.h;
 * quaternions are x y z w in the engine's convention (the conjugate of what the Xbox stores, as in ANIM). Tag ids are the
 * Xbox tag ids, as everywhere else. */
#ifndef WORLD_CINEDEF_H
#define WORLD_CINEDEF_H
#include <stddef.h>
#include <stdint.h>

#define CINEDEF_NAME_LEN 32
#define CINEDEF_TRACK_NAME_LEN 16
#define CINEDEF_NONE16 0xFFFFu
#define CINEDEF_YAW_HALF 1000            /* yaw counts: [-1000, 1000) */
#define CINEDEF_PITCH_LIMIT 500          /* pitch counts: [-500, 500] */
#define CINEDEF_MAX_LENGTH (1u << 20)    /* longest accepted recording, ticks (the longest real one is 3524) */
#define CINEDEF_MAX_EVENTS (1u << 22)

typedef enum { CINEDEF_OK = 0, CINEDEF_ERR_TRUNCATED, CINEDEF_ERR_MAGIC, CINEDEF_ERR_LAYOUT, CINEDEF_ERR_VERSION } cinedef_status;

/* Control flags of a recorded unit, HEK `unit_control_flags` bit order. Observed in the data: 0x001 0x002 0x040 0x100 0x200
 * 0x400 0x800 0x2000 and sums of them; the NAMES are unverified. */
#define CINEDEF_CF_CROUCH 0x0001u
#define CINEDEF_CF_JUMP 0x0002u
#define CINEDEF_CF_ACTION 0x0040u
#define CINEDEF_CF_MELEE 0x0080u
#define CINEDEF_CF_LOOK_DONT_TURN 0x0100u
#define CINEDEF_CF_FORCE_ALERT 0x0200u
#define CINEDEF_CF_RELOAD 0x0400u
#define CINEDEF_CF_PRIMARY_FIRE 0x0800u
#define CINEDEF_CF_SECONDARY_FIRE 0x1000u
#define CINEDEF_CF_GRENADE 0x2000u
#define CINEDEF_CF_EXCHANGE_WEAPON 0x4000u

/* event types (op >> 2) of the stream; the low two bits of an op say how the wait that follows its payload is coded */
enum { CINEDEF_EV_WAIT = 1, CINEDEF_EV_MODE = 2, CINEDEF_EV_FLAGS = 4, CINEDEF_EV_WEAPON = 5, CINEDEF_EV_THROTTLE = 6,
       CINEDEF_EV_AIM_SMALL = 14, CINEDEF_EV_AIM_LARGE = 22 };

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       rec_count, track_count, point_count, title_count, string_count, char_units, anim_ref_count, graph_count, pool_bytes;
	uint32_t       help_tag;                    /* tag id of the ingame help ustr the strings come from (0 = none) */
	const uint8_t *recs, *pool, *tracks, *points, *titles, *strtab, *chars, *arefs, *graphs;
} cinedef_section;

/* The control state of a recorded unit at one tick. */
typedef struct {
	uint8_t  mode, aux;              /* header bytes 0 and 1; `mode` goes 0 -> 3 on 73 recordings, meaning unverified */
	uint16_t word2;
	int16_t  weapon;                 /* weapon index, -1 none */
	int16_t  extra_a, extra_b;       /* version >= 3 / 4 header words, meaning unknown (grenade / zoom index?) */
	uint16_t flags;                  /* CINEDEF_CF_* */
	float    throttle[2];            /* x forward, y left; (0 0) (1 0) (-1 0) (0 1) (0 -1) (.707 -.707) occur */
	int16_t  yaw, pitch;             /* aim (aiming == looking on all 349 recordings), counts of pi / 1000 rad */
} cinedef_ctl;

typedef struct {
	char        name[CINEDEF_NAME_LEN];
	uint32_t    length;              /* ticks 0 .. length - 1 */
	uint32_t    stream_bytes, events;
	uint8_t     version, raw, ctl_version;      /* tag version (4), raw flag (0), unit control data version (0, 3, 4) */
	cinedef_ctl init;                /* the state before the first event */
} cinedef_rec;

typedef struct {
	uint8_t        type;             /* CINEDEF_EV_* */
	uint8_t        kind;             /* low two bits of the op */
	uint32_t       wait;             /* ticks to wait AFTER applying the payload */
	const uint8_t *payload;          /* PAYLOAD bytes of the type (little endian) */
} cinedef_event;

typedef struct {
	uint32_t tag_id, flags;
	uint32_t first_point, point_count;       /* into the point table */
	char     name[CINEDEF_TRACK_NAME_LEN];   /* last path component, truncated */
} cinedef_track;

typedef struct { float pos[3]; float quat[4]; } cinedef_point;

typedef struct {
	char     name[CINEDEF_NAME_LEN];
	int16_t  top, left, bottom, right;       /* text box, 640 x 480 virtual pixels */
	uint16_t string;                         /* index into the help strings, CINEDEF_NONE16 if none */
	uint16_t style, justify;                 /* HEK enums: plain 0 bold 1 italic 2 condense 3 underline 4 / left 0 right 1 center 2 */
	uint32_t colour, shadow;                 /* GS RGBA words */
	uint16_t fade_in, up, fade_out;          /* ticks */
} cinedef_title;

typedef struct {
	char     name[CINEDEF_NAME_LEN];
	uint32_t graph_tag;                      /* `antr` tag id */
	uint32_t resolved;                       /* 1 when `name` is an animation of the graph */
} cinedef_anim_ref;

cinedef_status cinedef_open(cinedef_section *s, const uint8_t *data, size_t size);

/* recorded animations */
cinedef_rec    cinedef_get_rec(const cinedef_section *s, uint32_t i);                 /* zeroed for a bad index */
int            cinedef_find_rec(const cinedef_section *s, const char *name);          /* first with that name, or -1 */
const uint8_t *cinedef_rec_stream(const cinedef_section *s, uint32_t i, uint32_t *bytes);   /* NULL for a bad index */
/* Decodes the event at stream[*pos] and advances *pos; returns 0 at the end of the stream (or on a malformed event, which a
 * validated section never contains). */
int  cinedef_next_event(const uint8_t *stream, uint32_t bytes, uint32_t *pos, cinedef_event *ev);
/* Applies the payload to a control state: yaw wraps into [-1000, 1000), pitch clamps to +-500. */
void cinedef_apply_event(cinedef_ctl *c, const cinedef_event *ev);

/* camera tracks (sorted by tag id) */
cinedef_track cinedef_get_track(const cinedef_section *s, uint32_t i);
int           cinedef_find_track(const cinedef_section *s, uint32_t tag_id);       /* index or -1 */
cinedef_point cinedef_get_point(const cinedef_section *s, uint32_t track, uint32_t k);   /* k-th control point of a track */

/* titles and strings */
cinedef_title cinedef_get_title(const cinedef_section *s, uint32_t i);
int           cinedef_find_title(const cinedef_section *s, const char *name);      /* index or -1 */
/* Copies string `index` of the help list as UTF-16 units into dst (NUL terminated when there is room); returns its length in
 * units, 0xFFFFFFFF when it does not exist. cap counts units. */
uint32_t      cinedef_get_string(const cinedef_section *s, uint32_t index, uint16_t *dst, uint32_t cap);

/* custom animation references and graphs */
cinedef_anim_ref cinedef_get_anim_ref(const cinedef_section *s, uint32_t i);
uint32_t         cinedef_get_graph(const cinedef_section *s, uint32_t i);            /* `antr` tag ids, ascending, unique */
int              cinedef_has_graph(const cinedef_section *s, uint32_t tag_id);

#endif
