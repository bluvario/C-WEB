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

    // a multipart body merges its plain fields, quietly ignoring file parts
    Http_Request req;
    Str_Map merged;
    strmap_init(&merged);
    const char *body =
        "--bb\r\n"
        "Content-Disposition: form-data; name=\"user\"\r\n"
        "\r\n"
        "bob\r\n"
        "--bb\r\n"
        "Content-Disposition: form-data; name=\"pic\"; filename=\"a.png\"\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
        "bytes\r\n"
        "--bb--\r\n";
    Request_Parse_Result pr = http_request_parse(&req, sv_from_cstr(
        "POST /x HTTP/1.1\r\nHost: h\r\n"
        "Content-Type: multipart/form-data; boundary=bb\r\n"
        "\r\n"));
    fails += check("multipart request parses", pr == REQ_OK);
    req.body = sv_from_cstr(body);
    fails += check("multipart merge ok", request_merge_params(&req, &merged) == 0);
    fails += check("multipart field merged",
        strcmp(strmap_get_cstr(&merged, "user"), "bob") == 0);
    fails += check("file part not merged", strmap_get_cstr(&merged, "pic") == NULL);
    strmap_free(&merged);
    http_request_free(&req);

    // mulipart with a malformed body surfaces as an error
    strmap_init(&merged);
    pr = http_request_parse(&req, sv_from_cstr(
        "POST /x HTTP/1.1\r\nHost: h\r\n"
        "Content-Type: multipart/form-data; boundary=zz\r\n"
        "\r\n"));
    fails += check("upload request parses", pr == REQ_OK);
    req.body = sv_from_cstr("--nope\r\n\r\n");
    fails += check("malformed multipart rejected", request_merge_params(&req, &merged) == -1);
    strmap_free(&merged);
    http_request_free(&req);

    if (fails == 0) {
        printf("params ok\n");
    }
    return fails != 0;
}