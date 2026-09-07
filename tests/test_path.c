#include <stdio.h>
#include <string.h>

#include "path.h"
#include "strbuf.h"
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
    Strbuf out;
    strbuf_init(&out);

    char bufs[][2][64] = {
        {"/a/b/../c", "/a/c"},
        {"//a//b/", "/a/b"},
        {"/a/./b", "/a/b"},
        {"/a/..", "/"},
        {"/", "/"},
        {"a/b/c/../../d", "a/d"},
        {"a//b", "a/b"},
    };
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
        out.count = 0;
        int rc = uri_path_normalize(sv_from_cstr(bufs[i][0]), &out);
        if (rc != 0 || strbuf_null_terminate(&out) != 0) {
            fprintf(stderr, "normalize failed on %s (rc=%d)\n", bufs[i][0], rc);
            return 1;
        }
        if (strcmp(out.items, bufs[i][1]) != 0) {
            fprintf(stderr, "normalize(%s) = %s, want %s\n", bufs[i][0], out.items, bufs[i][1]);
            return 1;
        }
    }

    // traversal attempts must be rejected outright
    out.count = 0;
    fails += check("escape above root rejected",
        uri_path_normalize(sv_from_cstr("/../etc/passwd"), &out) == -1);
    fails += check("double escape rejected",
        uri_path_normalize(sv_from_cstr("/a/../../b"), &out) == -1);
    fails += check("absolute escape rejected",
        uri_path_normalize(sv_from_cstr("/a/b/../../../x"), &out) == -1);

    // relative paths may pop their own segments but not climb past start
    out.count = 0;
    fails += check("relative pop is fine",
        uri_path_normalize(sv_from_cstr("a/../b"), &out) == 0 &&
        strbuf_null_terminate(&out) == 0 &&
        strcmp(out.items, "b") == 0);
    out.count = 0;
    fails += check("relative escape rejected",
        uri_path_normalize(sv_from_cstr("../b"), &out) == -1);

    strbuf_free(&out);

    if (fails == 0) {
        printf("path ok\n");
    }
    return fails != 0;
}