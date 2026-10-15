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
} video_t;

void  video_init(video_t *v);
/* Adds GS environment setup for the current back buffer to a GIF packet. */
void  video_add_setup_env(video_t *v, packet2_t *p);
/* Adds a full-screen clear of the current back buffer (colour only). */
void  video_add_clear(video_t *v, packet2_t *p, u8 r, u8 g, u8 b);
/* Waits for drawing + vsync, displays the back buffer, swaps. */
void  video_flip(video_t *v);
/* Free VRAM (bytes) after framebuffers and Z: the texture window. */
u32   video_texture_window_bytes(void);
