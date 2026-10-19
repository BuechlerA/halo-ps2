/* HUD sections reader (docs/20-hud-section.md): HUD0 (layouts), FONT (glyph tables), STRS (strings and messages) of a
 * `<map>.hud.hp2` pack written by tools/halo2ps2/hudconv.py. The textures live in the pack's HUDT section, a TEXS blob
 * that world/tex.c reads; sprites and glyphs name them by texture index.
 *
 * Pure C11, no heap, no double. Every *_open validates all tables once (sizes, alignment, ranges, enum values, finite
 * floats, string bounds), so the accessors never read outside the buffer whatever the bytes; records are decoded with
 * byte loads (the buffer needs no alignment). A failed open leaves the view zeroed and every accessor returns a neutral
 * record.
 *
 * Units: positions and sizes are pixels of a 640 x 480 virtual screen (origin top left, y down); the engine scales to
 * 512 x 448 itself. Time is in ticks of 1/30 s. Colours are RGBA words, R in the low byte, on the GS scale (0x80 = 1.0 per
 * channel, so a vertex colour can use them directly, a modulated texel at 0x80 stays unchanged). Texel coordinates are in
 * the converted HUDT texture (u1 / v1 exclusive). Tag ids are Xbox tag ids, as everywhere else. */
#ifndef UI_HUDDEF_H
#define UI_HUDDEF_H
#include "world/tex.h"
#include <stddef.h>
#include <stdint.h>

#define HUDDEF_NONE 0xFFFFu          /* no sprite / no texture / no icon */
#define HUDDEF_NO_STATE 0xFFu        /* element not attached to a weapon state */
#define HUDDEF_VIRT_W 640
#define HUDDEF_VIRT_H 480
#define HUDDEF_MAX_CHILD_DEPTH 8     /* child weapon HUD chain walked by huddef_find_crosshair */

typedef enum { HUDDEF_OK = 0, HUDDEF_ERR_TRUNCATED, HUDDEF_ERR_MAGIC, HUDDEF_ERR_LAYOUT, HUDDEF_ERR_VERSION } huddef_status;

/* hud kind */
enum { HUDDEF_UNIT = 0, HUDDEF_WEAPON, HUDDEF_GRENADE };
/* anchors: the corner of the title-safe frame (48 / 36 px in from the screen edges) or the screen centre an element's matching
 * corner (centre for HUDDEF_A_C) is placed at; positions arrive resolved, the converter did the maths (docs/20) */
enum { HUDDEF_A_TL = 0, HUDDEF_A_TR, HUDDEF_A_BL, HUDDEF_A_BR, HUDDEF_A_C };
#define HUDDEF_SAFE_X 48
#define HUDDEF_SAFE_Y 36
/* element roles */
enum {
	HUDDEF_R_NONE = 0, HUDDEF_R_HUD_BG, HUDDEF_R_SHIELD_BG, HUDDEF_R_HEALTH_BG, HUDDEF_R_MOTION_BG, HUDDEF_R_MOTION_FG,
	HUDDEF_R_MOTION_CENTER,          /* no sprite: x, y = the radar centre on screen; off_x = the radar radius in pixels (docs/20) */
	HUDDEF_R_AUX_BG, HUDDEF_R_STATIC, HUDDEF_R_OVERLAY, HUDDEF_R_CROSSHAIR,
	HUDDEF_R_GRENADE_BG, HUDDEF_R_GRENADE_COUNT_BG, HUDDEF_R_GRENADE_OVERLAY
};
enum { HUDDEF_M_SHIELD = 1, HUDDEF_M_HEALTH, HUDDEF_M_AUX, HUDDEF_M_WEAPON };
enum { HUDDEF_N_WEAPON = 1, HUDDEF_N_GRENADE };
/* weapon "state attached to" (elements of weapon HUDs; HUDDEF_NO_STATE elsewhere) */
enum { HUDDEF_S_TOTAL_AMMO = 0, HUDDEF_S_LOADED_AMMO, HUDDEF_S_HEAT, HUDDEF_S_AGE, HUDDEF_S_TOTAL_AMMO2, HUDDEF_S_LOADED_AMMO2,
       HUDDEF_S_TARGET_DISTANCE, HUDDEF_S_TARGET_ELEVATION };
