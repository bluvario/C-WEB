#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etag.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "date.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// the final handler the middleware wraps: stamps a known body so the etag is
// deterministic
static void body_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    http_response_add_body_cstr(res, "fixed body");
    (void)user_data;
}

// build a request for a method with an optional If-None-Match header (views
// borrow from a static buffer, like the other tests do)
static void build_req(Http_Request *req, const char *inm, const char *ims)
{
    static char raw[512];
    int n = snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nHost: x\r\n");
    if (inm != NULL) {
        n += snprintf(raw + n, sizeof raw - (size_t)n,
                      "If-None-Match: %s\r\n", inm);
    }
    if (ims != NULL) {
        n += snprintf(raw + n, sizeof raw - (size_t)n,
                      "If-Modified-Since: %s\r\n", ims);
    }
    snprintf(raw + n, sizeof raw - (size_t)n, "\r\n");
    http_request_parse(req, sv_from_cstr(raw));
}

static void make_req(Http_Request *req, const char *inm)
{
    build_req(req, inm, NULL);
}

static char *etag_value(Http_Response *res)
{
    return http_response_get_header(res, "ETag");
}

static void handler_with_etag(Http_Request *req, Http_Response *res,
                              void *user_data)
{
    (void)req;
    http_response_set_header(res, "ETag", "\"handler-tag\"");
    http_response_add_body_cstr(res, "fixed body");
    (void)user_data;
}

// stamps a known modification instant the way a dynamic handler would; the
// date http_date_rfc7231 serializes is what we echo back as If-Modified-Since
static void handler_with_last_modified(Http_Request *req, Http_Response *res,
                                       void *user_data)
{
    (void)req;
    http_response_set_last_modified(res, (time_t)1234567890);
    http_response_add_body_cstr(res, "stamped body");
    (void)user_data;
}

static void redirect_handler(Http_Request *req, Http_Response *res,
                             void *user_data)
{
    (void)req;
    http_response_redirect(res, HTTP_302_FOUND, "/elsewhere");
    (void)user_data;
}

