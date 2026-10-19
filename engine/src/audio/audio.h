/* EE-side audio client (docs/03 §8, ADR-030): the IOP runs ps2sdk's stock audsrv.irx (embedded in the ELF, loaded from a buffer so
 * neither PCSX2 nor the console needs host files); sound permutations are APCM blobs (16-byte header + SPU2 ADPCM blocks,
 * tools/halo2ps2/soundconv.py) uploaded to SPU2 RAM once per level and played on pooled voices. All calls are no-ops when audio
 * could not be initialised, so the game runs silent instead of failing. */
#ifndef AUDIO_AUDIO_H
#define AUDIO_AUDIO_H
#include <stdint.h>

typedef struct { int16_t handle; } audio_sample;       /* handle < 0: not loaded */

/* Loads libsd (ROM) and audsrv, starts the SPU2 ADPCM unit. Returns 1 if sound is available. */
int  audio_init(void);
int  audio_ready(void);
/* Uploads one APCM blob (size in bytes); returns 0 on success. The blob is copied by the IOP: the caller's buffer may be reused. */
int  audio_load(audio_sample *s, void *apcm, int size);
void audio_free(audio_sample *s);
/* Plays on a free voice at volume 0..1 and pan -1 (left) .. 1 (right). Returns the voice/channel or -1. */
int  audio_play(const audio_sample *s, float volume, float pan);
void audio_set_master(float volume);
unsigned audio_samples_loaded(void);
unsigned audio_plays(void);

#endif
