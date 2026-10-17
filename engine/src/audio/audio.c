#include "audio/audio.h"
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>
#include <sbv_patches.h>
#include <stdio.h>
#include <string.h>

extern unsigned char audsrv_irx[];
extern unsigned int  size_audsrv_irx;

#define MAX_SAMPLES 640
static audsrv_adpcm_t s_sample[MAX_SAMPLES];
static unsigned char  s_used[MAX_SAMPLES];
static int      s_ready;
static unsigned s_loaded, s_plays;

int audio_ready(void) { return s_ready; }
unsigned audio_samples_loaded(void) { return s_loaded; }
unsigned audio_plays(void) { return s_plays; }

int audio_init(void)
{
	int ret, id;
	SifInitRpc(0);
	sbv_patch_enable_lmb();                     /* SifExecModuleBuffer needs the LOADFILE buffer-load patch on stock BIOSes */
	ret = SifLoadModule("rom0:LIBSD", 0, NULL);
	if (ret < 0) { printf("[audio] LIBSD load failed (%d), silent\n", ret); return 0; }
	ret = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &id);
	if (ret < 0 || id < 0) { printf("[audio] audsrv.irx load failed (%d/%d), silent\n", ret, id); return 0; }
	ret = audsrv_init();
	if (ret != 0) { printf("[audio] audsrv_init failed: %s, silent\n", audsrv_get_error_string()); return 0; }
	if (audsrv_adpcm_init() != 0) { printf("[audio] adpcm unit init failed, silent\n"); return 0; }
	audsrv_set_volume(MAX_VOLUME);
	s_ready = 1;
	printf("[audio] ready\n");
	return 1;
}

int audio_load(audio_sample *s, void *apcm, int size)
{
	int i;
	s->handle = -1;
	if (!s_ready) return -1;
	for (i = 0; i < MAX_SAMPLES && s_used[i]; i++) {}
	if (i == MAX_SAMPLES) return -1;
	if (audsrv_load_adpcm(&s_sample[i], apcm, size) != 0) return -1;
	s_used[i] = 1; s->handle = (int16_t)i; s_loaded++;
	return 0;
}

void audio_free(audio_sample *s)
{
	if (!s_ready || s->handle < 0) return;
	audsrv_free_adpcm(&s_sample[s->handle]);
	s_used[s->handle] = 0; s->handle = -1;
	if (s_loaded) s_loaded--;
}

int audio_play(const audio_sample *s, float volume, float pan)
{
	int ch, vol, p;
	if (!s_ready || s->handle < 0) return -1;
	ch = audsrv_ch_play_adpcm(-1, &s_sample[s->handle]);
	if (ch < 0) return -1;
	vol = (int)(volume * (float)MAX_VOLUME); if (vol < 0) vol = 0; if (vol > MAX_VOLUME) vol = MAX_VOLUME;
	p = (int)(pan * 100.0f); if (p < -100) p = -100; if (p > 100) p = 100;
	audsrv_adpcm_set_volume_and_pan(ch, vol, p);
	s_plays++;
	return ch;
}

void audio_set_master(float volume)
{
	int v = (int)(volume * (float)MAX_VOLUME);
	if (s_ready) audsrv_set_volume(v < 0 ? 0 : v > MAX_VOLUME ? MAX_VOLUME : v);
}
