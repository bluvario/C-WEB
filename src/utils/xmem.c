#include "xmem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void oom(size_t size)
{
    fprintf(stderr, "FATAL: out of memory, tried to allocate %zu bytes\n", size);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        oom(n);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        oom(SIZE_MAX);
    }
    void *p = calloc(n, size);
    if (!p) {
        oom(n * size);
    }
    return p;
}

void *xrealloc(void *p, size_t size)
{
    if (size == 0) {
        free(p);
        return NULL;
    }
    void *m = realloc(p, size);
    if (!m) {
        oom(size);
    }
    return m;
}

void xfree(void *p)
{
    free(p);
}