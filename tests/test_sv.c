#include <stdio.h>

#include "sv.h"

int main(void)
{
    String_View greeting = sv_from_cstr("   hello world   ");
    greeting = sv_trim(greeting);
    if (!sv_equal(greeting, sv_from_cstr("hello world"))) {
        fprintf(stderr, "trim failed\n");
        return 1;
    }

    if (!sv_starts_with(greeting, "hello")) {
        fprintf(stderr, "starts_with failed\n");
        return 1;
    }
    if (sv_starts_with(greeting, "world")) {
        fprintf(stderr, "starts_with matched wrong prefix\n");
        return 1;
    }

    String_View remains = sv_from_cstr("GET /index.html HTTP/1.1");
    String_View method = sv_chop_by_delim(&remains, ' ');
    String_View path = sv_chop_by_delim(&remains, ' ');
    String_View version = sv_chop_by_delim(&remains, ' ');
    if (!sv_equal(method, sv_from_cstr("GET")) ||
        !sv_equal(path, sv_from_cstr("/index.html")) ||
        !sv_equal(version, sv_from_cstr("HTTP/1.1")) ||
        !sv_equal(remains, sv_from_cstr(""))) {
        fprintf(stderr, "request line split failed\n");
        return 1;
    }

    String_View url = sv_from_cstr("https://example.com/a=b&c=d");
    if (!sv_consume_prefix(&url, "https://")) {
        fprintf(stderr, "consume_prefix failed\n");
        return 1;
    }
    if (!sv_equal(url, sv_from_cstr("example.com/a=b&c=d"))) {
        fprintf(stderr, "consume_prefix left wrong tail\n");
        return 1;
    }

    String_View n = sv_from_cstr("  12345 ");
    n = sv_trim(n);
    long long parsed;
    if (!sv_to_i64(n, &parsed) || parsed != 12345) {
        fprintf(stderr, "sv_to_i64 failed\n");
        return 1;
    }
    if (sv_to_i64(sv_from_cstr("12x"), &parsed) || sv_to_i64(sv_from_cstr(""), &parsed)) {
        fprintf(stderr, "sv_to_i64 accepted garbage\n");
        return 1;
    }

    printf("sv ok\n");
    return 0;
}