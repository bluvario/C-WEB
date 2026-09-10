#ifndef CWEB_ETAG_H
#define CWEB_ETAG_H

#include "middleware.h"

// options for the conditional-GET middleware. pass a pointer as user_data, or
// NULL for the defaults (strong etag from the response body).
typedef struct {
    // when non-zero, the etag is advertised as weak ("W/\"...\""), which is
    // fine when bytes are not byte-for-byte comparable (e.g. after gzip). the
    // default (0) emits a strong etag, which every cacheable dynamic response
    // wants when it can.
    int weak;
} Http_Etag_Options;

// conditional-GET middleware: after the handler assembles a successful,
// non-streamed response with a body, stamps an ETag computed from the body
// bytes (SHA-256, hex). a matching If-None-Match then answers 304 and drops
// the body, so clients and caches short-circuit on the validator instead of
// re-downloading. responses that already carry an ETag header from the handler
// (e.g. static files) are left alone and only matched, never re-tagged.
//
// streamed (chunked) responses and non-2xx statuses are passed through
// untouched, since an etag for a partial entity would be meaningless.
void http_etag_middleware(Http_Request *req, Http_Response *res,
                          void *user_data,
                          Http_Handler_Fn next, void *next_data);

#endif