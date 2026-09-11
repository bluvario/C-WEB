#include "response.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "date.h"
#include "http.h"
#include "strbuf.h"
#include "xmem.h"

void http_response_init(Http_Response *res)
{
    res->status = HTTP_200_OK;
    res->suppress_body = false;
    res->keep_alive = false;
    res->no_layout = false;
    res->stream_fn = NULL;
    res->stream_user = NULL;
    res->stream_chunks = NULL;
    res->stream_chunk_count = 0;
    res->stream_chunk_cap = 0;
    res->upgrade_fn = NULL;
    res->upgrade_user = NULL;
    strbuf_init(&res->headers);
    strbuf_init(&res->body);
}

void http_response_free(Http_Response *res)
{
    for (size_t i = 0; i < res->stream_chunk_count; i++) {
        xfree(res->stream_chunks[i].data);
    }
    xfree(res->stream_chunks);
    strbuf_free(&res->headers);
    strbuf_free(&res->body);
}

void http_response_set_status(Http_Response *res, Http_Status status)
{
    res->status = status;
}

// emits a "Name: value" line. when replace is true any existing line with
// the same name (any case) is dropped first and the new line lands at the
// end; otherwise the line is appended untouched, so repeatable fields like
// Set-Cookie can carry several lines in one response.
static void response_emit_header(Http_Response *res, const char *name,
                                 const char *value, bool replace)
{
    if (!replace) {
        strbuf_append_cstr(&res->headers, name);
        strbuf_append_cstr(&res->headers, ": ");
        strbuf_append_cstr(&res->headers, value);
        strbuf_append_cstr(&res->headers, "\r\n");
        return;
    }

    size_t nn = strlen(name);
    Strbuf out;
    strbuf_init(&out);

    // copy every line that does not start with "name:" (any case); the
    // replacement is appended after them all, so later headers move up. the
    // first matching line's exact spelling is reused for the new line, so a
    // later setter that writes the name in a different case does not
    // re-cast a canonical header like Content-Type.
    const char *matched = NULL;
    size_t at = 0;
    while (at < res->headers.count) {
        size_t line_end = at;
        while (line_end < res->headers.count &&
               res->headers.items[line_end] != '\n') {
            line_end++;
        }
        size_t line_len = line_end - at;
        bool match = line_len >= nn &&
                     res->headers.items[at + nn] == ':';
        if (match) {
            for (size_t i = 0; i < nn; i++) {
                if (tolower((unsigned char)res->headers.items[at + i]) !=
                    tolower((unsigned char)name[i])) {
                    match = false;
                    break;
                }
            }
        }
        if (match && matched == NULL) {
            matched = res->headers.items + at;
        }
        if (!match) {
            strbuf_append(&out, res->headers.items + at, line_len);
            if (line_end < res->headers.count) {
                strbuf_append_char(&out, '\n');
            }
        }
        at = line_end < res->headers.count ? line_end + 1 : res->headers.count;
    }

    strbuf_append(&out, matched != NULL ? matched : name, nn);
    strbuf_append_cstr(&out, ": ");
    strbuf_append_cstr(&out, value);
    strbuf_append_cstr(&out, "\r\n");
    // like the rest of the header text, the result is kept NUL-terminated so
    // callers can strstr it safely
    strbuf_null_terminate(&out);

    strbuf_free(&res->headers);
    res->headers = out;
}

void http_response_set_header(Http_Response *res, const char *name, const char *value)
{
    response_emit_header(res, name, value, true);
}

void http_response_append_header(Http_Response *res, const char *name, const char *value)
{
    response_emit_header(res, name, value, false);
}

void http_response_set_header_replace(Http_Response *res, const char *name, const char *value)
{
    response_emit_header(res, name, value, true);
}

void http_response_add_body(Http_Response *res, String_View data)
{
    strbuf_append(&res->body, data.data, data.count);
}

void http_response_add_body_cstr(Http_Response *res, const char *text)
{
    strbuf_append_cstr(&res->body, text);
}