int main(void)
{
    // a plain GET gains a strong etag derived from the body
    {
        Http_Request req;
        make_req(&req, NULL);
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, body_handler, NULL);
        check("handler ran", res.status == HTTP_200_OK);
        char *etag = etag_value(&res);
        check("a strong etag was stamped", etag != NULL && strncmp(etag, "\"", 1) == 0);
        check("etag is a 64-hex digest", etag != NULL &&
              (int)strlen(etag) == 64 + 2);
        check("body survives", res.body.count == strlen("fixed body"));
        free(etag);
        http_request_free(&req);
        http_response_free(&res);
    }

    // the same body yields the same etag across runs
    {
        Http_Request req1, req2;
        make_req(&req1, NULL);
        make_req(&req2, NULL);
        Http_Response r1, r2;
        http_response_init(&r1);
        http_response_init(&r2);
        http_etag_middleware(&req1, &r1, NULL, body_handler, NULL);
        http_etag_middleware(&req2, &r2, NULL, body_handler, NULL);
        char *e1 = etag_value(&r1);
        char *e2 = etag_value(&r2);
        check("etag is deterministic", e1 != NULL && e2 != NULL &&
              strcmp(e1, e2) == 0);
        free(e1);
        free(e2);
        http_request_free(&req1);
        http_request_free(&req2);
        http_response_free(&r1);
        http_response_free(&r2);
    }

    // a matching If-None-Match turns a GET into 304 with the body dropped
    {
        Http_Response probe;
        Http_Request preq;
        make_req(&preq, NULL);
        http_response_init(&probe);
        http_etag_middleware(&preq, &probe, NULL, body_handler, NULL);
        char *etag = etag_value(&probe);
        http_response_free(&probe);
        http_request_free(&preq);

        Http_Request req;
        make_req(&req, etag); // echo our own etag back
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, body_handler, NULL);
        check("matching If-None-Match answers 304",
              res.status == HTTP_304_NOT_MODIFIED);
        check("304 drops the body", res.body.count == 0);
        // and the serialized head omits Content-Length and the body
        Strbuf out;
        strbuf_init(&out);
        http_response_serialize(&res, &out);
        strbuf_null_terminate(&out);
        check("serialized 304 has no Content-Length",
              strstr(out.items, "Content-Length:") == NULL);
        check("serialized 304 carries the etag",
              strstr(out.items, "ETag:") != NULL);
        strbuf_free(&out);
        free(etag);
        http_request_free(&req);
        http_response_free(&res);
    }

    // a non-matching If-None-Match keeps a full 200
    {
        Http_Request req;
        make_req(&req, "\"deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef\"");
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, body_handler, NULL);
        check("non-matching If-None-Match keeps 200",
              res.status == HTTP_200_OK);
        check("non-matching keeps the body",
              res.body.count == strlen("fixed body"));
        http_request_free(&req);
        http_response_free(&res);
    }

    // "*" matches any existing representation
    {
        Http_Request req;
        make_req(&req, "*");
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, body_handler, NULL);
        check("If-None-Match: * answers 304", res.status == HTTP_304_NOT_MODIFIED);
        http_request_free(&req);
        http_response_free(&res);
    }

    // a comma-separated list can carry several candidates; one match wins
    {
        Http_Response probe;
        Http_Request preq;
        make_req(&preq, NULL);
        http_response_init(&probe);
        http_etag_middleware(&preq, &probe, NULL, body_handler, NULL);
        char *etag = etag_value(&probe);
        http_response_free(&probe);
        http_request_free(&preq);

        char inm[256];
        snprintf(inm, sizeof inm, "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", %s",
                 etag);
        Http_Request req;
        make_req(&req, inm);
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, body_handler, NULL);
        check("a list with a matching entry answers 304",
              res.status == HTTP_304_NOT_MODIFIED);
        free(etag);
        http_request_free(&req);
        http_response_free(&res);
    }

    // an etag the handler already stamped is respected, not overwritten
    {
        Http_Request req;
        make_req(&req, NULL);
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, handler_with_etag, NULL);
        char *etag = etag_value(&res);
        check("handler's own etag is preserved",
              etag != NULL && strcmp(etag, "\"handler-tag\"") == 0);
        free(etag);
        http_request_free(&req);
        http_response_free(&res);
    }

    // a 3xx response is not validated (nothing cacheable to short-circuit)
    {
        Http_Request req;
        make_req(&req, "*");
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, redirect_handler, NULL);
        check("3xx status is not turned into 304",
              res.status == HTTP_302_FOUND);
        http_request_free(&req);
        http_response_free(&res);
    }

    // a handler-stamped Last-Modified is validated via If-Modified-Since
    {
        Http_Response probe;
        Http_Request preq;
        make_req(&preq, NULL);
        http_response_init(&probe);
        http_etag_middleware(&preq, &probe, NULL, handler_with_last_modified,
                             NULL);
        char *lm = http_response_get_header(&probe, "Last-Modified");
        check("Last-Modified was stamped",
              lm != NULL && strcmp(lm, "Fri, 13 Feb 2009 23:31:30 GMT") == 0);
        http_response_free(&probe);
        http_request_free(&preq);

        // an If-Modified-Since equal to the stamp answers 304 without a body
        Http_Request req;
        build_req(&req, NULL, lm); // echo the stamped date back
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, handler_with_last_modified,
                             NULL);
        check("equal If-Modified-Since answers 304",
              res.status == HTTP_304_NOT_MODIFIED);
        check("304 drops the body", res.body.count == 0);
        http_request_free(&req);
        http_response_free(&res);

        // a later If-Modified-Since is also "not modified"
        static char later_buf[64];
        http_date_rfc7231((time_t)1234567890 + 30, later_buf, sizeof later_buf);
        Http_Request req2;
        build_req(&req2, NULL, later_buf);
        Http_Response res2;
        http_response_init(&res2);
        http_etag_middleware(&req2, &res2, NULL, handler_with_last_modified,
                             NULL);
        check("later If-Modified-Since answers 304",
              res2.status == HTTP_304_NOT_MODIFIED);
        http_request_free(&req2);
        http_response_free(&res2);

        // an earlier If-Modified-Since means the client is stale: full 200
        static char earlier_buf[64];
        http_date_rfc7231((time_t)1234567890 - 60, earlier_buf, sizeof earlier_buf);
        Http_Request req3;
        build_req(&req3, NULL, earlier_buf);
        Http_Response res3;
        http_response_init(&res3);
        http_etag_middleware(&req3, &res3, NULL, handler_with_last_modified,
                             NULL);
        check("earlier If-Modified-Since keeps 200",
              res3.status == HTTP_200_OK);
        check("stale client keeps the body",
              res3.body.count == strlen("stamped body"));
        http_request_free(&req3);
        http_response_free(&res3);

        // If-None-Match takes precedence over If-Modified-Since: a matching
        // tag wins even when the date says the client should be stale
        Http_Response tag_probe;
        Http_Request tag_preq;
        make_req(&tag_preq, NULL);
        http_response_init(&tag_probe);
        http_etag_middleware(&tag_preq, &tag_probe, NULL,
                             handler_with_last_modified, NULL);
        char *tag = http_response_get_header(&tag_probe, "ETag");
        Http_Request req4;
        build_req(&req4, tag, earlier_buf);
        Http_Response res4;
        http_response_init(&res4);
        http_etag_middleware(&req4, &res4, NULL, handler_with_last_modified,
                             NULL);
        check("matching If-None-Match beats a stale If-Modified-Since",
              res4.status == HTTP_304_NOT_MODIFIED);
        http_request_free(&req4);
        http_response_free(&res4);
        free(tag);
        http_request_free(&tag_preq);
        http_response_free(&tag_probe);
        free(lm);
    }

    // If-Modified-Since alone, without a Last-Modified to validate against,
    // changes nothing (the middleware cannot guess the modification time)
    {
        Http_Request req;
        build_req(&req, NULL, "Sat, 14 Feb 2009 00:31:30 GMT");
        Http_Response res;
        http_response_init(&res);
        http_etag_middleware(&req, &res, NULL, body_handler, NULL);
        check("If-Modified-Since without Last-Modified keeps 200",
              res.status == HTTP_200_OK);
        check("body survived the non-validation",
              res.body.count == strlen("fixed body"));
        http_request_free(&req);
        http_response_free(&res);
    }

    if (fails == 0) {
        printf("etag ok\n");
    }
    return fails != 0;
}