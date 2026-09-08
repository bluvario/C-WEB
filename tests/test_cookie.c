#include <stdio.h>
#include <string.h>

#include "cookie.h"
#include "strbuf.h"
#include "strmap.h"
#include "sv.h"

int main(void)
{
    Str_Map jar;
    strmap_init(&jar);

    http_cookie_parse(&jar, sv_from_cstr("a=1; b=two words; c=\"quoted val\"; ; empty=x"));
    const char *v;
    v = strmap_get_cstr(&jar, "a");
    if (v == NULL || strcmp(v, "1") != 0) {
        fprintf(stderr, "a wrong: %s\n", v ? v : "(null)");
        return 1;
    }
    v = strmap_get_cstr(&jar, "b");
    if (v == NULL || strcmp(v, "two words") != 0) {
        fprintf(stderr, "b wrong: %s\n", v ? v : "(null)");
        return 1;
    }
    v = strmap_get_cstr(&jar, "c");
    if (v == NULL || strcmp(v, "quoted val") != 0) {
        fprintf(stderr, "quoted c wrong: %s\n", v ? v : "(null)");
        return 1;
    }
    v = strmap_get_cstr(&jar, "empty");
    if (v == NULL || strcmp(v, "x") != 0) {
        fprintf(stderr, "empty=x wrong: %s\n", v ? v : "(null)");
        return 1;
    }
    strmap_free(&jar);

    // duplicate names: the last one wins
    strmap_init(&jar);
    http_cookie_parse(&jar, sv_from_cstr("sid=a; sid=b"));
    v = strmap_get_cstr(&jar, "sid");
    if (v == NULL || strcmp(v, "b") != 0) {
        fprintf(stderr, "duplicate sid wrong: %s\n", v ? v : "(null)");
        return 1;
    }
    strmap_free(&jar);

    Http_Response res;
    http_response_init(&res);

    http_response_set_cookie(&res, "sid", "abc123", NULL);
    Cookie_Attrs attrs = {"admin", 3600, true, true};
    http_response_set_cookie(&res, "theme", "dark", &attrs);

    Strbuf wire;
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    const char *expect =
        "Set-Cookie: sid=abc123\r\n"
        "Set-Cookie: theme=dark; Path=admin; Max-Age=3600; HttpOnly; Secure\r\n";
    if (strstr(wire.items, expect) == NULL) {
        fprintf(stderr, "Set-Cookie wire wrong:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);

    printf("cookie ok\n");
    return 0;
}