void http_response_set_stream(Http_Response *res, Http_Stream_Fn fn, void *user_data)
{
    res->stream_fn = fn;
    res->stream_user = user_data;
    strbuf_free(&res->body);
    strbuf_init(&res->body);
    res->suppress_body = false;
}

void http_response_stream_write(Http_Response *res, const void *data, size_t len)
{
    if (res->stream_fn == NULL && res->stream_chunk_count == 0) {
        // first streamed write: drop any assembled body and switch the
        // response to chunked mode
        strbuf_free(&res->body);
        strbuf_init(&res->body);
        res->suppress_body = false;
    }
    if (res->stream_chunk_count == res->stream_chunk_cap) {
        size_t nc = res->stream_chunk_cap ? res->stream_chunk_cap * 2 : 8;
        res->stream_chunks = xrealloc(res->stream_chunks,
                                      nc * sizeof(*res->stream_chunks));
        res->stream_chunk_cap = nc;
    }
    Http_Stream_Chunk *c = &res->stream_chunks[res->stream_chunk_count];
    c->data = xmalloc(len > 0 ? len : 1);
    if (len > 0) {
        memcpy(c->data, data, len);
    }
    c->len = len;
    res->stream_chunk_count++;
}

void http_response_stream_write_cstr(Http_Response *res, const char *str)
{
    http_response_stream_write(res, str, strlen(str));
}

void http_response_stream_write_sv(Http_Response *res, String_View data)
{
    http_response_stream_write(res, data.data, data.count);
}

