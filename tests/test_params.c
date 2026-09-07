#include <stdio.h>
#include <string.h>

#include "params.h"
#include "strmap.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    Str_Map m;
    strmap_init(&m);

    fails += check("plain query parses", params_parse_into(&m, sv_from_cstr("name=alice&age=30")) == 0);
    fails += check("name decoded", strcmp(strmap_get_cstr(&m, "name"), "alice") == 0);
    fails += check("age decoded", strcmp(strmap_get_cstr(&m, "age"), "30") == 0);

    fails += check("percent and plus decoding",
        params_parse_into(&m, sv_from_cstr("tag=c%2B%2B&q=a+b&s=%D0%9F")) == 0);
    fails += check("c++ decoded", strcmp(strmap_get_cstr(&m, "tag"), "c++") == 0);
    fails += check("plus is space", strcmp(strmap_get_cstr(&m, "q"), "a b") == 0);
    fails += check("utf8 decoded", strcmp(strmap_get_cstr(&m, "s"), "\xD0\x9F") == 0);

    fails += check("duplicate keys overwrite", params_parse_into(&m, sv_from_cstr("a=1&a=2")) == 0 &&
        strcmp(strmap_get_cstr(&m, "a"), "2") == 0);
    fails += check("value-less key is empty", params_parse_into(&m, sv_from_cstr("flag")) == 0 &&
        strcmp(strmap_get_cstr(&m, "flag"), "") == 0);
    fails += check("empty pairs skipped", params_parse_into(&m, sv_from_cstr("x=1&&y=2")) == 0 &&
        strcmp(strmap_get_cstr(&m, "y"), "2") == 0);

    fails += check("malformed percent rejected", params_parse_into(&m, sv_from_cstr("x=%zz")) == -1);
    fails += check("truncated percent rejected", params_parse_into(&m, sv_from_cstr("x=%2")) == -1);

    strmap_free(&m);

    if (fails == 0) {
        printf("params ok\n");
    }
    return fails != 0;
}