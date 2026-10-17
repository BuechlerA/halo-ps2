/* Game audio: maps game events to sounds. Sound tags (snd!) resolve through SND0; the permutation data is uploaded to SPU2 on first
 * use (SNDD holds ready-made audsrv samples) and cached; effects are searched for sound parts (FXD0). 3D: volume from the distance
 * to the sound's min/max range, pan from the bearing relative to the camera. EE only (needs audsrv). */
#ifndef AUDIO_GAMEAUDIO_H
#define AUDIO_GAMEAUDIO_H
#include "audio/audio.h"
#include "audio/snddef.h"
#include "render/camera.h"
#include "sim/game.h"
#include "ui/huddef.h"
#include "world/fxdef.h"

typedef struct {
	const snd_section   *snd;
	const uint8_t       *sndd;
	size_t               sndd_size;
	const fxdef_section *fx;
	uint32_t             rng;
	unsigned             plays, loads, silent, failed;
	int                  log;
} gameaudio_t;

void gameaudio_init(gameaudio_t *a, const snd_section *snd, const uint8_t *sndd, size_t sndd_size, const fxdef_section *fx, int log);
/* Uploads every resident permutation to SPU2 now (level load) so the first shot does not stall on the IOP. Returns the count. */
unsigned gameaudio_preload(gameaudio_t *a);
/* Plays `tag` (a sound tag, or an effect tag whose sound parts are played) at `pos` (NULL: not positional). */
void gameaudio_play(gameaudio_t *a, uint32_t tag, const float pos[3], const camera_t *cam, float gain);
/* Plays the first sound of unit HUD `hud_tag` whose latched-to mask intersects `mask` (shield recharging 1, damaged 2, low 4, empty 8, health low 16, empty 32). */
void gameaudio_hud_sound(gameaudio_t *a, const huddef_section *hud, uint32_t hud_tag, uint32_t mask);
void gameaudio_events(gameaudio_t *a, const game_t *g, const camera_t *cam);

#endif
