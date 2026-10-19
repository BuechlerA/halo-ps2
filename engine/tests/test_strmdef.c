/* Host test for audio/strmdef.c: the STRM reader and the block decoder.
 *   test_strmdef FIXTURE.hp2 [REAL.strm.hp2[=REAL.crc] ...]
 * FIXTURE is the pack tools/tests/test_streamconv.py writes (STRM_FIXTURE=...), with FIXTURE.crc next to it: the reference decoder's
 * CRC-32 of the PCM of every stream. The numbers below are those of its synthetic world (see strm_testlib.h and the table in the test).
 * Checks: every field, lookups, bit-exact decode against the Python reference (CRC) and a naive formula decoder (random blocks),
 * truncation at every length, every byte flipped three ways, targeted field corruption; real packs are opened, walked, decoded on
 * a sample and, with a .crc file, compared with the Python decoder. ASan/UBSan clean. */
#include "strm_testlib.h"
#include <math.h>
#include <time.h>

static volatile uint32_t sink;

/* Touch everything an opened section hands out and check the invariants strm_open promises. `strd` (may be NULL) holds STRD: the first and
 * last block of every stream are decoded (inside the buffer, or ASan complains). */
static void walk(const strm_section *s, const uint8_t *strd)
{
	uint32_t acc = 0, i, k;
	for (i = 0; i < s->sound_count; i++) {
		strm_sound d = strm_get_sound(s, i);
		if (strm_find(s, d.tag_id) != (int)i) abort();
		if ((uint64_t)d.stream_first + d.stream_count > s->stream_count || !d.stream_count) abort();
		if (d.min_dist != d.min_dist || d.gain_mod != d.gain_mod || d.skip_fraction != d.skip_fraction) abort();
		acc += d.tag_id + d.klass + d.kind + d.flags + d.max_play_ms + d.tag_flags + (uint32_t)strlen(d.name);
	}
	for (i = 0; i < s->stream_count; i++) {
		strm_stream t = strm_get_stream(s, i);
		uint32_t unit = strm_unit_bytes(t.channels);
		if (t.channels < 1 || t.channels > 2 || t.format != STRM_FMT_XBOX_IMA || t.data_size % unit || t.data_size == 0) abort();
		if (t.frames != t.data_size / unit * STRM_BLOCK_FRAMES || t.data_offset % s->data_align) abort();
		if ((uint64_t)t.data_offset + t.data_size > s->data_size) abort();
		if (t.rate == 0 || t.rate > STRM_MAX_RATE || (t.loop_end != 0) != ((t.flags & STRM_F_LOOP) != 0)) abort();
		if (t.loop_end && (t.loop_start >= t.loop_end || t.loop_end > t.frames)) abort();
		if (t.gain != t.gain || t.skip != t.skip) abort();
		acc += t.data_offset + t.rate + t.flags + t.chunks + t.range + t.owner + t.src_bytes + (uint32_t)t.snr10;
		if (strd) {
			int16_t out[STRM_BLOCK_FRAMES * 2];
			strm_decode_blocks(strd + t.data_offset, t.channels, 1, out);
			strm_decode_blocks(strd + t.data_offset + t.data_size - unit, t.channels, 1, out);
			acc += (uint32_t)out[0] + (uint32_t)out[t.channels * 63];
		}
	}
	for (i = 0; i < s->loop_count; i++) {
		strm_loop l = strm_get_loop(s, i);
		if (strm_find_loop(s, l.tag_id) != (int)i) abort();
		if ((uint64_t)l.track_first + l.track_count > s->track_count || l.param != l.param) abort();
		for (k = 0; k < l.track_count; k++) {
			strm_track t = strm_get_track(s, &l, k);
			if (t.gain != t.gain || t.fade_in != t.fade_in || t.fade_out != t.fade_out) abort();
			acc += t.flags + t.start + t.loop + t.end + t.alt_loop + t.alt_end;
		}
		acc += (uint32_t)strlen(l.name);
	}
	sink = acc;
}

