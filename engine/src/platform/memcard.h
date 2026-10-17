/* Memory card saves (libmc over the ROM MCMAN/MCSERV modules): one file per slot in /HALOPS2/. Blocking calls (a save is rare: at checkpoints). */
#ifndef PLATFORM_MEMCARD_H
#define PLATFORM_MEMCARD_H
#include <stddef.h>

/* Loads the modules (after pad_init loaded SIO2MAN) and starts libmc. Returns 1 if a formatted card answers in slot 1. */
int  memcard_init(int format_blank);        /* format_blank: format an unformatted card (test runs on a fresh emulator card only) */
/* Writes `n` bytes to /HALOPS2/SAVE<slot>; returns 0 on success, negative on a card error. */
int  memcard_save(int slot, const void *data, size_t n);
/* Reads the slot into `buf` (capacity `cap`); returns the size, 0 if the slot does not exist, negative on error. */
long memcard_load(int slot, void *buf, size_t cap);

#endif
