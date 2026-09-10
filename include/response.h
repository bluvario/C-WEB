#ifndef CWEB_RESPONSE_H
#define CWEB_RESPONSE_H

#include <stdbool.h>

#include "http.h"
#include "json.h"
#include "strbuf.h"
#include "sv.h"

// streamed body source. fill buf with up to cap bytes and return how many,
// or 0 once the stream is done. runs on the connection's worker after the
// headers have been sent; never touch the socket, the server frames the
// chunks for you.
typedef size_t (*Http_Stream_Fn)(void *buf, size_t cap, void *user_data);

// in-memory response, assembled piece by piece then serialized. header and
// body buffers are owned, free the response when done with it.
typedef struct {
    Http_Status status;
    Strbuf headers; // raw "Name: value\r\n" text
    Strbuf body;    // the actual payload; HEAD requests keep it for length
    bool suppress_body; // serialize headers but emit no body bytes
    bool keep_alive; // advertise Connection: keep-alive instead of close
    bool no_layout;  // when set the generated layout wrapper skips framing
    Http_Stream_Fn stream_fn; // when set the body is chunked and streamed
    void *stream_user;
} Http_Response;

void http_response_init(Http_Response *res);
void http_response_free(Http_Response *res);

void http_response_set_status(Http_Response *res, Http_Status status);
// appends a header. calling it twice with the same name emits it twice,
// replacing existing headers is not implemented yet.
void http_response_set_header(Http_Response *res, const char *name, const char *value);

// replaces every existing header with this name (any case) by the given
// value; the new line lands at the end. an absent header is appended like
// http_response_set_header. handy when a handler wants to override a
// framework-provided default such as Content-Type.
void http_response_set_header_replace(Http_Response *res, const char *name, const char *value);

// scans the response's header text for name (any case) and returns a heap
// copy of its value, or NULL when the header is absent. caller xfrees it.
// used by middleware that needs to read back a header the handler set, e.g.
// Content-Type or Content-Encoding.
char *http_response_get_header(Http_Response *res, const char *name);
// true when the response carries a header with this name (any case)
bool http_response_has_header(Http_Response *res, const char *name);

void http_response_add_body(Http_Response *res, String_View data);
void http_response_add_body_cstr(Http_Response *res, const char *text);

// switches the response to transfer-encoding: chunked with fn as the source
// of body bytes. any buffered body is dropped: a response is either streamed
// or assembled, not both.
void http_response_set_stream(Http_Response *res, Http_Stream_Fn fn, void *user_data);

// turns the response into a redirect: sets Location and the given status,
// with no body (the client follows Location anyway). a plain-text
// Content-Type is only filled in when the response is otherwise bare. only
// 301/302/303/307/308 are valid redirect codes, anything else quietly
// becomes a 302.
void http_response_redirect(Http_Response *res, Http_Status status, const char *location);

// sets Content-Type: application/json; charset=utf-8, serializes the value
// compactly, appends it to the body, and frees the tree. the value is
// consumed: do not touch it afterwards.
void http_response_json(Http_Response *res, Http_Status status, Json_Value value);

// sets Content-Type: application/json; charset=utf-8 and appends body as-is,
// for callers that already hold serialized JSON text.
void http_response_json_raw(Http_Response *res, Http_Status status, const char *body);

// allows browser JavaScript from *origin to read this response ("*" opens it
// to any origin without credentials). also marks it with Vary: Origin so
// shared caches can keep the variants apart.
void http_response_set_cors(Http_Response *res, const char *origin);
// answers a preflight: which methods and request headers the resource is
// willing to run, and how long the answer may be cached (0 = no Max-Age).
void http_response_set_cors_allow(Http_Response *res, const char *methods,
                                  const char *request_headers, long max_age_seconds);

// writes the full HTTP/1.1 message (status line, headers, computed
// Content-Length, blank line, body) to *out. statuses without a body
// (204, 304) skip Content-Length and the body.
void http_response_serialize(Http_Response *res, Strbuf *out);

#endif