// header text is "Name: value\r\n" lines; true when name (any case) already
// appears at the start of a line
static bool response_has_header(Http_Response *res, const char *name)
{
    size_t nn = strlen(name);
    size_t at = 0;
    while (at + nn < res->headers.count) {
        size_t line_end = at;
        while (line_end < res->headers.count &&
               res->headers.items[line_end] != '\n') {
            line_end++;
        }
        size_t line_len = line_end - at;
        if (line_len >= nn &&
            res->headers.items[at + nn] == ':') {
            bool same = true;
            for (size_t i = 0; i < nn; i++) {
                if (tolower((unsigned char)res->headers.items[at + i]) !=
                    tolower((unsigned char)name[i])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
        at = line_end < res->headers.count ? line_end + 1 : res->headers.count;
    }
    return false;
}

bool http_response_has_header(Http_Response *res, const char *name)
{
    return response_has_header(res, name);
}

// returns the value after "Name:" trimmed of leading/trailing whitespace, as
// a heap copy; NULL when absent
char *http_response_get_header(Http_Response *res, const char *name)
{
    size_t nn = strlen(name);
    size_t at = 0;
    while (at + nn < res->headers.count) {
        size_t line_end = at;
        while (line_end < res->headers.count &&
               res->headers.items[line_end] != '\n') {
            line_end++;
        }
        size_t line_len = line_end - at;
        if (line_len >= nn &&
            res->headers.items[at + nn] == ':') {
            bool same = true;
            for (size_t i = 0; i < nn; i++) {
                if (tolower((unsigned char)res->headers.items[at + i]) !=
                    tolower((unsigned char)name[i])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                size_t value_start = at + nn + 1;
                while (value_start < line_end &&
                       (res->headers.items[value_start] == ' ' ||
                        res->headers.items[value_start] == '\t')) {
                    value_start++;
                }
                size_t value_end = line_end;
                while (value_end > value_start &&
                       (res->headers.items[value_end - 1] == ' ' ||
                        res->headers.items[value_end - 1] == '\t' ||
                        res->headers.items[value_end - 1] == '\r')) {
                    value_end--;
                }
                char *out = xmalloc(value_end - value_start + 1);
                if (value_end > value_start) {
                    memcpy(out, res->headers.items + value_start,
                           value_end - value_start);
                }
                out[value_end - value_start] = '\0';
                return out;
            }
        }
        at = line_end < res->headers.count ? line_end + 1 : res->headers.count;
    }
    return NULL;
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
    // a handler may already have set a content type, only fill in the
    // plain-text fallback when the response is still bare
    if (!response_has_header(res, "Content-Type")) {
        http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    }
}

void http_response_json(Http_Response *res, Http_Status status, Json_Value value)
{
    http_response_set_status(res, status);
    http_response_set_header_replace(res, "Content-Type", "application/json; charset=utf-8");
    Strbuf buf;
    strbuf_init(&buf);
    json_serialize(&value, &buf);
    if (strbuf_null_terminate(&buf) == 0) {
        http_response_add_body_cstr(res, buf.items);
    }
    strbuf_free(&buf);
    json_free_value(&value);
}

void http_response_json_raw(Http_Response *res, Http_Status status, const char *body)
{
    http_response_set_status(res, status);
    http_response_set_header_replace(res, "Content-Type", "application/json; charset=utf-8");
    http_response_add_body_cstr(res, body);
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

// did the handler already stamp a Date header? headers are raw "Name: value"
// text and can repeat, so every line gets a look, case-insensitively.
static bool has_header(Http_Response *res, const char *name)
{
    size_t n = strlen(name);
    const char *p = res->headers.items;
    const char *end = p + res->headers.count;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *colon = memchr(p, ':', len);
        size_t klen = colon ? (size_t)(colon - p) : len;
        if (klen == n) {
            bool same = true;
            for (size_t i = 0; i < n; i++) {
                if (toupper((unsigned char)p[i]) != toupper((unsigned char)name[i])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return false;
}

// writes the HTTP/1.1 message head (status line and headers). streamed
// responses advertise transfer-encoding: chunked and carry no body here; the
// server writes individual chunks afterwards.
void http_response_serialize_head(Http_Response *res, Strbuf *out)
{
    // responses in these statuses carry no body per RFC 9110; 1xx codes
    // (101 switching protocols) additionally must not be framed with a default
    // Connection header, since the handler supplies Connection: Upgrade
    bool status_has_no_body = res->status < 200 ||
                              res->status == HTTP_204_NO_CONTENT ||
                              res->status == HTTP_304_NOT_MODIFIED;
    bool informational = res->status >= 100 && res->status < 200;

    char line[128];
    snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n",
             (int)res->status, http_status_reason(res->status));
    strbuf_append_cstr(out, line);

    strbuf_append(out, res->headers.items, res->headers.count);

    bool streaming = res->stream_fn != NULL || res->stream_chunk_count > 0;
    if (!status_has_no_body) {
        if (streaming) {
            strbuf_append_cstr(out, "Transfer-Encoding: chunked\r\n");
        } else {
            // a HEAD reply advertises the body it would have sent as
            // Content-Length but carries none of the bytes, so suppress_body
            // leaves body.count intact
            char cl[64];
            snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", res->body.count);
            strbuf_append_cstr(out, cl);
        }
    }

    // RFC 9110: an origin server must date-stamp basic responses so clients
    // and caches can judge freshness; a handler's own Date header wins
    if (!informational && !has_header(res, "date")) {
        char d[64];
        http_date_rfc7231(time(NULL), d, sizeof(d));
        strbuf_append_cstr(out, "Date: ");
        strbuf_append_cstr(out, d);
        strbuf_append_cstr(out, "\r\n");
    }
    if (informational) {
        // the upgrade handshake brings its own Connection: Upgrade; a default
        // keep-alive/close line here would contradict it
        strbuf_append_cstr(out, "\r\n");
        return;
    }
    strbuf_append_cstr(out, res->keep_alive ? "Connection: keep-alive\r\n"
                                             : "Connection: close\r\n");
    strbuf_append_cstr(out, "\r\n");
}

void http_response_serialize(Http_Response *res, Strbuf *out)
{
    http_response_serialize_head(res, out);
    // streamed responses keep their body out of the buffer; the server writes
    // it chunk by chunk straight to the socket
    if (res->stream_fn != NULL || res->stream_chunk_count > 0) {
        return;
    }
    // responses in these statuses carry no body per RFC 9110 (head already
    // omitted their Content-Length)
    bool status_has_no_body = res->status < 200 ||
                              res->status == HTTP_204_NO_CONTENT ||
                              res->status == HTTP_304_NOT_MODIFIED;
    if (!status_has_no_body && !res->suppress_body) {
        strbuf_append(out, res->body.items, res->body.count);
    }
}