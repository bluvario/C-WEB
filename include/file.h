#ifndef CWEB_FILE_H
#define CWEB_FILE_H

#include <stddef.h>

// slurps a whole file into memory. returns 0 on success and hands back a
// NUL-terminated malloc'd buffer (caller owns it via xfree), -1 on failure.
int file_read_all(const char *path, char **out, size_t *out_len);

#endif