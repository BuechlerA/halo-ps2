/* DualShock 2 on port 0 (libpad over the ROM sio2man/padman modules). */
#pragma once
#include "render/flycam.h"

#define PAD_BTN_START    0x0008
#define PAD_BTN_SELECT   0x0001
#define PAD_BTN_TRIANGLE 0x1000
#define PAD_BTN_CIRCLE   0x2000
#define PAD_BTN_CROSS    0x4000
#define PAD_BTN_SQUARE   0x8000
#define PAD_BTN_L1       0x0400
#define PAD_BTN_R1       0x0800
#define PAD_BTN_L2       0x0100
#define PAD_BTN_R2       0x0200

/* Returns 1 if a pad is ready. Safe to call without a pad: pad_poll then reports no input. */
int  pad_init(void);
/* Fills the fly-camera input and returns the button bits that went down since the last call. */
unsigned pad_poll(fly_input *in);
