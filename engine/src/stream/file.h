/* Whole-file loads from host:/cdrom0: into an arena (level load only: ADR-006 forbids the heap
 * afterwards). Large reads go through fio in chunks so one call never exceeds IOP limits. */
#pragma once
#include "core/mem.h"
#include <stddef.h>
#include <stdint.h>

/* Returns a 64-byte aligned buffer holding the file, or NULL (arena full / not found). */
uint8_t *file_load(arena_t *a, const char *path, size_t *size_out);

/* Size of a file in bytes, or -1. */
long file_size_of(const char *path);
/* Read `size` bytes at `offset` into dst; returns 0 on success. */
int  file_read_at(const char *path, long offset, void *dst, size_t size);
