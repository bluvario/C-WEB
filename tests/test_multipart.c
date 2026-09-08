#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "multipart.h"
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

    char boundary[64];
    int br = multipart_boundary(
        sv_from_cstr("multipart/form-data; boundary=------------------------abc"),
        boundary, sizeof(boundary));
    fails += check("boundary extracted", br == 0 &&
                   strcmp(boundary, "------------------------abc") == 0);

    // a quoted boundary comes back unquoted
    br = multipart_boundary(sv_from_cstr("multipart/form-data; boundary=\"x-y\""),
                            boundary, sizeof(boundary));
    fails += check("quoted boundary unquoted", br == 0 && strcmp(boundary, "x-y") == 0);

    // a different media type has no boundary
    br = multipart_boundary(sv_from_cstr("text/plain"), boundary, sizeof(boundary));
    fails += check("non-multipart rejected", br == -1);

    const char *body =
        "--abc\r\n"
        "Content-Disposition: form-data; name=\"user\"\r\n"
        "\r\n"
        "alice\r\n"
        "--abc\r\n"
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"me.png\"\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
        "fake-png-bytes\r\n"
        "--abc--\r\n";

    Multipart m;
    fails += check("multipart parses", multipart_parse(&m, sv_from_cstr(body), "abc") == 0);

    Multipart_Part *user = multipart_get(&m, "user");
    fails += check("plain field found", user != NULL);
    if (user) {
        fails += check("plain field value", sv_equal(user->content, sv_from_cstr("alice")));
        fails += check("plain field has no filename", user->filename.count == 0);
    }

    Multipart_Part *avatar = multipart_get(&m, "avatar");
    fails += check("file part found", avatar != NULL);
    if (avatar) {
        fails += check("file part name", sv_equal(avatar->name, sv_from_cstr("avatar")));
        fails += check("file part filename", sv_equal(avatar->filename, sv_from_cstr("me.png")));
        fails += check("file part content", sv_equal(avatar->content, sv_from_cstr("fake-png-bytes")));
        const char *ct = strmap_get_cstr(&avatar->headers, "content-type");
        fails += check("part content-type kept", ct != NULL && strcmp(ct, "image/png") == 0);
    }

    fails += check("missing part is NULL", multipart_get(&m, "ghost") == NULL);
    multipart_free(&m);

    // trailing CRLF after the closing boundary is fine
    const char *body2 =
        "--b\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\nv\r\n--b--\r\n";
    fails += check("trailing CRLF tolerated", multipart_parse(&m, sv_from_cstr(body2), "b") == 0);
    Multipart_Part *k = multipart_get(&m, "k");
    fails += check("single-part value", k != NULL && sv_equal(k->content, sv_from_cstr("v")));
    multipart_free(&m);

    // a body that does not open with the boundary is malformed
    fails += check("bad preamble rejected",
                   multipart_parse(&m, sv_from_cstr("nope\r\n--b\r\n"), "b") == -1);

    // a part whose closing delimiter never shows up is malformed
    const char *body3 = "--b\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\nv";
    fails += check("unterminated part rejected",
                   multipart_parse(&m, sv_from_cstr(body3), "b") == -1);

    // empty part value is legal, empty boundary is not
    fails += check("empty boundary rejected",
                   multipart_parse(&m, sv_from_cstr("--x\r\n"), "") == -1);

    // saving a file part strips the client's directory and writes to disk
    if (mkdir("build/mp-up", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir failed\n");
        return 1;
    }
    const char *upbody =
        "--x\r\n"
        "Content-Disposition: form-data; name=\"doc\"; filename=\"photos/../up.png\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "PNGDATA\r\n"
        "--x--\r\n";
    Multipart up;
    fails += check("upload body parses", multipart_parse(&up, sv_from_cstr(upbody), "x") == 0);
    Multipart_Part *doc = multipart_get(&up, "doc");
    char out[512];
    fails += check("traversal filename flattened",
        doc != NULL && multipart_save(doc, "build/mp-up", out, sizeof(out)) == 0 &&
        strcmp(out, "build/mp-up/up.png") == 0);
    FILE *f = fopen(out, "rb");
    fails += check("saved file exists", f != NULL);
    if (f) {
        char readback[64] = {0};
        size_t got = fread(readback, 1, sizeof(readback), f);
        fclose(f);
        fails += check("saved bytes intact",
                       got == 7 && memcmp(readback, "PNGDATA", 7) == 0);
    }
    unlink(out);
    multipart_free(&up);

    // ".." and plain fields are refused without touching the disk
    const char *dotbody =
        "--x\r\nContent-Disposition: form-data; name=\"k\"; filename=\"..\"\r\n\r\nz\r\n--x--\r\n";
    Multipart dp;
    fails += check("dotdot upload parses", multipart_parse(&dp, sv_from_cstr(dotbody), "x") == 0);
    Multipart_Part *dot = multipart_get(&dp, "k");
    fails += check("dotdot filename refused",
        dot != NULL && multipart_save(dot, "build/mp-up", out, sizeof(out)) == -1);
    multipart_free(&dp);
    // plain fields carry no filename and cannot be saved
    const char *plainbody =
        "--y\r\nContent-Disposition: form-data; name=\"txt\"\r\n\r\nhi\r\n--y--\r\n";
    Multipart nf;
    fails += check("plain body parses", multipart_parse(&nf, sv_from_cstr(plainbody), "y") == 0);
    Multipart_Part *txt = multipart_get(&nf, "txt");
    fails += check("plain field cannot be saved",
        txt != NULL && multipart_save(txt, "build/mp-up", out, sizeof(out)) == -1);
    multipart_free(&nf);
    rmdir("build/mp-up");

    if (fails == 0) {
        printf("multipart ok\n");
    }
    return fails != 0;
}