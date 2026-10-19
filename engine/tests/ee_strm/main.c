/* Console test of the streamed audio player (docs/29-strm-section.md): loads halo.cfg, opens a .strm.hp2 pack, starts a music loop, a dialogue line and a
 * looping ambience on the three slots, runs N game ticks (stream_ee_tick every 33.3 ms, stream_ee_pump in between, busy-wait pacing on the EE
 * cycle counter), prints counters every second and, at the end, PCM statistics of what was pushed to audsrv. `strm_crc 1` instead plays one dialogue and one
 * music stream alone and prints the CRC-32 of 16000 pushed frames (compare with the host: same arithmetic, same CRC).
 *
 * cfg keys (this program only): strm <pack> (default assets/hp2/a10.strm.hp2) | strm_ticks N (default 900) | strm_music / strm_dialogue / strm_ambience <tag id>
 * (default: auto pick) | strm_log 1 | strm_noplay 1 (open only) | strm_pump 0/1 (the extra pump between ticks, default 1) | strm_crc 1 |
 * strm_stress 1 (also does a blocking file read while a stream read is in flight: safe, the IO thread makes ordinary blocking fio calls). */
#include <kernel.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/audio.h"
#include "audio/stream.h"
#include "core/config.h"
#include "core/mem.h"
#include "platform/timer.h"
#include "stream/file.h"
#include "stream/hp2.h"

#define U(x) ((unsigned)(x))
static uint8_t s_pool[3u << 20] __attribute__((aligned(64)));
static stream_player s_player;
static stream_ee s_ee;
static cfg_t s_cfg;

static uint32_t cfg_id(const char *key)
{
	const char *s = cfg_str(&s_cfg, key, NULL);
	return s ? (uint32_t)strtoul(s, NULL, 0) : 0;
}