static int check_decoder_random(void)
{
	/* random blocks, headers included (an index above 88 is clamped): table decoder == formula decoder, mono and stereo */
	uint32_t seed = 12345u, n;
	uint8_t blk[72 * 4];
	int16_t a[STRM_BLOCK_FRAMES * 2 * 4], b[STRM_BLOCK_FRAMES * 2];
	for (n = 0; n < 20000; n++) {
		unsigned ch = 1u + (n & 1u), i, bl, unit = strm_unit_bytes(ch);
		for (i = 0; i < sizeof blk; i++) {
			seed = seed * 1664525u + 1013904223u;
			blk[i] = (uint8_t)(seed >> 24);
		}
		if (n % 3 == 0)                                      /* sane headers most of the time */
			for (bl = 0; bl < 4; bl++) {
				blk[bl * unit + 2] = (uint8_t)((blk[bl * unit + 2]) % 89);
				if (ch == 2) blk[bl * unit + 6] = (uint8_t)(blk[bl * unit + 6] % 89);
			}
		strm_decode_blocks(blk, ch, 4, a);
		for (bl = 0; bl < 4; bl++) {
			naive_block(blk + bl * unit, ch, b);
			if (memcmp(b, a + bl * STRM_BLOCK_FRAMES * ch, STRM_BLOCK_FRAMES * ch * sizeof(int16_t)) != 0) {
				fprintf(stderr, "FAIL: block %u of %u differs from the formula decoder (ch %u)\n", bl, n, ch);
				return 1;
			}
		}
	}
	/* extremes: a full-scale step ladder clamps instead of wrapping */
	{
		uint8_t m[36];
		int16_t o[64];
		int i;
		memset(m, 0x77, sizeof m);
		m[0] = 0xFF; m[1] = 0x7F; m[2] = 88; m[3] = 0;
		strm_decode_blocks(m, 1, 1, o);
		for (i = 1; i < 64; i++) CHECK(o[i] == 32767);
		memset(m, 0xFF, sizeof m);
		m[0] = 0x00; m[1] = 0x80; m[2] = 88;
		strm_decode_blocks(m, 1, 1, o);
		for (i = 1; i < 64; i++) CHECK(o[i] == -32768);
		/* the header predictor is sample 0, an all-zero nibble run decays by step/8 each sample */
		memset(m, 0, sizeof m);
		m[0] = 0xE8; m[1] = 0x03; m[2] = 0;                       /* predictor 1000, index 0 (step 7: delta 0) */
		strm_decode_blocks(m, 1, 1, o);
		CHECK(o[0] == 1000 && o[1] == 1000 && o[63] == 1000);
		CHECK(strm_unit_bytes(1) == 36 && strm_unit_bytes(2) == 72);
	}
	return 0;
}

