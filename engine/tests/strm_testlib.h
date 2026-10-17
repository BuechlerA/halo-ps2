/* Shared by test_strmdef.c and test_stream.c: pack loading, the tag ids of the synthetic world tools/tests/test_streamconv.py builds,
 * and a naive IMA decoder written straight from the formula (the decoder under test uses tables). */
#ifndef STRM_TESTLIB_H
#define STRM_TESTLIB_H
#include "audio/strmdef.h"
#include "stream/hp2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* Tag ids of the synthetic world: entry i of its tag list has ((0xE174 + i) << 16) | i. */
#define WTAG(i) ((((uint32_t)0xE174u + (i)) << 16) | (i))
#define W_LOOP_M1 WTAG(2)
#define W_LOOP_AIR WTAG(3)
#define W_LOOP_HUM WTAG(4)
#define W_INTRO WTAG(5)      /* music, 44100 -> 32000 stereo, 22016 -> 16000 frames, 400 Hz */
#define W_LOOP WTAG(6)       /* music loop, chain of 4, 15936 frames, 1001.5 Hz */
#define W_ALT WTAG(7)        /* alternate loop, 15936 frames (looping), 2000 Hz */
#define W_END WTAG(8)        /* end sound, 8000 frames, 3000 Hz */
#define W_LONG WTAG(9)       /* long chain, not a loop, 11648 frames, 700 Hz */
#define W_LINE1 WTAG(10)     /* dialogue, 3 variants, mono 22050: 4480 / 2560 / 4480 frames, 300 / 500 / 300 Hz */
#define W_LINE2 WTAG(11)     /* stereo 44100 dialogue folded to mono 22050, 3200 frames, 800 Hz */
#define W_AIR WTAG(12)       /* long ambience loop, mono 22050, 143360 frames */
#define W_HUM WTAG(13)       /* a 1 s loop: not streamed */
#define W_FIRE WTAG(14)      /* resident gameplay sound: not in the pack */

typedef struct {
	uint8_t *strm;               /* STRM section bytes (malloc) */
	size_t   strm_size;
	uint32_t strd_size, strd_offset;
	size_t   file_size;
	FILE    *f;                  /* the pack, kept open for stream data */
} testpack;

static inline int testpack_open(testpack *t, const char *path)
{
	uint8_t hdr[64 + 32 * HP2_MAX_SECTIONS];
	hp2_pack pk;
	const hp2_section *sm, *sd;
	size_t n;
	memset(t, 0, sizeof *t);
	t->f = fopen(path, "rb");
	if (!t->f) return -1;
	fseek(t->f, 0, SEEK_END);
	t->file_size = (size_t)ftell(t->f);
	fseek(t->f, 0, SEEK_SET);
	n = t->file_size < sizeof hdr ? t->file_size : sizeof hdr;
	if (fread(hdr, 1, n, t->f) != n) return -1;
	if (hp2_open_header(&pk, hdr, n, t->file_size) != HP2_OK) return -1;
	sm = hp2_find(&pk, "STRM");
	sd = hp2_find(&pk, "STRD");
	if (!sm || !sd || sm->codec != HP2_CODEC_STORED || sd->codec != HP2_CODEC_STORED) return -1;
	t->strm = malloc(sm->stored_size);
	if (!t->strm) return -1;
	fseek(t->f, (long)sm->offset, SEEK_SET);
	if (fread(t->strm, 1, sm->stored_size, t->f) != sm->stored_size) return -1;
	if (hp2_crc32(t->strm, sm->stored_size) != sm->crc32) return -1;
	t->strm_size = sm->stored_size;
	t->strd_size = sd->raw_size;
	t->strd_offset = sd->offset;
	return 0;
}

static inline int testpack_read(const testpack *t, uint32_t strd_rel, void *dst, size_t n)
{
	if (fseek(t->f, (long)(t->strd_offset + strd_rel), SEEK_SET) != 0) return -1;
	return fread(dst, 1, n, t->f) == n ? 0 : -1;
}

static inline void testpack_close(testpack *t)
{
	if (t->f) fclose(t->f);
	free(t->strm);
	memset(t, 0, sizeof *t);
}

/* The decoder written from the formula: sample 0 is the header predictor, 63 nibbles follow (low nibble first). `body` = 32 bytes. */
static const int naive_steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
	107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
	4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
	22385, 24623, 27086, 29794, 32767
};

static inline void naive_channel(int pred, int idx, const uint8_t *body, int16_t *out, int stride)
{
	int i;
	static const int adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
	if (idx > 88) idx = 88;
	out[0] = (int16_t)pred;
	for (i = 0; i < 63; i++) {
		int nib = (i & 1) ? body[i >> 1] >> 4 : body[i >> 1] & 15;
		int step = naive_steps[idx], d = step >> 3;
		if (nib & 1) d += step >> 2;
		if (nib & 2) d += step >> 1;
		if (nib & 4) d += step;
		pred += (nib & 8) ? -d : d;
		if (pred > 32767) pred = 32767;
		if (pred < -32768) pred = -32768;
		idx += adj[nib & 7];
		if (idx < 0) idx = 0;
		if (idx > 88) idx = 88;
		out[(i + 1) * stride] = (int16_t)pred;
	}
}

static inline void naive_block(const uint8_t *src, unsigned channels, int16_t *out)
{
	if (channels == 1) {
		naive_channel((int16_t)(src[0] | src[1] << 8), src[2], src + 4, out, 1);
	} else {
		uint8_t l[32], r[32];
		int g;
		for (g = 0; g < 8; g++) {
			memcpy(l + g * 4, src + 8 + g * 8, 4);
			memcpy(r + g * 4, src + 12 + g * 8, 4);
		}
		naive_channel((int16_t)(src[0] | src[1] << 8), src[2], l, out, 2);
		naive_channel((int16_t)(src[4] | src[5] << 8), src[6], r, out + 1, 2);
	}
}

static inline uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#endif
