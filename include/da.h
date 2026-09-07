#ifndef CWEB_DA_H
#define CWEB_DA_H

#include <stddef.h>

#include "xmem.h"

// generic dynamic array. works on any struct with fields
//   T *items; size_t count; size_t capacity;
// zero-initialize the struct ({0}) and append away.
#define da_append(da, item) \
    do { \
        if ((da)->count >= (da)->capacity) { \
            (da)->capacity = (da)->capacity ? (da)->capacity * 2 : 256; \
            (da)->items = xrealloc((da)->items, (da)->capacity * sizeof(*(da)->items)); \
        } \
        (da)->items[(da)->count++] = (item); \
    } while (0)

#define da_free(da) xfree((da)->items)

#endif