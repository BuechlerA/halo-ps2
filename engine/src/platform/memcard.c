#include "platform/memcard.h"
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libmc.h>
/* libmc takes the IOP's FIO flags, not newlib's */
#define MC_O_RDONLY 0x0001
#define MC_O_WRONLY 0x0002
#define MC_O_CREAT 0x0200
#include <stdio.h>
#include <string.h>

static int s_ready;

static int mc_wait(void)
{
	int result = 0, cmd = 0;
	if (mcSync(0, &cmd, &result) < 0) return -1000;      /* no command pending / failure */
	return result;
}

int memcard_init(int format_blank)
{
	int ret, type, free_kb, format;
	SifInitRpc(0);
	ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);        /* pad_init loaded it already: a second load just reports it */
	ret = SifLoadModule("rom0:MCMAN", 0, NULL);
	if (ret < 0) { printf("[mc] MCMAN load failed (%d)\n", ret); return 0; }
	ret = SifLoadModule("rom0:MCSERV", 0, NULL);
	if (ret < 0) { printf("[mc] MCSERV load failed (%d)\n", ret); return 0; }
	if (mcInit(MC_TYPE_MC) < 0) { printf("[mc] mcInit failed\n"); return 0; }
	mcGetInfo(0, 0, &type, &free_kb, &format);
	ret = mc_wait();
	if (format_blank && type == MC_TYPE_PS2 && format != MC_FORMATTED) {
		mcFormat(0, 0);
		printf("[mc] formatted a blank card: %d\n", mc_wait());
		mcGetInfo(0, 0, &type, &free_kb, &format);
		ret = mc_wait();
	}
	if (ret < -1 || type != MC_TYPE_PS2 || format != MC_FORMATTED) { printf("[mc] no formatted PS2 card in slot 1 (result %d type %d format %d)\n", ret, type, format); return 0; }
	printf("[mc] card ready, %d KB free\n", free_kb);
	s_ready = 1;
	return 1;
}

static void slot_name(char *out, size_t cap, int slot) { snprintf(out, cap, "/HALOPS2/SAVE%d", slot); }

int memcard_save(int slot, const void *data, size_t n)
{
	char name[40];
	int fd, ret;
	size_t done = 0;
	if (!s_ready) return -1;
	mcMkDir(0, 0, "/HALOPS2");
	mc_wait();                                           /* exists already: not an error */
	slot_name(name, sizeof name, slot);
	mcDelete(0, 0, name);
	mc_wait();
	mcOpen(0, 0, name, MC_O_WRONLY | MC_O_CREAT);
	fd = mc_wait();
	if (fd < 0) { printf("[mc] open for write failed (%d)\n", fd); return -2; }
	while (done < n) {
		size_t chunk = n - done > 16384 ? 16384 : n - done;
		mcWrite(fd, (const char *)data + done, (int)chunk);
		ret = mc_wait();
		if (ret != (int)chunk) { printf("[mc] write failed (%d)\n", ret); mcClose(fd); mc_wait(); return -3; }
		done += chunk;
	}
	mcClose(fd);
	mc_wait();
	return 0;
}

long memcard_load(int slot, void *buf, size_t cap)
{
	char name[40];
	int fd, ret;
	size_t done = 0;
	if (!s_ready) return -1;
	slot_name(name, sizeof name, slot);
	mcOpen(0, 0, name, MC_O_RDONLY);
	fd = mc_wait();
	if (fd < 0) return 0;                                /* no such file */
	for (;;) {
		size_t want = cap - done > 16384 ? 16384 : cap - done;
		if (!want) break;
		mcRead(fd, (char *)buf + done, (int)want);
		ret = mc_wait();
		if (ret < 0) { mcClose(fd); mc_wait(); return -3; }
		if (ret == 0) break;
		done += (size_t)ret;
	}
	mcClose(fd);
	mc_wait();
	return (long)done;
}