/* weapon elements: map types they are meant for (the campaign skips HUDDEF_MAP_MULTIPLAYER) */
enum { HUDDEF_MAP_ANY = 0, HUDDEF_MAP_SOLO, HUDDEF_MAP_MULTIPLAYER };
/* blend of an element (same codes as tex.h TEX_BLEND_*) */
#define HUDDEF_BLEND_ALPHA TEX_BLEND_ALPHA
#define HUDDEF_BLEND_ADD TEX_BLEND_ADD
/* number flags */
#define HUDDEF_NUM_LEADING_ZEROS 1u
#define HUDDEF_NUM_ONLY_ZOOMED 2u
#define HUDDEF_NUM_TRAILING_M 4u
/* crosshair types (bit n of huddef_hud.crosshair_mask), HEK order */
enum { HUDDEF_X_AIM = 0, HUDDEF_X_ZOOM, HUDDEF_X_CHARGE, HUDDEF_X_SHOULD_RELOAD, HUDDEF_X_FLASH_HEAT, HUDDEF_X_FLASH_TOTAL_AMMO,
       HUDDEF_X_FLASH_BATTERY, HUDDEF_X_RELOAD_OVERHEAT, HUDDEF_X_FIRING_NO_AMMO, HUDDEF_X_THROWING_NO_GRENADE,
       HUDDEF_X_LOW_AMMO_NONE_LEFT, HUDDEF_X_SHOULD_RELOAD2, HUDDEF_X_FLASH_TOTAL_AMMO2, HUDDEF_X_FLASH_RELOAD2,
       HUDDEF_X_FIRING_NO_AMMO2, HUDDEF_X_LOW_AMMO2, HUDDEF_X_PRIMARY_READY, HUDDEF_X_SECONDARY_READY,
       HUDDEF_X_FIRING_DEPLETED_BATTERY, HUDDEF_X_MAX = 31 };
/* crosshair overlay flags (huddef_elem.flags of a HUDDEF_R_CROSSHAIR element), HEK names, unverified except 4 */
#define HUDDEF_CF_FLASHES 1u
#define HUDDEF_CF_NOT_A_SPRITE 2u
#define HUDDEF_CF_ONLY_ZOOMED 4u            /* verified: every zoom reticle of the pistol / sniper / rocket launcher has it */
/* link kinds */
enum { HUDDEF_L_WEAPON = 0, HUDDEF_L_UNIT, HUDDEF_L_GRENADE };

/* ---- HUD0 ------------------------------------------------------------------------------------------------------ */
typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       hud_count, elem_count, meter_count, number_count, cross_count, sound_count, sprite_count, icon_count, link_count;
	const uint8_t *globals, *huds, *elems, *meters, *numbers, *crosses, *sounds, *sprites, *icons, *links;
} huddef_section;

typedef struct {
	uint16_t tex;                    /* texture index in HUDT */
	uint16_t flags;                  /* HUDDEF_SF_* */
	uint16_t w, h;                   /* size in virtual pixels at scale 1 */
	uint16_t u0, v0, u1, v1;         /* texel rectangle of the converted texture */
	int16_t  reg_x, reg_y;           /* registration point relative to the sprite's top left (pixels) */
	uint32_t bitmap_tag;             /* source bitmap tag id (debugging) */
} huddef_sprite;
#define HUDDEF_SF_PADDED 1u          /* the source image was padded to a power of two on the right / bottom */
#define HUDDEF_SF_SCALED 2u          /* texels != source pixels (images over 256 were halved) */

typedef struct {
	uint8_t  role, state, allowed;   /* HUDDEF_R_*, HUDDEF_S_* or HUDDEF_NO_STATE, HUDDEF_MAP_* */
	uint8_t  frames;                 /* consecutive sprites of the sequence starting at `sprite` (0 when there is none) */
	uint16_t sprite;                 /* first sprite or HUDDEF_NONE */
	uint16_t type;                   /* weapon sub overlays / crosshair overlays: the tag's "type" word */
	uint32_t flags;                  /* weapon sub overlays: overlay flags; crosshair overlays: HUDDEF_CF_* */
	int16_t  x, y, w, h;             /* resolved top left and drawn size (virtual pixels, scale applied) */
	int16_t  off_x, off_y;           /* the tag's anchor offset */
	float    scale_x, scale_y;
	uint16_t scaling_flags;
	uint8_t  anchor, blend;
	uint32_t colour, flash_colour, disabled_colour;
	uint16_t flash_period, flash_delay, flash_count, flash_flags, flash_length;   /* ticks */
	uint16_t frame_rate, seq;        /* raw: sprite animation rate, sequence index in the source bitmap */
} huddef_elem;

typedef struct {
	uint8_t  role, state, allowed, flags;   /* flags: meter flags raw (bit 0 use min/max for state changes ...) */
	uint16_t sprite;
	uint8_t  min_value, anchor;
	int16_t  x, y, w, h, off_x, off_y;
	float    scale_x, scale_y;
	uint16_t scaling_flags;
	uint8_t  alpha_mult, alpha_bias;
	uint32_t colour_min, colour_max, colour_flash, colour_empty, colour_disabled;
	uint16_t value_scale, seq;
	float    opacity, translucency;
	float    min_fraction;           /* auxiliary meters */
	uint32_t aux_flags;
} huddef_meter;

