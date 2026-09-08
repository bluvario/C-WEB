#include <stdio.h>
#include <string.h>

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

    Http_Response res;
    http_response_init(&res);
    http_response_set_status(&res, HTTP_200_OK);
    http_response_set_header(&res, "Content-Type", "text/html");
    http_response_add_body_cstr(&res, "hello");

    Strbuf wire;
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;

    const char *expect =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello";
    fails += check("200 response wire format", strcmp(wire.items, expect) == 0);
    http_response_free(&res);
    strbuf_free(&wire);

    // 204 must not carry a body or Content-Length
    http_response_init(&res);
    http_response_set_status(&res, HTTP_204_NO_CONTENT);
    http_response_add_body_cstr(&res, "must not appear");
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;
    const char *expect204 =
        "HTTP/1.1 204 No Content\r\n"
        "Connection: close\r\n"
        "\r\n";
    fails += check("204 wire format", strcmp(wire.items, expect204) == 0);
    http_response_free(&res);
    strbuf_free(&wire);

    // redirect wires up Location, a body and the right status line
    http_response_init(&res);
    http_response_redirect(&res, HTTP_302_FOUND, "/login");
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;
    const char *expect302 =
        "HTTP/1.1 302 Found\r\n"
        "Location: /login\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 22\r\n"
        "Connection: close\r\n"
        "\r\n"
        "redirecting to /login\n";
    fails += check("302 wire format", strcmp(wire.items, expect302) == 0);
    http_response_free(&res);
    strbuf_free(&wire);

    // a bogus status quietly becomes 302
    http_response_init(&res);
    http_response_redirect(&res, HTTP_200_OK, "/home");
    fails += check("bogus redirect status coerced to 302", res.status == HTTP_302_FOUND);
    fails += check("redirect keeps Location header",
                   strstr(res.headers.items, "Location: /home\r\n") != NULL);
    http_response_free(&res);

    // CORS: a readable response and a cacheable preflight answer
    http_response_init(&res);
    http_response_set_cors(&res, "https://app.example");
    http_response_add_body_cstr(&res, "ok");
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;
    if (strstr(wire.items, "Access-Control-Allow-Origin: https://app.example\r\n") == NULL ||
        strstr(wire.items, "Vary: Origin\r\n") == NULL) {
        fprintf(stderr, "CORS headers missing:\n%s\n", wire.items);
        return 1;
    }
    fails += check("cors headers present", 1);
    http_response_free(&res);
    strbuf_free(&wire);

    http_response_init(&res);
    http_response_set_cors_allow(&res, "GET, POST", "Content-Type, X-App-Key", 600);
    if (strstr(res.headers.items, "Access-Control-Allow-Methods: GET, POST\r\n") == NULL ||
        strstr(res.headers.items, "Access-Control-Allow-Headers: Content-Type, X-App-Key\r\n") == NULL ||
        strstr(res.headers.items, "Access-Control-Max-Age: 600\r\n") == NULL) {
        fprintf(stderr, "preflight headers missing:\n%s\n", res.headers.items);
        return 1;
    }
    fails += check("preflight headers present", 1);
    http_response_free(&res);

    if (fails == 0) {
        printf("response ok\n");
    }
    return fails != 0;
}