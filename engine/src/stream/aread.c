#include "stream/aread.h"
#include <stdio.h>
#include <string.h>

#ifdef _EE
#define NEWLIB_PORT_AWARE
#include <fileio.h>
#include <kernel.h>
#include <tamtypes.h>

static void io_thread(void *arg)
{
	aread_t *a = (aread_t *)arg;
	for (;;) {
		int r = -1;
		WaitSema(a->sema);
		if (a->quit) break;
		if (fioLseek(a->fd, (int)a->req_off, 0 /* SEEK_SET */) >= 0) r = fioRead(a->fd, a->req_dst, (int)a->req_size);
		a->req_result = r;
		a->req_done = 1;
	}
	a->exited = 1;
	ExitDeleteThread();
}

int aread_open(aread_t *a, const char *path)
{
	char rb[160];
	ee_sema_t sema;
	ee_thread_t th;
	ee_thread_status_t me;
	int self = GetThreadId(), prio;
	extern void *_gp;
	memset(a, 0, sizeof *a);
	a->fd = a->tid = a->sema = -1;
	if (strchr(path, ':')) snprintf(rb, sizeof rb, "%s", path); else snprintf(rb, sizeof rb, "host:/%s", path);
	a->fd = fioOpen(rb, FIO_O_RDONLY);
	if (a->fd < 0) return -1;
	sema.init_count = 0; sema.max_count = 1; sema.option = 0;
	a->sema = CreateSema(&sema);
	if (a->sema < 0) { fioClose(a->fd); a->fd = -1; return -1; }
	if (ReferThreadStatus(self, &me) < 0) me.current_priority = 2;
	prio = (int)me.current_priority;
	if (prio < 2) { ChangeThreadPriority(self, 2); prio = 2; }       /* the game thread spins in graph_wait_vsync: the IO thread must outrank it (audio/stream.c) */
	memset(&th, 0, sizeof th);
	th.func = io_thread;
	th.stack = a->stack;
	th.stack_size = sizeof a->stack;
	th.gp_reg = &_gp;
	th.initial_priority = prio - 1;
	a->tid = CreateThread(&th);
	if (a->tid < 0 || StartThread(a->tid, a) < 0) { DeleteSema(a->sema); fioClose(a->fd); a->fd = a->tid = a->sema = -1; return -1; }
	return 0;
}

int aread_start(aread_t *a, uint32_t offset, void *dst, uint32_t bytes)
{
	if (a->fd < 0 || a->tid < 0 || a->pending) return -1;
	a->req_off = offset;
	a->req_dst = dst;
	a->want = bytes;
	a->req_size = (bytes + 63u) & ~63u;                              /* whole 64-byte lines: the DMA never shares a cache line with other data */
	a->req_done = 0;
	a->pending = 1;
	SignalSema(a->sema);
	return 0;
}

int aread_poll(aread_t *a)
{
	if (!a->pending) return -1;
	if (!a->req_done) return 0;
	a->pending = 0;
	return a->req_result >= (int)a->want ? 1 : -1;
}

void aread_close(aread_t *a)
{
	if (a->fd < 0) return;
	while (a->pending && !a->req_done) {}
	a->quit = 1;
	if (a->sema >= 0) SignalSema(a->sema);
	while (a->tid >= 0 && !a->exited) { /* the thread deletes itself */ }
	if (a->sema >= 0) DeleteSema(a->sema);
	fioClose(a->fd);
	a->fd = a->tid = a->sema = -1;
}
#else
int aread_open(aread_t *a, const char *path)
{
	memset(a, 0, sizeof *a);
	a->f = fopen(path, "rb");
	return a->f ? 0 : -1;
}

int aread_start(aread_t *a, uint32_t offset, void *dst, uint32_t bytes)
{
	FILE *f = a->f;
	if (!f || a->pending) return -1;
	a->result = fseek(f, (long)offset, SEEK_SET) == 0 && fread(dst, 1, bytes, f) == bytes ? 1 : -1;
	a->pending = 1;
	return 0;
}

int aread_poll(aread_t *a)
{
	if (!a->pending) return -1;
	a->pending = 0;
	return a->result;
}

void aread_close(aread_t *a)
{
	if (a->f) fclose((FILE *)a->f);
	a->f = NULL;
}
#endif
