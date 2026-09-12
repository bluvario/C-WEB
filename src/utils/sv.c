#include "sv.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

String_View sv_from_cstr(const char *s)
{
    return (String_View){.data = s, .count = strlen(s)};
}

String_View sv_trim_left(String_View sv)
{
    while (sv.count > 0 && isspace((unsigned char)sv.data[0])) {
        sv.data++;
        sv.count--;
    }
    return sv;
}

String_View sv_trim_right(String_View sv)
{
    while (sv.count > 0 && isspace((unsigned char)sv.data[sv.count - 1])) {
        sv.count--;
    }
    return sv;
}

String_View sv_trim(String_View sv)
{
    return sv_trim_right(sv_trim_left(sv));
}

bool sv_equal(String_View a, String_View b)
{
    if (a.count != b.count) {
        return false;
    }
    if (a.count == 0) {
        return true;
    }
    return memcmp(a.data, b.data, a.count) == 0;
}

bool sv_starts_with(String_View sv, const char *prefix)
{
    size_t n = strlen(prefix);
    return sv.count >= n && memcmp(sv.data, prefix, n) == 0;
}

bool sv_consume_prefix(String_View *sv, const char *prefix)
{
    size_t n = strlen(prefix);
    if (sv->count >= n && memcmp(sv->data, prefix, n) == 0) {
        sv->data += n;
        sv->count -= n;
        return true;
    }
    return false;
}

String_View sv_chop_by_delim(String_View *sv, char delim)
{
    size_t i = 0;
    while (i < sv->count && sv->data[i] != delim) {
        i++;
    }
    String_View result = {sv->data, i};
    if (i < sv->count) {
        sv->count -= i + 1;
        sv->data += i + 1;
    } else {
        sv->count -= i;
        sv->data += i;
    }
    return result;
}

bool sv_to_i64(String_View sv, long long *out)
{
    if (sv.count == 0) {
        return false;
    }
    bool neg = sv.data[0] == '-';
    size_t start = neg ? 1 : 0;
    if (start >= sv.count) {
        return false;
    }
    long long n = 0;
    for (size_t i = start; i < sv.count; i++) {
        char c = sv.data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        int d = c - '0';
        if (n > (LLONG_MAX - d) / 10) {
            return false;
        }
        n = n * 10 + d;
    }
    *out = neg ? -n : n;
    return true;
}

size_t sv_count_char(String_View sv, char c)
{
    size_t n = 0;
    for (size_t i = 0; i < sv.count; i++) {
        if (sv.data[i] == c) {
            n++;
        }
    }
    return n;
}