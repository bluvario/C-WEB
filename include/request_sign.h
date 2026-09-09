#ifndef CWEB_REQUEST_SIGN_H
#define CWEB_REQUEST_SIGN_H

#include <stddef.h>

#include "middleware.h"
#include "request.h"
#include "response.h"

// shared-secret request authentication, so a reverse proxy that already
// authenticated a user can prove a request really came from it. the proxy
// holds SECRET and stamps every request with
//     X-CWEB-Date:       <unix seconds>
//     X-CWEB-Signature:  hex(HMAC-SHA256(SECRET, "<METHOD>\n<PATH>\n<unix seconds>\n<body>"))
// the app recomputes the digest over the same facts and answers 403 unless it
// matches and the timestamp is fresh (replay resistance).
//
// canonicalization:
//     METHOD:  the request method token, uppercase, as it arrived ("GET", ...)
//     PATH:    req->path as parsed, query string excluded
//     unix seconds: the X-CWEB-Date value, seconds since the epoch
//     body:    the raw request body bytes (empty for requests without one)

#define CWEB_SIGN_DATE_HEADER "X-CWEB-Date"
#define CWEB_SIGN_MAC_HEADER "X-CWEB-Signature"

// fills hex (>= 65 chars) with the signature a proxy must attach for this
// request. same canonicalization http_request_signature_verify recomputes.
// method and path do not need to be NUL-terminated: their lengths say how
// many bytes each covers.
void http_request_signature_compute(const char *method, const char *path,
                                    size_t path_len, const char *body,
                                    size_t body_len, long long unix_seconds,
                                    const char *secret, char *hex,
                                    size_t hex_size);

// verifies a signed request: the X-CWEB-Signature header must be exactly 64
// lowercase hex characters, X-CWEB-Date within [now - max_age, now +
// max_age] seconds (0 selects the 300-second default), both recomputed and
// compared in constant time. returns 0 when the request is genuine and fresh,
// -1 otherwise (missing/malformed headers, mismatch, stale timestamp).
int http_request_signature_verify(Http_Request *req, const char *secret,
                                  unsigned long max_age_seconds);

// shared secret for the enforcement middleware; pass as user_data to
// http_signature_middleware. NULL options use the defaults (secret NULL,
// which rejects everything until a secret is configured).
typedef struct {
    const char *secret;
    unsigned long max_age_seconds; // 0 = default 300s
} Http_Signature_Options;

// middleware that refuses any request without a fresh, valid signature before
// a page runs: sets 403 and stops the chain. no-op on the signing side; point
// the reverse proxy at the compute helper (or its own HMAC-SHA256) instead.
void http_signature_middleware(Http_Request *req, Http_Response *res,
                               void *user_data,
                               Http_Handler_Fn next, void *next_data);

#endif