typedef struct {
	uint8_t  role, state, allowed, max_digits;
	uint8_t  flags, fraction_digits, anchor;
	int16_t  x, y, w, h, off_x, off_y;
	float    scale_x, scale_y;
	uint16_t scaling_flags;
	uint32_t colour, flash_colour, disabled_colour;
	uint16_t flash_period, flash_delay, flash_count, flash_flags, flash_length;
} huddef_number;

typedef struct {
	uint8_t  type;                   /* HUDDEF_X_* */
	uint16_t elem_first, elem_count; /* overlay elements (HUDDEF_R_CROSSHAIR) in the element table */
	uint32_t bitmap_tag;
	uint32_t hud_index;              /* filled by huddef_find_crosshair: the HUD (own or child) that provides it */
} huddef_crosshair;

typedef struct {
	uint32_t sound_tag, group, latched_to, hud_tag;   /* group: 'snd!' or 'lsnd' as a little-endian four-char code */
	float    scale;
} huddef_sound;

typedef struct {
	uint16_t sprite;                 /* sprite of the hud_globals icon bitmap or HUDDEF_NONE */
	uint16_t seq;
	int16_t  width_offset, x, y;
	uint32_t colour;
	uint8_t  b12, b13;
	uint16_t index;
} huddef_icon;

typedef struct {
	uint32_t tag_id;
	uint8_t  kind, anchor;
	uint16_t flags;                  /* weapon HUD flags raw: bit 0 use parent's flashing parameters */
	uint32_t child_tag;              /* weapon HUD whose elements and crosshairs are also in effect (0 = none) */
	uint16_t total_ammo_cutoff, loaded_ammo_cutoff, heat_cutoff, age_cutoff;
	uint16_t elem_first, elem_count; /* includes the crosshair overlay elements (reached through the crosshair list) */
	uint16_t meter_first, meter_count, number_first, number_count, cross_first, cross_count, sound_first, sound_count;
	uint32_t crosshair_mask;         /* bit n: this HUD has a crosshair of type n */
	uint16_t icon_seq;               /* pickup icon (weapon HUDs) or HUDDEF_NONE */
	int16_t  icon_width_offset, icon_x, icon_y;
	uint16_t icon_sprite;
} huddef_hud;

typedef struct {
	uint8_t  anchor;
	int16_t  off_x, off_y;
	uint32_t font_sp, font_mp;       /* font tag ids */
	float    item_scale_x, item_scale_y;
	uint32_t text_colour_a, text_colour_b;
	uint32_t default_weapon_hud;
	uint32_t item_strings, icon_strings;           /* ustr tag ids (STRS lists) */
	uint32_t messages, level_messages;             /* hmt tag ids (STRS message sets), 0 = none */
	uint32_t icon_bitmap, digit_tag;
	float    motion_range, motion_velocity, motion_scale;
	uint16_t damage_sprite, damage_mp_sprite;
	uint32_t damage_colour;
	uint16_t digit_first, digit_count;             /* sprites 0..9 digits, 10 '.', 11 ':', 12 '-', 13 'm' */
	int8_t   bitmap_digit_width, screen_digit_width, digit_x_offset, digit_y_offset, decimal_width, colon_width;
	uint16_t icon_count;
	uint16_t blip_sprite;                          /* motion sensor blip (ui\hud\bitmaps\hud_sensor_blip) or HUDDEF_NONE */
} huddef_globals;

typedef struct { uint32_t owner, kind, hud_tag, order; } huddef_link;

huddef_status huddef_open(huddef_section *s, const uint8_t *data, size_t size);
/* Checks the sprites against the HUDT texture section the pack carries (texture index, rectangle inside the texture). */
huddef_status huddef_check_textures(const huddef_section *s, const tex_section *t);
huddef_globals huddef_get_globals(const huddef_section *s);
huddef_sprite  huddef_get_sprite(const huddef_section *s, uint32_t i);
huddef_hud     huddef_get_hud(const huddef_section *s, uint32_t i);
int            huddef_find_hud(const huddef_section *s, uint32_t tag_id);        /* index or -1 */
huddef_elem    huddef_get_elem(const huddef_section *s, uint32_t i);
huddef_meter   huddef_get_meter(const huddef_section *s, uint32_t i);
huddef_number  huddef_get_number(const huddef_section *s, uint32_t i);
huddef_crosshair huddef_get_crosshair(const huddef_section *s, uint32_t i);
huddef_sound   huddef_get_sound(const huddef_section *s, uint32_t i);
huddef_icon    huddef_get_icon(const huddef_section *s, uint32_t i);
huddef_link    huddef_get_link(const huddef_section *s, uint32_t i);
/* The crosshair of `type` of weapon HUD `hud_tag`: its own, else the child HUD chain's (at most HUDDEF_MAX_CHILD_DEPTH).
 * Returns 1 and fills *out (hud_index = the HUD that owns it) or 0. */
