#ifndef CWEB_FILE_H
#define CWEB_FILE_H

#include <stddef.h>
#include <time.h>

// slurps a whole file into memory. returns 0 on success and hands back a
// NUL-terminated malloc'd buffer (caller owns it via xfree), -1 on failure.
int file_read_all(const char *path, char **out, size_t *out_len);

// reads a slice starting at offset; offset+len must lie within the file, the
// caller is trusted to have checked file_stat first. buffer handed back is
// NUL-terminated like file_read_all's.
int file_read_range(const char *path, size_t offset, size_t len,
                    char **out, size_t *out_len);

// last-modified time and size of path. returns 0 on success, -1 on error
// (either *mtime or *size may be NULL if you only need one of them).
int file_stat(const char *path, time_t *mtime, size_t *size);

#endif