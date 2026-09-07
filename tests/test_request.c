#include <stdio.h>
#include <string.h>

#include "request.h"
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

    Http_Request req;
    String_View raw = sv_from_cstr(
        "GET /index.html?page=1&tag=c%23 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: cweb-test/0.1\r\n"
        "\r\n");
    Request_Parse_Result r = http_request_parse(&req, raw);
    fails += check("GET request parses", r == REQ_OK);
    fails += check("method is GET", req.method == HTTP_GET);
    fails += check("path is /index.html", sv_equal(req.path, sv_from_cstr("/index.html")));
    fails += check("query is page=1&tag=c%23", sv_equal(req.query, sv_from_cstr("page=1&tag=c%23")));
    fails += check("version is HTTP/1.1", sv_equal(req.version, sv_from_cstr("HTTP/1.1")));
    fails += check("two headers", req.headers.count == 2);
    const String_View *host = http_request_get_header(&req, "host");
    fails += check("host header found case-insensitively", host != NULL && sv_equal(*host, sv_from_cstr("example.com")));
    const String_View *ua = http_request_get_header(&req, "USER-AGENT");
    fails += check("user-agent header found", ua != NULL && sv_equal(*ua, sv_from_cstr("cweb-test/0.1")));
    fails += check("missing header is NULL", http_request_get_header(&req, "x-missing") == NULL);
    http_request_free(&req);

    // LF-only line endings and a body after the headers
    String_View raw2 = sv_from_cstr(
        "POST /login HTTP/1.1\n"
        "Content-Length: 11\n"
        "\n"
        "hello=world");
    Request_Parse_Result r2 = http_request_parse(&req, raw2);
    fails += check("LF-only request parses", r2 == REQ_OK);
    fails += check("method is POST", req.method == HTTP_POST);
    fails += check("no query on /login", sv_equal(req.query, sv_from_cstr("")));
    const String_View *cl = http_request_get_header(&req, "Content-Length");
    fails += check("content-length parsed", cl != NULL && sv_equal(*cl, sv_from_cstr("11")));
    fails += check("body clamped to content-length", sv_equal(req.body, sv_from_cstr("hello=world")));
    http_request_free(&req);

    // no Content-Length means the body is everything left in the buffer
    String_View raw5 = sv_from_cstr("POST /x HTTP/1.1\r\n\r\nraw rest");
    Request_Parse_Result r5 = http_request_parse(&req, raw5);
    fails += check("no-content-length body okay", r5 == REQ_OK);
    fails += check("body takes the remainder", sv_equal(req.body, sv_from_cstr("raw rest")));
    http_request_free(&req);

    // Content-Length bigger than what arrived means the request is incomplete
    String_View raw6 = sv_from_cstr("POST /x HTTP/1.1\r\nContent-Length: 100\r\n\r\nshort");
    Request_Parse_Result r6 = http_request_parse(&req, raw6);
    fails += check("short body reported incomplete", r6 == REQ_INCOMPLETE);
    http_request_free(&req);

    // truncated request, needs more bytes
    Request_Parse_Result r3 = http_request_parse(&req, sv_from_cstr("GET / HTTP/1.1\r\nHost: x"));
    fails += check("incomplete request reported", r3 == REQ_INCOMPLETE);
    http_request_free(&req);

    // request line with a missing version
    Request_Parse_Result r4 = http_request_parse(&req, sv_from_cstr("GET /\r\n\r\n"));
    fails += check("garbage request line rejected", r4 == REQ_ERROR);
    http_request_free(&req);

    if (fails == 0) {
        printf("request ok\n");
    }
    return fails != 0;
}