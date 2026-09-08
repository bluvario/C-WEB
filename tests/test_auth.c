#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "base64.h"
#include "response.h"
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

    // build "alice:s3cret" through the real encoder so the test cannot drift
    // from the function it is meant to exercise
    Strbuf encoded;
    strbuf_init(&encoded);
    base64_encode_into(&encoded, sv_from_cstr("alice:s3cret"));
    strbuf_null_terminate(&encoded);
    char header[256];
    snprintf(header, sizeof(header), "Basic %s", encoded.items);
    strbuf_free(&encoded);

    char user[64], pass[64];
    fails += check("valid basic header parses",
        http_basic_auth_parse(sv_from_cstr(header), user, sizeof(user),
                              pass, sizeof(pass)) == 0);
    fails += check("user decoded", strcmp(user, "alice") == 0);
    fails += check("pass decoded", strcmp(pass, "s3cret") == 0);

    // scheme check is case-insensitive
    char lc[256];
    lc[0] = 'b';
    strcpy(lc + 1, header + 1);
    fails += check("lowercase scheme accepted",
        http_basic_auth_parse(sv_from_cstr(lc), user, sizeof(user),
                              pass, sizeof(pass)) == 0);

    // passwords are allowed to be empty
    Strbuf e2;
    strbuf_init(&e2);
    base64_encode_into(&e2, sv_from_cstr("bob:"));
    strbuf_null_terminate(&e2);
    char header2[256];
    snprintf(header2, sizeof(header2), "Basic %s", e2.items);
    fails += check("empty password allowed",
        http_basic_auth_parse(sv_from_cstr(header2), user, sizeof(user),
                              pass, sizeof(pass)) == 0 &&
        strcmp(pass, "") == 0);
    strbuf_free(&e2);

    // wrong scheme, garbage base64 and a missing colon all fail
    fails += check("wrong scheme rejected",
        http_basic_auth_parse(sv_from_cstr("Bearer token"), user, sizeof(user),
                              pass, sizeof(pass)) == -1);
    fails += check("empty scheme rejected",
        http_basic_auth_parse(sv_from_cstr("Basic "), user, sizeof(user),
                              pass, sizeof(pass)) == -1);
    fails += check("no colon rejected",
        http_basic_auth_parse(sv_from_cstr("Basic YWxpY2U="), user, sizeof(user),
                              pass, sizeof(pass)) == -1);
    fails += check("invalid base64 rejected",
        http_basic_auth_parse(sv_from_cstr("Basic %%%%"), user, sizeof(user),
                              pass, sizeof(pass)) == -1);

    // the challenge wires up a 401 with the realm echoed correctly
    Http_Response res;
    http_response_init(&res);
    http_response_require_basic_auth(&res, "secure \"area\"");
    if (res.status != HTTP_401_UNAUTHORIZED || res.body.count == 0) {
        fprintf(stderr, "401 challenge wrong\n");
        return 1;
    }
    if (strstr(res.headers.items, "WWW-Authenticate: Basic realm=\"secure \\\"area\\\"\"\r\n") == NULL) {
        fprintf(stderr, "challenge header wrong:\n%s\n", res.headers.items);
        return 1;
    }
    fails += check("401 challenge headers correct", 1);
    http_response_free(&res);

    if (fails == 0) {
        printf("auth ok\n");
    }
    return fails != 0;
}