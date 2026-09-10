#ifndef CWEB_REQUEST_ID_H
#define CWEB_REQUEST_ID_H

#include "middleware.h"

// options for the request-id middleware; pass a pointer as user_data, or NULL
// for the defaults (generate an id, never trust an inbound one).
typedef struct {
    // when non-zero, an inbound X-Request-Id is honored and echoed so every
    // hop behind a trusted proxy shares one trace id. default (0) ignores it
    // and mints a fresh id, because the header arrives from the public
    // internet and letting clients forge it would poison logs and audit
    // trails; honest only behind your own proxy.
    int honor_incoming;
} Http_RequestId_Opts;

// request-id middleware: gives every request a unique identifier, visible
// three ways:
//
//   * req->request_id -- a String_View on heap storage owned by the request
//     object and released by http_request_free, so handlers (and any
//     middleware downstream of this one) can read their own trace id;
//   * the response header X-Request-Id -- written exactly once, after the
//     handler runs, so even a short-circuiting or erroring chain carries the
//     id that would be logged;
//   * (optionally) the access-log line, when the access logger's opts ask for
//     it (Http_AccessLog_Opts.include_request_id).
//
// minted ids are 16 lowercase hex digits: a whole-process atomic counter
// riding on a per-process seed (clock + pid + address), so no two requests in
// the process ever share one and each restart moves to a fresh range.
//
// an inbound id is accepted only when honor_incoming is set *and* it survives
// the filter: after trimming it must be non-empty, at most 128 bytes, and hold
// no whitespace or control characters. anything else is dropped and replaced
// with a minted id, which also keeps header-injection-style payloads out.
void http_request_id_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data);

#endif