static int check_fixture(const testpack *tp, const uint8_t *strd)
{
	strm_section s;
	strm_sound d;
	strm_stream t;
	strm_loop l;
	strm_track tr;
	int i;
	CHECK(strm_open(&s, tp->strm, tp->strm_size, tp->strd_size) == STRM_OK);
	CHECK(s.sound_count == 8 && s.stream_count == 10 && s.loop_count == 2 && s.track_count == 2 && s.data_align == 2048);
	CHECK(s.passthrough_streams == 4 && s.transcoded_streams == 6 && s.shared_streams == 1);
	CHECK(s.dialogue_streams == 4 && s.music_streams == 5 && s.ambience_streams == 1);
	CHECK(s.data_size == tp->strd_size && tp->strd_offset % 2048 == 0);
	/* sounds: ascending tag ids, lookups, classes, kinds, flags */
	CHECK(strm_find(&s, W_INTRO) == 0 && strm_find(&s, W_LOOP) == 1 && strm_find(&s, W_ALT) == 2 && strm_find(&s, W_END) == 3);
	CHECK(strm_find(&s, W_LONG) == 4 && strm_find(&s, W_LINE1) == 5 && strm_find(&s, W_LINE2) == 6 && strm_find(&s, W_AIR) == 7);
	CHECK(strm_find(&s, W_HUM) == -1 && strm_find(&s, W_FIRE) == -1 && strm_find(&s, 0) == -1 && strm_find(&s, 0xFFFFFFFFu) == -1);
	d = strm_get_sound(&s, 5);
	CHECK(d.tag_id == W_LINE1 && d.klass == 19 && d.kind == STRM_KIND_DIALOGUE && d.flags == 0 && d.stream_count == 3 && d.stream_first == 5);
	CHECK(d.min_dist == 2.0f && d.max_dist == 30.0f && strstr(d.name, "line1") != NULL);
	d = strm_get_sound(&s, 6);
	CHECK(d.klass == 44 && d.kind == STRM_KIND_DIALOGUE && d.stream_count == 1 && d.stream_first == 8);
	d = strm_get_sound(&s, 1);
	CHECK(d.klass == 32 && d.kind == STRM_KIND_MUSIC && d.flags == STRM_S_LOOPING && d.stream_first == 1);
	d = strm_get_sound(&s, 7);
	CHECK(d.klass == 33 && d.kind == STRM_KIND_AMBIENCE && d.flags == (STRM_S_LOOPING | STRM_S_EXTRA));
	CHECK(strm_get_sound(&s, 8).tag_id == 0 && strm_get_sound(&s, 0xFFFFFFFFu).stream_count == 0);
	/* streams */
	t = strm_get_stream(&s, 0);
	CHECK(t.channels == 2 && t.rate == 32000 && t.frames == 16000 && t.data_size == 250u * 72 && t.flags == STRM_F_RESAMPLED && t.chunks == 1);
	CHECK(t.loop_start == 0 && t.loop_end == 0 && t.gain == 1.0f && t.owner == W_INTRO && t.snr10 > 200 && t.snr10 != 0x7FFFFFFF);
	t = strm_get_stream(&s, 1);
	CHECK(t.frames == 15936 && t.flags == (STRM_F_RESAMPLED | STRM_F_LOOP) && t.loop_start == 0 && t.loop_end == 15936 && t.chunks == 4 && t.rate == 32000);
	t = strm_get_stream(&s, 2);
	CHECK(t.frames == 15936 && t.flags == (STRM_F_RESAMPLED | STRM_F_LOOP) && t.loop_end == 15936);
	t = strm_get_stream(&s, 3);
	CHECK(t.frames == 8000 && t.flags == STRM_F_RESAMPLED);
	t = strm_get_stream(&s, 4);
	CHECK(t.frames == 11648 && t.chunks == 5 && t.flags == STRM_F_RESAMPLED && t.src_bytes == 5u * 50 * 72);
	{
		strm_stream a = strm_get_stream(&s, 5), b = strm_get_stream(&s, 6), c = strm_get_stream(&s, 7);
		CHECK(a.channels == 1 && a.rate == 22050 && a.frames == 4480 && a.data_size == 70u * 36 && a.flags == 0 && a.snr10 == 0x7FFFFFFF);
		CHECK(b.frames == 2560 && b.gain == 0.5f && fabsf(b.skip - 0.1f) < 1e-6f && b.data_offset != a.data_offset);
		CHECK(c.frames == 4480 && c.data_offset == a.data_offset && c.flags == STRM_F_SHARED && c.owner == W_LINE1);
	}
	t = strm_get_stream(&s, 8);
	CHECK(t.channels == 1 && t.rate == 22050 && t.frames == 3200 && t.flags == (STRM_F_FOLDED | STRM_F_RESAMPLED) && t.owner == W_LINE2);
	t = strm_get_stream(&s, 9);
	CHECK(t.channels == 1 && t.frames == 143360 && t.flags == STRM_F_LOOP && t.loop_end == 143360 && t.src_bytes == 2240u * 36);
	CHECK(strm_get_stream(&s, 10).data_size == 0 && strm_get_stream(&s, 0xFFFFFFFFu).channels == 0);
	for (i = 0; i < 10; i++) CHECK(strm_get_stream(&s, (uint32_t)i).data_offset % 2048 == 0);
	/* loops: sorted, track structure */
	CHECK(strm_find_loop(&s, W_LOOP_M1) == 0 && strm_find_loop(&s, W_LOOP_AIR) == 1 && strm_find_loop(&s, W_LOOP_HUM) == -1 && strm_find_loop(&s, 7) == -1);
	l = strm_get_loop(&s, 0);
	CHECK(l.tag_id == W_LOOP_M1 && l.track_count == 1 && l.track_first == 0 && strstr(l.name, "m1") != NULL);
	tr = strm_get_track(&s, &l, 0);
	CHECK(tr.start == W_INTRO && tr.loop == W_LOOP && tr.end == W_END && tr.alt_loop == W_ALT && tr.alt_end == STRM_NO_TAG);
	CHECK(tr.gain == 0.75f && fabsf(tr.fade_in - 0.2f) < 1e-6f && tr.fade_out == 2.0f);
	CHECK(strm_get_track(&s, &l, 1).start == 0 && strm_get_track(&s, &l, 0xFFFFFFFFu).loop == 0);
	l = strm_get_loop(&s, 1);
	tr = strm_get_track(&s, &l, 0);
	CHECK(l.tag_id == W_LOOP_AIR && tr.start == STRM_NO_TAG && tr.loop == W_AIR && tr.end == STRM_NO_TAG && tr.fade_out == 0.5f && tr.gain == 1.0f);
	CHECK(strm_get_loop(&s, 2).track_count == 0);
	walk(&s, strd);
	return 0;
}

