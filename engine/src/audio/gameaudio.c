#include "audio/gameaudio.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_PERMS 2048
static audio_sample s_handle[MAX_PERMS];
static uint8_t      s_tried[MAX_PERMS];

static float rnd01(gameaudio_t *a)
{
	uint32_t x = a->rng ? a->rng : 0x1234567u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	a->rng = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}

void gameaudio_init(gameaudio_t *a, const snd_section *snd, const uint8_t *sndd, size_t sndd_size, const fxdef_section *fx, int log)
{
	unsigned i;
	memset(a, 0, sizeof *a);
	a->snd = snd; a->sndd = sndd; a->sndd_size = sndd_size; a->fx = fx; a->log = log; a->rng = 0xC0FFEEu;
	for (i = 0; i < MAX_PERMS; i++) s_handle[i].handle = -1;
	memset(s_tried, 0, sizeof s_tried);
}

static void play_sound(gameaudio_t *a, int si, const float pos[3], const camera_t *cam, float gain)
{
	snd_sound sd = snd_get_sound(a->snd, (uint32_t)si);
	uint32_t k, global;
	snd_perm p;
	float vol = gain, pan = 0.0f;
	if (!sd.perm_count || (sd.flags & (SND_F_DROPPED | SND_F_STREAMED | SND_F_UNSUPPORTED))) { a->silent++; return; }
	if (sd.skip_fraction > 0.0f && rnd01(a) < sd.skip_fraction) return;
	k = (uint32_t)(rnd01(a) * (float)sd.perm_count);
	if (k >= sd.perm_count) k = sd.perm_count - 1;
	p = snd_get_perm(a->snd, &sd, k);
	global = sd.perm_first + k;
	if (global >= MAX_PERMS || p.sample_offset + p.size > a->sndd_size) { a->failed++; return; }
	if (!s_tried[global]) {
		s_tried[global] = 1;
		if (audio_load(&s_handle[global], (void *)(a->sndd + p.sample_offset), (int)p.size) == 0) a->loads++;
		else a->failed++;
	}
	if (pos && cam) {
		float dx = pos[0] - cam->pos[0], dy = pos[1] - cam->pos[1], dz = pos[2] - cam->pos[2];
		float d = sqrtf(dx * dx + dy * dy + dz * dz);
		float lo = sd.min_dist > 0.0f ? sd.min_dist : 3.0f, hi = sd.max_dist > lo ? sd.max_dist : lo * 6.0f;
		if (d > lo) vol *= d >= hi ? 0.0f : 1.0f - (d - lo) / (hi - lo);
		if (d > 0.01f) {                                   /* +y is Halo-left: right vector = (sin yaw, -cos yaw) */
			float rx = sinf(cam->yaw), ry = -cosf(cam->yaw);
			pan = (dx * rx + dy * ry) / d;
		}
	}
	vol *= p.gain > 0.0f ? p.gain : 1.0f;
	if (vol <= 0.01f) return;
	if (audio_play(&s_handle[global], vol > 1.0f ? 1.0f : vol, pan) >= 0) a->plays++;
	if (a->log) printf("[audio] play %s (tag %08x) perm %u vol %.2f pan %.2f\n", sd.name, (unsigned)sd.tag_id, (unsigned)k, vol, pan);
}

void gameaudio_play(gameaudio_t *a, uint32_t tag, const float pos[3], const camera_t *cam, float gain)
{
	int si;
	if (!a->snd || !tag || tag == 0xFFFFFFFFu) return;
	si = snd_find(a->snd, tag);
	if (si >= 0) { play_sound(a, si, pos, cam, gain); return; }
	if (a->fx) {
		int ei = fxdef_find_effect(a->fx, tag);
		if (ei >= 0) {
			fxd_effect e = fxdef_get_effect(a->fx, (uint32_t)ei);
			unsigned k, p;
			for (k = 0; k < e.event_count; k++) {
				fxd_event ev = fxdef_get_event(a->fx, &e, k);
				for (p = 0; p < ev.part_count; p++) {
					fxd_part part = fxdef_get_part(a->fx, &ev, p);
					if (part.kind == FXD_K_SOUND) {
						int s2 = snd_find(a->snd, part.tag_id);
						if (s2 >= 0) play_sound(a, s2, pos, cam, gain);
					}
				}
			}
		}
	}
}

void gameaudio_events(gameaudio_t *a, const game_t *g, const camera_t *cam)
{
	unsigned i, n;
	const game_event_t *ev = game_events(g, &n);
	for (i = 0; i < n; i++) {
		const game_event_t *e = &ev[i];
		switch (e->type) {
		case GE_FIRE: case GE_EMPTY: case GE_RELOAD: case GE_READY: case GE_OVERHEAT: case GE_THROW: case GE_ZOOM: case GE_PICKUP:
			if (e->tag) gameaudio_play(a, e->tag, NULL, cam, 1.0f);       /* the player's own sounds are not positional */
			break;
		case GE_IMPACT: case GE_DETONATE:
			if (e->tag) gameaudio_play(a, e->tag, e->pos, cam, 1.0f);
			break;
		default: break;
		}
	}
}

unsigned gameaudio_preload(gameaudio_t *a)
{
	unsigned si, k, n = 0;
	if (!a->snd) return 0;
	for (si = 0; si < a->snd->sound_count; si++) {
		snd_sound sd = snd_get_sound(a->snd, si);
		if (sd.flags & (SND_F_DROPPED | SND_F_STREAMED | SND_F_UNSUPPORTED)) continue;
		for (k = 0; k < sd.perm_count; k++) {
			snd_perm p = snd_get_perm(a->snd, &sd, k);
			uint32_t global = sd.perm_first + k;
			if (global >= MAX_PERMS || s_tried[global] || p.sample_offset + p.size > a->sndd_size) continue;
			s_tried[global] = 1;
			if (audio_load(&s_handle[global], (void *)(a->sndd + p.sample_offset), (int)p.size) == 0) { a->loads++; n++; }
			else a->failed++;
		}
	}
	return n;
}

void gameaudio_hud_sound(gameaudio_t *a, const huddef_section *hud, uint32_t hud_tag, uint32_t mask)
{
	int hi = hud ? huddef_find_hud(hud, hud_tag) : -1;
	huddef_hud h;
	unsigned i;
	if (hi < 0 || !a->snd) return;
	h = huddef_get_hud(hud, (uint32_t)hi);
	for (i = 0; i < h.sound_count; i++) {
		huddef_sound sd = huddef_get_sound(hud, (uint32_t)h.sound_first + i);
		if ((sd.latched_to & mask) && snd_find(a->snd, sd.sound_tag) >= 0) { gameaudio_play(a, sd.sound_tag, NULL, NULL, sd.scale > 0.0f && sd.scale <= 1.0f ? sd.scale : 1.0f); return; }
	}
}
