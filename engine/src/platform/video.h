/* Graphics Synthesizer setup: double-buffered colour + Z, interlaced NTSC output.
 * Chosen mode (ADR-003): 512x448 PSMCT16S x2, PSMZ24, full-height buffer with
 * the CRTC reading alternate lines per field (GS FFMD=0). */
#pragma once
#include <tamtypes.h>
#include <draw.h>
#include <packet2.h>

#define VIDEO_WIDTH  512
#define VIDEO_HEIGHT 448

typedef struct {
	framebuffer_t fb[2];
	zbuffer_t     z;
	int           back;        /* index of the buffer being drawn */
	u32           frames;      /* flips so far */
	u32           tex_base;    /* first free VRAM word address (page aligned): start of the texture window */
} video_t;

void  video_init(video_t *v);
/* Adds GS environment setup for the current back buffer to a GIF packet. */
void  video_add_setup_env(video_t *v, packet2_t *p);
/* Adds a full-screen clear of the current back buffer (colour only). */
void  video_add_clear(video_t *v, packet2_t *p, u8 r, u8 g, u8 b);
/* Fills the back buffer with a vertical gradient (colour only; Z is cleared to 0 = far): `top` at the
 * first row blending to `horizon` at row `horizon_y`, `horizon` below. */
void  video_add_sky(video_t *v, packet2_t *p, const u8 top[3], const u8 horizon[3], int horizon_y);
/* Waits for drawing + vsync, displays the back buffer, swaps. */
void  video_flip(video_t *v);
/* Free VRAM (bytes) after framebuffers and Z: the texture window. */
u32   video_texture_window_bytes(void);
/* VRAM is addressed in 32-bit words (page = 2048 words = 8 KB, block = 64 words). */
#define VIDEO_VRAM_WORDS (1024u * 1024u)