/* Every stream of the fixture decodes to what the Python reference decoder produced (CRC-32 of the interleaved PCM). */
static int check_crc_file(const strm_section *s, const testpack *tp, const char *crc_path, int must_match_all)
{
	FILE *f = fopen(crc_path, "r");
	char line[128];
	unsigned idx, matched = 0;
	unsigned long want;
	if (!f) { fprintf(stderr, "cannot open %s\n", crc_path); return 1; }
	while (fgets(line, sizeof line, f)) {
		strm_stream t;
		uint8_t *data;
		int16_t *pcm;
		uint32_t nblocks, b;
		if (sscanf(line, "%u %lx", &idx, &want) != 2) { fclose(f); return 1; }
		CHECK(idx < s->stream_count);
		t = strm_get_stream(s, idx);
		nblocks = t.data_size / strm_unit_bytes(t.channels);
		data = malloc(t.data_size);
		pcm = malloc((size_t)t.frames * t.channels * sizeof(int16_t));
		CHECK(data && pcm && testpack_read(tp, t.data_offset, data, t.data_size) == 0);
		for (b = 0; b < nblocks; b += 4096) strm_decode_blocks(data + (size_t)b * strm_unit_bytes(t.channels), t.channels,
		                                                         nblocks - b < 4096 ? nblocks - b : 4096, pcm + (size_t)b * STRM_BLOCK_FRAMES * t.channels);
		if (hp2_crc32((const uint8_t *)pcm, (size_t)t.frames * t.channels * sizeof(int16_t)) != (uint32_t)want) {
			fprintf(stderr, "FAIL: stream %u decodes differently from the Python reference\n", idx);
			return 1;
		}
		free(data);
		free(pcm);
		matched++;
	}
	fclose(f);
	CHECK(!must_match_all || matched == s->stream_count);
	printf("  %s: %u streams decode bit-exactly as the Python reference\n", crc_path, matched);
	return 0;
}