int main(void)
{
	arena_t a;
	size_t n = 0;
	uint8_t *txt;
	const char *pack;
	int ticks, t, played_music = 0, played_dialog = 0, played_amb = 0;
	uint32_t music = 0, dialog = 0, amb = 0, i;
	u32 next, t_start;
	const strm_section *sec;

	printf("[stream] strmtest start\n");
	arena_init(&a, "pool", s_pool, sizeof s_pool);
	txt = file_load(&a, "halo.cfg", &n);
	if (txt) cfg_parse(&s_cfg, (const char *)txt, n);
	pack = cfg_str(&s_cfg, "strm", "assets/hp2/a10.strm.hp2");
	ticks = (int)cfg_int(&s_cfg, "strm_ticks", 900);
	if (!audio_init()) printf("[stream] audio_init failed\n");
	if (stream_ee_open(&s_ee, &s_player, &a, pack, (int)cfg_int(&s_cfg, "strm_log", 1)) != 0) {
		printf("[stream] open failed\n[done]\n");
		return 1;
	}
	printf("[stream] main thread priority was %d, now %d%s, IO thread priority %d\n", s_ee.main_priority_orig, s_ee.main_priority, s_ee.main_demoted ? " (lowered by the glue)" : "", s_ee.main_priority - 1);
	s_ee.measure = 1;
	sec = &s_ee.sec;
	if (cfg_int(&s_cfg, "strm_noplay", 0)) { printf("[stream] opened only\n[done]\n"); return 0; }

	if (cfg_int(&s_cfg, "strm_crc", 0)) {
		static int16_t cap[2 * 24000];
		uint32_t pick[2] = { 0, 0 }, k, j;
		for (i = 0; i < sec->sound_count && !(pick[0] && pick[1]); i++) {
			strm_sound d = strm_get_sound(sec, i);
			if (!pick[0] && d.kind == STRM_KIND_DIALOGUE && d.klass == 19 && strm_get_stream(sec, d.stream_first).frames > 22050u * 3u) pick[0] = d.stream_first + 1;
			if (!pick[1] && d.kind == STRM_KIND_MUSIC) pick[1] = d.stream_first + 1;
		}
		for (k = 0; k < 2; k++) {
			uint32_t first = 0, crc;
			s_ee.capture = cap; s_ee.capture_cap = 24000; s_ee.capture_len = 0;
			memset(cap, 0, sizeof cap);
			if (!pick[k] || stream_play_stream(&s_player, (int)k, pick[k] - 1, 0, 1.0f, 0.0f, -1.0f) < 0) { printf("[stream] crc: nothing to play\n"); continue; }
			for (t = 0; t < 60 && s_ee.capture_len < 24000; t++) { next = timer_cycles() + EE_CLOCK_HZ / 30u; stream_ee_tick(&s_ee); while ((int32_t)(timer_cycles() - next) < 0) {} }
			for (j = 0; j < s_ee.capture_len && !cap[2 * j] && !cap[2 * j + 1]; j++) {}
			first = j;
			crc = hp2_crc32((const uint8_t *)(cap + 2 * first), (size_t)(16000) * 4u);
			printf("[stream] crc stream %u (slot %u): %u frames captured, audible from frame %u, crc32 of the next 16000 frames %08x\n", U(pick[k] - 1), U(k), U(s_ee.capture_len), U(first), U(crc));
			stream_kill(&s_player, (int)k);
			for (t = 0; t < 30; t++) { next = timer_cycles() + EE_CLOCK_HZ / 30u; stream_ee_tick(&s_ee); while ((int32_t)(timer_cycles() - next) < 0) {} }
		}
		s_ee.capture = NULL;
		printf("[done]\n");
		return 0;
	}
	music = cfg_id("strm_music");
	dialog = cfg_id("strm_dialogue");
	amb = cfg_id("strm_ambience");
	for (i = 0; i < sec->loop_count && !music; i++) {                       /* first music loop */
		strm_loop l = strm_get_loop(sec, i);
		strm_track tr = strm_get_track(sec, &l, 0);
		int si = strm_find(sec, tr.loop);
		if (si >= 0 && strm_get_sound(sec, (uint32_t)si).kind == STRM_KIND_MUSIC) music = l.tag_id;
	}
	for (i = 0; i < sec->sound_count && !(dialog && amb); i++) {
		strm_sound d = strm_get_sound(sec, i);
		if (!dialog && d.kind == STRM_KIND_DIALOGUE && d.klass == 19 && strm_get_stream(sec, d.stream_first).frames > 22050u * 3u) dialog = d.tag_id;
		if (!amb && d.kind == STRM_KIND_AMBIENCE) amb = d.tag_id;
	}
	printf("[stream] scenario: music loop %08x, dialogue %08x, ambience %08x, %d ticks\n", (unsigned)music, (unsigned)dialog, (unsigned)amb, ticks);

	t_start = next = timer_cycles();
	for (t = 0; t < ticks; t++) {
		const stream_stats *st;
		if (t == 0 && music) played_music = stream_play_loop(&s_player, STREAM_SLOT_MUSIC, music, 0, 1.0f, 0.0f, 1.0f) == 0;
		if (t == 45 && dialog) played_dialog = stream_play_sound(&s_player, STREAM_SLOT_DIALOGUE, dialog, 0, 0, 1.0f, 0.0f, 0.1f) >= 0;
		if (t == 120 && amb) played_amb = stream_play_sound(&s_player, STREAM_SLOT_AMBIENCE, amb, 0, STREAM_PLAY_LOOP, 0.5f, 0.0f, 0.5f) >= 0;
		if (t == ticks - 150 && music) stream_stop(&s_player, STREAM_SLOT_MUSIC, 2.0f);
		if (cfg_int(&s_cfg, "strm_stress", 0) && t == 300) {
			/* a blocking read while the stream may have a read in flight: the glue must be told first */
			uint8_t scratch[2048];
			stream_ee_sync(&s_ee);
			file_read_at(pack, 0, scratch, sizeof scratch);
		}
		stream_ee_tick(&s_ee);
		st = stream_get_stats(&s_player);
		if (t % 30 == 29)
			printf("[stream] t=%3ds reads %3u (%6u KiB) wait %2u under v%u o%u steal %u | ms last %.3f max %.3f avg %.3f audsrv max %.3f | audsrv queue %u..%u frames, pushed %u, partial %u | busy %d%d%d\n",
			       (t + 1) / 30, U(st->reads), U(st->read_bytes) / 1024u, U(st->read_wait_ticks), U(st->voice_underruns), U(st->out_underruns), U(st->steals),
			       st->ms_last, st->ms_max, st->ms_sum / (float)st->service_calls, st->ms_audsrv_max, U(st->queued_min), U(st->queued_max), U(st->pushed_frames),
			       U(st->push_partial), stream_slot_busy(&s_player, 0), stream_slot_busy(&s_player, 1), stream_slot_busy(&s_player, 2));
		next += EE_CLOCK_HZ / 60u;
		while ((int32_t)(timer_cycles() - next) < 0) {}                    /* 60 Hz pacing: the tick above runs at 30 Hz, the pump in between */
		if (cfg_int(&s_cfg, "strm_pump", 1)) stream_ee_pump(&s_ee);
		next += EE_CLOCK_HZ / 60u;
		while ((int32_t)(timer_cycles() - next) < 0) {}
	}
	{
		const stream_stats *st = stream_get_stats(&s_player);
		float n_f = (float)s_ee.measured_frames, secs = timer_ms(t_start, timer_cycles()) * 0.001f;
		int ch;
		printf("[stream] ran %d ticks in %.2f s of EE time\n", ticks, secs);
		printf("[stream] played: music %d dialogue %d ambience %d | plays %u finished %u loop passes %u format mismatches %u\n", played_music, played_dialog,
		       played_amb, U(st->plays), U(st->finished), U(st->loop_passes), U(st->format_mismatch));
		printf("[stream] read %u bytes in %u reads (%u errors, %u wait ticks), decoded %u frames, mixed %u, silence %u, underruns voice %u (%u frames) out %u, start latency max %u ticks\n",
		       U(st->read_bytes), U(st->reads), U(st->read_errors), U(st->read_wait_ticks), U(st->frames_decoded), U(st->frames_mixed), U(st->frames_silence), U(st->voice_underruns),
		       U(st->voice_underrun_frames), U(st->out_underruns), U(st->start_latency_max));
		printf("[stream] EE ms: tick (I/O+decode) %.3f per tick, pump (mix) %.3f per service call, audsrv RPCs %.3f per service call (%u calls)\n", st->ms_tick_sum / (float)st->ticks, st->ms_pump_sum / (float)st->service_calls, st->ms_aud_sum / (float)st->service_calls, U(st->service_calls));
		printf("[stream] EE ms per tick: avg %.3f max %.3f (audsrv calls max %.3f); pushed %u frames in %u calls (%u partial)\n", st->ms_sum / (float)st->service_calls,
		       st->ms_max, st->ms_audsrv_max, U(st->pushed_frames), U(st->push_calls), U(st->push_partial));
		for (ch = 0; ch < 2; ch++) {
			float mean = n_f > 0 ? (float)s_ee.sum[ch] / n_f : 0, rms = n_f > 0 ? sqrtf((float)s_ee.sumsq[ch] / n_f) : 0;
			printf("[stream] pcm %c: peak %d rms %.1f mean %.2f (%u frames)\n", ch ? 'R' : 'L', (int)s_ee.peak[ch], rms, mean, U(s_ee.measured_frames));
		}
	}
	stream_ee_close(&s_ee);
	printf("[done]\n");
	return 0;
}
