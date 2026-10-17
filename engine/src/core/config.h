/* Tiny "key value..." config parser for the optional host:halo.cfg boot file (debug/test runs:
 * pack paths, scripted camera, frame count, framebuffer dump frames). One entry per line, '#'
 * starts a comment, keys may repeat. Pure C11, no heap, host-tested. */
#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H
#include <stddef.h>

#define CFG_MAX_ENTRIES 48
#define CFG_KEY_LEN 24
#define CFG_ARG_LEN 120

typedef struct {
	char key[CFG_KEY_LEN];
	char arg[CFG_ARG_LEN];
} cfg_entry;

typedef struct {
	int       count;
	cfg_entry entry[CFG_MAX_ENTRIES];
} cfg_t;

/* Returns the number of lines ignored (too long / table full / key too long). */
int cfg_parse(cfg_t *c, const char *text, size_t len);
/* n-th (0-based) value of `key`, or NULL. */
const char *cfg_nth(const cfg_t *c, const char *key, int n);
const char *cfg_str(const cfg_t *c, const char *key, const char *def);
long        cfg_int(const cfg_t *c, const char *key, long def);
/* Parses up to `max` whitespace-separated floats from the first `key` line; returns how many. */
int         cfg_floats(const cfg_t *c, const char *key, float *out, int max);

#endif
