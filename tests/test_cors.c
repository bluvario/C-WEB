#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cors.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// the final handler the middleware wraps: stamps a body so tests can tell it
// ran, and records the fact separately for short-circuit detection
static int handler_ran = 0;

static void ok_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    handler_ran++;
    http_response_add_body_cstr(res, "hello");
    (void)user_data;
}

// build a request for method + a single Origin header value. the parsed
// request borrows from a static buffer (like a string literal would), so the
// views stay valid for the whole test run
static void make_req(Http_Request *req, const char *method, const char *origin)
{
    static char raw[512];
    snprintf(raw, sizeof raw,
             "%s / HTTP/1.1\r\nHost: x\r\n%s%s%s\r\n\r\n",
             method,
             origin ? "Origin: " : "",
             origin ? origin : "",
             origin ? "\r\n" : "");
    http_request_parse(req, sv_from_cstr(raw));
}

static int has_header(Http_Response *res, const char *name, const char *value)
{
    // http_response_get_header finds the header by name (any case) and
    // returns a heap copy of its value, so no raw strstr over the unbounded
    // header text
    char *got = http_response_get_header(res, name);
    int eq = got != NULL && strcmp(got, value) == 0;
    free(got);
    return eq;
}

int main(void)
{
    // actual GET from an allowed origin gets the CORS headers and the handler
    {
        Http_Request req;
        make_req(&req, "GET", "https://example.com");
        Http_Response res;
        http_response_init(&res);
        Http_Cors_Options opts = {.origin = "https://example.com"};
        handler_ran = 0;
        http_cors_middleware(&req, &res, &opts, ok_handler, NULL);
        check("handler ran for a matching GET", handler_ran == 1);
        check("response carries Access-Control-Allow-Origin",
              has_header(&res, "Access-Control-Allow-Origin", "https://example.com"));
        check("response carries Vary: Origin",
              has_header(&res, "Vary", "Origin"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // preflight OPTIONS from a matching origin short-circuits the chain
    {
        Http_Request req;
        make_req(&req, "OPTIONS", "https://example.com");
        Http_Response res;
        http_response_init(&res);
        Http_Cors_Options opts = {
            .origin = "https://example.com",
            .methods = "GET, POST",
            .headers = "Content-Type",
            .max_age = 600,
            .credentials = 1,
        };
        handler_ran = 0;
        http_cors_middleware(&req, &res, &opts, ok_handler, NULL);
        check("preflight skips the handler", handler_ran == 0);
        check("preflight answers 204", res.status == HTTP_204_NO_CONTENT);
        check("preflight allows the origin",
              has_header(&res, "Access-Control-Allow-Origin", "https://example.com"));
        check("preflight allows the methods",
              has_header(&res, "Access-Control-Allow-Methods", "GET, POST"));
        check("preflight allows the headers",
              has_header(&res, "Access-Control-Allow-Headers", "Content-Type"));
        check("preflight has a max age",
              has_header(&res, "Access-Control-Max-Age", "600"));
        check("preflight allows credentials",
              has_header(&res, "Access-Control-Allow-Credentials", "true"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // wildcard origin, no credentials
    {
        Http_Request req;
        make_req(&req, "GET", "https://anything.io");
        Http_Response res;
        http_response_init(&res);
        Http_Cors_Options opts = {.origin = "*"};
        handler_ran = 0;
        http_cors_middleware(&req, &res, &opts, ok_handler, NULL);
        check("wildcard matches any origin", handler_ran == 1);
        check("wildcard echoes the requesting origin",
              has_header(&res, "Access-Control-Allow-Origin", "https://anything.io"));
        check("wildcard never allows credentials",
              !has_header(&res, "Access-Control-Allow-Credentials", "true"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // NULL user_data behaves like origin *
    {
        Http_Request req;
        make_req(&req, "GET", "https://nowhere.test");
        Http_Response res;
        http_response_init(&res);
        handler_ran = 0;
        http_cors_middleware(&req, &res, NULL, ok_handler, NULL);
        check("NULL options still matches", handler_ran == 1);
        check("NULL options echoes origin",
              has_header(&res, "Access-Control-Allow-Origin", "https://nowhere.test"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // a non-matching origin passes through but gets no CORS headers
    {
        Http_Request req;
        make_req(&req, "GET", "https://evilsite.example");
        Http_Response res;
        http_response_init(&res);
        Http_Cors_Options opts = {.origin = "https://goodsite.example"};
        handler_ran = 0;
        http_cors_middleware(&req, &res, &opts, ok_handler, NULL);
        check("foreign origin still runs the handler", handler_ran == 1);
        check("foreign origin gets no allow-origin",
              !has_header(&res, "Access-Control-Allow-Origin", "https://evilsite.example"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // a request with no Origin header is not a CORS request at all
    {
        Http_Request req;
        make_req(&req, "GET", NULL);
        Http_Response res;
        http_response_init(&res);
        Http_Cors_Options opts = {.origin = "*"};
        handler_ran = 0;
        http_cors_middleware(&req, &res, &opts, ok_handler, NULL);
        check("no-origin request runs the handler", handler_ran == 1);
        check("no-origin request gets no CORS headers",
              !http_response_has_header(&res, "Access-Control-Allow-Origin"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // the preflight response serializes to a valid 204 (no body, no length)
    {
        Http_Request req;
        make_req(&req, "OPTIONS", "https://example.com");
        Http_Response res;
        http_response_init(&res);
        Http_Cors_Options opts = {.origin = "https://example.com"};
        http_cors_middleware(&req, &res, &opts, ok_handler, NULL);
        Strbuf out;
        strbuf_init(&out);
        http_response_serialize(&res, &out);
        strbuf_null_terminate(&out);
        check("preflight serializes with status 204",
              strstr(out.items, "HTTP/1.1 204") != NULL);
        check("preflight serializes with allow-origin",
              strstr(out.items, "Access-Control-Allow-Origin: https://example.com") != NULL);
        strbuf_free(&out);
        http_request_free(&req);
        http_response_free(&res);
    }

    if (fails == 0) {
        printf("cors ok\n");
    }
    return fails != 0;
}