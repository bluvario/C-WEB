#include "response.h"

#include <stdio.h>

#include "http.h"
#include "strbuf.h"

void http_response_init(Http_Response *res)
{
    res->status = HTTP_200_OK;
    res->suppress_body = false;
    res->keep_alive = false;
    strbuf_init(&res->headers);
    strbuf_init(&res->body);
}

void http_response_free(Http_Response *res)
{
    strbuf_free(&res->headers);
    strbuf_free(&res->body);
}

void http_response_set_status(Http_Response *res, Http_Status status)
{
    res->status = status;
}

void http_response_set_header(Http_Response *res, const char *name, const char *value)
{
    strbuf_append_cstr(&res->headers, name);
    strbuf_append_cstr(&res->headers, ": ");
    strbuf_append_cstr(&res->headers, value);
    strbuf_append_cstr(&res->headers, "\r\n");
}

void http_response_add_body(Http_Response *res, String_View data)
{
    strbuf_append(&res->body, data.data, data.count);
}

void http_response_add_body_cstr(Http_Response *res, const char *text)
{
    strbuf_append_cstr(&res->body, text);
}

void http_response_redirect(Http_Response *res, Http_Status status, const char *location)
{
    switch (status) {
        case HTTP_301_MOVED_PERMANENTLY:
        case HTTP_302_FOUND:
        case HTTP_303_SEE_OTHER:
        case HTTP_307_TEMPORARY_REDIRECT:
        case HTTP_308_PERMANENT_REDIRECT:
            break;
        default:
            status = HTTP_302_FOUND;
    }

    http_response_set_status(res, status);
    http_response_set_header(res, "Location", location);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_add_body_cstr(res, "redirecting to ");
    http_response_add_body_cstr(res, location);
    http_response_add_body_cstr(res, "\n");
}

void http_response_set_cors(Http_Response *res, const char *origin)
{
    http_response_set_header(res, "Access-Control-Allow-Origin", origin);
    http_response_set_header(res, "Vary", "Origin");
}

void http_response_set_cors_allow(Http_Response *res, const char *methods,
                                  const char *request_headers, long max_age_seconds)
{
    http_response_set_header(res, "Access-Control-Allow-Methods", methods);
    if (request_headers != NULL) {
        http_response_set_header(res, "Access-Control-Allow-Headers", request_headers);
    }
    if (max_age_seconds > 0) {
        char age[32];
        snprintf(age, sizeof(age), "%ld", max_age_seconds);
        http_response_set_header(res, "Access-Control-Max-Age", age);
    }
}

void http_response_serialize(Http_Response *res, Strbuf *out)
{
    // responses in these statuses carry no body per RFC 9110
    bool status_has_no_body = res->status == HTTP_204_NO_CONTENT || res->status == HTTP_304_NOT_MODIFIED;

    char line[128];
    snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n",
             (int)res->status, http_status_reason(res->status));
    strbuf_append_cstr(out, line);

    strbuf_append(out, res->headers.items, res->headers.count);

    // a HEAD reply advertises the body it would have sent as Content-Length
    // but carries none of the bytes, so suppress_body leaves body.count intact
    if (!status_has_no_body) {
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", res->body.count);
        strbuf_append_cstr(out, cl);
    }
    strbuf_append_cstr(out, res->keep_alive ? "Connection: keep-alive\r\n"
                                             : "Connection: close\r\n");
    strbuf_append_cstr(out, "\r\n");

    if (!status_has_no_body && !res->suppress_body) {
        strbuf_append(out, res->body.items, res->body.count);
    }
}