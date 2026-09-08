#include "response.h"

#include <stdio.h>

#include "http.h"
#include "strbuf.h"

void http_response_init(Http_Response *res)
{
    res->status = HTTP_200_OK;
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
    strbuf_append_cstr(out, "Connection: close\r\n");
    strbuf_append_cstr(out, "\r\n");

    if (!status_has_no_body && !res->suppress_body) {
        strbuf_append(out, res->body.items, res->body.count);
    }
}