int huddef_find_crosshair(const huddef_section *s, uint32_t hud_tag, uint32_t type, huddef_crosshair *out);
/* Link table: the HUD tag of weapon tag / unit tag (order = n-th unit HUD reference, 0 first) / grenade type index. */
int huddef_find_link(const huddef_section *s, uint32_t kind, uint32_t owner, uint32_t order, uint32_t *hud_tag);

/* ---- FONT ------------------------------------------------------------------------------------------------------ */
typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       font_count, glyph_count;
	const uint8_t *fonts, *glyphs;
} huddef_fonts;

typedef struct {
	uint32_t tag_id, flags;
	int16_t  ascent, descent, leading_h, leading_w;
	uint32_t glyph_first, glyph_count;
	uint16_t tex_first, tex_count;
} huddef_font;

typedef struct {
	uint16_t code;                   /* UTF-16 code unit */
	int16_t  advance;                /* pen advance in pixels */
	uint16_t w, h;                   /* glyph quad size; 0 for blanks */
	int16_t  ox, oy;                 /* quad left = pen x + ox, quad top = baseline y - oy */
	uint16_t tex, u0, v0, u1, v1;    /* HUDT texture index (HUDDEF_NONE for blanks) and texel rectangle */
} huddef_glyph;

huddef_status huddef_fonts_open(huddef_fonts *f, const uint8_t *data, size_t size);
huddef_font huddef_get_font(const huddef_fonts *f, uint32_t index);
int huddef_find_font(const huddef_fonts *f, uint32_t tag_id);                    /* index or -1 */
/* Glyph of `code` in font `font` (binary search); returns 1 and fills *out, or 0 (and *out zeroed). */
int huddef_find_glyph(const huddef_fonts *f, uint32_t font, uint32_t code, huddef_glyph *out);
/* Sum of the advances of the glyphs found in s[0..n); characters the font lacks count as the advance of '?' when it has
 * one, else 0. */
int32_t huddef_text_width(const huddef_fonts *f, uint32_t font, const uint16_t *s, uint32_t n);
huddef_status huddef_check_font_textures(const huddef_fonts *f, const tex_section *t);

/* ---- STRS ------------------------------------------------------------------------------------------------------ */
#define HUDDEF_STR_NONE 0xFFFFFFFFu
#define HUDDEF_SEG_TEXT 0
#define HUDDEF_SEG_ICON 1

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       list_count, set_count, message_count, segment_count, string_count, char_units;
	const uint8_t *lists, *sets, *messages, *segments, *table, *chars;
} huddef_strings;

typedef struct {
	uint8_t  kind;                   /* HUDDEF_SEG_TEXT or HUDDEF_SEG_ICON */
	uint8_t  icon;                   /* icon segments: the button icon / placeholder value */
	uint32_t length;                 /* text segments: UTF-16 units */
} huddef_segment;

huddef_status huddef_strings_open(huddef_strings *st, const uint8_t *data, size_t size);
int      huddef_find_list(const huddef_strings *st, uint32_t tag_id);             /* ustr tag -> list index or -1 */
uint32_t huddef_list_size(const huddef_strings *st, uint32_t list);               /* 0 for a bad list */
/* Copies string `index` of `list` as UTF-16 units into dst (NUL terminated when there is room); returns its length in
 * units (without the NUL), HUDDEF_STR_NONE when the list or index does not exist. cap counts units. */
uint32_t huddef_get_string(const huddef_strings *st, uint32_t list, uint32_t index, uint16_t *dst, uint32_t cap);
int      huddef_find_set(const huddef_strings *st, uint32_t tag_id);              /* hmt tag -> set index or -1 */
uint32_t huddef_set_size(const huddef_strings *st, uint32_t set);                 /* messages in the set */
int      huddef_find_message(const huddef_strings *st, uint32_t set, const char *name);   /* message index in the set or -1 */
uint32_t huddef_segment_count(const huddef_strings *st, uint32_t set, uint32_t message);
/* Segment k of a message; text segments are copied to dst like huddef_get_string. Returns 0 if it does not exist. */
int huddef_get_segment(const huddef_strings *st, uint32_t set, uint32_t message, uint32_t k, huddef_segment *out,
                       uint16_t *dst, uint32_t cap);

#endif
