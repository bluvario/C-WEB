#include <stdio.h>

#include "mime.h"
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

    fails += check("html", sv_equal(mime_for_path(sv_from_cstr("/index.html")), sv_from_cstr("text/html")));
    fails += check("upper case ext", sv_equal(mime_for_path(sv_from_cstr("/STYLE.CSS")), sv_from_cstr("text/css")));
    fails += check("png", sv_equal(mime_for_path(sv_from_cstr("/img/logo.png")), sv_from_cstr("image/png")));
    fails += check("nested dots", sv_equal(mime_for_path(sv_from_cstr("/v1.2/app.js")), sv_from_cstr("text/javascript")));
    fails += check("unknown", sv_equal(mime_for_path(sv_from_cstr("/file.xyz9")), sv_from_cstr("application/octet-stream")));
    fails += check("no extension", sv_equal(mime_for_path(sv_from_cstr("/noext")), sv_from_cstr("application/octet-stream")));
    fails += check("trailing dot", sv_equal(mime_for_path(sv_from_cstr("/file.")), sv_from_cstr("application/octet-stream")));
    fails += check("wasm", sv_equal(mime_for_path(sv_from_cstr("/mod.wasm")), sv_from_cstr("application/wasm")));

    if (fails == 0) {
        printf("mime ok\n");
    }
    return fails != 0;
}