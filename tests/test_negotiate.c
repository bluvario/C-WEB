#include <stdio.h>

#include "negotiate.h"
#include "sv.h"

int main(void)
{
    Accept_List list;
    static String_View avail[] = {
        {"text/html", sizeof("text/html") - 1},
        {"text/css", sizeof("text/css") - 1},
        {"image/png", sizeof("image/png") - 1},
        {"application/json", sizeof("application/json") - 1},
        {"audio/basic", sizeof("audio/basic") - 1},
    };
#define N (sizeof(avail) / sizeof(avail[0]))

    // plain favourites: text/html wins over everything patched in
    if (accept_parse(&list, sv_from_cstr("text/html, application/xhtml+xml;q=0.8, */*;q=0.1")) != 0) {
        fprintf(stderr, "basic header failed to parse\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 0) {
        fprintf(stderr, "text/html should win\n");
        return 1;
    }
    accept_free(&list);

    // only wildcard: best is the first available type that matches
    if (accept_parse(&list, sv_from_cstr("*/*")) != 0) {
        fprintf(stderr, "wildcard header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 0) {
        fprintf(stderr, "wildcard should match first\n");
        return 1;
    }
    accept_free(&list);

    // subtype wildcard still loses to an exact offer with full quality
    if (accept_parse(&list, sv_from_cstr("text/*;q=0.5, image/png")) != 0) {
        fprintf(stderr, "subtype header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 2) {
        fprintf(stderr, "image/png should win on q=1 against text/*\n");
        return 1;
    }
    accept_free(&list);

    // a q=0 exclusion beats a wildcard: the most specific range decides
    if (accept_parse(&list, sv_from_cstr("audio/*;q=0, audio/basic")) != 0) {
        fprintf(stderr, "exclusion header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 4) {
        fprintf(stderr, "audio/basic must survive audio/*;q=0\n");
        return 1;
    }
    accept_free(&list);

    // an explicitly killed type is gone even with a permissive wildcard
    if (accept_parse(&list, sv_from_cstr("text/plain;q=0, */*")) != 0) {
        fprintf(stderr, "killed-type header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 0) {
        fprintf(stderr, "next best should win after text/plain exclusion\n");
        return 1;
    }
    accept_free(&list);

    // nothing at all is acceptable
    if (accept_parse(&list, sv_from_cstr("image/gif;q=0, image/png;q=0")) != 0) {
        fprintf(stderr, "all-excluded header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != -1) {
        fprintf(stderr, "all excluded should be -1\n");
        return 1;
    }
    accept_free(&list);

    // whitespace and odd q spellings are tolerated
    if (accept_parse(&list, sv_from_cstr("text/html ; q= 0.5 , text/css;q=0.8")) != 0) {
        fprintf(stderr, "sloppy header failed to parse\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 1) {
        fprintf(stderr, "text/css should win the sloppy header\n");
        return 1;
    }
    accept_free(&list);

    // media types compare case-insensitively
    if (accept_parse(&list, sv_from_cstr("TEXT/HTML, image/png")) != 0) {
        fprintf(stderr, "case header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 0) {
        fprintf(stderr, "case-insensitive match failed\n");
        return 1;
    }
    accept_free(&list);

    // an empty list means the caller should assume anything goes
    if (accept_parse(&list, sv_from_cstr("")) != 0) {
        fprintf(stderr, "empty header failed\n");
        return 1;
    }
    if (accept_best(&list, avail, N) != 0) {
        fprintf(stderr, "empty accept should pick first\n");
        return 1;
    }
    accept_free(&list);

    // malformed q is rejected
    if (accept_parse(&list, sv_from_cstr("text/html;q=banana")) == 0) {
        fprintf(stderr, "bad q should fail the parse\n");
        return 1;
    }
    accept_free(&list);

    printf("negotiate ok\n");
    return 0;
}