/* Debug text: a 5x7 font in a 128x64 PSMT8 atlas, drawn as textured GS sprites through the VIF1
 * chain (PATH2, ordered after the geometry). Used by the profiler overlay. Pure C11. */
#ifndef RENDER_TEXT_H
#define RENDER_TEXT_H
#include "render/chain.h"
#include <stdint.h>

#define TEXT_ATLAS_W 128
#define TEXT_ATLAS_H 64
#define TEXT_CELL_W 6
#define TEXT_CELL_H 8
#define TEXT_MAX_CHARS 512

/* Fills the atlas: index 0 transparent, 1 = glyph pixel. 16 cells per row, character code - 32. */
void text_build_atlas(uint8_t atlas[TEXT_ATLAS_W * TEXT_ATLAS_H]);
/* The 1 KB CLUT image (16x16 PSMCT32): entry 0 transparent, entry 1 opaque white (alpha 0x80). */
void text_build_clut(uint32_t clut[256]);

typedef struct {
	uint32_t nchars;
	struct { int16_t x, y; uint8_t ch, r, g, b; } c[TEXT_MAX_CHARS];
} text_batch_t;

void text_batch_clear(text_batch_t *b);
/* Screen position in framebuffer pixels (origin top-left); '\n' starts a new line. */
void text_batch_print(text_batch_t *b, int x, int y, uint32_t rgb, const char *s);
/* Appends [FLUSHA, DIRECT(state + sprites)] to the chain. Atlas/CLUT blocks are in 64-word units. */
int  text_batch_emit(const text_batch_t *b, chain_t *c, uint32_t atlas_block, uint32_t clut_block,
                     int screen_w, int screen_h);

#endif
