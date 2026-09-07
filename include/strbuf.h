#ifndef CWEB_STRBUF_H
#define CWEB_STRBUF_H

#include <stddef.h>

typedef struct {
    char *items;
    size_t count;
    size_t capacity;
} Strbuf;

void strbuf_init(Strbuf *sb);
void strbuf_free(Strbuf *sb);

// all appends return 0 on success, -1 on allocation failure
int strbuf_append_char(Strbuf *sb, char c);
int strbuf_append(Strbuf *sb, const char *data, size_t len);
int strbuf_append_cstr(Strbuf *sb, const char *data);

// writes a trailing NUL (not counted) so the buffer can be handed to C strings
int strbuf_null_terminate(Strbuf *sb);

#endif