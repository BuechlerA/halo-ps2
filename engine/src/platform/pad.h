/* DualShock 2 on port 0 (libpad over the ROM sio2man/padman modules). */
#pragma once
#include "render/flycam.h"
#include "sim/input.h"

#define PAD_BTN_UP       0x0010
#define PAD_BTN_RIGHT    0x0020
#define PAD_BTN_DOWN     0x0040
#define PAD_BTN_LEFT     0x0080
#define PAD_BTN_START    0x0008
#define PAD_BTN_SELECT   0x0001
#define PAD_BTN_TRIANGLE 0x1000
#define PAD_BTN_CIRCLE   0x2000
#define PAD_BTN_CROSS    0x4000
#define PAD_BTN_SQUARE   0x8000
#define PAD_BTN_L1       0x0400
#define PAD_BTN_R1       0x0800
#define PAD_BTN_L3       0x0002
#define PAD_BTN_R3       0x0004
#define PAD_BTN_L2       0x0100
#define PAD_BTN_R2       0x0200

/* Returns 1 if a pad is ready. Safe to call without a pad: pad_poll then reports no input. */
int  pad_init(void);
/* Fills the fly-camera input and returns the button bits that went down since the last call. */
unsigned pad_poll(fly_input *in);
/* Full pad state for the game: sticks (-1..1, +y up) and held button bits. Returns 1 if a pad answered. */
int pad_read_raw(raw_input_t *out);
