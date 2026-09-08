#ifndef CWEB_FILE_H
#define CWEB_FILE_H

#include <stddef.h>
#include <time.h>

// slurps a whole file into memory. returns 0 on success and hands back a
// NUL-terminated malloc'd buffer (caller owns it via xfree), -1 on failure.
int file_read_all(const char *path, char **out, size_t *out_len);

// last-modified time of path in seconds since epoch, or -1 on any error
time_t file_mtime(const char *path);

#endif