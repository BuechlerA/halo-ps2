/* TEXS / LMAP section readers (docs/09-tex-section.md). They validate the tables of a section
 * already decompressed into memory and give bounds-checked access to textures, mips, CLUTs, shader
 * bindings and lightmap pages. Whatever tex_open / lm_open accept can be read through the
 * accessors without leaving the buffer. No heap, no ps2sdk dependency; byte loads only, so the
 * buffer needs no particular alignment (give it 16 so the data can be DMA'd as is). */
#ifndef WORLD_TEX_H
#define WORLD_TEX_H
#include <stddef.h>
#include <stdint.h>

#define TEX_PSMT8 0x13u            /* GS pixel storage formats */
#define TEX_PSMT4 0x14u
#define TEX_MAX_MIPS 4u
#define TEX_MIN_DIM 8u
#define TEX_MAX_DIM 1024u           /* the format's limit; resident textures stay at 256 (hero textures 512, ADR-073), only streamed variants (docs/37) go to 1024 */
#define TEX_NONE 0xFFFFu           /* binding without a texture: draw flat grey */

/* tex_info.flags */
#define TEX_F_PUNCH 1u             /* palette entry 0 is transparent, the rest opaque */
#define TEX_F_BLEND 2u             /* alpha is part of the palette (GS scale, 0x80 = 1.0) */
#define TEX_F_UPSCALED 4u          /* source was smaller than 8 px on a side */

/* tex_binding.flags */
#define TEX_B_ALPHA_TEST 1u
#define TEX_B_TWO_SIDED 2u
#define TEX_B_DECAL 4u
#define TEX_B_UNTEXTURED 8u
#define TEX_B_SKY 0x10u
#define TEX_B_BLENDED_BASE 0x20u

enum { TEX_BLEND_OPAQUE = 0, TEX_BLEND_ALPHA_TEST, TEX_BLEND_ALPHA, TEX_BLEND_ADD, TEX_BLEND_MULTIPLY,
       TEX_BLEND_DOUBLE_MULTIPLY, TEX_BLEND_SUBTRACT };
enum { TEX_GROUP_UNKNOWN = 0, TEX_GROUP_SENV, TEX_GROUP_SOTR, TEX_GROUP_SCHI, TEX_GROUP_SGLA, TEX_GROUP_SWAT };

typedef enum { TEX_OK = 0, TEX_ERR_TRUNCATED, TEX_ERR_MAGIC, TEX_ERR_LAYOUT } tex_status;

typedef struct {
	uint32_t source_id;            /* Xbox bitmap tag id, for debugging */
	uint16_t width, height;        /* mip 0 */
	uint8_t  psm, mip_count;
	uint16_t flags, clut_entries;  /* 256 for PSMT8, 16 for PSMT4 */
	uint32_t avg_rgba;             /* RGBA8, alpha on the GS 0x80 scale */
	uint32_t clut_offset, pixel_offset, pixel_bytes;   /* from the start of the section */
} tex_info;

typedef struct {
	const uint8_t *pixels;         /* raster order; PSMT4: two texels per byte, even texel low nibble */
	uint16_t       width, height;
	uint32_t       bytes;          /* padded to 16 */
} tex_mip_view;

typedef struct {
	uint32_t shader_id;
	uint16_t texture, flags;       /* texture index or TEX_NONE; TEX_B_* */
	uint8_t  blend, group;         /* TEX_BLEND_*, TEX_GROUP_* */
	uint16_t change;               /* colour change: 1..4 = change colour a..d tints a second pass of the part (objects, ADR-065), 0 = none */
	float    uv_scale[2];          /* multiply the baked texture uv */
	uint32_t flat_rgba;            /* average colour (flat grey when untextured) */
	uint16_t texture2;             /* the colour change pass's texture (base map with the mask as alpha), TEX_NONE = none */
} tex_binding;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       texture_count, binding_count;
	const uint8_t *textures, *bindings;
} tex_section;

tex_status tex_open(tex_section *s, const uint8_t *data, size_t size);
/* Same for a section of which only the first `avail` bytes (header and tables) are in memory (`size` = its whole length): the accessors that return pixel or CLUT
 * pointers (tex_mip, tex_clut) must not be used then; read the data from the offsets of tex_get. */
tex_status tex_open_tables(tex_section *s, const uint8_t *data, size_t avail, size_t size);
tex_info tex_get(const tex_section *s, uint32_t i);                    /* i < texture_count */
/* Number of mip levels a texture of this size may have (halve while the smaller side is > 8, max 4). */
uint32_t tex_max_mips(uint32_t width, uint32_t height);
/* Mip `level` (< mip_count) of `t`; returns 0 and clears *out when the level does not exist. */
int tex_mip(const tex_section *s, const tex_info *t, uint32_t level, tex_mip_view *out);
/* CLUT as a ready-to-upload image: 16x16 PSMCT32 (PSMT8, entries in CSM1 order) or 8x2 (PSMT4);
 * clut_entries * 4 bytes. */
const uint8_t *tex_clut(const tex_section *s, const tex_info *t);
tex_binding tex_get_binding(const tex_section *s, uint32_t i);         /* i < binding_count */
/* Binary search by shader tag id; returns 1 and fills *out when found. */
int tex_find_binding(const tex_section *s, uint32_t shader_id, tex_binding *out);

/* ---- lightmap pages (section LMn) ------------------------------------------------------------
 * 8-bit luminance (PSMT8) pages, one table slot per BSP lightmap page number; width 0 = unused.
 * All pages share one CLUT, a grey ramp whose alpha equals its index. */
#define LM_CLUT_BYTES 1024u

typedef struct {
	uint16_t width, height;        /* stored page */
	uint16_t src_width, src_height;
	uint8_t  psm;                  /* TEX_PSMT8 */
	uint32_t avg_rgba, avg_luma;
	uint32_t pixel_offset, pixel_bytes;
	const uint8_t *pixels;
} lm_page_info;

typedef struct {
	const uint8_t *data;
	size_t         size;
	uint32_t       page_count, used_count;
	float          gain;           /* luminance gain the converter applied */
	const uint8_t *pages, *clut;
} lm_section;

tex_status lm_open(lm_section *s, const uint8_t *data, size_t size);
/* Page `index` (the BSP material's lightmap_page); returns 0 for unused or out of range. */
int lm_get_page(const lm_section *s, uint32_t index, lm_page_info *out);

#endif
