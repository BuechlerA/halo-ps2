/* Player input: DualShock 2 mapping and scripted input for headless test runs. Pure C11, host-tested.
 *
 * Layout (Halo CE's Xbox layout on a DualShock 2): left stick move, right stick look, Cross jump, Square reload,
 * Triangle switch weapon / pick up, Circle melee, R2 fire, L2 throw grenade, L1 switch grenade type, R1 flashlight,
 * L3 crouch, R3 zoom, Select toggle overlay, Start pause, Select+Cross save a checkpoint, Select+Start revert to it. Scripted input (halo.cfg lines
 *     input <seconds> <lx> <ly> <rx> <ry> <buttons hex>
 * with the raw button bits of platform/pad.h) replaces the pad: each key holds until the next one. */
#ifndef SIM_INPUT_H
#define SIM_INPUT_H
#include "core/config.h"
#include "sim/player.h"

#define IN_JUMP     0x0001u
#define IN_CROUCH   0x0002u
#define IN_FIRE     0x0004u
#define IN_GRENADE  0x0008u
#define IN_MELEE    0x0010u
#define IN_RELOAD   0x0020u
#define IN_SWITCH   0x0040u     /* switch weapon / pick up */
#define IN_SWAPGREN 0x0080u
#define IN_LIGHT    0x0100u
#define IN_ZOOM     0x0200u
#define IN_OVERLAY  0x0400u
#define IN_PAUSE    0x0800u
#define IN_WALK     0x1000u
#define IN_SAVE     0x4000u     /* Select + Cross: save a checkpoint */
#define IN_REVERT   0x2000u     /* Select + Start: revert to the last checkpoint (debug / until the scripts save games) */

typedef struct { float lx, ly, rx, ry; unsigned buttons; } raw_input_t;   /* buttons: PAD_BTN_* bits, held */

typedef struct {
	player_cmd_t move;
	unsigned     held;       /* IN_* held this tick */
	unsigned     pressed;    /* IN_* that went down this tick */
} game_input_t;

#define INPUT_SCRIPT_MAX 120
typedef struct { float t, lx, ly, rx, ry; unsigned buttons; } input_key;
typedef struct { input_key key[INPUT_SCRIPT_MAX]; int count; } input_script;

/* Look controls from the settings (docs/39): invert the vertical look, sensitivity 1 .. 10 (3 = the engine's own rates). */
void input_set_look(int invert, int sens);
/* Maps raw pad state to game input; `prev_held` is the previous tick's `held`. */
void input_map(game_input_t *out, const raw_input_t *raw, unsigned prev_held);
/* Returns the number of keys parsed. Keys must be time-ordered; out-of-order ones are dropped. */
int  input_script_parse(input_script *s, const cfg_t *cfg);
/* The raw input in force at time `t` seconds (before the first key: neutral). */
void input_script_eval(const input_script *s, float t, raw_input_t *out);

#endif
