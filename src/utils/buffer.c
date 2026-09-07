#include "buffer.h"

#include <string.h>

#include "xmem.h"

void rb_init(Read_Buffer *b, size_t capacity)
{
    b->data = xmalloc(capacity);
    b->count = 0;
    b->capacity = capacity;
}

void rb_free(Read_Buffer *b)
{
    xfree(b->data);
    b->data = NULL;
    b->count = 0;
    b->capacity = 0;
}

String_View rb_write_head(Read_Buffer *b)
{
    // xmalloc above guarantees capacity >= 1 for the callers we have
    return (String_View){b->data + b->count, b->capacity - b->count};
}

void rb_commit(Read_Buffer *b, size_t n)
{
    if (n > b->capacity - b->count) {
        n = b->capacity - b->count;
    }
    b->count += n;
}

String_View rb_view(Read_Buffer *b)
{
    return (String_View){b->data, b->count};
}

void rb_discard(Read_Buffer *b, size_t n)
{
    if (n > b->count) {
        n = b->count;
    }
    if (n > 0) {
        memmove(b->data, b->data + n, b->count - n);
        b->count -= n;
    }
}