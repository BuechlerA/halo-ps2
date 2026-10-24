/* Player settings and campaign progress that outlive a run (docs/39-main-menu.md): the look controls of the Xbox's "advanced controller setup" (invert thumbstick, look sensitivity 1 .. 10),
 * the volume, and which campaign levels were finished. 16 bytes with a checksum, kept on the memory card (slot SETTINGS_SLOT) by main.c. Pure C11, host-tested. */
#ifndef SIM_SETTINGS_H
#define SIM_SETTINGS_H
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_SLOT 15                 /* /HALOPS2/SAVE15 */
#define SETTINGS_BYTES 16
#define SETTINGS_SENS_DEFAULT 3
#define SETTINGS_VOLUME_DEFAULT 8

typedef struct {
	uint8_t  invert;                     /* 1: pushing the right stick up looks down */
	uint8_t  sens;                       /* 1 .. 10; SETTINGS_SENS_DEFAULT scales the look input by 1 */
	uint8_t  volume;                     /* 0 .. 10 */
	uint8_t  last_diff;                  /* the difficulty last played (0 .. 3) */
	uint16_t completed;                  /* bit n: campaign level n was finished */
} settings_t;

void   settings_default(settings_t *s);
void   settings_clamp(settings_t *s);
size_t settings_pack(const settings_t *s, uint8_t out[SETTINGS_BYTES]);
int    settings_unpack(settings_t *s, const uint8_t *data, size_t n);        /* 1 = valid (magic, version, checksum), else s holds the defaults */
/* The playable campaign levels given the packs that exist: level 0 always, level n once level n - 1 is finished. */
uint16_t settings_unlocked(const settings_t *s, uint16_t installed);

#endif