static int check_corruption(const testpack *tp, const uint8_t *strd)
{
	strm_section s;
	size_t cut, i, n = tp->strm_size;
	uint8_t *copy = malloc(n);
	uint32_t so, vo, lo, to_;
	static const struct { const char *what; size_t at; unsigned len; uint32_t val; } hits[] = {
		{ "magic", 0, 4, 0 }, { "version", 4, 2, 2 }, { "size", 8, 4, 1 }, { "sound count", 0x0C, 4, 0x7FFFFFFF },
		{ "stream count", 0x10, 4, 0x7FFFFFFF }, { "data size", 0x2C, 4, 1 }, { "align", 0x30, 4, 3 }, { "align zero", 0x30, 4, 0 },
	};
	CHECK(copy);
	memcpy(copy, tp->strm, n);
	so = rd32le(copy + 0x1C); vo = rd32le(copy + 0x20); lo = rd32le(copy + 0x24); to_ = rd32le(copy + 0x28);
	/* truncated at every length: rejected (only the full length opens) */
	for (cut = 0; cut < n; cut++) CHECK(strm_open(&s, copy, cut, tp->strd_size) != STRM_OK && s.sound_count == 0 && strm_find(&s, W_LINE1) == -1);
	CHECK(strm_open(&s, copy, n, tp->strd_size) == STRM_OK);
	CHECK(strm_open(&s, NULL, 100, 0) == STRM_ERR_TRUNCATED);
	CHECK(strm_open(&s, copy, n, tp->strd_size + 1) == STRM_ERR_LAYOUT && strm_open(&s, copy, n, tp->strd_size - 1) == STRM_ERR_LAYOUT);
	/* named header / field corruption */
	for (i = 0; i < sizeof hits / sizeof hits[0]; i++) {
		memcpy(copy, tp->strm, n);
		if (hits[i].len == 4) wr32le(copy + hits[i].at, hits[i].val);
		else { copy[hits[i].at] = (uint8_t)hits[i].val; copy[hits[i].at + 1] = (uint8_t)(hits[i].val >> 8); }
		CHECK(strm_open(&s, copy, n, tp->strd_size) != STRM_OK);
	}
	{
		struct { const char *what; size_t at; unsigned len; uint32_t val; } f[] = {
			{ "sound order", so + 64, 4, 0 }, { "sound kind", so + 5, 1, 9 }, { "sound flags", so + 6, 2, 0x10 }, { "sound zero streams", so + 8, 2, 0 },
			{ "sound first", so + 12, 4, 100 }, { "sound NaN", so + 16, 4, 0x7FC00000 }, { "sound name", so + 40 + 23, 1, 'x' },
			{ "stream offset", vo, 4, 0xFFFFF800u }, { "stream align", vo + 4 * 64, 4, 7 }, { "stream size", vo + 4, 4, 35 },
			{ "stream huge size", vo + 4, 4, 0xFFFFFFFFu }, { "stream frames", vo + 8, 4, 65 }, { "stream rate", vo + 12, 4, 0 },
			{ "stream rate high", vo + 12, 4, 48001 }, { "loop start", vo + 64 + 16, 4, 99999 }, { "loop end", vo + 64 + 20, 4, 0 },
			{ "loop flag", vo + 34, 2, 1 }, { "stream format", vo + 33, 1, 2 }, { "stream channels", vo + 32, 1, 0 },
			{ "stream flags", vo + 34, 2, 16 }, { "stream NaN", vo + 28, 4, 0x7F800000 }, { "loop order", lo + 64, 4, 0 },
			{ "loop tracks", lo + 12, 2, 9 }, { "loop first", lo + 16, 4, 9 }, { "loop name", lo + 32 + 31, 1, 'x' },
			{ "track NaN", to_ + 8, 4, 0x7F800001 }, { "track gain inf", to_ + 4, 4, 0x7F800000 }
		};
		for (i = 0; i < sizeof f / sizeof f[0]; i++) {
			memcpy(copy, tp->strm, n);
			if (f[i].len == 4) wr32le(copy + f[i].at, f[i].val);
			else if (f[i].len == 2) { copy[f[i].at] = (uint8_t)f[i].val; copy[f[i].at + 1] = (uint8_t)(f[i].val >> 8); }
			else copy[f[i].at] = (uint8_t)f[i].val;
			if (strm_open(&s, copy, n, tp->strd_size) == STRM_OK) { fprintf(stderr, "FAIL: %s accepted\n", f[i].what); return 1; }
			CHECK(s.sound_count == 0 && s.stream_count == 0);
			walk(&s, NULL);
		}
	}
	/* every byte flipped three ways: rejected, or safe to walk completely */
	{
		static const uint8_t xors[3] = { 0xFF, 0x01, 0x80 };
		unsigned x, accepted = 0, rejected = 0;
		for (i = 0; i < n; i++)
			for (x = 0; x < 3; x++) {
				memcpy(copy, tp->strm, n);
				copy[i] ^= xors[x];
				if (strm_open(&s, copy, n, tp->strd_size) == STRM_OK) { walk(&s, strd); accepted++; }
				else { walk(&s, NULL); rejected++; }
			}
		printf("  byte flips: %u rejected, %u accepted and walked\n", rejected, accepted);
		CHECK(rejected > accepted / 4 && accepted > 0);
	}
	free(copy);
	return 0;
}

/* Random bytes through the decoder: no crash, output defined (ASan/UBSan). */
static int check_decoder_garbage(void)
{
	uint32_t seed = 99u;
	uint8_t blk[72 * 16];
	int16_t out[STRM_BLOCK_FRAMES * 2 * 16];
	unsigned n, i;
	for (n = 0; n < 2000; n++) {
		for (i = 0; i < sizeof blk; i++) { seed = seed * 1103515245u + 12345u; blk[i] = (uint8_t)(seed >> 16); }
		strm_decode_blocks(blk, 1u + (n & 1u), 16, out);
		sink += (uint32_t)out[0];
	}
	return 0;
}

