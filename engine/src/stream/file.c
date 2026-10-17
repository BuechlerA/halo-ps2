#include "stream/file.h"
#include <stdio.h>
#include <string.h>

#define CHUNK (256u * 1024u)

/* A path without a device ("assets/hp2/a10.hp2") means host:/<path>, i.e. relative to the directory
 * ps2client was started in (and to PCSX2's host root). A bare relative fopen would resolve against
 * the ELF's own directory on ps2link. */
static const char *resolve(const char *path, char *buf, size_t cap)
{
	if (strchr(path, ':')) return path;
	snprintf(buf, cap, "host:/%s", path);
	return buf;
}

uint8_t *file_load(arena_t *a, const char *path_in, size_t *size_out)
{
	char rb[160];
	const char *path = resolve(path_in, rb, sizeof rb);
	FILE *f = fopen(path, "rb");
	long len;
	uint8_t *buf;
	size_t done = 0;
	if (!f) { printf("[err] cannot open %s\n", path); return NULL; }
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) { fclose(f); printf("[err] %s is empty\n", path); return NULL; }
	buf = arena_alloc(a, (size_t)len, 64);
	if (!buf) { fclose(f); printf("[err] %s: %ld bytes do not fit the arena (%u free)\n", path, len, (unsigned)arena_free(a)); return NULL; }
	while (done < (size_t)len) {
		size_t want = (size_t)len - done < CHUNK ? (size_t)len - done : CHUNK;
		size_t got = fread(buf + done, 1, want, f);
		if (got == 0) { fclose(f); printf("[err] short read on %s at %u/%ld\n", path, (unsigned)done, len); return NULL; }
		done += got;
	}
	fclose(f);
	*size_out = (size_t)len;
	return buf;
}

long file_size_of(const char *path_in)
{
	char rb[160];
	const char *path = resolve(path_in, rb, sizeof rb);
	FILE *f = fopen(path, "rb");
	long len;
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fclose(f);
	return len;
}

int file_read_at(const char *path_in, long offset, void *dst, size_t size)
{
	char rb[160];
	const char *path = resolve(path_in, rb, sizeof rb);
	FILE *f = fopen(path, "rb");
	size_t done = 0;
	if (!f) return -1;
	if (fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
	while (done < size) {
		size_t want = size - done < CHUNK ? size - done : CHUNK;
		size_t got = fread((uint8_t *)dst + done, 1, want, f);
		if (got == 0) { fclose(f); return -1; }
		done += got;
	}
	fclose(f);
	return 0;
}
