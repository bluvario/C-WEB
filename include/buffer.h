#ifndef CWEB_BUFFER_H
#define CWEB_BUFFER_H

#include <stddef.h>

#include "sv.h"

// fixed-capacity read staging buffer: the socket layer recv()s into the free
// tail, then the request parser reads the buffered view. dropped bytes are
// compacted away so the buffer never runs dry until a whole message fits.
typedef struct {
    char *data;
    size_t count; // live bytes, always at the front of data
    size_t capacity;
} Read_Buffer;

void rb_init(Read_Buffer *b, size_t capacity);
void rb_free(Read_Buffer *b);

// view of the free tail, hand it to recv() then rb_commit what arrived
String_View rb_write_head(Read_Buffer *b);
void rb_commit(Read_Buffer *b, size_t n);

// everything buffered so far
String_View rb_view(Read_Buffer *b);

// forget the n oldest bytes, moving the rest to the front
void rb_discard(Read_Buffer *b, size_t n);

#endif