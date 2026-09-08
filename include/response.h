#ifndef CWEB_RESPONSE_H
#define CWEB_RESPONSE_H

#include "http.h"
#include "strbuf.h"
#include "sv.h"

// in-memory response, assembled piece by piece then serialized. header and
// body buffers are owned, free the response when done with it.
typedef struct {
    Http_Status status;
    Strbuf headers; // raw "Name: value\r\n" text
    Strbuf body;    // the actual payload; HEAD requests keep it for length
    bool suppress_body; // serialize headers but emit no body bytes
} Http_Response;

void http_response_init(Http_Response *res);
void http_response_free(Http_Response *res);

void http_response_set_status(Http_Response *res, Http_Status status);
// appends a header. calling it twice with the same name emits it twice,
// replacing existing headers is not implemented yet.
void http_response_set_header(Http_Response *res, const char *name, const char *value);

void http_response_add_body(Http_Response *res, String_View data);
void http_response_add_body_cstr(Http_Response *res, const char *text);

// turns the response into a redirect: sets Location, a tiny plain-text body
// and the given status. only 301/302/303/307/308 are valid redirect codes,
// anything else quietly becomes a 302.
void http_response_redirect(Http_Response *res, Http_Status status, const char *location);

// writes the full HTTP/1.1 message (status line, headers, computed
// Content-Length, blank line, body) to *out. statuses without a body
// (204, 304) skip Content-Length and the body.
void http_response_serialize(Http_Response *res, Strbuf *out);

#endif