/* GS texture-path self test (cfg `selftest 1`): draws known PSMT4 / PSMT8 index patterns with known
 * CLUTs so the upload layout, nibble order and CLUT shapes can be checked in a framebuffer dump
 * (or on hardware) without any game data. */
#pragma once
#include "platform/video.h"

/* Renders one frame into the current back buffer and waits for it; the caller then flips/dumps. */
void selftest_draw(video_t *v);
