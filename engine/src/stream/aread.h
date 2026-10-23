/* Asynchronous reads of one file: a request is *started* (one at a time) and *polled*, the read itself runs on an IO thread that makes ordinary blocking fio calls
 * (the pattern, and the reasons it is a thread and not fioSetBlockMode(FIO_NOWAIT), are in audio/stream.c and docs/29: fio has one RPC client, blocking calls from
 * several threads are serialised by the library, a read started with FIO_NOWAIT would be completed by whichever fio call comes next). The streamed textures
 * (world/hires.c) use it so a variant read never stalls a frame. On the host (tests) a read completes inside aread_start. */
#ifndef STREAM_AREAD_H
#define STREAM_AREAD_H
#include <stdint.h>

#ifdef _EE
typedef struct {
	int           fd, tid, sema;
	int           pending, quit;
	volatile int  req_done, req_result, exited;
	uint32_t      req_off, req_size, want;
	void         *req_dst;
	uint8_t       stack[8192] __attribute__((aligned(16)));
} aread_t;
#else
typedef struct { void *f; int pending, result; } aread_t;
#endif

/* path as for file_load ("assets/hp2/a10.thires.hp2"); 0 on success. */
int  aread_open(aread_t *a, const char *path);
/* Begin reading `bytes` at `offset` into dst (64-byte aligned, room for `bytes` rounded up to 64); 0 = started, -1 = busy or unusable. */
int  aread_start(aread_t *a, uint32_t offset, void *dst, uint32_t bytes);
/* 1 = finished ok (dst holds the bytes), 0 = running, -1 = failed (or nothing pending). */
int  aread_poll(aread_t *a);
void aread_close(aread_t *a);
#endif
