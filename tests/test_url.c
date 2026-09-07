#include <stdio.h>
#include <string.h>

#include "strbuf.h"
#include "url.h"

int main(void)
{
    Strbuf sb;
    strbuf_init(&sb);

    // '+' is the space form used in query strings
    if (url_decode_into(&sb, sv_from_cstr("a%20b+c%D0%9F")) != 0) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }
    if (strbuf_null_terminate(&sb) != 0) {
        fprintf(stderr, "null_terminate failed\n");
        return 1;
    }
    if (strcmp(sb.items, "a b c\xD0\x9F") != 0) {
        fprintf(stderr, "decode gave: %s\n", sb.items);
        return 1;
    }
    strbuf_free(&sb);

    strbuf_init(&sb);
    url_encode_into(&sb, sv_from_cstr("a b/c?&d=e~"));
    if (strbuf_null_terminate(&sb) != 0) {
        fprintf(stderr, "null_terminate failed\n");
        return 1;
    }
    if (strcmp(sb.items, "a%20b%2Fc%3F%26d%3De~") != 0) {
        fprintf(stderr, "encode gave: %s\n", sb.items);
        return 1;
    }
    strbuf_free(&sb);

    // roundtrip
    strbuf_init(&sb);
    url_encode_into(&sb, sv_from_cstr("x y&z=1"));
    if (strbuf_null_terminate(&sb) != 0) return 1;
    Strbuf back;
    strbuf_init(&back);
    if (url_decode_into(&back, sv_from_cstr(sb.items)) != 0) {
        fprintf(stderr, "roundtrip decode failed\n");
        return 1;
    }
    if (strbuf_null_terminate(&back) != 0) return 1;
    if (strcmp(back.items, "x y&z=1") != 0) {
        fprintf(stderr, "roundtrip gave: %s\n", back.items);
        return 1;
    }
    strbuf_free(&sb);
    strbuf_free(&back);

    // malformed sequences must be rejected
    if (url_decode_into(&sb, sv_from_cstr("%2")) == 0) {
        fprintf(stderr, "accepted truncated %%xx\n");
        return 1;
    }
    if (url_decode_into(&sb, sv_from_cstr("%zz")) == 0) {
        fprintf(stderr, "accepted non-hex %%xx\n");
        return 1;
    }

    printf("url ok\n");
    return 0;
}