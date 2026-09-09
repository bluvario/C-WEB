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

    if (strncmp(wire.items, "HTTP/1.1 200 OK\r\n", 17) != 0 ||
        strstr(wire.items, "Content-Type: text/html\r\n") == NULL ||
        strstr(wire.items, "Content-Length: 5\r\n") == NULL ||
        strstr(wire.items, "Connection: close\r\n") == NULL ||
        strstr(wire.items, "\r\n\r\nhello") == NULL) {
        fprintf(stderr, "200 wire format wrong:\n%s\n", wire.items);
        return 1;
    }
    fails += check("200 response wire format", 1);

    // regression: the body must land in the wire exactly once. a refactor
    // once appended it both in serialize and in the head writer, doubling it.
    size_t greps = 0;
    for (size_t i = 0; i + 4 < wire.count; i++) {
        if (memcmp(&wire.items[i], "hello", 5) == 0) {
            greps++;
        }
    }
    fails += check("body emitted exactly once", greps == 1);

    // serialized responses carry a usable RFC 7231 Date stamp in IMF-fixdate
    // shape: "Tue, 08 Sep 2026 12:49:36 GMT"
    const char *date = strstr(wire.items, "Date: ");
    if (date == NULL) {
        fprintf(stderr, "Date header missing:\n%s\n", wire.items);
        return 1;
    }
    const char *date_val = date + 6;
    // 29 chars of RFC 1123 date, trailing " GMT" begins at offset 25
    const char *gmt = strstr(date_val, " GMT\r\n");
    if (gmt == NULL || gmt - date_val != 25) {
        fprintf(stderr, "Date header malformed: %s\n", date);
        return 1;
    }
    fails += check("Date header present and well-formed", 1);
    http_response_free(&res);
    strbuf_free(&wire);

    // 204 must not carry a body or Content-Length, but still gets a Date
    http_response_init(&res);
    http_response_set_status(&res, HTTP_204_NO_CONTENT);
    http_response_add_body_cstr(&res, "must not appear");
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;
    if (strncmp(wire.items, "HTTP/1.1 204 No Content\r\n", 25) != 0 ||
        strstr(wire.items, "Content-Length:") != NULL ||
        strstr(wire.items, "must not appear") != NULL ||
        strstr(wire.items, "Connection: close\r\n") == NULL ||
        strstr(wire.items, "\r\nDate: ") == NULL) {
        fprintf(stderr, "204 wire format wrong:\n%s\n", wire.items);
        return 1;
    }
    fails += check("204 wire format", 1);
    http_response_free(&res);
    strbuf_free(&wire);

    // a handler that provides its own Date wins over the automatic one
    http_response_init(&res);
    http_response_set_header(&res, "Date", "Thu, 01 Jan 1970 00:00:00 GMT");
    http_response_add_body_cstr(&res, "x");
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;
    fails += check("handler Date kept", strstr(wire.items, "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n") != NULL);
    fails += check("handler Date not duplicated", strstr(wire.items, "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\nDate: ") == NULL);
    http_response_free(&res);
    strbuf_free(&wire);

    // redirect wires up Location and the right status line, no body
    http_response_init(&res);
    http_response_redirect(&res, HTTP_302_FOUND, "/login");
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    if (strbuf_null_terminate(&wire) != 0) return 1;
    if (strncmp(wire.items, "HTTP/1.1 302 Found\r\n", 20) != 0 ||
        strstr(wire.items, "Location: /login\r\n") == NULL ||
        strstr(wire.items, "Content-Type: text/plain; charset=utf-8\r\n") == NULL ||
        strstr(wire.items, "Content-Length: 0\r\n") == NULL ||
        strstr(wire.items, "redirecting to") != NULL) {
        fprintf(stderr, "302 wire format wrong:\n%s\n", wire.items);
        return 1;
    }
    fails += check("302 wire format", 1);
    http_response_free(&res);
    strbuf_free(&wire);

    // a bogus status quietly becomes 302
    http_response_init(&res);
    http_response_redirect(&res, HTTP_200_OK, "/home");
    fails += check("bogus redirect status coerced to 302", res.status == HTTP_302_FOUND);
    fails += check("redirect keeps Location header",
                   strstr(res.headers.items, "Location: /home\r\n") != NULL);
    http_response_free(&res);

    // a redirect on a response that already carries a content type and a body
    // must not duplicate them (a template page sets both up front)
    http_response_init(&res);
    http_response_set_header(&res, "Content-Type", "text/html; charset=utf-8");
    http_response_add_body_cstr(&res, "form");
    http_response_redirect(&res, HTTP_303_SEE_OTHER, "/");
    if (strbuf_null_terminate(&res.headers) != 0) return 1;
    if (strbuf_null_terminate(&res.body) != 0) return 1;
    const char *html_hdr = strstr(res.headers.items, "text/html");
    fails += check("redirect keeps existing content type", html_hdr != NULL);
    fails += check("redirect does not add a second content type",
                   html_hdr != NULL && strstr(html_hdr + 9, "Content-Type:") == NULL);
    fails += check("redirect keeps existing body",
                   strstr(res.body.items, "form") != NULL && !strstr(res.body.items, "redirecting to"));
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