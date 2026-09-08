#include "file.h"

#include <stdio.h>
#include <sys/stat.h>

#include "xmem.h"

int file_read_all(const char *path, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    // TODO: ftell gives a long, anything past LONG_MAX bytes will wrap around
    char *buf = xmalloc((size_t)size + 1);
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        xfree(buf);
        fclose(f);
        return -1;
    }
    buf[size] = '\0';
    fclose(f);

    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

int file_stat(const char *path, time_t *mtime, size_t *size)
{
#ifdef _WIN32
    struct _stat st;
    if (_stat(path, &st) != 0) {
        return -1;
    }
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
#endif
    if (mtime) {
        *mtime = st.st_mtime;
    }
    if (size) {
        *size = (size_t)st.st_size;
    }
    return 0;
}