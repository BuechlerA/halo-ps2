/* Debug framebuffer readback over TTY: GS local->host transfer of the buffer that was just
 * drawn, printed as "[fb]" hex rows that scripts/fb2png.py turns into a PNG. PCSX2 has no CLI
 * screenshot, so this is how rendering is verified headless (and on hardware via ps2link). */
#pragma once
#include <tamtypes.h>
#include <draw.h>

/* Reads the whole framebuffer and prints it. The GS must be idle (call after the flip that
 * waited for FINISH). Slow (about a second): only for debug runs selected by `fbdump <frame>`
 * in halo.cfg. */
void fbdump_frame(const framebuffer_t *fb, u32 frame);
