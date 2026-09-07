#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

#include "xmem.h"

#define SB_MIN_CAP 256

void strbuf_init(Strbuf *sb)
{
    sb->items = NULL;
    sb->count = 0;
    sb->capacity = 0;
}

void strbuf_free(Strbuf *sb)
{
    free(sb->items);
    strbuf_init(sb);
}

static int sb_grow_to(Strbuf *sb, size_t want)
{
    size_t cap = sb->capacity ? sb->capacity : SB_MIN_CAP;
    while (cap < want) {
        cap *= 2;
    }
    // xrealloc aborts on OOM, so failure is not actually possible
    char *mem = xrealloc(sb->items, cap);
    sb->items = mem;
    sb->capacity = cap;
    return 0;
}

int strbuf_append(Strbuf *sb, const char *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (sb->count + len > sb->capacity) {
        if (sb_grow_to(sb, sb->count + len) != 0) {
            return -1;
        }
    }
    memcpy(sb->items + sb->count, data, len);
    sb->count += len;
    return 0;
}

int strbuf_append_char(Strbuf *sb, char c)
{
    return strbuf_append(sb, &c, 1);
}

int strbuf_append_cstr(Strbuf *sb, const char *data)
{
    return strbuf_append(sb, data, strlen(data));
}

int strbuf_null_terminate(Strbuf *sb)
{
    if (sb->count + 1 > sb->capacity && sb_grow_to(sb, sb->count + 1) != 0) {
        return -1;
    }
    sb->items[sb->count] = '\0';
    return 0;
}