static int check_real(const char *arg)
{
	char path[512], *eq;
	testpack tp;
	strm_section s;
	size_t i, step, flips = 0;
	uint8_t *copy;
	uint32_t k;
	strm_status st;
	clock_t t0;
	double secs;
	unsigned long long samples = 0, bytes = 0;
	snprintf(path, sizeof path, "%s", arg);
	eq = strchr(path, '=');
	if (eq) *eq++ = 0;
	CHECK(testpack_open(&tp, path) == 0);
	st = strm_open(&s, tp.strm, tp.strm_size, tp.strd_size);
	if (st != STRM_OK) { fprintf(stderr, "FAIL: %s rejected (%d)\n", path, (int)st); return 1; }
	/* walk with real data on a spread of streams */
	walk(&s, NULL);
	step = s.stream_count / 200 + 1;
	t0 = clock();
	for (k = 0; k < s.stream_count; k += (uint32_t)step) {
		strm_stream t = strm_get_stream(&s, k);
		uint32_t unit = strm_unit_bytes(t.channels), nb = t.data_size / unit, b;
		uint8_t *data = malloc(t.data_size);
		int16_t *pcm = malloc((size_t)STRM_BLOCK_FRAMES * 2 * 64 * sizeof(int16_t));
		CHECK(data && pcm && testpack_read(&tp, t.data_offset, data, t.data_size) == 0);
		for (b = 0; b < nb; b += 64) {
			uint32_t n = nb - b < 64 ? nb - b : 64, j;
			strm_decode_blocks(data + (size_t)b * unit, t.channels, n, pcm);
			for (j = 0; j < n; j++) {                    /* spot check against the formula decoder */
				int16_t ref[STRM_BLOCK_FRAMES * 2];
				if ((b + j) % 97 == 0) {
					naive_block(data + (size_t)(b + j) * unit, t.channels, ref);
					CHECK(memcmp(ref, pcm + (size_t)j * STRM_BLOCK_FRAMES * t.channels, STRM_BLOCK_FRAMES * t.channels * sizeof(int16_t)) == 0);
				}
			}
			samples += (unsigned long long)n * STRM_BLOCK_FRAMES * t.channels;
			bytes += (unsigned long long)n * unit;
		}
		free(data);
		free(pcm);
	}
	secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
	printf("  %s: %u sounds, %u streams, %u loops: walked, %.1f MiB of ADPCM decoded and spot-checked\n", path, (unsigned)s.sound_count,
	       (unsigned)s.stream_count, (unsigned)s.loop_count, (double)bytes / 1048576.0);
	(void)secs; (void)samples;
	if (eq && *eq && check_crc_file(&s, &tp, eq, 0)) return 1;
	/* byte flips of the index (every 61st byte, one pattern) */
	copy = malloc(tp.strm_size);
	CHECK(copy);
	for (i = 0; i < tp.strm_size; i += 61) {
		memcpy(copy, tp.strm, tp.strm_size);
		copy[i] ^= 0xA5;
		if (strm_open(&s, copy, tp.strm_size, tp.strd_size) == STRM_OK) walk(&s, NULL);
		flips++;
	}
	printf("  %zu byte flips of the index: rejected or walked\n", flips);
	free(copy);
	testpack_close(&tp);
	return 0;
}

int main(int argc, char **argv)
{
	testpack tp;
	uint8_t *strd;
	int a;
	if (argc < 2) { fprintf(stderr, "usage: %s fixture.hp2 [real.strm.hp2[=crc] ...]\n", argv[0]); return 2; }
	CHECK(check_decoder_random() == 0);
	CHECK(check_decoder_garbage() == 0);
	CHECK(testpack_open(&tp, argv[1]) == 0);
	strd = malloc(tp.strd_size);
	CHECK(strd && testpack_read(&tp, 0, strd, tp.strd_size) == 0);
	CHECK(check_fixture(&tp, strd) == 0);
	{
		char crc[600];
		strm_section s;
		snprintf(crc, sizeof crc, "%s.crc", argv[1]);
		CHECK(strm_open(&s, tp.strm, tp.strm_size, tp.strd_size) == STRM_OK);
		CHECK(check_crc_file(&s, &tp, crc, 1) == 0);
	}
	CHECK(check_corruption(&tp, strd) == 0);
	free(strd);
	testpack_close(&tp);
	for (a = 2; a < argc; a++) CHECK(check_real(argv[a]) == 0);
	printf("test_strmdef: ok\n");
	return 0;
}
