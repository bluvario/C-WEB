#ifndef CWEB_XMEM_H
#define CWEB_XMEM_H

#include <stddef.h>

// allocations that print a loud message and abort instead of returning NULL.
// an OOM deep inside a request handler is a crash anyway, better to die
// cleanly right here than to return NULL and segfault three layers up.
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t size);
void xfree(void *p);

#endif