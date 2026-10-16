#include "platform/pad.h"
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>

static char s_padbuf[256] __attribute__((aligned(64)));
static int  s_ready;
static unsigned s_prev;

int pad_init(void)
{
	int ret, i;
	SifInitRpc(0);
	ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
	if (ret < 0) { printf("[err] SIO2MAN load failed (%d)\n", ret); return 0; }
	ret = SifLoadModule("rom0:PADMAN", 0, NULL);
	if (ret < 0) { printf("[err] PADMAN load failed (%d)\n", ret); return 0; }
	padInit(0);
	if (!padPortOpen(0, 0, s_padbuf)) { printf("[err] padPortOpen failed\n"); return 0; }
	for (i = 0; i < 100 && padGetState(0, 0) != PAD_STATE_STABLE && padGetState(0, 0) != PAD_STATE_FINDCTP1; i++) {
		/* wait for the port to settle (about a second at most; no pad = we move on) */
		int d;
		for (d = 0; d < 400000; d++) __asm__ volatile("");
	}
	if (padGetState(0, 0) != PAD_STATE_STABLE) {
		printf("[pad] no stable pad (state %d), continuing without input\n", padGetState(0, 0));
		return 0;
	}
	padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);   /* analog sticks, locked */
	s_ready = 1;
	printf("[pad] ready\n");
	return 1;
}

static float stick(unsigned char v) { return ((float)v - 127.5f) / 127.5f; }

unsigned pad_poll(fly_input *in)
{
	struct padButtonStatus b;
	unsigned buttons, down;
	memset(in, 0, sizeof *in);
	if (!s_ready || padGetState(0, 0) != PAD_STATE_STABLE) return 0;
	if (padRead(0, 0, &b) == 0) return 0;
	buttons = (unsigned)(0xFFFF ^ b.btns);
	in->move_x = stick(b.ljoy_h);
	in->move_y = -stick(b.ljoy_v);
	in->look_x = stick(b.rjoy_h);
	in->look_y = -stick(b.rjoy_v);
	in->rise = ((buttons & PAD_BTN_R1) ? 1.0f : 0.0f) - ((buttons & PAD_BTN_L1) ? 1.0f : 0.0f);
	in->fast = (buttons & PAD_BTN_R2) != 0;
	down = buttons & ~s_prev;
	s_prev = buttons;
	return down;
}
